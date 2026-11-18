// Quick redact's runner (#1618, round 1 of #1631): matching off the interface
// thread, the account read on the worker, and the two bounds the editor relies
// on: when `done` comes and how long exit waits for a worker.
#include "ui/AccountName.h"
#include "ui/QuickRedactRunner.h"
#include "fixtures/Waits.h"
#include <QElapsedTimer>
#include <QThread>
#include <QtTest>
#include <atomic>
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
// Every start position costs just under PCRE2's limit (15 a's with (a|aa)+$,
// 2.3 ms a 1000-character search on m74): these run until the budget.
quickRedact::Sources slowPatterns(int count) {
 quickRedact::Sources sources;sources.patterns.append(R"(JIRA-\d)");
 for(int i=0;i<count;++i)sources.patterns.append("(a|aa)+$");
 return sources;
}
QString slowPage() {QString line;for(int i=0;i<1000;++i)line+=QString(15,'a')+"b ";return line+" | JIRA-3";}
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
  QVERIFY(ui::stopQuickRedactWorkers(QDeadlineTimer(5000)));
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
 // M2, U6: exit cancels the worker, which stops before its next search, so
 // the wait ends well within quickRedactExitWaitMs even mid-way through
 // patterns that would run for seconds.
 void exitCancelsBetweenSearches() {
  auto* context=new QObject;bool called=false;
  // The account lookup tells when the worker is under way; matching follows at once.
  std::atomic_bool started{false};
  ui::setQuickRedactAccountLookup([&started]{started=true;return ui::Account{QStringLiteral("someone"),{}};});
  ui::runQuickRedact(context,words(slowPage()),{"name","list"},slowPatterns(20),[&](quickRedact::Result){called=true;},5000);
  ui::setQuickRedactAccountLookup({});
  PUTGD_TRY_VERIFY(started.load());
  // The editor closes mid-way: its window (the context) goes first.
  delete context;
  QElapsedTimer clock;clock.start();
  QVERIFY(ui::stopQuickRedactWorkers(QDeadlineTimer(ui::quickRedactExitWaitMs)));
  // One search is at most 33 ms (QuickRedact.cpp); the slack is for a loaded runner.
  qInfo("exit wait after cancel: %lld ms",qint64(clock.elapsed()));
  QVERIFY2(clock.elapsed()<test::scaled(200),qPrintable(QString::number(clock.elapsed())));
  PUTGD_VERIFY_STEADY(!called,200);
 }
 // M2, U6: the timeout path itself. A worker stuck in the account lookup (a
 // directory service that does not answer) is waited for only up to the limit,
 // then left to end on its own; when the lookup returns it sees the cancel and
 // matches nothing.
 void exitStopsWaitingAtTheLimit() {
  std::atomic_bool returned{false};
  ui::setQuickRedactAccountLookup([&returned]{QThread::msleep(1500);returned=true;return ui::Account{QStringLiteral("someone"),{}};});
  std::optional<quickRedact::Result> result;
  ui::runQuickRedact(this,words("someone filed JIRA-9"),{"name","list"},slowPatterns(0),[&](quickRedact::Result r){result=std::move(r);});
  ui::setQuickRedactAccountLookup({});
  QElapsedTimer clock;clock.start();
  QVERIFY(!ui::stopQuickRedactWorkers(QDeadlineTimer(200)));
  QVERIFY2(clock.elapsed()>=200&&clock.elapsed()<test::scaled(400),qPrintable(QString::number(clock.elapsed())));
  QVERIFY(!returned);
  // Detached, it ends by itself once the lookup returns, without matching.
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(result.has_value(),5000);
  QVERIFY(returned);QVERIFY(result->matches.isEmpty());QCOMPARE(result->skippedPatterns,QList<int>{0});
 }
};
QTEST_GUILESS_MAIN(QuickRedactRunnerTest)
#include "test_quick_redact_runner.moc"
