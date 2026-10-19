#include "EncoderCheck.h"
#include "BundledRuntime.h"
#include <QElapsedTimer>
#include <QFile>
#include <dlfcn.h>
#include <gst/gst.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

namespace putgd::record {
namespace {
constexpr int testWidth = 320;
constexpr int testHeight = 240;
QString avError(int rc) { char text[AV_ERROR_MAX_STRING_SIZE]{}; av_strerror(rc, text, sizeof text); return QString::fromUtf8(text); }
// Mirrors the live pipeline's input formats and the one rate control Intel's
// iHD driver accepts for the legacy element; the other defaults are fine here.
QString testPipeline(const QString& encoder) {
    const QString format = encoder.startsWith("va") ? QStringLiteral("NV12") : QStringLiteral("I420");
    const QString options = encoder == "vaapih264enc" ? QStringLiteral(" rate-control=cqp init-qp=24") : QString();
    return QStringLiteral("videotestsrc num-buffers=2 ! video/x-raw,format=%1,width=%2,height=%3,framerate=30/1 ! %4%5 ! h264parse ! fakesink")
        .arg(format).arg(testWidth).arg(testHeight).arg(encoder, options);
}
QString runTest(const QString& description) {
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(description.toUtf8().constData(), &error);
    if (!pipeline || error) {
        const QString failure = error ? QString::fromUtf8(error->message) : QStringLiteral("parse failed");
        if (error) g_error_free(error);
        if (pipeline) gst_object_unref(pipeline);
        return failure;
    }
    QString failure;
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) failure = QStringLiteral("refused to start");
    GstBus* bus = gst_element_get_bus(pipeline);
    QElapsedTimer clock;
    clock.start();
    bool finished = false;
    while (failure.isEmpty() && !finished && clock.elapsed() < 4000) {
        GstMessage* message = gst_bus_timed_pop_filtered(bus, 100 * GST_MSECOND, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        if (!message) continue;
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError* e = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(message, &e, &debug);
            failure = e ? QString::fromUtf8(e->message) : QStringLiteral("pipeline error");
            if (e) g_error_free(e);
            g_free(debug);
        } else finished = true;
        gst_message_unref(message);
    }
    if (failure.isEmpty() && !finished) failure = QStringLiteral("no output within 4 s");
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return failure;
}
// The same device search as the export: the default render node, then each
// node in turn (the display GPU may have no VA driver).
bool vaapiFrames(AVBufferRef*& device, AVBufferRef*& frames) {
    QStringList nodes{QString()};
    for (int i = 128; i < 136; ++i) { const auto node = QStringLiteral("/dev/dri/renderD%1").arg(i); if (QFile::exists(node)) nodes.append(node); }
    for (const auto& node : nodes) {
        if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, node.isEmpty() ? nullptr : node.toUtf8().constData(), nullptr, 0) < 0) { device = nullptr; continue; }
        if ((frames = av_hwframe_ctx_alloc(device))) {
            auto* pool = reinterpret_cast<AVHWFramesContext*>(frames->data);
            pool->format = AV_PIX_FMT_VAAPI;
            pool->sw_format = AV_PIX_FMT_NV12;
            pool->width = testWidth;
            pool->height = testHeight;
            pool->initial_pool_size = 4;
            if (av_hwframe_ctx_init(frames) == 0) return true;
            av_buffer_unref(&frames);
        }
        av_buffer_unref(&device);
    }
    return false;
}
QString openEncoder(const QString& name) {
    const AVCodec* codec = avcodec_find_encoder_by_name(name.toUtf8().constData());
    if (!codec) return QStringLiteral("not built into this FFmpeg");
    // Opening NVENC initialises CUDA, which takes seconds and wakes a sleeping
    // discrete GPU; the driver's encode library being loadable is the probe's
    // test, and the export still falls through when the open fails.
    if (name.endsWith("_nvenc")) {
        for (const char* library : {"libcuda.so.1", "libnvidia-encode.so.1"}) {
            void* handle = dlopen(library, RTLD_LAZY | RTLD_LOCAL);
            if (!handle) return QStringLiteral("%1 is not installed").arg(QString::fromLatin1(library));
            dlclose(handle);
        }
        return {};
    }
    AVBufferRef* device = nullptr;
    AVBufferRef* frames = nullptr;
    const bool vaapi = name.endsWith("_vaapi");
    if (vaapi && !vaapiFrames(device, frames)) return QStringLiteral("no VA-API device accepts it");
    AVCodecContext* c = avcodec_alloc_context3(codec);
    c->width = testWidth;
    c->height = testHeight;
    c->time_base = {1, 30};
    c->framerate = {30, 1};
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    if (vaapi) {
        c->pix_fmt = AV_PIX_FMT_VAAPI;
        c->hw_device_ctx = av_buffer_ref(device);
        c->hw_frames_ctx = av_buffer_ref(frames);
        c->bit_rate = 0;
        av_opt_set(c->priv_data, "rc_mode", "CQP", 0);
        c->global_quality = 24;
    } else if (name == "libopenh264") c->bit_rate = 1000000;
    const int rc = avcodec_open2(c, codec, nullptr);
    avcodec_free_context(&c);
    if (frames) av_buffer_unref(&frames);
    if (device) av_buffer_unref(&device);
    return rc < 0 ? avError(rc) : QString();
}
QString osRelease(const QString& key) {
    for (const char* path : {"/etc/os-release", "/usr/lib/os-release"}) {
        QFile file(QString::fromLatin1(path));
        if (!file.open(QIODevice::ReadOnly)) continue;
        while (!file.atEnd()) {
            const auto line = QString::fromUtf8(file.readLine()).trimmed();
            if (!line.startsWith(key + '=')) continue;
            auto value = line.mid(key.size() + 1);
            if (value.size() >= 2 && (value.startsWith('"') || value.startsWith('\''))) value = value.mid(1, value.size() - 2);
            return value;
        }
        return {};
    }
    return {};
}
}

