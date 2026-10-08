#pragma once
#include "HistoryService.h"
#include "LocalIpc.h"
#include <QSet>
namespace putgd {
class LocalServer final:public QObject {
 Q_OBJECT
public:
 LocalServer(SettingsService&,OperationService&,HistoryService&,QObject* parent=nullptr);
 Result listen(const QString& runtimeDirectory=QString{});
 QString socketPath() const {return server_.fullServerName();}
 static QString defaultRuntimeDirectory();
 static QString defaultSocketPath();
 static QJsonObject response(const QString& requestId,const Error&,const QJsonObject&);
 void dispatch(const QJsonObject&,JsonReply);
 void setUiActivation(std::function<void(const QString&)> callback) {uiActivation_=std::move(callback); }
private:
 void accept();
 std::function<void(const QString&)> uiActivation_;
 SettingsService& settings_;
 OperationService& operations_;
 HistoryService& history_;
 LocalIpcServer server_;
 QSet<LocalIpcConnection*> clients_;
};
}
