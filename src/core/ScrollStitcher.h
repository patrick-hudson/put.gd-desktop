// Scrolling-screenshot stitcher: offset finder and assembler.
// Dependency-free C++20. Pixels are 4 bytes, B,G,R,X (QImage::Format_RGB32 on
// little-endian). Only vertical scrolling; content moves by whole pixels.
#pragma once

#include <climits>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace putgd::scroll {

struct FrameView {
  const uint8_t* data = nullptr;
  int width = 0;
  int height = 0;
  std::ptrdiff_t stride = 0;  // bytes per row, >= 4 * width
  const uint8_t* row(int y) const { return data + static_cast<std::ptrdiff_t>(y) * stride; }
};

struct StitchOptions {
  double sideMarginFraction = 0.04;  // columns ignored at left/right (scrollbars, shadows)
  int bands = 8;                     // column bands hashed independently per row (1..32)
  int minBandsEqual = 6;             // tolerance: at most (bands - minBandsEqual) bands may differ
  int minOverlapRows = 24;           // evidence rows required in the overlap
  double minScore = 0.90;            // matched / (matched + conflicting) evidence rows
  double uniquenessMargin = 0.05;    // best must beat any rival outside +-1 by this
  int maxOutputRows = 32000;         // final image height limit (header + body + footer)
  // Added by the prototype (see notes):
  int edgeThreshold = 48;            // |dR+dG+dB| between horizontal neighbours that is an "edge"
  int flatConflictDelta = 96;        // two flat rows conflict if their colours differ by more (sum |d|)
  int noMoveMaxChangedRows = 2;      // same-y rows failing the loose rule tolerated for NoMovement
  int noMoveMaxStrictRows = -1;      // same-y rows changed at all tolerated; -1 = max(48, height/10)
  double hintTolerance = 0.25;       // an unproved candidate is taken within max(px, frac*hint) of the hint
  int hintTolerancePx = 8;
  double tieHintTolerance = 0.08;    // among tied candidates the hint decides only within max(px, spread + frac*hint)
  // Candidates tied under the tolerant rule (lines of text that differ in a
  // few characters): one wins when at least exactTieMinFraction of its
  // informative rows are equal bit for bit and no rival outside +-1 has more
  // than exactTieMaxRival of its own.
  double exactTieMinFraction = 0.80;
  double exactTieMaxRival = 0.50;
  // The margin rule (see ScrollStitcher::marginVerdict): a rival is disproved
  // only by at least this many informative margin rows.
  int marginRivalMinRows = 8;
  // What the pixels must show before an offset counts as proved (not merely
  // consistent). Matched rows that are all alike prove nothing however many
  // there are (24 rows of one stripe pattern match any other 24), so the
  // matched rows must hold this many different ones.
  int minDistinctRows = 24;
  // And an overlap that is a sliver at the far end of the search range is what
  // a jump past the whole viewport leaves by coincidence: the loop never plans
  // a step that keeps less than a quarter of the band, so a candidate whose
  // overlap is under max(sliverMinRows, band * sliverFraction) rows is proof
  // of nothing by itself. (Both rules: lab guests of 2026-11-10, where a page
  // that had moved 975 px in an 800 px viewport was matched on 25 rows at 775
  // and rows went missing from an image reported as complete.)
  int sliverMinRows = 36;
  double sliverFraction = 0.125;
  // The layout rule (see ScrollStitcher::columnsChanged and barVerdict). A
  // band of columns counts as steady while at most
  // layoutSteadyFraction of the overlap's rows are neither where the page
  // moved them nor where they were; a steady band with at least
  // layoutBrokenFraction of such rows in one step was repainted as a whole.
  // And a run of rows that newly stays put at the bottom of the frame is a bar
  // over the page, not the page, when at least layoutStaticMinRows of the rows
  // that should have scrolled out of it contradict it and they outnumber the
  // rows that did come out of it by more than layoutStaticRatio to one.
  double layoutSteadyFraction = 0.10;
  double layoutBrokenFraction = 0.90;
  int layoutStaticMinRows = 4;
  int layoutStaticRatio = 3;
  // The frame's edges are where things lie over the page that are not the
  // page: a browser's link status panel, a window border blended into the
  // last row, a button that floats over the bottom corner. Two rules keep
  // them out of the image (live web pages, acceptance of 2026-11-12).
  // The last edgeGuardFraction of the moving band (at least edgeGuardMinRows
  // rows, at most a quarter of it) is not written out when its frame is
  // stitched: the next frame shows the same rows higher up, away from the
  // edge, and they are taken from there; only rows the next frame no longer
  // shows inside its interior are written as they were first seen. 0: off.
  double edgeGuardFraction = 0.08;
  int edgeGuardMinRows = 24;
  // The interior of a frame, for those rows and for a bar that has just come
  // up at the top (see ScrollStitcher::addFrame): the moving band without
  // its first edgeTopFraction.
  double edgeTopFraction = 0.25;
  // A part of the frame that scrolls at a pace of its own (a code editor's
  // minimap, a column with a parallax effect) cannot be in one picture with
  // the page. It is proved per quarter of a band: at least
  // layoutBrokenFraction of the rows equal bit for bit at one other distance
  // (minDistinctRows different ones among them), fewer than ownPaceMaxWith at
  // the page's distance and where they were, and that in two steps in a row.
  double ownPaceMaxWith = 0.50;
  // The search by quarter bands (ScrollStitcher::columnSearch), for a page
  // between side bars: how far apart in brightness (the mean of the channels'
  // sum, in sixteenths: 96 is 6 of 765) two quarters of a row may be and
  // still be the same line drawn again.
  int columnLightTolerance = 96;
  // A quarter whose lines are too short to prove a distance names one when at
  // that distance at least this share of the rows in which it shows something
  // are equal bit for bit (and at no other more than exactTieMaxRival).
  double columnMovedFraction = 0.70;
  // A bar that comes up at the bottom late (ScrollStitcher::lateBar) becomes
  // the footer when every row can be accounted for, instead of ending the
  // capture as a change of layout.
  bool lateBarIsFooter = false;
};

