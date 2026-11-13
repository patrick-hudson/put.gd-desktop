#include "Recognize.h"
#include "CodeReader.h"
#include <QElapsedTimer>
#include <QFile>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <sched.h>
#include <thread>
#include <unistd.h>

namespace putgd::ocr {
namespace {
// One recognition task: a band (a banded image) or a layout block (a single pass).
struct Outcome { BandText text;bool done=false,cutShort=false,failed=false; };
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
 JobResult result;QElapsedTimer total;total.start();const auto start=std::chrono::steady_clock::now();
 if(!image.valid()||qint64(image.width)*image.height>maximumInputPixels){result.error=QStringLiteral("input_excessive");return result;}
 // Codes first, from the full image; their areas are filled in every band copy.
 QElapsedTimer stage;stage.start();
 result.recognition.codes=readCodes(image);
 QList<Quad> areas;for(const auto& code:result.recognition.codes)areas.append(code.quad);
 result.timings.codesMs=stage.elapsed();
 result.profile=measure(image,areas);
 const int threads=workerThreads(options.threads);

 // Panels: a whole screen of windows (an input of largeInputPixels or more)
 // is read window by window, column by column; anything smaller is one
 // panel, the whole image, read as before. On small windows panels hurt: a
 // chat window cut into its message list read 2.0 to 2.2% against 0.4%.
 const bool screen=qint64(image.width)*image.height>=options.limits.largeInputPixels;
 struct Panel { QRect rect;GreyView view;QList<Quad> codes;Profile profile;Plan plan;bool invert=false;QList<Band> tasks;bool split=false; };
 QList<Panel> panelList;
 for(const auto& rect:options.panels&&screen?panels(image,areas,result.profile.lineHeight):QList<QRect>{QRect(0,0,image.width,image.height)}){
  Panel p;p.rect=rect;
  p.view={image.row(rect.y())+rect.x(),rect.width(),rect.height(),image.stride};
  for(const auto& q:areas){
   Quad moved;bool inside=false;
   for(int i=0;i<4;++i){moved[i]=q[i]-rect.topLeft();inside|=rect.contains(q[i]);}
   if(inside)p.codes.append(moved);
  }
  p.profile=rect.size()==QSize(image.width,image.height)?result.profile:measure(p.view,p.codes);
  p.invert=options.invertDark&&p.profile.dark;
  panelList.append(std::move(p));
 }
 // The scale: each panel the one its text asks for, within the job's scaled
 // pixels, all lowered together towards 1x when they do not fit.
 const qint64 cap=screen?options.limits.largeScaledPixels:options.limits.scaledPixels;
 double wantedPixels=0;
 for(const auto& p:panelList){
  const double s=options.limits.forcedScale>0?options.limits.forcedScale:preferredScale(p.profile.lineHeight,options.limits.maximumScale);
  wantedPixels+=double(p.rect.width())*p.rect.height()*s*s;
 }
 const double lower=wantedPixels>double(cap)?std::sqrt(double(cap)/wantedPixels):1.0;
 for(auto& p:panelList){
  auto limits=options.limits;limits.threads=threads;limits.scaledPixels=std::numeric_limits<qint64>::max();
  const double s=options.limits.forcedScale>0?options.limits.forcedScale:preferredScale(p.profile.lineHeight,options.limits.maximumScale);
  limits.forcedScale=std::max(1.0,s*lower);
  p.plan=putgd::ocr::plan(p.rect.width(),p.rect.height(),p.profile,limits);
  if(!p.plan.error.isEmpty()){result.error=p.plan.error;return result;}
  p.tasks=p.plan.bands;
 }
 stage.restart();
 // Everything a worker thread touches lives here, on the heap. When the time
 // budget ends with a thread still inside an engine's Init or a recognition
 // that overran, the job ends without it: this is then never freed (the
 // helper leaves with _exit after its checkpoint), and that thread never
 // touches the result, the output or anything on this function's stack.
 struct Shared {
  explicit Shared(const QStringList& directories):data(directories){}
  LanguageData data;
  TextEngine first;
  std::vector<std::unique_ptr<TextEngine>> extra;
  std::mutex lock;std::condition_variable idle;
  QList<Outcome> outcomes;          // under lock, until closed
  bool closed=false;                // under lock: nothing is handed over after this
  std::atomic<bool> closing{false}; // read without the lock before a band copy
  std::atomic<int> next{0},copying{0},running{0},busy{0};
  std::atomic<qint64> slowest{0},taskGrowth{0},reserved{0};
  std::vector<qint64> taskPixels;   // scaled pixels of each task
  std::atomic<bool> stop{false};
  qint64 gateBudget=0;int budgetMs=0;int taskCount=0;
  std::chrono::steady_clock::time_point end;
  int left()const{return budgetMs>0?int(std::chrono::duration_cast<std::chrono::milliseconds>(end-std::chrono::steady_clock::now()).count()):0;}
  bool roomFor(qint64 bytes)const{return gateBudget<=0||residentBytes()+bytes<=gateBudget;}
 };
 auto* shared=new Shared(options.directories);
 shared->budgetMs=options.budgetMs;shared->end=start+std::chrono::milliseconds(options.budgetMs);
 const qint64 beforeEngine=residentBytes();
 result.error=shared->first.start(options.languages,options.strictLanguages,options.engine,result.failedLanguage);
 // What an engine costs: what the resident set rose by over its start, but
 // never under 16 MiB (English's engine measured 16 to 17 MiB on a fresh heap):
 // an engine starting in heap the job freed shows less (3 MiB in the helper on
 // a 24 MP screen of panels, whose analysis had just been freed).
 const qint64 engineCost=std::max<qint64>(16*1024*1024,residentBytes()-beforeEngine);
 result.timings.engineStartMs=stage.elapsed();
 if(!result.error.isEmpty()){delete shared;return result;}
 result.used=shared->first.used();result.skipped=shared->first.skipped();result.dataFiles=shared->data.files();
 // Decision 3: the budget grows by the size of the data files beyond English.
 qint64 memoryBudget=options.memoryBudget;
 if(memoryBudget>0)memoryBudget=std::min(std::max(options.memoryCeiling,memoryBudget),memoryBudget+shared->data.bytesBeyondEnglish());
 // The gates keep 8 MiB in reserve: what an engine allocates after its first
 // measurement (a sparse 24 MP page reached 131 MiB with four engines without it).
 shared->gateBudget=memoryBudget>0?memoryBudget-8*1024*1024:0;
 stage.restart();

