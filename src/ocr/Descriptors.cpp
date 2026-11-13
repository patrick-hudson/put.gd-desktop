#include "Descriptors.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace putgd::ocr {
namespace {
constexpr int inputSeals=F_SEAL_WRITE|F_SEAL_SHRINK|F_SEAL_GROW;
uint32_t little(const unsigned char* p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
}
InputImage::~InputImage(){if(base_&&base_!=MAP_FAILED)::munmap(base_,size_);}
QString InputImage::map(int fd,qint64 maximumPixels){
 const QString io=QStringLiteral("text_job_io");
 const int seals=::fcntl(fd,F_GET_SEALS);
 if(seals<0||(seals&inputSeals)!=inputSeals)return io;
 struct stat s{};
 if(::fstat(fd,&s)||!S_ISREG(s.st_mode)||s.st_size<qint64(inputHeaderBytes))return io;
 unsigned char header[inputHeaderBytes];
 if(::pread(fd,header,sizeof header,0)!=qint64(sizeof header)||std::memcmp(header,"PGRY",4)!=0)return io;
 const qint64 width=little(header+4),height=little(header+8),stride=little(header+12);
 if(width<1||height<1||stride<width||stride>width+64)return io;
 if(width*height>maximumPixels)return QStringLiteral("input_excessive");
 if(s.st_size!=qint64(inputHeaderBytes)+stride*height)return io;
 void* mapped=::mmap(nullptr,size_t(s.st_size),PROT_READ,MAP_SHARED,fd,0);
 if(mapped==MAP_FAILED)return io;
 base_=mapped;size_=size_t(s.st_size);
 view_={static_cast<const uint8_t*>(mapped)+inputHeaderBytes,int(width),int(height),int(stride)};
 return {};
}
bool outputReady(int fd){
 const int flags=::fcntl(fd,F_GETFL);
 struct stat s{};
 return flags>=0&&(flags&O_ACCMODE)!=O_RDONLY&&::fcntl(fd,F_GET_SEALS)>=0&&::fstat(fd,&s)==0&&S_ISREG(s.st_mode)&&s.st_size==0;
}
bool writeOutput(int fd,const QByteArray& bytes){
 qsizetype done=0;
 while(done<bytes.size()){
  const auto n=::write(fd,bytes.constData()+done,size_t(bytes.size()-done));
  if(n<0&&errno==EINTR)continue;
  if(n<=0)return false;
  done+=n;
 }
 return true;
}
}
