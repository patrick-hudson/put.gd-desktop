#pragma once
#include "Operation.h"
#include <QSqlDatabase>
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
 Error saveOperation(const Operation&); // operation and item ledger in one transaction
 Operation operation(const QString&,Error* = nullptr) const;
 QList<Operation> recover(Error* = nullptr);
 HistoryPage list(const HistoryQuery&) const;
 QJsonObject detail(const QString&,Error* = nullptr) const;
 QJsonArray recent(int limit,Error* = nullptr) const;
 Error expireBefore(const QString& timestamp);
 Error clear(); // preserves any identities retained by in-progress deletion
 Error saveDeletion(const QJsonObject&);
 QJsonObject deletion(const QString&,Error* = nullptr) const;
 Error updateRemote(const QString& historyId,const QString& status,const Error& = {});
private:
 Error initialize();
 QString connection_;
 QSqlDatabase db_;
 Error error_;
};
}
