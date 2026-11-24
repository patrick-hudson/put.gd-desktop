// Click highlights on Budgie (#1364) edit the user's labwc rc.xml: a recording's
// mouse binds go in and exactly they come out again, a broken file is left
// alone, and binds a crash left behind are removed. A fake gsettings on PATH
// stands in for the bridge; no labwc, dconf or Budgie needed.
#include "platform/budgie/BudgieClicks.h"
#include "platform/budgie/BudgieShortcuts.h"
#include <QDBusConnection>
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
private slots:
 void init(){
  QVERIFY(dir_.isValid());QDir().mkpath(dir_.filePath("budgie-desktop/labwc"));QDir().mkpath(dir_.filePath("bin"));
  QFile script(dir_.filePath("bin/gsettings"));QVERIFY(script.open(QIODevice::WriteOnly|QIODevice::Truncate));script.write(fakeGsettings);script.close();
  QFile::setPermissions(script.fileName(),QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner);
  store_=dir_.filePath("store");QFile::remove(store_);QFile::remove(store_+".log");
  qputenv("PATH",(dir_.filePath("bin")+':'+qEnvironmentVariable("PATH")).toUtf8());qputenv("FAKE_GSETTINGS_STORE",store_.toUtf8());qputenv("FAKE_GSETTINGS_DELAY","0");
 }
 // Click highlights (#1364): a recording's mouse binds go into rc.xml and exactly they come out again.
 void clickBindsStandInForTheBindsTheyReplace(){
  QString error;const auto with=BudgieClicks::withBinds(clickRc,&error);QVERIFY2(error.isEmpty(),qPrintable(error));
  QVERIFY(!BudgieClicks::hasBinds(clickRc));QVERIFY(BudgieClicks::hasBinds(with));
  // The file is the user's again, to the byte, and adding twice adds once.
  QCOMPARE(BudgieClicks::withoutBinds(with,&error),clickRc);QVERIFY(error.isEmpty());
  QCOMPARE(BudgieClicks::withBinds(with,&error),with);QVERIFY(error.isEmpty());
  // The bridge writes its tree back in its own layout: put.gd's elements are still found and removed.
  QString squeezed=with;squeezed.replace(QRegularExpression(">\\s+<"),"><");
  QVERIFY(BudgieClicks::hasBinds(squeezed));QVERIFY(!BudgieClicks::hasBinds(BudgieClicks::withoutBinds(squeezed,&error)));QVERIFY(error.isEmpty());
  QVERIFY(ownClickBinds(BudgieClicks::withoutBinds(squeezed)).isEmpty());
 }
 void clickBindsWithoutDefaultsOrAMouseElement_data(){
  QTest::addColumn<QString>("rc");
  QTest::newRow("no mouse element")<<QStringLiteral("<labwc_config>\n\t<core><gap>4</gap></core>\n</labwc_config>\n");
  QTest::newRow("one line")<<QStringLiteral("<labwc_config><mouse><default /></mouse></labwc_config>");
 }
 void clickBindsWithoutDefaultsOrAMouseElement(){
  QFETCH(QString,rc);
  QString error;const auto with=BudgieClicks::withBinds(rc,&error);QVERIFY2(error.isEmpty(),qPrintable(error));
  QVERIFY(BudgieClicks::hasBinds(with));
  QCOMPARE(BudgieClicks::withoutBinds(with,&error),rc);QCOMPARE(BudgieClicks::withBinds(with,&error),with);
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
  Error replied{"unanswered","",{}};
  clicks.watchClicks({{"kind","monitor"}},[](QJsonObject){},[&](Error error,QJsonObject){replied=error;});
  QVERIFY2(!replied,qPrintable(replied.message));QVERIFY(BudgieClicks::hasBinds(read()));
  // Budgie's bridge writes its own tree back, without put.gd's binds: they return.
  write(clickRc);PUTGD_TRY_VERIFY(BudgieClicks::hasBinds(read()));
  clicks.unwatchClicks();QCOMPARE(read(),clickRc);
  // A bridge that read the file during the recording writes the binds back afterwards: they go again.
  write(BudgieClicks::withBinds(clickRc));PUTGD_TRY_COMPARE(read(),clickRc);
 }
};
QTEST_GUILESS_MAIN(LabwcBridgeTest)
#include "test_labwc_bridge.moc"
