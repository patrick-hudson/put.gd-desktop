// Scrolling capture's matching engine (#1515): follows the picture of a region
// while its content scrolls, by whoever scrolls it, and composes the content
// into one page-space image. Dependency-free C++20, standard library only, so
// the same code runs in the tests without Qt. Pixels are 4 bytes, B,G,R,X.
//
// The band matcher (row signatures by brightness in column bands compared
// with a tolerance, candidates searched outward from the expected offset,
// motion mask, fixed header and footer, lookup of a frame inside the image
// already built, growth at both ends) is ported from wayscrollshot's
// src/stitch/band and mark-shot's crates/shot-stitch:
//
//   wayscrollshot, https://github.com/jswysnemc/wayscrollshot
//     commit 703474ad7b94a504306d7a93d21668ee162ebf1c, Copyright (c) 2025
//   mark-shot, https://github.com/jswysnemc/mark-shot
//     commit 4cf891014f584e9199e41a182c13f6f478435115, Copyright (c) 2026 Dhahbi Fadi
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy
//   of this software and associated documentation files (the "Software"), to deal
//   in the Software without restriction, including without limitation the rights
//   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//   copies of the Software, and to permit persons to whom the Software is
//   furnished to do so, subject to the following conditions:
//
//   The above copyright notice and this permission notice shall be included in all
//   copies or substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//   SOFTWARE.
//
// What is put.gd's own (MIT, put.gd): pinned parts as blocks of the frame
// rather than rows and columns; the frame's layers (the page, and what moves
// otherwise: an inner scroll area, a side bar on its way into place), told
// apart by tiles and columns rather than by how much text each shows; two
// views of a cell before it counts as the page's, and a cell seen once
// written only after the next pair could find it pinned; the composition of
// what is pinned once; the check of an offset cell by cell; and the canvas.
//
// One frame, in order: signatures (the mean of R+2G+B per cell of 8 columns
// and per voting band, and where they are textured); still? (the frame shows
// what the last placed frame showed: no movement); the search (every offset
// that leaves more than a sliver of overlap, coarsely on up to 64 textured
// rows, then each basin on every row, then cells; a second look with the best
// layer's tiles left out when much of the frame did something else); the
// decision (the page's layer, then the best by cells, then the expected
// offset among exact repeats); the check against the image; else a lookup in
// the image; else skipped. A placed frame teaches the pinned mask and is
// composed.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "core/ScrollStitcher.h"  // FrameView

