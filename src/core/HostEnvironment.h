#pragma once
#include <QHash>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <optional>

// The environment of a host program put.gd starts: a browser or file manager
// through gio, a software updater, gsettings, coredumpctl (#326). The AppImage's
// AppRun puts the image's libraries and Qt plugins first; a host gio that
// inherited them loaded the image's older GLib and exited 127, so the
// notification's link never opened. AppRun saves the host's values of what it
// replaces as PUTGD_HOST_<NAME> and lists the names in PUTGD_HOST_ENVIRONMENT.
// Outside an AppImage nothing is restored or removed, so DEB and RPM builds
// pass their environment on as before.
namespace putgd::host {
inline constexpr char savedNamesVariable[]="PUTGD_HOST_ENVIRONMENT";
inline constexpr char savedPrefix[]="PUTGD_HOST_";
// Variables a process changed for itself, with the value it started with
// (nullopt: it was unset).
using Kept=QHash<QString,std::optional<QString>>;
struct Inputs { QProcessEnvironment current; Kept kept; QString executable; };
QProcessEnvironment environment(const Inputs& inputs);
// This process's: the system environment, what keep() saved, /proc/self/exe.
QProcessEnvironment environment();
// Saves this process's value of a variable before it changes it for itself,
// so that host programs get the original.
void keep(const char* name);
// Whether this process runs from an AppImage mount: AppRun's list, or an
// executable inside APPDIR.
bool fromAppImage(const QProcessEnvironment& current,const QString& executable);
// Which installation this process belongs to (#1105): "appimage" only when its
// executable lies inside APPDIR, otherwise "system". Neither a saved host
// environment nor APPIMAGE alone counts: a DEB process can inherit both from an
// unrelated AppImage app, and must never run that image.
bool insideAppDir(const QProcessEnvironment& current,const QString& executable);
QString installationClass(const QProcessEnvironment& current,const QString& executable);
QString installationClass();
// The program's absolute path on the environment's PATH, or empty.
QString findProgram(const QString& program,const QProcessEnvironment& environment);
// Starts a host program detached with environment(), its standard streams on
// /dev/null: it outlives this process and must not hold its pipes.
bool startDetached(const QString& program,const QStringList& arguments,qint64* pid=nullptr);
}
