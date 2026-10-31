#include "platform/x11/CaptureControl.h"
#include "platform/x11/CaptureArtifactLease.h"
#include "ui/StdioDevice.h"
#include "core/BuildInfo.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFile>
#include <QImage>
#include <QCryptographicHash>
#include <QTimer>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits>
namespace {
bool cursorId(const QJsonValue& value){
 bool ok=false;const auto id=value.toString().toULongLong(&ok);
 return value.isString()&&ok&&id<=std::numeric_limits<quint32>::max()&&QString::number(id)==value.toString();
}
QJsonObject cursorSeed(const QString& mode){
 // Synthetic network-order ARGB: colored and translucent, with an asymmetric hotspot.
 auto seed=QJsonObject{{"width",2},{"height",2},{"xhot",1},{"yhot",0},
                      {"pixels",QString::fromLatin1(QByteArray::fromHex("ffff00008000800040000040ff0000ff").toBase64())}};
 if(mode=="cursor-id")return {{"id","123"}};
 if(mode=="cursor-id-negative")return {{"id",-1}};
 if(mode=="cursor-id-overflow")return {{"id","4294967296"}};
 if(mode=="cursor-extra")seed["extra"]=true;
 if(mode=="cursor-zero-width")seed["width"]=0;
 if(mode=="cursor-wide")seed["width"]=65;
 if(mode=="cursor-negative-height")seed["height"]=-1;
 if(mode=="cursor-fraction")seed["width"]=1.5;
 if(mode=="cursor-string-coordinate")seed["xhot"]="1";
 if(mode=="cursor-xhot-outside")seed["xhot"]=2;
 if(mode=="cursor-yhot-outside")seed["yhot"]=2;
 if(mode=="cursor-short-pixels")seed["pixels"]="AAAAAA==";
 if(mode=="cursor-invalid-base64")seed["pixels"]="!!!!!!!!!!!!!!!!!!!!!!!!";
 if(mode=="cursor-noncanonical-base64"){auto text=seed["pixels"].toString();text[text.size()-3]=QChar('x');seed["pixels"]=text;}
 if(mode=="cursor-long-pixels")seed["pixels"]=QString(21849,'A');
 if(mode=="cursor-nonlatin-pixels")seed["pixels"]=QString(24,QChar(0x0100));
 if(mode=="cursor-pixels-type")seed["pixels"]=QJsonArray{};
 return seed;
}
bool traceCursor(const QString& phase,const QJsonObject& bootstrap,const QJsonObject& frame){
 const auto path=qEnvironmentVariable("PUTGD_TEST_CAPTURE_TRACE");if(path.isEmpty())return true;
 // The test precreates this private, owned file. Never record nonce, payload or artifact path.
 const int fd=::open(QFile::encodeName(path).constData(),O_WRONLY|O_APPEND|O_CLOEXEC|O_NOFOLLOW);
 struct stat st{};
 if(fd<0)return false;
 if(::fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&0777)!=0600||st.st_nlink!=1||st.st_size>65536){::close(fd);return false;}
 QFile file;if(!file.open(fd,QIODevice::WriteOnly,QFileDevice::AutoCloseHandle)){::close(fd);return false;}
 const auto cursor=bootstrap["cursor"].toObject();
 const auto record=QJsonObject{{"phase",phase},{"appearance",bootstrap["appearance"]},{"outcome",bootstrap["outcome"]},{"action",bootstrap["target"].toObject()["action"]},
                              {"generation",bootstrap["generation"]},{"cache",cursor["cache"]},
                              {"cursorId",phase=="bootstrap"?cursor["id"]:frame["cursor"]}};
 const auto bytes=QJsonDocument(record).toJson(QJsonDocument::Compact)+'\n';
 return file.write(bytes)==bytes.size()&&file.flush();
}
}
int main(int argc,char** argv){
 putgd::UiControlPipes handles;if(!handles.valid())return 2;QCoreApplication app(argc,argv);putgd::StdioDevice pipe(handles.input(),handles.output());putgd::CaptureControl control(pipe);putgd::CaptureArtifactLease artifact;QJsonObject identity,bootstrap;QString previewCursor;bool bootstrapped=false,previewed=false,begun=false,terminal=false;const auto mode=qEnvironmentVariable("PUTGD_TEST_CAPTURE_MODE");
 QObject::connect(&control,&putgd::CaptureControl::frame,&app,[&](const QString& type,const QJsonObject& payload){
  if(type=="bootstrap"){
   const auto cursor=payload["cursor"].toObject();
   if(payload.size()!=((payload.contains("outcome")?10:9)+int(payload.contains("appearance")))||cursor.size()!=2||!cursorId(cursor["id"])||!cursor["cache"].isBool()||(payload.contains("outcome")&&(!payload["outcome"].isString()||payload["outcome"].toString().size()>256))){app.exit(4);return;}
   if(!artifact.acquire(payload["path"].toString(),payload["device"].toString(),payload["inode"].toString())){app.exit(5);return;}
   if(bootstrapped){app.exit(4);return;}bootstrapped=true;bootstrap=payload;
   if(!traceCursor("bootstrap",bootstrap,payload)){app.exit(5);return;}
   identity={{"nonce",payload["nonce"]},{"generation",payload["generation"]}};auto ready=identity;ready["cursor"]=QJsonObject{};
   if(mode=="cursor-seed"){if(cursor["cache"].toBool()&&cursor["id"]=="0")ready["cursor"]=cursorSeed(mode);}
   else if(mode.startsWith("cursor-")&&mode!="cursor-empty")ready["cursor"]=cursorSeed(mode);
   if(mode=="cursor-ready-type")ready["cursor"]="123";
   ready["build"]=QJsonDocument::fromJson(putgd::buildManifestJson()).object();if(mode=="wrong-build")ready["build"]=QJsonObject{};
   control.send("ready",ready);
   if(mode=="early-exit"){app.quit();return;}
   if(mode=="prebegin-result"){auto result=identity;result["bytes"]="45";result["sha256"]=QString(64,'0');result["width"]=1;result["height"]=1;control.send("result",result);}
   return;
  }
  if(!bootstrapped||payload.size()!=((type=="begin"||type=="preview")?3:2)||payload["nonce"]!=identity["nonce"]||payload["generation"]!=identity["generation"]){control.stop();app.exit(4);return;}
  if(type=="preview"){
   if(previewed||begun||terminal||bootstrap["target"].toObject()["action"]!="region"||!cursorId(payload["cursor"])){control.stop();app.exit(4);return;}
   QFile output(bootstrap["path"].toString());if(output.size()!=0){control.stop();app.exit(5);return;}
   previewed=true;previewCursor=payload["cursor"].toString();
   // Existing cache tests keep their bootstrap/begin trace contract. Preview
   // evidence is opt-in and contains no image, nonce or artifact path.
   if(mode.startsWith("preview-")&&!traceCursor("preview",bootstrap,payload)){control.stop();app.exit(5);return;}
   if(mode=="preview-cancel"||mode=="preview-error"){
    terminal=true;auto reply=identity;reply["code"]=mode=="preview-cancel"?"cancelled":"capture_geometry";
    reply["reason"]=mode=="preview-cancel"?QString{}:QString("Synthetic selection outside the layout");reply["details"]=mode=="preview-cancel"?QJsonObject{}:QJsonObject{{"stage","select"},{"check","fixture_selection"},{"selection",QJsonArray{0,1080,100,100}}};
    control.send(mode=="preview-cancel"?"cancelled":"error",reply);
    // Keep the child alive briefly so tests distinguish early notification from
    // final process completion. The controller must retain the ticket's error.
    QTimer::singleShot(200,&app,&QCoreApplication::quit);
   }
   return;
  }
  if(type=="begin"){
   const bool region=bootstrap["target"].toObject()["action"]=="region";
   if(begun||terminal||!cursorId(payload["cursor"])||(region&&(!previewed||payload["cursor"]!=previewCursor))){control.stop();app.exit(4);return;}
   if(!traceCursor("begin",bootstrap,payload)){app.exit(5);return;}
   begun=true;if(mode=="waiting")return;
   QFile output(bootstrap["path"].toString());if(!output.open(QIODevice::WriteOnly)){app.exit(5);return;}const int side=mode=="large"?700:3;QImage image(side,mode=="large"?700:2,QImage::Format_ARGB32);image.fill(Qt::blue);image.save(&output,"PNG");output.close();if(!output.open(QIODevice::ReadOnly)){app.exit(5);return;}const auto bytes=output.readAll();output.close();auto result=identity;result["bytes"]=QString::number(bytes.size());result["sha256"]=QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());result["width"]=mode=="wrong-dimension"?1000000000:image.width();result["height"]=image.height();if(mode=="wrong-hash")result["sha256"]=QString(64,'0');control.send("result",result);if(mode=="duplicate")control.send("result",result);return;
  }
  if(type=="accept"&&begun){app.quit();return;}
  if(type=="cancel"){auto reply=identity;reply["code"]="cancelled";reply["reason"]="";reply["details"]=QJsonObject{};control.send("cancelled",reply);app.quit();return;}
 });QObject::connect(&pipe,&QIODevice::readChannelFinished,&app,[&]{app.exit(3);});QTimer::singleShot(15000,&app,[&]{app.exit(9);});return app.exec();
}
