#include "Graphite.h"
#include "IconArt.h"
#include "VisualStyle.h"
#include <QAccessibleWidget>
#include <QClipboard>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QBoxLayout>
#include <QComboBox>
#include <QCursor>
#include <QEvent>
#include <QFocusEvent>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHash>
#include <QIconEngine>
#include <QKeyEvent>
#include <QScreen>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QRegularExpression>
#include <QVBoxLayout>
#include <QFrame>
#include <QFormLayout>
#include <QGridLayout>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QScrollBar>
#include <QScrollArea>
#include <QShowEvent>
#include <QTimer>
#include <QSlider>
#include <QStatusBar>
#include <QStyleFactory>
#include <QStyleOption>
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
#include <QAccessibilityHints>
#include <QStyleHints>
#endif
#include <QStyledItemDelegate>
#include <QTextEdit>
#include <QToolButton>
#include <QToolTip>
#include <QWindow>
#include <algorithm>
#include <memory>
#include <cmath>

namespace putgd::Graphite {
namespace {
QString& uiFamilyName() { static QString name; return name; }
QString& monoFamilyName() { static QString name; return name; }
const Theme::Palette& t() { return Theme::current(); }
QString role(const QWidget* widget) { return widget ? widget->property("visualRole").toString() : QString(); }
bool flushButton(const QWidget* widget) { return widget && role(widget) == "quiet" && widget->property("graphiteFlush").toBool(); }
bool progressAnimationEnabled(const QWidget* widget, const QStyleOption* option = nullptr) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
    if (QGuiApplication::styleHints()->accessibility()->motionPreference() == Qt::MotionPreference::ReducedMotion) return false;
#endif
    return (widget ? widget->style() : QApplication::style())->styleHint(QStyle::SH_Widget_Animate, option, widget);
}
bool keyboardState(const QStyleOption* option) {
    return (option->state & QStyle::State_HasFocus) && (option->state & QStyle::State_KeyboardFocusChange);
}
// A disabled control marked as a locked value (setLocked), or the editor
// inside one (an editable combo box's or a spin box's line edit).
bool lockedLook(const QStyleOption* option, const QWidget* widget) {
    if (option->state & QStyle::State_Enabled) return false;
    return isLocked(widget) || (widget && (qobject_cast<const QComboBox*>(widget->parentWidget()) || qobject_cast<const QAbstractSpinBox*>(widget->parentWidget())) && isLocked(widget->parentWidget()));
}
void rounded(QPainter* p, const QRectF& rect, const QColor& fill, const QColor& stroke = {}, qreal radius = Metric::radius, qreal width = 1) {
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setBrush(fill.isValid() ? QBrush(fill) : QBrush(Qt::NoBrush));
    if (stroke.isValid()) {
        p->setPen(QPen(stroke, width));
        const qreal inset = width / 2;
        p->drawRoundedRect(rect.adjusted(inset, inset, -inset, -inset), radius, radius);
    } else {
        p->setPen(Qt::NoPen);
        p->drawRoundedRect(rect, radius, radius);
    }
    p->restore();
}
// A rounded rectangle with only some corners rounded (split fields, units).
QPainterPath partlyRounded(const QRectF& r, qreal radius, bool left, bool right) {
    QPainterPath path;
    const qreal l = left ? radius : 0, rr = right ? radius : 0;
    path.moveTo(r.left() + l, r.top());
    path.lineTo(r.right() - rr, r.top());
    if (rr) path.quadTo(r.right(), r.top(), r.right(), r.top() + rr);
    path.lineTo(r.right(), r.bottom() - rr);
    if (rr) path.quadTo(r.right(), r.bottom(), r.right() - rr, r.bottom());
    path.lineTo(r.left() + l, r.bottom());
    if (l) path.quadTo(r.left(), r.bottom(), r.left(), r.bottom() - l);
    path.lineTo(r.left(), r.top() + l);
    if (l) path.quadTo(r.left(), r.top(), r.left() + l, r.top());
    path.closeSubpath();
    return path;
}
// A text field's frame: raised fill, the 3:1 control outline (the quiet
// border once disabled), a stronger stroke on hover, an accent stroke while
// focused, or the danger stroke when the field is invalid.
void field(QPainter* p, const QRectF& rect, const QStyleOption* option, const QWidget* widget, bool focused, bool squareRight = false, bool squareLeft = false) {
    const bool enabled = option->state & QStyle::State_Enabled;
    const bool locked = lockedLook(option, widget);
    QColor stroke = enabled ? controlOutline() : locked ? t().borderStrong : controlStroke();
    if (widget && widget->property("invalid").toBool()) stroke = t().danger;
    else if (focused && enabled) stroke = t().accent;
    else if (enabled && (option->state & QStyle::State_MouseOver)) stroke = t().textMuted;
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setPen(QPen(stroke, 1));
    // A locked value has no fill of its own: a read-only frame on the page.
    p->setBrush(enabled ? QBrush(t().surfaceRaised) : locked ? QBrush(Qt::NoBrush) : QBrush(t().surface));
    p->drawPath(partlyRounded(rect.adjusted(0.5, 0.5, -0.5, -0.5), Metric::radius, !squareLeft, !squareRight));
    p->restore();
}
struct Look {
    QColor fill, stroke, text;
    bool strong = false;
    bool underline = false;
};
// The colours of a button in its role and state.
Look buttonLook(const QString& roleName, const QStyleOption* option, bool flat, const QWidget* widget = nullptr) {
    const auto& c = t();
    const bool enabled = option->state & QStyle::State_Enabled;
    // A button that states a fact (Default) rather than offering an action.
    if (lockedLook(option, widget)) {
        Look look;
        look.stroke = c.borderStrong;
        look.text = c.textMuted;
        return look;
    }
    const bool hover = enabled && (option->state & QStyle::State_MouseOver);
    const bool down = enabled && (option->state & QStyle::State_Sunken);
    const bool on = (option->state & QStyle::State_On) && !(widget && widget->property("graphiteDisclosure").toBool());
    Look look;
    if (roleName == "link" || roleName == "version") {
        look.text = enabled ? (roleName == "version" && !hover ? c.textMuted : c.textSecondary) : c.textDisabled;
        look.underline = hover;
    } else if (roleName == "primary") {
        look.fill = enabled ? (down ? c.textSecondary : hover ? mix(c.text, c.textSecondary, 0.55) : c.text) : c.surfaceRaised;
        look.text = enabled ? c.bg : c.textDisabled;
        look.strong = true;
    } else if (roleName == "danger") {
        look.fill = enabled ? (down || hover ? c.dangerSurface : c.surfaceRaised) : c.surface;
        look.stroke = enabled ? c.danger : controlStroke();
        look.text = enabled ? (hover ? c.dangerHover : c.danger) : c.textDisabled;
    } else if (roleName == "info" || roleName == "quiet" || flat) {
        look.fill = down || on ? c.selected : hover ? mix(c.surface, c.surfaceRaised, 0.8) : QColor(Qt::transparent);
        look.text = enabled ? (on ? c.text : c.textSecondary) : c.textDisabled;
    } else {
        look.fill = !enabled ? mix(c.surface, c.surfaceRaised, 0.5) : down || on ? c.selected : hover ? mix(c.surfaceRaised, c.selected, 0.6) : c.surfaceRaised;
        look.stroke = on ? mix(c.borderStrong, c.selected, 0.45) : controlStroke();
        look.text = enabled ? (on ? c.text : c.textSecondary) : c.textDisabled;
    }
    if (on && roleName != "primary") look.strong = false;
    return look;
}
QString buttonRole(const QWidget* widget, const QStyleOption* option) {
    auto value = role(widget);
    if (value.isEmpty()) {
        if (const auto* button = qobject_cast<const QPushButton*>(widget); button && button->isFlat()) value = "quiet";
        if (option->state & QStyle::State_AutoRaise) value = "quiet";
    }
    return value;
}
QPixmap tinted(const QPixmap& source, const QColor& colour) {
    QPixmap result(source.size());
    result.setDevicePixelRatio(source.devicePixelRatio());
    result.fill(Qt::transparent);
    QPainter p(&result);
    p.drawPixmap(0, 0, source);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(result.rect(), colour);
    return result;
}
qreal dpr(const QWidget* widget) { return widget ? widget->devicePixelRatioF() : qApp->devicePixelRatio(); }
void check(QPainter* p, const QRectF& box, const QStyleOption* option, bool keyboard, bool locked = false) {
    const auto& c = t();
    const bool enabled = option->state & QStyle::State_Enabled;
    const bool on = option->state & QStyle::State_On, partial = option->state & QStyle::State_NoChange;
    // Checked stays visibly checked when disabled: a muted fill with the tick.
    QColor fill = on || partial ? (enabled ? c.text : c.textMuted) : (enabled ? c.surfaceRaised : locked ? QColor(Qt::transparent) : c.surface);
    QColor stroke = keyboard ? c.accent : on || partial ? fill : enabled ? c.controlOutline : locked ? c.textMuted : c.textDisabled;
    rounded(p, box, fill, stroke, 4, keyboard ? 1.5 : 1);
    if (!on && !partial) return;
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setPen(QPen(c.bg, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const qreal s = box.width() / 17.0;
    if (partial) p->drawLine(QPointF(box.left() + 4.5 * s, box.center().y()), QPointF(box.right() - 4.5 * s, box.center().y()));
    else {
        QPainterPath tick;
        tick.moveTo(box.left() + 4 * s, box.top() + 8.8 * s);
        tick.lineTo(box.left() + 7.2 * s, box.top() + 12 * s);
        tick.lineTo(box.left() + 13 * s, box.top() + 5.5 * s);
        p->drawPath(tick);
    }
    p->restore();
}
void radio(QPainter* p, const QRectF& box, const QStyleOption* option, bool keyboard, bool locked = false) {
    const auto& c = t();
    const bool enabled = option->state & QStyle::State_Enabled, on = option->state & QStyle::State_On;
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const QRectF circle = box.adjusted(0.75, 0.75, -0.75, -0.75);
    p->setPen(QPen(keyboard ? c.accent : on && enabled ? c.text : enabled ? c.controlOutline : locked ? c.textMuted : c.textDisabled, keyboard ? 1.5 : 1));
    p->setBrush(enabled ? QBrush(c.surfaceRaised) : locked ? QBrush(Qt::NoBrush) : QBrush(c.surface));
    p->drawEllipse(circle);
    if (on) {
        p->setPen(Qt::NoPen);
        p->setBrush(enabled ? c.text : c.textMuted);
        p->drawEllipse(circle.center(), circle.width() * 0.25, circle.width() * 0.25);
    }
    p->restore();
}
// Chrome glyphs from paths on a 20-unit grid.
class ChromeIcon final : public QIconEngine {
public:
    explicit ChromeIcon(QString name) : name_(std::move(name)) {}
    QIconEngine* clone() const override { return new ChromeIcon(name_); }
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State) override {
        paintChromeGlyph(*painter, rect, name_, mode == QIcon::Disabled ? t().textDisabled : mode == QIcon::Selected ? t().iconSelected : t().icon);
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override { return scaledPixmap(size, mode, state, 1.0); }
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        QPixmap pixmap(size * scale);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        paint(&painter, QRect(QPoint(), size), mode, state);
        return pixmap;
    }
private:
    QString name_;
};
}

qreal opticalBaseline(const QFontMetricsF& metrics, const QString& text, qreal centre) {
    // Centre the font's capital height, not each string's own ink: labels
    // side by side then share one baseline whether or not they have
    // descenders ("Capture window" beside "Record screen"), and still sit
    // optically in the middle whatever the platform font padding. Editable
    // text keeps Qt's own metrics; this is for painted labels only.
    if (text.trimmed().isEmpty()) return centre + (metrics.ascent() - metrics.descent()) / 2;
    const qreal cap = metrics.capHeight() > 0 ? metrics.capHeight() : metrics.tightBoundingRect(QStringLiteral("H")).height();
    return centre + cap / 2;
}
qreal textScale() {
    // Pixel sizes do not follow the desktop's text size, so the language
    // scales them by it explicitly: the desktop's general font against the
    // 14 px design body (never below 1, at most 2). PUTGD_TEXT_SCALE overrides
    // it for tests. Device pixel ratio is separate and handled by Qt.
    static const qreal scale = [] {
        bool ok = false;
        const qreal forced = qEnvironmentVariable("PUTGD_TEXT_SCALE").toDouble(&ok);
        if (ok && forced > 0) return std::clamp(forced, 0.5, 3.0);
        const auto system = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
        qreal pixels = system.pixelSize() > 0 ? system.pixelSize() : system.pointSizeF() * 96.0 / 72.0;
        // Desktop defaults are 10-11 pt (13-15 px); only a larger text size scales.
        return std::clamp(pixels / 14.666, 1.0, 2.0);
    }();
    return scale;
}
namespace {
qreal& installedBody() { static qreal pixels = 0; return pixels; }
qreal pixelsOf(const QFont& f) { return f.pixelSize() > 0 ? f.pixelSize() : f.pointSizeF() * 96.0 / 72.0; }
}
// An application font made larger after install (the recording and editor
// large-text contracts set one) scales the painted type with it.
qreal applicationScale() {
    if (installedBody() <= 0 || !qApp) return 1.0;
    return std::clamp(pixelsOf(QApplication::font()) / installedBody(), 0.75, 3.0);
}
int scaled(int pixels) { return int(std::lround(pixels * textScale() * applicationScale())); }
QString uiFamily() { return uiFamilyName().isEmpty() ? QApplication::font().family() : uiFamilyName(); }
QString monoFamily() { return monoFamilyName().isEmpty() ? QFontDatabase::systemFont(QFontDatabase::FixedFont).family() : monoFamilyName(); }
void setFamilies(const QString& ui, const QString& mono) {
    const bool first = uiFamilyName().isEmpty();
    uiFamilyName() = ui;
    monoFamilyName() = mono;
    // Fonts registered after install: adopt the family, keep the size in use.
    if (first && qApp && qApp->property("putgdGraphite").toBool()) {
        QFont f = QApplication::font();
        f.setFamilies({ui});
        QApplication::setFont(f);
    }
}
QFont font(int pixels, bool strong) {
    QFont f(uiFamily());
    f.setPixelSize(scaled(pixels));
    f.setWeight(strong ? QFont::DemiBold : QFont::Normal);
    return f;
}
QFont monoFont(int pixels) {
    QFont f(monoFamily());
    f.setPixelSize(scaled(pixels));
    return f;
}
QColor mix(const QColor& a, const QColor& b, qreal amount) {
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * amount, a.greenF() + (b.greenF() - a.greenF()) * amount,
                            a.blueF() + (b.blueF() - a.blueF()) * amount, a.alphaF() + (b.alphaF() - a.alphaF()) * amount);
}
QColor controlStroke() { return mix(t().border, t().borderStrong, 0.35); }
QColor controlOutline() { return t().controlOutline; }
bool keyboardFocus(const QWidget* widget) {
    return widget && widget->hasFocus() && widget->window()->testAttribute(Qt::WA_KeyboardFocusChange);
}
void setLocked(QWidget* widget, bool locked) {
    if (!widget || isLocked(widget) == locked) return;
    widget->setProperty("graphiteLocked", locked);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->updateGeometry();
    widget->update();
}
bool isLocked(const QWidget* widget) { return widget && widget->property("graphiteLocked").toBool(); }
int sidebarWidth(int baseline, int natural) { return std::clamp(natural, baseline, Metric::sidebarMax); }
QPalette palette() {
    const auto& p = t();
    QPalette palette;
    palette.setColor(QPalette::Window, p.bg);
    palette.setColor(QPalette::WindowText, p.text);
    palette.setColor(QPalette::Base, p.surfaceRaised);
    palette.setColor(QPalette::AlternateBase, p.surface);
    palette.setColor(QPalette::Text, p.text);
    palette.setColor(QPalette::Button, p.surfaceRaised);
    palette.setColor(QPalette::ButtonText, p.textSecondary);
    palette.setColor(QPalette::BrightText, p.text);
    palette.setColor(QPalette::Light, p.borderStrong);
    palette.setColor(QPalette::Midlight, p.border);
    palette.setColor(QPalette::Mid, p.border);
    palette.setColor(QPalette::Dark, p.bg);
    palette.setColor(QPalette::Shadow, p.bg);
    palette.setColor(QPalette::Highlight, p.accent);
    palette.setColor(QPalette::HighlightedText, p.onAccent);
    palette.setColor(QPalette::PlaceholderText, p.textMuted);
    palette.setColor(QPalette::Link, p.link);
    palette.setColor(QPalette::LinkVisited, p.link);
    palette.setColor(QPalette::ToolTipBase, p.surfaceRaised);
    palette.setColor(QPalette::ToolTipText, p.text);
    for (const auto role : {QPalette::Text, QPalette::ButtonText, QPalette::WindowText}) palette.setColor(QPalette::Disabled, role, p.textDisabled);
    palette.setColor(QPalette::Disabled, QPalette::Base, p.surface);
    palette.setColor(QPalette::Disabled, QPalette::Highlight, p.selected);
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, p.textMuted);
    return palette;
}
void install(QApplication& app) {
    // Once per process: a later call (another window, a test) never resets
    // the application font a caller enlarged, nor the scale it implies.
    if (app.property("putgdGraphite").toBool()) return;
    app.setProperty("putgdGraphite", true);
    if (!qobject_cast<Style*>(app.style())) app.setStyle(new Style);
    app.setPalette(palette());
    // An application font already enlarged against the desktop's (a test or a
    // caller) keeps its enlargement; the body size is the base it scales from.
    const qreal enlarged = std::max<qreal>(1.0, pixelsOf(app.font()) / std::max<qreal>(1.0, pixelsOf(QFontDatabase::systemFont(QFontDatabase::GeneralFont))));
    installedBody() = 0;
    if (!uiFamilyName().isEmpty()) {
        QFont f = font(Type::body);
        const int base = f.pixelSize();
        f.setPixelSize(int(std::lround(base * enlarged)));
        app.setFont(f);
        installedBody() = base;
    } else installedBody() = pixelsOf(app.font()) / enlarged;
    QObject::connect(&Theme::Notifier::instance(), &Theme::Notifier::changed, &app, [&app] {
        app.setPalette(palette());
        for (auto* widget : QApplication::allWidgets()) if (widget->isVisible()) widget->update();
    });
}

