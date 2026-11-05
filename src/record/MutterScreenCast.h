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
// connector or a logical area, with no dialog. Mutter serves it to session
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
    void close();
private:
    void start(const QString& stream, const QRect& logical);
    void fail(const Error&);
    QString session_, stream_;
    Done done_;
};
}
