#include "ui/MainWindow.h"
#include "core/BuildInfo.h"
#include "core/ReleaseLink.h"
#include "ui/VisualStyle.h"
#include "ui/Graphite.h"
#include "ui/PreferenceRows.h"
#include "ui/Fonts.h"
#include <QToolTip>
#include <QPainter>
#include <QStyleOptionViewItem>
#include <QStatusBar>
#include <QSet>
#include "ui/RedesignTheme.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QProgressBar>
#include <QAbstractItemView>
#include <QScrollBar>
#include <QScrollArea>
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
#include <QRadioButton>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QClipboard>
#include <QJsonArray>
#include <QLabel>
#include <QTextDocument>
#include <QListWidget>
#include <QPushButton>
#include <QTabWidget>
#include <QToolButton>
#include <QStyleOptionToolButton>
#include <QGridLayout>
#include <QMessageBox>
#include <memory>
#include <QInputDialog>
#include <QMenu>
#include <QTabBar>
#include <QWheelEvent>
#include "platform/HostOpen.h"
#include <QScopeGuard>
#include <QAction>
#include <QWidgetAction>
#include <QActionGroup>
#include <QFormLayout>
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
// Chooses the named action of the next confirmation (Graphite::confirm) and
// keeps the text it showed.
std::shared_ptr<QString> acceptNextConfirmation(){
 auto shown=std::make_shared<QString>();
 QTimer::singleShot(0,[shown]{for(auto* widget:QApplication::topLevelWidgets())if(auto* box=qobject_cast<QMessageBox*>(widget);box&&box->objectName()=="confirmAction"&&box->isVisible()){*shown=box->windowTitle()+"\n"+box->property("confirmationText").toString();box->button(QMessageBox::Yes)->click();return;}});
 return shown;
}
// The code behind the Settings status line: people read the message, the code
// stays in a property and the accessible description.
QString statusCode(QWidget& window){return window.findChild<QLabel*>("settingsStatus")->property("errorCode").toString();}
}
class SettingsGui:public QObject {
 Q_OBJECT
 QList<QUrl> openedUrls_;
public slots:
 void recordProtonUrl(const QUrl& url){openedUrls_.append(url);}
private slots:
 void stackedLabelsStayCloseToTheirNextWidget_data(){QTest::addColumn<int>("pixels");QTest::newRow("100")<<14;QTest::newRow("125")<<18;QTest::newRow("150")<<21;}
 void stackedLabelsStayCloseToTheirNextWidget(){
  QFETCH(int,pixels);Fonts::load();VisualStyle::apply(*qApp);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(pixels);qApp->setFont(font);QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1060,900);window.show();int checked=0;
  const auto check=[&](QWidget* page){putgd::test::settle(40);for(auto* value:page->findChildren<QWidget*>())if((value->property("preferenceInlineValue").toBool()||value->property("preferenceChoiceValue").toBool())&&value->isVisible()&&value->property("preferenceChoiceSizer").isValid()&&!value->property("preferenceShortcutValue").toBool()){auto* row=value->parentWidget();auto* label=qobject_cast<QWidget*>(row->property("preferenceLabel").value<QObject*>());QVERIFY(label);auto* captions=label->parentWidget();const bool stacked=row->property("stacked").toBool();const int gap=stacked?value->mapTo(row,QPoint()).y()-captions->mapTo(row,QPoint(0,captions->height())).y():value->mapTo(row,QPoint()).x()-captions->mapTo(row,QPoint(captions->width(),0)).x();QVERIFY2(gap>=0&&gap<=Graphite::space(stacked?Graphite::Space::s:Graphite::Space::l)+1,qPrintable(QString("%1 caption gap %2, stacked %3").arg(value->objectName()).arg(gap).arg(stacked)));++checked;}};
  window.navigate("settings");auto* tabs=window.findChild<QTabWidget*>("settingsTabs");for(int page=0;page<tabs->count();++page){tabs->setCurrentIndex(page);for(auto* toggle:tabs->currentWidget()->findChildren<QToolButton*>())if(toggle->property("graphiteDisclosure").toBool())toggle->setChecked(true);check(tabs->currentWidget());}window.navigate("destinations");window.findChild<QToolButton*>("advancedSettings")->setChecked(true);auto* provider=window.findChild<QComboBox*>("storageProvider");for(const auto* kind:{"s3","backblaze-b2","proton-drive"}){provider->setCurrentIndex(provider->findData(kind));check(window.findChild<QScrollArea*>("destinationScroll")->widget());}QVERIFY(checked>0);
 }
 void railPageHeadersShareTheirTopAndCentre(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1440,960);window.navigate("settings");window.show();putgd::test::settle(30);
  auto* tabs=window.findChild<QTabWidget*>("settingsTabs");int centre=-1;
  for(int page=0;page<tabs->count();++page){tabs->setCurrentIndex(page);putgd::test::settle(30);auto* heading=window.findChild<QLabel*>("settingsPageHeading"+tabs->tabText(page));auto* scroll=qobject_cast<QScrollArea*>(tabs->currentWidget());QCOMPARE(heading->mapTo(scroll->viewport(),QPoint()).y(),qMin(Graphite::pageHeaderTop(),Graphite::space(Graphite::Space::l)));QCOMPARE(heading->height(),Graphite::scaled(Graphite::Metric::control));const int y=heading->mapTo(&window,heading->rect().center()).y();if(centre<0)centre=y;QCOMPARE(y,centre);}
  window.navigate("destinations");putgd::test::settle(30);auto* heading=window.findChild<QLineEdit*>("destinationLabel");auto* more=window.findChild<QPushButton*>("destinationActions");QVERIFY2(qAbs(heading->mapTo(&window,heading->rect().center()).y()-centre)<=1,qPrintable(QString("Destination centre %1, Settings centre %2; title height %3").arg(heading->mapTo(&window,heading->rect().center()).y()).arg(centre).arg(heading->height())));QVERIFY(qAbs(more->mapTo(&window,more->rect().center()).y()-centre)<=1);
 }
 void disclosureIconsSurviveEveryToggleAndProviderChange(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();
  for(auto* toggle:window.settings()->findChildren<QToolButton*>())if(toggle->property("graphiteDisclosure").toBool()){toggle->setChecked(false);const auto closed=toggle->icon().pixmap(toggle->iconSize()).toImage();toggle->setChecked(true);QCOMPARE(toggle->arrowType(),Qt::NoArrow);QVERIFY(!toggle->icon().isNull());QVERIFY(toggle->icon().pixmap(toggle->iconSize()).toImage()!=closed);toggle->setChecked(false);QCOMPARE(toggle->icon().pixmap(toggle->iconSize()).toImage(),closed);}
  window.navigate("destinations");auto* advanced=window.findChild<QToolButton*>("advancedSettings");auto* provider=window.findChild<QComboBox*>("storageProvider");for(const auto& kind:{"s3","backblaze-b2","proton-drive"}){provider->setCurrentIndex(provider->findData(kind));for(bool open:{true,false}){advanced->setChecked(open);QCOMPARE(advanced->arrowType(),Qt::NoArrow);QVERIFY(!advanced->icon().isNull());QCOMPARE(advanced->width(),advanced->iconSize().width());}}
 }
 void desktopSupportKeysResizeAndActionsHaveTheirOwnRole_data(){QTest::addColumn<int>("pixels");QTest::newRow("normal")<<14;QTest::newRow("large")<<21;QTest::newRow("preview")<<0;}
 void desktopSupportKeysResizeAndActionsHaveTheirOwnRole(){
  QFETCH(int,pixels);Fonts::load();VisualStyle::apply(*qApp);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});if(pixels){auto font=original;font.setPixelSize(pixels);qApp->setFont(font);}QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1440,960);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);window.show();window.findChild<QToolButton*>("desktopSupportDetailsToggle")->setChecked(true);putgd::test::settle(30);auto* panel=window.findChild<QWidget*>("desktopSupportDetails");
  for(auto* key:panel->findChildren<QLabel*>())if(key->property("desktopSupportKey").toBool()){QCOMPARE(key->font().weight(),QFont::Normal);QCOMPARE(key->palette().color(QPalette::WindowText),Theme::current().textSecondary);QVERIFY2(key->fontMetrics().horizontalAdvance(key->text())<=key->width(),qPrintable(key->text()));QVERIFY2(key->heightForWidth(key->width())<=qMax(Graphite::lineHeight(Graphite::Type::body),key->fontMetrics().boundingRect(QRect(0,0,10000,10000),Qt::TextWordWrap,key->text()).height()+key->contentsMargins().top()+key->contentsMargins().bottom()),qPrintable(QString("%1: width %2, advance %3, heightForWidth %4, font height %5, minimum height %6, wrapped bounds %7, natural bounds %8, frame %9, indent %10").arg(key->text()).arg(key->width()).arg(key->fontMetrics().horizontalAdvance(key->text())).arg(key->heightForWidth(key->width())).arg(key->fontMetrics().height()).arg(key->minimumHeight()).arg(key->fontMetrics().boundingRect(QRect(0,0,key->width(),10000),Qt::TextWordWrap,key->text()).height()).arg(key->fontMetrics().boundingRect(QRect(0,0,10000,10000),Qt::TextWordWrap,key->text()).width()).arg(key->frameWidth()).arg(key->indent())));}
  auto* grid=qobject_cast<QGridLayout*>(panel->layout());QVERIFY(grid);
  for(auto* key:panel->findChildren<QLabel*>())if(key->property("desktopSupportKey").toBool()){
   int row,column,rowSpan,columnSpan;grid->getItemPosition(grid->indexOf(key),&row,&column,&rowSpan,&columnSpan);auto* item=grid->itemAtPosition(row,1);QVERIFY(item);auto* value=qobject_cast<QLabel*>(item->widget());QVERIFY(value);
   for(auto* label:{key,value}){QCOMPARE(label->alignment(),Qt::AlignLeft|Qt::AlignBottom);QVERIFY(label->minimumHeight()>=Graphite::lineHeight(Graphite::Type::body));}
   QCOMPARE(key->geometry().bottom(),value->geometry().bottom());
  }
  auto* label=window.findChild<QLabel*>("configPathLabel");const int wide=label->width();window.resize(1060,680);putgd::test::settle(30);QVERIFY(label->width()<=wide);for(auto* key:panel->findChildren<QLabel*>())if(key->property("desktopSupportKey").toBool())QVERIFY(key->height()>=key->heightForWidth(key->width()));
  for(const auto& name:{"showConfigInFolder","openConfigEditor","reloadSettings","copyConfigPath","refreshDesktopSupport","copyDesktopDetails"}){auto* action=window.findChild<QPushButton*>(name);QCOMPARE(action->property("visualRole").toString(),QString("textAction"));QCOMPARE(action->font().weight(),QFont::Normal);QCOMPARE(action->font().pixelSize(),Graphite::scaled(13));}
 }
 void updateIntroFollowsSupportAndDailyChecks(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(Settings::defaults(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);QJsonObject update{{"supported",true},{"channel","stable"}};fixture.server.setUiDiagnostics([&]{return QJsonObject{{"update",update}};});MainWindow window(fixture.session);window.navigate("settings");window.show();fixture.session.refreshSnapshot();auto* intro=window.findChild<QLabel*>("updatesIntro");auto* daily=window.findChild<QCheckBox*>("checkForUpdates");QVERIFY(intro&&daily);PUTGD_TRY_VERIFY(intro->isVisible());QVERIFY(intro->text().contains("once a day"));daily->setChecked(false);PUTGD_TRY_VERIFY(intro->text().contains("when you choose Check now"));update["supported"]=false;update["reason"]="development_build";fixture.session.refreshSnapshot();PUTGD_TRY_VERIFY(intro->isHidden());
 }
 void retryRetentionChoicesPreserveStoredValuesAndConsequences(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));auto doc=Settings::defaults();auto retention=doc["retention"].toObject();retention["failedArtifactDays"]=17;doc["retention"]=retention;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);window.show();auto* combo=window.findChild<QComboBox*>("failedArtifactChoice");auto* edit=window.findChild<QLineEdit*>("failedArtifactDays");auto* note=window.findChild<QLabel*>("failedArtifactConsequence");QVERIFY(combo&&edit&&note);QVERIFY(!combo->isEditable());QVERIFY(edit->isHidden());QVERIFY(note->isVisible());QCOMPARE(combo->currentText(),QString("17 days"));QCOMPARE(edit->text(),QString("17"));
  for(const int days:{1,3,7,14,30,90,0,17}){const int index=combo->findData(QString::number(days));QVERIFY(index>=0);combo->setCurrentIndex(index);QMetaObject::invokeMethod(combo,"activated",Q_ARG(int,index));PUTGD_TRY_COMPARE(service.document()["retention"].toObject()["failedArtifactDays"].toInt(-1),days);QCOMPARE(combo->currentText(),days==0?QString("Forever"):days==1?QString("1 day"):QString("%1 days").arg(days));QVERIFY(note->isVisible());QVERIFY(window.findChild<QLabel*>("failedArtifactDaysUnit")->isHidden());QCOMPARE(window.settings()->saveViewState()["numbers"].toObject()["failedArtifactDays"].toString(),QString::number(days));}
  auto state=window.settings()->saveViewState();auto numbers=state["numbers"].toObject();numbers["failedArtifactDays"]="0";state["numbers"]=numbers;QVERIFY(window.settings()->restoreViewState(state));QCOMPARE(combo->currentText(),QString("Forever"));QCOMPARE(combo->currentData().toString(),QString("0"));
  // Reloaded configuration and view checkpoints retain unlisted values, too.
  state=window.settings()->saveViewState();numbers=state["numbers"].toObject();numbers["failedArtifactDays"]="23";state["numbers"]=numbers;QVERIFY(window.settings()->restoreViewState(state));QCOMPARE(combo->currentText(),QString("23 days"));QCOMPARE(combo->currentData().toString(),QString("23"));
 }

 void foreverIsARealChoiceAndStillStoresEmptyRetention(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(Settings::defaults(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");auto* combo=window.findChild<QComboBox*>("historyRetentionChoice");auto* edit=window.findChild<QLineEdit*>("historyRetentionDays");QCOMPARE(combo->currentIndex(),0);QCOMPARE(edit->text(),QString("Forever"));QVERIFY(combo->currentData().toString().isEmpty());QVERIFY(window.settings()->saveViewState()["numbers"].toObject()["historyRetentionDays"].toString().isEmpty());
  combo->setCurrentIndex(1);QMetaObject::invokeMethod(combo,"activated",Q_ARG(int,1));PUTGD_TRY_COMPARE(service.document()["retention"].toObject()["historyRetentionDays"].toInt(),30);combo->setCurrentIndex(0);QMetaObject::invokeMethod(combo,"activated",Q_ARG(int,0));PUTGD_TRY_VERIFY(service.document()["retention"].toObject()["historyRetentionDays"].isNull());QCOMPARE(edit->text(),QString("Forever"));edit->setText("17");QMetaObject::invokeMethod(edit,"editingFinished");PUTGD_TRY_COMPARE(service.document()["retention"].toObject()["historyRetentionDays"].toInt(),17);edit->clear();QMetaObject::invokeMethod(edit,"editingFinished");PUTGD_TRY_VERIFY(service.document()["retention"].toObject()["historyRetentionDays"].isNull());QCOMPARE(edit->text(),QString("Forever"));auto state=window.settings()->saveViewState();QVERIFY(window.settings()->restoreViewState(state));QCOMPARE(edit->text(),QString("Forever"));
  const auto help=combo->property("settingDescription").toString();QVERIFY2(help.contains("Choose Forever to keep history indefinitely, or enter a number of days. "),qPrintable(help));QVERIFY(!help.contains("which is the default"));
 }
 void protonFolderRowsStayInsideTheirCard_data(){QTest::addColumn<int>("pixels");QTest::newRow("normal")<<14;QTest::newRow("large")<<21;}
 void protonFolderRowsStayInsideTheirCard(){
  QFETCH(int,pixels);Fonts::load();VisualStyle::apply(*qApp);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(pixels);qApp->setFont(font);QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=threeDestinations();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();auto settings=ProtonProvider::defaults();settings["accountRootUid"]="synthetic-account";settings["folderUid"]="synthetic-folder";settings["folderName"]="Saved folder";profile["provider"]="proton-drive";profile["settings"]=settings;profiles[0]=profile;doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1440,960);window.navigate("destinations");window.show();putgd::test::settle(30);
  auto* combo=window.findChild<QComboBox*>("protonFolder");auto* create=window.findChild<QPushButton*>("createProtonFolder");auto* refresh=window.findChild<QPushButton*>("inspectProtonAccount");auto* row=combo->parentWidget();while(row&&!row->property("preferenceRow").toBool())row=row->parentWidget();QVERIFY(row);QWidget* caption=static_cast<QWidget*>(row->property("preferenceLabel").value<QObject*>());QVERIFY(caption);if(row->property("stacked").toBool())QVERIFY(combo->mapTo(row,QPoint()).y()>=caption->mapTo(row,QPoint(0,caption->height())).y());else QVERIFY(combo->mapTo(row,QPoint()).x()>=caption->mapTo(row,QPoint(caption->width(),0)).x());for(auto* control:{static_cast<QWidget*>(combo),static_cast<QWidget*>(create),static_cast<QWidget*>(refresh)})QVERIFY(row->rect().contains(QRect(control->mapTo(row,QPoint()),control->size())));if(create->mapTo(row,QPoint()).y()>=combo->mapTo(row,QPoint(0,combo->height())).y()){QCOMPARE(create->mapTo(row,QPoint(create->width(),0)).x(),combo->mapTo(row,QPoint(combo->width(),0)).x());QCOMPARE(refresh->mapTo(row,QPoint()).y(),create->mapTo(row,QPoint()).y());}
 }
 void formsShareGutterAndAlignToFirstControlLine_data(){QTest::addColumn<int>("pixels");QTest::newRow("normal")<<14;QTest::newRow("large")<<21;}
 void formsShareGutterAndAlignToFirstControlLine(){
  QFETCH(int,pixels);Fonts::load();VisualStyle::apply(*qApp);const auto oldFont=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(oldFont);});auto font=oldFont;font.setPixelSize(pixels);qApp->setFont(font);
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();auto* tabs=window.findChild<QTabWidget*>("settingsTabs");
  auto* notifications=window.findChild<QCheckBox*>("notifications");putgd::test::settle(30);auto* preferences=notifications->parentWidget();while(preferences&&!preferences->findChild<QFormLayout*>())preferences=preferences->parentWidget();QVERIFY(preferences);auto* preferencesForm=preferences->findChild<QFormLayout*>();QVERIFY(preferencesForm);QVERIFY(!preferencesForm->labelForField(notifications->parentWidget()));
  int gutter=0;
  const auto check=[&](QWidget* page){for(auto* form:page->findChildren<QFormLayout*>())for(int row=0;row<form->rowCount();++row){auto* item=form->itemAt(row,QFormLayout::LabelRole);if(!item||!item->widget())continue;auto* label=item->widget();PUTGD_TRY_VERIFY(label->minimumWidth()>0);if(!gutter)gutter=label->minimumWidth();QCOMPARE(label->minimumWidth(),gutter);QVERIFY(gutter<=Graphite::scaled(240));QVERIFY(label->height()>=Graphite::scaled(Graphite::Metric::control));QVERIFY(label->minimumWidth()+16<=Graphite::Metric::labelGutter);QVERIFY(form->labelAlignment().testFlag(Qt::AlignBaseline));}};
  for(int page=0;page<tabs->count();++page){tabs->setCurrentIndex(page);putgd::test::settle(30);check(tabs->currentWidget());}
  window.navigate("destinations");auto* bucket=window.findChild<QLineEdit*>("s3Bucket");auto* error=window.findChild<QLabel*>("s3BucketError");QVERIFY(bucket&&error);error->setText("Synthetic validation error that wraps under the control.");error->show();putgd::test::settle(30);gutter=0;check(window.findChild<QScrollArea*>("destinationScroll")->widget());
  QLabel* caption=nullptr;for(auto* label:window.findChildren<QLabel*>())if(label->buddy()==bucket)caption=label;QVERIFY(caption);QVERIFY(!QRect(caption->mapTo(&window,QPoint()),caption->size()).intersects(QRect(bucket->mapTo(&window,QPoint()),bucket->size())));QCOMPARE(bucket->width(),window.findChild<QComboBox*>("s3RegionChoices")->width());QVERIFY(error->mapTo(&window,QPoint()).y()>=bucket->mapTo(&window,QPoint(0,bucket->height())).y());
  auto* provider=window.findChild<QComboBox*>("storageProvider");provider->setCurrentIndex(provider->findData("proton-drive"));putgd::test::settle(30);gutter=0;check(window.findChild<QScrollArea*>("destinationScroll")->widget());
 }
 void preferenceTextAndFirstControlGeometry_data(){
  QTest::addColumn<double>("scale");QTest::addColumn<QSize>("size");
  for(double scale:{1.0,1.25,1.5})for(const auto size:{QSize(1440,960),QSize(1060,680)})QTest::newRow(qPrintable(QString("%1-%2").arg(scale).arg(size.width())))<<scale<<size;
 }
 void preferenceTextAndFirstControlGeometry(){
  QFETCH(double,scale);QFETCH(QSize,size);Fonts::load();VisualStyle::apply(*qApp);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPointSizeF(14*scale*72/96);qApp->setFont(font);
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(size);window.navigate("settings");window.show();auto* tabs=window.findChild<QTabWidget*>("settingsTabs");
  const auto check=[&](QWidget* page){
   int labels=0;
   for(auto* form:page->findChildren<QFormLayout*>()){
    auto* card=form->parentWidget();if(!card->isVisibleTo(page)||!card->property("preferenceCard").toBool())continue;
    for(int row=0;row<form->rowCount();++row){
     auto* item=form->itemAt(row,QFormLayout::FieldRole);auto* field=item?item->widget():nullptr;if(!field||!form->isRowVisible(row))continue;
     if(auto* holder=qobject_cast<QWidget*>(field->property("preferenceLabel").value<QObject*>())){
      auto* caption=qobject_cast<QLabel*>(holder);if(!caption)caption=holder->findChild<QLabel*>();if(!caption||caption->isHidden())continue;++labels;
      QCOMPARE(caption->mapTo(card,QPoint()).x(),Graphite::space(Graphite::Space::l));
      auto* control=caption->buddy();if(!control||qobject_cast<QLabel*>(control)||field->property("preferenceStacked").toBool()||field->property("stacked").toBool())continue;
      if(qobject_cast<QComboBox*>(control->parentWidget())||qobject_cast<QPushButton*>(control->parentWidget()))control=control->parentWidget();
      const int delta=caption->mapTo(card,caption->rect().center()).y()-control->mapTo(card,control->rect().center()).y();
      QVERIFY2(qAbs(delta)<=2,qPrintable(QString("%1: label/control centre delta %2").arg(control->objectName()).arg(delta)));
     }
     if(auto* toggle=qobject_cast<QToolButton*>(field);toggle&&(toggle->property("preferenceDisclosure").toBool()||toggle->property("preferenceAddRow").toBool())){
      const int inset=Graphite::space(Graphite::Space::l);QCOMPARE(toggle->mapTo(card,QPoint(inset,0)).x(),inset);
      // Antialiased glyph edges need not contain the exact foreground colour.
      // Measure visible ink against the backdrop, in device pixels, away from
      // focus outlines, trailing glyphs and card borders.
      const QImage actual=toggle->grab().toImage();const qreal dpr=actual.devicePixelRatio();const QColor background=actual.pixelColor(qRound(2*dpr),actual.height()/2);
      QImage expected(actual.size(),QImage::Format_ARGB32_Premultiplied);expected.setDevicePixelRatio(dpr);expected.fill(background);const QColor ink=toggle->property("preferenceAddRow").toBool()?Theme::current().textSecondary:Theme::current().text;
      const int trailing=toggle->property("preferenceAddRow").toBool()?toggle->iconSize().width()+Graphite::space(Graphite::Space::s):Graphite::scaled(24);
      QPainter painter(&expected);Graphite::paintText(painter,QRect(inset,0,toggle->width()-2*inset-trailing,toggle->height()),toggle->text(),ink,Graphite::font());painter.end();
      const auto contrast=[&](const QColor& c){return qAbs(c.red()-background.red())+qAbs(c.green()-background.green())+qAbs(c.blue()-background.blue());};
      const auto first=[&](const QImage& image){for(int x=qCeil(2*dpr);x<image.width()/2;++x)for(int y=qCeil(2*dpr);y<image.height()-qCeil(2*dpr);++y)if(contrast(image.pixelColor(x,y))>=contrast(ink)/4)return x;return -1;};
      const int actualStart=first(actual),expectedStart=first(expected);QVERIFY(actualStart>=0);QVERIFY(expectedStart>=0);
      QVERIFY2(qAbs(actualStart-expectedStart)/dpr<=1,qPrintable(QString("%1 (%2) actual %3 expected %4 DPR %5 font %6").arg(toggle->objectName(),toggle->text()).arg(actualStart/dpr).arg(expectedStart/dpr).arg(dpr).arg(toggle->font().toString())));
     }
    }
   }
   QVERIFY(labels>0);
  };
  for(int page=0;page<tabs->count();++page){tabs->setCurrentIndex(page);for(auto* toggle:tabs->currentWidget()->findChildren<QToolButton*>())if(toggle->property("graphiteDisclosure").toBool())toggle->setChecked(true);putgd::test::settle(40);check(tabs->currentWidget());}
  window.navigate("destinations");auto* scroll=window.findChild<QScrollArea*>("destinationScroll");auto* provider=window.findChild<QComboBox*>("storageProvider");window.findChild<QToolButton*>("advancedSettings")->setChecked(true);
  for(const auto* kind:{"s3","backblaze-b2","proton-drive"}){provider->setCurrentIndex(provider->findData(kind));putgd::test::settle(40);check(scroll->widget());}
  provider->setCurrentIndex(provider->findData("s3"));auto* mode=window.findChild<QComboBox*>("credentialMode");mode->setCurrentIndex(mode->currentData()=="session"?mode->findData("keychain"):mode->findData("session"));putgd::test::settle(40);check(scroll->widget());
 }
 void formHintsAndTrailingActionsKeepTheirGeometry_data(){QTest::addColumn<int>("pixels");QTest::newRow("normal")<<14;QTest::newRow("large")<<21;}
 void formHintsAndTrailingActionsKeepTheirGeometry(){
  QFETCH(int,pixels);Fonts::load();VisualStyle::apply(*qApp);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(pixels);qApp->setFont(font);
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1440,960);window.show();window.navigate("settings");auto* tabs=window.findChild<QTabWidget*>("settingsTabs");
  const auto check=[&](QWidget* page){for(auto* hint:page->findChildren<QLabel*>()){if(!hint->objectName().endsWith("Hint")||!hint->isVisibleTo(page))continue;auto* holder=hint->parentWidget();if(holder->property("visualRole").toString()!="settingRow")continue;auto* layout=qobject_cast<QVBoxLayout*>(holder->layout());QVERIFY(layout);auto* line=layout->itemAt(0)->layout();QVERIFY(line);int bottom=0;for(int i=0;i<line->count();++i)if(auto* field=line->itemAt(i)->widget();field&&field->isVisibleTo(holder))bottom=qMax(bottom,field->mapTo(holder,QPoint(0,field->height())).y());QVERIFY2(hint->y()-bottom>=Graphite::space(Graphite::Space::xs),qPrintable(QString("%1 gap %2; hint y/h %3/%4, holder h %5, line y/h %6/%7, minimum h %8, hfw %9").arg(hint->objectName()).arg(hint->y()-bottom).arg(hint->y()).arg(hint->height()).arg(holder->height()).arg(line->geometry().y()).arg(line->geometry().height()).arg(layout->minimumSize().height()).arg(layout->heightForWidth(holder->width()))));}};
  for(int i=0;i<tabs->count();++i){tabs->setCurrentIndex(i);putgd::test::settle(30);check(tabs->currentWidget());}
  window.navigate("destinations");putgd::test::settle(30);check(window.findChild<QScrollArea*>("destinationScroll")->widget());auto* acl=window.findChild<QComboBox*>("s3AclMode");auto* aclHint=window.findChild<QLabel*>("s3AclModeHint");QVERIFY(acl&&aclHint);auto* scroll=window.findChild<QScrollArea*>("destinationScroll");scroll->ensureWidgetVisible(aclHint,0,40);putgd::test::settle(100);QVERIFY(aclHint->isVisible());QVERIFY(!QRect(aclHint->mapTo(&window,QPoint()),aclHint->size()).intersects(QRect(acl->mapTo(&window,QPoint()),acl->size())));QVERIFY(window.findChild<QLineEdit*>("s3PublicUrlBase")->mapTo(&window,QPoint()).y()-aclHint->mapTo(&window,QPoint(0,aclHint->height())).y()>=Graphite::space(Graphite::Space::m));auto* secret=window.findChild<QLineEdit*>("credentialSecretKey");auto* show=window.findChild<QAction*>("showCredentialSecretAction");QVERIFY(show);QVERIFY(secret->actions().contains(show));QVERIFY(!show->icon().isNull());QVERIFY(window.findChild<QPushButton*>("showCredentialSecret")->isHidden());
  auto* example=window.findChild<QLabel*>("linkExample");QLabel* caption=nullptr;for(auto* label:window.findChildren<QLabel*>())if(label->text()=="Example:")caption=label;QVERIFY(caption);const int labelBaseline=caption->mapTo(&window,QPoint()).y()+(caption->height()-caption->fontMetrics().height())/2+caption->fontMetrics().ascent();const int valueBaseline=example->mapTo(&window,QPoint()).y()+(example->height()-example->fontMetrics().height())/2+example->fontMetrics().ascent();QVERIFY(qAbs(labelBaseline-valueBaseline)<=1);
  window.findChild<QToolButton*>("advancedSettings")->setChecked(true);putgd::test::settle(30);auto* custom=window.findChild<QCheckBox*>("customEndpoint");auto* bucket=window.findChild<QLineEdit*>("s3Bucket");QLabel* bucketCaption=nullptr;for(auto* text:window.findChildren<QLabel*>())if(text->buddy()==bucket)bucketCaption=text;QVERIFY(bucketCaption);QCOMPARE(custom->mapTo(&window,QPoint()).x(),bucketCaption->mapTo(&window,QPoint()).x());
  auto* list=window.findChild<QListWidget*>("destinationList");list->setCurrentRow(2);auto* provider=window.findChild<QComboBox*>("storageProvider");provider->setCurrentIndex(provider->findData("proton-drive"));putgd::test::settle(30);auto* links=window.findChild<QRadioButton*>("protonCreateShareLink");QVERIFY(links);QCOMPARE(links->mapTo(&window,QPoint()).x(),window.findChild<QLabel*>("protonAccountStatus")->mapTo(&window,QPoint()).x());QVERIFY(!window.findChild<QPushButton*>("protonCopyingNote"));check(window.findChild<QScrollArea*>("destinationScroll")->widget());list->setCurrentRow(0);putgd::test::settle(50);scroll->ensureWidgetVisible(aclHint,0,40);putgd::test::settle(100);QVERIFY(aclHint->isVisible());QVERIFY(!QRect(aclHint->mapTo(&window,QPoint()),aclHint->size()).intersects(QRect(acl->mapTo(&window,QPoint()),acl->size())));
 }
 void outputControlHidesOnlyOnCapture(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(Settings::defaults(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.show();auto* output=window.findChild<QToolButton*>("captureOutputSummary");auto* warning=window.findChild<QWidget*>("commandOutputWarning");QVERIFY(output->isVisible());QVERIFY(warning->isVisible());QTest::keyClick(output,Qt::Key_Return);PUTGD_TRY_VERIFY(output->menu()->isVisible());output->menu()->close();window.findChild<QAction*>("changeCaptureOutput")->trigger();QCOMPARE(window.saveViewState()["page"].toInt(),2);QVERIFY(!output->isVisible());QVERIFY(!warning->isVisible());auto* tabs=window.findChild<QTabWidget*>("settingsTabs");tabs->setCurrentIndex(0);QVERIFY(output->isVisible());QVERIFY(warning->isVisible());tabs->setCurrentIndex(1);QVERIFY(!output->isVisible());window.navigate("destinations");QVERIFY(output->isVisible());QVERIFY(warning->isVisible());
 }
 void developmentUpdatesExplainWhyCheckingIsUnavailable(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(Settings::defaults(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);fixture.server.setUiDiagnostics([]{return QJsonObject{{"update",QJsonObject{{"supported",false},{"reason","development_build"}}}};});fixture.session.refreshSnapshot();PUTGD_TRY_VERIFY(!fixture.session.diagnostics()["update"].toObject().isEmpty());MainWindow window(fixture.session);window.navigate("settings");window.show();QCOMPARE(window.findChild<QLabel*>("updateStatus")->text(),QString("Update checks aren't available for development builds"));QVERIFY(window.findChild<QPushButton*>("checkForUpdatesNow")->isHidden());
 }
 void emptyDefaultRecoveryStartsAtDefaultOutput(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(Settings::defaults(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.show();window.showCaptureSettings(Settings::presentationOrder());
  auto* destination=window.findChild<QComboBox*>("defaultCaptureDestination");PUTGD_TRY_COMPARE(QApplication::focusWidget(),destination);QVERIFY(!window.findChild<QToolButton*>("captureExceptionsToggle")->isChecked());QVERIFY(destination->isVisible());QCOMPARE(window.findChild<QLabel*>("captureSummaryRegion")->text(),QString("Nothing is saved or uploaded"));
 }
 void shortcutDisplayAndClearingFollowItsValue(){
  ShortcutEditor editor;editor.setActions(Settings::defaults()["actions"].toObject());editor.show();QStringList order;for(auto* button:editor.findChildren<QPushButton*>())if(button->objectName().startsWith("record"))order<<button->objectName().mid(6).chopped(8).toLower();QCOMPARE(order,Settings::presentationOrder());
  for(const auto& action:Settings::presentationOrder()){auto* input=editor.editors().value(action);auto* clear=editor.findChild<QPushButton*>("clear"+action.left(1).toUpper()+action.mid(1)+"Shortcut");QVERIFY(input->isReadOnly());QVERIFY(!input->hasFrame());QVERIFY(clear->parentWidget()->isAncestorOf(input));QCOMPARE(clear->isEnabled(),!input->text().isEmpty());input->setText("Ctrl+1");QVERIFY(clear->isEnabled());QVERIFY(!clear->isHidden());QCOMPARE(clear->iconSize(),QSize(Graphite::scaled(12),Graphite::scaled(12)));QVERIFY(!clear->icon().isNull());clear->click();QVERIFY(input->text().isEmpty());QVERIFY(!clear->isEnabled());QVERIFY(clear->isHidden());QCOMPARE(input->placeholderText(),QString("Not set"));}
 }
 void shortcutSupportRetriesWhenItsWindowRegainsFocus(){
  ShortcutEditor editor;editor.setActions(Settings::defaults()["actions"].toObject());int retries=0;editor.setRefreshAction([&]{++retries;});editor.show();putgd::test::settle(10);auto* refresh=editor.findChild<QPushButton*>("refreshShortcutSupport");QVERIFY(refresh);QCOMPARE(refresh->property("visualRole").toString(),QString("quiet"));const int before=retries;QEvent activated(QEvent::WindowActivate);QApplication::sendEvent(&editor,&activated);QCOMPARE(retries,before+1);editor.setStatus({{"available",true},{"bindings",QJsonObject{}}});
 }
 void topLevelPagesShareOneRailEdge(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.show();
  for(const auto& page:{"history","settings","destinations"}){window.navigate(page);putgd::test::settle(20);for(const auto& name:{"historySidebar","settingsRail","destinationRail"})if(auto* rail=window.findChild<QWidget*>(name))if(rail->objectName()=="destinationRail")QVERIFY(rail->width()>=Graphite::railWidth()&&rail->width()<=Graphite::railWidth()+2*Graphite::space(Graphite::Space::xl));else QCOMPARE(rail->width(),Graphite::railWidth());}
 }
 void technicalPlaceholdersKeepTheValueTypeface(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(Settings::defaults(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings()->showLocalCopySettings();window.show();auto* input=window.findChild<QLineEdit*>("folderImages");QVERIFY(input);QCOMPARE(input->placeholderText(),QString("Same as Save to"));QCOMPARE(input->font().family(),Graphite::monoFamily());input->setText("synthetic");QCOMPARE(input->font().family(),Graphite::monoFont(Graphite::Type::technical).family());input->clear();QCOMPARE(input->font().family(),Graphite::monoFamily());
 }
 void inlineIdentifiersKeepTheInterfaceTypeface(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();
  for(const auto& name:{"s3Bucket","s3ObjectPrefix","s3Region"}){auto* edit=window.findChild<QLineEdit*>(name);QVERIFY(edit);edit->clear();QCOMPARE(edit->font().family(),Graphite::uiFamily());edit->setText("synthetic");QCOMPARE(edit->font().family(),Graphite::uiFamily());QCOMPARE(edit->font().pixelSize(),Graphite::scaled(Graphite::Type::body));}
 }
 void linkExampleFollowsDomainAndDisclosureCaptionsAreClickable(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();auto* field=window.findChild<QLineEdit*>("s3PublicUrlBase");auto* example=window.findChild<QLabel*>("linkExample");auto* row=window.findChild<QWidget*>("advancedConnectionRow");auto* toggle=window.findChild<QToolButton*>("advancedSettings");auto* summary=window.findChild<QLabel*>("connectionSummary");QVERIFY(row&&toggle&&summary&&example);QVERIFY(row->isAncestorOf(toggle));QVERIFY(row->isAncestorOf(summary));QVERIFY(!example->wordWrap());QCOMPARE(example->font().family(),Graphite::monoFamily());QCOMPARE(example->font().pixelSize(),Graphite::scaled(13));QCOMPARE(example->palette().color(QPalette::WindowText),Theme::current().textSecondary);QVERIFY(example->textInteractionFlags().testFlag(Qt::TextSelectableByMouse));field->setFocus();field->setText("https://synthetic.example.invalid");PUTGD_TRY_VERIFY(example->text().startsWith(field->text()));QCOMPARE(example->toolTip(),example->text());QTest::mouseClick(summary,Qt::LeftButton);QVERIFY(toggle->isChecked());QVERIFY(summary->isHidden());QTest::keyClick(toggle,Qt::Key_Space);QVERIFY(!toggle->isChecked());QVERIFY(!summary->isHidden());
 }
 void connectionSummaryShowsOnlyDeparturesFromDefaults(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=threeDestinations();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();auto settings=profile["settings"].toObject();settings["addressing"]="auto";settings["storageClass"]="STANDARD";settings["connectTimeoutMs"]=5000;settings["requestTimeoutMs"]=60000;profile["settings"]=settings;profiles[0]=profile;doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();auto* summary=window.findChild<QLabel*>("connectionSummary");QVERIFY(summary->text().isEmpty());QVERIFY(summary->isHidden());auto* advanced=window.findChild<QToolButton*>("advancedSettings");advanced->setChecked(true);auto* timeout=window.findChild<QLineEdit*>("s3ConnectTimeoutMs");QVERIFY(timeout);timeout->setText("7.5");advanced->setChecked(false);QVERIFY(!summary->isHidden());QCOMPARE(summary->text(),QString("7.5 s connect"));timeout->setText("5");advanced->setChecked(true);advanced->setChecked(false);QVERIFY(summary->isHidden());QVERIFY(summary->text().isEmpty());
 }
 void protonCopyingExplanationLivesInInlineHelp(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();auto* provider=window.findChild<QComboBox*>("storageProvider");provider->setCurrentIndex(provider->findData("proton-drive"));auto* radio=window.findChild<QRadioButton*>("protonCreateShareLink");QVERIFY(radio);QVERIFY(!window.findChild<QLabel*>("protonCopyingNote"));auto* row=radio->parentWidget();auto* help=row->findChild<QToolButton*>();QVERIFY(help);QVERIFY(help->accessibleDescription().contains("After uploading"));QVERIFY(!help->isChecked());help->click();auto* note=row->findChild<QLabel*>("protonCreateShareLinkHelpText");QVERIFY(note&&note->isVisible());QVERIFY(note->text().contains("After uploading"));help->click();QVERIFY(note->isHidden());bool visibleConsequence=false;for(auto* label:row->findChildren<QLabel*>())if(label->isVisible()&&label->text().contains("Anyone with the link"))visibleConsequence=true;QVERIFY(visibleConsequence);
 }
 void destinationHelpersStayCompactAndKeepTheirFullValues(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();auto* hint=window.findChild<QLabel*>("s3BucketHint");auto* bucket=window.findChild<QLineEdit*>("s3Bucket");auto* folder=window.findChild<QLineEdit*>("s3ObjectPrefix");bucket->setText("synthetic-bucket");folder->setText(QString("long-folder/").repeated(10));QVERIFY(!hint->wordWrap());QCOMPARE(hint->toolTip(),hint->text());QVERIFY(hint->text().contains(folder->text()));QCOMPARE(folder->placeholderText(),QString("Bucket root"));QVERIFY(!window.findChild<QLabel*>("credentialSessionTokenHint"));QCOMPARE(window.findChild<QLineEdit*>("credentialSessionToken")->placeholderText(),QString("Paste a session token (optional)"));
 }
 void destinationRailStatesBreakOnlyBetweenFacts_data(){QTest::addColumn<int>("pixels");QTest::newRow("100")<<14;QTest::newRow("150")<<21;}
 void destinationRailStatesBreakOnlyBetweenFacts(){
  QFETCH(int,pixels);VisualStyle::apply(*qApp);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(pixels);qApp->setFont(font);QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();putgd::test::settle(30);auto* list=window.findChild<QListWidget*>("destinationList");auto* rail=window.findChild<QWidget*>("destinationRail");QVERIFY(rail->width()>=Graphite::railWidth());QVERIFY(rail->width()<=Graphite::railWidth()+2*Graphite::space(Graphite::Space::xl));const int width=list->viewport()->width()-28-Graphite::scaled(Graphite::Metric::icon)-10;
  for(int row=0;row<list->count();++row)for(const auto& line:window.settings()->destinationRowLines(row)[1].split('\n'))QVERIFY(!line.endsWith("·"));
  const QString configured="Default · Configured";const int exact=qCeil(QFontMetricsF(Graphite::font(Graphite::Type::metadata)).horizontalAdvance(configured));QCOMPARE(PreferenceRows::destinationStateLines(configured,exact),QStringList{configured});QCOMPARE(PreferenceRows::destinationStateLines(configured,exact-1),QStringList({"Default","· Configured"}));
  for(const auto& state:{QString("Default · Configured"),QString("Default · Needs credentials"),QString("Default · Account and folder checked")})for(int available:{width,80}){const auto lines=PreferenceRows::destinationStateLines(state,available);QCOMPARE(lines.join(" "),state);for(int row=0;row<lines.size();++row){const auto& line=lines[row];QVERIFY(!line.endsWith("·"));if(row>0)QVERIFY(line.startsWith("· "));QVERIFY(!line.contains('\n'));}}
 }
 void destinationRailRowsFollowTheirShownContent(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();auto* list=window.findChild<QListWidget*>("destinationList");putgd::test::settle(20);auto* item=list->item(0);QStyleOptionViewItem option;option.widget=list;option.rect=QRect(0,0,list->viewport()->width(),100);auto index=list->model()->index(0,0);item->setText("Archive\nReady");const int shortHeight=list->itemDelegate()->sizeHint(option,index).height();item->setText("A destination with a name that takes two lines\nReady");const int tallHeight=list->itemDelegate()->sizeHint(option,index).height();QVERIFY(tallHeight>shortHeight);QVERIFY(shortHeight>=Graphite::scaled(56));QVERIFY(shortHeight<Graphite::scaled(76));
 }
 void destinationSelectionMarkerHasContrast(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");auto* list=window.findChild<QListWidget*>("destinationList");QVERIFY(list);const auto original=Theme::scheme();auto restore=qScopeGuard([&]{Theme::setScheme(original);});
  for(const auto scheme:{Theme::Scheme::Dark,Theme::Scheme::Light}){Theme::setScheme(scheme);QStyleOptionViewItem option;option.widget=list;option.rect=QRect(0,0,240,76);option.state=QStyle::State_Selected|QStyle::State_Enabled;QImage image(option.rect.size(),QImage::Format_RGB32);image.fill(Theme::current().surface);{QPainter p(&image);list->itemDelegate()->paint(&p,option,list->model()->index(0,0));}QCOMPARE(image.pixelColor(3,38),Theme::current().accent);QVERIFY(Theme::contrast(image.pixelColor(3,38),Theme::current().selected)>=3.0);}
 }
 void destinationCardsKeepConnectionTestAfterCredentials(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.show();window.navigate("destinations");putgd::test::settle(30);window.settings();
  auto* credentials=window.findChild<QWidget*>("credentialsEditor");auto* where=window.findChild<QWidget*>("destinationEditor");auto* access=window.findChild<QLineEdit*>("credentialAccessKey");auto* link=window.findChild<QLineEdit*>("s3PublicUrlBase");auto* test=window.findChild<QPushButton*>("testDestination");QVERIFY(credentials&&where&&access&&link&&test);
  PUTGD_TRY_VERIFY(credentials->isVisible());QVERIFY(where->mapTo(&window,QPoint()).y()<credentials->mapTo(&window,QPoint()).y());QVERIFY(link->mapTo(&window,QPoint()).y()<access->mapTo(&window,QPoint()).y());QVERIFY(credentials->isAncestorOf(test));QVERIFY(window.findChild<QScrollArea*>("destinationScroll")->widget()->isAncestorOf(test));QVERIFY(test->isVisible());QVERIFY(test->width()<=test->sizeHint().width()+4);
  auto* advanced=window.findChild<QToolButton*>("advancedSettings");advanced->setChecked(true);PUTGD_TRY_VERIFY(window.findChild<QLineEdit*>("s3StorageClass")->isVisible());QVERIFY(advanced->mapTo(&window,QPoint()).y()<link->mapTo(&window,QPoint()).y());QVERIFY(where->isAncestorOf(advanced));QVERIFY(where->isAncestorOf(window.findChild<QLineEdit*>("s3StorageClass")));
  advanced->setChecked(false);auto* provider=window.findChild<QComboBox*>("storageProvider");provider->setCurrentIndex(provider->findData("backblaze-b2"));QVERIFY(where->isAncestorOf(advanced));provider->setCurrentIndex(provider->findData("proton-drive"));putgd::test::settle(30);QVERIFY(!where->isAncestorOf(advanced));QVERIFY(window.findChild<QWidget*>("advancedConnectionRow")->parentWidget()->isAncestorOf(window.findChild<QSpinBox*>("protonExpiration")));auto* folder=window.findChild<QComboBox*>("protonFolder");QWidget* folderLabel=nullptr;for(auto* label:window.findChildren<QLabel*>())if(label->buddy()==folder)folderLabel=label;QVERIFY(folderLabel);PUTGD_TRY_VERIFY(folderLabel->minimumWidth()<=Graphite::scaled(240));const int gutter=folderLabel->minimumWidth();advanced->setChecked(true);putgd::test::settle(30);PUTGD_TRY_VERIFY(folderLabel->minimumWidth()>=gutter);advanced->setChecked(false);PUTGD_TRY_COMPARE(folderLabel->minimumWidth(),gutter);
 }
 void outputSummaryDescribesDefaultAndResolvedExceptions(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=threeDestinations();
  auto archive=doc["archive"].toObject();archive["keepUploads"]=false;doc["archive"]=archive;doc["captureDefaults"]=QJsonObject{{"destination","local"},{"profileId",QJsonValue::Null},{"publicationMode","default"}};
  auto actions=doc["actions"].toObject();for(const auto& name:Settings::actionNames()){auto a=actions[name].toObject();a["destination"]="default";a["profileId"]=QJsonValue::Null;actions[name]=a;}doc["actions"]=actions;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.show();
  auto* summary=window.findChild<QToolButton*>("captureOutputSummary");QCOMPARE(summary->text(),QString("Captures have no output"));window.resize(1060,680);window.show();PUTGD_TRY_COMPARE(summary->text(),QString("Captures have no output"));QVERIFY(summary->toolTip().contains("All actions: Nothing is saved or uploaded"));window.resize(1320,880);PUTGD_TRY_COMPARE(summary->text(),QString("Captures have no output"));
  int visibleDetails=0;for(const auto& name:Settings::presentationOrder())visibleDetails+=window.findChild<QAction*>("captureOutputDetail"+name)->isVisible();QCOMPARE(visibleDetails,0);QVERIFY(window.findChild<QAction*>("captureOutputDefault")->text().startsWith("All actions:"));
  auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();profile["provider"]="proton-drive";profile["settings"]=ProtonProvider::defaults();profiles[0]=profile;doc["profiles"]=profiles;
  auto region=actions["region"].toObject();region["destination"]="upload";region["profileId"]=profile["id"];region["publicationMode"]="private";actions["region"]=region;doc["actions"]=actions;QVERIFY(service.save(doc,service.revision()));
  PUTGD_TRY_VERIFY(summary->text()=="Mixed outputs");auto* popup=summary->menu();QVERIFY(popup);QString text=summary->accessibleName();for(auto* entry:popup->actions()){if(!entry->isVisible())continue;if(!entry->isSeparator()&&entry->objectName()!="changeCaptureOutput"){auto* info=qobject_cast<QWidgetAction*>(entry);QVERIFY(info);QVERIFY(info->isEnabled());QVERIFY(info->defaultWidget()->focusPolicy()==Qt::NoFocus);}text+="\n"+entry->text().replace("&&","&");}QVERIFY(popup->actions()[Settings::presentationOrder().size()+1]->isSeparator());QCOMPARE(popup->actions().last()->objectName(),QString("changeCaptureOutput"));
  QVERIFY(text.contains("Default output: Nothing is saved or uploaded"));QVERIFY(!text.contains("All actions:"));QVERIFY(text.contains("Capture region: Uploads right away · Private upload · "+profile["label"].toString()));QVERIFY(text.contains("Record screen: Upload when you choose · Private upload · "+profile["label"].toString()));QVERIFY(text.contains("Upload clipboard: Nothing is saved or uploaded"));QCOMPARE(text.count("No local copy"),2);QCOMPARE(summary->accessibleDescription().count("No local copy"),2);
  window.findChild<QAction*>("changeCaptureOutput")->trigger();PUTGD_TRY_VERIFY(window.findChild<QToolButton*>("captureExceptionsToggle")->isChecked());PUTGD_TRY_VERIFY(window.findChild<QComboBox*>("profileRegion")->hasFocus());popup->close();
  doc["captureDefaults"]=QJsonObject{{"destination","upload"},{"profileId",profile["id"]},{"publicationMode","default"}};QVERIFY(service.save(doc,service.revision()));PUTGD_TRY_VERIFY(window.findChild<QAction*>("captureOutputDefault")->text().endsWith(" · No local copy"));archive["keepUploads"]=true;doc["archive"]=archive;QVERIFY(service.save(doc,service.revision()));PUTGD_TRY_VERIFY(!summary->accessibleDescription().contains("No local copy"));
 }
 void outputMenuInformationWrapsAtLargeText_data(){QTest::addColumn<int>("pixels");QTest::newRow("normal")<<14;QTest::newRow("text150")<<21;}
 void outputMenuInformationWrapsAtLargeText(){
  QFETCH(int,pixels);VisualStyle::apply(*qApp);const auto old=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(old);});auto font=old;font.setPixelSize(pixels);qApp->setFont(font);QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=threeDestinations();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();profile["label"]=QString("Long destination name for output information ").repeated(2);profiles[0]=profile;doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1060,680);window.show();auto* summary=window.findChild<QToolButton*>("captureOutputSummary");auto* menu=summary->menu();menu->popup(summary->mapToGlobal(QPoint()));putgd::test::settle(30);for(auto* action:menu->actions())if(auto* info=qobject_cast<QWidgetAction*>(action)){auto* label=qobject_cast<QLabel*>(info->defaultWidget());QVERIFY(label&&label->wordWrap());QVERIFY(label->height()>=label->heightForWidth(label->width()));QCOMPARE(label->contentsMargins(),QMargins(Graphite::space(Graphite::Space::s),Graphite::space(Graphite::Space::s),Graphite::space(Graphite::Space::s),Graphite::space(Graphite::Space::s)));QTextDocument group;group.setHtml(label->text());const auto lines=group.toPlainText().split('\n');QCOMPARE(lines.size(),2);QCOMPARE(lines[0].simplified(),action->text().section(": ",0,0).simplified());QCOMPARE(lines[1].simplified(),action->text().section(": ",1).simplified());QCOMPARE(label->focusPolicy(),Qt::NoFocus);}menu->close();
 }
 void outputRecoveryRevealsAffectedOverrides(){
  for(const int count:{1,2}){
   QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=threeDestinations();auto archive=doc["archive"].toObject();archive["keepUploads"]=false;doc["archive"]=archive;auto actions=doc["actions"].toObject();const QStringList affected=count==1?QStringList{"clipboard"}:QStringList{"clipboard","window"};
   for(const auto& name:affected){auto a=actions[name].toObject();a["destination"]="local";a["profileId"]=QJsonValue::Null;actions[name]=a;}doc["actions"]=actions;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.show();
   auto* choose=window.findChild<QPushButton*>("chooseCommandOutput");QVERIFY(choose->isVisible());choose->click();PUTGD_TRY_VERIFY(window.findChild<QToolButton*>("captureExceptionsToggle")->isChecked());
   auto* focused=qobject_cast<QComboBox*>(QApplication::focusWidget());PUTGD_TRY_VERIFY(focused=qobject_cast<QComboBox*>(QApplication::focusWidget()));QVERIFY(affected.contains(focused->objectName().mid(7).toLower()));QVERIFY(focused->isEnabled());QVERIFY(focused->isVisible());
   for(const auto& action:Settings::invocableActionNames()){const auto title=action.left(1).toUpper()+action.mid(1);QCOMPARE(window.findChild<QLabel*>("captureSummary"+title)->property("outputNeedsAttention").toBool(),affected.contains(action));}
   focused->setCurrentIndex(focused->findData("development-s3"));PUTGD_TRY_VERIFY(!window.findChild<QLabel*>("captureSummary"+focused->objectName().mid(7))->property("outputNeedsAttention").toBool());
  }
 }
 void checkboxAccessibilityKeepsActionAndCategory(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();
  for(const auto& [name,category]:QList<QPair<QString,QString>>{{"launchAtLogin","Startup"},{"keepUploadCopies","Each capture"},{"notifications","Notifications"}}){auto* check=window.findChild<QCheckBox*>(name);QCOMPARE(check->accessibleName(),check->text());auto* hint=window.findChild<QLabel*>(name+"Hint");QCOMPARE(check->accessibleDescription(),category+(hint?". "+hint->text():QString{}));}
  QWidget host;QCheckBox check("Do the action",&host);VisualStyle::settingRow("Category","Help text",&check,&host);QCOMPARE(check.accessibleName(),check.text());QCOMPARE(check.accessibleDescription(),QString("Category. Help text"));
 }
 void renameDestinationSelectsInlineTitleAndAutosaves(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* title=window.findChild<QLineEdit*>("destinationLabel");auto* rename=window.findChild<QAction*>("renameDestination");QVERIFY(rename);rename->trigger();PUTGD_TRY_COMPARE(QApplication::focusWidget(),title);QCOMPARE(title->selectedText(),title->text());QTest::keyClicks(title,"Renamed destination");QTest::keyClick(title,Qt::Key_Tab);PUTGD_TRY_COMPARE(service.document()["profiles"].toArray()[0].toObject()["label"].toString(),QString("Renamed destination"));
 }
 void storeEligibilityPreservesTypedSecrets_data(){
  QTest::addColumn<bool>("password");QTest::newRow("credentials")<<false;QTest::newRow("link-password")<<true;
 }
 void storeEligibilityPreservesTypedSecrets(){
  QFETCH(bool,password);QTemporaryDir dir;auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();
  profile["credentials"]=QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}};
  if(password){profile["provider"]="proton-drive";profile["settings"]=ProtonProvider::defaults();}profiles[0]=profile;doc["profiles"]=profiles;
  SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);
  MainWindow window(fixture.session);window.navigate("destinations");window.show();
  auto* store=window.findChild<QPushButton*>(password?"storeProtonPassword":"storeCredentials");auto* field=window.findChild<QLineEdit*>(password?"protonSharePassword":"credentialSecretKey");
  auto* reason=window.findChild<QLabel*>(password?"passwordStoreReason":"credentialStoreReason");auto* label=window.findChild<QLineEdit*>("destinationLabel");auto* access=window.findChild<QLineEdit*>("credentialAccessKey");auto* token=window.findChild<QLineEdit*>("credentialSessionToken");
  field->setText("synthetic-typed-secret");access->setText("synthetic-access");token->setText("synthetic-token");int starts=0;
  fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject&){if(method.startsWith("setup.store"))++starts;};
  fixture.client.replyDelayMs["settings.save"]=250;store->setFocus();label->setText("Changed destination");
  QVERIFY(!store->isEnabled());QVERIFY(!reason->isHidden());QVERIFY(reason->text().contains("saving"));
  QVERIFY(QMetaObject::invokeMethod(store,"clicked",Qt::DirectConnection));QCOMPARE(starts,0);QCOMPARE(field->text(),QString("synthetic-typed-secret"));
  window.settings()->flushAutosave();QVERIFY(!store->isEnabled());QVERIFY(QMetaObject::invokeMethod(store,"clicked",Qt::DirectConnection));QCOMPARE(field->text(),QString("synthetic-typed-secret"));
  PUTGD_TRY_VERIFY(store->isEnabled());QVERIFY(reason->isHidden());
  // An unfinished destination edit blocks Store before either group is read.
  label->setFocus();label->setText("Unfinished destination");QVERIFY(!store->isEnabled());QVERIFY(QMetaObject::invokeMethod(store,"clicked",Qt::DirectConnection));QCOMPARE(field->text(),QString("synthetic-typed-secret"));QCOMPARE(starts,0);
  label->setText("Changed destination");QMetaObject::invokeMethod(label,"editingFinished",Qt::DirectConnection);PUTGD_TRY_VERIFY(store->isEnabled());
  fixture.client.failOnce["settings.save"]={"synthetic_save_refused","Synthetic save refusal",{}};label->setText("Retry destination");QMetaObject::invokeMethod(label,"editingFinished",Qt::DirectConnection);window.settings()->flushAutosave();PUTGD_TRY_VERIFY(window.settings()->saveViewState()["autosaveBlocked"].toBool());QVERIFY(!store->isEnabled());QVERIFY(reason->text().contains("Retry saving"));QCOMPARE(field->text(),QString("synthetic-typed-secret"));window.findChild<QPushButton*>("retryDestinationAutosave")->click();PUTGD_TRY_VERIFY(store->isEnabled());
  auto external=service.document();auto prefs=external["preferences"].toObject();prefs["notifications"]=!prefs["notifications"].toBool();external["preferences"]=prefs;QVERIFY(service.save(external,service.revision()));
  PUTGD_TRY_VERIFY(window.settings()->saveViewState()["conflict"].toBool());QVERIFY(!store->isEnabled());QVERIFY(reason->text().contains("conflict"));
  QVERIFY(!window.findChild<QPushButton*>("reloadCredentialConflict"));QVERIFY(!window.findChild<QPushButton*>("reloadPasswordConflict"));QVERIFY(reason->text().contains("Resolve the settings conflict before saving credentials"));auto* reload=window.findChild<QPushButton*>("reloadDestinationConflict");QVERIFY(reload&&!reload->isHidden());auto* scroll=window.findChild<QScrollArea*>("destinationScroll");auto* banner=window.findChild<QWidget*>("destinationStatusRow");QVERIFY(!scroll->isAncestorOf(banner));scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());PUTGD_TRY_VERIFY(banner->isVisible());QVERIFY(window.rect().contains(QRect(banner->mapTo(&window,QPoint()),banner->size())));
  QVERIFY(QMetaObject::invokeMethod(store,"clicked",Qt::DirectConnection));QCOMPARE(starts,0);QCOMPARE(field->text(),QString("synthetic-typed-secret"));QCOMPARE(access->text(),QString("synthetic-access"));QCOMPARE(token->text(),QString("synthetic-token"));
  const auto confirmation=acceptNextConfirmation();reload->click();QVERIFY(confirmation->startsWith("Reload settings from file\n"));PUTGD_TRY_VERIFY(!window.settings()->saveViewState()["conflict"].toBool());QVERIFY(field->text().isEmpty());PUTGD_TRY_VERIFY(reload->isHidden());
 }
 void storeClearsSecretsOnlyAfterAdmission_data(){storeEligibilityPreservesTypedSecrets_data();}
 void storeClearsSecretsOnlyAfterAdmission(){
  QFETCH(bool,password);QTemporaryDir dir;auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();profile["credentials"]=QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}};
  if(password){profile["provider"]="proton-drive";profile["settings"]=ProtonProvider::defaults();}profiles[0]=profile;doc["profiles"]=profiles;
  SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);
  fixture.setup.setProviderSetup([](const QString&,const Destination&,const QJsonObject& input,JsonReply reply,auto){reply({},{{"reference",input["reference"]}});});
  MainWindow window(fixture.session);window.navigate("destinations");window.show();auto* store=window.findChild<QPushButton*>(password?"storeProtonPassword":"storeCredentials");auto* field=window.findChild<QLineEdit*>(password?"protonSharePassword":"credentialSecretKey");auto* access=window.findChild<QLineEdit*>("credentialAccessKey");
  const auto method=password?QString("setup.storeSharePassword"):QString("setup.storeCredentials");field->setText("synthetic-secret");access->setText("synthetic-access");fixture.client.failOnce[method]={"synthetic_refusal","Synthetic setup refusal",{}};
  store->click();QCOMPARE(field->text(),QString("synthetic-secret"));PUTGD_TRY_COMPARE(statusCode(window),QString("synthetic_refusal"));QVERIFY(store->isEnabled());QCOMPARE(field->text(),QString("synthetic-secret"));
  fixture.client.replyDelayMs[method]=250;store->click();QVERIFY(!store->isEnabled());QCOMPARE(field->text(),QString("synthetic-secret"));PUTGD_TRY_VERIFY(field->text().isEmpty());if(!password)QVERIFY(access->text().isEmpty());
  QVERIFY(!QJsonDocument(service.document()).toJson().contains("synthetic-secret"));QVERIFY(!QJsonDocument(window.settings()->saveViewState()).toJson().contains("synthetic-secret"));
 }
 void choosingKeychainFromSessionCreatesOnlyAReference(){
  QTemporaryDir dir;auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();profile["credentials"]=QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}};profiles[0]=profile;doc["profiles"]=profiles;
  SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);int stores=0;fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject&){if(method=="setup.storeCredentials")++stores;};
  MainWindow window(fixture.session);window.navigate("destinations");window.show();auto* mode=window.findChild<QComboBox*>("credentialMode");window.findChild<QLineEdit*>("credentialSecretKey")->setText("synthetic-kept-input");mode->setCurrentIndex(mode->findData("keychain"));
  PUTGD_TRY_COMPARE(service.document()["profiles"].toArray()[0].toObject()["credentials"].toObject()["mode"].toString(),QString("keychain"));QCOMPARE(window.findChild<QLineEdit*>("credentialReference")->text(),QString("putgd/development-s3"));QCOMPARE(stores,0);QCOMPARE(window.findChild<QLineEdit*>("credentialSecretKey")->text(),QString("synthetic-kept-input"));QVERIFY(!window.findChild<QLabel*>("credentialStorageNotice")->isHidden());
 }
 void destinationDetailsExplainLockedAndReplacementValues(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();
  QVERIFY(!window.findChild<QPushButton*>("quitApplication"));auto* summary=window.findChild<QLabel*>("connectionSummary");auto* advanced=window.findChild<QToolButton*>("advancedSettings");QVERIFY(summary->isVisible());advanced->setChecked(true);QVERIFY(!summary->isVisible());advanced->setChecked(false);QVERIFY(summary->isVisible());
  auto* notice=window.findChild<QLabel*>("credentialStorageNotice");QVERIFY(notice->isHidden());auto* mode=window.findChild<QComboBox*>("credentialMode");mode->setCurrentIndex(mode->findData("session"));QVERIFY(!notice->isHidden());QVERIFY(notice->text().contains("aren't copied"));
  window.findChild<QListWidget*>("destinationList")->setCurrentRow(1);auto* makeDefault=window.findChild<QPushButton*>("makeDefaultDestination");QCOMPARE(makeDefault->text(),QString("Make default…"));window.findChild<QPushButton*>("testDestination")->click();PUTGD_TRY_COMPARE(makeDefault->text(),QString("Make default"));fixture.client.failOnce["setup.testDestination"]={"synthetic_retest_refused","Synthetic retest refusal",{}};window.findChild<QPushButton*>("testDestination")->click();QCOMPARE(makeDefault->text(),QString("Make default…"));PUTGD_TRY_COMPARE(statusCode(window),QString("synthetic_retest_refused"));
  window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(1);window.findChild<QToolButton*>("captureExceptionsToggle")->setChecked(true);auto* destination=window.findChild<QComboBox*>("defaultCaptureDestination");destination->setCurrentIndex(destination->findData(QString{}));window.findChild<QCheckBox*>("keepUploadCopies")->setChecked(false);
  auto* inherited=window.findChild<QLabel*>("captureSummaryRegion");QCOMPARE(inherited->text(),QString("Nothing is saved or uploaded"));QCOMPARE(inherited->property("feedbackTone").toString(),QString("neutral"));QCOMPARE(window.findChild<QLabel*>("captureSummaryClipboard")->property("feedbackTone").toString(),QString("neutral"));
  window.findChild<QCheckBox*>("overrideRegion")->click();QCOMPARE(inherited->text(),QString("Nothing is saved or uploaded"));QCOMPARE(inherited->property("feedbackTone").toString(),QString("error"));
 }
 void statusBandScalesItsRegularSummaryAtNarrowAndLargeText(){
  Fonts::load();VisualStyle::apply(*qApp);
  const auto original=QApplication::font();auto restore=qScopeGuard([&]{QApplication::setFont(original);});
  for(const int pixels:{14,21}){auto font=original;font.setPixelSize(pixels);QApplication::setFont(font);QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.resize(1060,680);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
   QCOMPARE(window.findChild<QLabel*>("appVersion")->font().pixelSize(),Graphite::scaled(13));QCOMPARE(window.findChild<QPushButton*>("appVersionLink")->font().pixelSize(),Graphite::scaled(13));auto* summary=window.findChild<QToolButton*>("captureOutputSummary");QCOMPARE(summary->font().pixelSize(),Graphite::scaled(13));QCOMPARE(summary->font().weight(),QFont::Normal);if(pixels>14)QVERIFY(summary->font().pixelSize()>13);auto* band=window.findChild<QStatusBar*>("statusBand");QVERIFY(band);QVERIFY(summary->height()>=summary->fontMetrics().height());QVERIFY(band->rect().contains(QRect(summary->mapTo(band,QPoint()),summary->size())));QCOMPARE(window.width(),1060);
   QVERIFY(window.findChild<QAction*>("changeCaptureOutput"));QVERIFY(summary->height()>=32);QVERIFY(band->height()>=summary->height());
   window.settings()->showCaptureSettings();window.findChild<QToolButton*>("captureExceptionsToggle")->setChecked(true);
   const auto scheme=Theme::scheme();auto restoreScheme=qScopeGuard([&]{Theme::setScheme(scheme);});Theme::setScheme(Theme::Scheme::Light);
   for(const auto& action:Settings::invocableActionNames()){
    auto name=action.left(1).toUpper()+action.mid(1);auto* override=window.findChild<QCheckBox*>("override"+name);if(!override->isChecked())override->click();
    for(const auto& prefix:QStringList{"profile","publication"}){auto* control=window.findChild<QComboBox*>(prefix+name);control->ensurePolished();QCOMPARE(control->font().pixelSize(),Graphite::font(Graphite::Type::body).pixelSize());PUTGD_TRY_COMPARE(control->height(),window.findChild<QComboBox*>("defaultCaptureDestination")->height());QVERIFY2(putgd::test::waitFor([&]{return control->parentWidget()->rect().contains(control->geometry());}),qPrintable(QString("%1: control %2,%3 %4x%5, parent %6x%7, visible %8").arg(control->objectName()).arg(control->x()).arg(control->y()).arg(control->width()).arg(control->height()).arg(control->parentWidget()->width()).arg(control->parentWidget()->height()).arg(control->isVisible())));Graphite::setLocked(control,true);Graphite::setLocked(control,false);QCOMPARE(control->font().pixelSize(),Graphite::font(Graphite::Type::body).pixelSize());QCOMPARE(control->sizeHint().height(),window.findChild<QComboBox*>("defaultCaptureDestination")->sizeHint().height());PUTGD_TRY_COMPARE(control->height(),window.findChild<QComboBox*>("defaultCaptureDestination")->height());}
   }
   Theme::setScheme(scheme);
   auto* check=window.findChild<QCheckBox*>("notifications");QVERIFY(check->sizeHint().height()>=32);QCOMPARE(check->style()->pixelMetric(QStyle::PM_IndicatorWidth),Graphite::scaled(17));
  }
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
 void destinationHeaderKeepsActionsReachable_data(){
  QTest::addColumn<int>("width");QTest::addColumn<int>("profileRow");QTest::addColumn<QString>("name");
  for(const int width:{1060,1440})for(const int row:{0,1}){
   const auto prefix=QByteArray::number(width)+(row?"-other-":"-default-");
   QTest::newRow((prefix+"short").constData())<<width<<row<<QString("Proton Drive");
   QTest::newRow((prefix+"long").constData())<<width<<row<<QString("Production screenshots for customer support — ").repeated(3).left(120);
   QTest::newRow((prefix+"unbroken").constData())<<width<<row<<QString(120,'W');
  }
 }
 void destinationHeaderKeepsActionsReachable(){
  QFETCH(int,width);QFETCH(int,profileRow);QFETCH(QString,name);
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=threeDestinations();auto profiles=doc["profiles"].toArray();auto profile=profiles[profileRow].toObject();profile["label"]=name;profiles[profileRow]=profile;doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(width,680);window.navigate("destinations");
  auto* list=window.findChild<QListWidget*>("destinationList");QVERIFY(list);list->setCurrentRow(profileRow);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  auto* label=window.findChild<QLineEdit*>("destinationLabel");auto* scroll=window.findChild<QScrollArea*>("destinationScroll");auto* test=window.findChild<QPushButton*>("testDestination");auto* makeDefault=window.findChild<QPushButton*>("makeDefaultDestination");QPushButton* more=nullptr;
  for(auto* button:window.findChildren<QPushButton*>())if(button->accessibleName()=="Destination actions")more=button;
  QVERIFY(!window.findChild<QToolButton*>("destinationLabelHelp"));QVERIFY(label&&scroll&&test&&makeDefault&&more);PUTGD_TRY_COMPARE(label->text(),name);
  QCOMPARE(window.width(),width);auto* viewport=scroll->viewport();
  QVERIFY(scroll->widget()->isAncestorOf(test));
  for(auto* control:QList<QWidget*>{label,makeDefault,more}){
   if(control==makeDefault&&profileRow==0){QVERIFY(control->isHidden());continue;}QVERIFY(control->isVisible());QWidget* available=viewport;const QRect rect(control->mapTo(available,QPoint{}),control->size());
   QVERIFY2(available->rect().contains(rect),qPrintable(QString("%1 exceeds the %2px destination viewport: x=%3 width=%4").arg(control->accessibleName().isEmpty()?control->objectName():control->accessibleName()).arg(viewport->width()).arg(rect.x()).arg(rect.width())));
  }
  QCOMPARE(scroll->horizontalScrollBar()->maximum(),0);
  if(viewport->width()>=Graphite::scaled(400)){auto* state=window.findChild<QLabel*>("destinationProviderState");const int center=(label->mapTo(viewport,QPoint()).y()+state->mapTo(viewport,QPoint(0,state->height())).y())/2;QVERIFY(qAbs(center-more->mapTo(viewport,more->rect().center()).y())<=Graphite::scaled(8));}
  QVERIFY(label->width()>=label->fontMetrics().horizontalAdvance("Pro"));
  // A popup must keep the selected name available to Copy; normal editing and
  // autosave still operate on the complete value, regardless of visible width.
  label->setFocus();label->selectAll();auto* editMenu=label->createStandardContextMenu();editMenu->popup(label->mapToGlobal(QPoint(0,label->height())));PUTGD_TRY_VERIFY(editMenu->isVisible());QCOMPARE(label->selectedText(),name);editMenu->close();delete editMenu;
  label->setFocus();QTest::keyClick(label,Qt::Key_End);QCOMPARE(label->cursorPosition(),name.size());QTest::keyClicks(label," edited");QCOMPARE(label->text(),name+" edited");
  scroll->ensureWidgetVisible(test,0,16);test->setFocus();QCOMPARE(label->cursorPosition(),0);QCOMPARE(label->text(),name+" edited");
  PUTGD_TRY_COMPARE(service.document()["profiles"].toArray()[profileRow].toObject()["label"].toString(),name+" edited");
  // The trailing action remains clickable, including for a non-default destination.
  auto* menu=more->menu();QVERIFY(menu);bool shown=false;QTimer closer;closer.setInterval(10);QObject::connect(&closer,&QTimer::timeout,menu,[&]{if(menu->isVisible()){shown=true;menu->close();}});closer.start();
  // QPushButton runs its menu modally on some Qt versions (6.4): close it from a timer.
  scroll->ensureWidgetVisible(more,0,16);QTest::mouseClick(more,Qt::LeftButton);PUTGD_TRY_VERIFY(shown);closer.stop();
 }
 void longDestinationLabelKeepsItsStateLine_data(){QTest::addColumn<int>("pixels");QTest::newRow("normal")<<14;QTest::newRow("large")<<21;}
 void longDestinationLabelKeepsItsStateLine(){
  QFETCH(int,pixels);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(pixels);qApp->setFont(font);
  // Long names get two bounded lines; the complete state stays below them.
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=threeDestinations();auto profiles=doc["profiles"].toArray();
  auto first=profiles[0].toObject();first["label"]="put.gd development destination with a synthetic name long enough to wrap twice";profiles[0]=first;doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1100,760);window.navigate("destinations");window.show();
  auto* list=window.findChild<QListWidget*>("destinationList");QVERIFY(list);QVERIFY(!list->wordWrap());
  const auto lines=window.settings()->destinationRowLines(0);QCOMPARE(lines.size(),2);
  const auto nameLines=lines[0].split('\n');QCOMPARE(nameLines.size(),2);QVERIFY(nameLines.last().endsWith(QChar(0x2026)));QVERIFY(first["label"].toString().startsWith(nameLines.first()));
  QVERIFY2(lines[1].startsWith("Default"),qPrintable(lines[1]));QVERIFY(lines[1].contains("Ready to test"));QVERIFY(!lines[1].contains(QChar(0x2026)));
  // Both name lines and the full status fit inside the row.
  const QFontMetrics metrics(list->font());const auto rect=list->visualItemRect(list->item(0));
  for(const auto& nameLine:nameLines)QVERIFY(metrics.horizontalAdvance(nameLine)<rect.width()-Graphite::scaled(28)-26);const QFontMetrics body(Graphite::font(Graphite::Type::body,true)),meta(Graphite::font(Graphite::Type::metadata));const int statusHeight=lines[1].split('\n').size()*meta.height();QVERIFY(rect.height()>=2*body.height()+statusHeight+Graphite::space(Graphite::Space::xs)+2*Graphite::space(Graphite::Space::m));
  const auto alternate=window.settings()->destinationRowLines(1);QCOMPARE(alternate[1],QString("Ready to test"));auto alternateName=alternate[0];alternateName.replace('\n',' ');QVERIFY(QString("Synthetic alternate").startsWith(alternateName.endsWith(QChar(0x2026))?alternateName.chopped(1):alternateName));
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
  MainWindow window(fixture.session);window.navigate("settings");window.show();fixture.session.refreshSnapshot();auto* settings=window.settings();
  bool opened=false;settings->diagnosticsCommand({{"action","openPackage"}},[&](Error,QJsonObject){opened=true;});PUTGD_TRY_VERIFY(opened);
  auto* dialog=settings->findChild<QDialog*>("diagnosticsPackageDialog");QVERIFY(dialog&&dialog->isVisible());QVERIFY(putgd::test::waitForWindowExposed(dialog));
  PUTGD_TRY_VERIFY(!settings->diagnosticsState()["package"].toObject()["uploadTarget"].toObject()["checking"].toBool());putgd::test::settle(100);
  const auto screen=dialog->screen()->availableGeometry();QCOMPARE(dialog->minimumWidth(),qMin(Graphite::scaled(560),screen.width()));QVERIFY(dialog->width()<=screen.width());QVERIFY2(dialog->height()<=screen.height(),qPrintable(QString("dialog %1 px on a %2 px screen").arg(dialog->height()).arg(screen.height())));
  auto* scroll=dialog->findChild<QScrollArea*>("diagnosticsPackageScroll");QVERIFY(scroll);QVERIFY(scroll->widget()->isAncestorOf(dialog->findChild<QLabel*>("packageContents")));
  auto* contents=dialog->findChild<QLabel*>("packageContents");QVERIFY(contents->isHidden());auto* inventory=dialog->findChild<QToolButton*>("packageContentsToggle");QVERIFY(inventory);inventory->click();QVERIFY(!contents->isHidden());putgd::test::settle(50);QVERIFY(!contents->text().isEmpty());QVERIFY(contents->height()>=contents->heightForWidth(contents->width()));
  // Large text needs more than the screen holds: the body scrolls, the buttons stay outside it.
  QVERIFY2(scroll->verticalScrollBar()->maximum()>0,qPrintable(QString("body %1 in %2").arg(scroll->widget()->height()).arg(scroll->viewport()->height())));
  for(const char* name:{"closePackage","savePackage"}){auto* button=dialog->findChild<QPushButton*>(name);QVERIFY(button->isVisible());QVERIFY(!scroll->isAncestorOf(button));
   const QRect shown(button->mapTo(dialog,QPoint()),button->size());QVERIFY2(dialog->rect().contains(shown),qPrintable(QString("%1 outside the dialog").arg(name)));}
  auto* cancel=dialog->findChild<QPushButton*>("closePackage");auto* save=dialog->findChild<QPushButton*>("savePackage");QVERIFY(save->x()-cancel->geometry().right()-1>=8);
  auto* footer=dialog->layout()->itemAt(dialog->layout()->count()-1)->layout();QVERIFY(footer);QVERIFY(footer->geometry().height()>=Graphite::scaled(Graphite::Metric::statusBand));
  QCOMPARE(dialog->findChild<QLabel*>("packageFolder")->font().family(),Graphite::monoFamily());
  dialog->reject();
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
  QVERIFY(window.findChild<QWidget*>("editImageChooserFailure")->isHidden());
 }
 // A chooser that fails is not a cancel: its reason shows beside the action that
 // opened it, what was there stays, and Try again opens the chooser again. A
 // cancel stays silent.
 void chooserFailuresShowBesideTheirActionWithTryAgain(){
  qputenv("PUTGD_FILE_DIALOG","fallback");auto restore=qScopeGuard([]{qunsetenv("PUTGD_FILE_DIALOG");});
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=Settings::developmentTemplate();auto archive=doc["archive"].toObject();archive["root"]=dir.filePath("archive");doc["archive"]=archive;QVERIFY(service.save(doc,{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(1);window.show();fixture.session.refreshSnapshot();
  const FileDialog::Result failed{FileDialog::Result::Status::Failed,{},"The chosen location is not a local file."},cancelled{};
  // Local copies → Images.
  auto* images=window.findChild<QLineEdit*>("folderImages");images->setText(dir.filePath("kept"));auto* failure=window.findChild<QWidget*>("folderImagesChooserFailure");QVERIFY(failure&&failure->isHidden());
  window.settings()->folderChooserFinished("outputFolder","images",failed);QVERIFY(failure->isVisibleTo(window.settings()));
  auto* message=window.findChild<QLabel*>("folderImagesChooserError");QVERIFY2(message->text().contains("not a local file")&&message->text().contains("unchanged"),qPrintable(message->text()));QCOMPARE(images->text(),dir.filePath("kept"));
  QVERIFY(window.findChild<QWidget*>("archiveRootChooserFailure")->isHidden());
  window.findChild<QPushButton*>("folderImagesChooserRetry")->click();QVERIFY(window.settings()->findChild<QFileDialog*>("putgdFileDialog"));
  window.settings()->fileDialogCommand("outputFolder","cancel",{},[](Error,QJsonObject){});PUTGD_TRY_VERIFY(failure->isHidden());QCOMPARE(images->text(),dir.filePath("kept"));
  window.settings()->folderChooserFinished("outputFolder","images",cancelled);QVERIFY(failure->isHidden());
  // Edit an image file…: under the command band, nothing opened.
  QSignalSpy edits(&window,&MainWindow::editRequested);auto* banner=window.findChild<QWidget*>("editImageChooserFailure");QVERIFY(banner->isHidden());
  window.imageChooserFinished(cancelled);QVERIFY(banner->isHidden());
  window.imageChooserFinished(failed);QVERIFY(banner->isVisible());QVERIFY(window.findChild<QLabel*>("editImageChooserError")->text().contains("not a local file"));QCOMPARE(edits.count(),0);
  auto* band=window.findChild<QWidget*>("commandBand");PUTGD_TRY_COMPARE(banner->geometry().top(),band->geometry().bottom()+1);
  window.findChild<QPushButton*>("retryEditImageChooser")->click();QVERIFY(window.editImageDialog().isOpen());window.editImageDialog().cancel();PUTGD_TRY_VERIFY(banner->isHidden());QCOMPARE(edits.count(),0);
  window.imageChooserFinished(failed);window.findChild<QToolButton*>("dismissEditImageChooserFailure")->click();QVERIFY(banner->isHidden());
  // The package dialog's Choose folder…
  DiagnosticsBundles bundles(fixture.operations(),dir.filePath("state"));fixture.server.setDiagnosticsBundles(&bundles);
  bool opened=false;window.settings()->diagnosticsCommand({{"action","openPackage"}},[&](Error,QJsonObject){opened=true;});PUTGD_TRY_VERIFY(opened);auto* dialog=window.settings()->findChild<QDialog*>("diagnosticsPackageDialog");
  auto* packageFailure=dialog->findChild<QWidget*>("packageFolderChooserFailure");QVERIFY(packageFailure->isHidden());const auto folder=dialog->findChild<QLabel*>("packageFolder")->text();
  window.settings()->folderChooserFinished("diagnosticsFolder",{},failed);QVERIFY(packageFailure->isVisible());QCOMPARE(dialog->findChild<QLabel*>("packageFolder")->text(),folder);QVERIFY(dialog->findChild<QLabel*>("packageFolderChooserError")->text().contains("not a local file"));
  window.settings()->folderChooserFinished("diagnosticsFolder",{},cancelled);QVERIFY(packageFailure->isHidden());
  dialog->reject();
 }
 void autoLimitsShowWhatTheyResolveTo(){
  constexpr qint64 gib=1LL<<30;ArtifactStore::setDiskForTesting(60*gib,100*gib);auto resetDisk=qScopeGuard([]{ArtifactStore::setDiskForTesting(-1,-1);});
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();
  auto* file=window.findChild<QLabel*>("maxFileBytesResolved");auto* batch=window.findChild<QLabel*>("maxBatchBytesResolved");auto* waiting=window.findChild<QLabel*>("pendingArtifactBytesResolved");
  QVERIFY(file&&batch&&waiting);
  PUTGD_TRY_COMPARE(file->text(),QString("No limit on this computer · keeps 4 GiB free"));QCOMPARE(batch->text(),file->text());QCOMPARE(waiting->text(),file->text());auto* reserve=window.findChild<QLabel*>("uploadReserveHint");QVERIFY(reserve);QCOMPARE(reserve->text(),file->text());for(auto* hint:{file,batch,waiting})QVERIFY(hint->isHidden());
  QVERIFY(file->toolTip().contains("keeps 4 GiB free"));
  // A typed size or No limit is its own answer.
  auto* edit=window.findChild<QLineEdit*>("maxFileBytes");edit->setText("2");QVERIFY(file->text().isEmpty());QVERIFY(!file->isVisibleTo(window.settings()));QVERIFY(reserve->isHidden());QVERIFY(!batch->isHidden());
  edit->setText("auto");QCOMPARE(file->text(),QString("No limit on this computer · keeps 4 GiB free"));
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
  PUTGD_TRY_VERIFY2(statusCode(window)=="config_conflict",qPrintable(status->text()));QVERIFY(status->text().contains("previous destination order is restored"));QVERIFY(!status->text().contains("config_conflict"));
  QCOMPARE(rowLabels(list),QStringList({"put.gd development destination","Synthetic alternate","Synthetic third"}));QCOMPARE(list->currentRow(),0);
  QCOMPARE(window.findChild<QLineEdit*>("destinationId")->text(),QString("development-s3"));
  QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),external);
  QVERIFY(window.settings()->autosaveText().contains("changed elsewhere"));
  // Further moves wait for a reload instead of saving over the other writer.
  QJsonObject result;QCOMPARE(window.settings()->moveSelectedDestination(1,{},result).code,QString("config_conflict"));
  acceptNextConfirmation();window.findChild<QPushButton*>("reloadSettings")->click();PUTGD_TRY_VERIFY(!window.settings()->autosaveText().contains("changed elsewhere"));
  QVERIFY(!window.settings()->moveSelectedDestination(1,"development-s3",result));
  PUTGD_TRY_COMPARE(profileIds(service.document()),QStringList({"synthetic-alternate","development-s3","synthetic-third"}));QVERIFY(!service.snapshot().notifications);
 }
 // A conflict offers its recovery where it is reported, on Settings and on
 // Destinations, and the confirmation names the edits a reload discards, not
 // what the other writer changed.
 void conflictOffersReloadBesideTheMessage(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();window.navigate("destinations");window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  auto* settingsReload=window.findChild<QPushButton*>("reloadConflict");auto* destinationReload=window.findChild<QPushButton*>("reloadDestinationConflict");QVERIFY(settingsReload&&destinationReload);QVERIFY(settingsReload->isHidden()&&destinationReload->isHidden());
  auto* number=window.findChild<QLineEdit*>("maxFiles");const auto saved=number->text();number->setText("invalid");PUTGD_TRY_VERIFY(number->property("invalid").toBool());
  auto external=service.document();auto prefs=external["preferences"].toObject();QVERIFY(prefs["notifications"].toBool());prefs["notifications"]=false;external["preferences"]=prefs;QVERIFY(service.save(external,service.revision()));
  PUTGD_TRY_VERIFY(window.settings()->saveViewState()["conflict"].toBool());
  auto* status=window.findChild<QLabel*>("settingsStatus");auto* destinationStatus=window.findChild<QLabel*>("destinationStatus");
  QCOMPARE(statusCode(window),QString("config_conflict"));QVERIFY(!status->text().contains("config_conflict"));
  QCOMPARE(status->text(),QString("Settings changed elsewhere. Your edits are kept here. Reload discards them."));
  window.resize(800,700);PUTGD_TRY_VERIFY(!QRect(destinationReload->mapTo(&window,QPoint()),destinationReload->size()).intersects(QRect(destinationStatus->mapTo(&window,QPoint()),destinationStatus->size())));
  // The band carries one bounded indicator; the explanation and Reload stay in the page.
  auto* bandStatus=window.findChild<QLabel*>("autosaveStatus");QCOMPARE(bandStatus->text(),QString("Changes not saved"));QVERIFY(bandStatus->isVisible());QVERIFY(window.findChild<QLabel*>("autosaveExplanation")->isHidden());
  auto* summary=window.findChild<QToolButton*>("captureOutputSummary");QVERIFY(summary->isEnabled());QVERIFY(summary->menu());
  QVERIFY(destinationStatus->isVisible());QVERIFY(destinationReload->isVisible());QCOMPARE(destinationReload->parentWidget()->parentWidget(),destinationStatus->parentWidget()->parentWidget());
  QVERIFY(!settingsReload->isHidden());QCOMPARE(settingsReload->parentWidget()->parentWidget(),status->parentWidget()->parentWidget());QCOMPARE(settingsReload->text(),QString("Reload from file…"));
  // An error is body text, not small print.
  QCOMPARE(status->font().pixelSize(),Graphite::font(Graphite::Type::body).pixelSize());
  // Cancel keeps the draft.
  QString shown;QTimer::singleShot(0,[&shown]{for(auto* widget:QApplication::topLevelWidgets())if(auto* box=qobject_cast<QMessageBox*>(widget);box&&box->objectName()=="confirmAction"&&box->isVisible()){shown=box->property("confirmationText").toString();box->button(QMessageBox::Cancel)->click();return;}});
  destinationReload->click();
  QVERIFY2(shown.contains("Advanced → Files in a batch"),qPrintable(shown));QVERIFY2(!shown.contains("Notifications")&&!shown.contains("notifications"),qPrintable(shown));
  QCOMPARE(number->text(),QString("invalid"));QVERIFY(window.settings()->saveViewState()["conflict"].toBool());QVERIFY(destinationReload->isVisible());
  // Reload from file replaces the draft with the file and clears the conflict.
  acceptNextConfirmation();destinationReload->click();
  PUTGD_TRY_COMPARE(number->text(),saved);QVERIFY(!window.settings()->saveViewState()["conflict"].toBool());QVERIFY(destinationReload->isHidden());QVERIFY(settingsReload->isHidden());
  QVERIFY(!window.findChild<QCheckBox*>("notifications")->isChecked());
  // Passive save information stays meta type.
  PUTGD_TRY_COMPARE(status->text(),QString("Settings reloaded from file."));QCOMPARE(status->font().pixelSize(),Graphite::font(Graphite::Type::technical).pixelSize());QVERIFY(statusCode(window).isEmpty());
 }
 // Inherited values are locked values: disabled, but painted as information
 // (textMuted in a read-only frame), unlike an unavailable action.
 void inheritedValuesAreLockedNotUnavailable(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(1);window.resize(1060,680);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  auto restore=qScopeGuard([]{Theme::setScheme(Theme::Scheme::Dark);});
  window.findChild<QToolButton*>("captureExceptionsToggle")->setChecked(true);
  auto* region=window.findChild<QComboBox*>("profileRegion");auto* sharing=window.findChild<QComboBox*>("publicationRegion");auto* clipboard=window.findChild<QComboBox*>("profileClipboard");
  QVERIFY(!region->isEnabled()&&Graphite::isLocked(region));QVERIFY(!sharing->isEnabled()&&Graphite::isLocked(sharing));QVERIFY(clipboard->isEnabled()&&!Graphite::isLocked(clipboard));
  // An inherited action is one compact row: its output in words, no fields.
  auto* summary=window.findChild<QLabel*>("captureSummaryRegion");QVERIFY(!summary->isVisible());QVERIFY(!region->isVisible());QVERIFY(clipboard->isVisible());
  window.findChild<QAction*>("addRegionException")->trigger();QVERIFY(summary->isVisible());auto* scroll=qobject_cast<QScrollArea*>(window.findChild<QTabWidget*>("settingsTabs")->currentWidget());QVERIFY(scroll);
  for(const auto scheme:{Theme::Scheme::Dark,Theme::Scheme::Light}){
   Theme::setScheme(scheme);putgd::test::settle(20);scroll->ensureWidgetVisible(summary,0,Graphite::space(Graphite::Space::l));PUTGD_TRY_VERIFY(scroll->viewport()->rect().contains(QRect(summary->mapTo(scroll->viewport(),QPoint()),summary->size())));const auto& p=Theme::palette(scheme);
   const auto shot=window.grab(QRect(summary->mapTo(&window,QPoint()),summary->size())).toImage().convertToFormat(QImage::Format_RGB32);
   // The inherited output reads at 4.5:1 against the page.
   double best=0;for(int y=0;y<shot.height();++y)for(int x=0;x<shot.width();++x)best=std::max(best,Theme::contrast(shot.pixelColor(x,y),p.bg));
   QVERIFY2(best>=4.5,qPrintable(Theme::name(scheme)+QString(" inherited summary contrast %1").arg(best)));
  }
  // Override makes the two values editable, starting from the inherited ones.
  PUTGD_TRY_VERIFY(region->isEnabled());QVERIFY(!Graphite::isLocked(region));QVERIFY(!window.findChild<QCheckBox*>("useDefaultRegion")->isChecked());PUTGD_TRY_VERIFY(region->isVisible());
  // The sidebar states Default; the redundant header action stays hidden.
  window.navigate("destinations");auto* list=window.findChild<QListWidget*>("destinationList");auto* makeDefault=window.findChild<QPushButton*>("makeDefaultDestination");
  list->setCurrentRow(0);PUTGD_TRY_VERIFY(makeDefault->isHidden());QVERIFY(list->item(0)->text().contains("Default"));
  list->setCurrentRow(1);PUTGD_TRY_COMPARE(makeDefault->text(),QString("Make default…"));QVERIFY(makeDefault->isVisible()&&!Graphite::isLocked(makeDefault));
 }
 // Sidebars share one measured width; long destination names stay inside it.
 void sidebarsFollowTheirLabelsAtLargeText(){
  auto font=QApplication::font();const auto original=font;auto restore=qScopeGuard([&]{QApplication::setFont(original);});
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);
  int baseRow=0;{MainWindow window(fixture.session);window.navigate("settings");window.resize(1060,680);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
   QCOMPARE(window.findChild<QWidget*>("settingsRail")->width(),Graphite::railWidth());auto* nav=window.findChild<QListWidget*>("settingsNavigation");baseRow=nav->visualItemRect(nav->item(0)).height();}
  font.setPixelSize(21);QApplication::setFont(font);
  MainWindow window(fixture.session);window.navigate("settings");window.resize(1060,680);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  auto* nav=window.findChild<QListWidget*>("settingsNavigation");QVERIFY(nav->visualItemRect(nav->item(0)).height()>baseRow);QVERIFY(Graphite::railWidth()<280);
  for(const char* name:{"settingsRail","destinationRail"}){auto* rail=window.findChild<QWidget*>(name);if(rail->objectName()=="destinationRail")QVERIFY(rail->width()>=Graphite::railWidth()&&rail->width()<=Graphite::railWidth()+2*Graphite::space(Graphite::Space::xl));else QCOMPARE(rail->width(),Graphite::railWidth());}
  QVERIFY(window.findChild<QWidget*>("destinationRail")->width()>=window.findChild<QWidget*>("settingsRail")->width());
  // Category labels fit inside the fixed rail.
  const QFontMetrics strong(Graphite::font(Graphite::Type::body,true));for(int i=0;i<nav->count();++i)QVERIFY(strong.horizontalAdvance(nav->item(i)->text())+74<=window.findChild<QWidget*>("settingsRail")->width());
  // The Add destination… button keeps its whole label.
  window.navigate("destinations");auto* add=window.findChild<QPushButton*>("addS3Destination");PUTGD_TRY_VERIFY(add->isVisible());QVERIFY2(add->width()>=add->sizeHint().width(),qPrintable(QString("%1 < %2").arg(add->width()).arg(add->sizeHint().width())));
 }
 // Shortcuts is named by the rail and its page heading only.
 void shortcutsPageHasNoNestedHeading(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(2);window.show();
  auto* page=window.findChild<QTabWidget*>("settingsTabs")->currentWidget();QStringList headings,sections;for(auto* label:page->findChildren<QLabel*>()){if(!label->isVisibleTo(page))continue;const auto role=label->property("visualRole").toString();if(role=="title")headings.append(label->text());if(role=="sectionTitle"||role=="section")sections.append(label->text());}
  QCOMPARE(headings,QStringList({"Shortcuts"}));QCOMPARE(sections,QStringList());QVERIFY(window.findChild<QFrame*>("shortcutsCard")->isAncestorOf(window.findChild<QLineEdit*>("shortcutRegion")));
 }
 void recordingOutputPreservesInheritanceAndExplicitRoutes_data(){
  QTest::addColumn<QString>("route");QTest::newRow("inherits-region")<<QString("inherit");QTest::newRow("explicit-upload")<<QString("upload");QTest::newRow("explicit-default")<<QString("default");
 }
 void recordingOutputPreservesInheritanceAndExplicitRoutes(){
  QFETCH(QString,route);QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=threeDestinations();auto actions=doc["actions"].toObject();auto region=actions["region"].toObject();region["destination"]="upload";region["profileId"]="synthetic-alternate";region["publicationMode"]="private";actions["region"]=region;
  auto record=actions["record"].toObject();record["shortcut"]="Ctrl+Shift+9";if(route!="inherit"){record["destination"]=route;record["profileId"]=route=="upload"?QJsonValue("synthetic-third"):QJsonValue(QJsonValue::Null);record["publicationMode"]=route=="upload"?"share":"default";}actions["record"]=record;doc["actions"]=actions;QVERIFY(service.save(doc,{}));const auto original=service.document();QFile stored(service.path());QVERIFY(stored.open(QIODevice::ReadOnly));const auto bytes=stored.readAll();stored.close();
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");auto* tabs=window.findChild<QTabWidget*>("settingsTabs");tabs->setCurrentIndex(1);window.show();window.findChild<QToolButton*>("captureExceptionsToggle")->setChecked(true);
  auto* inherit=window.findChild<QCheckBox*>("useDefaultRecord");auto* profile=window.findChild<QComboBox*>("profileRecord");auto* sharing=window.findChild<QComboBox*>("publicationRecord");auto* summary=window.findChild<QLabel*>("captureSummaryRecord");QVERIFY(inherit&&profile&&sharing&&summary);
  QCOMPARE(inherit->isChecked(),route=="inherit");QCOMPARE(profile->currentData().toString(),route=="inherit"?QString("synthetic-alternate"):route=="upload"?QString("synthetic-third"):QString("development-s3"));QCOMPARE(sharing->currentData().toString(),route=="inherit"?QString("private"):route=="upload"?QString("share"):QString("default"));if(route=="inherit"){QVERIFY(window.findChild<QLabel*>("recordingOutputInheritance")->isVisible());QVERIFY(summary->text().contains("Private upload"));QVERIFY(summary->text().contains("Synthetic alternate"));}
  QVERIFY(!validateUiState({{"schemaVersion",1},{"settings",window.settings()->saveViewState()}}));PUTGD_COMPARE_STEADY(service.document(),original,500);QVERIFY(stored.open(QIODevice::ReadOnly));QCOMPARE(stored.readAll(),bytes);stored.close();
  // Another capture edit must preserve the record entry, including explicit default inheritance.
  auto* base=window.findChild<QComboBox*>("defaultCaptureDestination");base->setCurrentIndex(base->findData("synthetic-alternate"));PUTGD_TRY_COMPARE(service.document()["captureDefaults"].toObject()["profileId"].toString(),QString("synthetic-alternate"));QCOMPARE(service.document()["actions"].toObject()["record"].toObject(),record);
  if(inherit->isChecked())window.findChild<QCheckBox*>("overrideRecord")->click();QVERIFY(profile->isEnabled());profile->setCurrentIndex(profile->findData("synthetic-third"));sharing->setCurrentIndex(sharing->findData("private"));PUTGD_TRY_COMPARE(service.snapshot().actions["record"].profileId,QString("synthetic-third"));PUTGD_TRY_COMPARE(service.snapshot().actions["record"].publicationMode,QString("private"));QCOMPARE(service.snapshot().actions["record"].shortcut,QString("Ctrl+Shift+9"));
  inherit->setChecked(true);PUTGD_TRY_COMPARE(service.document()["actions"].toObject()["record"].toObject().keys(),QStringList{"shortcut"});QCOMPARE(service.snapshot().actions["record"].profileId,service.snapshot().actions["region"].profileId);QCOMPARE(service.snapshot().actions["record"].publicationMode,service.snapshot().actions["region"].publicationMode);
 }
 void linkCopyingLivesWithDefaultCaptureOutput(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();auto* tabs=window.findChild<QTabWidget*>("settingsTabs");tabs->setCurrentIndex(1);
  auto* target=window.findChild<QComboBox*>("uploadClipboardBehavior");auto* sharing=window.findChild<QComboBox*>("defaultCaptureSharing");QVERIFY(target&&sharing);QVERIFY(tabs->widget(1)->isAncestorOf(target));QVERIFY(!tabs->widget(0)->isAncestorOf(target));QVERIFY(!window.findChild<QPushButton*>("captureClipboardSettings"));
  QFormLayout* form=nullptr;for(auto* candidate:tabs->widget(1)->findChildren<QFormLayout*>())if(candidate->parentWidget()->isAncestorOf(sharing)&&candidate->parentWidget()->isAncestorOf(target))form=candidate;QVERIFY(form);int sharingRow=-1,copyRow=-1;QFormLayout::ItemRole role;auto fieldRow=[form](QWidget* control){while(control&&control->parentWidget()!=form->parentWidget())control=control->parentWidget();return control;};form->getWidgetPosition(fieldRow(sharing),&sharingRow,&role);form->getWidgetPosition(fieldRow(target),&copyRow,&role);QCOMPARE(copyRow,sharingRow+1);
  target->setCurrentIndex(target->findData("list"));PUTGD_TRY_COMPARE(service.document()["preferences"].toObject()["uploadClipboardBehavior"].toString(),QString("list"));
 }
 void captureDefaultAndExplicitExceptionPersistAcrossReopen(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(1);window.show();
  QVERIFY(!window.findChild<QComboBox*>("defaultCaptureOutput"));QVERIFY(!window.findChild<QComboBox*>("destinationClipboard"));auto* destination=window.findChild<QComboBox*>("defaultCaptureDestination");QVERIFY(destination);const auto profileId=destination->currentData().toString();QVERIFY(!profileId.isEmpty());auto* local=window.findChild<QCheckBox*>("keepUploadCopies");QVERIFY(local);local->setChecked(true);PUTGD_TRY_VERIFY(service.snapshot().archive.keepUploads);QCOMPARE(service.snapshot().actions["region"].profileId,profileId);
  auto* exceptions=window.findChild<QToolButton*>("captureExceptionsToggle");QVERIFY(exceptions);QVERIFY(!exceptions->isChecked());exceptions->click();auto* inherit=window.findChild<QCheckBox*>("useDefaultClipboard");QVERIFY(inherit);inherit->setChecked(false);auto* clipboard=window.findChild<QComboBox*>("profileClipboard");clipboard->setCurrentIndex(clipboard->findData(QString{}));
  PUTGD_TRY_VERIFY(service.document().contains("captureDefaults"));PUTGD_TRY_COMPARE(service.snapshot().actions["clipboard"].destination,QString("local"));
  destination->setCurrentIndex(destination->findData(QString{}));PUTGD_TRY_COMPARE(service.snapshot().actions["region"].destination,QString("local"));QVERIFY(service.snapshot().archive.keepUploads);destination->setCurrentIndex(destination->findData(profileId));PUTGD_TRY_COMPARE(service.snapshot().actions["region"].destination,QString("upload"));QCOMPARE(service.snapshot().actions["clipboard"].destination,QString("local"));
  local->setChecked(false);PUTGD_TRY_VERIFY(!service.snapshot().archive.keepUploads);QCOMPARE(service.snapshot().actions["region"].profileId,profileId);QCOMPARE(service.snapshot().actions["clipboard"].destination,QString("local"));PUTGD_TRY_VERIFY(window.findChild<QLabel*>("captureStatusClipboard")->text().contains("New captures have no output."));
  MainWindow reopened(fixture.session);reopened.navigate("settings");QVERIFY(reopened.findChild<QCheckBox*>("useDefaultRegion")->isChecked());QVERIFY(!reopened.findChild<QCheckBox*>("useDefaultClipboard")->isChecked());QVERIFY(!validateUiState({{"schemaVersion",1},{"settings",window.settings()->saveViewState()}}));
 }
 void inheritedActionsShowTheDefaultOutputDisabled(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=Settings::developmentTemplate();auto proton=ProtonProvider::defaults();proton["shareByDefault"]=false;
  auto profiles=doc["profiles"].toArray();profiles.append(QJsonObject{{"id","synthetic-proton"},{"label","Synthetic Drive"},{"provider","proton-drive"},{"settings",proton},{"credentials",QJsonObject{{"mode","keychain"},{"reference","putgd/synthetic-proton"}}}});doc["profiles"]=profiles;
  doc["captureDefaults"]=QJsonObject{{"destination","upload"},{"profileId","development-s3"},{"publicationMode","default"}};auto actions=doc["actions"].toObject();for(const auto& name:Settings::actionNames()){auto action=actions[name].toObject();action["destination"]="default";action["profileId"]=QJsonValue::Null;actions[name]=action;}doc["actions"]=actions;QVERIFY(service.save(doc,{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(1);window.show();
  auto* exceptions=window.findChild<QToolButton*>("captureExceptionsToggle");QVERIFY(!exceptions->isChecked());exceptions->click();
  const QHash<QString,QString> labels{{"region","Capture region"},{"window","Capture window"},{"desktop","Capture desktop"},{"clipboard","Upload clipboard"},{"edit","Capture region & edit"}};
  auto title=[](const QString& name){return name.left(1).toUpper()+name.mid(1);};
  auto routing=[&]{const auto state=window.settings()->saveViewState();[&]{QVERIFY(!validateUiState({{"schemaVersion",1},{"settings",state}}));}();return state["captureRouting"].toObject();};
  auto expectInherited=[&](const QString& profileId,const QString& destination,const QString& sharing,const QString& effective,const QString& status){
   const auto routes=routing();
   for(const auto& name:Settings::actionNames()){
    auto* profile=window.findChild<QComboBox*>("profile"+title(name));auto* publication=window.findChild<QComboBox*>("publication"+title(name));auto* line=window.findChild<QLabel*>("captureStatus"+title(name));
    QVERIFY2(window.findChild<QCheckBox*>("useDefault"+title(name))->isChecked(),qPrintable(name));QVERIFY(!window.findChild<QCheckBox*>("override"+title(name))->isChecked());QVERIFY2(line->text().contains(status),qPrintable(line->text()));
    // The row defers to the default; resolved values remain in agent view state.
    QVERIFY2(window.findChild<QWidget*>("captureEditor"+title(name))->isHidden(),qPrintable(name));auto* summary=window.findChild<QLabel*>("captureSummary"+title(name));QVERIFY(!summary->isVisible());QCOMPARE(summary->text(),profileId.isEmpty()&&!window.findChild<QCheckBox*>("keepUploadCopies")->isChecked()?QString("Nothing is saved or uploaded"):QString("Same as above"));
    QVERIFY(!profile->isEnabled());QVERIFY(!publication->isEnabled());QCOMPARE(profile->currentData().toString(),profileId);QCOMPARE(profile->currentText(),destination);QCOMPARE(publication->currentText(),sharing);
    QCOMPARE(profile->accessibleName(),labels[name]+" destination: inherited from default, "+destination);QCOMPARE(publication->accessibleName(),labels[name]+" sharing: inherited from default, "+sharing);
    const auto route=routes[name].toObject();QVERIFY(route["inherited"].toBool());QVERIFY(!route["destinationEnabled"].toBool());QVERIFY(!route["sharingEnabled"].toBool());
    QCOMPARE(route["profileId"].toString(),profileId);QCOMPARE(route["destination"].toString(),destination);QCOMPARE(route["sharing"].toString(),sharing);QCOMPARE(route["effectivePublicationMode"].toString(),effective);QCOMPARE(route["status"].toString(),line->text());
   }
  };
  expectInherited("development-s3","put.gd development destination","Destination default — shared link","share","Uploads to the selected destination");
  QCOMPARE(routing()["default"].toObject()["publicationMode"].toString(),QString("default"));QVERIFY(routing()["default"].toObject()["sharingEnabled"].toBool());
  // Editing the default output updates every inheriting card at once.
  auto* destination=window.findChild<QComboBox*>("defaultCaptureDestination");auto* sharing=window.findChild<QComboBox*>("defaultCaptureSharing");
  destination->setCurrentIndex(destination->findData("synthetic-proton"));expectInherited("synthetic-proton","Synthetic Drive","Destination default — private upload","private","Uploads to the selected destination");
  sharing->setCurrentIndex(sharing->findData("share"));expectInherited("synthetic-proton","Synthetic Drive","Create a share link","share","Uploads to the selected destination");
  destination->setCurrentIndex(destination->findData(QString{}));expectInherited(QString{},"No destination","Create a share link","","New captures have no output.");
  QCOMPARE(window.findChild<QLabel*>("captureSummaryRegion")->property("feedbackTone").toString(),QString("neutral"));
  destination->setCurrentIndex(destination->findData("development-s3"));sharing->setCurrentIndex(sharing->findData("default"));expectInherited("development-s3","put.gd development destination","Destination default — shared link","share","Uploads to the selected destination");
  PUTGD_TRY_COMPARE(service.document()["captureDefaults"].toObject()["profileId"].toString(),QString("development-s3"));PUTGD_TRY_COMPARE(service.document()["captureDefaults"].toObject()["publicationMode"].toString(),QString("default"));
  // Unchecking starts from the inherited values and saves an explicit exception.
  window.findChild<QCheckBox*>("useDefaultRegion")->setChecked(false);auto* region=window.findChild<QComboBox*>("profileRegion");auto* regionSharing=window.findChild<QComboBox*>("publicationRegion");
  QVERIFY(window.findChild<QCheckBox*>("overrideRegion")->isChecked());QVERIFY(window.findChild<QWidget*>("captureEditorRegion")->isVisible());QVERIFY(window.findChild<QLabel*>("captureStatusRegion")->isHidden());QCOMPARE(window.findChild<QLabel*>("captureSummaryRegion")->text(),QString("Shared link · put.gd development destination"));
  QVERIFY(region->isEnabled());QVERIFY(regionSharing->isEnabled());QCOMPARE(region->currentData().toString(),QString("development-s3"));QCOMPARE(regionSharing->currentData().toString(),QString("default"));QCOMPARE(regionSharing->currentText(),QString("Destination default — shared link"));QCOMPARE(region->accessibleName(),QString("Capture region destination"));
  const auto route=routing()["region"].toObject();QVERIFY(!route["inherited"].toBool());QVERIFY(route["destinationEnabled"].toBool());QVERIFY(route["sharingEnabled"].toBool());QCOMPARE(route["effectivePublicationMode"].toString(),QString("share"));
  PUTGD_TRY_COMPARE(service.document()["actions"].toObject()["region"].toObject()["destination"].toString(),QString("upload"));QCOMPARE(service.document()["actions"].toObject()["region"].toObject()["profileId"].toString(),QString("development-s3"));
  for(const auto& name:Settings::actionNames())if(name!="region")QCOMPARE(service.document()["actions"].toObject()[name].toObject()["destination"].toString(),QString("default"));
  MainWindow reopened(fixture.session);reopened.settings();QVERIFY(!reopened.findChild<QCheckBox*>("useDefaultRegion")->isChecked());QVERIFY(reopened.findChild<QComboBox*>("profileRegion")->isEnabled());QVERIFY(reopened.findChild<QCheckBox*>("useDefaultEdit")->isChecked());QVERIFY(!reopened.findChild<QComboBox*>("profileEdit")->isEnabled());QCOMPARE(reopened.findChild<QComboBox*>("publicationEdit")->currentText(),QString("Destination default — shared link"));
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
  fixture.client.failOnce["settings.save"]={"client_unavailable","Synthetic lost reply",{}};field->setText("251");PUTGD_TRY_COMPARE(status->text(),QString("Changes not saved"));QVERIFY(status->toolTip().contains("Couldn’t save"));QVERIFY(window.findChild<QLabel*>("autosaveExplanation")->isHidden());QCOMPARE(service.snapshot().limits.maxFiles,250);window.findChild<QPushButton*>("retryAutosave")->click();PUTGD_TRY_COMPARE(service.snapshot().limits.maxFiles,251);PUTGD_TRY_VERIFY(status->text().isEmpty());
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
 void outputPopupGroupsMatchingActionsAndKeepsRecording(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=Settings::defaults();QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.show();auto* summary=window.findChild<QToolButton*>("captureOutputSummary");QVERIFY(summary);auto* menu=summary->menu();
  auto* region=menu->findChild<QAction*>("captureOutputDetailregion");auto* captureWindow=menu->findChild<QAction*>("captureOutputDetailwindow");auto* record=menu->findChild<QAction*>("captureOutputDetailrecord");QVERIFY(region&&captureWindow&&record);QVERIFY(!region->isVisible());QVERIFY(!captureWindow->isVisible());QVERIFY(!record->isVisible());QCOMPARE(menu->findChild<QAction*>("captureOutputDefault")->text(),QString("All actions: Nothing is saved or uploaded"));QVERIFY(summary->accessibleDescription().contains("Capture window:"));
  doc=Settings::developmentTemplate();QVERIFY(service.save(doc,service.revision()));PUTGD_TRY_VERIFY(region->isVisible());QVERIFY(region->text().contains("Capture window"));QVERIFY(region->text().contains("Capture desktop"));QVERIFY(!captureWindow->isVisible());auto* edit=menu->findChild<QAction*>("captureOutputDetailedit");QVERIFY(edit&&edit->isVisible());QVERIFY(edit->text().contains("Record screen"));QVERIFY(edit->text().contains("Upload when you choose"));QVERIFY(!record->isVisible());QVERIFY(summary->accessibleDescription().contains("Record screen: Upload when you choose"));
 }
 void captureDestinationPopupFitsAllThreeChoices(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto second=profiles.first().toObject();second["id"]="synthetic-second";second["label"]="Second destination";profiles.append(second);doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1060,680);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(1);window.show();auto* combo=window.findChild<QComboBox*>("defaultCaptureDestination");QVERIFY(combo);QCOMPARE(combo->count(),3);PUTGD_TRY_VERIFY(combo->width()>=combo->fontMetrics().horizontalAdvance(combo->itemText(1))+2*Graphite::space(Graphite::Space::l));combo->showPopup();auto* view=combo->view();PUTGD_TRY_VERIFY(view->isVisible());for(int i=0;i<combo->count();++i)QVERIFY2(view->viewport()->rect().contains(view->visualRect(combo->model()->index(i,0))),qPrintable(QString("Capture Destination choice %1 clipped: item %2,%3 %4x%5 viewport %6x%7 popup %8x%9").arg(i).arg(view->visualRect(combo->model()->index(i,0)).x()).arg(view->visualRect(combo->model()->index(i,0)).y()).arg(view->visualRect(combo->model()->index(i,0)).width()).arg(view->visualRect(combo->model()->index(i,0)).height()).arg(view->viewport()->width()).arg(view->viewport()->height()).arg(view->width()).arg(view->height())));QCOMPARE(view->verticalScrollBar()->maximum(),0);combo->hidePopup();
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
  auto* endpoint=window.findChild<QLabel*>("s3EndpointValue");auto* warning=window.findChild<QLabel*>("s3EndpointWarning");
  PUTGD_TRY_COMPARE(endpoint->text(),QString("https://s3.us-west-004.backblazeb2.com"));QCOMPARE(endpoint->font().family(),Graphite::monoFamily());
  QVERIFY(warning->isVisible());QVERIFY(warning->text().contains("lacks the conditional writes"));QCOMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["endpoint"].toString(),QString("https://s3.us-west-004.backblazeb2.com"));
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
  QTemporaryDir dir;CredentialStore secrets;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=configuration(dir.path(),true);doc["profiles"]=QJsonArray{QJsonObject{{"id","synthetic-proton"},{"label","Synthetic Proton"},{"provider","proton-drive"},{"settings",ProtonProvider::defaults()},{"credentials",QJsonObject{{"mode","keychain"},{"reference","putgd/synthetic-proton"}}}}};QVERIFY(service.save(doc,{}));UiSessionFixture fixture(service,secrets);int starts=0;JsonReply complete;fixture.setup.setProviderSetup([&](const QString& kind,const Destination&,const QJsonObject&,JsonReply done,auto event){QCOMPARE(kind,QString("connectAccount"));++starts;complete=done;event({{"event","auth"},{"url","https://account.proton.me/cli?synthetic=true"}});});openedUrls_.clear();host::setTestOpener([this](const QUrl& url){recordProtonUrl(url);});auto cleanup=qScopeGuard([]{host::setTestOpener({});});MainWindow window(fixture.session);window.navigate("destinations");window.show();auto* button=window.findChild<QPushButton*>("connectProtonAccount");button->click();PUTGD_TRY_COMPARE(openedUrls_.size(),1);QCOMPARE(openedUrls_.first().host(),QString("account.proton.me"));auto* account=window.findChild<QLabel*>("protonAccountStatus");QVERIFY(account);QVERIFY(account->text().contains("browser"));QVERIFY(account->isVisible());
  const auto setupId=window.settings()->saveViewState()["setupJobs"].toArray().first().toObject()["setupId"].toString();
  auto* retry=window.findChild<QPushButton*>("retryProtonBrowser");QVERIFY(retry&&!retry->isVisible());window.settings()->protonBrowserFinished(setupId,{{"opened",false}});QVERIFY(retry->isVisible()&&retry->isEnabled());QVERIFY(account->text().contains("Couldn't open"));
  QVERIFY(!QJsonDocument(window.settings()->saveViewState()).toJson().contains("account.proton.me"));retry->click();PUTGD_TRY_COMPARE(openedUrls_.size(),2);QCOMPARE(openedUrls_[0],openedUrls_[1]);QVERIFY(!retry->isVisible());QCOMPARE(starts,1);
  // While setup runs, its actions are disabled but the account and status text stay readable.
  QVERIFY(!button->isEnabled());QVERIFY(!window.findChild<QComboBox*>("protonFolder")->isEnabled());QVERIFY(account->isEnabled());QVERIFY(window.findChild<QLabel*>("protonFolderStatus")->isEnabled());QVERIFY(window.findChild<QLabel*>("destinationStatus")->text().contains("Connecting account"));putgd::test::settle(600);PUTGD_TRY_COMPARE(openedUrls_.size(),2);PUTGD_TRY_COMPARE(starts,1);PUTGD_TRY_VERIFY(complete);complete({"proton_runtime_unavailable","Synthetic runtime unavailable",{}},{});PUTGD_TRY_VERIFY(account->text().contains("Synthetic runtime unavailable"));PUTGD_TRY_VERIFY(button->isEnabled());window.settings()->protonBrowserFinished(setupId,{{"opened",false}});QVERIFY(!retry->isVisible());window.settings()->retryProtonBrowser();QCOMPARE(openedUrls_.size(),2);
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
 void captureRailExplainsActionAndCurrentSavedOutputWithoutInvokingIt(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);int invoked=0;connect(&window,&MainWindow::actionRequested,this,[&](auto){++invoked;});const QHash<QString,QString> labels{{"region","Capture region"},{"window","Capture window"},{"desktop","Capture desktop"},{"clipboard","Upload clipboard"},{"edit","Capture region & edit"}};for(const auto& action:Settings::actionNames()){QToolButton* button=nullptr;for(auto* candidate:window.findChildren<QToolButton*>("captureActionButton"))if(candidate->property("captureAction")==action)button=candidate;QVERIFY(button);QCOMPARE(button->accessibleName(),labels.value(action));QVERIFY(button->toolTip().startsWith(labels.value(action)));QVERIFY(button->toolTip().contains("\n"));QVERIFY(!button->accessibleDescription().isEmpty());}auto* help=window.findChild<QAction*>("captureActionsHelp");QVERIFY(help);help->trigger();auto* dialog=window.findChild<QMessageBox*>("settingHelpDialog");QVERIFY(dialog);QCOMPARE(dialog->windowTitle(),QString("Help"));QCOMPARE(dialog->text(),QString("Capture actions"));QVERIFY(dialog->informativeText().contains("focused external app window"));QVERIFY(dialog->informativeText().contains("Select the output summary in the status bar"));dialog->close();
  {QWidget host;QLineEdit control(&host);auto* row=VisualStyle::settingLabel("Screen region & edit shortcut","help",&control,&host);auto* caption=row->findChild<QLabel*>();QVERIFY(caption);QCOMPARE(caption->text(),QString("Screen region && edit shortcut"));QCOMPARE(caption->buddy(),&control);auto* info=row->findChild<QToolButton*>();QVERIFY(info&&!info->icon().isNull()&&info->text().isEmpty());QCOMPARE(control.accessibleName(),QString("Screen region & edit shortcut"));}
  auto* summary=window.findChild<QToolButton*>("captureOutputSummary");QVERIFY(summary->accessibleDescription().contains("Local only"));QVERIFY(summary->menu());QCOMPARE(invoked,0);
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
 void displayUnitsPreserveStoredBytesPixelsAndMilliseconds(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider s3(secrets);SettingsService service(dir.filePath("config.json"),[&](auto,auto settings){return s3.validateSettings(settings);});QVERIFY(service.save(Settings::developmentTemplate(),{}));UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();QCOMPARE(window.findChild<QLineEdit*>("maxFileBytes")->text(),QString("Auto"));QCOMPARE(window.findChild<QLineEdit*>("imagePixels")->text(),QString("100"));QCOMPARE(window.findChild<QLineEdit*>("s3ConnectTimeoutMs")->text(),QString("5"));window.findChild<QLineEdit*>("maxFileBytes")->setText("2.000000000931322574615478515625");window.findChild<QLineEdit*>("imagePixels")->setText("100.000001");window.findChild<QLineEdit*>("s3ConnectTimeoutMs")->setText("5.123");PUTGD_TRY_COMPARE(service.document()["limits"].toObject()["maxFileBytes"].toInteger(),2147483649LL);QCOMPARE(service.document()["limits"].toObject()["imagePixels"].toInteger(),100000001LL);QCOMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["connectTimeoutMs"].toInt(),5123);QCOMPARE(window.findChild<QLineEdit*>("maxFileBytes")->text(),QString("2.000000000931322574615478515625"));
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
 // Each command appears once: the application menu repeats neither the
 // command band nor the header's navigation, and Capture region has no menu
 // of the window and desktop buttons beside it.
 void applicationMenuDoesNotRepeatTheWindow(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);
  auto* menu=window.findChild<QMenu*>("applicationMenuPopup");QVERIFY(menu);
  QStringList entries;for(auto* action:menu->actions())if(!action->isSeparator()){QVERIFY(action->icon().isNull());entries<<action->text().replace("&&","&");}
  QCOMPARE(entries,QStringList({"Capture actions","Check for updates","About put.gd","Quit put.gd"}));
  QStringList band;for(auto* button:window.findChildren<QToolButton*>("captureActionButton")){band<<button->accessibleName();if(button->property("captureAction")!="edit")QVERIFY2(!button->menu(),qPrintable(button->accessibleName()));}
  for(const auto& entry:entries)QVERIFY2(!band.contains(entry)&&!QStringList({"History","Destinations","Settings"}).contains(entry),qPrintable(entry));
  QVERIFY(!window.findChild<QMenu*>("captureRegionMenu"));
 }
 // #239: the clipboard action says upload, like the tray; ids and keys stay.
 void clipboardActionSaysUpload(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();
  QToolButton* clipboard=nullptr;for(auto* button:window.findChildren<QToolButton*>("captureActionButton"))if(button->property("captureAction")=="clipboard")clipboard=button;
  QVERIFY(clipboard);QCOMPARE(clipboard->accessibleName(),QString("Upload clipboard"));QVERIFY(clipboard->accessibleDescription().startsWith("Upload copied local files"));QVERIFY(clipboard->accessibleDescription().contains("uploaded as text"));
  auto* helpAction=window.findChild<QAction*>("captureActionsHelp");QVERIFY(helpAction);helpAction->trigger();auto* helpDialog=window.findChild<QMessageBox*>("settingHelpDialog");QVERIFY(helpDialog);QVERIFY(helpDialog->informativeText().contains("Upload clipboard: upload copied local files",Qt::CaseInsensitive));QVERIFY(!helpDialog->informativeText().contains("mport"));helpDialog->close();
  auto* fileTemplate=window.findChild<QLineEdit*>("fileTemplate");QCOMPARE(fileTemplate->accessibleName(),QString("Copied & uploaded files"));QVERIFY(fileTemplate->toolTip().startsWith("Names copied and uploaded files when Custom template is selected"));
  QCOMPARE(fileTemplate->property("configPath").toString(),QString("/naming/fileTemplate"));
  // The size limits' help sits on their Auto / No limit choice.
  auto help=[&](const QString& key){auto* choice=window.findChild<QWidget*>(key+"Choice");return (choice?choice:window.findChild<QWidget*>(key))->toolTip();};
  for(const auto* name:{"maxFiles","maxFileBytes","pendingArtifactBytes","clipboardTextBytes"}){const auto text=help(name);QVERIFY2(!text.isEmpty(),name);QVERIFY2(!text.contains("mport"),qPrintable(text));}
  QVERIFY(help("maxFileBytes").startsWith("The largest individual uploaded file"));QVERIFY(help("clipboardTextBytes").contains("rejected before uploading"));
  QVERIFY(window.findChild<QWidget*>("useDefaultClipboard"));QCOMPARE(window.findChild<QCheckBox*>("useDefaultClipboard")->accessibleName(),QString("Upload clipboard: use default output"));
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
 void symbolicLimitsHideUnitsAndNumericLimitsKeepThem(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);window.show();
  for(const auto& key:QStringList{"maxFileBytes","maxBatchBytes","pendingArtifactBytes","historyRetentionDays"}){
   auto* edit=window.findChild<QLineEdit*>(key);auto* suffix=window.findChild<QLabel*>(key+"Unit");auto* editor=key=="historyRetentionDays"?window.findChild<QComboBox*>("historyRetentionChoice"):window.findChild<QComboBox*>(key+"Choice");QVERIFY(edit&&suffix&&editor);
   const auto values=key=="historyRetentionDays"?QStringList{"","12","","30"}:QStringList{"Auto","","12","auto","3.5"};for(const auto& value:values){edit->setText(value);bool numeric=false;value.toDouble(&numeric);QCOMPARE(!suffix->isHidden(),numeric);QCOMPARE(editor->property("hasUnit").toBool(),numeric);}
  }
  QVERIFY(window.findChild<QLineEdit*>("pendingArtifactBytes"));QCOMPARE(window.findChild<QComboBox*>("pendingArtifactBytesChoice")->accessibleName(),QString("Space for queued files"));QCOMPARE(window.findChild<QLineEdit*>("pendingOperations")->accessibleName(),QString("Queued operations"));
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
 void detailedSettingHelpIsReachableFromKeyboard(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);window.show();auto* help=window.findChild<QToolButton*>("historyRetentionChoiceHelp");QVERIFY(help);QVERIFY(help->isVisible());help->setFocus();QTest::keyClick(help,Qt::Key_Space);auto* note=window.findChild<QLabel*>("historyRetentionChoiceHelpText");QVERIFY(note);QVERIFY(note->isVisible());QVERIFY(note->wordWrap());QVERIFY(note->text().contains("metadata"));QVERIFY(note->mapTo(&window,QPoint()).y()>window.findChild<QComboBox*>("historyRetentionChoice")->mapTo(&window,QPoint()).y());QTest::keyClick(help,Qt::Key_Space);QVERIFY(note->isHidden());QVERIFY(!window.findChild<QMessageBox*>("settingHelpDialog"));
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
  finish({},{{"accountRootUid","synthetic-account"},{"accountEmail","synthetic@example.invalid"},{"folders",QJsonArray{QJsonObject{{"uid","synthetic-folder"},{"name","Saved folder"}},QJsonObject{{"uid","synthetic-other-id"},{"name","Another folder"}}}}});auto* folders=window.findChild<QComboBox*>("protonFolder");PUTGD_TRY_VERIFY(folders->findText("Another folder")>=0);QCOMPARE(folders->currentText(),QString("Saved folder"));QVERIFY(folders->findText("synthetic-other-id")==-1);auto* account=window.findChild<QLabel*>("protonAccountStatus");QCOMPARE(account->text(),QString("synthetic@example.invalid"));auto* list=window.findChild<QListWidget*>("destinationList");
  for(int repeat=0;repeat<5;++repeat){list->setCurrentRow(1);list->setCurrentRow(0);QVERIFY(account->isVisible());QVERIFY(account->text().contains("synthetic@example.invalid"));}QCOMPARE(inspections,1);window.navigate("history");window.navigate("destinations");QCOMPARE(inspections,1);auto* refresh=window.findChild<QPushButton*>("inspectProtonAccount");QVERIFY(refresh&&refresh->isVisible()&&refresh->isEnabled());QCOMPARE(folders->findData("__refresh__"),-1);refresh->click();PUTGD_TRY_COMPARE(inspections,2);QVERIFY(!refresh->isEnabled());QCOMPARE(folders->currentData().toString(),QString("synthetic-folder"));QVERIFY(progress->isVisible());QVERIFY(account->text().contains("synthetic@example.invalid"));finish({"synthetic_network","Could not load folders",{}},{});PUTGD_TRY_VERIFY(!progress->isVisible());QVERIFY(refresh->isEnabled());QVERIFY(account->text().contains("synthetic@example.invalid"));refresh->click();PUTGD_TRY_COMPARE(inspections,3);finish({},{{"accountRootUid","synthetic-account"},{"folders",QJsonArray{QJsonObject{{"uid","synthetic-folder"},{"name","Renamed saved folder"}},QJsonObject{{"uid","synthetic-new-id"},{"name","New folder"}}}}});PUTGD_TRY_VERIFY(folders->findText("New folder")>=0);QCOMPARE(folders->findText("Another folder"),-1);QCOMPARE(folders->currentData().toString(),QString("synthetic-folder"));QCOMPARE(folders->currentText(),QString("Renamed saved folder"));QVERIFY(account->text().contains("synthetic@example.invalid"));PUTGD_TRY_COMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["folderName"].toString(),QString("Renamed saved folder"));PUTGD_TRY_VERIFY(!window.settings()->saveViewState().contains("stateError"));QVERIFY(!validateUiState({{"schemaVersion",1},{"settings",window.settings()->saveViewState()}}));auto legacy=window.settings()->saveViewState();auto editors=legacy["profileEditors"].toArray();auto editor=editors[0].toObject();auto retained=editor["proton"].toObject();auto oldFolders=retained["folders"].toArray();oldFolders.append(QJsonObject{{"uid","__refresh__"},{"name",""},{"label","Refresh folders…"}});retained["folders"]=oldFolders;editor["proton"]=retained;editors[0]=editor;legacy["profileEditors"]=editors;MainWindow restored(fixture.session);QVERIFY(restored.settings()->restoreViewState(legacy));QCOMPARE(restored.findChild<QComboBox*>("protonFolder")->findData("__refresh__"),-1);QCOMPARE(restored.findChild<QComboBox*>("protonFolder")->currentData().toString(),QString("synthetic-folder"));QVERIFY(restored.findChild<QLabel*>("protonAccountStatus")->text().contains("synthetic@example.invalid"));QVERIFY(!QJsonDocument(service.document()).toJson().contains("synthetic@example.invalid"));QCOMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["folderUid"].toString(),QString("synthetic-folder"));
 }
 void connectedProtonAccountSurvivesRapidNavigation(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto proton=profiles.first().toObject();proton["id"]="synthetic-proton";proton["label"]="Synthetic Proton";proton["provider"]="proton-drive";auto account=ProtonProvider::defaults();account["accountRootUid"]="synthetic-account";account["folderUid"]="synthetic-folder";account["folderName"]="Saved folder";proton["settings"]=account;profiles.prepend(proton);doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.show();window.navigate("destinations");auto* list=window.findChild<QListWidget*>("destinationList");auto* label=window.findChild<QLabel*>("protonAccountStatus");auto* connect=window.findChild<QPushButton*>("connectProtonAccount");
  for(int repeat=0;repeat<5;++repeat){list->setCurrentRow(1);list->setCurrentRow(0);QVERIFY(label->isVisible());QVERIFY(label->text().startsWith("Account configured"));QCOMPARE(connect->text(),QString("Reconnect account…"));window.navigate("history");window.navigate("destinations");QVERIFY(label->isVisible());QVERIFY(label->text().startsWith("Account configured"));}
  putgd::test::settle(650);PUTGD_TRY_COMPARE(service.document(),doc);PUTGD_TRY_VERIFY(label->text().startsWith("Account configured"));PUTGD_TRY_VERIFY(!validateUiState({{"schemaVersion",1},{"settings",window.settings()->saveViewState()}}));
 }
 void protonConnectionCompletedWhileAwayIsShownOnReturn(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();const auto other=profiles.first();auto proton=other.toObject();proton["id"]="synthetic-proton";proton["label"]="Synthetic Proton";proton["provider"]="proton-drive";auto oldAccount=ProtonProvider::defaults();oldAccount["accountRootUid"]="synthetic-old-account";oldAccount["folderUid"]="synthetic-old-folder";proton["settings"]=oldAccount;profiles.prepend(proton);doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);JsonReply complete;int starts=0;fixture.setup.setProviderSetup([&](const QString& kind,const Destination&,const QJsonObject&,JsonReply done,auto){QCOMPARE(kind,QString("connectAccount"));++starts;complete=std::move(done);});MainWindow window(fixture.session);window.show();window.navigate("destinations");auto* list=window.findChild<QListWidget*>("destinationList");window.findChild<QPushButton*>("connectProtonAccount")->click();PUTGD_TRY_VERIFY(bool(complete));
  for(int repeat=0;repeat<3;++repeat){list->setCurrentRow(1);list->setCurrentRow(0);}list->setCurrentRow(1);complete({},{{"accountRootUid","synthetic-new-account"},{"accountEmail","synthetic-new@example.invalid"},{"folders",QJsonArray{QJsonObject{{"uid","synthetic-folder"},{"name","Saved folder"}}}}});PUTGD_TRY_VERIFY(window.settings()->saveViewState()["setupJobs"].toArray().isEmpty());QCOMPARE(service.document()["profiles"].toArray()[1],other);
  list->setCurrentRow(0);auto* label=window.findChild<QLabel*>("protonAccountStatus");QVERIFY(label->isVisible());PUTGD_TRY_VERIFY(label->text().contains("synthetic-new@example.invalid"));QCOMPARE(window.findChild<QPushButton*>("connectProtonAccount")->text(),QString("Reconnect account…"));PUTGD_TRY_COMPARE(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["accountRootUid"].toString(),QString("synthetic-new-account"));QCOMPARE(starts,1);QVERIFY(service.document()["profiles"].toArray()[0].toObject()["settings"].toObject()["folderUid"].toString().isEmpty());QCOMPARE(service.document()["profiles"].toArray()[1],other);
 }
 void storedCredentialHintsNeverExportFullKeysOrOverwriteInputs(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider s3(secrets);auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();profile["credentials"]=QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}};profiles[0]=profile;doc["profiles"]=profiles;SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return s3.validateSettings(value);});QVERIFY(service.save(doc,{}));secrets.setSession("development-s3",{"AKIA-synthetic-full-access","synthetic-full-secret","synthetic-token"});UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();auto* hint=window.findChild<QLabel*>("storedCredentialHint");PUTGD_TRY_VERIFY(hint->text().contains("AKIA"));QVERIFY(hint->text().contains("Secret key stored"));auto* access=window.findChild<QLineEdit*>("credentialAccessKey");QVERIFY(access->text().isEmpty());// The status row identifies the key; the field only invites a replacement.
