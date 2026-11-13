#pragma once
#include "Layout.h"
#include "OcrImage.h"
#include "TextEngine.h"

// From one band's engine output to the job's words, lines and copied text.
namespace putgd::ocr {
// The text for copying drops words a person would not want or could not
// read (#1428 decision 4): under the confidence floor, lone symbols an icon
// produced, glyphs under a minimum height and text whose contrast against its
// own background is under a floor. The candidates keep every word.
struct CopyFilter {
 float confidence=50;         // Tesseract's word confidence, 0..100
 float symbolConfidence=85;   // a word of one symbol needs this, and must be a code symbol
 int minimumLineHeight=5;     // its line's box, source pixels
 int contrast=32;             // grey levels a glyph pixel differs from the word's background
};
// Appends a band's words and lines in source pixels. At a seam (a band cut
// with no blank row, so bands overlap) words touching the cut edges are
// dropped, and a word found in both bands is kept once, the one with the
// higher confidence, in the earlier band's place. Returns the index of this
// band's first word.
int mergeBand(Recognition& job,const BandText& band,const Band& placement,const Band* previous,int previousFirstWord,
               double scale,int border,int bandIndex,int imageWidth,int imageHeight);
void applyCopyFilter(Recognition& job,const GreyView& source,const CopyFilter& filter);
// Kept words joined by spaces, lines by newlines, with a blank line between blocks.
QString copiedText(const Recognition& job);
}
