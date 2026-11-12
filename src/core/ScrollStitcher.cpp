#include "ScrollStitcher.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace putgd::scroll {
namespace {

constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001b3ull;

inline uint64_t fmix64(uint64_t k) {
  k ^= k >> 33;
  k *= 0xff51afd7ed558ccdull;
  k ^= k >> 33;
  k *= 0xc4ceb9fe1a85ec53ull;
  k ^= k >> 33;
  return k;
}

inline uint32_t loadPx(const uint8_t* row, int x) {
  uint32_t v;
  std::memcpy(&v, row + 4 * x, 4);
  return v & 0x00FFFFFFu;  // drop the X byte
}

inline int chanSum(uint32_t v) { return int(v & 0xFF) + int((v >> 8) & 0xFF) + int((v >> 16) & 0xFF); }

inline int colorDelta(uint32_t a, uint32_t b) {
  int d = 0;
  for (int s = 0; s < 24; s += 8) d += std::abs(int((a >> s) & 0xFF) - int((b >> s) & 0xFF));
  return d;
}

inline int tolerance(const StitchOptions& o, int bands) {
  return std::max(0, bands - std::clamp(o.minBandsEqual, 0, bands));
}

inline uint32_t equalMask(const uint64_t* a, const uint64_t* b, int bands) {
  uint32_t m = 0;
  for (int k = 0; k < bands; ++k) m |= uint32_t(a[k] == b[k]) << k;
  return m;
}

}  // namespace

const char* toString(StepStatus s) {
  switch (s) {
    case StepStatus::First: return "First";
    case StepStatus::Appended: return "Appended";
    case StepStatus::NoMovement: return "NoMovement";
    case StepStatus::Reversed: return "Reversed";
    case StepStatus::NoMatch: return "NoMatch";
    case StepStatus::Ambiguous: return "Ambiguous";
    case StepStatus::LimitReached: return "LimitReached";
    case StepStatus::InvalidFrame: return "InvalidFrame";
    case StepStatus::LayoutChanged: return "LayoutChanged";
  }
  return "?";
}

// ---------------------------------------------------------------- signatures

void FrameSignatures::compute(const FrameView& f, const StitchOptions& o) {
  width = f.width;
  height = f.height;
  bands = std::clamp(o.bands, 1, 32);
  int margin = int(std::lround(width * std::max(0.0, o.sideMarginFraction)));
  margin = std::clamp(margin, 0, std::max(0, (width - bands) / 2));
  colStart = margin;
  colEnd = width - margin;
  const size_t hb = size_t(height) * size_t(bands);
  bandHash.resize(hb);
  edgeHash.resize(hb);
  rowHash.resize(size_t(height));
  sideHash.resize(size_t(height) * 2);
  sideFlat.resize(size_t(height));
  flatBandMask.resize(size_t(height));
  edgeBandMask.resize(size_t(height));
  flatColor.resize(size_t(height));
  rowFlat.resize(size_t(height));

  int bx[33];
  const int n = colEnd - colStart;
  for (int k = 0; k <= bands; ++k) bx[k] = colStart + int((long long)n * k / bands);
  const int thr = o.edgeThreshold;

  for (int y = 0; y < height; ++y) {
    const uint8_t* r = f.row(y);
    uint64_t* bh = &bandHash[size_t(y) * bands];
    uint64_t* eh = &edgeHash[size_t(y) * bands];
    uint32_t flatMask = 0, edgeMask = 0;
    const uint32_t rowFirst = loadPx(r, colStart);
    bool allFlat = true;
    uint64_t rh = kFnvOffset;
    for (int k = 0; k < bands; ++k) {
      const int x0 = bx[k], x1 = bx[k + 1];
      // Two independent hash lanes (even/odd pixel pairs) halve the multiply
      // dependency chain; the result is still an exact-content hash.
      uint64_t h = kFnvOffset ^ (uint64_t(k) * 0x9E3779B97F4A7C15ull);
      uint64_t h2 = 0x6c62272e07bb0142ull;
      uint64_t e = 0x84222325cbf29ce4ull;
      const uint32_t first = loadPx(r, x0);
      uint32_t diffAcc = 0;
      bool edges = false;
      int prevS = chanSum(first);
      int x = x0;
      for (; x + 1 < x1; x += 2) {
        const uint32_t v0 = loadPx(r, x), v1 = loadPx(r, x + 1);
        h = (h ^ v0) * kFnvPrime;
        h2 = (h2 ^ v1) * kFnvPrime;
        diffAcc |= (v0 ^ first) | (v1 ^ first);
        const int s0 = chanSum(v0), s1 = chanSum(v1);
        const int d0 = s0 - prevS, d1 = s1 - s0;
        if (d0 > thr || d0 < -thr) {
          e = (e ^ uint64_t(x - x0 + 1)) * kFnvPrime;
          edges = true;
        }
        if (d1 > thr || d1 < -thr) {
          e = (e ^ uint64_t(x - x0 + 2)) * kFnvPrime;
          edges = true;
        }
        prevS = s1;
      }
      for (; x < x1; ++x) {
        const uint32_t v = loadPx(r, x);
        h = (h ^ v) * kFnvPrime;
        diffAcc |= v ^ first;
        const int s = chanSum(v);
        const int d = s - prevS;
        if (d > thr || d < -thr) {
          e = (e ^ uint64_t(x - x0 + 1)) * kFnvPrime;
          edges = true;
        }
        prevS = s;
      }
      h ^= fmix64(h2 + uint64_t(x1 - x0));
      bh[k] = fmix64(h);
      eh[k] = edges ? fmix64(e) : 0;
      const bool flat = diffAcc == 0;
      flatMask |= uint32_t(flat) << k;
      edgeMask |= uint32_t(edges) << k;
      if (!flat || first != rowFirst) allFlat = false;
      rh = (rh ^ bh[k]) * kFnvPrime;
    }
    // The margins, each on its own: what a scrollbar does in one must not
    // hide what the other shows (line numbers, the start of short lines).
    uint64_t left = kFnvOffset, right = kFnvOffset;
    uint32_t leftDiff = 0, rightDiff = 0;
    const uint32_t leftFirst = colStart > 0 ? loadPx(r, 0) : 0, rightFirst = colEnd < width ? loadPx(r, colEnd) : 0;
    for (int x = 0; x < colStart; ++x) {
      const uint32_t v = loadPx(r, x);
      left = (left ^ v) * kFnvPrime;
      leftDiff |= v ^ leftFirst;
    }
    for (int x = colEnd; x < width; ++x) {
      const uint32_t v = loadPx(r, x);
      right = (right ^ v) * kFnvPrime;
      rightDiff |= v ^ rightFirst;
    }
    rowHash[y] = fmix64(rh);
    sideHash[size_t(y) * 2] = left;
    sideHash[size_t(y) * 2 + 1] = right;
    sideFlat[y] = uint8_t((leftDiff == 0 ? 1 : 0) | (rightDiff == 0 ? 2 : 0));
    flatBandMask[y] = flatMask;
    edgeBandMask[y] = edgeMask;
    flatColor[y] = rowFirst;
    rowFlat[y] = allFlat ? 1 : 0;
  }
}

size_t FrameSignatures::bytes() const {
  return (bandHash.size() + edgeHash.size() + rowHash.size() + sideHash.size()) * 8 +
         (flatBandMask.size() + edgeBandMask.size() + flatColor.size()) * 4 + rowFlat.size() + sideFlat.size();
}

bool sameRowLoose(const FrameSignatures& a, int ya, const FrameSignatures& b, int yb,
                  const StitchOptions& o) {
  if (a.rowHash[ya] == b.rowHash[yb]) return true;
  const int B = a.bands;
  const uint32_t eq = equalMask(&a.bandHash[size_t(ya) * B], &b.bandHash[size_t(yb) * B], B);
  return B - std::popcount(eq) <= tolerance(o, B);
}

int countChangedRows(const FrameSignatures& a, const FrameSignatures& b, int y0, int y1,
                     const StitchOptions& o) {
  int n = 0;
  for (int y = y0; y < y1; ++y) n += sameRowLoose(a, y, b, y, o) ? 0 : 1;
  return n;
}

int countStrictChangedRows(const FrameSignatures& a, const FrameSignatures& b, int y0, int y1) {
  int n = 0;
  for (int y = y0; y < y1; ++y) n += a.rowHash[y] != b.rowHash[y];
  return n;
}

int strictChangeBudget(const StitchOptions& o, int height) {
  return o.noMoveMaxStrictRows >= 0 ? o.noMoveMaxStrictRows : std::max(48, height / 10);
}

bool framesDiffer(const FrameSignatures& a, const FrameSignatures& b, int y0, int y1,
                  const StitchOptions& o, int maxLooseChangedRows) {
  if (a.height != b.height || a.width != b.width) return true;
  return countChangedRows(a, b, y0, y1, o) > maxLooseChangedRows ||
         countStrictChangedRows(a, b, y0, y1) > strictChangeBudget(o, a.height) || marginsChanged(a, b);
}

bool marginsChanged(const FrameSignatures& a, const FrameSignatures& b) {
  if (a.height != b.height) return true;
  return a.sideHash != b.sideHash;
}

int classifyRowPair(const FrameSignatures& p, int yp, const FrameSignatures& c, int yc,
                    const StitchOptions& o) {
  const bool fp = p.rowFlat[yp], fc = c.rowFlat[yc];
  if (fp && fc) {
    // Flat rows never prove a match; strongly different flat colours disprove one.
    // They are not informative rows, so this does not enter the score (-2).
    return colorDelta(p.flatColor[yp], c.flatColor[yc]) > o.flatConflictDelta ? -2 : -1;
  }
  if (p.rowHash[yp] == c.rowHash[yc]) return 1;
  const int B = p.bands;
  const int tol = tolerance(o, B);
  const uint32_t eq = equalMask(&p.bandHash[size_t(yp) * B], &c.bandHash[size_t(yc) * B], B);
  if (B - std::popcount(eq) <= tol) {
    // Small difference: it is a match only if an informative (non-flat) band agrees,
    // otherwise it could be a short line or a caret on a blank row: no evidence.
    return (eq & ~p.flatBandMask[yp]) ? 1 : -1;
  }
  // Structure rule: same edge positions (tolerates a background highlight).
  const uint32_t seq = equalMask(&p.edgeHash[size_t(yp) * B], &c.edgeHash[size_t(yc) * B], B);
  const int sun = B - std::popcount(seq);
  if (sun <= tol) {
    if (seq & p.edgeBandMask[yp]) return 1;
    // Edge-less rows whose colours differ (a tint) plus a small local structure
    // (caret or spinner on a blank row): no evidence either way.
    if (sun > 0) return -1;
  }
  return 0;
}

// ---------------------------------------------------------------- stitcher

ScrollStitcher::ScrollStitcher(StitchOptions opts, RowSink sink, RowTakeBack takeBack)
    : opts_(opts), sink_(std::move(sink)), takeBack_(std::move(takeBack)) {
  opts_.bands = std::clamp(opts_.bands, 1, 32);
  opts_.minOverlapRows = std::max(1, opts_.minOverlapRows);
}

