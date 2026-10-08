#include "storage/s3/SdkRuntime.h"
#include <aws/s3/S3Client.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <QTest>

class SdkContract final : public QObject {
    Q_OBJECT
private slots:
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