void paintChevron(QPainter& p, const QPointF& c, qreal size, Qt::ArrowType direction, const QColor& colour, qreal width) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(colour, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    const qreal h = size / 2, q = size / 4;
    QPainterPath path;
    switch (direction) {
    case Qt::UpArrow: path.moveTo(c.x() - h, c.y() + q); path.lineTo(c.x(), c.y() - q); path.lineTo(c.x() + h, c.y() + q); break;
    case Qt::LeftArrow: path.moveTo(c.x() + q, c.y() - h); path.lineTo(c.x() - q, c.y()); path.lineTo(c.x() + q, c.y() + h); break;
    case Qt::RightArrow: path.moveTo(c.x() - q, c.y() - h); path.lineTo(c.x() + q, c.y()); path.lineTo(c.x() - q, c.y() + h); break;
    default: path.moveTo(c.x() - h, c.y() - q); path.lineTo(c.x(), c.y() + q); path.lineTo(c.x() + h, c.y() - q); break;
    }
    p.drawPath(path);
    p.restore();
}
void paintChromeGlyph(QPainter& painter, const QRectF& rect, const QString& name, const QColor& colour) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(rect.topLeft());
    painter.scale(rect.width() / 20.0, rect.height() / 20.0);
    painter.setPen(QPen(colour, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    if (name == "menu") for (int y : {5, 10, 15}) painter.drawLine(QPointF(3, y), QPointF(17, y));
    else if (name == "more") { painter.setPen(Qt::NoPen); painter.setBrush(colour); for (int x : {4, 10, 16}) painter.drawEllipse(QPointF(x, 10), 1.4, 1.4); }
    else if (name == "minimize") painter.drawLine(QPointF(4, 11), QPointF(16, 11));
    else if (name == "maximize") painter.drawRoundedRect(QRectF(4, 4, 12, 12), 1.5, 1.5);
    else if (name == "restore") { painter.drawRoundedRect(QRectF(3, 7, 10, 10), 1.5, 1.5); painter.drawLine(QPointF(7, 3), QPointF(17, 3)); painter.drawLine(QPointF(17, 3), QPointF(17, 13)); }
    else if (name == "close") { painter.drawLine(QPointF(5, 5), QPointF(15, 15)); painter.drawLine(QPointF(15, 5), QPointF(5, 15)); }
    else if (name == "check") { QPainterPath tick; tick.moveTo(4, 10.5); tick.lineTo(8, 14.5); tick.lineTo(16, 5.5); painter.drawPath(tick); }
    else if (name == "chevron-down") { painter.restore(); paintChevron(painter, rect.center(), rect.width() * 0.5, Qt::DownArrow, colour); return; }
    else if (name == "chevron-right") { painter.restore(); paintChevron(painter, rect.center(), rect.width() * 0.5, Qt::RightArrow, colour); return; }
    else if (name == "chevron-left") { painter.restore(); paintChevron(painter, rect.center(), rect.width() * 0.5, Qt::LeftArrow, colour); return; }
    painter.restore();
}
QIcon chromeIcon(const QString& name) { return QIcon(new ChromeIcon(name)); }
void paintText(QPainter& p, const QRectF& rect, const QString& text, const QColor& colour, const QFont& f, Qt::Alignment alignment, Qt::TextElideMode elide) {
    p.save();
    p.setFont(f);
    p.setPen(colour);
    const QFontMetricsF metrics(f);
    const auto shown = metrics.elidedText(text, elide, rect.width());
    const qreal width = metrics.horizontalAdvance(shown);
    qreal x = rect.left();
    if (alignment & Qt::AlignHCenter) x = rect.center().x() - width / 2;
    else if (alignment & Qt::AlignRight) x = rect.right() - width;
    qreal baseline = opticalBaseline(metrics, shown, rect.center().y());
    if (alignment & Qt::AlignTop) baseline = rect.top() + metrics.ascent();
    p.drawText(QPointF(x, baseline), shown);
    p.restore();
}

// ---------------------------------------------------------------- Style
Style::Style() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) { setObjectName(QStringLiteral("graphite")); }

void setMono(QWidget* widget, int pixels) {
    widget->setProperty("graphiteMonoPixels", pixels);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

void Style::polish(QPalette& palette) { palette = Graphite::palette(); }

void Style::polish(QWidget* widget) {
    QProxyStyle::polish(widget);
    if (qobject_cast<QAbstractButton*>(widget) || qobject_cast<QComboBox*>(widget) || qobject_cast<QAbstractSpinBox*>(widget) || qobject_cast<QTabBar*>(widget)
        || qobject_cast<QScrollBar*>(widget) || qobject_cast<QSlider*>(widget) || qobject_cast<QLineEdit*>(widget))
        widget->setAttribute(Qt::WA_Hover);
    if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
        focusTooltips(button);
        if (!button->icon().isNull()) {
            auto base = button->property("graphiteIconBase").toSize();
            if (!base.isValid()) {
                base = button->iconSize();
                if (base == QSize(scaled(Metric::icon), scaled(Metric::icon))) base = QSize(Metric::icon, Metric::icon);
                button->setProperty("graphiteIconBase", base);
            }
            button->setIconSize(QSize(scaled(base.width()), scaled(base.height())));
        }
    }
    if (auto* edit = qobject_cast<QLineEdit*>(widget); edit && !edit->property("graphitePlaceholderFont").toBool()) {
        edit->setProperty("graphitePlaceholderFont", true);
        QObject::connect(edit, &QLineEdit::textChanged, edit, [edit, empty = edit->text().isEmpty()](const QString& text) mutable {
            if (empty == text.isEmpty()) return;
            empty = text.isEmpty();
            edit->style()->polish(edit);
        });
    }
    if (auto* view = qobject_cast<QAbstractItemView*>(widget)) view->viewport()->setAttribute(Qt::WA_Hover);
    if (auto* combo = qobject_cast<QComboBox*>(widget)) {
        // The popup list paints its rows like every other list.
        if (auto* delegate = combo->itemDelegate(); delegate && QByteArray(delegate->metaObject()->className()) == "QComboBoxDelegate")
            combo->setItemDelegate(new QStyledItemDelegate(combo));
        combo->view()->setProperty("graphiteRowHeight", 32);
    }
    if (QByteArray(widget->metaObject()->className()) == "QTipLabel") widget->setFont(font(Type::metadata));
    // Type roles: size, weight and colour from the tokens. A role change
    // re-polishes (VisualStyle::role), as does a scheme switch.
    const auto name = role(widget);
    if (qobject_cast<QAbstractButton*>(widget) && (name == "link" || name == "version")) widget->setMinimumHeight(std::max(32, widget->minimumHeight()));
    const auto tone = widget->property("feedbackTone").toString();
    const auto binding = widget->property("bindingTone").toString();
    const bool locked = isLocked(widget);
    const bool bodyControl = qobject_cast<QComboBox*>(widget);
    const int monoPixels = widget->property("graphiteMonoPixels").toInt();
    if (name.isEmpty() && tone.isEmpty() && binding.isEmpty() && !locked && !bodyControl && !monoPixels) return;
    const auto& c = t();
    QColor colour;
    QFont f;
    bool setFont = true;
    if (name == "title" || name == "page") f = font(widget->property("compactHeading").toBool() ? Type::section : Type::page, true);
    else if (name == "sectionTitle" || name == "section") f = font(Type::section, true);
    else if (name == "strong") f = font(Type::body, true);
    else if (name == "eyebrow" || name == "railHeading" || name == "micro") { f = font(Type::micro, name != "micro"); colour = c.textMuted; }
    // An error is never small print: it takes body type even in a meta slot.
    else if (name == "meta") { f = font(tone == "error" ? Type::body : Type::metadata); colour = c.textMuted; }
    else if (name == "version") f = font(Type::micro);
    else if (name == "binding") { f = monoFont(Type::metadata); colour = c.textMuted; }
    else if (name == "mono" || name == "path" || (name == "link" && qobject_cast<QLineEdit*>(widget)) || name == "diagnostics") f = monoFont(Type::metadata);
    // Editable paths, URLs, prefixes, bucket names and identifiers: Mono at body size.
    else if (name == "technical") f = monoFont(Type::technical);
    else if (name == "pill") { f = font(Type::micro); colour = c.textSecondary; }
    else if (name == "editableTitle") f = font(widget->property("compactHeading").toBool() ? Type::section : Type::page, true);
    else if (bodyControl) f = font(Type::body);
    else setFont = false;
    if (monoPixels) { f = monoFont(monoPixels); setFont = true; }
    if (auto* edit = qobject_cast<QLineEdit*>(widget); setFont && edit && edit->text().isEmpty()) f.setFamily(uiFamily());
    // An outcome's tone outranks the slot's quiet colour: an error in a muted
    // line still reads as an error.
    if (tone == "error") colour = c.danger;
    else if (tone == "success") colour = c.success;
    else if (name == "subtitle" || name == "muted" || name == "default" || name == "private") colour = c.textMuted;
    else if (name == "secondary") colour = c.textSecondary;
    else if ((name == "link" && qobject_cast<QLabel*>(widget)) || binding == "active") colour = c.textSecondary;
    else if (name == "success" || tone == "success") colour = c.success;
    else if (name == "warning" || binding == "warning") colour = c.textSecondary;
    else if (name == "danger" || name == "error" || tone == "error" || binding == "error") colour = c.danger;
    else if (name == "status") colour = c.textSecondary;
    else if (name == "notice") colour = c.text;
    if (setFont && !qobject_cast<QAbstractButton*>(widget)) {
        widget->setFont(f);
        widget->setProperty("graphiteFont", true);
        widget->updateGeometry();
    } else if (setFont && name == "version") {
        widget->setFont(f);
        widget->setProperty("graphiteFont", true);
    }
    if (colour.isValid() || locked) {
        // Only the text roles: everything else still follows the parent.
        QPalette palette;
        palette.setResolveMask(0);
        if (colour.isValid())
            for (const auto group : {QPalette::Active, QPalette::Inactive}) {
                palette.setColor(group, QPalette::WindowText, colour);
                palette.setColor(group, QPalette::Text, colour);
            }
        // A locked value's text, and its editor's, is information to read.
        if (locked)
            for (const auto textRole : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) palette.setColor(QPalette::Disabled, textRole, c.textMuted);
        widget->setPalette(palette);
        widget->setProperty("graphitePalette", true);
    }
    if (bodyControl) {
        // QComboBox caches its hints across a manual unpolish/polish.
        QEvent change(QEvent::StyleChange);
        QCoreApplication::sendEvent(widget, &change);
        widget->updateGeometry();
    }
    if (auto* text = qobject_cast<QLabel*>(widget); text && (name == "status" || name == "notice")) {
        text->setMargin(9);
        widget->setProperty("graphiteMargin", true);
    }
    if (auto* text = qobject_cast<QLabel*>(widget); text && (name == "pill" || name == "unitSuffix")) {
        // Padding as a minimum size around the centred text, so it holds
        // whatever polishes the label later.
        const QFontMetrics metrics(text->font());
        text->setAlignment(Qt::AlignCenter);
        // A unit takes its field's height from the row (its field sets it), so
        // the joined pair shares top, bottom and baseline at any text size.
        text->setMinimumSize(metrics.horizontalAdvance(text->text()) + (name == "pill" ? 16 : 22), name == "pill" ? metrics.height() + 6 : 0);
        if (name == "unitSuffix") text->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        widget->setProperty("graphiteMargin", true);
    }
}

void Style::unpolish(QWidget* widget) {
    if (widget->property("graphiteFont").toBool()) { widget->setFont(QFont()); widget->setProperty("graphiteFont", {}); }
    if (widget->property("graphitePalette").toBool()) { widget->setPalette(QPalette()); widget->setProperty("graphitePalette", {}); }
    if (widget->property("graphiteMargin").toBool()) {
        if (auto* text = qobject_cast<QLabel*>(widget)) { text->setMargin(0); text->setMinimumSize(0, 0); }
        widget->setProperty("graphiteMargin", {});
    }
    QProxyStyle::unpolish(widget);
}

int Style::pixelMetric(PixelMetric metric, const QStyleOption* option, const QWidget* widget) const {
    switch (metric) {
    case PM_ButtonMargin: return 14;
    case PM_ButtonDefaultIndicator: case PM_ButtonShiftHorizontal: case PM_ButtonShiftVertical: return 0;
    case PM_DefaultFrameWidth: return 1;
    case PM_ComboBoxFrameWidth: case PM_SpinBoxFrameWidth: return 1;
    case PM_IndicatorWidth: case PM_IndicatorHeight: return 17;
    case PM_ExclusiveIndicatorWidth: case PM_ExclusiveIndicatorHeight: return 18;
    case PM_CheckBoxLabelSpacing: case PM_RadioButtonLabelSpacing: return 9;
    case PM_ScrollBarExtent: return std::max(12, int(std::lround(12 + 8 * (textScale() * applicationScale() - 1))));
    case PM_ScrollBarSliderMin: return 28;
    case PM_MenuPanelWidth: return 1;
    case PM_MenuHMargin: case PM_MenuVMargin: return 5;
    case PM_MenuButtonIndicator: return Metric::splitZone;
    case PM_SubMenuOverlap: return 2;
    case PM_ToolTipLabelFrameWidth: return 7;
    case PM_SplitterWidth: return 8;
    case PM_TabBarTabShiftVertical: case PM_TabBarTabShiftHorizontal: case PM_TabBarBaseOverlap: case PM_TabBarBaseHeight: return 0;
    case PM_TabBarTabHSpace: return 40;
    case PM_TabBarTabVSpace: return 16;
    case PM_SliderLength: case PM_SliderControlThickness: case PM_SliderThickness: return 32;
    case PM_ToolBarIconSize: case PM_ButtonIconSize: case PM_SmallIconSize: return scaled(Metric::icon);
    case PM_FocusFrameHMargin: case PM_FocusFrameVMargin: return 0;
    case PM_HeaderMargin: return 8;
    case PM_ProgressBarChunkWidth: return 1;
    case PM_MenuBarItemSpacing: return 4;
    default: return QProxyStyle::pixelMetric(metric, option, widget);
    }
}

int Style::styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget, QStyleHintReturn* data) const {
    switch (hint) {
    case SH_ComboBox_Popup: return 0;
    case SH_ComboBox_ListMouseTracking: return 1;
    case SH_TabBar_Alignment: return Qt::AlignCenter;
    case SH_ItemView_ShowDecorationSelected: return 1;
    case SH_ItemView_MovementWithoutUpdatingSelection: return 1;
    case SH_Menu_MouseTracking: case SH_MenuBar_MouseTracking: return 1;
    case SH_Menu_SubMenuPopupDelay: return 160;
    case SH_ToolTipLabel_Opacity: return 255;
    case SH_Slider_AbsoluteSetButtons: return Qt::LeftButton;
    case SH_FocusFrame_Mask: case SH_RubberBand_Mask: return 0;
    case SH_Table_GridLineColor: return int(t().border.rgb());
    case SH_DialogButtonBox_ButtonsHaveIcons: return 0;
    case SH_ScrollBar_Transient: return 0;
    case SH_Widget_Animation_Duration: return 110;
    case SH_DialogButtonLayout:
        if (widget && widget->window()->objectName() == "confirmAction") return QDialogButtonBox::GnomeLayout;
        return QProxyStyle::styleHint(hint, option, widget, data);
    default: return QProxyStyle::styleHint(hint, option, widget, data);
    }
}