int ScrollStitcher::outputHeight() const {
  if (!hasRef_) return 0;
  if (finished_) return int(bodyRows_);
  if (!moved_) return std::min(height_, opts_.maxOutputRows);
  return int(logicalRows()) + std::max(estBot_, 0);
}

size_t ScrollStitcher::memoryBytes() const {
  return image_.capacity() + pending_.capacity() + footer_.capacity() + held_.capacity() + proved_.pixels.capacity() + pace_.pixels.capacity() + (refFine_.capacity() + curFine_.capacity()) * 8 + (refLight_.capacity() + curLight_.capacity()) * 2 + ref_.bytes();
}

void ScrollStitcher::emitRaw(const uint8_t* row) {
  if (sink_) {
    sink_(row, width_);
  } else {
    image_.insert(image_.end(), row, row + size_t(width_) * 4);
  }
  ++bodyRows_;
}

void ScrollStitcher::emitRows(const FrameView& f, int y0, int y1) {
  if (y1 <= y0) return;
  const long long reserve = moved_ ? std::min(std::max(estBot_, 0), opts_.maxOutputRows) : 0;
  const long long allowed = std::max(0LL, opts_.maxOutputRows - reserve - bodyRows_);
  long long n = y1 - y0;
  if (n > allowed) {
    n = allowed;
    limited_ = true;
  }
  for (long long i = 0; i < n; ++i) {
    // Copy only the 4*width bytes of the row (stride may include padding).
    emitRaw(f.row(int(y0 + i)));
  }
}

void ScrollStitcher::flushPending(int rows) {
  FrameView pv{pending_.data(), width_, height_, std::ptrdiff_t(width_) * 4};
  bodyRows_ = 0;
  emitRows(pv, 0, std::clamp(rows, 0, height_));
  std::vector<uint8_t>().swap(pending_);
}

void ScrollStitcher::storeFooter(const FrameView& f) {
  footerRows_ = std::max(estBot_, 0);
  footer_.resize(size_t(footerRows_) * width_ * 4);
  for (int i = 0; i < footerRows_; ++i)
    std::memcpy(&footer_[size_t(i) * width_ * 4], f.row(height_ - footerRows_ + i), size_t(width_) * 4);
}

// The compared columns of every row in quarters of a band, each an exact hash
// of its pixels, and its mean brightness (the channels' sum, in sixteenths):
// what the own-pace rule and the search by quarter bands compare. Only for
// the frames the stitcher is given, not for every frame a settle check looks at.
void ScrollStitcher::fineHashes(const FrameView& f, const FrameSignatures& sig, std::vector<uint64_t>& out, std::vector<uint16_t>& light) const {
  const int F = sig.bands * kFinePerBand, n = sig.colEnd - sig.colStart;
  out.resize(size_t(f.height) * F);
  light.resize(size_t(f.height) * F);
  std::vector<int> edge(size_t(F) + 1);
  for (int j = 0; j <= F; ++j) {
    // Quarters of the signature's own bands, so that a quarter never straddles two.
    const int k = j / kFinePerBand, q = j % kFinePerBand;
    const int x0 = sig.colStart + int((long long)n * k / sig.bands), x1 = sig.colStart + int((long long)n * std::min(k + 1, sig.bands) / sig.bands);
    edge[size_t(j)] = j == F ? sig.colEnd : x0 + int((long long)(x1 - x0) * q / kFinePerBand);
  }
  for (int y = 0; y < f.height; ++y) {
    const uint8_t* r = f.row(y);
    uint64_t* o = &out[size_t(y) * F];
    for (int j = 0; j < F; ++j) {
      uint64_t h = kFnvOffset, h2 = 0x6c62272e07bb0142ull;
      int x = edge[size_t(j)];
      const int x0 = x, x1 = edge[size_t(j) + 1];
      uint32_t sum = 0;
      for (; x + 1 < x1; x += 2) {
        const uint32_t v0 = loadPx(r, x), v1 = loadPx(r, x + 1);
        h = (h ^ v0) * kFnvPrime;
        h2 = (h2 ^ v1) * kFnvPrime;
        sum += uint32_t(chanSum(v0) + chanSum(v1));
      }
      if (x < x1) {
        const uint32_t v = loadPx(r, x);
        h = (h ^ v) * kFnvPrime;
        sum += uint32_t(chanSum(v));
      }
      o[j] = fmix64(h ^ fmix64(h2 + uint64_t(x1)));
      light[size_t(y) * F + j] = uint16_t(x1 > x0 ? (uint64_t(sum) * 16) / uint64_t(x1 - x0) : 0);
    }
  }
}

// The edge guard (StitchOptions::edgeGuardFraction): how many of the moving
// band's last rows wait for the next frame.
int ScrollStitcher::guardRows(int band) const {
  if (opts_.edgeGuardFraction <= 0 || band <= 0) return 0;
  const int rows = std::max(opts_.edgeGuardMinRows, int(std::lround(band * opts_.edgeGuardFraction)));
  return std::clamp(rows, 0, band / 4);
}

void ScrollStitcher::holdRows(const uint8_t* rows, std::ptrdiff_t stride, int count) {
  heldRows_ = std::max(count, 0);
  held_.resize(size_t(heldRows_) * width_ * 4);
  for (int i = 0; i < heldRows_; ++i) std::memcpy(&held_[size_t(i) * width_ * 4], rows + i * stride, size_t(width_) * 4);
}

// Writes the first `count` held rows out as they were seen; the others are
// dropped, for the caller to take from a frame that shows them.
void ScrollStitcher::emitHeld(int count) {
  const int rows = std::clamp(count, 0, heldRows_);
  heldRows_ = 0;
  if (rows > 0) emitRows(FrameView{held_.data(), width_, rows, std::ptrdiff_t(width_) * 4}, 0, rows);
}

// The rows that wait, as they are now, and where they begin: what a cut back
// to `rows` (the rows known before a step) puts in their place again.
void ScrollStitcher::Shadow::save(const std::vector<uint8_t>& held, int heldRows, long long bodyRows, int width) {
  start = bodyRows;
  count = heldRows;
  pixels.assign(held.begin(), held.begin() + std::ptrdiff_t(size_t(heldRows) * width * 4));
}

// Cuts the image back to `rows`. The last of those rows waited when their
// step began and may since have been taken from a later frame, the very
// frame the cut is about; with the shadow of that step they are again as
// they were seen before it.
bool ScrollStitcher::cutBackTo(long long rows, const Shadow& shadow) {
  if (shadow.count > 0 && shadow.start + shadow.count == rows && shadow.start <= bodyRows_) {
    if (!takeBackTo(shadow.start)) return false;
    holdRows(shadow.pixels.data(), std::ptrdiff_t(width_) * 4, shadow.count);
    return true;
  }
  return takeBackTo(rows);
}

void ScrollStitcher::restartBeforeMovement() {
  if (!moved_ && !finished_) *this = ScrollStitcher(opts_, std::move(sink_), std::move(takeBack_));
}

void ScrollStitcher::finish() {
  if (finished_ || !hasRef_) {
    finished_ = true;
    return;
  }
  if (!moved_) {
    flushPending(height_);
  } else {
    emitHeld(heldRows_);
    const int rows = int(std::min<long long>(std::max(estBot_, 0), std::max(0LL, opts_.maxOutputRows - bodyRows_)));
    const int skip = footerRows_ - rows;  // footer_ may hold more rows than the final estimate
    for (int i = 0; i < rows; ++i) emitRaw(&footer_[size_t(skip + i) * width_ * 4]);
  }
  finished_ = true;
}

StepResult ScrollStitcher::addFrame(const FrameView& f, int hint) {
  FrameSignatures sig;
  if (f.data && f.width > 0 && f.height > 0) sig.compute(f, opts_);
  return addFrame(f, sig, hint);
}

