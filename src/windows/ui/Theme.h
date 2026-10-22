#pragma once
// The UI helper's visual system: a light and a dark palette in the spirit of the
// Linux redesign (docs/design.md: neutral surfaces, one blue accent, red for
// destructive actions) laid out like a current Windows 11 app. Standard controls
// stay standard; this file gives them the window's colours (WM_CTLCOLOR*), the
// system dark control themes, and custom-drawn faces for push buttons, check
// boxes, radio buttons, navigation items and list headers. It also rasterizes the
// app's two-tone glyphs (src/ui/IconArt.cpp) with anti-aliasing for buttons.
#include <initializer_list>
#include <string>
#include <windows.h>
#include <commctrl.h>

namespace putgd::ui::theme {
struct Palette {
    COLORREF rail, content, card, cardBorder, input, placeholder, notice, noticeBorder;
    COLORREF hover, pressed, selected, divider;
    COLORREF button, buttonHover, buttonPressed, buttonBorder, buttonBottom, inputBorder, inputBottom;
    COLORREF text, textSecondary, textMuted, textDisabled;
    COLORREF accent, accentHover, accentPressed, onAccent, link;
    COLORREF danger, warning, success;
    COLORREF icon, iconSecondary;
};
bool dark();
const Palette& colors();
// Switches the scheme for the process (menus and tooltips follow); true when it changed.
bool setDark(bool on);
// themeSetting: system | dark | light (preferences.appearance.theme).
bool wantsDark(const std::string& themeSetting);

COLORREF blend(COLORREF from, COLORREF to, int alpha255);
HBRUSH brush(COLORREF); // cached; never delete

// Window surfaces. A window's surface paints its background; a control takes its
// own surface when set, else its parent's.
enum class Surface { Content = 1, Rail, Card, Placeholder, Notice };
COLORREF surfaceColor(Surface);
void setSurface(HWND, Surface);
COLORREF backgroundOf(HWND);

// Applies the current scheme to the window and every descendant (dark control
// themes, list colours, row heights) and repaints it.
void applyTo(HWND root);

// ---- painting -------------------------------------------------------------------------
// Anti-aliased rounded rectangle (fill and optional 1 px border). dc should be a memory DC.
void roundRect(HDC, const RECT&, int radius, COLORREF fill, COLORREF border = CLR_INVALID);
void fill(HDC, const RECT&, COLORREF);
void drawText(HDC, const std::wstring&, RECT, COLORREF, HFONT, UINT format);
// IconArt-style two-tone glyph. Names: brand history destinations settings region edit
// window desktop clipboard info power warning images check.
HICON glyph(const std::string& name, int size, COLORREF main, COLORREF secondary);
void drawGlyph(HDC, const std::string& name, int x, int y, int size, COLORREF main, COLORREF secondary);

// ---- controls ---------------------------------------------------------------------------
enum class Role { Standard = 0, Primary, Danger, Nav, Action, Subtle, Link };
void setRole(HWND button, Role, const char* glyphName = nullptr);
void setSelected(HWND button, bool selected);
// Combo boxes (drop-down lists) get a button-like face, vertically centred text and a chevron.
void styleCombo(HWND combo);
// Edits drawn inside a rounded frame painted by their parent (Panel). place() sizes them.
void setFramed(HWND edit, bool framed);
bool framed(HWND);
// A control drawn on a rounded card painted by its parent (list views, list boxes).
void setCard(HWND, bool card);
bool card(HWND);
// The outer rectangle a framed edit or card control occupies, in its parent's coordinates.
RECT outerRect(HWND);
// Positions a framed edit or card inside the outer rectangle (x, y, width, height).
void placeInside(HWND, int x, int y, int width, int height);

// NM_CUSTOMDRAW for push buttons, check boxes, radio buttons and list headers; true when handled.
bool customDraw(NMHDR*, LRESULT& result);
// WM_CTLCOLOR*: colours for a child of `parent`. textRole: 0 normal, 1 muted, 2 error, 3 secondary heading.
LRESULT controlColor(UINT message, HDC, HWND control, int textRole);
// Paints what a panel derives from its children: framed-edit frames, cards and the
// divider above each section heading. scrollOffset: the panel's scroll position.
void paintDecor(HWND parent, HDC, int scrollOffset);
// Owner-drawn list box items (Destinations): selection, hover-free, two lines.
void measureListItem(HWND list, MEASUREITEMSTRUCT*);
void drawListItem(DRAWITEMSTRUCT*);

// A horizontal selector ("pivot") that reads as navigation: Capture | General | ...
// Speaks LB_SETCURSEL / LB_GETCURSEL and notifies its parent with WM_COMMAND LBN_SELCHANGE.
HWND pivot(HWND parent, int id, std::initializer_list<const wchar_t*> items);
int pivotWidth(HWND);
// Draws the pivot as a segmented control (a toggle between two or three views).
void setSegmented(HWND pivot);
// A list view column drawn as a coloured state dot and its word (tone from the word).
void setStateColumn(HWND list, int column);
}
