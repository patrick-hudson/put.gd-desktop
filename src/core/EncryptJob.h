#pragma once
#include "ArtifactStore.h"
#include "Error.h"
#include <QJsonObject>
#include <atomic>
#include <functional>
#include <memory>
namespace putgd {
// Encrypted links (#2079): encrypts one stored file in a short-lived `putgd-cli --encrypt-job` child,
// so the resident never maps libcrypto. The passphrase goes to the child on its standard
// input only; the key comes back on its standard output. The pages it writes are checked
// here (size and SHA-256) before they become artifacts. Synchronous: run it on a worker.
// The credential-store key of a destination's Encrypted links passphrase (its own entry, beside the
// destination's keys): the reference is profile.encrypt.passphraseCredentials.
inline QString encryptPassphraseId(const QString& profileId) {return QStringLiteral("encrypt:")+profileId;}
class EncryptJob final {
public:
 using Cancel=std::shared_ptr<std::atomic_bool>;
 explicit EncryptJob(QString helperPath={},int timeoutMs=30*60*1000);
 static QString defaultHelper();
 struct Result {Artifact page,data;QString key;Error error;};
 // `form` is inline or split; split also writes the data file, which the page names as `bin`.
 // `directory` is the operation's private artifact folder; new files there get UUID names.
 Result run(const Artifact& file,const QString& directory,const QString& form,const QString& pageName,const QString& binName,const QString& passphrase,Cancel={}) const;
 // A random name for an encrypted object: 12 letters and digits, then the extension.
 static QString randomName(const QString& extension);
 // Tests: the output files' names in `directory`, in place of new UUIDs.
 void setOutputIdsForTesting(QString page,QString data){pageIdForTesting_=std::move(page);dataIdForTesting_=std::move(data);}
 // Tests: runs as the resident starts checking the helper's output.
 void setVerifyingForTesting(std::function<void()> hook){verifyingForTesting_=std::move(hook);}
private:
 QString helperPath_;int timeoutMs_;QString pageIdForTesting_,dataIdForTesting_;std::function<void()> verifyingForTesting_;
};
}