 // The tasks: each panel's bands, or, for a panel read in a single pass and
 // large enough to share, strips cut at blank row gaps only. A strip boundary
 // splits no line and needs no seam, and the order stays the panel's own, top
 // to bottom, which is what page segmentation 6 reads. Every task is its own
 // band copy: inside a SetRectangle of one shared copy, Tesseract 5.3.4 read
 // the wrong rows.
 struct Task { int panel=0,index=0;qint64 pixels=0; };
 QList<Task> tasks;bool anyBanded=false;qint64 largestTask=0;
 for(int i=0;i<panelList.size();++i){
  auto& p=panelList[i];
  const qint64 scaled=qint64(std::ceil(p.rect.width()*p.plan.scale))*qint64(std::ceil(p.rect.height()*p.plan.scale));
  if(!p.plan.banded()&&threads>1&&scaled>=options.parallelPixels){
   const auto cut=strips(p.profile,p.rect.width(),p.rect.height(),threads*2);
   if(cut.size()>1){p.tasks=cut;p.split=true;}
  }
  anyBanded|=p.plan.banded();
  for(int j=0;j<p.tasks.size();++j){
   const qint64 pixels=qint64(std::ceil(p.rect.width()*p.plan.scale))*qint64(std::ceil((p.tasks[j].bottom-p.tasks[j].top)*p.plan.scale));
   tasks.append({i,j,pixels});largestTask=std::max(largestTask,pixels);
  }
 }
 // A task's band copy and scale. It reads the input mapping and this stack,
 // so a worker calls it only between raising and lowering shared->copying,
 // and never once closing is set; the end of the job waits for copying to
 // reach zero, which a copy (no I/O, bounded size) always does.
 const std::function<GreyImage(int,double&)> copyOf=[&panelList,&tasks,&options](int index,double& scale){
  const auto& t=tasks[index];const auto& p=panelList[t.panel];scale=p.plan.scale;
  return bandImage(p.view,p.tasks[t.index],p.plan.scale,options.limits.border,p.codes,p.profile.background,p.invert);
 };
 if(!options.dumpBand.isEmpty()){
  // Method comparisons only: the first task as Tesseract gets it (PGM).
  double scale=1;const auto copy=copyOf(0,scale);
  QFile dump(options.dumpBand);
  if(dump.open(QIODevice::WriteOnly)){dump.write("P5\n"+QByteArray::number(copy.width)+' '+QByteArray::number(copy.height)+"\n255\n");dump.write(copy.pixels);}
 }
 const int taskCount=int(tasks.size());
 const int wanted=taskCount>1?std::min(threads,taskCount):1;
 // A banded image or a whole screen of panels measures what its first task
 // costs before it shares: their tasks can be dense in ways a reservation by
 // size does not see (a 24 MP screen in panels reached 136 MiB sharing at once).
 const bool measureFirst=anyBanded||panelList.size()>1;
 shared->taskCount=taskCount;shared->outcomes.resize(taskCount);
 for(const auto& t:tasks)shared->taskPixels.push_back(t.pixels);
 // One worker: takes tasks until there are none, the budget or memory says
 // stop, or the job is closing. Touches only shared and its own copies.
 const auto work=[shared,copyOf](TextEngine* engine,bool extraWorker,const std::function<void()>& afterFirstTask){
  for(;;){
   if(shared->stop.load()||shared->closing.load())return;
   // The memory budget: an extra worker takes the next task only when what
   // it may grow by still fits beside what the other workers have reserved:
   // twice the largest growth a task has shown, or 12 bytes a scaled pixel of
   // that task, whichever is more (dense bands grew 15 to 20 bytes a pixel; a
   // whole screen's first panel can be tiny and its later ones large).
   const auto need=[&](int index){
    const qint64 pixels=index<shared->taskCount?shared->taskPixels[size_t(index)]:0;
    return std::max<qint64>(2*shared->taskGrowth.load(),12*pixels);
   };
   if(extraWorker&&!shared->roomFor(shared->reserved.load()+need(shared->next.load())))return;
   // Holding a task: the end of the job waits for it (within the budget).
   // Counted before the task is taken, so the end never sees every task
   // taken and none held while one is being picked up.
   shared->busy.fetch_add(1);
   struct Release { Shared* s; ~Release(){std::lock_guard<std::mutex> g(s->lock);s->busy.fetch_sub(1);s->idle.notify_all();} } release{shared};
   const int task=shared->next.fetch_add(1);
   if(task>=shared->taskCount)return;
   const qint64 reservation=need(task);shared->reserved.fetch_add(reservation);
   struct Unreserve { Shared* s;qint64 n; ~Unreserve(){s->reserved.fetch_sub(n);} } unreserve{shared,reservation};
   const qint64 before=residentBytes();
   // The time budget: never start a task that cannot finish, and give every
   // pass a deadline, so a pass that overruns returns its partial result.
   if(shared->budgetMs>0){const int remaining=shared->left();if(remaining<=0||(shared->slowest.load()>0&&remaining<shared->slowest.load())){shared->stop=true;return;}}
   const auto clock=std::chrono::steady_clock::now();
   shared->copying.fetch_add(1);
   if(shared->closing.load()){shared->copying.fetch_sub(1);return;}
   double scale=1;const auto copy=copyOf(task,scale);
   shared->copying.fetch_sub(1);
   // The deadline, read once after the copy: spent means stop, never "none".
   const int deadline=shared->budgetMs>0?shared->left():0;
   if(shared->budgetMs>0&&deadline<=0){shared->stop=true;return;}
   Outcome outcome;
   outcome.text=engine->recognize(copy,scale,deadline,&outcome.cutShort,&outcome.failed);
   outcome.done=true;
   if(outcome.failed||outcome.cutShort)shared->stop=true;
   const qint64 took=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-clock).count();
   raise(shared->slowest,took);raise(shared->taskGrowth,residentBytes()-before);
   {std::lock_guard<std::mutex> guard(shared->lock);if(shared->closed)return;shared->outcomes[task]=std::move(outcome);}
   if(afterFirstTask)afterFirstTask();
  }
 };
 std::vector<std::thread> pool;bool startedExtras=false;
 // Extra engines, each on its own thread, started at most once, from this
 // thread, and only while the time left covers twice the first engine's
 // start and the slowest task: an engine that would start too late is not
 // worth the memory. Waiting for them is bounded by the budget regardless.
 const auto startExtras=[&](int count){
  startedExtras=true;
  if(options.budgetMs>0&&shared->left()<2*result.timings.engineStartMs+shared->slowest.load()+250)return;
  for(int w=0;w<count;++w){
   shared->extra.push_back(std::make_unique<TextEngine>());auto* engine=shared->extra.back().get();
   shared->running.fetch_add(1);
   pool.emplace_back([shared,engine,work]{
    if(engine->startLike(shared->first).isEmpty())work(engine,true,{});
    std::lock_guard<std::mutex> guard(shared->lock);shared->running.fetch_sub(1);shared->idle.notify_all();
   });
  }
 };
 // How many extra engines fit in the memory budget when each costs this much.
 const auto fitting=[&](qint64 each){
  if(shared->gateBudget<=0)return wanted-1;
  const qint64 room=shared->gateBudget-residentBytes();
  return std::clamp(int(room/std::max<qint64>(1,each)),0,wanted-1);
 };
 // A banded image or a screen of panels measures before it shares: once the
 // first task is done, as many engines start as fit in the budget.
 const std::function<void()> afterFirstTask=[&]{
  // An engine with a task costs what the first one has come to since it
  // started, engine and working memory together, plus room for a task: that
  // task's growth again, and at least 8 bytes a pixel of the largest task,
  // since an engine keeps working memory sized to the largest it has read (on
  // a 24 MP screen of 104 panels, whose first was tiny, each extra engine came
  // to about 25 MiB, not 18, and three engines reached 136 MiB).
  if(measureFirst&&!startedExtras&&wanted>1&&!shared->stop.load())
   startExtras(fitting(std::max(engineCost,residentBytes()-beforeEngine)+std::max<qint64>(shared->taskGrowth.load(),8*largestTask)));
 };
 // Strips and panels read in one pass share at once: each extra engine is
 // reserved its own cost and four bytes a pixel of the largest task
 // (measured: four engines on a 1 MP region at 2x peaked at 101 to 123 MiB).
 if(!measureFirst&&wanted>1)startExtras(fitting(engineCost+4*largestTask));
 work(&shared->first,false,afterFirstTask);
 // Wait for the extra workers, never past the budget (and a second's grace
 // for a pass ending on its monitor deadline). Then close the handover: a
 // late worker's result is dropped, and once no band copy is running nothing
 // can reach this stack or the input again.
 QList<Outcome> outcomes;bool late=false;
 {
  std::unique_lock<std::mutex> guard(shared->lock);
  // Settled: no extra worker left, or none holding a task and none to take
  // (an engine still starting has nothing to give).
  const auto settled=[&]{return shared->running.load()==0||(shared->busy.load()==0&&(shared->stop.load()||shared->next.load()>=shared->taskCount));};
  if(options.budgetMs>0)shared->idle.wait_until(guard,shared->end+std::chrono::seconds(1),settled);
  else shared->idle.wait(guard,settled);
  shared->closed=true;shared->closing=true;shared->stop=true;
  late=shared->running.load()>0;
  outcomes=shared->outcomes;
 }
 while(shared->copying.load()>0)std::this_thread::yield();
 if(late){
  // Those threads are left as they are; the merge below sees their tasks as
  // missing and ends the text there, truncated.
  for(auto& thread:pool)thread.detach();
 }else{
  for(auto& thread:pool)thread.join();
  shared->data.release();
 }
 const int extraEngines=int(shared->extra.size());
 if(!late)delete shared;
 result.recognition.parts=taskCount;result.recognition.threads=1+extraEngines;result.recognition.panels=int(panelList.size());

