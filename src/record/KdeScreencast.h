#pragma once
#include "core/Error.h"
#include <QJsonObject>
#include <QObject>
#include <QRect>
#include <QString>
#include <functional>
#include <memory>

namespace putgd::record {
// KWin's own screencast protocol (zkde_screencast_unstable_v1): a PipeWire
// stream for an output, a logical region or one window (by KWin's window UUID),
// with no compositor dialog. The client is an independent Wayland connection,
// like the KDE output tracker.
class KdeScreencast final : public QObject {
    Q_OBJECT
public:
    struct Stream { quint32 node = 0; QRect logical; QString output; };
    using Done = std::function<void(Error, Stream)>;
    explicit KdeScreencast(QObject* parent = nullptr);
    ~KdeScreencast() override;
    // Connects and lists outputs; fills {version, outputs:[{name, logical}]}. False without the global.
    bool available(QJsonObject& facts, QString* reason = nullptr);
    // Empty name: the first output. The logical rectangle must lie on the outputs.
    // Recordings embed the pointer in the frames; pointerHidden leaves it out.
    void streamOutput(const QString& name, Done, bool pointerHidden = false);
    // A recording's region stream is one frame pixel per logical pixel. With
    // outputScale the frames are the output's own pixels (the scroll capture,
    // #1376): from protocol version 5 a scale of 0.0 lets KWin take the highest
    // scale in the region; before that the scale of the output under the
    // region's centre is passed.
    void streamRegion(const QRect& logical, Done, bool pointerHidden = false, bool outputScale = false);
    // The scale an outputScale region stream of this rectangle asks for
    // (0 from version 5, where KWin chooses).
    double regionScale(const QRect& logical);
    // The window's frame size follows it: KWin renegotiates the stream on a resize.
    // `logical` is the window's frame rectangle when the stream was requested.
    void streamWindow(const QString& uuid, const QRect& logical, Done, bool pointerHidden = false);
    void close();
signals:
    // KWin ended a stream that was already running (a streamed window closed).
    void lost();
private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
}
