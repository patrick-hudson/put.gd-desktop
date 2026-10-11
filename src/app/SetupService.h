#pragma once
#include "OperationService.h"
#include "secrets/CredentialStore.h"
#include <QQueue>
#include <QSet>
#include <memory>
#include <mutex>

namespace putgd {
class SetupService final : public QObject {
 Q_OBJECT
public:
 using DestinationTester=std::function<void(const QString&,const QJsonObject&,std::function<void(Error)>)>;
 SetupService(SettingsService&,CredentialStore&,DestinationTester,QObject* parent=nullptr);
 ~SetupService() override;
 void start(const QString& kind,const QString& profileId,const QString& expectedRevision,const QJsonObject& input,JsonReply);
 void inspect(const QString& id,JsonReply);
signals:
 void changed(const QString& setupId);
private:
 struct CompletionContext {std::mutex mutex;SetupService* owner=nullptr;};
 void finish(const QString& id,Error);
 SettingsService& settings_;
 CredentialStore& credentials_;
 DestinationTester tester_;
 QHash<QString,QJsonObject> jobs_;
 QQueue<QString> completed_;
 QSet<QString> active_;
 std::shared_ptr<CompletionContext> completion_;
};
}
