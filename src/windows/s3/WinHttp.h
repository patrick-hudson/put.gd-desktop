#pragma once
// Minimal synchronous WinHTTP transport for the S3 worker: one session per
// process, one connection per request (keep-alive is disabled so WinHTTP can
// never transparently replay a request on a reused connection that lost its
// reply), no redirects, no cookies, no automatic authentication. Certificate
// errors always fail. Bodies stream from a file (or a byte range of one) in 1 MiB reads.
// Nothing here logs; URLs, headers and bodies stay with the caller.
#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace putgd::winhttp {
struct Limits {
    int connectTimeoutMs = 10000;          // resolve + connect
    int requestTimeoutMs = 60000;          // each send / receive
    long long lowSpeedBytesPerSecond = 1;  // body upload must average at least this ...
    int lowSpeedTimeSeconds = 30;          // ... over every window of this length
};

enum class Failure { None, Connection, Tls, Timeout, LowSpeed, Cancelled, BodyRead, Invalid, Internal };

struct Request {
    std::string method;          // GET, HEAD, PUT, DELETE
    std::string scheme;          // https, or http (loopback only; the caller enforces it)
    std::string host;            // authority as signed: host[:port], IPv6 in brackets
    std::string path;            // already percent-encoded, starts with '/'
    std::string query;           // encoded, without '?'; empty for none
    std::vector<std::pair<std::string, std::string>> headers; // sent verbatim (Host is replaced by `host`)
    std::string body;            // small in-memory body (used when bodyFile is empty)
    std::string bodyFile;        // streamed body: the whole file, which must be bodyBytes long ...
    long long bodyOffset = -1;   // ... or, when >= 0, bodyBytes from this offset (a multipart part)
    long long bodyBytes = 0;
    size_t maximumResponseBytes = 64 * 1024;
};

struct Response {
    int status = 0;                              // 0 when no HTTP response arrived
    std::map<std::string, std::string> headers;  // lower-case names
    std::string body;                            // at most maximumResponseBytes
    bool truncated = false;
    Failure failure = Failure::None;
    unsigned long systemError = 0;               // WinHTTP / Win32 error number
    // True once the complete request (headers and every body byte) was handed to
    // the transport, or when that cannot be ruled out: the server may have acted.
    bool delivered = false;
    long long elapsedMs = 0;
    long long bytesSent = 0;
    std::string header(const std::string& lowerCaseName) const;
};

struct Observer {
    const std::atomic<bool>* cancel = nullptr;                    // checked between body writes
    // Cancel also cuts the request while a body write blocks or the reply is
    // awaited (the request handle is closed within ~50 ms). Only for requests
    // that cannot publish, such as UploadPart; a PUT that may have published drains.
    bool cutAnywhere = false;
    std::function<void(long long sent, long long total)> progress; // body bytes handed to the transport
    std::function<void()> bodySent;                               // every body byte written; awaiting reply
};

class Session {
public:
    Session() = default;
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    bool open();
    Response perform(const Request&, const Limits&, const Observer& = {});
private:
    void* handle_ = nullptr; // HINTERNET
};
}
