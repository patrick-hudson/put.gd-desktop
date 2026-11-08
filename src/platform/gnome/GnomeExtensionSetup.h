#pragma once
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>

// The per-user setup a GNOME 50 session needs: the Shell extension's files
// (a package's system copy, or the copy this AppImage carries) and its entry in
// org.gnome.shell enabled-extensions. Everything here runs in the user's session;
// a package's post-install script runs as root and cannot set per-user GNOME state.
//
// Appending to enabled-extensions before the login that makes the Shell discover
// a new extension is what turns "install, log out, enable" into "install, log
// out". Proven on a real GNOME 50.1 session on 2026-11-08 (issue records in the
// PR): `gnome-extensions enable` refuses an extension the Shell has not scanned,
// but the setting is read at the next start of the Shell.
namespace putgd::gnome {
inline constexpr char extensionUuid[]="putgd@put.gd";
// A marker put.gd writes next to a copy it installed. Anything else in that
// directory, or a copy whose files changed, is treated as someone else's.
inline constexpr char copyMarkerName[]=".putgd-managed";

// Who can see this: capabilities.helper.setup, the desktop.setup session method,
// putgd-cli and the MCP helper. `code` is non-empty when the state needs the user.
struct SetupStatus {
 QString state;
 QString code;
 QString message;
 QJsonObject details;
 bool needsUser()const{return !code.isEmpty();}
 QJsonObject toJson()const;
};

// The Shell's answer to org.gnome.Shell.Extensions: version property and
// GetExtensionInfo for our uuid. An empty extension means the Shell has not
// discovered it yet (no login since the files arrived).
struct ShellFacts {
 QString version;
 QJsonObject extension;
};
struct ProcessResult {
 bool started=false;
 int exitCode=-1;
 QString standardOutput;
 QString standardError;
};

struct SetupInputs {
 QString dataHome;
 QStringList dataDirs;
 // The running AppImage's own copy of the extension; empty for a native install.
 QString bundledDir;
 int packagedVersion=0;
 std::function<ShellFacts()> shell;
 std::function<ProcessResult(const QStringList&)> gsettings;
};

// Runs in the session (from a worker thread; blocking). Idempotent: repeating it
// with nothing to do changes no file or setting.
SetupStatus runExtensionSetup(const SetupInputs&);
// Removes a put.gd-owned user copy (exact files, then the directory) and its
// enabled-extensions entry. Never touches a copy put.gd didn't install.
SetupStatus removeOwnedUserCopy(const SetupInputs&);
// The real environment: host paths, the host's gsettings and the session bus.
SetupInputs defaultSetupInputs();

// GVariant text-format helpers used on `gsettings get` output. Exposed for tests.
std::optional<QStringList> parseStringList(const QString& text);
std::optional<QString> appendToStringList(const QString& text,const QString& value);
}
