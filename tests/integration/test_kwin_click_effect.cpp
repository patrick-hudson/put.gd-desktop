// KWin's Mouse Click Animation effect switched on for a recording (#1364),
// against a scripted KWin and KGlobalAccel on a private bus: whatever state the
// user had (not loaded, loaded and off, loaded and on) is the state afterwards.
#include "platform/kde/KWinClickEffect.h"
#include <QDBusConnection>
#include <QtTest>
#include "fixtures/Waits.h"

using namespace putgd;
namespace {
struct Desk { bool loaded = false, enabled = false, reportsEnabled = true, shortcutWorks = true, loads = true; QStringList calls; };
class FakeEffects final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kwin.Effects")
public:
    explicit FakeEffects(Desk* desk) : desk_(desk) {}
public slots:
    Q_SCRIPTABLE bool isEffectLoaded(const QString& name) { return name == "mouseclick" && desk_->loaded; }
    // A freshly loaded effect is switched off, as in KWin.
    Q_SCRIPTABLE bool loadEffect(const QString& name) { desk_->calls << "load"; if (name != "mouseclick" || !desk_->loads) return false; desk_->loaded = true; desk_->enabled = false; return true; }
    Q_SCRIPTABLE void unloadEffect(const QString& name) { desk_->calls << "unload"; if (name == "mouseclick") { desk_->loaded = false; desk_->enabled = false; } }
    Q_SCRIPTABLE QString supportInformation(const QString& name) {
        if (name != "mouseclick" || !desk_->loaded) return {};
        return QStringLiteral("mouseclick:\ncolor1: #ff0000\nringCount: 2\n") + (desk_->reportsEnabled ? QStringLiteral("enabled: %1\n").arg(desk_->enabled ? "true" : "false") : QString()) + "showText: true\n";
    }
private:
    Desk* desk_;
};
class FakeAccel final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kglobalaccel.Component")
public:
    explicit FakeAccel(Desk* desk) : desk_(desk) {}
public slots:
    Q_SCRIPTABLE void invokeShortcut(const QString& name) { desk_->calls << "toggle"; if (name == "ToggleMouseClick" && desk_->loaded && desk_->shortcutWorks) desk_->enabled = !desk_->enabled; }
