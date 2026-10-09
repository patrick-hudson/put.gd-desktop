#include "platform/x11/X11SourceAdapter.h"
#include "platform/x11/X11NativeSnapshot.h"
#include "platform/x11/CaptureControl.h"
#include "platform/x11/CaptureArtifactLease.h"
#include "ui/StdioDevice.h"
#include "core/BuildInfo.h"
#include <QApplication>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QTimer>
#include <QThread>
#include <QFile>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QUuid>
#include <memory>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace {
// PNG writer may fail before its output exceeds the accepted artifact budget.
class BoundedFile final:public QFile {
public:
 explicit BoundedFile(qint64 bound):bound_(bound){}
 bool exceeded()const{return exceeded_;}
protected:
 qint64 writeData(const char* bytes,qint64 count)override{
  if(count<0||pos()>bound_||count>bound_-pos()){exceeded_=true;return -1;}
  return QFile::writeData(bytes,count);
 }
private:qint64 bound_;bool exceeded_=false;
};
bool identity(const struct stat& s,const QString& device,const QString& inode){return S_ISREG(s.st_mode)&&s.st_uid==getuid()&&(s.st_mode&0777)==0600&&s.st_nlink==1&&QString::number(qulonglong(s.st_dev))==device&&QString::number(qulonglong(s.st_ino))==inode;}
bool positive(const QJsonValue& v,qint64 maximum){return v.isDouble()&&v.toInteger()>0&&v.toInteger()<=maximum&&v.toDouble()==v.toInteger();}
}
int main(int argc,char** argv){
 if(argc==2&&(std::strcmp(argv[1],"--version")==0||std::strcmp(argv[1],"-v")==0)){std::printf("putgd-capture %s\n",putgd::buildVersion());return 0;}
 putgd::UiControlPipes handles;if(!handles.valid())return 2;
 QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);QCoreApplication::setOrganizationName("putgd");QCoreApplication::setApplicationName("putgd-capture");QCoreApplication::setApplicationVersion(putgd::buildVersion());
 putgd::StdioDevice pipes(handles.input(),handles.output());if(!pipes.isOpen())return 2;putgd::CaptureControl control(pipes);
 putgd::X11NativeSnapshot native;putgd::X11Capture resolver;putgd::X11SourceAdapter source;
 const auto build=QJsonDocument::fromJson(putgd::buildManifestJson()).object();QString nonce,generation,path,device,inode;QJsonObject frozen;putgd::ResourceLimits limits;
 putgd::CaptureArtifactLease artifact;
 bool bootstrapped=false,ready=false,settled=false,resultSent=false,cancelled=false;QThread* encoder=nullptr;
 QTimer deadline;deadline.setSingleShot(true);deadline.start(10000);
 const auto envelope=[&]{return QJsonObject{{"nonce",nonce},{"generation",generation}};};
 const auto finish=[&](const QString& code){if(settled)return;settled=true;deadline.stop();auto message=envelope();message["code"]=code;control.send(code=="cancelled"?"cancelled":"error",message);QTimer::singleShot(0,&app,&QCoreApplication::quit);};
 QObject::connect(&deadline,&QTimer::timeout,&app,[&]{if(ready){finish("capture_timeout");source.cancelAcquisition();}else app.exit(3);});
 QObject::connect(&pipes,&QIODevice::readChannelFinished,&app,[&]{cancelled=true;source.cancelAcquisition();app.exit(3);});
 QObject::connect(&control,&putgd::CaptureControl::failed,&app,[&]{cancelled=true;source.cancelAcquisition();app.exit(3);});
 QObject::connect(&control,&putgd::CaptureControl::frame,&app,[&](const QString& type,const QJsonObject& payload){
  if(type=="bootstrap"){
   if(bootstrapped||payload.size()!=8||payload["build"].toObject()!=build||!payload["nonce"].isString()||QUuid(payload["nonce"].toString()).isNull()||!payload["generation"].isString()||!payload["path"].isString()||!payload["device"].isString()||!payload["inode"].isString()||!payload["limits"].isObject()){app.exit(4);return;}
   nonce=payload["nonce"].toString();generation=payload["generation"].toString();bool ok=false;const auto g=generation.toULongLong(&ok);if(!ok||!g||QString::number(g)!=generation){app.exit(4);return;}
   path=payload["path"].toString();device=payload["device"].toString();inode=payload["inode"].toString();frozen=payload["target"].toObject();const auto l=payload["limits"].toObject();
   if(l.size()!=3||!positive(l["imagePixels"],1000000000LL)||!positive(l["imageBytes"],1099511627776LL)||!positive(l["maxFileBytes"],1099511627776LL)||!path.startsWith("/tmp/putgd-capture-")||!path.endsWith("/result.png")||path.contains("/../")||path.count('/')!=3){app.exit(4);return;}
   limits.imagePixels=l["imagePixels"].toInteger();limits.imageBytes=l["imageBytes"].toInteger();limits.maxFileBytes=l["maxFileBytes"].toInteger();
   const auto parent=QFile::encodeName(path.left(path.lastIndexOf('/')));struct stat directory{};if(::lstat(parent.constData(),&directory)||!S_ISDIR(directory.st_mode)||directory.st_uid!=getuid()||(directory.st_mode&0777)!=0700){app.exit(4);return;}
   const int fd=::open(QFile::encodeName(path).constData(),O_WRONLY|O_CLOEXEC|O_NOFOLLOW);struct stat s{};const bool valid=fd>=0&&!::fstat(fd,&s)&&identity(s,device,inode)&&s.st_size==0;if(fd>=0)::close(fd);if(!valid||!artifact.acquire(path,device,inode)){app.exit(4);return;}
   bootstrapped=true;putgd::Error error;const auto target=native.resolve(frozen,resolver,error);ready=true;auto reply=envelope();reply["build"]=build;control.send("ready",reply);
   if(error){finish(error.code);return;}deadline.start(120000);
   // The passed target is the invocation target; never rediscover focused window.
   source.acquireFrozen(target,limits,[&](putgd::PayloadResult result){
    if(settled||cancelled)return;
    if(result.error){finish(result.error.code);return;}
    putgd::Error changed;if(!native.unchanged(frozen,changed)){finish(changed.code);return;}
    struct Encoded {QString hash; qint64 bytes=0; int width=0,height=0;putgd::Error error;};auto encoded=std::make_shared<Encoded>();
    const auto output=path,dev=device,ino=inode;const auto acceptedLimits=limits;
    encoder=QThread::create([encoded,image=std::move(result.payload.image),output,dev,ino,acceptedLimits]()mutable{
     auto fail=[&](const char* code){encoded->error={QString::fromLatin1(code),"Capture encoding failed",{}};};
     const qint64 pixels=qint64(image.width())*image.height();if(image.isNull()||pixels>acceptedLimits.imagePixels||image.sizeInBytes()>acceptedLimits.imageBytes){fail("input_excessive");return;}encoded->width=image.width();encoded->height=image.height();
     const int fd=::open(QFile::encodeName(output).constData(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);struct stat s{};if(fd<0||::fstat(fd,&s)||!identity(s,dev,ino)||s.st_size!=0){if(fd>=0)::close(fd);fail("capture_io");return;}
     BoundedFile file(std::min(acceptedLimits.maxFileBytes,pixels*4+pixels*4/1000+encoded->height+65536));if(!file.open(fd,QIODevice::ReadWrite,QFileDevice::AutoCloseHandle)){::close(fd);fail("capture_io");return;}
     if(!image.save(&file,"PNG")||!file.flush()){fail(file.exceeded()?"input_excessive":"encoding_failed");return;}image={};encoded->bytes=file.size();if(encoded->bytes<45||encoded->bytes>acceptedLimits.maxFileBytes||encoded->bytes>pixels*4+pixels*4/1000+encoded->height+65536){fail("input_excessive");return;}file.seek(0);QCryptographicHash hash(QCryptographicHash::Sha256);if(!hash.addData(&file)){fail("capture_io");return;}encoded->hash=QString::fromLatin1(hash.result().toHex());
    });
    auto* worker=encoder;QObject::connect(worker,&QThread::finished,&app,[&,worker,encoded]{encoder=nullptr;worker->deleteLater();if(settled||cancelled)return;if(encoded->error){finish(encoded->error.code);return;}putgd::Error changed;if(!native.unchanged(frozen,changed)){finish(changed.code);return;}resultSent=true;auto reply=envelope();reply["bytes"]=QString::number(encoded->bytes);reply["sha256"]=encoded->hash;reply["width"]=encoded->width;reply["height"]=encoded->height;control.send("result",reply);deadline.start(10000);});worker->start();
   });return;
  }
  if(!ready||payload.size()!=2||payload["nonce"]!=nonce||payload["generation"]!=generation){app.exit(4);return;}
  if(type=="cancel"){cancelled=true;source.cancelAcquisition();finish("cancelled");return;}
  if(type=="accept"&&resultSent&&!settled){settled=true;deadline.stop();app.quit();return;}
  app.exit(4);
 });
 const int status=app.exec();if(encoder){encoder->disconnect(&app);encoder->wait();delete encoder;}artifact.cleanup();return status;
}
