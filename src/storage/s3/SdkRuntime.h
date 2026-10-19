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
