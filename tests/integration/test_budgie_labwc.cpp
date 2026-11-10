#include <QScopeGuard>
#include "platform/wayland/SurfaceVisibility.h"
#include "platform/wayland/CaptureConnection.h"
#include <wayland-client.h>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>
// The Budgie adapter and the shared Wayland capture code against a real
// headless labwc (#1275): WLR_BACKENDS=headless WLR_RENDERER=pixman, two
// outputs, and a known-colour client. Headless results support the desktop
// acceptance rows; they never replace them. Skips when labwc is not installed.
#include "platform/budgie/BudgieSourceAdapter.h"
#include "platform/budgie/LabwcBridgeBackend.h"
#include "platform/wayland/ClipboardKeeper.h"
#include "platform/wayland/ImageCopyCapture.h"
#include "platform/wayland/WaylandOutputs.h"
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
#include "fixtures/Waits.h"
#ifdef PUTGD_TEST_IMAGE_COPY_SOURCE
#include "record/ImageCopySource.h"
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#endif
#include <cstdlib>

namespace putgd {
struct BudgieCaptureTestAccess {
 static void visibility(BudgieSourceAdapter& adapter,std::function<void(std::function<void(bool)>)> query){adapter.visibilityQuery_=std::move(query);}
 static void query(BudgieSourceAdapter& adapter,std::function<void(bool)> reply){querySurfaceVisible(&adapter,adapter.capture_.connection(),std::move(reply));}
};
}
using namespace putgd;
namespace {
const QList<QRgb> outputColours{qRgb(200,40,40),qRgb(40,40,200),qRgb(200,200,40),qRgb(40,200,200)};
template<typename T> bool waitFor(T condition,int ms=10000){return putgd::test::waitFor(std::move(condition),ms);}
QRgb rgb(const QImage& image,int x,int y){return image.pixel(x,y)|0xff000000;}
}
class BudgieLabwcTest:public QObject {
 Q_OBJECT
 QTemporaryDir runtime_{QStringLiteral("/tmp/pgl-XXXXXX")};QString config_;
 QProcess labwc_,fixture_;QString labwcProgram_,randr_,wlCopy_,wlPaste_;
 QString labwcVersion_;
 // The fixture's surfaces, by output name in WaylandOutputs order.
 QList<CaptureFrame> layout_;
 bool runRandr(const QStringList& arguments){QProcess p;p.start(randr_,arguments);return p.waitForFinished(5000)&&p.exitCode()==0;}
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
  if(QStandardPaths::findExecutable("putgd-labwc-fixture",{QCoreApplication::applicationDirPath()}).isEmpty())QSKIP("The fixture was built without LayerShellQt");
  randr_=QStandardPaths::findExecutable("wlr-randr");wlCopy_=QStandardPaths::findExecutable("wl-copy");wlPaste_=QStandardPaths::findExecutable("wl-paste");
  QVERIFY(runtime_.isValid());QFile::setPermissions(runtime_.path(),QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner);
  config_=runtime_.filePath("labwc");QDir().mkpath(config_);
  QFile rc(config_+"/rc.xml");QVERIFY(rc.open(QIODevice::WriteOnly));rc.write("<?xml version=\"1.0\"?>\n<labwc_config><core><decoration>server</decoration></core><keyboard></keyboard></labwc_config>\n");rc.close();
  {QProcess version;version.start(labwcProgram_,{"--version"});version.waitForFinished(3000);labwcVersion_=QString::fromUtf8(version.readAllStandardOutput()+version.readAllStandardError()).simplified();}
  auto environment=QProcessEnvironment::systemEnvironment();
  for(const auto& name:{"WAYLAND_DISPLAY","DISPLAY","XDG_CURRENT_DESKTOP"})environment.remove(name);
  environment.insert("XDG_RUNTIME_DIR",runtime_.path());environment.insert("WLR_BACKENDS","headless");environment.insert("WLR_RENDERER","pixman");
  environment.insert("WLR_HEADLESS_OUTPUTS","2");environment.insert("WLR_LIBINPUT_NO_DEVICES","1");
  labwc_.setProcessEnvironment(environment);labwc_.setProcessChannelMode(QProcess::MergedChannels);
  labwc_.start(labwcProgram_,{"-C",config_});QVERIFY(labwc_.waitForStarted(5000));
  QString socket;QVERIFY2(waitFor([&]{for(const auto& name:QDir(runtime_.path()).entryList({"wayland-*"},QDir::System)){if(!name.endsWith(".lock")){socket=name;return true;}}return false;}),labwc_.readAll().constData());
  qputenv("XDG_RUNTIME_DIR",runtime_.path().toUtf8());qputenv("WAYLAND_DISPLAY",socket.toUtf8());
  qputenv("XDG_SESSION_TYPE","wayland");qputenv("XDG_CURRENT_DESKTOP","Budgie");qunsetenv("QT_QPA_PLATFORM");
  // Two 1280x720 outputs side by side, HEADLESS-1 on the left (wlroots places them the other way).
  if(!randr_.isEmpty())runRandr({"--output","HEADLESS-1","--pos","0,0","--output","HEADLESS-2","--pos","1280,0"});
  auto fixtureEnvironment=QProcessEnvironment::systemEnvironment();fixtureEnvironment.insert("QT_QPA_PLATFORM","wayland");
  fixture_.setProcessEnvironment(fixtureEnvironment);fixture_.start(QCoreApplication::applicationDirPath()+"/putgd-labwc-fixture",{});
  QVERIFY(fixture_.waitForStarted(5000));
  QVERIFY2(waitFor([&]{return fixture_.readAllStandardOutput().contains("ready")||fixture_.state()!=QProcess::Running;},15000)&&fixture_.state()==QProcess::Running,fixture_.readAllStandardError().constData());
  WaylandOutputs outputs;QVERIFY(waitFor([&]{return outputs.layout().size()==2;}));layout_=outputs.layout();
  qInfo()<<"labwc"<<labwcVersion_<<"outputs"<<layout_.size();
 }
 void cleanupTestCase(){
  if(fixture_.state()!=QProcess::NotRunning){fixture_.terminate();if(!fixture_.waitForFinished(3000))fixture_.kill();}
  if(labwc_.state()!=QProcess::NotRunning){labwc_.terminate();if(!labwc_.waitForFinished(3000))labwc_.kill();}
 }
 void visibilityRefusesFullConnectionBacklogWithoutBlocking(){
  QTemporaryDir directory("/tmp/pg-vis-XXXXXX");QVERIFY(directory.isValid());
  const auto path=directory.path()+"/display";QVERIFY(path.toUtf8().size()<=107);
  QProcess listener;listener.start(QCoreApplication::applicationDirPath()+"/putgd-session-lock-fixture",{"--backlog",path});
  auto stop=qScopeGuard([&]{if(listener.state()!=QProcess::NotRunning){listener.terminate();listener.waitForFinished(3000);}});
  QVERIFY(listener.waitForStarted(3000));QVERIFY(waitFor([&]{return listener.readAllStandardOutput().contains("ready");}));
  const auto previous=qgetenv("WAYLAND_DISPLAY");auto restore=qScopeGuard([&]{qputenv("WAYLAND_DISPLAY",previous);});
  qputenv("WAYLAND_DISPLAY",path.toUtf8());
  CaptureConnection connection;bool done=false,visible=true;QObject context;QElapsedTimer elapsed;elapsed.start();
  querySurfaceVisible(&context,connection,[&](bool result){visible=result;done=true;});
  QVERIFY2(elapsed.elapsed()<1000,"Connecting must not block the event-loop thread");
  QVERIFY(waitFor([&]{return done;},1000));QVERIFY(!visible);
 }
 void visibilityAllowsSlowCompositorSetup(){
  // Pause only our private child compositor, reproducing setup queued behind
  // the first software-rendered window without consuming the frame deadline.
  const auto pid=static_cast<pid_t>(labwc_.processId());QVERIFY(pid>0);
  QVERIFY(::kill(pid,SIGSTOP)==0);
  auto resume=qScopeGuard([&]{::kill(pid,SIGCONT);});
  QTimer::singleShot(1200,this,[pid]{::kill(pid,SIGCONT);});
  CaptureConnection connection;bool done=false,visible=false;QObject context;
  querySurfaceVisible(&context,connection,[&](bool result){visible=result;done=true;});
  QVERIFY(waitFor([&]{return done;},5000));QVERIFY(visible);
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
 void lockedWindowAfterFreezeIsRefused(){
  BudgieSourceAdapter adapter;ImageCopyCapture probe;
  QVERIFY(waitFor([&]{return probe.outputsAvailable();}));
  if(!probe.windowsAvailable())QSKIP("Compositor has no window capture source");
  QProcess locker;bool locked=false;int witnesses=0;
  auto cleanup=qScopeGuard([&]{if(locker.state()!=QProcess::NotRunning){locker.terminate();locker.waitForFinished(3000);}});
  // The first witness is requested only after freezeActive succeeded. Hold
  // that continuation until our real session-lock surfaces have mapped.
  BudgieCaptureTestAccess::visibility(adapter,[&](auto reply){
   ++witnesses;locker.start(QCoreApplication::applicationDirPath()+"/putgd-session-lock-fixture",{});
   locked=waitFor([&]{return locker.readAllStandardOutput().contains("locked");},5000);
   BudgieCaptureTestAccess::query(adapter,std::move(reply));
  });
  PayloadResult result;bool done=false;
  adapter.prepareAcquisition("window")(ResourceLimits{},[&](PayloadResult value){result=std::move(value);done=true;});
  QVERIFY(waitFor([&]{return done;},10000));QVERIFY(locked);QCOMPARE(witnesses,1);
  QCOMPARE(result.error.code,QString("session_locked"));QVERIFY(result.payload.image.isNull());
 }
 void lockDuringCopyIsRefused_data(){QTest::addColumn<QString>("action");QTest::newRow("output batch")<<QString("desktop");QTest::newRow("frozen window")<<QString("window");}
 void lockDuringCopyIsRefused(){
  QFETCH(QString,action);BudgieSourceAdapter adapter;ImageCopyCapture probe;
  QVERIFY(waitFor([&]{return probe.outputsAvailable();}));
  if(action=="window"&&!probe.windowsAvailable())QSKIP("Compositor has no window capture source");
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
  QVERIFY(waitFor([&]{return done;},10000));QVERIFY(locked);QCOMPARE(witnesses,2);
  QCOMPARE(result.error.code,QString("session_locked"));QVERIFY(result.payload.image.isNull());
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
  locker.terminate();QVERIFY(locker.waitForFinished(3000));QCOMPARE(locker.exitCode(),0);
  done=false;
  adapter.acquire("desktop",ResourceLimits{},[&](PayloadResult value){result=std::move(value);done=true;});
  QVERIFY(waitFor([&]{return done;},5000));
  QVERIFY2(!result.error,qPrintable(result.error.message));QVERIFY(!result.payload.image.isNull());
 }
 void protocolsAdvertised(){
  ImageCopyCapture capture;QVERIFY(waitFor([&]{return capture.outputsAvailable();}));
  const auto protocols=capture.status()["protocols"].toObject();
  for(const char* name:{"ext_image_copy_capture_manager_v1","ext_output_image_capture_source_manager_v1","ext_foreign_toplevel_list_v1","zwlr_foreign_toplevel_manager_v1","wl_shm"})QVERIFY2(protocols.contains(QLatin1String(name)),name);
 }
 void outputPixelsExact(){
  ImageCopyCapture capture;QVERIFY(waitFor([&]{return capture.outputsAvailable();}));
  for(int i=0;i<layout_.size();++i){
   const auto& frame=layout_[i];QImage image;Error error;bool done=false;
   capture.captureOutput(frame.identity,ResourceLimits{},[&](QImage result,Error failure){image=std::move(result);error=failure;done=true;});
   QVERIFY(waitFor([&]{return done;}));QVERIFY2(!error,qPrintable(error.message));
   if(qEnvironmentVariableIsSet("PUTGD_LABWC_TEST_DUMP"))image.save(qEnvironmentVariable("PUTGD_LABWC_TEST_DUMP")+"/output-"+frame.identity+".png");
   QCOMPARE(image.size(),frame.nativeRect.size());
   QCOMPARE(rgb(image,10,10),qRgb(255,255,255));
   QCOMPARE(rgb(image,image.width()-10,10),qRgb(0,0,0));
   QCOMPARE(rgb(image,image.width()/2,image.height()-5),outputColours[i]);
  }
 }
 void toplevelsListedAndActive(){
  ImageCopyCapture capture;QVERIFY(waitFor([&]{for(const auto& w:capture.toplevels())if(w.appId=="putgd-labwc-fixture")return true;return false;}));
  // The list itself is active-aware without the toplevel capture source.
  bool listedActive=false;for(const auto& w:capture.toplevels())listedActive|=w.appId=="putgd-labwc-fixture"&&w.activated;QVERIFY(listedActive);
  ImageCopyCapture::Toplevel active;Error error;bool done=false;
  capture.freezeActive([&](ImageCopyCapture::Toplevel window,Error failure){active=window;error=failure;done=true;});
  QVERIFY(waitFor([&]{return done;}));
  // labwc below 0.20 has no toplevel capture source: freezing a window target is refused up front.
  if(!capture.windowsAvailable()){QCOMPARE(error.code,QString("window_unavailable"));return;}
  QVERIFY2(!error,qPrintable(error.message));
  QCOMPARE(active.appId,QString("putgd-labwc-fixture"));QCOMPARE(active.title,QString("fixture-window"));QVERIFY(!active.identifier.isEmpty());
 }
 void windowCaptureOrReason(){
  BudgieSourceAdapter adapter;ImageCopyCapture probe;QVERIFY(waitFor([&]{return probe.outputsAvailable();}));
  QString reason;const bool windows=probe.windowsAvailable(&reason);
  PayloadResult result;bool done=false;
  adapter.prepareAcquisition("window")(ResourceLimits{},[&](PayloadResult r){result=std::move(r);done=true;});
  QVERIFY(waitFor([&]{return done;}));
  if(!windows){
   // labwc below 0.20: the reason names the missing protocol.
   QVERIFY(reason.contains("ext-foreign-toplevel-image-capture-source"));
   QCOMPARE(result.error.code,QString("window_unavailable"));
   QVERIFY(!adapter.capabilities()["capture"].toObject()["window"].toObject()["available"].toBool());
   return;
  }
  QVERIFY2(!result.error,qPrintable(result.error.message));
  // Client content only: the server-side title bar is not part of it.
  QCOMPARE(result.payload.image.size(),QSize(240,160));
  for(const auto& point:{QPoint(0,0),QPoint(239,0),QPoint(120,80),QPoint(0,159),QPoint(239,159)})QCOMPARE(rgb(result.payload.image,point.x(),point.y()),qRgb(0,160,0));
 }
 void desktopComposite(){
  BudgieSourceAdapter adapter;QVERIFY(waitFor([&]{return adapter.capabilities()["capture"].toObject()["desktop"].toObject()["available"].toBool();}));
  PayloadResult result;bool done=false;
  adapter.prepareAcquisition("desktop")(ResourceLimits{},[&](PayloadResult r){result=std::move(r);done=true;});
  QVERIFY(waitFor([&]{return done;}));QVERIFY2(!result.error,qPrintable(result.error.message));
  QRectF bounds;for(const auto& frame:layout_)bounds=bounds.united(frame.logicalRect);
  QCOMPARE(QSizeF(result.payload.image.size()),bounds.size());
  for(int i=0;i<layout_.size();++i){const auto r=layout_[i].logicalRect.toRect().translated(-bounds.topLeft().toPoint());QCOMPARE(rgb(result.payload.image,r.left()+10,r.top()+10),qRgb(255,255,255));QCOMPARE(rgb(result.payload.image,r.center().x(),r.bottom()-4),outputColours[i]);}
  QCOMPARE(adapter.capabilities()["adapter"].toString(),QString("budgie-wayland"));
 }
 void clipboardBothWays(){
  if(wlCopy_.isEmpty()||wlPaste_.isEmpty())QSKIP("wl-clipboard is not installed");
  BudgieSourceAdapter adapter;QVERIFY(waitFor([&]{return adapter.capabilities()["clipboard"].toObject()["available"].toBool();}));
  QProcess primary;primary.start(wlPaste_,{"--primary","--no-newline"});primary.waitForFinished(3000);const auto primaryBefore=primary.readAllStandardOutput();
  // put.gd -> a separate client.
  Error copyError;bool copied=false;adapter.copy({"text","https://put.gd/x/labwc-test",{},{}},[&](Error e){copyError=e;copied=true;});
  QVERIFY(waitFor([&]{return copied;}));QVERIFY2(!copyError,qPrintable(copyError.message));
  // put.gd serves the data from this event loop, so wait without blocking it.
  QProcess paste;paste.start(wlPaste_,{"--no-newline"});QVERIFY(waitFor([&]{return paste.state()==QProcess::NotRunning;},5000));QCOMPARE(paste.readAllStandardOutput(),QByteArray("https://put.gd/x/labwc-test"));
  // A separate client -> put.gd, while put.gd has no focus at all.
  QProcess copy;copy.start(wlCopy_,{"labwc import text"});QVERIFY(waitFor([&]{return copy.state()==QProcess::NotRunning;},5000));
  QVERIFY(waitFor([&]{return adapter.clipboardRevision()>0;}));
  PayloadResult imported;bool done=false;adapter.prepareAcquisition("clipboard")(ResourceLimits{},[&](PayloadResult r){imported=std::move(r);done=true;});
  QVERIFY(waitFor([&]{return done;}));QVERIFY2(!imported.error,qPrintable(imported.error.message));QCOMPARE(QString::fromUtf8(imported.payload.bytes),QString("labwc import text"));
  primary.start(wlPaste_,{"--primary","--no-newline"});QVERIFY(waitFor([&]{return primary.state()==QProcess::NotRunning;},5000));QCOMPARE(primary.readAllStandardOutput(),primaryBefore);
 }
 void clipboardOutlivesTheResident(){
  if(wlCopy_.isEmpty()||wlPaste_.isEmpty())QSKIP("wl-clipboard is not installed");
  QVERIFY(keepers().isEmpty());
  {BudgieSourceAdapter adapter;ownClipboard(adapter,{"text","https://put.gd/x/after-quit",{},{}});if(QTest::currentTestFailed())return;}
  QVERIFY(waitFor([&]{return paste()=="https://put.gd/x/after-quit";},5000));
  QCOMPARE(keepers().size(),1);
  const auto types=paste({"--list-types"});
  for(const char* type:{"text/plain","UTF8_STRING","text/plain;charset=utf-8","application/x-putgd-result-"})QVERIFY2(types.contains(type),types.constData());
  // Another app copies: the keeper leaves, with its content.
  takeClipboard("another app");
  QVERIFY(waitFor([&]{return keepers().isEmpty();},3000));QCOMPARE(paste(),QByteArray("another app"));
 }
 void aCopyStillInFlightOutlivesTheResident(){
  if(wlCopy_.isEmpty()||wlPaste_.isEmpty())QSKIP("wl-clipboard is not installed");
  // Review: the adapter goes away before the compositor has announced the copy.
  Error copyError;
  {BudgieSourceAdapter adapter;QVERIFY(waitFor([&]{return adapter.capabilities()["clipboard"].toObject()["available"].toBool();}));adapter.copy({"text","https://put.gd/x/in-flight",{},{}},[&](Error e){copyError=e;});}
  QVERIFY(waitFor([&]{return paste()=="https://put.gd/x/in-flight";},5000));QVERIFY2(!copyError,qPrintable(copyError.message));
  takeClipboard("after the in-flight copy");QVERIFY(waitFor([&]{return keepers().isEmpty();},3000));
 }
 void anImageOutlivesTheResident(){
  if(wlCopy_.isEmpty()||wlPaste_.isEmpty())QSKIP("wl-clipboard is not installed");
  // Larger than a pipe's buffer, so a reader takes it in several writes.
  QByteArray png("\x89PNG\r\n\x1a\n");png.append(QByteArray(3*1024*1024,'\x5a'));
  CopyPayload payload{"image",{},{},{}};payload.encodedImage=png;
  {BudgieSourceAdapter adapter;ownClipboard(adapter,payload);if(QTest::currentTestFailed())return;}
  QVERIFY(waitFor([&]{return paste({"--no-newline","--type","image/png"})==png;},5000));
  takeClipboard("after the image");QVERIFY(waitFor([&]{return keepers().isEmpty();},3000));
 }
 void nothingIsKeptWhenAnotherAppOwnsTheClipboard(){
  if(wlCopy_.isEmpty()||wlPaste_.isEmpty())QSKIP("wl-clipboard is not installed");
  {BudgieSourceAdapter adapter;ownClipboard(adapter,{"text","https://put.gd/x/replaced",{},{}});if(QTest::currentTestFailed())return;takeClipboard("copied later");QVERIFY(waitFor([&]{return paste()=="copied later";},5000));}
  PUTGD_VERIFY_STEADY(keepers().isEmpty(),1000);QCOMPARE(paste(),QByteArray("copied later"));
 }
 void aClipboardManagerIsLeftToIt(){
  if(wlCopy_.isEmpty()||wlPaste_.isEmpty())QSKIP("wl-clipboard is not installed");
  // What clipman and wl-clip-persist do: restore the selection when its owner goes.
  QProcess manager;manager.start(wlPaste_,{"--watch","sh","-c",QString("cat >/dev/null; [ \"$CLIPBOARD_STATE\" = nil ] && '%1' restored-by-manager; true").arg(wlCopy_)});QVERIFY(manager.waitForStarted(3000));
  {BudgieSourceAdapter adapter;ownClipboard(adapter,{"text","https://put.gd/x/managed",{},{}});if(QTest::currentTestFailed()){manager.kill();manager.waitForFinished(3000);return;}QVERIFY(waitFor([&]{return paste()=="https://put.gd/x/managed";},5000));}
  const bool restored=waitFor([&]{return paste()=="restored-by-manager";},5000);
  const bool gone=waitFor([&]{return keepers().isEmpty();},3000);
  manager.kill();manager.waitForFinished(3000);
  QVERIFY(restored);QVERIFY(gone);
 }
 void anOversizedResultIsNotKept(){
  QVERIFY(keepers().isEmpty());
  const QHash<QString,QByteArray> data{{"image/png",QByteArray(65*1024*1024,'x')},{"application/x-putgd-result-x",{}}};
  QVERIFY(!startClipboardKeeper(data));
  PUTGD_VERIFY_STEADY(keepers().isEmpty(),500);
 }
 void rotatedAndFractionalOutputs_data(){
  QTest::addColumn<QString>("transform");QTest::addColumn<int>("rotation");
  for(const auto& [name,degrees]:std::initializer_list<std::pair<const char*,int>>{{"90",90},{"180",180},{"270",270},{"flipped",0},{"flipped-90",90}})QTest::newRow(name)<<QString::fromLatin1(name)<<degrees;
 }
 void rotatedAndFractionalOutputs(){
  if(randr_.isEmpty())QSKIP("wlr-randr is not installed");
  QFETCH(QString,transform);QFETCH(int,rotation);
  const auto name=layout_.first().identity;
  QVERIFY(runRandr({"--output",name,"--transform",transform}));
  auto restore=qScopeGuard([&]{runRandr({"--output",name,"--transform","normal","--scale","1"});runRandr({"--output","HEADLESS-1","--pos","0,0","--output","HEADLESS-2","--pos","1280,0"});putgd::test::settle(500);});
  WaylandOutputs outputs;QVERIFY(waitFor([&]{for(const auto& f:outputs.layout())if(f.identity==name&&f.rotation==rotation)return true;return false;}));
  ImageCopyCapture capture;QVERIFY(waitFor([&]{return capture.outputsAvailable();}));
  auto shoot=[&](QImage& image){Error error;bool done=false;capture.captureOutput(name,ResourceLimits{},[&](QImage r,Error e){image=std::move(r);error=e;done=true;});return waitFor([&]{return done;})&&!error;};
  // The fixture redraws its turned background; wait until the capture shows it.
  QImage image;QVERIFY(waitFor([&]{return shoot(image)&&rgb(image,image.width()/2,image.height()-5)!=qRgb(0,0,0);}));
  if(qEnvironmentVariableIsSet("PUTGD_LABWC_TEST_DUMP"))image.save(qEnvironmentVariable("PUTGD_LABWC_TEST_DUMP")+"/transform-"+transform+".png");
  CaptureFrame frame;for(const auto& f:outputs.layout())if(f.identity==name)frame=f;
  QCOMPARE(image.size(),frame.nativeRect.size());
  // Upright: the white mark is at the logical top-left, the black at the top-right.
  QCOMPARE(rgb(image,10,10),qRgb(255,255,255));QCOMPARE(rgb(image,image.width()-10,10),qRgb(0,0,0));
  if(transform!="90")return;
  QVERIFY(runRandr({"--output",name,"--transform","normal","--scale","1.5"}));
  QVERIFY(waitFor([&]{for(const auto& f:outputs.layout())if(f.identity==name&&f.rotation==0&&qAbs(f.density-1.5)<0.01)return true;return false;}));
  QVERIFY(waitFor([&]{return shoot(image)&&rgb(image,90,90)==qRgb(255,255,255)&&rgb(image,100,100)!=qRgb(255,255,255);}));for(const auto& f:outputs.layout())if(f.identity==name)frame=f;
  QCOMPARE(image.size(),frame.nativeRect.size());
  // The 64-logical-pixel mark covers 96 buffer pixels at 1.5.
  QCOMPARE(rgb(image,90,90),qRgb(255,255,255));QCOMPARE(rgb(image,100,100),outputColours[0]);
 }
 void recordingFramesAreUpright_data(){QTest::addColumn<QString>("transform");QTest::addColumn<int>("rotation");QTest::newRow("90")<<"90"<<90;QTest::newRow("flipped-90")<<"flipped-90"<<90;}
 void recordingFramesAreUpright(){
#ifndef PUTGD_TEST_IMAGE_COPY_SOURCE
  QSKIP("built without GStreamer");
#else
  // The recorder turns a rotated output's frames upright, as the screenshot path does (review #1275).
  if(randr_.isEmpty())QSKIP("wlr-randr is not installed");
  QFETCH(QString,transform);QFETCH(int,rotation);
  gst_init(nullptr,nullptr);
  const auto name=layout_.first().identity;
  QVERIFY(runRandr({"--output",name,"--transform",transform}));
  auto restore=qScopeGuard([&]{runRandr({"--output",name,"--transform","normal"});runRandr({"--output","HEADLESS-1","--pos","0,0","--output","HEADLESS-2","--pos","1280,0"});putgd::test::settle(500);});
  WaylandOutputs outputs;QVERIFY(waitFor([&]{for(const auto& f:outputs.layout())if(f.identity==name&&f.rotation==rotation)return true;return false;}));
  CaptureFrame frame;for(const auto& f:outputs.layout())if(f.identity==name)frame=f;
  record::ImageCopySource source;QSize size;QString identity;
  const auto error=source.open({false,name},&size,&identity);QVERIFY2(!error,qPrintable(error.message));
  QCOMPARE(size,frame.nativeRect.size());
  GError* parseError=nullptr;
  GstElement* pipeline=gst_parse_launch(QStringLiteral("appsrc name=src is-live=true format=time caps=\"%1\" ! videoconvert ! video/x-raw,format=BGRx ! appsink name=sink max-buffers=2 drop=true sync=false").arg(source.caps()).toUtf8().constData(),&parseError);
  QVERIFY2(pipeline,parseError?parseError->message:"no pipeline");
  auto cleanup=qScopeGuard([&]{source.stop();gst_element_set_state(pipeline,GST_STATE_NULL);gst_object_unref(pipeline);});
  GstElement* appsrc=gst_bin_get_by_name(GST_BIN(pipeline),"src");GstElement* sink=gst_bin_get_by_name(GST_BIN(pipeline),"sink");
  QVERIFY(gst_element_set_state(pipeline,GST_STATE_PLAYING)!=GST_STATE_CHANGE_FAILURE);
  source.attach(appsrc,[](Error){});gst_object_unref(appsrc);
  // The fixture redraws its turned background; wait for a frame that shows it.
  QImage image;
  QVERIFY(waitFor([&]{
   GstSample* sample=gst_app_sink_try_pull_sample(GST_APP_SINK(sink),200*GST_MSECOND);if(!sample)return false;
   GstMapInfo map;GstBuffer* buffer=gst_sample_get_buffer(sample);GstStructure* caps=gst_caps_get_structure(gst_sample_get_caps(sample),0);int w=0,h=0;gst_structure_get_int(caps,"width",&w);gst_structure_get_int(caps,"height",&h);
   if(gst_buffer_map(buffer,&map,GST_MAP_READ)){image=QImage(map.data,w,h,w*4,QImage::Format_RGB32).copy();gst_buffer_unmap(buffer,&map);}
   gst_sample_unref(sample);return !image.isNull()&&rgb(image,image.width()/2,image.height()-5)!=qRgb(0,0,0);},15000));
  gst_object_unref(sink);
  QCOMPARE(image.size(),frame.nativeRect.size());
  // Upright: the white mark at the logical top-left, the black at the top-right.
  QCOMPARE(rgb(image,10,10),qRgb(255,255,255));QCOMPARE(rgb(image,image.width()-10,10),qRgb(0,0,0));
#endif
 }
 void displayChangeInvalidates(){
  if(randr_.isEmpty())QSKIP("wlr-randr is not installed");
  BudgieSourceAdapter adapter;QVERIFY(waitFor([&]{return adapter.capabilities()["capture"].toObject()["desktop"].toObject()["available"].toBool();}));
  auto prepared=adapter.prepareAcquisition("desktop");
  const auto name=layout_.last().identity;QVERIFY(runRandr({"--output",name,"--scale","2"}));
  auto restore=qScopeGuard([&]{runRandr({"--output",name,"--scale","1"});putgd::test::settle(500);});
  QSignalSpy changed(&adapter,&BudgieSourceAdapter::capabilitiesChanged);QVERIFY(putgd::test::waitForSignal(changed));
  PayloadResult result;bool done=false;prepared(ResourceLimits{},[&](PayloadResult r){result=std::move(r);done=true;});
  QVERIFY(waitFor([&]{return done;}));QCOMPARE(result.error.code,QString("display_changed"));
 }
 // The layer-shell selector in putgd-region, driven by a virtual pointer drag:
 // exact size, only the fixture's pixels (no outline), live and screenshot first.
 // The region lies on one of two outputs, so only that output is read.
 void regionDragExactPixels_data(){QTest::addColumn<bool>("screenshotFirst");QTest::newRow("live")<<false;QTest::newRow("screenshot-first")<<true;}
 void regionDragExactPixels(){
  QFETCH(bool,screenshotFirst);
  const auto pointer=QDir(QStringLiteral(PUTGD_SOURCE_DIR)).filePath("tools/lab/wayland_pointer.py");
  if(!QFile::exists(QCoreApplication::applicationDirPath()+"/putgd-region"))QSKIP("putgd-region was not built");
  // Earlier cases turn and rescale outputs; start once the layout is back and still.
  WaylandOutputs outputs;const auto original=[&]{const auto now=outputs.layout();if(now.size()!=layout_.size())return false;for(int i=0;i<now.size();++i)if(now[i].identity!=layout_[i].identity||now[i].logicalRect!=layout_[i].logicalRect||now[i].density!=layout_[i].density||now[i].rotation!=layout_[i].rotation)return false;return true;};
  PUTGD_VERIFY_STEADY(original(),1000);
  BudgieSourceAdapter adapter;adapter.setRegionScreenshotFirst(screenshotFirst);
  QVERIFY(waitFor([&]{return adapter.capabilities()["capture"].toObject()["region"].toObject()["available"].toBool();}));
  PayloadResult result;bool done=false;
  adapter.prepareAcquisition("region")(ResourceLimits{},[&](PayloadResult r){result=std::move(r);done=true;});
  QRectF bounds;for(const auto& frame:layout_)bounds=bounds.united(frame.logicalRect);
  // The fixture's first output is plain colour away from its marks and the window.
  const QPoint start=layout_.first().logicalRect.toRect().topLeft()+QPoint(80,200);
  bool dragged=false;
  QVERIFY(waitFor([&]{
   if(done)return true;
   QProcess drag;drag.start("python3",{"-I",pointer,"drag",QString::number(start.x()-bounds.x()),QString::number(start.y()-bounds.y()),QString::number(start.x()-bounds.x()+200),QString::number(start.y()-bounds.y()+150),"--extent",QString::number(int(bounds.width())),QString::number(int(bounds.height()))});
   dragged|=drag.waitForFinished(5000)&&drag.exitCode()==0;
   return done;
  },20000));
  QVERIFY(dragged);
  QVERIFY2(!result.error,qPrintable(result.error.message));
  QCOMPARE(result.payload.image.size(),QSize(200,150));
  const auto image=result.payload.image.convertToFormat(QImage::Format_RGB32);
  for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)if(rgb(image,x,y)!=outputColours[0])QFAIL(qPrintable(QStringLiteral("pixel %1,%2 is not the fixture background").arg(x).arg(y)));
 }
 void ownWindowsRefused(){
  for(const char* id:{"putgd","putgd-ui","putgd-record","putgd-region"})QVERIFY(BudgieSourceAdapter::ownAppId(QString::fromLatin1(id)));
  QVERIFY(!BudgieSourceAdapter::ownAppId("putgd-labwc-fixture"));
 }
 void bridgeKeyNormalization(){
  QCOMPARE(LabwcBridgeBackend::gtkAccelerator("Ctrl+Shift+1"),QString("<Control><Shift>1"));
  QCOMPARE(LabwcBridgeBackend::gtkAccelerator("Ctrl+Shift+F9"),QString("<Control><Shift>F9"));
  QCOMPARE(LabwcBridgeBackend::gtkAccelerator("Print"),QString("Print"));
  QCOMPARE(LabwcBridgeBackend::gtkAccelerator("Meta+Alt+A"),QString("<Alt><Super>a"));
  QCOMPARE(LabwcBridgeBackend::labwcKey("<Control><Shift>F9"),QString("C-S-F9"));
  QCOMPARE(LabwcBridgeBackend::labwcKey("<Control><Shift>1"),QString("C-S-1"));
  QCOMPARE(LabwcBridgeBackend::normalizedLabwcKey("C-S-F9"),LabwcBridgeBackend::normalizedLabwcKey("S-C-f9"));
  QCOMPARE(LabwcBridgeBackend::normalizedLabwcKey("C-Print"),LabwcBridgeBackend::normalizedLabwcKey("C-print"));
  QVERIFY(LabwcBridgeBackend::normalizedLabwcKey("C-Print")!=LabwcBridgeBackend::normalizedLabwcKey("Print"));
  QCOMPARE(LabwcBridgeBackend::parseStrv("['/a/', '/b\\'c/']"),QStringList({"/a/","/b'c/"}));
  QCOMPARE(LabwcBridgeBackend::parseStrv("@as []"),QStringList());
  QCOMPARE(LabwcBridgeBackend::strv({}),QString("@as []"));
  QVERIFY(LabwcBridgeBackend::pathFor("region").endsWith("/customputgdregion/"));
  QVERIFY(LabwcBridgeBackend::trigger("window").endsWith("org.putgd.App.Budgie.Trigger string:window"));
 }
 void bridgeReadsRcXml(){
  QTemporaryDir dir;QFile file(dir.filePath("rc.xml"));QVERIFY(file.open(QIODevice::WriteOnly));
  file.write("<labwc_config><keyboard><keybind key=\"Print\" bridge=\"budgie-wm/take-full-screenshot\"><action name=\"Execute\" command=\"budgie-screenshot\"/></keybind>"
             "<keybind key=\"C-S-1\" bridge=\"customputgdregion\"><action name=\"Execute\" command=\"x\"/></keybind><keybind key=\"W-Return\"><action name=\"Execute\" command=\"foot\"/></keybind></keyboard></labwc_config>");file.close();
  const auto keybinds=LabwcBridgeBackend::readKeybinds(file.fileName());QCOMPARE(keybinds.size(),3);
  QCOMPARE(keybinds[0].bridge,QString("budgie-wm/take-full-screenshot"));QCOMPARE(keybinds[1].key,QString("C-S-1"));QCOMPARE(keybinds[2].command,QString("foot"));
 }
};
QTEST_GUILESS_MAIN(BudgieLabwcTest)
#include "test_budgie_labwc.moc"
