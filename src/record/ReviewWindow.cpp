#include "ReviewWindow.h"
#include "ClipExporter.h"
#include "DebugLog.h"
#include "Duration.h"
#include "EffectiveAudio.h"
#include "Filmstrip.h"
#include "FilmstripTimeline.h"
#include "Overlays.h"
#include "ReviewPanel.h"
#include "platform/HostOpen.h"
#include "ui/FileDialog.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"
#include "ui/Ui.h"
#include <QAbstractSpinBox>
#include <QAccessible>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLineF>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QSet>
#include <QShortcut>
#include <QThread>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <tuple>
#include <utility>

namespace putgd::record {
namespace {
constexpr int handleSize = 8;
constexpr qint64 secondUs = 1000000;
// Skimming and snapping, remembered for the session (#1613 A7, B4): on until
// the user turns them off with S / N or the transport's Skim toggle.
bool sessionSkimming = true, sessionSnapping = true;
// The exact skim frames kept (B2): about 30, never more than 64 MB of pictures.
constexpr int skimCacheFrames = 30;
constexpr qint64 skimCacheBytes = 64ll * 1024 * 1024;
// The live cover's request for the frame without it: never a revision the skim accepts.
constexpr quint64 uncoveredRevision = std::numeric_limits<quint64>::max();
bool textField(const QWidget* w) { return qobject_cast<const QLineEdit*>(w) || qobject_cast<const QAbstractSpinBox*>(w); }
int reviewSpace(Ui::Space value) { return static_cast<int>(value); }
// Text controls keep at least 3 px above and below their full text line at every scale.
int textControlHeight(const QWidget* widget) { return std::max(28, widget->fontMetrics().height() + 2 * 3); }
// #1613 row heights (design.md "Window"), never smaller than the text needs.
int toolRowHeight(const QWidget* w) { return std::max(52, textControlHeight(w) + 20); }
int transportHeight(const QWidget* w) { return std::max(42, textControlHeight(w) + 12); }
int footerHeight(const QWidget* w) { return std::max(56, textControlHeight(w) + 24); }
int destinationMenuPadding() { return 6 + Ui::scaled(8) + 2; }
QString sharingName(const QString& mode) { return mode == "private" ? QStringLiteral("Private upload") : QStringLiteral("Shared link"); }
QString formatShort(const QString& format) {
    return format == "webm" ? QStringLiteral("WebM") : format == "webp" ? QStringLiteral("WebP") : format.toUpper();
}
// Media overlays use fixed colours, not the app theme: they sit on captured
// content, which is as likely pale as dark (visual language, Recording).
const QColor mediaAccent(0xE8, 0xB6, 0x6D);
// A cover outline readable on any content: a dark line under a light (or the
// accent, when selected) one.
void dualOutline(QPainter& painter, const QRectF& r, const QColor& colour, qreal width, Qt::PenStyle style) {
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(0, 0, 0, 170), width + 2, Qt::SolidLine));
    painter.drawRect(r);
    painter.setPen(QPen(colour, width, style));
    painter.drawRect(r);
}
// The destination with its sharing mode ("Project archive · Shared link"); a
// long name elides before the mode. The popup keeps the full details.
class DestinationMenu final : public QToolButton {
public:
    using QToolButton::QToolButton;
    QSize sizeHint() const override { return {fontMetrics().horizontalAdvance(text()) + destinationMenuPadding() + 12, textControlHeight(this)}; }
    QSize minimumSizeHint() const override {
        const auto suffix = property("sharingSuffix").toString();
        const int name = std::min(fontMetrics().horizontalAdvance(property("destinationName").toString()), fontMetrics().horizontalAdvance(QStringLiteral("Project a…")));
        return {name + fontMetrics().horizontalAdvance(suffix) + destinationMenuPadding() + 12, textControlHeight(this)};
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QStyleOptionToolButton option;
        initStyleOption(&option);
        QPainter painter(this);
        if (option.state & (QStyle::State_MouseOver | QStyle::State_Sunken | QStyle::State_HasFocus))
            style()->drawPrimitive(QStyle::PE_PanelButtonTool, &option, &painter, this);
        const auto ink = isEnabled() ? Theme::current().textSecondary : Theme::current().textDisabled;
        const auto textRect = rect().adjusted(6, 0, -destinationMenuPadding(), 0);
        const auto suffix = property("sharingSuffix").toString();
        const auto full = property("destinationName").toString() + suffix;
        const auto display = fontMetrics().horizontalAdvance(full) <= textRect.width() ? full
            : fontMetrics().elidedText(property("destinationName").toString(), Qt::ElideRight, std::max(0, textRect.width() - fontMetrics().horizontalAdvance(suffix))) + suffix;
        painter.setFont(font());
        painter.setPen(ink);
        painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, display);
        const qreal textEnd = textRect.left() + std::min(fontMetrics().horizontalAdvance(display), textRect.width());
        Ui::paintChevron(painter, QPointF(textEnd + 6 + Ui::scaled(8) / 2.0, height() / 2.0), Ui::scaled(8), Qt::DownArrow, ink);
    }
};
// "Preview is silent" with a muted speaker glyph, in the transport.
class SilentPreview final : public QWidget {
public:
    explicit SilentPreview(QWidget* parent) : QWidget(parent) {
        setObjectName("previewSilent");
        setAccessibleName(text());
        setFont(Ui::font(Ui::Type::caption));
        connect(&Theme::Notifier::instance(), &Theme::Notifier::changed, this, qOverload<>(&QWidget::update));
    }
    static QString text() { return QStringLiteral("Preview is silent"); }
    QSize sizeHint() const override { return {glyph() + 6 + fontMetrics().horizontalAdvance(text()) + 2, fontMetrics().height()}; }
    QSize minimumSizeHint() const override { return {glyph(), fontMetrics().height()}; }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const auto ink = Theme::current().textMuted;
        Ui::paintChromeGlyph(painter, QRectF(0, (height() - glyph()) / 2.0, glyph(), glyph()), "muted-speaker", ink);
        if (width() >= sizeHint().width()) Ui::paintText(painter, QRectF(glyph() + 6, 0, width() - glyph() - 6, height()), text(), ink, font(), Qt::AlignLeft | Qt::AlignVCenter, Qt::ElideNone);
    }
private:
    static int glyph() { return Ui::scaled(14); }
};
}

// #1613: a message in the tool row's middle, never over or beside the
// picture (D9): the cover placement ("Draw the redaction · 0:08.2 – 0:12.6"
// with Cancel and its Esc key), the blur warning (warningSurface / warning) or
// a frame that couldn't be decoded (dangerSurface / danger, with Retry
// preview). The text elides when narrow; below its full width the key hint
// goes first (the compact form).
class ViewerBanner final : public QWidget {
public:
    enum class Tone { Info, Warning, Error };
    ViewerBanner(Tone tone, QWidget* parent) : QWidget(parent), tone_(tone) {
        setFont(Ui::font(Ui::Type::caption));
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        connect(&Theme::Notifier::instance(), &Theme::Notifier::changed, this, qOverload<>(&QWidget::update));
        hide();
    }
    void setText(const QString& text) {
        if (text_ == text) return;
        text_ = text;
        setAccessibleName(text);
        setToolTip(text);
        updateGeometry();
        place();
        update();
    }
    QString text() const { return text_; }
    void setGlyph(const QString& glyph) { glyph_ = glyph; update(); }
    // A quiet action at the right edge with a key hint after it.
    QPushButton* setAction(const QString& text, const QString& key) {
        action_ = new QPushButton(text, this);
        action_->setObjectName("viewerBannerAction");
        action_->setProperty("uiDensity", "compact");
        Ui::role(action_, "quiet");
        key_ = key;
        place();
        return action_;
    }
    QSize sizeHint() const override { return {fullWidth(), std::max(Ui::scaled(32), fontMetrics().height() + 14)}; }
    QSize minimumSizeHint() const override { return {120, sizeHint().height()}; }
protected:
    void resizeEvent(QResizeEvent*) override { place(); }
    void paintEvent(QPaintEvent*) override {
        const auto& c = Theme::current();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(tone_ == Tone::Warning ? c.warningSurface : tone_ == Tone::Error ? c.dangerSurface : c.surface);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), Ui::Metric::radius, Ui::Metric::radius);
        const int side = Ui::scaled(16), pad = reviewSpace(Ui::Space::m);
        const QColor ink = tone_ == Tone::Warning ? c.warning : tone_ == Tone::Error ? c.danger : c.text;
        const QRect glyph(pad, (height() - side) / 2, side, side);
        if (tone_ != Tone::Info) Ui::icon("alert", ink).paint(&painter, glyph);
        else if (!glyph_.isEmpty()) Ui::icon(glyph_).paint(&painter, glyph);
        int right = width() - pad;
        if (action_) {
            right = action_->geometry().left() - reviewSpace(Ui::Space::s);
            if (showKey()) {
                const auto keyFont = Ui::monoFont(Ui::Type::caption);
                const int keyWidth = QFontMetrics(keyFont).horizontalAdvance(key_) + 10;
                const QRectF cap(width() - pad - keyWidth, (height() - QFontMetrics(keyFont).height() - 4) / 2.0, keyWidth, QFontMetrics(keyFont).height() + 4);
                painter.setPen(QPen(c.border, 1));
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(cap.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
                Ui::paintText(painter, cap, key_, c.textMuted, keyFont, Qt::AlignCenter, Qt::ElideNone);
            }
        }
        auto textFont = font();
        textFont.setWeight(tone_ == Tone::Warning ? QFont::Medium : QFont::Normal);
        const int left = glyph.right() + reviewSpace(Ui::Space::s);
        Ui::paintText(painter, QRectF(left, 0, std::max(0, right - left), height()), text_, ink, textFont);
    }
private:
    int keySpace() const { return key_.isEmpty() ? 0 : QFontMetrics(Ui::monoFont(Ui::Type::caption)).horizontalAdvance(key_) + 10 + reviewSpace(Ui::Space::s); }
    // Glyph, the whole text, the action and its key.
    int fullWidth() const {
        const int pad = reviewSpace(Ui::Space::m);
        QFont textFont = font();
        textFont.setWeight(tone_ == Tone::Warning ? QFont::Medium : QFont::Normal);
        int w = pad + Ui::scaled(16) + reviewSpace(Ui::Space::s) + QFontMetrics(textFont).horizontalAdvance(text_) + pad;
        if (action_) w += reviewSpace(Ui::Space::s) + action_->sizeHint().width() + keySpace();
        return w;
    }
    bool showKey() const { return !key_.isEmpty() && width() >= fullWidth(); }
    void place() {
        if (!action_) return;
        const int pad = reviewSpace(Ui::Space::m);
        const int keyWidth = showKey() ? keySpace() : 0;
        const int h = std::min(height() - 4, textControlHeight(action_));
        const int w = action_->sizeHint().width();
        action_->setGeometry(width() - pad - keyWidth - w, (height() - h) / 2, w, h);
    }
    Tone tone_;
    QString text_, glyph_, key_;
    QPushButton* action_ = nullptr;
};

// ---- FrameView ----

