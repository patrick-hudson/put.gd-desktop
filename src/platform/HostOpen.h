#pragma once
#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <functional>

// Opening links and folders on the host (#326). D-Bus first, so nothing is
// started from this process: the desktop portal's OpenURI and OpenDirectory,
// and the file manager's ShowItems. Without them, `gio open` (or xdg-open)
// runs with host::environment(), never with the AppImage's libraries.
// Every request ends with one result: {"route": "portal" | "file-manager" |
// "launcher", "opened": bool}, plus "fallbackFrom" (the D-Bus error or portal
// response that sent it on) and "pid" for a launcher.
namespace putgd::host {
using Opened=std::function<void(const QJsonObject&)>;
// An http(s) link in the default browser. Anything else is refused.
void openLink(const QUrl& link,Opened done={});
// A local file selected in its folder: ShowItems, then the portal's
// OpenDirectory, then `gio open` on the folder.
void showInFolder(const QString& path,Opened done={});
// A local file in its default application, through `gio open`: the portal's
// OpenFile can ask which application to use before it opens anything.
void openFile(const QString& path,Opened done={});
// Tests: how long a D-Bus call and a portal's answer may take.
void setOpenTimeoutsMs(int callMs,int responseMs);
// Tests: every request goes here instead, as the URL it would open (a folder
// as its file URL); an empty function restores the host.
void setTestOpener(std::function<void(const QUrl&)> opener);
}
