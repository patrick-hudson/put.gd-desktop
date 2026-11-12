#include "platform/x11/CaptureProcessController.h"
#include "platform/x11/CaptureCursorCache.h"
#include "platform/x11/X11NativeSnapshot.h"
#include <QTest>
#include <QElapsedTimer>
#include <QVersionNumber>
#include <QJsonArray>
#include <QColor>
#include <memory>
#include <QImageReader>
#include "platform/x11/CaptureArtifactLease.h"
#include "core/BuildInfo.h"
#include <QJsonDocument>
#include <QDir>
#include <QUuid>
#include <sys/stat.h>
#include <thread>
#include <QScopeGuard>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xtest.h>
#include "platform/x11/X11CoreSourceAdapter.h"
#include <QTemporaryDir>
#include "fixtures/Waits.h"
namespace {
bool cacheExpected(){return QVersionNumber::fromString(QString::fromLatin1(qVersion()))<QVersionNumber(6,5,0);}
QJsonObject regionSnapshot(char fingerprint='a');
bool createCursorTrace(const QString& path){
 QFile file(path);if(!file.open(QIODevice::WriteOnly)||!file.setPermissions(QFile::ReadOwner|QFile::WriteOwner))return false;
 file.close();qputenv("PUTGD_TEST_CAPTURE_TRACE",QFile::encodeName(path));return true;
}
QList<QJsonObject> cursorTrace(const QString& path){
 QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};const auto bytes=file.readAll();
 if(!bytes.endsWith('\n'))return {};
 QList<QJsonObject> records;
 for(const auto& line:bytes.split('\n')){if(line.isEmpty())continue;QJsonParseError error;const auto document=QJsonDocument::fromJson(line,&error);if(error.error!=QJsonParseError::NoError||!document.isObject())return {};records.append(document.object());}
 return records;
}
}

