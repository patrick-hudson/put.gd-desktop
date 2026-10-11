#pragma once
#include <QFile>
#include <QString>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace putgd {
// Pins the verified private directory. Cleanup never traverses symlinks and
// preserves a replacement file/directory or any additional directory contents.
// Call cleanup only after the encoder has finished using its output descriptor.
class CaptureArtifactLease final {
public:
 ~CaptureArtifactLease(){cleanup();}
 CaptureArtifactLease()=default;
 CaptureArtifactLease(const CaptureArtifactLease&)=delete;
 CaptureArtifactLease& operator=(const CaptureArtifactLease&)=delete;
 bool acquire(const QString& path,const QString& device,const QString& inode){
  if(directoryFd_>=0||!path.startsWith("/tmp/putgd-capture-")||!path.endsWith("/result.png")||path.count('/')!=3)return false;
  leaf_=QFile::encodeName(path.mid(5,path.size()-5-11));
  parentFd_=::open("/tmp",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
  if(parentFd_<0)return false;
  struct stat named{};
  if(::fstatat(parentFd_,leaf_.constData(),&named,AT_SYMLINK_NOFOLLOW)||!privateDirectory(named))return false;
  directoryFd_=::openat(parentFd_,leaf_.constData(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
  if(directoryFd_<0||::fstat(directoryFd_,&directory_)||!sameDirectory(named,directory_))return false;
  struct stat output{};
  if(::fstatat(directoryFd_,"result.png",&output,AT_SYMLINK_NOFOLLOW)||!privateFile(output)||QString::number(qulonglong(output.st_dev))!=device||QString::number(qulonglong(output.st_ino))!=inode||output.st_size!=0)return false;
  file_=output;verified_=true;return true;
 }
 void cleanup(){
  if(verified_){
   struct stat named{},output{};
   if(!::fstatat(parentFd_,leaf_.constData(),&named,AT_SYMLINK_NOFOLLOW)&&sameDirectory(named,directory_)&&
      !::fstatat(directoryFd_,"result.png",&output,AT_SYMLINK_NOFOLLOW)&&privateFile(output)&&output.st_dev==file_.st_dev&&output.st_ino==file_.st_ino){
    if(!::unlinkat(directoryFd_,"result.png",0)&&!::fstatat(parentFd_,leaf_.constData(),&named,AT_SYMLINK_NOFOLLOW)&&sameDirectory(named,directory_))
     ::unlinkat(parentFd_,leaf_.constData(),AT_REMOVEDIR); // succeeds only if empty
   }
  }
  verified_=false;
  if(directoryFd_>=0){::close(directoryFd_);directoryFd_=-1;}
  if(parentFd_>=0){::close(parentFd_);parentFd_=-1;}
 }
private:
 static bool privateDirectory(const struct stat& s){return S_ISDIR(s.st_mode)&&s.st_uid==getuid()&&(s.st_mode&0777)==0700;}
 static bool sameDirectory(const struct stat& a,const struct stat& b){return privateDirectory(a)&&privateDirectory(b)&&a.st_dev==b.st_dev&&a.st_ino==b.st_ino;}
 static bool privateFile(const struct stat& s){return S_ISREG(s.st_mode)&&s.st_uid==getuid()&&(s.st_mode&0777)==0600&&s.st_nlink==1;}
 int parentFd_=-1,directoryFd_=-1;QByteArray leaf_;struct stat directory_{},file_{};bool verified_=false;
};
}
