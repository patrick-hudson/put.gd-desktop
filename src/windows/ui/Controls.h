#pragma once
// Small Win32 helpers for the on-demand UI helper (--ui): common controls v6
// created with the system message font at the window's DPI, a scrollable panel
// that forwards its children's notifications, a row layout, tooltips, standard
// dialogs (TaskDialog, IFileOpenDialog) and the dark title-bar hint.
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <windows.h>
#include <commctrl.h>

namespace putgd::ui {
std::wstring wide(std::string_view utf8);
std::string narrow(std::wstring_view utf16);

// ---- DPI -------------------------------------------------------------------
UINT windowDpi(HWND);
UINT systemDpi();
inline int scale(int px, UINT dpi) { return MulDiv(px, int(dpi), 96); }
bool adjustWindowRectForDpi(RECT& rect, DWORD style, DWORD exStyle, UINT dpi);
// Opts the process into per-monitor v2 awareness when the manifest did not.
void enablePerMonitorDpi();
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

// ---- Fonts -------------------------------------------------------------------
// Body 9 pt; Heading 12 pt semibold (section headers); Title 20 pt (page titles);
// Subtitle 14 pt semibold; Large 10.5 pt (selectors); Small 8 pt (captions).
enum class FontRole { Normal = 1, Bold = 2, Heading = 3, Title = 4, Subtitle = 5, Large = 6, Small = 7, SmallBold = 8 };
HFONT font(FontRole, UINT dpi);
// Sets the font and remembers its role so refreshFonts can reapply it after a DPI change.
void setFont(HWND, FontRole, UINT dpi);
void refreshFonts(HWND root, UINT dpi);
int lineHeight(UINT dpi); // the body font's height in pixels
int fontHeight(HFONT);
// Height of a control's (word-wrapped) text at the given width, in its own font.
int textHeight(HWND, int width);
// A push button's natural width: its text plus padding, at least `minimum` logical pixels.
int buttonWidth(HWND, int minimum = 96);

// ---- Controls ----------------------------------------------------------------
HWND label(HWND parent, const std::wstring& text, int id = 0, DWORD extraStyle = 0);
HWND heading(HWND parent, const std::wstring& text);
HWND edit(HWND parent, int id, DWORD extraStyle = 0);
HWND multilineReadOnly(HWND parent, int id);
HWND password(HWND parent, int id);
HWND combo(HWND parent, int id);
HWND check(HWND parent, int id, const std::wstring& text);
HWND radio(HWND parent, int id, const std::wstring& text, bool firstInGroup);
HWND button(HWND parent, int id, const std::wstring& text, DWORD extraStyle = 0);
HWND listBox(HWND parent, int id, bool ownerDrawn = false);
HWND listView(HWND parent, int id, DWORD extraStyle = 0);
HWND imageBox(HWND parent, int id);

std::string text(HWND);
std::wstring textW(HWND);
void setText(HWND, std::string_view utf8);
void setTextW(HWND, const std::wstring&);
// Sets the text only when it differs, so the caret and selection survive refreshes.
void syncText(HWND, std::string_view utf8);
bool checked(HWND);
void setChecked(HWND, bool);
void setCue(HWND edit, const std::wstring&);
void enable(HWND, bool);
void show(HWND, bool);
bool visible(HWND);
// Overwrites the control's text buffer before clearing it (secret inputs).
void wipe(HWND);

struct Choice { std::wstring label; std::string value; bool enabled = true; };
void setChoices(HWND combo, const std::vector<Choice>&);
std::string selected(HWND combo);
bool select(HWND combo, const std::string& value);

// ListView helpers (report view).
void addColumn(HWND list, int index, const std::wstring& title, int width);
int addRow(HWND list, const std::vector<std::wstring>& cells, LPARAM data);
std::vector<int> selectedRows(HWND list);
LPARAM rowData(HWND list, int row);

// ---- Layout ------------------------------------------------------------------
// Positions a control; combo boxes get room for their drop-down list and the
// given height for their face; framed edits and cards sit inside the rectangle.
void place(HWND, int x, int y, int width, int height);

// Simple top-to-bottom rows: a label column and a control column.
class Rows {
public:
    Rows(UINT dpi, int x, int y, int width, int labelWidth = 190);
    int px(int value) const { return scale(value, dpi_); }
    int y() const { return y_; }
    int x() const { return x_; }
    int width() const { return width_; }
    int controlX() const { return x_ + label_; }
    int controlWidth() const { return width_ - label_; }
    int fieldHeight() const;
    int buttonHeight() const;
    void heading(HWND);                                 // a section header; a divider is painted above all but the first
    void text(HWND, int lines = 1);                     // wrapped label across the full width (measured; `lines` is a minimum)
    // label | control [width 0 = fill] | trailing control
    void field(HWND label, HWND control, int controlWidth = 0, HWND trailing = nullptr, int trailingWidth = 0);
    // Shown under the control column only when it has text; hidden otherwise.
    void error(HWND);
    // Controls side by side starting at the given column (true = control column).
    // Widths: > 0 logical pixels, 0 the rest of the row, < 0 the button's natural width.
    void line(const std::vector<std::pair<HWND, int>>& items, bool controlColumn = false, int height = 0);
    void full(HWND, int height);
    // A control of a given pixel height in the control column (or across the full width).
    void block(HWND, int heightPx, bool controlColumn = true);
    void space(int value);
private:
    UINT dpi_;
    int x_, y_, width_, label_;
    bool first_ = true;
};

// ---- Panel ---------------------------------------------------------------------
// A plain child window hosting controls. It forwards WM_COMMAND/WM_NOTIFY/WM_TIMER
// to its owner, paints its theme surface with the frames, cards and section
// dividers its children ask for (Theme.h), colours its children, and can scroll
// vertically (children are laid out again at the new offset).
class Panel {
public:
    std::function<void(int id, int code, HWND control)> onCommand;
    std::function<LRESULT(NMHDR*)> onNotify;
    std::function<int(int width, int height)> onLayout; // lays out children at offsetY(); returns content height
    std::function<void(UINT_PTR)> onTimer;
    std::function<bool(UINT, WPARAM, LPARAM, LRESULT&)> onMessage;
    HWND create(HWND parent, bool scrollable);
    int scrollOffset() const { return scroll_; }
    HWND hwnd() const { return hwnd_; }
    int offsetY() const { return -scroll_; }
    void layout();
    void scrollTo(int y);
    UINT dpi() const { return windowDpi(hwnd_); }
    // Marks a static control as an error (red) or notice (normal) text.
    void markError(HWND control, bool error);
    void markMuted(HWND control);
    void markFaint(HWND control); // tertiary text (captions)
private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    void updateScrollBar(int height);
    HWND hwnd_ = nullptr;
    bool scrollable_ = false, laying_ = false;
    int scroll_ = 0, content_ = 0;
    void paint();
    std::vector<HWND> errors_, muted_, faint_;
};

// ---- Tooltips ------------------------------------------------------------------
HWND createTooltip(HWND owner);
void setTooltip(HWND tooltip, HWND control, const std::wstring& text);

// ---- Theme ---------------------------------------------------------------------
bool systemPrefersDark();
// themeSetting: system | dark | light (preferences.appearance.theme).
void applyTitleBarTheme(HWND, const std::string& themeSetting);

// ---- Dialogs and shell -----------------------------------------------------------
void message(HWND owner, const std::wstring& title, const std::wstring& instruction, const std::wstring& content, bool error = false);
// A TaskDialog with a custom accept button and Cancel (default); true when accepted.
bool confirm(HWND owner, const std::wstring& title, const std::wstring& instruction, const std::wstring& content,
             const std::wstring& accept, const std::wstring& footer = {}, bool warning = true);
bool pickFolder(HWND owner, const std::wstring& title, const std::string& initial, std::string& chosen);
bool pickImageFile(HWND owner, std::string& chosen);
void revealInExplorer(const std::string& path);
void openUrl(const std::string& url);
std::wstring localTime(const std::string& isoUtc); // short date + time in the user's locale; empty if invalid
// "Just now", "12 min ago", "Today 09:41", "Yesterday 18:02", "Mon 18:02", else the short date; empty if invalid.
std::wstring relativeTime(const std::string& isoUtc);
}
