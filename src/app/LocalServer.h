#pragma once
#include "HistoryService.h"
#include "ErrorLog.h"
#include "LocalIpc.h"
#include "DiagnosticsBundle.h"
#include "SetupService.h"
#include "UpdateService.h"
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
 // Each destination's last check (#1692), in ui.snapshot and settings.get as destinationStatus.
 void setDestinationStatus(DestinationStatus* status) {destinationStatus_=status;}
 // settings.importUploader (#1361): a provider adds a destination from a file the
 // resident reads itself, so the file's credentials never cross the socket.
 void setUploaderImport(std::function<void(const QJsonObject&,JsonReply)> handler) {uploaderImport_=std::move(handler);}
 void setDiagnosticsBundles(DiagnosticsBundles* bundles) {bundles_=bundles;}
 // The error log (#1753): errors.* methods, and every user-facing error a session
 // method answers is recorded there, its entry's ID in details.errorLogId.
 void setErrorLog(ErrorLog* log) {errorLog_=log;}
 void setUpdateService(UpdateService* updates) {updates_=updates;}
 void setUiDiagnostics(std::function<QJsonObject()> callback) {uiDiagnostics_=std::move(callback);}
 void setUiQuit(std::function<void()> callback) {uiQuit_=std::move(callback);}
 void setUiViewState(std::function<QJsonObject()> callback) {uiViewState_=std::move(callback);}
 static QString defaultRuntimeDirectory();
 static QString defaultSocketPath();
 static QJsonObject response(const QString& requestId,const Error&,const QJsonObject&);
 // How long a client waits for this method's reply.
 static int replyDeadlineMs(const QString& method);
 void dispatch(const QJsonObject&,JsonReply);
 // #1962: the secret this resident hands its own putgd-ui on the private control pipe. A Copy text request
 // that carries it ("person") is the person's; any other, from putgd-cli, the MCP helper or another client,
 // is an agent's, and its copy withholds a code that holds a secret. Never part of a reply.
 QString personToken() const {return personToken_;}
 void setUiActivation(std::function<void(const QString&,std::function<void(Error)>)> callback) {uiActivation_=std::move(callback); }
 // A command for an open interface view; the helper validates its semantics.
 void setUiCommand(std::function<void(const QString&,const QJsonObject&,JsonReply)> callback) {uiCommand_=std::move(callback);}
 // The resident's "diagnostics saved" notification (with Show in folder) and
 // Show in folder itself (#122).
 void setDiagnosticsNotifications(std::function<void(const QJsonObject&)> saved,std::function<Error(const QString&)> reveal) {diagnosticsSaved_=std::move(saved);revealPackage_=std::move(reveal);}
 // history.openLink: the link a notification opens, by the same host opener (#326).
 void setLinkOpener(std::function<void(const QUrl&,std::function<void(const QJsonObject&)>)> open) {openLink_=std::move(open);}
private:
 void accept();
 // Records a session method's user-facing error in the error log, or resolves the
 // entry an earlier failure of the same call left open.
 void noteOutcome(const QString& method,const QJsonObject& params,Error& error);
 void actOnError(const QJsonObject& entry,const QString& action,JsonReply);
 std::function<void(const QString&,std::function<void(Error)>)> uiActivation_;
 std::function<void(const QString&,const QJsonObject&,JsonReply)> uiCommand_;
 std::function<void(const QJsonObject&,JsonReply)> uploaderImport_;
 std::function<QJsonObject()> uiDiagnostics_,uiViewState_;
 std::function<void()> uiQuit_;
 std::function<void(const QJsonObject&)> diagnosticsSaved_;
 std::function<Error(const QString&)> revealPackage_;
 std::function<void(const QUrl&,std::function<void(const QJsonObject&)>)> openLink_;
 QPointer<SetupService> setup_;
 QPointer<DestinationStatus> destinationStatus_;
 QPointer<DiagnosticsBundles> bundles_;
 QPointer<UpdateService> updates_;
 QPointer<ErrorLog> errorLog_;
 const QString residentEpoch_;
 const QString personToken_;
 SettingsService& settings_;
 OperationService& operations_;
 HistoryService& history_;
 LocalIpcServer server_;
 QSet<LocalIpcConnection*> clients_;
 bool fromPerson(const QJsonObject& params) const;
};
}
