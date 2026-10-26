#include "s3/WinHttp.h"
#include "common/Paths.h"
#include "common/Text.h"
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <memory>
#include <vector>

namespace putgd::winhttp {
namespace {
constexpr DWORD readChunk = 1024 * 1024;  // file reads
constexpr DWORD writeSlice = 64 * 1024;   // WinHttpWriteData calls, so speed and cancel are observed often

struct Handle {
    HINTERNET h = nullptr;
    Handle() = default;
    explicit Handle(HINTERNET handle) : h(handle) {}
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};
struct File {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~File() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};

bool tlsError(DWORD e) {
    switch (e) {
    case ERROR_WINHTTP_SECURE_FAILURE: case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID: case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_INVALID_CA: case ERROR_WINHTTP_SECURE_CERT_REV_FAILED: case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
    case ERROR_WINHTTP_SECURE_INVALID_CERT: case ERROR_WINHTTP_SECURE_CERT_REVOKED: case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
    case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
        return true;
    default:
        return false;
    }
}
// Failures that happen before any request byte can reach the server.
bool beforeDelivery(DWORD e) {
    return tlsError(e) || e == ERROR_WINHTTP_NAME_NOT_RESOLVED || e == ERROR_WINHTTP_CANNOT_CONNECT || e == ERROR_WINHTTP_INVALID_URL ||
           e == ERROR_WINHTTP_UNRECOGNIZED_SCHEME || e == ERROR_WINHTTP_AUTO_PROXY_SERVICE_ERROR || e == ERROR_WINHTTP_AUTODETECTION_FAILED ||
           e == ERROR_WINHTTP_BAD_AUTO_PROXY_SCRIPT || e == ERROR_WINHTTP_UNABLE_TO_DOWNLOAD_SCRIPT;
}
Failure classify(DWORD e) {
    if (tlsError(e)) return Failure::Tls;
    if (e == ERROR_WINHTTP_TIMEOUT) return Failure::Timeout;
    if (e == ERROR_WINHTTP_OPERATION_CANCELLED) return Failure::Cancelled;
    return Failure::Connection;
}
bool headerSafe(const std::string& s) {
    for (const unsigned char c : s) if (c < 0x20 || c >= 0x7f) return false;
    return true;
}
std::wstring wideAscii(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::string narrowAscii(const std::wstring& s) {
    std::string out;
    out.reserve(s.size());
    for (const wchar_t c : s) out += (c < 0x80) ? char(c) : '?';
    return out;
}
void parseHeaders(const std::wstring& raw, std::map<std::string, std::string>& out) {
    size_t start = 0;
    bool first = true; // the status line
    while (start < raw.size()) {
        auto end = raw.find(L"\r\n", start);
        if (end == std::wstring::npos) end = raw.size();
        const auto line = narrowAscii(raw.substr(start, end - start));
        start = end + 2;
        if (first) { first = false; continue; }
        const auto colon = line.find(':');
        if (colon == std::string::npos || colon == 0) continue;
        const auto name = text::lower(text::trim(line.substr(0, colon)));
        const auto value = text::trim(line.substr(colon + 1));
        auto it = out.find(name);
        if (it == out.end()) out.emplace(name, value);
        else it->second += "," + value;
    }
}
}

std::string Response::header(const std::string& lowerCaseName) const {
    const auto it = headers.find(lowerCaseName);
    return it == headers.end() ? std::string() : it->second;
}

Session::~Session() {
    if (handle_) WinHttpCloseHandle(static_cast<HINTERNET>(handle_));
}

bool Session::open() {
    if (handle_) return true;
    HINTERNET session = WinHttpOpen(L"putgd-s3-worker", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) session = WinHttpOpen(L"putgd-s3-worker", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    DWORD modern = protocols | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &modern, sizeof modern))
#endif
        WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof redirects);
    handle_ = session;
    return true;
}

