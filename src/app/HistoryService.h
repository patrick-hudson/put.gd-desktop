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
 // Makes the item's missing image or video preview (#190); see OperationService::makePreview.
 void preview(const QString&,JsonReply);
 void clear(JsonReply);
 void copyImage(const QString&,JsonReply);
 void copyLink(const QString&,JsonReply);
 void changeSharing(const QString& historyId,const QString& action,const QJsonObject& options,JsonReply);
 void inspectSharing(const QString&,JsonReply);
 void prepareDelete(const QStringList&,JsonReply);
 void deleteConfirmed(const QStringList&,const QString& acknowledgement,JsonReply);
 void inspectDelete(const QString&,JsonReply);
 void retryDelete(const QString&,JsonReply);
 void cancelDelete(const QString&,JsonReply);
signals:
 void changed();
private:
 void notifyChanged();
 QJsonObject prepare(const QStringList&,Error&) const;
 void expireConfigured();
 void recordStoppedSharing(QJsonObject,const Error&);
 void runSharing(QJsonObject,std::shared_ptr<std::atomic_bool>);
 void runDelete(QJsonObject,std::shared_ptr<std::atomic_bool>);
 OperationService& operations_;
 QTimer retention_,notification_;
 QHash<QString,std::shared_ptr<std::atomic_bool>> active_;
 // Stopped jobs whose last state could not be committed. Never replay a mutation.
 QHash<QString,QJsonObject> stoppedSharing_;
};
}
