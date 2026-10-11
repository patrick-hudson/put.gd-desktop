#include "storage/s3/SdkRuntime.h"
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/http/HttpClient.h>
#include <aws/core/http/HttpClientFactory.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <QTest>
#include <atomic>
#include <barrier>
#include <stdexcept>
#include <thread>
#include <vector>

class SdkContract final : public QObject {
    Q_OBJECT
private slots:
    void deferredOwnerLifecycle() {
        using Runtime = putgd::SdkRuntime;
        QVERIFY_EXCEPTION_THROWN(Runtime::ensureInitialized(), std::logic_error);
        {
            Runtime unused(Runtime::Initialization::Deferred);
            QVERIFY_EXCEPTION_THROWN(([] {
                Runtime duplicate(Runtime::Initialization::Deferred);
            }()), std::logic_error);
            // Destroying a rejected second owner must not unregister this one.
            // Leave this generation unused to exercise shutdown without InitAPI.
        }
        QVERIFY_EXCEPTION_THROWN(Runtime::ensureInitialized(), std::logic_error);
        {
            Runtime next(Runtime::Initialization::Deferred);
            Runtime::ensureInitialized();
            QVERIFY_EXCEPTION_THROWN(([] { Runtime duplicate; }()), std::logic_error);
            Runtime::ensureInitialized();
        }
        QVERIFY_EXCEPTION_THROWN(Runtime::ensureInitialized(), std::logic_error);
    }
    void concurrentDeferredInitialization() {
        using Runtime = putgd::SdkRuntime;
        constexpr int generations = 4;
        constexpr int threadCount = 8;
        constexpr int callsPerThread = 16;
        for (int generation = 0; generation < generations; ++generation) {
            Runtime sdk(Runtime::Initialization::Deferred);
            std::atomic_int failures{0};
            std::barrier start(threadCount);
            std::vector<std::thread> workers;
            workers.reserve(threadCount);
            for (int worker = 0; worker < threadCount; ++worker) {
                workers.emplace_back([&] {
                    start.arrive_and_wait();
                    try {
                        for (int call = 0; call < callsPerThread; ++call)
                            Runtime::ensureInitialized();
                    } catch (...) {
                        ++failures;
                    }
                });
            }
            for (auto& worker : workers) worker.join();
            QCOMPARE(failures.load(), 0);
            if (Runtime::safeDeleteTransport()) {
                // Exercise the installed factory without a request or credentials.
                // Both configuration and client die before their owning runtime.
                Aws::Client::ClientConfiguration config(false, "legacy", true);
                auto client = Aws::Http::CreateHttpClient(config);
                QVERIFY(client != nullptr);
            }
        }
        QVERIFY_EXCEPTION_THROWN(Runtime::ensureInitialized(), std::logic_error);
    }
    void conditionalHeaders() {
        putgd::SdkRuntime sdk;
        QCOMPARE(QString::fromLatin1(Aws::S3::S3Client::GetServiceName()), QStringLiteral("s3"));
        Aws::S3::Model::PutObjectRequest create;
        create.SetIfNoneMatch("*");
        auto createHeaders = create.GetRequestSpecificHeaders();
        QVERIFY(createHeaders.count("if-none-match") == 1);
        QCOMPARE(QString::fromStdString(createHeaders.at("if-none-match")), QStringLiteral("*"));
        Aws::S3::Model::PutObjectRequest replace;
        replace.SetIfMatch("\"saved-etag\"");
        auto replaceHeaders = replace.GetRequestSpecificHeaders();
        QVERIFY(replaceHeaders.count("if-match") == 1);
        QCOMPARE(QString::fromStdString(replaceHeaders.at("if-match")), QStringLiteral("\"saved-etag\""));
        Aws::S3::Model::DeleteObjectRequest remove;
        remove.SetIfMatch("\"saved-etag\"");
        auto deleteHeaders = remove.GetRequestSpecificHeaders();
        QVERIFY(deleteHeaders.count("if-match") == 1);
        QCOMPARE(QString::fromStdString(deleteHeaders.at("if-match")), QStringLiteral("\"saved-etag\""));
    }
};
QTEST_GUILESS_MAIN(SdkContract)
#include "test_sdk_contract.moc"
