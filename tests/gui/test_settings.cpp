#include "ui/MainWindow.h"
#include "core/BuildInfo.h"
#include "core/ReleaseLink.h"
#include "ui/VisualStyle.h"
#include "ui/RedesignTheme.h"
#include <QVBoxLayout>
#include <QAbstractItemView>
#include <QScrollBar>
#include "ui/SettingsWindow.h"
#include "ui/ShortcutEditor.h"
#include "ui/ViewState.h"
#include "fixtures/UiSessionFixture.h"
#include "storage/s3/S3Provider.h"
#include "storage/proton/ProtonProvider.h"
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
using namespace putgd;using namespace putgd::test;
class SettingsGui:public QObject {
 Q_OBJECT
 QList<QUrl> openedUrls_;
public slots:
 void recordProtonUrl(const QUrl& url){openedUrls_.append(url);}
private slots:
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
  const QHash<QString,QString> labels{{"region","Region capture"},{"window","Window capture"},{"desktop","Desktop capture"},{"clipboard","Clipboard import"},{"edit","Region capture & edit"}};
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
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);int invoked=0;connect(&window,&MainWindow::actionRequested,this,[&](auto){++invoked;});const QHash<QString,QString> labels{{"region","Capture region"},{"window","Capture window"},{"desktop","Capture desktop"},{"clipboard","Import clipboard"},{"edit","Capture region & edit"}};for(const auto& action:Settings::actionNames()){QPushButton* button=nullptr;for(auto* candidate:window.findChildren<QPushButton*>("captureRailButton"))if(candidate->property("captureAction")==action)button=candidate;QVERIFY(button);QCOMPARE(button->accessibleName(),labels.value(action));QVERIFY(button->toolTip().contains("Save locally"));QVERIFY(button->accessibleDescription().contains("clipboard",Qt::CaseInsensitive));}auto* help=window.findChild<QToolButton*>("captureActionsHeadingHelp");QVERIFY(help);help->click();auto* dialog=window.findChild<QMessageBox*>("settingHelpDialog");QVERIFY(dialog);QVERIFY(dialog->informativeText().contains("focused external app window"));dialog->close();
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
  QTemporaryDir dir;CredentialStore secrets;S3Provider s3(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto settings){return s3.validateSettings(settings);});QVERIFY(service.save(Settings::developmentTemplate(),{}));UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);QCOMPARE(window.findChild<QLineEdit*>("maxFileBytes")->text(),QString("2"));QCOMPARE(window.findChild<QLineEdit*>("imagePixels")->text(),QString("100"));QCOMPARE(window.findChild<QLineEdit*>("s3ConnectTimeoutMs")->text(),QString("5"));window.findChild<QLineEdit*>("maxFileBytes")->setText("2.000000000931322574615478515625");window.findChild<QLineEdit*>("imagePixels")->setText("100.000001");window.findChild<QLineEdit*>("s3ConnectTimeoutMs")->setText("5.123");QTRY_COMPARE(service.document()["limits"].toObject()["maxFileBytes"].toInteger(),2147483649LL);QCOMPARE(service.document()["limits"].toObject()["imagePixels"].toInteger(),100000001LL);QCOMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["connectTimeoutMs"].toInt(),5123);QCOMPARE(window.findChild<QLineEdit*>("maxFileBytes")->text(),QString("2.000000000931322574615478515625"));
 }
 void resetOneAndResetAllUseRealDefaults(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));auto doc=configuration(dir.path(),true);auto limits=doc["limits"].toObject();limits["concurrentUploads"]=4;doc["limits"]=limits;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);QPushButton* reset=nullptr;for(auto* button:window.findChildren<QPushButton*>())if(button->accessibleName()=="Reset Uploads at once to default")reset=button;QVERIFY(reset);QVERIFY(reset->isVisible());reset->click();QTRY_COMPARE(service.document()["limits"].toObject()["concurrentUploads"].toInt(),1);window.findChild<QLineEdit*>("maxFiles")->setText("25");QTRY_COMPARE(service.document()["limits"].toObject()["maxFiles"].toInt(),25);
  QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QMessageBox*>(widget))dialog->button(QMessageBox::Yes)->click();});window.findChild<QPushButton*>("resetAdvancedDefaults")->click();QTRY_COMPARE(service.document()["limits"].toObject()["maxFiles"].toInt(),100);
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
 void newDestinationIsDraftUntilValidAndCannotBecomeDefaultWithEmptyBucket(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();QVERIFY(!window.findChild<QPushButton*>("makeDefaultDestination")->isEnabled());window.findChild<QLineEdit*>("s3Bucket")->setText("a");QTest::qWait(600);QVERIFY(service.revision().isEmpty());QVERIFY(window.findChild<QLineEdit*>("s3Bucket")->property("invalid").toBool());
 }
 void optionalDefaultsStayAbsentAndDoNotBlockDestinationSetup(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return provider.validateSettings(value);});auto doc=Settings::developmentTemplate();auto naming=doc["naming"].toObject();for(const auto& key:QStringList{"fileTemplate","captureTemplate","imageTemplate","textTemplate"})naming.remove(key);doc["naming"]=naming;auto limits=doc["limits"].toObject();limits.remove("pendingImageBytes");doc["limits"]=limits;auto preferences=doc["preferences"].toObject();preferences.remove("uploadClipboardBehavior");doc["preferences"]=preferences;QVERIFY(service.save(doc,{}));int tests=0;UiSessionFixture fixture(service,secrets,[&](auto,auto,auto reply){++tests;reply({});});MainWindow window(fixture.session);auto* pending=window.findChild<QLineEdit*>("pendingImageBytes");QVERIFY(pending);QVERIFY(!pending->property("defaultText").toString().isEmpty());QTRY_COMPARE(pending->text(),pending->property("defaultText").toString());window.findChild<QCheckBox*>("notifications")->setChecked(false);QTRY_VERIFY(!service.document()["preferences"].toObject()["notifications"].toBool());QTRY_VERIFY(!window.settings()->saveViewState().contains("stateError"));QVERIFY(!service.document()["naming"].toObject().contains("fileTemplate"));QVERIFY(!service.document()["limits"].toObject().contains("pendingImageBytes"));QVERIFY(!service.document()["preferences"].toObject().contains("uploadClipboardBehavior"));window.findChild<QPushButton*>("testDestination")->click();QTRY_COMPARE(tests,1);
 }
 void failedAutosaveKeepsDraftAcrossReopenAndRequiresExplicitRetry(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));const auto before=service.document();CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);fixture.client.failOnce["settings.save"]={"client_unavailable","Synthetic lost save reply",{}};window.findChild<QLineEdit*>("archiveRoot")->setText(dir.filePath("retry-output"));QTRY_VERIFY(window.settings()->saveViewState()["autosaveBlocked"].toBool());QTest::qWait(600);QCOMPARE(service.document(),before);const auto state=window.settings()->saveViewState();MainWindow reopened(fixture.session);QVERIFY(reopened.settings()->restoreViewState(state));QVERIFY(reopened.findChild<QPushButton*>("retryAutosave")->isHidden()==false);reopened.findChild<QPushButton*>("retryAutosave")->click();QTRY_COMPARE(service.document()["archive"].toObject()["root"].toString(),dir.filePath("retry-output"));QVERIFY(!reopened.settings()->saveViewState()["autosaveBlocked"].toBool());
 }
 void detailedSettingHelpIsReachableFromKeyboard(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);window.show();auto* help=window.findChild<QToolButton*>("maxFileBytesHelp");QVERIFY(help);QVERIFY(help->isVisible());help->setFocus();QTest::keyClick(help,Qt::Key_Space);QMessageBox* dialog=nullptr;QTRY_VERIFY((dialog=window.findChild<QMessageBox*>("settingHelpDialog")));QVERIFY(dialog->informativeText().contains("GiB"));QVERIFY(dialog->informativeText().contains("rejected"));QCOMPARE(dialog->textFormat(),Qt::PlainText);dialog->close();
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
 void railShowsTheResidentVersionAndOpensItsGitHubPage(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();QTRY_VERIFY(fixture.session.ready());
  auto* rail=window.findChild<QWidget*>("mainRail");auto* label=window.findChild<QLabel*>("appVersion");auto* link=window.findChild<QPushButton*>("appVersionLink");QVERIFY(rail&&label&&link);QVERIFY(rail->isAncestorOf(label)&&rail->isAncestorOf(link));
  // The real build identity: the full compiled version, as diagnostics and About report it.
  const auto build=QJsonDocument::fromJson(buildManifestJson()).object();QCOMPARE(fixture.session.build(),build);const auto current=releaseLink(build);QWidget* shown=current.url.isEmpty()?static_cast<QWidget*>(label):link;
  QTRY_VERIFY(shown->isVisible());QCOMPARE(shown->property("text").toString(),QString::fromLatin1(buildVersion()));QCOMPARE(shown->accessibleName(),"put.gd version "+QString::fromLatin1(buildVersion()));
  QVERIFY2(shown->mapTo(rail,QPoint(0,shown->height())).y()>rail->height()-60,"The version sits at the bottom of the rail");
  openedUrls_.clear();QDesktopServices::setUrlHandler("https",this,"recordProtonUrl");auto cleanup=qScopeGuard([]{QDesktopServices::unsetUrlHandler("https");});
  QJsonObject snapshot;fixture.server.dispatch({{"schemaVersion",1},{"requestId","version"},{"session",fixture.server.socketPath()},{"method","ui.snapshot"},{"params",QJsonObject{}}},[&](Error e,QJsonObject data){QVERIFY(!e);snapshot=data;});QTRY_VERIFY(!snapshot.isEmpty());
  auto apply=[&](const QString& version,bool dirty){auto injected=build;injected["version"]=version;injected["dirty"]=dirty;injected["gitCommit"]="528fe444b0cb0123456789abcdef0123456789ab";auto data=snapshot;data["build"]=injected;fixture.session.applySnapshot(data);};
  apply("0.1.1-nightly.430",false);QTRY_VERIFY(link->isVisible());QVERIFY(!label->isVisible());QCOMPARE(link->text(),QString("0.1.1-nightly.430"));QCOMPARE(link->accessibleName(),QString("put.gd version 0.1.1-nightly.430"));QVERIFY(link->focusPolicy()&Qt::TabFocus);
  link->setFocus();QTest::keyClick(link,Qt::Key_Space);QTRY_COMPARE(openedUrls_.size(),1);QCOMPARE(openedUrls_.first(),QUrl("https://github.com/patrick-hudson/put.gd-desktop/releases/tag/v0.1.1-nightly.430"));
  // Dirty development builds have no public page: the full string stays readable, unlinked.
  const QString dirty="0.1.1-dev.430+g528fe444b0cb.dirty";apply(dirty,true);QTRY_VERIFY(label->isVisible());QVERIFY(!link->isVisible());QCOMPARE(label->text(),dirty);QCOMPARE(label->accessibleName(),"put.gd version "+dirty);
  QVERIFY2(label->sizeHint().width()<=rail->contentsRect().width()-28,"The longest development version fits in the rail");QCOMPARE(openedUrls_.size(),1);
 }
};
QTEST_MAIN(SettingsGui)
#include "test_settings.moc"
