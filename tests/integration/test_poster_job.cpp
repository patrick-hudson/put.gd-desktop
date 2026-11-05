#include "core/ArtifactStore.h"
#include "core/PosterJob.h"
#include <QBuffer>
#include <QElapsedTimer>
#include <QImageReader>
#include <QProcess>
#include <QRandomGenerator>
#include <QUuid>
#include <QtTest>
#include <sys/stat.h>
#include <unistd.h>
using namespace putgd;

// #112: History's poster frames come from a transient helper that reads only
// the file the resident opened, under bounded memory, CPU and output, and is
// killed at its deadline. Every failure is soft: no poster, never a hang.
class PosterJobTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    QString input_;
    static QString fixture() { return QStringLiteral(PUTGD_POSTER_FIXTURE); }
    static QImage decode(const QByteArray& png) {
        QBuffer buffer;
        buffer.setData(png);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer, "png");
        return reader.read();
    }
    QByteArray render(const QByteArray& mode, Error& error, const QString& file = {}, int timeoutMs = 10000, qint64 budget = 1 << 20) {
        qputenv("PUTGD_TEST_POSTER_MODE", mode);
        return PosterJob(fixture(), timeoutMs).render(file.isEmpty() ? input_ : file, 100'000'000, budget, error);
    }
private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        input_ = dir_.filePath("video.mp4");
        QFile file(input_);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(QByteArray(4096, '\x5a')) == 4096);
    }
    void cleanup() { qunsetenv("PUTGD_TEST_POSTER_MODE"); }
    void helperReadsTheOpenedFileUnderLimits() {
        Error error;
        const auto png = render({}, error);
        QVERIFY2(!error, qPrintable(error.code));
        const auto image = decode(png);
        QCOMPARE(image.size(), QSize(64, 36));
        QCOMPARE(image.text("putgd-duration-ms"), QString("4200"));
        // The file arrives as standard input, not as a name the helper could reopen elsewhere.
        QCOMPARE(image.text("stdin-regular"), QString("1"));
        QCOMPARE(image.text("stdin-bytes"), QString("4096"));
        const auto limits = image.text("limits");
        QVERIFY2(limits.contains("fsize=0") && limits.contains("nofile=64") && limits.contains("core=0") && limits.contains("cpu=21"), qPrintable(limits));
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
        QVERIFY2(limits.contains("as=2147483648"), qPrintable(limits));
#endif
        QVERIFY(image.text("nice").toInt() >= 10);
    }
    void failuresAreSoft_data() {
        QTest::addColumn<QByteArray>("mode");
        QTest::addColumn<QString>("code");
        QTest::newRow("decode failure") << QByteArray("fail") << "poster_failed";
        QTest::newRow("over the pixel limit") << QByteArray("excessive") << "input_excessive";
        QTest::newRow("not a png") << QByteArray("garbage") << "poster_protocol";
        QTest::newRow("larger than a poster") << QByteArray("huge") << "poster_protocol";
        QTest::newRow("endless output") << QByteArray("flood") << "poster_protocol";
    }
    void failuresAreSoft() {
        QFETCH(QByteArray, mode);
        QFETCH(QString, code);
        Error error;
        QElapsedTimer clock;
        clock.start();
        QVERIFY(render(mode, error).isEmpty());
        QCOMPARE(error.code, code);
        QVERIFY(clock.elapsed() < 5000);
    }
    void lateHelperIsKilledAtTheDeadline() {
        Error error;
        QElapsedTimer clock;
        clock.start();
        QVERIFY(render("hang", error, {}, 600).isEmpty());
        QCOMPARE(error.code, QString("poster_timeout"));
        QVERIFY2(clock.elapsed() < 3000, qPrintable(QString::number(clock.elapsed())));
    }
    void cancellationStopsTheHelper() {
        qputenv("PUTGD_TEST_POSTER_MODE", "hang");
        auto cancelled = std::make_shared<std::atomic_bool>(false);
        Error error;
        QElapsedTimer clock;
        clock.start();
        // The job is synchronous: another thread raises the flag.
        QThread* thread = QThread::create([&] { QThread::msleep(300); cancelled->store(true); });
        thread->start();
        QVERIFY(PosterJob(fixture(), 10000).render(input_, 100'000'000, 1 << 20, error, cancelled).isEmpty());
        thread->wait();
        delete thread;
        QCOMPARE(error.code, QString("cancelled"));
        QVERIFY(clock.elapsed() < 3000);
    }
    void onlyRegularFilesAreHandedOver() {
        Error error;
        const auto link = dir_.filePath("link.mp4");
        QVERIFY(QFile::link(input_, link));
        QVERIFY(render({}, error, link).isEmpty());
        QCOMPARE(error.code, QString("poster_failed"));
        const auto fifo = dir_.filePath("fifo.mp4");
        QCOMPARE(::mkfifo(QFile::encodeName(fifo).constData(), 0600), 0);
        QElapsedTimer clock;
        clock.start();
        QVERIFY(render({}, error, fifo).isEmpty());
        QCOMPARE(error.code, QString("poster_failed"));
        QVERIFY(clock.elapsed() < 2000);
        const auto empty = dir_.filePath("empty.mp4");
        QVERIFY(QFile(empty).open(QIODevice::WriteOnly));
        QVERIFY(render({}, error, empty).isEmpty());
        QVERIFY(render({}, error, dir_.filePath("missing.mp4")).isEmpty());
    }
    void missingHelperIsReported() {
        Error error;
        QVERIFY(PosterJob(dir_.filePath("no-such-helper")).render(input_, 100'000'000, 1 << 20, error).isEmpty());
        QCOMPARE(error.code, QString("poster_helper_missing"));
    }
    void postersShareTheImagePreviewCache() {
        QTemporaryDir data;
        QVERIFY(data.isValid());
        ArtifactStore store(data.filePath("artifacts"));
        ArtifactStore::setPosterHelperForTesting(fixture());
        auto restore = qScopeGuard([] { ArtifactStore::setPosterHelperForTesting({}); });
        Artifact video;
        video.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        video.path = input_;
        video.category = "videos";
        video.mime = "video/mp4";
        const auto path = store.poster(video, 1 << 20, 100'000'000);
        QVERIFY(QFileInfo(path).isFile());
        QCOMPARE(QFileInfo(path).fileName(), video.id + ".png");
        QCOMPARE(QFileInfo(path).absoluteDir(), QDir(data.filePath("previews")));
        QVERIFY(!(QFile::permissions(path) & (QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther)));
        // Disabled cache, non-video artifacts and image-only previews make none.
        QVERIFY(store.poster(video, 0, 100'000'000).isEmpty());
        auto image = video;
        image.category = "images";
        QVERIFY(store.poster(image, 1 << 20, 100'000'000).isEmpty());
        QVERIFY(store.preview(video, 1 << 20, 100'000'000).isEmpty());
        Error failure;
        qputenv("PUTGD_TEST_POSTER_MODE", "fail");
        video.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(store.poster(video, 1 << 20, 100'000'000, {}, &failure).isEmpty());
        QCOMPARE(failure.code, QString("poster_failed"));
        store.clearPreviews();
        QVERIFY(!QFile::exists(path));
    }
    // The real helper, when this build has one: our own exports, the working
    // clip, and corrupt, truncated and oversized files.
    void productHelper() {
#ifndef PUTGD_RECORD_HELPER
        QSKIP("putgd-record is not built");
#else
        const QString helper = QStringLiteral(PUTGD_RECORD_HELPER);
        const auto clip = dir_.filePath("working.mkv");
        QCOMPARE(QProcess::execute(helper, {"--av-marker", clip, "3"}), 0);
        QStringList inputs{clip};
        for (const auto* name : {"export.mp4", "export.webm"}) {
            const auto output = dir_.filePath(name);
            if (QProcess::execute(helper, {"--export", clip, output, "--no-audio"}) == 0) inputs << output;
            else qWarning() << "no encoder for" << name;
        }
        for (const auto& input : inputs) {
            Error error;
            QElapsedTimer clock;
            clock.start();
            const auto png = PosterJob(helper).render(input, 100'000'000, 1 << 20, error);
            QVERIFY2(!error, qPrintable(input + ": " + error.code));
            const auto image = decode(png);
            QCOMPARE(image.size(), QSize(256, 144));
            const auto length = image.text("putgd-duration-ms").toLongLong();
            QVERIFY2(length >= 2900 && length <= 3100, qPrintable(QString::number(length)));
            qInfo().noquote() << QFileInfo(input).fileName() << "poster" << clock.elapsed() << "ms" << png.size() << "bytes";
            Error excessive;
            QVERIFY(PosterJob(helper).render(input, 1280 * 720 - 1, 1 << 20, excessive).isEmpty());
            QCOMPARE(excessive.code, QString("input_excessive"));
        }
        auto corrupt = dir_.filePath("corrupt.mp4");
        {
            QFile file(corrupt);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QByteArray bytes(256 * 1024, 0);
            for (auto& byte : bytes) byte = char(QRandomGenerator::global()->generate());
            bytes.replace(0, 12, QByteArray("\0\0\0\x18" "ftypisom", 12));
            file.write(bytes);
        }
        QStringList bad{corrupt};
        for (const auto& input : inputs) {
            QFile source(input);
            QVERIFY(source.open(QIODevice::ReadOnly));
            const auto cut = dir_.filePath("truncated-" + QFileInfo(input).fileName());
            QFile target(cut);
            QVERIFY(target.open(QIODevice::WriteOnly));
            target.write(source.read(source.size() * 2 / 5));
            bad << cut;
        }
        for (const auto& input : bad) {
            Error error;
            QElapsedTimer clock;
            clock.start();
            const auto png = PosterJob(helper).render(input, 100'000'000, 1 << 20, error);
            // A truncated file may still hold a decodable frame; either way the job ends soft and in time.
            QVERIFY2(error ? png.isEmpty() : decode(png).width() == 256, qPrintable(input));
            QVERIFY2(clock.elapsed() < 10000, qPrintable(input));
            qInfo().noquote() << QFileInfo(input).fileName() << (error ? error.code : QString("poster")) << clock.elapsed() << "ms";
        }
        Error corruptError;
        QVERIFY(PosterJob(helper).render(corrupt, 100'000'000, 1 << 20, corruptError).isEmpty());
#endif
    }
    // GIF and WebP recordings are images: their first frame comes through the
    // image preview path (ImageJob in putgd-ui), not the poster helper.
    void animatedExportsUseTheImagePreview() {
#ifndef PUTGD_RECORD_HELPER
        QSKIP("putgd-record is not built");
#else
        const QString helper = QStringLiteral(PUTGD_RECORD_HELPER);
        const auto clip = dir_.filePath("animated.mkv");
        QCOMPARE(QProcess::execute(helper, {"--av-marker", clip, "2"}), 0);
        QTemporaryDir data;
        QVERIFY(data.isValid());
        ArtifactStore store(data.filePath("artifacts"));
        int checked = 0;
        for (const auto& [format, mime] : {std::pair{QString("gif"), QString("image/gif")}, std::pair{QString("webp"), QString("image/webp")}}) {
            const auto output = dir_.filePath("animated." + format);
            if (QProcess::execute(helper, {"--export", clip, output, "--max-edge", "320"}) != 0) { qWarning() << "no encoder for" << format; continue; }
            Artifact artifact;
            artifact.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            artifact.path = output;
            artifact.mime = mime;
            artifact.category = "images";
            const auto preview = store.preview(artifact, 1 << 20, 100'000'000);
            if (preview.isEmpty() && format == "webp" && !QImageReader::supportedImageFormats().contains("webp")) { qWarning() << "no Qt WebP image plugin"; continue; }
            QVERIFY2(!preview.isEmpty(), qPrintable(format));
            QImageReader reader(preview, "png");
            QCOMPARE(reader.size(), QSize(256, 144));
            ++checked;
        }
        if (!checked) QSKIP("Neither GIF nor WebP export is available");
#endif
    }
};
QTEST_GUILESS_MAIN(PosterJobTest)
#include "test_poster_job.moc"