QVERIFY(!access->placeholderText().isEmpty());QVERIFY(!access->placeholderText().contains("AKIA"));QVERIFY(hint->text().contains("For this session"));QCOMPARE(window.findChild<QLineEdit*>("credentialSecretKey")->echoMode(),QLineEdit::Password);const auto state=QJsonDocument(window.settings()->saveViewState()).toJson();QVERIFY(!state.contains("AKIA"));QVERIFY(!state.contains("synthetic-full-secret"));bool done=false;QJsonObject reply;fixture.session.request("setup.credentialHint",{{"profileId","development-s3"},{"expectedRevision",service.revision()}},this,[&](Error error,QJsonObject value){QVERIFY(!error);reply=value;done=true;});PUTGD_TRY_VERIFY(done);const auto encoded=QJsonDocument(reply).toJson();QVERIFY(!encoded.contains("synthetic-full-access"));QVERIFY(!encoded.contains("synthetic-full-secret"));QVERIFY(!encoded.contains("synthetic-token"));
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
  complete={};test->click();PUTGD_TRY_VERIFY(bool(complete));QVERIFY(status->text().contains("Testing upload"));
  complete({"s3_permission_denied","S3 denied the test upload. Check IAM and bucket policy permissions.",{}});
  PUTGD_TRY_VERIFY(test->isEnabled());PUTGD_TRY_VERIFY(status->text().contains("IAM and bucket policy"));
  QCOMPARE(status->property("feedbackTone").toString(),QString("error"));
 }
 void destinationActionsDescribeTheActiveProvider(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=threeDestinations();auto profiles=doc["profiles"].toArray();auto b2=profiles[1].toObject();b2["provider"]="backblaze-b2";b2["settings"]=b2::defaults();profiles[1]=b2;auto proton=profiles[2].toObject();proton["provider"]="proton-drive";proton["settings"]=ProtonProvider::defaults();profiles[2]=proton;doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();auto* list=window.findChild<QListWidget*>("destinationList");auto* test=window.findChild<QPushButton*>("testDestination");auto* secret=window.findChild<QLineEdit*>("credentialSecretKey");QPushButton* show=nullptr;for(auto* button:window.findChildren<QPushButton*>())if(button->accessibleName()=="Show secret access key")show=button;QVERIFY(show);auto* scope=window.findChild<QLabel*>("destinationTestScope");QVERIFY(scope&&scope->isVisible());QCOMPARE(test->text(),QString("Test upload and link"));
  list->setCurrentRow(1);QCOMPARE(secret->accessibleName(),QString("Application key"));QCOMPARE(show->accessibleName(),QString("Show application key"));QCOMPARE(window.findChild<QAction*>("showCredentialSecretAction")->text(),show->accessibleName());secret->setText("synthetic-input");show->click();QCOMPARE(show->accessibleName(),QString("Hide application key"));show->click();QCOMPARE(show->accessibleName(),QString("Show application key"));
  list->setCurrentRow(2);QCOMPARE(test->text(),QString("Check account and folder"));QCOMPARE(test->accessibleName(),test->text());QVERIFY(scope->isVisible());QVERIFY(scope->text().contains("Upload and sharing aren't tested"));
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
  PUTGD_TRY_COMPARE(statusCode(window),QString("credential_setup_required"));
  QVERIFY(secret->text().isEmpty());QVERIFY(!show->isChecked());QVERIFY(!secret->isReadOnly());
 }
 void uiStateStrictlyRejectsUnexpectedAndSecretFields(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);auto state=QJsonObject{{"schemaVersion",1},{"settings",window.settings()->saveViewState()},{"main",window.saveViewState()}};QVERIFY(!validateUiState(state));auto main=state["main"].toObject();main["secretKey"]="synthetic-secret";state["main"]=main;auto error=validateUiState(state);QVERIFY(error);QVERIFY(!QJsonDocument(error.toJson()).toJson().contains("synthetic-secret"));
 }
 void acceptedSetupIdSurvivesAnUncertainInspectionReply(){
  QTemporaryDir d;CredentialStore secrets;S3Provider provider(secrets);SettingsService service(d.filePath("config.json"),[&](auto,auto settings){return provider.validateSettings(settings);});QVERIFY(service.save(Settings::developmentTemplate(),{}));int starts=0;std::function<void(Error)> finish;UiSessionFixture fixture(service,secrets,[&](auto,auto,auto reply){++starts;finish=reply;});fixture.client.failOnce["setup.inspect"]={"client_unavailable","Synthetic lost inspection reply",{}};SettingsWindow window(fixture.session);window.findChild<QPushButton*>("testDestination")->click();PUTGD_TRY_VERIFY(statusCode(window)=="client_unavailable");const auto saved=window.saveViewState();QCOMPARE(saved["setupJobs"].toArray().size(),1);SettingsWindow reopened(fixture.session);QVERIFY(reopened.restoreViewState(saved));QVERIFY(finish);finish({});PUTGD_TRY_VERIFY(reopened.findChild<QLabel*>("settingsStatus")->text().contains("Connection successful"));QCOMPARE(starts,1);
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
  PUTGD_TRY_COMPARE(status->property("errorCode").toString(),QString("client_unavailable"));
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
  PUTGD_TRY_COMPARE(statusCode(window),QString("setup_not_found"));
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
  // Controls are disabled one by one, not their sections: labels stay enabled.
  QVERIFY(!window.findChild<QLineEdit*>("s3Bucket")->isEnabled());QVERIFY(!window.findChild<QPushButton*>("storeCredentials")->isEnabled());QVERIFY(window.findChild<QWidget*>("destinationEditor")->isEnabled());QVERIFY(window.findChild<QLabel*>("storedCredentialHint")->isEnabled());
  finish["second-s3"]({});
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(window.saveViewState()["setupJobs"].toArray().isEmpty(),2000);
  QVERIFY(window.findChild<QPushButton*>("testDestination")->isEnabled());QCOMPARE(starts,2);
  // Afterwards each control returns to its provider's state.
  QVERIFY(window.findChild<QLineEdit*>("s3Bucket")->isEnabled());QVERIFY(window.findChild<QComboBox*>("s3AclMode")->isEnabled());QVERIFY(!window.findChild<QComboBox*>("b2LinkMode")->isEnabled());
 }
 void localCopiesShareTheCapturePageWithTheBlockingNotice(){
  // One output decision on one page: Local copies sit on Capture, and the
  // no-output notice sits right under Destination with the two ways out.
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=Settings::developmentTemplate();auto archive=doc["archive"].toObject();archive["keepUploads"]=false;doc["archive"]=archive;QVERIFY(service.save(doc,{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1060,680);window.navigate("settings");auto* tabs=window.findChild<QTabWidget*>("settingsTabs");tabs->setCurrentIndex(1);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  auto* keep=window.findChild<QCheckBox*>("keepUploadCopies");QVERIFY(keep);QVERIFY(tabs->widget(1)->isAncestorOf(keep));QVERIFY(!tabs->widget(0)->isAncestorOf(keep));QVERIFY(tabs->widget(1)->isAncestorOf(window.findChild<QLineEdit*>("archiveRoot")));
  auto* destination=window.findChild<QComboBox*>("defaultCaptureDestination");auto* sharing=window.findChild<QComboBox*>("defaultCaptureSharing");auto* block=window.findChild<QWidget*>("captureOutputBlock");QVERIFY(destination&&sharing&&block);
  QVERIFY(!block->isVisible());
  destination->setCurrentIndex(destination->findData(QString{}));PUTGD_TRY_VERIFY(block->isVisible());
  auto* page=tabs->widget(1);const auto top=[&](QWidget* widget){return widget->mapTo(page,QPoint()).y();};QVERIFY(top(block)>top(destination));QVERIFY(top(block)<top(sharing));
  auto* scroll=qobject_cast<QScrollArea*>(page);QVERIFY(scroll);QVERIFY(scroll->viewport()->rect().contains(QRect(block->mapTo(scroll->viewport(),QPoint()),block->size())));
  QVERIFY(window.findChild<QLabel*>("captureOutputNotice")->text().contains("New captures have no output."));
  QCOMPARE(window.findChild<QPushButton*>("chooseCaptureDestination")->text(),QString("Choose a destination"));
  window.findChild<QPushButton*>("enableLocalCopies")->click();QVERIFY(keep->isChecked());PUTGD_TRY_VERIFY(service.snapshot().archive.keepUploads);QVERIFY(!block->isVisible());
  // With no destination at all, the notice's action goes to Destinations to add one.
  QTemporaryDir fresh;SettingsService first(fresh.filePath("config.json"));CredentialStore none;UiSessionFixture empty(first,none);MainWindow initial(empty.session);initial.navigate("settings");initial.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(1);initial.show();
  initial.findChild<QCheckBox*>("keepUploadCopies")->setChecked(false);auto* add=initial.findChild<QPushButton*>("chooseCaptureDestination");PUTGD_TRY_VERIFY(add->isVisible());QCOMPARE(add->text(),QString("Add a destination…"));
  add->click();QCOMPARE(initial.findChild<QTabBar*>("mainNavigation")->currentIndex(),1);
 }
 void deepLinksRevealAndFocusTheirControl(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);
  MainWindow window(fixture.session);window.resize(1060,680);window.navigate("settings");auto* tabs=window.findChild<QTabWidget*>("settingsTabs");tabs->setCurrentIndex(1);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  auto* scroll=qobject_cast<QScrollArea*>(tabs->widget(1));auto* bar=scroll->verticalScrollBar();PUTGD_TRY_VERIFY(bar->maximum()>0);bar->setValue(bar->maximum());
  // Change output… from History: Capture from its top, Destination focused.
  window.navigate("history");window.findChild<QAction*>("changeCaptureOutput")->trigger();
  auto* destination=window.findChild<QComboBox*>("profileClipboard");PUTGD_TRY_COMPARE(window.focusWidget(),static_cast<QWidget*>(destination));
  QCOMPARE(tabs->currentIndex(),1);QVERIFY(scroll->viewport()->rect().contains(QRect(destination->mapTo(scroll->viewport(),QPoint()),destination->size())));
  window.activateWindow();window.navigate("history");window.showLocalCopySettings();auto* keep=window.findChild<QCheckBox*>("keepUploadCopies");
  PUTGD_TRY_VERIFY(keep->hasFocus());QVERIFY(Graphite::keyboardFocus(keep));QCOMPARE(tabs->currentIndex(),1);
  const auto shot=keep->grab().toImage();const auto accent=Theme::current().accent;int focusPixels=0;for(int y=0;y<shot.height();++y)for(int x=0;x<shot.width();++x){const auto c=shot.pixelColor(x,y);if(qAbs(c.red()-accent.red())<16&&qAbs(c.green()-accent.green())<16&&qAbs(c.blue()-accent.blue())<16)++focusPixels;}QVERIFY2(focusPixels>=4,"The revealed checkbox must have a visible keyboard-focus cue");
  QVERIFY(scroll->viewport()->rect().contains(QRect(keep->mapTo(scroll->viewport(),QPoint()),keep->size())));
  // Updates: without an update action or a usable Check now, the status itself takes focus.
  window.settings()->showUpdates();auto* status=window.findChild<QLabel*>("updateStatus");PUTGD_TRY_COMPARE(window.focusWidget(),static_cast<QWidget*>(status));QCOMPARE(tabs->currentIndex(),0);
 }
 void storedCredentialsShowOneStatusRowAboveSpacedActions(){
  QTemporaryDir dir;CredentialStore secrets;S3Provider s3(secrets);auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();profile["credentials"]=QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}};profiles[0]=profile;doc["profiles"]=profiles;SettingsService service(dir.filePath("config.json"),[&](auto,auto value){return s3.validateSettings(value);});QVERIFY(service.save(doc,{}));secrets.setSession("development-s3",{"AKIA-synthetic-full-access","synthetic-full-secret",{}});
  UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1060,680);window.navigate("destinations");window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  auto* hint=window.findChild<QLabel*>("storedCredentialHint");PUTGD_TRY_VERIFY(hint->isVisible()&&hint->text().contains("AKIA"));
  // The identification appears once: in the status row, under the heading and above the fields.
  auto* editor=window.findChild<QWidget*>("credentialsEditor");int mentions=0;for(auto* label:editor->findChildren<QLabel*>())if(label->isVisible()&&label->text().contains("AKIA"))++mentions;QCOMPARE(mentions,1);
  for(auto* field:editor->findChildren<QLineEdit*>())QVERIFY2(!field->placeholderText().contains("AKIA")&&!field->placeholderText().contains("Stored"),qPrintable(field->objectName()));
  auto* access=window.findChild<QLineEdit*>("credentialAccessKey");QVERIFY(hint->mapTo(editor,QPoint()).y()<access->mapTo(editor,QPoint()).y());
  // Session credentials need only the primary store action; nothing is locked.
  auto* store=window.findChild<QPushButton*>("storeCredentials");auto* unlock=window.findChild<QPushButton*>("unlockCredentials");QVERIFY(store&&unlock);QVERIFY(unlock->isHidden());QCOMPARE(store->property("visualRole").toString(),QString("primary"));QCOMPARE(store->width(),store->sizeHint().width());QCOMPARE(store->mapTo(editor,QPoint(store->width(),0)).x(),access->mapTo(editor,QPoint(access->width(),0)).x());
 }
 void destinationTitleElidesOnlyWhileUnfocused(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();const auto name=QString("Production screenshots for customer support — ").repeated(3).left(120);profile["label"]=name;profiles[0]=profile;doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));
  CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1060,680);window.navigate("destinations");window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  auto* label=window.findChild<QLineEdit*>("destinationLabel");QVERIFY(!window.findChild<QToolButton*>("destinationLabelHelp"));window.findChild<QPushButton*>("testDestination")->setFocus();QVERIFY(!label->hasFocus());
  QVERIFY(label->fontMetrics().horizontalAdvance(name)>label->width());
  // The full name is in the tooltip while the title is shortened.
  QHelpEvent tip(QEvent::ToolTip,QPoint(8,8),label->mapToGlobal(QPoint(8,8)));QApplication::sendEvent(label,&tip);PUTGD_TRY_COMPARE(QToolTip::text(),name);QToolTip::hideText();
  // Renaming retains the inline editing and accessibility paths.
  label->setFocus();label->selectAll();QTest::keyClicks(label,"Archive");window.findChild<QPushButton*>("testDestination")->setFocus();PUTGD_TRY_COMPARE(label->text(),QString("Archive"));QVERIFY(!label->accessibleDescription().isEmpty());
 }
 void shortcutRecoveryIsOnePageLevelAction(){
  ShortcutEditor editor;editor.setActions(Settings::defaults()["actions"].toObject());int refreshes=0;editor.setRefreshAction([&]{++refreshes;});editor.show();
  auto title=[](const QString& name){return name.left(1).toUpper()+name.mid(1);};
  auto* refresh=editor.findChild<QPushButton*>("refreshShortcutSupport");QVERIFY(refresh);
  // Nothing reported yet: one status and one Refresh, no repeated row notes.
  editor.setStatus({});QVERIFY(refresh->isVisible());for(const auto& action:Settings::invocableActionNames())QVERIFY2(editor.findChild<QLabel*>("shortcut"+title(action)+"Status")->isHidden(),qPrintable(action));
  refresh->click();QCOMPARE(refreshes,1);
  // A problem specific to one action keeps its own line.
  QJsonObject bindings;for(const auto& action:Settings::invocableActionNames()){const auto draft=editor.editors().value(action)->text();if(!draft.isEmpty())bindings[action]=QJsonObject{{"requested",draft},{"actual",QJsonArray{draft}},{"bound",true},{"state","bound"}};}
  auto conflicted=bindings;conflicted["region"]=QJsonObject{{"requested",editor.editors().value("region")->text()},{"state","shortcut_conflict"}};
  editor.setStatus({{"available",true},{"bindings",conflicted}});auto* region=editor.findChild<QLabel*>("shortcutRegionStatus");QVERIFY(!region->isHidden());QVERIFY(region->text().startsWith("Conflict"));QVERIFY(refresh->isVisible());
  for(const auto& action:Settings::invocableActionNames())if(action!="region")QVERIFY2(editor.findChild<QLabel*>("shortcut"+title(action)+"Status")->isHidden(),qPrintable(action));
  // Everything confirmed: nothing to refresh.
  editor.setStatus({{"available",true},{"bindings",bindings}});QVERIFY(refresh->isHidden());QVERIFY(region->isHidden());
  // In Settings, the action asks the resident again.
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);int snapshots=0;fixture.server.setUiDiagnostics([&]{++snapshots;return fixture.operations().diagnostics();});
  MainWindow window(fixture.session);window.settings();PUTGD_TRY_VERIFY(fixture.session.ready());const int before=snapshots;window.findChild<QPushButton*>("refreshShortcutSupport")->click();PUTGD_TRY_VERIFY(snapshots>before);
 }
 void settingsAndDestinationsFitAt1060_data(){QTest::addColumn<int>("pixels");QTest::addColumn<bool>("light");for(const int pixels:{14,21})for(const bool light:{false,true})QTest::newRow(qPrintable(QString("%1-%2").arg(pixels).arg(light?"light":"dark")))<<pixels<<light;}
 void settingsAndDestinationsFitAt1060(){
  QFETCH(int,pixels);QFETCH(bool,light);const auto oldFont=qApp->font();const auto oldScheme=Theme::scheme();auto restore=qScopeGuard([&]{qApp->setFont(oldFont);Theme::setScheme(oldScheme);});auto font=oldFont;font.setPixelSize(pixels);qApp->setFont(font);Theme::setScheme(light?Theme::Scheme::Light:Theme::Scheme::Dark);
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1060,680);window.navigate("settings");window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));auto* tabs=window.findChild<QTabWidget*>("settingsTabs");
  auto fits=[&](QScrollArea* scroll){QVERIFY(scroll);QCOMPARE(window.width(),1060);PUTGD_TRY_VERIFY2(scroll->widget()->width()<=scroll->viewport()->width(),qPrintable(QString("Page %1 > viewport %2").arg(scroll->widget()->width()).arg(scroll->viewport()->width())));for(auto* form:scroll->widget()->findChildren<QFormLayout*>()){if(!form->parentWidget()->isVisibleTo(scroll->widget()))continue;for(int row=0;row<form->rowCount();++row){if(!form->isRowVisible(row))continue;auto* label=form->itemAt(row,QFormLayout::LabelRole);auto* field=form->itemAt(row,QFormLayout::FieldRole);if(label&&field){PUTGD_TRY_VERIFY(!label->geometry().intersects(field->geometry()));PUTGD_TRY_VERIFY(field->geometry().width()>=field->minimumSize().width());}}}};
  for(int page=0;page<tabs->count();++page){tabs->setCurrentIndex(page);fits(qobject_cast<QScrollArea*>(tabs->currentWidget()));}QVERIFY(window.findChild<QComboBox*>("defaultCaptureSharing")->font().pixelSize()>=pixels);
  window.navigate("destinations");auto* provider=window.findChild<QComboBox*>("storageProvider");for(const auto& name:QStringList{"s3","proton-drive","backblaze-b2"}){provider->setCurrentIndex(provider->findData(name));window.findChild<QToolButton*>("advancedSettings")->setChecked(true);fits(window.findChild<QScrollArea*>("destinationScroll"));}
 }
 void formStackingUsesMeasuredFieldsAndMargins(){
  QScrollArea scroll;auto* page=new QWidget;scroll.setWidget(page);scroll.setWidgetResizable(true);auto* outer=new QVBoxLayout(page);outer->setContentsMargins(20,0,20,0);QList<QFormLayout*> forms;
  for(int i=0;i<2;++i){auto* group=new QWidget;auto* layout=new QVBoxLayout(group);layout->setContentsMargins(8,0,8,0);auto* form=new QFormLayout;form->setContentsMargins(4,0,4,0);form->setHorizontalSpacing(12);auto* field=new QLineEdit;field->setMinimumWidth(i?220:80);form->addRow(new QLabel("Value"),field);layout->addLayout(form);outer->addWidget(group);forms.append(form);}
  Graphite::alignForms(page);scroll.resize(520,300);scroll.show();PUTGD_TRY_COMPARE(forms[0]->rowWrapPolicy(),QFormLayout::DontWrapRows);QCOMPARE(forms[1]->rowWrapPolicy(),QFormLayout::DontWrapRows);
  scroll.resize(300,300);PUTGD_TRY_COMPARE(forms[0]->rowWrapPolicy(),QFormLayout::WrapAllRows);QCOMPARE(forms[1]->rowWrapPolicy(),QFormLayout::WrapAllRows);
  scroll.resize(520,300);PUTGD_TRY_COMPARE(forms[0]->rowWrapPolicy(),QFormLayout::DontWrapRows);QCOMPARE(forms[1]->rowWrapPolicy(),QFormLayout::DontWrapRows);
 }
 void storageServiceMenuTracksDestinationAndProvider(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=threeDestinations();auto profiles=doc["profiles"].toArray();auto proton=profiles[1].toObject();proton["provider"]="proton-drive";proton["settings"]=ProtonProvider::defaults();profiles[1]=proton;doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();auto* group=window.findChild<QActionGroup*>("storageServices");QVERIFY(group&&group->isExclusive());QVERIFY(group->checkedAction());QCOMPARE(group->checkedAction()->data().toString(),QString("s3"));window.findChild<QListWidget*>("destinationList")->setCurrentRow(1);QCOMPARE(group->checkedAction()->data().toString(),QString("proton-drive"));
  for(auto* action:group->actions())if(action->data()=="backblaze-b2")action->trigger();QCOMPARE(window.findChild<QComboBox*>("storageProvider")->currentData().toString(),QString("backblaze-b2"));QCOMPARE(group->checkedAction()->data().toString(),QString("backblaze-b2"));
 }
 void configEditorFailureOffersSelectablePathAndCopy(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(Settings::defaults(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);window.show();window.findChild<QToolButton*>("desktopSupportDetailsToggle")->setChecked(true);QVERIFY(QFile::remove(service.path()));window.findChild<QPushButton*>("openConfigEditor")->click();auto* failure=window.findChild<QLabel*>("configEditorFailure");auto* copy=window.findChild<QPushButton*>("copyConfigPath");PUTGD_TRY_VERIFY(failure->isVisible());QVERIFY(copy->isVisible());auto* path=window.findChild<QLabel*>("configPath");QVERIFY(path->textInteractionFlags()&Qt::TextSelectableByKeyboard);copy->click();QCOMPARE(qApp->clipboard()->text(),service.path());
 }
 void formsUseTheirValueTypesAndTargets(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::developmentTemplate(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);
  MainWindow window(fixture.session);window.resize(1060,680);window.navigate("settings");auto* tabs=window.findChild<QTabWidget*>("settingsTabs");tabs->setCurrentIndex(3);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  // Editable filename templates use the stacked technical field role.
  for(const auto* key:{"fileTemplate","captureTemplate","imageTemplate","textTemplate"}){auto* field=window.findChild<QLineEdit*>(key);QCOMPARE(field->font().family(),Graphite::monoFamily());QCOMPARE(field->font().pixelSize(),Graphite::scaled(Graphite::Type::technical));}
  // The ACL choice says what the upload requests; stored values are unchanged.
  auto* acl=window.findChild<QComboBox*>("s3AclMode");QCOMPARE(acl->itemText(acl->findData("omit")),QString("Use bucket policy"));QCOMPARE(acl->itemText(acl->findData("public-read")),QString("Request public read"));QCOMPARE(acl->accessibleName(),QString("Upload access request"));
  // Help keeps its 14 px glyph; radio help follows the title line height.
  int helps=0;for(auto* help:window.findChildren<QToolButton*>())if(help->property("settingHelpFor").isValid()){++helps;QCOMPARE(help->size(),QSize(Graphite::scaled(24),Graphite::scaled(window.findChild<QRadioButton*>(help->property("settingHelpFor").toString())?Graphite::Type::bodyLine:24)));QCOMPARE(help->iconSize(),QSize(Graphite::scaled(14),Graphite::scaled(14)));QVERIFY(help->text().isEmpty());QVERIFY(!help->accessibleDescription().isEmpty());}QVERIFY(helps>=3);
  // Numeric inputs stay compact; choice controls share the family width. Only changed values show Reset.
  window.findChild<QToolButton*>("resourceLimitsToggle")->setChecked(true);putgd::test::settle(20);
  QSet<int> widths;for(const auto* key:{"concurrentUploads","maxFiles","imageBytes","pendingImageBytes","recentActivityLimit"}){auto* edit=window.findChild<QLineEdit*>(key);QWidget* box=qobject_cast<QComboBox*>(edit->parentWidget())?edit->parentWidget():edit;widths.insert(box->width());}
  QCOMPARE(widths.size(),1);for(const auto* name:{"failedArtifactDaysUnit","historyRetentionDaysUnit"})QCOMPARE(window.findChild<QLabel*>(name)->property("visualRole").toString(),QString("unitSuffix"));
  QPushButton* reset=nullptr;for(auto* button:window.findChildren<QPushButton*>())if(button->accessibleName()=="Reset Uploads at once to default")reset=button;QVERIFY(reset&&!reset->isVisible());
  for(auto* label:tabs->widget(3)->findChildren<QLabel*>())QVERIFY2(!(label->isVisible()&&label->text()=="Default"),"No plain Default labels beside values");
  window.findChild<QLineEdit*>("concurrentUploads")->setText("5");QVERIFY(reset->isVisible());
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
  send(QEvent::KeyPress,Qt::Key_1,Qt::NoModifier,10);QCOMPARE(input->placeholderText(),QString("Press shortcut…"));
  send(QEvent::KeyRelease,Qt::Key_1,Qt::NoModifier,10);QCOMPARE(input->text(),QString("Shift+!"));QCOMPARE(input->placeholderText(),QString("Not set"));
  record->click();send(QEvent::KeyPress,Qt::Key_1,Qt::KeypadModifier,87);send(QEvent::KeyRelease,Qt::Key_1,Qt::KeypadModifier,87);
  QCOMPARE(input->text(),QKeySequence(QKeyCombination(Qt::KeypadModifier,Qt::Key_1)).toString(QKeySequence::PortableText));QCOMPARE(input->placeholderText(),QString("Not set"));
  for(const auto modifierFirst:{false,true}){
   record->click();send(QEvent::KeyPress,Qt::Key_Control,Qt::ControlModifier,37);send(QEvent::KeyPress,Qt::Key_2,Qt::ControlModifier|Qt::KeypadModifier,88);
   if(modifierFirst){send(QEvent::KeyRelease,Qt::Key_Control,Qt::NoModifier,37);send(QEvent::KeyRelease,Qt::Key_2,Qt::KeypadModifier,88);}
   else {send(QEvent::KeyRelease,Qt::Key_2,Qt::ControlModifier|Qt::KeypadModifier,88);send(QEvent::KeyRelease,Qt::Key_Control,Qt::NoModifier,37);}
   QCOMPARE(input->text(),QKeySequence(QKeyCombination(Qt::ControlModifier|Qt::KeypadModifier,Qt::Key_2)).toString(QKeySequence::PortableText));QCOMPARE(input->placeholderText(),QString("Not set"));
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
  QTest::keyClick(input,Qt::Key_Escape);QCOMPARE(transitions,QList<bool>({true,false}));ready({});QCOMPARE(input->placeholderText(),QString("Not set"));
  record->click();ready({"shortcut_unavailable","Synthetic suspension failure",{}});QCOMPARE(transitions,QList<bool>({true,false,true,false}));QCOMPARE(input->text(),old);
  record->click();ready({});QTest::keyPress(input,Qt::Key_Control);QTest::mouseClick(record,Qt::LeftButton);QCOMPARE(input->placeholderText(),QString("Not set"));QCOMPARE(transitions.last(),false);
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
  record->click();QTest::keyClick(input,Qt::Key_Escape);restored({"shortcut_restore_failed","Synthetic restore failure",{}});QVERIFY(status->text().contains("Couldn't restore shortcuts"));QCOMPARE(input->text(),QString("Ctrl+1"));
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
  PUTGD_TRY_COMPARE(channel->currentData().toString(),expected);QCOMPARE(channel->isEnabled(),!managed);QCOMPARE(Graphite::isLocked(channel),managed);
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
  const QUrl docs(update["action"].toObject()["otherChannel"].toObject()["docs"].toString());QList<QUrl> opened;host::setTestOpener([&](const QUrl& url){opened.append(url);});auto restoreOpener=qScopeGuard([]{host::setTestOpener({});});window.findChild<QPushButton*>("otherChannelDocs")->click();QCOMPARE(opened,QList<QUrl>{docs});
  auto* browser=static_cast<Graphite::BrowserFeedback*>(window.findChild<QWidget*>("channelBrowserFeedback"));QVERIFY(browser);browser->opening(docs)({{"opened",false}});QVERIFY(browser->isVisibleTo(settings));browser->findChild<QPushButton*>()->click();QCOMPARE(qApp->clipboard()->text(),docs.toString(QUrl::FullyEncoded));browser->clear();

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
  QVERIFY(line->text().startsWith("Up to date: 0.1.1 is the newest stable version."));QVERIFY(!install->isVisibleTo(settings));
 }
 void unavailableCaptureActionsOpenOutputRecovery(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));auto doc=Settings::defaults();auto archive=doc["archive"].toObject();archive["keepUploads"]=false;doc["archive"]=archive;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.show();QSignalSpy invoked(&window,&MainWindow::actionRequested);QSignalSpy edited(&window,&MainWindow::editRequested);auto* pages=window.findChild<QTabBar*>("mainNavigation");
  for(auto* button:window.findChildren<QToolButton*>("captureActionButton")){QVERIFY(button->isEnabled());QVERIFY(button->focusPolicy()&Qt::TabFocus);QVERIFY(!button->property("captureBadge").toBool());QVERIFY(button->accessibleDescription().contains("New captures have no output."));QVERIFY(button->toolTip().contains("New captures have no output."));for(int activation=0;activation<4;++activation){window.navigate("history");button->setFocus();PUTGD_TRY_VERIFY(button->hasFocus());if(activation==0)QTest::mouseClick(button,Qt::LeftButton,Qt::NoModifier,QPoint(8,button->height()/2));else if(activation<3)QTest::keyClick(button,activation==1?Qt::Key_Space:Qt::Key_Return);else button->click();QCOMPARE(pages->currentIndex(),2);QCOMPARE(window.findChild<QTabWidget*>("settingsTabs")->currentIndex(),1);PUTGD_TRY_VERIFY(window.findChild<QComboBox*>("defaultCaptureDestination")->hasFocus());QVERIFY(window.findChild<QLabel*>("actionRecoveryError")->text().contains("New captures have no output."));}}
  QCOMPARE(invoked.size(),0);QCOMPARE(edited.size(),0);auto* menu=window.findChild<QMenu*>("editSourceMenu");QVERIFY(menu);menu->actions().first()->trigger();QCOMPARE(edited.size(),1);QCOMPARE(edited.first().first().toString(),QString("clipboard"));
  bool opened=false;QTimer::singleShot(50,menu,[&]{opened=menu->isVisible();menu->hide();});QToolButton* edit=nullptr;for(auto* button:window.findChildren<QToolButton*>("captureActionButton"))if(button->property("captureAction")=="edit")edit=button;QVERIFY(edit);QTest::mouseClick(edit,Qt::LeftButton,Qt::NoModifier,QPoint(edit->width()-8,edit->height()/2));QVERIFY(opened);QCOMPARE(invoked.size(),0);
 }
 void folderAndEndpointDisclosuresPreserveConfiguredValues(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=threeDestinations();QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.show();auto* toggle=window.findChild<QToolButton*>("categoryFoldersToggle");auto* folders=window.findChild<QWidget*>("categoryFolders");QVERIFY(toggle&&folders);QVERIFY(!toggle->isChecked());QVERIFY(folders->isHidden());toggle->click();QVERIFY(!folders->isHidden());auto archive=doc["archive"].toObject();auto overrides=archive["folders"].toObject();overrides["images"]=dir.filePath("images");archive["folders"]=overrides;doc["archive"]=archive;QVERIFY(service.save(doc,service.revision()));PUTGD_TRY_COMPARE(window.findChild<QLineEdit*>("folderImages")->text(),dir.filePath("images"));QVERIFY(toggle->isChecked());window.navigate("destinations");auto* advanced=window.findChild<QToolButton*>("advancedSettings");auto* endpoint=window.findChild<QLabel*>("s3EndpointValue");QVERIFY(!endpoint->isVisibleTo(&window));advanced->setChecked(true);QVERIFY(endpoint->isVisibleTo(&window));auto* mode=window.findChild<QComboBox*>("credentialMode");auto* store=window.findChild<QPushButton*>("storeCredentials");mode->setCurrentIndex(mode->findData("session"));QCOMPARE(store->text(),QString("Use for this session"));mode->setCurrentIndex(mode->findData("keychain"));QCOMPARE(store->text(),QString("Save to keychain"));
 }
 void destinationHeaderAndTestFollowProviderContext(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.resize(1440,1000);window.show();auto* title=window.findChild<QLineEdit*>("destinationLabel");auto* state=window.findChild<QLabel*>("destinationProviderState");auto* description=window.findChild<QLabel*>("destinationProviderDescription");auto* header=window.findChild<QWidget*>("destinationHeader");QVERIFY(!window.findChild<QLabel*>("destinationProviderGlyph"));PUTGD_TRY_COMPARE(title->mapTo(header,QPoint()).x(),description->mapTo(header,QPoint()).x());QVERIFY(state->mapTo(header,QPoint()).y()<description->mapTo(header,QPoint()).y());QCOMPARE(state->font().pixelSize(),Graphite::scaled(13));const int rail=window.findChild<QWidget*>("destinationRail")->width();title->setText(QString(200,'W'));PUTGD_COMPARE_STEADY(window.findChild<QWidget*>("destinationRail")->width(),rail,50);QVERIFY(title->geometry().right()<state->geometry().left());
  auto* provider=window.findChild<QComboBox*>("storageProvider");auto* test=window.findChild<QPushButton*>("testDestination");for(const auto& name:QStringList{"proton-drive","s3","proton-drive","backblaze-b2"}){provider->setCurrentIndex(provider->findData(name));putgd::test::settle(20);auto* peer=name=="proton-drive"?static_cast<QWidget*>(window.findChild<QLabel*>("protonAccountStatus")):static_cast<QWidget*>(window.findChild<QLineEdit*>("credentialAccessKey"));auto* card=peer->parentWidget();while(card&&!card->property("preferenceCard").toBool())card=card->parentWidget();QVERIFY(card&&card->isAncestorOf(test));}
 }
 void destinationTestingStaysInTheScrollingFormDuringRecovery(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.resize(1060,680);window.show();putgd::test::settle(40);
  auto* scroll=window.findChild<QScrollArea*>("destinationScroll");auto* test=window.findChild<QPushButton*>("testDestination");auto* section=window.findChild<QWidget*>("destinationTestSection");auto* description=window.findChild<QLabel*>("destinationProviderDescription");auto* provider=window.findChild<QComboBox*>("storageProvider");QVERIFY(description->isVisible());QVERIFY(scroll->widget()->isAncestorOf(test));
  for(const auto& name:QStringList{"s3","backblaze-b2","proton-drive"}){provider->setCurrentIndex(provider->findData(name));putgd::test::settle(20);QVERIFY(description->isVisible());QVERIFY(description->text().contains(name=="s3"?"Amazon S3":name=="backblaze-b2"?"Backblaze B2":"Proton Drive"));}
  test->setEnabled(false);QMetaObject::invokeMethod(window.settings(),"viewStateChanged");PUTGD_TRY_VERIFY(!section->property("pinned").toBool());QVERIFY(scroll->widget()->isAncestorOf(test));QVERIFY(!window.findChild<QWidget*>("destinationStatusRow")->isAncestorOf(test));test->setEnabled(true);
 }
 void compactSettingsKeepSharedInsetsWithoutAConflict(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1060,680);window.navigate("settings");window.show();auto* tabs=window.findChild<QTabWidget*>("settingsTabs");auto* page=qobject_cast<QScrollArea*>(tabs->currentWidget());auto* heading=window.findChild<QLabel*>("settingsPageHeadingGeneral");PUTGD_TRY_VERIFY(heading->property("compactHeading").toBool());QCOMPARE(page->widget()->layout()->contentsMargins(),QMargins(Graphite::space(Graphite::Space::xxl),qMin(Graphite::pageHeaderTop(),Graphite::space(Graphite::Space::l)),Graphite::space(Graphite::Space::xxl),Graphite::space(Graphite::Space::xl)));QCOMPARE(page->widget()->layout()->spacing(),Graphite::space(Graphite::Space::xxl));QVERIFY(window.findChild<QLabel*>("settingsPageDescriptionGeneral")->isHidden());for(auto* card:page->widget()->findChildren<QWidget*>())if(card->property("visualRole")=="card")QCOMPARE(card->layout()->contentsMargins(),QMargins(0,Graphite::space(Graphite::Space::xl),0,0));
 }
 void destinationConflictFooterHasOnlyReloadRecovery(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.resize(1060,680);window.show();auto state=window.settings()->saveViewState();state["conflict"]=true;QVERIFY(window.settings()->restoreViewState(state));putgd::test::settle(30);auto* footer=window.findChild<QWidget*>("destinationStatusRow");auto* test=window.findChild<QPushButton*>("testDestination");auto* scope=window.findChild<QLabel*>("destinationTestScope");auto* scroll=window.findChild<QScrollArea*>("destinationScroll");QVERIFY(scroll->widget()->isAncestorOf(test));QVERIFY(scroll->widget()->isAncestorOf(scope));QVERIFY(!footer->isAncestorOf(test));QVERIFY(window.findChild<QPushButton*>("reloadDestinationConflict")->isVisible());for(auto* button:footer->findChildren<QPushButton*>())if(button->isVisible())QCOMPARE(button->objectName(),QString("reloadDestinationConflict"));scroll->ensureWidgetVisible(test,0,20);PUTGD_TRY_VERIFY(scope->height()>=scope->heightForWidth(scope->width()));
 }
 void captureExceptionFieldsShareTheDefaultValueColumn(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1600,900);window.showCaptureSettings();window.show();for(const auto& action:QStringList{"region","edit"})window.findChild<QAction*>("add"+action.left(1).toUpper()+action.mid(1)+"Exception")->trigger();auto* base=window.findChild<QComboBox*>("defaultCaptureDestination");for(const auto size:{QSize(1600,900),QSize(1060,680)}){window.resize(size);putgd::test::settle(50);for(const auto& name:QStringList{"Region","Edit","Clipboard"}){auto* field=window.findChild<QComboBox*>("profile"+name);QVERIFY(field->isVisible());QCOMPARE(field->mapTo(&window,QPoint()).x(),base->mapTo(&window,QPoint()).x());}}
 }
 void shortcutHelpIsOneDisclosure(){
  ShortcutEditor editor;editor.setActions(Settings::defaults()["actions"].toObject());editor.show();auto* toggle=editor.findChild<QToolButton*>("shortcutHelpToggle");auto* help=editor.findChild<QLabel*>("shortcutHelp");QVERIFY(toggle&&help);QVERIFY(!help->isVisible());toggle->click();QVERIFY(help->isVisible());toggle->click();QVERIFY(!help->isVisible());QCOMPARE(editor.findChildren<QToolButton*>().size(),2);
 }
 void signedOutProtonDefersFoldersUntilConnected(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto proton=profiles.first().toObject();proton["provider"]="proton-drive";proton["settings"]=ProtonProvider::defaults();profiles[0]=proton;doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();auto* button=window.findChild<QPushButton*>("connectProtonAccount");auto* prerequisite=window.findChild<QLabel*>("protonFolderPrerequisite");auto* folders=window.findChild<QWidget*>("protonFolderSection");QCOMPARE(button->text(),QString("Connect Proton Drive…"));QVERIFY(button->isVisible());QVERIFY(prerequisite->isVisible());QVERIFY(!folders->isVisible());QVERIFY(button->mapTo(&window,QPoint()).y()<prerequisite->mapTo(&window,QPoint()).y());
  auto settings=proton["settings"].toObject();settings["accountRootUid"]="synthetic-account";settings["folderUid"]="synthetic-folder";proton["settings"]=settings;profiles[0]=proton;doc["profiles"]=profiles;QVERIFY(service.save(doc,service.revision()));PUTGD_TRY_VERIFY(folders->isVisible());QVERIFY(!prerequisite->isVisible());auto* list=window.findChild<QListWidget*>("destinationList");QVERIFY(list->item(0)->text().contains("Configured"));QVERIFY(!list->item(0)->text().contains("checked"));
  JsonReply complete;fixture.setup.setProviderSetup([&](const QString& kind,const Destination&,const QJsonObject&,JsonReply reply,auto){if(kind=="testDestination")complete=reply;else reply({},{});});window.findChild<QPushButton*>("testDestination")->click();PUTGD_TRY_VERIFY(bool(complete));complete({},{{"testScope","account-folder"}});PUTGD_TRY_VERIFY(list->item(0)->text().contains("Account and folder checked"));QVERIFY(window.findChild<QLabel*>("protonAccountStatus")->text().contains("sharing aren't tested"));
 }
 void exceptionMenuAddsOnlyOverriddenRowsAndRemoveRestoresInheritance(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=Settings::defaults();auto archive=doc["archive"].toObject();archive["keepUploads"]=true;doc["archive"]=archive;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.showCaptureSettings();window.show();auto* toggle=window.findChild<QToolButton*>("captureExceptionsToggle");toggle->setChecked(true);auto* add=window.findChild<QToolButton*>("addCaptureException");QVERIFY(add&&add->isVisible());PUTGD_TRY_VERIFY(add->width()>=add->fontMetrics().horizontalAdvance(add->text())+add->iconSize().width()+2*Graphite::space(Graphite::Space::l)+Graphite::space(Graphite::Space::s));
  for(const auto& action:Settings::presentationOrder()){const auto name=action.left(1).toUpper()+action.mid(1);auto* row=window.findChild<QWidget*>("captureException"+name);auto* entry=window.findChild<QAction*>("add"+name+"Exception");QVERIFY(row&&entry);QVERIFY(row->isHidden());QVERIFY(entry->isVisible());entry->trigger();QVERIFY(!row->isHidden());QVERIFY(!entry->isVisible());QVERIFY(!window.findChild<QCheckBox*>("useDefault"+name)->isChecked());window.findChild<QPushButton*>("remove"+name+"Exception")->click();QVERIFY(row->isHidden());QVERIFY(entry->isVisible());QVERIFY(window.findChild<QCheckBox*>("useDefault"+name)->isChecked());}
  for(const auto& action:Settings::presentationOrder())window.findChild<QAction*>("add"+action.left(1).toUpper()+action.mid(1)+"Exception")->trigger();QVERIFY(!add->isEnabled());QCOMPARE(window.findChild<QLabel*>("captureExceptionsSummary")->text(),QString("6 exceptions"));window.findChild<QPushButton*>("removeRegionException")->click();QVERIFY(add->isEnabled());QCOMPARE(window.findChild<QLabel*>("captureExceptionsSummary")->text(),QString("5 exceptions"));
 }
 void recordingInheritanceRemainsVisibleWhenRegionChangesItsOutput(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=threeDestinations();auto actions=doc["actions"].toObject();for(const auto& name:Settings::presentationOrder()){auto action=actions[name].toObject();action["destination"]="default";action["profileId"]=QJsonValue::Null;actions[name]=action;}actions["record"]=QJsonObject{{"shortcut",actions["record"].toObject()["shortcut"]}};doc["actions"]=actions;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.showCaptureSettings();window.show();auto* line=window.findChild<QLabel*>("recordingOutputInheritance");QVERIFY(line->isHidden());window.findChild<QAction*>("addRegionException")->trigger();auto* region=window.findChild<QComboBox*>("profileRegion");region->setCurrentIndex(region->findData("synthetic-alternate"));QVERIFY(line->isVisible());window.findChild<QToolButton*>("captureExceptionsToggle")->setChecked(false);QVERIFY(line->isVisible());window.findChild<QAction*>("addRecordException")->trigger();QVERIFY(line->isHidden());window.findChild<QPushButton*>("removeRecordException")->click();QVERIFY(line->isVisible());window.findChild<QPushButton*>("removeRegionException")->click();QVERIFY(line->isHidden());
 }
 void compactCommandsKeepCoreTaskLabels_data(){QTest::addColumn<bool>("mixed");QTest::newRow("default")<<false;QTest::newRow("mixed")<<true;}
 void compactCommandsKeepCoreTaskLabels(){
  QFETCH(bool,mixed);Fonts::load();VisualStyle::apply(*qApp);
  const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(21);qApp->setFont(font);QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});if(mixed){auto doc=threeDestinations();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();profile["provider"]="proton-drive";profile["settings"]=ProtonProvider::defaults();profiles[0]=profile;doc["profiles"]=profiles;auto archive=doc["archive"].toObject();archive["keepUploads"]=false;doc["archive"]=archive;doc["captureDefaults"]=QJsonObject{{"destination","local"},{"profileId",QJsonValue::Null},{"publicationMode","default"}};auto actions=doc["actions"].toObject();for(const auto& name:Settings::presentationOrder()){auto action=actions[name].toObject();if(!action.contains("shortcut"))action["shortcut"]="";action["destination"]=name=="region"||name=="record"?"upload":"default";action["profileId"]=name=="region"||name=="record"?profile["id"]:QJsonValue(QJsonValue::Null);action["publicationMode"]=name=="region"?"private":"default";actions[name]=action;}doc["actions"]=actions;QVERIFY(service.save(doc,{}));}CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1060,680);window.show();putgd::test::settle(30);auto* grid=qobject_cast<QGridLayout*>(window.findChild<QWidget*>("commandBand")->layout());QVERIFY(grid);QCOMPARE(grid->rowCount(),1);QCOMPARE(window.width(),1060);for(auto* button:window.findChildren<QToolButton*>("captureActionButton")){if(button->property("captureAction")=="edit")QVERIFY(button->text().startsWith("Capture region && edit"));if(QStringList{"region","window","desktop","edit"}.contains(button->property("captureAction").toString()))QCOMPARE(button->toolButtonStyle(),Qt::ToolButtonTextBesideIcon);else if(button->toolButtonStyle()==Qt::ToolButtonIconOnly)QVERIFY(!button->toolTip().isEmpty()&&!button->accessibleName().isEmpty());QVERIFY(!button->text().contains(" · "));if(button->property("captureAction")=="window")QVERIFY(button->text()=="Window"||button->text()=="Capture window");if(button->property("captureAction")=="desktop")QVERIFY(button->text()=="Desktop"||button->text()=="Capture desktop");QVERIFY(window.rect().contains(QRect(button->mapTo(&window,QPoint()),button->size())));QVERIFY(button->width()>=button->sizeHint().width());}
 }
 void resourcesDiscloseModifiedAndInvalidLimits(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(Settings::defaults(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);window.show();auto* toggle=window.findChild<QToolButton*>("resourceLimitsToggle");auto* limits=window.findChild<QWidget*>("resourceLimits");QVERIFY(toggle&&limits);QVERIFY(!toggle->isChecked());QVERIFY(limits->isHidden());QVERIFY(window.findChild<QLineEdit*>("historyRetentionDays")->isVisible());QVERIFY(window.findChild<QPushButton*>("createDiagnosticsPackage")->isVisible());
  toggle->setChecked(true);auto state=window.settings()->saveViewState();QVERIFY(window.settings()->restoreViewState(state));QVERIFY(toggle->isChecked());toggle->setChecked(false);auto* count=window.findChild<QLineEdit*>("concurrentUploads");count->setText("5");QVERIFY(!toggle->isChecked());count->setText("invalid");QMetaObject::invokeMethod(count,"editingFinished");PUTGD_TRY_VERIFY(toggle->isChecked());
 }
 void bucketHintAndCredentialGuidanceFollowTheEditors(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");auto* bucket=window.findChild<QLineEdit*>("s3Bucket");auto* folder=window.findChild<QLineEdit*>("s3ObjectPrefix");auto* hint=window.findChild<QLabel*>("s3BucketHint");QVERIFY(bucket&&folder&&hint);bucket->setText("synthetic-bucket");folder->setText("captures/");QCOMPARE(hint->text(),QString("Uploads go to s3://synthetic-bucket/captures/"));bucket->clear();QVERIFY(hint->text().startsWith("Required"));
  auto* mode=window.findChild<QComboBox*>("credentialMode");auto* button=window.findChild<QPushButton*>("storeCredentials");for(const auto& value:{"keychain","session"}){mode->setCurrentIndex(mode->findData(value));QVERIFY(window.findChild<QLineEdit*>("credentialSecretKey")->toolTip().contains(button->text()));QVERIFY(mode->accessibleDescription().contains(button->text()));auto* access=window.findChild<QLineEdit*>("credentialAccessKey");access->setText("synthetic-access");QVERIFY(window.settings()->autosaveText().contains(button->text()));access->clear();}
 }
 void settingLabelsUseBuddiesAndOnlyUsefulHelpButtons(){
  QWidget host;QLineEdit field(&host);field.setObjectName("syntheticValue");auto* label=VisualStyle::settingLabel("Value","A longer explanation",&field,&host);QVERIFY(label->findChild<QLabel*>());QCOMPARE(label->findChild<QLabel*>()->buddy(),&field);QCOMPARE(label->findChildren<QToolButton*>().size(),1);auto* help=label->findChild<QToolButton*>();QVERIFY(help->text().isEmpty());QCOMPARE(help->size(),QSize(Graphite::scaled(24),Graphite::scaled(24)));QCOMPARE(label->findChild<QLabel*>()->focusPolicy(),Qt::NoFocus);
  auto* plain=VisualStyle::settingLabel("Value","Value",&field,&host);QVERIFY(plain->findChildren<QToolButton*>().isEmpty());auto* empty=VisualStyle::settingLabel("Value",{},&field,&host);QVERIFY(empty->findChildren<QToolButton*>().isEmpty());
 }
 void settingsKeepHelpForDetailsBeyondLabelsAndHints(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.settings();
  for(const auto& key:QStringList{"launchAtLogin","notifications","keepUploadCopies","archiveRoot","folderImages","folderVideos","folderText","folderOther","s3ObjectPrefix","b2LinkDays","protonFolder","storedCredentialHint","updateCommand","s3Service","s3Bucket","s3RegionChoices","credentialAccessKey","credentialSecretKey","credentialSessionToken","credentialMode"}){
   auto* control=window.findChild<QWidget*>(key);QVERIFY2(control,qPrintable(key));QVERIFY2(!window.findChild<QToolButton*>(key+"Help"),qPrintable(key));QLabel* caption=nullptr;for(auto* label:window.findChildren<QLabel*>())if(label->buddy()==control)caption=label;if(qobject_cast<QCheckBox*>(control)){QVERIFY(caption);QCOMPARE(caption->focusPolicy(),Qt::NoFocus);}else{QVERIFY2(caption,qPrintable(key));QCOMPARE(caption->focusPolicy(),Qt::NoFocus);}
  }
  for(const auto& key:QStringList{"archiveRoot","s3ObjectPrefix","b2LinkDays","protonFolder"}){
   auto* hint=window.findChild<QLabel*>(key+"Hint");auto* control=window.findChild<QWidget*>(key);QVERIFY2(hint&&!hint->text().isEmpty(),qPrintable(key));QVERIFY(control->accessibleDescription().endsWith(hint->text()));
  }
  QVERIFY(!window.findChild<QToolButton*>("appearanceThemeHelp"));
  for(const auto& key:QStringList{"unlockCredentialsAtStartup","uploadClipboardBehavior","namingMode","collisionPolicy","captureTemplate","s3AclMode","protonExpiration"}){
   auto* help=window.findChild<QToolButton*>(key+"Help");QVERIFY2(help,qPrintable(key));QCOMPARE(help->focusPolicy(),Qt::StrongFocus);QVERIFY(!help->accessibleDescription().isEmpty());
  }
 }
 void destinationStatusUsesTheColumnInsetAndFullWidth(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();auto* row=window.findChild<QWidget*>("destinationStatusRow");QVERIFY(row);QCOMPARE(row->layout()->contentsMargins().left(),Graphite::space(Graphite::Space::xxl));QCOMPARE(row->maximumWidth(),QWIDGETSIZE_MAX);QCOMPARE(window.findChild<QWidget*>("settingsStatusRow")->maximumWidth(),QWIDGETSIZE_MAX);
 }
 void shortcutFieldsMeasureTheirContentsAndKeepFamilyAlignment_data(){QTest::addColumn<int>("pixels");QTest::newRow("normal")<<14;QTest::newRow("large")<<21;}
 void shortcutFieldsMeasureTheirContentsAndKeepFamilyAlignment(){
  QFETCH(int,pixels);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(pixels);qApp->setFont(font);QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.resize(1440,1100);window.show();auto* tabs=window.findChild<QTabWidget*>("settingsTabs");auto* choice=window.findChild<QComboBox*>("appearanceTheme");PUTGD_TRY_VERIFY(choice->width()>=Graphite::scaled(240));putgd::test::settle(50);const int width=choice->width();tabs->setCurrentIndex(2);auto* record=window.findChild<QPushButton*>("recordRegionShortcut");PUTGD_TRY_VERIFY(record->width()<width);const int originalWidth=record->width();window.findChild<QLineEdit*>("shortcutRegion")->setText("Ctrl+Alt+Shift+Meta+Print");PUTGD_TRY_VERIFY(record->width()>originalWidth);for(const auto& action:Settings::presentationOrder())QCOMPARE(window.findChild<QPushButton*>("record"+action.left(1).toUpper()+action.mid(1)+"Shortcut")->width(),record->width());tabs->setCurrentIndex(3);auto* retention=window.findChild<QComboBox*>("historyRetentionChoice");PUTGD_TRY_COMPARE(retention->width(),width);
 }
 void captureExceptionSummaryPrecedesTheChevron_data(){QTest::addColumn<int>("pixels");QTest::newRow("normal")<<14;QTest::newRow("large")<<21;}
 void captureExceptionSummaryPrecedesTheChevron(){
  Fonts::load();VisualStyle::apply(*qApp);QFETCH(int,pixels);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(pixels);qApp->setFont(font);QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.showCaptureSettings();window.resize(1060,680);window.show();auto* toggle=window.findChild<QToolButton*>("captureExceptionsToggle");auto* summary=window.findChild<QLabel*>("captureExceptionsSummary");QVERIFY(summary);auto* row=summary->parentWidget();auto* heading=static_cast<QLabel*>(row->property("preferenceLabel").value<QObject*>());QVERIFY(heading);PUTGD_TRY_VERIFY(summary->width()>0);QCOMPARE(summary->palette().color(QPalette::WindowText),Theme::current().textSecondary);QCOMPARE(summary->alignment(),Qt::AlignRight|Qt::AlignVCenter);QCOMPARE(toggle->accessibleName(),heading->text());
  const auto rect=[row](QWidget* widget){return QRect(widget->mapTo(row,QPoint()),widget->size());};QVERIFY(rect(heading).right()<rect(summary).left());QVERIFY(rect(summary).right()<rect(toggle).left());QCOMPARE(rect(summary).center().y(),rect(toggle).center().y());QCOMPARE(toggle->accessibleDescription(),QString("None"));QTest::mouseClick(summary,Qt::LeftButton);QVERIFY(toggle->isChecked());window.findChild<QAction*>("addClipboardException")->trigger();QCOMPARE(summary->text(),QString("1 exception"));QCOMPARE(toggle->accessibleDescription(),summary->text());window.findChild<QAction*>("addRegionException")->trigger();QCOMPARE(summary->text(),QString("2 exceptions"));window.findChild<QPushButton*>("removeClipboardException")->click();window.findChild<QPushButton*>("removeRegionException")->click();QCOMPARE(summary->text(),QString("None"));
 }
 void emptyExceptionListsDoNotAddASectionGap(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.showCaptureSettings();window.resize(1440,1100);window.show();auto* toggle=window.findChild<QToolButton*>("captureExceptionsToggle");auto* body=window.findChild<QWidget*>("captureExceptions");QCOMPARE(window.findChild<QLabel*>("captureExceptionsSummary")->text(),QString("None"));toggle->setChecked(true);QVERIFY(body->isHidden());window.findChild<QAction*>("addClipboardException")->trigger();QVERIFY(body->isVisible());auto* section=body->parentWidget();QVERIFY(section->property("preferenceSection").toBool());window.findChild<QPushButton*>("removeClipboardException")->click();QVERIFY(body->isHidden());auto* column=window.findChild<QWidget*>("capturePreferenceColumn");auto* layout=qobject_cast<QVBoxLayout*>(column->layout());QVERIFY(layout);const int index=layout->indexOf(section);QVERIFY(index>=0&&index+1<layout->count());QWidget* next=nullptr;for(int n=index+1;n<layout->count()&&!next;++n)next=layout->itemAt(n)->widget();QVERIFY(next);PUTGD_TRY_COMPARE(next->y()-section->geometry().bottom()-1,Graphite::space(Graphite::Space::xxl));
 }
 void desktopSupportUsesADisclosureAndCopiesExactDetails(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(Settings::defaults(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);const QJsonObject details{{"trayAvailable",true},{"shortcuts",QJsonObject{{"available",false}}},{"capabilities",QJsonObject{{"source",QJsonObject{{"adapter","gnome-wayland"},{"capture",QJsonObject{{"region",QJsonObject{{"available",true}}}}}}}}}};fixture.server.setUiDiagnostics([&]{return details;});fixture.session.refreshSnapshot();PUTGD_TRY_COMPARE(fixture.session.diagnostics(),details);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);window.show();auto* settings=window.settings();auto* body=window.findChild<QWidget*>("desktopSupportDetails");QVERIFY(body&&body->isHidden());fixture.server.setUiCommand([settings](const QString&,const QJsonObject& params,JsonReply reply){settings->diagnosticsCommand(params,std::move(reply));});auto command=[&](QJsonObject params){bool done=false;Error result;fixture.client.request("ui.diagnostics",params,settings,[&](Error error,QJsonObject){result=error;done=true;});if(!putgd::test::waitFor([&]{return done;}))return Error{"test_timeout","Diagnostics command did not finish",{}};return result;};QVERIFY(!command({{"action","setDesktopDetails"},{"enabled",true}}));QVERIFY(body->isVisible());QCOMPARE(window.findChild<QLabel*>("desktopSupportRegion")->text(),QString("Available"));QCOMPARE(window.findChild<QLabel*>("desktopSupportShortcuts")->text(),QString("Unavailable"));QCOMPARE(window.findChild<QLabel*>("desktopSupportTray")->text(),QString("Available"));QVERIFY(!command({{"action","copyDetails"}}));QVERIFY(qApp->clipboard()->text().startsWith(QString::fromUtf8(QJsonDocument(details).toJson(QJsonDocument::Indented))));auto state=settings->saveViewState();QVERIFY(!state.contains("stateError"));auto invalid=state;invalid["desktopSupportDetails"]="invalid";QVERIFY(!settings->restoreViewState(invalid));QVERIFY(!command({{"action","setDesktopDetails"},{"enabled",false}}));QVERIFY(body->isHidden());QVERIFY(settings->restoreViewState(state));QVERIFY(body->isVisible());QCOMPARE(command({{"action","setDesktopDetails"}}).code,QString("invalid_request"));QVERIFY(!command({{"action","refreshDesktopSupport"}}));QList<QUrl> opened;host::setTestOpener([&](const QUrl& url){opened.append(url);});auto cleanup=qScopeGuard([]{host::setTestOpener({});});QVERIFY(!command({{"action","showConfigFolder"}}));QCOMPARE(opened,QList<QUrl>{QUrl::fromLocalFile(dir.path())});
 }
 void diagnosticConfigurationStaysInsideDesktopDetails(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(Settings::defaults(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);window.resize(1440,1100);window.show();auto* body=window.findChild<QWidget*>("desktopSupportDetails");auto* path=window.findChild<Graphite::ElidedLabel*>("configPath");QVERIFY(path&&body->isAncestorOf(path));QVERIFY(path->isHidden()||!path->isVisible());window.findChild<QToolButton*>("desktopSupportDetailsToggle")->setChecked(true);QVERIFY(path->isVisible());QCOMPARE(path->text(),service.path());for(auto* key:body->findChildren<QLabel*>())if(key->wordWrap()&&!key->isHidden())PUTGD_TRY_VERIFY2(key->height()>=key->heightForWidth(key->width()),qPrintable(key->text()));auto* label=window.findChild<QLabel*>("configPathLabel");QVERIFY(label&&label->buddy()==path);const auto top=[&](QWidget* widget){return widget->mapTo(body,QPoint()).y();};PUTGD_TRY_VERIFY(qAbs(top(label)-top(path))<=2);PUTGD_TRY_VERIFY(label->mapTo(body,QPoint(label->width(),0)).x()+Graphite::space(Graphite::Space::l)<=path->mapTo(body,QPoint()).x());for(const auto& name:{"showConfigInFolder","openConfigEditor","reloadSettings"}){auto* button=window.findChild<QPushButton*>(name);QVERIFY(button->property("graphiteFlush").toBool());QVERIFY(top(button)>top(path));QVERIFY(button->mapTo(body,QPoint()).x()>=path->mapTo(body,QPoint()).x());}for(const auto& key:QStringList{"accepted","observed"})QVERIFY(window.findChild<QLabel*>(key+"Revision")->isHidden());QVERIFY(body->isAncestorOf(window.findChild<QPushButton*>("copyDesktopDetails")));QVERIFY(body->isAncestorOf(window.findChild<QPushButton*>("showConfigInFolder")));
  const auto accepted=service.revision();{QFile file(service.path());QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));file.write("{invalid-json");}QVERIFY(!service.reload());PUTGD_TRY_VERIFY(fixture.session.metadata()["observedRevision"].toString()!=accepted);PUTGD_TRY_VERIFY(!window.findChild<QLabel*>("observedRevision")->isHidden());QCOMPARE(window.findChild<QLabel*>("acceptedRevision")->accessibleName(),accepted);QCOMPARE(window.findChild<QLabel*>("observedRevision")->accessibleName(),fixture.session.metadata()["observedRevision"].toString());

 }
 void advancedCompactValuesEndInTheSharedColumn(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);window.resize(1440,1100);window.show();auto* field=window.findChild<QLineEdit*>("failedArtifactDays");auto* count=window.findChild<QLineEdit*>("recentActivityLimit");auto* choice=window.findChild<QComboBox*>("historyRetentionChoice");QVERIFY(field&&count&&choice);putgd::test::settle(50);auto* retry=window.findChild<QComboBox*>("failedArtifactChoice");QVERIFY(retry);QCOMPARE(retry->parentWidget()->width(),choice->parentWidget()->width());QCOMPARE(count->width(),Graphite::scaled(120));QCOMPARE(count->alignment(),Qt::AlignRight);QCOMPARE(count->mapTo(&window,QPoint(count->width(),0)).x(),choice->mapTo(&window,QPoint(choice->width(),0)).x());auto* unit=window.findChild<QLabel*>("failedArtifactDaysUnit");QVERIFY(unit&&unit->isHidden());QCOMPARE(retry->mapTo(&window,QPoint(retry->width(),0)).x(),choice->mapTo(&window,QPoint(choice->width(),0)).x());
 }
 void checkboxCaptionsUseTheirAvailableWidthBeforeWrapping(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");window.resize(1440,960);window.show();auto* check=window.findChild<QCheckBox*>("unlockCredentialsAtStartup");QLabel* caption=nullptr;for(auto* label:window.findChildren<QLabel*>())if(label->buddy()==check)caption=label;auto* help=window.findChild<QToolButton*>("unlockCredentialsAtStartupHelp");QVERIFY(caption&&help);PUTGD_TRY_VERIFY(caption->width()>=caption->fontMetrics().horizontalAdvance(caption->text()));QCOMPARE(help->mapTo(caption->parentWidget(),QPoint()).x(),caption->width()+Graphite::space(Graphite::Space::xs));
  auto* holder=caption->parentWidget();holder->setFixedWidth(caption->fontMetrics().horizontalAdvance("Unlock saved credentials")+help->width()+Graphite::space(Graphite::Space::xs));putgd::test::settle(30);QVERIFY(caption->height()>caption->fontMetrics().height());QVERIFY(help->y()<caption->fontMetrics().height());QVERIFY(holder->rect().contains(help->geometry()));
 }
 void advancedOnlyDisclosesRetentionAndDebugHelp(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("settings");auto* page=window.findChild<QTabWidget*>("settingsTabs")->widget(3);int helps=0;for(auto* button:page->findChildren<QToolButton*>())if(!button->property("settingHelpFor").toString().isEmpty()){++helps;QVERIFY(button->objectName()=="historyRetentionChoiceHelp"||button->objectName()=="debugLoggingHelp");}QCOMPARE(helps,2);
  for(const auto& name:{"pendingArtifactBytesChoice","pendingImageBytes","previewCacheBytes","failedArtifactDays","logSummary","uncleanExit"}){auto* control=window.findChild<QWidget*>(name);QVERIFY(control);QVERIFY(!control->accessibleDescription().isEmpty());QVERIFY(!window.findChild<QToolButton*>(QString(name)+"Help"));}
  window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);window.show();auto* help=window.findChild<QToolButton*>("historyRetentionChoiceHelp");help->click();QVERIFY(window.findChild<QLabel*>("historyRetentionChoiceHelpText")->isVisible());help->click();QVERIFY(!window.findChild<QLabel*>("historyRetentionChoiceHelpText")->isVisible());
 }
 void addDestinationUsesAQuietNavigationRow(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();putgd::test::settle(30);auto* add=window.findChild<QPushButton*>("addS3Destination");auto* list=window.findChild<QListWidget*>("destinationList");QVERIFY(add&&list);QCOMPARE(add->text(),QString("Add destination"));QVERIFY(!add->icon().isNull());QCOMPARE(add->iconSize(),QSize(Graphite::scaled(16),Graphite::scaled(16)));QCOMPARE(add->property("visualRole").toString(),QString("quiet"));QCOMPARE(add->height(),Graphite::scaled(36));QVERIFY(add->property("alignLeft").toBool());QCOMPARE(add->mapTo(&window,QPoint()).x(),list->mapTo(&window,QPoint()).x());QCOMPARE(add->width(),list->width());QVERIFY(add->menu());QVERIFY(add->focusPolicy()&Qt::TabFocus);
 }
 void diagnosticsPackageIsAnAlignedSecondaryAction(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1600,1400);window.navigate("settings");window.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);window.show();putgd::test::settle(30);
  auto* package=window.findChild<QPushButton*>("createDiagnosticsPackage");auto* clear=window.findChild<QPushButton*>("clearLogs");QVERIFY(package&&clear);QCOMPARE(package->property("visualRole").toString(),QString("secondary"));QVERIFY(package->width()<package->parentWidget()->width());auto* summary=window.findChild<QLabel*>("logSummary");QVERIFY(summary);QCOMPARE(package->mapTo(&window,QPoint(package->width(),0)).x(),clear->mapTo(&window,QPoint(clear->width(),0)).x());QVERIFY(summary->mapTo(&window,QPoint()).y()>summary->parentWidget()->findChild<QLabel*>()->mapTo(&window,QPoint()).y());QVERIFY(!package->icon().isNull());
 }
 void choiceFamiliesKeepTheStandardWidthWhenContentsChange(){
  QWidget host;QVBoxLayout column(&host);QList<QFormLayout*> forms;QList<QComboBox*> choices;
  for(int i=0;i<3;++i){auto* form=new QFormLayout;VisualStyle::styleForm(form);column.addLayout(form);auto* combo=VisualStyle::choiceBox(&host);combo->addItem("Short");form->addRow("Value",combo);forms<<form;choices<<combo;}
  VisualStyle::alignChoices(&host,{forms[0],forms[1]});VisualStyle::alignChoices(&host,{forms[2]});host.resize(1000,400);host.show();putgd::test::settle(30);for(auto* combo:choices)QCOMPARE(combo->width(),Graphite::scaled(320));
  choices[0]->addItem(QString(120,'W'));PUTGD_TRY_COMPARE(choices[0]->width(),Graphite::scaled(320));QCOMPARE(choices[1]->width(),choices[0]->width());QCOMPARE(choices[2]->width(),Graphite::scaled(320));
 }
 void familyLabelGuttersAreCappedAndFieldsUseTheRemainder_data(){QTest::addColumn<int>("pixels");QTest::newRow("100")<<14;QTest::newRow("125")<<18;QTest::newRow("150")<<21;}
 void familyLabelGuttersAreCappedAndFieldsUseTheRemainder(){
  QFETCH(int,pixels);Fonts::load();VisualStyle::apply(*qApp);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(pixels);qApp->setFont(font);QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1600,1100);window.showCaptureSettings();window.show();
  int familyGutter=-1;const auto check=[&](QComboBox* combo){QVERIFY(combo);auto* value=combo->parentWidget();while(value&&!value->property("preferenceChoiceValue").toBool())value=value->parentWidget();QVERIFY(value);auto* row=value->parentWidget();auto* caption=qobject_cast<QWidget*>(row->property("preferenceLabel").value<QObject*>());QVERIFY(caption);if(row->property("stacked").toBool()){QVERIFY(value->width()>=Graphite::scaled(280)||value->width()==row->width()-2*Graphite::space(Graphite::Space::l));return;}const int gutter=value->mapTo(row,QPoint()).x()-caption->parentWidget()->mapTo(row,QPoint()).x();QVERIFY(gutter<=220+Graphite::scaled(24)+Graphite::space(Graphite::Space::xs)+Graphite::space(Graphite::Space::l));if(familyGutter<0)familyGutter=gutter;QCOMPARE(gutter,familyGutter);QCOMPARE(value->mapTo(row,QPoint(value->width(),0)).x(),row->width()-Graphite::space(Graphite::Space::l));QVERIFY(caption->width()<=220+Graphite::scaled(24)+Graphite::space(Graphite::Space::xs));for(auto* hint:row->findChildren<QLabel*>(Qt::FindDirectChildrenOnly))if(hint->isVisible()&&hint->objectName().endsWith("Hint")){QCOMPARE(hint->width(),row->width()-2*Graphite::space(Graphite::Space::l));QVERIFY(hint->height()>=hint->heightForWidth(hint->width()));}};
  auto* tabs=window.findChild<QTabWidget*>("settingsTabs");for(int page:{0,1,3}){tabs->setCurrentIndex(page);putgd::test::settle(50);for(auto* combo:tabs->currentWidget()->findChildren<QComboBox*>())if(combo->isVisible()&&combo->property("preferenceChoice").toBool())check(combo);}familyGutter=-1;window.navigate("destinations");window.findChild<QToolButton*>("advancedSettings")->setChecked(true);putgd::test::settle(50);for(auto* combo:window.findChild<QScrollArea*>("destinationScroll")->findChildren<QComboBox*>())if(combo->isVisible()&&combo->property("preferenceChoice").toBool())check(combo);
 }
 void formChoicesShareWidthsAndTextFieldsKeepTheirColumn(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1600,1200);window.showCaptureSettings();window.show();
  auto* choices=window.findChild<QComboBox*>("defaultCaptureDestination");QVERIFY(choices);putgd::test::settle(30);PUTGD_TRY_VERIFY(choices->width()>=Graphite::scaled(240));const int width=choices->width();
  for(const auto& name:{"defaultCaptureDestination","defaultCaptureSharing","namingMode","collisionPolicy"}){auto* combo=window.findChild<QComboBox*>(name);QVERIFY(combo);QCOMPARE(combo->width(),width);}
  auto* path=window.findChild<QLineEdit*>("archiveRoot");QVERIFY(path->width()>=Graphite::scaled(280));
  choices->addItem(QString(120,'W'));putgd::test::settle(30);for(const auto& name:{"defaultCaptureDestination","defaultCaptureSharing","namingMode","collisionPolicy"})QCOMPARE(window.findChild<QComboBox*>(name)->width(),width);
  window.navigate("destinations");putgd::test::settle(30);auto* serviceChoice=window.findChild<QComboBox*>("s3Service");auto* region=window.findChild<QComboBox*>("s3RegionChoices");auto* acl=window.findChild<QComboBox*>("s3AclMode");QVERIFY(serviceChoice&&region&&acl);QCOMPARE(serviceChoice->width(),region->width());QCOMPARE(serviceChoice->width(),acl->width());QCOMPARE(window.findChild<QLineEdit*>("s3Bucket")->width(),region->width());QCOMPARE(window.findChild<QLineEdit*>("s3ObjectPrefix")->width(),region->width());
  window.resize(1060,680);putgd::test::settle(30);for(auto* combo:{serviceChoice,region,acl}){const auto bounds=QRect(combo->mapTo(&window,QPoint()),combo->size());QVERIFY(bounds.left()>=0&&bounds.right()<window.width());}QCOMPARE(window.findChild<QScrollArea*>("destinationScroll")->horizontalScrollBar()->maximum(),0);
 }
 void cardsAlignAllOrdinaryFieldLeftEdges(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();
  for(const auto size:{QSize(1440,1000),QSize(1060,680)}){window.resize(size);putgd::test::settle(60);for(auto* card:window.findChildren<QWidget*>())if(card->property("preferenceCard").toBool()&&card->isVisibleTo(&window)){int left=-1;for(auto* value:card->findChildren<QWidget*>())if(value->property("preferenceInlineValue").toBool()||value->property("preferenceChoiceValue").toBool()){if(!value->isVisibleTo(card))continue;const int x=value->mapTo(card,QPoint()).x();if(left<0)left=x;QCOMPARE(x,left);}}}
 }
 void providerCaptionMeasuresTheHelpTargetAtLargeText(){
  const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(21);qApp->setFont(font);
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1440,1000);window.navigate("destinations");window.show();putgd::test::settle(50);
  auto* acl=window.findChild<QComboBox*>("s3AclMode");auto* help=window.findChild<QToolButton*>("s3AclModeHelp");QVERIFY(acl&&help);QLabel* label=nullptr;for(auto* text:window.findChildren<QLabel*>())if(text->buddy()==acl)label=text;QVERIFY(label);
  const int natural=label->fontMetrics().horizontalAdvance(label->text())+help->width()+label->parentWidget()->layout()->spacing();QVERIFY2(natural-help->width()-label->parentWidget()->layout()->spacing()<=220,qPrintable(QString("Caption needs %1 px including help").arg(natural)));
  PUTGD_TRY_VERIFY2(label->width()>=label->fontMetrics().horizontalAdvance(label->text()),qPrintable(QString("Caption %1 px, label %2/%3, help %4/%5").arg(label->parentWidget()->width()).arg(label->width()).arg(label->fontMetrics().horizontalAdvance(label->text())).arg(help->width()).arg(help->sizeHint().width())));
 }
 void pageActionsAndFieldsStayWithinOneColumn(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1600,1200);window.navigate("settings");window.show();auto* tabs=window.findChild<QTabWidget*>("settingsTabs");QVERIFY(tabs);
  for(int page=0;page<tabs->count();++page){tabs->setCurrentIndex(page);putgd::test::settle(30);auto* scroll=qobject_cast<QScrollArea*>(tabs->widget(page));QVERIFY(scroll);for(auto* card:scroll->findChildren<QFrame*>())if(card->property("visualRole")=="card")QVERIFY(card->width()<=Graphite::scaled(760));auto* heading=window.findChild<QLabel*>("settingsPageHeading"+tabs->tabText(page));auto* subtitle=window.findChild<QLabel*>("settingsPageDescription"+tabs->tabText(page));QVERIFY(heading&&subtitle&&subtitle->isHidden());QVERIFY(!subtitle->text().isEmpty());QVERIFY(heading->parentWidget()->width()<=Graphite::scaled(760));}
  auto* reset=window.findChild<QPushButton*>("resetAdvancedDefaults");auto* heading=window.findChild<QLabel*>("settingsPageHeadingAdvanced");QVERIFY(reset&&heading);QVERIFY(window.findChild<QWidget*>("resourceLimits")->isAncestorOf(reset));QVERIFY(reset->isHidden()||!reset->isVisible());
  window.showCaptureSettings();putgd::test::settle(30);auto* path=window.findChild<QLineEdit*>("archiveRoot");QVERIFY(path);auto* pathHint=window.findChild<QLabel*>("archiveRootHint");QVERIFY(pathHint);QVERIFY(pathHint->alignment()&Qt::AlignLeft);QCOMPARE(pathHint->mapTo(&window,QPoint()).x(),path->mapTo(&window,QPoint()).x());QVERIFY(path->width()<=Graphite::scaled(760)-2*Graphite::space(Graphite::Space::l));QPushButton* browse=nullptr;for(auto* button:path->parentWidget()->findChildren<QPushButton*>())if(button->text()=="Browse…")browse=button;QVERIFY(browse);QCOMPARE(browse->mapTo(&window,QPoint()).x()-path->mapTo(&window,QPoint(path->width(),0)).x(),Graphite::space(Graphite::Space::s));
  window.navigate("destinations");putgd::test::settle(30);auto* header=window.findChild<QWidget*>("destinationHeader");auto* more=window.findChild<QPushButton*>("destinationActions");QVERIFY(header&&more);QVERIFY(header->width()<=Graphite::scaled(760));QCOMPARE(more->mapTo(header,QPoint(more->width(),0)).x(),header->width());QVERIFY(!window.findChild<QToolButton*>("destinationLabelHelp"));auto* region=window.findChild<QComboBox*>("s3RegionChoices");QVERIFY(region&&region->lineEdit());QCOMPARE(region->font().family(),Graphite::uiFamily());QCOMPARE(region->lineEdit()->font().family(),Graphite::uiFamily());
 }
 void shortcutsAndAdvancedUseContainedCards(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1440,960);window.navigate("settings");window.show();auto* tabs=window.findChild<QTabWidget*>("settingsTabs");
  int left=-1;for(int page=0;page<4;++page){tabs->setCurrentIndex(page);putgd::test::settle(30);auto* heading=window.findChild<QLabel*>("settingsPageHeading"+tabs->tabText(page));const int x=heading->mapTo(&window,QPoint()).x();if(left<0)left=x;QCOMPARE(x,left);}
  tabs->setCurrentIndex(2);putgd::test::settle(30);auto* card=window.findChild<QWidget*>("shortcutPreferenceCard");auto* help=window.findChild<QToolButton*>("shortcutHelpToggle");QVERIFY(card&&help);QVERIFY(card->isAncestorOf(help));for(const auto& action:Settings::presentationOrder()){const auto title=action.left(1).toUpper()+action.mid(1);QVERIFY(card->isAncestorOf(window.findChild<QPushButton*>("record"+title+"Shortcut")));}help->setChecked(true);QVERIFY(window.findChild<QLabel*>("shortcutHelp")->isVisible());help->setChecked(false);QVERIFY(window.findChild<QLabel*>("shortcutHelp")->isHidden());auto* refresh=window.findChild<QPushButton*>("refreshShortcutSupport");QCOMPARE(refresh->parentWidget(),window.findChild<QLabel*>("settingsPageHeadingShortcuts")->parentWidget());
  tabs->setCurrentIndex(3);putgd::test::settle(30);auto* toggle=window.findChild<QToolButton*>("resourceLimitsToggle");auto* history=toggle->parentWidget();QVERIFY(history->property("preferenceCard").toBool());QVERIFY(history->isAncestorOf(window.findChild<QWidget*>("resourceLimits")));toggle->setChecked(true);QVERIFY(window.findChild<QLineEdit*>("maxFileBytes")->isVisible());toggle->setChecked(false);QVERIFY(!window.findChild<QLineEdit*>("maxFileBytes")->isVisible());int cards=0;for(auto* child:window.findChild<QWidget*>("advancedPreferenceColumn")->findChildren<QWidget*>())if(child->property("preferenceCard").toBool())++cards;QCOMPARE(cards,2);
 }
 void radioChoicesLeadTheirWrappedText_data(){QTest::addColumn<int>("pixels");QTest::newRow("normal")<<14;QTest::newRow("large")<<21;}
 void radioChoicesLeadTheirWrappedText(){
  Fonts::load();VisualStyle::apply(*qApp);QFETCH(int,pixels);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(pixels);qApp->setFont(font);QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=threeDestinations();auto profiles=doc["profiles"].toArray();auto proton=profiles[1].toObject();proton["provider"]="proton-drive";proton["settings"]=ProtonProvider::defaults();profiles[1]=proton;doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();window.findChild<QListWidget*>("destinationList")->setCurrentRow(1);
  for(const auto size:{QSize(1440,960),QSize(1060,680)}){window.resize(size);putgd::test::settle(30);const auto radios=window.findChildren<QRadioButton*>();QCOMPARE(radios.size(),2);for(auto* radio:radios){QVERIFY(radio->isVisible());auto* row=radio->parentWidget();auto* line=qobject_cast<QHBoxLayout*>(row->layout()->itemAt(0)->layout());QVERIFY(line);QCOMPARE(line->itemAt(0)->widget(),radio);QCOMPARE(line->spacing(),Graphite::space(Graphite::Space::m));QCOMPARE(radio->width(),Graphite::scaled(24));QLabel* title=nullptr;for(auto* label:row->findChildren<QLabel*>())if(label->buddy()==radio)title=label;QVERIFY(title);QCOMPARE(title->text(),radio->accessibleName());auto* help=window.findChild<QToolButton*>(radio->objectName()+"Help");QVERIFY(help);QCOMPARE(help->height(),Graphite::scaled(Graphite::Type::bodyLine));QCOMPARE(help->property("graphiteIconRaise").toInt(),1);const auto rect=[row](QWidget* widget){return QRect(widget->mapTo(row,QPoint()),widget->size());};QVERIFY(rect(radio).right()<rect(title).left());QCOMPARE(rect(radio).top(),rect(title).top());const int offset=qMax(0,(title->fontMetrics().height()-Graphite::scaled(18))/2);QCOMPARE(radio->contentsMargins().top(),offset);QVERIFY(qAbs(rect(radio).top()+offset+Graphite::scaled(18)/2-(rect(title).top()+title->fontMetrics().height()/2))<=1);for(auto* label:row->findChildren<QLabel*>())if(label!=title){QVERIFY(rect(label).left()>=rect(title).left());QVERIFY(label->wordWrap());QVERIFY(label->height()>=label->heightForWidth(label->width()));if(label==line->itemAt(1)->layout()->itemAt(1)->widget())QCOMPARE(rect(label).top()-rect(title).bottom()-1,Graphite::space(Graphite::Space::xs));}QVERIFY(rect(help).left()>=rect(title).left());help->click();auto* note=window.findChild<QLabel*>(radio->objectName()+"HelpText");QVERIFY(note&&note->isVisible());PUTGD_TRY_VERIFY(rect(note).left()>=rect(title).left());QVERIFY(note->wordWrap());QTest::keyClick(help,Qt::Key_Space);QVERIFY(note->isHidden());}}
  auto* privateMode=window.findChild<QRadioButton*>("protonKeepPrivate");QTest::mouseClick(privateMode,Qt::LeftButton);QVERIFY(privateMode->isChecked());QVERIFY(!window.findChild<QCheckBox*>("protonShareDefault")->isChecked());auto* share=window.findChild<QRadioButton*>("protonCreateShareLink");share->setFocus();QTest::keyClick(share,Qt::Key_Space);QVERIFY(window.findChild<QCheckBox*>("protonShareDefault")->isChecked());
 }
 void radioTextSelectsTheChoiceWhileHelpStaysIndependent(){
  QWidget host;auto* layout=new QVBoxLayout(&host);auto* radio=new QRadioButton("Keep private");radio->setObjectName("privateChoice");radio->setAutoExclusive(false);auto* row=PreferenceRows::radio(radio,"No share link is created. Existing folder access still applies.","Upload privately without creating a link.");layout->addWidget(row);host.resize(520,200);host.show();QVERIFY(putgd::test::waitForWindowExposed(&host));QLabel* title=nullptr;QLabel* caption=nullptr;for(auto* label:row->findChildren<QLabel*>())if(label->buddy()==radio)title=label;else caption=label;auto* help=row->findChild<QToolButton*>("privateChoiceHelp");QVERIFY(title&&caption&&help);QCOMPARE(radio->accessibleName(),QString("Keep private"));QSignalSpy clicked(radio,&QRadioButton::clicked);
  for(auto* label:{title,caption}){radio->setChecked(false);QTest::mouseClick(label,Qt::LeftButton,{},label->rect().center());QVERIFY(radio->isChecked());QVERIFY(radio->hasFocus());}QCOMPARE(clicked.size(),2);
  radio->setChecked(false);QTest::mouseClick(help,Qt::LeftButton);QVERIFY(help->isChecked());QVERIFY(!radio->isChecked());QCOMPARE(clicked.size(),2);auto* explanation=row->findChild<QLabel*>("privateChoiceHelpText");QVERIFY(explanation&&explanation->isVisible());help->setFocus();QTest::keyClick(help,Qt::Key_Space);QVERIFY(!help->isChecked());QVERIFY(!radio->isChecked());
  QTest::mouseClick(title,Qt::RightButton);QVERIFY(!radio->isChecked());radio->setEnabled(false);QTest::mouseClick(caption,Qt::LeftButton);QVERIFY(!radio->isChecked());radio->setEnabled(true);radio->setFocus();QTest::keyClick(radio,Qt::Key_Space);QVERIFY(radio->isChecked());QCOMPARE(clicked.size(),3);QCOMPARE(radio->accessibleName(),QString("Keep private"));
 }
 void missingCredentialsOfferFocusedNavigation_data(){QTest::addColumn<int>("pixels");QTest::newRow("text100")<<14;QTest::newRow("text150")<<21;}
 void missingCredentialsOfferFocusedNavigation(){
  Fonts::load();VisualStyle::apply(*qApp);QFETCH(int,pixels);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(pixels);qApp->setFont(font);QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=threeDestinations();auto profiles=doc["profiles"].toArray();auto profile=profiles[0].toObject();profile["credentials"]=QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}};profiles[0]=profile;doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1060,680);window.navigate("destinations");window.show();window.activateWindow();auto* action=window.findChild<QPushButton*>("addDestinationCredentials");auto* scroll=window.findChild<QScrollArea*>("destinationScroll");auto* access=window.findChild<QLineEdit*>("credentialAccessKey");auto* secret=window.findChild<QLineEdit*>("credentialSecretKey");QVERIFY(action&&scroll&&access&&secret);PUTGD_TRY_VERIFY(action->isVisible());QVERIFY(window.findChild<QLabel*>("destinationProviderState")->text().contains("Needs credentials"));const auto revision=service.revision();int mutations=0;fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject&){if(method=="settings.save"||method=="setup.storeCredentials")++mutations;};
  for(auto* target:{access,secret}){if(target==secret)access->setText("synthetic-navigation-id");scroll->verticalScrollBar()->setValue(0);action->setFocus();QTest::keyClick(action,Qt::Key_Space);PUTGD_TRY_VERIFY(target->hasFocus());PUTGD_TRY_VERIFY2(scroll->viewport()->rect().contains(QRect(target->mapTo(scroll->viewport(),QPoint()),target->size())),qPrintable(QString("field %1 at %2,%3 %4x%5, viewport %6x%7, scroll %8/%9").arg(target->objectName()).arg(target->mapTo(scroll->viewport(),QPoint()).x()).arg(target->mapTo(scroll->viewport(),QPoint()).y()).arg(target->width()).arg(target->height()).arg(scroll->viewport()->width()).arg(scroll->viewport()->height()).arg(scroll->verticalScrollBar()->value()).arg(scroll->verticalScrollBar()->maximum())));QVERIFY(scroll->verticalScrollBar()->value()>0);QCOMPARE(service.revision(),revision);QCOMPARE(mutations,0);}
  auto* list=window.findChild<QListWidget*>("destinationList");list->setCurrentRow(1);PUTGD_TRY_VERIFY(action->isHidden());
 }
 void credentialStorageNoticeKeepsItsMeasuredHeight_data(){QTest::addColumn<int>("pixels");QTest::newRow("text100")<<14;QTest::newRow("text150")<<21;}
 void credentialStorageNoticeKeepsItsMeasuredHeight(){
  Fonts::load();VisualStyle::apply(*qApp);QFETCH(int,pixels);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(pixels);qApp->setFont(font);
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();auto* mode=window.findChild<QComboBox*>("credentialMode");auto* notice=window.findChild<QLabel*>("credentialStorageNotice");QVERIFY(mode&&notice);PUTGD_TRY_VERIFY(fixture.session.ready());QVERIFY(notice->isHidden());
  for(const auto size:{QSize(1440,1000),QSize(1060,680),QSize(1440,1000)}){window.resize(size);mode->setCurrentIndex(mode->currentData()=="session"?mode->findData("keychain"):mode->findData("session"));PUTGD_TRY_VERIFY(notice->isVisible());putgd::test::settle(30);const QRect control(mode->mapTo(&window,QPoint()),mode->size()),message(notice->mapTo(&window,QPoint()),notice->size());QVERIFY(!control.intersects(message));QVERIFY(message.top()>control.bottom());QVERIFY(notice->height()>=notice->heightForWidth(notice->width()));auto* value=notice->parentWidget();QVERIFY(value->rect().contains(notice->geometry()));QVERIFY(value->rect().contains(QRect(mode->mapTo(value,QPoint()),mode->size())));}
 }
 void largeTextSettingsKeepUsefulRowsInView(){
  Fonts::load();VisualStyle::apply(*qApp);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(21);qApp->setFont(font);QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1060,680);window.navigate("settings");window.show();auto* tabs=window.findChild<QTabWidget*>("settingsTabs");
  for(int page:{0,2}){tabs->setCurrentIndex(page);putgd::test::settle(50);auto* scroll=qobject_cast<QScrollArea*>(tabs->currentWidget());scroll->verticalScrollBar()->setValue(0);int visible=0;QStringList geometry;for(auto* row:scroll->widget()->findChildren<QWidget*>())if(row->property("preferenceRow").toBool()&&row->isVisible()){const QRect rect(row->mapTo(scroll->viewport(),QPoint()),row->size());geometry<<QString("%1 y%2 h%3").arg(row->objectName()).arg(rect.y()).arg(rect.height());if(scroll->viewport()->rect().contains(rect))++visible;}QVERIFY2(visible>=(page==0?4:3),qPrintable(QString("page %1 has %2 complete rows (%3); viewport %4").arg(page).arg(visible).arg(geometry.join(", ")).arg(scroll->viewport()->height())));QCOMPARE(scroll->horizontalScrollBar()->maximum(),0);}
  // A fixed choice whose widest option fits keeps its card inline, even when free text would need scaled(280).
  {tabs->setCurrentIndex(0);putgd::test::settle(30);auto* theme=window.findChild<QComboBox*>("appearanceTheme");QLabel* caption=nullptr;for(auto* label:window.findChildren<QLabel*>())if(label->buddy()==theme)caption=label;QVERIFY(theme&&caption);const int captionCentre=caption->mapTo(&window,caption->rect().center()).y(),themeCentre=theme->mapTo(&window,theme->rect().center()).y();QVERIFY2(qAbs(captionCentre-themeCentre)<=theme->height()/2,qPrintable(QString("caption %1, choice %2").arg(captionCentre).arg(themeCentre)));QVERIFY(theme->mapTo(&window,QPoint()).x()>caption->mapTo(&window,QPoint(caption->width(),0)).x());}
  int right=-1;for(const auto& action:Settings::presentationOrder()){auto* input=window.findChild<QLineEdit*>("shortcut"+action.left(1).toUpper()+action.mid(1));auto* field=input->parentWidget()->parentWidget();auto* row=field->parentWidget();QVERIFY(!row->property("stacked").toBool());const int edge=field->mapTo(&window,QPoint(field->width(),0)).x();if(right<0)right=edge;QCOMPARE(edge,right);QVERIFY(input->width()>=input->fontMetrics().horizontalAdvance(input->text()));}
 }
 void preferenceCardsKeepControlsAndIndicatorsAligned_data(){QTest::addColumn<int>("pixels");QTest::newRow("normal")<<14;QTest::newRow("large")<<21;}
 void preferenceCardsKeepControlsAndIndicatorsAligned(){
  Fonts::load();VisualStyle::apply(*qApp);QFETCH(int,pixels);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(pixels);qApp->setFont(font);QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1440,1200);window.navigate("settings");window.show();auto* tabs=window.findChild<QTabWidget*>("settingsTabs");
  for(const auto& size:{QSize(1440,960),QSize(1060,680)}){window.resize(size);int columnLeft=-1;for(int page=0;page<2;++page){tabs->setCurrentIndex(page);putgd::test::settle(30);auto* scroll=qobject_cast<QScrollArea*>(tabs->widget(page));auto* column=window.findChild<QWidget*>(page?"capturePreferenceColumn":"generalPreferenceColumn");QVERIFY(column);QVERIFY(column->width()<=Graphite::scaled(760));const int left=column->mapTo(scroll->viewport(),QPoint()).x(),right=scroll->viewport()->width()-left-column->width();QCOMPARE(left,Graphite::space(Graphite::Space::xxl));QVERIFY(right>=left-2);const int globalLeft=column->mapTo(&window,QPoint()).x();if(columnLeft<0)columnLeft=globalLeft;QCOMPARE(globalLeft,columnLeft);QWidget* previousSection=nullptr;for(auto* section:column->findChildren<QWidget*>(Qt::FindDirectChildrenOnly))if(section->property("preferenceSection").toBool()){auto* heading=section->layout()->itemAt(0)->widget();QCOMPARE(heading->height(),heading->sizeHint().height());for(auto* label:section->findChildren<QLabel*>(Qt::FindDirectChildrenOnly))if(label->wordWrap())QVERIFY(label->height()>=label->heightForWidth(label->width()));QVERIFY(column->rect().contains(section->geometry()));if(previousSection)PUTGD_TRY_VERIFY2(section->y()-previousSection->geometry().bottom()-1>=Graphite::space(Graphite::Space::xxl),qPrintable(QString("page %1 window %2x%3; section y/h/min/max %4/%5/%6/%7; previous bottom %8; column h/min/minimum/HFW %9/%10/%11/%12; spacing %13; previous min/HFW %14/%15; current min/HFW %16/%17").arg(page).arg(window.width()).arg(window.height()).arg(section->y()).arg(section->height()).arg(section->minimumHeight()).arg(section->maximumHeight()).arg(previousSection->geometry().bottom()+1).arg(column->height()).arg(column->minimumHeight()).arg(column->layout()->totalMinimumSize().height()).arg(column->layout()->totalHeightForWidth(column->width())).arg(column->layout()->spacing()).arg(previousSection->layout()->totalMinimumSize().height()).arg(previousSection->layout()->totalHeightForWidth(previousSection->width())).arg(section->layout()->totalMinimumSize().height()).arg(section->layout()->totalHeightForWidth(section->width()))));previousSection=section;}int cards=0;for(auto* card:column->findChildren<QWidget*>())if(card->property("preferenceCard").toBool()&&card->isVisibleTo(column)){++cards;QVERIFY(card->parentWidget()->rect().contains(card->geometry()));auto* form=card->findChild<QFormLayout*>();QVERIFY(form);for(int row=0;row<form->rowCount();++row){auto* item=form->itemAt(row,QFormLayout::FieldRole);if(!item||!item->widget()||!form->isRowVisible(row))continue;auto* field=item->widget();QVERIFY(card->rect().contains(QRect(field->mapTo(card,QPoint()),field->size())));if(auto* text=qobject_cast<QLabel*>(field))QVERIFY(text->height()>=text->heightForWidth(text->width()));else if(field->property("preferenceRow").toBool())QVERIFY2(field->height()>=Graphite::scaled(Graphite::Metric::control)+2*Graphite::space(Graphite::Space::s),qPrintable(QString("field %1 height %2, floor %3, class %4, minimum %5").arg(field->objectName()).arg(field->height()).arg(Graphite::scaled(Graphite::Metric::control)+2*Graphite::space(Graphite::Space::s)).arg(field->metaObject()->className()).arg(field->minimumHeight())));QVERIFY(!form->itemAt(row,QFormLayout::LabelRole));for(auto* value:field->findChildren<QWidget*>())if(value->objectName().endsWith("PreferenceValue"))if(value->property("preferenceChoiceValue").toBool()){QVERIFY(value->width()>0);QCOMPARE(value->mapTo(card,QPoint(value->width(),0)).x(),card->width()-Graphite::space(Graphite::Space::l));}}}QVERIFY(cards>=3);QCOMPARE(scroll->horizontalScrollBar()->maximum(),0);}}
  tabs->setCurrentIndex(0);auto* check=window.findChild<QCheckBox*>("notifications");QVERIFY(!check->text().isEmpty());QCOMPARE(check->accessibleName(),check->text());QLabel* label=nullptr;for(auto* candidate:tabs->widget(0)->findChildren<QLabel*>())if(candidate->buddy()==check)label=candidate;QVERIFY(label);const bool before=check->isChecked();QTest::mouseClick(label,Qt::LeftButton);QCOMPARE(check->isChecked(),!before);
 }
 void destinationFieldsHintsAndSharingTextFit_data(){QTest::addColumn<int>("pixels");QTest::newRow("normal")<<14;QTest::newRow("large")<<21;}
 void destinationFieldsHintsAndSharingTextFit(){
  Fonts::load();VisualStyle::apply(*qApp);QFETCH(int,pixels);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(pixels);qApp->setFont(font);QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=threeDestinations();auto profiles=doc["profiles"].toArray();auto proton=profiles[2].toObject();auto account=ProtonProvider::defaults();account["accountRootUid"]="synthetic-account";account["folderUid"]="synthetic-folder";account["folderName"]="Saved folder";proton["provider"]="proton-drive";proton["settings"]=account;profiles[2]=proton;doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1440,1200);window.navigate("destinations");window.show();auto* list=window.findChild<QListWidget*>("destinationList");list->setCurrentRow(0);putgd::test::settle(30);auto* firstField=window.findChild<QWidget*>("s3Service");QVERIFY(firstField);PUTGD_TRY_VERIFY(firstField->isVisible());for(const auto& name:{"s3Service","s3Bucket","s3RegionChoices","s3ObjectPrefix","credentialAccessKey","credentialSecretKey","credentialSessionToken","credentialMode"}){auto* field=window.findChild<QWidget*>(name);QVERIFY(field);PUTGD_TRY_COMPARE(field->mapTo(&window,QPoint(field->width(),0)).x(),firstField->mapTo(&window,QPoint(firstField->width(),0)).x());if(qobject_cast<QComboBox*>(field))QCOMPARE(field->font().family(),Graphite::uiFamily());else QCOMPARE(field->font().family(),Graphite::uiFamily());}
  auto* token=window.findChild<QLineEdit*>("credentialSessionToken");auto* mode=window.findChild<QComboBox*>("credentialMode");QVERIFY(token&&mode);QVERIFY(!window.findChild<QLabel*>("credentialSessionTokenHint"));QVERIFY2(mode->mapTo(&window,QPoint()).y()-token->mapTo(&window,QPoint(0,token->height())).y()>=Graphite::space(Graphite::Space::m),"The next row keeps its control gap without a token helper");const int familyLeft=firstField->mapTo(&window,QPoint()).x();list->setCurrentRow(2);auto* provider=window.findChild<QComboBox*>("storageProvider");provider->setCurrentIndex(provider->findData("proton-drive"));putgd::test::settle(30);auto* folder=window.findChild<QComboBox*>("protonFolder");QVERIFY(folder);PUTGD_TRY_VERIFY(folder->isVisible());QVERIFY(folder->width()>=Graphite::scaled(200));QCOMPARE(folder->mapTo(&window,QPoint()).x(),familyLeft);list->setCurrentRow(0);putgd::test::settle(30);const int s3Left=firstField->mapTo(&window,QPoint()).x();list->setCurrentRow(2);putgd::test::settle(30);QCOMPARE(folder->mapTo(&window,QPoint()).x(),s3Left);bool found=false;for(auto* label:window.findChildren<QLabel*>())if(label->text().startsWith("Anyone with the link can view it.")){found=true;QVERIFY(label->height()>=label->heightForWidth(label->width()));QVERIFY(label->parentWidget()->rect().contains(QRect(label->mapTo(label->parentWidget(),QPoint()),label->size())));}QVERIFY(found);
 }
 void protonAccountActionsStayInlineWhenTheirContentsFit(){
  VisualStyle::apply(*qApp);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(21);qApp->setFont(font);QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=threeDestinations();auto profiles=doc["profiles"].toArray();auto proton=profiles[2].toObject();proton["provider"]="proton-drive";auto settings=ProtonProvider::defaults();settings["accountRootUid"]="synthetic-account";settings["accountEmail"]="designer@example.invalid";settings["folderUid"]="synthetic-folder";proton["settings"]=settings;profiles[2]=proton;doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1060,680);window.navigate("destinations");window.show();window.findChild<QListWidget*>("destinationList")->setCurrentRow(2);putgd::test::settle(30);
  auto* account=window.findChild<QLabel*>("protonAccountStatus");auto* reconnect=window.findChild<QPushButton*>("connectProtonAccount");auto* test=window.findChild<QPushButton*>("testDestination");QVERIFY(account&&reconnect&&test);auto* row=reconnect->parentWidget();auto* line=qobject_cast<QBoxLayout*>(row->layout()->itemAt(0)->layout());QVERIFY(line);QCOMPARE(line->direction(),QBoxLayout::LeftToRight);QVERIFY(reconnect->mapTo(&window,QPoint()).x()>=account->mapTo(&window,QPoint(account->width(),0)).x()+Graphite::space(Graphite::Space::l));QVERIFY(test->mapTo(&window,QPoint()).x()>row->mapTo(&window,QPoint()).x()+row->width()/2);
 }
 void sharedFormsUseOneMinimumControlWidth(){
  QWidget host;QVBoxLayout outer(&host);QList<QWidget*> pages;QList<QFormLayout*> forms;
  for(int width:{80,220}){auto* page=new QWidget;auto* box=new QVBoxLayout(page);box->setContentsMargins(28,0,28,0);auto* form=new QFormLayout;VisualStyle::styleForm(form);auto* field=new QLineEdit;field->setMinimumWidth(width);form->addRow(new QLabel("Value"),field);box->addLayout(form);outer.addWidget(page);pages<<page;forms<<form;}
  for(auto* page:pages)Graphite::alignForms(page,forms);host.resize(520,300);host.show();PUTGD_TRY_COMPARE(forms[0]->rowWrapPolicy(),QFormLayout::DontWrapRows);QCOMPARE(forms[1]->rowWrapPolicy(),forms[0]->rowWrapPolicy());host.resize(320,300);PUTGD_TRY_COMPARE(forms[0]->rowWrapPolicy(),QFormLayout::WrapAllRows);QCOMPARE(forms[1]->rowWrapPolicy(),forms[0]->rowWrapPolicy());
 }
 void destinationCardsKeepProviderActionsAndFieldsContained_data(){QTest::addColumn<int>("pixels");QTest::addColumn<bool>("light");QTest::newRow("dark")<<14<<false;QTest::newRow("light")<<14<<true;QTest::newRow("dark-large")<<21<<false;QTest::newRow("light-large")<<21<<true;}
 void destinationCardsKeepProviderActionsAndFieldsContained(){
  Fonts::load();VisualStyle::apply(*qApp);QFETCH(int,pixels);QFETCH(bool,light);const auto original=qApp->font();const auto scheme=Theme::scheme();auto restore=qScopeGuard([&]{qApp->setFont(original);Theme::setScheme(scheme);});auto font=original;font.setPixelSize(pixels);qApp->setFont(font);Theme::setScheme(light?Theme::Scheme::Light:Theme::Scheme::Dark);
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});auto doc=threeDestinations();auto profiles=doc["profiles"].toArray();auto proton=profiles[2].toObject();auto account=ProtonProvider::defaults();account["accountRootUid"]="synthetic-account";account["folderUid"]="synthetic-folder";account["folderName"]="Saved folder";proton["provider"]="proton-drive";proton["settings"]=account;profiles[2]=proton;doc["profiles"]=profiles;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.navigate("destinations");window.show();auto* scroll=window.findChild<QScrollArea*>("destinationScroll");auto* column=window.findChild<QWidget*>("destinationPreferenceColumn");auto* list=window.findChild<QListWidget*>("destinationList");auto* advanced=window.findChild<QToolButton*>("advancedSettings");auto* test=window.findChild<QPushButton*>("testDestination");
  auto cardOf=[](QWidget* widget){for(auto* parent=widget->parentWidget();parent;parent=parent->parentWidget())if(parent->property("preferenceCard").toBool())return parent;return static_cast<QWidget*>(nullptr);};
  for(const auto size:{QSize(1600,1100),QSize(1060,680)}){window.resize(size);for(int profile:{0,2,0}){list->setCurrentRow(profile);putgd::test::settle(30);PUTGD_TRY_COMPARE(column->width(),qMin(Graphite::scaled(760),scroll->viewport()->width()-2*Graphite::space(Graphite::Space::xxl)));QCOMPARE(column->mapTo(scroll->viewport(),QPoint()).x(),Graphite::space(Graphite::Space::xxl));QCOMPARE(scroll->horizontalScrollBar()->maximum(),0);QCOMPARE(window.findChild<QLabel*>("destinationProviderState")->text(),list->currentItem()->text().section('\n',1));auto* identity=window.findChild<QWidget*>("destinationHeader");QVERIFY(identity&&!cardOf(identity));QVERIFY(!identity->property("preferenceCard").toBool());QVERIFY(identity->isAncestorOf(window.findChild<QLabel*>("destinationProviderDescription")));QCOMPARE(identity->layout()->contentsMargins(),QMargins());auto* testCard=cardOf(test);QVERIFY(testCard);QVERIFY(testCard->parentWidget()->property("preferenceSection").toBool());for(auto* heading:testCard->findChildren<QLabel*>())QVERIFY(heading->property("visualRole")!="sectionTitle");QVERIFY(test->isVisible());QVERIFY(window.findChild<QLabel*>("destinationTestScope")->isVisible());QVERIFY(testCard->rect().contains(QRect(test->mapTo(testCard,QPoint()),test->size())));QVERIFY(cardOf(advanced));
    for(auto* card:column->findChildren<QWidget*>())if(card->property("preferenceCard").toBool()&&card->isVisible()){PUTGD_TRY_VERIFY(column->rect().contains(QRect(card->mapTo(column,QPoint()),card->size())));for(auto* control:card->findChildren<QWidget*>())if((qobject_cast<QLineEdit*>(control)||qobject_cast<QComboBox*>(control)||qobject_cast<QAbstractButton*>(control))&&control->isVisible()){PUTGD_TRY_VERIFY2(card->rect().contains(QRect(control->mapTo(card,QPoint()),control->size())),qPrintable(QString("%1 profile %2, window %3x%4, card %5x%6, control %7,%8 %9x%10").arg(control->objectName()).arg(profile).arg(window.width()).arg(window.height()).arg(card->width()).arg(card->height()).arg(control->mapTo(card,QPoint()).x()).arg(control->mapTo(card,QPoint()).y()).arg(control->width()).arg(control->height())));if(qobject_cast<QPushButton*>(control))PUTGD_TRY_VERIFY2(control->height()>=control->sizeHint().height(),qPrintable(QString("%1 height %2, minimum %3, hint %4").arg(control->objectName()).arg(control->height()).arg(control->minimumHeight()).arg(control->sizeHint().height())));}}
    if(profile==0){auto* bucket=window.findChild<QLineEdit*>("s3Bucket");QLabel* label=nullptr;for(auto* candidate:column->findChildren<QLabel*>())if(candidate->buddy()==bucket)label=candidate;QVERIFY(label);QVERIFY(!QRect(label->mapTo(column,QPoint()),label->size()).intersects(QRect(bucket->mapTo(column,QPoint()),bucket->size())));auto* region=window.findChild<QComboBox*>("s3RegionChoices");if(region->isVisible()){PUTGD_TRY_COMPARE(region->width(),window.findChild<QComboBox*>("s3Service")->width());PUTGD_TRY_VERIFY(window.findChild<QComboBox*>("s3AclMode")->width()>=Graphite::scaled(280));}}
    else{auto* refresh=window.findChild<QPushButton*>("inspectProtonAccount");QCOMPARE(refresh->size(),QSize(Graphite::scaled(36),Graphite::scaled(36)));QCOMPARE(refresh->iconSize(),QSize(Graphite::scaled(20),Graphite::scaled(20)));for(const auto name:{"connectProtonAccount","createProtonFolder"}){auto* action=window.findChild<QPushButton*>(name);QVERIFY(action->property("graphiteDensity")!="compact");QVERIFY2(action->height()>=Graphite::scaled(36),qPrintable(QString("%1 height %2, standard %3").arg(name).arg(action->height()).arg(Graphite::scaled(36))));}for(const auto name:{"protonCreateShareLink","protonKeepPrivate"}){auto* radio=window.findChild<QRadioButton*>(name);QVERIFY(radio);QCOMPARE(radio->width(),Graphite::scaled(24));QVERIFY(!radio->accessibleName().isEmpty());auto* help=window.findChild<QToolButton*>(QString(name)+"Help");QVERIFY(help);help->click();auto* note=window.findChild<QLabel*>(QString(name)+"HelpText");QVERIFY(note&&note->isVisible());QCOMPARE(note->font().pixelSize(),Graphite::scaled(13));help->click();}auto* privateMode=window.findChild<QRadioButton*>("protonKeepPrivate");privateMode->click();QVERIFY(window.findChild<QCheckBox*>("protonShareDefault")->isChecked()==false);window.findChild<QRadioButton*>("protonCreateShareLink")->click();QVERIFY(window.findChild<QCheckBox*>("protonShareDefault")->isChecked());}
    if(profile==0){auto* provider=window.findChild<QComboBox*>("storageProvider");provider->setCurrentIndex(provider->findData("backblaze-b2"));PUTGD_TRY_VERIFY(!window.findChild<QLineEdit*>("credentialSessionToken")->isVisible());auto* mode=window.findChild<QComboBox*>("b2LinkMode");PUTGD_TRY_VERIFY(mode->isVisible());for(const auto sharing:{"authorized","public"}){mode->setCurrentIndex(mode->findData(sharing));QMetaObject::invokeMethod(mode,"activated",Q_ARG(int,mode->currentIndex()));PUTGD_TRY_COMPARE(window.findChild<QSpinBox*>("b2LinkDays")->isVisible(),QString(sharing)=="authorized");QCOMPARE(scroll->horizontalScrollBar()->maximum(),0);if(QString(sharing)=="authorized"){auto* days=window.findChild<QSpinBox*>("b2LinkDays");QLabel* unit=nullptr;for(auto* text:days->parentWidget()->findChildren<QLabel*>())if(text->property("visualRole")=="unitSuffix")unit=text;QVERIFY(unit);PUTGD_TRY_COMPARE(unit->mapTo(&window,QPoint()).x(),days->mapTo(&window,QPoint(days->width(),0)).x());}}auto* access=window.findChild<QLineEdit*>("credentialAccessKey");QCOMPARE(access->accessibleName(),QString("Application key ID"));QLabel* caption=nullptr;for(auto* text:column->findChildren<QLabel*>())if(text->buddy()==access)caption=text;QVERIFY(caption);QCOMPARE(caption->text(),QString("Application key ID"));provider->setCurrentIndex(provider->findData("s3"));PUTGD_TRY_VERIFY(window.findChild<QLineEdit*>("credentialSessionToken")->isVisible());QCOMPARE(caption->text(),QString("Access key ID"));}
    advanced->setChecked(true);PUTGD_TRY_COMPARE(scroll->horizontalScrollBar()->maximum(),0);advanced->setChecked(false);
   }}
 }
 void destinationFooterAndActionsShareTheFormColumn(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"),[](auto,auto){return Error{};});QVERIFY(service.save(threeDestinations(),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1600,1100);window.navigate("destinations");window.show();auto* footer=window.findChild<QWidget*>("destinationStatusRow");auto* more=window.findChild<QPushButton*>("destinationActions");auto* list=window.findChild<QListWidget*>("destinationList");QVERIFY(footer&&more&&list);PUTGD_TRY_VERIFY(footer->width()<=Graphite::scaled(880)+56);QCOMPARE(more->size(),QSize(Graphite::scaled(36),Graphite::scaled(36)));QCOMPARE(list->iconSize(),QSize(Graphite::scaled(Graphite::Metric::icon),Graphite::scaled(Graphite::Metric::icon)));auto* test=window.findChild<QPushButton*>("testDestination");QVERIFY(window.findChild<QScrollArea*>("destinationScroll")->widget()->isAncestorOf(test));
 }
 void conflictOutranksOutputNoticesAndCompactHeadingsRecover(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));auto doc=Settings::defaults();auto archive=doc["archive"].toObject();archive["keepUploads"]=false;doc["archive"]=archive;QVERIFY(service.save(doc,{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1060,680);window.navigate("settings");window.show();auto* settings=window.settings();auto* tabs=window.findChild<QTabWidget*>("settingsTabs");tabs->setCurrentIndex(1);PUTGD_TRY_VERIFY(window.findChild<QWidget*>("captureOutputBlock")->isVisible());auto state=settings->saveViewState();state["conflict"]=true;QVERIFY(settings->restoreViewState(state));PUTGD_TRY_VERIFY(window.findChild<QWidget*>("captureOutputBlock")->isHidden());tabs->setCurrentIndex(0);PUTGD_TRY_VERIFY(window.findChild<QWidget*>("commandOutputWarning")->isHidden());auto* heading=window.findChild<QLabel*>("settingsPageHeadingGeneral");auto* intro=window.findChild<QLabel*>("settingsPageDescriptionGeneral");QVERIFY(heading&&intro);PUTGD_TRY_VERIFY(heading->property("compactHeading").toBool());QVERIFY(intro->isHidden());auto* page=qobject_cast<QScrollArea*>(tabs->currentWidget());QCOMPARE(page->widget()->layout()->contentsMargins(),QMargins(Graphite::space(Graphite::Space::xxl),qMin(Graphite::pageHeaderTop(),Graphite::space(Graphite::Space::l)),Graphite::space(Graphite::Space::xxl),Graphite::space(Graphite::Space::xl)));QCOMPARE(page->widget()->layout()->spacing(),Graphite::space(Graphite::Space::xxl));QCOMPARE(heading->font().pixelSize(),Graphite::font(Graphite::Type::compactPage,true).pixelSize());QVERIFY(heading->font().pixelSize()>Graphite::font(Graphite::Type::section,true).pixelSize());const auto scheme=Theme::scheme();Theme::setScheme(scheme==Theme::Scheme::Dark?Theme::Scheme::Light:Theme::Scheme::Dark);QCOMPARE(heading->font().pixelSize(),Graphite::font(Graphite::Type::compactPage,true).pixelSize());Theme::setScheme(scheme);window.resize(1440,1200);PUTGD_TRY_VERIFY(!heading->property("compactHeading").toBool());QVERIFY(intro->isHidden());
 }
 void commandBandChildrenStayApartAtMinimumWidth_data(){QTest::addColumn<int>("pixels");QTest::newRow("text100")<<14;QTest::newRow("text150")<<21;QTest::newRow("text200")<<28;}
 void commandBandChildrenStayApartAtMinimumWidth(){
  QFETCH(int,pixels);Fonts::load();VisualStyle::apply(*qApp);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(pixels);qApp->setFont(font);
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.show();auto* band=window.findChild<QWidget*>("commandBand");QVERIFY(band);
  for(const int width:{1440,1060,window.minimumWidth()}){window.resize(width,680);putgd::test::settle(30);QList<QRect> bounds;for(auto* child:band->findChildren<QWidget*>(QString{},Qt::FindDirectChildrenOnly)){if(!child->isVisible()||child->isWindow())continue;const auto rect=child->geometry();QVERIFY2(band->rect().contains(rect),qPrintable(child->objectName()));for(const auto& other:bounds)QVERIFY2(!other.intersects(rect),qPrintable(QString("%1 overlaps at %2 px text, window %3").arg(child->objectName()).arg(pixels).arg(window.width())));bounds.append(rect);if(auto* button=qobject_cast<QToolButton*>(child)){QCOMPARE(button->font().pixelSize(),pixels);QVERIFY(button->width()>=button->sizeHint().width());if(button->popupMode()==QToolButton::MenuButtonPopup){QStyleOptionToolButton option;option.initFrom(button);option.rect=button->rect();option.features=QStyleOptionToolButton::MenuButtonPopup|QStyleOptionToolButton::HasMenu;const auto arrow=button->style()->subControlRect(QStyle::CC_ToolButton,&option,QStyle::SC_ToolButtonMenu,button);QCOMPARE(arrow.width(),Graphite::scaled(Graphite::Metric::splitArrow));}}}QVERIFY(bounds.size()>=6);}
 }
 void mainBandsUseSharedInsetsAndOneTypeSizePerSide(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.show();
  auto* band=window.findChild<QWidget*>("commandBand");auto* status=window.findChild<QWidget*>("statusBand");QVERIFY(band&&status);
  const auto insets=QMargins(Graphite::space(Graphite::Space::m),Graphite::space(Graphite::Space::s),Graphite::space(Graphite::Space::m),Graphite::space(Graphite::Space::s));
  for(auto size:{QSize(1440,1000),QSize(1060,680)}){window.resize(size);putgd::test::settle(30);QCOMPARE(band->layout()->contentsMargins(),insets);QCOMPARE(status->height(),Graphite::scaled(40));auto* header=window.findChild<QWidget*>("windowHeader");auto* navigation=window.findChild<QTabBar*>("mainNavigation");QVERIFY(header->height()>=navigation->height()+2*Graphite::space(Graphite::Space::xs));if(size.height()<Graphite::scaled(720))QVERIFY(header->height()<Graphite::scaled(Graphite::Metric::header));}
  auto* chrome=window.findChild<QWidget*>("mainChrome");QVERIFY(chrome&&chrome->isAncestorOf(band)&&chrome->isAncestorOf(window.findChild<QWidget*>("windowHeader")));
  auto* output=window.findChild<QToolButton*>("captureOutputSummary");QCOMPARE(output->font().pixelSize(),Graphite::scaled(13));QCOMPARE(output->font().weight(),QFont::Normal);
  auto* version=window.findChild<QLabel*>("appVersion");auto* versionLink=window.findChild<QPushButton*>("appVersionLink");QCOMPARE(version->font().pixelSize(),Graphite::scaled(13));QCOMPARE(version->font().weight(),QFont::Normal);QCOMPARE(version->font().pixelSize(),versionLink->font().pixelSize());QCOMPARE(window.findChild<QPushButton*>("updateAvailable")->font().pixelSize(),version->font().pixelSize());
  fixture.server.setUiDiagnostics([&]{auto data=fixture.operations().diagnostics();data["update"]=QJsonObject{{"supported",true},{"result",QJsonObject{{"updateAvailable",true},{"latest","26.1028.9999"}}}};return data;});fixture.session.refreshSnapshot();auto* update=window.findChild<QPushButton*>("updateAvailable");PUTGD_TRY_VERIFY(update->isVisible());
  auto* settings=window.settings();auto state=settings->saveViewState();state["conflict"]=true;QVERIFY(settings->restoreViewState(state));auto* feedback=window.findChild<QLabel*>("autosaveStatus");PUTGD_TRY_VERIFY(feedback->isVisible());QCOMPARE(feedback->font().pixelSize(),Graphite::scaled(13));QVERIFY(version->isHidden());QVERIFY(window.findChild<QWidget*>("appVersionGroup")->isHidden());QVERIFY(update->isVisible());QVERIFY(update->width()>=update->sizeHint().width());QVERIFY(window.findChild<QLabel*>("autosaveExplanation")->isHidden());
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
  window.resize(1060,680);apply(QString(180,'x')+"-dev.430",false);PUTGD_TRY_VERIFY(label->isHidden());auto* summary=window.findChild<QToolButton*>("captureOutputSummary");QVERIFY(summary);PUTGD_TRY_VERIFY(summary->width()>=summary->sizeHint().width());
 }
};
QTEST_MAIN(SettingsGui)
#include "test_settings.moc"
