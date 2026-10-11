#include "core/LocalArchive.h"
#include <QTemporaryDir>
#include <QFile>
#include <QTest>
using namespace putgd;
class ArchiveTest:public QObject {
 Q_OBJECT
private slots:
 void copiesAreIndependentAndUnique() {QTemporaryDir d;ArtifactStore store(d.filePath("spool"));ClipboardPayload p;p.kind="text";p.bytes="synthetic immutable";auto a=store.acquire("job",p,ResourceLimits{});QVERIFY(!a.error);a.artifacts[0].name="Text-2026-10-08-04-27-21.txt";ArchiveSettings s;s.root=d.filePath("archive");auto first=LocalArchive::save(a.artifacts[0],s),second=LocalArchive::save(a.artifacts[0],s);QCOMPARE(first.status,QString("saved"));QCOMPARE(first.path,d.filePath("archive/text/Text-2026-10-08-04-27-21.txt"));QVERIFY(first.path!=second.path);QVERIFY(first.path.contains("/text/"));QFile f(first.path);QVERIFY(f.open(QIODevice::ReadOnly));QCOMPARE(f.readAll(),QByteArray("synthetic immutable"));f.close();QVERIFY(f.open(QIODevice::WriteOnly|QIODevice::Truncate));f.write("edited");f.close();QVERIFY(!store.verify(a.artifacts[0]));QVERIFY(QFile::exists(second.path));}
 void overridesAndFailure() {ArchiveSettings s;s.root="/default";s.folders["videos"]="/override";QCOMPARE(LocalArchive::folder(s,"videos"),QString("/override"));QCOMPARE(LocalArchive::folder(s,"images"),QString("/default/images"));QTemporaryDir d;QFile f(d.filePath("blocked"));QVERIFY(f.open(QIODevice::WriteOnly));f.close();s.root=f.fileName();Artifact a;a.path=f.fileName();a.category="other";a.name="a.bin";QCOMPARE(LocalArchive::save(a,s).status,QString("failed"));}
};
QTEST_GUILESS_MAIN(ArchiveTest)
#include "test_archive.moc"