QSize Style::sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& contents, const QWidget* widget) const {
    switch (type) {
    case CT_PushButton: {
        const auto* button = qstyleoption_cast<const QStyleOptionButton*>(option);
        const auto name = role(widget);
        if (name == "link" || name == "version") return {std::max(32, contents.width() + 8), std::max(32, contents.height() + 8)};
        if (name == "info") return {std::max(contents.width() + 6, 23), std::max(contents.height() + 4, 23)};
        // Measure the label in the weight it is painted in (primary is semibold).
        int w = contents.width() + 2 * 14;
        if (button && !button->text.isEmpty()) {
            QFont f = widget ? widget->font() : QApplication::font();
            if (name == "primary") f.setWeight(QFont::DemiBold);
            const int text = int(std::ceil(QFontMetricsF(f).horizontalAdvance(QString(button->text).remove(QRegularExpression(QStringLiteral("&(?!&)"))))));
            const int icon = button->icon.isNull() ? 0 : button->iconSize.width() + 8;
            w = text + icon + 2 * 14 + 2;
        }
        if (button && (button->features & QStyleOptionButton::HasMenu) && !(widget && widget->property("noChevron").toBool())) w += Metric::splitZone - 8;
        if (button && button->text.isEmpty()) return {std::max(Metric::control, contents.width() + 18), std::max(Metric::control, contents.height() + 14)};
        if (flushButton(widget)) w -= 14;
        if (button && !button->text.isEmpty()) w = std::max(w, 64);
        return {w, std::max(Metric::control, contents.height() + 14)};
    }
    case CT_ToolButton: {
        const auto* tool = qstyleoption_cast<const QStyleOptionToolButton*>(option);
        if (!tool) break;
        const auto name = role(widget);
        const bool split = tool->features & QStyleOptionToolButton::MenuButtonPopup;
        const bool menu = (tool->features & QStyleOptionToolButton::HasMenu) && !split && !(widget && widget->property("noChevron").toBool());
        const bool text = !tool->text.isEmpty() && tool->toolButtonStyle != Qt::ToolButtonIconOnly;
        const bool icon = !tool->icon.isNull() && tool->toolButtonStyle != Qt::ToolButtonTextOnly;
        if (name == "info") return {std::max(23, tool->iconSize.width() + 5), std::max(23, tool->iconSize.height() + 5)};
        if (name == "link" || name == "version") {
            const QFontMetrics metrics(widget ? widget->font() : QApplication::font());
            return {std::max(32, metrics.horizontalAdvance(tool->text) + 8 + (icon ? tool->iconSize.width() + 6 : 0)), std::max(32, metrics.height() + 8)};
        }
        if (!text) {
            const int side = std::max(Metric::iconTarget, (icon ? tool->iconSize.height() : 16) + 14);
            return {side + (split ? Metric::splitArrow : 0), side};
        }
        QFont f = widget ? widget->font() : QApplication::font();
        if (name == "primary") f.setWeight(QFont::DemiBold);
        const QFontMetrics metrics(f);
        int w = int(std::ceil(QFontMetricsF(f).horizontalAdvance(QString(tool->text).remove(QRegularExpression(QStringLiteral("&(?!&)")))))) + 2 * 14 + 2 + (icon ? tool->iconSize.width() + 8 : 0);
        if (split) w += Metric::splitArrow - 6;
        else if (menu) w += Metric::splitZone - 6;
        if (flushButton(widget)) w -= 14;
        return {w, std::max(Metric::control, metrics.height() + 14)};
    }
    case CT_LineEdit: {
        const auto* frame = qstyleoption_cast<const QStyleOptionFrame*>(option);
        if (role(widget) == "editableTitle") return contents + QSize(4, 10);
        if (frame && frame->lineWidth <= 0) break;
        return {contents.width() + 22, std::max(Metric::control, contents.height() + 14)};
    }
    case CT_ComboBox: {
        const auto* combo = qstyleoption_cast<const QStyleOptionComboBox*>(option);
        if (combo && !combo->frame) break;
        return {contents.width() + 12 + Metric::splitZone + 6, std::max(Metric::control, contents.height() + 14)};
    }
    case CT_SpinBox: {
        const auto* spin = qstyleoption_cast<const QStyleOptionSpinBox*>(option);
        if (spin && !spin->frame) break;
        const int buttons = spin && spin->buttonSymbols == QAbstractSpinBox::NoButtons ? 0 : 64;
        return {contents.width() + 12 + buttons + 4, std::max(Metric::control, contents.height() + 14)};
    }
    case CT_CheckBox: case CT_RadioButton: {
        auto size = QProxyStyle::sizeFromContents(type, option, contents, widget);
        return {size.width(), std::max(size.height(), 32)};
    }
    case CT_MenuItem: {
        const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option);
        if (!item) break;
        if (item->menuItemType == QStyleOptionMenuItem::Separator) return {contents.width(), 9};
        const QFontMetrics metrics(item->font);
        auto text = item->text;
        QString shortcut;
        if (const int tab = text.indexOf('\t'); tab >= 0) { shortcut = text.mid(tab + 1); text = text.left(tab); }
        int w = 12 + (item->menuHasCheckableItems ? 26 : 0) + (item->maxIconWidth > 0 ? scaled(Metric::icon) + 10 : 0) + metrics.size(Qt::TextSingleLine | Qt::TextShowMnemonic, text).width() + 16;
        if (!shortcut.isEmpty()) w += 28 + int(std::ceil(QFontMetricsF(font(Type::metadata)).horizontalAdvance(shortcut)));
        else if (item->reservedShortcutWidth > 0) w += 28 + item->reservedShortcutWidth;
        if (item->menuItemType == QStyleOptionMenuItem::SubMenu) w += 20;
        return {w, std::max(32, metrics.height() + 12)};
    }
    case CT_TabBarTab: {
        const auto* tab = qstyleoption_cast<const QStyleOptionTab*>(option);
        if (!tab) break;
        const QFontMetrics metrics(font(Type::body, true));
        const int icon = tab->icon.isNull() ? 0 : tab->iconSize.width() + 8;
        return {std::max(96, metrics.horizontalAdvance(QString(tab->text).remove('&')) + icon + 40), 40};
    }
    case CT_HeaderSection: {
        auto size = QProxyStyle::sizeFromContents(type, option, contents, widget);
        return {size.width(), std::max(size.height(), 34)};
    }
    case CT_ItemViewItem: {
        auto size = QProxyStyle::sizeFromContents(type, option, contents, widget);
        if (widget) if (const int rows = widget->property("graphiteRowHeight").toInt(); rows > 0) size.setHeight(std::max(size.height(), rows));
        return size;
    }
    default: break;
    }
    return QProxyStyle::sizeFromContents(type, option, contents, widget);
}

QRect Style::subElementRect(SubElement element, const QStyleOption* option, const QWidget* widget) const {
    switch (element) {
    case SE_LineEditContents: {
        const auto* frame = qstyleoption_cast<const QStyleOptionFrame*>(option);
        // A small inset keeps a semibold first glyph's stem clear of the clip.
        if (role(widget) == "editableTitle") return option->rect.adjusted(widget->hasFocus() ? 3 : -2, 0, -3, -2);
        if (frame && frame->lineWidth > 0) return option->rect.adjusted(10, 1, -10, -1);
        return option->rect;
    }
    case SE_PushButtonFocusRect: return option->rect;
    case SE_PushButtonContents: return option->rect.adjusted(10, 2, -10, -2);
    default: return QProxyStyle::subElementRect(element, option, widget);
    }
}

QRect Style::subControlRect(ComplexControl control, const QStyleOptionComplex* option, SubControl sub, const QWidget* widget) const {
    const QRect r = option->rect;
    switch (control) {
    case CC_Slider: {
        const auto* slider = qstyleoption_cast<const QStyleOptionSlider*>(option);
        if (!slider) break;
        const bool horizontal = slider->orientation == Qt::Horizontal;
        const int length = pixelMetric(PM_SliderLength, option, widget);
        const int thickness = pixelMetric(PM_SliderControlThickness, option, widget);
        const int position = sliderPositionFromValue(slider->minimum, slider->maximum, slider->sliderPosition, std::max(0, (horizontal ? r.width() : r.height()) - length), slider->upsideDown);
        if (sub == SC_SliderHandle)
            return horizontal ? QRect(r.left() + position, r.top() + (r.height() - thickness) / 2, length, thickness)
                              : QRect(r.left() + (r.width() - thickness) / 2, r.top() + position, thickness, length);
        if (sub == SC_SliderGroove) return r;
        break;
    }
    case CC_ComboBox: {
        const auto* combo = qstyleoption_cast<const QStyleOptionComboBox*>(option);
        if (combo && !combo->frame) break;
        switch (sub) {
        case SC_ComboBoxFrame: case SC_ComboBoxListBoxPopup: return r;
        case SC_ComboBoxArrow: return visualRect(option->direction, r, QRect(r.right() - Metric::splitZone - 2, r.top(), Metric::splitZone + 2, r.height()));
        case SC_ComboBoxEditField: return visualRect(option->direction, r, r.adjusted(12, 1, -(Metric::splitZone + 4), -1));
        default: break;
        }
        break;
    }
    case CC_SpinBox: {
        const auto* spin = qstyleoption_cast<const QStyleOptionSpinBox*>(option);
        if (!spin || !spin->frame) break;
        const int buttons = spin->buttonSymbols == QAbstractSpinBox::NoButtons ? 0 : 64;
        switch (sub) {
        case SC_SpinBoxFrame: return r;
        case SC_SpinBoxEditField: return visualRect(option->direction, r, r.adjusted(11, 1, -(buttons + 3), -1));
        case SC_SpinBoxUp: return buttons ? visualRect(option->direction, r, QRect(r.right() - 32, r.top(), 32, r.height())) : QRect();
        case SC_SpinBoxDown: return buttons ? visualRect(option->direction, r, QRect(r.right() - 64, r.top(), 32, r.height())) : QRect();
        default: break;
        }
        break;
    }
    case CC_ScrollBar: {
        const auto* bar = qstyleoption_cast<const QStyleOptionSlider*>(option);
        if (!bar) break;
        const bool horizontal = bar->orientation == Qt::Horizontal;
        const int length = horizontal ? r.width() : r.height();
        int slider = length;
        if (bar->maximum != bar->minimum) {
            const qint64 range = qint64(bar->maximum) - bar->minimum;
            slider = int(qint64(bar->pageStep) * length / (range + bar->pageStep));
            slider = std::clamp(slider, std::min(length, pixelMetric(PM_ScrollBarSliderMin, option, widget)), length);
        }
        const int start = sliderPositionFromValue(bar->minimum, bar->maximum, bar->sliderPosition, length - slider, bar->upsideDown);
        auto span = [&](int from, int size) { return horizontal ? QRect(r.left() + from, r.top(), size, r.height()) : QRect(r.left(), r.top() + from, r.width(), size); };
        QRect result;
        switch (sub) {
        case SC_ScrollBarSubLine: case SC_ScrollBarAddLine: return {};
        case SC_ScrollBarSubPage: result = span(0, start); break;
        case SC_ScrollBarAddPage: result = span(start + slider, length - start - slider); break;
        case SC_ScrollBarGroove: result = r; break;
        case SC_ScrollBarSlider: result = span(start, slider); break;
        default: break;
        }
        return visualRect(bar->direction, r, result);
    }
    case CC_ToolButton: {
        const auto* tool = qstyleoption_cast<const QStyleOptionToolButton*>(option);
        if (!tool || !(tool->features & QStyleOptionToolButton::MenuButtonPopup)) break;
        if (sub == SC_ToolButtonMenu) return visualRect(option->direction, r, QRect(r.right() - Metric::splitArrow + 1, r.top(), Metric::splitArrow, r.height()));
        if (sub == SC_ToolButton) return visualRect(option->direction, r, r.adjusted(0, 0, -Metric::splitArrow, 0));
        break;
    }
    default: break;
    }
    return QProxyStyle::subControlRect(control, option, sub, widget);
}

QStyle::SubControl Style::hitTestComplexControl(ComplexControl control, const QStyleOptionComplex* option, const QPoint& point, const QWidget* widget) const {
    if (control == CC_Slider) {
        if (subControlRect(control, option, SC_SliderHandle, widget).contains(point)) return SC_SliderHandle;
        if (subControlRect(control, option, SC_SliderGroove, widget).contains(point)) return SC_SliderGroove;
        return SC_None;
    }
    return QProxyStyle::hitTestComplexControl(control, option, point, widget);
}

QPixmap Style::generatedIconPixmap(QIcon::Mode mode, const QPixmap& pixmap, const QStyleOption* option) const {
    if (mode == QIcon::Disabled) return tinted(pixmap, t().textDisabled);
    // Selection is a neutral fill here, not the highlight colour: a pixmap icon
    // without a selected variant stays as it is.
    if (mode == QIcon::Selected) return pixmap;
    return QProxyStyle::generatedIconPixmap(mode, pixmap, option);
}

