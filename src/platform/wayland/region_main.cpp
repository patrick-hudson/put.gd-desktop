#include "LayerShellRegionOverlay.h"
#include "PlasmaDesktopFont.h"
#include "ui/QuickBackend.h"
#include "RegionProtocol.h"
#include "ipc/CaptureControl.h"
#include "ipc/StdioDevice.h"
#include "ui/Theme.h"
#include "ui/CountdownPill.h"
#include "platform/CountdownProcess.h"
#include "core/Settings.h"
#include "core/BuildInfo.h"
#include <QApplication>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QScreen>
#include <QTimer>
#include <QUuid>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <functional>
#include <thread>
#include <cerrno>
#include <utility>
#if defined(PUTGD_HAVE_LAYER_SHELL) && QT_VERSION >= QT_VERSION_CHECK(6,5,0)
#include <wayland-client.h>
#define PUTGD_REGION_TEARDOWN_FENCE 1
#endif

namespace {
QList<putgd::CaptureFrame> layout() {
 QList<putgd::CaptureFrame> frames;
 for(auto* screen:QGuiApplication::screens()) {
  putgd::CaptureFrame frame; frame.identity=screen->name(); frame.logicalRect=screen->geometry();
  frame.rotation=screen->angleBetween(screen->nativeOrientation(),screen->orientation());
  frames.append(std::move(frame));
 }
 return frames;
}
}
int main(int argc,char** argv) {
 if(argc==2&&std::strcmp(argv[1],"--version-json")==0){
  std::printf("%s\n",putgd::buildManifestJson());return 0;
 }
 if(argc==2 && (std::strcmp(argv[1],"--version")==0 || std::strcmp(argv[1],"-v")==0)) {
  std::printf("putgd-region %s\n",putgd::buildVersion()); return 0;
 }
 // Countdown mode (#1363): the delay's pill and nothing else; see CountdownProcess.
 const bool countdownMode=argc>1&&std::strcmp(argv[1],putgd::CountdownProcess::argument)==0;
 int countdownSeconds=0;
 if(countdownMode) {
  if(argc!=5) return 2;
  if(std::strcmp(argv[4],putgd::buildVersion())!=0) return putgd::CountdownProcess::buildExit;
  bool numeric=false; countdownSeconds=QByteArray(argv[2]).toInt(&numeric);
  if(!numeric||countdownSeconds<1||!putgd::Settings::captureDelayChoices().contains(countdownSeconds)) return 2;
 }
 putgd::UiControlPipes handles; if(!countdownMode&&!handles.valid()) return 2;
 // This helper paints its own surfaces and pills from the app theme. Plasma's
 // platform theme would load KDE Frameworks, Qt Quick and Mesa on every region
 // press, 90 ms of the hotkey on Kubuntu's native package, for one thing the
 // selector shows: the desktop font, which is read directly below. Where that
 // plugin isn't installed, as in the AppImage, Qt's own theme stays as it was.
 const bool plasmaFont=putgd::plasmaThemeExpected(qgetenv("QT_QPA_PLATFORMTHEME"),qgetenv("XDG_CURRENT_DESKTOP"))&&putgd::plasmaThemeInstalled(QCoreApplication::libraryPaths());
 if(plasmaFont) {
  QGuiApplication::setDesktopSettingsAware(false);
  qunsetenv("QT_QPA_PLATFORMTHEME");
  qunsetenv("QT_STYLE_OVERRIDE");
 }
 int applicationArguments=countdownMode?1:argc;
 QApplication app(applicationArguments,argv); app.setQuitOnLastWindowClosed(false);
 if(plasmaFont) QGuiApplication::setFont(putgd::kdeDesktopFont(putgd::kdeGlobalsPaths()));
 if(countdownMode) {
  app.setDesktopFileName("putgd-region"); QCoreApplication::setApplicationName("putgd-region");
  putgd::Theme::setScheme(std::strcmp(argv[3],"light")==0?putgd::Theme::Scheme::Light:putgd::Theme::Scheme::Dark);
  if(!QGuiApplication::platformName().startsWith("wayland")) return 1;
  std::unique_ptr<putgd::CountdownPill> pill;
  pill=std::make_unique<putgd::CountdownPill>(countdownSeconds,[&](bool expired) {
   // Outside the pill's own event handler: take its surface down, and report
   // the result only once the compositor has.
   QTimer::singleShot(0,&app,[&,expired] {
    // A cancelled countdown is followed by no capture: the pill just goes.
    if(!expired) { pill.reset(); app.exit(putgd::CountdownProcess::cancelledExit); return; }
    putgd::hideLayerShellCountdown(*pill,[&](bool acknowledged,bool presented) { pill.reset(); app.exit(!acknowledged?4:presented?0:putgd::CountdownProcess::hiddenExit); });
   });
  });
  if(!putgd::showLayerShellCountdown(*pill)) return 1;
  pill->start();
  const int status=app.exec();
  // The exit status is the countdown's answer, and the pill's surface is gone by
  // now. Qt Wayland's display teardown can wait forever on its event thread
  // (Qt 6.10, #1332), and the resident then saw no answer until its deadline
  // killed this helper: a cancelled or finished countdown became
  // delay_unavailable 25 seconds later. Leave without that teardown.
  std::fflush(nullptr);
  std::_Exit(status);
 }
 // Load the pill font while this process waits for its bootstrap; a surface does
 // not wait for it to map.
 std::thread fonts=putgd::LayerShellRegionOverlay::preloadFonts();
 const auto joinFonts=qScopeGuard([&]{fonts.join();});
 app.setDesktopFileName("putgd-region");
 QCoreApplication::setOrganizationName("putgd"); QCoreApplication::setApplicationName("putgd-region");
 QCoreApplication::setApplicationVersion(putgd::buildVersion());
 putgd::StdioDevice pipes(handles.input(),handles.output()); if(!pipes.isOpen()) return 2;
 putgd::CaptureControl control(pipes); QTimer deadline; deadline.setSingleShot(true); deadline.start(10000);
 const auto build=QJsonDocument::fromJson(putgd::buildManifestJson()).object();
 QString nonce,generation; QList<putgd::CaptureFrame> frozen;
 std::unique_ptr<putgd::LayerShellRegionOverlay> selector;QString outcome;bool screenshotFirst=false;QList<putgd::CaptureFrame> backdrop;
 // Delayed region (#1363): the pill shows between selection and the result.
 std::unique_ptr<putgd::CountdownPill> pill;int delaySeconds=0;
 bool bootstrapped=false,ready=false,begun=false,resultSent=false,settled=false,teardownPending=false,deferKeyboard=false;
 QTimer teardownDeadline;teardownDeadline.setSingleShot(true);
 std::function<void(bool)> teardownReply;
#ifdef PUTGD_REGION_TEARDOWN_FENCE
 wl_callback* teardownFence=nullptr;
#endif
 const auto envelope=[&]{return QJsonObject{{"nonce",nonce},{"generation",generation}};};
 const auto exitNow=[&](int status){
  if(settled) return;
  settled=true; deadline.stop(); teardownDeadline.stop();teardownReply={};
#ifdef PUTGD_REGION_TEARDOWN_FENCE
  if(teardownFence){wl_callback_destroy(teardownFence);teardownFence=nullptr;}
#endif
  selector.reset(); pill.reset(); control.stop(); app.exit(status);
 };
 std::function<void(bool)> completeTeardown=[&](bool acknowledged){
  teardownDeadline.stop();
#ifdef PUTGD_REGION_TEARDOWN_FENCE
  if(teardownFence){wl_callback_destroy(teardownFence);teardownFence=nullptr;}
#endif
  auto reply=std::move(teardownReply);teardownReply={};if(reply)reply(acknowledged);
 };
 const auto teardown=[&](std::function<void(bool)> reply){
  if(settled||teardownPending)return;
  teardownPending=true;deadline.stop();const bool hadSelector=bool(selector)||bool(pill);selector.reset();pill.reset();teardownReply=std::move(reply);
  if(!hadSelector){completeTeardown(true);return;}
#ifdef PUTGD_REGION_TEARDOWN_FENCE
  // Destruction queues hide/destroy requests on the SAME Qt connection that
  // created the layer surfaces. A different client's sync cannot certify this.
  auto* native=app.nativeInterface<QNativeInterface::QWaylandApplication>();
  if(!native){completeTeardown(false);return;}
  teardownFence=wl_display_sync(native->display());
  if(!teardownFence){completeTeardown(false);return;}
  static const wl_callback_listener listener={[](void* data,wl_callback*,uint32_t){
   (*static_cast<std::function<void(bool)>*>(data))(true);
  }};
  if(wl_callback_add_listener(teardownFence,&listener,&completeTeardown)<0){completeTeardown(false);return;}
  teardownDeadline.start(1000);
  if(wl_display_flush(native->display())<0&&errno!=EAGAIN)completeTeardown(false);
#else
  // This build cannot map layer selector surfaces; there is nothing to fence.
  completeTeardown(true);
#endif
 };
 QObject::connect(&teardownDeadline,&QTimer::timeout,&app,[&]{completeTeardown(false);});
 const auto close=[&](int status){
  if(settled||teardownPending)return;
  teardown([&,status](bool acknowledged){exitNow(acknowledged?status:4);});
 };
 const auto fail=[&](const QString& code){
  if(settled||teardownPending)return;
  teardown([&,code](bool acknowledged){
   if(!acknowledged){exitNow(4);return;}
   auto message=envelope();message["code"]=code;
   if(!ready||!control.send(code=="cancelled"?"cancelled":"error",message)){exitNow(4);return;}
   // The authenticated terminal frame certifies completed surface teardown.
   // Writes are synchronous; defer normal exit outside the emitting callback.
   QTimer::singleShot(0,&app,[&]{exitNow(0);});
  });
 };
 QObject::connect(&pipes,&QIODevice::readChannelFinished,&app,[&]{close(3);});
 QObject::connect(&control,&putgd::CaptureControl::failed,&app,[&]{close(4);});
 QObject::connect(&deadline,&QTimer::timeout,&app,[&]{if(ready) fail("capture_timeout"); else close(3);});
 QObject::connect(&control,&putgd::CaptureControl::frame,&app,[&](const QString& type,const QJsonObject& payload){
  if(settled||teardownPending) return;
  if(type=="bootstrap") {
   if(bootstrapped || payload.size()!=4+int(payload.contains("outcome"))+int(payload.contains("frozen"))+int(payload.contains("appearance"))+int(payload.contains("deferKeyboard"))+int(payload.contains("delay")) || payload["build"].toObject()!=build ||
      (payload.contains("frozen")&&(!payload["frozen"].isObject()||payload["frozen"].toObject().size()!=2||!payload["frozen"]["fd"].isDouble()||!payload["frozen"]["frames"].isArray())) ||
      !payload["nonce"].isString() || QUuid(payload["nonce"].toString()).isNull() ||
      !payload["generation"].isString() || !payload["layout"].isArray() || (payload.contains("outcome")&&(!payload["outcome"].isString()||payload["outcome"].toString().size()>256))) { close(4); return; }
   if(payload.contains("appearance")&&(!payload["appearance"].isObject()||(!payload["appearance"].toObject().isEmpty()&&payload["appearance"]["scheme"]!="light"&&payload["appearance"]["scheme"]!="dark"))){close(4);return;}
   putgd::Theme::setScheme(payload["appearance"]["scheme"]=="light"?putgd::Theme::Scheme::Light:putgd::Theme::Scheme::Dark);
   bool numeric=false; const auto g=payload["generation"].toString(); const auto number=g.toULongLong(&numeric);
   if(!numeric || !number || QString::number(number)!=g) { close(4); return; }
   if(payload.contains("deferKeyboard")&&payload["deferKeyboard"]!=true){close(4);return;}
   deferKeyboard=payload.contains("deferKeyboard");
   // Only a live selection counts down here; a frozen one was delayed before its freeze.
   if(payload.contains("delay")) {
    const auto delay=payload["delay"];
    if(!delay.isDouble()||delay.toDouble()!=delay.toInt()||delay.toInt()<1||!putgd::Settings::captureDelayChoices().contains(delay.toInt())||payload.contains("frozen")) { close(4); return; }
    delaySeconds=delay.toInt();
   }
   outcome=payload["outcome"].toString();nonce=payload["nonce"].toString(); generation=g; bootstrapped=true;
   putgd::Error error; frozen=putgd::waylandRegionLayout(payload["layout"].toArray(),error);
   if(error) { close(4); return; }
   // Screenshot first (#629): the resident's frozen frames become the backdrop.
   if(payload.contains("frozen")) {
    const auto descriptor=payload["frozen"].toObject();const auto fd=descriptor["fd"].toInteger();
    // The geometry-only layout keeps validating results; the selector draws these.
    backdrop=frozen;
    if(fd<3 || fd>65535 || !putgd::waylandFrozenPixels(int(fd),descriptor["frames"].toArray(),backdrop,error)) { close(4); return; }
    screenshotFirst=true;
   }
   ready=true; auto message=envelope(); message["build"]=build;
   if(!control.send("ready",message)) { close(4); return; }
   if(!QGuiApplication::platformName().startsWith("wayland")) { fail("region_unavailable"); return; }
   if(!putgd::waylandRegionLayoutsMatch(frozen,layout())) fail("display_changed");
   return;
  }
  if(type=="pointer") {
   if(!begun || payload.size()!=5 || payload["nonce"]!=nonce || payload["generation"]!=generation || !payload["kind"].isString() || !payload["x"].isDouble() || !payload["y"].isDouble()) { close(4); return; }
   // The selection may already have ended on the helper's own input.
   if(selector && !resultSent) selector->relayPointer(payload["kind"].toString(),QPointF(payload["x"].toDouble(),payload["y"].toDouble()));
   return;
  }
  if(type=="takeover") {
   if(!begun || payload.size()!=4 || payload["nonce"]!=nonce || payload["generation"]!=generation || !payload["key"].isDouble() || !payload["modifiers"].isDouble()) { close(4); return; }
   if(selector && !resultSent) selector->takeKeyboard(payload["key"].toInt(),payload["modifiers"].toInt());
   return;
  }
  if(!ready || payload.size()!=2 || payload["nonce"]!=nonce || payload["generation"]!=generation) { close(4); return; }
  if(type=="cancel") { fail("cancelled"); return; }
  if(type=="begin") {
   if(begun || !putgd::waylandRegionLayoutsMatch(frozen,layout())) { if(begun) close(4); else fail("display_changed"); return; }
   begun=true; deadline.start(120000);
   selector=std::make_unique<putgd::LayerShellRegionOverlay>(screenshotFirst?backdrop:frozen,nullptr,false,outcome,screenshotFirst);
   QObject::connect(selector.get(),&putgd::LayerShellRegionOverlay::cancelled,&app,[&](const putgd::Error& error){
    // Destroy outside the emitting Surface event handler.
    const auto code=error.code; QTimer::singleShot(0,&app,[&,code]{fail(code);});
   });
   QObject::connect(selector.get(),&putgd::LayerShellRegionOverlay::selected,&app,[&](const QRectF& rectangle){
    // selected is emitted only after a rendered transparent frame and the unmap fence.
    // The signal can originate from an overlay callback, so defer destruction.
    QTimer::singleShot(0,&app,[&,rectangle]{
     if(settled || teardownPending || resultSent) return;
     if(!putgd::waylandRegionLayoutsMatch(frozen,layout())) { fail("display_changed"); return; }
     putgd::Error error; const auto encoded=putgd::waylandRegionRectangle(rectangle);
     putgd::waylandRegionRectangle(encoded,frozen,error); if(error) { fail("capture_geometry"); return; }
     selector.reset();
     const auto report=[&,encoded] {
      resultSent=true; auto message=envelope(); message["rectangle"]=encoded;
      if(!control.send("result",message)) { close(4); return; }
      deadline.start(10000);
     };
     if(!delaySeconds) { report(); return; }
     // Area first, then the countdown: the result leaves only after the pill's
     // surface is gone, so the resident's capture cannot contain it.
     QScreen* display=nullptr;
     for(auto* screen:QGuiApplication::screens()) if(QRectF(screen->geometry()).contains(rectangle.center())) display=screen;
     deadline.start((delaySeconds+20)*1000);
     pill=std::make_unique<putgd::CountdownPill>(delaySeconds,[&,report](bool expired) {
      QTimer::singleShot(0,&app,[&,report,expired] {
       if(settled || teardownPending || resultSent || !pill) return;
       if(!expired) { fail("cancelled"); return; }
       putgd::hideLayerShellCountdown(*pill,[&,report](bool acknowledged,bool presented) {
        if(settled || teardownPending || resultSent) return;
        pill.reset();
        if(!acknowledged) { close(4); return; }
        // Not on screen when the delay ran out: locked or blanked, so no capture.
        if(!presented) { fail("session_locked"); return; }
        if(!putgd::waylandRegionLayoutsMatch(frozen,layout())) { fail("display_changed"); return; }
        report();
       });
      });
     });
     if(!putgd::showLayerShellCountdown(*pill,display)) { fail("region_unavailable"); return; }
     pill->start();
    });
   });
   selector->setDeferKeyboard(deferKeyboard);
   selector->start(); return;
  }
  if(type=="accept" && resultSent) { close(0); return; }
  close(4);
 });
 return app.exec();
}
