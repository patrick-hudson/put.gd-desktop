#pragma once
#include "core/Error.h"
#include "core/Operation.h"
#include "core/SettingsService.h"
#include <QHash>
#include <QObject>
#include <QSet>
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
 // Counts stores of new credentials (and Proton sign-ins) for a destination.
 quint64 credentialRevision(const QString& profileId) const {return credentialRevisions_.value(profileId);}
 void credentialsChanged(const QString& profileId);
 // A Test ended. fingerprint and revision are the ones it started with: a test that ran
 // while the settings or credentials changed records nothing.
 void testFinished(const QString& profileId,const QString& fingerprint,quint64 revision,const Error& error);
 // An upload operation starts its provider work, and its end. A file uploaded in this
 // run passes the destination; a sign-in, permission or missing bucket/folder refusal
 // fails it; anything else, such as a network failure, a timeout or a cancel, leaves it.
 void uploadStarting(const Operation& operation);
 void uploadSettled(const Operation& operation);
 // Forgets destinations that are no longer configured.
 void prune();
 // {profileId: {state: passed|failed|untested, source: test|upload, errorCode, fingerprint}}
 // for every configured destination, against its current settings and credentials.
 QJsonObject publicJson() const;
 QString path() const {return path_;}
signals:
 void changed();
private:
 struct Record {QString state,source,errorCode,fingerprint;quint64 credentialRevision=0;};
 struct Attempt {quint64 credentialRevision=0;QSet<QString> uploaded;};
 bool current(const QString& profileId,const QString& fingerprint,quint64 revision) const;
 void put(const QString& profileId,Record record);
 void save();
 QString path_;
 SettingsService& settings_;
 QHash<QString,Record> records_;
 QHash<QString,quint64> credentialRevisions_;
 QHash<QString,Attempt> attempts_;
};
}
