#include "platform/x11/CaptureControl.h"
#include "platform/x11/CaptureArtifactLease.h"
#include "ui/StdioDevice.h"
#include "core/BuildInfo.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QFile>
#include <QImage>
#include <QCryptographicHash>
#include <QTimer>
#include <fcntl.h>
#include <unistd.h>
int main(int argc,char** argv){
 putgd::UiControlPipes handles;if(!handles.valid())return 2;QCoreApplication app(argc,argv);putgd::StdioDevice pipe(handles.input(),handles.output());putgd::CaptureControl control(pipe);putgd::CaptureArtifactLease artifact;QJsonObject identity;const auto mode=qEnvironmentVariable("PUTGD_TEST_CAPTURE_MODE");
 QObject::connect(&control,&putgd::CaptureControl::frame,&app,[&](const QString& type,const QJsonObject& payload){
  if(type=="bootstrap"){
   if(!artifact.acquire(payload["path"].toString(),payload["device"].toString(),payload["inode"].toString())){app.exit(5);return;}
   identity={{"nonce",payload["nonce"]},{"generation",payload["generation"]}};auto ready=identity;ready["build"]=QJsonDocument::fromJson(putgd::buildManifestJson()).object();if(mode=="wrong-build")ready["build"]=QJsonObject{};control.send("ready",ready);if(mode=="wrong-build"||mode=="waiting")return;if(mode=="early-exit"){app.quit();return;}
   QFile output(payload["path"].toString());if(!output.open(QIODevice::WriteOnly)){app.exit(5);return;}const int side=mode=="large"?700:3;QImage image(side,mode=="large"?700:2,QImage::Format_ARGB32);image.fill(Qt::blue);image.save(&output,"PNG");output.close();if(!output.open(QIODevice::ReadOnly)){app.exit(5);return;}const auto bytes=output.readAll();output.close();auto result=identity;result["bytes"]=QString::number(bytes.size());result["sha256"]=QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());result["width"]=mode=="wrong-dimension"?1000000000:image.width();result["height"]=image.height();if(mode=="wrong-hash")result["sha256"]=QString(64,'0');control.send("result",result);if(mode=="duplicate")control.send("result",result);return;
  }
  if(type=="accept"){app.quit();return;}
  if(type=="cancel"){auto reply=identity;reply["code"]="cancelled";control.send("cancelled",reply);app.quit();return;}
 });QObject::connect(&pipe,&QIODevice::readChannelFinished,&app,[&]{app.exit(3);});QTimer::singleShot(15000,&app,[&]{app.exit(9);});return app.exec();
}
