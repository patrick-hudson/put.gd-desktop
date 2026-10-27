#pragma once
#include <QUrl>
#include <QJsonObject>
#include <functional>
#include "Theme.h"
#include <QFont>
#include <QIcon>
#include <QLabel>
#include <QPointer>
#include <QProxyStyle>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QWidget>

class QPushButton;class QApplication;class QLineEdit;class QMessageBox;
class QAbstractButton;class QAbstractScrollArea;class QFontMetricsF;class QBoxLayout;class QFrame;class QHBoxLayout;class QLabel;class QMenu;class QToolButton;

// The Graphite visual language (2026-10-26): one painted control set for every
// app-owned surface. Paint owns appearance; the Qt widget underneath keeps
// input, focus, text editing, accessibility and OS integration. Colours are
// the Theme tokens, read when painting, so a scheme switch is a repaint.
namespace putgd::Graphite {
// Geometry in logical pixels at the default text scale.
namespace Metric {
constexpr int space = 4, control = 36, iconTarget = 32, radius = 7, segmentRadius = 6, trackRadius = 8, swatchRadius = 5;
constexpr int header = 56, commandBand = 60, statusBand = 26, icon = 18, splitZone = 26;
// A split button's menu half: a full 32 px pointer target with the same small
// chevron (combo boxes and menu buttons keep splitZone for their chevron).
constexpr int splitArrow = 32;
constexpr int navRow = 38, destinationRow = 68, historyRow = 76, thumbWidth = 80, thumbHeight = 50;
constexpr int historySidebar = 184, settingsSidebar = 210, destinationSidebar = 236, inspector = 300;
// The widest a page sidebar grows for long labels or enlarged text.
constexpr int sidebarMax = 260;
}
// Type sizes in pixels; weights are 400 and 600 only. Technical values (paths,
// URLs, prefixes, bucket names, identifiers) use Plex Mono at body size: the
// "technical" type role.
namespace Type {
constexpr int body = 14, metadata = 12, micro = 11, section = 17, page = 23, technical = 14;
}
// Families registered by Fonts::load (IBM Plex) in processes that bundle them;
// the system families elsewhere.
QString uiFamily();
QString monoFamily();
void setFamilies(const QString& ui, const QString& mono);
QFont font(int pixels = Type::body, bool strong = false);
QFont monoFont(int pixels = Type::metadata);
// Orthogonal to colour roles; restored on every polish.
void setMono(QWidget*, int pixels = Type::metadata);
QColor mix(const QColor& a, const QColor& b, qreal amount);
// The hairline between a raised control and its surface: quieter than border.
// Buttons use it; their labels identify them.
QColor controlStroke();
// The 3:1 boundary of an essential input (Theme's controlOutline): text
// fields, combo boxes, spin boxes, check boxes and radio buttons.
QColor controlOutline();
// True while the widget has focus that came from the keyboard.
bool keyboardFocus(const QWidget*);
// A locked value: a disabled control that still states something, such as a
// setting inherited from a default or one held while setup runs. It reads as
// information, not as an unavailable action: textMuted (4.5:1) in a quiet
// read-only frame, without a combo box's chevron or a spin box's steps. The
// mark applies only while the control is disabled; the caller still decides
// that. Unavailable actions keep textDisabled.
void setLocked(QWidget*, bool locked);
bool isLocked(const QWidget*);

// Installs the Graphite style, palette and font on the application and keeps
// them in step with Theme scheme switches. Safe to call more than once.
void install(QApplication&);
QPalette palette();

// Glyphs drawn from paths, for chrome controls IconArt has no symbol for:
// "menu", "more", "minimize", "maximize", "restore", "close", "chevron-down",
// "chevron-right", "chevron-left", "check". Follows scheme switches like VisualStyle::icon.
QIcon chromeIcon(const QString& name);
void paintChromeGlyph(QPainter&, const QRectF&, const QString& name, const QColor&);
void paintChevron(QPainter&, const QPointF& centre, qreal size, Qt::ArrowType, const QColor&, qreal width = 1.5);
// The baseline that puts the ink of text (its tight bounds) on centre.
qreal opticalBaseline(const QFontMetricsF&, const QString&, qreal centre);
// The desktop text size against the design's 14 px body (1 to 2); type and
// control heights scale with it. See Graphite.cpp.
qreal textScale();
int scaled(int pixels);
// A page sidebar's width: what its labels, icons and padding need (natural),
// never under its baseline width and never over Metric::sidebarMax. Rows grow
// taller with the text; the sidebar does not take the window with them.
int sidebarWidth(int baseline, int natural);
// Text vertically centred on its ink, elided to the rectangle.
void paintText(QPainter&, const QRectF&, const QString&, const QColor&, const QFont&, Qt::Alignment = Qt::AlignLeft | Qt::AlignVCenter, Qt::TextElideMode = Qt::ElideRight);

class Style final : public QProxyStyle {
    Q_OBJECT
public:
    Style();
    void polish(QWidget*) override;
    void unpolish(QWidget*) override;
    void polish(QPalette&) override;
    int pixelMetric(PixelMetric, const QStyleOption* = nullptr, const QWidget* = nullptr) const override;
    int styleHint(StyleHint, const QStyleOption* = nullptr, const QWidget* = nullptr, QStyleHintReturn* = nullptr) const override;
    QSize sizeFromContents(ContentsType, const QStyleOption*, const QSize&, const QWidget* = nullptr) const override;
    QRect subElementRect(SubElement, const QStyleOption*, const QWidget* = nullptr) const override;
    QRect subControlRect(ComplexControl, const QStyleOptionComplex*, SubControl, const QWidget* = nullptr) const override;
    SubControl hitTestComplexControl(ComplexControl, const QStyleOptionComplex*, const QPoint&, const QWidget* = nullptr) const override;
    void drawPrimitive(PrimitiveElement, const QStyleOption*, QPainter*, const QWidget* = nullptr) const override;
    void drawControl(ControlElement, const QStyleOption*, QPainter*, const QWidget* = nullptr) const override;
    void drawComplexControl(ComplexControl, const QStyleOptionComplex*, QPainter*, const QWidget* = nullptr) const override;
    QPixmap generatedIconPixmap(QIcon::Mode, const QPixmap&, const QStyleOption*) const override;
private:
    void drawButtonLabel(const QStyleOption*, const QString& text, const QIcon&, QSize iconSize, QPainter*, const QWidget*, QRect body) const;
};

// A region of the window: its fill token and the hairlines on its edges.
// role: "bg" (default), "surface" or "raised".
class Surface : public QWidget {
    Q_OBJECT
public:
    explicit Surface(const char* role = "bg", Qt::Edges hairlines = {}, QWidget* parent = nullptr);
    void setHairlines(Qt::Edges edges) { edges_ = edges; update(); }
protected:
    void paintEvent(QPaintEvent*) override;
private:
    QByteArray role_;
    Qt::Edges edges_;
};
// One hairline, horizontal unless asked otherwise.
QFrame* rule(QWidget* parent = nullptr, Qt::Orientation = Qt::Horizontal);
// A label in a type role: "page", "section", "body", "strong", "meta",
// "micro", "muted" and the status roles "success", "warning", "danger".
QLabel* label(const QString& text, const char* role = "body", QWidget* parent = nullptr);

// Rows of a page's sidebar: icon, label and an optional second line (role
// SecondLineRole), with the shared hover, selected fill and focus ring.
class NavigationDelegate final : public QStyledItemDelegate {
    Q_OBJECT
public:
    static constexpr int SecondLineRole = Qt::UserRole + 40;
    explicit NavigationDelegate(QObject* parent, int rowHeight = Metric::navRow);
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override;
    void paint(QPainter*, const QStyleOptionViewItem&, const QModelIndex&) const override;
private:
    int rowHeight_;
};

// One line of text that elides in the middle instead of widening its window;
// the full text stays in its tooltip and accessible name. A revealable label
// (an actionable summary, not a heading) is also a tab stop: Enter, Space or a
// click opens its full plain text in a small popup a keyboard can read, select
// and close with Escape.
class ElidedLabel final : public QLabel {
    Q_OBJECT
public:
    explicit ElidedLabel(QWidget* parent = nullptr);
    void setText(const QString&);
    void setRevealable(bool);
    void setRevealText(const QString& text) { revealText_ = text; }
    bool revealable() const { return revealable_; }
    bool elided() const;
    // Opens the full text popup (revealable labels); the popup while it is open.
    void reveal();
    QWidget* fullTextPopup() const { return popup_; }
    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;
protected:
    bool event(QEvent*) override;
    void paintEvent(QPaintEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void focusInEvent(QFocusEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
private:
    QRect textRect() const;
    bool revealable_ = false;
    QString revealText_;
    QPointer<QWidget> popup_;
};

// Browser failures stay beside the originating action, with the exact link to copy.
class BrowserFeedback final : public QWidget {
public:
    explicit BrowserFeedback(QWidget* parent = nullptr);
    std::function<void(const QJsonObject&)> opening(const QUrl&);
    void clear();
protected:
    void showEvent(QShowEvent*) override;
private:
    QLabel* message_;
    QPushButton* copy_;
    QUrl url_;
    quint64 request_ = 0;
};

// The segmented control: one track, a raised selected segment, labels centred
// on their segment. A QTabBar underneath keeps keyboard and accessible tabs.
class Segmented final : public QTabBar {
    Q_OBJECT
public:
    explicit Segmented(QWidget* parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
protected:
    QSize tabSizeHint(int) const override;
    QSize minimumTabSizeHint(int index) const override { return tabSizeHint(index); }
    void paintEvent(QPaintEvent*) override;
};

// The segmented track for a few checkable quiet buttons (List / Gallery):
// the same track as Segmented behind them; each button stays a real button.
class SegmentTrack final : public QWidget {
    Q_OBJECT
public:
    explicit SegmentTrack(QWidget* parent = nullptr);
    void addButton(QToolButton*);
protected:
    void paintEvent(QPaintEvent*) override;
};

// The window header shared by the main window and document windows: the
// application menu and name at the left, a centred slot (navigation or a
// document title), and window controls at the right when put.gd draws its own
// frame. The window manager still moves, resizes, maximizes and closes the
// window (QWindow system move/resize); with PUTGD_SYSTEM_DECORATIONS=1 the
// desktop's own frame is used instead and the window controls are hidden.
class WindowHeader final : public QWidget {
    Q_OBJECT
public:
    // Standard: the 56 px band of the main window and the editor. Compact: one
    // line for windows whose content needs the height (Review); its height
    // follows the title's text metrics with 32 px window controls, and it is
    // hidden when the desktop draws the frame (whose title bar names the window).
    enum class Density { Standard, Compact };
    explicit WindowHeader(QWidget* window, QMenu* menu = nullptr, Density density = Density::Standard);
    void setCentre(QWidget*);
    // A document's name and its truthful upload state ("Not uploaded"). The
    // compact header shows the name only; keep the state elsewhere.
    void setDocument(const QString& name, const QString& state = {});
    void addTrailing(QWidget*);
    QToolButton* menuButton() const { return menu_; }
    Density density() const { return density_; }
    void chromeAfter(QWidget* last);
    // The compact band's height for the current fonts: the title's line height
    // plus padding, never less than a 32 px control target plus padding.
    int compactHeight() const;
    // Whether put.gd draws this process's window frames.
    static bool customFrame();
    // Tests and the preview force the custom frame on or off on platforms where
    // it is otherwise decided by the platform name (offscreen). 0: no override.
    static void setCustomFrameOverride(int forced);
    // Makes a top-level window frameless (when customFrame()) with resize
    // edges, and puts this header above its content.
    static WindowHeader* install(QWidget* window, QBoxLayout* rootLayout, QMenu* menu = nullptr, Density density = Density::Standard);
protected:
    void mousePressEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void paintEvent(QPaintEvent*) override;
    void changeEvent(QEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;
private:
    void syncWindowState();
    void fitHeight();
    Density density_ = Density::Standard;
    QPointer<QWidget> window_;
    QToolButton* menu_ = nullptr;
    QLabel* name_ = nullptr;
    QHBoxLayout* row_ = nullptr;
    QWidget* centre_ = nullptr;
    QWidget* leading_ = nullptr;
    QWidget* trailing_ = nullptr;
    QHBoxLayout* trailingRow_ = nullptr;
    ElidedLabel* docName_ = nullptr;
    QLabel* docState_ = nullptr;
    QToolButton *minimize_ = nullptr, *maximize_ = nullptr, *close_ = nullptr;
};
// An item view drawn on the surface behind it (no Base fill of its own).
void transparentView(QAbstractScrollArea*);
// A confirmation that names its action: the title, the consequence and its
// concrete target in text, Cancel (initially focused, Escape) and the named
// action. The action is QMessageBox::Yes underneath and window titles stay as
// they were, so agents and tests that find a dialog and press Yes keep working. True when the action was chosen.
bool confirm(QWidget* parent, const QString& title, const QString& text, const QString& action, bool destructive = false);
// The same component for asynchronous session control; caller opens and owns it.
QMessageBox* confirm(QWidget* parent, const QString& title, const QString& text, const QString& action, const QString& safe, bool destructive);
// One label gutter for every form on a page (the widest label, at most
// 240 px), and choice controls at a natural width (at most 400 px); text and
// path fields keep growing. Call once a page's forms are built.
void alignForms(QWidget* page);
// Side by side while the watched widget is at least threshold px wide,
// stacked below that: never two cramped columns.
void responsiveColumns(QBoxLayout*, QWidget* watched, int threshold);
// Show a title from its beginning when it is not being edited. Keep the normal
// shrinkable line-edit geometry so long names cannot displace adjacent actions.
void showTextStartWhenUnfocused(QLineEdit*);
// Icon-only controls also name themselves when reached by keyboard.
void focusTooltips(QAbstractButton*);
// A disclosure button: a quiet, checkable button with one chevron (right when
// collapsed, down when expanded) whose accessible description says which.
void disclosure(QToolButton*);
// Builds a button in a role (see Style): "primary", "secondary" (default),
// "quiet", "danger", "link". glyph is an IconArt name, or empty.
QToolButton* toolButton(const QString& text, const QString& glyph = {}, const char* role = nullptr, QWidget* parent = nullptr);
}
