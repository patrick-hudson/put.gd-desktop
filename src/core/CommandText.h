#pragma once
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace putgd::commandtext {
// What new captures do with their result, written once for the tray's Capture
// output submenu, the window's status band and its output popup.
struct OutputSummary {
 // The default route's words: "Shared link · Proton Drive + local copy",
 // "Local only", "Nothing is saved or uploaded" or "Choose a destination".
 QString defaultName;
 bool mixed = false;
 // The status band's control: "Capture output: <name>", "Mixed outputs",
 // "Captures save locally" or "Captures have no output".
 QString band;
 // The tray row, which names itself: "Capture output: <name>" or "Capture output: mixed".
 QString tray;
 // The detail rows in order: "Default: …" (or "All actions: …" when nothing
 // differs), the timing row for the editing and recording actions on the
 // default ("Capture region & edit · Record screen: Upload when you choose",
 // only when the default keeps something), then one row per group of
 // exceptions with the same outcome. Each is "<names>: <outcome>"; names in
 // one row are joined with " · ".
 QStringList rows;
 // How many of rows are exceptions: the last ones.
 int exceptions = 0;
 // Per routed action: its own output words, its full detail line
 // ("Capture window: Saves locally · Local only") and whether it differs from the default.
 QHash<QString, QString> output, detail;
 QHash<QString, bool> differs;
};
// The output words for one effective route (Settings::effectiveAction) of the document.
QString outputName(const QJsonObject& document, const QJsonObject& route);
// Whether uploads to this destination get a link to copy: every destination but an SFTP one
// without a Link address (#1871), whose uploads say where the file went instead.
bool givesLinks(const QJsonObject& profile);
OutputSummary outputSummary(const QJsonObject& document);

// The capture delay (#1363): "Capture delay: none" / "Capture delay: 5 seconds",
// a choice's label ("None", "5 seconds") and the line a delayed button's tooltip adds.
QString delayTitle(int seconds);
QString delayChoice(int seconds);
QString delayTooltip(int seconds);
// The disabled row under the choices while a delay is set and hotkeys skip it.
QString hotkeysImmediate();

// A recording operation's phase in words ("Recording paused"); empty for any
// operation that is not an active recording session.
QString recordingPhase(const QJsonObject& operation);
}
