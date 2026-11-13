#include "Recognize.h"
#include "CodeReader.h"
#include <QElapsedTimer>
#include <QFile>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <sched.h>
#include <thread>
#include <unistd.h>

namespace putgd::ocr {
namespace {
// One recognition task: a band (a banded image) or a layout block (a single pass).
struct Outcome { BandText text;bool done=false,cutShort=false; };
// The resident set now, from /proc/self/statm (readable under Landlock).
qint64 residentBytes(){
 QFile statm(QStringLiteral("/proc/self/statm"));
 if(!statm.open(QIODevice::ReadOnly))return 0;
 const auto fields=statm.readAll().split(' ');
 return fields.value(1).toLongLong()*::sysconf(_SC_PAGESIZE);
}
void raise(std::atomic<qint64>& value,qint64 seen){qint64 now=value.load();while(seen>now&&!value.compare_exchange_weak(now,seen)){}}
}
int workerThreads(int requested){
 // The CPUs this process may run on (affinity, a container's cpuset), not the
 // machine's: glibc's hardware_concurrency counts every online CPU.
 cpu_set_t set;CPU_ZERO(&set);
 const int cores=::sched_getaffinity(0,sizeof set,&set)==0?std::max(1,CPU_COUNT(&set)):int(std::max(1u,std::thread::hardware_concurrency()));
 return std::clamp(requested>0?requested:cores,1,maximumWorkerThreads);
}
JobResult recognizeImage(const GreyView& image,const JobOptions& options){
 JobResult result;QElapsedTimer total;total.start();
 if(!image.valid()||qint64(image.width)*image.height>maximumInputPixels){result.error=QStringLiteral("input_excessive");return result;}
 // Codes first, from the full image; their areas are filled in every band copy.
 QElapsedTimer stage;stage.start();
 result.recognition.codes=readCodes(image);
 QList<Quad> areas;for(const auto& code:result.recognition.codes)areas.append(code.quad);
 result.timings.codesMs=stage.elapsed();
 result.profile=measure(image,areas);
 const int threads=workerThreads(options.threads);
 auto limits=options.limits;limits.threads=threads;
 const auto plan=putgd::ocr::plan(image.width,image.height,result.profile,limits);
 if(!plan.error.isEmpty()){result.error=plan.error;return result;}
 stage.restart();
 LanguageData data(options.directories);
 TextEngine first;
 const qint64 beforeEngine=residentBytes();
 result.error=first.start(options.languages,options.strictLanguages,options.engine,result.failedLanguage);
 const qint64 engineCost=std::max<qint64>(0,residentBytes()-beforeEngine);
 result.timings.engineStartMs=stage.elapsed();
 if(!result.error.isEmpty())return result;
 result.used=first.used();result.skipped=first.skipped();result.dataFiles=data.files();
 stage.restart();
 const bool invert=options.invertDark&&result.profile.dark;
 const auto left=[&]{return options.budgetMs>0?int(options.budgetMs-total.elapsed()):0;};

 // A single pass is one band copy; a banded image makes a copy per task.
 GreyImage whole;QList<QRect> blocks;
 if(!plan.banded())whole=bandImage(image,plan.bands.first(),plan.scale,options.limits.border,areas,result.profile.background,invert);
 if(!options.dumpBand.isEmpty()){
  // Method comparisons only: the first band as Tesseract gets it (PGM).
  const auto copy=plan.banded()?bandImage(image,plan.bands.first(),plan.scale,options.limits.border,areas,result.profile.background,invert):whole;
  QFile dump(options.dumpBand);
  if(dump.open(QIODevice::WriteOnly)){dump.write("P5\n"+QByteArray::number(copy.width)+' '+QByteArray::number(copy.height)+"\n255\n");dump.write(copy.pixels);}
 }
 // Parallel only where there is enough to share: several bands, or a single
 // pass large enough that its blocks outweigh starting another engine.
 const bool blockParallel=!plan.banded()&&threads>1&&qint64(whole.width)*whole.height>=options.parallelPixels;
 const int wanted=plan.banded()?std::min<int>(threads,int(plan.bands.size())):(blockParallel?threads:1);

 // The task list: bands are known now; blocks once the first engine has laid
 // the page out, while the other engines start.
 std::mutex lock;std::condition_variable ready;bool tasksKnown=plan.banded()||!blockParallel;int taskCount=plan.banded()?int(plan.bands.size()):1;
 QList<Outcome> outcomes;outcomes.resize(plan.banded()?plan.bands.size():1);
 std::atomic<int> next{0};std::atomic<qint64> slowest{0},taskGrowth{0};std::atomic<bool> stop{false};
 std::function<void(TextEngine&,bool)> work;
 const auto roomFor=[&](qint64 bytes){return options.memoryBudget<=0||residentBytes()+bytes<=options.memoryBudget;};
 std::vector<std::unique_ptr<TextEngine>> extra;std::vector<std::thread> pool;
 // Extra engines, each on its own thread; started at most once, from this thread.
 const std::function<void(int)> startExtras=[&](int count){
  for(int w=0;w<count;++w){
   extra.push_back(std::make_unique<TextEngine>());auto* engine=extra.back().get();
   pool.emplace_back([&,engine]{if(engine->startLike(first).isEmpty())work(*engine,true);});
  }
 };
 // How many extra engines fit in the memory budget when each costs this much.
 const auto fitting=[&](qint64 each){
  if(options.memoryBudget<=0)return wanted-1;
  const qint64 room=options.memoryBudget-residentBytes();
  return std::clamp(int(room/std::max<qint64>(1,each)),0,wanted-1);
 };
 work=[&](TextEngine& engine,bool extraWorker){
  {std::unique_lock<std::mutex> wait(lock);ready.wait(wait,[&]{return tasksKnown;});}
  for(;;){
   // The memory budget: an extra worker leaves its tasks to the others.
   if(extraWorker&&!roomFor(taskGrowth.load()))return;
   const int task=next.fetch_add(1);
   if(task>=taskCount||stop.load())return;
   const qint64 before=residentBytes();
   // The time budget: never start a task that cannot finish, and give every
   // pass a deadline, so a pass that overruns returns its partial result.
   if(options.budgetMs>0){const int remaining=left();if(remaining<=0||(slowest.load()>0&&remaining<slowest.load())){stop=true;return;}}
   QElapsedTimer clock;clock.start();
   Outcome outcome;
   if(plan.banded()){
    const auto copy=bandImage(image,plan.bands[task],plan.scale,options.limits.border,areas,result.profile.background,invert);
    outcome.text=engine.recognize(copy,plan.scale,left(),&outcome.cutShort);
   }else outcome.text=engine.recognize(whole,plan.scale,left(),&outcome.cutShort,blocks.isEmpty()?QRect():blocks[task]);
   outcome.done=true;
   qint64 seen=slowest.load();while(clock.elapsed()>seen&&!slowest.compare_exchange_weak(seen,clock.elapsed())){}
   if(outcome.cutShort)stop=true;
   raise(taskGrowth,residentBytes()-before);
   {std::lock_guard<std::mutex> guard(lock);outcomes[task]=std::move(outcome);}
   // A banded image measures before it shares: once the first band is done,
   // as many engines start as fit in the budget at what that band cost.
   if(!extraWorker&&plan.banded()&&extra.empty()&&wanted>1&&!stop.load())startExtras(fitting(engineCost+taskGrowth.load()));
  }
 };
 if(blockParallel){
  // A single pass shares at once, while the first engine lays the page out:
  // each extra engine is reserved its own cost and two bytes a pixel of the
  // pass (measured: four engines on a 1 MP region at 2x peaked at 123 MiB).
  startExtras(fitting(engineCost+2*qint64(whole.width)*whole.height));
  blocks=first.layoutBlocks(whole,plan.scale);
  {std::lock_guard<std::mutex> guard(lock);
   if(blocks.size()<2)blocks.clear();
   taskCount=blocks.isEmpty()?1:int(blocks.size());outcomes.resize(taskCount);tasksKnown=true;}
  ready.notify_all();
 }
 work(first,false);
 for(auto& thread:pool)thread.join();
 data.release();
 result.recognition.parts=taskCount;result.recognition.threads=1+int(extra.size());

 // Joined in task order, never in the order the tasks finished: Tesseract's
 // reading order inside a band, band by band, block by block.
 if(plan.banded()){
  int previousFirst=0;
  for(int i=0;i<outcomes.size();++i){
   if(!outcomes[i].done){result.truncated=true;break;}
   previousFirst=mergeBand(result.recognition,outcomes[i].text,plan.bands[i],i>0?&plan.bands[i-1]:nullptr,previousFirst,plan.scale,options.limits.border,i,image.width,image.height);
   if(outcomes[i].cutShort){result.truncated=true;break;}
  }
 }else{
  QList<BandText> parts;
  for(const auto& outcome:outcomes){
   if(!outcome.done){result.truncated=true;break;}
   parts.append(outcome.text);
   if(outcome.cutShort){result.truncated=true;break;}
  }
  mergeBand(result.recognition,joinParts(parts),plan.bands.first(),nullptr,0,plan.scale,options.limits.border,0,image.width,image.height);
 }
 result.timings.recognitionMs=stage.elapsed();
 result.recognition.banded=plan.banded();result.recognition.scale=plan.scale;result.recognition.bands=int(plan.bands.size());
 joinUrls(result.recognition);
 applyCopyFilter(result.recognition,image,options.filter);
 result.text=copiedText(result.recognition,&result.lineTops);
 result.timings.totalMs=total.elapsed();
 return result;
}
TextResult toTextResult(const JobResult& job,const JobOptions& options){
 TextResult out;out.text=job.text;out.lineTops=job.lineTops;out.truncated=job.truncated;out.banded=job.recognition.banded;
 out.languagesUsed=job.used;out.languagesSkipped=job.skipped;
 out.engineStartMs=job.timings.engineStartMs;out.recognizeMs=job.timings.codesMs+job.timings.recognitionMs;
 const auto word=[](const Word& w){return TextWord{w.text,w.rect,int(std::lround(w.confidence))};};
 for(const auto& w:job.recognition.words){
  if(options.words&&w.kept)out.words.append(word(w));
  if(options.candidates)out.candidates.append(word(w));
 }
 for(const auto& c:job.recognition.codes){
  TextCode code;code.format=c.format;code.binary=!c.text;code.text=c.text?c.content:QString();code.length=c.length;
  code.quad={c.quad[0],c.quad[1],c.quad[2],c.quad[3]};out.codes.append(code);
 }
 return out;
}
}
