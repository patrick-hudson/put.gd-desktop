#pragma once
#include "TextBackend.h"
#include "core/Operation.h"
#include <QHash>
#include <QObject>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>

namespace putgd {
// Text recognition in the resident (#1428): the one interface the Copy text
// action, text.recognize and History's Copy text call. It owns a text
// operation from acceptance to its terminal state:
//   accepted -> acquiring -> recognizing -> completed | cancelled | failed
// Both acceptance checkpoints (accepted, acquiring) are durable before the
// source is read, and `recognizing` before the helper starts. Nothing
// recognized is journaled, logged or observed: the journal and the terminal
// record hold states, counts, the copy outcome and an error code.
// One text operation at a time: a second request while one runs is refused at
// once with text_busy, and nothing queues.
class TextService final:public QObject {
 Q_OBJECT
public:
 using JsonReply=std::function<void(Error,QJsonObject)>;
 // Decision 7: what the resident keeps for an agent, at most, and for how long.
 static constexpr qsizetype resultCacheBytes=4*1024*1024;
 static constexpr int resultLifetimeMs=120000;
 // The last operations text.inspect still answers for once they ended.
 static constexpr int terminalRecords=32;
 static QStringList sources(); // decision 8: region, window, monitor, desktop, clipboard, file, history
 struct Acceptance {
  bool interactive=false; // holds the one interactive-acquisition slot
  bool copy=false; // takes the explicit-copy ticket and the expected clipboard revision
 };
 struct Host {
  std::function<Settings()> settings;
  // A new text operation: identity, settings snapshot, journal ownership, the
  // pending-operation limit and, for an interactive source, the selection slot.
  // With copy it stores the explicit-copy ticket (invocation) and the clipboard
  // revision in the operation, and advances the upload-copy epoch.
  std::function<Error(Operation&,const Acceptance&)> accept;
  std::function<void(const Operation&,std::function<void(Error)>)> persist;
  // Current work, as for any accepted operation.
  std::function<void(const Operation&)> observe;
  // The terminal state. A failed operation is journaled; a completed or
  // cancelled one's row is removed. Releases what accept took.
  std::function<void(Operation)> finish;
  // One explicit plain-text copy through the resident's copy queue, refused
  // (copy_suppressed) when the user changed the clipboard after acceptance or a
  // later explicit copy took the ticket.
  std::function<void(const Operation&,const QString& text,std::function<void(Error)>)> copy;
  // Runs `work` off the session thread and then `done` on it.
  std::function<void(std::function<void()> work,std::function<void()> done)> background;
  // Asks for heap reclamation once a kept result is gone; optional.
  std::function<void()> reclaim;
  // The desktop locale's name (de_DE), for the default language; optional.
  std::function<QString()> locale;
 };
 // Reads the acquired pixels, in any format, on the worker that recognizes
 // them: a decode never runs on the session thread. An empty error and a null
 // image is source_unsupported.
 using Load=std::function<QImage(Error&)>;
 // What ended acquisition: the pixels' loader, or an error (cancelled and
 // source_cancelled end the operation as cancelled). Callable from any thread;
 // only the first call counts.
 using Deliver=std::function<void(Error,Load)>;
 struct Request {
  QString source="region";
  // Named by the caller: 1 to 3 identifiers that must all load. Empty: the
  // saved preference, else the locale's language and English.
  QStringList languages;
  // One clipboard write when anything was found. False leaves the clipboard
  // alone and ends without a notification.
  bool copy=false;
  // Keep the result for text.inspect (text.recognize, history.copyText with
  // keepResult). False keeps counts only and asks the helper for no words.
  bool keepResult=false;
  bool interactive=false;
  // Called on the session thread at acceptance, before anything is journaled:
  // where an adapter freezes its target or shows its selector. It may deliver
  // early (Escape in the selector), which ends the operation; optional.
  std::function<void(const Operation&,Deliver)> prepare;
  // Reads the source. Called on the session thread once the operation is
  // journaled as acquiring; may deliver from any thread.
  std::function<void(Deliver)> acquire;
  // Ends a running acquisition (Escape, text.cancel), which then delivers; optional.
  std::function<void()> cancelAcquisition;
 };
 explicit TextService(Host,QObject* parent=nullptr);
 ~TextService() override;
 // Tests inject a fake; production installs HelperTextBackend. Not while active.
 void setBackend(std::unique_ptr<TextBackend>);
 // Answers {operationId, state} once the operation is journaled as accepted.
 void recognize(Request,JsonReply);
 // The live state, or the terminal record: state, source, characterCount,
 // codeCount, copyStatus, truncated, banded, languagesUsed, languagesSkipped, timings,
 // error, resultExpired, and while the result is kept text, words and codes.
 // An older or unknown ID is operation_not_found.
 Error inspect(const QString& operationId,QJsonObject& result) const;
 // Answers with the terminal record once the operation has ended.
 void cancel(const QString& operationId,JsonReply);
 // The capabilities `text` block: the backend's report and availability per source.
 // sourceErrors names the sources this desktop cannot read, with the adapter's error.
 QJsonObject capabilities(const QHash<QString,Error>& sourceErrors={}) const;
 bool active() const {return !id_.isEmpty();}
 QString activeId() const {return id_;}
 void shutdown();
 void setResultLifetimeForTesting(int ms) {lifetimeMs_=ms;}
 // What the cache holds now, for the memory bound's tests.
 qsizetype cachedBytes() const {return cache_.size();}
signals:
 // An operation reached its terminal state. record is what inspect answers
 // (nothing recognized); notification is the Copy text message when the request
 // asked for a copy, else empty.
 void finished(const QString& operationId,const QJsonObject& record,const QString& notification);
private:
 void advance(const QString& id,const QString& state,std::function<void()> next);
 Deliver deliverer(const QString& id);
 void delivered(const QString& id,Error,Load);
 // Runs what was delivered while a journal save was pending; true when it ended the step.
 bool settle(const QString& id);
 void acquired(const QString& id,Error,Load);
 void recognized(const QString& id,Error,std::shared_ptr<TextResult>);
 void complete(const QString& id,std::shared_ptr<TextResult>,const QString& copyStatus);
 void terminate(Error,bool cancelled);
 void end(QJsonObject record,const QString& notification);
 void keep(const QString& id,const TextResult&);
 void dropCache();
 Host host_;
 // Shared with the worker that runs a job, which may outlive a replaced backend.
 std::shared_ptr<TextBackend> backend_;
 QString id_;
 Operation op_;
 Request request_;
 TextJob::Cancel cancel_;
 bool delivered_=false;
 // A journal save is in flight: nothing may end the operation until it lands,
 // or its row could be written after the terminal one.
 bool persisting_=false;
 struct Delivery {Error error;Load load;};
 std::optional<Delivery> waiting_;
 QList<JsonReply> cancelReplies_;
 QHash<QString,QJsonObject> records_;
 QStringList recordOrder_;
 // The agent's result as one compact byte array, never a parsed tree, so
 // dropping it returns the memory.
 QByteArray cache_;
 QString cacheId_;
 QTimer expiry_;
 int lifetimeMs_=resultLifetimeMs;
 qint64 copyMs_=0;
};
}
