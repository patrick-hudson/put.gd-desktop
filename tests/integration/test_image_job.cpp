#include "core/ImageJob.h"
#include <QtTest>
#include <QBuffer>
#include <QImage>
#include <QFile>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>

using namespace putgd;
namespace {
QByteArray imageBytes(const char* format) {
    QImage image(32,16,QImage::Format_ARGB32);
    image.fill(Qt::blue);
    image.setPixelColor(8,8,Qt::red);
    QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
    return image.save(&buffer,format) ? bytes : QByteArray{};
}
}
class ImageJobTest : public QObject {
    Q_OBJECT
    static QString fixture(){return QString::fromUtf8(PUTGD_IMAGE_JOB_FIXTURE);}
    static QJsonObject record(const QString& path){QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};return QJsonDocument::fromJson(file.readAll()).object();}
    static void verifyDrained(const QString& path){
        const auto value=record(path);QVERIFY(!value.isEmpty());
        const auto pid=value["pid"].toInteger();QVERIFY(pid>0);
        QVERIFY2(!QFileInfo::exists("/proc/"+QString::number(pid)),"ImageJob returned while its helper still exists");
        const auto directory=value["jobDirectory"].toString();QVERIFY(directory.startsWith("/tmp/pgi-"));
        QVERIFY2(!QFileInfo::exists(directory),"Private image-job files survived completion");
    }
    static void fixtureMode(const QByteArray& mode,const QString& recordPath){
        qputenv("PUTGD_TEST_IMAGE_JOB_MODE",mode);qputenv("PUTGD_TEST_IMAGE_JOB_RECORD",QFile::encodeName(recordPath));
    }
private slots:
    void cleanup(){qunsetenv("PUTGD_TEST_IMAGE_JOB_MODE");qunsetenv("PUTGD_TEST_IMAGE_JOB_RECORD");}
    // The helper is untrusted: one representative of each failure class fails closed, and
    // leaves no helper process or private job files behind. Only the timeout row has a
    // short deadline; the others get one a slow machine cannot reach first (#1417).
    void helperFailures_data(){
        QTest::addColumn<QByteArray>("mode");QTest::addColumn<QString>("code");QTest::addColumn<int>("deadlineMs");
        QTest::newRow("stale-nonce")<<QByteArray("stale-nonce")<<QString("image_job_protocol")<<60000;
        QTest::newRow("crash-after-result")<<QByteArray("crash-after-result")<<QString("image_job_exit")<<60000;
        QTest::newRow("output-permissions")<<QByteArray("output-permissions")<<QString("image_job_io")<<60000;
        QTest::newRow("stall")<<QByteArray("stall")<<QString("image_job_timeout")<<500;
    }
    void helperFailures(){
        QFETCH(QByteArray,mode);QFETCH(QString,code);QFETCH(int,deadlineMs);QTemporaryDir dir;QVERIFY(dir.isValid());
        const auto path=dir.filePath("record.json");fixtureMode(mode,path);Error error;
        QVERIFY(ImageJob(fixture(),deadlineMs).discover(error).isEmpty());QCOMPARE(error.code,code);
        verifyDrained(path);
    }
    void privateInputReplacementFailsClosed(){
        QTemporaryDir dir;QVERIFY(dir.isValid());const auto path=dir.filePath("record.json");fixtureMode("replace-input",path);
        Error error;QVERIFY(ImageJob(fixture(),60000).decode(imageBytes("PNG"),ResourceLimits{},error).isNull());
        QCOMPARE(error.code,QString("image_job_io"));verifyDrained(path);
    }
};
QTEST_GUILESS_MAIN(ImageJobTest)
#include "test_image_job.moc"
