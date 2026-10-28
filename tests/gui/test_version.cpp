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
#include <QStyle>
#include <QTextEdit>
#include <QScrollArea>
#include <QScrollBar>
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
  auto* details=dialog->findChild<QPushButton*>("buildDetails");auto* rows=dialog->findChild<QWidget*>("aboutBuildDetails");auto* copy=dialog->findChild<QPushButton*>("copyBuildDetails");QVERIFY(details&&rows&&copy);QVERIFY(!rows->isVisible());QVERIFY(!copy->isVisible());details->click();QApplication::processEvents();QVERIFY(rows->isVisible());QVERIFY(copy->isVisible());
  auto* commit=dialog->findChild<QTextEdit*>("buildCommit");QVERIFY(commit);QCOMPARE(commit->toPlainText(),manifest["gitCommit"].toString());QCOMPARE(commit->wordWrapMode(),QTextOption::WrapAnywhere);QVERIFY(commit->isReadOnly());QCOMPARE(commit->horizontalScrollBar()->maximum(),0);commit->selectAll();commit->copy();QCOMPARE(qApp->clipboard()->text(),manifest["gitCommit"].toString());copy->click();QCOMPARE(QJsonDocument::fromJson(qApp->clipboard()->text().toUtf8()).object(),manifest);details->click();
  QVERIFY(!dialog->testAttribute(Qt::WA_DeleteOnClose));
  dialog->close();
  QVERIFY(dialog);
  QVERIFY(!dialog->isVisible());
  dialog->show();details->click();QCOMPARE(details->text(),QString("Hide build details"));QVERIFY(rows->isVisible());details->click();QCOMPARE(details->text(),QString("Build details"));QVERIFY(!rows->isVisible());dialog->close();
  owner.reset();
  QVERIFY(dialog.isNull());
 }

 void aboutUsesTheSharedSpacingAndTypeRoles() {
  VisualStyle::apply(*qApp);AboutWindow dialog({{"version","26.1027.1543-dev"}});dialog.show();QApplication::processEvents();
  auto* content=dialog.findChild<QWidget*>("aboutContent");auto* buttons=dialog.findChild<QWidget*>("aboutActions");auto* icon=dialog.findChild<QLabel*>("qt_msgboxex_icon_label");auto* tagline=dialog.findChild<QLabel*>("qt_msgbox_informativelabel");auto* version=dialog.findChild<QLabel*>("aboutVersion");auto* website=dialog.findChild<QPushButton*>("putgdWebsite");QVERIFY(content&&buttons&&icon&&tagline&&version&&website);for(auto* text:{dialog.findChild<QLabel*>("qt_msgbox_label"),tagline,version})QVERIFY2(text->height()>=text->fontMetrics().height(),qPrintable(text->objectName()));
  const int margin=Graphite::space(Graphite::Space::xl);QCOMPARE(dialog.layout()->contentsMargins(),QMargins(margin,margin,margin,margin));QCOMPARE(content->mapTo(&dialog,QPoint()),QPoint(margin,margin));QVERIFY(icon->width()>=Graphite::scaled(64));QVERIFY(icon->height()>=Graphite::scaled(64));QCOMPARE(tagline->font().pixelSize(),Graphite::scaled(13));QCOMPARE(tagline->palette().color(QPalette::WindowText),Theme::current().textSecondary);QCOMPARE(version->palette().color(QPalette::WindowText),Theme::current().textSecondary);
  QVERIFY(version->mapTo(&dialog,QPoint()).y()<website->mapTo(&dialog,QPoint()).y());QCOMPARE(buttons->mapTo(&dialog,QPoint()).y()-content->mapTo(&dialog,QPoint(0,content->height())).y(),margin);QCOMPARE(buttons->mapTo(&dialog,QPoint(buttons->width(),0)).x(),dialog.width()-margin);
 }

 void disclosureLabelsFitAtLargeText() {
  VisualStyle::apply(*qApp);const auto oldFont=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(oldFont);});auto font=oldFont;font.setPixelSize(21);qApp->setFont(font);
  AboutWindow dialog({{"version","26.1027.1543-dev+g0123456789ab"}});dialog.show();QApplication::processEvents();for(const auto* name:{"qt_msgbox_label","qt_msgbox_informativelabel","aboutVersion"}){auto* text=dialog.findChild<QLabel*>(name);QVERIFY(text);QVERIFY2(text->height()>=text->fontMetrics().height(),qPrintable(QString("%1: %2 < %3").arg(name).arg(text->height()).arg(text->fontMetrics().height())));}auto* details=dialog.findChild<QPushButton*>("buildDetails");QVERIFY(details);
  for(int i=0;i<2;++i){details->click();QApplication::processEvents();const int padding=2*details->style()->pixelMetric(QStyle::PM_ButtonMargin,nullptr,details);QCOMPARE(details->fontMetrics().elidedText(details->text(),Qt::ElideRight,details->width()-padding),details->text());}
 }

 void shortVersionIsSelectableAndFullBuildStaysInDetails(){
  VisualStyle::apply(*qApp);const QString identity="26.1027.1544-dev+g0123456789abcdef0123456789abcdef.dirty";AboutWindow dialog({{"version",identity}});dialog.show();QApplication::processEvents();auto* version=dialog.findChild<QLabel*>("aboutVersion");auto* tagline=dialog.findChild<QLabel*>("qt_msgbox_informativelabel");auto* website=dialog.findChild<QPushButton*>("putgdWebsite");QVERIFY(version&&tagline&&website);QVERIFY(!website->icon().isNull());QCOMPARE(website->property("visualRole").toString(),QString("link"));QCOMPARE(version->text(),QString("26.1027.1544"));QCOMPARE(version->toolTip(),identity);QVERIFY(dialog.detailedText().contains(identity));QVERIFY(!version->wordWrap());QVERIFY(version->textInteractionFlags().testFlag(Qt::TextSelectableByKeyboard));QVERIFY(version->textInteractionFlags().testFlag(Qt::TextSelectableByMouse));QVERIFY(version->height()>=version->heightForWidth(version->width()));QVERIFY(dialog.width()>=Graphite::scaled(380));QVERIFY(website->mapTo(&dialog,QPoint()).y()>tagline->mapTo(&dialog,QPoint()).y());QVERIFY(version->mapTo(&dialog,QPoint()).y()>tagline->mapTo(&dialog,QPoint()).y());QCOMPARE(dialog.findChild<QPushButton*>("closeAbout")->property("visualRole").toString(),QString("lead"));QCOMPARE(dialog.findChild<QPushButton*>("buildDetails")->property("visualRole").toString(),QString("quiet"));QVERIFY(dialog.findChild<QPushButton*>("closeAbout")->isDefault());
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
