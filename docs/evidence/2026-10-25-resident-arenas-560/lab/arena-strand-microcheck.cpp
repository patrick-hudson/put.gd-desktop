// Reproduces stranded non-main arena tops: worker threads churn small chunks,
// exit, then malloc_trim(0) runs (as the resident's idle cleanup does).
#include <malloc.h>
#include <thread>
#include <vector>
#include <random>
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
static long rssKiB(){std::ifstream f("/proc/self/status");std::string l;while(std::getline(f,l))if(l.rfind("RssAnon:",0)==0)return std::atol(l.c_str()+8);return -1;}
int main(int argc,char**argv){
 mallopt(M_MMAP_THRESHOLD,128*1024);
 if(argc>1&&std::atoi(argv[1])>0)mallopt(M_ARENA_MAX,std::atoi(argv[1]));
 const int threads=8;
 long before=rssKiB();
 for(int round=0;round<3;++round){
  std::barrier sync(threads);std::vector<std::thread> pool;
  for(int t=0;t<threads;++t)pool.emplace_back([&,t]{
   sync.arrive_and_wait();std::mt19937 rng(t*7+round);std::vector<void*> live;
   for(int i=0;i<12000;++i){size_t n=16+rng()%4096;void*p=malloc(n);memset(p,1,n);live.push_back(p);
    if(live.size()>1200){size_t k=rng()%live.size();free(live[k]);live[k]=live.back();live.pop_back();}}
   for(void*p:live)free(p);});
  for(auto&th:pool)th.join();
 }
 long settled=rssKiB();malloc_trim(0);long trimmed=rssKiB();
 struct mallinfo2 mi=mallinfo2();
 std::printf("arena_max=%s rssAnon before=%ld settled=%ld afterTrim=%ld KiB; mallinfo arena=%zu free=%zu inuse=%zu\n",argc>1?argv[1]:"default",before,settled,trimmed,mi.arena,mi.fordblks,mi.uordblks);
}
