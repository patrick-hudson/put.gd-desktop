#include "core/BuildInfo.h"
#include "ui/AboutWindow.h"
#include "ui/VisualStyle.h"
#include "ui/Graphite.h"
#include "platform/HostOpen.h"
#include <QLabel>
#include <QMessageBox>
#include <QDialogButtonBox>
#include <QLayout>
#include <QUrl>
#include <QApplication>
#include <QJsonDocument>
#include <QPointer>
#include <QPushButton>
#include <QToolButton>
#include <QStyle>
#include <QTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QFormLayout>
#include <QClipboard>
#include <QTest>
#include <QTimer>
#include <QScopeGuard>
#include <memory>

using namespace putgd;

class VersionView final : public QObject {
 Q_OBJECT
private slots:
 void aboutUsesTheConnectedResidentBuildAndIsModeless() {
  auto manifest = QJsonDocument::fromJson(buildManifestJson()).object();
  // Deliberately differ from this test binary to prove the displayed identity
  // comes from the connected resident rather than a helper-local build lookup.
  manifest["version"] = "0.1.1-dev.999+g0123456789ab";
  manifest["gitCommit"] = "0123456789abcdef0123456789abcdef01234567";
  auto owner = std::make_unique<AboutWindow>(manifest);
  QPointer<AboutWindow> dialog = owner.get();
  bool enteredModalLoop = false;
  QTimer modalGuard;
  modalGuard.setSingleShot(true);
  connect(&modalGuard, &QTimer::timeout, this, [&] {
   if (auto* modal = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
    enteredModalLoop = true;
    modal->accept();
   }
  });
  modalGuard.start(0);
  dialog->show();
  QApplication::processEvents();
  modalGuard.stop();
  QVERIFY2(!enteredModalLoop, "About must leave capture input and other views usable");
  QVERIFY(dialog->isVisible());
  QVERIFY(!dialog->isModal());
  QCOMPARE(dialog->windowModality(), Qt::NonModal);
  QVERIFY(!QApplication::activeModalWidget());
  auto* version=dialog->findChild<QLabel*>("aboutVersion");QVERIFY(version);QCOMPARE(version->text(),manifest["version"].toString().section('-',0,0).section('+',0,0));
  QCOMPARE(QJsonDocument::fromJson(dialog->detailedText().toUtf8()).object(),manifest);
  auto* details=dialog->findChild<QToolButton*>("buildDetails");auto* rows=dialog->findChild<QWidget*>("aboutBuildDetails");auto* copy=dialog->findChild<QPushButton*>("copyBuildDetails");QVERIFY(details&&rows&&copy);QVERIFY(details->property("graphiteDisclosure").toBool());QVERIFY(rows->property("preferenceCard").toBool());QVERIFY(copy->property("graphiteFlush").toBool());QCOMPARE(details->parentWidget()->objectName(),QString("aboutIdentity"));QVERIFY(!rows->isVisible());QVERIFY(!copy->isVisible());details->click();QApplication::processEvents();QVERIFY(rows->isVisible());QVERIFY(copy->isVisible());
  auto* commit=dialog->findChild<QTextEdit*>("buildCommit");QVERIFY(commit);QCOMPARE(commit->toPlainText(),manifest["gitCommit"].toString().left(12));QCOMPARE(commit->toolTip(),manifest["gitCommit"].toString());QCOMPARE(commit->accessibleDescription(),manifest["gitCommit"].toString());QCOMPARE(commit->wordWrapMode(),QTextOption::WrapAnywhere);QVERIFY(!dialog->findChild<QTextEdit*>("buildVersion")->isVisible());QVERIFY(dialog->width()==Graphite::scaled(440));QVERIFY(commit->isReadOnly());QCOMPARE(commit->horizontalScrollBar()->maximum(),0);commit->selectAll();commit->copy();QCOMPARE(qApp->clipboard()->text(),manifest["gitCommit"].toString());const auto copyBounds=copy->geometry();copy->click();QCOMPARE(QJsonDocument::fromJson(qApp->clipboard()->text().toUtf8()).object(),manifest);QCOMPARE(copy->text(),QString("Copied"));QCOMPARE(copy->accessibleName(),QString("Copied"));QApplication::processEvents();QCOMPARE(copy->geometry(),copyBounds);QTRY_COMPARE(copy->text(),QString("Copy build details"));QCOMPARE(copy->geometry(),copyBounds);details->click();
  QVERIFY(!dialog->testAttribute(Qt::WA_DeleteOnClose));
  dialog->close();
  QVERIFY(dialog);
  QVERIFY(!dialog->isVisible());
  dialog->show();details->click();QVERIFY(details->isChecked());QVERIFY(rows->isVisible());details->click();QCOMPARE(details->text(),QString("Build details"));QVERIFY(!rows->isVisible());dialog->close();
  owner.reset();
  QVERIFY(dialog.isNull());
 }

