#include "core/UploadNaming.h"
#include <QTest>
using namespace putgd;
class NamingTest:public QObject {
 Q_OBJECT
private slots:
 void collisionsAndNames() {Artifact a;a.originalFile=true;a.name="report.pdf";a.mime="application/pdf";Artifact b=a;Artifact c=a;c.name="notes.txt";
 auto n=UploadNaming::batch({a,b,c},"original","overwrite");QCOMPARE(n[0].status,QString("conflict"));QCOMPARE(n[1].status,QString("conflict"));QCOMPARE(n[2].status,QString("pending"));
 n=UploadNaming::batch({a,b},"original","skip");QCOMPARE(n[0].status,QString("pending"));QCOMPARE(n[1].status,QString("skipped"));n=UploadNaming::batch({a,b},"original","rename");QVERIFY(n[0].name!=n[1].name);QVERIFY(n[1].name.endsWith(".pdf"));a.name=QString(400,QChar(0x4e2d))+".pdf";auto name=UploadNaming::candidate(a,"original",false);QVERIFY(name.toUtf8().size()<=255);QVERIFY(name.endsWith(".pdf"));a.name="Report.pdf";b.name="report.pdf";n=UploadNaming::batch({a,b},"original","overwrite");QCOMPARE(n[0].status,QString("pending"));QCOMPARE(n[1].status,QString("pending"));}
};
QTEST_GUILESS_MAIN(NamingTest)
#include "test_upload_naming.moc"
