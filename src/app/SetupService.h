#pragma once
#include "OperationService.h"
#include "secrets/CredentialStore.h"
#include "DestinationStatus.h"
#include <QQueue>
#include <QSet>
#include <atomic>
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
 // A provider whose credentials are named values instead of a key pair (a custom
 // HTTP uploader, #1361). Given a saved profile and typed values, it fills `names`
 // with the credential-store keys the profile's settings need and checks the values;
 // setup_unsupported for any other provider. storeCredentials then merges the typed
 // values over the cached ones and stores the set.
 using NamedCredentials=std::function<Error(const QJsonObject& profile,const QMap<QString,QString>& values,QStringList* names)>;
 void setNamedCredentials(NamedCredentials handler){namedCredentials_=std::move(handler);}
 // Where a Test's result and a store of new credentials are recorded (#1692).
 void setDestinationStatus(DestinationStatus* status){status_=status;}
 void start(const QString& kind,const QString& profileId,const QString& expectedRevision,const QJsonObject& input,JsonReply);
 void inspect(const QString& id,JsonReply);
 // Asks a running provider setup job to stop (a Nextcloud sign-in waiting for the browser,
 // #1872). The job then ends on its own with its provider's answer.
 void cancel(const QString& id,JsonReply);
 // The stop flag of the running provider setup job of this kind for this destination, for the
 // provider handler; a fresh unset flag when none is running.
 std::shared_ptr<std::atomic_bool> cancellation(const QString& profileId,const QString& kind) const;
 void credentialHint(const QString& profileId,const QString& expectedRevision,JsonReply);
 void revealCredentialSecret(const QString& profileId,const QString& expectedRevision,JsonReply);
 // Once per resident: starts an unlock job for each saved S3 keychain profile
 // that is not cached, unless the preference is off. The desktop keyring may
 // show its own unlock prompt. Returns the started setup IDs.
 QStringList unlockSavedCredentials();
 // Whether that startup read has run and every job it started has ended. A
 // file-manager upload waits for it (#1105): a resident started for one would
 // otherwise reach the destination before its saved credentials are read.
 bool savedCredentialsSettled() const {return startupUnlockDone_&&startupUnlocks_.isEmpty();}
 // Counts finished storeCredentials and unlockCredentials jobs, whoever started
 // them. ui.snapshot carries it so an open Destinations page asks for its key
 // hint again when the startup unlock ends after the page opened (#600).
 quint64 credentialGeneration() const {return credentialGeneration_;}
signals:
 void changed(const QString& setupId);
private:
 bool startupUnlockDone_=false;
 quint64 credentialGeneration_=0;
 QSet<QString> startupUnlocks_;
 struct CompletionContext {std::mutex mutex;SetupService* owner=nullptr;};
 void finish(const QString& id,Error,QJsonObject result={});
 SettingsService& settings_;
 CredentialStore& credentials_;
 DestinationTester tester_;
 ProviderSetup providerSetup_;
 NamedCredentials namedCredentials_;
 DestinationStatus* status_=nullptr;
 // A running Test's connection fingerprint and credential revision when it started.
 struct TestStart {QString fingerprint;quint64 credentialRevision=0;};
 QHash<QString,TestStart> testStarts_;
 bool usesCredentialStore(const QJsonObject& profile) const;
 QHash<QString,QJsonObject> jobs_;
 QQueue<QString> completed_;
 QSet<QString> active_;
 QHash<QString,std::shared_ptr<std::atomic_bool>> cancellations_; // by job ID
 std::shared_ptr<CompletionContext> completion_;
};
}
