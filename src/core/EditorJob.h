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
 // starting → editing → finalizing → closed
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

struct EditorResult {QByteArray png;int width=0,height=0;Error error;};

// Synchronous supervision of `putgd-ui --edit-image`. Run on a background
// worker. Startup and finalization have deadlines; editing itself does not.
class EditorJob final {
public:
 static constexpr qint64 admissionPixels=24000000;
 static constexpr int maximumQueuedCommands=16;
 EditorJob(QString stagingRoot,QString helperPath={},int startupTimeoutMs=20000,int finalizeTimeoutMs=120000,int commandTimeoutMs=8000);
 // Writes `image` to private staging, then releases it before the helper
 // starts. Returns only bytes that pass identity and sanitized-PNG checks.
 EditorResult run(QImage& image,const QString& destination,qint64 outputBytes,const std::shared_ptr<EditorSession>&) const;
 // Removes abandoned staging left by an earlier resident. Call before editing.
 static void discardStaging(const QString& stagingRoot);
private:
 QString root_,helperPath_;
 int startupTimeoutMs_,finalizeTimeoutMs_,commandTimeoutMs_;
};
}
