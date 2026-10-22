#pragma once
// minisign signatures (#293 phase 3), as the Linux build checks them
// (src/cli/Minisign.cpp): the release signer signs each release's SHA256SUMS
// with the channel's key (prehashed "ED", or legacy "Ed").
#include "common/Error.h"
#include <string>

namespace putgd::minisign {
// Checks signatureFile (a .minisig) over message with publicKeyText (the base64
// line or the whole .pub file): key id, the signature and the global signature
// over the trusted comment. On success *trustedComment is the comment.
// Refusals are update_signature_invalid with details.check naming what failed.
Error verify(const std::string& message, const std::string& signatureFile, const std::string& publicKeyText, std::string* trustedComment = nullptr);
}