private:
    Desk* desk_;
};
}
class KWinClickEffectTest : public QObject {
    Q_OBJECT
    Desk desk_;
    FakeEffects effects_{&desk_};
    FakeAccel accel_{&desk_};
    static Error watch(KWinClickEffect& clicks) {
        Error replied{"unanswered", "", {}};
        clicks.watchClicks({{"kind", "monitor"}}, [](QJsonObject) {}, [&](Error error, QJsonObject) { replied = error; });
        if (!putgd::test::waitFor([&] { return replied.code != "unanswered"; }, 5000)) return {"unanswered", "no reply", {}};
        return replied;
    }
    bool settled(bool loaded, bool enabled) { return putgd::test::waitFor([&] { return desk_.loaded == loaded && desk_.enabled == enabled; }, 5000); }
private slots:
    void initTestCase() {
        auto bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("fake-plasma"));
        if (!bus.isConnected()) QSKIP("no session bus");
        QVERIFY(bus.registerObject("/Effects", &effects_, QDBusConnection::ExportScriptableSlots));
        QVERIFY(bus.registerObject("/component/kwin", &accel_, QDBusConnection::ExportScriptableSlots));
        QVERIFY(bus.registerService("org.kde.KWin"));
        QVERIFY(bus.registerService("org.kde.kglobalaccel"));
    }
    void init() { desk_ = Desk{}; }
    void reportedState() {
        QCOMPARE(KWinClickEffect::enabledIn("ringCount: 2\nenabled: true\nshowText: false\n"), 1);
        QCOMPARE(KWinClickEffect::enabledIn("enabled: false"), 0);
        QCOMPARE(KWinClickEffect::enabledIn("ringCount: 2\nshowText: false\n"), -1);
        QCOMPARE(KWinClickEffect::enabledIn("notenabled: true\n"), -1);
    }
    void theUsersStateComesBack_data() {
        QTest::addColumn<bool>("loaded"); QTest::addColumn<bool>("enabled"); QTest::addColumn<QStringList>("during"); QTest::addColumn<QStringList>("after");
        QTest::newRow("not loaded") << false << false << QStringList{"load", "toggle"} << QStringList{"toggle", "unload"};
        QTest::newRow("loaded, off") << true << false << QStringList{"toggle"} << QStringList{"toggle"};
        // The user's own rings: nothing is touched, before or after.
        QTest::newRow("loaded, on") << true << true << QStringList{} << QStringList{};
    }
    void theUsersStateComesBack() {
        QFETCH(bool, loaded); QFETCH(bool, enabled); QFETCH(QStringList, during); QFETCH(QStringList, after);
        desk_.loaded = loaded; desk_.enabled = enabled;
        KWinClickEffect clicks;
        QVERIFY(clicks.clickCapability()["available"].toBool());
        const auto error = watch(clicks); QVERIFY2(!error, qPrintable(error.message));
        QVERIFY(desk_.loaded); QVERIFY(desk_.enabled); QCOMPARE(desk_.calls, during);
        desk_.calls.clear();
        clicks.unwatchClicks();
        QVERIFY(settled(loaded, enabled)); PUTGD_COMPARE_STEADY(desk_.calls, after, 300);
        QCOMPARE(desk_.loaded, loaded); QCOMPARE(desk_.enabled, enabled);
    }
    void recordThenStopAtOnce() {
        KWinClickEffect clicks;
        Error replied{"unanswered", "", {}};
        clicks.watchClicks({}, [](QJsonObject) {}, [&](Error error, QJsonObject) { replied = error; });
        clicks.unwatchClicks();
        QVERIFY(putgd::test::waitFor([&] { return replied.code != "unanswered" && desk_.calls.contains("unload"); }, 5000));
        PUTGD_VERIFY_STEADY(!desk_.loaded && !desk_.enabled, 300);
        QCOMPARE(desk_.calls, (QStringList{"load", "toggle", "toggle", "unload"}));
    }
    void aStateKWinDoesNotReportIsLeftAlone() {
        desk_.loaded = true; desk_.enabled = true; desk_.reportsEnabled = false;
        KWinClickEffect clicks;
        const auto error = watch(clicks);
        QCOMPARE(error.code, QString("click_effect_unavailable")); QVERIFY(error.message.contains("does not report"));
        PUTGD_VERIFY_STEADY(desk_.loaded && desk_.enabled && desk_.calls.isEmpty(), 300);
        clicks.unwatchClicks(); PUTGD_VERIFY_STEADY(desk_.calls.isEmpty(), 300);
    }
    void aToggleThatDoesNotTakeUndoesTheLoad() {
        desk_.shortcutWorks = false;
        KWinClickEffect clicks;
        const auto error = watch(clicks);
        QCOMPARE(error.code, QString("click_effect_unavailable")); QVERIFY(error.message.contains("did not switch on"));
        QVERIFY(settled(false, false)); QCOMPARE(desk_.calls, (QStringList{"load", "toggle", "unload"}));
    }
    void anEffectThatDoesNotLoad() {
        desk_.loads = false;
        KWinClickEffect clicks;
        QCOMPARE(watch(clicks).code, QString("click_effect_unavailable"));
        PUTGD_VERIFY_STEADY(!desk_.loaded && desk_.calls == QStringList{"load"}, 300);
    }
    void switchedOffByTheUserDuringTheRecording() {
        desk_.loaded = true;
        KWinClickEffect clicks;
        QVERIFY(!watch(clicks)); QVERIFY(desk_.enabled);
        desk_.enabled = false; desk_.calls.clear();
        clicks.unwatchClicks();
        // Off is what it was before the recording: nothing to switch back, and it stays loaded.
        PUTGD_VERIFY_STEADY(desk_.loaded && !desk_.enabled && desk_.calls.isEmpty(), 400);
    }
};
QTEST_GUILESS_MAIN(KWinClickEffectTest)
#include "test_kwin_click_effect.moc"