FrameView::FrameView(QWidget* parent, std::shared_ptr<MaskNames> names) : QWidget(parent), maskNames_(names ? std::move(names) : std::make_shared<MaskNames>()), nudge_(this) {
    setMinimumSize(320, 180);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // Keyboard cover editing (#283 accessibility): the frame takes focus by Tab or click.
    setFocusPolicy(Qt::StrongFocus);
    nudge_.setSingleShot(true);
    nudge_.setInterval(350);
    connect(&nudge_, &QTimer::timeout, this, &FrameView::commitNudge);
    connect(&Theme::Notifier::instance(), &Theme::Notifier::changed, this, qOverload<>(&QWidget::update));
}
void FrameView::setFrame(const QImage& image, qint64 timeUs) { frame_ = image; timeUs_ = timeUs; setPreviewError({}); announce(); update(); }
void FrameView::setSkim(const QImage& image, qint64 timeUs) { skim_ = image; skimUs_ = timeUs; update(); }
void FrameView::clearSkim() { fading_ = {}; if (skim_.isNull()) return; skim_ = {}; update(); }
// F2: the skim picture gives way to the frame with a short crossfade.
void FrameView::fadeOutSkim(int ms) {
    if (skim_.isNull()) return;
    fading_ = skim_;
    skim_ = {};
    fadeMs_ = std::max(1, ms);
    fadeClock_.start();
    auto* tick = new QTimer(this);
    tick->setInterval(16);
    connect(tick, &QTimer::timeout, this, [this, tick] {
        if (fading_.isNull() || fadeClock_.elapsed() >= fadeMs_) { fading_ = {}; tick->deleteLater(); }
        update();
    });
    tick->start();
    update();
}
// Q10: the skim picture becomes the playhead's picture (no flash of the old frame).
void FrameView::promoteSkim() {
    fading_ = {};
    if (skim_.isNull()) return;
    frame_ = skim_;
    timeUs_ = skimUs_;
    skim_ = {};
    setPreviewError({});
    announce();
    update();
}
// Q22: a cover hovered elsewhere (its bar, its Edits row) is outlined here.
void FrameView::setLinkedHover(const QString& id) {
    if (linkedHover_ == id) return;
    linkedHover_ = id;
    update();
}
void FrameView::armCreate(const QString& kind) { armed_ = kind; setCursor(Qt::CrossCursor); announce(); update(); }
void FrameView::disarm() {
    if (drag_ == Drag::Create) drag_ = Drag::None;
    armed_.clear();
    unsetCursor();
    announce();
    update();
}
void FrameView::setPreviewError(const QString& message) {
    if (error_ == message) return;
    error_ = message;
    announce();
    update();
    // The window shows it in the tool row with Retry preview (D9).
    emit previewErrorChanged();
}
QString FrameView::previewErrorText() const {
    if (error_.isEmpty()) return {};
    // A frame still on screen is an earlier one: say which, as well as dimming it.
    return frame_.isNull() ? error_ : error_ + QStringLiteral(" Showing an earlier frame (%1).").arg(durationText(timeUs_));
}
void FrameView::resizeEvent(QResizeEvent* e) { QWidget::resizeEvent(e); clampPan(); emit zoomChanged(); }
void FrameView::setMasks(const QList<RecordingMask>& masks, const QString& selected) {
    // A keyboard nudge not yet proposed keeps its place over the last answer.
    QRect pending;
    if (nudging_) for (const auto& m : std::as_const(masks_)) if (m.id == selected_ && m.id == selected) pending = m.rect;
    // R27: a move or resize in progress keeps its box over an answer that
    // didn't change that cover; if the cover itself changed, the gesture ends.
    std::optional<QRect> dragged;
    if ((drag_ == Drag::Move || drag_ == Drag::Resize)) {
        const auto incoming = std::find_if(masks.cbegin(), masks.cend(), [&](const RecordingMask& m) { return m.id == dragId_; });
        const auto current = std::find_if(masks_.cbegin(), masks_.cend(), [&](const RecordingMask& m) { return m.id == dragId_; });
        if (incoming != masks.cend() && incoming->rect == original_ && current != masks_.cend()) dragged = current->rect;
        else { drag_ = Drag::None; moved_ = false; unsetCursor(); endLive(); }
    }
    if (selected != selected_) commitNudge();
    maskNames_->observe(masks);
    masks_ = masks;
    selected_ = selected;
    if (!pending.isNull()) for (auto& m : masks_) if (m.id == selected_) m.rect = pending;
    if (dragged) for (auto& m : masks_) if (m.id == dragId_) m.rect = *dragged;
    announce();
    update();
}
void FrameView::announce() {
    QString text = QStringLiteral("No cover selected.");
    for (const auto& m : std::as_const(masks_)) {
        if (m.id != selected_) continue;
        text = QStringLiteral("%1 selected: %2 × %3 at %4, %5. Arrows move, Shift+arrows resize, Ctrl+arrows 10 px; Delete removes, Escape deselects.")
            .arg(maskNames_->name(m)).arg(m.rect.width()).arg(m.rect.height()).arg(m.rect.x()).arg(m.rect.y());
        break;
    }
    if (!armed_.isEmpty()) text = armed_ == "solid" ? QStringLiteral("Drag on the frame to place the redaction") : QStringLiteral("Drag on the frame to place the blur");
    if (!error_.isEmpty()) text.prepend(error_ + QStringLiteral(". "));
    if (accessibleDescription() == text) return;
    setAccessibleDescription(text);
    QAccessibleEvent event(this, QAccessible::DescriptionChanged);
    QAccessible::updateAccessibility(&event);
}
QSize FrameView::source() const { return canvas_.isEmpty() ? frame_.size() : canvas_; }
QRect FrameView::area() const { return contentsRect().adjusted(0, 0, -std::min(rightInset_, std::max(0, contentsRect().width() - 160)), 0); }
void FrameView::setRightInset(int px) {
    px = std::max(0, px);
    if (rightInset_ == px) return;
    rightInset_ = px;
    clampPan();
    update();
    emit zoomChanged();
}
void FrameView::setActualSize(bool actual) {
    if (actual_ == actual) return;
    actual_ = actual;
    pan_ = {};
    if (drag_ == Drag::Pan) drag_ = Drag::None;
    update();
    emit zoomChanged();
}
int FrameView::zoomPercent() const {
    const QSize src = source();
    if (src.isEmpty()) return 100;
    if (actual_) return 100;
    return std::max(1, int(std::lround(imageRect().width() * devicePixelRatioF() * 100.0 / src.width())));
}
void FrameView::panBy(const QPointF& sourcePixels) {
    if (!actual_) return;
    pan_ += sourcePixels;
    clampPan();
    update();
}
void FrameView::clampPan() {
    const QSize src = source();
    if (!actual_ || src.isEmpty()) { pan_ = {}; return; }
    const qreal dpr = devicePixelRatioF();
    const qreal spareX = std::max(0.0, (src.width() - area().width() * dpr) / 2), spareY = std::max(0.0, (src.height() - area().height() * dpr) / 2);
    pan_ = QPointF(std::clamp(pan_.x(), -spareX, spareX), std::clamp(pan_.y(), -spareY, spareY));
}
QRect FrameView::imageRect() const {
    const QSize src = source();
    const QRect a = area();
    if (src.isEmpty()) return a;
    if (actual_) {
        // One source pixel per device pixel; pan_ is the source offset at the area's centre.
        const qreal dpr = devicePixelRatioF();
        const QSizeF size(src.width() / dpr, src.height() / dpr);
        const QPointF centre = QRectF(a).center();
        return QRectF(QPointF(centre.x() - size.width() / 2 - pan_.x() / dpr, centre.y() - size.height() / 2 - pan_.y() / dpr), size).toRect();
    }
    const QSize scaled = src.scaled(a.size(), Qt::KeepAspectRatio);
    // With an overlay over the right side the picture keeps to the left.
    const int x = rightInset_ > 0 ? a.left() : a.left() + (a.width() - scaled.width()) / 2;
    return QRect(QPoint(x, a.top() + (a.height() - scaled.height()) / 2), scaled);
}
QRect FrameView::bounds() const { return QRect(QPoint(0, 0), source()); }
QRect FrameView::defaultMaskRect() const {
    const QRect all = bounds();
    if (all.width() < 4 || all.height() < 4) return {};
    const QSize size(std::max(2, all.width() / 2), std::max(2, all.height() / 2));
    return QRect(QPoint((all.width() - size.width()) / 2, (all.height() - size.height()) / 2), size);
}
QPointF FrameView::toCanvas(const QPointF& p) const {
    const auto r = imageRect();
    const QSize src = source();
    if (r.width() < 1 || r.height() < 1) return p;
    return QPointF((p.x() - r.x()) * src.width() / r.width(), (p.y() - r.y()) * src.height() / r.height());
}
QRectF FrameView::toView(const QRect& c) const {
    const auto r = imageRect();
    const QSize src = source();
    if (src.isEmpty()) return c;
    const qreal sx = qreal(r.width()) / src.width(), sy = qreal(r.height()) / src.height();
    return QRectF(r.x() + c.x() * sx, r.y() + c.y() * sy, c.width() * sx, c.height() * sy);
}
bool FrameView::event(QEvent* e) {
    // Arrows nudge the selected cover, or pan at 100 %, only once the viewer
    // has them (a click on a cover, or Tab into the viewer, W8); otherwise they
    // step frames in the window. Esc during a gesture or a nudge, or to give the
    // arrows back, is the frame's own.
    if (e->type() == QEvent::ShortcutOverride) {
        const auto* key = static_cast<QKeyEvent*>(e);
        const bool arrow = key->key() == Qt::Key_Left || key->key() == Qt::Key_Right || key->key() == Qt::Key_Up || key->key() == Qt::Key_Down;
        if (arrow && arrowsClaimed_ && !(key->modifiers() & Qt::AltModifier) && (!selected_.isEmpty() || actual_)) e->accept();
        if (key->key() == Qt::Key_Escape && (drag_ != Drag::None || nudging_ || (arrowsClaimed_ && armed_.isEmpty()))) e->accept();
        // While placing, Esc is the window's (it stops placing) and still
        // gives the arrows back.
        else if (key->key() == Qt::Key_Escape && arrowsClaimed_) claimArrows(false);
    }
    // A lost window (W5): a gesture in progress goes back to where it started.
    if (e->type() == QEvent::WindowDeactivate && drag_ != Drag::None) cancelGesture();
    return QWidget::event(e);
}
namespace {
// Handles in the image editor's order (EditorCanvas::handleAt): 0 top-left,
// 1 top, 2 top-right, 3 right, 4 bottom-right, 5 bottom, 6 bottom-left, 7 left.
std::array<QPointF, 8> handlePoints(const QRectF& r) {
    return {r.topLeft(), QPointF(r.center().x(), r.top()), r.topRight(), QPointF(r.right(), r.center().y()),
            r.bottomRight(), QPointF(r.center().x(), r.bottom()), r.bottomLeft(), QPointF(r.left(), r.center().y())};
}
Qt::CursorShape resizeCursor(int handle) {
    return handle == 0 || handle == 4 ? Qt::SizeFDiagCursor : handle == 2 || handle == 6 ? Qt::SizeBDiagCursor : handle == 1 || handle == 5 ? Qt::SizeVerCursor : Qt::SizeHorCursor;
}
}
// The selected cover's box and its eight grips (handle order), in this
// widget's coordinates, from the same geometry hover, press and paint use.
QJsonObject FrameView::selectedGeometry() const {
    for (const auto& mask : masks_) {
        if (mask.id != selected_) continue;
        const auto r = toView(mask.rect);
        QJsonArray grips;
        const auto points = handlePoints(r);
        for (int i = 0; i < 8; ++i) grips.append(QJsonArray{points[size_t(i)].x(), points[size_t(i)].y()});
        return QJsonObject{{"box", QJsonArray{r.x(), r.y(), r.width(), r.height()}}, {"grips", grips}};
    }
    return {};
}
// The handle a point takes on a cover's view rect (W1): the nearest corner
// within its 16 × 16 px target, else the side whose band holds it.
int FrameView::handleAt(const QRectF& r, const QPointF& p) {
    // How far p lies inside each side (negative: outside it).
    const qreal left = p.x() - r.left(), right = r.right() - p.x(), top = p.y() - r.top(), bottom = r.bottom() - p.y();
    // A zone never reaches into the central 6 px: a tiny box keeps a body.
    const qreal roomX = std::max(0.0, (r.width() - 6) / 2), roomY = std::max(0.0, (r.height() - 6) / 2);
    const auto within = [](qreal inside, qreal outside, qreal in) { return inside >= -outside && inside <= in; };
    // Corners win, with 16 × 16 px targets.
    const qreal cornerX = std::min(8.0, roomX), cornerY = std::min(8.0, roomY);
    int best = -1;
    qreal nearest = std::numeric_limits<qreal>::max();
    for (const auto& [handle, x, y] : std::array<std::tuple<int, qreal, qreal>, 4>{{{0, left, top}, {2, right, top}, {4, right, bottom}, {6, left, bottom}}}) {
        if (!within(x, 8, cornerX) || !within(y, 8, cornerY)) continue;
        if (const qreal d = x * x + y * y; d < nearest) { nearest = d; best = handle; }
    }
    if (best >= 0) return best;
    // Then a band along each side, 6 px outside, anywhere along it.
    // Inside, 6 px on a box at least 40 px across that way, else 4 (R21).
    const qreal bandX = std::min(r.width() >= 40 ? 6.0 : 4.0, roomX), bandY = std::min(r.height() >= 40 ? 6.0 : 4.0, roomY);
    const bool alongX = left >= 0 && right >= 0, alongY = top >= 0 && bottom >= 0;
    for (const auto& [handle, along, inside, band] : std::array<std::tuple<int, bool, qreal, qreal>, 4>{{{1, alongX, top, bandY}, {5, alongX, bottom, bandY}, {7, alongY, left, bandX}, {3, alongY, right, bandX}}}) {
        if (!along || !within(inside, 6, band)) continue;
        if (const qreal d = std::abs(inside); d < nearest) { nearest = d; best = handle; }
    }
    return best;
}
// The opposite edge or corner stays fixed until the dragged one passes it,
// then the box flips; edges stay in the canvas and the box keeps 2 × 2 px.
QRect FrameView::resizedRect(const QRect& r, int handle, const QPoint& d, const QRect& bounds) {
    int left = r.x(), top = r.y(), right = r.x() + r.width(), bottom = r.y() + r.height();
    if (handle == 0 || handle == 6 || handle == 7) left += d.x();
    if (handle == 2 || handle == 3 || handle == 4) right += d.x();
    if (handle == 0 || handle == 1 || handle == 2) top += d.y();
    if (handle == 4 || handle == 5 || handle == 6) bottom += d.y();
    // Past the opposite side the box flips (W1), as in the image editor.
    if (left > right) std::swap(left, right);
    if (top > bottom) std::swap(top, bottom);
    const int maxRight = bounds.x() + bounds.width(), maxBottom = bounds.y() + bounds.height();
    left = std::clamp(left, bounds.x(), maxRight);
    right = std::clamp(right, bounds.x(), maxRight);
    top = std::clamp(top, bounds.y(), maxBottom);
    bottom = std::clamp(bottom, bounds.y(), maxBottom);
    // At least 2 × 2 source pixels.
    if (right - left < 2) { right = std::min(maxRight, left + 2); left = right - 2; }
    if (bottom - top < 2) { bottom = std::min(maxBottom, top + 2); top = bottom - 2; }
    return QRect(left, top, right - left, bottom - top);
}
// What a press at p would take: the selected cover's handles, then its body,
// then the body of another cover shown at this frame (topmost first). An
// unselected cover has no handles: a press never grabs what it can't see.
FrameView::Target FrameView::targetAt(const QPointF& p) const {
    for (const auto& mask : std::as_const(masks_)) {
        if (mask.id != selected_) continue;
        const auto r = toView(mask.rect);
        if (const int h = handleAt(r, p); h >= 0) return {Target::Kind::Handle, mask.id, h};
        if (r.contains(p)) return {Target::Kind::Body, mask.id, -1};
    }
    for (auto it = masks_.crbegin(); it != masks_.crend(); ++it)
        if (it->id != selected_ && it->start <= timeUs_ && timeUs_ < it->end && toView(it->rect).contains(p)) return {Target::Kind::Body, it->id, -1};
    return {};
}
void FrameView::updateHover(const QPointF& p) {
    const auto target = armed_.isEmpty() && drag_ == Drag::None && !playing_ ? targetAt(p) : Target{};
    // The last grip under the pointer and when it left it, for a press that
    // arrives late (Q23, Q24). A pointer resting on a grip sends no motion,
    // so the time counts from leaving, never from the last sample there.
    if (drag_ == Drag::None) {
        if (target.kind == Target::Kind::Handle) { gripAt_ = p; gripLeft_.invalidate(); }
        else if (gripAt_ && !gripLeft_.isValid()) gripLeft_.start();
    }
    // The outline follows a cover shown at this frame; the selected one is outlined anyway.
    QString hovered;
    for (auto it = masks_.crbegin(); it != masks_.crend(); ++it) if ((it->start <= timeUs_ && timeUs_ < it->end) && toView(it->rect).contains(p)) { hovered = it->id; break; }
    const int handle = target.kind == Target::Kind::Handle ? target.handle : -1;
    if (hovered_ != hovered || hoveredHandle_ != handle) {
        const bool changed = hovered_ != hovered;
        hovered_ = hovered;
        hoveredHandle_ = handle;
        update();
        if (changed) emit hoveredCoverChanged(hovered_);
    }
    if (drag_ != Drag::None) return;
    if (!armed_.isEmpty()) setCursor(placeWaiting_ ? Qt::BusyCursor : Qt::CrossCursor);
    else if (target.kind == Target::Kind::Handle) setCursor(resizeCursor(target.handle));
    else if (target.kind == Target::Kind::Body) setCursor(Qt::SizeAllCursor);
    // At 100 % the picture can be grabbed and panned (R30).
    else if (actual_ && imageRect().contains(p.toPoint())) setCursor(Qt::OpenHandCursor);
    else unsetCursor();
}
// ---- the live cover (D4) ----
void FrameView::beginLive(const QString& id) {
    const auto it = std::find_if(masks_.cbegin(), masks_.cend(), [&](const RecordingMask& m) { return m.id == id; });
    if (it == masks_.cend() || !(it->start <= timeUs_ && timeUs_ < it->end)) return;  // not on this frame: nothing baked to follow
    if (liveId_ == id) return;
    liveId_ = id;
    if (uncoveredId_ != id) uncovered_ = {};
    emit liveEditStarted(id, timeUs_);
    update();
}
void FrameView::previewStrength(const QString& id, int strength) {
    liveStrength_ = strength;
    beginLive(id);
    if (liveId_ != id) liveStrength_.reset();  // not shown at this frame
    update();
}
void FrameView::endLive() {
    liveStrength_.reset();
    if (liveId_.isEmpty() && uncovered_.isNull()) return;
    liveId_.clear();
    uncovered_ = {};
    uncoveredId_.clear();
    composite_ = {};
    update();
}
void FrameView::setUncovered(const QString& id, const QImage& image, qint64 timeUs) {
    if (id != liveId_) return;
    uncovered_ = image;
    uncoveredId_ = id;
    uncoveredUs_ = timeUs;
    composite_ = {};
    update();
}
void FrameView::dropUncovered() {
    if (uncovered_.isNull()) return;
    uncovered_ = {};
    uncoveredId_.clear();
    composite_ = {};
    update();
}
bool FrameView::livePending() const { return !liveId_.isEmpty() && !(uncoveredId_ == liveId_ && uncoveredUs_ == timeUs_ && !uncovered_.isNull()); }
// The picture with the live cover's effect where its box is now: the frame
// without that cover once it has arrived, else the shown frame (its old place
// still covered) with a pending outline.
const QImage& FrameView::livePicture() {
    const auto it = std::find_if(masks_.cbegin(), masks_.cend(), [&](const RecordingMask& m) { return m.id == liveId_; });
    if (it == masks_.cend()) return frame_;
    const QImage& base = livePending() ? frame_ : uncovered_;
    auto mask = *it;
    // R29: a blur strength still coalescing shows at once.
    if (liveStrength_) mask.strength = *liveStrength_;
    const auto key = std::make_tuple(base.cacheKey(), mask.rect, mask.kind, mask.strength);
    if (!composite_.isNull() && compositeKey_ == key) return composite_;
    composite_ = base.copy();
    paintCoverPreview(composite_, mask, source());
    compositeKey_ = key;
    return composite_;
}
void FrameView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), Theme::current().well);
    painter.setClipRect(area());
    const auto target = imageRect();
    Ui::paintImageShadow(painter, target);
    painter.fillRect(target, Qt::black);
    const bool skimming = !skim_.isNull();
    const bool live = !skimming && !playing_ && !liveId_.isEmpty() && !frame_.isNull();
    if (skimming) painter.drawImage(target, skim_);
    else if (live) painter.drawImage(target, livePicture());
    else if (!frame_.isNull()) painter.drawImage(target, frame_);
    if (!skimming && !fading_.isNull() && fadeClock_.isValid()) {
        painter.setOpacity(std::clamp(1.0 - double(fadeClock_.elapsed()) / fadeMs_, 0.0, 1.0));
        painter.drawImage(target, fading_);
        painter.setOpacity(1.0);
    }
    // An earlier frame under a preview error is dimmed so it never reads as current.
    if (stale() && !skimming) painter.fillRect(target, QColor(0, 0, 0, 140));
    const qint64 at = skimming ? skimUs_ : timeUs_;
    if (!playing_) for (const auto& mask : std::as_const(masks_)) {
        const bool active = mask.start <= at && at < mask.end;
        const bool selected = mask.id == selected_;
        const bool linked = mask.id == hovered_ || mask.id == linkedHover_;
        if (!active && !selected && !linked) continue;
        const auto r = toView(mask.rect);
        const bool hovered = linked && !selected;
        dualOutline(painter, r, selected ? mediaAccent : active ? QColor(255, 255, 255, hovered ? 255 : 230) : QColor(255, 255, 255, 150), selected || hovered ? 2 : 1, active ? Qt::SolidLine : Qt::DashLine);
        // The effect is still on its old place until the frame without it arrives.
        if (live && mask.id == liveId_ && livePending()) dualOutline(painter, r.adjusted(-3, -3, 3, 3), QColor(255, 255, 255, 230), 1, Qt::DotLine);
        if (selected) {
            // Handles at the corners and edge middles, each a resize grip; the hovered one grows.
            painter.setPen(QPen(QColor(0, 0, 0, 170), 1));
            painter.setBrush(mediaAccent);
            const auto points = handlePoints(r);
            for (int i = 0; i < 8; ++i) {
                const qreal side = i == hoveredHandle_ || (drag_ == Drag::Resize && i == handle_) ? handleSize + 2 : handleSize;
                painter.drawRect(QRectF(points[size_t(i)].x() - side / 2, points[size_t(i)].y() - side / 2, side, side));
            }
        }
    }
    if (drag_ == Drag::Create) {
        // The cover the release will make (F5): the default box while the
        // drag is under 8 × 8 px, else the drawn box, clipped to the frame.
        const auto pending = toView(placementAt(current_)).intersected(target);
        painter.setPen(Qt::NoPen);
        painter.setBrush(armed_ == "solid" ? QBrush(QColor(0, 0, 0, 200)) : QBrush(QColor(255, 255, 255, 60)));
        painter.drawRect(pending);
        dualOutline(painter, pending, mediaAccent, 1.5, Qt::DashLine);
    }
    painter.setClipping(false);
    if (Ui::keyboardFocus(this)) { painter.setPen(QPen(Theme::current().accent, 2)); painter.setBrush(Qt::NoBrush); painter.drawRect(QRectF(rect()).adjusted(1, 1, -1, -1)); }
}
void FrameView::focusInEvent(QFocusEvent* e) {
    QWidget::focusInEvent(e);
    // Tab into the viewer gives it the arrows (W8).
    if (e->reason() == Qt::TabFocusReason || e->reason() == Qt::BacktabFocusReason) claimArrows(true);
    update();
}
void FrameView::focusOutEvent(QFocusEvent* e) { commitNudge(); claimArrows(false); QWidget::focusOutEvent(e); update(); }
void FrameView::claimArrows(bool on) {
    if (arrowsClaimed_ == on) return;
    arrowsClaimed_ = on;
    emit arrowsClaimedChanged(on);
}
void FrameView::setPlaceWaiting(bool waiting) {
    if (placeWaiting_ == waiting) return;
    placeWaiting_ = waiting;
    if (!armed_.isEmpty()) setCursor(waiting ? Qt::BusyCursor : Qt::CrossCursor);
}
// The cover a placing drag from the press to p makes (P12, F5): under 8 × 8
// view px it is a click and places the default box at the press; anything
// larger (a thin one too) is the drawn box, clamped to the picture (W22),
// at least 2 source px each way. Empty when nothing would be made.
QRect FrameView::placementAt(const QPointF& p) const {
    const QRectF dragged = QRectF(anchor_, p).normalized();
    if (dragged.width() < 8 && dragged.height() < 8) {
        const QRect image = imageRect();
        const QPointF at(std::clamp<qreal>(anchor_.x(), image.left(), image.right()), std::clamp<qreal>(anchor_.y(), image.top(), image.bottom()));
        return defaultMaskRectAt(toCanvas(at));
    }
    QRect r = QRectF(toCanvas(anchor_), toCanvas(p)).normalized().toAlignedRect().intersected(bounds());
    const QRect all = bounds();
    if (r.width() < 2) { r.setWidth(2); r.moveLeft(std::clamp(r.left(), 0, std::max(0, all.width() - 2))); }
    if (r.height() < 2) { r.setHeight(2); r.moveTop(std::clamp(r.top(), 0, std::max(0, all.height() - 2))); }
    return r.width() >= 2 && r.height() >= 2 && all.contains(r) ? r : QRect();
}
QRect FrameView::placementPreview() const { return drag_ == Drag::Create ? placementAt(current_) : QRect(); }
// A click while armed (W6): a box about 20 % × 8 % of the frame centred on the point, kept in the frame.
QRect FrameView::defaultMaskRectAt(const QPointF& canvasPoint) const {
    const QRect all = bounds();
    if (all.width() < 4 || all.height() < 4) return {};
    const QSize size(std::max(2, all.width() / 5), std::max(2, all.height() * 8 / 100));
    const int x = std::clamp(int(std::lround(canvasPoint.x() - size.width() / 2.0)), 0, all.width() - size.width());
    const int y = std::clamp(int(std::lround(canvasPoint.y() - size.height() / 2.0)), 0, all.height() - size.height());
    return QRect(QPoint(x, y), size);
}
// Esc, a right press, Ctrl+Z or a lost window during a gesture (D7, W5, W20):
// a move or resize goes back to where it started (the cover stays selected),
// a pan to where it was, a box being drawn is dropped (placing stays armed).
bool FrameView::cancelGesture() {
    if (drag_ == Drag::None) return false;
    for (auto& mask : masks_) if ((drag_ == Drag::Move || drag_ == Drag::Resize) && mask.id == dragId_) mask.rect = original_;
    if (drag_ == Drag::Pan) { pan_ = panFrom_; clampPan(); }
    drag_ = Drag::None;
    moved_ = false;
    unsetCursor();
    if (!armed_.isEmpty()) setCursor(placeWaiting_ ? Qt::BusyCursor : Qt::CrossCursor);
    endLive();
    announce();
    update();
    return true;
}
void FrameView::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape && drag_ != Drag::None) { cancelGesture(); return; }
    // The first Esc during a nudge takes the nudge back; the next one gives the
    // arrows back to the frames (W8); then it deselects.
    if (e->key() == Qt::Key_Escape && nudging_) { cancelNudge(); return; }
    auto mask = std::find_if(masks_.begin(), masks_.end(), [&](const RecordingMask& m) { return m.id == selected_; });
    // Esc always gives held arrows back, cover selected or not (no arrow trap at 100 %).
    if (e->key() == Qt::Key_Escape && arrowsClaimed_ && armed_.isEmpty()) { claimArrows(false); emit arrowsReleased(); return; }
    if (e->key() == Qt::Key_Escape && mask != masks_.end() && armed_.isEmpty()) { selected_.clear(); announce(); emit maskSelected({}); update(); return; }
    QPoint step;
    switch (e->key()) {
    case Qt::Key_Left: step = {-1, 0}; break;
    case Qt::Key_Right: step = {1, 0}; break;
    case Qt::Key_Up: step = {0, -1}; break;
    case Qt::Key_Down: step = {0, 1}; break;
    default: QWidget::keyPressEvent(e); return;
    }
    if ((e->modifiers() & Qt::AltModifier) || !arrowsClaimed_) { QWidget::keyPressEvent(e); return; }
    if (mask == masks_.end() || drag_ != Drag::None) {
        if (actual_ && drag_ == Drag::None) {
            // At 100 % the arrows pan; Shift pans a quarter of the view.
            const qreal dpr = devicePixelRatioF();
            const qreal distance = (e->modifiers() & Qt::ShiftModifier ? area().width() / 4.0 : 40.0) * dpr;
            panBy(QPointF(step) * distance);
            return;
        }
        QWidget::keyPressEvent(e);
        return;
    }
    // The editor's steps: 1 px, 10 px with Ctrl; Shift resizes.
    step *= e->modifiers() & Qt::ControlModifier ? 10 : 1;
    if (!nudging_) nudgeFrom_ = mask->rect;
    nudging_ = true;
    const QRect all = bounds();
    QRect r = mask->rect;
    if (e->modifiers() & Qt::ShiftModifier) {
        // Resize from the bottom-right corner (its handle).
        r = resizedRect(r, 4, step, all);
    } else {
        r.translate(step);
        r.moveLeft(std::clamp(r.left(), 0, std::max(0, all.width() - r.width())));
        r.moveTop(std::clamp(r.top(), 0, std::max(0, all.height() - r.height())));
    }
    mask->rect = r;
    const QString id = mask->id;
    beginLive(id);
    announce();
    nudge_.start();
    update();
}
void FrameView::commitNudge() {
    if (!nudging_) return;
    nudging_ = false;
    nudge_.stop();
    bool changed = false;
    QRect rect;
    QString id;
    for (const auto& mask : std::as_const(masks_)) if (mask.id == selected_ && mask.rect != nudgeFrom_) { changed = true; rect = mask.rect; id = mask.id; }
    // Unchanged, there is no answer to wait for: the frame on screen is current.
    if (!changed) { endLive(); return; }
    emit maskMoved(id, rect);
}
void FrameView::cancelNudge() {
    if (!nudging_) return;
    nudging_ = false;
    nudge_.stop();
    for (auto& mask : masks_) if (mask.id == selected_) mask.rect = nudgeFrom_;
    endLive();
    announce();
    update();
}
void FrameView::mousePressEvent(QMouseEvent* e) {
    // A right press during a gesture cancels it (D7).
    if (e->button() == Qt::RightButton && drag_ != Drag::None) { cancelGesture(); return; }
    if (e->button() != Qt::LeftButton) return;
    // While playing the picture grabs nothing: a click pauses (W3).
    if (playing_) { emit pauseRequested(); return; }
    commitNudge();
    const auto p = e->position();
    moved_ = false;
    panFrom_ = pan_;
    pressAt_ = p;
    if (!armed_.isEmpty()) {
        // Drawing waits for the frame it is drawn on (W6); it may start up to
        // 8 px beside the picture (W22).
        if (placeWaiting_ || !imageRect().adjusted(-8, -8, 8, 8).contains(p.toPoint())) return;
        drag_ = Drag::Create; anchor_ = p; current_ = p; update(); return;
    }
    auto target = targetAt(p);
    // A press on empty picture within 60 ms of the pointer leaving a grip and
    // within 8 px of where it left (each axis), the timeline's rule (Q10t),
    // takes that grip: a press that arrives late (a pointer already moving,
    // emulated middle buttons) still grabs what was under the cursor (Q23,
    // Q24). A press on a cover or another grip keeps what it hit, and the drag
    // counts from the real press point, so a press that never moves changes
    // nothing (r5-neon #6).
    if (target.kind == Target::Kind::None && gripAt_ && gripLeft_.isValid() && gripLeft_.elapsed() <= 60
        && std::abs(p.x() - gripAt_->x()) <= 8 && std::abs(p.y() - gripAt_->y()) <= 8) {
        if (const auto grip = targetAt(*gripAt_); grip.kind == Target::Kind::Handle) target = grip;
    }
    gripAt_.reset();
    gripLeft_.invalidate();
    if (target.kind != Target::Kind::None) {
        const auto it = std::find_if(masks_.cbegin(), masks_.cend(), [&](const RecordingMask& m) { return m.id == target.id; });
        if (it == masks_.cend()) return;
        // Taken before the selection is announced: the window answers it with setMasks.
        original_ = it->rect;
        dragId_ = target.id;
        handle_ = target.handle;
        pressedSelected_ = selected_ == target.id;
        drag_ = target.kind == Target::Kind::Handle ? Drag::Resize : Drag::Move;
        anchor_ = p;
        setCursor(drag_ == Drag::Resize ? resizeCursor(handle_) : Qt::SizeAllCursor);
        // A click on a cover gives the viewer the arrows (W8).
        claimArrows(true);
        if (!pressedSelected_) { selected_ = target.id; announce(); emit maskSelected(selected_); }
        update();
        return;
    }
    // At 100 % a drag pans; a click there deselects (D8).
    if (actual_) { drag_ = Drag::Pan; anchor_ = p; setCursor(Qt::ClosedHandCursor); return; }
    // A click on empty picture gives the arrows back to the frames.
    claimArrows(false);
    if (!selected_.isEmpty()) {
        selected_.clear();
        announce();
        emit maskSelected({});
        update();
    }
}
void FrameView::leaveEvent(QEvent* e) {
    if (!hovered_.isEmpty()) { hovered_.clear(); emit hoveredCoverChanged({}); }
    hoveredHandle_ = -1;
    if (drag_ == Drag::None) unsetCursor();
    if (!armed_.isEmpty()) setCursor(Qt::CrossCursor);
    update();
    QWidget::leaveEvent(e);
}
// The gesture at pointer p: the drawn area, the pan, or the cover moved or
// resized by the pointer's delta from the press. Release applies it at the
// release point too (W5).
void FrameView::applyDrag(const QPointF& p) {
    if (drag_ == Drag::Create) { current_ = p; update(); return; }
    if (drag_ == Drag::Pan) { pan_ = panFrom_ - (p - anchor_) * devicePixelRatioF(); clampPan(); update(); return; }
    const QPointF delta = toCanvas(p) - toCanvas(anchor_);
    auto mask = std::find_if(masks_.begin(), masks_.end(), [&](const RecordingMask& m) { return m.id == dragId_; });
    if (mask == masks_.end()) return;
    const QRect all = bounds();
    if (drag_ == Drag::Move) { QRect moved = original_.translated(delta.toPoint()); moved.moveLeft(std::clamp(moved.left(), 0, std::max(0, all.width() - moved.width()))); moved.moveTop(std::clamp(moved.top(), 0, std::max(0, all.height() - moved.height()))); mask->rect = moved; }
    else mask->rect = resizedRect(original_, handle_, delta.toPoint(), all);
    announce();
    update();
}
// Nothing moves until the pointer passes the dead zone: 4 px, or 8 px when the
// press landed on a cover that wasn't selected (its first press selects, W2).
bool FrameView::pastDeadZone(const QPointF& p) const {
    const qreal zone = drag_ == Drag::Move && !pressedSelected_ ? 8 : drag_ == Drag::Create ? 3 : 4;
    // Travel counts from the real press, even when a late press took a grip
    // the pointer had just left: a click that doesn't move never edits.
    return std::max(std::abs(p.x() - pressAt_.x()), std::abs(p.y() - pressAt_.y())) >= zone;
}
void FrameView::mouseMoveEvent(QMouseEvent* e) {
    const auto p = e->position();
    updateHover(p);
    if (drag_ == Drag::None) return;
    if (!moved_ && !pastDeadZone(p)) return;
    const bool started = !moved_;
    moved_ = true;
    applyDrag(p);
    if (started && (drag_ == Drag::Move || drag_ == Drag::Resize)) beginLive(dragId_);
}
void FrameView::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || drag_ == Drag::None) return;
    const auto p = e->position();
    const bool previewed = moved_;  // a drawn area was shown (Q14)
    const bool moved = moved_ || pastDeadZone(p);
    moved_ = false;
    if (moved) applyDrag(p);
    if (drag_ == Drag::Pan) {
        drag_ = Drag::None;
        unsetCursor();
        // A click (no pan) deselects and gives the arrows back to the frames.
        if (!moved) claimArrows(false);
        if (!moved && !selected_.isEmpty()) { selected_.clear(); announce(); emit maskSelected({}); update(); }
        updateHover(p);
        return;
    }
    if (drag_ == Drag::Create) {
        const auto kind = armed_;
        drag_ = Drag::None;
        update();
        // Exactly what the preview showed (F5).
        Q_UNUSED(previewed);
        if (const QRect r = placementAt(p); !r.isEmpty()) emit maskCreated(kind, r);
        return;
    }
    if (!moved) {
        // A click: nothing changed.
        for (auto& mask : masks_) if (mask.id == dragId_) mask.rect = original_;
    }
    drag_ = Drag::None;
    const QString id = dragId_;
    QRect rect;
    bool changed = false;
    for (const auto& mask : std::as_const(masks_)) if (mask.id == id && mask.rect != original_) { changed = true; rect = mask.rect; }
    // Unchanged (a click, or back where it started): the frame on screen is current.
    if (!changed) endLive();
    updateHover(p);
    if (changed) emit maskMoved(id, rect);
}

// ---- ReviewWindow ----

QString ReviewWindow::audioName() const {
    if (options_.microphoneRecorded) return options_.systemRecorded ? QStringLiteral("System audio and microphone") : QStringLiteral("Microphone");
    return QStringLiteral("System audio");
}
void ReviewWindow::applyAudio() {
    const bool keep = effectiveAudio(config_, options_.audioRecorded, audioIntent_);
    config_.audio.system = keep && (options_.systemRecorded || !options_.microphoneRecorded);
    config_.audio.microphone = keep && options_.microphoneRecorded;
}
// What the export carries for sound, as the footer's format button says it.
QString ReviewWindow::audioSummary() const {
    if (config_.silent()) return QStringLiteral("no sound");
    if (!options_.audioRecorded) return QStringLiteral("no audio recorded");
    return effectiveAudio(config_, options_.audioRecorded, audioIntent_) ? QStringLiteral("with ") + audioName().toLower() : QStringLiteral("without audio");
}

