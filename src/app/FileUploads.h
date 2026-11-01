#pragma once
#include "core/Settings.h"
#include <QStringList>
namespace putgd {
// "Upload with put.gd" from a file manager (#1105): the checks and routing the
// session method upload.files runs before any operation exists.
namespace FileUploads {
// The destination a file-manager upload goes to, resolved once at acceptance:
// the requested profile, else the capture default when it is an upload with a
// profile, else the only destination. Never a local-only save.
struct Route { Error error; QString profileId,publicationMode=QStringLiteral("default"); };
Route resolve(const Settings&,const QString& requestedProfile={});
// The operation's own settings document: captureDefaults names the resolved
// route, so routeOf, retry and a restarted resident read the same destination.
QJsonObject routedDocument(const Settings&,const Route&);
// Every requested path checked, folders expanded per preferences.fileManagerFolders,
// and the result bounded by the file count, the size limits, the destination's
// largest object (0: none) and the disk (negative: unknown). Runs on a worker:
// over GVfs or KIOFuse each stat may be slow. Rejections name every bad file.
struct Selection {
 Error error;
 QStringList files;   // the acquisition's sources: files, and folders in zip mode
 QStringList origins; // the requested path each source came from
 bool zipFolders=false;
 qint64 sourceBytes=0,diskBytes=0;
};
Selection select(const QStringList& paths,const Settings&,const QString& action,qint64 objectLimit,qint64 diskAvailable);
// The installation identity check, the first step of upload.files.
Error matchInstallation(const QString& requested,const QString& resident);
}
}
