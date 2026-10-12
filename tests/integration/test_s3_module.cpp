#include "storage/s3/S3Module.h"
#if defined(PUTGD_S3_BAD_FACTORY)
// Each fixture violates one compatibility field and must never reach creation.
extern "C" Q_DECL_EXPORT const putgd::S3ModuleApi* putgd_s3_module_v1() {
    static const putgd::S3ModuleApi api{
#if defined(PUTGD_S3_BAD_VERSION)
        putgd::s3ModuleContractVersion + 1,
#else
        putgd::s3ModuleContractVersion,
#endif
#if defined(PUTGD_S3_BAD_BUILD)
        "incompatible",
#else
        PUTGD_S3_BUILD_ID,
#endif
#if defined(PUTGD_S3_BAD_TRANSPORT)
        !bool(PUTGD_SDK_CURL_TRANSPORT),
#else
        bool(PUTGD_SDK_CURL_TRANSPORT),
#endif
        +[](putgd::CredentialLookup, putgd::ReadinessCheck) -> putgd::S3Driver* {
            qFatal("An incompatible module reached driver creation");
        }, +[](putgd::S3Driver*) {
            qFatal("An incompatible module reached driver destruction");
        }};
    return &api;
}
#else
#include "fixtures/S3Fixture.h"
#include "core/HistoryStore.h"
#include "core/LocalArchive.h"
#include "core/SettingsService.h"
#include <QFile>
#include <future>
#include <mutex>
#include <thread>
#include <stdexcept>
using namespace putgd;
namespace {
bool sdkMapped() {
#ifdef Q_OS_LINUX
    QFile maps("/proc/self/maps");
    if (!maps.open(QIODevice::ReadOnly)) qFatal("Cannot inspect module loading contract");
    const auto contents = maps.readAll();
    return contents.contains("libaws") || contents.contains("libs2n");
#else
    return false;
#endif
}
Error createFailure(const QString& path) {
    S3Module module(path);
    auto done = std::make_shared<std::promise<Error>>();
    auto result = done->get_future();
    module.post([&module, done] {
        Error error;
        auto driver = module.createDriver([](auto, auto) { return std::optional<Credentials>{}; },
                                           [](auto) { return Error{}; }, error);
        if (driver) qFatal("An unavailable module created a driver");
        done->set_value(error);
    });
    return result.get();
}
}
class S3ModuleContract final : public QObject {
    Q_OBJECT
private slots:
    void residentPathsDoNotLoadSdk() {
        QVERIFY(!sdkMapped());
        CredentialStore credentials;
        S3Provider provider(credentials);
        const auto profile = Settings::developmentTemplate()["profiles"].toArray()[0].toObject();
        Destination destination{"s3", "resident-profile", "revision", profile["settings"].toObject(), {{"mode", "session"}, {"reference", QJsonValue::Null}}};
        QVERIFY(!provider.validateSettings(destination.settings));
        QVERIFY(provider.capabilities(destination).stat);
        QVERIFY(!provider.serializationKey(destination, {}, "synthetic.txt").isEmpty());
        QCOMPARE(provider.readiness(destination).code, QString("credential_setup_required"));
        credentials.setSession(destination.profileId, {"PUTGD_SYNTHETIC_ACCESS", "synthetic-module-secret", {}});
        QVERIFY(!provider.readiness(destination));
        auto cancelled = std::make_shared<std::atomic_bool>(true);
        UploadRequest request; request.destination = destination; request.cancellation = cancelled;
        std::atomic_bool replied = false;
        Error error;
        provider.upload(request, [&](UploadResult result) { error = result.error; replied.store(true); });
        QTRY_VERIFY(replied.load());
        QCOMPARE(error.code, QString("cancelled"));
        QTemporaryDir local;
        ProviderRegistry providers;
        providers.add("s3", std::make_shared<S3Provider>(credentials));
        SettingsService settings(local.filePath("config.json"), [&](const auto& id, const auto& value) {
            return providers.validate(id, value);
        });
        QVERIFY(settings.save(Settings::developmentTemplate(), {}));
        ArtifactStore artifacts(local.filePath("spool"));
        ClipboardPayload payload; payload.kind = "text"; payload.bytes = "synthetic local-only bytes";
        auto acquired = artifacts.acquire("local-operation", payload, ResourceLimits{});
        QVERIFY(!acquired.error);
        ArchiveSettings archive; archive.root = local.filePath("archive");
        QCOMPARE(LocalArchive::save(acquired.artifacts[0], archive).status, QString("saved"));
        HistoryStore history(local.filePath("history.sqlite"));
        QVERIFY(!history.error());
        Operation operation; operation.id = "local-operation"; operation.created = "2026-10-08T00:00:00.000Z";
        operation.settings = Settings::fromValidated(Settings::defaults()); operation.destination = destination;
        OperationItem item; item.artifact = acquired.artifacts[0]; item.uploadStatus = "uploaded";
        item.receipt.destination = destination; item.receipt.publicationId = "synthetic-publication";
        item.receipt.objectReference = {{"schemaVersion", 1}, {"key", "synthetic.txt"}};
        item.receipt.directUrl = "https://example.invalid/synthetic.txt";
        operation.items.append(item);
        QVERIFY(!history.saveOperation(operation, &providers));
        QVERIFY(!history.indexPublications(providers));
        QCOMPARE(history.list({}).total, 1);
        QCOMPARE(history.detail(item.artifact.id)["receipt"].toObject()["directUrl"].toString(), item.receipt.directUrl);
        QVERIFY(!sdkMapped());
    }
    void missingAndIncompatibleModule() {
        QTemporaryDir directory;
        QCOMPARE(createFailure(directory.filePath("missing-module.so")).code, QString("s3_module_unavailable"));
        QCOMPARE(createFailure(QString::fromUtf8(S3_INCOMPATIBLE_PATH)).code, QString("s3_module_unavailable"));
        QVERIFY(!sdkMapped());
    }
    void deployedFailure_data() {
        QTest::addColumn<QString>("modulePath");
        QTest::newRow("missing") << QString{};
        QTest::newRow("wrong-version") << QString::fromUtf8(S3_INCOMPATIBLE_PATH);
        QTest::newRow("wrong-build") << QString::fromUtf8(S3_WRONG_BUILD_PATH);
        QTest::newRow("wrong-transport") << QString::fromUtf8(S3_WRONG_TRANSPORT_PATH);
    }
    void deployedFailure() {
        QFETCH(QString, modulePath);
        QTemporaryDir directory;
        const auto bin = directory.filePath("bin");
        QVERIFY(QDir().mkpath(bin));
        const auto executable = QDir(bin).filePath("module-contract");
        QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), executable));
        QVERIFY(QFile::setPermissions(executable, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
#ifdef Q_OS_LINUX
        const auto helper = QDir(bin).filePath("putgd-s3-worker");
        QVERIFY(QFile::copy(QDir(QCoreApplication::applicationDirPath()).filePath("putgd-s3-worker"), helper));
        QVERIFY(QFile::setPermissions(helper, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
#endif
        if (!modulePath.isEmpty()) {
            const auto module = QDir(bin).absoluteFilePath(QString::fromUtf8(S3_MODULE_INSTALL_RELATIVE));
            QVERIFY(QDir().mkpath(QFileInfo(module).absolutePath()));
            QVERIFY(QFile::copy(modulePath, module));
        }
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("PUTGD_TEST_DEPLOYMENT_CHILD", "1");
        child.setProcessEnvironment(environment);
        child.start(executable, {"unavailableFacadeChild"});
        QVERIFY(child.waitForStarted());
        QVERIFY(child.waitForFinished(15000));
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QVERIFY2(child.exitCode() == 0, child.readAllStandardOutput().constData());
    }
    void unavailableFacadeChild() {
        if (qEnvironmentVariable("PUTGD_TEST_DEPLOYMENT_CHILD") != "1") QSKIP("Subprocess helper");
        CredentialStore credentials;
        S3Provider provider(credentials);
        const auto profile = Settings::developmentTemplate()["profiles"].toArray()[0].toObject();
        Destination destination{"s3", "missing-profile", "revision", profile["settings"].toObject(), {{"mode", "session"}, {"reference", QJsonValue::Null}}};
        destination.settings["endpoint"] = "http://127.0.0.1:1";
        credentials.setSession(destination.profileId, {"PUTGD_SYNTHETIC_ACCESS", "synthetic-module-secret", {}});
        QVERIFY(!provider.validateSettings(destination.settings));
        QVERIFY(provider.capabilities(destination).stat);
        std::atomic_int callbacks = 0;
        std::atomic_int failures = 0;
        for (int i = 0; i < 2; ++i) provider.checkName(destination, "synthetic.txt", [&](NameCheck value) {
            if (value.status != "inconclusive" || value.error.code != "s3_module_unavailable") ++failures;
            if (!value.error.message.contains("restart put.gd") || !value.error.message.contains("reinstall")) ++failures;
            ++callbacks;
        });
        QTRY_COMPARE(callbacks.load(), 2);
        QCOMPARE(failures.load(), 0);
        QVERIFY(!sdkMapped());
    }
    void idleRequestsReleaseDriver() {
        test::S3Fixture fixture;
        auto module = S3Module::acquire();
        QCOMPARE(module->liveDriverCount(), 0);
        const auto before = fixture.provider.diagnostics();
        QCOMPARE(before["queuedRequests"].toInt(), 0);
        QCOMPARE(before["retainedDrivers"].toInt(), 0);
        std::thread::id worker;
        std::atomic_int lifecycleFailures = 0;
        for (int generation = 0; generation < 3; ++generation) {
            auto result = fixture.await<NameCheck>([&](auto reply) {
                fixture.provider.checkName(fixture.destination, "idle-generation.txt", [&, reply](NameCheck value) {
                    // The real network driver's callback is still inside its owned job.
                    if (module->liveDriverCount() != 1) ++lifecycleFailures;
                    const auto active = fixture.provider.diagnostics();
                    if (active["queuedRequests"].toInt() != 1 || active["retainedDrivers"].toInt() != 1) ++lifecycleFailures;
                    if (worker == std::thread::id{}) worker = std::this_thread::get_id();
                    else if (std::this_thread::get_id() != worker) ++lifecycleFailures;
                    reply(std::move(value));
                });
            });
            QCOMPARE(result.status, QString("absent"));
            QVERIFY(!result.error);
            QCOMPARE(lifecycleFailures.load(), 0);
            // FIFO barrier runs after callback return and completion, avoiding a
            // callback-versus-destructor race in the idle assertions.
            auto drained = std::make_shared<std::promise<void>>();
            auto completed = drained->get_future();
            module->post([drained] { drained->set_value(); });
            QVERIFY(completed.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
            const auto idle = fixture.provider.diagnostics();
            QCOMPARE(idle["queuedRequests"].toInt(), 0);
            QCOMPARE(idle["retainedDrivers"].toInt(), 0);
            QCOMPARE(module->liveDriverCount(), 0);
            QVERIFY(idle["workerStarted"].toBool());
        }
        QCOMPARE(fixture.events("HEAD").size(), 3);
#ifdef Q_OS_LINUX
        QVERIFY(!sdkMapped()); // Linux SDK mappings belong only to the exited child.
#endif
    }
    void callbackAdmissionKeepsDriverUntilSuccessorDrains() {
        test::S3Fixture fixture;
        auto module = S3Module::acquire();
        QCOMPARE(module->liveDriverCount(), 0);
        struct Observations {
            std::atomic_int callbacks = 0;
            std::atomic_int failures = 0;
            std::atomic_int betweenJobs = 0;
            std::thread::id worker;
        };
        auto observed = std::make_shared<Observations>();
        auto result = fixture.await<NameCheck>([&](auto reply) {
            fixture.provider.checkName(fixture.destination, "reentrant-first.txt",
                // Accepted jobs keep the provider/module alive until they drain.
                // Borrow here so a timeout cannot leave the worker as last owner.
                [module = module.get(), provider = &fixture.provider, destination = fixture.destination, observed, reply](NameCheck first) {
                    ++observed->callbacks;
                    if (first.error || first.status != "absent") ++observed->failures;
                    observed->worker = std::this_thread::get_id();
                    // This observer is before the successor in the FIFO, but runs
                    // after the first job's completion. It catches destroying and
                    // recreating a driver between accepted jobs, not only leaks.
                    module->post([module, provider, observed] {
                        const auto pending = provider->diagnostics();
                        if (pending["queuedRequests"].toInt() != 1 ||
                            pending["retainedDrivers"].toInt() != 1 || module->liveDriverCount() != 1 ||
                            std::this_thread::get_id() != observed->worker) ++observed->failures;
                        ++observed->betweenJobs;
                    });
                    provider->checkName(destination, "reentrant-second.txt",
                        [module, provider, observed, reply](NameCheck second) {
                            ++observed->callbacks;
                            const auto active = provider->diagnostics();
                            if (active["queuedRequests"].toInt() != 1 ||
                                active["retainedDrivers"].toInt() != 1 || module->liveDriverCount() != 1 ||
                                std::this_thread::get_id() != observed->worker) ++observed->failures;
                            reply(std::move(second));
                        });
                    if (provider->diagnostics()["queuedRequests"].toInt() != 2) ++observed->failures;
                });
        });
        QCOMPARE(result.status, QString("absent"));
        QVERIFY(!result.error);
        auto drained = std::make_shared<std::promise<void>>();
        auto completed = drained->get_future();
        module->post([drained] { drained->set_value(); });
        QVERIFY(completed.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        QCOMPARE(observed->callbacks.load(), 2);
        QCOMPARE(observed->betweenJobs.load(), 1);
        QCOMPARE(observed->failures.load(), 0);
        QVERIFY(observed->worker != std::this_thread::get_id());
        const auto idle = fixture.provider.diagnostics();
        QCOMPARE(idle["queuedRequests"].toInt(), 0);
        QCOMPARE(idle["retainedDrivers"].toInt(), 0);
        QCOMPARE(module->liveDriverCount(), 0);
        QCOMPARE(fixture.events("HEAD").size(), 2);
    }
    void concurrentProvidersAndPermanentWorker() {
        test::S3Fixture fixture;
        auto other = std::make_unique<S3Provider>(fixture.credentials);
        std::atomic_int callbacks = 0;
        std::atomic_int failures = 0;
        std::mutex mutex;
        std::thread::id worker;
        auto reply = [&](NameCheck result) {
            if (result.error || result.status != "absent") ++failures;
            {
                std::lock_guard lock(mutex);
                if (worker == std::thread::id{}) worker = std::this_thread::get_id();
                else if (worker != std::this_thread::get_id()) ++failures;
            }
            ++callbacks;
        };
        std::thread first([&] { for (int i = 0; i < 8; ++i) fixture.provider.checkName(fixture.destination, "first.txt", reply); });
        std::thread second([&] { for (int i = 0; i < 8; ++i) other->checkName(fixture.destination, "second.txt", reply); });
        first.join(); second.join();
        QTRY_COMPARE(callbacks.load(), 16);
        QCOMPARE(failures.load(), 0);
        QVERIFY(worker != std::this_thread::get_id());
#ifdef Q_OS_LINUX
        QVERIFY(!sdkMapped());
#endif
        // QThreadPool's old default idle expiry is 30 seconds. SDK ownership must
        // still use the same permanent worker after this interval.
        QTest::qWait(31000);
        other->checkName(fixture.destination, "after-idle.txt", reply);
        other.reset(); // Drain its accepted callback and clients before returning.
        QCOMPARE(callbacks.load(), 17);
        QCOMPARE(failures.load(), 0);
        fixture.provider.checkName(fixture.destination, "remaining-provider.txt", reply);
        QTRY_COMPARE(callbacks.load(), 18);
        QCOMPARE(failures.load(), 0);
    }
    void boundedQueueDrainsExactlyOnce() {
        test::S3Fixture fixture;
        auto provider = std::make_unique<S3Provider>(fixture.credentials);
        auto module = S3Module::acquire();
        std::promise<void> blocked;
        auto started = blocked.get_future();
        auto release = std::make_shared<std::promise<void>>();
        auto unblock = release->get_future().share();
        module->post([&blocked, unblock] { blocked.set_value(); unblock.wait(); });
        started.get();
        std::atomic_int callbacks = 0;
        std::atomic_int busy = 0;
        std::atomic_int errors = 0;
        for (int i = 0; i < 64; ++i) provider->checkName(fixture.destination, "bounded.txt", [&](NameCheck value) {
            if (value.error.code == "provider_busy") ++busy;
            else if (value.error || value.status != "absent") ++errors;
            ++callbacks;
        });
        QCOMPARE(busy.load(), 32);
        QCOMPARE(callbacks.load(), 32);
        release->set_value();
        provider.reset();
        QCOMPARE(callbacks.load(), 64);
        QCOMPARE(errors.load(), 0);
    }
    void exceptionsReleaseAdmissionAndPreserveUncertainty() {
        test::S3Fixture fixture;
        auto provider = std::make_unique<S3Provider>(fixture.credentials);
        std::atomic_int callbacks = 0;
        std::atomic_int failures = 0;
        provider->checkName(fixture.destination, "throwing-callback.txt", [&](NameCheck value) {
            if (value.error) ++failures;
            ++callbacks;
            throw std::runtime_error("synthetic callback exception");
        });
        provider->checkName(fixture.destination, "after-exception.txt", [&](NameCheck value) {
            if (value.error) ++failures;
            ++callbacks;
        });
        provider.reset();
        QCOMPARE(callbacks.load(), 2);
        QCOMPARE(failures.load(), 0);
        auto uploaded = fixture.upload(fixture.request("checkpoint-exception.txt"));
        QCOMPARE(uploaded.status, QString("uploaded"));
        auto removed = fixture.remove(uploaded.receipt, {}, [](auto, auto) {
            throw std::runtime_error("synthetic checkpoint exception");
        });
        QCOMPARE(removed.status, QString("unknown"));
        QCOMPARE(removed.error.code, QString("s3_transport_unknown"));
        QVERIFY(removed.progress.isEmpty());
        QCOMPARE(fixture.events("DELETE").size(), 0);
        auto acknowledgedThenThrown = fixture.remove(uploaded.receipt, {}, [](auto, auto ack) {
            ack({});
            throw std::runtime_error("synthetic exception after checkpoint acknowledgement");
        });
        QCOMPARE(acknowledgedThenThrown.status, QString("unknown"));
        QCOMPARE(acknowledgedThenThrown.error.code, QString("s3_transport_unknown"));
        QVERIFY(acknowledgedThenThrown.progress.isEmpty());
        QCOMPARE(fixture.events("DELETE").size(), 0);
    }
    void newRuntimeGenerationAfterDrain() {
        // Earlier providers and drivers are gone. A new generation initializes
        // and shuts down on its own permanent worker with the module still pinned.
        test::S3Fixture fixture;
        auto checked = fixture.await<NameCheck>([&](auto callback) {
            fixture.provider.checkName(fixture.destination, "new-generation.txt", callback);
        });
        QCOMPARE(checked.status, QString("absent"));
        QVERIFY(!checked.error);
    }
};
QTEST_GUILESS_MAIN(S3ModuleContract)
#include "test_s3_module.moc"
#endif
