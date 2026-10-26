#include "ui/MainWindow.h"
#include "core/BuildInfo.h"
#include "core/ReleaseLink.h"
#include "ui/VisualStyle.h"
#include "ui/RedesignTheme.h"
#include <QVBoxLayout>
#include <QProgressBar>
#include <QAbstractItemView>
#include <QScrollBar>
#include "ui/SettingsWindow.h"
#include "ui/FileDialog.h"
#include <QFileDialog>
#include "ui/ShortcutEditor.h"
#include "ui/ShortcutKeyboard.h"
#include "ui/ViewState.h"
#include "fixtures/UiSessionFixture.h"
#include "app/DiagnosticsBundle.h"
#include "core/SystemUpdates.h"
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
#include <QClipboard>
#include <QJsonArray>
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
#include "platform/HostOpen.h"
#include <QScopeGuard>
#include <QAction>
#include "fixtures/Waits.h"
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
  PUTGD_TRY_COMPARE(profileIds(service.document()),QStringList({"development-s3","synthetic-third","synthetic-alternate"}));
  PUTGD_TRY_COMPARE(service.document()["profiles"].toArray()[2].toObject()["label"].toString(),QString("Edited alternate"));
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
  PUTGD_TRY_COMPARE(profileIds(service.document()),QStringList({"development-s3","synthetic-alternate","synthetic-third"}));
  // The UI-driving path selects by ID and moves, reporting the new order.
  QJsonObject result;QVERIFY(!window.settings()->moveSelectedDestination(-1,"synthetic-third",result));
  QCOMPARE(result["selected"].toString(),QString("synthetic-third"));QCOMPARE(result["order"].toArray(),QJsonArray({"development-s3","synthetic-third","synthetic-alternate"}));
  PUTGD_TRY_COMPARE(profileIds(service.document()),QStringList({"development-s3","synthetic-third","synthetic-alternate"}));
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
  auto* show=dialog->findChild<QPushButton*>("showPackageInFolder");QVERIFY(show->isVisibleTo(dialog));show->click();PUTGD_TRY_COMPARE(revealed,QStringList{path});
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
  // A working destination: checked by default, named, with its warning; the upload links and copies.
  const int uploadsBefore=provider->uploads.load();
  command({{"action","openPackage"}});PUTGD_TRY_VERIFY(package()["uploadTarget"].toObject()["available"].toBool());
  QVERIFY(upload->isVisibleTo(dialog));QVERIFY(upload->isChecked());QVERIFY2(upload->text().contains("Synthetic opaque provider"),qPrintable(upload->text()));
  QVERIFY(note->text().contains("anyone with the link"));QCOMPARE(dialog->findChild<QPushButton*>("savePackage")->text(),QString("Save and upload"));QVERIFY(package()["upload"].toBool());
  auto [started,startedState]=command({{"action","savePackage"}});QVERIFY2(!started,qPrintable(started.message));
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(package().contains("saved"),20000);
  const auto saved=package()["saved"].toObject();QVERIFY2(saved["link"].toString().startsWith("https://"),QJsonDocument(saved).toJson().constData());
  QVERIFY(saved["linkCopied"].toBool());QCOMPARE(source.copied.text,saved["link"].toString());QCOMPARE(provider->uploads.load(),uploadsBefore+1);QVERIFY(QFileInfo(saved["path"].toString()).isFile());
  auto* result=dialog->findChild<QLabel*>("packageResult");QVERIFY2(result->text().contains("Uploaded to Synthetic opaque provider. Link copied: https://"),qPrintable(result->text()));
  command({{"action","closePackage"}});
  // An agent can leave it unticked: the package stays local.
  command({{"action","openPackage"}});PUTGD_TRY_VERIFY(package()["uploadTarget"].toObject()["available"].toBool());
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
   if(!putgd::test::waitFor([&]{return done;},5000))qWarning("ui.fileDialog timed out");return {error,result};};
  QCOMPARE(call({{"dialog","editImage"},{"action","open"},{"field","root"}}).first.code,QString("invalid_request"));
  QCOMPARE(call({{"dialog","outputFolder"},{"action","open"},{"field","music"}}).first.code,QString("invalid_request"));
  QCOMPARE(call({{"dialog","shell"},{"action","open"}}).first.code,QString("invalid_request"));
  QCOMPARE(call({{"dialog","diagnosticsFolder"},{"action","open"}}).first.code,QString("diagnostics_dialog_closed"));
  // Browse… for Videos: the chosen folder lands in its field and autosaves.
  QTemporaryDir videos;QVERIFY(videos.isValid());
  auto [error,state]=call({{"dialog","outputFolder"},{"action","open"},{"field","videos"}});QVERIFY2(!error,qPrintable(error.message));
  QVERIFY(state["open"].toBool());QCOMPARE(state["backend"].toString(),QString("fallback"));QCOMPARE(state["mode"].toString(),QString("folder"));QCOMPARE(state["title"].toString(),QString("Choose output folder"));
  QCOMPARE(call({{"dialog","outputFolder"},{"action","open"},{"field","images"}}).first.code,QString("file_dialog_open"));
  QFileDialog* chooser=nullptr;PUTGD_TRY_VERIFY((chooser=window.findChild<QFileDialog*>("putgdFileDialog"))&&chooser->isVisible());
  QCOMPARE(chooser->fileMode(),QFileDialog::Directory);
  chooser->setDirectory(videos.path());chooser->selectFile(videos.path());QMetaObject::invokeMethod(chooser,"accept");
  PUTGD_TRY_COMPARE(window.findChild<QLineEdit*>("folderVideos")->text(),videos.path());
  PUTGD_TRY_COMPARE(service.snapshot().archive.folders.value("videos"),videos.path());
  QCOMPARE(call({{"dialog","outputFolder"},{"action","inspect"}}).second["last"].toObject()["status"].toString(),QString("chosen"));
  // Edit an image file…: opened, then cancelled through the command.
  auto opened=call({{"dialog","editImage"},{"action","open"}});QVERIFY(opened.second["open"].toBool());QCOMPARE(opened.second["mode"].toString(),QString("open"));
  PUTGD_TRY_VERIFY((chooser=window.findChild<QFileDialog*>("putgdFileDialog"))&&chooser->isVisible());QCOMPARE(chooser->nameFilters(),QStringList{"Images (*.png *.jpg *.jpeg)"});
  auto cancelled=call({{"dialog","editImage"},{"action","cancel"}});QVERIFY(!cancelled.second["open"].toBool());QCOMPARE(cancelled.second["last"].toObject()["status"].toString(),QString("cancelled"));
 }
 void autoLimitsShowWhatTheyResolveTo(){
  constexpr qint64 gib=1LL<<30;ArtifactStore::setDiskForTesting(60*gib,100*gib);auto resetDisk=qScopeGuard([]{ArtifactStore::setDiskForTesting(-1,-1);});
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();
  auto* file=window.findChild<QLabel*>("maxFileBytesResolved");auto* batch=window.findChild<QLabel*>("maxBatchBytesResolved");auto* waiting=window.findChild<QLabel*>("pendingArtifactBytesResolved");
  QVERIFY(file&&batch&&waiting);
  PUTGD_TRY_COMPARE(file->text(),QString("· no limit (keeps 4 GiB free)"));QCOMPARE(batch->text(),file->text());QCOMPARE(waiting->text(),file->text());
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
  PUTGD_TRY_VERIFY2(status->text().contains("config_conflict"),qPrintable(status->text()));QVERIFY(status->text().contains("previous destination order is restored"));
  QCOMPARE(rowLabels(list),QStringList({"put.gd development destination","Synthetic alternate","Synthetic third"}));QCOMPARE(list->currentRow(),0);
  QCOMPARE(window.findChild<QLineEdit*>("destinationId")->text(),QString("development-s3"));
  QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),external);
  QVERIFY(window.settings()->autosaveText().contains("changed elsewhere"));
  // Further moves wait for a reload instead of saving over the other writer.
  QJsonObject result;QCOMPARE(window.settings()->moveSelectedDestination(1,{},result).code,QString("config_conflict"));
  window.findChild<QPushButton*>("reloadSettings")->click();PUTGD_TRY_VERIFY(!window.settings()->autosaveText().contains("changed elsewhere"));
  QVERIFY(!window.settings()->moveSelectedDestination(1,"development-s3",result));
  PUTGD_TRY_COMPARE(profileIds(service.document()),QStringList({"synthetic-alternate","development-s3","synthetic-third"}));QVERIFY(!service.snapshot().notifications);
 }
 void captureDefaultAndExplicitExceptionPersistAcrossReopen(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(1);window.show();
  QVERIFY(!window.findChild<QComboBox*>("defaultCaptureOutput"));QVERIFY(!window.findChild<QComboBox*>("destinationClipboard"));auto* destination=window.findChild<QComboBox*>("defaultCaptureDestination");QVERIFY(destination);const auto profileId=destination->currentData().toString();QVERIFY(!profileId.isEmpty());auto* local=window.findChild<QCheckBox*>("keepUploadCopies");QVERIFY(local);local->setChecked(true);PUTGD_TRY_VERIFY(service.snapshot().archive.keepUploads);QCOMPARE(service.snapshot().actions["region"].profileId,profileId);
  auto* exceptions=window.findChild<QToolButton*>("captureExceptionsToggle");QVERIFY(exceptions);QVERIFY(!exceptions->isChecked());exceptions->click();auto* inherit=window.findChild<QCheckBox*>("useDefaultClipboard");QVERIFY(inherit);inherit->setChecked(false);auto* clipboard=window.findChild<QComboBox*>("profileClipboard");clipboard->setCurrentIndex(clipboard->findData(QString{}));
  PUTGD_TRY_VERIFY(service.document().contains("captureDefaults"));PUTGD_TRY_COMPARE(service.snapshot().actions["clipboard"].destination,QString("local"));
  destination->setCurrentIndex(destination->findData(QString{}));PUTGD_TRY_COMPARE(service.snapshot().actions["region"].destination,QString("local"));QVERIFY(service.snapshot().archive.keepUploads);destination->setCurrentIndex(destination->findData(profileId));PUTGD_TRY_COMPARE(service.snapshot().actions["region"].destination,QString("upload"));QCOMPARE(service.snapshot().actions["clipboard"].destination,QString("local"));
  local->setChecked(false);PUTGD_TRY_VERIFY(!service.snapshot().archive.keepUploads);QCOMPARE(service.snapshot().actions["region"].profileId,profileId);QCOMPARE(service.snapshot().actions["clipboard"].destination,QString("local"));PUTGD_TRY_VERIFY(window.findChild<QLabel*>("captureStatusClipboard")->text().contains("will fail"));
  MainWindow reopened(fixture.session);reopened.navigate("settings");QVERIFY(reopened.findChild<QCheckBox*>("useDefaultRegion")->isChecked());QVERIFY(!reopened.findChild<QCheckBox*>("useDefaultClipboard")->isChecked());QVERIFY(!validateUiState({{"schemaVersion",1},{"settings",window.settings()->saveViewState()}}));
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
  PUTGD_TRY_COMPARE(service.document()["captureDefaults"].toObject()["profileId"].toString(),QString("development-s3"));PUTGD_TRY_COMPARE(service.document()["captureDefaults"].toObject()["publicationMode"].toString(),QString("default"));
  // Unchecking starts from the inherited values and saves an explicit exception.
  window.findChild<QCheckBox*>("useDefaultRegion")->setChecked(false);auto* region=window.findChild<QComboBox*>("profileRegion");auto* regionSharing=window.findChild<QComboBox*>("publicationRegion");
  QVERIFY(region->isEnabled());QVERIFY(regionSharing->isEnabled());QCOMPARE(region->currentData().toString(),QString("development-s3"));QCOMPARE(regionSharing->currentData().toString(),QString("default"));QCOMPARE(regionSharing->currentText(),QString("Use destination default"));QCOMPARE(region->accessibleName(),QString("Region capture destination"));
  const auto route=routing()["region"].toObject();QVERIFY(!route["inherited"].toBool());QVERIFY(route["destinationEnabled"].toBool());QVERIFY(route["sharingEnabled"].toBool());QCOMPARE(route["effectivePublicationMode"].toString(),QString("share"));
  PUTGD_TRY_COMPARE(service.document()["actions"].toObject()["region"].toObject()["destination"].toString(),QString("upload"));QCOMPARE(service.document()["actions"].toObject()["region"].toObject()["profileId"].toString(),QString("development-s3"));
  for(const auto& name:Settings::actionNames())if(name!="region")QCOMPARE(service.document()["actions"].toObject()[name].toObject()["destination"].toString(),QString("default"));
  MainWindow reopened(fixture.session);reopened.settings();QVERIFY(!reopened.findChild<QCheckBox*>("useDefaultRegion")->isChecked());QVERIFY(reopened.findChild<QComboBox*>("profileRegion")->isEnabled());QVERIFY(reopened.findChild<QCheckBox*>("useDefaultEdit")->isChecked());QVERIFY(!reopened.findChild<QComboBox*>("profileEdit")->isEnabled());QCOMPARE(reopened.findChild<QComboBox*>("publicationEdit")->currentText(),QString("Share and copy a link (destination default)"));
  // The readable projection is strictly typed.
  auto state=window.settings()->saveViewState();auto routes=state["captureRouting"].toObject();auto edit=routes["edit"].toObject();edit["destinationEnabled"]="false";routes["edit"]=edit;state["captureRouting"]=routes;QVERIFY(validateUiState({{"schemaVersion",1},{"settings",state}}));
  edit["destinationEnabled"]=false;edit["draft"]="unexpected";routes["edit"]=edit;state["captureRouting"]=routes;QVERIFY(validateUiState({{"schemaVersion",1},{"settings",state}}));
 }
 void firstLaunchHasNoDestinationOrAutomaticConfigurationWrite(){
  QTemporaryDir dir;
  const auto path=dir.filePath("config.json");
  SettingsService service(path);
  const auto original=service.document();
  CredentialStore secrets;
  UiSessionFixture fixture(service,secrets);
  MainWindow window(fixture.session);
  window.navigate("destinations");window.show();
  QCOMPARE(window.findChild<QListWidget*>("destinationList")->count(),0);
  QVERIFY(window.findChild<QLabel*>("destinationsEmpty")->isVisible());
  QVERIFY(!window.findChild<QWidget*>("destinationScroll")->isVisible());
  QCOMPARE(window.findChild<QComboBox*>("storageProvider")->currentIndex(),-1);
  auto* destination=window.findChild<QComboBox*>("defaultCaptureDestination");
  QCOMPARE(destination->count(),1);
  QVERIFY(destination->currentData().toString().isEmpty());
  const auto state=window.settings()->saveViewState();
  QVERIFY(!state.contains("stateError"));
  QVERIFY(state["draft"].toObject()["profiles"].toArray().isEmpty());
  QCOMPARE(state["draft"].toObject()["actions"],original["actions"]);
  QVERIFY(!state["initialProfile"].toBool());
  MainWindow reopened(fixture.session);
  QVERIFY(reopened.settings()->restoreViewState(state));
  QCOMPARE(reopened.findChild<QListWidget*>("destinationList")->count(),0);
  fixture.session.invalidate();PUTGD_VERIFY_STEADY(service.revision().isEmpty(),650);
  QCOMPARE(service.document(),original);
  QVERIFY(service.revision().isEmpty());
  QVERIFY(!QFile::exists(path));
 }
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
 void firstLaunchLocalCopiesNeedNoDestination(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));const auto actions=service.document()["actions"];CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();auto* destination=window.findChild<QComboBox*>("defaultCaptureDestination");QCOMPARE(destination->count(),1);QVERIFY(destination->currentData().toString().isEmpty());window.findChild<QCheckBox*>("keepUploadCopies")->setChecked(true);PUTGD_TRY_VERIFY(!service.revision().isEmpty());PUTGD_TRY_VERIFY(service.snapshot().archive.keepUploads);QVERIFY(service.document()["profiles"].toArray().isEmpty());QCOMPARE(service.document()["actions"],actions);PUTGD_TRY_VERIFY(window.findChild<QLabel*>("autosaveStatus")->text().isEmpty());
  MainWindow reopened(fixture.session);reopened.settings();QVERIFY(reopened.findChild<QCheckBox*>("keepUploadCopies")->isChecked());QVERIFY(reopened.findChild<QComboBox*>("defaultCaptureDestination")->currentData().toString().isEmpty());QVERIFY(reopened.findChild<QLabel*>("autosaveStatus")->text().isEmpty());
  SettingsService restarted(service.path());QVERIFY(restarted.reload());QVERIFY(restarted.document()["profiles"].toArray().isEmpty());QCOMPARE(restarted.document()["actions"],actions);
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
  fixture.client.failOnce["settings.save"]={"client_unavailable","Synthetic lost reply",{}};field->setText("251");PUTGD_TRY_VERIFY(status->text().contains("Couldn’t save"));QCOMPARE(service.snapshot().limits.maxFiles,250);window.findChild<QPushButton*>("retryAutosave")->click();PUTGD_TRY_COMPARE(service.snapshot().limits.maxFiles,251);PUTGD_TRY_VERIFY(status->text().isEmpty());
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
   combo->setCurrentIndex(startingIndex);combo->showPopup();auto* view=combo->view();PUTGD_TRY_VERIFY(view->isVisible());
   const auto viewport=view->viewport()->rect();
   QVERIFY2(viewport.contains(view->visualRect(combo->model()->index(0,0))),"First option is clipped");
   QVERIFY2(viewport.contains(view->visualRect(combo->model()->index(count-1,0))),"Last option is clipped");
   QCOMPARE(view->verticalScrollBar()->maximum(),0);
   const int target=startingIndex==0?count-1:0;
   QTest::mouseClick(view->viewport(),Qt::LeftButton,Qt::NoModifier,view->visualRect(combo->model()->index(target,0)).center());
   PUTGD_TRY_COMPARE(combo->currentIndex(),target);PUTGD_TRY_VERIFY(!view->isVisible());
   combo->showPopup();QTest::keyClick(view,Qt::Key_Home);QTest::keyClick(view,Qt::Key_Return);
   PUTGD_TRY_COMPARE(combo->currentIndex(),0);PUTGD_TRY_VERIFY(!view->isVisible());
  }
 }
 void captureDestinationPopupFitsAllThreeChoices(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto second=profiles.first().toObject();second["id"]="synthetic-second";second["label"]="Second destination";profiles.append(second);doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1060,680);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(1);window.show();auto* combo=window.findChild<QComboBox*>("defaultCaptureDestination");QVERIFY(combo);QCOMPARE(combo->count(),3);combo->showPopup();auto* view=combo->view();PUTGD_TRY_VERIFY(view->isVisible());for(int i=0;i<combo->count();++i)QVERIFY2(view->viewport()->rect().contains(view->visualRect(combo->model()->index(i,0))),"Capture Destination choice is clipped");QCOMPARE(view->verticalScrollBar()->maximum(),0);combo->hidePopup();
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
  PUTGD_TRY_COMPARE(picker->currentText(),QString("Other S3-compatible"));
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
  auto* bucket=window.findChild<QLineEdit*>("s3Bucket");PUTGD_TRY_VERIFY(bucket->isVisible());
  for(const auto* name:{"s3Service","s3RegionChoices","s3AclMode"})QVERIFY2(!window.findChild<QComboBox*>(name)->isVisible(),name);
  QVERIFY(window.findChild<QComboBox*>("b2LinkMode")->isVisible());QVERIFY(!window.findChild<QSpinBox*>("b2LinkDays")->isVisible());
  QVERIFY(!window.findChild<QLineEdit*>("credentialSessionToken")->isVisible());
  QCOMPARE(window.findChild<QLineEdit*>("credentialAccessKey")->accessibleName(),QString("Application key ID"));
  QCOMPARE(window.findChild<QLineEdit*>("credentialSecretKey")->accessibleName(),QString("Application key"));
  bucket->setText("putgd-test-bucket");QMetaObject::invokeMethod(bucket,"editingFinished");
  const auto saved=[&]{return service.document()["profiles"].toArray()[0].toObject();};
  PUTGD_TRY_COMPARE(saved()["provider"].toString(),QString("backblaze-b2"));
  auto expected=b2::defaults();expected["bucket"]="putgd-test-bucket";
  PUTGD_TRY_COMPARE(saved()["settings"].toObject(),expected);
  QCOMPARE(saved()["credentials"].toObject()["reference"].toString(),QString("putgd/b2/development-s3"));
  auto* mode=window.findChild<QComboBox*>("b2LinkMode");mode->setCurrentIndex(mode->findData("authorized"));emit mode->activated(mode->currentIndex());
  PUTGD_TRY_COMPARE(saved()["settings"].toObject()["linkMode"].toString(),QString("authorized"));
  PUTGD_TRY_VERIFY(window.findChild<QSpinBox*>("b2LinkDays")->isVisible());QVERIFY(!window.findChild<QLineEdit*>("s3PublicUrlBase")->isVisible());
  QVERIFY(saved()["settings"].toObject()["publicUrlBase"].isNull());QCOMPARE(saved()["settings"].toObject()["linkValidityDays"].toInt(),7);
 }
 void redesignedLongDropdownScrollsWithinItsVisibleItemLimit(){
  VisualStyle::apply(*qApp);QWidget root;auto* layout=new QVBoxLayout(&root);auto* combo=VisualStyle::choiceBox(&root);
  combo->setMaxVisibleItems(7);for(int i=0;i<60;++i)combo->addItem(QString("Option %1").arg(i+1));
  layout->addWidget(combo);layout->addStretch();RedesignTheme::apply(root);root.resize(600,400);root.show();combo->showPopup();
  auto* view=combo->view();PUTGD_TRY_VERIFY(view->isVisible());QVERIFY(view->verticalScrollBar()->maximum()>0);
  QVERIFY(view->viewport()->height()<view->sizeHintForRow(0)*8);
  QTest::keyClick(view,Qt::Key_End);QTest::keyClick(view,Qt::Key_Return);PUTGD_TRY_COMPARE(combo->currentIndex(),59);PUTGD_TRY_VERIFY(!view->isVisible());
 }
 void protonConnectOpensSignInOnceAndShowsProgressBesideButton(){
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=configuration(dir.path(),true);doc["profiles"]=QJsonArray{QJsonObject{{"id","synthetic-proton"},{"label","Synthetic Proton"},{"provider","proton-drive"},{"settings",ProtonProvider::defaults()},{"credentials",QJsonObject{{"mode","keychain"},{"reference","putgd/synthetic-proton"}}}}};QVERIFY(service.save(doc,{}));UiSessionFixture fixture(service,secrets);int starts=0;JsonReply complete;fixture.setup.setProviderSetup([&](const QString& kind,const Destination&,const QJsonObject&,JsonReply done,auto event){QCOMPARE(kind,QString("connectAccount"));++starts;complete=done;event({{"event","auth"},{"url","https://account.proton.me/cli?synthetic=true"}});});openedUrls_.clear();host::setTestOpener([this](const QUrl& url){recordProtonUrl(url);});auto cleanup=qScopeGuard([]{host::setTestOpener({});});MainWindow window(fixture.session);window.navigate("destinations");window.show();auto* button=window.findChild<QPushButton*>("connectProtonAccount");button->click();PUTGD_TRY_COMPARE(openedUrls_.size(),1);QCOMPARE(openedUrls_.first().host(),QString("account.proton.me"));auto* account=window.findChild<QLabel*>("protonAccountStatus");QVERIFY(account);QVERIFY(account->text().contains("browser"));QVERIFY(account->isVisible());putgd::test::settle(600);PUTGD_TRY_COMPARE(openedUrls_.size(),1);PUTGD_TRY_COMPARE(starts,1);PUTGD_TRY_VERIFY(complete);complete({"proton_runtime_unavailable","Synthetic runtime unavailable",{}},{});PUTGD_TRY_VERIFY(account->text().contains("Synthetic runtime unavailable"));PUTGD_TRY_VERIFY(button->isEnabled());
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
  PUTGD_TRY_COMPARE(inspections,1);QVERIFY(!test->isEnabled());
  auto* status=window.findChild<QLabel*>("destinationStatus");QVERIFY(status->text().contains("account and selected folder"));
  finish({},{{"testScope","account-folder"}});
  PUTGD_TRY_VERIFY(status->text().contains("Account and selected folder are accessible"));QVERIFY(!status->text().contains("upload, public link"));QCOMPARE(uploads,0);
  if(editAfterTest){auto* folder=window.findChild<QComboBox*>("protonFolder");folder->addItem("Another folder","synthetic-other");folder->setCurrentIndex(folder->count()-1);PUTGD_TRY_COMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["folderUid"].toString(),QString("synthetic-other"));}
  bool warned=false;QTimer dismiss;connect(&dismiss,&QTimer::timeout,this,[&]{if(auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())){warned=true;box->button(QMessageBox::Yes)->click();}});dismiss.start(10);
  window.findChild<QPushButton*>("makeDefaultDestination")->click();
  PUTGD_TRY_COMPARE(service.document()["captureDefaults"].toObject()["profileId"].toString(),QString("synthetic-proton"));
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
  PUTGD_TRY_VERIFY(inspect());QCOMPARE(job["error"].toObject()["code"].toString(),expected);QCOMPARE(uploads,0);QCOMPARE(inspections,1);
 }

 void protonToS3DraftSavesProviderTogetherWithFinishedBucket(){
  QTemporaryDir dir;CredentialStore secrets;ProviderRegistry registry;registry.add("s3",std::make_shared<S3Provider>(secrets));registry.add("proton-drive",std::make_shared<ProtonProvider>());SettingsService service(dir.filePath("config.json"),[&](auto id,auto values){return registry.validate(id,values);},nullptr,[&](auto id,auto reference){return registry.validateCredentials(id,reference);});auto doc=configuration(dir.path(),true);doc["profiles"]=QJsonArray{QJsonObject{{"id","synthetic-proton"},{"label","Synthetic destination"},{"provider","proton-drive"},{"settings",ProtonProvider::defaults()},{"credentials",QJsonObject{{"mode","keychain"},{"reference","putgd/synthetic-proton"}}}}};QVERIFY(service.save(doc,{}));UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();auto* provider=window.findChild<QComboBox*>("storageProvider");provider->setCurrentIndex(provider->findData("s3"));auto* status=window.findChild<QLabel*>("settingsStatus");PUTGD_TRY_VERIFY(status->text().contains("Invalid S3 settings"));QCOMPARE(service.document()["profiles"].toArray()[0].toObject()["provider"].toString(),QString("proton-drive"));QVERIFY(!service.document()["profiles"].toArray()[0].toObject()["settings"].toObject().contains("bucket"));auto* bucket=window.findChild<QLineEdit*>("s3Bucket");bucket->setText("synthetic-valid-bucket");QMetaObject::invokeMethod(bucket,"editingFinished");PUTGD_TRY_COMPARE(service.document()["profiles"].toArray()[0].toObject()["provider"].toString(),QString("s3"));QCOMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["bucket"].toString(),QString("synthetic-valid-bucket"));QVERIFY(!status->text().contains("Proton"));
 }
 void wheelOverSettingsTabsDoesNotChangePageButKeyboardStillWorks(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();auto* tabs=window.findChild<QTabWidget*>("settingsTabs");auto* bar=tabs->tabBar();tabs->setCurrentIndex(1);const QPointF position=bar->tabRect(1).center();for(const auto delta:{120,-120}){QWheelEvent event(position,bar->mapToGlobal(position.toPoint()),QPoint{},QPoint{0,delta},Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);QApplication::sendEvent(bar,&event);QCOMPARE(tabs->currentIndex(),1);}bar->setFocus();QTest::keyClick(bar,Qt::Key_Right);QCOMPARE(tabs->currentIndex(),2);
 }
 void editRailButtonOffersOtherSourcesWithoutGrowingTheRail(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(configuration(dir.path(),true),{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);
  QCOMPARE(window.findChildren<QToolButton*>("captureActionButton").size(),Settings::invocableActionNames().size());
  QToolButton* record=nullptr;for(auto* button:window.findChildren<QToolButton*>("captureActionButton"))if(button->property("captureAction")=="record")record=button;QVERIFY(record);
  QToolButton* edit=nullptr;for(auto* button:window.findChildren<QToolButton*>("captureActionButton"))if(button->property("captureAction")=="edit")edit=button;
  // Graphite: the other sources are the split button's menu, a right-click or a drop;
  // the main action stays Capture region & edit and never takes another source.
  QVERIFY(edit);QCOMPARE(edit->contextMenuPolicy(),Qt::CustomContextMenu);QVERIFY(edit->acceptDrops());QCOMPARE(edit->popupMode(),QToolButton::MenuButtonPopup);
  QVERIFY(edit->toolTip().contains("menu"));
  auto* menu=window.findChild<QMenu*>("editSourceMenu");QVERIFY(menu);QCOMPARE(edit->menu(),menu);
  QStringList labels;for(auto* action:menu->actions())if(!action->isSeparator())labels.append(action->text().replace("&&","&"));
  QCOMPARE(labels,QStringList({"Capture region & edit","Edit clipboard image","Edit an image file…"}));
  QStringList requests;connect(&window,&MainWindow::editRequested,this,[&](const QString& source,const QString& path){requests.append(source+"|"+path);});
  int actions=0;connect(&window,&MainWindow::actionRequested,this,[&](auto){++actions;});
  window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  emit edit->customContextMenuRequested(QPoint(8,8));PUTGD_TRY_VERIFY(menu->isVisible());menu->actions().at(2)->trigger();menu->hide();
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
 void captureRailExplainsActionAndCurrentSavedOutputWithoutInvokingIt(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);int invoked=0;connect(&window,&MainWindow::actionRequested,this,[&](auto){++invoked;});const QHash<QString,QString> labels{{"region","Capture region"},{"window","Capture window"},{"desktop","Capture desktop"},{"clipboard","Upload clipboard"},{"edit","Capture region & edit"}};for(const auto& action:Settings::actionNames()){QToolButton* button=nullptr;for(auto* candidate:window.findChildren<QToolButton*>("captureActionButton"))if(candidate->property("captureAction")==action)button=candidate;QVERIFY(button);QCOMPARE(button->accessibleName(),labels.value(action));QVERIFY(button->toolTip().contains("Save locally"));QVERIFY(button->accessibleDescription().contains("clipboard",Qt::CaseInsensitive));}auto* help=window.findChild<QToolButton*>("captureActionsHeadingHelp");QVERIFY(help);help->click();auto* dialog=window.findChild<QMessageBox*>("settingHelpDialog");QVERIFY(dialog);QVERIFY(dialog->informativeText().contains("focused external app window"));dialog->close();
  {QWidget host;QLineEdit control(&host);auto* row=VisualStyle::settingLabel("Screen region & edit shortcut","help",&control,&host);QLabel* caption=nullptr;for(auto* label:row->findChildren<QLabel*>())if(label->buddy()==&control)caption=label;QVERIFY(caption);QCOMPARE(caption->text(),QString("Screen region && edit shortcut"));QCOMPARE(control.accessibleName(),QString("Screen region & edit shortcut"));}
  auto doc=service.document();auto protonSettings=ProtonProvider::defaults();protonSettings["shareByDefault"]=false;doc["profiles"]=QJsonArray{QJsonObject{{"id","synthetic-proton"},{"label","Synthetic Drive"},{"provider","proton-drive"},{"settings",protonSettings},{"credentials",QJsonObject{{"mode","keychain"},{"reference","putgd/synthetic-drive"}}}}};auto actions=doc["actions"].toObject();auto region=actions["region"].toObject();region["destination"]="upload";region["profileId"]="synthetic-proton";actions["region"]=region;doc["actions"]=actions;QVERIFY(service.save(doc,service.revision()));QToolButton* capture=nullptr;for(auto* button:window.findChildren<QToolButton*>("captureActionButton"))if(button->property("captureAction")=="region")capture=button;QVERIFY(capture);PUTGD_TRY_VERIFY(capture->accessibleDescription().contains("Upload privately"));QVERIFY(capture->accessibleDescription().contains("clipboard is left unchanged"));region["publicationMode"]="share";actions["region"]=region;doc["actions"]=actions;QVERIFY(service.save(doc,service.revision()));PUTGD_TRY_VERIFY(capture->accessibleDescription().contains("copy its share link"));QCOMPARE(invoked,0);
 }
 void constructionDoesNotWriteAndHasNoSaveCancelControls(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider s3(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto settings){return s3.validateSettings(settings);});QVERIFY(service.save(Settings::developmentTemplate(),{}));const auto initial=service.document();const auto revision=service.revision();UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();putgd::test::settle(500);
  PUTGD_TRY_COMPARE(service.document(),initial);PUTGD_TRY_COMPARE(service.revision(),revision);PUTGD_TRY_VERIFY(!window.findChild<QPushButton*>("saveSettings"));PUTGD_TRY_VERIFY(!window.findChild<QPushButton*>("cancelSettings"));PUTGD_TRY_VERIFY(!window.findChild<QPushButton*>("setupLocal"));auto* tabs=window.findChild<QTabWidget*>("settingsTabs");QVERIFY(tabs);QCOMPARE(tabs->count(),4);QCOMPARE(tabs->tabText(0),QString("General"));QCOMPARE(tabs->tabText(3),QString("Advanced"));
 }
 void finishedFieldAutosavesAndSurvivesRestart(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));auto doc=configuration(dir.path(),true);QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();auto* root=window.findChild<QLineEdit*>("archiveRoot");QVERIFY(root);window.activateWindow();root->setFocus();PUTGD_TRY_VERIFY(root->hasFocus());root->selectAll();QTest::keyClicks(root,dir.filePath("saved-locally"));putgd::test::settle(450);PUTGD_TRY_COMPARE(service.document()["archive"].toObject()["root"],doc["archive"].toObject()["root"]);window.findChild<QCheckBox*>("launchAtLogin")->setFocus();PUTGD_TRY_COMPARE(service.document()["archive"].toObject()["root"].toString(),dir.filePath("saved-locally"));SettingsService restarted(service.path());QVERIFY(restarted.reload());QCOMPARE(restarted.document()["archive"],service.document()["archive"]);
 }
 void invalidEditStaysInlineWhileIndependentValidToggleSaves(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);auto* number=window.findChild<QLineEdit*>("maxFiles");number->setText("not a number");QMetaObject::invokeMethod(number,"editingFinished");PUTGD_TRY_VERIFY(number->property("invalid").toBool());QCOMPARE(service.document()["limits"].toObject()["maxFiles"].toInt(),100);auto* notify=window.findChild<QCheckBox*>("notifications");notify->setChecked(false);PUTGD_TRY_VERIFY(!service.document()["preferences"].toObject()["notifications"].toBool());QCOMPARE(number->text(),QString("not a number"));QVERIFY(number->property("invalid").toBool());QCOMPARE(service.document()["limits"].toObject()["maxFiles"].toInt(),100);
 }
 void displayUnitsPreserveStoredBytesPixelsAndMilliseconds(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider s3(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto settings){return s3.validateSettings(settings);});QVERIFY(service.save(Settings::developmentTemplate(),{}));UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();QCOMPARE(window.findChild<QLineEdit*>("maxFileBytes")->text(),QString("Auto"));QCOMPARE(window.findChild<QLineEdit*>("imagePixels")->text(),QString("100"));QCOMPARE(window.findChild<QLineEdit*>("s3ConnectTimeoutMs")->text(),QString("5"));window.findChild<QLineEdit*>("maxFileBytes")->setText("2.000000000931322574615478515625");window.findChild<QLineEdit*>("imagePixels")->setText("100.000001");window.findChild<QLineEdit*>("s3ConnectTimeoutMs")->setText("5.123");PUTGD_TRY_COMPARE(service.document()["limits"].toObject()["maxFileBytes"].toInteger(),2147483649LL);QCOMPARE(service.document()["limits"].toObject()["imagePixels"].toInteger(),100000001LL);QCOMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["connectTimeoutMs"].toInt(),5123);QCOMPARE(window.findChild<QLineEdit*>("maxFileBytes")->text(),QString("2.000000000931322574615478515625"));
 }
 void resetOneAndResetAllUseRealDefaults(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));auto doc=configuration(dir.path(),true);auto limits=doc["limits"].toObject();limits["concurrentUploads"]=4;doc["limits"]=limits;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);QPushButton* reset=nullptr;for(auto* button:window.findChildren<QPushButton*>())if(button->accessibleName()=="Reset Uploads at once to default")reset=button;QVERIFY(reset);QVERIFY(reset->isVisible());reset->click();PUTGD_TRY_COMPARE(service.document()["limits"].toObject()["concurrentUploads"].toInt(),3);window.findChild<QLineEdit*>("maxFiles")->setText("25");PUTGD_TRY_COMPARE(service.document()["limits"].toObject()["maxFiles"].toInt(),25);
  // #238: Diagnostics now shares the tab; Reset all to defaults still resets only the limits.
  auto* debug=window.findChild<QCheckBox*>("debugLogging");debug->setChecked(true);PUTGD_TRY_VERIFY(service.snapshot().debugLogging);
  QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QMessageBox*>(widget))dialog->button(QMessageBox::Yes)->click();});window.findChild<QPushButton*>("resetAdvancedDefaults")->click();PUTGD_TRY_COMPARE(service.document()["limits"].toObject()["maxFiles"].toInt(),100);
  putgd::test::settle(500);PUTGD_TRY_VERIFY(debug->isChecked());PUTGD_TRY_VERIFY(service.snapshot().debugLogging);
 }
 void externalEditsPreserveInvalidDraftAndReloadRecovers(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();auto* number=window.findChild<QLineEdit*>("maxFiles");number->setText("invalid");PUTGD_TRY_VERIFY(number->property("invalid").toBool());auto external=service.document();auto prefs=external["preferences"].toObject();prefs["notifications"]=false;external["preferences"]=prefs;QVERIFY(service.save(external,service.revision()));PUTGD_TRY_VERIFY(window.settings()->saveViewState()["conflict"].toBool());QCOMPARE(number->text(),QString("invalid"));window.findChild<QPushButton*>("reloadSettings")->click();PUTGD_TRY_COMPARE(number->text(),QString("100"));QVERIFY(!window.settings()->saveViewState()["conflict"].toBool());QVERIFY(!window.findChild<QCheckBox*>("notifications")->isChecked());
 }
 void malformedExternalReloadPreservesAcceptedSettingsAndBlocksStaleOverwrite(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));const auto original=service.document();const auto revision=service.revision();CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();auto* field=window.findChild<QLineEdit*>("maxFiles");field->setText("invalid draft");PUTGD_TRY_VERIFY(field->property("invalid").toBool());QFile external(service.path());QVERIFY(external.open(QIODevice::WriteOnly|QIODevice::Truncate));QCOMPARE(external.write("{malformed"),qint64(10));external.close();window.findChild<QPushButton*>("reloadSettings")->click();auto* status=window.findChild<QLabel*>("settingsStatus");PUTGD_TRY_VERIFY(status->text().contains("config_invalid"));PUTGD_TRY_VERIFY(!window.settings()->saveViewState().contains("stateError"));QCOMPARE(service.document(),original);QCOMPARE(service.revision(),revision);QVERIFY(!service.save(original,revision));QVERIFY(external.open(QIODevice::ReadOnly));QCOMPARE(external.readAll(),QByteArray("{malformed"));external.close();QVERIFY(external.open(QIODevice::WriteOnly|QIODevice::Truncate));const auto valid=QJsonDocument(original).toJson(QJsonDocument::Indented);QCOMPARE(external.write(valid),qint64(valid.size()));external.close();window.findChild<QPushButton*>("reloadSettings")->click();PUTGD_TRY_COMPARE(field->text(),QString("100"));QCOMPARE(service.document(),original);
 }
 void destinationsUseFriendlyEnumsAndKeepCredentialsOutOfConfigAndCheckpoint(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider s3(secrets);ProtonProvider proton;SettingsService service(dir.filePath("config.json"),[&](auto id,auto settings){return id=="s3"?s3.validateSettings(settings):proton.validateSettings(settings);});QVERIFY(service.save(Settings::developmentTemplate(),{}));UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();QVERIFY(!window.findChild<QLineEdit*>("destinationId")->isVisible());auto* acl=window.findChild<QComboBox*>("s3AclMode");QCOMPARE(acl->currentData().toString(),QString("public-read"));QVERIFY(!acl->currentText().contains("public-read"));window.findChild<QLineEdit*>("credentialSecretKey")->setText("synthetic-never-export-me");window.findChild<QLineEdit*>("credentialAccessKey")->setText("synthetic-access");const auto encoded=QJsonDocument(window.settings()->saveViewState()).toJson();QVERIFY(!encoded.contains("synthetic-never-export-me"));QVERIFY(!encoded.contains("synthetic-access"));QVERIFY(!QJsonDocument(service.document()).toJson().contains("synthetic-never-export-me"));
  auto* provider=window.findChild<QComboBox*>("storageProvider");provider->setCurrentIndex(provider->findData("proton-drive"));PUTGD_TRY_COMPARE(service.document()["profiles"].toArray()[0].toObject()["provider"].toString(),QString("proton-drive"));window.findChild<QDoubleSpinBox*>("protonRequestTimeout")->setValue(321.123);QMetaObject::invokeMethod(window.findChild<QDoubleSpinBox*>("protonRequestTimeout"),"editingFinished");PUTGD_TRY_COMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["requestTimeoutMs"].toInt(),321123);provider->setCurrentIndex(provider->findData("s3"));QCOMPARE(window.findChild<QLineEdit*>("credentialSecretKey")->text(),QString("synthetic-never-export-me"));window.settings()->clearCredentialInputs();QVERIFY(window.findChild<QLineEdit*>("credentialSecretKey")->text().isEmpty());
 }
 void perActionLocalOutputAndClipboardDeliveryRemainIndependent(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=Settings::developmentTemplate();auto archive=doc["archive"].toObject();archive["keepUploads"]=true;doc["archive"]=archive;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();auto* clipboard=window.findChild<QComboBox*>("uploadClipboardBehavior");clipboard->setCurrentIndex(clipboard->findData("list"));window.findChild<QCheckBox*>("useDefaultRegion")->setChecked(false);auto* destination=window.findChild<QComboBox*>("profileRegion");destination->setCurrentIndex(0);PUTGD_TRY_COMPARE(service.document()["preferences"].toObject()["uploadClipboardBehavior"].toString(),QString("list"));PUTGD_TRY_COMPARE(service.snapshot().actions["region"].destination,QString("local"));QCOMPARE(service.snapshot().actions["desktop"].destination,QString("upload"));QVERIFY(service.snapshot().archive.keepUploads);
 }
 // #60: General → Appearance saves preferences.appearance.theme; an untouched
 // config keeps no appearance object.
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
 // #239: the clipboard action says upload, like the tray; ids and keys stay.
 void clipboardActionSaysUpload(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();
  QToolButton* clipboard=nullptr;for(auto* button:window.findChildren<QToolButton*>("captureActionButton"))if(button->property("captureAction")=="clipboard")clipboard=button;
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
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto settings){return provider.validateSettings(settings);});UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* add=window.findChild<QPushButton*>("addS3Destination");
  QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QInputDialog*>(widget))dialog->reject();});add->menu()->actions().first()->trigger();
  QCOMPARE(window.findChild<QListWidget*>("destinationList")->count(),0);QVERIFY(service.revision().isEmpty());
  QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QInputDialog*>(widget)){dialog->setTextValue("My S3 destination");dialog->accept();}});add->menu()->actions().first()->trigger();
  QCOMPARE(window.findChild<QListWidget*>("destinationList")->count(),1);QVERIFY(!window.findChild<QLabel*>("destinationsEmpty")->isVisible());QVERIFY(window.findChild<QWidget*>("destinationScroll")->isVisible());
  QVERIFY(!window.findChild<QPushButton*>("makeDefaultDestination")->isEnabled());auto* bucket=window.findChild<QLineEdit*>("s3Bucket");bucket->setText("a");putgd::test::settle(600);PUTGD_TRY_VERIFY(service.revision().isEmpty());PUTGD_TRY_VERIFY(bucket->property("invalid").toBool());
  bucket->setText("synthetic-first-bucket");QMetaObject::invokeMethod(bucket,"editingFinished");PUTGD_TRY_COMPARE(service.document()["profiles"].toArray().size(),1);
  const auto profile=service.document()["profiles"].toArray()[0].toObject();QCOMPARE(profile["label"].toString(),QString("My S3 destination"));QVERIFY(service.snapshot().actions["region"].profileId.isEmpty());
  SettingsService restarted(service.path(),[&](auto,auto settings){return provider.validateSettings(settings);});QVERIFY(restarted.reload());QCOMPARE(restarted.document()["profiles"],service.document()["profiles"]);
 }
 void optionalDefaultsStayAbsentAndDoNotBlockDestinationSetup(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return provider.validateSettings(value);});auto doc=Settings::developmentTemplate();auto naming=doc["naming"].toObject();for(const auto& key:QStringList{"fileTemplate","captureTemplate","imageTemplate","textTemplate"})naming.remove(key);doc["naming"]=naming;auto limits=doc["limits"].toObject();limits.remove("pendingImageBytes");doc["limits"]=limits;auto preferences=doc["preferences"].toObject();preferences.remove("uploadClipboardBehavior");doc["preferences"]=preferences;QVERIFY(service.save(doc,{}));int tests=0;UiSessionFixture fixture(service,secrets,[&](auto,auto,auto reply){++tests;reply({});});MainWindow window(fixture.session);window.settings();auto* pending=window.findChild<QLineEdit*>("pendingImageBytes");QVERIFY(pending);QVERIFY(!pending->property("defaultText").toString().isEmpty());PUTGD_TRY_COMPARE(pending->text(),pending->property("defaultText").toString());window.findChild<QCheckBox*>("notifications")->setChecked(false);PUTGD_TRY_VERIFY(!service.document()["preferences"].toObject()["notifications"].toBool());PUTGD_TRY_VERIFY(!window.settings()->saveViewState().contains("stateError"));QVERIFY(!service.document()["naming"].toObject().contains("fileTemplate"));QVERIFY(!service.document()["limits"].toObject().contains("pendingImageBytes"));QVERIFY(!service.document()["preferences"].toObject().contains("uploadClipboardBehavior"));window.findChild<QPushButton*>("testDestination")->click();PUTGD_TRY_COMPARE(tests,1);
 }
 void failedAutosaveKeepsDraftAcrossReopenAndRequiresExplicitRetry(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));const auto before=service.document();CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();fixture.client.failOnce["settings.save"]={"client_unavailable","Synthetic lost save reply",{}};window.findChild<QLineEdit*>("archiveRoot")->setText(dir.filePath("retry-output"));PUTGD_TRY_VERIFY(window.settings()->saveViewState()["autosaveBlocked"].toBool());putgd::test::settle(600);PUTGD_TRY_COMPARE(service.document(),before);const auto state=window.settings()->saveViewState();MainWindow reopened(fixture.session);QVERIFY(reopened.settings()->restoreViewState(state));QVERIFY(reopened.findChild<QPushButton*>("retryAutosave")->isHidden()==false);reopened.findChild<QPushButton*>("retryAutosave")->click();PUTGD_TRY_COMPARE(service.document()["archive"].toObject()["root"].toString(),dir.filePath("retry-output"));QVERIFY(!reopened.settings()->saveViewState()["autosaveBlocked"].toBool());
 }
 void artifactLimitsOfferAutoAndNoLimit(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();
  auto limit=[&](const QString& key)->QJsonValue{return service.document()["limits"].toObject().value(key);};
  for(const auto& key:Settings::artifactByteLimitKeys()){
   auto* edit=window.findChild<QLineEdit*>(key);auto* combo=window.findChild<QComboBox*>(key+"Choice");QVERIFY(edit);QVERIFY(combo);
   QCOMPARE(edit->text(),QString("Auto"));QCOMPARE(edit->placeholderText(),QString("No limit"));
   QCOMPARE(combo->itemText(0),QString("Auto"));QCOMPARE(combo->itemText(1),QString("No limit"));
   QVERIFY(window.findChild<QToolButton*>(key+"ChoiceHelp"));
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
 void detailedSettingHelpIsReachableFromKeyboard(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);window.show();auto* help=window.findChild<QToolButton*>("maxFileBytesChoiceHelp");QVERIFY(help);QVERIFY(help->isVisible());help->setFocus();QTest::keyClick(help,Qt::Key_Space);QMessageBox* dialog=nullptr;PUTGD_TRY_VERIFY((dialog=window.findChild<QMessageBox*>("settingHelpDialog")));QVERIFY(dialog->informativeText().contains("GiB"));QVERIFY(dialog->informativeText().contains("rejected"));QCOMPARE(dialog->textFormat(),Qt::PlainText);dialog->close();
 }
 void addDestinationDialogPersistsValidDraftAndRemovalPreservesOriginal(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return provider.validateSettings(value);});QVERIFY(service.save(Settings::developmentTemplate(),{}));const auto original=service.document()["profiles"].toArray()[0].toObject();UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();auto* add=window.findChild<QPushButton*>("addS3Destination");QVERIFY(add&&add->menu());QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QInputDialog*>(widget)){dialog->setTextValue("Synthetic second destination");dialog->accept();}});add->menu()->actions().first()->trigger();auto* bucket=window.findChild<QLineEdit*>("s3Bucket");bucket->setText("synthetic-second-bucket");QMetaObject::invokeMethod(bucket,"editingFinished");PUTGD_TRY_COMPARE(service.document()["profiles"].toArray().size(),2);PUTGD_TRY_VERIFY(!window.settings()->saveViewState().contains("stateError"));const auto profiles=service.document()["profiles"].toArray();QCOMPARE(profiles[0].toObject(),original);QCOMPARE(profiles[1].toObject()["label"].toString(),QString("Synthetic second destination"));QCOMPARE(profiles[1].toObject()["settings"].toObject()["bucket"].toString(),QString("synthetic-second-bucket"));SettingsService restarted(service.path(),[&](auto,auto value){return provider.validateSettings(value);});QVERIFY(restarted.reload());QCOMPARE(restarted.document()["profiles"],profiles);QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QMessageBox*>(widget))dialog->button(QMessageBox::Yes)->click();});window.findChild<QPushButton*>("removeDestination")->click();PUTGD_TRY_COMPARE(service.document()["profiles"].toArray().size(),1);QCOMPARE(service.document()["profiles"].toArray()[0].toObject(),original);
 }
 void removingDestinationBeforeDebounceNeverAppliesItsEditToNextDestination(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return provider.validateSettings(value);});auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto second=profiles[0].toObject();second["id"]="second-destination";second["label"]="Second destination";auto fields=second["settings"].toObject();fields["bucket"]="second-bucket";second["settings"]=fields;profiles.append(second);doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();window.findChild<QLineEdit*>("s3Bucket")->setText("edited-first-bucket");QMetaObject::invokeMethod(window.findChild<QLineEdit*>("s3Bucket"),"editingFinished");QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QMessageBox*>(widget))dialog->button(QMessageBox::Yes)->click();});window.findChild<QPushButton*>("removeDestination")->click();PUTGD_TRY_COMPARE(service.document()["profiles"].toArray().size(),1);QCOMPARE(service.document()["profiles"].toArray()[0].toObject(),second);
 }
 void finishedGeneralEditSavesAfterDelayedDestinationSetupCompletes(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return provider.validateSettings(value);});QVERIFY(service.save(Settings::developmentTemplate(),{}));std::function<void(Error)> complete;UiSessionFixture fixture(service,secrets,[&](auto,auto,auto reply){complete=reply;});MainWindow window(fixture.session);window.settings();window.findChild<QPushButton*>("testDestination")->click();PUTGD_TRY_VERIFY(bool(complete));window.findChild<QCheckBox*>("notifications")->setChecked(false);putgd::test::settle(550);PUTGD_TRY_VERIFY(service.document()["preferences"].toObject()["notifications"].toBool());complete({});PUTGD_TRY_VERIFY(!service.document()["preferences"].toObject()["notifications"].toBool());
 }
 void openingConnectedProtonDestinationLoadsNamedFoldersWithoutReconnecting(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=configuration(dir.path());auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();profile["provider"]="proton-drive";auto protonSettings=ProtonProvider::defaults();protonSettings["accountRootUid"]="synthetic-account";protonSettings["folderUid"]="synthetic-folder";protonSettings["folderName"]="Saved folder";profile["settings"]=protonSettings;profiles[0]=profile;profiles.append(Settings::developmentTemplate()["profiles"].toArray().first());doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);int inspections=0;JsonReply finish;fixture.setup.setProviderSetup([&](const QString& kind,const Destination&,const QJsonObject&,JsonReply reply,auto){QCOMPARE(kind,QString("inspectAccount"));++inspections;finish=std::move(reply);});
  fixture.server.setUiDiagnostics([]{return QJsonObject{{"capabilities",QJsonObject{{"providers",QJsonObject{{"proton-drive",QJsonObject{}}}}}}};});fixture.session.refreshSnapshot();PUTGD_TRY_VERIFY(fixture.session.diagnostics()["capabilities"].toObject()["providers"].toObject().contains("proton-drive"));MainWindow window(fixture.session);window.show();putgd::test::settle(50);PUTGD_TRY_COMPARE(inspections,0);window.navigate("destinations");PUTGD_TRY_COMPARE(inspections,1);auto* progress=window.findChild<QLabel*>("protonFolderStatus");QVERIFY(progress);QVERIFY(progress->isVisible());QVERIFY(progress->text().contains("Loading folders"));
  finish({},{{"accountRootUid","synthetic-account"},{"accountEmail","synthetic@example.invalid"},{"folders",QJsonArray{QJsonObject{{"uid","synthetic-folder"},{"name","Saved folder"}},QJsonObject{{"uid","synthetic-other-id"},{"name","Another folder"}}}}});auto* folders=window.findChild<QComboBox*>("protonFolder");PUTGD_TRY_VERIFY(folders->findText("Another folder")>=0);QCOMPARE(folders->currentText(),QString("Saved folder"));QVERIFY(folders->findText("synthetic-other-id")==-1);auto* account=window.findChild<QLabel*>("protonAccountStatus");QVERIFY(account->text().contains("synthetic@example.invalid"));auto* list=window.findChild<QListWidget*>("destinationList");
  for(int repeat=0;repeat<5;++repeat){list->setCurrentRow(1);list->setCurrentRow(0);QVERIFY(account->isVisible());QVERIFY(account->text().contains("synthetic@example.invalid"));}QCOMPARE(inspections,1);window.navigate("history");window.navigate("destinations");QCOMPARE(inspections,1);auto* refresh=window.findChild<QPushButton*>("inspectProtonAccount");QVERIFY(refresh&&refresh->isVisible()&&refresh->isEnabled());QCOMPARE(folders->findData("__refresh__"),-1);refresh->click();PUTGD_TRY_COMPARE(inspections,2);QVERIFY(!refresh->isEnabled());QCOMPARE(folders->currentData().toString(),QString("synthetic-folder"));QVERIFY(progress->isVisible());QVERIFY(account->text().contains("synthetic@example.invalid"));finish({"synthetic_network","Could not load folders",{}},{});PUTGD_TRY_VERIFY(!progress->isVisible());QVERIFY(refresh->isEnabled());QVERIFY(account->text().contains("synthetic@example.invalid"));refresh->click();PUTGD_TRY_COMPARE(inspections,3);finish({},{{"accountRootUid","synthetic-account"},{"folders",QJsonArray{QJsonObject{{"uid","synthetic-folder"},{"name","Renamed saved folder"}},QJsonObject{{"uid","synthetic-new-id"},{"name","New folder"}}}}});PUTGD_TRY_VERIFY(folders->findText("New folder")>=0);QCOMPARE(folders->findText("Another folder"),-1);QCOMPARE(folders->currentData().toString(),QString("synthetic-folder"));QCOMPARE(folders->currentText(),QString("Renamed saved folder"));QVERIFY(account->text().contains("synthetic@example.invalid"));PUTGD_TRY_COMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["folderName"].toString(),QString("Renamed saved folder"));PUTGD_TRY_VERIFY(!window.settings()->saveViewState().contains("stateError"));QVERIFY(!validateUiState({{"schemaVersion",1},{"settings",window.settings()->saveViewState()}}));auto legacy=window.settings()->saveViewState();auto editors=legacy["profileEditors"].toArray();auto editor=editors[0].toObject();auto retained=editor["proton"].toObject();auto oldFolders=retained["folders"].toArray();oldFolders.append(QJsonObject{{"uid","__refresh__"},{"name",""},{"label","Refresh folders…"}});retained["folders"]=oldFolders;editor["proton"]=retained;editors[0]=editor;legacy["profileEditors"]=editors;MainWindow restored(fixture.session);QVERIFY(restored.settings()->restoreViewState(legacy));QCOMPARE(restored.findChild<QComboBox*>("protonFolder")->findData("__refresh__"),-1);QCOMPARE(restored.findChild<QComboBox*>("protonFolder")->currentData().toString(),QString("synthetic-folder"));QVERIFY(restored.findChild<QLabel*>("protonAccountStatus")->text().contains("synthetic@example.invalid"));QVERIFY(!QJsonDocument(service.document()).toJson().contains("synthetic@example.invalid"));QCOMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["folderUid"].toString(),QString("synthetic-folder"));
 }
 void connectedProtonAccountSurvivesRapidNavigation(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto proton=profiles.first().toObject();proton["id"]="synthetic-proton";proton["label"]="Synthetic Proton";proton["provider"]="proton-drive";auto account=ProtonProvider::defaults();account["accountRootUid"]="synthetic-account";account["folderUid"]="synthetic-folder";account["folderName"]="Saved folder";proton["settings"]=account;profiles.prepend(proton);doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.show();window.navigate("destinations");auto* list=window.findChild<QListWidget*>("destinationList");auto* label=window.findChild<QLabel*>("protonAccountStatus");auto* connect=window.findChild<QPushButton*>("connectProtonAccount");
  for(int repeat=0;repeat<5;++repeat){list->setCurrentRow(1);list->setCurrentRow(0);QVERIFY(label->isVisible());QVERIFY(label->text().startsWith("Connected"));QCOMPARE(connect->text(),QString("Reconnect account…"));window.navigate("history");window.navigate("destinations");QVERIFY(label->isVisible());QVERIFY(label->text().startsWith("Connected"));}
  putgd::test::settle(650);PUTGD_TRY_COMPARE(service.document(),doc);PUTGD_TRY_VERIFY(label->text().startsWith("Connected"));PUTGD_TRY_VERIFY(!validateUiState({{"schemaVersion",1},{"settings",window.settings()->saveViewState()}}));
 }
 void protonConnectionCompletedWhileAwayIsShownOnReturn(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();const auto other=profiles.first();auto proton=other.toObject();proton["id"]="synthetic-proton";proton["label"]="Synthetic Proton";proton["provider"]="proton-drive";auto oldAccount=ProtonProvider::defaults();oldAccount["accountRootUid"]="synthetic-old-account";oldAccount["folderUid"]="synthetic-old-folder";proton["settings"]=oldAccount;profiles.prepend(proton);doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);JsonReply complete;int starts=0;fixture.setup.setProviderSetup([&](const QString& kind,const Destination&,const QJsonObject&,JsonReply done,auto){QCOMPARE(kind,QString("connectAccount"));++starts;complete=std::move(done);});MainWindow window(fixture.session);window.show();window.navigate("destinations");auto* list=window.findChild<QListWidget*>("destinationList");window.findChild<QPushButton*>("connectProtonAccount")->click();PUTGD_TRY_VERIFY(bool(complete));
  for(int repeat=0;repeat<3;++repeat){list->setCurrentRow(1);list->setCurrentRow(0);}list->setCurrentRow(1);complete({},{{"accountRootUid","synthetic-new-account"},{"accountEmail","synthetic-new@example.invalid"},{"folders",QJsonArray{QJsonObject{{"uid","synthetic-folder"},{"name","Saved folder"}}}}});PUTGD_TRY_VERIFY(window.settings()->saveViewState()["setupJobs"].toArray().isEmpty());QCOMPARE(service.document()["profiles"].toArray()[1],other);
  list->setCurrentRow(0);auto* label=window.findChild<QLabel*>("protonAccountStatus");QVERIFY(label->isVisible());PUTGD_TRY_VERIFY(label->text().contains("synthetic-new@example.invalid"));QCOMPARE(window.findChild<QPushButton*>("connectProtonAccount")->text(),QString("Reconnect account…"));PUTGD_TRY_COMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["accountRootUid"].toString(),QString("synthetic-new-account"));QCOMPARE(starts,1);QVERIFY(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["folderUid"].toString().isEmpty());QCOMPARE(service.document()["profiles"].toArray()[1],other);
 }
 void storedCredentialHintsNeverExportFullKeysOrOverwriteInputs(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider s3(secrets);auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();profile["credentials"]=QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}};profiles[0]=profile;doc["profiles"]=profiles;SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return s3.validateSettings(value);});QVERIFY(service.save(doc,{}));secrets.setSession("development-s3",{"AKIA-synthetic-full-access","synthetic-full-secret","synthetic-token"});UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();auto* hint=window.findChild<QLabel*>("storedCredentialHint");PUTGD_TRY_VERIFY(hint->text().contains("AKIA"));QVERIFY(hint->text().contains("Secret key stored"));auto* access=window.findChild<QLineEdit*>("credentialAccessKey");QVERIFY(access->text().isEmpty());QVERIFY(access->placeholderText().contains("AKIA"));QVERIFY(!access->placeholderText().contains("full-access"));QCOMPARE(window.findChild<QLineEdit*>("credentialSecretKey")->echoMode(),QLineEdit::Password);const auto state=QJsonDocument(window.settings()->saveViewState()).toJson();QVERIFY(!state.contains("AKIA"));QVERIFY(!state.contains("synthetic-full-secret"));bool done=false;QJsonObject reply;fixture.session.request("setup.credentialHint",{{"profileId","development-s3"},{"expectedRevision",service.revision()}},this,[&](Error error,QJsonObject value){QVERIFY(!error);reply=value;done=true;});PUTGD_TRY_VERIFY(done);const auto encoded=QJsonDocument(reply).toJson();QVERIFY(!encoded.contains("synthetic-full-access"));QVERIFY(!encoded.contains("synthetic-full-secret"));QVERIFY(!encoded.contains("synthetic-token"));
 }
 void connectionTestShowsItsResultOnTheDestinationPage(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});
  auto document=Settings::developmentTemplate();auto profiles=document["profiles"].toArray();auto second=profiles[0].toObject();
  second["id"]="second";second["label"]="Second destination";profiles.append(second);document["profiles"]=profiles;
  QVERIFY(service.save(document,{}));CredentialStore secrets;std::function<void(Error)> complete;
  UiSessionFixture fixture(service,secrets,[&](auto,auto,auto reply){complete=reply;});MainWindow window(fixture.session);
  window.navigate("destinations");window.show();QLabel* status=nullptr;
  for(auto* label:window.findChildren<QLabel*>())if(label->accessibleName()=="Destination status")status=label;
  QVERIFY(status);auto* test=window.findChild<QPushButton*>("testDestination");test->click();PUTGD_TRY_VERIFY(bool(complete));
  complete({});PUTGD_TRY_VERIFY(test->isEnabled());PUTGD_TRY_VERIFY(status->text().contains("Connection successful"));
  QVERIFY(status->isVisible());QCOMPARE(status->property("feedbackTone").toString(),QString("success"));
  auto* destinations=window.findChild<QListWidget*>("destinationList");destinations->setCurrentRow(1);QVERIFY(status->text().isEmpty());destinations->setCurrentRow(0);
  complete={};test->click();PUTGD_TRY_VERIFY(bool(complete));QVERIFY(status->text().contains("Testing connection"));
  complete({"s3_permission_denied","S3 denied the test upload. Check IAM and bucket policy permissions.",{}});
  PUTGD_TRY_VERIFY(test->isEnabled());PUTGD_TRY_VERIFY(status->text().contains("IAM and bucket policy"));
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
  PUTGD_TRY_COMPARE(secret->text(),QString("synthetic-saved-secret"));QCOMPARE(secret->echoMode(),QLineEdit::Normal);
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
  secret->clear();show->click();PUTGD_TRY_COMPARE(secret->text(),QString("synthetic-saved-secret"));
  auto* list=window.findChild<QListWidget*>("destinationList");list->setCurrentRow(1);
  QVERIFY(secret->text().isEmpty());QCOMPARE(show->text(),QString("Show"));list->setCurrentRow(0);QVERIFY(secret->text().isEmpty());
  fixture.client.replyDelayMs["setup.revealCredentialSecret"]=150;
  show->click();show->click();putgd::test::settle(250);PUTGD_TRY_VERIFY(secret->text().isEmpty());PUTGD_TRY_COMPARE(secret->echoMode(),QLineEdit::Password);
  show->click();list->setCurrentRow(1);list->setCurrentRow(0);putgd::test::settle(250);PUTGD_TRY_VERIFY(secret->text().isEmpty());
  show->click();PUTGD_TRY_COMPARE(secret->text(),QString("synthetic-saved-secret"));window.settings()->clearCredentialInputs();
  QVERIFY(secret->text().isEmpty());QCOMPARE(show->text(),QString("Show"));QVERIFY(!show->isChecked());QVERIFY(!secret->isReadOnly());
  show->click();window.navigate("settings");putgd::test::settle(250);PUTGD_TRY_VERIFY(secret->text().isEmpty());window.navigate("destinations");
  secrets.forget("development-s3",profile["credentials"].toObject());show->click();
  PUTGD_TRY_VERIFY(window.findChild<QLabel*>("settingsStatus")->text().contains("credential_setup_required"));
  QVERIFY(secret->text().isEmpty());QVERIFY(!show->isChecked());QVERIFY(!secret->isReadOnly());
 }
 void uiStateStrictlyRejectsUnexpectedAndSecretFields(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);auto state=QJsonObject{{"schemaVersion",1},{"settings",window.settings()->saveViewState()},{"main",window.saveViewState()}};QVERIFY(!validateUiState(state));auto main=state["main"].toObject();main["secretKey"]="synthetic-secret";state["main"]=main;auto error=validateUiState(state);QVERIFY(error);QVERIFY(!QJsonDocument(error.toJson()).toJson().contains("synthetic-secret"));
 }
 void acceptedSetupIdSurvivesAnUncertainInspectionReply(){
  QTemporaryDir d;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(d.filePath("config.json"),[&](auto,auto settings){return provider.validateSettings(settings);});QVERIFY(service.save(Settings::developmentTemplate(),{}));int starts=0;std::function<void(Error)> finish;UiSessionFixture fixture(service,secrets,[&](auto,auto,auto reply){++starts;finish=reply;});fixture.client.failOnce["setup.inspect"]={"client_unavailable","Synthetic lost inspection reply",{}};SettingsWindow window(fixture.session);window.findChild<QPushButton*>("testDestination")->click();PUTGD_TRY_VERIFY(window.findChild<QLabel*>("settingsStatus")->text().contains("client_unavailable"));const auto saved=window.saveViewState();QCOMPARE(saved["setupJobs"].toArray().size(),1);SettingsWindow reopened(fixture.session);QVERIFY(reopened.restoreViewState(saved));QVERIFY(finish);finish({});PUTGD_TRY_VERIFY(reopened.findChild<QLabel*>("settingsStatus")->text().contains("Connection successful"));QCOMPARE(starts,1);
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
  PUTGD_TRY_VERIFY(status->text().contains("client_unavailable"));
  QCOMPARE(starts,1);QVERIFY(finish);QVERIFY(!test->isEnabled());
  window.findChild<QLineEdit*>("s3Bucket")->setText("unsaved-bucket");
  window.findChild<QLineEdit*>("credentialSecretKey")->setText("synthetic-unsubmitted-secret");
  finish({});
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(window.saveViewState()["setupJobs"].toArray().isEmpty(),2000);
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
  for(int i=0;i<65;++i){const auto id=start();QVERIFY(!id.isEmpty());if(i==0)expired=id;PUTGD_TRY_VERIFY(completed(id));}
  Error missing;fixture.setup.inspect(expired,[&](Error error,QJsonObject){missing=error;});QCOMPARE(missing.code,QString("setup_not_found"));
  hold=true;const auto pending=start();QVERIFY(!pending.isEmpty());QVERIFY(finish);QCOMPARE(starts,66);
  SettingsWindow window(fixture.session);auto state=window.saveViewState();
  state["setupJobs"]=QJsonArray{QJsonObject{{"setupId",expired},{"kind","testDestination"},{"profileId","development-s3"}},QJsonObject{{"setupId",pending},{"kind","testDestination"},{"profileId","development-s3"}}};
  QVERIFY(window.restoreViewState(state));
  auto* test=window.findChild<QPushButton*>("testDestination");
  PUTGD_TRY_VERIFY(window.findChild<QLabel*>("settingsStatus")->text().contains("setup_not_found"));
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(window.saveViewState()["setupJobs"].toArray().size(),1,2000);
  QCOMPARE(window.saveViewState()["setupJobs"].toArray().first().toObject()["setupId"].toString(),pending);
  QVERIFY(!test->isEnabled());
  finish({});
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(window.saveViewState()["setupJobs"].toArray().isEmpty(),2000);
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
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(window.saveViewState()["setupJobs"].toArray().size(),1,2000);
  QVERIFY(!window.findChild<QPushButton*>("testDestination")->isEnabled());
  finish["second-s3"]({});
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(window.saveViewState()["setupJobs"].toArray().isEmpty(),2000);
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
 void shortcutRecordingStoresTheUnshiftedKey(){
  // A US map for scan code 13: Shift+4 is $. KDE never sends Ctrl+Shift+$.
  ShortcutEditor editor;editor.setRecordingControl([](bool,auto reply){reply({});});
  editor.setChordResolver([](const QKeyEvent& event){return event.nativeScanCode()==13?unshiftedChord(event.keyCombination(),'4','$'):event.keyCombination();});
  editor.setActions(Settings::defaults()["actions"].toObject());editor.show();
  auto* record=editor.findChild<QPushButton*>("recordEditShortcut");auto* input=editor.editors().value("edit");QVERIFY(record);QVERIFY(input);
  const auto send=[&](QEvent::Type type,int key,Qt::KeyboardModifiers modifiers,quint32 scan){QKeyEvent event(type,key,modifiers,scan,0,0);QApplication::sendEvent(input,&event);};
  const auto shift=Qt::ControlModifier|Qt::ShiftModifier;
  record->click();send(QEvent::KeyPress,Qt::Key_Control,Qt::ControlModifier,37);send(QEvent::KeyPress,Qt::Key_Shift,shift,50);
  send(QEvent::KeyPress,Qt::Key_Dollar,shift,13);
  QVERIFY(editor.findChild<QLabel*>("shortcutRecordingStatus")->text().startsWith(QKeySequence(shift|Qt::Key_4).toString(QKeySequence::NativeText)));
  send(QEvent::KeyRelease,Qt::Key_Dollar,shift,13);send(QEvent::KeyRelease,Qt::Key_Shift,Qt::ControlModifier,50);send(QEvent::KeyRelease,Qt::Key_Control,Qt::NoModifier,37);
  QCOMPARE(input->text(),QString("Ctrl+Shift+4"));
  // Letters keep Qt's key.
  record->click();send(QEvent::KeyPress,Qt::Key_Control,Qt::ControlModifier,37);send(QEvent::KeyPress,Qt::Key_Shift,shift,50);
  send(QEvent::KeyPress,Qt::Key_E,shift,26);send(QEvent::KeyRelease,Qt::Key_E,shift,26);send(QEvent::KeyRelease,Qt::Key_Shift,Qt::ControlModifier,50);send(QEvent::KeyRelease,Qt::Key_Control,Qt::NoModifier,37);
  QCOMPARE(input->text(),QString("Ctrl+Shift+E"));
 }
 void shortcutStatusComparesTheEffectiveBinding(){
  ShortcutEditor editor;editor.setActions(Settings::defaults()["actions"].toObject());editor.show();
  auto* input=editor.editors().value("region");auto* label=editor.findChild<QLabel*>("shortcutRegionStatus");QVERIFY(label);
  input->setText("Ctrl+Shift+1");
  const auto status=[&](const QJsonObject& entry){editor.setStatus({{"available",true},{"bindings",QJsonObject{{"region",entry}}}});};
  // KDE holds Ctrl+Shift+1 as Ctrl+!, which is what was asked of it: no line.
  status({{"requested","Ctrl+Shift+1"},{"effective","Ctrl+!"},{"actual",QJsonArray{"Ctrl+!"}},{"bound",true},{"state","bound"}});
  QVERIFY(label->isHidden());
  // A desktop binding other than the effective chord is reported.
  status({{"requested","Ctrl+Shift+1"},{"effective","Ctrl+!"},{"actual",QJsonArray{"Ctrl+Shift+!"}},{"bound",true},{"state","bound"}});
  QVERIFY(!label->isHidden());QCOMPARE(label->text(),QString("Active  ·  Ctrl+Shift+!"));
  // The effective chord of an older request does not vouch for a newer draft.
  status({{"requested","Ctrl+Shift+2"},{"effective","Ctrl+@"},{"actual",QJsonArray{"Ctrl+@"}},{"bound",true},{"state","bound"}});
  QVERIFY(!label->isHidden());
  // Backends without an effective chord still compare the draft.
  status({{"requested","Ctrl+Shift+1"},{"actual",QJsonArray{"Ctrl+Shift+1"}},{"bound",true},{"state","bound"}});
  QVERIFY(label->isHidden());
 }
 void shortcutRecordingCancellationAndFailuresReleaseSuspension(){
  QList<bool> transitions;std::function<void(Error)> ready;ShortcutEditor editor;
  editor.setRecordingControl([&](bool active,auto reply){transitions.append(active);if(active)ready=reply;else reply({});});
  editor.setActions(Settings::defaults()["actions"].toObject());editor.show();putgd::test::settle(1);
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
  editor.setActions(Settings::defaults()["actions"].toObject());editor.show();putgd::test::settle(1);
  auto* record=editor.findChild<QPushButton*>("recordRegionShortcut");auto* input=editor.editors().value("region");auto* status=editor.findChild<QLabel*>("shortcutRecordingStatus");
  record->click();QTest::keyClick(input,Qt::Key_1,Qt::ControlModifier);const auto recorded=status->text();
  restored({"shortcut_recording_interrupted","Global shortcut configuration changed.",{}});QCOMPARE(status->text(),recorded);QCOMPARE(input->text(),QString("Ctrl+1"));
  record->click();QTest::keyClick(input,Qt::Key_Escape);restored({"shortcut_restore_failed","Synthetic restore failure",{}});QVERIFY(status->text().contains("Could not restore shortcuts"));QCOMPARE(input->text(),QString("Ctrl+1"));
 }
 // #344: Channel offers Stable and Nightly and starts on this build's own; the
 // build's own is stored as "build", the other one explicitly.
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
  apply(true,"stable","build",{});PUTGD_TRY_COMPARE(channel->currentData().toString(),QString("stable"));QVERIFY(channel->isVisibleTo(window.settings()));
  apply(true,"nightly","build",{});PUTGD_TRY_COMPARE(channel->currentData().toString(),QString("nightly"));
  // Showing the build's channel saves nothing: an untouched file keeps no updates object.
  PUTGD_COMPARE_STEADY(stored(),QString("(none)"),200);
  // The other channel is stored by name, and the build's own as "build".
  channel->setCurrentIndex(0);PUTGD_TRY_COMPARE(stored(),QString("stable"));
  apply(true,"nightly","stable",{});PUTGD_TRY_VERIFY(window.findChild<QLabel*>("updateStatus")->text().contains("Not checked yet"));QCOMPARE(channel->currentData().toString(),QString("stable"));
  channel->setCurrentIndex(1);PUTGD_TRY_COMPARE(stored(),QString("build"));
  // A stored "build" from before #344 shows as this build's channel too.
  apply(true,"stable","build",{});PUTGD_TRY_COMPARE(channel->currentData().toString(),QString("stable"));
  // A development build or Flatpak never checks: the row is hidden and nothing is rewritten.
  apply(false,QJsonValue(),"build","development_build");PUTGD_TRY_VERIFY(!channel->isVisibleTo(window.settings()));
  window.findChild<QCheckBox*>("notifications")->setChecked(!window.findChild<QCheckBox*>("notifications")->isChecked());PUTGD_COMPARE_STEADY(stored(),QString("build"),300);
  apply(false,"stable","build","flatpak");PUTGD_TRY_VERIFY(!channel->isVisibleTo(window.settings()));
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
  PUTGD_TRY_COMPARE(channel->currentData().toString(),expected);QCOMPARE(channel->isEnabled(),!managed);
  QCOMPARE(source->isVisibleTo(window.settings()),managed);QCOMPARE(text->isVisibleTo(window.settings()),managed);
  QVERIFY(line->text().startsWith("Not checked yet"));
  QJsonObject wire;fixture.server.dispatch({{"schemaVersion",1},{"requestId","startup-update"},{"session",fixture.server.socketPath()},{"method","update.status"},{"params",QJsonObject{}}},[&](Error error,QJsonObject data){QVERIFY(!error);wire=data;});
  QCOMPARE(wire,updates.status());QCOMPARE(wire["channel"].toString(),expected);QVERIFY(wire["attemptedAt"].isNull());
  auto* notifications=window.findChild<QCheckBox*>("notifications");const bool before=notifications->isChecked();notifications->setChecked(!before);
  PUTGD_TRY_COMPARE(service.snapshot().notifications,!before);PUTGD_COMPARE_STEADY(service.snapshot().updateChannel,chosen,300);
  if(!managed){
   channel->setCurrentIndex(channel->findData(expected=="stable"?"nightly":"stable"));
   PUTGD_TRY_COMPARE(service.snapshot().updateChannel,expected=="stable"?QString("build"):QString("stable"));
  }
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
  QVERIFY(line->text().startsWith("Up to date: 26.1022.968-nightly is the newest nightly version."));
  // Moved to stable by hand, whose release is older: said plainly, with the command that installs it anyway.
  apply("26.1022.968-nightly","nightly","stable","26.1020.950",false,{{"version","26.1020.950"},{"command","sudo apt install --allow-downgrades putgd/stable"}});
  PUTGD_TRY_COMPARE(channel->currentData().toString(),QString("stable"));QVERIFY(!channel->isEnabled());
  QVERIFY(line->text().startsWith("Your system now updates put.gd from the stable repository. Stable 26.1020.950 is older than this nightly build (26.1022.968-nightly), so the system updater keeps this version until a newer stable release is out."));
  QVERIFY(command->isVisibleTo(settings));QCOMPARE(command->text(),QString("sudo apt install --allow-downgrades putgd/stable"));
  QVERIFY(text->text().startsWith("To move to nightly"));QVERIFY(commands->text().endsWith("putgd/nightly"));
  // No stable release yet: the nightly stays until the first one.
  apply("26.1022.968-nightly","nightly","stable",QJsonValue(),false);
  PUTGD_TRY_VERIFY(line->text().startsWith("There's no stable release yet. Your system updates put.gd from the stable repository, so this nightly build (26.1022.968-nightly) stays as it is"));
  QVERIFY(!command->isVisibleTo(settings));
  // A newer stable release is an ordinary update through the system's updater.
  apply("26.1022.968-nightly","nightly","stable","26.1101.990",true);
  PUTGD_TRY_VERIFY(open->isVisibleTo(settings));QVERIFY(line->text().startsWith("put.gd 26.1101.990 is available from put.gd's stable repository."));
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
  PUTGD_TRY_VERIFY(line->text().startsWith("There's no stable release yet. This nightly build (0.1.1-nightly.900) stays as it is"));
  QVERIFY(!line->text().contains("Up to date"));QVERIFY(!install->isVisibleTo(settings));
  // The first stable release: Switch to stable 0.1.1.
  apply("0.1.1-nightly.900","nightly","stable","0.1.1",true,{{"state","idle"},{"kind","switch"},{"available",true},{"running",false},{"restartRequired",false},
   {"switch",QJsonObject{{"from","nightly"},{"to","stable"},{"version","0.1.1"},{"tag","v0.1.1"},{"older",false}}}});
  PUTGD_TRY_COMPARE(install->text(),QString("Switch to stable 0.1.1"));QVERIFY(install->isVisibleTo(settings));
  QVERIFY(line->text().startsWith("Stable 0.1.1 is available. This is nightly 0.1.1-nightly.900; switching replaces it with the stable release."));
  // Without an install service here the click's refusal shows; it was asked for.
  install->click();PUTGD_TRY_VERIFY(status->text().contains("does not check for updates"));
  // Back from stable to an older nightly: labelled, and confirmed first.
  apply("0.1.1","stable","nightly","0.1.1-nightly.901",false,{{"state","idle"},{"kind","switch"},{"available",true},{"running",false},{"restartRequired",false},
   {"switch",QJsonObject{{"from","stable"},{"to","nightly"},{"version","0.1.1-nightly.901"},{"tag","v0.1.1-nightly.901"},{"older",true}}}});
  PUTGD_TRY_COMPARE(install->text(),QString("Switch to nightly 0.1.1-nightly.901 (older)"));
  QVERIFY(line->text().startsWith("Nightly 0.1.1-nightly.901 is older than this stable build (0.1.1)."));
  QString asked;
  QTimer dismiss;connect(&dismiss,&QTimer::timeout,this,[&]{if(auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())){asked=box->text();box->button(QMessageBox::Cancel)->click();}});dismiss.start(10);
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
  QVERIFY(line->text().startsWith("Up to date: 0.1.1 is the newest stable version."));QVERIFY(!install->isVisibleTo(settings));
 }
 void railShowsTheResidentVersionAndOpensItsGitHubPage(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();PUTGD_TRY_VERIFY(fixture.session.ready());
  auto* rail=window.findChild<QWidget*>("statusBand");auto* label=window.findChild<QLabel*>("appVersion");auto* link=window.findChild<QPushButton*>("appVersionLink");QVERIFY(rail&&label&&link);QVERIFY(rail->isAncestorOf(label)&&rail->isAncestorOf(link));
  // The real build identity: the full compiled version, as diagnostics and About report it.
  const auto build=QJsonDocument::fromJson(buildManifestJson()).object();QCOMPARE(fixture.session.build(),build);const auto current=releaseLink(build);QWidget* shown=current.url.isEmpty()?static_cast<QWidget*>(label):link;
  PUTGD_TRY_VERIFY(shown->isVisible());QCOMPARE(shown->property("text").toString().remove(QChar(0x200B)),QString::fromLatin1(buildVersion()));QCOMPARE(shown->accessibleName(),"put.gd version "+QString::fromLatin1(buildVersion()));
  QVERIFY2(shown->mapTo(&window,QPoint(shown->width(),shown->height())).x()>window.width()-shown->width()-40,"The version sits at the right of the status band");
  openedUrls_.clear();host::setTestOpener([this](const QUrl& url){recordProtonUrl(url);});auto cleanup=qScopeGuard([]{host::setTestOpener({});});
  QJsonObject snapshot;fixture.server.dispatch({{"schemaVersion",1},{"requestId","version"},{"session",fixture.server.socketPath()},{"method","ui.snapshot"},{"params",QJsonObject{}}},[&](Error e,QJsonObject data){QVERIFY(!e);snapshot=data;});PUTGD_TRY_VERIFY(!snapshot.isEmpty());
  auto apply=[&](const QString& version,bool dirty){auto injected=build;injected["version"]=version;injected["dirty"]=dirty;injected["gitCommit"]="528fe444b0cb0123456789abcdef0123456789ab";auto data=snapshot;data["build"]=injected;fixture.session.applySnapshot(data);};
  apply("0.1.1-nightly.430",false);PUTGD_TRY_VERIFY(link->isVisible());QVERIFY(!label->isVisible());QCOMPARE(link->text(),QString("0.1.1-nightly.430"));QCOMPARE(link->accessibleName(),QString("put.gd version 0.1.1-nightly.430"));QVERIFY(link->focusPolicy()&Qt::TabFocus);
  link->setFocus();QTest::keyClick(link,Qt::Key_Space);PUTGD_TRY_COMPARE(openedUrls_.size(),1);QCOMPARE(openedUrls_.first(),QUrl("https://github.com/patrick-hudson/put.gd-desktop/releases/tag/v0.1.1-nightly.430"));
  // A development build has no GitHub release, even when its commit is pushed: plain text.
  const QString clean="0.1.1-dev.430+g528fe444b0cb";apply(clean,false);PUTGD_TRY_VERIFY(label->isVisible());QVERIFY(!link->isVisible());QCOMPARE(label->text().remove(QChar(0x200B)),clean);QVERIFY2(label->height()>=label->fontMetrics().height(),"A clean development version shows on one line");
  // Dirty development builds have no public page: the full string stays readable, unlinked, wrapping after "+" if needed.
  const QString dirty="0.1.1-dev.430+g528fe444b0cb.dirty";apply(dirty,true);PUTGD_TRY_VERIFY(label->isVisible());QVERIFY(!link->isVisible());QCOMPARE(label->text().remove(QChar(0x200B)),dirty);QCOMPARE(label->accessibleName(),"put.gd version "+dirty);
  PUTGD_TRY_VERIFY2(window.rect().contains(QRect(label->mapTo(&window,QPoint()),label->size()))&&label->width()>=label->fontMetrics().horizontalAdvance(dirty),"The longest development version shows whole in the status band");QCOMPARE(openedUrls_.size(),1);
 }
};
QTEST_MAIN(SettingsGui)
#include "test_settings.moc"