QJsonObject snapshot(){return {{"schemaVersion",1},{"action","desktop"},{"window","0"},{"windowRect",QJsonArray{}},{"area",QJsonArray{}},{"selection",QJsonArray{}},{"selectionLayout",QJsonArray{}},{"layout",QJsonObject{{"outputs",QJsonArray{QJsonObject{{"identity","monitor"},{"native",QJsonArray{0,0,640,480}},{"rotation",1}}}},{"configuration","1"},{"root",QJsonArray{0,0,640,480}},{"scale",QString(64,'a')}}}};}
namespace {
QJsonObject regionSnapshot(char fingerprint){auto result=snapshot();result["action"]="region";auto layout=result["layout"].toObject();layout["scale"]=QString(64,QChar(fingerprint));result["layout"]=layout;return result;}
}
namespace {
int pointerGrabStatus(xcb_connection_t* connection){
 const auto root=xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root;
 auto* reply=xcb_grab_pointer_reply(connection,xcb_grab_pointer(connection,0,root,0,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC,XCB_WINDOW_NONE,XCB_CURSOR_NONE,XCB_CURRENT_TIME),nullptr);
 const int status=reply?reply->status:-1;std::free(reply);
 // The next grab comes from another connection, and the server may read that one
 // first: a flush alone lets it find this probe's grab still in place (#398).
 if(status==XCB_GRAB_STATUS_SUCCESS){xcb_ungrab_pointer(connection,XCB_CURRENT_TIME);std::free(xcb_get_input_focus_reply(connection,xcb_get_input_focus(connection),nullptr));}
 return status;
}
void seedCursor(putgd::CaptureProcessController& controller){
 qputenv("PUTGD_TEST_CAPTURE_MODE",cacheExpected()?"cursor-seed":"observe-cursor");int completed=0;
 controller.start(regionSnapshot(),{},[&](putgd::PayloadResult value){QVERIFY2(!value.error,qPrintable(value.error.code));++completed;});
 PUTGD_TRY_COMPARE(completed,1);
}
}
class CaptureProcessTests final:public QObject {
 Q_OBJECT
private slots:
 void pointerProbeReleasePrecedesTheNextHold(){
  auto* connection=xcb_connect(nullptr,nullptr);QVERIFY(connection&&!xcb_connection_has_error(connection));const auto close=qScopeGuard([&]{xcb_disconnect(connection);});
  putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);seedCursor(controller);
  // #398's late reports ran a pre-#1202 tree. Exercise its cross-connection
  // boundary repeatedly, without retries: each successful probe must release
  // its grab before the resident's very next request reaches the X server.
  for(int i=0;i<32;++i){
   QCOMPARE(pointerGrabStatus(connection),int(XCB_GRAB_STATUS_SUCCESS));
   QVERIFY(controller.holdCrosshair());
   QCOMPARE(pointerGrabStatus(connection),int(XCB_GRAB_STATUS_ALREADY_GRABBED));
   controller.releaseUnclaimedCrosshair();
  }
  QCOMPARE(pointerGrabStatus(connection),int(XCB_GRAB_STATUS_SUCCESS));
 }
 void crosshairHoldLastsUntilTheHelperAsks(){
  auto* connection=xcb_connect(nullptr,nullptr);QVERIFY(connection&&!xcb_connection_has_error(connection));const auto close=qScopeGuard([&]{xcb_disconnect(connection);});
  putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);
  // The first press has no crosshair to show yet.
  QVERIFY(!controller.holdCrosshair());
  seedCursor(controller);QVERIFY(!controller.busy());
  // A press that starts no selection gives the pointer straight back.
  QVERIFY(controller.holdCrosshair());QCOMPARE(pointerGrabStatus(connection),int(XCB_GRAB_STATUS_ALREADY_GRABBED));
  controller.releaseUnclaimedCrosshair();QCOMPARE(pointerGrabStatus(connection),int(XCB_GRAB_STATUS_SUCCESS));
  QTemporaryDir directory("/tmp/putgd-hold-XXXXXX");QVERIFY(directory.isValid());const auto trace=directory.filePath("trace.jsonl");QVERIFY(createCursorTrace(trace));
  qputenv("PUTGD_TEST_CAPTURE_MODE","preview-handoff");
  QVERIFY(controller.holdCrosshair());
  auto ticket=controller.prepare(regionSnapshot(),{});
  PUTGD_TRY_COMPARE(cursorTrace(trace).size(),3);
  const auto frames=cursorTrace(trace);QCOMPARE(frames[1]["phase"].toString(),QString("preview"));QVERIFY(frames[1]["hold"].toString().toULongLong()>0);QCOMPARE(frames[2]["phase"].toString(),QString("released"));
  // The controller has let go before it answered the helper.
  QCOMPARE(pointerGrabStatus(connection),int(XCB_GRAB_STATUS_SUCCESS));
  int completed=0;putgd::Error error;controller.begin(ticket,[&](putgd::PayloadResult value){error=value.error;++completed;});PUTGD_TRY_COMPARE(completed,1);QVERIFY2(!error,qPrintable(error.code));
  // Failure and cancellation end a hold as well.
  qputenv("PUTGD_TEST_CAPTURE_MODE","preview-cancel");QVERIFY(controller.holdCrosshair());int early=0;
  auto cancelled=controller.prepare(regionSnapshot(),{},[&](putgd::Error){++early;});
  PUTGD_TRY_COMPARE(early,1);QCOMPARE(pointerGrabStatus(connection),int(XCB_GRAB_STATUS_SUCCESS));
  PUTGD_TRY_VERIFY(!controller.busy());
  qputenv("PUTGD_TEST_CAPTURE_MODE","preview-waiting");QVERIFY(controller.holdCrosshair());
  auto dropped=controller.prepare(regionSnapshot(),{});PUTGD_TRY_VERIFY(controller.preparedReady());
  QCOMPARE(pointerGrabStatus(connection),int(XCB_GRAB_STATUS_ALREADY_GRABBED));
  controller.cancel();QCOMPARE(pointerGrabStatus(connection),int(XCB_GRAB_STATUS_SUCCESS));PUTGD_TRY_VERIFY(!controller.busy());
 }
 void crosshairHoldRefusedByAnotherClient(){
  // An open menu or a drag holds the pointer. The press still captures, on the
  // helper's own crosshair, and the resident has nothing left to let go of.
  auto* connection=xcb_connect(nullptr,nullptr);QVERIFY(connection&&!xcb_connection_has_error(connection));const auto close=qScopeGuard([&]{xcb_disconnect(connection);});
  auto* menu=xcb_connect(nullptr,nullptr);QVERIFY(menu&&!xcb_connection_has_error(menu));const auto closeMenu=qScopeGuard([&]{xcb_disconnect(menu);});
  putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);seedCursor(controller);
  QTemporaryDir directory("/tmp/putgd-hold-XXXXXX");QVERIFY(directory.isValid());const auto trace=directory.filePath("trace.jsonl");QVERIFY(createCursorTrace(trace));
  // Free, the hold succeeds: what refuses it below is the other client.
  QVERIFY(controller.holdCrosshair());controller.releaseUnclaimedCrosshair();
  const auto root=xcb_setup_roots_iterator(xcb_get_setup(menu)).data->root;
  auto* grab=xcb_grab_pointer_reply(menu,xcb_grab_pointer(menu,0,root,0,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC,XCB_WINDOW_NONE,XCB_CURSOR_NONE,XCB_CURRENT_TIME),nullptr);
  const int menuStatus=grab?grab->status:-1;std::free(grab);QCOMPARE(menuStatus,int(XCB_GRAB_STATUS_SUCCESS));
  qputenv("PUTGD_TEST_CAPTURE_MODE","preview-handoff");
  QVERIFY(!controller.holdCrosshair());QVERIFY(!controller.crosshairHeld());
  auto ticket=controller.prepare(regionSnapshot(),{});
  PUTGD_TRY_COMPARE(cursorTrace(trace).size(),2);QCOMPARE(cursorTrace(trace)[1]["hold"].toString(),QString("0"));QVERIFY(!controller.crosshairHeld());
  int completed=0;putgd::Error error;controller.begin(ticket,[&](putgd::PayloadResult value){error=value.error;++completed;});PUTGD_TRY_COMPARE(completed,1);QVERIFY2(!error,qPrintable(error.code));
  xcb_ungrab_pointer(menu,XCB_CURRENT_TIME);std::free(xcb_get_input_focus_reply(menu,xcb_get_input_focus(menu),nullptr));
  QCOMPARE(pointerGrabStatus(connection),int(XCB_GRAB_STATUS_SUCCESS));
 }
 void escapeDuringTheHold(){
  // The real press path: a key through the server, the resident's shortcut, the
  // hold, then a helper that is still loading when Escape arrives. Escape always
  // gives the desktop its pointer back.
  auto* connection=xcb_connect(nullptr,nullptr);QVERIFY(connection&&!xcb_connection_has_error(connection));const auto close=qScopeGuard([&]{xcb_disconnect(connection);});
  const auto root=xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root;
  auto* symbols=xcb_key_symbols_alloc(connection);QVERIFY(symbols);const auto freeSymbols=qScopeGuard([&]{xcb_key_symbols_free(symbols);});
  const auto code=[&](uint32_t sym){auto* codes=xcb_key_symbols_get_keycode(symbols,sym);const uint8_t found=codes?codes[0]:0;std::free(codes);return found;};
  const auto fake=[&](uint8_t type,uint8_t detail){xcb_test_fake_input(connection,type,detail,XCB_CURRENT_TIME,root,0,0,0);auto* fence=xcb_get_input_focus_reply(connection,xcb_get_input_focus(connection),nullptr);std::free(fence);};
  const auto chord=[&]{const uint8_t ctrl=code(0xffe3),shift=code(0xffe1),one=code('1');fake(XCB_KEY_PRESS,ctrl);fake(XCB_KEY_PRESS,shift);fake(XCB_KEY_PRESS,one);fake(XCB_KEY_RELEASE,one);fake(XCB_KEY_RELEASE,shift);fake(XCB_KEY_RELEASE,ctrl);};
  putgd::X11CoreSourceAdapter adapter(nullptr,PUTGD_CAPTURE_FIXTURE);adapter.bindShortcuts({{"region","Ctrl+Shift+1"}});
  QVERIFY(adapter.capabilities()["shortcuts"].toObject()["available"].toBool());
  QList<putgd::Error> terminals;int released=0,presses=0;
  QList<putgd::SourceAdapter::PreparedAcquisition> prepared;
  const putgd::ResourceLimits limits;
  connect(&adapter,&putgd::X11CoreSourceAdapter::actionRequested,this,[&](const QString& action){++presses;prepared.append(adapter.prepareAcquisition(action,limits,[&](putgd::Error error){terminals.append(error);},[&]{++released;}));});
  qputenv("PUTGD_TEST_CAPTURE_MODE",cacheExpected()?"cursor-seed":"observe-cursor");
  chord();PUTGD_TRY_COMPARE(presses,1);
  int completed=0;prepared.takeFirst()(limits,[&](putgd::PayloadResult){++completed;});PUTGD_TRY_COMPARE(completed,1);PUTGD_TRY_COMPARE(released,1);
  QTemporaryDir directory("/tmp/putgd-hold-XXXXXX");QVERIFY(directory.isValid());const auto trace=directory.filePath("trace.jsonl");QVERIFY(createCursorTrace(trace));
  qputenv("PUTGD_TEST_CAPTURE_MODE","preview-late");released=0;
  chord();PUTGD_TRY_COMPARE(presses,2);
  QCOMPARE(pointerGrabStatus(connection),int(XCB_GRAB_STATUS_ALREADY_GRABBED));
  const uint8_t escape=code(0xff1b);fake(XCB_KEY_PRESS,escape);fake(XCB_KEY_RELEASE,escape);
  // The pointer and cursor come back at once, ahead of the helper it no longer needs.
  PUTGD_TRY_COMPARE(pointerGrabStatus(connection),int(XCB_GRAB_STATUS_SUCCESS));
  PUTGD_TRY_COMPARE(terminals.size(),1);QCOMPARE(terminals[0].code,QString("cancelled"));
  PUTGD_TRY_COMPARE(released,1);
  PUTGD_TRY_COMPARE(pointerGrabStatus(connection),int(XCB_GRAB_STATUS_SUCCESS));
 }
 void screenshotFirstAndOtherSourcesHoldNothing(){
  auto* connection=xcb_connect(nullptr,nullptr);QVERIFY(connection&&!xcb_connection_has_error(connection));const auto close=qScopeGuard([&]{xcb_disconnect(connection);});
  putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);seedCursor(controller);
  QTemporaryDir directory("/tmp/putgd-hold-XXXXXX");QVERIFY(directory.isValid());const auto trace=directory.filePath("trace.jsonl");QVERIFY(createCursorTrace(trace));
  qputenv("PUTGD_TEST_CAPTURE_MODE","preview-waiting");
  // A hold's Leave events would change what Screenshot first is about to freeze.
  QVERIFY(controller.holdCrosshair());
  auto ticket=controller.prepare(regionSnapshot(),{},{},{},{},true);
  QCOMPARE(pointerGrabStatus(connection),int(XCB_GRAB_STATUS_SUCCESS));
  PUTGD_TRY_COMPARE(cursorTrace(trace).size(),2);QCOMPARE(cursorTrace(trace)[1]["hold"].toString(),QString("0"));
  controller.cancel();PUTGD_TRY_VERIFY(!controller.busy());
 }
 void observedCursorEnablesTheHoldOnNewerQt(){
  if(cacheExpected())QSKIP("Qt below 6.5 seeds this cache from libXcursor");
  putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);seedCursor(controller);
  // The observed image never replaces the helper's own Qt cursor.
  QTemporaryDir directory("/tmp/putgd-hold-XXXXXX");QVERIFY(directory.isValid());const auto trace=directory.filePath("trace.jsonl");QVERIFY(createCursorTrace(trace));
  qputenv("PUTGD_TEST_CAPTURE_MODE","preview-handoff");QVERIFY(controller.holdCrosshair());
  auto ticket=controller.prepare(regionSnapshot(),{});PUTGD_TRY_COMPARE(cursorTrace(trace).size(),3);
  const auto frames=cursorTrace(trace);QCOMPARE(frames[0]["cursorId"].toString(),QString("0"));QCOMPARE(frames[1]["cursorId"].toString(),QString("0"));QVERIFY(frames[1]["hold"].toString().toULongLong()>0);
  controller.cancel();PUTGD_TRY_VERIFY(!controller.busy());
 }
 void cleanup(){qunsetenv("PUTGD_TEST_CAPTURE_MODE");qunsetenv("PUTGD_TEST_CAPTURE_TRACE");}
 void resolvedAppearanceReachesTheIsolatedCaptureHelper(){
  QTemporaryDir directory("/tmp/putgd-theme-XXXXXX");QVERIFY(directory.isValid());const auto trace=directory.filePath("trace.jsonl");QVERIFY(createCursorTrace(trace));
  putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);
  for(const auto& scheme:QStringList{"light","dark"}){
   const QJsonObject appearance{{"scheme",scheme},{"theme","system"}};int count=0;putgd::Error error;controller.start(snapshot(),{},[&](putgd::PayloadResult value){error=value.error;++count;},{},false,appearance);
   PUTGD_TRY_COMPARE(count,1);QVERIFY2(!error,qPrintable(error.message));QVERIFY(!controller.busy());const auto frames=cursorTrace(trace);QVERIFY(frames.size()>=2);QCOMPARE(frames[frames.size()-2]["appearance"].toObject(),appearance);
  }
 }
 void malformedReadyCursorIsRejectedBeforeBegin_data(){
  QTest::addColumn<QByteArray>("mode");QTest::addColumn<QString>("code");
  for(const char* mode:{"cursor-id","cursor-id-negative","cursor-id-overflow","cursor-extra", "cursor-zero-width","cursor-wide","cursor-negative-height","cursor-fraction","cursor-string-coordinate","cursor-xhot-outside","cursor-yhot-outside","cursor-short-pixels","cursor-invalid-base64","cursor-noncanonical-base64","cursor-long-pixels","cursor-nonlatin-pixels","cursor-pixels-type"})
   QTest::newRow(mode)<<QByteArray(mode)<<QString("capture_protocol");
  QTest::newRow("wrong-ready-cursor-type")<<QByteArray("cursor-ready-type")<<QString("capture_incompatible");
 }
 void malformedReadyCursorIsRejectedBeforeBegin(){
  QFETCH(QByteArray,mode);QFETCH(QString,code);
  putgd::CaptureCursorCache backend;QVERIFY2(backend.supported(),"Run cache protocol tests on the private Render-capable Xvfb server");
  QTemporaryDir directory("/tmp/putgd-cache-XXXXXX");QVERIFY(directory.isValid());const auto trace=directory.filePath("trace.jsonl");QVERIFY(createCursorTrace(trace));
  qputenv("PUTGD_TEST_CAPTURE_MODE",mode);putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);
  int count=0;putgd::Error error;controller.start(regionSnapshot(),{},[&](putgd::PayloadResult value){error=value.error;++count;});
  PUTGD_TRY_COMPARE(count,1);QCOMPARE(error.code,code);QVERIFY(!controller.busy());QCOMPARE(controller.processId(),qint64(0));
  putgd::test::settle(20);PUTGD_TRY_COMPARE(count,1);
  const auto frames=cursorTrace(trace);QCOMPARE(frames.size(),1);QCOMPARE(frames[0]["phase"].toString(),QString("bootstrap"));
 }
 void nonRegionReadyCannotSupplyCursorSeed(){
  qputenv("PUTGD_TEST_CAPTURE_MODE","cursor-force-seed");putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);
  int count=0;putgd::Error error;controller.start(snapshot(),{},[&](putgd::PayloadResult value){error=value.error;++count;});
  PUTGD_TRY_COMPARE(count,1);QCOMPARE(error.code,QString("capture_protocol"));QVERIFY(!controller.busy());
 }
 void cursorCacheSurvivesHelperExitAndDesktopCaptureButInvalidatesFingerprint(){
  putgd::CaptureCursorCache backend;QVERIFY2(backend.supported(),"Run cache reuse tests on the private Render-capable Xvfb server");
  QTemporaryDir directory("/tmp/putgd-cache-XXXXXX");QVERIFY(directory.isValid());const auto trace=directory.filePath("trace.jsonl");QVERIFY(createCursorTrace(trace));
  putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);
  const QList<QJsonObject> targets{regionSnapshot('a'),snapshot(),regionSnapshot('a'),regionSnapshot('b'),regionSnapshot('a'),regionSnapshot('a')};
  for(qsizetype index=0;index<targets.size();++index){
   qputenv("PUTGD_TEST_CAPTURE_MODE",index==1||index==5?"cursor-empty":"cursor-seed");int count=0;putgd::PayloadResult result;
   controller.start(targets[index],{},[&](putgd::PayloadResult value){result=std::move(value);++count;});PUTGD_TRY_COMPARE(count,1);
   QVERIFY2(!result.error,qPrintable(result.error.code));QVERIFY(!controller.busy());QCOMPARE(controller.processId(),qint64(0));
   const auto exit=controller.diagnostics()["lastExit"].toObject();QCOMPARE(exit["status"].toString(),QString("normal"));QCOMPARE(exit["code"].toInt(),0);
  }
  const auto frames=cursorTrace(trace);QCOMPARE(frames.size(),12);
  for(qsizetype index=0;index<targets.size();++index){
   const auto bootstrap=frames[index*2],begin=frames[index*2+1];
   QCOMPARE(bootstrap["phase"].toString(),QString("bootstrap"));QCOMPARE(begin["phase"].toString(),QString("begin"));
   QCOMPARE(bootstrap["generation"].toString(),QString::number(index+1));QCOMPARE(begin["generation"],bootstrap["generation"]);
   QCOMPARE(bootstrap["action"],targets[index]["action"]);QCOMPARE(bootstrap["cache"].toBool(),cacheExpected()&&index!=1);
  }
  if(!cacheExpected()){for(const auto& frame:frames){QVERIFY(!frame["cache"].toBool());QCOMPARE(frame["cursorId"].toString(),QString("0"));}return;}
  const auto first=frames[1]["cursorId"].toString();QVERIFY(first.toULongLong()>0);QCOMPARE(frames[0]["cursorId"].toString(),QString("0"));
  QCOMPARE(frames[2]["cursorId"].toString(),QString("0"));QCOMPARE(frames[3]["cursorId"].toString(),QString("0"));
  QCOMPARE(frames[4]["cursorId"].toString(),first);QCOMPARE(frames[5]["cursorId"].toString(),first);
  QCOMPARE(frames[6]["cursorId"].toString(),QString("0"));const auto changed=frames[7]["cursorId"].toString();QVERIFY(changed.toULongLong()>0);QVERIFY(changed!=first);
  QCOMPARE(frames[8]["cursorId"].toString(),QString("0"));const auto restored=frames[9]["cursorId"].toString();QVERIFY(restored.toULongLong()>0);QVERIFY(restored!=changed);QVERIFY(restored!=first);
  QCOMPARE(frames[10]["cursorId"].toString(),restored);QCOMPARE(frames[11]["cursorId"].toString(),restored);
 }
 void cursorCacheInvalidatesNativeLayoutWithoutScaleChange_data(){
  QTest::addColumn<int>("change");
  QTest::newRow("root-dimensions")<<0;
  QTest::newRow("output-dimensions")<<1;
  QTest::newRow("root-and-output-dimensions")<<2;
 }
 void cursorCacheInvalidatesNativeLayoutWithoutScaleChange(){
  QFETCH(int,change);
  putgd::CaptureCursorCache backend;QVERIFY2(backend.supported(),"Run native-layout cache tests on the private Render-capable Xvfb server");
  QTemporaryDir directory("/tmp/putgd-cache-XXXXXX");QVERIFY(directory.isValid());const auto trace=directory.filePath("trace.jsonl");QVERIFY(createCursorTrace(trace));
  const auto original=regionSnapshot();auto changed=original;auto layout=changed["layout"].toObject();
  if(change!=1)layout["root"]=QJsonArray{0,0,800,600};
  if(change!=0){auto outputs=layout["outputs"].toArray();auto output=outputs[0].toObject();output["native"]=change==1?QJsonArray{0,0,320,240}:QJsonArray{0,0,800,600};outputs[0]=output;layout["outputs"]=outputs;}
  changed["layout"]=layout;
  QCOMPARE(layout["scale"],original["layout"].toObject()["scale"]);
  QCOMPARE(layout["configuration"],original["layout"].toObject()["configuration"]);
  QVERIFY(layout!=original["layout"].toObject());
  qputenv("PUTGD_TEST_CAPTURE_MODE","cursor-seed");putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);
  for(const auto& target:QList<QJsonObject>{original,original,changed,changed}){
   int count=0;putgd::PayloadResult result;controller.start(target,{},[&](putgd::PayloadResult value){result=std::move(value);++count;});PUTGD_TRY_COMPARE(count,1);
   QVERIFY2(!result.error,qPrintable(result.error.code));QVERIFY(!controller.busy());QCOMPARE(controller.processId(),qint64(0));
  }
  const auto frames=cursorTrace(trace);QCOMPARE(frames.size(),8);
  if(!cacheExpected()){for(const auto& frame:frames){QVERIFY(!frame["cache"].toBool());QCOMPARE(frame["cursorId"].toString(),QString("0"));}return;}
  const auto first=frames[1]["cursorId"].toString();QVERIFY(first.toULongLong()>0);
  QCOMPARE(frames[0]["cursorId"].toString(),QString("0"));QCOMPARE(frames[2]["cursorId"].toString(),first);QCOMPARE(frames[3]["cursorId"].toString(),first);
  QCOMPARE(frames[4]["cursorId"].toString(),QString("0"));const auto second=frames[5]["cursorId"].toString();QVERIFY(second.toULongLong()>0);QVERIFY(second!=first);
  QCOMPARE(frames[6]["cursorId"].toString(),second);QCOMPARE(frames[7]["cursorId"].toString(),second);
 }
 void cursorEnvironmentFrozenLayoutInvalidatesResidentCache(){
  const QList<QByteArray> names{"XDG_SESSION_TYPE","WAYLAND_DISPLAY","QT_QPA_PLATFORM","XCURSOR_RESIZED","XCURSOR_THEME"};
  QList<bool> present;QList<QByteArray> previous;
  for(const auto& name:names){present.append(qEnvironmentVariableIsSet(name.constData()));previous.append(qgetenv(name.constData()));}
  const auto restore=qScopeGuard([&]{for(qsizetype i=0;i<names.size();++i){if(present[i])qputenv(names[i].constData(),previous[i]);else qunsetenv(names[i].constData());}});
  // This CTest owns a private Xvfb server; isolate its native metadata route from the host session.
  qputenv("XDG_SESSION_TYPE","x11");qunsetenv("WAYLAND_DISPLAY");qputenv("QT_QPA_PLATFORM","xcb");qunsetenv("XCURSOR_RESIZED");
  putgd::X11NativeSnapshot native;QVERIFY(native.available());putgd::Error error;
  const auto original=native.freeze("region",{},error);QVERIFY2(!error,qPrintable(error.code));
  putgd::CaptureCursorCache backend;QVERIFY2(backend.supported(),"Run environment cache tests on the private Render-capable Xvfb server");
  QTemporaryDir directory("/tmp/putgd-cache-XXXXXX");QVERIFY(directory.isValid());const auto trace=directory.filePath("trace.jsonl");QVERIFY(createCursorTrace(trace));
  qputenv("PUTGD_TEST_CAPTURE_MODE","cursor-seed");putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);
  for(int stage=0;stage<2;++stage){
   QJsonObject target=original;
   if(stage){qputenv("XCURSOR_THEME",previous.last()+"-putgd-synthetic-change");target=native.freeze("region",{},error);QVERIFY2(!error,qPrintable(error.code));QVERIFY(target["layout"].toObject()["scale"]!=original["layout"].toObject()["scale"]);}
   for(int repeat=0;repeat<2;++repeat){
    int count=0;putgd::PayloadResult result;controller.start(target,{},[&](putgd::PayloadResult value){result=std::move(value);++count;});PUTGD_TRY_COMPARE(count,1);
    QVERIFY2(!result.error,qPrintable(result.error.code));QVERIFY(!controller.busy());QCOMPARE(controller.processId(),qint64(0));
    const auto exit=controller.diagnostics()["lastExit"].toObject();QCOMPARE(exit["status"].toString(),QString("normal"));QCOMPARE(exit["code"].toInt(),0);
   }
  }
  const auto frames=cursorTrace(trace);QCOMPARE(frames.size(),8);
  for(int index=0;index<4;++index){QCOMPARE(frames[index*2]["phase"].toString(),QString("bootstrap"));QCOMPARE(frames[index*2+1]["phase"].toString(),QString("begin"));QCOMPARE(frames[index*2]["generation"].toString(),QString::number(index+1));QCOMPARE(frames[index*2+1]["generation"],frames[index*2]["generation"]);}
  if(!cacheExpected()){for(const auto& frame:frames){QVERIFY(!frame["cache"].toBool());QCOMPARE(frame["cursorId"].toString(),QString("0"));}return;}
  for(int index=0;index<4;++index)QVERIFY(frames[index*2]["cache"].toBool());
  const auto first=frames[1]["cursorId"].toString();QVERIFY(first.toULongLong()>0);QCOMPARE(frames[0]["cursorId"].toString(),QString("0"));QCOMPARE(frames[2]["cursorId"].toString(),first);QCOMPARE(frames[3]["cursorId"].toString(),first);
  const auto changed=frames[5]["cursorId"].toString();QVERIFY(changed.toULongLong()>0);QVERIFY(changed!=first);QCOMPARE(frames[4]["cursorId"].toString(),QString("0"));QCOMPARE(frames[6]["cursorId"].toString(),changed);QCOMPARE(frames[7]["cursorId"].toString(),changed);
 }
 void unsolicitedReplacementSeedCannotPoisonCachedCursor(){
  putgd::CaptureCursorCache backend;QVERIFY2(backend.supported(),"Run cache replacement tests on the private Render-capable Xvfb server");
  QTemporaryDir directory("/tmp/putgd-cache-XXXXXX");QVERIFY(directory.isValid());const auto trace=directory.filePath("trace.jsonl");QVERIFY(createCursorTrace(trace));
  putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);
  for(int index=0;index<3;++index){
   qputenv("PUTGD_TEST_CAPTURE_MODE",index==1?"cursor-force-seed":"cursor-seed");int count=0;putgd::PayloadResult result;
   controller.start(regionSnapshot(),{},[&](putgd::PayloadResult value){result=std::move(value);++count;});PUTGD_TRY_COMPARE(count,1);
   if(index==1)QCOMPARE(result.error.code,QString("capture_protocol"));else QVERIFY2(!result.error,qPrintable(result.error.code));
   QVERIFY(!controller.busy());
  }
  const auto frames=cursorTrace(trace);QCOMPARE(frames.size(),5);
  if(!cacheExpected()){for(const auto& frame:frames){QVERIFY(!frame["cache"].toBool());QCOMPARE(frame["cursorId"].toString(),QString("0"));}return;}
  const auto first=frames[1]["cursorId"].toString();QVERIFY(first.toULongLong()>0);
  QCOMPARE(frames[2]["phase"].toString(),QString("bootstrap"));QCOMPARE(frames[2]["cursorId"].toString(),first);
  QCOMPARE(frames[3]["phase"].toString(),QString("bootstrap"));QCOMPARE(frames[3]["cursorId"].toString(),first);QCOMPARE(frames[4]["cursorId"].toString(),first);
 }
 void optionalCursorExportCanStayEmpty(){
  putgd::CaptureCursorCache backend;QVERIFY2(backend.supported(),"Run optional export tests on the private Render-capable Xvfb server");
  QTemporaryDir directory("/tmp/putgd-cache-XXXXXX");QVERIFY(directory.isValid());const auto trace=directory.filePath("trace.jsonl");QVERIFY(createCursorTrace(trace));
  qputenv("PUTGD_TEST_CAPTURE_MODE","cursor-empty");putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);
  for(int index=0;index<2;++index){int count=0;putgd::PayloadResult result;controller.start(regionSnapshot(),{},[&](putgd::PayloadResult value){result=std::move(value);++count;});PUTGD_TRY_COMPARE(count,1);QVERIFY2(!result.error,qPrintable(result.error.code));QVERIFY(!controller.busy());}
  const auto frames=cursorTrace(trace);QCOMPARE(frames.size(),4);
  for(const auto& frame:frames){QCOMPARE(frame["cache"].toBool(),cacheExpected());QCOMPARE(frame["cursorId"].toString(),QString("0"));}
 }
 void explicitResizeDisablesResidentCacheAcrossGenerations(){
  const bool present=qEnvironmentVariableIsSet("XCURSOR_RESIZED");const auto previous=qgetenv("XCURSOR_RESIZED");
  const auto restore=qScopeGuard([&]{if(present)qputenv("XCURSOR_RESIZED",previous);else qunsetenv("XCURSOR_RESIZED");});
  QTemporaryDir directory("/tmp/putgd-cache-XXXXXX");QVERIFY(directory.isValid());const auto trace=directory.filePath("trace.jsonl");QVERIFY(createCursorTrace(trace));
  qunsetenv("XCURSOR_RESIZED");putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);qputenv("PUTGD_TEST_CAPTURE_MODE","cursor-seed");
  int primed=0;putgd::Error error;controller.start(regionSnapshot(),{},[&](putgd::PayloadResult result){error=result.error;++primed;});PUTGD_TRY_COMPARE(primed,1);QVERIFY(!error);
  QVERIFY(!controller.busy());const auto prior=cursorTrace(trace);QCOMPARE(prior.size(),2);QCOMPARE(prior[0]["cache"].toBool(),cacheExpected());if(cacheExpected())QVERIFY(prior[1]["cursorId"].toString().toULongLong()>0);else QCOMPARE(prior[1]["cursorId"].toString(),QString("0"));
  QVERIFY(createCursorTrace(trace));qputenv("XCURSOR_RESIZED","1");
  for(int index=0;index<2;++index){int count=0;controller.start(regionSnapshot(),{},[&](putgd::PayloadResult result){error=result.error;++count;});PUTGD_TRY_COMPARE(count,1);QVERIFY(!error);QVERIFY(!controller.busy());QCOMPARE(controller.processId(),qint64(0));}
  const auto frames=cursorTrace(trace);QCOMPARE(frames.size(),4);for(const auto& frame:frames){QVERIFY(!frame["cache"].toBool());QCOMPARE(frame["cursorId"].toString(),QString("0"));}
 }
 void unavailableCursorCachePreservesOptionalFallback(){
  const bool hadDisplay=qEnvironmentVariableIsSet("DISPLAY");const auto previousDisplay=qgetenv("DISPLAY");qunsetenv("DISPLAY");
  const auto restore=qScopeGuard([&]{if(hadDisplay)qputenv("DISPLAY",previousDisplay);else qunsetenv("DISPLAY");});
  putgd::CaptureCursorCache backend;QVERIFY(!backend.supported());
  QTemporaryDir directory("/tmp/putgd-cache-XXXXXX");QVERIFY(directory.isValid());const auto trace=directory.filePath("trace.jsonl");QVERIFY(createCursorTrace(trace));
  qputenv("PUTGD_TEST_CAPTURE_MODE","cursor-seed");putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);
  int count=0;putgd::PayloadResult result;controller.start(regionSnapshot(),{},[&](putgd::PayloadResult value){result=std::move(value);++count;});PUTGD_TRY_COMPARE(count,1);
  QVERIFY2(!result.error,qPrintable(result.error.code));QVERIFY(!controller.busy());const auto frames=cursorTrace(trace);QCOMPARE(frames.size(),2);
  for(const auto& frame:frames){QVERIFY(!frame["cache"].toBool());QCOMPARE(frame["cursorId"].toString(),QString("0"));}
 }
 void captureOutcomeTravelsOutsideTheFrozenTarget(){
  QTemporaryDir dir;const auto trace=dir.filePath("outcome.jsonl");QFile file(trace);QVERIFY(file.open(QIODevice::WriteOnly));QVERIFY(file.setPermissions(QFile::ReadOwner|QFile::WriteOwner));file.close();
  qputenv("PUTGD_TEST_CAPTURE_TRACE",trace.toUtf8());
  putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);bool done=false;
  const auto frozen=snapshot();controller.start(frozen,{},[&](putgd::PayloadResult result){QVERIFY(!result.error);done=true;},"Opens the image editor");
  PUTGD_TRY_VERIFY(done);const auto frames=cursorTrace(trace);QVERIFY(!frames.isEmpty());QCOMPARE(frames.first()["outcome"].toString(),QString("Opens the image editor"));QCOMPARE(frozen,snapshot());
 }
 void genuinePngValidatedOnlyAfterNormalChildExit(){putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);int completed=0;putgd::PayloadResult result;controller.start(snapshot(),{},[&](putgd::PayloadResult r){result=std::move(r);++completed;});PUTGD_TRY_COMPARE(completed,1);QVERIFY2(!result.error,qPrintable(result.error.code));QCOMPARE(result.payload.image.size(),QSize(3,2));QCOMPARE(result.payload.image.pixelColor(1,1),QColor(Qt::blue));QVERIFY(!controller.busy());QCOMPARE(controller.processId(),qint64(0));const auto exit=controller.diagnostics()["lastExit"].toObject();QCOMPARE(exit["status"].toString(),QString("normal"));QCOMPARE(exit["code"].toInt(),0);QVERIFY(exit["processId"].toInteger()>0);QVERIFY(exit["startTicks"].toString().toULongLong()>0);}
 void preparedHelperStaysIdleUntilBegin(){
  putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);
  auto ticket=controller.prepare(snapshot(),{});QVERIFY(controller.busy());
  PUTGD_TRY_VERIFY(controller.preparedReady());putgd::test::settle(40);PUTGD_TRY_VERIFY(controller.busy());
  int count=0;putgd::PayloadResult result;
  controller.begin(ticket,[&](putgd::PayloadResult value){result=std::move(value);++count;});
  PUTGD_TRY_COMPARE(count,1);QVERIFY2(!result.error,qPrintable(result.error.code));QVERIFY(!controller.busy());
 }
 void regionPreviewPrecedesBeginWithoutProducingImage_data(){
  QTest::addColumn<bool>("cachedCursor");
  QTest::newRow("fallback-cursor")<<false;
  QTest::newRow("cached-cursor")<<true;
 }
 void regionPreviewPrecedesBeginWithoutProducingImage(){
  QFETCH(bool,cachedCursor);putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);
  if(cachedCursor){
   putgd::CaptureCursorCache backend;QVERIFY2(backend.supported(),"Run preview cache tests on the private Render-capable Xvfb server");
   qputenv("PUTGD_TEST_CAPTURE_MODE","cursor-seed");int completed=0;putgd::Error error;
   controller.start(regionSnapshot(),{},[&](putgd::PayloadResult value){error=value.error;++completed;});
   PUTGD_TRY_COMPARE(completed,1);QVERIFY2(!error,qPrintable(error.code));
  }
  QTemporaryDir directory("/tmp/putgd-preview-XXXXXX");QVERIFY(directory.isValid());const auto trace=directory.filePath("trace.jsonl");QVERIFY(createCursorTrace(trace));
  qputenv("PUTGD_TEST_CAPTURE_MODE","preview-waiting");int early=0;
  auto ticket=controller.prepare(regionSnapshot(),{},[&](putgd::Error){++early;});
  PUTGD_TRY_COMPARE(cursorTrace(trace).size(),2);QVERIFY(controller.preparedReady());QCOMPARE(early,0);
  const auto before=cursorTrace(trace);QCOMPARE(before[0]["phase"].toString(),QString("bootstrap"));QCOMPARE(before[1]["phase"].toString(),QString("preview"));
  QCOMPARE(before[1]["generation"],before[0]["generation"]);
  QCOMPARE(before[0]["cache"].toBool(),cacheExpected());if(!cacheExpected())QCOMPARE(before[0]["cursorId"].toString(),QString("0"));
  const auto preview=before[1]["cursorId"].toString();if(cachedCursor&&cacheExpected())QVERIFY(preview.toULongLong()>0);else QCOMPARE(preview,QString("0"));
  putgd::test::settle(40);PUTGD_TRY_COMPARE(cursorTrace(trace).size(),2);PUTGD_TRY_COMPARE(early,0);PUTGD_TRY_VERIFY(controller.busy());
  int completed=0;putgd::PayloadResult result;
  controller.begin(ticket,[&](putgd::PayloadResult value){result=std::move(value);++completed;});
  PUTGD_TRY_COMPARE(completed,1);QVERIFY2(!result.error,qPrintable(result.error.code));QCOMPARE(result.payload.image.size(),QSize(3,2));QCOMPARE(early,0);QVERIFY(!controller.busy());
  const auto after=cursorTrace(trace);QCOMPARE(after.size(),3);QCOMPARE(after[2]["phase"].toString(),QString("begin"));QCOMPARE(after[2]["cursorId"].toString(),preview);
 }
 void earlyPreviewTerminalIsReportedOnceAndRetained_data(){
  QTest::addColumn<QByteArray>("mode");QTest::addColumn<QString>("code");
  QTest::newRow("escape")<<QByteArray("preview-cancel")<<QString("cancelled");
  QTest::newRow("display-error")<<QByteArray("preview-error")<<QString("capture_geometry");
  // A delayed live region whose countdown ends on a locked screen (#1363): the lock, not a protocol fault.
  QTest::newRow("locked-at-expiry")<<QByteArray("preview-locked")<<QString("session_locked");
 }
 void earlyPreviewTerminalIsReportedOnceAndRetained(){
  QFETCH(QByteArray,mode);QFETCH(QString,code);
  QTemporaryDir directory("/tmp/putgd-preview-XXXXXX");QVERIFY(directory.isValid());const auto trace=directory.filePath("trace.jsonl");QVERIFY(createCursorTrace(trace));
  qputenv("PUTGD_TEST_CAPTURE_MODE",mode);putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);
  int early=0;bool aliveAtNotification=false;putgd::Error terminal;
  auto ticket=controller.prepare(regionSnapshot(),{},[&](putgd::Error error){terminal=std::move(error);aliveAtNotification=controller.busy()&&controller.processId()>0;++early;});
  PUTGD_TRY_COMPARE(early,1);QCOMPARE(terminal.code,code);QVERIFY(aliveAtNotification);QVERIFY(!controller.preparedReady());
  // The helper's own reason and geometry reach the operation's error (#331).
  if(code=="capture_geometry"){QCOMPARE(terminal.message,QString("Synthetic selection outside the layout"));QCOMPARE(terminal.details["check"].toString(),QString("fixture_selection"));QCOMPARE(terminal.details["selection"].toArray(),(QJsonArray{0,1080,100,100}));}
  // The lock keeps its own code and wording; it is not reported as a protocol fault.
  else if(code=="session_locked")QCOMPARE(terminal.message,QString("The screen was locked when the capture delay ended, so nothing was captured."));
  else QVERIFY(terminal.details.isEmpty());
  // BEGIN must deliver the latched failure even while the old helper is exiting,
  // and cannot authorize a pixel read or start another child.
  int completed=0;putgd::Error result;
  controller.begin(ticket,[&](putgd::PayloadResult value){result=value.error;++completed;});
  QCOMPARE(completed,1);QCOMPARE(result.code,code);
  PUTGD_TRY_VERIFY(!controller.busy());QCOMPARE(controller.processId(),qint64(0));putgd::test::settle(30);PUTGD_TRY_COMPARE(early,1);PUTGD_TRY_COMPARE(completed,1);
  const auto frames=cursorTrace(trace);QCOMPARE(frames.size(),2);QCOMPARE(frames[1]["phase"].toString(),QString("preview"));
 }
 void nonRegionPreparationNeverRequestsPreview_data(){
  QTest::addColumn<QString>("action");QTest::newRow("desktop")<<QString("desktop");QTest::newRow("window")<<QString("window");
 }
 void nonRegionPreparationNeverRequestsPreview(){
  QFETCH(QString,action);auto target=snapshot();target["action"]=action;
  if(action=="window"){target["window"]="1";target["windowRect"]=QJsonArray{16,16,32,32};}
  QTemporaryDir directory("/tmp/putgd-preview-XXXXXX");QVERIFY(directory.isValid());const auto trace=directory.filePath("trace.jsonl");QVERIFY(createCursorTrace(trace));
  qputenv("PUTGD_TEST_CAPTURE_MODE","preview-waiting");putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);int early=0;
  auto ticket=controller.prepare(target,{},[&](putgd::Error){++early;});PUTGD_TRY_VERIFY(controller.preparedReady());putgd::test::settle(40);
  const auto before=cursorTrace(trace);QCOMPARE(before.size(),1);QCOMPARE(before[0]["phase"].toString(),QString("bootstrap"));QCOMPARE(early,0);
  int completed=0;putgd::Error error;controller.begin(ticket,[&](putgd::PayloadResult value){error=value.error;++completed;});
  PUTGD_TRY_COMPARE(completed,1);QVERIFY2(!error,qPrintable(error.code));QCOMPARE(early,0);QVERIFY(!controller.busy());QCOMPARE(cursorTrace(trace).size(),2);
 }
 void cancellationWhileStartingSuppressesPreview(){
  QTemporaryDir directory("/tmp/putgd-preview-XXXXXX");QVERIFY(directory.isValid());const auto trace=directory.filePath("trace.jsonl");QVERIFY(createCursorTrace(trace));
  qputenv("PUTGD_TEST_CAPTURE_MODE","preview-waiting");putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);int early=0;putgd::Error terminal;
  auto ticket=controller.prepare(regionSnapshot(),{},[&](putgd::Error error){terminal=std::move(error);++early;});
  QVERIFY(!controller.preparedReady());controller.cancel();controller.cancel();
  PUTGD_TRY_COMPARE(early,1);QCOMPARE(terminal.code,QString("cancelled"));PUTGD_TRY_VERIFY(!controller.busy());
  int completed=0;putgd::Error error;controller.begin(ticket,[&](putgd::PayloadResult value){error=value.error;++completed;});
  QCOMPARE(completed,1);QCOMPARE(error.code,QString("cancelled"));putgd::test::settle(30);PUTGD_TRY_COMPARE(early,1);
  const auto frames=cursorTrace(trace);QCOMPARE(frames.size(),1);QCOMPARE(frames[0]["phase"].toString(),QString("bootstrap"));
  const auto exit=controller.diagnostics()["lastExit"].toObject();QCOMPARE(exit["status"].toString(),QString("normal"));QCOMPARE(exit["code"].toInt(),0);
 }
 void sourceReleasePrecedesCallbacksThatDestroyController_data(){
  QTest::addColumn<bool>("deleteOnRelease");QTest::newRow("delete-in-acquisition")<<false;QTest::newRow("delete-in-release")<<true;
 }
 void sourceReleasePrecedesCallbacksThatDestroyController(){
  QFETCH(bool,deleteOnRelease);auto controller=std::make_unique<putgd::CaptureProcessController>(PUTGD_CAPTURE_FIXTURE);int releases=0,completed=0;
  auto ticket=controller->prepare(snapshot(),{}, {},[&]{QVERIFY(!controller->busy());QCOMPARE(controller->processId(),qint64(0));++releases;if(deleteOnRelease)controller.reset();});
  controller->begin(ticket,[&](putgd::PayloadResult result){QVERIFY(!result.error);QCOMPARE(releases,1);++completed;controller.reset();});
  PUTGD_TRY_COMPARE(completed,1);QCOMPARE(releases,1);QVERIFY(!controller);putgd::test::settle(30);PUTGD_TRY_COMPARE(completed,1);PUTGD_TRY_COMPARE(releases,1);
 }
 void earlyReleaseWaitsForCompleteHelperCleanup(){
  qputenv("PUTGD_TEST_CAPTURE_MODE","preview-error");putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);
  int terminal=0,released=0;
  auto ticket=controller.prepare(regionSnapshot(),{},[&](putgd::Error error){QCOMPARE(error.code,QString("capture_geometry"));QVERIFY(controller.busy());QCOMPARE(released,0);++terminal;},[&]{QVERIFY(!controller.busy());QCOMPARE(controller.processId(),qint64(0));QCOMPARE(terminal,1);++released;});
  PUTGD_TRY_COMPARE(terminal,1);PUTGD_TRY_COMPARE(released,1);putgd::test::settle(30);PUTGD_TRY_COMPARE(released,1);
  int count=0;controller.begin(ticket,[&](putgd::PayloadResult result){QCOMPARE(result.error.code,QString("capture_geometry"));++count;});QCOMPARE(count,1);
 }
 void cancelledMissingHelperRetainsFirstTerminalError(){
  putgd::CaptureProcessController controller("/missing-putgd-preview-helper");int early=0;putgd::Error terminal;
  auto ticket=controller.prepare(regionSnapshot(),{},[&](putgd::Error error){terminal=error;++early;});
  controller.cancel();PUTGD_TRY_VERIFY(!controller.busy());PUTGD_TRY_COMPARE(early,1);QCOMPARE(terminal.code,QString("cancelled"));
  int count=0;putgd::Error retained;controller.begin(ticket,[&](putgd::PayloadResult result){retained=result.error;++count;});
  QCOMPARE(count,1);QCOMPARE(retained.code,terminal.code);putgd::test::settle(30);PUTGD_TRY_COMPARE(early,1);
 }
 void earlyTerminalCallbackCanDestroyController(){
  qputenv("PUTGD_TEST_CAPTURE_MODE","preview-error");auto controller=std::make_unique<putgd::CaptureProcessController>(PUTGD_CAPTURE_FIXTURE);int early=0;
  auto ticket=controller->prepare(regionSnapshot(),{},[&](putgd::Error error){QCOMPARE(error.code,QString("capture_geometry"));++early;controller.reset();});
  PUTGD_TRY_COMPARE(early,1);QVERIFY(!controller);ticket.reset();putgd::test::settle(30);PUTGD_TRY_COMPARE(early,1);
 }
 void cancellationNotificationCanDestroyControllerWhileStarting(){
  qputenv("PUTGD_TEST_CAPTURE_MODE","preview-waiting");auto controller=std::make_unique<putgd::CaptureProcessController>(PUTGD_CAPTURE_FIXTURE);int early=0;
  auto ticket=controller->prepare(regionSnapshot(),{},[&](putgd::Error error){QCOMPARE(error.code,QString("cancelled"));++early;controller.reset();});
  QVERIFY(!controller->preparedReady());controller->cancel();
  PUTGD_TRY_COMPARE(early,1);QVERIFY(!controller);ticket.reset();putgd::test::settle(30);PUTGD_TRY_COMPARE(early,1);
 }
 void terminalAfterBeginRequestUsesOnlyAcquisitionCallback_data(){
  QTest::addColumn<QByteArray>("mode");QTest::addColumn<QString>("code");
  QTest::newRow("escape")<<QByteArray("preview-cancel")<<QString("cancelled");
  QTest::newRow("display-error")<<QByteArray("preview-error")<<QString("capture_geometry");
  QTest::newRow("locked-at-expiry")<<QByteArray("preview-locked")<<QString("session_locked");
 }
 void terminalAfterBeginRequestUsesOnlyAcquisitionCallback(){
  QFETCH(QByteArray,mode);QFETCH(QString,code);qputenv("PUTGD_TEST_CAPTURE_MODE",mode);
  putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);int early=0,completed=0;putgd::Error error;
  auto ticket=controller.prepare(regionSnapshot(),{},[&](putgd::Error){++early;});
  // A requested BEGIN transfers terminal delivery to the ordinary callback,
  // even if READY and the preview's terminal event have not arrived yet.
  controller.begin(ticket,[&](putgd::PayloadResult value){error=value.error;++completed;});
  PUTGD_TRY_COMPARE(completed,1);QCOMPARE(error.code,code);QCOMPARE(early,0);QVERIFY(!controller.busy());
  putgd::test::settle(30);PUTGD_TRY_COMPARE(completed,1);PUTGD_TRY_COMPARE(early,0);
 }
 void preparedLimitsRemainTheDecodeBound(){
  putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);putgd::ResourceLimits limits;limits.imagePixels=1;limits.imageBytes=4;
  auto ticket=controller.prepare(snapshot(),limits);PUTGD_TRY_VERIFY(controller.preparedReady());int count=0;putgd::Error error;
  controller.begin(ticket,[&](putgd::PayloadResult value){error=value.error;++count;});
  PUTGD_TRY_COMPARE(count,1);QCOMPARE(error.code,QString("input_excessive"));QVERIFY(!controller.busy());
 }
 void abandonedPreparationCancelsWithoutBegin(){
  putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);
  auto ticket=controller.prepare(snapshot(),{});PUTGD_TRY_VERIFY(controller.preparedReady());ticket.reset();
  PUTGD_TRY_VERIFY(!controller.busy());QCOMPARE(controller.processId(),qint64(0));
  QCOMPARE(controller.diagnostics()["lastExit"].toObject()["code"].toInt(),0);
  QCOMPARE(controller.diagnostics()["lastExit"].toObject()["status"].toString(),QString("normal"));
 }
 void lastPreparationCanDieOnJournalWorker(){
  putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);
  auto ticket=controller.prepare(snapshot(),{});PUTGD_TRY_VERIFY(controller.preparedReady());
  std::thread worker([ticket=std::move(ticket)]()mutable{ticket.reset();});worker.join();
  PUTGD_TRY_VERIFY(!controller.busy());QCOMPARE(controller.processId(),qint64(0));
  QCOMPARE(controller.diagnostics()["lastExit"].toObject()["code"].toInt(),0);
 }
 void earlyFailureIsRetainedUntilBegin(){
  qputenv("PUTGD_TEST_CAPTURE_MODE","early-exit");
  putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);
  auto ticket=controller.prepare(snapshot(),{});PUTGD_TRY_VERIFY(!controller.busy());
  int count=0;putgd::Error error;
  controller.begin(ticket,[&](putgd::PayloadResult value){error=value.error;++count;});
  QCOMPARE(count,1);QVERIFY(error);putgd::test::settle(30);PUTGD_TRY_COMPARE(count,1);PUTGD_TRY_VERIFY(!controller.busy());
 }
 void cancelledPreparationNeverRestartsWhenBeginArrives(){
  putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);
  auto ticket=controller.prepare(snapshot(),{});controller.cancel();PUTGD_TRY_VERIFY(!controller.busy());
  const auto previous=controller.diagnostics()["lastExit"].toObject();int count=0;putgd::Error error;
  controller.begin(ticket,[&](putgd::PayloadResult value){error=value.error;++count;});
  QCOMPARE(count,1);QCOMPARE(error.code,QString("cancelled"));QVERIFY(!controller.busy());
  QCOMPARE(controller.diagnostics()["lastExit"].toObject(),previous);
 }
 void prebeginResultIsRejectedAndFailureRemembered(){
  qputenv("PUTGD_TEST_CAPTURE_MODE","prebegin-result");
  putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);
  auto ticket=controller.prepare(snapshot(),{});PUTGD_TRY_VERIFY(!controller.busy());int count=0;putgd::Error error;
  controller.begin(ticket,[&](putgd::PayloadResult value){error=value.error;++count;});
  QCOMPARE(count,1);QCOMPARE(error.code,QString("capture_protocol"));
 }
 void duplicateBeginDoesNotDisturbValidAcquisition(){
  putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);auto ticket=controller.prepare(snapshot(),{});
  int first=0,duplicate=0;putgd::Error duplicateError;
  controller.begin(ticket,[&](putgd::PayloadResult value){QVERIFY(!value.error);++first;});
  controller.begin(ticket,[&](putgd::PayloadResult value){duplicateError=value.error;++duplicate;});
  QCOMPARE(duplicate,1);QCOMPARE(duplicateError.code,QString("capture_protocol"));PUTGD_TRY_COMPARE(first,1);QVERIFY(!controller.busy());
 }
 void discardAfterBeginCannotCancelActiveCapture(){
  qputenv("PUTGD_TEST_CAPTURE_MODE","waiting");
  putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);auto ticket=controller.prepare(snapshot(),{});PUTGD_TRY_VERIFY(controller.preparedReady());
  int count=0;putgd::Error error;
  controller.begin(ticket,[&](putgd::PayloadResult value){error=value.error;++count;});
  controller.discard(ticket);putgd::test::settle(40);PUTGD_TRY_COMPARE(count,0);PUTGD_TRY_VERIFY(controller.busy());
  controller.cancel();PUTGD_TRY_COMPARE(count,1);QCOMPARE(error.code,QString("cancelled"));QVERIFY(!controller.busy());
 }
 void staleAbandonCannotCancelNewGeneration(){
  putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);auto old=controller.prepare(snapshot(),{});PUTGD_TRY_VERIFY(controller.preparedReady());
  controller.discard(old);PUTGD_TRY_VERIFY(!controller.busy());old.reset(); // queues old epoch cleanup
  auto current=controller.prepare(snapshot(),{});int count=0;
  controller.begin(current,[&](putgd::PayloadResult value){QVERIFY(!value.error);++count;});
  PUTGD_TRY_COMPARE(count,1);QVERIFY(!controller.busy());QCOMPARE(controller.diagnostics()["lastExit"].toObject()["generation"].toString(),QString("2"));
 }
 void trustedImageRespectsItsAcceptedBudgetWithoutChangingReaderGuard(){
  // CTest sets QT_IMAGEIO_MAXALLOC=1 before the first image reader is created.
  QCOMPARE(QImageReader::allocationLimit(),1);
  qputenv("PUTGD_TEST_CAPTURE_MODE","large");
  putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);
  int count=0;putgd::PayloadResult result;
  controller.start(snapshot(),{},[&](putgd::PayloadResult value){result=std::move(value);++count;});
  PUTGD_TRY_COMPARE(count,1);
  QVERIFY2(!result.error,qPrintable(result.error.code));
  QCOMPARE(result.payload.image.size(),QSize(700,700));
  QCOMPARE(result.payload.image.pixelColor(699,699),QColor(Qt::blue));
  QCOMPARE(QImageReader::allocationLimit(),1);
 }
 void immediateCancelWaitsForReadyAndExitsNormally(){
  qputenv("PUTGD_TEST_CAPTURE_MODE","waiting");
  putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);
  int count=0;putgd::Error error;
  controller.start(snapshot(),{},[&](putgd::PayloadResult value){error=value.error;++count;});
  controller.cancel();
  PUTGD_TRY_COMPARE(count,1);
  QCOMPARE(error.code,QString("cancelled"));
  const auto exit=controller.diagnostics()["lastExit"].toObject();
  QCOMPARE(exit["status"].toString(),QString("normal"));
  QCOMPARE(exit["code"].toInt(),0);
 }
 void malformedResultsSettleOnce_data(){QTest::addColumn<QByteArray>("mode");for(const char* mode:{"wrong-build","wrong-dimension","wrong-hash","duplicate","early-exit"})QTest::newRow(mode)<<QByteArray(mode);}
 void malformedResultsSettleOnce(){QFETCH(QByteArray,mode);qputenv("PUTGD_TEST_CAPTURE_MODE",mode);putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);int count=0;putgd::Error error;controller.start(snapshot(),{},[&](putgd::PayloadResult r){error=r.error;++count;});PUTGD_TRY_COMPARE(count,1);QVERIFY(error);QVERIFY(!controller.busy());putgd::test::settle(20);PUTGD_TRY_COMPARE(count,1);
  if(mode=="wrong-hash"){QCOMPARE(error.code,QString("capture_failed"));QCOMPARE(error.details["stage"].toString(),QString("resident_verify"));QCOMPARE(error.details["width"].toInt(),3);QCOMPARE(error.details["height"].toInt(),2);QVERIFY(error.message.contains("SHA-256"));}}
 void cancellationOwnsOnlyOneChildAndCompletesOnce(){qputenv("PUTGD_TEST_CAPTURE_MODE","waiting");putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);int count=0;putgd::Error error;controller.start(snapshot(),{},[&](putgd::PayloadResult r){error=r.error;++count;});PUTGD_TRY_VERIFY(controller.processId()>0);putgd::test::settle(30);controller.cancel();controller.cancel();PUTGD_TRY_COMPARE(count,1);QCOMPARE(error.code,QString("cancelled"));QVERIFY(!controller.busy());}
 void missingHelperCallbackCanDestroyController(){
  auto controller=std::make_unique<putgd::CaptureProcessController>("/does-not-exist-putgd-capture");
  int count=0;
  controller->start(snapshot(),{},[&](putgd::PayloadResult result){
   QVERIFY(result.error);
   ++count;
   controller.reset();
  });
  PUTGD_TRY_COMPARE(count,1);
  QVERIFY(!controller);
  putgd::test::settle(30);
  PUTGD_TRY_COMPARE(count,1);
 }
 void successfulCallbackCanDestroyController(){
  auto controller=std::make_unique<putgd::CaptureProcessController>(PUTGD_CAPTURE_FIXTURE);
  int count=0;
  controller->start(snapshot(),{},[&](putgd::PayloadResult result){
   QVERIFY(!result.error);
   QCOMPARE(result.payload.image.size(),QSize(3,2));
   ++count;
   controller.reset();
  });
  PUTGD_TRY_COMPARE(count,1);
  QVERIFY(!controller);
  putgd::test::settle(30);
  PUTGD_TRY_COMPARE(count,1);
 }
 void successfulCallbackCanStartNewGeneration(){
  putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);
  int count=0;
  controller.start(snapshot(),{},[&](putgd::PayloadResult first){
   QVERIFY(!first.error);
   ++count;
   controller.start(snapshot(),{},[&](putgd::PayloadResult second){
    QVERIFY(!second.error);
    ++count;
   });
  });
  PUTGD_TRY_COMPARE(count,2);
  QVERIFY(!controller.busy());
  QCOMPARE(controller.diagnostics()["lastExit"].toObject()["generation"].toString(),QString("2"));
 }
 void destructionCancelsOutstandingCallbackExactlyOnce(){
  qputenv("PUTGD_TEST_CAPTURE_MODE","waiting");
  auto controller=std::make_unique<putgd::CaptureProcessController>(PUTGD_CAPTURE_FIXTURE);
  int count=0;putgd::Error error;
  controller->start(snapshot(),{},[&](putgd::PayloadResult result){error=result.error;++count;});
  PUTGD_TRY_VERIFY(controller->processId()>0);
  controller.reset();
  QCOMPARE(count,1);
  QCOMPARE(error.code,QString("cancelled"));
  putgd::test::settle(30);
  PUTGD_TRY_COMPARE(count,1);
 }
 void helperEofCleansVerifiedArtifact(){
  qputenv("PUTGD_TEST_CAPTURE_MODE","waiting");
  QTemporaryDir directory("/tmp/putgd-capture-XXXXXX");QVERIFY(directory.isValid());
  const auto path=directory.filePath("result.png");QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));QVERIFY(file.setPermissions(QFile::ReadOwner|QFile::WriteOwner));file.close();
  struct stat st{};QVERIFY(!::lstat(QFile::encodeName(path).constData(),&st));
  QProcess process;process.setProcessChannelMode(QProcess::SeparateChannels);process.setReadChannel(QProcess::StandardOutput);
  putgd::CaptureControl control(process);bool ready=false;connect(&control,&putgd::CaptureControl::frame,this,[&](const QString& type,const QJsonObject&){if(type=="ready")ready=true;});
  process.start(PUTGD_CAPTURE_FIXTURE,{});QVERIFY(process.waitForStarted());
  QVERIFY(control.send("bootstrap",{{"build",QJsonDocument::fromJson(putgd::buildManifestJson()).object()},{"nonce",QUuid::createUuid().toString(QUuid::WithoutBraces)},{"generation","1"},{"target",snapshot()},{"cursor",QJsonObject{{"id","0"},{"cache",false},{"observe",false}}},{"path",path},{"device",QString::number(qulonglong(st.st_dev))},{"inode",QString::number(qulonglong(st.st_ino))},{"limits",QJsonObject{{"imagePixels",1000},{"imageBytes",4000},{"maxFileBytes",4000}}}}));
  PUTGD_TRY_VERIFY(ready);process.closeWriteChannel();QVERIFY(process.waitForFinished());QCOMPARE(process.exitStatus(),QProcess::NormalExit);QCOMPARE(process.exitCode(),3);QVERIFY(!QFile::exists(path));QVERIFY(!QDir(directory.path()).exists());
 }
 void artifactCleanupPreservesReplacementFile(){
  QTemporaryDir directory("/tmp/putgd-capture-XXXXXX");QVERIFY(directory.isValid());const auto path=directory.filePath("result.png");
  QFile original(path);QVERIFY(original.open(QIODevice::ReadWrite));QVERIFY(original.setPermissions(QFile::ReadOwner|QFile::WriteOwner));struct stat st{};QVERIFY(!::lstat(QFile::encodeName(path).constData(),&st));
  putgd::CaptureArtifactLease artifact;QVERIFY(artifact.acquire(path,QString::number(qulonglong(st.st_dev)),QString::number(qulonglong(st.st_ino))));
  QVERIFY(QFile::remove(path));QFile replacement(path);QVERIFY(replacement.open(QIODevice::WriteOnly));QVERIFY(replacement.setPermissions(QFile::ReadOwner|QFile::WriteOwner));QCOMPARE(replacement.write("replacement"),qint64(11));replacement.close();
  artifact.cleanup();QVERIFY(QFile::exists(path));QVERIFY(replacement.open(QIODevice::ReadOnly));QCOMPARE(replacement.readAll(),QByteArray("replacement"));
 }
 void artifactCleanupPreservesReplacementDirectory(){
  QTemporaryDir directory("/tmp/putgd-capture-XXXXXX");QVERIFY(directory.isValid());const auto oldPath=directory.path();const auto moved=oldPath+"-moved";const auto path=directory.filePath("result.png");
  QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));QVERIFY(file.setPermissions(QFile::ReadOwner|QFile::WriteOwner));file.close();struct stat st{};QVERIFY(!::lstat(QFile::encodeName(path).constData(),&st));
  putgd::CaptureArtifactLease artifact;QVERIFY(artifact.acquire(path,QString::number(qulonglong(st.st_dev)),QString::number(qulonglong(st.st_ino))));
  QVERIFY(QDir().rename(oldPath,moved));QVERIFY(QDir().mkdir(oldPath));QVERIFY(QFile::setPermissions(oldPath,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
  artifact.cleanup();QVERIFY(QDir(oldPath).exists());QVERIFY(QFile::exists(moved+"/result.png"));QVERIFY(QDir(moved).removeRecursively());
 }
 void missingHelperDoesNotRestart(){putgd::CaptureProcessController controller("/does-not-exist-putgd-capture");int count=0;controller.start(snapshot(),{},[&](putgd::PayloadResult r){QVERIFY(r.error);++count;});PUTGD_TRY_COMPARE(count,1);QVERIFY(!controller.busy());QCOMPARE(controller.diagnostics()["lastExit"].toObject()["processId"].toInteger(),qint64(0));QVERIFY(controller.diagnostics()["lastExit"].toObject()["startTicks"].toString().isEmpty());putgd::test::settle(30);PUTGD_TRY_COMPARE(count,1);}
};
QTEST_GUILESS_MAIN(CaptureProcessTests)
#include "test_capture_process.moc"
