// A test build of putgd-ocr's confinement: the helper's own confine() with the
// helper's own settings, optionally a first recognition or a banded one with
// the distribution's Tesseract and OpenMP, then one forbidden action. It prints
// "errno=N" when the action returns, or dies by the seccomp filter (SIGSYS).
//   test_ocr_confinement_probe [--recognize | --bands] none|socket-inet|socket-unix|exec|fork|write PATH|read PATH|vm-read PID
// "none" prints the Landlock ABI, for the per-host record.
#include "ocr/Confinement.h"
#include "ocr/Recognize.h"
#include <QByteArray>
#include <QString>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace putgd::ocr;
namespace {
int report(long result){std::printf("result=%ld errno=%d\n",result,result<0?errno:0);std::fflush(stdout);return 0;}
// Stripes of blocks: enough for Tesseract to find lines and words to try.
QByteArray stripes(int width,int height){
 QByteArray grey(qsizetype(width)*height,char(255));
 for(int y=0;y<height;++y){
  if((y%24)<12)continue;
  for(int x=0;x<width;++x)if(((x/9)%3)!=2&&((x/90)%5)!=4)grey[qsizetype(y)*width+x]=char(20);
 }
 return grey;
}
}
int main(int argc,char** argv){
 int i=1;int recognition=0;
 if(i<argc&&!std::strcmp(argv[i],"--recognize")){recognition=1;++i;}
 else if(i<argc&&!std::strcmp(argv[i],"--bands")){recognition=2;++i;}
 if(i>=argc){std::fprintf(stderr,"usage: test_ocr_confinement_probe [--recognize|--bands] ACTION [ARG]\n");return 2;}
 const QString action=QString::fromLatin1(argv[i]);const char* argument=i+1<argc?argv[i+1]:"";
 if(sanitizedBuild()){std::printf("sanitized\n");return 0;}
 QStringList directories;for(const auto& d:qEnvironmentVariable("PUTGD_TEXT_DATA").split(QLatin1Char(':'),Qt::SkipEmptyParts))directories.append(d);
 Confinement c;c.readable=readableDirectories(directories);c.dataBytes=288ll*1024*1024;c.cpuSeconds=21;c.fileBytes=putgd::textjob::maximumOutputBytes;c.descriptors=32;
 QString failed;int abi=0;
 if(!confine(c,failed,abi)){std::printf("confine failed: %s abi=%d\n",qPrintable(failed),abi);return 3;}
 std::printf("abi=%d\n",abi);std::fflush(stdout);
 if(recognition){
  JobOptions options;options.directories=directories;
  const int width=1200,height=recognition==2?2400:300;
  if(recognition==2){options.limits.singlePassPixels=1000000;options.limits.bandPixels=1000000;options.limits.forcedScale=1;}
  const auto grey=stripes(width,height);
  const auto result=recognizeImage({reinterpret_cast<const uint8_t*>(grey.constData()),width,height,width},options);
  std::printf("recognized error=%s bands=%d words=%d\n",qPrintable(result.error),result.recognition.bands,int(result.recognition.words.size()));std::fflush(stdout);
  if(!result.error.isEmpty())return 4;
 }
 if(action=="none")return report(0);
 if(action=="socket-inet")return report(::socket(AF_INET,SOCK_STREAM,0));
 if(action=="socket-unix")return report(::socket(AF_UNIX,SOCK_STREAM,0));
 if(action=="exec"){char* const args[]={const_cast<char*>("/bin/true"),nullptr};return report(::execve("/bin/true",args,environ));}
 if(action=="fork"){const auto pid=::fork();if(pid==0)::_exit(0);if(pid>0)::waitpid(pid,nullptr,0);return report(pid);}
 if(action=="write")return report(::open(argument,O_WRONLY|O_CREAT|O_CLOEXEC,0600));
 if(action=="read")return report(::open(argument,O_RDONLY|O_CLOEXEC));
 if(action=="vm-read"){char buffer[16];iovec local{buffer,sizeof buffer},remote{reinterpret_cast<void*>(0x400000),sizeof buffer};return report(::process_vm_readv(pid_t(std::atoi(argument)),&local,1,&remote,1,0));}
 std::fprintf(stderr,"unknown action\n");return 2;
}
