#include "app/LocalServer.h"
#include "app/UiControl.h"
#include "core/BuildInfo.h"
#include "platform/x11/X11Clipboard.h"
#include "ui/ViewState.h"
#include "fixtures/U2Fixtures.h"
#include <QApplication>
#include <QClipboard>
#include <QJsonDocument>
#include <QProcess>
#include <QSet>
#include <xcb/xcb.h>
#include <cstdlib>
#include <cstring>

using namespace putgd;
using namespace putgd::test;

namespace {
// The parent supplies the documented private protocol, while the child is the
// actual product executable. This is isolated X11 protocol evidence, not proof
// of a desktop compositor, native theme, tray, or window-manager integration.
class ProductHelper final : public QObject {
public:
 explicit ProductHelper(LocalServer& server, const QJsonObject& restored = {})
  : control(process) {
  process.setProcessChannelMode(QProcess::SeparateChannels);
  connect(&process, &QProcess::readyReadStandardError, this, [this] {
   stderrTail += process.readAllStandardError();
   if (stderrTail.size() > 32768) stderrTail = stderrTail.last(32768);
  });
  connect(&control, &UiControl::failed, this, [this](Error error) { failure = error; });
  connect(&control, &UiControl::frame, this,
   [this](const QString& type, const QString& sequence, const QJsonObject& payload) {
    if (type == "ready") ready = true;
    else if (type == "shown") shown.insert(payload["view"].toString());
    else if (type == "checkpoint") {
     const auto state = payload["state"].toObject();
     if (const auto error = validateUiState(state)) { failure = error; return; }
     latestState = state;
     control.send("checkpointAccepted", {{"replyTo", sequence}});
    } else if (type == "closeRequested") {
     closeSequence = sequence;
     closeState = payload["state"].toObject();
    } else if (type == "closeConfirmed") {
     confirmed = payload["replyTo"] == closeSequence;
    } else if (type == "incompatible" || type == "activationFailed") {
     failure = {"helper_rejected", type, {}};
    }
   });
  connect(&process, &QProcess::started, this, [this, &server, restored] {
   control.send("bootstrap", {{"socketPath", server.socketPath()},
    {"residentEpoch", server.residentEpoch()},
    {"build", QJsonDocument::fromJson(buildManifestJson()).object()},
    {"state", restored.isEmpty() ? QJsonObject{{"schemaVersion", 1}} : restored}});
  });
  process.start(QString::fromUtf8(PUTGD_PRODUCT_UI_PATH), {});
 }
 ~ProductHelper() override {
  control.stop();
  if (process.state() != QProcess::NotRunning) {
   process.kill();
   process.waitForFinished(3000);
  }
 }
 void activate(const QString& name) { control.send("activate", {{"view", name}}); }
 void acceptClose() { control.send("closeAccepted", {{"replyTo", closeSequence}}); }
 QProcess process;
 UiControl control;
 bool ready = false, confirmed = false;
 QSet<QString> shown;
 QJsonObject latestState, closeState;
 QString closeSequence;
 QByteArray stderrTail;
 Error failure;
};

class XWindows final {
public:
 XWindows() : connection(xcb_connect(nullptr, nullptr)) {
  if (!xcb_connection_has_error(connection))
   root = xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root;
 }
 ~XWindows() { xcb_disconnect(connection); }
 bool available() const { return root && !xcb_connection_has_error(connection); }
 xcb_atom_t atom(const char* name) {
  const auto cookie = xcb_intern_atom(connection, 0, std::strlen(name), name);
  auto* reply = xcb_intern_atom_reply(connection, cookie, nullptr);
  const auto result = reply ? reply->atom : xcb_atom_t(XCB_ATOM_NONE);
  std::free(reply);
  return result;
 }
 QByteArray property(xcb_window_t window, const char* name, xcb_atom_t type) {
  auto* reply = xcb_get_property_reply(connection,
   xcb_get_property(connection, 0, window, atom(name), type, 0, 4096), nullptr);
  QByteArray bytes;
  if (reply) bytes = QByteArray(static_cast<const char*>(xcb_get_property_value(reply)),
                               xcb_get_property_value_length(reply));
  std::free(reply);
  return bytes;
 }
 bool mapped(xcb_window_t window) {
  auto* reply = xcb_get_window_attributes_reply(connection,
   xcb_get_window_attributes(connection, window), nullptr);
  const bool visible = reply && reply->map_state == XCB_MAP_STATE_VIEWABLE;
  std::free(reply);
  return visible;
 }
 xcb_window_t find(qint64 pid, const QString& title) {
  auto* tree = xcb_query_tree_reply(connection, xcb_query_tree(connection, root), nullptr);
  if (!tree) return XCB_WINDOW_NONE;
  xcb_window_t found = XCB_WINDOW_NONE;
  const auto* children = xcb_query_tree_children(tree);
  for (int i = 0; i < xcb_query_tree_children_length(tree); ++i) {
   const auto owner = property(children[i], "_NET_WM_PID", XCB_ATOM_CARDINAL);
   quint32 value = 0;
   if (owner.size() == sizeof(value)) std::memcpy(&value, owner.constData(), sizeof(value));
   if (value == pid && mapped(children[i]) &&
       QString::fromUtf8(property(children[i], "_NET_WM_NAME", atom("UTF8_STRING"))) == title) {
    found = children[i];
    break;
   }
  }
  std::free(tree);
  return found;
 }
 bool close(xcb_window_t window) {
  const auto deleteWindow = atom("WM_DELETE_WINDOW");
  const auto protocols = property(window, "WM_PROTOCOLS", XCB_ATOM_ATOM);
  bool supported = false;
  constexpr qsizetype atomBytes = sizeof(xcb_atom_t);
  for (qsizetype offset = 0; offset + atomBytes <= protocols.size(); offset += atomBytes) {
   xcb_atom_t value;
   std::memcpy(&value, protocols.constData() + offset, sizeof(value));
   supported |= value == deleteWindow;
  }
  if (!supported) return false;
  xcb_client_message_event_t event{};
  event.response_type = XCB_CLIENT_MESSAGE;
  event.format = 32;
  event.window = window;
  event.type = atom("WM_PROTOCOLS");
  event.data.data32[0] = deleteWindow;
  event.data.data32[1] = XCB_CURRENT_TIME;
  auto* error = xcb_request_check(connection, xcb_send_event_checked(connection, 0,
   window, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char*>(&event)));
  const bool sent = !error;
  std::free(error);
  xcb_flush(connection);
  return sent;
 }
 xcb_window_t clipboardOwner() {
  auto* reply = xcb_get_selection_owner_reply(connection,
   xcb_get_selection_owner(connection, atom("CLIPBOARD")), nullptr);
  const auto owner = reply ? reply->owner : xcb_window_t(XCB_WINDOW_NONE);
  std::free(reply);
  return owner;
 }
 xcb_connection_t* connection = nullptr;
 xcb_window_t root = XCB_WINDOW_NONE;
};
}

