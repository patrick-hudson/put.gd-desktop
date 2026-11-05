#include "core/ArtifactStore.h"
#include "core/FolderZip.h"
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QRandomGenerator>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
using namespace putgd;
namespace {
constexpr qint64 mib=1024*1024,gib=1024*mib;
quint32 le(const QByteArray& b,int at,int bytes) {quint32 v=0;for(int i=bytes-1;i>=0;--i)v=v<<8|quint8(b[at+i]);return v;}
quint64 le64(const QByteArray& b,int at) {return quint64(le(b,at,4))|quint64(le(b,at+4,4))<<32;}
struct ZipEntry {QByteArray name;quint16 flags=0,method=0;quint32 crc=0,external=0;quint64 size=0,compressed=0,offset=0;QByteArray content;};
struct ZipListing {QList<ZipEntry> entries;quint64 total=0,directoryOffset=0;quint32 classicEntries=0,classicOffset=0;QString error;};
// An independent reader: classic end record, ZIP64 locator and end record, the central
// directory, then every local header, which must carry the final CRC and sizes (no data
// descriptors). Entry data is inflated and checked against its CRC; contents up to
// `keep` bytes are returned.
ZipListing readZip(const QString& path,qint64 keep=16*mib) {
 ZipListing out;QFile f(path);if(!f.open(QIODevice::ReadOnly)) {out.error="cannot open";return out;}
 const qint64 size=f.size();if(size<98) {out.error="too short";return out;}
 f.seek(size-98);const auto tail=f.read(98);
 if(le(tail,76,4)!=0x06054b50||le(tail,56,4)!=0x07064b50||le(tail,0,4)!=0x06064b50) {out.error="end records missing";return out;}
 out.classicEntries=le(tail,76+10,2);out.classicOffset=le(tail,76+16,4);
 if(le64(tail,56+8)!=quint64(size-98)) {out.error="locator offset wrong";return out;}
 out.total=le64(tail,32);const auto directoryBytes=le64(tail,40);out.directoryOffset=le64(tail,48);
 if(out.directoryOffset+directoryBytes!=quint64(size-98)) {out.error="directory does not end at the end records";return out;}
 f.seek(qint64(out.directoryOffset));const auto directory=f.read(qint64(directoryBytes));
 for(qint64 at=0;at<directory.size();) {
  if(le(directory,at,4)!=0x02014b50) {out.error="central signature";return out;}
  ZipEntry e;e.flags=le(directory,at+8,2);e.method=le(directory,at+10,2);e.crc=le(directory,at+16,4);
  const int name=le(directory,at+28,2),extra=le(directory,at+30,2),comment=le(directory,at+32,2);e.external=le(directory,at+38,4);
  e.name=directory.mid(at+46,name);const auto x=directory.mid(at+46+name,extra);
  if(le(directory,at+20,4)!=0xffffffff||le(directory,at+24,4)!=0xffffffff||le(directory,at+42,4)!=0xffffffff||extra!=28||le(x,0,2)!=1||le(x,2,2)!=24) {out.error="central ZIP64 extra missing: "+e.name;return out;}
  e.size=le64(x,4);e.compressed=le64(x,12);e.offset=le64(x,20);out.entries.append(e);at+=46+name+extra+comment;
 }
 if(quint64(out.entries.size())!=out.total) {out.error="entry count";return out;}
 z_stream z{};inflateInit2(&z,-MAX_WBITS);QByteArray in(4*mib,0),inflated(4*mib,0);
 for(auto& e:out.entries) {
  f.seek(qint64(e.offset));const auto local=f.read(30);
  const int name=le(local,26,2),extra=le(local,28,2);const auto rest=f.read(name+extra);
  if(le(local,0,4)!=0x04034b50||le(local,6,2)!=e.flags||le(local,8,2)!=e.method||le(local,14,4)!=e.crc||rest.left(name)!=e.name) {out.error="local header differs: "+e.name;break;}
  const auto x=rest.mid(name);
  if(extra!=20||le(x,0,2)!=1||le64(x,4)!=e.size||le64(x,12)!=e.compressed) {out.error="local ZIP64 sizes not patched: "+e.name;break;}
  quint32 crc=0;quint64 produced=0;qint64 left=qint64(e.compressed);inflateReset(&z);
  while(left>0||e.method==8) {
   const auto chunk=f.read(qMin<qint64>(left,in.size()));left-=chunk.size();
   if(e.method==0) {crc=crc32(crc,reinterpret_cast<const Bytef*>(chunk.constData()),uInt(chunk.size()));produced+=chunk.size();if(qint64(e.size)<=keep)e.content+=chunk;if(chunk.isEmpty())break;continue;}
   z.next_in=reinterpret_cast<Bytef*>(const_cast<char*>(chunk.constData()));z.avail_in=uInt(chunk.size());int status=Z_OK;
   do {z.next_out=reinterpret_cast<Bytef*>(inflated.data());z.avail_out=uInt(inflated.size());status=inflate(&z,Z_NO_FLUSH);
    if(status!=Z_OK&&status!=Z_STREAM_END&&status!=Z_BUF_ERROR)break;
    const auto n=inflated.size()-z.avail_out;crc=crc32(crc,reinterpret_cast<const Bytef*>(inflated.constData()),uInt(n));produced+=n;if(qint64(e.size)<=keep)e.content+=inflated.left(n);
   } while(z.avail_out==0);
   if(status==Z_STREAM_END)break;
   if(chunk.isEmpty()||(status!=Z_OK&&status!=Z_BUF_ERROR)) {out.error="inflate failed: "+e.name;break;}
  }
  if(!out.error.isEmpty())break;
  if(crc!=e.crc||produced!=e.size) {out.error="data differs from CRC or size: "+e.name;break;}
 }
 inflateEnd(&z);return out;
}
QString tool(const QString& name) {return QStandardPaths::findExecutable(name);}
// Info-ZIP and 7-Zip, run under a UTF-8 locale so names keep their bytes.
QString runTool(const QString& program,const QStringList& arguments,int timeout=600000) {
 QProcess p;auto environment=QProcessEnvironment::systemEnvironment();environment.insert("LC_ALL","C.UTF-8");p.setProcessEnvironment(environment);
 p.start(program,arguments);if(!p.waitForFinished(timeout))return "did not finish";
 return p.exitStatus()==QProcess::NormalExit&&p.exitCode()==0?QString{}:QString::fromUtf8(p.readAllStandardOutput()+p.readAllStandardError()).right(2000);
}
bool make(const QString& path,const QByteArray& data) {QDir().mkpath(QFileInfo(path).path());QFile f(path);return f.open(QIODevice::WriteOnly)&&f.write(data)==data.size();}
QByteArray randomBytes(qint64 n) {QByteArray b(n,0);QRandomGenerator::global()->fillRange(reinterpret_cast<quint32*>(b.data()),n/4);return b;}
QByteArray fileBytes(const QString& path) {QFile f(path);return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray();}
QStringList entriesOf(const QString& dir) {return QDir(dir).entryList(QDir::AllEntries|QDir::Hidden|QDir::System|QDir::NoDotAndDotDot);}
qint64 statusKiB(const QByteArray& key) {QFile f("/proc/self/status");if(!f.open(QIODevice::ReadOnly))return -1;for(const auto& line:f.readAll().split('\n'))if(line.startsWith(key+":"))return line.mid(key.size()+1).trimmed().split(' ').value(0).toLongLong();return -1;}
// Resets VmHWM to the current RSS (Linux 4.0+), so a peak covers only what follows.
void resetPeak() {QFile f("/proc/self/clear_refs");if(f.open(QIODevice::WriteOnly))f.write("5");}
const ZipEntry* find(const ZipListing& l,const QByteArray& name) {for(const auto& e:l.entries)if(e.name==name)return &e;return nullptr;}
// Every external check available on this host; a missing tool is reported, not failed.
void externalChecks(const QString& archive) {
 if(const auto unzip=tool("unzip");!unzip.isEmpty()) {const auto r=runTool(unzip,{"-tqq",archive});QVERIFY2(r.isEmpty(),qPrintable("unzip -t: "+r));}
 else qInfo("unzip not installed: skipped unzip -t");
 const auto sevenZip=!tool("7z").isEmpty()?tool("7z"):tool("7zz");
 if(!sevenZip.isEmpty()) {const auto r=runTool(sevenZip,{"t","-bd",archive});QVERIFY2(r.isEmpty(),qPrintable("7z t: "+r));}
 else qInfo("7-Zip not installed: skipped 7z t");
}
}
class FolderZipTest:public QObject {
 Q_OBJECT
 static bool large() {return qEnvironmentVariableIntValue("PUTGD_FOLDER_ZIP_LARGE")==1;}
private slots:
 void archiveOpensToTheFolder() {
  QTemporaryDir d;const auto root=d.filePath("Photos");
  QVERIFY(make(root+"/a.txt","first file\n"));QVERIFY(make(root+"/.hidden","hidden\n"));QVERIFY(QDir().mkpath(root+"/empty"));
  const QString unicode=QString::fromUtf8("sub/ü-é 日本.txt");QVERIFY(make(root+"/"+unicode,"unicode\n"));
  QVERIFY(QFile::link(root+"/a.txt",root+"/link.txt"));QVERIFY(QFile::link(root+"/sub",root+"/dirlink"));QVERIFY(QFile::link(d.filePath("missing"),root+"/dangling"));
  QCOMPARE(mkfifo(QFile::encodeName(root+"/fifo").constData(),0600),0);
  const auto plan=FolderZip::plan(root);QVERIFY2(!plan.error,qPrintable(plan.error.message));
  QCOMPARE(plan.archiveName,QString("Photos.zip"));QCOMPARE(plan.folder,root);
  QCOMPARE(plan.entries,6);QCOMPARE(plan.files,3);QCOMPARE(plan.sourceBytes,qint64(11+7+8));
  const auto out=d.filePath("out/Photos.zip");QVERIFY(QDir().mkpath(d.filePath("out")));
  const auto error=FolderZip::write(plan,out);QVERIFY2(!error,qPrintable(error.message));
  QCOMPARE(entriesOf(d.filePath("out")),QStringList{"Photos.zip"});
  QCOMPARE(QFileInfo(out).permissions()&(QFile::ReadGroup|QFile::WriteGroup|QFile::ReadOther|QFile::WriteOther),QFile::Permissions{});
  QVERIFY(QFileInfo(out).size()<=plan.finalBound);QVERIFY(plan.peakBytes>=plan.finalBound);
  const auto zip=readZip(out);QVERIFY2(zip.error.isEmpty(),qPrintable(zip.error));
  QSet<QByteArray> names;for(const auto& e:zip.entries) {names.insert(e.name);QVERIFY(e.flags&0x0800);QVERIFY(!(e.flags&0x0008));}
  QCOMPARE(names,(QSet<QByteArray>{"Photos/","Photos/a.txt","Photos/.hidden","Photos/empty/","Photos/sub/",("Photos/"+unicode).toUtf8()}));
  QCOMPARE(find(zip,"Photos/a.txt")->content,QByteArray("first file\n"));QCOMPARE(find(zip,("Photos/"+unicode).toUtf8())->content,QByteArray("unicode\n"));
  QVERIFY(find(zip,"Photos/empty/")->external&0x10);QCOMPARE(find(zip,"Photos/empty/")->size,0ULL);
  // The classic end record holds the real count and offset when they fit.
  QCOMPARE(zip.classicEntries,6u);QCOMPARE(quint64(zip.classicOffset),zip.directoryOffset);
  externalChecks(out);
  if(const auto unzip=tool("unzip");!unzip.isEmpty()) {
   const auto r=runTool(unzip,{"-q",out,"-d",d.filePath("x")});QVERIFY2(r.isEmpty(),qPrintable(r));
   QCOMPARE(entriesOf(d.filePath("x")),QStringList{"Photos"});
   QCOMPARE(fileBytes(d.filePath("x/Photos/.hidden")),QByteArray("hidden\n"));QCOMPARE(fileBytes(d.filePath("x/Photos/"+unicode)),QByteArray("unicode\n"));
   QVERIFY(QFileInfo(d.filePath("x/Photos/empty")).isDir());QVERIFY(!QFileInfo::exists(d.filePath("x/Photos/link.txt")));QVERIFY(!QFileInfo::exists(d.filePath("x/Photos/fifo")));
  }
  // An existing output is never replaced.
  QCOMPARE(FolderZip::write(plan,out).code,QString("artifact_io"));QVERIFY(QFileInfo(out).size()>0);
 }
 void emptyFolderIsAValidArchive() {
  QTemporaryDir d;QVERIFY(QDir().mkpath(d.filePath("Nothing")));
  const auto plan=FolderZip::plan(d.filePath("Nothing"));QVERIFY(!plan.error);QCOMPARE(plan.entries,1);QCOMPARE(plan.files,0);
  QVERIFY(!FolderZip::write(plan,d.filePath("n.zip")));const auto zip=readZip(d.filePath("n.zip"));QVERIFY2(zip.error.isEmpty(),qPrintable(zip.error));
  QCOMPARE(zip.entries.size(),1);QCOMPARE(zip.entries[0].name,QByteArray("Nothing/"));externalChecks(d.filePath("n.zip"));
 }
 void mixesStoredAndDeflatedEntries() {
  QTemporaryDir d;const auto root=d.filePath("Mixed");
  const auto noise=randomBytes(3*mib),text=QByteArray("put.gd folder zip ").repeated(40000);
  QByteArray halves(QByteArray(mib,'z'));halves+=randomBytes(2*mib);
  QVERIFY(make(root+"/noise.bin",noise));QVERIFY(make(root+"/zeros.bin",QByteArray(3*mib,0)));QVERIFY(make(root+"/text.txt",text));
  QVERIFY(make(root+"/tiny",QByteArray("ab")));QVERIFY(make(root+"/empty.txt",{}));QVERIFY(make(root+"/probe.bin",QByteArray(mib,'p')));
  QVERIFY(make(root+"/two.bin",QByteArray(2*mib,'q')));QVERIFY(make(root+"/halves.bin",halves));
  const auto plan=FolderZip::plan(root);QVERIFY(!plan.error);
  QVERIFY(!FolderZip::write(plan,d.filePath("m.zip")));QVERIFY(QFileInfo(d.filePath("m.zip")).size()<=plan.finalBound);
  const auto zip=readZip(d.filePath("m.zip"));QVERIFY2(zip.error.isEmpty(),qPrintable(zip.error));
  const auto method=[&](const char* name){return find(zip,QByteArray("Mixed/")+name)->method;};
  QCOMPARE(method("noise.bin"),quint16(0));QCOMPARE(find(zip,"Mixed/noise.bin")->compressed,quint64(noise.size()));QCOMPARE(find(zip,"Mixed/noise.bin")->content,noise);
  QCOMPARE(method("zeros.bin"),quint16(8));QVERIFY(find(zip,"Mixed/zeros.bin")->compressed<quint64(64*1024));QCOMPARE(find(zip,"Mixed/zeros.bin")->content,QByteArray(3*mib,0));
  QCOMPARE(method("text.txt"),quint16(8));QCOMPARE(find(zip,"Mixed/text.txt")->content,text);
  QCOMPARE(method("tiny"),quint16(0));QCOMPARE(method("empty.txt"),quint16(0));QCOMPARE(find(zip,"Mixed/empty.txt")->size,0ULL);
  // Exactly one probe, and exactly two read chunks: both end on a chunk boundary.
  QCOMPARE(method("probe.bin"),quint16(8));QCOMPARE(find(zip,"Mixed/probe.bin")->content,QByteArray(mib,'p'));
  QCOMPARE(method("two.bin"),quint16(8));QCOMPARE(find(zip,"Mixed/two.bin")->content,QByteArray(2*mib,'q'));
  // The probe said deflate; the random rest makes it larger than its first MiB suggested. Still valid.
  QCOMPARE(method("halves.bin"),quint16(8));QCOMPARE(find(zip,"Mixed/halves.bin")->content,halves);
  externalChecks(d.filePath("m.zip"));
 }
 void localHeadersCarryTheFinalValues() {
  QTemporaryDir d;QVERIFY(make(d.filePath("L/one.txt"),QByteArray("local header ").repeated(1000)));QVERIFY(make(d.filePath("L/two.bin"),randomBytes(4096)));
  const auto plan=FolderZip::plan(d.filePath("L"));QVERIFY(!FolderZip::write(plan,d.filePath("l.zip")));
  const auto bytes=fileBytes(d.filePath("l.zip"));QVERIFY(bytes.size()>30);
  // Walk the local headers in order, as a streaming unzipper does.
  qint64 at=0;int files=0;
  while(le(bytes,at,4)==0x04034b50) {
   const auto flags=le(bytes,at+6,2),name=le(bytes,at+26,2),extra=le(bytes,at+28,2);
   QCOMPARE(flags,0x0800u);QCOMPARE(le(bytes,at+4,2),45u);QCOMPARE(le(bytes,at+18,4),0xffffffffu);QCOMPARE(le(bytes,at+22,4),0xffffffffu);
   const auto entry=bytes.mid(at+30,name);const auto compressed=le64(bytes,at+30+name+12),size=le64(bytes,at+30+name+4);
   if(!entry.endsWith('/')) {++files;QVERIFY(le(bytes,at+14,4)!=0);QVERIFY(size>0);}
   at+=30+name+extra+qint64(compressed);
  }
  QCOMPARE(files,2);QCOMPARE(le(bytes,at,4),0x02014b50u);
 }
 void boundsCoverTheArchive() {
  QTemporaryDir d;const auto root=d.filePath("Bounds");
  for(int i=0;i<200;++i)QVERIFY(make(root+QStringLiteral("/d%1/f%2.bin").arg(i%7).arg(i),i%3?randomBytes(i*13):QByteArray(i*31,'x')));
  QVERIFY(QDir().mkpath(root+"/deep/a/b/c/d/e"));
  const auto plan=FolderZip::plan(root);QVERIFY(!plan.error);QCOMPARE(plan.files,200);QCOMPARE(plan.entries,1+7+200+6);
  QVERIFY(!FolderZip::write(plan,d.filePath("b.zip")));
  const auto size=QFileInfo(d.filePath("b.zip")).size();QVERIFY(size<=plan.finalBound);QVERIFY(plan.peakBytes>=plan.finalBound);
  // The peak adds one more copy of the central directory: the spill beside its appended copy.
  const auto zip=readZip(d.filePath("b.zip"));QVERIFY2(zip.error.isEmpty(),qPrintable(zip.error));
  QVERIFY(plan.peakBytes-plan.finalBound>=qint64(quint64(size-98)-zip.directoryOffset));
  externalChecks(d.filePath("b.zip"));
 }
 void sourceThatChangesNeverCorruptsTheArchive() {
  QTemporaryDir d;const auto root=d.filePath("Grow");const auto noise=randomBytes(mib);
  QVERIFY(make(root+"/log.bin",noise));QVERIFY(make(root+"/other.txt","other\n"));
  auto plan=FolderZip::plan(root);QVERIFY(!plan.error);
  // Grows past the bound after the plan: refused by name, and nothing is left behind.
  {QFile f(root+"/log.bin");QVERIFY(f.open(QIODevice::Append));f.write(randomBytes(256*1024));}
  QVERIFY(QDir().mkpath(d.filePath("out")));
  auto error=FolderZip::write(plan,d.filePath("out/g.zip"));
  QCOMPARE(error.code,QString("source_changed"));QVERIFY2(error.message.contains("Grow/log.bin"),qPrintable(error.message));
  QVERIFY(entriesOf(d.filePath("out")).isEmpty());
  // A few bytes fit the slack: the archive holds what was actually read.
  plan=FolderZip::plan(root);QVERIFY(!plan.error);
  {QFile f(root+"/other.txt");QVERIFY(f.open(QIODevice::Append));f.write("more\n");}
  {QFile f(root+"/log.bin");QVERIFY(f.resize(4096));}
  error=FolderZip::write(plan,d.filePath("out/g.zip"));QVERIFY2(!error,qPrintable(error.message));
  auto zip=readZip(d.filePath("out/g.zip"));QVERIFY2(zip.error.isEmpty(),qPrintable(zip.error));
  QCOMPARE(find(zip,"Grow/other.txt")->content,QByteArray("other\nmore\n"));QCOMPARE(find(zip,"Grow/log.bin")->content,noise.left(4096));
  // New entries after the plan, beyond its bound, are refused too.
  QVERIFY(QFile::remove(d.filePath("out/g.zip")));plan=FolderZip::plan(root);
  for(int i=0;i<50;++i)QVERIFY(make(root+QStringLiteral("/new%1.txt").arg(i),"n"));
  QCOMPARE(FolderZip::write(plan,d.filePath("out/g.zip")).code,QString("source_changed"));QVERIFY(entriesOf(d.filePath("out")).isEmpty());
 }
 void cancelAndUnreadableSourcesLeaveNothing() {
  QTemporaryDir d;const auto root=d.filePath("Stop");QVERIFY(make(root+"/a.bin",QByteArray(3*mib,'a')));
  const auto plan=FolderZip::plan(root);QVERIFY(QDir().mkpath(d.filePath("out")));
  int polls=0;const auto error=FolderZip::write(plan,d.filePath("out/s.zip"),[&]{return ++polls>2;});
  QCOMPARE(error.code,QString("cancelled"));QVERIFY(entriesOf(d.filePath("out")).isEmpty());
  QVERIFY(FolderZip::plan(d.filePath("missing")).error);QCOMPARE(FolderZip::plan(root+"/a.bin").error.code,QString("source_unavailable"));
  if(geteuid()==0)QSKIP("root reads every file");
  QVERIFY(make(root+"/locked/secret.txt","s"));QVERIFY(make(root+"/closed.txt","c"));
  QVERIFY(QFile::setPermissions(root+"/closed.txt",{}));
  auto refused=FolderZip::plan(root);QCOMPARE(refused.error.code,QString("source_unavailable"));QVERIFY2(refused.error.message.contains("Stop/closed.txt"),qPrintable(refused.error.message));
  QVERIFY(QFile::setPermissions(root+"/closed.txt",QFile::ReadOwner|QFile::WriteOwner));QVERIFY(QFile::setPermissions(root+"/locked",QFile::ExeOwner));
  refused=FolderZip::plan(root);QVERIFY2(refused.error.message.contains("Stop/locked"),qPrintable(refused.error.message));
  QVERIFY(QFile::setPermissions(root+"/locked",QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
 }
 void storeAcquiresAFolderBesideAFile() {
  QTemporaryDir d;const auto root=d.filePath("Photos");QVERIFY(make(root+"/a.txt","alpha\n"));QVERIFY(make(root+"/b/c.bin",QByteArray(5000,'c')));
  QVERIFY(make(d.filePath("note.txt"),"note\n"));
  ClipboardPayload p;p.kind="files";p.files={root,d.filePath("note.txt")};p.zipFolders=true;
  ArtifactStore store(d.filePath("spool"));const auto op=QUuid::createUuid().toString(QUuid::WithoutBraces);
  auto result=store.acquire(op,p,ResourceLimits{},"upload");QVERIFY2(!result.error,qPrintable(result.error.message));QCOMPARE(result.artifacts.size(),2);
  const auto& zip=result.artifacts[0];
  QCOMPARE(zip.name,QString("Photos.zip"));QCOMPARE(zip.mime,QString("application/zip"));QVERIFY(zip.originalFile);QCOMPARE(zip.category,QString("other"));
  QCOMPARE(zip.bytes,QFileInfo(zip.path).size());QVERIFY(!store.verify(zip));QVERIFY(zip.path.startsWith(d.filePath("spool/"+op+"/")));
  const auto listing=readZip(zip.path);QVERIFY2(listing.error.isEmpty(),qPrintable(listing.error));QCOMPARE(listing.entries.size(),4);
  QCOMPARE(find(listing,"Photos/a.txt")->content,QByteArray("alpha\n"));
  QCOMPARE(result.artifacts[1].name,QString("note.txt"));QCOMPARE(fileBytes(result.artifacts[1].path),QByteArray("note\n"));
  // Only the two artifacts and the manifest remain: no spill file.
  QCOMPARE(entriesOf(d.filePath("spool/"+op)).size(),3);
  QCOMPARE(store.recover(op).artifacts.size(),2);
  // An empty folder is one valid archive, not an empty source.
  QVERIFY(QDir().mkpath(d.filePath("Empty")));p.files={d.filePath("Empty")};
  result=store.acquire(QUuid::createUuid().toString(QUuid::WithoutBraces),p,ResourceLimits{});QVERIFY2(!result.error,qPrintable(result.error.message));
  QCOMPARE(result.artifacts[0].name,QString("Empty.zip"));QCOMPARE(readZip(result.artifacts[0].path).entries.size(),1);
  // Without zip mode a folder stays an unsupported source; a symlink to a folder is never zipped.
  p.files={root};p.zipFolders=false;result=store.acquire(QUuid::createUuid().toString(QUuid::WithoutBraces),p,ResourceLimits{});
  QCOMPARE(result.error.code,QString("source_unavailable"));QCOMPARE(result.error.message,QString("Source is not an accessible regular file"));QCOMPARE(result.error.details["itemIndex"].toInt(),0);
  QVERIFY(QFile::link(root,d.filePath("linked")));p.files={d.filePath("linked")};p.zipFolders=true;
  QCOMPARE(store.acquire(QUuid::createUuid().toString(QUuid::WithoutBraces),p,ResourceLimits{}).error.code,QString("source_unavailable"));
  QCOMPARE(entriesOf(d.filePath("spool")).size(),2);
 }
 void storeChecksAFolderByItsTotal() {
  QTemporaryDir d;const auto root=d.filePath("Photos");QVERIFY(make(root+"/a.bin",QByteArray(3*mib,0)));QVERIFY(make(d.filePath("one.bin"),QByteArray(mib,'1')));
  ClipboardPayload p;p.kind="files";p.files={d.filePath("one.bin"),root};p.zipFolders=true;ArtifactStore store(d.filePath("spool"));
  const auto id=[]{return QUuid::createUuid().toString(QUuid::WithoutBraces);};
  ResourceLimits limits;limits.maxFileBytes=2*mib;
  auto r=store.acquire(id(),p,limits);QCOMPARE(r.error.code,QString("input_excessive"));QCOMPARE(r.error.details["itemIndex"].toInt(),1);
  QVERIFY2(r.error.message.startsWith("Photos (folder) totals 3 MB; the Largest file limit is 2 MB"),qPrintable(r.error.message));
  limits=ResourceLimits{};limits.maxBatchBytes=3*mib+mib/2;r=store.acquire(id(),p,limits);
  QVERIFY2(r.error.message.contains("Photos (folder) totals 3 MB; this batch would pass the Largest batch limit"),qPrintable(r.error.message));
  r=store.acquire(id(),p,ResourceLimits{},"clipboard",Settings::fromValidated(Settings::defaults()),{},2*mib);
  QCOMPARE(r.error.code,QString("destination_object_too_large"));QVERIFY2(r.error.message.startsWith("Photos (folder) totals 3 MB; this destination accepts"),qPrintable(r.error.message));
  // The zip's own bound is checked too: many empty files total nothing but still take headers.
  const auto many=d.filePath("Many");for(int i=0;i<300;++i)QVERIFY(make(many+QStringLiteral("/empty-file-with-a-long-name-%1.txt").arg(i),{}));
  p.files={many};limits=ResourceLimits{};limits.maxFileBytes=32*1024;r=store.acquire(id(),p,limits);
  QCOMPARE(r.error.code,QString("input_excessive"));QVERIFY2(r.error.message.startsWith("Many (folder) can zip to up to"),qPrintable(r.error.message));
  // The disk reservation is the folder's peak plus the plain file's size.
  p.files={d.filePath("one.bin"),root};const auto plan=FolderZip::plan(root);
  ArtifactStore::setDiskForTesting(gib+mib,4*gib);r=store.acquire(id(),p,ResourceLimits{});ArtifactStore::setDiskForTesting(-1,-1);
  QCOMPARE(r.error.code,QString("disk_space_insufficient"));QCOMPARE(r.error.details["requiredBytes"].toInteger(),plan.peakBytes+mib);
  // The archive's real size counts toward the waiting-uploads limit like any artifact.
  limits=ResourceLimits{};limits.pendingArtifactBytes=mib+1024;r=store.acquire(id(),p,limits);
  QCOMPARE(r.error.code,QString("input_excessive"));QVERIFY(r.error.message.contains("Waiting uploads"));QCOMPARE(r.error.details["itemIndex"].toInt(),1);
  limits.pendingArtifactBytes=2*mib;r=store.acquire(id(),p,limits);QVERIFY2(!r.error,qPrintable(r.error.message));
  QCOMPARE(entriesOf(d.filePath("spool")).size(),1);
 }
 // PUTGD_FOLDER_ZIP_LARGE=1: over 4 GiB of data. The stored file's first MiB is random
 // (so the probe stores it) and the rest a sparse hole; the deflated one is all hole.
 void largerThanFourGiB() {
  if(!large())QSKIP("set PUTGD_FOLDER_ZIP_LARGE=1 for the >4 GiB case");
  QTemporaryDir d;const auto root=d.filePath("Large");QVERIFY(make(root+"/stored.bin",randomBytes(mib)));
  {QFile f(root+"/stored.bin");QVERIFY(f.open(QIODevice::ReadWrite));QVERIFY(f.resize(4*gib+256*mib));}
  {QFile f(root+"/zeros.bin");QVERIFY(QDir().mkpath(root));QVERIFY(f.open(QIODevice::WriteOnly));QVERIFY(f.resize(5*gib));}
  const auto block=randomBytes(mib);QByteArray pattern;for(int i=0;i<64;++i)pattern+=block;QVERIFY(make(root+"/pattern.bin",pattern));
  for(int i=0;i<16;++i)QVERIFY(make(root+QStringLiteral("/small/%1.txt").arg(i),QByteArray("small ").repeated(i*100+1)));
  QElapsedTimer clock;clock.start();const auto plan=FolderZip::plan(root);QVERIFY(!plan.error);
  resetPeak();const auto before=statusKiB("VmRSS");
  const auto error=FolderZip::write(plan,d.filePath("large.zip"));QVERIFY2(!error,qPrintable(error.message));
  const auto growth=statusKiB("VmHWM")-before;const auto written=clock.elapsed();
  const auto size=QFileInfo(d.filePath("large.zip")).size();QVERIFY(size<=plan.finalBound);QVERIFY(size>4*gib);
  qInfo("large: %lld source bytes -> %lld archive bytes (bound %lld, peak %lld) in %lld ms; peak RSS growth %lld KiB",plan.sourceBytes,size,plan.finalBound,plan.peakBytes,written,growth);
  QVERIFY2(growth>=0&&growth<32*1024,qPrintable(QString::number(growth)));
  const auto zip=readZip(d.filePath("large.zip"),0);QVERIFY2(zip.error.isEmpty(),qPrintable(zip.error));
  QCOMPARE(find(zip,"Large/stored.bin")->method,quint16(0));QCOMPARE(find(zip,"Large/stored.bin")->size,quint64(4*gib+256*mib));
  QCOMPARE(find(zip,"Large/zeros.bin")->method,quint16(8));QCOMPARE(find(zip,"Large/zeros.bin")->size,quint64(5*gib));
  QCOMPARE(find(zip,"Large/pattern.bin")->method,quint16(0));
  QVERIFY(zip.directoryOffset>quint64(4*gib));QCOMPARE(zip.classicOffset,0xffffffffu);
  qint64 above=0;for(const auto& e:zip.entries)if(e.offset>0xffffffffULL)++above;qInfo("large: %lld local headers beyond 4 GiB",above);
  clock.restart();externalChecks(d.filePath("large.zip"));qInfo("large: unzip -t and 7z t took %lld ms",clock.elapsed());
 }
 void moreThan65535Entries() {
  if(!large())QSKIP("set PUTGD_FOLDER_ZIP_LARGE=1 for the >65,535-entry case");
  QTemporaryDir d;const auto root=d.filePath("Many");
  for(int dir=0;dir<200;++dir)for(int i=0;i<350;++i)QVERIFY(make(root+QStringLiteral("/d%1/f%2").arg(dir).arg(i),i%2?QByteArray::number(dir*i):QByteArray{}));
  QElapsedTimer clock;clock.start();const auto plan=FolderZip::plan(root);QVERIFY(!plan.error);QCOMPARE(plan.entries,qint64(1+200+70000));
  const auto planned=clock.restart();resetPeak();const auto before=statusKiB("VmRSS");
  const auto error=FolderZip::write(plan,d.filePath("many.zip"));QVERIFY2(!error,qPrintable(error.message));
  const auto growth=statusKiB("VmHWM")-before;const auto size=QFileInfo(d.filePath("many.zip")).size();
  qInfo("many: %lld entries, plan %lld ms, write %lld ms, %lld archive bytes (bound %lld, peak %lld); peak RSS growth %lld KiB",plan.entries,planned,clock.elapsed(),size,plan.finalBound,plan.peakBytes,growth);
  QVERIFY(size<=plan.finalBound);QVERIFY2(growth>=0&&growth<32*1024,qPrintable(QString::number(growth)));
  const auto zip=readZip(d.filePath("many.zip"));QVERIFY2(zip.error.isEmpty(),qPrintable(zip.error));
  QCOMPARE(zip.total,quint64(70201));QCOMPARE(zip.classicEntries,0xffffu);
  clock.restart();externalChecks(d.filePath("many.zip"));qInfo("many: unzip -t and 7z t took %lld ms",clock.elapsed());
 }
};
QTEST_GUILESS_MAIN(FolderZipTest)
#include "test_folder_zip.moc"