namespace putgd::scroll {

struct TrackerOptions {
  // Columns left out of the vote at each side (scrollbars, shadows). They are
  // still composed like the rest.
  double sideMarginFraction = 0.04;
  // The composition unit: a cell is this many columns of one row. The pinned
  // mask is kept in blocks of blockRows such rows.
  int cellPx = 8;
  int blockRows = 8;
  // Voting bands across the compared columns (at most 64).
  int bands = 32;
  // Tolerances, in sixteenths of R+2G+B (one grey level of all three
  // channels is 64): two cells or bands agree within *Tolerance; a cell or
  // band is textured (it differs from the row above or below) by *Texture.
  // The band tolerance grows for narrow bands (it is the cell's scaled by
  // the square root of their widths, at least bandTolerance); "stayed" is
  // judged strictly, within an eighth of the cell tolerance.
  int bandTolerance = 160;
  int bandTexture = 192;
  int cellTolerance = 480;
  int cellTexture = 640;
  // An overlap is evidence only when it is no sliver: at least this many rows
  // and this share of the frame.
  int minOverlapRows = 48;
  double minOverlapFraction = 0.08;
  // Rows that hold agreeing textured cells, at the least.
  int minEvidenceRows = 16;
  // Within the tiles that move with the offset, the share of informative
  // lanes (textured, not pinned, not standing still) that must agree, by
  // bands and by cells. Cells mainly rank the candidates: between
  // neighbouring offsets, and between lines that differ in a few characters.
  double acceptBands = 0.85;
  double acceptCells = 0.50;
  // The tiles (a band by 32 rows) that move with an offset must be at least
  // this share of the tiles that show something: the page is the largest
  // layer. A side bar that sticks, a strip at a pace of its own, an inner
  // scroll area are in the others.
  double minArea = 0.20;
  // The check against the image already built: the frame placed by the pair
  // contradicts the image when fewer than this share of its tiles that show
  // something agree with it.
  double keepShare = 0.40;
  // Per row of tiles, in the columns that mostly agree at the offset: rows
  // where moving tiles outnumber tiles that stood still, against rows where
  // the still ones do. At least this share: what moves is the page only when
  // it moves over the frame's height. An inner scroll area (a page's own
  // example box under the pointer) moves in a few rows while the text above
  // and below it in the same columns stands still.
  double minRowCoverage = 0.50;
  // A second look for another layer when the best candidate leaves at least
  // this share of the tiles neither moving with it nor standing still.
  double secondLayer = 0.20;
  // When nothing explains the frame and at least this share of it stood
  // still, the page did not move and something changed in place.
  double stillFallback = 0.50;
  // Where that much stood still, what moved is the page only when its rows
  // of tiles cover at least this share in its columns; where stillMostly of
  // it stood still, stillCoverageMostly. A page's code block that scrolled
  // under the pointer covered 0.62 with three quarters still; a page moving
  // under a header, side bars and an advertisement bar that nothing has
  // pinned yet covers 0.77 with half to two thirds still.
  double stillCoverage = 0.70;
  double stillMostly = 0.70;
  double stillCoverageMostly = 0.80;
  // Candidates below this share of agreeing bands are given up after a
  // coarse look.
  double coarseFloor = 0.30;
  // Candidates within this share of cells of the best are tied (content that
  // repeats); the expected offset decides between them or the frame is skipped.
  double tieCells = 0.012;
  // The frame did not move when this share of its textured cells stayed.
  double stillShare = 0.97;
  // Pinned: a block that stayed through this many consecutive moving steps
  // of at least learnMinStep rows (twice as many when only its flat inside
  // stayed). A block pinned once is released when it moves with the page
  // again.
  int pinAfterSteps = 2;
  int learnMinStep = 4;
  // Holes in a pinned part up to this share of the frame height are closed
  // (the flat inside of a floating box has no texture to vote with).
  double closeFraction = 0.34;
  // ... and gaps across up to this share of the frame width.
  double closeWidthFraction = 0.15;
  // Height cap of the image, all rows (as StitchOptions::maxOutputRows).
  long long maxOutputRows = 32000;
  // After this many frames in a row that could not be placed the state is Lost.
  int lostAfterSkips = 2;
};

// The image is written to a canvas the caller owns: page rows, 0 the first
// frame's first row, negative above it. A test backs it with memory, the
// helper with its spool file. The engine never holds the image.
class TrackerCanvas {
 public:
  virtual ~TrackerCanvas() = default;
  // Pixels [x0, x1) of page row `row`, BGRx. A row written for the first
  // time extends the extent; rows between are zero until written.
  virtual void writeSpan(long long row, int x0, int x1, const uint8_t* bgrx) = 0;
  // The whole row as written so far. False outside the extent.
  virtual bool readRow(long long row, uint8_t* bgrx) const = 0;
  // Rows [top, bottom) have been written to.
  virtual long long top() const = 0;
  virtual long long bottom() const = 0;
};

// The canvas in memory, for tests and the replay tool.
class MemoryCanvas final : public TrackerCanvas {
 public:
  explicit MemoryCanvas(int width) : width_(width) {}
  void writeSpan(long long row, int x0, int x1, const uint8_t* bgrx) override;
  bool readRow(long long row, uint8_t* bgrx) const override;
  long long top() const override { return top_; }
  long long bottom() const override { return top_ + rows_; }
  int width() const { return width_; }
  // Rows [from, to) packed, BGRx.
  std::vector<uint8_t> image(long long from, long long to) const;

