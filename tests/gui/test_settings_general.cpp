#include "SettingsGuiFixture.h"
class SettingsGui:public SettingsGuiFixture {
 Q_OBJECT
private slots:
 void conflictOffersReloadBesideTheMessage(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();window.navigate("destinations");window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  auto* settingsReload=window.findChild<QPushButton*>("reloadConflict");auto* destinationReload=window.findChild<QPushButton*>("reloadDestinationConflict");QVERIFY(settingsReload&&destinationReload);QVERIFY(settingsReload->isHidden()&&destinationReload->isHidden());
  auto* number=window.findChild<QLineEdit*>("maxFiles");const auto saved=number->text();number->setText("invalid");PUTGD_TRY_VERIFY(number->property("invalid").toBool());
  auto external=service.document();auto prefs=external["preferences"].toObject();QVERIFY(prefs["notifications"].toBool());prefs["notifications"]=false;external["preferences"]=prefs;QVERIFY(service.save(external,service.revision()));
  PUTGD_TRY_VERIFY(window.settings()->saveViewState()["conflict"].toBool());
  auto* status=window.findChild<QLabel*>("settingsStatus");auto* destinationStatus=window.findChild<QLabel*>("destinationStatus");
  QCOMPARE(statusCode(window),QString("config_conflict"));QVERIFY(!status->text().contains("config_conflict"));
  QCOMPARE(status->text(),QString("Settings changed elsewhere. Your edits are kept here. Reload discards them."));
  // Council 22: the conflict is a warning banner beside its Reload, never red text.
  auto* destinationBanner=window.findChild<Ui::Banner*>("destinationConflictBanner");auto* settingsBanner=window.findChild<Ui::Banner*>("settingsConflictBanner");QVERIFY(destinationBanner&&settingsBanner);for(auto* banner:{destinationBanner,settingsBanner}){QCOMPARE(banner->tone(),Ui::Banner::Tone::Warning);QCOMPARE(banner->textLabel()->text(),status->text());QVERIFY(banner->textLabel()->palette().color(QPalette::WindowText)!=Theme::current().danger);QCOMPARE(banner->actionButton()->text(),QString("Reload from file…"));}
  window.resize(800,700);PUTGD_TRY_VERIFY(!QRect(destinationReload->mapTo(&window,QPoint()),destinationReload->size()).intersects(QRect(destinationBanner->textLabel()->mapTo(&window,QPoint()),destinationBanner->textLabel()->size())));
  // The band carries one bounded indicator; the explanation and Reload stay in the page.
  auto* bandStatus=window.findChild<QLabel*>("autosaveStatus");QCOMPARE(bandStatus->text(),QString("Changes not saved"));QVERIFY(bandStatus->isVisible());QCOMPARE(bandStatus->property("feedbackTone").toString(),QString("neutral"));QCOMPARE(bandStatus->palette().color(QPalette::WindowText),Theme::current().textSecondary);QVERIFY(window.findChild<QLabel*>("autosaveExplanation")->isHidden());
  auto* summary=window.findChild<QToolButton*>("captureOutputSummary");QVERIFY(summary->isEnabled());QVERIFY(summary->menu());
  QVERIFY(destinationBanner->isVisible());QVERIFY(!destinationStatus->isVisible());QCOMPARE(destinationStatus->text(),status->text());QVERIFY(destinationReload->isVisible());QCOMPARE(destinationReload->parentWidget()->parentWidget(),destinationStatus->parentWidget()->parentWidget());
  QVERIFY(!settingsReload->isHidden());QCOMPARE(settingsReload->parentWidget()->parentWidget(),status->parentWidget()->parentWidget());QCOMPARE(settingsReload->text(),QString("Reload from file…"));
  // An error is body text, not small print.
  QCOMPARE(status->font().pixelSize(),Ui::font(Ui::Type::body).pixelSize());
  // Cancel keeps the draft.
  QString shown;QTimer::singleShot(0,[&shown]{for(auto* widget:QApplication::topLevelWidgets())if(auto* box=qobject_cast<QMessageBox*>(widget);box&&box->objectName()=="confirmAction"&&box->isVisible()){shown=box->property("confirmationText").toString();box->button(QMessageBox::Cancel)->click();return;}});
  destinationReload->click();
  QVERIFY2(shown.contains("Advanced → Files in a batch"),qPrintable(shown));QVERIFY2(!shown.contains("Notifications")&&!shown.contains("notifications"),qPrintable(shown));
  QCOMPARE(number->text(),QString("invalid"));QVERIFY(window.settings()->saveViewState()["conflict"].toBool());QVERIFY(destinationReload->isVisible());
  // Reload from file replaces the draft with the file and clears the conflict.
  acceptNextConfirmation();destinationReload->click();
  PUTGD_TRY_COMPARE(number->text(),saved);QVERIFY(!window.settings()->saveViewState()["conflict"].toBool());QVERIFY(destinationReload->isHidden());QVERIFY(settingsReload->isHidden());QVERIFY(destinationBanner->isHidden());QVERIFY(settingsBanner->isHidden());
  QVERIFY(!window.findChild<QCheckBox*>("notifications")->isChecked());
  // Passive save information stays meta type.
  PUTGD_TRY_COMPARE(status->text(),QString("Settings reloaded from file."));QCOMPARE(status->font().pixelSize(),Ui::font(Ui::Type::technical).pixelSize());QVERIFY(statusCode(window).isEmpty());
 }
 // Inherited values are locked values: disabled, but painted as information
 // (textMuted in a read-only frame), unlike an unavailable action.
 void openingExistingConfigurationPreservesExactFileAndRoutes_data(){
  QTest::addColumn<QString>("kind");
  QTest::newRow("saved-empty")<<QString("empty");
  QTest::newRow("legacy-s3-1-default")<<QString("legacy");
  QTest::newRow("mixed-providers-and-action-exception")<<QString("mixed");
 }
 void openingExistingConfigurationPreservesExactFileAndRoutes(){
  QFETCH(QString,kind);
  QTemporaryDir dir;
  SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});
  auto doc=Settings::defaults();
  if(kind=="legacy"){
   doc=Settings::developmentTemplate();
   auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();
   profile["id"]="s3-1";profile["label"]="S3 destination";
   profile["credentials"]=QJsonObject{{"mode","keychain"},{"reference","putgd/s3-1"}};
   profiles[0]=profile;doc["profiles"]=profiles;
   auto actions=doc["actions"].toObject();
   for(const auto& name:Settings::actionNames()){auto action=actions[name].toObject();action["profileId"]="s3-1";actions[name]=action;}
   doc["actions"]=actions;
  }else if(kind=="mixed"){
   doc=threeDestinations();auto profiles=doc["profiles"].toArray();
   auto proton=profiles[1].toObject();proton["provider"]="proton-drive";proton["settings"]=ProtonProvider::defaults();profiles[1]=proton;
   auto backblaze=profiles[2].toObject();backblaze["provider"]="backblaze-b2";auto settings=b2::defaults();settings["bucket"]="synthetic-bucket";backblaze["settings"]=settings;profiles[2]=backblaze;
   doc["profiles"]=profiles;
  }
  QVERIFY(service.save(doc,{}));
  QFile file(service.path());QVERIFY(file.open(QIODevice::ReadOnly));const auto bytes=file.readAll();file.close();
  const auto revision=service.revision();
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);
  int writes=0;
  fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject&){if(method=="settings.save"||method=="settings.patch")++writes;};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  QCOMPARE(window.findChild<QListWidget*>("destinationList")->count(),doc["profiles"].toArray().size());
  const auto state=window.settings()->saveViewState();QVERIFY(!state.contains("stateError"));
  MainWindow reopened(fixture.session);QVERIFY(reopened.settings()->restoreViewState(state));
  fixture.session.invalidate();PUTGD_COMPARE_STEADY(writes,0,650);
  QCOMPARE(writes,0);QCOMPARE(service.document(),doc);QCOMPARE(service.revision(),revision);
  QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),bytes);
 }
 void unchangedFieldsDoNotSaveOrClaimChangesWereSaved(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));const auto original=service.document();const auto revision=service.revision();CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();auto* status=window.findChild<QLabel*>("autosaveStatus");auto* field=window.findChild<QLineEdit*>("maxFiles");auto* tabs=window.findChild<QTabWidget*>("settingsTabs");tabs->setCurrentIndex(3);
  fixture.client.failOnce["settings.save"]={"unexpected_save","Unchanged input must not dispatch settings.save",{}};
  field->setFocus();QTest::mouseClick(field,Qt::LeftButton);QTest::keyClick(field,Qt::Key_Tab);QVERIFY(QMetaObject::invokeMethod(field,"editingFinished"));putgd::test::settle(650);PUTGD_TRY_VERIFY(fixture.client.failOnce.contains("settings.save"));PUTGD_TRY_VERIFY(status->text().isEmpty());PUTGD_TRY_COMPARE(service.document(),original);PUTGD_TRY_COMPARE(service.revision(),revision);
  field->setFocus();field->setText("101");field->setText("100");QTest::keyClick(field,Qt::Key_Tab);putgd::test::settle(650);PUTGD_TRY_VERIFY(fixture.client.failOnce.contains("settings.save"));PUTGD_TRY_VERIFY(status->text().isEmpty());PUTGD_TRY_COMPARE(service.document(),original);
  fixture.client.failOnce.clear();field->setText("250");PUTGD_TRY_COMPARE(service.snapshot().limits.maxFiles,250);PUTGD_TRY_VERIFY(status->text().isEmpty());
  fixture.client.failOnce["settings.save"]={"unexpected_save","An unchanged accepted value must not save twice",{}};field->setFocus();QTest::keyClick(field,Qt::Key_Tab);putgd::test::settle(650);PUTGD_TRY_VERIFY(fixture.client.failOnce.contains("settings.save"));PUTGD_TRY_VERIFY(status->text().isEmpty());
  MainWindow reopened(fixture.session);QVERIFY(reopened.findChild<QLabel*>("autosaveStatus")->text().isEmpty());fixture.session.invalidate();putgd::test::settle(650);PUTGD_TRY_VERIFY(reopened.findChild<QLabel*>("autosaveStatus")->text().isEmpty());
 }
 void revertingWhileAutosaveReplyIsPendingPersistsTheVisibleValue_data(){
  QTest::addColumn<int>("rootMode");QTest::newRow("leaf")<<0;QTest::newRow("root-and-leaf")<<1;QTest::newRow("second-in-flight-save")<<2;
 }
 void revertingWhileAutosaveReplyIsPendingPersistsTheVisibleValue(){
  QFETCH(int,rootMode);QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();auto* local=window.findChild<QCheckBox*>("keepUploadCopies");QVERIFY(local->isChecked());fixture.client.replyDelayMs["settings.save"]=700;
  if(rootMode){auto* inherit=window.findChild<QCheckBox*>("useDefaultRegion");inherit->setChecked(!inherit->isChecked());if(rootMode==2){PUTGD_TRY_VERIFY(service.document().contains("captureDefaults"));QVERIFY(window.settings()->saveViewState().contains("stateError"));}}
  local->setChecked(false);PUTGD_TRY_VERIFY(!service.snapshot().archive.keepUploads);QVERIFY(window.settings()->saveViewState().contains("stateError"));local->setChecked(true);
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(service.snapshot().archive.keepUploads,4000);PUTGD_TRY_VERIFY(!window.settings()->saveViewState().contains("stateError"));PUTGD_TRY_VERIFY(!window.settings()->saveViewState()["dirty"].toBool());MainWindow reopened(fixture.session);reopened.settings();QVERIFY(reopened.findChild<QCheckBox*>("keepUploadCopies")->isChecked());
 }
 void newerEqualValuedParentSurvivesSubmittedChildOverride(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return provider.validateSettings(value);});QVERIFY(service.save(Settings::developmentTemplate(),{}));UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();auto* region=window.findChild<QLineEdit*>("s3Region");auto* bucket=window.findChild<QLineEdit*>("s3Bucket");const auto original=bucket->text();fixture.client.replyDelayMs["settings.save"]=700;
  region->setText("us-west-2");bucket->setText("synthetic-new-bucket");PUTGD_TRY_COMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["bucket"].toString(),QString("synthetic-new-bucket"));QVERIFY(window.settings()->saveViewState().contains("stateError"));bucket->setText(original);region->setText("us-east-2");region->setText("us-west-2");
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["bucket"].toString(),original,4000);PUTGD_TRY_VERIFY(!window.settings()->saveViewState().contains("stateError"));PUTGD_TRY_VERIFY(!window.settings()->saveViewState()["dirty"].toBool());QCOMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["region"].toString(),QString("us-west-2"));MainWindow reopened(fixture.session);reopened.settings();QCOMPARE(reopened.findChild<QLineEdit*>("s3Bucket")->text(),original);
 }
 void autosaveStatusReportsOnlyActionableIssues(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();window.show();auto* status=window.findChild<QLabel*>("autosaveStatus");QVERIFY(status);QVERIFY(status->text().isEmpty());
  auto* field=window.findChild<QLineEdit*>("maxFiles");field->setText("invalid");QVERIFY(status->text().isEmpty());PUTGD_TRY_VERIFY(field->property("invalid").toBool());QVERIFY(!window.findChild<QLabel*>("maxFilesError")->text().isEmpty());QVERIFY(status->text().isEmpty());
  field->setText("250");PUTGD_TRY_COMPARE(service.snapshot().limits.maxFiles,250);PUTGD_TRY_VERIFY(status->text().isEmpty());
  fixture.client.failOnce["settings.save"]={"client_unavailable","Synthetic lost reply",{}};field->setText("251");PUTGD_TRY_COMPARE(status->text(),QString("Changes not saved"));QVERIFY(status->toolTip().contains("Couldn’t save"));QVERIFY(window.findChild<QLabel*>("autosaveExplanation")->isHidden());QCOMPARE(service.snapshot().limits.maxFiles,250);window.findChild<QPushButton*>("retryAutosave")->click();PUTGD_TRY_COMPARE(service.snapshot().limits.maxFiles,251);PUTGD_TRY_VERIFY(status->text().isEmpty());
 }

 void editRailButtonOffersOtherSourcesWithoutGrowingTheRail(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(configuration(dir.path(),true),{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);
  QCOMPARE(window.findChildren<QToolButton*>("captureActionButton").size(),Settings::presentationOrder().size());
  QToolButton* record=nullptr;for(auto* button:window.findChildren<QToolButton*>("captureActionButton"))if(button->property("captureAction")=="record")record=button;QVERIFY(record);
  QToolButton* edit=nullptr;for(auto* button:window.findChildren<QToolButton*>("captureActionButton"))if(button->property("captureAction")=="edit")edit=button;
  // The other sources are the split button's menu, a right-click or a drop;
  // the main action stays Capture region & edit and never takes another source.
  QVERIFY(edit);QCOMPARE(edit->contextMenuPolicy(),Qt::CustomContextMenu);QVERIFY(edit->acceptDrops());QCOMPARE(edit->popupMode(),QToolButton::MenuButtonPopup);
  QVERIFY(edit->accessibleDescription().contains("menu"));QVERIFY(edit->toolTip().contains("\n"));
  auto* menu=window.findChild<QMenu*>("editSourceMenu");QVERIFY(menu);QCOMPARE(edit->menu(),menu);
  QStringList labels;for(auto* action:menu->actions())if(!action->isSeparator())labels.append(action->text().replace("&&","&"));
  // The menu holds only the other sources; the button itself is Capture region & edit.
  QCOMPARE(labels,QStringList({"Edit clipboard image","Edit an image file…"}));
  QStringList requests;connect(&window,&MainWindow::editRequested,this,[&](const QString& source,const QString& path){requests.append(source+"|"+path);});
  int actions=0;connect(&window,&MainWindow::actionRequested,this,[&](auto){++actions;});
  window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  emit edit->customContextMenuRequested(QPoint(8,8));PUTGD_TRY_VERIFY(menu->isVisible());menu->actions().at(0)->trigger();menu->hide();
  QCOMPARE(requests,QStringList({"clipboard|"}));QCOMPARE(actions,0);requests.clear();
  QImage pixel(2,2,QImage::Format_RGB32);pixel.fill(Qt::blue);const auto png=dir.filePath("Dropped.PNG");QVERIFY(pixel.save(png,"PNG"));
  const auto text=dir.filePath("notes.txt");{QFile file(text);QVERIFY(file.open(QIODevice::WriteOnly));file.write("x");}
  auto drop=[&](const QList<QUrl>& urls){
   QMimeData data;data.setUrls(urls);
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
   const QPointF at(20,20);
#else
   const QPoint at(20,20);
#endif
   QDragEnterEvent enter(at,Qt::CopyAction,&data,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(edit,&enter);
   const bool accepted=enter.isAccepted();const bool lit=edit->property("dropTarget").toBool();
   QDropEvent dropped(QPointF(at),Qt::CopyAction,&data,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(edit,&dropped);
   return accepted&&lit&&!edit->property("dropTarget").toBool();
  };
  QVERIFY(!drop({QUrl::fromLocalFile(text)}));QVERIFY(!edit->property("dropTarget").toBool());QVERIFY(!drop({QUrl::fromLocalFile(png),QUrl::fromLocalFile(png)}));QVERIFY(!drop({QUrl("https://example.invalid/a.png")}));QVERIFY(requests.isEmpty());
  QVERIFY(drop({QUrl::fromLocalFile(png)}));QCOMPARE(requests,QStringList({"file|"+QFileInfo(png).absoluteFilePath()}));QCOMPARE(actions,0);
 }
 void constructionDoesNotWriteAndHasNoSaveCancelControls(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider s3(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto settings){return s3.validateSettings(settings);});QVERIFY(service.save(Settings::developmentTemplate(),{}));const auto initial=service.document();const auto revision=service.revision();UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();putgd::test::settle(500);
  PUTGD_TRY_COMPARE(service.document(),initial);PUTGD_TRY_COMPARE(service.revision(),revision);PUTGD_TRY_VERIFY(!window.findChild<QPushButton*>("saveSettings"));PUTGD_TRY_VERIFY(!window.findChild<QPushButton*>("cancelSettings"));PUTGD_TRY_VERIFY(!window.findChild<QPushButton*>("setupLocal"));auto* tabs=window.findChild<QTabWidget*>("settingsTabs");QVERIFY(tabs);QCOMPARE(tabs->count(),4);QCOMPARE(tabs->tabText(0),QString("General"));QCOMPARE(tabs->tabText(3),QString("Advanced"));
 }
 void finishedFieldAutosavesAndSurvivesRestart(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));auto doc=configuration(dir.path(),true);QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(1);window.show();auto* root=window.findChild<QLineEdit*>("archiveRoot");QVERIFY(root);window.activateWindow();root->setFocus();PUTGD_TRY_VERIFY(root->hasFocus());root->selectAll();QTest::keyClicks(root,dir.filePath("saved-locally"));putgd::test::settle(450);PUTGD_TRY_COMPARE(service.document()["archive"].toObject()["root"],doc["archive"].toObject()["root"]);window.findChild<QCheckBox*>("keepUploadCopies")->setFocus();PUTGD_TRY_COMPARE(service.document()["archive"].toObject()["root"].toString(),dir.filePath("saved-locally"));SettingsService restarted(service.path());QVERIFY(restarted.reload());QCOMPARE(restarted.document()["archive"],service.document()["archive"]);
 }
 void invalidEditStaysInlineWhileIndependentValidToggleSaves(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);auto* number=window.findChild<QLineEdit*>("maxFiles");number->setText("not a number");QMetaObject::invokeMethod(number,"editingFinished");PUTGD_TRY_VERIFY(number->property("invalid").toBool());QCOMPARE(service.document()["limits"].toObject()["maxFiles"].toInt(),100);auto* notify=window.findChild<QCheckBox*>("notifications");notify->setChecked(false);PUTGD_TRY_VERIFY(!service.document()["preferences"].toObject()["notifications"].toBool());QCOMPARE(number->text(),QString("not a number"));QVERIFY(number->property("invalid").toBool());QCOMPARE(service.document()["limits"].toObject()["maxFiles"].toInt(),100);
 }
 void resetOneAndResetAllUseRealDefaults(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));auto doc=configuration(dir.path(),true);auto limits=doc["limits"].toObject();limits["concurrentUploads"]=4;doc["limits"]=limits;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();window.findChild<QToolButton*>("resourceLimitsToggle")->setChecked(true);window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);QPushButton* reset=nullptr;for(auto* button:window.findChildren<QPushButton*>())if(button->accessibleName()=="Reset Uploads at once to default")reset=button;QVERIFY(reset);QVERIFY(reset->isVisible());reset->click();PUTGD_TRY_COMPARE(service.document()["limits"].toObject()["concurrentUploads"].toInt(),3);window.findChild<QLineEdit*>("maxFiles")->setText("25");PUTGD_TRY_COMPARE(service.document()["limits"].toObject()["maxFiles"].toInt(),25);
  // #238: Diagnostics now shares the tab; Reset limits to defaults resets only the limits.
  auto* debug=window.findChild<QCheckBox*>("debugLogging");debug->setChecked(true);PUTGD_TRY_VERIFY(service.snapshot().debugLogging);
  auto* resetAll=window.findChild<QPushButton*>("resetAdvancedDefaults");QCOMPARE(resetAll->text(),QString("Reset resource and history limits…"));const auto confirmation=acceptNextConfirmation();resetAll->click();QVERIFY2(confirmation->startsWith("Reset limits to defaults\n")&&confirmation->contains("Debug logging")&&!confirmation->contains("Reset all"),qPrintable(*confirmation));PUTGD_TRY_COMPARE(service.document()["limits"].toObject()["maxFiles"].toInt(),100);
  putgd::test::settle(500);PUTGD_TRY_VERIFY(debug->isChecked());PUTGD_TRY_VERIFY(service.snapshot().debugLogging);
 }
 void externalEditsPreserveInvalidDraftAndReloadRecovers(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();auto* number=window.findChild<QLineEdit*>("maxFiles");number->setText("invalid");PUTGD_TRY_VERIFY(number->property("invalid").toBool());auto external=service.document();auto prefs=external["preferences"].toObject();prefs["notifications"]=false;external["preferences"]=prefs;QVERIFY(service.save(external,service.revision()));PUTGD_TRY_VERIFY(window.settings()->saveViewState()["conflict"].toBool());QCOMPARE(number->text(),QString("invalid"));acceptNextConfirmation();window.findChild<QPushButton*>("reloadSettings")->click();PUTGD_TRY_COMPARE(number->text(),QString("100"));QVERIFY(!window.settings()->saveViewState()["conflict"].toBool());QVERIFY(!window.findChild<QCheckBox*>("notifications")->isChecked());
 }
 void malformedExternalReloadPreservesAcceptedSettingsAndBlocksStaleOverwrite(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));const auto original=service.document();const auto revision=service.revision();CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();auto* field=window.findChild<QLineEdit*>("maxFiles");field->setText("invalid draft");PUTGD_TRY_VERIFY(field->property("invalid").toBool());QFile external(service.path());QVERIFY(external.open(QIODevice::WriteOnly|QIODevice::Truncate));QCOMPARE(external.write("{malformed"),qint64(10));external.close();acceptNextConfirmation();window.findChild<QPushButton*>("reloadSettings")->click();auto* status=window.findChild<QLabel*>("settingsStatus");PUTGD_TRY_VERIFY(status->property("errorCode").toString()=="config_invalid");PUTGD_TRY_VERIFY(!window.settings()->saveViewState().contains("stateError"));QCOMPARE(service.document(),original);QCOMPARE(service.revision(),revision);QVERIFY(!service.save(original,revision));QVERIFY(external.open(QIODevice::ReadOnly));QCOMPARE(external.readAll(),QByteArray("{malformed"));external.close();QVERIFY(external.open(QIODevice::WriteOnly|QIODevice::Truncate));const auto valid=QJsonDocument(original).toJson(QJsonDocument::Indented);QCOMPARE(external.write(valid),qint64(valid.size()));external.close();acceptNextConfirmation();window.findChild<QPushButton*>("reloadSettings")->click();PUTGD_TRY_COMPARE(field->text(),QString("100"));QCOMPARE(service.document(),original);
 }
 void appearanceThemeIsSavedAndReloaded(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();
  auto* theme=window.findChild<QComboBox*>("appearanceTheme");QVERIFY(theme);QCOMPARE(theme->currentData().toString(),QString("system"));QCOMPARE(theme->count(),3);
  window.findChild<QCheckBox*>("notifications")->setChecked(false);PUTGD_TRY_VERIFY(!service.document()["preferences"].toObject()["notifications"].toBool());QVERIFY(!service.document()["preferences"].toObject().contains("appearance"));
  theme->setCurrentIndex(theme->findData("light"));PUTGD_TRY_COMPARE(service.document()["preferences"].toObject()["appearance"].toObject()["theme"].toString(),QString("light"));QCOMPARE(service.snapshot().appearanceTheme,QString("light"));
  // A draft carrying the appearance object still checkpoints (view-state allow-list).
  const auto state=window.settings()->saveViewState();QVERIFY(state["draft"].toObject()["preferences"].toObject().contains("appearance"));QVERIFY(!validateUiState({{"schemaVersion",1},{"settings",state}}));
  auto doc=service.document();auto preferences=doc["preferences"].toObject();preferences["appearance"]=QJsonObject{{"theme","dark"}};doc["preferences"]=preferences;QVERIFY(service.save(doc,service.revision()));
  fixture.session.invalidate();PUTGD_TRY_COMPARE(theme->currentData().toString(),QString("dark"));
 }
 // Each command appears once: the application menu repeats neither the
 // command band nor the header's navigation, and Capture region has no menu
 // of the window and desktop buttons beside it.
 void failedAutosaveKeepsDraftAcrossReopenAndRequiresExplicitRetry(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));const auto before=service.document();CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();fixture.client.failOnce["settings.save"]={"client_unavailable","Synthetic lost save reply",{}};window.findChild<QLineEdit*>("archiveRoot")->setText(dir.filePath("retry-output"));PUTGD_TRY_VERIFY(window.settings()->saveViewState()["autosaveBlocked"].toBool());putgd::test::settle(600);PUTGD_TRY_COMPARE(service.document(),before);const auto state=window.settings()->saveViewState();MainWindow reopened(fixture.session);QVERIFY(reopened.settings()->restoreViewState(state));QVERIFY(reopened.findChild<QPushButton*>("retryAutosave")->isHidden()==false);reopened.findChild<QPushButton*>("retryAutosave")->click();PUTGD_TRY_COMPARE(service.document()["archive"].toObject()["root"].toString(),dir.filePath("retry-output"));QVERIFY(!reopened.settings()->saveViewState()["autosaveBlocked"].toBool());
 }

};
QTEST_MAIN(SettingsGui)
#include "test_settings_general.moc"
