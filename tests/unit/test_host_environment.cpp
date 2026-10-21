// #326: the environment of a host program put.gd starts. Synthetic mounts and
// a synthetic host program; not desktop acceptance.
#include "core/HostEnvironment.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace putgd;

class HostEnvironmentTest final:public QObject {
 Q_OBJECT
 QTemporaryDir root_;
 QString mount_,executable_;
 // What AppRun and the AppImage runtime give the resident on a host that set
 // LD_LIBRARY_PATH and not QT_PLUGIN_PATH.
 QProcessEnvironment appImage()const{
  QProcessEnvironment env;
  env.insert("PATH","/usr/local/bin:/usr/bin");env.insert("DISPLAY",":0");env.insert("DBUS_SESSION_BUS_ADDRESS","unix:path=/run/user/1000/bus");
  env.insert("APPDIR",mount_);env.insert("APPIMAGE","/home/u/put.gd.AppImage");env.insert("ARGV0","put.gd.AppImage");env.insert("OWD","/home/u");
  env.insert("PUTGD_HOST_ENVIRONMENT","LD_LIBRARY_PATH QT_PLUGIN_PATH");env.insert("PUTGD_HOST_LD_LIBRARY_PATH","/opt/host/lib");
  env.insert("LD_LIBRARY_PATH",mount_+"/usr/lib/putgd:"+mount_+"/usr/lib:"+mount_+"/usr/lib/putgd:/opt/host/lib");
  env.insert("QT_PLUGIN_PATH",mount_+"/usr/plugins:"+mount_+"/usr/lib/qt6/plugins");
  return env;
 }
 static QStringList entries(const QProcessEnvironment& env){QStringList result;for(const auto& key:env.keys())result.append(key+"="+env.value(key));result.sort();return result;}
private slots:
 void initTestCase(){
  QVERIFY(root_.isValid());
  mount_=root_.filePath(".mount_putgdA1b2C3");QVERIFY(QDir().mkpath(mount_+"/usr/bin"));
  executable_=mount_+"/usr/bin/putgd";QFile file(executable_);QVERIFY(file.open(QIODevice::WriteOnly));file.close();
 }
 void packagesKeepTheirEnvironment(){
  // A DEB/RPM: the launcher's private SDK path stays; nothing is restored.
  QProcessEnvironment env;env.insert("PATH","/usr/bin");env.insert("LD_LIBRARY_PATH","/usr/bin/../lib64/putgd");env.insert("QT_PLUGIN_PATH","/user/plugins");
  const auto result=host::environment({env,{},"/usr/bin/putgd"});
  QCOMPARE(entries(result),entries(env));
  QVERIFY(!host::fromAppImage(env,"/usr/bin/putgd"));
  // APPDIR alone, left in the environment by something else, is not this process's mount.
  env.insert("APPDIR",mount_);
  QCOMPARE(entries(host::environment({env,{},"/usr/bin/putgd"})),entries(env));
 }
 void appImageRestoresWhatAppRunReplaced(){
  const auto result=host::environment({appImage(),{},executable_});
  QCOMPARE(result.value("LD_LIBRARY_PATH"),QString("/opt/host/lib"));
  QVERIFY2(!result.contains("QT_PLUGIN_PATH"),"The host set none, so the host program gets none");
  for(const char* gone:{"PUTGD_HOST_ENVIRONMENT","PUTGD_HOST_LD_LIBRARY_PATH","APPDIR","APPIMAGE","ARGV0","OWD"})QVERIFY2(!result.contains(gone),gone);
  QCOMPARE(result.value("PATH"),QString("/usr/local/bin:/usr/bin"));
  QCOMPARE(result.value("DISPLAY"),QString(":0"));
  QCOMPARE(result.value("DBUS_SESSION_BUS_ADDRESS"),QString("unix:path=/run/user/1000/bus"));
  for(const auto& line:entries(result))QVERIFY2(!line.contains(mount_),qPrintable(line));
 }
 void hostValuesAreRestoredExactly(){
  auto env=appImage();env.insert("PUTGD_HOST_LD_LIBRARY_PATH","");env.insert("PUTGD_HOST_QT_PLUGIN_PATH","/a b/plugins::");
  const auto result=host::environment({env,{},executable_});
  QVERIFY(result.contains("LD_LIBRARY_PATH"));QCOMPARE(result.value("LD_LIBRARY_PATH"),QString());
  QCOMPARE(result.value("QT_PLUGIN_PATH"),QString("/a b/plugins::"));
  env.remove("PUTGD_HOST_LD_LIBRARY_PATH");
  QVERIFY(!host::environment({env,{},executable_}).contains("LD_LIBRARY_PATH"));
 }
 void otherMountPathsAreDropped(){
  auto env=appImage();
  env.insert("GST_PLUGIN_SYSTEM_PATH_1_0",mount_+"/usr/lib/gstreamer-1.0");
  env.insert("GST_PLUGIN_SCANNER",mount_+"/usr/libexec/gstreamer-1.0/gst-plugin-scanner");
  env.insert("PYTHONPATH","/home/u/py:"+mount_+"/usr/lib/python3");
  env.insert("XDG_DATA_DIRS",mount_+"/usr/share:/usr/local/share:/usr/share");
  env.insert("LD_PRELOAD","/usr/lib/libhost.so "+mount_+"/usr/lib/libimage.so");
  env.insert("NEIGHBOUR",mount_+"-other/lib");
  env.insert("NOT_A_PATH","x:"+mount_.mid(1));
  const auto result=host::environment({env,{},executable_});
  QVERIFY(!result.contains("GST_PLUGIN_SYSTEM_PATH_1_0"));
  QVERIFY(!result.contains("GST_PLUGIN_SCANNER"));
  QCOMPARE(result.value("PYTHONPATH"),QString("/home/u/py"));
  QCOMPARE(result.value("XDG_DATA_DIRS"),QString("/usr/local/share:/usr/share"));
  QCOMPARE(result.value("LD_PRELOAD"),QString("/usr/lib/libhost.so"));
  QCOMPARE(result.value("NEIGHBOUR"),mount_+"-other/lib");
  QCOMPARE(result.value("NOT_A_PATH"),"x:"+mount_.mid(1));
 }
 void mountIsMatchedThroughSymlinks(){
  // AppImage runtimes mount under /tmp; APPDIR may name it through a link.
  const auto link=root_.filePath("linked-mount");QVERIFY(QFile::link(mount_,link));
  auto env=appImage();env.insert("APPDIR",link);env.insert("GST_PLUGIN_PATH",mount_+"/usr/lib/gstreamer-1.0");env.insert("GI_TYPELIB_PATH",link+"/usr/lib/girepository-1.0");
  const auto result=host::environment({env,{},executable_});
  QVERIFY(!result.contains("GST_PLUGIN_PATH"));QVERIFY(!result.contains("GI_TYPELIB_PATH"));
 }
 void olderAppRunIsRecognisedByTheExecutable(){
  // No saved values: the image's own entries are still dropped.
  auto env=appImage();env.remove("PUTGD_HOST_ENVIRONMENT");env.remove("PUTGD_HOST_LD_LIBRARY_PATH");
  QVERIFY(host::fromAppImage(env,executable_));
  const auto result=host::environment({env,{},executable_});
  QCOMPARE(result.value("LD_LIBRARY_PATH"),QString("/opt/host/lib"));
  QVERIFY(!result.contains("QT_PLUGIN_PATH"));QVERIFY(!result.contains("APPDIR"));
 }
 void malformedSavedNamesAreIgnored(){
  auto env=appImage();
  env.insert("PUTGD_HOST_ENVIRONMENT","LD_LIBRARY_PATH path;rm PUTGD_HOST_X lower");
  env.insert("PUTGD_HOST_path;rm","x");env.insert("lower","kept");env.insert("PUTGD_HOST_lower","replaced");
  const auto result=host::environment({env,{},executable_});
  QCOMPARE(result.value("LD_LIBRARY_PATH"),QString("/opt/host/lib"));
  QCOMPARE(result.value("lower"),QString("kept"));
  for(const auto& key:result.keys())QVERIFY2(!key.startsWith("PUTGD_HOST_"),qPrintable(key));
 }
 void processChangesAreUndoneInEveryPackage(){
  // putgd-ui's editor clears the desktop identity and sets a theme for itself;
  // the resident forces stderr logging for its helpers.
  QProcessEnvironment env;env.insert("QT_QPA_PLATFORMTHEME","generic");env.insert("QT_FORCE_STDERR_LOGGING","1");
  host::Kept kept{{"QT_QPA_PLATFORMTHEME",std::nullopt},{"XDG_CURRENT_DESKTOP",QString("X-Cinnamon")},{"QT_FORCE_STDERR_LOGGING",std::nullopt}};
  for(const auto& current:{env,[&]{auto image=appImage();image.insert("QT_QPA_PLATFORMTHEME","generic");return image;}()}){
   const auto result=host::environment({current,kept,executable_});
   QVERIFY(!result.contains("QT_QPA_PLATFORMTHEME"));QVERIFY(!result.contains("QT_FORCE_STDERR_LOGGING"));
   QCOMPARE(result.value("XDG_CURRENT_DESKTOP"),QString("X-Cinnamon"));
  }
 }
 void keepRecordsTheFirstValue(){
  qputenv("PUTGD_TEST_KEEP","first");host::keep("PUTGD_TEST_KEEP");qputenv("PUTGD_TEST_KEEP","second");host::keep("PUTGD_TEST_KEEP");
  qunsetenv("PUTGD_TEST_UNSET");host::keep("PUTGD_TEST_UNSET");qputenv("PUTGD_TEST_UNSET","set later");
  const auto result=host::environment();
  QCOMPARE(result.value("PUTGD_TEST_KEEP"),QString("first"));QVERIFY(!result.contains("PUTGD_TEST_UNSET"));
  qunsetenv("PUTGD_TEST_KEEP");qunsetenv("PUTGD_TEST_UNSET");
 }
 void programsAreFoundOnTheHostPath(){
  const auto bin=root_.filePath("host-bin");QVERIFY(QDir().mkpath(bin));
  QFile tool(bin+"/putgd-test-tool");QVERIFY(tool.open(QIODevice::WriteOnly));tool.write("#!/bin/sh\n");tool.close();
  QVERIFY(tool.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
  QProcessEnvironment env;env.insert("PATH","/nonexistent:"+bin);
  QCOMPARE(host::findProgram("putgd-test-tool",env),bin+"/putgd-test-tool");
  QCOMPARE(host::findProgram(bin+"/putgd-test-tool",env),bin+"/putgd-test-tool");
  QVERIFY(host::findProgram("putgd-test-missing",env).isEmpty());
 }
 void startedHostProgramSeesNoImagePaths(){
  // This process plays the AppImage resident; the host program records its environment.
  const auto receipt=root_.filePath("receipt");
  const auto program=root_.filePath("record-environment");
  QFile script(program);QVERIFY(script.open(QIODevice::WriteOnly));
  script.write(("#!/bin/sh\nenv > '"+receipt+".tmp' && mv '"+receipt+".tmp' '"+receipt+"'\n").toUtf8());script.close();
  QVERIFY(script.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
  const auto saved=QProcessEnvironment::systemEnvironment();
  const auto image=appImage();
  for(const auto& key:image.keys())if(key!="PATH"&&key!="DISPLAY"&&key!="DBUS_SESSION_BUS_ADDRESS")qputenv(key.toUtf8().constData(),image.value(key).toUtf8());
  qint64 pid=0;const bool started=host::startDetached(program,{},&pid);
  for(const auto& key:image.keys())if(key!="PATH"&&key!="DISPLAY"&&key!="DBUS_SESSION_BUS_ADDRESS"){if(saved.contains(key))qputenv(key.toUtf8().constData(),saved.value(key).toUtf8());else qunsetenv(key.toUtf8().constData());}
  QVERIFY(started);QVERIFY(pid>0);
  QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(receipt),5000);
  QFile file(receipt);QVERIFY(file.open(QIODevice::ReadOnly));const auto recorded=QString::fromUtf8(file.readAll());
  QVERIFY2(!recorded.contains(mount_),qPrintable(recorded));
  QVERIFY(recorded.contains("\nLD_LIBRARY_PATH=/opt/host/lib\n")||recorded.startsWith("LD_LIBRARY_PATH=/opt/host/lib\n"));
  QVERIFY(!recorded.contains("PUTGD_HOST_"));
 }
};

QTEST_GUILESS_MAIN(HostEnvironmentTest)
#include "test_host_environment.moc"
