#include <cstring>
#include "app/LocalServer.h"
#include "core/BuildInfo.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QUuid>
#include <cstdio>
int main(int argc,char** argv) {
 if(argc==2&&std::strcmp(argv[1],"--version-json")==0){
  std::printf("%s\n",putgd::buildManifestJson());return 0;
 }
 QCoreApplication app(argc,argv);QCoreApplication::setOrganizationName("putgd");QCoreApplication::setApplicationName("putgd-cli");QCoreApplication::setApplicationVersion(putgd::buildVersion());
 QCommandLineParser parser;parser.setApplicationDescription("JSON client for the resident put.gd session");parser.addHelpOption();parser.addVersionOption();parser.addOption({"socket","Session socket path","path"});parser.addOption({"request-id","Correlation ID","id"});parser.addPositionalArgument("method","Versioned session method");parser.addPositionalArgument("params","JSON parameter object (default {})","[params]");parser.process(app);
 const auto args=parser.positionalArguments();const auto id=parser.isSet("request-id")?parser.value("request-id"):QUuid::createUuid().toString(QUuid::WithoutBraces);
 auto output=[](const QJsonObject& result) {const auto bytes=QJsonDocument(result).toJson(QJsonDocument::Compact)+'\n';std::fwrite(bytes.constData(),1,bytes.size(),stdout);};
 auto error=[&](QString code,QString message) {output(putgd::LocalServer::response(id,{code,message,{}},{}));return 1;};
 if(args.size()<1||args.size()>2)return error("invalid_request","Supply a method and optional JSON parameter object");
 if(args[0]=="setup.storeCredentials")return error("credentials_require_settings","Enter credentials through Settings.");
 const auto bytes=args.size()==2?args[1].toUtf8():QByteArray("{}");if(bytes.size()>1024*1024)return error("request_too_large","Request exceeds 1 MiB");
 QJsonParseError parseError;auto params=QJsonDocument::fromJson(bytes,&parseError);if(parseError.error!=QJsonParseError::NoError||!params.isObject())return error("invalid_json","Parameters must be a JSON object");
 const auto path=parser.isSet("socket")?parser.value("socket"):putgd::LocalServer::defaultSocketPath();QLocalSocket socket;socket.connectToServer(path);
 if(!socket.waitForConnected(3000))return error("session_unavailable","Start the resident put.gd app in this user session");
 QJsonObject request{{"schemaVersion",1},{"requestId",id},{"session",path},{"method",args[0]},{"params",params.object()}};
 socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact)+'\n');if(!socket.waitForBytesWritten(3000))return error("transport_failed","Cannot send request to resident session");
 QByteArray reply;while(!reply.contains('\n')) {if(!socket.waitForReadyRead(15000))return error("transport_failed","Resident session did not return a bounded response");reply+=socket.readAll();if(reply.size()>16*1024*1024)return error("response_too_large","Resident response exceeds 16 MiB");}
 auto response=QJsonDocument::fromJson(reply,&parseError);if(parseError.error!=QJsonParseError::NoError||!response.isObject()||response.object()["schemaVersion"]!=1||response.object()["requestId"]!=id||!response.object()["ok"].isBool())return error("invalid_response","Resident response does not match request schema and ID");output(response.object());return response.object()["ok"].toBool()?0:1;
}
