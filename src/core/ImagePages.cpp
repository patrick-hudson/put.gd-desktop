#include "core/ImagePages.h"
#include <cstddef>
#include <cstdlib>
qint64 putgd::ImagePageSnapshot::bytesInUse() const {return qint64(offsetof(ImagePageSnapshot,segment)+segments*sizeof(Segment)+(bitsUsed+7)/8);}
#if defined(__linux__) && defined(__GLIBC__)
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

namespace putgd {
namespace {
// /proc/<pid>/pagemap entry bits (Documentation/admin-guide/mm/pagemap.rst).
constexpr quint64 pagePresent=1ULL<<63,pageSwapped=1ULL<<62,pageFile=1ULL<<61;
struct ReleasePass {int pagemap=-1;uintptr_t pageSize=0;ImagePageRelease result;const ImagePageSnapshot* keep=nullptr;int cursor=0;};
struct SnapshotPass {int pagemap=-1;uintptr_t pageSize=0;ImagePageSnapshot* snapshot=nullptr;int cursor=0;bool readable=true;};
// Whole pages inside a read-only or code segment only: a page at either edge may be another segment's.
bool cleanSegmentPages(const dl_phdr_info* info,const ElfW(Phdr)& segment,uintptr_t pageSize,uintptr_t& start,uintptr_t& end) {
 if(segment.p_type!=PT_LOAD||(segment.p_flags&PF_W)||!segment.p_filesz)return false;
 const uintptr_t mask=pageSize-1,first=info->dlpi_addr+segment.p_vaddr;
 start=(first+mask)&~mask;end=(first+segment.p_filesz)&~mask;
 return start<end;
}
// The snapshot's entry for exactly this range. The loader's order rarely changes,
// so the search resumes after the last match.
int findSegment(const ImagePageSnapshot& snapshot,uintptr_t start,quint32 pages,int& cursor) {
 for(int n=0;n<snapshot.segments;++n) {
  const auto at=(cursor+n)%snapshot.segments;
  if(snapshot.segment[at].start==start&&snapshot.segment[at].pages==pages) {cursor=at+1;return at;}
 }
 return -1;
}
// No page in the range holds anything but the file's own bytes.
bool holdsOnlyFilePages(const ReleasePass& pass,uintptr_t start,uintptr_t end) {
 quint64 entries[512];
 for(auto at=start;at<end;) {
  const auto count=std::min<uintptr_t>((end-at)/pass.pageSize,std::size(entries));
  const auto wanted=ssize_t(count*sizeof(quint64));
  if(pread(pass.pagemap,entries,size_t(wanted),off_t(at/pass.pageSize*sizeof(quint64)))!=wanted)return false;
  for(uintptr_t i=0;i<count;++i)if((entries[i]&pageSwapped)||((entries[i]&pagePresent)&&!(entries[i]&pageFile)))return false;
  at+=count*pass.pageSize;
 }
 return true;
}
bool dropPages(ReleasePass& pass,uintptr_t start,uintptr_t end) {
 const auto pages=quint32((end-start)/pass.pageSize);
 const auto found=pass.keep?findSegment(*pass.keep,start,pages,pass.cursor):-1;
 const auto kept=[&](quint32 page) {
  const auto bit=pass.keep->segment[found].firstBit+page;
  return (pass.keep->bits[bit/64]>>(bit%64))&1;
 };
 for(quint32 page=0;page<pages;) {
  if(found>=0&&kept(page)) {pass.result.keptBytes+=qint64(pass.pageSize);++page;continue;}
  auto run=page+1;
  while(run<pages&&!(found>=0&&kept(run)))++run;
  if(madvise(reinterpret_cast<void*>(start+page*pass.pageSize),(run-page)*pass.pageSize,MADV_DONTNEED)!=0)return false;
  pass.result.bytes+=qint64((run-page)*pass.pageSize);
  page=run;
 }
 return true;
}
// Runs under the loader's lock, so no object can be unmapped meanwhile; it
// allocates nothing and takes no other lock.
int releaseObject(dl_phdr_info* info,size_t,void* data) {
 auto& pass=*static_cast<ReleasePass*>(data);
 if(info->dlpi_name&&std::strstr(info->dlpi_name,"linux-vdso"))return 0;
 for(int i=0;i<info->dlpi_phnum;++i) {
  uintptr_t start=0,end=0;
  if(!cleanSegmentPages(info,info->dlpi_phdr[i],pass.pageSize,start,end))continue;
  if(!holdsOnlyFilePages(pass,start,end)||!dropPages(pass,start,end)) {++pass.result.skipped;continue;}
  ++pass.result.segments;
 }
 return 0;
}
// Under the loader's lock too, with the same promises.
int snapshotObject(dl_phdr_info* info,size_t,void* data) {
 auto& pass=*static_cast<SnapshotPass*>(data);auto& snapshot=*pass.snapshot;
 if(info->dlpi_name&&std::strstr(info->dlpi_name,"linux-vdso"))return 0;
 for(int i=0;i<info->dlpi_phnum;++i) {
  uintptr_t start=0,end=0;
  if(!cleanSegmentPages(info,info->dlpi_phdr[i],pass.pageSize,start,end))continue;
  const auto pages=quint32((end-start)/pass.pageSize);
  auto found=findSegment(snapshot,start,pages,pass.cursor);
  if(found<0) {
   if(snapshot.segments==ImagePageSnapshot::maxSegments||snapshot.bitsUsed+quint64(pages)>quint64(ImagePageSnapshot::maxWords)*64) {snapshot.overflowed=true;continue;}
   found=snapshot.segments++;snapshot.segment[found]={start,pages,snapshot.bitsUsed};snapshot.bitsUsed+=pages;
  }
  const auto firstBit=snapshot.segment[found].firstBit;
  quint64 entries[512];
  for(quint32 page=0;page<pages;) {
   const auto count=std::min<quint32>(pages-page,std::size(entries));
   const auto wanted=ssize_t(count*sizeof(quint64));
   if(pread(pass.pagemap,entries,size_t(wanted),off_t((start/pass.pageSize+page)*sizeof(quint64)))!=wanted) {pass.readable=false;return 1;}
   for(quint32 n=0;n<count;++n)if(entries[n]&pagePresent) {const auto bit=firstBit+page+n;snapshot.bits[bit/64]|=1ULL<<(bit%64);}
   page+=count;
  }
 }
 return 0;
}
}
bool snapshotImagePages(ImagePageSnapshot& snapshot) {
 SnapshotPass pass;pass.pageSize=uintptr_t(sysconf(_SC_PAGESIZE));pass.snapshot=&snapshot;
 pass.pagemap=open("/proc/self/pagemap",O_RDONLY|O_CLOEXEC);
 if(pass.pagemap<0)return false;
 dl_iterate_phdr(snapshotObject,&pass);
 close(pass.pagemap);
 qint64 mapped=0;
 for(quint32 word=0;word<(snapshot.bitsUsed+63)/64;++word)mapped+=std::popcount(snapshot.bits[word]);
 snapshot.mappedBytes=mapped*qint64(pass.pageSize);
 return pass.readable;
}
ImagePageRelease releaseCleanImagePages(const ImagePageSnapshot* keep) {
 ReleasePass pass;pass.pageSize=uintptr_t(sysconf(_SC_PAGESIZE));pass.keep=keep;
 if(keep&&keep->overflowed)return pass.result;
 // Without page flags nothing proves a segment clean, so nothing is released.
 pass.pagemap=open("/proc/self/pagemap",O_RDONLY|O_CLOEXEC);
 if(pass.pagemap<0)return pass.result;
 dl_iterate_phdr(releaseObject,&pass);
 close(pass.pagemap);
 return pass.result;
}
}
#else
namespace putgd {
bool snapshotImagePages(ImagePageSnapshot&) {return false;}
ImagePageRelease releaseCleanImagePages(const ImagePageSnapshot*) {return {};}
}
#endif
