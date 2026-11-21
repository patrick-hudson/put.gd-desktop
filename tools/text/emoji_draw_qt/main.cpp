// Draws emoji as Qt's own text renderer does, for tools/text/emoji_templates.py
// (--drawer). Qt draws COLRv1 glyphs itself, not through cairo, and leaves out
// some of their shading, so a Plasma desktop with a COLRv1 emoji font shows
// them differently from GTK and the browsers.
//
//   putgd-emoji-draw-qt FAMILY PIXELS < sequences > records
//
// Reads one UTF-8 emoji sequence per line (emoji_templates.py --list-sequences)
// and writes, for each, in order and little-endian: u32 text bytes, the text,
// u32 width, u32 height, then the glyph's box on transparent as premultiplied
// RGBA rows. Width and height are 0 when the family does not draw the sequence
// as one glyph. Fontconfig finds the family, so FONTCONFIG_FILE chooses which
// file is drawn. emoji_templates.py --drawn reads the records.
//
//   cmake -S tools/text/emoji_draw_qt -B build/emoji-draw-qt -DCMAKE_BUILD_TYPE=Release
//   cmake --build build/emoji-draw-qt
#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QTextLayout>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {
void put(uint32_t value) {
    const unsigned char bytes[4] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
    std::fwrite(bytes, 1, 4, stdout);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: putgd-emoji-draw-qt FAMILY PIXELS < sequences\n");
        return 2;
    }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    int qtArgc = 1;
    QGuiApplication app(qtArgc, argv);
    const int pixels = std::atoi(argv[2]);
    if (pixels < 8 || pixels > 1024) {
        std::fprintf(stderr, "PIXELS must be 8 to 1024\n");
        return 2;
    }
    QFont font(QString::fromUtf8(argv[1]));
    font.setPixelSize(pixels);
    font.setStyleStrategy(QFont::NoFontMerging);
    const int side = int(pixels * 2.2);
    for (std::string line; std::getline(std::cin, line);) {
        const QString text = QString::fromUtf8(line);
        put(uint32_t(line.size()));
        std::fwrite(line.data(), 1, line.size(), stdout);
        QTextLayout layout(text, font);
        layout.beginLayout();
        layout.createLine();
        layout.endLayout();
        qsizetype glyphs = 0;
        bool known = true;
        for (const auto& run : layout.glyphRuns()) {
            glyphs += run.glyphIndexes().size();
            for (const auto glyph : run.glyphIndexes()) known = known && glyph != 0;
        }
        if (glyphs != 1 || !known) {
            put(0);
            put(0);
            continue;
        }
        QImage image(side, side, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            layout.draw(&painter, QPointF(pixels * 0.3, pixels * 0.2));
        }
        int left = side, top = side, right = -1, bottom = -1;
        for (int y = 0; y < side; ++y) {
            const uchar* row = image.constScanLine(y);
            for (int x = 0; x < side; ++x) {
                if (row[4 * x + 3] == 0) continue;
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
        if (right < 0) {
            put(0);
            put(0);
            continue;
        }
        put(uint32_t(right - left + 1));
        put(uint32_t(bottom - top + 1));
        for (int y = top; y <= bottom; ++y)
            std::fwrite(image.constScanLine(y) + 4 * left, 1, size_t(right - left + 1) * 4, stdout);
    }
    return std::fflush(stdout) == 0 ? 0 : 1;
}
