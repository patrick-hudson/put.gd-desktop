// WCAG contrast ratios of the theme in both schemes: a ratio slightly under its
// threshold is invisible in the rendered catalog. Runs on pushes to nightly
// (ctest label `nightly`); `ctest -R gui_theme_contrast` runs it locally.
#include "ui/Ui.h"
#include "ui/Theme.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QStyleOptionButton>
#include <QStyleOptionSlider>
#include <QVBoxLayout>
#include <QtTest>
#include <cmath>
#include "fixtures/Waits.h"

using namespace putgd;

class ThemeTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { Ui::apply(*qApp); }
    void init() { Theme::setScheme(Theme::Scheme::Dark); }
    void textAndFocusMeetAaInBothSchemes() {
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            const auto& p = Theme::palette(scheme);
            const QList<QPair<const char*, QColor>> texts{{"text", p.text}, {"textSecondary", p.textSecondary}, {"textMuted", p.textMuted}, {"textDisabled", p.textDisabled}, {"link", p.link},
                                                          {"success", p.success}, {"danger", p.danger}};
            const QList<QPair<const char*, QColor>> backgrounds{{"bg", p.bg}, {"surface", p.surface}, {"surfaceRaised", p.surfaceRaised}, {"selected", p.selected}};
            for (const auto& [name, text] : texts)
                for (const auto& [background, colour] : backgrounds)
                    QVERIFY2(Theme::contrast(text, colour) >= (QString(name) == "textDisabled" ? 3.0 : 4.5), qPrintable(QString("%1 %2 on %3: %4").arg(Theme::name(scheme), name, background).arg(Theme::contrast(text, colour))));
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
            QVERIFY(Theme::contrast(p.onAccent, p.accent) >= 4.5);
            QVERIFY(Theme::contrast(p.onAccent, p.accentHover) >= 4.5);
            QVERIFY(Theme::contrast(p.success, p.successSurface) >= 4.5);
            QVERIFY(Theme::contrast(p.warning, p.warningSurface) >= 4.5);
            QVERIFY(Theme::contrast(p.danger, p.dangerSurface) >= 4.5);
        }
    }
    void selectedNavigationHasPersistentContrastingMarkers() {
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);const auto& c=Theme::current();
            QListWidget rows;rows.addItem("History");Ui::NavigationDelegate delegate(nullptr);
            QStyleOptionViewItem option;option.rect=QRect(0,0,200,44);option.state=QStyle::State_Selected|QStyle::State_Enabled;
            QImage image(200,44,QImage::Format_RGB32);image.fill(c.surface);{QPainter p(&image);delegate.paint(&p,option,rows.model()->index(0,0));}
            QVERIFY(Theme::contrast(image.pixelColor(3,22),c.selected)>=3.0);QVERIFY(Theme::contrast(c.accent,c.surface)>=3.0);
        }
    }
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
                QVERIFY2(Theme::contrast(shot.pixelColor(handle.center()), surface) >= 3.0, qPrintable(Theme::name(scheme) + " scrollbar on " + surface.name()));
            }
        }
    }
    void sliderTroughMeetsContrastOnEverySurface() {
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);const auto& c=Theme::current();
            for (const auto surface : {c.bg, c.surface, c.surfaceRaised}) for (const auto orientation : {Qt::Horizontal, Qt::Vertical}) {
                QSlider slider(orientation);slider.resize(orientation==Qt::Horizontal?QSize(180,36):QSize(36,180));
                QStyleOptionSlider option;option.initFrom(&slider);option.rect=slider.rect();option.orientation=orientation;option.minimum=0;option.maximum=100;option.sliderPosition=0;option.sliderValue=0;option.state=QStyle::State_Enabled;option.subControls=QStyle::SC_SliderGroove|QStyle::SC_SliderHandle;
                QImage shot(slider.size(),QImage::Format_RGB32);shot.fill(surface);{QPainter painter(&shot);slider.style()->drawComplexControl(QStyle::CC_Slider,&option,&painter,&slider);}
                const auto groove=slider.style()->subControlRect(QStyle::CC_Slider,&option,QStyle::SC_SliderGroove,&slider);
                QVERIFY2(Theme::contrast(shot.pixelColor(groove.center()),surface)>=3.0,qPrintable(Theme::name(scheme)+" slider on "+surface.name()));
            }
        }
    }
    void liveIndicatorsMeetContrast() {
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            const auto& p=Theme::palette(scheme);
            for (const auto& background : {p.bg,p.surface,p.surfaceRaised,p.selected}) QVERIFY(Theme::contrast(p.live,background)>=3.0);
            const bool light=scheme==Theme::Scheme::Light;
            const auto& tints=Theme::trayTints(light);
            // Typical plain and tinted panels of each brightness.
            for (const auto& panel : {light?QColor("#ffffff"):QColor("#141414"),light?QColor("#dedee2"):QColor("#3b3b3f")}) QVERIFY(Theme::contrast(tints.live,panel)>=3.0);
        }
    }
    void lockedReadoutsKeepMutedTextContrast() {
        QWidget window;
        QVBoxLayout layout(&window);
        QComboBox choice; choice.addItem("Forever");
        QComboBox editable; editable.setEditable(true); editable.addItem("Saved value");
        QSpinBox number; number.setValue(42);
        QLineEdit value("Saved path"); value.setReadOnly(true);
        QPushButton fact("Default");
        for (auto* widget : QList<QWidget*>{&choice, &editable, &number, &value, &fact}) {
            Ui::setLocked(widget, true);
            widget->setEnabled(false);
            layout.addWidget(widget);
        }
        Ui::styleWindow(window); window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);
            const auto& c = Theme::current();
            for (auto* widget : QList<QWidget*>{&choice, &editable, editable.lineEdit(), &number, number.findChild<QLineEdit*>(), &value, &fact}) {
                QVERIFY(widget);
                const auto shot = widget->grab().toImage();
                int ink = 0;
                // Native editors use subpixel antialiasing: their glyph cores
                // can approach the token without containing its exact RGB.
                for (int y = 0; y < shot.height(); ++y) for (int x = 0; x < shot.width(); ++x) {
                    const auto pixel = shot.pixelColor(x, y);
                    const int distance = std::abs(pixel.red() - c.textMuted.red()) + std::abs(pixel.green() - c.textMuted.green()) + std::abs(pixel.blue() - c.textMuted.blue());
                    ink += distance <= 12 && Theme::contrast(pixel, c.bg) >= 4.5;
                }
                QVERIFY2(ink > 0, qPrintable(QString(widget->metaObject()->className()) + " in " + Theme::name(scheme)));
            }
        }
    }
    void essentialInputBoundariesReachThreeToOne() {
        QWidget window;
        auto* layout = new QVBoxLayout(&window);
        auto* edit = new QLineEdit(&window);
        auto* combo = Ui::choiceBox(&window);
        combo->addItem("Synthetic");
        auto* spin = new QSpinBox(&window);
        auto* box = new QCheckBox("Synthetic", &window);
        for (QWidget* widget : {static_cast<QWidget*>(edit), static_cast<QWidget*>(combo), static_cast<QWidget*>(spin), static_cast<QWidget*>(box)}) { widget->setMinimumWidth(160); layout->addWidget(widget); }
        Ui::styleWindow(window);
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
};

QTEST_MAIN(ThemeTest)
#include "test_theme.moc"
