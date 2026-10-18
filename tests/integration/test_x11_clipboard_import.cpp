#include "platform/x11/X11Clipboard.h"
#include "platform/x11/X11ClipboardFormats.h"
#include "../fixtures/X11SelectionFixture.h"
#include <QtTest>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <memory>
using namespace putgd;
namespace {
// Image plugins whose code is mapped into this process; dlopen maps an
// executable segment. Nothing here encodes or decodes with Qt image codecs,
// so any plugin present was loaded by the importer under test.
QStringList loadedImagePlugins(){
 QFile maps("/proc/self/maps");if(!maps.open(QIODevice::ReadOnly))return {"/proc/self/maps unreadable"};
 QStringList loaded;
 for(const auto& line:maps.readAll().split('\n')){
  const auto fields=line.simplified().split(' ');
  if(fields.size()>=6&&fields[1].contains('x')&&fields[5].contains("/imageformats/")){const auto path=QString::fromLocal8Bit(fields[5]);if(!loaded.contains(path))loaded.append(path);}
 }
 return loaded;
}
// 3x2 RGB images: red, green, blue over yellow, cyan, magenta. Encoded by
// hand so this process never asks Qt for an image writer.
const QByteArray png=QByteArray::fromHex("89504e470d0a1a0a0000000d49484452000000030000000208020000001216f14d000000154944415478da63f8cfc0c000c1ff81d47f20f11f004ac908f85fb37d750000000049454e44ae426082");
const QByteArray bmp=QByteArray::fromHex("424d4e0000000000000036000000280000000300000002000000010018000000000018000000130b0000130b0000000000000000000000ffffffff00ff00ff0000000000ff00ff00ff0000000000");
// Running image jobs: putgd-ui children of this process.
QList<qint64> imageJobs(){
 QList<qint64> jobs;
 for(const auto& entry:QDir("/proc").entryList(QDir::Dirs)){
  bool number=false;const auto pid=entry.toLongLong(&number);if(!number)continue;
  QFile stat("/proc/"+entry+"/stat");if(!stat.open(QIODevice::ReadOnly))continue;
  const auto line=stat.readAll();const auto open=line.indexOf('('),close=line.lastIndexOf(')');if(open<0||close<open)continue;
  const auto fields=line.mid(close+2).split(' ');
  if(line.mid(open+1,close-open-1)=="putgd-ui"&&fields.size()>1&&fields[1].toLongLong()==QCoreApplication::applicationPid())jobs.append(pid);
 }
 return jobs;
}
bool importInto(X11Clipboard& importer,PayloadResult& result){
 bool done=false;importer.acquire({},[&](PayloadResult value){result=std::move(value);done=true;});
 return QTest::qWaitFor([&]{return done;},20000);
}
bool expectedPixels(const QImage& image){
 return image.size()==QSize(3,2)&&image.pixelColor(0,0)==QColor(Qt::red)&&image.pixelColor(1,0)==QColor(Qt::green)&&image.pixelColor(2,0)==QColor(Qt::blue)
  &&image.pixelColor(0,1)==QColor(Qt::yellow)&&image.pixelColor(1,1)==QColor(Qt::cyan)&&image.pixelColor(2,1)==QColor(Qt::magenta);
}
}
class X11ClipboardImportTest final:public QObject {
 Q_OBJECT
private slots:
 void initTestCase(){
  // A synthetic plugin is always installed, so an in-process plugin load is
  // observable even where the system ships no image plugins. The transient
  // image job inherits QT_PLUGIN_PATH and lists its MIME types.
  QCoreApplication::addLibraryPath(QStringLiteral(PUTGD_IMAGE_PLUGIN_FIXTURE_DIR));
  qputenv("QT_PLUGIN_PATH",PUTGD_IMAGE_PLUGIN_FIXTURE_DIR);
  QCOMPARE(loadedImagePlugins(),QStringList{});
 }
 void cleanup(){QCOMPARE(loadedImagePlugins(),QStringList{});}
 void standardOffersSkipImageDiscovery(){
  int discoveries=0;const auto discover=[&]{++discoveries;return QList<QByteArray>{"image/jpeg","application/x-plugin-image"};};
  const QList<QByteArray> text{"TARGETS","TIMESTAMP","MULTIPLE","SAVE_TARGETS","STRING","TEXT","COMPOUND_TEXT","text/plain","UTF8_STRING","text/plain;charset=utf-8"};
  QCOMPARE(x11ClipboardImportTargets(text,discover),(QList<QByteArray>{"UTF8_STRING","text/plain;charset=utf-8","text/plain"}));
  QCOMPARE(x11ClipboardImportTargets({"TARGETS","UTF8_STRING","text/uri-list"},discover),(QList<QByteArray>{"text/uri-list","UTF8_STRING"}));
  QVERIFY(x11ClipboardImportTargets({},discover).isEmpty());
  QCOMPARE(discoveries,0);
 }
 void pngPrioritySkipsImageDiscovery(){
  int discoveries=0;const auto discover=[&]{++discoveries;return QList<QByteArray>{"image/jpeg","image/png"};};
  const QList<QByteArray> offered{"TARGETS","image/jpeg","text/html","application/x-qt-image","image/png","text/uri-list","text/plain"};
  // Only a URI list without local files advances, so other images were never reached.
  QCOMPARE(x11ClipboardImportTargets(offered,discover),(QList<QByteArray>{"text/uri-list","image/png","text/plain"}));
  QCOMPARE(discoveries,0);
 }
 void unknownMimeOffersKeepDiscoveryOrder(){
  int discoveries=0;const auto discover=[&]{++discoveries;return QList<QByteArray>{"image/webp","application/x-plugin-image","image/bmp","image/png"};};
  const QList<QByteArray> offered{"TARGETS","image/bmp","STRING","application/x-plugin-image","UTF8_STRING"};
  QCOMPARE(x11ClipboardImportTargets(offered,discover),(QList<QByteArray>{"application/x-plugin-image","image/bmp","UTF8_STRING"}));QCOMPARE(discoveries,1);
  QCOMPARE(x11ClipboardImportTargets({"text/html","UTF8_STRING"},discover),(QList<QByteArray>{"UTF8_STRING"}));QCOMPARE(discoveries,2);
 }
 void pngDecodesInProcessWithoutPlugins(){
  X11SelectionFixture owner({{"TIMESTAMP",{}},{"SAVE_TARGETS",{}},{"image/bmp",bmp},{"image/png",png},{"UTF8_STRING","synthetic caption"}});
  X11Clipboard importer;PayloadResult result;QVERIFY(importInto(importer,result));QVERIFY2(!result.error,qPrintable(result.error.code));
  QCOMPARE(result.payload.kind,QString("image"));QVERIFY(expectedPixels(result.payload.image));
  QCOMPARE(owner.requested,QList<QByteArray>{"image/png"});
 }
 void textOffersImportWithoutImageWork(){
  X11SelectionFixture owner({{"TIMESTAMP",{}},{"MULTIPLE",{}},{"SAVE_TARGETS",{}},{"STRING","synthetic text"},{"TEXT","synthetic text"},{"COMPOUND_TEXT","synthetic text"},{"UTF8_STRING","synthetic text"},{"text/plain","synthetic text"}});
  X11Clipboard importer;PayloadResult result;QVERIFY(importInto(importer,result));QVERIFY2(!result.error,qPrintable(result.error.code));
  QCOMPARE(result.payload.bytes,QByteArray("synthetic text"));QCOMPARE(owner.requested,QList<QByteArray>{"UTF8_STRING"});
 }
 void otherImageFormatsDecodeInImageJob(){
  X11Clipboard importer;
  {X11SelectionFixture owner({{"image/bmp",bmp},{"text/plain","synthetic caption"}},true);PayloadResult result;QVERIFY(importInto(importer,result));QVERIFY2(!result.error,qPrintable(result.error.code));
   QVERIFY(expectedPixels(result.payload.image));QCOMPARE(owner.requested,QList<QByteArray>{"image/bmp"});QVERIFY(owner.incrementalTransfers>0);}
  // A claimed PNG keeps Qt's content detection, out of process.
  {X11SelectionFixture owner({{"image/png",bmp}});PayloadResult result;QVERIFY(importInto(importer,result));QVERIFY2(!result.error,qPrintable(result.error.code));
   QVERIFY(expectedPixels(result.payload.image));QCOMPARE(owner.requested,QList<QByteArray>{"image/png"});}
 }
 void discoveredPluginTypesComeFromImageJob(){
  X11Clipboard importer;
  // Only the image job knows the synthetic plugin's type. Its handler cannot
  // decode, and an image error is terminal rather than falling back to text.
  {X11SelectionFixture owner({{"image/x-putgd-gnome-mime","synthetic"},{"UTF8_STRING","synthetic fallback"}});PayloadResult result;QVERIFY(importInto(importer,result));
   QCOMPARE(result.error.code,QString("source_unsupported"));QCOMPARE(owner.requested,QList<QByteArray>{"image/x-putgd-gnome-mime"});}
  {X11SelectionFixture owner({{"application/x-putgd-unlisted","synthetic"},{"UTF8_STRING","synthetic fallback"}});PayloadResult result;QVERIFY(importInto(importer,result));QVERIFY2(!result.error,qPrintable(result.error.code));
   QCOMPARE(result.payload.bytes,QByteArray("synthetic fallback"));QCOMPARE(owner.requested,QList<QByteArray>{"UTF8_STRING"});}
 }
 void cancellationStopsImageDiscovery(){
  // The slow synthetic plugin holds discovery in the image job for three
  // seconds, so cancellation always arrives while that child runs.
  const auto plugins=qgetenv("QT_PLUGIN_PATH");auto restore=qScopeGuard([&]{qputenv("QT_PLUGIN_PATH",plugins);});
  qputenv("QT_PLUGIN_PATH",QByteArray(PUTGD_IMAGE_PLUGIN_FIXTURE_DIR ":" PUTGD_SLOW_PLUGIN_FIXTURE_DIR));
  X11SelectionFixture owner({{"image/bmp",bmp}});
  auto importer=std::make_unique<X11Clipboard>();bool done=false;PayloadResult result;
  importer->acquire({},[&](PayloadResult value){result=std::move(value);done=true;});
  QTRY_VERIFY(!imageJobs().isEmpty());QVERIFY(!done);importer->cancel();
  QTRY_VERIFY(done);QCOMPARE(result.error.code,QString("cancelled"));
  // Destruction waits for workers; a cancelled image job must stop promptly.
  QElapsedTimer clock;clock.start();importer.reset();QVERIFY2(clock.elapsed()<2500,qPrintable(QString::number(clock.elapsed())));
  QCOMPARE(imageJobs(),QList<qint64>{});QVERIFY(owner.requested.isEmpty());
 }
};
QTEST_GUILESS_MAIN(X11ClipboardImportTest)
#include "test_x11_clipboard_import.moc"
