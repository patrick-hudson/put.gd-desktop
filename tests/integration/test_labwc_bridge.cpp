// The labwc bridge backend's gsettings sequencing (#1275), against a fake
// gsettings on PATH that stores keys in a file and answers slowly. No labwc,
// dconf or Budgie needed: rc.xml is a plain file the test edits.
#include "platform/budgie/BudgieClicks.h"
#include "platform/budgie/BudgieShortcuts.h"
#include "platform/budgie/LabwcBridgeBackend.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QJsonArray>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include "fixtures/Waits.h"

using namespace putgd;
namespace {
// A Budgie rc.xml in the bridge's own formatting, with one bind of the user's on a chord put.gd also binds.
const QString clickRc=QStringLiteral(R"(<?xml version="1.0"?>
<labwc_config>
	<!-- kept as written -->
	<mouse>
		<doubleClickTime>400</doubleClickTime>
		<default />
		<context name="Desktop">
			<mousebind button="Left" action="Press">
				<action name="Focus" />
			</mousebind>
		</context>
		<context name="client">
			<mousebind button="Ctrl-Left" action="Drag">
				<action name="Move" />
			</mousebind>
			<mousebind button="C-Left" action="press">
				<action name="Execute" command="notify-send &quot;mine&quot;" />
			</mousebind>
		</context>
	</mouse>
</labwc_config>
)");
// put.gd's own binds in a file: "context|button" -> each action's name, or its command for Execute.
QHash<QString,QStringList> ownClickBinds(const QString& rc){
 QHash<QString,QStringList> binds;QXmlStreamReader reader(rc);QString context,bind;bool own=false;
 while(!reader.atEnd()){
  const auto token=reader.readNext();
  if(token==QXmlStreamReader::StartElement&&reader.name()==QLatin1String("context")){context=reader.attributes().value("name").toString();own=reader.attributes().value("putgd")==QLatin1String(BudgieClicks::marker);}
  else if(token==QXmlStreamReader::StartElement&&reader.name()==QLatin1String("mousebind")&&own){bind=context+'|'+reader.attributes().value("button").toString();binds[bind];}
  else if(token==QXmlStreamReader::StartElement&&reader.name()==QLatin1String("action")&&own)binds[bind].append(reader.attributes().hasAttribute("command")?reader.attributes().value("command").toString():reader.attributes().value("name").toString());
  else if(token==QXmlStreamReader::EndElement&&reader.name()==QLatin1String("context"))own=false;
 }
 return reader.hasError()?QHash<QString,QStringList>{}:binds;
}
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
 // Click highlights (#1364): a recording's mouse binds go into rc.xml and exactly they come out again.
 void clickBindsStandInForTheBindsTheyReplace(){
  QString error;const auto with=BudgieClicks::withBinds(clickRc,&error);QVERIFY2(error.isEmpty(),qPrintable(error));
  QVERIFY(!BudgieClicks::hasBinds(clickRc));QVERIFY(BudgieClicks::hasBinds(with));
  // The file is the user's again, to the byte, and adding twice adds once.
  QCOMPARE(BudgieClicks::withoutBinds(with,&error),clickRc);QVERIFY(error.isEmpty());
  QCOMPARE(BudgieClicks::withBinds(with,&error),with);QVERIFY(error.isEmpty());
  const auto binds=ownClickBinds(with);QCOMPARE(binds.size(),21);
  // labwc's default for a bare press on a window, which a later bind with the same key replaces.
  QCOMPARE(binds["Client|Left"],(QStringList{"Focus","Raise",BudgieClicks::command(1)}));
  QCOMPARE(binds["Client|Middle"],(QStringList{"Focus","Raise",BudgieClicks::command(2)}));
  QCOMPARE(binds["Client|Right"],(QStringList{"Focus","Raise",BudgieClicks::command(3)}));
  // The user's own bind on that chord keeps its action; another event on the chord is not a Press bind.
  QCOMPARE(binds["Client|C-Left"],(QStringList{"notify-send \"mine\"",BudgieClicks::command(1)}));
  QCOMPARE(binds["Client|S-Right"],QStringList{BudgieClicks::command(3)});
  QCOMPARE(binds["Client|C-S-Middle"],QStringList{BudgieClicks::command(2)});
  QCOMPARE(binds["Titlebar|Left"],(QStringList{"Focus","Raise",BudgieClicks::command(1)}));
  QCOMPARE(binds["Titlebar|Right"],QStringList{BudgieClicks::command(3)});
  // The bridge writes its tree back in its own layout: put.gd's elements are still found and removed.
  QString squeezed=with;squeezed.replace(QRegularExpression(">\\s+<"),"><");
  QVERIFY(BudgieClicks::hasBinds(squeezed));QVERIFY(!BudgieClicks::hasBinds(BudgieClicks::withoutBinds(squeezed,&error)));QVERIFY(error.isEmpty());
  QVERIFY(ownClickBinds(BudgieClicks::withoutBinds(squeezed)).isEmpty());
 }
 void clickBindsWithoutDefaultsOrAMouseElement_data(){
  QTest::addColumn<QString>("rc");QTest::addColumn<bool>("focusRaise");
  QTest::newRow("no mouse element")<<QStringLiteral("<labwc_config>\n\t<core><gap>4</gap></core>\n</labwc_config>\n")<<true;
  QTest::newRow("empty mouse element")<<QStringLiteral("<labwc_config>\n\t<mouse />\n</labwc_config>\n")<<true;
  // No <default/>: a press on a window does nothing of labwc's, and put.gd adds nothing to that.
  QTest::newRow("no defaults")<<QStringLiteral("<labwc_config>\n\t<mouse>\n\t\t<context name=\"Frame\"><mousebind button=\"A-Left\" action=\"Drag\"><action name=\"Move\" /></mousebind></context>\n\t</mouse>\n</labwc_config>\n")<<false;
  QTest::newRow("one line")<<QStringLiteral("<labwc_config><mouse><default /></mouse></labwc_config>")<<true;
 }
 void clickBindsWithoutDefaultsOrAMouseElement(){
  QFETCH(QString,rc);QFETCH(bool,focusRaise);
  QString error;const auto with=BudgieClicks::withBinds(rc,&error);QVERIFY2(error.isEmpty(),qPrintable(error));
  QCOMPARE(BudgieClicks::withoutBinds(with,&error),rc);QCOMPARE(BudgieClicks::withBinds(with,&error),with);
  const auto binds=ownClickBinds(with);QCOMPARE(binds.size(),21);
  QCOMPARE(binds["Client|Left"],focusRaise?(QStringList{"Focus","Raise",BudgieClicks::command(1)}):QStringList{BudgieClicks::command(1)});
  // Without a mouse element of the user's, put.gd's own asks for labwc's defaults, as none at all does.
  QCOMPARE(with.contains("<default />"),focusRaise);
 }
 void clickBindsLeaveABrokenFileAlone(){
  QString error;const QString broken=QStringLiteral("<labwc_config><mouse><context name=\"Client\">");
  QCOMPARE(BudgieClicks::withBinds(broken,&error),broken);QVERIFY(!error.isEmpty());
  QCOMPARE(BudgieClicks::withoutBinds(broken,&error),broken);QVERIFY(!error.isEmpty());
 }
 void clicksAreBoundForARecordingOnly(){
  const QString path=dir_.filePath("budgie-desktop/labwc/rc.xml");
  auto write=[&](const QString& text){QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));file.write(text.toUtf8());};
  auto read=[&]{QFile file(path);return file.open(QIODevice::ReadOnly)?QString::fromUtf8(file.readAll()):QString();};
  if(!QDBusConnection::sessionBus().isConnected())QSKIP("no session bus");
  // A crash left put.gd's binds in the file: they go when the resident starts.
  write(BudgieClicks::withBinds(clickRc));
  BudgieShortcuts shortcuts;BudgieClicks clicks(nullptr,path);
  PUTGD_TRY_COMPARE(read(),clickRc);
  const auto capability=clicks.clickCapability();
  QVERIFY2(capability["available"].toBool(),qPrintable(capability["message"].toString()));
  QCOMPARE(capability["route"].toString(),QString("labwc-mouse-binds"));QVERIFY(capability["needsPointer"].toBool());
  QCOMPARE(capability["targets"].toArray(),(QJsonArray{"monitor","desktop","region"}));
  QVERIFY(capability["targetReasons"].toObject()["window"].toString().contains("cursor source"));
  QList<QJsonObject> presses;Error replied{"unanswered","",{}};
  clicks.watchClicks({{"kind","monitor"}},[&](QJsonObject press){presses.append(press);},[&](Error error,QJsonObject){replied=error;});
  QVERIFY2(!replied,qPrintable(replied.message));QVERIFY(BudgieClicks::hasBinds(read()));
  // What a bound press runs.
  auto press=[](uint button){QDBusConnection::sessionBus().call(QDBusMessage::createMethodCall("org.putgd.App.Budgie","/org/putgd/Clicks","org.putgd.App.Budgie","Click")<<button,QDBus::Block,2000);};
  press(3);PUTGD_TRY_COMPARE(presses.size(),1);
  QCOMPARE(presses.last()["button"].toInt(),3);QVERIFY(presses.last()["pointer"].toBool());QVERIFY(!presses.last().contains("x"));
  press(7);press(0);PUTGD_COMPARE_STEADY(presses.size(),1,200);
  // Budgie's bridge writes its own tree back, without put.gd's binds: they return.
  write(clickRc);PUTGD_TRY_VERIFY(BudgieClicks::hasBinds(read()));
  clicks.unwatchClicks();QCOMPARE(read(),clickRc);
  press(1);PUTGD_COMPARE_STEADY(presses.size(),1,200);
  // A bridge that read the file during the recording writes the binds back afterwards: they go again.
  write(BudgieClicks::withBinds(clickRc));PUTGD_TRY_COMPARE(read(),clickRc);
 }
 void clicksNeedThisResidentsBusName(){
  const QString path=dir_.filePath("budgie-desktop/labwc/rc.xml");
  {QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));file.write(clickRc.toUtf8());}
  if(!QDBusConnection::sessionBus().isConnected())QSKIP("no session bus");
  // Nobody owns org.putgd.App.Budgie here: a bound press would reach no put.gd.
  BudgieClicks clicks(nullptr,path);
  QCOMPARE(clicks.clickCapability()["available"].toBool(),false);QCOMPARE(clicks.clickCapability()["reason"].toString(),QString("budgie_service_unavailable"));
  Error replied;clicks.watchClicks({},[](QJsonObject){},[&](Error error,QJsonObject){replied=error;});
  QCOMPARE(replied.code,QString("budgie_service_unavailable"));
  PUTGD_VERIFY_STEADY(!BudgieClicks::hasBinds([&]{QFile file(path);return file.open(QIODevice::ReadOnly)?QString::fromUtf8(file.readAll()):QString();}()),300);
 }
};
QTEST_GUILESS_MAIN(LabwcBridgeTest)
#include "test_labwc_bridge.moc"
