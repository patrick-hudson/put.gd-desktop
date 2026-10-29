// Isolated X11 rendering/input/lifetime contracts. This is not compositor proof.
#include "platform/x11/NativeRegionOverlay.h"
#include "ui/RegionSelection.h"
#include <QAccessible>
#include <QDir>
#include <QPainter>
#include "platform/x11/X11Reply.h"
#include "platform/x11/CaptureCursorCache.h"
#include <QtTest>
#include <QElapsedTimer>
#include <QApplication>
#include <QScreen>
#include <QWidget>
#include <QCryptographicHash>
#include <QtEndian>
#include <QScopeGuard>
#include <xcb/xtest.h>
#include <xcb/xfixes.h>
#include <xcb/xcb_keysyms.h>
#include <cstring>
#include <memory>
#include <limits>
#include "fixtures/Waits.h"
using namespace putgd;
class NativeRegionTest final:public QObject {
 Q_OBJECT
 xcb_connection_t* connection_=nullptr;
 xcb_window_t root_=0;
 xcb_key_symbols_t* keys_=nullptr;
 struct Window {xcb_window_t id;QRect rect;};
 QList<CaptureFrame> frames()const {return {{"left",QRectF(0,0,640,800),QRect(0,0,640,800),1,0,{}},{"right",QRectF(640,0,320,400),QRect(640,0,640,800),2,0,{}}};}
 QList<Window> drawables(){
  QList<Window> result;auto tree=x11::reply(xcb_query_tree_reply(connection_,xcb_query_tree(connection_,root_),nullptr));if(!tree)return result;
  const auto pidAtom=x11::internAtom(connection_,"_NET_WM_PID");auto* children=xcb_query_tree_children(tree.get());
  for(int i=0;i<xcb_query_tree_children_length(tree.get());++i){const auto w=children[i];auto attr=x11::reply(xcb_get_window_attributes_reply(connection_,xcb_get_window_attributes(connection_,w),nullptr));if(!attr||!attr->override_redirect||attr->map_state!=XCB_MAP_STATE_VIEWABLE)continue;auto p=x11::reply(xcb_get_property_reply(connection_,xcb_get_property(connection_,0,w,pidAtom,XCB_ATOM_CARDINAL,0,1),nullptr));uint32_t owner=0;if(p&&p->format==32&&xcb_get_property_value_length(p.get())==4)std::memcpy(&owner,xcb_get_property_value(p.get()),4);if(owner!=QCoreApplication::applicationPid())continue;auto g=x11::reply(xcb_get_geometry_reply(connection_,xcb_get_geometry(connection_,w),nullptr));if(g)result.append({w,QRect(g->x,g->y,g->width,g->height)});}
  return result;
 }
 xcb_window_t at(QPoint point){for(const auto& w:drawables())if(w.rect.contains(point))return w.id;return 0;}
 QByteArray pixels(QPoint point,int width=1){for(const auto& w:drawables())if(w.rect.contains(point)){auto image=x11::reply(xcb_get_image_reply(connection_,xcb_get_image(connection_,XCB_IMAGE_FORMAT_Z_PIXMAP,w.id,point.x()-w.rect.x(),point.y()-w.rect.y(),width,1,~0u),nullptr));if(image)return QByteArray(reinterpret_cast<const char*>(xcb_get_image_data(image.get())),xcb_get_image_data_length(image.get()));}return {};}
 bool white(QPoint point){const auto b=pixels(point);return b.size()==4&&quint8(b[0])==255&&quint8(b[1])==255&&quint8(b[2])==255&&quint8(b[3])==255;}
 void motion(QPoint point){xcb_test_fake_input(connection_,XCB_MOTION_NOTIFY,0,XCB_CURRENT_TIME,root_,point.x(),point.y(),0);xcb_flush(connection_);}
 void button(int number,bool down){xcb_test_fake_input(connection_,down?XCB_BUTTON_PRESS:XCB_BUTTON_RELEASE,number,XCB_CURRENT_TIME,root_,0,0,0);xcb_flush(connection_);}
 void key(uint32_t symbol,bool down){auto* codes=xcb_key_symbols_get_keycode(keys_,symbol);QVERIFY(codes&&codes[0]);const auto code=codes[0];std::free(codes);xcb_test_fake_input(connection_,down?XCB_KEY_PRESS:XCB_KEY_RELEASE,code,XCB_CURRENT_TIME,root_,0,0,0);xcb_flush(connection_);}
 void keyClick(uint32_t symbol,uint32_t modifier=0){if(modifier)key(modifier,true);key(symbol,true);key(symbol,false);if(modifier)key(modifier,false);}
 void escape(){auto* codes=xcb_key_symbols_get_keycode(keys_,0xff1b);QVERIFY(codes&&codes[0]);const auto key=codes[0];std::free(codes);xcb_test_fake_input(connection_,XCB_KEY_PRESS,key,XCB_CURRENT_TIME,root_,0,0,0);xcb_test_fake_input(connection_,XCB_KEY_RELEASE,key,XCB_CURRENT_TIME,root_,0,0,0);xcb_flush(connection_);}
 QByteArray cursor(){auto image=x11::reply(xcb_xfixes_get_cursor_image_reply(connection_,xcb_xfixes_get_cursor_image(connection_),nullptr));if(!image)return {};QByteArray data;for(const auto n:{image->width,image->height,image->xhot,image->yhot}){const uint16_t v=n;data.append(reinterpret_cast<const char*>(&v),sizeof(v));}data.append(reinterpret_cast<const char*>(xcb_xfixes_get_cursor_image_cursor_image(image.get())),xcb_xfixes_get_cursor_image_cursor_image_length(image.get())*4);return QCryptographicHash::hash(data,QCryptographicHash::Sha256);}
 void availableGrabs(){auto pointer=x11::reply(xcb_grab_pointer_reply(connection_,xcb_grab_pointer(connection_,0,root_,XCB_EVENT_MASK_BUTTON_PRESS,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC,XCB_NONE,XCB_NONE,XCB_CURRENT_TIME),nullptr));const int pointerStatus=pointer?pointer->status:-1;if(pointerStatus==XCB_GRAB_STATUS_SUCCESS)xcb_ungrab_pointer(connection_,XCB_CURRENT_TIME);auto keyboard=x11::reply(xcb_grab_keyboard_reply(connection_,xcb_grab_keyboard(connection_,0,root_,XCB_CURRENT_TIME,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC),nullptr));const int keyboardStatus=keyboard?keyboard->status:-1;if(keyboardStatus==XCB_GRAB_STATUS_SUCCESS)xcb_ungrab_keyboard(connection_,XCB_CURRENT_TIME);xcb_flush(connection_);QCOMPARE(pointerStatus,int(XCB_GRAB_STATUS_SUCCESS));QCOMPARE(keyboardStatus,int(XCB_GRAB_STATUS_SUCCESS));}
private slots:
 void resolvedOutcomeSurvivesNativeSelection(){
  for(const QString outcome:{QString("Uploads right away to Proton Drive"),QString("Saves locally"),QString("Opens the image editor")}){
   NativeRegionOverlay overlay(frames(),nullptr,XCB_CURSOR_NONE,outcome);overlay.start();PUTGD_TRY_VERIFY(!drawables().isEmpty());
   auto* accessible=QAccessible::queryAccessibleInterface(&overlay);QVERIFY(accessible);QVERIFY(accessible->text(QAccessible::Description).endsWith(outcome));
   keyClick(0xff53);PUTGD_TRY_VERIFY(accessible->text(QAccessible::Description).contains("Enter to capture"));
   QVERIFY(accessible->text(QAccessible::Description).endsWith(outcome));overlay.cancel();PUTGD_TRY_VERIFY(drawables().isEmpty());
  }
 }
 void initTestCase(){connection_=xcb_connect(nullptr,nullptr);QVERIFY(!xcb_connection_has_error(connection_));root_=xcb_setup_roots_iterator(xcb_get_setup(connection_)).data->root;auto fixes=x11::reply(xcb_xfixes_query_version_reply(connection_,xcb_xfixes_query_version(connection_,5,0),nullptr));QVERIFY(fixes&&fixes->major_version>=2);keys_=xcb_key_symbols_alloc(connection_);QVERIFY(keys_);}
 void cleanupTestCase(){xcb_key_symbols_free(keys_);xcb_disconnect(connection_);}
 void cachedCursorSurvivesSourceClientExit(){
  // Assertions below may return early, including while a cursor-image probe owns input.
  const auto releasePointer=qScopeGuard([&]{xcb_ungrab_pointer(connection_,XCB_CURRENT_TIME);auto fence=x11::reply(xcb_get_input_focus_reply(connection_,xcb_get_input_focus(connection_),nullptr));});
  CaptureCursorCache cache;QVERIFY(cache.supported());
  const QString fingerprint(64,'a');
  QCOMPARE(cache.lookup(fingerprint),xcb_cursor_t(XCB_CURSOR_NONE));
  auto* source=xcb_connect(nullptr,nullptr);const auto closeSource=qScopeGuard([&]{if(source)xcb_disconnect(source);});QVERIFY(!xcb_connection_has_error(source));
  const auto font=xcb_generate_id(source),id=xcb_generate_id(source);
  auto* error=xcb_request_check(source,xcb_open_font_checked(source,font,6,"cursor"));QVERIFY(!error);
  error=xcb_request_check(source,xcb_create_glyph_cursor_checked(source,id,font,font,34,35,65535,65535,65535,0,0,0));QVERIFY(!error);
  auto imageWithCursor=[&](xcb_cursor_t value){
   xcb_generic_error_t* protocolError=nullptr;
   auto grab=x11::reply(xcb_grab_pointer_reply(connection_,xcb_grab_pointer(connection_,false,root_,XCB_EVENT_MASK_BUTTON_PRESS,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC,XCB_NONE,value,XCB_CURRENT_TIME),&protocolError));
   std::free(protocolError);protocolError=nullptr;
   if(!grab||grab->status!=XCB_GRAB_STATUS_SUCCESS)return QByteArray{};
   auto probe=x11::reply(xcb_xfixes_get_cursor_image_reply(connection_,xcb_xfixes_get_cursor_image(connection_),&protocolError));
   std::free(protocolError);
   const auto image=cursor();xcb_ungrab_pointer(connection_,XCB_CURRENT_TIME);auto fence=x11::reply(xcb_get_input_focus_reply(connection_,xcb_get_input_focus(connection_),nullptr));return image;
  };
  const auto expected=imageWithCursor(id);QVERIFY(!expected.isEmpty());
  auto grab=x11::reply(xcb_grab_pointer_reply(connection_,xcb_grab_pointer(connection_,false,root_,XCB_EVENT_MASK_BUTTON_PRESS,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC,XCB_NONE,id,XCB_CURRENT_TIME),nullptr));QVERIFY(grab&&grab->status==XCB_GRAB_STATUS_SUCCESS);
  auto image=x11::reply(xcb_xfixes_get_cursor_image_reply(connection_,xcb_xfixes_get_cursor_image(connection_),nullptr));QVERIFY(image);
  QByteArray wire(image->width*image->height*4,Qt::Uninitialized);const auto* argb=xcb_xfixes_get_cursor_image_cursor_image(image.get());
  for(int i=0;i<image->width*image->height;++i)qToBigEndian(argb[i],wire.data()+i*4);
  xcb_ungrab_pointer(connection_,XCB_CURRENT_TIME);xcb_flush(connection_);
  QVERIFY(cache.seed(image->width,image->height,image->xhot,image->yhot,wire,fingerprint));const auto cached=cache.lookup(fingerprint);QVERIFY(cached&&cached!=id);
  xcb_disconnect(source);source=nullptr;
  // Independent clients have no disconnect/request ordering guarantee. Retry
  // until the server retires the original ID, releasing every successful probe.
  auto originalGone=[&]{
   xcb_generic_error_t* gone=nullptr;
   auto old=x11::reply(xcb_grab_pointer_reply(connection_,xcb_grab_pointer(connection_,false,root_,XCB_EVENT_MASK_BUTTON_PRESS,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC,XCB_NONE,id,XCB_CURRENT_TIME),&gone));
   const int code=gone?gone->error_code:0;std::free(gone);
   if(old&&old->status==XCB_GRAB_STATUS_SUCCESS){xcb_ungrab_pointer(connection_,XCB_CURRENT_TIME);auto fence=x11::reply(xcb_get_input_focus_reply(connection_,xcb_get_input_focus(connection_),nullptr));}
   return !old&&code==XCB_CURSOR;
  };
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(originalGone(),2000);
  QCOMPARE(imageWithCursor(cached),expected);
  QCOMPARE(cache.lookup(fingerprint),cached);
  QCOMPARE(cache.lookup(QString(64,'b')),xcb_cursor_t(XCB_CURSOR_NONE));
  QCOMPARE(cache.lookup(fingerprint),xcb_cursor_t(XCB_CURSOR_NONE));
  QVERIFY(!cache.seed(65,1,0,0,QByteArray(260,'\0'),fingerprint));
  QVERIFY(!cache.seed(1,1,1,0,QByteArray(4,'\0'),fingerprint));
  QVERIFY(!cache.seed(1,1,0,0,QByteArray(3,'\0'),fingerprint));
  QVERIFY(!cache.seed(1,1,0,0,QByteArray(4,'\0'),"invalid"));
  QCOMPARE(cache.lookup(fingerprint),xcb_cursor_t(XCB_CURSOR_NONE));availableGrabs();
 }
 void cachePreservesArgbChannelsAndAsymmetricHotspot(){
  CaptureCursorCache cache;QVERIFY(cache.supported());const QString fingerprint(64,'c');
  const quint32 values[]={0xff123456,0x80604020,0,0xffff0000,0xff00ff00,0xff0000ff};
  QByteArray wire(24,Qt::Uninitialized);for(int i=0;i<6;++i)qToBigEndian(values[i],wire.data()+i*4);
  QVERIFY(cache.seed(3,2,2,1,wire,fingerprint));const auto id=cache.lookup(fingerprint);QVERIFY(id);
  auto grab=x11::reply(xcb_grab_pointer_reply(connection_,xcb_grab_pointer(connection_,false,root_,XCB_EVENT_MASK_BUTTON_PRESS,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC,XCB_NONE,id,XCB_CURRENT_TIME),nullptr));QVERIFY(grab&&grab->status==XCB_GRAB_STATUS_SUCCESS);
  auto image=x11::reply(xcb_xfixes_get_cursor_image_reply(connection_,xcb_xfixes_get_cursor_image(connection_),nullptr));QVERIFY(image);
  QCOMPARE(image->width,uint16_t(3));QCOMPARE(image->height,uint16_t(2));QCOMPARE(image->xhot,uint16_t(2));QCOMPARE(image->yhot,uint16_t(1));
  const auto* actual=xcb_xfixes_get_cursor_image_cursor_image(image.get());for(int i=0;i<6;++i)QCOMPARE(actual[i],values[i]);
  xcb_ungrab_pointer(connection_,XCB_CURRENT_TIME);xcb_flush(connection_);availableGrabs();
 }
 void transparentDrawablesAndLogicalSelection(){
  motion({100,400});NativeRegionOverlay overlay(frames());int selected=0,cancelled=0;QRectF selection;connect(&overlay,&RegionSelector::selected,this,[&](const QRectF& r){selection=r;++selected;});connect(&overlay,&RegionSelector::cancelled,this,[&](const Error&){++cancelled;});overlay.start();PUTGD_TRY_COMPARE(drawables().size(),2);QCOMPARE(cancelled,0);
  for(const auto& frame:frames()){const auto w=at(frame.nativeRect.center());QVERIFY(w);auto g=x11::reply(xcb_get_geometry_reply(connection_,xcb_get_geometry(connection_,w),nullptr));QCOMPARE(g->depth,uint8_t(32));const auto value=pixels(frame.nativeRect.bottomLeft()+QPoint(20,-20));QCOMPARE(value.size(),4);QCOMPARE(quint8(value[3]),frame.identity=="left"?uint8_t(0):uint8_t(70));}
  motion({40,40});button(1,true);motion({1200,600});auto* accessible=QAccessible::queryAccessibleInterface(&overlay);QVERIFY(accessible);PUTGD_TRY_VERIFY(accessible->text(QAccessible::Description).contains("Release to capture"));PUTGD_TRY_VERIFY(white({44,41}));PUTGD_TRY_VERIFY(white({646,82}));button(1,false);PUTGD_TRY_COMPARE(selected,1);QCOMPARE(cancelled,0);QCOMPARE(selection,QRectF(40,40,880,260));PUTGD_TRY_COMPARE(drawables().size(),0);availableGrabs();
 }
 void tabSwitchesDisplayAndClampsSelection(){
  motion({100,400});NativeRegionOverlay overlay(frames());QSignalSpy selected(&overlay,&RegionSelector::selected);
  overlay.start();PUTGD_TRY_COMPARE(drawables().size(),2);
  auto* accessible=QAccessible::queryAccessibleInterface(&overlay);QVERIFY(accessible);
  keyClick(0xff09);PUTGD_TRY_VERIFY(accessible->text(QAccessible::Description).contains("Display 2"));
  keyClick(0xff09,0xffe1);PUTGD_TRY_VERIFY(accessible->text(QAccessible::Description).contains("Display 1"));
  keyClick(0xff53);PUTGD_TRY_VERIFY(accessible->text(QAccessible::Description).contains("320 × 400"));
  keyClick(0xff09);PUTGD_TRY_VERIFY(accessible->text(QAccessible::Description).contains("320 × 400 at 640, 0"));
  keyClick(0xff0d);PUTGD_TRY_COMPARE(selected.size(),1);
  QCOMPARE(qvariant_cast<QRectF>(selected.first().first()),QRectF(640,0,320,400));availableGrabs();
 }
 void keyboardRegionUsesPointerScreenAndSharedSteps(){
  motion({1000,200});NativeRegionOverlay overlay(frames());QSignalSpy selected(&overlay,&RegionSelector::selected);QSignalSpy cancelled(&overlay,&RegionSelector::cancelled);
  overlay.start();PUTGD_TRY_COMPARE(drawables().size(),2);
  auto* accessible=QAccessible::queryAccessibleInterface(&overlay);QVERIFY(accessible);QVERIFY(accessible->text(QAccessible::Description).contains("Drag to select"));
  QVERIFY(accessible->text(QAccessible::Description).contains("Enter to capture"));
  keyClick(0xff53); // Right: initialise on the mixed-density second monitor.
  PUTGD_TRY_VERIFY(accessible->text(QAccessible::Description).contains("160 × 200"));
  QVERIFY(accessible->text(QAccessible::Description).contains("Enter to capture"));
  // Pointer movement does not change the keyboard selection's monitor.
  motion({100,700});keyClick(0xff53);keyClick(0xff54,0xffe3);keyClick(0xff53,0xffe1);
  key(0xffe3,true);keyClick(0xff52,0xffe1);key(0xffe3,false);
  keyClick(0xff0d);PUTGD_TRY_COMPARE(selected.size(),1);QCOMPARE(cancelled.size(),0);
  QCOMPARE(qvariant_cast<QRectF>(selected.first().first()),QRectF(721,110,161,190));QCOMPARE(drawables().size(),0);availableGrabs();
 }
 void enterWithoutRegionUsesCurrentScreen_data(){QTest::addColumn<QPoint>("point");QTest::addColumn<QRectF>("expected");QTest::addColumn<uint>("acceptKey");QTest::newRow("left-return")<<QPoint(100,700)<<frames()[0].logicalRect<<uint(0xff0d);QTest::newRow("right-keypad-enter")<<QPoint(1000,700)<<frames()[1].logicalRect<<uint(0xff8d);}
 void enterWithoutRegionUsesCurrentScreen(){
  QFETCH(QPoint,point);QFETCH(QRectF,expected);QFETCH(uint,acceptKey);motion(point);NativeRegionOverlay overlay(frames());QSignalSpy selected(&overlay,&RegionSelector::selected);overlay.start();PUTGD_TRY_COMPARE(drawables().size(),2);
  keyClick(acceptKey);PUTGD_TRY_COMPARE(selected.size(),1);QCOMPARE(qvariant_cast<QRectF>(selected.first().first()),expected);availableGrabs();
 }
 void keyboardRegionPreservesNegativeLogicalOrigin(){
  auto layout=frames();layout[0].logicalRect.translate(-640,-100);motion({100,700});NativeRegionOverlay overlay(layout);QSignalSpy selected(&overlay,&RegionSelector::selected);overlay.start();PUTGD_TRY_COMPARE(drawables().size(),2);
  keyClick(0xff51);keyClick(0xff0d);PUTGD_TRY_COMPARE(selected.size(),1);QCOMPARE(qvariant_cast<QRectF>(selected.first().first()),QRectF(-480,100,320,400));availableGrabs();
 }
 void keyboardThenMouseKeepsDragGeometry(){
  motion({1000,200});NativeRegionOverlay overlay(frames());QSignalSpy selected(&overlay,&RegionSelector::selected);overlay.start();PUTGD_TRY_COMPARE(drawables().size(),2);
  keyClick(0xff53);motion({40,100});button(1,true);motion({1200,600});button(1,false);PUTGD_TRY_COMPARE(selected.size(),1);
  QCOMPARE(qvariant_cast<QRectF>(selected.first().first()),QRectF(40,100,880,200));availableGrabs();
 }
 void keyboardRegionCanCancelAndBoundsStayOnScreen(){
  motion({1000,200});NativeRegionOverlay overlay(frames());QSignalSpy cancelled(&overlay,&RegionSelector::cancelled);overlay.start();PUTGD_TRY_COMPARE(drawables().size(),2);
  auto* accessible=QAccessible::queryAccessibleInterface(&overlay);QVERIFY(accessible);keyClick(0xff53);
  for(int i=0;i<100;++i)keyClick(0xff53,0xffe3);
  PUTGD_TRY_COMPARE(accessible->rect(),QRect(800,100,160,200));escape();PUTGD_TRY_COMPARE(cancelled.size(),1);availableGrabs();
 }
 void instructionBackdropAndRenders_data(){QTest::addColumn<bool>("large");QTest::newRow("normal")<<false;QTest::newRow("text150")<<true;}
 void instructionBackdropAndRenders(){
  QFETCH(bool,large);const auto original=QGuiApplication::font();const auto restore=qScopeGuard([&]{QGuiApplication::setFont(original);});
  if(large){auto font=original;if(font.pixelSize()>0)font.setPixelSize(qRound(font.pixelSize()*1.5));else font.setPointSizeF(font.pointSizeF()*1.5);QGuiApplication::setFont(font);}
  motion({100,700});NativeRegionOverlay overlay(frames());overlay.start();PUTGD_TRY_COMPARE(drawables().size(),2);keyClick(0xff53);
  auto* accessible=QAccessible::queryAccessibleInterface(&overlay);QVERIFY(accessible);PUTGD_TRY_COMPARE(accessible->rect(),QRect(160,200,320,400));
  for(const auto& w:drawables()){
   const auto image=x11::reply(xcb_get_image_reply(connection_,xcb_get_image(connection_,XCB_IMAGE_FORMAT_Z_PIXMAP,w.id,0,0,w.rect.width(),w.rect.height(),~0u),nullptr));QVERIFY(image);
   QImage rendered(xcb_get_image_data(image.get()),w.rect.width(),w.rect.height(),QImage::Format_ARGB32_Premultiplied);
   const auto badge=rendered.copy(0,0,w.rect.width(),160);int light=0,dark=0;
   for(int y=0;y<badge.height();++y)for(int x=0;x<badge.width();++x){const auto c=badge.pixelColor(x,y);if(c.alpha()==255){if(c.lightness()>192)++light;if(c.lightness()<64)++dark;}}
   if(w.rect.x()==0){QVERIFY(light>100);QVERIFY(dark>100);}else{QCOMPARE(light,0);QCOMPARE(dark,0);}
   const auto dir=qEnvironmentVariable("PUTGD_REGION_RENDER_DIR");if(!dir.isEmpty())for(const auto& variant:{QString("dark"),QString("light")}){const auto outputVariant=large?QString("text150"):variant;QVERIFY(QDir().mkpath(dir+"/"+outputVariant));QImage composed(rendered.size(),QImage::Format_RGB32);composed.fill(variant=="dark"?QColor("#191919"):QColor("#EEEEEB"));QPainter painter(&composed);painter.drawImage(0,0,rendered);painter.end();QVERIFY(composed.save(dir+"/"+outputVariant+"/native-"+QString::number(w.rect.x())+(large?"-"+variant:QString{})+".png"));}
  }
  overlay.cancel();availableGrabs();
 }
 void outlineContrastsWithBothWhiteAndBlack(){
  NativeRegionOverlay overlay(frames());overlay.start();PUTGD_TRY_COMPARE(drawables().size(),2);
  motion({40,40});button(1,true);motion({400,300});PUTGD_TRY_VERIFY(white({100,41}));
  bool dark=false,light=false;
  for(int y=38;y<47;++y){const auto b=pixels({100,y});if(b.size()!=4)continue;
   const int alpha=quint8(b[3]);
   dark|=alpha==255&&quint8(b[0])<64&&quint8(b[1])<64&&quint8(b[2])<64;
   light|=alpha==255&&quint8(b[0])>192&&quint8(b[1])>192&&quint8(b[2])>192;
  }
  overlay.cancel();button(1,false);PUTGD_TRY_COMPARE(drawables().size(),0);availableGrabs();
  QVERIFY2(dark,"The outline needs opaque dark pixels to remain visible over white content");
  QVERIFY2(light,"The outline needs opaque light pixels to remain visible over dark content");
 }
 void movingOutlineClearsPreviousPixels(){
  NativeRegionOverlay overlay(frames());overlay.start();PUTGD_TRY_COMPARE(drawables().size(),2);motion({40,40});button(1,true);motion({1200,600});
  auto whiteCount=[&]{const auto bytes=pixels({1196,300},8);int count=0;for(int i=0;i+3<bytes.size();i+=4)if(quint8(bytes[i])==255&&quint8(bytes[i+1])==255&&quint8(bytes[i+2])==255&&quint8(bytes[i+3])==255)++count;return count;};
  PUTGD_TRY_VERIFY(whiteCount()>0);motion({1150,550});PUTGD_TRY_COMPARE(whiteCount(),0);overlay.cancel();button(1,false);PUTGD_TRY_COMPARE(drawables().size(),0);availableGrabs();
 }
 void nativeCursorMatchesQtCrosshair(){
  QWidget control(nullptr,Qt::Tool|Qt::FramelessWindowHint|Qt::WindowDoesNotAcceptFocus);control.setAttribute(Qt::WA_TranslucentBackground);control.setGeometry(0,0,1,1);control.setCursor(Qt::CrossCursor);control.show();control.grabMouse(Qt::CrossCursor);motion({20,20});putgd::test::settle(30);const auto expected=cursor();QVERIFY(!expected.isEmpty());control.releaseMouse();control.hide();
  for(bool cached:{false,true}){
   CaptureCursorCache cache;quint32 id=0;
   if(cached){
    control.show();control.grabMouse(Qt::CrossCursor);motion({20,20});putgd::test::settle(10);
    auto image=x11::reply(xcb_xfixes_get_cursor_image_reply(connection_,xcb_xfixes_get_cursor_image(connection_),nullptr));QVERIFY(image);
    QByteArray wire(image->width*image->height*4,Qt::Uninitialized);const auto* data=xcb_xfixes_get_cursor_image_cursor_image(image.get());for(int i=0;i<image->width*image->height;++i)qToBigEndian(data[i],wire.data()+i*4);
    control.releaseMouse();control.hide();QVERIFY(cache.seed(image->width,image->height,image->xhot,image->yhot,wire,QString(64,'a')));id=cache.lookup(QString(64,'a'));
   }
   NativeRegionOverlay overlay(frames(),nullptr,id);overlay.start();PUTGD_TRY_COMPARE(drawables().size(),2);motion({1000,200});PUTGD_TRY_COMPARE(cursor(),expected);overlay.cancel();PUTGD_TRY_COMPARE(drawables().size(),0);availableGrabs();
  }
 }
 void cancellationModes_data(){QTest::addColumn<int>("mode");QTest::newRow("escape")<<0;QTest::newRow("right-click")<<1;QTest::newRow("zero-area")<<2;QTest::newRow("explicit-twice")<<3;}
 void cancellationModes(){QFETCH(int,mode);for(int repeat=0;repeat<2;++repeat){NativeRegionOverlay overlay(frames());int cancelled=0,selected=0;QString code;connect(&overlay,&RegionSelector::cancelled,this,[&](const Error& e){code=e.code;++cancelled;});connect(&overlay,&RegionSelector::selected,this,[&](const QRectF&){++selected;});overlay.start();PUTGD_TRY_COMPARE(drawables().size(),2);if(mode==0)escape();else if(mode==1){button(3,true);button(3,false);}else if(mode==2){motion({100,100});button(1,true);button(1,false);}else{overlay.cancel();overlay.cancel();}PUTGD_TRY_COMPARE(cancelled,1);QCOMPARE(selected,0);QCOMPARE(code,QString("cancelled"));PUTGD_TRY_COMPARE(drawables().size(),0);availableGrabs();}}
 void destructionDuringCompletionDoesNotRetainInput(){
  for(int mode=0;mode<3;++mode){auto overlay=std::make_unique<NativeRegionOverlay>(frames());int completed=0;connect(overlay.get(),&RegionSelector::cancelled,this,[&](const Error&){++completed;overlay.reset();});connect(overlay.get(),&RegionSelector::selected,this,[&](const QRectF&){++completed;overlay.reset();});overlay->start();PUTGD_TRY_COMPARE(drawables().size(),2);if(mode==0){motion({40,40});button(1,true);motion({400,400});button(1,false);}else if(mode==1){keyClick(0xff53);keyClick(0xff0d);}else escape();PUTGD_TRY_COMPARE(completed,1);QVERIFY(!overlay);PUTGD_TRY_COMPARE(drawables().size(),0);availableGrabs();}
 }
 void cancellationDuringStartup(){
  auto overlay=std::make_unique<NativeRegionOverlay>(frames());int completed=0;
  connect(overlay.get(),&RegionSelector::cancelled,this,[&](const Error& e){QCOMPARE(e.code,QString("cancelled"));++completed;overlay.reset();});
  QTimer::singleShot(0,this,[&]{if(overlay)overlay->cancel();});
  overlay->start();PUTGD_TRY_COMPARE(completed,1);QVERIFY(!overlay);QCOMPARE(drawables().size(),0);availableGrabs();
 }
 void destructionDuringStartup(){
  auto overlay=std::make_unique<NativeRegionOverlay>(frames());QTimer::singleShot(0,this,[&]{overlay.reset();});
  overlay->start();PUTGD_TRY_VERIFY(!overlay);QCOMPARE(drawables().size(),0);availableGrabs();
 }
 void unmatchedReleaseBeforeEscape(){
  motion({100,100});button(1,true);
  auto* pressed=xcb_query_pointer_reply(connection_,xcb_query_pointer(connection_,root_),nullptr);const bool down=pressed&&(pressed->mask&XCB_BUTTON_MASK_1);std::free(pressed);QVERIFY(down);
  NativeRegionOverlay overlay(frames());int cancelled=0;connect(&overlay,&RegionSelector::cancelled,this,[&](const Error&){++cancelled;});overlay.start();PUTGD_TRY_COMPARE(drawables().size(),2);
  auto* codes=xcb_key_symbols_get_keycode(keys_,0xff1b);QVERIFY(codes&&codes[0]);const auto key=codes[0];std::free(codes);
  xcb_test_fake_input(connection_,XCB_BUTTON_RELEASE,1,XCB_CURRENT_TIME,root_,0,0,0);
  xcb_test_fake_input(connection_,XCB_KEY_PRESS,key,XCB_CURRENT_TIME,root_,0,0,0);xcb_test_fake_input(connection_,XCB_KEY_RELEASE,key,XCB_CURRENT_TIME,root_,0,0,0);xcb_flush(connection_);
  PUTGD_TRY_COMPARE(cancelled,1);QCOMPARE(drawables().size(),0);availableGrabs();
 }
 void displayChangeDuringStartup(){
  auto overlay=std::make_unique<NativeRegionOverlay>(frames());int completed=0;
  connect(overlay.get(),&RegionSelector::cancelled,this,[&](const Error& e){QCOMPARE(e.code,QString("display_changed"));++completed;overlay.reset();});
  QTimer::singleShot(0,this,[]{auto* screen=QGuiApplication::primaryScreen();QMetaObject::invokeMethod(screen,"geometryChanged",Qt::DirectConnection,Q_ARG(QRect,screen->geometry()));});
  overlay->start();PUTGD_TRY_COMPARE(completed,1);QVERIFY(!overlay);QCOMPARE(drawables().size(),0);availableGrabs();
 }
 void nativeCoordinateEligibility(){
  QVERIFY(NativeRegionOverlay::supportsLayout(frames()));
  auto layout=frames();layout[0].nativeRect=QRect(0,0,32768,800);layout.removeLast();QVERIFY(NativeRegionOverlay::supportsLayout(layout));
  layout[0].nativeRect.setWidth(32769);QVERIFY(!NativeRegionOverlay::supportsLayout(layout));
  layout[0].nativeRect=QRect(32760,0,8,800);QVERIFY(NativeRegionOverlay::supportsLayout(layout));
  layout[0].nativeRect.setWidth(9);QVERIFY(!NativeRegionOverlay::supportsLayout(layout));
  layout[0].nativeRect=QRect(0,0,640,32769);QVERIFY(!NativeRegionOverlay::supportsLayout(layout));
  layout=frames();layout[0].logicalRect.setX(std::numeric_limits<qreal>::quiet_NaN());QVERIFY(!NativeRegionOverlay::supportsLayout(layout));
  layout=frames();layout[0].density=std::numeric_limits<qreal>::infinity();QVERIFY(!NativeRegionOverlay::supportsLayout(layout));
  QVERIFY(!NativeRegionOverlay::supportsLayout({}));
 }
 void existingInputGrabIsPreserved_data(){QTest::addColumn<bool>("pointer");QTest::newRow("pointer")<<true;QTest::newRow("keyboard")<<false;}
 void existingInputGrabIsPreserved(){
  QFETCH(bool,pointer);
  if(pointer){auto r=x11::reply(xcb_grab_pointer_reply(connection_,xcb_grab_pointer(connection_,false,root_,XCB_EVENT_MASK_BUTTON_PRESS,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC,XCB_NONE,XCB_NONE,XCB_CURRENT_TIME),nullptr));QVERIFY(r&&r->status==XCB_GRAB_STATUS_SUCCESS);}
  else {auto r=x11::reply(xcb_grab_keyboard_reply(connection_,xcb_grab_keyboard(connection_,false,root_,XCB_CURRENT_TIME,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC),nullptr));QVERIFY(r&&r->status==XCB_GRAB_STATUS_SUCCESS);}
  // Selection retries a held grab for a bounded time, then says why (#341).
  QElapsedTimer waited;waited.start();
  NativeRegionOverlay overlay(frames());int cancelled=0;Error failure;connect(&overlay,&RegionSelector::cancelled,this,[&](const Error& e){failure=e;++cancelled;});overlay.start();PUTGD_TRY_COMPARE_WITH_TIMEOUT(cancelled,1,NativeRegionOverlay::grabWaitMs+3000);QCOMPARE(failure.code,QString("capture_unavailable"));QCOMPARE(drawables().size(),0);
  QVERIFY(waited.elapsed()>=NativeRegionOverlay::grabWaitMs);
  QCOMPARE(failure.details["check"].toString(),QString("input_grab"));QVERIFY(failure.details["attempts"].toInt()>1);
  QCOMPARE(failure.details[pointer?"pointerHeld":"keyboardHeld"].toBool(),true);QCOMPARE(failure.details[pointer?"keyboardHeld":"pointerHeld"].toBool(),false);
  QVERIFY(failure.message.contains("Close it, then try again"));
  auto* probe=xcb_connect(nullptr,nullptr);int status=-1;
  if(pointer){auto r=x11::reply(xcb_grab_pointer_reply(probe,xcb_grab_pointer(probe,false,root_,XCB_EVENT_MASK_BUTTON_PRESS,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC,XCB_NONE,XCB_NONE,XCB_CURRENT_TIME),nullptr));if(r)status=r->status;}
  else{auto r=x11::reply(xcb_grab_keyboard_reply(probe,xcb_grab_keyboard(probe,false,root_,XCB_CURRENT_TIME,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC),nullptr));if(r)status=r->status;}
  xcb_disconnect(probe);QCOMPARE(status,int(XCB_GRAB_STATUS_ALREADY_GRABBED));
  if(pointer)xcb_ungrab_pointer(connection_,XCB_CURRENT_TIME);else xcb_ungrab_keyboard(connection_,XCB_CURRENT_TIME);xcb_flush(connection_);availableGrabs();
 }
 void selectionStartsWhenAnotherClientsGrabEnds(){
  // An open menu holds the keyboard and pointer (#341). Selection waits with
  // no grab of its own, then starts as soon as the menu lets go.
  auto* menu=xcb_connect(nullptr,nullptr);const auto closeMenu=qScopeGuard([&]{xcb_disconnect(menu);});QVERIFY(!xcb_connection_has_error(menu));
  auto pointer=x11::reply(xcb_grab_pointer_reply(menu,xcb_grab_pointer(menu,false,root_,XCB_EVENT_MASK_BUTTON_PRESS,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC,XCB_NONE,XCB_NONE,XCB_CURRENT_TIME),nullptr));QVERIFY(pointer&&pointer->status==XCB_GRAB_STATUS_SUCCESS);
  auto keyboard=x11::reply(xcb_grab_keyboard_reply(menu,xcb_grab_keyboard(menu,false,root_,XCB_CURRENT_TIME,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC),nullptr));QVERIFY(keyboard&&keyboard->status==XCB_GRAB_STATUS_SUCCESS);
  NativeRegionOverlay overlay(frames());int cancelled=0,selected=0;QString code;
  connect(&overlay,&RegionSelector::cancelled,this,[&](const Error& e){code=e.code;++cancelled;});connect(&overlay,&RegionSelector::selected,this,[&](const QRectF&){++selected;});
  overlay.start();PUTGD_TRY_COMPARE(drawables().size(),2);PUTGD_COMPARE_STEADY(cancelled,0,300);
  xcb_ungrab_pointer(menu,XCB_CURRENT_TIME);xcb_ungrab_keyboard(menu,XCB_CURRENT_TIME);x11::reply(xcb_get_input_focus_reply(menu,xcb_get_input_focus(menu),nullptr));
  auto owned=[&]{
   auto probe=x11::reply(xcb_grab_keyboard_reply(menu,xcb_grab_keyboard(menu,false,root_,XCB_CURRENT_TIME,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC),nullptr));
   if(probe&&probe->status==XCB_GRAB_STATUS_SUCCESS){xcb_ungrab_keyboard(menu,XCB_CURRENT_TIME);x11::reply(xcb_get_input_focus_reply(menu,xcb_get_input_focus(menu),nullptr));return false;}
   return probe&&probe->status==XCB_GRAB_STATUS_ALREADY_GRABBED;
  };
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(owned(),2000);
  motion({40,40});button(1,true);motion({400,300});button(1,false);
  PUTGD_TRY_COMPARE(selected,1);QCOMPARE(cancelled,0);PUTGD_TRY_COMPARE(drawables().size(),0);availableGrabs();
 }
 void invalidResidentCursorReleasesBothGrabs(){NativeRegionOverlay overlay(frames(),nullptr,xcb_generate_id(connection_));int cancelled=0;connect(&overlay,&RegionSelector::cancelled,this,[&](const Error& e){QCOMPARE(e.code,QString("capture_unavailable"));++cancelled;});overlay.start();PUTGD_TRY_COMPARE(cancelled,1);PUTGD_TRY_COMPARE(drawables().size(),0);availableGrabs();}
 void invalidLayoutFailsWithoutTakingGrabs(){NativeRegionOverlay overlay({});int cancelled=0;connect(&overlay,&RegionSelector::cancelled,this,[&](const Error& e){QVERIFY(bool(e));++cancelled;});overlay.start();PUTGD_TRY_COMPARE(cancelled,1);QCOMPARE(drawables().size(),0);availableGrabs();}
};
QTEST_MAIN(NativeRegionTest)
#include "test_native_region_overlay.moc"
