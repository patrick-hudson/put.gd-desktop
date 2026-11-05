#include "core/AllocatorPolicy.h"
#include <QtTest>
#include <barrier>
#include <cstring>
#include <random>
#include <thread>
#include <vector>
#if defined(__has_feature)
#if __has_feature(address_sanitizer)||__has_feature(thread_sanitizer)||__has_feature(memory_sanitizer)
#define PUTGD_REPLACED_ALLOCATOR 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)||defined(__SANITIZE_THREAD__)
#define PUTGD_REPLACED_ALLOCATOR 1
#endif
using namespace putgd;
// #560: after a 50-upload burst the resident kept about 12 MB in worker arenas
// whose entirely free top chunks malloc_trim cannot return. The resident policy
// must leave such worker churn trimmable.
class AllocatorPolicyContract:public QObject {
 Q_OBJECT
private slots:
 void workerChurnIsTrimmable(){
#if !defined(__linux__)||!defined(__GLIBC__)
  QSKIP("glibc allocator policy");
#elif defined(PUTGD_REPLACED_ALLOCATOR)
  QSKIP("sanitizers replace the glibc allocator");
#else
  // Like the resident's main(), before any worker thread starts.
  QVERIFY(applyResidentAllocatorPolicy());
  malloc_trim(0);const auto baseline=mallinfo2();
  // Concurrent workers, as in a burst of journal saves, queries and transfers:
  // each holds a changing set of small chunks, frees them all and exits.
  constexpr int workers=8;
  for(int round=0;round<3;++round){
   std::barrier start(workers);std::vector<std::thread> threads;
   for(int worker=0;worker<workers;++worker)threads.emplace_back([&start,worker,round]{
    start.arrive_and_wait();std::mt19937 random(worker*31+round);std::vector<void*> live;
    for(int i=0;i<12000;++i){
     const size_t size=16+random()%4096;auto* chunk=std::malloc(size);std::memset(chunk,1,size);live.push_back(chunk);
     if(live.size()>1200){const size_t victim=random()%live.size();std::free(live[victim]);live[victim]=live.back();live.pop_back();}
    }
    for(auto* chunk:live)std::free(chunk);
   });
   for(auto& thread:threads)thread.join();
  }
  // The resident's idle cleanup.
  malloc_trim(0);const auto settled=mallinfo2();
  // Per-thread arenas keep over 1 MB here; one trimmable arena keeps a few pages.
  const auto retained=qint64(settled.arena)-qint64(baseline.arena);
  QVERIFY2(retained<512*1024,qPrintable(QString("worker churn left %1 bytes of allocator heap after malloc_trim (baseline %2)").arg(retained).arg(baseline.arena)));
#endif
 }
};
QTEST_GUILESS_MAIN(AllocatorPolicyContract)
#include "test_allocator_policy.moc"
