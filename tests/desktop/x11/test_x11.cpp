#include "platform/x11/X11Clipboard.h"
#include "platform/x11/X11Shortcuts.h"
#include "platform/x11/X11Capture.h"
#include "platform/x11/X11SourceAdapter.h"
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
#include "ui/RegionOverlay.h"
#include "../../fixtures/X11SelectionFixture.h"
#include "../../fixtures/X11DesktopFixture.h"
#include <X11/keysym.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xcb.h>
#include <xcb/xtest.h>
using namespace putgd;
class X11Test:public QObject {
 Q_OBJECT
private slots:
 void nativeSelectionTextAndRevision() {
  X11Clipboard importer;QVERIFY(importer.available());
  auto before=importer.revision();QGuiApplication::clipboard()->setText("synthetic native clipboard");QTRY_VERIFY(importer.revision()>before);
  bool done=false;PayloadResult result;importer.acquire({},[&](PayloadResult r){result=r;done=true;});QTRY_VERIFY(done);QVERIFY2(!result.error,qPrintable(result.error.message));QCOMPARE(result.payload.bytes,QByteArray("synthetic native clipboard"));
 }
 void nativeFilesImageAndUrlPrecedence() {
  X11Clipboard importer;QTemporaryDir dir;QStringList expected{dir.filePath("first.txt"),dir.filePath("second.txt")};
  for(const auto& path:expected){QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write("synthetic");}
  auto* files=new QMimeData;QList<QUrl> urls;for(const auto& path:expected)urls.append(QUrl::fromLocalFile(path));files->setUrls(urls);files->setText("lower precedence");QGuiApplication::clipboard()->setMimeData(files);QTest::qWait(20);
  bool done=false;PayloadResult result;importer.acquire({},[&](PayloadResult r){result=r;done=true;});QTRY_VERIFY(done);QVERIFY(!result.error);QCOMPARE(result.payload.files,expected);
  QImage expectedImage(24,16,QImage::Format_ARGB32);expectedImage.fill(Qt::cyan);QGuiApplication::clipboard()->setImage(expectedImage);QTest::qWait(20);done=false;importer.acquire({},[&](PayloadResult r){result=r;done=true;});QTRY_VERIFY(done);QVERIFY(!result.error);QCOMPARE(result.payload.image.size(),expectedImage.size());QCOMPARE(result.payload.image.pixelColor(2,2),QColor(Qt::cyan));
  auto* url=new QMimeData;url->setUrls({QUrl("https://example.invalid/synthetic")});url->setText("https://example.invalid/synthetic");QGuiApplication::clipboard()->setMimeData(url);QTest::qWait(20);done=false;importer.acquire({},[&](PayloadResult r){result=r;done=true;});QTRY_VERIFY(done);QVERIFY2(!result.error,qPrintable(result.error.message));QCOMPARE(result.payload.kind,QString("text"));QCOMPARE(result.payload.bytes,QByteArray("https://example.invalid/synthetic"));
 }
 void remoteUriFallsThroughToText() {
  X11SelectionFixture owner({{"text/uri-list","https://example.invalid/synthetic\r\n"},{"UTF8_STRING","plain caption https://example.invalid/synthetic"}});X11Clipboard importer;bool done=false;PayloadResult result;importer.acquire({},[&](PayloadResult r){result=r;done=true;});QTRY_VERIFY(done);QVERIFY2(!result.error,qPrintable(result.error.message));QCOMPARE(result.payload.kind,QString("text"));QCOMPARE(result.payload.bytes,QByteArray("plain caption https://example.invalid/synthetic"));
 }
 void bmpImageAndIncrementalSelection() {
  QImage image(19,17,QImage::Format_RGB32);image.fill(Qt::magenta);QByteArray encoded;QBuffer buffer(&encoded);QVERIFY(buffer.open(QIODevice::WriteOnly));QVERIFY(image.save(&buffer,"BMP"));
  X11SelectionFixture owner({{"text/uri-list","https://example.invalid/not-local\r\n"},{"image/bmp",encoded}},true);X11Clipboard importer;bool done=false;PayloadResult result;importer.acquire({},[&](PayloadResult r){result=r;done=true;});QTRY_VERIFY(done);QVERIFY2(!result.error,qPrintable(result.error.message));QCOMPARE(result.payload.image.size(),image.size());QCOMPARE(result.payload.image.pixelColor(3,3),QColor(Qt::magenta));QVERIFY(owner.incrementalTransfers>0);
 }
 void incrementalText() {
  const QByteArray expected(20000,'x');X11SelectionFixture owner({{"UTF8_STRING",expected}},true);X11Clipboard importer;bool done=false;PayloadResult result;importer.acquire({},[&](PayloadResult r){result=r;done=true;});QTRY_VERIFY(done);QVERIFY2(!result.error,qPrintable(result.error.message));QCOMPARE(result.payload.bytes,expected);QVERIFY(owner.incrementalTransfers>0);
 }
 void invocationRevisionIncludesQueuedOwnerChange() {
  X11Clipboard importer;X11SelectionFixture owner({{"UTF8_STRING","invocation-source"}});const auto accepted=importer.revision();bool done=false;PayloadResult result;importer.acquire({},[&](PayloadResult r){result=r;done=true;},accepted);QTRY_VERIFY(done);QVERIFY2(!result.error,qPrintable(result.error.message));QCOMPARE(result.payload.bytes,QByteArray("invocation-source"));
 }
 void nativeKeyActivationWithLocks() {
  X11Shortcuts shortcuts;shortcuts.bind({{"region","Ctrl+Shift+1"}});int activated=0;connect(&shortcuts,&X11Shortcuts::activated,this,[&](const QString& a){if(a=="region")++activated;});
  auto* c=xcb_connect(nullptr,nullptr);auto* symbols=xcb_key_symbols_alloc(c);auto code=[&](xcb_keysym_t key){auto* codes=xcb_key_symbols_get_keycode(symbols,key);auto result=codes?codes[0]:xcb_keycode_t(0);free(codes);return result;};
  auto send=[&](xcb_keysym_t key,bool press){auto k=code(key);QVERIFY(k);xcb_test_fake_input(c,press?XCB_KEY_PRESS:XCB_KEY_RELEASE,k,XCB_CURRENT_TIME,0,0,0,0);xcb_flush(c);};
  auto chord=[&]{send(XK_Control_L,true);send(XK_Shift_L,true);send(XK_1,true);send(XK_1,false);send(XK_Shift_L,false);send(XK_Control_L,false);};
  chord();QTRY_COMPARE(activated,1);send(XK_Caps_Lock,true);send(XK_Caps_Lock,false);send(XK_Num_Lock,true);send(XK_Num_Lock,false);chord();QTRY_COMPARE(activated,2);send(XK_Caps_Lock,true);send(XK_Caps_Lock,false);send(XK_Num_Lock,true);send(XK_Num_Lock,false);xcb_key_symbols_free(symbols);xcb_disconnect(c);
 }
 void regionCancellationAndExactSelection() {
  CaptureFrame frame{"fixture",QRectF(0,0,100,100),QRect(0,0,100,100),1,0,QImage(100,100,QImage::Format_ARGB32)};frame.pixels.fill(Qt::red);
  auto surface=[]()->QWidget*{for(auto* w:QApplication::topLevelWidgets())if(w->objectName()=="putgd-region-overlay"&&w->isVisible())return w;return nullptr;};
  {RegionOverlay overlay({frame});bool cancelled=false;connect(&overlay,&RegionOverlay::cancelled,this,[&](const Error& e){cancelled=e.code=="cancelled";});overlay.start();QVERIFY(surface());QTest::keyClick(surface(),Qt::Key_Escape);QVERIFY(cancelled);}
  {RegionOverlay overlay({frame});bool cancelled=false;connect(&overlay,&RegionOverlay::cancelled,this,[&](const Error& e){cancelled=e.code=="cancelled";});overlay.start();QVERIFY(surface());QTest::mouseClick(surface(),Qt::RightButton);QVERIFY(cancelled);}
  {RegionOverlay overlay({frame});QRectF selected;connect(&overlay,&RegionOverlay::selected,this,[&](QRectF r){selected=r;});overlay.start();QVERIFY(surface());auto* w=surface();QTest::mousePress(w,Qt::LeftButton,Qt::NoModifier,QPoint(10,20));QTest::mouseRelease(w,Qt::LeftButton,Qt::NoModifier,QPoint(60,70));QCOMPARE(selected,QRectF(10,20,50,50));Error error;auto image=composeFrames({frame},selected,100000,error);QVERIFY(!error);QCOMPARE(image.size(),QSize(50,50));QCOMPARE(image.pixelColor(2,2),QColor(Qt::red));}
 }
 void encodedImageResultIsServedToNativeClipboardConsumer() {
  QImage expected(17,11,QImage::Format_ARGB32);expected.fill(Qt::yellow);
  CopyPayload payload;payload.kind="image";QBuffer buffer(&payload.encodedImage);QVERIFY(buffer.open(QIODevice::WriteOnly));QVERIFY(expected.save(&buffer,"PNG"));buffer.close();
  X11Clipboard owner;bool copied=false;owner.copy(payload,[&](Error error){QVERIFY(!error);copied=true;});QVERIFY(copied);
  PayloadResult result;bool imported=false;X11Clipboard consumer;
  consumer.acquire({},[&](PayloadResult value){result=std::move(value);imported=true;});
  QTRY_VERIFY(imported);QVERIFY2(!result.error,qPrintable(result.error.message));QCOMPARE(result.payload.kind,QString("image"));QCOMPARE(result.payload.image,expected);
 }
 void simultaneousClipboardImportsAreSerialized() {
  X11SelectionFixture owner({{"UTF8_STRING","same detached synthetic source"}},true);X11Clipboard importer;
  int completed=0;PayloadResult first,second;importer.acquire({},[&](PayloadResult r){first=r;++completed;});importer.acquire({},[&](PayloadResult r){second=r;++completed;});QTRY_COMPARE(completed,2);QVERIFY2(!first.error,qPrintable(first.error.message));QVERIFY2(!second.error,qPrintable(second.error.message));QCOMPARE(first.payload.bytes,second.payload.bytes);QCOMPARE(first.payload.bytes,QByteArray("same detached synthetic source"));
 }
 void cancellingRegionDoesNotCancelClipboardImport() {
  bool imported=false,cancelled=false;PayloadResult clipboard;X11SelectionFixture owner({{"UTF8_STRING","independent clipboard source"}});X11SourceAdapter source;
  source.acquire("clipboard",{},[&](PayloadResult r){clipboard=r;imported=true;});source.acquire("region",{},[&](PayloadResult r){cancelled=r.error.code=="cancelled";});source.cancelAcquisition();QVERIFY(cancelled);QTRY_VERIFY(imported);QVERIFY2(!clipboard.error,qPrintable(clipboard.error.message));QCOMPARE(clipboard.payload.bytes,QByteArray("independent clipboard source"));
 }
 void regionRetainsExternalFocusAndNativeEscape() {
  X11DesktopFixture desktop;QVERIFY(desktop.window);QCOMPARE(desktop.inputFocus(),desktop.window);
  bool cancelled=false;X11SourceAdapter source;source.acquire("region",{},[&](PayloadResult r){cancelled=r.error.code=="cancelled";});QTest::qWait(20);QCOMPARE(desktop.inputFocus(),desktop.window);
  desktop.key(XK_Escape,true);desktop.key(XK_Escape,false);QTRY_VERIFY(cancelled);QCOMPARE(desktop.inputFocus(),desktop.window);
 }
 void heldGlobalChordHandsKeyboardToSelector() {
  X11DesktopFixture desktop;bool invoked=false,cancelled=false;X11SourceAdapter source;source.bindShortcuts({{"region","Ctrl+Shift+1"}});
  connect(&source,&X11SourceAdapter::actionRequested,this,[&](const QString& action){if(action=="region"){invoked=true;source.acquire(action,{},[&](PayloadResult r){cancelled=r.error.code=="cancelled";});}});
  desktop.key(XK_Control_L,true);desktop.key(XK_Shift_L,true);desktop.key(XK_1,true);QTRY_VERIFY(invoked);
  // The trigger keys are still held. Escape must reach the selector rather than
  // remain trapped in the shortcut connection's active passive-key grab.
  desktop.key(XK_Escape,true);desktop.key(XK_Escape,false);QTRY_VERIFY(cancelled);QCOMPARE(desktop.inputFocus(),desktop.window);
 }
 void clipboardQueueBoundAndFrozenOwner() {
  int completed=0;PayloadResult a,b,excess;X11Clipboard importer;X11SelectionFixture first({{"UTF8_STRING","first owner"}},true);ResourceLimits limits;limits.pendingOperations=1;limits.concurrentUploads=1;
  importer.acquire(limits,[&](PayloadResult r){a=r;++completed;});importer.acquire(limits,[&](PayloadResult r){b=r;++completed;});importer.acquire(limits,[&](PayloadResult r){excess=r;});QCOMPARE(excess.error.code,QString("queue_full"));
  X11SelectionFixture replacement({{"UTF8_STRING","new owner"}});QTRY_COMPARE(completed,2);QCOMPARE(a.error.code,QString("clipboard_changed"));QCOMPARE(b.error.code,QString("clipboard_changed"));
 }
 void asciiLetterShortcutNeedsNoImplicitShift() {
  X11DesktopFixture desktop;X11Shortcuts shortcuts;int activated=0;connect(&shortcuts,&X11Shortcuts::activated,this,[&](const QString&){++activated;});shortcuts.bind({{"region","Ctrl+A"}});QVERIFY(shortcuts.status()["region"].toObject()["bound"].toBool());
  desktop.key(XK_Control_L,true);desktop.key(XK_a,true);desktop.key(XK_a,false);desktop.key(XK_Control_L,false);QTRY_COMPARE(activated,1);
 }
 void shiftedPrintableShortcutUsesActualChord() {
  X11DesktopFixture desktop;X11Shortcuts shortcuts;int activated=0;connect(&shortcuts,&X11Shortcuts::activated,this,[&](const QString&){++activated;});shortcuts.bind({{"region","Ctrl+!"}});QVERIFY(shortcuts.status()["region"].toObject()["bound"].toBool());
  desktop.key(XK_Control_L,true);desktop.key(XK_1,true);desktop.key(XK_1,false);desktop.key(XK_Control_L,false);QTest::qWait(20);QCOMPARE(activated,0);
  desktop.key(XK_Control_L,true);desktop.key(XK_Shift_L,true);desktop.key(XK_1,true);desktop.key(XK_1,false);desktop.key(XK_Shift_L,false);desktop.key(XK_Control_L,false);QTRY_COMPARE(activated,1);
 }
 void keyboardMappingChangeRebindsActualCodes() {
  X11DesktopFixture desktop;X11Shortcuts shortcuts;int activated=0,changes=0;connect(&shortcuts,&X11Shortcuts::activated,this,[&](const QString&){++activated;});connect(&shortcuts,&X11Shortcuts::bindingsChanged,this,[&]{++changes;});shortcuts.bind({{"region","Ctrl+F6"}});
  const auto code=xcb_get_setup(desktop.connection)->max_keycode;auto* old=xcb_get_keyboard_mapping_reply(desktop.connection,xcb_get_keyboard_mapping(desktop.connection,code,1),nullptr);QVERIFY(old);QList<xcb_keysym_t> previous;auto* values=xcb_get_keyboard_mapping_keysyms(old);for(int i=0;i<old->keysyms_per_keycode;++i)previous.append(values[i]);free(old);
  struct Restore {xcb_connection_t* connection;xcb_keycode_t code;QList<xcb_keysym_t> values;~Restore(){xcb_change_keyboard_mapping(connection,1,code,values.size(),values.constData());xcb_flush(connection);}} restore{desktop.connection,code,previous};
  QList<xcb_keysym_t> replacement(previous.size(),XCB_NO_SYMBOL);replacement[0]=XK_F6;if(replacement.size()>1)replacement[1]=XK_F6;const auto before=changes;xcb_change_keyboard_mapping(desktop.connection,1,code,replacement.size(),replacement.constData());xcb_flush(desktop.connection);QTRY_VERIFY(changes>before);QVERIFY(shortcuts.status()["region"].toObject()["bound"].toBool());
  desktop.key(XK_Control_L,true);desktop.rawKey(code,true);desktop.rawKey(code,false);desktop.key(XK_Control_L,false);QTRY_COMPARE(activated,1);
 }
 void nonresponsiveOwnerKeepsEventLoopAlive() {
  xcb_connection_t* c=xcb_connect(nullptr,nullptr);auto root=xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
  auto w=xcb_generate_id(c);xcb_create_window(c,XCB_COPY_FROM_PARENT,w,root,0,0,1,1,0,XCB_WINDOW_CLASS_INPUT_ONLY,XCB_COPY_FROM_PARENT,0,nullptr);
  auto a=xcb_intern_atom_reply(c,xcb_intern_atom(c,0,9,"CLIPBOARD"),nullptr);QVERIFY(a);xcb_set_selection_owner(c,w,a->atom,XCB_CURRENT_TIME);free(a);xcb_flush(c);
  X11Clipboard importer;bool done=false;PayloadResult result;int heartbeats=0;QTimer heartbeat;connect(&heartbeat,&QTimer::timeout,this,[&]{++heartbeats;});heartbeat.start(10);
  importer.acquire({},[&](PayloadResult r){result=r;done=true;});QTRY_VERIFY_WITH_TIMEOUT(done,6500);QVERIFY(heartbeats>50);QCOMPARE(result.error.code,QString("clipboard_timeout"));xcb_disconnect(c);
 }
 void conflictsAreReported() {
  X11Shortcuts first,second;QVERIFY(first.available());QVERIFY(second.available());
  first.bind({{"region","Ctrl+Shift+1"}});second.bind({{"region","Ctrl+Shift+1"}});
  QVERIFY(first.status()["region"].toObject()["bound"].toBool());QVERIFY(!second.status()["region"].toObject()["bound"].toBool());QCOMPARE(second.status()["region"].toObject()["error"].toString(),QString("shortcut_conflict"));
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
  X11Capture capture;QVERIFY(capture.available());Error error;auto target=capture.freeze("desktop",error);QVERIFY2(!error,qPrintable(error.message));auto frames=capture.frames(target,error);QVERIFY2(!error,qPrintable(error.message));QVERIFY(!frames.isEmpty());QVERIFY(!frames.first().pixels.isNull());
 }
};
QTEST_MAIN(X11Test)
#include "test_x11.moc"
