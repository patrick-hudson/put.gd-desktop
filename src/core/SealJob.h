#pragma once
#include "ArtifactStore.h"
#include "Error.h"
#include <QJsonObject>
#include <atomic>
#include <functional>
#include <memory>
namespace putgd {
// Sealed links (#2079): seals one stored file in a short-lived `putgd-cli --seal-job` child,
// so the resident never maps libcrypto. The passphrase goes to the child on its standard
// input only; the key comes back on its standard output. The pages it writes are checked
// here (size and SHA-256) before they become artifacts. Synchronous: run it on a worker.
// The credential-store key of a destination's Seal links passphrase (its own entry, beside the
// destination's keys): the reference is profile.seal.passphraseCredentials.
inline QString sealPassphraseId(const QString& profileId) {return QStringLiteral("seal:")+profileId;}
class SealJob final {
public:
 using Cancel=std::shared_ptr<std::atomic_bool>;
 explicit SealJob(QString helperPath={},int timeoutMs=30*60*1000);
 static QString defaultHelper();
 struct Result {Artifact page,data;QString key;Error error;};
 // `form` is inline or split; split also writes the data file, which the page names as `bin`.
 // `directory` is the operation's private artifact folder; new files there get UUID names.
 Result run(const Artifact& file,const QString& directory,const QString& form,const QString& pageName,const QString& binName,const QString& passphrase,Cancel={}) const;
 // A random name for a sealed object: 12 letters and digits, then the extension.
 static QString randomName(const QString& extension);
 // Tests: the output files' names in `directory`, in place of new UUIDs.
 void setOutputIdsForTesting(QString page,QString data){pageIdForTesting_=std::move(page);dataIdForTesting_=std::move(data);}
 // Tests: runs as the resident starts checking the helper's output.
 void setVerifyingForTesting(std::function<void()> hook){verifyingForTesting_=std::move(hook);}
private:
 QString helperPath_;int timeoutMs_;QString pageIdForTesting_,dataIdForTesting_;std::function<void()> verifyingForTesting_;
};
}