 void aboutUsesTheSharedSpacingAndTypeRoles() {
  VisualStyle::apply(*qApp);AboutWindow dialog({{"version","26.1027.1543-dev"}});dialog.show();QApplication::processEvents();
  auto* content=dialog.findChild<QWidget*>("aboutContent");auto* buttons=dialog.findChild<QWidget*>("aboutActions");auto* icon=dialog.findChild<QLabel*>("qt_msgboxex_icon_label");auto* tagline=dialog.findChild<QLabel*>("qt_msgbox_informativelabel");auto* version=dialog.findChild<QLabel*>("aboutVersion");auto* website=dialog.findChild<QPushButton*>("putgdWebsite");QVERIFY(content&&buttons&&icon&&tagline&&version&&website);for(auto* text:{dialog.findChild<QLabel*>("qt_msgbox_label"),tagline,version})QVERIFY2(text->height()>=text->fontMetrics().height(),qPrintable(text->objectName()));
  const int margin=Graphite::space(Graphite::Space::xl);QCOMPARE(dialog.layout()->contentsMargins(),QMargins(margin,margin,margin,margin));QCOMPARE(content->mapTo(&dialog,QPoint()),QPoint(margin,margin));QVERIFY(icon->width()>=Graphite::scaled(64));QVERIFY(icon->height()>=Graphite::scaled(64));QCOMPARE(tagline->font().pixelSize(),Graphite::scaled(13));QCOMPARE(tagline->palette().color(QPalette::WindowText),Theme::current().textSecondary);QCOMPARE(version->palette().color(QPalette::WindowText),Theme::current().textSecondary);
  QVERIFY(version->mapTo(&dialog,QPoint()).y()<website->mapTo(&dialog,QPoint()).y());QCOMPARE(buttons->mapTo(&dialog,QPoint()).y()-content->mapTo(&dialog,QPoint(0,content->height())).y(),margin);QCOMPARE(buttons->mapTo(&dialog,QPoint(buttons->width(),0)).x(),dialog.width()-margin);
 }

