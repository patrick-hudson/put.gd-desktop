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
#include <system_error>
#include <thread>
#include <unistd.h>

namespace putgd::ocr {
namespace {
// One recognition task: a band (a banded image) or a layout block (a single pass).
struct Outcome { BandText text;bool done=false,cutShort=false,failed=false;int engine=0; };
// The resident set now, from /proc/self/statm (readable under Landlock).
qint64 residentBytes(){
 QFile statm(QStringLiteral("/proc/self/statm"));
 if(!statm.open(QIODevice::ReadOnly))return 0;
 const auto fields=statm.readAll().split(' ');
 return fields.value(1).toLongLong()*::sysconf(_SC_PAGESIZE);
}
// The resident set less its shared pages (file and shmem, the input memfd's
// among them): what this process itself allocated, from /proc/self/statm.
qint64 privateBytes(){
 QFile statm(QStringLiteral("/proc/self/statm"));
 if(!statm.open(QIODevice::ReadOnly))return 0;
 const auto fields=statm.readAll().split(' ');
 return (fields.value(1).toLongLong()-fields.value(2).toLongLong())*::sysconf(_SC_PAGESIZE);
}
void raise(std::atomic<qint64>& value,qint64 seen){qint64 now=value.load();while(seen>now&&!value.compare_exchange_weak(now,seen)){}}
// Inside a code's corners (in order around it, either way); never inside a
// degenerate code, all of whose corners are on one line.
bool insideCode(const Quad& q,QPoint p){
 int sign=0;
 for(int i=0;i<4;++i){
  const QPoint a=q[size_t(i)],b=q[size_t((i+1)%4)];
  const qint64 cross=qint64(b.x()-a.x())*(p.y()-a.y())-qint64(b.y()-a.y())*(p.x()-a.x());
  if(cross==0)continue;
  const int s=cross>0?1:-1;if(sign!=0&&s!=sign)return false;sign=s;
 }
 return sign!=0;
}
}
// The CPUs this process may use: its affinity mask, lowered to its cgroup's
// CPU quota (cpu.max, rounded up). The quota is read from /sys/fs/cgroup, so
// main() counts before confinement; under Landlock only the affinity counts.
static int availableCores(){
 // The CPUs this process may run on (affinity, a container's cpuset), not the
 // machine's: glibc's hardware_concurrency counts every online CPU.
 cpu_set_t set;CPU_ZERO(&set);
 int cores=::sched_getaffinity(0,sizeof set,&set)==0?std::max(1,CPU_COUNT(&set)):int(std::max(1u,std::thread::hardware_concurrency()));
 // A CPU quota (cgroup v2 cpu.max, "quota period" or "max period") on this
 // process's cgroup or any parent: the tightest one, rounded up to whole CPUs.
 QFile self(QStringLiteral("/proc/self/cgroup"));
 if(!self.open(QIODevice::ReadOnly))return cores;
 QStringList parts;bool found=false;
 for(const auto& line:self.readAll().split('\n'))if(line.startsWith("0::/")){parts=QString::fromUtf8(line.mid(4)).split(QLatin1Char('/'),Qt::SkipEmptyParts);found=true;break;}
 if(!found||parts.contains(QLatin1String(".."))||parts.contains(QLatin1String(".")))return cores;
 // /sys/fs/cgroup may show a subtree (a bind mount, not a cgroup namespace):
 // the path is relative to the hierarchy's root, so take off the mount's own
 // root, from mountinfo ("id parent dev root mountpoint ... - cgroup2 ...").
 QFile mounts(QStringLiteral("/proc/self/mountinfo"));
 if(!mounts.open(QIODevice::ReadOnly))return cores;
 QStringList mountRoot;bool mounted=false;
 for(const auto& line:mounts.readAll().split('\n')){
  const auto fields=line.split(' ');const auto dash=fields.indexOf("-");
  if(fields.size()>4&&dash>4&&dash+1<fields.size()&&fields[4]=="/sys/fs/cgroup"&&fields[dash+1]=="cgroup2"){
   mountRoot=QString::fromUtf8(fields[3]).split(QLatin1Char('/'),Qt::SkipEmptyParts);mounted=true;
  }
 }
 if(!mounted||parts.mid(0,int(mountRoot.size()))!=mountRoot)return cores;
 parts=parts.mid(int(mountRoot.size()));
 const QString root=QStringLiteral("/sys/fs/cgroup/");
 for(;;){
  QFile max(root+parts.join(QLatin1Char('/'))+QStringLiteral("/cpu.max"));
  if(max.open(QIODevice::ReadOnly)){
   const auto fields=max.readAll().simplified().split(' ');
   bool quotaOk=false,periodOk=false;
   const qint64 quota=fields.value(0).toLongLong(&quotaOk),period=fields.value(1).toLongLong(&periodOk);
   if(quotaOk&&periodOk&&quota>0&&period>0)cores=int(std::min<qint64>(cores,std::max<qint64>(1,(quota+period-1)/period)));
  }
  if(parts.isEmpty())break;
  parts.removeLast();
 }
 return cores;
}
int workerThreads(int requested){
 return std::clamp(requested>0?requested:availableCores(),1,maximumWorkerThreads);
}
JobResult recognizeImage(const GreyView& image,const JobOptions& options){
 JobResult result;QElapsedTimer total;total.start();const auto start=std::chrono::steady_clock::now();
 // On a machine of mixed cores, this thread and every thread the job starts
 // (they inherit it) run on the faster ones; this thread's own set comes back
 // on every way out.
 struct Affinity {
  cpu_set_t before;bool kept=false;
  explicit Affinity(const QList<int>& cpus){
   if(cpus.isEmpty()||::sched_getaffinity(0,sizeof before,&before)!=0)return;
   cpu_set_t set;CPU_ZERO(&set);for(const int c:cpus)if(c>=0&&c<CPU_SETSIZE)CPU_SET(c,&set);
   kept=CPU_COUNT(&set)>0&&::sched_setaffinity(0,sizeof set,&set)==0;
  }
  ~Affinity(){if(kept)::sched_setaffinity(0,sizeof before,&before);}
 } affinity{options.cpus};
 if(!image.valid()||qint64(image.width)*image.height>maximumInputPixels){result.error=QStringLiteral("input_excessive");return result;}
 // Codes first, from the full image; their areas are filled in every band copy.
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
  // Extra engines starting now, and started so far: a task's growth is not
  // measured across an engine's start, whose memory it would count.
  std::atomic<int> starting{0},started{0};
  // The emoji scan is running: its memory is not a task's growth either.
  std::shared_ptr<const std::atomic<bool>> emojiScanning;
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
 // The first engine starts on a thread of its own while the codes are read and
 // the page is measured and planned (#1929: its start was 115 ms of a 4K
 // check on a 4-core laptop, after 200 ms of that analysis).
 qint64 engineStartMs=0,engineRise=0;
 const qint64 beforeEngine=residentBytes();
 std::thread starter;
 {
  const auto startFirst=[shared,&options,&result,&engineStartMs,&engineRise]{
   QElapsedTimer clock;clock.start();const qint64 before=privateBytes();
   result.error=shared->first.start(options.languages,options.strictLanguages,options.engine,result.failedLanguage);
   engineRise=privateBytes()-before;engineStartMs=clock.elapsed();
  };
  // A thread the system refuses: the engine starts here, before the codes, as before.
  try{starter=std::thread(startFirst);}catch(const std::system_error&){startFirst();}
 }
 struct StarterJoin { std::thread& t; ~StarterJoin(){if(t.joinable())t.join();} } starterJoin{starter};
 QElapsedTimer stage;stage.start();
 result.recognition.codes=readCodes(image);
 QList<Quad> areas;for(const auto& code:std::as_const(result.recognition.codes))areas.append(code.quad);
 result.timings.codesMs=stage.elapsed();
 // Emoji (#1748): Tesseract has no emoji, so it reads one as stray symbols or
 // glued letters. Colour glyphs are named from the templates on their own
 // thread, alongside the analysis, the engines' start and recognition (#1815:
 // on its own it held a 4K screen's job on one core for about half a second);
 // what was found replaces Tesseract's reading at the end. The scan touches
 // only the colour plane, the templates, the code areas and its own result
 // here, and every way out of this function joins it first.
 EmojiScan emoji;qint64 emojiMs=0;bool emojiInline=false;
 // Read by the workers, which may outlive this function: on the heap.
 const auto scanning=std::make_shared<std::atomic<bool>>(false);shared->emojiScanning=scanning;
 const auto scanEmoji=[&]{
  QElapsedTimer clock;clock.start();
  emoji=findEmoji(options.colour,*options.emojiTemplates,options.emoji,[&]{return options.emojiBudgetMs>0&&total.elapsed()>=options.emojiBudgetMs;});
  options.emojiTemplates->release();
  // A colour glyph inside a code (a logo in a QR code) is part of the code,
  // which is copied as its own line.
  emoji.hits.removeIf([&](const EmojiHit& hit){
   for(const auto& q:std::as_const(areas))if(insideCode(q,hit.rect.center()))return true;
   return false;
  });
  emojiMs=clock.elapsed();
  scanning->store(false);
 };
 std::thread emojiThread;
 struct Joiner { std::thread& t; ~Joiner(){if(t.joinable())t.join();} } joiner{emojiThread};
 if(options.colour.valid()&&options.emojiTemplates&&options.emojiTemplates->count()>0){
  scanning->store(true);
  // A thread the system refuses: the scan runs here, as before.
  try{emojiThread=std::thread(scanEmoji);}catch(const std::system_error&){scanEmoji();emojiInline=true;}
 }
 result.profile=measure(image,areas);
 const int threads=workerThreads(options.threads);
 // Bands and strips are cut as for at most four engines (#1812): more engines
 // share the same tasks, so the text does not change with the core count
 // above four (cut for six, 47 of the 176 corpus captures read differently).
 const int cutThreads=std::min(threads,4);

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
  for(const auto& q:std::as_const(areas)){
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
  auto limits=options.limits;limits.threads=cutThreads;limits.scaledPixels=std::numeric_limits<qint64>::max();
  const double s=options.limits.forcedScale>0?options.limits.forcedScale:preferredScale(p.profile.lineHeight,options.limits.maximumScale);
  limits.forcedScale=std::max(1.0,s*lower);
  p.plan=putgd::ocr::plan(p.rect.width(),p.rect.height(),p.profile,limits);
  if(!p.plan.error.isEmpty()){if(starter.joinable())starter.join();delete shared;result.error=p.plan.error;return result;}
  p.tasks=p.plan.bands;
 }
 stage.restart();
 if(starter.joinable())starter.join();
 // What an engine costs: what the resident set rose by over its start, but
 // never under 16 MiB (English's engine measured 16 to 17 MiB on a fresh heap):
 // an engine starting in heap the job freed shows less (3 MiB in the helper on
 // a 24 MP screen of panels, whose analysis had just been freed). Only private
 // pages count: the codes and emoji scans beside it fault in the input's
 // mapping, and counting those took a dense 4K terminal on m74 from 5.1 s
 // (the engine started alone) to 6.2 s; counting private pages, 4.9 s. What
 // the analysis allocates beside it still counts, so the cost reads high, never low.
 const qint64 engineCost=std::max<qint64>(16*1024*1024,engineRise);
 result.timings.engineStartMs=engineStartMs;
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
 // large enough to share, strips cut between text lines only (at blank row
 // gaps, or at the least-inked row between two lines). A strip boundary
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
   const auto cut=strips(p.profile,p.rect.width(),p.rect.height(),cutThreads*options.stripsPerThread);
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
 // A banded image or a whole screen of panels can be dense in ways a
 // reservation of a few bytes a pixel does not see (a 24 MP screen in panels
 // reached 136 MiB sharing at once): its extra engines start at once with a
 // reservation for the densest task measured, and more start once the first
 // task has shown what one costs (#1929: reading that task alone first left
 // three of four cores idle for 1.4 s of a dense 4K screen's 4.6 s).
 const bool dense=anyBanded||panelList.size()>1;
 shared->taskCount=taskCount;shared->outcomes.resize(taskCount);
 for(const auto& t:std::as_const(tasks))shared->taskPixels.push_back(t.pixels);
 // One worker: takes tasks until there are none, the budget or memory says
 // stop, or the job is closing. Touches only shared and its own copies.
 const auto work=[shared,copyOf](TextEngine* engine,int worker,const std::function<void()>& afterFirstTask){
  const bool extraWorker=worker>0;
  qint64 largestRead=0;  // the scaled pixels of the largest task this worker has read
  for(;;){
   if(shared->stop.load()||shared->closing.load())return;
   // The memory budget: an extra worker takes the next task only when what
   // it may grow by still fits beside what the other workers have reserved:
   // twice the largest growth a task has shown, or 12 bytes a scaled pixel of
   // that task, whichever is more (dense bands grew 15 to 20 bytes a pixel; a
   // whole screen's first panel can be tiny and its later ones large). Once
   // this worker has read a task, only the pixels beyond the largest it has
   // read count, at 12 bytes: the working memory of a task is freed into this
   // thread's own heap and reused by the next. (Counting it again left a dense
   // 1 MP terminal to one engine: 6 of its 8 strips.)
   const auto need=[&](int index){
    const qint64 pixels=index<shared->taskCount?shared->taskPixels[size_t(index)]:0;
    if(largestRead>0)return 12*std::max<qint64>(0,pixels-largestRead);
    return std::max<qint64>(2*shared->taskGrowth.load(),12*pixels);
   };
   // A worker whose next task needs nothing new adds no memory by going on,
   // whatever the resident set of the job has come to.
   if(extraWorker){const qint64 more=need(shared->next.load());if(more>0&&!shared->roomFor(shared->reserved.load()+more))return;}
   // Holding a task: the end of the job waits for it (within the budget).
   // Counted before the task is taken, so the end never sees every task
   // taken and none held while one is being picked up.
   shared->busy.fetch_add(1);
   struct Release { Shared* s; ~Release(){std::lock_guard<std::mutex> g(s->lock);s->busy.fetch_sub(1);s->idle.notify_all();} } release{shared};
   const int task=shared->next.fetch_add(1);
   if(task>=shared->taskCount)return;
   const qint64 reservation=need(task);shared->reserved.fetch_add(reservation);
   largestRead=std::max(largestRead,shared->taskPixels[size_t(task)]);
   struct Unreserve { Shared* s;qint64 n; ~Unreserve(){s->reserved.fetch_sub(n);} } unreserve{shared,reservation};
   const bool quiet=shared->starting.load()==0&&!shared->emojiScanning->load();const int startsBefore=shared->started.load();const qint64 before=residentBytes();
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
   Outcome outcome;outcome.engine=worker;
   outcome.text=engine->recognize(copy,scale,deadline,&outcome.cutShort,&outcome.failed);
   outcome.done=true;
   if(outcome.failed||outcome.cutShort)shared->stop=true;
   const qint64 took=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-clock).count();
   raise(shared->slowest,took);
   // (With seven engines starting beside a 4K screen's second task, it
   // showed 79 MiB, and every extra worker then found no room for twice that.)
   const qint64 after=residentBytes();
   if(quiet&&shared->starting.load()==0&&!shared->emojiScanning->load()&&shared->started.load()==startsBefore)raise(shared->taskGrowth,after-before);
   {std::lock_guard<std::mutex> guard(shared->lock);if(shared->closed)return;shared->outcomes[task]=std::move(outcome);}
   if(afterFirstTask)afterFirstTask();
  }
 };
 std::vector<std::thread> pool;bool toppedUp=false;
 // Extra engines, each on its own thread, started from this thread, at most
 // wanted - 1 in all, and only while the time left covers twice the first
 // engine's start and the slowest task: an engine that would start too late
 // is not worth the memory. Waiting for them is bounded by the budget regardless.
 const auto startExtras=[&](int count){
  count=std::min(count,wanted-1-int(shared->extra.size()));
  if(count<=0)return;
  if(options.budgetMs>0&&shared->left()<2*result.timings.engineStartMs+shared->slowest.load()+250)return;
  for(int w=0;w<count;++w){
   shared->extra.push_back(std::make_unique<TextEngine>());auto* engine=shared->extra.back().get();
   shared->running.fetch_add(1);shared->starting.fetch_add(1);
   const int worker=int(shared->extra.size());
   // A thread the system refuses (RLIMIT_DATA counts each stack, which the
   // memory gate does not see) ends the pool here; the engines running go on.
   try{
    pool.emplace_back([shared,engine,work,worker]{
     const bool engineStarted=engine->startLike(shared->first).isEmpty();
     shared->started.fetch_add(1);shared->starting.fetch_sub(1);
     if(engineStarted)work(engine,worker,{});
     std::lock_guard<std::mutex> guard(shared->lock);shared->running.fetch_sub(1);shared->idle.notify_all();
    });
   }catch(const std::system_error&){shared->running.fetch_sub(1);shared->starting.fetch_sub(1);shared->extra.pop_back();break;}
  }
 };
 // How many more extra engines fit in the memory budget when each costs this
 // much; an engine still starting has not shown its cost yet, so it counts as one.
 const auto fitting=[&](qint64 each){
  if(shared->gateBudget<=0)return wanted-1;
  each=std::max<qint64>(1,each);
  const qint64 room=shared->gateBudget-residentBytes()-shared->starting.load()*each;
  return std::clamp(int(room/each),0,wanted-1);
 };
 // Once the first task is done, a banded image or a screen of panels starts
 // as many more engines as now fit, by what that task cost.
 const std::function<void()> afterFirstTask=[&]{
  // An engine with a task costs what the first one has come to since it
  // started, engine and working memory together, plus room for a task: that
  // task's growth again, and at least 8 bytes a pixel of the largest task,
  // since an engine keeps working memory sized to the largest it has read (on
  // a 24 MP screen of 104 panels, whose first was tiny, each extra engine came
  // to about 25 MiB, not 18, and three engines reached 136 MiB).
  if(dense&&!toppedUp&&wanted>1&&!shared->stop.load()){toppedUp=true;
   startExtras(fitting(std::max(engineCost,residentBytes()-beforeEngine)+std::max<qint64>(shared->taskGrowth.load(),8*largestTask)));}
 };
 // Strips and panels read in one pass share at once: each extra engine is
 // reserved its own cost and four bytes a pixel of the largest task
 // (measured: four engines on a 1 MP region at 2x peaked at 101 to 123 MiB).
 // An extra engine shares the language data the first one read, so it costs
 // the first one's start less those bytes (on m74 the first took 24 MiB with
 // English, each extra about 15; counting 24 each left one core of four idle
 // on a dense 1 MP terminal).
 // A banded image or a screen of panels reserves 20 bytes a pixel of its
 // largest task for each, the most a dense band grew by (15 to 20).
 const qint64 extraEngineCost=std::max<qint64>(16*1024*1024,engineCost-shared->data.bytesHanded());
 if(wanted>1)startExtras(fitting(extraEngineCost+(dense?20:4)*largestTask));
 work(&shared->first,0,afterFirstTask);
 // Where the faint lines are looked for (#1894; read with the other second
 // reads below), for Quick redact's candidates only. A whole screen of panels is searched window by window (#1912): a faint
 // line has no grey step of 32 levels, so it can sit outside the trimmed
 // panel, below a window's last bright line. Each panel grows through its
 // window's plain background, never into another panel, and stops at the
 // window's edge: a desktop of another shade in the region would be taken for
 // ink, and no row would look blank. Anything smaller is one panel, the whole
 // image, as before.
 // readTop/readBottom: the rows its panel was read in, when the region grew past them.
 struct FaintRegion { QRect rect;GreyView view;QList<Quad> codes;Profile profile;double scale=1;bool invert=false;int readTop=0,readBottom=0; };
 QList<FaintRegion> faintRegions;
 if(options.candidates){
  if(panelList.size()==1&&panelList.first().rect==QRect(0,0,image.width,image.height)){
   const auto& p=panelList.first();
   faintRegions.append({p.rect,p.view,p.codes,p.profile,p.plan.scale,p.invert});
  }else{
   // Grown one after another, each clear of the others and of the areas
   // already grown, so no faint line is read twice (Sol on #1920).
   QList<QRect> grown;
   for(const auto& p:std::as_const(panelList)){
    QList<QRect> others=grown;for(const auto& o:std::as_const(panelList))if(&o!=&p)others.append(o.rect);
    const QRect rect=plainAround(image,p.rect,others,p.profile.background);
    grown.append(rect);
    FaintRegion r;r.rect=rect;r.view={image.row(rect.y())+rect.x(),rect.width(),rect.height(),image.stride};
    for(const auto& q:std::as_const(areas)){
     Quad moved;bool inside=false;
     for(int i=0;i<4;++i){moved[i]=q[i]-rect.topLeft();inside|=rect.contains(q[i]);}
     if(inside)r.codes.append(moved);
    }
    r.profile=measure(r.view,r.codes);r.invert=options.invertDark&&r.profile.dark;r.scale=p.plan.scale;
    r.readTop=p.rect.top()-rect.top();r.readBottom=p.rect.bottom()+1-rect.top();
    faintRegions.append(std::move(r));
   }
  }
 }
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
 }
 const int extraEngines=int(shared->extra.size());
 // The engines stay for the second reads; a late worker's are never freed.
 const auto finish=[&]{if(!late){shared->data.release();delete shared;}};
 result.recognition.parts=taskCount;result.recognition.threads=1+extraEngines;result.recognition.panels=int(panelList.size());
 result.recognition.engineTasks.fill(0,1+extraEngines);
 for(const auto& outcome:std::as_const(outcomes))if(outcome.done&&outcome.engine<=extraEngines)++result.recognition.engineTasks[outcome.engine];

 // An engine that failed on a task fails the job: no text is not success.
 for(const auto& outcome:std::as_const(outcomes))if(outcome.failed){finish();result.error=QStringLiteral("engine_unavailable");return result;}
 // Joined in task order, never in the order the tasks finished: panel by
 // panel, and inside a panel band by band or strip by strip, each in
 // Tesseract's reading order. The first task missing or cut short ends it.
 bool ended=false;int t=0;QList<int> panelWords;
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
  for(auto line:std::as_const(part.lines)){line.rect.translate(p.rect.topLeft());line.block+=blockBase;result.recognition.lines.append(line);}
  for(auto word:std::as_const(part.words)){
   word.rect.translate(p.rect.topLeft());word.line+=lineBase;
   for(auto& symbol:word.symbols)symbol.rect.translate(p.rect.topLeft());
   result.recognition.words.append(std::move(word));
  }
  result.recognition.banded|=p.plan.banded();
  result.recognition.scale=std::max(result.recognition.scale,p.plan.scale);result.recognition.bands+=int(p.plan.bands.size());
  panelWords.append(int(result.recognition.words.size()));
 }
 // Second reads, for Quick redact's candidates only (a job that asks for
 // them), each from a copy of its own, read once more by an engine:
 // - #1894: lines of faint ink alone (comment grey beside brighter text), which
 //   Tesseract's Otsu binarization turns to background, with their contrast stretched;
 // - #1912: in a grown panel, text outside the rows its panel was read in;
 // - #1911: a line holding a word that looks like a key or token, which
 //   recognition was unsure of, from a copy that measures every pixel against
 //   its local background. One misread character breaks a token, and this is a
 //   second chance; both readings stay candidates.
 // A page with none costs nothing. They go in one queue, faint and unread
 // lines first, then the token lines least confident first across the page
 // (Sol on #1920), on every engine the job has, within one budget (#1929):
 // a read the budget leaves out or cuts short adds nothing, and the check is
 // not truncated by it. Only the first read is copied.
 struct Second { int region=0;Band band;int left=0,right=0;bool faint=false; };
 QList<Second> seconds;
 if(options.candidates&&!result.truncated&&!late){
  for(int i=0;i<faintRegions.size();++i){
   const auto& p=faintRegions[i];
   // At most eight runs a region, as before.
   auto bands=faintLines(p.profile,p.rect.width(),p.rect.height(),9);
   // In a grown panel, also the text outside the rows its panel was read in (#1912).
   // All of them, before the cap below counts the bands (Sol on #1924); one is
   // left out only when a faint band holds all of it (CodeRabbit on #1924): read
   // whole otherwise, a word read twice is dropped when the faint words join.
   if(p.readBottom>p.readTop){
    const auto faintOnly=bands;
    for(const auto& u:unreadLines(p.profile,p.rect.width(),p.rect.height(),p.readTop,p.readBottom,p.rect.height()))
     if(std::none_of(faintOnly.cbegin(),faintOnly.cend(),[&](const Band& b){return b.top<=u.top&&u.bottom<=b.bottom;}))bands.append(u);
    std::sort(bands.begin(),bands.end(),[](const Band& a,const Band& b){return a.top<b.top;});
   }
   for(int b=0;b<bands.size()&&b<8;++b)seconds.append({i,bands.at(b),0,p.rect.width(),true});
  }
  struct Pending { int panel=0;TokenBand band; };
  QList<Pending> pending;
  for(int i=0;i<panelList.size();++i){
   const auto& p=panelList[i];
   QList<TokenWord> unsure;
   for(int w=i>0?panelWords[i-1]:0;w<panelWords[i];++w){
    const auto& word=result.recognition.words.at(w);
    if(uncertainToken(word.text,word.confidence))unsure.append({word.rect.translated(-p.rect.topLeft()),word.confidence});
   }
   for(const auto& t:tokenLines(unsure,p.rect.height()))pending.append({i,t});
  }
  std::stable_sort(pending.begin(),pending.end(),[](const Pending& a,const Pending& b){return a.band.confidence<b.band.confidence;});
  for(const auto& t:std::as_const(pending)){
   const auto& p=panelList[t.panel];
   // The words' run of text in the band's rows, not the screen's width (#1929).
   const auto columns=tokenColumns(p.view,t.band.band,t.band.left,t.band.right,p.profile.background,int(tokenMargin*t.band.tallest),std::max(2,int(tokenGap*t.band.tallest)));
   seconds.append({t.panel,t.band.band,columns.first,columns.second,false});
  }
 }
 // The budget: a share of the first pass's time, within the floor and the cap,
 // ending secondReadMarginMs before the job's own budget at the latest.
 qint64 secondMs=std::clamp<qint64>(qint64(double(stage.elapsed())*secondReadShare),secondReadFloorMs,secondReadCapMs);
 if(options.budgetMs>0)secondMs=std::min<qint64>(secondMs,shared->left()-secondReadMarginMs);
 // Read by the workers, which may outlive this function like the first pass's
 // (an engine overrunning its deadline): on the heap, never freed then.
 struct SecondRun {
  std::mutex lock;std::condition_variable idle;
  std::atomic<int> next{0},copying{0},running{0};
  std::atomic<bool> closing{false},stop{false};
  // The slowest read so far, in nanoseconds a scaled pixel: a read that would
  // not finish in the time left is not started (one cut short reads nothing).
  std::atomic<qint64> pace{0};
  std::vector<qint64> pixels;   // each read's scaled pixels
  bool closed=false;          // under lock: nothing is handed over after this
  int count=0;
  std::chrono::steady_clock::time_point end;
  QList<Outcome> outcomes;    // under lock, until closed
  int left()const{return int(std::chrono::duration_cast<std::chrono::milliseconds>(end-std::chrono::steady_clock::now()).count());}
 };
 QList<Outcome> secondOutcomes;
 if(!seconds.isEmpty()&&secondMs>0){
  auto* run=new SecondRun;run->count=int(seconds.size());run->outcomes.resize(run->count);
  for(const auto& s:std::as_const(seconds)){
   const double scale=s.faint?faintRegions[s.region].scale:panelList[s.region].plan.scale;
   run->pixels.push_back(qint64(std::ceil((s.right-s.left)*scale))*qint64(std::ceil((s.band.bottom-s.band.top)*scale)));
  }
  run->end=std::chrono::steady_clock::now()+std::chrono::milliseconds(secondMs);
  // A second read's copy. It reads the input and this stack, so a worker calls
  // it only between raising and lowering run->copying, and never once closing
  // is set; the end of the reads waits for copying to reach zero.
  const std::function<GreyImage(int,double&)> secondCopy=[&seconds,&faintRegions,&panelList,&options](int index,double& scale){
   const auto& s=seconds[index];
   if(s.faint){
    const auto& p=faintRegions[s.region];scale=p.scale;
    auto copy=bandImage(p.view,s.band,p.scale,options.limits.border,p.codes,p.profile.background,p.invert);
    stretchContrast(copy,p.invert?255-p.profile.background:p.profile.background);
    return copy;
   }
   const auto& p=panelList[s.region];scale=p.plan.scale;
   const GreyView columns{p.view.row(0)+s.left,s.right-s.left,p.view.height,p.view.stride};
   QList<Quad> codes;
   for(const auto& q:p.codes){Quad m=q;for(auto& c:m)c.rx()-=s.left;codes.append(m);}
   return contrastBand(columns,s.band,p.plan.scale,options.limits.border,codes);
  };
  const auto readSeconds=[run,secondCopy](TextEngine* engine,int worker){
   for(;;){
    if(run->stop.load()||run->closing.load())return;
    const int index=run->next.fetch_add(1);
    if(index>=run->count||run->left()<=0)return;
    if(run->pace.load()*run->pixels[size_t(index)]/1000000>=run->left())continue;
    run->copying.fetch_add(1);
    if(run->closing.load()){run->copying.fetch_sub(1);return;}
    double scale=1;const auto copy=secondCopy(index,scale);
    run->copying.fetch_sub(1);
    const int deadline=run->left();
    if(deadline<=0)return;
    Outcome outcome;outcome.engine=worker;
    const auto began=std::chrono::steady_clock::now();
    outcome.text=engine->recognize(copy,scale,deadline,&outcome.cutShort,&outcome.failed);
    if(!outcome.cutShort&&!outcome.failed)raise(run->pace,std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-began).count()/std::max<qint64>(1,run->pixels[size_t(index)]));
    // A failed engine stops every second read (Sol on #1904); what was read stands.
    if(outcome.failed){run->stop=true;return;}
    if(outcome.cutShort)return;
    outcome.done=true;
    std::lock_guard<std::mutex> guard(run->lock);if(run->closed)return;run->outcomes[index]=std::move(outcome);
   }
  };
  // The engines the first pass started, and, while time and memory allow, as
  // many more as the queue and the job's threads take, each on its own thread.
  const int engines=std::min(threads,run->count);
  const int readyExtras=int(std::count_if(shared->extra.cbegin(),shared->extra.cend(),[](const auto& e){return e->ready();}));
  int more=std::max(0,engines-1-readyExtras);
  if(more>0&&run->left()>2*result.timings.engineStartMs){
   const qint64 largest=*std::max_element(run->pixels.cbegin(),run->pixels.cend());
   // Within the memory budget, as an extra engine of the first pass.
   if(shared->gateBudget>0){
    const qint64 each=extraEngineCost+20*largest;
    more=std::clamp(int((shared->gateBudget-residentBytes())/std::max<qint64>(1,each)),0,more);
   }
  }else more=0;
  std::vector<std::thread> readers;
  const auto launch=[&](TextEngine* engine,bool start,int worker){
   run->running.fetch_add(1);
   try{
    readers.emplace_back([run,shared,engine,start,worker,readSeconds]{
     if(!start||engine->startLike(shared->first).isEmpty())readSeconds(engine,worker);
     std::lock_guard<std::mutex> guard(run->lock);run->running.fetch_sub(1);run->idle.notify_all();
    });
    return true;
   }catch(const std::system_error&){run->running.fetch_sub(1);return false;}
  };
  int launched=0;
  for(int w=0;w<int(shared->extra.size())&&launched<engines-1;++w){
   if(!shared->extra[size_t(w)]->ready())continue;
   if(!launch(shared->extra[size_t(w)].get(),false,w+1))break;
   ++launched;
  }
  for(int w=0;w<more;++w){
   shared->extra.push_back(std::make_unique<TextEngine>());
   if(!launch(shared->extra.back().get(),true,int(shared->extra.size()))){shared->extra.pop_back();break;}
  }
  readSeconds(&shared->first,0);
  // Every engine's read ends at its deadline; a quarter of a second's grace
  // for one ending on its monitor, never past the job's budget (the margin).
  bool secondLate=false;
  {
   std::unique_lock<std::mutex> guard(run->lock);
   run->idle.wait_until(guard,run->end+std::chrono::milliseconds(250),[&]{return run->running.load()==0;});
   run->closed=true;run->closing=true;run->stop=true;
   secondLate=run->running.load()>0;
   secondOutcomes=run->outcomes;
  }
  while(run->copying.load()>0)std::this_thread::yield();
  if(secondLate){for(auto& thread:readers)thread.detach();late=true;}
  else{for(auto& thread:readers)thread.join();delete run;}
 }
 finish();
 result.timings.recognitionMs=stage.elapsed();
 {QElapsedTimer wait;wait.start();if(emojiThread.joinable())emojiThread.join();result.timings.emojiWaitMs=emojiInline?emojiMs:wait.elapsed();}
 result.emoji=int(emoji.hits.size());result.emojiCandidates=emoji.candidates;result.emojiCut=emoji.cut;
 result.timings.emojiMs=emojiMs;
 // Emoji take the place of what Tesseract made of them.
 placeEmoji(result.recognition,emoji.hits);
 joinUrls(result.recognition);
 applyCopyFilter(result.recognition,image,options.filter);
 result.text=copiedText(result.recognition,&result.lineTops);
 // The faint lines join the words after the copied text is made: they are not
 // copied (kept stays false), and a word where one was already read is left out.
 // The tokens' second read joins last, every word of it, marked second: Quick
 // redact matches those words apart from the first read's, since the two
 // readings of one line side by side would be read as one.
 for(const bool faintPass:{true,false})for(int i=0;i<secondOutcomes.size();++i){
  const auto& s=seconds[i];const auto& outcome=secondOutcomes[i];
  if(!outcome.done||s.faint!=faintPass)continue;
  const QRect rect=s.faint?faintRegions[s.region].rect:panelList[s.region].rect;
  const double scale=s.faint?faintRegions[s.region].scale:panelList[s.region].plan.scale;
  const QPoint origin=rect.topLeft()+QPoint(s.left,0);
  Recognition part;mergeBand(part,outcome.text,s.band,nullptr,0,scale,options.limits.border,0,s.right-s.left,rect.height());
  const int lineBase=int(result.recognition.lines.size());
  const int block=result.recognition.lines.isEmpty()?0:result.recognition.lines.last().block+1;
  for(auto line:std::as_const(part.lines)){line.rect.translate(origin);line.block=block;result.recognition.lines.append(line);}
  for(auto word:std::as_const(part.words)){
   word.rect.translate(origin);word.line+=lineBase;word.kept=false;word.second=!s.faint;
   for(auto& symbol:word.symbols)symbol.rect.translate(origin);
   if(s.faint){
    const bool seen=std::any_of(result.recognition.words.cbegin(),result.recognition.words.cend(),[&](const Word& w){const auto both=w.rect.intersected(word.rect);return qint64(both.width())*both.height()*2>=qint64(word.rect.width())*word.rect.height();});
    if(seen)continue;
   }
   result.recognition.words.append(std::move(word));
  }
 }
 result.timings.totalMs=total.elapsed();
 return result;
}
TextResult toTextResult(const JobResult& job,const JobOptions& options){
 TextResult out;out.text=job.text;out.lineTops=job.lineTops;out.truncated=job.truncated;out.banded=job.recognition.banded;
 out.languagesUsed=job.used;out.languagesSkipped=job.skipped;
 out.engineStartMs=job.timings.engineStartMs;out.recognizeMs=job.timings.codesMs+job.timings.recognitionMs+job.timings.emojiWaitMs;
 const auto word=[](const Word& w){return TextWord{w.text,w.rect,int(std::lround(w.confidence)),w.second};};
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
