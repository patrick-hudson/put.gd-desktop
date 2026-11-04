#include "core/ImagePages.h"
#include <QtTest>
#include <cstring>
#include <fstream>
#include <memory>
#if defined(__linux__) && defined(__GLIBC__)
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
using namespace putgd;
namespace {
// Lands in the test binary's read-only segment, whole pages of it.
alignas(4096) const char readOnlyTable[3*4096]={'p','u','t','g','d',1,2,3};
// Far enough apart, and from anything else, that the kernel's usual fault-around (64 KiB) can't map one for another.
constexpr size_t farApart=16*4096;
alignas(4096) const char snapshotTable[5*farApart]={'p','u','t','g','d'};
long residentPages() {std::ifstream statm("/proc/self/statm");long size=0,resident=0;statm>>size>>resident;return resident;}
char touch(const char* address) {return *static_cast<const volatile char*>(address);}
#if defined(__linux__) && defined(__GLIBC__)
bool mapped(const void* address) {
 const auto pageSize=uintptr_t(sysconf(_SC_PAGESIZE));
 const int pagemap=open("/proc/self/pagemap",O_RDONLY|O_CLOEXEC);
 quint64 entry=0;
 const auto read=pread(pagemap,&entry,sizeof entry,off_t(reinterpret_cast<uintptr_t>(address)/pageSize*sizeof entry));
 close(pagemap);
 return read==ssize_t(sizeof entry)&&(entry>>63);
}
// The records hold a process's worth of pages, too much for this test's stack.
std::unique_ptr<ImagePageSnapshot> emptySnapshot() {return std::make_unique<ImagePageSnapshot>();}
// Where a page's bit lives, or nullptr if the snapshot has no segment holding it.
quint64* snapshotWord(ImagePageSnapshot& snapshot,const void* address,quint64& mask) {
 const auto pageSize=uintptr_t(sysconf(_SC_PAGESIZE)),at=reinterpret_cast<uintptr_t>(address);
 for(int i=0;i<snapshot.segments;++i) {
  const auto& segment=snapshot.segment[i];
  if(at<segment.start||at>=segment.start+segment.pages*pageSize)continue;
  const auto bit=segment.firstBit+quint32((at-segment.start)/pageSize);
  mask=1ULL<<(bit%64);
  return &snapshot.bits[bit/64];
 }
 return nullptr;
}
#endif
}
class ImagePagesContract:public QObject {
 Q_OBJECT
private slots:
 void releasesCleanPagesAndTheyComeBackIntact() {
#if !defined(__linux__) || !defined(__GLIBC__)
  QSKIP("Linux glibc only");
#else
  // The table faults in from the binary before and after the release.
  const auto before=QByteArray(readOnlyTable,sizeof readOnlyTable);
  const auto resident=residentPages();
  const auto release=releaseCleanImagePages();
  QVERIFY(release.segments>0);QVERIFY(release.bytes>0);
  QVERIFY(residentPages()<resident);
  QCOMPARE(QByteArray(readOnlyTable,sizeof readOnlyTable),before);
  // Qt's and libc's code still runs from the pages just released.
  QCOMPARE(QString::number(4096),QString("4096"));
#endif
 }
 void aSnapshotKeepsThePagesItSawAndNoOthers() {
#if !defined(__linux__) || !defined(__GLIBC__)
  QSKIP("Linux glibc only");
#else
  const auto* hot=snapshotTable+farApart;const auto* cold=snapshotTable+2*farApart;
  releaseCleanImagePages();
  QVERIFY(!mapped(cold));
  QCOMPARE(touch(hot),'\0');
  auto snapshot=emptySnapshot();
  QVERIFY(snapshotImagePages(*snapshot));
  QVERIFY(snapshot->segments>0);QVERIFY(!snapshot->overflowed);
  QVERIFY(snapshot->mappedBytes>0);QVERIFY(snapshot->bytesInUse()<=qint64(sizeof(ImagePageSnapshot)));
  quint64 mask=0;
  auto* word=snapshotWord(*snapshot,hot,mask);
  QVERIFY(word);QVERIFY(*word&mask);
  // Used after the snapshot, like a capture's encoder. A newer kernel can map a
  // whole large folio for one touch, so the snapshot may have seen this page too.
  word=snapshotWord(*snapshot,cold,mask);
  QVERIFY(word);*word&=~mask;
  QCOMPARE(touch(cold),'\0');QVERIFY(mapped(cold));
  const auto release=releaseCleanImagePages(snapshot.get());
  QVERIFY(release.bytes>0);QVERIFY(release.keptBytes>0);
  QVERIFY(mapped(hot));QVERIFY(!mapped(cold));
  QCOMPARE(touch(snapshotTable),'p');QCOMPARE(touch(cold),'\0');
#endif
 }
 void aLaterSnapshotOnlyAddsPages() {
#if !defined(__linux__) || !defined(__GLIBC__)
  QSKIP("Linux glibc only");
#else
  const auto* first=snapshotTable+farApart;const auto* second=snapshotTable+2*farApart;
  releaseCleanImagePages();
  QCOMPARE(touch(first),'\0');
  auto snapshot=emptySnapshot();
  QVERIFY(snapshotImagePages(*snapshot));
  const auto recorded=snapshot->segments;
  // The first page has gone from memory by the second press, which can't undo it.
  releaseCleanImagePages();
  QVERIFY(!mapped(first));
  QCOMPARE(touch(second),'\0');
  QVERIFY(snapshotImagePages(*snapshot));
  QCOMPARE(snapshot->segments,recorded);
  QCOMPARE(touch(first),'\0');
  releaseCleanImagePages(snapshot.get());
  QVERIFY(mapped(first));QVERIFY(mapped(second));
#endif
 }
 void aStaleSnapshotReleasesWhatItDoesNotRecognise() {
#if !defined(__linux__) || !defined(__GLIBC__)
  QSKIP("Linux glibc only");
#else
  const auto* page=snapshotTable+farApart;
  QCOMPARE(touch(page),'\0');
  auto snapshot=emptySnapshot();
  QVERIFY(snapshotImagePages(*snapshot));
  // As if every library had since moved or changed size.
  for(int i=0;i<snapshot->segments;++i)snapshot->segment[i].start+=quintptr(1)<<40;
  const auto release=releaseCleanImagePages(snapshot.get());
  QVERIFY(release.bytes>0);QCOMPARE(release.keptBytes,qint64(0));
  QVERIFY(!mapped(page));
  QCOMPARE(touch(page),'\0');
#endif
 }
 void aSnapshotThatRanOutOfRoomReleasesNothing() {
#if !defined(__linux__) || !defined(__GLIBC__)
  QSKIP("Linux glibc only");
#else
  const auto* page=snapshotTable+farApart;
  QCOMPARE(touch(page),'\0');
  auto snapshot=emptySnapshot();
  snapshot->overflowed=true;
  const auto release=releaseCleanImagePages(snapshot.get());
  QCOMPARE(release.segments,0);QCOMPARE(release.bytes,qint64(0));
  QVERIFY(mapped(page));
#endif
 }
 void aSegmentWithAPrivatePageKeepsIt() {
#if !defined(__linux__) || !defined(__GLIBC__)
  QSKIP("Linux glibc only");
#else
  // What a debugger's breakpoint does: one private page in a read-only segment.
  const auto pageSize=size_t(sysconf(_SC_PAGESIZE));
  auto* page=const_cast<char*>(readOnlyTable)+pageSize;
  QCOMPARE(mprotect(page,pageSize,PROT_READ|PROT_WRITE),0);
  *reinterpret_cast<volatile char*>(page)='x';
  QCOMPARE(mprotect(page,pageSize,PROT_READ),0);
  // Keeping other pages doesn't change that, whatever the snapshot recorded.
  auto snapshot=emptySnapshot();
  QVERIFY(snapshotImagePages(*snapshot));
  for(const auto keep:{static_cast<const ImagePageSnapshot*>(nullptr),static_cast<const ImagePageSnapshot*>(snapshot.get())}) {
   const auto release=releaseCleanImagePages(keep);
   QVERIFY(release.skipped>=1);
  }
  const char kept=*reinterpret_cast<const volatile char*>(page);
  QCOMPARE(kept,'x');
  // Its neighbour in the same segment was not lost either.
  const char neighbour=*reinterpret_cast<const volatile char*>(readOnlyTable);
  QCOMPARE(neighbour,'p');
#endif
 }
};
QTEST_APPLESS_MAIN(ImagePagesContract)
#include "test_image_pages.moc"