StepResult ScrollStitcher::addFrame(const FrameView& f, const FrameSignatures& cur, int hint,
                                    int unc) {
  StepResult r;
  r.fixedTop = std::max(estTop_, 0);
  r.fixedBottom = std::max(estBot_, 0);
  if (finished_ || !f.data || f.width <= 0 || f.height <= 0 || f.stride < f.width * 4 ||
      cur.width != f.width || cur.height != f.height) {
    r.status = StepStatus::InvalidFrame;
    return r;
  }
  if (!hasRef_) {
    width_ = f.width;
    height_ = f.height;
    pending_.resize(size_t(width_) * height_ * 4);
    for (int y = 0; y < height_; ++y)
      std::memcpy(&pending_[size_t(y) * width_ * 4], f.row(y), size_t(width_) * 4);
    bodyRows_ = height_;
    refOrigin_ = 0;
    ref_ = cur;
    fineHashes(f, cur, refFine_, refLight_);
    hasRef_ = true;
    r.status = StepStatus::First;
    return r;
  }
  if (f.width != width_ || f.height != height_) {
    r.status = StepStatus::InvalidFrame;
    return r;
  }
  if (limited_) {
    r.status = StepStatus::LimitReached;
    return r;
  }
  if (changed_) {
    r.status = StepStatus::LayoutChanged;
    return r;
  }
  const int H = height_;
  fineHashes(f, cur, curFine_, curLight_);
  r.changedRows = countChangedRows(ref_, cur, 0, H, opts_);
  const bool margin = marginsChanged(ref_, cur);
  // Loose rule: changes confined to <= (bands - minBandsEqual) bands are noise.
  // Strict rule: any exact row change counts, but a small number of rows (a
  // caret or spinner on blank rows) is tolerated. Content whose ink always sits
  // in <= 2 bands (short lines) passes the loose rule while scrolling, so both
  // must say "unchanged" for NoMovement.
  const bool looseIdentical = r.changedRows <= opts_.noMoveMaxChangedRows;
  const bool identical =
      looseIdentical && countStrictChangedRows(ref_, cur, 0, H) <= strictChangeBudget(opts_, H);
  const bool strictRuns = looseIdentical && !identical;
  auto same = [&](int y) {
    return strictRuns ? ref_.rowHash[y] == cur.rowHash[y] : sameRowLoose(ref_, y, cur, y, opts_);
  };
  auto refresh = [&] {
    // The page is where it was, the same picture but for a few rows: rows that
    // wait for a better look (the edge guard) get this frame's, where it shows
    // them as the same rows. What was over them when their frame was taken (a
    // status panel that has gone since) is then not in the image. Not after a
    // change in place found by the search: its rows may be another page's.
    for (int i = 0; moved_ && identical && i < heldRows_; ++i) {
      const long long y = bodyRows_ + i - refOrigin_;
      if (y < std::max(estTop_, 0) || y >= H - std::max(estBot_, 0)) continue;
      if (sameRowLoose(ref_, int(y), cur, int(y), opts_)) std::memcpy(&held_[size_t(i) * width_ * 4], f.row(int(y)), size_t(width_) * 4);
    }
    ref_ = cur;
    refFine_.swap(curFine_);
    refLight_.swap(curLight_);
    if (moved_) storeFooter(f);
  };
  auto noMovement = [&](double score) {
    refresh();
    r.status = StepStatus::NoMovement;
    r.score = score;
    return r;
  };
  // Identical frames are NoMovement unless a margin changed (a scrollbar moved,
  // or line numbers scrolled beside lines that are all alike): then the pixels
  // decide, the margin rule and the caller's hint among them. Without a hint
  // only an offset the pixels prove counts; anything else is NoMovement below.
  if (identical && !margin) return noMovement(1.0);

  int top, bot;
  int stayedBottom = 0;  // rows at the bottom that are where they were, as measured in this step
  int stayedTop = 0;     // and at the top
  if (identical) {
    top = std::max(estTop_, 0);
    bot = std::max(estBot_, 0);
  } else {
    int t = 0;
    while (t < H && same(t)) ++t;
    int b = 0;
    while (b < H - t && same(H - 1 - b)) ++b;
    if (!strictRuns) {
      // Rows that pass the tolerant rule are fixed rows only while few of them
      // changed at all. Short lines that scrolled pass it everywhere, and a
      // small change in place among them (the mouse pointer a compositor
      // paints into the frames) was then taken for the whole difference: the
      // step counted as "changed in place" and its rows were never appended
      // (three lines missing from a Qt text view on Hyprland, lab guest of
      // 2026-11-10). The runs are then measured by exact equality.
      const int changedInRuns = countStrictChangedRows(ref_, cur, 0, t) + countStrictChangedRows(ref_, cur, H - b, H);
      if (changedInRuns > strictChangeBudget(opts_, H)) {
        t = 0;
        while (t < H && ref_.rowHash[t] == cur.rowHash[t]) ++t;
        b = 0;
        while (b < H - t && ref_.rowHash[H - 1 - b] == cur.rowHash[H - 1 - b]) ++b;
      }
    }
    // Over-estimates (content that happened to be identical under a header) only
    // shrink over later steps: the estimate is the minimum over moving steps.
    top = estTop_ >= 0 ? std::min(estTop_, t) : t;
    bot = estBot_ >= 0 ? std::min(estBot_, b) : b;
    stayedBottom = b;
    stayedTop = t;
  }
  if (H - top - bot < opts_.minOverlapRows + 1) return noMovement(0.0);  // changed in place

  Search s = locate(cur, top, H - bot, hint, unc);
  if (s.status == StepStatus::NoMatch && !identical) {
    // Runs of identical rows at the frame edges may just be blank content that
    // did not change; header/footer bands end with informative rows, so retry
    // the search on a band that drops trailing flat rows of each run. The
    // committed (conservative) estimates stay top/bot.
    int t2 = top, b2 = bot;
    while (t2 > 0 && cur.rowFlat[t2 - 1]) --t2;
    while (b2 > 0 && cur.rowFlat[H - b2]) --b2;
    if ((t2 != top || b2 != bot) && H - t2 - b2 >= opts_.minOverlapRows + 1) {
      Search s2 = locate(cur, t2, H - b2, hint, unc);
      s2.evaluated += s.evaluated;
      if (s2.status == StepStatus::Appended ||
          (s.status == StepStatus::NoMatch && s2.status == StepStatus::Ambiguous))
        s = s2;
      else
        s.evaluated = s2.evaluated;
    }
  }
  // A bar that comes up at the top after the page has moved (a web page's
  // sticky header, shown once its title has scrolled away). It hides rows
  // that are in the image already, appended rows come from the bottom, so no
  // row is lost under it; but the header estimate only shrinks, the bar's
  // rows then contradict every offset in every later step, and a step that
  // keeps less than ten times the bar's height of overlap is refused: every
  // capture of such a page ended as "lost track" a few steps in (live pages,
  // acceptance of 2026-11-12). The run of rows that newly stays put at the top
  // is taken for a header when it shows something (window background over a
  // viewport that got shorter does not) and the rows that should have
  // scrolled into it contradict it, as for a bar at the bottom (barVerdict).
  bool topRefused = false;
  if (moved_ && stayedTop > top) {
    Search below = s;
    if ((s.status == StepStatus::NoMatch || s.status == StepStatus::Ambiguous) && H - stayedTop - bot >= opts_.minOverlapRows + 1) {
      below = locate(cur, stayedTop, H - bot, hint, unc);
      below.evaluated += s.evaluated;
    }
    if (below.status == StepStatus::Appended && below.d > 0 && headerVerdict(cur, top, stayedTop, bot, below)) {
      top = stayedTop;
      s = below;
    } else {
      topRefused = true;
    }
  }
  // The frame in which such a bar first shows has it only in itself: its top
  // rows are neither where they were nor where the page moved them. The
  // offset is then looked for in the frame's interior, by the same rules; the
  // rows this step appends are the frame's last, joined to the image by the
  // rows right above them, and what came up at the top is judged by the next
  // step (a header, or it is gone again). Not when a run at the top stayed
  // and was refused as a header above: that is a layout change.
  int interior = top;  // the first row of this frame that may be written out
  if (s.status == StepStatus::NoMatch && !identical && !topRefused) {
    const int skip = int((H - top - bot) * opts_.edgeTopFraction);
    if (skip > 0 && H - top - skip - bot >= opts_.minOverlapRows + 1) {
      Search inner = locate(cur, top + skip, H - bot, hint, unc);
      inner.evaluated += s.evaluated;
      if (inner.status == StepStatus::Appended && inner.d > 0) {
        s = inner;
        interior = top + skip;
      } else {
        s.evaluated = inner.evaluated;
      }
    }
  }
  const auto tell = [&r](const Search& found) {
    r.candidatesEvaluated = found.evaluated;
    r.score = found.score;
    r.evidenceRows = found.evidence;
    r.lowConfidence = found.lowConfidence;
    r.offset = found.d;
  };
  tell(s);
  // The layout rule, rows (see barVerdict). The run is asked about whether or
  // not the search found an offset: a bar a tenth of the overlap high leaves
  // the search with too many conflicts, and the page above the bar then says
  // how far it moved.
  bool settled = true;
  if (moved_ && stayedBottom > bot) {
    Search above = s;
    if ((s.status == StepStatus::NoMatch || s.status == StepStatus::Ambiguous) && H - top - stayedBottom >= opts_.minOverlapRows + 1)
      above = locate(cur, top, H - stayedBottom, hint, unc);
    const Bar bar = above.status == StepStatus::Appended && above.d > 0 ? barVerdict(cur, top, bot, stayedBottom, above) : Bar::Open;
    if (bar == Bar::Is) {
      // A bar that was in every frame since the page first moved (only the
      // first frame, taken before any wheel step, is without it) is how the
      // page looks once it scrolls: a footer the first step could not know.
      // The rows that step took from under it go back, the bar becomes the
      // footer, and this frame, which shows those rows above the bar, goes
      // on from the first frame's last row. A bar that comes later, after
      // rows of frames that moved without it are in the image, is a change.
      // (And, where the caller asked for it, a bar that came up in the last
      // frame alone and shows something: see lateBar.)
      const long long start = provedRows_ - (refOrigin_ + above.d);
      const bool sinceFirst = provedRows_ == firstRows_ && start >= top && start <= H - stayedBottom;
      if (sinceFirst ? !cutBackTo(provedRows_, proved_) : !lateBar(cur, top, bot, stayedBottom, above.d)) {
        r.offset = above.d;
        return layoutChanged(r, true);
      }
      s = above;
      tell(s);
      bot = stayedBottom;
    } else if (s.status == StepStatus::Appended) {
      settled = bar == Bar::IsNot;
    }
  }
  if (s.status != StepStatus::Appended) {
    if (identical) return noMovement(1.0);
    r.status = s.status;
    return r;
  }
  const int d = s.d;
  // The layout rule, columns: the offset explains the rows, but a band of
  // columns that every step so far left steady is now different in nearly
  // every row, neither moved with the page nor where it was.
  // A band with a quarter that moved as one piece by a distance of its own is
  // a side bar that sticks or comes unstuck in this step, not a repaint: the
  // own-pace rule judges it (ownPace), not this one.
  uint32_t steady = 0, own = 0;
  std::vector<uint8_t> astray;
  if (d > 0) paceOf(top, H - bot, d, astray);
  for (size_t j = 0; j < astray.size(); ++j)
    if (astray[j]) own |= 1u << (j / kFinePerBand);
  if (columnsChanged(cur, top, H - bot, d, steady, own)) return layoutChanged(r, false);
  // In-place change only. Frames alike in the compared columns say "moved
  // back" only when the pixels proved it (the margin rule), not on a hint.
  if (d == 0 || (identical && d < 0 && s.lowConfidence)) return noMovement(s.score);
  steadyBands_ = haveSteady_ ? steadyBands_ & steady : steady;
  haveSteady_ = true;

  // The first frame minus its (now known) footer; its last rows wait for the
  // next frame like any other frame's (the edge guard).
  const auto firstMovement = [&] {
    moved_ = true;
    const int guard = std::min(guardRows(H - top - bot), H - bot);
    std::vector<uint8_t> last(pending_.begin() + std::ptrdiff_t(size_t(H - bot - guard) * width_ * 4), pending_.begin() + std::ptrdiff_t(size_t(H - bot) * width_ * 4));
    flushPending(H - bot - guard);
    if (!limited_) holdRows(last.data(), std::ptrdiff_t(width_) * 4, guard);
    firstRows_ = logicalRows();
  };
  if (d > 0) {
    const long long curOrigin = refOrigin_ + d;
    const long long body = moved_ ? logicalRows() : (long long)(H - bot);
    const long long start = body - curOrigin;
    if (start < interior) {  // would leave a gap (only possible after a footer estimate shrank), or take rows from under what came up at the top
      r.status = StepStatus::NoMatch;
      return r;
    }
    estTop_ = top;
    estBot_ = bot;
    if (!moved_ && bot >= opts_.maxOutputRows) {
      flushPending(H);  // the cap cannot hold the footer: keep the first viewport's prefix
      limited_ = finished_ = true;
      r.status = StepStatus::LimitReached;
      return r;
    }
    if (!moved_) firstMovement();
    const long long before = logicalRows();
    // The layout rule, a pace of its own (see ownPace): nothing of this frame is taken.
    if (const long long cut = ownPace(astray, before); cut >= 0) {
      r.ownPace = true;
      changed_ = true;
      cutBackTo(cut, pace_);
      r.status = StepStatus::LayoutChanged;
      r.appendedRows = 0;
      return r;
    }
    if (paceBegan_) pace_.save(held_, heldRows_, bodyRows_, width_);
    if (settled) {
      provedRows_ = before;
      proved_.save(held_, heldRows_, bodyRows_, width_);
    }
    // The edge guard. Rows that waited and that this frame shows in its
    // interior are taken from it; those above are written as they were seen.
    // Then the frame's new rows, but for its own last ones, which wait.
    const int end = H - bot;
    const int trusted = std::max({interior, top + int((H - top - bot) * opts_.edgeTopFraction), trustedFrom_});
    const long long firstHeld = bodyRows_ - curOrigin;  // where this frame has the first row that waits
    const int above = int(std::clamp<long long>(trusted - firstHeld, 0, heldRows_));
    // Rows that wait and lie below this frame (it moved less than they reach) go on waiting as they are.
    const int shown = int(std::clamp<long long>(end - firstHeld, above, heldRows_));
    const std::vector<uint8_t> below(held_.begin() + std::ptrdiff_t(size_t(shown) * width_ * 4), held_.begin() + std::ptrdiff_t(size_t(heldRows_) * width_ * 4));
    const int belowRows = heldRows_ - shown;
    emitHeld(above);
    if (!limited_) {
      const int next = int(std::min<long long>(bodyRows_ - curOrigin, end));
      const int from = std::max(next, end - guardRows(H - top - bot));
      emitRows(f, next, from);
      if (!limited_) {
        holdRows(f.row(from), f.stride, end - from);
        held_.insert(held_.end(), below.begin(), below.end());
        heldRows_ += belowRows;
        // The cap counts the rows that wait as well.
        const long long reserve = std::min(std::max(estBot_, 0), opts_.maxOutputRows);
        if (logicalRows() > opts_.maxOutputRows - reserve) emitHeld(heldRows_);
      }
    }
    r.appendedRows = int(logicalRows() - before);
    lastStep_ = d;
    lastBefore_ = before;
    refOrigin_ = curOrigin;
    ref_ = cur;
    refFine_.swap(curFine_);
    refLight_.swap(curLight_);
    storeFooter(f);
    r.status = limited_ ? StepStatus::LimitReached : StepStatus::Appended;
  } else {
    estTop_ = top;
    estBot_ = bot;
    if (!moved_ && bot >= opts_.maxOutputRows) {
      flushPending(H);  // the cap cannot hold the footer: keep the first viewport's prefix
      limited_ = finished_ = true;
      r.status = StepStatus::LimitReached;
      return r;
    }
    if (!moved_) {
      firstMovement();
      provedRows_ = firstRows_;
      proved_.save(held_, heldRows_, bodyRows_, width_);
    }
    refOrigin_ += d;  // later frames append only rows below what is already emitted
    lastStep_ = 0;
    ref_ = cur;
    refFine_.swap(curFine_);
    refLight_.swap(curLight_);
    storeFooter(f);
    r.status = StepStatus::Reversed;
  }
  r.fixedTop = estTop_;
  r.fixedBottom = estBot_;
  return r;
}

