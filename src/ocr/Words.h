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
 // Tesseract's word confidence (0..100) does not separate icons from text:
 // on put.gd's own captures real words scored 13 to 35 and every URL 0, while
 // icon noise ("O1", "(A", "a)") scored 0 to 38. Shape does. So only words of
 // one or two characters need this confidence...
 float confidence=40;
 // ...a lone symbol must be a code symbol with this confidence...
 float symbolConfidence=85;
 // ...and a word of symbols alone must hold only code symbols ("&&", "+=", "{"),
 // never a dash, quote or sign an icon produced ("——", "(®)").
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
// Tesseract splits URLs after the scheme and at dots ("https: //example. test"),
// and addresses at a dot of their domain ("corpus@example. invalid"):
// joins "scheme:" with a following "//...", then continues a URL across a split
// after . / ? = & # - _ when the next word on the line starts with a lowercase
// letter or digit, or when the next word starts with / ? # & =, and the gap is
// under half the line height.
void joinUrls(Recognition& job);
void applyCopyFilter(Recognition& job,const GreyView& source,const CopyFilter& filter);
// Kept words joined by spaces, lines by newlines, with a blank line between
// blocks. lineTops gets the top edge of each line of the text, a blank line
// taking the top of the line after it.
QString copiedText(const Recognition& job,QList<int>* lineTops=nullptr);
}