void Style::drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* p, const QWidget* widget) const {
    const auto& c = t();
    switch (element) {
    case PE_FrameFocusRect: case PE_FrameDefaultButton: case PE_FrameButtonTool: case PE_FrameButtonBevel: case PE_FrameStatusBarItem:
    case PE_FrameTabBarBase: case PE_FrameTabWidget: case PE_IndicatorButtonDropDown: case PE_PanelScrollAreaCorner: case PE_FrameDockWidget:
        return;
    case PE_PanelButtonCommand: case PE_PanelButtonBevel: case PE_PanelButtonTool: {
        const auto look = buttonLook(buttonRole(widget, option), option, element == PE_PanelButtonTool && (option->state & State_AutoRaise), widget);
        QColor stroke = keyboardState(option) ? c.accent : look.stroke;
        if (!look.fill.isValid() && !stroke.isValid()) return;
        rounded(p, option->rect, look.fill.isValid() ? look.fill : QColor(Qt::transparent), stroke, Metric::radius, keyboardState(option) ? 1.5 : 1);
        if (widget && qobject_cast<SegmentTrack*>(widget->parentWidget()) && (option->state & State_On))
            p->fillRect(QRectF(option->rect.center().x() - 12, option->rect.bottom() - 3, 24, 2), c.accent);
        return;
    }
    case PE_PanelLineEdit: {
        const auto* frame = qstyleoption_cast<const QStyleOptionFrame*>(option);
        if (role(widget) == "editableTitle") {
            // A quiet solid underline on hover, the accent while editing; none at rest.
            const bool focused = option->state & State_HasFocus;
            if (!focused && !(option->state & State_MouseOver)) return;
            p->save();
            QPen pen(focused ? c.accent : c.borderStrong, focused ? 2 : 1, Qt::SolidLine);
            p->setPen(pen);
            const qreal y = option->rect.bottom() - (focused ? 0.5 : 0);
            p->drawLine(QPointF(option->rect.left(), y), QPointF(option->rect.right(), y));
            p->restore();
            return;
        }
        if (!frame || frame->lineWidth <= 0) return;
        field(p, option->rect, option, widget, option->state & State_HasFocus, widget && widget->property("hasUnit").toBool());
        return;
    }
    case PE_FrameLineEdit: return;
    case PE_Frame: case PE_FrameMenu: {
        if (element == PE_FrameMenu) {
            p->save();
            p->setPen(c.borderStrong);
            p->setBrush(Qt::NoBrush);
            p->drawRect(option->rect.adjusted(0, 0, -1, -1));
            p->restore();
            return;
        }
        if (qobject_cast<const QPlainTextEdit*>(widget) || qobject_cast<const QTextEdit*>(widget) || (widget && widget->property("graphiteFieldFrame").toBool())) {
            if (widget->objectName() == "historySummary") return;
            field(p, option->rect, option, widget, widget->hasFocus());
            return;
        }
        p->save();
        p->setPen(c.border);
        p->setBrush(Qt::NoBrush);
        p->drawRect(QRectF(option->rect).adjusted(0.5, 0.5, -0.5, -0.5));
        p->restore();
        return;
    }
    case PE_PanelMenu: p->fillRect(option->rect, c.surfaceRaised); return;
    case PE_PanelTipLabel:
        p->fillRect(option->rect, c.surfaceRaised);
        p->save();
        p->setPen(c.borderStrong);
        p->drawRect(option->rect.adjusted(0, 0, -1, -1));
        p->restore();
        return;
    case PE_IndicatorCheckBox: case PE_IndicatorItemViewItemCheck: {
        const int side = std::min({option->rect.width(), option->rect.height(), 17});
        const QRectF box(option->rect.x() + (option->rect.width() - side) / 2.0, option->rect.y() + (option->rect.height() - side) / 2.0, side, side);
        check(p, box, option, element == PE_IndicatorCheckBox && keyboardState(option), lockedLook(option, widget));
        return;
    }
    case PE_IndicatorRadioButton: {
        const int side = std::min({option->rect.width(), option->rect.height(), 18});
        const QRectF box(option->rect.x() + (option->rect.width() - side) / 2.0, option->rect.y() + (option->rect.height() - side) / 2.0, side, side);
        radio(p, box, option, keyboardState(option), lockedLook(option, widget));
        return;
    }
    case PE_IndicatorArrowDown: case PE_IndicatorArrowUp: case PE_IndicatorArrowLeft: case PE_IndicatorArrowRight: case PE_IndicatorHeaderArrow: {
        Qt::ArrowType direction = element == PE_IndicatorArrowUp ? Qt::UpArrow : element == PE_IndicatorArrowLeft ? Qt::LeftArrow : element == PE_IndicatorArrowRight ? Qt::RightArrow : Qt::DownArrow;
        if (element == PE_IndicatorHeaderArrow) if (const auto* header = qstyleoption_cast<const QStyleOptionHeader*>(option)) direction = header->sortIndicator == QStyleOptionHeader::SortUp ? Qt::UpArrow : Qt::DownArrow;
        const qreal size = std::min<qreal>(10, std::min(option->rect.width(), option->rect.height()) * 0.7);
        paintChevron(*p, QRectF(option->rect).center(), size, direction, option->state & State_Enabled ? c.textSecondary : c.textDisabled);
        return;
    }
    case PE_IndicatorBranch: {
        if (!(option->state & State_Children)) return;
        paintChevron(*p, QRectF(option->rect).center(), 8, option->state & State_Open ? Qt::DownArrow : Qt::RightArrow, c.textMuted);
        return;
    }
    case PE_PanelItemViewItem: case PE_PanelItemViewRow: {
        const auto* item = qstyleoption_cast<const QStyleOptionViewItem*>(option);
        const bool selected = option->state & State_Selected;
        const bool hover = (option->state & State_MouseOver) && (option->state & State_Enabled);
        if (element == PE_PanelItemViewRow) {
            if (item && (item->features & QStyleOptionViewItem::Alternate) && !selected) p->fillRect(option->rect, c.surface);
            return;
        }
        if (!selected && !hover) return;
        QColor fill = selected ? c.selected : mix(c.surface, c.surfaceRaised, 0.6);
        bool left = true, right = true;
        if (item) switch (item->viewItemPosition) {
            case QStyleOptionViewItem::Beginning: right = false; break;
            case QStyleOptionViewItem::Middle: left = right = false; break;
            case QStyleOptionViewItem::End: left = false; break;
            default: break;
            }
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(Qt::NoPen);
        p->setBrush(fill);
        const QRectF box = QRectF(option->rect).adjusted(left ? 2 : 0, 1, right ? -2 : 0, -1);
        p->drawPath(partlyRounded(box, 6, left, right));
        if ((option->state & State_HasFocus) && widget && keyboardFocus(widget) && selected) {
            p->setPen(QPen(c.accent, 1));
            p->setBrush(Qt::NoBrush);
            p->drawPath(partlyRounded(box.adjusted(0.5, 0.5, -0.5, -0.5), 6, left, right));
        }
        p->restore();
        return;
    }
    case PE_FrameGroupBox: {
        p->save();
        p->setPen(c.border);
        p->drawLine(option->rect.topLeft(), option->rect.topRight());
        p->restore();
        return;
    }
    case PE_IndicatorToolBarSeparator: {
        p->save();
        p->setPen(c.border);
        const auto r = option->rect;
        if (option->state & State_Horizontal) p->drawLine(r.center().x(), r.top() + 6, r.center().x(), r.bottom() - 6);
        else p->drawLine(r.left() + 6, r.center().y(), r.right() - 6, r.center().y());
        p->restore();
        return;
    }
    case PE_PanelStatusBar: p->fillRect(option->rect, c.surface); return;
    case PE_Widget: return;
    default: break;
    }
    QProxyStyle::drawPrimitive(element, option, p, widget);
}

void Style::drawButtonLabel(const QStyleOption* option, const QString& rawText, const QIcon& icon, QSize iconSize, QPainter* p, const QWidget* widget, QRect body) const {
    const auto name = buttonRole(widget, option);
    const auto look = buttonLook(name, option, false, widget);
    QFont f = widget ? widget->font() : QApplication::font();
    if (look.strong) f.setWeight(QFont::DemiBold);
    f.setUnderline(look.underline);
    const QFontMetricsF metrics(f);
    const bool hasIcon = !icon.isNull() && iconSize.isValid() && iconSize.width() > 0;
    const int iconWidth = hasIcon ? iconSize.width() : 0;
    const QString plain = QString(rawText).remove(QRegularExpression(QStringLiteral("&(?!&)")));
    const qreal available = std::max<qreal>(0, body.width() - iconWidth - (hasIcon && !plain.isEmpty() ? 8 : 0));
    const QString shown = metrics.elidedText(plain, Qt::ElideRight, available);
    const qreal textWidth = shown.isEmpty() ? 0 : metrics.horizontalAdvance(shown);
    const qreal group = iconWidth + (hasIcon && !shown.isEmpty() ? 8 : 0) + textWidth;
    const bool leftAligned = flushButton(widget) || name == "link" || name == "version" || (widget && widget->property("alignLeft").toBool());
    qreal x = leftAligned ? body.left() : body.left() + (body.width() - group) / 2.0;
    if (hasIcon) {
        const bool enabled = option->state & State_Enabled;
        const QRectF target(std::round(x), body.top() + (body.height() - iconSize.height()) / 2.0, iconSize.width(), iconSize.height());
        // A checked or pressed quiet or secondary button sits on the selected fill.
        const bool onSelected = enabled && ((option->state & State_Sunken) || ((option->state & State_On) && !(widget && widget->property("graphiteDisclosure").toBool()))) && name != "primary" && name != "danger" && name != "link" && name != "version";
        auto pixmap = icon.pixmap(iconSize, dpr(widget), !enabled ? QIcon::Disabled : onSelected ? QIcon::Selected : QIcon::Normal, option->state & State_On ? QIcon::On : QIcon::Off);
        if (name == "primary" || name == "danger") pixmap = tinted(pixmap, look.text);
        if (widget && widget->property("headerInactive").toBool()) pixmap = tinted(pixmap, t().textMuted);
        p->drawPixmap(target.topLeft(), pixmap);
        x += iconWidth + (shown.isEmpty() ? 0 : 8);
    }
    if (!shown.isEmpty()) {
        p->save();
        p->setFont(f);
        p->setPen(look.text);
        const qreal baseline = opticalBaseline(metrics, shown, body.top() + body.height() / 2.0);
        p->drawText(QPointF(x, baseline), shown);
        p->restore();
    }
}

void Style::drawControl(ControlElement element, const QStyleOption* option, QPainter* p, const QWidget* widget) const {
    const auto& c = t();
    switch (element) {
    case CE_PushButtonBevel: {
        const auto* button = qstyleoption_cast<const QStyleOptionButton*>(option);
        drawPrimitive(PE_PanelButtonCommand, option, p, widget);
        if (button && (button->features & QStyleOptionButton::HasMenu) && !(widget && widget->property("noChevron").toBool())) {
            const auto look = buttonLook(buttonRole(widget, option), option, false, widget);
            paintChevron(*p, QPointF(option->rect.right() - Metric::splitZone / 2.0 + 1, option->rect.center().y() + 0.5), 8, Qt::DownArrow, look.text);
        }
        return;
    }
    case CE_PushButton: {
        drawControl(CE_PushButtonBevel, option, p, widget);
        drawControl(CE_PushButtonLabel, option, p, widget);
        return;
    }
    case CE_PushButtonLabel: {
        const auto* button = qstyleoption_cast<const QStyleOptionButton*>(option);
        if (!button) break;
        const auto name = buttonRole(widget, option);
        QRect body = (name == "link" || name == "version") ? option->rect.adjusted(4, 0, -4, 0) : option->rect.adjusted(12, 0, -12, 0);
        if (flushButton(widget)) body.setLeft(option->rect.left());
        if ((button->features & QStyleOptionButton::HasMenu) && !(widget && widget->property("noChevron").toBool())) body.setRight(option->rect.right() - Metric::splitZone);
        drawButtonLabel(option, button->text, button->icon, button->iconSize, p, widget, body);
        return;
    }
    case CE_ToolButtonLabel: {
        const auto* tool = qstyleoption_cast<const QStyleOptionToolButton*>(option);
        if (!tool) break;
        if (tool->toolButtonStyle == Qt::ToolButtonTextUnderIcon) break;
        const auto name = buttonRole(widget, option);
        const bool textShown = tool->toolButtonStyle != Qt::ToolButtonIconOnly && !tool->text.isEmpty();
        const bool iconShown = tool->toolButtonStyle != Qt::ToolButtonTextOnly && !tool->icon.isNull();
        QRect body = textShown ? option->rect.adjusted(name == "link" || name == "version" ? 4 : 12, 0, name == "link" || name == "version" ? -4 : -12, 0) : option->rect;
        if (flushButton(widget)) body.setLeft(option->rect.left());
        drawButtonLabel(option, textShown ? tool->text : QString(), iconShown ? tool->icon : QIcon(), tool->iconSize, p, widget, body);
        return;
    }
    case CE_CheckBoxLabel: case CE_RadioButtonLabel: {
        const auto* button = qstyleoption_cast<const QStyleOptionButton*>(option);
        if (!button) break;
        QStyleOptionButton copy(*button);
        copy.palette.setColor(QPalette::WindowText, option->state & State_Enabled ? c.text : lockedLook(option, widget) ? c.textMuted : c.textDisabled);
        QProxyStyle::drawControl(element, &copy, p, widget);
        return;
    }
    case CE_MenuItem: {
        const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option);
        if (!item) break;
        const QRect r = option->rect;
        if (item->menuItemType == QStyleOptionMenuItem::Separator) {
            p->save();
            p->setPen(c.border);
            p->drawLine(r.left() + 10, r.center().y(), r.right() - 10, r.center().y());
            p->restore();
            return;
        }
        if (item->menuItemType == QStyleOptionMenuItem::EmptyArea) return;
        const bool enabled = option->state & State_Enabled;
        const bool selected = enabled && (option->state & State_Selected);
        if (selected) rounded(p, QRectF(r).adjusted(2, 1, -2, -1), c.selected, {}, 5);
        const QColor ink = enabled ? (selected ? c.text : c.textSecondary) : c.textDisabled;
        int x = r.left() + 12;
        if (item->menuHasCheckableItems) {
            const QRectF box(x, r.center().y() - 7.5, 15, 15);
            if (item->checkType == QStyleOptionMenuItem::Exclusive) {
                if (item->checked) {
                    p->save();
                    p->setRenderHint(QPainter::Antialiasing);
                    p->setPen(Qt::NoPen);
                    p->setBrush(ink);
                    p->drawEllipse(box.center(), 3.5, 3.5);
                    p->restore();
                }
            } else if (item->checkType == QStyleOptionMenuItem::NonExclusive) {
                // A checkable item shows its state either way: a tick when on,
                // an empty box when off.
                if (item->checked) paintChromeGlyph(*p, box, QStringLiteral("check"), ink);
                else rounded(p, box.adjusted(1.5, 1.5, -1.5, -1.5), {}, enabled ? c.textMuted : c.textDisabled, 3);
            }
            x += 26;
        }
        if (item->maxIconWidth > 0) {
            if (!item->icon.isNull()) {
                const auto pixmap = item->icon.pixmap(QSize(scaled(Metric::icon), scaled(Metric::icon)), dpr(widget), !enabled ? QIcon::Disabled : selected ? QIcon::Selected : QIcon::Normal);
                p->drawPixmap(QPointF(x, r.center().y() - scaled(Metric::icon) / 2.0), pixmap);
            }
            x += scaled(Metric::icon) + 10;
        }
        auto text = item->text;
        QString shortcut;
        if (const int tab = text.indexOf('\t'); tab >= 0) { shortcut = text.mid(tab + 1); text = text.left(tab); }
        int right = r.right() - 12 - (item->menuItemType == QStyleOptionMenuItem::SubMenu ? 20 : 0);
        if (!shortcut.isEmpty()) {
            const auto f = font(Type::metadata);
            const int width = int(std::ceil(QFontMetricsF(f).horizontalAdvance(shortcut)));
            paintText(*p, QRectF(right - width, r.top(), width, r.height()), shortcut, enabled ? c.textMuted : c.textDisabled, f, Qt::AlignRight | Qt::AlignVCenter);
            right -= width + 24;
        }
        p->save();
        p->setFont(item->font);
        p->setPen(ink);
        const QRect textRect(x, r.top(), std::max(0, right - x), r.height());
        const auto elided = QFontMetrics(item->font).elidedText(text, Qt::ElideRight, textRect.width(), Qt::TextShowMnemonic);
        p->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft | Qt::TextShowMnemonic | Qt::TextSingleLine, elided);
        p->restore();
        if (item->menuItemType == QStyleOptionMenuItem::SubMenu) paintChevron(*p, QPointF(r.right() - 16, r.center().y()), 8, Qt::RightArrow, enabled ? c.textMuted : c.textDisabled);
        return;
    }
    case CE_MenuEmptyArea: case CE_MenuScroller: case CE_MenuTearoff: p->fillRect(option->rect, c.surfaceRaised); return;
    case CE_MenuBarEmptyArea: p->fillRect(option->rect, c.bg); return;
    case CE_MenuBarItem: {
        const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option);
        if (!item) break;
        p->fillRect(option->rect, c.bg);
        if (option->state & (State_Selected | State_Sunken)) rounded(p, QRectF(option->rect).adjusted(1, 3, -1, -3), c.selected, {}, 5);
        p->save();
        p->setPen(option->state & State_Enabled ? c.text : c.textDisabled);
        p->drawText(option->rect, Qt::AlignCenter | Qt::TextShowMnemonic, item->text);
        p->restore();
        return;
    }
    case CE_TabBarTabShape: {
        const auto* tab = qstyleoption_cast<const QStyleOptionTab*>(option);
        if (!tab || (tab->shape != QTabBar::RoundedNorth && tab->shape != QTabBar::TriangularNorth && tab->shape != QTabBar::RoundedSouth)) break;
        const QRectF r = QRectF(option->rect).adjusted(0, 2, 0, -2);
        const bool first = tab->position == QStyleOptionTab::Beginning || tab->position == QStyleOptionTab::OnlyOneTab;
        const bool last = tab->position == QStyleOptionTab::End || tab->position == QStyleOptionTab::OnlyOneTab;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(Qt::NoPen);
        p->setBrush(mix(c.bg, c.surface, 0.6));
        p->drawPath(partlyRounded(r, Metric::trackRadius, first, last));
        p->restore();
        if (option->state & State_Selected) {
            rounded(p, r.adjusted(3, 3, -3, -3), c.selected, keyboardState(option) ? c.accent : mix(c.borderStrong, c.selected, 0.7), Metric::segmentRadius);
        } else if (option->state & State_MouseOver) {
            rounded(p, r.adjusted(3, 3, -3, -3), mix(c.surface, c.surfaceRaised, 0.6), {}, Metric::segmentRadius);
        }
        return;
    }
    case CE_TabBarTabLabel: {
        const auto* tab = qstyleoption_cast<const QStyleOptionTab*>(option);
        if (!tab) break;
        const bool selected = option->state & State_Selected;
        const auto f = font(Type::body, selected);
        const QColor ink = !(option->state & State_Enabled) ? c.textDisabled : selected ? c.text : option->state & State_MouseOver ? c.textSecondary : c.textMuted;
        QRect r = option->rect;
        if (!tab->icon.isNull()) {
            const int width = QFontMetrics(f).horizontalAdvance(QString(tab->text).remove('&')) + tab->iconSize.width() + 8;
            const int x = r.center().x() - width / 2;
            p->drawPixmap(QPoint(x, r.center().y() - tab->iconSize.height() / 2), tab->icon.pixmap(tab->iconSize, dpr(widget)));
            r.setLeft(x + tab->iconSize.width() + 8);
            paintText(*p, r, QString(tab->text).remove('&'), ink, f, Qt::AlignLeft | Qt::AlignVCenter);
            return;
        }
        paintText(*p, r.adjusted(8, 0, -8, 0), QString(tab->text).remove('&'), ink, f, Qt::AlignHCenter | Qt::AlignVCenter);
        return;
    }
    case CE_HeaderSection: {
        p->fillRect(option->rect, widget && widget->parentWidget() ? widget->parentWidget()->palette().color(QPalette::Base) : c.bg);
        p->save();
        p->setPen(c.border);
        p->drawLine(option->rect.bottomLeft(), option->rect.bottomRight());
        p->restore();
        return;
    }
    case CE_HeaderLabel: {
        const auto* header = qstyleoption_cast<const QStyleOptionHeader*>(option);
        if (!header) break;
        paintText(*p, QRectF(option->rect).adjusted(8, 0, -8, 0), header->text, c.textMuted, font(Type::metadata), Qt::Alignment(header->textAlignment) | Qt::AlignVCenter);
        return;
    }
    case CE_HeaderEmptyArea: p->fillRect(option->rect, c.bg); return;
    case CE_ItemViewItem: {
        const auto* item = qstyleoption_cast<const QStyleOptionViewItem*>(option);
        if (!item) break;
        QStyleOptionViewItem copy(*item);
        for (const auto group : {QPalette::Active, QPalette::Inactive}) {
            copy.palette.setColor(group, QPalette::HighlightedText, c.text);
            copy.palette.setColor(group, QPalette::Highlight, c.selected);
        }
        QProxyStyle::drawControl(element, &copy, p, widget);
        return;
    }
    case CE_ProgressBarGroove: {
        const QRectF r = option->rect;
        const qreal h = std::min<qreal>(r.height(), 8);
        rounded(p, QRectF(r.left(), r.center().y() - h / 2, r.width(), h), c.surfaceRaised, {}, h / 2);
        return;
    }
    case CE_ProgressBarContents: {
        const auto* bar = qstyleoption_cast<const QStyleOptionProgressBar*>(option);
        if (!bar) break;
        if (bar->minimum == 0 && bar->maximum == 0) {
            auto* progress = qobject_cast<QProgressBar*>(const_cast<QWidget*>(widget));
            const bool animate = progressAnimationEnabled(widget, option);
            qreal phase = progress ? progress->property("graphiteBusyPhase").toDouble() : 0;
            if (progress) {
                auto* timer = progress->findChild<QTimer*>("graphiteBusyTimer", Qt::FindDirectChildrenOnly);
                if (!timer) {
                    timer = new QTimer(progress);
                    timer->setObjectName("graphiteBusyTimer");
                    timer->setInterval(60);
                    QObject::connect(timer, &QTimer::timeout, progress, [progress, timer] {
                        if (!progress->isVisible() || !progress->isEnabled() || progress->minimum() != 0 || progress->maximum() != 0
                            || !progressAnimationEnabled(progress)) { timer->stop(); return; }
                        progress->setProperty("graphiteBusyPhase", std::fmod(progress->property("graphiteBusyPhase").toDouble() + 0.015, 1.0));
                        progress->update();
                    });
                }
                if (animate && progress->isVisible() && progress->isEnabled()) { if (!timer->isActive()) timer->start(); }
                else timer->stop();
            }
            // Qt 6.4 has no reduced-motion preference API. Honour disabled style
            // animation; otherwise a quiet four-second sweep avoids flashing.
            const QRectF r = option->rect;
            const qreal h = std::min<qreal>(r.height(), 8), length = std::min(r.width(), std::max(h, r.width() * 0.28));
            const qreal position = animate ? (1 - std::cos(phase * 2 * 3.141592653589793)) / 2 : 0.5;
            const qreal x = r.left() + (r.width() - length) * (bar->invertedAppearance ? 1 - position : position);
            rounded(p, QRectF(x, r.center().y() - h / 2, length, h), option->state & State_Enabled ? c.text : c.textDisabled, {}, h / 2);
            return;
        }
        const QRectF r = option->rect;
        const qreal h = std::min<qreal>(r.height(), 8);
        const qreal fraction = std::clamp(double(qint64(bar->progress) - bar->minimum) / std::max<qint64>(1, qint64(bar->maximum) - bar->minimum), 0.0, 1.0);
        if (fraction <= 0) return;
        rounded(p, QRectF(r.left(), r.center().y() - h / 2, std::max(h, r.width() * fraction), h), option->state & State_Enabled ? c.text : c.textDisabled, {}, h / 2);
        return;
    }
    case CE_ProgressBarLabel: {
        const auto* bar = qstyleoption_cast<const QStyleOptionProgressBar*>(option);
        if (!bar || !bar->textVisible || option->rect.height() < 14) return;
        paintText(*p, option->rect, bar->text, c.textSecondary, font(Type::metadata), Qt::AlignCenter);
        return;
    }
    case CE_Splitter: {
        p->save();
        p->setPen(c.border);
        const auto r = option->rect;
        if (option->state & State_Horizontal) p->drawLine(r.center().x(), r.top(), r.center().x(), r.bottom());
        else p->drawLine(r.left(), r.center().y(), r.right(), r.center().y());
        p->restore();
        return;
    }
    case CE_ShapedFrame: {
        const auto* frame = qstyleoption_cast<const QStyleOptionFrame*>(option);
        const auto name = role(widget);
        const QRectF r = option->rect;
        if (name == "status") {
            const auto tone = widget->property("feedbackTone").toString();
            const QColor fill = tone == "success" ? c.successSurface : tone == "error" ? c.dangerSurface : c.surface;
            const QColor stroke = tone == "success" ? mix(c.success, c.successSurface, 0.45) : tone == "error" ? mix(c.danger, c.dangerSurface, 0.45) : c.border;
            rounded(p, r, fill, stroke, Metric::radius);
            return;
        }
        if (name == "notice") {
            rounded(p, r, c.surfaceRaised, c.borderStrong, Metric::radius);
            return;
        }
        if (name == "card") {
            p->save();
            p->setPen(c.border);
            p->drawLine(r.topLeft(), r.topRight());
            p->restore();
            return;
        }
        if (name == "separator") { p->fillRect(option->rect, c.border); return; }
        // A recessed area for media: previews and thumbnails sit on it.
        if (name == "well") { rounded(p, r, c.bg, c.border, Metric::radius); return; }
        if (name == "pill") { rounded(p, r, c.selected, c.borderStrong, std::min<qreal>(11, r.height() / 2)); return; }
        if (name == "unitSuffix") {
            p->save();
            p->setRenderHint(QPainter::Antialiasing);
            p->setPen(widget->isEnabled() ? controlOutline() : controlStroke());
            p->setBrush(widget->isEnabled() ? c.surfaceRaised : c.surface);
            p->drawPath(partlyRounded(r.adjusted(0.5, 0.5, -0.5, -0.5), Metric::radius, false, true));
            p->restore();
            return;
        }
        if (frame && (frame->frameShape == QFrame::HLine || frame->frameShape == QFrame::VLine)) {
            p->save();
            p->setPen(c.border);
            if (frame->frameShape == QFrame::HLine) p->drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
            else p->drawLine(QPointF(r.center().x(), r.top()), QPointF(r.center().x(), r.bottom()));
            p->restore();
            return;
        }
        if (frame && frame->frameShape == QFrame::NoFrame) return;
        if (frame && (frame->frameShape == QFrame::StyledPanel || frame->frameShape == QFrame::Panel || frame->frameShape == QFrame::Box || frame->frameShape == QFrame::WinPanel)) {
            drawPrimitive(PE_Frame, option, p, widget);
            return;
        }
        break;
    }
    case CE_RubberBand:
        rounded(p, option->rect, QColor(c.accent.red(), c.accent.green(), c.accent.blue(), 40), c.accent, 2);
        return;
    case CE_ComboBoxLabel: {
        const auto* combo = qstyleoption_cast<const QStyleOptionComboBox*>(option);
        if (!combo || combo->editable) break;
        QRect r = subControlRect(CC_ComboBox, combo, SC_ComboBoxEditField, widget);
        if (!combo->currentIcon.isNull()) {
            const auto pixmap = combo->currentIcon.pixmap(combo->iconSize, dpr(widget), option->state & State_Enabled ? QIcon::Normal : QIcon::Disabled);
            p->drawPixmap(QPoint(r.left(), r.center().y() - combo->iconSize.height() / 2), pixmap);
            r.setLeft(r.left() + combo->iconSize.width() + 8);
        }
        const auto f = widget ? widget->font() : QApplication::font();
        paintText(*p, r, combo->currentText, option->state & State_Enabled ? c.text : lockedLook(option, widget) ? c.textMuted : c.textDisabled, f, Qt::AlignLeft | Qt::AlignVCenter);
        return;
    }
    case CE_SizeGrip: return;
    default: break;
    }
    QProxyStyle::drawControl(element, option, p, widget);
}

