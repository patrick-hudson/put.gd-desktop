#include "core/UploadNaming.h"
#include <QTest>
#include <QSet>
using namespace putgd;
class NamingTest:public QObject {
 Q_OBJECT
private slots:
 void tinyProviderBoundsRemainValidAndCannotLoopOnCollisions() {
  Artifact file;file.name="report.png";file.originalFile=true;
  const auto name=UploadNaming::candidate(file,"original",false,1);
  QVERIFY(name!=".");QVERIFY(!name.isEmpty());QVERIFY(name.toUtf8().size()<=1);
  QList<Artifact> artifacts;for(int index=0;index<64;++index)artifacts.append(file);
  const auto batch=UploadNaming::batch(artifacts,"original","rename",1);
  QCOMPARE(batch.size(),64);QSet<QString> pending;int conflicts=0;
  for(const auto& item:batch) {
   QVERIFY(!item.name.isEmpty());QVERIFY(item.name!=".");QVERIFY(item.name.toUtf8().size()<=1);
   if(item.status=="pending") {QVERIFY(!pending.contains(item.name));pending.insert(item.name);}
   else {QCOMPARE(item.status,QString("conflict"));++conflicts;}
  }
  QVERIFY(conflicts>0);
 }
 void templateTokensHaveExactMeaning() {
  Artifact file;file.name="report.final.pdf";file.originalFile=true;file.id="b76d3ddb-7c66-4de5-b0dd-7400f2b20d96";
  const auto acquired=QDateTime(QDate(2026,10,8),QTime(4,27,21));
  const auto rendered=UploadNaming::renderTemplate("{name}-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}-{uuid}.{ext}",file,acquired,file.id);
  QVERIFY(!rendered.error);
  QCOMPARE(rendered.name,QString("report.final-2026-10-08-04-27-21-b76d3ddb-7c66-4de5-b0dd-7400f2b20d96.pdf"));
  auto settings=Settings::fromValidated(Settings::defaults());
  QCOMPARE(UploadNaming::acquiredName(file,settings,"clipboard",acquired).name,file.name);
  settings.namingMode="template";settings.fileTemplate="Invoice-{name}-{yyyy}.{ext}";
  QCOMPARE(UploadNaming::acquiredName(file,settings,"clipboard",acquired).name,QString("Invoice-report.final-2026.pdf"));
  file.originalFile=false;file.name="image.png";file.mime="image/png";
  QCOMPARE(UploadNaming::acquiredName(file,settings,"region",acquired).name,QString("Screen-Capture-2026-10-08-04-27-21.png"));
  QCOMPARE(UploadNaming::acquiredName(file,settings,"window",acquired).name,QString("Screen-Capture-2026-10-08-04-27-21.png"));
  QCOMPARE(UploadNaming::acquiredName(file,settings,"desktop",acquired).name,QString("Screen-Capture-2026-10-08-04-27-21.png"));
  QCOMPARE(UploadNaming::acquiredName(file,settings,"clipboard",acquired).name,QString("Image-2026-10-08-04-27-21.png"));
  settings.captureTemplate="Capture-{uuid}.{ext}";
  QCOMPARE(UploadNaming::acquiredName(file,settings,"region",acquired).name,QString("Capture-b76d3ddb-7c66-4de5-b0dd-7400f2b20d96.png"));
  file.name="text.txt";file.mime="text/plain";
  QCOMPARE(UploadNaming::acquiredName(file,settings,"clipboard",acquired).name,QString("Text-2026-10-08-04-27-21.txt"));
  settings.namingMode="generated";
  auto random=UploadNaming::acquiredName(file,settings,"clipboard",acquired);
  QCOMPARE(random.name,QString("b76d3ddb-7c66-4de5-b0dd-7400f2b20d96.txt"));
  file.name=random.name;file.namingPrepared=true;
  QCOMPARE(UploadNaming::candidate(file,"generated"),random.name);
  QCOMPARE(UploadNaming::candidate(Artifact::fromJson(file.toJson()),"generated"),random.name);
  file.name="README";file.originalFile=true;
  QCOMPARE(UploadNaming::acquiredName(file,settings,"clipboard",acquired).name,file.id);
 }
 void unsafeTemplatesAndEmptyResultsFail() {
  Artifact file;file.name="README";
  const auto acquired=QDateTime(QDate(2026,10,8),QTime(4,27,21));
  for(const auto& format:QStringList{"{wat}","{name","name}","{name}/x","{name}\\x",QString(QChar(0))+"x","line\n{name}","",".",".."})
   QVERIFY(UploadNaming::renderTemplate(format,file,acquired).error);
  QVERIFY(UploadNaming::renderTemplate("{ext}",file,acquired).error);
  auto settings=Settings::fromValidated(Settings::defaults());file.originalFile=true;file.name="unsafe\nname.txt";
  QVERIFY(UploadNaming::acquiredName(file,settings,"clipboard",acquired).error);
 }
 void confirmedCollisionKeepsPreparedPrefixAndByteBound() {
  const auto original=QString("Capture-2026-10-08-04-27-21.png");
  const auto renamed=UploadNaming::renamedCandidate(original);
  QVERIFY(renamed.startsWith("Capture-2026-10-08-04-27-21-"));QVERIFY(renamed.endsWith(".png"));QVERIFY(renamed!=original);
  const auto unicode=QString::fromUcs4(U"\U0001f680");Artifact file;file.name=unicode.repeated(100)+".png";file.namingPrepared=true;
  const auto bounded=UploadNaming::candidate(file,"original",false,64);QVERIFY(bounded.toUtf8().size()<=64);QVERIFY(bounded.endsWith(".png"));QVERIFY(!bounded.contains(QChar::ReplacementCharacter));
 }
 void originalModeKeepsAcquiredCaptureName() {
  Artifact capture;capture.name="Screen-Capture-2026-10-08-04-27-21.png";capture.mime="image/png";
  QCOMPARE(UploadNaming::candidate(capture,"original"),capture.name);
 }
 void collisionsAndNames() {Artifact a;a.originalFile=true;a.name="report.pdf";a.mime="application/pdf";Artifact b=a;Artifact c=a;c.name="notes.txt";
 auto n=UploadNaming::batch({a,b,c},"original","overwrite");QCOMPARE(n[0].status,QString("conflict"));QCOMPARE(n[1].status,QString("conflict"));QCOMPARE(n[2].status,QString("pending"));
 n=UploadNaming::batch({a,b},"original","skip");QCOMPARE(n[0].status,QString("pending"));QCOMPARE(n[1].status,QString("skipped"));n=UploadNaming::batch({a,b},"original","rename");QVERIFY(n[0].name!=n[1].name);QVERIFY(n[1].name.endsWith(".pdf"));a.name=QString(400,QChar(0x4e2d))+".pdf";auto name=UploadNaming::candidate(a,"original",false);QVERIFY(name.toUtf8().size()<=255);QVERIFY(name.endsWith(".pdf"));a.name="Report.pdf";b.name="report.pdf";n=UploadNaming::batch({a,b},"original","overwrite");QCOMPARE(n[0].status,QString("pending"));QCOMPARE(n[1].status,QString("pending"));}
};
QTEST_GUILESS_MAIN(NamingTest)
#include "test_upload_naming.moc"
