#include "storage/s3/S3Module.h"
#include "storage/s3/S3ProcessDriver.h"
#include <QCryptographicHash>
#include "storage/s3/S3Provider.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#ifdef Q_OS_UNIX
#include <cerrno>
#include <csignal>
#endif

using namespace putgd;
namespace {
QJsonObject state() {
    QFile file(qEnvironmentVariable("PUTGD_S3_FAULT_STATE"));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4096) return {};
    return QJsonDocument::fromJson(file.read(4096)).object();
}
bool processExists(qint64 pid) {
#ifdef Q_OS_UNIX
    if (pid <= 0) return true;
    errno = 0;
    return ::kill(pid_t(pid), 0) == 0 || errno != ESRCH;
#else
    Q_UNUSED(pid);
    return true;
#endif
}
Destination destination() {
    const auto profile = Settings::developmentTemplate()["profiles"].toArray()[0].toObject();
    Destination result{"s3", "fault-fixture", "fault-fixture-revision", profile["settings"].toObject(),
                       {{"mode", "session"}, {"reference", QJsonValue::Null}}};
    result.settings["endpoint"] = "http://127.0.0.1:1";
    result.settings["bucket"] = "isolated-fixture";
    result.settings["objectPrefix"] = "";
    result.settings["aclMode"] = "omit";
    result.settings["addressing"] = "path";
    result.settings["publicUrlBase"] = QJsonValue::Null;
    return result;
}
}
class S3WorkerTest final : public QObject {
    Q_OBJECT
private slots:
    void faultyChildren_data() {
        QTest::addColumn<QString>("mode");
        for (const auto* mode : {"normal", "wrong_build", "duplicate_ready", "wrong_nonce", "normal_missing_terminal",
                                 "terminal_then_crash", "oversized", "truncated", "ready_then_crash", "activated_wrong_nonce", "ready_ignores_activate", "activated_silent", "cancel_ignores_alive", "prepared_cleanup"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }
    void faultyChildren() {
#ifndef Q_OS_LINUX
        QSKIP("The production one-shot S3 worker is Linux-only");
#else
        QFETCH(QString, mode);
        // Relocation selects the production deployed sibling lookup without any
        // test-only helper override in S3ProcessDriver.
        QTemporaryDir directory(QDir::tempPath() + "/p-s3-XXXXXX");
        QVERIFY(directory.isValid());
        QVERIFY(QFile::setPermissions(directory.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QDir root(directory.path());
        QVERIFY(root.mkdir("bin"));
        const auto bin = root.filePath("bin");
        QVERIFY(QFile::setPermissions(bin, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto executable = QDir(bin).filePath("test_s3_worker");
        const auto helper = QDir(bin).filePath("putgd-s3-worker");
        QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), executable));
        QVERIFY(QFile::copy(QString::fromUtf8(S3_WORKER_FIXTURE_PATH), helper));
        const auto executablePermissions = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
        QVERIFY(QFile::setPermissions(executable, executablePermissions));
        QVERIFY(QFile::setPermissions(helper, executablePermissions));
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("PUTGD_S3_FAULT_CHILD", "1");
        environment.insert("PUTGD_S3_FAULT", mode);
        environment.insert("PUTGD_S3_FAULT_STATE", root.filePath("worker-state.json"));
        child.setProcessEnvironment(environment);
        child.setProcessChannelMode(QProcess::SeparateChannels);
        child.start(executable, {mode == "prepared_cleanup" ? "isolatedPreparedCleanup" : mode == "cancel_ignores_alive" ? "isolatedCancelledWorker" : "isolatedFaultCase"});
        QVERIFY(child.waitForStarted(5000));
        if (!child.waitForFinished(25000)) {
            child.kill();
            child.waitForFinished(5000);
            QFAIL("Isolated faulty-child test exceeded its deadline");
        }
        const auto output = child.readAllStandardOutput().left(8192) + child.readAllStandardError().left(4096);
        QVERIFY2(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0, output.constData());
#endif
    }
    void isolatedFaultCase() {
        if (qEnvironmentVariable("PUTGD_S3_FAULT_CHILD") != "1") QSKIP("Runs only in the relocated isolated child");
        const auto mode = qEnvironmentVariable("PUTGD_S3_FAULT");
        CredentialStore credentials;
        const auto target = destination();
        credentials.setSession(target.profileId, {"PUTGD_SYNTHETIC_ACCESS", "synthetic-only-not-a-real-key", {}});
        std::atomic_int callbacks{0};
        std::atomic_bool aliveAtCallback{true};
        std::atomic_bool acknowledgedAtCallback{false};
        auto delivered = std::make_shared<std::promise<NameCheck>>();
        auto result = delivered->get_future();
        {
            S3Provider provider(credentials);
            provider.checkName(target, "synthetic.txt", [&](NameCheck value) {
                const auto snapshot = state();
                aliveAtCallback.store(processExists(snapshot["pid"].toString().toLongLong()));
                acknowledgedAtCallback.store(snapshot["resultAck"].toBool());
                if (callbacks.fetch_add(1) == 0) delivered->set_value(std::move(value));
            });
            QTRY_VERIFY_WITH_TIMEOUT(result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 20000);
            const auto checked = result.get();
            QCOMPARE(callbacks.load(), 1);
            QVERIFY(!aliveAtCallback.load());
            if (mode == "normal") {
                QCOMPARE(checked.status, QString("absent"));
                QVERIFY(!checked.error);
                QCOMPARE(checked.objectReference, (QJsonObject{{"schemaVersion", 1}, {"key", "synthetic.txt"}}));
                QVERIFY(checked.concurrencyToken.isEmpty());
                QVERIFY(acknowledgedAtCallback.load());
            } else {
                QCOMPARE(checked.status, QString("inconclusive"));
                const bool preparation = mode == "wrong_build" || mode == "duplicate_ready" || mode == "wrong_nonce";
                QCOMPARE(checked.error.code, preparation ? QString("s3_module_unavailable") : QString("s3_transport_unknown"));
                QVERIFY(checked.objectReference.isEmpty());
                QVERIFY(checked.concurrencyToken.isEmpty());
                if (mode == "terminal_then_crash") QVERIFY(acknowledgedAtCallback.load());
            }
            QTRY_COMPARE_WITH_TIMEOUT(provider.diagnostics()["queuedRequests"].toInt(), 0, 5000);
            QCOMPARE(provider.diagnostics()["retainedDrivers"].toInt(), 0);
            QTest::qWait(100);
            QCOMPARE(callbacks.load(), 1);
        }
        const auto snapshot = state();
        QVERIFY(!processExists(snapshot["pid"].toString().toLongLong()));
        QCOMPARE(callbacks.load(), 1);
    }
    void isolatedCancelledWorker() {
        if (qEnvironmentVariable("PUTGD_S3_FAULT_CHILD") != "1") QSKIP("Runs only in the relocated isolated child");
        Error error;
        auto driver = std::unique_ptr<S3Driver>(createS3ProcessDriver("/missing-test-module", [](auto, auto) -> std::optional<Credentials> {
            return Credentials{"PUTGD_SYNTHETIC_ACCESS", "synthetic-only", {}};
        }, [](auto) { return Error{}; }, error, {200}));
        QVERIFY(driver && !error);
        const auto target = destination();
        const QByteArray bytes("synthetic cancellation fixture");
        QTemporaryDir directory;
        QFile source(directory.filePath("input.txt"));
        QVERIFY(source.open(QIODevice::WriteOnly));
        QCOMPARE(source.write(bytes), bytes.size());
        source.close();
        Artifact artifact; artifact.id = "cancel-item"; artifact.path = source.fileName(); artifact.name = "input.txt";
        artifact.mime = "text/plain"; artifact.bytes = bytes.size();
        artifact.sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        auto token = std::make_shared<std::atomic_bool>(false);
        UploadRequest request{target, artifact, artifact.name, "cancel-publication", "absent", {{"schemaVersion", 1}, {"key", artifact.name}}, {}, token};
        std::thread cancel([token] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); token->store(true); });
        int callbacks = 0; UploadResult result;
        driver->upload(request, [&](UploadResult value) { ++callbacks; result = std::move(value); });
        cancel.join();
        QCOMPARE(callbacks, 1);
        QCOMPARE(result.status, QString("unknown"));
        QCOMPARE(result.error.code, QString("s3_transport_unknown"));
        const auto snapshot = state();
        QVERIFY(snapshot["shutdown"].toBool());
        QVERIFY(snapshot["activated"].toBool());
        QVERIFY(!processExists(snapshot["pid"].toString().toLongLong()));
    }
    void isolatedPreparedCleanup() {
        if (qEnvironmentVariable("PUTGD_S3_FAULT_CHILD") != "1") QSKIP("Runs only in the relocated isolated child");
        auto module = S3Module::acquire();
        auto prepared = std::make_shared<std::promise<bool>>();
        auto completed = prepared->get_future();
        auto* owner = module.get();
        module->post([owner, prepared] {
            Error error;
            auto* driver = owner->createDriver(
                [](const QString&, const QJsonObject&) -> std::optional<Credentials> { return {}; },
                [](const Destination&) { return Error{}; }, error);
            const bool valid = driver && !error;
            if (driver) owner->destroyDriver(driver);
            prepared->set_value(valid);
        });
        QTRY_VERIFY_WITH_TIMEOUT(completed.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 20000);
        QVERIFY(completed.get());
        QCOMPARE(module->liveDriverCount(), 0);
        const auto snapshot = state();
        QVERIFY(snapshot["shutdown"].toBool());
        QVERIFY(!snapshot["activated"].toBool());
        QVERIFY(!processExists(snapshot["pid"].toString().toLongLong()));
        module.reset();
    }
};
QTEST_GUILESS_MAIN(S3WorkerTest)
#include "test_s3_worker.moc"
