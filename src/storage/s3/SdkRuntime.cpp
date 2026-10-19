#include "SdkRuntime.h"
#include "TlsCompatibility.h"
#include <aws/core/Aws.h>
#include <aws/core/utils/logging/LogSystemInterface.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <memory>
#include <condition_variable>
#include <exception>
#include <thread>
#include <stdexcept>
#if defined(PUTGD_SDK_CURL_TRANSPORT)
#include <aws/core/http/HttpClientFactory.h>
#include <aws/core/http/URI.h>
#include <aws/core/http/curl/CurlHttpClient.h>
#include <aws/core/http/standard/StandardHttpRequest.h>
#include <curl/curl.h>
#include <csignal>
#endif

namespace putgd {
namespace {
// One stderr line per SDK record; the resident redacts and stores it.
class StderrLogSystem final : public Aws::Utils::Logging::LogSystemInterface {
public:
    explicit StderrLogSystem(Aws::Utils::Logging::LogLevel level) : level_(level) {}
    Aws::Utils::Logging::LogLevel GetLogLevel() const override { return level_; }
    void Log(Aws::Utils::Logging::LogLevel level, const char* tag, const char* format, ...) override {
        va_list args;
        va_start(args, format);
        vaLog(level, tag, format, args);
        va_end(args);
    }
    void vaLog(Aws::Utils::Logging::LogLevel level, const char* tag, const char* format, va_list args) override {
        if (!enabled(level)) return;
        char text[2048];
        std::vsnprintf(text, sizeof text, format, args);
        writeLine(level, tag, text);
    }
    void LogStream(Aws::Utils::Logging::LogLevel level, const char* tag, const Aws::OStringStream& stream) override {
        if (enabled(level)) writeLine(level, tag, stream.str());
    }
    void Flush() override { std::fflush(stderr); }
private:
    bool enabled(Aws::Utils::Logging::LogLevel level) const {
        return level != Aws::Utils::Logging::LogLevel::Off && level <= level_ && level <= Aws::Utils::Logging::LogLevel::Info;
    }
    void writeLine(Aws::Utils::Logging::LogLevel level, const char* tag, const std::string_view text) {
        using Aws::Utils::Logging::LogLevel;
        const char mark = level <= LogLevel::Error ? 'E' : level == LogLevel::Warn ? 'W' : 'I';
        std::string line = "putgd-aws\t";
        line += mark;
        line += '\t';
        for (const char* c = tag ? tag : ""; *c && line.size() < 80; ++c) line += *c == '\t' || *c == '\n' ? ' ' : *c;
        line += '\t';
        for (const char c : text.substr(0, 2000)) {
            if (c == '\n') line += " | ";
            else if (c == '\t' || c == '\r') line += ' ';
            else line += c;
        }
        while (line.ends_with(" | ")) line.resize(line.size() - 3);
        line += '\n';
        std::lock_guard lock(mutex_);
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fflush(stderr);
    }
    const Aws::Utils::Logging::LogLevel level_;
    std::mutex mutex_;
};
Aws::Utils::Logging::LogLevel sdkLevel(SdkRuntime::LogLevel level) {
    switch (level) {
    case SdkRuntime::LogLevel::Error: return Aws::Utils::Logging::LogLevel::Error;
    case SdkRuntime::LogLevel::Info: return Aws::Utils::Logging::LogLevel::Info;
    case SdkRuntime::LogLevel::Warn: break;
    }
    return Aws::Utils::Logging::LogLevel::Warn;
}
#if defined(PUTGD_SDK_CURL_TRANSPORT)
thread_local bool destructiveRequest = false;
// libcurl can transparently replay an empty-body DELETE when a reused connection
// loses its response. That bypasses the SDK retry strategy and is unsafe for S3
// marker creation. Use the SDK's supported per-handle customization, preserving
// its signing, request/response handling, timeout and certificate implementation.
class NonReplayingCurlClient final : public Aws::Http::CurlHttpClient {
public:
    explicit NonReplayingCurlClient(const Aws::Client::ClientConfiguration& config)
        : Aws::Http::CurlHttpClient(config) {
#if LIBCURL_VERSION_NUM >= 0x074900
        const auto* version = curl_version_info(CURLVERSION_NOW);
        smallInitialShare_ = version && version->ssl_version &&
            smallInitialTlsShare(version->version_num, version->ssl_version) && defaultSystemTlsGroups();
        if (smallInitialShare_) {
            // Validate before the SDK acquires any request handle or headers.
            std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> probe(curl_easy_init(), curl_easy_cleanup);
            smallInitialShare_ = probe &&
                curl_easy_setopt(probe.get(), CURLOPT_SSL_EC_CURVES, "?X25519MLKEM768:DEFAULT") == CURLE_OK;
        }
#endif
    }
    std::shared_ptr<Aws::Http::HttpResponse> MakeRequest(const std::shared_ptr<Aws::Http::HttpRequest>& request,
        Aws::Utils::RateLimits::RateLimiterInterface* read = nullptr,
        Aws::Utils::RateLimits::RateLimiterInterface* write = nullptr) const override {
        struct RequestScope {
            bool previous = destructiveRequest;
            explicit RequestScope(bool destructive) { destructiveRequest = destructive; }
            ~RequestScope() { destructiveRequest = previous; }
        } scope(request->GetMethod() == Aws::Http::HttpMethod::HTTP_DELETE);
        return Aws::Http::CurlHttpClient::MakeRequest(request, read, write);
    }
protected:
    void OverrideOptionsOnConnectionHandle(CURL* handle) const noexcept override {
#if LIBCURL_VERSION_NUM >= 0x074900
        if (smallInitialShare_ &&
            curl_easy_setopt(handle, CURLOPT_SSL_EC_CURVES, "?X25519MLKEM768:DEFAULT") != CURLE_OK)
            // Never throw from this SDK hook: it owns an acquired pooled handle.
            // If allocation fails, retain secure TLS defaults and normal errors.
            (void)curl_easy_setopt(handle, CURLOPT_SSL_EC_CURVES, nullptr);
#endif
        if (destructiveRequest) {
            curl_easy_setopt(handle, CURLOPT_FRESH_CONNECT, 1L);
            curl_easy_setopt(handle, CURLOPT_FORBID_REUSE, 1L);
        }
    }
private:
    bool smallInitialShare_ = false;
};
class NonReplayingHttpFactory final : public Aws::Http::HttpClientFactory {
public:
    std::shared_ptr<Aws::Http::HttpClient> CreateHttpClient(const Aws::Client::ClientConfiguration& config) const override {
        return Aws::MakeShared<NonReplayingCurlClient>("putgd-http", config);
    }
    std::shared_ptr<Aws::Http::HttpRequest> CreateHttpRequest(const Aws::String& uri, Aws::Http::HttpMethod method,
                                                          const Aws::IOStreamFactory& stream) const override {
        return CreateHttpRequest(Aws::Http::URI(uri), method, stream);
    }
    std::shared_ptr<Aws::Http::HttpRequest> CreateHttpRequest(const Aws::Http::URI& uri, Aws::Http::HttpMethod method,
                                                          const Aws::IOStreamFactory& stream) const override {
        auto request = Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>("putgd-http", uri, method);
        request->SetResponseStreamFactory(stream);
        return request;
    }
    void InitStaticState() override {
        Aws::Http::CurlHttpClient::InitGlobalState();
#ifndef _WIN32
        // Preserve the default SDK factory's process-level SIGPIPE behavior.
        std::signal(SIGPIPE, +[](int) {});
#endif
    }
    void CleanupStaticState() override { Aws::Http::CurlHttpClient::CleanupGlobalState(); }
};
#endif
}
struct SdkRuntime::State {
    Aws::SDKOptions options;
    bool initialized = false;
    const bool deferred;
    std::thread owner;
    std::mutex ownerMutex;
    std::condition_variable ownerChanged;
    bool ready = false;
    bool stopping = false;
    std::exception_ptr initializationError;
    struct Registry {
        std::mutex mutex;
        State* current = nullptr;
    };
    static Registry& registry() {
        static Registry value;
        return value;
    }
    explicit State(bool defer) : deferred(defer) {
        const auto level = sdkLevel(SdkRuntime::logLevel(std::getenv("PUTGD_AWS_LOG_LEVEL")));
        options.loggingOptions.logLevel = level;
        options.loggingOptions.logger_create_fn = [level] {
            return Aws::MakeShared<StderrLogSystem>("putgd-log", level);
        };
#if defined(PUTGD_SDK_CURL_TRANSPORT)
        options.httpOptions.httpClientFactory_create_fn = [] {
            return Aws::MakeShared<NonReplayingHttpFactory>("putgd-http");
        };
#endif
    }
    // Production module owners initialize immediately on their permanent network
    // worker. Standalone deferred owners accept arbitrary caller threads, so their
    // lazy SDK init/shutdown pair needs its own permanent owner thread.
    void ensureInitialized() {
        if (initialized) return;
        if (!deferred) {
            Aws::InitAPI(options);
            initialized = true;
            return;
        }
        std::unique_lock lock(ownerMutex);
        if (!owner.joinable()) owner = std::thread([this] {
            {
                std::lock_guard ownerLock(ownerMutex);
                try {
                    Aws::InitAPI(options);
                    initialized = true;
                } catch (...) {
                    initializationError = std::current_exception();
                }
                ready = true;
                ownerChanged.notify_all();
            }
            std::unique_lock ownerLock(ownerMutex);
            ownerChanged.wait(ownerLock, [this] { return stopping; });
            ownerLock.unlock();
            if (initialized) Aws::ShutdownAPI(options);
        });
        ownerChanged.wait(lock, [this] { return ready; });
        if (initializationError) std::rethrow_exception(initializationError);
    }
    ~State() {
        if (owner.joinable()) {
            {
                std::lock_guard lock(ownerMutex);
                stopping = true;
                ownerChanged.notify_one();
            }
            owner.join();
        } else if (initialized) Aws::ShutdownAPI(options);
    }
};
SdkRuntime::SdkRuntime(Initialization initialization) : state_(std::make_unique<State>(initialization == Initialization::Deferred)) {
    auto& registry = State::registry();
    std::lock_guard lock(registry.mutex);
    if (registry.current) throw std::logic_error("Only one owning SdkRuntime may exist at a time");
    if (initialization == Initialization::Immediate) state_->ensureInitialized();
    registry.current = state_.get();
}
void SdkRuntime::ensureInitialized() {
    auto& registry = State::registry();
    std::lock_guard lock(registry.mutex);
    if (!registry.current) throw std::logic_error("S3 work requires an owning SdkRuntime");
    registry.current->ensureInitialized();
}
SdkRuntime::LogLevel SdkRuntime::logLevel(const char* value) {
    if (value && std::strcmp(value, "error") == 0) return LogLevel::Error;
    if (value && std::strcmp(value, "info") == 0) return LogLevel::Info;
    return LogLevel::Warn;
}
bool SdkRuntime::safeDeleteTransport() {
#if defined(PUTGD_SDK_CURL_TRANSPORT)
    return true;
#else
    return false;
#endif
}
SdkRuntime::~SdkRuntime() {
    auto& registry = State::registry();
    std::lock_guard lock(registry.mutex);
    // Keep the generation registered until SDK cleanup is complete. A later
    // owner cannot initialize against a previous generation still shutting down.
    state_.reset();
    registry.current = nullptr;
}
}