// The layout inside the frame changed: nothing of this frame or a later one is
// taken. With takeBack the image also loses the rows that are not proved to be
// the page's (see barVerdict).
StepResult ScrollStitcher::layoutChanged(StepResult r, bool takeBack) {
  changed_ = true;
  if (takeBack) cutBackTo(provedRows_, proved_);
  r.status = StepStatus::LayoutChanged;
  r.appendedRows = 0;
  return r;
}

bool ScrollStitcher::takeBackTo(long long rows) {
  if (rows >= logicalRows()) return true;
  if (rows >= bodyRows_) {  // only rows that still wait (the edge guard)
    heldRows_ = int(rows - bodyRows_);
    return true;
  }
  if (sink_ && !takeBack_) return false;
  heldRows_ = 0;
  if (sink_) takeBack_(bodyRows_ - rows);
  else image_.resize(size_t(rows) * size_t(width_) * 4);
  bodyRows_ = rows;
  return true;
}

// The layout rule, rows. The footer estimate only shrinks, so a run of rows
// that newly stays where it is at the bottom of the frame (`stayed` rows, more
// than the footer `bot`) is taken for page: blank lines that happen to be
// alike, most of the time. When the page moved by d, the rows that were in
// that run are now d higher. If instead the rows d above it contradict it, the
// run did not move with the page: it is a bar that came up over the page, or
// the window's background under a viewport that got shorter, and the rows
// appended since it came were the bar's, with the page's rows hidden under it
// (acceptance of 2026-11-11: a complete-looking image with a bar in its
// body, ended as "end of content").
// Is: at least layoutStaticMinRows rows contradict the run and they outnumber
// the rows that did come out of it by more than layoutStaticRatio to one.
// IsNot: every row is accounted for. Open: neither.
// The step in which a bar first shows cannot tell (its frame is the first
// with the bar in it and its rows are already appended), so rows count as
// proved only up to the last step whose verdict was IsNot or that showed no
// such run (provedRows_), and the image is cut back to there.
ScrollStitcher::Bar ScrollStitcher::barVerdict(const FrameSignatures& cur, int top, int bot, int stayed, const Search& how) const {
  const int d = how.d;
  int against = 0, with = 0, open = 0;
  for (int y = std::max(top, height_ - stayed - d); y < height_ - bot - d; ++y) {
    const int k = rowPair(how, cur, y + d, y);
    if (k == 0 || k == -2) ++against;
    else if (k > 0) ++with;
    else if (!(ref_.rowFlat[y + d] && cur.rowFlat[y] && ref_.flatColor[y + d] == cur.flatColor[y])) ++open;
  }
  if (against >= opts_.layoutStaticMinRows && against > opts_.layoutStaticRatio * with) return Bar::Is;
  return against == 0 && open == 0 ? Bar::IsNot : Bar::Open;
}

// A pair of rows as the search that found the offset compares them: by the
// eight bands (classifyRowPair), or over the quarters columnSearch kept.
int ScrollStitcher::rowPair(const Search& how, const FrameSignatures& cur, int yRef, int yCur) const {
  if (how.kept.empty()) return classifyRowPair(ref_, yRef, cur, yCur, opts_);
  const int F = ref_.bands * kFinePerBand;
  int differ = 0;
  for (int j : how.kept) {
    const size_t a = size_t(yCur) * F + j, b = size_t(yRef) * F + j;
    differ += !(curFine_[a] == refFine_[b] || std::abs(int(curLight_[a]) - int(refLight_[b])) <= opts_.columnLightTolerance);
  }
  if (differ > int(how.kept.size()) / 4) return 0;
  return ref_.rowFlat[yRef] && cur.rowFlat[yCur] ? -1 : 1;
}

// A bar that came up at the bottom in the last frame, after rows of frames
// without it are in the image: a web page's own banner, slid in over the last
// rows of the viewport once the reader has scrolled a while (an
// encyclopedia's appeal for donations, a cookie notice; live pages,
// acceptance of 2026-11-12, where the capture of such an article ended at
// 1,611 of 24,117 rows). The layout rule ends the capture there, because the
// step that first showed the bar appended the bar's rows as page. Where the
// caller asks for it (StitchOptions::lateBarIsFooter) the bar becomes the
// footer instead and the capture goes on, when every row can be accounted for:
//  - the bar shows something (a viewport that got shorter leaves window
//    background, one flat colour, and that stays a change of layout);
//  - it came up in the last frame and no earlier: the step before was the
//    last that showed no such run (provedRows_ is what was out before it);
//  - that step moved the page at least as far as the bar is high, so the rows
//    it took from under the bar are the image's last rows and no row seen
//    before it was taken from there;
//  - and this frame shows the first of those rows above the bar.
// Those last rows are taken back; this frame, whose rows above the bar the
// search proved, supplies them. Returns false when any of that fails.
bool ScrollStitcher::lateBar(const FrameSignatures& cur, int top, int bot, int stayed, int d) {
  if (!opts_.lateBarIsFooter) return false;
  const int rows = stayed - bot;
  if (rows <= 0 || lastStep_ < rows || provedRows_ != lastBefore_) return false;
  int shows = 0;
  for (int y = height_ - stayed; y < height_ - bot; ++y) shows += !cur.rowFlat[y];
  if (shows < opts_.layoutStaticMinRows) return false;
  const long long keep = logicalRows() - rows;
  const long long start = keep - (refOrigin_ + d);
  if (keep < provedRows_ || start < top || start > height_ - stayed) return false;
  return takeBackTo(keep);
}

// The same question at the top, for a run of rows [top, run) that newly stays
// where it is: a header that came up over the page, when it shows something
// (at least layoutStaticMinRows rows that are not one flat colour) and the
// rows that should have scrolled into it contradict it.
bool ScrollStitcher::headerVerdict(const FrameSignatures& cur, int top, int run, int bot, const Search& how) const {
  const int d = how.d;
  int against = 0, with = 0, shows = 0;
  for (int y = top; y < run && y + d < height_ - bot; ++y) {
    const int k = rowPair(how, cur, y + d, y);
    if (k == 0 || k == -2) ++against;
    else if (k > 0) ++with;
    shows += !cur.rowFlat[y];
  }
  return shows >= opts_.layoutStaticMinRows && against >= opts_.layoutStaticMinRows && against > opts_.layoutStaticRatio * with;
}

// The layout rule, a pace of its own. A column that scrolls with the page at
// another rate (a code editor's minimap at a third of it, a parallax strip)
// passes the tolerant rule like a side bar that stays put, since it covers
// two bands at most; but its rows in the image are then neither the page's
// nor a fixed bar's: every step appends the strip as it happened to stand,
// and the image, reported as complete, is a mosaic there (acceptance of
// 2026-11-12: 24,000 rows, the 100 px strip wrong from row 600 on, on five
// desktops). No tall image can show such a strip: by the time the page has
// moved a viewport, the strip's row that belongs beside a page row is no
// longer on the screen. So the capture ends, and says so.
// It is proved per quarter of a band, and per row: at one distance e that is
// neither the page's nor none, at least layoutBrokenFraction of the overlap's
// rows are equal bit for bit, minDistinctRows different ones among them,
// while fewer than ownPaceMaxWith are at the page's distance and fewer where
// they were. One such step is what a side bar does when it comes unstuck or
// sticks (it moves part of the way), so it takes two in a row; the image is
// then cut back to the rows from before the first of them. Something that
// changes without moving (a video, an animation) has no such distance and is
// left to the tolerant rule as before. The side margins are not looked at.
// paceOf: per quarter band, whether this step shows it at a distance of its own.
void ScrollStitcher::paceOf(int top, int bottom, int d, std::vector<uint8_t>& astray) const {
  const int F = ref_.bands * kFinePerBand, M = bottom - top;
  astray.assign(size_t(F), 0);
  if (refFine_.size() != size_t(height_) * F || curFine_.size() != refFine_.size()) return;
  const int sliver = std::max({opts_.minOverlapRows, opts_.sliverMinRows, int(M * opts_.sliverFraction)});
  std::vector<uint64_t> rows;
  for (int j = 0; j < F; ++j) {
    // Rows of this quarter band equal at distance e; stops counting once more than `budget` differ.
    const auto equalAt = [&](int e, int budget, int& n) {
      const int y0 = top + std::max(0, -e), y1 = bottom - std::max(0, e);
      n = std::max(0, y1 - y0);
      int equal = 0, differ = 0;
      for (int y = y0; y < y1 && differ <= budget; ++y) {
        if (curFine_[size_t(y) * F + j] == refFine_[size_t(y + e) * F + j]) ++equal;
        else ++differ;
      }
      return differ > budget ? -1 : equal;
    };
    int nPage = 0, nStill = 0;
    const int withPage = equalAt(d, INT_MAX, nPage), still = equalAt(0, INT_MAX, nStill);
    if (nPage < sliver || withPage >= opts_.ownPaceMaxWith * nPage || still >= opts_.ownPaceMaxWith * nStill) continue;
    for (int e = -(M - sliver); e <= M - sliver && !astray[size_t(j)]; ++e) {
      if (std::abs(e - d) <= 1 || std::abs(e) <= 1) continue;
      int n = 0;
      const int allowed = int(std::floor((1.0 - opts_.layoutBrokenFraction) * (M - std::abs(e)) + 1e-9));
      if (equalAt(e, allowed, n) < 0) continue;
      rows.clear();
      const int y0 = top + std::max(0, -e), y1 = bottom - std::max(0, e);
      for (int y = y0; y < y1; ++y)
        if (curFine_[size_t(y) * F + j] == refFine_[size_t(y + e) * F + j]) rows.push_back(curFine_[size_t(y) * F + j]);
      std::sort(rows.begin(), rows.end());
      astray[size_t(j)] = int(std::unique(rows.begin(), rows.end()) - rows.begin()) >= opts_.minDistinctRows;
    }
  }
}

