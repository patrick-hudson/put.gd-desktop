#include "ScrollBar.h"
#include "BarPlacement.h"
#include "Overlays.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"
#include "ui/Ui.h"
#include <QApplication>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPushButton>
#include <QScreen>

namespace putgd::record {
namespace {
const QStringList allStates{"ready", "waiting", "following", "lost", "scrollingToTop", "autoScrolling", "paused", "resuming", "finishing"};
const QStringList allPauses{"pointer", "scrolled", "no_movement"};
}
ScrollBar::ScrollBar(QWidget* parent) : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::X11BypassWindowManagerHint | Qt::WindowDoesNotAcceptFocus) {
    setObjectName("putgd-scroll-bar");
    setWindowTitle(QString::fromLatin1(controlBarTitle));
    setAttribute(Qt::WA_ShowWithoutActivating);
    Fonts::load();
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) Ui::apply(*app);
    connect(&Theme::Notifier::instance(), &Theme::Notifier::changed, this, [this] { refresh(); update(); });
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(Ui::space(Ui::Space::m), Ui::space(Ui::Space::s), Ui::space(Ui::Space::m), Ui::space(Ui::Space::s));
    row->setSpacing(Ui::space(Ui::Space::s));
    dot_ = new QLabel(this);
    dot_->setFixedSize(Ui::scaled(12), Ui::scaled(12));
    // The state is written, not only coloured.
    label_ = new QLabel(this);
    label_->setObjectName("state");
    Ui::role(label_, "strong");
    height_ = new QLabel(this);
    height_->setObjectName("height");
    height_->setAccessibleName("Height so far");
    Ui::setMono(height_, Ui::Type::body);
    // What to do next, in words, for as long as the capture runs.
    hint_ = new QLabel(this);
    hint_->setObjectName("hint");
    Ui::role(hint_, "secondary");
    autoScroll_ = new QPushButton("Auto-scroll", this);
    autoScroll_->setObjectName("autoScroll");
    autoScroll_->setCheckable(true);
    autoScroll_->setIcon(Ui::icon("scroll"));
    autoScroll_->setToolTip("put.gd turns the page for you; moving the mouse pauses it");
    Ui::role(autoScroll_, "secondary");
    done_ = new QPushButton("Done", this);
    done_->setObjectName("done");
    done_->setToolTip("Finish and keep the image");
    Ui::role(done_, "primary");
    cancel_ = new QPushButton("Cancel", this);
    cancel_->setObjectName("cancel");
    cancel_->setToolTip("Discard this capture");
    Ui::role(cancel_, "quiet");
    auto* text = new QVBoxLayout;
    text->setContentsMargins(0, 0, 0, 0);
    text->setSpacing(0);
    auto* first = new QHBoxLayout;
    first->setContentsMargins(0, 0, 0, 0);
    first->setSpacing(Ui::space(Ui::Space::s));
    first->addWidget(label_);
    first->addWidget(height_);
    first->addStretch(1);
    text->addLayout(first);
    text->addWidget(hint_);
    row->addWidget(dot_);
    row->addLayout(text);
    row->addStretch(1);
    row->addWidget(autoScroll_);
    row->addWidget(done_);
    row->addWidget(cancel_);
    // The toggle is the session's to set: a click asks, the session answers.
    connect(autoScroll_, &QPushButton::clicked, this, [this](bool on) { autoScroll_->setChecked(auto_); emit autoScrollRequested(on); });
    connect(done_, &QPushButton::clicked, this, &ScrollBar::doneRequested);
    connect(cancel_, &QPushButton::clicked, this, &ScrollBar::cancelRequested);
    fitSize();
    refresh();
}
QString ScrollBar::heightText(int rows) { return QLocale(QLocale::English, QLocale::UnitedStates).toString(std::max(0, rows)) + QStringLiteral(" px"); }
QStringList ScrollBar::words(const QString& state, const QString& pausedBy, const QString& finishKey) {
    const QString finish = finishKey.isEmpty() ? QStringLiteral("Done when you reach the end") : QStringLiteral("Done or %1 when you reach the end").arg(finishKey);
    if (state == "waiting") return {QStringLiteral("Scroll the page"), finish};
    if (state == "following") return {QStringLiteral("Following"), finish};
    if (state == "lost") return {QStringLiteral("Lost the page"), QStringLiteral("Scroll back a little")};
    if (state == "scrollingToTop") return {QStringLiteral("Scrolling to the top…"), QStringLiteral("Move the mouse to pause")};
    if (state == "autoScrolling") return {QStringLiteral("Auto-scrolling"), QStringLiteral("Move the mouse to pause")};
    if (state == "paused") {
        if (pausedBy == "no_movement") return {QStringLiteral("Nothing scrolled here"), QStringLiteral("Scroll the page yourself")};
        if (pausedBy == "scrolled") return {QStringLiteral("Paused: you scrolled"), QStringLiteral("Scroll on, or Auto-scroll to go on")};
        return {QStringLiteral("Paused: you moved the mouse"), QStringLiteral("Scroll on, or Auto-scroll to go on")};
    }
    if (state == "resuming") return {QStringLiteral("Auto-scroll in a moment…"), QStringLiteral("Let go of the mouse")};
    if (state == "finishing") return {QStringLiteral("Saving…"), QString()};
    return {QStringLiteral("Ready"), QString()};
}
// One size for every state, so the bar never moves once it is placed and its
// buttons stay where the user is reaching for them: sized for the longest texts.
void ScrollBar::fitSize() {
    const QFontMetrics text1(label_->font()), text2(hint_->font()), mono(height_->font());
    int widest = 0, widestHint = 0;
    for (const QString& state : allStates)
        for (const QString& by : allPauses) {
            const auto w = words(state, by, finishKey_);
            widest = std::max(widest, text1.horizontalAdvance(w[0]));
            widestHint = std::max(widestHint, text2.horizontalAdvance(w[1]));
        }
    label_->setMinimumWidth(widest + 4);
    hint_->setMinimumWidth(widestHint + 4);
    height_->setMinimumWidth(mono.horizontalAdvance(heightText(88888)) + 4);
    setMinimumSize(0, 0);
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    hint_->setText(words("following", {}, finishKey_)[1]);
    adjustSize();
    setFixedSize(sizeHint());
}
void ScrollBar::setFinishKey(const QString& key) {
    if (finishKey_ == key) return;
    finishKey_ = key;
    fitSize();
    refresh();
}
void ScrollBar::setState(const QString& state) {
    if (state_ == state) return;
    state_ = state;
    if (state != "paused" && state != "resuming") pausedBy_.clear();
    refresh();
}
void ScrollBar::setPaused(const QString& by) {
    if (state_ == "paused" && pausedBy_ == by) return;
    state_ = QStringLiteral("paused");
    pausedBy_ = by;
    refresh();
}
void ScrollBar::setHeight(int rows) {
    if (rows_ == rows) return;
    rows_ = rows;
    refresh();
}
void ScrollBar::setAutoScroll(bool on) {
    if (auto_ == on) return;
    auto_ = on;
    refresh();
}
void ScrollBar::setPicture(bool picture) {
    if (picture_ == picture) return;
    picture_ = picture;
    refresh();
}
void ScrollBar::refresh() {
    const auto& tokens = Theme::current();
    const auto w = words(state_, pausedBy_, finishKey_);
    const bool finishing = state_ == "finishing", ready = state_ == "ready";
    const bool driving = state_ == "autoScrolling" || state_ == "scrollingToTop";
    label_->setText(w[0]);
    hint_->setText(w[1]);
    height_->setText(ready || state_ == "scrollingToTop" ? QString() : heightText(rows_));
    const QColor dot = driving ? tokens.live : state_ == "lost" ? tokens.warning : ready || finishing ? tokens.textSecondary : tokens.accent;
    dot_->setStyleSheet(QStringLiteral("background: %1; border-radius: %2px;").arg(dot.name()).arg(dot_->width() / 2.0));
    // On, it shows the pause glyph: a checked secondary button differs too little in the dark theme.
    const bool on = auto_ || state_ == "resuming";
    autoScroll_->setChecked(on);
    autoScroll_->setIcon(Ui::icon(on ? "pause" : "scroll"));
    autoScroll_->setToolTip(on ? QStringLiteral("Hand the page back to you") : QStringLiteral("put.gd turns the page for you; moving the mouse pauses it"));
    autoScroll_->setEnabled(!finishing && !ready && state_ != "resuming");
    done_->setEnabled(picture_ && !finishing && !ready);
    cancel_->setEnabled(!finishing);
}
void ScrollBar::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), Theme::current().bg);
}
bool ScrollBar::place(const QRect& captured) {
    const auto mode = surfaceMode();
    // The Shell places it (the resident asks the extension), or nobody can.
    if (mode == SurfaceMode::Compositor) { outside_ = false; return false; }
    const QRect area = mode == SurfaceMode::X11 ? barLogicalRect(captured) : captured;
    const QList<QRect> screens = barScreens();
    // On X11 the window manager draws a soft shadow around the bar, and at
    // 10 px it reached the rectangle: the last five rows of Cinnamon captures
    // were up to 3 levels darker under the bar's columns (acceptance of
    // 2026-11-12). Further away, the shadow stays outside the rectangle.
    // Beside the rectangle on its own screen first, on every side, so that the
    // pointer's way from its rest on the bar to the page stays short (#1514).
    const BarSpot spot = placeBar(size(), area, screens, true, mode == SurfaceMode::X11 ? 32 : 10, true);
    outside_ = spot.outside && spot.screen >= 0;
    if (!outside_) return false;
    const auto native = QGuiApplication::screens();
    if (mode == SurfaceMode::LayerShell && spot.screen < native.size()) barToLayer(this, native[spot.screen], spot.rect, false);
    else setGeometry(spot.rect);
    return true;
}
// No room outside (#1519): the bottom-right corner inside, 10 px from the
// edges, as the GNOME Shell places it. The session keeps its rows out of the
// image.
bool ScrollBar::placeInside(const QRect& captured) {
    const auto mode = surfaceMode();
    outside_ = false;
    if (mode == SurfaceMode::Compositor) return false;
    const QRect area = mode == SurfaceMode::X11 ? barLogicalRect(captured) : captured;
    const int gap = 10;
    if (area.width() < width() + 2 * gap || area.height() < height() + 2 * gap) return false;
    const QRect spot(area.right() + 1 - gap - width(), area.bottom() + 1 - gap - height(), width(), height());
    QScreen* on = nullptr;
    for (QScreen* screen : QGuiApplication::screens()) if (screen->geometry().contains(spot.center())) on = screen;
    if (mode == SurfaceMode::LayerShell && on) barToLayer(this, on, spot, false);
    else setGeometry(spot);
    return true;
}
}
