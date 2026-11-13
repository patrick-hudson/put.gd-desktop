#include "Recognize.h"
#include "CodeReader.h"
#include <QElapsedTimer>
#include <QFile>
#include <algorithm>
#include <atomic>
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
 JobResult result;QElapsedTimer total;total.start();
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
 LanguageData data(options.directories);
 TextEngine first;
 const qint64 beforeEngine=residentBytes();
 result.error=first.start(options.languages,options.strictLanguages,options.engine,result.failedLanguage);
 const qint64 engineCost=std::max<qint64>(0,residentBytes()-beforeEngine);
 result.timings.engineStartMs=stage.elapsed();
 if(!result.error.isEmpty())return result;
 result.used=first.used();result.skipped=first.skipped();result.dataFiles=data.files();
 // Decision 3: the budget grows by the size of the data files beyond English.
 qint64 memoryBudget=options.memoryBudget;
 if(memoryBudget>0)memoryBudget=std::min(std::max(options.memoryCeiling,memoryBudget),memoryBudget+data.bytesBeyondEnglish());
 // The gates keep 8 MiB in reserve: what an engine allocates after its first
 // measurement (a sparse 24 MP page reached 131 MiB with four engines without it).
 const qint64 gateBudget=memoryBudget>0?memoryBudget-8*1024*1024:0;
 stage.restart();
 const auto left=[&]{return options.budgetMs>0?int(options.budgetMs-total.elapsed()):0;};

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
 const auto copyOf=[&](const Task& t){
  const auto& p=panelList[t.panel];
  return bandImage(p.view,p.tasks[t.index],p.plan.scale,options.limits.border,p.codes,p.profile.background,p.invert);
 };
 if(!options.dumpBand.isEmpty()){
  // Method comparisons only: the first task as Tesseract gets it (PGM).
  const auto copy=copyOf(tasks.first());
  QFile dump(options.dumpBand);
  if(dump.open(QIODevice::WriteOnly)){dump.write("P5\n"+QByteArray::number(copy.width)+' '+QByteArray::number(copy.height)+"\n255\n");dump.write(copy.pixels);}
 }
 const int taskCount=int(tasks.size());
 const int wanted=taskCount>1?std::min(threads,taskCount):1;
 std::mutex lock;
 QList<Outcome> outcomes;outcomes.resize(taskCount);
 std::atomic<int> next{0};std::atomic<qint64> slowest{0},taskGrowth{0};std::atomic<bool> stop{false};
 std::function<void(TextEngine&,bool)> work;
 const auto roomFor=[&](qint64 bytes){return gateBudget<=0||residentBytes()+bytes<=gateBudget;};
 std::vector<std::unique_ptr<TextEngine>> extra;std::vector<std::thread> pool;
 // Extra engines, each on its own thread; started at most once, from this thread.
 // Starting one is a synchronous Init that cannot be interrupted, and the
 // join waits for it: only while the time left covers twice the first
 // engine's start and the slowest task so far, so it never outlives the budget.
 const std::function<void(int)> startExtras=[&](int count){
  if(options.budgetMs>0&&left()<2*result.timings.engineStartMs+slowest.load()+250)return;
  for(int w=0;w<count;++w){
   extra.push_back(std::make_unique<TextEngine>());auto* engine=extra.back().get();
   pool.emplace_back([&,engine]{if(engine->startLike(first).isEmpty())work(*engine,true);});
  }
 };
 // How many extra engines fit in the memory budget when each costs this much.
 const auto fitting=[&](qint64 each){
  if(gateBudget<=0)return wanted-1;
  const qint64 room=gateBudget-residentBytes();
  return std::clamp(int(room/std::max<qint64>(1,each)),0,wanted-1);
 };
 work=[&](TextEngine& engine,bool extraWorker){
  for(;;){
   // The memory budget: an extra worker leaves its tasks to the others unless
   // twice the largest growth a task has shown still fits; on dense pages a
   // later band grows more than the first (139 MiB on m74 with one times).
   if(extraWorker&&!roomFor(2*taskGrowth.load()))return;
   const int task=next.fetch_add(1);
   if(task>=taskCount||stop.load())return;
   const qint64 before=residentBytes();
   // The time budget: never start a task that cannot finish, and give every
   // pass a deadline, so a pass that overruns returns its partial result.
   if(options.budgetMs>0){const int remaining=left();if(remaining<=0||(slowest.load()>0&&remaining<slowest.load())){stop=true;return;}}
   QElapsedTimer clock;clock.start();
   Outcome outcome;
   const auto copy=copyOf(tasks[task]);
   // The deadline, read once after the copy: spent means stop, never "none".
   const int deadline=options.budgetMs>0?left():0;
   if(options.budgetMs>0&&deadline<=0){stop=true;return;}
   outcome.text=engine.recognize(copy,panelList[tasks[task].panel].plan.scale,deadline,&outcome.cutShort,&outcome.failed);
   if(outcome.failed)stop=true;
   outcome.done=true;
   qint64 seen=slowest.load();while(clock.elapsed()>seen&&!slowest.compare_exchange_weak(seen,clock.elapsed())){}
   if(outcome.cutShort)stop=true;
   raise(taskGrowth,residentBytes()-before);
   {std::lock_guard<std::mutex> guard(lock);outcomes[task]=std::move(outcome);}
   // A banded image measures before it shares: once the first task is done,
   // as many engines start as fit in the budget at what that task cost.
   if(!extraWorker&&anyBanded&&extra.empty()&&wanted>1&&!stop.load())startExtras(fitting(engineCost+2*taskGrowth.load()));
  }
 };
 // Strips and panels read in one pass share at once: each extra engine is
 // reserved its own cost and four bytes a pixel of the largest task
 // (measured: four engines on a 1 MP region at 2x peaked at 101 to 123 MiB).
 if(!anyBanded&&wanted>1)startExtras(fitting(engineCost+4*largestTask));
 work(first,false);
 for(auto& thread:pool)thread.join();
 data.release();
 result.recognition.parts=taskCount;result.recognition.threads=1+int(extra.size());result.recognition.panels=int(panelList.size());

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
