#pragma once
#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <functional>
// `putgd --upload -- <paths or URIs>`: the short-lived client a file manager's
// "Upload with put.gd" entry runs (#1105). It hands the selection to the
// resident (starting one in its own scope when none runs), waits until put.gd
// holds its own copy, and reports every refusal or failure as a notification,
// since no terminal is attached. It never becomes the resident itself.
namespace putgd::UploadClient {
// argv[1] is --upload. Runs before any Qt application object exists: Qt would
// read its own options (-platform, ...) from the selected file names.
bool requested(int argc,char** argv);
int run(int argc,char** argv);

// The pieces below are separate for tests.
// Whether the leaf unit of a /proc/self/cgroup text is a .service: a file
// manager started by D-Bus activation runs in one, and stopping it stops
// everything left in its cgroup. Every user process sits under
// user@<uid>.service, so only the last path component counts.
bool leafUnitIsService(const QByteArray& cgroup);
// LD_LIBRARY_PATH without the private SDK library directory the DEB/RPM
// wrapper adds beside the binary (bindir/../<libdir>/putgd); every other entry stays.
QString withoutPrivateLibraries(const QString& libraryPath,const QString& binaryDirectory);
// A URI with its scheme and host lowercased, escaping and the default port
// normalised (GLib's GUri), so gio's printed URI matches the requested one.
QString normalizedUri(const QString& uri);
// Whether the percent-decoded URI contains a newline anywhere: gio prints such
// a path flattened, so it could match a different, real file.
bool decodedNewline(const QString& uri);
// uri → local path from the complete output of `gio info` (one record per uri: line).
QHash<QString,QString> parseGioInfo(const QByteArray& output);
// needingGio: the unresolved arguments only gio could have resolved, when it couldn't run.
struct Resolution { QStringList paths,unresolved,needingGio; };
// What the host's gio found; `unavailable` when it couldn't run at all, as opposed
// to running and finding no local path.
struct GioAnswer { QHash<QString,QString> found; bool unavailable=false; };
// Each argument after `--upload --`: an absolute path stays as given; a URI is
// resolved to a filesystem path (GVfs FUSE, KIOFuse) or left unresolved;
// anything else is unresolved. `gioInfo` runs the AppImage's host gio for the
// URIs it can't decode itself; DEB/RPM builds resolve through GIO in-process.
using GioInfo=std::function<GioAnswer(const QStringList& uris)>;
Resolution resolve(const QStringList& arguments,bool appImage,const GioInfo& gioInfo);
// How a refusal names an unresolved argument: its file name, or the whole URI
// for a Recent item, whose last segment is a hash rather than a name.
QString noticeName(const QString& argument);
// Whether the resident predates upload.files. A current one can answer
// invalid_request too, so that code is settled by asking again with no
// parameters: a resident that doesn't know the method then says unknown_method,
// and one that does repeats invalid_request. `codeWithoutParameters` makes that call.
bool residentLacksUpload(const QString& code,const std::function<QString()>& codeWithoutParameters);
// The names of the files the resident reported missing (`details.files` of its
// refusal) whose whole GVfs FUSE mount can't be seen: the share was disconnected or
// gvfsd-fuse stopped, and the files themselves may be fine. A missing file on a
// mount that answers isn't one.
QStringList unreachableOverFuse(const QJsonArray& files,const QString& runtimeDirectory);
// A notice through the host's own dialog tool, for a session with no notification
// server: without one, stderr would be all a right-click shows. False when no
// display or tool could show it. `environment` is the host's own.
bool showDialog(const QString& title,const QString& body,const QProcessEnvironment& environment);
}
