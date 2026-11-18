#include "QuickRedactRunner.h"
#include "AccountName.h"
#include <QCoreApplication>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QTimer>
#include <memory>
#include <numeric>
#include <optional>

namespace putgd::ui {
namespace {
// What the worker has found so far. The worker and the interface thread both
// hold it, so neither outlives the other's data: the worker owns its inputs, and
// nothing here points at the editor.
struct Progress {
 QMutex mutex;
 std::optional<editor::quickRedact::Result> result;
 bool delivered=false; // interface thread only
};
// The workers still running, each with the time it is waited for until. Read
// and written on the interface thread only, and never destroyed: the exit
// wait below runs while the application object goes away.
QHash<QThread*,QDeadlineTimer>& live() {static auto* value=new QHash<QThread*,QDeadlineTimer>;return *value;}
// At exit the process's static data goes away, matching's compiled
// expressions among it, so QApplication's destructor (a post routine) waits for
// the workers first: at most until the last one's deadline.
void waitAtExit() {quickRedactWorkersEnded(QDeadlineTimer(QDeadlineTimer::Forever));}
}
bool quickRedactWorkersEnded(QDeadlineTimer limit) {
 bool all=true;
 for(auto it=live().cbegin();it!=live().cend();++it)all=it.key()->wait(qMin(it.value(),limit))&&all;
 return all;
}
void runQuickRedact(QObject* context,QList<editor::RecognizedWord> candidates,QStringList categories,editor::quickRedact::Sources sources,std::function<void(editor::quickRedact::Result)> done,int budgetMs) {
 auto progress=std::make_shared<Progress>();
 const bool patterns=categories.contains(QStringLiteral("list"))&&!sources.patterns.isEmpty();
 // Given up on before anything was found: every pattern is reported skipped,
 // never the page as checked.
 QList<int> allPatterns(patterns?sources.patterns.size():0);std::iota(allPatterns.begin(),allPatterns.end(),0);
 auto* thread=QThread::create([candidates=std::move(candidates),categories=std::move(categories),sources=std::move(sources),progress,budgetMs]() mutable {
  // The account is read here, off the interface thread: getpwuid_r can wait on
  // a directory service and the AccountsService fallback on the system bus.
  if(categories.contains(QStringLiteral("name"))&&sources.login.isEmpty()){const auto account=currentAccount();sources.login=account.login;sources.realName=account.realName;}
  // One pass. Each stage it finishes is kept, so a deadline that passes
  // mid-way still delivers the marks found before it.
  auto result=editor::quickRedact::find(candidates,categories,sources,budgetMs,[&progress](const editor::quickRedact::Result& partial){
   QMutexLocker lock(&progress->mutex);progress->result=partial;
  });
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
 static const bool registered=[]{qAddPostRoutine(waitAtExit);return true;}();
 Q_UNUSED(registered);
 live().insert(thread,QDeadlineTimer(budgetMs+quickRedactGraceMs));
 QObject::connect(thread,&QObject::destroyed,[thread]{live().remove(thread);});
 thread->start(QThread::LowPriority);
}
}