Response Session::perform(const Request& request, const Limits& limits, const Observer& observer) {
    Response response;
    const ULONGLONG started = GetTickCount64();
    auto finish = [&](Failure failure, DWORD error) {
        response.failure = failure;
        response.systemError = error;
        response.elapsedMs = (long long)(GetTickCount64() - started);
        return response;
    };
    if (!handle_ && !open()) return finish(Failure::Internal, GetLastError());

    // Host and port for the connection; the Host header keeps the signed authority.
    const auto url = text::parseUrl(request.scheme + "://" + request.host + "/");
    const bool secure = request.scheme == "https";
    if (!url.valid || (!secure && request.scheme != "http") || request.path.empty() || request.path[0] != '/') return finish(Failure::Invalid, 0);
    for (const auto& [name, value] : request.headers) if (name.empty() || !headerSafe(name) || !headerSafe(value) || text::containsChar(name, ':')) return finish(Failure::Invalid, 0);
    if (!headerSafe(request.path) || !headerSafe(request.query) || !headerSafe(request.host)) return finish(Failure::Invalid, 0);
    std::string host = url.host;
    if (host.size() > 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    const INTERNET_PORT port = url.port >= 0 ? INTERNET_PORT(url.port) : (secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT);

    Handle connection(WinHttpConnect(static_cast<HINTERNET>(handle_), paths::wide(host).c_str(), port, 0));
    if (!connection) return finish(Failure::Internal, GetLastError());
    const auto target = request.path + (request.query.empty() ? std::string() : "?" + request.query);
    // Paths and queries are fully encoded already: send them byte for byte.
    Handle handle(WinHttpOpenRequest(connection.h, wideAscii(request.method).c_str(), wideAscii(target).c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, (secure ? WINHTTP_FLAG_SECURE : 0) | WINHTTP_FLAG_ESCAPE_DISABLE | WINHTTP_FLAG_ESCAPE_DISABLE_QUERY));
    if (!handle) return finish(Failure::Internal, GetLastError());

    const int connectMs = std::max(1, limits.connectTimeoutMs), requestMs = std::max(1, limits.requestTimeoutMs);
    if (!WinHttpSetTimeouts(handle.h, connectMs, connectMs, requestMs, requestMs)) return finish(Failure::Internal, GetLastError());
    DWORD features = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_KEEP_ALIVE | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    if (!WinHttpSetOption(handle.h, WINHTTP_OPTION_DISABLE_FEATURE, &features, sizeof features)) return finish(Failure::Internal, GetLastError());

    const bool streamed = !request.bodyFile.empty();
    const long long total = streamed ? request.bodyBytes : (long long)request.body.size();
    const bool large = total > 0xFFFFFFFFLL;
    std::wstring headers = L"Host: " + wideAscii(request.host) + L"\r\n";
    for (const auto& [name, value] : request.headers) {
        if (text::lower(name) == "host" || text::lower(name) == "content-length") continue;
        headers += wideAscii(name) + L": " + wideAscii(value) + L"\r\n";
    }
    if (large) headers += L"Content-Length: " + wideAscii(std::to_string(total)) + L"\r\n";
    if (!WinHttpAddRequestHeaders(handle.h, headers.c_str(), DWORD(-1L), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE))
        return finish(Failure::Invalid, GetLastError());

    File file;
    if (streamed) {
        file.h = CreateFileW(paths::wide(request.bodyFile).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        LARGE_INTEGER size{};
        if (file.h == INVALID_HANDLE_VALUE || !GetFileSizeEx(file.h, &size) || size.QuadPart != total) return finish(Failure::BodyRead, GetLastError());
    }
    if (observer.cancel && observer.cancel->load()) return finish(Failure::Cancelled, 0);

    const bool hasBody = total > 0;
    BOOL ok;
    if (streamed || large)
        ok = WinHttpSendRequest(handle.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, large ? WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH : DWORD(total), 0);
    else
        ok = WinHttpSendRequest(handle.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, hasBody ? const_cast<char*>(request.body.data()) : WINHTTP_NO_REQUEST_DATA,
                                DWORD(total), DWORD(total), 0);
    if (!ok) {
        const DWORD error = GetLastError();
        // A request whose body never started cannot complete at the server; a
        // bodyless one may have been fully written before the failure.
        const bool bodyPending = streamed && hasBody;
        response.delivered = !bodyPending && !beforeDelivery(error);
        return finish(classify(error), error);
    }

    if (streamed && hasBody) {
        std::vector<char> buffer(readChunk);
        long long sent = 0, windowBytes = 0;
        ULONGLONG windowStart = GetTickCount64();
        const ULONGLONG windowMs = ULONGLONG(std::max(1, limits.lowSpeedTimeSeconds)) * 1000;
        const long long windowMinimum = std::max(1LL, limits.lowSpeedBytesPerSecond) * std::max(1, limits.lowSpeedTimeSeconds);
        if (observer.progress) observer.progress(0, total);
        while (sent < total) {
            DWORD read = 0;
            const DWORD want = DWORD(std::min<long long>(readChunk, total - sent));
            if (!ReadFile(file.h, buffer.data(), want, &read, nullptr) || read == 0) return finish(Failure::BodyRead, GetLastError());
            for (DWORD offset = 0; offset < read;) {
                if (observer.cancel && observer.cancel->load()) return finish(Failure::Cancelled, 0);
                const DWORD slice = std::min(writeSlice, read - offset);
                DWORD written = 0;
                if (!WinHttpWriteData(handle.h, buffer.data() + offset, slice, &written)) {
                    const DWORD error = GetLastError();
                    return finish(classify(error), error);
                }
                if (written == 0) return finish(Failure::Connection, 0);
                offset += written;
                sent += written;
                windowBytes += written;
                response.bytesSent = sent;
                if (observer.progress) observer.progress(sent, total);
                const ULONGLONG now = GetTickCount64();
                if (now - windowStart >= windowMs) {
                    // Scale the minimum to the measured window, which can exceed its nominal length.
                    const double required = double(windowMinimum) * double(now - windowStart) / double(windowMs);
                    if (double(windowBytes) < required && sent < total) return finish(Failure::LowSpeed, 0);
                    windowStart = now;
                    windowBytes = 0;
                }
            }
        }
    }
    response.delivered = true;
    if (observer.bodySent) observer.bodySent();

    if (!WinHttpReceiveResponse(handle.h, nullptr)) {
        const DWORD error = GetLastError();
        return finish(classify(error), error);
    }
    DWORD status = 0, size = sizeof status;
    if (!WinHttpQueryHeaders(handle.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
        return finish(Failure::Connection, GetLastError());
    response.status = int(status);
    DWORD rawBytes = 0;
    WinHttpQueryHeaders(handle.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &rawBytes, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && rawBytes > 0 && rawBytes < 1024 * 1024) {
        std::wstring raw(rawBytes / sizeof(wchar_t) + 1, L'\0');
        if (WinHttpQueryHeaders(handle.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &rawBytes, WINHTTP_NO_HEADER_INDEX)) {
            raw.resize(rawBytes / sizeof(wchar_t));
            parseHeaders(raw, response.headers);
        }
    }
    if (request.method != "HEAD" && status != 204 && status != 304) {
        std::vector<char> chunk(16 * 1024);
        while (response.body.size() < request.maximumResponseBytes) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(handle.h, &available)) {
                const DWORD error = GetLastError();
                // The status arrived; a cut body only matters to callers that read it.
                return finish(classify(error), error);
            }
            if (available == 0) break;
            const DWORD want = DWORD(std::min<size_t>({size_t(available), chunk.size(), request.maximumResponseBytes - response.body.size()}));
            DWORD read = 0;
            if (!WinHttpReadData(handle.h, chunk.data(), want, &read)) {
                const DWORD error = GetLastError();
                return finish(classify(error), error);
            }
            if (read == 0) break;
            response.body.append(chunk.data(), read);
        }
        if (response.body.size() >= request.maximumResponseBytes) {
            DWORD available = 0;
            response.truncated = WinHttpQueryDataAvailable(handle.h, &available) && available > 0;
        }
    }
    return finish(Failure::None, 0);
}
}
