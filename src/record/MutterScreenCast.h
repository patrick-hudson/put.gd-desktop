#pragma once
#include "core/Error.h"
#include <QJsonObject>
#include <QObject>
#include <QRect>
#include <QString>
#include <atomic>
#include <functional>
#include <mutex>

namespace putgd::record {
// Mutter's screen cast service (org.gnome.Mutter.ScreenCast), the API GNOME's
// own portal backend and recorder use: a PipeWire stream for a monitor
// connector, a logical area or one window (Mutter's window id), with no dialog. Mutter serves it to session
// clients on the desktops tested; where it refuses, the portal remains.
class MutterScreenCast final : public QObject {
    Q_OBJECT
public:
    struct Stream { quint32 node = 0; QRect logical; };
    using Done = std::function<void(Error, Stream)>;
    explicit MutterScreenCast(QObject* parent = nullptr);
    ~MutterScreenCast() override;
    static bool available(QJsonObject& facts, QString* reason = nullptr);
    // Empty connector: the primary monitor. Recordings draw the cursor into the
    // frames (Mutter's cursor-mode 1); cursorHidden asks for mode 0.
    void recordMonitor(const QString& connector, const QRect& logical, Done, bool cursorHidden = false);
    void recordArea(const QRect& logical, Done, bool cursorHidden = false);
    // A window stream is a fixed buffer with the window drawn into it; each
    // frame's PipeWire crop region says where (the pipeline follows it).
    void recordWindow(quint64 windowId, const QRect& logical, Done, bool cursorHidden = false);
    // A scroll capture (#1376): the area with the cursor hidden, in a screen
    // cast linked to a RemoteDesktop session (org.gnome.Mutter.RemoteDesktop)
    // that turns the wheel and places the pointer. The order is Mutter's:
    // RemoteDesktop CreateSession, its SessionId, ScreenCast CreateSession with
    // that id, RecordArea, then Start on the RemoteDesktop session only, which
    // starts the linked cast. The Shell shows its screen-sharing indicator
    // while it runs; a click on it closes the session (closedByCompositor).
    void recordAreaLinked(const QRect& logical, Done);
    // Whether Mutter's RemoteDesktop service has an owner on the session bus.
    static bool remoteDesktopAvailable(QString* reason = nullptr);
    bool linked() const;
    // Pointer and wheel for the linked session, from any thread. Mutter takes
    // them only from the D-Bus connection that created the session: this one.
    // Motion is in stream pixels of the area; a positive step scrolls down.
    bool pointerMotion(double streamX, double streamY);
    bool pointerMotionBy(double dx, double dy);
    bool pointerAxis(int steps);
    // Mutter (or the user, through the Shell's indicator) ended the session.
    bool closedByCompositor() const { return closed_.load(); }
    void close();
signals:
    // Mutter closed a session that was already streaming (a recorded window closed).
    void lost();
private slots:
    void sessionClosed();
private:
    void create(const char* method, const QList<QVariant>& arguments, const QRect& logical, const Error& refused, const QString& label, bool linked = false);
    void start(const QString& stream, const QRect& logical);
    void fail(const Error&);
    // Guards the three paths: the capture loop's thread reads them.
    mutable std::mutex paths_;
    QString session_, stream_, remote_;
    std::atomic<bool> closed_{false};
    Done done_;
};
}
