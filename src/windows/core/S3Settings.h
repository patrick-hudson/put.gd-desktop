#pragma once
// Non-secret S3 profile rules shared by the settings validator, the resident's
// provider facade and the S3 worker (docs/s3.md). No network, no SDK.
#include "common/Error.h"
#include <string>
namespace putgd::s3 {
Error validateSettings(const json::Object& settings);
Error validateCredentials(const json::Object& credentials);
bool virtualAddressing(const json::Object& settings);
// Loopback literal hosts may use plain HTTP (local fixtures).
bool loopbackHost(const std::string& host);
// The direct object URL for a key: the public base when configured, else derived
// from endpoint/addressing/bucket. Each key segment is percent-encoded once.
std::string publicObjectUrl(const json::Object& settings, const std::string& key);
// Signing endpoint host for a request: bucket-prefixed in virtual mode.
std::string requestHost(const json::Object& settings);
// Request path for a key (always starts with '/'): "/bucket/key" in path mode.
std::string requestPath(const json::Object& settings, const std::string& key);
// Prefix + candidate name; a separator is added only when a nonempty prefix lacks one.
std::string objectKey(const json::Object& settings, const std::string& candidate);
}
