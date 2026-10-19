#include "Overlays.h"
#include "BarPlacement.h"
#include "ui/SelectionOutline.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QWindow>
#include <algorithm>
#include <utility>
#ifdef PUTGD_RECORD_LAYER_SHELL
#include <LayerShellQt/Window>
#endif

namespace putgd::record {
SurfaceMode surfaceMode() {
    static const SurfaceMode mode = [] {
        if (!QGuiApplication::platformName().startsWith("wayland")) return SurfaceMode::X11;
#ifdef PUTGD_RECORD_LAYER_SHELL
        // KWin implements wlr-layer-shell; the same selector serves KDE screenshots.
        if (qEnvironmentVariable("XDG_CURRENT_DESKTOP").contains("KDE", Qt::CaseInsensitive)) return SurfaceMode::LayerShell;
#endif
        return SurfaceMode::Compositor;
    }();
    return mode;
}
namespace {
#ifdef PUTGD_RECORD_LAYER_SHELL
// An overlay layer surface at a logical desktop position on `screen`; panels'
// exclusive zones are ignored so the margins are relative to the output.
void toLayer(QWidget* widget, QScreen* screen, const QRect& logical, bool keyboard) {
    widget->setScreen(screen);
    widget->setGeometry(logical);
    widget->winId();
    auto* layer = LayerShellQt::Window::get(widget->windowHandle());
    layer->setScope(QStringLiteral("putgd-record"));
    layer->setLayer(LayerShellQt::Window::LayerOverlay);
    layer->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop) | LayerShellQt::Window::AnchorLeft);
    layer->setExclusiveZone(-1);
    layer->setKeyboardInteractivity(keyboard ? LayerShellQt::Window::KeyboardInteractivityOnDemand : LayerShellQt::Window::KeyboardInteractivityNone);
    layer->setMargins(QMargins(logical.x() - screen->geometry().x(), logical.y() - screen->geometry().y(), 0, 0));
}
#endif
constexpr Qt::WindowFlags passiveFlags = Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::X11BypassWindowManagerHint | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput;
// X11 root pixels from Qt's logical screen coordinates.
QRect toRoot(const QRectF& logical) {
    QScreen* screen = QGuiApplication::screenAt(logical.center().toPoint());
    if (!screen) screen = QGuiApplication::primaryScreen();
    const qreal dpr = screen ? screen->devicePixelRatio() : 1.0;
    const QRectF origin = screen ? screen->geometry() : QRectF();
    const QPointF native = screen ? QPointF(origin.topLeft()) * dpr : QPointF();
    const QRectF shifted = logical.translated(-origin.topLeft());
    return QRectF(native + shifted.topLeft() * dpr, shifted.size() * dpr).toAlignedRect();
}
QRectF fromRoot(const QRect& root) {
    for (QScreen* screen : QGuiApplication::screens()) {
        const qreal dpr = screen->devicePixelRatio();
        const QRectF native(QPointF(screen->geometry().topLeft()) * dpr, QSizeF(screen->geometry().size()) * dpr);
        if (native.contains(QPointF(root.center()))) {
            const QPointF local = (QPointF(root.topLeft()) - native.topLeft()) / dpr;
            return QRectF(QPointF(screen->geometry().topLeft()) + local, QSizeF(root.size()) / dpr);
        }
    }
    return root;
}
// Qt logical rectangle of root pixels that may span screens with different
// scales: each screen's piece is mapped on its own.
QRect fromRootSpan(const QRect& root) {
    QRectF span;
    for (QScreen* screen : QGuiApplication::screens()) {
        const qreal dpr = screen->devicePixelRatio();
        const QRectF native(QPointF(screen->geometry().topLeft()) * dpr, QSizeF(screen->geometry().size()) * dpr);
        const QRectF piece = QRectF(root).intersected(native);
        if (piece.isEmpty()) continue;
        span |= QRectF(QPointF(screen->geometry().topLeft()) + (piece.topLeft() - native.topLeft()) / dpr, piece.size() / dpr);
    }
    return span.isEmpty() ? fromRoot(root).toAlignedRect() : span.toAlignedRect();
}
QList<QRect> screenRects() {
    QList<QRect> rects;
    for (QScreen* screen : QGuiApplication::screens()) rects.append(screen->geometry());
    return rects;
}
const char* const barStyle = R"QSS(
QWidget#putgd-record-bar { background: #15171c; border: 1px solid #3a4150; border-radius: 10px; }
QLabel { color: #e6e8ee; font-size: 13px; }
QLabel#time { font-family: "DejaVu Sans Mono", monospace; font-size: 14px; min-width: 64px; }
QCheckBox { color: #e6e8ee; spacing: 6px; }
QCheckBox:disabled { color: #6f7685; }
QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid #9bbcff; border-radius: 3px; background: #22262f; }
QCheckBox::indicator:checked { background: #9bbcff; }
QCheckBox::indicator:disabled { border-color: #3a4150; background: #1c1f26; }
QComboBox { background: #22262f; color: #e6e8ee; border: 1px solid #3a4150; border-radius: 6px; padding: 2px 8px; }
QComboBox:disabled { color: #6f7685; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox QAbstractItemView { background: #22262f; color: #e6e8ee; border: 1px solid #3a4150; selection-background-color: #3a4150; selection-color: #ffffff; outline: none; }
QPushButton { background: #2a2f3a; color: #e6e8ee; border: 1px solid #3a4150; border-radius: 6px; padding: 4px 12px; }
QPushButton:hover { background: #343a47; }
QPushButton#record { background: #2e7d32; color: white; border-color: #43a047; }
QPushButton#record:hover { background: #388e3c; }
QPushButton#stop { background: #c62828; color: white; border-color: #e53935; }
QPushButton#stop:hover { background: #d32f2f; }
QPushButton:disabled { color: #6f7685; background: #1c1f26; }
)QSS";
}

QString formatHint(const QString& format, const QString& codec) {
    if (format == "mp4" && codec == "h265") return QStringLiteral("Smaller files, but Firefox and Chrome on Linux cannot play H.265 links; Safari and Edge can. Choose H.264 for a link anyone can open.");
    if (format == "mp4") return QStringLiteral("Plays everywhere.");
    if (format == "webm") return QStringLiteral("VP9 with Opus audio; plays in browsers except older Safari.");
    if (format == "gif") return QStringLiteral("Silent animation, at most 60 s and 1280 px.");
    return QStringLiteral("Silent animated WebP, at most 60 s and 1280 px.");
}
QString formatLabel(const QString& format, const QString& codec) {
    if (format == "mp4") return codec == "h265" ? QStringLiteral("MP4 (H.265)") : QStringLiteral("MP4 (H.264)");
    if (format == "webm") return QStringLiteral("WebM (VP9)");
    if (format == "gif") return QStringLiteral("GIF (silent)");
    return QStringLiteral("WebP (silent)");
}

class SelectionOverlay::Surface final : public QWidget {
public:
    Surface(SelectionOverlay& owner, QScreen* screen) : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::X11BypassWindowManagerHint | Qt::WindowDoesNotAcceptFocus), owner_(owner), logical_(screen->geometry()) {
        setObjectName("putgd-record-select");
        setAttribute(Qt::WA_TranslucentBackground);
        setScreen(screen);
        setGeometry(logical_.toAlignedRect());
        setCursor(Qt::CrossCursor);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
    }
    void refresh() { update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), QColor(0, 0, 0, 70));
        const auto clipped = owner_.selection_.intersected(logical_);
        if (!clipped.isEmpty()) {
            const QRectF local = clipped.translated(-logical_.topLeft());
            painter.fillRect(local, Qt::transparent);
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
            paintSelectionOutline(painter, local);
            painter.setPen(Qt::white);
            painter.drawText(local.adjusted(6, 6, -6, -6), Qt::AlignTop | Qt::AlignLeft, QStringLiteral("%1 × %2").arg(int(clipped.width())).arg(int(clipped.height())));
        } else {
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
            painter.setPen(Qt::white);
            painter.drawText(rect().adjusted(24, 24, -24, -24), Qt::AlignTop | Qt::AlignLeft, QStringLiteral("Drag to choose the area to record. Esc cancels."));
        }
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::RightButton) { owner_.cancel(); return; }
        if (e->button() == Qt::LeftButton) { owner_.press(e->globalPosition()); grabMouse(); }
    }
    void mouseMoveEvent(QMouseEvent* e) override { owner_.move(e->globalPosition()); }
    void mouseReleaseEvent(QMouseEvent* e) override { if (e->button() == Qt::LeftButton) { releaseMouse(); owner_.release(e->globalPosition()); } }
    void keyPressEvent(QKeyEvent* e) override { if (e->key() == Qt::Key_Escape) owner_.cancel(); }
private:
    SelectionOverlay& owner_;
    QRectF logical_;
};
SelectionOverlay::SelectionOverlay(QObject* parent) : QObject(parent) {}
SelectionOverlay::~SelectionOverlay() { finish(); for (auto* s : surfaces_) delete s; }
void SelectionOverlay::start() {
    for (QScreen* screen : QGuiApplication::screens()) { auto* surface = new Surface(*this, screen); surfaces_.append(surface); surface->show(); surface->raise(); }
    Surface* focus = surfaces_.isEmpty() ? nullptr : surfaces_.first();
    for (auto* s : surfaces_) if (s->geometry().contains(QCursor::pos())) focus = s;
    if (focus) focus->grabKeyboard();
    connect(qGuiApp, &QGuiApplication::screenAdded, this, [this] { cancel({"display_changed", "Display changed during region selection", {}}); });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this] { cancel({"display_changed", "Display changed during region selection", {}}); });
}
void SelectionOverlay::press(QPointF point) { if (finished_) return; anchor_ = point; dragging_ = true; selection_ = QRectF(point, QSizeF{}); }
void SelectionOverlay::move(QPointF point) { if (!dragging_ || finished_) return; selection_ = QRectF(anchor_, point).normalized(); for (auto* s : surfaces_) s->refresh(); }
void SelectionOverlay::release(QPointF point) {
    if (!dragging_ || finished_) return;
    move(point);
    if (selection_.width() < 4 || selection_.height() < 4) { cancel(); return; }
    const auto root = toRoot(selection_);
    finish();
    emit selected(root);
}
void SelectionOverlay::complete(const QRect& root) {
    if (finished_) return;
    finish();
    emit selected(root);
}
void SelectionOverlay::finish() { finished_ = true; for (auto* s : surfaces_) { s->releaseKeyboard(); s->releaseMouse(); s->hide(); } }
void SelectionOverlay::cancel(Error error) { if (finished_) return; finish(); emit cancelled(error); }

BoundsOverlay::BoundsOverlay(QObject* parent) : QObject(parent) {}
BoundsOverlay::~BoundsOverlay() { hideAll(); for (auto* w : ring_ + dim_) delete w; }
namespace {
// A flat, input-transparent surface: ring segments and dim strips.
class PassiveSurface final : public QWidget {
public:
    PassiveSurface(const QColor& color) : QWidget(nullptr, passiveFlags), color_(color) {
        setObjectName("putgd-record-bounds");
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_NoSystemBackground);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), color_);
    }
private:
    QColor color_;
};
}
void BoundsOverlay::add(QList<QWidget*>& into, const QRect& rect, const QColor& color) {
    if (rect.width() < 1 || rect.height() < 1) return;
#ifdef PUTGD_RECORD_LAYER_SHELL
    if (surfaceMode() == SurfaceMode::LayerShell) {
        // Layer surfaces live on one output: split strips that cross a seam.
        for (QScreen* screen : QGuiApplication::screens()) {
            const QRect piece = rect.intersected(screen->geometry());
            if (piece.isEmpty()) continue;
            auto* w = new PassiveSurface(color);
            toLayer(w, screen, piece, false);
            into.append(w);
            w->show();
        }
        return;
    }
#endif
    auto* w = new PassiveSurface(color);
    w->setGeometry(fromRoot(rect).toAlignedRect());
    into.append(w);
    w->show();
}
void BoundsOverlay::showBounds(const QRect& root) {
    hideAll();
    rect_ = root;
    constexpr int thickness = 3;
    const QRect outer = root.adjusted(-thickness, -thickness, thickness, thickness);
    const QList<QRect> edges{QRect(outer.left(), outer.top(), outer.width(), thickness), QRect(outer.left(), root.bottom() + 1, outer.width(), thickness),
        QRect(outer.left(), root.top(), thickness, root.height()), QRect(root.right() + 1, root.top(), thickness, root.height())};
    for (const auto& edge : edges) add(ring_, edge, QColor(255, 255, 255, 235));
    // A dark hairline inside the white ring keeps it visible over light content.
    const QRect inner = root.adjusted(-1, -1, 1, 1);
    const QList<QRect> lines{QRect(inner.left(), inner.top(), inner.width(), 1), QRect(inner.left(), root.bottom() + 1, inner.width(), 1), QRect(inner.left(), root.top(), 1, root.height()), QRect(root.right() + 1, root.top(), 1, root.height())};
    for (const auto& line : lines) add(ring_, line, QColor(0, 0, 0, 200));
    if (dimmed_) setDimmed(true);
}
void BoundsOverlay::setDimmed(bool dimmed) {
    dimmed_ = dimmed;
    for (auto* w : dim_) delete w;
    dim_.clear();
    if (!dimmed || rect_.isEmpty()) return;
    QRect desktop;
    for (QScreen* screen : QGuiApplication::screens()) desktop |= surfaceMode() == SurfaceMode::X11 ? toRoot(screen->geometry()) : screen->geometry();
    const QList<QRect> strips{QRect(desktop.left(), desktop.top(), desktop.width(), rect_.top() - desktop.top()), QRect(desktop.left(), rect_.bottom() + 1, desktop.width(), desktop.bottom() - rect_.bottom()),
        QRect(desktop.left(), rect_.top(), rect_.left() - desktop.left(), rect_.height()), QRect(rect_.right() + 1, rect_.top(), desktop.right() - rect_.right(), rect_.height())};
    for (const auto& strip : strips) add(dim_, strip, QColor(0, 0, 0, 110));
}
void BoundsOverlay::hideAll() {
    for (auto* w : ring_ + dim_) delete w;
    ring_.clear();
    dim_.clear();
    rect_ = {};
}