QStringList usableLiveEncoders(const QStringList& present, QJsonObject& failures) {
    QStringList usable;
    for (const auto& name : present) {
        const auto failure = runTest(testPipeline(name));
        if (failure.isEmpty()) usable.append(name);
        else failures[name] = failure;
    }
    return usable;
}
QStringList usableExportEncoders(const QStringList& names, QJsonObject& failures) {
    QStringList usable;
    for (const auto& name : names) {
        const auto failure = openEncoder(name);
        if (failure.isEmpty()) usable.append(name);
        else failures[name] = failure;
    }
    return usable;
}
bool h264DecoderUsable(QString& decoder, QString& failure) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) { failure = QStringLiteral("this FFmpeg has no H.264 decoder"); return false; }
    decoder = QString::fromUtf8(codec->name);
    AVCodecContext* c = avcodec_alloc_context3(codec);
    const int rc = avcodec_open2(c, codec, nullptr);
    avcodec_free_context(&c);
    if (rc < 0) { failure = avError(rc); return false; }
    return true;
}
QString h264Hint(const QString& reason) {
    if (runsFromAppImage())
        return QStringLiteral("The AppImage encodes H.264 with your graphics driver (VA-API or NVIDIA NVENC) and none accepted it here; "
                              "install your GPU's VA-API driver with H.264 encoding, or use the put.gd DEB or RPM package.");
    const auto id = osRelease("ID");
    const auto like = osRelease("ID_LIKE").split(' ', Qt::SkipEmptyParts);
    // Fedora's openh264enc and FFmpeg link libopenh264, which defaults to the
    // noopenh264 placeholder; Cisco's library comes from its own repository.
    if (id == "fedora" || like.contains("fedora"))
        return QStringLiteral("Install Cisco's OpenH264 in place of Fedora's placeholder: sudo dnf install openh264 gstreamer1-plugin-openh264 "
                              "(if the fedora-cisco-openh264 repository is off, first run sudo dnf config-manager setopt fedora-cisco-openh264.enabled=1).");
    if (id == "debian" || id == "ubuntu" || like.contains("debian") || like.contains("ubuntu"))
        return reason == "h264_decoder_unusable"
            ? QStringLiteral("Install FFmpeg's codec library: sudo apt install libavcodec-extra.")
            : QStringLiteral("Install an H.264 encoder: sudo apt install gstreamer1.0-plugins-ugly (x264), or gstreamer1.0-plugins-bad with your GPU's VA-API driver.");
    return reason == "h264_decoder_unusable"
        ? QStringLiteral("Install an FFmpeg build with an H.264 decoder.")
        : QStringLiteral("Install a GStreamer H.264 encoder: x264enc, openh264enc or your GPU's VA-API encoder.");
}
}
