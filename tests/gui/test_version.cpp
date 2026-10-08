#include "core/BuildInfo.h"
#include "fixtures/U2Fixtures.h"
#include "ui/TrayController.h"
#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QTimer>
using namespace putgd;
using namespace putgd::test;
class VersionView final : public QObject {
 Q_OBJECT
private slots:
 void aboutIsModelessAndReleasedWithoutOpeningOtherViews() {
  QTemporaryDir directory;QVERIFY(directory.isValid());ProviderRegistry providers;
  SettingsService settings(directory.filePath("config.json"));Source source;
  OperationService operations(directory.filePath("data"),settings,providers,source);
  auto tray=std::make_unique<TrayController>(operations,[]()->SettingsWindow&{qFatal("About opened settings");},
    []()->HistoryWindow&{qFatal("About opened history");},[]()->RecentOperations&{qFatal("About opened activity");});
  QAction* about=nullptr;
  for(auto* widget:QApplication::topLevelWidgets())if(auto* menu=qobject_cast<QMenu*>(widget))
   for(auto* action:menu->actions())if(action->text()=="About put.gd")about=action;
  QVERIFY(about);
  const auto dialogs=[] {
   QList<QMessageBox*> result;
   for(auto* widget:QApplication::topLevelWidgets())
    if(auto* dialog=qobject_cast<QMessageBox*>(widget))result.append(dialog);
   return result;
  };
  QVERIFY(dialogs().isEmpty());
  // Close a regressed modal dialog from its nested event loop so this test fails
  // promptly instead of waiting for a person or the test timeout.
  bool enteredModalLoop=false;
  QTimer modalGuard;modalGuard.setSingleShot(true);
  connect(&modalGuard,&QTimer::timeout,this,[&]{
   if(auto* dialog=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())){
    enteredModalLoop=true;dialog->accept();
   }
  });
  modalGuard.start(0);about->trigger();QApplication::processEvents();modalGuard.stop();
  QVERIFY2(!enteredModalLoop,"About must not block capture input with a modal event loop");
  QCOMPARE(dialogs().size(),1);
  QPointer<QMessageBox> dialog=dialogs().first();
  QVERIFY(dialog->isVisible());QVERIFY(!dialog->isModal());
  QCOMPARE(dialog->windowModality(),Qt::NonModal);
  QVERIFY(!QApplication::activeModalWidget());
  QCOMPARE(dialog->text(),QString("put.gd %1").arg(buildVersion()));
  QCOMPARE(QJsonDocument::fromJson(dialog->detailedText().toUtf8()).object(),
    QJsonDocument::fromJson(buildManifestJson()).object());
  about->trigger();QCOMPARE(dialogs().size(),1);QCOMPARE(dialogs().first(),dialog.data());
  dialog->close();QTRY_VERIFY(dialog.isNull());QVERIFY(dialogs().isEmpty());
  about->trigger();QCOMPARE(dialogs().size(),1);dialog=dialogs().first();
  QVERIFY(dialog->isVisible());
  tray.reset();QVERIFY(dialog.isNull());QVERIFY(dialogs().isEmpty());
  QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
 }
};
QTEST_MAIN(VersionView)
#include "test_version.moc"
