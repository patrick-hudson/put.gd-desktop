#include "SdkRuntime.h"
#include <aws/core/Aws.h>

namespace putgd {
struct SdkRuntime::State { Aws::SDKOptions options; };
SdkRuntime::SdkRuntime() : state_(std::make_unique<State>()) {
    Aws::InitAPI(state_->options);
}
SdkRuntime::~SdkRuntime() { Aws::ShutdownAPI(state_->options); }
}
