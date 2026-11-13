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
 // Category totals, the query's total and its files per day range (HistoryStore::counts).
 void counts(const HistoryQuery&,const QList<std::pair<QString,QString>>& days,JsonReply);
 void detail(const QString&,JsonReply);
 // Makes the item's missing image or video preview (#190); see OperationService::makePreview.
 void preview(const QString&,JsonReply,bool retry=false);
 void clear(JsonReply);
 void copyImage(const QString&,JsonReply);
 void unlockLinkCredentials(const QString&,JsonReply);
 void copyLink(const QString&,JsonReply);
 // Several stored links at once, one per line in selection order: all of
 // them, or none when any is no longer available.
 void copyLinks(const QStringList& ids,JsonReply);
 // history.qr (#1428): the QR code of the link Copy link would copy now, after
 // the same receipt refresh, as a PNG on the clipboard (copy) or in a new file
 // (save; an existing one only with overwrite), or handed to `show` for the
 // History window. Refused with link_unavailable and its reason for a private,
 // removed or revoked link and an expired one that cannot be refreshed, and with
 // qr_too_long over 2,331 bytes.
 using QrShow=std::function<void(const QJsonObject& link,JsonReply)>;
 void qr(const QString& id,const QString& mode,const QString& path,bool overwrite,JsonReply,QrShow show={});
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
 QJsonObject copyableLink(const QString&,Error&);
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
