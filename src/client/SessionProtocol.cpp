#include "SessionProtocol.h"
#include <QJsonDocument>
#include <QJsonParseError>

namespace putgd::SessionProtocol {
namespace {
constexpr int deadlineMilliseconds = 15000;
// Time for the reply to cross the socket after the resident's own deadline.
constexpr int transportMarginMilliseconds = 15000;
}

QByteArray encodeRequest(const QString& wireId, const QString& session, const QString& method,
                         const QJsonObject& params) {
    return QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"requestId", wireId}, {"session", session},
                                     {"method", method}, {"params", params}})
               .toJson(QJsonDocument::Compact) + '\n';
}

int deadlineMs(const QString& method) {
    if (method == "diagnostics.bundle") return diagnosticsBundleReplyMs + transportMarginMilliseconds;
    if (method == "diagnostics.uploadTarget") return diagnosticsUploadTargetReplyMs + transportMarginMilliseconds;
    if (method == "history.preview") return historyPreviewReplyMs + transportMarginMilliseconds;
    if (method == "upload.files") return uploadFilesReplyMs + transportMarginMilliseconds;
    return deadlineMilliseconds;
}

bool decodeReply(const QByteArray& frame, const QString& wireId, Error& error, QJsonObject& result) {
    if (frame.size() > maximumResponse || !frame.endsWith('\n') || frame.indexOf('\n') != frame.size() - 1) return false;
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(frame, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return false;
    const auto envelope = document.object();
    const auto reported = envelope["error"].toObject();
    if (!envelope["schemaVersion"].isDouble() || envelope["schemaVersion"].toDouble() != 1 ||
        !envelope["requestId"].isString() || envelope["requestId"].toString() != wireId ||
        !envelope["ok"].isBool() || !envelope["result"].isObject() ||
        !envelope["error"].isObject() || !reported["code"].isString() ||
        !reported["message"].isString() || !reported["details"].isObject() ||
        envelope["ok"].toBool() != reported["code"].toString().isEmpty()) return false;
    error = {reported["code"].toString(), reported["message"].toString(), reported["details"].toObject()};
    result = envelope["result"].toObject();
    return true;
}

Error invalidResponse(bool submitted) {
    return {"invalid_response", "Resident response does not match the request protocol",
            submitted ? QJsonObject{{"outcomeUnknown", true}} : QJsonObject{}};
}
}