ReviewWindow::ReviewWindow(const Options& options, const RecordingConfig& config, QWidget* parent) : QWidget(parent, Qt::Window), options_(options), config_(config), frameTimer_(this) {
    audioIntent_ = config_.audio.any();
    applyAudio();
    setObjectName("putgd-review");
    setWindowTitle("Review recording");
    // The app's painted controls (#198: in the interface's scheme).
    Fonts::load();
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) Ui::apply(*app);
    setAttribute(Qt::WA_DeleteOnClose, false);
    kept_ = recording::KeptSpans(edit_, options_.durationUs);
    auto* frame = new QVBoxLayout(this);
    frame->setSizeConstraint(QLayout::SetNoConstraint);
    frame->setContentsMargins(0, 0, 0, 0);
    frame->setSpacing(0);

    // Title: "Review recording" · "Not uploaded".
    header_ = Ui::WindowHeader::install(this, frame, nullptr, Ui::WindowHeader::Density::Compact);
    header_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    header_->setAccessibleName("Review recording");
    if (auto* brand = header_->findChild<Ui::ElidedLabel*>("brandName")) brand->setText(QStringLiteral("Review recording"));
    auto* title = new QWidget(this);
    title->setObjectName("reviewTitle");
    auto* titleRow = new QHBoxLayout(title);
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->setSpacing(reviewSpace(Ui::Space::m));
    auto* name = new QLabel(QStringLiteral("Review recording"), title);
    name->setObjectName("reviewTitleName");
    name->setFont(Ui::font(Ui::Type::body, true));
    auto* draft = new QLabel(QStringLiteral("Not uploaded"), title);
    draft->setObjectName("draftState");
    draft->setToolTip("Nothing uploads until you choose Upload; Save a copy… exports locally.");
    Ui::role(draft, "meta");
    titleRow->addWidget(name);
    titleRow->addWidget(draft);
    titleRow->addStretch(1);
    if (Ui::WindowHeader::customFrame()) header_->setLeading(title);
    else {
        // The desktop's frame names the window; the state still shows.
        title->setFixedHeight(std::max(40, title->fontMetrics().height() + 16));
        titleRow->setContentsMargins(reviewSpace(Ui::Space::l), 0, reviewSpace(Ui::Space::l), 0);
        frame->addWidget(title);
    }

    // Tool row: the image editor's tools and keys.
    toolRow_ = new Ui::Surface("bg", Qt::BottomEdge, this);
    toolRow_->setObjectName("reviewTools");
    toolRow_->setFixedHeight(toolRowHeight(this));
    auto* tools = new QHBoxLayout(toolRow_);
    tools->setContentsMargins(reviewSpace(Ui::Space::m), 0, reviewSpace(Ui::Space::m), 0);
    tools->setSpacing(reviewSpace(Ui::Space::xs));
    auto tool = [this](const QString& text, const char* glyph, const QString& name, const QString& tip, QWidget* parent) {
        auto* button = Ui::toolButton(text, QString::fromLatin1(glyph), "quiet", parent);
        button->setAccessibleName(name);
        button->setToolTip(tip);
        button->setIconSize(QSize(Ui::scaled(Ui::Metric::icon), Ui::scaled(Ui::Metric::icon)));
        button->setFixedHeight(std::max(Ui::scaled(Ui::Metric::iconTarget), textControlHeight(button) + 4));
        if (text.isEmpty()) button->setFixedWidth(button->height());
        // A click never takes the keys (Space, arrows, [ ]) from the timeline (E2).
        button->setFocusPolicy(Qt::TabFocus);
        Ui::focusTooltips(button);
        return button;
    };
    select_ = tool({}, "select", "Select", "Select (V)", toolRow_);
    select_->setObjectName("toolSelect");
    select_->setCheckable(true);
    cut_ = tool("Cut", "cut", "Cut", "Cut the selected time (C)", toolRow_);
    cut_->setObjectName("toolCut");
    redact_ = tool("Redact", "redact", "Redact", "Redact part of the picture for the selected time, or the whole recording (X)", toolRow_);
    redact_->setObjectName("toolRedact");
    redact_->setCheckable(true);
    blur_ = tool("Blur", "blur", "Blur", "Blur part of the picture for the selected time, or the whole recording (B). Blur may be reversible.", toolRow_);
    blur_->setObjectName("toolBlur");
    blur_->setCheckable(true);
    undo_ = tool({}, "undo", "Undo", "Undo (Ctrl+Z)", toolRow_);
    undo_->setObjectName("toolUndo");
    redo_ = tool({}, "redo", "Redo", "Redo (Ctrl+Shift+Z)", toolRow_);
    redo_->setObjectName("toolRedo");
    pictureZoom_ = tool("Fit · 100%", nullptr, "Picture zoom", "Zoom the picture", toolRow_);
    pictureZoom_->setObjectName("pictureZoom");
    pictureZoom_->setPopupMode(QToolButton::InstantPopup);
    auto* zoomMenu = new QMenu(pictureZoom_);
    zoomMenu->setObjectName("pictureZoomMenu");
    auto* fit = zoomMenu->addAction(Ui::icon("zoom-fit"), QStringLiteral("Fit"));
    auto* actual = zoomMenu->addAction(Ui::icon("zoom-actual"), QStringLiteral("100%"));
    pictureZoom_->setMenu(zoomMenu);
    editsToggle_ = tool("Edits", "panel-right", "Edits", "Show the edits panel", toolRow_);
    editsToggle_->setObjectName("editsToggle");
    editsToggle_->setCheckable(true);
    editsToggle_->hide();
    tools->addWidget(select_);
    auto separator = [this, tools] {
        auto* line = Ui::rule(toolRow_, Qt::Vertical);
        line->setFixedHeight(Ui::scaled(20));
        tools->addSpacing(reviewSpace(Ui::Space::xs));
        tools->addWidget(line, 0, Qt::AlignVCenter);
        tools->addSpacing(reviewSpace(Ui::Space::xs));
    };
    separator();
    tools->addWidget(cut_);
    tools->addWidget(redact_);
    tools->addWidget(blur_);
    separator();
    tools->addWidget(undo_);
    tools->addWidget(redo_);
    // The row's free middle holds the placement prompt, the blur warning and a
    // frame error (D9): showing one never moves the picture. It takes only the
    // space left over, so it never widens the window.
    toolMessages_ = new QWidget(toolRow_);
    toolMessages_->setObjectName("toolMessages");
    toolMessages_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* messages = new QHBoxLayout(toolMessages_);
    messages->setContentsMargins(reviewSpace(Ui::Space::m), 0, reviewSpace(Ui::Space::m), 0);
    messages->setSpacing(reviewSpace(Ui::Space::s));
    messages->addStretch(1);
    tools->addWidget(toolMessages_, 1);
    tools->addWidget(pictureZoom_);
    tools->addWidget(editsToggle_);
    frame->addWidget(toolRow_);

    // Below the tool row: the viewer column and the right panel.
    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    frame->addLayout(body, 1);
    auto* column = new QVBoxLayout;
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    body->addLayout(column, 1);
    // The notice and the status lines lie over the viewer's top edge and never
    // refit the picture (D9); the placement prompt, the blur warning and a
    // frame error are in the tool row.
    auto* bannerArea = new Ui::Surface("well", {}, this);
    bannerArea_ = bannerArea;
    bannerArea->setObjectName("viewerBanners");
    auto* banners = new QVBoxLayout(bannerArea);
    banners->setContentsMargins(reviewSpace(Ui::Space::m), reviewSpace(Ui::Space::xs), reviewSpace(Ui::Space::m), 0);
    banners->setSpacing(reviewSpace(Ui::Space::xs));
    // Why the capture ended when it stopped itself (#225); hidden otherwise.
    notice_ = new QLabel(this);
    notice_->setObjectName("notice");
    Ui::role(notice_, "notice");
    notice_->setWordWrap(true);
    notice_->setTextFormat(Qt::PlainText);
    notice_->setAccessibleName("Why the recording stopped");
    auto* noticeBand = new Ui::Banner(Ui::Banner::Tone::Info, {}, bannerArea);
    noticeBand->setContentWidget(notice_);
    noticeBand->hide();
    noticeBand_ = noticeBand;
    banners->addWidget(noticeBand);
    auto* messageRow = static_cast<QHBoxLayout*>(toolMessages_->layout());
    // Short notes: "Loading frame…", the arrows' owner, what an undo undid (W6, W8, W10).
    toolNote_ = new ViewerBanner(ViewerBanner::Tone::Info, toolMessages_);
    toolNote_->setObjectName("toolNote");
    messageRow->insertWidget(messageRow->count() - 1, toolNote_);
    noteTimer_.setSingleShot(true);
    connect(&noteTimer_, &QTimer::timeout, this, [this] { transientNote_.clear(); refreshNote(); });
    placeBanner_ = new ViewerBanner(ViewerBanner::Tone::Info, toolMessages_);
    placeBanner_->setObjectName("placementBanner");
    auto* cancelPlace = placeBanner_->setAction(QStringLiteral("Cancel"), QStringLiteral("Esc"));
    cancelPlace->setObjectName("cancelPlacement");
    cancelPlace->setToolTip(QStringLiteral("Cancel placing the cover (Esc)"));
    connect(cancelPlace, &QPushButton::clicked, this, &ReviewWindow::cancelPlacement);
    messageRow->insertWidget(messageRow->count() - 1, placeBanner_);
    blurBanner_ = new ViewerBanner(ViewerBanner::Tone::Warning, toolMessages_);
    blurBanner_->setObjectName("blurWarning");
    blurBanner_->setText(QStringLiteral("Blur may be reversible. Use Redact for private information."));
    messageRow->insertWidget(messageRow->count() - 1, blurBanner_);
    frameError_ = new ViewerBanner(ViewerBanner::Tone::Error, toolMessages_);
    frameError_->setObjectName("previewError");
    auto* retryPreview = frameError_->setAction(QStringLiteral("Retry preview"), {});
    retryPreview->setObjectName("retryPreview");
    retryPreview->setToolTip(QStringLiteral("Decode the frame at the playhead again"));
    connect(retryPreview, &QPushButton::clicked, this, [this] { drive({{"action", "retryPreview"}}); });
    messageRow->insertWidget(messageRow->count() - 1, frameError_);
    // Errors, results and format advice with their actions; shown only when there are any.
    statusRow_ = new QWidget(bannerArea);
    statusRow_->setObjectName("statusRow");
    statusLines_ = new QVBoxLayout(statusRow_);
    statusLines_->setContentsMargins(0, 0, 0, 0);
    statusLines_->setSpacing(reviewSpace(Ui::Space::xs));
    statusRow_->hide();
    banners->addWidget(statusRow_);
    bannerArea->hide();
    view_ = new FrameView(this, maskNames_);
    view_->setObjectName("frame");
    view_->setContentsMargins(reviewSpace(Ui::Space::m), reviewSpace(Ui::Space::xs), reviewSpace(Ui::Space::m), reviewSpace(Ui::Space::xs));
    view_->setCanvas(options.canvas);
    view_->setAccessibleName("Recording frame");
    column->addWidget(view_, 1);

    // Transport: Play (speeds in its menu), frame steps, output time, silence, timeline zoom.
    transport_ = new Ui::Surface("bg", Qt::TopEdge, this);
    transport_->setObjectName("reviewTransport");
    transport_->setFixedHeight(transportHeight(this));
    auto* transport = new QHBoxLayout(transport_);
    transport->setContentsMargins(reviewSpace(Ui::Space::m), 0, reviewSpace(Ui::Space::m), 0);
    transport->setSpacing(reviewSpace(Ui::Space::xs));
    play_ = tool({}, "play", "Play", "Play the kept footage silently (Space)", transport_);
    play_->setObjectName("play");
    // Play and its speeds are two buttons side by side, so each answers over
    // all of its visible half (W13: a split button gave its centre to the menu).
    playSpeed_ = tool({}, "chevron-down", "Playback speed", "Playback speed: 0.5× to 4× (L plays faster)", transport_);
    playSpeed_->setObjectName("playSpeed");
    playSpeed_->setIconSize(QSize(Ui::scaled(12), Ui::scaled(12)));
    // The button always names its speed and keeps one width, wide enough for
    // "0.5×" and the chevron, so the transport never shifts (r5-neon #11).
    playSpeed_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    playSpeed_->setText(QStringLiteral("0.5×"));
    playSpeed_->setFixedWidth(std::max(playSpeed_->sizeHint().width(), playSpeed_->fontMetrics().horizontalAdvance(QStringLiteral("0.5×")) + Ui::scaled(12) + Ui::scaled(18)));
    playSpeed_->setText(QStringLiteral("1×"));
    speedMenu_ = new QMenu(playSpeed_);
    speedMenu_->setObjectName("speedMenu");
    speeds_ = new QActionGroup(speedMenu_);
    for (const auto& [label, factor] : std::initializer_list<std::pair<const char*, double>>{{"0.5×", 0.5}, {"1×", 1.0}, {"2×", 2.0}, {"4×", 4.0}}) {
        auto* action = speedMenu_->addAction(QString::fromUtf8(label));
        action->setCheckable(true);
        action->setData(factor);
        action->setChecked(factor == 1.0);
        speeds_->addAction(action);
    }
    // Opened by its own click, so the style draws no second arrow beside the glyph.
    connect(playSpeed_, &QToolButton::clicked, this, [this] { speedMenu_->popup(playSpeed_->mapToGlobal(QPoint(0, playSpeed_->height()))); });
    previousFrame_ = tool({}, "frame-back", "Previous frame", "Previous frame (,)", transport_);
    previousFrame_->setObjectName("previousFrame");
    nextFrame_ = tool({}, "frame-forward", "Next frame", "Next frame (.)", transport_);
    nextFrame_->setObjectName("nextFrame");
    time_ = new QLabel(transport_);
    time_->setObjectName("time");
    Ui::role(time_, "secondary");
    Ui::setMono(time_, Ui::Type::technical);
    time_->setAccessibleName("Clip time");
    clipTime_ = new QLabel(QStringLiteral("clip time"), transport_);
    clipTime_->setObjectName("clipTimeCaption");
    Ui::role(clipTime_, "meta");
    silent_ = nullptr;
    auto* silent = new SilentPreview(transport_);
    skimToggle_ = tool("Skim", nullptr, "Skim", "Show the frame under the pointer as it rests on the timeline (S)", transport_);
    skimToggle_->setObjectName("skimToggle");
    skimToggle_->setCheckable(true);
    snapToggle_ = tool("Snap", nullptr, "Snap", "Snap edges to the playhead, trim ends, cuts, covers and the selection (N); hold Ctrl while dragging to place freely", transport_);
    snapToggle_->setObjectName("snapToggle");
    snapToggle_->setCheckable(true);
    timelineZoom_ = tool("Fit", "zoom", "Zoom the timeline", "Zoom the timeline: Fit, or a closer level (= and -, Shift+Z fits)", transport_);
    timelineZoom_->setObjectName("timelineZoom");
    timelineZoom_->setPopupMode(QToolButton::InstantPopup);
    timelineZoomMenu_ = new QMenu(timelineZoom_);
    timelineZoomMenu_->setObjectName("timelineZoomMenu");
    timelineZoom_->setMenu(timelineZoomMenu_);
    transport->addWidget(play_);
    transport->addWidget(playSpeed_);
    transport->addSpacing(reviewSpace(Ui::Space::xs));
    transport->addWidget(previousFrame_);
    transport->addWidget(nextFrame_);
    transport->addSpacing(reviewSpace(Ui::Space::s));
    transport->addWidget(time_);
    transport->addSpacing(reviewSpace(Ui::Space::xs));
    transport->addWidget(clipTime_);
    transport->addStretch(1);
    transport->addWidget(silent);
    transport->addSpacing(reviewSpace(Ui::Space::m));
    transport->addWidget(skimToggle_);
    transport->addWidget(snapToggle_);
    transport->addWidget(timelineZoom_);
    column->addWidget(transport_);

    timeline_ = new FilmstripTimeline(maskNames_, this);
    timeline_->setDuration(options_.durationUs, config_.fps);
    column->addWidget(timeline_);

    // The right panel, 280 px, scrolling when its list is long; folded into
    // the Edits overlay below ~1180×720.
    auto* panelHost = new QScrollArea(this);
    panelHost->setObjectName("reviewPanelHost");
    panelHost->setFrameShape(QFrame::NoFrame);
    panelHost->setWidgetResizable(true);
    panelHost->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    panelHost->setFixedWidth(280);
    Ui::transparentView(panelHost);
    auto* panelBox = new QWidget(panelHost);
    auto* panelLayout = new QVBoxLayout(panelBox);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(0);
    panel_ = new ReviewPanel(maskNames_, panelBox);
    panelLayout->addWidget(panel_, 1);
    panelHost->setWidget(panelBox);
    body->addWidget(panelHost);
    overlay_ = new EditsOverlay(this);

    // Footer: format and sound … Discard… · Save a copy… · destination · Upload.
    footer_ = new Ui::Surface("bg", Qt::TopEdge, this);
    footer_->setObjectName("reviewFooter");
    footer_->setFixedHeight(footerHeight(this));
    auto* footerRow = new QHBoxLayout(footer_);
    footerRow->setContentsMargins(reviewSpace(Ui::Space::l), 0, reviewSpace(Ui::Space::l), 0);
    footerRow->setSpacing(0);
    auto* actions = new QWidget(footer_);
    actions->setObjectName("reviewActions");
    auto* actionRow = new QHBoxLayout(actions);
    actionRow->setContentsMargins(0, 0, 0, 0);
    actionRow->setSpacing(reviewSpace(Ui::Space::s));
    footerRow->addWidget(actions, 1, Qt::AlignVCenter);
    format_ = Ui::toolButton(QStringLiteral("MP4"), {}, "quiet", actions);
    format_->setObjectName("format");
    format_->setAccessibleName("Output format");
    format_->setPopupMode(QToolButton::InstantPopup);
    formatMenu_ = new QMenu(format_);
    formatMenu_->setObjectName("formatMenu");
    formatMenu_->setToolTipsVisible(true);
    formats_ = new QActionGroup(formatMenu_);
    for (const auto& format : options.formats) {
        auto codecs = options.codecs[format].toArray();
        if (codecs.isEmpty()) codecs.append(QString());
        for (const auto& codec : codecs) {
            auto* action = formatMenu_->addAction(formatLabel(format, codec.toString()));
            action->setCheckable(true);
            action->setData(QStringList{format, codec.toString()});
            action->setToolTip(formatHint(format, codec.toString()));
            formats_->addAction(action);
        }
    }
    formatMenu_->addSeparator();
    includeAudio_ = formatMenu_->addAction(QStringLiteral("Include %1").arg(audioName().toLower()));
    includeAudio_->setObjectName("includeAudio");
    includeAudio_->setCheckable(true);
    format_->setMenu(formatMenu_);
    cancel_ = new QPushButton("Discard…", actions);
    cancel_->setObjectName("discard");
    cancel_->setToolTip("Discard this recording and its edits (asks first)");
    Ui::role(cancel_, "quiet");
    save_ = new QPushButton("Save a copy…", actions);
    save_->setObjectName("saveCopy");
    save_->setIcon(Ui::icon("save"));
    save_->setToolTip("Export the reviewed clip to a file on this computer. Nothing is uploaded.");
    destinationMenu_ = new DestinationMenu(actions);
    destinationMenu_->setObjectName("uploadDestinationMenu");
    destinationMenu_->setAutoRaise(true);
    destinationMenu_->setPopupMode(QToolButton::InstantPopup);
    destinationMenu_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    destinationMenu_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* details = new QMenu(destinationMenu_);
    auto* detailsAction = new QWidgetAction(details);
    destinationFull_ = new QLabel(details);
    destinationFull_->setObjectName("uploadDestinationWrapped");
    destinationFull_->setTextFormat(Qt::PlainText);
    destinationFull_->setWordWrap(true);
    destinationFull_->setMargin(reviewSpace(Ui::Space::m));
    destinationFull_->setMaximumWidth(480);
    destinationFull_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    destinationFull_->setFocusPolicy(Qt::StrongFocus);
    Ui::role(destinationFull_, "meta");
    detailsAction->setDefaultWidget(destinationFull_);
    details->addAction(detailsAction);
    destinationMenu_->setMenu(details);
    upload_ = new QPushButton("Upload", actions);
    upload_->setObjectName("upload");
    upload_->setIcon(Ui::icon("upload"));
    upload_->setDefault(true);
    Ui::role(upload_, "primary");
    upload_->setToolTip("Upload the reviewed clip with its edits (Ctrl+Enter). The preview is silent; video exports keep the recorded audio.");
    actionRow->addWidget(format_);
    actionRow->addStretch(1);
    actionRow->addWidget(cancel_);
    actionRow->addSpacing(12 - actionRow->spacing());
    actionRow->addWidget(save_);
    actionRow->addWidget(destinationMenu_);
    actionRow->addWidget(upload_);
    for (auto* button : std::initializer_list<QWidget*>{format_, cancel_, save_, destinationMenu_, upload_}) {
        // A click never takes the keys; Tab still reaches them (Q16).
        button->setFocusPolicy(Qt::TabFocus);
        button->setProperty("uiDensity", "compact");
        button->setFixedHeight(textControlHeight(button) + 4);
    }
    frame->addWidget(footer_);
    errorDetails_ = new QPushButton("Details…", this);
    errorDetails_->setObjectName("reviewErrorDetails");
    errorDetails_->setProperty("uiDensity", "compact");
    errorDetails_->setProperty("driveAction", QJsonObject{{"action", "showAdvice"}});
    Ui::role(errorDetails_, "quiet");
    errorDetails_->hide();
    connect(errorDetails_, &QPushButton::clicked, this, [this] { drive({{"action", "showAdvice"}}); });

    // Thumbnails, skim frames and playhead frames: one worker each, never waited on.
    filmstrip_ = new Filmstrip(this);
    skimmer_ = new FrameSkimmer(this);
    frames_ = new FrameSkimmer(this);
    if (!options_.workingClip.isEmpty()) {
        filmstrip_->open(options_.workingClip, options_.durationUs, options_.canvas);
        skimmer_->open(options_.workingClip, options_.canvas);
        frames_->open(options_.workingClip, options_.canvas);
    }
    QPointer<Filmstrip> strip(filmstrip_);
    timeline_->setTileSource([strip](qint64 at) { return strip ? strip->tile(at) : QImage(); });
    connect(filmstrip_, &Filmstrip::tileReady, timeline_, &FilmstripTimeline::tilesChanged);
    tileTimer_.setSingleShot(true);
    tileTimer_.setInterval(0);
    connect(&tileTimer_, &QTimer::timeout, this, &ReviewWindow::requestTiles);
    connect(timeline_, &FilmstripTimeline::visibleSpanChanged, &tileTimer_, qOverload<>(&QTimer::start));
    connect(skimmer_, &FrameSkimmer::frameReady, this, [this](qint64 requested, qint64 frameUs, const QImage& frame, quint64 revision, bool exact) {
        // The frame without a cover being moved goes only under that cover's live effect.
        if (revision == uncoveredRevision) {
            if (!exact) return;
            if (uncoveredAsked_ && uncoveredAsked_->second == requested) view_->setUncovered(uncoveredAsked_->first, frame, requested);
            uncoveredAsked_.reset();
            return;
        }
        // R16: one skim request in flight. Its answer (covers applied, as every
        // skim frame) shows when it is near the pointer's time, exact or not;
        // then the newest time is asked for at once.
        if (exact && skimAsked_ && skimAsked_->first == requested && skimAsked_->second == revision) skimAsked_.reset();
        if (exact && revision == edit_.revision) cacheSkim({requested, frameUs, revision, viewEdge(), frame});
        if (!skimUs_ || playing_ || !timeline_->skimming() || revision != edit_.revision) return;
        // Q11: the exact answer for the time skimmed is that time's frame,
        // whatever its stored time (Matroska rounding, the clip's last frame).
        if (exact && requested == *skimUs_) {
            view_->setSkim(frame, *skimUs_);
            timeline_->setSkimExact(true);
            return;
        }
        if (const auto* hit = cachedSkim(*skimUs_)) {
            view_->setSkim(hit->image, *skimUs_);
            timeline_->setSkimExact(true);
            return;
        }
        // Q12: a nearby frame only when no cover starts or ends between it and the skim time.
        const qint64 lo = std::min(frameUs, *skimUs_), hi = std::max(frameUs, *skimUs_);
        const bool coverEdgeBetween = std::any_of(edit_.masks.cbegin(), edit_.masks.cend(), [&](const RecordingMask& m) {
            return (m.start > lo && m.start <= hi) || (m.end > lo && m.end <= hi);
        });
        if (std::abs(frameUs - *skimUs_) <= skimReach() && !coverEdgeBetween) {
            view_->setSkim(frame, frameUs);
            timeline_->setSkimExact(false);
        }
        if (!skimAsked_ || skimAsked_->first != *skimUs_) askSkimFrame(*skimUs_);
    });
    connect(skimmer_, &FrameSkimmer::frameFailed, this, [this] { skimAsked_.reset(); });
    connect(frames_, &FrameSkimmer::frameReady, this, [this](qint64 requested, qint64 frameUs, const QImage& frame, quint64 revision, bool exact) {
        // Playback draws its own frames; an answer for an older edit is followed by a newer one.
        if (playing_ || revision != edit_.revision) return;
        // A frame on the way to a newer request (a fast scrub): shown at its own
        // time, never taken as the frame for the playhead.
        if (!exact) { if (!view_->dragging()) view_->setFrame(frame, frameUs); return; }
        view_->setFrame(frame, requested);
        shownRevision_ = revision;
        if (requested == playhead_) {
            shownFor_ = playhead_;
            shownActual_ = frameUs;
            // The frame a placement is drawn on is shown (W6).
            if (view_->placeWaiting() && !edgePreview_) stopPlaceWait();
        }
        // The frame for the edit a cover's move made: its effect is in the picture now.
        if (liveWaitRevision_ && revision >= *liveWaitRevision_ && !view_->dragging() && !view_->nudging()) { liveWaitRevision_.reset(); view_->endLive(); }
    });
    connect(frames_, &FrameSkimmer::frameFailed, this, [this](qint64 requested, const QString& message) {
        if (playing_ || !frameAsked_ || frameAsked_->at != requested) return;
        // An Enter waiting for this frame expires (W6).
        enterPlacePending_ = false;
        enterPlaceTimer_.stop();
        // A placement keeps waiting: the error (with Retry preview) says why (R18).
        showFrameError(requested, message);
    });

    // Timeline gestures.
    // A press on the ruler or the strip seeks at once; playing, it keeps playing
    // from there (C3). A scrub or an edge drag pauses for its duration. A scrub
    // keeps the selection (A14); only selectionCleared clears it.
    connect(timeline_, &FilmstripTimeline::scrubStarted, this, [this] { stopPlayback(); });
    connect(timeline_, &FilmstripTimeline::seekRequested, this, [this](qint64 at) {
        const bool resume = playing_;
        seek(at);
        if (resume) startPlayback(playhead_, true);
    });
    connect(timeline_, &FilmstripTimeline::selectionCleared, this, [this] { if (timeSelection_ || !selectedKind_.isEmpty()) clearSelection(); });
    // R18t: a plain click on the filmstrip clears a time selection only; a
    // selected cut or cover stays selected (Set to playhead, Shift+I / O).
    connect(timeline_, &FilmstripTimeline::timeSelectionCleared, this, [this] {
        if (!timeSelection_) return;
        timeSelection_.reset();
        timeline_->setTimeSelection(std::nullopt);
        // A seek click doesn't change what a placement covers (P13).
        refreshAll();
    });
    connect(timeline_, &FilmstripTimeline::timeSelected, this, &ReviewWindow::selectTime);
    connect(timeline_, &FilmstripTimeline::cutSelected, this, [this](const QString& id) { selectCut(id, false); });
    // A cover bar click seeks only when the playhead is outside the cover (A5).
    // A press on a cover's bar or edge selects it and leaves the playhead where
    // it is (r5-neon #2); seeking on select stays for the Edits list and keys.
    connect(timeline_, &FilmstripTimeline::coverSelected, this, [this](const QString& id) { selectCover(id, false); });
    // T6: a cancelled cut or cover press gives back the trim selection, as its Edits row selects it.
    connect(timeline_, &FilmstripTimeline::trimSelected, this, [this] { selectEdit(QStringLiteral("trim"), {}); });
    connect(timeline_, &FilmstripTimeline::moreCoversRequested, this, &ReviewWindow::revealHiddenCovers);
    connect(timeline_, &FilmstripTimeline::hoverChanged, this, [this](const QString& kind, const QString& id) {
        view_->setLinkedHover(kind == "cover" ? id : QString());
        panel_->setLinkedHover(kind, id);
    });
    // Any edge drag (a trim bracket, a cut or cover edge, a time selection's
    // moving end): the viewer shows the frame the edge lands on, at once.
    // W4: a trim, a time selection or a cover edge shows the frame at the
    // edge (a start edge its first frame, an end edge its last); a cut edge
    // shows the kept side; a cover is previewed with its proposed span.
    connect(timeline_, &FilmstripTimeline::trimEdgeDragged, this, [this](bool start, qint64 at) {
        stopPlayback();
        beginEdgePreview();
        edgePreview_ = start ? at : previousFrame(at);
        requestFrame();
    });
    connect(timeline_, &FilmstripTimeline::editEdgeDragged, this, [this](const QString& kind, const QString& id, bool start, qint64 at) {
        stopPlayback();
        beginEdgePreview();
        if (kind == "cut") edgePreview_ = start ? previousFrame(at) : at;
        else edgePreview_ = start ? at : previousFrame(at);
        edgeEdit_.reset();
        if (const auto* mask = kind == "cover" ? maskById(id) : nullptr) {
            // The proposed span: the frame shown is covered as it will be.
            RecordingEdit proposed = edit_;
            for (auto& m : proposed.masks) if (m.id == id) {
                if (start) { m.start = at; m.end = std::max(mask->end, nextFrameEdge(at)); }
                else { m.end = at; m.start = std::min(mask->start, *edgePreview_); }
            }
            edgeEdit_ = proposed;
        }
        requestFrame();
    });
    // On release the edge frame stays until the proposal is answered, then the
    // viewer switches once (W4); a drag that proposed nothing switches at once.
    connect(timeline_, &FilmstripTimeline::edgeDragFinished, this, [this] {
        QMetaObject::invokeMethod(this, [this] {
            if (!edgePreview_) return;
            if (proposalSeq_ > edgeDragSeq_) { edgeHoldProposal_ = proposalSeq_; edgeHoldTimer_.start(); return; }
            endEdgePreview();
        }, Qt::QueuedConnection);
    });
    edgeHoldTimer_.setSingleShot(true);
    edgeHoldTimer_.setInterval(1500);
    connect(&edgeHoldTimer_, &QTimer::timeout, this, [this] { if (edgeHoldProposal_) endEdgePreview(); });
    connect(timeline_, &FilmstripTimeline::trimProposed, this, &ReviewWindow::proposeTrim);
    connect(timeline_, &FilmstripTimeline::cutAdjusted, this, &ReviewWindow::adjustCut);
    connect(timeline_, &FilmstripTimeline::coverRetimed, this, &ReviewWindow::retimeCover);
    // Skimming (B2, B4): the viewer keeps its picture until the exact frame
    // (covers applied) arrives; nothing changes on hover with skimming off.
    connect(timeline_, &FilmstripTimeline::skimMoved, this, [this](qint64 at) {
        if (playing_ || !timeline_->skimming()) return;
        skimUs_ = at;
        skimMovedClock_.restart();
        skimReturn_.stop();
        if (const auto* hit = cachedSkim(at)) { view_->setSkim(hit->image, at); timeline_->setSkimExact(true); return; }
        timeline_->setSkimExact(false);
        // R16: the newest time is queued at once; the decode in flight is not
        // dropped but hands over the frame it has reached (shown when near).
        askSkimFrame(at);
    });
    connect(timeline_, &FilmstripTimeline::skimRested, this, [this](qint64 at) {
        if (playing_ || !timeline_->skimming() || !skimUs_ || *skimUs_ != at || cachedSkim(at) || (skimAsked_ && skimAsked_->first == at)) return;
        askSkimFrame(at);
    });
    // P10: a skim that ends while the pointer is still on the timeline (it
    // rests on a grip, or pauses) keeps its picture until a press or until
    // the pointer leaves the timeline: no flash back to the playhead frame.
    // F2: a skim that ends without a press returns to the playhead frame after
    // 250 ms with a 100 ms crossfade; a skim back within 250 ms carries on.
    skimReturn_.setSingleShot(true);
    skimReturn_.setInterval(250);
    connect(&skimReturn_, &QTimer::timeout, this, [this] { if (!skimUs_) view_->fadeOutSkim(100); });
    connect(timeline_, &FilmstripTimeline::skimLeft, this, [this] {
        skimUs_.reset();
        skimAsked_.reset();
        skimmer_->cancel();
        if (view_->skimming()) skimReturn_.start();
    });
    connect(timeline_, &FilmstripTimeline::skimmingChanged, this, [this](bool on) {
        sessionSkimming = on;
        skimToggle_->setChecked(on);
        if (!on) { skimUs_.reset(); skimAsked_.reset(); skimmer_->cancel(); view_->clearSkim(); }
    });
    connect(timeline_, &FilmstripTimeline::snappingChanged, this, [this](bool on) { sessionSnapping = on; snapToggle_->setChecked(on); });
    connect(timeline_, &FilmstripTimeline::zoomChanged, this, [this] { refreshTransport(); });
    timeline_->setSkimming(sessionSkimming);
    timeline_->setSnapping(sessionSnapping);
    skimToggle_->setChecked(timeline_->skimming());
    connect(skimToggle_, &QToolButton::toggled, this, [this](bool on) { timeline_->setSkimming(on); });
    snapToggle_->setChecked(timeline_->snapping());
    connect(snapToggle_, &QToolButton::toggled, this, [this](bool on) { timeline_->setSnapping(on); });
    // T15: a press on the strip while playing pauses for the press; a click
    // plays on from where it landed, a drag stays paused.
    connect(timeline_, &FilmstripTimeline::playbackHeld, this, [this] { heldPlaying_ = heldPlaying_ || playing_; stopPlayback(); });
    connect(timeline_, &FilmstripTimeline::playbackReleased, this, [this](bool resume) {
        const bool wasPlaying = heldPlaying_;
        heldPlaying_ = false;
        if (resume && wasPlaying && !playing_) startPlayback(playhead_, true);
    });

    // Panel.
    connect(panel_, &ReviewPanel::cutRequested, this, &ReviewWindow::cutSelection);
    // The panel's Redact / Blur toggle placement as the tool row's do (R28).
    connect(panel_, &ReviewPanel::coverRequested, this, [this](const QString& kind) { if (placing_ && placing_->kind == kind) cancelPlacement(); else arm(kind); });
    connect(panel_, &ReviewPanel::restoreCut, this, &ReviewWindow::restoreCut);
    // F6: a cut's typed times go through the same adjust as dragging its edges.
    connect(panel_, &ReviewPanel::cutAdjustRequested, this, [this](const QString& id, qint64 start, qint64 end) { stopPlayback(); adjustCut(id, start, end); });
    connect(panel_, &ReviewPanel::cutEdgeToPlayhead, this, [this](const QString& id, bool start) {
        if (const auto why = cutEdgeToPlayhead(id, start); !why.isEmpty()) refuseChange(why);
    });
    connect(panel_, &ReviewPanel::playAcross, this, &ReviewWindow::playAcross);
    connect(panel_, &ReviewPanel::seekCoverEdge, this, &ReviewWindow::seekCoverEdge);
    connect(panel_, &ReviewPanel::coverEdgeToPlayhead, this, [this](const QString& id, bool start) { stopPlayback(); setMaskAt(start, id); });
    connect(panel_, &ReviewPanel::coverEdgeEdited, this, [this](const QString& id, bool start, qint64 at) {
        updateCover(id, [&](QJsonObject& mask) { mask[start ? "start" : "end"] = double(at); });
    });
    connect(panel_, &ReviewPanel::coverKindChanged, this, &ReviewWindow::setCoverKind);
    // R29: the picture shows a strength at once; the edit follows the 350 ms coalescing.
    connect(panel_, &ReviewPanel::coverStrengthPreview, this, [this](const QString& id, int strength) {
        if (strength < 0) view_->endLive(); else view_->previewStrength(id, strength);
    });
    connect(panel_, &ReviewPanel::coverStrengthChanged, this, [this](const QString& id, int strength) {
        const quint64 before = proposalSeq_;
        updateCover(id, [&](QJsonObject& mask) { mask["strength"] = strength; });
        if (proposalSeq_ != before && view_->liveId() == id) { liveProposal_ = proposalSeq_; liveWaitRevision_.reset(); }
    });
    connect(panel_, &ReviewPanel::editActivated, this, [this](const QString& kind, const QString& id) { selectEdit(kind, id); });
    connect(panel_, &ReviewPanel::rowHovered, this, [this](const QString& kind, const QString& id) {
        timeline_->setLinkedHover(kind == "trim" ? QString() : kind, kind == "trim" ? QString() : id);
        view_->setLinkedHover(kind == "cover" ? id : QString());
    });
    // W17: Enter in a panel field (or Esc on unchanged text) hands the keys back.
    connect(panel_, &ReviewPanel::focusReturnRequested, this, [this] {
        if (overlay_->isOpen()) overlay_->setFocus(Qt::OtherFocusReason);
        else timeline_->setFocus(Qt::OtherFocusReason);
    });
    // W11: the folded overlay ends 8 px above the transport.
    overlay_->setFloor(transport_);
    // Q17: an outside press on the picture only closes the overlay; elsewhere it closes it and acts.
    overlay_->setConsumeArea(view_);
    // Closed by a pointer press, the keys go back to the timeline (P14).
    connect(overlay_, &EditsOverlay::dismissedByPointer, this, [this] { timeline_->setFocus(Qt::OtherFocusReason); });
    connect(panel_, &ReviewPanel::resetTrim, this, [this] { propose(QJsonArray{QJsonObject{{"op", "resetTrim"}}}); });
    connect(panel_, &ReviewPanel::removeCover, this, [this](const QString& id) { propose(QJsonArray{QJsonObject{{"op", "removeMask"}, {"maskId", id}}}); });
    connect(overlay_, &EditsOverlay::closed, this, [this] { editsToggle_->setChecked(false); view_->setRightInset(0); });
    connect(editsToggle_, &QToolButton::clicked, this, [this] { setPanelOpen(!overlay_->isOpen()); });

    // Viewer.
    connect(view_, &FrameView::previewErrorChanged, this, [this] {
        frameError_->setText(view_->previewErrorText());
        refreshNote();
    });
    connect(view_, &FrameView::maskCreated, this, [this](const QString& kind, const QRect& r) { placeCover(kind, r); });
    connect(view_, &FrameView::maskMoved, this, [this](const QString& id, const QRect& r) {
        const quint64 before = proposalSeq_;
        updateCover(id, [&](QJsonObject& mask) { mask["rect"] = QJsonArray{r.x(), r.y(), r.width(), r.height()}; });
        // The live cover holds until the frame for this edit is shown.
        if (proposalSeq_ != before && view_->liveId() == id) { liveProposal_ = proposalSeq_; liveWaitRevision_.reset(); }
        else view_->endLive();
    });
    // A cover shown at this frame started to move: the frame without it, from
    // the skim worker (no skim runs during a viewer gesture). Its answer goes
    // only to the live cover, never to the skim or its cache.
    connect(view_, &FrameView::liveEditStarted, this, [this](const QString& id, qint64 at) {
        skimUs_.reset();
        skimAsked_.reset();
        skimmer_->cancel();
        view_->clearSkim();
        timeline_->skimAt(std::nullopt);
        liveWaitRevision_.reset();
        liveProposal_ = 0;
        askUncovered(id, at);
    });
    connect(view_, &FrameView::maskSelected, this, [this](const QString& id) { if (id.isEmpty()) clearSelection(); else selectCover(id, false); });
    // W3: a click on the picture while playing pauses, nothing else.
    connect(view_, &FrameView::pauseRequested, this, &ReviewWindow::stopPlayback);
    placeHintTimer_.setSingleShot(true);
    placeHintTimer_.setInterval(2000);
    connect(&placeHintTimer_, &QTimer::timeout, this, [this] { placeHint_.clear(); refreshNote(); });
    // Q22: hovering a cover's box, its bar or its Edits row highlights the other two.
    connect(view_, &FrameView::hoveredCoverChanged, this, [this](const QString& id) {
        timeline_->setLinkedHover(id.isEmpty() ? QString() : QStringLiteral("cover"), id);
        panel_->setLinkedHover(id.isEmpty() ? QString() : QStringLiteral("cover"), id);
    });
    // W8: the tool row says when the arrows move the box; Esc gives them back to the frames.
    connect(view_, &FrameView::arrowsClaimedChanged, this, &ReviewWindow::refreshNote);
    connect(view_, &FrameView::arrowsReleased, this, [this] { timeline_->setFocus(Qt::OtherFocusReason); refreshNote(); });
    connect(view_, &FrameView::zoomChanged, this, &ReviewWindow::refreshTools);
    // A nudge is a new edit: an upload waiting for an answer is called off.

    // Tools and transport.
    connect(select_, &QToolButton::clicked, this, [this] { cancelPlacement(); refreshTools(); });
    connect(cut_, &QToolButton::clicked, this, &ReviewWindow::cutSelection);
    connect(redact_, &QToolButton::clicked, this, [this] { if (placing_ && placing_->kind == "solid") cancelPlacement(); else arm("solid"); });
    connect(blur_, &QToolButton::clicked, this, [this] { if (placing_ && placing_->kind == "blur") cancelPlacement(); else arm("blur"); });
    connect(undo_, &QToolButton::clicked, this, [this] { undoRedo(true); });
    connect(redo_, &QToolButton::clicked, this, [this] { undoRedo(false); });
    connect(fit, &QAction::triggered, this, [this] { view_->setActualSize(false); });
    connect(actual, &QAction::triggered, this, [this] { view_->setActualSize(true); });
    // 100 % shows source pixels: the frame (and a resting skim) is decoded again at that size.
    connect(view_, &FrameView::zoomChanged, this, [this] {
        const int edge = viewEdge();
        if (edge == shownEdge_) return;
        shownEdge_ = edge;
        // Playback restarts at the new size; a skim frame asked at the old size is dropped.
        if (playing_) { const qint64 from = playhead_; stopPlayback(); startPlayback(from, true); return; }
        requestFrame();
        skimCache_.clear();
        if (skimUs_) {
            skimmer_->cancel();
            skimAsked_.reset();
            timeline_->setSkimExact(false);
            askSkimFrame(*skimUs_);
        }
    });
    connect(play_, &QToolButton::clicked, this, [this] { if (playing_) stopPlayback(); else startPlayback(); });
    connect(speeds_, &QActionGroup::triggered, this, [this](QAction* action) {
        speed_ = action->data().toDouble();
        playSpeed_->setText(action->text());
        if (playing_) { const qint64 from = playhead_; stopPlayback(); startPlayback(from, true); }
    });
    connect(previousFrame_, &QToolButton::clicked, this, [this] { stepFrames(-1); });
    connect(nextFrame_, &QToolButton::clicked, this, [this] { stepFrames(1); });
    // The timeline zoom menu: Fit and every level of the ladder (A13).
    // One group for the menu's life; clear() deletes the actions, which leave it.
    auto* zoomGroup = new QActionGroup(timelineZoomMenu_);
    connect(timelineZoomMenu_, &QMenu::aboutToShow, this, [this, group = zoomGroup] {
        timelineZoomMenu_->clear();
        for (const int level : timeline_->zoomLevels()) {
            auto* action = timelineZoomMenu_->addAction(level <= 1 ? QStringLiteral("Fit") : QStringLiteral("%1×").arg(level));
            action->setCheckable(true);
            action->setChecked(level == timeline_->zoom());
            group->addAction(action);
            connect(action, &QAction::triggered, this, [this, level] { timeline_->setZoom(level); });
        }
    });
    connect(formats_, &QActionGroup::triggered, this, [this](QAction* action) {
        const auto value = action->data().toStringList();
        if (value.size() != 2 || (value[0] == config_.format && value[1] == config_.codec)) return;
        config_.format = value[0];
        config_.codec = value[1];
        applyAudio();
        emitConfig();
    });
    connect(includeAudio_, &QAction::toggled, this, [this](bool on) {
        if (on == audioIntent_ || config_.silent()) return;
        audioIntent_ = on;
        applyAudio();
        emitConfig();
    });
    // Upload hands the clip to the resident: the review closes at once and the
    // export and upload continue in the background, reported like any upload.
    connect(upload_, &QPushButton::clicked, this, &ReviewWindow::upload);
    chooser_ = new FileDialog(this);
    connect(save_, &QPushButton::clicked, this, [this] { stopPlayback(); chooseCopy(); });
    connect(cancel_, &QPushButton::clicked, this, &ReviewWindow::askDiscard);

    // Keys with modifiers; the plain keys arrive in keyPressEvent so a focused
    // field keeps its own typing, arrows and Delete.
    auto shortcut = [this](const QKeySequence& key, auto slot) { auto* s = new QShortcut(key, this); s->setContext(Qt::WindowShortcut); connect(s, &QShortcut::activated, this, slot); return s; };
    // During a drag (timeline or picture) these cancel only that drag (W20).
    // During the blur strength's coalescing it takes back only that step (Q18).
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Z), [this] { if (!cancelDrags() && !panel_->cancelPendingStrength()) undoRedo(true); });
    shortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), [this] { if (!cancelDrags()) undoRedo(false); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Y), [this] { if (!cancelDrags()) undoRedo(false); });
    // Ctrl+Enter uploads as the button does; never with a menu open.
    // A held key never repeats into an upload.
    for (const auto key : {Qt::Key_Return, Qt::Key_Enter}) shortcut(QKeySequence(Qt::CTRL | key), [this] { if (!QApplication::activePopupWidget()) upload(); })->setAutoRepeat(false);
    // An upload waiting for its edit's answer gives up after 2 s and says so.
    uploadWait_.setSingleShot(true);
    uploadWait_.setInterval(2000);
    connect(&uploadWait_, &QTimer::timeout, this, [this] { cancelUpload(QStringLiteral("your last change wasn't confirmed in time")); });
    saveWait_.setSingleShot(true);
    saveWait_.setInterval(2000);
    connect(&saveWait_, &QTimer::timeout, this, [this] { cancelSaveCopy(QStringLiteral("your last change wasn't confirmed in time")); });
    // Space plays and pauses from any focus but a text field or spin box (they
    // claim it in ShortcutOverride); a clicked button never takes it (E2).
    // A row or button reached with Tab (its focus ring shown) takes Space as its
    // own activation (W20).
    shortcut(QKeySequence(Qt::Key_Space), [this] {
        if (exporting_) return;
        auto* focus = focusWidget();
        if (focus && isAncestorOf(focus) && Ui::keyboardFocus(focus)) {
            if (auto* button = qobject_cast<QAbstractButton*>(focus)) { if (button->isEnabled()) button->click(); return; }
            if (focus->objectName() == "reviewPanelRow") { QKeyEvent key(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" ")); QApplication::sendEvent(focus, &key); return; }
        }
        if (playing_) stopPlayback();
        else { adoptSkim(); startPlayback(); }  // Q10: play from the skimmed frame
    })->setAutoRepeat(false);
    shortcut(QKeySequence(Qt::ALT | Qt::Key_Left), [this] { stepEdit(-1); });
    shortcut(QKeySequence(Qt::ALT | Qt::Key_Right), [this] { stepEdit(1); });
    view_->installEventFilter(this);
    timeline_->installEventFilter(this);
    // P9: presses anywhere in the window are seen first (they never stop here).
    qApp->installEventFilter(this);
    bannerArea_->installEventFilter(this);

    enterPlaceTimer_.setSingleShot(true);
    enterPlaceTimer_.setInterval(1000);
    // Its second is up with the frame still loading (the banner says so) or
    // failed (the error says so): the Enter expires and nothing is placed on a
    // frame it wasn't meant for (W6).
    connect(&enterPlaceTimer_, &QTimer::timeout, this, [this] {
        if (enterPlacePending_ && placing_ && !view_->placeWaiting()) { placePendingEnter(); return; }
        enterPlacePending_ = false;
    });
    placeWaitTimer_.setSingleShot(true);
    placeWaitTimer_.setInterval(100);
    connect(&placeWaitTimer_, &QTimer::timeout, this, [this] { if (view_->placeWaiting()) { loadingFrame_ = true; refreshNote(); } });
    frameTimer_.setSingleShot(true);
    frameTimer_.setInterval(std::max(0, qEnvironmentVariableIntValue("PUTGD_RECORD_REVIEW_DELAY_MS")));
    connect(&frameTimer_, &QTimer::timeout, this, &ReviewWindow::askFrame);
    setConfig(config);
    setEdit({});
    timeline_->setFocus();
    resize(defaultSize().boundedTo(availableReviewSize()));
    refreshFold();
}
ReviewWindow::~ReviewWindow() {
    // Children torn down after this body (focus leaving the viewer, a row) must
    // not reach the window's handlers once its members are gone.
    for (auto* child : findChildren<QObject*>()) child->disconnect(this);
    if (playStop_) playStop_->store(true);
    if (playThread_) playThread_->wait();
}
QSize ReviewWindow::defaultSize() { return {1440, 900}; }
QSize ReviewWindow::availableReviewSize() const {
    if (!options_.availableSize.isEmpty()) return options_.availableSize;
    const auto* display = screen() ? screen() : QGuiApplication::primaryScreen();
    return display ? display->availableGeometry().size() - QSize(24, 48) : QSize(1342, 720);
}
QSize ReviewWindow::minimumSizeHint() const {
    if (!footer_ || !layout()) return QWidget::minimumSizeHint();
    // Folded, the panel is the overlay: the viewer column and the footer set the width.
    const int width = std::max({640, footer_->minimumSizeHint().width(), toolRow_->minimumSizeHint().width(), transport_->minimumSizeHint().width()});
    int height = toolRow_->height() + transport_->height() + timeline_->minimumSizeHint().height() + footer_->height() + 180;
    if (header_->isVisibleTo(this)) height += header_->sizeHint().height();
    return {width, std::max(480, height)};
}

