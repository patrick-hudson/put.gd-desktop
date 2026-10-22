#pragma once
// The log's hard redaction (docs/diagnostics.md, "Redaction"), the same rules as
// the Linux logging::redact: authorization values, signed-URL parameters, URL
// queries and fragments, values after secret-like names, bearer/basic
// credentials, PEM private keys, AWS access key IDs, 40-character secret-key
// shapes and long base64 runs. Structured fields named like secrets or content
// lose their value. A hand-written scanner rather than std::regex: libstdc++'s
// regex recurses per character and can exhaust the stack on a long line.
#include "common/Json.h"
#include <string>
#include <string_view>

namespace putgd::redact {
std::string text(std::string_view);
json::Value value(const json::Value&);
json::Object object(const json::Object&);
// password, secretKey, token, credentials, clipboard, text, content, payload ...
bool sensitiveKey(std::string_view key);
}
