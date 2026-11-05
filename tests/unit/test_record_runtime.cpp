#include "record/BundledRuntime.h"
#include <QtTest>
using namespace putgd::record;

// Inside the AppImage, putgd-record's GStreamer must see only the image's
// plugins; anywhere else the host's GStreamer configuration stays untouched.
class RecordRuntime : public QObject {
    Q_OBJECT
private slots:
    void confinesGStreamerToTheImageOnlyFromInsideIt() {
        QTemporaryDir image, cache, elsewhere;
        QVERIFY(image.isValid() && cache.isValid() && elsewhere.isValid());
        QVERIFY(QDir(image.path()).mkpath("usr/bin") && QDir(image.path()).mkpath("usr/lib/gstreamer-1.0") && QDir(image.path()).mkpath("usr/libexec/gstreamer-1.0"));
        const auto helper = image.filePath("usr/bin/putgd-record");
        const auto scanner = image.filePath("usr/libexec/gstreamer-1.0/gst-plugin-scanner");
        const auto hostHelper = elsewhere.filePath("putgd-record");
        for (const auto& path : {helper, scanner, hostHelper}) {
            QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
            QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        }
        const auto root = QFileInfo(image.path()).canonicalFilePath();
        const auto values = bundledGStreamerEnvironment(image.path(), helper, cache.path());
        QCOMPARE(values.value("GST_PLUGIN_SYSTEM_PATH_1_0"), root + "/usr/lib/gstreamer-1.0");
        QCOMPARE(values.value("GST_PLUGIN_SYSTEM_PATH"), root + "/usr/lib/gstreamer-1.0");
        QCOMPARE(values.value("GST_PLUGIN_SCANNER_1_0"), root + "/usr/libexec/gstreamer-1.0/gst-plugin-scanner");
        // Extra plugin paths are emptied, not left to the caller's environment.
        QVERIFY(values.contains("GST_PLUGIN_PATH_1_0") && values.value("GST_PLUGIN_PATH_1_0").isEmpty());
        QVERIFY(values.contains("GST_PLUGIN_PATH") && values.value("GST_PLUGIN_PATH").isEmpty());
        QCOMPARE(values.value("GST_REGISTRY_1_0"), cache.path() + "/putgd/gstreamer-1.0-appimage-registry.bin");
        // A host install that merely inherits APPDIR, or an image without the
        // plugins and scanner, keeps the host's GStreamer.
        QVERIFY(bundledGStreamerEnvironment(image.path(), hostHelper, cache.path()).isEmpty());
        QVERIFY(bundledGStreamerEnvironment(QString(), helper, cache.path()).isEmpty());
        QVERIFY(QFile::remove(scanner));
        QVERIFY(bundledGStreamerEnvironment(image.path(), helper, cache.path()).isEmpty());
    }
};

QTEST_GUILESS_MAIN(RecordRuntime)
#include "test_record_runtime.moc"
