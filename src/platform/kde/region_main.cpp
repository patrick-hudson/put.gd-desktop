#include "KdeRegionOverlay.h"
#include "KdeRegionProtocol.h"
#include "platform/x11/CaptureControl.h"
#include "ui/StdioDevice.h"
#include "core/BuildInfo.h"
#include <QApplication>
#include <QJsonDocument>
#include <QScreen>
#include <QTimer>
#include <QUuid>
#include <cstdio>
#include <cstring>
#include <memory>
#include <functional>
#include <cerrno>
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
  std::printf("putgd-kde-region %s\n",putgd::buildVersion()); return 0;
 }
 putgd::UiControlPipes handles; if(!handles.valid()) return 2;
 QApplication app(argc,argv); app.setQuitOnLastWindowClosed(false);
 app.setDesktopFileName("putgd-kde-region");
 QCoreApplication::setOrganizationName("putgd"); QCoreApplication::setApplicationName("putgd-kde-region");
 QCoreApplication::setApplicationVersion(putgd::buildVersion());
 putgd::StdioDevice pipes(handles.input(),handles.output()); if(!pipes.isOpen()) return 2;
 putgd::CaptureControl control(pipes); QTimer deadline; deadline.setSingleShot(true); deadline.start(10000);
 const auto build=QJsonDocument::fromJson(putgd::buildManifestJson()).object();
 QString nonce,generation; QList<putgd::CaptureFrame> frozen;
 std::unique_ptr<putgd::KdeRegionOverlay> selector;
 bool bootstrapped=false,ready=false,begun=false,resultSent=false,settled=false,teardownPending=false;
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
  selector.reset(); control.stop(); app.exit(status);
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
  teardownPending=true;deadline.stop();const bool hadSelector=bool(selector);selector.reset();teardownReply=std::move(reply);
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
   if(bootstrapped || payload.size()!=4 || payload["build"].toObject()!=build ||
      !payload["nonce"].isString() || QUuid(payload["nonce"].toString()).isNull() ||
      !payload["generation"].isString() || !payload["layout"].isArray()) { close(4); return; }
   bool numeric=false; const auto g=payload["generation"].toString(); const auto number=g.toULongLong(&numeric);
   if(!numeric || !number || QString::number(number)!=g) { close(4); return; }
   nonce=payload["nonce"].toString(); generation=g; bootstrapped=true;
   putgd::Error error; frozen=putgd::kdeRegionLayout(payload["layout"].toArray(),error);
   if(error) { close(4); return; }
   ready=true; auto message=envelope(); message["build"]=build;
   if(!control.send("ready",message)) { close(4); return; }
   if(!QGuiApplication::platformName().startsWith("wayland")) { fail("region_unavailable"); return; }
   if(!putgd::kdeRegionLayoutsMatch(frozen,layout())) fail("display_changed");
   return;
  }
  if(!ready || payload.size()!=2 || payload["nonce"]!=nonce || payload["generation"]!=generation) { close(4); return; }
  if(type=="cancel") { fail("cancelled"); return; }
  if(type=="begin") {
   if(begun || !putgd::kdeRegionLayoutsMatch(frozen,layout())) { if(begun) close(4); else fail("display_changed"); return; }
   begun=true; deadline.start(120000);
   selector=std::make_unique<putgd::KdeRegionOverlay>(frozen);
   QObject::connect(selector.get(),&putgd::KdeRegionOverlay::cancelled,&app,[&](const putgd::Error& error){
    // Destroy outside the emitting Surface event handler.
    const auto code=error.code; QTimer::singleShot(0,&app,[&,code]{fail(code);});
   });
   QObject::connect(selector.get(),&putgd::KdeRegionOverlay::selected,&app,[&](const QRectF& rectangle){
    // selected is emitted only after a rendered transparent frame and the unmap fence.
    // The signal can originate from an overlay callback, so defer destruction.
    QTimer::singleShot(0,&app,[&,rectangle]{
     if(settled || teardownPending || resultSent) return;
     if(!putgd::kdeRegionLayoutsMatch(frozen,layout())) { fail("display_changed"); return; }
     putgd::Error error; const auto encoded=putgd::kdeRegionRectangle(rectangle);
     putgd::kdeRegionRectangle(encoded,frozen,error); if(error) { fail("capture_geometry"); return; }
     selector.reset(); resultSent=true; auto message=envelope(); message["rectangle"]=encoded;
     if(!control.send("result",message)) { close(4); return; }
     deadline.start(10000);
    });
   });
   selector->start(); return;
  }
  if(type=="accept" && resultSent) { close(0); return; }
  close(4);
 });
 return app.exec();
}
