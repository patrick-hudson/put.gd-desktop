#include "core/ClipboardPayload.h"
#include "core/ArtifactStore.h"
#include <QDir>
#include <QTemporaryDir>
#include <QMimeData>
#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <QTest>
using namespace putgd;
class PayloadTest: public QObject {
 Q_OBJECT
private slots:
 void precedenceAndLimits() {
  QTemporaryDir d; QFile f(d.filePath("fixture.txt")); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("fixture"); f.close();
  QMimeData mime; mime.setText("https://example.invalid"); mime.setUrls({QUrl::fromLocalFile(f.fileName())});
  auto p=ClipboardPayload::fromMime(mime, ResourceLimits{}); QVERIFY(!p.error); QCOMPARE(p.payload.kind,QString("files"));
  mime.setUrls({}); mime.setImageData(QImage(2,2,QImage::Format_RGB32));
  p=ClipboardPayload::fromMime(mime,ResourceLimits{}); QCOMPARE(p.payload.kind,QString("image"));
  QMimeData url;url.setUrls({QUrl("https://example.invalid/no-fetch")});url.setText("https://example.invalid/no-fetch");auto urlResult=ClipboardPayload::fromMime(url,ResourceLimits{});QVERIFY(!urlResult.error);QCOMPARE(urlResult.payload.kind,QString("text"));QCOMPARE(urlResult.payload.bytes,QByteArray("https://example.invalid/no-fetch"));
  QMimeData empty; QVERIFY(ClipboardPayload::fromMime(empty,ResourceLimits{}).error);
  ResourceLimits limits; limits.clipboardTextBytes=3; QMimeData text; text.setText("long"); QCOMPARE(ClipboardPayload::fromMime(text,limits).error.code,QString("input_excessive"));
 }
 void contentRoutingAndBoundedPrivatePreviews() {
  QTemporaryDir d;ArtifactStore store(d.filePath("spool"));ClipboardPayload image;image.kind="image";image.image=QImage(512,512,QImage::Format_RGB32);image.image.fill(Qt::red);auto acquired=store.acquire("image",image,ResourceLimits{});QVERIFY(!acquired.error);auto artifact=acquired.artifacts[0];QCOMPARE(artifact.category,QString("images"));const auto preview=store.preview(artifact,4096,1000000);QVERIFY(!preview.isEmpty());QImage thumbnail(preview);QVERIFY(thumbnail.width()<=256);QVERIFY(thumbnail.height()<=256);QVERIFY(store.preview(artifact,0,1000000).isEmpty());QVERIFY(store.preview(artifact,1,1000000).isEmpty());
  QFile movie(d.filePath("movie.bin"));QVERIFY(movie.open(QIODevice::WriteOnly));movie.write(QByteArray::fromHex("000000186674797069736f6d0000020069736f6d69736f32"));movie.close();ClipboardPayload files;files.kind="files";files.files={movie.fileName()};auto video=store.acquire("video",files,ResourceLimits{});QVERIFY(!video.error);QVERIFY(video.artifacts[0].mime.startsWith("video/"));QCOMPARE(video.artifacts[0].category,QString("videos"));store.clearPreviews();QVERIFY(!QFile::exists(preview));QVERIFY(!store.verify(artifact));
 }
 void pngPreviewKeepsBoundsAndPixels() {
  QTemporaryDir d;ArtifactStore store(d.filePath("spool"));ClipboardPayload image;image.kind="image";
  image.image=QImage(512,256,QImage::Format_RGB32);image.image.fill(Qt::red);
  auto acquired=store.acquire(QUuid::createUuid().toString(QUuid::WithoutBraces),image,ResourceLimits{});
  QVERIFY(!acquired.error);const auto artifact=acquired.artifacts.constFirst();
  QVERIFY(QFileInfo(artifact.path).suffix().isEmpty());QCOMPARE(artifact.mime,QString("image/png"));
  QVERIFY(store.preview(artifact,4096,512*256-1).isEmpty());
  const auto path=store.preview(artifact,4096,512*256);QVERIFY(!path.isEmpty());
  const QImage preview(path);QCOMPARE(preview.size(),QSize(256,128));
  QCOMPARE(preview.pixelColor(10,10),QColor(Qt::red));QVERIFY(!store.verify(artifact));
 }
 void previewUsesContentWhenMimeIsInaccurate() {
  QTemporaryDir d;ArtifactStore store(d.filePath("spool"));
  const auto input=d.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces));
  QImage image(32,16,QImage::Format_RGB32);image.fill(Qt::blue);QVERIFY(image.save(input,"BMP"));
  Artifact artifact;artifact.id=QUuid::createUuid().toString(QUuid::WithoutBraces);
  artifact.path=input;artifact.category="images";artifact.mime="image/png";
  const auto path=store.preview(artifact,4096,32*16);QVERIFY(!path.isEmpty());
  const QImage preview(path);QCOMPARE(preview.size(),QSize(256,128));
  QCOMPARE(preview.pixelColor(10,10),QColor(Qt::blue));
 }
 void destinationMaximumAndFreeDiskAreCheckedBeforeCopying() {
  QTemporaryDir d;
  auto write=[&](const QString& name,qint64 size){QFile f(d.filePath(name));if(!f.open(QIODevice::WriteOnly))return QString();f.write(QByteArray(size,'s'));return f.fileName();};
  ClipboardPayload p;p.kind="files";p.files={write("small.bin",10),write("large.bin",30)};
  ArtifactStore store(d.filePath("spool"));
  auto result=store.acquire("operation",p,ResourceLimits{},"clipboard",Settings::fromValidated(Settings::defaults()),{},20);
  QCOMPARE(result.error.code,QString("destination_object_too_large"));QCOMPARE(result.error.details["itemIndex"].toInt(),1);
  QVERIFY(result.error.message.contains("large.bin"));QVERIFY(result.artifacts.isEmpty());
  QVERIFY(QDir(d.filePath("spool")).entryList(QDir::AllEntries|QDir::Hidden|QDir::NoDotAndDotDot).isEmpty());
  // The reserve is min(5%, 4 GB), at least 1 GB (#213).
  constexpr qint64 gib=1LL<<30;
  QCOMPARE(ArtifactStore::reserveFor(100*gib),4*gib);QCOMPARE(ArtifactStore::reserveFor(40*gib),2*gib);QCOMPARE(ArtifactStore::reserveFor(20*gib),gib);
  QCOMPARE(ArtifactStore::reserveFor(4*gib),gib);QCOMPARE(ArtifactStore::reserveFor(-1),gib);
  // Free space must hold the whole batch plus the reserve, in every limit mode.
  ArtifactStore::setDiskForTesting(gib+39,4*gib);
  result=store.acquire("operation",p,ResourceLimits{});
  QCOMPARE(result.error.code,QString("disk_space_insufficient"));QCOMPARE(result.error.details["requiredBytes"].toInteger(),40LL);
  QCOMPARE(result.error.details["reserveBytes"].toInteger(),gib);QVERIFY(result.error.message.contains("kept free for the system"));
  QVERIFY(QDir(d.filePath("spool")).entryList(QDir::AllEntries|QDir::Hidden|QDir::NoDotAndDotDot).isEmpty());
  ArtifactStore::setDiskForTesting(gib+40,4*gib);
  result=store.acquire("operation",p,ResourceLimits{});QVERIFY2(!result.error,qPrintable(result.error.message));QCOMPARE(result.artifacts.size(),2);
  // Bytes held by an import in flight are not free for a concurrent one.
  {ArtifactStore::DiskBudget seen;const auto held=ArtifactStore::reserveDisk(d.filePath("spool"),30,&seen);QVERIFY(held);
   QVERIFY(!ArtifactStore::reserveDisk(d.filePath("spool"),11,&seen));QCOMPARE(seen.reservedBytes,30LL);
   result=store.acquire("concurrent",p,ResourceLimits{});QCOMPARE(result.error.code,QString("disk_space_insufficient"));}
  // No bytes stay reserved once an import has finished.
  QCOMPARE(ArtifactStore::diskBudget(d.filePath("spool")).reservedBytes,0LL);
  QCOMPARE(ArtifactStore::diskBudget(d.filePath("spool")).available(),40LL);
  ArtifactStore::setDiskForTesting(-1,-1);
  const auto real=ArtifactStore::diskBudget(d.filePath("spool"));QVERIFY(real.freeBytes>0&&real.totalBytes>0&&real.reserveBytes>=gib);
  // Each budget names the limit that refused the file.
  ResourceLimits small;small.maxFileBytes=20;
  result=store.acquire("other",p,small);QCOMPARE(result.error.code,QString("input_excessive"));QVERIFY(result.error.message.contains("Largest file"));
 }
 void batchAcquisitionIsAtomic() {
  QTemporaryDir d; QFile f(d.filePath("ok.txt")); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("synthetic"); f.close();
  ArtifactStore store(d.filePath("spool")); ClipboardPayload p; p.kind="files"; p.files={f.fileName(),d.filePath("missing.txt")};
  auto result=store.acquire("operation",p,ResourceLimits{}); QCOMPARE(result.error.code,QString("source_unavailable")); QVERIFY(result.artifacts.isEmpty());
  QVERIFY(!QDir(d.filePath("spool/operation")).exists()); QCOMPARE(result.error.details["itemIndex"].toInt(),1);
 }
};
QTEST_GUILESS_MAIN(PayloadTest)
#include "test_payload.moc"
