#include "core/ImagePages.h"
#include <QtTest>
#include <cstring>
#include <fstream>
#if defined(__linux__) && defined(__GLIBC__)
#include <sys/mman.h>
#include <unistd.h>
#endif
using namespace putgd;
namespace {
// Lands in the test binary's read-only segment, whole pages of it.
alignas(4096) const char readOnlyTable[3*4096]={'p','u','t','g','d',1,2,3};
long residentPages() {std::ifstream statm("/proc/self/statm");long size=0,resident=0;statm>>size>>resident;return resident;}
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
  const auto release=releaseCleanImagePages();
  QVERIFY(release.skipped>=1);
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
