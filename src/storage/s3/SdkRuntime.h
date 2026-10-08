#pragma once

#include <memory>

namespace putgd {
// Construct once before providers; destroy after all requests and providers drain.
class SdkRuntime final {
public:
    SdkRuntime();
    ~SdkRuntime();
    SdkRuntime(const SdkRuntime&) = delete;
    SdkRuntime& operator=(const SdkRuntime&) = delete;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
