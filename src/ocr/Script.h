#pragma once
#include "OcrImage.h"
#include <QList>
#include <QRect>
#include <QString>
#include <QStringList>

// #1981: a page in a script the job's languages can't read (Chinese, Russian,
// Arabic on an English system) comes out of the first pass as Latin garbage.
// The garbled lines are found from that pass for free, their script is
// detected on a composite of only those lines (Tesseract's OSD; on the whole
// screen it was wrong or gave nothing on real pages), they are read again
// with [that script's language, eng], and the second reading replaces the
// first only when its confidence is clearly higher. Engine-free: the helper's
// unit tests reach all of it.
namespace putgd::ocr {
// A segment is garbled when its words' mean confidence is under this...
inline constexpr float garbledConfidence=50;
// ...and at least this many of them hold an ASCII letter (the garbage an
// English reading makes of another script).
inline constexpr int garbledLetteredWords=3;
// A job triggers when its garbled segments hold at least this share of its
// words, and there are at least two of them. The research sweep
// (#1975 pages): 86 of 90 non-Latin captures fire, 46 of 1,611 Latin ones.
inline constexpr double garbledShare=0.25;
inline constexpr int garbledSegmentsNeeded=2;
// The second reading is kept only when its mean confidence is at least this
// much higher than the first's on the same lines. Rereads in the right
// language rose 29 to 56 points; in a wrong one at most 12.
inline constexpr float keepRise=15;
// OSD reads at most this many of the garbled segments, the widest.
inline constexpr int detectionPieces=60;

// A run of words on one line, in reading order, cut where the gap to the
// next word is three line heights or more. first and last index the job's
// words, [first, last).
struct Segment { int line=0,first=0,last=0;QRect rect;float confidence=0;bool garbled=false; };
QList<Segment> lineSegments(const Recognition& job);
// The garbled segments when the job triggers, in reading order; empty otherwise.
QList<Segment> garbledSegments(const Recognition& job);
// The language data for a script OSD names, or empty: one language a script,
// the ones the AppImage carries and the packages recommend.
QString languageForScript(const QString& script);
bool keepReread(double before,double after);
// One segment's two readings: their words' confidence sums and counts.
// read: its group was read in time; a read with no words reads nothing.
struct SegmentReading { bool read=false;double before=0,after=0;int beforeCount=0,afterCount=0; };
// The garbled segments' verdict: over those that gave words in both readings
// only (one that read nothing keeps its first reading and does not count),
// the second reading is kept when keepReread holds; take marks which of
// `garbled` are replaced.
struct ScriptDecision { bool keep=false;double before=0,after=0;QList<bool> take; };
ScriptDecision decideGarbled(const QList<SegmentReading>& readings,const QList<int>& garbled);
// Another segment of a page whose language was kept: replaced when its
// second reading has words and a higher mean confidence.
bool takeOther(const SegmentReading& reading);

// A segment's box, grown by a third of its height (at least 4 px) on every
// side, within bounds: room for accents and descenders.
QRect paddedPiece(const QRect& rect,const QRect& bounds);
// Segments stacked one under another on white, each scaled and its polarity
// set like a band copy, with gap pixels between them.
// codes: the panel's code areas, in view pixels; they are filled with the background as in a band copy.
struct PieceSource { GreyView view;QRect rect;int background=255;bool invert=false;double scale=1;QList<Quad> codes={}; };
struct Composite { GreyImage image;QList<int> tops;QList<int> heights;QList<double> scales; };
Composite composite(const QList<PieceSource>& pieces,int gap);
// The pixels that composite allocates: its widest piece times all pieces and gaps.
qint64 compositePixels(const QList<PieceSource>& pieces,int gap);
// Which piece of a composite a box (composite pixels) belongs to: the one its
// centre falls in, or -1 in a gap.
int pieceAt(const Composite& composite,const QRect& box);
// A box of piece `index` (composite pixels) in its source's pixels.
QRect pieceToSource(const Composite& composite,int index,const PieceSource& piece,const QRect& box);

// No space between two words on a line when the characters that meet are of
// scripts written without spaces (Han, kana, Thai, CJK punctuation): Tesseract
// splits such text into words of a character or a few.
void joinUnspaced(Recognition& job);
}
