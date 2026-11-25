#pragma once

#include <memory>

namespace putgd {
// An adapter-private owner; destroy after all clients and SDK objects drain.
// Immediate owners stay on their constructing thread (the production module worker).
// Standalone deferred owners lazily retain one SDK init/shutdown thread.
class SdkRuntime final {
public:
    enum class Initialization { Immediate, Deferred };
    explicit SdkRuntime(Initialization initialization = Initialization::Immediate);
    // An owning runtime must remain alive until every SDK object is destroyed.
    static void ensureInitialized();
    ~SdkRuntime();
    static bool safeDeleteTransport();
    // Whether the last request this thread sent may be sent again when its connection was never
    // set up (#2051): a GET or HEAD, which only reads, or a request sent on a fresh connection that
    // is never reused (a DELETE, or one in a SingleSendScope). libcurl transparently resends a
    // request whose reused connection lost its reply, so for any other request a failed reconnect
    // doesn't show that nothing was sent. False without the Curl transport.
    static bool lastRequestResendable();
    // While one lives on a thread, every request that thread sends goes out on a fresh connection
    // that is never reused, and with redirect following off (#1690). Test upload and link's
    // synthetic PutObject and CreateMultipartUpload use it: libcurl could otherwise send such a
    // request again by itself after losing a reused connection's reply, or follow a redirect with
    // it, and leave an object or upload behind that the test's journal can't account for.
    class SingleSendScope final {
    public:
        SingleSendScope();
        ~SingleSendScope();
        SingleSendScope(const SingleSendScope&) = delete;
        SingleSendScope& operator=(const SingleSendScope&) = delete;
    private:
        bool previous_;
    };
    // AWS SDK log records go to stderr as "putgd-aws<TAB>E|W|I<TAB>tag<TAB>text"
    // lines, which the resident collects into its log (#74). The level comes
    // from PUTGD_AWS_LOG_LEVEL: "error", "info" (debug logging) or anything
    // else for warn. Debug and Trace log request headers, including
    // authorization data, so no value selects them.
    enum class LogLevel { Error, Warn, Info };
    static LogLevel logLevel(const char* value);
    SdkRuntime(const SdkRuntime&) = delete;
    SdkRuntime& operator=(const SdkRuntime&) = delete;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