 // An engine that failed on a task fails the job: no text is not success.
 for(const auto& outcome:outcomes)if(outcome.failed){result.error=QStringLiteral("engine_unavailable");return result;}
 // Joined in task order, never in the order the tasks finished: panel by
 // panel, and inside a panel band by band or strip by strip, each in
 // Tesseract's reading order. The first task missing or cut short ends it.
 bool ended=false;int t=0;
 for(int i=0;i<panelList.size()&&!ended;++i){
  const auto& p=panelList[i];
  Recognition part;int previousFirst=0;
  for(int j=0;j<p.tasks.size();++j,++t){
   if(!outcomes[t].done){result.truncated=true;ended=true;break;}
   previousFirst=mergeBand(part,outcomes[t].text,p.tasks[j],j>0?&p.tasks[j-1]:nullptr,previousFirst,p.plan.scale,options.limits.border,j,p.rect.width(),p.rect.height());
   if(outcomes[t].cutShort){result.truncated=true;ended=true;break;}
  }
  // Into the image's pixels; a new panel starts a new block.
  const int lineBase=int(result.recognition.lines.size());
  const int blockBase=result.recognition.lines.isEmpty()?0:result.recognition.lines.last().block+1;
  for(auto line:part.lines){line.rect.translate(p.rect.topLeft());line.block+=blockBase;result.recognition.lines.append(line);}
  for(auto word:part.words){word.rect.translate(p.rect.topLeft());word.line+=lineBase;result.recognition.words.append(std::move(word));}
  result.recognition.banded|=p.plan.banded();
  result.recognition.scale=std::max(result.recognition.scale,p.plan.scale);result.recognition.bands+=int(p.plan.bands.size());
 }
 result.timings.recognitionMs=stage.elapsed();
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
