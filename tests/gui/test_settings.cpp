#include "ui/MainWindow.h"
#include "core/BuildInfo.h"
#include "core/ReleaseLink.h"
#include "ui/VisualStyle.h"
#include "ui/RedesignTheme.h"
#include <QVBoxLayout>
#include <QAbstractItemView>
#include <QScrollBar>
#include "ui/SettingsWindow.h"
#include "ui/FileDialog.h"
#include <QFileDialog>
#include "ui/ShortcutEditor.h"
#include "ui/ViewState.h"
#include "fixtures/UiSessionFixture.h"
#include "app/DiagnosticsBundle.h"
#include "core/Log.h"
#include "core/ArtifactStore.h"
#include <QJsonDocument>
#include <QDialog>
#include <QDir>
#include "storage/s3/S3Provider.h"
#include "storage/proton/ProtonProvider.h"
#include "storage/b2/B2Provider.h"
#include <QSpinBox>
#include <QMimeData>
#include <QDropEvent>
#include <QUrl>
#include <QFile>
#include <QFileInfo>
#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTabWidget>
#include <QToolButton>
#include <QMessageBox>
#include <QInputDialog>
#include <QMenu>
#include <QTabBar>
#include <QWheelEvent>
#include <QDesktopServices>
#include <QScopeGuard>
#include <QAction>
using namespace putgd;using namespace putgd::test;
namespace {
// Three S3 destinations: development-s3 is the capture default, clipboard is an exception to synthetic-third.
QJsonObject threeDestinations(){
 auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();
 for(const auto& [id,label]:QList<QPair<QString,QString>>{{"synthetic-alternate","Synthetic alternate"},{"synthetic-third","Synthetic third"}}){auto profile=profiles.first().toObject();profile["id"]=id;profile["label"]=label;profile["credentials"]=QJsonObject{{"mode","keychain"},{"reference","putgd/"+id}};profiles.append(profile);}
 doc["profiles"]=profiles;doc["captureDefaults"]=QJsonObject{{"destination","upload"},{"profileId","development-s3"},{"publicationMode","default"}};
 auto actions=doc["actions"].toObject();for(const auto& name:Settings::actionNames()){auto action=actions[name].toObject();action["destination"]=name=="clipboard"?"upload":"default";action["profileId"]=name=="clipboard"?QJsonValue("synthetic-third"):QJsonValue(QJsonValue::Null);actions[name]=action;}doc["actions"]=actions;return doc;
}
QStringList profileIds(const QJsonObject& document){QStringList ids;for(const auto& p:document["profiles"].toArray())ids.append(p.toObject()["id"].toString());return ids;}
QStringList rowLabels(QListWidget* list){QStringList labels;for(int i=0;i<list->count();++i)labels.append(list->item(i)->text().section('\n',0,0));return labels;}
}
class SettingsGui:public QObject {
 Q_OBJECT
 QList<QUrl> openedUrls_;
public slots:
 void recordProtonUrl(const QUrl& url){openedUrls_.append(url);}
private slots:
 void reorderingDestinationsKeepsSelectionEditsAndRouting(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));
  auto routing=[&]{auto doc=service.document();doc.remove("profiles");return doc;};const auto routes=routing();
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* list=window.findChild<QListWidget*>("destinationList");QVERIFY(list);QCOMPARE(list->dragDropMode(),QAbstractItemView::InternalMove);
  auto* up=window.findChild<QAction*>("moveDestinationUp");auto* down=window.findChild<QAction*>("moveDestinationDown");QVERIFY(up&&down);
  QCOMPARE(up->shortcut(),QKeySequence(Qt::ALT|Qt::Key_Up));QCOMPARE(down->shortcut(),QKeySequence(Qt::ALT|Qt::Key_Down));
  QVERIFY(!up->isEnabled());QVERIFY(down->isEnabled());
  list->setCurrentRow(1);QVERIFY(up->isEnabled());
  // A queued label edit and an unstored credential input both stay with their destination.
  auto* label=window.findChild<QLineEdit*>("destinationLabel");auto* access=window.findChild<QLineEdit*>("credentialAccessKey");
  label->setText("Edited alternate");access->setText("SYNTHETICACCESSKEY");
  down->trigger();
  QCOMPARE(rowLabels(list),QStringList({"put.gd development destination","Synthetic third","Edited alternate"}));
  QCOMPARE(list->currentRow(),2);QCOMPARE(window.findChild<QLineEdit*>("destinationId")->text(),QString("synthetic-alternate"));
  QCOMPARE(label->text(),QString("Edited alternate"));QCOMPARE(access->text(),QString("SYNTHETICACCESSKEY"));QVERIFY(up->isEnabled());QVERIFY(!down->isEnabled());
  QTRY_COMPARE(profileIds(service.document()),QStringList({"development-s3","synthetic-third","synthetic-alternate"}));
  QTRY_COMPARE(service.document()["profiles"].toArray()[2].toObject()["label"].toString(),QString("Edited alternate"));
  QCOMPARE(routing(),routes);QCOMPARE(service.snapshot().actions["clipboard"].profileId,QString("synthetic-third"));
  // The Destination dropdowns follow the order and keep their selections by ID.
  for(const auto& name:{"profileRegion","profileClipboard","defaultCaptureDestination"}){
   auto* combo=window.findChild<QComboBox*>(name);QVERIFY2(combo,name);QStringList ids;for(int i=1;i<combo->count();++i)ids.append(combo->itemData(i).toString());
   QCOMPARE(ids,profileIds(service.document()));
  }
  QCOMPARE(window.findChild<QComboBox*>("profileClipboard")->currentData().toString(),QString("synthetic-third"));
  QCOMPARE(window.findChild<QComboBox*>("defaultCaptureDestination")->currentData().toString(),QString("development-s3"));
  QVERIFY(list->item(0)->text().contains("Default"));QVERIFY(!list->item(1)->text().contains("Default"));QVERIFY(!list->item(2)->text().contains("Default"));
  // Alt+Up on the focused list is the same move.
  list->setFocus();QTest::keyClick(list,Qt::Key_Up,Qt::AltModifier);
  QCOMPARE(rowLabels(list),QStringList({"put.gd development destination","Edited alternate","Synthetic third"}));QCOMPARE(list->currentRow(),1);QCOMPARE(access->text(),QString("SYNTHETICACCESSKEY"));
  QTRY_COMPARE(profileIds(service.document()),QStringList({"development-s3","synthetic-alternate","synthetic-third"}));
  // The UI-driving path selects by ID and moves, reporting the new order.
  QJsonObject result;QVERIFY(!window.settings()->moveSelectedDestination(-1,"synthetic-third",result));
  QCOMPARE(result["selected"].toString(),QString("synthetic-third"));QCOMPARE(result["order"].toArray(),QJsonArray({"development-s3","synthetic-third","synthetic-alternate"}));
  QTRY_COMPARE(profileIds(service.document()),QStringList({"development-s3","synthetic-third","synthetic-alternate"}));
  QCOMPARE(window.settings()->moveSelectedDestination(-1,"development-s3",result).code,QString("destination_move_invalid"));
  QCOMPARE(window.settings()->moveSelectedDestination(1,"missing",result).code,QString("profile_not_found"));
  QCOMPARE(routing(),routes);access->clear();
  // The saved order survives a restart.
  SettingsService restarted(dir.filePath("config.json"),[](auto,auto){return Error{};});QCOMPARE(profileIds(restarted.document()),QStringList({"development-s3","synthetic-third","synthetic-alternate"}));
  UiSessionFixture restartedFixture(restarted,secrets);MainWindow reopened(restartedFixture.session);reopened.navigate("destinations");
  QCOMPARE(rowLabels(reopened.findChild<QListWidget*>("destinationList")),QStringList({"put.gd development destination","Synthetic third","Edited alternate"}));
  QVERIFY(!validateUiState({{"schemaVersion",1},{"settings",window.settings()->saveViewState()}}));
 }
 void longDestinationLabelKeepsItsStateLine(){
  // #134: a label that would wrap is elided to one line; Default · state stays in the row.
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=threeDestinations();auto profiles=doc["profiles"].toArray();
  auto first=profiles[0].toObject();first["label"]="put.gd development destination with a synthetic name long enough to wrap twice";profiles[0]=first;doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1100,760);window.navigate("destinations");window.show();
  auto* list=window.findChild<QListWidget*>("destinationList");QVERIFY(list);QVERIFY(!list->wordWrap());
  const auto lines=window.settings()->destinationRowLines(0);QCOMPARE(lines.size(),2);
  QVERIFY2(lines[0].endsWith(QChar(0x2026)),qPrintable(lines[0]));QVERIFY(first["label"].toString().startsWith(lines[0].chopped(1)));
  QVERIFY2(lines[1].startsWith("Default"),qPrintable(lines[1]));QVERIFY(lines[1].contains("Ready to test")||lines[1].endsWith(QChar(0x2026)));
  // Both lines fit the row: one line each, inside its fixed height.
  const QFontMetrics metrics(list->font());const auto rect=list->visualItemRect(list->item(0));
  for(const auto& line:lines)QVERIFY2(metrics.horizontalAdvance(line)<rect.width(),qPrintable(line));
  QVERIFY(2*metrics.lineSpacing()<rect.height()-20);
  QCOMPARE(window.settings()->destinationRowLines(1),QStringList({"Synthetic alternate","Ready to test"}));
  QCOMPARE(list->item(0)->toolTip().section('\n',0,0),first["label"].toString());
  // Pressing another row selects it without rebuilding the rows, so the view keeps the
  // pressed row and a drag can start from it (found natively on GNOME Wayland, #135).
  auto* third=list->item(2);third->setData(Qt::UserRole+42,"same row object");QTest::mousePress(list->viewport(),Qt::LeftButton,{},list->visualItemRect(third).center());
  QCOMPARE(list->currentRow(),2);QCOMPARE(list->item(2)->data(Qt::UserRole+42).toString(),QString("same row object"));QCOMPARE(window.findChild<QLineEdit*>("destinationId")->text(),QString("synthetic-third"));
  QTest::mouseRelease(list->viewport(),Qt::LeftButton,{},list->visualItemRect(third).center());
  QCOMPARE(list->item(0)->text().section('\n',0,0),first["label"].toString());
  // #133: the drag image is a slim label beside the pointer, never over the drop line's
  // start cap, wherever the row is grabbed.
  QPoint hotSpot;const auto image=window.settings()->destinationDragImage(1,&hotSpot);QVERIFY(!image.isNull());
  const QSize logical=(QSizeF(image.size())/image.devicePixelRatio()).toSize();
  QVERIFY2(logical.height()<rect.height()/2,qPrintable(QString::number(logical.height())));QVERIFY(hotSpot.x()<0&&hotSpot.y()<0);
  const auto row=list->visualItemRect(list->item(1));
  for(int y=row.top();y<=row.bottom();y+=4)for(int x=row.left();x<=row.right();x+=8){
   const QPoint pointer(x,y);const QRect drawn(pointer-hotSpot,logical);
   const int gap=y<row.center().y()?row.top():row.bottom()+1;
   QVERIFY2(!drawn.contains(QPoint(row.left()+5,gap)),qPrintable(QString("%1,%2").arg(x).arg(y)));
   QVERIFY(drawn.top()>pointer.y());
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
  // #238: the whole card is on Advanced, directly above Config file; General has none of it.
  auto* tabs=window.findChild<QTabWidget*>("settingsTabs");
  for(const auto* name:{"debugLogging","logSummary","clearLogs","createDiagnosticsPackage","uncleanExit"}){auto* control=window.findChild<QWidget*>(name);QVERIFY2(control&&tabs->widget(3)->isAncestorOf(control),name);QVERIFY2(!tabs->widget(0)->isAncestorOf(control),name);}
  auto cardOf=[](QWidget* w){while(w&&w->property("visualRole")!="card")w=w->parentWidget();return w;};
  auto* diagnosticsCard=cardOf(window.findChild<QWidget*>("debugLogging"));auto* configCard=cardOf(window.findChild<QPushButton*>("reloadSettings"));QVERIFY(diagnosticsCard&&configCard&&diagnosticsCard!=configCard);
  int diagnosticsAt=-1,configAt=-1;for(auto* layout:diagnosticsCard->parentWidget()->findChildren<QBoxLayout*>()){const int a=layout->indexOf(diagnosticsCard),b=layout->indexOf(configCard);if(a>=0&&b>=0){diagnosticsAt=a;configAt=b;}}
  QCOMPARE(configAt,diagnosticsAt+1);tabs->setCurrentIndex(3);
  QTRY_VERIFY2(summary->text().contains("files")||summary->text().contains("1 file"),qPrintable(summary->text()));QVERIFY(summary->text().contains("oldest record"));
  QTRY_VERIFY(window.findChild<QLabel*>("uncleanExit")->text().contains("0.1.1-synthetic"));QVERIFY(window.findChild<QLabel*>("uncleanExit")->text().contains("stopped unexpectedly"));
  auto* degraded=window.findChild<QLabel*>("loggingDegraded");QVERIFY(degraded->isHidden());
  degrade=true;fixture.session.refreshSnapshot();QTRY_VERIFY(degraded->isVisibleTo(settings));QVERIFY(degraded->text().contains("disk is full"));QVERIFY(degraded->text().contains("12 records"));degrade=false;
  // Debug logging is a setting: it saves like the others and keeps packageFolder beside it.
  auto* debug=window.findChild<QCheckBox*>("debugLogging");QVERIFY(debug);QVERIFY(!debug->isChecked());debug->setChecked(true);
  QTRY_VERIFY(service.snapshot().debugLogging);QVERIFY(window.findChild<QLabel*>("debugLoggingStatus")->text().startsWith("On."));
  // Clear logs through the UI command (the button asks first).
  auto command=[&](const QJsonObject& params)->std::pair<Error,QJsonObject>{bool done=false;Error error{"test_timeout","No answer",{}};QJsonObject result;settings->diagnosticsCommand(params,[&](Error e,QJsonObject r){error=e;result=r;done=true;});if(!QTest::qWaitFor([&]{return done;},15000))qWarning("diagnostics command timed out");return {error,result};};
  auto [cleared,clearState]=command({{"action","clearLogs"}});QVERIFY2(!cleared,qPrintable(cleared.message));QVERIFY(clearState["result"].toObject()["removedFiles"].toInt()>=1);
  // The dialog previews the package and writes nothing until Save.
  window.findChild<QPushButton*>("createDiagnosticsPackage")->click();auto* dialog=settings->findChild<QDialog*>("diagnosticsPackageDialog");QVERIFY(dialog);QTRY_VERIFY(dialog->isVisible());
  auto* contents=dialog->findChild<QLabel*>("packageContents");QTRY_VERIFY2(contents->text().contains("logs/putgd.jsonl"),qPrintable(contents->text()));QVERIFY(contents->text().contains("manifest.json"));
  QVERIFY(dialog->findChild<QWidget*>("diagnosticsPackageExtras"));
  QCOMPARE(QDir::fromNativeSeparators(dialog->findChild<QLabel*>("packageFolder")->text()),QDir(dir.filePath("archive")).filePath("diagnostics"));
  QVERIFY(dialog->findChild<QLabel*>("packageRedaction")->text().contains("replaced by hashes"));
  // Wrapped labels get their full height once the preview has filled them (no clipped lines).
  for(const auto* name:{"packageScope","packageContents","packageOperations","packageRedaction"}){auto* label=dialog->findChild<QLabel*>(name);QTRY_VERIFY2(label->height()>=label->heightForWidth(label->width()),name);}
  dialog->findChild<QCheckBox*>("includeFileNames")->setChecked(true);QTRY_VERIFY(dialog->findChild<QLabel*>("packageRedaction")->text().contains("kept as they are"));
  QTRY_COMPARE(settings->diagnosticsState()["package"].toObject()["preview"].toObject()["redaction"].toString(),QString("names"));
  QVERIFY(!QFileInfo::exists(dir.filePath("archive/diagnostics")));
  // A chosen folder is remembered in preferences.diagnostics.packageFolder.
  QTemporaryDir out;auto [moved,movedState]=command({{"action","setPackage"},{"folder",out.path()}});QVERIFY2(!moved,qPrintable(moved.message));
  QCOMPARE(movedState["package"].toObject()["preview"].toObject()["folder"].toString(),out.path());
  QTRY_COMPARE(service.document()["preferences"].toObject()["diagnostics"].toObject()["packageFolder"].toString(),out.path());QVERIFY(service.snapshot().debugLogging);
  QCOMPARE(command({{"action","setPackage"},{"folder",dir.filePath("missing")}}).first.code,QString("diagnostics_output_unavailable"));
  dialog->findChild<QPushButton*>("savePackage")->click();auto* result=dialog->findChild<QLabel*>("packageResult");QTRY_VERIFY(result->isVisibleTo(dialog));QVERIFY2(result->text().startsWith("Saved putgd-diagnostics-"),qPrintable(result->text()));
  QCOMPARE(notified.size(),1);const auto path=notified[0].toObject()["path"].toString();QCOMPARE(QFileInfo(path).absolutePath(),QFileInfo(out.path()).absoluteFilePath());QCOMPARE(notified[0].toObject()["redaction"].toString(),QString("names"));
  auto* show=dialog->findChild<QPushButton*>("showPackageInFolder");QVERIFY(show->isVisibleTo(dialog));show->click();QTRY_COMPARE(revealed,QStringList{path});
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
  test::Source source;OperationService operations(dir.filePath("data"),service,providers,source);QTRY_VERIFY(operations.ready());HistoryService history(operations);
  UiSessionFixture fixture(service,operations,history);
  DiagnosticsBundles bundles(operations,dir.filePath("state"));fixture.server.setDiagnosticsBundles(&bundles);
  Error testResult{"s3_authentication_failed","Synthetic authentication failure",{}};bool holdTest=false;std::function<void(Error)> heldTest;int tests=0;
  bundles.setDestinationTest([&](const QString& profileId,std::function<void(Error)> done){QCOMPARE(profileId,QString("test"));++tests;if(holdTest)heldTest=done;else done(testResult);});
  MainWindow window(fixture.session);window.navigate("settings");window.show();fixture.session.refreshSnapshot();auto* settings=window.settings();
  auto command=[&](const QJsonObject& params)->std::pair<Error,QJsonObject>{bool done=false;Error error{"test_timeout","No answer",{}};QJsonObject result;settings->diagnosticsCommand(params,[&](Error e,QJsonObject r){error=e;result=r;done=true;});if(!QTest::qWaitFor([&]{return done;},15000))qWarning("diagnostics command timed out");return {error,result};};
  auto package=[&]{return settings->diagnosticsState()["package"].toObject();};
  // A destination that fails Test connection: no checkbox, the reason instead.
  auto [opened,openState]=command({{"action","openPackage"}});QVERIFY2(!opened,qPrintable(opened.message));
  auto* dialog=settings->findChild<QDialog*>("diagnosticsPackageDialog");auto* upload=dialog->findChild<QCheckBox*>("uploadPackage");auto* note=dialog->findChild<QLabel*>("packageUploadNote");
  QVERIFY(upload&&note);QVERIFY(dialog->findChild<QWidget*>("diagnosticsPackageExtras")->isAncestorOf(upload));
  QTRY_VERIFY(!package()["uploadTarget"].toObject()["checking"].toBool());
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
  command({{"action","closePackage"}});QTRY_VERIFY(heldTest);std::exchange(heldTest,{})({});holdTest=false;
  // A working destination: checked by default, named, with its warning; the upload links and copies.
  const int uploadsBefore=provider->uploads.load();
  command({{"action","openPackage"}});QTRY_VERIFY(package()["uploadTarget"].toObject()["available"].toBool());
  QVERIFY(upload->isVisibleTo(dialog));QVERIFY(upload->isChecked());QVERIFY2(upload->text().contains("Synthetic opaque provider"),qPrintable(upload->text()));
  QVERIFY(note->text().contains("anyone with the link"));QCOMPARE(dialog->findChild<QPushButton*>("savePackage")->text(),QString("Save and upload"));QVERIFY(package()["upload"].toBool());
  auto [started,startedState]=command({{"action","savePackage"}});QVERIFY2(!started,qPrintable(started.message));
  QTRY_VERIFY_WITH_TIMEOUT(package().contains("saved"),20000);
  const auto saved=package()["saved"].toObject();QVERIFY2(saved["link"].toString().startsWith("https://"),QJsonDocument(saved).toJson().constData());
  QVERIFY(saved["linkCopied"].toBool());QCOMPARE(source.copied.text,saved["link"].toString());QCOMPARE(provider->uploads.load(),uploadsBefore+1);QVERIFY(QFileInfo(saved["path"].toString()).isFile());
  auto* result=dialog->findChild<QLabel*>("packageResult");QVERIFY2(result->text().contains("Uploaded to Synthetic opaque provider. Link copied: https://"),qPrintable(result->text()));
  command({{"action","closePackage"}});
  // An agent can leave it unticked: the package stays local.
  command({{"action","openPackage"}});QTRY_VERIFY(package()["uploadTarget"].toObject()["available"].toBool());
  auto [cleared,clearedState]=command({{"action","setPackage"},{"upload",false}});QVERIFY(!cleared);QVERIFY(!clearedState["package"].toObject()["upload"].toBool());
  auto [plain,plainState]=command({{"action","savePackage"}});QVERIFY(!plain);QVERIFY(!plainState["package"].toObject()["saved"].toObject().contains("link"));QCOMPARE(provider->uploads.load(),uploadsBefore+1);
  command({{"action","closePackage"}});
  operations.shutdown();
 }
 // #149: what Auto resolves to, from settings.get effectiveLimits.
 void fileDialogsOpenTheSharedChooser(){
  // #259: Choose folder…, Browse… and Edit an image file… open the shared chooser,
  // and ui.fileDialog drives them. The fallback is forced: no portal is asked.
  qputenv("PUTGD_FILE_DIALOG","fallback");auto restore=qScopeGuard([]{qunsetenv("PUTGD_FILE_DIALOG");});
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=Settings::developmentTemplate();auto archive=doc["archive"].toObject();archive["root"]=dir.filePath("archive");doc["archive"]=archive;QVERIFY(service.save(doc,{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);
  MainWindow window(fixture.session);window.show();fixture.session.refreshSnapshot();
  fixture.server.setUiCommand([&](const QString& name,const QJsonObject& params,JsonReply reply){QCOMPARE(name,QString("fileDialog"));
   if(params["dialog"]=="editImage"){if(params["action"]=="open")window.chooseImageToEdit();else if(params["action"]=="cancel")window.editImageDialog().cancel();reply({},window.editImageDialog().state());return;}
   window.settings()->fileDialogCommand(params["dialog"].toString(),params["action"].toString(),params["field"].toString(),std::move(reply));});
  auto call=[&](const QJsonObject& params)->std::pair<Error,QJsonObject>{bool done=false;Error error{"test_timeout","No answer",{}};QJsonObject result;
   fixture.server.dispatch({{"schemaVersion",1},{"requestId","ui"},{"session",fixture.server.socketPath()},{"method","ui.fileDialog"},{"params",params}},[&](Error e,QJsonObject r){error=e;result=r;done=true;});
   if(!QTest::qWaitFor([&]{return done;},5000))qWarning("ui.fileDialog timed out");return {error,result};};
  QCOMPARE(call({{"dialog","editImage"},{"action","open"},{"field","root"}}).first.code,QString("invalid_request"));
  QCOMPARE(call({{"dialog","outputFolder"},{"action","open"},{"field","music"}}).first.code,QString("invalid_request"));
  QCOMPARE(call({{"dialog","shell"},{"action","open"}}).first.code,QString("invalid_request"));
  QCOMPARE(call({{"dialog","diagnosticsFolder"},{"action","open"}}).first.code,QString("diagnostics_dialog_closed"));
  // Browse… for Videos: the chosen folder lands in its field and autosaves.
  QTemporaryDir videos;QVERIFY(videos.isValid());
  auto [error,state]=call({{"dialog","outputFolder"},{"action","open"},{"field","videos"}});QVERIFY2(!error,qPrintable(error.message));
  QVERIFY(state["open"].toBool());QCOMPARE(state["backend"].toString(),QString("fallback"));QCOMPARE(state["mode"].toString(),QString("folder"));QCOMPARE(state["title"].toString(),QString("Choose output folder"));
  QCOMPARE(call({{"dialog","outputFolder"},{"action","open"},{"field","images"}}).first.code,QString("file_dialog_open"));
  QFileDialog* chooser=nullptr;QTRY_VERIFY((chooser=window.findChild<QFileDialog*>("putgdFileDialog"))&&chooser->isVisible());
  QCOMPARE(chooser->fileMode(),QFileDialog::Directory);
  chooser->setDirectory(videos.path());chooser->selectFile(videos.path());QMetaObject::invokeMethod(chooser,"accept");
  QTRY_COMPARE(window.findChild<QLineEdit*>("folderVideos")->text(),videos.path());
  QTRY_COMPARE(service.snapshot().archive.folders.value("videos"),videos.path());
  QCOMPARE(call({{"dialog","outputFolder"},{"action","inspect"}}).second["last"].toObject()["status"].toString(),QString("chosen"));
  // Edit an image file…: opened, then cancelled through the command.
  auto opened=call({{"dialog","editImage"},{"action","open"}});QVERIFY(opened.second["open"].toBool());QCOMPARE(opened.second["mode"].toString(),QString("open"));
  QTRY_VERIFY((chooser=window.findChild<QFileDialog*>("putgdFileDialog"))&&chooser->isVisible());QCOMPARE(chooser->nameFilters(),QStringList{"Images (*.png *.jpg *.jpeg)"});
  auto cancelled=call({{"dialog","editImage"},{"action","cancel"}});QVERIFY(!cancelled.second["open"].toBool());QCOMPARE(cancelled.second["last"].toObject()["status"].toString(),QString("cancelled"));
 }
 void autoLimitsShowWhatTheyResolveTo(){
  constexpr qint64 gib=1LL<<30;ArtifactStore::setDiskForTesting(60*gib,100*gib);auto resetDisk=qScopeGuard([]{ArtifactStore::setDiskForTesting(-1,-1);});
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();
  auto* file=window.findChild<QLabel*>("maxFileBytesResolved");auto* batch=window.findChild<QLabel*>("maxBatchBytesResolved");auto* waiting=window.findChild<QLabel*>("pendingArtifactBytesResolved");
  QVERIFY(file&&batch&&waiting);
  QTRY_COMPARE(file->text(),QString("· no limit (keeps 4 GiB free)"));QCOMPARE(batch->text(),file->text());QCOMPARE(waiting->text(),file->text());
  QVERIFY(file->toolTip().contains("keeps 4 GiB free"));
  // A typed size or No limit is its own answer.
  auto* edit=window.findChild<QLineEdit*>("maxFileBytes");edit->setText("2");QVERIFY(file->text().isEmpty());QVERIFY(!file->isVisibleTo(window.settings()));
  edit->setText("auto");QCOMPARE(file->text(),QString("· no limit (keeps 4 GiB free)"));
 }
 void conflictingReorderRestoresThePreviousOrder(){
  QTemporaryDir dir;const auto path=dir.filePath("config.json");SettingsService service(path,[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* list=window.findChild<QListWidget*>("destinationList");list->setCurrentRow(0);
  // Another writer changes the file just before the reorder is saved.
  QByteArray external;fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject&){if(method!="settings.save"||!external.isEmpty())return;auto doc=service.document();auto preferences=doc["preferences"].toObject();preferences["notifications"]=false;doc["preferences"]=preferences;external=QJsonDocument(doc).toJson();QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write(external);};
  window.findChild<QAction*>("moveDestinationDown")->trigger();
  QCOMPARE(rowLabels(list),QStringList({"Synthetic alternate","put.gd development destination","Synthetic third"}));
  QLabel* status=window.findChild<QLabel*>("settingsStatus");QVERIFY(status);
  QTRY_VERIFY2(status->text().contains("config_conflict"),qPrintable(status->text()));QVERIFY(status->text().contains("previous destination order is restored"));
  QCOMPARE(rowLabels(list),QStringList({"put.gd development destination","Synthetic alternate","Synthetic third"}));QCOMPARE(list->currentRow(),0);
  QCOMPARE(window.findChild<QLineEdit*>("destinationId")->text(),QString("development-s3"));
  QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),external);
  QVERIFY(window.settings()->autosaveText().contains("changed elsewhere"));
  // Further moves wait for a reload instead of saving over the other writer.
  QJsonObject result;QCOMPARE(window.settings()->moveSelectedDestination(1,{},result).code,QString("config_conflict"));
  window.findChild<QPushButton*>("reloadSettings")->click();QTRY_VERIFY(!window.settings()->autosaveText().contains("changed elsewhere"));
  QVERIFY(!window.settings()->moveSelectedDestination(1,"development-s3",result));
  QTRY_COMPARE(profileIds(service.document()),QStringList({"synthetic-alternate","development-s3","synthetic-third"}));QVERIFY(!service.snapshot().notifications);
 }
 void captureDefaultAndExplicitExceptionPersistAcrossReopen(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(1);window.show();
  QVERIFY(!window.findChild<QComboBox*>("defaultCaptureOutput"));QVERIFY(!window.findChild<QComboBox*>("destinationClipboard"));auto* destination=window.findChild<QComboBox*>("defaultCaptureDestination");QVERIFY(destination);const auto profileId=destination->currentData().toString();QVERIFY(!profileId.isEmpty());auto* local=window.findChild<QCheckBox*>("keepUploadCopies");QVERIFY(local);local->setChecked(true);QTRY_VERIFY(service.snapshot().archive.keepUploads);QCOMPARE(service.snapshot().actions["region"].profileId,profileId);
  auto* exceptions=window.findChild<QToolButton*>("captureExceptionsToggle");QVERIFY(exceptions);QVERIFY(!exceptions->isChecked());exceptions->click();auto* inherit=window.findChild<QCheckBox*>("useDefaultClipboard");QVERIFY(inherit);inherit->setChecked(false);auto* clipboard=window.findChild<QComboBox*>("profileClipboard");clipboard->setCurrentIndex(clipboard->findData(QString{}));
  QTRY_VERIFY(service.document().contains("captureDefaults"));QTRY_COMPARE(service.snapshot().actions["clipboard"].destination,QString("local"));
  destination->setCurrentIndex(destination->findData(QString{}));QTRY_COMPARE(service.snapshot().actions["region"].destination,QString("local"));QVERIFY(service.snapshot().archive.keepUploads);destination->setCurrentIndex(destination->findData(profileId));QTRY_COMPARE(service.snapshot().actions["region"].destination,QString("upload"));QCOMPARE(service.snapshot().actions["clipboard"].destination,QString("local"));
  local->setChecked(false);QTRY_VERIFY(!service.snapshot().archive.keepUploads);QCOMPARE(service.snapshot().actions["region"].profileId,profileId);QCOMPARE(service.snapshot().actions["clipboard"].destination,QString("local"));QTRY_VERIFY(window.findChild<QLabel*>("captureStatusClipboard")->text().contains("will fail"));
  MainWindow reopened(fixture.session);QVERIFY(reopened.findChild<QCheckBox*>("useDefaultRegion")->isChecked());QVERIFY(!reopened.findChild<QCheckBox*>("useDefaultClipboard")->isChecked());QVERIFY(!validateUiState({{"schemaVersion",1},{"settings",window.settings()->saveViewState()}}));
 }
 void inheritedActionsShowTheDefaultOutputDisabled(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=Settings::developmentTemplate();auto proton=ProtonProvider::defaults();proton["shareByDefault"]=false;
  auto profiles=doc["profiles"].toArray();profiles.append(QJsonObject{{"id","synthetic-proton"},{"label","Synthetic Drive"},{"provider","proton-drive"},{"settings",proton},{"credentials",QJsonObject{{"mode","keychain"},{"reference","putgd/synthetic-proton"}}}});doc["profiles"]=profiles;
  doc["captureDefaults"]=QJsonObject{{"destination","upload"},{"profileId","development-s3"},{"publicationMode","default"}};auto actions=doc["actions"].toObject();for(const auto& name:Settings::actionNames()){auto action=actions[name].toObject();action["destination"]="default";action["profileId"]=QJsonValue::Null;actions[name]=action;}doc["actions"]=actions;QVERIFY(service.save(doc,{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(1);window.show();
  auto* exceptions=window.findChild<QToolButton*>("captureExceptionsToggle");QVERIFY(!exceptions->isChecked());exceptions->click();
  const QHash<QString,QString> labels{{"region","Region capture"},{"window","Window capture"},{"desktop","Desktop capture"},{"clipboard","Clipboard upload"},{"edit","Region capture & edit"}};
  auto title=[](const QString& name){return name.left(1).toUpper()+name.mid(1);};
  auto routing=[&]{const auto state=window.settings()->saveViewState();[&]{QVERIFY(!validateUiState({{"schemaVersion",1},{"settings",state}}));}();return state["captureRouting"].toObject();};
  auto expectInherited=[&](const QString& profileId,const QString& destination,const QString& sharing,const QString& effective,const QString& status){
   const auto routes=routing();
   for(const auto& name:Settings::actionNames()){
    auto* profile=window.findChild<QComboBox*>("profile"+title(name));auto* publication=window.findChild<QComboBox*>("publication"+title(name));auto* line=window.findChild<QLabel*>("captureStatus"+title(name));
    QVERIFY2(window.findChild<QCheckBox*>("useDefault"+title(name))->isChecked(),qPrintable(name));QVERIFY2(window.findChild<QWidget*>("captureEditor"+title(name))->isVisible(),qPrintable(name));QVERIFY(line->isVisible());QVERIFY2(line->text().contains(status),qPrintable(line->text()));
    QVERIFY(!profile->isEnabled());QVERIFY(!publication->isEnabled());QCOMPARE(profile->currentData().toString(),profileId);QCOMPARE(profile->currentText(),destination);QCOMPARE(publication->currentText(),sharing);
    QCOMPARE(profile->accessibleName(),labels[name]+" destination: inherited from default, "+destination);QCOMPARE(publication->accessibleName(),labels[name]+" sharing: inherited from default, "+sharing);
    const auto route=routes[name].toObject();QVERIFY(route["inherited"].toBool());QVERIFY(!route["destinationEnabled"].toBool());QVERIFY(!route["sharingEnabled"].toBool());
    QCOMPARE(route["profileId"].toString(),profileId);QCOMPARE(route["destination"].toString(),destination);QCOMPARE(route["sharing"].toString(),sharing);QCOMPARE(route["effectivePublicationMode"].toString(),effective);QCOMPARE(route["status"].toString(),line->text());
   }
  };
  expectInherited("development-s3","put.gd development destination","Share and copy a link (destination default)","share","Uploads to the selected destination");
  QCOMPARE(routing()["default"].toObject()["publicationMode"].toString(),QString("default"));QVERIFY(routing()["default"].toObject()["sharingEnabled"].toBool());
  // Editing the default output updates every inheriting card at once.
  auto* destination=window.findChild<QComboBox*>("defaultCaptureDestination");auto* sharing=window.findChild<QComboBox*>("defaultCaptureSharing");
  destination->setCurrentIndex(destination->findData("synthetic-proton"));expectInherited("synthetic-proton","Synthetic Drive","Keep private, copy nothing (destination default)","private","Uploads to the selected destination");
  sharing->setCurrentIndex(sharing->findData("share"));expectInherited("synthetic-proton","Synthetic Drive","Share and copy a link","share","Uploads to the selected destination");
  destination->setCurrentIndex(destination->findData(QString{}));expectInherited(QString{},"No destination (uploads off)","Share and copy a link","","will fail");
  destination->setCurrentIndex(destination->findData("development-s3"));sharing->setCurrentIndex(sharing->findData("default"));expectInherited("development-s3","put.gd development destination","Share and copy a link (destination default)","share","Uploads to the selected destination");
  QTRY_COMPARE(service.document()["captureDefaults"].toObject()["profileId"].toString(),QString("development-s3"));QTRY_COMPARE(service.document()["captureDefaults"].toObject()["publicationMode"].toString(),QString("default"));
  // Unchecking starts from the inherited values and saves an explicit exception.
  window.findChild<QCheckBox*>("useDefaultRegion")->setChecked(false);auto* region=window.findChild<QComboBox*>("profileRegion");auto* regionSharing=window.findChild<QComboBox*>("publicationRegion");
  QVERIFY(region->isEnabled());QVERIFY(regionSharing->isEnabled());QCOMPARE(region->currentData().toString(),QString("development-s3"));QCOMPARE(regionSharing->currentData().toString(),QString("default"));QCOMPARE(regionSharing->currentText(),QString("Use destination default"));QCOMPARE(region->accessibleName(),QString("Region capture destination"));
  const auto route=routing()["region"].toObject();QVERIFY(!route["inherited"].toBool());QVERIFY(route["destinationEnabled"].toBool());QVERIFY(route["sharingEnabled"].toBool());QCOMPARE(route["effectivePublicationMode"].toString(),QString("share"));
  QTRY_COMPARE(service.document()["actions"].toObject()["region"].toObject()["destination"].toString(),QString("upload"));QCOMPARE(service.document()["actions"].toObject()["region"].toObject()["profileId"].toString(),QString("development-s3"));
  for(const auto& name:Settings::actionNames())if(name!="region")QCOMPARE(service.document()["actions"].toObject()[name].toObject()["destination"].toString(),QString("default"));
  MainWindow reopened(fixture.session);QVERIFY(!reopened.findChild<QCheckBox*>("useDefaultRegion")->isChecked());QVERIFY(reopened.findChild<QComboBox*>("profileRegion")->isEnabled());QVERIFY(reopened.findChild<QCheckBox*>("useDefaultEdit")->isChecked());QVERIFY(!reopened.findChild<QComboBox*>("profileEdit")->isEnabled());QCOMPARE(reopened.findChild<QComboBox*>("publicationEdit")->currentText(),QString("Share and copy a link (destination default)"));
  // The readable projection is strictly typed.
  auto state=window.settings()->saveViewState();auto routes=state["captureRouting"].toObject();auto edit=routes["edit"].toObject();edit["destinationEnabled"]="false";routes["edit"]=edit;state["captureRouting"]=routes;QVERIFY(validateUiState({{"schemaVersion",1},{"settings",state}}));
  edit["destinationEnabled"]=false;edit["draft"]="unexpected";routes["edit"]=edit;state["captureRouting"]=routes;QVERIFY(validateUiState({{"schemaVersion",1},{"settings",state}}));
 }
 void firstLaunchLocalCopiesNeedNoDestination(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);auto* destination=window.findChild<QComboBox*>("defaultCaptureDestination");destination->setCurrentIndex(destination->findData(QString{}));window.findChild<QCheckBox*>("keepUploadCopies")->setChecked(true);QTRY_VERIFY(!service.revision().isEmpty());QTRY_VERIFY(service.snapshot().archive.keepUploads);QVERIFY(service.document()["profiles"].toArray().isEmpty());QCOMPARE(service.snapshot().actions["region"].destination,QString("local"));QTRY_VERIFY(window.findChild<QLabel*>("autosaveStatus")->text().isEmpty());
  MainWindow reopened(fixture.session);QVERIFY(reopened.findChild<QCheckBox*>("keepUploadCopies")->isChecked());QVERIFY(reopened.findChild<QComboBox*>("defaultCaptureDestination")->currentData().toString().isEmpty());QVERIFY(reopened.findChild<QLabel*>("autosaveStatus")->text().isEmpty());
 }
 void unchangedFieldsDoNotSaveOrClaimChangesWereSaved(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));const auto original=service.document();const auto revision=service.revision();CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();auto* status=window.findChild<QLabel*>("autosaveStatus");auto* field=window.findChild<QLineEdit*>("maxFiles");auto* tabs=window.findChild<QTabWidget*>("settingsTabs");tabs->setCurrentIndex(3);
  fixture.client.failOnce["settings.save"]={"unexpected_save","Unchanged input must not dispatch settings.save",{}};
  field->setFocus();QTest::mouseClick(field,Qt::LeftButton);QTest::keyClick(field,Qt::Key_Tab);QVERIFY(QMetaObject::invokeMethod(field,"editingFinished"));QTest::qWait(650);QVERIFY(fixture.client.failOnce.contains("settings.save"));QVERIFY(status->text().isEmpty());QCOMPARE(service.document(),original);QCOMPARE(service.revision(),revision);
  field->setFocus();field->setText("101");field->setText("100");QTest::keyClick(field,Qt::Key_Tab);QTest::qWait(650);QVERIFY(fixture.client.failOnce.contains("settings.save"));QVERIFY(status->text().isEmpty());QCOMPARE(service.document(),original);
  fixture.client.failOnce.clear();field->setText("250");QTRY_COMPARE(service.snapshot().limits.maxFiles,250);QTRY_VERIFY(status->text().isEmpty());
  fixture.client.failOnce["settings.save"]={"unexpected_save","An unchanged accepted value must not save twice",{}};field->setFocus();QTest::keyClick(field,Qt::Key_Tab);QTest::qWait(650);QVERIFY(fixture.client.failOnce.contains("settings.save"));QVERIFY(status->text().isEmpty());
  MainWindow reopened(fixture.session);QVERIFY(reopened.findChild<QLabel*>("autosaveStatus")->text().isEmpty());fixture.session.invalidate();QTest::qWait(650);QVERIFY(reopened.findChild<QLabel*>("autosaveStatus")->text().isEmpty());
 }
 void revertingWhileAutosaveReplyIsPendingPersistsTheVisibleValue_data(){
  QTest::addColumn<int>("rootMode");QTest::newRow("leaf")<<0;QTest::newRow("root-and-leaf")<<1;QTest::newRow("second-in-flight-save")<<2;
 }
 void revertingWhileAutosaveReplyIsPendingPersistsTheVisibleValue(){
  QFETCH(int,rootMode);QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);auto* local=window.findChild<QCheckBox*>("keepUploadCopies");QVERIFY(local->isChecked());fixture.client.replyDelayMs["settings.save"]=700;
  if(rootMode){auto* inherit=window.findChild<QCheckBox*>("useDefaultRegion");inherit->setChecked(!inherit->isChecked());if(rootMode==2){QTRY_VERIFY(service.document().contains("captureDefaults"));QVERIFY(window.settings()->saveViewState().contains("stateError"));}}
  local->setChecked(false);QTRY_VERIFY(!service.snapshot().archive.keepUploads);QVERIFY(window.settings()->saveViewState().contains("stateError"));local->setChecked(true);
  QTRY_VERIFY_WITH_TIMEOUT(service.snapshot().archive.keepUploads,4000);QTRY_VERIFY(!window.settings()->saveViewState().contains("stateError"));QTRY_VERIFY(!window.settings()->saveViewState()["dirty"].toBool());MainWindow reopened(fixture.session);QVERIFY(reopened.findChild<QCheckBox*>("keepUploadCopies")->isChecked());
 }
 void newerEqualValuedParentSurvivesSubmittedChildOverride(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return provider.validateSettings(value);});QVERIFY(service.save(Settings::developmentTemplate(),{}));UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);auto* region=window.findChild<QLineEdit*>("s3Region");auto* bucket=window.findChild<QLineEdit*>("s3Bucket");const auto original=bucket->text();fixture.client.replyDelayMs["settings.save"]=700;
  region->setText("us-west-2");bucket->setText("synthetic-new-bucket");QTRY_COMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["bucket"].toString(),QString("synthetic-new-bucket"));QVERIFY(window.settings()->saveViewState().contains("stateError"));bucket->setText(original);region->setText("us-east-2");region->setText("us-west-2");
  QTRY_COMPARE_WITH_TIMEOUT(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["bucket"].toString(),original,4000);QTRY_VERIFY(!window.settings()->saveViewState().contains("stateError"));QTRY_VERIFY(!window.settings()->saveViewState()["dirty"].toBool());QCOMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["region"].toString(),QString("us-west-2"));MainWindow reopened(fixture.session);QCOMPARE(reopened.findChild<QLineEdit*>("s3Bucket")->text(),original);
 }
 void autosaveStatusReportsOnlyActionableIssues(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.show();auto* status=window.findChild<QLabel*>("autosaveStatus");QVERIFY(status);QVERIFY(status->text().isEmpty());
  auto* field=window.findChild<QLineEdit*>("maxFiles");field->setText("invalid");QVERIFY(status->text().isEmpty());QTRY_VERIFY(field->property("invalid").toBool());QVERIFY(!window.findChild<QLabel*>("maxFilesError")->text().isEmpty());QVERIFY(status->text().isEmpty());
  field->setText("250");QTRY_COMPARE(service.snapshot().limits.maxFiles,250);QTRY_VERIFY(status->text().isEmpty());
  fixture.client.failOnce["settings.save"]={"client_unavailable","Synthetic lost reply",{}};field->setText("251");QTRY_VERIFY(status->text().contains("Couldn’t save"));QCOMPARE(service.snapshot().limits.maxFiles,250);window.findChild<QPushButton*>("retryAutosave")->click();QTRY_COMPARE(service.snapshot().limits.maxFiles,251);QTRY_VERIFY(status->text().isEmpty());
 }

 void redesignedShortDropdownShowsEveryOptionWithoutScrolling_data(){
  QTest::addColumn<int>("count");QTest::addColumn<bool>("editable");
  for(const auto count:{1,2,3,8})for(const auto editable:{false,true})QTest::newRow(qPrintable(QString("%1-options-%2").arg(count).arg(editable?"editable":"fixed")))<<count<<editable;
 }
 void redesignedShortDropdownShowsEveryOptionWithoutScrolling(){
  QFETCH(int,count);QFETCH(bool,editable);VisualStyle::apply(*qApp);
  QWidget root;auto* layout=new QVBoxLayout(&root);auto* combo=VisualStyle::choiceBox(&root);
  combo->setEditable(editable);for(int i=0;i<count;++i)combo->addItem(QString("Option %1").arg(i+1));
  layout->addWidget(combo);layout->addStretch();RedesignTheme::apply(root);root.resize(600,400);root.show();
  for(const int startingIndex:QList<int>{0,count-1}){
   combo->setCurrentIndex(startingIndex);combo->showPopup();auto* view=combo->view();QTRY_VERIFY(view->isVisible());
   const auto viewport=view->viewport()->rect();
   QVERIFY2(viewport.contains(view->visualRect(combo->model()->index(0,0))),"First option is clipped");
   QVERIFY2(viewport.contains(view->visualRect(combo->model()->index(count-1,0))),"Last option is clipped");
   QCOMPARE(view->verticalScrollBar()->maximum(),0);
   const int target=startingIndex==0?count-1:0;
   QTest::mouseClick(view->viewport(),Qt::LeftButton,Qt::NoModifier,view->visualRect(combo->model()->index(target,0)).center());
   QTRY_COMPARE(combo->currentIndex(),target);QTRY_VERIFY(!view->isVisible());
   combo->showPopup();QTest::keyClick(view,Qt::Key_Home);QTest::keyClick(view,Qt::Key_Return);
   QTRY_COMPARE(combo->currentIndex(),0);QTRY_VERIFY(!view->isVisible());
  }
 }
 void captureDestinationPopupFitsAllThreeChoices(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto second=profiles.first().toObject();second["id"]="synthetic-second";second["label"]="Second destination";profiles.append(second);doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1060,680);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(1);window.show();auto* combo=window.findChild<QComboBox*>("defaultCaptureDestination");QVERIFY(combo);QCOMPARE(combo->count(),3);combo->showPopup();auto* view=combo->view();QTRY_VERIFY(view->isVisible());for(int i=0;i<combo->count();++i)QVERIFY2(view->viewport()->rect().contains(view->visualRect(combo->model()->index(i,0))),"Capture Destination choice is clipped");QCOMPARE(view->verticalScrollBar()->maximum(),0);combo->hidePopup();
 }
 // #145: Backblaze B2's S3 API lacks conditional writes, so the S3 picker no longer offers it.
 // A saved B2 endpoint still loads, as "Other S3-compatible", and says why uploads fail.
 void s3ServicePickerOmitsBackblazeAndExplainsSavedB2Endpoint(){
  QTemporaryDir dir;CredentialStore secrets;ProviderRegistry registry;registry.add("s3",std::make_shared<S3Provider>(secrets));SettingsService service(dir.filePath("config.json"),[&](auto id,auto values){return registry.validate(id,values);});
  auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto profile=profiles.first().toObject();auto settings=profile["settings"].toObject();
  settings["endpoint"]="https://s3.us-west-004.backblazeb2.com";settings["region"]="us-west-004";profile["settings"]=settings;profiles[0]=profile;doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));
  UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* picker=window.findChild<QComboBox*>("s3Service");QVERIFY(picker);
  QStringList items;for(int i=0;i<picker->count();++i)items<<picker->itemText(i);
  QCOMPARE(items,QStringList({"Amazon S3","Cloudflare R2","Other S3-compatible"}));
  QTRY_COMPARE(picker->currentText(),QString("Other S3-compatible"));
  bool explained=false;for(auto* label:window.findChildren<QLabel*>())if(label->text().startsWith("Endpoint: https://s3.us-west-004.backblazeb2.com")&&label->text().contains("lacks the conditional writes"))explained=true;
  QVERIFY(explained);QCOMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["endpoint"].toString(),QString("https://s3.us-west-004.backblazeb2.com"));
 }
 // #282: a Backblaze B2 destination edits its own fields and saves settings the B2 provider accepts.
 void backblazeDestinationUsesItsOwnFieldsAndSavesValidSettings(){
  QTemporaryDir dir;CredentialStore secrets;ProviderRegistry registry;registry.add("s3",std::make_shared<S3Provider>(secrets));registry.add("backblaze-b2",std::make_shared<B2Provider>(secrets));
  SettingsService service(dir.filePath("config.json"),[&](auto id,auto values){return registry.validate(id,values);},nullptr,[&](auto id,auto reference){return registry.validateCredentials(id,reference);});
  QVERIFY(service.save(Settings::developmentTemplate(),{}));
  UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* provider=window.findChild<QComboBox*>("storageProvider");provider->setCurrentIndex(provider->findData("backblaze-b2"));
  auto* bucket=window.findChild<QLineEdit*>("s3Bucket");QTRY_VERIFY(bucket->isVisible());
  for(const auto* name:{"s3Service","s3RegionChoices","s3AclMode"})QVERIFY2(!window.findChild<QComboBox*>(name)->isVisible(),name);
  QVERIFY(window.findChild<QComboBox*>("b2LinkMode")->isVisible());QVERIFY(!window.findChild<QSpinBox*>("b2LinkDays")->isVisible());
  QVERIFY(!window.findChild<QLineEdit*>("credentialSessionToken")->isVisible());
  QCOMPARE(window.findChild<QLineEdit*>("credentialAccessKey")->accessibleName(),QString("Application key ID"));
  QCOMPARE(window.findChild<QLineEdit*>("credentialSecretKey")->accessibleName(),QString("Application key"));
  bucket->setText("putgd-test-bucket");QMetaObject::invokeMethod(bucket,"editingFinished");
  const auto saved=[&]{return service.document()["profiles"].toArray()[0].toObject();};
  QTRY_COMPARE(saved()["provider"].toString(),QString("backblaze-b2"));
  auto expected=b2::defaults();expected["bucket"]="putgd-test-bucket";
  QTRY_COMPARE(saved()["settings"].toObject(),expected);
  QCOMPARE(saved()["credentials"].toObject()["reference"].toString(),QString("putgd/b2/development-s3"));
  auto* mode=window.findChild<QComboBox*>("b2LinkMode");mode->setCurrentIndex(mode->findData("authorized"));emit mode->activated(mode->currentIndex());
  QTRY_COMPARE(saved()["settings"].toObject()["linkMode"].toString(),QString("authorized"));
  QTRY_VERIFY(window.findChild<QSpinBox*>("b2LinkDays")->isVisible());QVERIFY(!window.findChild<QLineEdit*>("s3PublicUrlBase")->isVisible());
  QVERIFY(saved()["settings"].toObject()["publicUrlBase"].isNull());QCOMPARE(saved()["settings"].toObject()["linkValidityDays"].toInt(),7);
 }
 void redesignedLongDropdownScrollsWithinItsVisibleItemLimit(){
  VisualStyle::apply(*qApp);QWidget root;auto* layout=new QVBoxLayout(&root);auto* combo=VisualStyle::choiceBox(&root);
  combo->setMaxVisibleItems(7);for(int i=0;i<60;++i)combo->addItem(QString("Option %1").arg(i+1));
  layout->addWidget(combo);layout->addStretch();RedesignTheme::apply(root);root.resize(600,400);root.show();combo->showPopup();
  auto* view=combo->view();QTRY_VERIFY(view->isVisible());QVERIFY(view->verticalScrollBar()->maximum()>0);
  QVERIFY(view->viewport()->height()<view->sizeHintForRow(0)*8);
  QTest::keyClick(view,Qt::Key_End);QTest::keyClick(view,Qt::Key_Return);QTRY_COMPARE(combo->currentIndex(),59);QTRY_VERIFY(!view->isVisible());
 }
 void protonConnectOpensSignInOnceAndShowsProgressBesideButton(){
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=configuration(dir.path(),true);doc["profiles"]=QJsonArray{QJsonObject{{"id","synthetic-proton"},{"label","Synthetic Proton"},{"provider","proton-drive"},{"settings",ProtonProvider::defaults()},{"credentials",QJsonObject{{"mode","keychain"},{"reference","putgd/synthetic-proton"}}}}};QVERIFY(service.save(doc,{}));UiSessionFixture fixture(service,secrets);int starts=0;JsonReply complete;fixture.setup.setProviderSetup([&](const QString& kind,const Destination&,const QJsonObject&,JsonReply done,auto event){QCOMPARE(kind,QString("connectAccount"));++starts;complete=done;event({{"event","auth"},{"url","https://account.proton.me/cli?synthetic=true"}});});openedUrls_.clear();QDesktopServices::setUrlHandler("https",this,"recordProtonUrl");auto cleanup=qScopeGuard([]{QDesktopServices::unsetUrlHandler("https");});MainWindow window(fixture.session);window.navigate("destinations");window.show();auto* button=window.findChild<QPushButton*>("connectProtonAccount");button->click();QTRY_COMPARE(openedUrls_.size(),1);QCOMPARE(openedUrls_.first().host(),QString("account.proton.me"));auto* account=window.findChild<QLabel*>("protonAccountStatus");QVERIFY(account);QVERIFY(account->text().contains("browser"));QVERIFY(account->isVisible());QTest::qWait(600);QCOMPARE(openedUrls_.size(),1);QCOMPARE(starts,1);QVERIFY(complete);complete({"proton_runtime_unavailable","Synthetic runtime unavailable",{}},{});QTRY_VERIFY(account->text().contains("Synthetic runtime unavailable"));QTRY_VERIFY(button->isEnabled());
 }

 void protonConnectionTestAllowsMakingTheDestinationDefault_data(){
  QTest::addColumn<bool>("rootFolder");QTest::addColumn<bool>("editAfterTest");
  QTest::newRow("top-level-folder")<<false<<false;QTest::newRow("my-files-root")<<true<<false;QTest::newRow("changed-folder-needs-retest")<<false<<true;
 }
 void protonConnectionTestAllowsMakingTheDestinationDefault(){
  QFETCH(bool,rootFolder);QFETCH(bool,editAfterTest);
  QTemporaryDir dir;CredentialStore secrets;
  SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});
  auto doc=configuration(dir.path(),true);auto account=ProtonProvider::defaults();
  account["accountRootUid"]="synthetic-root";account["folderUid"]=rootFolder?"synthetic-root":"synthetic-folder";
  doc["profiles"]=QJsonArray{QJsonObject{{"id","synthetic-proton"},{"label","Synthetic Proton"},{"provider","proton-drive"},{"settings",account},{"credentials",QJsonObject{{"mode","keychain"},{"reference","putgd/synthetic-proton"}}}}};
  QVERIFY(service.save(doc,{}));int uploads=0,inspections=0;JsonReply finish;
  UiSessionFixture fixture(service,secrets,[&](auto,auto,auto reply){++uploads;reply({"test_unsupported","S3-only tester",{}});});
  fixture.setup.setProviderSetup([&](const QString& kind,const Destination& destination,const QJsonObject& input,JsonReply reply,auto){
   QCOMPARE(kind,QString("testDestination"));QCOMPARE(destination.settings,account);QVERIFY(input.isEmpty());++inspections;finish=std::move(reply);
  });
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* test=window.findChild<QPushButton*>("testDestination");QVERIFY(test->isVisible());test->click();
  QTRY_COMPARE(inspections,1);QVERIFY(!test->isEnabled());
  auto* status=window.findChild<QLabel*>("destinationStatus");QVERIFY(status->text().contains("account and selected folder"));
  finish({},{{"testScope","account-folder"}});
  QTRY_VERIFY(status->text().contains("Account and selected folder are accessible"));QVERIFY(!status->text().contains("upload, public link"));QCOMPARE(uploads,0);
  if(editAfterTest){auto* folder=window.findChild<QComboBox*>("protonFolder");folder->addItem("Another folder","synthetic-other");folder->setCurrentIndex(folder->count()-1);QTRY_COMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["folderUid"].toString(),QString("synthetic-other"));}
  bool warned=false;QTimer dismiss;connect(&dismiss,&QTimer::timeout,this,[&]{if(auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())){warned=true;box->button(QMessageBox::Yes)->click();}});dismiss.start(10);
  window.findChild<QPushButton*>("makeDefaultDestination")->click();
  QTRY_COMPARE(service.document()["captureDefaults"].toObject()["profileId"].toString(),QString("synthetic-proton"));
  QCOMPARE(warned,editAfterTest);QCOMPARE(service.snapshot().actions["region"].profileId,QString("synthetic-proton"));
 }
 void protonConnectionTestPropagatesProviderErrors_data(){
  QTest::addColumn<QString>("expected");
  for(const auto& code:QStringList{"proton_setup_required","proton_account_changed","proton_folder_unavailable","authentication_required"})QTest::newRow(qPrintable(code))<<code;
 }
 void protonConnectionTestPropagatesProviderErrors(){
  QFETCH(QString,expected);
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});
  auto doc=configuration(dir.path(),true);auto account=ProtonProvider::defaults();account["accountRootUid"]="synthetic-root";account["folderUid"]="synthetic-folder";
  doc["profiles"]=QJsonArray{QJsonObject{{"id","synthetic-proton"},{"label","Synthetic Proton"},{"provider","proton-drive"},{"settings",account},{"credentials",QJsonObject{{"mode","keychain"},{"reference","putgd/synthetic-proton"}}}}};QVERIFY(service.save(doc,{}));
  int uploads=0,inspections=0;UiSessionFixture fixture(service,secrets,[&](auto,auto,auto reply){++uploads;reply({});});
  fixture.setup.setProviderSetup([&](const QString& kind,const Destination&,const QJsonObject&,JsonReply reply,auto){++inspections;QCOMPARE(kind,QString("testDestination"));reply({expected,"Synthetic provider failure",{}},{});});
  QString id;fixture.setup.start("testDestination","synthetic-proton",service.revision(),{},[&](Error error,QJsonObject data){QVERIFY(!error);id=data["setupId"].toString();});
  QJsonObject job;auto inspect=[&]{fixture.setup.inspect(id,[&](Error error,QJsonObject data){QVERIFY(!error);job=data;});return job["state"]=="failed";};
  QTRY_VERIFY(inspect());QCOMPARE(job["error"].toObject()["code"].toString(),expected);QCOMPARE(uploads,0);QCOMPARE(inspections,1);
 }

 void protonToS3DraftSavesProviderTogetherWithFinishedBucket(){
  QTemporaryDir dir;CredentialStore secrets;ProviderRegistry registry;registry.add("s3",std::make_shared<S3Provider>(secrets));registry.add("proton-drive",std::make_shared<ProtonProvider>());SettingsService service(dir.filePath("config.json"),[&](auto id,auto values){return registry.validate(id,values);},nullptr,[&](auto id,auto reference){return registry.validateCredentials(id,reference);});auto doc=configuration(dir.path(),true);doc["profiles"]=QJsonArray{QJsonObject{{"id","synthetic-proton"},{"label","Synthetic destination"},{"provider","proton-drive"},{"settings",ProtonProvider::defaults()},{"credentials",QJsonObject{{"mode","keychain"},{"reference","putgd/synthetic-proton"}}}}};QVERIFY(service.save(doc,{}));UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();auto* provider=window.findChild<QComboBox*>("storageProvider");provider->setCurrentIndex(provider->findData("s3"));auto* status=window.findChild<QLabel*>("settingsStatus");QTRY_VERIFY(status->text().contains("Invalid S3 settings"));QCOMPARE(service.document()["profiles"].toArray()[0].toObject()["provider"].toString(),QString("proton-drive"));QVERIFY(!service.document()["profiles"].toArray()[0].toObject()["settings"].toObject().contains("bucket"));auto* bucket=window.findChild<QLineEdit*>("s3Bucket");bucket->setText("synthetic-valid-bucket");QMetaObject::invokeMethod(bucket,"editingFinished");QTRY_COMPARE(service.document()["profiles"].toArray()[0].toObject()["provider"].toString(),QString("s3"));QCOMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["bucket"].toString(),QString("synthetic-valid-bucket"));QVERIFY(!status->text().contains("Proton"));
 }
 void wheelOverSettingsTabsDoesNotChangePageButKeyboardStillWorks(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();auto* tabs=window.findChild<QTabWidget*>("settingsTabs");auto* bar=tabs->tabBar();tabs->setCurrentIndex(1);const QPointF position=bar->tabRect(1).center();for(const auto delta:{120,-120}){QWheelEvent event(position,bar->mapToGlobal(position.toPoint()),QPoint{},QPoint{0,delta},Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);QApplication::sendEvent(bar,&event);QCOMPARE(tabs->currentIndex(),1);}bar->setFocus();QTest::keyClick(bar,Qt::Key_Right);QCOMPARE(tabs->currentIndex(),2);
 }
 void editRailButtonOffersOtherSourcesWithoutGrowingTheRail(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(configuration(dir.path(),true),{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);
  QCOMPARE(window.findChildren<QPushButton*>("captureRailButton").size(),Settings::invocableActionNames().size());
  QPushButton* record=nullptr;for(auto* button:window.findChildren<QPushButton*>("captureRailButton"))if(button->property("captureAction")=="record")record=button;QVERIFY(record);
  QPushButton* edit=nullptr;for(auto* button:window.findChildren<QPushButton*>("captureRailButton"))if(button->property("captureAction")=="edit")edit=button;
  // No extra control on the button: the other sources are a right-click or a drop.
  QVERIFY(edit);QVERIFY(edit->findChildren<QToolButton*>().isEmpty());QCOMPARE(edit->contextMenuPolicy(),Qt::CustomContextMenu);QVERIFY(edit->acceptDrops());
  QVERIFY(edit->toolTip().contains("Right-click"));
  auto* menu=window.findChild<QMenu*>("editSourceMenu");QVERIFY(menu);
  QStringList labels;for(auto* action:menu->actions())labels.append(action->text());
  QCOMPARE(labels,QStringList({"Edit clipboard image","Edit an image file…"}));
  QStringList requests;connect(&window,&MainWindow::editRequested,this,[&](const QString& source,const QString& path){requests.append(source+"|"+path);});
  int actions=0;connect(&window,&MainWindow::actionRequested,this,[&](auto){++actions;});
  window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
  emit edit->customContextMenuRequested(QPoint(8,8));QTRY_VERIFY(menu->isVisible());menu->actions().first()->trigger();menu->hide();
  QCOMPARE(requests,QStringList({"clipboard|"}));QCOMPARE(actions,0);requests.clear();
  QImage pixel(2,2,QImage::Format_RGB32);pixel.fill(Qt::blue);const auto png=dir.filePath("Dropped.PNG");QVERIFY(pixel.save(png,"PNG"));
  const auto text=dir.filePath("notes.txt");{QFile file(text);QVERIFY(file.open(QIODevice::WriteOnly));file.write("x");}
  auto drop=[&](const QList<QUrl>& urls){
   QMimeData data;data.setUrls(urls);const QPoint at(20,20);
   QDragEnterEvent enter(at,Qt::CopyAction,&data,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(edit,&enter);
   const bool accepted=enter.isAccepted();const bool lit=edit->property("dropTarget").toBool();
   QDropEvent dropped(QPointF(at),Qt::CopyAction,&data,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(edit,&dropped);
   return accepted&&lit&&!edit->property("dropTarget").toBool();
  };
  QVERIFY(!drop({QUrl::fromLocalFile(text)}));QVERIFY(!edit->property("dropTarget").toBool());QVERIFY(!drop({QUrl::fromLocalFile(png),QUrl::fromLocalFile(png)}));QVERIFY(!drop({QUrl("https://example.invalid/a.png")}));QVERIFY(requests.isEmpty());
  QVERIFY(drop({QUrl::fromLocalFile(png)}));QCOMPARE(requests,QStringList({"file|"+QFileInfo(png).absoluteFilePath()}));QCOMPARE(actions,0);
 }
 void captureRailExplainsActionAndCurrentSavedOutputWithoutInvokingIt(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);int invoked=0;connect(&window,&MainWindow::actionRequested,this,[&](auto){++invoked;});const QHash<QString,QString> labels{{"region","Capture region"},{"window","Capture window"},{"desktop","Capture desktop"},{"clipboard","Upload clipboard"},{"edit","Capture region & edit"}};for(const auto& action:Settings::actionNames()){QPushButton* button=nullptr;for(auto* candidate:window.findChildren<QPushButton*>("captureRailButton"))if(candidate->property("captureAction")==action)button=candidate;QVERIFY(button);QCOMPARE(button->accessibleName(),labels.value(action));QVERIFY(button->toolTip().contains("Save locally"));QVERIFY(button->accessibleDescription().contains("clipboard",Qt::CaseInsensitive));}auto* help=window.findChild<QToolButton*>("captureActionsHeadingHelp");QVERIFY(help);help->click();auto* dialog=window.findChild<QMessageBox*>("settingHelpDialog");QVERIFY(dialog);QVERIFY(dialog->informativeText().contains("focused external app window"));dialog->close();
  {QWidget host;QLineEdit control(&host);auto* row=VisualStyle::settingLabel("Screen region & edit shortcut","help",&control,&host);QLabel* caption=nullptr;for(auto* label:row->findChildren<QLabel*>())if(label->buddy()==&control)caption=label;QVERIFY(caption);QCOMPARE(caption->text(),QString("Screen region && edit shortcut"));QCOMPARE(control.accessibleName(),QString("Screen region & edit shortcut"));}
  auto doc=service.document();auto protonSettings=ProtonProvider::defaults();protonSettings["shareByDefault"]=false;doc["profiles"]=QJsonArray{QJsonObject{{"id","synthetic-proton"},{"label","Synthetic Drive"},{"provider","proton-drive"},{"settings",protonSettings},{"credentials",QJsonObject{{"mode","keychain"},{"reference","putgd/synthetic-drive"}}}}};auto actions=doc["actions"].toObject();auto region=actions["region"].toObject();region["destination"]="upload";region["profileId"]="synthetic-proton";actions["region"]=region;doc["actions"]=actions;QVERIFY(service.save(doc,service.revision()));QPushButton* capture=nullptr;for(auto* button:window.findChildren<QPushButton*>("captureRailButton"))if(button->property("captureAction")=="region")capture=button;QVERIFY(capture);QTRY_VERIFY(capture->accessibleDescription().contains("Upload privately"));QVERIFY(capture->accessibleDescription().contains("clipboard is left unchanged"));region["publicationMode"]="share";actions["region"]=region;doc["actions"]=actions;QVERIFY(service.save(doc,service.revision()));QTRY_VERIFY(capture->accessibleDescription().contains("copy its share link"));QCOMPARE(invoked,0);
 }
 void constructionDoesNotWriteAndHasNoSaveCancelControls(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider s3(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto settings){return s3.validateSettings(settings);});QVERIFY(service.save(Settings::developmentTemplate(),{}));const auto initial=service.document();const auto revision=service.revision();UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();QTest::qWait(500);
  QCOMPARE(service.document(),initial);QCOMPARE(service.revision(),revision);QVERIFY(!window.findChild<QPushButton*>("saveSettings"));QVERIFY(!window.findChild<QPushButton*>("cancelSettings"));QVERIFY(!window.findChild<QPushButton*>("setupLocal"));auto* tabs=window.findChild<QTabWidget*>("settingsTabs");QVERIFY(tabs);QCOMPARE(tabs->count(),4);QCOMPARE(tabs->tabText(0),QString("General"));QCOMPARE(tabs->tabText(3),QString("Advanced"));
 }
 void finishedFieldAutosavesAndSurvivesRestart(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));auto doc=configuration(dir.path(),true);QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();auto* root=window.findChild<QLineEdit*>("archiveRoot");QVERIFY(root);window.activateWindow();root->setFocus();QTRY_VERIFY(root->hasFocus());root->selectAll();QTest::keyClicks(root,dir.filePath("saved-locally"));QTest::qWait(450);QCOMPARE(service.document()["archive"].toObject()["root"],doc["archive"].toObject()["root"]);window.findChild<QCheckBox*>("launchAtLogin")->setFocus();QTRY_COMPARE(service.document()["archive"].toObject()["root"].toString(),dir.filePath("saved-locally"));SettingsService restarted(service.path());QVERIFY(restarted.reload());QCOMPARE(restarted.document()["archive"],service.document()["archive"]);
 }
 void invalidEditStaysInlineWhileIndependentValidToggleSaves(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);auto* number=window.findChild<QLineEdit*>("maxFiles");number->setText("not a number");QMetaObject::invokeMethod(number,"editingFinished");QTRY_VERIFY(number->property("invalid").toBool());QCOMPARE(service.document()["limits"].toObject()["maxFiles"].toInt(),100);auto* notify=window.findChild<QCheckBox*>("notifications");notify->setChecked(false);QTRY_VERIFY(!service.document()["preferences"].toObject()["notifications"].toBool());QCOMPARE(number->text(),QString("not a number"));QVERIFY(number->property("invalid").toBool());QCOMPARE(service.document()["limits"].toObject()["maxFiles"].toInt(),100);
 }
 void displayUnitsPreserveStoredBytesPixelsAndMilliseconds(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider s3(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto settings){return s3.validateSettings(settings);});QVERIFY(service.save(Settings::developmentTemplate(),{}));UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);QCOMPARE(window.findChild<QLineEdit*>("maxFileBytes")->text(),QString("Auto"));QCOMPARE(window.findChild<QLineEdit*>("imagePixels")->text(),QString("100"));QCOMPARE(window.findChild<QLineEdit*>("s3ConnectTimeoutMs")->text(),QString("5"));window.findChild<QLineEdit*>("maxFileBytes")->setText("2.000000000931322574615478515625");window.findChild<QLineEdit*>("imagePixels")->setText("100.000001");window.findChild<QLineEdit*>("s3ConnectTimeoutMs")->setText("5.123");QTRY_COMPARE(service.document()["limits"].toObject()["maxFileBytes"].toInteger(),2147483649LL);QCOMPARE(service.document()["limits"].toObject()["imagePixels"].toInteger(),100000001LL);QCOMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["connectTimeoutMs"].toInt(),5123);QCOMPARE(window.findChild<QLineEdit*>("maxFileBytes")->text(),QString("2.000000000931322574615478515625"));
 }
 void resetOneAndResetAllUseRealDefaults(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));auto doc=configuration(dir.path(),true);auto limits=doc["limits"].toObject();limits["concurrentUploads"]=4;doc["limits"]=limits;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);QPushButton* reset=nullptr;for(auto* button:window.findChildren<QPushButton*>())if(button->accessibleName()=="Reset Uploads at once to default")reset=button;QVERIFY(reset);QVERIFY(reset->isVisible());reset->click();QTRY_COMPARE(service.document()["limits"].toObject()["concurrentUploads"].toInt(),3);window.findChild<QLineEdit*>("maxFiles")->setText("25");QTRY_COMPARE(service.document()["limits"].toObject()["maxFiles"].toInt(),25);
  // #238: Diagnostics now shares the tab; Reset all to defaults still resets only the limits.
  auto* debug=window.findChild<QCheckBox*>("debugLogging");debug->setChecked(true);QTRY_VERIFY(service.snapshot().debugLogging);
  QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QMessageBox*>(widget))dialog->button(QMessageBox::Yes)->click();});window.findChild<QPushButton*>("resetAdvancedDefaults")->click();QTRY_COMPARE(service.document()["limits"].toObject()["maxFiles"].toInt(),100);
  QTest::qWait(500);QVERIFY(debug->isChecked());QVERIFY(service.snapshot().debugLogging);
 }
 void externalEditsPreserveInvalidDraftAndReloadRecovers(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);auto* number=window.findChild<QLineEdit*>("maxFiles");number->setText("invalid");QTRY_VERIFY(number->property("invalid").toBool());auto external=service.document();auto prefs=external["preferences"].toObject();prefs["notifications"]=false;external["preferences"]=prefs;QVERIFY(service.save(external,service.revision()));QTRY_VERIFY(window.settings()->saveViewState()["conflict"].toBool());QCOMPARE(number->text(),QString("invalid"));window.findChild<QPushButton*>("reloadSettings")->click();QTRY_COMPARE(number->text(),QString("100"));QVERIFY(!window.settings()->saveViewState()["conflict"].toBool());QVERIFY(!window.findChild<QCheckBox*>("notifications")->isChecked());
 }
 void malformedExternalReloadPreservesAcceptedSettingsAndBlocksStaleOverwrite(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));const auto original=service.document();const auto revision=service.revision();CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);auto* field=window.findChild<QLineEdit*>("maxFiles");field->setText("invalid draft");QTRY_VERIFY(field->property("invalid").toBool());QFile external(service.path());QVERIFY(external.open(QIODevice::WriteOnly|QIODevice::Truncate));QCOMPARE(external.write("{malformed"),qint64(10));external.close();window.findChild<QPushButton*>("reloadSettings")->click();auto* status=window.findChild<QLabel*>("settingsStatus");QTRY_VERIFY(status->text().contains("config_invalid"));QTRY_VERIFY(!window.settings()->saveViewState().contains("stateError"));QCOMPARE(service.document(),original);QCOMPARE(service.revision(),revision);QVERIFY(!service.save(original,revision));QVERIFY(external.open(QIODevice::ReadOnly));QCOMPARE(external.readAll(),QByteArray("{malformed"));external.close();QVERIFY(external.open(QIODevice::WriteOnly|QIODevice::Truncate));const auto valid=QJsonDocument(original).toJson(QJsonDocument::Indented);QCOMPARE(external.write(valid),qint64(valid.size()));external.close();window.findChild<QPushButton*>("reloadSettings")->click();QTRY_COMPARE(field->text(),QString("100"));QCOMPARE(service.document(),original);
 }
 void destinationsUseFriendlyEnumsAndKeepCredentialsOutOfConfigAndCheckpoint(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider s3(secrets);ProtonProvider proton;SettingsService service(dir.filePath("config.json"),[&](auto id,auto settings){return id=="s3"?s3.validateSettings(settings):proton.validateSettings(settings);});QVERIFY(service.save(Settings::developmentTemplate(),{}));UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();QVERIFY(!window.findChild<QLineEdit*>("destinationId")->isVisible());auto* acl=window.findChild<QComboBox*>("s3AclMode");QCOMPARE(acl->currentData().toString(),QString("public-read"));QVERIFY(!acl->currentText().contains("public-read"));window.findChild<QLineEdit*>("credentialSecretKey")->setText("synthetic-never-export-me");window.findChild<QLineEdit*>("credentialAccessKey")->setText("synthetic-access");const auto encoded=QJsonDocument(window.settings()->saveViewState()).toJson();QVERIFY(!encoded.contains("synthetic-never-export-me"));QVERIFY(!encoded.contains("synthetic-access"));QVERIFY(!QJsonDocument(service.document()).toJson().contains("synthetic-never-export-me"));
  auto* provider=window.findChild<QComboBox*>("storageProvider");provider->setCurrentIndex(provider->findData("proton-drive"));QTRY_COMPARE(service.document()["profiles"].toArray()[0].toObject()["provider"].toString(),QString("proton-drive"));window.findChild<QDoubleSpinBox*>("protonRequestTimeout")->setValue(321.123);QMetaObject::invokeMethod(window.findChild<QDoubleSpinBox*>("protonRequestTimeout"),"editingFinished");QTRY_COMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["requestTimeoutMs"].toInt(),321123);provider->setCurrentIndex(provider->findData("s3"));QCOMPARE(window.findChild<QLineEdit*>("credentialSecretKey")->text(),QString("synthetic-never-export-me"));window.settings()->clearCredentialInputs();QVERIFY(window.findChild<QLineEdit*>("credentialSecretKey")->text().isEmpty());
 }
 void perActionLocalOutputAndClipboardDeliveryRemainIndependent(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=Settings::developmentTemplate();auto archive=doc["archive"].toObject();archive["keepUploads"]=true;doc["archive"]=archive;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);auto* clipboard=window.findChild<QComboBox*>("uploadClipboardBehavior");clipboard->setCurrentIndex(clipboard->findData("list"));window.findChild<QCheckBox*>("useDefaultRegion")->setChecked(false);auto* destination=window.findChild<QComboBox*>("profileRegion");destination->setCurrentIndex(0);QTRY_COMPARE(service.document()["preferences"].toObject()["uploadClipboardBehavior"].toString(),QString("list"));QTRY_COMPARE(service.snapshot().actions["region"].destination,QString("local"));QCOMPARE(service.snapshot().actions["desktop"].destination,QString("upload"));QVERIFY(service.snapshot().archive.keepUploads);
 }
 // #60: General → Appearance saves preferences.appearance.theme; an untouched
 // config keeps no appearance object.
 void appearanceThemeIsSavedAndReloaded(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);
  auto* theme=window.findChild<QComboBox*>("appearanceTheme");QVERIFY(theme);QCOMPARE(theme->currentData().toString(),QString("system"));QCOMPARE(theme->count(),3);
  window.findChild<QCheckBox*>("notifications")->setChecked(false);QTRY_VERIFY(!service.document()["preferences"].toObject()["notifications"].toBool());QVERIFY(!service.document()["preferences"].toObject().contains("appearance"));
  theme->setCurrentIndex(theme->findData("light"));QTRY_COMPARE(service.document()["preferences"].toObject()["appearance"].toObject()["theme"].toString(),QString("light"));QCOMPARE(service.snapshot().appearanceTheme,QString("light"));
  // A draft carrying the appearance object still checkpoints (view-state allow-list).
  const auto state=window.settings()->saveViewState();QVERIFY(state["draft"].toObject()["preferences"].toObject().contains("appearance"));QVERIFY(!validateUiState({{"schemaVersion",1},{"settings",state}}));
  auto doc=service.document();auto preferences=doc["preferences"].toObject();preferences["appearance"]=QJsonObject{{"theme","dark"}};doc["preferences"]=preferences;QVERIFY(service.save(doc,service.revision()));
  fixture.session.invalidate();QTRY_COMPARE(theme->currentData().toString(),QString("dark"));
 }
 // #239: the clipboard action says upload, like the tray; ids and keys stay.
 void clipboardActionSaysUpload(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);
  QPushButton* clipboard=nullptr;for(auto* button:window.findChildren<QPushButton*>("captureRailButton"))if(button->property("captureAction")=="clipboard")clipboard=button;
  QVERIFY(clipboard);QCOMPARE(clipboard->accessibleName(),QString("Upload clipboard"));QVERIFY(clipboard->accessibleDescription().startsWith("Upload copied local files"));QVERIFY(clipboard->accessibleDescription().contains("uploaded as text"));
  auto* railHelp=window.findChild<QToolButton*>("captureActionsHeadingHelp");QVERIFY(railHelp);QVERIFY(railHelp->toolTip().contains("Upload clipboard: upload copied local files"));QVERIFY(!railHelp->toolTip().contains("mport"));
  auto* fileTemplate=window.findChild<QLineEdit*>("fileTemplate");QCOMPARE(fileTemplate->accessibleName(),QString("Copied & uploaded files"));QVERIFY(fileTemplate->toolTip().startsWith("Names copied and uploaded files when Custom template is selected"));
  QCOMPARE(fileTemplate->property("configPath").toString(),QString("/naming/fileTemplate"));
  // The size limits' help sits on their Auto / No limit choice.
  auto help=[&](const QString& key){auto* choice=window.findChild<QWidget*>(key+"Choice");return (choice?choice:window.findChild<QWidget*>(key))->toolTip();};
  for(const auto* name:{"maxFiles","maxFileBytes","pendingArtifactBytes","clipboardTextBytes"}){const auto text=help(name);QVERIFY2(!text.isEmpty(),name);QVERIFY2(!text.contains("mport"),qPrintable(text));}
  QVERIFY(help("maxFileBytes").startsWith("The largest individual uploaded file"));QVERIFY(help("clipboardTextBytes").contains("rejected before uploading"));
  QVERIFY(window.findChild<QWidget*>("useDefaultClipboard"));QCOMPARE(window.findChild<QCheckBox*>("useDefaultClipboard")->accessibleName(),QString("Clipboard upload: use default output"));
 }
 void newDestinationIsDraftUntilValidAndCannotBecomeDefaultWithEmptyBucket(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();QVERIFY(!window.findChild<QPushButton*>("makeDefaultDestination")->isEnabled());window.findChild<QLineEdit*>("s3Bucket")->setText("a");QTest::qWait(600);QVERIFY(service.revision().isEmpty());QVERIFY(window.findChild<QLineEdit*>("s3Bucket")->property("invalid").toBool());
 }
 void optionalDefaultsStayAbsentAndDoNotBlockDestinationSetup(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return provider.validateSettings(value);});auto doc=Settings::developmentTemplate();auto naming=doc["naming"].toObject();for(const auto& key:QStringList{"fileTemplate","captureTemplate","imageTemplate","textTemplate"})naming.remove(key);doc["naming"]=naming;auto limits=doc["limits"].toObject();limits.remove("pendingImageBytes");doc["limits"]=limits;auto preferences=doc["preferences"].toObject();preferences.remove("uploadClipboardBehavior");doc["preferences"]=preferences;QVERIFY(service.save(doc,{}));int tests=0;UiSessionFixture fixture(service,secrets,[&](auto,auto,auto reply){++tests;reply({});});MainWindow window(fixture.session);auto* pending=window.findChild<QLineEdit*>("pendingImageBytes");QVERIFY(pending);QVERIFY(!pending->property("defaultText").toString().isEmpty());QTRY_COMPARE(pending->text(),pending->property("defaultText").toString());window.findChild<QCheckBox*>("notifications")->setChecked(false);QTRY_VERIFY(!service.document()["preferences"].toObject()["notifications"].toBool());QTRY_VERIFY(!window.settings()->saveViewState().contains("stateError"));QVERIFY(!service.document()["naming"].toObject().contains("fileTemplate"));QVERIFY(!service.document()["limits"].toObject().contains("pendingImageBytes"));QVERIFY(!service.document()["preferences"].toObject().contains("uploadClipboardBehavior"));window.findChild<QPushButton*>("testDestination")->click();QTRY_COMPARE(tests,1);
 }
 void failedAutosaveKeepsDraftAcrossReopenAndRequiresExplicitRetry(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));const auto before=service.document();CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);fixture.client.failOnce["settings.save"]={"client_unavailable","Synthetic lost save reply",{}};window.findChild<QLineEdit*>("archiveRoot")->setText(dir.filePath("retry-output"));QTRY_VERIFY(window.settings()->saveViewState()["autosaveBlocked"].toBool());QTest::qWait(600);QCOMPARE(service.document(),before);const auto state=window.settings()->saveViewState();MainWindow reopened(fixture.session);QVERIFY(reopened.settings()->restoreViewState(state));QVERIFY(reopened.findChild<QPushButton*>("retryAutosave")->isHidden()==false);reopened.findChild<QPushButton*>("retryAutosave")->click();QTRY_COMPARE(service.document()["archive"].toObject()["root"].toString(),dir.filePath("retry-output"));QVERIFY(!reopened.settings()->saveViewState()["autosaveBlocked"].toBool());
 }
 void artifactLimitsOfferAutoAndNoLimit(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);
  auto limit=[&](const QString& key)->QJsonValue{return service.document()["limits"].toObject().value(key);};
  for(const auto& key:Settings::artifactByteLimitKeys()){
   auto* edit=window.findChild<QLineEdit*>(key);auto* combo=window.findChild<QComboBox*>(key+"Choice");QVERIFY(edit);QVERIFY(combo);
   QCOMPARE(edit->text(),QString("Auto"));QCOMPARE(edit->placeholderText(),QString("No limit"));
   QCOMPARE(combo->itemText(0),QString("Auto"));QCOMPARE(combo->itemText(1),QString("No limit"));
   QVERIFY(window.findChild<QToolButton*>(key+"ChoiceHelp"));
  }
  auto* edit=window.findChild<QLineEdit*>("maxFileBytes");auto* combo=window.findChild<QComboBox*>("maxFileBytesChoice");
  combo->setCurrentIndex(1);emit combo->activated(1);
  QTRY_VERIFY(limit("maxFileBytes").isNull());QCOMPARE(edit->text(),QString());
  edit->setText("3");QTRY_COMPARE(limit("maxFileBytes").toInteger(),3221225472LL);
  edit->setText("auto");QTRY_COMPARE(limit("maxFileBytes"),QJsonValue("auto"));
  combo->setCurrentIndex(0);emit combo->activated(0);edit->setText("");QTRY_VERIFY(limit("maxFileBytes").isNull());
  combo->setCurrentIndex(0);emit combo->activated(0);QTRY_COMPARE(limit("maxFileBytes"),QJsonValue("auto"));QCOMPARE(edit->text(),QString("Auto"));
  QCOMPARE(limit("maxBatchBytes"),QJsonValue("auto"));
 }
 void detailedSettingHelpIsReachableFromKeyboard(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);window.show();auto* help=window.findChild<QToolButton*>("maxFileBytesChoiceHelp");QVERIFY(help);QVERIFY(help->isVisible());help->setFocus();QTest::keyClick(help,Qt::Key_Space);QMessageBox* dialog=nullptr;QTRY_VERIFY((dialog=window.findChild<QMessageBox*>("settingHelpDialog")));QVERIFY(dialog->informativeText().contains("GiB"));QVERIFY(dialog->informativeText().contains("rejected"));QCOMPARE(dialog->textFormat(),Qt::PlainText);dialog->close();
 }
 void addDestinationDialogPersistsValidDraftAndRemovalPreservesOriginal(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return provider.validateSettings(value);});QVERIFY(service.save(Settings::developmentTemplate(),{}));const auto original=service.document()["profiles"].toArray()[0].toObject();UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();auto* add=window.findChild<QPushButton*>("addS3Destination");QVERIFY(add&&add->menu());QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QInputDialog*>(widget)){dialog->setTextValue("Synthetic second destination");dialog->accept();}});add->menu()->actions().first()->trigger();auto* bucket=window.findChild<QLineEdit*>("s3Bucket");bucket->setText("synthetic-second-bucket");QMetaObject::invokeMethod(bucket,"editingFinished");QTRY_COMPARE(service.document()["profiles"].toArray().size(),2);QTRY_VERIFY(!window.settings()->saveViewState().contains("stateError"));const auto profiles=service.document()["profiles"].toArray();QCOMPARE(profiles[0].toObject(),original);QCOMPARE(profiles[1].toObject()["label"].toString(),QString("Synthetic second destination"));QCOMPARE(profiles[1].toObject()["settings"].toObject()["bucket"].toString(),QString("synthetic-second-bucket"));SettingsService restarted(service.path(),[&](auto,auto value){return provider.validateSettings(value);});QVERIFY(restarted.reload());QCOMPARE(restarted.document()["profiles"],profiles);QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QMessageBox*>(widget))dialog->button(QMessageBox::Yes)->click();});window.findChild<QPushButton*>("removeDestination")->click();QTRY_COMPARE(service.document()["profiles"].toArray().size(),1);QCOMPARE(service.document()["profiles"].toArray()[0].toObject(),original);
 }
 void removingDestinationBeforeDebounceNeverAppliesItsEditToNextDestination(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return provider.validateSettings(value);});auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto second=profiles[0].toObject();second["id"]="second-destination";second["label"]="Second destination";auto fields=second["settings"].toObject();fields["bucket"]="second-bucket";second["settings"]=fields;profiles.append(second);doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();window.findChild<QLineEdit*>("s3Bucket")->setText("edited-first-bucket");QMetaObject::invokeMethod(window.findChild<QLineEdit*>("s3Bucket"),"editingFinished");QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QMessageBox*>(widget))dialog->button(QMessageBox::Yes)->click();});window.findChild<QPushButton*>("removeDestination")->click();QTRY_COMPARE(service.document()["profiles"].toArray().size(),1);QCOMPARE(service.document()["profiles"].toArray()[0].toObject(),second);
 }
 void finishedGeneralEditSavesAfterDelayedDestinationSetupCompletes(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return provider.validateSettings(value);});QVERIFY(service.save(Settings::developmentTemplate(),{}));std::function<void(Error)> complete;UiSessionFixture fixture(service,secrets,[&](auto,auto,auto reply){complete=reply;});MainWindow window(fixture.session);window.findChild<QPushButton*>("testDestination")->click();QTRY_VERIFY(bool(complete));window.findChild<QCheckBox*>("notifications")->setChecked(false);QTest::qWait(550);QVERIFY(service.document()["preferences"].toObject()["notifications"].toBool());complete({});QTRY_VERIFY(!service.document()["preferences"].toObject()["notifications"].toBool());
 }
 void openingConnectedProtonDestinationLoadsNamedFoldersWithoutReconnecting(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=configuration(dir.path());auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();profile["provider"]="proton-drive";auto protonSettings=ProtonProvider::defaults();protonSettings["accountRootUid"]="synthetic-account";protonSettings["folderUid"]="synthetic-folder";protonSettings["folderName"]="Saved folder";profile["settings"]=protonSettings;profiles[0]=profile;profiles.append(Settings::developmentTemplate()["profiles"].toArray().first());doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);int inspections=0;JsonReply finish;fixture.setup.setProviderSetup([&](const QString& kind,const Destination&,const QJsonObject&,JsonReply reply,auto){QCOMPARE(kind,QString("inspectAccount"));++inspections;finish=std::move(reply);});
  fixture.server.setUiDiagnostics([]{return QJsonObject{{"capabilities",QJsonObject{{"providers",QJsonObject{{"proton-drive",QJsonObject{}}}}}}};});fixture.session.refreshSnapshot();QTRY_VERIFY(fixture.session.diagnostics()["capabilities"].toObject()["providers"].toObject().contains("proton-drive"));MainWindow window(fixture.session);window.show();QTest::qWait(50);QCOMPARE(inspections,0);window.navigate("destinations");QTRY_COMPARE(inspections,1);auto* progress=window.findChild<QLabel*>("protonFolderStatus");QVERIFY(progress);QVERIFY(progress->isVisible());QVERIFY(progress->text().contains("Loading folders"));
  finish({},{{"accountRootUid","synthetic-account"},{"accountEmail","synthetic@example.invalid"},{"folders",QJsonArray{QJsonObject{{"uid","synthetic-folder"},{"name","Saved folder"}},QJsonObject{{"uid","synthetic-other-id"},{"name","Another folder"}}}}});auto* folders=window.findChild<QComboBox*>("protonFolder");QTRY_VERIFY(folders->findText("Another folder")>=0);QCOMPARE(folders->currentText(),QString("Saved folder"));QVERIFY(folders->findText("synthetic-other-id")==-1);auto* account=window.findChild<QLabel*>("protonAccountStatus");QVERIFY(account->text().contains("synthetic@example.invalid"));auto* list=window.findChild<QListWidget*>("destinationList");
  for(int repeat=0;repeat<5;++repeat){list->setCurrentRow(1);list->setCurrentRow(0);QVERIFY(account->isVisible());QVERIFY(account->text().contains("synthetic@example.invalid"));}QCOMPARE(inspections,1);window.navigate("history");window.navigate("destinations");QCOMPARE(inspections,1);auto* refresh=window.findChild<QPushButton*>("inspectProtonAccount");QVERIFY(refresh&&refresh->isVisible()&&refresh->isEnabled());QCOMPARE(folders->findData("__refresh__"),-1);refresh->click();QTRY_COMPARE(inspections,2);QVERIFY(!refresh->isEnabled());QCOMPARE(folders->currentData().toString(),QString("synthetic-folder"));QVERIFY(progress->isVisible());QVERIFY(account->text().contains("synthetic@example.invalid"));finish({"synthetic_network","Could not load folders",{}},{});QTRY_VERIFY(!progress->isVisible());QVERIFY(refresh->isEnabled());QVERIFY(account->text().contains("synthetic@example.invalid"));refresh->click();QTRY_COMPARE(inspections,3);finish({},{{"accountRootUid","synthetic-account"},{"folders",QJsonArray{QJsonObject{{"uid","synthetic-folder"},{"name","Renamed saved folder"}},QJsonObject{{"uid","synthetic-new-id"},{"name","New folder"}}}}});QTRY_VERIFY(folders->findText("New folder")>=0);QCOMPARE(folders->findText("Another folder"),-1);QCOMPARE(folders->currentData().toString(),QString("synthetic-folder"));QCOMPARE(folders->currentText(),QString("Renamed saved folder"));QVERIFY(account->text().contains("synthetic@example.invalid"));QTRY_COMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["folderName"].toString(),QString("Renamed saved folder"));QTRY_VERIFY(!window.settings()->saveViewState().contains("stateError"));QVERIFY(!validateUiState({{"schemaVersion",1},{"settings",window.settings()->saveViewState()}}));auto legacy=window.settings()->saveViewState();auto editors=legacy["profileEditors"].toArray();auto editor=editors[0].toObject();auto retained=editor["proton"].toObject();auto oldFolders=retained["folders"].toArray();oldFolders.append(QJsonObject{{"uid","__refresh__"},{"name",""},{"label","Refresh folders…"}});retained["folders"]=oldFolders;editor["proton"]=retained;editors[0]=editor;legacy["profileEditors"]=editors;MainWindow restored(fixture.session);QVERIFY(restored.settings()->restoreViewState(legacy));QCOMPARE(restored.findChild<QComboBox*>("protonFolder")->findData("__refresh__"),-1);QCOMPARE(restored.findChild<QComboBox*>("protonFolder")->currentData().toString(),QString("synthetic-folder"));QVERIFY(restored.findChild<QLabel*>("protonAccountStatus")->text().contains("synthetic@example.invalid"));QVERIFY(!QJsonDocument(service.document()).toJson().contains("synthetic@example.invalid"));QCOMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["folderUid"].toString(),QString("synthetic-folder"));
 }
 void connectedProtonAccountSurvivesRapidNavigation(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto proton=profiles.first().toObject();proton["id"]="synthetic-proton";proton["label"]="Synthetic Proton";proton["provider"]="proton-drive";auto account=ProtonProvider::defaults();account["accountRootUid"]="synthetic-account";account["folderUid"]="synthetic-folder";account["folderName"]="Saved folder";proton["settings"]=account;profiles.prepend(proton);doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.show();window.navigate("destinations");auto* list=window.findChild<QListWidget*>("destinationList");auto* label=window.findChild<QLabel*>("protonAccountStatus");auto* connect=window.findChild<QPushButton*>("connectProtonAccount");
  for(int repeat=0;repeat<5;++repeat){list->setCurrentRow(1);list->setCurrentRow(0);QVERIFY(label->isVisible());QVERIFY(label->text().startsWith("Connected"));QCOMPARE(connect->text(),QString("Reconnect account…"));window.navigate("history");window.navigate("destinations");QVERIFY(label->isVisible());QVERIFY(label->text().startsWith("Connected"));}
  QTest::qWait(650);QCOMPARE(service.document(),doc);QVERIFY(label->text().startsWith("Connected"));QVERIFY(!validateUiState({{"schemaVersion",1},{"settings",window.settings()->saveViewState()}}));
 }
 void protonConnectionCompletedWhileAwayIsShownOnReturn(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();const auto other=profiles.first();auto proton=other.toObject();proton["id"]="synthetic-proton";proton["label"]="Synthetic Proton";proton["provider"]="proton-drive";auto oldAccount=ProtonProvider::defaults();oldAccount["accountRootUid"]="synthetic-old-account";oldAccount["folderUid"]="synthetic-old-folder";proton["settings"]=oldAccount;profiles.prepend(proton);doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);JsonReply complete;int starts=0;fixture.setup.setProviderSetup([&](const QString& kind,const Destination&,const QJsonObject&,JsonReply done,auto){QCOMPARE(kind,QString("connectAccount"));++starts;complete=std::move(done);});MainWindow window(fixture.session);window.show();window.navigate("destinations");auto* list=window.findChild<QListWidget*>("destinationList");window.findChild<QPushButton*>("connectProtonAccount")->click();QTRY_VERIFY(bool(complete));
  for(int repeat=0;repeat<3;++repeat){list->setCurrentRow(1);list->setCurrentRow(0);}list->setCurrentRow(1);complete({},{{"accountRootUid","synthetic-new-account"},{"accountEmail","synthetic-new@example.invalid"},{"folders",QJsonArray{QJsonObject{{"uid","synthetic-folder"},{"name","Saved folder"}}}}});QTRY_VERIFY(window.settings()->saveViewState()["setupJobs"].toArray().isEmpty());QCOMPARE(service.document()["profiles"].toArray()[1],other);
  list->setCurrentRow(0);auto* label=window.findChild<QLabel*>("protonAccountStatus");QVERIFY(label->isVisible());QTRY_VERIFY(label->text().contains("synthetic-new@example.invalid"));QCOMPARE(window.findChild<QPushButton*>("connectProtonAccount")->text(),QString("Reconnect account…"));QTRY_COMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["accountRootUid"].toString(),QString("synthetic-new-account"));QCOMPARE(starts,1);QVERIFY(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["folderUid"].toString().isEmpty());QCOMPARE(service.document()["profiles"].toArray()[1],other);
 }
 void storedCredentialHintsNeverExportFullKeysOrOverwriteInputs(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider s3(secrets);auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();profile["credentials"]=QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}};profiles[0]=profile;doc["profiles"]=profiles;SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return s3.validateSettings(value);});QVERIFY(service.save(doc,{}));secrets.setSession("development-s3",{"AKIA-synthetic-full-access","synthetic-full-secret","synthetic-token"});UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);auto* hint=window.findChild<QLabel*>("storedCredentialHint");QTRY_VERIFY(hint->text().contains("AKIA"));QVERIFY(hint->text().contains("Secret key stored"));auto* access=window.findChild<QLineEdit*>("credentialAccessKey");QVERIFY(access->text().isEmpty());QVERIFY(access->placeholderText().contains("AKIA"));QVERIFY(!access->placeholderText().contains("full-access"));QCOMPARE(window.findChild<QLineEdit*>("credentialSecretKey")->echoMode(),QLineEdit::Password);const auto state=QJsonDocument(window.settings()->saveViewState()).toJson();QVERIFY(!state.contains("AKIA"));QVERIFY(!state.contains("synthetic-full-secret"));bool done=false;QJsonObject reply;fixture.session.request("setup.credentialHint",{{"profileId","development-s3"},{"expectedRevision",service.revision()}},this,[&](Error error,QJsonObject value){QVERIFY(!error);reply=value;done=true;});QTRY_VERIFY(done);const auto encoded=QJsonDocument(reply).toJson();QVERIFY(!encoded.contains("synthetic-full-access"));QVERIFY(!encoded.contains("synthetic-full-secret"));QVERIFY(!encoded.contains("synthetic-token"));
 }
 void connectionTestShowsItsResultOnTheDestinationPage(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});
  auto document=Settings::developmentTemplate();auto profiles=document["profiles"].toArray();auto second=profiles[0].toObject();
  second["id"]="second";second["label"]="Second destination";profiles.append(second);document["profiles"]=profiles;
  QVERIFY(service.save(document,{}));CredentialStore secrets;std::function<void(Error)> complete;
  UiSessionFixture fixture(service,secrets,[&](auto,auto,auto reply){complete=reply;});MainWindow window(fixture.session);
  window.navigate("destinations");window.show();QLabel* status=nullptr;
  for(auto* label:window.findChildren<QLabel*>())if(label->accessibleName()=="Destination status")status=label;
  QVERIFY(status);auto* test=window.findChild<QPushButton*>("testDestination");test->click();QTRY_VERIFY(bool(complete));
  complete({});QTRY_VERIFY(test->isEnabled());QTRY_VERIFY(status->text().contains("Connection successful"));
  QVERIFY(status->isVisible());QCOMPARE(status->property("feedbackTone").toString(),QString("success"));
  auto* destinations=window.findChild<QListWidget*>("destinationList");destinations->setCurrentRow(1);QVERIFY(status->text().isEmpty());destinations->setCurrentRow(0);
  complete={};test->click();QTRY_VERIFY(bool(complete));QVERIFY(status->text().contains("Testing connection"));
  complete({"s3_permission_denied","S3 denied the test upload. Check IAM and bucket policy permissions.",{}});
  QTRY_VERIFY(test->isEnabled());QTRY_VERIFY(status->text().contains("IAM and bucket policy"));
  QCOMPARE(status->property("feedbackTone").toString(),QString("error"));
 }
 void savedSecretCanBeShownHiddenAndNeverRetainedAsAnInput(){
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});
  auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();
  profile["credentials"]=QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}};profiles[0]=profile;
  auto second=profile;second["id"]="second";profiles.append(second);doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));
  secrets.setSession("development-s3",{"synthetic-access","synthetic-saved-secret",{}});
  UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* secret=window.findChild<QLineEdit*>("credentialSecretKey");QPushButton* show=nullptr;
  for(auto* button:window.findChildren<QPushButton*>())if(button->accessibleName()=="Show secret access key")show=button;
  QVERIFY(show);QVERIFY(secret->text().isEmpty());show->click();
  QTRY_COMPARE(secret->text(),QString("synthetic-saved-secret"));QCOMPARE(secret->echoMode(),QLineEdit::Normal);
  QVERIFY(secret->isReadOnly());QCOMPARE(show->text(),QString("Hide"));
  QVERIFY(!window.settings()->hasCredentialInputs());
  QVERIFY(window.settings()->autosaveText().isEmpty());
  QVERIFY(!QJsonDocument(window.saveViewState()).toJson().contains("synthetic-saved-secret"));
  QVERIFY(!QJsonDocument(window.settings()->saveViewState()).toJson().contains("synthetic-saved-secret"));
  QVERIFY(!QJsonDocument(service.document()).toJson().contains("synthetic-saved-secret"));
  show->click();QVERIFY(secret->text().isEmpty());QVERIFY(!secret->isReadOnly());QCOMPARE(secret->echoMode(),QLineEdit::Password);
  secret->setFocus();QTest::keyClicks(secret,"x");show->click();
  QTest::keyClick(secret,Qt::Key_Z,Qt::ControlModifier);QTest::keyClick(secret,Qt::Key_Z,Qt::ControlModifier);
  QVERIFY(secret->text().isEmpty());show->click();
  secret->setText("synthetic-typed-secret");show->click();QCOMPARE(secret->echoMode(),QLineEdit::Normal);
  show->click();QCOMPARE(secret->text(),QString("synthetic-typed-secret"));QCOMPARE(secret->echoMode(),QLineEdit::Password);
  secret->clear();show->click();QTRY_COMPARE(secret->text(),QString("synthetic-saved-secret"));
  auto* list=window.findChild<QListWidget*>("destinationList");list->setCurrentRow(1);
  QVERIFY(secret->text().isEmpty());QCOMPARE(show->text(),QString("Show"));list->setCurrentRow(0);QVERIFY(secret->text().isEmpty());
  fixture.client.replyDelayMs["setup.revealCredentialSecret"]=150;
  show->click();show->click();QTest::qWait(250);QVERIFY(secret->text().isEmpty());QCOMPARE(secret->echoMode(),QLineEdit::Password);
  show->click();list->setCurrentRow(1);list->setCurrentRow(0);QTest::qWait(250);QVERIFY(secret->text().isEmpty());
  show->click();QTRY_COMPARE(secret->text(),QString("synthetic-saved-secret"));window.settings()->clearCredentialInputs();
  QVERIFY(secret->text().isEmpty());QCOMPARE(show->text(),QString("Show"));QVERIFY(!show->isChecked());QVERIFY(!secret->isReadOnly());
  show->click();window.navigate("settings");QTest::qWait(250);QVERIFY(secret->text().isEmpty());window.navigate("destinations");
  secrets.forget("development-s3",profile["credentials"].toObject());show->click();
  QTRY_VERIFY(window.findChild<QLabel*>("settingsStatus")->text().contains("credential_setup_required"));
  QVERIFY(secret->text().isEmpty());QVERIFY(!show->isChecked());QVERIFY(!secret->isReadOnly());
 }
 void uiStateStrictlyRejectsUnexpectedAndSecretFields(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);auto state=QJsonObject{{"schemaVersion",1},{"settings",window.settings()->saveViewState()},{"main",window.saveViewState()}};QVERIFY(!validateUiState(state));auto main=state["main"].toObject();main["secretKey"]="synthetic-secret";state["main"]=main;auto error=validateUiState(state);QVERIFY(error);QVERIFY(!QJsonDocument(error.toJson()).toJson().contains("synthetic-secret"));
 }
 void acceptedSetupIdSurvivesAnUncertainInspectionReply(){
  QTemporaryDir d;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(d.filePath("config.json"),[&](auto,auto settings){return provider.validateSettings(settings);});QVERIFY(service.save(Settings::developmentTemplate(),{}));int starts=0;std::function<void(Error)> finish;UiSessionFixture fixture(service,secrets,[&](auto,auto,auto reply){++starts;finish=reply;});fixture.client.failOnce["setup.inspect"]={"client_unavailable","Synthetic lost inspection reply",{}};SettingsWindow window(fixture.session);window.findChild<QPushButton*>("testDestination")->click();QTRY_VERIFY(window.findChild<QLabel*>("settingsStatus")->text().contains("client_unavailable"));const auto saved=window.saveViewState();QCOMPARE(saved["setupJobs"].toArray().size(),1);SettingsWindow reopened(fixture.session);QVERIFY(reopened.restoreViewState(saved));QVERIFY(finish);finish({});QTRY_VERIFY(reopened.findChild<QLabel*>("settingsStatus")->text().contains("Connection successful"));QCOMPARE(starts,1);
 }
 void transientSetupInspectionRecoversWithoutReplayingAcceptedWork(){
  QTemporaryDir d;CredentialStore secrets;S3Provider provider(secrets);
  SettingsService service(d.filePath("config.json"),[&](auto,auto settings){return provider.validateSettings(settings);});
  QVERIFY(service.save(Settings::developmentTemplate(),{}));
  int starts=0;std::function<void(Error)> finish;
  UiSessionFixture fixture(service,secrets,[&](auto,auto,auto reply){++starts;finish=reply;});
  fixture.client.failOnce["setup.inspect"]={"client_unavailable","Synthetic lost inspection reply",{}};
  SettingsWindow window(fixture.session);
  auto* test=window.findChild<QPushButton*>("testDestination");
  auto* status=window.findChild<QLabel*>("settingsStatus");
  test->click();
  QTRY_VERIFY(status->text().contains("client_unavailable"));
  QCOMPARE(starts,1);QVERIFY(finish);QVERIFY(!test->isEnabled());
  window.findChild<QLineEdit*>("s3Bucket")->setText("unsaved-bucket");
  window.findChild<QLineEdit*>("credentialSecretKey")->setText("synthetic-unsubmitted-secret");
  finish({});
  QTRY_VERIFY_WITH_TIMEOUT(window.saveViewState()["setupJobs"].toArray().isEmpty(),2000);
  QVERIFY(status->text().contains("original destination settings"));QVERIFY(test->isEnabled());
  QCOMPARE(starts,1);
  QCOMPARE(window.findChild<QLineEdit*>("s3Bucket")->text(),QString("unsaved-bucket"));
  QCOMPARE(window.findChild<QLineEdit*>("credentialSecretKey")->text(),QString("synthetic-unsubmitted-secret"));
  QVERIFY(window.saveViewState()["dirty"].toBool());
  QVERIFY(!QJsonDocument(window.saveViewState()).toJson().contains("synthetic-unsubmitted-secret"));
 }
 void expiredSetupResultDoesNotStarveLaterAcceptedWork(){
  QTemporaryDir d;CredentialStore secrets;S3Provider provider(secrets);
  SettingsService service(d.filePath("config.json"),[&](auto,auto settings){return provider.validateSettings(settings);});
  QVERIFY(service.save(Settings::developmentTemplate(),{}));
  int starts=0;bool hold=false;std::function<void(Error)> finish;
  UiSessionFixture fixture(service,secrets,[&](auto,auto,auto reply){++starts;if(hold)finish=reply;else reply({});});
  auto start=[&]{QString id;fixture.setup.start("testDestination","development-s3",service.revision(),{},[&](Error error,QJsonObject data){QVERIFY(!error);id=data["setupId"].toString();});return id;};
  auto completed=[&](const QString& id){bool done=false;fixture.setup.inspect(id,[&](Error error,QJsonObject data){done=!error&&data["state"]=="completed";});return done;};
  QString expired;
  for(int i=0;i<65;++i){const auto id=start();QVERIFY(!id.isEmpty());if(i==0)expired=id;QTRY_VERIFY(completed(id));}
  Error missing;fixture.setup.inspect(expired,[&](Error error,QJsonObject){missing=error;});QCOMPARE(missing.code,QString("setup_not_found"));
  hold=true;const auto pending=start();QVERIFY(!pending.isEmpty());QVERIFY(finish);QCOMPARE(starts,66);
  SettingsWindow window(fixture.session);auto state=window.saveViewState();
  state["setupJobs"]=QJsonArray{QJsonObject{{"setupId",expired},{"kind","testDestination"},{"profileId","development-s3"}},QJsonObject{{"setupId",pending},{"kind","testDestination"},{"profileId","development-s3"}}};
  QVERIFY(window.restoreViewState(state));
  auto* test=window.findChild<QPushButton*>("testDestination");
  QTRY_VERIFY(window.findChild<QLabel*>("settingsStatus")->text().contains("setup_not_found"));
  QTRY_COMPARE_WITH_TIMEOUT(window.saveViewState()["setupJobs"].toArray().size(),1,2000);
  QCOMPARE(window.saveViewState()["setupJobs"].toArray().first().toObject()["setupId"].toString(),pending);
  QVERIFY(!test->isEnabled());
  finish({});
  QTRY_VERIFY_WITH_TIMEOUT(window.saveViewState()["setupJobs"].toArray().isEmpty(),2000);
  QVERIFY(test->isEnabled());QCOMPARE(starts,66);
 }
 void destinationTestButtonStaysDisabledUntilEveryAcceptedTestFinishes(){
  QTemporaryDir d;CredentialStore secrets;S3Provider provider(secrets);
  SettingsService service(d.filePath("config.json"),[&](auto,auto settings){return provider.validateSettings(settings);});
  auto document=Settings::developmentTemplate();auto profiles=document["profiles"].toArray();auto second=profiles.first().toObject();second["id"]="second-s3";profiles.append(second);document["profiles"]=profiles;QVERIFY(service.save(document,{}));
  int starts=0;QHash<QString,std::function<void(Error)>> finish;
  UiSessionFixture fixture(service,secrets,[&](const QString& profile,auto,auto reply){++starts;finish[profile]=reply;});
  QJsonArray jobs;
  for(const auto& profile:QStringList{"development-s3","second-s3"})fixture.setup.start("testDestination",profile,service.revision(),{},[&](Error error,QJsonObject data){QVERIFY(!error);jobs.append(QJsonObject{{"setupId",data["setupId"]},{"kind","testDestination"},{"profileId",profile}});});
  QCOMPARE(starts,2);QCOMPARE(finish.size(),2);
  finish["development-s3"]({});
  SettingsWindow window(fixture.session);auto state=window.saveViewState();state["setupJobs"]=jobs;QVERIFY(window.restoreViewState(state));
  QTRY_COMPARE_WITH_TIMEOUT(window.saveViewState()["setupJobs"].toArray().size(),1,2000);
  QVERIFY(!window.findChild<QPushButton*>("testDestination")->isEnabled());
  finish["second-s3"]({});
  QTRY_VERIFY_WITH_TIMEOUT(window.saveViewState()["setupJobs"].toArray().isEmpty(),2000);
  QVERIFY(window.findChild<QPushButton*>("testDestination")->isEnabled());QCOMPARE(starts,2);
 }
 void shortcutRecordsOnReleaseAndEscapePreservesDraft(){
  ShortcutEditor editor;editor.setRecordingControl([](bool,auto reply){reply({});});editor.setActions(Settings::defaults()["actions"].toObject());editor.show();
  auto* record=editor.findChild<QPushButton*>("recordRegionShortcut");QVERIFY(record);
  auto* input=editor.editors().value("region");const auto previous=input->text();
  record->click();QTest::keyPress(input,Qt::Key_Control);QTest::keyPress(input,Qt::Key_1,Qt::ControlModifier);
  QCOMPARE(input->text(),previous);
  QKeyEvent releaseOne(QEvent::KeyRelease,Qt::Key_1,Qt::ControlModifier);QApplication::sendEvent(input,&releaseOne);QCOMPARE(input->text(),previous);
  QTest::keyRelease(input,Qt::Key_Control);QCOMPARE(input->text(),QString("Ctrl+1"));
  record->click();QTest::keyPress(input,Qt::Key_Control);QTest::keyPress(input,Qt::Key_2,Qt::ControlModifier);
  QTest::keyClick(input,Qt::Key_Escape,Qt::ControlModifier);QTest::keyRelease(input,Qt::Key_2,Qt::ControlModifier);QTest::keyRelease(input,Qt::Key_Control);
  QCOMPARE(input->text(),QString("Ctrl+1"));
 }
 void shortcutRecordingNormalizesEveryActionAndIgnoresRepeat(){
  ShortcutEditor editor;editor.setRecordingControl([](bool,auto reply){reply({});});editor.setActions(Settings::defaults()["actions"].toObject());editor.show();
  for(const auto& action:Settings::invocableActionNames()){
   const auto title=action.left(1).toUpper()+action.mid(1);auto* input=editor.editors().value(action);QVERIFY2(input,qPrintable(action));const auto old=input->text();
   editor.findChild<QPushButton*>("record"+title+"Shortcut")->click();
   QTest::keyPress(input,Qt::Key_Control);QTest::keyRelease(input,Qt::Key_Control);QCOMPARE(input->text(),old);
   QTest::keyPress(input,Qt::Key_Control);QTest::keyPress(input,Qt::Key_Shift,Qt::ControlModifier);QTest::keyPress(input,Qt::Key_F8,Qt::ControlModifier|Qt::ShiftModifier);
   QKeyEvent repeatedPress(QEvent::KeyPress,Qt::Key_F8,Qt::ControlModifier|Qt::ShiftModifier,{},true);QApplication::sendEvent(input,&repeatedPress);
   QKeyEvent repeatedRelease(QEvent::KeyRelease,Qt::Key_F8,Qt::ControlModifier|Qt::ShiftModifier,{},true);QApplication::sendEvent(input,&repeatedRelease);QCOMPARE(input->text(),old);
   QTest::keyRelease(input,Qt::Key_Control,Qt::ShiftModifier);QTest::keyRelease(input,Qt::Key_Shift);QCOMPARE(input->text(),old);
   QTest::keyRelease(input,Qt::Key_F8);QCOMPARE(input->text(),QString("Ctrl+Shift+F8"));
   QCOMPARE(editor.actions({})[action].toObject()["shortcut"].toString(),QString("Ctrl+Shift+F8"));
   QTest::keyClick(input,Qt::Key_Tab);QVERIFY(QApplication::focusWidget()!=input);
   editor.findChild<QPushButton*>("clear"+title+"Shortcut")->click();QVERIFY(input->text().isEmpty());
  }
 }
 void shortcutRecordingTracksPhysicalKeysAcrossModifierRelease(){
  ShortcutEditor editor;editor.setRecordingControl([](bool,auto reply){reply({});});editor.setActions(Settings::defaults()["actions"].toObject());editor.show();
  auto* record=editor.findChild<QPushButton*>("recordRegionShortcut");auto* input=editor.editors().value("region");const auto old=input->text();record->click();
  const auto send=[&](QEvent::Type type,int key,Qt::KeyboardModifiers modifiers,quint32 scan){QKeyEvent event(type,key,modifiers,scan,0,0);QApplication::sendEvent(input,&event);};
  send(QEvent::KeyPress,Qt::Key_Shift,Qt::ShiftModifier,50);send(QEvent::KeyPress,Qt::Key_Exclam,Qt::ShiftModifier,10);
  send(QEvent::KeyRelease,Qt::Key_Shift,Qt::NoModifier,50);QCOMPARE(input->text(),old);
  // X11 can report a repeated key with a new symbol when modifiers change.
  send(QEvent::KeyPress,Qt::Key_1,Qt::NoModifier,10);QCOMPARE(record->text(),QString("Cancel"));
  send(QEvent::KeyRelease,Qt::Key_1,Qt::NoModifier,10);QCOMPARE(input->text(),QString("Shift+!"));QCOMPARE(record->text(),QString("Record shortcut"));
  record->click();send(QEvent::KeyPress,Qt::Key_1,Qt::KeypadModifier,87);send(QEvent::KeyRelease,Qt::Key_1,Qt::KeypadModifier,87);
  QCOMPARE(input->text(),QKeySequence(QKeyCombination(Qt::KeypadModifier,Qt::Key_1)).toString(QKeySequence::PortableText));QCOMPARE(record->text(),QString("Record shortcut"));
  for(const auto modifierFirst:{false,true}){
   record->click();send(QEvent::KeyPress,Qt::Key_Control,Qt::ControlModifier,37);send(QEvent::KeyPress,Qt::Key_2,Qt::ControlModifier|Qt::KeypadModifier,88);
   if(modifierFirst){send(QEvent::KeyRelease,Qt::Key_Control,Qt::NoModifier,37);send(QEvent::KeyRelease,Qt::Key_2,Qt::KeypadModifier,88);}
   else {send(QEvent::KeyRelease,Qt::Key_2,Qt::ControlModifier|Qt::KeypadModifier,88);send(QEvent::KeyRelease,Qt::Key_Control,Qt::NoModifier,37);}
   QCOMPARE(input->text(),QKeySequence(QKeyCombination(Qt::ControlModifier|Qt::KeypadModifier,Qt::Key_2)).toString(QKeySequence::PortableText));QCOMPARE(record->text(),QString("Record shortcut"));
  }
 }
 void shortcutRecordingCancellationAndFailuresReleaseSuspension(){
  QList<bool> transitions;std::function<void(Error)> ready;ShortcutEditor editor;
  editor.setRecordingControl([&](bool active,auto reply){transitions.append(active);if(active)ready=reply;else reply({});});
  editor.setActions(Settings::defaults()["actions"].toObject());editor.show();QTest::qWait(1);
  auto* input=editor.editors().value("region");auto* record=editor.findChild<QPushButton*>("recordRegionShortcut");const auto old=input->text();
  record->click();QTest::keyClick(input,Qt::Key_1,Qt::ControlModifier);QCOMPARE(input->text(),old);
  QTest::keyClick(input,Qt::Key_Escape);QCOMPARE(transitions,QList<bool>({true,false}));ready({});QCOMPARE(record->text(),QString("Record shortcut"));
  record->click();ready({"shortcut_unavailable","Synthetic suspension failure",{}});QCOMPARE(transitions,QList<bool>({true,false,true,false}));QCOMPARE(input->text(),old);
  record->click();ready({});QTest::keyPress(input,Qt::Key_Control);QTest::mouseClick(record,Qt::LeftButton);QCOMPARE(record->text(),QString("Record shortcut"));QCOMPARE(transitions.last(),false);
  record->click();ready({});editor.editors().value("window")->setFocus();QCOMPARE(transitions.last(),false);QCOMPARE(input->text(),old);
  record->click();ready({});QEvent deactivate(QEvent::WindowDeactivate);QApplication::sendEvent(&editor,&deactivate);QCOMPARE(transitions.last(),false);
  record->click();ready({});QKeyEvent unsupported(QEvent::KeyPress,Qt::Key_unknown,Qt::NoModifier);QApplication::sendEvent(input,&unsupported);QCOMPARE(transitions.last(),false);QVERIFY(editor.findChild<QLabel*>("shortcutRecordingStatus")->text().contains("Unsupported"));
  record->click();ready({});editor.hide();QCOMPARE(transitions.last(),false);QCOMPARE(input->text(),old);
 }
 void shortcutRecordingDistinguishesSupersededRestoreFromFailure(){
  std::function<void(Error)> restored;ShortcutEditor editor;
  editor.setRecordingControl([&](bool active,auto reply){if(active)reply({});else restored=std::move(reply);});
  editor.setActions(Settings::defaults()["actions"].toObject());editor.show();QTest::qWait(1);
  auto* record=editor.findChild<QPushButton*>("recordRegionShortcut");auto* input=editor.editors().value("region");auto* status=editor.findChild<QLabel*>("shortcutRecordingStatus");
  record->click();QTest::keyClick(input,Qt::Key_1,Qt::ControlModifier);const auto recorded=status->text();
  restored({"shortcut_recording_interrupted","Global shortcut configuration changed.",{}});QCOMPARE(status->text(),recorded);QCOMPARE(input->text(),QString("Ctrl+1"));
  record->click();QTest::keyClick(input,Qt::Key_Escape);restored({"shortcut_restore_failed","Synthetic restore failure",{}});QVERIFY(status->text().contains("Could not restore shortcuts"));QCOMPARE(input->text(),QString("Ctrl+1"));
 }
 // #305: "This build's channel" says which channel it resolves to, from the
 // build's channel in update.status; a build that never checks keeps the plain label.
 void updateChannelNamesWhatThisBuildFollows(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);QJsonObject update;
  fixture.server.setUiDiagnostics([&]{auto data=fixture.operations().diagnostics();data["update"]=update;return data;});
  MainWindow window(fixture.session);window.navigate("settings");window.show();QTRY_VERIFY(fixture.session.ready());
  auto* channel=window.findChild<QComboBox*>("updateChannel");QVERIFY(channel);QCOMPARE(channel->itemData(0).toString(),QString("build"));
  auto apply=[&](bool supported,const QJsonValue& buildChannel,const QString& setting,const QString& reason){
   update={{"supported",supported},{"build",QJsonObject{{"version","0.2.0-synthetic"},{"channel",buildChannel},{"supported",supported},{"reason",reason.isEmpty()?QJsonValue():QJsonValue(reason)}}},
    {"check",true},{"channelSetting",setting},{"channel",setting=="build"?buildChannel:QJsonValue(setting)},{"checking",false},{"result",QJsonObject{}},{"error",QJsonObject{}},
    {"checkedAt",QJsonValue()},{"nextCheckAt",QJsonValue()},{"reason",reason.isEmpty()?QJsonValue():QJsonValue(reason)}};
   fixture.session.refreshSnapshot();
  };
  apply(true,"stable","build",{});QTRY_COMPARE(channel->itemText(0),QString("This build's channel (stable)"));
  QVERIFY(channel->isVisibleTo(window.settings()));QCOMPARE(channel->currentText(),QString("This build's channel (stable)"));
  apply(true,"nightly","build",{});QTRY_COMPARE(channel->itemText(0),QString("This build's channel (nightly)"));
  // Watching the other channel doesn't change what this build follows.
  apply(true,"nightly","stable",{});QTRY_VERIFY(window.findChild<QLabel*>("updateStatus")->text().contains("Not checked yet"));QCOMPARE(channel->itemText(0),QString("This build's channel (nightly)"));
  QCOMPARE(channel->itemText(1),QString("Stable"));QCOMPARE(channel->itemText(2),QString("Nightly"));
  // No channel: a development build, and a Flatpak (tagged, but it never checks).
  apply(false,QJsonValue(),"build","development_build");QTRY_COMPARE(channel->itemText(0),QString("This build's channel"));QVERIFY(!channel->isVisibleTo(window.settings()));
  apply(true,"stable","build",{});QTRY_COMPARE(channel->itemText(0),QString("This build's channel (stable)"));
  apply(false,"stable","build","flatpak");QTRY_COMPARE(channel->itemText(0),QString("This build's channel"));
 }
 void railShowsTheResidentVersionAndOpensItsGitHubPage(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();QTRY_VERIFY(fixture.session.ready());
  auto* rail=window.findChild<QWidget*>("mainRail");auto* label=window.findChild<QLabel*>("appVersion");auto* link=window.findChild<QPushButton*>("appVersionLink");QVERIFY(rail&&label&&link);QVERIFY(rail->isAncestorOf(label)&&rail->isAncestorOf(link));
  // The real build identity: the full compiled version, as diagnostics and About report it.
  const auto build=QJsonDocument::fromJson(buildManifestJson()).object();QCOMPARE(fixture.session.build(),build);const auto current=releaseLink(build);QWidget* shown=current.url.isEmpty()?static_cast<QWidget*>(label):link;
  QTRY_VERIFY(shown->isVisible());QCOMPARE(shown->property("text").toString().remove(QChar(0x200B)),QString::fromLatin1(buildVersion()));QCOMPARE(shown->accessibleName(),"put.gd version "+QString::fromLatin1(buildVersion()));
  QVERIFY2(shown->mapTo(rail,QPoint(0,shown->height())).y()>rail->height()-60,"The version sits at the bottom of the rail");
  openedUrls_.clear();QDesktopServices::setUrlHandler("https",this,"recordProtonUrl");auto cleanup=qScopeGuard([]{QDesktopServices::unsetUrlHandler("https");});
  QJsonObject snapshot;fixture.server.dispatch({{"schemaVersion",1},{"requestId","version"},{"session",fixture.server.socketPath()},{"method","ui.snapshot"},{"params",QJsonObject{}}},[&](Error e,QJsonObject data){QVERIFY(!e);snapshot=data;});QTRY_VERIFY(!snapshot.isEmpty());
  auto apply=[&](const QString& version,bool dirty){auto injected=build;injected["version"]=version;injected["dirty"]=dirty;injected["gitCommit"]="528fe444b0cb0123456789abcdef0123456789ab";auto data=snapshot;data["build"]=injected;fixture.session.applySnapshot(data);};
  apply("0.1.1-nightly.430",false);QTRY_VERIFY(link->isVisible());QVERIFY(!label->isVisible());QCOMPARE(link->text(),QString("0.1.1-nightly.430"));QCOMPARE(link->accessibleName(),QString("put.gd version 0.1.1-nightly.430"));QVERIFY(link->focusPolicy()&Qt::TabFocus);
  link->setFocus();QTest::keyClick(link,Qt::Key_Space);QTRY_COMPARE(openedUrls_.size(),1);QCOMPARE(openedUrls_.first(),QUrl("https://github.com/patrick-hudson/put.gd-desktop/releases/tag/v0.1.1-nightly.430"));
  // A development build has no GitHub release, even when its commit is pushed: plain text.
  const QString clean="0.1.1-dev.430+g528fe444b0cb";apply(clean,false);QTRY_VERIFY(label->isVisible());QVERIFY(!link->isVisible());QCOMPARE(label->text().remove(QChar(0x200B)),clean);QVERIFY2(label->heightForWidth(label->width())<=label->fontMetrics().lineSpacing()+2,"A clean development version fits on one line");
  // Dirty development builds have no public page: the full string stays readable, unlinked, wrapping after "+" if needed.
  const QString dirty="0.1.1-dev.430+g528fe444b0cb.dirty";apply(dirty,true);QTRY_VERIFY(label->isVisible());QVERIFY(!link->isVisible());QCOMPARE(label->text().remove(QChar(0x200B)),dirty);QCOMPARE(label->accessibleName(),"put.gd version "+dirty);
  QTRY_VERIFY2(label->width()<=rail->contentsRect().width()-28&&label->height()>=label->heightForWidth(label->width()),"The longest development version fits in the rail");QCOMPARE(openedUrls_.size(),1);
 }
};
QTEST_MAIN(SettingsGui)
#include "test_settings.moc"