void Style::drawComplexControl(ComplexControl control, const QStyleOptionComplex* option, QPainter* p, const QWidget* widget) const {
    const auto& c = t();
    switch (control) {
    case CC_ToolButton: {
        const auto* tool = qstyleoption_cast<const QStyleOptionToolButton*>(option);
        if (!tool || tool->toolButtonStyle == Qt::ToolButtonTextUnderIcon) break;
        const bool split = tool->features & QStyleOptionToolButton::MenuButtonPopup;
        const bool chevron = (tool->features & QStyleOptionToolButton::HasMenu) && !split && !(widget && widget->property("noChevron").toBool())
                             && !tool->text.isEmpty() && tool->toolButtonStyle != Qt::ToolButtonIconOnly;
        QStyleOption panel(*option);
        if (tool->activeSubControls & SC_ToolButtonMenu) panel.state |= State_Sunken;
        if (!(tool->state & State_Enabled)) panel.state &= ~(State_MouseOver | State_Sunken);
        drawPrimitive(PE_PanelButtonTool, &panel, p, widget);
        const auto look = buttonLook(buttonRole(widget, option), &panel, tool->state & State_AutoRaise, widget);
        QStyleOptionToolButton label(*tool);
        if (split || chevron) label.rect = option->rect.adjusted(0, 0, -(Metric::splitZone - 4), 0);
        if (split) label.rect = subControlRect(CC_ToolButton, tool, SC_ToolButton, widget);
        drawControl(CE_ToolButtonLabel, &label, p, widget);
        if (split) {
            const auto menu = subControlRect(CC_ToolButton, tool, SC_ToolButtonMenu, widget);
            p->save();
            p->setPen(mix(look.fill.isValid() && look.fill.alpha() ? look.fill : c.surfaceRaised, look.text, 0.25));
            p->drawLine(menu.left(), menu.top() + 9, menu.left(), menu.bottom() - 9);
            p->restore();
            paintChevron(*p, QRectF(menu).center() + QPointF(0, 0.5), 8, Qt::DownArrow, look.text);
        } else if (chevron) {
            paintChevron(*p, QPointF(option->rect.right() - Metric::splitZone / 2.0 + 2, option->rect.center().y() + 0.5), 8, Qt::DownArrow, look.text);
        }
        return;
    }
    case CC_ComboBox: {
        const auto* combo = qstyleoption_cast<const QStyleOptionComboBox*>(option);
        if (!combo || !combo->frame) break;
        const bool focused = combo->editable ? (option->state & State_HasFocus) : keyboardState(option) || (option->state & State_On);
        field(p, option->rect, option, widget, focused);
        // A locked value can't be opened: no chevron.
        if (lockedLook(option, widget)) return;
        const auto arrow = subControlRect(CC_ComboBox, combo, SC_ComboBoxArrow, widget);
        paintChevron(*p, QRectF(arrow).center() + QPointF(-2, 0.5), 9, Qt::DownArrow, option->state & State_Enabled ? c.textSecondary : c.textDisabled);
        return;
    }
    case CC_SpinBox: {
        const auto* spin = qstyleoption_cast<const QStyleOptionSpinBox*>(option);
        if (!spin || !spin->frame) break;
        field(p, option->rect, option, widget, option->state & State_HasFocus, widget && widget->property("hasUnit").toBool());
        if (spin->buttonSymbols == QAbstractSpinBox::NoButtons || lockedLook(option, widget)) return;
        for (const auto sub : {SC_SpinBoxUp, SC_SpinBoxDown}) {
            const auto box = subControlRect(CC_SpinBox, spin, sub, widget);
            const bool enabled = (option->state & State_Enabled) && (spin->stepEnabled & (sub == SC_SpinBoxUp ? QAbstractSpinBox::StepUpEnabled : QAbstractSpinBox::StepDownEnabled));
            if (enabled && (spin->activeSubControls & sub) && (option->state & (State_MouseOver | State_Sunken)))
                rounded(p, QRectF(box).adjusted(2, 0, 0, 0), mix(c.surfaceRaised, c.selected, (option->state & State_Sunken) ? 1.0 : 0.5), {}, 4);
            p->setPen(QPen(enabled ? c.textSecondary : c.textDisabled, 1.5, Qt::SolidLine, Qt::RoundCap));
            const QPointF centre = QRectF(box).center();
            p->drawLine(centre + QPointF(-4, 0), centre + QPointF(4, 0));
            if (sub == SC_SpinBoxUp) p->drawLine(centre + QPointF(0, -4), centre + QPointF(0, 4));
        }
        return;
    }
    case CC_ScrollBar: {
        const auto* bar = qstyleoption_cast<const QStyleOptionSlider*>(option);
        if (!bar) break;
        const auto slider = subControlRect(CC_ScrollBar, bar, SC_ScrollBarSlider, widget);
        if (slider.isEmpty() || bar->minimum == bar->maximum) return;
        const bool active = (bar->activeSubControls & SC_ScrollBarSlider) && (option->state & (State_MouseOver | State_Sunken));
        const QRectF handle = bar->orientation == Qt::Horizontal ? QRectF(slider).adjusted(1, 2, -1, -2) : QRectF(slider).adjusted(2, 1, -2, -1);
        rounded(p, handle, active ? c.textMuted : c.controlOutline, {}, 3);
        return;
    }
    case CC_Slider: {
        const auto* slider = qstyleoption_cast<const QStyleOptionSlider*>(option);
        if (!slider) break;
        const auto track = subControlRect(CC_Slider, slider, SC_SliderGroove, widget);
        const auto handle = subControlRect(CC_Slider, slider, SC_SliderHandle, widget);
        const bool horizontal = slider->orientation == Qt::Horizontal;
        const auto groove = horizontal ? track.adjusted((handle.width() - 1) / 2, 0, -handle.width() / 2, 0)
                                       : track.adjusted(0, (handle.height() - 1) / 2, 0, -handle.height() / 2);
        const bool enabled = option->state & State_Enabled;
        if (horizontal) {
            const qreal y = groove.center().y() - 1.5;
            rounded(p, QRectF(groove.left() + 2, y, groove.width() - 4, 3), c.controlOutline, {}, 1.5);
            const qreal filled = slider->upsideDown ? groove.right() - handle.center().x() : handle.center().x() - groove.left();
            rounded(p, QRectF(slider->upsideDown ? handle.center().x() : groove.left() + 2, y, std::max<qreal>(0, filled - 2), 3), enabled ? c.accent : c.textDisabled, {}, 1.5);
        } else {
            const qreal x = groove.center().x() - 1.5;
            rounded(p, QRectF(x, groove.top() + 2, 3, groove.height() - 4), c.controlOutline, {}, 1.5);
        }
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(keyboardState(option) ? QPen(c.accent, 2) : Qt::NoPen);
        p->setBrush(enabled ? c.text : c.textDisabled);
        p->drawEllipse(QRectF(handle).center(), 6, 6);
        p->restore();
        return;
    }
    case CC_GroupBox: break;
    default: break;
    }
    QProxyStyle::drawComplexControl(control, option, p, widget);
}