// Quarters of a band: the columns the own-pace rule looks at.
inline constexpr int kFinePerBand = 4;

// Per-row signatures of one frame. This is the only per-frame state the
// stitcher keeps (for the reference frame).
class FrameSignatures {
 public:
  void compute(const FrameView& frame, const StitchOptions& opts);

  int width = 0, height = 0, bands = 0;
  int colStart = 0, colEnd = 0;           // compared columns [colStart, colEnd)
  std::vector<uint64_t> bandHash;          // height * bands, exact pixel hash per band
  std::vector<uint64_t> edgeHash;          // height * bands, hash of edge x positions per band
  std::vector<uint64_t> rowHash;           // combination of all band hashes
  std::vector<uint64_t> sideHash;          // height * 2: hash of the ignored left and right margin columns
  std::vector<uint32_t> flatBandMask;      // bit k: band k is one colour
  std::vector<uint32_t> edgeBandMask;      // bit k: band k has at least one edge
  std::vector<uint32_t> flatColor;         // colour of a flat row
  std::vector<uint8_t> rowFlat;            // whole compared row is one colour
  std::vector<uint8_t> sideFlat;           // bit 0: the left margin is one colour (or empty); bit 1: the right
};

// Same-y comparison used for settle, NoMovement and fixed header/footer runs:
// rows are "the same" when at most (bands - minBandsEqual) bands differ.
bool sameRowLoose(const FrameSignatures& a, int ya, const FrameSignatures& b, int yb,
                  const StitchOptions& opts);
// Number of rows in [y0, y1) that are not sameRowLoose at the same y.
int countChangedRows(const FrameSignatures& a, const FrameSignatures& b, int y0, int y1,
                     const StitchOptions& opts);
bool marginsChanged(const FrameSignatures& a, const FrameSignatures& b);
// Rows in [y0, y1) whose exact row hash differs.
int countStrictChangedRows(const FrameSignatures& a, const FrameSignatures& b, int y0, int y1);
int strictChangeBudget(const StitchOptions& o, int height);
// The "did anything move" rule shared by NoMovement and settle detection: more
// than maxLooseChangedRows rows fail the loose rule, or more than the strict
// budget rows changed at all, or any margin (scrollbar) row changed.
bool framesDiffer(const FrameSignatures& a, const FrameSignatures& b, int y0, int y1,
                  const StitchOptions& o, int maxLooseChangedRows);
// Pair classification used by the offset search: +1 match (evidence for),
// 0 conflict (evidence against), -1 no evidence, -2 two flat rows of clearly
// different colour (not informative, so not scored, but the candidate is not
// conflict-free).
int classifyRowPair(const FrameSignatures& prev, int yPrev, const FrameSignatures& cur, int yCur,
                    const StitchOptions& opts);

