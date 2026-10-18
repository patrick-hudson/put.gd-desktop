#pragma once
#include "core/Error.h"
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPoint>
#include <QSize>
#include <QString>
#include <QVariant>
#include <functional>

namespace putgd::record {
// One xdg-desktop-portal ScreenCast session: CreateSession → SelectSources →
// Start (the compositor shows its own picker; the user chooses a monitor or
// window) → OpenPipeWireRemote. The result is the PipeWire node to capture
// and the stream's logical position and size. Close ends the session; the
// compositor stops the stream and the picker's grant with it.
class PortalScreenCast final : public QObject {
    Q_OBJECT
public:
    enum SourceType : quint32 { Monitor = 1, Window = 2, Virtual = 4 };
    enum CursorMode : quint32 { Hidden = 1, Embedded = 2, Metadata = 4 };
    struct Stream { quint32 node = 0; QPoint position; QSize size; quint32 sourceType = 0; };
    struct Result { QList<Stream> streams; int fd = -1; QString restoreToken; };
    using Done = std::function<void(Error, Result)>;
    explicit PortalScreenCast(QObject* parent = nullptr);
    ~PortalScreenCast() override;
    // Portal presence and version on the session bus; fills the capability facts.
    static bool available(QJsonObject& facts, QString* reason = nullptr);
    void start(quint32 sourceTypes, const QString& restoreToken, Done);
    void close();
    bool active() const { return !session_.isEmpty(); }
    // Demarshals the Start response's `streams` (a(ua{sv})) value.
    static QList<Stream> parseStreams(const QVariant& value);
    // One stream from its node and property map; (ii) pairs arrive as D-Bus
    // structures from the portal or as two-element lists.
    static Stream parseStream(quint32 node, const QVariantMap& props);
private:
    void request(const QString& method, const QList<QVariant>& arguments, QVariantMap options, std::function<void(quint32, QVariantMap)> onResponse);
    void fail(const Error&);
    QString token();
    QString session_, sessionToken_;
    Done done_;
    int counter_ = 0;
};
}
