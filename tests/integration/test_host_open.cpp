// #326: links and folders opened on the host, against a fake
// org.freedesktop.portal.OpenURI and FileManager1 on dbus-run-session's private
// bus, which can activate nothing. This process plays an AppImage resident;
// the host's gio and xdg-open are scripts that record how they were started
// (shell builtins only: one case leaves nothing else on PATH).
// Not desktop acceptance.
#include "platform/HostOpen.h"
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QTimer>
#include <QtTest>
#include <optional>

using namespace putgd;

namespace {
const QString service=QStringLiteral("org.freedesktop.portal.Desktop");
const QString path=QStringLiteral("/org/freedesktop/portal/desktop");
}

class FakeOpenUri final:public QObject,protected QDBusContext {
 Q_OBJECT
 Q_CLASSINFO("D-Bus Interface","org.freedesktop.portal.OpenURI")
public:
 enum class Answer { Open, Cancel, Fail, Wait, NoReply, Refuse };
 explicit FakeOpenUri(QDBusConnection peer):peer_(peer){}
 Answer answer=Answer::Open;
 QStringList calls;
 QString parent,uri,directory;
 QVariantMap options;
public slots:
 QDBusObjectPath OpenURI(const QString& parentWindow,const QString& target,const QVariantMap& callOptions){
  uri=target;return start("OpenURI",parentWindow,callOptions);
 }
 QDBusObjectPath OpenDirectory(const QString& parentWindow,const QDBusUnixFileDescriptor& fd,const QVariantMap& callOptions){
  // The file the descriptor names, as the real portal reads it.
  directory=QFileInfo(QString("/proc/self/fd/%1").arg(fd.fileDescriptor())).symLinkTarget();
  return start("OpenDirectory",parentWindow,callOptions);
 }
private:
 QDBusObjectPath start(const QString& method,const QString& parentWindow,const QVariantMap& callOptions){
  calls.append(method);parent=parentWindow;options=callOptions;
  if(answer==Answer::Refuse){sendErrorReply("org.freedesktop.DBus.Error.UnknownMethod","Synthetic old portal");return {};}
  const auto handle=path+"/request/"+message().service().mid(1).replace('.','_')+"/"+callOptions.value("handle_token").toString();
  if(answer==Answer::NoReply){setDelayedReply(true);return {};}
  const auto code=answer==Answer::Open?0u:answer==Answer::Cancel?1u:2u;
  if(answer!=Answer::Wait)QTimer::singleShot(20,this,[this,handle,code]{
   auto signal=QDBusMessage::createSignal(handle,"org.freedesktop.portal.Request","Response");signal<<code<<QVariantMap{};peer_.send(signal);
  });
  return QDBusObjectPath(handle);
 }
 QDBusConnection peer_;
};

class FakeFileManager final:public QObject,protected QDBusContext {
 Q_OBJECT
 Q_CLASSINFO("D-Bus Interface","org.freedesktop.FileManager1")
public:
 bool refuse=false;
 QList<QStringList> shown;
public slots:
 void ShowItems(const QStringList& uris,const QString&){
  if(refuse){sendErrorReply("org.freedesktop.DBus.Error.ServiceUnknown","Synthetic missing file manager");return;}
  shown.append(uris);
 }
};

