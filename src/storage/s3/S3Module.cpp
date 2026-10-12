#include "S3Module.h"
#if defined(PUTGD_S3_MODULE_EXPORTS)
extern "C" Q_DECL_EXPORT const putgd::S3ModuleApi* putgd_s3_module_v1() {
    static const putgd::S3ModuleApi api{
        putgd::s3ModuleContractVersion, PUTGD_S3_BUILD_ID, bool(PUTGD_SDK_CURL_TRANSPORT),
        +[](putgd::CredentialLookup credentials, putgd::ReadinessCheck readiness) -> putgd::S3Driver* {
            return new putgd::NetworkS3Driver(std::move(credentials), std::move(readiness));
        }, +[](putgd::S3Driver* driver) { delete driver; }};
    return &api;
}
#else
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
namespace putgd {
namespace {
Error unavailable() {
    return {"s3_module_unavailable", "S3 support is missing or incompatible; restart put.gd after an update. If it remains unavailable, reinstall this build of put.gd", {}};
}
}
struct S3Module::State {
    QString path;
    QLibrary library;
    const S3ModuleApi* api = nullptr;
    bool attempted = false;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::function<void()>> queue;
    std::thread worker;
    bool stopping = false;
    std::atomic_int liveDrivers = 0;
    std::atomic_bool workerWasStarted = false;
    explicit State(QString location) : path(std::move(location)), library(path) {
        // Retain code and SDK DSOs after first use. No idle unload or static SDK owner.
        library.setLoadHints(QLibrary::ResolveAllSymbolsHint | QLibrary::PreventUnloadHint);
    }
    void loop() {
        for (;;) {
            std::function<void()> work;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [this] { return stopping || !queue.empty(); });
                if (queue.empty()) return;
                work = std::move(queue.front());
                queue.pop_front();
            }
            work();
        }
    }
    bool load() {
        if (attempted) return api;
        attempted = true;
        if (!QFileInfo(path).isAbsolute() || !library.load()) return false;
        const auto entry = reinterpret_cast<const S3ModuleApi* (*)()>(library.resolve("putgd_s3_module_v1"));
        if (!entry) return false;
        const auto candidate = entry();
        if (!candidate || candidate->version != s3ModuleContractVersion || !candidate->buildIdentity ||
            QByteArray(candidate->buildIdentity) != PUTGD_S3_BUILD_ID ||
            candidate->safeDeleteTransport != bool(PUTGD_SDK_CURL_TRANSPORT) ||
            !candidate->create || !candidate->destroy) return false;
        api = candidate;
        return true;
    }
};
QString S3Module::defaultPath() {
    const auto executableDirectory = QDir::cleanPath(QCoreApplication::applicationDirPath());
    // Build-tree lookup applies only to executables in this exact build directory.
    if (executableDirectory == QString::fromUtf8(PUTGD_S3_BUILD_APP_DIR))
        return QString::fromUtf8(PUTGD_S3_BUILD_MODULE_PATH);
    return QDir(executableDirectory).absoluteFilePath(QString::fromUtf8(PUTGD_S3_INSTALL_MODULE_RELATIVE));
}
std::shared_ptr<S3Module> S3Module::acquire() {
    static std::mutex mutex;
    static std::weak_ptr<S3Module> current;
    std::lock_guard lock(mutex);
    auto owner = current.lock();
    if (!owner) {
        owner = std::make_shared<S3Module>();
        current = owner;
    }
    return owner;
}
S3Module::S3Module(QString path) : state_(std::make_unique<State>(std::move(path))) {}
S3Module::~S3Module() {
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
        state_->changed.notify_one();
    }
    if (state_->worker.joinable()) state_->worker.join();
}
void S3Module::post(std::function<void()> work) {
    std::lock_guard lock(state_->mutex);
    if (!state_->worker.joinable()) {
        state_->worker = std::thread([this] { state_->loop(); });
        state_->workerWasStarted.store(true);
    }
    state_->queue.push_back(std::move(work));
    state_->changed.notify_one();
}
S3Driver* S3Module::createDriver(CredentialLookup credentials, ReadinessCheck readiness, Error& error) {
    if (std::this_thread::get_id() != state_->worker.get_id()) {
        error = unavailable();
        return nullptr;
    }
    if (state_->load()) {
        try {
            if (auto driver = state_->api->create(std::move(credentials), std::move(readiness))) {
                ++state_->liveDrivers;
                return driver;
            }
        } catch (...) {
            // Never expose SDK diagnostics or cached credential material.
        }
    }
    error = unavailable();
    return nullptr;
}
void S3Module::destroyDriver(S3Driver* driver) {
    auto destroy = [this, driver] {
        state_->api->destroy(driver);
        --state_->liveDrivers;
    };
    if (std::this_thread::get_id() == state_->worker.get_id()) {
        destroy();
        return;
    }
    auto done = std::make_shared<std::promise<void>>();
    auto finished = done->get_future();
    post([destroy = std::move(destroy), done] {
        try {
            destroy();
            done->set_value();
        } catch (...) {
            done->set_exception(std::current_exception());
        }
    });
    finished.get();
}
int S3Module::liveDriverCount() const { return state_->liveDrivers.load(); }
bool S3Module::workerStarted() const { return state_->workerWasStarted.load(); }
}
#endif
