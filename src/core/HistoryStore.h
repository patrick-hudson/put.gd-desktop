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
 Error forget(const QStringList& ids,const QSet<QString>& running,int* removed=nullptr);
 // Whether forget() would remove this record now: no link change pending and no deletion that may have reached the server.
 bool forgettable(const QString& historyId,const QSet<QString>& running) const;
 // requireRecords: a new deletion, recorded only while each of its records is still in History (#1759).
 Error saveDeletion(const QJsonObject&,bool requireRecords=false);
 QJsonObject deletion(const QString&,Error* = nullptr) const;
 Error saveSharing(const QJsonObject&,bool requireRecord=false); // journal and effective receipt in one transaction
 QJsonObject sharing(const QString&,Error* = nullptr) const;
 // Record a successful explicit link copy without rewriting item/deletion ledgers
 // or claiming that a copied subset recovered an entire batch.
 Error refreshLink(const QString& historyId,const StorageReceipt& before,const StorageReceipt& after);
 Error recordCopiedLink(const QString& historyId,QString& operationId);
 Error updateRemote(const QString& historyId,const QString& status,const Error& = {});
private:
 Error initialize();
 bool releasable(const QString& historyId,const QSet<QString>& running,QStringList* release=nullptr) const;
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
