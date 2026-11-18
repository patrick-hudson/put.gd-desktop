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
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QShortcut>
#include <QThread>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace putgd::record {
namespace {
constexpr int handleSize = 8;
constexpr qint64 secondUs = 1000000;
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
// The card over the frame when a preview could not be decoded.
class ErrorCard final : public QWidget {
public:
    using QWidget::QWidget;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Theme::mediaScrim());
        painter.drawRoundedRect(QRectF(rect()), 8, 8);
    }
};
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

// #1613: a banner along the viewer's top edge: the cover placement ("Draw the
// redaction · 0:08.2 – 0:12.6" with Cancel and its Esc key) or the blur
// warning (warningSurface / warning). It takes space from the well, never
// covers the picture.
class ViewerBanner final : public QWidget {
public:
    enum class Tone { Info, Warning };
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
    QSize sizeHint() const override { return {400, std::max(Ui::scaled(32), fontMetrics().height() + 14)}; }
    QSize minimumSizeHint() const override { return {120, sizeHint().height()}; }
protected:
    void resizeEvent(QResizeEvent*) override { place(); }
    void paintEvent(QPaintEvent*) override {
        const auto& c = Theme::current();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(tone_ == Tone::Warning ? c.warningSurface : c.surface);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), Ui::Metric::radius, Ui::Metric::radius);
        const int side = Ui::scaled(16), pad = reviewSpace(Ui::Space::m);
        const QColor ink = tone_ == Tone::Warning ? c.warning : c.text;
        const QRect glyph(pad, (height() - side) / 2, side, side);
        if (tone_ == Tone::Warning) Ui::icon("alert", c.warning).paint(&painter, glyph);
        else if (!glyph_.isEmpty()) Ui::icon(glyph_).paint(&painter, glyph);
        int right = width() - pad;
        if (action_) {
            right = action_->geometry().left() - reviewSpace(Ui::Space::s);
            if (!key_.isEmpty()) {
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
    void place() {
        if (!action_) return;
        const int pad = reviewSpace(Ui::Space::m);
        const int keyWidth = key_.isEmpty() ? 0 : QFontMetrics(Ui::monoFont(Ui::Type::caption)).horizontalAdvance(key_) + 10 + reviewSpace(Ui::Space::s);
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
    errorCard_ = new ErrorCard(this);
    errorCard_->setObjectName("previewError");
    auto* row = new QHBoxLayout(errorCard_);
    row->setContentsMargins(reviewSpace(Ui::Space::l), reviewSpace(Ui::Space::s), reviewSpace(Ui::Space::s), reviewSpace(Ui::Space::s));
    row->setSpacing(reviewSpace(Ui::Space::m));
    errorText_ = new QLabel(errorCard_);
    errorText_->setObjectName("previewErrorText");
    errorText_->setWordWrap(true);
    errorText_->setTextFormat(Qt::PlainText);
    QPalette onMedia = errorText_->palette();
    onMedia.setColor(QPalette::WindowText, Theme::onMedia());
    errorText_->setPalette(onMedia);
    retry_ = new QPushButton(QStringLiteral("Retry preview"), errorCard_);
    retry_->setObjectName("retryPreview");
    retry_->setToolTip(QStringLiteral("Decode the frame at the playhead again"));
    row->addWidget(errorText_, 1);
    row->addWidget(retry_, 0, Qt::AlignVCenter);
    connect(retry_, &QPushButton::clicked, this, &FrameView::retryRequested);
    errorCard_->hide();
    connect(&Theme::Notifier::instance(), &Theme::Notifier::changed, this, qOverload<>(&QWidget::update));
}
void FrameView::setFrame(const QImage& image, qint64 timeUs) { frame_ = image; timeUs_ = timeUs; setPreviewError({}); announce(); update(); }
void FrameView::setSkim(const QImage& image, qint64 timeUs) { skim_ = image; skimUs_ = timeUs; update(); }
void FrameView::clearSkim() { if (skim_.isNull()) return; skim_ = {}; update(); }
void FrameView::armCreate(const QString& kind) { armed_ = kind; setCursor(Qt::CrossCursor); announce(); update(); }
void FrameView::disarm() {
    if (drag_ == Drag::Create) drag_ = Drag::None;
    armed_.clear();
    unsetCursor();
    announce();
    update();
}
void FrameView::setPreviewError(const QString& message) {
    error_ = message;
    announce();
    if (message.isEmpty()) { errorCard_->hide(); update(); return; }
    // A frame still on screen is an earlier one: say which, as well as dimming it.
    const QString text = frame_.isNull() ? message : message + QStringLiteral("\nShowing an earlier frame (%1).").arg(durationText(timeUs_));
    errorText_->setText(text);
    errorText_->setAccessibleName(text);
    errorCard_->show();
    placeErrorCard();
    update();
}
void FrameView::placeErrorCard() {
    if (errorCard_->isHidden()) return;
    const QRect where = (frame_.isNull() ? area() : imageRect()).intersected(area()).adjusted(8, 8, -8, -8);
    if (where.isEmpty()) return;
    auto* row = static_cast<QBoxLayout*>(errorCard_->layout());
    const auto margins = row->contentsMargins();
    const int width = std::min(where.width(), Ui::scaled(640));
    const int inner = std::max(1, width - margins.left() - margins.right());
    const int buttonWidth = retry_->sizeHint().width();
    const int textWidth = inner - buttonWidth - row->spacing();
    const bool stacked = textWidth < errorText_->fontMetrics().horizontalAdvance(QStringLiteral("Showing an earlier frame"));
    row->setDirection(stacked ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    row->setAlignment(retry_, stacked ? Qt::AlignLeft : Qt::AlignVCenter);
    const int labelHeight = errorText_->heightForWidth(std::max(1, stacked ? inner : textWidth));
    const int contentHeight = stacked ? labelHeight + row->spacing() + retry_->sizeHint().height() : std::max(labelHeight, retry_->sizeHint().height());
    const int height = std::min(where.height(), contentHeight + margins.top() + margins.bottom());
    errorCard_->setGeometry(QRect(QPoint(where.center().x() - width / 2, where.center().y() - height / 2), QSize(width, height)));
}
void FrameView::resizeEvent(QResizeEvent* e) { QWidget::resizeEvent(e); clampPan(); placeErrorCard(); emit zoomChanged(); }
void FrameView::setMasks(const QList<RecordingMask>& masks, const QString& selected) {
    // A keyboard nudge not yet proposed keeps its place over the last answer.
    QRect pending;
    if (nudging_) for (const auto& m : std::as_const(masks_)) if (m.id == selected_ && m.id == selected) pending = m.rect;
    if (selected != selected_) commitNudge();
    maskNames_->observe(masks);
    masks_ = masks;
    selected_ = selected;
    if (!pending.isNull()) for (auto& m : masks_) if (m.id == selected_) m.rect = pending;
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
    placeErrorCard();
    update();
    emit zoomChanged();
}
void FrameView::setActualSize(bool actual) {
    if (actual_ == actual) return;
    actual_ = actual;
    pan_ = {};
    if (drag_ == Drag::Pan) drag_ = Drag::None;
    placeErrorCard();
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
    // Arrows nudge a selected cover or pan at 100 %; otherwise they step time
    // in the window, so the frame only claims them when it will use them.
    if (e->type() == QEvent::ShortcutOverride) {
        const auto* key = static_cast<QKeyEvent*>(e);
        const bool arrow = key->key() == Qt::Key_Left || key->key() == Qt::Key_Right || key->key() == Qt::Key_Up || key->key() == Qt::Key_Down;
        if (arrow && !(key->modifiers() & Qt::AltModifier) && (!selected_.isEmpty() || actual_)) e->accept();
    }
    return QWidget::event(e);
}
void FrameView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), Theme::current().well);
    painter.setClipRect(area());
    const auto target = imageRect();
    Ui::paintImageShadow(painter, target);
    painter.fillRect(target, Qt::black);
    const bool skimming = !skim_.isNull();
    if (skimming) painter.drawImage(target, skim_);
    else if (!frame_.isNull()) painter.drawImage(target, frame_);
    // An earlier frame under a preview error is dimmed so it never reads as current.
    if (stale() && !skimming) painter.fillRect(target, QColor(0, 0, 0, 140));
    const qint64 at = skimming ? skimUs_ : timeUs_;
    if (!playing_) for (const auto& mask : std::as_const(masks_)) {
        const bool active = mask.start <= at && at < mask.end;
        const bool selected = mask.id == selected_;
        if (!active && !selected && mask.id != hovered_) continue;
        const auto r = toView(mask.rect);
        dualOutline(painter, r, selected ? mediaAccent : active ? QColor(255, 255, 255, 230) : QColor(255, 255, 255, 150), selected ? 2 : 1, active ? Qt::SolidLine : Qt::DashLine);
        if (selected) {
            // Handles at the corners and edge middles; the bottom-right one resizes.
            painter.setPen(QPen(QColor(0, 0, 0, 170), 1));
            painter.setBrush(mediaAccent);
            for (const QPointF& p : {r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight(), QPointF(r.center().x(), r.top()), QPointF(r.center().x(), r.bottom()), QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y())})
                painter.drawRect(QRectF(p.x() - handleSize / 2.0, p.y() - handleSize / 2.0, handleSize, handleSize));
        }
    }
    if (drag_ == Drag::Create) {
        // The area being dragged, clipped to the frame like the cover it becomes.
        const auto pending = QRectF(anchor_, current_).normalized().intersected(target);
        painter.setPen(Qt::NoPen);
        painter.setBrush(armed_ == "solid" ? QBrush(QColor(0, 0, 0, 200)) : QBrush(QColor(255, 255, 255, 60)));
        painter.drawRect(pending);
        dualOutline(painter, pending, mediaAccent, 1.5, Qt::DashLine);
    }
    painter.setClipping(false);
    if (Ui::keyboardFocus(this)) { painter.setPen(QPen(Theme::current().accent, 2)); painter.setBrush(Qt::NoBrush); painter.drawRect(QRectF(rect()).adjusted(1, 1, -1, -1)); }
}
void FrameView::focusInEvent(QFocusEvent* e) { QWidget::focusInEvent(e); update(); }
void FrameView::focusOutEvent(QFocusEvent* e) { commitNudge(); QWidget::focusOutEvent(e); update(); }
void FrameView::keyPressEvent(QKeyEvent* e) {
    auto mask = std::find_if(masks_.begin(), masks_.end(), [&](const RecordingMask& m) { return m.id == selected_; });
    if (e->key() == Qt::Key_Escape && mask != masks_.end() && armed_.isEmpty()) { commitNudge(); selected_.clear(); announce(); emit maskSelected({}); update(); return; }
    QPoint step;
    switch (e->key()) {
    case Qt::Key_Left: step = {-1, 0}; break;
    case Qt::Key_Right: step = {1, 0}; break;
    case Qt::Key_Up: step = {0, -1}; break;
    case Qt::Key_Down: step = {0, 1}; break;
    default: QWidget::keyPressEvent(e); return;
    }
    if (e->modifiers() & Qt::AltModifier) { QWidget::keyPressEvent(e); return; }
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
        // Resize from the bottom-right corner, as the grip does.
        r.setWidth(std::clamp(r.width() + step.x(), 2, std::max(2, all.right() + 1 - r.left())));
        r.setHeight(std::clamp(r.height() + step.y(), 2, std::max(2, all.bottom() + 1 - r.top())));
    } else {
        r.translate(step);
        r.moveLeft(std::clamp(r.left(), 0, std::max(0, all.width() - r.width())));
        r.moveTop(std::clamp(r.top(), 0, std::max(0, all.height() - r.height())));
    }
    mask->rect = r;
    announce();
    nudge_.start();
    update();
}
void FrameView::commitNudge() {
    if (!nudging_) return;
    nudging_ = false;
    nudge_.stop();
    for (const auto& mask : std::as_const(masks_)) if (mask.id == selected_ && mask.rect != nudgeFrom_) emit maskMoved(mask.id, mask.rect);
}
void FrameView::cancelNudge() {
    if (!nudging_) return;
    nudging_ = false;
    nudge_.stop();
    for (auto& mask : masks_) if (mask.id == selected_) mask.rect = nudgeFrom_;
    announce();
    update();
}
void FrameView::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    commitNudge();
    const auto p = e->position();
    if (!armed_.isEmpty()) { drag_ = Drag::Create; anchor_ = p; current_ = p; update(); return; }
    const RecordingMask* nearest = nullptr;
    qreal distance = std::numeric_limits<qreal>::max();
    for (const auto& mask : std::as_const(masks_)) {
        if (mask.id != selected_ && !(mask.start <= timeUs_ && timeUs_ < mask.end)) continue;
        const auto corner = toView(mask.rect).bottomRight();
        if (!QRectF(corner - QPointF(16, 16), QSizeF(32, 32)).contains(p)) continue;
        const auto delta = corner - p;
        const auto squared = delta.x() * delta.x() + delta.y() * delta.y();
        if (squared < distance || (squared == distance && mask.id == selected_)) { nearest = &mask; distance = squared; }
    }
    if (nearest) {
        selected_ = nearest->id; original_ = nearest->rect;
        drag_ = Drag::Resize; anchor_ = p;
        announce(); emit maskSelected(selected_); update(); return;
    }
    for (auto it = masks_.rbegin(); it != masks_.rend(); ++it) {
        if (it->id != selected_ && !(it->start <= timeUs_ && timeUs_ < it->end)) continue;
        if (toView(it->rect).contains(p)) { selected_ = it->id; announce(); emit maskSelected(it->id); drag_ = Drag::Move; anchor_ = p; original_ = it->rect; update(); return; }
    }
    if (actual_) { drag_ = Drag::Pan; anchor_ = p; panFrom_ = pan_; setCursor(Qt::ClosedHandCursor); return; }
    if (!selected_.isEmpty()) {
        selected_.clear();
        announce();
        emit maskSelected({});
        update();
    }
}
void FrameView::leaveEvent(QEvent* e) {
    hovered_.clear();
    update();
    QWidget::leaveEvent(e);
}
void FrameView::mouseMoveEvent(QMouseEvent* e) {
    QString hovered;
    for (auto it = masks_.crbegin(); it != masks_.crend(); ++it) if ((it->start <= timeUs_ && timeUs_ < it->end) && toView(it->rect).contains(e->position())) { hovered = it->id; break; }
    if (hovered_ != hovered) { hovered_ = hovered; update(); }
    if (drag_ == Drag::None) return;
    const auto p = e->position();
    if (drag_ == Drag::Create) { current_ = p; update(); return; }
    if (drag_ == Drag::Pan) { pan_ = panFrom_ - (p - anchor_) * devicePixelRatioF(); clampPan(); update(); return; }
    const QPointF delta = toCanvas(p) - toCanvas(anchor_);
    auto mask = std::find_if(masks_.begin(), masks_.end(), [&](const RecordingMask& m) { return m.id == selected_; });
    if (mask == masks_.end()) return;
    const QRect all = bounds();
    if (drag_ == Drag::Move) { QRect moved = original_.translated(delta.toPoint()); moved.moveLeft(std::clamp(moved.left(), 0, std::max(0, all.width() - moved.width()))); moved.moveTop(std::clamp(moved.top(), 0, std::max(0, all.height() - moved.height()))); mask->rect = moved; }
    else { QRect resized = original_; resized.setWidth(std::max(2, original_.width() + int(delta.x()))); resized.setHeight(std::max(2, original_.height() + int(delta.y()))); mask->rect = resized.intersected(all); }
    announce();
    update();
}
void FrameView::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || drag_ == Drag::None) return;
    const auto p = e->position();
    if (drag_ == Drag::Pan) { drag_ = Drag::None; unsetCursor(); return; }
    if (drag_ == Drag::Create) {
        const QRect r = QRectF(toCanvas(anchor_), toCanvas(p)).normalized().toAlignedRect().intersected(bounds());
        const auto kind = armed_;
        drag_ = Drag::None;
        update();
        if (r.width() >= 2 && r.height() >= 2) emit maskCreated(kind, r);
        return;
    }
    drag_ = Drag::None;
    for (const auto& mask : std::as_const(masks_)) if (mask.id == selected_ && mask.rect != original_) emit maskMoved(mask.id, mask.rect);
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
    tools->addStretch(1);
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
    // Banners take space from the well along its top edge, never over the picture.
    auto* bannerArea = new Ui::Surface("well", {}, this);
    bannerArea->setObjectName("viewerBanners");
    auto* banners = new QVBoxLayout(bannerArea);
    banners->setContentsMargins(reviewSpace(Ui::Space::m), reviewSpace(Ui::Space::s), reviewSpace(Ui::Space::m), 0);
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
    placeBanner_ = new ViewerBanner(ViewerBanner::Tone::Info, bannerArea);
    placeBanner_->setObjectName("placementBanner");
    auto* cancelPlace = placeBanner_->setAction(QStringLiteral("Cancel"), QStringLiteral("Esc"));
    cancelPlace->setObjectName("cancelPlacement");
    cancelPlace->setToolTip(QStringLiteral("Cancel placing the cover (Esc)"));
    connect(cancelPlace, &QPushButton::clicked, this, &ReviewWindow::cancelPlacement);
    banners->addWidget(placeBanner_);
    blurBanner_ = new ViewerBanner(ViewerBanner::Tone::Warning, bannerArea);
    blurBanner_->setObjectName("blurWarning");
    blurBanner_->setText(QStringLiteral("Blur may be reversible. Use Redact for private information."));
    banners->addWidget(blurBanner_);
    // Errors, results and format advice with their actions; shown only when there are any.
    statusRow_ = new QWidget(bannerArea);
    statusRow_->setObjectName("statusRow");
    statusLines_ = new QVBoxLayout(statusRow_);
    statusLines_->setContentsMargins(0, 0, 0, 0);
    statusLines_->setSpacing(reviewSpace(Ui::Space::xs));
    statusRow_->hide();
    banners->addWidget(statusRow_);
    bannerArea->hide();
    column->addWidget(bannerArea);
    view_ = new FrameView(this, maskNames_);
    view_->setObjectName("frame");
    view_->setContentsMargins(reviewSpace(Ui::Space::m), reviewSpace(Ui::Space::s), reviewSpace(Ui::Space::m), reviewSpace(Ui::Space::s));
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
    play_->setPopupMode(QToolButton::MenuButtonPopup);
    play_->setFixedWidth(play_->height() + Ui::Metric::splitZone);
    speedMenu_ = new QMenu(play_);
    speedMenu_->setObjectName("speedMenu");
    speeds_ = new QActionGroup(speedMenu_);
    for (const auto& [label, factor] : std::initializer_list<std::pair<const char*, double>>{{"0.5×", 0.5}, {"1×", 1.0}, {"2×", 2.0}, {"4×", 4.0}}) {
        auto* action = speedMenu_->addAction(QString::fromUtf8(label));
        action->setCheckable(true);
        action->setData(factor);
        action->setChecked(factor == 1.0);
        speeds_->addAction(action);
    }
    play_->setMenu(speedMenu_);
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
    timelineZoom_ = tool("Fit", "zoom", "Zoom the timeline", "Zoom the timeline", transport_);
    timelineZoom_->setObjectName("timelineZoom");
    transport->addWidget(play_);
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

    // Thumbnails and skim: one worker each, never waited on.
    filmstrip_ = new Filmstrip(this);
    skimmer_ = new FrameSkimmer(this);
    if (!options_.workingClip.isEmpty()) {
        filmstrip_->open(options_.workingClip, options_.durationUs, options_.canvas);
        skimmer_->open(options_.workingClip, options_.canvas);
    }
    QPointer<Filmstrip> strip(filmstrip_);
    timeline_->setTileSource([strip](qint64 at) { return strip ? strip->tile(at) : QImage(); });
    connect(filmstrip_, &Filmstrip::tileReady, timeline_, &FilmstripTimeline::tilesChanged);
    tileTimer_.setSingleShot(true);
    tileTimer_.setInterval(0);
    connect(&tileTimer_, &QTimer::timeout, this, &ReviewWindow::requestTiles);
    connect(timeline_, &FilmstripTimeline::visibleSpanChanged, &tileTimer_, qOverload<>(&QTimer::start));
    connect(skimmer_, &FrameSkimmer::frameReady, this, [this](qint64 requested, qint64, const QImage& frame) {
        // Exact only as the answer to the request still current, at the edit's current revision.
        if (!skimUs_ || *skimUs_ != requested || playing_ || !skimAsked_ || *skimAsked_ != std::pair<qint64, quint64>(requested, edit_.revision)) return;
        skimAsked_.reset();
        view_->setSkim(frame, requested);
        timeline_->setSkimExact(true);
    });

    // Timeline gestures.
    // A ruler press scrubs and clears the selection; a strip click seeks, then clears it.
    connect(timeline_, &FilmstripTimeline::scrubStarted, this, [this] { stopPlayback(); if (timeSelection_ || !selectedKind_.isEmpty()) clearSelection(); });
    connect(timeline_, &FilmstripTimeline::seekRequested, this, [this](qint64 at) { seek(at); });
    connect(timeline_, &FilmstripTimeline::selectionCleared, this, [this] { if (timeSelection_ || !selectedKind_.isEmpty()) clearSelection(); });
    connect(timeline_, &FilmstripTimeline::timeSelected, this, &ReviewWindow::selectTime);
    connect(timeline_, &FilmstripTimeline::cutSelected, this, [this](const QString& id) { selectCut(id, false); });
    connect(timeline_, &FilmstripTimeline::coverSelected, this, [this](const QString& id) { selectCover(id); });
    connect(timeline_, &FilmstripTimeline::trimEdgeDragged, this, [this](bool start, qint64 at) {
        stopPlayback();
        edgePreview_ = start ? at : previousFrame(at);
        frameTimer_.start();
    });
    connect(timeline_, &FilmstripTimeline::editEdgeDragged, this, [this](const QString&, const QString&, bool start, qint64 at) {
        stopPlayback();
        edgePreview_ = start ? at : previousFrame(at);
        frameTimer_.start();
    });
    connect(timeline_, &FilmstripTimeline::edgeDragFinished, this, [this] { edgePreview_.reset(); frameTimer_.start(); });
    connect(timeline_, &FilmstripTimeline::trimProposed, this, &ReviewWindow::proposeTrim);
    connect(timeline_, &FilmstripTimeline::cutAdjusted, this, &ReviewWindow::adjustCut);
    connect(timeline_, &FilmstripTimeline::coverRetimed, this, &ReviewWindow::retimeCover);
    connect(timeline_, &FilmstripTimeline::skimMoved, this, [this](qint64 at) {
        if (playing_) return;
        skimUs_ = at;
        skimAsked_.reset();
        skimmer_->cancel();
        const auto tile = filmstrip_->tile(at);
        if (!tile.isNull()) view_->setSkim(tile, at);
    });
    connect(timeline_, &FilmstripTimeline::skimRested, this, [this](qint64 at) {
        if (playing_ || !skimUs_ || *skimUs_ != at) return;
        askSkimFrame(at);
    });
    connect(timeline_, &FilmstripTimeline::skimLeft, this, [this] { skimUs_.reset(); skimAsked_.reset(); skimmer_->cancel(); view_->clearSkim(); });
    connect(timeline_, &FilmstripTimeline::zoomChanged, this, [this] { refreshTransport(); });

    // Panel.
    connect(panel_, &ReviewPanel::cutRequested, this, &ReviewWindow::cutSelection);
    connect(panel_, &ReviewPanel::coverRequested, this, &ReviewWindow::arm);
    connect(panel_, &ReviewPanel::restoreCut, this, &ReviewWindow::restoreCut);
    connect(panel_, &ReviewPanel::playAcross, this, &ReviewWindow::playAcross);
    connect(panel_, &ReviewPanel::seekCoverEdge, this, &ReviewWindow::seekCoverEdge);
    connect(panel_, &ReviewPanel::coverEdgeToPlayhead, this, [this](const QString& id, bool start) { stopPlayback(); setMaskAt(start, id); });
    connect(panel_, &ReviewPanel::coverEdgeEdited, this, [this](const QString& id, bool start, qint64 at) {
        updateCover(id, [&](QJsonObject& mask) { mask[start ? "start" : "end"] = double(at); });
    });
    connect(panel_, &ReviewPanel::coverKindChanged, this, &ReviewWindow::setCoverKind);
    connect(panel_, &ReviewPanel::coverStrengthChanged, this, [this](const QString& id, int strength) {
        updateCover(id, [&](QJsonObject& mask) { mask["strength"] = strength; });
    });
    connect(panel_, &ReviewPanel::editActivated, this, [this](const QString& kind, const QString& id) { selectEdit(kind, id); });
    connect(panel_, &ReviewPanel::resetTrim, this, [this] { propose(QJsonArray{QJsonObject{{"op", "resetTrim"}}}); });
    connect(panel_, &ReviewPanel::removeCover, this, [this](const QString& id) { propose(QJsonArray{QJsonObject{{"op", "removeMask"}, {"maskId", id}}}); });
    connect(overlay_, &EditsOverlay::closed, this, [this] { editsToggle_->setChecked(false); view_->setRightInset(0); });
    connect(editsToggle_, &QToolButton::clicked, this, [this] { setPanelOpen(!overlay_->isOpen()); });

    // Viewer.
    connect(view_, &FrameView::retryRequested, this, [this] { frameTimer_.stop(); refreshFrame(); });
    connect(view_, &FrameView::maskCreated, this, [this](const QString& kind, const QRect& r) { placeCover(kind, r); });
    connect(view_, &FrameView::maskMoved, this, [this](const QString& id, const QRect& r) {
        updateCover(id, [&](QJsonObject& mask) { mask["rect"] = QJsonArray{r.x(), r.y(), r.width(), r.height()}; });
    });
    connect(view_, &FrameView::maskSelected, this, [this](const QString& id) { if (id.isEmpty()) clearSelection(); else selectCover(id, false); });
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
        frameTimer_.start();
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
        if (playing_) { const qint64 from = playhead_; stopPlayback(); startPlayback(from, true); }
    });
    connect(previousFrame_, &QToolButton::clicked, this, [this] { stepFrames(-1); });
    connect(nextFrame_, &QToolButton::clicked, this, [this] { stepFrames(1); });
    connect(timelineZoom_, &QToolButton::clicked, this, [this] { timeline_->setZoom(timeline_->zoom() >= 4 ? 1 : timeline_->zoom() * 2, playhead_); });
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
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Z), [this] { undoRedo(true); });
    shortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), [this] { undoRedo(false); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Y), [this] { undoRedo(false); });
    // Ctrl+Enter uploads as the button does; never with a menu open.
    // A held key never repeats into an upload.
    for (const auto key : {Qt::Key_Return, Qt::Key_Enter}) shortcut(QKeySequence(Qt::CTRL | key), [this] { if (!QApplication::activePopupWidget()) upload(); })->setAutoRepeat(false);
    // An upload waiting for its edit's answer gives up after 2 s and says so.
    uploadWait_.setSingleShot(true);
    uploadWait_.setInterval(2000);
    connect(&uploadWait_, &QTimer::timeout, this, [this] { cancelUpload(QStringLiteral("your last change wasn't confirmed in time")); });
    shortcut(QKeySequence(Qt::ALT | Qt::Key_Left), [this] { stepEdit(-1); });
    shortcut(QKeySequence(Qt::ALT | Qt::Key_Right), [this] { stepEdit(1); });
    view_->installEventFilter(this);
    timeline_->installEventFilter(this);
    // While an Upload waits, any input in this window or its dialogs cancels it.
    qApp->installEventFilter(this);

    frameTimer_.setSingleShot(true);
    frameTimer_.setInterval(qEnvironmentVariableIntValue("PUTGD_RECORD_REVIEW_DELAY_MS") > 0 ? qEnvironmentVariableIntValue("PUTGD_RECORD_REVIEW_DELAY_MS") : 60);
    connect(&frameTimer_, &QTimer::timeout, this, &ReviewWindow::refreshFrame);
    setConfig(config);
    setEdit({});
    timeline_->setFocus();
    resize(defaultSize().boundedTo(availableReviewSize()));
    refreshFold();
}
ReviewWindow::~ReviewWindow() {
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
    if (auto* banners = findChild<QWidget*>("viewerBanners"); banners && banners->isVisibleTo(this)) height += banners->sizeHint().height();
    return {width, std::max(480, height)};
}

