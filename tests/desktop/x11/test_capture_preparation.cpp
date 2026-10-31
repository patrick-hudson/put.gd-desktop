// Actual helper protocol under isolated X11; not real-compositor acceptance.
#include "platform/x11/CaptureControl.h"
#include "platform/x11/X11NativeSnapshot.h"
#include "platform/x11/X11SourceAdapter.h"
#include "platform/x11/X11Reply.h"
#include "core/BuildInfo.h"
#include <QTest>
#include <QVersionNumber>
#include <QLibrary>
#include <QProcess>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QImage>
#include <QCryptographicHash>
#include <QScopeGuard>
#include <QWindow>
#include <QGuiApplication>
#include <QtEndian>
#include <xcb/xfixes.h>
#include <xcb/xcb_keysyms.h>
#include <QJsonDocument>
#include <QUuid>
#include <sys/stat.h>
#include <cstring>
#include <memory>
#include <X11/Xcursor/Xcursor.h>
#include "fixtures/Waits.h"
#undef None
#undef CursorShape
#undef Bool
#undef Status
using namespace putgd;
namespace {
struct HelperSession {
 QTemporaryDir directory{"/tmp/putgd-capture-XXXXXX"};
 QString path,nonce=QUuid::createUuid().toString(QUuid::WithoutBraces),failure;
 QFile artifact;QProcess process;CaptureControl control{process};QProcessEnvironment environment=QProcessEnvironment::systemEnvironment();
 bool ready=false;int results=0,cancellations=0,errors=0;QJsonObject result;
 ~HelperSession(){if(process.state()!=QProcess::NotRunning){process.kill();process.waitForFinished(2000);}}
 QJsonObject command(const QString& cursor="0")const{return {{"nonce",nonce},{"generation","1"},{"cursor",cursor}};}
 QJsonObject envelope()const{return {{"nonce",nonce},{"generation","1"}};}
 bool start(const QString& action="region"){
  X11NativeSnapshot snapshot;Error error;const auto frozen=snapshot.freeze(action,{},error);if(error){failure=error.code;return false;}
  if(!directory.isValid()||!QFile::setPermissions(directory.path(),QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner))return false;
  path=directory.filePath("result.png");artifact.setFileName(path);
  if(!artifact.open(QIODevice::WriteOnly)||!artifact.setPermissions(QFile::ReadOwner|QFile::WriteOwner))return false;
  artifact.close();if(!artifact.open(QIODevice::ReadOnly))return false;
  struct stat identity{};if(::lstat(QFile::encodeName(path).constData(),&identity))return false;
  process.setProcessChannelMode(QProcess::SeparateChannels);process.setReadChannel(QProcess::StandardOutput);
  QObject::connect(&control,&CaptureControl::frame,&process,[this](const QString& type,const QJsonObject& payload){
   if(payload["nonce"]!=nonce||payload["generation"]!="1"){failure="foreign helper response";return;}
   if(type=="ready")ready=true;
   if(type=="result"){++results;result=payload;}
   if(type=="cancelled")++cancellations;
   if(type=="error"){++errors;failure=payload["code"].toString();}
  });
  process.setProcessEnvironment(environment);process.start(PUTGD_REAL_CAPTURE_HELPER,{});if(!process.waitForStarted())return false;
  return control.send("bootstrap",{{"build",QJsonDocument::fromJson(buildManifestJson()).object()},{"nonce",nonce},{"generation","1"},{"target",frozen},{"cursor",QJsonObject{{"id","0"},{"cache",false}}},{"path",path},{"device",QString::number(qulonglong(identity.st_dev))},{"inode",QString::number(qulonglong(identity.st_ino))},{"limits",QJsonObject{{"imagePixels",10000000},{"imageBytes",40000000},{"maxFileBytes",40000000}}}});
 }
};
QList<xcb_window_t> selectorWindows(xcb_connection_t* connection,qint64 pid){
 QList<xcb_window_t> result;const auto root=xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root;
 const auto atom=x11::internAtom(connection,"_NET_WM_PID");auto tree=x11::reply(xcb_query_tree_reply(connection,xcb_query_tree(connection,root),nullptr));if(!tree)return result;
 for(int i=0;i<xcb_query_tree_children_length(tree.get());++i){
  const auto window=xcb_query_tree_children(tree.get())[i];auto attributes=x11::reply(xcb_get_window_attributes_reply(connection,xcb_get_window_attributes(connection,window),nullptr));if(!attributes||!attributes->override_redirect||attributes->map_state!=XCB_MAP_STATE_VIEWABLE)continue;
  auto property=x11::reply(xcb_get_property_reply(connection,xcb_get_property(connection,0,window,atom,XCB_ATOM_CARDINAL,0,1),nullptr));uint32_t owner=0;if(property&&property->format==32&&xcb_get_property_value_length(property.get())==4)std::memcpy(&owner,xcb_get_property_value(property.get()),4);
  if(owner==uint32_t(pid))result.append(window);
 }
 return result;
}
int pointerGrabStatus(xcb_connection_t* connection){
 const auto root=xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root;
 auto reply=x11::reply(xcb_grab_pointer_reply(connection,xcb_grab_pointer(connection,0,root,0,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC,XCB_WINDOW_NONE,XCB_CURSOR_NONE,XCB_CURRENT_TIME),nullptr));
 if(reply&&reply->status==XCB_GRAB_STATUS_SUCCESS)xcb_ungrab_pointer(connection,XCB_CURRENT_TIME);
 xcb_flush(connection);return reply?reply->status:-1;
}
bool sendButton(xcb_connection_t* connection,xcb_window_t window,QPoint point,bool down,int button=1){
 xcb_button_press_event_t event{};event.response_type=down?XCB_BUTTON_PRESS:XCB_BUTTON_RELEASE;event.detail=uint8_t(button);event.root=xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root;event.event=window;event.root_x=int16_t(point.x());event.root_y=int16_t(point.y());event.same_screen=true;
 auto error=x11::reply(xcb_request_check(connection,xcb_send_event_checked(connection,false,window,down?XCB_EVENT_MASK_BUTTON_PRESS:XCB_EVENT_MASK_BUTTON_RELEASE,reinterpret_cast<const char*>(&event))));xcb_flush(connection);return !error;
}
bool sendEscape(xcb_connection_t* connection,xcb_window_t window){
 auto* symbols=xcb_key_symbols_alloc(connection);if(!symbols)return false;auto* codes=xcb_key_symbols_get_keycode(symbols,0xff1b);
 if(!codes||!codes[0]){std::free(codes);xcb_key_symbols_free(symbols);return false;}
 xcb_key_press_event_t event{};event.response_type=XCB_KEY_PRESS;event.detail=codes[0];event.root=xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root;event.event=window;event.same_screen=true;
 std::free(codes);xcb_key_symbols_free(symbols);
 auto error=x11::reply(xcb_request_check(connection,xcb_send_event_checked(connection,false,window,XCB_EVENT_MASK_KEY_PRESS,reinterpret_cast<const char*>(&event))));xcb_flush(connection);return !error;
}
QByteArray wireFrame(const QString& type,const QJsonObject& payload,int sequence){return QJsonDocument(QJsonObject{{"version",1},{"sequence",QString::number(sequence)},{"type",type},{"payload",payload}}).toJson(QJsonDocument::Compact)+'\n';}
}
class CapturePreparationTest final:public QObject {
 Q_OBJECT
private slots:
 void cursorEnvironmentInvalidatesAndRestoresFrozenLayout_data(){
  QTest::addColumn<QByteArray>("variable");QTest::addColumn<bool>("emptyPresence");
  for(const auto* name:{"XCURSOR_THEME","XCURSOR_SIZE","XCURSOR_PATH","XCURSOR_RESIZED","XCURSOR_DISCOVER","XCURSOR_CORE"}){QTest::newRow(name)<<QByteArray(name)<<false;QTest::newRow((QByteArray(name)+"-set-empty").constData())<<QByteArray(name)<<true;}
 }
 void cursorEnvironmentInvalidatesAndRestoresFrozenLayout(){
  QFETCH(QByteArray,variable);QFETCH(bool,emptyPresence);const bool present=qEnvironmentVariableIsSet(variable.constData());const auto previous=qgetenv(variable.constData());
  const auto restore=qScopeGuard([&]{if(present)qputenv(variable.constData(),previous);else qunsetenv(variable.constData());});
  if(emptyPresence)qunsetenv(variable.constData());
  X11NativeSnapshot snapshot;QVERIFY(snapshot.available());Error error;const auto original=snapshot.freeze("region",{},error)["layout"].toObject();QVERIFY(!error);
  qputenv(variable.constData(),emptyPresence?QByteArray{}:previous+"-putgd-synthetic-change");const auto changed=snapshot.freeze("region",{},error)["layout"].toObject();QVERIFY(!error);QVERIFY(changed["scale"]!=original["scale"]);
  if(!emptyPresence&&present)qputenv(variable.constData(),previous);else qunsetenv(variable.constData());
  QCOMPARE(snapshot.freeze("region",{},error)["layout"].toObject(),original);QVERIFY(!error);
 }
 void adapterTeardownSuppressesDeferredSelection(){
  qputenv("XCURSOR_THEME","static");
  auto* connection=xcb_connect(nullptr,nullptr);QVERIFY(connection&&!xcb_connection_has_error(connection));const auto close=qScopeGuard([&]{xcb_disconnect(connection);});
  X11Capture capture;Error error;const auto target=capture.freeze("region",error);QVERIFY2(!error,qPrintable(error.code));
  int callbacks=0;auto source=std::make_unique<X11SourceAdapter>(nullptr,true);
  source->selectRegionFrozen(target,ResourceLimits{},[&](RegionSelectionResult){++callbacks;});
  PUTGD_TRY_VERIFY(!selectorWindows(connection,QCoreApplication::applicationPid()).isEmpty());PUTGD_TRY_COMPARE(pointerGrabStatus(connection),int(XCB_GRAB_STATUS_ALREADY_GRABBED));
  // Do not run the event loop between cancel and reset: this reproduces helper
  // EOF/rejection exiting before its selector's DeferredDelete is processed.
  source->cancelAcquisition();QCOMPARE(callbacks,0);source.reset();QCOMPARE(callbacks,0);
  QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);QCOMPARE(callbacks,0);
  QVERIFY(selectorWindows(connection,QCoreApplication::applicationPid()).isEmpty());QCOMPARE(pointerGrabStatus(connection),int(XCB_GRAB_STATUS_SUCCESS));
 }
 void earlySelectionWaitsForBegin_data(){
  QTest::addColumn<QByteArray>("theme");QTest::addColumn<QString>("ending");
  for(const auto& theme:{QByteArray("static"),QByteArray("animated"),QByteArray("oversized"),QByteArray("missing")})QTest::newRow(("select-before-begin-"+theme).constData())<<theme<<QString("begin-late");
  QTest::newRow("begin-before-selection")<<QByteArray("static")<<QString("begin-first");
  QTest::newRow("cancel-selected-before-begin")<<QByteArray("static")<<QString("cancel");
  QTest::newRow("buffered-begin-cancel-after-selection")<<QByteArray("static")<<QString("buffered-cancel");
  QTest::newRow("eof-selected-before-begin")<<QByteArray("static")<<QString("eof");
 }
 void earlySelectionWaitsForBegin(){
  QFETCH(QByteArray,theme);QFETCH(QString,ending);qputenv("XCURSOR_THEME",theme);
  auto* connection=xcb_connect(nullptr,nullptr);QVERIFY(connection&&!xcb_connection_has_error(connection));const auto close=qScopeGuard([&]{xcb_disconnect(connection);});
  const auto root=xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root;
  const auto background=xcb_generate_id(connection);const uint32_t red=0x00cc2244;
  auto create=x11::reply(xcb_request_check(connection,xcb_create_window_checked(connection,XCB_COPY_FROM_PARENT,background,root,300,200,240,180,0,XCB_WINDOW_CLASS_INPUT_OUTPUT,XCB_COPY_FROM_PARENT,XCB_CW_BACK_PIXEL,&red)));QVERIFY(!create);
  const auto remove=qScopeGuard([&]{xcb_destroy_window(connection,background);xcb_flush(connection);});
  xcb_map_window(connection,background);auto fence=x11::reply(xcb_get_input_focus_reply(connection,xcb_get_input_focus(connection),nullptr));QVERIFY(fence);
  HelperSession helper;QVERIFY2(helper.start(),qPrintable(helper.failure));PUTGD_TRY_VERIFY(helper.ready);
  QVERIFY(helper.control.send("preview",helper.command()));
  PUTGD_TRY_VERIFY(!selectorWindows(connection,helper.process.processId()).isEmpty());
  PUTGD_TRY_COMPARE(pointerGrabStatus(connection),int(XCB_GRAB_STATUS_ALREADY_GRABBED));
  QCOMPARE(helper.results,0);QCOMPARE(helper.errors,0);QCOMPARE(helper.artifact.size(),qint64(0));
  if(ending=="begin-first")QVERIFY(helper.control.send("begin",helper.command()));
  const auto window=selectorWindows(connection,helper.process.processId()).first();
  // These are explicit synthetic XCB events, not physical/compositor evidence.
  QVERIFY(sendButton(connection,window,{340,240},true));QVERIFY(sendButton(connection,window,{420,300},false));
  PUTGD_TRY_VERIFY(selectorWindows(connection,helper.process.processId()).isEmpty());
  PUTGD_TRY_COMPARE(pointerGrabStatus(connection),int(XCB_GRAB_STATUS_SUCCESS));
  if(ending!="begin-first"){
   putgd::test::settle(100);PUTGD_TRY_COMPARE(helper.process.state(),QProcess::Running);PUTGD_TRY_COMPARE(helper.results,0);PUTGD_TRY_COMPARE(helper.errors,0);PUTGD_TRY_COMPARE(helper.artifact.size(),qint64(0));
   // Painting changes after selection. A successful late BEGIN must acquire
   // this later frame, proving selection did not freeze/cache desktop pixels.
   const uint32_t blue=0x002266cc;xcb_change_window_attributes(connection,background,XCB_CW_BACK_PIXEL,&blue);xcb_clear_area(connection,false,background,0,0,0,0);fence=x11::reply(xcb_get_input_focus_reply(connection,xcb_get_input_focus(connection),nullptr));QVERIFY(fence);
  }
  if(ending=="cancel")QVERIFY(helper.control.send("cancel",helper.envelope()));
  else if(ending=="buffered-cancel"){
   const auto bytes=wireFrame("begin",helper.command(),3)+wireFrame("cancel",helper.envelope(),4);QCOMPARE(helper.process.write(bytes),qint64(bytes.size()));
  }else if(ending=="eof")helper.process.closeWriteChannel();
  else{
   if(ending=="begin-late")QVERIFY(helper.control.send("begin",helper.command()));
   PUTGD_TRY_COMPARE(helper.results,1);QCOMPARE(helper.errors,0);QCOMPARE(helper.cancellations,0);
   QImage image(helper.path);QVERIFY(!image.isNull());QCOMPARE(image.size(),QSize(80,60));
   const auto expected=ending=="begin-first"?QColor(0xcc,0x22,0x44):QColor(0x22,0x66,0xcc);
   for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)QCOMPARE(image.pixelColor(x,y),expected);
   QCOMPARE(helper.result["width"].toInt(),80);QCOMPARE(helper.result["height"].toInt(),60);QCOMPARE(helper.result["bytes"].toString().toLongLong(),helper.artifact.size());
   helper.artifact.seek(0);QCOMPARE(helper.result["sha256"].toString(),QString::fromLatin1(QCryptographicHash::hash(helper.artifact.readAll(),QCryptographicHash::Sha256).toHex()));
   QVERIFY(helper.control.send("accept",helper.envelope()));
  }
  PUTGD_TRY_COMPARE(helper.process.state(),QProcess::NotRunning);QCOMPARE(helper.process.exitStatus(),QProcess::NormalExit);QCOMPARE(helper.process.exitCode(),ending=="eof"?3:0);QVERIFY(!QFile::exists(helper.path));
  if(ending=="cancel"||ending=="buffered-cancel"||ending=="eof"){QCOMPARE(helper.results,0);QCOMPARE(helper.errors,0);QCOMPARE(helper.artifact.size(),qint64(0));QCOMPARE(helper.cancellations,ending=="eof"?0:1);}
 }
 void accessibilityStartsAfterTheSelectorMaps(){
  // QGuiApplication::exec() would start Qt's AT-SPI bridge before the helper
  // reads its bootstrap: 20-30 ms of synchronous D-Bus ahead of every selector
  // (#1063). AT_SPI_BUS_ADDRESS makes Qt enable the bridge and connect here.
  qputenv("XCURSOR_THEME","static");
  QTemporaryDir bus("/tmp/putgd-a11y-XXXXXX");QVERIFY(bus.isValid());
  QLocalServer server;QVERIFY(server.listen(bus.filePath("bus")));int connections=0;
  // Refusing the connection keeps the helper's D-Bus handshake short.
  connect(&server,&QLocalServer::newConnection,this,[&]{while(auto* socket=server.nextPendingConnection()){++connections;socket->abort();socket->deleteLater();}});
  auto* connection=xcb_connect(nullptr,nullptr);QVERIFY(connection&&!xcb_connection_has_error(connection));const auto close=qScopeGuard([&]{xcb_disconnect(connection);});
  HelperSession helper;helper.environment.insert("AT_SPI_BUS_ADDRESS","unix:path="+bus.filePath("bus"));
  QVERIFY2(helper.start(),qPrintable(helper.failure));PUTGD_TRY_VERIFY(helper.ready);
  server.waitForNewConnection(0);QCOMPARE(connections,0);
  QVERIFY(helper.control.send("preview",helper.command()));
  PUTGD_TRY_VERIFY(!selectorWindows(connection,helper.process.processId()).isEmpty());
  // Accessibility still starts once the selector is shown, without any input.
  PUTGD_TRY_VERIFY(connections>0);
  QVERIFY(helper.control.send("cancel",helper.envelope()));
  PUTGD_TRY_COMPARE(helper.process.state(),QProcess::NotRunning);QCOMPARE(helper.process.exitStatus(),QProcess::NormalExit);QCOMPARE(helper.process.exitCode(),0);
  QCOMPARE(helper.cancellations,1);QCOMPARE(helper.results,0);QCOMPARE(helper.errors,0);QCOMPARE(helper.artifact.size(),qint64(0));
 }
 void previewTermination_data(){
  QTest::addColumn<QString>("scenario");
  for(const auto& scenario:{"escape","cancel","eof","foreign","generation","duplicate","desktop","extra-key","cursor-mismatch","preview-after-begin"})QTest::newRow(scenario)<<QString::fromLatin1(scenario);
 }
 void previewTermination(){
  QFETCH(QString,scenario);qputenv("XCURSOR_THEME","static");
  auto* connection=xcb_connect(nullptr,nullptr);QVERIFY(connection&&!xcb_connection_has_error(connection));const auto close=qScopeGuard([&]{xcb_disconnect(connection);});
  HelperSession helper;QVERIFY2(helper.start(scenario=="desktop"?"desktop":"region"),qPrintable(helper.failure));PUTGD_TRY_VERIFY(helper.ready);
  if(scenario=="foreign"||scenario=="generation"||scenario=="extra-key"||scenario=="desktop"){
   auto command=helper.command();if(scenario=="foreign")command["nonce"]="foreign";if(scenario=="generation")command["generation"]="2";if(scenario=="extra-key")command["unexpected"]=true;
   // One batch: no valid following frame may enter acquisition after rejection.
   const auto bytes=wireFrame("preview",command,2)+wireFrame("begin",helper.command(),3);QCOMPARE(helper.process.write(bytes),qint64(bytes.size()));
  }else{
   QVERIFY(helper.control.send(scenario=="preview-after-begin"?"begin":"preview",helper.command()));
   PUTGD_TRY_VERIFY(!selectorWindows(connection,helper.process.processId()).isEmpty());PUTGD_TRY_COMPARE(pointerGrabStatus(connection),int(XCB_GRAB_STATUS_ALREADY_GRABBED));
   QCOMPARE(helper.results,0);QCOMPARE(helper.artifact.size(),qint64(0));
   if(scenario=="escape")QVERIFY(sendEscape(connection,selectorWindows(connection,helper.process.processId()).first()));
   else if(scenario=="cancel")QVERIFY(helper.control.send("cancel",helper.envelope()));
   else if(scenario=="eof")helper.process.closeWriteChannel();
   else if(scenario=="cursor-mismatch")QVERIFY(helper.control.send("begin",helper.command("1")));
   else QVERIFY(helper.control.send("preview",helper.command()));
  }
  PUTGD_TRY_COMPARE(helper.process.state(),QProcess::NotRunning);QCOMPARE(helper.process.exitStatus(),QProcess::NormalExit);QCOMPARE(helper.process.exitCode(),scenario=="eof"?3:scenario=="escape"||scenario=="cancel"?0:4);
  QCOMPARE(helper.results,0);QCOMPARE(helper.errors,0);QCOMPARE(helper.cancellations,scenario=="escape"||scenario=="cancel"?1:0);QCOMPARE(helper.artifact.size(),qint64(0));QVERIFY(!QFile::exists(helper.path));
  PUTGD_TRY_COMPARE(pointerGrabStatus(connection),int(XCB_GRAB_STATUS_SUCCESS));QVERIFY(selectorWindows(connection,helper.process.processId()).isEmpty());
 }
 void bootstrapRemainsInvisibleAndDoesNotOwnInput_data(){
  QTest::addColumn<bool>("badBegin");QTest::addColumn<bool>("cache");QTest::addColumn<QByteArray>("theme");QTest::addColumn<bool>("seedExpected");QTest::addColumn<bool>("resizeRequested");QTest::addColumn<bool>("resourceResize");
  QLibrary cursorLibrary(QStringLiteral("Xcursor"),1);QVERIFY(cursorLibrary.load());const bool resizeApi=cursorLibrary.resolve("XcursorGetResizable")!=nullptr;
  const bool oldLoader=QVersionNumber::fromString(QString::fromLatin1(qVersion()))<QVersionNumber(6,5,0);
  QTest::newRow("cancel-before-begin")<<false<<false<<QByteArray("static")<<false<<false<<false;
  QTest::newRow("cancel-before-begin-with-cursor-seed")<<false<<true<<QByteArray("static")<<(QVersionNumber::fromString(QString::fromLatin1(qVersion()))<QVersionNumber(6,5,0))<<false<<false;
  QTest::newRow("animated-cursor-falls-back")<<false<<true<<QByteArray("animated")<<false<<false<<false;
  QTest::newRow("oversized-cursor-falls-back")<<false<<true<<QByteArray("oversized")<<false<<false<<false;
  QTest::newRow("missing-cursor-falls-back")<<false<<true<<QByteArray("missing")<<false<<false<<false;
  QTest::newRow("resized-cursor-falls-back")<<false<<true<<QByteArray("static")<<false<<true<<false;
  QTest::newRow("resource-resized-cursor-policy")<<false<<true<<QByteArray("static")<<(oldLoader&&!resizeApi)<<false<<true;
  QTest::newRow("reject-foreign-begin")<<true<<false<<QByteArray("static")<<false<<false<<false;
 }
 void bootstrapRemainsInvisibleAndDoesNotOwnInput(){
  QFETCH(bool,badBegin);QFETCH(bool,cache);QFETCH(QByteArray,theme);QFETCH(bool,seedExpected);QFETCH(bool,resizeRequested);QFETCH(bool,resourceResize);
  const bool hadResize=qEnvironmentVariableIsSet("XCURSOR_RESIZED");const auto previousResize=qgetenv("XCURSOR_RESIZED");
  if(resizeRequested)qputenv("XCURSOR_RESIZED","1");else qunsetenv("XCURSOR_RESIZED");
  const auto restoreResize=qScopeGuard([&]{if(hadResize)qputenv("XCURSOR_RESIZED",previousResize);else qunsetenv("XCURSOR_RESIZED");});
  qputenv("XCURSOR_THEME",theme);
  auto* resources=resourceResize?xcb_connect(nullptr,nullptr):nullptr;
  xcb_atom_t oldResourceType=XCB_ATOM_NONE;uint8_t oldResourceFormat=0;QByteArray oldResourceBytes;bool resourceChanged=false;
  const auto restoreResourceState=[&]{
   if(!resourceChanged)return true;
   const auto cookie=oldResourceType==XCB_ATOM_NONE?xcb_delete_property_checked(resources,xcb_setup_roots_iterator(xcb_get_setup(resources)).data->root,XCB_ATOM_RESOURCE_MANAGER):
    xcb_change_property_checked(resources,XCB_PROP_MODE_REPLACE,xcb_setup_roots_iterator(xcb_get_setup(resources)).data->root,XCB_ATOM_RESOURCE_MANAGER,oldResourceType,oldResourceFormat,uint32_t(oldResourceBytes.size()/(oldResourceFormat/8)),oldResourceBytes.constData());
   auto error=x11::reply(xcb_request_check(resources,cookie));if(error)return false;resourceChanged=false;return true;
  };
  const auto restoreResource=qScopeGuard([&]{if(resources){restoreResourceState();xcb_disconnect(resources);}});
  if(resourceResize){
   QVERIFY(resources&&!xcb_connection_has_error(resources));const auto root=xcb_setup_roots_iterator(xcb_get_setup(resources)).data->root;
   auto previous=x11::reply(xcb_get_property_reply(resources,xcb_get_property(resources,0,root,XCB_ATOM_RESOURCE_MANAGER,XCB_GET_PROPERTY_TYPE_ANY,0,262144),nullptr));QVERIFY(previous);QCOMPARE(previous->bytes_after,uint32_t(0));
   oldResourceType=previous->type;oldResourceFormat=previous->format;oldResourceBytes=QByteArray(static_cast<const char*>(xcb_get_property_value(previous.get())),xcb_get_property_value_length(previous.get()));
   QVERIFY(oldResourceType==XCB_ATOM_NONE||(oldResourceType==XCB_ATOM_STRING&&oldResourceFormat==8));
   // A fresh helper Display reads this private server resource. Never export
   // existing resource bytes; preserve exact property identity for restoration.
   const auto replacement=oldResourceBytes+"\nXcursor.resized: true\n";
   auto error=x11::reply(xcb_request_check(resources,xcb_change_property_checked(resources,XCB_PROP_MODE_REPLACE,root,XCB_ATOM_RESOURCE_MANAGER,XCB_ATOM_STRING,8,uint32_t(replacement.size()),replacement.constData())));QVERIFY(!error);resourceChanged=true;
  }
  X11NativeSnapshot snapshot;QVERIFY(snapshot.available());Error error;
  const auto frozen=snapshot.freeze(badBegin?"desktop":"region",{},error);QVERIFY2(!error,qPrintable(error.code));
  QTemporaryDir directory("/tmp/putgd-capture-XXXXXX");QVERIFY(directory.isValid());
  QVERIFY(QFile::setPermissions(directory.path(),QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
  const auto path=directory.filePath("result.png");QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));QVERIFY(file.setPermissions(QFile::ReadOwner|QFile::WriteOwner));file.close();QVERIFY(file.open(QIODevice::ReadOnly));
  struct stat identity{};QVERIFY(!::lstat(QFile::encodeName(path).constData(),&identity));
  QProcess process;process.setProcessChannelMode(QProcess::SeparateChannels);process.setReadChannel(QProcess::StandardOutput);CaptureControl control(process);
  const auto stopHelper=qScopeGuard([&]{if(process.state()!=QProcess::NotRunning){process.kill();process.waitForFinished(2000);}});
  const auto nonce=QUuid::createUuid().toString(QUuid::WithoutBraces);bool ready=false;QJsonObject seed;int results=0,errors=0;
  connect(&control,&CaptureControl::frame,this,[&](const QString& type,const QJsonObject& payload){
   if(type=="ready"){QCOMPARE(payload["nonce"].toString(),nonce);QCOMPARE(payload["generation"].toString(),QString("1"));seed=payload["cursor"].toObject();ready=true;}
   if(type=="result")++results;
   if(type=="error"&&ready)++errors;
  });
  process.start(PUTGD_REAL_CAPTURE_HELPER,{});QVERIFY(process.waitForStarted());
  QVERIFY(control.send("bootstrap",{{"build",QJsonDocument::fromJson(buildManifestJson()).object()},{"nonce",nonce},{"generation","1"},{"target",frozen},{"cursor",QJsonObject{{"id","0"},{"cache",cache}}},{"path",path},{"device",QString::number(qulonglong(identity.st_dev))},{"inode",QString::number(qulonglong(identity.st_ino))},{"limits",QJsonObject{{"imagePixels",badBegin?1:10000000},{"imageBytes",badBegin?4:40000000},{"maxFileBytes",40000000}}}}));
  PUTGD_TRY_VERIFY(ready);putgd::test::settle(80);PUTGD_TRY_COMPARE(results,0);PUTGD_TRY_COMPARE(QFileInfo(path).size(),qint64(0));
  auto* connection=xcb_connect(nullptr,nullptr);QVERIFY(connection&&!xcb_connection_has_error(connection));
  const auto cleanup=qScopeGuard([&]{xcb_disconnect(connection);});const auto root=xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root;
  const auto pidAtom=x11::internAtom(connection,"_NET_WM_PID");auto tree=x11::reply(xcb_query_tree_reply(connection,xcb_query_tree(connection,root),nullptr));QVERIFY(tree);
  for(int i=0;i<xcb_query_tree_children_length(tree.get());++i){const auto window=xcb_query_tree_children(tree.get())[i];auto p=x11::reply(xcb_get_property_reply(connection,xcb_get_property(connection,0,window,pidAtom,XCB_ATOM_CARDINAL,0,1),nullptr));uint32_t pid=0;if(p&&p->format==32&&xcb_get_property_value_length(p.get())==4)std::memcpy(&pid,xcb_get_property_value(p.get()),4);if(pid!=uint32_t(process.processId()))continue;auto attributes=x11::reply(xcb_get_window_attributes_reply(connection,xcb_get_window_attributes(connection,window),nullptr));QVERIFY(attributes);QVERIFY(attributes->map_state!=XCB_MAP_STATE_VIEWABLE);}
  auto pointer=x11::reply(xcb_grab_pointer_reply(connection,xcb_grab_pointer(connection,0,root,0,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC,XCB_WINDOW_NONE,XCB_CURSOR_NONE,XCB_CURRENT_TIME),nullptr));QVERIFY(pointer);QCOMPARE(pointer->status,uint8_t(XCB_GRAB_STATUS_SUCCESS));xcb_ungrab_pointer(connection,XCB_CURRENT_TIME);
  auto keyboard=x11::reply(xcb_grab_keyboard_reply(connection,xcb_grab_keyboard(connection,0,root,XCB_CURRENT_TIME,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC),nullptr));QVERIFY(keyboard);QCOMPARE(keyboard->status,uint8_t(XCB_GRAB_STATUS_SUCCESS));xcb_ungrab_keyboard(connection,XCB_CURRENT_TIME);xcb_flush(connection);
  if(!seedExpected)QVERIFY(seed.isEmpty());
  if(seedExpected){
   auto fixes=x11::reply(xcb_xfixes_query_version_reply(connection,xcb_xfixes_query_version(connection,5,0),nullptr));QVERIFY(fixes&&fixes->major_version>=2);
   // Compare the actual helper's public Xcursor seed with Qt's native cursor,
   // rather than seeding the cache from the expected Qt image itself.
   QVERIFY2(!seed.isEmpty(),"The generated static theme must provide an ARGB crosshair");
   QWindow expected;expected.setGeometry(100,100,200,200);expected.setCursor(Qt::CrossCursor);expected.show();
   QVERIFY(putgd::test::waitForWindowExposed(&expected));
   xcb_warp_pointer(connection,XCB_WINDOW_NONE,root,0,0,0,0,150,150);xcb_flush(connection);
   auto image=x11::reply(xcb_xfixes_get_cursor_image_reply(connection,xcb_xfixes_get_cursor_image(connection),nullptr));QVERIFY(image);
   QCOMPARE(seed["width"].toInt(),int(image->width));QCOMPARE(seed["height"].toInt(),int(image->height));
   QCOMPARE(seed["xhot"].toInt(),int(image->xhot));QCOMPARE(seed["yhot"].toInt(),int(image->yhot));
   const auto actual=QByteArray::fromBase64(seed["pixels"].toString().toLatin1());
   QCOMPARE(actual.size(),qsizetype(image->width)*image->height*4);
   const auto* pixels=xcb_xfixes_get_cursor_image_cursor_image(image.get());
   for(qsizetype i=0;i<qsizetype(image->width)*image->height;++i)QCOMPARE(qFromBigEndian<quint32>(actual.constData()+i*4),pixels[i]);
   expected.hide();
  }
  if(badBegin){
   // A valid second begin would synchronously emit image_limit with this row
   // budget, providing an observable acquisition-admission boundary.
   // One pipe write: rejection of the first buffered frame must prevent the
   // decoder from delivering the otherwise valid second begin command.
   const auto frame=[&](const QString& identity,int sequence){return QJsonDocument(QJsonObject{{"version",1},{"sequence",QString::number(sequence)},{"type","begin"},{"payload",QJsonObject{{"nonce",identity},{"generation","1"},{"cursor","0"}}}}).toJson(QJsonDocument::Compact)+'\n';};
   const auto bytes=frame("foreign",2)+frame(nonce,3);QCOMPARE(process.write(bytes),qint64(bytes.size()));
  }else QVERIFY(control.send("cancel",{{"nonce",nonce},{"generation","1"}}));
  PUTGD_TRY_COMPARE(process.state(),QProcess::NotRunning);QCOMPARE(process.exitStatus(),QProcess::NormalExit);QCOMPARE(process.exitCode(),badBegin?4:0);QVERIFY(!QFile::exists(path));QCOMPARE(results,0);QCOMPARE(errors,0);
  // The open descriptor survives helper unlink. A desktop acquisition triggered
  // by the buffered valid begin could leave encoded bytes on this inode.
  // Zero bytes alone cannot prove no earlier screen read; decoder-stop coverage
  // separately verifies that no following buffered command is delivered.
  QCOMPARE(file.size(),qint64(0));QVERIFY(restoreResourceState());
 }
};
int main(int argc,char** argv){
 // Pin public Xcursor inputs before Qt opens its Display. No guest/system
 // theme installation is needed; all helper processes inherit this private path.
 QTemporaryDir themes("/tmp/putgd-cursor-theme-XXXXXX");if(!themes.isValid())return 2;
 for(const auto& name:{QByteArray("static"),QByteArray("animated"),QByteArray("oversized")}){
  const auto path=themes.filePath(QString::fromLatin1(name)+"/cursors");if(!QDir().mkpath(path))return 2;
  const int count=name=="animated"?2:1,width=name=="oversized"?65:5,height=7;
  auto* images=XcursorImagesCreate(count);if(!images)return 2;
  for(int frame=0;frame<count;++frame){
   auto* image=XcursorImageCreate(width,height);if(!image){XcursorImagesDestroy(images);return 2;}
   image->size=24;image->xhot=2;image->yhot=3;image->delay=40;
   for(int y=0;y<height;++y)for(int x=0;x<width;++x)image->pixels[y*width+x]=(x==2||y==3)?(frame?0xffcc8844u:0xff4466ccu):0;
   images->images[frame]=image;++images->nimage;
  }
  const bool saved=XcursorFilenameSaveImages(QFile::encodeName(path+"/cross").constData(),images);XcursorImagesDestroy(images);if(!saved)return 2;
 }
 qputenv("XCURSOR_PATH",QFile::encodeName(themes.path()));qputenv("XCURSOR_THEME","static");qputenv("XCURSOR_SIZE","24");
 QGuiApplication::setDesktopSettingsAware(false);qunsetenv("QT_QPA_PLATFORMTHEME");qunsetenv("QT_STYLE_OVERRIDE");
 QGuiApplication app(argc,argv);CapturePreparationTest test;return QTest::qExec(&test,argc,argv);
}
#include "test_capture_preparation.moc"
