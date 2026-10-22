#pragma once
// S3 multipart upload rules (docs/s3.md, "Large files: multipart upload") as pure
// functions: the part plan, part ranges, ETag bookkeeping, the
// CompleteMultipartUpload document, the CreateMultipartUpload/ListParts replies
// and the resume journal the resident keeps on the history item. Portable: no
// Windows headers, so tests/test_multipart.cpp builds on any host. The same
// numbers as the Linux src/storage/s3/S3Multipart.h.
#include "common/Error.h"
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace putgd::s3::multipart {
// S3's limits: parts of 5 MiB to 5 GiB (only the last may be smaller), at most
// 10,000 parts and objects of at most 5 TiB.
inline constexpr long long minimumPartBytes = 5LL * 1024 * 1024;
inline constexpr long long maximumPartBytes = 5LL * 1024 * 1024 * 1024;
inline constexpr long long maximumParts = 10000;
inline constexpr long long maximumObjectBytes = 5LL * 1024 * 1024 * 1024 * 1024;
// put.gd's policy: files up to 64 MiB use one PutObject; larger files go in
// 16 MiB parts, or larger in whole MiB when 10,000 parts would not hold them.
inline constexpr long long thresholdBytes = 64LL * 1024 * 1024;
inline constexpr long long defaultPartBytes = 16LL * 1024 * 1024;

// Part size for a file, or zero for a single PutObject.
long long partBytes(long long bytes);
long long partCount(long long bytes, long long partBytes);
bool validPlan(long long bytes, long long partBytes);
struct Range { long long offset = 0, length = 0; };
// Byte range of part `number` (1-based); length 0 when the number is outside the plan.
Range partRange(int number, long long bytes, long long partBytes);

// ETags: S3 quotes them; a plain part ETag (no SSE-KMS) is the part's hex MD5.
std::string plainEtag(std::string_view);
bool md5Etag(std::string_view plain);
// The completed object's ETag when every part ETag is a plain MD5: the MD5 of the
// part MD5s in part order, a dash and the part count. Empty when any part is not.
std::string compositeEtag(const std::map<int, std::string>& partEtags);

// XML ---------------------------------------------------------------------------
std::string xmlEscape(std::string_view);
// The five predefined entities and numeric references; nullopt for anything else.
std::optional<std::string> xmlUnescape(std::string_view);
// Unescaped text of the first <name>…</name> in `body` at or after `from`, at
// most `maximum` bytes; nullopt when absent, too long or badly escaped.
std::optional<std::string> xmlText(std::string_view body, std::string_view name, size_t maximum, size_t from = 0);
// A 200 reply that carries an S3 <Error> document (CompleteMultipartUpload can
// fail after its status line was sent).
bool errorDocument(std::string_view body);
// The CompleteMultipartUpload body: every part in number order with its quoted ETag.
std::string completeRequest(const std::map<int, std::string>& partEtags);
bool validUploadId(std::string_view);
// The UploadId of a CreateMultipartUpload reply; empty when missing or invalid.
std::string createdUploadId(std::string_view body);
struct ListedPart { int number = 0; std::string etag; long long size = -1; };
struct PartListing {
    bool ok = false;
    std::vector<ListedPart> parts;
    bool truncated = false;
    int nextMarker = 0;
};
PartListing parseListParts(std::string_view body);

// Resume journal -------------------------------------------------------------
// The plan alone {schemaVersion, partBytes}, or the plan plus the created
// upload {uploadId, key, bytes, sha256, phase: parts|completing}; the same shape
// the Linux worker protocol accepts. An empty object is no journal.
bool validJournal(const json::Object&);
bool journalCreated(const json::Object&);
// True when a created journal belongs to exactly this target, artifact and plan.
bool journalMatches(const json::Object&, const std::string& key, long long bytes, const std::string& sha256, long long partBytes);
}
