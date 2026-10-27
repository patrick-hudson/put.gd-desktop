#include "Overlays.h"
#include "BarPlacement.h"
#include "ui/SelectionOutline.h"
#include "ui/RegionSelection.h"
#include "ui/Theme.h"
#include "ui/VisualStyle.h"
#include "ui/Graphite.h"
#include "ui/Fonts.h"
#include <QApplication>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QFontMetricsF>
#include <QPalette>
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
// The bar in the interface's scheme (#198). Its outline meets 3:1 against
// both black and white in either scheme, so the bar's edge shows over any
// content; the primary actions use the shared neutral role.

}

QString formatHint(const QString& format, const QString& codec) {
    if (format == "mp4" && codec == "h265") return QStringLiteral("Smaller files, but Firefox and Chrome on Linux cannot play H.265 links; Safari and Edge can. Use MP4 (H.264) for broader playback compatibility.");
    if (format == "mp4") return QStringLiteral("Recommended for browser sharing.");
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

namespace {
QString displayName(QScreen* screen) {
    return screen->name().isEmpty() ? QStringLiteral("Display %1").arg(QGuiApplication::screens().indexOf(screen) + 1) : screen->name();
}
}

class SelectionOverlay::Surface final : public QWidget {
public:
    Surface(SelectionOverlay& owner, QScreen* screen) : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::X11BypassWindowManagerHint | Qt::WindowDoesNotAcceptFocus), owner_(owner), logical_(screen->geometry()) {
        setObjectName("putgd-record-select");
        setAccessibleName("Recording region");
        describeRegion(this, {}, true, displayName(screen));
        setAttribute(Qt::WA_TranslucentBackground);
        setScreen(screen);
        setGeometry(logical_.toAlignedRect());
        setCursor(Qt::CrossCursor);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        connect(&Theme::Notifier::instance(), &Theme::Notifier::changed, this, qOverload<>(&QWidget::update));
    }
    void refresh() { describeRegion(this, owner_.selection_, true, displayName(QGuiApplication::screens()[activeScreen()])); update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), QColor(0, 0, 0, 70));
        const auto clipped = owner_.selection_.isEmpty() && screen() == QGuiApplication::screens()[activeScreen()] ? logical_ : owner_.selection_.intersected(logical_);
        if (!clipped.isEmpty()) {
            const QRectF local = clipped.translated(-logical_.topLeft());
            painter.fillRect(local, Qt::transparent);
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
            paintSelectionOutline(painter, local);
        }
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        if (owner_.keyboardScreen_ < 0 || screen() == QGuiApplication::screens()[activeScreen()])
            paintRegionInstructions(painter, rect(), palette(), regionInstructions(owner_.selection_, true, displayName(QGuiApplication::screens()[activeScreen()])));
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::RightButton) { owner_.cancel(); return; }
        if (e->button() == Qt::LeftButton) { owner_.press(e->globalPosition()); grabMouse(); }
    }
    void mouseMoveEvent(QMouseEvent* e) override { owner_.move(e->globalPosition()); }
    void mouseReleaseEvent(QMouseEvent* e) override { if (e->button() == Qt::LeftButton) { releaseMouse(); owner_.release(e->globalPosition()); } }
    bool event(QEvent* e) override {
        if (e->type() == QEvent::KeyPress) { auto* key = static_cast<QKeyEvent*>(e); if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) { keyPressEvent(key); return true; } }
        return QWidget::event(e);
    }
    int activeScreen() const { return owner_.keyboardScreen_ >= 0 ? owner_.keyboardScreen_ : std::max(0, int(QGuiApplication::screens().indexOf(screen()))); }
    void keyPressEvent(QKeyEvent* e) override {
        int screen = activeScreen();
        QList<QRectF> screens;
        for (auto* output : QGuiApplication::screens()) screens.append(output->geometry());
        const bool switched = switchRegionDisplay(owner_.selection_, screens, screen, e->key(), e->modifiers());
        switch (switched ? RegionKeyAction::Change : regionKey(owner_.selection_, screens[screen], e->key(), e->modifiers())) {
        case RegionKeyAction::Cancel: owner_.cancel(); break;
        case RegionKeyAction::Accept: owner_.complete(toRoot(owner_.selection_)); break;
        case RegionKeyAction::Change: owner_.keyboardScreen_ = screen; owner_.dragging_ = false; for (auto* s : owner_.surfaces_) s->refresh(); break;
        case RegionKeyAction::Ignore: e->ignore(); break;
        }
    }

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
    if (focus) { keyboardScreen_ = QGuiApplication::screens().indexOf(focus->screen()); focus->grabKeyboard(); for (auto* surface : surfaces_) surface->refresh(); }
    connect(qGuiApp, &QGuiApplication::screenAdded, this, [this] { cancel({"display_changed", "Display changed during region selection", {}}); });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this] { cancel({"display_changed", "Display changed during region selection", {}}); });
}
void SelectionOverlay::press(QPointF point) { if (finished_) return; keyboardScreen_ = -1; anchor_ = point; dragging_ = true; selection_ = QRectF(point, QSizeF{}); }
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
    void setColor(const QColor& color) { if (color_ == color) return; color_ = color; update(); }
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
    // KWin stacks overlay layer surfaces in the order they map, and raise() does
    // not reorder them: strips mapped at Record would cover the bar (#127). On
    // the layer shell they are mapped now, clear, before the bar, and Record
    // only darkens them.
    if (dimmed_ || surfaceMode() == SurfaceMode::LayerShell) addDim(dimmed_ ? dimColor : Qt::transparent);
}
void BoundsOverlay::addDim(const QColor& color) {
    for (auto* w : dim_) delete w;
    dim_.clear();
    if (rect_.isEmpty()) return;
    QRect desktop;
    for (QScreen* screen : QGuiApplication::screens()) desktop |= surfaceMode() == SurfaceMode::X11 ? toRoot(screen->geometry()) : screen->geometry();
    const QList<QRect> strips{QRect(desktop.left(), desktop.top(), desktop.width(), rect_.top() - desktop.top()), QRect(desktop.left(), rect_.bottom() + 1, desktop.width(), desktop.bottom() - rect_.bottom()),
        QRect(desktop.left(), rect_.top(), rect_.left() - desktop.left(), rect_.height()), QRect(rect_.right() + 1, rect_.top(), desktop.right() - rect_.right(), rect_.height())};
    for (const auto& strip : strips) add(dim_, strip, color);
}
void BoundsOverlay::setDimmed(bool dimmed) {
    dimmed_ = dimmed;
    if (surfaceMode() == SurfaceMode::LayerShell && !dim_.isEmpty()) {
        for (auto* w : dim_) static_cast<PassiveSurface*>(w)->setColor(dimmed ? dimColor : QColor(Qt::transparent));
        return;
    }
    if (dimmed) addDim(dimColor);
    else { for (auto* w : dim_) delete w; dim_.clear(); }
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
    // Graphite controls; the bar paints its own surface and outline.
    Fonts::load();
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) VisualStyle::apply(*app);
    connect(&Theme::Notifier::instance(), &Theme::Notifier::changed, this, [this] { refresh(); update(); });
    auto* rows = new QVBoxLayout(this);
    rows->setContentsMargins(12, 8, 12, 8);
    rows->setSpacing(6);
    top_ = new QHBoxLayout;
    top_->setSpacing(8);
    rows->addLayout(top_);
    dot_ = new QLabel(this);
    dot_->setFixedSize(12, 12);
    // The state is written, not only coloured.
    state_label_ = new QLabel(this);
    state_label_->setObjectName("state");
    VisualStyle::role(state_label_, "strong");
    time_ = new QLabel("0:00.0", this);
    time_->setObjectName("time");
    time_->setFont(Graphite::monoFont(Graphite::Type::body));
    time_->setMinimumWidth(QFontMetrics(time_->font()).horizontalAdvance(QStringLiteral("00:00.0")) + 4);
    choices_ = new QWidget(this);
    choices_->setObjectName("choices");
    auto* choices = new QHBoxLayout(choices_);
    choices->setContentsMargins(0, 0, 0, 0);
    choices->setSpacing(8);
    target_ = new QLabel(options.targetLabel, choices_);
    target_->setObjectName("target");
    audio_ = new QCheckBox("System audio", choices_);
    audio_->setChecked(config.audio.system);
    format_ = VisualStyle::choiceBox(choices_);
    format_->setObjectName("format");
    format_->setAccessibleName("Recording format");
    format_->setToolTip("The format the recording is reviewed and exported in");
    for (const auto& format : options.formats) {
        const auto codecs = options.codecs[format].toArray();
        if (codecs.isEmpty()) format_->addItem(formatLabel(format, {}), QStringList{format, QString()});
        for (const auto& codec : codecs) { format_->addItem(formatLabel(format, codec.toString()), QStringList{format, codec.toString()}); format_->setItemData(format_->count() - 1, formatHint(format, codec.toString()), Qt::ToolTipRole); }
    }
    for (int i = 0; i < format_->count(); ++i) { const auto value = format_->itemData(i).toStringList(); if (value.size() == 2 && value[0] == config.format && value[1] == config.codec) format_->setCurrentIndex(i); }
    lockedAudio_ = new QLabel(choices_);
    lockedAudio_->setObjectName("lockedAudio");
    lockedFormat_ = new QLabel(choices_);
    lockedFormat_->setObjectName("lockedFormat");
    for (auto* label : {target_, lockedAudio_, lockedFormat_}) VisualStyle::role(label, "muted");
    for (auto* w : std::initializer_list<QWidget*>{target_, audio_, format_, lockedAudio_, lockedFormat_}) choices->addWidget(w);
    record_ = new QPushButton("Start recording", this); record_->setObjectName("record");
    record_->setIcon(VisualStyle::icon("record"));
    VisualStyle::role(record_, "primary");
    pause_ = new QPushButton("Pause", this);
    pause_->setIcon(VisualStyle::icon("pause"));
    resume_ = new QPushButton("Resume", this);
    resume_->setIcon(VisualStyle::icon("play"));
    stop_ = new QPushButton("Stop recording", this); stop_->setObjectName("stop");
    VisualStyle::role(stop_, "primary");
    cancel_ = new QPushButton("Cancel", this);
    cancel_->setObjectName("cancel");
    cancel_->setToolTip("Discard this recording");
    VisualStyle::role(cancel_, "quiet");
    // The discard question, in the bar so it stays where the bar may be (never over the clip).
    question_ = new QLabel("Discard this recording?", this);
    question_->setObjectName("discardQuestion");
    VisualStyle::role(question_, "strong");
    keep_ = new QPushButton("Keep recording", this);
    keep_->setObjectName("keepRecording");
    discard_ = new QPushButton("Discard recording", this);
    discard_->setObjectName("discardRecording");
    VisualStyle::role(discard_, "danger");
    for (auto* w : std::initializer_list<QWidget*>{dot_, state_label_, time_, choices_, question_}) top_->addWidget(w);
    top_->addStretch(1);
    for (auto* w : std::initializer_list<QWidget*>{record_, pause_, resume_, stop_, keep_, discard_, cancel_}) top_->addWidget(w);
    connect(record_, &QPushButton::clicked, this, &ControlBar::recordRequested);
    connect(pause_, &QPushButton::clicked, this, &ControlBar::pauseRequested);
    connect(resume_, &QPushButton::clicked, this, &ControlBar::resumeRequested);
    connect(stop_, &QPushButton::clicked, this, &ControlBar::stopRequested);
    // Before Record there is nothing to lose; after it, Discard… asks first.
    connect(cancel_, &QPushButton::clicked, this, [this] { if (state_ == "ready") emit cancelRequested(); else askDiscard(); });
    setTabOrder(keep_, discard_);
    connect(keep_, &QPushButton::clicked, this, &ControlBar::keepRecording);
    connect(discard_, &QPushButton::clicked, this, [this] { confirming_ = false; refresh(); emit cancelRequested(); });
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
    const QScreen* primary = QGuiApplication::primaryScreen();
    fitTo(primary ? primary->geometry() : QRect());
}
void ControlBar::arrange(bool twoRows) {
    twoRows_ = twoRows;
    auto* rows = static_cast<QVBoxLayout*>(layout());
    top_->removeWidget(choices_);
    rows->removeWidget(choices_);
    if (twoRows) rows->addWidget(choices_, 0, Qt::AlignLeft);
    else top_->insertWidget(top_->indexOf(time_) + 1, choices_);
    refresh();
}
void ControlBar::fitTo(const QRect& screen) {
    // Two rows when one would take most of the screen's width: a 150% text
    // size on a 1366 px laptop, or a narrow portrait monitor.
    arrange(false);
    if (screen.width() > 0 && widestSize().width() > screen.width() * 7 / 10) arrange(true);
}
void ControlBar::askDiscard() {
    if (state_ != "recording" && state_ != "paused") return;
    confirming_ = true;
    refresh();
    keep_->setFocus();
}
void ControlBar::keepRecording() { confirming_ = false; refresh(); }
void ControlBar::refresh() {
    const bool ready = state_ == "ready", recording = state_ == "recording", paused = state_ == "paused";
    const bool handle = handle_ && !ready;
    // The question belongs to a recording that can still be discarded.
    if (!recording && !paused) confirming_ = false;
    const bool asking = confirming_ && !handle;
    const auto& tokens = Theme::current();
    setPalette(Graphite::palette());
    setProperty("themeScheme", Theme::name(Theme::scheme()));
    dot_->setStyleSheet(QStringLiteral("background: %1; border-radius: 6px;").arg((recording ? tokens.danger : paused ? tokens.warning : ready ? tokens.success : tokens.textMuted).name()));
    state_label_->setText(recording ? QStringLiteral("Recording") : paused ? QStringLiteral("Paused") : ready ? QStringLiteral("Ready to record") : QStringLiteral("Stopping…"));
    // Once recording starts these are information, not disabled choices.
    const bool withSound = config_.audio.system && !config_.silent();
    audio_->setEnabled(ready && options_.audioSupported && !config_.silent());
    audio_->setChecked(withSound);
    audio_->setToolTip(config_.silent() ? QStringLiteral("GIF and WebP are silent") : ready ? QStringLiteral("Include the sound the computer plays") : withSound ? QStringLiteral("System audio is being recorded") : QStringLiteral("System audio is not being recorded"));
    format_->setEnabled(ready);
    audio_->setVisible(ready);
    format_->setVisible(ready);
    lockedAudio_->setVisible(!ready);
    lockedFormat_->setVisible(!ready);
    lockedAudio_->setText(withSound ? QStringLiteral("System audio") : QStringLiteral("No audio"));
    lockedAudio_->setToolTip(audio_->toolTip());
    lockedAudio_->setAccessibleName(QStringLiteral("Audio: ") + lockedAudio_->text());
    lockedFormat_->setText(format_->currentText());
    lockedFormat_->setAccessibleName(QStringLiteral("Recording format: ") + lockedFormat_->text());
    lockedFormat_->setToolTip(QStringLiteral("Recording format; choose the export format in review"));
    cancel_->setText(ready ? QStringLiteral("Cancel") : QStringLiteral("Discard…"));
    keep_->setText(paused ? QStringLiteral("Keep paused") : QStringLiteral("Keep recording"));
    for (auto* w : std::initializer_list<QWidget*>{state_label_, time_}) w->setVisible(!handle);
    choices_->setVisible(!handle && !asking);
    cancel_->setVisible(!handle && !asking);
    record_->setVisible(ready);
    pause_->setVisible(recording && !handle && !asking);
    resume_->setVisible(paused && !asking);
    stop_->setVisible((recording || paused) && !asking);
    for (auto* w : std::initializer_list<QWidget*>{question_, keep_, discard_}) w->setVisible(asking);
    stop_->setEnabled(state_ != "stopping");
    cancel_->setEnabled(state_ != "stopping");
    cancel_->setToolTip(ready ? QStringLiteral("Close without recording") : QStringLiteral("Discard this recording (asks first)"));
    layout()->setContentsMargins(handle ? QMargins(8, 4, 8, 4) : QMargins(12, 8, 12, 8));
    adjustSize();
    reposition();
}
void ControlBar::setState(const QString& state) { state_ = state; refresh(); }
void ControlBar::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), Theme::current().bg);
    painter.setPen(Theme::current().controlOutline);
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
}
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
    const bool asking = std::exchange(confirming_, false);
    for (const char* each : {"ready", "recording", "paused", "stopping"}) {
        state_ = QString::fromLatin1(each);
        for (const bool question : {false, true}) { confirming_ = question; refresh(); widest = widest.expandedTo(size()); }
    }
    state_ = state;
    confirming_ = asking;
    handle_ = handle;
    refresh();
    placed_ = placed;
    return widest;
}
bool ControlBar::placeAround(const QRect& root) {
    recorded_ = fromRootSpan(root);
    if (const auto screens = screenRects(); !screens.isEmpty()) fitTo(screens.value(std::max(0, homeScreen(recorded_, screens))));
    outside_ = placeBar(widestSize(), recorded_, screenRects()).outside;
    placed_ = Placed::Root;
    dragged_ = false;
    reposition();
    return outside_;
}
bool ControlBar::placeOnLayer(const QRect& logical) {
    recorded_ = logical;
    if (const auto screens = screenRects(); !screens.isEmpty()) fitTo(screens.value(std::max(0, homeScreen(recorded_, screens))));
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
    // Escape is the safe answer to the discard question; before Record it cancels.
    if (e->key() == Qt::Key_Escape) { if (confirming_) keepRecording(); else if (state_ == "ready") emit cancelRequested(); return; }
    QWidget::keyPressEvent(e);
}
}
