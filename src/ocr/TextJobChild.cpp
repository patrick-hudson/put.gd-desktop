#include "TextJobChild.h"
#include "core/BuildInfo.h"
#include "ipc/StdioDevice.h"
#include "ipc/UiControl.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QTimer>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace putgd::textjob {
namespace {
// The sealed input, mapped read-only for the life of the job.
struct Input {
 void* map=MAP_FAILED;size_t bytes=0;
 ~Input(){if(map!=MAP_FAILED)::munmap(map,bytes);}
 bool open(const InputHeader& expected,InputImage& image){
  struct stat status{};const int seals=::fcntl(inputDescriptor,F_GET_SEALS);
  // Only a sealed memfd of exactly the announced size is read.
  if(seals<0||(seals&requiredSeals)!=requiredSeals||::fstat(inputDescriptor,&status)!=0||status.st_size!=inputBytes(expected))return false;
  bytes=size_t(status.st_size);map=::mmap(nullptr,bytes,PROT_READ,MAP_PRIVATE,inputDescriptor,0);
  if(map==MAP_FAILED)return false;
  InputHeader header;
  if(!decodeInputHeader(QByteArray(static_cast<const char*>(map),inputHeaderBytes),status.st_size,header)||header.width!=expected.width||header.height!=expected.height
     ||header.stride!=expected.stride||header.colourStride!=expected.colourStride)return false;
  const auto* base=static_cast<const unsigned char*>(map);
  image={base+inputHeaderBytes,header.width,header.height,header.stride,base+colourOffset(header),header.colourStride,{}};
  // Whole pages inside the rows only: a page shared with a row still to be read stays.
  const auto start=reinterpret_cast<quintptr>(map);const qint64 colour=colourOffset(header),colourStride=header.colourStride,end=qint64(bytes);
  image.releaseColour=[start,colour,colourStride,end](int top,int bottom){
   const qint64 page=::sysconf(_SC_PAGESIZE);if(page<=0||bottom<=top)return;
   const qint64 first=(colour+qint64(top)*colourStride+page-1)/page*page,last=std::min(end,colour+qint64(bottom)*colourStride)/page*page;
   if(last>first)::madvise(reinterpret_cast<void*>(start+quintptr(first)),size_t(last-first),MADV_DONTNEED);
  };
  return true;
 }
};
bool writeOutput(const QByteArray& bytes){
 qint64 written=0;
 while(written<bytes.size()){const auto count=::pwrite(outputDescriptor,bytes.constData()+written,size_t(bytes.size()-written),off_t(written));if(count<0&&errno==EINTR)continue;if(count<=0)return false;written+=count;}
 return true;
}
}
int runChild(int& argc,char** argv,const Engine& engine,const QString& startupFailure){
 QCoreApplication app(argc,argv);
 StdioDevice pipes;if(!pipes.isOpen())return 2;
 UiControl control(pipes);
 const auto build=QJsonDocument::fromJson(buildManifestJson()).object();
 QString nonce;InputHeader header;Options options;bool bootstrapped=false,activated=false,answered=false;
 QObject::connect(&pipes,&QIODevice::readChannelFinished,&app,[&]{app.exit(5);});
 QObject::connect(&control,&UiControl::failed,&app,[&]{app.exit(3);});
 QObject::connect(&control,&UiControl::frame,&app,[&](const QString& type,const QString&,const QJsonObject& payload){
  if(type=="bootstrap"&&!bootstrapped){
   // Another build or protocol: say so and go; the parent reports text_incompatible.
   if(!decodeBootstrap(payload,build,nonce,header,options)){control.send("incompatible");QTimer::singleShot(0,&app,[&]{app.exit(4);});return;}
   bootstrapped=true;control.send("ready",ready(nonce,build));return;
  }
  if(type=="activate"&&bootstrapped&&!activated&&payload.size()==3&&validEnvelope(payload,nonce)){
   activated=true;ChildFailure failure{startupFailure,{}};Input input;InputImage image;TextResult result;QByteArray output;
   if(!failure&&!input.open(header,image))failure={"text_job_io",{}};
   if(!failure)failure=engine(image,options,qEnvironmentVariable(dataPathVariable).split(':',Qt::SkipEmptyParts),result);
   if(!failure){output=encodeResult(std::move(result),options);if(!writeOutput(output))failure={"text_job_io",{}};}
   answered=true;control.send("checkpoint",failure?checkpointFailure(nonce,failure.code,failure.language):checkpoint(nonce,output));return;
  }
  // Only then does the child return normal zero.
  if(type=="checkpointAccepted"&&answered&&payload.size()==3&&validEnvelope(payload,nonce)){app.exit(0);return;}
  app.exit(3);
 });
 return app.exec();
}
}