// ---- time ----
qint64 ReviewWindow::frameUs() const { return std::max<qint64>(1, qRound64(1e6 / std::max(1, config_.fps))); }
qint64 ReviewWindow::frameAt(qint64 index) const { return std::llround(double(index) * 1e6 / std::max(1, config_.fps)); }
// Edges come from frame indexes (edge k = round(k·10⁶/fps)), never from adding
// a rounded frame length: 33,333 + 33,333 is not frame 2's 66,667.
qint64 ReviewWindow::frameIndex(qint64 us) const { return std::llround(double(us) * std::max(1, config_.fps) / 1e6); }
qint64 ReviewWindow::nextFrameEdge(qint64 at) const { return std::min(options_.durationUs, frameAt(frameIndex(at) + 1)); }
// The edge after the frame the viewer shows for the playhead, from that
// frame's own time, never the nominal playhead frame (#1635 U44, V1): at 30 fps
// a playhead at 40 ms shows the frame at 66,667 µs. A stored time is rounded to
// its nearest grid frame (Matroska keeps 33,333 as 33,000, and the exporter puts
// that frame on 33,333). A seek not yet shown is decoded now; with no frame
// shown for the playhead there is no end (the caller refuses).
std::optional<qint64> ReviewWindow::shownFrameEnd() {
    const auto resolved = [this] { return shownFor_ == playhead_ && shownActual_ >= 0 && !edgePreview_; };
    if (!resolved()) decodeFrameNow();
    if (!resolved()) return std::nullopt;
    return std::min(options_.durationUs, frameAt(frameIndex(shownActual_) + 1));
}
qint64 ReviewWindow::previousFrame(qint64 edge) const { return frameAt(std::max<qint64>(0, frameIndex(edge) - 1)); }
// Frame times are rounded (30 fps: 66,667 µs is frame 2), so the estimate is
// checked against the rounded times on both sides of it.
std::optional<qint64> ReviewWindow::firstFrameIn(const recording::Span& span) const {
    const int fps = std::max(1, config_.fps);
    qint64 k = std::max<qint64>(0, qint64(std::floor(double(span.start) * fps / 1e6)) - 1);
    while (frameAt(k) < span.start) ++k;
    const qint64 t = frameAt(k);
    return t < span.end ? std::optional<qint64>(t) : std::nullopt;
}
std::optional<qint64> ReviewWindow::lastFrameIn(const recording::Span& span) const {
    const int fps = std::max(1, config_.fps);
    qint64 k = qint64(std::ceil(double(span.end) * fps / 1e6)) + 1;
    while (k >= 0 && frameAt(k) >= span.end) --k;
    if (k < 0) return std::nullopt;
    const qint64 t = frameAt(k);
    return t >= span.start ? std::optional<qint64>(t) : std::nullopt;
}
std::optional<qint64> ReviewWindow::firstKeptFrame(qint64 start, qint64 end) const {
    for (const auto& part : kept_.keptParts(start, end)) if (const auto t = firstFrameIn(part)) return t;
    return std::nullopt;
}
std::optional<qint64> ReviewWindow::lastKeptFrame(qint64 start, qint64 end) const {
    const auto parts = kept_.keptParts(start, end);
    for (auto it = parts.crbegin(); it != parts.crend(); ++it) if (const auto t = lastFrameIn(*it)) return t;
    return std::nullopt;
}
void ReviewWindow::seek(qint64 us, bool user) {
    if (user) { stopPlayback(); pendingCut_.reset(); }
    playhead_ = std::clamp<qint64>(us, 0, std::max<qint64>(0, options_.durationUs - 1));
    timeline_->setPlayhead(playhead_);
    // Q10: seeking to the skimmed time (a click where the pointer rests, a key
    // that took the skim) keeps the skim picture as the playhead's: no flash.
    if (view_->skimming() && std::abs(view_->skimTime() - playhead_) <= frameUs()) {
        const qint64 skimmed = view_->skimTime();
        view_->promoteSkim();
        if (const auto* hit = cachedSkim(skimmed); hit && skimmed == playhead_) { shownFor_ = playhead_; shownActual_ = hit->frameUs; frameAsked_ = FrameAsk{playhead_, edit_.revision, viewEdge(), {}}; shownRevision_ = edit_.revision; }
    }
    if (skimUs_) { skimUs_.reset(); skimmer_->cancel(); }
    skimAsked_.reset();
    timeline_->setSkimExact(false);
    timeline_->skimAt(std::nullopt);  // the same time skimmed again asks again
    view_->clearSkim();
    refreshTransport();
    requestFrame();
    // Q15: a seek while placing waits again for the frame at the new time.
    if (placing_ && shownFor_ != playhead_ && frameTimer_.interval() == 0) { view_->setPlaceWaiting(true); placeWaitTimer_.start(); }
}
// Q10: a skim on the timeline wins for keys: the playhead goes to the
// frame-snapped skim time first (keeping its picture), then the key acts.
bool ReviewWindow::adoptSkim() {
    if (!skimUs_ || playing_ || exporting_) return false;
    // P11: only a skim that moved within the last 1.5 s; a stale one is dropped
    // and the key acts at the playhead.
    if (!skimMovedClock_.isValid() || skimMovedClock_.elapsed() > 1500) {
        skimUs_.reset();
        skimAsked_.reset();
        skimmer_->cancel();
        timeline_->skimAt(std::nullopt);
        view_->clearSkim();
        return false;
    }
    seek(std::min(frameAt(frameIndex(*skimUs_)), std::max<qint64>(0, options_.durationUs - 1)));
    return true;
}
void ReviewWindow::stepFrames(int frames) {
    const int fps = std::max(1, config_.fps);
    const qint64 index = std::llround(double(playhead_) * fps / 1e6) + frames;
    seek(std::max<qint64>(0, frameAt(std::max<qint64>(0, index))));
}
void ReviewWindow::stepTime(qint64 us) { seek(std::clamp<qint64>(playhead_ + us, 0, std::max<qint64>(0, options_.durationUs - 1))); }

// ---- selection ----
const RecordingMask* ReviewWindow::maskById(const QString& id) const {
    const auto it = std::find_if(edit_.masks.cbegin(), edit_.masks.cend(), [&](const RecordingMask& m) { return m.id == id; });
    return it == edit_.masks.cend() ? nullptr : &*it;
}
RecordingMask* ReviewWindow::selectedMask() {
    if (selectedKind_ != "cover") return nullptr;
    auto it = std::find_if(edit_.masks.begin(), edit_.masks.end(), [&](const RecordingMask& m) { return m.id == selectedId_; });
    return it == edit_.masks.end() ? nullptr : &*it;
}
const RecordingMask* ReviewWindow::selectedMask() const { return const_cast<ReviewWindow*>(this)->selectedMask(); }
void ReviewWindow::setTimeSelection(std::optional<recording::Span> span) {
    if (span && span->empty()) span.reset();
    timeSelection_ = span;
    selectedKind_.clear();
    selectedId_.clear();
    timeline_->setTimeSelection(span);
    view_->setMasks(edit_.masks, {});
    // A placement follows the shown selection (R28).
    if (placing_) placing_->span = placementSpan(placing_->kind, placing_->whole);
    refreshAll();
}
// The time a cover being placed covers: the time selection snapped outward to
// whole frames, or the whole recording.
recording::Span ReviewWindow::placementSpan(const QString&, bool& whole) const {
    whole = !timeSelection_;
    if (!timeSelection_) return {0, options_.durationUs};
    const int fps = std::max(1, config_.fps);
    const qint64 start = frameAt(qint64(std::floor(double(timeSelection_->start) * fps / 1e6 + 1e-9)));
    const qint64 end = std::min(options_.durationUs, frameAt(qint64(std::ceil(double(timeSelection_->end) * fps / 1e6 - 1e-9))));
    recording::Span span{std::min(start, timeSelection_->start), std::max(end, std::min(options_.durationUs, timeSelection_->end))};
    if (span.empty()) span.end = nextFrameEdge(span.start);
    return span;
}
// A drag on the filmstrip (or recording.review selectRange): the half-open span.
void ReviewWindow::selectTime(qint64 start, qint64 end) {
    stopPlayback();
    setTimeSelection(recording::Span{start, end});
}
void ReviewWindow::clearSelection() {
    timeSelection_.reset();
    selectedKind_.clear();
    selectedId_.clear();
    timeline_->setTimeSelection(std::nullopt);
    timeline_->setSelectedEdit({}, {});
    view_->setMasks(edit_.masks, {});
    // A placement keeps its time: only a range gesture changes it (P13).
    refreshAll();
}
void ReviewWindow::selectCover(const QString& id, bool seekToIt) {
    const auto* mask = maskById(id);
    if (!mask) { clearSelection(); return; }
    stopPlayback();
    if (placing_) cancelPlacement();
    timeSelection_.reset();
    selectedKind_ = QStringLiteral("cover");
    selectedId_ = id;
    timeline_->setSelectedEdit(selectedKind_, id);
    view_->setMasks(edit_.masks, id);
    revealRow_ = {QStringLiteral("cover"), id};
    // Its first visible frame, only when the playhead is outside the cover (A5,
    // W15); wholly outside kept footage, its start (paused, "Excluded from output").
    if (seekToIt && !(mask->start <= playhead_ && playhead_ < mask->end)) {
        const auto first = firstKeptFrame(mask->start, mask->end);
        seek(first ? *first : firstFrameIn({mask->start, mask->end}).value_or(mask->start));
    }
    refreshAll();
}
void ReviewWindow::selectCut(const QString& id, bool seekToIt) {
    const auto it = std::find_if(edit_.cuts.cbegin(), edit_.cuts.cend(), [&](const RecordingCut& c) { return c.id == id; });
    if (it == edit_.cuts.cend()) { clearSelection(); return; }
    stopPlayback();
    if (placing_) cancelPlacement();
    timeSelection_.reset();
    selectedKind_ = QStringLiteral("cut");
    selectedId_ = id;
    timeline_->setSelectedEdit(selectedKind_, id);
    revealRow_ = {QStringLiteral("cut"), id};
    view_->setMasks(edit_.masks, {});
    // From Edits: the first kept frame after it (the last before a trailing cut).
    if (seekToIt) {
        auto at = firstKeptFrame(it->end, options_.durationUs);
        if (!at) at = lastKeptFrame(0, it->start);
        if (at) seek(*at);
    }
    refreshAll();
}
void ReviewWindow::selectEdit(const QString& kind, const QString& id) {
    if (kind == "cover") { selectCover(id); return; }
    if (kind == "cut") { selectCut(id, true); return; }
    if (kind != "trim") { clearSelection(); return; }
    stopPlayback();
    if (placing_) cancelPlacement();
    timeSelection_.reset();
    selectedKind_ = QStringLiteral("trim");
    selectedId_.clear();
    timeline_->setSelectedEdit(selectedKind_, {});
    view_->setMasks(edit_.masks, {});
    if (const auto first = firstKeptFrame(0, options_.durationUs)) seek(*first);
    refreshAll();
}
// Trim, cuts and covers in source order, as the Edits list shows them.
QList<QPair<QString, QString>> ReviewWindow::editsInOrder() const {
    struct Item { qint64 start; QString kind, id; };
    QList<Item> items;
    if (edit_.trimStart > 0 || (edit_.trimEnd && *edit_.trimEnd < options_.durationUs)) items.append({edit_.trimStart, QStringLiteral("trim"), {}});
    for (const auto& cut : edit_.cuts) items.append({cut.start, QStringLiteral("cut"), cut.id});
    for (const auto& mask : edit_.masks) items.append({mask.start, QStringLiteral("cover"), mask.id});
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.start < b.start; });
    QList<QPair<QString, QString>> order;
    for (const auto& item : items) order.append({item.kind, item.id});
    return order;
}
void ReviewWindow::stepEdit(int direction) {
    const auto order = editsInOrder();
    if (order.isEmpty() || exporting_) return;
    int at = -1;
    for (int i = 0; i < order.size(); ++i) if (order[i].first == selectedKind_ && order[i].second == selectedId_) at = i;
    const int next = at < 0 ? (direction > 0 ? 0 : int(order.size()) - 1) : std::clamp(at + direction, 0, int(order.size()) - 1);
    if (next == at) return;
    selectEdit(order[next].first, order[next].second);
}

