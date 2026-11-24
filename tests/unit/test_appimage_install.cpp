#include "app/AppImageInstall.h"
#include "fixtures/ElfImage.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMap>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#include <sys/stat.h>
#include <unistd.h>
using namespace putgd;
namespace {
const QString nightly="zsync|https://app.put.gd/dl/nightly/appimage.zsync";
using putgd::test::elfImage;
void write(const QString& path,const QByteArray& bytes,QFile::Permissions permissions=QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner|QFile::ReadGroup|QFile::ExeGroup) {
 QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));QCOMPARE(file.write(bytes),bytes.size());file.close();QVERIFY(file.setPermissions(permissions));
}
QByteArray read(const QString& path) {QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};}
QString hex(const QByteArray& bytes) {return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
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
 // #412: an image put.gd must not replace is refused with the reason Settings
 // shows after "The update wasn't installed: ", and the file is left alone.
 // CI can't make another user's file or a read-only mount, so the system's
 // answers come through the Access seam.
 void refusesAnImageItMustNotReplace() {
  QTemporaryDir dir;const auto path=dir.filePath("putgd-0.1.1-nightly.800-x86_64.AppImage");const auto bytes=elfImage(nightly.toUtf8());write(path,bytes);
  const auto folder=QFileInfo(path).canonicalPath();
  QStringList asked;
  const appimage::Access allowed{
   [&](const QString& file){asked.append("owner "+file);return true;},
   [&](const QString& directory){asked.append("readOnly "+directory);return false;},
   [&](const QString& directory){asked.append("writable "+directory);return true;}};
  Error error;QVERIFY(!appimage::inspect(path,1024,error,allowed).path.isEmpty());QVERIFY(!error);
  // Each probe is asked about the file itself and its real folder.
  QCOMPARE(asked,(QStringList{"owner "+QFileInfo(path).canonicalFilePath(),"readOnly "+folder,"writable "+folder}));
  auto otherOwner=allowed;otherOwner.ownedByUser=[](const QString&){return false;};
  auto readOnly=allowed;readOnly.readOnly=[](const QString&){return true;};
  auto unwritable=allowed;unwritable.writable=[](const QString&){return false;};
  const QList<std::pair<appimage::Access,QString>> refusals{
   {otherOwner,"The AppImage belongs to another user, so put.gd won't replace it"},
   {readOnly,"The AppImage's folder is on a read-only file system"},
   {unwritable,"put.gd can't write to the AppImage's folder"}};
  for(const auto& [access,message]:refusals) {
   error={};const auto image=appimage::inspect(path,1024,error,access);
   QCOMPARE(error.code,QString("update_image_not_writable"));QCOMPARE(error.message,message);
   QCOMPARE(error.details,(QJsonObject{{"directory",folder}}));QVERIFY(image.path.isEmpty());
  }
  // Another user's file on a read-only file system: the owner is the reason given.
  auto both=otherOwner;both.readOnly=readOnly.readOnly;both.writable=unwritable.writable;
  error={};appimage::inspect(path,1024,error,both);QCOMPARE(error.message,refusals.first().second);
  // Refusing changes nothing in the folder.
  QCOMPARE(read(path),bytes);QCOMPARE(QDir(dir.path()).entryList(QDir::AllEntries|QDir::Hidden|QDir::NoDotAndDotDot),QStringList{QFileInfo(path).fileName()});
 }
 // The real answers behind the seam: this user's file and folder, a folder
 // without write access, and a read-only mount when the host has one.
 void hostAccessAsksTheSystem() {
  const auto host=appimage::hostAccess();
  QTemporaryDir dir;const auto folder=dir.filePath("ro");QVERIFY(QDir().mkdir(folder));
  const auto path=folder+"/putgd.AppImage";write(path,elfImage(nightly.toUtf8()));
  QVERIFY(host.ownedByUser(path));QVERIFY(!host.readOnly(folder));QVERIFY(host.writable(folder));
  if(::getuid()!=0) {
   // The root directory belongs to root.
   QVERIFY(!host.ownedByUser("/"));QVERIFY(!host.writable("/"));
   QVERIFY(QFile::setPermissions(folder,QFile::ReadOwner|QFile::ExeOwner));
   const auto restore=qScopeGuard([&]{QFile::setPermissions(folder,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);});
   QVERIFY(!host.writable(folder));
   Error error;appimage::inspect(path,1024,error);
   QCOMPARE(error.code,QString("update_image_not_writable"));QCOMPARE(error.message,QString("put.gd can't write to the AppImage's folder"));
  }
  // Any read-only mount this host has (a snap, /sys in a container) reads as one.
  QFile mounts("/proc/self/mounts");QVERIFY(mounts.open(QIODevice::ReadOnly));
  // A later mount on the same point covers an earlier one.
  QMap<QString,bool> readOnlyAt;
  for(const auto& line:QString::fromUtf8(mounts.readAll()).split('\n',Qt::SkipEmptyParts)) {
   const auto fields=line.split(' ');
   if(fields.size()>3&&fields[2]!="autofs"&&!fields[1].contains('\\'))readOnlyAt[fields[1]]=fields[3].split(',').contains("ro");
  }
  QString readOnlyMount;
  for(auto it=readOnlyAt.cbegin();it!=readOnlyAt.cend()&&readOnlyMount.isEmpty();++it)if(it.value()&&QFileInfo(it.key()).isDir())readOnlyMount=it.key();
  if(readOnlyMount.isEmpty())qInfo("No read-only mount on this host; the seam covers the refusal");
  else QVERIFY2(host.readOnly(readOnlyMount),qPrintable(readOnlyMount));
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
 // #412: the new version didn't start, so the user moved the backup back
 // (docs/design/updates.md, "If the new version won't start"). The old version's
 // next start forgets the install instead of waiting for a restart.
 void forgetsAnUpdateWhoseBackupWasMovedBack() {
  QTemporaryDir dir;const auto path=dir.filePath("putgd-0.1.1-nightly.800-x86_64.AppImage");const auto old=elfImage(nightly.toUtf8(),"800");write(path,old);
  Error error;const auto image=appimage::inspect(path,1,error);QVERIFY(!error);
  const auto candidate=dir.filePath("candidate");write(candidate,elfImage(nightly.toUtf8(),"801"));
  const auto backup=appimage::swap(candidate,image,"0.1.1-nightly.800",error);QVERIFY(!error);
  QTemporaryDir state;const QJsonObject installed{{"image",image.path},{"version","0.1.1-nightly.801"},{"previousVersion","0.1.1-nightly.800"}};
  QVERIFY(appimage::writeState(state.path(),{{"backup",backup},{"installed",installed}}));
  // The guide's command: mv .putgd-update-backup-VERSION.AppImage over the image.
  QCOMPARE(::rename(QFile::encodeName(backup["path"].toString()).constData(),QFile::encodeName(image.path).constData()),0);
  const auto settled=appimage::settle(state.path(),image.path,"0.1.1-nightly.800");
  QVERIFY(settled["installed"].isNull());QVERIFY(settled["backup"].isNull());
  QCOMPARE(settled["restored"].toObject(),(QJsonObject{{"image",image.path},{"version","0.1.1-nightly.801"},{"runningVersion","0.1.1-nightly.800"}}));
  QCOMPARE(appimage::readState(state.path())["restored"],settled["restored"]);
  QCOMPARE(read(image.path),old);QCOMPARE(QDir(dir.path()).entryList(QDir::AllEntries|QDir::Hidden|QDir::NoDotAndDotDot),QStringList{QFileInfo(path).fileName()});
  // A copy put back (not the recorded file) proves nothing: the install still waits.
  write(candidate,elfImage(nightly.toUtf8(),"801"));
  const auto again=appimage::swap(candidate,image,"0.1.1-nightly.800",error);QVERIFY(!error);
  QVERIFY(appimage::writeState(state.path(),{{"backup",again},{"installed",installed}}));
  QVERIFY(QFile::remove(image.path));QVERIFY(QFile::copy(again["path"].toString(),image.path));
  const auto waiting=appimage::settle(state.path(),image.path,"0.1.1-nightly.800");
  QCOMPARE(waiting["installed"].toObject()["version"].toString(),QString("0.1.1-nightly.801"));QCOMPARE(waiting["backup"].toObject(),again);QVERIFY(QFileInfo::exists(again["path"].toString()));
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
};
QTEST_GUILESS_MAIN(AppImageInstallTest)
#include "test_appimage_install.moc"
