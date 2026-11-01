#include "FolderZip.h"
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <memory>
#include <utility>
#include <vector>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
namespace putgd {
namespace {
// Local header + ZIP64 extra (sizes), central record + ZIP64 extra (sizes, offset),
// then the ZIP64 end record, its locator and the classic end record.
constexpr qint64 probeBytes=1024*1024,localFixed=30+20,centralFixed=46+28,endRecords=56+20+22,sinkBytes=256*1024;
void put16(QByteArray& out,quint32 value) {out.append(char(value&0xff));out.append(char((value>>8)&0xff));}
void put32(QByteArray& out,quint32 value) {put16(out,value&0xffff);put16(out,value>>16);}
void put64(QByteArray& out,quint64 value) {put32(out,quint32(value));put32(out,quint32(value>>32));}
bool deflateStart(z_stream& z) {z={};return deflateInit2(&z,Z_DEFAULT_COMPRESSION,Z_DEFLATED,-MAX_WBITS,8,Z_DEFAULT_STRATEGY)==Z_OK;}
QString shown(const QByteArray& name) {return QString::fromUtf8(name.endsWith('/')?name.chopped(1):name);}
Error unreadable(const QString& name,int code) {return {"source_unavailable",QStringLiteral("Cannot read %1: %2").arg(name,QString::fromLocal8Bit(std::strerror(code))),{}};}
Error changed(const QString& name) {return {"source_changed",QStringLiteral("%1 changed while put.gd was zipping it; try again").arg(name),{}};}
// DOS date (high half) and time (low half) in local time, clamped to 1980-2107.
quint32 dosTime(time_t seconds) {
 tm local{};if(!localtime_r(&seconds,&local)||local.tm_year<80)return (1u<<5|1u)<<16;
 if(local.tm_year>207)return quint32((127u<<9|12u<<5|31u)<<16|(23u<<11|59u<<5|29u));
 return quint32(((local.tm_year-80)<<9|(local.tm_mon+1)<<5|local.tm_mday)<<16|(local.tm_hour<<11|local.tm_min<<5|local.tm_sec/2));
}
struct Entry {int parent=-1;const char* leaf=nullptr;QByteArray name;bool directory=false;struct stat info{};};
struct DirectoryClose {void operator()(DIR* d) const {closedir(d);}};
struct Descriptor {int fd=-1;~Descriptor(){if(fd>=0)::close(fd);}};
// Visits the folder, then everything under it, parents first and in directory order.
// One open directory per level: memory and descriptors follow depth, not entry count.
Error walk(const QString& folder,const std::function<Error(const Entry&)>& visit) {
 const QFileInfo info(folder);const auto leaf=QFile::encodeName(info.fileName());const auto display=info.fileName();
 if(leaf.isEmpty())return {"source_unsupported",QStringLiteral("Cannot zip %1").arg(folder),{}};
 Descriptor parent;parent.fd=::open(QFile::encodeName(info.absolutePath()).constData(),O_PATH|O_DIRECTORY|O_CLOEXEC);
 Entry top;top.parent=parent.fd;top.leaf=leaf.constData();top.directory=true;
 if(parent.fd<0||fstatat(parent.fd,leaf.constData(),&top.info,AT_SYMLINK_NOFOLLOW)!=0)return unreadable(display,errno);
 if(!S_ISDIR(top.info.st_mode))return {"source_unavailable",QStringLiteral("%1 is not a folder").arg(display),{}};
 QByteArray raw=leaf+'/';top.name=QString::fromUtf8(raw).toUtf8();
 if(auto e=visit(top))return e;
 std::vector<std::pair<std::unique_ptr<DIR,DirectoryClose>,int>> stack;
 const int first=openat(parent.fd,leaf.constData(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);DIR* opened=first<0?nullptr:fdopendir(first);
 if(!opened) {const int code=errno;if(first>=0)::close(first);return unreadable(display,code);}
 stack.emplace_back(opened,raw.size());
 while(!stack.empty()) {
  DIR* current=stack.back().first.get();raw.truncate(stack.back().second);
  errno=0;const dirent* item=readdir(current);
  if(!item) {if(errno)return unreadable(shown(raw),errno);stack.pop_back();continue;}
  if(!std::strcmp(item->d_name,".")||!std::strcmp(item->d_name,".."))continue;
  Entry entry;entry.parent=dirfd(current);entry.leaf=item->d_name;
  if(fstatat(entry.parent,item->d_name,&entry.info,AT_SYMLINK_NOFOLLOW)!=0) {if(errno==ENOENT)continue;return unreadable(shown(raw+item->d_name),errno);}
  // Symlinks, sockets, FIFOs and devices are skipped, never followed.
  entry.directory=S_ISDIR(entry.info.st_mode);if(!entry.directory&&!S_ISREG(entry.info.st_mode))continue;
  const auto path=raw+item->d_name+(entry.directory?"/":"");entry.name=QString::fromUtf8(path).toUtf8();
  if(entry.name.size()>0xffff)return {"source_unsupported",QStringLiteral("%1 has a path too long for a zip archive").arg(shown(entry.name).left(255)),{}};
  if(auto e=visit(entry))return e;
  if(!entry.directory)continue;
  const int fd=openat(entry.parent,item->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);DIR* child=fd<0?nullptr:fdopendir(fd);
  if(!child) {const int code=errno;if(fd>=0)::close(fd);return unreadable(shown(path),code);}
  raw=path;stack.emplace_back(child,raw.size());
 }
 return {};
}
bool writeAll(int fd,const char* data,qint64 size,qint64 at=-1) {
 while(size>0) {
  const auto n=at<0?::write(fd,data,size_t(size)): ::pwrite(fd,data,size_t(size),at);
  if(n<0&&errno==EINTR)continue;
  if(n<=0)return false;
  data+=n;size-=n;if(at>=0)at+=n;
 }
 return true;
}
// Fills `size` bytes unless the file ends first; -1 on a read error.
qint64 readFull(int fd,char* data,qint64 size) {
 qint64 total=0;
 while(total<size) {const auto n=::read(fd,data+total,size_t(size-total));if(n<0&&errno==EINTR)continue;if(n<0)return -1;if(n==0)break;total+=n;}
 return total;
}
// Buffered appends to a descriptor; patches land behind the write position.
struct Sink {
 int fd=-1;qint64 offset=0;QByteArray buffer;
 bool flush() {const bool ok=writeAll(fd,buffer.constData(),buffer.size());buffer.clear();return ok;}
 bool put(const char* data,qint64 size) {
  offset+=size;if(buffer.size()+size<=sinkBytes) {buffer.append(data,size);return true;}
  return flush()&&writeAll(fd,data,size);
 }
 bool put(const QByteArray& data) {return put(data.constData(),data.size());}
 bool patch(qint64 at,const QByteArray& data) {return flush()&&writeAll(fd,data.constData(),data.size(),at);}
};
}

FolderZip::Plan FolderZip::plan(const QString& folder) {
 Plan plan;plan.folder=QDir::cleanPath(QFileInfo(folder).absoluteFilePath());plan.archiveName=QFileInfo(plan.folder).fileName()+".zip";
 z_stream z;if(!deflateStart(z)) {plan.error={"artifact_io","Cannot start compression",{}};return plan;}
 qint64 data=0,central=0,local=0;
 plan.error=walk(plan.folder,[&](const Entry& entry)->Error {
  ++plan.entries;local+=localFixed+entry.name.size();central+=centralFixed+entry.name.size();
  if(entry.directory)return {};
  if(faccessat(entry.parent,entry.leaf,R_OK,0)!=0)return unreadable(shown(entry.name),errno);
  const qint64 size=entry.info.st_size;++plan.files;plan.sourceBytes+=size;
  data+=qMax<qint64>(size,qint64(deflateBound(&z,uLong(size))));return {};
 });
 deflateEnd(&z);
 if(!plan.error) {plan.finalBound=data+local+central+endRecords;plan.peakBytes=plan.finalBound+central;}
 return plan;
}

Error FolderZip::write(const Plan& plan,const QString& output,const std::function<bool()>& cancelled) {
 if(plan.error)return plan.error;
 const auto outputPath=QFile::encodeName(output),spillPath=QFile::encodeName(QFileInfo(output).dir().filePath(QUuid::createUuid().toString(QUuid::WithoutBraces)));
 const auto folderName=QFileInfo(plan.folder).fileName();
 Sink out,spill;
 // Removes the partial archive and the spill file unless the archive completes.
 struct Files {Sink& out;Sink& spill;const QByteArray &outPath,&spillPath;bool keep=false;
  ~Files(){if(spill.fd>=0) {::close(spill.fd);::unlink(spillPath.constData());}if(out.fd>=0) {::close(out.fd);if(!keep)::unlink(outPath.constData());}}
 } files{out,spill,outputPath,spillPath};
 out.fd=::open(outputPath.constData(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
 if(out.fd<0)return {"artifact_io",QStringLiteral("Cannot create the archive for %1").arg(folderName),{}};
 spill.fd=::open(spillPath.constData(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC,0600);
 if(spill.fd<0)return {"artifact_io",QStringLiteral("Cannot create the archive for %1").arg(folderName),{}};
 z_stream z;if(!deflateStart(z))return {"artifact_io","Cannot start compression",{}};
 const std::unique_ptr<z_stream,int(*)(z_stream*)> stream(&z,deflateEnd);
 // Fixed buffers: the read buffer doubles as the 1 MiB probe; the probe's deflate
 // output is kept so a file that fits the probe is not deflated twice.
 std::vector<char> input(probeBytes),probe(deflateBound(&z,probeBytes)),deflated(sinkBytes);
 const Error writeFailed{"artifact_io",QStringLiteral("Cannot write the archive for %1").arg(folderName),{}};
 const qint64 directoryBudget=plan.peakBytes-plan.finalBound;qint64 entries=0;
 // Whatever the walk finds now, the archive stays within the bound the checks and the
 // disk reservation used: a source that grew fails here instead of overrunning them.
 auto fits=[&](const Entry& entry,qint64 more) {
  const qint64 record=centralFixed+entry.name.size();
  return out.offset+more+spill.offset+record+endRecords<=plan.finalBound&&spill.offset+record<=directoryBudget;
 };
 auto stopped=[&]{return cancelled&&cancelled();};
 const auto walked=walk(plan.folder,[&](const Entry& entry)->Error {
  if(stopped())return {"cancelled","Cancelled",{}};
  struct stat info=entry.info;Descriptor source;
  if(!entry.directory) {
   source.fd=openat(entry.parent,entry.leaf,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_NOCTTY|O_CLOEXEC);
   // Gone or replaced by a symlink since the walk listed it: skipped, as the walk would.
   if(source.fd<0)return errno==ENOENT||errno==ELOOP?Error{}:unreadable(shown(entry.name),errno);
   if(fstat(source.fd,&info)!=0)return unreadable(shown(entry.name),errno);
   if(!S_ISREG(info.st_mode))return {};
   fcntl(source.fd,F_SETFL,fcntl(source.fd,F_GETFL)&~O_NONBLOCK);
  }
  qint64 read=0,written=0;quint32 crc=0;bool store=true,ended=true;qint64 probed=0;
  if(!entry.directory) {
   read=readFull(source.fd,input.data(),probeBytes);if(read<0)return unreadable(shown(entry.name),errno);
   crc=crc32(0,reinterpret_cast<const Bytef*>(input.data()),uInt(read));ended=read<probeBytes;
   deflateReset(&z);z.next_in=reinterpret_cast<Bytef*>(input.data());z.avail_in=uInt(read);z.next_out=reinterpret_cast<Bytef*>(probe.data());z.avail_out=uInt(probe.size());
   if(deflate(&z,Z_FINISH)!=Z_STREAM_END)return {"artifact_io","Compression failed",{}};
   probed=qint64(z.total_out);store=read==0||probed>=read;
  }
  const qint64 header=out.offset;const quint32 when=dosTime(info.st_mtime);const quint16 method=store?0:8;
  QByteArray local;put32(local,0x04034b50);put16(local,45);put16(local,0x0800);put16(local,method);put32(local,when);
  put32(local,0);put32(local,0xffffffff);put32(local,0xffffffff);put16(local,quint32(entry.name.size()));put16(local,20);
  local+=entry.name;put16(local,1);put16(local,16);put64(local,0);put64(local,0);
  if(!fits(entry,local.size()))return changed(shown(entry.name));
  if(!out.put(local))return writeFailed;
  auto append=[&](const char* data,qint64 size)->Error {
   if(!fits(entry,size))return changed(shown(entry.name));
   if(!out.put(data,size))return writeFailed;
   written+=size;return {};
  };
  // The next chunk of the same single read: CRC and size come from these bytes.
  auto next=[&]()->qint64 {
   if(stopped())return -2;
   const auto n=readFull(source.fd,input.data(),probeBytes);if(n<0)return n;
   crc=crc32(crc,reinterpret_cast<const Bytef*>(input.data()),uInt(n));read+=n;ended=n<probeBytes;return n;
  };
  auto failed=[&](qint64 n)->Error {return n==-2?Error{"cancelled","Cancelled",{}}:unreadable(shown(entry.name),errno);};
  if(!entry.directory&&store) {
   if(auto e=append(input.data(),read))return e;
   while(!ended) {const auto n=next();if(n<0)return failed(n);if(auto e=append(input.data(),n))return e;}
  } else if(!entry.directory&&ended) {
   if(auto e=append(probe.data(),probed))return e;
  } else if(!entry.directory) {
   deflateReset(&z);
   auto pump=[&](qint64 size,int flush)->Error {
    z.next_in=reinterpret_cast<Bytef*>(input.data());z.avail_in=uInt(size);int status=Z_OK;
    do {
     z.next_out=reinterpret_cast<Bytef*>(deflated.data());z.avail_out=uInt(deflated.size());
     status=deflate(&z,flush);if(status==Z_STREAM_ERROR)return {"artifact_io","Compression failed",{}};
     if(auto e=append(deflated.data(),qint64(deflated.size()-z.avail_out)))return e;
    } while(flush==Z_FINISH?status!=Z_STREAM_END:z.avail_out==0);
    return {};
   };
   qint64 size=read;
   for(;;) {
    if(auto e=pump(size,ended?Z_FINISH:Z_NO_FLUSH))return e;
    if(ended)break;
    size=next();if(size<0)return failed(size);
   }
  }
  if(!entry.directory) {
   QByteArray crcBytes,sizes;put32(crcBytes,crc);put64(sizes,quint64(read));put64(sizes,quint64(written));
   if(!out.patch(header+14,crcBytes)||!out.patch(header+30+entry.name.size()+4,sizes))return writeFailed;
  }
  const quint32 mode=(entry.directory?S_IFDIR|0700:S_IFREG|0600)|(info.st_mode&0777);
  QByteArray central;put32(central,0x02014b50);put16(central,0x032d);put16(central,45);put16(central,0x0800);put16(central,method);put32(central,when);
  put32(central,crc);put32(central,0xffffffff);put32(central,0xffffffff);put16(central,quint32(entry.name.size()));put16(central,28);put16(central,0);
  put16(central,0);put16(central,0);put32(central,mode<<16|(entry.directory?0x10:0));put32(central,0xffffffff);
  central+=entry.name;put16(central,1);put16(central,24);put64(central,quint64(read));put64(central,quint64(written));put64(central,quint64(header));
  if(!fits(entry,0))return changed(shown(entry.name));
  if(!spill.put(central))return writeFailed;
  ++entries;return {};
 });
 if(walked)return walked;
 if(stopped())return {"cancelled","Cancelled",{}};
 // Append the spilled central directory, then the ZIP64 and classic end records.
 const qint64 directory=out.offset,directoryBytes=spill.offset;
 if(!spill.flush()||::lseek(spill.fd,0,SEEK_SET)!=0)return writeFailed;
 for(qint64 copied=0;copied<directoryBytes;) {
  const auto n=readFull(spill.fd,input.data(),qMin<qint64>(probeBytes,directoryBytes-copied));
  if(n<=0||!out.put(input.data(),n))return writeFailed;
  copied+=n;
 }
 const qint64 end64=out.offset;
 QByteArray end;put32(end,0x06064b50);put64(end,44);put16(end,0x032d);put16(end,45);put32(end,0);put32(end,0);
 put64(end,quint64(entries));put64(end,quint64(entries));put64(end,quint64(directoryBytes));put64(end,quint64(directory));
 put32(end,0x07064b50);put32(end,0);put64(end,quint64(end64));put32(end,1);
 put32(end,0x06054b50);put16(end,0);put16(end,0);put16(end,quint32(qMin<qint64>(entries,0xffff)));put16(end,quint32(qMin<qint64>(entries,0xffff)));
 put32(end,quint32(qMin<qint64>(directoryBytes,0xffffffff)));put32(end,quint32(qMin<qint64>(directory,0xffffffff)));put16(end,0);
 if(out.offset+end.size()>plan.finalBound)return changed(folderName);
 if(!out.put(end)||!out.flush())return writeFailed;
 const int fd=std::exchange(out.fd,-1);if(::close(fd)!=0) {::unlink(outputPath.constData());return writeFailed;}
 files.keep=true;return {};
}
}
