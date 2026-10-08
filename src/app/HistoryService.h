#pragma once
#include "OperationService.h"
#include <QTimer>
namespace putgd {
class HistoryService final:public QObject {
 Q_OBJECT
public:
 explicit HistoryService(OperationService&,QObject* parent=nullptr);
 ~HistoryService() override;
 void list(const HistoryQuery&,JsonReply);
 void detail(const QString&,JsonReply);
 void clear(JsonReply);
 void copyLink(const QString&,JsonReply);
 void prepareDelete(const QStringList&,JsonReply);
 void deleteConfirmed(const QStringList&,const QString& acknowledgement,JsonReply);
 void inspectDelete(const QString&,JsonReply);
 void retryDelete(const QString&,JsonReply);
 void cancelDelete(const QString&,JsonReply);
private:
 QJsonObject prepare(const QStringList&,Error&) const;
 void expireConfigured();
 void runDelete(QJsonObject,std::shared_ptr<std::atomic_bool>);
 OperationService& operations_;
 QTimer retention_;
 QHash<QString,std::shared_ptr<std::atomic_bool>> active_;
};
}
