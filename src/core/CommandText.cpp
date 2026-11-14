#include "CommandText.h"
#include "Settings.h"
#include <QJsonArray>

namespace putgd::commandtext {
namespace {
const QString nothing = QStringLiteral("Nothing is saved or uploaded");
const QString unchosen = QStringLiteral("Choose a destination");
bool uploads(const QJsonObject& route) { return route["destination"] == "upload" && !route["profileId"].toString().isEmpty(); }
bool editsOrRecords(const QString& action) { return action == "edit" || action == "record"; }
}

QString outputName(const QJsonObject& document, const QJsonObject& route) {
 const bool local = document["archive"].toObject()["keepUploads"].toBool();
 if (route["destination"] == "local" || route["profileId"].toString().isEmpty()) return local ? QStringLiteral("Local only") : nothing;
 for (const auto& value : document["profiles"].toArray()) {
  const auto profile = value.toObject();
  if (profile["id"] != route["profileId"]) continue;
  const auto mode = route["publicationMode"].toString("default");
  const bool proton = profile["provider"] == "proton-drive";
  const bool privateUpload = mode == "private" || (mode == "default" && proton && !profile["settings"].toObject()["shareByDefault"].toBool(true));
  return (privateUpload ? (proton ? QStringLiteral("Private upload") : QStringLiteral("Private upload unavailable")) : QStringLiteral("Shared link"))
   + " · " + profile["label"].toString() + (local ? QStringLiteral(" + local copy") : QString{});
 }
 return unchosen;
}

OutputSummary outputSummary(const QJsonObject& document) {
 OutputSummary summary;
 const bool keep = document["archive"].toObject()["keepUploads"].toBool();
 const auto defaults = document.contains("captureDefaults") ? document["captureDefaults"].toObject() : Settings::effectiveAction(document, "region");
 summary.defaultName = outputName(document, defaults);
 const auto noLocalCopy = [&](const QJsonObject& route) { return !keep && uploads(route) ? QStringLiteral(" · No local copy") : QString{}; };
 // Exceptions with the same outcome share one row, in command order.
 QStringList keys; QHash<QString, QStringList> names; QHash<QString, QString> outcomes;
 QStringList onDefault;
 for (const auto& action : Settings::presentationOrder()) {
  const auto route = Settings::effectiveAction(document, action);
  const auto output = outputName(document, route);
  const bool differs = output != summary.defaultName || route["profileId"].toString() != defaults["profileId"].toString();
  const bool upload = uploads(route);
  // Editing and recording upload or save only when you choose to; the others right away.
  const auto timing = editsOrRecords(action) ? (upload ? QStringLiteral("Upload when you choose") : QStringLiteral("Save when you choose"))
                                             : (upload ? QStringLiteral("Uploads right away") : QStringLiteral("Saves locally"));
  const auto outcome = output == nothing ? output : timing + " · " + output + noLocalCopy(route);
  const auto label = Settings::actionLabel(action);
  summary.output.insert(action, output); summary.differs.insert(action, differs); summary.detail.insert(action, label + ": " + outcome);
  summary.mixed |= differs;
  if (!differs) { if (editsOrRecords(action)) onDefault << label; continue; }
  const auto key = (upload ? route["profileId"].toString() : QString{}) + ":" + outcome;
  if (!keys.contains(key)) { keys << key; outcomes.insert(key, outcome); }
  names[key] << label;
 }
 summary.rows << (summary.mixed ? QStringLiteral("Default: ") : QStringLiteral("All actions: ")) + summary.defaultName + noLocalCopy(defaults);
 // The default row carries no timing, because it is not the same for every
 // action. The next row says it for the editing and recording actions on the
 // default, and only when the default has an output to keep.
 const bool usable = summary.defaultName != nothing && summary.defaultName != unchosen && !summary.defaultName.startsWith("Private upload unavailable");
 if (usable && !onDefault.isEmpty()) summary.rows << onDefault.join(" · ") + ": " + (uploads(defaults) ? QStringLiteral("Upload when you choose") : QStringLiteral("Save when you choose"));
 for (const auto& key : std::as_const(keys)) summary.rows << names.value(key).join(" · ") + ": " + outcomes.value(key);
 summary.exceptions = int(keys.size());
 summary.band = summary.mixed ? QStringLiteral("Mixed outputs") : summary.defaultName == "Local only" ? QStringLiteral("Captures save locally")
  : summary.defaultName == nothing ? QStringLiteral("Captures have no output") : "Capture output: " + summary.defaultName;
 summary.tray = "Capture output: " + (summary.mixed ? QStringLiteral("mixed") : summary.defaultName);
 return summary;
}

QString delayTitle(int seconds) { return seconds > 0 ? QStringLiteral("Capture delay: %1 seconds").arg(seconds) : QStringLiteral("Capture delay: none"); }
QString delayChoice(int seconds) { return seconds > 0 ? QStringLiteral("%1 seconds").arg(seconds) : QStringLiteral("None"); }
QString delayTooltip(int seconds) { return QStringLiteral("Waits %1 seconds first").arg(seconds); }
QString hotkeysImmediate() { return QStringLiteral("Hotkeys capture at once"); }

QString recordingPhase(const QJsonObject& operation) {
 if (operation["action"] != "record" || operation["state"] != "recording") return {};
 const auto phase = operation["phase"].toString(operation["state"].toString());
 if (phase == "preparing") return "Setting up the recording";
 if (phase == "ready") return "Ready to record";
 if (phase == "recording") return "Recording";
 if (phase == "paused") return "Recording paused";
 if (phase == "stopping") return "Stopping the recording";
 if (phase == "reviewing") return "Reviewing the recording";
 if (phase == "exporting") return "Saving a copy of the recording";
 if (phase == "encoding") return "Encoding the recording";
 return "Recording";
}
}
