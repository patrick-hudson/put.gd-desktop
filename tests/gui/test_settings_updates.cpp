#include "SettingsGuiFixture.h"
class SettingsGui:public SettingsGuiFixture {
 Q_OBJECT
private slots:
 void updateChannelHelpReservesItsHeightAfterItAppears(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);QJsonObject update;
  fixture.server.setUiDiagnostics([&]{auto data=fixture.operations().diagnostics();data["update"]=update;return data;});
  MainWindow window(fixture.session);window.resize(1060,680);window.navigate("settings");window.show();PUTGD_TRY_VERIFY(fixture.session.ready());
  auto* channel=window.findChild<QComboBox*>("updateChannel");auto* source=window.findChild<QLabel*>("updateChannelSource");QVERIFY(channel&&source);
  for(int pass=0;pass<2;++pass){
   update={{"supported",true},{"check",true},{"channelSource","repository"},{"channel","nightly"},{"build",QJsonObject{{"version","26.1026.1-nightly"},{"channel","nightly"},{"supported",true}}},{"action",QJsonObject{{"kind","software-updates"},{"repository","managed"},{"file","/etc/yum.repos.d/putgd.repo"}}}};fixture.session.refreshSnapshot();PUTGD_TRY_VERIFY(source->isVisibleTo(window.settings()));
   const auto fits=[&]{return source->mapTo(&window,QPoint()).y()>=channel->mapTo(&window,QPoint(0,channel->height())).y()&&source->parentWidget()->rect().contains(source->geometry())&&source->height()>=source->heightForWidth(source->width());};
   PUTGD_VERIFY_STEADY(fits(),200);
   update["channelSource"]="setting";fixture.session.refreshSnapshot();PUTGD_TRY_VERIFY(!source->isVisibleTo(window.settings()));
  }
 }
 void updateIntroFollowsSupportAndDailyChecks(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(Settings::defaults(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);QJsonObject update{{"supported",true},{"channel","stable"}};fixture.server.setUiDiagnostics([&]{return QJsonObject{{"update",update}};});MainWindow window(fixture.session);window.navigate("settings");window.show();fixture.session.refreshSnapshot();auto* intro=window.findChild<QLabel*>("updatesIntro");auto* daily=window.findChild<QCheckBox*>("checkForUpdates");QVERIFY(intro&&daily);PUTGD_TRY_VERIFY(intro->isVisible());QVERIFY(intro->text().contains("once a day"));daily->setChecked(false);PUTGD_TRY_VERIFY(intro->text().contains("when you choose Check now"));update["supported"]=false;update["reason"]="development_build";fixture.session.refreshSnapshot();PUTGD_TRY_VERIFY(intro->isHidden());
 }
 void developmentUpdatesExplainWhyCheckingIsUnavailable(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(Settings::defaults(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);fixture.server.setUiDiagnostics([]{return QJsonObject{{"update",QJsonObject{{"supported",false},{"reason","development_build"}}}};});fixture.session.refreshSnapshot();PUTGD_TRY_VERIFY(!fixture.session.diagnostics()["update"].toObject().isEmpty());MainWindow window(fixture.session);window.navigate("settings");window.show();QCOMPARE(window.findChild<QLabel*>("updateStatus")->text(),QString("Update checks aren't available for development builds"));QVERIFY(window.findChild<QPushButton*>("checkForUpdatesNow")->isHidden());
 }
 void updateStatusKeepsTheRowBesideCheckNow(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);const QJsonObject update{{"supported",true},{"check",true},{"checking",false},{"checkedAt",QJsonValue()},{"nextCheckAt",QJsonValue()},{"channel","nightly"},{"result",QJsonObject{}},{"error",QJsonObject{}},{"build",QJsonObject{{"version","26.1026.1-nightly"},{"channel","nightly"},{"supported",true}}}};
  fixture.server.setUiDiagnostics([&]{auto data=fixture.operations().diagnostics();data["update"]=update;return data;});
  MainWindow window(fixture.session);window.resize(1060,680);window.navigate("settings");window.show();PUTGD_TRY_VERIFY(fixture.session.ready());window.settings()->showUpdates();
  auto* status=window.findChild<QLabel*>("updateStatus");auto* check=window.findChild<QPushButton*>("checkForUpdatesNow");QVERIFY(status&&check);PUTGD_TRY_VERIFY(check->isVisible()&&status->text().startsWith("Not checked yet"));PUTGD_VERIFY_STEADY(status->text().contains(QString("26.1026.1")+QChar(0x2011)+"nightly"),30);QVERIFY(status->alignment()&Qt::AlignLeft);
  const auto row=status->parentWidget();QVERIFY(status->width()>=row->width()-check->width()-2*Ui::space(Ui::Space::l));QVERIFY(check->x()>=status->x()+status->width());
  const int lines=status->heightForWidth(status->width())-status->contentsMargins().top()-status->contentsMargins().bottom();QVERIFY2(lines<=status->fontMetrics().lineSpacing()+status->fontMetrics().height(),qPrintable(QString("%1 px of text in %2 px").arg(lines).arg(status->width())));
 }
 void updateChannelStartsOnThisBuildsChannel(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);QJsonObject update;
  fixture.server.setUiDiagnostics([&]{auto data=fixture.operations().diagnostics();data["update"]=update;return data;});
  MainWindow window(fixture.session);window.navigate("settings");window.show();PUTGD_TRY_VERIFY(fixture.session.ready());
  auto* channel=window.findChild<QComboBox*>("updateChannel");QVERIFY(channel);
  QCOMPARE(channel->count(),2);QCOMPARE(channel->itemText(0),QString("Stable"));QCOMPARE(channel->itemData(0).toString(),QString("stable"));
  QCOMPARE(channel->itemText(1),QString("Nightly"));QCOMPARE(channel->itemData(1).toString(),QString("nightly"));
  auto apply=[&](bool supported,const QJsonValue& buildChannel,const QString& setting,const QString& reason){
   update={{"supported",supported},{"build",QJsonObject{{"version","0.2.0-synthetic"},{"channel",buildChannel},{"supported",supported},{"reason",reason.isEmpty()?QJsonValue():QJsonValue(reason)}}},
    {"check",true},{"channelSetting",setting},{"channel",setting=="build"?buildChannel:QJsonValue(setting)},{"checking",false},{"result",QJsonObject{}},{"error",QJsonObject{}},
    {"checkedAt",QJsonValue()},{"nextCheckAt",QJsonValue()},{"reason",reason.isEmpty()?QJsonValue():QJsonValue(reason)}};
   fixture.session.refreshSnapshot();
  };
  const auto stored=[&]{return service.document()["preferences"].toObject()["updates"].toObject()["channel"].toString("(none)");};
  apply(true,"stable","build",{});PUTGD_TRY_COMPARE(channel->currentData().toString(),QString("stable"));PUTGD_TRY_VERIFY(channel->isVisibleTo(window.settings()));
  apply(true,"nightly","build",{});PUTGD_TRY_COMPARE(channel->currentData().toString(),QString("nightly"));
  // Showing the build's channel saves nothing: an untouched file keeps no updates object.
  PUTGD_COMPARE_STEADY(stored(),QString("(none)"),200);
  // The other channel is stored by name, and the build's own as "build".
  channel->setCurrentIndex(0);PUTGD_TRY_COMPARE(stored(),QString("stable"));
  apply(true,"nightly","stable",{});PUTGD_TRY_VERIFY(window.findChild<QLabel*>("updateStatus")->text().contains("Not checked yet"));QCOMPARE(channel->currentData().toString(),QString("stable"));
  channel->setCurrentIndex(1);PUTGD_TRY_COMPARE(stored(),QString("build"));
  // A stored "build" from before #344 shows as this build's channel too.
  apply(true,"stable","build",{});PUTGD_TRY_COMPARE(channel->currentData().toString(),QString("stable"));
  // A development build or unknown package never checks: the row is hidden and nothing is rewritten.
  apply(false,QJsonValue(),"build","development_build");PUTGD_TRY_VERIFY(!channel->isVisibleTo(window.settings()));
  window.findChild<QCheckBox*>("notifications")->setChecked(!window.findChild<QCheckBox*>("notifications")->isChecked());PUTGD_COMPARE_STEADY(stored(),QString("build"),300);
  apply(false,"stable","build","no_package");PUTGD_TRY_VERIFY(!channel->isVisibleTo(window.settings()));
 }
 // #491: the real service supplies ownership before the first UI snapshot,
 // with no feed result. Saving unrelated preferences must retain the choice
 // used by manual installs sharing this configuration.
 void updateChannelIsCorrectOnFirstSnapshot_data(){
  QTest::addColumn<bool>("managed");QTest::addColumn<QString>("chosen");
  QTest::newRow("fresh-repository")<<true<<QString("build");
  QTest::newRow("repository-with-manual-choice")<<true<<QString("nightly");
  QTest::newRow("fresh-manual")<<false<<QString("build");
  QTest::newRow("manual-with-choice")<<false<<QString("stable");
 }
 void updateChannelIsCorrectOnFirstSnapshot(){
  QFETCH(bool,managed);QFETCH(QString,chosen);
  QTemporaryDir dir;QVERIFY(dir.isValid());
  const bool had=qEnvironmentVariableIsSet("PUTGD_UPDATE_SYSTEM_ROOT");const auto old=qgetenv("PUTGD_UPDATE_SYSTEM_ROOT");
  const auto restore=qScopeGuard([&]{if(had)qputenv("PUTGD_UPDATE_SYSTEM_ROOT",old);else qunsetenv("PUTGD_UPDATE_SYSTEM_ROOT");});
  qputenv("PUTGD_UPDATE_SYSTEM_ROOT",dir.path().toUtf8());
  if(managed){
   QVERIFY(QDir().mkpath(dir.path()+"/etc/apt/sources.list.d"));QFile file(dir.path()+"/etc/apt/sources.list.d/putgd.sources");
   QVERIFY(file.open(QIODevice::WriteOnly));file.write(("Types: deb\nURIs: "+update::packageRepository()+"/apt/debian13\nSuites: stable\nComponents: main\n").toUtf8());
  }
  SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});
  auto doc=Settings::defaults();auto prefs=doc["preferences"].toObject();prefs["updates"]=QJsonObject{{"channel",chosen}};doc["preferences"]=prefs;QVERIFY(service.save(doc,{}));
  UpdateService updates(service,dir.path(),"/bin/false");
  updates.setBuild(update::build(QJsonObject{{"version","26.1022.968-nightly"},{"dirty",false},{"sourceProvenance","git"}},"debian13",true));updates.start();
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);fixture.server.setUpdateService(&updates);
  fixture.server.setUiDiagnostics([&]{return QJsonObject{{"update",updates.status()}};});
  fixture.session.refreshSnapshot();PUTGD_TRY_VERIFY(fixture.session.diagnostics()["update"].isObject());
  MainWindow window(fixture.session);window.resize(1100,700);window.navigate("settings");window.show();window.settings()->showUpdates();PUTGD_TRY_VERIFY(fixture.session.ready());
  auto* channel=window.findChild<QComboBox*>("updateChannel");auto* source=window.findChild<QLabel*>("updateChannelSource");
  auto* text=window.findChild<QLabel*>("otherChannelText");auto* line=window.findChild<QLabel*>("updateStatus");
  QVERIFY(channel&&source&&text&&line);
  const auto expected=managed||chosen=="stable"?QString("stable"):QString("nightly");
  PUTGD_TRY_COMPARE(channel->currentData().toString(),expected);QCOMPARE(channel->isEnabled(),!managed);QCOMPARE(Ui::isLocked(channel),managed);
  QCOMPARE(source->isVisibleTo(window.settings()),managed);QCOMPARE(text->isVisibleTo(window.settings()),managed);
  QVERIFY(QString(line->text()).replace(QChar(0x2011),QChar('-')).startsWith("Not checked yet"));
  QJsonObject wire;fixture.server.dispatch({{"schemaVersion",1},{"requestId","startup-update"},{"session",fixture.server.socketPath()},{"method","update.status"},{"params",QJsonObject{}}},[&](Error error,QJsonObject data){QVERIFY(!error);wire=data;});
  QCOMPARE(wire,updates.status());QCOMPARE(wire["channel"].toString(),expected);QVERIFY(wire["attemptedAt"].isNull());
  auto* notifications=window.findChild<QCheckBox*>("notifications");const bool before=notifications->isChecked();notifications->setChecked(!before);
  PUTGD_TRY_COMPARE(service.snapshot().notifications,!before);PUTGD_COMPARE_STEADY(service.snapshot().updateChannel,chosen,300);
  if(!managed){
   channel->setCurrentIndex(channel->findData(expected=="stable"?"nightly":"stable"));
   PUTGD_TRY_COMPARE(service.snapshot().updateChannel,expected=="stable"?QString("build"):QString("stable"));
  }
 }
 // A stored updates object may leave out check or channel. Saving another
 // preference must not add them, or the draft never matches the file and every
 // toolbar action is refused as "not saved".
 void partialUpdatesObjectSavesCleanly_data(){
  QTest::addColumn<QJsonObject>("updates");
  QTest::newRow("channel-only")<<QJsonObject{{"channel","build"}};
  QTest::newRow("check-only")<<QJsonObject{{"check",false}};
  QTest::newRow("empty")<<QJsonObject{};
  QTest::newRow("both")<<QJsonObject{{"check",true},{"channel","stable"}};
 }
 void partialUpdatesObjectSavesCleanly(){
  QFETCH(QJsonObject,updates);
  QTemporaryDir dir;QVERIFY(dir.isValid());SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});
  auto doc=Settings::defaults();auto prefs=doc["preferences"].toObject();prefs["updates"]=updates;doc["preferences"]=prefs;QVERIFY(service.save(doc,{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);
  MainWindow window(fixture.session);window.navigate("settings");window.show();PUTGD_TRY_VERIFY(fixture.session.ready());
  QVERIFY(!window.settings()->actionSaveError());
  auto* notifications=window.findChild<QCheckBox*>("notifications");QVERIFY(notifications);const bool before=notifications->isChecked();notifications->setChecked(!before);
  PUTGD_TRY_COMPARE(service.snapshot().notifications,!before);
  QCOMPARE(service.document()["preferences"].toObject()["updates"].toObject(),updates);
  PUTGD_TRY_VERIFY(!window.settings()->actionSaveError());
 }
 // #386: a DEB/RPM that updates from put.gd's repository shows the
 // repository's channel without a choice, and the commands that move to the
 // other one; put.gd runs none of them and never rewrites the stored choice.
 void repositoryInstallShowsItsChannelAndHowToMove(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);QJsonObject update;
  fixture.server.setUiDiagnostics([&]{auto data=fixture.operations().diagnostics();data["update"]=update;return data;});
  MainWindow window(fixture.session);window.navigate("settings");window.show();PUTGD_TRY_VERIFY(fixture.session.ready());
  auto* channel=window.findChild<QComboBox*>("updateChannel");auto* source=window.findChild<QLabel*>("updateChannelSource");auto* line=window.findChild<QLabel*>("updateStatus");
  auto* text=window.findChild<QLabel*>("otherChannelText");auto* commands=window.findChild<QLabel*>("otherChannelCommands");auto* copy=window.findChild<QPushButton*>("copyOtherChannelCommands");
  auto* command=window.findChild<QLineEdit*>("updateCommand");auto* open=window.findChild<QPushButton*>("openSoftwareUpdates");
  QVERIFY(channel&&source&&line&&text&&commands&&copy&&command&&open);
  const auto settings=window.settings();
  const auto stored=[&]{return service.document()["preferences"].toObject()["updates"].toObject()["channel"].toString("(none)");};
  auto apply=[&](const QString& version,const QString& buildChannel,const QString& repository,const QJsonValue& latest,bool newer,const QJsonObject& older={}){
   const auto other=repository=="nightly"?QString("stable"):QString("nightly");
   const QJsonArray moves{"sudo install -m 644 /usr/share/putgd/repo/putgd-"+other+".sources /etc/apt/sources.list.d/putgd.sources","sudo apt update","sudo apt install --allow-downgrades putgd/"+other};
   QJsonObject action{{"kind","software-updates"},{"manager","apt"},{"repository","managed"},{"file","/etc/apt/sources.list.d/putgd.sources"},{"channel",repository},
    {"command","sudo apt update && sudo apt upgrade"},{"updater",QJsonObject{{"id","mintupdate"},{"name","Update Manager"},{"program","/usr/bin/mintupdate"}}},
    {"otherChannel",QJsonObject{{"channel",other},{"commands",moves},{"docs","https://github.com/patrick-hudson/put.gd-desktop/blob/main/docs/package-repositories.md#switching-channels"}}},
    {"older",older.isEmpty()?QJsonValue():QJsonValue(older)}};
   update={{"supported",true},{"build",QJsonObject{{"version",version},{"channel",buildChannel},{"supported",true},{"packageTarget","ubuntu24.04"}}},
    {"check",true},{"channelSetting","build"},{"channel",repository},{"channelSource","repository"},{"checking",false},{"action",action},{"error",QJsonObject{}},
    {"checkedAt",QJsonValue()},{"nextCheckAt",QJsonValue()},
    {"result",QJsonObject{{"channel",repository},{"channelSource","repository"},{"current",version},{"latest",latest},{"updateAvailable",newer}}},
    {"install",QJsonObject{{"state","idle"},{"kind","update"},{"available",false},{"reason","not_appimage"},{"switch",QJsonValue()},{"running",false},{"restartRequired",false}}}};
   fixture.session.refreshSnapshot();
  };
  // A nightly DEB on its nightly repository: Nightly, shown but not chosen, and how to reach stable.
  apply("26.1022.968-nightly","nightly","nightly","26.1022.968-nightly",false);
  PUTGD_TRY_VERIFY(text->isVisibleTo(settings));QCOMPARE(channel->currentData().toString(),QString("nightly"));QVERIFY(!channel->isEnabled());
  QVERIFY(source->isVisibleTo(settings));QCOMPARE(source->text(),QString("Set by your system's put.gd repository, /etc/apt/sources.list.d/putgd.sources."));
  QVERIFY(text->text().startsWith("To move to stable, run these commands in a terminal. Stable is usually older than a nightly"));
  QCOMPARE(commands->text(),QString("sudo install -m 644 /usr/share/putgd/repo/putgd-stable.sources /etc/apt/sources.list.d/putgd.sources &&\nsudo apt update &&\nsudo apt install --allow-downgrades putgd/stable"));
  copy->click();QCOMPARE(QApplication::clipboard()->text(),commands->text());
  const QUrl docs(update["action"].toObject()["otherChannel"].toObject()["docs"].toString());QList<QUrl> opened;host::setTestOpener([&](const QUrl& url){opened.append(url);});auto restoreOpener=qScopeGuard([]{host::setTestOpener({});});window.findChild<QPushButton*>("otherChannelDocs")->click();QCOMPARE(opened,QList<QUrl>{docs});
  auto* browser=static_cast<Ui::BrowserFeedback*>(window.findChild<QWidget*>("channelBrowserFeedback"));QVERIFY(browser);browser->opening(docs)({{"opened",false}});QVERIFY(browser->isVisibleTo(settings));browser->findChild<QPushButton*>()->click();QCOMPARE(qApp->clipboard()->text(),docs.toString(QUrl::FullyEncoded));browser->clear();

  QVERIFY(QString(line->text()).replace(QChar(0x2011),QChar('-')).startsWith("Up to date: 26.1022.968-nightly is the newest nightly version."));
  // Moved to stable by hand, whose release is older: said plainly, with the command that installs it anyway.
  apply("26.1022.968-nightly","nightly","stable","26.1020.950",false,{{"version","26.1020.950"},{"command","sudo apt install --allow-downgrades putgd/stable"}});
  PUTGD_TRY_COMPARE(channel->currentData().toString(),QString("stable"));QVERIFY(!channel->isEnabled());
  QVERIFY(QString(line->text()).replace(QChar(0x2011),QChar('-')).startsWith("Your system now updates put.gd from the stable repository. Stable 26.1020.950 is older than this nightly build (26.1022.968-nightly), so the system updater keeps this version until a newer stable release is out."));
  QVERIFY(command->isVisibleTo(settings));QCOMPARE(command->text(),QString("sudo apt install --allow-downgrades putgd/stable"));
  QVERIFY(text->text().startsWith("To move to nightly"));QVERIFY(commands->text().endsWith("putgd/nightly"));
  // No stable release yet: the nightly stays until the first one.
  apply("26.1022.968-nightly","nightly","stable",QJsonValue(),false);
  PUTGD_TRY_VERIFY(QString(line->text()).replace(QChar(0x2011),QChar('-')).startsWith("There's no stable release yet. Your system updates put.gd from the stable repository, so this nightly build (26.1022.968-nightly) stays as it is"));
  QVERIFY(!command->isVisibleTo(settings));
  // A newer stable release is an ordinary update through the system's updater.
  apply("26.1022.968-nightly","nightly","stable","26.1101.990",true);
  PUTGD_TRY_VERIFY(open->isVisibleTo(settings));QVERIFY(QString(line->text()).replace(QChar(0x2011),QChar('-')).startsWith("put.gd 26.1101.990 is available from put.gd's stable repository."));
  // Nothing was stored: the choice belongs to builds that switch themselves.
  window.findChild<QCheckBox*>("notifications")->setChecked(!window.findChild<QCheckBox*>("notifications")->isChecked());PUTGD_COMPARE_STEADY(stored(),QString("(none)"),300);
  // Without a repository the choice is back, and the instructions go.
  update["channelSource"]="setting";update["channel"]="nightly";update["action"]=QJsonObject{{"kind","download"},{"repository","absent"}};
  auto result=update["result"].toObject();result["channel"]="nightly";result["channelSource"]="setting";update["result"]=result;fixture.session.refreshSnapshot();
  PUTGD_TRY_VERIFY(channel->isEnabled());QVERIFY(!source->isVisibleTo(settings));QVERIFY(!text->isVisibleTo(settings));QCOMPARE(channel->currentData().toString(),QString("nightly"));
 }
 // #293 phase 3: an AppImage with an update offers Download and install, shows
 // the install's progress, then Restart put.gd; a repository install never does.
 void appImageInstallShowsProgressThenRestart(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);QJsonObject update;
  fixture.server.setUiDiagnostics([&]{auto data=fixture.operations().diagnostics();data["update"]=update;return data;});
  MainWindow window(fixture.session);window.navigate("settings");window.show();PUTGD_TRY_VERIFY(fixture.session.ready());
  auto* install=window.findChild<QPushButton*>("installUpdate");auto* restart=window.findChild<QPushButton*>("restartAfterUpdate");auto* download=window.findChild<QPushButton*>("downloadUpdate");
  auto* progress=window.findChild<QProgressBar*>("updateInstallProgress");auto* status=window.findChild<QLabel*>("updateInstallStatus");
  QVERIFY(install&&restart&&download&&progress&&status);
  auto apply=[&](const QJsonObject& installState,const QJsonObject& action=QJsonObject{{"kind","download"},{"repository","none"}}){
   update={{"supported",true},{"build",QJsonObject{{"version","0.1.1-nightly.800"},{"channel","nightly"},{"supported",true},{"packageTarget","appimage"}}},
    {"check",true},{"channelSetting","build"},{"channel","nightly"},{"checking",false},{"action",action},{"error",QJsonObject{}},{"checkedAt",QJsonValue()},{"nextCheckAt",QJsonValue()},
    {"result",QJsonObject{{"channel","nightly"},{"current","0.1.1-nightly.800"},{"latest","0.1.1-nightly.801"},{"updateAvailable",true},
     {"downloadUrl","https://github.com/patrick-hudson/put.gd-desktop/releases/download/v0.1.1-nightly.801/putgd-0.1.1-nightly.801-x86_64.AppImage"}}},
    {"install",installState}};
   fixture.session.refreshSnapshot();
  };
  const auto settings=window.settings();
  apply({{"state","idle"},{"available",true},{"running",false},{"restartRequired",false}});
  PUTGD_TRY_VERIFY(install->isVisibleTo(settings));QVERIFY(install->isEnabled());QVERIFY(download->isVisibleTo(settings));QVERIFY(!restart->isVisibleTo(settings));QVERIFY(!status->isVisibleTo(settings));
  apply({{"state","running"},{"available",true},{"running",true},{"phase","download"},{"method","release"},{"bytes",50*1024*1024},{"total",100*1024*1024},{"restartRequired",false}});
  PUTGD_TRY_VERIFY(progress->isVisibleTo(settings));QCOMPARE(progress->value(),500);QVERIFY(!install->isEnabled());QVERIFY(status->text().contains("Downloading from the GitHub release"));
  apply({{"state","running"},{"available",true},{"running",true},{"phase","zsync"},{"method","appimageupdatetool"},{"bytes",0},{"total",100}});
  PUTGD_TRY_VERIFY(status->text().contains("appimageupdatetool"));
  apply({{"state","installed"},{"available",true},{"running",false},{"restartRequired",true},{"backup","/home/u/Applications/.putgd-update-backup-0.1.1-nightly.800.AppImage"},
   {"installed",QJsonObject{{"version","0.1.1-nightly.801"},{"method","zsync"}}}});
  PUTGD_TRY_VERIFY(restart->isVisibleTo(settings));QVERIFY(!install->isVisibleTo(settings));QVERIFY(!download->isVisibleTo(settings));QVERIFY(!progress->isVisibleTo(settings));
  QVERIFY(status->text().contains("Installed 0.1.1-nightly.801 with a zsync update. Restart put.gd to use it."));QVERIFY(status->text().contains(".putgd-update-backup-0.1.1-nightly.800.AppImage"));
  // After the restart: this build is the installed version, and Settings says so.
  apply({{"state","idle"},{"available",true},{"running",false},{"restartRequired",false},
   {"installed",QJsonObject{{"version","0.1.1-nightly.800"},{"previousVersion","0.1.1-nightly.799"},{"method","zsync"},{"installedAt","2026-10-21T22:48:09Z"}}}});
  PUTGD_TRY_VERIFY(status->text().startsWith("Updated from 0.1.1-nightly.799 to 0.1.1-nightly.800 with a zsync update on "));QVERIFY(status->isVisibleTo(settings));QVERIFY(!restart->isVisibleTo(settings));
  apply({{"state","failed"},{"available",true},{"running",false},{"restartRequired",false},{"error",QJsonObject{{"code","update_verification_failed"},{"message","The download doesn't match the release's SHA256SUMS; nothing was changed"}}}});
  PUTGD_TRY_VERIFY(status->text().contains("doesn't match the release's SHA256SUMS"));QVERIFY(install->isVisibleTo(settings));
  // #412: an image put.gd must not replace says why, and the download link stays.
  for(const QString message:{"The AppImage belongs to another user, so put.gd won't replace it","The AppImage's folder is on a read-only file system","put.gd can't write to the AppImage's folder"}){
   apply({{"state","failed"},{"available",true},{"running",false},{"restartRequired",false},
    {"error",QJsonObject{{"code","update_image_not_writable"},{"message",message},{"details",QJsonObject{{"directory","/home/u/Applications"}}}}}});
   PUTGD_TRY_COMPARE(status->text(),"The update wasn't installed: "+message+".");QVERIFY(status->isVisibleTo(settings));QVERIFY(download->isVisibleTo(settings));
  }
  // This resident has no install service: the click's refusal shows.
  install->click();PUTGD_TRY_VERIFY(status->text().contains("does not check for updates"));QVERIFY(install->isEnabled());
  // A repository install and a non-AppImage never offer it.
  apply({{"state","idle"},{"available",false},{"running",false},{"reason","not_appimage"},{"restartRequired",false}});PUTGD_TRY_VERIFY(!install->isVisibleTo(settings));
  apply({{"state","idle"},{"available",true},{"running",false},{"restartRequired",false}},{{"kind","software-updates"},{"manager","apt"},{"command","sudo apt update && sudo apt upgrade"},{"updater",QJsonValue()}});
  PUTGD_TRY_VERIFY(!download->isVisibleTo(settings));QVERIFY(!install->isVisibleTo(settings));
 }
 // #344: an AppImage watching the other channel offers an explicit switch, says
 // when that channel has no release yet, and confirms an older version first.
 void appImageWatchingTheOtherChannelOffersASwitch(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);QJsonObject update;
  fixture.server.setUiDiagnostics([&]{auto data=fixture.operations().diagnostics();data["update"]=update;return data;});
  MainWindow window(fixture.session);window.navigate("settings");window.show();PUTGD_TRY_VERIFY(fixture.session.ready());
  auto* install=window.findChild<QPushButton*>("installUpdate");auto* restart=window.findChild<QPushButton*>("restartAfterUpdate");
  auto* line=window.findChild<QLabel*>("updateStatus");auto* status=window.findChild<QLabel*>("updateInstallStatus");QVERIFY(install&&restart&&line&&status);
  auto apply=[&](const QString& version,const QString& buildChannel,const QString& channel,const QJsonValue& latest,bool newer,const QJsonObject& installState){
   update={{"supported",true},{"build",QJsonObject{{"version",version},{"channel",buildChannel},{"supported",true},{"packageTarget","appimage"}}},
    {"check",true},{"channelSetting",channel},{"channel",channel},{"checking",false},{"action",QJsonObject{{"kind","download"},{"repository","none"}}},{"error",QJsonObject{}},
    {"checkedAt",QJsonValue()},{"nextCheckAt",QJsonValue()},
    {"result",QJsonObject{{"channel",channel},{"current",version},{"latest",latest},{"updateAvailable",newer},{"downloadUrl","https://app.put.gd/dl/"+channel+"/appimage"}}},{"install",installState}};
   fixture.session.refreshSnapshot();
  };
  const auto settings=window.settings();
  // No stable release yet: said plainly, with nothing to click.
  apply("0.1.1-nightly.900","nightly","stable",QJsonValue(),false,{{"state","idle"},{"kind","switch"},{"available",false},{"reason","no_release"},{"switch",QJsonValue()},{"running",false},{"restartRequired",false}});
  PUTGD_TRY_VERIFY(QString(line->text()).replace(QChar(0x2011),QChar('-')).startsWith("There's no stable release yet. This nightly build (0.1.1-nightly.900) stays as it is"));
  QVERIFY(!QString(line->text()).replace(QChar(0x2011),QChar('-')).contains("Up to date"));QVERIFY(!install->isVisibleTo(settings));
  // The first stable release: Switch to stable 0.1.1.
  apply("0.1.1-nightly.900","nightly","stable","0.1.1",true,{{"state","idle"},{"kind","switch"},{"available",true},{"running",false},{"restartRequired",false},
   {"switch",QJsonObject{{"from","nightly"},{"to","stable"},{"version","0.1.1"},{"tag","v0.1.1"},{"older",false}}}});
  PUTGD_TRY_COMPARE(install->text(),QString("Switch to stable 0.1.1"));QVERIFY(install->isVisibleTo(settings));
  QVERIFY(QString(line->text()).replace(QChar(0x2011),QChar('-')).startsWith("Stable 0.1.1 is available. This is nightly 0.1.1-nightly.900; switching replaces it with the stable release."));
  // Without an install service here the click's refusal shows; it was asked for.
  install->click();PUTGD_TRY_VERIFY(status->text().contains("does not check for updates"));
  // Back from stable to an older nightly: labelled, and confirmed first.
  apply("0.1.1","stable","nightly","0.1.1-nightly.901",false,{{"state","idle"},{"kind","switch"},{"available",true},{"running",false},{"restartRequired",false},
   {"switch",QJsonObject{{"from","stable"},{"to","nightly"},{"version","0.1.1-nightly.901"},{"tag","v0.1.1-nightly.901"},{"older",true}}}});
  PUTGD_TRY_COMPARE(install->text(),QString("Switch to nightly 0.1.1-nightly.901 (older)"));
  QVERIFY(QString(line->text()).replace(QChar(0x2011),QChar('-')).startsWith("Nightly 0.1.1-nightly.901 is older than this stable build (0.1.1)."));
  QString asked;
  QTimer dismiss;connect(&dismiss,&QTimer::timeout,this,[&]{if(auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())){asked=box->property("confirmationText").toString();box->button(QMessageBox::Cancel)->click();}});dismiss.start(10);
  status->clear();install->click();dismiss.stop();
  PUTGD_TRY_VERIFY(asked.contains("which is older than this build (0.1.1)"));QVERIFY(asked.contains("may not open settings or history"));
  PUTGD_VERIFY_STEADY(!status->text().contains("does not check for updates"),100);QVERIFY(install->isEnabled());
  // Switched and waiting, then switched and running.
  apply("0.1.1-nightly.900","nightly","stable","0.1.1",true,{{"state","installed"},{"kind","switch"},{"available",true},{"running",false},{"restartRequired",true},{"switch",QJsonValue()},
   {"installed",QJsonObject{{"version","0.1.1"},{"channel","stable"},{"previousChannel","nightly"},{"method","zsync"}}}});
  PUTGD_TRY_VERIFY(status->text().startsWith("Switched to stable 0.1.1 with a zsync update. Restart put.gd to use it."));QVERIFY(restart->isVisibleTo(settings));
  apply("0.1.1","stable","stable","0.1.1",false,{{"state","idle"},{"kind","update"},{"available",true},{"running",false},{"restartRequired",false},{"switch",QJsonValue()},
   {"installed",QJsonObject{{"version","0.1.1"},{"previousVersion","0.1.1-nightly.900"},{"channel","stable"},{"previousChannel","nightly"},{"method","download"},{"installedAt","2026-10-22T10:00:00Z"}}}});
  PUTGD_TRY_VERIFY(status->text().startsWith("Switched from nightly 0.1.1-nightly.900 to stable 0.1.1 from a full download on "));
  QVERIFY(QString(line->text()).replace(QChar(0x2011),QChar('-')).startsWith("Up to date: 0.1.1 is the newest stable version."));QVERIFY(!install->isVisibleTo(settings));
 }

};
QTEST_MAIN(SettingsGui)
#include "test_settings_updates.moc"