class HostOpenTest final:public QObject {
 Q_OBJECT
 QDBusConnection peer_=QDBusConnection::connectToBus(QDBusConnection::SessionBus,"putgd-fake-host-desktop");
 FakeOpenUri* portal_=nullptr;
 FakeFileManager* files_=nullptr;
 QTemporaryDir root_;
 QString mount_,bin_,log_;
 QByteArray path_;
 // Each started opener: its program, arguments and what of the image it saw.
 QStringList launches(){
  QFile file(log_);if(!file.open(QIODevice::ReadOnly))return {};
  return QString::fromUtf8(file.readAll()).split('\n',Qt::SkipEmptyParts);
 }
 QStringList waitForLaunches(int count){
  for(int waited=0;launches().size()<count&&waited<5000;waited+=10)QTest::qWait(10);
  return launches();
 }
 static QJsonObject run(const std::function<void(host::Opened)>& request){
  std::optional<QJsonObject> result;
  request([&](const QJsonObject& value){result=value;});
  for(int waited=0;!result&&waited<8000;waited+=10)QTest::qWait(10);
  return result.value_or(QJsonObject{{"timedOut",true}});
 }
 void writeOpener(const QString& directory,const QString& name){
  QFile script(directory+"/"+name);QVERIFY(script.open(QIODevice::WriteOnly));
  script.write(("#!/bin/sh\nprintf '%s|%s|lib=%s|plugins=%s|appdir=%s|saved=%s\\n' \"${0##*/}\" \"$*\" "
                "\"${LD_LIBRARY_PATH-unset}\" \"${QT_PLUGIN_PATH-unset}\" \"${APPDIR-unset}\" \"${PUTGD_HOST_ENVIRONMENT-unset}\" >> '"+log_+"'\n").toUtf8());
  script.close();QVERIFY(script.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
 }
 void registerFileManager(bool present){
  if(present){QVERIFY(peer_.registerService("org.freedesktop.FileManager1"));}
  else peer_.unregisterService("org.freedesktop.FileManager1");
 }
private slots:
 void initTestCase(){
  QVERIFY2(QDBusConnection::sessionBus().isConnected(),"Run under dbus-run-session");
  const auto activatable=QDBusConnection::sessionBus().interface()->activatableServiceNames().value();
  QVERIFY2(!activatable.contains(service)&&!activatable.contains("org.freedesktop.FileManager1"),"Run under dbus-run-session --config-file=tests/fixtures/dbus-session-isolated.conf");
  QVERIFY(peer_.isConnected());
  QVERIFY(root_.isValid());
  mount_=root_.filePath(".mount_putgdT3st");QVERIFY(QDir().mkpath(mount_+"/usr/lib"));
  bin_=root_.filePath("host-bin");QVERIFY(QDir().mkpath(bin_));log_=root_.filePath("launches");
  writeOpener(bin_,"gio");writeOpener(bin_,"xdg-open");
  // What AppRun gives the resident on a host with its own library path.
  path_=qgetenv("PATH");qputenv("PATH",bin_.toUtf8()+":"+path_);
  qputenv("APPDIR",mount_.toUtf8());qputenv("APPIMAGE","/home/u/put.gd.AppImage");
  qputenv("PUTGD_HOST_ENVIRONMENT","LD_LIBRARY_PATH QT_PLUGIN_PATH");qputenv("PUTGD_HOST_LD_LIBRARY_PATH","/opt/host/lib");qunsetenv("PUTGD_HOST_QT_PLUGIN_PATH");
  qputenv("LD_LIBRARY_PATH",(mount_+"/usr/lib:"+mount_+"/usr/lib/putgd:/opt/host/lib").toUtf8());qputenv("QT_PLUGIN_PATH",(mount_+"/usr/plugins").toUtf8());
  portal_=new FakeOpenUri(peer_);files_=new FakeFileManager;
  QVERIFY(peer_.registerObject(path,portal_,QDBusConnection::ExportAllSlots));
  QVERIFY(peer_.registerObject("/org/freedesktop/FileManager1",files_,QDBusConnection::ExportAllSlots));
  host::setOpenTimeoutsMs(500,1000);
 }
 void cleanupTestCase(){peer_.unregisterService(service);registerFileManager(false);delete files_;qputenv("PATH",path_);}
 void init(){
  portal_->answer=FakeOpenUri::Answer::Open;portal_->calls.clear();portal_->uri.clear();portal_->directory.clear();
  files_->refuse=false;files_->shown.clear();QFile::remove(log_);
  QVERIFY(peer_.registerService(service));registerFileManager(false);
 }

 void linkOpensThroughThePortalWithNothingStarted(){
  const QUrl link("https://put.gd/a b/clip%C3%A9.png?x=1");
  const auto result=run([&](host::Opened done){host::openLink(link,done);});
  QCOMPARE(result["route"].toString(),QString("portal"));QVERIFY(result["opened"].toBool());
  QCOMPARE(portal_->calls,QStringList{"OpenURI"});
  QCOMPARE(portal_->uri,link.toString(QUrl::FullyEncoded));QCOMPARE(portal_->parent,QString());
  QVERIFY(portal_->options.value("handle_token").toString().startsWith("putgd"));
  QTest::qWait(100);QVERIFY(launches().isEmpty());
 }
 void failedPortalFallsBackToHostGioWithoutImagePaths(){
  for(const auto answer:{FakeOpenUri::Answer::Fail,FakeOpenUri::Answer::Refuse}){
   QFile::remove(log_);portal_->answer=answer;
   const auto result=run([&](host::Opened done){host::openLink(QUrl("https://put.gd/x.png"),done);});
   QCOMPARE(result["route"].toString(),QString("launcher"));QVERIFY(result["opened"].toBool());QCOMPARE(result["program"].toString(),QString("gio"));
   QCOMPARE(result["fallbackFrom"].toString(),QString(answer==FakeOpenUri::Answer::Fail?"portal_response_2":"org.freedesktop.DBus.Error.UnknownMethod"));
   const auto started=waitForLaunches(1);
   QCOMPARE(started,QStringList{"gio|open https://put.gd/x.png|lib=/opt/host/lib|plugins=unset|appdir=unset|saved=unset"});
  }
 }
 void missingOrHungPortalFallsBackOnce(){
  peer_.unregisterService(service);
  auto result=run([&](host::Opened done){host::openLink(QUrl("https://put.gd/missing.png"),done);});
  QCOMPARE(result["route"].toString(),QString("launcher"));QCOMPARE(result["fallbackFrom"].toString(),QString("org.freedesktop.DBus.Error.ServiceUnknown"));
  QCOMPARE(waitForLaunches(1).size(),1);
  QVERIFY(peer_.registerService(service));QFile::remove(log_);
  portal_->answer=FakeOpenUri::Answer::NoReply;
  result=run([&](host::Opened done){host::openLink(QUrl("https://put.gd/hung.png"),done);});
  QCOMPARE(result["route"].toString(),QString("launcher"));QCOMPARE(result["fallbackFrom"].toString(),QString("org.freedesktop.DBus.Error.NoReply"));
  QTest::qWait(1200);QCOMPARE(launches().size(),1);
 }
 void cancelledOrUnansweredIsNotOpenedTwice(){
  portal_->answer=FakeOpenUri::Answer::Cancel;
  auto result=run([&](host::Opened done){host::openLink(QUrl("https://put.gd/cancel.png"),done);});
  QCOMPARE(result["route"].toString(),QString("portal"));QVERIFY(!result["opened"].toBool());QCOMPARE(result["error"].toString(),QString("cancelled"));
  portal_->answer=FakeOpenUri::Answer::Wait;
  result=run([&](host::Opened done){host::openLink(QUrl("https://put.gd/wait.png"),done);});
  QCOMPARE(result["error"].toString(),QString("no_answer"));
  QTest::qWait(100);QVERIFY(launches().isEmpty());
 }
 void onlyWebLinksAreOpened(){
  for(const auto& link:{QUrl("file:///etc/hostname"),QUrl("javascript:alert(1)"),QUrl()}){
   const auto result=run([&](host::Opened done){host::openLink(link,done);});
   QCOMPARE(result["error"].toString(),QString("unsupported_link"));
  }
  QVERIFY(portal_->calls.isEmpty());QTest::qWait(50);QVERIFY(launches().isEmpty());
 }
 void showInFolderAsksTheFileManagerFirst(){
  registerFileManager(true);
  const auto package=root_.filePath("putgd-diagnostics-20261021-010203Z.zip");{QFile file(package);QVERIFY(file.open(QIODevice::WriteOnly));}
  const auto result=run([&](host::Opened done){host::showInFolder(package,done);});
  QCOMPARE(result["route"].toString(),QString("file-manager"));QVERIFY(result["opened"].toBool());
  QCOMPARE(files_->shown,QList<QStringList>{{QUrl::fromLocalFile(package).toString(QUrl::FullyEncoded)}});
  QVERIFY(portal_->calls.isEmpty());QTest::qWait(50);QVERIFY(launches().isEmpty());
 }
 void showInFolderUsesThePortalWithoutAFileManager(){
  const auto package=root_.filePath("with space é.zip");{QFile file(package);QVERIFY(file.open(QIODevice::WriteOnly));}
  for(const bool refusing:{false,true}){
   registerFileManager(refusing);files_->refuse=refusing;portal_->calls.clear();
   const auto result=run([&](host::Opened done){host::showInFolder(package,done);});
   QCOMPARE(result["route"].toString(),QString("portal"));QVERIFY(result["opened"].toBool());
   QCOMPARE(result["fallbackFrom"].toString(),QString("org.freedesktop.DBus.Error.ServiceUnknown"));
   QCOMPARE(portal_->calls,QStringList{"OpenDirectory"});QCOMPARE(portal_->directory,QFileInfo(package).canonicalFilePath());
  }
  QTest::qWait(50);QVERIFY(launches().isEmpty());
 }
 void showInFolderFallsBackToGioOnTheFolder(){
  const auto package=root_.filePath("fallback.zip");{QFile file(package);QVERIFY(file.open(QIODevice::WriteOnly));}
  portal_->answer=FakeOpenUri::Answer::Fail;
  auto result=run([&](host::Opened done){host::showInFolder(package,done);});
  QCOMPARE(result["route"].toString(),QString("launcher"));
  QCOMPARE(result["fallbackFrom"].toString(),QString("org.freedesktop.DBus.Error.ServiceUnknown, portal_response_2"));
  QCOMPARE(waitForLaunches(1),QStringList{"gio|open "+QUrl::fromLocalFile(root_.path()).toString(QUrl::FullyEncoded)+"|lib=/opt/host/lib|plugins=unset|appdir=unset|saved=unset"});
  result=run([&](host::Opened done){host::showInFolder(root_.filePath("gone.zip"),done);});
  QCOMPARE(result["error"].toString(),QString("missing"));
 }
 void openFileAndXdgOpenUseTheHostEnvironment(){
  const auto config=root_.filePath("config.json");{QFile file(config);QVERIFY(file.open(QIODevice::WriteOnly));}
  auto result=run([&](host::Opened done){host::openFile(config,done);});
  QCOMPARE(result["route"].toString(),QString("launcher"));QCOMPARE(result["program"].toString(),QString("gio"));QVERIFY(result["pid"].toInteger()>0);
  QCOMPARE(waitForLaunches(1),QStringList{"gio|open "+QUrl::fromLocalFile(config).toString(QUrl::FullyEncoded)+"|lib=/opt/host/lib|plugins=unset|appdir=unset|saved=unset"});
  // A host without gio: xdg-open.
  const auto other=root_.filePath("no-gio");QVERIFY(QDir().mkpath(other));writeOpener(other,"xdg-open");
  qputenv("PATH",other.toUtf8());auto restore=qScopeGuard([&]{qputenv("PATH",bin_.toUtf8()+":"+path_);});
  QFile::remove(log_);
  result=run([&](host::Opened done){host::openFile(config,done);});
  QCOMPARE(result["program"].toString(),QString("xdg-open"));
  QCOMPARE(waitForLaunches(1),QStringList{"xdg-open|"+QUrl::fromLocalFile(config).toString(QUrl::FullyEncoded)+"|lib=/opt/host/lib|plugins=unset|appdir=unset|saved=unset"});
  qputenv("PATH",root_.filePath("empty").toUtf8());
  result=run([&](host::Opened done){host::openFile(config,done);});
  QVERIFY(!result["opened"].toBool());QCOMPARE(result["error"].toString(),QString("launcher_missing"));
 }
 void testOpenerReplacesTheHost(){
  QList<QUrl> seen;host::setTestOpener([&](const QUrl& url){seen.append(url);});auto restore=qScopeGuard([]{host::setTestOpener({});});
  const auto result=run([&](host::Opened done){host::openLink(QUrl("https://put.gd/t.png"),done);});
  QCOMPARE(result["route"].toString(),QString("test"));QCOMPARE(seen,QList<QUrl>{QUrl("https://put.gd/t.png")});
  QVERIFY(portal_->calls.isEmpty());
 }
};

QTEST_GUILESS_MAIN(HostOpenTest)
#include "test_host_open.moc"
