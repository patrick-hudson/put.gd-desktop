#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>
#include <QTextStream>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 3) {
        QTextStream(stderr) << "Usage: putgd-render-icons SVG OUTPUT_DIRECTORY\n";
        return 1;
    }
    QSvgRenderer renderer(QString::fromLocal8Bit(argv[1]));
    if (!renderer.isValid()) {
        QTextStream(stderr) << "Invalid application icon SVG\n";
        return 1;
    }
    const QDir root(QString::fromLocal8Bit(argv[2]));
    for (const int size : {16, 22, 24, 32, 48, 64, 128, 256}) {
        QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        renderer.render(&painter);
        painter.end();
        const auto directory = root.filePath(QStringLiteral("%1x%1/apps").arg(size));
        if (!QDir().mkpath(directory) || !image.save(directory + QStringLiteral("/putgd.png"))) {
            QTextStream(stderr) << "Could not write application icon at " << size << " pixels\n";
            return 1;
        }
    }
    return 0;
}
