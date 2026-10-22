#include "app/AppImageInstall.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#include <sys/stat.h>
#include <unistd.h>
using namespace putgd;
namespace {
const QString nightly="zsync|https://app.put.gd/dl/nightly/appimage.zsync";
// A little-endian ELF64 file with the AppImage type 2 magic and a .upd_info
// section, as the runtime lays it out; payload makes versions differ.
QByteArray elfImage(const QByteArray& updateInformation,const QByteArray& payload={}) {
 const QByteArray names("\0.shstrtab\0.upd_info\0",21);
 auto info=updateInformation;info.append(QByteArray(1024-info.size(),'\0'));
 QByteArray header(64,'\0');
 header.replace(0,4,"\x7f""ELF");header[4]=2;header[5]=1;header[6]=1;header[8]='A';header[9]='I';header[10]=2;
 const quint64 tableOffset=64+names.size()+info.size();
 qToLittleEndian<quint64>(tableOffset,header.data()+0x28);
 qToLittleEndian<quint16>(64,header.data()+0x3A);qToLittleEndian<quint16>(3,header.data()+0x3C);qToLittleEndian<quint16>(1,header.data()+0x3E);
 QByteArray table(3*64,'\0');
 const auto section=[&](int index,quint32 name,quint64 offset,quint64 size){
  qToLittleEndian<quint32>(name,table.data()+index*64);qToLittleEndian<quint64>(offset,table.data()+index*64+24);qToLittleEndian<quint64>(size,table.data()+index*64+32);
 };
 section(1,1,64,names.size());section(2,11,64+names.size(),info.size());
 return header+names+info+table+payload;
}
void write(const QString& path,const QByteArray& bytes,QFile::Permissions permissions=QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner|QFile::ReadGroup|QFile::ExeGroup) {
 QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));QCOMPARE(file.write(bytes),bytes.size());file.close();QVERIFY(file.setPermissions(permissions));
}
QByteArray read(const QString& path) {QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};}
QString hex(const QByteArray& bytes) {return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
// appimageupdatetool's output for lab 886 -> 887 on m74 (#293 phase 3), as the
// pipe put.gd reads it delivered it; only the home folder is renamed.
const QByteArray appimageupdatetoolOutput=
 "zsync2: used 114933760 local, fetched 1251832\n"
 "\x1b[2K\r100.00% done (109.89 of 109.89 MiB)...\n"
 "Validation warning: AppImage not signed\n"
 "Update successful. New file created: /home/lab/Applications/.putgd-update-bnesRJ/putgd-0.1.1-nightly.887-x86_64.AppImage\n";
ino_t inode(const QString& path) {struct stat info{};return ::stat(QFile::encodeName(path).constData(),&info)==0?info.st_ino:0;}
mode_t mode(const QString& path) {struct stat info{};return ::stat(QFile::encodeName(path).constData(),&info)==0?(info.st_mode&07777):0;}
}
// The file half of the in-app AppImage update (#293 phase 3): what is checked,
// what is replaced, and what is never touched.
class AppImageInstallTest:public QObject {
 Q_OBJECT
private slots:
 void inspectsTheRunningImage() {
  QTemporaryDir dir;const auto path=dir.filePath("putgd-0.1.1-nightly.800-x86_64.AppImage");
  write(path,elfImage(nightly.toUtf8()));
  Error error;const auto image=appimage::inspect(path,1024,error);
  QVERIFY2(!error,qPrintable(error.message));
  QCOMPARE(image.path,QFileInfo(path).canonicalFilePath());QCOMPARE(image.directory,QFileInfo(path).canonicalPath());QCOMPARE(image.updateInformation,nightly);
  // A link to the image resolves to the file itself.
  const auto link=dir.filePath("put.gd.AppImage");QVERIFY(QFile::link(path,link));
  QCOMPARE(appimage::inspect(link,1024,error).path,image.path);
  // Not an AppImage, missing, relative.
  write(dir.filePath("plain"),"#!/bin/sh\n");
  for(const auto& bad:{dir.filePath("plain"),dir.filePath("missing"),QString("relative.AppImage"),QString()}){
   error={};appimage::inspect(bad,1024,error);QCOMPARE(error.code,QString("update_install_unavailable"));
  }
  // No room for another image.
  error={};appimage::inspect(path,qint64(1)<<60,error);QCOMPARE(error.code,QString("update_disk_full"));
 }
 void refusesAFolderItCannotWrite() {
  if(::getuid()==0)QSKIP("root writes anywhere");
  QTemporaryDir dir;const auto folder=dir.filePath("ro");QVERIFY(QDir().mkdir(folder));
  const auto path=folder+"/putgd.AppImage";write(path,elfImage(nightly.toUtf8()));
  QVERIFY(QFile::setPermissions(folder,QFile::ReadOwner|QFile::ExeOwner));
  Error error;appimage::inspect(path,1024,error);
  QFile::setPermissions(folder,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
  QCOMPARE(error.code,QString("update_image_not_writable"));
 }
 void readsTheReleaseLine() {
  const QByteArray sums="0000000000000000000000000000000000000000000000000000000000000001  putgd-0.1.1-nightly.801-x86_64.AppImage\n"
                        "00000000000000000000000000000000000000000000000000000000000000ff *putgd-0.1.1-nightly.801-x86_64.AppImage.zsync\n";
  QCOMPARE(appimage::sumsLine(sums,"putgd-0.1.1-nightly.801-x86_64.AppImage"),QString("0000000000000000000000000000000000000000000000000000000000000001"));
  QCOMPARE(appimage::sumsLine(sums,"putgd-0.1.1-nightly.801-x86_64.AppImage.zsync"),QString("00000000000000000000000000000000000000000000000000000000000000ff"));
  QVERIFY(appimage::sumsLine(sums,"putgd-0.1.1-nightly.801-x86_64").isEmpty());
  // Listed twice is ambiguous, upper case is not sha256sum's format.
  QVERIFY(appimage::sumsLine(sums+sums,"putgd-0.1.1-nightly.801-x86_64.AppImage").isEmpty());
  QVERIFY(appimage::sumsLine("00000000000000000000000000000000000000000000000000000000000000FF  a\n","a").isEmpty());
 }
 void verifiesHashMagicAndUpdateInformation() {
  QTemporaryDir dir;const auto path=dir.filePath("new");const auto bytes=elfImage(nightly.toUtf8(),"801");write(path,bytes);
  QVERIFY(!appimage::verify(path,hex(bytes),nightly));
  auto error=appimage::verify(path,hex("tampered"),nightly);QCOMPARE(error.code,QString("update_verification_failed"));QCOMPARE(error.details["check"].toString(),QString("sha256"));
  // Another channel's image is not this update, even with a matching hash.
  error=appimage::verify(path,hex(bytes),"zsync|https://app.put.gd/dl/stable/appimage.zsync");QCOMPARE(error.details["check"].toString(),QString("updateInformation"));
  auto plain=bytes;plain[8]=0;write(path,plain);
  error=appimage::verify(path,hex(plain),nightly);QCOMPARE(error.details["check"].toString(),QString("appimage"));
 }
 void swapsAtomicallyAndKeepsAHiddenBackup() {
  QTemporaryDir dir;const auto path=dir.filePath("putgd-0.1.1-nightly.800-x86_64.AppImage");
  const auto old=elfImage(nightly.toUtf8(),"800"),next=elfImage(nightly.toUtf8(),"801");
  write(path,old,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner|QFile::ReadGroup|QFile::ExeGroup|QFile::ReadOther|QFile::ExeOther);
  const auto beside=dir.filePath("putgd-0.1.1-nightly.799-x86_64.AppImage");write(beside,elfImage(nightly.toUtf8(),"799"));
  const auto oldInode=inode(path);
  Error error;const auto image=appimage::inspect(path,next.size(),error);QVERIFY(!error);
  const auto staging=dir.filePath(".putgd-update-AbC123");QVERIFY(QDir().mkdir(staging));
  const auto candidate=staging+"/new";write(candidate,next,QFile::ReadOwner|QFile::WriteOwner);
  const auto backup=appimage::swap(candidate,image,"0.1.1-nightly.800",error);
  QVERIFY2(!error,qPrintable(error.message));
  QCOMPARE(read(path),next);
  // Same mode as the old file, executable as before.
  QCOMPARE(mode(path),mode_t(0755));
  // The backup is the old inode, hidden from the login script's putgd-*.AppImage scan.
  QCOMPARE(QFileInfo(backup["path"].toString()).fileName(),QString(".putgd-update-backup-0.1.1-nightly.800.AppImage"));
  QCOMPARE(inode(backup["path"].toString()),oldInode);QCOMPARE(read(backup["path"].toString()),old);QCOMPARE(backup["copied"].toBool(),false);
  QCOMPARE(read(beside),elfImage(nightly.toUtf8(),"799"));
  QVERIFY(!QFileInfo::exists(candidate));
  // A second backup of the same version gets its own name; nothing is overwritten.
  write(candidate,elfImage(nightly.toUtf8(),"802"));
  const auto second=appimage::swap(candidate,image,"0.1.1-nightly.800",error);QVERIFY(!error);
  QCOMPARE(QFileInfo(second["path"].toString()).fileName(),QString(".putgd-update-backup-0.1.1-nightly.800-2.AppImage"));
  QCOMPARE(read(backup["path"].toString()),old);
 }
 void settlesOnlyWhatItRecorded() {
  QTemporaryDir dir;const auto path=dir.filePath("putgd.AppImage");write(path,elfImage(nightly.toUtf8(),"800"));
  Error error;const auto image=appimage::inspect(path,1,error);QVERIFY(!error);
  const auto candidate=dir.filePath("candidate");write(candidate,elfImage(nightly.toUtf8(),"801"));
  const auto backup=appimage::swap(candidate,image,"0.1.1-nightly.800",error);QVERIFY(!error);
  QTemporaryDir state;const auto staging=dir.filePath(".putgd-update-Xy12Zz");QVERIFY(QDir().mkdir(staging));write(staging+"/download.part","partial");
  QVERIFY(appimage::writeState(state.path(),{{"staging",staging},{"backup",backup},{"installed",QJsonObject{{"image",image.path},{"version","0.1.1-nightly.801"}}}}));
  // Still the old version running (no restart yet): the backup stays; the staging folder goes.
  auto settled=appimage::settle(state.path(),image.path,"0.1.1-nightly.800");
  QVERIFY(!QFileInfo::exists(staging));QVERIFY(settled["staging"].isNull());QVERIFY(QFileInfo::exists(backup["path"].toString()));
  // The new version started from the path: the backup goes.
  settled=appimage::settle(state.path(),image.path,"0.1.1-nightly.801");
  QVERIFY(!QFileInfo::exists(backup["path"].toString()));QVERIFY(settled["backup"].isNull());QCOMPARE(settled["installed"].toObject()["backupRemoved"].toBool(),true);
  QCOMPARE(appimage::readState(state.path())["installed"].toObject()["version"].toString(),QString("0.1.1-nightly.801"));
  // A backup the user replaced is no longer put.gd's file: it stays, only the record goes.
  write(candidate,elfImage(nightly.toUtf8(),"802"));
  const auto again=appimage::swap(candidate,image,"0.1.1-nightly.801",error);QVERIFY(!error);
  QVERIFY(QFile::remove(again["path"].toString()));write(again["path"].toString(),"the user's own file");
  QVERIFY(appimage::writeState(state.path(),{{"backup",again},{"installed",QJsonObject{{"image",image.path},{"version","0.1.1-nightly.802"}}}}));
  settled=appimage::settle(state.path(),image.path,"0.1.1-nightly.802");
  QCOMPARE(read(again["path"].toString()),QByteArray("the user's own file"));QVERIFY(settled["backup"].isNull());QCOMPARE(settled["installed"].toObject()["backupRemoved"].toBool(),false);
 }
 void removesOnlyItsOwnStagingFolders() {
  QTemporaryDir dir;
  const auto ours=dir.filePath(".putgd-update-AbC123");QVERIFY(QDir().mkdir(ours));write(ours+"/seed.AppImage","x");write(ours+"/.zs-old","y");
  QVERIFY(appimage::removeStaging(ours));QVERIFY(!QFileInfo::exists(ours));
  const auto other=dir.filePath("Applications");QVERIFY(QDir().mkdir(other));write(other+"/keep","x");
  QVERIFY(!appimage::removeStaging(other));QVERIFY(QFileInfo::exists(other+"/keep"));
  // A folder inside means it is not what the install made.
  const auto nested=dir.filePath(".putgd-update-Nest12");QVERIFY(QDir().mkpath(nested+"/inner"));write(nested+"/inner/keep","x");
  QVERIFY(!appimage::removeStaging(nested));QVERIFY(QFileInfo::exists(nested+"/inner/keep"));
  const auto link=dir.filePath(".putgd-update-Link12");QVERIFY(QFile::link(other,link));
  QVERIFY(!appimage::removeStaging(link));QVERIFY(QFileInfo::exists(other+"/keep"));
 }
 void findsTheHostsZsyncToolAndPinsTheRelease() {
  QTemporaryDir dir;
  QProcessEnvironment host;host.insert("PATH",dir.path());
  QVERIFY(!appimage::findZsyncTool(host));
  write(dir.filePath("zsync2"),"#!/bin/sh\n");
  QCOMPARE(appimage::findZsyncTool(host).kind,QString("zsync2"));
  write(dir.filePath("appimageupdatetool"),"#!/bin/sh\n");
  const auto tool=appimage::findZsyncTool(host);
  QCOMPARE(tool.kind,QString("appimageupdatetool"));QCOMPARE(tool.path,dir.filePath("appimageupdatetool"));
  const QString url="https://pkg.put.gd/releases/v0.1.1-nightly.801/putgd-0.1.1-nightly.801-x86_64.AppImage.zsync";
  QCOMPARE(appimage::zsyncArguments(tool,"/s/seed.AppImage","/s/out.AppImage",url),(QStringList{"-u","zsync|"+url,"/s/seed.AppImage"}));
  QCOMPARE(appimage::zsyncArguments({"zsync2","/usr/bin/zsync2"},"/s/seed.AppImage","/s/out.AppImage",url),(QStringList{"-i","/s/seed.AppImage","-o","/s/out.AppImage",url}));
  // zsync2 gets the host's CA bundle (#297, #329).
  QCOMPARE(appimage::zsyncEnvironment(host,"/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem").value("SSL_CERT_FILE"),QString("/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem"));
  QVERIFY(!appimage::zsyncEnvironment(host,{}).contains("SSL_CERT_FILE"));
 }
 // #349: zsync.output keeps what the tool's terminal would show, not its
 // escape sequences and every progress redraw.
 void cleansTheToolsOutput() {
  QCOMPARE(appimage::zsyncOutputTail(appimageupdatetoolOutput),
           QString("zsync2: used 114933760 local, fetched 1251832 | 100.00% done (109.89 of 109.89 MiB)... | Validation warning: AppImage not signed | "
                   "Update successful. New file created: /home/lab/Applications/.putgd-update-bnesRJ/putgd-0.1.1-nightly.887-x86_64.AppImage"));
  for(const auto& line:appimage::terminalLines(appimageupdatetoolOutput))
   for(const auto c:line)QVERIFY2(c.unicode()>=0x20&&c.unicode()!=0x7f,qPrintable(line));
  // A redraw overwrites from the line's start; an erase clears what it names.
  QCOMPARE(appimage::terminalLines("abcdef\rXY\n"),QStringList{"XYcdef"});
  QCOMPARE(appimage::terminalLines("  0.00% done (0.00 of 109.89 MiB)...\x1b[2K\r 50.00%\x1b[K\n"),QStringList{"50.00%"});
  QCOMPARE(appimage::terminalLines("abcdef\x1b[1K\rgh"),QStringList{"gh"});
  // Colours, titles, CRLF, backspace, bell and other controls go; blank lines too.
  QCOMPARE(appimage::terminalLines("\x1b[1;31mError:\x1b[0m bad\r\n\r\n\x1b]0;title\x07ok\x1b]2;t\x1b\\\tdone\nab\bc\x07\x01\x7f\n\x1b(B\x1b" "7x\n\xc2\x9b" "2Ky\n"),
           (QStringList{"Error: bad","ok done","ac","x","y"}));
  // Only the last four lines, and at most 400 characters.
  QCOMPARE(appimage::zsyncOutputTail("1\n2\n3\n4\n5\n6\n"),QString("3 | 4 | 5 | 6"));
  QCOMPARE(appimage::zsyncOutputTail(QByteArray(1000,'x')).size(),400);
  QVERIFY(appimage::zsyncOutputTail("\x1b[2K\r\n\r").isEmpty());
 }
};
QTEST_GUILESS_MAIN(AppImageInstallTest)
#include "test_appimage_install.moc"
