#include "app/LocalIpc.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <csignal>
#include <cstdio>
// A resident left behind by an ended login (#490), as its session socket shows
// it: it answers every request, and after ui.quit it exits (quit), stays until
// SIGTERM (stay), or ignores SIGTERM too, so only SIGKILL ends it (stubborn).
int main(int argc,char* argv[]) {
 QCoreApplication app(argc,argv);
 if(argc!=3)return 2;
 const QString path=QString::fromLocal8Bit(argv[1]),mode=QString::fromLatin1(argv[2]);
 if(mode=="stubborn")std::signal(SIGTERM,SIG_IGN);
 putgd::LocalIpcServer server;
 if(!server.listen(path))return 3;
 QObject::connect(&server,&putgd::LocalIpcServer::newConnection,&app,[&]{
  while(auto* connection=server.nextPendingConnection()){
   QObject::connect(connection,&putgd::LocalIpcConnection::disconnected,connection,&QObject::deleteLater);
   QObject::connect(connection,&putgd::LocalIpcConnection::readyRead,connection,[&app,connection,mode]{
    const auto request=QJsonDocument::fromJson(connection->readAll()).object();
    connection->stopReading();
    const QJsonObject error{{"code",""},{"message",""},{"details",QJsonObject{}}};
    const QJsonObject response{{"schemaVersion",1},{"requestId",request["requestId"]},{"ok",true},{"result",QJsonObject{{"quitting",true}}},{"error",error}};
    connection->sendAndClose(QJsonDocument(response).toJson(QJsonDocument::Compact)+'\n');
    if(request["method"]=="ui.quit"&&mode=="quit")QTimer::singleShot(0,&app,&QCoreApplication::quit);
   });
  }
 });
 std::printf("ready\n");std::fflush(stdout);
 return app.exec();
}
