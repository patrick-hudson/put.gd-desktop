#include "app/UpdateService.h"
#include "app/AppImageInstall.h"
#include "core/Log.h"
#include "core/SystemUpdates.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <sys/stat.h>
#include "fixtures/Waits.h"
using namespace putgd;
// Records its arguments, prints the canned reply and exits with the canned code.
constexpr auto fakeScript=R"sh(printf '%s\n' "$*" >> "$0.calls"; cat "$0.out"; exit "$(cat "$0.code")")sh";
// The resident's daily update check (#293): schedule, persistence and who is
// told. A shell script stands in for `putgd-cli --check-update`.
class UpdateServiceTest:public QObject {
 Q_OBJECT
 QTemporaryDir dir;
 QByteArray oldSystemRoot;
 bool hadSystemRoot=false;
 QString repositoryFile(const QString& target) const {return dir.path()+(target=="fedora44"?"/system/etc/yum.repos.d/putgd.repo":"/system/etc/apt/sources.list.d/putgd.sources");}
 void repository(const QString& target,const QString& channel="stable",const QString& state="managed") {
  const auto path=repositoryFile(target);QVERIFY(QDir().mkpath(QFileInfo(path).path()));
  QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));
  const auto base=state=="foreign"?QString("https://example.invalid"):update::packageRepository();
  const auto text=state=="invalid"?QString("not a repository\n"):target=="fedora44"?
   "[putgd]\nbaseurl="+base+"/rpm/fedora/44/x86_64/"+channel+"\nenabled="+(state=="disabled"?"0":"1")+"\n":
   "Types: deb\nURIs: "+base+"/apt/"+target+"\nSuites: "+channel+"\nComponents: main\nEnabled: "+(state=="disabled"?"no":"yes")+"\n";
  file.write(text.toUtf8());
 }
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
 // #344: what the last check of the other channel found decides the switch.
 static QJsonObject found(const QString& channel,const QString& current,const QJsonValue& latest,bool newer){
  return {{"schemaVersion",1},{"ok",true},{"error",QJsonObject{}},{"result",QJsonObject{{"channel",channel},{"current",current},{"latest",latest},{"updateAvailable",newer},
   {"tag",latest.isString()?QJsonValue("v"+latest.toString()):QJsonValue()},{"asset",QJsonValue()}}}};
 }
