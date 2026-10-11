#pragma once
#include "SdkRuntime.h"
#include <memory>
#include <thread>
namespace putgd {
// Live SDK state is shared by drivers, never owned by a static object's destructor.
// The facade's one permanent worker creates and releases every context generation.
class S3SdkContext final {
public:
    static std::shared_ptr<S3SdkContext> acquire();
    ~S3SdkContext();
private:
    S3SdkContext();
    const std::thread::id owner_;
    SdkRuntime runtime_;
};
}
