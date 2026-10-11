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
    SdkRuntime(const SdkRuntime&) = delete;
    SdkRuntime& operator=(const SdkRuntime&) = delete;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