BrowserFeedback::BrowserFeedback(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    message_ = new QLabel("Couldn't open the browser", this);
    message_->setWordWrap(true);
    message_->setTextFormat(Qt::PlainText);
    VisualStyle::role(message_, "danger");
    layout->addWidget(message_);
    copy_ = new QPushButton("Copy link", this);
    VisualStyle::role(copy_, "link");
    layout->addWidget(copy_, 0, Qt::AlignLeft);
    connect(copy_, &QPushButton::clicked, this, [this] { qApp->clipboard()->setText(url_.toString(QUrl::FullyEncoded)); });
    hide();
}
void BrowserFeedback::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    QTimer::singleShot(0, this, [this] {
        if (!isVisible()) return;
        for (auto* owner = parentWidget(); owner; owner = owner->parentWidget()) if (auto* scroll = qobject_cast<QScrollArea*>(owner)) {
            if (auto* layout = scroll->widget()->layout()) layout->activate();
            scroll->ensureWidgetVisible(this, 24, 24);
            break;
        }
    });
}
void BrowserFeedback::clear() { ++request_; hide(); }
std::function<void(const QJsonObject&)> BrowserFeedback::opening(const QUrl& url) {
    hide();
    url_ = url;
    const auto request = ++request_;
    return [guard = QPointer<BrowserFeedback>(this), request](const QJsonObject& result) {
        if (!guard || request != guard->request_) return;
        guard->setVisible(!result["opened"].toBool());
    };
}

// ---------------------------------------------------------------- Widgets
Surface::Surface(const char* role, Qt::Edges hairlines, QWidget* parent) : QWidget(parent), role_(role), edges_(hairlines) {}
void Surface::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const auto& c = t();
    p.fillRect(rect(), role_ == "surface" ? c.surface : role_ == "raised" ? c.surfaceRaised : c.bg);
    p.setPen(c.border);
    if (edges_ & Qt::TopEdge) p.drawLine(0, 0, width(), 0);
    if (edges_ & Qt::BottomEdge) p.drawLine(0, height() - 1, width(), height() - 1);
    if (edges_ & Qt::LeftEdge) p.drawLine(0, 0, 0, height());
    if (edges_ & Qt::RightEdge) p.drawLine(width() - 1, 0, width() - 1, height());
}
QFrame* rule(QWidget* parent, Qt::Orientation orientation) {
    auto* line = new QFrame(parent);
    line->setFrameShape(orientation == Qt::Horizontal ? QFrame::HLine : QFrame::VLine);
    if (orientation == Qt::Horizontal) line->setFixedHeight(1);
    else line->setFixedWidth(1);
    return line;
}
QLabel* label(const QString& text, const char* role, QWidget* parent) {
    auto* widget = new QLabel(text, parent);
    if (QByteArray(role) != "body") VisualStyle::role(widget, role);
    return widget;
}
QToolButton* toolButton(const QString& text, const QString& glyph, const char* role, QWidget* parent) {
    auto* button = new QToolButton(parent);
    // A literal "&" (Capture region & edit) is not a mnemonic marker.
    button->setText(QString(text).replace('&', QLatin1String("&&")));
    button->setAccessibleName(text);
    if (!glyph.isEmpty()) button->setIcon(VisualStyle::icon(glyph));
    button->setIconSize(QSize(Metric::icon, Metric::icon));
    button->setToolButtonStyle(text.isEmpty() ? Qt::ToolButtonIconOnly : glyph.isEmpty() ? Qt::ToolButtonTextOnly : Qt::ToolButtonTextBesideIcon);
    button->setFocusPolicy(Qt::StrongFocus);
    if (role) button->setProperty("visualRole", role);
    return button;
}

namespace {
// The full text of a revealable label: a raised panel with a hairline, the
// text selectable by mouse and keyboard, closed by Escape or a click outside.
class FullTextPopup final : public Surface {
public:
    FullTextPopup(const QString& text, const QString& title, QWidget* owner) : Surface("raised", Qt::TopEdge | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge, owner) {
        setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
        setAttribute(Qt::WA_DeleteOnClose);
        setObjectName("elidedLabelFullText");
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(8);
        if (!title.isEmpty()) layout->addWidget(Graphite::label(title, "meta", this));
        label_ = new QLabel(text, this);
        label_->setObjectName("elidedLabelFullTextValue");
        label_->setTextFormat(Qt::PlainText);
        label_->setWordWrap(true);
        label_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        label_->setFocusPolicy(Qt::StrongFocus);
        label_->setAccessibleName(text);
        auto* scroll = new QScrollArea(this);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setWidget(label_);
        if (!title.isEmpty()) { scroll->setProperty("graphiteFieldFrame", true); scroll->setFrameShape(QFrame::StyledPanel); label_->setMargin(8); }
        layout->addWidget(scroll);
        label_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        setAccessibleName(text);
    }
    QLabel* label() const { return label_; }
protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) { close(); return; }
        Surface::keyPressEvent(event);
    }
private:
    QLabel* label_;
};
}
namespace {
class AccessibleElidedLabel final : public QAccessibleWidget {
public:
    explicit AccessibleElidedLabel(ElidedLabel* label) : QAccessibleWidget(label) {}
    QAccessible::Role role() const override { return label()->revealable() ? QAccessible::Button : QAccessible::StaticText; }
    QStringList actionNames() const override {
        auto actions = QAccessibleWidget::actionNames();
        if (label()->revealable() && !actions.contains(QAccessibleActionInterface::pressAction())) actions.prepend(QAccessibleActionInterface::pressAction());
        return actions;
    }
    QString localizedActionName(const QString& action) const override {
        return action == QAccessibleActionInterface::pressAction() && label()->revealable() ? label()->revealActionName() : QAccessibleWidget::localizedActionName(action);
    }
    void doAction(const QString& action) override {
        if (label()->revealable() && action == QAccessibleActionInterface::pressAction()) label()->reveal();
        else QAccessibleWidget::doAction(action);
    }
    QStringList keyBindingsForAction(const QString& action) const override {
        return action == QAccessibleActionInterface::pressAction() && label()->revealable() ? QStringList{"Enter", "Space"} : QAccessibleWidget::keyBindingsForAction(action);
    }
private:
    ElidedLabel* label() const { return static_cast<ElidedLabel*>(widget()); }
};
}
ElidedLabel::ElidedLabel(QWidget* parent) : QLabel(parent) {
    static const bool factory = [] {
        QAccessible::installFactory([](const QString&, QObject* object) -> QAccessibleInterface* {
            if (auto* label = qobject_cast<ElidedLabel*>(object)) return new AccessibleElidedLabel(label);
            return nullptr;
        });
        return true;
    }();
    Q_UNUSED(factory);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
}
QSize ElidedLabel::sizeHint() const { return QLabel::sizeHint() + QSize(revealable_ ? scaled(12) + 18 : 0, 0); }
QSize ElidedLabel::minimumSizeHint() const { return {std::min(QLabel::minimumSizeHint().width(), fontMetrics().averageCharWidth() * 8) + (revealable_ ? scaled(12) + 18 : 0), QLabel::minimumSizeHint().height()}; }
QRect ElidedLabel::textRect() const {
    if (!revealable_) return contentsRect();
    auto box = contentsRect().adjusted(6, 0, -scaled(12) - 12, 0);
    // Keep the summary and its disclosure together even in a stretching footer.
    if (!wordWrap()) box.setWidth(std::min(box.width(), int(std::ceil(QFontMetricsF(font()).horizontalAdvance(text())))));
    return box;
}
bool ElidedLabel::event(QEvent* event) {
    if (revealable_ && (event->type() == QEvent::HoverEnter || event->type() == QEvent::HoverLeave)) update();
    return QLabel::event(event);
}
void ElidedLabel::setText(const QString& text) {
    QLabel::setText(text);
    setToolTip(text);
    setAccessibleName(text);
}
void ElidedLabel::setRevealable(bool on) {
    revealable_ = on;
    // Headings stay out of the tab order; an actionable summary is a stop.
    setFocusPolicy(on ? Qt::StrongFocus : Qt::NoFocus);
    setCursor(on ? Qt::PointingHandCursor : Qt::ArrowCursor);
    setAttribute(Qt::WA_Hover, on);
    setAccessibleDescription(on ? QStringLiteral("Output details. Press Enter to show the full text.") : QString());
    setMinimumHeight(on ? 32 : 0);
    updateGeometry();
    update();
}
int ElidedLabel::heightForWidth(int width) const { return QLabel::heightForWidth(width - (revealable_ ? scaled(12) + 18 : 0)); }
bool ElidedLabel::elided() const { return QFontMetricsF(font()).horizontalAdvance(text()) > textRect().width(); }
void ElidedLabel::reveal() {
    if ((!revealable_ && revealTitle_.isEmpty()) || text().isEmpty()) return;
    if (popup_) popup_->close();
    auto* popup = new FullTextPopup(revealText_.isEmpty() ? text() : revealText_, revealTitle_, this);
    popup_ = popup;
    const auto screen = (window() && window()->screen() ? window()->screen() : QGuiApplication::primaryScreen())->availableGeometry();
    const int width = std::min(std::max(this->width(), scaled(320)), std::min(scaled(560), screen.width() - 16));
    popup->setFixedWidth(width);
    const int maxHeight = std::min(screen.height() - 16, window() ? window()->height() - 16 : screen.height() - 16);
    popup->label()->setMinimumHeight(popup->label()->heightForWidth(width - 36));
    popup->resize(width, std::min(maxHeight, popup->label()->minimumHeight() + (revealTitle_.isEmpty() ? 20 : scaled(48))));
    // Above a label in the lower half of its window (the status band), below
    // one higher up; the other side when the screen has no room.
    const QPoint above = mapToGlobal(QPoint(0, -popup->height() - 4)), below = mapToGlobal(QPoint(0, height() + 4));
    const bool lower = window() && mapTo(window(), QPoint(0, 0)).y() > window()->height() / 2;
    QPoint at = lower ? above : below;
    if (lower && at.y() < screen.top()) at = below;
    else if (!lower && at.y() + popup->height() > screen.bottom()) at = above;
    at.setX(std::clamp(at.x(), screen.left() + 8, std::max(screen.left() + 8, screen.right() - width - 8)));
    at.setY(std::max(at.y(), screen.top() + 8));
    popup->move(at);
    popup->show();
    popup->label()->setFocus(Qt::PopupFocusReason);
}
void ElidedLabel::keyPressEvent(QKeyEvent* event) {
    if (revealable_ && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Space)) { reveal(); return; }
    QLabel::keyPressEvent(event);
}
void ElidedLabel::mouseReleaseEvent(QMouseEvent* event) {
    if (revealable_ && event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) { reveal(); return; }
    QLabel::mouseReleaseEvent(event);
}
void ElidedLabel::focusInEvent(QFocusEvent* event) { QLabel::focusInEvent(event); update(); }
void ElidedLabel::focusOutEvent(QFocusEvent* event) { QLabel::focusOutEvent(event); update(); }
void ElidedLabel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (revealable_) {
        if (isEnabled() && underMouse()) rounded(&p, QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), t().surfaceRaised, {}, 4);
        const auto box = textRect();
        if (wordWrap()) {
            paintChevron(p, QPointF(box.right() + 7 + scaled(12) / 2.0, box.center().y()), 8, Qt::DownArrow, isEnabled() ? t().textSecondary : t().textDisabled);
        } else VisualStyle::icon("info").paint(&p, QRect(box.right() + 7, box.center().y() - scaled(12) / 2, scaled(12), scaled(12)), Qt::AlignCenter, isEnabled() ? QIcon::Normal : QIcon::Disabled);
    }
    // A revealable summary's keyboard focus: the amber ring around its text.
    if (revealable_ && keyboardFocus(this)) {
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(t().accent, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    }
    const QColor ink = property("headerInactive").toBool() ? t().textMuted : palette().color(isEnabled() ? QPalette::Normal : QPalette::Disabled, foregroundRole());
    if (wordWrap()) {
        p.setFont(font());
        p.setPen(ink);
        p.drawText(textRect(), alignment() | Qt::TextWordWrap, text());
    } else paintText(p, textRect(), text(), ink, font(), alignment(), elideMode_);
}

namespace {
class WarningLabel final : public QLabel {
public:
    WarningLabel(const QString& text, QWidget* parent) : QLabel(text, parent) { setWordWrap(true); }
protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::DynamicPropertyChange || event->type() == QEvent::FontChange) {
            const int gap = property("graphiteWarningGap").isValid() ? property("graphiteWarningGap").toInt() : 8;
            setContentsMargins(role(this) == "warning" ? scaled(Metric::icon) + gap : 0, 0, 0, 0);
        }
        return QLabel::event(event);
    }
    void paintEvent(QPaintEvent* event) override {
        QLabel::paintEvent(event);
        if (role(this) != "warning") return;
        QPainter p(this);
        const int side = scaled(Metric::icon);
        const auto glyph = IconArt::symbol("alert", t().bg, std::max(1, (side + 23) / 24), t().accent);
        p.drawImage(QRect(0, std::max(0, (fontMetrics().height() - side) / 2), side, side), glyph);
    }
};
}
QLabel* warningLabel(const QString& text, QWidget* parent) {
    auto* label = new WarningLabel(text, parent);
    VisualStyle::role(label, "warning");
    return label;
}
QString blurWarningText() {
    return QStringLiteral("Blur may be reversible. Use Redact for private information.");
}
QWidget* blurWarning(const std::function<void()>& useRedact, QWidget* parent) {
    auto* warning = new QWidget(parent);
    auto* layout = new QVBoxLayout(warning);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    auto* text = warningLabel(blurWarningText(), warning);
    text->setObjectName("blurWarningText");
    layout->addWidget(text);
    auto* action = toolButton("Use Redact", {}, "quiet", warning);
    action->setObjectName("useRedact");
    layout->addWidget(action, 0, Qt::AlignLeft);
    QObject::connect(action, &QToolButton::clicked, warning, useRedact);
    return warning;
}
void paintSelection(QPainter& painter, const QRectF& rect, bool keyboardFocused, qreal radius) {
    rounded(&painter, rect, t().selected, keyboardFocused ? t().accent : t().borderStrong, radius);
}

NavigationDelegate::NavigationDelegate(QObject* parent, int rowHeight) : QStyledItemDelegate(parent), rowHeight_(rowHeight) {}
QSize NavigationDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    const bool two = !index.data(SecondLineRole).toString().isEmpty();
    const QFontMetrics metrics(Graphite::font());
    const bool wrap = index.data(WrapLabelRole).toBool() && QFontMetrics(Graphite::font(Type::body, true)).horizontalAdvance(index.data().toString()) > option.rect.width() - scaled(Metric::icon) - 38;
    const int natural = wrap ? 2 * metrics.height() + 16 : two ? metrics.height() + QFontMetrics(Graphite::font(Type::metadata)).height() + 22 : metrics.height() + 16;
    return {option.rect.width(), std::max(scaled(two && rowHeight_ < Metric::destinationRow ? Metric::destinationRow : rowHeight_), natural)};
}
void NavigationDelegate::paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    const auto& c = t();
    const bool selected = option.state & QStyle::State_Selected, hover = option.state & QStyle::State_MouseOver;
    const bool enabled = option.state & QStyle::State_Enabled;
    const QRectF box = QRectF(option.rect).adjusted(2, 2, -2, -2);
    if (selected || hover) rounded(p, box, selected ? c.selected : mix(c.surface, c.surfaceRaised, 0.7), {}, 6);
    if (selected) p->fillRect(QRectF(box.left(), box.top() + 8, 2, box.height() - 16), c.accent);
    const auto* view = qobject_cast<const QAbstractItemView*>(option.widget);
    if (selected && view && keyboardFocus(view)) rounded(p, box, {}, c.accent, 6);
    const auto icon = index.data(Qt::DecorationRole).value<QIcon>();
    qreal x = box.left() + 12;
    if (!icon.isNull()) {
        const auto pixmap = icon.pixmap(QSize(scaled(Metric::icon), scaled(Metric::icon)), dpr(option.widget), !enabled ? QIcon::Disabled : selected ? QIcon::Selected : QIcon::Normal);
        p->drawPixmap(QPointF(x, box.center().y() - scaled(Metric::icon) / 2.0), pixmap);
        x += scaled(Metric::icon) + 10;
    }
    const auto first = index.data(Qt::DisplayRole).toString(), second = index.data(SecondLineRole).toString();
    const QColor ink = !enabled ? c.textDisabled : selected ? c.text : c.textSecondary;
    const QRectF text(x, box.top(), box.right() - 10 - x, box.height());
    if (second.isEmpty() && index.data(WrapLabelRole).toBool()) {
        const auto labelFont = Graphite::font(Type::body, selected);
        const QFontMetrics metrics(labelFont);
        if (metrics.horizontalAdvance(first) <= text.width()) paintText(*p, text, first, ink, labelFont);
        else {
            int split = first.lastIndexOf(' ');
            for (int i = first.indexOf(' '); i >= 0; i = first.indexOf(' ', i + 1)) {
                if (metrics.horizontalAdvance(first.left(i)) > text.width()) break;
                split = i;
            }
            const qreal top = text.center().y() - metrics.height();
            paintText(*p, QRectF(text.left(), top, text.width(), metrics.height()), first.left(split), ink, labelFont, Qt::AlignLeft | Qt::AlignVCenter, Qt::ElideNone);
            paintText(*p, QRectF(text.left(), top + metrics.height(), text.width(), metrics.height()), first.mid(split + 1), ink, labelFont, Qt::AlignLeft | Qt::AlignVCenter, Qt::ElideNone);
        }
    }
    else if (second.isEmpty()) paintText(*p, text, first, ink, Graphite::font(Type::body, selected));
    else {
        const qreal h = box.height();
        paintText(*p, QRectF(text.left(), box.top() + h * 0.18, text.width(), h * 0.36), first, ink, Graphite::font(Type::body, selected));
        paintText(*p, QRectF(text.left(), box.top() + h * 0.52, text.width(), h * 0.32), second, enabled ? c.textMuted : c.textDisabled, Graphite::font(Type::metadata));
    }
}

