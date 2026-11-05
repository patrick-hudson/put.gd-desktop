#include "storage/s3/S3Module.h"
#include "storage/s3/S3WorkerProtocol.h"
#include "app/UiControl.h"
#include "core/BuildInfo.h"
#include <QUuid>
#include "fixtures/S3Fixture.h"
#include "storage/s3/S3ProcessDriver.h"
#include <QCryptographicHash>
#include "storage/s3/S3Provider.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
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
#include <stdexcept>
#include <vector>
#ifdef Q_OS_UNIX
#include <cerrno>
#include <csignal>
#endif
#include "fixtures/Waits.h"

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
    void realWorkerCoalescesProgressUntilAcknowledged() {
        test::S3Fixture fixture;
        auto request = fixture.request("backpressure.txt", QByteArray(1024 * 1024, 'p'));
        request.destination.settings["requestTimeoutMs"] = 10000;
        request.destination.settings["lowSpeedTimeSeconds"] = 10;
        fixture.reset({{"rules", QJsonArray{QJsonObject{{"method", "PUT"}, {"barrier", "progress"}}}}});
        QProcess child;child.setProcessChannelMode(QProcess::SeparateChannels);
        UiControl control(child);
        const auto nonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto envelope = [&] {return QJsonObject{{"role", "s3-worker"}, {"nonce", nonce}, {"generation", "1"}};};
        auto acknowledge = [&](const QString& kind,const QString& sequence,const QJsonObject& data) {
            auto value=envelope();value["kind"]=kind;value["replyTo"]=sequence;value["data"]=data;
            return !control.send("checkpointAccepted",value).isEmpty();
        };
        bool ready=false,wireFailed=false,terminal=false;QString readinessRpc;QJsonObject result;
        QList<UploadProgress> progress;QStringList sequences;
        connect(&control,&UiControl::failed,this,[&](Error){wireFailed=true;});
        connect(&control,&UiControl::frame,this,[&](const QString& type,const QString& sequence,const QJsonObject& value) {
            if(type=="ready"){ready=true;return;}
            const auto kind=value["kind"].toString();
            if(type=="observed"&&kind=="uploadProgress") {
                const auto observed=UploadProgress::fromJson(value["data"].toObject());
                if(!observed){wireFailed=true;return;}
                progress.append(*observed);sequences.append(sequence);return;
            }
            if(type=="observed"&&kind=="credentials") {
                acknowledge(kind,value["rpc"].toString(),{{"credentials",s3wire::encode(Credentials{"PUTGD_SYNTHETIC_ACCESS","putgd-synthetic-secret-not-a-real-key",{},1})}});
            } else if(type=="observed"&&kind=="readiness") {
                // Hold the real SDK before body transfer, while its preparing
                // observation is forwarded by the production worker timer.
                readinessRpc=value["rpc"].toString();
            } else if(type=="checkpoint"&&kind=="result") {
                result=value["data"].toObject();terminal=true;
                for(qsizetype index=1;index<sequences.size();++index)acknowledge("uploadProgress",sequences[index],{});
                acknowledge("result","terminal",{});
            }
        });
        child.start(QString::fromUtf8(S3_REAL_WORKER_PATH),{});QVERIFY(child.waitForStarted(5000));
        auto boot=envelope();boot["build"]=QJsonDocument::fromJson(buildManifestJson()).object();
        boot["contract"]=QString::fromUtf8(PUTGD_S3_BUILD_ID);boot["safeDelete"]=bool(PUTGD_SDK_CURL_TRANSPORT);boot["modulePath"]=S3Module::defaultPath();
        QVERIFY(!control.send("bootstrap",boot).isEmpty());PUTGD_TRY_VERIFY_WITH_TIMEOUT(ready&&!wireFailed,5000);
        auto active=envelope();active["method"]="upload";active["request"]=s3wire::encode(request);
        QVERIFY(!control.send("activate",active).isEmpty());
        PUTGD_TRY_VERIFY_WITH_TIMEOUT(!readinessRpc.isEmpty()&&progress.size()==1,5000);
        QCOMPARE(progress.first().phase,QString("preparing"));
        QVERIFY(acknowledge("readiness",readinessRpc,{{"error",s3wire::encode(Error{})}}));
        PUTGD_TRY_COMPARE_WITH_TIMEOUT(fixture.events("PUT").size(),1,5000);
        putgd::test::settle(650);PUTGD_TRY_COMPARE(progress.size(),1);PUTGD_TRY_VERIFY(!terminal);PUTGD_TRY_VERIFY(!wireFailed);
        QVERIFY(acknowledge("uploadProgress",sequences.first(),{}));
        PUTGD_TRY_COMPARE_WITH_TIMEOUT(progress.size(),2,5000);
        QCOMPARE(progress.last().phase,QString("uploading"));
        QCOMPARE(progress.last().bytesTransferred,std::optional<qint64>(request.artifact.bytes));
        QCOMPARE(progress.last().bytesTotal,std::optional<qint64>(request.artifact.bytes));
        // Keep that observation outstanding. The terminal flush can add exactly
        // one final value, then deliver the durable result without an ACK wait.
        fixture.control("/__release",{{"barrier","progress"}},true);
        PUTGD_TRY_VERIFY_WITH_TIMEOUT(terminal,5000);QCOMPARE(progress.size(),3);
        QCOMPARE(progress.last().phase,QString("finalizing"));QCOMPARE(result["status"].toString(),QString("uploaded"));
        PUTGD_TRY_COMPARE_WITH_TIMEOUT(child.state(),QProcess::NotRunning,5000);
        QCOMPARE(child.exitStatus(),QProcess::NormalExit);QCOMPARE(child.exitCode(),0);QVERIFY(!wireFailed);
    }
    void faultyChildren_data() {
        QTest::addColumn<QString>("mode");
        for (const auto* mode : {"normal", "wrong_build", "duplicate_ready", "wrong_nonce", "normal_missing_terminal",
                                 "terminal_then_crash", "oversized", "truncated", "ready_then_crash", "activated_wrong_nonce", "ready_ignores_activate", "activated_silent", "cancel_ignores_alive", "invalid_upload", "invalid_remove", "prepared_cleanup", "progress_valid", "progress_negative", "progress_over_total", "progress_wrong_total", "progress_unknown_field", "progress_before_activation", "progress_after_terminal", "progress_wrong_method", "progress_wrong_phase", "progress_bad_sequence", "progress_callback_throw", "progress_fractional", "progress_missing_total", "progress_missing_counter"})
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
        QString testCase = "isolatedFaultCase";
        if (mode == "prepared_cleanup") testCase = "isolatedPreparedCleanup";
        else if (mode == "cancel_ignores_alive") testCase = "isolatedCancelledWorker";
        else if (mode.startsWith("invalid_")) testCase = "isolatedInvalidRequest";
        else if (mode.startsWith("progress_")) testCase = "isolatedProgressCase";
        child.start(executable, {testCase});
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
            PUTGD_TRY_VERIFY_WITH_TIMEOUT(result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 20000);
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
            PUTGD_TRY_COMPARE_WITH_TIMEOUT(provider.diagnostics()["queuedRequests"].toInt(), 0, 5000);
            QCOMPARE(provider.diagnostics()["retainedDrivers"].toInt(), 0);
            putgd::test::settle(100);
            PUTGD_TRY_COMPARE(callbacks.load(), 1);
        }
        const auto snapshot = state();
        QVERIFY(!processExists(snapshot["pid"].toString().toLongLong()));
        QCOMPARE(callbacks.load(), 1);
    }
    void isolatedProgressCase() {
        if (qEnvironmentVariable("PUTGD_S3_FAULT_CHILD") != "1") QSKIP("Runs only in the relocated isolated child");
        const auto mode = qEnvironmentVariable("PUTGD_S3_FAULT");
        const auto target = destination();
        CredentialStore credentials;
        credentials.setSession(target.profileId, {"PUTGD_SYNTHETIC_ACCESS", "synthetic-only-not-a-real-key", {}});
        S3Provider provider(credentials);
        std::atomic_int progressCount{0};
        auto delivered = std::make_shared<std::promise<Error>>();
        auto result = delivered->get_future();
        if (mode == "progress_wrong_method") {
            provider.checkName(target, "synthetic.txt", [delivered](NameCheck reply) { delivered->set_value(reply.error); });
        } else {
            Artifact artifact{"synthetic-item", "/tmp/synthetic-progress", "synthetic.txt", "text/plain", "", QString(64, 'a'), 17, false, {}, false};
            UploadRequest request{target, artifact, "synthetic.txt", "synthetic-operation/item", "absent",
                                  {{"schemaVersion", 1}, {"key", "synthetic.txt"}}, {}, {}};
            request.progress = [&](const UploadProgress& value) {
                if (value.phase == "uploading" && value.bytesTransferred == 8 && value.bytesTotal == 17) ++progressCount;
                if (mode == "progress_callback_throw") throw std::runtime_error("Synthetic callback exception");
            };
            provider.upload(request, [delivered](UploadResult reply) { delivered->set_value(reply.error); });
        }
        PUTGD_TRY_VERIFY_WITH_TIMEOUT(result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 20000);
        const auto error = result.get();
        QCOMPARE(error.code, (mode == "progress_valid" || mode == "progress_callback_throw") ? QString("s3_request_failed") : QString("s3_transport_unknown"));
        QCOMPARE(progressCount.load(), (mode == "progress_valid" || mode == "progress_callback_throw") ? 1 : 0);
        putgd::test::settle(350);
        PUTGD_TRY_COMPARE(progressCount.load(), (mode == "progress_valid" || mode == "progress_callback_throw") ? 1 : 0);
    }
    void slowInlineCheckpointDoesNotLookLikeChildSilence() {
        test::S3Fixture fixture;
        auto uploaded = fixture.upload(fixture.request("slow-inline.txt"));
        QCOMPARE(uploaded.status, QString("uploaded"));
        Error error;
        auto driver = std::unique_ptr<S3Driver>(createS3ProcessDriver(S3Module::defaultPath(),
            [&](const QString& profile, const QJsonObject& reference) { return fixture.credentials.cached(profile, reference); },
            [&](const Destination& value) { return fixture.provider.readiness(value); }, error, {0, 1500}));
        QVERIFY(driver && !error);
        int callbacks = 0, checkpoints = 0; DeleteResult result;
        driver->remove({"slow-inline-delete", uploaded.receipt, {}, {}}, [&](auto, auto ack) {
            ++checkpoints;
            // Real host save/fsync callbacks can block their caller. During this
            // interval the healthy child's heartbeats cannot be read by Qt.
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            ack({});
        }, [&](DeleteResult value) { ++callbacks; result = std::move(value); });
        QCOMPARE(callbacks, 1);
        QCOMPARE(result.status, QString("removed"));
        QVERIFY(!result.error);
        QCOMPARE(checkpoints, 2);
        QCOMPARE(fixture.events("DELETE").size(), 1);
    }
    void asynchronousCheckpointWaitDoesNotRequireChildHeartbeats() {
        test::S3Fixture fixture;
        auto uploaded = fixture.upload(fixture.request("async-ack.txt"));
        QCOMPARE(uploaded.status, QString("uploaded"));
        Error error;
        auto driver = std::unique_ptr<S3Driver>(createS3ProcessDriver(S3Module::defaultPath(),
            [&](const QString& profile, const QJsonObject& reference) { return fixture.credentials.cached(profile, reference); },
            [&](const Destination& value) { return fixture.provider.readiness(value); }, error, {0, 1500}));
        QVERIFY(driver && !error);
        std::vector<std::thread> acknowledgements;
        int callbacks = 0; DeleteResult result;
        driver->remove({"async-ack-delete", uploaded.receipt, {}, {}}, [&](auto, auto ack) {
            acknowledgements.emplace_back([ack = std::move(ack)] {
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                ack({});
            });
        }, [&](DeleteResult value) { ++callbacks; result = std::move(value); });
        // Join before assertions/timeout returns; the callback owns only its
        // shared acknowledgement mailbox, never transport or test-stack state.
        for (auto& acknowledgement : acknowledgements) acknowledgement.join();
        QCOMPARE(callbacks, 1);
        QCOMPARE(result.status, QString("removed"));
        QVERIFY(!result.error);
        QCOMPARE(acknowledgements.size(), size_t(2));
        QCOMPARE(fixture.events("DELETE").size(), 1);
    }
    void checkpointDeadlineRejectsMissingAndLateAcknowledgement_data() {
        QTest::addColumn<bool>("lateAcknowledgement");
        QTest::newRow("missing") << false;
        QTest::newRow("late") << true;
    }
    void checkpointDeadlineRejectsMissingAndLateAcknowledgement() {
        QFETCH(bool, lateAcknowledgement);
        test::S3Fixture fixture;
        auto uploaded = fixture.upload(fixture.request("ack-deadline.txt"));
        QCOMPARE(uploaded.status, QString("uploaded"));
        Error error;
        auto driver = std::unique_ptr<S3Driver>(createS3ProcessDriver(S3Module::defaultPath(),
            [&](const QString& profile, const QJsonObject& reference) { return fixture.credentials.cached(profile, reference); },
            [&](const Destination& value) { return fixture.provider.readiness(value); }, error, {0, 0, 100}));
        QVERIFY(driver && !error);
        std::vector<std::thread> acknowledgements;
        int callbacks = 0, checkpoints = 0; DeleteResult result;
        driver->remove({"ack-deadline-delete", uploaded.receipt, {}, {}}, [&](auto, auto ack) {
            ++checkpoints;
            if(lateAcknowledgement) acknowledgements.emplace_back([ack = std::move(ack)] {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                ack({});
            });
        }, [&](DeleteResult value) { ++callbacks; result = std::move(value); });
        for(auto& acknowledgement : acknowledgements) acknowledgement.join();
        QCOMPARE(callbacks, 1);
        QCOMPARE(checkpoints, 1);
        QCOMPARE(result.status, QString("unknown"));
        QCOMPARE(result.error.code, QString("s3_transport_unknown"));
        QVERIFY(result.progress.isEmpty());
        QCOMPARE(fixture.events("DELETE").size(), 0);
    }
    void minimalReconcileAndHistoricalUrlRequestsKeepParity() {
        test::S3Fixture fixture;
        const auto request = fixture.request("historical-url.txt");
        auto uploaded = fixture.upload(request);
        QCOMPARE(uploaded.status, QString("uploaded"));
        Error error;
        auto driver = std::unique_ptr<S3Driver>(createS3ProcessDriver(S3Module::defaultPath(),
            [&](const QString& profile, const QJsonObject& reference) { return fixture.credentials.cached(profile, reference); },
            [&](const Destination& value) { return fixture.provider.readiness(value); }, error));
        QVERIFY(driver && !error);
        UploadRequest minimal;
        minimal.destination = uploaded.receipt.destination;
        minimal.publicationId = uploaded.receipt.publicationId;
        minimal.artifact.sha256 = uploaded.receipt.objectReference["sha256"].toString();
        minimal.artifact.bytes = uploaded.receipt.objectReference["bytes"].toInteger();
        minimal.artifact.path = "relative-unused-path";
        minimal.objectReference = {{"key", uploaded.receipt.objectReference["key"]}};
        StatResult reconciled;
        driver->reconcile(minimal, [&](StatResult value) { reconciled = std::move(value); });
        QCOMPARE(reconciled.status, QString("own"));
        QVERIFY(!reconciled.error);
        QCOMPARE(reconciled.receipt.publicationId, uploaded.receipt.publicationId);
        auto historical = uploaded.receipt;
        historical.directUrl = "https://old.example.invalid/legacy-object?old-link=1";
        auto optionalToken = historical; optionalToken.concurrencyToken.clear();
        StatResult inspected;
        driver->stat(optionalToken, [&](StatResult value) { inspected = std::move(value); });
        QCOMPARE(inspected.status, QString("own"));
        QVERIFY(!inspected.error);
        QCOMPARE(inspected.receipt.directUrl, uploaded.receipt.directUrl);
        const QJsonObject progress{{"schemaVersion", 1}, {"phase", "object_intent"}, {"target", historical.toJson()}};
        int callbacks = 0; DeleteResult removed;
        driver->remove({"", historical, progress, {}}, [](auto, auto ack) { ack({}); },
            [&](DeleteResult value) { ++callbacks; removed = std::move(value); });
        QCOMPARE(callbacks, 1);
        QCOMPARE(removed.status, QString("removed"));
        QCOMPARE(removed.progress["target"].toObject(), historical.toJson());
        QCOMPARE(fixture.events("DELETE").size(), 1);
        QCOMPARE(fixture.events("DELETE")[0].toObject()["bucket"].toString(), QString("isolated-fixture"));
    }
    void invalidWireRequestsNeverHaveRemoteEffects() {
        test::S3Fixture fixture;
        auto request = fixture.request("invalid-wire.txt");
        auto uploaded = fixture.upload(request);
        QCOMPARE(uploaded.status, QString("uploaded"));
        const auto before = fixture.events().size();
        Error error;
        auto driver = std::unique_ptr<S3Driver>(createS3ProcessDriver(S3Module::defaultPath(),
            [&](const QString& profile, const QJsonObject& reference) { return fixture.credentials.cached(profile, reference); },
            [&](const Destination& value) { return fixture.provider.readiness(value); }, error));
        QVERIFY(driver && !error);
        request.artifact.mime.clear();
        int callbacks = 0; UploadResult badUpload;
        driver->upload(request, [&](UploadResult value) { ++callbacks; badUpload = std::move(value); });
        QCOMPARE(callbacks, 1);
        QCOMPARE(badUpload.status, QString("failed"));
        QCOMPARE(badUpload.error.code, QString("artifact_invalid"));
        QCOMPARE(fixture.events().size(), before);
        auto receipt = uploaded.receipt; receipt.schemaVersion = 2;
        QJsonObject progress{{"schemaVersion", 1}, {"phase", "object_intent"}, {"target", uploaded.receipt.toJson()}};
        DeleteResult badDelete;
        driver->remove({"invalid-receipt-delete", receipt, progress, {}}, [](auto, auto ack) { ack({}); },
            [&](DeleteResult value) { ++callbacks; badDelete = std::move(value); });
        QCOMPARE(callbacks, 2);
        QCOMPARE(badDelete.status, QString("failed"));
        QCOMPARE(badDelete.error.code, QString("receipt_invalid"));
        QCOMPARE(badDelete.progress, progress);
        QCOMPARE(fixture.events().size(), before);
        QCOMPARE(fixture.events("DELETE").size(), 0);
        NameCheck invalidKey;
        driver->checkName(fixture.destination, QString("bad") + QChar(0) + "key", [&](NameCheck value) { ++callbacks; invalidKey = std::move(value); });
        QCOMPARE(invalidKey.status, QString("inconclusive"));
        QCOMPARE(invalidKey.error.code, QString("s3_key_invalid"));
        QCOMPARE(fixture.events().size(), before);
        auto longPrefix = fixture.destination;
        longPrefix.settings["objectPrefix"] = QString(1022, 'p');
        driver->checkName(longPrefix, "ab", [&](NameCheck value) { ++callbacks; invalidKey = std::move(value); });
        QCOMPARE(invalidKey.error.code, QString("s3_key_invalid"));
        QCOMPARE(callbacks, 4);
        QCOMPARE(fixture.events().size(), before);
    }
    void isolatedInvalidRequest() {
        if (qEnvironmentVariable("PUTGD_S3_FAULT_CHILD") != "1") QSKIP("Runs only in the relocated isolated child");
        Error error;
        auto driver = std::unique_ptr<S3Driver>(createS3ProcessDriver("/missing-test-module", [](auto, auto) -> std::optional<Credentials> { return {}; },
            [](auto) { return Error{}; }, error));
        QVERIFY(driver && !error);
        int callbacks = 0;
        if (qEnvironmentVariable("PUTGD_S3_FAULT") == "invalid_upload") {
            UploadRequest request; request.destination = destination();
            UploadResult result;
            driver->upload(request, [&](UploadResult value) { ++callbacks; result = std::move(value); });
            QCOMPARE(result.status, QString("failed"));
            QCOMPARE(result.error.code, QString("artifact_invalid"));
        } else {
            DeleteRequest request; request.id = "invalid-delete"; request.receipt.destination = destination();
            request.progress = {{"opaque", "original-progress"}};
            DeleteResult result;
            driver->remove(request, [](auto, auto ack) { ack({}); }, [&](DeleteResult value) { ++callbacks; result = std::move(value); });
            QCOMPARE(result.status, QString("failed"));
            QCOMPARE(result.error.code, QString("receipt_invalid"));
            QCOMPARE(result.progress, request.progress);
        }
        driver.reset();
        QCOMPARE(callbacks, 1);
        const auto snapshot = state();
        QVERIFY(!snapshot["activated"].toBool());
        QVERIFY(snapshot["shutdown"].toBool());
        QVERIFY(!processExists(snapshot["pid"].toString().toLongLong()));
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
        // Cancel an active, heartbeating worker: wait for its activation record
        // instead of a fixed delay, which a loaded host can outrun.
        std::thread cancel([token] {
            QElapsedTimer clock; clock.start();
            while (!state()["activated"].toBool() && clock.elapsed() < 10000) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            token->store(true);
        });
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
        PUTGD_TRY_VERIFY_WITH_TIMEOUT(completed.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 20000);
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
