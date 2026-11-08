#include "SettingsGuiFixture.h"
class SettingsGui:public SettingsGuiFixture {
 Q_OBJECT
private slots:
 void desktopSupportKeysResizeAndActionsHaveTheirOwnRole_data(){QTest::addColumn<int>("pixels");QTest::newRow("normal")<<14;QTest::newRow("large")<<21;QTest::newRow("preview")<<0;}
 void desktopSupportKeysResizeAndActionsHaveTheirOwnRole(){
  QFETCH(int,pixels);Fonts::load();Ui::apply(*qApp);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});if(pixels){auto font=original;font.setPixelSize(pixels);qApp->setFont(font);}QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1440,960);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);window.show();window.findChild<QToolButton*>("desktopSupportDetailsToggle")->setChecked(true);putgd::test::settle(30);auto* panel=window.findChild<QWidget*>("desktopSupportDetails");
  for(auto* key:panel->findChildren<QLabel*>())if(key->property("desktopSupportKey").toBool()){QCOMPARE(key->font().weight(),QFont::Normal);QCOMPARE(key->palette().color(QPalette::WindowText),Theme::current().textSecondary);QVERIFY2(key->fontMetrics().horizontalAdvance(key->text())<=key->width(),qPrintable(key->text()));QVERIFY2(key->heightForWidth(key->width())<=qMax(Ui::lineHeight(Ui::Type::body),key->fontMetrics().boundingRect(QRect(0,0,10000,10000),Qt::TextWordWrap,key->text()).height()+key->contentsMargins().top()+key->contentsMargins().bottom()),qPrintable(QString("%1: width %2, advance %3, heightForWidth %4, font height %5, minimum height %6, wrapped bounds %7, natural bounds %8, frame %9, indent %10").arg(key->text()).arg(key->width()).arg(key->fontMetrics().horizontalAdvance(key->text())).arg(key->heightForWidth(key->width())).arg(key->fontMetrics().height()).arg(key->minimumHeight()).arg(key->fontMetrics().boundingRect(QRect(0,0,key->width(),10000),Qt::TextWordWrap,key->text()).height()).arg(key->fontMetrics().boundingRect(QRect(0,0,10000,10000),Qt::TextWordWrap,key->text()).width()).arg(key->frameWidth()).arg(key->indent())));}
  auto* grid=qobject_cast<QGridLayout*>(panel->layout());QVERIFY(grid);
  for(auto* key:panel->findChildren<QLabel*>())if(key->property("desktopSupportKey").toBool()){
   int row,column,rowSpan,columnSpan;grid->getItemPosition(grid->indexOf(key),&row,&column,&rowSpan,&columnSpan);auto* item=grid->itemAtPosition(row,1);QVERIFY(item);auto* value=qobject_cast<QLabel*>(item->widget());QVERIFY(value);
   for(auto* label:{key,value}){QCOMPARE(label->alignment(),Qt::AlignLeft|Qt::AlignBottom);QVERIFY(label->minimumHeight()>=Ui::lineHeight(Ui::Type::body));}
   QCOMPARE(key->geometry().bottom(),value->geometry().bottom());
  }
  auto* label=window.findChild<QLabel*>("configPathLabel");const int wide=label->width();window.resize(1060,680);PUTGD_VERIFY_STEADY(label->width()<=wide,30);for(auto* key:panel->findChildren<QLabel*>())if(key->property("desktopSupportKey").toBool())QVERIFY(key->height()>=key->heightForWidth(key->width()));
  for(const auto& name:{"showConfigInFolder","openConfigEditor","reloadSettings","copyConfigPath","refreshDesktopSupport","copyDesktopDetails"}){auto* action=window.findChild<QPushButton*>(name);QCOMPARE(action->property("visualRole").toString(),QString("textAction"));QCOMPARE(action->font().weight(),QFont::Normal);QCOMPARE(action->font().pixelSize(),Ui::scaled(14));}
 }
 void retryRetentionChoicesPreserveStoredValuesAndConsequences(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));auto doc=Settings::defaults();auto retention=doc["retention"].toObject();retention["failedArtifactDays"]=17;doc["retention"]=retention;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);window.show();auto* combo=window.findChild<QComboBox*>("failedArtifactChoice");auto* edit=window.findChild<QLineEdit*>("failedArtifactDays");auto* note=window.findChild<QLabel*>("failedArtifactConsequence");QVERIFY(combo&&edit&&note);QVERIFY(!combo->isEditable());QVERIFY(edit->isHidden());QVERIFY(note->isVisible());QCOMPARE(combo->currentText(),QString("17 days"));QCOMPARE(edit->text(),QString("17"));
  for(const int days:{1,3,7,14,30,90,0,17}){const int index=combo->findData(QString::number(days));QVERIFY(index>=0);combo->setCurrentIndex(index);QMetaObject::invokeMethod(combo,"activated",Q_ARG(int,index));PUTGD_TRY_COMPARE(service.document()["retention"].toObject()["failedArtifactDays"].toInt(-1),days);QCOMPARE(combo->currentText(),days==0?QString("Forever"):days==1?QString("1 day"):QString("%1 days").arg(days));QVERIFY(note->isVisible());QVERIFY(!window.findChild<QLabel*>("failedArtifactDaysUnit"));QCOMPARE(window.settings()->saveViewState()["numbers"].toObject()["failedArtifactDays"].toString(),QString::number(days));}
  auto state=window.settings()->saveViewState();auto numbers=state["numbers"].toObject();numbers["failedArtifactDays"]="0";state["numbers"]=numbers;QVERIFY(window.settings()->restoreViewState(state));QCOMPARE(combo->currentText(),QString("Forever"));QCOMPARE(combo->currentData().toString(),QString("0"));
  // Reloaded configuration and view checkpoints retain unlisted values, too.
  state=window.settings()->saveViewState();numbers=state["numbers"].toObject();numbers["failedArtifactDays"]="23";state["numbers"]=numbers;QVERIFY(window.settings()->restoreViewState(state));QCOMPARE(combo->currentText(),QString("23 days"));QCOMPARE(combo->currentData().toString(),QString("23"));
 }

 void foreverIsARealChoiceAndStillStoresEmptyRetention(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(Settings::defaults(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");auto* combo=window.findChild<QComboBox*>("historyRetentionChoice");auto* edit=window.findChild<QLineEdit*>("historyRetentionDays");QCOMPARE(combo->currentIndex(),0);QCOMPARE(edit->text(),QString("Forever"));QVERIFY(combo->currentData().toString().isEmpty());QVERIFY(window.settings()->saveViewState()["numbers"].toObject()["historyRetentionDays"].toString().isEmpty());
  combo->setCurrentIndex(1);QMetaObject::invokeMethod(combo,"activated",Q_ARG(int,1));PUTGD_TRY_COMPARE(service.document()["retention"].toObject()["historyRetentionDays"].toInt(),30);combo->setCurrentIndex(0);QMetaObject::invokeMethod(combo,"activated",Q_ARG(int,0));PUTGD_TRY_VERIFY(service.document()["retention"].toObject()["historyRetentionDays"].isNull());QCOMPARE(edit->text(),QString("Forever"));edit->setText("17");QMetaObject::invokeMethod(edit,"editingFinished");PUTGD_TRY_COMPARE(service.document()["retention"].toObject()["historyRetentionDays"].toInt(),17);edit->clear();QMetaObject::invokeMethod(edit,"editingFinished");PUTGD_TRY_VERIFY(service.document()["retention"].toObject()["historyRetentionDays"].isNull());QCOMPARE(edit->text(),QString("Forever"));auto state=window.settings()->saveViewState();QVERIFY(window.settings()->restoreViewState(state));QCOMPARE(edit->text(),QString("Forever"));
  const auto help=combo->property("settingDescription").toString();QVERIFY2(help.contains("Choose Forever to keep history indefinitely, or enter a number of days. "),qPrintable(help));QVERIFY(!help.contains("which is the default"));
 }
 void copiedDiagnosticsPreserveExactPathsAndRevisions_data(){
  QTest::addColumn<bool>("unicodePath");
  QTest::newRow("ordinary-path")<<false;QTest::newRow("literal-zero-width-character")<<true;
 }
 void copiedDiagnosticsPreserveExactPathsAndRevisions(){
  QFETCH(bool,unicodePath);QTemporaryDir dir;
  const auto name=QString("config ")+(unicodePath?QString(QChar(0x200B)):QString{})+QString(64,'a')+".json";
  SettingsService service(dir.filePath(name));QVERIFY(service.save(Settings::defaults(),{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();
  auto* metadata=window.findChild<QLabel*>("settingsMetadata");QVERIFY(metadata);
  auto* copy=window.findChild<QPushButton*>("copyDesktopDetails");QVERIFY(copy);
  for(int revision=0;revision<2;++revision){
   const auto plain="Configuration: "+service.path()+"\nAccepted revision: "+service.revision()+"\nObserved file revision: "+service.metadata()["observedRevision"].toString();
   PUTGD_TRY_COMPARE(metadata->accessibleName(),plain);
   QVERIFY2(metadata->text().count(QChar(0x200B))>plain.count(QChar(0x200B)),"Fixture must exercise display-only wrap characters");
   QApplication::clipboard()->clear();copy->click();
   QCOMPARE(QApplication::clipboard()->text(),QString::fromUtf8(QJsonDocument(fixture.session.diagnostics()).toJson(QJsonDocument::Indented))+"\n"+plain);
   // A selection copied from the label is the exact text, not its display break points.
   QApplication::clipboard()->clear();metadata->setSelection(0,metadata->text().size());QTest::keyClick(metadata,Qt::Key_C,Qt::ControlModifier);
   QCOMPARE(QApplication::clipboard()->text(),plain);
   if(revision==0){auto doc=service.document();auto preferences=doc["preferences"].toObject();preferences["notifications"]=!preferences["notifications"].toBool();doc["preferences"]=preferences;QVERIFY(service.save(doc,service.revision()));}
  }
 }
 void diagnosticsGroupShowsLogsAndSavesPackages(){
  // #122: Settings → Advanced → Diagnostics (#238), its package dialog and the ui.diagnostics command.
  QTemporaryDir dir;logging::open(dir.filePath("state/logs"),{},"putgd-test");auto closeLog=qScopeGuard([]{logging::close();});
  for(int i=0;i<3;++i)logging::info(logApp(),"Synthetic diagnostics record");
  logging::flush();
  SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=Settings::developmentTemplate();auto archive=doc["archive"].toObject();archive["root"]=dir.filePath("archive");doc["archive"]=archive;QVERIFY(service.save(doc,{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);
  DiagnosticsBundles bundles(fixture.operations(),dir.filePath("state"));fixture.server.setDiagnosticsBundles(&bundles);
  QJsonArray notified;QStringList revealed;fixture.server.setDiagnosticsNotifications([&](const QJsonObject& saved){notified.append(saved);},[&](const QString& path){revealed.append(path);return Error{};});
  const QJsonObject unclean{{"detected",false},{"last",QJsonObject{{"cause","unknown"},{"previousVersion","0.1.1-synthetic"},{"previousStart","2026-10-19T23:10:00.000Z"},{"detectedAt","2026-10-19T23:15:00.000Z"}}}};
  bool degrade=false;
  fixture.server.setUiDiagnostics([&]{auto data=fixture.operations().diagnostics();data["uncleanExit"]=unclean;if(degrade){auto logging=data["logging"].toObject();logging["degraded"]=true;logging["degradedReason"]="disk_full";logging["droppedRecords"]=12;data["logging"]=logging;data["loggingDegraded"]=true;}return data;});
  MainWindow window(fixture.session);window.navigate("settings");window.show();fixture.session.refreshSnapshot();
  auto* settings=window.settings();auto* summary=window.findChild<QLabel*>("logSummary");QVERIFY(summary);
  // Diagnostics and the disclosed configuration controls share Advanced.
  auto* tabs=window.findChild<QTabWidget*>("settingsTabs");
  for(const auto* name:{"debugLogging","logSummary","clearLogs","createDiagnosticsPackage","uncleanExit"}){auto* control=window.findChild<QWidget*>(name);QVERIFY2(control&&tabs->widget(3)->isAncestorOf(control),name);QVERIFY2(!tabs->widget(0)->isAncestorOf(control),name);}
  auto* details=window.findChild<QWidget*>("desktopSupportDetails");QVERIFY(details&&details->isAncestorOf(window.findChild<QPushButton*>("reloadSettings")));QVERIFY(details->isHidden());tabs->setCurrentIndex(3);
  PUTGD_TRY_VERIFY2(summary->text().contains("files")||summary->text().contains("1 file"),qPrintable(summary->text()));QVERIFY(summary->text().contains("oldest record"));
  PUTGD_TRY_VERIFY(window.findChild<QLabel*>("uncleanExit")->text().contains("0.1.1-synthetic"));QVERIFY(window.findChild<QLabel*>("uncleanExit")->text().contains("stopped unexpectedly"));
  auto* degraded=window.findChild<QLabel*>("loggingDegraded");QVERIFY(degraded->isHidden());
  degrade=true;fixture.session.refreshSnapshot();PUTGD_TRY_VERIFY(degraded->isVisibleTo(settings));QVERIFY(degraded->text().contains("disk is full"));QVERIFY(degraded->text().contains("12 records"));degrade=false;
  // Debug logging is a setting: it saves like the others and keeps packageFolder beside it.
  auto* debug=window.findChild<QCheckBox*>("debugLogging");QVERIFY(debug);QVERIFY(!debug->isChecked());debug->setChecked(true);
  PUTGD_TRY_VERIFY(service.snapshot().debugLogging);QVERIFY(window.findChild<QLabel*>("debugLoggingStatus")->text().startsWith("On."));
  // Clear logs through the UI command (the button asks first).
  auto command=[&](const QJsonObject& params)->std::pair<Error,QJsonObject>{bool done=false;Error error{"test_timeout","No answer",{}};QJsonObject result;settings->diagnosticsCommand(params,[&](Error e,QJsonObject r){error=e;result=r;done=true;});if(!putgd::test::waitFor([&]{return done;},15000))qWarning("diagnostics command timed out");return {error,result};};
  auto [cleared,clearState]=command({{"action","clearLogs"}});QVERIFY2(!cleared,qPrintable(cleared.message));QVERIFY(clearState["result"].toObject()["removedFiles"].toInt()>=1);
  // The dialog previews the package and writes nothing until Save.
  window.findChild<QPushButton*>("createDiagnosticsPackage")->click();auto* dialog=settings->findChild<QDialog*>("diagnosticsPackageDialog");QVERIFY(dialog);PUTGD_TRY_VERIFY(dialog->isVisible());
  auto* contents=dialog->findChild<QLabel*>("packageContents");PUTGD_TRY_VERIFY2(contents->text().contains("logs/putgd.jsonl"),qPrintable(contents->text()));QVERIFY(contents->text().contains("manifest.json"));
  QVERIFY(dialog->findChild<QWidget*>("diagnosticsPackageExtras"));
  QCOMPARE(QDir::fromNativeSeparators(dialog->findChild<QLabel*>("packageFolder")->text()),QDir(dir.filePath("archive")).filePath("diagnostics"));
  QVERIFY(dialog->findChild<QLabel*>("packageRedaction")->text().contains("replaced by hashes"));
  auto* inventory=dialog->findChild<QToolButton*>("packageContentsToggle");QVERIFY(inventory);QVERIFY(dialog->findChild<QLabel*>("packageContents")->isHidden());inventory->click();
  // Wrapped labels get their full height once the preview has filled them (no clipped lines).
  for(const auto* name:{"packageScope","packageContents","packageOperations","packageRedaction"}){auto* label=dialog->findChild<QLabel*>(name);PUTGD_TRY_VERIFY2(label->height()>=label->heightForWidth(label->width()),name);}
  dialog->findChild<QCheckBox*>("includeFileNames")->setChecked(true);PUTGD_TRY_VERIFY(dialog->findChild<QLabel*>("packageRedaction")->text().contains("kept as they are"));
  PUTGD_TRY_COMPARE(settings->diagnosticsState()["package"].toObject()["preview"].toObject()["redaction"].toString(),QString("names"));
  QVERIFY(!QFileInfo::exists(dir.filePath("archive/diagnostics")));
  // A chosen folder is remembered in preferences.diagnostics.packageFolder.
  QTemporaryDir out;auto [moved,movedState]=command({{"action","setPackage"},{"folder",out.path()}});QVERIFY2(!moved,qPrintable(moved.message));
  QCOMPARE(movedState["package"].toObject()["preview"].toObject()["folder"].toString(),out.path());
  PUTGD_TRY_COMPARE(service.document()["preferences"].toObject()["diagnostics"].toObject()["packageFolder"].toString(),out.path());QVERIFY(service.snapshot().debugLogging);
  QCOMPARE(command({{"action","setPackage"},{"folder",dir.filePath("missing")}}).first.code,QString("diagnostics_output_unavailable"));
  dialog->findChild<QPushButton*>("savePackage")->click();auto* result=dialog->findChild<QLabel*>("packageResult");PUTGD_TRY_VERIFY(result->isVisibleTo(dialog));QVERIFY2(result->text().startsWith("Saved putgd-diagnostics-"),qPrintable(result->text()));
  QCOMPARE(notified.size(),1);const auto path=notified[0].toObject()["path"].toString();QCOMPARE(QFileInfo(path).absolutePath(),QFileInfo(out.path()).absoluteFilePath());QCOMPARE(notified[0].toObject()["redaction"].toString(),QString("names"));
  auto* show=dialog->findChild<QPushButton*>("showPackageInFolder");QVERIFY(show->isVisibleTo(dialog));auto* close=dialog->findChild<QPushButton*>("closePackage");auto* save=dialog->findChild<QPushButton*>("savePackage");PUTGD_TRY_VERIFY(save->x()-close->geometry().right()-1>=8);QVERIFY(close->x()-show->geometry().right()-1>=8);show->click();PUTGD_TRY_COMPARE(revealed,QStringList{path});
  QCOMPARE(command({{"action","savePackage"}}).first.code,QString("diagnostics_busy"));
  QCOMPARE(command({{"action","closePackage"}}).second["package"].toObject()["open"].toBool(),false);QVERIFY(!dialog->isVisible());
  QCOMPARE(command({{"action","savePackage"}}).first.code,QString("diagnostics_dialog_closed"));
  // Reopened, it starts from the remembered folder with names hashed again.
  auto [reopened,openState]=command({{"action","openPackage"}});QVERIFY(!reopened);const auto package=openState["package"].toObject();
  QVERIFY(package["open"].toBool());QCOMPARE(package["folder"].toString(),QDir::toNativeSeparators(out.path()));QCOMPARE(package["preview"].toObject()["redaction"].toString(),QString("hashed"));
  auto [savedAgain,savedState]=command({{"action","savePackage"}});QVERIFY2(!savedAgain,qPrintable(savedAgain.message));QVERIFY(QFileInfo(savedState["package"].toObject()["saved"].toObject()["path"].toString()).isFile());
  command({{"action","closePackage"}});
  QVERIFY(!validateUiState({{"schemaVersion",1},{"settings",settings->saveViewState()}}));
 }
 // #158: Also upload and copy a link, offered once diagnostics.uploadTarget says the
 // destination works; the check never blocks the dialog and a broken one says why.
 void diagnosticsPackageOffersAWorkingUpload(){
  QTemporaryDir dir;logging::open(dir.filePath("state/logs"),{},"putgd-test");auto closeLog=qScopeGuard([]{logging::close();});
  logging::info(logApp(),"Synthetic diagnostics record");logging::flush();
  ProviderRegistry providers;auto provider=std::make_shared<test::OpaqueProvider>();providers.add("opaque",provider);
  SettingsService service(dir.filePath("config.json"),[&](auto id,auto value){return providers.validate(id,value);});QVERIFY(service.save(test::configuration(dir.path()),{}));
  test::Source source;OperationService operations(dir.filePath("data"),service,providers,source);PUTGD_TRY_VERIFY(operations.ready());HistoryService history(operations);
  UiSessionFixture fixture(service,operations,history);
  DiagnosticsBundles bundles(operations,dir.filePath("state"));fixture.server.setDiagnosticsBundles(&bundles);
  Error testResult{"s3_authentication_failed","Synthetic authentication failure",{}};bool holdTest=false;std::function<void(Error)> heldTest;int tests=0;
  bundles.setDestinationTest([&](const QString& profileId,std::function<void(Error)> done){QCOMPARE(profileId,QString("test"));++tests;if(holdTest)heldTest=done;else done(testResult);});
  MainWindow window(fixture.session);window.navigate("settings");window.show();fixture.session.refreshSnapshot();auto* settings=window.settings();
  auto command=[&](const QJsonObject& params)->std::pair<Error,QJsonObject>{bool done=false;Error error{"test_timeout","No answer",{}};QJsonObject result;settings->diagnosticsCommand(params,[&](Error e,QJsonObject r){error=e;result=r;done=true;});if(!putgd::test::waitFor([&]{return done;},15000))qWarning("diagnostics command timed out");return {error,result};};
  auto package=[&]{return settings->diagnosticsState()["package"].toObject();};
  // A destination that fails Test connection: no checkbox, the reason instead.
  auto [opened,openState]=command({{"action","openPackage"}});QVERIFY2(!opened,qPrintable(opened.message));
  auto* dialog=settings->findChild<QDialog*>("diagnosticsPackageDialog");auto* upload=dialog->findChild<QCheckBox*>("uploadPackage");auto* note=dialog->findChild<QLabel*>("packageUploadNote");
  QVERIFY(upload&&note);QVERIFY(dialog->findChild<QWidget*>("diagnosticsPackageExtras")->isAncestorOf(upload));
  PUTGD_TRY_VERIFY(!package()["uploadTarget"].toObject()["checking"].toBool());
  QCOMPARE(package()["uploadTarget"].toObject()["reason"].toString(),QString("diagnostics_destination_unavailable"));
  QVERIFY(!upload->isVisibleTo(dialog));QCOMPARE(note->text(),QString("Upload unavailable: Test connection to Synthetic opaque provider failed: Synthetic authentication failure."));
  QCOMPARE(command({{"action","setPackage"},{"upload",true}}).first.code,QString("diagnostics_upload_unavailable"));
  QCOMPARE(dialog->findChild<QPushButton*>("savePackage")->text(),QString("Save package"));
  command({{"action","closePackage"}});
  // A slow check: the preview loads and a local save works meanwhile.
  holdTest=true;testResult={};
  command({{"action","openPackage"}});QVERIFY(package()["uploadTarget"].toObject()["checking"].toBool());QVERIFY(note->text().startsWith("Checking"));
  QCOMPARE(command({{"action","setPackage"},{"upload",true}}).first.code,QString("diagnostics_upload_checking"));
  auto [local,localState]=command({{"action","savePackage"}});QVERIFY2(!local,qPrintable(local.message));
  QVERIFY(localState["package"].toObject()["saved"].toObject()["path"].toString().endsWith(".zip"));QVERIFY(!localState["package"].toObject()["saved"].toObject().contains("upload"));
  command({{"action","closePackage"}});PUTGD_TRY_VERIFY(heldTest);std::exchange(heldTest,{})({});holdTest=false;
  // A working destination: offered, named and with its warning, but never ticked
  // by the check; only ticking it makes Save upload, link and copy.
  const int uploadsBefore=provider->uploads.load();auto* save=dialog->findChild<QPushButton*>("savePackage");
  command({{"action","openPackage"}});PUTGD_TRY_VERIFY(package()["uploadTarget"].toObject()["available"].toBool());
  QVERIFY(upload->isVisibleTo(dialog));QVERIFY(upload->isEnabled());QVERIFY(!upload->isChecked());QVERIFY2(upload->text().contains("Synthetic opaque provider"),qPrintable(upload->text()));
  QVERIFY(note->text().contains("anyone with the link"));QCOMPARE(save->text(),QString("Save package"));QVERIFY(!package()["upload"].toBool());
  upload->setChecked(true);QCOMPARE(save->text(),QString("Save and upload"));QVERIFY(package()["upload"].toBool());
  upload->setChecked(false);QCOMPARE(save->text(),QString("Save package"));upload->click();QCOMPARE(save->text(),QString("Save and upload"));
  auto [started,startedState]=command({{"action","savePackage"}});QVERIFY2(!started,qPrintable(started.message));
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(package().contains("saved"),20000);
  const auto saved=package()["saved"].toObject();QVERIFY2(saved["link"].toString().startsWith("https://"),QJsonDocument(saved).toJson().constData());
  QVERIFY(saved["linkCopied"].toBool());QCOMPARE(source.copied.text,saved["link"].toString());QCOMPARE(provider->uploads.load(),uploadsBefore+1);QVERIFY(QFileInfo(saved["path"].toString()).isFile());
  auto* result=dialog->findChild<QLabel*>("packageResult");QVERIFY2(result->text().contains("Uploaded to Synthetic opaque provider. Link copied: https://"),qPrintable(result->text()));
  command({{"action","closePackage"}});
  // Every opening starts unticked again; an agent can leave it so: the package stays local.
  command({{"action","openPackage"}});PUTGD_TRY_VERIFY(package()["uploadTarget"].toObject()["available"].toBool());
  QVERIFY(!upload->isChecked());QVERIFY(!package()["upload"].toBool());QCOMPARE(save->text(),QString("Save package"));
  auto [cleared,clearedState]=command({{"action","setPackage"},{"upload",false}});QVERIFY(!cleared);QVERIFY(!clearedState["package"].toObject()["upload"].toBool());
  auto [plain,plainState]=command({{"action","savePackage"}});QVERIFY(!plain);QVERIFY(!plainState["package"].toObject()["saved"].toObject().contains("link"));QCOMPARE(provider->uploads.load(),uploadsBefore+1);
  command({{"action","closePackage"}});
  operations.shutdown();
 }
 // At 150% text the package dialog stays inside the screen: its body scrolls and
 // Cancel and Save stay pinned and whole below it.
 void diagnosticsPackageDialogFitsTheScreenAtLargeText(){
  auto font=QApplication::font();const auto original=font;auto restore=qScopeGuard([&]{QApplication::setFont(original);});font.setPixelSize(21);QApplication::setFont(font);
  QTemporaryDir dir;logging::open(dir.filePath("state/logs"),{},"putgd-test");auto closeLog=qScopeGuard([]{logging::close();});logging::info(logApp(),"Synthetic diagnostics record");logging::flush();
  SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=Settings::developmentTemplate();auto archive=doc["archive"].toObject();archive["root"]=dir.filePath("archive");doc["archive"]=archive;QVERIFY(service.save(doc,{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);DiagnosticsBundles bundles(fixture.operations(),dir.filePath("state"));fixture.server.setDiagnosticsBundles(&bundles);
  // The layout fixture needs a journal to preview, not an in-flight migration.
  PUTGD_TRY_VERIFY(fixture.operations().ready());
  MainWindow window(fixture.session);window.navigate("settings");window.show();fixture.session.refreshSnapshot();auto* settings=window.settings();
  bool opened=false;settings->diagnosticsCommand({{"action","openPackage"}},[&](Error,QJsonObject){opened=true;});PUTGD_TRY_VERIFY(opened);
  auto* dialog=settings->findChild<QDialog*>("diagnosticsPackageDialog");QVERIFY(dialog&&dialog->isVisible());QVERIFY(putgd::test::waitForWindowExposed(dialog));
  PUTGD_TRY_VERIFY(!settings->diagnosticsState()["package"].toObject()["uploadTarget"].toObject()["checking"].toBool());putgd::test::settle(100);
  const auto screen=dialog->screen()->availableGeometry();QCOMPARE(dialog->minimumWidth(),qMin(Ui::scaled(560),screen.width()));QVERIFY(dialog->width()<=screen.width());QVERIFY2(dialog->height()<=screen.height(),qPrintable(QString("dialog %1 px on a %2 px screen").arg(dialog->height()).arg(screen.height())));
  auto* scroll=dialog->findChild<QScrollArea*>("diagnosticsPackageScroll");QVERIFY(scroll);QVERIFY(scroll->widget()->isAncestorOf(dialog->findChild<QLabel*>("packageContents")));
  auto* contents=dialog->findChild<QLabel*>("packageContents");QVERIFY(contents->isHidden());auto* inventory=dialog->findChild<QToolButton*>("packageContentsToggle");QVERIFY(inventory);inventory->click();QVERIFY(!contents->isHidden());PUTGD_VERIFY_STEADY(!contents->text().isEmpty(),50);QVERIFY(contents->height()>=contents->heightForWidth(contents->width()));
  // Large text needs more than the screen holds: the body scrolls, the buttons stay outside it.
  QVERIFY2(scroll->verticalScrollBar()->maximum()>0,qPrintable(QString("body %1 in %2").arg(scroll->widget()->height()).arg(scroll->viewport()->height())));
  for(const char* name:{"closePackage","savePackage"}){auto* button=dialog->findChild<QPushButton*>(name);QVERIFY(button->isVisible());QVERIFY(!scroll->isAncestorOf(button));
   const QRect shown(button->mapTo(dialog,QPoint()),button->size());QVERIFY2(dialog->rect().contains(shown),qPrintable(QString("%1 outside the dialog").arg(name)));}
  auto* cancel=dialog->findChild<QPushButton*>("closePackage");auto* save=dialog->findChild<QPushButton*>("savePackage");QVERIFY(save->x()-cancel->geometry().right()-1>=8);
  auto* footer=dialog->layout()->itemAt(dialog->layout()->count()-1)->layout();QVERIFY(footer);QVERIFY(footer->geometry().height()>=Ui::scaled(Ui::Metric::statusBand));
  QCOMPARE(dialog->findChild<QLabel*>("packageFolder")->font().family(),Ui::monoFamily());
  dialog->reject();
 }
 // #149: what Auto resolves to, from settings.get effectiveLimits.
 void autoLimitsShowWhatTheyResolveTo(){
  constexpr qint64 gib=1LL<<30;ArtifactStore::setDiskForTesting(60*gib,100*gib);auto resetDisk=qScopeGuard([]{ArtifactStore::setDiskForTesting(-1,-1);});
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();
  auto* file=window.findChild<QLabel*>("maxFileBytesResolved");auto* batch=window.findChild<QLabel*>("maxBatchBytesResolved");auto* waiting=window.findChild<QLabel*>("pendingArtifactBytesResolved");
  QVERIFY(file&&batch&&waiting);
  PUTGD_TRY_COMPARE(file->text(),QString("No limit on this computer · keeps 4 GB free"));QCOMPARE(batch->text(),file->text());QCOMPARE(waiting->text(),file->text());auto* reserve=window.findChild<QLabel*>("uploadReserveHint");QVERIFY(reserve);QCOMPARE(reserve->text(),file->text());for(auto* hint:{file,batch,waiting})QVERIFY(hint->isHidden());
  QVERIFY(file->toolTip().contains("keeps 4 GB free"));
  // A typed size or No limit is its own answer.
  auto* edit=window.findChild<QLineEdit*>("maxFileBytes");edit->setText("2");QVERIFY(file->text().isEmpty());QVERIFY(!file->isVisibleTo(window.settings()));QVERIFY(reserve->isHidden());QVERIFY(!batch->isHidden());
  edit->setText("auto");QCOMPARE(file->text(),QString("No limit on this computer · keeps 4 GB free"));
 }
 void longDropdownScrollsWithinItsVisibleItemLimit(){
  Ui::apply(*qApp);QWidget root;auto* layout=new QVBoxLayout(&root);auto* combo=Ui::choiceBox(&root);
  combo->setMaxVisibleItems(7);for(int i=0;i<60;++i)combo->addItem(QString("Option %1").arg(i+1));
  layout->addWidget(combo);layout->addStretch();Ui::styleWindow(root);root.resize(600,400);root.show();combo->showPopup();
  auto* view=combo->view();PUTGD_TRY_VERIFY(view->isVisible());QVERIFY(view->verticalScrollBar()->maximum()>0);
  QVERIFY(view->viewport()->height()<view->sizeHintForRow(0)*8);
  QTest::keyClick(view,Qt::Key_End);QTest::keyClick(view,Qt::Key_Return);PUTGD_TRY_COMPARE(combo->currentIndex(),59);PUTGD_TRY_VERIFY(!view->isVisible());
 }
 void debugLoggingDescriptionStaysInsideItsRow_data(){QTest::addColumn<int>("pixels");QTest::newRow("100%")<<14;QTest::newRow("150%")<<21;}
 void debugLoggingDescriptionStaysInsideItsRow(){
  QFETCH(int,pixels);Ui::apply(*qApp);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(pixels);qApp->setFont(font);QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(Settings::defaults(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));auto* debug=window.findChild<QCheckBox*>("debugLogging");auto* description=window.findChild<QLabel*>("debugLoggingStatus");QVERIFY(debug&&description);auto* row=description->parentWidget();while(row&&!row->property("preferenceRow").toBool())row=row->parentWidget();QVERIFY(row);
  for(const bool checked:{false,true,false}){debug->setChecked(checked);PUTGD_TRY_COMPARE(service.snapshot().debugLogging,checked);for(int width=900;width<=1440;width+=10){window.resize(width,900);PUTGD_VERIFY_STEADY(row->rect().contains(QRect(description->mapTo(row,QPoint()),description->size())),5);QVERIFY2(row->rect().contains(QRect(description->mapTo(row,QPoint()),description->size())),qPrintable(QString("debug %1 at %2: description %3 row %4").arg(checked).arg(width).arg(description->height()).arg(row->height())));QVERIFY(description->height()>=description->heightForWidth(description->width()));}}
 }
 void exactAndSymbolicLimitsKeepTheirInputWithCaptionUnits(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();for(const auto* key:{"maxFileBytes","maxBatchBytes","pendingArtifactBytes","imageBytes","imagePixels"}){auto* edit=window.findChild<QLineEdit*>(key);QVERIFY(edit);QVERIFY(window.findChild<QLabel*>(QString(key)+"Unit"));for(const auto& value:QStringList{"Auto","","2.000000000931322574615478515625"}){edit->setText(value);QCOMPARE(edit->text(),value);}}QVERIFY(window.findChild<QLabel*>("failedArtifactConsequence"));
 }

 void artifactLimitsOfferAutoAndNoLimit(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();
  auto limit=[&](const QString& key)->QJsonValue{return service.document()["limits"].toObject().value(key);};
  for(const auto& key:Settings::artifactByteLimitKeys()){
   auto* edit=window.findChild<QLineEdit*>(key);auto* combo=window.findChild<QComboBox*>(key+"Choice");QVERIFY(edit);QVERIFY(combo);
   QCOMPARE(edit->text(),QString("Auto"));QCOMPARE(edit->placeholderText(),QString("No limit"));
   QCOMPARE(combo->itemText(0),QString("Auto"));QCOMPARE(combo->itemText(1),QString("No limit"));
   QVERIFY(!window.findChild<QToolButton*>(key+"ChoiceHelp"));
  }
  auto* edit=window.findChild<QLineEdit*>("maxFileBytes");auto* combo=window.findChild<QComboBox*>("maxFileBytesChoice");
  combo->setCurrentIndex(1);emit combo->activated(1);
  PUTGD_TRY_VERIFY(limit("maxFileBytes").isNull());QCOMPARE(edit->text(),QString());
  edit->setText("3");PUTGD_TRY_COMPARE(limit("maxFileBytes").toInteger(),3221225472LL);
  edit->setText("auto");PUTGD_TRY_COMPARE(limit("maxFileBytes"),QJsonValue("auto"));
  combo->setCurrentIndex(0);emit combo->activated(0);edit->setText("");PUTGD_TRY_VERIFY(limit("maxFileBytes").isNull());
  combo->setCurrentIndex(0);emit combo->activated(0);PUTGD_TRY_COMPARE(limit("maxFileBytes"),QJsonValue("auto"));QCOMPARE(edit->text(),QString("Auto"));
  QCOMPARE(limit("maxBatchBytes"),QJsonValue("auto"));
 }
 void configEditorFailureOffersSelectablePathAndCopy(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(Settings::defaults(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);window.show();window.findChild<QToolButton*>("desktopSupportDetailsToggle")->setChecked(true);QVERIFY(QFile::remove(service.path()));window.findChild<QPushButton*>("openConfigEditor")->click();auto* failure=window.findChild<QLabel*>("configEditorFailure");auto* copy=window.findChild<QPushButton*>("copyConfigPath");PUTGD_TRY_VERIFY(failure->isVisible());QVERIFY(copy->isVisible());auto* path=window.findChild<QLabel*>("configPath");QVERIFY(path->textInteractionFlags()&Qt::TextSelectableByKeyboard);copy->click();QCOMPARE(qApp->clipboard()->text(),service.path());
 }
 void resourcesDiscloseModifiedAndInvalidLimits(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::defaults(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);window.show();auto* toggle=window.findChild<QToolButton*>("resourceLimitsToggle");auto* limits=window.findChild<QWidget*>("resourceLimits");QVERIFY(toggle&&limits);QVERIFY(!toggle->isChecked());QVERIFY(limits->isHidden());QVERIFY(window.findChild<QLineEdit*>("historyRetentionDays")->isVisible());QVERIFY(window.findChild<QPushButton*>("createDiagnosticsPackage")->isVisible());
  toggle->setChecked(true);auto state=window.settings()->saveViewState();QVERIFY(window.settings()->restoreViewState(state));QVERIFY(toggle->isChecked());toggle->setChecked(false);auto* count=window.findChild<QLineEdit*>("concurrentUploads");count->setText("5");QVERIFY(!toggle->isChecked());count->setText("invalid");QMetaObject::invokeMethod(count,"editingFinished");PUTGD_TRY_VERIFY(toggle->isChecked());
 }
 void desktopSupportUsesADisclosureAndCopiesExactDetails(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(Settings::defaults(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);const QJsonObject details{{"trayAvailable",true},{"shortcuts",QJsonObject{{"available",false}}},{"capabilities",QJsonObject{{"source",QJsonObject{{"adapter","gnome-wayland"},{"capture",QJsonObject{{"region",QJsonObject{{"available",true}}}}}}}}}};fixture.server.setUiDiagnostics([&]{return details;});fixture.session.refreshSnapshot();PUTGD_TRY_COMPARE(fixture.session.diagnostics(),details);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);window.show();auto* settings=window.settings();auto* body=window.findChild<QWidget*>("desktopSupportDetails");QVERIFY(body&&body->isHidden());fixture.server.setUiCommand([settings](const QString&,const QJsonObject& params,JsonReply reply){settings->diagnosticsCommand(params,std::move(reply));});auto command=[&](QJsonObject params){bool done=false;Error result;fixture.client.request("ui.diagnostics",params,settings,[&](Error error,QJsonObject){result=error;done=true;});if(!putgd::test::waitFor([&]{return done;}))return Error{"test_timeout","Diagnostics command did not finish",{}};return result;};QVERIFY(!command({{"action","setDesktopDetails"},{"enabled",true}}));QVERIFY(body->isVisible());QCOMPARE(window.findChild<QLabel*>("desktopSupportRegion")->text(),QString("Available"));QCOMPARE(window.findChild<QLabel*>("desktopSupportShortcuts")->text(),QString("Unavailable"));QCOMPARE(window.findChild<QLabel*>("desktopSupportTray")->text(),QString("Available"));QVERIFY(!command({{"action","copyDetails"}}));QVERIFY(qApp->clipboard()->text().startsWith(QString::fromUtf8(QJsonDocument(details).toJson(QJsonDocument::Indented))));auto state=settings->saveViewState();QVERIFY(!state.contains("stateError"));auto invalid=state;invalid["desktopSupportDetails"]="invalid";QVERIFY(!settings->restoreViewState(invalid));QVERIFY(!command({{"action","setDesktopDetails"},{"enabled",false}}));QVERIFY(body->isHidden());QVERIFY(settings->restoreViewState(state));QVERIFY(body->isVisible());QCOMPARE(command({{"action","setDesktopDetails"}}).code,QString("invalid_request"));QVERIFY(!command({{"action","refreshDesktopSupport"}}));QList<QUrl> opened;host::setTestOpener([&](const QUrl& url){opened.append(url);});auto cleanup=qScopeGuard([]{host::setTestOpener({});});QVERIFY(!command({{"action","showConfigFolder"}}));QCOMPARE(opened,QList<QUrl>{QUrl::fromLocalFile(dir.path())});
 }
 void unavailableCaptureKindShowsTheAdaptersReason(){
  // #1295: labwc 0.9 offers no window capture; Settings and the toolbar say so before anyone presses it.
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(Settings::defaults(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);
  const QString reason="Window capture needs ext-foreign-toplevel-image-capture-source-manager-v1, which this compositor does not offer";
  QJsonObject details{{"trayAvailable",true},{"capabilities",QJsonObject{{"source",QJsonObject{{"adapter","budgie-wayland"},{"capture",QJsonObject{{"region",QJsonObject{{"available",true}}},{"window",QJsonObject{{"available",false},{"code","window_unavailable"},{"error",reason}}},{"desktop",QJsonObject{{"available",true}}}}}}}}}};
  fixture.server.setUiDiagnostics([&]{return details;});fixture.session.refreshSnapshot();PUTGD_TRY_COMPARE(fixture.session.diagnostics(),details);
  MainWindow window(fixture.session);QToolButton* windowButton=nullptr;QToolButton* desktopButton=nullptr;for(auto* button:window.findChildren<QToolButton*>()){if(button->property("captureAction")=="window")windowButton=button;if(button->property("captureAction")=="desktop")desktopButton=button;}QVERIFY(windowButton&&desktopButton);
  QVERIFY(windowButton->property("captureBadge").toBool());QVERIFY(windowButton->property("captureUnavailable").toBool());QVERIFY(windowButton->toolTip().contains(reason));QVERIFY(windowButton->isEnabled());
  QVERIFY(!desktopButton->property("captureBadge").toBool());QVERIFY(!desktopButton->toolTip().contains("unavailable"));
  window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);window.show();window.findChild<QToolButton*>("desktopSupportDetailsToggle")->setChecked(true);
  auto* gap=window.findChild<QLabel*>("desktopSupportWindowGap");QVERIFY(gap);PUTGD_TRY_VERIFY(gap->isVisible());QCOMPARE(gap->text(),"Unavailable · "+reason);QVERIFY(window.findChild<QLabel*>("desktopSupportWindowGapLabel")->isVisible());QVERIFY(!window.findChild<QLabel*>("desktopSupportDesktopGap")->isVisible());
  // The desktop gains window capture (a compositor update, a new session): both go back to normal.
  details["capabilities"]=QJsonObject{{"source",QJsonObject{{"adapter","budgie-wayland"},{"capture",QJsonObject{{"region",QJsonObject{{"available",true}}},{"window",QJsonObject{{"available",true}}},{"desktop",QJsonObject{{"available",true}}}}}}}};
  fixture.session.refreshSnapshot();PUTGD_TRY_COMPARE(fixture.session.diagnostics(),details);PUTGD_TRY_VERIFY(!gap->isVisible());PUTGD_TRY_VERIFY(!windowButton->property("captureBadge").toBool());QVERIFY(!windowButton->toolTip().contains(reason));
 }
 void diagnosticConfigurationStaysInsideDesktopDetails(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(Settings::defaults(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);window.resize(1440,1100);window.show();auto* body=window.findChild<QWidget*>("desktopSupportDetails");auto* path=window.findChild<Ui::ElidedLabel*>("configPath");QVERIFY(path&&body->isAncestorOf(path));QVERIFY(path->isHidden()||!path->isVisible());window.findChild<QToolButton*>("desktopSupportDetailsToggle")->setChecked(true);QVERIFY(path->isVisible());QCOMPARE(path->text(),service.path());for(auto* key:body->findChildren<QLabel*>())if(key->wordWrap()&&!key->isHidden())PUTGD_TRY_VERIFY2(key->height()>=key->heightForWidth(key->width()),qPrintable(key->text()));auto* label=window.findChild<QLabel*>("configPathLabel");QVERIFY(label&&label->buddy()==path);const auto top=[&](QWidget* widget){return widget->mapTo(body,QPoint()).y();};PUTGD_TRY_VERIFY(qAbs(top(label)-top(path))<=2);PUTGD_TRY_VERIFY(label->mapTo(body,QPoint(label->width(),0)).x()+Ui::space(Ui::Space::l)<=path->mapTo(body,QPoint()).x());for(const auto& name:{"showConfigInFolder","openConfigEditor","reloadSettings"}){auto* button=window.findChild<QPushButton*>(name);QVERIFY(button->property("uiFlush").toBool());QVERIFY(top(button)>top(path));QVERIFY(button->mapTo(body,QPoint()).x()>=path->mapTo(body,QPoint()).x());}for(const auto& key:QStringList{"accepted","observed"})QVERIFY(window.findChild<QLabel*>(key+"Revision")->isHidden());QVERIFY(body->isAncestorOf(window.findChild<QPushButton*>("copyDesktopDetails")));QVERIFY(body->isAncestorOf(window.findChild<QPushButton*>("showConfigInFolder")));
  const auto accepted=service.revision();{QFile file(service.path());QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));file.write("{invalid-json");}QVERIFY(!service.reload());PUTGD_TRY_VERIFY(fixture.session.metadata()["observedRevision"].toString()!=accepted);PUTGD_TRY_VERIFY(!window.findChild<QLabel*>("observedRevision")->isHidden());QCOMPARE(window.findChild<QLabel*>("acceptedRevision")->accessibleName(),accepted);QCOMPARE(window.findChild<QLabel*>("observedRevision")->accessibleName(),fixture.session.metadata()["observedRevision"].toString());

 }
 void advancedOnlyDisclosesRetentionAndDebugHelp(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");auto* page=window.findChild<QTabWidget*>("settingsTabs")->widget(3);int helps=0;for(auto* button:page->findChildren<QToolButton*>())if(!button->property("settingHelpFor").toString().isEmpty()){++helps;QVERIFY(button->objectName()=="historyRetentionChoiceHelp");}QCOMPARE(helps,1);
  for(const auto& name:{"pendingArtifactBytesChoice","pendingImageBytes","previewCacheBytes","failedArtifactDays","logSummary","uncleanExit"}){auto* control=window.findChild<QWidget*>(name);QVERIFY(control);QVERIFY(!control->accessibleDescription().isEmpty());QVERIFY(!window.findChild<QToolButton*>(QString(name)+"Help"));}
  window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);window.show();auto* help=window.findChild<QToolButton*>("historyRetentionChoiceHelp");help->click();QVERIFY(window.findChild<QLabel*>("historyRetentionChoiceHelpText")->isVisible());help->click();QVERIFY(!window.findChild<QLabel*>("historyRetentionChoiceHelpText")->isVisible());
 }
 void diagnosticsPackageIsAnAlignedSecondaryAction(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1600,1400);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);window.show();putgd::test::settle(30);
  auto* package=window.findChild<QPushButton*>("createDiagnosticsPackage");auto* clear=window.findChild<QPushButton*>("clearLogs");QVERIFY(package&&clear);QCOMPARE(package->property("visualRole").toString(),QString("secondary"));QVERIFY(package->width()<package->parentWidget()->width());auto* summary=window.findChild<QLabel*>("logSummary");QVERIFY(summary);QCOMPARE(package->mapTo(&window,QPoint(package->width(),0)).x(),clear->mapTo(&window,QPoint(clear->width(),0)).x());QVERIFY(summary->mapTo(&window,QPoint()).y()>summary->parentWidget()->findChild<QLabel*>()->mapTo(&window,QPoint()).y());QVERIFY(package->icon().isNull());
 }

};
QTEST_MAIN(SettingsGui)
#include "test_settings_advanced.moc"
