#pragma once
// The bar-coded ruler page's layout and line patterns (see ScrollRuler.h),
// without Qt: the tracker's unit test draws the same page.
#include <cstdint>

namespace putgd::test::ruler {
inline constexpr int rowHeight = 40, cells = 16, labelWidth = 64, scrollbarWidth = 12, indexFirstLine = 2, fineFirstLine = 30;
inline constexpr uint32_t dark = 0xff101010, light = 0xfffafafa, headerColor = 0xff205080, footerColor = 0xff806020;

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
}
