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
 using ProviderSetup=std::function<void(const QString&,const Destination&,const QJsonObject&,JsonReply,std::function<void(QJsonObject)>)>;
 void setProviderSetup(ProviderSetup handler){providerSetup_=std::move(handler);}
 void start(const QString& kind,const QString& profileId,const QString& expectedRevision,const QJsonObject& input,JsonReply);
 void inspect(const QString& id,JsonReply);
 void credentialHint(const QString& profileId,const QString& expectedRevision,JsonReply);
 void revealCredentialSecret(const QString& profileId,const QString& expectedRevision,JsonReply);
 // Once per resident: starts an unlock job for each saved S3 keychain profile
 // that is not cached, unless the preference is off. The desktop keyring may
 // show its own unlock prompt. Returns the started setup IDs.
 QStringList unlockSavedCredentials();
signals:
 void changed(const QString& setupId);
private:
 bool startupUnlockDone_=false;
 struct CompletionContext {std::mutex mutex;SetupService* owner=nullptr;};
 void finish(const QString& id,Error,QJsonObject result={});
 SettingsService& settings_;
 CredentialStore& credentials_;
 DestinationTester tester_;
 ProviderSetup providerSetup_;
 QHash<QString,QJsonObject> jobs_;
 QQueue<QString> completed_;
 QSet<QString> active_;
 std::shared_ptr<CompletionContext> completion_;
};
}