class UiNativeLifecycleTest : public QObject {
 Q_OBJECT
private slots:
 void normalCloseCheckpointsDirtyDraftAndRelaunchesCleanly() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  SettingsService settings(dir.filePath("config.json"));
  QVERIFY(settings.save(configuration(dir.path(), true), {}));
  const auto savedDocument = settings.document();
  ProviderRegistry providers;
  Source source;
  OperationService operations(dir.filePath("data"), settings, providers, source);
  HistoryService history(operations);
  LocalServer server(settings, operations, history);
  QVERIFY(server.listen(dir.filePath("runtime")));
  QTRY_VERIFY(operations.ready());
  XWindows windows;
  QVERIFY(windows.available());

  QJsonObject retained;
  qint64 previousPid = 0;
  for (int cycle = 0; cycle < 4; ++cycle) {
   ProductHelper helper(server, retained);
   QTRY_VERIFY_WITH_TIMEOUT(helper.ready || helper.failure, 10000);
   QVERIFY2(!helper.failure, qPrintable(helper.failure.code));
   const auto pid = helper.process.processId();
   QVERIFY(pid > 0 && pid != previousPid);
   previousPid = pid;
   helper.activate("settings");
   QTRY_VERIFY(helper.shown.contains("settings"));
   xcb_window_t window = XCB_WINDOW_NONE;
   QTRY_VERIFY((window = windows.find(pid, "put.gd — Settings")) != XCB_WINDOW_NONE);
   QVERIFY(windows.close(window));
   QTRY_VERIFY_WITH_TIMEOUT(!helper.closeSequence.isEmpty() || helper.failure, 5000);
   QVERIFY2(!helper.failure, qPrintable(helper.failure.code));
   QVERIFY(!validateUiState(helper.closeState));
   QCOMPARE(helper.process.state(), QProcess::Running);
   QVERIFY(windows.mapped(window)); // The last window stays until the checkpoint is accepted.
   if (cycle) {
    const auto state = helper.closeState["settings"].toObject();
    QCOMPARE(state["draft"].toObject()["archive"].toObject()["root"].toString(), dir.filePath("unsaved-draft"));
    QCOMPARE(state["numbers"].toObject()["maxFiles"].toString(), QString("unfinished number"));
    QVERIFY(state["dirty"].toBool());
    QVERIFY(state["advanced"].toBool());
    QCOMPARE(state["section"].toInt(), 0);
   }
   retained = helper.closeState;
   helper.acceptClose();
   QTRY_VERIFY(helper.confirmed);
   QTRY_COMPARE_WITH_TIMEOUT(helper.process.state(), QProcess::NotRunning, 10000);
   QCOMPARE(helper.process.exitStatus(), QProcess::NormalExit);
   QVERIFY2(helper.process.exitCode() == 0, helper.stderrTail.constData());
   QVERIFY2(!helper.failure, qPrintable(helper.failure.code));
   QVERIFY(!windows.mapped(window));
   QCOMPARE(settings.document(), savedDocument); // Closing never saves the draft.

   if (!cycle) {
    // Supply a nonsecret, unfinished draft through the same checkpoint boundary
    // the resident uses. The real helper must restore and re-export it on every
    // normal close; input-widget editing itself is covered by gui_settings.
    auto state = retained["settings"].toObject();
    auto draft = state["draft"].toObject();
    auto archive = draft["archive"].toObject();
    archive["root"] = dir.filePath("unsaved-draft");
    draft["archive"] = archive;
    state["draft"] = draft;
    auto numbers = state["numbers"].toObject();
    numbers["maxFiles"] = "unfinished number";
    state["numbers"] = numbers;
    state["dirty"] = true;
    state["advanced"] = true;
    state["section"] = 0;
    retained["settings"] = state;
    QVERIFY(!validateUiState(retained));
   }
  }
 }

 void closingAllViewsPreservesAcceptedWorkAndResidentClipboard() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  SettingsService settings(dir.filePath("config.json"));
  QVERIFY(settings.save(configuration(dir.path(), true), {}));
  ProviderRegistry providers;
  Source source;
  source.defer = true;
  OperationService operations(dir.filePath("data"), settings, providers, source);
  HistoryService history(operations);
  LocalServer server(settings, operations, history);
  QVERIFY(server.listen(dir.filePath("runtime")));
  QTRY_VERIFY(operations.ready());
  XWindows windows;
  QVERIFY(windows.available());
  ProductHelper helper(server);
  QTRY_VERIFY_WITH_TIMEOUT(helper.ready || helper.failure, 10000);
  QVERIFY2(!helper.failure, qPrintable(helper.failure.code));

  const QString clipboard = "https://example.invalid/synthetic-resident-result";
  QGuiApplication::clipboard()->setText(clipboard);
  const auto owner = windows.clipboardOwner();
  QVERIFY(owner != XCB_WINDOW_NONE);
  QString operationId;
  operations.invoke("clipboard", [&](Error error, QJsonObject accepted) {
   QVERIFY(!error);
   operationId = accepted["operationId"].toString();
  });
  QTRY_VERIFY(!operationId.isEmpty());
  QTRY_COMPARE(source.acquisitions.size(), 1);

  const QList<QPair<QString, QString>> views{{"settings", "put.gd — Settings"},
   {"history", "put.gd — History"}, {"activity", "put.gd — Activity"}, {"about", "About put.gd"}};
  QList<xcb_window_t> shown;
  for (const auto& [view, title] : views) {
   helper.activate(view);
   QTRY_VERIFY(helper.shown.contains(view));
   xcb_window_t window = XCB_WINDOW_NONE;
   QTRY_VERIFY((window = windows.find(helper.process.processId(), title)) != XCB_WINDOW_NONE);
   shown.append(window);
  }
  for (qsizetype i = 0; i < shown.size() - 1; ++i) {
   QVERIFY(windows.close(shown[i]));
   QTRY_VERIFY(!windows.mapped(shown[i]));
   QCOMPARE(helper.process.state(), QProcess::Running);
   QVERIFY(helper.closeSequence.isEmpty());
   QCOMPARE(source.acquisitions.size(), 1);
  }
  QVERIFY(windows.close(shown.last()));
  QTRY_VERIFY(!helper.closeSequence.isEmpty());
  QVERIFY(!validateUiState(helper.closeState));
  helper.acceptClose();
  QTRY_VERIFY(helper.confirmed);
  QTRY_COMPARE_WITH_TIMEOUT(helper.process.state(), QProcess::NotRunning, 10000);
  QCOMPARE(helper.process.exitStatus(), QProcess::NormalExit);
  QVERIFY2(helper.process.exitCode() == 0, helper.stderrTail.constData());
  QVERIFY2(!helper.failure, qPrintable(helper.failure.code));

  QCOMPARE(windows.clipboardOwner(), owner);
  X11Clipboard independentReader;
  QVERIFY(independentReader.available());
  PayloadResult result;
  bool read = false;
  independentReader.acquire({}, [&](PayloadResult payload) { result = std::move(payload); read = true; });
  QTRY_VERIFY(read);
  QVERIFY2(!result.error, qPrintable(result.error.code));
  QCOMPARE(result.payload.bytes, clipboard.toUtf8());
  QCOMPARE(source.acquisitions.size(), 1);
  source.acquisitions.takeFirst()({textPayload(), {}});
  QTRY_COMPARE(source.copies, 1);
  QJsonObject completed;
  operations.inspect(operationId, [&](Error error, QJsonObject value) {
   QVERIFY(!error);
   completed = value;
  });
  QTRY_VERIFY(!completed.isEmpty());
  QCOMPARE(completed["state"].toString(), QString("completed"));
  QCOMPARE(source.reads, 1);
  QCOMPARE(source.copied.kind, QString("text"));
  QCOMPARE(source.copied.text.toUtf8(), textPayload().bytes);
 }
};
QTEST_MAIN(UiNativeLifecycleTest)
#include "test_ui_native_lifecycle.moc"
