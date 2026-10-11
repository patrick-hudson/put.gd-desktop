#include "fixtures/U2Fixtures.h"
#include <QSaveFile>
#include <QSignalSpy>
using namespace putgd;using namespace putgd::test;
class ReloadContract:public QObject {
 Q_OBJECT
private slots:
 void replacementsAndValidationHaveNoSideEffects() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});auto doc=configuration(d.path());QVERIFY(settings.save(doc,{}));Source source;OperationService operations(d.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());QSignalSpy accepted(&settings,&SettingsService::accepted);
  for(int index=0;index<3;++index) {auto preferences=doc["preferences"].toObject();preferences["notifications"]=index%2==1;doc["preferences"]=preferences;QSaveFile file(settings.path());file.setDirectWriteFallback(false);QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(doc).toJson());QVERIFY(file.commit());QTRY_COMPARE(accepted.count(),index+1);QCOMPARE(settings.document(),doc);}
  QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);QCOMPARE(provider->uploads.load(),0);QCOMPARE(provider->deletes.load(),0);QTest::qWait(250);QCOMPARE(accepted.count(),3);
 }
};
QTEST_GUILESS_MAIN(ReloadContract)
#include "test_config_reload.moc"
