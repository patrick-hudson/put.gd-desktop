#include "ui/Appearance.h"
#include "ui/PanelTone.h"
#include "ui/RedesignTheme.h"
#include "ui/Theme.h"
#include "ui/ViewState.h"
#include "ui/VisualStyle.h"
#include <QApplication>
#include <QCheckBox>
#include <QRadioButton>
#include <QStyleOptionButton>
#include <QComboBox>
#include <QFile>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QDir>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSpinBox>
#include <QStyleOptionSpinBox>
#include <QVBoxLayout>
#include <QtTest>
#include "fixtures/Waits.h"

using namespace putgd;

// #60 phase 1: one token set with dark and light values, a System / Dark /
// Light setting that follows the desktop live, and a tray tint that follows
// the panel rather than the app.
class ThemeTest : public QObject {
    Q_OBJECT
    static QColor centre(const QIcon& icon) {
        const auto image = icon.pixmap(48, 48).toImage();
        // The history glyph's minute hand runs from the centre straight up.
        return image.pixelColor(image.width() / 2, image.height() * 9 / 24);
    }
private slots:
    void init() { Theme::setScheme(Theme::Scheme::Dark); }
    void textAndFocusMeetAaInBothSchemes() {
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            const auto& p = Theme::palette(scheme);
            const QList<QPair<const char*, QColor>> texts{{"text", p.text}, {"textSecondary", p.textSecondary}, {"textMuted", p.textMuted}, {"link", p.link},
                                                          {"success", p.success}, {"warning", p.warning}, {"danger", p.danger}};
            const QList<QPair<const char*, QColor>> backgrounds{{"bg", p.bg}, {"surface", p.surface}, {"surfaceRaised", p.surfaceRaised}, {"selected", p.selected}};
            for (const auto& [name, text] : texts)
                for (const auto& [background, colour] : backgrounds)
                    QVERIFY2(Theme::contrast(text, colour) >= 4.5, qPrintable(QString("%1 %2 on %3: %4").arg(Theme::name(scheme), name, background).arg(Theme::contrast(text, colour))));
            for (const auto& [background, colour] : backgrounds) {
                QVERIFY2(Theme::contrast(p.accent, colour) >= 3.0, qPrintable(QString("%1 focus ring on %2").arg(Theme::name(scheme), background)));
                QVERIFY2(Theme::contrast(p.icon, colour) >= 3.0, qPrintable(QString("%1 icon on %2").arg(Theme::name(scheme), background)));
            }
            QVERIFY(Theme::contrast(p.onAccent, p.accent) >= 4.5);
            QVERIFY(Theme::contrast(p.onAccent, p.accentHover) >= 4.5);
            QVERIFY(Theme::contrast(p.success, p.successSurface) >= 4.5);
            QVERIFY(Theme::contrast(p.warning, p.warningSurface) >= 4.5);
            QVERIFY(Theme::contrast(p.danger, p.dangerSurface) >= 4.5);
            // Every token is reported, as #rrggbb.
            const auto tokens = Theme::tokens(scheme);
            QCOMPARE(tokens.size(), 23);
            for (const auto& value : tokens) QVERIFY(QRegularExpression("^#[0-9a-f]{6}$").match(value.toString()).hasMatch());
        }
        QCOMPARE(Theme::tokens(Theme::Scheme::Dark)["bg"].toString(), QString("#111317"));
        QVERIFY(Theme::tokens(Theme::Scheme::Light)["bg"] != Theme::tokens(Theme::Scheme::Dark)["bg"]);
    }
    void trayTintsSuitTheirPanels() {
        // Typical panels: GNOME Shell's and Mint-Y-Dark's dark bars, Breeze's light one.
        for (const auto& panel : {QColor("#000000"), QColor("#1d1d1d"), QColor("#2f343f")})
            for (const auto& tint : {Theme::trayTints(false).idle, Theme::trayTints(false).upload, Theme::trayTints(false).encoding, Theme::trayTints(false).error})
                QVERIFY2(Theme::contrast(tint, panel) >= 3.0, qPrintable(tint.name() + " on " + panel.name()));
        for (const auto& panel : {QColor("#ffffff"), QColor("#dee0e2"), QColor("#eff0f1")})
            for (const auto& tint : {Theme::trayTints(true).idle, Theme::trayTints(true).upload, Theme::trayTints(true).encoding, Theme::trayTints(true).error})
                QVERIFY2(Theme::contrast(tint, panel) >= 3.0, qPrintable(tint.name() + " on " + panel.name()));
        // The dark-panel set is the one put.gd always drew.
        QCOMPARE(Theme::trayTints(false).upload, QColor("#62d69b"));
    }
    void stylesheetsUseOnlyTokens() {
        QWidget probe;
        RedesignTheme::apply(probe);
        probe.show();
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);
            const auto qss = probe.styleSheet();
            QVERIFY2(!QRegularExpression("%[A-Za-z_]+%").match(qss).hasMatch(), "a placeholder was left unreplaced");
            QVERIFY(qss.contains(Theme::current().bg.name()));
            QVERIFY(qApp->styleSheet().contains(Theme::current().accent.name()));
            QVERIFY(!QRegularExpression("%[A-Za-z_]+%").match(qApp->styleSheet()).hasMatch());
            // The painted indicator images exist for this scheme.
            for (const auto* name : {"check", "check@2x", "radio", "radio-checked", "chevron", "spin-up", "spin-down", "spin-up-off", "spin-down-off", "spin-up@2x"})
                QVERIFY2(QFile::exists(RedesignTheme::indicators(scheme) + "/" + name + ".png"), name);
        }
    }
    // #300: a checked, disabled check box or radio button still looks checked:
    // a muted tick or dot on the accent-tinted fill, not an empty box.
    void checkedDisabledIndicatorsStayChecked() {
        QWidget window;
        auto* layout = new QVBoxLayout(&window);
        QList<QAbstractButton*> buttons;
        for (const bool radio : {false, true})
            for (const bool checked : {false, true})
                for (const bool enabled : {true, false}) {
                    QAbstractButton* button = radio ? static_cast<QAbstractButton*>(new QRadioButton("Synthetic", &window)) : new QCheckBox("Synthetic", &window);
                    if (radio) button->setAutoExclusive(false);
                    button->setChecked(checked);
                    button->setEnabled(enabled);
                    layout->addWidget(button);
                    buttons.append(button);
                }
        RedesignTheme::apply(window);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);
            const auto& p = Theme::palette(scheme);
            QList<QImage> shots;
            for (auto* button : buttons) {
                QStyleOptionButton option;
                option.initFrom(button);
                const auto element = qobject_cast<QRadioButton*>(button) ? QStyle::SE_RadioButtonIndicator : QStyle::SE_CheckBoxIndicator;
                const auto box = button->style()->subElementRect(element, &option, button);
                QVERIFY2(box.width() >= 15 && box.height() >= 15, "no indicator");
                shots.append(button->grab(box).toImage().convertToFormat(QImage::Format_RGB32));
            }
            const auto near = [](const QColor& c, const QColor& t, int d) { return std::abs(c.red() - t.red()) + std::abs(c.green() - t.green()) + std::abs(c.blue() - t.blue()) < d; };
            const auto count = [&](const QImage& image, const QColor& colour, int d) {
                int n = 0;
                for (int y = 0; y < image.height(); ++y)
                    for (int x = 0; x < image.width(); ++x) n += near(image.pixelColor(x, y), colour, d);
                return n;
            };
            const auto differing = [](const QImage& a, const QImage& b) {
                int n = 0;
                for (int y = 0; y < std::min(a.height(), b.height()); ++y)
                    for (int x = 0; x < std::min(a.width(), b.width()); ++x) n += a.pixel(x, y) != b.pixel(x, y);
                return n;
            };
            const auto what = [&](const char* text) { return qPrintable(Theme::name(scheme) + ": " + text); };
            // Order: [radio][checked][enabled]; disabled is the odd index.
            const auto& boxOff = shots[1], &boxOnEnabled = shots[2], &boxOn = shots[3];
            const auto& radioOff = shots[5], &radioOn = shots[7];
            // A checked, disabled indicator differs from an unchecked, disabled one,
            // and from a checked, enabled one.
            QVERIFY2(differing(boxOn, boxOff) > 40, what("checked and unchecked disabled check boxes look alike"));
            QVERIFY2(differing(boxOn, boxOnEnabled) > 40, what("a disabled check box looks enabled"));
            QVERIFY2(differing(radioOn, radioOff) > 20, what("checked and unchecked disabled radio buttons look alike"));
            // The box keeps its accent-tinted fill and a visible tick; unchecked has neither.
            const int inside = (boxOn.width() - 4) * (boxOn.height() - 4);
            QVERIFY2(count(boxOn, p.selected, 8) * 3 > inside, what("checked, disabled box is not on the muted accent fill"));
            QVERIFY2(count(boxOn, p.textMuted, 60) >= 6, what("checked, disabled box has no tick"));
            QCOMPARE(count(boxOff, p.textMuted, 60), 0);
            QCOMPARE(count(boxOff, p.selected, 8), 0);
            QVERIFY2(count(radioOn, p.textMuted, 60) >= 6, what("checked, disabled radio button has no dot"));
            QCOMPARE(count(radioOff, p.textMuted, 60), 0);
        }
    }
    // #233: spin box steps are token chevrons on the themed field, in both
    // schemes, not Fusion's default sub-controls.
    void spinBoxStepsUseTokenChevrons() {
        QWidget window;
        auto* layout = new QVBoxLayout(&window);
        auto* spin = new QSpinBox(&window);
        spin->setRange(0, 100);
        spin->setValue(50);
        spin->setMinimumWidth(120);
        layout->addWidget(spin);
        RedesignTheme::apply(window);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);
            const auto& p = Theme::palette(scheme);
            QStyleOptionSpinBox option;
            option.initFrom(spin);
            option.subControls = QStyle::SC_SpinBoxUp | QStyle::SC_SpinBoxDown | QStyle::SC_SpinBoxFrame | QStyle::SC_SpinBoxEditField;
            const auto shot = spin->grab().toImage();
            for (const auto control : {QStyle::SC_SpinBoxUp, QStyle::SC_SpinBoxDown}) {
                const auto box = spin->style()->subControlRect(QStyle::CC_SpinBox, &option, control, spin) & shot.rect();
                QVERIFY2(!box.isEmpty(), "no step button");
                int chevron = 0, field = 0;
                for (int y = box.top(); y <= box.bottom(); ++y)
                    for (int x = box.left(); x <= box.right(); ++x) {
                        const auto c = shot.pixelColor(x, y);
                        const auto near = [&](const QColor& t, int d) { return std::abs(c.red() - t.red()) + std::abs(c.green() - t.green()) + std::abs(c.blue() - t.blue()) < d; };
                        if (near(p.textSecondary, 90)) ++chevron;
                        if (near(p.surfaceRaised, 12)) ++field;
                    }
                QVERIFY2(chevron >= 4, qPrintable(Theme::name(scheme) + ": no chevron in a step button"));
                // The button shows the field behind it, not a bevel of its own.
                QVERIFY2(field * 2 > box.width() * box.height(), qPrintable(Theme::name(scheme) + ": step button is not on the field colour"));
            }
        }
    }
    // #237: a switch never replaces the application stylesheet (Qt would
    // re-polish every widget); windows carry the scheme instead, and a hidden
    // window restyles when it is next shown.
    void schemeSwitchKeepsTheApplicationStylesheet() {
        QWidget plain;
        auto* label = new QLabel("Synthetic", &plain);
        label->setProperty("visualRole", "status");
        new QVBoxLayout(&plain);
        plain.layout()->addWidget(label);
        plain.show();
        QVERIFY(putgd::test::waitForWindowExposed(&plain));
        QWidget hidden;
        RedesignTheme::apply(hidden);
        hidden.resize(80, 60);
        const auto sheet = qApp->styleSheet();
        const auto rootSheet = hidden.styleSheet();
        QCOMPARE(plain.property("themeScheme").toString(), QString("dark"));
        const auto pixel = [&] { return plain.grab().toImage().pixelColor(label->geometry().center().x(), label->geometry().top() + 4); };
        QCOMPARE(pixel(), Theme::palette(Theme::Scheme::Dark).surface);
        QVERIFY(Theme::setScheme(Theme::Scheme::Light));
        QCOMPARE(qApp->styleSheet(), sheet);
        QCOMPARE(plain.property("themeScheme").toString(), QString("light"));
        // The application rules for the new scheme apply to the open window.
        QCOMPARE(pixel(), Theme::palette(Theme::Scheme::Light).surface);
        // Neither stylesheet changes; the hidden window catches up when shown.
        QCOMPARE(hidden.styleSheet(), rootSheet);
        QVERIFY(rootSheet.contains(Theme::palette(Theme::Scheme::Dark).bg.name()) && rootSheet.contains(Theme::palette(Theme::Scheme::Light).bg.name()));
        hidden.show();
        QVERIFY(putgd::test::waitForWindowExposed(&hidden));
        QCOMPARE(hidden.property("themeScheme").toString(), QString("light"));
        QCOMPARE(hidden.grab().toImage().pixelColor(4, 4), Theme::palette(Theme::Scheme::Light).bg);
        // A window opened later is marked with the active scheme.
        QWidget later;
        later.show();
        QCOMPARE(later.property("themeScheme").toString(), QString("light"));
    }
    void schemeSwitchRestylesOpenWindowsLive() {
        QWidget window;
        auto* layout = new QVBoxLayout(&window);
        auto* button = new QPushButton("Upload", &window);
        button->setIcon(VisualStyle::icon("history"));
        layout->addWidget(button);
        RedesignTheme::apply(window);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        QCOMPARE(window.palette().color(QPalette::Window), Theme::palette(Theme::Scheme::Dark).bg);
        const auto darkGlyph = centre(VisualStyle::icon("history"));
        QVERIFY(Theme::setScheme(Theme::Scheme::Light));
        QVERIFY(!Theme::setScheme(Theme::Scheme::Light));
        QCOMPARE(window.palette().color(QPalette::Window), Theme::palette(Theme::Scheme::Light).bg);
        QCOMPARE(window.property("themeScheme").toString(), QString("light"));
        QCOMPARE(qApp->palette().color(QPalette::Window), Theme::palette(Theme::Scheme::Light).bg);
        // The same QIcon now draws in the light scheme's icon colour.
        const auto lightGlyph = centre(button->icon());
        QVERIFY2(lightGlyph != darkGlyph, qPrintable(lightGlyph.name() + " " + darkGlyph.name()));
        QVERIFY2(std::abs(lightGlyph.red() - Theme::palette(Theme::Scheme::Light).icon.red()) < 24, qPrintable(lightGlyph.name()));
        const auto shot = window.grab().toImage();
        QCOMPARE(shot.pixelColor(2, 2), Theme::palette(Theme::Scheme::Light).bg);
    }
    void desktopPreferenceOrder_data() {
        QTest::addColumn<int>("qt");
        QTest::addColumn<int>("portal");
        QTest::addColumn<QString>("gnomeScheme");
        QTest::addColumn<QString>("gtkTheme");
        QTest::addColumn<QString>("kdeglobals");
        QTest::addColumn<QString>("desktop");
        QTest::addColumn<QString>("source");
        QTest::newRow("Qt 6.5+ style hint") << 2 << 2 << "" << "" << "" << "dark" << "qt";
        QTest::newRow("portal dark (Qt 6.4)") << 0 << 1 << "prefer-light" << "Adwaita" << "" << "dark" << "portal";
        QTest::newRow("portal light (Plasma)") << 0 << 2 << "" << "" << "" << "light" << "portal";
        QTest::newRow("GNOME prefer-dark") << 0 << 0 << "prefer-dark" << "Adwaita" << "" << "dark" << "portal-gnome";
        QTest::newRow("GNOME default, Adwaita") << 0 << 0 << "default" << "Adwaita" << "" << "light" << "portal-gtk-theme";
        QTest::newRow("Cinnamon Mint-Y-Dark-Aqua") << 0 << 0 << "default" << "Mint-Y-Dark-Aqua" << "" << "dark" << "portal-gtk-theme";
        QTest::newRow("Cinnamon Mint-Y-Aqua") << 0 << 0 << "default" << "Mint-Y-Aqua" << "" << "light" << "portal-gtk-theme";
        QTest::newRow("Plasma without portal, Breeze") << 0 << 0 << "" << "" << "[Colors:Window]\nBackgroundNormal=239,240,241\n" << "light" << "kdeglobals";
        QTest::newRow("Plasma without portal, Breeze Dark") << 0 << 0 << "" << "" << "[Colors:Window]\nBackgroundNormal=32,35,38\n" << "dark" << "kdeglobals";
        QTest::newRow("nothing says") << 0 << 0 << "" << "" << "" << "" << "unknown";
    }
    void desktopPreferenceOrder() {
        QFETCH(int, qt);
        QFETCH(int, portal);
        QFETCH(QString, gnomeScheme);
        QFETCH(QString, gtkTheme);
        QFETCH(QString, kdeglobals);
        QFETCH(QString, desktop);
        QFETCH(QString, source);
        Appearance::Inputs in;
        in.qt = qt;
        in.portal = portal;
        in.gnomeScheme = gnomeScheme;
        in.gtkTheme = gtkTheme;
        in.kdeglobals = kdeglobals;
        in.kde = !kdeglobals.isEmpty();
        const auto [preference, decidedBy] = Appearance::decide(in);
        QCOMPARE(preference, desktop);
        QCOMPARE(decidedBy, source);
    }
    void settingPicksTheSchemeAndReportsIt() {
        Appearance appearance;
        QSignalSpy changes(&appearance, &Appearance::changed);
        appearance.setPreference("light");
        QCOMPARE(Theme::scheme(), Theme::Scheme::Light);
        QCOMPARE(appearance.state()["theme"].toString(), QString("light"));
        QCOMPARE(appearance.state()["scheme"].toString(), QString("light"));
        QCOMPARE(appearance.state()["tokens"].toObject()["bg"].toString(), Theme::palette(Theme::Scheme::Light).bg.name());
        appearance.setPreference("dark");
        QCOMPARE(Theme::scheme(), Theme::Scheme::Dark);
        QVERIFY(changes.count() >= 2);
        appearance.setPreference("sepia");
        QCOMPARE(appearance.preference(), QString("system"));
        // The view state carries it to CLI/MCP readers, and rejects malformed reports.
        QVERIFY(!validateUiState({{"schemaVersion", 1}, {"appearance", appearance.state()}}));
        auto bad = appearance.state();
        bad["scheme"] = "sepia";
        QCOMPARE(validateUiState({{"schemaVersion", 1}, {"appearance", bad}}).code, QString("ui_state_invalid"));
        bad = appearance.state();
        bad["extra"] = true;
        QCOMPARE(validateUiState({{"schemaVersion", 1}, {"appearance", bad}}).code, QString("ui_state_invalid"));
    }
    // #198: the resident resolves the same setting for putgd-record without
    // switching its own scheme.
    void resolverLeavesTheActiveSchemeAlone() {
        Appearance resolver(nullptr, Appearance::Role::Resolver);
        QSignalSpy changes(&resolver, &Appearance::changed);
        resolver.setPreference("light");
        QCOMPARE(resolver.scheme(), Theme::Scheme::Light);
        QCOMPARE(Theme::scheme(), Theme::Scheme::Dark);
        QCOMPARE(changes.count(), 1);
        resolver.setPreference("dark");
        QCOMPARE(resolver.scheme(), Theme::Scheme::Dark);
        QCOMPARE(changes.count(), 2);
        QCOMPARE(Theme::scheme(), Theme::Scheme::Dark);
    }
    void panelToneRules_data() {
        QTest::addColumn<QString>("desktop");
        QTest::addColumn<QString>("plasmarc");
        QTest::addColumn<QString>("kdeglobals");
        QTest::addColumn<QString>("cinnamon");
        QTest::addColumn<QString>("styleColors");
        QTest::addColumn<bool>("light");
        QTest::addColumn<QString>("source");
        QTest::newRow("GNOME: dark top bar") << "GNOME" << "" << "" << "" << "" << false << "gnome-shell";
        QTest::newRow("Ubuntu GNOME") << "ubuntu:GNOME" << "" << "" << "" << "" << false << "gnome-shell";
        QTest::newRow("Plasma: Breeze Dark style") << "KDE" << "[Theme]\nname=breeze-dark\n" << "[Colors:Header]\nBackgroundNormal=222,224,226\n" << "" << "" << false << "plasmarc";
        QTest::newRow("Plasma: light header") << "KDE" << "" << "[Colors:Header]\nBackgroundNormal=222,224,226\n[Colors:Window]\nBackgroundNormal=32,35,38\n" << "" << "" << true << "kdeglobals";
        QTest::newRow("Plasma: dark window") << "KDE" << "" << "[Colors:Window]\nBackgroundNormal=32,35,38\n" << "" << "" << false << "kdeglobals";
        QTest::newRow("Plasma: untouched") << "KDE" << "" << "" << "" << "" << true << "kde-default";
        QTest::newRow("Cinnamon: Mint-Y-Dark-Aqua") << "X-Cinnamon" << "" << "" << "Mint-Y-Dark-Aqua" << "" << false << "cinnamon-theme";
        QTest::newRow("Cinnamon: Mint-Y-Aqua") << "X-Cinnamon" << "" << "" << "Mint-Y-Aqua" << "" << true << "cinnamon-theme";
        // Kubuntu's default style (k72) and Breeze Dark from kdedefaults (d69) ship
        // dark window colours, whatever the light colour scheme says.
        QTest::newRow("Plasma: Kubuntu style colours") << "KDE" << "[Theme]\nname=kubuntu\n" << "[Colors:Header]\nBackgroundNormal=222,224,226\n" << "" << "[Colors:Window]\nBackgroundNormal=32,35,38\n" << false << "plasma-style";
        QTest::newRow("Plasma: Breeze Light style colours") << "KDE" << "[Theme]\nname=breeze-light\n" << "" << "" << "[Colors:Window]\nBackgroundNormal=239,240,241\n" << true << "plasma-style";
        QTest::newRow("unknown desktop") << "XFCE" << "" << "" << "" << "" << false << "default";
    }
    void panelToneRules() {
        QFETCH(QString, desktop);
        QFETCH(QString, plasmarc);
        QFETCH(QString, kdeglobals);
        QFETCH(QString, cinnamon);
        QFETCH(QString, styleColors);
        QFETCH(bool, light);
        QFETCH(QString, source);
        const auto [isLight, decidedBy] = PanelTone::decide({desktop, plasmarc, kdeglobals, cinnamon, styleColors});
        QCOMPARE(isLight, light);
        QCOMPARE(decidedBy, source);
    }
    // The real lookup: the style comes from ~/.config/kdedefaults (as on d69) and
    // its colours file from XDG_DATA_DIRS, while the colour scheme stays light.
    void panelToneReadsKdeCascadeAndStyleColours() {
        QTemporaryDir config, data;
        QVERIFY(config.isValid() && data.isValid());
        auto write = [](const QString& path, const QByteArray& text) { QDir().mkpath(QFileInfo(path).absolutePath()); QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(text) == text.size(); };
        QVERIFY(write(config.filePath("kdedefaults/plasmarc"), "[Theme]\nname=breeze-dark\n"));
        QVERIFY(write(config.filePath("kdeglobals"), "[Colors:Header]\nBackgroundNormal=222,224,226\n"));
        QVERIFY(write(data.filePath("plasma/desktoptheme/breeze-dark/colors"), "[Colors:Window]\nBackgroundNormal=32,35,38\n"));
        const QByteArray saved[] = {qgetenv("XDG_CONFIG_HOME"), qgetenv("XDG_DATA_DIRS"), qgetenv("XDG_CURRENT_DESKTOP"), qgetenv("XDG_DATA_HOME")};
        auto restore = qScopeGuard([&] {
            const char* names[] = {"XDG_CONFIG_HOME", "XDG_DATA_DIRS", "XDG_CURRENT_DESKTOP", "XDG_DATA_HOME"};
            for (int i = 0; i < 4; ++i) saved[i].isNull() ? qunsetenv(names[i]) : qputenv(names[i], saved[i]);
        });
        qputenv("XDG_CONFIG_HOME", config.path().toLocal8Bit());
        qputenv("XDG_DATA_HOME", config.filePath("data").toLocal8Bit());
        qputenv("XDG_DATA_DIRS", data.path().toLocal8Bit());
        qputenv("XDG_CURRENT_DESKTOP", "KDE");
        QCOMPARE(PanelTone::plasmaStyle(), QString("breeze-dark"));
        PanelTone tone;
        QVERIFY(!tone.light());
        QCOMPARE(tone.source(), QString("plasma-style"));
    }
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    VisualStyle::apply(app);
    ThemeTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_theme.moc"
