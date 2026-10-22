// Multipart upload rules (docs/s3.md, "Large files: multipart upload"): MD5, the
// part plan, ETag bookkeeping, the CompleteMultipartUpload document, S3's reply
// parsing and the resume journal. Portable: no Windows headers.
#include "common/Md5.h"
#include "common/Text.h"
#include "s3/Multipart.h"
#include <cstdio>
#include <string>

using namespace putgd;
namespace mp = putgd::s3::multipart;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
    constexpr long long mib = 1024 * 1024;

    // RFC 1321 test suite, plus a multi-block input.
    CHECK(crypto::Md5::hexDigest("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(crypto::Md5::hexDigest("a") == "0cc175b9c0f1b6a831c399e269772661");
    CHECK(crypto::Md5::hexDigest("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(crypto::Md5::hexDigest("message digest") == "f96b697d7cb7938d525a2f31aaf161d0");
    CHECK(crypto::Md5::hexDigest("abcdefghijklmnopqrstuvwxyz") == "c3fcd3d76192e4007dfb496cca67e13b");
    CHECK(crypto::Md5::hexDigest("12345678901234567890123456789012345678901234567890123456789012345678901234567890") == "57edf4a22be3c955ac49da2e2107b67a");
    {
        crypto::Md5 streamed;
        const std::string chunk(1000, 'a');
        for (int i = 0; i < 1000; ++i) streamed.update(chunk);
        const auto d = streamed.finish();
        CHECK(text::hex(d.data(), d.size()) == "7707d6ae4e027c70eea2a935c2296f21");
    }

    // The plan: one PutObject up to 64 MiB, then 16 MiB parts, growing in whole
    // MiB only when 10,000 parts would not hold the file.
    CHECK(mp::partBytes(0) == 0);
    CHECK(mp::partBytes(64 * mib) == 0);
    CHECK(mp::partBytes(64 * mib + 1) == 16 * mib);
    CHECK(mp::partCount(64 * mib + 1, 16 * mib) == 5);
    CHECK(mp::partBytes(10000 * 16 * mib) == 16 * mib);
    CHECK(mp::partBytes(10000 * 16 * mib + 1) == 17 * mib);
    CHECK(mp::partBytes(mp::maximumObjectBytes) == 525 * mib);
    CHECK(mp::partCount(mp::maximumObjectBytes, 525 * mib) == 9987);
    CHECK(mp::partBytes(mp::maximumObjectBytes + 1) == 0);
    for (const long long bytes : {64 * mib + 1, 66 * mib, 1000 * mib, 160000 * mib + 7, mp::maximumObjectBytes}) {
        const auto part = mp::partBytes(bytes);
        CHECK(part % mib == 0);
        CHECK(mp::validPlan(bytes, part));
        CHECK(mp::partCount(bytes, part) <= mp::maximumParts);
    }
    CHECK(!mp::validPlan(10 * mib, 4 * mib));               // below S3's 5 MiB minimum
    CHECK(!mp::validPlan(10001 * 5 * mib, 5 * mib));        // more than 10,000 parts
    CHECK(!mp::validPlan(0, 16 * mib));
    CHECK(!mp::validPlan(mp::maximumObjectBytes + 1, 600 * mib));
    CHECK(mp::validPlan(5 * mib + 1, 5 * mib));
    {
        const long long bytes = 66 * mib;
        const auto first = mp::partRange(1, bytes, 16 * mib), last = mp::partRange(5, bytes, 16 * mib);
        CHECK(first.offset == 0 && first.length == 16 * mib);
        CHECK(last.offset == 64 * mib && last.length == 2 * mib);
        CHECK(mp::partRange(6, bytes, 16 * mib).length == 0);
        CHECK(mp::partRange(0, bytes, 16 * mib).length == 0);
    }

    // ETags: quotes stripped; the completed object's ETag from the part MD5s.
    CHECK(mp::plainEtag("\"E09C80C42FDA55F9D992E59CA6B3307D\"") == "e09c80c42fda55f9d992e59ca6b3307d");
    CHECK(mp::md5Etag("e09c80c42fda55f9d992e59ca6b3307d"));
    CHECK(!mp::md5Etag("e09c80c42fda55f9d992e59ca6b3307d-2"));
    CHECK(mp::compositeEtag({{1, "\"e09c80c42fda55f9d992e59ca6b3307d\""}, {2, "\"92eb5ffee6ae2fec3ad71c777531578f\""}}) == "3192ca8a7eba2efb8c52f8e48729e5f1-2");
    // Order is part order, not insertion order; an SSE-KMS (non-MD5) ETag gives none.
    CHECK(mp::compositeEtag({{2, "\"92eb5ffee6ae2fec3ad71c777531578f\""}, {1, "\"e09c80c42fda55f9d992e59ca6b3307d\""}}) == "3192ca8a7eba2efb8c52f8e48729e5f1-2");
    CHECK(mp::compositeEtag({{1, "\"e09c80c42fda55f9d992e59ca6b3307d\""}, {2, "\"kms-opaque-tag\""}}).empty());
    CHECK(mp::compositeEtag({}).empty());

    // The CompleteMultipartUpload document lists parts in number order.
    CHECK(mp::completeRequest({{2, "\"bb\""}, {1, "\"aa\""}}) ==
          "<CompleteMultipartUpload xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
          "<Part><PartNumber>1</PartNumber><ETag>&quot;aa&quot;</ETag></Part>"
          "<Part><PartNumber>2</PartNumber><ETag>&quot;bb&quot;</ETag></Part></CompleteMultipartUpload>");

    // XML text and entities.
    CHECK(mp::xmlUnescape("&quot;a&amp;b&lt;&gt;&apos;&#65;&#x42;") == std::optional<std::string>("\"a&b<>'AB"));
    CHECK(!mp::xmlUnescape("&bogus;"));
    CHECK(!mp::xmlUnescape("&#xE9;"));
    CHECK(!mp::xmlUnescape("&amp"));
    CHECK(mp::xmlText("<a><Code>NoSuchUpload</Code></a>", "Code", 64) == std::optional<std::string>("NoSuchUpload"));
    CHECK(!mp::xmlText("<Code>toolong</Code>", "Code", 3));
    CHECK(mp::errorDocument("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Error><Code>InternalError</Code></Error>"));
    CHECK(!mp::errorDocument("<CompleteMultipartUploadResult><ETag>&quot;x&quot;</ETag></CompleteMultipartUploadResult>"));

    // CreateMultipartUpload reply.
    const std::string created = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<InitiateMultipartUploadResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
                                "<Bucket>put.gd</Bucket><Key>acceptance/a&amp;b.bin</Key>"
                                "<UploadId>VXBsb2FkIElEIGZvciA2aWWpbmcncyBteS1tb3ZpZS5tMnRzIHVwbG9hZA.x_y~z-</UploadId></InitiateMultipartUploadResult>";
    CHECK(mp::createdUploadId(created) == "VXBsb2FkIElEIGZvciA2aWWpbmcncyBteS1tb3ZpZS5tMnRzIHVwbG9hZA.x_y~z-");
    CHECK(mp::createdUploadId("<InitiateMultipartUploadResult><UploadId>bad id</UploadId></InitiateMultipartUploadResult>").empty());
    CHECK(mp::createdUploadId("<Error><UploadId>abc</UploadId></Error>").empty());
    CHECK(mp::validUploadId("abc+/=._~-"));
    CHECK(!mp::validUploadId(""));
    CHECK(!mp::validUploadId("a&b"));

    // ListParts reply: escaped ETags, header elements that look like parts, truncation.
    const std::string listed =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<ListPartsResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
        "<Bucket>put.gd</Bucket><Key>acceptance/&lt;Part&gt;.bin</Key><UploadId>abc</UploadId>"
        "<PartNumberMarker>0</PartNumberMarker><NextPartNumberMarker>2</NextPartNumberMarker><MaxParts>2</MaxParts><IsTruncated>true</IsTruncated>"
        "<Part><PartNumber>1</PartNumber><LastModified>2026-10-22T00:00:00.000Z</LastModified><ETag>&quot;e09c80c42fda55f9d992e59ca6b3307d&quot;</ETag><Size>16777216</Size></Part>"
        "<Part><PartNumber>2</PartNumber><LastModified>2026-10-22T00:00:00.000Z</LastModified><ETag>\"92eb5ffee6ae2fec3ad71c777531578f\"</ETag><Size>16777216</Size></Part>"
        "<Initiator><ID>x</ID></Initiator></ListPartsResult>";
    {
        const auto parsed = mp::parseListParts(listed);
        CHECK(parsed.ok);
        CHECK(parsed.parts.size() == 2);
        CHECK(parsed.truncated && parsed.nextMarker == 2);
        CHECK(parsed.parts.size() == 2 && parsed.parts[0].number == 1 && parsed.parts[0].size == 16 * mib &&
              mp::plainEtag(parsed.parts[0].etag) == "e09c80c42fda55f9d992e59ca6b3307d");
        CHECK(parsed.parts.size() == 2 && parsed.parts[1].number == 2 && mp::plainEtag(parsed.parts[1].etag) == "92eb5ffee6ae2fec3ad71c777531578f");
    }
    {
        const auto empty = mp::parseListParts("<ListPartsResult><Bucket>b</Bucket><IsTruncated>false</IsTruncated></ListPartsResult>");
        CHECK(empty.ok && empty.parts.empty() && !empty.truncated);
        CHECK(!mp::parseListParts("<Error><Code>NoSuchUpload</Code></Error>").ok);
        CHECK(!mp::parseListParts("<ListPartsResult><IsTruncated>true</IsTruncated></ListPartsResult>").ok); // truncated without a marker
        CHECK(!mp::parseListParts("<ListPartsResult><Part><PartNumber>0</PartNumber><ETag>x</ETag><Size>1</Size></Part></ListPartsResult>").ok);
        CHECK(!mp::parseListParts("<ListPartsResult><Part><PartNumber>1</PartNumber><ETag>x</ETag></Part></ListPartsResult>").ok);
    }

    // The resume journal: plan alone, or plan plus the created upload.
    const std::string sha(64, 'a');
    json::Object plan{{"schemaVersion", 1}, {"partBytes", double(16 * mib)}};
    CHECK(mp::validJournal({}));
    CHECK(mp::validJournal(plan));
    CHECK(!mp::journalCreated(plan));
    json::Object journal = plan;
    journal["uploadId"] = "abc"; journal["key"] = "acceptance/file.bin"; journal["bytes"] = double(66 * mib); journal["sha256"] = sha; journal["phase"] = "parts";
    CHECK(mp::validJournal(journal));
    CHECK(mp::journalCreated(journal));
    CHECK(mp::journalMatches(journal, "acceptance/file.bin", 66 * mib, sha, 16 * mib));
    CHECK(!mp::journalMatches(journal, "acceptance/other.bin", 66 * mib, sha, 16 * mib));
    CHECK(!mp::journalMatches(journal, "acceptance/file.bin", 66 * mib + 1, sha, 16 * mib));
    CHECK(!mp::journalMatches(journal, "acceptance/file.bin", 66 * mib, std::string(64, 'b'), 16 * mib));
    CHECK(!mp::journalMatches(journal, "acceptance/file.bin", 66 * mib, sha, 17 * mib));
    {
        auto bad = journal; bad["phase"] = "done"; CHECK(!mp::validJournal(bad));
        bad = journal; bad.remove("key"); CHECK(!mp::validJournal(bad));
        bad = journal; bad["uploadId"] = "a b"; CHECK(!mp::validJournal(bad));
        bad = journal; bad["sha256"] = "ABC"; CHECK(!mp::validJournal(bad));
        bad = journal; bad["extra"] = true; CHECK(!mp::validJournal(bad));
        bad = journal; bad["schemaVersion"] = 2; CHECK(!mp::validJournal(bad));
        bad = plan; bad["partBytes"] = 0; CHECK(!mp::validJournal(bad));
    }

    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::puts("multipart: all checks passed");
    return 0;
}