private slots:
 void init(){
  QVERIFY(dir.isValid());for(const auto& name:QDir(dir.path()).entryList(QDir::Files))QFile::remove(dir.filePath(name));
  QDir(dir.path()+"/system").removeRecursively();
  hadSystemRoot=qEnvironmentVariableIsSet("PUTGD_UPDATE_SYSTEM_ROOT");oldSystemRoot=qgetenv("PUTGD_UPDATE_SYSTEM_ROOT");
  qputenv("PUTGD_UPDATE_SYSTEM_ROOT",(dir.path()+"/system").toUtf8());
 }
 void cleanup(){if(hadSystemRoot)qputenv("PUTGD_UPDATE_SYSTEM_ROOT",oldSystemRoot);else qunsetenv("PUTGD_UPDATE_SYSTEM_ROOT");}
 void startupUsesLocalRepositoryWithoutAFeed_data(){
  QTest::addColumn<QString>("target");QTest::addColumn<QString>("state");QTest::addColumn<QString>("chosen");QTest::addColumn<bool>("checks");
  for(const auto& target:{"ubuntu24.04","ubuntu26.04","debian13","fedora44","appimage"})
   for(const auto& state:{"managed","absent","disabled","foreign","invalid"})
    for(const auto& chosen:{"build","nightly"})
     for(const bool checks:{false,true})QTest::newRow(qPrintable(QString("%1-%2-%3-%4").arg(target,state,chosen).arg(checks)))<<QString(target)<<QString(state)<<QString(chosen)<<checks;
 }
 void startupUsesLocalRepositoryWithoutAFeed(){
  QFETCH(QString,target);QFETCH(QString,state);QFETCH(QString,chosen);QFETCH(bool,checks);
  if(state!="absent")repository(target,"stable",state);
  SettingsService settings(dir.filePath("config.json"));configure(settings,{{"channel",chosen},{"check",checks}});
  const auto document=settings.document();const auto revision=settings.revision();
  auto updates=service(settings,build("26.1022.968-nightly",target));
  updates->setRandom([](qint64 low,qint64){return low;});updates->start();
  const bool managed=state=="managed"&&target!="appimage";
  const auto status=updates->status();
  QCOMPARE(status["channel"].toString(),managed?QString("stable"):QString("nightly"));
  QCOMPARE(status["channelSource"].toString(),managed?QString("repository"):QString("setting"));
  QCOMPARE(status["channelSetting"].toString(),chosen);QCOMPARE(settings.document(),document);QCOMPARE(settings.revision(),revision);
  QCOMPARE(status["action"].toObject()["kind"].toString(),managed?QString("software-updates"):QString("download"));
  QCOMPARE(status["action"].toObject()["otherChannel"].isObject(),managed);
  QVERIFY(status["checkedAt"].isNull());QVERIFY(status["attemptedAt"].isNull());QVERIFY(status["result"].isNull());
  QVERIFY(!status["checking"].toBool());QVERIFY(calls().isEmpty());
  QCOMPARE(updates->nextCheckInMs()>0,checks);
 }
 void startupDiscardsStaleRepositoryOwnership(){
  SettingsService settings(dir.filePath("config.json"));configure(settings,{{"check",false},{"channel","nightly"}});
  repository("debian13","stable");
  QFile state(dir.filePath("update-check.json"));QVERIFY(state.open(QIODevice::WriteOnly));
  state.write(QJsonDocument(QJsonObject{{"schemaVersion",1},{"checkedAt",now.toString(Qt::ISODate)},
   {"result",QJsonObject{{"channel","stable"},{"channelSource","repository"},{"latest","26.1020.950"},{"action",QJsonObject{{"kind","software-updates"},{"channel","stable"}}}}}}).toJson());state.close();
  // The file changed since the successful check: startup follows it now.
  repository("debian13","nightly");
  auto changed=service(settings,build("26.1022.968-nightly","debian13"));changed->start();
  QCOMPARE(changed->status()["channel"].toString(),QString("nightly"));QVERIFY(changed->status()["result"].isNull());
  QVERIFY(QFile::remove(repositoryFile("debian13")));
  auto absent=service(settings,build("26.1022.968-nightly","debian13"));absent->start();
  QCOMPARE(absent->status()["channelSource"].toString(),QString("setting"));QCOMPARE(absent->status()["action"].toObject()["repository"].toString(),QString("absent"));
  // A different install sharing the state must never inherit apt ownership.
  configure(settings,{{"check",false},{"channel","stable"}});
  auto image=service(settings,build("26.1022.968-nightly"));image->start();
  const auto status=image->status();QCOMPARE(status["channelSource"].toString(),QString("setting"));
  QCOMPARE(status["result"].toObject()["channelSource"].toString(),QString("setting"));
  QCOMPARE(status["result"].toObject()["action"].toObject()["kind"].toString(),QString("download"));
 }
 void failedCheckStillRefreshesLocalOwnership(){
  SettingsService settings(dir.filePath("config.json"));configure(settings,{{"check",false}});
  repository("fedora44","stable");
  auto updates=service(settings,build("26.1022.968-nightly","fedora44"));updates->start();
  QCOMPARE(updates->status()["channel"].toString(),QString("stable"));
  repository("fedora44","nightly");
  answer({{"schemaVersion",1},{"ok",false},{"error",QJsonObject{{"code","update_feed_unavailable"},{"message","Offline"}}}},1);
  bool replied=false;updates->check([&](Error error,QJsonObject){QCOMPARE(error.code,QString("update_feed_unavailable"));replied=true;});
  PUTGD_TRY_VERIFY(replied);QCOMPARE(calls().last(),QString("--check-update nightly"));
  QCOMPARE(updates->status()["channelSource"].toString(),QString("repository"));
  QVERIFY(QFile::remove(repositoryFile("fedora44")));replied=false;
  updates->check([&](Error error,QJsonObject){QVERIFY(error);replied=true;});PUTGD_TRY_VERIFY(replied);
  QCOMPARE(updates->status()["channelSource"].toString(),QString("setting"));
  QCOMPARE(updates->status()["action"].toObject()["repository"].toString(),QString("absent"));
 }
 void unsupportedBuildsNeverCheck(){
  SettingsService settings(dir.filePath("config.json"));configure(settings);
  auto updates=service(settings,build("0.1.1-dev.5+gabc","debian13"));updates->start();
  QCOMPARE(updates->nextCheckInMs(),-1);QCOMPARE(updates->status()["reason"].toString(),QString("development_build"));QCOMPARE(updates->status()["enabled"].toBool(),false);
  Error error;updates->check([&](Error e,QJsonObject){error=e;});QCOMPARE(error.code,QString("update_check_unavailable"));
  putgd::test::settle(100);PUTGD_TRY_VERIFY(calls().isEmpty());
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
  configure(settings,{{"check",false}});PUTGD_TRY_COMPARE(restarted.nextCheckInMs(),-1);QVERIFY(restarted.status()["nextCheckAt"].isNull());
 }
 void stableAnnouncesEachVersionOnce(){
  SettingsService settings(dir.filePath("config.json"));configure(settings);answer(available("stable","0.1.1"));
  auto updates=service(settings,build("0.1.0"));QSignalSpy announced(updates.get(),&UpdateService::updateAvailable);updates->start();
  PUTGD_TRY_COMPARE(announced.size(),1);QCOMPARE(announced.first().first().toJsonObject()["latest"].toString(),QString("0.1.1"));
  QCOMPARE(calls(),QStringList{"--check-update stable"});
  PUTGD_TRY_VERIFY(!updates->status()["checking"].toBool());
  const auto status=updates->status();QCOMPARE(status["result"].toObject()["updateAvailable"].toBool(),true);QCOMPARE(status["notifies"].toBool(),true);
  // A day later the same version is not announced again.
  now=now.addDays(1);auto restarted=service(settings,build("0.1.0"));QSignalSpy again(restarted.get(),&UpdateService::updateAvailable);restarted->start();
  PUTGD_TRY_COMPARE(calls().size(),2);PUTGD_TRY_VERIFY(!restarted->status()["checking"].toBool());QCOMPARE(again.size(),0);
  // A newer one is.
  answer(available("stable","0.1.2"));now=now.addDays(1);auto third=service(settings,build("0.1.0"));QSignalSpy newer(third.get(),&UpdateService::updateAvailable);third->start();
  PUTGD_TRY_COMPARE(newer.size(),1);
 }
 void nightlyAndSilencedNotificationsOnlyReport(){
  SettingsService settings(dir.filePath("config.json"));configure(settings);answer(available("nightly","0.1.1-nightly.590"));
  auto nightly=service(settings,build("0.1.1-nightly.588"));QSignalSpy announced(nightly.get(),&UpdateService::updateAvailable);nightly->start();
  PUTGD_TRY_COMPARE(calls().size(),1);PUTGD_TRY_VERIFY(!nightly->status()["checking"].toBool());
  QCOMPARE(announced.size(),0);QCOMPARE(nightly->status()["result"].toObject()["latest"].toString(),QString("0.1.1-nightly.590"));QCOMPARE(nightly->status()["notifies"].toBool(),false);
  QFile::remove(dir.filePath("update-check.json"));
  configure(settings,{},false);answer(available("stable","0.1.1"));
  auto quiet=service(settings,build("0.1.0"));QSignalSpy silent(quiet.get(),&UpdateService::updateAvailable);quiet->start();
  PUTGD_TRY_COMPARE(calls().size(),2);PUTGD_TRY_VERIFY(!quiet->status()["checking"].toBool());QCOMPARE(silent.size(),0);
 }
 void checkNowRepliesAndAChosenChannelWins(){
  SettingsService settings(dir.filePath("config.json"));configure(settings,{{"channel","stable"}});answer(available("stable","0.1.1"));
  UpdateService updates(settings,dir.path(),"/bin/sh");updates.setProgram("/bin/sh",{"-c",fakeScript,fake()});
  updates.setBuild(build("0.1.1-nightly.588"));updates.setClock([this]{return now;});updates.start();
  QCOMPARE(updates.status()["channel"].toString(),QString("stable"));
  QSignalSpy announced(&updates,&UpdateService::updateAvailable);
  bool replied=false;QJsonObject status;updates.check([&](Error error,QJsonObject result){QVERIFY(!error);replied=true;status=result;});
  PUTGD_TRY_VERIFY(replied);QCOMPARE(calls(),QStringList{"--check-update stable"});
  QCOMPARE(status["result"].toObject()["latest"].toString(),QString("0.1.1"));
  // Someone who checked by hand has seen it: no notification for it later.
  QCOMPARE(announced.size(),0);
  const auto saved=QJsonDocument::fromJson([&]{QFile f(dir.filePath("update-check.json"));return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray();}()).object();
  QCOMPARE(saved["notifiedVersion"].toString(),QString("0.1.1"));
 }
 void unreachableFeedWaitsADay(){
  // No result has ever arrived: the scheduled check must not repeat at once.
  SettingsService settings(dir.filePath("config.json"));configure(settings);
  answer({{"schemaVersion",1},{"ok",false},{"result",QJsonObject{}},{"error",QJsonObject{{"code","update_feed_unavailable"},{"message","Cannot read the update feed"},{"details",QJsonObject{}}}}},1);
  auto updates=service(settings,build("0.1.0"));updates->start();PUTGD_TRY_COMPARE(calls().size(),1);PUTGD_TRY_VERIFY(!updates->status()["checking"].toBool());
  putgd::test::settle(300);PUTGD_TRY_COMPARE(calls().size(),1);
  PUTGD_TRY_COMPARE(updates->status()["nextCheckAt"].toString(),now.addMSecs(UpdateService::intervalMs).toString(Qt::ISODate));
  PUTGD_TRY_VERIFY(updates->status()["result"].isNull());PUTGD_TRY_COMPARE(updates->status()["error"].toObject()["code"].toString(),QString("update_feed_unavailable"));
 }
 void repositoryInstallsLeaveNotificationsToTheSystem(){
  repository("debian13");
  // A stable DEB whose putgd.sources is in use: the check child says so, and
  // the system updater notifies, not put.gd.
  SettingsService settings(dir.filePath("config.json"));configure(settings);
  const QJsonObject managed{{"kind","software-updates"},{"manager","apt"},{"command","sudo apt update && sudo apt upgrade"},
   {"updater",QJsonObject{{"id","gnome-software"},{"name","GNOME Software"},{"program","/usr/bin/gnome-software"}}}};
  auto reply=available("stable","0.1.1");auto result=reply["result"].toObject();result["action"]=managed;reply["result"]=result;answer(reply);
  auto updates=service(settings,build("0.1.0","debian13"));
  QSignalSpy announced(updates.get(),&UpdateService::updateAvailable);updates->start();
  PUTGD_TRY_COMPARE(calls().size(),1);PUTGD_TRY_VERIFY(!updates->status()["checking"].toBool());
  QCOMPARE(announced.size(),0);QCOMPARE(updates->status()["notifies"].toBool(),false);
  QCOMPARE(updates->status()["action"].toObject()["kind"].toString(),QString("software-updates"));
  // Opening the updater is the child's job too.
  answer({{"schemaVersion",1},{"ok",true},{"error",QJsonObject{}},{"result",QJsonObject{{"opened","gnome-software"},{"action",managed}}}});
  bool replied=false;QJsonObject opened;updates->openSoftwareUpdates([&](Error error,QJsonObject r){QVERIFY(!error);opened=r;replied=true;});
  PUTGD_TRY_VERIFY(replied);QCOMPARE(opened["opened"].toString(),QString("gnome-software"));QCOMPARE(calls().last(),QString("--open-software-updates"));
  // Its refusal reaches the caller unchanged.
  answer({{"schemaVersion",1},{"ok",false},{"result",QJsonObject{}},{"error",QJsonObject{{"code","software_updates_unavailable"},{"message","No software updater is installed"},{"details",QJsonObject{{"command","sudo apt update && sudo apt upgrade"}}}}}},1);
  Error error;replied=false;updates->openSoftwareUpdates([&](Error e,QJsonObject){error=e;replied=true;});
  PUTGD_TRY_VERIFY(replied);QCOMPARE(error.code,QString("software_updates_unavailable"));QCOMPARE(error.details["command"].toString(),QString("sudo apt update && sudo apt upgrade"));
  // The repository file deleted: the next check reports a download, and stable notifies again.
  QVERIFY(QFile::remove(repositoryFile("debian13")));
  now=now.addDays(1);result["action"]=QJsonObject{{"kind","download"},{"repository","absent"}};result["latest"]="0.1.2";reply["result"]=result;answer(reply);
  auto later=service(settings,build("0.1.0","debian13"));QSignalSpy again(later.get(),&UpdateService::updateAvailable);later->start();
  PUTGD_TRY_COMPARE(again.size(),1);QCOMPARE(later->status()["notifies"].toBool(),true);
 }
 // #386: a DEB/RPM follows its repository's channel, which the check child
 // reads; the setting stays as stored, and nothing offers to switch.
 void repositoryInstallsFollowTheirRepositorysChannel(){
  repository("ubuntu24.04");
  SettingsService settings(dir.filePath("config.json"));configure(settings);
  const QJsonObject managed{{"kind","software-updates"},{"manager","apt"},{"channel","stable"},{"command","sudo apt update && sudo apt upgrade"},{"updater",QJsonValue()},
   {"older",QJsonObject{{"version","26.1020.950"},{"command","sudo apt install --allow-downgrades putgd/stable"}}}};
  auto reply=found("stable","26.1022.968-nightly","26.1020.950",false);auto result=reply["result"].toObject();result["action"]=managed;result["channelSource"]="repository";reply["result"]=result;answer(reply);
  auto updates=service(settings,build("26.1022.968-nightly","ubuntu24.04"));QSignalSpy announced(updates.get(),&UpdateService::updateAvailable);updates->start();
  // The resident already knows the repository channel before launching the child.
  PUTGD_TRY_COMPARE(calls().size(),1);QCOMPARE(calls().first(),QString("--check-update stable"));PUTGD_TRY_VERIFY(!updates->status()["checking"].toBool());
  auto status=updates->status();
  QCOMPARE(status["channel"].toString(),QString("stable"));QCOMPARE(status["channelSource"].toString(),QString("repository"));QCOMPARE(status["channelSetting"].toString(),QString("build"));
  QCOMPARE(status["result"].toObject()["latest"].toString(),QString("26.1020.950"));QCOMPARE(status["action"].toObject()["older"].toObject()["version"].toString(),QString("26.1020.950"));
  QCOMPARE(status["notifies"].toBool(),false);QCOMPARE(announced.size(),0);
  const auto install=status["install"].toObject();QCOMPARE(install["kind"].toString(),QString("update"));QVERIFY(install["switch"].isNull());QCOMPARE(install["reason"].toString(),QString("not_appimage"));
  // The next check asks for the repository's channel, and a restart remembers it.
  Error error;bool replied=false;updates->check([&](Error e,QJsonObject){error=e;replied=true;});PUTGD_TRY_VERIFY(replied);QVERIFY(!error);
  QCOMPARE(calls().last(),QString("--check-update stable"));
  auto restarted=service(settings,build("26.1022.968-nightly","ubuntu24.04"));restarted->start();
  QCOMPARE(restarted->status()["channel"].toString(),QString("stable"));QCOMPARE(restarted->status()["channelSource"].toString(),QString("repository"));
  // The repository file removed: the setting decides again.
  QVERIFY(QFile::remove(repositoryFile("ubuntu24.04")));
  result["channel"]="nightly";result["action"]=QJsonObject{{"kind","download"},{"repository","absent"}};result["channelSource"]="setting";reply["result"]=result;answer(reply);
  replied=false;restarted->check([&](Error e,QJsonObject){error=e;replied=true;});PUTGD_TRY_VERIFY(replied);
  QCOMPARE(restarted->status()["channelSource"].toString(),QString("setting"));QCOMPARE(restarted->status()["channel"].toString(),QString("nightly"));
 }
 void changedRepositoryDoesNotAnnounceAnotherChannel_data(){
  QTest::addColumn<QString>("target");QTest::addColumn<QString>("state");QTest::addColumn<bool>("manual");
  for(const auto& target:{QString("debian13"),QString("fedora44")})
   for(const auto& state:{QString("absent"),QString("disabled")})
    for(const auto manual:{false,true})
     QTest::newRow(qPrintable(target+"-"+state+(manual?"-manual":"-background")))<<target<<state<<manual;
 }
 void changedRepositoryDoesNotAnnounceAnotherChannel(){
  QFETCH(QString,target);QFETCH(QString,state);QFETCH(bool,manual);
  repository(target,"nightly");
  SettingsService settings(dir.filePath("config.json"));configure(settings,{{"channel","stable"}});
  auto reply=available("nightly","0.1.1-nightly.590");auto result=reply["result"].toObject();
  result["channelSource"]="repository";reply["result"]=result;answer(reply);
  auto updates=service(settings,build("0.1.0",target));
  // Hold the child after it observes Nightly until the local repository changes.
  updates->setProgram("/bin/sh",{"-c",R"sh(printf '%s\n' "$*" >> "$0.calls"; while [ ! -f "$0.release" ]; do sleep .01; done; cat "$0.out")sh",fake()});
  if(manual)updates->setRandom([](qint64 low,qint64){return low;});
  QSignalSpy announced(updates.get(),&UpdateService::updateAvailable);updates->start();
  bool replied=false;
  if(manual)updates->check([&](Error error,QJsonObject){QVERIFY(!error);replied=true;});
  PUTGD_TRY_COMPARE(calls(),QStringList{"--check-update nightly"});
  if(state=="absent")QVERIFY(QFile::remove(repositoryFile(target)));
  else repository(target,"nightly","disabled");
  // Prevent a second scheduled check from obscuring this in-flight result.
  configure(settings,{{"channel","stable"},{"check",false}});
  QFile release(fake()+".release");QVERIFY(release.open(QIODevice::WriteOnly));release.close();
  PUTGD_TRY_VERIFY(!updates->status()["checking"].toBool());
  if(manual)QVERIFY(replied);
  QCOMPARE(updates->status()["channel"].toString(),QString("stable"));
  QCOMPARE(updates->status()["channelSource"].toString(),QString("setting"));
  QVERIFY(updates->status()["result"].isNull());QCOMPARE(announced.size(),0);
  QFile saved(dir.filePath("update-check.json"));QVERIFY(saved.open(QIODevice::ReadOnly));
  const auto persisted=QJsonDocument::fromJson(saved.readAll()).object();
  QCOMPARE(persisted["attemptedChannel"].toString(),QString("nightly"));
  QVERIFY(persisted["notifiedVersions"].toObject().isEmpty());
 }
 void failedCheckKeepsTheLastResult(){
  SettingsService settings(dir.filePath("config.json"));configure(settings);answer(available("stable","0.1.1"));
  auto updates=service(settings,build("0.1.0"));updates->start();PUTGD_TRY_COMPARE(calls().size(),1);PUTGD_TRY_VERIFY(!updates->status()["checking"].toBool());
  answer({{"schemaVersion",1},{"ok",false},{"result",QJsonObject{}},{"error",QJsonObject{{"code","update_feed_unavailable"},{"message","Cannot read the update feed"},{"details",QJsonObject{}}}}},1);
  Error error;bool replied=false;updates->check([&](Error e,QJsonObject){error=e;replied=true;});
  PUTGD_TRY_VERIFY(replied);QCOMPARE(error.code,QString("update_feed_unavailable"));
  const auto status=updates->status();QCOMPARE(status["result"].toObject()["latest"].toString(),QString("0.1.1"));
  QCOMPARE(status["error"].toObject()["code"].toString(),QString("update_feed_unavailable"));
  // A failure waits a day like a success; it never retries in a loop.
  QCOMPARE(updates->status()["nextCheckAt"].toString(),now.addMSecs(UpdateService::intervalMs).toString(Qt::ISODate));
  // A child that prints nothing usable is a failure too.
  answer({},3);replied=false;updates->check([&](Error e,QJsonObject){error=e;replied=true;});PUTGD_TRY_VERIFY(replied);QCOMPARE(error.code,QString("update_check_failed"));
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
  PUTGD_TRY_COMPARE(updates->status()["install"].toObject()["state"].toString(),QString("installed"));
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
  PUTGD_TRY_VERIFY(quit);
 }
 void aFailedInstallKeepsItsReasonAndNothingWaits(){
  SettingsService settings(dir.filePath("config.json"));configure(settings);
  auto updates=service(settings,build("0.1.0"));updates->setImage(dir.filePath("putgd.AppImage"));updates->start();
  answerLines({{{"schemaVersion",1},{"ok",false},{"result",QJsonObject{}},{"error",QJsonObject{{"code","update_verification_failed"},{"message","The download doesn't match the release's SHA256SUMS; nothing was changed"},{"details",QJsonObject{{"check","sha256"}}}}}}},1);
  updates->install({{"confirm",true},{"method","download"}},[](Error,QJsonObject){});
  PUTGD_TRY_COMPARE(updates->status()["install"].toObject()["state"].toString(),QString("failed"));
  const auto install=updates->status()["install"].toObject();
  QCOMPARE(install["error"].toObject()["code"].toString(),QString("update_verification_failed"));QCOMPARE(install["restartRequired"].toBool(),false);
  Error error;updates->restart({{"confirm",true}},[&](Error e,QJsonObject){error=e;});QCOMPARE(error.code,QString("update_restart_unavailable"));
  // A child that dies without a result is a failure too.
  answerLines({},9);updates->install({{"confirm",true}},[](Error,QJsonObject){});
  PUTGD_TRY_COMPARE(updates->status()["install"].toObject()["error"].toObject()["code"].toString(),QString("update_install_failed"));
 }
 void onlyAnAppImageOnItsOwnChannelInstalls(){
  SettingsService settings(dir.filePath("config.json"));configure(settings);
  auto deb=service(settings,build("0.1.0","debian13"));deb->setImage({});deb->start();
  QCOMPARE(deb->status()["install"].toObject()["reason"].toString(),QString("not_appimage"));
  Error error;deb->install({{"confirm",true}},[&](Error e,QJsonObject){error=e;});QCOMPARE(error.code,QString("update_install_unavailable"));
  auto unpacked=service(settings,build("0.1.0"));unpacked->setImage({});unpacked->start();
  QCOMPARE(unpacked->status()["install"].toObject()["reason"].toString(),QString("not_running_from_appimage"));
  // A stable image watching nightly switches (#344), but only to a release a check found.
  configure(settings,{{"channel","nightly"}});
  auto other=service(settings,build("0.1.0"));other->setImage(dir.filePath("putgd.AppImage"));other->start();
  QCOMPARE(other->status()["install"].toObject()["reason"].toString(),QString("not_checked"));QCOMPARE(other->status()["install"].toObject()["kind"].toString(),QString("switch"));
  error={};other->install({{"confirm",true},{"channel","nightly"}},[&](Error e,QJsonObject){error=e;});QCOMPARE(error.code,QString("update_install_unavailable"));QCOMPARE(error.details["reason"].toString(),QString("not_checked"));
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
 // #412: the backup moved back over an update that didn't start ends the
 // wait for a restart, so Settings offers the update again.
 void aBackupMovedBackEndsTheWaitForARestart(){
  SettingsService settings(dir.filePath("config.json"));configure(settings);
  const auto image=dir.filePath("putgd.AppImage"),backup=dir.filePath(".putgd-update-backup-0.1.0.AppImage");
  {QFile file(image);QVERIFY(file.open(QIODevice::WriteOnly));file.write("new image");}
  {QFile file(backup);QVERIFY(file.open(QIODevice::WriteOnly));file.write("old image");}
  struct stat kept{};QCOMPARE(::stat(QFile::encodeName(backup).constData(),&kept),0);
  QVERIFY(appimage::writeState(dir.path(),{{"installed",QJsonObject{{"image",image},{"version","0.1.1"},{"previousVersion","0.1.0"}}},
   {"backup",QJsonObject{{"path",backup},{"device",QString::number(quint64(kept.st_dev))},{"inode",QString::number(quint64(kept.st_ino))},{"size",qint64(kept.st_size)},{"mtime",qint64(kept.st_mtim.tv_sec)}}}}));
  auto waiting=service(settings,build("0.1.0"));waiting->setImage(image);waiting->start();
  QCOMPARE(waiting->status()["install"].toObject()["restartRequired"].toBool(),true);
  Error error;waiting->install({{"confirm",true}},[&](Error e,QJsonObject){error=e;});QCOMPARE(error.code,QString("update_restart_required"));
  QCOMPARE(::rename(QFile::encodeName(backup).constData(),QFile::encodeName(image).constData()),0);
  auto restored=service(settings,build("0.1.0"));restored->setImage(image);restored->start();
  const auto install=restored->status()["install"].toObject();
  QCOMPARE(install["restartRequired"].toBool(),false);QVERIFY(install["backup"].isNull());QVERIFY(install["installed"].isNull()||install["installed"].toObject().isEmpty());
  QCOMPARE(install["available"].toBool(),true);QCOMPARE(appimage::readState(dir.path())["restored"].toObject()["version"].toString(),QString("0.1.1"));
 }
 void aSwitchNamesItsChannelAndAnOlderOneMustBeAllowed(){
  SettingsService settings(dir.filePath("config.json"));configure(settings,{{"channel","nightly"}});
  // Stable 0.1.1 watching nightly, whose newest is still 0.1.1-nightly.900: older.
  answer(found("nightly","0.1.1","0.1.1-nightly.900",false));
  auto updates=service(settings,build("0.1.1"));const auto image=dir.filePath("putgd.AppImage");updates->setImage(image);
  QSignalSpy replaced(updates.get(),&UpdateService::imageReplaced);updates->start();
  PUTGD_TRY_COMPARE(calls().size(),1);PUTGD_TRY_VERIFY(!updates->status()["checking"].toBool());
  auto install=updates->status()["install"].toObject();
  QCOMPARE(install["kind"].toString(),QString("switch"));QCOMPARE(install["available"].toBool(),true);
  const auto offer=install["switch"].toObject();
  QCOMPARE(offer["from"].toString(),QString("stable"));QCOMPARE(offer["to"].toString(),QString("nightly"));QCOMPARE(offer["version"].toString(),QString("0.1.1-nightly.900"));
  QCOMPARE(offer["tag"].toString(),QString("v0.1.1-nightly.900"));QCOMPARE(offer["older"].toBool(),true);
  // Confirm alone is not enough: the switch must name where it goes, and allow the older version.
  Error error;updates->install({{"confirm",true}},[&](Error e,QJsonObject){error=e;});QCOMPARE(error.code,QString("update_switch_confirm_required"));
  error={};updates->install({{"confirm",true},{"channel","stable"}},[&](Error e,QJsonObject){error=e;});QCOMPARE(error.code,QString("update_switch_confirm_required"));
  error={};updates->install({{"confirm",true},{"channel","nightly"}},[&](Error e,QJsonObject){error=e;});QCOMPARE(error.code,QString("update_switch_older"));
  error={};updates->install({{"confirm",true},{"channel","nightly"},{"allowOlder","yes"}},[&](Error e,QJsonObject){error=e;});QCOMPARE(error.code,QString("invalid_request"));
  QCOMPARE(calls().size(),1);
  answerLines({{{"schemaVersion",1},{"ok",true},{"error",QJsonObject{}},{"result",QJsonObject{{"installed",true},{"version","0.1.1-nightly.900"},{"channel","nightly"},{"previousChannel","stable"},{"switched",true},{"method","zsync"}}}}});
  bool replied=false;updates->install({{"confirm",true},{"channel","nightly"},{"allowOlder",true}},[&](Error e,QJsonObject){QVERIFY(!e);replied=true;});QVERIFY(replied);
  QVERIFY(appimage::writeState(dir.path(),{{"installed",QJsonObject{{"image",image},{"version","0.1.1-nightly.900"},{"channel","nightly"},{"previousChannel","stable"},{"method","zsync"}}}}));
  PUTGD_TRY_COMPARE(updates->status()["install"].toObject()["state"].toString(),QString("installed"));
  QCOMPARE(calls().last(),"--install-update nightly --method auto --state "+dir.path()+" --switch --allow-older");
  // The login script is written again from the new image, and the version switched to counts as seen.
  QCOMPARE(replaced.size(),1);QCOMPARE(updates->status()["install"].toObject()["restartRequired"].toBool(),true);
  const auto saved=QJsonDocument::fromJson([&]{QFile f(dir.filePath("update-check.json"));return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray();}()).object();
  QCOMPARE(saved["notifiedVersions"].toObject()["nightly"].toString(),QString("0.1.1-nightly.900"));
 }
 void aNewerSwitchNeedsOnlyItsChannel(){
  SettingsService settings(dir.filePath("config.json"));configure(settings,{{"channel","nightly"}});
  answer(found("nightly","0.1.1","0.1.2-nightly.5",true));
  auto updates=service(settings,build("0.1.1"));updates->setImage(dir.filePath("putgd.AppImage"));updates->start();
  PUTGD_TRY_COMPARE(calls().size(),1);PUTGD_TRY_VERIFY(!updates->status()["checking"].toBool());
  QCOMPARE(updates->status()["install"].toObject()["switch"].toObject()["older"].toBool(),false);
  answerLines({},9);bool replied=false;updates->install({{"confirm",true},{"channel","nightly"}},[&](Error e,QJsonObject){QVERIFY(!e);replied=true;});QVERIFY(replied);
  PUTGD_TRY_COMPARE(calls().size(),2);QCOMPARE(calls().last(),"--install-update nightly --method auto --state "+dir.path()+" --switch");
  PUTGD_TRY_COMPARE(updates->status()["install"].toObject()["state"].toString(),QString("failed"));
 }
 // Before the first stable release: a clear "none yet", never an error or a switch.
 void noStableReleaseYetOffersNothing(){
  SettingsService settings(dir.filePath("config.json"));configure(settings,{{"channel","stable"}});
  answer(found("stable","0.1.1-nightly.900",QJsonValue(),false));
  auto updates=service(settings,build("0.1.1-nightly.900"));updates->setImage(dir.filePath("putgd.AppImage"));
  QSignalSpy announced(updates.get(),&UpdateService::updateAvailable);updates->start();
  PUTGD_TRY_COMPARE(calls().size(),1);PUTGD_TRY_VERIFY(!updates->status()["checking"].toBool());
  const auto status=updates->status();
  QVERIFY(status["error"].isNull());QVERIFY(status["result"].toObject()["latest"].isNull());QCOMPARE(status["result"].toObject()["updateAvailable"].toBool(),false);
  const auto install=status["install"].toObject();
  QCOMPARE(install["kind"].toString(),QString("switch"));QCOMPARE(install["available"].toBool(),false);QCOMPARE(install["reason"].toString(),QString("no_release"));QVERIFY(install["switch"].isNull());
  Error error;updates->install({{"confirm",true},{"channel","stable"}},[&](Error e,QJsonObject){error=e;});
  QCOMPARE(error.code,QString("update_install_unavailable"));QCOMPARE(error.details["reason"].toString(),QString("no_release"));
  QCOMPARE(announced.size(),0);QCOMPARE(calls().size(),1);
 }
 // Notifications remember a version per channel; the first result after a
 // channel change is shown in Settings, so it isn't announced as well.
 void switchingChannelsNeitherRepeatsNorHidesAnAnnouncement(){
  SettingsService settings(dir.filePath("config.json"));configure(settings);
  // A nightly AppImage checks nightly first.
  answer(found("nightly","0.1.1-nightly.900","0.1.1-nightly.901",true));
  auto nightly=service(settings,build("0.1.1-nightly.900"));nightly->start();PUTGD_TRY_COMPARE(calls().size(),1);PUTGD_TRY_VERIFY(!nightly->status()["checking"].toBool());
  // Stable is chosen: the first stable result shows in Settings, without a notification.
  // (Each resident goes before the next, so only one reacts to a settings change.)
  nightly.reset();configure(settings,{{"channel","stable"}});answer(found("stable","0.1.1-nightly.900","0.1.1",true));now=now.addSecs(60);
  auto chosen=service(settings,build("0.1.1-nightly.900"));QSignalSpy first(chosen.get(),&UpdateService::updateAvailable);chosen->start();
  PUTGD_TRY_COMPARE(calls().size(),2);PUTGD_TRY_VERIFY(!chosen->status()["checking"].toBool());QCOMPARE(first.size(),0);
  auto saved=QJsonDocument::fromJson([&]{QFile f(dir.filePath("update-check.json"));return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray();}()).object();
  QCOMPARE(saved["notifiedVersions"].toObject()["stable"].toString(),QString("0.1.1"));QCOMPARE(saved["notifiedVersion"].toString(),QString("0.1.1"));
  // A later stable release is announced once, as on a stable build.
  chosen.reset();answer(found("stable","0.1.1-nightly.900","0.1.2",true));now=now.addDays(1);
  auto later=service(settings,build("0.1.1-nightly.900"));QSignalSpy second(later.get(),&UpdateService::updateAvailable);later->start();
  PUTGD_TRY_COMPARE(second.size(),1);QCOMPARE(second.first().first().toJsonObject()["latest"].toString(),QString("0.1.2"));
  PUTGD_TRY_VERIFY(!later->status()["checking"].toBool());
  // Back to nightly and to stable again: 0.1.2 was announced on stable already.
  later.reset();configure(settings,{{"channel","nightly"}});answer(found("nightly","0.1.1-nightly.900","0.1.1-nightly.950",true));now=now.addDays(1);
  auto back=service(settings,build("0.1.1-nightly.900"));back->start();PUTGD_TRY_COMPARE(calls().size(),4);PUTGD_TRY_VERIFY(!back->status()["checking"].toBool());
  back.reset();configure(settings,{{"channel","stable"}});answer(found("stable","0.1.1-nightly.900","0.1.2",true));now=now.addDays(1);
  auto again=service(settings,build("0.1.1-nightly.900"));QSignalSpy repeated(again.get(),&UpdateService::updateAvailable);again->start();
  PUTGD_TRY_COMPARE(calls().size(),5);PUTGD_TRY_VERIFY(!again->status()["checking"].toBool());QCOMPARE(repeated.size(),0);
  saved=QJsonDocument::fromJson([&]{QFile f(dir.filePath("update-check.json"));return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray();}()).object();
  QCOMPARE(saved["notifiedVersions"].toObject()["stable"].toString(),QString("0.1.2"));QCOMPARE(saved["notifiedVersions"].toObject()["nightly"].toString(),QString("0.1.1-nightly.950"));
  // The nightly version seen in between didn't replace stable's: a day later 0.1.2 stays quiet.
  again.reset();now=now.addDays(1);auto quiet=service(settings,build("0.1.1-nightly.900"));QSignalSpy none(quiet.get(),&UpdateService::updateAvailable);quiet->start();
  PUTGD_TRY_COMPARE(calls().size(),6);PUTGD_TRY_VERIFY(!quiet->status()["checking"].toBool());QCOMPARE(none.size(),0);
 }
 // A state file from before #344 kept one version; it counts for its own channel.
 void theOldSingleNotifiedVersionCountsForItsChannel(){
  SettingsService settings(dir.filePath("config.json"));configure(settings);
  QFile state(dir.filePath("update-check.json"));QVERIFY(state.open(QIODevice::WriteOnly));
  state.write(QJsonDocument(QJsonObject{{"schemaVersion",1},{"attemptedAt",now.addDays(-2).toString(Qt::ISODate)},{"attemptedChannel","stable"},{"notifiedVersion","0.1.1"},
   {"result",QJsonObject{{"channel","stable"},{"current","0.1.0"},{"latest","0.1.1"},{"updateAvailable",true}}}}).toJson());state.close();
  answer(found("stable","0.1.0","0.1.1",true));
  auto updates=service(settings,build("0.1.0"));QSignalSpy announced(updates.get(),&UpdateService::updateAvailable);updates->start();
  PUTGD_TRY_COMPARE(calls().size(),1);PUTGD_TRY_VERIFY(!updates->status()["checking"].toBool());QCOMPARE(announced.size(),0);
 }
};
QTEST_GUILESS_MAIN(UpdateServiceTest)
#include "test_update_service.moc"
