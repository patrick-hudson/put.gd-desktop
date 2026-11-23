#pragma once
#include "OcrImage.h"
#include "core/TextJobProtocol.h"
#include <QList>
#include <QString>
#include <algorithm>
#include <utility>

// Scale and band planning, and the band copies Tesseract reads (#1428 decision 3).
// Pure image arithmetic: no engine, no allocation beyond one band at a time.
namespace putgd::ocr {
// Admission: the editor's own limit.
using textjob::maximumInputPixels;
// One pass or one band never exceeds this after scaling, overlap included.
inline constexpr qint64 bandPixelLimit=8000000;

struct Profile {
 int background=255;     // estimated background grey
 bool dark=false;        // light text on a dark background
 int lineHeight=0;       // median height of the ink row runs; 0 when none
 QList<int> rowInk;      // ink pixels per row, code areas excluded
 // Faint ink per row (#1894): edges at least faintContrast from the background
 // that stay on the background's side of the Otsu threshold, which Tesseract's
 // own Otsu binarization then drops (comment grey on a dark editor beside
 // brighter text). Code areas excluded.
 QList<int> faintInk;
 bool blank(int y,int width)const{return rowInk.at(y)<=std::max(2,width/256);}
};
// Background, polarity, ink per row and the text height, from the whole image.
// Pixels inside the codes' bounding boxes do not count.
Profile measure(const GreyView& image,const QList<Quad>& codes);

struct Band {
 int top=0,bottom=0;     // source rows [top, bottom)
 bool seamAbove=false;   // starts inside the band before it (no gap was found)
};
struct Plan {
 double scale=1;
 QList<Band> bands;      // one band means a single pass
 QString error;          // input_excessive
 bool banded()const{return bands.size()>1;}
};
struct PlanLimits {
 // The whole job's scaled pixels. Measured on m74 (i5-6500T, Tesseract 5.3.4):
 // above this a scale over 1x costs more than the deadline and the speed bars
 // allow (a 4K UI page took 20 s at 1.7x and 3 s at 1x).
 qint64 scaledPixels=8000000;
 // The same for an input of at least largeInputPixels (a whole screen of 4K or
 // more), which is also the only input cut into panels; each panel gets the
 // scale its text asks for within this.
 qint64 largeScaledPixels=16000000;
 qint64 largeInputPixels=4000000;
 qint64 singlePassPixels=bandPixelLimit; // up to this the image is recognized whole
 // A banded image is cut into bands of about this size, or eight line heights
 // when that is more, never over bandPixelLimit. 0 picks it from the threads:
 // the scaled image over twice the threads, between 1 and 2 MP, so every
 // thread has bands to take. Tesseract's layout time grows faster than a
 // band's size: on m74 a 24 MP page took 17.5 s in 8 MP bands and 8.8 s in
 // 2 MP bands, and a dense 4K page 6.0 s in 2 MP bands and 3.6 s in 1 MP ones.
 qint64 bandPixels=0;
 int threads=1;
 int border=0;                  // background pixels added on every side of a band, scaled
 int maximumScale=3;
 double forcedScale=0;           // method comparisons only: this scale, before the cap
};
// The scale chosen from the text height, then limited by the job's scaled-pixel
// cap; and the bands: cuts at the last blank row gap in a band's final quarter,
// otherwise at the limit with the next band starting two line heights earlier.
Plan plan(int width,int height,const Profile& profile,const PlanLimits& limits);
// The integer scale the text height asks for, before the cap.
int preferredScale(int lineHeight,int maximumScale);

// Cuts rows [0, height) into about `parts` strips between text lines only:
// each cut is the middle of the blank run nearest its even share, searched
// within half a share either way; with no blank row there, the row with the
// least ink within one line pitch (the profile's period) of the share, when it
// holds at most a quarter of the ink around it; otherwise that cut is skipped.
// Strips are at least four line heights. One strip means no cut was possible.
QList<Band> strips(const Profile& profile,int width,int height,int parts);

// The panels of a screen: windows, columns and bars, each read on its own and
// in reading order (a cut across the page reads the top part first, a cut
// down it the left part first). A classic XY-cut on text-like activity: grey
// steps of at least 32 levels, kept on a 4x4 px grid, with long straight runs
// (window borders, rules, underlines) left out so a border inside a gap does
// not hide the gap. A cut down the page needs a gutter of at least three line
// heights with activity on both sides, which a word gap never is; a cut across
// it is kept only when one of its parts is then cut down. A single-window or
// single-column image is exactly one panel, the whole image. With several,
// each is trimmed to its activity plus a margin, and an empty one is dropped.
QList<QRect> panels(const GreyView& image,const QList<Quad>& codes,int lineHeight);

// #1894: the rows that hold only faint ink, as bands to read again with their
// contrast stretched: runs of rows with faint ink and no ink at the threshold,
// at least half a line height (three rows) and at most three line heights
// tall, padded by up to two rows that hold no ink at the threshold, at most
// `limit` of them, top to bottom.
QList<Band> faintLines(const Profile& profile,int width,int height,int limit=8);
// #1912: a trimmed panel grown through its window's plain background: each
// side moves out while at least a quarter of the next row or column, across
// the area so far, is within `tolerance` of `background` and, on a line of 8 px
// or more, each of its quarters holds such pixels for at least 1/32 of the
// line's length (a line of text leaves that much paper; a desktop or frame of
// another shade leaves none, and a stripe of the window's shade across it
// leaves quarters empty),
// and it meets none of `others` (the other panels and the areas already grown). A faint line has no grey step of 32 levels, so trimming
// leaves it out; the growth takes it back in, and stops at a window's edge,
// where a desktop or frame of another shade would be counted as ink.
QRect plainAround(const GreyView& image,const QRect& from,const QList<QRect>& others,int background,int tolerance=6);
// #1912: in a panel grown through its window, the rows outside the rows the
// panel was read in [readTop, readBottom) that hold ink or faint ink: text the
// first pass never saw. Over a whole window of plain background, Otsu's
// threshold can fall below a long faint line, which then counts as ink and
// not as faint, so faintLines() alone misses it. Runs of half a line to three
// lines, padded by up to two rows short of the read rows, at most `limit`.
QList<Band> unreadLines(const Profile& profile,int width,int height,int readTop,int readBottom,int limit=8);
// Stretches a band copy's contrast so its darkest pixel is black and the
// background white: faint ink becomes ink Otsu keeps.
void stretchContrast(GreyImage& band,int background);

// #1911: a word recognition was unsure of that looks like a key or token: at
// least tokenLength characters once quotes and brackets are trimmed, letters and
// digits both, below tokenConfidence. One misread character breaks such a token
// for Quick redact, and a second, different read of its line is a second chance
// (on the 10k secret corpus it covered 83 more of 1,933 secrets; on 176 ordinary
// captures the test held on 8, 0.3% of their rows).
inline constexpr int tokenLength=12;
inline constexpr float tokenConfidence=70;
bool uncertainToken(const QString& text,float confidence);
// The rows to read again around such words: each word's box grown by a third
// of its height and one row, joined where they overlap while the band stays
// within four of its words' heights (a dense page is many short bands, not one
// band of the page), inside [0, height). The least confident band first, so a
// read cut short by time keeps the most doubtful lines. Whole rows, not only
// the words: Tesseract reads a token in its line better (on the corpus, word
// areas alone covered 47 more secrets against 96 for rows).
struct TokenWord { QRect rect;float confidence=0; };
// confidence: its least confident word's; [left, right): the columns its words span.
struct TokenBand { Band band;float confidence=0;int left=0,right=0;int tallest=0; };
QList<TokenBand> tokenLines(const QList<TokenWord>& words,int height);
// The columns of a token band to read again (#1929): its words' columns grown by
// `margin`, then outward to the nearest gap of `gap` columns holding no ink (no
// pixel `contrast` or more from the background) in the band's rows, so the read
// keeps the words' run of text and leaves out blank screen and other columns of
// text. A full-width row of a 4K terminal was 5,334 px wide after scaling, more
// than half of it blank. Within [0, source.width).
std::pair<int,int> tokenColumns(const GreyView& source,const Band& band,int left,int right,int background,int margin,int gap,int contrast=12);
// All second reads of a job together (faint lines, text a grown panel never
// read, uncertain tokens) take at most secondReadShare of the first pass's
// time, never under secondReadFloorMs nor over secondReadCapMs, on every
// engine the job has (#1929: capped at the first pass's time on one engine,
// they doubled a dense 4K check on a 4-core laptop, 5.2 s to 10.3 s). They end
// secondReadMarginMs before the job's own budget at the latest, and never mark
// a check truncated: what the first pass read stands either way.
// A token band's columns: its words' grown by tokenMargin of their height,
// then out to a gap of tokenGap of it (between words; #1929).
inline constexpr double tokenMargin=4,tokenGap=0.6;
inline constexpr double secondReadShare=0.25;
inline constexpr int secondReadFloorMs=400;
inline constexpr int secondReadCapMs=1000;
inline constexpr int secondReadMarginMs=1000;
// A band copy for that second read: every pixel's distance from its local
// background (the median of the 80 px around it), stretched so the strongest
// ink nearby is black, on white. It reads dark and light, faint and coloured
// backgrounds alike; scaled and bordered like bandImage.
GreyImage contrastBand(const GreyView& source,const Band& band,double scale,int border,const QList<Quad>& codes);

// A band of the source, scaled, with every code polygon filled with the
// background, inverted when the page is dark, and with the border added.
GreyImage bandImage(const GreyView& source,const Band& band,double scale,int border,
                    const QList<Quad>& codes,int background,bool invert);
}
