#include "core/BuildInfo.h"
#include "ui/AboutWindow.h"
#include <QApplication>
#include <QJsonDocument>
#include <QPointer>
#include <QPushButton>
#include <QTextEdit>
#include <QTest>
#include <QTimer>
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
  manifest["commit"] = "0123456789abcdef0123456789abcdef01234567";
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
  QCOMPARE(dialog->text(), QString("put.gd %1").arg(manifest["version"].toString()));
  QCOMPARE(QJsonDocument::fromJson(dialog->detailedText().toUtf8()).object(), manifest);
  QVERIFY(dialog->button(QMessageBox::Close));QVERIFY(!dialog->button(QMessageBox::Ok));auto* details=dialog->findChild<QPushButton*>("buildDetails");QVERIFY(details);QCOMPARE(details->text(),QString("Build details"));details->click();QCOMPARE(details->text(),QString("Hide build details"));auto* data=dialog->findChild<QTextEdit*>();QVERIFY(data&&data->isVisible()&&data->isReadOnly());data->selectAll();QCOMPARE(QJsonDocument::fromJson(data->textCursor().selectedText().replace(QChar::ParagraphSeparator,'\n').toUtf8()).object(),manifest);details->click();QCOMPARE(details->text(),QString("Build details"));
  QCOMPARE(dialog->informativeText(), QString("Screen capture and file sharing"));
  QVERIFY(!dialog->testAttribute(Qt::WA_DeleteOnClose));
  dialog->close();
  QVERIFY(dialog);
  QVERIFY(!dialog->isVisible());
  dialog->show();details->click();QCOMPARE(details->text(),QString("Hide build details"));QVERIFY(data->isVisible());details->click();QCOMPARE(details->text(),QString("Build details"));QVERIFY(!data->isVisible());dialog->close();
  owner.reset();
  QVERIFY(dialog.isNull());
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
