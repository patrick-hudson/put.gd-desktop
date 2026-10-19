#include "MediaProbe.h"
#include "EncoderCheck.h"
#include "core/RecordingModel.h"
#include "PortalScreenCast.h"
#include "WaylandAcquisition.h"
#include <QFileInfo>
#include "MutterScreenCast.h"
#ifdef PUTGD_RECORD_KDE_SCREENCAST
#include "KdeScreencast.h"
#endif
#include <QDBusConnection>
#include <QDBusMessage>
#include <QJsonArray>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <gst/gst.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

namespace putgd::record {
namespace {
#ifdef PUTGD_RECORD_KDE_SCREENCAST
// "major.minor.patch" from KWin's support information, or empty.
QString kwinVersion() {
    auto message = QDBusMessage::createMethodCall("org.kde.KWin", "/KWin", "org.kde.KWin", "supportInformation");
    const auto reply = QDBusConnection::sessionBus().call(message, QDBus::Block, 1500);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) return {};
    const auto match = QRegularExpression(QStringLiteral("KWin version: (\\d+\\.\\d+(?:\\.\\d+)?)")).match(reply.arguments().first().toString().left(65536));
    return match.hasMatch() ? match.captured(1) : QString{};
}
#endif
QString sessionType() {
    const auto env = QProcessEnvironment::systemEnvironment();
    const auto type = env.value("XDG_SESSION_TYPE");
    if (type == "wayland" || (type.isEmpty() && !env.value("WAYLAND_DISPLAY").isEmpty())) return "wayland";
    if (type == "x11" || (type.isEmpty() && !env.value("DISPLAY").isEmpty())) return "x11";
    return type.isEmpty() ? QStringLiteral("none") : type;
}
// Plugin version of a registered element factory, or null when absent; the
// plugin file's directory goes into `directories` (the AppImage audit checks
// that every one is inside the image).
QJsonValue element(const char* name, QStringList& directories) {
    GstElementFactory* factory = gst_element_factory_find(name);
    if (!factory) return QJsonValue(QJsonValue::Null);
    QString version;
    if (GstPlugin* plugin = gst_plugin_feature_get_plugin(GST_PLUGIN_FEATURE(factory))) {
        version = QString::fromUtf8(gst_plugin_get_version(plugin));
        if (const gchar* file = gst_plugin_get_filename(plugin)) {
            const auto directory = QFileInfo(QString::fromUtf8(file)).absolutePath();
            if (!directories.contains(directory)) directories.append(directory);
        }
        gst_object_unref(plugin);
    }
    gst_object_unref(factory);
    return version.isEmpty() ? QStringLiteral("present") : version;
}
bool elementProperty(const char* name, const char* property) {
    GstElement* instance = gst_element_factory_make(name, nullptr);
    if (!instance) return false;
    const bool found = g_object_class_find_property(G_OBJECT_GET_CLASS(instance), property) != nullptr;
    gst_object_unref(instance);
    return found;
}
QJsonArray present(const QJsonObject& inventory, const QStringList& ordered) {
    QJsonArray result;
    for (const auto& name : ordered) if (!inventory[name].isNull() && inventory.contains(name)) result.append(name);
    return result;
}
QString avVersion(unsigned packed) { return QStringLiteral("%1.%2.%3").arg(packed >> 16).arg((packed >> 8) & 0xff).arg(packed & 0xff); }
QStringList strings(const QJsonArray& values) { QStringList result; for (const auto& value : values) result.append(value.toString()); return result; }
}

