#include "storage/s3/SdkRuntime.h"
#include "storage/s3/S3ErrorDiagnostics.h"
#include "storage/s3/TlsCompatibility.h"
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/http/HttpClient.h>
#include <aws/core/http/HttpClientFactory.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#if defined(PUTGD_OPENSSL_GROUP_POLICY)
#include <openssl/err.h>
#endif
#include <atomic>
#include <barrier>
#include <stdexcept>
#include <thread>
#include <vector>

class SdkContract final : public QObject {
    Q_OBJECT
private slots:
    void transportCodeDoesNotExportSdkMessage() {
        QCOMPARE(putgd::sdkCurlCode("curlCode: 7, private endpoint and secret"), 7);
        QCOMPARE(putgd::sdkCurlCode("curlCode: 35, TLS details"), 35);
        QCOMPARE(putgd::sdkCurlCode("curlCode: 999, future transport"), 999);
        for (const auto input : {"prefix curlCode: 7, secret", "curlCode: -1, secret", "curlCode: +7, secret",
                "curlCode: 7secret, detail", "curlCode: 1000, detail", "curlCode: 7", "curlCode: , detail",
                "curlCode:  7, detail", "curlCode: 7\n, detail", "ordinary SDK failure"})
            QCOMPARE(putgd::sdkCurlCode(input), -1);
    }
    void initialTlsShareCompatibilityPreservesOlderAndOtherBackends() {
        QVERIFY(putgd::smallInitialTlsShare(0x081200, "OpenSSL/3.5.5"));
        QVERIFY(putgd::smallInitialTlsShare(0x074900, "OpenSSL/3.5.0"));
        QVERIFY(putgd::smallInitialTlsShare(0x081200, "OpenSSL/4.0.0"));
        for (const auto backend : {"OpenSSL/3.0.13", "OpenSSL/3.4.9", "LibreSSL/3.5.5",
                "GnuTLS/3.8.9", "Schannel", "GnuTLS/3.8.9 (OpenSSL/3.5.5)",
                "OpenSSL/3.5.5garbage", "OpenSSL/3.5.5 extra", "OpenSSL/3.x.5", "OpenSSL/3.5suffix", "OpenSSL/", ""})
            QVERIFY(!putgd::smallInitialTlsShare(0x081200, backend));
        QVERIFY(!putgd::smallInitialTlsShare(0x074800, "OpenSSL/3.5.5"));
    }
#if defined(PUTGD_OPENSSL_GROUP_POLICY)
    void initialTlsSharePreservesConfiguredSystemGroups() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("openssl.cnf");
        const auto write = [&](const QByteArray& contents) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly)) return false;
            return file.write(contents) == contents.size();
        };
        QVERIFY(write("openssl_conf=init\n[init]\nssl_conf=ssl\n[ssl]\nsystem_default=policy\n[policy]\nGroups=P-256\n"));
        QVERIFY(!putgd::defaultSystemTlsGroups(qPrintable(path)));
        for (const auto name : {"Curves", "1.Groups", "2.Curves"}) {
            QVERIFY(write(QByteArray("openssl_conf=init\n[init]\nssl_conf=ssl\n[ssl]\nsystem_default=policy\n[policy]\n") + name + "=P-256\n"));
            QVERIFY(!putgd::defaultSystemTlsGroups(qPrintable(path)));
        }
        // Resolve includes and variables with OpenSSL's own parser.
        QFile included(directory.filePath("policy.cnf"));
        QVERIFY(included.open(QIODevice::WriteOnly));
        QVERIFY(included.write("[policy]\nGroups=P-256\n") > 0);
        included.close();
        QVERIFY(write("section=policy\nopenssl_conf=init\n[init]\nssl_conf=ssl\n[ssl]\nsystem_default=${default::section}\n.include " + included.fileName().toUtf8() + "\n"));
        QVERIFY(!putgd::defaultSystemTlsGroups(qPrintable(path)));
        QVERIFY(write("openssl_conf=init\n[init]\nssl_conf=ssl\n[ssl]\nsystem_default=policy\n[policy]\nMinProtocol=TLSv1.2\n"));
        QVERIFY(putgd::defaultSystemTlsGroups(qPrintable(path)));
        QVERIFY(write("# No SSL group policy\n"));
        QVERIFY(putgd::defaultSystemTlsGroups(qPrintable(path)));
        QVERIFY(putgd::defaultSystemTlsGroups(""));
        QVERIFY(!putgd::defaultSystemTlsGroups(nullptr));
        QVERIFY(!putgd::defaultSystemTlsGroups(qPrintable(directory.filePath("missing.cnf"))));
        QVERIFY(write(".include " + directory.filePath("missing-include.cnf").toUtf8() + "\n"));
        QVERIFY(!putgd::defaultSystemTlsGroups(qPrintable(path)));
        QVERIFY(write("openssl_conf=missing\n"));
        QVERIFY(!putgd::defaultSystemTlsGroups(qPrintable(path)));
        QVERIFY(write("openssl_conf=${undefined}\n"));
        QVERIFY(!putgd::defaultSystemTlsGroups(qPrintable(path)));
        ERR_clear_error();
        ERR_raise(ERR_LIB_USER, 123);
        const auto previous = ERR_peek_last_error();
        QVERIFY(!putgd::defaultSystemTlsGroups(qPrintable(path)));
        QCOMPARE(ERR_peek_last_error(), previous);
        ERR_clear_error();
    }
#endif
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
