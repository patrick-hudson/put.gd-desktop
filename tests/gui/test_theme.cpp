#include "ui/Appearance.h"
#include "ui/PanelTone.h"
#include "ui/RedesignTheme.h"
#include "ui/Theme.h"
#include "ui/ViewState.h"
#include "ui/VisualStyle.h"
#include "ui/Graphite.h"
#include "ui/Fonts.h"
#include "ui/IconArt.h"
#include <QFontInfo>
#include <QFontDatabase>
#include "ui/BlurRadius.h"
#include <QApplication>
#include <cmath>
#include <QContextMenuEvent>
#include <QFormLayout>
#include <QListWidget>
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
#include <QSpinBox>
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
#include <QProgressBar>
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
    void sharedTypeRolesKeepTheirWeightsAndHeadingScale() {
        Fonts::load();
        QVERIFY(QFontDatabase::styles(Graphite::uiFamily()).contains("Medium"));
        const auto original = qApp->font();
        auto restore = qScopeGuard([&] { qApp->setFont(original); });
        const int baseline = QFontInfo(original).pixelSize();
        for (const int body : {14, 21}) {
            auto appFont = original; appFont.setPixelSize(qRound(baseline * body / 14.0)); qApp->setFont(appFont);
            QCOMPARE(Graphite::font().pixelSize(), body);
            QCOMPARE(Graphite::font(Graphite::Type::caption).pixelSize(), body == 14 ? 12 : 18);
            QCOMPARE(Graphite::font(Graphite::Type::section, true).pixelSize(), body == 14 ? 16 : 21);
            QCOMPARE(Graphite::font(Graphite::Type::page, true).pixelSize(), body == 14 ? 22 : 29);
            QCOMPARE(Graphite::font(Graphite::Type::compactPage, true).pixelSize(), body == 14 ? 20 : 26);
            QVERIFY2(std::abs(Graphite::font(Graphite::Type::page, true).letterSpacing() + 0.2) <= 1.0 / 64, qPrintable(QString::number(Graphite::font(Graphite::Type::page, true).letterSpacing())));
            QCOMPARE(Graphite::labelFont().weight(), QFont::Normal);
            QCOMPARE(Graphite::labelFont(true).weight(), QFont::Medium);
            QCOMPARE(Graphite::font(Graphite::Type::caption, true).weight(), QFont::Normal);
            QCOMPARE(Graphite::font(Graphite::Type::section, true).weight(), QFont::DemiBold);
            std::unique_ptr<QLabel> section(Graphite::label("Appearance", "section"));
            QCOMPARE(section->contentsMargins().top(), Graphite::scaled(24));
            QCOMPARE(section->contentsMargins().bottom(), Graphite::scaled(8));
            QVERIFY(section->minimumHeight() >= Graphite::lineHeight(Graphite::Type::section) + Graphite::scaled(32));
            QCOMPARE(Graphite::space(Graphite::Space::l), Graphite::scaled(16));
        }
    }
    void chromeTargetsAndTitleFitFollowTextSize() {
        const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);Graphite::WindowHeader::setCustomFrameOverride(0);});Graphite::WindowHeader::setCustomFrameOverride(1);
        QVERIFY(qApp->style()->pixelMetric(QStyle::PM_ScrollBarExtent)>=12);QCOMPARE(qApp->style()->pixelMetric(QStyle::PM_SplitterWidth),8);auto enlarged=original;enlarged.setPixelSize(21);qApp->setFont(enlarged);QVERIFY(qApp->style()->pixelMetric(QStyle::PM_ScrollBarExtent)>=16);
        QWidget window;QVBoxLayout layout(&window);layout.setContentsMargins(0,0,0,0);auto* header=Graphite::WindowHeader::install(&window,&layout,nullptr,Graphite::WindowHeader::Density::Compact);header->setDocument("Review recording");auto* state=new QLabel("Not uploaded");header->addTrailing(state);window.resize(680,200);window.show();putgd::test::settle(20);auto* title=header->findChild<QLabel*>("documentName");QVERIFY(title);QVERIFY(title->width()>=title->fontMetrics().horizontalAdvance(title->text()));QEvent inactive(QEvent::WindowDeactivate);QApplication::sendEvent(&window,&inactive);QVERIFY(title->property("headerInactive").toBool());QVERIFY(header->findChild<QToolButton*>("windowClose")->isEnabled());QEvent active(QEvent::WindowActivate);QApplication::sendEvent(&window,&active);QVERIFY(!title->property("headerInactive").toBool());window.resize(1200,200);putgd::test::settle(20);QVERIFY(header->property("balancedTitle").toBool());
    }
    void customHeaderOffersWindowActions() {
        Graphite::WindowHeader::setCustomFrameOverride(1);
        auto restore=qScopeGuard([]{Graphite::WindowHeader::setCustomFrameOverride(0);});
        QWidget window;QVBoxLayout layout(&window);auto* header=Graphite::WindowHeader::install(&window,&layout);window.show();
        auto popup=[&]{QContextMenuEvent event(QContextMenuEvent::Mouse,QPoint(8,8),header->mapToGlobal(QPoint(8,8)));QApplication::sendEvent(header,&event);return header->findChild<QMenu*>("windowContextMenu");};
        auto* menu=popup();QVERIFY(menu);QCOMPARE(menu->actions().first()->text(),QString("Minimize"));QCOMPARE(menu->actions()[1]->text(),QString("Maximize"));menu->actions()[1]->trigger();menu->close();QVERIFY(window.isMaximized());QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        menu=popup();QVERIFY(menu);QCOMPARE(menu->actions()[1]->text(),QString("Restore"));menu->actions()[1]->trigger();menu->close();QVERIFY(!window.isMaximized());QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        menu=popup();QVERIFY(menu);QCOMPARE(menu->actions().last()->text(),QString("Close"));menu->actions().last()->trigger();QVERIFY(!window.isVisible());
    }
    void warningTextAndNeutralSelectionFollowThePalette() {
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);const auto& c=Theme::current();
            std::unique_ptr<QLabel> warning(Graphite::warningLabel("Warning: Blur is a visual effect."));warning->resize(320,72);warning->show();putgd::test::settle(10);
            QCOMPARE(warning->palette().color(QPalette::WindowText),c.textSecondary);QVERIFY(warning->contentsMargins().left()>=Graphite::scaled(Graphite::Metric::icon));
            const auto image=warning->grab().toImage();int amber=0;for(int y=0;y<image.height();++y)for(int x=0;x<warning->contentsMargins().left();++x){const auto pixel=image.pixelColor(x,y);if(pixel.red()-pixel.blue()>20&&Theme::contrast(pixel,c.bg)>=3.0)++amber;}QVERIFY(amber>0);
            VisualStyle::role(warning.get(),"secondary");QCOMPARE(warning->contentsMargins().left(),0);
            QImage neutral(80,40,QImage::Format_RGB32);neutral.fill(c.bg);{QPainter painter(&neutral);Graphite::paintSelection(painter,QRectF(0,0,80,40));}QCOMPARE(neutral.pixelColor(40,20),c.selected);QCOMPARE(neutral.pixelColor(40,0),c.selected);
            QImage focused=neutral;{QPainter painter(&focused);Graphite::paintSelection(painter,QRectF(0,0,80,40),true);}QCOMPARE(focused.pixelColor(40,0),c.accent);
        }
    }
    void quietAndDisabledButtonsKeepReadableNeutralText() {
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);const auto& c=Theme::current();
            for(const auto* role:{"link","quiet"}){
                QPushButton button("Change output");VisualStyle::role(&button,role);QStyleOptionButton option;option.rect=QRect(0,0,200,48);option.text=button.text();
                auto render=[&](QStyle::State state){option.state=state;QImage image(option.rect.size(),QImage::Format_RGB32);image.fill(c.bg);QPainter painter(&image);button.style()->drawControl(QStyle::CE_PushButton,&option,&painter,&button);return image;};
                for(const auto state:{QStyle::State(QStyle::State_Enabled),QStyle::State(QStyle::State_Enabled|QStyle::State_MouseOver),QStyle::State(QStyle::State_None)}){const auto image=render(state);int ink=0,amber=0;for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x){ink+=image.pixelColor(x,y)==(state&QStyle::State_Enabled?c.textSecondary:c.textDisabled);amber+=image.pixelColor(x,y)==c.accent;}QVERIFY(ink>0);QCOMPARE(amber,0);}
                QCOMPARE(render(QStyle::State_None),render(QStyle::State_MouseOver|QStyle::State_Sunken));
            }
        }
    }
    void menuLiteralAmpersandsReserveTheirVisibleWidth() {
        for(const auto scheme:{Theme::Scheme::Dark,Theme::Scheme::Light}) {
            Theme::setScheme(scheme);const auto& c=Theme::current();
            const QString display="Capture region & edit: No output enabled · No local copy";auto encoded=display;encoded.replace("&","&&");
            QMenu menu;QStyleOptionMenuItem option;option.state=QStyle::State_None;option.font=Graphite::font();option.text=encoded;option.menuItemType=QStyleOptionMenuItem::Normal;option.menuHasCheckableItems=false;
            const auto size=menu.style()->sizeFromContents(QStyle::CT_MenuItem,&option,{},&menu);QVERIFY(size.width()>=QFontMetrics(option.font).horizontalAdvance(display)+28);option.rect=QRect(QPoint(),size);
            QImage actual(size,QImage::Format_RGB32);actual.fill(c.bg);{QPainter painter(&actual);menu.style()->drawControl(QStyle::CE_MenuItem,&option,&painter,&menu);}
            QImage expected(size,QImage::Format_RGB32);expected.fill(c.bg);{QPainter painter(&expected);painter.setFont(option.font);painter.setPen(c.textDisabled);painter.drawText(QRect(12,0,size.width()-25,size.height()),Qt::AlignVCenter|Qt::AlignLeft|Qt::TextSingleLine,display);}
            QCOMPARE(actual,expected);
        }
    }
    void menuShortcutsKeepTheirCompleteInk() {
        for(const auto scheme:{Theme::Scheme::Dark,Theme::Scheme::Light}) {
            Theme::setScheme(scheme);const auto& c=Theme::current();
            QMenu menu;QStyleOptionMenuItem option;option.state=QStyle::State_Enabled;option.rect=QRect(0,0,220,40);option.font=Graphite::font();option.text="Zoom out\tCtrl+-";option.menuItemType=QStyleOptionMenuItem::Normal;
            QImage actual(option.rect.size(),QImage::Format_RGB32);actual.fill(c.bg);{QPainter painter(&actual);menu.style()->drawControl(QStyle::CE_MenuItem,&option,&painter,&menu);}
            const auto font=Graphite::font(Graphite::Type::metadata);const int width=qCeil(QFontMetricsF(font).horizontalAdvance("Ctrl+-"));const QRect text(option.rect.right()-12-width,0,width,option.rect.height());
            QImage expected(option.rect.size(),QImage::Format_RGB32);expected.fill(c.bg);{QPainter painter(&expected);Graphite::paintText(painter,text,"Ctrl+-",c.textMuted,font,Qt::AlignRight|Qt::AlignVCenter,Qt::ElideNone);}
            QCOMPARE(actual.copy(text),expected.copy(text));
        }
    }
    void blurRadiusSharesJoinedUnitAndLinkedSlider() {
        for (const bool compact : {false, true}) {
            BlurRadius radius(2,64,12,compact);radius.resize(compact?QSize(360,50):QSize(360,90));radius.show();putgd::test::settle(10);
            auto* spin=radius.spinBox();auto* unit=radius.unitLabel();QVERIFY(radius.label()->buddy()==spin);QCOMPARE(spin->suffix(),QString());
            QCOMPARE(unit->x(),spin->geometry().right()+1);QCOMPARE(unit->y(),spin->y());QCOMPARE(unit->height(),spin->height());
            spin->setFocus(Qt::TabFocusReason);spin->setValue(63);QTest::keyClick(spin,Qt::Key_Up);QCOMPARE(spin->value(),64);QTest::keyClick(spin,Qt::Key_Up);QCOMPARE(spin->value(),64);
            if (compact) QVERIFY(!radius.slider());
            else {QCOMPARE(radius.slider()->value(),64);radius.slider()->setValue(2);QCOMPARE(spin->value(),2);}
        }
    }
    void sharedBlurWarningInvokesTheHostsRedactAction() {
        int calls=0;std::unique_ptr<QWidget> warning(Graphite::blurWarning([&]{++calls;}));warning->resize(300,150);warning->show();putgd::test::settle(10);
        auto* action=warning->findChild<QToolButton*>("useRedact");auto* text=warning->findChild<QLabel*>("blurWarningText");QVERIFY(action&&text);QCOMPARE(text->text(),Graphite::blurWarningText());
        action->setFocus(Qt::TabFocusReason);QTest::keyClick(action,Qt::Key_Space);QCOMPARE(calls,1);QVERIFY(text->geometry().bottom()<action->y());
        warning->setEnabled(false);QTest::mouseClick(action,Qt::LeftButton);QCOMPARE(calls,1);
    }
    void confirmationsSeparateQuestionFromConsequence() {
        std::unique_ptr<QMessageBox> box(Graphite::confirm(nullptr,"Discard edits","Discard your edits?\n\nYour marks will be lost.","Discard","Keep editing",true));
        box->show();putgd::test::settle(10);
        auto* question=box->findChild<QLabel*>("qt_msgbox_label");auto* consequence=box->findChild<QLabel*>("qt_msgbox_informativelabel");
        QVERIFY(question&&consequence);QCOMPARE(question->font(),Graphite::font(Graphite::Type::body,true));
        QCOMPARE(consequence->palette().color(QPalette::WindowText),Theme::current().textSecondary);
        QCOMPARE(consequence->y()-question->geometry().bottom()-1,12);
        const auto* buttons=box->findChild<QDialogButtonBox*>();QVERIFY(buttons);QCOMPARE(buttons->y()-consequence->geometry().bottom()-1,16);
        QCOMPARE(box->defaultButton(),box->button(QMessageBox::Cancel));QCOMPARE(box->escapeButton(),box->button(QMessageBox::Cancel));
    }
    void disclosuresOnlyChangeTheirChevronWhenExpanded() {
        for(const auto scheme:{Theme::Scheme::Dark,Theme::Scheme::Light}){
            Theme::setScheme(scheme);QToolButton button;button.setText("Keyboard help");Graphite::disclosure(&button);button.resize(button.sizeHint());
            auto render=[&](bool open){button.setChecked(open);QStyleOptionToolButton option;option.initFrom(&button);option.rect=button.rect();option.state=QStyle::State_Enabled|(open?QStyle::State_On:QStyle::State_Off);option.text=button.text();option.icon=button.icon();option.iconSize=button.iconSize();option.toolButtonStyle=button.toolButtonStyle();QImage shot(button.size(),QImage::Format_RGB32);shot.fill(Theme::current().surface);{QPainter p(&shot);button.style()->drawComplexControl(QStyle::CC_ToolButton,&option,&p,&button);}return shot;};
            const auto closed=render(false),open=render(true);QVERIFY(closed!=open);
            QCOMPARE(closed.copy(36,0,closed.width()-36,closed.height()),open.copy(36,0,open.width()-36,open.height()));
            QCOMPARE(open.pixelColor(open.width()/2,2),Theme::current().surface);
        }
    }
    void flushButtonsPaintTheirContentAtTheColumnEdge() {
        QPixmap glyph(12,12);glyph.fill(Qt::cyan);
        for(const auto* role:{"quiet","link"})for(const bool tool:{false,true}){
            QPushButton push("Remove mark");QToolButton button;button.setText("Keyboard help");button.setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            QAbstractButton* widget=tool?static_cast<QAbstractButton*>(&button):&push;VisualStyle::role(widget,role);widget->setIcon(QIcon(glyph));widget->setIconSize(QSize(12,12));
            const int padded=widget->sizeHint().width();widget->setProperty("graphiteFlush",true);
            // Ask the style directly: dynamic properties need no widget recreation.
            QImage shot(220,36,QImage::Format_RGB32);shot.fill(Qt::black);QPainter p(&shot);
            if(tool){QStyleOptionToolButton option;option.rect=shot.rect();option.state=QStyle::State_Enabled;option.text=button.text();option.icon=button.icon();option.iconSize=button.iconSize();option.toolButtonStyle=button.toolButtonStyle();if(QString(role)=="quiet")QCOMPARE(button.style()->sizeFromContents(QStyle::CT_ToolButton,&option,{},&button).width(),padded-14);button.style()->drawControl(QStyle::CE_ToolButtonLabel,&option,&p,&button);}
            else{QStyleOptionButton option;option.rect=shot.rect();option.state=QStyle::State_Enabled;option.text=push.text();option.icon=push.icon();option.iconSize=push.iconSize();push.style()->drawControl(QStyle::CE_PushButtonLabel,&option,&p,&push);}
            p.end();QCOMPARE(shot.pixelColor(0,18),QColor(Qt::cyan));
        }
    }
    void dangerBordersAndNeutralProgressUseSemanticColours() {
        for(const auto scheme:{Theme::Scheme::Dark,Theme::Scheme::Light}){
            Theme::setScheme(scheme);const auto& c=Theme::current();QPushButton danger("Delete from destination");VisualStyle::role(&danger,"danger");
            QStyleOptionButton option;option.rect=QRect(0,0,240,36);option.state=QStyle::State_Enabled;option.text=danger.text();QImage shot(option.rect.size(),QImage::Format_RGB32);shot.fill(c.bg);
            {QPainter p(&shot);danger.style()->drawControl(QStyle::CE_PushButton,&option,&p,&danger);}QCOMPARE(shot.pixelColor(120,0),c.danger);QVERIFY(Theme::contrast(c.danger,c.surfaceRaised)>=4.5);
            QProgressBar bar;QStyleOptionProgressBar progress;progress.rect=QRect(0,0,100,8);progress.state=QStyle::State_Enabled;progress.minimum=0;progress.maximum=100;progress.progress=50;
            shot=QImage(100,8,QImage::Format_RGB32);shot.fill(c.bg);{QPainter p(&shot);bar.style()->drawControl(QStyle::CE_ProgressBarGroove,&progress,&p,&bar);bar.style()->drawControl(QStyle::CE_ProgressBarContents,&progress,&p,&bar);}
            QCOMPARE(shot.pixelColor(20,4),c.text);QCOMPARE(shot.pixelColor(80,4),c.surfaceRaised);QCOMPARE(c.warning,c.accent);
        }
    }
    void selectedNavigationHasPersistentContrastingMarkers() {
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);const auto& c=Theme::current();
            QListWidget rows;rows.addItem("History");Graphite::NavigationDelegate delegate(nullptr);
            QStyleOptionViewItem option;option.rect=QRect(0,0,200,44);option.state=QStyle::State_Selected|QStyle::State_Enabled;
            QImage image(200,44,QImage::Format_RGB32);image.fill(c.surface);{QPainter p(&image);delegate.paint(&p,option,rows.model()->index(0,0));}
            QCOMPARE(image.pixelColor(3,22),c.accent);QVERIFY(Theme::contrast(image.pixelColor(3,22),c.selected)>=3.0);QVERIFY(Theme::contrast(c.accent,c.surface)>=3.0);
            Graphite::Segmented tabs;tabs.addTab("History");tabs.addTab("Settings");tabs.resize(tabs.sizeHint());tabs.show();tabs.clearFocus();putgd::test::settle(10);
            auto shot=tabs.grab().toImage();const auto r=tabs.tabRect(0);QCOMPARE(shot.pixelColor(r.center().x(),r.bottom()-7),c.accent);
            Graphite::SegmentTrack track;auto* button=Graphite::toolButton("List",{},"quiet",&track);track.addButton(button);button->setChecked(true);track.resize(track.sizeHint());track.show();putgd::test::settle(10);
            shot=button->grab().toImage();QCOMPARE(shot.pixelColor(button->rect().center().x(),button->rect().bottom()-3),c.accent);
        }
    }
    void formFamilyGutterStaysStableWhenLabelsHide() {
        QWidget page;auto* box=new QVBoxLayout(&page);auto* shortSection=new QWidget(&page);auto* longSection=new QWidget(&page);box->addWidget(shortSection);box->addWidget(longSection);
        auto* shortForm=new QFormLayout(shortSection);auto* longForm=new QFormLayout(longSection);auto* shortLabel=new QLabel("Folder");auto* longLabel=new QLabel("Very long credential identification");shortForm->addRow(shortLabel,new QLineEdit);longForm->addRow(longLabel,new QLineEdit);
        Graphite::alignForms(&page);page.resize(700,300);page.show();PUTGD_TRY_VERIFY(shortLabel->minimumWidth()>shortLabel->fontMetrics().horizontalAdvance(shortLabel->text()));
        const int expanded=shortLabel->minimumWidth();QVERIFY(expanded+16<=Graphite::Metric::labelGutter);QVERIFY(longLabel->wordWrap());QVERIFY(longLabel->height()>=longLabel->heightForWidth(longLabel->width()));longSection->hide();putgd::test::settle(30);QCOMPARE(shortLabel->minimumWidth(),expanded);
        longSection->show();PUTGD_TRY_COMPARE(shortLabel->minimumWidth(),expanded);longForm->setRowVisible(0,false);putgd::test::settle(30);QCOMPARE(shortLabel->minimumWidth(),expanded);
        shortLabel->setText("Upload folder");putgd::test::settle(30);QCOMPARE(shortLabel->minimumWidth(),expanded);
    }
    void iconButtonsNameThemselvesOnKeyboardFocus() {
        QWidget page;auto* row=new QHBoxLayout(&page);auto* first=Graphite::toolButton("First","images","quiet",&page);auto* icon=Graphite::toolButton("Edit an image","edit","quiet",&page);icon->setToolButtonStyle(Qt::ToolButtonIconOnly);icon->setToolTip("Edit an image");row->addWidget(first);row->addWidget(icon);page.show();page.activateWindow();first->setFocus();putgd::test::settle(10);
        QTest::keyClick(first,Qt::Key_Tab);PUTGD_TRY_VERIFY(icon->hasFocus());PUTGD_TRY_COMPARE(QToolTip::text(),QString("Edit an image"));
        QTest::keyClick(icon,Qt::Key_Backtab);PUTGD_TRY_VERIFY(first->hasFocus());PUTGD_TRY_VERIFY(!QToolTip::isVisible());
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
    void segmentUsesAnUnderlineWithoutASelectionPill() {
        Graphite::SegmentTrack track;
        auto* list=Graphite::toolButton("List"); auto* gallery=Graphite::toolButton("Gallery");
        track.addButton(list); track.addButton(gallery); list->setChecked(true);
        track.resize(track.sizeHint()); track.show(); putgd::test::settle(10);
        QVERIFY(list->height()>=Graphite::Metric::iconTarget);
        QCOMPARE(track.height(),Graphite::scaled(Graphite::Metric::control));
        const auto shot=track.grab().toImage(); const int x=list->geometry().center().x();
        QVERIFY(shot.pixelColor(x,Graphite::Metric::segmentInset)!=Theme::current().selected);
        int amber=0;for(int y=0;y<shot.height();++y)for(int px=list->geometry().left();px<=list->geometry().right();++px)if(shot.pixelColor(px,y)==Theme::current().accent)++amber;QVERIFY(amber>=24);
        QVERIFY(shot.pixelColor(x,Graphite::Metric::segmentInset-1)!=Theme::current().selected);
    }
    void recessedMediaKeepsAnEdgeInBothSchemes() {
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme); const auto& c = Theme::current();
            QImage image(100, 80, QImage::Format_RGB32); image.fill(c.well);
            { QPainter p(&image); Graphite::paintImageShadow(p, QRectF(20,20,60,40)); p.fillRect(QRectF(20,20,60,40), c.well); Graphite::paintImageEdge(p, QRectF(20,20,60,40)); }
            QVERIFY(image.pixelColor(50,20) != c.well);
            QCOMPARE(image.pixelColor(50,40), c.well);
            QCOMPARE(image.pixelColor(50,65) != c.well, scheme == Theme::Scheme::Dark);
        }
        QCOMPARE(Graphite::Metric::trackRadius - Graphite::Metric::segmentInset, Graphite::Metric::segmentRadius);
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
                QCOMPARE(badge.pixelColor(10,10),tints.live);
            }
            for (const auto& panel : {light?QColor("#ffffff"):QColor("#141414"),light?QColor("#dedee2"):QColor("#3b3b3f")}) QVERIFY(Theme::contrast(tints.live,panel)>=3.0);
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
            QVERIFY(Theme::contrast(Graphite::mix(p.bg, p.border, p.border.alphaF()), p.bg) < 2.0);
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
        // Graphite: amber, never blue, and a neutral primary action.
        QCOMPARE(Theme::tokens(Theme::Scheme::Dark)["accent"].toString(), QString("#e8b66d"));
        QCOMPARE(Theme::tokens(Theme::Scheme::Dark)["controlOutline"].toString(), QString("#7c7c78"));
        QCOMPARE(Theme::tokens(Theme::Scheme::Light)["controlOutline"].toString(), QString("#82827b"));
        QVERIFY(Theme::tokens(Theme::Scheme::Light)["bg"] != Theme::tokens(Theme::Scheme::Dark)["bg"]);
    }
    void bannerGlyphsRemainVisibleOnTheirSurface() {
        for(const auto scheme:{Theme::Scheme::Dark,Theme::Scheme::Light}) for(const auto tone:{Graphite::Banner::Tone::Info,Graphite::Banner::Tone::Warning,Graphite::Banner::Tone::Error,Graphite::Banner::Tone::Success}) {
            Theme::setScheme(scheme);Graphite::Banner banner(tone,"A readable explanation.");banner.resize(420,90);banner.show();putgd::test::settle(10);
            const auto image=banner.grab().toImage();int readable=0;
            for(int y=2;y<image.height()-2;++y)for(int x=Graphite::scaled(12);x<Graphite::scaled(12+Graphite::Metric::icon);++x)readable+=Theme::contrast(image.pixelColor(x,y),Theme::current().surface)>=3.0;
            QVERIFY(readable>0);
        }
    }
    void outlineIconsKeepSecondaryColourInsideOptionalWashes() {
        const QStringList outlines{"upload","failed-uploads","download","region","window","desktop","clipboard","history","destinations","settings","preferences","info","shortcuts","edit","stop","select","redact","blur","crop","annotate-arrow","rectangle","highlight","chevron-down","text","undo","redo","zoom-fit","zoom-actual","back","forward","up","folder-new","save","view-list","view-detail","play","pause","trim-in","trim-out","trim-reset","mask-in","mask-out","remove","bucket","proton","add","file","video"};
        for (const auto& name : outlines) {
            const auto art = IconArt::symbol(name, Qt::white, 4, Qt::red);
            QVERIFY2(art == IconArt::symbol(name, Qt::white, 4, Qt::blue), qPrintable(name));
            int ink = 0;
            for (int y = 0; y < art.height(); ++y) for (int x = 0; x < art.width(); ++x) {
                const auto alpha = art.pixelColor(x, y).alpha();
                ink += alpha > 0;
                if (x < 4 || y < 4 || x >= art.width()-4 || y >= art.height()-4) QVERIFY2(alpha == 0, qPrintable(name));
            }
            QVERIFY2(ink > 100, qPrintable(name));
        }
        for (const auto& name : {"record", "alert", "image"}) {
            const auto art = IconArt::symbol(name, Qt::white, 4, Qt::red);
            QVERIFY(art != IconArt::symbol(name, Qt::white, 4, Qt::blue));
            // Sample empty interior, away from the antialiased outline.
            const auto interior = QString(name)=="record" ? QPoint(12,12) : QString(name)=="alert" ? QPoint(7,17) : QPoint(9,9);
            const auto wash = art.pixelColor(interior*4);
            QVERIFY2(wash.alpha() >= 45 && wash.alpha() <= 47, name);
            QCOMPARE(wash.red(), 255);QCOMPARE(wash.blue(), 0);
        }
        for (const auto& name : {"window", "desktop", "clipboard", "info", "shortcuts", "stop", "save", "play", "pause", "bucket", "file"}) {
            const auto art = IconArt::symbol(name, Qt::white, 4);
            int solid = 0;
            for (int y = 0; y < art.height(); ++y) for (int x = 0; x < art.width(); ++x) solid += art.pixelColor(x,y).alpha() > 128;
            QVERIFY2(solid < art.width()*art.height()/2, name);
        }
        const auto gear=IconArt::symbol("settings",Qt::white,4);
        QVERIFY(gear.pixelColor(48,10).alpha()>200);
        QCOMPARE(gear.pixelColor(48,48).alpha(),0);
        QCOMPARE(IconArt::symbol("brand", Qt::white), IconArt::liftApp(Qt::white));
        QCOMPARE(IconArt::symbol("tray", Qt::white), IconArt::liftTray(Qt::white));
    }
    void outlinesUseOpticalSizingForTheirShape() {
        const auto extent=[](const QString& name) {
            const auto art=IconArt::symbol(name,Qt::white,4);QRect ink;
            for(int y=0;y<art.height();++y)for(int x=0;x<art.width();++x)if(art.pixelColor(x,y).alpha()>127)ink=ink.united(QRect(x,y,1,1));
            return std::max(ink.width(),ink.height())/4.0;
        };
        for(const auto& name:{"image","stop","rectangle","zoom-fit"})QVERIFY2(std::abs(extent(name)-18)<=0.5,name);
        for(const auto& name:{"window","desktop","shortcuts","save"})QVERIFY2(std::abs(extent(name)-20)<=0.5,name);
        QVERIFY(std::abs(extent("add")-21)<=0.5);
    }
    void providerOutlinesStayDistinctAtSidebarSize() {
        for(const auto scheme:{Theme::Scheme::Dark,Theme::Scheme::Light}) {
            Theme::setScheme(scheme);
            for(const int size:{16,24}) {
                const auto cloud=VisualStyle::icon("proton").pixmap(size,size).toImage();
                QVERIFY(cloud!=VisualStyle::icon("destinations").pixmap(size,size).toImage());
                QVERIFY(cloud!=VisualStyle::icon("bucket").pixmap(size,size).toImage());
                QCOMPARE(cloud.pixelColor(size/2,size/2).alpha(),0);
                int ink=0;for(int y=0;y<size;++y)for(int x=0;x<size;++x)ink+=cloud.pixelColor(x,y).alpha()>128;
                QVERIFY(ink>size*2);QVERIFY(ink<size*size/2);
            }
        }
    }
    void helpGlyphStaysMutedAcrossSchemesAndKeepsKeyboardDisclosure() {
        const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) for (const int pixels : {14,21}) {
            auto font=original;font.setPixelSize(pixels);qApp->setFont(font);Theme::setScheme(scheme);
            QWidget page;auto* layout=new QVBoxLayout(&page);page.setProperty("inlineHelpRow",true);
            auto* control=new QLineEdit(&page);control->setObjectName("testHelp");layout->addWidget(control);
            auto* help=VisualStyle::helpButton("Term","An explanation.",control,&page);layout->addWidget(help);page.show();putgd::test::settle(10);
            QCOMPARE(help->size(),QSize(Graphite::scaled(24),Graphite::scaled(24)));
            QCOMPARE(help->iconSize(),QSize(Graphite::scaled(14),Graphite::scaled(14)));
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
            const auto icon = VisualStyle::icon("settings");
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
    // A locked value (Graphite::setLocked) reads as information: its text is
    // textMuted at 4.5:1 on the page. Unavailable controls keep readable
    // textDisabled and omit interaction. Unlocking restores their shared look.
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
            QVERIFY2(best(unavailable) >= 4.5, qPrintable(Theme::name(scheme) + QString(" unavailable %1").arg(best(unavailable))));
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
        // Status hues come from the same semantic palette as the windows.
        for(const auto scheme:{Theme::Scheme::Dark,Theme::Scheme::Light}){
            const auto& p=Theme::palette(scheme);const auto& t=Theme::trayTints(scheme==Theme::Scheme::Light);
            QVERIFY(std::abs(t.upload.hsvHue()-p.success.hsvHue())<3);QVERIFY(std::abs(t.encoding.hsvHue()-p.accent.hsvHue())<3);QVERIFY(std::abs(t.error.hsvHue()-p.danger.hsvHue())<3);
        }
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
    // A primary button is the neutral action fill with background-colour ink;
    // its label, alone or as one group with its icon, is centred.
    void compactControlsMeasureTheirPaintedTypeAndScale() {
        const auto original = QApplication::font();
        const auto restore = qScopeGuard([&] { QApplication::setFont(original); });
        for (const qreal scale : {1.0, 1.5}) {
            auto enlarged = original; enlarged.setPixelSize(qRound(QFontInfo(original).pixelSize() * scale)); QApplication::setFont(enlarged);
            QPushButton push("Start here");
            QToolButton tool; tool.setText("End here"); tool.setToolButtonStyle(Qt::ToolButtonTextOnly);
            QComboBox combo; combo.addItem("Compatible MP4");
            for (auto* widget : {static_cast<QWidget*>(&push), static_cast<QWidget*>(&tool), static_cast<QWidget*>(&combo)}) {
                widget->ensurePolished();
                const int standard = widget->sizeHint().height();
                widget->setProperty("graphiteDensity", "compact");
                QCOMPARE(widget->font().pixelSize(), Graphite::scaled(13));
                QCOMPARE(widget->font().weight(), QFont::Normal);
                const auto hint = widget->sizeHint();
                QCOMPARE(hint.height(), Graphite::scaled(Graphite::Metric::controlCompact));
                QVERIFY(hint.height() <= standard);
                QVERIFY(hint.width() >= widget->fontMetrics().horizontalAdvance(qobject_cast<QComboBox*>(widget) ? combo.currentText() : qobject_cast<QAbstractButton*>(widget)->text()));
                widget->setProperty("graphiteDensity", {});
                QCOMPARE(widget->font().pixelSize(), Graphite::scaled(Graphite::Type::body));
                QCOMPARE(widget->sizeHint().height(), standard);
            }
            combo.setFrame(false);
            combo.setProperty("graphiteDensity", "compact");
            QCOMPARE(combo.sizeHint().height(), Graphite::scaled(Graphite::Metric::controlCompact));
        }
    }
    void actionWeightsFollowTheirPurpose() {
        for (const auto* role : {"quiet", "secondary", "danger", "primary", "lead"}) {
            QPushButton button("Apply changes"); VisualStyle::role(&button, role);
            QCOMPARE(button.font().weight(), QByteArray(role) == "primary" || QByteArray(role) == "lead" ? QFont::Medium : QFont::Normal);
        }
        QCheckBox check("Launch at login"); check.ensurePolished();
        QRadioButton radio("Save locally"); radio.ensurePolished();
        QToolButton disclosure; Graphite::disclosure(&disclosure); disclosure.ensurePolished();
        QCOMPARE(check.font().weight(), QFont::Normal);
        QCOMPARE(radio.font().weight(), QFont::Normal);
        QCOMPARE(disclosure.font().weight(), QFont::Normal);
        disclosure.setChecked(true);
        QCOMPARE(disclosure.font().weight(), QFont::Normal);
    }
    void leadButtonKeepsTheContentActionProminent() {
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme); const auto& c = Theme::current();
            QPushButton lead("Capture region"); VisualStyle::role(&lead, "lead");
            QStyleOptionButton option; option.rect=QRect(0,0,200,40); option.state=QStyle::State_Enabled; option.text=lead.text();
            QImage image(option.rect.size(),QImage::Format_RGB32); image.fill(c.bg);
            { QPainter p(&image); lead.style()->drawControl(QStyle::CE_PushButton,&option,&p,&lead); }
            QCOMPARE(image.pixelColor(100,3),c.lead);
            QCOMPARE(image.pixelColor(100,0),c.leadOutline);
            QCOMPARE(lead.font().weight(),QFont::Medium);
            QVERIFY(Theme::contrast(c.text,c.lead)>=4.5);
            QVERIFY(c.lead!=c.text);
        }
    }
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
                QCOMPARE(shot.pixelColor(shot.width() / 2, 3), p.primary);
                QVERIFY(Theme::contrast(p.bg, p.primary) >= 4.5);
                QVERIFY(Theme::contrast(p.bg, p.primaryHover) >= 4.5);
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
    // Technical values use the 13 px Plex Mono role.
    void technicalValuesUseTheirMonoRole() {
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
            QCOMPARE(w->font().pixelSize(), Graphite::scaled(Graphite::Type::technical));
        }
        QVERIFY(name->font().family() != Graphite::monoFamily());
        // A technical label states a value; it is not painted as a link.
        QVERIFY(value->palette().color(QPalette::WindowText) != Theme::current().link);
    }
    void revealableSummaryHasPersistentDisclosureAndHover() {
        Graphite::ElidedLabel label;label.resize(220,36);label.setRevealable(true);label.show();label.clearFocus();putgd::test::settle(10);
        for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
            Theme::setScheme(scheme);label.setAttribute(Qt::WA_UnderMouse,false);const auto idle=label.grab().toImage();
            label.setRevealable(false);const auto plain=label.grab().toImage();QVERIFY(idle!=plain);label.setRevealable(true);
            label.setAttribute(Qt::WA_UnderMouse,true);const auto hover=label.grab().toImage();QVERIFY(hover!=idle);
            label.setAttribute(Qt::WA_UnderMouse,false);
        }
    }
    void summaryDisclosureFollowsShortTextInAStretchingFooter() {
        Graphite::ElidedLabel label;
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
            QVERIFY(right <= end + 6 + Graphite::scaled(14));
        }
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
        auto* feedback = new Graphite::BrowserFeedback(body);
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
    void familiarIndicatorsScaleWithText() {
        QCheckBox check("Synthetic");QRadioButton radio("Synthetic");
        QCOMPARE(check.style()->pixelMetric(QStyle::PM_IndicatorWidth),Graphite::scaled(17));
        QCOMPARE(radio.style()->pixelMetric(QStyle::PM_ExclusiveIndicatorWidth),Graphite::scaled(18));
        QCOMPARE(check.style()->pixelMetric(QStyle::PM_CheckBoxLabelSpacing),Graphite::scaled(9));
        check.ensurePolished();radio.ensurePolished();
        QVERIFY(check.sizeHint().height()>=Graphite::scaled(17));QVERIFY(radio.sizeHint().height()>=Graphite::scaled(18));
    }
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
    // Shared minus/plus targets retain native editing and keyboard stepping.
    void spinBoxStepsUseAdjacentTargets() {
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