enum class StepStatus {
  First,         // first frame stored
  Appended,      // offset > 0 found, rows appended (appendedRows may be 0 if all were already emitted)
  NoMovement,    // frame identical (or only changed in place); nothing appended
  Reversed,      // offset < 0 (content moved back, e.g. elastic bounce); nothing appended, tracking kept
  NoMatch,       // no offset explains the frame; output and reference untouched
  Ambiguous,     // several offsets explain the frame equally well; output and reference untouched
  LimitReached,  // maxOutputRows reached; output truncated at the limit
  InvalidFrame,  // size changed, null data, or finish() already called
  LayoutChanged, // the page's place inside the frame changed (the layout rule), or a part of the frame scrolls at its own pace; the output ends with the rows from before, and no later frame is taken
};
const char* toString(StepStatus);

struct StepResult {
  StepStatus status = StepStatus::NoMatch;
  int offset = 0;          // content scroll distance in px (prev[y + offset] == cur[y])
  double score = 0.0;      // score of the chosen (or best rejected) candidate
  int appendedRows = 0;
  int fixedTop = 0;        // current sticky header estimate (rows)
  int fixedBottom = 0;     // current sticky footer estimate (rows)
  bool lowConfidence = false;  // chosen via the hint among tied candidates, or hint accepted without evidence
  int evidenceRows = 0;    // matched + conflicting rows of the chosen candidate
  int changedRows = 0;     // same-y changed rows versus the reference frame
  int candidatesEvaluated = 0;
  bool ownPace = false;    // LayoutChanged because a part of the frame scrolls at a pace of its own
};

// Receives each output row once, top to bottom. Rows are never revised.
using RowSink = std::function<void(const uint8_t* bgrxRow, int width)>;
// The one exception: the layout rules (ScrollStitcher::barVerdict, ownPace)
// take back the last `rows` rows the sink was given, before any row after them. A sink
// that comes without it keeps those rows; its image is then not cut back.
using RowTakeBack = std::function<void(long long rows)>;

inline constexpr int kNoHint = -1;  // any negative hint means "unknown"

class ScrollStitcher {
 public:
  explicit ScrollStitcher(StitchOptions opts = {}, RowSink sink = {}, RowTakeBack takeBack = {});

  // hint: expected offset in px (>= 0), or negative for unknown.
  StepResult addFrame(const FrameView& settledFrame, int expectedOffsetHint = kNoHint);
  // Same, with signatures the caller already computed for this frame (the session
  // computes them for settle detection anyway).
  // hintUncertaintyPx: how far the caller's hint may be off (e.g. notches times
  // the observed spread of px-per-notch). Tied candidates (periodic content) are
  // resolved by the hint only when every rival is clearly outside that range.
  StepResult addFrame(const FrameView& settledFrame, const FrameSignatures& sig,
                      int expectedOffsetHint, int hintUncertaintyPx = 0);

  // Emits the remaining rows (first frame if nothing moved, then the footer).
  void finish();
  // A new scroll target before any body was emitted: discard only the pending viewport.
  void restartBeforeMovement();

  int outputHeight() const;  // rows emitted so far + held + pending + footer
  // Moving band of the reference frame, [top, bottom) in frame rows.
  int movingBandTop() const { return estTop_ > 0 ? estTop_ : 0; }
  int movingBandBottom() const { return height_ - (estBot_ > 0 ? estBot_ : 0); }
  bool hasFrame() const { return hasRef_; }
  // Largest scroll distance that keeps >= 1.5 * minOverlapRows non-flat rows of
  // the reference frame inside the overlap, or -1 when the reference has fewer
  // (a blind, hint-only step is then unavoidable).
  int maxEvidenceStep() const;
  // The pointer rests in the rectangle while frames are read (it has no place
  // outside): rows of a frame above this one may show what the page does
  // under it and are not taken in place of rows that wait (the edge guard).
  void setTrustedFrom(int row) { trustedFrom_ = std::max(0, row); }
  // The column of the frame the wheel is turned over (the pointer's place):
  // what scrolls there is the page (see columnSearch). Unset: the middle.
  void setWheelColumn(int x) { wheelColumn_ = x; }
  const StitchOptions& options() const { return opts_; }

  // Internal-buffer mode only (no sink): the image, valid after finish().
  const std::vector<uint8_t>& image() const { return image_; }
  std::vector<uint8_t> takeImage() { return std::move(image_); }

