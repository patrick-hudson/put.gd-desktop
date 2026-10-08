#include "core/ClipboardPayload.h"
#include "core/ArtifactStore.h"
#include <QTemporaryDir>
#include <QMimeData>
#include <QFile>
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
 void batchAcquisitionIsAtomic() {
  QTemporaryDir d; QFile f(d.filePath("ok.txt")); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("synthetic"); f.close();
  ArtifactStore store(d.filePath("spool")); ClipboardPayload p; p.kind="files"; p.files={f.fileName(),d.filePath("missing.txt")};
  auto result=store.acquire("operation",p,ResourceLimits{}); QCOMPARE(result.error.code,QString("source_unavailable")); QVERIFY(result.artifacts.isEmpty());
  QVERIFY(!QDir(d.filePath("spool/operation")).exists()); QCOMPARE(result.error.details["itemIndex"].toInt(),1);
 }
};
QTEST_GUILESS_MAIN(PayloadTest)
#include "test_payload.moc"
