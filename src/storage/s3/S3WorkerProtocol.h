#pragma once
#include "core/StorageProvider.h"
#include "secrets/CredentialStore.h"

namespace putgd::s3wire {
// Private same-build wire format. Credentials travel only on the private child
// channel; these objects must never enter diagnostics or persistent history.
inline constexpr qsizetype maximumFrameBytes = 10 * 1024 * 1024;
QJsonObject encode(const UploadRequest&);
QJsonObject encode(const DeleteRequest&);
QJsonObject encode(const NameCheck&);
QJsonObject encode(const UploadResult&);
QJsonObject encode(const StatResult&);
QJsonObject encode(const DeleteResult&);
QJsonObject encode(const Error&);
QJsonObject encode(const Credentials&);

// Decode into a temporary: invalid input never partially changes the caller.
// Cancellation remains process-local and is not part of the JSON schema.
bool decode(const QJsonObject&, UploadRequest&);
bool decode(const QJsonObject&, DeleteRequest&);
bool decode(const QJsonObject&, NameCheck&);
bool decode(const QJsonObject&, UploadResult&);
bool decode(const QJsonObject&, StatResult&);
bool decode(const QJsonObject&, DeleteResult&);
bool decode(const QJsonObject&, Destination&);
bool decode(const QJsonObject&, StorageReceipt&);
bool decode(const QJsonObject&, Artifact&);
bool decode(const QJsonObject&, Error&);
bool decode(const QJsonObject&, Credentials&);
// Request-only codecs preserve fields ignored by the SDK and historical links.
// Response receipt decoding above still requires complete canonical receipts.
bool decodeReconcileRequest(const QJsonObject&, UploadRequest&);
bool decodeReceiptRequest(const QJsonObject&, StorageReceipt&);
bool validateRequest(const QString& method, const QJsonObject&);
// Validate the actual synthetic-test result shapes without changing the payload.
bool decodeTestDestinationResult(const QJsonObject&);
}
