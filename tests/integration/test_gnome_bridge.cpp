#include "platform/gnome/GnomeProtocol.h"
#include "platform/gnome/GnomeAdapter.h"
#include <QTest>
#include <QSignalSpy>
#include <QBuffer>
#include <QImage>
#include <QImageReader>
#include <QTemporaryDir>
#include <QFile>
#include <QUrl>
#include <QJsonArray>
#include <QJsonDocument>
#include <QGuiApplication>
#include <QDBusMessage>
#include <QDBusContext>
#include <QDBusConnectionInterface>
#include <QScopeGuard>
#include <QProcess>
#include <QUuid>
#include <cstdio>
#include "fixtures/Waits.h"
using namespace putgd;
class GnomeBridgeTest final:public QObject {
 Q_OBJECT
 QJsonObject meta(const QByteArray& bytes,const QString& mime="text/plain;charset=utf-8") {
  return {{"version",1},{"requestId","synthetic-request"},{"mime",mime},{"bytes",bytes.size()}};
 }
private slots:
 void plainTextUrlsRemainPayloadBytes() {
  const QByteArray bytes="https://example.invalid/synthetic";
  auto result=gnome::decodeTransfer(meta(bytes),bytes,"synthetic-request",{});
  QVERIFY2(!result.error,qPrintable(result.error.message));
  QCOMPARE(result.payload.kind,QString("text"));QCOMPARE(result.payload.bytes,bytes);
 }
 void forgedStaleAndLengthMismatchAreRejected() {
  const QByteArray bytes="synthetic";
  QCOMPARE(gnome::decodeTransfer(meta(bytes),bytes,"another-request",{}).error.code,QString("bridge_protocol"));
  auto metadata=meta(bytes);metadata["bytes"]=bytes.size()+1;
  QCOMPARE(gnome::decodeTransfer(metadata,bytes,"synthetic-request",{}).error.code,QString("bridge_protocol"));
  metadata=meta(bytes);metadata["version"]=2;
  QCOMPARE(gnome::decodeTransfer(metadata,bytes,"synthetic-request",{}).error.code,QString("bridge_protocol"));
 }
 void boundedTextAndImageBeforeDecode() {
  ResourceLimits limits;limits.clipboardTextBytes=4;const QByteArray text="oversized";
  QCOMPARE(gnome::decodeTransfer(meta(text),text,"synthetic-request",limits).error.code,QString("input_excessive"));
  QImage image(16,8,QImage::Format_ARGB32);image.fill(Qt::green);QByteArray png;QBuffer buffer(&png);buffer.open(QIODevice::WriteOnly);QVERIFY(image.save(&buffer,"PNG"));
  limits.imagePixels=100;
  QCOMPARE(gnome::decodeTransfer(meta(png,"image/png"),png,"synthetic-request",limits).error.code,QString("input_excessive"));
  limits.imagePixels=200;auto result=gnome::decodeTransfer(meta(png,"image/png"),png,"synthetic-request",limits);
  QVERIFY(!result.error);QCOMPARE(result.payload.image.size(),image.size());QCOMPARE(result.payload.image.pixelColor(4,3),QColor(Qt::green));
 }
 void orderedLocalUrisAndForeignRejection() {
  QTemporaryDir dir;QStringList expected{dir.filePath("a synthetic.txt"),dir.filePath("b synthetic.txt")};
  QByteArray bytes;for(const auto& path:expected){QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write("synthetic");bytes+=QUrl::fromLocalFile(path).toEncoded()+"\r\n";}
  auto result=gnome::decodeTransfer(meta(bytes,"text/uri-list"),bytes,"synthetic-request",{});
  QVERIFY(!result.error);QCOMPARE(result.payload.kind,QString("files"));QCOMPARE(result.payload.files,expected);
  bytes+="https://example.invalid/remote\r\n";
  QCOMPARE(gnome::decodeTransfer(meta(bytes,"text/uri-list"),bytes,"synthetic-request",{}).error.code,QString("source_unsupported"));
 }
 void captureFramesComposeGapsAndRejectMalformedGeometry() {
  QImage image(8,4,QImage::Format_ARGB32);image.fill(Qt::red);QByteArray png;QBuffer buffer(&png);buffer.open(QIODevice::WriteOnly);QVERIFY(image.save(&buffer,"PNG"));
  const QByteArray bytes=png+png;
  QJsonArray frames;
  frames.append(QJsonObject{{"offset",0},{"length",png.size()},{"identity","left"},{"logical",QJsonArray{-4,0,4,2}},{"native",QJsonArray{0,0,8,4}},{"density",2},{"rotation",0}});
  frames.append(QJsonObject{{"offset",png.size()},{"length",png.size()},{"identity","right"},{"logical",QJsonArray{2,0,4,2}},{"native",QJsonArray{0,0,8,4}},{"density",2},{"rotation",0}});
  auto metadata=meta(bytes,"application/x-putgd-capture");metadata["frames"]=frames;metadata["selection"]=QJsonArray{-4,0,10,2};
  auto result=gnome::decodeTransfer(metadata,bytes,"synthetic-request",{});
  QVERIFY2(!result.error,qPrintable(result.error.message));QCOMPARE(result.payload.image.size(),QSize(20,4));
  QCOMPARE(result.payload.image.pixelColor(2,2),QColor(Qt::red));QCOMPARE(result.payload.image.pixelColor(9,2).alpha(),0);
  auto frame=frames[0].toObject();frame["length"]=bytes.size()+1;frames[0]=frame;metadata["frames"]=frames;
  QCOMPARE(gnome::decodeTransfer(metadata,bytes,"synthetic-request",{}).error.code,QString("bridge_protocol"));
 }
 void malformedImageAndUnsupportedMimeAreRejected() {
  const QByteArray bytes="synthetic invalid image";
  QCOMPARE(gnome::decodeTransfer(meta(bytes,"image/png"),bytes,"synthetic-request",{}).error.code,QString("source_unsupported"));
  QCOMPARE(gnome::decodeTransfer(meta(bytes,"application/octet-stream"),bytes,"synthetic-request",{}).error.code,QString("source_unsupported"));
 }
};

