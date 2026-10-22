// SigV4 signer checks against the AWS documented S3 examples
// (sig-v4-header-based-auth.html, "Example: GET Object" and its derived steps).
// Portable: includes only the signer.
#include "s3/SigV4.h"
#include <chrono>
#include <cstdio>

using namespace putgd;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
    sigv4::Credentials credentials{"AKIAIOSFODNN7EXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY", {}};

    // GET Object: GET /test.txt with Range: bytes=0-9.
    sigv4::Request get;
    get.method = "GET";
    get.host = "examplebucket.s3.amazonaws.com";
    get.path = "/test.txt";
    get.headers = {{"Range", "bytes=0-9"}};
    get.payloadHash = sigv4::emptyPayloadHash;
    const auto signedGet = sigv4::sign(get, credentials, "us-east-1", "s3", "20130524T000000Z");
    CHECK(signedGet.signedHeaders == "host;range;x-amz-content-sha256;x-amz-date");
    CHECK(signedGet.canonicalRequest ==
          "GET\n/test.txt\n\nhost:examplebucket.s3.amazonaws.com\nrange:bytes=0-9\n"
          "x-amz-content-sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n"
          "x-amz-date:20130524T000000Z\n\nhost;range;x-amz-content-sha256;x-amz-date\n"
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(signedGet.stringToSign ==
          "AWS4-HMAC-SHA256\n20130524T000000Z\n20130524/us-east-1/s3/aws4_request\n"
          "7344ae5b7ee6c3e7e6b0fe0640412a37625d1fbfff95c48bbb2dc43964946972");
    CHECK(signedGet.signature == "f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
    CHECK(signedGet.authorization ==
          "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request,"
          "SignedHeaders=host;range;x-amz-content-sha256;x-amz-date,"
          "Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");

    // GET Bucket Lifecycle: a valueless query parameter signs as "lifecycle=".
    sigv4::Request lifecycle;
    lifecycle.method = "GET";
    lifecycle.host = "examplebucket.s3.amazonaws.com";
    lifecycle.path = "/";
    lifecycle.query = {{"lifecycle", ""}};
    lifecycle.payloadHash = sigv4::emptyPayloadHash;
    const auto signedLifecycle = sigv4::sign(lifecycle, credentials, "us-east-1", "s3", "20130524T000000Z");
    CHECK(signedLifecycle.query == "lifecycle=");
    CHECK(signedLifecycle.signature == "fea454ca298b7da1c68078a5d1bdbfbbe0d65c699e0f91ac7a200a0136783543");

    // GET Bucket (list objects): sorted, encoded query.
    sigv4::Request list = lifecycle;
    list.query = {{"prefix", "J"}, {"max-keys", "2"}};
    const auto signedList = sigv4::sign(list, credentials, "us-east-1", "s3", "20130524T000000Z");
    CHECK(signedList.query == "max-keys=2&prefix=J");
    CHECK(signedList.signature == "34b48302e7b5fa45bde8084f4b7868a86f0a534bc59db6670ed5711ef69dc6f7");

    // PUT Object with a payload hash and an extra x-amz header.
    sigv4::Request put;
    put.method = "PUT";
    put.host = "examplebucket.s3.amazonaws.com";
    put.path = "/test%24file.text";
    put.headers = {{"Date", "Fri, 24 May 2013 00:00:00 GMT"}, {"x-amz-storage-class", "REDUCED_REDUNDANCY"}, {"Content-Length", "11"}};
    put.payloadHash = "44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072";
    const auto signedPut = sigv4::sign(put, credentials, "us-east-1", "s3", "20130524T000000Z");
    CHECK(signedPut.signedHeaders == "date;host;x-amz-content-sha256;x-amz-date;x-amz-storage-class");
    CHECK(signedPut.signature == "98ad721746da40c64f1a55b78f14c238d841ea1380cd77a1b5971af0ece108bd");

    // A session token is signed and sent.
    sigv4::Credentials temporary = credentials;
    temporary.sessionToken = "token  value";
    const auto signedToken = sigv4::sign(get, temporary, "us-east-1", "s3", "20130524T000000Z");
    CHECK(signedToken.signedHeaders == "host;range;x-amz-content-sha256;x-amz-date;x-amz-security-token");
    CHECK(sigv4::canonicalHeaderValue("  a   b  ") == "a b");

    CHECK(sigv4::amzDate(std::chrono::system_clock::time_point(std::chrono::seconds(1369353600))) == "20130524T000000Z");
    CHECK(sigv4::amzDate(std::chrono::system_clock::time_point(std::chrono::seconds(951782400 + 86399))) == "20000229T235959Z");
    CHECK(sigv4::canonicalQuery({{"versionId", "a b/+"}, {"a", ""}}) == "a=&versionId=a%20b%2F%2B");

    temporary.wipe();
    CHECK(temporary.secretKey.empty() && temporary.sessionToken.empty());

    if (failures) { std::fprintf(stderr, "sigv4: %d failure(s)\n", failures); return 1; }
    std::printf("sigv4: ok\n");
    return 0;
}