// ---- edits ----
void ReviewWindow::propose(const QJsonArray& commands) {
    if (exporting_) return;
    emit editProposed(edit_.revision, commands, ++proposalSeq_);
}
void ReviewWindow::undoRedo(bool undo) {
    if (exporting_) return;
    // Ctrl+Z while placing cancels the placement.
    if (placing_) { if (undo) cancelPlacement(); return; }
    // The first Ctrl+Z during a nudge not yet proposed takes back only the nudge (D6).
    if (view_->nudging()) { view_->cancelNudge(); return; }
    if (undo ? !edit_.canUndo : !edit_.canRedo) return;
    stopPlayback();
    propose(QJsonArray{QJsonObject{{"op", undo ? "undo" : "redo"}}});
    undoProposal_ = proposalSeq_;
    undoWasUndo_ = undo;
    undoBefore_ = edit_;
}
// Trim brackets dragged (and released), or recording.review setTrim.
void ReviewWindow::proposeTrim(qint64 start, qint64 end) {
    pendingTrimStart_ = start != edit_.trimStart;
    propose(QJsonArray{QJsonObject{{"op", "setTrim"}, {"start", double(start)}, {"end", end >= options_.durationUs ? QJsonValue(QJsonValue::Null) : QJsonValue(double(end))}}});
    pendingTrimProposal_ = proposalSeq_;
}
// A selected cut's edge dragged, or recording.review adjustCut.
void ReviewWindow::adjustCut(const QString& id, qint64 start, qint64 end) {
    propose(QJsonArray{QJsonObject{{"op", "updateCut"}, {"cutId", id}, {"start", double(start)}, {"end", double(end)}}});
}
// A selected cover bar's end dragged on the timeline.
void ReviewWindow::retimeCover(const QString& id, qint64 start, qint64 end) {
    updateCover(id, [&](QJsonObject& mask) { mask["start"] = double(start); mask["end"] = double(end); });
}
// The panel's Redact | Blur choice for a selected cover.
void ReviewWindow::setCoverKind(const QString& id, const QString& kind) {
    updateCover(id, [&](QJsonObject& mask) { mask["kind"] = kind; if (kind == "blur" && !mask.contains("strength")) mask["strength"] = 16; });
}
// The Edits toggle: the folded panel as an overlay.
void ReviewWindow::setPanelOpen(bool open) {
    if (open == overlay_->isOpen()) return;
    if (!open) { overlay_->dismiss(); return; }
    overlay_->openFor(panel_, editsToggle_);
    editsToggle_->setChecked(true);
    placeOverlayInset();
}
void ReviewWindow::cutSelection() {
    if (!timeSelection_ || exporting_ || placing_) return;
    stopPlayback();
    const auto span = *timeSelection_;
    propose(QJsonArray{QJsonObject{{"op", "addCut"}, {"start", double(span.start)}, {"end", double(span.end)}}});
    pendingCut_ = span;
    pendingCutProposal_ = proposalSeq_;
    timeSelection_.reset();
    timeline_->setTimeSelection(std::nullopt);
    refreshAll();
}
void ReviewWindow::arm(const QString& kind) {
    if (exporting_ || (kind != "solid" && kind != "blur")) return;
    stopPlayback();
    view_->commitNudge();
    // The time selection snapped outward to whole frames, or the whole recording.
    Placement placement{kind, {0, options_.durationUs}, true};
    placement.span = placementSpan(kind, placement.whole);
    placing_ = placement;
    selectedKind_.clear();
    selectedId_.clear();
    timeline_->setSelectedEdit({}, {});
    if (timeSelection_) timeline_->setTimeSelection(timeSelection_);
    view_->setMasks(edit_.masks, {});
    view_->armCreate(kind);
    // The viewer pauses on the playhead frame inside the selection's kept
    // footage, else the first kept frame in it, else its first source frame.
    const auto& span = placing_->span;
    if (!(span.contains(playhead_) && kept_.kept(playhead_))) {
        const auto first = firstKeptFrame(span.start, span.end);
        seek(first ? *first : firstFrameIn(span).value_or(span.start), false);
    }
    // Drawing waits for the frame at the placement time on every path (W6,
    // R18); past 100 ms the prompt says so. (The test hook that holds frames
    // back, PUTGD_RECORD_REVIEW_DELAY_MS, draws on whatever is shown.)
    if (shownFor_ != playhead_ && frameTimer_.interval() == 0) { view_->setPlaceWaiting(true); placeWaitTimer_.start(); }
    if (overlay_->isOpen()) overlay_->dismiss();
    view_->setFocus(Qt::OtherFocusReason);
    refreshAll();
}
void ReviewWindow::cancelPlacement() {
    if (!placing_) return;
    placing_.reset();
    stopPlaceWait();
    view_->disarm();
    refreshAll();
}
void ReviewWindow::placeCover(const QString& kind, const QRect& rect) {
    if (!placing_ || exporting_) return;
    const auto span = placing_->span;
    QJsonObject mask{{"kind", kind}, {"rect", QJsonArray{rect.x(), rect.y(), rect.width(), rect.height()}}, {"start", double(span.start)}, {"end", double(span.end)}};
    if (kind == "blur") mask["strength"] = 16;
    placing_.reset();
    stopPlaceWait();
    view_->disarm();
    timeSelection_.reset();
    timeline_->setTimeSelection(std::nullopt);
    // One command creates the box and its time; the new cover becomes the selection.
    propose(QJsonArray{QJsonObject{{"op", "addMask"}, {"mask", mask}}});
    // The arrows step frames again (W8).
    timeline_->setFocus(Qt::OtherFocusReason);
    refreshAll();
}
void ReviewWindow::updateCover(const QString& id, const std::function<void(QJsonObject&)>& change) {
    const auto* mask = maskById(id);
    if (!mask) return;
    auto updated = mask->toJson();
    updated.remove("id");
    change(updated);
    if (updated["kind"].toString() == "solid") updated.remove("strength");
    propose(QJsonArray{QJsonObject{{"op", "updateMask"}, {"maskId", id}, {"mask", updated}}});
}
void ReviewWindow::restoreCut(const QString& id) {
    stopPlayback();
    propose(QJsonArray{QJsonObject{{"op", "removeCut"}, {"cutId", id}}});
}
void ReviewWindow::playAcross(const QString& id) {
    const auto it = std::find_if(edit_.cuts.cbegin(), edit_.cuts.cend(), [&](const RecordingCut& c) { return c.id == id; });
    if (it == edit_.cuts.cend() || kept_.empty()) return;
    // The overlay closes first, so nothing plays under it.
    if (overlay_->isOpen()) overlay_->dismiss();
    stopPlayback();
    const qint64 seam = kept_.toOutput(it->start);
    qint64 from = kept_.toSource(std::max<qint64>(0, seam - secondUs));
    if (const auto first = firstKeptFrame(0, options_.durationUs); first && from < *first) from = *first;
    seek(from, false);
    startPlayback(from);
}
// A cut's Set to playhead (the panel's button, recording.review setCutEdge):
// the frame on screen is its first (Starts) or last (Ends) frame, as a
// cover's. The reason when it can't, else empty.
QString ReviewWindow::cutEdgeToPlayhead(const QString& id, bool start) {
    const auto it = std::find_if(edit_.cuts.cbegin(), edit_.cuts.cend(), [&](const RecordingCut& c) { return c.id == id; });
    if (it == edit_.cuts.cend()) return QStringLiteral("No cut has this ID");
    stopPlayback();
    if (start) { adjustCut(id, shownFrameStart(), it->end); return {}; }
    const auto end = shownFrameEnd();
    if (!end) return QStringLiteral("The frame at the playhead isn't shown yet. Try again once it is.");
    adjustCut(id, it->start, *end);
    return {};
}
// Q10, F2: a key or action that acts at a frame takes a live skim's frame
// first; with none, what is shown is the playhead.
void ReviewWindow::takeSkimForKey() {
    if (!adoptSkim() && !skimUs_ && view_->skimming()) { skimReturn_.stop(); view_->clearSkim(); }
}
void ReviewWindow::askUncovered(const QString& id, qint64 at) {
    auto without = edit_;
    without.revision = uncoveredRevision;
    without.masks.erase(std::remove_if(without.masks.begin(), without.masks.end(), [&](const RecordingMask& m) { return m.id == id; }), without.masks.end());
    uncoveredAsked_ = std::pair<QString, qint64>(id, at);
    skimmer_->request(at, without, viewEdge());
}
void ReviewWindow::seekCoverEdge(const QString& id, bool start) {
    const auto* mask = maskById(id);
    if (!mask) return;
    const auto at = start ? firstKeptFrame(mask->start, mask->end) : lastKeptFrame(mask->start, mask->end);
    if (at) seek(*at);
}
// A change refused here was never proposed: a waiting Upload is cancelled so
// it never goes out without it (#1635 V3).
QString ReviewWindow::refuseChange(const QString& message) {
    ++refusals_;
    cancelUpload(QStringLiteral("a change wasn't applied"));
    cancelSaveCopy(QStringLiteral("a change wasn't applied"));
    showError(message, false);
    return message;
}
// The start of the frame on screen for the playhead (P16), as shownFrameEnd
// finds its end; with no frame to read, the playhead itself.
qint64 ReviewWindow::shownFrameStart() {
    const auto resolved = [this] { return shownFor_ == playhead_ && shownActual_ >= 0 && !edgePreview_; };
    if (!resolved() && !options_.workingClip.isEmpty() && QFileInfo::exists(options_.workingClip)) decodeFrameNow();
    if (!resolved()) return playhead_;
    return std::min(frameAt(frameIndex(shownActual_)), std::max<qint64>(0, options_.durationUs - 1));
}
QString ReviewWindow::setTrimAt(bool start) {
    const qint64 at = start ? shownFrameStart() : playhead_;
    const auto refuse = [this](const QString& message) { return refuseChange(message); };
    if (start) {
        if (at >= edit_.exportEnd(options_.durationUs)) return refuse("Trim start must be before the trim end");
        propose(QJsonArray{QJsonObject{{"op", "setTrim"}, {"start", double(at)}, {"end", edit_.trimEnd ? QJsonValue(double(*edit_.trimEnd)) : QJsonValue(QJsonValue::Null)}}});
        return {};
    }
    // The frame shown at the playhead is the last one kept.
    const auto shown = shownFrameEnd();
    if (!shown) return refuse("The frame at the playhead isn't shown yet. Try again once it is.");
    const qint64 end = *shown;
    if (end <= edit_.trimStart) return refuse("Trim end must be after the trim start");
    propose(QJsonArray{QJsonObject{{"op", "setTrim"}, {"start", double(edit_.trimStart)}, {"end", end >= options_.durationUs ? QJsonValue(QJsonValue::Null) : QJsonValue(double(end))}}});
    return {};
}
QString ReviewWindow::setMaskAt(bool start, const QString& id) {
    const auto refuse = [this](const QString& message) { return refuseChange(message); };
    const auto* mask = id.isEmpty() ? selectedMask() : maskById(id);
    if (!mask) return refuse("Select a cover to set its times");
    const qint64 at = start ? shownFrameStart() : playhead_;
    if (start) {
        if (at >= mask->end) return refuse("The cover must start before it ends");
        updateCover(mask->id, [&](QJsonObject& m) { m["start"] = double(at); });
    } else {
        // Until the playhead: the frame shown stays covered.
        const auto shown = shownFrameEnd();
        if (!shown) return refuse("The frame at the playhead isn't shown yet. Try again once it is.");
        const qint64 end = *shown;
        if (end <= mask->start) return refuse("The cover must end after it starts");
        updateCover(mask->id, [&](QJsonObject& m) { m["end"] = double(end); });
    }
    return {};
}
void ReviewWindow::removeSelectedCover() { if (const auto* mask = selectedMask()) propose(QJsonArray{QJsonObject{{"op", "removeMask"}, {"maskId", mask->id}}}); }
// An armed blur becomes an armed redaction; a selected blur becomes a
// redaction in place (one updateMask, one undo step).
void ReviewWindow::useSolidCover() {
    if (placing_ && placing_->kind == "blur") { placing_->kind = QStringLiteral("solid"); view_->armCreate("solid"); refreshAll(); return; }
    const auto* mask = selectedMask();
    if (!mask || mask->kind != "blur") return;
    updateCover(mask->id, [](QJsonObject& m) { m["kind"] = "solid"; });
}

void ReviewWindow::setEdit(const RecordingEdit& edit, const QJsonObject& answer) {
    // A cover the user just placed becomes the selection; a removed one leaves none.
    QStringList before;
    for (const auto& mask : std::as_const(edit_.masks)) before.append(mask.id);
    const bool changed = edit.revision != edit_.revision;
    // W3: the covers other than the live one, before and after.
    const QString live = view_->liveId();
    const auto others = [&live](const RecordingEdit& e) {
        QJsonArray all;
        for (const auto& mask : e.masks) if (mask.id != live) all.append(mask.toJson());
        return all;
    };
    const bool othersChanged = changed && !live.isEmpty() && others(edit) != others(edit_);
    // An accepted edit answers a refusal or an earlier result in the status row.
    if (changed && !exporting_) message_ = {};
    maskNames_->observe(edit.masks);
    edit_ = edit;
    kept_ = recording::KeptSpans(edit_, options_.durationUs);
    // The answer to one of this window's proposals: answered in order, refused
    // ones with the reason (the edit is then the resident's unchanged one).
    const auto answered = quint64(std::max<qint64>(0, answer["proposal"].toInteger()));
    const auto refused = answer["refused"].toObject();
    // Only the answer the upload waits for lets it go on; any other push cancels it.
    // What the upload would export changed: an edit other than the ones it waits for.
    if (changed && !(answer.contains("proposal") && answered <= uploadAwaits_)) cancelUpload();
    if (answered > answeredSeq_) answeredSeq_ = std::min(answered, proposalSeq_);
    if (!refused.isEmpty()) {
        lastRefusal_ = refused;
        lastRefusal_["proposal"] = double(answered);
    }
    // Only the cut's own answer moves the playhead to its seam; its refusal forgets it.
    const bool cutAnswered = pendingCut_ && answered && answered == pendingCutProposal_;
    // A refused cut gives its range back, dashed, so it can be fixed (W9).
    if (cutAnswered && !refused.isEmpty()) {
        if (pendingCut_ && !timeSelection_ && selectedKind_.isEmpty() && !placing_) timeSelection_ = pendingCut_;
        pendingCut_.reset();
    }
    timeline_->setEdit(edit_);
    if (isVisible() || initialFitDone_) for (const auto& mask : std::as_const(edit_.masks)) if (!before.contains(mask.id)) {
        selectedKind_ = QStringLiteral("cover");
        selectedId_ = mask.id;
        timeSelection_.reset();
    }
    if (selectedKind_ == "cover" && !maskById(selectedId_)) selectedKind_.clear(), selectedId_.clear();
    if (selectedKind_ == "cut" && std::none_of(edit_.cuts.cbegin(), edit_.cuts.cend(), [this](const RecordingCut& c) { return c.id == selectedId_; })) selectedKind_.clear(), selectedId_.clear();
    if (selectedKind_ == "trim" && edit_.trimStart == 0 && !edit_.trimEnd) selectedKind_.clear();
    timeline_->setSelectedEdit(selectedKind_, selectedId_);
    if (timeSelection_) timeline_->setTimeSelection(timeSelection_);
    view_->setMasks(edit_.masks, selectedKind_ == "cover" ? selectedId_ : QString());
    // W3: another cover changed under the live one: its base, the frame
    // without it, is asked for again with the other covers as they are now.
    if (othersChanged && view_->liveId() == live) {
        view_->dropUncovered();
        askUncovered(live, view_->frameTime());
    }
    // Opens paused on the first kept frame. Later accepted edits keep the
    // user's playhead, except right after making a cut: 1 s before its seam.
    if (!isVisible() && !initialFitDone_) {
        playhead_ = firstKeptFrame(0, options_.durationUs).value_or(edit_.trimStart);
        timeline_->setPlayhead(playhead_);
    } else if (cutAnswered && pendingCut_ && !kept_.empty()) {
        const qint64 seam = kept_.toOutput(pendingCut_->start);
        qint64 at = kept_.toSource(std::max<qint64>(0, seam - secondUs));
        if (const auto first = firstKeptFrame(0, options_.durationUs); first && at < *first) at = *first;
        pendingCut_.reset();
        seek(at, false);
    }
    // A cover's move answered: accepted, the live effect holds until the frame
    // for this revision is shown; refused, the frame on screen is current.
    if (liveProposal_ && answered >= liveProposal_) {
        liveProposal_ = 0;
        const bool gesture = view_->dragging() || view_->nudging();
        if (!refused.isEmpty() || !changed) { if (!gesture) view_->endLive(); }
        else liveWaitRevision_ = edit_.revision;
    }
    // W10: an undo or redo says what it took back or put back, and selects a restored item.
    if (undoProposal_ && answered == undoProposal_) {
        undoProposal_ = 0;
        if (refused.isEmpty() && changed) noteUndo();
    }
    // C4: a trim that leaves the playhead outside the kept footage brings it to
    // the edge just set, the frame the drag showed.
    if (pendingTrimProposal_ && answered == pendingTrimProposal_) {
        pendingTrimProposal_ = 0;
        if (refused.isEmpty() && !kept_.kept(playhead_)) {
            const auto at = pendingTrimStart_ ? firstKeptFrame(edit_.trimStart, options_.durationUs) : lastKeptFrame(0, edit_.exportEnd(options_.durationUs));
            if (at) seek(*at, false);
        }
    }
    // W4: the edge frame held since release gives way once, now: to the
    // playhead frame at this revision, or, with the playhead now excluded,
    // the playhead moves to the edge frame (the picture doesn't change).
    if (edgeHoldProposal_ && answered >= edgeHoldProposal_ && edgePreview_) {
        const qint64 edge = *edgePreview_;
        if (refused.isEmpty() && !kept_.kept(playhead_) && kept_.kept(edge)) seek(edge, false);
        endEdgePreview();
    }
    if (changed) {
        skimCache_.clear();
        // Playback and the exact skim frame follow the current edit: playback
        // starts again from the playhead, a skim frame is asked for again (the
        // viewer keeps its picture until it arrives). With no kept footage
        // ahead of the playhead it pauses there, never starting over.
        if (playing_) { const qint64 from = playhead_; stopPlayback(); startPlayback(from, true); }
        if (skimUs_ && !playing_) {
            skimmer_->cancel();
            timeline_->setSkimExact(false);
            askSkimFrame(*skimUs_);
        }
    }
    if (!refused.isEmpty() && !exporting_) {
        // Never silent: the status says what was not applied, and a waiting upload is cancelled.
        auto why = refused["message"].toString().trimmed();
        while (why.endsWith('.')) why.chop(1);
        showError(why.isEmpty() ? QStringLiteral("That change wasn't applied.") : QStringLiteral("That change wasn't applied: %1.").arg(why), false);
    }
    if (!refused.isEmpty()) {
        ++refusals_;
        cancelUpload(QStringLiteral("a change wasn't applied"));
        cancelSaveCopy(QStringLiteral("a change wasn't applied"));
        panel_->revertFields();
    }
    trySaveCopy();
    if (tryUpload()) return;
    refreshAll();
    requestFrame();
}
bool ReviewWindow::setConfig(const RecordingConfig& config, quint64 seq) {
    // The window's own output choice wins over a config sent before the resident saw it.
    if (seq < configSeq_) return false;
    auto shown = config_.toJson(), next = config.toJson();
    shown.remove("budgetBytes"); next.remove("budgetBytes");
    if (shown == next) return true;
    cancelUpload();
    if (!config.silent()) audioIntent_ = config.audio.any();
    config_ = config;
    applyAudio();
    timeline_->setDuration(options_.durationUs, config_.fps);
    refreshAll();
    return true;
}
void ReviewWindow::setExporting(bool exporting, const QString& label) {
    stopPlayback();
    if (exporting) { cancelPlacement(); chooser_->cancel(); if (adviceDialog_) { adviceDialog_->close(); adviceDialog_.clear(); } if (overlay_->isOpen()) overlay_->dismiss(); }
    if (exporting && discard_) answerDiscard(false);
    exporting_ = exporting;
    for (auto* w : std::initializer_list<QWidget*>{toolRow_, transport_, timeline_, view_, panel_, upload_, save_, cancel_, format_, destinationMenu_}) w->setEnabled(!exporting);
    // Progress while it runs, then the result (a failure arrives through showError).
    message_ = label.isEmpty() ? StatusEntry{} : StatusEntry{label, exporting ? QStringLiteral("info") : QStringLiteral("success"), {}};
    if (!exporting && !label.isEmpty()) message_.actions = {{QStringLiteral("Dismiss"), QJsonObject{{"action", "dismissStatus"}}}};
    refreshAll();
}
bool ReviewWindow::savedCurrent() const {
    auto saved = savedConfig_.toJson(), current = config_.toJson();
    saved.remove("budgetBytes"); current.remove("budgetBytes");
    return savedRevision_ == edit_.revision && saved == current;
}
void ReviewWindow::setSavedCopy(const QString& path, quint64 revision, const RecordingConfig& config) {
    savedPath_ = path;
    savedRevision_ = revision;
    savedConfig_ = config;
    savedDismissed_ = false;
    lastShowInFolder_ = {};
    refreshAll();
}
void ReviewWindow::showError(const QString& message, bool retryExport) {
    message_ = message.isEmpty() ? StatusEntry{} : StatusEntry{message, QStringLiteral("error"), {{QStringLiteral("Dismiss"), QJsonObject{{"action", "dismissStatus"}}}}};
    if (!message.isEmpty() && retryExport) message_.actions.prepend({QStringLiteral("Try again"), QJsonObject{{"action", "retryExport"}}});
    refreshAll();
}
void ReviewWindow::setNotice(const QString& text) {
    notice_->setText(text);
    noticeBand_->setVisible(!text.isEmpty());
    refreshBanners();
}
void ReviewWindow::reopen(const QString& error, bool retry) {
    closing_ = false;
    show();
    raise();
    activateWindow();
    if (!error.isEmpty()) showError(error, retry);
}
void ReviewWindow::setLimits(qint64 animationMaxUs, int animationMaxEdge) {
    animationMaxUs_ = animationMaxUs;
    animationMaxEdge_ = animationMaxEdge;
    refreshAll();
}
// The upload rule (#1635): Upload (Ctrl+Enter, Enter) commits a pending nudge
// and waits until every proposal made so far was accepted, then sends that
// revision with the output the window shows. It is cancelled, through
// cancelUpload(), only when what it would export changes: the edit's revision
// moves for anything but the awaited answers (a later edit, undo or redo, by
// the user or an agent), or the output (format, codec, sound, size) changes;
// and by a refusal or 2 s without an answer. The resident refuses an upload at
// any other revision or output.
void ReviewWindow::upload() {
    // A time or strength still being typed is committed first (P9).
    panel_->commitFields();
    // Mid-gesture or placing there is a change not yet made: nothing to upload.
    if (exporting_ || closing_ || placing_ || view_->dragging() || timeline_->dragging()) return;
    if (uploadWaiting_) {
        uploadNotice_ = {QStringLiteral("Upload already waiting for your last change to be saved."), QStringLiteral("info"), {}};
        refreshStatus();
        return;
    }
    stopPlayback();
    view_->commitNudge();
    panel_->flushStrength();
    uploadNotice_ = {};
    uploadAwaits_ = proposalSeq_;
    uploadWaiting_ = true;
    uploadWait_.start();
    tryUpload();
}
bool ReviewWindow::tryUpload() {
    if (!uploadWaiting_ || proposalPending()) return false;
    uploadWaiting_ = false;
    uploadWait_.stop();
    uploadNotice_ = {};  // "already waiting" is over once it is sent
    refreshAll();
    closing_ = true;
    emit uploadRequested(edit_.revision, config_);
    close();
    return true;
}
void ReviewWindow::cancelUpload(const QString& why) {
    if (!uploadWaiting_) return;
    uploadWaiting_ = false;
    uploadWait_.stop();
    // Its own status line, kept when the awaited answer arrives.
    uploadNotice_ = {QStringLiteral("Upload cancelled: %1. Press Upload again.").arg(why.isEmpty() ? QStringLiteral("you changed the clip") : why), QStringLiteral("warning"), {}};
    refreshAll();
}
void ReviewWindow::emitConfig() {
    cancelUpload();
    emit configChanged(config_, ++configSeq_);
    refreshAll();
}
// Fit keeps the 1280 px preview; 100 % asks for the source's own pixels.
int ReviewWindow::viewEdge() const {
    if (view_ && view_->actualSize() && !options_.canvas.isEmpty()) return std::max({1280, options_.canvas.width(), options_.canvas.height()});
    return 1280;
}
// R16: an answer shows when its frame lies within 2 strip px of the pointer's
// time, or 250 ms, whichever is wider.
qint64 ReviewWindow::skimReach() const {
    const auto span = timeline_->visibleSpan();
    return std::max<qint64>(250000, (span.end - span.start) * 2 / std::max(1, timeline_->stripRect().width()));
}
void ReviewWindow::askSkimFrame(qint64 at) {
    skimAsked_ = std::pair<qint64, quint64>(at, edit_.revision);
    skimmer_->request(at, edit_, viewEdge());
}
// An exact frame answers every time from its request up to its own time (no
// frame lies between), unless a cover starts or ends in between: then the
// covers on it could differ.
const ReviewWindow::SkimFrame* ReviewWindow::cachedSkim(qint64 at) const {
    for (auto it = skimCache_.crbegin(); it != skimCache_.crend(); ++it) {
        if (it->revision != edit_.revision || it->edge != viewEdge()) continue;
        // The requested time itself, or a time between the request and the
        // frame's own time (either way round: a last frame lies before it, Q11).
        if (at == it->requested) return &*it;
        const qint64 lo = std::min(it->requested, it->frameUs), hi = std::max(it->requested, it->frameUs);
        if (at < lo || at > hi) continue;
        const bool edgeBetween = std::any_of(edit_.masks.cbegin(), edit_.masks.cend(), [&](const RecordingMask& m) {
            return (m.start > std::min(it->requested, at) && m.start <= std::max(it->requested, at)) || (m.end > std::min(it->requested, at) && m.end <= std::max(it->requested, at));
        });
        if (!edgeBetween) return &*it;
    }
    return nullptr;
}
void ReviewWindow::cacheSkim(SkimFrame frame) {
    skimCache_.removeIf([&](const SkimFrame& f) { return f.requested == frame.requested && f.edge == frame.edge; });
    skimCache_.append(std::move(frame));
    const qint64 bytes = std::max<qint64>(1, skimCache_.last().image.sizeInBytes());
    const int keep = int(std::clamp<qint64>(skimCacheBytes / bytes, 1, skimCacheFrames));
    while (skimCache_.size() > keep) skimCache_.removeFirst();
}

