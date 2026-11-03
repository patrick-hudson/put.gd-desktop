#include "ui/Fonts.h"
#include <QtTest>
#include <QTemporaryDir>
#ifndef PUTGD_INSTALL_DATADIR
#define PUTGD_INSTALL_DATADIR "share"
#endif
using namespace putgd;
// The interface, editor and recording processes map the installed font files
// (shared page cache) and fall back to the resource copies, which cost each
// process a private heap copy, only when the installed directory is missing.
class FontsContract:public QObject {
 Q_OBJECT
private slots:
 // putgd_fonts is a static library: register its resources explicitly rather
 // than relying on the linker pulling in the .qrc object's initializer.
 void initTestCase(){Q_INIT_RESOURCE(fonts);}
 void relativeDataDirectoryBesideBin(){
  QTemporaryDir root;QVERIFY(root.isValid());
  const QDir prefix(root.path());
  QVERIFY(prefix.mkpath("bin"));
  QCOMPARE(Fonts::installedDirectory(prefix.filePath("bin"),"share"),QString());
  QVERIFY(prefix.mkpath("share/putgd/fonts"));
  QCOMPARE(Fonts::installedDirectory(prefix.filePath("bin"),"share"),QDir::cleanPath(prefix.filePath("share/putgd/fonts")));
  // A non-default relative data directory resolves beside bin/ the same way.
  QCOMPARE(Fonts::installedDirectory(prefix.filePath("bin"),"data"),QString());
  QVERIFY(prefix.mkpath("data/putgd/fonts"));
  QCOMPARE(Fonts::installedDirectory(prefix.filePath("bin"),"data"),QDir::cleanPath(prefix.filePath("data/putgd/fonts")));
 }
 void absoluteDataDirectoryAsGiven(){
  QTemporaryDir root,elsewhere;QVERIFY(root.isValid());QVERIFY(elsewhere.isValid());
  const QDir prefix(root.path());QVERIFY(prefix.mkpath("bin"));
  QCOMPARE(Fonts::installedDirectory(prefix.filePath("bin"),elsewhere.path()),QString());
  QVERIFY(QDir(elsewhere.path()).mkpath("putgd/fonts"));
  QCOMPARE(Fonts::installedDirectory(prefix.filePath("bin"),elsewhere.path()),QDir::cleanPath(QDir(elsewhere.path()).filePath("putgd/fonts")));
 }
 void configuredDataDirectoryIsTheDefault(){
  // The one-argument form uses the data directory this build was configured
  // with, whatever CMAKE_INSTALL_DATADIR was.
  QTemporaryDir root;QVERIFY(root.isValid());
  const QDir prefix(root.path());QVERIFY(prefix.mkpath("bin"));
  const QString bin=prefix.filePath("bin");
  QCOMPARE(Fonts::installedDirectory(bin),Fonts::installedDirectory(bin,QStringLiteral(PUTGD_INSTALL_DATADIR)));
  if(!QDir::isAbsolutePath(QStringLiteral(PUTGD_INSTALL_DATADIR))){
   QVERIFY(prefix.mkpath(QStringLiteral(PUTGD_INSTALL_DATADIR)+"/putgd/fonts"));
   QCOMPARE(Fonts::installedDirectory(bin),QDir::cleanPath(prefix.filePath(QStringLiteral(PUTGD_INSTALL_DATADIR)+"/putgd/fonts")));
  }
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
