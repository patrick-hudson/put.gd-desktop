#pragma once
#include "core/Error.h"
#include "core/Operation.h"
#include "core/SettingsService.h"
#include <QHash>
#include <QObject>
#include <QSet>
#include <functional>
#include <mutex>
namespace putgd {
class OperationService;
// The resident's record of each destination's last check (#1692): passed or failed, by a
// Test or by an upload, and for which connection (destinationcheck::fingerprint) and
// credential revision. It lives in the data folder as destination-status.json, owner-only,
// with no credential or link in it, so closing Settings, a restart and logging in again
// keep it. A record counts only while its fingerprint and credential revision are the
// destination's current ones; changing a connection setting or storing new credentials
// makes it stale without a write. Nothing checks a destination on a timer.
class DestinationStatus final:public QObject {
 Q_OBJECT
public:
 DestinationStatus(QString path,SettingsService&,QObject* parent=nullptr);
 // Reads the file. A missing, unreadable or invalid file starts empty.
 void load();
 // Follows uploads and forgets removed destinations from then on.
 void follow(OperationService& operations);
 // Whether a destination keeps keys (SetupService::storesKeys, and a custom uploader with secret
 // values). A session-mode destination that does is kept in memory only, so after a restart it is
 // untested. Unset, every destination counts as keeping keys. SFTP follows setHoldsSessionSecret.
 void setUsesCredentials(std::function<bool(const QJsonObject& profile)> uses){usesCredentials_=std::move(uses);}
 // #2118: whether the session credential cache holds a non-empty SFTP password or key passphrase
 // for this destination right now. Unset, no SFTP destination counts as session-only.
 void setHoldsSessionSecret(std::function<bool(const QString& profileId,const QJsonObject& profile)> holds){holdsSessionSecret_=std::move(holds);}
 // Counts stores of new credentials (and Proton sign-ins) for a destination.
 quint64 credentialRevision(const QString& profileId) const {return credentialRevisions_.value(profileId);}
 // A store of credentials starts or succeeds: every configured destination that names the
 // same keychain entry has new keys too.
 void credentialsChanged(const QString& profileId);
 // A Test ended. fingerprint and revision are the ones it started with: a test that ran
 // while the settings or credentials changed records nothing.
 void testFinished(const QString& profileId,const QString& fingerprint,quint64 revision,const Error& error);
 // An upload operation starts its provider work, and its end. A file uploaded in this
 // run passes the destination; a sign-in, permission or missing bucket/folder refusal
 // this run met fails it; anything else, such as a network failure, a timeout, a cancel
 // or an error a retry carried unchanged from History, leaves it.
 void uploadStarting(const Operation& operation);
 void uploadSettled(const Operation& operation);
 // Keychain entries put.gd saved credentials to successfully (#1890), kept across restarts and
 // apart from the revisions above: a store that keeps one secret trusts "not found" only for an
 // entry put.gd never wrote. Forgotten once no destination names the entry.
 bool keychainSaved(const QString& reference) const {return keychainEntries_.contains(reference);}
 void keychainWritten(const QString& reference);
 // Encrypted links (#2079): the Test check of whether this destination serves an encrypted page
 // as a page (text/html, inline, the exact bytes). Tied to the connection fingerprint
 // like a test; the Encrypted links setting and per-upload Encrypt are available only while it
 // passed. reason is one sentence when it failed.
 void encryptChecked(const QString& profileId,const QString& fingerprint,bool passed,const QString& reason);
 // {state: passed|failed|changed|untested, reason?} against the destination's current settings:
 // changed when the check ran on an earlier connection (#2118), untested when it never ran on
 // these keys (also every destination set up before the check existed).
 QJsonObject encryptCheck(const QString& profileId) const;
 // The same for a connection fingerprint the caller computed (an accepted upload's own
 // settings); safe from any thread.
 QJsonObject encryptCheckFor(const QString& profileId,const QString& fingerprint) const;
 // Forgets destinations that are no longer configured.
 void prune();
 // {profileId: {state: passed|failed|untested, source: test|upload, errorCode, fingerprint}}
 // for every configured destination, against its current settings and credentials.
 QJsonObject publicJson() const;
signals:
 void changed();
private:
 // sessionOnly: made with keys kept only for this session, so never written (#2118).
 struct Record {QString state,source,errorCode,fingerprint;quint64 credentialRevision=0;bool sessionOnly=false;};
 // Whether a check made now would be made with keys kept only for this session.
 bool sessionOnlyNow(const QString& profileId) const;
 // What the operation held when this run started: its uploaded files and each item's
 // error code.
 struct Attempt {quint64 credentialRevision=0;QSet<QString> uploaded;QHash<QString,QString> errors;};
 bool current(const QString& profileId,const QString& fingerprint,quint64 revision) const;
 void put(const QString& profileId,Record record);
 void save();
 QString path_;
 SettingsService& settings_;
 QHash<QString,Record> records_;
 struct EncryptRecord {bool passed=false;QString reason,fingerprint;quint64 credentialRevision=0;bool sessionOnly=false;};
 QHash<QString,EncryptRecord> encryptRecords_; // written on the owning thread, under encryptMutex_
 mutable std::mutex encryptMutex_;
 QHash<QString,quint64> credentialRevisions_;
 QSet<QString> keychainEntries_;
 QHash<QString,Attempt> attempts_;
 std::function<bool(const QJsonObject&)> usesCredentials_;
 std::function<bool(const QString&,const QJsonObject&)> holdsSessionSecret_;
};
}
