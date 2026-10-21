#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <functional>
namespace putgd::update {
// DEB/RPM installs that update from put.gd's own repository (#293 phase 2).
// The package adds /etc/apt/sources.list.d/putgd.sources or
// /etc/yum.repos.d/putgd.repo; while that file is present, enabled and names
// the repository this build was made for, the system's software updater owns
// updates, so put.gd points there and sends no notification of its own.

// The repository this build's package adds (PUTGD_PACKAGE_REPOSITORY_URL).
QString packageRepository();

struct Updater {
 QString id,name,program;
 QStringList arguments;
};
// The desktop's own updater first (Cinnamon: Update Manager, Plasma: Discover,
// GNOME: GNOME Software), then any other one installed. found is false when
// none is installed. which() resolves a program name to a path, or empty.
struct UpdaterChoice {bool found=false;Updater updater;};
UpdaterChoice softwareUpdater(const QString& currentDesktop,const std::function<QString(const QString&)>& which);

// The action Settings, the CLI and MCP offer for this build's package:
// {kind: "software-updates", manager, repository, channel, updater, command}
// while the repository file is in use, otherwise {kind: "download",
// repository: "absent"|"disabled"|"other"|"none"}. root prefixes the system
// paths (tests); PUTGD_UPDATE_SYSTEM_ROOT does the same for the lab.
QJsonObject systemAction(const QString& target,const QString& root,const QString& repository,const UpdaterChoice&);
// This computer: the compiled repository, the session's desktop and PATH.
QJsonObject systemAction(const QString& target);
// Launches the updater systemAction names; false when there is none.
bool openSoftwareUpdates(const QJsonObject& action);
}
