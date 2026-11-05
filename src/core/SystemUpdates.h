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
// {kind: "software-updates", manager, repository, channel, updater, command,
// otherChannel, older} while the repository file is in use, otherwise {kind:
// "download", repository: "absent"|"disabled"|"other"|"none"}. root prefixes
// the system paths (tests); PUTGD_UPDATE_SYSTEM_ROOT does the same for the lab.
//
// A repository install follows its repository's channel. put.gd never changes
// it (#386): otherChannel is {channel, commands, docs}, the commands someone
// runs in a terminal to move to the other channel, or null when the
// repository's suite is neither channel. older is set by the check child:
// {version, command} when the repository's release is older than this build.
QJsonObject systemAction(const QString& target,const QString& root,const QString& repository,const UpdaterChoice&);
// This computer: the compiled repository, the session's desktop and PATH.
QJsonObject systemAction(const QString& target);
// The commands that move a repository install to channel: the package's copy
// of that channel's repository file into place, then the package from it,
// even when it is older. manager is apt or dnf.
QStringList moveCommands(const QString& manager,const QString& channel);
// The last of them alone: installs the repository's package once its file is
// in place, which the system updater won't do while it is older.
QString installFromRepositoryCommand(const QString& manager,const QString& channel);
// Where the docs explain moving between channels.
QString channelDocs();
// Launches the updater systemAction names; false when there is none.
bool openSoftwareUpdates(const QJsonObject& action);
}