// ownPace: the rule's memory. Returns the number of rows to keep when a
// quarter band was at a distance of its own in this step and the one before, or -1.
long long ScrollStitcher::ownPace(const std::vector<uint8_t>& astray, long long before) {
  if (paceSince_.size() != astray.size()) paceSince_.assign(astray.size(), -1);
  long long cut = -1;
  for (size_t j = 0; j < astray.size(); ++j) {
    if (!astray[j]) paceSince_[j] = -1;
    else if (paceSince_[j] < 0) paceSince_[j] = before;
    else cut = cut < 0 ? paceSince_[j] : std::min(cut, paceSince_[j]);
  }
  // The earliest run still going began with this step: the caller keeps the
  // rows that wait as they are now (see cutBackTo).
  long long earliest = -1;
  for (const long long since : paceSince_)
    if (since >= 0 && (earliest < 0 || since < earliest)) earliest = since;
  paceBegan_ = earliest == before;
  return cut;
}

// The layout rule, columns. A row passes the tolerant rule with up to
// (bands - minBandsEqual) bands different, which is right for what changes in
// place in a few rows (a hover effect, a caret, a pointer) and for a column
// that never scrolls with the page (a side bar, a minimap). It is wrong when
// the page's viewport gets narrower under a rectangle that stays as it is: the
// window's background then replaces the page in the same bands of every row,
// every row still passes, and rows of two different layouts are stitched into
// one image that ends as "end of content" (acceptance of 2026-11-11: a 960 px
// rectangle over a window resized to show 760 px of the page).
// What tells the two apart is the band's own history. In a band that belongs
// to the page, or to something fixed beside it, nearly every row of the overlap
// is either where the offset says (it moved with the page) or where it was (it
// stayed): `steady`. A band that was steady in every proved step so far and in
// this step is neither in nearly every row was repainted as a whole. A band
// that never was steady (a minimap, a scrollbar's thumb wider than the side
// margin over most rows) is left to the tolerant rule as before, and so is
// anything that touches a few rows. The side margins are not looked at: a
// scrollbar fades in and out there.
// `steady`: the bands this step left steady. d == 0 asks about a change in place.
bool ScrollStitcher::columnsChanged(const FrameSignatures& cur, int top, int bottom, int d, uint32_t& steady, uint32_t own) const {
  const int y0 = top + std::max(0, -d), y1 = bottom - std::max(0, d), n = y1 - y0;
  steady = 0;
  if (n < opts_.minOverlapRows) return false;
  const int B = ref_.bands;
  int neither[32] = {};
  for (int y = y0; y < y1; ++y) {
    const uint64_t* now = &cur.bandHash[size_t(y) * B];
    const uint64_t* moved = &ref_.bandHash[size_t(y + d) * B];
    const uint64_t* stayed = &ref_.bandHash[size_t(y) * B];
    for (int k = 0; k < B; ++k) neither[k] += now[k] != moved[k] && now[k] != stayed[k];
  }
  uint32_t broken = 0;
  for (int k = 0; k < B; ++k) {
    if (neither[k] <= opts_.layoutSteadyFraction * n) steady |= 1u << k;
    if (neither[k] >= opts_.layoutBrokenFraction * n) broken |= 1u << k;
  }
  return haveSteady_ && (broken & steadyBands_ & ~own) != 0;
}

// The offset of this frame: the search over the eight bands, and where that
// finds none or cannot decide, the search by quarter bands (columnSearch). And where it finds
// "changed in place" (no offset, which needs no proof): between two side bars
// the page is what changes in a few bands of every row, and a page of short
// lines that had moved 414 px was taken for one that stood still, twice, and
// the capture ended as "end of content" a third short of the page's end
// (a reference page, real desktop, 2026-11-12). The quarter bands say whether
// the page moved after all.
ScrollStitcher::Search ScrollStitcher::locate(const FrameSignatures& cur, int top, int bottom, int hint, int unc) const {
  Search found = search(cur, top, bottom, hint, unc);
  const bool inPlace = found.status == StepStatus::Appended && found.d == 0;
  // Ambiguous as well: beside one side bar the rows of the true offset are
  // not equal bit for bit as whole rows, a sliver at the far end of the range
  // ties with it, and neither the exact rule nor the hint decides (the last
  // step of an article, 51 px to the page's end, in Chromium; real desktop,
  // 2026-11-12). The quarter bands prove the offset or leave it ambiguous.
  // And an offset the bands found is checked against the quarters: where
  // some of them show at that offset mostly not what they showed before, two
  // things moved in the frame, and the bands follow the wider one (side bars
  // on their way to where they stick, 12 px, beside a narrow page that moved
  // 300). The quarter bands say which is the page.
  const bool offset = found.status == StepStatus::Appended && found.d != 0;
  if (offset && !contradicted(top, bottom, found.d)) return found;
  if (found.status != StepStatus::NoMatch && found.status != StepStatus::Ambiguous && !inPlace && !offset) return found;
  Search byColumns = columnSearch(cur, top, bottom);
  if (byColumns.status == StepStatus::Appended) {
    if (offset && std::abs(byColumns.d - found.d) <= 1) return found;  // the quarters say the same
    byColumns.evaluated += found.evaluated;
    return byColumns;
  }
  // An offset the bands proved stands unless the quarters prove another.
  if (offset) return found;
  // "In place" stands only while nothing in the frame moved as a piece. Where
  // columns did and no offset is proved for the page, the step is not known
  // to be none: a reference frame taken here would lose the rows in between
  // (792 rows of a reference page went missing that way, in an image that
  // ended as "end of content"; GNOME, 2026-11-12).
  if (inPlace && byColumns.status == StepStatus::Ambiguous) found.status = StepStatus::Ambiguous;
  return found;
}

// Whether at distance d at least two quarter bands show, in the rows in which
// they show anything, mostly not what they showed before. Such a quarter must
// show something in an eighth of the rows at least: a hover band, a caret or
// a scrollbar's thumb over blank paper is a few rows and is what the tolerant
// rule is for.
bool ScrollStitcher::contradicted(int top, int bottom, int d) const {
  const int F = ref_.bands * kFinePerBand;
  if (refFine_.size() != size_t(height_) * F || curFine_.size() != refFine_.size()) return false;
  const int y0 = top + std::max(0, -d), y1 = bottom - std::max(0, d);
  const int shownMin = std::max({8, opts_.minOverlapRows / 3, (y1 - y0) / 8});
  int against = 0;
  for (int j = 0; j < F && against < 2; ++j) {
    int count = 0, equal = 0;
    for (int y = y0; y < y1; ++y) {
      const size_t a = size_t(y) * F + j, b = size_t(y + d) * F + j;
      // Shows something: not the row above it over again in both frames, or not the same as it was.
      const bool same = curFine_[a] == refFine_[b] || std::abs(int(curLight_[a]) - int(refLight_[b])) <= opts_.columnLightTolerance;
      const bool flat = y > y0 && curFine_[a] == curFine_[a - F] && refFine_[b] == refFine_[b - F];
      if (same && flat) continue;
      ++count;
      equal += same;
    }
    against += count >= shownMin && equal < opts_.ownPaceMaxWith * count;
  }
  return against >= 2;
}

