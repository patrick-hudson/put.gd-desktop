#include "S3Provider.h"
#include "S3Module.h"
#include "PublicUrl.h"
#include "IpAddress.h"
#include <condition_variable>
#include <mutex>
#include <vector>
namespace putgd {
namespace {
constexpr bool s3SafeDeleteTransport = PUTGD_SDK_CURL_TRANSPORT;
QString objectKey(const Destination& destination, const QString& candidate) {
    return s3ObjectKey(destination.settings, candidate);
}
Error accessError() { return {"credential_setup_required", "Credentials for this destination are locked or missing. Unlock saved credentials, or enter them again in Destinations if they were for this session only.", {}}; }
Error cancelled() { return {"cancelled", "Cancelled. Requests already sent to S3 may still have taken effect.", {}}; }
Error busy() { return {"provider_busy", "Too many S3 requests are already waiting. Wait for current uploads to finish and try again.", {}}; }
bool stopped(const std::shared_ptr<std::atomic_bool>& token) { return token && token->load(); }
// Callback exceptions must neither redeliver a reply nor strand queue admission.
template<class... Args> std::function<void(Args...)> once(std::function<void(Args...)> callback) {
    auto delivered = std::make_shared<std::atomic_bool>(false);
    return [callback = std::move(callback), delivered](Args... args) {
        if (!delivered->exchange(true)) callback(std::move(args)...);
    };
}
Error unexpected() {
    return {"s3_transport_unknown", "The S3 helper exited unexpectedly, so the file may already be in S3. Select Retry upload to check before sending again.", {}};
}

}
struct S3Provider::State {
    CredentialStore& credentials;
    S3UploadPolicy policy;
    // One dispatch lane: its own module worker thread and driver. Calls on
    // different lanes run in parallel; each lane stays FIFO.
    struct Lane {
        std::shared_ptr<S3Module> module;
        S3Driver* driver = nullptr; // Only this lane's module worker accesses it.
        bool retained = false;      // Guarded by State::mutex, for diagnostics.
        int queued = 0;             // Guarded by State::mutex.
    };
    const int maximumLanes;
    std::vector<std::unique_ptr<Lane>> lanes; // Guarded by mutex; lanes are added, never removed.
    std::atomic_int queuedSnapshot = 0;
    std::mutex mutex;
    std::condition_variable drained;
    int queued = 0;
    bool closing = false;
    State(CredentialStore& store, S3UploadPolicy uploads, int laneLimit) : credentials(store), policy(uploads), maximumLanes(laneLimit) {
        lanes.push_back(std::make_unique<Lane>());
        lanes.front()->module = S3Module::acquire();
    }
    ~State() {
        {
            std::unique_lock lock(mutex);
            closing = true;
            drained.wait(lock, [this] { return queued == 0; });
        }
        // Destroy clients and the last SDK owner on the same worker that made them.
        for (const auto& lane : lanes)
            if (lane->driver) lane->module->destroyDriver(lane->driver);
    }
    void complete(Lane& lane, bool releaseIdleDriver = true) {
        bool release = false;
        {
            std::lock_guard lock(mutex);
            release = releaseIdleDriver && lane.queued == 1 && lane.driver;
        }
        if (release) {
            // Keep this job counted until SDK shutdown finishes on its owner.
            // New admissions need not wait: their work is FIFO-queued behind
            // this cleanup and cannot access the driver until it returns.
            lane.module->destroyDriver(lane.driver);
            lane.driver = nullptr;
            std::lock_guard lock(mutex);
            lane.retained = false;
        }
        std::lock_guard lock(mutex);
        --lane.queued;
        --queued;
        queuedSnapshot.store(queued);
        drained.notify_all();
    }
    // An idle lane first, then a new lane up to the limit, then the shortest queue.
    Lane& select() {
        Lane* best = nullptr;
        for (const auto& lane : lanes)
            if (!best || lane->queued < best->queued) best = lane.get();
        if (best->queued > 0 && int(lanes.size()) < maximumLanes) {
            lanes.push_back(std::make_unique<Lane>());
            lanes.back()->module = std::make_shared<S3Module>();
            best = lanes.back().get();
        }
        return *best;
    }
    template<class Work, class Fail>
    void run(S3Provider& owner, const Destination& destination,
             const std::shared_ptr<std::atomic_bool>& cancellation, Work work, Fail fail) {
        Lane* lane = nullptr;
        {
            std::unique_lock lock(mutex);
            if (closing || queued >= 32) {
                lock.unlock();
                fail(busy());
                return;
            }
            try { lane = &select(); }
            catch (...) {
                lock.unlock();
                fail({"s3_module_unavailable", "Couldn't start S3 support. Try again, or reinstall put.gd if it keeps failing.", {}});
                return;
            }
            ++lane->queued;
            ++queued;
            queuedSnapshot.store(queued);
        }
        try {
            lane->module->post([this, lane, &owner, destination, cancellation, work = std::move(work), fail] {
                struct Completion { State& state; Lane& lane; ~Completion() { state.complete(lane); } } completion{*this, *lane};
                try {
                    if (auto error = owner.readiness(destination)) fail(error);
                    else if (stopped(cancellation)) fail(cancelled());
                    else {
                        Error error;
                        if (!lane->driver) lane->driver = lane->module->createDriver(
                            [this](const QString& profile, const QJsonObject& reference) {
                                return credentials.cached(profile, reference);
                            }, [&owner](const Destination& value) { return owner.readiness(value); }, error, cancellation);
                        { std::lock_guard lock(mutex); lane->retained = lane->driver != nullptr; }
                        if (lane->driver) work(*lane->driver);
                        else fail(error);
                    }
                } catch (...) {
                    // No exception text or SDK diagnostics can expose cached secrets.
                    // The once-only reply also handles an exception after delivery.
                    try { fail(unexpected()); } catch (...) {}
                }
            });
        } catch (...) {
            // A failed post completes on its caller, not the module worker.
            // Never block that caller on worker destruction while holding this
            // admission mutex: another provider's callback can re-enter us.
            // Rare allocation failures may retain an idle driver until the next
            // accepted job or provider destruction, without losing admission.
            complete(*lane, false);
            fail({"s3_module_unavailable", "Couldn't start S3 support. Try again, or reinstall put.gd if it keeps failing.", {}});
        }
    }
};
int S3Provider::maximumLanes() {
#if defined(Q_OS_LINUX)
    return 32;
#else
    return 1;
#endif
}
S3Provider::S3Provider(CredentialStore& credentials, S3UploadPolicy policy, int lanes)
    : state_(std::make_unique<State>(credentials, policy, lanes > 0 ? qMin(lanes, maximumLanes()) : maximumLanes())) {}
S3Provider::~S3Provider() = default;
int S3Provider::maximumConcurrentRequests(const Destination&) const { return state_->maximumLanes; }
QJsonObject S3Provider::diagnostics() const {
    // Resource snapshots never block the GUI during SDK shutdown or thread joins:
    // the mutex guards only counters and the lane list, never driver teardown.
    int retained = 0, live = 0, lanes = 0;
    bool started = false;
    {
        std::lock_guard lock(state_->mutex);
        lanes = int(state_->lanes.size());
        for (const auto& lane : state_->lanes) {
            retained += lane->retained ? 1 : 0;
            live += lane->module->liveDriverCount();
            started = started || lane->module->workerStarted();
        }
    }
    return {{"queuedRequests", state_->queuedSnapshot.load()}, {"retainedDrivers", retained},
            {"moduleLiveDrivers", live}, {"workerStarted", started}, {"lanes", lanes}};
}
ProviderCapabilities S3Provider::capabilities(const Destination& destination) const {
    const auto prefix = s3StoredKey(destination.settings["objectPrefix"].toString()).toUtf8().size();
    ProviderCapabilities result{true, true, s3SafeDeleteTransport, true, true, s3SafeDeleteTransport, true, int(qMax(1, 1024 - prefix - (prefix ? 1 : 0))),
            "Deletes only the uploaded object or version if it hasn't changed. Browser or CDN caches may keep serving it."};
    // Larger files upload in parts; S3 itself stores objects of at most 5 TiB.
    result.maximumObjectBytes = s3MaximumObjectBytes;
    result.settlesUploads = true;
    return result;
}
QString S3Provider::serializationKey(const Destination& destination, const QJsonObject& object, const QString& candidate) const {
    return destination.settings["endpoint"].toString() + "/" + destination.settings["bucket"].toString() + "/" +
           (object["key"].isString() ? s3StoredKey(object["key"].toString()) : objectKey(destination, candidate));
}
namespace {
// `current` holds settings to today's rules. Without it, a Region accepted before
// #1675 still loads, so Settings can show and fix it, and work on uploads made
// before then still runs.
Error settingsError(const QJsonObject& settings, bool current) {
    const QStringList fields{"bucket", "region", "endpoint", "objectPrefix", "storageClass", "aclMode", "publicUrlBase", "addressing", "connectTimeoutMs", "requestTimeoutMs", "lowSpeedBytesPerSecond", "lowSpeedTimeSeconds"};
    auto bad = [] { return Error{"s3_settings_invalid", "Invalid S3 settings. Check Bucket, Endpoint, Addressing, Upload access request, Storage class, and Advanced connection settings.", {}}; };
    auto base = settings; base.remove("linkMode"); base.remove("linkValidityDays");
    if (base.keys().size() != fields.size()) return bad();
    if (settings.contains("linkMode") && !QStringList{"public", "presigned"}.contains(settings["linkMode"].toString())) return bad();
    if (settings.contains("linkValidityDays")) {const auto days=settings["linkValidityDays"].toDouble();if(days<1||days>7||days!=int(days))return bad();}
    for (const auto& field : fields) if (!settings.contains(field)) return bad();
    for (const auto& field : {"bucket", "region", "endpoint", "objectPrefix", "storageClass", "aclMode", "addressing"})
        if (!settings[field].isString()) return bad();
    const auto bucket = settings["bucket"].toString();
    if (bucket.toUtf8().size() < 3 || bucket.toUtf8().size() > 63 || bucket.contains('/') || bucket.contains(':') || bucket.contains(' ') ||
        settings["objectPrefix"].toString().toUtf8().size() > 1022 ||
        settings["objectPrefix"].toString().contains(QChar(0))) return bad();
    // Cloudflare R2 needs no Region at all: its requests are signed with auto (S3Driver).
    const bool r2 = s3CloudflareR2Endpoint(settings["endpoint"].toString());
    if (settings["region"].toString().isEmpty() && !r2)
        return {"s3_settings_invalid", s3RegionProblem({}), {{"field", "region"}}};
    // Region first: on AWS, Settings writes Region into the generated endpoint host, so a
    // Region with spaces would otherwise read as a bad endpoint, under either set of rules.
    // Cloudflare R2 ignores Region: its requests are signed with auto (S3Driver).
    const auto regionProblem = r2 ? QString() : s3RegionProblem(settings["region"].toString(), settings["endpoint"].toString());
    const Error regionError{"s3_settings_invalid", regionProblem, {{"field", "region"}}};
    if (current && !regionProblem.isEmpty()) return regionError;
    const auto endpoint = QUrl(settings["endpoint"].toString());
    const auto address=classifyIpAddress(endpoint.host());
    if (!endpoint.isValid() || endpoint.host().isEmpty() || !endpoint.userInfo().isEmpty() || endpoint.hasQuery() || endpoint.hasFragment() ||
        (endpoint.scheme() != "https" && !(endpoint.scheme() == "http" && address.loopback))) return regionProblem.isEmpty() ? bad() : regionError;
    if (settings["publicUrlBase"].isString()) {
        const auto base = QUrl(settings["publicUrlBase"].toString());
        const auto host=classifyIpAddress(base.host());
        if (!base.isValid() || base.host().isEmpty() || !base.userInfo().isEmpty() || base.hasQuery() || base.hasFragment() ||
            (base.scheme() != "https" && !(base.scheme() == "http" && host.loopback))) return bad();
    } else if (!settings["publicUrlBase"].isNull()) return bad();
    if (!QStringList{"public-read", "omit"}.contains(settings["aclMode"].toString()) ||
        !QStringList{"path", "virtual", "auto"}.contains(settings["addressing"].toString()) ||
        !QStringList{"STANDARD", "REDUCED_REDUNDANCY", "STANDARD_IA", "ONEZONE_IA", "INTELLIGENT_TIERING", "GLACIER", "DEEP_ARCHIVE", "GLACIER_IR"}.contains(settings["storageClass"].toString())) return bad();
    for (const auto& field : {"connectTimeoutMs", "requestTimeoutMs", "lowSpeedBytesPerSecond", "lowSpeedTimeSeconds"}) {
        const auto value = settings[field].toDouble();
        if (!settings[field].isDouble() || value < 1 || value > 2147483647 || value != qint64(value)) return bad();
    }
    if (s3VirtualAddressing(settings) && (bucket.contains('.') || address.numeric)) return bad();
    // An Endpoint path is refused on every host, Cloudflare R2 included: requests and links add
    // the bucket after it, so a bucket typed there is named twice (#1676).
    if (const auto problem = s3EndpointProblem(settings["endpoint"].toString()); current && !problem.isEmpty())
        return {"s3_settings_invalid", problem, {{"field", "endpoint"}}};
    return {};
}
}
Error S3Provider::validateSettings(const QJsonObject& settings) const { return settingsError(settings, true); }
Error S3Provider::validateStoredSettings(const QJsonObject& settings) const { return settingsError(settings, false); }
Error S3Provider::readiness(const Destination& destination) const {
    if (auto error = validateStoredSettings(destination.settings)) return error;
    if (!state_->credentials.cached(destination.profileId, destination.credentialReference)) return accessError();
    return {};
}
void S3Provider::checkName(const Destination& destination, const QString& candidate, std::function<void(NameCheck)> reply, std::shared_ptr<std::atomic_bool> cancellation) {
    reply = once(std::move(reply));
    // New uploads and tests use today's rules; work on recorded uploads uses readiness.
    if (auto error = validateSettings(destination.settings)) { reply({"inconclusive", {}, {}, error}); return; }
    state_->run(*this, destination, cancellation, [destination, candidate, reply, cancellation](S3Driver& driver) {
        driver.checkName(destination, candidate, reply, cancellation);
    }, [reply](Error error) { reply({"inconclusive", {}, {}, error}); });
}
void S3Provider::stat(const StorageReceipt& expected, std::function<void(StatResult)> reply) {
    reply = once(std::move(reply));
    state_->run(*this, expected.destination, {}, [expected, reply](S3Driver& driver) {
        driver.stat(expected, reply);
    }, [reply](Error error) { reply({"unknown", {}, error}); });
}
void S3Provider::statForRemoval(const StorageReceipt& expected, std::shared_ptr<std::atomic_bool> kill, std::function<void(StatResult)> reply) {
    reply = once(std::move(reply));
    state_->run(*this, expected.destination, kill, [expected, kill, reply](S3Driver& driver) {
        driver.statForRemoval(expected, kill, reply);
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
    // A multipart upload already started (its upload id is recorded) continues at its recorded
    // target under the rules it started with, so it can finish or be aborted there (#1676); only
    // a new upload meets today's rules.
    const bool resuming = !request.resume["uploadId"].toString().isEmpty();
    if (auto error = resuming ? validateStoredSettings(request.destination.settings) : validateSettings(request.destination.settings)) { reply({"failed", {}, error}); return; }
    auto planned = request;
    // A new large upload starts as a multipart plan. An existing journal keeps
    // its own part size: its uploaded parts were cut with it.
    if (planned.resume.isEmpty())
        if (const auto part = s3PartBytes(request.artifact.bytes, state_->policy))
            planned.resume = {{"schemaVersion", 1}, {"partBytes", double(part)}};
    state_->run(*this, request.destination, request.cancellation, [request = std::move(planned), reply](S3Driver& driver) {
        driver.upload(request, reply);
    }, [reply](Error error) { reply({error.code == "s3_transport_unknown" ? "unknown" : "failed", {}, error}); });
}
void S3Provider::remove(const DeleteRequest& request, DurableCheckpoint save, std::function<void(DeleteResult)> reply) {
    reply = once(std::move(reply));
    // Delete may need to reconcile an earlier journal even after cancellation.
    // Preserve the driver state machine's checks before every destructive step.
    // A deletion queued behind other work does not start once put.gd quits (#1602).
    state_->run(*this, request.receipt.destination, request.kill, [request, save, reply](S3Driver& driver) {
        driver.remove(request, save, reply);
    }, [request, reply](Error error) {
        const bool uncertain = error.code == "s3_transport_unknown";
        // Empty progress preserves the caller's latest acknowledged checkpoint.
        reply({uncertain ? "unknown" : "failed", uncertain ? QJsonObject{} : request.progress, error});
    });
}
void S3Provider::abandonUpload(const Destination& destination, const QJsonObject& journal, std::function<void(Error)> reply) {
    reply = once(std::move(reply));
    if (journal["uploadId"].toString().isEmpty()) {
        reply({});
        return;
    }
    state_->run(*this, destination, {}, [destination, journal, reply](S3Driver& driver) {
        driver.abandon(destination, journal, reply);
    }, reply);
}
void S3Provider::testDestination(const Destination& destination, DurableCheckpoint save, std::function<void(QJsonObject, Error)> reply) {
    reply = once(std::move(reply));
    if (auto error = validateSettings(destination.settings)) { reply({}, error); return; }
    state_->run(*this, destination, {}, [destination, save, reply](S3Driver& driver) {
        driver.testDestination(destination, save, reply);
    }, [reply](Error error) { reply({}, error); });
}
}

namespace putgd {
void S3Provider::refreshLink(const StorageReceipt& receipt, std::function<void(StatResult)> reply) {refreshLinkForCopy(receipt,{},std::move(reply));}
void S3Provider::refreshLinkForCopy(const StorageReceipt& receipt, std::shared_ptr<std::atomic_bool> kill, std::function<void(StatResult)> reply) {
    if(receipt.destination.settings["linkMode"]!="presigned") {reply({"unknown",receipt,{"link_refresh_unsupported","This link does not expire",{}}});return;}
    reply=once(std::move(reply));
    state_->run(*this,receipt.destination,kill,[receipt,kill,reply](S3Driver& driver){driver.refreshLinkForCopy(receipt,kill,reply);},
        [reply](Error e){reply({"unknown",{},e});});
}
}

namespace putgd {
void S3Provider::unlockLinkCredentials(const StorageReceipt& receipt,std::function<void(Error)> reply) {
    state_->credentials.unlock(receipt.destination.profileId,receipt.destination.credentialReference,std::move(reply));
}
}
