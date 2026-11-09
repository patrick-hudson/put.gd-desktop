#include "ui/Appearance.h"
#include "ui/PanelTone.h"
#include "ui/Ui.h"
#include "ui/Theme.h"
#include "ui/ViewState.h"
#include "ui/Fonts.h"
#include "ui/IconArt.h"
#include <QFontInfo>
#include "ui/BlurRadius.h"
#include <QApplication>
#include <cmath>
#include <QContextMenuEvent>
#include <QListWidget>
#include <QStyledItemDelegate>
#include <QToolTip>
#include <QAccessible>
#include <QClipboard>
#include "platform/HostOpen.h"
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
#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusMessage>
#endif
#include <QSpinBox>
#include <QScreen>
#include <QScrollBar>
#include <QSlider>
#include <QScrollArea>
#include <QStyleOptionSlider>
#include <QStyleOptionSpinBox>
#include <QVBoxLayout>
#include <QStyleOptionToolButton>
#include <QHBoxLayout>
#include <QToolButton>
#include <QMenu>
#include <QMessageBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <memory>
#include <QtTest>
#include <QtMath>
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
    void preferenceSurfacesAndSecondaryTypeFollowTheScheme() {
        Ui::apply(*qApp);
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);
            const auto& palette = Theme::current();
            QCOMPARE(Theme::preferencePage(), scheme == Theme::Scheme::Light ? QColor("#F5F5F2") : palette.bg);
            QCOMPARE(Theme::preferenceCard(), scheme == Theme::Scheme::Light ? QColor("#FFFFFF") : palette.surface);
            QCOMPARE(Theme::preferenceBorder(), scheme == Theme::Scheme::Light ? QColor("#E2E1DB") : palette.border);
            QCOMPARE(Theme::preferenceSeparator(), scheme == Theme::Scheme::Light ? QColor("#ECEBE6") : palette.border);
            QVERIFY(Theme::contrast(palette.textSecondary, Theme::preferencePage()) >= 4.5);
            QVERIFY(Theme::contrast(palette.textSecondary, Theme::preferenceCard()) >= 4.5);
            QVERIFY(Theme::contrast(palette.bg, scheme == Theme::Scheme::Dark ? palette.textSecondary : palette.text) >= 3.0);
            for (const auto* role : {"secondary", "muted", "subtitle", "meta", "caption"}) {
                std::unique_ptr<QLabel> label(Ui::label("Readable context", role));
                QCOMPARE(label->font().pixelSize(), Ui::scaled(13));
                QCOMPARE(label->palette().color(QPalette::WindowText), palette.textSecondary);
            }
            Ui::Surface page("preferencePage");page.resize(100,60);page.show();
            QCOMPARE(page.grab().toImage().pixelColor(50,30), Theme::preferencePage());
            Ui::Surface card("surface");card.setProperty("preferenceCard",true);card.resize(100,60);card.show();
            QCOMPARE(card.grab().toImage().pixelColor(50,30), Theme::preferenceCard());
        }
    }

    void chromeTargetsAndTitleFitFollowTextSize() {
        const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);Ui::WindowHeader::setCustomFrameOverride(0);});Ui::WindowHeader::setCustomFrameOverride(1);
        QVERIFY(qApp->style()->pixelMetric(QStyle::PM_ScrollBarExtent)>=12);QCOMPARE(qApp->style()->pixelMetric(QStyle::PM_SplitterWidth),8);auto enlarged=original;enlarged.setPixelSize(21);qApp->setFont(enlarged);QVERIFY(qApp->style()->pixelMetric(QStyle::PM_ScrollBarExtent)>=16);
        QWidget window;QVBoxLayout layout(&window);layout.setContentsMargins(0,0,0,0);auto* header=Ui::WindowHeader::install(&window,&layout,nullptr,Ui::WindowHeader::Density::Compact);header->setDocument("Review recording");auto* state=new QLabel("Not uploaded");header->addTrailing(state);window.resize(680,200);window.show();putgd::test::settle(20);auto* title=header->findChild<QLabel*>("documentName");QVERIFY(title);QVERIFY(title->width()>=title->fontMetrics().horizontalAdvance(title->text()));QEvent inactive(QEvent::WindowDeactivate);QApplication::sendEvent(&window,&inactive);QVERIFY(title->property("headerInactive").toBool());QVERIFY(header->findChild<QToolButton*>("windowClose")->isEnabled());QEvent active(QEvent::WindowActivate);QApplication::sendEvent(&window,&active);QVERIFY(!title->property("headerInactive").toBool());window.resize(1200,200);PUTGD_VERIFY_STEADY(header->property("balancedTitle").toBool(),20);
    }
    void chromeButtonsLeaveRoomAroundScaledGlyphs() {
        const auto original = qApp->font();
        const auto restore = qScopeGuard([&] { qApp->setFont(original); Ui::WindowHeader::setCustomFrameOverride(0); });
        Ui::WindowHeader::setCustomFrameOverride(1);
        for (const qreal scale : {1.0, 1.25, 1.5}) {
            auto enlarged = original; enlarged.setPointSizeF(QFontInfo(original).pixelSize() * scale * 72.0 / 96.0); qApp->setFont(enlarged);
            QWidget window; QVBoxLayout layout(&window); QMenu menu;
            auto* header = Ui::WindowHeader::install(&window, &layout, &menu, Ui::WindowHeader::Density::Compact);
            window.resize(900, 200); window.show(); putgd::test::settle(10);
            for (const auto* name : {"applicationMenu", "windowMinimize", "windowMaximize", "windowClose"}) {
                auto* button = header->findChild<QToolButton*>(name); QVERIFY(button);
                QVERIFY(button->width() >= button->iconSize().width() + 8);
                QVERIFY(button->height() >= button->iconSize().height() + 8);
                QVERIFY(header->rect().contains(QRect(button->mapTo(header, QPoint()), button->size())));
            }
        }
    }
    void customHeaderOffersWindowActions() {
        Ui::WindowHeader::setCustomFrameOverride(1);
        auto restore=qScopeGuard([]{Ui::WindowHeader::setCustomFrameOverride(0);});
        QWidget window;QVBoxLayout layout(&window);auto* header=Ui::WindowHeader::install(&window,&layout);window.show();
        auto popup=[&]{QContextMenuEvent event(QContextMenuEvent::Mouse,QPoint(8,8),header->mapToGlobal(QPoint(8,8)));QApplication::sendEvent(header,&event);return header->findChild<QMenu*>("windowContextMenu");};
        auto* menu=popup();QVERIFY(menu);QCOMPARE(menu->actions().first()->text(),QString("Minimize"));QCOMPARE(menu->actions()[1]->text(),QString("Maximize"));menu->actions()[1]->trigger();menu->close();QVERIFY(window.isMaximized());QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        menu=popup();QVERIFY(menu);QCOMPARE(menu->actions()[1]->text(),QString("Restore"));menu->actions()[1]->trigger();menu->close();QVERIFY(!window.isMaximized());QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        menu=popup();QVERIFY(menu);QCOMPARE(menu->actions().last()->text(),QString("Close"));menu->actions().last()->trigger();QVERIFY(!window.isVisible());
    }

    void quietAndDisabledButtonsKeepReadableNeutralText() {
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);const auto& c=Theme::current();
            for(const auto* role:{"link","quiet"}){
                QPushButton button("Change output");Ui::role(&button,role);QStyleOptionButton option;option.rect=QRect(0,0,200,48);option.text=button.text();
                auto render=[&](QStyle::State state){option.state=state;QImage image(option.rect.size(),QImage::Format_RGB32);image.fill(c.bg);QPainter painter(&image);button.style()->drawControl(QStyle::CE_PushButton,&option,&painter,&button);return image;};
                for(const auto state:{QStyle::State(QStyle::State_Enabled),QStyle::State(QStyle::State_Enabled|QStyle::State_MouseOver),QStyle::State(QStyle::State_None)}){const auto image=render(state);int ink=0,amber=0;for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x){ink+=image.pixelColor(x,y)==(state&QStyle::State_Enabled?(QString(role)=="link"?c.link:c.textSecondary):c.textDisabled);amber+=image.pixelColor(x,y)==c.accent;}QVERIFY(ink>0);QCOMPARE(amber,0);}
                QCOMPARE(render(QStyle::State_None),render(QStyle::State_MouseOver|QStyle::State_Sunken));
            }
        }
    }

    void informationalMenuItemsRemainReadableAndNonInteractive() {
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);
            const auto& c = Theme::current();
            QMenu menu;
            auto* information = menu.addAction("Uploading… 50% · 2 MB of 4 MB");
            information->setProperty("visualRole", "information");
            information->setEnabled(false);
            auto* cancel = menu.addAction("Cancel upload");
            auto* unavailable = menu.addAction("Unavailable action");
            unavailable->setEnabled(false);
            menu.show();
            QCoreApplication::processEvents();
            const auto image = menu.grab().toImage();
            const auto rect = menu.actionGeometry(information);
            QVERIFY(!rect.isEmpty());
            QStyleOptionMenuItem measured;measured.text=information->text();measured.font=Ui::font(Ui::Type::micro);measured.menuItemType=QStyleOptionMenuItem::Normal;measured.menuHasCheckableItems=false;measured.maxIconWidth=0;
            const auto measuredSize=menu.style()->sizeFromContents(QStyle::CT_MenuItem,&measured,{},&menu);
            QCOMPARE(measuredSize.width(),QFontMetrics(Ui::font(Ui::Type::technical)).size(Qt::TextSingleLine|Qt::TextShowMnemonic,information->text()).width()+28);
            const auto background = image.pixelColor(rect.center().x(), rect.top());
            auto strongestInkContrast = [&](QRect bounds) {
                qreal contrast = 1;
                bounds.adjust(12, 1, -12, -1);
                for (int y = bounds.top(); y <= bounds.bottom(); ++y)
                    for (int x = bounds.left(); x <= bounds.right(); ++x)
                        contrast = qMax(contrast, Theme::contrast(image.pixelColor(x,y), background));
                return contrast;
            };
            // Subpixel font rendering need not contain any exact token pixels.
            QVERIFY(strongestInkContrast(rect) >= 4.5);
            const auto disabledContrast = strongestInkContrast(menu.actionGeometry(unavailable));
            QVERIFY(disabledContrast > 1);
            QVERIFY(disabledContrast <= Theme::contrast(c.textDisabled, background) + 0.02);
            QTest::keyClick(&menu, Qt::Key_Down);
            QCOMPARE(menu.activeAction(), cancel);
            menu.close();
        }
    }

    void blurRadiusSharesJoinedUnitAndLinkedSlider() {
        for (const bool compact : {false, true}) {
            BlurRadius radius(2,64,12,compact);radius.resize(compact?QSize(360,50):QSize(360,90));radius.show();putgd::test::settle(10);
            auto* spin=radius.spinBox();auto* unit=radius.unitLabel();QVERIFY(radius.label()->buddy()==spin);QCOMPARE(spin->suffix(),QString());
            QCOMPARE(unit->x(),spin->geometry().right()+1);QCOMPARE(unit->y(),spin->y());QCOMPARE(unit->height(),spin->height());
            spin->setFocus(Qt::TabFocusReason);spin->setValue(63);QTest::keyClick(spin,Qt::Key_Up);QCOMPARE(spin->value(),64);QTest::keyClick(spin,Qt::Key_Up);QCOMPARE(spin->value(),64);
            if (compact) QVERIFY(!radius.slider());
            else {QCOMPARE(radius.slider()->value(),64);radius.slider()->setValue(2);QCOMPARE(spin->value(),2);radius.resize(180,radius.sizeHint().height());PUTGD_COMPARE_STEADY(radius.slider()->width(),radius.width(),10);QVERIFY(radius.slider()->y()>spin->mapTo(&radius,spin->rect().bottomLeft()).y());}
        }
    }
    void sharedBlurWarningInvokesTheHostsRedactAction() {
        int calls=0;std::unique_ptr<QWidget> warning(Ui::blurWarning([&]{++calls;}));warning->resize(300,150);warning->show();putgd::test::settle(10);
        auto* action=warning->findChild<QToolButton*>("useRedact");auto* text=warning->findChild<QLabel*>("blurWarningText");QVERIFY(action&&text);QCOMPARE(text->text(),Ui::blurWarningText());QVERIFY(!text->text().contains(action->text()));QVERIFY(action->accessibleDescription().contains(text->text()));QVERIFY(action->accessibleDescription().contains(action->text()));QVERIFY(action->property("uiFlush").toBool());QCOMPARE(action->mapTo(warning.get(),QPoint()).x(),text->mapTo(warning.get(),QPoint(text->contentsMargins().left(),0)).x());
        action->setFocus(Qt::TabFocusReason);QTest::keyClick(action,Qt::Key_Space);QCOMPARE(calls,1);QVERIFY(text->geometry().bottom()<action->y());
        warning->setEnabled(false);QTest::mouseClick(action,Qt::LeftButton);QCOMPARE(calls,1);
    }
    void confirmationsSeparateQuestionFromConsequence() {
        std::unique_ptr<QMessageBox> box(Ui::confirm(nullptr,"Discard edits","Discard your edits?\n\nYour marks will be lost.","Discard","Keep editing",true));
        box->show();putgd::test::settle(10);
        auto* question=box->findChild<QLabel*>("qt_msgbox_label");auto* consequence=box->findChild<QLabel*>("qt_msgbox_informativelabel");
        QVERIFY(question&&consequence);QCOMPARE(question->font(),Ui::font(Ui::Type::body,true));
        QCOMPARE(consequence->palette().color(QPalette::WindowText),Theme::current().textSecondary);
        QCOMPARE(consequence->y()-question->geometry().bottom()-1,12);
        const auto* buttons=box->findChild<QDialogButtonBox*>();QVERIFY(buttons);QCOMPARE(buttons->y()-consequence->geometry().bottom()-1,16);
        QCOMPARE(box->layout()->contentsMargins(),QMargins(Ui::space(Ui::Space::xl),Ui::space(Ui::Space::l)+Ui::space(Ui::Space::xs),Ui::space(Ui::Space::xl),Ui::space(Ui::Space::l)));
        QCOMPARE(box->defaultButton(),box->button(QMessageBox::Cancel));QCOMPARE(box->escapeButton(),box->button(QMessageBox::Cancel));
    }
    void confirmationColumnsGrowWithTextAndFitTheScreen() {
        const auto original = qApp->font();
        const auto restore = qScopeGuard([&] { qApp->setFont(original); });
        int previous = 0;
        for (const int pixels : {14, 21}) {
            auto enlarged = original; enlarged.setPixelSize(pixels); qApp->setFont(enlarged);
            std::unique_ptr<QMessageBox> box(Ui::confirm(nullptr, "Reload settings",
                "Replace this window's settings with the configuration file as it is now?\nThose unsaved edits are discarded: credentials typed but not stored.", "Reload from file", "Cancel", true));
            box->show(); putgd::test::settle(10);
            auto* title = box->findChild<QLabel*>("qt_msgbox_label");
            auto* body = box->findChild<QLabel*>("qt_msgbox_informativelabel");
            QVERIFY(title && body); QCOMPARE(title->width(), body->width());
            QCOMPARE(title->font(), Ui::font(Ui::Type::body, true));
            QVERIFY(title->wordWrap());
            QVERIFY(title->contentsRect().width() < title->width());
            QVERIFY(title->heightForWidth(title->width()) >= 2 * title->fontMetrics().height());
            QVERIFY(title->width() <= Ui::scaled(420));
            QVERIFY(box->width() <= box->screen()->availableGeometry().width());
            QVERIFY(box->rect().contains(title->geometry())); QVERIFY(box->rect().contains(body->geometry()));
            if (previous) QVERIFY(box->width() >= previous * 1.35);
            previous = box->width();
        }
    }

    void selectedNavigationHasPersistentContrastingMarkers() {
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);const auto& c=Theme::current();
            QListWidget rows;rows.addItem("History");Ui::NavigationDelegate delegate(nullptr);
            QStyleOptionViewItem option;option.rect=QRect(0,0,200,44);option.state=QStyle::State_Selected|QStyle::State_Enabled;
            QImage image(200,44,QImage::Format_RGB32);image.fill(c.surface);{QPainter p(&image);delegate.paint(&p,option,rows.model()->index(0,0));}
            QCOMPARE(image.pixelColor(3,22),c.accent);QVERIFY(Theme::contrast(image.pixelColor(3,22),c.selected)>=3.0);QVERIFY(Theme::contrast(c.accent,c.surface)>=3.0);
            Ui::Segmented tabs;tabs.addTab("History");tabs.addTab("Settings");tabs.resize(tabs.sizeHint());tabs.show();tabs.clearFocus();putgd::test::settle(10);
            auto shot=tabs.grab().toImage();const auto r=tabs.tabRect(0);QCOMPARE(shot.pixelColor(r.center().x(),r.bottom()-7),c.accent);
            Ui::SegmentTrack track;auto* button=Ui::toolButton("List",{},"quiet",&track);track.addButton(button);button->setChecked(true);track.resize(track.sizeHint());track.show();putgd::test::settle(10);
            shot=button->grab().toImage();QCOMPARE(shot.pixelColor(button->rect().center().x(),button->rect().bottom()-3),c.accent);
        }
    }

    void iconButtonsNameThemselvesOnKeyboardFocus() {
        QWidget page;auto* row=new QHBoxLayout(&page);auto* first=Ui::toolButton("First","images","quiet",&page);auto* icon=Ui::toolButton("Edit an image","edit","quiet",&page);icon->setToolButtonStyle(Qt::ToolButtonIconOnly);icon->setToolTip("Edit an image");row->addWidget(first);row->addWidget(icon);page.show();page.activateWindow();first->setFocus();putgd::test::settle(10);
        QTest::keyClick(first,Qt::Key_Tab);PUTGD_TRY_VERIFY(icon->hasFocus());PUTGD_TRY_COMPARE(QToolTip::text(),QString("Edit an image"));
        QTest::keyClick(icon,Qt::Key_Backtab);PUTGD_TRY_VERIFY(first->hasFocus());PUTGD_TRY_VERIFY(!QToolTip::isVisible());
    }
    // A light desktop's tooltip colours (dark ink) on the dark tip panel left
    // hover text unreadable: Qt hands tips the desktop's tooltip palette again
    // on every desktop palette notification (KGlobalSettings, a colour scheme
    // change), after the app style set the application palette. Tips take their
    // ink from the scheme, live.
    void tooltipsReadOnTheirPanelWhateverTheDesktopPalette() {
        QPalette desktop;desktop.setColor(QPalette::ToolTipBase,Qt::white);desktop.setColor(QPalette::ToolTipText,Qt::black);
        auto restore=qScopeGuard([tips=QToolTip::palette()]{QToolTip::hideText();QToolTip::setPalette(tips);});
        QToolTip::setPalette(desktop);
        QWidget page;page.resize(200,100);page.show();QVERIFY(putgd::test::waitForWindowExposed(&page));
        const auto tip=[]()->QWidget*{for(auto* widget:QApplication::topLevelWidgets())if(widget->isVisible()&&QByteArray(widget->metaObject()->className())=="QTipLabel")return widget;return nullptr;};
        const auto readable=[](QWidget* label){const auto& p=Theme::current();const auto ink=label->palette().color(QPalette::ToolTipText);return ink==p.text&&Theme::contrast(ink,p.surfaceRaised)>=4.5;};
        QToolTip::showText(page.mapToGlobal(QPoint(20,20)),"Copy link",&page);
        PUTGD_TRY_VERIFY(tip());QVERIFY(readable(tip()));
        // A scheme switch while the tip is up recolours it.
        Theme::setScheme(Theme::Scheme::Light);
        PUTGD_TRY_VERIFY(tip()&&readable(tip()));
        QCOMPARE(QToolTip::palette().color(QPalette::ToolTipText),Theme::current().text);
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
                const auto thumb = shot.pixelColor(handle.center());
                QCOMPARE(thumb, p.borderStrong);
                int inkWidth = 0;
                for (int x = 0; x < shot.width(); ++x) inkWidth += shot.pixelColor(x, handle.center().y()) != surface;
                QCOMPARE(inkWidth, 6);
                QVERIFY(bar.style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, &bar) >= 12);
                QVERIFY2(Theme::contrast(thumb, surface) >= 3.0, qPrintable(Theme::name(scheme) + " scrollbar on " + surface.name()));
                for (const auto state : {QStyle::State_MouseOver, QStyle::State_Sunken}) {
                    option.state = QStyle::State_Enabled | state; option.activeSubControls = QStyle::SC_ScrollBarSlider;
                    shot.fill(surface); painter.begin(&shot); bar.style()->drawComplexControl(QStyle::CC_ScrollBar, &option, &painter, &bar); painter.end();
                    QVERIFY(Theme::contrast(shot.pixelColor(handle.center()), surface) > Theme::contrast(thumb, surface));
                }
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
                const auto groove=slider.style()->subControlRect(QStyle::CC_Slider,&option,QStyle::SC_SliderGroove,&slider);const auto trough=shot.pixelColor(groove.center());
                QCOMPARE(trough,c.controlOutline);QVERIFY2(Theme::contrast(trough,surface)>=3.0,qPrintable(Theme::name(scheme)+" slider on "+surface.name()));
            }
        }
    }

    void errorBannersUseTheirToneWithoutAnOptIn() {
        const auto previous = Theme::scheme();
        const auto restore = qScopeGuard([previous] { Theme::setScheme(previous); });
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);
            Ui::Banner banner(Ui::Banner::Tone::Error, "Save failed");
            banner.resize(500, 48); banner.show();
            const auto shot = banner.grab().toImage();
            QCOMPARE(shot.pixelColor(250, 0), Theme::current().dangerSurface);
            QCOMPARE(shot.pixelColor(250, 24), Theme::current().dangerSurface);
            QCOMPARE(banner.textLabel()->palette().color(QPalette::WindowText), Theme::current().text);
            QVERIFY(Theme::contrast(Theme::current().text, Theme::current().dangerSurface) >= 4.5);
            banner.setAction("Try again");
            QCOMPARE(banner.actionButton()->property("visualRole").toString(), QString("textAction"));
            QVERIFY(banner.actionButton()->property("uiStrong").toBool());
            QCOMPARE(banner.actionButton()->font().weight(), QFont::DemiBold);
            auto* content = new QWidget(&banner); auto* row = new QHBoxLayout(content);
            auto* message = Ui::label("Couldn't save", "meta", content);
            auto* details = Ui::toolButton("Details", {}, "secondary", content);
            row->addWidget(message); row->addWidget(details); banner.setContentWidget(content);
            QCOMPARE(message->palette().color(QPalette::WindowText), Theme::current().text);
            QCOMPARE(message->font().pixelSize(), Ui::scaled(Ui::Type::technical));
            QCOMPARE(details->property("visualRole").toString(), QString("textAction"));
            QVERIFY(details->property("uiStrong").toBool());
            QCOMPARE(details->font().weight(), QFont::DemiBold);
            banner.setTone(Ui::Banner::Tone::Info);
            QCOMPARE(details->property("visualRole").toString(), QString("textAction"));
            QVERIFY(details->property("uiStrong").toBool());
            QCOMPARE(message->palette().color(QPalette::WindowText), Theme::current().textSecondary);
            for (const auto tone : {Ui::Banner::Tone::Info, Ui::Banner::Tone::Warning, Ui::Banner::Tone::Success}) {
                banner.setTone(tone);
                QCOMPARE(details->font().weight(), QFont::DemiBold);
                QCOMPARE(banner.actionButton()->font().weight(), QFont::DemiBold);
            }
            banner.setTone(Ui::Banner::Tone::Error);
            QCOMPARE(message->palette().color(QPalette::WindowText), Theme::current().text);
            QCOMPARE(details->font().weight(), QFont::DemiBold);
        }
    }
    void trayBadgesKeepRecordingVisibleWithLatchedFailure() {
        for (const bool light : {false, true}) {
            const auto& tints = Theme::trayTints(light);
            QCOMPARE(tints.idle, Theme::palette(light ? Theme::Scheme::Light : Theme::Scheme::Dark).icon);
            const auto idle = IconArt::liftTray(tints.idle, -1, 4);
            const auto failed = IconArt::liftTray(tints.idle, -2, 4);
            for (const int state : {1, 2}) {
                const auto combined = IconArt::liftTray(tints.idle, -2, 4, state);
                QVERIFY(combined != failed);
                QCOMPARE(combined.copy(0, 0, 12 * 4, 24 * 4), failed.copy(0, 0, 12 * 4, 24 * 4));
                QCOMPARE(combined.pixelColor(15 * 4, 5 * 4), state == 1 ? tints.live : tints.idle);
                if (state == 2) for (int x : {17, 20}) QCOMPARE(combined.pixelColor(x * 4, 5 * 4).alpha(), 0);
            }
            for (const int side : {16, 20, 24, 48}) for (const int state : {1, 2}) {
                const auto combined = Ui::icon("tray", tints.idle, -2, state).pixmap(side, side).toImage();
                const auto failure = Ui::icon("tray", tints.error, -2).pixmap(side, side).toImage();
                QVERIFY(combined != failure);
                QCOMPARE(combined.copy(0, 0, side / 2, side), failure.copy(0, 0, side / 2, side));
            }
            for (const int state : {0, 1, 2}) {
                const auto image = IconArt::liftTray(tints.idle, state ? -1 : -2, 4, state);
                // The knockout ring clears the original arrow at the same corner.
                QVERIFY(idle.pixelColor(13 * 4, 7 * 4).alpha() > 0);
                QCOMPARE(image.pixelColor(13 * 4, 7 * 4).alpha(), 0);
                const auto base = state ? idle : IconArt::liftTray(tints.error, -1, 4);
                QCOMPARE(image.copy(0, 0, 12 * 4, 24 * 4), base.copy(0, 0, 12 * 4, 24 * 4));
                QCOMPARE(image.pixelColor(15 * 4, 5 * 4), state == 0 ? tints.error : state == 1 ? tints.live : tints.idle);
                if (state == 0) { QCOMPARE(image.pixelColor(18 * 4 + 3, 3 * 4).alpha(), 0); QCOMPARE(image.pixelColor(18 * 4 + 3, 7 * 4 + 3).alpha(), 0); }
                if (state == 2) for (int x : {17, 20}) QCOMPARE(image.pixelColor(x * 4, 5 * 4).alpha(), 0);
                for (const int side : {16, 20, 24, 48}) {
                    const auto small = Ui::icon("tray", tints.idle, state ? -1 : -2, state).pixmap(side, side).toImage();
                    const auto plain = Ui::icon("tray", state ? tints.idle : tints.error).pixmap(side, side).toImage();
                    QCOMPARE(small.size(), QSize(side, side));
                    QCOMPARE(plain.size(), QSize(side, side));
                    int disc = 0, holes = 0;
                    for (int y = 1; y < side * 9 / 24; ++y) for (int x = side * 15 / 24; x < side * 23 / 24; ++x) {
                        disc += small.pixelColor(x, y).alpha() > 230;
                        holes += small.pixelColor(x, y).alpha() < 128;
                    }
                    QVERIFY2(disc >= 3, qPrintable(QString("badge %1 at %2 px has %3 solid pixels").arg(state).arg(side).arg(disc)));
                    if (state != 1) QVERIFY2(holes >= 2, qPrintable(QString("badge %1 at %2 px has %3 knockout pixels").arg(state).arg(side).arg(holes)));
                    QCOMPARE(small.copy(0, 0, side / 2, side), plain.copy(0, 0, side / 2, side));
                }
            }
        }
    }
    void liveIndicatorsMeetContrastWithoutReusingDanger() {
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            const auto& p=Theme::palette(scheme);
            QVERIFY(p.live!=p.danger);
            for (const auto& background : {p.bg,p.surface,p.surfaceRaised,p.selected}) QVERIFY(Theme::contrast(p.live,background)>=3.0);
            const bool light=scheme==Theme::Scheme::Light;
            const auto& tints=Theme::trayTints(light);
            for (const auto& tint : {tints.idle,tints.upload,tints.encoding,tints.error}) {
                const auto badge=IconArt::liftTray(tint,-1,2,1);
                QCOMPARE(badge.pixelColor(38,10),tints.live);
            }
            for (const auto& panel : {light?QColor("#ffffff"):QColor("#141414"),light?QColor("#dedee2"):QColor("#3b3b3f")}) QVERIFY(Theme::contrast(tints.live,panel)>=3.0);
        }
    }
    void warningFeedbackAndStatePillsKeepNeutralText() {
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme); const auto& c = Theme::current();
            QLabel feedback("Review this choice"); feedback.setProperty("feedbackTone", "warning"); Ui::role(&feedback, "meta");
            QCOMPARE(feedback.palette().color(QPalette::WindowText), c.textSecondary);
            QVERIFY(Theme::contrast(c.textSecondary, c.warningSurface) >= 4.5);
            QListWidget view; Ui::decorateView(&view, {0});
            for (const auto state : {"partial", "pending", "running"}) {
                view.clear(); view.addItem(state);
                QStyleOptionViewItem option; option.initFrom(&view); option.widget = &view; option.rect = QRect(0, 0, 180, 40);
                option.font = view.font(); option.fontMetrics = QFontMetrics(option.font);
                QImage image(option.rect.size(), QImage::Format_RGB32); image.fill(c.bg);
                { QPainter painter(&image); view.itemDelegate()->paint(&painter, option, view.model()->index(0, 0)); }
                const bool warning = QString(state) == "partial";
                QCOMPARE(image.pixelColor(20, 8), warning ? c.warningSurface : c.surfaceRaised);
                int neutralInk = 0;
                const auto fill = warning ? c.warningSurface : c.surfaceRaised;
                for (int y = 8; y < 32; ++y) for (int x = 18; x < 100; ++x) {
                    const auto pixel = image.pixelColor(x, y);
                    neutralInk += Theme::contrast(pixel, fill) >= 3.0 && std::abs(pixel.red() - c.textSecondary.red()) < 25
                        && std::abs(pixel.green() - c.textSecondary.green()) < 25 && std::abs(pixel.blue() - c.textSecondary.blue()) < 25;
                }
                QVERIFY(neutralInk > 5);
            }
        }
    }
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
            // The outline is not the decorative border: section rules stay quiet.
            QVERIFY(Theme::contrast(Ui::mix(p.bg, p.border, p.border.alphaF()), p.bg) < 2.0);
            QVERIFY(Theme::contrast(p.onAccent, p.accent) >= 4.5);
            QVERIFY(Theme::contrast(p.onAccent, p.accentHover) >= 4.5);
            QVERIFY(Theme::contrast(p.success, p.successSurface) >= 4.5);
            QVERIFY(Theme::contrast(p.warning, p.warningSurface) >= 4.5);
            QVERIFY(Theme::contrast(p.danger, p.dangerSurface) >= 4.5);
            // Every token is reported, as #rrggbb.
            const auto tokens = Theme::tokens(scheme);
            QCOMPARE(tokens.size(), 32);
            QVERIFY(tokens.contains("primary"));
            QVERIFY(tokens.contains("primaryHover"));
            QVERIFY(tokens.contains("controlOutline"));
            for (const auto& value : tokens) QVERIFY(QRegularExpression("^#[0-9a-f]{6}$").match(value.toString()).hasMatch());
        }
        QCOMPARE(Theme::tokens(Theme::Scheme::Dark)["bg"].toString(), QString("#191919"));
        QCOMPARE(Theme::tokens(Theme::Scheme::Light)["bg"].toString(), QString("#ffffff"));
        // Amber, never blue, and a neutral primary action.
        QCOMPARE(Theme::tokens(Theme::Scheme::Dark)["accent"].toString(), QString("#e8b66d"));
        QCOMPARE(Theme::tokens(Theme::Scheme::Dark)["controlOutline"].toString(), QString("#7c7c78"));
        QCOMPARE(Theme::tokens(Theme::Scheme::Light)["controlOutline"].toString(), QString("#82827b"));
        QVERIFY(Theme::tokens(Theme::Scheme::Light)["bg"] != Theme::tokens(Theme::Scheme::Dark)["bg"]);
    }

    void bannerGlyphsRemainVisibleOnTheirSurface() {
        for(const auto scheme:{Theme::Scheme::Dark,Theme::Scheme::Light}) for(const auto tone:{Ui::Banner::Tone::Info,Ui::Banner::Tone::Warning,Ui::Banner::Tone::Error,Ui::Banner::Tone::Success}) {
            Theme::setScheme(scheme);Ui::Banner banner(tone,"A readable explanation.");banner.resize(420,90);banner.show();putgd::test::settle(10);
            const auto image=banner.grab().toImage();int readable=0;
            for(int y=2;y<image.height()-2;++y)for(int x=Ui::scaled(12);x<Ui::scaled(12+Ui::Metric::icon);++x)readable+=Theme::contrast(image.pixelColor(x,y),Theme::current().surface)>=3.0;
            QVERIFY(readable>0);
        }
    }

    void helpGlyphStaysMutedAcrossSchemesAndKeepsKeyboardDisclosure() {
        const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) for (const int pixels : {14,21}) {
            auto font=original;font.setPixelSize(pixels);qApp->setFont(font);Theme::setScheme(scheme);
            QWidget page;auto* layout=new QVBoxLayout(&page);page.setProperty("inlineHelpRow",true);
            auto* control=new QLineEdit(&page);control->setObjectName("testHelp");layout->addWidget(control);
            auto* help=Ui::helpButton("Term","An explanation.",control,&page);layout->addWidget(help);page.show();PUTGD_COMPARE_STEADY(help->size(),QSize(Ui::scaled(24),Ui::scaled(24)),10);
            QCOMPARE(help->iconSize(),QSize(Ui::scaled(14),Ui::scaled(14)));
            const auto art=help->icon().pixmap(48,48).toImage();int ink=0;
            for(int y=0;y<art.height();++y)for(int x=0;x<art.width();++x){const auto pixel=art.pixelColor(x,y);if(pixel.alpha()<255)continue;QCOMPARE(pixel,Theme::current().textMuted);++ink;}
            QVERIFY(ink>10);QCOMPARE(art.pixelColor(16,24).alpha(),0);
            help->setFocus();QTest::keyClick(help,Qt::Key_Space);auto* note=page.findChild<QLabel*>("testHelpHelpText");QVERIFY(note&&note->isVisible());QTest::keyClick(help,Qt::Key_Space);QVERIFY(note->isHidden());
        }
    }
    void selectedOutlineIconsReadOnTheSelectedFill() {
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);
            const auto& p = Theme::current();
            QVERIFY(Theme::contrast(p.iconSelected, p.selected) >= 3.0);
            const auto icon = Ui::icon("settings");
            const auto normal = icon.pixmap(QSize(36,36), QIcon::Normal).toImage(), selected = icon.pixmap(QSize(36,36), QIcon::Selected).toImage();
            QVERIFY(normal != selected);
            int stroke = 0;
            for (int y = 0; y < selected.height(); ++y) for (int x = 0; x < selected.width(); ++x) {
                const auto colour = selected.pixelColor(x,y);
                if (colour.alpha() < 250) continue;
                QVERIFY(std::abs(colour.red()-p.iconSelected.red()) + std::abs(colour.green()-p.iconSelected.green()) + std::abs(colour.blue()-p.iconSelected.blue()) < 12);
                ++stroke;
            }
            QVERIFY(stroke > 10);
            QCOMPARE(selected.pixelColor(18,18).alpha(), 0);
        }
    }
    // Custom-framed windows carry only their resize grips: no line is painted
    // along any edge, in either scheme.

    // A locked value (Ui::setLocked) reads as information: its text is
    // textMuted at 4.5:1 on the page. Unavailable controls keep readable
    // textDisabled at 3:1 and omit interaction. Unlocking restores their shared look.
    void lockedValuesStayReadable() {
        QWidget window;
        auto* layout = new QVBoxLayout(&window);
        auto* locked = Ui::choiceBox(&window);
        auto* unavailable = Ui::choiceBox(&window);
        for (auto* combo : {locked, unavailable}) { combo->addItem("Synthetic value"); combo->setMinimumWidth(200); combo->setEnabled(false); layout->addWidget(combo); }
        Ui::setLocked(locked, true);
        Ui::styleWindow(window);
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
            QVERIFY2(best(unavailable) >= 3.0, qPrintable(Theme::name(scheme) + QString(" unavailable %1").arg(best(unavailable))));
        }
        Ui::setLocked(locked, false);
        QVERIFY(!Ui::isLocked(locked));
        Theme::setScheme(Theme::Scheme::Dark);
        const auto a = window.grab(QRect(locked->mapTo(&window, QPoint()), locked->size())).toImage();
        const auto b = window.grab(QRect(unavailable->mapTo(&window, QPoint()), unavailable->size())).toImage();
        QCOMPARE(a, b);
    }
    void lockedReadoutsKeepMutedTextContrast() {
        QWidget window;
        QVBoxLayout layout(&window);
        QComboBox choice; choice.setObjectName("lockedChoice"); choice.addItem("Forever");
        QComboBox editable; editable.setObjectName("lockedEditableChoice"); editable.setEditable(true); editable.addItem("Saved value");
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
                for (const auto role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText})
                    QCOMPARE(widget->palette().color(QPalette::Disabled, role), c.textMuted);
                const auto shot = widget->grab().toImage();
                int ink = 0;
                // Native editors use subpixel antialiasing: their glyph cores
                // can approach the token without containing its exact RGB.
                for (int y = 0; y < shot.height(); ++y) for (int x = 0; x < shot.width(); ++x) {
                    const auto pixel = shot.pixelColor(x, y);
                    const int distance = std::abs(pixel.red() - c.textMuted.red()) + std::abs(pixel.green() - c.textMuted.green()) + std::abs(pixel.blue() - c.textMuted.blue());
                    ink += distance <= 12 && Theme::contrast(pixel, c.bg) >= 4.5;
                }
                QVERIFY2(ink > 0, qPrintable(QString(widget->metaObject()->className()) + " " + widget->objectName()));
                for (const auto& surface : {c.bg, c.surface, c.surfaceRaised, c.selected}) {
                    QVERIFY(Theme::contrast(c.textMuted, surface) >= 4.5);
                    QVERIFY(Theme::contrast(c.textMuted, surface) > Theme::contrast(c.textDisabled, surface));
                }
            }
        }
    }
    void trayTintsSuitTheirPanels() {
        // Typical panels: GNOME Shell's and Mint-Y-Dark's dark bars, Breeze's light one.
        for (const auto& panel : {QColor("#000000"), QColor("#1d1d1d"), QColor("#2f343f")})
            for (const auto& tint : {Theme::trayTints(false).idle, Theme::trayTints(false).upload, Theme::trayTints(false).encoding, Theme::trayTints(false).error})
                QVERIFY2(Theme::contrast(tint, panel) >= 3.0, qPrintable(tint.name() + " on " + panel.name()));
        for (const auto& panel : {QColor("#ffffff"), QColor("#dee0e2"), QColor("#eff0f1")})
            for (const auto& tint : {Theme::trayTints(true).idle, Theme::trayTints(true).upload, Theme::trayTints(true).encoding, Theme::trayTints(true).error})
                QVERIFY2(Theme::contrast(tint, panel) >= 3.0, qPrintable(tint.name() + " on " + panel.name()));
        // Status hues come from the same semantic palette as the windows.
        for(const auto scheme:{Theme::Scheme::Dark,Theme::Scheme::Light}){
            const auto& p=Theme::palette(scheme);const auto& t=Theme::trayTints(scheme==Theme::Scheme::Light);
            QVERIFY(std::abs(t.upload.hsvHue()-p.success.hsvHue())<3);QVERIFY(std::abs(t.encoding.hsvHue()-p.accent.hsvHue())<3);QVERIFY(std::abs(t.error.hsvHue()-p.danger.hsvHue())<3);
        }
    }
    // One painted style, no application stylesheet (2026-10-26).

    // A primary button is the neutral action fill with background-colour ink;
    // its label, alone or as one group with its icon, is centred.
    void chevronsFollowTextScale() {
        const auto original = qApp->font();
        const auto restore = qScopeGuard([&] { qApp->setFont(original); });
        QHash<int, int> previousSize, previousInk;
        for (const qreal scale : {1.0, 1.25, 1.5}) {
            auto enlarged = original; enlarged.setPointSizeF(QFontInfo(original).pixelSize() * scale * 72.0 / 96.0); qApp->setFont(enlarged);
            for (int kind = 0; kind < 7; ++kind) {
                QImage image(360, 240, QImage::Format_ARGB32_Premultiplied); image.setDevicePixelRatio(4); image.fill(Qt::transparent);
                QPainter painter(&image); QStyleOption option; option.rect = QRect(0, 0, 90, 60); option.state = QStyle::State_Enabled;
                QWidget quiet; quiet.setProperty("visualRole", "quiet");
                if (kind < 2) {
                    option.state |= QStyle::State_Children | QStyle::State_Open;
                    qApp->style()->drawPrimitive(kind == 0 ? QStyle::PE_IndicatorArrowDown : QStyle::PE_IndicatorBranch, &option, &painter);
                } else if (kind == 2) {
                    QStyleOptionComboBox combo; combo.rect = option.rect; combo.state = option.state; combo.frame = false;
                    qApp->style()->drawComplexControl(QStyle::CC_ComboBox, &combo, &painter);
                } else if (kind == 3) {
                    QStyleOptionButton button; button.rect = option.rect; button.state = option.state; button.features = QStyleOptionButton::HasMenu;
                    qApp->style()->drawControl(QStyle::CE_PushButtonBevel, &button, &painter, &quiet);
                } else if (kind < 6) {
                    QStyleOptionToolButton button; button.rect = option.rect; button.state = option.state | QStyle::State_AutoRaise;
                    button.text = " "; button.toolButtonStyle = Qt::ToolButtonTextOnly;
                    button.features = kind == 4 ? QStyleOptionToolButton::HasMenu : QStyleOptionToolButton::MenuButtonPopup;
                    qApp->style()->drawComplexControl(QStyle::CC_ToolButton, &button, &painter, &quiet);
                } else {
                    QStyleOptionMenuItem menu; menu.rect = option.rect; menu.state = option.state; menu.menuItemType = QStyleOptionMenuItem::SubMenu;
                    qApp->style()->drawControl(QStyle::CE_MenuItem, &menu, &painter);
                }
                painter.end(); QRect bounds; int ink = 0;
                for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
                    const auto pixel = image.pixelColor(x, y);
                    if (pixel.alpha() > 64 && Theme::contrast(pixel, Theme::current().surfaceRaised) >= 3.0) { bounds |= QRect(x, y, 1, 1); ++ink; }
                }
                const int size = std::max(bounds.width(), bounds.height());
                QVERIFY2(size > previousSize.value(kind), qPrintable(QString("Chevron %1 at scale %2: %3 px after %4").arg(kind).arg(scale).arg(size).arg(previousSize.value(kind))));
                QVERIFY2(ink > previousInk.value(kind), qPrintable(QString("Chevron %1 at scale %2: %3 ink pixels after %4").arg(kind).arg(scale).arg(ink).arg(previousInk.value(kind)))); previousSize[kind] = size; previousInk[kind] = ink;
            }
            for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
                const auto folder = Ui::indicators(scheme);
                QCOMPARE(QImage(folder + "/chevron.png").size(), QSize(Ui::scaled(12), Ui::scaled(12)));
                QCOMPARE(QImage(folder + "/chevron@2x.png").size(), QSize(Ui::scaled(12), Ui::scaled(12)) * 2);
            }
        }
    }
    void compactControlsMeasureTheirPaintedTypeAndScale() {
        const auto original = QApplication::font();
        const auto restore = qScopeGuard([&] { QApplication::setFont(original); });
        for (const qreal scale : {1.0, 1.25, 1.5}) {
            auto enlarged = original; enlarged.setPointSizeF(QFontInfo(original).pixelSize() * scale * 72.0 / 96.0); QApplication::setFont(enlarged);
            QPushButton push("Start here");
            QToolButton tool; tool.setText("End here"); tool.setToolButtonStyle(Qt::ToolButtonTextOnly);
            QComboBox combo; combo.addItem("Compatible MP4");
            for (auto* widget : {static_cast<QWidget*>(&push), static_cast<QWidget*>(&tool), static_cast<QWidget*>(&combo)}) {
                widget->ensurePolished();
                const int standard = widget->sizeHint().height();
                QVERIFY(standard >= Ui::scaled(Ui::Metric::control));
                widget->setProperty("uiDensity", "compact");
                QCOMPARE(widget->font().pixelSize(), Ui::scaled(13));
                QCOMPARE(widget->font().weight(), QFont::Normal);
                const auto hint = widget->sizeHint();
                QCOMPARE(hint.height(), Ui::scaled(Ui::Metric::controlCompact));
                QVERIFY(hint.height() <= standard);
                QVERIFY(hint.width() >= widget->fontMetrics().horizontalAdvance(qobject_cast<QComboBox*>(widget) ? combo.currentText() : qobject_cast<QAbstractButton*>(widget)->text()));
                widget->setProperty("uiDensity", {});
                QCOMPARE(widget->font().pixelSize(), Ui::scaled(Ui::Type::body));
                QCOMPARE(widget->sizeHint().height(), standard);
            }
            combo.setFrame(false);
            combo.setProperty("uiDensity", "compact");
            QCOMPARE(combo.sizeHint().height(), Ui::scaled(Ui::Metric::controlCompact));
        }
    }
    void popupRowsKeepPaddingAroundScaledText() {
        const auto original = qApp->font();
        const auto restore = qScopeGuard([&] { qApp->setFont(original); });
        int previousCombo = 0, previousMenu = 0;
        for (const qreal scale : {1.0, 1.25, 1.5}) {
            auto enlarged = original; enlarged.setPointSizeF(QFontInfo(original).pixelSize() * scale * 72.0 / 96.0); qApp->setFont(enlarged);
            QComboBox combo; combo.addItem("Compatible MP4"); combo.show(); combo.showPopup(); putgd::test::settle(10);
            const int comboRow = combo.view()->sizeHintForRow(0);
            QVERIFY(comboRow >= combo.view()->fontMetrics().height() + Ui::scaled(12));
            QVERIFY(comboRow > previousCombo); previousCombo = comboRow;
            combo.hidePopup();
            QMenu menu; auto* action = menu.addAction("Save a copy…"); menu.popup(QPoint(0, 0)); putgd::test::settle(10);
            const int menuRow = menu.actionGeometry(action).height();
            QVERIFY(menuRow >= menu.fontMetrics().height() + Ui::scaled(12));
            QVERIFY(menuRow > previousMenu); previousMenu = menuRow;
            menu.close();
        }
    }
    void stepperAndComboArrowTargetsGrowWithText() {
        const auto original = qApp->font();
        const auto restore = qScopeGuard([&] { qApp->setFont(original); });
        int previousStep = 0, previousArrow = 0;
        for (const qreal scale : {1.0, 1.25, 1.5}) {
            auto enlarged = original; enlarged.setPointSizeF(QFontInfo(original).pixelSize() * scale * 72.0 / 96.0); qApp->setFont(enlarged);
            QSpinBox spin; spin.setRange(0, 100); spin.setValue(42); spin.ensurePolished(); spin.resize(spin.sizeHint()); spin.show();
            QComboBox combo; combo.addItem("Compatible MP4"); combo.ensurePolished(); combo.resize(combo.sizeHint());
            for (const auto direction : {Qt::LeftToRight, Qt::RightToLeft}) {
                spin.setLayoutDirection(direction); combo.setLayoutDirection(direction);
                QStyleOptionSpinBox option; option.initFrom(&spin); option.frame = true; option.buttonSymbols = QAbstractSpinBox::UpDownArrows;
                const auto up = spin.style()->subControlRect(QStyle::CC_SpinBox, &option, QStyle::SC_SpinBoxUp, &spin);
                const auto down = spin.style()->subControlRect(QStyle::CC_SpinBox, &option, QStyle::SC_SpinBoxDown, &spin);
                const auto edit = spin.style()->subControlRect(QStyle::CC_SpinBox, &option, QStyle::SC_SpinBoxEditField, &spin);
                QVERIFY(up.width() > previousStep); QCOMPARE(up.width(), down.width());
                QVERIFY(spin.rect().contains(up)); QVERIFY(spin.rect().contains(down));
                QVERIFY(!edit.intersects(up)); QVERIFY(!edit.intersects(down)); QVERIFY(!up.intersects(down));
                QTest::mouseClick(&spin, Qt::LeftButton, Qt::NoModifier, up.center()); QCOMPARE(spin.value(), 43);
                QTest::mouseClick(&spin, Qt::LeftButton, Qt::NoModifier, down.center()); QCOMPARE(spin.value(), 42);
                QStyleOptionComboBox choice; choice.initFrom(&combo); choice.frame = true;
                const auto arrow = combo.style()->subControlRect(QStyle::CC_ComboBox, &choice, QStyle::SC_ComboBoxArrow, &combo);
                QVERIFY(arrow.width() > previousArrow); QVERIFY(combo.rect().contains(arrow));
            }
            previousStep = Ui::scaled(32); previousArrow = Ui::scaled(Ui::Metric::splitZone) + 2;
        }
    }

    // A split button's menu half is a full 32 px target; its small chevron sits
    // centred in it, and clicking anywhere in it opens the menu.
    void splitButtonKeepsTextClearOfTheDividerAtLargeText() {
        Ui::apply(*qApp);
        const auto original = QApplication::font();
        const auto restore = qScopeGuard([original] { QApplication::setFont(original); });
        auto enlarged = original; enlarged.setPixelSize(21); QApplication::setFont(enlarged);
        for (const bool tight : {false, true}) {
            class Button final : public QToolButton { public: QStyleOptionToolButton option() const { QStyleOptionToolButton value; initStyleOption(&value); return value; } };
            Button button;
            button.setText("Capture region && edit"); button.setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            button.setIcon(Ui::icon("edit")); button.setIconSize(QSize(24,24));
            button.setProperty("uiTight", tight); Ui::role(&button, "quiet");
            QMenu menu; menu.addAction("Capture desktop"); button.setMenu(&menu);
            button.setPopupMode(QToolButton::MenuButtonPopup); button.ensurePolished(); button.resize(button.sizeHint());
            QVERIFY(button.font().pixelSize() >= 21);
            const auto option = button.option();
            const auto action = button.style()->subControlRect(QStyle::CC_ToolButton, &option, QStyle::SC_ToolButton, &button);
            const auto label = QFontMetricsF(button.font()).horizontalAdvance("Capture region & edit") + button.iconSize().width() + 8;
            QVERIFY2(action.width() >= std::ceil(label) + Ui::space(Ui::Space::s) + int(tight ? Ui::Space::xs : Ui::Space::m), qPrintable(QString("tight %1 action %2 label %3 space %4 font %5 icon %6 hint %7").arg(tight).arg(action.width()).arg(label).arg(Ui::space(Ui::Space::s)).arg(button.font().pixelSize()).arg(button.iconSize().width()).arg(button.sizeHint().width())));
        }
    }
    void splitButtonArrowZoneIsAFullTarget() {
        QWidget window;
        auto* layout = new QHBoxLayout(&window);
        auto* split = Ui::toolButton("Capture region & edit", "edit", "quiet", &window);
        auto* menu = new QMenu(split);
        menu->addAction("Edit clipboard image");
        split->setMenu(menu);
        split->setPopupMode(QToolButton::MenuButtonPopup);
        auto* iconOnly = Ui::toolButton("Capture region & edit", "edit", "quiet", &window);
        iconOnly->setMenu(menu);
        iconOnly->setPopupMode(QToolButton::MenuButtonPopup);
        iconOnly->setToolButtonStyle(Qt::ToolButtonIconOnly);
        layout->addWidget(split);
        layout->addWidget(iconOnly);
        Ui::styleWindow(window);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        for (auto* button : {split, iconOnly}) {
            QStyleOptionToolButton option;
            option.initFrom(button);
            option.rect = button->rect();
            option.features = QStyleOptionToolButton::MenuButtonPopup | QStyleOptionToolButton::HasMenu;
            const auto arrow = button->style()->subControlRect(QStyle::CC_ToolButton, &option, QStyle::SC_ToolButtonMenu, button);
            QVERIFY2(arrow.width() >= Ui::Metric::iconTarget, qPrintable(QString("arrow zone %1 px").arg(arrow.width())));
            QCOMPARE(arrow.right(), button->rect().right());
            // The action half keeps its own room beside it.
            const auto action = button->style()->subControlRect(QStyle::CC_ToolButton, &option, QStyle::SC_ToolButton, button);
            QVERIFY(!action.intersects(arrow));
            QVERIFY(action.width() >= Ui::Metric::iconTarget);
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
    // Technical values use the 13 px Plex Mono role.

    void revealableSummaryHasPersistentDisclosureAndHover() {
        Ui::ElidedLabel label;label.resize(220,36);label.setRevealable(true);label.show();label.clearFocus();putgd::test::settle(10);
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);label.setAttribute(Qt::WA_UnderMouse,false);const auto idle=label.grab().toImage();
            label.setRevealable(false);const auto plain=label.grab().toImage();QVERIFY(idle!=plain);label.setRevealable(true);
            label.setAttribute(Qt::WA_UnderMouse,true);const auto hover=label.grab().toImage();QVERIFY(hover!=idle);
            label.setAttribute(Qt::WA_UnderMouse,false);
        }
    }
    void summaryDisclosureFollowsShortTextInAStretchingFooter() {
        Ui::ElidedLabel label;
        label.resize(800, 36);
        label.setText("Private upload to Project archive");
        label.show();label.clearFocus();label.setAttribute(Qt::WA_UnderMouse, false);
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);
            label.setRevealable(true);const auto summary = label.grab().toImage();
            label.setRevealable(false);const auto plain = label.grab().toImage();
            const int end = 6 + int(std::ceil(QFontMetricsF(label.font()).horizontalAdvance(label.text())));
            int changed = 0, right = 0;
            for (int y = 0; y < summary.height(); ++y) for (int x = end + 6; x < summary.width(); ++x) {
                if (summary.pixel(x, y) != plain.pixel(x, y)) {++changed;right = std::max(right, x);}
            }
            QVERIFY(changed > 0);
            QVERIFY(right <= end + 6 + Ui::scaled(14));
        }
    }
    void revealedTextWidthFollowsItsNaturalTextInsteadOfItsOwner() {
        const auto original = qApp->font();
        const auto restore = qScopeGuard([&] { qApp->setFont(original); });
        for (const auto& text : {QString("Private upload"), QString("Capture output: Shared link · Production screenshots for customer support + local copy · 2 action exceptions")}) {
            int previous = 0;
            for (const qreal scale : {1.0, 1.25, 1.5}) {
                auto enlarged = original; enlarged.setPointSizeF(QFontInfo(original).pixelSize() * scale * 72.0 / 96.0); qApp->setFont(enlarged);
                Ui::ElidedLabel label; label.setFont(Ui::font()); label.setText(text); label.setRevealable(true);
                label.resize(scale == 1.0 ? 700 : 180, 60); label.show(); label.reveal();
                auto* popup = label.fullTextPopup(); QVERIFY(popup && popup->isVisible());
                const int maximum = std::min(Ui::scaled(560), popup->screen()->availableGeometry().width() - 16);
                QVERIFY(popup->width() >= std::min(Ui::scaled(360), maximum));
                QVERIFY(popup->width() <= maximum); QVERIFY(popup->width() >= previous);
                previous = popup->width(); popup->close();
            }
        }
    }
    // A revealable summary is a tab stop whose Enter shows its full plain text;
    // headings built on the same label stay out of the tab order.
    void revealableSummaryReadsFromTheKeyboard() {
        QWidget window;
        auto* layout = new QHBoxLayout(&window);
        auto* before = new QPushButton("Before", &window);
        auto* summary = new Ui::ElidedLabel(&window);
        const QString full = "Capture output: Shared link · Production screenshots for customer support + local copy · 2 action exceptions";
        summary->setText(full);
        summary->setRevealable(true);
        auto* interface = QAccessible::queryAccessibleInterface(summary);
        QVERIFY(interface);
        QCOMPARE(interface->role(), QAccessible::Button);
        auto* actions = interface->actionInterface();
        QVERIFY(actions);
        QVERIFY(actions->actionNames().contains(QAccessibleActionInterface::pressAction()));
        QCOMPARE(actions->localizedActionName(QAccessibleActionInterface::pressAction()), QString("Show full text"));
        summary->setRevealActionName("Show full save path");
        QCOMPARE(actions->localizedActionName(QAccessibleActionInterface::pressAction()), QString("Show full save path"));
        QVERIFY(actions->actionNames().contains(QAccessibleActionInterface::setFocusAction()));
        auto* heading = new Ui::ElidedLabel(&window);
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
        // Read-only details keep keyboard selection without an editing caret.
        bool caretHidden = false;
        for (auto* control : text->findChildren<QObject*>(QString(), Qt::FindDirectChildrenOnly))
            if (QByteArray(control->metaObject()->className()) == "QWidgetTextControl") caretHidden = control->property("cursorWidth").toInt() == 0;
        QVERIFY(caretHidden);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
        PUTGD_TRY_VERIFY(!summary->fullTextPopup());
        // A click opens it too.
        QTest::mouseClick(summary, Qt::LeftButton);
        QVERIFY(summary->fullTextPopup() && summary->fullTextPopup()->isVisible());
        summary->fullTextPopup()->close();
        putgd::test::settle(20);
        summary->setRevealText("Resolved action outputs");
        actions->doAction(QAccessibleActionInterface::pressAction());
        QVERIFY(summary->fullTextPopup());
        QCOMPARE(summary->fullTextPopup()->findChild<QLabel*>("elidedLabelFullTextValue")->text(), QString("Resolved action outputs"));
        summary->fullTextPopup()->close();
        QCOMPARE(QAccessible::queryAccessibleInterface(heading)->role(), QAccessible::StaticText);
    }
    void browserFailureCopiesExactLinkAndRetryClearsIt() {
        QWidget window;
        auto* layout = new QVBoxLayout(&window);
        auto* scroll = new QScrollArea(&window);
        scroll->setWidgetResizable(true);
        auto* body = new QWidget;
        auto* column = new QVBoxLayout(body);
        auto* preceding = new QWidget(body);
        preceding->setFixedHeight(300);
        column->addWidget(preceding);
        auto* feedback = new Ui::BrowserFeedback(body);
        column->addWidget(feedback);
        scroll->setWidget(body);
        layout->addWidget(scroll);
        window.resize(400, 220);
        window.show();
        const QUrl refused("unsupported://synthetic-browser-link/path?value=one%20two");
        host::openLink(refused, feedback->opening(refused));
        QVERIFY(feedback->isVisible());
        PUTGD_TRY_VERIFY(scroll->viewport()->rect().contains(QRect(feedback->mapTo(scroll->viewport(), QPoint()), feedback->size())));
        QVERIFY(feedback->findChild<QLabel*>()->text().contains("Couldn't open the browser"));
        auto* copy = feedback->findChild<QPushButton*>();
        QCOMPARE(copy->text(), QString("Copy link"));
        copy->click();
        QCOMPARE(qApp->clipboard()->text(), refused.toString(QUrl::FullyEncoded));
        auto earlier = feedback->opening(QUrl("https://example.invalid/earlier"));
        const QUrl latest("https://example.invalid/latest");
        auto current = feedback->opening(latest);
        earlier({{"opened", false}});
        QVERIFY(feedback->isHidden());
        current({{"opened", false}});
        QVERIFY(feedback->isVisible());
        copy->click();
        QCOMPARE(qApp->clipboard()->text(), latest.toString(QUrl::FullyEncoded));
        feedback->clear();
        current({{"opened", false}});
        QVERIFY(feedback->isHidden());
        QUrl opened;
        host::setTestOpener([&](const QUrl& url) { opened = url; });
        auto restore = qScopeGuard([] { host::setTestOpener({}); });
        const QUrl accepted("https://example.invalid/release");
        host::openLink(accepted, feedback->opening(accepted));
        QCOMPARE(opened, accepted);
        QVERIFY(feedback->isHidden());
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
        Ui::styleWindow(window);
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
            QCOMPARE(count(boxOff.copy(4,4,boxOff.width()-8,boxOff.height()-8), p.textMuted, 60), 0);
            QVERIFY2(count(radioOn, p.textMuted, 60) >= 6, what("checked, disabled radio button has no dot"));
            QCOMPARE(count(radioOff.copy(4,4,radioOff.width()-8,radioOff.height()-8), p.textMuted, 60), 0);
        }
    }
    // Essential inputs are found by their painted boundary: the left edge of a
    // text field, combo box, spin box and check box reads at 3:1 on the page
    // in both schemes, where the mixed border used to give about 1.6:1.
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
    // Shared minus/plus targets retain native editing and keyboard stepping.
    void spinBoxStepsUseAdjacentTargets() {
        QWidget window;
        auto* layout = new QVBoxLayout(&window);
        auto* spin = new QSpinBox(&window);
        spin->setRange(0, 100);
        spin->setValue(50);
        spin->setMinimumWidth(120);
        layout->addWidget(spin);
        Ui::styleWindow(window);
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
                QVERIFY2(box.width() >= 32 && box.height() >= 32, "step target smaller than 32 px");
                const int before = spin->value();
                QTest::mouseClick(spin, Qt::LeftButton, {}, box.center());
                QCOMPARE(spin->value(), before + (control == QStyle::SC_SpinBoxUp ? 1 : -1));
                int glyph = 0, field = 0;
                for (int y = box.top(); y <= box.bottom(); ++y)
                    for (int x = box.left(); x <= box.right(); ++x) {
                        const auto c = shot.pixelColor(x, y);
                        const auto near = [&](const QColor& t, int d) { return std::abs(c.red() - t.red()) + std::abs(c.green() - t.green()) + std::abs(c.blue() - t.blue()) < d; };
                        if (near(p.textSecondary, 90)) ++glyph;
                        if (near(p.surfaceRaised, 12)) ++field;
                    }
                QVERIFY2(glyph >= 4, qPrintable(Theme::name(scheme) + ": no step glyph"));
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
        Ui::styleWindow(plain);
        plain.show();
        QVERIFY(putgd::test::waitForWindowExposed(&plain));
        QWidget hidden;
        Ui::styleWindow(hidden);
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
        button->setIcon(Ui::icon("history"));
        layout->addWidget(button);
        Ui::styleWindow(window);
        window.show();
        QVERIFY(putgd::test::waitForWindowExposed(&window));
        QCOMPARE(window.palette().color(QPalette::Window), Theme::palette(Theme::Scheme::Dark).bg);
        const auto darkGlyph = centre(Ui::icon("history"));
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
    // #702: Cinnamon's portal does not serve org.cinnamon.theme; dconf's change
    // notice does, as a key or a directory prefix and the paths under it.
    void panelToneCinnamonDconfPaths_data() {
        QTest::addColumn<QString>("prefix");
        QTest::addColumn<QStringList>("changes");
        QTest::addColumn<bool>("theme");
        QTest::newRow("gsettings set: the key itself") << "/org/cinnamon/theme/name" << QStringList{""} << true;
        QTest::newRow("several keys under the schema") << "/org/cinnamon/theme/" << QStringList{"name", "symbolic-relative-size"} << true;
        QTest::newRow("several schemas, one the theme") << "/org/cinnamon/" << QStringList{"desktop/interface/gtk-theme", "theme/name"} << true;
        QTest::newRow("reset of /org/cinnamon/") << "/org/cinnamon/" << QStringList{""} << true;
        QTest::newRow("reset of everything") << "/" << QStringList{""} << true;
        QTest::newRow("GTK theme only") << "/org/cinnamon/desktop/interface/gtk-theme" << QStringList{""} << false;
        QTest::newRow("a sibling schema directory") << "/org/cinnamon/" << QStringList{"desktop/", "themes/name"} << false;
        QTest::newRow("a key named like the schema") << "/org/cinnamon/theme" << QStringList{""} << false;
        QTest::newRow("another desktop") << "/org/gnome/desktop/interface/" << QStringList{"gtk-theme"} << false;
    }
    void panelToneCinnamonDconfPaths() {
        QFETCH(QString, prefix);
        QFETCH(QStringList, changes);
        QFETCH(bool, theme);
        QCOMPARE(PanelTone::cinnamonThemeChanged(prefix, changes), theme);
    }
    // A desktop theme chosen on its own re-tints the tray: the resident re-reads
    // the theme on dconf's notice, and only on one about the theme. Runs on the
    // private bus of ctest's gui_theme_cinnamon_dconf, never a desktop's own.
    void panelToneFollowsCinnamonDconfNotice() {
#ifndef Q_OS_LINUX
        QSKIP("dconf is Linux only");
#else
        if (qEnvironmentVariable("PUTGD_ISOLATED_DCONF_TEST") != "1" || !QDBusConnection::sessionBus().isConnected())
            QSKIP("needs the private session bus of ctest's gui_theme_cinnamon_dconf");
        QTemporaryDir bin;
        QVERIFY(bin.isValid());
        // gsettings as the resident runs it: prints the theme the "desktop" has
        // now and records each read.
        const auto theme = bin.filePath("theme"), reads = bin.filePath("reads");
        auto write = [](const QString& path, const QByteArray& text) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(text) == text.size(); };
        QVERIFY(write(theme, "Mint-Y-Dark-Aqua"));
        QVERIFY(write(bin.filePath("gsettings"), "#!/bin/sh\necho read >> '" + reads.toUtf8() + "'\nprintf \"'%s'\\n\" \"$(cat '" + theme.toUtf8() + "')\"\n"));
        QVERIFY(QFile::setPermissions(bin.filePath("gsettings"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const QByteArray saved[] = {qgetenv("PATH"), qgetenv("XDG_CURRENT_DESKTOP")};
        auto restore = qScopeGuard([&] {
            const char* names[] = {"PATH", "XDG_CURRENT_DESKTOP"};
            for (int i = 0; i < 2; ++i) saved[i].isNull() ? qunsetenv(names[i]) : qputenv(names[i], saved[i]);
        });
        qputenv("PATH", bin.path().toLocal8Bit() + ":" + saved[0]);
        qputenv("XDG_CURRENT_DESKTOP", "X-Cinnamon");
        auto readCount = [&] { QFile file(reads); return file.open(QIODevice::ReadOnly) ? file.readAll().count('\n') : 0; };
        auto notice = [](const QString& prefix, const QStringList& changes, const QString& writer = "user") {
            auto signal = QDBusMessage::createSignal("/ca/desrt/dconf/Writer/" + writer, "ca.desrt.dconf.Writer", "Notify");
            signal << prefix << changes << QString(":1.1:user:1");
            QVERIFY(QDBusConnection::sessionBus().send(signal));
        };
        PanelTone tone;
        PUTGD_TRY_COMPARE(tone.source(), QString("cinnamon-theme"));
        QVERIFY(!tone.light());
        QCOMPARE(readCount(), 1);
        QSignalSpy changed(&tone, &PanelTone::changed);
        // Another setting: no re-read, past the 300 ms debounce.
        QVERIFY(write(theme, "Mint-Y"));
        notice("/org/cinnamon/desktop/interface/gtk-theme", {""});
        PUTGD_COMPARE_STEADY(readCount(), 1, 700);
        QVERIFY(!tone.light());
        // The desktop theme itself, as `gsettings set org.cinnamon.theme name` reports it.
        notice("/org/cinnamon/theme/name", {""});
        PUTGD_TRY_VERIFY(tone.light());
        QCOMPARE(readCount(), 2);
        QCOMPARE(changed.count(), 1);
        // And back, as a reset of the whole Cinnamon tree reports it, from a
        // profile whose user-db is not named "user".
        QVERIFY(write(theme, "Mint-Y-Dark"));
        notice("/org/cinnamon/", {""}, "custom");
        PUTGD_TRY_VERIFY(!tone.light());
        QCOMPARE(changed.count(), 2);
#endif
    }
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Ui::apply(app);
    ThemeTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_theme.moc"
