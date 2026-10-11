#include "S3SdkContext.h"
#include <QtGlobal>
#include <mutex>
#include <stdexcept>
namespace putgd {
S3SdkContext::S3SdkContext() : owner_(std::this_thread::get_id()) {}
std::shared_ptr<S3SdkContext> S3SdkContext::acquire() {
    static std::mutex mutex;
    static std::weak_ptr<S3SdkContext> current;
    std::lock_guard lock(mutex);
    auto context = current.lock();
    if (context && context->owner_ != std::this_thread::get_id())
        throw std::logic_error("S3 module must use its permanent SDK owner thread");
    if (!context) {
        context = std::shared_ptr<S3SdkContext>(new S3SdkContext);
        current = context;
    }
    return context;
}
S3SdkContext::~S3SdkContext() {
    if (owner_ != std::this_thread::get_id())
        qFatal("S3 SDK shutdown must run on its initialization thread");
}
}
