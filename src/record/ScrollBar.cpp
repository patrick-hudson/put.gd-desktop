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
    stop_ = new QPushButton("Stop", this);
    stop_->setObjectName("stop");
    stop_->setIcon(Ui::icon("stop"));
    stop_->setToolTip("Stop and keep the image so far");
    Ui::role(stop_, "primary");
    cancel_ = new QPushButton("Cancel", this);
    cancel_->setObjectName("cancel");
    cancel_->setToolTip("Discard this capture");
    Ui::role(cancel_, "quiet");
    for (auto* w : std::initializer_list<QWidget*>{dot_, label_, height_}) row->addWidget(w);
    row->addStretch(1);
    row->addWidget(stop_);
    row->addWidget(cancel_);
    connect(stop_, &QPushButton::clicked, this, &ScrollBar::stopRequested);
    connect(cancel_, &QPushButton::clicked, this, &ScrollBar::cancelRequested);
    // One size for every state, so the bar never moves once it is placed.
    const QFontMetrics text(label_->font()), mono(height_->font());
    int widest = 0;
    for (const char* each : {"Ready", "Scrolling to the top…", "Capturing", "Saving…"}) widest = std::max(widest, text.horizontalAdvance(QString::fromUtf8(each)));
    label_->setMinimumWidth(widest + 4);
    height_->setMinimumWidth(mono.horizontalAdvance(heightText(88888)) + 4);
    refresh();
    adjustSize();
    setFixedSize(sizeHint());
}
QString ScrollBar::heightText(int rows) { return QLocale(QLocale::English, QLocale::UnitedStates).toString(std::max(0, rows)) + QStringLiteral(" px"); }
void ScrollBar::setState(const QString& state) {
    if (state_ == state) return;
    state_ = state;
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
    label_->setText(capturing ? QStringLiteral("Capturing") : toTop ? QStringLiteral("Scrolling to the top…") : finishing ? QStringLiteral("Saving…") : QStringLiteral("Ready"));
    height_->setText(capturing || finishing ? heightText(rows_) : QString());
    dot_->setStyleSheet(QStringLiteral("background: %1; border-radius: %2px;").arg((capturing || toTop ? tokens.live : tokens.textSecondary).name()).arg(dot_->width() / 2.0));
    // Stop keeps an image, so it needs one: nothing is stitched before capturing.
    stop_->setEnabled(capturing);
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
    const BarSpot spot = placeBar(size(), area, screens);
    outside_ = spot.outside && spot.screen >= 0;
    if (!outside_) return false;
    const auto native = QGuiApplication::screens();
    if (mode == SurfaceMode::LayerShell && spot.screen < native.size()) barToLayer(this, native[spot.screen], spot.rect, false);
    else setGeometry(spot.rect);
    return true;
}
}
