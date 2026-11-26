#include "TrayMenu.h"
#include "core/CommandText.h"
#include "core/Settings.h"
#include <functional>

namespace putgd::traymenu {
namespace {
constexpr int retired = -1;
struct Row { Kind kind; const char* action; const char* label; int parent; const char* name; };
// ID = position + 1. Never reorder, remove or reuse an entry; retire it.
constexpr Row rows[] {
 /* 1*/ {Kind::Command, "region", "", 0, "region"},
 /* 2*/ {Kind::Command, "window", "", 0, "window"},
 /* 3*/ {Kind::Command, "desktop", "", 0, "desktop"},
 /* 4*/ {Kind::Command, "clipboard", "", 0, "clipboard"},
 /* 5*/ {Kind::Command, "edit", "", 0, "edit"},
 /* 6*/ {Kind::Separator, "", "", 0, "traySeparatorScreenshots"},
 /* 7*/ {Kind::Command, "settings", "Settings", 0, "preferences"},
 // 8 was "History": the same action, the main window on History, under the name that says what it opens.
 /* 8*/ {Kind::Command, "history", "Open put.gd", 0, "history"},
 // 9 was "Recent activity", which opened the same History page.
 /* 9*/ {Kind::Command, "", "", retired, ""},
 /*10*/ {Kind::Separator, "", "", 0, "traySeparatorEditing"},
 // 11 was "About put.gd"; About stays in the window's menu and ui.show.
 /*11*/ {Kind::Command, "", "", retired, ""},
 /*12*/ {Kind::Command, "quit", "Quit put.gd", 0, "trayQuit"},
 /*13*/ {Kind::Submenu, "", "Edit image", 0, "edit-sources"},
 /*14*/ {Kind::Command, "edit-clipboard", "Edit clipboard image", 13, "trayEditClipboard"},
 /*15*/ {Kind::Command, "edit-file", "Edit an image file…", 13, "trayEditFile"},
 /*16*/ {Kind::Command, "record", "", 0, "record"},
 // Hidden until a stable update is available; its label names the version (#293).
 /*17*/ {Kind::Command, "update", "Update available", 0, "update"},
 // 18 was "Destinations", a tab of the window "Open put.gd" opens.
 /*18*/ {Kind::Command, "", "", retired, ""},
 /*19*/ {Kind::Info, "", "", 0, "trayUploadStatus"},
 /*20*/ {Kind::Command, "cancel-upload", "", 0, "trayCancelUpload"},
 /*21*/ {Kind::Separator, "", "", 0, "trayStatusSeparator"},
 /*22*/ {Kind::Command, "failed-uploads", "", 0, "trayFailedUploads"},
 /*23*/ {Kind::Submenu, "", "", 0, "trayOutputSummary"},
 /*24*/ {Kind::Info, "", "", 23, "trayOutputRow0"},
 /*25*/ {Kind::Info, "", "", 23, "trayOutputRow1"},
 /*26*/ {Kind::Info, "", "", 23, "trayOutputRow2"},
 /*27*/ {Kind::Info, "", "", 23, "trayOutputRow3"},
 /*28*/ {Kind::Info, "", "", 23, "trayOutputRow4"},
 /*29*/ {Kind::Info, "", "", 23, "trayOutputRow5"},
 /*30*/ {Kind::Command, "capture-settings", "Change output…", 23, "trayChangeOutput"},
 // #1363, from 31.
 /*31*/ {Kind::Command, "monitor", "", 0, "monitor"},
 /*32*/ {Kind::Command, "repeat", "", 0, "repeat"},
 /*33*/ {Kind::Submenu, "", "", 0, "trayCaptureDelay"},
 /*34*/ {Kind::Choice, "delay-0", "", 33, "trayCaptureDelay0"},
 /*35*/ {Kind::Choice, "delay-3", "", 33, "trayCaptureDelay3"},
 /*36*/ {Kind::Choice, "delay-5", "", 33, "trayCaptureDelay5"},
 /*37*/ {Kind::Choice, "delay-10", "", 33, "trayCaptureDelay10"},
 /*38*/ {Kind::Info, "", "", 23, "trayOutputRow6"},
 /*39*/ {Kind::Command, "scroll", "", 0, "scroll"},
 /*40*/ {Kind::Info, "", "", 23, "trayOutputRow7"},
 // 41, Copy text (#1428).
 /*41*/ {Kind::Command, "text", "", 0, "text"},
 // From 42, the agreed layout (2026-11-14): the recording rows of the live
 // block, the hotkeys note under the delay, a ninth output row and the separators.
 /*42*/ {Kind::Info, "", "", 0, "trayRecordingStatus"},
 /*43*/ {Kind::Command, "show-recording", "Show recording", 0, "trayShowRecording"},
 /*44*/ {Kind::Info, "", "", 33, "trayDelayHotkeys"},
 /*45*/ {Kind::Info, "", "", 23, "trayOutputRow8"},
 /*46*/ {Kind::Separator, "", "", 23, "trayOutputSeparator"},
 /*47*/ {Kind::Separator, "", "", 0, "traySeparatorClipboard"},
 /*48*/ {Kind::Separator, "", "", 0, "traySeparatorSettings"},
 /*49*/ {Kind::Separator, "", "", 0, "traySeparatorQuit"},
 // #2079: Capture region with Seal on for this upload, shown under Capture region when its destination can seal.
 /*50*/ {Kind::Command, "region-sealed", "Capture region (sealed)", 0, "trayRegionSealed"},
};
constexpr int total = sizeof(rows) / sizeof(rows[0]);
// Output detail rows in order: the first six, then 38, 40 and 45.
int outputIndex(int id) { return id >= 24 && id <= 29 ? id - 24 : id == 38 ? 6 : id == 40 ? 7 : id == 45 ? 8 : -1; }
const Row& row(int id) { return rows[id - 1]; }
}

int count() { return total; }
bool known(int id) { return id == 0 || (id > 0 && id <= total && row(id).parent != retired); }
Kind kind(int id) { return id == 0 ? Kind::Submenu : row(id).kind; }
QString action(int id) { return id == 0 ? QString{} : QString::fromLatin1(row(id).action); }
QString name(int id) { return id == 0 ? QString{} : QString::fromLatin1(row(id).name); }
int parent(int id) { return id == 0 ? retired : row(id).parent; }

QList<int> children(int id) {
 if (id == 0) {
  // The live block (shown only while it has rows), then the commands in
  // Settings::actionTable() order with a separator at each group boundary,
  // the two capture settings, the window and the update, and Quit.
  QList<int> result{42, 43, 19, 20, 22, 21};
  int group = 0;
  for (const auto& command : Settings::actionTable()) {
   if (command.group != group) { result.append(group == 0 ? 6 : 10); group = command.group; }
   for (int child = 1; child <= total; ++child) if (row(child).parent == 0 && row(child).kind == Kind::Command && QLatin1String(row(child).action) == command.action) result.append(child);
   if (command.action == "region") result.append(50);
   if (command.action == "edit") result.append(13);
  }
  result.append({47, 33, 23, 48, 8, 7, 17, 49, 12});
  return result;
 }
 if (id == 23) return {24, 25, 26, 27, 28, 29, 38, 40, 45, 46, 30};
 QList<int> result;
 for (int child = 1; child <= total; ++child) if (row(child).parent == id) result.append(child);
 return result;
}

Shown shown(int id, const State& state) {
 Shown s;
 if (id == 0) return s;
 const auto& r = row(id);
 const auto command = QString::fromLatin1(r.action);
 s.label = Settings::shortcutOrder().contains(command) ? Settings::actionLabel(command) : QString::fromUtf8(r.label);
 if (r.kind == Kind::Info) s.enabled = false;
 if (Settings::shortcutOrder().contains(command)) s.shortcut = state.shortcuts.value(command);
 const bool block = !state.recording.isEmpty() || !state.upload.isEmpty() || state.attention;
 switch (id) {
 case 42: s.label = state.recording; s.conditional = true; s.visible = !state.recording.isEmpty(); break;
 case 43: s.conditional = true; s.visible = !state.recording.isEmpty(); break;
 case 19: s.label = state.upload; s.conditional = true; s.visible = !state.upload.isEmpty(); break;
 case 20: s.label = state.uploads > 1 ? QStringLiteral("Cancel all uploads…") : QStringLiteral("Cancel upload"); s.conditional = true; s.visible = s.enabled = !state.upload.isEmpty(); break;
 case 22: s.label = state.attentionLabel; s.conditional = true; s.visible = state.attention; break;
 case 21: s.conditional = true; s.visible = block; break;
 case 17: if (!state.update.isEmpty()) s.label = state.update; s.conditional = true; s.visible = !state.update.isEmpty(); break;
 case 23: s.label = state.output; break;
 case 33: s.label = commandtext::delayTitle(state.delay); break;
 case 44: s.label = commandtext::hotkeysImmediate(); s.conditional = true; s.visible = state.delay > 0 && !state.hotkeysDelay; break;
 case 50: s.conditional = true; s.visible = state.sealedRegion; break;
 default: break;
 }
 if (r.kind == Kind::Choice) { const int seconds = command.mid(6).toInt(); s.label = commandtext::delayChoice(seconds); s.toggle = seconds == state.delay ? 1 : 0; }
 if (const int index = outputIndex(id); index >= 0) { s.label = state.outputRows.value(index); s.conditional = true; s.visible = index < state.outputRows.size(); }
 return s;
}

bool dispatches(int id, const State& state) {
 if (id <= 0 || !known(id) || action(id).isEmpty()) return false;
 const auto s = shown(id, state);
 return s.visible && s.enabled;
}

QString text(const State& state) {
 QStringList lines;
 std::function<void(int, int)> walk = [&](int id, int depth) {
  for (const int child : children(id)) {
   const auto s = shown(child, state);
   if (!s.visible) continue;
   const QString indent(depth * 4, ' ');
   QString line;
   if (kind(child) == Kind::Separator) line = "────";
   else {
    line = s.toggle == 1 ? "● " : s.toggle == 0 ? "○ " : QString{};
    line += kind(child) == Kind::Info ? "[" + s.label + "]" : s.label;
    if (kind(child) == Kind::Submenu) line += "  ▸";
    if (!s.shortcut.isEmpty()) line += "    " + s.shortcut;
   }
   lines << indent + line + QString("    #%1").arg(child);
   if (kind(child) == Kind::Submenu) walk(child, depth + 1);
  }
 };
 walk(0, 0);
 return lines.join('\n') + '\n';
}
}
