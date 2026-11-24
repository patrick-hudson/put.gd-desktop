#pragma once
#include "client/SessionClient.h"
#include <QThreadPool>
namespace putgd {
// The UI owns only cached JSON and one bounded thumbnail worker. All mutations go
// through the resident endpoint and are never replayed on transport failure.
class UiSession final:public QObject {
 Q_OBJECT
public:
 explicit UiSession(SessionClient&,QObject* parent=nullptr);
 // How many History previews decode at once (#1937).
 static int previewWorkers(bool allCores,int cores,qint64 memoryAvailable);
 int previewWorkers() const {return previews_.maxThreadCount();}
 ~UiSession() override;
 void refreshSnapshot(std::function<void(Error)> callback={});
 void applySnapshot(const QJsonObject&);
 void invalidate();
 const QJsonObject& document()const{return document_;}
 QJsonObject metadata()const{return metadata_;}
 QString revision()const{return metadata_["acceptedRevision"].toString();}
 QString path()const{return metadata_["path"].toString();}
 bool ready()const{return ready_;}
 // While the journal is unavailable, why: the resident's recovery error, or "initializing" during its first attempt.
 Error journalError()const{return journalError_;}
 QJsonObject diagnostics()const{return diagnostics_;}
 QJsonObject build()const{return build_;}
 QString residentEpoch()const{return epoch_;}
 // #1962: the resident's secret for the person's own Copy text (History's and the dashboard's), from the
 // private control pipe. Sent only with those requests; agents never see it.
 void setPersonToken(QString token){personToken_=std::move(token);}
 QString personToken()const{return personToken_;}
 // Each destination's last check, as the resident keeps it (#1692): passed, failed or
 // untested against the saved settings, with the connection fingerprint it was for.
 QJsonObject destinationStatus()const{return destinationStatus_;}
 // The error log's {revision, total, unresolved} (#1753).
 QJsonObject errorLog()const{return errorLog_;}
 SessionClient::RequestId request(const QString&,const QJsonObject&,QObject*,SessionClient::Reply);
 void saveAsync(const QJsonObject&,const QString&,std::function<void(Result)>);
 void reloadAsync(std::function<void(Result)>);
 using ShortcutRecordingHandler=std::function<void(bool,std::function<void(Error)>)>;
 void setShortcutRecordingHandler(ShortcutRecordingHandler handler){shortcutRecording_=std::move(handler);}
 void setShortcutRecording(bool active,QObject* receiver,std::function<void(Error)> reply);
 void background(std::function<QJsonObject(Error&)>,QObject*,SessionClient::Reply);
signals:
 void accepted(QString revision);
 void statusChanged();
 void operationsChanged();
 void initialized();
 void unavailable(Error);
 void stateChanged();
 // A storeCredentials or unlockCredentials job ended since the last snapshot,
 // such as the resident's startup unlock: saved-key hints may have changed.
 void credentialsChanged();
 // A destination's last check changed in the resident: a Test, an upload or new credentials.
 void destinationStatusChanged();
 void errorLogChanged();
private:
 QString personToken_;
 void applySettings(const QJsonObject&);
 ShortcutRecordingHandler shortcutRecording_;
 SessionClient& client_;
 QJsonObject document_,metadata_,diagnostics_,build_,destinationStatus_,errorLog_;
 QString epoch_;
 Error journalError_;
 QThreadPool previews_;
 bool ready_=false,snapshotActive_=false,snapshotPending_=false;
 int mutations_=0;
 quint64 generation_=0;
 qint64 credentialGeneration_=-1;
 QList<std::function<void(Error)>> snapshotCallbacks_;
};
}
