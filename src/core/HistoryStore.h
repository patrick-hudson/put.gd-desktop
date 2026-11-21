#pragma once
#include <optional>
#include "Operation.h"
#include <QSqlDatabase>
#include <QSet>
namespace putgd {
struct HistoryQuery {
 int limit=50,offset=0;
 QString search,category,mime,profileId,uploadStatus,localStatus,remoteStatus,from,to;
};
struct HistoryPage { QJsonArray records; qint64 total=0; Error error; QJsonObject toJson() const; };
// The error log (#1753): user-facing errors, newest first. resolved -1 is either.
struct ErrorLogQuery {
 int limit=50,offset=0,resolved=-1;
 QString kind,search,historyId,operationId;
};
// Instances belong to the creating worker thread; never share a Qt SQL connection.
class HistoryStore {
public:
 explicit HistoryStore(QString path);
 ~HistoryStore();
 HistoryStore(const HistoryStore&)=delete;
 Error error() const {return error_;}
 Error saveOperation(const Operation&,const ProviderRegistry* = nullptr); // operation and item ledger in one transaction
 // Persist the full operation snapshot atomically with only changed ledger items.
 Error checkpointOperation(const Operation&,const QSet<QString>& changedItems,const ProviderRegistry* = nullptr);
 // A completed or cancelled text operation leaves no journal row (#1428), so a
 // much-used action does not grow the journal. Failed and interrupted ones stay.
 Error removeTextOperation(const QString& id);
 Operation operation(const QString&,Error* = nullptr) const;
 QList<Operation> recover(Error* = nullptr);
 Error indexPublications(const ProviderRegistry&);
 HistoryPage list(const HistoryQuery&) const;
 // History's counts: per-category totals of every stored file (all, images,
 // videos, text+other, failed; failed counts failed and unknown-outcome uploads),
 // the query's total, and the query's files in
 // each [from, to] ISO range of days (at most 64).
 QJsonObject counts(const HistoryQuery&,const QList<std::pair<QString,QString>>& days,Error* = nullptr) const;
 QJsonObject detail(const QString&,Error* = nullptr) const;
 QJsonArray recent(int limit,Error* = nullptr) const;
 // Operations created at or after `from` (ISO UTC) in one of `states`, newest
 // first, at most `limit`; for diagnostics packages (#74).
 QList<Operation> operationsSince(const QString& from,const QStringList& states,int limit,Error* = nullptr) const;
 // The newest finished upload operation to a destination (completed, partial or
 // failed), or none: the diagnostics upload's "working" check (#123).
 std::optional<Operation> lastUpload(const QString& profileId,Error* = nullptr) const;
 // Operations with an item whose upload journal still names a remote upload (#146).
 QStringList journaledOperations(Error* = nullptr) const;
 Error expireBefore(const QString& timestamp);
 Error clear(); // preserves any identities retained by in-progress deletion
 // Removes these records from History the way clear() removes all of them (#1759).
 // Releases the settled deletions that sent nothing and removes the records, in one transaction;
 // `running` are the deletions running in this resident.
 Error forget(const QStringList& ids,const QSet<QString>& running,int* removed=nullptr,QStringList* removedIds=nullptr);
 // Why forget() would keep this record now: "sharing" (a link change pending or unknown), "deletion"
 // (a deletion running or one that may have reached the server), or empty when it would remove it.
 QString forgetBlocker(const QString& historyId,const QSet<QString>& running) const;
 // requireRecords: a new deletion, recorded only while each of its records is still in History (#1759).
 Error saveDeletion(const QJsonObject&,bool requireRecords=false);
 // A deletion's run starts: records removed from History only are dropped from it (doc["forgotten"]).
 Error startDeletion(QJsonObject& doc);
 QJsonObject deletion(const QString&,Error* = nullptr) const;
 Error saveSharing(const QJsonObject&,bool requireRecord=false); // journal and effective receipt in one transaction
 QJsonObject sharing(const QString&,Error* = nullptr) const;
 // Record a successful explicit link copy without rewriting item/deletion ledgers
 // or claiming that a copied subset recovered an entire batch.
 Error refreshLink(const QString& historyId,const StorageReceipt& before,const StorageReceipt& after);
 Error recordCopiedLink(const QString& historyId,QString& operationId);
 Error updateRemote(const QString& historyId,const QString& status,const Error& = {});
 // The error log (#1753), a table beside the journal: entries are already redacted
 // JSON objects with id, time, kind and subject. saveError inserts or replaces one by
 // ID, and then keeps
 // the newest `keep` entries written at or after `notBefore`.
 Error saveError(QJsonObject entry,int keep,const QString& notBefore,QJsonObject* saved=nullptr);
 // An item's name (from History, or its operation once History was cleared) and the
 // destination its receipt names: {name, profileId, providerId}, each when known.
 QJsonObject errorItem(const QString& historyId,const QString& operationId) const;
 // Marks every open entry about `subject` resolved, recorded before now; answers their IDs.
 Error resolveErrors(const QString& subject,const QString& at,const QString& resolution,QStringList* ids=nullptr);
 // {entries, total, unresolved}: the page newest first, each entry with the item's current
 // name, previewPath and itemPresent when it names a History item.
 QJsonObject listErrors(const ErrorLogQuery&,Error* = nullptr) const;
 QJsonObject errorEntry(const QString& id,Error* = nullptr) const;
 // Removes the given entries (all when ids is empty), or only resolved ones.
 Error clearErrors(const QStringList& ids,bool resolvedOnly,int* removed=nullptr);
 // {total, unresolved, subjects}: the open subjects, so a later success can resolve them.
 QJsonObject errorSummary(Error* = nullptr) const;
private:
 Error ensureErrorLog();
 Error initialize();
 bool releasable(const QString& historyId,const QSet<QString>& running,QStringList* release=nullptr) const;
 bool writeDeletion(const QJsonObject&);
 bool beginWrite();
 Error persistOperation(const Operation&,const ProviderRegistry*,const QSet<QString>* changedItems);
 // Stored artifact and preview paths are absolute. A journal moved with its
 // data folder (#56) reads them under the folder it is in now.
 void relocate(Operation&) const;
 QJsonObject relocate(QJsonObject record) const;
 QString root_;
 QString connection_;
 QSqlDatabase db_;
 Error error_;
};
}
