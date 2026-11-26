#include "SessionRequest.h"
#include "app/LocalServer.h"
#include <QDeadlineTimer>
#include <QJsonDocument>
#include <QLocalSocket>
#include <cstdio>
namespace putgd::cli {
void output(const QJsonObject& result) {const auto bytes=QJsonDocument(result).toJson(QJsonDocument::Compact)+'\n';std::fwrite(bytes.constData(),1,bytes.size(),stdout);std::fflush(stdout);}
int fail(const QString& id,const QString& code,const QString& message,const QJsonObject& details) {output(LocalServer::response(id,{code,message,details},{}));return 1;}
int sendSessionRequest(const QString& socketPath,const QString& id,const QString& method,const QJsonObject& params) {
 const auto path=socketPath.isEmpty()?LocalServer::defaultSocketPath():socketPath;QLocalSocket socket;socket.connectToServer(path);
 if(!socket.waitForConnected(3000))return fail(id,"session_unavailable","put.gd isn't running. Start put.gd in this user session first.");
 QJsonObject request{{"schemaVersion",1},{"requestId",id},{"session",path},{"method",method},{"params",params}};
 socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact)+'\n');if(!socket.waitForBytesWritten(3000))return fail(id,"transport_failed","Couldn't send the request to put.gd.");
 QDeadlineTimer deadline(LocalServer::replyDeadlineMs(method));
 QByteArray reply;while(!reply.contains('\n')) {if(!socket.waitForReadyRead(int(deadline.remainingTime())))return fail(id,"transport_failed","put.gd didn't return a complete response.");reply+=socket.readAll();if(reply.size()>16*1024*1024)return fail(id,"response_too_large","Response from put.gd exceeds 16 MB.");}
 QJsonParseError parseError;auto response=QJsonDocument::fromJson(reply,&parseError);
 if(parseError.error!=QJsonParseError::NoError||!response.isObject()||response.object()["schemaVersion"]!=1||response.object()["requestId"]!=id||!response.object()["ok"].isBool())return fail(id,"invalid_response","Response from put.gd was invalid or didn't match the request.");
 output(response.object());return response.object()["ok"].toBool()?0:1;
}
}
