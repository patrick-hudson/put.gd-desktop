#pragma once
// The bar-coded ruler page of the scroll-capture tests (#1376) and its
// verifier. Every pixel line of the page says which line it is, so a stitched
// image is checked line by line: a wrong stitch is a decode failure and never
// a judgement call.
//
// A row is 40 px tall. Across the code area it carries 16 cells:
//   lines 0..1    a separator (all cells dark),
//   lines 2..29   the row's index as 16 black or white cells, bit 15 first,
//   lines 30..39  ten fine lines, each a 16-cell pattern that depends on the
//                 row and the line, so no two neighbouring lines look alike
//                 and no vertical offset but the true one explains an overlap.
// Left of the code area is a label column with the row's number as text (not
// decoded); right of it, a scrollbar strip.
#include <QColor>
#include <QImage>
#include <QString>
#include <cstdint>

namespace putgd::test::ruler {
inline constexpr int rowHeight = 40, cells = 16, labelWidth = 64, scrollbarWidth = 12, indexFirstLine = 2, fineFirstLine = 30;
inline constexpr QRgb dark = 0xff101010, light = 0xfffafafa, headerColor = 0xff205080, footerColor = 0xff806020;

// The 16 cells of one line of one row, bit 15 the leftmost cell.
inline uint16_t pattern(int row, int line) {
    if (line < indexFirstLine) return 0xffff;
    if (line < fineFirstLine) return uint16_t(row);
    uint32_t x = uint32_t(row) * 2654435761u + uint32_t(line) * 40503u + 0x9e3779b9u;
    x ^= x >> 15; x *= 0x2c1b3c6du; x ^= x >> 12; x *= 0x297a2d39u; x ^= x >> 15;
    // Never all one colour: a flat line is no evidence for the stitcher.
    const uint16_t bits = uint16_t(x);
    return bits == 0 || bits == 0xffff ? uint16_t(0x5a5a ^ line) : bits;
}
// The code area of a page `width` pixels wide: where it starts and how wide a cell is.
inline int codeLeft() { return labelWidth; }
inline int cellWidth(int width) { return (width - labelWidth - scrollbarWidth) / cells; }

struct Verdict {
    bool ok = false;
    int rows = 0;        // whole rows decoded
    int firstBadLine = -1;
    QString problem;
};
// Checks an image of the page: an optional sticky header of `header` lines on
// top and footer of `footer` lines at the bottom, and between them the lines
// of rows firstRow.. in order, starting at line `firstLine` of that row, down
// to the last line of row lastRow. Every line is decoded at every cell centre.
inline Verdict verify(const QImage& image, int header, int footer, int firstRow, int lastRow, int firstLine = 0, int lastLine = rowHeight - 1) {
    Verdict verdict;
    const int width = image.width(), cell = cellWidth(width);
    const int expected = header + footer + (lastRow - firstRow) * rowHeight + (lastLine + 1) - firstLine;
    if (cell < 4) { verdict.problem = QStringLiteral("the image is too narrow to decode"); return verdict; }
    if (image.height() != expected) { verdict.problem = QStringLiteral("height %1, expected %2").arg(image.height()).arg(expected); verdict.firstBadLine = std::min(image.height(), expected); }
    auto isDark = [&](int x, int y) { return qGray(image.pixel(x, y)) < 128; };
    const int body = image.height() - header - footer;
    for (int y = 0; y < image.height(); ++y) {
        const int x0 = codeLeft() + cell / 2;
        if (y < header || y >= image.height() - footer) {
            const QRgb want = y < header ? headerColor : footerColor;
            for (int c = 0; c < cells; ++c) {
                const QRgb got = image.pixel(x0 + c * cell, y);
                if (qAbs(qRed(got) - qRed(want)) > 8 || qAbs(qGreen(got) - qGreen(want)) > 8 || qAbs(qBlue(got) - qBlue(want)) > 8) {
                    if (verdict.problem.isEmpty() || verdict.firstBadLine > y) { verdict.problem = QStringLiteral("line %1 is not the sticky %2").arg(y).arg(y < header ? "header" : "footer"); verdict.firstBadLine = y; }
                    return verdict;
                }
            }
            continue;
        }
        const int position = firstLine + (y - header);
        const int row = firstRow + position / rowHeight, line = position % rowHeight;
        uint16_t got = 0;
        for (int c = 0; c < cells; ++c) got = uint16_t((got << 1) | (isDark(x0 + c * cell, y) ? 1 : 0));
        if (got != pattern(row, line)) {
            verdict.problem = QStringLiteral("output line %1 should be row %2 line %3 (cells %4) and reads %5").arg(y).arg(row).arg(line).arg(pattern(row, line), 4, 16, QLatin1Char('0')).arg(got, 4, 16, QLatin1Char('0'));
            verdict.firstBadLine = y;
            return verdict;
        }
    }
    verdict.rows = body / rowHeight;
    verdict.ok = verdict.problem.isEmpty();
    return verdict;
}
}
