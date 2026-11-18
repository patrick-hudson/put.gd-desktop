#include "QuickRedactRunner.h"
#include <QCoreApplication>
#include <QDeadlineTimer>
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
// How long past the budget the editor waits before giving up on the worker.
// quickRedact::find stops its patterns itself (PCRE2 limits and a clock check
// between matches, see QuickRedact.cpp), so the worker normally answers within
// the budget; this is the backstop if it does not.
constexpr int graceMs=1000;
// The editor's budget for all patterns together (decision 6).
constexpr int budgetMs=2000;
}
void runQuickRedact(QObject* context,QList<editor::RecognizedWord> candidates,QStringList categories,editor::quickRedact::Sources sources,std::function<void(editor::quickRedact::Result)> done) {
 auto progress=std::make_shared<Progress>();
 const bool patterns=categories.contains(QStringLiteral("list"))&&!sources.patterns.isEmpty();
 // Given up on: every pattern is reported skipped, never the page as checked.
 QList<int> allPatterns(patterns?sources.patterns.size():0);std::iota(allPatterns.begin(),allPatterns.end(),0);
 auto* thread=QThread::create([candidates=std::move(candidates),categories=std::move(categories),sources=std::move(sources),progress,patterns]{
  // With patterns, everything else first: if the patterns outlast the
  // deadline, the editor still gets these, with every pattern skipped.
  if(patterns){
   auto quick=sources;quick.patterns.clear();
   auto first=editor::quickRedact::find(candidates,categories,quick,budgetMs);
   first.skippedPatterns.resize(sources.patterns.size());std::iota(first.skippedPatterns.begin(),first.skippedPatterns.end(),0);
   QMutexLocker lock(&progress->mutex);progress->result=std::move(first);
  }
  auto result=editor::quickRedact::find(candidates,categories,sources,budgetMs);
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
 QTimer::singleShot(budgetMs+graceMs,context,deliver);
 // At exit the process's static data goes away, matching's compiled
 // expressions among it, so a worker still running then waits out what is left
 // of its time (it stops its patterns itself within the budget) rather than
 // reading freed memory.
 if(auto* application=QCoreApplication::instance()){
  const QDeadlineTimer deadline(budgetMs+graceMs);
  QObject::connect(application,&QCoreApplication::aboutToQuit,thread,[thread,deadline]{thread->wait(deadline);});
 }
 thread->start(QThread::LowPriority);
}
}
