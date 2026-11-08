// The labwc bridge backend's gsettings sequencing (#1275), against a fake
// gsettings on PATH that stores keys in a file and answers slowly. No labwc,
// dconf or Budgie needed: rc.xml is a plain file the test edits.
#include "platform/budgie/BudgieShortcuts.h"
#include "platform/budgie/LabwcBridgeBackend.h"
#include <QDBusConnection>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include "fixtures/Waits.h"

using namespace putgd;
namespace {
const QString entrySchema=QStringLiteral("org.buddiesofbudgie.settings-daemon.plugins.media-keys.custom-keybinding");
// get SCHEMA KEY / set SCHEMA KEY VALUE / reset-recursively SCHEMA, stored as "SCHEMA KEY<TAB>VALUE" lines.
const char* fakeGsettings=R"(#!/bin/bash
store="$FAKE_GSETTINGS_STORE"; touch "$store"; echo "$*" >> "$store.log"; sleep "${FAKE_GSETTINGS_DELAY:-0}"
case "$1" in
 get) v=$(grep -F -- "$2 $3	" "$store" | tail -n1 | cut -f2-); if [ -z "$v" ]; then [ "$3" = custom-keybindings ] && echo "@as []" || echo "''"; else echo "$v"; fi ;;
 set) grep -v -F -- "$2 $3	" "$store" > "$store.tmp" || true; printf '%s %s\t%s\n' "$2" "$3" "$4" >> "$store.tmp"; mv "$store.tmp" "$store" ;;
 reset-recursively) grep -v -F -- "$2 " "$store" > "$store.tmp" || true; mv "$store.tmp" "$store" ;;
esac
)";
}
class LabwcBridgeTest:public QObject {
 Q_OBJECT
 QTemporaryDir dir_;QString store_;
 QString value(const QString& action,const QString& key){
  QFile file(store_);if(!file.open(QIODevice::ReadOnly))return {};
  const auto prefix=entrySchema+':'+LabwcBridgeBackend::pathFor(action)+' '+key+'\t';QString found;
  for(const auto& line:QString::fromUtf8(file.readAll()).split('\n'))if(line.startsWith(prefix))found=line.mid(prefix.size());
  return found;
 }
 void writeRc(const QByteArray& keybinds){QFile rc(dir_.filePath("budgie-desktop/labwc/rc.xml"));QVERIFY(rc.open(QIODevice::WriteOnly|QIODevice::Truncate));rc.write("<labwc_config><keyboard>"+keybinds+"</keyboard></labwc_config>\n");}
 bool unbound(const QString& action){const auto v=value(action,"binding");return v.isEmpty()||v=="''";}
private slots:
 void init(){
  QVERIFY(dir_.isValid());QDir().mkpath(dir_.filePath("budgie-desktop/labwc"));QDir().mkpath(dir_.filePath("bin"));
  QFile script(dir_.filePath("bin/gsettings"));QVERIFY(script.open(QIODevice::WriteOnly|QIODevice::Truncate));script.write(fakeGsettings);script.close();
  QFile::setPermissions(script.fileName(),QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner);
  store_=dir_.filePath("store");QFile::remove(store_);QFile::remove(store_+".log");
  qputenv("PATH",(dir_.filePath("bin")+':'+qEnvironmentVariable("PATH")).toUtf8());qputenv("FAKE_GSETTINGS_STORE",store_.toUtf8());qputenv("FAKE_GSETTINGS_DELAY","0.2");
  writeRc({});
 }
 void pauseWhileTheFirstApplyIsQueuedWritesNothing(){
  // P1 (review): a pause acknowledged before the queued apply's read answers must keep every key out of rc.xml.
  LabwcBridgeBackend bridge(nullptr,dir_.path());QVERIFY(bridge.available());
  bridge.bind({{"region","Ctrl+Shift+1"}});
  bool paused=false;Error error;bridge.setPaused(true,[&](Error e){error=e;paused=true;});
  QVERIFY(putgd::test::waitFor([&]{return paused;},10000));QVERIFY(!error);
  // The queued read answers within a second; nothing may be written after it.
  PUTGD_VERIFY_STEADY(unbound("region"),3000);
  QCOMPARE(bridge.status()["region"].toObject()["state"].toString(),QString("recording"));
  // Resuming applies the latest request.
  bool resumed=false;bridge.setPaused(false,[&](Error){resumed=true;});QVERIFY(putgd::test::waitFor([&]{return resumed;},10000));
  QVERIFY(putgd::test::waitFor([&]{return value("region","binding")=="'<Control><Shift>1'";},15000));
 }
 void pauseAfterTheWritesClearsThemAfterward(){
  LabwcBridgeBackend bridge(nullptr,dir_.path());bridge.bind({{"region","Ctrl+Shift+1"}});
  QVERIFY(putgd::test::waitFor([&]{return value("region","binding")=="'<Control><Shift>1'";},15000));
  bool paused=false;bridge.setPaused(true,[&](Error){paused=true;});QVERIFY(putgd::test::waitFor([&]{return paused;},10000));
  QCOMPARE(value("region","binding"),QString("''"));
 }
 void aConflictEndsWhenItsOwnerLetsGoOfTheKey(){
  writeRc("<keybind key=\"C-S-1\" bridge=\"someone/else\"><action name=\"Execute\" command=\"x\"/></keybind>");
  LabwcBridgeBackend bridge(nullptr,dir_.path());bridge.bind({{"region","Ctrl+Shift+1"}});
  QVERIFY(putgd::test::waitFor([&]{return bridge.status()["region"].toObject()["state"].toString()=="shortcut_conflict";},15000));QVERIFY(unbound("region"));
  // The owner moves to another key; rc.xml is rewritten without a new bind() from put.gd.
  writeRc("<keybind key=\"C-S-9\" bridge=\"someone/else\"><action name=\"Execute\" command=\"x\"/></keybind>");
  QVERIFY(putgd::test::waitFor([&]{return value("region","binding")=="'<Control><Shift>1'";},15000));
  QVERIFY(bridge.status()["region"].toObject()["state"].toString()!="shortcut_conflict");
 }
 void aSecondResidentBindsNothing(){
  // Another process owns org.putgd.App.Budgie: a bound key would call it, so no backend is probed or written.
  auto squatter=QDBusConnection::connectToBus(QDBusConnection::SessionBus,QStringLiteral("squatter"));
  if(!squatter.isConnected())QSKIP("no session bus");
  QVERIFY(squatter.registerService("org.putgd.App.Budgie"));
  {
   BudgieShortcuts shortcuts;shortcuts.bind({{"region","Ctrl+Shift+1"}});
   QVERIFY(putgd::test::waitFor([&]{return shortcuts.capabilities()["backend"].toString()=="none";},5000));
   QCOMPARE(shortcuts.capabilities()["available"].toBool(true),false);
   QCOMPARE(shortcuts.capabilities()["bindings"].toObject()["region"].toObject()["state"].toString(),QString("shortcut_unavailable"));
   PUTGD_VERIFY_STEADY(!QFile::exists(store_+".log"),1000);
  }
  squatter.unregisterService("org.putgd.App.Budgie");QDBusConnection::disconnectFromBus(QStringLiteral("squatter"));
 }
};
QTEST_GUILESS_MAIN(LabwcBridgeTest)
#include "test_labwc_bridge.moc"
