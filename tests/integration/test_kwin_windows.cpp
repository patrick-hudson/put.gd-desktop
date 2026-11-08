// #1278: the KDE window list for window recordings. KWin has no D-Bus call that
// lists windows, so put.gd loads a one-shot KWin script that calls it back. A
// fake KWin on a private session bus (dbus-run-session) loads the script the
// way KWin's org.kde.kwin.Scripting does and answers through callDBus's call;
// only the bus owner of org.kde.KWin is believed. Never desktop proof.
#include "platform/kde/KWinWindows.h"
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QtTest>
using namespace putgd;

namespace {
QJsonObject raw(const QString& identity, qint64 pid, const QString& resourceClass, bool active) {
    return {{"identity", identity}, {"title", resourceClass + " window"}, {"desktopFile", resourceClass == "konsole" ? QString("org.kde.konsole") : QString()}, {"resourceClass", resourceClass},
            {"pid", double(pid)}, {"rect", QJsonArray{10, 20, 800, 600}}, {"active", active}, {"minimized", false}};
}
// org.kde.kwin.Script on /Scripting/Script<N>: run() executes the script's one callDBus.
class FakeScript final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kwin.Script")
public:
    FakeScript(QDBusConnection bus, QString path, QJsonArray windows, bool silent) : bus_(bus), path_(std::move(path)), windows_(std::move(windows)), silent_(silent) {}
public slots:
    void run() {
        QFile file(path_);
        if (silent_) return;
        if (!file.open(QIODevice::ReadOnly)) return;
        const auto script = QString::fromUtf8(file.readAll());
        const auto call = QRegularExpression(R"(callDBus\("([^"]+)", "([^"]+)", "org\.putgd\.KWinWindows", "Windows", "([^"]+)")").match(script);
        if (!call.hasMatch()) return;
        auto message = QDBusMessage::createMethodCall(call.captured(1), call.captured(2), "org.putgd.KWinWindows", "Windows");
        message << call.captured(3) << QString::fromUtf8(QJsonDocument(windows_).toJson(QJsonDocument::Compact));
        bus_.asyncCall(message);
    }
private:
    QDBusConnection bus_;
    QString path_;
    QJsonArray windows_;
    bool silent_ = false;
};
class FakeKWin final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kwin.Scripting")
public:
    explicit FakeKWin(QDBusConnection bus) : bus_(bus) {}
    QJsonArray windows;
    bool silent = false;
    QStringList loaded, unloaded;
public slots:
    int loadScript(const QString& path, const QString& plugin) {
        loaded << plugin;
        auto* script = new FakeScript(bus_, path, windows, silent);
        script->setParent(this);
        const int id = loaded.size();
        bus_.registerObject(QStringLiteral("/Scripting/Script%1").arg(id), script, QDBusConnection::ExportAllSlots);
        return id;
    }
    bool unloadScript(const QString& plugin) { unloaded << plugin; return true; }
private:
    QDBusConnection bus_;
};
}

class KWinWindowsContract : public QObject {
    Q_OBJECT
private slots:
    void leavesOutPutgdsOwnWindows() {
        const QJsonArray listed{raw("{11111111-1111-1111-1111-111111111111}", 4000, "konsole", false), raw("{22222222-2222-2222-2222-222222222222}", 1000, "putgd-ui", true),
                                raw("not-a-uuid", 4001, "firefox", false), raw("{33333333-3333-3333-3333-333333333333}", 4002, "firefox", false)};
        const auto result = kwinRecordingWindows(listed, [](const QJsonObject& w) { return w["resourceClass"].toString() == "putgd-ui"; });
        QVERIFY(result["activeExcluded"].toBool());
        const auto windows = result["windows"].toArray();
        QCOMPARE(windows.size(), 2);
        QCOMPARE(windows[0].toObject()["identity"].toString(), QString("{11111111-1111-1111-1111-111111111111}"));
        QCOMPARE(windows[0].toObject()["appId"].toString(), QString("org.kde.konsole"));
        QCOMPARE(windows[1].toObject()["appId"].toString(), QString("firefox"));
        QVERIFY(!windows[0].toObject().contains("pid"));
    }
    void quotesTheCallbackAddress() {
        const auto script = kwinWindowListScript(":1.42", "token\"x");
        QVERIFY(script.contains(R"(callDBus(":1.42", "/org/putgd/KWinWindows", "org.putgd.KWinWindows", "Windows", "token\"x", JSON.stringify(out)))"));
        QVERIFY(script.contains("workspace.windowList()"));
    }
    void listsThroughAKWinScript() {
        if (!QDBusConnection::sessionBus().isConnected()) QSKIP("needs dbus-run-session");
        auto kwinBus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "fake-kwin");
        FakeKWin kwin(kwinBus);
        kwin.windows = {raw("{11111111-1111-1111-1111-111111111111}", 4000, "konsole", true)};
        QVERIFY(kwinBus.registerObject("/Scripting", &kwin, QDBusConnection::ExportAllSlots));
        QVERIFY(kwinBus.registerService("org.kde.KWin"));
        KWinWindows windows;
        Error error;
        QJsonObject result;
        bool done = false;
        windows.list([](const QJsonObject&) { return false; }, [&](Error e, QJsonObject r) { error = e; result = r; done = true; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
        QVERIFY2(!error, qPrintable(error.message));
        QCOMPARE(result["windows"].toArray().size(), 1);
        QVERIFY(result["windows"].toArray()[0].toObject()["active"].toBool());
        QCOMPARE(kwin.loaded.size(), 1);
        QTRY_COMPARE(kwin.unloaded, kwin.loaded);
        // Another client calling the callback with the request's token is not KWin.
        kwin.silent = true;
        done = false;
        windows.list({}, [&](Error e, QJsonObject r) { error = e; result = r; done = true; });
        QTRY_COMPARE(kwin.loaded.size(), 2);
        const auto token = kwin.loaded.last().mid(QString("putgd-recording-windows-").size());
        auto spoof = QDBusMessage::createMethodCall(QDBusConnection::sessionBus().baseService(), "/org/putgd/KWinWindows", "org.putgd.KWinWindows", "Windows");
        spoof << token << QString::fromUtf8(QJsonDocument(QJsonArray{raw("{44444444-4444-4444-4444-444444444444}", 4100, "spoof", true)}).toJson(QJsonDocument::Compact));
        auto other = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "spoofer");
        other.asyncCall(spoof);
        // KWin never answers: the request times out instead of taking the spoofed list.
        QTRY_VERIFY_WITH_TIMEOUT(done, KWinWindows::timeoutMs + 2000);
        QCOMPARE(error.code, QString("helper_unavailable"));
        QCOMPARE(error.message, QString("KWin did not list its windows"));
        QDBusConnection::disconnectFromBus("spoofer");
        kwinBus.unregisterService("org.kde.KWin");
        QDBusConnection::disconnectFromBus("fake-kwin");
    }
};
QTEST_GUILESS_MAIN(KWinWindowsContract)
#include "test_kwin_windows.moc"