namespace {
class Confirmation final : public QMessageBox {
public:
    using QMessageBox::QMessageBox;
protected:
    void showEvent(QShowEvent* event) override {
        QMessageBox::showEvent(event);
        auto* grid = qobject_cast<QGridLayout*>(layout());
        auto* title = findChild<QLabel*>("qt_msgbox_label");
        auto* body = findChild<QLabel*>("qt_msgbox_informativelabel");
        auto* buttons = findChild<QDialogButtonBox*>();
        if (!grid || !title || !buttons) return;
        while (auto* item = grid->takeAt(0)) delete item;
        for (int row = 0; row < grid->rowCount(); ++row) { grid->setRowMinimumHeight(row, 0); grid->setRowStretch(row, 0); }
        grid->setSpacing(0);
        for (int column = 0; column < grid->columnCount(); ++column) { grid->setColumnMinimumWidth(column, 0); grid->setColumnStretch(column, 0); }
        title->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        grid->addWidget(title, 0, 0);
        if (body) {
            body->setContentsMargins(0, 0, 0, 0);
            body->setAlignment(Qt::AlignLeft | Qt::AlignTop);
            grid->setRowMinimumHeight(1, 12);
            grid->addWidget(body, 2, 0);
        }
        grid->setRowMinimumHeight(3, 16);
        grid->addWidget(buttons, 4, 0);
        grid->activate();
        adjustSize();
    }
};
}
QMessageBox* confirm(QWidget* parent, const QString& title, const QString& text, const QString& action, const QString& safe, bool destructive) {
    const int split = text.indexOf('\n');
    auto* box = new Confirmation(QMessageBox::NoIcon, title, split < 0 ? text : text.left(split), QMessageBox::Yes | QMessageBox::Cancel, parent);
    box->setObjectName(QStringLiteral("confirmAction"));
    box->setProperty("confirmationText", text);
    // QMessageBox constructed its button box before it had our component name.
    if (auto* buttons = box->findChild<QDialogButtonBox*>()) {
        QEvent change(QEvent::StyleChange);
        QCoreApplication::sendEvent(buttons, &change);
    }
    box->setTextFormat(Qt::PlainText);
    if (split >= 0) box->setInformativeText(text.mid(split + 1).trimmed());
    if (auto* question = box->findChild<QLabel*>("qt_msgbox_label")) VisualStyle::role(question, "strong");
    if (auto* consequence = box->findChild<QLabel*>("qt_msgbox_informativelabel")) VisualStyle::role(consequence, "secondary");
    if (auto* grid = qobject_cast<QGridLayout*>(box->layout())) grid->setVerticalSpacing(4);
    auto* yes = box->button(QMessageBox::Yes);
    yes->setObjectName("confirmDiscard");
    yes->setText(action);
    yes->setAccessibleName(action);
    yes->setProperty("visualRole", destructive ? "danger" : "primary");
    auto* keep = box->button(QMessageBox::Cancel);
    keep->setText(safe);
    keep->setAccessibleName(safe);
    box->setDefaultButton(QMessageBox::Cancel);
    box->setEscapeButton(QMessageBox::Cancel);
    box->setWindowModality(Qt::WindowModal);
    keep->setFocus();
    return box;
}

bool confirm(QWidget* parent, const QString& title, const QString& text, const QString& action, bool destructive) {
    std::unique_ptr<QMessageBox> box(confirm(parent, title, text, action, QStringLiteral("Cancel"), destructive));
    return box->exec() == QMessageBox::Yes;
}

void alignForms(QWidget* page, const QList<QFormLayout*>& sharedForms) {
    const auto forms = page->findChildren<QFormLayout*>();
    for (auto* form : forms) {
        form->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
        for (int row = 0; row < form->rowCount(); ++row) {
            if (auto* label = form->itemAt(row, QFormLayout::LabelRole); label && label->widget()) label->widget()->setFixedHeight(scaled(Metric::control));
            auto* field = form->itemAt(row, QFormLayout::FieldRole);
            if (!field) continue;
            QList<QWidget*> controls;
            if (field->widget()) controls << field->widget() << field->widget()->findChildren<QWidget*>();
            for (auto* control : controls) {
                const bool unitted = qobject_cast<QLineEdit*>(control) && control->parentWidget() && !qobject_cast<QComboBox*>(control->parentWidget())
                    && std::any_of(control->parentWidget()->children().cbegin(), control->parentWidget()->children().cend(), [](QObject* o) { return o->property("visualRole").toString() == "unitSuffix"; });
                if (!qobject_cast<QComboBox*>(control) && !qobject_cast<QAbstractSpinBox*>(control) && !unitted) continue;
                // A long choice is a preferred width, not the form's minimum.
                // Keep its natural size when there is room and let the style
                // elide the closed value when an enlarged-text form is narrow.
                if (qobject_cast<QComboBox*>(control) && !control->minimumWidth())
                    control->setMinimumWidth(std::min(control->minimumSizeHint().width(), scaled(160)));
                // A value with its unit is one compound control: cap and align
                // the pair, so the unit stays attached to its field.
                QWidget* target = control;
                if (auto* holder = control->parentWidget()) {
                    for (auto* sibling : holder->findChildren<QLabel*>(Qt::FindDirectChildrenOnly))
                        if (sibling->property("visualRole").toString() == "unitSuffix") { target = holder; break; }
                }
                target->setMaximumWidth(scaled(400));
                control = target;
                // A capped control starts at the field column, never centred in it.
                if (auto* parent = control->parentWidget()) {
                    QList<QLayout*> layouts = parent->findChildren<QLayout*>(Qt::FindDirectChildrenOnly);
                    if (parent->layout()) layouts.prepend(parent->layout());
                    for (int i = 0; i < layouts.size(); ++i) {
                        if (layouts[i]->setAlignment(control, Qt::AlignLeft | Qt::AlignVCenter)) break;
                        for (int j = 0; j < layouts[i]->count(); ++j) if (auto* nested = layouts[i]->itemAt(j)->layout()) layouts << nested;
                    }
                }
            }
        }
    }
    // Share within one form family, excluding hidden provider fields.
    // Measure fields after their control sizing, and watch reflow and visibility.
    class FormMode final : public QObject {
    public:
        FormMode(QWidget* page, const QList<QFormLayout*>& forms, const QList<QFormLayout*>& sharedForms)
            : QObject(page), page_(page), forms_(forms), sharedForms_(sharedForms.isEmpty() ? forms : sharedForms), shared_(!sharedForms.isEmpty()) {
            page->installEventFilter(this);
            if (page->parentWidget()) page->parentWidget()->installEventFilter(this);
            for (auto* form : forms_) {
                form->parentWidget()->installEventFilter(this);
                for (auto* child : form->parentWidget()->findChildren<QWidget*>()) child->installEventFilter(this);
                form->setRowWrapPolicy(QFormLayout::WrapAllRows);
            }
            schedule();
        }
    protected:
        bool eventFilter(QObject*, QEvent* event) override {
            if (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest || event->type() == QEvent::Show || event->type() == QEvent::Hide || event->type() == QEvent::FontChange)
                schedule();
            return false;
        }
    private:
        void schedule() {
            if (pending_) return;
            pending_ = true;
            QTimer::singleShot(0, this, [this] { apply(); pending_ = false; });
        }
        bool measured(QFormLayout* form, int row) const {
            QWidget* body = form->parentWidget();
            for (auto* widget = body; widget; widget = widget->parentWidget())
                if (auto* scroll = qobject_cast<QScrollArea*>(widget)) { body = scroll->widget(); break; }
            return form->isRowVisible(row) && form->parentWidget()->isVisibleTo(body);
        }
        void apply() {
            QString measurement;
            for (auto* form : sharedForms_)
                for (int row = 0; row < form->rowCount(); ++row)
                    if (auto* item = form->itemAt(row, QFormLayout::LabelRole); item && item->widget()) {
                        auto* label = item->widget();
                        measurement += QString::number(measured(form, row)) + label->font().key();
                        for (auto* text : label->findChildren<QLabel*>()) measurement += text->text();
                        for (auto* text : label->findChildren<QAbstractButton*>()) measurement += text->text();
                        if (auto* text = qobject_cast<QLabel*>(label)) measurement += text->text();
                    }
            if (measurement != measurement_) {
                measurement_ = measurement;
                int gutter = 0;
                for (auto* form : sharedForms_)
                    for (int row = 0; row < form->rowCount(); ++row)
                        if (auto* item = form->itemAt(row, QFormLayout::LabelRole); item && item->widget()) {
                            auto* label = item->widget();
                            label->setFixedHeight(scaled(Metric::control));
                            label->setMinimumWidth(0);
                            if (measured(form, row)) gutter = std::max(gutter, label->sizeHint().width());
                        }
                gutter_ = std::min(gutter, scaled(240));
                for (auto* form : sharedForms_)
                    for (int row = 0; row < form->rowCount(); ++row)
                        if (auto* item = form->itemAt(row, QFormLayout::LabelRole); item && item->widget()) item->widget()->setMinimumWidth(gutter_);
            }
            bool stack = false;
            const int pageWidth = page_->parentWidget() ? std::min(page_->width(), page_->parentWidget()->width()) : page_->width();
            for (auto* form : forms_) {
                if (!form->parentWidget()->isVisibleTo(page_)) continue;
                int fieldWidth = 0;
                bool labelled = false;
                for (int row = 0; row < form->rowCount(); ++row) {
                    if (!form->isRowVisible(row)) continue;
                    auto* label = form->itemAt(row, QFormLayout::LabelRole);
                    auto* field = form->itemAt(row, QFormLayout::FieldRole);
                    if (!label || !field) continue;
                    labelled = true;
                    fieldWidth = std::max(fieldWidth, field->minimumSize().width());
                }
                if (!labelled) continue;
                int margins = 0;
                for (auto* owner = static_cast<QObject*>(form); owner && !qobject_cast<QWidget*>(owner); owner = owner->parent())
                    if (auto* layout = qobject_cast<QLayout*>(owner)) margins += layout->contentsMargins().left() + layout->contentsMargins().right();
                for (auto* widget = form->parentWidget()->parentWidget(); widget; widget = widget->parentWidget()) {
                    if (auto* layout = widget->layout()) margins += layout->contentsMargins().left() + layout->contentsMargins().right();
                    if (widget == page_) break;
                }
                const int available = pageWidth - margins;
                // A family shares a useful minimum control width.
                stack |= available < gutter_ + std::max(0, form->horizontalSpacing()) + (shared_ ? std::max(240, std::min(320, fieldWidth)) : fieldWidth);
            }
            const auto policy = stack ? QFormLayout::WrapAllRows : QFormLayout::DontWrapRows;
            for (auto* form : forms_) if (form->rowWrapPolicy() != policy) form->setRowWrapPolicy(policy);
        }
        QWidget* page_;
        QList<QFormLayout*> forms_, sharedForms_;
        bool shared_;
        int gutter_ = 0;
        QString measurement_;
        bool pending_ = false;
    };
    new FormMode(page, forms, sharedForms);
}
namespace {
class ColumnSwitch final : public QObject {
public:
    ColumnSwitch(QBoxLayout* layout, QWidget* watched, int threshold) : QObject(watched), layout_(layout), threshold_(threshold) { watched->installEventFilter(this); apply(watched->width()); }
protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Resize) apply(static_cast<QWidget*>(watched)->width());
        return false;
    }
private:
    void apply(int width) {
        const auto direction = width >= scaled(threshold_) ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom;
        if (layout_->direction() != direction) layout_->setDirection(direction);
    }
    QBoxLayout* layout_;
    int threshold_;
};
}
void responsiveColumns(QBoxLayout* layout, QWidget* watched, int threshold) { new ColumnSwitch(layout, watched, threshold); }

void focusTooltips(QAbstractButton* button) {
    if (button->property("graphiteFocusTooltip").toBool()) return;
    button->setProperty("graphiteFocusTooltip", true);
    class FocusTooltip final : public QObject {
    public:
        explicit FocusTooltip(QAbstractButton* button) : QObject(button), button_(button) { button->installEventFilter(this); }
    protected:
        bool eventFilter(QObject*, QEvent* event) override {
            if (event->type() == QEvent::FocusIn) {
                const auto reason = static_cast<QFocusEvent*>(event)->reason();
                const auto* tool = qobject_cast<QToolButton*>(button_);
                const bool iconOnly = tool ? tool->toolButtonStyle() == Qt::ToolButtonIconOnly : button_->text().isEmpty();
                if (iconOnly && (reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason || reason == Qt::ShortcutFocusReason)) {
                    const auto text = button_->toolTip().isEmpty() ? button_->accessibleName() : button_->toolTip();
                    if (!text.isEmpty()) { shown_ = true; QToolTip::showText(button_->mapToGlobal(QPoint(button_->width() / 2, button_->height())), text, button_); }
                }
            } else if (shown_ && (event->type() == QEvent::FocusOut || event->type() == QEvent::Hide || event->type() == QEvent::MouseButtonPress)) {
                QToolTip::hideText(); shown_ = false;
            }
            return false;
        }
    private:
        QAbstractButton* button_;
        bool shown_ = false;
    };
    new FocusTooltip(button);
}

void showTextStartWhenUnfocused(QLineEdit* edit) {
    class TextStart final : public QObject {
    public:
        explicit TextStart(QLineEdit* edit) : QObject(edit), edit_(edit) {
            edit->installEventFilter(this);
            QObject::connect(edit, &QLineEdit::textChanged, this, [this] { apply(); });
            apply();
        }
    protected:
        bool eventFilter(QObject*, QEvent* event) override {
            if (event->type() == QEvent::FontChange || event->type() == QEvent::Show || event->type() == QEvent::Polish || event->type() == QEvent::Resize) apply();
            // A context menu is still part of editing: preserve its selection.
            if (event->type() == QEvent::FocusOut && static_cast<QFocusEvent*>(event)->reason() != Qt::PopupFocusReason) edit_->setCursorPosition(0);
            return false;
        }
    private:
        void apply() {
            if (!edit_->hasFocus()) edit_->setCursorPosition(0);
        }
        QLineEdit* edit_;
    };
    new TextStart(edit);
}

void transparentView(QAbstractScrollArea* view) {
    view->setAutoFillBackground(false);
    view->viewport()->setAutoFillBackground(false);
    view->setAttribute(Qt::WA_TranslucentBackground, false);
    view->setFrameShape(QFrame::NoFrame);
}

void disclosure(QToolButton* button) {
    button->setCheckable(true);
    button->setProperty("graphiteDisclosure", true);
    button->setProperty("visualRole", "quiet");
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setIconSize(QSize(14, 14));
    button->setFocusPolicy(Qt::StrongFocus);
    const auto sync = [button] {
        button->setIcon(chromeIcon(button->isChecked() ? QStringLiteral("chevron-down") : QStringLiteral("chevron-right")));
        button->setAccessibleDescription(button->isChecked() ? QStringLiteral("Expanded") : QStringLiteral("Collapsed"));
    };
    sync();
    QObject::connect(button, &QToolButton::toggled, button, sync);
}

