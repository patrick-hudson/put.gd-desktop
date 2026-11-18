#include "QuickRedactRunner.h"
#include "core/ParentDeath.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QProcessEnvironment>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <memory>
#include <numeric>
#include <optional>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#if __has_include(<linux/landlock.h>)
#include <linux/landlock.h>
#endif
#ifndef CLOSE_RANGE_CLOEXEC
#define CLOSE_RANGE_CLOEXEC (1U << 2)
#endif

namespace putgd::ui {
namespace {
// What the worker has found so far, and whether it should stop. The worker
// and the interface thread both hold it, so neither outlives the other's data:
// the worker owns its inputs, and nothing here points at the editor.
struct Progress {
 QMutex mutex;
 std::optional<editor::quickRedact::Result> result;
 std::atomic_bool cancel{false};
 bool delivered=false; // interface thread only
};
// The workers still running. Read and written on the interface thread only,
// and never destroyed, since the exit wait runs while the application object
// goes away.
QHash<QThread*,std::shared_ptr<Progress>>& live() {static auto* value=new QHash<QThread*,std::shared_ptr<Progress>>;return *value;}
QMutex& settingsMutex() {static auto* value=new QMutex;return *value;}
AccountLookup& lookup() {static auto* value=new AccountLookup([](const std::function<bool()>& stopped){return currentAccount(stopped);});return *value;}
QString& matcherProgram() {static auto* value=new QString;return *value;}
std::atomic_int starts{0};
// At exit the process's static data goes away, matching's compiled
// expressions among it. QApplication's destructor (a post routine) therefore
// stops the workers first: it sets their cancel flags, which they read after
// the password entry, before the AccountsService call and every 50 ms while
// the matcher child runs (which they then kill), and waits at most
// quickRedactExitWaitMs. A worker still running then (in a directory-service
// lookup that does not return, say) is left to end on its own.
void stopAtExit() {stopQuickRedactWorkers(QDeadlineTimer(quickRedactExitWaitMs));}
QString program() {
 QMutexLocker lock(&settingsMutex());
 if(!matcherProgram().isEmpty())return matcherProgram();
 // The editor is putgd-ui itself; anything else (a test) looks beside it.
 const QFileInfo self(QCoreApplication::applicationFilePath());
 return self.fileName()==QLatin1String("putgd-ui")?self.absoluteFilePath():self.absolutePath()+QStringLiteral("/putgd-ui");
}
// The pattern matcher in a child process, killed at `deadline` or on cancel.
// It reports each pattern as it ends, so the ones it finished survive a kill;
// the rest are missing, and find reports them skipped. Nothing about the
// patterns or the text is logged; they cross only the pipe.
QList<editor::quickRedact::PatternOutcome> matchInChild(const QStringList& subjects,const QStringList& patterns,QDeadlineTimer deadline,const std::atomic_bool& cancel) {
 QList<editor::quickRedact::PatternOutcome> out;
 if(deadline.hasExpired()||cancel.load())return out;
 QProcess process;
 process.setProcessChannelMode(QProcess::SeparateChannels);process.setStandardErrorFile(QProcess::nullDevice());
 // Cleared: the child needs nothing from the environment.
 QProcessEnvironment environment;environment.insert(QStringLiteral("LC_ALL"),QStringLiteral("C.UTF-8"));process.setProcessEnvironment(environment);
 const auto parent=::getpid();
 process.setChildProcessModifier([parent]{
  // Only async-signal-safe calls between fork and exec.
  if(!dieWithParent(parent,SIGKILL))::_exit(127);
#ifdef SYS_close_range
  ::syscall(SYS_close_range,3U,~0U,CLOSE_RANGE_CLOEXEC);
#endif
 });
 QElapsedTimer clock;clock.start();
 process.start(program(),{QStringLiteral("--match-patterns")});
 if(!process.waitForStarted(int(qBound(qint64(1),deadline.remainingTime(),qint64(2000))))){process.kill();process.waitForFinished(1000);return out;}
 ++starts;
 QJsonObject request{{"subjects",QJsonArray::fromStringList(subjects)},{"patterns",QJsonArray::fromStringList(patterns)}};
 process.write(QJsonDocument(request).toJson(QJsonDocument::Compact));process.closeWriteChannel();
 QByteArray pending;
 const auto take=[&]{
  pending+=process.readAllStandardOutput();
  for(qsizetype end;(end=pending.indexOf('\n'))>=0;){
   const auto line=QJsonDocument::fromJson(pending.left(end)).object();pending.remove(0,end+1);
   editor::quickRedact::PatternOutcome outcome;outcome.pattern=line["p"].toInt(-1);outcome.finished=line["f"].toBool();
   if(outcome.pattern<0||outcome.pattern>=patterns.size())continue;
   for(const auto& value:line["h"].toArray()){const auto hit=value.toArray();outcome.hits.append({hit.at(0).toInt(-1),qsizetype(hit.at(1).toInteger(-1)),qsizetype(hit.at(2).toInteger(-1))});}
   out.append(std::move(outcome));
  }
 };
 while(process.state()!=QProcess::NotRunning&&out.size()<patterns.size()&&!deadline.hasExpired()&&!cancel.load()){
  process.waitForReadyRead(int(qBound(qint64(1),deadline.remainingTime(),qint64(50))));
  take();
 }
 take();
 if(process.state()!=QProcess::NotRunning){process.kill();process.waitForFinished(1000);}
 return out;
}
#if defined(SYS_landlock_create_ruleset)&&defined(LANDLOCK_CREATE_RULESET_VERSION)
// Landlock with every file-system right handled and none granted: the child
// can open no file at all once its input is read. Best effort: an older kernel
// keeps the other limits.
void denyFiles() {
 const long abi=::syscall(SYS_landlock_create_ruleset,nullptr,0,LANDLOCK_CREATE_RULESET_VERSION);
 if(abi<1)return;
 landlock_ruleset_attr attribute{};
 attribute.handled_access_fs=abi>=5?(1ULL<<16)-1:abi>=3?(1ULL<<15)-1:abi>=2?(1ULL<<14)-1:(1ULL<<13)-1;
 const int fd=int(::syscall(SYS_landlock_create_ruleset,&attribute,sizeof(attribute),0));
 if(fd<0)return;
 ::syscall(SYS_landlock_restrict_self,fd,0);::close(fd);
}
#else
void denyFiles() {}
#endif
}
void setQuickRedactAccountLookup(AccountLookup replacement) {
 QMutexLocker lock(&settingsMutex());
 lookup()=replacement?std::move(replacement):AccountLookup([](const std::function<bool()>& stopped){return currentAccount(stopped);});
}
void setQuickRedactMatcherProgram(QString path) {QMutexLocker lock(&settingsMutex());matcherProgram()=std::move(path);}
int quickRedactMatcherStarts() {return starts.load();}
bool stopQuickRedactWorkers(QDeadlineTimer limit) {
 for(auto it=live().cbegin();it!=live().cend();++it)it.value()->cancel.store(true);
 bool all=true;
 for(auto it=live().cbegin();it!=live().cend();++it)all=it.key()->wait(limit)&&all;
 return all;
}
void runQuickRedact(QObject* context,QList<editor::RecognizedWord> candidates,QStringList categories,editor::quickRedact::Sources sources,std::function<void(editor::quickRedact::Result)> done,int budgetMs) {
 auto progress=std::make_shared<Progress>();
 const bool patterns=categories.contains(QStringLiteral("list"))&&!sources.patterns.isEmpty();
 // Given up on before anything was found: every pattern is reported skipped,
 // never the page as checked.
 QList<int> allPatterns(patterns?sources.patterns.size():0);std::iota(allPatterns.begin(),allPatterns.end(),0);
 AccountLookup read;{QMutexLocker lock(&settingsMutex());read=lookup();}
 // The deadline counts from here: the account lookup, matching and the child.
 const QDeadlineTimer deadline(budgetMs);
 auto* thread=QThread::create([candidates=std::move(candidates),categories=std::move(categories),sources=std::move(sources),progress,budgetMs,read=std::move(read),deadline]() mutable {
  const auto stopped=[&progress]{return progress->cancel.load();};
  // The account is read here, off the interface thread: getpwuid_r can wait on
  // a directory service and the AccountsService fallback on the system bus.
  if(categories.contains(QStringLiteral("name"))&&sources.login.isEmpty()){const auto account=read(stopped);sources.login=account.login;sources.realName=account.realName;}
  // The editor may have closed meanwhile.
  if(stopped())return;
  const editor::quickRedact::PatternMatcher matcher=[&](const QStringList& subjects,const QStringList& patterns){return matchInChild(subjects,patterns,deadline,progress->cancel);};
  auto result=editor::quickRedact::find(candidates,categories,sources,budgetMs,[&progress](const editor::quickRedact::Result& partial){
   QMutexLocker lock(&progress->mutex);progress->result=partial;
  },&progress->cancel,matcher);
  QMutexLocker lock(&progress->mutex);progress->result=std::move(result);
 });
 auto deliver=[progress,allPatterns,done=std::move(done)]{
  if(progress->delivered)return;
  progress->delivered=true;
  editor::quickRedact::Result result;result.skippedPatterns=allPatterns;
  {QMutexLocker lock(&progress->mutex);if(progress->result)result=*progress->result;}
  if(done)done(std::move(result));
 };
 // Both run on the interface thread, and only while `context` exists; the
 // thread deletes itself when it ends, whether or not anyone still waits.
 QObject::connect(thread,&QThread::finished,context,deliver);
 QObject::connect(thread,&QThread::finished,thread,&QObject::deleteLater);
 QTimer::singleShot(budgetMs+quickRedactGraceMs,context,deliver);
 static const bool registered=[]{qAddPostRoutine(stopAtExit);return true;}();
 Q_UNUSED(registered);
 live().insert(thread,progress);
 QObject::connect(thread,&QObject::destroyed,[thread]{live().remove(thread);});
 thread->start(QThread::LowPriority);
}
int runQuickRedactMatcher() {
 // Confined before reading anything: no new privileges, not dumpable, no file
 // it may write (RLIMIT_FSIZE 0) or open (Landlock), few descriptors, no core
 // and a CPU limit as a backstop to the parent's kill.
 ::prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0);::prctl(PR_SET_DUMPABLE,0,0,0,0);
 const auto limit=[](int resource,rlim_t value){struct rlimit l{value,value};::setrlimit(resource,&l);};
 limit(RLIMIT_FSIZE,0);limit(RLIMIT_CORE,0);limit(RLIMIT_NOFILE,16);limit(RLIMIT_CPU,30);
 denyFiles();
 // The request: subjects and patterns, at most 64 MiB.
 QByteArray input;char buffer[65536];
 for(;;){
  const auto count=::read(STDIN_FILENO,buffer,sizeof buffer);
  if(count<0&&errno==EINTR)continue;
  if(count<=0)break;
  input.append(buffer,count);if(input.size()>64*1024*1024)return 2;
 }
 const auto request=QJsonDocument::fromJson(input).object();
 QStringList subjects,patterns;
 for(const auto& value:request["subjects"].toArray())subjects.append(value.toString());
 for(const auto& value:request["patterns"].toArray())patterns.append(value.toString());
 // One line a pattern, written as it ends. A pattern with more hits than
 // could ever be words is reported unfinished rather than flooding the pipe.
 constexpr qsizetype maximumHits=200000;
 editor::quickRedact::matchPatterns(subjects,patterns,[](editor::quickRedact::PatternOutcome outcome){
  if(outcome.hits.size()>maximumHits){outcome.finished=false;outcome.hits.clear();}
  QJsonArray hits;for(const auto& hit:outcome.hits)hits.append(QJsonArray{hit.subject,qint64(hit.start),qint64(hit.length)});
  const auto line=QJsonDocument(QJsonObject{{"p",outcome.pattern},{"f",outcome.finished},{"h",hits}}).toJson(QJsonDocument::Compact)+'\n';
  std::fwrite(line.constData(),1,size_t(line.size()),stdout);std::fflush(stdout);
 });
 std::fflush(stdout);
 ::_exit(0);
}
}
