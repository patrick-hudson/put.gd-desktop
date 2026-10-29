#include "platform/x11/X11Clipboard.h"
#include "platform/x11/X11Shortcuts.h"
#include "platform/x11/X11Capture.h"
#include "platform/x11/X11SourceAdapter.h"
#include "platform/x11/CinnamonRegionCapture.h"
#include "platform/x11/X11CompositorSettle.h"
#include <QDBusConnection>
#include <QDBusContext>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QtTest>
#include <QClipboard>
#include <QGuiApplication>
#include <QTimer>
#include <QTemporaryDir>
#include <QFile>
#include <QBuffer>
#include <QUrl>
#include <QApplication>
#include <QKeySequence>
#include <QPaintEvent>
#include "ui/RegionOverlay.h"
#include "ui/RegionSelection.h"
#include "ui/SelectionOutline.h"
#include "../../fixtures/X11SelectionFixture.h"
#include "../../fixtures/X11DesktopFixture.h"
#include "../../fixtures/X11MappingBarrier.h"
#include <X11/keysym.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xcb.h>
#include <xcb/xtest.h>
#include <unistd.h>
#include "fixtures/Waits.h"
using namespace putgd;
// A screen locker on the disposable bus that x11_isolated supplies (#341).
class ScreenSaverFixture:public QObject {
 Q_OBJECT
 Q_CLASSINFO("D-Bus Interface","org.freedesktop.ScreenSaver")
public:
 explicit ScreenSaverFixture(bool active):active(active){auto bus=QDBusConnection::sessionBus();registered=bus.registerObject("/org/freedesktop/ScreenSaver",this,QDBusConnection::ExportAllSlots)&&bus.registerService("org.freedesktop.ScreenSaver");}
 ~ScreenSaverFixture(){auto bus=QDBusConnection::sessionBus();if(registered)bus.unregisterService("org.freedesktop.ScreenSaver");bus.unregisterObject("/org/freedesktop/ScreenSaver");}
 bool active,registered=false;int calls=0;
public slots:
 bool GetActive(){++calls;return active;}
};
class RegionPaintProbe final:public QObject {
public:
 QRegion painted;int count=0;
 bool eventFilter(QObject*,QEvent* event)override{if(event->type()==QEvent::Paint){painted+=static_cast<QPaintEvent*>(event)->region();++count;}return false;}
};
// This service runs only on the disposable bus supplied by x11_isolated.
// It proves native transport/lifetime contracts, not compositor behavior.
class CinnamonScreenshotFixture:public QObject,protected QDBusContext {
 Q_OBJECT
 Q_CLASSINFO("D-Bus Interface","org.Cinnamon")
public:
 enum Mode {Open,WrongSize,WideDepth,Malformed,ServiceError,Slow};
 explicit CinnamonScreenshotFixture(Mode mode=Open):mode(mode){auto bus=QDBusConnection::sessionBus();registered=bus.registerObject("/org/Cinnamon",this,QDBusConnection::ExportAllSlots)&&bus.registerService("org.Cinnamon");}
 ~CinnamonScreenshotFixture(){close();auto bus=QDBusConnection::sessionBus();if(registered)bus.unregisterService("org.Cinnamon");bus.unregisterObject("/org/Cinnamon");}
 void close(){output.reset();}
 Mode mode;bool registered=false;int requests=0;QString path;QRect rectangle;QList<QRect> rectangles;bool cursor=true,flash=true;std::unique_ptr<QFile> output;
public slots:
 void ScreenshotArea(bool includeCursor,int x,int y,int width,int height,bool includeFlash,const QString& filename){
  ++requests;path=filename;rectangle={x,y,width,height};rectangles.append(rectangle);cursor=includeCursor;flash=includeFlash;
  if(mode==ServiceError){sendErrorReply("org.Cinnamon.SyntheticFailure","arbitrary synthetic payload must stay private");return;}
  output=std::make_unique<QFile>(filename);if(!output->open(QIODevice::WriteOnly))return;
  if(mode==Malformed)output->write("incomplete native image");
  else if(mode==Slow){
   // Half the PNG now and the rest later, as a slow writer would.
   QBuffer png;png.open(QIODevice::WriteOnly);QImage image(width,height,QImage::Format_RGB32);image.fill(Qt::cyan);image.save(&png,"PNG");
   const auto bytes=png.data();output->write(bytes.left(bytes.size()/2));output->flush();
   QTimer::singleShot(400,this,[filename,rest=bytes.mid(bytes.size()/2)]{QFile file(filename);if(file.open(QIODevice::Append))file.write(rest);});
  }
  else{QImage image(width+(mode==WrongSize),height,mode==WideDepth?QImage::Format_RGBA64:QImage::Format_RGB32);image.fill(Qt::cyan);image.save(output.get(),"PNG");}
  output->flush(); // Complete bytes are present while the writer is still open.
 }
};
// Owns the compositing-manager selection and paints override-redirect "scene"
// windows, standing in for a compositor's delayed presentation. Xvfb has no
// compositor; this proves the settle contract, not any compositor's timing.
class X11CompositorFixture {
public:
 X11CompositorFixture(){
  connection=xcb_connect(nullptr,nullptr);screen=xcb_setup_roots_iterator(xcb_get_setup(connection)).data;owner=xcb_generate_id(connection);
  xcb_create_window(connection,XCB_COPY_FROM_PARENT,owner,screen->root,0,0,1,1,0,XCB_WINDOW_CLASS_INPUT_ONLY,XCB_COPY_FROM_PARENT,0,nullptr);
  auto* atom=xcb_intern_atom_reply(connection,xcb_intern_atom(connection,0,strlen("_NET_WM_CM_S0"),"_NET_WM_CM_S0"),nullptr);xcb_set_selection_owner(connection,owner,atom->atom,XCB_CURRENT_TIME);free(atom);sync();
 }
 ~X11CompositorFixture(){for(auto window:scenes)xcb_destroy_window(connection,window);xcb_destroy_window(connection,owner);sync();xcb_disconnect(connection);}
 xcb_window_t scene(QRect r,uint32_t color){auto w=xcb_generate_id(connection);uint32_t values[]{color,1};xcb_create_window(connection,screen->root_depth,w,screen->root,r.x(),r.y(),r.width(),r.height(),0,XCB_WINDOW_CLASS_INPUT_OUTPUT,screen->root_visual,XCB_CW_BACK_PIXEL|XCB_CW_OVERRIDE_REDIRECT,values);xcb_map_window(connection,w);scenes.append(w);sync();return w;}
 void repaint(xcb_window_t w){xcb_clear_area(connection,0,w,0,0,0,0);sync();}
 void withdraw(xcb_window_t w){xcb_unmap_window(connection,w);sync();}
 void sync(){xcb_flush(connection);free(xcb_get_input_focus_reply(connection,xcb_get_input_focus(connection),nullptr));}
 xcb_connection_t* connection=nullptr;xcb_screen_t* screen=nullptr;xcb_window_t owner=0;QList<xcb_window_t> scenes;
};
class X11Test:public QObject {
 Q_OBJECT
private slots:
 void compositorSettleFollowsRepaintsOverSelection() {
  const QRect selected(30,30,40,30);
  {X11CompositorSettle settle({selected});int settled=0;connect(&settle,&X11CompositorSettle::settled,this,[&]{++settled;});settle.start();QCOMPARE(settled,1);QVERIFY(!settle.composited());}
  X11CompositorFixture compositor;const auto inside=compositor.scene({40,40,10,10},0x000000),outside=compositor.scene({300,300,10,10},0x000000);
  auto repaints=[&](xcb_window_t window,int count,QElapsedTimer& last,QObject& scope){auto* timer=new QTimer(&scope);timer->setInterval(20);connect(timer,&QTimer::timeout,this,[&compositor,&last,timer,window,count,done=0]()mutable{compositor.repaint(window);last.start();if(++done==count)timer->deleteLater();});timer->start();return QPointer<QTimer>(timer);};
  {// Repaints over the selection hold the read until they have been quiet.
   QObject scope;X11CompositorSettle settle({selected},nullptr,{80,5000});bool settled=false;qint64 quiet=-1;QElapsedTimer last;connect(&settle,&X11CompositorSettle::settled,this,[&]{settled=true;quiet=last.isValid()?last.elapsed():-1;});
   settle.start();QVERIFY(settle.composited());auto timer=repaints(inside,8,last,scope);putgd::test::settle(120);PUTGD_TRY_VERIFY(!settled);PUTGD_TRY_VERIFY_WITH_TIMEOUT(settled,3000);QVERIFY(timer.isNull());QVERIFY2(quiet>=70,qPrintable(QString::number(quiet)));QVERIFY(settle.repaints()>=8);}
  {// Unrelated repaints elsewhere do not delay it.
   QObject scope;X11CompositorSettle settle({selected},nullptr,{80,5000});bool settled=false;QElapsedTimer last,elapsed;elapsed.start();connect(&settle,&X11CompositorSettle::settled,this,[&]{settled=true;});
   settle.start();auto timer=repaints(outside,30,last,scope);PUTGD_TRY_VERIFY_WITH_TIMEOUT(settled,3000);QVERIFY2(elapsed.elapsed()<400,qPrintable(QString::number(elapsed.elapsed())));PUTGD_TRY_VERIFY(timer.isNull());}
  {// Continuous repaints over the selection are bounded. The 300 ms bound is a
   // coarse timer, which Qt may fire up to 5% early.
   QObject scope;X11CompositorSettle settle({selected},nullptr,{80,300});bool settled=false;QElapsedTimer last,elapsed;elapsed.start();connect(&settle,&X11CompositorSettle::settled,this,[&]{settled=true;});
   settle.start();auto timer=repaints(inside,40,last,scope);PUTGD_TRY_VERIFY_WITH_TIMEOUT(settled,3000);QVERIFY2(elapsed.elapsed()>=285&&elapsed.elapsed()<700,qPrintable(QString::number(elapsed.elapsed())));QVERIFY(!timer.isNull());PUTGD_TRY_VERIFY(timer.isNull());}
  {// Cancellation completes immediately and exactly once.
   X11CompositorSettle settle({selected});int settled=0;connect(&settle,&X11CompositorSettle::settled,this,[&]{++settled;});settle.start();QCOMPARE(settled,0);settle.cancel();QCOMPARE(settled,1);settle.cancel();putgd::test::settle(150);PUTGD_TRY_COMPARE(settled,1);}
 }
 void compositedRegionReadsAfterStaleSelectorFrames() {
  X11DesktopFixture desktop;QVERIFY(desktop.window);X11CompositorFixture compositor;
  bool completed=false,staleAtCompletion=true;PayloadResult captured;X11SourceAdapter source;
  source.acquire("region",{},[&](PayloadResult result){captured=std::move(result);completed=true;});
  QWidget* surface=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(widget->objectName()=="putgd-region-overlay"&&widget->isVisible())surface=widget;QVERIFY(surface);
  // The compositor's last presented frame still shows the selector outline.
  const auto stale=compositor.scene({30,30,40,30},0x000000);bool presented=false;
  QTest::mousePress(surface,Qt::LeftButton,Qt::NoModifier,surface->mapFromGlobal(QPoint(30,30)));QTest::mouseMove(surface,surface->mapFromGlobal(QPoint(70,60)));
  QTest::mouseRelease(surface,Qt::LeftButton,Qt::NoModifier,surface->mapFromGlobal(QPoint(70,60)));QVERIFY(!completed);
  // It keeps repainting the closing selector for a few frames, then presents the desktop.
  QTimer fade;fade.setInterval(16);int frames=0;
  connect(&fade,&QTimer::timeout,this,[&]{if(++frames<6){compositor.repaint(stale);return;}fade.stop();compositor.withdraw(stale);presented=true;});
  fade.start();
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(completed,5000);staleAtCompletion=!presented;
  QVERIFY2(!captured.error,qPrintable(captured.error.message));QVERIFY(!staleAtCompletion);QCOMPARE(captured.payload.image.size(),QSize(40,30));
  for(int y=0;y<captured.payload.image.height();++y){for(int x=0;x<captured.payload.image.width();++x)QCOMPARE(captured.payload.image.pixelColor(x,y),QColor(Qt::green));}
 }
 void compositedRegionCancellationDuringSettle() {
  X11DesktopFixture desktop;X11CompositorFixture compositor;int calls=0;PayloadResult captured;X11SourceAdapter source;
  source.acquire("region",{},[&](PayloadResult result){captured=std::move(result);++calls;});
  QWidget* surface=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(widget->objectName()=="putgd-region-overlay"&&widget->isVisible())surface=widget;QVERIFY(surface);
  // The compositor keeps repainting the selection, so only the bound or cancellation ends the wait.
  const auto busy=compositor.scene({40,40,10,10},0x000000);QTimer repaint;repaint.setInterval(16);connect(&repaint,&QTimer::timeout,this,[&]{compositor.repaint(busy);});repaint.start();
  QTest::mousePress(surface,Qt::LeftButton,Qt::NoModifier,surface->mapFromGlobal(QPoint(30,30)));QTest::mouseMove(surface,surface->mapFromGlobal(QPoint(70,60)));QTest::mouseRelease(surface,Qt::LeftButton,Qt::NoModifier,surface->mapFromGlobal(QPoint(70,60)));
  // Let the selector be destroyed, so the adapter is waiting on the compositor.
  putgd::test::settle(100);PUTGD_TRY_COMPARE(calls,0);
  source.cancelAcquisition();QCOMPARE(calls,1);QCOMPARE(captured.error.code,QString("cancelled"));putgd::test::settle(250);PUTGD_TRY_COMPARE(calls,1);
 }
 void nativeSelectionTextAndRevision() {
  X11Clipboard importer;QVERIFY(importer.available());
  auto before=importer.revision();QGuiApplication::clipboard()->setText("synthetic native clipboard");PUTGD_TRY_VERIFY(importer.revision()>before);
  bool done=false;PayloadResult result;importer.acquire({},[&](PayloadResult r){result=r;done=true;});PUTGD_TRY_VERIFY(done);QVERIFY2(!result.error,qPrintable(result.error.message));QCOMPARE(result.payload.bytes,QByteArray("synthetic native clipboard"));
 }
 void nativeFilesImageAndUrlPrecedence() {
  X11Clipboard importer;QTemporaryDir dir;QStringList expected{dir.filePath("first.txt"),dir.filePath("second.txt")};
  for(const auto& path:expected){QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write("synthetic");}
  auto* files=new QMimeData;QList<QUrl> urls;for(const auto& path:expected)urls.append(QUrl::fromLocalFile(path));files->setUrls(urls);files->setText("lower precedence");QGuiApplication::clipboard()->setMimeData(files);putgd::test::settle(20);
  bool done=false;PayloadResult result;importer.acquire({},[&](PayloadResult r){result=r;done=true;});PUTGD_TRY_VERIFY(done);QVERIFY(!result.error);QCOMPARE(result.payload.files,expected);
  QImage expectedImage(24,16,QImage::Format_ARGB32);expectedImage.fill(Qt::cyan);QGuiApplication::clipboard()->setImage(expectedImage);putgd::test::settle(20);done=false;importer.acquire({},[&](PayloadResult r){result=r;done=true;});PUTGD_TRY_VERIFY(done);QVERIFY(!result.error);QCOMPARE(result.payload.image.size(),expectedImage.size());QCOMPARE(result.payload.image.pixelColor(2,2),QColor(Qt::cyan));
  auto* url=new QMimeData;url->setUrls({QUrl("https://example.invalid/synthetic")});url->setText("https://example.invalid/synthetic");QGuiApplication::clipboard()->setMimeData(url);putgd::test::settle(20);done=false;importer.acquire({},[&](PayloadResult r){result=r;done=true;});PUTGD_TRY_VERIFY(done);QVERIFY2(!result.error,qPrintable(result.error.message));QCOMPARE(result.payload.kind,QString("text"));QCOMPARE(result.payload.bytes,QByteArray("https://example.invalid/synthetic"));
 }
 void remoteUriFallsThroughToText() {
  X11SelectionFixture owner({{"text/uri-list","https://example.invalid/synthetic\r\n"},{"UTF8_STRING","plain caption https://example.invalid/synthetic"}});X11Clipboard importer;bool done=false;PayloadResult result;importer.acquire({},[&](PayloadResult r){result=r;done=true;});PUTGD_TRY_VERIFY(done);QVERIFY2(!result.error,qPrintable(result.error.message));QCOMPARE(result.payload.kind,QString("text"));QCOMPARE(result.payload.bytes,QByteArray("plain caption https://example.invalid/synthetic"));
 }
 void bmpImageAndIncrementalSelection() {
  QImage image(19,17,QImage::Format_RGB32);image.fill(Qt::magenta);QByteArray encoded;QBuffer buffer(&encoded);QVERIFY(buffer.open(QIODevice::WriteOnly));QVERIFY(image.save(&buffer,"BMP"));
  X11SelectionFixture owner({{"text/uri-list","https://example.invalid/not-local\r\n"},{"image/bmp",encoded}},true);X11Clipboard importer;bool done=false;PayloadResult result;importer.acquire({},[&](PayloadResult r){result=r;done=true;});PUTGD_TRY_VERIFY(done);QVERIFY2(!result.error,qPrintable(result.error.message));QCOMPARE(result.payload.image.size(),image.size());QCOMPARE(result.payload.image.pixelColor(3,3),QColor(Qt::magenta));QVERIFY(owner.incrementalTransfers>0);
 }
 void incrementalText() {
  const QByteArray expected(20000,'x');X11SelectionFixture owner({{"UTF8_STRING",expected}},true);X11Clipboard importer;bool done=false;PayloadResult result;importer.acquire({},[&](PayloadResult r){result=r;done=true;});PUTGD_TRY_VERIFY(done);QVERIFY2(!result.error,qPrintable(result.error.message));QCOMPARE(result.payload.bytes,expected);QVERIFY(owner.incrementalTransfers>0);
 }
 void invocationRevisionIncludesQueuedOwnerChange() {
  X11Clipboard importer;X11SelectionFixture owner({{"UTF8_STRING","invocation-source"}});const auto accepted=importer.revision();bool done=false;PayloadResult result;importer.acquire({},[&](PayloadResult r){result=r;done=true;},accepted);PUTGD_TRY_VERIFY(done);QVERIFY2(!result.error,qPrintable(result.error.message));QCOMPARE(result.payload.bytes,QByteArray("invocation-source"));
 }
 void nativeKeyActivationWithLocks() {
  X11Shortcuts shortcuts;shortcuts.bind({{"region","Ctrl+Shift+1"}});int activated=0;connect(&shortcuts,&X11Shortcuts::activated,this,[&](const QString& a){if(a=="region")++activated;});
  auto* c=xcb_connect(nullptr,nullptr);auto* symbols=xcb_key_symbols_alloc(c);auto code=[&](xcb_keysym_t key){auto* codes=xcb_key_symbols_get_keycode(symbols,key);auto result=codes?codes[0]:xcb_keycode_t(0);free(codes);return result;};
  auto send=[&](xcb_keysym_t key,bool press){auto k=code(key);QVERIFY(k);xcb_test_fake_input(c,press?XCB_KEY_PRESS:XCB_KEY_RELEASE,k,XCB_CURRENT_TIME,0,0,0,0);xcb_flush(c);};
  auto chord=[&]{send(XK_Control_L,true);send(XK_Shift_L,true);send(XK_1,true);send(XK_1,false);send(XK_Shift_L,false);send(XK_Control_L,false);};
  chord();PUTGD_TRY_COMPARE(activated,1);send(XK_Caps_Lock,true);send(XK_Caps_Lock,false);send(XK_Num_Lock,true);send(XK_Num_Lock,false);chord();PUTGD_TRY_COMPARE(activated,2);send(XK_Caps_Lock,true);send(XK_Caps_Lock,false);send(XK_Num_Lock,true);send(XK_Num_Lock,false);xcb_key_symbols_free(symbols);xcb_disconnect(c);
 }
 void selectionOutlineStaysInsideTinyAndScaledBounds(){
  for(qreal scale:{1.0,1.25,1.5,2.0})for(int width:{1,2,3,6,30})for(int height:{1,2,3,6,30}){
   QImage image(100,100,QImage::Format_ARGB32_Premultiplied);image.fill(Qt::transparent);
   const QRectF bounds(8,8,width,height);
   {QPainter painter(&image);painter.scale(scale,scale);paintSelectionOutline(painter,bounds);}
   const QRect allowed=QRectF(bounds.x()*scale,bounds.y()*scale,width*scale,height*scale).toAlignedRect();
   bool dark=false,light=false;
   for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x){
    const auto color=image.pixelColor(x,y);if(!color.alpha())continue;
    QVERIFY2(allowed.contains(x,y),"The outline must not extend outside the capture geometry");
    dark|=color==QColor(Qt::black);light|=color==QColor(Qt::white);
   }
   QVERIFY(dark);if(width>=6&&height>=6)QVERIFY(light);
   if(width>=10&&height>=10)QCOMPARE(image.pixelColor(qRound(bounds.center().x()*scale),qRound(bounds.center().y()*scale)).alpha(),0);
  }
 }
 void regionCancellationAndExactSelection() {
  CaptureFrame frame{"fixture",QRectF(0,0,1000,1000),QRect(0,0,1000,1000),1,0,QImage(1000,1000,QImage::Format_ARGB32)};frame.pixels.fill(Qt::red);
  auto surface=[]()->QWidget*{for(auto* w:QApplication::topLevelWidgets())if(w->objectName()=="putgd-region-overlay"&&w->isVisible())return w;return nullptr;};
  {RegionOverlay overlay({frame});bool cancelled=false;connect(&overlay,&RegionOverlay::cancelled,this,[&](const Error& e){cancelled=e.code=="cancelled";});overlay.start();QVERIFY(surface());QTest::keyClick(surface(),Qt::Key_Escape);QVERIFY(cancelled);}
  {RegionOverlay overlay({frame});bool cancelled=false;connect(&overlay,&RegionOverlay::cancelled,this,[&](const Error& e){cancelled=e.code=="cancelled";});overlay.start();QVERIFY(surface());QTest::mouseClick(surface(),Qt::RightButton);QVERIFY(cancelled);}
  {RegionOverlay overlay({frame});QRectF selected;connect(&overlay,&RegionOverlay::selected,this,[&](QRectF r){selected=r;});overlay.start();QVERIFY(surface());auto* w=surface();
   auto rendered=[w]{QImage image(w->size(),QImage::Format_ARGB32);image.fill(Qt::transparent);w->render(&image);return image;};
   QCOMPARE(rendered().pixelColor(30,40).alpha(),0);
   QTest::mousePress(w,Qt::LeftButton,Qt::NoModifier,QPoint(10,20));QTest::mouseMove(w,QPoint(60,70));
   const auto dragging=rendered();QCOMPARE(dragging.pixelColor(30,40).alpha(),0);QCOMPARE(dragging.pixelColor(200,200).alpha(),0);QCOMPARE(dragging.pixelColor(900,900).alpha(),0);QCOMPARE(dragging.pixelColor(10,20),QColor(Qt::black));QCOMPARE(dragging.pixelColor(11,21),QColor(Qt::white));
   // Observe actual queued paints and backing pixels: render() would repaint
   // the full widget and hide stale strokes or incomplete damage. Full damage
   // is intentional: the compositor may sample a newer, larger outline.
   putgd::test::settle(20);RegionPaintProbe paints;w->installEventFilter(&paints);
   auto* connection=xcb_connect(nullptr,nullptr);
   auto backing=[&]{QImage image;auto* reply=xcb_get_image_reply(connection,xcb_get_image(connection,XCB_IMAGE_FORMAT_Z_PIXMAP,w->winId(),0,0,w->width(),w->height(),~0u),nullptr);
    if(reply&&reply->depth==32&&xcb_get_image_data_length(reply)>=w->width()*w->height()*4){image=QImage(xcb_get_image_data(reply),w->width(),w->height(),w->width()*4,QImage::Format_ARGB32).copy();}free(reply);return image;};
   QTest::mouseMove(w,QPoint(90,90));PUTGD_TRY_VERIFY(paints.count>0);
   QVERIFY(paints.painted.contains(w->rect()));QVERIFY(paints.painted.contains(QPoint(59,40)));QVERIFY(paints.painted.contains(QPoint(89,40)));
   // The paint reaches the X server after Qt flushes it; wait for the frame there.
   QImage grown;PUTGD_TRY_VERIFY(!(grown=backing()).isNull()&&grown.pixelColor(59,40).alpha()==0);QCOMPARE(grown.pixelColor(30,69).alpha(),0);QCOMPARE(grown.pixelColor(89,40),QColor(Qt::black));QCOMPARE(grown.pixelColor(88,40),QColor(Qt::white));
   paints.painted={};paints.count=0;QTest::mouseMove(w,QPoint(60,70));PUTGD_TRY_VERIFY(paints.count>0);
   QVERIFY(paints.painted.contains(w->rect()));QVERIFY(paints.painted.contains(QPoint(89,40)));QVERIFY(paints.painted.contains(QPoint(59,40)));
   // No scrim (#629): the old edge is cleared, and outside stays transparent.
   QImage shrunk;PUTGD_TRY_VERIFY(!(shrunk=backing()).isNull()&&shrunk.pixelColor(89,40).alpha()==0);QCOMPARE(shrunk.pixelColor(200,200).alpha(),0);QCOMPARE(shrunk.pixelColor(59,40),QColor(Qt::black));QCOMPARE(shrunk.pixelColor(58,40),QColor(Qt::white));
   w->removeEventFilter(&paints);xcb_disconnect(connection);
   QTest::mouseRelease(w,Qt::LeftButton,Qt::NoModifier,QPoint(60,70));QCOMPARE(selected,QRectF(10,20,50,50));QVERIFY(!surface());Error error;auto image=composeFrames({frame},selected,100000,error);QVERIFY(!error);QCOMPARE(image.size(),QSize(50,50));QCOMPARE(image.pixelColor(2,2),QColor(Qt::red));}
  const CaptureFrame native{"fixture",QRectF(30,40,10,8),QRect(30,40,10,8),1,0,{}};
  {CinnamonScreenshotFixture fixture;QVERIFY(fixture.registered);auto* request=new CinnamonRegionCapture({native},{});QPointer<CinnamonRegionCapture> guard=request;bool done=false;QList<CaptureFrame> frames;Error error;
   connect(request,&CinnamonRegionCapture::acquired,this,[&](QList<CaptureFrame> captured,Error value){frames=std::move(captured);error=value;done=true;});request->start();PUTGD_TRY_COMPARE(fixture.requests,1);
   QCOMPARE(fixture.rectangle,native.nativeRect);QVERIFY(!fixture.cursor&&!fixture.flash);QVERIFY(QFileInfo(fixture.path).size()>0);putgd::test::settle(50);PUTGD_TRY_VERIFY(!done);
   fixture.close();PUTGD_TRY_VERIFY(done);QVERIFY2(!error,qPrintable(error.message));QCOMPARE(frames.size(),1);QCOMPARE(frames[0].pixels.size(),native.nativeRect.size());QCOMPARE(frames[0].pixels.pixelColor(4,3),QColor(Qt::cyan));QVERIFY(!QFileInfo::exists(fixture.path));PUTGD_TRY_VERIFY(guard.isNull());}
  {CinnamonScreenshotFixture fixture;QVERIFY(fixture.registered);auto* request=new CinnamonRegionCapture({native},{});QPointer<CinnamonRegionCapture> guard=request;int completed=0;Error error;
   connect(request,&CinnamonRegionCapture::acquired,this,[&](QList<CaptureFrame>,Error value){error=value;++completed;});request->start();PUTGD_TRY_COMPARE(fixture.requests,1);request->cancel();QCOMPARE(completed,1);QCOMPARE(error.code,QString("cancelled"));QVERIFY(!QFileInfo::exists(QFileInfo(fixture.path).absolutePath()));fixture.close();putgd::test::settle(50);PUTGD_TRY_COMPARE(completed,1);PUTGD_TRY_VERIFY(guard.isNull());}
  for(auto mode:{CinnamonScreenshotFixture::WrongSize,CinnamonScreenshotFixture::WideDepth,CinnamonScreenshotFixture::Malformed,CinnamonScreenshotFixture::ServiceError}){CinnamonScreenshotFixture fixture(mode);QVERIFY(fixture.registered);auto* request=new CinnamonRegionCapture({native},{});bool done=false;Error error;
   connect(request,&CinnamonRegionCapture::acquired,this,[&](QList<CaptureFrame> frames,Error value){QVERIFY(frames.isEmpty());error=value;done=true;});request->start();PUTGD_TRY_COMPARE(fixture.requests,1);
   if(mode==CinnamonScreenshotFixture::WideDepth){QFile png(fixture.path);QVERIFY(png.open(QIODevice::ReadOnly));const auto header=png.read(33);QCOMPARE(header.size(),33);QCOMPARE(static_cast<unsigned char>(header[24]),16);}
   fixture.close();PUTGD_TRY_VERIFY(done);
   QCOMPARE(error.code,QString(mode==CinnamonScreenshotFixture::WrongSize?"capture_geometry":mode==CinnamonScreenshotFixture::ServiceError?"capture_unavailable":"capture_failed"));QVERIFY(!error.message.contains("arbitrary synthetic payload"));
   // Each failure names its check and frame geometry; never the service's text or a path (#331).
   const auto details=QString::fromUtf8(QJsonDocument(error.details).toJson(QJsonDocument::Compact));
   QCOMPARE(error.details["stage"].toString(),QString("cinnamon"));QCOMPARE(error.details["completion"].toString(),QString("inotify"));
   QCOMPARE(error.details["check"].toString(),QString(mode==CinnamonScreenshotFixture::WrongSize?"png_dimensions":mode==CinnamonScreenshotFixture::WideDepth?"png_depth":mode==CinnamonScreenshotFixture::Malformed?"png_size":"service_call"));
   QCOMPARE(error.details["output"].toString(),QString("fixture"));QCOMPARE(error.details["native"].toArray(),(QJsonArray{30,40,10,8}));
   QVERIFY(!details.contains("arbitrary synthetic payload"));QVERIFY(!details.contains(QFileInfo(fixture.path).absolutePath()));}
  {CinnamonScreenshotFixture fixture;QVERIFY(fixture.registered);ResourceLimits limits;limits.imageBytes=10*8*4-1;auto* request=new CinnamonRegionCapture({native},limits);bool done=false;Error error;
   connect(request,&CinnamonRegionCapture::acquired,this,[&](QList<CaptureFrame>,Error value){error=value;done=true;});request->start();QVERIFY(done);QCOMPARE(error.code,QString("input_excessive"));QCOMPARE(fixture.requests,0);}
 }
 // #331: three monitors of mixed sizes (a shorter middle one) at a tenth of
 // the reported layout. Every monitor is requested at once and decoded.
 static QList<CaptureFrame> mixedMonitorFrames(){
  return {{"DP-1-2",QRectF(0,0,256,144),QRect(0,0,256,144),1,0,{}},{"eDP-1",QRectF(256,0,192,108),QRect(256,0,192,108),1,0,{}},{"DP-3",QRectF(448,0,256,144),QRect(448,0,256,144),1,0,{}}};
 }
 void cinnamonRegionCaptureDecodesMixedMonitors() {
  CinnamonScreenshotFixture fixture;QVERIFY(fixture.registered);const auto layout=mixedMonitorFrames();
  auto* request=new CinnamonRegionCapture(layout,{});QPointer<CinnamonRegionCapture> guard=request;bool done=false;QList<CaptureFrame> frames;Error error;
  connect(request,&CinnamonRegionCapture::acquired,this,[&](QList<CaptureFrame> captured,Error value){frames=std::move(captured);error=value;done=true;});request->start();
  PUTGD_TRY_COMPARE(fixture.requests,3);QCOMPARE(fixture.rectangles,(QList<QRect>{layout[0].nativeRect,layout[1].nativeRect,layout[2].nativeRect}));
  fixture.close();PUTGD_TRY_VERIFY(done);QVERIFY2(!error,qPrintable(error.message));QCOMPARE(frames.size(),3);
  for(int i=0;i<3;++i){QCOMPARE(frames[i].pixels.size(),layout[i].nativeRect.size());QCOMPARE(frames[i].pixels.pixelColor(5,5),QColor(Qt::cyan));}
  PUTGD_TRY_VERIFY(guard.isNull());
 }
 void cinnamonRegionCaptureWithoutCompletionWatch_data(){QTest::addColumn<int>("mode");QTest::newRow("complete")<<int(CinnamonScreenshotFixture::Open);QTest::newRow("slow-writer")<<int(CinnamonScreenshotFixture::Slow);}
 void cinnamonRegionCaptureWithoutCompletionWatch() {
  // inotify instances are a per-user limit every program shares. When none is
  // left, region capture polls for finished PNGs instead of failing (#331).
  QFETCH(int,mode);CinnamonScreenshotFixture fixture{CinnamonScreenshotFixture::Mode(mode)};QVERIFY(fixture.registered);const auto layout=mixedMonitorFrames();
  CinnamonRegionCapture::setWatchErrorForTesting(EMFILE);auto restore=qScopeGuard([]{CinnamonRegionCapture::setWatchErrorForTesting(0);});
  auto* request=new CinnamonRegionCapture(layout,{});QPointer<CinnamonRegionCapture> guard=request;bool done=false;QList<CaptureFrame> frames;Error error;
  connect(request,&CinnamonRegionCapture::acquired,this,[&](QList<CaptureFrame> captured,Error value){frames=std::move(captured);error=value;done=true;});
  QTest::ignoreMessage(QtWarningMsg,"Cinnamon capture completion watch unavailable (EMFILE); polling for finished PNGs");request->start();
  PUTGD_TRY_COMPARE(fixture.requests,3);
  if(mode==CinnamonScreenshotFixture::Slow){putgd::test::settle(200);PUTGD_TRY_VERIFY(!done);}
  // The last writer is still open: complete bytes, not a close, finish polling.
  PUTGD_TRY_VERIFY(done);QVERIFY2(!error,qPrintable(error.code+": "+error.message));QCOMPARE(frames.size(),3);
  for(int i=0;i<3;++i){QCOMPARE(frames[i].pixels.size(),layout[i].nativeRect.size());QCOMPARE(frames[i].pixels.pixelColor(5,5),QColor(Qt::cyan));}
  fixture.close();PUTGD_TRY_VERIFY(guard.isNull());
 }
 void encodedImageResultIsServedToNativeClipboardConsumer() {
  QImage expected(17,11,QImage::Format_ARGB32);expected.fill(Qt::yellow);
  CopyPayload payload;payload.kind="image";QBuffer buffer(&payload.encodedImage);QVERIFY(buffer.open(QIODevice::WriteOnly));QVERIFY(expected.save(&buffer,"PNG"));buffer.close();
  X11Clipboard owner;bool copied=false;owner.copy(payload,[&](Error error){QVERIFY(!error);copied=true;});QVERIFY(copied);
  PayloadResult result;bool imported=false;X11Clipboard consumer;
  consumer.acquire({},[&](PayloadResult value){result=std::move(value);imported=true;});
  PUTGD_TRY_VERIFY(imported);QVERIFY2(!result.error,qPrintable(result.error.message));QCOMPARE(result.payload.kind,QString("image"));QCOMPARE(result.payload.image,expected);
 }
 void simultaneousClipboardImportsAreSerialized() {
  X11SelectionFixture owner({{"UTF8_STRING","same detached synthetic source"}},true);X11Clipboard importer;
  int completed=0;PayloadResult first,second;importer.acquire({},[&](PayloadResult r){first=r;++completed;});importer.acquire({},[&](PayloadResult r){second=r;++completed;});PUTGD_TRY_COMPARE(completed,2);QVERIFY2(!first.error,qPrintable(first.error.message));QVERIFY2(!second.error,qPrintable(second.error.message));QCOMPARE(first.payload.bytes,second.payload.bytes);QCOMPARE(first.payload.bytes,QByteArray("same detached synthetic source"));
 }
 void cancellingRegionDoesNotCancelClipboardImport() {
  bool imported=false,cancelled=false;PayloadResult clipboard;X11SelectionFixture owner({{"UTF8_STRING","independent clipboard source"}});X11SourceAdapter source;
  source.acquire("clipboard",{},[&](PayloadResult r){clipboard=r;imported=true;});source.acquire("region",{},[&](PayloadResult r){cancelled=r.error.code=="cancelled";});source.cancelAcquisition();PUTGD_TRY_VERIFY(cancelled);PUTGD_TRY_VERIFY(imported);QVERIFY2(!clipboard.error,qPrintable(clipboard.error.message));QCOMPARE(clipboard.payload.bytes,QByteArray("independent clipboard source"));
 }
 void regionRetainsExternalFocusAndNativeEscape() {
  X11DesktopFixture desktop;QVERIFY(desktop.window);QCOMPARE(desktop.inputFocus(),desktop.window);
  bool cancelled=false;X11SourceAdapter source;source.acquire("region",{},[&](PayloadResult r){cancelled=r.error.code=="cancelled";});putgd::test::settle(20);PUTGD_TRY_COMPARE(desktop.inputFocus(),desktop.window);
  desktop.key(XK_Escape,true);desktop.key(XK_Escape,false);PUTGD_TRY_VERIFY(cancelled);QCOMPARE(desktop.inputFocus(),desktop.window);
  bool completed=false;PayloadResult captured;source.acquire("region",{},[&](PayloadResult result){captured=std::move(result);completed=true;});
  QWidget* surface=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(widget->objectName()=="putgd-region-overlay"&&widget->isVisible())surface=widget;QVERIFY(surface);
  auto repaintDesktop=[&](uint32_t color){xcb_change_window_attributes(desktop.connection,desktop.window,XCB_CW_BACK_PIXEL,&color);xcb_clear_area(desktop.connection,0,desktop.window,0,0,0,0);xcb_flush(desktop.connection);desktop.inputFocus();};
  repaintDesktop(0xff0000); // The desktop changes while the selector is open.
  QTest::mousePress(surface,Qt::LeftButton,Qt::NoModifier,surface->mapFromGlobal(QPoint(30,30)));QTest::mouseMove(surface,surface->mapFromGlobal(QPoint(70,60)));
  repaintDesktop(0x0000ff); // It changes again during the drag, before capture.
  QTest::mouseRelease(surface,Qt::LeftButton,Qt::NoModifier,surface->mapFromGlobal(QPoint(70,60)));PUTGD_TRY_VERIFY(completed);QVERIFY2(!captured.error,qPrintable(captured.error.message));QCOMPARE(captured.payload.image.size(),QSize(40,30));
  for(int y=0;y<captured.payload.image.height();++y){for(int x=0;x<captured.payload.image.width();++x)QCOMPARE(captured.payload.image.pixelColor(x,y),QColor(Qt::blue));}
  QCOMPARE(desktop.inputFocus(),desktop.window);
 }
 void heldGlobalChordHandsKeyboardToSelector() {
  X11DesktopFixture desktop;bool invoked=false,cancelled=false;X11SourceAdapter source;source.bindShortcuts({{"region","Ctrl+Shift+1"}});
  connect(&source,&X11SourceAdapter::actionRequested,this,[&](const QString& action){if(action=="region"){invoked=true;source.acquire(action,{},[&](PayloadResult r){cancelled=r.error.code=="cancelled";});}});
  desktop.key(XK_Control_L,true);desktop.key(XK_Shift_L,true);desktop.key(XK_1,true);PUTGD_TRY_VERIFY(invoked);
  // The trigger keys are still held. Escape must reach the selector rather than
  // remain trapped in the shortcut connection's active passive-key grab.
  desktop.key(XK_Escape,true);desktop.key(XK_Escape,false);PUTGD_TRY_VERIFY(cancelled);QCOMPARE(desktop.inputFocus(),desktop.window);
 }
 void clipboardQueueBoundAndFrozenOwner() {
  int completed=0;PayloadResult a,b,excess;X11Clipboard importer;X11SelectionFixture first({{"UTF8_STRING","first owner"}},true);ResourceLimits limits;limits.pendingOperations=1;limits.concurrentUploads=1;
  importer.acquire(limits,[&](PayloadResult r){a=r;++completed;});importer.acquire(limits,[&](PayloadResult r){b=r;++completed;});importer.acquire(limits,[&](PayloadResult r){excess=r;});QCOMPARE(excess.error.code,QString("queue_full"));
  X11SelectionFixture replacement({{"UTF8_STRING","new owner"}});PUTGD_TRY_COMPARE(completed,2);QCOMPARE(a.error.code,QString("clipboard_changed"));QCOMPARE(b.error.code,QString("clipboard_changed"));
 }
 void liveDigitRebindRemovesOldChordAndKeepsOtherActions() {
  X11DesktopFixture desktop;X11Shortcuts shortcuts;QStringList actions;
  connect(&shortcuts,&X11Shortcuts::activated,this,[&](const QString& action){actions.append(action);});
  auto chord=[&](bool shift,bool alt,int digit){
   desktop.key(XK_Control_L,true);if(shift)desktop.key(XK_Shift_L,true);if(alt)desktop.key(XK_Alt_L,true);
   desktop.key(digit,true);desktop.key(digit,false);
   if(alt)desktop.key(XK_Alt_L,false);
   if(shift)desktop.key(XK_Shift_L,false);
   desktop.key(XK_Control_L,false);
   desktop.inputFocus();
  };
  auto bind=[&](const QString& region){shortcuts.bind({{"region",region},{"desktop","Ctrl+Shift+3"}});QVERIFY(shortcuts.status()["region"].toObject()["bound"].toBool());QVERIFY(shortcuts.status()["desktop"].toObject()["bound"].toBool());};
  bind("Ctrl+Shift+1");chord(true,false,XK_1);PUTGD_TRY_COMPARE(actions,QStringList{"region"});
  bind("Ctrl+1");
  // A separate client's checked grab proves removal at the server, without
  // inferring absence from a short event-loop wait alone.
  const auto root=xcb_setup_roots_iterator(xcb_get_setup(desktop.connection)).data->root;
  auto removed=[&](uint16_t modifiers){
   auto* error=xcb_request_check(desktop.connection,xcb_grab_key_checked(desktop.connection,0,root,modifiers,desktop.code(XK_1),XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC));
   const bool available=!error;free(error);
   if(available){xcb_ungrab_key(desktop.connection,desktop.code(XK_1),root,modifiers);desktop.inputFocus();}
   return available;
  };
  QVERIFY(removed(XCB_MOD_MASK_CONTROL|XCB_MOD_MASK_SHIFT));
  chord(true,false,XK_1);putgd::test::settle(30);PUTGD_TRY_COMPARE(actions.size(),1);
  chord(false,false,XK_1);PUTGD_TRY_COMPARE(actions.size(),2);QCOMPARE(actions.last(),QString("region"));
  chord(true,false,XK_3);PUTGD_TRY_COMPARE(actions.size(),3);QCOMPARE(actions.last(),QString("desktop"));
  bind("Ctrl+Alt+1");QVERIFY(removed(XCB_MOD_MASK_CONTROL));
  chord(false,false,XK_1);putgd::test::settle(30);PUTGD_TRY_COMPARE(actions.size(),3);
  chord(false,true,XK_1);PUTGD_TRY_COMPARE(actions.size(),4);QCOMPARE(actions.last(),QString("region"));
  chord(true,false,XK_3);PUTGD_TRY_COMPARE(actions.size(),5);QCOMPARE(actions.last(),QString("desktop"));
  putgd::test::settle(30);PUTGD_TRY_COMPARE(actions.size(),5);
 }
 void asciiLetterShortcutNeedsNoImplicitShift() {
  X11DesktopFixture desktop;X11Shortcuts shortcuts;int activated=0;connect(&shortcuts,&X11Shortcuts::activated,this,[&](const QString&){++activated;});shortcuts.bind({{"region","Ctrl+A"}});QVERIFY(shortcuts.status()["region"].toObject()["bound"].toBool());
  desktop.key(XK_Control_L,true);desktop.key(XK_a,true);desktop.key(XK_a,false);desktop.key(XK_Control_L,false);PUTGD_TRY_COMPARE(activated,1);
 }
 void shiftedPrintableShortcutUsesActualChord() {
  X11DesktopFixture desktop;X11Shortcuts shortcuts;int activated=0;connect(&shortcuts,&X11Shortcuts::activated,this,[&](const QString&){++activated;});shortcuts.bind({{"region","Ctrl+!"}});QVERIFY(shortcuts.status()["region"].toObject()["bound"].toBool());
  desktop.key(XK_Control_L,true);desktop.key(XK_1,true);desktop.key(XK_1,false);desktop.key(XK_Control_L,false);putgd::test::settle(20);PUTGD_TRY_COMPARE(activated,0);
  desktop.key(XK_Control_L,true);desktop.key(XK_Shift_L,true);desktop.key(XK_1,true);desktop.key(XK_1,false);desktop.key(XK_Shift_L,false);desktop.key(XK_Control_L,false);PUTGD_TRY_COMPARE(activated,1);
 }
 void keyboardMappingChangeRebindsActualCodes() {
  X11DesktopFixture desktop;X11Shortcuts shortcuts;int activated=0,changes=0;connect(&shortcuts,&X11Shortcuts::activated,this,[&](const QString&){++activated;});connect(&shortcuts,&X11Shortcuts::bindingsChanged,this,[&]{++changes;});shortcuts.bind({{"region","Ctrl+F6"}});
  const auto code=xcb_get_setup(desktop.connection)->max_keycode;auto* old=xcb_get_keyboard_mapping_reply(desktop.connection,xcb_get_keyboard_mapping(desktop.connection,code,1),nullptr);QVERIFY(old);QList<xcb_keysym_t> previous;auto* values=xcb_get_keyboard_mapping_keysyms(old);for(int i=0;i<old->keysyms_per_keycode;++i)previous.append(values[i]);free(old);
  struct Restore {xcb_connection_t* connection;xcb_keycode_t code;QList<xcb_keysym_t> values;~Restore(){xcb_change_keyboard_mapping(connection,1,code,values.size(),values.constData());xcb_flush(connection);}} restore{desktop.connection,code,previous};
  QList<xcb_keysym_t> replacement(previous.size(),XCB_NO_SYMBOL);replacement[0]=XK_F6;if(replacement.size()>1)replacement[1]=XK_F6;const auto before=changes;xcb_change_keyboard_mapping(desktop.connection,1,code,replacement.size(),replacement.constData());xcb_flush(desktop.connection);PUTGD_TRY_VERIFY(changes>before);QVERIFY(shortcuts.status()["region"].toObject()["bound"].toBool());
  desktop.key(XK_Control_L,true);desktop.rawKey(code,true);desktop.rawKey(code,false);desktop.key(XK_Control_L,false);PUTGD_TRY_COMPARE(activated,1);
 }
 void mappingRefreshKeepsUnchangedChordGrabbed() {
  X11DesktopFixture desktop;X11MappingBarrier barrier;X11Shortcuts shortcuts;int activated=0,changes=0;bool injected=false;
  connect(&shortcuts,&X11Shortcuts::activated,this,[&](const QString& action){QCOMPARE(action,QString("region"));++activated;});
  connect(&shortcuts,&X11Shortcuts::bindingsChanged,this,[&]{++changes;});
  shortcuts.bind({{"region","Ctrl+Shift+1"}});QVERIFY(shortcuts.status()["region"].toObject()["bound"].toBool());
  barrier.arm([&](xcb_connection_t* connection){
   // Finish any earlier ungrabs before the independent client's key events.
   auto* sync=xcb_get_input_focus_reply(connection,xcb_get_input_focus(connection),nullptr);QVERIFY(sync);free(sync);
   desktop.key(XK_Control_L,true);desktop.key(XK_Shift_L,true);desktop.key(XK_1,true);desktop.key(XK_1,false);desktop.key(XK_Shift_L,false);desktop.key(XK_Control_L,false);
   QCOMPARE(desktop.inputFocus(),desktop.window);injected=true;
  });
  const auto code=xcb_get_setup(desktop.connection)->max_keycode;
  auto* mapping=xcb_get_keyboard_mapping_reply(desktop.connection,xcb_get_keyboard_mapping(desktop.connection,code,1),nullptr);QVERIFY(mapping);
  // A real server notification with unchanged symbols must never drop a grab.
  xcb_change_keyboard_mapping(desktop.connection,1,code,mapping->keysyms_per_keycode,xcb_get_keyboard_mapping_keysyms(mapping));free(mapping);xcb_flush(desktop.connection);
  PUTGD_TRY_VERIFY(injected);QVERIFY(changes>1);PUTGD_TRY_COMPARE(activated,1);QVERIFY(shortcuts.status()["region"].toObject()["bound"].toBool());
 }
 void mappingConflictReleasesRejectedGrabsAndPreservesOtherActions() {
  X11DesktopFixture desktop;X11Shortcuts shortcuts;int changes=0;QStringList activated;
  connect(&shortcuts,&X11Shortcuts::bindingsChanged,this,[&]{++changes;});
  connect(&shortcuts,&X11Shortcuts::activated,this,[&](const QString& action){activated.append(action);});
  const QHash<QString,QString> bindings{{"region","Ctrl+F6"},{"clipboard","Ctrl+Shift+1"}};shortcuts.bind(bindings);
  QVERIFY(shortcuts.status()["region"].toObject()["bound"].toBool());QVERIFY(shortcuts.status()["clipboard"].toObject()["bound"].toBool());
  const auto original=desktop.code(XK_F6),extra=xcb_get_setup(desktop.connection)->max_keycode;QVERIFY(original&&original!=extra);
  auto* mapping=xcb_get_keyboard_mapping_reply(desktop.connection,xcb_get_keyboard_mapping(desktop.connection,extra,1),nullptr);QVERIFY(mapping);
  const auto* values=xcb_get_keyboard_mapping_keysyms(mapping);QList<xcb_keysym_t> previous;for(int i=0;i<mapping->keysyms_per_keycode;++i)previous.append(values[i]);free(mapping);
  struct Restore {xcb_connection_t* connection;xcb_keycode_t code;QList<xcb_keysym_t> values;~Restore(){xcb_change_keyboard_mapping(connection,1,code,values.size(),values.constData());xcb_flush(connection);}} restore{desktop.connection,extra,previous};
  const auto root=xcb_setup_roots_iterator(xcb_get_setup(desktop.connection)).data->root;
  auto* error=xcb_request_check(desktop.connection,xcb_grab_key_checked(desktop.connection,0,root,XCB_MOD_MASK_CONTROL,extra,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC));QVERIFY(!error);
  QList<xcb_keysym_t> replacement(previous.size(),XCB_NO_SYMBOL);replacement[0]=XK_F6;if(replacement.size()>1)replacement[1]=XK_F6;
  const auto before=changes;xcb_change_keyboard_mapping(desktop.connection,1,extra,replacement.size(),replacement.constData());xcb_flush(desktop.connection);PUTGD_TRY_VERIFY(changes>before);
  QVERIFY(!shortcuts.status()["region"].toObject()["bound"].toBool());QCOMPARE(shortcuts.status()["region"].toObject()["error"].toString(),QString("shortcut_conflict"));QVERIFY(shortcuts.status()["clipboard"].toObject()["bound"].toBool());
  // Rejection must remove both retained and newly installed lock variants. The
  // resident releases them on its own connection, which the X server may serve
  // after this one, so each grab is retried until it succeeds or the wait ends.
  auto grabbable=[&](xcb_keycode_t code,uint16_t mask){auto* refused=xcb_request_check(desktop.connection,xcb_grab_key_checked(desktop.connection,0,root,mask,code,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC));
   if(refused){free(refused);return false;}xcb_ungrab_key(desktop.connection,code,root,mask);return true;};
  for(const auto code:{original,extra})for(uint16_t mask=0;mask<256;++mask){
   if(!(mask&XCB_MOD_MASK_CONTROL))continue;
   PUTGD_TRY_VERIFY(grabbable(code,mask));
  }
  desktop.inputFocus();shortcuts.bind(bindings);QVERIFY(shortcuts.status()["region"].toObject()["bound"].toBool());
  desktop.key(XK_Control_L,true);desktop.key(XK_Shift_L,true);desktop.key(XK_1,true);desktop.key(XK_1,false);desktop.key(XK_Shift_L,false);desktop.key(XK_Control_L,false);PUTGD_TRY_COMPARE(activated,QStringList{"clipboard"});
  // The same native chord can move between actions; disabled and duplicate
  // actions must not keep ownership after the reconciliation.
  shortcuts.bind({{"first","Ctrl+Shift+1"},{"second","Ctrl+Shift+1"},{"disabled",""}});
  QVERIFY(shortcuts.status()["first"].toObject()["bound"].toBool());QCOMPARE(shortcuts.status()["second"].toObject()["error"].toString(),QString("shortcut_conflict"));QCOMPARE(shortcuts.status()["disabled"].toObject()["error"].toString(),QString("disabled"));
  desktop.key(XK_Control_L,true);desktop.key(XK_Shift_L,true);desktop.key(XK_1,true);desktop.key(XK_1,false);desktop.key(XK_Shift_L,false);desktop.key(XK_Control_L,false);PUTGD_TRY_COMPARE(activated,(QStringList{"clipboard","first"}));
  shortcuts.bind({});X11Shortcuts next;next.bind({{"region","Ctrl+Shift+1"}});QVERIFY(next.status()["region"].toObject()["bound"].toBool());
 }
 void nonresponsiveOwnerKeepsEventLoopAlive() {
  xcb_connection_t* c=xcb_connect(nullptr,nullptr);auto root=xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
  auto w=xcb_generate_id(c);xcb_create_window(c,XCB_COPY_FROM_PARENT,w,root,0,0,1,1,0,XCB_WINDOW_CLASS_INPUT_ONLY,XCB_COPY_FROM_PARENT,0,nullptr);
  auto a=xcb_intern_atom_reply(c,xcb_intern_atom(c,0,9,"CLIPBOARD"),nullptr);QVERIFY(a);xcb_set_selection_owner(c,w,a->atom,XCB_CURRENT_TIME);free(a);xcb_flush(c);
  X11Clipboard importer;bool done=false;PayloadResult result;int heartbeats=0;QTimer heartbeat;connect(&heartbeat,&QTimer::timeout,this,[&]{++heartbeats;});heartbeat.start(10);
  importer.acquire({},[&](PayloadResult r){result=r;done=true;});PUTGD_TRY_VERIFY_WITH_TIMEOUT(done,6500);QVERIFY(heartbeats>50);QCOMPARE(result.error.code,QString("clipboard_timeout"));xcb_disconnect(c);
  CinnamonScreenshotFixture fixture;QVERIFY(fixture.registered);CaptureFrame frame{"fixture",QRectF(0,0,10,8),QRect(0,0,10,8),1,0,{}};auto* request=new CinnamonRegionCapture({frame},{});bool settled=false;Error error;
  connect(request,&CinnamonRegionCapture::acquired,this,[&](QList<CaptureFrame>,Error value){error=value;settled=true;});const int before=heartbeats;request->start();PUTGD_TRY_COMPARE(fixture.requests,1);PUTGD_TRY_VERIFY_WITH_TIMEOUT(settled,12000);QCOMPARE(error.code,QString("capture_timeout"));QVERIFY(heartbeats-before>50);QVERIFY(!QFileInfo::exists(fixture.path));fixture.close();
 }
 void recordingDiscardsShortcutPressQueuedBeforeNativePause() {
  X11DesktopFixture desktop;X11Shortcuts shortcuts;int activated=0;connect(&shortcuts,&X11Shortcuts::activated,this,[&](const QString&){++activated;});
  shortcuts.bind({{"region","Ctrl+1"}});QVERIFY(shortcuts.status()["region"].toObject()["bound"].toBool());
  desktop.key(XK_Control_L,true);desktop.key(XK_1,true);desktop.key(XK_1,false);desktop.key(XK_Control_L,false);desktop.inputFocus();
  Error error;shortcuts.setRecording(true,[&](Error value){error=value;});QVERIFY(!error);putgd::test::settle(20);PUTGD_TRY_COMPARE(activated,0);
  shortcuts.setRecording(false,[&](Error value){error=value;});QVERIFY(!error);putgd::test::settle(20);PUTGD_TRY_COMPARE(activated,0);
  desktop.key(XK_Control_L,true);desktop.key(XK_1,true);desktop.key(XK_1,false);desktop.key(XK_Control_L,false);PUTGD_TRY_COMPARE(activated,1);
 }
 void recordingReleasesGrabsAndRestoresLatestSavedBindings() {
  X11Shortcuts saved,competitor;saved.bind({{"region","Ctrl+Shift+1"}});QVERIFY(saved.status()["region"].toObject()["bound"].toBool());
  bool acknowledged=false;Error error;saved.setRecording(true,[&](Error value){error=value;acknowledged=true;});QVERIFY(acknowledged);QVERIFY(!error);
  competitor.bind({{"region","Ctrl+Shift+1"}});QVERIFY(competitor.status()["region"].toObject()["bound"].toBool());
  saved.bind({{"region","Ctrl+Shift+2"}});QCOMPARE(saved.status()["region"].toObject()["requested"].toString(),QString("Ctrl+Shift+2"));QVERIFY(!saved.status()["region"].toObject()["bound"].toBool());
  competitor.bind({});saved.setRecording(false,[&](Error value){error=value;});QVERIFY(!error);QVERIFY(saved.status()["region"].toObject()["bound"].toBool());
  competitor.bind({{"old","Ctrl+Shift+1"},{"new","Ctrl+Shift+2"}});QVERIFY(competitor.status()["old"].toObject()["bound"].toBool());QVERIFY(!competitor.status()["new"].toObject()["bound"].toBool());
 }
 void conflictsAreReported() {
  X11Shortcuts first,second;QVERIFY(first.available());QVERIFY(second.available());
  first.bind({{"region","Ctrl+Shift+1"}});second.bind({{"region","Ctrl+Shift+1"}});
  QVERIFY(first.status()["region"].toObject()["bound"].toBool());QVERIFY(!second.status()["region"].toObject()["bound"].toBool());QCOMPARE(second.status()["region"].toObject()["error"].toString(),QString("shortcut_conflict"));
 }
 void residentWindowIsRejected() {
  QWidget window;window.resize(40,30);window.show();
  auto* c=xcb_connect(nullptr,nullptr);const auto root=xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
  auto* atom=xcb_intern_atom_reply(c,xcb_intern_atom(c,0,18,"_NET_ACTIVE_WINDOW"),nullptr);QVERIFY(atom);
  const xcb_window_t own=window.winId();xcb_change_property(c,XCB_PROP_MODE_REPLACE,root,atom->atom,XCB_ATOM_WINDOW,32,1,&own);xcb_flush(c);
  auto* sync=xcb_get_input_focus_reply(c,xcb_get_input_focus(c),nullptr);free(sync);
  X11Capture capture;Error error;capture.freeze("window",error);QCOMPARE(error.code,QString("capture_target"));
  xcb_delete_property(c,root,atom->atom);free(atom);xcb_disconnect(c);
 }
 void uiProcessWindowRejectedAndFrozenExternalPreserved() {
  X11DesktopFixture desktop;QVERIFY(desktop.window);
  auto* c=desktop.connection;const auto root=xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
  auto atom=[&](const char* name){auto* value=xcb_intern_atom_reply(c,xcb_intern_atom(c,0,strlen(name),name),nullptr);const auto result=value?value->atom:xcb_atom_t(0);free(value);return result;};
  const auto active=atom("_NET_ACTIVE_WINDOW"),pidAtom=atom("_NET_WM_PID");const uint32_t helperPid=424242;
  xcb_change_property(c,XCB_PROP_MODE_REPLACE,root,active,XCB_ATOM_WINDOW,32,1,&desktop.window);
  xcb_change_property(c,XCB_PROP_MODE_REPLACE,desktop.window,pidAtom,XCB_ATOM_CARDINAL,32,1,&helperPid);desktop.inputFocus();
  X11Capture capture;Error error;capture.setUiProcess(helperPid);capture.freeze("window",error);QCOMPARE(error.code,QString("capture_target"));
  capture.setUiProcess(0);error={};const auto frozen=capture.freeze("window",error);QVERIFY2(!error,qPrintable(error.message));QCOMPARE(frozen.window,desktop.window);
  // Registration changes after invocation cannot retarget a frozen external window.
  capture.setUiProcess(helperPid);QVERIFY(capture.unchanged(frozen,error));QVERIFY(!error);
  capture.setUiProcess(helperPid+1);error={};capture.freeze("window",error);QVERIFY(!error);
  capture.setUiProcess(0);xcb_delete_property(c,root,active);
 }
 void residentChildWindowRejectedWithoutRegistration() {
  // The Edit & upload editor never registers; this process stands in for the
  // resident that started it, so the window of its unregistered child is refused.
  QProcess editor;editor.start("sleep",{"30"});QVERIFY(editor.waitForStarted());
  QProcess finished;finished.start("true",{});QVERIFY(finished.waitForStarted());const auto gone=uint32_t(finished.processId());QVERIFY(finished.waitForFinished());
  X11DesktopFixture desktop;QVERIFY(desktop.window);
  auto* c=desktop.connection;const auto root=xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
  auto atom=[&](const char* name){auto* value=xcb_intern_atom_reply(c,xcb_intern_atom(c,0,strlen(name),name),nullptr);const auto result=value?value->atom:xcb_atom_t(0);free(value);return result;};
  const auto active=atom("_NET_ACTIVE_WINDOW"),pidAtom=atom("_NET_WM_PID");
  auto own=[&](uint32_t pid){xcb_change_property(c,XCB_PROP_MODE_REPLACE,desktop.window,pidAtom,XCB_ATOM_CARDINAL,32,1,&pid);desktop.inputFocus();};
  xcb_change_property(c,XCB_PROP_MODE_REPLACE,root,active,XCB_ATOM_WINDOW,32,1,&desktop.window);
  own(uint32_t(editor.processId()));
  X11Capture capture;Error error;capture.freeze("window",error);QCOMPARE(error.code,QString("capture_target"));
  capture.setUiProcess(424242);error={};capture.freeze("window",error);QCOMPARE(error.code,QString("capture_target"));capture.setUiProcess(0);
  // A process this one didn't start, and a PID with no /proc entry, stay external.
  own(uint32_t(getppid()));error={};auto frozen=capture.freeze("window",error);QVERIFY2(!error,qPrintable(error.message));QCOMPARE(frozen.window,desktop.window);
  own(gone);error={};frozen=capture.freeze("window",error);QVERIFY2(!error,qPrintable(error.message));QCOMPARE(frozen.window,desktop.window);
  xcb_delete_property(c,root,active);editor.kill();editor.waitForFinished();
 }
 void activeWindowFrozenAndMovedTargetRejected() {
  auto* c=xcb_connect(nullptr,nullptr);auto* screen=xcb_setup_roots_iterator(xcb_get_setup(c)).data;auto root=screen->root;
  auto atom=[&](const char* name){auto* r=xcb_intern_atom_reply(c,xcb_intern_atom(c,0,strlen(name),name),nullptr);auto result=r?r->atom:xcb_atom_t(0);free(r);return result;};
  auto makeWindow=[&](int x,int y,int width,int height,uint32_t color){auto w=xcb_generate_id(c);uint32_t values[]{color,1};xcb_create_window(c,screen->root_depth,w,root,x,y,width,height,0,XCB_WINDOW_CLASS_INPUT_OUTPUT,screen->root_visual,XCB_CW_BACK_PIXEL|XCB_CW_OVERRIDE_REDIRECT,values);xcb_map_window(c,w);return w;};
  const auto first=makeWindow(100,100,100,80,0xff0000);const auto second=makeWindow(140,120,40,30,0x0000ff);auto active=atom("_NET_ACTIVE_WINDOW"),extents=atom("_NET_FRAME_EXTENTS");uint32_t sides[]{3,4,5,6};xcb_change_property(c,XCB_PROP_MODE_REPLACE,first,extents,XCB_ATOM_CARDINAL,32,4,sides);xcb_change_property(c,XCB_PROP_MODE_REPLACE,root,active,XCB_ATOM_WINDOW,32,1,&first);xcb_flush(c);auto* sync=xcb_get_input_focus_reply(c,xcb_get_input_focus(c),nullptr);free(sync);
  X11Capture capture;Error error;auto target=capture.freeze("window",error);QVERIFY2(!error,qPrintable(error.message));QCOMPARE(target.window,first);QCOMPARE(target.nativeWindowRect,QRect(97,95,107,91));
  xcb_change_property(c,XCB_PROP_MODE_REPLACE,root,active,XCB_ATOM_WINDOW,32,1,&second);xcb_flush(c);auto frames=capture.frames(target,error);QVERIFY(!error);QVERIFY(!frames.isEmpty());QCOMPARE(frames.first().nativeRect,target.nativeWindowRect);auto image=composeFrames(frames,target.logicalSelection,10000000,error);QVERIFY(!error);QCOMPARE(image.size(),QSize(107,91));QCOMPARE(image.pixelColor(10,10),QColor(Qt::red));QCOMPARE(image.pixelColor(50,30),QColor(Qt::blue));
  uint32_t x=110;xcb_configure_window(c,first,XCB_CONFIG_WINDOW_X,&x);xcb_flush(c);sync=xcb_get_input_focus_reply(c,xcb_get_input_focus(c),nullptr);free(sync);QVERIFY(!capture.unchanged(target,error));QCOMPARE(error.code,QString("capture_target"));xcb_destroy_window(c,first);xcb_destroy_window(c,second);xcb_delete_property(c,root,active);xcb_disconnect(c);
 }
 void desktopIsDetached() {
  X11Capture capture;QVERIFY(capture.available());QVERIFY(!capture.cinnamonCompositor());
  auto* c=xcb_connect(nullptr,nullptr);const auto root=xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
  const auto window=xcb_generate_id(c);xcb_create_window(c,XCB_COPY_FROM_PARENT,window,root,0,0,1,1,0,XCB_WINDOW_CLASS_INPUT_ONLY,XCB_COPY_FROM_PARENT,0,nullptr);
  auto atom=[&](const char* name){auto* value=xcb_intern_atom_reply(c,xcb_intern_atom(c,0,strlen(name),name),nullptr);const auto result=value?value->atom:xcb_atom_t(0);free(value);return result;};
  const auto compositor=atom("_NET_WM_CM_S0"),supporting=atom("_NET_SUPPORTING_WM_CHECK"),version=atom("_MUFFIN_VERSION");
  auto sync=[&]{xcb_flush(c);auto* value=xcb_get_input_focus_reply(c,xcb_get_input_focus(c),nullptr);free(value);};
  xcb_set_selection_owner(c,window,compositor,XCB_CURRENT_TIME);sync();QVERIFY(!capture.cinnamonCompositor());
  xcb_change_property(c,XCB_PROP_MODE_REPLACE,root,supporting,XCB_ATOM_WINDOW,32,1,&window);xcb_change_property(c,XCB_PROP_MODE_REPLACE,window,supporting,XCB_ATOM_WINDOW,32,1,&window);
  xcb_change_property(c,XCB_PROP_MODE_REPLACE,window,version,atom("UTF8_STRING"),8,5,"6.6.1");sync();QVERIFY(capture.cinnamonCompositor()); // The owner has no WM_CLASS, like real Muffin.
  xcb_delete_property(c,window,version);sync();QVERIFY(!capture.cinnamonCompositor());xcb_delete_property(c,root,supporting);xcb_destroy_window(c,window);xcb_disconnect(c);
  Error error;auto target=capture.freeze("desktop",error);QVERIFY2(!error,qPrintable(error.message));auto frames=capture.frames(target,error);QVERIFY2(!error,qPrintable(error.message));QVERIFY(!frames.isEmpty());QVERIFY(!frames.first().pixels.isNull());
 }

 void shortcutFiresOnceWhileAnotherClientHoldsTheKeyboard() {
  // #341: an open menu's active grab stops every passive grab. The XInput2
  // raw path fires instead, and exactly once when both paths see the press.
  X11DesktopFixture desktop;X11Shortcuts shortcuts;QStringList actions;
  connect(&shortcuts,&X11Shortcuts::activated,this,[&](const QString& action){actions.append(action);});
  shortcuts.bind({{"region","Ctrl+Shift+1"},{"desktop","Ctrl+Shift+3"}});
  QVERIFY(shortcuts.status()["region"].toObject()["bound"].toBool());
  const auto paths=shortcuts.keyPaths();QVERIFY(paths["passive"].toBool());QVERIFY(paths["raw"].toObject()["available"].toBool());QVERIFY(!paths["raw"].toObject()["xinput"].toString().isEmpty());
  auto chord=[&](int digit,bool repeat=false){
   desktop.key(XK_Control_L,true);desktop.key(XK_Shift_L,true);desktop.key(digit,true);if(repeat){desktop.key(digit,true);desktop.key(digit,true);}desktop.key(digit,false);desktop.key(XK_Shift_L,false);desktop.key(XK_Control_L,false);desktop.inputFocus();
  };
  auto last=[&]{return shortcuts.keyPaths()["lastActivation"].toObject();};
  chord(XK_1);PUTGD_TRY_COMPARE(actions,QStringList{"region"});QCOMPARE(last()["path"].toString(),QString("passive"));QCOMPARE(last()["action"].toString(),QString("region"));
  PUTGD_COMPARE_STEADY(actions.size(),1,50);
  {
   X11GrabbingClient menu;QVERIFY(menu.grabbed);
   chord(XK_3);PUTGD_TRY_COMPARE(actions.size(),2);QCOMPARE(actions.last(),QString("desktop"));QCOMPARE(last()["path"].toString(),QString("raw"));
   // The menu keeps receiving the keys: put.gd only listens.
   PUTGD_TRY_VERIFY(menu.keyPresses()>=3);
   chord(XK_1,true);PUTGD_TRY_COMPARE(actions.size(),3);QCOMPARE(actions.last(),QString("region"));
   // Neither the repeats, an unbound chord nor a missing modifier fire.
   desktop.key(XK_Control_L,true);desktop.key(XK_1,true);desktop.key(XK_1,false);desktop.key(XK_Control_L,false);desktop.inputFocus();
   desktop.key(XK_Shift_L,true);desktop.key(XK_3,true);desktop.key(XK_3,false);desktop.key(XK_Shift_L,false);desktop.inputFocus();
   PUTGD_COMPARE_STEADY(actions.size(),3,100);
   // Recording a new shortcut pauses the raw path as well.
   Error paused{"pending",{},{}};shortcuts.setRecording(true,[&](Error e){paused=e;});QVERIFY(!paused);
   chord(XK_1);PUTGD_COMPARE_STEADY(actions.size(),3,100);
   Error restored{"pending",{},{}};shortcuts.setRecording(false,[&](Error e){restored=e;});QVERIFY(!restored);
   chord(XK_1);PUTGD_TRY_COMPARE(actions.size(),4);QCOMPARE(last()["path"].toString(),QString("raw"));
  }
  chord(XK_3);PUTGD_TRY_COMPARE(actions.size(),5);QCOMPARE(actions.last(),QString("desktop"));QCOMPARE(last()["path"].toString(),QString("passive"));
  PUTGD_COMPARE_STEADY(actions.size(),5,50);
  QCOMPARE(last()["sequence"].toInteger(),qint64(5));
 }
 void rawShortcutIsIgnoredWhileTheSessionIsLocked() {
  // A screen locker holds the keyboard too; the raw path must not capture
  // from a locked session (#341).
  X11DesktopFixture desktop;X11Shortcuts shortcuts;int activated=0;
  connect(&shortcuts,&X11Shortcuts::activated,this,[&](const QString&){++activated;});
  shortcuts.bind({{"region","Ctrl+Shift+1"}});QVERIFY(shortcuts.status()["region"].toObject()["bound"].toBool());
  ScreenSaverFixture saver(true);QVERIFY(saver.registered);
  X11GrabbingClient locker;QVERIFY(locker.grabbed);
  auto chord=[&]{desktop.key(XK_Control_L,true);desktop.key(XK_Shift_L,true);desktop.key(XK_1,true);desktop.key(XK_1,false);desktop.key(XK_Shift_L,false);desktop.key(XK_Control_L,false);desktop.inputFocus();};
  chord();
  PUTGD_TRY_COMPARE(shortcuts.keyPaths()["lastActivation"].toObject()["suppressed"].toString(),QString("session_locked"));
  QCOMPARE(activated,0);QVERIFY(saver.calls>=1);
  saver.active=false;chord();PUTGD_TRY_COMPARE(activated,1);
  QCOMPARE(shortcuts.keyPaths()["lastActivation"].toObject()["path"].toString(),QString("raw"));
  QVERIFY(!shortcuts.keyPaths()["lastActivation"].toObject().contains("suppressed"));
 }
};
QTEST_MAIN(X11Test)
#include "test_x11.moc"
