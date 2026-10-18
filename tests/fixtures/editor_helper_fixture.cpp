// A misbehaving stand-in for `putgd-ui --edit-image`, used only by
// editor_job_integration to prove the resident's failure handling. The product
// helper never reads PUTGD_TEST_EDITOR_MODE; EditorJob selects this executable
// through its constructor.
#include "app/UiControl.h"
#include "ui/StdioDevice.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QImageWriter>
#include <QJsonArray>
#include <QTimer>
#include <cstdlib>
#include <sys/resource.h>

using namespace putgd;
int main(int argc,char** argv) {
 const rlimit noCore{0,0};::setrlimit(RLIMIT_CORE,&noCore);
 QCoreApplication app(argc,argv);
 StdioDevice pipes;if(!pipes.isOpen())return 2;
 UiControl control(pipes);
 const auto mode=qEnvironmentVariable("PUTGD_TEST_EDITOR_MODE");
 QJsonObject request,envelope;
 QObject::connect(&pipes,&QIODevice::readChannelFinished,&app,[&]{app.exit(5);});
 QObject::connect(&control,&UiControl::failed,&app,[&]{app.exit(3);});
 QObject::connect(&control,&UiControl::frame,&app,[&](const QString& type,const QString&,const QJsonObject& payload){
  if(type=="bootstrap"){
   request=payload;envelope={{"role",payload["role"]},{"nonce",payload["nonce"]},{"generation",payload["generation"]}};
   if(mode=="silent")return;
   auto ready=envelope;ready["build"]=payload["build"];control.send("ready",ready);
  }else if(type=="activate"){
   control.send("editing",envelope);
   if(mode=="crash"){QTimer::singleShot(200,&app,[]{std::abort();});return;}
   control.send("finalizing",envelope);
   if(mode=="hang-finalizing")return;
   // Write an export the resident must reject: not a PNG, or a digest mismatch.
   QByteArray bytes("not a png at all");
   if(mode=="wrong-digest"){QImage image(4,4,QImage::Format_RGB32);image.fill(Qt::red);QBuffer buffer(&bytes);bytes.clear();buffer.open(QIODevice::WriteOnly);QImageWriter(&buffer,"png").write(image);}
   QFile output(request["output"].toObject()["path"].toString());
   if(!output.open(QIODevice::WriteOnly|QIODevice::Truncate)||output.write(bytes)!=bytes.size()||!output.flush()){app.exit(4);return;}
   auto digest=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);
   if(mode=="wrong-digest")digest=QCryptographicHash::hash("different bytes",QCryptographicHash::Sha256);
   auto result=envelope;result["ok"]=true;result["bytes"]=QString::number(bytes.size());result["sha256"]=QString::fromLatin1(digest.toHex());
   result["width"]=4;result["height"]=4;result["recentColors"]=QJsonArray{};control.send("checkpoint",result);
  }else if(type=="checkpointAccepted"){
   QTimer::singleShot(0,&app,&QCoreApplication::quit);
  }
 });
 return app.exec();
}
