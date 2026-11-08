#pragma once
#include "core/Error.h"
#include <QDBusMessage>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <functional>
#include <memory>

class QTemporaryFile;
class QTimer;
namespace putgd {
// The windows a recording can follow on Plasma Wayland, by KWin's window UUID
// (zkde_screencast stream_window). KWin has no D-Bus call that lists windows,
// so a one-shot KWin script reads workspace.windowList() and calls back this
// process's own unique bus name; only a call from the bus owner of
// org.kde.KWin is accepted. Takes about 0.1 s.
class KWinWindows final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.putgd.KWinWindows")
public:
    using Reply = std::function<void(Error, QJsonObject)>;
    // Decides which listed windows are put.gd's own (from the raw entry: pid,
    // resourceClass, desktopFile).
    using Own = std::function<bool(const QJsonObject&)>;
    explicit KWinWindows(QObject* parent = nullptr);
    ~KWinWindows() override;
    // Replies {windows: [{identity, title, appId, rect, active, minimized}], activeExcluded}.
    void list(Own, Reply);
    static constexpr int timeoutMs = 3000;
public slots:
    // The script's callback: `token` names the request, `json` is its window array.
    void Windows(const QString& token, const QString& json, const QDBusMessage& message);
private:
    struct Request;
    void finish(const QString& token, Error, const QJsonArray& raw = {});
    QHash<QString, std::shared_ptr<Request>> requests_;
    bool registered_ = false;
};
// The script's raw entries filtered for a recording: put.gd's own windows are
// left out (activeExcluded when the active one was), identities must be KWin
// UUIDs, text is bounded.
QJsonObject kwinRecordingWindows(const QJsonArray& raw, const KWinWindows::Own& own);
// The KWin script that lists windows and calls `service` back with `token`.
QString kwinWindowListScript(const QString& service, const QString& token);
}
