#include "SdkRuntime.h"
#include <aws/core/Aws.h>
#include <mutex>
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
#if defined(PUTGD_SDK_CURL_TRANSPORT)
thread_local bool destructiveRequest = false;
// libcurl can transparently replay an empty-body DELETE when a reused connection
// loses its response. That bypasses the SDK retry strategy and is unsafe for S3
// marker creation. Use the SDK's supported per-handle customization, preserving
// its signing, request/response handling, timeout and certificate implementation.
class NonReplayingCurlClient final : public Aws::Http::CurlHttpClient {
public:
    explicit NonReplayingCurlClient(const Aws::Client::ClientConfiguration& config)
        : Aws::Http::CurlHttpClient(config) {}
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
    void OverrideOptionsOnConnectionHandle(CURL* handle) const override {
        if (destructiveRequest) {
            curl_easy_setopt(handle, CURLOPT_FRESH_CONNECT, 1L);
            curl_easy_setopt(handle, CURLOPT_FORBID_REUSE, 1L);
        }
    }
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
    struct Registry {
        std::mutex mutex;
        State* current = nullptr;
    };
    static Registry& registry() {
        static Registry value;
        return value;
    }
    State() {
#if defined(PUTGD_SDK_CURL_TRANSPORT)
        options.httpOptions.httpClientFactory_create_fn = [] {
            return Aws::MakeShared<NonReplayingHttpFactory>("putgd-http");
        };
#endif
    }
    // The registry mutex serializes this with creation and complete shutdown.
    void ensureInitialized() {
        if (!initialized) {
            Aws::InitAPI(options);
            initialized = true;
        }
    }
    ~State() {
        if (initialized) Aws::ShutdownAPI(options);
    }
};
SdkRuntime::SdkRuntime(Initialization initialization) : state_(std::make_unique<State>()) {
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
