#pragma once
#include "core/Error.h"
#include <QByteArray>
#include <QString>
namespace putgd::minisign {
// minisign signatures (#293 phase 3): the release signer signs each
// release's SHA256SUMS with the channel's key (prehashed "ED", or legacy "Ed").
// Only putgd-cli's install child links this and OpenSSL's libcrypto; the
// resident never does.

// The release key a build of this channel trusts (packaging/linux/repo/keys),
// as the base64 line of its .pub file; empty for any other channel.
QByteArray releaseKey(const QString& channel);
// False in a build without libcrypto: then every verification fails.
bool available();
// Checks signatureFile (a .minisig) over message with publicKey (the base64
// line or the whole .pub file): key id, the signature and the global signature
// over the trusted comment. On success *trustedComment is the comment.
// Refusals are update_signature_invalid with details.check naming what failed.
Error verify(const QByteArray& message,const QByteArray& signatureFile,const QByteArray& publicKey,QString* trustedComment=nullptr);
}
