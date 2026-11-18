#include "QuickRedactRunner.h"
#include <QCoreApplication>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <memory>
#include <numeric>
#include <optional>

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
AccountLookup& lookup() {static auto* value=new AccountLookup(currentAccount);return *value;}
// At exit the process's static data goes away, matching's compiled
// expressions among it. QApplication's destructor (a post routine) therefore
// stops the workers first: it sets their cancel flags, which they read after
// the account lookup and before every search, and waits at most
// quickRedactExitWaitMs. A worker still running then (in a directory-service
// lookup that does not return, say) is left to end on its own.
void stopAtExit() {stopQuickRedactWorkers(QDeadlineTimer(quickRedactExitWaitMs));}
}
void setQuickRedactAccountLookup(AccountLookup replacement) {lookup()=replacement?std::move(replacement):AccountLookup(currentAccount);}
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
 auto read=lookup();
 auto* thread=QThread::create([candidates=std::move(candidates),categories=std::move(categories),sources=std::move(sources),progress,budgetMs,read=std::move(read)]() mutable {
  // The account is read here, off the interface thread: getpwuid_r can wait on
  // a directory service and the AccountsService fallback on the system bus.
  if(categories.contains(QStringLiteral("name"))&&sources.login.isEmpty()){const auto account=read();sources.login=account.login;sources.realName=account.realName;}
  // The editor may have closed meanwhile.
  if(progress->cancel.load())return;
  // One pass. Each stage it finishes is kept, so a deadline that passes
  // mid-way still delivers the marks found before it.
  auto result=editor::quickRedact::find(candidates,categories,sources,budgetMs,[&progress](const editor::quickRedact::Result& partial){
   QMutexLocker lock(&progress->mutex);progress->result=partial;
  },&progress->cancel);
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
}
