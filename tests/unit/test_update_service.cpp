#include "app/UpdateService.h"
#include "app/AppImageInstall.h"
#include "core/Log.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <sys/stat.h>
using namespace putgd;
// Records its arguments, prints the canned reply and exits with the canned code.
constexpr auto fakeScript=R"sh(printf '%s\n' "$*" >> "$0.calls"; cat "$0.out"; exit "$(cat "$0.code")")sh";
// The resident's daily update check (#293): schedule, persistence and who is
// told. A shell script stands in for `putgd-cli --check-update`.
class UpdateServiceTest:public QObject {
 Q_OBJECT
 QTemporaryDir dir;
 QDateTime now=QDateTime::fromString("2026-10-20T12:00:00Z",Qt::ISODate);
 static update::Build build(const QString& version,const QString& target="appimage"){
  return update::build(QJsonObject{{"version",version},{"gitCommit","528fe444b0cb0123456789abcdef0123456789ab"},{"dirty",false},{"sourceProvenance","git"}},target,true);
 }
 QString fake() const {return dir.filePath("fake");}
 // The child's reply line, exit code and the arguments of every call.
 void answer(const QJsonObject& reply,int code=0){
  QFile out(fake()+".out");QVERIFY(out.open(QIODevice::WriteOnly));out.write(QJsonDocument(reply).toJson(QJsonDocument::Compact)+"\n");out.close();
  QFile exit(fake()+".code");QVERIFY(exit.open(QIODevice::WriteOnly));exit.write(QByteArray::number(code));
 }
 // Several reply lines: the install child's progress events, then its result.
 void answerLines(const QList<QJsonObject>& lines,int code=0){
  QFile out(fake()+".out");QVERIFY(out.open(QIODevice::WriteOnly));for(const auto& line:lines)out.write(QJsonDocument(line).toJson(QJsonDocument::Compact)+"\n");out.close();
  QFile exit(fake()+".code");QVERIFY(exit.open(QIODevice::WriteOnly));exit.write(QByteArray::number(code));
 }
 QStringList calls() const {QFile file(fake()+".calls");if(!file.open(QIODevice::ReadOnly))return {};return QString::fromUtf8(file.readAll()).split('\n',Qt::SkipEmptyParts);}
 static QJsonObject available(const QString& channel,const QString& latest){
  return {{"schemaVersion",1},{"ok",true},{"error",QJsonObject{}},{"result",QJsonObject{{"channel",channel},{"current","0.1.0"},{"latest",latest},{"updateAvailable",true},
   {"notesUrl","https://github.com/patrick-hudson/put.gd-desktop/releases/tag/v"+latest},{"downloadUrl","https://app.put.gd/dl/"+channel+"/appimage"},{"checkedAt","2026-10-20T12:00:00Z"}}}};
 }
 // Startup delay 0 so a scheduled check runs at once; no jitter.
 std::unique_ptr<UpdateService> service(SettingsService& settings,const update::Build& b){
  auto result=std::make_unique<UpdateService>(settings,dir.path(),"/bin/sh");
  result->setProgram("/bin/sh",{"-c",fakeScript,fake()});
  result->setBuild(b);result->setClock([this]{return now;});
  result->setRandom([](qint64,qint64){return qint64(0);});
  return result;
 }
 void configure(SettingsService& settings,const QJsonObject& updates={},bool notifications=true){
  auto document=Settings::defaults();auto preferences=document["preferences"].toObject();
  preferences["notifications"]=notifications;if(!updates.isEmpty())preferences["updates"]=updates;document["preferences"]=preferences;
  QVERIFY(settings.save(document,settings.revision()));
 }
private slots:
 void init(){QVERIFY(dir.isValid());for(const auto& name:QDir(dir.path()).entryList(QDir::Files))QFile::remove(dir.filePath(name));}
 void unsupportedBuildsNeverCheck(){
  SettingsService settings(dir.filePath("config.json"));configure(settings);
  auto updates=service(settings,build("0.1.1-dev.5+gabc","debian13"));updates->start();
  QCOMPARE(updates->nextCheckInMs(),-1);QCOMPARE(updates->status()["reason"].toString(),QString("development_build"));QCOMPARE(updates->status()["enabled"].toBool(),false);
  Error error;updates->check([&](Error e,QJsonObject){error=e;});QCOMPARE(error.code,QString("update_check_unavailable"));
  QTest::qWait(100);QVERIFY(calls().isEmpty());
 }
 void firstCheckWaitsForStartupAndLaterOnesFollowTheLastAttempt(){
  SettingsService settings(dir.filePath("config.json"));configure(settings);
  UpdateService updates(settings,dir.path(),"/bin/false");updates.setBuild(build("0.1.0"));updates.setClock([this]{return now;});
  updates.setRandom([](qint64 low,qint64){return low;});updates.start();
  // 5 minutes after start, at the earliest.
  QCOMPARE(updates.status()["nextCheckAt"].toString(),now.addSecs(5*60).toString(Qt::ISODate));
  QFile state(dir.filePath("update-check.json"));QVERIFY(state.open(QIODevice::WriteOnly));
  state.write(QJsonDocument(QJsonObject{{"schemaVersion",1},{"attemptedAt",now.addSecs(-3600).toString(Qt::ISODate)},{"attemptedChannel","stable"}}).toJson());state.close();
  UpdateService restarted(settings,dir.path(),"/bin/false");restarted.setBuild(build("0.1.0"));restarted.setClock([this]{return now;});
  restarted.setRandom([](qint64 low,qint64){return low;});restarted.start();
  // 24 hours after the last attempt, minus the 2-hour jitter chosen here.
  QCOMPARE(restarted.status()["nextCheckAt"].toString(),now.addSecs(-3600).addMSecs(UpdateService::intervalMs-UpdateService::jitterMs).toString(Qt::ISODate));
  // Turning daily checks off stops the schedule; Check now still works.
  configure(settings,{{"check",false}});QTRY_COMPARE(restarted.nextCheckInMs(),-1);QVERIFY(restarted.status()["nextCheckAt"].isNull());
 }
 void stableAnnouncesEachVersionOnce(){
  SettingsService settings(dir.filePath("config.json"));configure(settings);answer(available("stable","0.1.1"));
  auto updates=service(settings,build("0.1.0"));QSignalSpy announced(updates.get(),&UpdateService::updateAvailable);updates->start();
  QTRY_COMPARE(announced.size(),1);QCOMPARE(announced.first().first().toJsonObject()["latest"].toString(),QString("0.1.1"));
  QCOMPARE(calls(),QStringList{"--check-update stable"});
  QTRY_VERIFY(!updates->status()["checking"].toBool());
  const auto status=updates->status();QCOMPARE(status["result"].toObject()["updateAvailable"].toBool(),true);QCOMPARE(status["notifies"].toBool(),true);
  // A day later the same version is not announced again.
  now=now.addDays(1);auto restarted=service(settings,build("0.1.0"));QSignalSpy again(restarted.get(),&UpdateService::updateAvailable);restarted->start();
  QTRY_COMPARE(calls().size(),2);QTRY_VERIFY(!restarted->status()["checking"].toBool());QCOMPARE(again.size(),0);
  // A newer one is.
  answer(available("stable","0.1.2"));now=now.addDays(1);auto third=service(settings,build("0.1.0"));QSignalSpy newer(third.get(),&UpdateService::updateAvailable);third->start();
  QTRY_COMPARE(newer.size(),1);
 }
 void nightlyAndSilencedNotificationsOnlyReport(){
  SettingsService settings(dir.filePath("config.json"));configure(settings);answer(available("nightly","0.1.1-nightly.590"));
  auto nightly=service(settings,build("0.1.1-nightly.588"));QSignalSpy announced(nightly.get(),&UpdateService::updateAvailable);nightly->start();
  QTRY_COMPARE(calls().size(),1);QTRY_VERIFY(!nightly->status()["checking"].toBool());
  QCOMPARE(announced.size(),0);QCOMPARE(nightly->status()["result"].toObject()["latest"].toString(),QString("0.1.1-nightly.590"));QCOMPARE(nightly->status()["notifies"].toBool(),false);
  QFile::remove(dir.filePath("update-check.json"));
  configure(settings,{},false);answer(available("stable","0.1.1"));
  auto quiet=service(settings,build("0.1.0"));QSignalSpy silent(quiet.get(),&UpdateService::updateAvailable);quiet->start();
  QTRY_COMPARE(calls().size(),2);QTRY_VERIFY(!quiet->status()["checking"].toBool());QCOMPARE(silent.size(),0);
 }
 void checkNowRepliesAndAChosenChannelWins(){
  SettingsService settings(dir.filePath("config.json"));configure(settings,{{"channel","stable"}});answer(available("stable","0.1.1"));
  UpdateService updates(settings,dir.path(),"/bin/sh");updates.setProgram("/bin/sh",{"-c",fakeScript,fake()});
  updates.setBuild(build("0.1.1-nightly.588"));updates.setClock([this]{return now;});updates.start();
  QCOMPARE(updates.status()["channel"].toString(),QString("stable"));
  QSignalSpy announced(&updates,&UpdateService::updateAvailable);
  bool replied=false;QJsonObject status;updates.check([&](Error error,QJsonObject result){QVERIFY(!error);replied=true;status=result;});
  QTRY_VERIFY(replied);QCOMPARE(calls(),QStringList{"--check-update stable"});
  QCOMPARE(status["result"].toObject()["latest"].toString(),QString("0.1.1"));
  // Someone who checked by hand has seen it: no notification for it later.
  QCOMPARE(announced.size(),0);
  const auto saved=QJsonDocument::fromJson([&]{QFile f(dir.filePath("update-check.json"));f.open(QIODevice::ReadOnly);return f.readAll();}()).object();
  QCOMPARE(saved["notifiedVersion"].toString(),QString("0.1.1"));
 }
 void unreachableFeedWaitsADay(){
  // No result has ever arrived: the scheduled check must not repeat at once.
  SettingsService settings(dir.filePath("config.json"));configure(settings);
  answer({{"schemaVersion",1},{"ok",false},{"result",QJsonObject{}},{"error",QJsonObject{{"code","update_feed_unavailable"},{"message","Cannot read the update feed"},{"details",QJsonObject{}}}}},1);
  auto updates=service(settings,build("0.1.0"));updates->start();QTRY_COMPARE(calls().size(),1);QTRY_VERIFY(!updates->status()["checking"].toBool());
  QTest::qWait(300);QCOMPARE(calls().size(),1);
  QCOMPARE(updates->status()["nextCheckAt"].toString(),now.addMSecs(UpdateService::intervalMs).toString(Qt::ISODate));
  QVERIFY(updates->status()["result"].isNull());QCOMPARE(updates->status()["error"].toObject()["code"].toString(),QString("update_feed_unavailable"));
 }
 void repositoryInstallsLeaveNotificationsToTheSystem(){
  // A stable DEB whose putgd.sources is in use: the check child says so, and
  // the system updater notifies, not put.gd.
  SettingsService settings(dir.filePath("config.json"));configure(settings);
  const QJsonObject managed{{"kind","software-updates"},{"manager","apt"},{"command","sudo apt update && sudo apt upgrade"},
   {"updater",QJsonObject{{"id","gnome-software"},{"name","GNOME Software"},{"program","/usr/bin/gnome-software"}}}};
  auto reply=available("stable","0.1.1");auto result=reply["result"].toObject();result["action"]=managed;reply["result"]=result;answer(reply);
  auto updates=service(settings,build("0.1.0","debian13"));
  QSignalSpy announced(updates.get(),&UpdateService::updateAvailable);updates->start();
  QTRY_COMPARE(calls().size(),1);QTRY_VERIFY(!updates->status()["checking"].toBool());
  QCOMPARE(announced.size(),0);QCOMPARE(updates->status()["notifies"].toBool(),false);
  QCOMPARE(updates->status()["action"].toObject()["kind"].toString(),QString("software-updates"));
  // Opening the updater is the child's job too.
  answer({{"schemaVersion",1},{"ok",true},{"error",QJsonObject{}},{"result",QJsonObject{{"opened","gnome-software"},{"action",managed}}}});
  bool replied=false;QJsonObject opened;updates->openSoftwareUpdates([&](Error error,QJsonObject r){QVERIFY(!error);opened=r;replied=true;});
  QTRY_VERIFY(replied);QCOMPARE(opened["opened"].toString(),QString("gnome-software"));QCOMPARE(calls().last(),QString("--open-software-updates"));
  // Its refusal reaches the caller unchanged.
  answer({{"schemaVersion",1},{"ok",false},{"result",QJsonObject{}},{"error",QJsonObject{{"code","software_updates_unavailable"},{"message","No software updater is installed"},{"details",QJsonObject{{"command","sudo apt update && sudo apt upgrade"}}}}}},1);
  Error error;replied=false;updates->openSoftwareUpdates([&](Error e,QJsonObject){error=e;replied=true;});
  QTRY_VERIFY(replied);QCOMPARE(error.code,QString("software_updates_unavailable"));QCOMPARE(error.details["command"].toString(),QString("sudo apt update && sudo apt upgrade"));
  // The repository file deleted: the next check reports a download, and stable notifies again.
  now=now.addDays(1);result["action"]=QJsonObject{{"kind","download"},{"repository","absent"}};result["latest"]="0.1.2";reply["result"]=result;answer(reply);
  auto later=service(settings,build("0.1.0","debian13"));QSignalSpy again(later.get(),&UpdateService::updateAvailable);later->start();
  QTRY_COMPARE(again.size(),1);QCOMPARE(later->status()["notifies"].toBool(),true);
 }
 void failedCheckKeepsTheLastResult(){
  SettingsService settings(dir.filePath("config.json"));configure(settings);answer(available("stable","0.1.1"));
  auto updates=service(settings,build("0.1.0"));updates->start();QTRY_COMPARE(calls().size(),1);QTRY_VERIFY(!updates->status()["checking"].toBool());
  answer({{"schemaVersion",1},{"ok",false},{"result",QJsonObject{}},{"error",QJsonObject{{"code","update_feed_unavailable"},{"message","Cannot read the update feed"},{"details",QJsonObject{}}}}},1);
  Error error;bool replied=false;updates->check([&](Error e,QJsonObject){error=e;replied=true;});
  QTRY_VERIFY(replied);QCOMPARE(error.code,QString("update_feed_unavailable"));
  const auto status=updates->status();QCOMPARE(status["result"].toObject()["latest"].toString(),QString("0.1.1"));
  QCOMPARE(status["error"].toObject()["code"].toString(),QString("update_feed_unavailable"));
  // A failure waits a day like a success; it never retries in a loop.
  QCOMPARE(updates->status()["nextCheckAt"].toString(),now.addMSecs(UpdateService::intervalMs).toString(Qt::ISODate));
  // A child that prints nothing usable is a failure too.
  answer({},3);replied=false;updates->check([&](Error e,QJsonObject){error=e;replied=true;});QTRY_VERIFY(replied);QCOMPARE(error.code,QString("update_check_failed"));
 }
 // The in-app AppImage update (#293 phase 3): only with confirm, never while
 // busy, progress from the child, then a restart into the new version.
 void installNeedsConfirmAndReportsTheChildsProgress(){
  SettingsService settings(dir.filePath("config.json"));configure(settings);
  auto updates=service(settings,build("0.1.0"));const auto image=dir.filePath("putgd.AppImage");updates->setImage(image);updates->start();
  QCOMPARE(updates->status()["install"].toObject()["available"].toBool(),true);
  Error error;updates->install({},[&](Error e,QJsonObject){error=e;});QCOMPARE(error.code,QString("update_confirm_required"));
  error={};updates->install({{"confirm",false}},[&](Error e,QJsonObject){error=e;});QCOMPARE(error.code,QString("update_confirm_required"));
  error={};updates->install({{"confirm",true},{"method","torrent"}},[&](Error e,QJsonObject){error=e;});QCOMPARE(error.code,QString("invalid_request"));
  error={};updates->install({{"confirm",true},{"image","/tmp/other"}},[&](Error e,QJsonObject){error=e;});QCOMPARE(error.code,QString("invalid_request"));
  updates->setBusy([]{return QString("A recording is running; install the update when it has finished");});
  error={};updates->install({{"confirm",true}},[&](Error e,QJsonObject){error=e;});QCOMPARE(error.code,QString("update_install_busy"));
  QVERIFY(calls().isEmpty());
  updates->setBusy({});
  answerLines({{{"event","progress"},{"phase","download"},{"method","release"},{"bytes",40},{"total",100}},
   {{"schemaVersion",1},{"ok",true},{"error",QJsonObject{}},{"result",QJsonObject{{"installed",true},{"version","0.1.1"},{"method","download"}}}}});
  bool replied=false;QJsonObject status;updates->install({{"confirm",true}},[&](Error e,QJsonObject r){QVERIFY(!e);status=r;replied=true;});
  QVERIFY(replied);QCOMPARE(status["install"].toObject()["state"].toString(),QString("running"));QCOMPARE(status["install"].toObject()["running"].toBool(),true);
  // A second request while it runs is refused.
  error={};updates->install({{"confirm",true}},[&](Error e,QJsonObject){error=e;});QCOMPARE(error.code,QString("update_install_busy"));
  // What the child records on success.
  QVERIFY(appimage::writeState(dir.path(),{{"installed",QJsonObject{{"image",image},{"version","0.1.1"},{"method","download"}}},{"backup",QJsonObject{{"path",dir.filePath(".putgd-update-backup-0.1.0.AppImage")}}}}));
  QTRY_COMPARE(updates->status()["install"].toObject()["state"].toString(),QString("installed"));
  // (The daily check that started with the service runs too.)
  QVERIFY(calls().contains("--install-update stable --method auto --state "+dir.path()));
  const auto install=updates->status()["install"].toObject();
  QCOMPARE(install["bytes"].toInteger(),40);QCOMPARE(install["method"].toString(),QString("release"));QCOMPARE(install["result"].toObject()["version"].toString(),QString("0.1.1"));
  QCOMPARE(install["restartRequired"].toBool(),true);QCOMPARE(install["backup"].toString(),dir.filePath(".putgd-update-backup-0.1.0.AppImage"));
  // Installed and waiting: no second install, and a restart starts the image from the host, then quits.
  error={};updates->install({{"confirm",true}},[&](Error e,QJsonObject){error=e;});QCOMPARE(error.code,QString("update_restart_required"));
  qint64 pid=0;QString started;bool quit=false;
  updates->setRestarter([&](qint64 p,const QString& i){pid=p;started=i;return true;});updates->setQuit([&]{quit=true;});
  error={};updates->restart({},[&](Error e,QJsonObject){error=e;});QCOMPARE(error.code,QString("update_confirm_required"));QVERIFY(!quit);
  QJsonObject restarting;updates->restart({{"confirm",true}},[&](Error e,QJsonObject r){QVERIFY(!e);restarting=r;});
  QCOMPARE(restarting["version"].toString(),QString("0.1.1"));QCOMPARE(pid,QCoreApplication::applicationPid());QCOMPARE(started,image);
  QTRY_VERIFY(quit);
 }
 void aFailedInstallKeepsItsReasonAndNothingWaits(){
  SettingsService settings(dir.filePath("config.json"));configure(settings);
  auto updates=service(settings,build("0.1.0"));updates->setImage(dir.filePath("putgd.AppImage"));updates->start();
  answerLines({{{"schemaVersion",1},{"ok",false},{"result",QJsonObject{}},{"error",QJsonObject{{"code","update_verification_failed"},{"message","The download doesn't match the release's SHA256SUMS; nothing was changed"},{"details",QJsonObject{{"check","sha256"}}}}}}},1);
  updates->install({{"confirm",true},{"method","download"}},[](Error,QJsonObject){});
  QTRY_COMPARE(updates->status()["install"].toObject()["state"].toString(),QString("failed"));
  const auto install=updates->status()["install"].toObject();
  QCOMPARE(install["error"].toObject()["code"].toString(),QString("update_verification_failed"));QCOMPARE(install["restartRequired"].toBool(),false);
  Error error;updates->restart({{"confirm",true}},[&](Error e,QJsonObject){error=e;});QCOMPARE(error.code,QString("update_restart_unavailable"));
  // A child that dies without a result is a failure too.
  answerLines({},9);updates->install({{"confirm",true}},[](Error,QJsonObject){});
  QTRY_COMPARE(updates->status()["install"].toObject()["error"].toObject()["code"].toString(),QString("update_install_failed"));
 }
 void onlyAnAppImageOnItsOwnChannelInstalls(){
  SettingsService settings(dir.filePath("config.json"));configure(settings);
  auto deb=service(settings,build("0.1.0","debian13"));deb->setImage({});deb->start();
  QCOMPARE(deb->status()["install"].toObject()["reason"].toString(),QString("not_appimage"));
  Error error;deb->install({{"confirm",true}},[&](Error e,QJsonObject){error=e;});QCOMPARE(error.code,QString("update_install_unavailable"));
  auto unpacked=service(settings,build("0.1.0"));unpacked->setImage({});unpacked->start();
  QCOMPARE(unpacked->status()["install"].toObject()["reason"].toString(),QString("not_running_from_appimage"));
  // A stable image watching nightly would get another channel's image.
  configure(settings,{{"channel","nightly"}});
  auto other=service(settings,build("0.1.0"));other->setImage(dir.filePath("putgd.AppImage"));other->start();
  QCOMPARE(other->status()["install"].toObject()["reason"].toString(),QString("other_channel"));
  QVERIFY(calls().isEmpty());
 }
 // After an update the saved check names the version that now runs: it's not offered again.
 void aSavedResultIsComparedWithTheVersionThatRunsNow(){
  SettingsService settings(dir.filePath("config.json"));configure(settings);
  QFile state(dir.filePath("update-check.json"));QVERIFY(state.open(QIODevice::WriteOnly));
  state.write(QJsonDocument(QJsonObject{{"schemaVersion",1},{"attemptedAt",now.toString(Qt::ISODate)},{"attemptedChannel","stable"},{"checkedAt",now.toString(Qt::ISODate)},
   {"result",QJsonObject{{"channel","stable"},{"current","0.1.0"},{"latest","0.1.1"},{"updateAvailable",true}}}}).toJson());state.close();
  auto old=service(settings,build("0.1.0"));old->start();
  QCOMPARE(old->status()["result"].toObject()["updateAvailable"].toBool(),true);
  auto updated=service(settings,build("0.1.1"));updated->start();
  const auto result=updated->status()["result"].toObject();
  QCOMPARE(result["updateAvailable"].toBool(),false);QCOMPARE(result["current"].toString(),QString("0.1.1"));QCOMPARE(result["latest"].toString(),QString("0.1.1"));
 }
 void theFirstStartOfTheNewVersionRemovesTheBackup(){
  SettingsService settings(dir.filePath("config.json"));configure(settings);
  const auto image=dir.filePath("putgd.AppImage"),backup=dir.filePath(".putgd-update-backup-0.1.0.AppImage");
  {QFile file(backup);QVERIFY(file.open(QIODevice::WriteOnly));file.write("old image");}
  struct stat kept{};QCOMPARE(::stat(QFile::encodeName(backup).constData(),&kept),0);
  QVERIFY(appimage::writeState(dir.path(),{{"installed",QJsonObject{{"image",image},{"version","0.1.1"}}},
   {"backup",QJsonObject{{"path",backup},{"device",QString::number(quint64(kept.st_dev))},{"inode",QString::number(quint64(kept.st_ino))},{"size",qint64(kept.st_size)},{"mtime",qint64(kept.st_mtim.tv_sec)}}}}));
  // Still the old version: kept, and a restart is due.
  auto old=service(settings,build("0.1.0"));old->setImage(image);old->start();
  QVERIFY(QFileInfo::exists(backup));QCOMPARE(old->status()["install"].toObject()["restartRequired"].toBool(),true);
  auto updated=service(settings,build("0.1.1"));updated->setImage(image);updated->start();
  QVERIFY(!QFileInfo::exists(backup));
  const auto install=updated->status()["install"].toObject();
  QCOMPARE(install["restartRequired"].toBool(),false);QVERIFY(install["backup"].isNull());QCOMPARE(install["installed"].toObject()["backupRemoved"].toBool(),true);
 }
};
QTEST_GUILESS_MAIN(UpdateServiceTest)
#include "test_update_service.moc"
