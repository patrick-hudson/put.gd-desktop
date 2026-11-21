#pragma once
#include "OperationService.h"
#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QSet>
#include <deque>
namespace putgd {
// The error log (#1753): every user-facing error put.gd reports, kept so it can be
// reviewed later in Settings → Error log, through errors.* and the MCP helper. It
// lives in the history database's error_log table (HistoryStore), next to the items
// and operations its entries name, and is written by one queue so an entry and the
// later success that resolves it keep their order. Entries are redacted the way log
// records are before they are stored. The newest `keep` entries younger than
// `keepDays` are kept.
class ErrorLog final:public QObject {
 Q_OBJECT
public:
 static constexpr int keep=500,keepDays=90,maximumDetailBytes=16*1024;
 // A repeat of the same open problem within this many seconds updates its entry.
 static constexpr int coalesceSeconds=600;
 struct Entry {
  QString kind,action,code,message;
  QJsonObject details;
  QString operationId,itemId,historyId,deletionId,sharingId,setupId,profileId,itemName,provider,destination;
  // What a later success resolves: "item:<historyId>", "deletion:<historyId>",
  // "operation:<id>", "test:<profileId>", "settings", ... Empty: never resolved by itself.
  QString subject;
  // Next steps the entry offers, when not worked out from the rest.
  QStringList steps;
 };
 explicit ErrorLog(OperationService&,QObject* parent=nullptr);
 // The kinds of action an entry can be about, in the order Settings lists them.
 static const QStringList& kinds();
 // An operation's kind from its action: capture, recording, upload, text_recognition or diagnostics.
 static QString kindForAction(const QString& action);
 // Whether a session method's error is one a person should find in the log: never
 // a malformed request, a busy or not-yet-ready resident, or a closed window.
 static bool userFacing(const Error&);
 // Records an entry and answers its ID at once; the write follows on the queue.
 QString record(Entry);
 QString record(const QString& kind,const Error& error,Entry context={});
 // Marks the open entries about `subject` resolved: the problem went away.
 void resolve(const QString& subject,const QString& resolution);
 // Entries from an operation's terminal snapshot (publicJson), and the successes it shows.
 // The constructor follows OperationService's results itself; deletions, sharing and setup
 // jobs are connected by their owner.
 void operationFinished(const QJsonObject& operation);
 // From a deletion's terminal journal document and a sharing change's.
 void deletionFinished(const QJsonObject& deletion);
 void sharingFinished(const QJsonObject& sharing);
 // From a setup job's record (setup.inspect) once it has ended.
 void setupFinished(const QJsonObject& job);
 void list(const ErrorLogQuery&,JsonReply);
 void inspect(const QString& id,JsonReply);
 void clear(const QStringList& ids,bool resolvedOnly,JsonReply);
 // {revision, total, unresolved}: the snapshot's field, so open windows refresh.
 QJsonObject summary() const;
signals:
 void changed();
private:
 using Work=std::function<QJsonObject(HistoryStore&,Error&)>;
 void enqueue(Work,JsonReply done={});
 void pump();
 void load();
 QJsonObject entryJson(const Entry&,const QString& id,const QDateTime& now,int occurrences,const QString& first) const;
 QString destinationLabel(const QString& profileId,const QString& provider) const;
 OperationService& operations_;
 struct Job {Work work;JsonReply done;};
 std::deque<Job> queue_;
 bool running_=false,loaded_=false;
 quint64 revision_=0;
 qint64 total_=0,unresolved_=0;
 // Subjects with an open entry, so a success elsewhere costs nothing when none is open.
 QSet<QString> open_;
 struct Recent {QString id,first;QDateTime last;int occurrences=1;};
 QHash<QString,Recent> recent_;
};
}
