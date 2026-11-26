#include "seal/SealCrypto.h"
#include "seal/SealPage.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
using namespace putgd;
namespace {
const QString vectorsPath=QStringLiteral("tests/fixtures/seal/vectors.json");
QByteArray content(const QJsonObject& spec) {
 if(spec.contains("text"))return spec["text"].toString().toUtf8();
 if(spec.contains("hex"))return QByteArray::fromHex(spec["hex"].toString().toLatin1());
 const qint64 size=qint64(spec["pattern"].toDouble());QByteArray out(size,'\0');
 for(qint64 i=0;i<size;++i)out[i]=char((i*31+7)%251);
 return out;
}
struct Vector {QJsonObject json;QByteArray plain,key;seal::Fixed fixed;seal::Meta meta;QString passphrase;int iterations=0;};
Vector vector(const QJsonObject& json) {
 Vector v;v.json=json;v.plain=content(json["content"].toObject());v.key=QByteArray::fromHex(json["link"].toString().toLatin1());
 v.fixed={QByteArray::fromHex(json["salt"].toString().toLatin1()),QByteArray::fromHex(json["noncePrefix"].toString().toLatin1())};
 v.meta={json["name"].toString(),json["type"].toString(),v.plain.size()};v.passphrase=json["passphrase"].toString();v.iterations=json["iterations"].toInt();
 return v;
}
QJsonObject document() {QFile file(vectorsPath);if(!file.open(QIODevice::ReadOnly))return {};return QJsonDocument::fromJson(file.readAll()).object();}
QByteArray sealed(const Vector& v) {
 QBuffer input;input.setData(v.plain);input.open(QIODevice::ReadOnly);QByteArray out;
 const auto error=seal::seal(input,v.meta,v.key,v.passphrase,[&out](const char* d,qint64 n){out.append(d,n);return true;},v.iterations?v.iterations:seal::passphraseIterations,v.fixed);
 return error?QByteArray():out;
}
Error opened(const QByteArray& container,const QByteArray& key,const QString& passphrase,QByteArray* plain=nullptr,seal::Meta* meta=nullptr) {
 QBuffer input;input.setData(container);input.open(QIODevice::ReadOnly);QByteArray out;
 const auto error=seal::open(input,container.size(),key,passphrase,[&out](const char* d,qint64 n){out.append(d,n);return true;},meta);
 if(plain)*plain=out;return error;
}
Vector named(const QString& id) {for(const auto& entry:document()["vectors"].toArray())if(entry.toObject()["id"]==id)return vector(entry.toObject());return {};}
constexpr qint64 full=seal::chunkBytes+seal::tagBytes;
}
// Sealed links (#2079): the container format against fixed vectors, every way a
// container can be changed, and the pinned viewer page.
class SealTest:public QObject {
 Q_OBJECT
private slots:
 // Each vector seals to the same bytes every time and opens to its file and description.
 // PUTGD_SEAL_WRITE_VECTORS=1 rewrites the hashes and small containers (a format change only).
 void vectorsSealAndOpenIdentically() {
  auto doc=document();auto list=doc["vectors"].toArray();QVERIFY(!list.isEmpty());
  const bool write=qEnvironmentVariableIsSet("PUTGD_SEAL_WRITE_VECTORS");
  for(qsizetype i=0;i<list.size();++i) {
   auto json=list[i].toObject();const auto v=vector(json);
   const auto container=sealed(v);QVERIFY2(!container.isEmpty(),qPrintable(json["id"].toString()));
   QCOMPARE(container.size(),seal::containerBytes(seal::encodeMeta(v.meta).size()+v.plain.size()));
   const auto digest=QString::fromLatin1(QCryptographicHash::hash(container,QCryptographicHash::Sha256).toHex());
   if(write){json["containerSha256"]=digest;json["container"]=container.size()<65536?QString::fromLatin1(container.toBase64()):QString();list[i]=json;}
   else {
    QCOMPARE(digest,json["containerSha256"].toString());
    if(!json["container"].toString().isEmpty())QCOMPARE(container,QByteArray::fromBase64(json["container"].toString().toLatin1()));
   }
   QByteArray plain;seal::Meta meta;const auto error=opened(container,v.key,v.passphrase,&plain,&meta);
   QVERIFY2(!error,qPrintable(json["id"].toString()+": "+error.message));
   QCOMPARE(plain,v.plain);QCOMPARE(meta.name,v.meta.name);QCOMPARE(meta.type,v.meta.type);QCOMPARE(meta.size,v.meta.size);
  }
  if(write){doc["vectors"]=list;QFile file(vectorsPath);QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));file.write(QJsonDocument(doc).toJson(QJsonDocument::Indented));}
 }
 // Every chunk but the last is full; a plaintext of exactly one chunk is one last chunk.
 void boundaryVectorIsOneFullChunk() {
  const auto v=named("boundary");QCOMPARE(seal::encodeMeta(v.meta).size()+v.plain.size(),seal::chunkBytes);
  QCOMPARE(sealed(v).size(),qint64(seal::headerBytes)+full);
 }
 void changedBytesNeverOpen() {
  const auto v=named("multi");const auto container=sealed(v);QVERIFY(container.size()>seal::headerBytes+2*full);
  const auto damaged=[&](const QByteArray& bytes){return opened(bytes,v.key,{}).code;};
  // One bit anywhere: the header's magic, version, flags, chunk size, salt, nonce prefix,
  // reserved bytes and key check, and the first, middle and last chunk and their tags.
  for(const qint64 at:{0LL,8LL,9LL,13LL,17LL,25LL,55LL,60LL,70LL,96LL,96+full-1,96+full+500,qint64(container.size())-17,qint64(container.size())-1}) {
   auto changed=container;changed[at]=char(changed[at]^0x01);
   const auto code=damaged(changed);QVERIFY2(code=="seal_damaged"||code=="seal_version",qPrintable(QString::number(at)+" "+code));
  }
  // Cut: the last chunk gone (the new last isn't marked last), one byte gone, all but the header.
  QCOMPARE(damaged(container.left(seal::headerBytes+2*full)),QString("seal_damaged"));
  QCOMPARE(damaged(container.chopped(1)),QString("seal_damaged"));
  QCOMPARE(damaged(container.left(seal::headerBytes)),QString("seal_damaged"));
  QCOMPARE(damaged(container.left(seal::headerBytes+full)),QString("seal_damaged"));
  // Reordered chunks, a chunk repeated, bytes added after the end.
  const auto head=container.left(seal::headerBytes),c0=container.mid(seal::headerBytes,full),c1=container.mid(seal::headerBytes+full,full),c2=container.mid(seal::headerBytes+2*full);
  QCOMPARE(damaged(head+c1+c0+c2),QString("seal_damaged"));
  QCOMPARE(damaged(head+c0+c1+c1+c2),QString("seal_damaged"));
  QCOMPARE(damaged(head+c0+c2),QString("seal_damaged"));
  QCOMPARE(damaged(container+QByteArray(17,'\0')),QString("seal_damaged"));
  // A chunk from another container with the same key but a different salt.
  auto other=v;other.fixed.salt[0]=char(other.fixed.salt[0]^1);const auto second=sealed(other);
  QCOMPARE(damaged(head+second.mid(seal::headerBytes,full)+c1+c2),QString("seal_damaged"));
  // Nothing of a chunk that fails is written.
  QByteArray plain;opened(head+c0+c1,v.key,{},&plain);QCOMPARE(plain.size(),seal::chunkBytes-seal::encodeMeta(v.meta).size());
 }
 void wrongKeyNeverOpens() {
  const auto v=named("hello");const auto container=sealed(v);
  auto key=v.key;key[31]=char(key[31]^0x80);
  QCOMPARE(opened(container,key,{}).code,QString("seal_damaged"));
  QCOMPARE(opened(container,QByteArray(32,'\0'),{}).code,QString("seal_damaged"));
  QCOMPARE(opened(container,v.key.left(31),{}).code,QString("seal_damaged"));
 }
 // The passphrase is a second factor: the key alone, or a wrong passphrase, opens nothing.
 // Its Unicode form doesn't matter (NFC on both sides).
 void passphraseIsASecondFactor() {
  const auto v=named("passphrase");const auto container=sealed(v);
  QCOMPARE(opened(container,v.key,{}).code,QString("seal_passphrase_required"));
  QCOMPARE(opened(container,v.key,"ete ☃ battery").code,QString("seal_passphrase"));
  QCOMPARE(opened(container,v.key,v.passphrase+" ").code,QString("seal_passphrase"));
  QByteArray plain;QVERIFY(!opened(container,v.key,QString::fromUtf8("été ☃ battery"),&plain));QCOMPARE(plain,v.plain);
  auto key=v.key;key[0]=char(key[0]^1);QCOMPARE(opened(container,key,v.passphrase).code,QString("seal_damaged"));
  // The header records the passphrase: clearing its flag breaks the key check.
  auto cleared=container;cleared[9]=0;for(int i=16;i<20;++i)cleared[i]=0;QCOMPARE(opened(cleared,v.key,{}).code,QString("seal_damaged"));
  // Without a fixed salt every seal differs.
  auto random=v;random.fixed={};QVERIFY(sealed(random)!=sealed(random));
 }
 void fragmentsCarryOnlyCanonicalKeys() {
  const auto v=named("hello");const auto text=seal::fragment(v.key);
  QCOMPARE(text.size(),46);QVERIFY(text.startsWith("v1."));
  QCOMPARE(seal::keyFromFragment(text),v.key);QCOMPARE(seal::keyFromFragment("#"+text),v.key);
  QVERIFY(seal::keyFromFragment(text.chopped(1)).isEmpty());QVERIFY(seal::keyFromFragment("v2."+text.mid(3)).isEmpty());
  QVERIFY(seal::keyFromFragment(text.left(45)+"+").isEmpty());
  // The last character holds 4 unused bits: a different one decoding to the same key isn't the key's form.
  auto last=text;const QString alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  last[45]=alphabet[(alphabet.indexOf(text[45])|1)==alphabet.indexOf(text[45])?alphabet.indexOf(text[45])-1:alphabet.indexOf(text[45])+1];
  QVERIFY(seal::keyFromFragment(last).isEmpty());
  QCOMPARE(seal::randomKey().size(),32);QVERIFY(seal::randomKey()!=seal::randomKey());
 }
 // The compiled-in viewer is src/seal/viewer.html byte for byte, at the pinned hash.
 void viewerIsPinned() {
  QVERIFY(seal::viewerIntact());
  QFile file("src/seal/viewer.html");QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),seal::viewer());
  QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(seal::viewer(),QCryptographicHash::Sha256).toHex()),QString::fromLatin1(seal::viewerSha256));
  const auto csp=seal::pagePrefix(false);
  QVERIFY(csp.contains("default-src 'none'"));QVERIFY(csp.contains("img-src blob:; media-src blob:; connect-src 'none'"));
  QVERIFY(seal::pagePrefix(true).contains("connect-src 'self'"));QVERIFY(!seal::pagePrefix(true).contains("connect-src 'none'"));
  QVERIFY(!seal::viewer().contains("http://")&&!seal::viewer().contains("https://"));
 }
 void pagesParseOnlyWhenUnchanged() {
  const auto container=sealed(named("hello"));
  const auto page=seal::pagePrefix(false)+seal::dataOpen(false)+container.toBase64()+seal::dataClose()+seal::pageSuffix();
  seal::ParsedPage parsed;QVERIFY(!seal::parsePage(page,parsed));QVERIFY(!parsed.split);QCOMPARE(QByteArray::fromBase64(parsed.base64),container);
  const auto split=seal::pagePrefix(true)+seal::dataOpen(true,"k3Xq9vTa2mPw.bin")+seal::dataClose()+seal::pageSuffix();
  QVERIFY(!seal::parsePage(split,parsed));QVERIFY(parsed.split);QCOMPARE(parsed.bin,QString("k3Xq9vTa2mPw.bin"));
  // Any change outside the data, a split page under the inline CSP, a bad name or data.
  auto changed=page;changed.replace("Sealed with put.gd","Sealed with put.gx");QCOMPARE(seal::parsePage(changed,parsed).code,QString("seal_page_changed"));
  auto script=page;script.replace("\"use strict\";","\"use strict\";fetch('//x');");QCOMPARE(seal::parsePage(script,parsed).code,QString("seal_page_changed"));
  QCOMPARE(seal::parsePage(seal::pagePrefix(false)+seal::dataOpen(true,"a.bin")+seal::dataClose()+seal::pageSuffix(),parsed).code,QString("seal_page_changed"));
  QCOMPARE(seal::parsePage(seal::pagePrefix(true)+seal::dataOpen(true,"../a.bin")+seal::dataClose()+seal::pageSuffix(),parsed).code,QString("seal_page_changed"));
  QCOMPARE(seal::parsePage(seal::pagePrefix(false)+seal::dataOpen(false)+"QUJD</script><script>x"+seal::dataClose()+seal::pageSuffix(),parsed).code,QString("seal_page_changed"));
  QCOMPARE(seal::parsePage(seal::pagePrefix(false)+seal::dataOpen(false)+"QUJ=D==="+seal::dataClose()+seal::pageSuffix(),parsed).code,QString("seal_page_changed"));
 }
 // The resident's helper: putgd-cli --seal-job writes a page (and a data file) and answers the key.
 void sealJobWritesPagesThatOpen() {
  const auto cli=qEnvironmentVariable("PUTGD_TEST_CLI");if(cli.isEmpty())QSKIP("PUTGD_TEST_CLI is not set");
  QTemporaryDir dir;QVERIFY(dir.isValid());
  const auto v=named("multi");QFile in(dir.filePath("in"));QVERIFY(in.open(QIODevice::WriteOnly));in.write(v.plain);in.close();
  for(const bool split:{false,true}) {
   QJsonObject request{{"input",in.fileName()},{"bytes",double(v.plain.size())},{"name","pattern.bin"},{"type","application/octet-stream"},{"form",split?"split":"inline"},
    {"page",dir.filePath(split?"split.html":"inline.html")},{"passphrase","hunter2"}};
   if(split){request["data"]=dir.filePath("split.bin");request["bin"]="Abc123.bin";}
   QProcess job;job.start(cli,{"--seal-job"});QVERIFY(job.waitForStarted());job.write(QJsonDocument(request).toJson());job.closeWriteChannel();QVERIFY(job.waitForFinished(60000));
   const auto answer=QJsonDocument::fromJson(job.readAllStandardOutput()).object();QVERIFY2(answer["ok"].toBool(),job.readAllStandardError().constData());
   const auto key=seal::keyFromFragment(answer["key"].toString());QCOMPARE(key.size(),32);
   QFile page(request["page"].toString());QVERIFY(page.open(QIODevice::ReadOnly));const auto bytes=page.readAll();
   QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex()),answer["page"].toObject()["sha256"].toString());
   QVERIFY((page.permissions()&(QFile::ReadGroup|QFile::ReadOther))==0);
   seal::ParsedPage parsed;QVERIFY(!seal::parsePage(bytes,parsed));QCOMPARE(parsed.split,split);
   QByteArray container;
   if(split){QCOMPARE(parsed.bin,QString("Abc123.bin"));QFile data(request["data"].toString());QVERIFY(data.open(QIODevice::ReadOnly));container=data.readAll();
    QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(container,QCryptographicHash::Sha256).toHex()),answer["data"].toObject()["sha256"].toString());}
   else container=QByteArray::fromBase64(parsed.base64);
   QByteArray plain;QVERIFY(!opened(container,key,"hunter2",&plain));QCOMPARE(plain,v.plain);
   // An existing output is never overwritten.
   QProcess again;again.start(cli,{"--seal-job"});QVERIFY(again.waitForStarted());again.write(QJsonDocument(request).toJson());again.closeWriteChannel();QVERIFY(again.waitForFinished(60000));
   QVERIFY(!QJsonDocument::fromJson(again.readAllStandardOutput()).object()["ok"].toBool());
  }
 }
};
QTEST_GUILESS_MAIN(SealTest)
#include "test_seal.moc"
