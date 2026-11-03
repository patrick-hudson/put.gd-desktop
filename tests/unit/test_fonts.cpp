#include "ui/Fonts.h"
#include <QtTest>
#include <QTemporaryDir>
using namespace putgd;
// The interface, editor and recording processes map the installed font files
// (shared page cache) and fall back to the resource copies, which cost each
// process a private heap copy, only when the installed directory is missing.
class FontsContract:public QObject {
 Q_OBJECT
private slots:
 void installedDirectoryBesideBin(){
  QTemporaryDir root;QVERIFY(root.isValid());
  const QDir prefix(root.path());
  QVERIFY(prefix.mkpath("bin"));
  QCOMPARE(Fonts::installedDirectory(prefix.filePath("bin")),QString());
  QVERIFY(prefix.mkpath("share/putgd/fonts"));
  QCOMPARE(Fonts::installedDirectory(prefix.filePath("bin")),QDir::cleanPath(prefix.filePath("share/putgd/fonts")));
 }
 void sourcePrefersInstalledFile(){
  QTemporaryDir root;QVERIFY(root.isValid());
  const QString name="IBMPlexSans-Regular.ttf";
  QCOMPARE(Fonts::source(QString(),name),QString(":/fonts/")+name);
  QCOMPARE(Fonts::source(root.path(),name),QString(":/fonts/")+name); // not installed there
  QFile file(QDir(root.path()).filePath(name));QVERIFY(file.open(QIODevice::WriteOnly));file.write("not a font");file.close();
  QCOMPARE(Fonts::source(root.path(),name),QFileInfo(file).absoluteFilePath());
  QCOMPARE(Fonts::source(root.path(),"IBMPlexMono-Regular.ttf"),QString(":/fonts/IBMPlexMono-Regular.ttf"));
 }
 void resourceCopiesExist(){
  for(const char* name:{"IBMPlexSans-Regular.ttf","IBMPlexSans-Medium.ttf","IBMPlexSans-SemiBold.ttf","IBMPlexMono-Regular.ttf"})
   QVERIFY2(QFile::exists(Fonts::source(QString(),name)),name);
 }
};
QTEST_GUILESS_MAIN(FontsContract)
#include "test_fonts.moc"
