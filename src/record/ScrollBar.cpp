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
    // What the mouse does to the capture, in words, for as long as it runs (#1514).
    hint_ = new QLabel(this);
    hint_->setObjectName("hint");
    Ui::role(hint_, "secondary");
    resume_ = new QPushButton("Resume", this);
    resume_->setObjectName("resume");
    resume_->setIcon(Ui::icon("play"));
    resume_->setToolTip("Go on capturing from here");
    Ui::role(resume_, "primary");
    stop_ = new QPushButton("Stop", this);
    stop_->setObjectName("stop");
    stop_->setIcon(Ui::icon("stop"));
    stop_->setToolTip("Stop and keep the image so far");
    Ui::role(stop_, "primary");
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
    row->addWidget(resume_);
    row->addWidget(stop_);
    row->addWidget(cancel_);
    connect(resume_, &QPushButton::clicked, this, &ScrollBar::resumeRequested);
    connect(stop_, &QPushButton::clicked, this, &ScrollBar::stopRequested);
    connect(cancel_, &QPushButton::clicked, this, &ScrollBar::cancelRequested);
    // One size for every state, so the bar never moves once it is placed and
    // Stop stays where the user is reaching for it: sized for the longest
    // texts, with Resume shown (it is hidden outside a pause, and the stretch
    // keeps Stop and Cancel where they are).
    const QFontMetrics text1(label_->font()), text2(hint_->font()), mono(height_->font());
    int widest = 0, widestHint = 0;
    for (const char* each : {"Ready", "Scrolling to the top…", "Capturing", "Paused: you moved the mouse", "Paused", "Resuming…", "Saving…"}) widest = std::max(widest, text1.horizontalAdvance(QString::fromUtf8(each)));
    for (const char* each : {"Move the mouse to pause", "Stop keeps the image so far", "Let go of the mouse"}) widestHint = std::max(widestHint, text2.horizontalAdvance(QString::fromUtf8(each)));
    label_->setMinimumWidth(widest + 4);
    hint_->setMinimumWidth(widestHint + 4);
    height_->setMinimumWidth(mono.horizontalAdvance(heightText(88888)) + 4);
    hint_->setText(QStringLiteral("Move the mouse to pause"));
    resume_->show();
    adjustSize();
    setFixedSize(sizeHint());
    refresh();
}
QString ScrollBar::heightText(int rows) { return QLocale(QLocale::English, QLocale::UnitedStates).toString(std::max(0, rows)) + QStringLiteral(" px"); }
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
void ScrollBar::refresh() {
    const auto& tokens = Theme::current();
    const bool capturing = state_ == "capturing", toTop = state_ == "scrollingToTop", finishing = state_ == "finishing";
    const bool paused = state_ == "paused", resuming = state_ == "resuming";
    QString label = QStringLiteral("Ready"), hint;
    if (capturing) label = QStringLiteral("Capturing");
    else if (toTop) label = QStringLiteral("Scrolling to the top…");
    else if (paused) label = pausedBy_ == "pointer" ? QStringLiteral("Paused: you moved the mouse") : QStringLiteral("Paused");
    else if (resuming) label = QStringLiteral("Resuming…");
    else if (finishing) label = QStringLiteral("Saving…");
    if (capturing || toTop) hint = QStringLiteral("Move the mouse to pause");
    else if (paused) hint = QStringLiteral("Stop keeps the image so far");
    else if (resuming) hint = QStringLiteral("Let go of the mouse");
    label_->setText(label);
    hint_->setText(hint);
    height_->setText(capturing || finishing || paused || resuming ? heightText(rows_) : QString());
    dot_->setStyleSheet(QStringLiteral("background: %1; border-radius: %2px;").arg((capturing || toTop ? tokens.live : paused || resuming ? tokens.accent : tokens.textSecondary).name()).arg(dot_->width() / 2.0));
    // Stop keeps an image, and every state from the first frame on has one:
    // on the way to the top it is the viewport as it is then.
    stop_->setEnabled(capturing || toTop || paused || resuming);
    resume_->setVisible(paused || resuming);
    resume_->setEnabled(paused);
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
}