// The graphical mode uses a real native Qt Wayland connection but no windows.
// The Core mode uses a synthetic GNOME session environment and no display.
// The separate, private-bus peer is synthetic: this proves the resident bridge
// state machine and D-Bus transport, never GNOME Shell capture acceptance.
class GnomeHelperFixture final:public QObject,protected QDBusContext {
 Q_OBJECT
 Q_CLASSINFO("D-Bus Interface","org.putgd.Gnome50")
 uint currentPid_=0,failPid_=0,failures_=0;
 QJsonArray registrations_,lastAcquireMimes_;QString lastOutcome_,lastRegionMode_;int acquires_=0;
 int hellos_=0,binds_=0,commits_=0;
 QJsonObject lastBindings_;
 QString helloError_=qEnvironmentVariable("PUTGD_TEST_HELLO_ERROR"),prepareError_=qEnvironmentVariable("PUTGD_TEST_PREPARE_ERROR");
 QString copyId_,copyMime_;QByteArray copyBytes_;qulonglong copySize_=0;
 static QString json(const QJsonObject& value){return QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact));}
public slots:
 // PUTGD_TEST_EXTENSION_VERSION plays an older extension still loaded after an upgrade (#131).
 QString Hello(){++hellos_;
  if(helloError_=="malformed")return "not-json";
  if(helloError_=="incompatible")return json({{"version",99},{"shellMajor",50}});
  if(!helloError_.isEmpty()){sendErrorReply(helloError_,"Synthetic bridge failure");return {};}
  return json({{"version",1},{"shellMajor",50},{"uiProcessVersion",1},{"clipboardRevision",1},{"extensionVersion",qEnvironmentVariableIntValue("PUTGD_TEST_EXTENSION_VERSION")?qEnvironmentVariableIntValue("PUTGD_TEST_EXTENSION_VERSION"):putgd::gnome::extensionVersion}});}
 QString SetUiProcess(uint pid){
  registrations_.append(int(pid));
  if(pid==failPid_&&failures_){--failures_;return json({{"error",QJsonObject{{"code","helper_unavailable"},{"message","Synthetic registration failure"},{"details",QJsonObject{}}}}});}
  currentPid_=pid;return json({});
 }
 QString Bind(const QString& encoded){
  ++binds_;lastBindings_=QJsonDocument::fromJson(encoded.toUtf8()).object();QJsonObject bindings;
  for(auto i=lastBindings_.begin();i!=lastBindings_.end();++i){const auto entry=i.value().toObject();bindings[i.key()]=QJsonObject{{"requested",entry["requested"]},{"bound",!entry["accelerator"].toString().isEmpty()}};}
  return json({{"bindings",bindings}});
 }
 QString BeginCopy(const QString& id,const QString& mime,qulonglong size){copyId_=id;copyMime_=mime;copySize_=size;copyBytes_.clear();return json({});}
 QString WriteCopy(const QString& id,qulonglong offset,const QString& data){
  if(id!=copyId_||offset!=qulonglong(copyBytes_.size()))return json({{"error",QJsonObject{{"code","bridge_protocol"},{"message","Synthetic copy identity mismatch"}}}});
  copyBytes_+=QByteArray::fromBase64(data.toLatin1());return json({});
 }
 QString CommitCopy(const QString& id){
  if(id!=copyId_||qulonglong(copyBytes_.size())!=copySize_)return json({{"error",QJsonObject{{"code","bridge_protocol"},{"message","Synthetic copy size mismatch"}}}});
  ++commits_;return json({{"clipboardRevision",2}});
 }
 void SetErrors(const QString& hello,const QString& prepare){helloError_=hello;prepareError_=prepare;}
 QString Prepare(const QString& id,const QString&){if(!prepareError_.isEmpty()){sendErrorReply(prepareError_,"Synthetic preparation failure");return {};}return json({{"version",1},{"requestId",id}});}
 QString Acquire(const QString& id,const QString& encoded){
  const auto bounds=QJsonDocument::fromJson(encoded.toUtf8()).object();++acquires_;lastOutcome_=bounds["outcome"].toString();lastRegionMode_=bounds["regionMode"].toString();const auto mimes=bounds["imageMimes"].toArray();lastAcquireMimes_=mimes;
  for(const auto& mime:mimes)
   if(!mime.isString()||!mime.toString().startsWith("image/"))
    return json({{"error",QJsonObject{{"code","bridge_protocol"},{"message","Invalid supported image MIME list"}}}});
  return json({{"version",1},{"requestId",id},{"mime","text/plain;charset=utf-8"},{"bytes",1}});
 }
 QString Read(const QString& id,qulonglong offset,uint){return json({{"version",1},{"requestId",id},{"offset",double(offset)},{"data","eA=="}});}
 QString Release(const QString&){return json({});}
 void FailRegistration(uint pid,uint count){failPid_=pid;failures_=count;}
 QString State(){return json({{"outcome",lastOutcome_},{"regionMode",lastRegionMode_},{"acquires",acquires_},{"imageMimes",lastAcquireMimes_},{"currentPid",int(currentPid_)},{"registrations",registrations_},{"hellos",hellos_},{"binds",binds_},{"lastBindings",lastBindings_},{"copyMime",copyMime_},{"copyData",QString::fromLatin1(copyBytes_.toBase64())},{"commits",commits_}});}
 void TriggerWindow(const QString& ticket){emit Action("window",ticket);}
 signals:
 void Action(const QString& action,const QString& ticket);
};
class GnomeAdapterTransportTest final:public QObject {
 Q_OBJECT
 QProcess fixture_;
 bool coreMode_;
public:
 explicit GnomeAdapterTransportTest(bool coreMode=false):coreMode_(coreMode){}
private:
 QDBusMessage control(const QString& method,const QVariantList& args={}){
  auto request=QDBusMessage::createMethodCall("org.gnome.Shell","/org/gnome/Shell/Extensions/Putgd","org.putgd.Gnome50",method);request.setArguments(args);
  return QDBusConnection::sessionBus().call(request,QDBus::Block,2000);
 }
 QJsonObject state(){return QJsonDocument::fromJson(control("State").arguments().value(0).toString().toUtf8()).object();}
 bool startFixture(int extensionVersion=0,const QString& helloError={}){
  auto environment=QProcessEnvironment::systemEnvironment();
  if(extensionVersion)environment.insert("PUTGD_TEST_EXTENSION_VERSION",QString::number(extensionVersion));else environment.remove("PUTGD_TEST_EXTENSION_VERSION");
  environment.insert("PUTGD_TEST_HELLO_ERROR",helloError);
  fixture_.setProcessEnvironment(environment);
  fixture_.start(QCoreApplication::applicationFilePath(),{"--gnome-helper-fixture"});
  if(!fixture_.waitForStarted(3000)||!fixture_.waitForReadyRead(3000))return false;
  return fixture_.readLine()=="ready\n";
 }
 bool windowAvailable(const GnomeAdapter& source){return source.capabilities()["capture"].toObject()["window"].toObject()["available"].toBool();}
 bool helperAvailable(const GnomeAdapter& source){return source.capabilities()["helper"].toObject()["available"].toBool();}
 void stopFixture(){if(fixture_.state()!=QProcess::NotRunning){fixture_.terminate();if(!fixture_.waitForFinished(3000)){fixture_.kill();fixture_.waitForFinished(3000);}}}
private slots:
 void initTestCase(){
  QCoreApplication::addLibraryPath(QStringLiteral(PUTGD_GNOME_MIME_FIXTURE_DIR));
  // Clipboard image types are listed by the transient image job, a child process.
  qputenv("QT_PLUGIN_PATH",PUTGD_GNOME_MIME_FIXTURE_DIR);
  QVERIFY(QImageReader::supportedMimeTypes().contains("application/x-putgd-gnome-mime"));
  QVERIFY(QImageReader::supportedMimeTypes().contains("image/x-putgd-gnome-mime"));
  if(coreMode_){
   QVERIFY(!qobject_cast<QGuiApplication*>(QCoreApplication::instance()));
   QCOMPARE(qEnvironmentVariable("XDG_SESSION_TYPE"),QString("wayland"));
   QCOMPARE(qEnvironmentVariable("XDG_CURRENT_DESKTOP"),QString("GNOME"));
   QVERIFY(!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"));
  }else{
   QVERIFY2(QGuiApplication::platformName().startsWith("wayland"),"--adapter-transport requires native Qt Wayland and an isolated dbus-run-session");
  }
  QVERIFY(QDBusConnection::sessionBus().isConnected());
 }
 void init(){QVERIFY2(startFixture(),fixture_.readAllStandardError().constData());}
 void cleanup(){stopFixture();}
 void missingEndpointGuidancePreservesOtherFailures_data(){
  QTest::addColumn<QString>("failure");QTest::addColumn<QString>("code");QTest::addColumn<bool>("guidance");
  for(const char* name:{"UnknownMethod","UnknownObject","UnknownInterface"})QTest::newRow(name)<<QString("org.freedesktop.DBus.Error.")+name<<QString("helper_unavailable")<<true;
  for(const char* name:{"AccessDenied","NoReply"})QTest::newRow(name)<<QString("org.freedesktop.DBus.Error.")+name<<QString("helper_unavailable")<<false;
  QTest::newRow("malformed")<<QString("malformed")<<QString("bridge_protocol")<<false;
  QTest::newRow("incompatible")<<QString("incompatible")<<QString("helper_incompatible")<<false;
 }
 void missingEndpointGuidancePreservesOtherFailures(){
  QFETCH(QString,failure);QFETCH(QString,code);QFETCH(bool,guidance);stopFixture();QVERIFY(startFixture(0,failure));
  GnomeAdapter source;PUTGD_TRY_VERIFY(!source.capabilities()["helper"].toObject()["error"].toObject()["code"].toString().isEmpty());
  PayloadResult result;bool done=false;source.acquire("desktop",{},[&](PayloadResult value){result=std::move(value);done=true;});QVERIFY(done);QCOMPARE(result.error.code,code);
  QCOMPARE(result.error.message.contains("gnome-extensions enable putgd@put.gd"),guidance);
  if(guidance){QVERIFY(result.error.message.contains("install"));QVERIFY(result.error.message.contains("log out and back in"));}
  if(failure.startsWith("org."))QCOMPARE(result.error.details["dbusError"].toString(),failure);
 }
 void endpointDisappearingAfterDiscoveryGetsGuidanceButMissingMethodsDoNot(){
  GnomeAdapter source;PUTGD_TRY_VERIFY(helperAvailable(source));const QString missing="org.freedesktop.DBus.Error.UnknownMethod";
  // A missing Prepare on a live bridge is a different failure from a disabled extension.
  control("SetErrors",{QString{},missing});PayloadResult result;bool done=false;
  source.acquire("desktop",{},[&](PayloadResult value){result=std::move(value);done=true;});QVERIFY(done);
  QCOMPARE(result.error.details["dbusError"].toString(),missing);QVERIFY(!result.error.message.contains("gnome-extensions enable"));
  control("SetErrors",{missing,missing});done=false;
  source.acquire("desktop",{},[&](PayloadResult value){result=std::move(value);done=true;});QVERIFY(done);
  QVERIFY(result.error.message.contains("gnome-extensions enable putgd@put.gd"));
  control("SetErrors",{QString{},QString{}});done=false;
  source.acquire("desktop",{},[&](PayloadResult value){result=std::move(value);done=true;});PUTGD_TRY_VERIFY(done);QVERIFY2(!result.error,qPrintable(result.error.message));
 }
 void nonImageReaderMetadataCannotBreakClipboardAcquisition(){
  GnomeAdapter source;PUTGD_TRY_VERIFY(helperAvailable(source));
  bool completed=false;PayloadResult result;
  source.acquire("clipboard",{},[&](PayloadResult value){result=std::move(value);completed=true;});
  PUTGD_TRY_VERIFY(completed);QVERIFY2(!result.error,qPrintable(result.error.message));
  QCOMPARE(result.payload.kind,QString("text"));QCOMPARE(result.payload.bytes,QByteArray("x"));
  const auto mimes=state()["imageMimes"].toArray();
  QVERIFY(mimes.contains("image/x-putgd-gnome-mime"));
  QVERIFY(!mimes.contains("application/x-putgd-gnome-mime"));
 }
 void outcomeIsFrozenWithThePreparedCapture(){
  GnomeAdapter source;PUTGD_TRY_VERIFY(helperAvailable(source));
  source.setCaptureOutcome("Uploads right away to Proton Drive");
  auto prepared=source.prepareAcquisition("region");
  source.setCaptureOutcome("Opens the image editor");
  bool completed=false;prepared({},[&](PayloadResult){completed=true;});PUTGD_TRY_VERIFY(completed);
  QCOMPARE(state()["outcome"].toString(),QString("Uploads right away to Proton Drive"));
 }
 void screenshotFirstTravelsWithRegionCapturesOnly(){
  // #629: a current extension gets regionMode only for region captures.
  GnomeAdapter source;PUTGD_TRY_VERIFY(helperAvailable(source));source.setRegionScreenshotFirst(true);
  for(const auto& [action,expected]:QList<QPair<QString,QString>>{{"region","screenshot_first"},{"desktop",QString()}}){
   bool completed=false;source.acquire(action,{},[&](PayloadResult value){QVERIFY(value.error.code!="helper_outdated");completed=true;});PUTGD_TRY_VERIFY(completed);
   QCOMPARE(state()["regionMode"].toString(),expected);
  }
 }
 void screenCapturesSendOnlyPngWithoutListingImagePlugins(){
  // Listing image types instantiates every installed Qt image plugin; only a
  // clipboard import needs them, and the image job lists them out of process.
  GnomeAdapter source;PUTGD_TRY_VERIFY(helperAvailable(source));
  for(const char* action:{"region","desktop"}){
   bool completed=false;source.acquire(action,{},[&](PayloadResult){completed=true;});PUTGD_TRY_VERIFY(completed);
   QCOMPARE(state()["imageMimes"].toArray(),QJsonArray{"image/png"});
  }
  bool completed=false;source.acquire("clipboard",{},[&](PayloadResult){completed=true;});PUTGD_TRY_VERIFY(completed);
  const auto mimes=state()["imageMimes"].toArray();QCOMPARE(mimes.first().toString(),QString("image/png"));QVERIFY(mimes.contains("image/x-putgd-gnome-mime"));
 }
 void clipboardAndShortcutTransport(){
  GnomeAdapter source;PUTGD_TRY_VERIFY(helperAvailable(source));
  source.bindShortcuts({{"region","Ctrl+Shift+1"},{"clipboard","Ctrl+Alt+V"}});
  PUTGD_TRY_COMPARE(state()["lastBindings"].toObject()["region"].toObject()["accelerator"].toString(),QString("<Control><Shift>1"));
  QCOMPARE(state()["lastBindings"].toObject()["clipboard"].toObject()["accelerator"].toString(),QString("<Control><Alt>v"));
  PUTGD_TRY_VERIFY(source.capabilities()["shortcuts"].toObject()["bindings"].toObject()["region"].toObject()["bound"].toBool());
  bool acquired=false;PayloadResult acquiredResult;
  source.acquire("clipboard",{},[&](PayloadResult value){acquiredResult=std::move(value);acquired=true;});
  PUTGD_TRY_VERIFY(acquired);QVERIFY2(!acquiredResult.error,qPrintable(acquiredResult.error.message));QCOMPARE(acquiredResult.payload.bytes,QByteArray("x"));
  CopyPayload payload;payload.kind="text";payload.text=QString(gnome::chunkBytes+17,QChar('x'))+QString::fromUtf8("\xc3\xa9");
  bool copied=false;Error copyError;source.copy(payload,[&](Error error){copyError=error;copied=true;});
  PUTGD_TRY_VERIFY(copied);QVERIFY2(!copyError,qPrintable(copyError.message));
  const auto copiedState=state();QCOMPARE(copiedState["copyMime"].toString(),QString("text/plain;charset=utf-8"));
  QCOMPARE(QByteArray::fromBase64(copiedState["copyData"].toString().toLatin1()),payload.text.toUtf8());QCOMPARE(copiedState["commits"].toInt(),1);
 }
 void unsupportedCoreSessionCannotClaimClientName(){
  if(!coreMode_)QSKIP("Core routing guard; graphical mode retains its native Qt platform check");
  const auto previous=qgetenv("XDG_CURRENT_DESKTOP");const auto restore=qScopeGuard([&]{qputenv("XDG_CURRENT_DESKTOP",previous);});
  qputenv("XDG_CURRENT_DESKTOP","KDE");GnomeAdapter source;
  QVERIFY(!helperAvailable(source));QCOMPARE(source.capabilities()["helper"].toObject()["error"].toObject()["code"].toString(),QString("session_unsupported"));
  QVERIFY(!QDBusConnection::sessionBus().interface()->isServiceRegistered("org.putgd.App.Gnome50").value());
 }
 void recordingSuspendsNativeBindingsAcrossSettingsAndOwnerChanges(){
  GnomeAdapter source;PUTGD_TRY_VERIFY(windowAvailable(source));source.bindShortcuts({{"window","Ctrl+1"}});
  PUTGD_TRY_VERIFY(!state()["lastBindings"].toObject()["window"].toObject()["accelerator"].toString().isEmpty());
  bool done=false;Error error;source.setShortcutRecording(true,[&](Error value){error=value;done=true;});QVERIFY(!done);PUTGD_TRY_VERIFY(done);QVERIFY2(!error,qPrintable(error.code));
  QCOMPARE(state()["lastBindings"].toObject()["window"].toObject()["accelerator"].toString(),QString{});
  QSignalSpy invoked(&source,&GnomeAdapter::actionRequested);control("TriggerWindow",{QUuid::createUuid().toString(QUuid::WithoutBraces)});putgd::test::settle(50);PUTGD_TRY_COMPARE(invoked.size(),0);
  source.bindShortcuts({{"window","Ctrl+2"}});putgd::test::settle(50);PUTGD_TRY_COMPARE(state()["lastBindings"].toObject()["window"].toObject()["accelerator"].toString(),QString{});
  stopFixture();PUTGD_TRY_VERIFY(!helperAvailable(source));QVERIFY(startFixture());PUTGD_TRY_VERIFY(windowAvailable(source));PUTGD_TRY_VERIFY(state()["binds"].toInt()>0);
  QCOMPARE(state()["lastBindings"].toObject()["window"].toObject()["accelerator"].toString(),QString{});
  done=false;source.setShortcutRecording(false,[&](Error value){error=value;done=true;});PUTGD_TRY_VERIFY(done);QVERIFY(!error);
  QCOMPARE(state()["lastBindings"].toObject()["window"].toObject()["accelerator"].toString(),QString("<Control>2"));
 }
 void clearFailureRecoversWithoutUiReopen(){
  GnomeAdapter source;PUTGD_TRY_VERIFY(windowAvailable(source));source.setUiProcess(1234);
  QCOMPARE(control("FailRegistration",{uint(0),uint(1)}).type(),QDBusMessage::ReplyMessage);source.setUiProcess(0);
  QVERIFY(helperAvailable(source));QVERIFY(!windowAvailable(source));QVERIFY(source.capabilities()["capture"].toObject()["region"].toObject()["available"].toBool());
  bool rejected=false;source.acquire("window",{},[&](PayloadResult result){rejected=true;QCOMPARE(result.error.code,QString("helper_unavailable"));});QVERIFY(rejected);
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(windowAvailable(source),5000);
  auto recovered=state();QCOMPARE(recovered["currentPid"].toInt(),0);QCOMPARE(recovered["registrations"].toArray(),(QJsonArray{0,1234,0,0}));QCOMPARE(recovered["hellos"].toInt(),1);QCOMPARE(recovered["binds"].toInt(),1);
  bool completed=false;PayloadResult result;
  connect(&source,&GnomeAdapter::actionRequested,this,[&](const QString& action){source.acquire(action,{},[&](PayloadResult value){result=std::move(value);completed=true;});});
  QCOMPARE(control("TriggerWindow",{QUuid::createUuid().toString(QUuid::WithoutBraces)}).type(),QDBusMessage::ReplyMessage);PUTGD_TRY_VERIFY(completed);
  QVERIFY2(!result.error,qPrintable(result.error.message));QCOMPARE(result.payload.bytes,QByteArray("x"));
 }
 void liveFailureStaysClosedAndNewPidSupersedesClear(){
  GnomeAdapter source;PUTGD_TRY_VERIFY(windowAvailable(source));
  QCOMPARE(control("FailRegistration",{uint(1234),uint(1)}).type(),QDBusMessage::ReplyMessage);source.setUiProcess(1234);QVERIFY(!windowAvailable(source));
  putgd::test::settle(3300);PUTGD_TRY_VERIFY(!windowAvailable(source));PUTGD_TRY_COMPARE(state()["registrations"].toArray(),(QJsonArray{0,1234}));
  PUTGD_TRY_COMPARE(control("FailRegistration",{uint(0),uint(1)}).type(),QDBusMessage::ReplyMessage);source.setUiProcess(0);QVERIFY(!windowAvailable(source));
  source.setUiProcess(5678);QVERIFY(windowAvailable(source));putgd::test::settle(3300);
  PUTGD_TRY_COMPARE(state()["currentPid"].toInt(),5678);PUTGD_TRY_COMPARE(state()["registrations"].toArray(),(QJsonArray{0,1234,0,5678}));
 }
 void outdatedExtensionAsksForALogOut(){
  // The package was upgraded while version 1 (no recording protocol) stays loaded.
  stopFixture();QVERIFY(startFixture(1));
  GnomeAdapter source;PUTGD_TRY_VERIFY(helperAvailable(source));
  const auto helper=source.capabilities()["helper"].toObject();
  QVERIFY(helper["outdated"].toBool());QCOMPARE(helper["packagedExtensionVersion"].toInt(),putgd::gnome::extensionVersion);
  QVERIFY(!source.selectsRegions());
  Error error;bool done=false;source.selectRegion([&](Error value,QJsonObject){error=value;done=true;});QVERIFY(done);
  QCOMPARE(error.code,QString("helper_outdated"));QVERIFY2(error.message.contains("log out and back in"),qPrintable(error.message));
  // Screenshots keep working with the loaded extension.
  QVERIFY(source.capabilities()["capture"].toObject()["region"].toObject()["available"].toBool());
  QCOMPARE(source.regionError().code,QString("helper_outdated"));
  // #629: the loaded extension ignores regionMode, so Screenshot first must not
  // silently select live; Live mode still captures with it.
  const auto before=state()["acquires"].toInt();
  source.setRegionScreenshotFirst(true);PayloadResult frozen;bool frozenDone=false;
  source.acquire("region",{},[&](PayloadResult value){frozen=std::move(value);frozenDone=true;});PUTGD_TRY_VERIFY(frozenDone);
  QCOMPARE(frozen.error.code,QString("helper_outdated"));QVERIFY2(frozen.error.message.contains("log out and back in"),qPrintable(frozen.error.message));
  QCOMPARE(state()["acquires"].toInt(),before);
  source.setRegionScreenshotFirst(false);PayloadResult live;bool liveDone=false;
  source.acquire("region",{},[&](PayloadResult value){live=std::move(value);liveDone=true;});PUTGD_TRY_VERIFY(liveDone);
  QVERIFY(live.error.code!="helper_outdated");QCOMPARE(state()["acquires"].toInt(),before+1);QCOMPARE(state()["regionMode"].toString(),QString());
  stopFixture();PUTGD_TRY_VERIFY(!helperAvailable(source));QVERIFY(startFixture());PUTGD_TRY_VERIFY(helperAvailable(source));QVERIFY(!source.capabilities()["helper"].toObject()["outdated"].toBool());
 }
 void ownerReplacementRegistersLatestPid(){
  GnomeAdapter source;PUTGD_TRY_VERIFY(windowAvailable(source));source.setUiProcess(1234);
  QCOMPARE(control("FailRegistration",{uint(0),uint(1)}).type(),QDBusMessage::ReplyMessage);source.setUiProcess(0);QVERIFY(!windowAvailable(source));
  QCOMPARE(control("FailRegistration",{uint(5678),uint(1)}).type(),QDBusMessage::ReplyMessage);source.setUiProcess(5678);QVERIFY(!windowAvailable(source));
  stopFixture();PUTGD_TRY_VERIFY(!helperAvailable(source));QVERIFY(startFixture());PUTGD_TRY_VERIFY(windowAvailable(source));
  QCOMPARE(state()["currentPid"].toInt(),5678);QCOMPARE(state()["registrations"].toArray(),QJsonArray{5678});
 }
};
int main(int argc,char** argv){
 if(argc==2&&QByteArray(argv[1])=="--gnome-helper-fixture"){
  QCoreApplication app(argc,argv);GnomeHelperFixture fixture;auto bus=QDBusConnection::sessionBus();
  if(!bus.registerObject("/org/gnome/Shell/Extensions/Putgd",&fixture,QDBusConnection::ExportAllSlots|QDBusConnection::ExportAllSignals)||!bus.registerService("org.gnome.Shell"))return 2;
  std::fputs("ready\n",stdout);std::fflush(stdout);return app.exec();
 }
 if(argc>1&&QByteArray(argv[1])=="--core-adapter-transport"){
  for(int i=1;i<argc;++i)argv[i]=argv[i+1];
  --argc;
  QCoreApplication app(argc,argv);GnomeAdapterTransportTest test(true);return QTest::qExec(&test,argc,argv);
 }
 if(argc>1&&QByteArray(argv[1])=="--adapter-transport"){
  for(int i=1;i<argc;++i)argv[i]=argv[i+1];
  --argc;
  QGuiApplication app(argc,argv);GnomeAdapterTransportTest test;return QTest::qExec(&test,argc,argv);
 }
 QCoreApplication app(argc,argv);GnomeBridgeTest test;return QTest::qExec(&test,argc,argv);
}
#include "test_gnome_bridge.moc"