// ---- refresh ----
void ReviewWindow::refreshAll() {
    refreshTools();
    refreshTransport();
    refreshFooter();
    refreshPanel();
    refreshStatus();
    refreshBanners();
}
void ReviewWindow::refreshTools() {
    if (!select_) return;
    const bool idle = !exporting_;
    select_->setChecked(!placing_);
    redact_->setChecked(placing_ && placing_->kind == "solid");
    blur_->setChecked(placing_ && placing_->kind == "blur");
    cut_->setEnabled(idle && timeSelection_.has_value() && !placing_);
    cut_->setToolTip(timeSelection_ ? QStringLiteral("Cut the selected time (C)") : QStringLiteral("Select time on the filmstrip to cut it (C)"));
    undo_->setEnabled(idle && (edit_.canUndo || placing_));
    redo_->setEnabled(idle && edit_.canRedo && !placing_);
    pictureZoom_->setText(view_->actualSize() ? QStringLiteral("100%") : QStringLiteral("Fit · %1%").arg(view_->zoomPercent()));
    pictureZoom_->setAccessibleDescription(pictureZoom_->text());
}
void ReviewWindow::refreshTransport() {
    if (!time_) return;
    const qint64 output = kept_.toOutput(playhead_);
    time_->setText(QStringLiteral("%1 / %2").arg(durationText(output), durationText(kept_.outputDuration())));
    const bool excluded = !kept_.kept(playhead_);
    // P18: with trims or cuts the recording time is shown too.
    const bool differs = edit_.trimStart > 0 || edit_.trimEnd || !edit_.cuts.isEmpty();
    if (excluded) clipTime_->setText(QStringLiteral("rec %1 · excluded").arg(durationText(playhead_)));
    else clipTime_->setText(differs ? QStringLiteral("clip · rec %1").arg(durationText(playhead_)) : QStringLiteral("clip time"));
    time_->setToolTip(QStringLiteral("Clip time after trims and cuts / clip length. Recording time %1 of %2.").arg(durationText(playhead_), durationText(options_.durationUs)));
    time_->setAccessibleDescription(time_->text() + (excluded ? QStringLiteral(", excluded from output") : QString()));
    play_->setIcon(Ui::icon(playing_ ? "pause" : "play"));
    play_->setAccessibleName(playing_ ? QStringLiteral("Pause") : QStringLiteral("Play"));
    play_->setToolTip(playing_ ? QStringLiteral("Pause the preview (Space)") : QStringLiteral("Play the kept footage silently (Space); speeds in its menu"));
    const int zoom = timeline_->zoom();
    timelineZoom_->setText(zoom <= 1 ? QStringLiteral("Fit") : QStringLiteral("%1×").arg(zoom));
}
void ReviewWindow::refreshFooter() {
    if (!format_) return;
    format_->setText(QStringLiteral("%1 · %2").arg(formatShort(config_.format), audioSummary()));
    format_->setToolTip(formatLabel(config_.format, config_.codec) + "\n" + formatHint(config_.format, config_.codec));
    format_->setAccessibleDescription(format_->text() + "\n" + format_->toolTip());
    for (auto* action : formats_->actions()) {
        const auto value = action->data().toStringList();
        action->setChecked(value.value(0) == config_.format && value.value(1) == config_.codec);
        if (value.value(0) == "gif" || value.value(0) == "webp") {
            const qint64 seconds = animationMaxUs_ > 0 ? animationMaxUs_ / secondUs : 60;
            const int edge = animationMaxEdge_ > 0 ? animationMaxEdge_ : 1280;
            action->setText(QStringLiteral("%1 · no sound · at most %2 s, %3 px").arg(value[0] == "gif" ? QStringLiteral("Animated GIF") : QStringLiteral("Animated WebP")).arg(seconds).arg(edge));
        }
    }
    includeAudio_->setVisible(options_.audioRecorded && !config_.silent());
    includeAudio_->blockSignals(true);
    includeAudio_->setChecked(audioIntent_);
    includeAudio_->blockSignals(false);
    const auto mode = sharingName(options_.sharingMode);
    const auto shown = options_.destination.isEmpty() ? options_.uploadSummary : options_.destination + QStringLiteral(" · ") + mode;
    destinationMenu_->setText(shown);
    destinationMenu_->setProperty("destinationName", options_.destination.isEmpty() ? options_.uploadSummary : options_.destination);
    destinationMenu_->setProperty("sharingSuffix", options_.destination.isEmpty() ? QString() : QStringLiteral(" · ") + mode);
    const auto details = "Destination: " + options_.destination + "\nSharing: " + mode + "\n\n" + options_.uploadSummary + "\n" + formatLabel(config_.format, config_.codec) + "\n" + formatHint(config_.format, config_.codec) + "\nAudio: " + audioSummary();
    destinationFull_->setText(details);
    destinationMenu_->setToolTip(details);
    destinationMenu_->setAccessibleName(shown);
    destinationMenu_->setAccessibleDescription(details);
    destinationMenu_->setVisible(!options_.uploadSummary.isEmpty());
    destinationMenu_->setMaximumWidth(destinationMenu_->sizeHint().width());
    destinationMenu_->updateGeometry();
    // Save and Upload wait while a cover is being placed.
    upload_->setEnabled(!exporting_ && !placing_);
    save_->setEnabled(!exporting_ && !placing_);
    upload_->setToolTip(placing_ ? QStringLiteral("Place or cancel the cover first") : QStringLiteral("Upload the reviewed clip with its edits (Ctrl+Enter). The preview is silent; video exports keep the recorded audio."));
}
void ReviewWindow::refreshPanel() {
    if (!panel_) return;
    ReviewPanel::State state;
    state.edit = edit_;
    state.durationUs = options_.durationUs;
    state.fps = config_.fps;
    state.kept = kept_;
    state.timeSelection = timeSelection_;
    state.selectedKind = selectedKind_;
    state.selectedId = selectedId_;
    if (const auto* mask = selectedMask()) {
        state.coverFirstKept = firstKeptFrame(mask->start, mask->end);
        state.coverLastKept = lastKeptFrame(mask->start, mask->end);
    }
    state.playheadUs = playhead_;
    state.placingKind = placing_ ? placing_->kind : QString();
    panel_->setState(state);
    // Q20: a cut or cover selected anywhere has its Edits row in view.
    if (revealRow_) { panel_->revealRow(revealRow_->first, revealRow_->second); revealRow_.reset(); }
}
void ReviewWindow::refreshBanners() {
    if (!placeBanner_) return;
    refreshNote();
    const bool any = !noticeBand_->isHidden() || !statusRow_->isHidden();
    if (bannerArea_->isHidden() == any) bannerArea_->setVisible(any);
    placeBanners();
}
QList<ReviewWindow::StatusEntry> ReviewWindow::statusEntries() const {
    QList<StatusEntry> entries;
    if (!uploadNotice_.text.isEmpty()) entries.append(uploadNotice_);
    if (!message_.text.isEmpty()) entries.append(message_);
    if (!savedPath_.isEmpty() && !savedDismissed_ && !exporting_)
        entries.append({(savedCurrent() ? QStringLiteral("Saved %1") : QStringLiteral("Earlier copy saved: %1")).arg(QFileInfo(savedPath_).fileName()),
            savedCurrent() ? QStringLiteral("success") : QStringLiteral("info"),
            {{QStringLiteral("Copy path"), {{"action", "copyPath"}}}, {QStringLiteral("Show in folder"), {{"action", "showInFolder"}}}, {QStringLiteral("Dismiss"), {{"action", "dismissSavedCopy"}}}}});
    if (exporting_) return entries;
    // A switch to a format this recording offers, when one would resolve the problem.
    const auto offerCompatibleVideo = [this]() -> std::optional<QPair<QString, QJsonObject>> {
        for (auto* action : formats_->actions())
            if (action->data().toStringList() == QStringList{"mp4", "h264"}) return QPair<QString, QJsonObject>{QStringLiteral("Use MP4 (H.264)"), QJsonObject{{"action", "format"}, {"format", "mp4"}, {"codec", "h264"}}};
        return std::nullopt;
    };
    if (config_.format == "mp4" && config_.codec == "h265") {
        StatusEntry codec{QStringLiteral("Use MP4 (H.264) for broader playback compatibility."), QStringLiteral("warning"), {}};
        if (const auto h264 = offerCompatibleVideo()) codec.actions.append(*h264);
        if (message_.tone != "error") entries.append(codec);
    }
    // GIF/WebP limits count kept time (trim and cuts).
    const qint64 keptUs = kept_.outputDuration();
    if (config_.animation() && animationMaxUs_ > 0 && keptUs > animationMaxUs_) {
        const qint64 seconds = animationMaxUs_ / secondUs;
        StatusEntry trim{QStringLiteral("%1 is limited to %2 s and the clip is %3. Trim or cut it to %2 s or less before exporting.").arg(formatShort(config_.format)).arg(seconds).arg(durationText(keptUs)), QStringLiteral("warning"), {}};
        trim.actions.append({QStringLiteral("Keep the first %1 s").arg(seconds), QJsonObject{{"action", "trimToLimit"}}});
        if (const auto mp4 = offerCompatibleVideo()) trim.actions.append(*mp4);
        entries.append(trim);
    }
    if (config_.animation() && animationMaxEdge_ > 0 && !options_.canvas.isEmpty()) {
        const auto size = recording::outputSize(options_.canvas, config_.maxEdge);
        if (std::max(size.width(), size.height()) > animationMaxEdge_) {
            StatusEntry edge{QStringLiteral("%1 is limited to %2 px and this recording is %3 × %4 px, so the export will be refused.").arg(formatShort(config_.format)).arg(animationMaxEdge_).arg(size.width()).arg(size.height()), QStringLiteral("warning"), {}};
            if (const auto mp4 = offerCompatibleVideo()) edge.actions.append(*mp4);
            entries.append(edge);
        }
    }
    return entries;
}
void ReviewWindow::refreshStatus() {
    if (!statusRow_) return;
    auto entries = statusEntries();
    for (auto& entry : entries) {
        if (entry.tone != "error" || entry.text != message_.text) continue;
        const QString raw = entry.text;
        if (raw.contains("No space left", Qt::CaseInsensitive) || (raw.contains("disk", Qt::CaseInsensitive) && (raw.contains("full", Qt::CaseInsensitive) || raw.contains("space", Qt::CaseInsensitive))))
            entry.text = QStringLiteral("Disk is full. Free up space, then try again.");
        else if (raw.contains("Permission denied", Qt::CaseInsensitive))
            entry.text = QStringLiteral("Couldn't save the copy: permission denied. Choose another location, then try again.");
        else {
            entry.text = raw.section('\n', 0, 0);
            if (std::any_of(entry.actions.cbegin(), entry.actions.cend(), [](const auto& action) { return action.second["action"] == "retryExport"; }) && !entry.text.contains("save", Qt::CaseInsensitive) && !entry.text.contains("export", Qt::CaseInsensitive) && !entry.text.contains("upload", Qt::CaseInsensitive))
                entry.text.prepend(QStringLiteral("Couldn't export the recording: "));
        }
    }
    // One line at a time, an error first; the other facts stay in inspect. A
    // cancelled upload says so on a line of its own above it.
    if (!uploadNotice_.text.isEmpty()) entries.removeOne(uploadNotice_);
    if (!entries.isEmpty()) {
        const auto error = std::find_if(entries.cbegin(), entries.cend(), [](const auto& entry) { return entry.tone == "error"; });
        entries = {error == entries.cend() ? entries.first() : *error};
    }
    if (!uploadNotice_.text.isEmpty()) entries.prepend(uploadNotice_);
    // Try again waits while a cover is being placed, as Save and Upload do.
    const auto guardRetry = [this] {
        for (auto* button : statusRow_->findChildren<QPushButton*>("statusAction"))
            if (button->property("driveAction").toJsonObject()["action"] == "retryExport") {
                button->setEnabled(!placing_);
                button->setToolTip(placing_ ? QStringLiteral("Place or cancel the cover first") : QString());
            }
    };
    if (entries == shownStatus_) { guardRetry(); return; }
    shownStatus_ = entries;
    errorDetails_->hide();
    errorDetails_->setParent(this);
    // Later: a status action's own click may be what changed the row.
    while (auto* item = statusLines_->takeAt(0)) { if (auto* w = item->widget()) { w->hide(); w->deleteLater(); } delete item; }
    for (const auto& entry : std::as_const(entries)) {
        const auto tone = entry.tone == "error" ? Ui::Banner::Tone::Error : entry.tone == "warning" ? Ui::Banner::Tone::Warning : entry.tone == "success" ? Ui::Banner::Tone::Success : Ui::Banner::Tone::Info;
        auto* band = new Ui::Banner(tone, {}, statusRow_);
        band->setObjectName("statusLine");
        auto* line = new QWidget(band);
        auto* row = new QHBoxLayout(line);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(reviewSpace(Ui::Space::s));
        const bool saved = std::any_of(entry.actions.cbegin(), entry.actions.cend(), [](const auto& action) { return action.second["action"] == "copyPath"; });
        QLabel* text;
        if (saved) {
            auto* label = new Ui::ElidedLabel(line);
            label->setText(entry.text);
            label->setToolTip(savedPath_);
            text = label;
        } else text = new QLabel(entry.text, line);
        text->setObjectName("status");
        text->setTextFormat(Qt::PlainText);
        text->setWordWrap(!saved);
        text->setTextInteractionFlags(Qt::TextSelectableByMouse);
        text->setAccessibleName(entry.text);
        Ui::role(text, "meta");
        row->addWidget(text, 1);
        if (entry.tone == "error" && std::any_of(entry.actions.cbegin(), entry.actions.cend(), [](const auto& action) { return action.second["action"] == "dismissStatus"; })) {
            errorDetails_->setParent(line);
            row->addWidget(errorDetails_, 0, Qt::AlignVCenter);
            errorDetails_->show();
        }
        for (const auto& [label, params] : entry.actions) {
            const auto action = params["action"].toString();
            if (action == "dismissStatus" || action == "dismissSavedCopy") {
                band->setDismissible(true, label);
                auto* dismiss = band->dismissButton();
                dismiss->setObjectName("statusAction");
                dismiss->setProperty("driveAction", params);
                connect(dismiss, &QToolButton::clicked, this, [this, params] { drive(params); });
                continue;
            }
            auto* button = new QPushButton(label, line);
            button->setProperty("uiDensity", "compact");
            button->setObjectName("statusAction");
            Ui::role(button, "quiet");
            button->setProperty("driveAction", params);
            connect(button, &QPushButton::clicked, this, [this, params] { drive(params); });
            row->addWidget(button, 0, Qt::AlignVCenter);
        }
        band->setContentWidget(line);
        statusLines_->addWidget(band);
        band->show();
    }
    statusRow_->setVisible(!entries.isEmpty());
    guardRetry();
    refreshBanners();
}
void ReviewWindow::refreshFold() {
    if (!panel_) return;
    const auto limit = foldBelow();
    const bool fold = width() < limit.width() || height() < limit.height();
    auto* host = findChild<QWidget*>("reviewPanelHost");
    if (fold == folded_ && host->isHidden() == fold) return;
    folded_ = fold;
    if (!fold && overlay_->isOpen()) overlay_->dismiss();
    host->setVisible(!fold);
    editsToggle_->setVisible(fold);
}
void ReviewWindow::placeOverlayInset() {
    if (!overlay_->isOpen()) { view_->setRightInset(0); placeBanners(); return; }
    const int left = view_->mapFrom(this, overlay_->geometry().topLeft()).x();
    view_->setRightInset(std::max(0, view_->width() - left + reviewSpace(Ui::Space::s)));
    placeBanners();
}
// The banners along the viewer's top edge: the strip the picture leaves free
// holds one; more stack down over the picture, never moving it (D9).
void ReviewWindow::placeBanners() {
    if (!bannerArea_ || !view_) return;
    const QPoint at = view_->mapTo(this, QPoint());
    const int width = std::max(0, view_->width() - view_->rightInset());
    auto* layout = bannerArea_->layout();
    layout->activate();
    const int wanted = layout->hasHeightForWidth() ? layout->totalHeightForWidth(width) : bannerArea_->sizeHint().height();
    bannerArea_->setGeometry(at.x(), at.y(), width, std::min(wanted, view_->height()));
    bannerArea_->raise();
    if (overlay_ && overlay_->isOpen()) overlay_->raise();
}
void ReviewWindow::requestTiles() {
    const auto times = timeline_->tileTimes();
    if (times.isEmpty() || !filmstrip_) return;
    // The strip's slot in device pixels, at the clip's aspect so the timeline
    // can crop the tile to cover its slot.
    const QRect strip = timeline_->stripRect();
    const qreal dpr = timeline_->devicePixelRatioF();
    const QSize slot(std::max(1, qRound(strip.width() / double(times.size()) * dpr)), std::max(1, qRound(strip.height() * dpr)));
    const QSize size = options_.canvas.isEmpty() ? slot : options_.canvas.scaled(slot, Qt::KeepAspectRatioByExpanding);
    filmstrip_->request(times, size);
}
void ReviewWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    refreshFold();
    placeOverlayInset();
    refreshTools();
}
void ReviewWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!initialFitDone_) {
        // Opens at 1440×900 within the work area.
        initialFitDone_ = true;
        resize(defaultSize().boundedTo(availableReviewSize()));
        setMinimumSize(minimumSizeHint().boundedTo(availableReviewSize()));
        requestFrame();
        tileTimer_.start();
    }
    refreshFold();
}