// The search by quarter bands, for a page between side bars. The tolerant
// rule forgives two bands of eight that do not move with the page, which is
// one side bar. A page with one at each side (an encyclopedia article between
// its contents and its tools, a reference page between two navigation
// columns) has four to six such bands; every row with something in a side bar
// then contradicts every offset, and the capture ended at its first step
// (live pages, acceptance of 2026-11-12). Which columns are not the page is
// proved here instead of allowed for: a quarter band is the page's when at
// one distance d at least layoutBrokenFraction of the overlap's rows are
// equal bit for bit, minDistinctRows different ones among them; it is
// something else when it is so at another distance and not at d: where it
// was (a bar that stays), or part of the way (a bar that sticks or comes
// unstuck in this step). The page is what moved nearest the pointer: outward
// from the quarter the wheel is turned over, the first quarters that moved
// say by how much, as one piece or, where their lines are too short to prove
// a distance, by the one distance at which most of what they show is equal
// bit for bit (columnMovedFraction, no other above exactTieMaxRival). What
// moves beside the page, however wide, is farther from the pointer. The rows
// are then compared over every quarter that is not proved to be something
// else, by the tolerant rule's own measure (a quarter of them may differ),
// and the offset has to pass what any offset has to: the score, the number of
// evidence rows, different rows among them, more than a sliver of overlap,
// and no rival. Rows blank in every quarter that counts are no evidence.
// Where something moved near the pointer and no offset passes, the answer is
// Ambiguous, never "nothing moved". The pixels alone decide; no hint is heard.
// A quarter of a row is the same here when it is equal bit for bit, or as
// bright within columnLightTolerance. A browser does not always draw a line
// of text the same way twice: in a block that scrolls sideways (a code
// listing) the same glyphs come out with other antialiasing after the page
// has moved, 150 of 783 rows in one step of a documentation page, and every
// such capture ended there (Firefox 158, acceptance of 2026-11-12). The same
// line drawn again is as bright in every quarter to within two of 765; another
// line is not: one row off, a quarter of the rows had more than a quarter of
// their quarters further apart than that. Rows equal bit for bit must still
// be there, different ones, for a column to count and for an offset to pass.
ScrollStitcher::Search ScrollStitcher::columnSearch(const FrameSignatures& cur, int top, int bottom) const {
  Search out;
  const int F = ref_.bands * kFinePerBand, M = bottom - top;
  if (refFine_.size() != size_t(height_) * F || curFine_.size() != refFine_.size()) return out;
  const int sliver = std::max({opts_.minOverlapRows, opts_.sliverMinRows, int(M * opts_.sliverFraction)});
  const int Dmax = M - sliver;
  if (Dmax < 1) return out;
  const auto same = [&](int y, int e, int j) {
    const size_t a = size_t(y) * F + j, b = size_t(y + e) * F + j;
    return curFine_[a] == refFine_[b] || std::abs(int(curLight_[a]) - int(refLight_[b])) <= opts_.columnLightTolerance;
  };
  // Each quarter's ground: the row it shows most often (blank paper, a
  // listing's background). A quarter of a row shows something when it is not that.
  std::vector<uint64_t> curGround(static_cast<size_t>(F)), refGround(static_cast<size_t>(F));
  std::vector<int> distinct(static_cast<size_t>(F));
  std::vector<uint64_t> rows;
  const auto ground = [&](const std::vector<uint64_t>& fine, int j, int* different) {
    rows.clear();
    for (int y = top; y < bottom; ++y) rows.push_back(fine[size_t(y) * F + j]);
    std::sort(rows.begin(), rows.end());
    uint64_t best = rows.front();
    int bestRun = 0, kinds = 0;
    for (size_t i = 0; i < rows.size();) {
      size_t k = i;
      while (k < rows.size() && rows[k] == rows[i]) ++k;
      ++kinds;
      if (int(k - i) > bestRun) {
        bestRun = int(k - i);
        best = rows[i];
      }
      i = k;
    }
    if (different) *different = kinds;
    return best;
  };
  for (int j = 0; j < F; ++j) {
    curGround[size_t(j)] = ground(curFine_, j, &distinct[size_t(j)]);
    refGround[size_t(j)] = ground(refFine_, j, nullptr);
  }
  // Of the rows in which quarter j shows something in either frame at distance e: how many, and how many are the same.
  const auto shown = [&](int j, int e, int stride, int& count, bool exact = false) {
    const int y0 = top + std::max(0, -e), y1 = bottom - std::max(0, e);
    int equal = 0;
    count = 0;
    for (int y = y0; y < y1; y += stride) {
      if (curFine_[size_t(y) * F + j] == curGround[size_t(j)] && refFine_[size_t(y + e) * F + j] == refGround[size_t(j)]) continue;
      ++count;
      equal += exact ? curFine_[size_t(y) * F + j] == refFine_[size_t(y + e) * F + j] : same(y, e, j);
    }
    return equal;
  };
  // The distances at which each quarter band moved as one piece; whether it
  // stayed where it was; whether what it shows changed.
  std::vector<std::vector<int>> moved(static_cast<size_t>(F));
  std::vector<uint8_t> still(static_cast<size_t>(F), 0), changed(static_cast<size_t>(F), 0);
  const int shownMin = std::max(8, opts_.minOverlapRows / 3);
  for (int j = 0; j < F; ++j) {
    int count = 0;
    const int equal = shown(j, 0, 1, count);
    still[size_t(j)] = count >= shownMin && equal >= opts_.layoutBrokenFraction * count;
    changed[size_t(j)] = count >= shownMin && equal < opts_.ownPaceMaxWith * count;
    if (distinct[size_t(j)] < opts_.minDistinctRows) continue;  // nothing to tell it by
    for (int e = -Dmax; e <= Dmax; ++e) {
      const int y0 = top + std::max(0, -e), y1 = bottom - std::max(0, e);
      const int allowed = int(std::floor((1.0 - opts_.layoutBrokenFraction) * (y1 - y0) + 1e-9));
      int differ = 0;
      for (int y = y0; y < y1 && differ <= allowed; ++y) differ += !same(y, e, j);
      if (differ > allowed) continue;
      rows.clear();
      for (int y = y0; y < y1; ++y)
        if (curFine_[size_t(y) * F + j] == refFine_[size_t(y + e) * F + j]) rows.push_back(curFine_[size_t(y) * F + j]);
      std::sort(rows.begin(), rows.end());
      if (int(std::unique(rows.begin(), rows.end()) - rows.begin()) >= opts_.minDistinctRows) moved[size_t(j)].push_back(e);
    }
    // A column that repeats moves "as one piece" at every multiple of its pitch: it tells nothing.
    if (moved[size_t(j)].size() > 8) moved[size_t(j)].clear();
  }
  const auto at = [&](int j, int d) { return std::find(moved[size_t(j)].begin(), moved[size_t(j)].end(), d) != moved[size_t(j)].end(); };
  // The quarter under the pointer (the middle of the frame when no one said).
  const int wheelX = std::clamp(wheelColumn_ >= 0 ? wheelColumn_ : width_ / 2, ref_.colStart, std::max(ref_.colStart, ref_.colEnd - 1));
  int under = 0;
  {
    const int n = ref_.colEnd - ref_.colStart, B = ref_.bands;
    const int k = std::min(B - 1, int((long long)(wheelX - ref_.colStart) * B / std::max(1, n)));
    const int x0 = ref_.colStart + int((long long)n * k / B), x1 = ref_.colStart + int((long long)n * (k + 1) / B);
    under = k * kFinePerBand + std::min(kFinePerBand - 1, int((long long)(wheelX - x0) * kFinePerBand / std::max(1, x1 - x0)));
  }
  struct Passing {
    int d, evidence;
    double score, exact;  // exact: the share of the rows that show something that are equal bit for bit
    int exactRows;
    std::vector<int> kept;
  };
  std::vector<Passing> passing;
  // A quarter that changed and has no rows enough of its own to prove a
  // distance by (short lines) may still name one: the distance at which most
  // of what it shows is equal bit for bit, when no other comes near it. One
  // that is itself nowhere changed in place (a video). 0: none; found once.
  std::vector<int> named(static_cast<size_t>(F), INT_MIN);
  const auto nameOf = [&](int j) {
    int& name = named[size_t(j)];
    if (name != INT_MIN) return name;
    name = 0;
    if (!changed[size_t(j)] || !moved[size_t(j)].empty()) return name;
    int bestD = 0;
    double best = 0, second = 0;
    for (int e = -Dmax; e <= Dmax; ++e) {
      if (e == 0) continue;
      int count = 0;
      int equal = shown(j, e, 4, count, true);  // every fourth row first
      if (count < shownMin / 2 || equal < opts_.ownPaceMaxWith * count) continue;
      equal = shown(j, e, 1, count, true);
      if (count < shownMin) continue;
      const double share = double(equal) / count;
      if (share > best) {
        if (std::abs(e - bestD) > 1) second = best;
        best = share;
        bestD = e;
      } else if (std::abs(e - bestD) > 1 && share > second) {
        second = share;
      }
    }
    if (best >= opts_.columnMovedFraction && second <= opts_.exactTieMaxRival) name = bestD;
    return name;
  };
  // One distance as the page's: every quarter that is not proved to be
  // something else (it stayed and shows something, or moved by another
  // distance) is compared row by row.
  std::vector<int> tried;
  const auto consider = [&](int d) {
    if (d == 0 || std::abs(d) > Dmax || std::find(tried.begin(), tried.end(), d) != tried.end()) return;
    tried.push_back(d);
    std::vector<int> kept;
    for (int j = 0; j < F; ++j) {
      if (at(j, d)) kept.push_back(j);
      else if (!moved[size_t(j)].empty() || still[size_t(j)]) continue;
      else if (named[size_t(j)] != INT_MIN && named[size_t(j)] != 0 && std::abs(named[size_t(j)] - d) > 1) continue;
      else kept.push_back(j);
    }
    if (kept.empty()) return;
    const int mayDiffer = int(kept.size()) / 4;
    const int y0 = top + std::max(0, -d), y1 = bottom - std::max(0, d);
    const int budget = int(std::floor((1.0 - opts_.minScore) * (y1 - y0) + 1e-9));
    int match = 0, conflict = 0, showing = 0, exactShowing = 0;
    rows.clear();
    for (int y = y0; y < y1 && conflict <= budget; ++y) {
      int differ = 0;
      bool exact = true, shows = false;
      uint64_t row = kFnvOffset;
      for (int j : kept) {
        const uint64_t a = curFine_[size_t(y) * F + j], b = refFine_[size_t(y + d) * F + j];
        differ += !same(y, d, j);
        exact = exact && a == b;
        shows = shows || a != curGround[size_t(j)] || b != refGround[size_t(j)];
        row = (row ^ a) * kFnvPrime;
      }
      if (differ > mayDiffer) {
        ++conflict;
        showing += shows;
        continue;
      }
      if (!shows) continue;  // blank in every quarter that counts: no evidence
      ++match;
      ++showing;
      if (exact) {
        ++exactShowing;
        rows.push_back(row);  // only rows equal bit for bit count as different rows
      }
    }
    ++out.evaluated;
    if (conflict > budget || match + conflict < opts_.minOverlapRows) return;
    const double score = double(match) / (match + conflict);
    std::sort(rows.begin(), rows.end());
    if (score < opts_.minScore || int(std::unique(rows.begin(), rows.end()) - rows.begin()) < opts_.minDistinctRows) return;
    passing.push_back({d, match + conflict, score, showing ? double(exactShowing) / showing : 0.0, exactShowing, kept});
  };
  // The page is what moved nearest the pointer: outward from the quarter the
  // wheel was turned over, the first quarters that moved (as a piece, or by a
  // distance they name) say by how much. Side bars that stay, or that move
  // part of the way, are farther off, however wide they are.
  // A quarter that is where it was has not moved, whatever other distance it
  // fits as well: a column of line numbers' tens digits is the same one line
  // up and one line down, and at the end of a text, where the wheel moves
  // nothing, it was taken for a column that moved and nothing proved it (a
  // GTK 4 text view on Plasma had all 1,500 lines and ended lost_track;
  // real desktop, 2026-11-12). The nearest quarter that tells anything and
  // stayed ends the search: nothing moved under the pointer.
  bool somethingMoved = false, stayed = false;
  for (int reach = 0; reach < F && !somethingMoved && !stayed; ++reach) {
    for (int j : {under - reach, under + reach}) {
      if (j < 0 || j >= F || (reach == 0 && j != under)) continue;
      if (at(j, 0) || still[size_t(j)]) {
        stayed = true;
      } else {
        for (int e : moved[size_t(j)]) {
          somethingMoved = true;
          consider(e);
        }
        if (moved[size_t(j)].empty() && nameOf(j) != 0) {
          somethingMoved = true;
          consider(nameOf(j));
        }
      }
      if (reach == 0) break;
    }
  }
  if (passing.empty()) {
    // Something moved near the pointer and no offset is proved: not known to be none.
    if (somethingMoved) out.status = StepStatus::Ambiguous;
    return out;
  }
  // One distance, or the one whose rows are equal bit for bit where the
  // others' mostly are not (the exact rule's measure).
  const Passing* best = &passing.front();
  for (const Passing& p : passing)
    if (p.exact > best->exact || (p.exact == best->exact && p.exactRows > best->exactRows)) best = &p;
  for (const Passing& p : passing) {
    if (std::abs(p.d - best->d) <= 1) continue;
    if (best->exact < opts_.exactTieMinFraction || p.exact > opts_.exactTieMaxRival) {
      out.status = StepStatus::Ambiguous;  // two offsets explain the frame
      return out;
    }
  }
  out.status = StepStatus::Appended;
  out.d = best->d;
  out.score = best->score;
  out.evidence = best->evidence;
  out.kept = best->kept;
  return out;
}