 private:
  static constexpr long long kChunkRows = 256;
  uint8_t* rowPtr(long long row, bool create);
  int width_;
  long long top_ = 0, rows_ = 0;  // extent
  std::map<long long, std::vector<uint8_t>> chunks_;
  uint8_t* cached_ = nullptr;
  long long cachedChunk_ = 0;
};

// What the driver expects of a frame: how far the content moved since the
// frame before it (not since the last placed one), and how far that may be
// off. put.gd's own wheel steps know it; a person's scrolling does not.
struct TrackerHint {
  bool known = false;
  int offset = 0;
  int uncertainty = 0;
};

// A rectangle of the frame in frame pixels: x, y its top left corner. Empty
// when w or h is not positive.
struct TrackerRect {
  int x = 0, y = 0, w = 0, h = 0;
  bool empty() const { return w <= 0 || h <= 0; }
};

enum class TrackStatus {
  Placed,      // the frame's place in the page is proved (the first frame too)
  NoMovement,  // the content did not move (changes in place are ignored)
  Skipped,     // no place could be proved; nothing was written
  CapReached,  // the image is at its height cap; nothing more is written
  Invalid,     // the frame's size differs from the first frame's, or no data
};
const char* toString(TrackStatus);

enum class SkipReason {
  None,
  NoEvidence,  // nothing textured to compare (a blank stretch) and no hint
  NoMatch,     // no offset explains the frame (it moved too far, or is another page)
  Ambiguous,   // the content repeats and the expected offset cannot decide
  Contradicted,// the offset found against the last frame disagrees with the image
};
const char* toString(SkipReason);

enum class TrackState {
  Waiting,    // no frame yet
  Following,  // the last frame was placed or did not move
  Lost,       // frames are being skipped: the bar says "Scroll back a little"
  Capped,     // the height cap was reached
};
const char* toString(TrackState);

struct TrackResult {
  TrackStatus status = TrackStatus::Skipped;
  SkipReason reason = SkipReason::None;
  int offset = 0;              // content movement since the last placed frame, + downwards
  long long position = 0;      // page row of the frame's first row (placed frames)
  int addedTop = 0, addedBottom = 0;  // rows the image grew by
  double confidence = 0;       // agreeing share of the compared cells
  bool byExpectation = false;  // repeating content: the expected offset chose among equal candidates
  bool onHint = false;         // placed on the driver's hint alone (nothing to compare)
  bool recovered = false;      // found inside the image after skipped frames
  TrackState state = TrackState::Waiting;
};

struct TrackTotals {
  int frames = 0, placed = 0, noMovement = 0, skipped = 0, invalid = 0;
  int longestSkipRun = 0;
  int recovered = 0;
  int byExpectation = 0;
  int onHint = 0;              // placements on the hint alone: a gap bridged without evidence
  int pinnedRegions = 0;       // pinned parts found (connected regions of the final mask)
  bool capReached = false;
  long long top = 0, bottom = 0;  // the image's extent in page rows
  // Under the excluded rectangle (ScrollTracker::setExcluded), in rows of the
  // image: cells that only the rectangle kept from being seen (every placed
  // frame that held them had them under it), filled with the plain colour,
  // and the rows that hold them. 0 when every such cell was shown outside it.
  long long hiddenCells = 0, hiddenRows = 0;
};

class ScrollTracker {
 public:
  ScrollTracker(TrackerCanvas& canvas, TrackerOptions options = {});
  ~ScrollTracker();
  ScrollTracker(const ScrollTracker&) = delete;
  ScrollTracker& operator=(const ScrollTracker&) = delete;

  TrackResult addFrame(const FrameView& frame, TrackerHint hint = {});
  // A rectangle of the frame that is not the application (put.gd's own bar
  // drawn inside the region). Its cells never vote, never count as evidence
  // or contradiction, are never composed and never drawn as a pinned part:
  // the page rows under it come from frames in which they are clear, as under
  // a pinned box. It is rounded out to whole cells (cellPx columns) and block
  // rows (blockRows rows). Set it before the first frame or between frames;
  // it applies from the next addFrame(). An empty rectangle clears it.
  //
  // What no frame shows clear. When the rectangle spans every cell of the
  // width and touches the frame's top or bottom edge (a bar across the
  // bottom of the region), the frame rows it covers there never count for
  // the extent: the image ends above such a bar, also at the page's end, and
  // addedTop/addedBottom never include them. Any other cell under it that no
  // frame showed clear (a bar over part of the width at the page's very end
  // or start) is filled in finish() like anything else no frame showed
  // clear, with the plain colour of its column (the most common colour of
  // the written rows beside it), and counted in TrackTotals::hiddenCells and
  // hiddenRows.
  void setExcluded(TrackerRect rect);
  TrackerRect excluded() const { return excludedRect_; }
  // Writes what is pinned once (at the top as the first screen showed it, at
  // the bottom as the last), fills what no frame showed clear, and returns
  // the extent. No frame is taken after it.
  TrackTotals finish();

