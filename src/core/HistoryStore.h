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
 Operation operation(const QString&,Error* = nullptr) const;
 QList<Operation> recover(Error* = nullptr);
 Error indexPublications(const ProviderRegistry&);
 HistoryPage list(const HistoryQuery&) const;
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
 Error saveDeletion(const QJsonObject&);
 QJsonObject deletion(const QString&,Error* = nullptr) const;
 Error saveSharing(const QJsonObject&); // journal and effective receipt in one transaction
 QJsonObject sharing(const QString&,Error* = nullptr) const;
 Error updateRemote(const QString& historyId,const QString& status,const Error& = {});
private:
 Error initialize();
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