int ScrollStitcher::maxEvidenceStep() const {
  if (!hasRef_) return -1;
  const int top = movingBandTop(), bottom = movingBandBottom();
  // In thirds of a row. A row with ink in more bands than the noise tolerance
  // counts whole. A row with ink in fewer (a short line of text) matches only
  // when it is equal bit for bit, and a caret or spinner on a blank row looks
  // the same and never matches, so such rows count a third: a block of short
  // lines is evidence, a caret is not.
  const int need = 3 * (opts_.minOverlapRows + opts_.minOverlapRows / 2);
  const uint32_t all = opts_.bands >= 32 ? ~0u : ((1u << opts_.bands) - 1);
  const int tol = tolerance(opts_, opts_.bands);
  const int partial = std::max(8, opts_.minOverlapRows / 2);
  int count = 0, wide = 0, partialY = -1;
  for (int y = bottom - 1; y >= top; --y) {
    const int inked = std::popcount(~ref_.flatBandMask[y] & all);
    count += inked > tol ? 3 : inked > 0 ? 1 : 0;
    wide += inked > tol ? 1 : 0;
    if (wide == partial && partialY < 0) partialY = y;
    if (count >= need) return y - top;
  }
  // Fewer rows than wanted: still keep the lowest `partial` of them in the
  // overlap so the next step has partial evidence (useful when content enters
  // at the bottom below a tall flat region).
  return partialY >= 0 ? partialY - top : -1;
}

// The margin rule. The side margins are left out of the row comparison because
// a scrollbar lives there, and in a wide selection so do the line numbers of an
// editor or the first characters of short lines: the only columns that differ
// from line to line, while the compared columns repeat with the line pitch
// (a full-width Qt text view of 1,500 numbered lines ended as "lost track" on
// its first step; real desktop run of 2026-11-10). Among candidates the
// compared columns cannot tell apart, one is proved by a margin when
//  - that margin's rows did not stay where they were (a fixed side bar or a
//    scrollbar is equal at the same y and proves nothing about the page),
//  - at the candidate at least exactTieMinFraction of the margin's informative
//    rows, and at least minOverlapRows of them, are equal bit for bit, their
//    compared columns the same under the tolerant rule,
//  - and at every rival outside +-1 at most exactTieMaxRival are, counted over
//    at least marginRivalMinRows informative rows.
// The two margins are judged apart and must not contradict each other.
// `exact`: the compared columns of a row must be equal bit for bit as well, not
// only under the tolerant rule. That is the rule for a number the margin's edge cuts in two (a
// 1,260 px Qt text view on Budgie, lab guest of 2026-11-11: the tens digit of
// `Row 0042` in the margin, the units digit beside it). Ten lines away the
// compared columns are then equal on most rows and the margin is not; three
// lines away the margin is equal on seven lines of ten and the tolerant rule
// passes the other digit. Neither part alone tells the offsets apart; a row
// equal in both does. Returns INT_MIN when no offset is proved.
int ScrollStitcher::marginVerdict(const FrameSignatures& cur, int top, int bottom,
                                  const std::vector<int>& candidates, bool exact) const {
  if (candidates.size() < 2) return INT_MIN;
  int verdict = INT_MIN;
  std::vector<double> fraction(candidates.size());
  for (int side = 0; side < 2; ++side) {
    const uint8_t bit = uint8_t(1u << side);
    // Rows of the overlap at offset d in which this margin shows anything, and
    // how many of them are equal (with their compared columns when asked).
    auto count = [&](int d, bool wholeRow, int& informative) {
      const int y0 = top + std::max(0, -d), y1 = bottom - std::max(0, d);
      int equal = 0;
      informative = 0;
      for (int y = y0; y < y1; ++y) {
        if ((ref_.sideFlat[y + d] & bit) && (cur.sideFlat[y] & bit)) continue;
        ++informative;
        equal += ref_.sideHash[size_t(y + d) * 2 + side] == cur.sideHash[size_t(y) * 2 + side] &&
                 (!wholeRow || (exact ? ref_.rowHash[y + d] == cur.rowHash[y] : sameRowLoose(ref_, y + d, cur, y, opts_)));
      }
      return equal;
    };
    int informative = 0;
    const int still = count(0, false, informative);
    if (informative < opts_.minOverlapRows) continue;
    const double stillFraction = double(still) / informative;
    // With `exact` a margin that mostly stayed where it was is still heard (a
    // step of whole lines leaves `Row 00` and the tens digit of most lines in
    // place), but only when it fits the winner better than it fits standing
    // still, which a fixed side bar and a scrollbar never do (below).
    if (!exact && stillFraction > opts_.exactTieMaxRival) continue;
    int bestD = INT_MIN, bestRows = 0;
    double best = -1;
    bool judged = true;
    for (size_t i = 0; judged && i < candidates.size(); ++i) {
      const int equal = count(candidates[i], true, informative);
      if (informative < std::min(opts_.marginRivalMinRows, opts_.minOverlapRows)) judged = false;
      else if ((fraction[i] = double(equal) / informative) > best) {
        best = fraction[i];
        bestD = candidates[i];
        bestRows = informative;
      }
    }
    if (!judged || best < opts_.exactTieMinFraction || bestRows < opts_.minOverlapRows) continue;
    for (size_t i = 0; judged && i < candidates.size(); ++i)
      if (std::abs(candidates[i] - bestD) > 1 && fraction[i] > opts_.exactTieMaxRival) judged = false;
    if (!judged) continue;
    if (exact) {
      const int alone = count(bestD, false, informative);
      if (informative <= 0 || double(alone) / informative <= stillFraction) continue;
    }
    // The winner's equal margin rows must be different rows, and its overlap
    // more than a sliver, like any other proof.
    {
      const int M = bottom - top;
      if (M - std::abs(bestD) < std::max(opts_.sliverMinRows, int(M * opts_.sliverFraction))) continue;
      std::vector<uint64_t> rows;
      const int y0 = top + std::max(0, -bestD), y1 = bottom - std::max(0, bestD);
      for (int y = y0; y < y1; ++y) {
        const uint64_t sideRow = cur.sideHash[size_t(y) * 2 + side];
        if (((ref_.sideFlat[y + bestD] & bit) && (cur.sideFlat[y] & bit)) || ref_.sideHash[size_t(y + bestD) * 2 + side] != sideRow) continue;
        if (!exact) rows.push_back(sideRow);
        // The row as a whole: its margin and its compared columns.
        else if (ref_.rowHash[y + bestD] == cur.rowHash[y]) rows.push_back(fmix64(sideRow) ^ cur.rowHash[y]);
      }
      std::sort(rows.begin(), rows.end());
      if (int(std::unique(rows.begin(), rows.end()) - rows.begin()) < opts_.minDistinctRows) continue;
    }
    if (verdict != INT_MIN && std::abs(verdict - bestD) > 1) return INT_MIN;
    if (verdict == INT_MIN) verdict = bestD;
  }
  return verdict;
}

