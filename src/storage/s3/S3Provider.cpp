#include "S3Provider.h"
#include "S3Module.h"
#include "PublicUrl.h"
#include <QHostAddress>
#include <condition_variable>
#include <mutex>
namespace putgd {
namespace {
constexpr bool s3SafeDeleteTransport = PUTGD_SDK_CURL_TRANSPORT;
QString objectKey(const Destination& destination, const QString& candidate) {
    auto prefix = destination.settings["objectPrefix"].toString();
    if (!prefix.isEmpty() && !prefix.endsWith('/')) prefix += '/';
    return prefix + candidate;
}
Error accessError() { return {"credential_setup_required", "Restore explicit credentials for this original destination", {}}; }
Error cancelled() { return {"cancelled", "Cancellation requested; sent requests may still have taken effect", {}}; }
Error busy() { return {"provider_busy", "S3 work queue is full", {}}; }
bool stopped(const std::shared_ptr<std::atomic_bool>& token) { return token && token->load(); }
// Callback exceptions must neither redeliver a reply nor strand queue admission.
template<class... Args> std::function<void(Args...)> once(std::function<void(Args...)> callback) {
    auto delivered = std::make_shared<std::atomic_bool>(false);
    return [callback = std::move(callback), delivered](Args... args) {
        if (!delivered->exchange(true)) callback(std::move(args)...);
    };
}
Error unexpected() {
    return {"s3_transport_unknown", "S3 work ended unexpectedly; reconcile its recorded target before retrying", {}};
}

}
struct S3Provider::State {
    CredentialStore& credentials;
    std::shared_ptr<S3Module> module = S3Module::acquire();
    S3Driver* driver = nullptr; // Only the permanent module worker accesses this.
    std::mutex mutex;
    std::condition_variable drained;
    int queued = 0;
    bool closing = false;
    explicit State(CredentialStore& store) : credentials(store) {}
    ~State() {
        {
            std::unique_lock lock(mutex);
            closing = true;
            drained.wait(lock, [this] { return queued == 0; });
        }
        // Destroy clients and the last SDK owner on the same worker that made them.
        if (driver) module->destroyDriver(driver);
    }
    void complete() {
        std::lock_guard lock(mutex);
        --queued;
        drained.notify_all();
    }
    template<class Work, class Fail>
    void run(S3Provider& owner, const Destination& destination,
             const std::shared_ptr<std::atomic_bool>& cancellation, Work work, Fail fail) {
        {
            std::unique_lock lock(mutex);
            if (closing || queued >= 32) {
                lock.unlock();
                fail(busy());
                return;
            }
            ++queued;
        }
        try {
            module->post([this, &owner, destination, cancellation, work = std::move(work), fail] {
                struct Completion { State& state; ~Completion() { state.complete(); } } completion{*this};
                try {
                    if (auto error = owner.readiness(destination)) fail(error);
                    else if (stopped(cancellation)) fail(cancelled());
                    else {
                        Error error;
                        if (!driver) driver = module->createDriver(
                            [this](const QString& profile, const QJsonObject& reference) {
                                return credentials.cached(profile, reference);
                            }, [&owner](const Destination& value) { return owner.readiness(value); }, error);
                        if (driver) work(*driver);
                        else fail(error);
                    }
                } catch (...) {
                    // No exception text or SDK diagnostics can expose cached secrets.
                    // The once-only reply also handles an exception after delivery.
                    try { fail(unexpected()); } catch (...) {}
                }
            });
        } catch (...) {
            complete();
            fail({"s3_module_unavailable", "S3 support could not start; retry or reinstall this build of put.gd", {}});
        }
    }
};
S3Provider::S3Provider(CredentialStore& credentials) : state_(std::make_unique<State>(credentials)) {}
S3Provider::~S3Provider() = default;
ProviderCapabilities S3Provider::capabilities(const Destination& destination) const {
    const auto prefix = destination.settings["objectPrefix"].toString().toUtf8().size();
    return {true, true, s3SafeDeleteTransport, true, true, s3SafeDeleteTransport, true, int(qMax(1, 1024 - prefix - (prefix ? 1 : 0))),
            "Guarded origin removal; selected versions only; browser/CDN caches may retain bytes"};
}
QString S3Provider::serializationKey(const Destination& destination, const QJsonObject& object, const QString& candidate) const {
    return destination.settings["endpoint"].toString() + "/" + destination.settings["bucket"].toString() + "/" +
           (object["key"].isString() ? object["key"].toString() : objectKey(destination, candidate));
}
Error S3Provider::validateSettings(const QJsonObject& settings) const {
    const QStringList fields{"bucket", "region", "endpoint", "objectPrefix", "storageClass", "aclMode", "publicUrlBase", "addressing", "connectTimeoutMs", "requestTimeoutMs", "lowSpeedBytesPerSecond", "lowSpeedTimeSeconds"};
    auto bad = [] { return Error{"s3_settings_invalid", "Invalid S3 settings; use a supported endpoint, addressing, ACL, storage class and positive timeouts", {}}; };
    if (settings.keys().size() != fields.size()) return bad();
    for (const auto& field : fields) if (!settings.contains(field)) return bad();
    for (const auto& field : {"bucket", "region", "endpoint", "objectPrefix", "storageClass", "aclMode", "addressing"})
        if (!settings[field].isString()) return bad();
    const auto bucket = settings["bucket"].toString();
    if (bucket.toUtf8().size() < 3 || bucket.toUtf8().size() > 63 || bucket.contains('/') || bucket.contains(':') || bucket.contains(' ') ||
        settings["region"].toString().isEmpty() || settings["objectPrefix"].toString().toUtf8().size() > 1022 ||
        settings["objectPrefix"].toString().contains(QChar(0))) return bad();
    const auto endpoint = QUrl(settings["endpoint"].toString());
    QHostAddress address(endpoint.host());
    if (!endpoint.isValid() || endpoint.host().isEmpty() || !endpoint.userInfo().isEmpty() || endpoint.hasQuery() || endpoint.hasFragment() ||
        (endpoint.scheme() != "https" && !(endpoint.scheme() == "http" && address.isLoopback()))) return bad();
    if (settings["publicUrlBase"].isString()) {
        const auto base = QUrl(settings["publicUrlBase"].toString());
        QHostAddress host(base.host());
        if (!base.isValid() || base.host().isEmpty() || !base.userInfo().isEmpty() || base.hasQuery() || base.hasFragment() ||
            (base.scheme() != "https" && !(base.scheme() == "http" && host.isLoopback()))) return bad();
    } else if (!settings["publicUrlBase"].isNull()) return bad();
    if (!QStringList{"public-read", "omit"}.contains(settings["aclMode"].toString()) ||
        !QStringList{"path", "virtual", "auto"}.contains(settings["addressing"].toString()) ||
        !QStringList{"STANDARD", "REDUCED_REDUNDANCY", "STANDARD_IA", "ONEZONE_IA", "INTELLIGENT_TIERING", "GLACIER", "DEEP_ARCHIVE", "GLACIER_IR"}.contains(settings["storageClass"].toString())) return bad();
    for (const auto& field : {"connectTimeoutMs", "requestTimeoutMs", "lowSpeedBytesPerSecond", "lowSpeedTimeSeconds"}) {
        const auto value = settings[field].toDouble();
        if (!settings[field].isDouble() || value < 1 || value > 2147483647 || value != qint64(value)) return bad();
    }
    if (s3VirtualAddressing(settings) && (bucket.contains('.') || !address.isNull())) return bad();
    return {};
}
Error S3Provider::readiness(const Destination& destination) const {
    if (auto error = validateSettings(destination.settings)) return error;
    if (!state_->credentials.cached(destination.profileId, destination.credentialReference)) return accessError();
    return {};
}
void S3Provider::checkName(const Destination& destination, const QString& candidate, std::function<void(NameCheck)> reply) {
    reply = once(std::move(reply));
    state_->run(*this, destination, {}, [destination, candidate, reply](S3Driver& driver) {
        driver.checkName(destination, candidate, reply);
    }, [reply](Error error) { reply({"inconclusive", {}, {}, error}); });
}
void S3Provider::stat(const StorageReceipt& expected, std::function<void(StatResult)> reply) {
    reply = once(std::move(reply));
    state_->run(*this, expected.destination, {}, [expected, reply](S3Driver& driver) {
        driver.stat(expected, reply);
    }, [reply](Error error) { reply({"unknown", {}, error}); });
}
void S3Provider::reconcile(const UploadRequest& request, std::function<void(StatResult)> reply) {
    reply = once(std::move(reply));
    state_->run(*this, request.destination, {}, [request, reply](S3Driver& driver) {
        driver.reconcile(request, reply);
    }, [reply](Error error) { reply({"unknown", {}, error}); });
}
void S3Provider::upload(const UploadRequest& request, std::function<void(UploadResult)> reply) {
    reply = once(std::move(reply));
    state_->run(*this, request.destination, request.cancellation, [request, reply](S3Driver& driver) {
        driver.upload(request, reply);
    }, [reply](Error error) { reply({error.code == "s3_transport_unknown" ? "unknown" : "failed", {}, error}); });
}
void S3Provider::remove(const DeleteRequest& request, DurableCheckpoint save, std::function<void(DeleteResult)> reply) {
    reply = once(std::move(reply));
    // Delete may need to reconcile an earlier journal even after cancellation.
    // Preserve the driver state machine's checks before every destructive step.
    state_->run(*this, request.receipt.destination, {}, [request, save, reply](S3Driver& driver) {
        driver.remove(request, save, reply);
    }, [request, reply](Error error) {
        const bool uncertain = error.code == "s3_transport_unknown";
        // Empty progress preserves the caller's latest acknowledged checkpoint.
        reply({uncertain ? "unknown" : "failed", uncertain ? QJsonObject{} : request.progress, error});
    });
}
void S3Provider::testDestination(const Destination& destination, DurableCheckpoint save, std::function<void(QJsonObject, Error)> reply) {
    reply = once(std::move(reply));
    state_->run(*this, destination, {}, [destination, save, reply](S3Driver& driver) {
        driver.testDestination(destination, save, reply);
    }, [reply](Error error) { reply({}, error); });
}
}
