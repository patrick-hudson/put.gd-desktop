#pragma once
#include "core/Error.h"
#include <QByteArray>
#include <QJsonObject>
#include <QString>
// The request/reply frames every session.sock client exchanges with the
// resident, and the per-method reply deadlines. Core-only: the resident's own
// --upload client (#1105) and SessionClient (QLocalSocket, QtNetwork) share
// these so the resident never links QtNetwork (acceptance matrix gate C).
namespace putgd {
// The resident answers these only after a destination test or an upload (#123);
// LocalServer::replyDeadlineMs gives them the same time.
inline constexpr int diagnosticsBundleReplyMs = 600000, diagnosticsUploadTargetReplyMs = 300000;
// history.preview answers once a missing preview is made (#190), after at most
// a short wait behind other previews and one bounded decode.
inline constexpr int historyPreviewReplyMs = 45000;
// desktop.setup.recheck runs gsettings and waits for the Shell, each bounded.
inline constexpr int desktopSetupRecheckReplyMs = 90000;
// upload.files stats every selected file, and every file in a selected folder,
// before it answers (#1105): slow over GVfs or KIOFuse.
inline constexpr int uploadFilesReplyMs = 120000;
// settings.importUploader stores the file's secret values first; a locked keyring
// shows its own unlock prompt, which a person has to answer (#1361).
inline constexpr int uploaderImportReplyMs = 120000;
namespace SessionProtocol {
inline constexpr qsizetype maximumRequest = 1024 * 1024;
inline constexpr qsizetype maximumResponse = 16 * 1024 * 1024;
// One request frame: the compact JSON envelope and its terminating newline.
QByteArray encodeRequest(const QString& wireId, const QString& session, const QString& method,
                         const QJsonObject& params);
// How long a request waits for its reply: 15 s, or longer for the methods
// the resident answers late, plus the time for the reply to cross the socket.
int deadlineMs(const QString& method);
// One complete reply frame checked against the request's wireId. False when
// the frame does not match the protocol; error and result are then untouched.
bool decodeReply(const QByteArray& frame, const QString& wireId, Error& error, QJsonObject& result);
// submitted: the request reached the transport, so the outcome is unknown.
Error invalidResponse(bool submitted);
}
}
