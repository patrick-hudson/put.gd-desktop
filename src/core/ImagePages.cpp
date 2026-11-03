#include "core/ImagePages.h"
#include <cstdlib>
#if defined(__linux__) && defined(__GLIBC__)
#include <algorithm>
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
struct ReleasePass {int pagemap=-1;uintptr_t pageSize=0;ImagePageRelease result;};
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
// Runs under the loader's lock, so no object can be unmapped meanwhile; it
// allocates nothing and takes no other lock.
int releaseObject(dl_phdr_info* info,size_t,void* data) {
 auto& pass=*static_cast<ReleasePass*>(data);
 if(info->dlpi_name&&std::strstr(info->dlpi_name,"linux-vdso"))return 0;
 for(int i=0;i<info->dlpi_phnum;++i) {
  const auto& segment=info->dlpi_phdr[i];
  if(segment.p_type!=PT_LOAD||(segment.p_flags&PF_W)||!segment.p_filesz)continue;
  // Whole pages inside the segment only: a page at either edge may be another segment's.
  const uintptr_t mask=pass.pageSize-1,first=info->dlpi_addr+segment.p_vaddr;
  const auto start=(first+mask)&~mask,end=(first+segment.p_filesz)&~mask;
  if(start>=end)continue;
  if(!holdsOnlyFilePages(pass,start,end)||madvise(reinterpret_cast<void*>(start),end-start,MADV_DONTNEED)!=0) {++pass.result.skipped;continue;}
  ++pass.result.segments;pass.result.bytes+=qint64(end-start);
 }
 return 0;
}
}
ImagePageRelease releaseCleanImagePages() {
 ReleasePass pass;pass.pageSize=uintptr_t(sysconf(_SC_PAGESIZE));
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
ImagePageRelease releaseCleanImagePages() {return {};}
}
#endif
