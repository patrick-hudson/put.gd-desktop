#include "MediaProbe.h"
#include "core/RecordingModel.h"
#include "PortalScreenCast.h"
#include <QJsonArray>
#include <QProcessEnvironment>
#include <gst/gst.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

namespace putgd::record {
namespace {
QString sessionType() {
    const auto env = QProcessEnvironment::systemEnvironment();
    const auto type = env.value("XDG_SESSION_TYPE");
    if (type == "wayland" || (type.isEmpty() && !env.value("WAYLAND_DISPLAY").isEmpty())) return "wayland";
    if (type == "x11" || (type.isEmpty() && !env.value("DISPLAY").isEmpty())) return "x11";
    return type.isEmpty() ? QStringLiteral("none") : type;
}
// Plugin version of a registered element factory, or null when absent.
QJsonValue element(const char* name) {
    GstElementFactory* factory = gst_element_factory_find(name);
    if (!factory) return QJsonValue(QJsonValue::Null);
    QString version;
    if (GstPlugin* plugin = gst_plugin_feature_get_plugin(GST_PLUGIN_FEATURE(factory))) {
        version = QString::fromUtf8(gst_plugin_get_version(plugin));
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
        for (const char* name : {"ximagesrc", "pipewiresrc", "pulsesrc", "vah264lpenc", "vah264enc", "vaapih264enc", "x264enc", "openh264enc",
                                 "avenc_aac", "fdkaacenc", "voaacenc", "matroskamux", "mp4mux", "webmmux", "videoconvert", "videoscale", "videorate",
                                 "videocrop", "compositor", "appsink", "webpenc", "vp9enc", "x265enc"})
            elements[name] = element(name);
        gst["elements"] = elements;
        gst["webpAnimated"] = !elements["webpenc"].isNull() && elementProperty("webpenc", "animated");
        gst["liveSources"] = present(elements, {"ximagesrc", "pipewiresrc"});
        gst["liveH264Encoders"] = present(elements, {"vah264lpenc", "vah264enc", "vaapih264enc", "x264enc", "openh264enc"});
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
    for (const char* name : {"trim", "atrim", "setpts", "asetpts", "boxblur", "drawbox", "crop", "scale", "fps", "palettegen", "paletteuse", "format"})
        filters[name] = avfilter_get_by_name(name) != nullptr;
    ff["filters"] = filters;
    result["ffmpeg"] = ff;

    auto any = [&](std::initializer_list<const char*> names) { for (const char* n : names) if (encoders[n].toBool()) return true; return false; };
    auto filtersOk = [&](std::initializer_list<const char*> names) { for (const char* n : names) if (!filters[n].toBool()) return false; return true; };
    auto list = [&](std::initializer_list<const char*> names) { QJsonArray a; for (const char* n : names) if (encoders[n].toBool()) a.append(n); return a; };
    result["exportEncoders"] = QJsonObject{{"h264", list({"h264_nvenc", "libx264", "h264_vaapi", "libopenh264"})}, {"h265", list({"hevc_nvenc", "libx265", "hevc_vaapi"})}, {"vp9", list({"libvpx-vp9"})}};

    const bool baseFilters = filtersOk({"trim", "setpts", "boxblur", "drawbox", "crop", "scale", "fps", "format"});
    QJsonArray formats;
    QJsonObject codecs;
    if (baseFilters && muxers["mp4"].toBool() && any({"h264_nvenc", "libx264", "h264_vaapi", "libopenh264"})) {
        formats.append("mp4");
        QJsonArray mp4{"h264"};
        if (any({"libx265", "hevc_vaapi"})) mp4.append("h265");
        codecs["mp4"] = mp4;
    }
    if (baseFilters && muxers["webm"].toBool() && encoders["libvpx-vp9"].toBool()) { formats.append("webm"); codecs["webm"] = QJsonArray{"vp9"}; }
    if (baseFilters && muxers["gif"].toBool() && encoders["gif"].toBool() && filtersOk({"palettegen", "paletteuse"})) formats.append("gif");
    if (baseFilters && muxers["webp"].toBool() && encoders["libwebp_anim"].toBool()) formats.append("webp");
    result["formats"] = formats;
    result["codecs"] = codecs;

    const auto session = result["session"].toString();
    const bool liveSource = gst["available"].toBool() && ((session == "x11" && gst["liveSources"].toArray().contains("ximagesrc")) || (session == "wayland" && gst["liveSources"].toArray().contains("pipewiresrc")));
    const bool liveEncoder = gst["available"].toBool() && !gst["liveH264Encoders"].toArray().isEmpty();
    result["liveCapture"] = QJsonObject{{"source", liveSource}, {"encoder", liveEncoder}, {"audio", gst["available"].toBool() && !gst["audioSources"].toArray().isEmpty() && !gst["liveAacEncoders"].toArray().isEmpty()}};
    // X11 reads the root with ximagesrc; Wayland shares a monitor or window
    // through the ScreenCast portal (the compositor's picker) into pipewiresrc.
    QJsonObject portal;
    QString portalReason;
    const bool portalOk = session == "wayland" && PortalScreenCast::available(portal, &portalReason);
    if (session == "wayland") { portal["available"] = portalOk; if (!portalOk) portal["reason"] = portalReason; result["portal"] = portal; }
    result["acquisition"] = session == "x11" ? "x11" : session == "wayland" ? "portal" : "none";
    const int sourceTypes = portal["sourceTypes"].toInt();
    QJsonArray targets;
    if (session == "x11" && liveSource) targets = QJsonArray{"monitor", "desktop", "region", "window"};
    else if (portalOk && liveSource) { targets = QJsonArray{"monitor", "desktop", "region"}; if (sourceTypes & int(PortalScreenCast::Window)) targets.append("window"); }
    result["targets"] = targets;
    result["audio"] = QJsonObject{{"system", result["liveCapture"].toObject()["audio"].toBool()}, {"microphone", false}};
    result["overlayExclusion"] = QJsonObject{};
    const bool implemented = session == "x11" || portalOk;
    result["available"] = gst["available"].toBool() && liveSource && liveEncoder && !formats.isEmpty() && implemented;
    if (!result["available"].toBool())
        result["reason"] = !gst["available"].toBool() ? QStringLiteral("gstreamer_unavailable")
            : !liveSource ? (session == "none" ? QStringLiteral("no_display_session") : QStringLiteral("live_source_missing"))
            : !liveEncoder ? QStringLiteral("live_encoder_missing")
            : formats.isEmpty() ? QStringLiteral("export_encoder_missing")
            : session == "wayland" ? (portalReason.isEmpty() ? QStringLiteral("portal_unavailable") : portalReason)
            : QStringLiteral("session_unsupported");
    return result;
}
}