// ---- time ----
qint64 ReviewWindow::frameUs() const { return std::max<qint64>(1, qRound64(1e6 / std::max(1, config_.fps))); }
qint64 ReviewWindow::frameAt(qint64 index) const { return std::llround(double(index) * 1e6 / std::max(1, config_.fps)); }
// Edges come from frame indexes (edge k = round(k·10⁶/fps)), never from adding
// a rounded frame length: 33,333 + 33,333 is not frame 2's 66,667.
qint64 ReviewWindow::frameIndex(qint64 us) const { return std::llround(double(us) * std::max(1, config_.fps) / 1e6); }
qint64 ReviewWindow::nextFrameEdge(qint64 at) const { return std::min(options_.durationUs, frameAt(frameIndex(at) + 1)); }
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
    if (skimUs_) { skimUs_.reset(); skimmer_->cancel(); }
    skimAsked_.reset();
    timeline_->setSkimExact(false);
    timeline_->skimAt(std::nullopt);  // the same time skimmed again asks again
    view_->clearSkim();
    refreshTransport();
    frameTimer_.start();
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
    refreshAll();
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
    // Its first visible frame; wholly outside kept footage, its start (paused,
    // "Excluded from output").
    if (seekToIt) {
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
    // A nudge not yet proposed is dropped, not sent; the step is then the resident's at this revision.
    view_->cancelNudge();
    if (undo ? !edit_.canUndo : !edit_.canRedo) return;
    stopPlayback();
    propose(QJsonArray{QJsonObject{{"op", undo ? "undo" : "redo"}}});
}
// Trim brackets dragged (and released), or recording.review setTrim.
void ReviewWindow::proposeTrim(qint64 start, qint64 end) {
    propose(QJsonArray{QJsonObject{{"op", "setTrim"}, {"start", double(start)}, {"end", end >= options_.durationUs ? QJsonValue(QJsonValue::Null) : QJsonValue(double(end))}}});
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
    if (timeSelection_) {
        const int fps = std::max(1, config_.fps);
        const qint64 start = frameAt(qint64(std::floor(double(timeSelection_->start) * fps / 1e6 + 1e-9)));
        const qint64 end = std::min(options_.durationUs, frameAt(qint64(std::ceil(double(timeSelection_->end) * fps / 1e6 - 1e-9))));
        placement = {kind, {std::min(start, timeSelection_->start), std::max(end, std::min(options_.durationUs, timeSelection_->end))}, false};
        if (placement.span.empty()) placement.span.end = nextFrameEdge(placement.span.start);
    }
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
    if (overlay_->isOpen()) overlay_->dismiss();
    view_->setFocus(Qt::OtherFocusReason);
    refreshAll();
}
void ReviewWindow::cancelPlacement() {
    if (!placing_) return;
    placing_.reset();
    view_->disarm();
    refreshAll();
}
void ReviewWindow::placeCover(const QString& kind, const QRect& rect) {
    if (!placing_ || exporting_) return;
    const auto span = placing_->span;
    QJsonObject mask{{"kind", kind}, {"rect", QJsonArray{rect.x(), rect.y(), rect.width(), rect.height()}}, {"start", double(span.start)}, {"end", double(span.end)}};
    if (kind == "blur") mask["strength"] = 16;
    placing_.reset();
    view_->disarm();
    timeSelection_.reset();
    timeline_->setTimeSelection(std::nullopt);
    // One command creates the box and its time; the new cover becomes the selection.
    propose(QJsonArray{QJsonObject{{"op", "addMask"}, {"mask", mask}}});
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
void ReviewWindow::seekCoverEdge(const QString& id, bool start) {
    const auto* mask = maskById(id);
    if (!mask) return;
    const auto at = start ? firstKeptFrame(mask->start, mask->end) : lastKeptFrame(mask->start, mask->end);
    if (at) seek(*at);
}
QString ReviewWindow::setTrimAt(bool start) {
    const qint64 at = playhead_;
    const auto refuse = [this](const QString& message) { showError(message, false); return message; };
    if (start) {
        if (at >= edit_.exportEnd(options_.durationUs)) return refuse("Trim start must be before the trim end");
        propose(QJsonArray{QJsonObject{{"op", "setTrim"}, {"start", double(at)}, {"end", edit_.trimEnd ? QJsonValue(double(*edit_.trimEnd)) : QJsonValue(QJsonValue::Null)}}});
        return {};
    }
    // The playhead frame is the last one kept.
    const qint64 end = nextFrameEdge(at);
    if (end <= edit_.trimStart) return refuse("Trim end must be after the trim start");
    propose(QJsonArray{QJsonObject{{"op", "setTrim"}, {"start", double(edit_.trimStart)}, {"end", end >= options_.durationUs ? QJsonValue(QJsonValue::Null) : QJsonValue(double(end))}}});
    return {};
}
QString ReviewWindow::setMaskAt(bool start, const QString& id) {
    const auto refuse = [this](const QString& message) { showError(message, false); return message; };
    const auto* mask = id.isEmpty() ? selectedMask() : maskById(id);
    if (!mask) return refuse("Select a cover to set its times");
    const qint64 at = playhead_;
    if (start) {
        if (at >= mask->end) return refuse("The cover must start before it ends");
        updateCover(mask->id, [&](QJsonObject& m) { m["start"] = double(at); });
    } else {
        // Until the playhead: the frame shown stays covered.
        const qint64 end = nextFrameEdge(at);
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
    if (!answer.contains("proposal")) cancelUpload();
    if (answered > answeredSeq_) answeredSeq_ = std::min(answered, proposalSeq_);
    if (!refused.isEmpty()) {
        lastRefusal_ = refused;
        lastRefusal_["proposal"] = double(answered);
    }
    // Only the cut's own answer moves the playhead to its seam; its refusal forgets it.
    const bool cutAnswered = pendingCut_ && answered && answered == pendingCutProposal_;
    if (cutAnswered && !refused.isEmpty()) pendingCut_.reset();
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
    if (changed) {
        // Playback and the exact skim frame follow the current edit: playback
        // starts again from the playhead, a skim frame is asked for again.
        // With no kept footage ahead of the playhead it pauses there, never starting over.
        if (playing_) { const qint64 from = playhead_; stopPlayback(); startPlayback(from, true); }
        if (skimUs_ && !playing_) {
            skimmer_->cancel();
            timeline_->setSkimExact(false);
            const auto tile = filmstrip_->tile(*skimUs_);
            if (tile.isNull()) view_->clearSkim(); else view_->setSkim(tile, *skimUs_);
            askSkimFrame(*skimUs_);
        }
    }
    if (!refused.isEmpty() && !exporting_) {
        // Never silent: the status says what was not applied, and a waiting upload is cancelled.
        auto why = refused["message"].toString().trimmed();
        while (why.endsWith('.')) why.chop(1);
        showError(why.isEmpty() ? QStringLiteral("That change wasn't applied.") : QStringLiteral("That change wasn't applied: %1.").arg(why), false);
    }
    if (!refused.isEmpty()) cancelUpload(QStringLiteral("a change wasn't applied"));
    if (tryUpload()) return;
    refreshAll();
    frameTimer_.start();
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
    if (exporting) cancelUpload();
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
    cancelUpload();  // never past an error
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
// and waits until every proposal was accepted, then sends that revision with
// the output the window shows. While it waits, anything that happens cancels
// it through cancelUpload(): input in this window or its dialogs, an agent's
// action, a format or sound change, a dialog or an export starting, a push from
// the resident other than the awaited answer, a refusal or 2 s without an
// answer. The resident refuses an upload at any other revision or output.
void ReviewWindow::upload() {
    // Mid-gesture or placing there is a change not yet made: nothing to upload.
    if (exporting_ || closing_ || placing_ || view_->dragging() || timeline_->dragging()) return;
    stopPlayback();
    view_->commitNudge();
    uploadNotice_ = {};
    uploadWaiting_ = true;
    uploadWait_.start();
    tryUpload();
}
bool ReviewWindow::tryUpload() {
    if (!uploadWaiting_ || proposalPending()) return false;
    uploadWaiting_ = false;
    uploadWait_.stop();
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
    uploadNotice_ = {QStringLiteral("Upload cancelled: %1. Press Upload again.").arg(why.isEmpty() ? QStringLiteral("you changed the clip") : why), QStringLiteral("warning"), {{QStringLiteral("Dismiss"), QJsonObject{{"action", "dismissStatus"}}}}};
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
void ReviewWindow::askSkimFrame(qint64 at) {
    skimAsked_ = std::pair<qint64, quint64>(at, edit_.revision);
    skimmer_->request(at, edit_, viewEdge());
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
    clipTime_->setText(excluded ? QStringLiteral("clip time · Excluded from output") : QStringLiteral("clip time"));
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
    panel_->setState(state);
}
void ReviewWindow::refreshBanners() {
    if (!placeBanner_) return;
    if (placing_) {
        const auto& span = placing_->span;
        const auto what = placing_->kind == "solid" ? QStringLiteral("Draw the redaction") : QStringLiteral("Draw the blur");
        placeBanner_->setText(placing_->whole ? what + QStringLiteral(" · whole recording") : QStringLiteral("%1 · %2 – %3").arg(what, durationText(span.start), durationText(span.end)));
        placeBanner_->setGlyph(placing_->kind == "solid" ? QStringLiteral("redact") : QStringLiteral("blur"));
    }
    placeBanner_->setVisible(placing_.has_value());
    const auto* mask = selectedMask();
    blurBanner_->setVisible((placing_ && placing_->kind == "blur") || (!placing_ && mask && mask->kind == "blur"));
    auto* area = findChild<QWidget*>("viewerBanners");
    const bool any = !placeBanner_->isHidden() || !blurBanner_->isHidden() || !noticeBand_->isHidden() || !statusRow_->isHidden();
    if (area->isHidden() == any) area->setVisible(any);
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
    if (!overlay_->isOpen()) { view_->setRightInset(0); return; }
    const int left = view_->mapFrom(this, overlay_->geometry().topLeft()).x();
    view_->setRightInset(std::max(0, view_->width() - left + reviewSpace(Ui::Space::s)));
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
        frameTimer_.start();
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
        {"lastRefusal", lastRefusal_.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(lastRefusal_)}, {"uploadWaiting", uploadWaiting_},
        {"playbackQueuedEnd", bool(playEnded_ && playEnded_->load())},
        {"playhead", QJsonObject{{"sourceUs", double(playhead_)}, {"outputUs", double(kept_.toOutput(playhead_))}}},
        {"undo", QJsonObject{{"canUndo", edit_.canUndo}, {"canRedo", edit_.canRedo}}},
        {"viewer", QJsonObject{{"zoom", view_->actualSize() ? QStringLiteral("actual") : QStringLiteral("fit")}, {"percent", view_->zoomPercent()}, {"pan", pan}}},
        {"warning", blurBanner_->isVisibleTo(this) ? QJsonValue(blurBanner_->text()) : QJsonValue(QJsonValue::Null)}, {"folded", folded_},
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
        {"preview", QJsonObject{{"error", view_->previewError().isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(view_->previewError())}, {"stale", view_->stale()}, {"frameTimeUs", view_->hasFrame() ? QJsonValue(double(view_->frameTime())) : QJsonValue(QJsonValue::Null)}, {"frameSize", QJsonArray{view_->frameSize().width(), view_->frameSize().height()}}, {"skimSize", QJsonArray{view_->skimSize().width(), view_->skimSize().height()}}}},
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
    if (params["action"] != "inspect") cancelUpload();
    const quint64 before = proposalSeq_;
    auto result = driveAction(params);
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
    if (action == "retryPreview") { frameTimer_.stop(); refreshFrame(); return done(); }
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
        message_ = {};
        uploadNotice_ = {};
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
        if (!placing_ && !selectedMask()) return invalid("Arm Redact or Blur to draw a box, or select a mask first");
        if (r.size() != 4) return invalid("placeMask takes rect [x, y, width, height] in canvas pixels");
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
    if (action == "timelineZoom") {
        const auto level = whole("level");
        if (!level || (*level != 1 && *level != 2 && *level != 4 && *level != 8 && *level != 16)) return invalid("timelineZoom takes level 1 (Fit), 2, 4, 8 or 16");
        timeline_->setZoom(int(*level), playhead_);
        return done();
    }
    if (action == "timelineFit") { timeline_->setZoom(1, playhead_); return done(); }
    if (action == "viewerZoom") {
        const auto mode = params["mode"].toString();
        if (mode != "fit" && mode != "actual") return invalid("viewerZoom takes mode fit|actual");
        view_->setActualSize(mode == "actual");
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
void ReviewWindow::refreshFrame() {
    QImage image;
    qint64 actual = 0;
    const qint64 at = std::min<qint64>(edgePreview_ ? *edgePreview_ : playhead_, std::max<qint64>(0, options_.durationUs - 1));
    debugLog(QStringLiteral("review: frame at %1").arg(at));
    if (auto e = previewFrame(options_.workingClip, edit_, config_, at, viewEdge(), options_.canvas, image, &actual)) {
        // Never a silent old frame: say so over it until a refresh succeeds.
        debugLog("review: preview failed " + e.message);
        QString reason;
        const QFileInfo clip(options_.workingClip);
        if (!clip.exists()) reason = QStringLiteral("the private recording file is missing");
        else if (!clip.isReadable()) reason = QStringLiteral("the private recording file isn't readable; check its permissions");
        else if (e.message.startsWith("Cannot open") || e.message.startsWith("Cannot read") || e.message.startsWith("Reading")) reason = QStringLiteral("the private recording file couldn't be read; check that it is intact");
        else if (e.message.startsWith("The working clip has no video")) reason = QStringLiteral("the private recording has no readable video track");
        else if (e.message.startsWith("No frame")) reason = QStringLiteral("no video frame is available at this time; try a nearby frame");
        else reason = QStringLiteral("the video frame couldn't be decoded; try a nearby frame or retry the preview");
        view_->setPreviewError(QStringLiteral("The frame at %1 couldn't be decoded (%2).").arg(durationText(at), reason));
        return;
    }
    view_->setFrame(image, at);
    debugLog(QStringLiteral("review: frame %1x%2").arg(image.width()).arg(image.height()));
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
        const qint64 outputStart = kept.toOutput(start);
        // Output time the wall clock has reached at this speed.
        const auto due = [&] { return outputStart + qint64(clock.elapsed() * 1000 * speed); };
        qint64 shownAt = -1;
        bool reachedEnd = true;
        for (const auto& span : kept.spans()) {
            if (span.end <= start) continue;
            if (stop->load()) { reachedEnd = false; break; }
            // Each kept span is opened at its start: a cut is never decoded through.
            const qint64 from = std::max(start, span.start);
            FrameSequence frames;
            if (frames.open(clip, from)) { reachedEnd = false; break; }
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
                    guard->playhead_ = frameUs;
                    guard->timeline_->setPlayhead(frameUs);
                    guard->refreshTransport();
                }, Qt::QueuedConnection);
            }
            if (stop->load()) { reachedEnd = false; break; }
        }
        ended->store(reachedEnd);  // the run played to the end and queued it (inspect, tests)
        QMetaObject::invokeMethod(guard, [guard, stop, reachedEnd] {
            if (!guard || guard->playStop_ != stop) return;
            guard->stopPlayback();
            // Stopped at the trim end: on the last kept frame.
            if (reachedEnd) if (const auto last = guard->lastKeptFrame(0, guard->options_.durationUs)) guard->seek(*last, false);
        }, Qt::QueuedConnection);
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
    if (exporting_ || placing_ || chooser_->isOpen()) return false;
    cancelUpload();
    const auto extension = config_.extension();
    FileDialog::Options options{FileDialog::Mode::Save, "Save a copy", QDir::homePath(), "recording." + extension, {{config_.format.toUpper(), {"*." + extension}}}};
    return chooser_->open(options, [this, extension](const FileDialog::Result& result) {
        if (result.failed()) { showError("Couldn't save a copy there: " + result.error, false); return; }
        if (!result.chosen() || exporting_) return;
        const auto& path = result.path;
        emit saveCopyRequested(path.endsWith("." + extension, Qt::CaseInsensitive) ? path : path + "." + extension, edit_.revision);
    });
}
void ReviewWindow::hideEvent(QHideEvent* e) {
    // Upload, cancel and a stop elsewhere hide the review: its chooser goes too
    // (minimizing is a spontaneous hide and leaves it open).
    if (!e->spontaneous()) {
        chooser_->cancel();
        if (discard_) answerDiscard(false);
        if (adviceDialog_) { adviceDialog_->close(); adviceDialog_.clear(); }
        if (overlay_->isOpen()) overlay_->dismiss();
    }
    QWidget::hideEvent(e);
}
bool ReviewWindow::eventFilter(QObject* watched, QEvent* event) {
    if (uploadWaiting_) {
        const auto type = event->type();
        const bool input = type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick || type == QEvent::Wheel
            || (type == QEvent::KeyPress && !static_cast<QKeyEvent*>(event)->isAutoRepeat());
        auto* widget = qobject_cast<QWidget*>(watched);
        // A dialog, menu or popup of this window opening counts as much as a click.
        const bool opened = type == QEvent::Show && widget && widget->isWindow() && widget != this;
        if (input || opened) for (auto* w = widget; w; w = w->parentWidget()) if (w == this) { cancelUpload(); break; }
    }
    // Delete and Backspace cut or remove only from the viewer and the timeline
    // (the Edits list handles its own rows; a field keeps its own keys).
    if (event->type() == QEvent::KeyPress && (watched == view_ || watched == timeline_)) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if ((key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace) && !(key->modifiers() & ~Qt::KeypadModifier) && !exporting_) {
            if (timeSelection_ && !placing_) { cutSelection(); return true; }
            if (selectedMask() && !placing_) { stopPlayback(); removeSelectedCover(); return true; }
            return true;  // Delete never restores a cut
        }
    }
    return QWidget::eventFilter(watched, event);
}
void ReviewWindow::keyPressEvent(QKeyEvent* e) {
    const auto modifiers = e->modifiers() & ~Qt::KeypadModifier;
    if (exporting_) { QWidget::keyPressEvent(e); return; }
    const bool plain = modifiers == Qt::NoModifier, shift = modifiers == Qt::ShiftModifier;
    switch (e->key()) {
    case Qt::Key_Space: if (plain) { if (playing_) stopPlayback(); else startPlayback(); return; } break;
    case Qt::Key_Escape:
        if (plain) {
            if (placing_) { cancelPlacement(); return; }
            if (timeSelection_ || !selectedKind_.isEmpty()) { clearSelection(); return; }
        }
        break;
    case Qt::Key_V: if (plain) { cancelPlacement(); return; } break;
    case Qt::Key_C: if (plain) { cutSelection(); return; } break;
    case Qt::Key_X: if (plain) { arm("solid"); return; } break;
    case Qt::Key_B: if (plain) { arm("blur"); return; } break;
    case Qt::Key_I: case Qt::Key_O:
        if (plain) { stopPlayback(); setTrimAt(e->key() == Qt::Key_I); return; }
        if (shift) { stopPlayback(); setMaskAt(e->key() == Qt::Key_I); return; }
        break;
    case Qt::Key_Comma: if (plain) { stepFrames(-1); return; } break;
    case Qt::Key_Period: if (plain) { stepFrames(1); return; } break;
    case Qt::Key_Left: case Qt::Key_Right:
        if (plain || shift) { stepTime((e->key() == Qt::Key_Left ? -1 : 1) * (shift ? secondUs : secondUs / 10)); return; }
        break;
    case Qt::Key_Return: case Qt::Key_Enter:
        // A plain widget has no default button, so Enter is handled here by
        // focus and state: a field keeps it, a focused button presses itself;
        // while placing it places the centred cover, a selected cut is
        // restored, otherwise it uploads.
        if (e->isAutoRepeat()) return;  // a held Enter places or restores once, never uploads
        if (plain) {
            auto* focus = focusWidget();
            if (qobject_cast<QLineEdit*>(focus) || qobject_cast<QAbstractSpinBox*>(focus)) return;
            if (auto* button = qobject_cast<QAbstractButton*>(focus); button && isAncestorOf(button)) { if (button->isEnabled()) button->click(); return; }
            if (placing_) { if (const QRect r = view_->defaultMaskRect(); !r.isEmpty()) placeCover(placing_->kind, r); return; }
            if (selectedKind_ == "cut") { restoreCut(selectedId_); return; }
            upload();
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