QJsonObject probeMedia() {
    QJsonObject result{{"schemaVersion", 1}, {"session", sessionType()}};
    GError* error = nullptr;
    QJsonObject gst;
    if (!gst_init_check(nullptr, nullptr, &error)) {
        gst["available"] = false;
        gst["error"] = error ? QString::fromUtf8(error->message) : QStringLiteral("gst_init failed");
        if (error) g_error_free(error);
    } else {
        gst["available"] = true;
        gst["version"] = QString::fromUtf8(gst_version_string());
        QJsonObject elements;
        QStringList directories;
        for (const char* name : {"ximagesrc", "pipewiresrc", "pulsesrc", "vah264lpenc", "vah264enc", "vaapih264enc", "x264enc", "openh264enc",
                                 "avenc_aac", "fdkaacenc", "voaacenc", "matroskamux", "mp4mux", "webmmux", "videoconvert", "videoscale", "videorate",
                                 "videocrop", "compositor", "appsink", "webpenc", "vp9enc", "x265enc", "h264parse", "aacparse", "audioconvert", "audioresample"})
            elements[name] = element(name, directories);
        gst["elements"] = elements;
        gst["pluginDirectories"] = QJsonArray::fromStringList(directories);
        gst["webpAnimated"] = !elements["webpenc"].isNull() && elementProperty("webpenc", "animated");
        gst["liveSources"] = present(elements, {"ximagesrc", "pipewiresrc"});
        gst["liveH264Encoders"] = present(elements, {"vah264lpenc", "vah264enc", "vaapih264enc", "x264enc", "openh264enc"});
        QJsonObject failures;
        gst["usableH264Encoders"] = QJsonArray::fromStringList(usableLiveEncoders(strings(gst["liveH264Encoders"].toArray()), failures));
        gst["encoderFailures"] = failures;
        gst["liveAacEncoders"] = present(elements, {"avenc_aac", "fdkaacenc", "voaacenc"});
        gst["audioSources"] = present(elements, {"pulsesrc", "pipewiresrc"});
    }
    result["gstreamer"] = gst;

    QJsonObject ff{{"avcodec", avVersion(avcodec_version())}, {"avformat", avVersion(avformat_version())}, {"avfilter", avVersion(avfilter_version())}, {"avutil", avVersion(avutil_version())}};
    QJsonObject encoders;
    for (const char* name : {"h264_nvenc", "libx264", "h264_vaapi", "libopenh264", "hevc_nvenc", "libx265", "hevc_vaapi", "libvpx-vp9", "vp9_vaapi", "gif", "libwebp_anim", "aac", "libopus", "libvorbis"})
        encoders[name] = avcodec_find_encoder_by_name(name) != nullptr;
    ff["encoders"] = encoders;
    QJsonObject muxers;
    for (const char* name : {"mp4", "webm", "gif", "webp", "matroska"}) muxers[name] = av_guess_format(name, nullptr, nullptr) != nullptr;
    ff["muxers"] = muxers;
    QJsonObject filters;
    for (const char* name : {"trim", "atrim", "setpts", "asetpts", "avgblur", "drawbox", "crop", "scale", "fps", "palettegen", "paletteuse", "format"})
        filters[name] = avfilter_get_by_name(name) != nullptr;
    ff["filters"] = filters;

    auto filtersOk = [&](std::initializer_list<const char*> names) { for (const char* n : names) if (!filters[n].toBool()) return false; return true; };
    auto list = [&](std::initializer_list<const char*> names) { QJsonArray a; for (const char* n : names) if (encoders[n].toBool()) a.append(n); return a; };
    // Only encoders that actually open count: a registered libopenh264 may be
    // Fedora's placeholder and a registered h264_vaapi may lack a driver profile.
    QJsonObject exportFailures;
    auto usable = [&](const QJsonArray& names) { return QJsonArray::fromStringList(usableExportEncoders(strings(names), exportFailures)); };
    // In the export's preference order: the machine's GPU encoders, then the software ones.
    result["exportEncoders"] = QJsonObject{{"h264", usable(list({"h264_nvenc", "h264_vaapi", "libx264", "libopenh264"}))}, {"h265", usable(list({"hevc_nvenc", "hevc_vaapi", "libx265"}))}, {"vp9", usable(list({"libvpx-vp9"}))}};
    ff["encoderFailures"] = exportFailures;
    QString decoderName, decoderFailure;
    const bool decoderOk = h264DecoderUsable(decoderName, decoderFailure);
    ff["h264Decoder"] = QJsonObject{{"name", decoderName.isEmpty() ? QJsonValue() : QJsonValue(decoderName)}, {"usable", decoderOk}, {"failure", decoderOk ? QJsonValue() : QJsonValue(decoderFailure)}};
    result["ffmpeg"] = ff;
    const auto exportable = result["exportEncoders"].toObject();

    const bool baseFilters = filtersOk({"trim", "setpts", "avgblur", "drawbox", "crop", "scale", "fps", "format"});
    QJsonArray formats;
    QJsonObject codecs;
    if (baseFilters && muxers["mp4"].toBool() && !exportable["h264"].toArray().isEmpty()) {
        formats.append("mp4");
        QJsonArray mp4{"h264"};
        if (!exportable["h265"].toArray().isEmpty()) mp4.append("h265");
        codecs["mp4"] = mp4;
    }
    if (baseFilters && muxers["webm"].toBool() && !exportable["vp9"].toArray().isEmpty()) { formats.append("webm"); codecs["webm"] = QJsonArray{"vp9"}; }
    if (baseFilters && muxers["gif"].toBool() && encoders["gif"].toBool() && filtersOk({"palettegen", "paletteuse"})) formats.append("gif");
    if (baseFilters && muxers["webp"].toBool() && encoders["libwebp_anim"].toBool()) formats.append("webp");
    result["formats"] = formats;
    result["codecs"] = codecs;

    const auto session = result["session"].toString();
    const bool liveSource = gst["available"].toBool() && ((session == "x11" && gst["liveSources"].toArray().contains("ximagesrc")) || (session == "wayland" && gst["liveSources"].toArray().contains("pipewiresrc")));
    const bool liveEncoder = gst["available"].toBool() && !gst["usableH264Encoders"].toArray().isEmpty();
    result["liveCapture"] = QJsonObject{{"source", liveSource}, {"encoder", liveEncoder}, {"audio", gst["available"].toBool() && !gst["audioSources"].toArray().isEmpty() && !gst["liveAacEncoders"].toArray().isEmpty()}};
    // X11 reads the root with ximagesrc; Wayland shares a monitor or window
    // through the ScreenCast portal (the compositor's picker) into pipewiresrc.
    QJsonObject portal;
    QString portalReason;
    const bool portalOk = session == "wayland" && PortalScreenCast::available(portal, &portalReason);
    if (session == "wayland") { portal["available"] = portalOk; if (!portalOk) portal["reason"] = portalReason; result["portal"] = portal; }
    // Dialog-free compositor paths first: KWin's screencast protocol on Plasma,
    // Mutter's screen cast service on GNOME; the portal (with restore tokens) otherwise.
    // Both native paths name a PipeWire node on the session daemon, so its socket
    // must be reachable; the portal passes its own connection as a descriptor.
    const auto env = QProcessEnvironment::systemEnvironment();
    const auto desktop = env.value("XDG_CURRENT_DESKTOP").toLower();
    const QString pipewire = pipewireSocket(env);
    if (session == "wayland") result["pipewire"] = QJsonObject{{"socket", pipewire.isEmpty() ? QJsonValue() : QJsonValue(pipewire)}};
    QString acquisition = session == "x11" ? "x11" : session == "wayland" ? "portal" : "none";
    if (session == "wayland" && pipewire.isEmpty()) {
        // leave the portal in charge: it carries its own PipeWire connection
    } else if (session == "wayland" && desktop.contains("kde")) {
#ifdef PUTGD_RECORD_KDE_SCREENCAST
        QJsonObject kde; QString reason; KdeScreencast probe;
        const bool ok = probe.available(kde, &reason); kde["available"] = ok; if (!ok) kde["reason"] = reason;
        // KWin 6.6 leaves the requesting client's own windows (the control bar,
        // ring and dimming) out of the streams it creates for that client.
        const auto version = kwinVersion();
        const auto parts = version.split('.');
        kde["kwinVersion"] = version.isEmpty() ? QJsonValue() : QJsonValue(version);
        kde["hidesRequester"] = parts.size() >= 2 && (parts[0].toInt() > 6 || (parts[0].toInt() == 6 && parts[1].toInt() >= 6));
        result["kdeScreencast"] = kde;
        if (ok) acquisition = "kde-screencast";
#endif
    } else if (session == "wayland" && desktop.contains("gnome")) {
        QJsonObject mutter; QString reason;
        const bool ok = MutterScreenCast::available(mutter, &reason); mutter["available"] = ok; if (!ok) mutter["reason"] = reason; result["mutterScreenCast"] = mutter;
        if (ok) acquisition = "mutter-screencast";
    }
    result["acquisition"] = acquisition;
    const bool nativeOk = acquisition == "kde-screencast" || acquisition == "mutter-screencast";
    const int sourceTypes = portal["sourceTypes"].toInt();
    QJsonArray targets;
    if (session == "x11" && liveSource) targets = QJsonArray{"monitor", "desktop", "region", "window"};
    else if ((portalOk || nativeOk) && liveSource) { targets = QJsonArray{"monitor", "desktop", "region"}; if (portalOk && (sourceTypes & int(PortalScreenCast::Window))) targets.append("window"); }
    result["targets"] = targets;
    result["audio"] = QJsonObject{{"system", result["liveCapture"].toObject()["audio"].toBool()}, {"microphone", false}};
    result["overlayExclusion"] = QJsonObject{};
    const bool implemented = session == "x11" || portalOk || nativeOk;
    result["available"] = gst["available"].toBool() && liveSource && liveEncoder && decoderOk && !formats.isEmpty() && implemented;
    if (!result["available"].toBool())
        result["reason"] = !gst["available"].toBool() ? QStringLiteral("gstreamer_unavailable")
            : !liveSource ? (session == "none" ? QStringLiteral("no_display_session") : QStringLiteral("live_source_missing"))
            : !liveEncoder ? (gst["liveH264Encoders"].toArray().isEmpty() ? QStringLiteral("live_encoder_missing") : QStringLiteral("h264_encoder_unusable"))
            : !decoderOk ? QStringLiteral("h264_decoder_unusable")
            : formats.isEmpty() ? QStringLiteral("export_encoder_missing")
            : session == "wayland" ? (portalReason.isEmpty() ? QStringLiteral("portal_unavailable") : portalReason)
            : QStringLiteral("session_unsupported");
    // Recording keeps an H.264 working clip, so without a working H.264 encoder
    // or decoder nothing records; say what to install rather than only why.
    const auto reason = result["reason"].toString();
    if (reason == "live_encoder_missing" || reason == "h264_encoder_unusable" || reason == "h264_decoder_unusable") {
        result["message"] = reason == "h264_decoder_unusable" ? QStringLiteral("Screen recording needs an H.264 decoder, and FFmpeg cannot open one here.")
            : reason == "h264_encoder_unusable" ? QStringLiteral("Screen recording needs an H.264 encoder, and none of the installed ones (%1) works here.").arg(strings(gst["liveH264Encoders"].toArray()).join(", "))
            : QStringLiteral("Screen recording needs an H.264 encoder, and none is available here.");
        result["hint"] = h264Hint(reason);
    }
    return result;
}
}