ControlBar::ControlBar(const Options& options, const RecordingConfig& config, QWidget* parent) : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::X11BypassWindowManagerHint), config_(config), options_(options) {
    setObjectName("putgd-record-bar");
    setWindowTitle(QString::fromLatin1(controlBarTitle));
    setAttribute(Qt::WA_ShowWithoutActivating);
    setStyleSheet(barStyle);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(8);
    dot_ = new QLabel(this);
    dot_->setFixedSize(12, 12);
    time_ = new QLabel("0:00.0", this);
    time_->setObjectName("time");
    target_ = new QLabel(options.targetLabel, this);
    audio_ = new QCheckBox("System audio", this);
    audio_->setChecked(config.audio.system);
    format_ = new QComboBox(this);
    for (const auto& format : options.formats) {
        const auto codecs = options.codecs[format].toArray();
        if (codecs.isEmpty()) format_->addItem(formatLabel(format, {}), QStringList{format, QString()});
        for (const auto& codec : codecs) { format_->addItem(formatLabel(format, codec.toString()), QStringList{format, codec.toString()}); format_->setItemData(format_->count() - 1, formatHint(format, codec.toString()), Qt::ToolTipRole); }
    }
    for (int i = 0; i < format_->count(); ++i) { const auto value = format_->itemData(i).toStringList(); if (value.size() == 2 && value[0] == config.format && value[1] == config.codec) format_->setCurrentIndex(i); }
    record_ = new QPushButton("Record", this); record_->setObjectName("record");
    pause_ = new QPushButton("Pause", this);
    resume_ = new QPushButton("Resume", this);
    stop_ = new QPushButton("Stop", this); stop_->setObjectName("stop");
    cancel_ = new QPushButton("Cancel", this);
    for (auto* w : std::initializer_list<QWidget*>{dot_, time_, target_, audio_, format_, record_, pause_, resume_, stop_, cancel_}) layout->addWidget(w);
    connect(record_, &QPushButton::clicked, this, &ControlBar::recordRequested);
    connect(pause_, &QPushButton::clicked, this, &ControlBar::pauseRequested);
    connect(resume_, &QPushButton::clicked, this, &ControlBar::resumeRequested);
    connect(stop_, &QPushButton::clicked, this, &ControlBar::stopRequested);
    connect(cancel_, &QPushButton::clicked, this, &ControlBar::cancelRequested);
    connect(audio_, &QCheckBox::toggled, this, [this](bool on) { if (config_.audio.system == on) return; config_.audio.system = on; emit configChanged(config_); });
    connect(format_, &QComboBox::currentIndexChanged, this, [this](int index) {
        const auto value = format_->itemData(index).toStringList();
        if (value.size() != 2 || (value[0] == config_.format && value[1] == config_.codec)) return;
        config_.format = value[0];
        config_.codec = value[1];
        if (config_.silent()) config_.audio.system = false;
        refresh();
        emit configChanged(config_);
    });
    refresh();
}
void ControlBar::refresh() {
    const bool ready = state_ == "ready", recording = state_ == "recording", paused = state_ == "paused";
    const bool handle = handle_ && !ready;
    dot_->setStyleSheet(QStringLiteral("background: %1; border-radius: 6px;").arg(recording ? "#e53935" : paused ? "#f9a825" : ready ? "#4caf50" : "#9e9e9e"));
    audio_->setEnabled(ready && options_.audioSupported && !config_.silent());
    audio_->setChecked(config_.audio.system && !config_.silent());
    audio_->setToolTip(config_.silent() ? QStringLiteral("GIF and WebP are silent") : QStringLiteral("Include the sound the computer plays"));
    format_->setEnabled(ready);
    for (auto* w : std::initializer_list<QWidget*>{time_, target_, audio_, format_, cancel_}) w->setVisible(!handle);
    record_->setVisible(ready);
    pause_->setVisible(recording && !handle);
    resume_->setVisible(paused);
    stop_->setVisible(recording || paused);
    stop_->setEnabled(state_ != "stopping");
    cancel_->setEnabled(state_ != "stopping");
    layout()->setContentsMargins(handle ? QMargins(8, 4, 8, 4) : QMargins(12, 8, 12, 8));
    adjustSize();
    reposition();
}
void ControlBar::setState(const QString& state) { state_ = state; refresh(); }
void ControlBar::setRetained(qint64 us) {
    const qint64 tenths = us / 100000;
    time_->setText(QStringLiteral("%1:%2.%3").arg(tenths / 600).arg((tenths / 10) % 60, 2, 10, QChar('0')).arg(tenths % 10));
}
QSize ControlBar::widestSize() {
    const QString state = state_;
    const bool handle = handle_;
    const Placed placed = std::exchange(placed_, Placed::None);
    handle_ = false;
    QSize widest;
    for (const char* each : {"ready", "recording", "paused", "stopping"}) { state_ = QString::fromLatin1(each); refresh(); widest = widest.expandedTo(size()); }
    state_ = state;
    handle_ = handle;
    refresh();
    placed_ = placed;
    return widest;
}
bool ControlBar::placeAround(const QRect& root) {
    recorded_ = fromRootSpan(root);
    outside_ = placeBar(widestSize(), recorded_, screenRects()).outside;
    placed_ = Placed::Root;
    dragged_ = false;
    reposition();
    return outside_;
}
bool ControlBar::placeOnLayer(const QRect& logical) {
    recorded_ = logical;
    outside_ = placeBar(widestSize(), recorded_, screenRects()).outside;
#ifdef PUTGD_RECORD_LAYER_SHELL
    placed_ = Placed::Layer;
#else
    placed_ = Placed::Root;
#endif
    dragged_ = false;
    reposition();
    return outside_;
}
void ControlBar::reposition() {
    if (placed_ == Placed::None) return;
    const QList<QRect> screens = screenRects();
    if (dragged_ && screen_ >= 0 && screen_ < screens.size()) {
        // Keep the user's spot, anchored by its right edge, on the same screen.
        const QRect kept = clampToScreen(QRect(QPoint(spot_.right() + 1 - width(), spot_.top()), size()), screens[screen_]);
        if (!outside_ || !kept.intersects(recorded_)) { apply(kept, screen_); return; }
        dragged_ = false;
    }
    // A bar that had to sit inside stays in the corner even when its handle
    // would fit outside, so what prepare reported stays true.
    const BarSpot spot = placeBar(size(), recorded_, screens, outside_);
    apply(spot.rect, spot.screen);
}
void ControlBar::apply(const QRect& logical, int screen) {
    spot_ = logical;
    screen_ = screen;
#ifdef PUTGD_RECORD_LAYER_SHELL
    if (placed_ == Placed::Layer) {
        const auto screens = QGuiApplication::screens();
        // A layer surface stays on the output it was created for.
        if (layerScreen_ >= 0 && layerScreen_ < screens.size() && screen != layerScreen_) {
            screen_ = layerScreen_;
            spot_ = clampToScreen(logical, screens[layerScreen_]->geometry());
        }
        if (screen_ < 0 || screen_ >= screens.size()) return;
        layerScreen_ = screen_;
        toLayer(this, screens[screen_], spot_, true);
        return;
    }
#endif
    setGeometry(spot_);
}
void ControlBar::mousePressEvent(QMouseEvent* e) {
    // X11 bars bypass the window manager, so the bar moves itself.
    if (e->button() == Qt::LeftButton && placed_ == Placed::Root && surfaceMode() == SurfaceMode::X11) {
        dragging_ = true;
        dragOffset_ = e->globalPosition().toPoint() - spot_.topLeft();
        e->accept();
        return;
    }
    QWidget::mousePressEvent(e);
}
void ControlBar::mouseMoveEvent(QMouseEvent* e) {
    if (!dragging_) { QWidget::mouseMoveEvent(e); return; }
    const QPoint global = e->globalPosition().toPoint();
    const QList<QRect> screens = screenRects();
    int screen = -1;
    for (int i = 0; i < screens.size(); ++i) if (screens[i].contains(global)) screen = i;
    if (screen < 0) return;
    const QRect rect = clampToScreen(QRect(global - dragOffset_, size()), screens[screen]);
    // A bar that stays out of the clip cannot be dragged into it.
    if (outside_ && rect.intersects(recorded_)) return;
    dragged_ = true;
    apply(rect, screen);
}
void ControlBar::mouseReleaseEvent(QMouseEvent* e) {
    if (dragging_ && e->button() == Qt::LeftButton) { dragging_ = false; return; }
    QWidget::mouseReleaseEvent(e);
}
void ControlBar::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) { if (state_ == "ready") emit cancelRequested(); return; }
    QWidget::keyPressEvent(e);
}
}
