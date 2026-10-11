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
 ~UiSession() override;
 void refreshSnapshot(std::function<void(Error)> callback={});
 void applySnapshot(const QJsonObject&);
 void invalidate();
 const QJsonObject& document()const{return document_;}
 QJsonObject metadata()const{return metadata_;}
 QString revision()const{return metadata_["acceptedRevision"].toString();}
 QString path()const{return metadata_["path"].toString();}
 bool ready()const{return ready_;}
 QJsonObject diagnostics()const{return diagnostics_;}
 QJsonObject build()const{return build_;}
 QString residentEpoch()const{return epoch_;}
 SessionClient::RequestId request(const QString&,const QJsonObject&,QObject*,SessionClient::Reply);
 void saveAsync(const QJsonObject&,const QString&,std::function<void(Result)>);
 void reloadAsync(std::function<void(Result)>);
 void background(std::function<QJsonObject(Error&)>,QObject*,SessionClient::Reply);
signals:
 void accepted(QString revision);
 void statusChanged();
 void operationsChanged();
 void initialized();
 void unavailable(Error);
 void stateChanged();
private:
 void applySettings(const QJsonObject&);
 SessionClient& client_;
 QJsonObject document_,metadata_,diagnostics_,build_;
 QString epoch_;
 QThreadPool previews_;
 bool ready_=false,snapshotActive_=false,snapshotPending_=false;
 int mutations_=0;
 quint64 generation_=0;
 QList<std::function<void(Error)>> snapshotCallbacks_;
};
}