ScrollStitcher::Search ScrollStitcher::search(const FrameSignatures& cur, int top, int bottom,
                                              int hint, int unc) const {
  unc = std::max(0, unc);
  Search out;
  const int M = bottom - top;
  const int Dmax = M - opts_.minOverlapRows;
  if (Dmax < 0) return out;
  const int N = 2 * Dmax + 1;
  std::vector<int> mm(size_t(N), 0), cc(size_t(N), 0), ff(size_t(N), 0);
  std::vector<uint8_t> state(size_t(N), 0);  // 0 unvisited, 1 evaluated, 2 aborted
  std::vector<int> order;
  order.reserve(size_t(N));
  auto push = [&](int d) {
    if (d < -Dmax || d > Dmax || state[size_t(d + Dmax)]) return;
    state[size_t(d + Dmax)] = 3;  // queued
    order.push_back(d);
  };
  const bool hasHint = hint >= 0;
  if (hasHint) {
    push(hint);
    for (int k = 1; k <= 16; ++k) {
      push(hint - k);
      push(hint + k);
    }
  }
  for (int a = 0; a <= Dmax; ++a) {
    push(a);
    push(-a);
  }

  double best = -1.0;
  for (int d : order) {
    const double sThresh = std::max(opts_.minScore, best - opts_.uniquenessMargin);
    const int overlap = M - std::abs(d);
    const int budget = int(std::floor((1.0 - sThresh) * overlap + 1e-9));
    const int y0 = top + std::max(0, -d), y1 = bottom - std::max(0, d);
    int m = 0, c = 0, fc = 0;
    bool aborted = false;
    for (int y = y0; y < y1; ++y) {
      const int k = classifyRowPair(ref_, y + d, cur, y, opts_);
      if (k > 0) {
        ++m;
      } else if (k == -2) {
        ++fc;
      } else if (k == 0 && ++c > budget) {
        aborted = true;
        break;
      }
    }
    const size_t i = size_t(d + Dmax);
    mm[i] = m;
    cc[i] = c;
    ff[i] = fc;
    state[i] = aborted ? 2 : 1;
    ++out.evaluated;
    if (!aborted && m + c >= opts_.minOverlapRows) {
      const double s = double(m) / double(m + c);
      if (s >= opts_.minScore && s > best) best = s;
    }
  }

  auto scoreOf = [&](int d) {
    const size_t i = size_t(d + Dmax);
    const int n = mm[i] + cc[i];
    return n ? double(mm[i]) / n : 0.0;
  };
  auto passing = [&](int d) {
    const size_t i = size_t(d + Dmax);
    return state[i] == 1 && mm[i] + cc[i] >= opts_.minOverlapRows && scoreOf(d) >= opts_.minScore;
  };
  auto evidence = [&](int d) { return mm[size_t(d + Dmax)] + cc[size_t(d + Dmax)]; };
  auto closeness = [&](int d) { return hasHint ? std::abs(d - hint) : std::abs(d); };
  // Proof, as opposed to consistency (see StitchOptions::minDistinctRows).
  auto sliver = [&](int d) { return M - std::abs(d) < std::max(opts_.sliverMinRows, int(M * opts_.sliverFraction)); };
  auto proved = [&](int d) {
    if (sliver(d)) return false;
    std::vector<uint64_t> rows;
    const int y0 = top + std::max(0, -d), y1 = bottom - std::max(0, d);
    for (int y = y0; y < y1; ++y)
      if (classifyRowPair(ref_, y + d, cur, y, opts_) > 0) rows.push_back(cur.rowHash[y]);
    std::sort(rows.begin(), rows.end());
    return int(std::unique(rows.begin(), rows.end()) - rows.begin()) >= opts_.minDistinctRows;
  };
  // A candidate the pixels agree with but do not prove is taken only when the
  // caller's hint says the same, and then with low confidence.
  auto nearHint = [&](int d) {
    return hasHint && std::abs(d - hint) <= std::max(opts_.hintTolerancePx, int(std::lround(opts_.hintTolerance * hint)));
  };

  int bestD = INT_MIN;
  for (int d = -Dmax; d <= Dmax; ++d) {
    if (!passing(d)) continue;
    if (bestD == INT_MIN) {
      bestD = d;
      continue;
    }
    const double sd = scoreOf(d), sb = scoreOf(bestD);
    if (sd > sb || (sd == sb && (evidence(d) > evidence(bestD) ||
                                 (evidence(d) == evidence(bestD) && closeness(d) < closeness(bestD)))))
      bestD = d;
  }

  if (bestD != INT_MIN) {
    const double sb = scoreOf(bestD);
    std::vector<int> topSet;
    for (int d = -Dmax; d <= Dmax; ++d)
      if (passing(d) && scoreOf(d) >= sb - opts_.uniquenessMargin) topSet.push_back(d);
    bool rivals = false;
    for (int d : topSet) rivals |= std::abs(d - bestD) > 1;
    if (!rivals) {
      out.d = bestD;
      out.score = sb;
      out.evidence = evidence(bestD);
      // No movement needs no proof; an offset does.
      if (bestD == 0 || proved(bestD)) {
        out.status = StepStatus::Appended;
      } else if (nearHint(bestD)) {
        out.status = StepStatus::Appended;
        out.lowConfidence = true;
      } else {
        out.status = StepStatus::Ambiguous;
      }
      return out;
    }
    out.d = bestD;
    out.score = sb;
    out.evidence = evidence(bestD);
    out.status = StepStatus::Ambiguous;
    // The rivals pass the tolerant rule alike. That rule takes a row whose
    // difference is confined to two bands for the same row, which is right
    // for a hover effect and wrong for text whose lines differ in a few
    // characters (numbered lines, a log, a table): every multiple of the
    // line pitch then explains the frame. The pixels themselves still tell
    // them apart: at the true offset the informative rows are equal bit for
    // bit, at such a rival a line's rows are not. The exact rule decides only
    // when one candidate (with its +-1 neighbours) is nearly all exact and
    // every rival is mostly not: content that repeats exactly keeps rivals
    // that are exact too and stays ambiguous, and so does a step whose
    // overlap is mostly an in-place change.
    {
      int exactD = INT_MIN;
      double exactBest = -1;
      bool decided = true;
      std::vector<double> fraction(topSet.size(), 0.0);
      for (size_t i = 0; i < topSet.size(); ++i) {
        const int d = topSet[i];
        const int y0 = top + std::max(0, -d), y1 = bottom - std::max(0, d);
        int equal = 0, informative = 0;
        for (int y = y0; y < y1; ++y) {
          if (ref_.rowFlat[y + d] && cur.rowFlat[y]) continue;
          ++informative;
          equal += ref_.rowHash[y + d] == cur.rowHash[y];
        }
        if (informative < opts_.minOverlapRows) {
          decided = false;  // a candidate the exact rule cannot judge
          break;
        }
        fraction[i] = double(equal) / informative;
        if (fraction[i] > exactBest) {
          exactBest = fraction[i];
          exactD = d;
        }
      }
      decided = decided && exactBest >= opts_.exactTieMinFraction;
      for (size_t i = 0; decided && i < topSet.size(); ++i)
        if (std::abs(topSet[i] - exactD) > 1 && fraction[i] > opts_.exactTieMaxRival) decided = false;
      // Exact rows in a sliver, or rows that are all alike, are a coincidence
      // waiting to happen (a static overlay spoils the true offset's rows and
      // a rival 90 lines away wins on five clean ones; real desktop run of
      // 2026-11-10).
      decided = decided && (exactD == 0 || proved(exactD));
      if (decided) {
        out.status = StepStatus::Appended;
        out.d = exactD;
        out.score = scoreOf(exactD);
        out.evidence = evidence(exactD);
        return out;
      }
    }
    // The compared columns cannot tell the rivals apart; the margins may, by
    // themselves or together with the compared columns.
    int byMargin = marginVerdict(cur, top, bottom, topSet, false);
    if (byMargin == INT_MIN) byMargin = marginVerdict(cur, top, bottom, topSet, true);
    if (const int d = byMargin; d != INT_MIN) {
      out.status = StepStatus::Appended;
      out.d = d;
      out.score = scoreOf(d);
      out.evidence = evidence(d);
      return out;
    }
    if (!hasHint) return out;
    int c1 = topSet.front();
    for (int d : topSet)
      if (std::abs(d - hint) < std::abs(c1 - hint) ||
          (std::abs(d - hint) == std::abs(c1 - hint) && scoreOf(d) > scoreOf(c1)))
        c1 = d;
    const int dist = std::abs(c1 - hint);
    // Among offsets the pixels cannot tell apart the hint decides only close
    // by: within what the wheel's own spread allows and tieHintTolerance of
    // the hint. A quarter of the hint was allowed; on a page of tables that
    // repeat every 505 px a step that had moved 924 px for a hint of 528
    // (three wheel steps arrived late) was then taken for one of 419, a table
    // went missing from the image, and the capture ended as "end of content"
    // (a pull request page on GNOME, 2026-11-12). Farther off than this the
    // wheel did something else than was asked, and the step is ambiguous.
    const int tol = std::max(opts_.hintTolerancePx, unc + int(std::lround(opts_.tieHintTolerance * hint)));
    bool ok = dist <= tol;
    for (int d : topSet)
      if (std::abs(d - c1) > 1 && std::abs(d - hint) <= std::max(2 * dist + 2, dist + 2 * unc)) ok = false;
    if (!ok) return out;
    int pick = c1;
    for (int d : topSet)
      if (std::abs(d - c1) <= 1 && scoreOf(d) > scoreOf(pick)) pick = d;
    out.status = StepStatus::Appended;
    out.d = pick;
    out.score = scoreOf(pick);
    out.evidence = evidence(pick);
    out.lowConfidence = true;
    return out;
  }

  // No candidate has enough evidence. Conflict-free candidates are consistent
  // with the pixels but unproven (blank or flat overlap).
  bool anyConflictFree = false;
  double bestRejected = 0;
  for (int d = -Dmax; d <= Dmax; ++d) {
    const size_t i = size_t(d + Dmax);
    if (state[i] == 1 && cc[i] + ff[i] == 0) anyConflictFree = true;
    if (state[i] == 1 && scoreOf(d) > bestRejected) {
      bestRejected = scoreOf(d);
      out.d = d;
      out.evidence = evidence(d);
    }
  }
  out.score = bestRejected;
  {
    // Nothing informative in the compared columns (a wide selection whose only
    // ink is at its left edge): the margins may still prove one offset.
    std::vector<int> free;
    for (int d = -Dmax; d <= Dmax; ++d)
      if (state[size_t(d + Dmax)] == 1 && cc[size_t(d + Dmax)] + ff[size_t(d + Dmax)] == 0) free.push_back(d);
    if (const int d = marginVerdict(cur, top, bottom, free, false); d != INT_MIN) {
      out.status = StepStatus::Appended;
      out.d = d;
      out.score = 1.0;
      out.evidence = evidence(d);
      return out;
    }
  }
  if (hasHint) {
    // Partial evidence: conflict-free candidates with some matched informative
    // rows (fewer than minOverlapRows). Accepted only next to the hint and only
    // when no other such candidate competes.
    const int partialMin = std::max(8, opts_.minOverlapRows / 2);
    const int tol = std::max(opts_.hintTolerancePx, int(std::lround(opts_.hintTolerance * hint)));
    int c1 = INT_MIN;
    for (int d = -Dmax; d <= Dmax; ++d) {
      const size_t i = size_t(d + Dmax);
      if (state[i] != 1 || cc[i] + ff[i] != 0 || mm[i] < partialMin) continue;
      if (c1 == INT_MIN || std::abs(d - hint) < std::abs(c1 - hint)) c1 = d;
    }
    if (c1 != INT_MIN && std::abs(c1 - hint) <= tol) {
      const int dist = std::abs(c1 - hint);
      bool ok = true;
      int pick = c1;
      for (int d = -Dmax; d <= Dmax; ++d) {
        const size_t i = size_t(d + Dmax);
        if (state[i] != 1 || cc[i] + ff[i] != 0 || mm[i] < partialMin) continue;
        if (std::abs(d - c1) > 1 && std::abs(d - hint) <= std::max(2 * dist + 2, dist + 2 * unc)) ok = false;
        if (std::abs(d - c1) <= 1 && mm[i] > mm[size_t(pick + Dmax)]) pick = d;
      }
      if (ok) {
        out.status = StepStatus::Appended;
        out.d = pick;
        out.score = 1.0;
        out.evidence = evidence(pick);
        out.lowConfidence = true;
        return out;
      }
    }
  }
  if (!hasHint) {
    // Without a hint, a single conflict-free candidate (+-1) with at least 3/4
    // of minOverlapRows matched rows, and no other conflict-free candidate with
    // any matched row, is accepted with low confidence.
    const int need = std::max(8, (opts_.minOverlapRows * 3 + 3) / 4);
    int c1 = INT_MIN;
    bool unique = true;
    for (int d = -Dmax; d <= Dmax; ++d) {
      const size_t i = size_t(d + Dmax);
      if (state[i] != 1 || cc[i] + ff[i] != 0 || mm[i] < need) continue;
      if (c1 == INT_MIN || mm[i] > mm[size_t(c1 + Dmax)]) c1 = d;
    }
    if (c1 != INT_MIN) {
      for (int d = -Dmax; d <= Dmax; ++d) {
        const size_t i = size_t(d + Dmax);
        if (state[i] == 1 && cc[i] + ff[i] == 0 && mm[i] > 0 && std::abs(d - c1) > 1) unique = false;
      }
      if (unique && !sliver(c1)) {
        out.status = StepStatus::Appended;
        out.d = c1;
        out.score = 1.0;
        out.evidence = evidence(c1);
        out.lowConfidence = true;
        return out;
      }
    }
  }
  if (hasHint) {
    // Blind step: no candidate has real evidence. Take the conflict-free
    // candidate nearest the hint (preferring one with some matched rows). The
    // hint itself is used when it is conflict-free; a hint that the pixels
    // contradict is corrected to the nearest consistent offset.
    const int tol = std::max(opts_.hintTolerancePx, int(std::lround(opts_.hintTolerance * hint)));
    int pick = INT_MIN;
    auto better = [&](int d, int p) {
      const size_t i = size_t(d + Dmax), j = size_t(p + Dmax);
      if ((mm[i] > 0) != (mm[j] > 0)) return mm[i] > 0;
      if (std::abs(d - hint) != std::abs(p - hint)) return std::abs(d - hint) < std::abs(p - hint);
      return mm[i] > mm[j];
    };
    for (int d = std::max(-Dmax, hint - tol); d <= std::min(Dmax, hint + tol); ++d) {
      const size_t i = size_t(d + Dmax);
      if (state[i] != 1 || cc[i] + ff[i] != 0) continue;
      if (pick == INT_MIN || better(d, pick)) pick = d;
    }
    if (pick != INT_MIN) {
      out.status = StepStatus::Appended;
      out.d = pick;
      out.score = scoreOf(pick);
      out.evidence = evidence(pick);
      out.lowConfidence = true;
      return out;
    }
  }
  out.status = anyConflictFree ? StepStatus::Ambiguous : StepStatus::NoMatch;
  return out;
}

}  // namespace putgd::scroll
