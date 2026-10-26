#pragma once
#include "Error.h"
#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <QStringList>

class QFile;

namespace putgd {
// Thread-safe handle to one live editor. The session thread queues agent
// commands; the editor worker relays them over the helper's private pipes.
class EditorSession final {
public:
 using Reply=std::function<void(Error,QJsonObject)>;
 // Replies run on the editor worker thread; callers marshal them.
 void request(const QString& command,const QJsonObject& params,Reply);
 void cancel() {cancelled_.store(true);}
 bool cancelled() const {return cancelled_.load();}
 // starting → editing → finalizing → closed; a recoverable preparation
 // failure returns finalizing → editing.
 QString phase() const;
 struct Pending {QString command;QJsonObject params;Reply reply;};
 QList<Pending> take();
 void setPhase(const QString&);
private:
 mutable std::mutex mutex_;
 QList<Pending> queue_;
 QString phase_="starting";
 std::atomic_bool cancelled_{false};
};

// recentColors is set whenever the helper reached its checkpoint.
struct EditorResult {QByteArray png;int width=0,height=0;Error error;std::optional<QStringList> recentColors;};

// Synchronous supervision of `putgd-ui --edit-image`. Run on a background
// worker. Startup and finalization have deadlines; editing itself does not.
class EditorJob final {
public:
 static constexpr qint64 admissionPixels=24000000;
 static constexpr int maximumQueuedCommands=16;
 EditorJob(QString stagingRoot,QString helperPath={},int startupTimeoutMs=20000,int finalizeTimeoutMs=120000,int commandTimeoutMs=8000);
 // Custom colors the editor offers as quick picks; it reports them back.
 EditorJob& withRecentColors(QStringList colors){recentColors_=std::move(colors);return *this;}
 // "dark" or "light": the helper's scheme, the interface's resolved one.
 EditorJob& withAppearance(QString scheme){appearance_=std::move(scheme);return *this;}
 // Writes `image` to private staging, then releases it before the helper
 // starts. Returns only bytes that pass identity and sanitized-PNG checks.
 EditorResult run(QImage& image,const QString& destination,qint64 outputBytes,const std::shared_ptr<EditorSession>&) const;
 // Stages a PNG/JPEG file's bytes without decoding them in this process.
 EditorResult runFile(const QString& path,qint64 maximumInputBytes,const QString& destination,qint64 outputBytes,const std::shared_ptr<EditorSession>&) const;
 // Removes abandoned staging left by an earlier resident. Call before editing.
 static void discardStaging(const QString& stagingRoot);
private:
 EditorResult supervise(const std::function<bool(QFile&)>& stage,const QString& destination,qint64 outputBytes,const std::shared_ptr<EditorSession>&) const;
 QString root_,helperPath_,appearance_=QStringLiteral("dark");QStringList recentColors_;
 int startupTimeoutMs_,finalizeTimeoutMs_,commandTimeoutMs_;
};
}
