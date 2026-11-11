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
  double hintTolerance = 0.25;       // hint tie-break accepts a candidate within max(px, frac*hint)
  int hintTolerancePx = 8;
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
};

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

  size_t bytes() const;
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
};

// Receives each final output row once, top to bottom. Rows are never revised.
using RowSink = std::function<void(const uint8_t* bgrxRow, int width)>;

inline constexpr int kNoHint = -1;  // any negative hint means "unknown"

class ScrollStitcher {
 public:
  explicit ScrollStitcher(StitchOptions opts = {}, RowSink sink = {});

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

  int outputWidth() const { return width_; }
  int outputHeight() const;  // rows emitted so far + pending + footer
  // Moving band of the reference frame, [top, bottom) in frame rows.
  int movingBandTop() const { return estTop_ > 0 ? estTop_ : 0; }
  int movingBandBottom() const { return height_ - (estBot_ > 0 ? estBot_ : 0); }
  bool hasFrame() const { return hasRef_; }
  // Largest scroll distance that keeps >= 1.5 * minOverlapRows non-flat rows of
  // the reference frame inside the overlap, or -1 when the reference has fewer
  // (a blind, hint-only step is then unavoidable).
  int maxEvidenceStep() const;
  bool limitReached() const { return limited_; }
  const StitchOptions& options() const { return opts_; }

  // Internal-buffer mode only (no sink): the image, valid after finish().
  const std::vector<uint8_t>& image() const { return image_; }
  std::vector<uint8_t> takeImage() { return std::move(image_); }

  // Peak bytes held by the stitcher right now (excluding the caller's frames).
  size_t memoryBytes() const;

 private:
  struct Search {
    StepStatus status = StepStatus::NoMatch;
    int d = 0;
    double score = 0;
    bool lowConfidence = false;
    int evidence = 0;
    int evaluated = 0;
  };
  Search search(const FrameSignatures& cur, int top, int bottom, int hint, int unc) const;
  int marginVerdict(const FrameSignatures& cur, int top, int bottom, const std::vector<int>& candidates) const;
  void emitRows(const FrameView& f, int y0, int y1);
  void emitRaw(const uint8_t* row);
  void storeFooter(const FrameView& f);
  void flushPending(int rows);

  StitchOptions opts_;
  RowSink sink_;
  std::vector<uint8_t> image_;     // internal mode output
  std::vector<uint8_t> pending_;   // copy of the first frame until the first movement
  std::vector<uint8_t> footer_;    // bottom rows of the reference frame (>= estBot_ rows)
  int footerRows_ = 0;
  FrameSignatures ref_;
  bool hasRef_ = false, moved_ = false, limited_ = false, finished_ = false;
  int width_ = 0, height_ = 0;
  long long refOrigin_ = 0;        // body row of reference frame row 0
  long long bodyRows_ = 0;         // body rows emitted (or pending) so far
  int estTop_ = -1, estBot_ = -1;  // fixed header/footer estimates, -1 unknown
};

}  // namespace putgd::scroll
