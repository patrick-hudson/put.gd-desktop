#pragma once
#include "HistoryService.h"
#include "LocalIpc.h"
#include "SetupService.h"
#include <QPointer>
#include <QSet>
namespace putgd {
class LocalServer final:public QObject {
 Q_OBJECT
public:
 LocalServer(SettingsService&,OperationService&,HistoryService&,QObject* parent=nullptr);
 Result listen(const QString& runtimeDirectory=QString{});
 QString socketPath() const {return server_.fullServerName();}
 QString residentEpoch() const {return residentEpoch_;}
 void setSetupService(SetupService* service) {setup_=service;}
 void setUiDiagnostics(std::function<QJsonObject()> callback) {uiDiagnostics_=std::move(callback);}
 void setUiQuit(std::function<void()> callback) {uiQuit_=std::move(callback);}
 static QString defaultRuntimeDirectory();
 static QString defaultSocketPath();
 static QJsonObject response(const QString& requestId,const Error&,const QJsonObject&);
 void dispatch(const QJsonObject&,JsonReply);
 void setUiActivation(std::function<void(const QString&,std::function<void(Error)>)> callback) {uiActivation_=std::move(callback); }
private:
 void accept();
 std::function<void(const QString&,std::function<void(Error)>)> uiActivation_;
 std::function<QJsonObject()> uiDiagnostics_;
 std::function<void()> uiQuit_;
 QPointer<SetupService> setup_;
 const QString residentEpoch_;
 SettingsService& settings_;
 OperationService& operations_;
 HistoryService& history_;
 LocalIpcServer server_;
 QSet<LocalIpcConnection*> clients_;
};
}
