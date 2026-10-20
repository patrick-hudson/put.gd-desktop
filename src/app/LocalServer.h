#pragma once
#include "HistoryService.h"
#include "LocalIpc.h"
#include "DiagnosticsBundle.h"
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
 void setDiagnosticsBundles(DiagnosticsBundles* bundles) {bundles_=bundles;}
 void setUiDiagnostics(std::function<QJsonObject()> callback) {uiDiagnostics_=std::move(callback);}
 void setUiQuit(std::function<void()> callback) {uiQuit_=std::move(callback);}
 void setUiViewState(std::function<QJsonObject()> callback) {uiViewState_=std::move(callback);}
 static QString defaultRuntimeDirectory();
 static QString defaultSocketPath();
 static QJsonObject response(const QString& requestId,const Error&,const QJsonObject&);
 // How long a client waits for this method's reply.
 static int replyDeadlineMs(const QString& method);
 void dispatch(const QJsonObject&,JsonReply);
 void setUiActivation(std::function<void(const QString&,std::function<void(Error)>)> callback) {uiActivation_=std::move(callback); }
 // A command for an open interface view; the helper validates its semantics.
 void setUiCommand(std::function<void(const QString&,const QJsonObject&,JsonReply)> callback) {uiCommand_=std::move(callback);}
 // The resident's "diagnostics saved" notification (with Show in folder) and
 // Show in folder itself (#122).
 void setDiagnosticsNotifications(std::function<void(const QJsonObject&)> saved,std::function<Error(const QString&)> reveal) {diagnosticsSaved_=std::move(saved);revealPackage_=std::move(reveal);}
private:
 void accept();
 std::function<void(const QString&,std::function<void(Error)>)> uiActivation_;
 std::function<void(const QString&,const QJsonObject&,JsonReply)> uiCommand_;
 std::function<QJsonObject()> uiDiagnostics_,uiViewState_;
 std::function<void()> uiQuit_;
 std::function<void(const QJsonObject&)> diagnosticsSaved_;
 std::function<Error(const QString&)> revealPackage_;
 QPointer<SetupService> setup_;
 QPointer<DiagnosticsBundles> bundles_;
 const QString residentEpoch_;
 SettingsService& settings_;
 OperationService& operations_;
 HistoryService& history_;
 LocalIpcServer server_;
 QSet<LocalIpcConnection*> clients_;
};
}
