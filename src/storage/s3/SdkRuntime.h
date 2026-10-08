#pragma once

#include <memory>

namespace putgd {
// Construct once before providers; destroy after all requests and providers drain.
// Deferred mode avoids SDK setup until a provider starts real S3 work.
class SdkRuntime final {
public:
    enum class Initialization { Immediate, Deferred };
    explicit SdkRuntime(Initialization initialization = Initialization::Immediate);
    // An owning runtime must remain alive until every SDK object is destroyed.
    static void ensureInitialized();
    ~SdkRuntime();
    static bool safeDeleteTransport();
    SdkRuntime(const SdkRuntime&) = delete;
    SdkRuntime& operator=(const SdkRuntime&) = delete;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