// ---- advice, inspect, drive ----
void ReviewWindow::showAdvice() {
    if (adviceDialog_) { auto* dialog = adviceDialog_.data(); adviceDialog_.clear(); dialog->close(); return; }
    if (message_.tone != "error") return;
    const bool failed = std::any_of(message_.actions.cbegin(), message_.actions.cend(), [](const auto& action) { return action.second["action"] == "retryExport"; });
    auto* dialog = new QDialog(this);
    adviceDialog_ = dialog;
    dialog->setObjectName("reviewAdvice");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    const QString title = failed ? QStringLiteral("Export failed") : QStringLiteral("Recording error");
    dialog->setWindowTitle(title);
    dialog->setAccessibleName(title);
    auto* root = new QVBoxLayout(dialog);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto* body = new QWidget(dialog);
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(reviewSpace(Ui::Space::xl), reviewSpace(Ui::Space::l), reviewSpace(Ui::Space::xl), reviewSpace(Ui::Space::l));
    layout->setSpacing(reviewSpace(Ui::Space::m));
    auto* heading = new QLabel(title, body);
    Ui::role(heading, "sectionTitle");
    layout->addWidget(heading);
    // The heading already says "Export failed"; the details don't repeat it.
    auto shown = message_.text;
    if (shown.startsWith(title + QStringLiteral(": "))) { shown.remove(0, title.size() + 2); if (!shown.isEmpty()) shown[0] = shown[0].toUpper(); }
    auto* text = new QLabel(shown, body);
    text->setObjectName("reviewAdviceText");
    text->setProperty("plainText", message_.text);
    text->setTextFormat(Qt::PlainText);
    text->setWordWrap(true);
    Ui::readOnlyText(text);
    text->setAccessibleName(title);
    layout->addWidget(text);
    layout->addStretch();
    root->addWidget(body, 1);
    auto* footer = new Ui::Surface("bg", Qt::TopEdge, dialog);
    auto* actions = new QHBoxLayout(footer);
    actions->setContentsMargins(reviewSpace(Ui::Space::l), reviewSpace(Ui::Space::m), reviewSpace(Ui::Space::l), reviewSpace(Ui::Space::m));
    actions->setSpacing(reviewSpace(Ui::Space::s));
    actions->addStretch();
    if (failed) {
        auto* retry = new QPushButton("Try again", footer);
        Ui::role(retry, "primary");
        retry->setDefault(true);
        connect(retry, &QPushButton::clicked, this, [this, dialog] { dialog->close(); drive({{"action", "retryExport"}}); });
        actions->addWidget(retry);
    }
    auto* close = new QPushButton("Close", footer);
    connect(close, &QPushButton::clicked, this, [dialog] { dialog->close(); });
    actions->addWidget(close);
    root->addWidget(footer);
    const QSize available = availableReviewSize().boundedTo(dialog->screen()->availableGeometry().size());
    const int width = std::min(Ui::scaled(640), available.width());
    const int height = heading->sizeHint().height() + text->heightForWidth(std::max(1, width - layout->contentsMargins().left() - layout->contentsMargins().right())) + footer->sizeHint().height() + Ui::scaled(62);
    dialog->resize(QSize(width, height).boundedTo(available));
    dialog->show();
    text->setFocus();
}
QJsonObject ReviewWindow::inspect() const {
    auto rect = [](const QRect& r) { return QJsonArray{r.x(), r.y(), r.width(), r.height()}; };
    const auto* mask = selectedMask();
    const QRect video(view_->mapTo(this, view_->imageRect().topLeft()), view_->imageRect().size());
    const double area = double(std::max(1, width())) * std::max(1, height());
    const QPoint timelineAt = timeline_->mapTo(this, QPoint());
    QJsonArray masks;
    for (const auto& m : edit_.masks) masks.append(QJsonObject{{"id", m.id}, {"name", maskNames_->name(m)}, {"kind", m.kind}, {"start", double(m.start)}, {"end", double(m.end)}, {"timeline", rect(timeline_->coverBarRect(m.id).translated(timelineAt))}});
    QJsonArray cuts;
    for (const auto& c : edit_.cuts) cuts.append(QJsonObject{{"id", c.id}, {"start", double(c.start)}, {"end", double(c.end)}, {"timeline", rect(timeline_->cutRect(c.id).translated(timelineAt))}});
    // The status lines as shown: each line's words, tone and its actions as drive() calls.
    QJsonArray statusRow;
    QStringList lines;
    for (const auto& entry : shownStatus_) {
        QJsonArray actions;
        for (const auto& [label, params] : entry.actions) actions.append(QJsonObject{{"label", label}, {"drive", params}});
        statusRow.append(QJsonObject{{"text", entry.text}, {"tone", entry.tone}, {"actions", actions}});
        lines.append(entry.text);
    }
    QJsonArray statusFacts;
    for (const auto& entry : statusEntries()) {
        QJsonArray actions;
        for (const auto& [label, params] : entry.actions) actions.append(QJsonObject{{"label", label}, {"drive", params}});
        statusFacts.append(QJsonObject{{"text", entry.text}, {"tone", entry.tone}, {"actions", actions}});
    }
    // #1613: a time selection and an edit selection never coexist.
    const QJsonValue selection = timeSelection_ ? QJsonValue(QJsonObject{{"start", double(timeSelection_->start)}, {"end", double(timeSelection_->end)}, {"lengthUs", double(timeSelection_->end - timeSelection_->start)}}) : QJsonValue(QJsonValue::Null);
    const QJsonValue selectedEdit = selectedKind_.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(QJsonObject{{"kind", selectedKind_}, {"id", selectedId_.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(selectedId_)}});
    const QJsonValue pan = QJsonObject{{"x", view_->pan().x()}, {"y", view_->pan().y()}};
    QJsonValue placement(QJsonValue::Null);
    if (placing_) placement = QJsonObject{{"kind", placing_->kind}, {"start", double(placing_->span.start)}, {"end", double(placing_->span.end)}, {"whole", placing_->whole}, {"banner", placeBanner_->text()}};
    auto timeline = timeline_->inspect();
    timeline["rect"] = rect(timeline_->geometry());
    timeline["strip"] = rect(timeline_->stripRect().translated(timelineAt));
    timeline["ruler"] = rect(timeline_->rulerRect().translated(timelineAt));
    auto panel = panel_->inspect();
    panel["folded"] = folded_;
    panel["overlayOpen"] = overlay_->isOpen();
    return QJsonObject{
        {"visible", isVisible()}, {"size", QJsonArray{width(), height()}}, {"minimumSize", QJsonArray{minimumSizeHint().expandedTo(minimumSize()).width(), minimumSizeHint().expandedTo(minimumSize()).height()}},
        {"scheme", Theme::name(Theme::scheme())}, {"exporting", exporting_},
        {"playheadUs", double(playhead_)}, {"outputPlayheadUs", double(kept_.toOutput(playhead_))}, {"outputDurationUs", double(kept_.outputDuration())},
        {"outsideKeptPart", !kept_.kept(playhead_)}, {"playing", playing_}, {"speed", speed_},
        {"revision", double(edit_.revision)}, {"canUndo", edit_.canUndo}, {"canRedo", edit_.canRedo},
        {"trim", QJsonObject{{"start", double(edit_.trimStart)}, {"end", double(edit_.exportEnd(options_.durationUs))}}},
        {"cuts", cuts}, {"masks", masks}, {"selectedMaskId", mask ? QJsonValue(mask->id) : QJsonValue(QJsonValue::Null)},
        {"selectedMaskName", mask ? QJsonValue(maskNames_->name(*mask)) : QJsonValue(QJsonValue::Null)},
        {"selection", selection}, {"selectedEdit", selectedEdit}, {"edits", panel["rows"]}, {"placement", placement},
        {"lastRefusal", lastRefusal_.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(lastRefusal_)}, {"uploadWaiting", uploadWaiting_}, {"uploadDeadlineMs", uploadWait_.isActive() ? uploadWait_.remainingTime() : -1},
        {"playbackQueuedEnd", bool(playEnded_ && playEnded_->load())},
        {"playhead", QJsonObject{{"sourceUs", double(playhead_)}, {"outputUs", double(kept_.toOutput(playhead_))}}},
        {"undo", QJsonObject{{"canUndo", edit_.canUndo}, {"canRedo", edit_.canRedo}}},
        {"viewer", QJsonObject{{"zoom", view_->actualSize() ? QStringLiteral("actual") : QStringLiteral("fit")}, {"percent", view_->zoomPercent()}, {"pan", pan},
            {"live", view_->liveId().isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(QJsonObject{{"maskId", view_->liveId()}, {"pending", view_->livePending()}})},
            {"arrowsClaimed", view_->arrowsClaimed()}, {"placeWaiting", view_->placeWaiting()},
            // Q23/Q24: where the selected cover's box and grips really are, in window coordinates.
            {"selectedCover", [&]() -> QJsonValue {
                auto geometry = view_->selectedGeometry();
                if (geometry.isEmpty()) return QJsonValue(QJsonValue::Null);
                const QPoint origin = view_->mapTo(this, QPoint());
                auto box = geometry["box"].toArray();
                geometry["box"] = QJsonArray{box[0].toDouble() + origin.x(), box[1].toDouble() + origin.y(), box[2], box[3]};
                QJsonArray grips;
                for (const auto& g : geometry["grips"].toArray()) grips.append(QJsonArray{g.toArray()[0].toDouble() + origin.x(), g.toArray()[1].toDouble() + origin.y()});
                geometry["grips"] = grips;
                return geometry;
            }()}}},
        {"toolNote", toolNote_->isHidden() ? QJsonValue(QJsonValue::Null) : QJsonValue(toolNote_->text())},
        {"warning", blurApplies() ? QJsonValue(blurBanner_->text()) : QJsonValue(QJsonValue::Null)}, {"folded", folded_},
        {"armed", view_->armed().isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(view_->armed())},
        {"blurGuidance", QJsonObject{{"visible", blurBanner_->isVisibleTo(this)}, {"text", blurBanner_->text()}}},
        {"tools", QJsonObject{{"cut", cut_->isEnabled()}, {"undo", undo_->isEnabled()}, {"redo", redo_->isEnabled()}, {"pictureZoom", pictureZoom_->text()}, {"editsToggle", editsToggle_->isVisibleTo(this)}}},
        {"panel", panel}, {"timeline", timeline},
        {"savedCopy", QJsonObject{{"path", savedPath_}, {"revision", double(savedRevision_)}, {"current", !savedPath_.isEmpty() && savedCurrent()}, {"showInFolder", lastShowInFolder_}}},
        {"errorDetails", QJsonObject{{"visible", errorDetails_->isVisibleTo(this)}, {"drive", QJsonObject{{"action", "showAdvice"}}}}},
        {"adviceDialog", QJsonObject{{"open", bool(adviceDialog_ && adviceDialog_->isVisible())}, {"text", adviceDialog_ && adviceDialog_->isVisible() ? QJsonValue(adviceDialog_->findChild<QLabel*>("reviewAdviceText")->property("plainText").toString()) : QJsonValue(QJsonValue::Null)}}},
        {"status", lines.join('\n')}, {"statusRow", statusRow}, {"statusFacts", statusFacts},
        {"audio", format_->text()}, {"previewMuted", true}, {"audioRequested", audioIntent_}, {"audioEffective", effectiveAudio(config_, options_.audioRecorded, audioIntent_)},
        {"destination", options_.destination}, {"sharingMode", options_.sharingMode}, {"uploadSummary", options_.uploadSummary},
        {"notice", noticeBand_->isVisibleTo(this) ? QJsonValue(notice_->text()) : QJsonValue(QJsonValue::Null)},
        {"preview", QJsonObject{{"error", view_->previewError().isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(view_->previewError())}, {"stale", view_->stale()}, {"frameTimeUs", view_->hasFrame() ? QJsonValue(double(view_->frameTime())) : QJsonValue(QJsonValue::Null)}, {"shownUs", shownFor_ == playhead_ ? QJsonValue(double(shownActual_)) : QJsonValue(QJsonValue::Null)}, {"frameSize", QJsonArray{view_->frameSize().width(), view_->frameSize().height()}}, {"skimSize", QJsonArray{view_->skimSize().width(), view_->skimSize().height()}},
            // An edge drag's frame shown and held until its proposal is answered (W4).
            {"edgePreviewUs", edgePreview_ ? QJsonValue(double(*edgePreview_)) : QJsonValue(QJsonValue::Null)}, {"edgeHeld", edgeHoldProposal_ != 0}}},
        {"frameFocus", view_->hasFocus()},
        {"discardConfirmation", QJsonObject{{"open", bool(discard_)}, {"text", discard_ ? QJsonValue(discard_->text()) : QJsonValue(QJsonValue::Null)}}},
        {"fileDialog", chooser_->state()},
        {"format", QJsonObject{{"format", config_.format}, {"codec", config_.codec}}},
        {"video", QJsonObject{{"rect", rect(video)}, {"share", double(video.width()) * video.height() / area}}},
        {"header", QJsonObject{{"visible", header_->isVisibleTo(this)}, {"rect", rect(header_->geometry())}, {"customFrame", Ui::WindowHeader::customFrame()}}}};
}
// The state after an action that proposed edits lists them (proposals: [n, …]),
// so the resident can tell whether it refused one: inspect's lastRefusal.proposal.
ReviewWindow::DriveResult ReviewWindow::drive(const QJsonObject& params) {
    const quint64 before = proposalSeq_;
    auto result = driveAction(params);
    // An edit-proposing action refused here (bad values, nothing selected, a
    // dialog open, an export running) is a change not applied: like a refused
    // gesture it cancels a waiting Upload (#1635 W2). The error code is kept.
    static const QSet<QString> edits{"setIn", "setOut", "resetTrim", "addMask", "placeMask", "useSolidCover", "trimToLimit", "setMaskIn", "setMaskOut",
        "strength", "removeMask", "cut", "adjustCut", "restoreCut", "undo", "redo", "setCoverEdge", "setCoverEdgeToPlayhead", "coverKind", "setTrim", "retimeCover", "setCutEdge"};
    if (!result.code.isEmpty() && edits.contains(params["action"].toString())) refuseChange(result.message);
    if (result.code.isEmpty() && proposalSeq_ != before) {
        QJsonArray made;
        for (quint64 n = before + 1; n <= proposalSeq_; ++n) made.append(double(n));
        result.state["proposals"] = made;
    }
    return result;
}
ReviewWindow::DriveResult ReviewWindow::driveAction(const QJsonObject& params) {
    const auto action = params["action"].toString();
    const auto invalid = [](const QString& message) { return DriveResult{"invalid_request", message, {}}; };
    auto done = [this](const QString& error = {}) { return error.isEmpty() ? DriveResult{{}, {}, inspect()} : DriveResult{"invalid_request", error, {}}; };
    if (action == "inspect") return done();
    // The discard confirmation, as the Discard… button, the close button and its two answers.
    if (action == "keepReviewing" || action == "confirmDiscard") {
        if (!discard_) return {"recording_state", "No discard confirmation is open", {}};
        answerDiscard(action == "confirmDiscard");
        return done();
    }
    if (discard_) return {"recording_state", "The discard confirmation is open; answer it first (keepReviewing or confirmDiscard)", {}};
    // #259: the Save a copy… chooser, as the button opens it; the copy is saved
    // when someone picks a file in it (recording.saveCopy saves to a path).
    if (action == "closeFileDialog") { chooser_->cancel(); return done(); }
    if (exporting_) return {"recording_state", "The review controls are disabled while an export runs", {}};
    if (!isVisible()) return {"recording_state", "The review window is closed", {}};
    if (chooser_->isOpen()) return {"recording_state", "The Save copy dialog is open; close it first (closeFileDialog)", {}};
    if (action == "saveCopyDialog") {
        if (placing_) return {"recording_state", "Place or cancel the cover first", {}};
        chooseCopy();
        return done();
    }
    if (action == "discard") { askDiscard(); return done(); }
    if (action == "retryExport") {
        if (std::none_of(message_.actions.cbegin(), message_.actions.cend(), [](const auto& action) { return action.second["action"] == "retryExport"; })) return invalid("There is no failed export to retry");
        // As Save and Upload: never while a cover is being placed or a change is unanswered.
        if (placing_) return {"recording_state", "Place or cancel the cover first", {}};
        if (proposalPending() || view_->nudging() || view_->dragging() || timeline_->dragging()) return {"recording_state", "Wait until your last change is saved, then try again", {}};
        stopPlayback();
        // residentRetries: the resident asked for the check and retries itself (recording.review).
        if (!params["residentRetries"].toBool()) emit retryExportRequested();
        return done();
    }
    if (action == "retryPreview") {
        // A clip that could not be opened is opened again; the frame is asked again either way.
        if (frames_->failedToOpen() && !options_.workingClip.isEmpty()) frames_->open(options_.workingClip, options_.canvas);
        frameAsked_.reset();
        frameTimer_.stop();
        askFrame();
        return done();
    }
    if (action == "showAdvice") {
        if (message_.tone != "error") return invalid("There is no error to explain");
        showAdvice();
        return done();
    }
    if (action == "dismissSavedCopy") { savedDismissed_ = true; refreshAll(); return done(); }
    if (action == "copyPath" || action == "showInFolder") {
        if (savedPath_.isEmpty()) return invalid("Save a copy first");
        if (action == "copyPath") QGuiApplication::clipboard()->setText(savedPath_);
        else {
            QPointer<ReviewWindow> guard(this);
            host::showInFolder(savedPath_, [guard](const QJsonObject& result) {
                if (!guard) return;
                guard->lastShowInFolder_ = result;
                if (!result["opened"].toBool()) guard->showError(QStringLiteral("Couldn't open the folder. Copy the path and open it in your file manager."), false);
            });
        }
        return done();
    }
    if (action == "dismissStatus") {
        message_ = {};  // the upload notice stays until Upload is pressed again
        if (adviceDialog_) { adviceDialog_->close(); adviceDialog_.clear(); }
        refreshAll();
        return done();
    }
    if (action == "seek") {
        const auto time = params["time"].toDouble(-1);
        if (!params["time"].isDouble() || time < 0 || time >= double(options_.durationUs) || time != std::floor(time)) return invalid("seek takes a clip time in microseconds within the recording");
        seek(qint64(time));
        return done();
    }
    if (action == "play") { if (placing_) cancelPlacement(); startPlayback(); return done(); }
    if (action == "pause") { stopPlayback(); return done(); }
    if (action == "speed") {
        QAction* chosen = nullptr;
        for (auto* entry : speeds_->actions()) if (params["speed"].isDouble() && entry->data().toDouble() == params["speed"].toDouble()) chosen = entry;
        if (!chosen) return invalid("speed is 0.5, 1, 2 or 4");
        chosen->trigger();
        return done();
    }
    if (action == "setIn" || action == "setOut") {
        stopPlayback();
        return done(setTrimAt(action == "setIn"));
    }
    if (action == "resetTrim") { stopPlayback(); propose(QJsonArray{QJsonObject{{"op", "resetTrim"}}}); return done(); }
    if (action == "addMask") {
        // X/B and a drawn box in one step: the cover spans the time selection
        // (whole frames) or the whole recording. Without a rect, the centred box Enter places.
        const auto kind = params["kind"].toString();
        const auto r = params["rect"].toArray();
        if ((kind != "blur" && kind != "solid") || (params.contains("rect") && r.size() != 4)) return invalid("addMask takes kind blur|solid and an optional rect [x, y, width, height] in canvas pixels");
        const QRect rect = params.contains("rect") ? QRect(r[0].toInt(-1), r[1].toInt(-1), r[2].toInt(-1), r[3].toInt(-1)) : view_->defaultMaskRect();
        if (rect.x() < 0 || rect.y() < 0 || rect.width() < 2 || rect.height() < 2 || (!options_.canvas.isEmpty() && !QRect(QPoint(), options_.canvas).contains(rect))) return invalid("The cover rect must lie inside the canvas");
        if (!placing_ || placing_->kind != kind) arm(kind);
        placeCover(kind, rect);
        return done();
    }
    if (action == "selectMask") {
        const auto id = params["maskId"].isNull() || params["maskId"].isUndefined() ? QString() : params["maskId"].toString();
        if (!id.isEmpty() && !maskById(id)) return invalid("No mask has this ID");
        if (id.isEmpty()) clearSelection(); else selectCover(id);
        return done();
    }
    if (action == "placeMask") {
        // While a cover is armed: the box a pointer drag draws (one addMask). Otherwise
        // the selected cover's box, as a pointer drag or the frame's arrow keys set it.
        const auto r = params["rect"].toArray();
        if (params.contains("rect") == params.contains("at")) return invalid("placeMask takes exactly one of rect [x, y, width, height] or, while placing, at [x, y]");
        if (!placing_ && !selectedMask()) return invalid("Arm Redact or Blur to draw a box, or select a mask first");
        // A click while placing (W6): the default box centred at a canvas point.
        if (placing_ && params.contains("at")) {
            const auto at = params["at"].toArray();
            const QPointF point(at[0].toInt(-1), at[1].toInt(-1));  // whole canvas pixels, as rect
            if (at.size() != 2 || point.x() < 0 || point.y() < 0 || (!options_.canvas.isEmpty() && (point.x() >= options_.canvas.width() || point.y() >= options_.canvas.height()))) return invalid("placeMask at takes [x, y] in canvas pixels inside the canvas");
            stopPlayback();
            emit view_->maskCreated(placing_->kind, view_->defaultMaskRectAt(point));
            return done();
        }
        if (r.size() != 4) return invalid("placeMask takes rect [x, y, width, height] in canvas pixels, or while placing at [x, y]");
        const QRect rect(r[0].toInt(-1), r[1].toInt(-1), r[2].toInt(-1), r[3].toInt(-1));
        if (rect.x() < 0 || rect.y() < 0 || rect.width() < 2 || rect.height() < 2 || (!options_.canvas.isEmpty() && !QRect(QPoint(), options_.canvas).contains(rect))) return invalid("The mask rect must lie inside the canvas");
        stopPlayback();
        view_->commitNudge();
        if (placing_) emit view_->maskCreated(placing_->kind, rect);
        else emit view_->maskMoved(selectedId_, rect);
        return done();
    }
    if (action == "useSolidCover") {
        const auto* mask = selectedMask();
        if (!(placing_ && placing_->kind == "blur") && (!mask || mask->kind != "blur")) return invalid("Select a blur (or arm Blur) first");
        stopPlayback();
        useSolidCover();
        return done();
    }
    if (action == "trimToLimit") {
        if (!config_.animation() || animationMaxUs_ <= 0 || kept_.outputDuration() <= animationMaxUs_) return invalid("The clip is within the format's length limit");
        stopPlayback();
        // Trim the end so the kept output (after cuts) fits the limit.
        const qint64 end = kept_.toSource(animationMaxUs_);
        propose(QJsonArray{QJsonObject{{"op", "setTrim"}, {"start", double(edit_.trimStart)}, {"end", double(end)}}});
        return done();
    }
    if (action == "setMaskIn" || action == "setMaskOut") {
        stopPlayback();
        return done(setMaskAt(action == "setMaskIn"));
    }
    if (action == "strength") {
        const auto* mask = selectedMask();
        const auto value = params["strength"].toDouble(-1);
        if (!mask || mask->kind != "blur") return invalid("Select a blur mask first");
        if (value < 2 || value > 64 || value != std::floor(value)) return invalid("strength is 2 to 64");
        if (mask->strength != int(value)) updateCover(mask->id, [&](QJsonObject& m) { m["strength"] = int(value); });
        return done();
    }
    if (action == "removeMask") {
        if (!selectedMask()) return invalid("Select a mask first");
        stopPlayback();
        removeSelectedCover();
        return done();
    }
    // #1613: the redesigned window's controls. Each calls the function its button,
    // key or gesture calls; the checks only turn a refused state into an error.
    const auto state = [](const QString& message) { return DriveResult{"recording_state", message, {}}; };
    const auto whole = [&params](const char* key) -> std::optional<qint64> {
        const auto value = params[QLatin1String(key)];
        if (!value.isDouble() || value.toDouble() != std::floor(value.toDouble()) || std::abs(value.toDouble()) > 9e15) return std::nullopt;
        return qint64(value.toDouble());
    };
    const auto cutById = [this](const QString& id) { return std::any_of(edit_.cuts.cbegin(), edit_.cuts.cend(), [&](const RecordingCut& c) { return c.id == id; }); };
    const auto idParam = [&params](const char* key) { return params[QLatin1String(key)].isString() ? params[QLatin1String(key)].toString() : QString(); };
    const auto withinRecording = [this](const std::optional<qint64>& start, const std::optional<qint64>& end) { return start && end && *start >= 0 && *end > *start && *end <= options_.durationUs; };
    if (action == "selectRange") {
        const auto start = whole("start"), end = whole("end");
        if (!withinRecording(start, end)) return invalid("selectRange takes start < end, in microseconds within the recording");
        if (placing_) return state("Place or cancel the cover first");
        selectTime(*start, *end);
        return done();
    }
    if (action == "clearSelection") { if (timeSelection_ || !selectedKind_.isEmpty()) clearSelection(); return done(); }
    if (action == "cut") {
        if (placing_) return state("Place or cancel the cover first");
        if (!timeSelection_) return state("Select time on the filmstrip to cut it");
        cutSelection();
        return done();
    }
    if (action == "selectCut" || action == "restoreCut" || action == "playAcross" || action == "adjustCut") {
        const auto id = idParam("cutId");
        if (id.isEmpty() || !cutById(id)) return invalid("No cut has this ID");
        if (action == "adjustCut") {
            const auto start = whole("start"), end = whole("end");
            if (!withinRecording(start, end)) return invalid("adjustCut takes start < end, in microseconds within the recording");
            adjustCut(id, *start, *end);
        } else if (action == "selectCut") selectCut(id, false);
        else if (action == "restoreCut") restoreCut(id);
        else {
            if (kept_.empty()) return state("Nothing is kept to play");
            playAcross(id);
        }
        return done();
    }
    if (action == "undo" || action == "redo") {
        const bool undo = action == "undo";
        if (placing_ && !undo) return state("Place or cancel the cover first");
        if (!placing_ && !(undo ? edit_.canUndo : edit_.canRedo)) return state(undo ? "There is nothing to undo" : "There is nothing to redo");
        undoRedo(undo);
        return done();
    }
    if (action == "armRedact" || action == "armBlur") { arm(action == "armRedact" ? QStringLiteral("solid") : QStringLiteral("blur")); return done(); }
    if (action == "cancelPlacement") {
        if (!placing_) return state("No cover is being placed");
        cancelPlacement();
        refreshTools();
        return done();
    }
    if (action == "selectEdit") {
        const auto kind = params["kind"].toString(), id = idParam("editId");
        const bool known = (kind == "cut" && cutById(id)) || (kind == "cover" && maskById(id)) || (kind == "trim" && editsInOrder().contains({QStringLiteral("trim"), QString()}));
        if (!known) return invalid("selectEdit takes kind cut|cover|trim and the editId of an edit in the Edits list (a trim has none)");
        selectEdit(kind, id);
        return done();
    }
    if (action == "nextEdit" || action == "previousEdit") {
        if (editsInOrder().isEmpty()) return state("There are no edits to step through");
        stepEdit(action == "nextEdit" ? 1 : -1);
        return done();
    }
    if (action == "setCoverEdge") {
        // The Shows from / Until fields: their commit, with the same frame snapping.
        if (!selectedMask()) return state("Select a cover first");
        if (params["edge"] != "start" && params["edge"] != "end") return invalid("edge is start or end");
        const auto time = whole("time");
        if (!time || *time < 0 || *time > options_.durationUs) return invalid("setCoverEdge takes edge start|end and a recording time in microseconds");
        stopPlayback();
        if (!panel_->commitCoverEdge(params["edge"] == "start", *time)) return state("The cover's time fields are not shown");
        return done();
    }
    if (action == "seekCoverStart" || action == "seekCoverEnd" || action == "setCoverEdgeToPlayhead" || action == "coverKind") {
        const auto* mask = selectedMask();
        if (!mask) return state("Select a cover first");
        const bool start = action == "seekCoverStart" || (action == "setCoverEdgeToPlayhead" && params["edge"] == "start");
        if (action == "seekCoverStart" || action == "seekCoverEnd") {
            if (!(start ? firstKeptFrame(mask->start, mask->end) : lastKeptFrame(mask->start, mask->end))) return state("The cover is excluded from the output");
            seekCoverEdge(mask->id, start);
        } else if (action == "setCoverEdgeToPlayhead") {
            if (params["edge"] != "start" && params["edge"] != "end") return invalid("edge is start or end");
            stopPlayback();
            return done(setMaskAt(start, mask->id));
        } else {
            const auto kind = params["kind"].toString();
            if (kind != "solid" && kind != "blur") return invalid("coverKind takes kind solid|blur");
            if (mask->kind != kind) setCoverKind(mask->id, kind);
        }
        return done();
    }
    // T8: the timeline ignores zoom during a drag; say so rather than claim it.
    if ((action == "timelineZoom" || action == "timelineFit" || action == "zoomIn" || action == "zoomOut") && timeline_->dragging())
        return state("The timeline is being dragged; zoom is ignored until the drag ends");
    if (action == "timelineZoom") {
        // A level of the zoom button's menu: inspect's timeline.zoomLevels.
        const auto levels = timeline_->zoomLevels();
        const auto level = whole("level");
        if (!level || !levels.contains(int(*level))) {
            QStringList named;
            for (const int l : levels) named.append(QString::number(l));
            return invalid(QStringLiteral("timelineZoom takes a level of timeline.zoomLevels: %1 (1 is Fit)").arg(named.join(QStringLiteral(", "))));
        }
        timeline_->setZoom(int(*level));
        return done();
    }
    if (action == "timelineFit") { timeline_->zoomFit(); return done(); }
    // = and - (and Ctrl+= / Ctrl+-): one level closer or farther.
    // The viewport stays where it is (W19).
    if (action == "zoomIn") { timeline_->zoomIn(); return done(); }
    if (action == "zoomOut") { timeline_->zoomOut(); return done(); }
    // S and the transport's Skim toggle; N.
    if (action == "setSkimming" || action == "setSnapping") {
        if (!params["on"].isBool()) return invalid(action + QStringLiteral(" takes on true|false"));
        if (action == "setSkimming") timeline_->setSkimming(params["on"].toBool());
        else timeline_->setSnapping(params["on"].toBool());
        return done();
    }
    // [ and ]: the time selection's start or end at the playhead.
    if (action == "markSelection") {
        if (params["edge"] != "start" && params["edge"] != "end") return invalid("markSelection takes edge start|end");
        if (placing_) return state("Place or cancel the cover first");
        takeSkimForKey();
        timeline_->markSelectionEdge(params["edge"] == "start");
        return done();
    }
    // Up / Down: the previous or next edit point.
    if (action == "editPoint") {
        const auto delta = whole("delta");
        if (!delta || (*delta != 1 && *delta != -1)) return invalid("editPoint takes delta 1 or -1");
        takeSkimForKey();
        stepEditPoint(int(*delta));
        return done();
    }
    // A cover bar's body slid, or both its ends at once: one updateMask, as the drag's release.
    if (action == "retimeCover") {
        const auto* mask = selectedMask();
        if (!mask) return state("Select a cover first");
        const auto start = whole("start"), end = whole("end");
        if (!withinRecording(start, end)) return invalid("retimeCover takes start < end, in microseconds within the recording");
        stopPlayback();
        retimeCover(mask->id, *start, *end);
        return done();
    }
    // The "+n" chip: the Edits list at the covers the lanes hide.
    if (action == "moreCovers") {
        if (timeline_->inspect()["hiddenCovers"].toInt() <= 0) return state("Every cover is shown in the lanes");
        revealHiddenCovers();
        return done();
    }
    if (action == "viewerZoom") {
        const auto mode = params["mode"].toString();
        if (mode != "fit" && mode != "actual") return invalid("viewerZoom takes mode fit|actual");
        view_->setActualSize(mode == "actual");
        return done();
    }
    // A selected cut's Set to playhead (Starts or Ends).
    if (action == "setCutEdge") {
        const auto id = idParam("cutId");
        if (id.isEmpty() || !cutById(id)) return invalid("No cut has this ID");
        if (params["edge"] != "start" && params["edge"] != "end") return invalid("setCutEdge takes edge start|end");
        if (placing_) return state("Place or cancel the cover first");
        if (const auto why = cutEdgeToPlayhead(id, params["edge"] == "start"); !why.isEmpty()) return state(why);
        return done();
    }
    // The overview track: the zoomed timeline's view from a source time.
    if (action == "scrollTimeline") {
        const auto start = whole("start");
        if (!start || *start < 0 || *start > options_.durationUs) return invalid("scrollTimeline takes start in microseconds within the recording");
        if (timeline_->zoom() <= 1) return state("The timeline is at Fit: the whole recording is in view");
        timeline_->scrollTo(*start);
        return done();
    }
    if (action == "viewerPan") {
        if (!params["dx"].isDouble() || !params["dy"].isDouble() || !std::isfinite(params["dx"].toDouble()) || !std::isfinite(params["dy"].toDouble())) return invalid("viewerPan takes dx and dy in source pixels");
        if (!view_->actualSize()) return state("The picture is at Fit; set viewerZoom actual before panning");
        view_->panBy({params["dx"].toDouble(), params["dy"].toDouble()});
        return done();
    }
    if (action == "frameStep") {
        const auto delta = whole("delta");
        if (!delta || (*delta != 1 && *delta != -1)) return invalid("frameStep takes delta 1 or -1");
        stepFrames(int(*delta));
        return done();
    }
    if (action == "skim") {
        const auto time = whole("time");
        if (!time || *time < 0 || *time >= options_.durationUs) return invalid("skim takes a recording time in microseconds");
        if (playing_) return state("The timeline skims only while paused");
        if (!timeline_->skimming()) return state("Skimming is off (S); turn it on with setSkimming first");
        timeline_->skimAt(*time);
        return done();
    }
    if (action == "skimEnd") { timeline_->skimAt(std::nullopt); return done(); }
    if (action == "panel") {
        if (!params["open"].isBool()) return invalid("panel takes open true|false");
        if (!folded_) return state("The Edits panel is shown beside the viewer at this window size; make the window smaller to fold it");
        setPanelOpen(params["open"].toBool());
        return done();
    }
    if (action == "resize") {
        const auto width = whole("width"), height = whole("height");
        if (!width || !height || *width < 200 || *height < 200 || *width > 16000 || *height > 16000) return invalid("resize takes width and height in pixels, 200 to 16000");
        resize(int(*width), int(*height));
        return done();
    }
    if (action == "setTrim") {
        const auto start = whole("start");
        const auto end = params["end"].isNull() || params["end"].isUndefined() ? std::optional<qint64>(options_.durationUs) : whole("end");
        if (!withinRecording(start, end)) return invalid("setTrim takes start < end (end null: the recording's end), in microseconds within the recording");
        stopPlayback();
        proposeTrim(*start, *end);
        return done();
    }
    if (action == "format") {
        const QStringList wanted{params["format"].toString(), params["codec"].toString()};
        for (auto* entry : formats_->actions()) if (entry->data().toStringList() == wanted) { entry->trigger(); return done(); }
        return invalid("This output format is not offered");
    }
    return invalid("Unknown review action");
}

