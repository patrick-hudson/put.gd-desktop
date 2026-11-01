#pragma once
#include "core/Error.h"
#include <QJsonObject>
#include <QStringList>
namespace putgd {
// "Upload with put.gd" in file managers (#1105): the per-user files put.gd
// writes. DEB and RPM menu files and plugins are system files; the system
// install adds only its GNOME desktop icons (DING) script and its COSMIC entry
// here. The AppImage writes every per-user entry through a stable launcher in
// <dataRoot>/file-manager, which also holds each installation's owned-files
// record (<installation>.owned) and version stamp (<installation>.stamp).
struct FileManagerContext {
 QString home,dataHome,configHome,dataRoot; // XDG roots and put.gd's data root (owned-files record and version stamp live there)
 QString installation;                       // host::installationClass()
 QString appImage;                           // $APPIMAGE, read only when installation=="appimage"
 QString systemLauncher=QStringLiteral("/usr/bin/putgd-launcher");
 QString iconSource;                         // putgd.svg to copy next to the AppImage launcher
 bool enabled=true;                          // Settings::fileManagerMenu; ignored for "system"
 QString version;                            // putgd::buildVersion()
 // What the system has, for tests; empty asks the system. dataDirs: XDG_DATA_DIRS
 // (system menu files, DING, COSMIC Files). libraryDirs: where /usr/lib*
 // <dir>/nautilus/extensions-4, caja/extensions-2.0 and peony-extensions hold
 // put.gd's plugins. searchPath: PATH, for the file-manager programs.
 QStringList dataDirs,libraryDirs;
 QString searchPath;
 QString desktop,kdeVersion;                 // XDG_CURRENT_DESKTOP and KDE_SESSION_VERSION, for tests; empty asks the environment
 QString cgroupFile=QStringLiteral("/proc/self/cgroup"); // what the AppImage launcher reads
 QString userRuntime; // the login's runtime directory, where its user manager listens; empty: /run/user/<uid>
};
struct FileManagerResult {
 Error error;               // e.g. {"cosmic_context_actions_unparseable",...}: nothing written to that file
 bool firstRun=false;       // no version stamp existed for this installation identity; stamp now written
 bool announce=false;       // firstRun, on a desktop where this installation has an entry: say so
 QStringList written,removed,skipped; // absolute paths: changed now; removed; not written (a duplicate, or a file put.gd does not manage)
 QStringList restartable;   // firstRun only: the session's own file manager when it needs a restart. "nautilus"/"caja"/"peony" with a system plugin; "pcmanfm"/"pcmanfm-qt" with a FileManager-Actions entry
 QString launcher;          // the stable launcher script (AppImage)
 QJsonObject toJson() const;
};
class FileManagerIntegration {
public:
 static FileManagerResult apply(const FileManagerContext&);
 // Arms or stops the integration's own user-manager path unit + cleanup service
 // (AppImage only; separate from LoginStartup's). Context-free tests don't touch systemd.
 // No user manager is no error: an entry used after the image is gone removes
 // the entries itself. A failure's details name the stage, exit status and reason.
 static Error monitor(const FileManagerContext&,bool enabled);
 // Whether this failure should be announced: once until it changes or clears
 // (no error), across resident starts. It stays in the log and diagnostics each time.
 static bool announceOnce(const FileManagerContext&,const Error&);
 // What the cleanup service runs when the AppImage is gone: remove owned files and our COSMIC entry only.
 // The service itself runs the launcher's shell copy (no put.gd binary is left
 // then); this is the same cleanup in process, for any put.gd resident.
 static Error cleanupIfMissing(const FileManagerContext&);
 // The menu label and the per-user file paths, for tests and diagnostics.
 static QString label();
 // The /bin/sh -c script that restarts a restartable file manager, its name as $1; empty for any other name.
 static QString restartScript(const QString& name);
 // The existing system directories whose put.gd files (menu files, plugins) make
 // per-user ones duplicates; a DEB or RPM installed beside the AppImage lands there.
 static QStringList systemDirectories(const FileManagerContext&);
 static QString launcherPath(const FileManagerContext&);
 static QString cosmicPath(const FileManagerContext&);
};
}