Segmented::Segmented(QWidget* parent) : QTabBar(parent) {
    setDrawBase(false);
    setExpanding(false);
    setUsesScrollButtons(false);
    setElideMode(Qt::ElideNone);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover);
}
QSize Segmented::tabSizeHint(int index) const {
    const QFontMetrics metrics(Graphite::font(Type::body, true));
    return {std::max(110, metrics.horizontalAdvance(tabText(index)) + 44), std::max(scaled(40), metrics.height() + 20)};
}
QSize Segmented::sizeHint() const {
    int width = 0;
    for (int i = 0; i < count(); ++i) width += tabSizeHint(i).width();
    return {width, count() ? tabSizeHint(0).height() : scaled(40)};
}
QSize Segmented::minimumSizeHint() const { return sizeHint(); }
void Segmented::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const auto& c = t();
    QRectF track;
    for (int i = 0; i < count(); ++i) track |= QRectF(tabRect(i));
    rounded(&p, track.adjusted(0.5, 2.5, -0.5, -2.5), mix(c.bg, c.surface, 0.6), c.border, Metric::trackRadius);
    const int hovered = underMouse() ? tabAt(mapFromGlobal(QCursor::pos())) : -1;
    for (int i = 0; i < count(); ++i) {
        const QRectF r = QRectF(tabRect(i)).adjusted(3, 5, -3, -5);
        const bool on = i == currentIndex();
        if (on) rounded(&p, r, c.selected, mix(c.borderStrong, c.selected, 0.6), Metric::segmentRadius);
        if (on) p.fillRect(QRectF(r.center().x() - 12, r.bottom() - 3, 24, 2), c.accent);
        if (on && keyboardFocus(this)) rounded(&p, r, {}, c.accent, Metric::segmentRadius, 1.5);
        const QColor ink = !isTabEnabled(i) ? c.textDisabled : on ? c.text : i == hovered ? c.textSecondary : c.textMuted;
        paintText(p, r, tabText(i), ink, Graphite::font(Type::body, on), Qt::AlignCenter);
    }
}

SegmentTrack::SegmentTrack(QWidget* parent) : QWidget(parent) {
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(2, 2, 2, 2);
    row->setSpacing(2);
}
void SegmentTrack::addButton(QToolButton* button) {
    button->setParent(this);
    button->setCheckable(true);
    button->setAutoExclusive(true);
    button->setProperty("visualRole", "quiet");
    // The track adds its own 2 px, so the whole control is one field high and
    // each segment keeps a 32 px pointer target.
    button->setFixedHeight(std::max(Metric::iconTarget, scaled(Metric::control) - 4));
    static_cast<QHBoxLayout*>(layout())->addWidget(button);
}
void SegmentTrack::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const auto& c = t();
    rounded(&p, QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), mix(c.bg, c.surface, 0.6), c.border, Metric::trackRadius);
}

// ---------------------------------------------------------------- Window chrome
namespace {
class WindowEdge final : public QWidget {
public:
    WindowEdge(QWidget* owner, Qt::Edges edges) : QWidget(owner), owner_(owner), edges_(edges) {
        setAttribute(Qt::WA_NoSystemBackground);
        const bool horizontal = edges == Qt::LeftEdge || edges == Qt::RightEdge, vertical = edges == Qt::TopEdge || edges == Qt::BottomEdge;
        const bool falling = edges == (Qt::LeftEdge | Qt::TopEdge) || edges == (Qt::RightEdge | Qt::BottomEdge);
        setCursor(horizontal ? Qt::SizeHorCursor : vertical ? Qt::SizeVerCursor : falling ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor);
    }
    Qt::Edges edges() const { return edges_; }
protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && owner_->windowHandle()) owner_->windowHandle()->startSystemResize(edges_);
    }
    // The window's 1 px outline: frameless windows have no desktop border.
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setPen(t().borderStrong);
        if (edges_ == Qt::LeftEdge) p.drawLine(0, 0, 0, height());
        else if (edges_ == Qt::RightEdge) p.drawLine(width() - 1, 0, width() - 1, height());
        else if (edges_ == Qt::TopEdge) p.drawLine(0, 0, width(), 0);
        else if (edges_ == Qt::BottomEdge) p.drawLine(0, height() - 1, width(), height() - 1);
        else {
            if (edges_ & Qt::LeftEdge) p.drawLine(0, 0, 0, height());
            if (edges_ & Qt::RightEdge) p.drawLine(width() - 1, 0, width() - 1, height());
            if (edges_ & Qt::TopEdge) p.drawLine(0, 0, width(), 0);
            if (edges_ & Qt::BottomEdge) p.drawLine(0, height() - 1, width(), height() - 1);
        }
    }
private:
    QWidget* owner_;
    Qt::Edges edges_;
};
class WindowEdges final : public QObject {
public:
    explicit WindowEdges(QWidget* owner) : QObject(owner), owner_(owner) {
        for (Qt::Edges e : {Qt::Edges(Qt::LeftEdge), Qt::Edges(Qt::RightEdge), Qt::Edges(Qt::TopEdge), Qt::Edges(Qt::BottomEdge), Qt::LeftEdge | Qt::TopEdge,
                            Qt::RightEdge | Qt::TopEdge, Qt::LeftEdge | Qt::BottomEdge, Qt::RightEdge | Qt::BottomEdge})
            edges_.append(new WindowEdge(owner, e));
        owner->installEventFilter(this);
        place();
    }
protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::Resize || event->type() == QEvent::Show || event->type() == QEvent::WindowStateChange || event->type() == QEvent::ChildAdded) place();
        return false;
    }
private:
    void place() {
        const int w = owner_->width(), h = owner_->height(), e = 4, k = 10;
        const QList<QRect> boxes{{0, k, e, h - 2 * k}, {w - e, k, e, h - 2 * k}, {k, 0, w - 2 * k, e}, {k, h - e, w - 2 * k, e},
                                 {0, 0, k, k}, {w - k, 0, k, k}, {0, h - k, k, k}, {w - k, h - k, k, k}};
        const bool free = !(owner_->windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen));
        for (int i = 0; i < edges_.size(); ++i) {
            edges_[i]->setGeometry(boxes[i]);
            edges_[i]->setVisible(free);
            edges_[i]->raise();
        }
    }
    QWidget* owner_;
    QList<WindowEdge*> edges_;
};
QToolButton* chromeButton(const QString& glyph, const QString& name, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setIcon(chromeIcon(glyph));
    button->setIconSize(QSize(20, 20));
    button->setFixedSize(Metric::iconTarget, Metric::iconTarget);
    button->setProperty("visualRole", "quiet");
    button->setProperty("noChevron", true);
    button->setAccessibleName(name);
    button->setToolTip(name);
    button->setFocusPolicy(Qt::TabFocus);
    return button;
}
}

namespace {
int& customFrameOverride() { static int forced = 0; return forced; }
}
void WindowHeader::chromeAfter(QWidget* last) {
    for (auto* button : {minimize_, maximize_, close_}) if (button) { QWidget::setTabOrder(last, button); last = button; }
    if (menu_) QWidget::setTabOrder(last, menu_);
}
void WindowHeader::setCustomFrameOverride(int forced) { customFrameOverride() = forced; }
bool WindowHeader::customFrame() {
    if (customFrameOverride()) return customFrameOverride() > 0;
    if (qEnvironmentVariable("PUTGD_SYSTEM_DECORATIONS") == QLatin1String("1")) return false;
    const auto platform = QGuiApplication::platformName();
    return platform == QLatin1String("xcb") || platform.startsWith(QLatin1String("wayland"));
}

WindowHeader::WindowHeader(QWidget* window, QMenu* menu, Density density) : QWidget(window), density_(density), window_(window) {
    setObjectName(QStringLiteral("windowHeader"));
    setProperty("headerDensity", density == Density::Compact ? QStringLiteral("compact") : QStringLiteral("standard"));
    row_ = new QHBoxLayout(this);
    row_->setContentsMargins(density == Density::Compact ? 10 : 12, 0, density == Density::Compact ? 4 : 12, 0);
    row_->setSpacing(6);
    leading_ = new QWidget(this);
    auto* leading = new QHBoxLayout(leading_);
    leading->setContentsMargins(0, 0, 0, 0);
    leading->setSpacing(10);
    menu_ = chromeButton(QStringLiteral("menu"), QStringLiteral("Application menu"), leading_);
    menu_->setObjectName(QStringLiteral("applicationMenu"));
    menu_->setFocusPolicy(Qt::StrongFocus);
    menu_->setPopupMode(QToolButton::InstantPopup);
    if (menu) menu_->setMenu(menu);
    menu_->setVisible(menu);
    leading->addWidget(menu_);
    name_ = new QLabel(QStringLiteral("put.gd"), leading_);
    name_->setObjectName(QStringLiteral("brandName"));
    // The compact band sets the brand at the title's size, so one text line fits it.
    VisualStyle::role(name_, density == Density::Compact ? "strong" : "sectionTitle");
    leading->addWidget(name_);
    leading->addStretch();
    row_->addWidget(leading_);
    row_->addStretch(1);
    row_->addStretch(1);
    trailing_ = new QWidget(this);
    trailingRow_ = new QHBoxLayout(trailing_);
    trailingRow_->setContentsMargins(0, 0, 0, 0);
    trailingRow_->setSpacing(4);
    trailingRow_->addStretch();
    if (customFrame()) {
        minimize_ = chromeButton(QStringLiteral("minimize"), QStringLiteral("Minimize"), trailing_);
        maximize_ = chromeButton(QStringLiteral("maximize"), QStringLiteral("Maximize"), trailing_);
        close_ = chromeButton(QStringLiteral("close"), QStringLiteral("Close window"), trailing_);
        minimize_->setObjectName(QStringLiteral("windowMinimize"));
        maximize_->setObjectName(QStringLiteral("windowMaximize"));
        close_->setObjectName(QStringLiteral("windowClose"));
        trailingRow_->addSpacing(8);
        for (auto* button : {minimize_, maximize_, close_}) trailingRow_->addWidget(button);
        connect(minimize_, &QToolButton::clicked, this, [this] { if (window_) window_->showMinimized(); });
        connect(maximize_, &QToolButton::clicked, this, [this] {
            if (!window_) return;
            if (window_->isMaximized()) window_->showNormal();
            else window_->showMaximized();
        });
        connect(close_, &QToolButton::clicked, this, [this] { if (window_) window_->close(); });
    }
    row_->addWidget(trailing_);
    window->installEventFilter(this);
    connect(&Theme::Notifier::instance(), &Theme::Notifier::changed, this, [this] {
        auto palette = name_->palette();
        palette.setColor(QPalette::WindowText, property("headerInactive").toBool() ? t().textMuted : t().text);
        name_->setPalette(palette);
    });
    fitHeight();
    // Without put.gd's frame the desktop's title bar names the window; a compact
    // header would only repeat it and take the content's height.
    if (density == Density::Compact && !customFrame()) hide();
    syncWindowState();
}
int WindowHeader::compactHeight() const {
    const QFontMetricsF title(Graphite::font(Type::body, true));
    // Compact review chrome keeps the full text line and 32 px targets.
    const int text = int(std::ceil(title.height())) + 4;
    return std::max(Metric::iconTarget, text);
}
void WindowHeader::fitHeight() { setFixedHeight(density_ == Density::Compact ? compactHeight() : scaled(Metric::header)); }
void WindowHeader::resizeEvent(QResizeEvent* event) { QWidget::resizeEvent(event); syncWindowState(); }
void WindowHeader::changeEvent(QEvent* event) {
    // A later application font (enlarged text) changes the line height.
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) fitHeight();
    QWidget::changeEvent(event);
}
void WindowHeader::setCentre(QWidget* centre) {
    if (centre_) { row_->removeWidget(centre_); centre_->hide(); }
    centre_ = centre;
    centre->setParent(this);
    row_->insertWidget(2, centre, 0, Qt::AlignCenter);
    centre->show();
    syncWindowState();
}
void WindowHeader::setDocument(const QString& name, const QString& state) {
    if (!docName_) {
        auto* block = new QWidget(this);
        block->setObjectName(QStringLiteral("documentTitle"));
        auto* lines = new QVBoxLayout(block);
        lines->setContentsMargins(0, 0, 0, 0);
        lines->setSpacing(1);
        docName_ = new ElidedLabel(block);
        docName_->setObjectName(QStringLiteral("documentName"));
        docName_->setAlignment(Qt::AlignCenter);
        docName_->setTextFormat(Qt::PlainText);
        VisualStyle::role(docName_, "strong");
        docState_ = new QLabel(block);
        docState_->setObjectName(QStringLiteral("documentState"));
        docState_->setAlignment(Qt::AlignCenter);
        docState_->setTextFormat(Qt::PlainText);
        // Metadata (12 px), not micro: the state is read, not decoration.
        VisualStyle::role(docState_, "meta");
        lines->addWidget(docName_);
        lines->addWidget(docState_);
        setCentre(block);
    }
    docName_->setText(name);
    docName_->setToolTip(name);
    docState_->setText(state);
    // One line in the compact band: its state belongs to the window's footer.
    docState_->setVisible(!state.isEmpty() && density_ != Density::Compact);
    syncWindowState();
}
void WindowHeader::addTrailing(QWidget* widget) {
    trailingRow_->insertWidget(1, widget);
    syncWindowState();
}
void WindowHeader::syncWindowState() {
    if (maximize_ && window_) {
        const bool maximized = window_->isMaximized();
        maximize_->setIcon(chromeIcon(maximized ? QStringLiteral("restore") : QStringLiteral("maximize")));
        maximize_->setAccessibleName(maximized ? QStringLiteral("Restore") : QStringLiteral("Maximize"));
        maximize_->setToolTip(maximize_->accessibleName());
    }
    leading_->setMinimumWidth(0);
    trailing_->setMinimumWidth(0);
    const int side = std::max(leading_->sizeHint().width(), trailing_->sizeHint().width());
    const auto margins = row_->contentsMargins();
    const int titleWidth = docName_ ? docName_->fontMetrics().horizontalAdvance(docName_->text()) : centre_ ? centre_->sizeHint().width() : 0;
    const bool balanced = width() >= margins.left() + margins.right() + 2 * side + titleWidth + 4 * row_->spacing();
    if (balanced) { leading_->setMinimumWidth(side); trailing_->setMinimumWidth(side); }
    if (docName_) { docName_->setAlignment(balanced ? Qt::AlignCenter : Qt::AlignLeft | Qt::AlignVCenter); docState_->setAlignment(docName_->alignment()); }
    row_->setStretch(1, balanced ? 1 : 0);
    row_->setStretch(3, 1);
    setProperty("balancedTitle", balanced);
}
bool WindowHeader::eventFilter(QObject* watched, QEvent* event) {
    if (watched == window_ && (event->type() == QEvent::WindowStateChange || event->type() == QEvent::Show || event->type() == QEvent::Resize)) syncWindowState();
    if (watched == window_ && (event->type() == QEvent::WindowActivate || event->type() == QEvent::WindowDeactivate)) {
        const bool inactive = event->type() == QEvent::WindowDeactivate;
        setProperty("headerInactive", inactive);
        for (auto* child : findChildren<QWidget*>()) { child->setProperty("headerInactive", inactive); child->update(); }
        auto palette = name_->palette(); palette.setColor(QPalette::WindowText, inactive ? t().textMuted : t().text); name_->setPalette(palette);
        update();
    }
    if (watched == window_ && event->type() == QEvent::WindowTitleChange && !docName_) name_->setToolTip(window_->windowTitle());
    return QWidget::eventFilter(watched, event);
}
void WindowHeader::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && customFrame() && window_ && window_->windowHandle()) {
        window_->windowHandle()->startSystemMove();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}
void WindowHeader::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && customFrame() && window_) {
        if (window_->isMaximized()) window_->showNormal();
        else window_->showMaximized();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}
void WindowHeader::contextMenuEvent(QContextMenuEvent* event) {
    if (!customFrame() || !window_) { QWidget::contextMenuEvent(event); return; }
    auto* menu = new QMenu(this);
    menu->setObjectName("windowContextMenu");
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addAction("Minimize", window_, &QWidget::showMinimized);
    menu->addAction(window_->isMaximized() ? "Restore" : "Maximize", this, [this] {
        if (window_->isMaximized()) window_->showNormal();
        else window_->showMaximized();
    });
    menu->addSeparator();
    menu->addAction("Close", window_, &QWidget::close);
    menu->popup(event->globalPos());
    event->accept();
}
void WindowHeader::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), t().bg);
    p.setPen(t().border);
    p.drawLine(0, height() - 1, width(), height() - 1);
}
WindowHeader* WindowHeader::install(QWidget* window, QBoxLayout* rootLayout, QMenu* menu, Density density) {
    if (customFrame()) {
        window->setWindowFlag(Qt::FramelessWindowHint, true);
        new WindowEdges(window);
    }
    auto* header = new WindowHeader(window, menu, density);
    if (auto* main = qobject_cast<QMainWindow*>(window)) main->setMenuWidget(header);
    else if (rootLayout) rootLayout->insertWidget(0, header);
    return header;
}
}
