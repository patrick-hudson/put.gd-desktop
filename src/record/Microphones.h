#pragma once
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <atomic>
#include <gst/gst.h>

namespace putgd::record {
// The inputs a recording can narrate from (#1364), read from the sound server
// through GStreamer's PulseAudio device provider (PulseAudio and PipeWire's
// pulse layer). The monitor of an output is never in the list and is never
// opened as a microphone: recording it would add the computer's own playback a
// second time. Listing connects to the sound server and opens no device.
// `stream-properties` for every pulsesrc that opens a microphone. PipeWire's session manager moves a
// stream whose device disappears to the default input, and where nothing is plugged in that is an
// output's monitor: the recording would go on with the computer's own sound in place of a voice.
// `node.dont-reconnect` (what PA_STREAM_DONT_MOVE becomes in PipeWire's PulseAudio layer) leaves the
// stream unlinked instead (measured with PipeWire 1.4.2 and WirePlumber 0.5.8: the pinned stream has no
// source after the device is removed, an unpinned one is on the monitor). PulseAudio itself ignores
// the property; there the pipeline's own check of the stream's device drops the audio.
inline constexpr const char* pinnedStream = "props,node.dont-reconnect=(string)true";
struct Microphone {
    QString id;     // the source name pulsesrc opens
    QString label;  // the device's description
    bool isDefault = false;
};
struct MicrophoneList {
    QList<Microphone> devices;
    QString defaultSource;         // the server's default source, which may be a monitor
    bool defaultIsMonitor = false;
    bool server = false;           // the sound server answered
};
struct MicrophoneChoice {
    Microphone device;
    QString reason, message;       // set when there is nothing to open
    bool available() const { return !device.id.isEmpty(); }
};
// pulsesrc and, for the mix with system audio, audiomixer.
bool microphoneElements(QString* missing = nullptr);
MicrophoneList listMicrophones();
// `wanted` is a source name from the list, or empty for the system default
// input. A wanted device that is gone, a default that is an output's monitor
// and a machine without an input all answer with a reason, never a monitor.
MicrophoneChoice chooseMicrophone(const MicrophoneList&, const QString& wanted);
// {available, reason?, message?, device?: {id, label}, devices: [{id, label, default}], defaultIsMonitor}
QJsonObject microphonesJson(const QString& wanted);

// Peak level of the newest audio, written on a streaming thread and read by
// the control bar's meter.
struct LevelMeter {
    std::atomic<float> peak{0.0f};
    // The largest sample since the last call, 0..1.
    float take() { return peak.exchange(0.0f); }
    // A buffer probe for interleaved F32LE audio; user data is the LevelMeter.
    static GstPadProbeReturn probe(GstPad*, GstPadProbeInfo*, gpointer);
};

// The microphone opened for the bar's meter before Record: the toggle is on,
// nothing is recorded yet, and the meter shows the input is live. It is closed
// when the toggle goes off, when the recording pipeline takes the device over
// and when the session ends.
class MicrophoneMonitor final : public QObject {
    Q_OBJECT
public:
    explicit MicrophoneMonitor(QObject* parent = nullptr);
    ~MicrophoneMonitor() override;
    // Opens `device`; false with `failure` set when the pipeline cannot start.
    bool open(const QString& device, QString* failure = nullptr);
    void close();
    bool isOpen() const { return pipeline_ != nullptr; }
    QString device() const { return device_; }
    LevelMeter& level() { return level_; }
private:
    GstElement* pipeline_ = nullptr;
    QString device_;
    LevelMeter level_;
};
}
