#include "fixtures/U2Fixtures.h"
#include "fixtures/FakeScrollBackend.h"
#include "fixtures/Waits.h"
#include "app/LocalServer.h"
#include <QSignalSpy>
#include <QProcess>
#include <QJsonDocument>
using namespace putgd;using namespace putgd::test;
class ScrollTest:public QObject {
 Q_OBJECT
private slots:
 void lifecycle_data(){QTest::addColumn<QString>("bad");for(const auto& s:QStringList{"","hash","size","ihdr"})QTest::newRow(qPrintable(s.isEmpty()?"valid":s))<<s;}
 void lifecycle(){
 QFETCH(QString,bad);QTemporaryDir dir("/tmp/ps-XXXXXX");ProviderRegistry registry;SettingsService settings(dir.filePath("config.json"));auto doc=configuration(dir.path(),true);QVERIFY(settings.save(doc,{}));Source source;OperationService operations(dir.filePath("data"),settings,registry,source);HistoryService history(operations);LocalServer server(settings,operations,history);
 auto fake=std::make_unique<FakeScrollBackend>();auto* backend=fake.get();backend->corruption=bad;backend->heightCap=true;bool acceptedBeforeFrames=false;backend->onPrepare=[&](const QString& id){HistoryStore store(operations.databasePath());acceptedBeforeFrames=store.operation(id).id==id;};operations.setScrollBackend(std::move(fake));QVERIFY(waitFor([&]{return operations.ready();}));
 QVERIFY(dir.filePath("runtime/session.sock").toUtf8().size()<=107);QVERIFY(server.listen(dir.filePath("runtime")));
 auto call=[&](QString method,QJsonObject p=QJsonObject{}){QProcess client;client.start(PUTGD_CLI_PATH,{"--socket",server.socketPath(),method,QString::fromUtf8(QJsonDocument(p).toJson(QJsonDocument::Compact))});if(!waitFor([&]{return client.state()==QProcess::NotRunning;}))qFatal("CLI timed out");const auto response=QJsonDocument::fromJson(client.readAllStandardOutput()).object();const auto e=response["error"].toObject();return qMakePair(Error{e["code"].toString(),e["message"].toString(),e["details"].toObject()},response["result"].toObject());};
 QVERIFY(call("scroll.capabilities").second["available"].toBool());QVERIFY(call("scroll.prepare",{{"maxHeight",1}}).first);
 auto prepared=call("scroll.prepare",{{"rect",QJsonArray{0,0,64,64}}});QVERIFY2(!prepared.first,qPrintable(prepared.first.message));const auto id=prepared.second["id"].toString();QVERIFY(!id.isEmpty());QVERIFY(acceptedBeforeFrames);
 QCOMPARE(call("action.invoke",{{"action","region"}}).first.code,QString("scroll_active"));QCOMPARE(call("recording.prepare",{{"target",QJsonObject{{"kind","region"}}},{"format","mp4"}}).first.code,QString("scroll_active"));
 QVERIFY(!call("scroll.start",{{"id",id}}).first);QSignalSpy warning(&operations.scroll(),&ScrollCaptureService::warning);call("scroll.stop",{{"id",id}});auto result=call("scroll.inspect",{{"id",id}}).second;
 QCOMPARE(result["state"].toString(),bad.isEmpty()?QString("done"):QString("failed"));if(bad.isEmpty()){QCOMPARE(warning.size(),1);QVERIFY(result["note"].toString().contains("30,000"));QCOMPARE(result["endReason"].toString(),QString("height_cap"));QVERIFY(result["truncated"].toBool());HistoryStore store(operations.databasePath());QVERIFY(waitFor([&]{return !store.operation(id).items.isEmpty();}));QCOMPARE(store.operation(id).note,result["note"].toString());}
 auto next=call("scroll.prepare");const auto nextId=next.second["id"].toString();QVERIFY(!nextId.isEmpty());QVERIFY(!call("scroll.select",{{"id",nextId},{"rect",QJsonArray{0,0,64,64}}}).first);QVERIFY(!call("scroll.cancel",{{"id",nextId}}).first);QCOMPARE(call("scroll.inspect",{{"id",nextId}}).second["state"].toString(),QString("cancelled"));
 }
};
QTEST_GUILESS_MAIN(ScrollTest)
#include "test_scroll_service.moc"
