#pragma once
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

// The tray menu, as one table both trays read: the dbusmenu export
// (NativeTray) and the QMenu of the QSystemTrayIcon fallback (TrayController).
// It uses only what plain dbusmenu has: labels, separators, submenus, radio
// marks, disabled rows and visible flags. Nothing depends on icons, headings,
// tooltips or custom rows.
namespace putgd::traymenu {
// Everything a row's label, visibility and marks come from.
struct State {
 QString recording;                     // the recording's phase in words; empty without one
 QString upload;                        // "Uploading 2 files…" / "Preparing upload…"; empty when nothing uploads
 int uploads = 0;                       // upload operations; more than one offers "Cancel all uploads…"
 bool attention = false;                // a failure or setup step waits to be looked at
 QString attentionLabel = QStringLiteral("Failed uploads");
 QString output = QStringLiteral("Capture output");   // the output submenu's title
 QStringList outputRows;                // its disabled detail rows (commandtext::OutputSummary::rows)
 int delay = 0;                         // the capture delay in seconds
 bool hotkeysDelay = false;             // preferences.captureDelayShortcuts
 QString update;                        // "Update available: <version>"; empty hides the row
 QHash<QString, QString> shortcuts;     // action -> the desktop's bound key (portable text)
 bool encryptedRegion = false;             // #2079: Capture region's destination can encrypt (its check passed)
 bool operator==(const State&) const = default;
};
enum class Kind { Command, Separator, Submenu, Info, Choice };
// Native menu IDs are 1-based table positions. They never move, and a retired
// ID is never reused: a row that leaves the menu keeps its slot, unknown on the
// wire. New rows are appended.
int count();
bool known(int id);
Kind kind(int id);
// What a click on the row does: an action ID ("region") or a tray command
// ("settings", "delay-5", "show-recording"); empty for rows that do nothing.
QString action(int id);
// The row's stable name: the QMenu action's objectName in the fallback tray
// and the catalog's row identity ("region", "trayCancelUpload").
QString name(int id);
int parent(int id);
// The rows of the menu (0) or a submenu, in display order.
QList<int> children(int id);
struct Shown {
 QString label;
 bool visible = true, enabled = true;
 bool conditional = false;              // visible changes with State; sent explicitly
 int toggle = -1;                       // radio rows: 1 chosen, 0 not; -1 for other rows
 QString shortcut;                      // the bound key, for rows that start an action
};
Shown shown(int id, const State&);
// Whether a click on the row does something in this state.
bool dispatches(int id, const State&);
// The menu as plain text, one row per line, submenus indented: what a tray host
// shows, for the catalog and for review. Hidden rows are left out.
QString text(const State&);
}
