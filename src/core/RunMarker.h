#pragma once
#include <QJsonObject>
#include <QString>
namespace putgd {
// The resident's "running" marker (#74 §4) in the state folder. A marker found
// at startup means the previous resident never shut down cleanly; it is logged,
// kept in a short history and reported by diagnostics and diagnostics packages.
// SIGTERM, SIGHUP and SIGINT note their number beside the marker, then take
// their default action, so a session logout is told apart from a crash or a
// power loss. Crash backtraces are #74 §5.
class RunMarker {
public:
 static constexpr int retainedRecords=10;
 // Records a marker left behind, then writes this resident's own.
 static void begin(const QString& stateRoot,const QString& version);
 // Clean shutdown: removes the marker.
 static void end();
 // {detected, last, recent[]}: whether this start found an unclean exit, that
 // record, and the latest retained records, newest first.
 static QJsonObject report();
};
}
