#pragma once
#include "core/Error.h"
#include <QJsonObject>
#include <QObject>
#include <QRect>
#include <QString>
#include <functional>

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
    // Empty connector: the primary monitor.
    void recordMonitor(const QString& connector, const QRect& logical, Done);
    void recordArea(const QRect& logical, Done);
    // A window stream is a fixed buffer with the window drawn into it; each
    // frame's PipeWire crop region says where (the pipeline follows it).
    void recordWindow(quint64 windowId, const QRect& logical, Done);
    void close();
signals:
    // Mutter closed a session that was already streaming (a recorded window closed).
    void lost();
private slots:
    void sessionClosed();
private:
    void create(const char* method, const QList<QVariant>& arguments, const QRect& logical, const Error& refused, const QString& label);
    void start(const QString& stream, const QRect& logical);
    void fail(const Error&);
    QString session_, stream_;
    Done done_;
};
}
