#include "fixtures/U2Fixtures.h"
#include <QSaveFile>
#include <QSignalSpy>
#include "fixtures/Waits.h"
using namespace putgd;using namespace putgd::test;
class ReloadContract:public QObject {
 Q_OBJECT
private slots:
 void uploadClipboardDefaultsAndLegacyConfiguration() {
  auto doc=Settings::defaults();
  QCOMPARE(doc["preferences"].toObject()["uploadClipboardBehavior"].toString(),QString("each_link"));
  auto preferences=doc["preferences"].toObject();preferences.remove("uploadClipboardBehavior");doc["preferences"]=preferences;
  QVERIFY(!Settings::validate(doc));QTemporaryDir d;SettingsService settings(d.filePath("config.json"));
  QVERIFY(settings.save(doc,{}));QCOMPARE(settings.document(),doc); // Loading does not rewrite legacy bytes.
 }
 void uploadClipboardModeValidatesAndReloads() {
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));auto doc=Settings::defaults();
  auto preferences=doc["preferences"].toObject();preferences["uploadClipboardBehavior"]="list";doc["preferences"]=preferences;
  QVERIFY(!Settings::validate(doc));QVERIFY(settings.save(doc,{}));
  QCOMPARE(settings.document()["preferences"].toObject()["uploadClipboardBehavior"].toString(),QString("list"));
  for(const auto& invalid:QList<QJsonValue>{"latest",true,42,QJsonValue::Null}) {
   auto rejected=doc;auto prefs=rejected["preferences"].toObject();prefs["uploadClipboardBehavior"]=invalid;rejected["preferences"]=prefs;
   QCOMPARE(Settings::validate(rejected).code,QString("config_invalid"));
  }
 }
 void replacementsAndValidationHaveNoSideEffects() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});auto doc=configuration(d.path());QVERIFY(settings.save(doc,{}));Source source;OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());QSignalSpy accepted(&settings,&SettingsService::accepted);
  for(int index=0;index<3;++index) {auto preferences=doc["preferences"].toObject();preferences["notifications"]=index%2==1;doc["preferences"]=preferences;QSaveFile file(settings.path());file.setDirectWriteFallback(false);QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(doc).toJson());QVERIFY(file.commit());PUTGD_TRY_COMPARE(accepted.count(),index+1);QCOMPARE(settings.document(),doc);}
  QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);QCOMPARE(provider->uploads.load(),0);QCOMPARE(provider->deletes.load(),0);putgd::test::settle(250);PUTGD_TRY_COMPARE(accepted.count(),3);
 }
};
QTEST_GUILESS_MAIN(ReloadContract)
#include "test_config_reload.moc"
