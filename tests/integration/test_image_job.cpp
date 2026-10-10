#include "core/ImageJob.h"
#include "core/ArtifactStore.h"
#include <QtTest>
#include <QBuffer>
#include <QFile>
#include <QGuiApplication>
#include <QImageReader>
#include <QTemporaryDir>
#include <QUuid>
#include <atomic>
#include <thread>
#include <chrono>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>

using namespace putgd;
namespace {
class ScopedDisplayEnvironment final {
public:
    ScopedDisplayEnvironment(){
        for(const auto* name:{"DISPLAY","WAYLAND_DISPLAY","QT_QPA_PLATFORM"})
            values_.append({QByteArray(name),qEnvironmentVariableIsSet(name),qgetenv(name)});
    }
    ~ScopedDisplayEnvironment(){
        for(const auto& value:values_)
            if(value.present)qputenv(value.name.constData(),value.bytes);
            else qunsetenv(value.name.constData());
    }
    void clear(){for(const auto& value:values_)qunsetenv(value.name.constData());}
    ScopedDisplayEnvironment(const ScopedDisplayEnvironment&)=delete;
    ScopedDisplayEnvironment& operator=(const ScopedDisplayEnvironment&)=delete;
private:
    struct Value{QByteArray name;bool present;QByteArray bytes;};
    QList<Value> values_;
};
// Actual BITMAPV4HEADER + BGRA pixels, not a fabricated helper response.
// The narrow random-alpha rows expose PNG row-filter transport overhead while
// keeping the encoded input and decoded image under a small finite budget.
QByteArray tallAlphaBmp(int height){
    constexpr int offset=14+108;
    QByteArray bytes(offset+4*height,'\0');bytes[0]='B';bytes[1]='M';
    auto put16=[&](int at,quint16 value){qToLittleEndian(value,reinterpret_cast<uchar*>(bytes.data()+at));};
    auto put32=[&](int at,quint32 value){qToLittleEndian(value,reinterpret_cast<uchar*>(bytes.data()+at));};
    put32(2,quint32(bytes.size()));put32(10,offset);put32(14,108);put32(18,1);
    put32(22,quint32(height));put16(26,1);put16(28,32);put32(30,3);put32(34,4*height);
    put32(54,0x00ff0000);put32(58,0x0000ff00);put32(62,0x000000ff);put32(66,0xff000000);
    put32(70,0x73524742); // LCS_sRGB
    quint32 state=0x9e3779b9;
    for(int y=0;y<height;++y){state^=state<<13;state^=state>>17;state^=state<<5;put32(offset+4*y,state);}
    return bytes;
}
const QByteArray svgText = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"96\" height=\"64\"><rect width=\"96\" height=\"64\" fill=\"white\"/><text x=\"4\" y=\"44\" font-family=\"sans-serif\" font-size=\"18\" fill=\"black\">put.gd</text></svg>";
QByteArray imageBytes(const char* format) {
    QImage image(32,16,QImage::Format_ARGB32);
    image.fill(Qt::blue);
    image.setPixelColor(8,8,Qt::red);
    QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
    return image.save(&buffer,format) ? bytes : QByteArray{};
}
QImage readPng(const QByteArray& bytes) {
    QBuffer buffer;buffer.setData(bytes);buffer.open(QIODevice::ReadOnly);
    return QImageReader(&buffer,"png").read();
}
bool containsDarkText(const QImage& image) {
    int dark=0,white=0;
    for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x){
        const auto color=image.pixelColor(x,y);
        if(color.alpha()>200&&color.red()<80&&color.green()<80&&color.blue()<80)++dark;
        if(color.alpha()>200&&color.red()>240&&color.green()>240&&color.blue()>240)++white;
    }
    return dark>10&&white>100;
}
}
class ImageJobTest : public QObject {
    Q_OBJECT
    static QString product(){return QString::fromUtf8(PUTGD_PRODUCT_UI_PATH);}
    static QString fixture(){return QString::fromUtf8(PUTGD_IMAGE_JOB_FIXTURE);}
    bool svgAvailable(Error& error){return ImageJob(product()).discover(error).contains("image/svg+xml");}
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
    void coreParentIsPreserved(){
        QVERIFY(QCoreApplication::instance());
        QVERIFY(!qobject_cast<QGuiApplication*>(QCoreApplication::instance()));
        Error error; const auto formats=ImageJob(product()).discover(error);
        QVERIFY2(!error,qPrintable(error.code));QVERIFY(formats.contains("image/png"));
        QVERIFY(!qobject_cast<QGuiApplication*>(QCoreApplication::instance()));
    }
    void genericDecodeAndArtifactPreviewWithoutDisplay(){
        ScopedDisplayEnvironment environment;environment.clear();
        for(const auto* name:{"DISPLAY","WAYLAND_DISPLAY","QT_QPA_PLATFORM"})QVERIFY(!qEnvironmentVariableIsSet(name));
        QVERIFY(!qobject_cast<QGuiApplication*>(QCoreApplication::instance()));
        const auto bytes=imageBytes("BMP");QVERIFY(!bytes.isEmpty());
        Error error;const auto decoded=ImageJob(product()).decode(bytes,ResourceLimits{},error);
        QVERIFY2(!error,qPrintable(error.code));QCOMPARE(decoded.size(),QSize(32,16));
        QCOMPARE(decoded.pixelColor(0,0),QColor(Qt::blue));QCOMPARE(decoded.pixelColor(8,8),QColor(Qt::red));
        QTemporaryDir dir;QVERIFY(dir.isValid());QFile file(dir.filePath("image.bmp"));
        QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(bytes),qint64(bytes.size()));file.close();
        ArtifactStore store(dir.filePath("spool"));ClipboardPayload payload;payload.kind="files";payload.files={file.fileName()};
        const auto acquired=store.acquire(QUuid::createUuid().toString(QUuid::WithoutBraces),payload,ResourceLimits{});
        QVERIFY2(!acquired.error,qPrintable(acquired.error.code));QCOMPARE(acquired.artifacts.size(),1);
        const auto artifact=acquired.artifacts.constFirst();QCOMPARE(artifact.category,QString("images"));
        QVERIFY(QFileInfo(artifact.path).suffix().isEmpty());
        const auto path=store.preview(artifact,1024*1024,32*16);QVERIFY(!path.isEmpty());
        QFile preview(path);QVERIFY(preview.open(QIODevice::ReadOnly));const auto thumbnail=readPng(preview.readAll());
        QCOMPARE(thumbnail.size(),QSize(256,128));QCOMPARE(thumbnail.pixelColor(0,0),QColor(Qt::blue));
        QVERIFY(!store.verify(artifact));QVERIFY(!qobject_cast<QGuiApplication*>(QCoreApplication::instance()));
        for(const auto* name:{"DISPLAY","WAYLAND_DISPLAY","QT_QPA_PLATFORM"})QVERIFY(!qEnvironmentVariableIsSet(name));
    }
    void tallBmpTransportPreservesValidPixelsBeyondRawPlusConstant(){
        constexpr int height=500000;const auto bytes=tallAlphaBmp(height);
        QBuffer input;input.setData(bytes);QVERIFY(input.open(QIODevice::ReadOnly));
        QImageReader reader(&input);QCOMPARE(reader.size(),QSize(1,height));
        const auto original=reader.read();QCOMPARE(original.size(),QSize(1,height));
        QByteArray png;QBuffer output(&png);QVERIFY(output.open(QIODevice::WriteOnly));QVERIFY(original.save(&output,"PNG"));
        ResourceLimits limits;limits.imagePixels=height;limits.imageBytes=bytes.size();limits.maxFileBytes=8*1024*1024;
        QVERIFY(original.sizeInBytes()<=limits.imageBytes);QVERIFY(bytes.size()<=limits.imageBytes);
        QVERIFY2(png.size()>limits.imageBytes+65536,"The real fixture does not expose the previous transport ceiling");
        QVERIFY(png.size()<limits.maxFileBytes);
        Error error;const auto decoded=ImageJob(product()).decode(bytes,limits,error);
        QVERIFY2(!error,qPrintable(error.code));QCOMPARE(decoded.size(),original.size());QCOMPARE(decoded,original);
        QVERIFY(!qobject_cast<QGuiApplication*>(QCoreApplication::instance()));
    }
    void realHelperDecodesBuiltinFormats_data(){
        QTest::addColumn<QByteArray>("format");
        QTest::newRow("png")<<QByteArray("PNG");
        QTest::newRow("bmp-content-detection")<<QByteArray("BMP");
        QTest::newRow("jpeg-content-detection")<<QByteArray("JPEG");
    }
    void realHelperDecodesBuiltinFormats(){
        QFETCH(QByteArray,format);const auto bytes=imageBytes(format.constData());
        if(format=="JPEG"&&bytes.isEmpty())QSKIP("The installed Qt JPEG encoder is absent.");
        QVERIFY(!bytes.isEmpty());
        Error error;const auto image=ImageJob(product()).decode(bytes,ResourceLimits{},error);
        QVERIFY2(!error,qPrintable(error.code));QCOMPARE(image.size(),QSize(32,16));
        if(format=="JPEG")QVERIFY(image.pixelColor(0,0).blue()>200);
        else {QCOMPARE(image.pixelColor(0,0),QColor(Qt::blue));QCOMPARE(image.pixelColor(8,8),QColor(Qt::red));}
        QVERIFY(!qobject_cast<QGuiApplication*>(QCoreApplication::instance()));
    }
    void svgTextDecodeHasFontServicesOnlyInChild(){
        Error discoveryError;const bool svg=svgAvailable(discoveryError);QVERIFY2(!discoveryError,qPrintable(discoveryError.code));
        if(!svg)QSKIP("The installed Qt SVG image codec is absent; real desktop acceptance requires this test to pass.");
        Error error;const auto image=ImageJob(product()).decode(svgText,ResourceLimits{},error);
        QVERIFY2(!error,qPrintable(error.code));QCOMPARE(image.size(),QSize(96,64));QVERIFY(containsDarkText(image));
        QVERIFY(!qobject_cast<QGuiApplication*>(QCoreApplication::instance()));
    }
    void svgRegularFilePreviewThroughArtifactStore(){
        Error discoveryError;const bool svg=svgAvailable(discoveryError);QVERIFY2(!discoveryError,qPrintable(discoveryError.code));
        if(!svg)QSKIP("The installed Qt SVG image codec is absent; real desktop acceptance requires this test to pass.");
        QTemporaryDir dir;QVERIFY(dir.isValid());QFile file(dir.filePath("text.svg"));
        QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(svgText),qint64(svgText.size()));file.close();
        ArtifactStore store(dir.filePath("spool"));ClipboardPayload payload;payload.kind="files";payload.files={file.fileName()};
        const auto acquired=store.acquire(QUuid::createUuid().toString(QUuid::WithoutBraces),payload,ResourceLimits{});
        QVERIFY2(!acquired.error,qPrintable(acquired.error.code));QCOMPARE(acquired.artifacts.size(),1);
        const auto artifact=acquired.artifacts.constFirst();QCOMPARE(artifact.category,QString("images"));
        QVERIFY(QFileInfo(artifact.path).suffix().isEmpty());
        const auto path=store.preview(artifact,1024*1024,96*64);QVERIFY(!path.isEmpty());
        QFile preview(path);QVERIFY(preview.open(QIODevice::ReadOnly));const auto image=readPng(preview.readAll());
        QVERIFY(!image.isNull());QVERIFY(image.width()<=256&&image.height()<=256);QVERIFY(containsDarkText(image));
        QVERIFY(!store.verify(artifact));QVERIFY(!qobject_cast<QGuiApplication*>(QCoreApplication::instance()));
    }
    void regularFilePreviewSniffsContentAndHonorsLimits(){
        QTemporaryDir dir;QVERIFY(dir.isValid());QFile file(dir.filePath("no-extension"));
        const auto bytes=imageBytes("BMP");QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(bytes),qint64(bytes.size()));file.close();
        Error error;const auto png=ImageJob(product()).preview(file.fileName(),32*16,1024*1024,error);
        QVERIFY2(!error,qPrintable(error.code));const auto image=readPng(png);QCOMPARE(image.size(),QSize(256,128));
        QCOMPARE(image.pixelColor(0,0),QColor(Qt::blue));
        error={};QVERIFY(ImageJob(product()).preview(file.fileName(),32*16-1,1024*1024,error).isEmpty());QVERIFY(error);
        error={};QVERIFY(ImageJob(product()).preview(file.fileName(),32*16,1,error).isEmpty());QVERIFY(error);
    }
    void decodeHonorsPixelAndByteLimits(){
        const auto bytes=imageBytes("BMP");ResourceLimits limits;limits.imagePixels=32*16-1;
        Error error;QVERIFY(ImageJob(product()).decode(bytes,limits,error).isNull());QVERIFY(error);
        limits=ResourceLimits{};limits.imageBytes=32*16*4-1;error={};
        QVERIFY(ImageJob(product()).decode(bytes,limits,error).isNull());QVERIFY(error);
    }
    void authenticatedResultWaitsForNormalExitAndPreservesDiscoveryOrder(){
        QTemporaryDir dir;QVERIFY(dir.isValid());const auto path=dir.filePath("record.json");fixtureMode("delayed-exit",path);
        Error error;QElapsedTimer clock;clock.start();const auto formats=ImageJob(fixture(),2000).discover(error);
        QVERIFY2(!error,qPrintable(error.code));QVERIFY(clock.elapsed()>=180);
        QCOMPARE(formats,QList<QByteArray>({"image/x-custom-first","application/x-vendor-image","image/png","image/x-custom-first"}));
        QVERIFY(record(path)["accepted"].toBool());verifyDrained(path);
    }
    void helperFailures_data(){
        QTest::addColumn<QByteArray>("mode");QTest::addColumn<QString>("code");
        for(const auto* mode:{"malformed","version-mismatch","stale-nonce","stale-generation","wrong-role","build-mismatch","stale-result","duplicate-result","invalid-discovery"})
            QTest::newRow(mode)<<QByteArray(mode)<<QString("image_job_protocol");
        for(const auto* mode:{"exit-before-result","nonzero-after-result","crash-after-result"})
            QTest::newRow(mode)<<QByteArray(mode)<<QString("image_job_exit");
        for(const auto* mode:{"replace-output","output-permissions","output-hardlink","wrong-size","wrong-hash"})
            QTest::newRow(mode)<<QByteArray(mode)<<QString("image_job_io");
        for(const auto* mode:{"partial-frame","stall","no-exit"})
            QTest::newRow(mode)<<QByteArray(mode)<<QString("image_job_timeout");
        QTest::newRow("authenticated-error")<<QByteArray("error-result")<<QString("source_unsupported");
    }
    void helperFailures(){
        QFETCH(QByteArray,mode);QFETCH(QString,code);QTemporaryDir dir;QVERIFY(dir.isValid());
        const auto path=dir.filePath("record.json");fixtureMode(mode,path);Error error;QElapsedTimer clock;clock.start();
        QVERIFY(ImageJob(fixture(),500).discover(error).isEmpty());QCOMPARE(error.code,code);
        QVERIFY(clock.elapsed()<4000);verifyDrained(path);
    }
    void privateInputReplacementFailsClosed(){
        QTemporaryDir dir;QVERIFY(dir.isValid());const auto path=dir.filePath("record.json");fixtureMode("replace-input",path);
        Error error;QVERIFY(ImageJob(fixture(),2000).decode(imageBytes("PNG"),ResourceLimits{},error).isNull());
        QCOMPARE(error.code,QString("image_job_io"));verifyDrained(path);
    }
    void cancellationDrainsTheChildAndFiles(){
        QTemporaryDir dir;QVERIFY(dir.isValid());const auto path=dir.filePath("record.json");fixtureMode("cancel-stall",path);
        auto cancelled=std::make_shared<std::atomic_bool>(false);
        std::atomic_bool childReady=false;
        std::thread trigger([&,cancelled]{
            for(int attempt=0;attempt<100;++attempt){
                if(record(path)["jobDirectory"].toString().startsWith("/tmp/pgi-")){childReady.store(true);break;}
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            cancelled->store(true);
        });
        Error error;const auto formats=ImageJob(fixture(),2000).discover(error,cancelled);trigger.join();
        QVERIFY(childReady.load());QVERIFY(formats.isEmpty());QCOMPARE(error.code,QString("cancelled"));verifyDrained(path);
    }
    void alreadyCancelledDoesNotLaunch(){
        QTemporaryDir dir;QVERIFY(dir.isValid());const auto path=dir.filePath("record.json");fixtureMode("normal",path);
        auto cancelled=std::make_shared<std::atomic_bool>(true);Error error;
        QVERIFY(ImageJob(fixture()).discover(error,cancelled).isEmpty());QCOMPARE(error.code,QString("cancelled"));QVERIFY(!QFileInfo::exists(path));
    }
    void missingHelperFailsWithoutResidue(){
        Error error;QVERIFY(ImageJob("/does-not-exist/putgd-ui",500).discover(error).isEmpty());QCOMPARE(error.code,QString("image_job_start"));
    }

};
QTEST_GUILESS_MAIN(ImageJobTest)
#include "test_image_job.moc"
