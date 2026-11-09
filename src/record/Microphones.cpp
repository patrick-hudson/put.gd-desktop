#include "Microphones.h"
#include "DebugLog.h"
#include "core/RecordingModel.h"
#include <QJsonArray>
#include <cmath>
#include <cstring>

namespace putgd::record {
namespace {
bool present(const char* element) {
    GstElementFactory* factory = gst_element_factory_find(element);
    if (!factory) return false;
    gst_object_unref(factory);
    return true;
}
QString quoted(const QString& value) { QString s = value; s.replace('\\', "\\\\").replace('"', "\\\""); return '"' + s + '"'; }
}

bool microphoneElements(QString* missing) {
    for (const char* name : {"pulsesrc", "audiomixer", "audioconvert", "audioresample", "identity"})
        if (!present(name)) { if (missing) *missing = QString::fromLatin1(name); return false; }
    return true;
}

MicrophoneList listMicrophones() {
    MicrophoneList list;
    GstDeviceProvider* provider = gst_device_provider_factory_get_by_name("pulsedeviceprovider");
    if (!provider) return list;
    // A started provider knows the server's default source; its one-shot probe does not.
    if (!gst_device_provider_start(provider)) { gst_object_unref(provider); return list; }
    list.server = true;
    GList* devices = gst_device_provider_get_devices(provider);
    for (GList* item = devices; item; item = item->next) {
        GstDevice* device = GST_DEVICE(item->data);
        gchar* klass = gst_device_get_device_class(device);
        const bool source = klass && std::strstr(klass, "Source");
        g_free(klass);
        if (!source) continue;
        gchar* name = nullptr;
        g_object_get(device, "internal-name", &name, nullptr);
        gchar* label = gst_device_get_display_name(device);
        GstStructure* properties = gst_device_get_properties(device);
        gboolean isDefault = FALSE;
        const gchar* deviceClass = properties ? gst_structure_get_string(properties, "device.class") : nullptr;
        if (properties) gst_structure_get_boolean(properties, "is-default", &isDefault);
        const QString id = QString::fromUtf8(name ? name : "");
        const bool monitor = (deviceClass && std::strcmp(deviceClass, "monitor") == 0) || recording::monitorSourceName(id);
        if (isDefault) { list.defaultSource = id; list.defaultIsMonitor = monitor; }
        if (!id.isEmpty() && !monitor) list.devices.append({id, QString::fromUtf8(label ? label : name), bool(isDefault)});
        if (properties) gst_structure_free(properties);
        g_free(label);
        g_free(name);
    }
    g_list_free_full(devices, gst_object_unref);
    gst_device_provider_stop(provider);
    gst_object_unref(provider);
    return list;
}

MicrophoneChoice chooseMicrophone(const MicrophoneList& list, const QString& wanted) {
    MicrophoneChoice choice;
    if (!list.server) { choice.reason = QStringLiteral("sound_server_unavailable"); choice.message = QStringLiteral("The sound server did not answer, so no microphone can be opened."); return choice; }
    if (!wanted.isEmpty()) {
        if (recording::monitorSourceName(wanted)) { choice.reason = QStringLiteral("microphone_is_monitor"); choice.message = QStringLiteral("The chosen device is an output's monitor, not a microphone."); return choice; }
        for (const auto& device : list.devices) if (device.id == wanted) { choice.device = device; return choice; }
        choice.reason = QStringLiteral("microphone_missing");
        choice.message = QStringLiteral("The microphone chosen in Settings is not connected.");
        return choice;
    }
    for (const auto& device : list.devices) if (device.isDefault) { choice.device = device; return choice; }
    if (list.devices.isEmpty()) { choice.reason = QStringLiteral("no_microphone"); choice.message = QStringLiteral("No microphone is connected."); return choice; }
    // The default source is an output's monitor (nothing is plugged in), or the
    // server named none: put.gd never guesses another input in its place.
    choice.reason = list.defaultIsMonitor ? QStringLiteral("default_is_monitor") : QStringLiteral("no_default_microphone");
    choice.message = list.defaultIsMonitor ? QStringLiteral("The system's default input is the monitor of an output, not a microphone. Plug one in, or choose a device in Settings.")
                                           : QStringLiteral("The system has no default input. Choose a microphone in Settings.");
    return choice;
}

QJsonObject microphonesJson(const QString& wanted) {
    QJsonObject result;
    QString missing;
    QJsonArray devices;
    if (!microphoneElements(&missing)) {
        result["available"] = false;
        result["reason"] = "microphone_elements_missing";
        result["message"] = QStringLiteral("This installation's GStreamer has no %1 element, which microphone recording needs.").arg(missing);
        result["devices"] = devices;
        result["defaultIsMonitor"] = false;
        return result;
    }
    const auto list = listMicrophones();
    for (const auto& device : list.devices) devices.append(QJsonObject{{"id", device.id}, {"label", device.label}, {"default", device.isDefault}});
    const auto choice = chooseMicrophone(list, wanted);
    result["available"] = choice.available();
    if (choice.available()) result["device"] = QJsonObject{{"id", choice.device.id}, {"label", choice.device.label}};
    else { result["reason"] = choice.reason; result["message"] = choice.message; }
    result["devices"] = devices;
    result["defaultIsMonitor"] = list.defaultIsMonitor;
    return result;
}

GstPadProbeReturn LevelMeter::probe(GstPad*, GstPadProbeInfo* info, gpointer user) {
    auto* meter = static_cast<LevelMeter*>(user);
    GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    GstMapInfo map;
    if (!buffer || !gst_buffer_map(buffer, &map, GST_MAP_READ)) return GST_PAD_PROBE_OK;
    const auto* samples = reinterpret_cast<const float*>(map.data);
    const gsize count = map.size / sizeof(float);
    float peak = 0.0f;
    for (gsize i = 0; i < count; ++i) { const float value = std::fabs(samples[i]); if (value > peak) peak = value; }
    gst_buffer_unmap(buffer, &map);
    if (!std::isfinite(peak)) peak = 0.0f;
    peak = std::min(peak, 1.0f);
    float previous = meter->peak.load();
    while (peak > previous && !meter->peak.compare_exchange_weak(previous, peak)) {}
    return GST_PAD_PROBE_OK;
}

MicrophoneMonitor::MicrophoneMonitor(QObject* parent) : QObject(parent) {}
MicrophoneMonitor::~MicrophoneMonitor() { close(); }
bool MicrophoneMonitor::open(const QString& device, QString* failure) {
    if (pipeline_ && device_ == device) return true;
    close();
    if (device.isEmpty() || recording::monitorSourceName(device)) { if (failure) *failure = QStringLiteral("no microphone"); return false; }
    const QString description = QStringLiteral("pulsesrc name=msrc device=%1 client-name=\"put.gd recording\" ! audioconvert ! audio/x-raw,format=F32LE,layout=interleaved,channels=1 ! fakesink name=sink sync=false async=false").arg(quoted(device));
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(description.toUtf8().constData(), &error);
    if (!pipeline || error) {
        if (failure) *failure = error ? QString::fromUtf8(error->message) : QStringLiteral("parse failed");
        if (error) g_error_free(error);
        if (pipeline) gst_object_unref(pipeline);
        return false;
    }
    if (GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink")) {
        if (GstPad* pad = gst_element_get_static_pad(sink, "sink")) { gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, &LevelMeter::probe, &level_, nullptr); gst_object_unref(pad); }
        gst_object_unref(sink);
    }
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        if (failure) *failure = QStringLiteral("the microphone could not be opened");
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return false;
    }
    pipeline_ = pipeline;
    device_ = device;
    level_.take();
    debugLog("microphone: meter opened on " + device);
    return true;
}
void MicrophoneMonitor::close() {
    if (!pipeline_) return;
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    level_.take();
    debugLog("microphone: meter closed on " + device_);
    device_.clear();
}
}
