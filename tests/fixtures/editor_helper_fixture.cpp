// A misbehaving stand-in for `putgd-ui --edit-image`, used only by
// editor_job_integration to prove the resident's failure handling. The product
// helper never reads PUTGD_TEST_EDITOR_MODE; EditorJob selects this executable
// through its constructor.
#include "ipc/UiControl.h"
#include "ipc/StdioDevice.h"
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
namespace {
// A sanitized 4x4 grey PNG (IHDR, IDAT, IEND only) built by hand: the handoff accepts it.
QByteArray sanitizedPng(){
 const auto crc=[](const QByteArray& bytes){quint32 c=0xffffffffU;for(const auto byte:bytes){c^=quint8(byte);for(int k=0;k<8;++k)c=(c>>1)^(0xedb88320U&(0U-(c&1U)));}return c^0xffffffffU;};
 const auto be=[](quint32 v){QByteArray b(4,0);for(int i=0;i<4;++i)b[i]=char(v>>(24-8*i));return b;};
 const auto chunk=[&](const QByteArray& type,const QByteArray& data){return be(quint32(data.size()))+type+data+be(crc(type+data));};
 QByteArray rows;for(int y=0;y<4;++y)rows+=QByteArray(1,0)+QByteArray(4,char(0x80));
 const auto header=be(4)+be(4)+QByteArray("\x08\x00\x00\x00\x00",5);
 return QByteArray("\x89PNG\r\n\x1a\n",8)+chunk("IHDR",header)+chunk("IDAT",qCompress(rows).mid(4))+chunk("IEND",{});
}
}
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
   QByteArray bytes=mode=="linger"?sanitizedPng():QByteArray("not a png at all");
   if(mode=="wrong-digest"){QImage image(4,4,QImage::Format_RGB32);image.fill(Qt::red);QBuffer buffer(&bytes);bytes.clear();buffer.open(QIODevice::WriteOnly);QImageWriter(&buffer,"png").write(image);}
   QFile output(request["output"].toObject()["path"].toString());
   if(!output.open(QIODevice::WriteOnly|QIODevice::Truncate)||output.write(bytes)!=bytes.size()||!output.flush()){app.exit(4);return;}
   auto digest=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);
   if(mode=="wrong-digest")digest=QCryptographicHash::hash("different bytes",QCryptographicHash::Sha256);
   auto result=envelope;result["ok"]=true;result["bytes"]=QString::number(bytes.size());result["sha256"]=QString::fromLatin1(digest.toHex());
   result["width"]=4;result["height"]=4;result["recentColors"]=QJsonArray{};control.send("checkpoint",result);
  }else if(type=="checkpointAccepted"){
   // #2011: the image was handed off, then the helper doesn't close.
   if(mode=="linger")return;
   QTimer::singleShot(0,&app,&QCoreApplication::quit);
  }
 });
 return app.exec();
}
