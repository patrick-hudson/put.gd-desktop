#include <QScopeGuard>
#include "platform/wayland/SurfaceVisibility.h"
#include "platform/wayland/CaptureConnection.h"
#include <wayland-client.h>
#include <sys/socket.h>
#include <unistd.h>
// The Budgie adapter's lock-screen refusal (#1385) and the shared Wayland
// capture's visibility witness against a real headless labwc (#1275):
// WLR_BACKENDS=headless WLR_RENDERER=pixman, two outputs, and a real
// ext-session-lock client. Nothing behind a lock screen is ever captured, and
// the witness never answers from another compositor than the one captured.
// Skips when labwc is not installed.
#include "platform/budgie/BudgieSourceAdapter.h"
#include "platform/wayland/ImageCopyCapture.h"
#include "platform/wayland/WaylandOutputs.h"
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
#include "fixtures/Waits.h"

namespace putgd {
struct BudgieCaptureTestAccess {
 static void visibility(BudgieSourceAdapter& adapter,std::function<void(std::function<void(bool)>)> query){adapter.visibilityQuery_=std::move(query);}
 static void query(BudgieSourceAdapter& adapter,std::function<void(bool)> reply){querySurfaceVisible(&adapter,adapter.capture_.connection(),std::move(reply));}
};
}
using namespace putgd;
namespace {
template<typename T> bool waitFor(T condition,int ms=10000){return putgd::test::waitFor(std::move(condition),ms);}
}
class BudgieLabwcTest:public QObject {
 Q_OBJECT
 QTemporaryDir runtime_{QStringLiteral("/tmp/pgl-XXXXXX")};QString config_;
 QProcess labwc_;QString labwcProgram_,wlCopy_,wlPaste_;
 QList<CaptureFrame> layout_;
 // The keeper names itself; comm stays readable although it isn't dumpable.
 QList<int> keepersBefore_; // A real put.gd session on this machine may have its own.
 QList<int> keepers()const{
  QList<int> found;
  for(const auto& entry:QDir("/proc").entryList(QDir::Dirs)){bool numeric=false;const int pid=entry.toInt(&numeric);if(!numeric)continue;QFile comm("/proc/"+entry+"/comm");if(!comm.open(QIODevice::ReadOnly)||comm.readAll().trimmed()!="putgd-clipboard")continue;
   // CI's container has no init to reap it: an exited keeper stays listed as a zombie.
   QFile stat("/proc/"+entry+"/stat");const auto text=stat.open(QIODevice::ReadOnly)?stat.readAll():QByteArray();const auto end=text.lastIndexOf(')');if(end>=0&&text.mid(end+2,1)!="Z"&&!keepersBefore_.contains(pid))found.append(pid);}
  return found;
 }
 QByteArray paste(const QStringList& arguments={"--no-newline"}){QProcess paste;paste.start(wlPaste_,arguments);if(!waitFor([&]{return paste.state()==QProcess::NotRunning;},5000))return {};return paste.readAllStandardOutput();}
 void takeClipboard(const QString& text){QProcess copy;copy.start(wlCopy_,{text});QVERIFY(waitFor([&]{return copy.state()==QProcess::NotRunning;},5000));}
 void ownClipboard(BudgieSourceAdapter& adapter,const CopyPayload& payload){
  QVERIFY(waitFor([&]{return adapter.capabilities()["clipboard"].toObject()["available"].toBool();}));
  Error error;bool copied=false;adapter.copy(payload,[&](Error e){error=e;copied=true;});
  QVERIFY(waitFor([&]{return copied;}));QVERIFY2(!error,qPrintable(error.message));
 }
private slots:
 void initTestCase(){
  keepersBefore_=keepers();
  labwcProgram_=QStandardPaths::findExecutable("labwc");
  if(labwcProgram_.isEmpty())QSKIP("labwc is not installed");
  wlCopy_=QStandardPaths::findExecutable("wl-copy");wlPaste_=QStandardPaths::findExecutable("wl-paste");
  QVERIFY(runtime_.isValid());QFile::setPermissions(runtime_.path(),QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner);
  config_=runtime_.filePath("labwc");QDir().mkpath(config_);
  QFile rc(config_+"/rc.xml");QVERIFY(rc.open(QIODevice::WriteOnly));rc.write("<?xml version=\"1.0\"?>\n<labwc_config><core><decoration>server</decoration></core><keyboard></keyboard></labwc_config>\n");rc.close();
  auto environment=QProcessEnvironment::systemEnvironment();
  for(const auto& name:{"WAYLAND_DISPLAY","DISPLAY","XDG_CURRENT_DESKTOP"})environment.remove(name);
  environment.insert("XDG_RUNTIME_DIR",runtime_.path());environment.insert("WLR_BACKENDS","headless");environment.insert("WLR_RENDERER","pixman");
  environment.insert("WLR_HEADLESS_OUTPUTS","2");environment.insert("WLR_LIBINPUT_NO_DEVICES","1");
  labwc_.setProcessEnvironment(environment);labwc_.setProcessChannelMode(QProcess::MergedChannels);
  labwc_.start(labwcProgram_,{"-C",config_});QVERIFY(labwc_.waitForStarted(5000));
  QString socket;QVERIFY2(waitFor([&]{for(const auto& name:QDir(runtime_.path()).entryList({"wayland-*"},QDir::System)){if(!name.endsWith(".lock")){socket=name;return true;}}return false;}),labwc_.readAll().constData());
  qputenv("XDG_RUNTIME_DIR",runtime_.path().toUtf8());qputenv("WAYLAND_DISPLAY",socket.toUtf8());
  qputenv("XDG_SESSION_TYPE","wayland");qputenv("XDG_CURRENT_DESKTOP","Budgie");qunsetenv("QT_QPA_PLATFORM");
  WaylandOutputs outputs;QVERIFY(waitFor([&]{return outputs.layout().size()==2;}));layout_=outputs.layout();
 }
 void cleanupTestCase(){
  if(labwc_.state()!=QProcess::NotRunning){labwc_.terminate();if(!labwc_.waitForFinished(3000))labwc_.kill();}
 }
 void visibilityRefusesAFullConnectionBacklog(){
  QTemporaryDir directory("/tmp/pg-vis-XXXXXX");QVERIFY(directory.isValid());
  const auto path=directory.path()+"/display";QVERIFY(path.toUtf8().size()<=107);
  QProcess listener;listener.start(QCoreApplication::applicationDirPath()+"/putgd-session-lock-fixture",{"--backlog",path});
  auto stop=qScopeGuard([&]{if(listener.state()!=QProcess::NotRunning){listener.terminate();listener.waitForFinished(3000);}});
  QVERIFY(listener.waitForStarted(3000));QVERIFY(waitFor([&]{return listener.readAllStandardOutput().contains("ready");}));
  const auto previous=qgetenv("WAYLAND_DISPLAY");auto restore=qScopeGuard([&]{qputenv("WAYLAND_DISPLAY",previous);});
  qputenv("WAYLAND_DISPLAY",path.toUtf8());
  CaptureConnection connection;bool done=false,visible=true;QObject context;
  querySurfaceVisible(&context,connection,[&](bool result){visible=result;done=true;});
  QVERIFY(waitFor([&]{return done;},1000));QVERIFY(!visible);
 }
 void visibilityUsesCaptureCompositor(){
  auto* inherited=wl_display_connect(nullptr);QVERIFY(inherited);
  auto disconnect=qScopeGuard([&]{wl_display_disconnect(inherited);});
  const auto display=qgetenv("WAYLAND_DISPLAY"),socket=qgetenv("WAYLAND_SOCKET");
  auto restore=qScopeGuard([&]{qputenv("WAYLAND_DISPLAY",display);if(socket.isNull())qunsetenv("WAYLAND_SOCKET");else qputenv("WAYLAND_SOCKET",socket);});
  qputenv("WAYLAND_SOCKET",QByteArray::number(wl_display_get_fd(inherited)));
  qputenv("WAYLAND_DISPLAY",runtime_.filePath("wrong-compositor").toUtf8());
  ImageCopyCapture capture;QVERIFY(waitFor([&]{return capture.outputsAvailable();}));
  bool done=false,visible=false;QObject context;
  querySurfaceVisible(&context,capture.connection(),[&](bool result){visible=result;done=true;});
  QVERIFY(waitFor([&]{return done;},5000));QVERIFY(visible);
  QImage image;Error error;done=false;
  capture.captureOutput(layout_.first().identity,ResourceLimits{},[&](QImage result,Error failure){image=std::move(result);error=std::move(failure);done=true;});
  QVERIFY(waitFor([&]{return done;}));QVERIFY2(!error,qPrintable(error.message));QVERIFY(!image.isNull());
 }
 void anonymousInheritedConnectionRefusesRatherThanProbingAnotherSession(){
  int sockets[2];QVERIFY(::socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,sockets)==0);
  auto close=qScopeGuard([&]{::close(sockets[0]);::close(sockets[1]);});
  const auto previous=qgetenv("WAYLAND_SOCKET");auto restore=qScopeGuard([&]{if(previous.isNull())qunsetenv("WAYLAND_SOCKET");else qputenv("WAYLAND_SOCKET",previous);});
  qputenv("WAYLAND_SOCKET",QByteArray::number(sockets[0]));
  ImageCopyCapture capture;bool done=false,visible=true;QObject context;
  querySurfaceVisible(&context,capture.connection(),[&](bool result){visible=result;done=true;});
  QVERIFY(waitFor([&]{return done;},1000));QVERIFY(!visible);QVERIFY(!capture.outputsAvailable());
 }
 void captureConnectionReconnectsOnlyAfterPixelConnectionLoss_data(){QTest::addColumn<bool>("inherited");QTest::newRow("display path")<<false;QTest::newRow("inherited socket")<<true;}
 void captureConnectionReconnectsOnlyAfterPixelConnectionLoss(){
  QFETCH(bool,inherited);QTemporaryDir directory("/tmp/pg-peer-XXXXXX");QVERIFY(directory.isValid());
  const auto path=directory.filePath("display");QVERIFY(path.toUtf8().size()<=107);
  const auto display=qgetenv("WAYLAND_DISPLAY"),socket=qgetenv("WAYLAND_SOCKET");
  auto restore=qScopeGuard([&]{qputenv("WAYLAND_DISPLAY",display);if(socket.isNull())qunsetenv("WAYLAND_SOCKET");else qputenv("WAYLAND_SOCKET",socket);});
  qputenv("WAYLAND_DISPLAY",path.toUtf8());qunsetenv("WAYLAND_SOCKET");
  QProcess server;auto cleanup=qScopeGuard([&]{if(server.state()!=QProcess::NotRunning){server.terminate();server.waitForFinished(3000);}});
  const auto start=[&]{server.start(QCoreApplication::applicationDirPath()+"/putgd-session-lock-fixture",{"--listen",path});return waitFor([&]{return server.readAllStandardOutput().contains("ready");},3000);};
  QVERIFY(start());CaptureConnection source;auto* original=source.connect();QVERIFY(original);
  if(inherited)qputenv("WAYLAND_SOCKET",QByteArray::number(wl_display_get_fd(original)));
  CaptureConnection connection;auto* first=connection.connect();QVERIFY(first);wl_display_disconnect(first);wl_display_disconnect(original);
  server.terminate();QVERIFY(server.waitForFinished(3000));QVERIFY(start());
  // A witness cannot silently change servers while capture still has its pin.
  auto* wrong=connection.connect();if(wrong)wl_display_disconnect(wrong);QVERIFY(!wrong);
  connection.disconnected();auto* restarted=connection.connect();
  if(restarted)wl_display_disconnect(restarted);
  QCOMPARE(restarted!=nullptr,!inherited);
 }
 void countdownLockQueryUsesVisibility(){
  BudgieSourceAdapter adapter;QVERIFY(waitFor([&]{return adapter.capabilities()["capture"].toObject()["desktop"].toObject()["available"].toBool();}));
  QProcess locker;auto cleanup=qScopeGuard([&]{if(locker.state()!=QProcess::NotRunning){locker.terminate();locker.waitForFinished(3000);}});
  locker.start(QCoreApplication::applicationDirPath()+"/putgd-session-lock-fixture",{});QVERIFY(waitFor([&]{return locker.readAllStandardOutput().contains("locked");},5000));
  bool done=false,locked=false;adapter.sessionLocked([&](bool value){locked=value;done=true;});
  QVERIFY(waitFor([&]{return done;},5000));QVERIFY(locked);
  locker.terminate();QVERIFY(locker.waitForFinished(3000));done=false;
  adapter.sessionLocked([&](bool value){locked=value;done=true;});QVERIFY(waitFor([&]{return done;},5000));QVERIFY(!locked);
 }
 void lockDuringCopyIsRefused(){
  const QString action("desktop");BudgieSourceAdapter adapter;ImageCopyCapture probe;
  QVERIFY(waitFor([&]{return probe.outputsAvailable();}));
  QVERIFY(waitFor([&]{return adapter.capabilities()["capture"].toObject()["desktop"].toObject()["available"].toBool();}));
  QProcess locker;bool locked=false;int witnesses=0;
  auto cleanup=qScopeGuard([&]{if(locker.state()!=QProcess::NotRunning){locker.terminate();locker.waitForFinished(3000);}});
  BudgieCaptureTestAccess::visibility(adapter,[&](auto reply){
   const bool first=++witnesses==1;
   BudgieCaptureTestAccess::query(adapter,[&,first,reply=std::move(reply)](bool visible)mutable{
    if(first&&visible){
     // Deliver the successful pre-copy witness only after locking, forcing
     // the copy to overlap a lock. The second real witness must discard it.
     locker.start(QCoreApplication::applicationDirPath()+"/putgd-session-lock-fixture",{});
     locked=waitFor([&]{return locker.readAllStandardOutput().contains("locked");},5000);
    }
    reply(visible);
   });
  });
  PayloadResult result;bool done=false;
  adapter.prepareAcquisition(action)(ResourceLimits{},[&](PayloadResult value){result=std::move(value);done=true;});
  QVERIFY(waitFor([&]{return done;},10000));QVERIFY(locked);
  QCOMPARE(result.error.code,QString("session_locked"));QVERIFY(result.payload.image.isNull());QCOMPARE(witnesses,2);
 }
 void lockedDesktopCaptureIsRefused(){
  BudgieSourceAdapter adapter;
  QVERIFY(waitFor([&]{return adapter.capabilities()["capture"].toObject()["desktop"].toObject()["available"].toBool();}));
  QProcess locker;locker.start(QCoreApplication::applicationDirPath()+"/putgd-session-lock-fixture",{});
  auto cleanup=qScopeGuard([&]{if(locker.state()!=QProcess::NotRunning){locker.terminate();locker.waitForFinished(3000);}});
  QVERIFY(locker.waitForStarted(3000));
  QVERIFY(waitFor([&]{return locker.readAllStandardOutput().contains("locked");},5000));
  PayloadResult result;bool done=false;
  adapter.acquire("desktop",ResourceLimits{},[&](PayloadResult value){result=std::move(value);done=true;});
  QVERIFY(waitFor([&]{return done;},5000));
  QCOMPARE(result.error.code,QString("session_locked"));
  QVERIFY(result.payload.image.isNull());
 }
 void nothingIsKeptWhenAnotherAppOwnsTheClipboard(){
  if(wlCopy_.isEmpty()||wlPaste_.isEmpty())QSKIP("wl-clipboard is not installed");
  {BudgieSourceAdapter adapter;ownClipboard(adapter,{"text","https://put.gd/x/replaced",{},{}});if(QTest::currentTestFailed())return;takeClipboard("copied later");QVERIFY(waitFor([&]{return paste()=="copied later";},5000));}
  PUTGD_VERIFY_STEADY(keepers().isEmpty(),1000);QCOMPARE(paste(),QByteArray("copied later"));
 }
};
QTEST_GUILESS_MAIN(BudgieLabwcTest)
#include "test_budgie_labwc.moc"