 private:
  struct Search {
    StepStatus status = StepStatus::NoMatch;
    int d = 0;
    double score = 0;
    bool lowConfidence = false;
    int evidence = 0;
    int evaluated = 0;
    std::vector<int> kept;  // columnSearch: the quarter bands the rows were compared over
  };
  Search search(const FrameSignatures& cur, int top, int bottom, int hint, int unc) const;
  Search locate(const FrameSignatures& cur, int top, int bottom, int hint, int unc) const;
  Search columnSearch(const FrameSignatures& cur, int top, int bottom) const;
  bool contradicted(int top, int bottom, int d) const;
  int marginVerdict(const FrameSignatures& cur, int top, int bottom, const std::vector<int>& candidates, bool exact) const;
  bool columnsChanged(const FrameSignatures& cur, int top, int bottom, int d, uint32_t& steady, uint32_t own) const;
  enum class Bar { Is, IsNot, Open };
  Bar barVerdict(const FrameSignatures& cur, int top, int bot, int stayed, const Search& how) const;
  bool headerVerdict(const FrameSignatures& cur, int top, int run, int bot, const Search& how) const;
  bool lateBar(const FrameSignatures& cur, int top, int bot, int stayed, int d);
  int rowPair(const Search& how, const FrameSignatures& cur, int yRef, int yCur) const;
  void paceOf(int top, int bottom, int d, std::vector<uint8_t>& astray) const;
  long long ownPace(const std::vector<uint8_t>& astray, long long before);
  void fineHashes(const FrameView& f, const FrameSignatures& sig, std::vector<uint64_t>& out, std::vector<uint16_t>& light) const;
  int guardRows(int band) const;
  void holdRows(const uint8_t* rows, std::ptrdiff_t stride, int count);
  void emitHeld(int count);
  long long logicalRows() const { return bodyRows_ + heldRows_; }
  struct Shadow {
    std::vector<uint8_t> pixels;
    long long start = 0;
    int count = 0;
    void save(const std::vector<uint8_t>& held, int heldRows, long long bodyRows, int width);
  };
  bool cutBackTo(long long rows, const Shadow& shadow);
  bool takeBackTo(long long rows);
  StepResult layoutChanged(StepResult r, bool takeBack);
  void emitRows(const FrameView& f, int y0, int y1);
  void emitRaw(const uint8_t* row);
  void storeFooter(const FrameView& f);
  void flushPending(int rows);

  StitchOptions opts_;
  RowSink sink_;
  RowTakeBack takeBack_;
  std::vector<uint8_t> image_;     // internal mode output
  std::vector<uint8_t> pending_;   // copy of the first frame until the first movement
  std::vector<uint8_t> footer_;    // bottom rows of the reference frame (>= estBot_ rows)
  int footerRows_ = 0;
  // The edge guard: body rows bodyRows_ .. bodyRows_ + heldRows_, seen at the
  // bottom of the reference frame and not written out yet.
  std::vector<uint8_t> held_;
  int heldRows_ = 0;
  int trustedFrom_ = 0, wheelColumn_ = -1;
  // The last step that appended: how far the page moved, and how many rows were out before it.
  int lastStep_ = 0;
  long long lastBefore_ = -1;
  FrameSignatures ref_;
  bool hasRef_ = false, moved_ = false, limited_ = false, finished_ = false;
  int width_ = 0, height_ = 0;
  long long refOrigin_ = 0;        // body row of reference frame row 0
  long long bodyRows_ = 0;         // body rows emitted (or pending) so far; held rows come on top
  int estTop_ = -1, estBot_ = -1;  // fixed header/footer estimates, -1 unknown
  // The layout rule's memory: the bands every proved step so far left steady;
  // how many body rows were out before the last step that showed no new
  // static run at the bottom (rows up to there cannot be a bar's); and how
  // many the first frame gave, before the page had moved at all.
  uint32_t steadyBands_ = 0;
  bool haveSteady_ = false, changed_ = false;
  long long provedRows_ = 0, firstRows_ = -1;
  // The own-pace rule's memory, per quarter band: how many body rows were out
  // before the step that first showed it at a pace of its own, -1 when the
  // last step did not.
  std::vector<long long> paceSince_;
  bool paceBegan_ = false;
  // The rows that waited when the step began that provedRows_ names, and when
  // the earliest own-pace run began (see cutBackTo).
  Shadow proved_, pace_;
  // Exact hashes per quarter of a band and row (see fineHashes): the reference frame's and this frame's.
  std::vector<uint64_t> refFine_, curFine_;
  std::vector<uint16_t> refLight_, curLight_;
};

}  // namespace putgd::scroll
