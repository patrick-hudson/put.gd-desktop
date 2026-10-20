// A stand-in for `putgd-record --poster` (#112). PosterJob selects this
// executable by constructor or ArtifactStore::setPosterHelperForTesting; the
// product helper never reads these controls.
#include <QBuffer>
#include <QByteArray>
#include <QImage>
#include <QThread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
QByteArray limit(int resource) {
    rlimit value{};
    if (::getrlimit(resource, &value)) return "error";
    return value.rlim_cur == RLIM_INFINITY ? QByteArray("unlimited") : QByteArray::number(qulonglong(value.rlim_cur));
}
int put(const QByteArray& bytes) { return std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout) == size_t(bytes.size()) && std::fflush(stdout) == 0 ? 0 : 4; }
int poster(int edge) {
    QImage image(edge, edge * 9 / 16, QImage::Format_RGB32);
    image.fill(qRgb(40, 120, 200));
    struct stat input{};
    image.setText("putgd-duration-ms", "4200");
    image.setText("stdin-bytes", ::fstat(STDIN_FILENO, &input) ? "error" : QString::number(input.st_size));
    image.setText("stdin-regular", S_ISREG(input.st_mode) ? "1" : "0");
    image.setText("limits", QString::fromLatin1("as=" + limit(RLIMIT_AS) + " fsize=" + limit(RLIMIT_FSIZE) + " nofile=" + limit(RLIMIT_NOFILE) + " cpu=" + limit(RLIMIT_CPU) + " core=" + limit(RLIMIT_CORE)));
    image.setText("nice", QString::number(::getpriority(PRIO_PROCESS, 0)));
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    return image.save(&buffer, "PNG") ? put(png) : 4;
}
}

int main(int argc, char** argv) {
    if (argc != 3 || std::strcmp(argv[1], "--poster") != 0 || std::atoll(argv[2]) < 1) return 2;
    const QByteArray mode = qgetenv("PUTGD_TEST_POSTER_MODE");
    if (mode == "hang") for (;;) QThread::sleep(1);
    if (mode == "flood") { const QByteArray chunk(65536, 'x'); for (;;) if (put(chunk)) return 4; }
    if (mode == "garbage") return put("not a png");
    if (mode == "fail") return 1;
    if (mode == "excessive") return 3;
    if (mode == "huge") return poster(400);
    if (mode == "slow") QThread::sleep(2);
    return poster(64);
}
