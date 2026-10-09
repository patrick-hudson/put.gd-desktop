#include "fixtures/U2Fixtures.h"
#include "ui/TrayController.h"
#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QPointer>
#include <QSystemTrayIcon>

using namespace putgd;
using namespace putgd::test;

namespace {
QAction* actionNamed(QSystemTrayIcon* tray, const QString& text) {
 if (!tray || !tray->contextMenu()) return nullptr;
 for (auto* action : tray->contextMenu()->actions())
  if (action->text() == text) return action;
 return nullptr;
}

class CaptureSource final : public SourceAdapter {
public:
 QStringList actions;
 int copies = 0;
 quint64 clipboardRevision() const override { return 0; }
 QJsonObject capabilities() const override { return {}; }
 void acquire(const QString& action, const ResourceLimits&, std::function<void(PayloadResult)> done) override {
  actions.append(action);
  done({{}, {"source_cancelled", "Synthetic selection cancelled", {}}});
 }
 void cancelAcquisition() override {}
 void copy(const CopyPayload&, std::function<void(Error)> done) override { ++copies; done({}); }
};
}

class ResidentTray final : public QObject {
 Q_OBJECT
private slots:
 void delegatesEveryViewAndExplicitQuitWithoutConstructingViews() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  ProviderRegistry providers;
  SettingsService settings(directory.filePath("config.json"));
  CaptureSource source;
  OperationService operations(directory.filePath("data"), settings, providers, source);
  QStringList views;
  int quits = 0;
  auto controller = std::make_unique<TrayController>(operations,
    [&](const QString& view) { views.append(view); }, [&] { ++quits; });
  QPointer<QSystemTrayIcon> tray = controller->findChild<QSystemTrayIcon*>();
  QVERIFY(tray);
  QVERIFY(!tray->icon().isNull());
  QPointer<QMenu> menu = tray->contextMenu();
  QVERIFY(menu);
  const auto windows = QApplication::topLevelWidgets();
  for (const auto& name : QStringList{"Settings and setup", "Upload history", "Recent activity", "About put.gd"}) {
   auto* action = actionNamed(tray, name);
   QVERIFY(action);
   action->trigger();
  }
  QCOMPARE(views, QStringList({"settings", "history", "activity", "about"}));
  QCOMPARE(QApplication::topLevelWidgets(), windows);
  QCOMPARE(quits, 0);
  controller->activate();
  QCOMPARE(views.last(), QString("settings"));
  QVERIFY(QMetaObject::invokeMethod(tray, "activated", Qt::DirectConnection,
                                   Q_ARG(QSystemTrayIcon::ActivationReason, QSystemTrayIcon::Trigger)));
  QCOMPARE(views.last(), QString("activity"));
  auto* quit = actionNamed(tray, "Quit");
  QVERIFY(quit);
  quit->trigger();
  QCOMPARE(quits, 1);
  QVERIFY(tray);
  QCOMPARE(source.actions.size(), 0);
  QCOMPARE(source.copies, 0);
  controller.reset();
  QVERIFY(tray.isNull());
  QVERIFY(menu.isNull());
 }

 void everyCaptureActionWorksWithNoViewsOpen() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  ProviderRegistry providers;
  SettingsService settings(directory.filePath("config.json"));
  QVERIFY(settings.save(configuration(directory.path(), true), {}));
  CaptureSource source;
  OperationService operations(directory.filePath("data"), settings, providers, source);
  QTRY_VERIFY(operations.ready());
  QStringList views;
  TrayController controller(operations, [&](const QString& view) { views.append(view); }, [] {});
  auto* tray = controller.findChild<QSystemTrayIcon*>();
  QVERIFY(tray);
  QSet<QString> terminalActions;
  connect(&operations, &OperationService::changed, this, [&](const QString& id) {
   operations.inspect(id, [&](Error error, QJsonObject operation) {
    QVERIFY(!error);
    if (operation["state"] == "cancelled") terminalActions.insert(operation["action"].toString());
   });
  });
  for (const auto& name : QStringList{"region", "window", "desktop", "clipboard"}) {
   auto* action = actionNamed(tray, "Capture " + name);
   QVERIFY(action);
   action->trigger();
   QTRY_VERIFY(source.actions.contains(name));
   // The acquisition callback is processed asynchronously by the resident.
   // Wait for its terminal event before beginning another screen selection.
   QTRY_VERIFY(terminalActions.contains(name));
  }
  QCOMPARE(source.actions, QStringList({"region", "window", "desktop", "clipboard"}));
  QVERIFY(views.isEmpty());
  QCOMPARE(source.copies, 0);
 }

 void rejectedCaptureCallbackCanDestroyItsTray() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  ProviderRegistry providers;
  SettingsService settings(directory.filePath("config.json"));
  CaptureSource source;
  OperationService operations(directory.filePath("data"), settings, providers, source);
  QTRY_VERIFY(operations.ready());
  std::unique_ptr<TrayController> controller;
  QString opened;
  controller = std::make_unique<TrayController>(operations, [&](const QString& view) {
   opened = view;
   controller.reset();
  }, [] {});
  auto* tray = controller->findChild<QSystemTrayIcon*>();
  QPointer<QAction> action = actionNamed(tray, "Capture region");
  QVERIFY(action);
  action->trigger();
  QCOMPARE(opened, QString("activity"));
  QVERIFY(!controller);
  QVERIFY(action.isNull());
 }

 void rejectedCaptureOpensActivityWithoutOwningAWindow() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  ProviderRegistry providers;
  SettingsService settings(directory.filePath("config.json"));
  CaptureSource source;
  OperationService operations(directory.filePath("data"), settings, providers, source);
  QTRY_VERIFY(operations.ready());
  QStringList views;
  TrayController controller(operations, [&](const QString& view) { views.append(view); }, [] {});
  auto* tray = controller.findChild<QSystemTrayIcon*>();
  auto* action = actionNamed(tray, "Capture region");
  QVERIFY(action);
  action->trigger();
  QTRY_COMPARE(views, QStringList({"activity"}));
  QVERIFY(source.actions.isEmpty());
 }
};

QTEST_MAIN(ResidentTray)
#include "test_tray.moc"
