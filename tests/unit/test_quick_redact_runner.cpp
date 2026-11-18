// Quick redact's runner (#1618, round 1 of #1631): matching off the interface
// thread, the account read on the worker, and the two bounds the editor relies
// on: when `done` comes and how long exit waits for a worker.
#include "ui/AccountName.h"
#include "ui/QuickRedactRunner.h"
#include "fixtures/Waits.h"
#include <QElapsedTimer>
#include <QThread>
#include <QtTest>
#include <optional>
using namespace putgd;
using namespace putgd::editor;
namespace {
QList<RecognizedWord> words(const QString& page) {
 QList<RecognizedWord> out;int line=0,x=0;
 for(const auto& token:page.split(' ',Qt::SkipEmptyParts)){
  if(token=="|"){++line;x=0;continue;}
  out.append({token,QRect(x,line*30,int(token.size())*8,20),90,line});x+=int(token.size())*8+8;
 }
 return out;
}
QStringList found(const QList<RecognizedWord>& candidates,const quickRedact::Result& result) {
 QStringList out;
 for(const auto& match:result.matches){QStringList text;for(const int w:match.words)text.append(candidates.at(w).text);out.append(match.category+": "+text.join(' '));}
 return out;
}
// Every start position costs just under PCRE2's limit: these run until the budget.
quickRedact::Sources slowPatterns(int count) {
 quickRedact::Sources sources;sources.patterns.append(R"(JIRA-\d)");
 for(int i=0;i<count;++i)sources.patterns.append("(a+)+$");
 return sources;
}
QString slowPage() {QString line;for(int i=0;i<1000;++i)line+=QString(14,'a')+"b ";return line+" | JIRA-3";}
}
class QuickRedactRunnerTest final:public QObject {
 Q_OBJECT
private slots:
 void deliversOnTheInterfaceThread() {
  const auto candidates=words("Hammond filed JIRA-9");
  quickRedact::Sources sources;sources.terms={"Hammond"};sources.patterns={R"(JIRA-\d)"};
  std::optional<quickRedact::Result> result;QThread* thread=nullptr;
  ui::runQuickRedact(this,candidates,{"list"},sources,[&](quickRedact::Result r){result=std::move(r);thread=QThread::currentThread();});
  PUTGD_TRY_VERIFY(result.has_value());
  QCOMPARE(thread,QThread::currentThread());
  QCOMPARE(found(candidates,*result),(QStringList{"list: Hammond","list: JIRA-9"}));
  QVERIFY(ui::quickRedactWorkersEnded(QDeadlineTimer(5000)));
 }
 // M9: with no login given, the worker reads the account itself.
 void readsTheAccountOnTheWorker() {
  const auto account=ui::currentAccount();
  if(account.login.size()<3)QSKIP("this account's login is too short to match on its own");
  const auto candidates=words("cd /home/"+account.login+"/project");
  std::optional<quickRedact::Result> result;
  ui::runQuickRedact(this,candidates,{"name"},{},[&](quickRedact::Result r){result=std::move(r);});
  PUTGD_TRY_VERIFY(result.has_value());
  QCOMPARE(found(candidates,*result),QStringList{"name: /home/"+account.login+"/project"});
 }
 // M2: patterns that would run for seconds end at the budget; `done` keeps the
 // fast pattern's mark and names the rest as skipped.
 void slowPatternsEndAtTheBudget() {
  const auto candidates=words(slowPage());
  constexpr int budget=300;
  std::optional<quickRedact::Result> result;qint64 elapsed=-1;
  QElapsedTimer clock;clock.start();
  ui::runQuickRedact(this,candidates,{"list"},slowPatterns(20),[&](quickRedact::Result r){result=std::move(r);elapsed=clock.elapsed();},budget);
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(result.has_value(),5000);
  QVERIFY2(elapsed<budget+ui::quickRedactGraceMs,qPrintable(QString::number(elapsed)));
  QCOMPARE(found(candidates,*result),QStringList{"list: JIRA-3"});
  QVERIFY(result->skippedPatterns.contains(20));QVERIFY(!result->skippedPatterns.contains(0));
 }
 // M2: the exit wait (QApplication's destructor) returns once the worker ends,
 // which is within the budget plus the grace even for runaway patterns.
 void exitWaitIsBounded() {
  constexpr int budget=300;
  auto* context=new QObject;bool called=false;
  ui::runQuickRedact(context,words(slowPage()),{"list"},slowPatterns(20),[&](quickRedact::Result){called=true;},budget);
  // The editor closes at once: its window (the context) goes first.
  delete context;
  QElapsedTimer clock;clock.start();
  QVERIFY(ui::quickRedactWorkersEnded(QDeadlineTimer(QDeadlineTimer::Forever)));
  QVERIFY2(clock.elapsed()<budget+ui::quickRedactGraceMs,qPrintable(QString::number(clock.elapsed())));
  // Nothing is delivered to a context that is gone.
  PUTGD_VERIFY_STEADY(!called,200);
 }
};
QTEST_GUILESS_MAIN(QuickRedactRunnerTest)
#include "test_quick_redact_runner.moc"