  TrackState state() const { return state_; }
  const TrackTotals& totals() const { return totals_; }
  long long top() const { return top_; }
  long long bottom() const { return bottom_; }
  long long height() const { return hasFrame_ ? bottom_ - top_ : 0; }
  const TrackerOptions& options() const { return opts_; }

  // Diagnostics for tests and the replay tool: the pinned mask in blocks of
  // the frame (blockRows x cellPx), row-major, 1 where pinned.
  std::vector<uint8_t> pinnedMask(int& blockCols, int& blockRowsOut) const;

  struct Signature;  // per-frame row signatures (ScrollTracker.cpp)
  struct Store;      // per page row: what is written, and its band signature

 private:
  struct Candidate {
    int d = 0;
    double bands = 0, cells = 0;         // agreeing share of all informative lanes (the ranking)
    double area = 0;         // tiles that move with d, of the tiles that show something
    double other = 0;        // tiles that neither move with d nor stood still
    int movingTiles = 0;
    std::vector<uint8_t> moving;  // per tile: 1 moves with d, 2 shows something else (it disagrees or stood still), 0 blank
    double bandsKept = 0, cellsKept = 0;  // agreement within the moving tiles (the acceptance)
    double coverage = 0;     // rows of tiles that moved, against rows that stood still, in its columns
    int informative = 0, rows = 0;
    bool accepted = false;
  };
  // Agreeing and informative lanes per tile (a voting band by kTileRows rows
  // of the incoming frame), for one candidate.
  struct Tally {
    std::vector<uint16_t> agree, inf;
    int bands = 0;
    unsigned agreeSum = 0, infSum = 0;
    int rows = 0, texturedRows = 0;
    void clear(int bandCount, int height);
    void total();
    double share() const { return infSum ? double(agreeSum) / infSum : 0.0; }
    // The page's tiles at this offset: those that mostly agree.
    bool keptTile(size_t i) const { return inf[i] > 0 && 2 * agree[i] >= inf[i]; }
  };

  void computeExcluded(const Signature& cur, const Signature& ref);
  void bandTally(const Signature& cur, const Signature& ref, int d, Tally& t) const;
  void cellTally(const Signature& cur, const Signature& ref, int d, Tally& t) const;
  void storeBandTally(const Signature& cur, long long pos, int rowStep, Tally& t) const;
  void storeCellTally(const Signature& cur, long long pos, Tally& t) const;
  void judge(Candidate& c, const Tally& bands, const Tally& cells, int y0, int y1) const;
  bool stillTile(size_t i) const;
  double coarseShare(const Signature& cur, const Signature& ref, int d, double abortBelow,
                     const std::vector<uint16_t>& incl) const;
  void searchPass(const Signature& cur, const Signature& ref, int expected, const std::vector<uint16_t>& incl,
                  std::vector<Candidate>& out) const;
  double storeCoarseShare(const Signature& cur, long long pos, double abortBelow) const;
  std::vector<int> pickPeaks(const std::vector<float>& coarse, double floor, long long center) const;
  bool stillFrame(const Signature& cur, const Signature& ref, double* share) const;
  std::vector<Candidate> searchPair(const Signature& cur, const Signature& ref, int expected) const;
  std::vector<Candidate> searchStore(const Signature& cur, long long expectedPos, bool wide) const;
  int decide(std::vector<Candidate>& cands, int expected, int uncertainty, bool hinted,
             bool* byExpectation, SkipReason* why) const;
  bool contradicted(const Signature& cur, long long pos) const;
  void learn(const Signature& cur, const Signature& ref, int d);
  void closeMask();
  void close(const std::vector<uint8_t>& src, std::vector<uint8_t>& out) const;
  void blockedWords(std::vector<uint64_t>& out) const;
  void writeCells(long long p, const uint8_t* row, const uint16_t* bands, const uint16_t* tex, const uint64_t* mask,
                  int level, const uint64_t* blocked);
  void flushPending(const std::vector<uint64_t>& blocked);
  void compose(const FrameView& frame, const Signature& cur, const Signature* ref, int d, long long pos,
               TrackResult& r);
  struct FrameCopy {
    std::vector<uint8_t> px;     // the frame's pixels, where tag says so
    std::vector<uint32_t> tag;   // per block: the serial of the copy that wrote it
    uint32_t serial = 0;
    long long pos = 0;
  };
  void keepCopy(const FrameView& frame, FrameCopy& copy, long long pos, bool all, int fromRow);
  void placeFrame(const FrameView& frame, Signature& cur, const Signature* ref, int d, long long pos,
                  TrackResult& r);

