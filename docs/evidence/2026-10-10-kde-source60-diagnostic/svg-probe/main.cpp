#include <QBuffer>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#include <memory>

namespace {
void writeJson(const QJsonObject& value) {
    const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
    std::fwrite(bytes.constData(), 1, static_cast<std::size_t>(bytes.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}
const QByteArray svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="96" height="64" viewBox="0 0 96 64"><rect width="96" height="64" fill="white"/><text x="4" y="44" font-family="sans-serif" font-size="18" fill="black">put.gd</text></svg>)SVG";
}
int main(int argc, char** argv) {
    if (argc != 2) return 64;
    const QByteArray mode(argv[1]);
    if (mode != "core" && mode != "gui" && mode != "inventory") return 64;
    writeJson({{"phase", "starting"}, {"mode", QString::fromLatin1(mode)},
          {"compiledQt", QT_VERSION_STR}, {"runtimeQt", qVersion()}});
    std::unique_ptr<QCoreApplication> application;
    if (mode == "gui") application = std::make_unique<QGuiApplication>(argc, argv);
    else application = std::make_unique<QCoreApplication>(argc, argv);
    if (mode == "inventory") {
        const auto types = QImageReader::supportedMimeTypes();
        writeJson({{"phase", "inventory"}, {"svgMimeAdvertised", types.contains("image/svg+xml")},
              {"supportedMimeCount", types.size()}});
        return 0;
    }
    QBuffer input;
    input.setData(svg);
    if (!input.open(QIODevice::ReadOnly)) return 65;
    // Exactly the production non-PNG hint: empty format, content auto-detection.
    QImageReader reader(&input, QByteArray{});
    writeJson({{"phase", "reader_start"}, {"inputBytes", svg.size()}});
    const auto size = reader.size();
    writeJson({{"phase", "size"}, {"width", size.width()}, {"height", size.height()}});
    if (!size.isValid() || size != QSize(96, 64)) return 2;
    const auto image = reader.read();
    int inkPixels = 0;
    if (!image.isNull()) {
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const auto pixel = image.pixel(x, y);
                if (qAlpha(pixel) > 0 && qRed(pixel) < 200 && qGreen(pixel) < 200 && qBlue(pixel) < 200)
                    ++inkPixels;
            }
        }
    }
    const bool ok = !image.isNull() && image.size() == QSize(96, 64) && inkPixels > 0;
    writeJson({{"phase", "decoded"}, {"ok", ok}, {"nullImage", image.isNull()},
          {"width", image.width()}, {"height", image.height()}, {"inkPixels", inkPixels},
          {"format", QString::fromLatin1(reader.format())}, {"readerError", int(reader.error())}});
    return ok ? 0 : 3;
}
