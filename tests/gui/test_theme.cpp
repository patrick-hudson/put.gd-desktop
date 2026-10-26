#include "ui/Appearance.h"
#include "ui/PanelTone.h"
#include "ui/RedesignTheme.h"
#include "ui/Theme.h"
#include "ui/ViewState.h"
#include "ui/VisualStyle.h"
#include "ui/Graphite.h"
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
#include <QLineEdit>
#include <QPushButton>
#include <QPainter>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSpinBox>
#include <QScrollBar>
#include <QStyleOptionSlider>
#include <QStyleOptionSpinBox>
#include <QVBoxLayout>
#include <QStyleOptionToolButton>
#include <QHBoxLayout>
#include <QToolButton>
#include <QMenu>
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
    void idleScrollbarsMeetContrastOnEveryScrollSurface() {
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);
            const auto& p = Theme::palette(scheme);
            for (const auto surface : {p.bg, p.surface, p.surfaceRaised}) {
                QScrollBar bar(Qt::Vertical);
                bar.resize(12, 160); bar.setRange(0, 100); bar.setPageStep(30);
                QStyleOptionSlider option;
                option.initFrom(&bar); option.rect = bar.rect(); option.orientation = Qt::Vertical;
                option.minimum = 0; option.maximum = 100; option.pageStep = 30;
                option.state = QStyle::State_Enabled; option.subControls = QStyle::SC_ScrollBarSlider;
                QImage shot(bar.size(), QImage::Format_RGB32); shot.fill(surface);
                QPainter painter(&shot); bar.style()->drawComplexControl(QStyle::CC_ScrollBar, &option, &painter, &bar); painter.end();
                const auto handle = bar.style()->subControlRect(QStyle::CC_ScrollBar, &option, QStyle::SC_ScrollBarSlider, &bar);
                const auto thumb = shot.pixelColor(handle.center());
                QCOMPARE(thumb, p.controlOutline);
                QVERIFY2(Theme::contrast(thumb, surface) >= 3.0, qPrintable(Theme::name(scheme) + " scrollbar on " + surface.name()));
                for (const auto state : {QStyle::State_MouseOver, QStyle::State_Sunken}) {
                    option.state = QStyle::State_Enabled | state; option.activeSubControls = QStyle::SC_ScrollBarSlider;
                    shot.fill(surface); painter.begin(&shot); bar.style()->drawComplexControl(QStyle::CC_ScrollBar, &option, &painter, &bar); painter.end();
                    QVERIFY(Theme::contrast(shot.pixelColor(handle.center()), surface) > Theme::contrast(thumb, surface));
                }
            }
        }
    }
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
            // Essential control boundaries and the icons' secondary tone read at
            // 3:1 on every page surface; the two icon tones stay 3:1 apart, so a
            // duotone glyph keeps its inner detail.
            for (const auto& [background, colour] : backgrounds) {
                if (QString(background) == "selected") continue;
                QVERIFY2(Theme::contrast(p.controlOutline, colour) >= 3.0, qPrintable(QString("%1 control outline on %2: %3").arg(Theme::name(scheme), background).arg(Theme::contrast(p.controlOutline, colour))));
                QVERIFY2(Theme::contrast(p.iconSecondary, colour) >= 3.0, qPrintable(QString("%1 secondary icon tone on %2: %3").arg(Theme::name(scheme), background).arg(Theme::contrast(p.iconSecondary, colour))));
            }
            QVERIFY2(Theme::contrast(p.icon, p.iconSecondary) >= 3.0, qPrintable(Theme::name(scheme) + " icon tones"));
            // The outline is not the decorative border: section rules stay quiet.
            QVERIFY(Theme::contrast(p.border, p.bg) < 2.0);
            QVERIFY(Theme::contrast(p.onAccent, p.accent) >= 4.5);
            QVERIFY(Theme::contrast(p.onAccent, p.accentHover) >= 4.5);
            QVERIFY(Theme::contrast(p.success, p.successSurface) >= 4.5);
            QVERIFY(Theme::contrast(p.warning, p.warningSurface) >= 4.5);
            QVERIFY(Theme::contrast(p.danger, p.dangerSurface) >= 4.5);
            // Every token is reported, as #rrggbb.
            const auto tokens = Theme::tokens(scheme);
            QCOMPARE(tokens.size(), 26);
            QVERIFY(tokens.contains("controlOutline"));
            for (const auto& value : tokens) QVERIFY(QRegularExpression("^#[0-9a-f]{6}$").match(value.toString()).hasMatch());
        }
        QCOMPARE(Theme::tokens(Theme::Scheme::Dark)["bg"].toString(), QString("#191919"));
        QCOMPARE(Theme::tokens(Theme::Scheme::Light)["bg"].toString(), QString("#eeeeeb"));
        // Graphite: amber, never blue, and a neutral primary action.
        QCOMPARE(Theme::tokens(Theme::Scheme::Dark)["accent"].toString(), QString("#e8b66d"));
        QCOMPARE(Theme::tokens(Theme::Scheme::Dark)["controlOutline"].toString(), QString("#7c7c78"));
        QCOMPARE(Theme::tokens(Theme::Scheme::Light)["controlOutline"].toString(), QString("#82827b"));
        QVERIFY(Theme::tokens(Theme::Scheme::Light)["bg"] != Theme::tokens(Theme::Scheme::Dark)["bg"]);
    }
    // A selected row, segment or menu item draws its glyph in the selected
    // pair: the silhouette and the inner detail each reach 3:1 on the selected
    // fill, and stay 3:1 apart (the normal secondary tone fell to about 2.7:1).
    void selectedIconTonesReadOnTheSelectedFill() {
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            const auto& p = Theme::palette(scheme);
            const auto what = [&](const char* text, double ratio) { return qPrintable(QString("%1 %2: %3").arg(Theme::name(scheme), text).arg(ratio)); };
            QVERIFY2(Theme::contrast(p.iconSelected, p.selected) >= 3.0, what("selected silhouette", Theme::contrast(p.iconSelected, p.selected)));
            QVERIFY2(Theme::contrast(p.iconSecondarySelected, p.selected) >= 3.0, what("selected inner detail", Theme::contrast(p.iconSecondarySelected, p.selected)));
            QVERIFY2(Theme::contrast(p.iconSelected, p.iconSecondarySelected) >= 3.0, what("selected tones apart", Theme::contrast(p.iconSelected, p.iconSecondarySelected)));
            // Painted, not only declared: QIcon::Selected renders the selected pair.
            Theme::setScheme(scheme);
            const auto icon = VisualStyle::icon("settings");
            const auto normal = icon.pixmap(QSize(36, 36), QIcon::Normal).toImage(), selected = icon.pixmap(QSize(36, 36), QIcon::Selected).toImage();
            QVERIFY(normal != selected);
            const auto near = [](const QColor& a, const QColor& b) { return std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) + std::abs(a.blue() - b.blue()) < 12; };
            int silhouette = 0, detail = 0;
            for (int y = 0; y < selected.height(); ++y)
                for (int x = 0; x < selected.width(); ++x) {
                    const auto colour = selected.pixelColor(x, y);
                    if (colour.alpha() < 250) continue;
                    silhouette += near(colour, p.iconSelected);
                    detail += near(colour, p.iconSecondarySelected);
                }
            QVERIFY2(silhouette > 10 && detail > 4, what("selected pixels", silhouette * 1000 + detail));
        }
    }
    // A locked value (Graphite::setLocked) reads as information: its text is
    // textMuted at 4.5:1 on the page, while an unavailable control keeps the
    // faded textDisabled. Unlocking restores the plain disabled look.
    void lockedValuesStayReadable() {
        QWidget window;
        auto* layout = new QVBoxLayout(&window);
        auto* locked = VisualStyle::choiceBox(&window);
        auto* unavailable = VisualStyle::choiceBox(&window);
        for (auto* combo : {locked, unavailable}) { combo->addItem("Synthetic value"); combo->setMinimumWidth(200); combo->setEnabled(false); layout->addWidget(combo); }
        Graphite::setLocked(locked, true);
        RedesignTheme::apply(window);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);
            const auto& p = Theme::palette(scheme);
            const auto best = [&](QWidget* widget) {
                const auto shot = window.grab(QRect(widget->mapTo(&window, QPoint()), widget->size())).toImage().convertToFormat(QImage::Format_RGB32);
                double ratio = 0;
                for (int y = 0; y < shot.height(); ++y)
                    for (int x = 0; x < shot.width(); ++x) ratio = std::max(ratio, Theme::contrast(shot.pixelColor(x, y), p.bg));
                return ratio;
            };
            QVERIFY2(best(locked) >= 4.5, qPrintable(Theme::name(scheme) + QString(" locked %1").arg(best(locked))));
            QVERIFY2(best(unavailable) < 4.5, qPrintable(Theme::name(scheme) + QString(" unavailable %1").arg(best(unavailable))));
        }
        Graphite::setLocked(locked, false);
        QVERIFY(!Graphite::isLocked(locked));
        Theme::setScheme(Theme::Scheme::Dark);
        const auto a = window.grab(QRect(locked->mapTo(&window, QPoint()), locked->size())).toImage();
        const auto b = window.grab(QRect(unavailable->mapTo(&window, QPoint()), unavailable->size())).toImage();
        QCOMPARE(a, b);
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
    // Graphite (2026-10-26): one painted style, no application stylesheet.
    void graphiteStylePaintsWithoutStylesheets() {
        QWidget probe;
        RedesignTheme::apply(probe);
        probe.show();
        QVERIFY(qobject_cast<Graphite::Style*>(qApp->style()));
        QVERIFY(qApp->styleSheet().isEmpty());
        QVERIFY(probe.styleSheet().isEmpty());
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);
            QCOMPARE(qApp->palette().color(QPalette::Window), Theme::palette(scheme).bg);
            QCOMPARE(probe.palette().color(QPalette::Window), Theme::palette(scheme).bg);
            // The painted indicator images still exist for this scheme (record overlays' fallback).
            for (const auto* name : {"check", "check@2x", "radio", "radio-checked", "chevron", "spin-up", "spin-down", "spin-up-off", "spin-down-off", "spin-up@2x"})
                QVERIFY2(QFile::exists(RedesignTheme::indicators(scheme) + "/" + name + ".png"), name);
        }
    }
    // A primary button is the neutral text fill with background-colour ink;
    // its label, alone or as one group with its icon, is centred.
    void primaryButtonIsNeutralAndCentred() {
        QWidget window;
        auto* layout = new QVBoxLayout(&window);
        auto* plain = new QPushButton("Upload", &window);
        auto* withIcon = new QPushButton(VisualStyle::icon("upload"), "Upload", &window);
        for (auto* button : {plain, withIcon}) { VisualStyle::role(button, "primary"); button->setMinimumWidth(220); layout->addWidget(button); }
        RedesignTheme::apply(window);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);
            const auto& p = Theme::palette(scheme);
            for (auto* button : {plain, withIcon}) {
                const auto shot = button->grab().toImage().convertToFormat(QImage::Format_RGB32);
                QCOMPARE(shot.pixelColor(shot.width() / 2, 3), p.text);
                // Scan inside the rounded corners (radius 7) and the outline, so
                // only label and icon ink can match.
                const QRect inside = shot.rect().adjusted(9, 4, -9, -4);
                int left = shot.width(), right = -1, top = shot.height(), bottom = -1;
                for (int y = inside.top(); y <= inside.bottom(); ++y)
                    for (int x = inside.left(); x <= inside.right(); ++x) {
                        const auto c = shot.pixelColor(x, y);
                        if (std::abs(c.red() - p.bg.red()) + std::abs(c.green() - p.bg.green()) + std::abs(c.blue() - p.bg.blue()) < 120) { left = std::min(left, x); right = std::max(right, x); top = std::min(top, y); bottom = std::max(bottom, y); }
                    }
                QVERIFY(right > left && bottom > top);
                // The ink is a label, not the whole button: well inside the scan area.
                QVERIFY(left > inside.left() + 20 && right < inside.right() - 20);
                QVERIFY2(std::abs((left + right) / 2.0 - (shot.width() - 1) / 2.0) <= 2, qPrintable(QString("%1-%2 of %3").arg(left).arg(right).arg(shot.width())));
                QVERIFY2(std::abs((top + bottom) / 2.0 - (shot.height() - 1) / 2.0) <= 2, qPrintable(QString("%1-%2 of %3").arg(top).arg(bottom).arg(shot.height())));
            }
        }
    }
    // A split button's menu half is a full 32 px target; its small chevron sits
    // centred in it, and clicking anywhere in it opens the menu.
    void splitButtonArrowZoneIsAFullTarget() {
        QWidget window;
        auto* layout = new QHBoxLayout(&window);
        auto* split = Graphite::toolButton("Capture region & edit", "edit", "quiet", &window);
        auto* menu = new QMenu(split);
        menu->addAction("Edit clipboard image");
        split->setMenu(menu);
        split->setPopupMode(QToolButton::MenuButtonPopup);
        auto* iconOnly = Graphite::toolButton("Capture region & edit", "edit", "quiet", &window);
        iconOnly->setMenu(menu);
        iconOnly->setPopupMode(QToolButton::MenuButtonPopup);
        iconOnly->setToolButtonStyle(Qt::ToolButtonIconOnly);
        layout->addWidget(split);
        layout->addWidget(iconOnly);
        RedesignTheme::apply(window);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        for (auto* button : {split, iconOnly}) {
            QStyleOptionToolButton option;
            option.initFrom(button);
            option.rect = button->rect();
            option.features = QStyleOptionToolButton::MenuButtonPopup | QStyleOptionToolButton::HasMenu;
            const auto arrow = button->style()->subControlRect(QStyle::CC_ToolButton, &option, QStyle::SC_ToolButtonMenu, button);
            QVERIFY2(arrow.width() >= Graphite::Metric::iconTarget, qPrintable(QString("arrow zone %1 px").arg(arrow.width())));
            QCOMPARE(arrow.right(), button->rect().right());
            // The action half keeps its own room beside it.
            const auto action = button->style()->subControlRect(QStyle::CC_ToolButton, &option, QStyle::SC_ToolButton, button);
            QVERIFY(!action.intersects(arrow));
            QVERIFY(action.width() >= Graphite::Metric::iconTarget);
            // The chevron's ink is small and centred in the zone, not stretched across it.
            const auto shot = button->grab().toImage().convertToFormat(QImage::Format_RGB32);
            const auto& p = Theme::current();
            int left = shot.width(), right = -1;
            for (int y = 8; y < shot.height() - 8; ++y)
                for (int x = arrow.left() + 2; x <= arrow.right() - 2; ++x)
                    if (Theme::contrast(shot.pixelColor(x, y), p.bg) > 3) { left = std::min(left, x); right = std::max(right, x); }
            QVERIFY(right > left);
            QVERIFY2(right - left <= 12, qPrintable(QString("chevron %1 px wide").arg(right - left)));
            QVERIFY2(std::abs((left + right) / 2.0 - (arrow.left() + arrow.right()) / 2.0) <= 2.5, qPrintable(QString("chevron %1-%2 in %3-%4").arg(left).arg(right).arg(arrow.left()).arg(arrow.right())));
        }
    }
    // Technical values (paths, URLs, identifiers) use Plex Mono at body size.
    void technicalValuesUseMonoAtBodySize() {
        QWidget window;
        auto* layout = new QVBoxLayout(&window);
        auto* field = new QLineEdit("screenshots/", &window);
        auto* value = new QLabel("https://captures.example.invalid/screenshots/Screenshot.png", &window);
        auto* name = new QLabel("Project archive", &window);
        for (QWidget* w : std::initializer_list<QWidget*>{field, value}) VisualStyle::role(w, "technical");
        for (QWidget* w : std::initializer_list<QWidget*>{field, value, name}) layout->addWidget(w);
        RedesignTheme::apply(window);
        for (QWidget* w : std::initializer_list<QWidget*>{field, value}) {
            QCOMPARE(w->font().family(), Graphite::monoFamily());
            QCOMPARE(w->font().pixelSize(), Graphite::scaled(Graphite::Type::body));
        }
        QVERIFY(name->font().family() != Graphite::monoFamily());
        // A technical label states a value; it is not painted as a link.
        QVERIFY(value->palette().color(QPalette::WindowText) != Theme::current().link);
    }
    // A revealable summary is a tab stop whose Enter shows its full plain text;
    // headings built on the same label stay out of the tab order.
    void revealableSummaryReadsFromTheKeyboard() {
        QWidget window;
        auto* layout = new QHBoxLayout(&window);
        auto* before = new QPushButton("Before", &window);
        auto* summary = new Graphite::ElidedLabel(&window);
        const QString full = "Capture output: Shared link · Production screenshots for customer support + local copy · 2 action exceptions";
        summary->setText(full);
        summary->setRevealable(true);
        auto* heading = new Graphite::ElidedLabel(&window);
        heading->setText("All items");
        layout->addWidget(before);
        layout->addWidget(summary, 1);
        layout->addWidget(heading);
        window.resize(360, 60);
        window.show();
        QVERIFY(putgd::test::waitForWindowActive(&window));
        QVERIFY(summary->elided());
        QCOMPARE(heading->focusPolicy(), Qt::NoFocus);
        before->setFocus(Qt::TabFocusReason);
        QTest::keyClick(before, Qt::Key_Tab);
        QCOMPARE(QApplication::focusWidget(), summary);
        QTest::keyClick(summary, Qt::Key_Return);
        QWidget* popup = summary->fullTextPopup();
        QVERIFY(popup && popup->isVisible());
        auto* text = popup->findChild<QLabel*>("elidedLabelFullTextValue");
        QVERIFY(text);
        QCOMPARE(text->text(), full);
        QCOMPARE(text->textFormat(), Qt::PlainText);
        QVERIFY(text->textInteractionFlags() & Qt::TextSelectableByKeyboard);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
        PUTGD_TRY_VERIFY(!summary->fullTextPopup());
        // A click opens it too.
        QTest::mouseClick(summary, Qt::LeftButton);
        QVERIFY(summary->fullTextPopup() && summary->fullTextPopup()->isVisible());
        summary->fullTextPopup()->close();
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
            // The box keeps a muted fill and a visible tick; unchecked has neither.
            const int inside = (boxOn.width() - 4) * (boxOn.height() - 4);
            QVERIFY2(count(boxOn, p.textMuted, 8) * 3 > inside, what("checked, disabled box is not on the muted fill"));
            QVERIFY2(count(boxOn, p.bg, 60) >= 6, what("checked, disabled box has no tick"));
            QCOMPARE(count(boxOff, p.textMuted, 60), 0);
            QVERIFY2(count(radioOn, p.textMuted, 60) >= 6, what("checked, disabled radio button has no dot"));
            QCOMPARE(count(radioOff, p.textMuted, 60), 0);
        }
    }
    // Essential inputs are found by their painted boundary: the left edge of a
    // text field, combo box, spin box and check box reads at 3:1 on the page
    // in both schemes, where the mixed border used to give about 1.6:1.
    void essentialInputBoundariesReachThreeToOne() {
        QWidget window;
        auto* layout = new QVBoxLayout(&window);
        auto* edit = new QLineEdit(&window);
        auto* combo = VisualStyle::choiceBox(&window);
        combo->addItem("Synthetic");
        auto* spin = new QSpinBox(&window);
        auto* box = new QCheckBox("Synthetic", &window);
        for (QWidget* widget : {static_cast<QWidget*>(edit), static_cast<QWidget*>(combo), static_cast<QWidget*>(spin), static_cast<QWidget*>(box)}) { widget->setMinimumWidth(160); layout->addWidget(widget); }
        RedesignTheme::apply(window);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);
            const auto& p = Theme::palette(scheme);
            for (QWidget* field : {static_cast<QWidget*>(edit), static_cast<QWidget*>(combo), static_cast<QWidget*>(spin)}) {
                const auto shot = field->grab().toImage().convertToFormat(QImage::Format_RGB32);
                const auto edge = shot.pixelColor(0, shot.height() / 2);
                QVERIFY2(Theme::contrast(edge, p.bg) >= 3.0 && Theme::contrast(edge, p.surfaceRaised) >= 3.0,
                         qPrintable(QString("%1 %2 edge %3").arg(Theme::name(scheme), field->metaObject()->className(), edge.name())));
            }
            QStyleOptionButton option;
            option.initFrom(box);
            const auto indicator = box->style()->subElementRect(QStyle::SE_CheckBoxIndicator, &option, box);
            const auto shot = box->grab(indicator).toImage().convertToFormat(QImage::Format_RGB32);
            double best = 0;
            for (int x = 0; x < shot.width() / 2; ++x) best = std::max(best, Theme::contrast(shot.pixelColor(x, shot.height() / 2), p.bg));
            QVERIFY2(best >= 3.0, qPrintable(Theme::name(scheme) + " check box edge"));
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
            option.frame = true;
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
    void schemeSwitchRestylesWithoutStylesheets() {
        QWidget plain;
        auto* label = new QLabel("Synthetic", &plain);
        label->setProperty("visualRole", "status");
        new QVBoxLayout(&plain);
        plain.layout()->addWidget(label);
        RedesignTheme::apply(plain);
        plain.show();
        QVERIFY(putgd::test::waitForWindowExposed(&plain));
        QWidget hidden;
        RedesignTheme::apply(hidden);
        hidden.resize(80, 60);
        QCOMPARE(plain.property("themeScheme").toString(), QString("dark"));
        const auto pixel = [&] { return plain.grab().toImage().pixelColor(label->geometry().center().x(), label->geometry().top() + 4); };
        QCOMPARE(pixel(), Theme::palette(Theme::Scheme::Dark).surface);
        QVERIFY(Theme::setScheme(Theme::Scheme::Light));
        QVERIFY(qApp->styleSheet().isEmpty());
        QCOMPARE(plain.property("themeScheme").toString(), QString("light"));
        QCOMPARE(pixel(), Theme::palette(Theme::Scheme::Light).surface);
        // The hidden window catches up when shown.
        hidden.show();
        QVERIFY(putgd::test::waitForWindowExposed(&hidden));
        QCOMPARE(hidden.property("themeScheme").toString(), QString("light"));
        QCOMPARE(hidden.grab().toImage().pixelColor(4, 4), Theme::palette(Theme::Scheme::Light).bg);
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