  TrackerCanvas& canvas_;
  TrackerOptions opts_;
  TrackState state_ = TrackState::Waiting;
  TrackTotals totals_;
  bool hasFrame_ = false, finished_ = false, capped_ = false;
  int width_ = 0, height_ = 0;
  int cells_ = 0, cellLo_ = 0, cellHi_ = 0;  // cells, and the compared ones [cellLo_, cellHi_)
  int bandTol_ = 0;
  int blockRowCount_ = 0;
  std::unique_ptr<Signature> last_;           // the last placed frame's signatures
  std::unique_ptr<Signature> work_;           // the incoming frame's
  long long lastPos_ = 0;
  long long top_ = 0, bottom_ = 0;            // extent written
  // Expectation: the last movement per frame, and the hints since the last
  // placed frame.
  double velocity_ = 0;
  int lastMove_ = 0;  // the last placed movement, kept through frames that did not move
  int framesSincePlaced_ = 0;
  long long hintSum_ = 0, hintUnc_ = 0;
  bool hintKnown_ = false;
  int skipRun_ = 0;
  // The pinned mask, per block: steps it stayed, steps it moved, pinned.
  std::vector<uint8_t> stayed_, wandering_, pinned_, closed_, stillNow_;
  // What a placed frame does not offer the image: pinned or stood still in
  // the last pair, closed and widened.
  std::vector<uint8_t> blocked_;
  // Band boundaries in cells; and per row of the frame, the bands and cells
  // that vote (not pinned, not standing still since the last placed frame).
  std::vector<int> bandLo_;
  std::vector<uint16_t> bandIncl_, cellIncl_;
  // The closed mask as 16-bit lanes: per block row, per cell and per band;
  // the excluded rectangle is in them too.
  std::vector<uint16_t> closed16_, bandPinned16_;
  // The excluded rectangle: as given, per block, as words per block row, and
  // the frame rows it leaves [effTop_, effBottom_) (rows it covers across the
  // whole width at the top and the bottom are not).
  TrackerRect excludedRect_;
  std::vector<uint8_t> excl_;
  std::vector<uint16_t> exclLane16_;
  std::vector<uint64_t> exclWords_;
  int effTop_ = 0, effBottom_ = 0;
  void applyExcluded();
  void refreshLanes();
  // Rows of the incoming frame [0, y) that show something outside what is pinned.
  std::vector<int> texturedRows_;
  // Per tile of the incoming frame: its textured band lanes outside what is
  // pinned, and those of them that stood still since the last placed frame.
  std::vector<uint16_t> texTile_, stillTile_, texCellTile_;
  // The incoming frame's rows the coarse look compares, spread over it.
  std::vector<int> anchors_;
  // Copies of the topmost and the bottommost placed frames, for what is
  // pinned; the first screen's most common colour.
  FrameCopy topCopy_, bottomCopy_;
  uint32_t pageColour_ = 0;
  unsigned pageCell_ = 0;  // its cell value
  int learnedSteps_ = 0;
  // Placed frames' rows with cells seen once, waiting (their pixels, their
  // bands and texture, and which cells): for the next pair, and while the
  // mask is young for it to learn what is pinned; and the rows the cap allows.
  struct Pending {
    long long pos = 0;
    std::vector<int> rows;
    std::vector<uint8_t> pixels;
    std::vector<uint16_t> bands;
    std::vector<uint64_t> open;
  };
  std::vector<Pending> pending_;
  void narrowPending(Pending& p, const std::vector<uint64_t>& blocked) const;
  long long allowLo_ = 0, allowHi_ = 0;
  uint32_t frameSerial_ = 0;  // placed frames, counted from 1
  std::unique_ptr<Store> store_;
};

}  // namespace putgd::scroll
