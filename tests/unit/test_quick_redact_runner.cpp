// Quick redact's runner (#1618, rounds 1 to 3 of #1631): matching off the
// interface thread, user patterns in a child process killed at the deadline,
// the account read on the worker, and exit.
#include "ui/AccountName.h"
#include "ui/QuickRedactRunner.h"
#include "fixtures/Waits.h"
#include <QElapsedTimer>
#include <QProcess>
#include <QThread>
#include <QTimer>
#include <QtTest>
#include <atomic>
#include <cstdio>
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
// Round 3's measured runaways: lookarounds PCRE2 does not count toward its
// limit (seconds a search on m74), and the classic nested repeat.
const QStringList runaways{R"((?:(?=.*+)){3000}\w{12})",R"((?:(?=.*+).)+\d)","(a+)+$"};
QString longLine() {QStringList filler;for(int i=0;i<120;++i)filler.append(QStringLiteral("aaaaaaa"));return filler.join(' ');}
// The real-exit child (M20): PUTGD_RUNNER_EXIT_CHILD=cooperative|stuck.
int exitChild(int argc,char** argv) {
 QCoreApplication app(argc,argv);
 ui::setQuickRedactMatcherProgram(QStringLiteral(PUTGD_UI_BINARY));
 const bool stuck=qEnvironmentVariable("PUTGD_RUNNER_EXIT_CHILD")=="stuck";
 ui::setQuickRedactAccountLookup([stuck](const std::function<bool()>& stopped){
  std::puts("lookup");std::fflush(stdout);
  if(stuck)QThread::msleep(10000);
  else{while(!stopped())QThread::msleep(5);std::puts("cancel seen");std::fflush(stdout);}
  return ui::Account{QStringLiteral("someone"),{}};
 });
 QObject context;
 ui::runQuickRedact(&context,words("someone filed JIRA-9"),{"name","list"},quickRedact::Sources{{},{},{},{R"(JIRA-\d)"}},[](quickRedact::Result){},10000);
 QTimer::singleShot(200,&app,[&app]{std::puts("quit");std::fflush(stdout);app.quit();});
 return app.exec();
}
}
class QuickRedactRunnerTest final:public QObject {
 Q_OBJECT
private slots:
 void initTestCase() {ui::setQuickRedactMatcherProgram(QStringLiteral(PUTGD_UI_BINARY));}
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
 // Round 3: the kill is the bound. Patterns that would run for seconds are
 // killed at the deadline and named; the other categories, the terms and the
 // pattern that finished first all land.
 void runawayPatternsAreKilledAtTheDeadline() {
  const auto candidates=words("Write to jane@example.com about Hammond and JIRA-5 | "+longLine()+" 7");
  quickRedact::Sources sources;sources.terms={"Hammond"};sources.patterns=QStringList{R"(JIRA-\d)"}+runaways;
  constexpr int budget=1000;
  std::optional<quickRedact::Result> result;qint64 elapsed=-1;
  QElapsedTimer clock;clock.start();
  ui::runQuickRedact(this,candidates,{"email","list"},sources,[&](quickRedact::Result r){result=std::move(r);elapsed=clock.elapsed();},budget);
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(result.has_value(),10000);
  qInfo("deadline %d ms, result after %lld ms (kill and reap: %lld ms)",budget,elapsed,elapsed-budget);
  QVERIFY2(elapsed>=budget-50&&elapsed<budget+test::scaled(500),qPrintable(QString::number(elapsed)));
  QCOMPARE(found(candidates,*result),(QStringList{"email: jane@example.com","list: Hammond","list: JIRA-5"}));
  QVERIFY(!result->skippedPatterns.contains(0));
  for(const int p:{1,2,3})QVERIFY2(result->skippedPatterns.contains(p),qPrintable(QString::number(p)));
 }
 // What starting the child costs once per run: a run with one quick pattern,
 // against the same run without patterns (which never starts it).
 void startsTheChildOnlyForPatterns() {
  const auto candidates=words("Hammond filed JIRA-9");
  quickRedact::Sources plain;plain.login=QStringLiteral("someone");plain.terms={"Hammond"};
  auto patterned=plain;patterned.patterns={R"(JIRA-\d)"};
  const int before=ui::quickRedactMatcherStarts();
  qint64 without=0,with=0;
  for(int i=0;i<10;++i){
   const bool child=i>=5;
   std::optional<quickRedact::Result> result;QElapsedTimer clock;clock.start();qint64 elapsed=0;
   ui::runQuickRedact(this,candidates,{"list","name"},child?patterned:plain,[&](quickRedact::Result r){result=std::move(r);elapsed=clock.elapsed();});
   PUTGD_TRY_VERIFY_WITH_TIMEOUT(result.has_value(),5000);
   (child?with:without)=qMax(child?with:without,elapsed);
   QCOMPARE(ui::quickRedactMatcherStarts(),before+(child?i-4:0));
  }
  qInfo("slowest run of 5 without patterns %lld ms, with one pattern %lld ms",without,with);
 }
 // A missing matcher program skips the patterns and nothing else.
 void aMissingMatcherSkipsOnlyPatterns() {
  ui::setQuickRedactMatcherProgram(QStringLiteral("/nonexistent/putgd-ui"));
  const auto candidates=words("Hammond filed JIRA-9");
  std::optional<quickRedact::Result> result;
  ui::runQuickRedact(this,candidates,{"list"},quickRedact::Sources{{},{},{"Hammond"},{R"(JIRA-\d)"}},[&](quickRedact::Result r){result=std::move(r);});
  PUTGD_TRY_VERIFY(result.has_value());
  ui::setQuickRedactMatcherProgram(QStringLiteral(PUTGD_UI_BINARY));
  QCOMPARE(found(candidates,*result),QStringList{"list: Hammond"});QCOMPARE(result->skippedPatterns,QList<int>{0});
 }
 // M2, U6: exit cancels the worker, which kills the child within 50 ms.
 void exitKillsTheChild() {
  auto* context=new QObject;bool called=false;
  std::atomic_bool started{false};
  ui::setQuickRedactAccountLookup([&started](const std::function<bool()>&){started=true;return ui::Account{QStringLiteral("someone"),{}};});
  ui::runQuickRedact(context,words(longLine()+" 7"),{"name","list"},quickRedact::Sources{{},{},{},runaways},[&](quickRedact::Result){called=true;},10000);
  ui::setQuickRedactAccountLookup({});
  PUTGD_TRY_VERIFY(started.load());
  // The editor closes mid-way: its window (the context) goes first.
  delete context;
  QElapsedTimer clock;clock.start();
  QVERIFY(ui::stopQuickRedactWorkers(QDeadlineTimer(ui::quickRedactExitWaitMs)));
  qInfo("exit wait with the child running: %lld ms",qint64(clock.elapsed()));
  QVERIFY2(clock.elapsed()<test::scaled(300),qPrintable(QString::number(clock.elapsed())));
  PUTGD_VERIFY_STEADY(!called,200);
 }
 // U6: the stop check comes between the password entry and the bus.
 void aStoppedLookupSkipsTheBus() {
  bool asked=false;
  const auto account=ui::currentAccount([&asked]{asked=true;return true;});
  QVERIFY(!account.login.isEmpty());
  // Asked only when the password entry had no full name to give.
  if(!account.realName.isEmpty())QVERIFY(!asked);else QVERIFY(asked);
 }
 // M2, U6: the timeout path itself. A worker stuck in the account lookup is
 // waited for only up to the limit, then left to end on its own; when the
 // lookup returns it sees the cancel and matches nothing.
 void exitStopsWaitingAtTheLimit() {
  std::atomic_bool returned{false};
  ui::setQuickRedactAccountLookup([&returned](const std::function<bool()>&){QThread::msleep(1500);returned=true;return ui::Account{QStringLiteral("someone"),{}};});
  std::optional<quickRedact::Result> result;
  ui::runQuickRedact(this,words("someone filed JIRA-9"),{"name","list"},quickRedact::Sources{{},{},{},{R"(JIRA-\d)"}},[&](quickRedact::Result r){result=std::move(r);});
  ui::setQuickRedactAccountLookup({});
  QElapsedTimer clock;clock.start();
  QVERIFY(!ui::stopQuickRedactWorkers(QDeadlineTimer(200)));
  QVERIFY2(clock.elapsed()>=200&&clock.elapsed()<test::scaled(400),qPrintable(QString::number(clock.elapsed())));
  QVERIFY(!returned);
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(result.has_value(),5000);
  QVERIFY(returned);QVERIFY(result->matches.isEmpty());QCOMPARE(result->skippedPatterns,QList<int>{0});
 }
 // M20: a real exit. A child process starts a run whose account lookup does
 // not return, then quits; QApplication's destructor must cancel the worker
 // and the process must end within quickRedactExitWaitMs + 500 ms, with exit
 // code 0. Without the post routine the cooperative lookup never sees the
 // cancel, so the first case fails.
 void aRealExitEndsWithinTheLimit_data() {QTest::addColumn<QString>("mode");QTest::newRow("cooperative lookup")<<"cooperative";QTest::newRow("stuck lookup")<<"stuck";}
 void aRealExitEndsWithinTheLimit() {
  QFETCH(QString,mode);
  QProcess child;auto environment=QProcessEnvironment::systemEnvironment();environment.insert("PUTGD_RUNNER_EXIT_CHILD",mode);
  child.setProcessEnvironment(environment);child.setProcessChannelMode(QProcess::SeparateChannels);
  child.start(QCoreApplication::applicationFilePath(),{});
  QVERIFY(child.waitForStarted(5000));
  QByteArray out;QElapsedTimer sinceQuit;
  PUTGD_TRY_VERIFY_WITH_TIMEOUT((out+=child.readAllStandardOutput(),out.contains("quit\n")),10000);
  sinceQuit.start();
  // waitForFinished is false for a process that already ended (the cooperative one ends at once).
  QVERIFY(child.state()==QProcess::NotRunning||child.waitForFinished(test::scaled(5000)));
  const auto ended=sinceQuit.elapsed();
  out+=child.readAllStandardOutput();
  qInfo("%s: ended %lld ms after quitting",qPrintable(mode),ended);
  QCOMPARE(child.exitStatus(),QProcess::NormalExit);QCOMPARE(child.exitCode(),0);
  QVERIFY2(ended<ui::quickRedactExitWaitMs+test::scaled(500),qPrintable(QString::number(ended)));
  if(mode=="cooperative"){QVERIFY2(out.contains("cancel seen\n"),out.constData());QVERIFY2(ended<test::scaled(300),qPrintable(QString::number(ended)));}
  else QVERIFY2(ended>=ui::quickRedactExitWaitMs-100,qPrintable(QString::number(ended)));
 }
};
int main(int argc,char** argv) {
 if(qEnvironmentVariableIsSet("PUTGD_RUNNER_EXIT_CHILD"))return exitChild(argc,argv);
 QCoreApplication app(argc,argv);
 QuickRedactRunnerTest test;
 return QTest::qExec(&test,argc,argv);
}
#include "test_quick_redact_runner.moc"
