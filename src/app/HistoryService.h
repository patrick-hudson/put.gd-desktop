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
 // Remove from History only (#1759): these records leave History; the files and any deletion or
 // sharing in progress for them are untouched. Answers {removed}.
 void forget(const QStringList& ids,JsonReply);
 void copyImage(const QString&,JsonReply);
 void unlockLinkCredentials(const QString&,JsonReply);
 // Unlocks the saved credentials of the destination the item was uploaded to.
 void unlockItemCredentials(const QString&,JsonReply);
 void copyLink(const QString&,JsonReply);
 // Forget key (#2079): the encrypted link's key leaves History; the file stays where it was uploaded.
 void forgetEncryptKey(const QString&,JsonReply);
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
 // Nextcloud (#1872): the share ID an earlier step recorded and, for a revoke, a SHA-256 of the
 // recorded link's token, for the provider to find put.gd's own share. Empty for other providers.
 static QJsonObject recordedShare(const StorageReceipt& receipt,const QJsonObject& previousProgress,const QString& action);
 void prepareDelete(const QStringList&,JsonReply);
 void deleteConfirmed(const QStringList&,const QString& acknowledgement,JsonReply);
 void inspectDelete(const QString&,JsonReply);
 void retryDelete(const QString&,JsonReply);
 void cancelDelete(const QString&,JsonReply);
 // Quit's first step (#1602): no new deletion or retry is admitted, and the helper of
 // every running deletion, and of a Copy Link renewal, is killed, so it sends nothing more. What it had sent is
 // recorded unknown with its journaled intent; work that sent nothing is left as it was.
 void stopDeletions();
signals:
 void changed();
 // A deletion or sharing change ended; its terminal journal document (#1753).
 void deletionFinished(const QJsonObject& deletion);
 // Remove from History only removed these records (#1759).
 void recordsForgotten(const QStringList& historyIds);
 void sharingFinished(const QJsonObject& sharing);
private:
 QJsonObject copyableLink(const QString&,Error&);
 void notifyChanged();
 QJsonObject prepare(const QStringList&,const QJsonArray& profiles,const QSet<QString>& runningDeletions,Error&) const;
 // Marks an uploaded record (and its grouped records) whose destination was removed from
 // Settings with nothing at the same place: destinationRemoved true (#1759).
 static void markRemovedDestinations(QJsonObject& record,const QJsonArray& profiles);
 void expireConfigured();
 void recordStoppedSharing(QJsonObject,const Error&);
 void runSharing(QJsonObject,std::shared_ptr<std::atomic_bool>);
 void runDelete(QJsonObject,std::shared_ptr<std::atomic_bool>);
 OperationService& operations_;
 QTimer retention_,notification_;
 QHash<QString,std::shared_ptr<std::atomic_bool>> active_;
 // Set by stopDeletions(): every deletion's helper kill, and the end of admission.
 std::shared_ptr<std::atomic_bool> quitting_=std::make_shared<std::atomic_bool>(false);
 // Stopped jobs whose last state could not be committed. Never replay a mutation.
 QHash<QString,QJsonObject> stoppedSharing_;
 // The accepted destinations, for workers (#1759): copied here when Settings change.
 mutable std::mutex profilesMutex_;QJsonArray profiles_;
 QJsonArray currentProfiles() const {std::lock_guard lock(profilesMutex_);return profiles_;}
};
}