 void expandedAboutFitsTheScreenWithoutSidewaysScroll_data(){QTest::addColumn<int>("pixels");QTest::newRow("text100")<<14;QTest::newRow("text150")<<21;}
 void expandedAboutFitsTheScreenWithoutSidewaysScroll(){
  QFETCH(int,pixels);VisualStyle::apply(*qApp);const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});auto font=original;font.setPixelSize(pixels);qApp->setFont(font);
  const QJsonObject manifest{{"version","26.1028.1702-dev+ga40505cb7d78.dirty"},{"gitCommit","a40505cb7d788b598878277469fad0c1f2e3d4c5"},{"dirty",true},{"baseVersion","26.1028.1702"},{"commitCount",4182},{"gitTree","5c0ffee5c0ffee5c0ffee5c0ffee5c0ffee5c0ff"},{"packageVersion","26.1028.1702~dev.4182.ga40505cb7d78"},{"schemaVersion",1},{"sourceProvenance","git"}};
  AboutWindow dialog(manifest);dialog.show();QApplication::processEvents();dialog.findChild<QToolButton*>("buildDetails")->click();QApplication::processEvents();
  const auto available=dialog.screen()->availableGeometry();QVERIFY2(dialog.height()<=available.height()&&dialog.width()<=available.width(),qPrintable(QString("%1x%2 in %3x%4").arg(dialog.width()).arg(dialog.height()).arg(available.width()).arg(available.height())));
  QVERIFY2(dialog.height()<=dialog.maximumFitHeight(),qPrintable(QString("Height %1 exceeds %2").arg(dialog.height()).arg(dialog.maximumFitHeight())));QCOMPARE(dialog.maximumFitHeight(),qMin(Graphite::scaled(600),available.height()-2*Graphite::space(Graphite::Space::xl)));for(const auto* name:{"buildCommit","build_gitTree"}){auto* hash=dialog.findChild<QTextEdit*>(name);QCOMPARE(hash->toPlainText().size(),12);QVERIFY(hash->toolTip().size()>12);QCOMPARE(hash->accessibleDescription(),hash->toolTip());QCOMPARE(hash->font().family(),Graphite::monoFamily());}QCOMPARE(dialog.findChild<QTextEdit*>("build_commitCount")->font().family(),Graphite::uiFamily());
  auto* card=dialog.findChild<QWidget*>("aboutBuildDetails");QVERIFY(card->isVisible());int captionLeft=-1,valueLeft=-1;
  for(auto* value:card->findChildren<QTextEdit*>()){
   QCOMPARE(value->wordWrapMode(),QTextOption::WrapAnywhere);QCOMPARE(value->horizontalScrollBar()->maximum(),0);QVERIFY(value->height()>=value->heightForWidth(value->width()));
   if(valueLeft<0)valueLeft=value->x();QCOMPARE(value->x(),valueLeft);
   auto* caption=card->findChild<QLabel*>(value->objectName()+"Caption");QVERIFY(caption);if(captionLeft<0)captionLeft=caption->x();QCOMPARE(caption->x(),captionLeft);QVERIFY(caption->x()+caption->width()<=valueLeft);QCOMPARE(caption->y(),value->y());
  }
  QCOMPARE(card->findChild<QLabel*>("build_gitTreeCaption")->text(),QString("Git tree"));
  // The whole card is visible unless the screen is shorter than the dialog's readable maximum.
  auto* scroll=dialog.findChild<QScrollArea*>("aboutBuildScroll");QVERIFY(scroll);if(dialog.height()<dialog.maximumFitHeight())QCOMPARE(scroll->verticalScrollBar()->maximum(),0);
  for(auto* name:{"copyBuildDetails","buildDetails","closeAbout"}){auto* button=dialog.findChild<QAbstractButton*>(name);QVERIFY(dialog.rect().contains(QRect(button->mapTo(&dialog,QPoint()),button->size())));}
 }
 void unknownBuildFactsUseSecondaryProse(){
  AboutWindow dialog({{"version","26.1028.1"},{"sourceProvenance","unknown"}});dialog.show();dialog.findChild<QToolButton*>("buildDetails")->click();QApplication::processEvents();for(const auto& name:{"buildCommit","buildState","build_sourceProvenance"}){auto* value=dialog.findChild<QTextEdit*>(name);QVERIFY(value);QCOMPARE(value->toPlainText(),QString("Unknown"));QCOMPARE(value->property("visualRole").toString(),QString("secondary"));QCOMPARE(value->font().family(),Graphite::uiFamily());}
 }
 void disclosureLabelsFitAtLargeText() {
  VisualStyle::apply(*qApp);const auto oldFont=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(oldFont);});auto font=oldFont;font.setPixelSize(21);qApp->setFont(font);
  AboutWindow dialog({{"version","26.1027.1543-dev+g0123456789ab"}});dialog.show();QApplication::processEvents();for(const auto* name:{"qt_msgbox_label","qt_msgbox_informativelabel","aboutVersion"}){auto* text=dialog.findChild<QLabel*>(name);QVERIFY(text);QVERIFY2(text->height()>=text->fontMetrics().height(),qPrintable(QString("%1: %2 < %3").arg(name).arg(text->height()).arg(text->fontMetrics().height())));}auto* details=dialog.findChild<QToolButton*>("buildDetails");QVERIFY(details);
  for(int i=0;i<2;++i){details->click();QApplication::processEvents();const int padding=details->iconSize().width()+Graphite::space(Graphite::Space::s);QCOMPARE(details->fontMetrics().elidedText(details->text(),Qt::ElideRight,details->width()-padding),details->text());}
 }

 void shortVersionIsSelectableAndFullBuildStaysInDetails(){
  VisualStyle::apply(*qApp);const QString identity="26.1027.1544-dev+g0123456789abcdef0123456789abcdef.dirty";AboutWindow dialog({{"version",identity}});dialog.show();QApplication::processEvents();auto* version=dialog.findChild<QLabel*>("aboutVersion");auto* tagline=dialog.findChild<QLabel*>("qt_msgbox_informativelabel");auto* website=dialog.findChild<QPushButton*>("putgdWebsite");QVERIFY(version&&tagline&&website);QVERIFY(!website->icon().isNull());QCOMPARE(website->property("visualRole").toString(),QString("link"));QCOMPARE(version->text(),QString("26.1027.1544"));QCOMPARE(version->toolTip(),identity);QVERIFY(dialog.detailedText().contains(identity));QVERIFY(!version->wordWrap());QVERIFY(version->textInteractionFlags().testFlag(Qt::TextSelectableByKeyboard));QVERIFY(version->textInteractionFlags().testFlag(Qt::TextSelectableByMouse));QVERIFY(version->height()>=version->heightForWidth(version->width()));QVERIFY(dialog.width()>=Graphite::scaled(380));QVERIFY(website->mapTo(&dialog,QPoint()).y()>tagline->mapTo(&dialog,QPoint()).y());QVERIFY(version->mapTo(&dialog,QPoint()).y()>tagline->mapTo(&dialog,QPoint()).y());QCOMPARE(dialog.findChild<QPushButton*>("closeAbout")->property("visualRole").toString(),QString("lead"));QCOMPARE(dialog.findChild<QToolButton*>("buildDetails")->property("visualRole").toString(),QString("quiet"));QVERIFY(dialog.findChild<QPushButton*>("closeAbout")->isDefault());
 }

 void aboutFollowsItsOwnerLifetime() {
  auto* owner = new QWidget;
  QPointer<AboutWindow> dialog = new AboutWindow({{"version", "0.1.1"}}, owner);
  dialog->show();
  QVERIFY(dialog->isVisible());
  delete owner;
  QVERIFY(dialog.isNull());
 }
};

QTEST_MAIN(VersionView)
#include "test_version.moc"