// ---- frames and playback ----
void ReviewWindow::requestFrame() {
    if (frameTimer_.interval() > 0) { frameTimer_.start(); return; }
    askFrame();
}
void ReviewWindow::askFrame() {
    if (!frames_ || playing_) return;  // playback draws its own frames
    const qint64 at = std::min<qint64>(edgePreview_ ? *edgePreview_ : playhead_, std::max<qint64>(0, options_.durationUs - 1));
    // A cover edge dragged onto the playhead's frame is still decoded with its proposed span (P15).
    QString variant;
    if (edgeEdit_ && edgePreview_) for (const auto& m : std::as_const(edgeEdit_->masks)) variant += QStringLiteral("%1:%2-%3;").arg(m.id).arg(m.start).arg(m.end);
    const FrameAsk ask{at, edit_.revision, viewEdge(), variant};
    // Asked already and answered or on its way: nothing new to decode.
    if (frameAsked_ && frameAsked_->at == ask.at && frameAsked_->revision == ask.revision && frameAsked_->edge == ask.edge && frameAsked_->variant == ask.variant) return;
    // Answers carry only their time and revision: another picture for the
    // same time (100 %, a cover edge's proposed span) drops the one on its way (W4).
    if (frameAsked_ && frameAsked_->at == ask.at && frameAsked_->revision == ask.revision) frames_->cancel();
    frameAsked_ = ask;
    // A cover edge being dragged is previewed with its proposed span (W4).
    frames_->request(at, edgeEdit_ && edgePreview_ ? *edgeEdit_ : edit_, ask.edge);
}
void ReviewWindow::beginEdgePreview() {
    // F1: an edge drag shows its edge frame for the whole drag: any skim goes now.
    if (skimUs_ || view_->skimming()) {
        skimUs_.reset();
        skimAsked_.reset();
        skimmer_->cancel();
        skimReturn_.stop();
        view_->clearSkim();
    }
    if (!edgePreview_ || edgeHoldProposal_) edgeDragSeq_ = proposalSeq_;
    edgeHoldProposal_ = 0;
    edgeHoldTimer_.stop();
}
void ReviewWindow::endEdgePreview() {
    edgeHoldProposal_ = 0;
    edgeHoldTimer_.stop();
    if (!edgePreview_) return;
    edgePreview_.reset();
    edgeEdit_.reset();
    frameAsked_.reset();  // the edge frame may share the playhead's time but not its edit
    requestFrame();
}
// W20: Ctrl+Z during a drag cancels only that drag. True when one was cancelled.
bool ReviewWindow::cancelDrags() {
    if (view_->cancelGesture()) return true;
    if (!timeline_->dragging()) return false;
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(timeline_, &escape);
    return true;
}
void ReviewWindow::stopPlaceWait() {
    placeWaitTimer_.stop();
    loadingFrame_ = false;
    view_->setPlaceWaiting(false);
    refreshNote();
    placePendingEnter();
}
// An Enter given while the placement frame loaded places now (P12).
void ReviewWindow::placePendingEnter() {
    if (!enterPlacePending_) return;
    enterPlacePending_ = false;
    enterPlaceTimer_.stop();
    if (placing_) if (const QRect r = view_->defaultMaskRect(); !r.isEmpty()) placeCover(placing_->kind, r);
}
// A note's time starts when it is given (Q13): it never plays late.
void ReviewWindow::showNote(const QString& text, int ms) {
    transientNote_ = text;
    noteMs_ = ms;
    noteTimer_.start(ms);
    refreshNote();
}
// One short note in the tool row: what just happened, else "Loading frame…"
// while placing waits, else who has the arrows.
// The tool row's one message (R25): the placement prompt, else a frame error,
// else the blur warning, else a note (what just happened, the arrows' owner).
// A frame error comes first while placement waits for that frame (R18); a
// lower message waits for the row instead of being squeezed in.
void ReviewWindow::refreshNote() {
    if (!toolNote_) return;
    QString note = transientNote_;
    if (note.isEmpty() && view_->arrowsClaimed() && view_->hasFocus() && selectedMask()) note = QStringLiteral("Arrows move the box · Esc for frames");
    const bool waiting = placing_ && view_->placeWaiting();
    if (placing_) {
        const auto& span = placing_->span;
        // A blur's prompt carries its warning (Q13).
        const auto what = placing_->kind == "solid" ? QStringLiteral("Draw the redaction") : QStringLiteral("Draw the blur (may be reversible)");
        auto text = placing_->whole ? what + QStringLiteral(" · whole recording") : QStringLiteral("%1 · %2 – %3").arg(what, durationText(span.start), durationText(span.end));
        if (!placeHint_.isEmpty()) text = placeHint_;
        placeBanner_->setText(waiting && loadingFrame_ ? QStringLiteral("Loading the frame to draw on…") : text);
        placeBanner_->setGlyph(placing_->kind == "solid" ? QStringLiteral("redact") : QStringLiteral("blur"));
    }
    const bool error = !frameError_->text().isEmpty();
    const bool showError = error && (!placing_ || waiting);
    const bool showPlace = placing_ && !showError;
    // Notes come before the blur warning (Q13); the warning isn't repeated
    // while the side panel shows it beside the selected blur.
    const bool panelShowsBlur = !placing_ && selectedMask() && (!folded_ || (overlay_ && overlay_->isOpen()));
    const bool showNote = !note.isEmpty() && !showPlace && !showError;
    const bool showBlur = blurApplies() && !panelShowsBlur && !showPlace && !showError && !showNote;
    placeBanner_->setVisible(showPlace);
    frameError_->setVisible(showError);
    blurBanner_->setVisible(showBlur);
    toolNote_->setText(note);
    toolNote_->setVisible(showNote);
}
bool ReviewWindow::blurApplies() const {
    const auto* mask = selectedMask();
    return (placing_ && placing_->kind == "blur") || (!placing_ && mask && mask->kind == "blur");
}
// What an undo or redo changed, from the edit before it to the answer.
void ReviewWindow::noteUndo() {
    const auto span = [](qint64 a, qint64 b) { return QStringLiteral("%1–%2").arg(durationText(a), durationText(b)); };
    QString what, kind, id;
    recording::Span item;  // the span the step changed, scrolled into view
    for (const auto& cut : std::as_const(edit_.cuts)) {
        const auto old = std::find_if(undoBefore_.cuts.cbegin(), undoBefore_.cuts.cend(), [&](const RecordingCut& c) { return c.id == cut.id; });
        if (old == undoBefore_.cuts.cend() || old->start != cut.start || old->end != cut.end) { what = QStringLiteral("Cut ") + span(cut.start, cut.end); kind = QStringLiteral("cut"); id = cut.id; item = {cut.start, cut.end}; break; }
    }
    if (what.isEmpty()) for (const auto& mask : std::as_const(edit_.masks)) {
        const auto old = std::find_if(undoBefore_.masks.cbegin(), undoBefore_.masks.cend(), [&](const RecordingMask& m) { return m.id == mask.id; });
        if (old == undoBefore_.masks.cend() || old->toJson() != mask.toJson()) { what = maskNames_->name(mask) + QLatin1Char(' ') + span(mask.start, mask.end); kind = QStringLiteral("cover"); id = mask.id; item = {mask.start, mask.end}; break; }
    }
    if (what.isEmpty()) for (const auto& cut : std::as_const(undoBefore_.cuts))
        if (std::none_of(edit_.cuts.cbegin(), edit_.cuts.cend(), [&](const RecordingCut& c) { return c.id == cut.id; })) { what = QStringLiteral("Cut ") + span(cut.start, cut.end); break; }
    if (what.isEmpty()) for (const auto& mask : std::as_const(undoBefore_.masks))
        if (std::none_of(edit_.masks.cbegin(), edit_.masks.cend(), [&](const RecordingMask& m) { return m.id == mask.id; })) { what = maskNames_->name(mask) + QLatin1Char(' ') + span(mask.start, mask.end); break; }
    if (what.isEmpty() && (edit_.trimStart != undoBefore_.trimStart || edit_.trimEnd != undoBefore_.trimEnd)) what = QStringLiteral("Trim ") + span(edit_.trimStart, edit_.exportEnd(options_.durationUs));
    if (what.isEmpty()) return;
    showNote((undoWasUndo_ ? QStringLiteral("Undid: ") : QStringLiteral("Redid: ")) + what, 1500);
    // An item the step brought back (or changed) is selected; the playhead stays.
    if (kind == "cut") selectCut(id, false);
    else if (kind == "cover") selectCover(id, false);
    if (!kind.isEmpty()) timeline_->reveal(item);
}
void ReviewWindow::showFrameError(qint64 at, const QString& message) {
    // Never a silent old frame: say so over it until a refresh succeeds.
    debugLog("review: preview failed " + message);
    QString reason;
    const QFileInfo clip(options_.workingClip);
    if (!clip.exists()) reason = QStringLiteral("the private recording file is missing");
    else if (!clip.isReadable()) reason = QStringLiteral("the private recording file isn't readable; check its permissions");
    else if (message.startsWith("Cannot open") || message.startsWith("Cannot read") || message.startsWith("Reading")) reason = QStringLiteral("the private recording file couldn't be read; check that it is intact");
    else if (message.startsWith("The working clip has no video")) reason = QStringLiteral("the private recording has no readable video track");
    else if (message.startsWith("No frame")) reason = QStringLiteral("no video frame is available at this time; try a nearby frame");
    else reason = QStringLiteral("the video frame couldn't be decoded; try a nearby frame or retry the preview");
    // A retry asks again even for the same time.
    frameAsked_.reset();
    view_->setPreviewError(QStringLiteral("The frame at %1 couldn't be decoded (%2).").arg(durationText(at), reason));
}
void ReviewWindow::decodeFrameNow() {
    QImage image;
    qint64 actual = 0;
    const qint64 at = std::min<qint64>(edgePreview_ ? *edgePreview_ : playhead_, std::max<qint64>(0, options_.durationUs - 1));
    debugLog(QStringLiteral("review: frame at %1 (now)").arg(at));
    if (auto e = previewFrame(options_.workingClip, edit_, config_, at, viewEdge(), options_.canvas, image, &actual)) { showFrameError(at, e.message); return; }
    view_->setFrame(image, at);
    shownRevision_ = edit_.revision;
    if (!edgePreview_) { shownFor_ = at; shownActual_ = actual; }
}
void ReviewWindow::startPlayback(std::optional<qint64> fromUs, bool resume) {
    if (exporting_ || playing_ || kept_.empty()) return;
    const auto spans = kept_.spans();
    qint64 start = fromUs ? *fromUs : playhead_;
    // Not kept: from the next kept footage; at the last kept frame or past
    // the end, Play starts the kept footage again; a restart (zoom, speed, an
    // edit) stays paused there instead.
    const auto last = lastKeptFrame(0, options_.durationUs);
    if (const auto next = kept_.nextKept(start); next && (!last || *next < *last)) start = *next;
    else if (resume) return;
    else start = spans.first().start;
    if (placing_) cancelPlacement();
    edgePreview_.reset();
    skimUs_.reset();
    skimmer_->cancel();
    view_->clearSkim();
    view_->endLive();
    liveWaitRevision_.reset();
    // Playback draws over the asked frame: the next seek asks again, even for the same time.
    frameAsked_.reset();
    playing_ = true;
    view_->setPlaying(true);
    timeline_->setPlaying(true);
    refreshTransport();
    auto stop = std::make_shared<std::atomic_bool>(false);
    playStop_ = stop;
    auto ended = std::make_shared<std::atomic_bool>(false);
    playEnded_ = ended;
    QPointer<ReviewWindow> guard(this);
    const QString clip = options_.workingClip;
    const QSize canvas = options_.canvas;
    const auto edit = edit_;
    const auto kept = kept_;
    const double speed = std::max(0.1, speed_);
    const int edge = viewEdge();
    auto* thread = QThread::create([guard, stop, ended, clip, canvas, edit, kept, start, speed, edge] {
        QElapsedTimer clock;
        clock.start();
        qint64 heldMs = 0;  // time spent seeking between spans: the clock doesn't run on (R26)
        const qint64 outputStart = kept.toOutput(start);
        // Output time the wall clock has reached at this speed.
        const auto due = [&] { return outputStart + qint64((clock.elapsed() - heldMs) * 1000 * speed); };
        qint64 shownAt = -1;
        bool reachedEnd = true;
        // One input and decoder for the whole run: each kept span is sought to
        // at its start (a cut is never decoded through), never reopened (R26).
        FrameSequence frames;
        bool opened = false;
        for (const auto& span : kept.spans()) {
            if (span.end <= start) continue;
            if (stop->load()) { reachedEnd = false; break; }
            const qint64 from = std::max(start, span.start);
            const qint64 seekStarted = clock.elapsed();
            if (opened ? frames.seek(clip, from) : frames.open(clip, from)) { reachedEnd = false; break; }
            opened = true;
            heldMs += clock.elapsed() - seekStarted;
            QImage image;
            qint64 frameUs = 0;
            // Frames more than a frame behind the clock are decoded but not drawn,
            // unless nothing has been drawn for a quarter second.
            const auto wanted = [&](qint64 at) {
                if (at >= span.end) return true;
                const qint64 out = kept.toOutput(at);
                return out + 40000 >= due() || shownAt < 0 || out - shownAt >= 250000;
            };
            while (!stop->load() && frames.next(edit, edge, canvas, image, frameUs, nullptr, wanted)) {
                if (frameUs >= span.end) break;
                if (frameUs < from) continue;  // before the span (or the start): never shown
                const qint64 out = kept.toOutput(frameUs);
                while (!stop->load() && due() < out) QThread::msleep(4);
                if (stop->load()) break;
                shownAt = out;
                QMetaObject::invokeMethod(guard, [guard, stop, image, frameUs] {
                    if (!guard || !guard->playing_ || guard->playStop_ != stop) return;
                    guard->view_->setFrame(image, frameUs);
                    guard->shownFor_ = guard->shownActual_ = frameUs;
                    guard->playhead_ = frameUs;
                    guard->timeline_->setPlayhead(frameUs);
                    guard->refreshTransport();
                }, Qt::QueuedConnection);
            }
            if (stop->load()) { reachedEnd = false; break; }
        }
        QMetaObject::invokeMethod(guard, [guard, stop, reachedEnd] {
            if (!guard || guard->playStop_ != stop) return;
            guard->stopPlayback();
            // Stopped at the trim end: on the last kept frame.
            if (reachedEnd) if (const auto last = guard->lastKeptFrame(0, guard->options_.durationUs)) guard->seek(*last, false);
        }, Qt::QueuedConnection);
        ended->store(reachedEnd);  // set once the end is queued (inspect, tests)
    });
    playThread_ = thread;
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}
void ReviewWindow::stopPlayback() {
    // The run's queued frames and its end are dropped with its token.
    if (playStop_) playStop_->store(true);
    playStop_.reset();
    if (!playing_) return;
    playing_ = false;
    view_->setPlaying(false);
    timeline_->setPlaying(false);
    refreshTransport();
}

// ---- save, discard, close, keys ----
bool ReviewWindow::chooseCopy() {
    panel_->commitFields();  // P9
    if (exporting_ || placing_ || chooser_->isOpen() || !saveWaitingPath_.isEmpty()) return false;
    view_->commitNudge();
    panel_->flushStrength();
    // The copy holds the changes made before Save, or is not saved (W5).
    saveAwaits_ = proposalSeq_;
    saveRefusalMark_ = refusals_;
    const auto extension = config_.extension();
    FileDialog::Options options{FileDialog::Mode::Save, "Save a copy", QDir::homePath(), "recording." + extension, {{config_.format.toUpper(), {"*." + extension}}}};
    return chooser_->open(options, [this, extension](const FileDialog::Result& result) {
        if (result.failed()) { showError("Couldn't save a copy there: " + result.error, false); return; }
        if (!result.chosen() || exporting_) return;
        if (refusals_ != saveRefusalMark_) { showError(QStringLiteral("No copy was saved: a change wasn't applied. Check the edits, then save again."), false); return; }
        const auto& path = result.path;
        saveWaitingPath_ = path.endsWith("." + extension, Qt::CaseInsensitive) ? path : path + "." + extension;
        saveWait_.start();
        trySaveCopy();
    });
}
void ReviewWindow::trySaveCopy() {
    if (saveWaitingPath_.isEmpty() || answeredSeq_ < saveAwaits_) return;
    const QString path = std::exchange(saveWaitingPath_, QString());
    saveWait_.stop();
    emit saveCopyRequested(path, edit_.revision);
}
void ReviewWindow::cancelSaveCopy(const QString& why) {
    if (saveWaitingPath_.isEmpty()) return;
    saveWaitingPath_.clear();
    saveWait_.stop();
    showError(QStringLiteral("No copy was saved: %1. Check the edits, then save again.").arg(why), false);
}
void ReviewWindow::hideEvent(QHideEvent* e) {
    // Upload, cancel and a stop elsewhere hide the review: its chooser goes too
    // (minimizing is a spontaneous hide and leaves it open).
    if (!e->spontaneous()) {
        chooser_->cancel();
        saveWaitingPath_.clear();
        saveWait_.stop();
        if (discard_) answerDiscard(false);
        if (adviceDialog_) { adviceDialog_->close(); adviceDialog_.clear(); }
        if (overlay_->isOpen()) overlay_->dismiss();
    }
    QWidget::hideEvent(e);
}
bool ReviewWindow::eventFilter(QObject* watched, QEvent* event) {
    // P9: a press anywhere in the window outside the panel field being typed
    // in commits that field and hands the keys to the timeline before the
    // pressed control acts (Tab-only controls don't take focus by click).
    if (event->type() == QEvent::MouseButtonPress) {
        auto* target = qobject_cast<QWidget*>(watched);
        auto* focus = focusWidget();
        if (target && target->window() == this && focus && textField(focus) && panel_->isAncestorOf(focus) && target != focus && !focus->isAncestorOf(target) && !(focus->parentWidget() && focus->parentWidget()->isAncestorOf(target) && qobject_cast<QAbstractSpinBox*>(focus))) {
            panel_->commitFields();
            timeline_->setFocus(Qt::MouseFocusReason);
        }
    }
    if (watched == view_ && (event->type() == QEvent::Resize || event->type() == QEvent::Move)) placeBanners();
    if (watched == bannerArea_ && event->type() == QEvent::LayoutRequest) QMetaObject::invokeMethod(this, &ReviewWindow::placeBanners, Qt::QueuedConnection);
    // Delete and Backspace cut or remove only from the viewer and the timeline
    // (the Edits list handles its own rows; a field keeps its own keys).
    if (event->type() == QEvent::KeyPress && (watched == view_ || watched == timeline_)) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if ((key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace) && !(key->modifiers() & ~Qt::KeypadModifier) && !exporting_) {
            if (timeSelection_ && !placing_) { cutSelection(); return true; }
            if (selectedMask() && !placing_) { stopPlayback(); removeSelectedCover(); return true; }
            // Delete never restores a cut, and says so (W10).
            if (selectedKind_ == "cut") showNote(QStringLiteral("Delete doesn't restore a cut — use Restore or Ctrl+Z"), 2000);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
void ReviewWindow::keyPressEvent(QKeyEvent* e) {
    const auto modifiers = e->modifiers() & ~Qt::KeypadModifier;
    if (exporting_) { QWidget::keyPressEvent(e); return; }
    const bool plain = modifiers == Qt::NoModifier, shift = modifiers == Qt::ShiftModifier;
    // A text field or spin box keeps its own keys: no single-key action fires
    // while one has focus, even a key it passed on (W17).
    const bool typing = textField(focusWidget());
    if (typing && e->key() != Qt::Key_Return && e->key() != Qt::Key_Enter) { QWidget::keyPressEvent(e); return; }
    // Q10: these act at the skimmed frame when a skim is present.
    static const QSet<int> takesSkim{Qt::Key_I, Qt::Key_O, Qt::Key_BracketLeft, Qt::Key_BracketRight, Qt::Key_L, Qt::Key_J,
        Qt::Key_Left, Qt::Key_Right, Qt::Key_Comma, Qt::Key_Period, Qt::Key_Up, Qt::Key_Down};
    // With no live skim the key acts at the playhead, and that is what is shown (F2).
    if (takesSkim.contains(e->key()) && (plain || shift)) takeSkimForKey();
    switch (e->key()) {
    case Qt::Key_Space: if (plain) { if (playing_) stopPlayback(); else startPlayback(); return; } break;
    case Qt::Key_Escape:
        if (plain) {
            if (placing_) { cancelPlacement(); return; }
            if (timeSelection_ || !selectedKind_.isEmpty()) { clearSelection(); return; }
        }
        break;
    case Qt::Key_V: if (plain) { cancelPlacement(); return; } break;
    // Delete / Backspace from any focus but a field (a row handles its own first, P14).
    case Qt::Key_Delete: case Qt::Key_Backspace:
        if (plain && !placing_) {
            if (timeSelection_) { cutSelection(); return; }
            if (selectedMask()) { stopPlayback(); removeSelectedCover(); return; }
            if (selectedKind_ == "cut") showNote(QStringLiteral("Delete doesn't restore a cut — use Restore or Ctrl+Z"), 2000);
            return;
        }
        break;
    case Qt::Key_C: if (plain) { cutSelection(); return; } break;
    case Qt::Key_X: if (plain) { arm("solid"); return; } break;
    case Qt::Key_B: if (plain) { arm("blur"); return; } break;
    // S skims, N snaps (on by default, remembered for the session).
    case Qt::Key_S: if (plain && !typing) { timeline_->setSkimming(!timeline_->skimming()); return; } break;
    case Qt::Key_N: if (plain && !typing) { timeline_->setSnapping(!timeline_->snapping()); return; } break;
    // Shift+Z fits the timeline; = and - (plain or with Ctrl) zoom at the playhead.
    case Qt::Key_Z: if (shift && !typing) { timeline_->zoomFit(); return; } break;
    case Qt::Key_Equal: case Qt::Key_Plus:
        if (!typing && (plain || shift || modifiers == Qt::ControlModifier || modifiers == (Qt::ControlModifier | Qt::ShiftModifier))) { timeline_->zoomIn(); return; }
        break;
    case Qt::Key_Minus:
        if (!typing && (plain || modifiers == Qt::ControlModifier)) { timeline_->zoomOut(); return; }
        break;
    // [ and ] mark the time selection at the playhead from any focus but a text field (E4).
    case Qt::Key_BracketLeft: case Qt::Key_BracketRight:
        // They always mark a time selection (Q7); a cover's times are Shift+I / Shift+O.
        if (plain && !typing && !placing_) { timeline_->markSelectionEdge(e->key() == Qt::Key_BracketLeft); return; }
        break;
    case Qt::Key_I: case Qt::Key_O:
        if (plain) { stopPlayback(); setTrimAt(e->key() == Qt::Key_I); return; }
        if (shift) { stopPlayback(); setMaskAt(e->key() == Qt::Key_I); return; }
        break;
    // J steps back a second, K pauses, L plays (again: the next faster speed) (W12).
    // J mirrors L (P17): playing above 1× it steps the speed down, at 1× it
    // pauses; paused it steps back a second.
    case Qt::Key_J:
        if (plain) {
            if (!playing_) { stepTime(-secondUs); return; }
            if (speed_ > 1.0) {
                const auto speeds = speeds_->actions();
                for (auto it = speeds.crbegin(); it != speeds.crend(); ++it) if ((*it)->data().toDouble() < speed_ && (*it)->data().toDouble() >= 1.0) { (*it)->trigger(); break; }
                showNote(QStringLiteral("Speed %1").arg(speeds_->checkedAction() ? speeds_->checkedAction()->text() : QStringLiteral("1×")), 1000);
            } else {
                stopPlayback();
                showNote(QStringLiteral("Paused"), 1000);
            }
            return;
        }
        break;
    // K pauses and goes back to 1× (Q19).
    case Qt::Key_K:
        if (plain) {
            stopPlayback();
            for (auto* action : speeds_->actions()) if (action->data().toDouble() == 1.0 && !action->isChecked()) action->trigger();
            return;
        }
        break;
    case Qt::Key_L:
        if (plain) {
            if (!playing_) { startPlayback(); return; }
            const auto speeds = speeds_->actions();
            for (auto* action : speeds) if (action->data().toDouble() > speed_) { action->trigger(); break; }
            showNote(QStringLiteral("Speed %1").arg(speeds_->checkedAction() ? speeds_->checkedAction()->text() : QStringLiteral("1×")), 1000);
            return;
        }
        break;
    case Qt::Key_Comma: if (plain) { stepFrames(-1); return; } break;
    case Qt::Key_Period: if (plain) { stepFrames(1); return; } break;
    // One frame; Shift one second (E1).
    case Qt::Key_Left: case Qt::Key_Right:
        if (plain) { stepFrames(e->key() == Qt::Key_Left ? -1 : 1); return; }
        if (shift) { stepTime((e->key() == Qt::Key_Left ? -1 : 1) * secondUs); return; }
        break;
    // The previous or next edit point: a trim end, a cut's or a cover's edge.
    case Qt::Key_Up: case Qt::Key_Down:
        if (plain) { stepEditPoint(e->key() == Qt::Key_Up ? -1 : 1); return; }
        break;
    case Qt::Key_Return: case Qt::Key_Enter:
        // A plain widget has no default button, so Enter is handled here by
        // focus and state: a field keeps it, a focused button presses itself,
        // and while placing it places the centred cover. It never uploads
        // unless Upload itself has focus (Ctrl+Enter uploads from anywhere),
        // and a selected cut is restored only by its Restore button or row (E3).
        if (e->isAutoRepeat()) return;  // a held Enter places or presses once
        if (plain) {
            auto* focus = focusWidget();
            if (textField(focus)) return;
            if (auto* button = qobject_cast<QAbstractButton*>(focus); button && isAncestorOf(button)) { if (button->isEnabled()) button->click(); return; }
            if (placing_) {
                // P12: on the frame it is meant for; while that frame loads, Enter waits (at most 1 s).
                if (view_->placeWaiting()) { enterPlacePending_ = true; enterPlaceTimer_.start(); return; }
                if (const QRect r = view_->defaultMaskRect(); !r.isEmpty()) placeCover(placing_->kind, r);
                return;
            }
            return;
        }
        break;
    case Qt::Key_Home: case Qt::Key_End:
        if (plain) {
            const bool home = e->key() == Qt::Key_Home;
            if (const auto* mask = selectedMask()) { seekCoverEdge(mask->id, home); return; }
            if (const auto at = home ? firstKeptFrame(0, options_.durationUs) : lastKeptFrame(0, options_.durationUs)) seek(*at);
            return;
        }
        break;
    default: break;
    }
    QWidget::keyPressEvent(e);
}
// Up / Down: the playhead to the nearest edit point before or after it.
void ReviewWindow::stepEditPoint(int direction) {
    QList<qint64> points;
    if (const auto first = firstKeptFrame(0, options_.durationUs)) points.append(*first);
    if (const auto last = lastKeptFrame(0, options_.durationUs)) points.append(*last);
    // A cut's boundary from its kept side: the last kept frame before it, the
    // first after it (Q21); nothing inside a cut is an edit point.
    for (const auto& cut : std::as_const(edit_.cuts)) {
        if (const auto before = lastKeptFrame(0, cut.start)) points << *before;
        if (const auto after = firstKeptFrame(cut.end, options_.durationUs)) points << *after;
    }
    for (const auto& mask : std::as_const(edit_.masks)) points << mask.start << mask.end;
    points.removeIf([this](qint64 t) { return !kept_.kept(t); });
    std::optional<qint64> best;
    for (const qint64 t : std::as_const(points)) {
        if (t < 0 || t >= options_.durationUs) continue;
        if (direction > 0 && t > playhead_ && (!best || t < *best)) best = t;
        if (direction < 0 && t < playhead_ && (!best || t > *best)) best = t;
    }
    if (best) seek(*best);
}
// The "+n" chip (A12): the Edits list, scrolled to the first cover the lanes hide.
void ReviewWindow::revealHiddenCovers() {
    QString id;
    for (const auto& mask : std::as_const(edit_.masks)) if (timeline_->coverBarRect(mask.id).isEmpty()) { id = mask.id; break; }
    if (id.isEmpty()) return;
    if (folded_) setPanelOpen(true);
    auto* row = panel_->rowFor(QStringLiteral("cover"), id);
    if (!row) return;
    for (auto* up = row->parentWidget(); up; up = up->parentWidget())
        if (auto* scroll = qobject_cast<QScrollArea*>(up)) { scroll->ensureWidgetVisible(row); break; }
    row->setFocus(Qt::OtherFocusReason);
}
void ReviewWindow::closeEvent(QCloseEvent* e) {
    // Closing the window would discard the recording: ask first, as Discard… does.
    if (!closing_ && !exporting_) { e->ignore(); askDiscard(); return; }
    e->accept();
}
void ReviewWindow::askDiscard() {
    if (exporting_ || closing_) return;
    if (discard_) { discard_->raise(); discard_->activateWindow(); return; }
    stopPlayback();
    view_->commitNudge();
    auto* box = Ui::confirm(this, QStringLiteral("Discard recording"),
        QStringLiteral("Discard this recording and its edits?\nThe recording, its trim, cuts and covers are deleted. Nothing has been uploaded; copies you saved stay."),
        QStringLiteral("Discard recording"), QStringLiteral("Keep reviewing"), true);
    auto* keep = box->button(QMessageBox::Cancel);
    keep->setObjectName("keepReviewing");
    box->setWindowModality(Qt::WindowModal);
    box->setAttribute(Qt::WA_DeleteOnClose);
    connect(box, &QMessageBox::finished, this, [this, box] {
        const bool discard = box->clickedButton() == box->findChild<QPushButton*>("confirmDiscard");
        if (discard_ == box) discard_ = nullptr;
        if (!discard || closing_) return;
        closing_ = true;
        emit cancelRequested();
    });
    discard_ = box;
    box->open();
    keep->setFocus();
}
void ReviewWindow::answerDiscard(bool discard) {
    if (!discard_) return;
    auto* box = discard_.data();
    if (auto* button = box->findChild<QPushButton*>(discard ? "confirmDiscard" : "keepReviewing")) button->click();
    else box->reject();
}
}
