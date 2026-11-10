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
  marginHash.resize(size_t(height));
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
    uint64_t mh = kFnvOffset;
    for (int x = 0; x < colStart; ++x) mh = (mh ^ loadPx(r, x)) * kFnvPrime;
    for (int x = colEnd; x < width; ++x) mh = (mh ^ loadPx(r, x)) * kFnvPrime;
    rowHash[y] = fmix64(rh);
    marginHash[y] = mh;
    flatBandMask[y] = flatMask;
    edgeBandMask[y] = edgeMask;
    flatColor[y] = rowFirst;
    rowFlat[y] = allFlat ? 1 : 0;
  }
}

size_t FrameSignatures::bytes() const {
  return (bandHash.size() + edgeHash.size() + rowHash.size() + marginHash.size()) * 8 +
         (flatBandMask.size() + edgeBandMask.size() + flatColor.size()) * 4 + rowFlat.size();
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
  for (int y = 0; y < a.height; ++y)
    if (a.marginHash[y] != b.marginHash[y]) return true;
  return false;
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

ScrollStitcher::ScrollStitcher(StitchOptions opts, RowSink sink)
    : opts_(opts), sink_(std::move(sink)) {
  opts_.bands = std::clamp(opts_.bands, 1, 32);
  opts_.minOverlapRows = std::max(1, opts_.minOverlapRows);
}

int ScrollStitcher::outputHeight() const {
  if (!hasRef_) return 0;
  if (finished_) return int(bodyRows_);
  if (!moved_) return std::min(height_, opts_.maxOutputRows);
  return int(bodyRows_) + std::max(estBot_, 0);
}

size_t ScrollStitcher::memoryBytes() const {
  return image_.capacity() + pending_.capacity() + footer_.capacity() + ref_.bytes();
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
  const long long reserve = moved_ ? std::max(estBot_, 0) : 0;
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

void ScrollStitcher::finish() {
  if (finished_ || !hasRef_) {
    finished_ = true;
    return;
  }
  if (!moved_) {
    flushPending(height_);
  } else {
    const int rows = std::max(estBot_, 0);
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
  const int H = height_;
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
    ref_ = cur;
    if (moved_) storeFooter(f);
  };
  auto noMovement = [&](double score) {
    refresh();
    r.status = StepStatus::NoMovement;
    r.score = score;
    return r;
  };
  // Identical frames are NoMovement unless the margin (scrollbar) moved and the
  // caller expects movement: then the hint is tested against the pixels.
  if (identical && !(margin && hint > 0)) return noMovement(1.0);

  int top, bot;
  if (identical) {
    top = std::max(estTop_, 0);
    bot = std::max(estBot_, 0);
  } else {
    int t = 0;
    while (t < H && same(t)) ++t;
    int b = 0;
    while (b < H - t && same(H - 1 - b)) ++b;
    // Over-estimates (content that happened to be identical under a header) only
    // shrink over later steps: the estimate is the minimum over moving steps.
    top = estTop_ >= 0 ? std::min(estTop_, t) : t;
    bot = estBot_ >= 0 ? std::min(estBot_, b) : b;
  }
  if (H - top - bot < opts_.minOverlapRows + 1) return noMovement(0.0);  // changed in place

  Search s = search(cur, top, H - bot, hint, unc);
  if (s.status == StepStatus::NoMatch && !identical) {
    // Runs of identical rows at the frame edges may just be blank content that
    // did not change; header/footer bands end with informative rows, so retry
    // the search on a band that drops trailing flat rows of each run. The
    // committed (conservative) estimates stay top/bot.
    int t2 = top, b2 = bot;
    while (t2 > 0 && cur.rowFlat[t2 - 1]) --t2;
    while (b2 > 0 && cur.rowFlat[H - b2]) --b2;
    if ((t2 != top || b2 != bot) && H - t2 - b2 >= opts_.minOverlapRows + 1) {
      Search s2 = search(cur, t2, H - b2, hint, unc);
      s2.evaluated += s.evaluated;
      if (s2.status == StepStatus::Appended ||
          (s.status == StepStatus::NoMatch && s2.status == StepStatus::Ambiguous))
        s = s2;
      else
        s.evaluated = s2.evaluated;
    }
  }
  r.candidatesEvaluated = s.evaluated;
  r.score = s.score;
  r.evidenceRows = s.evidence;
  r.lowConfidence = s.lowConfidence;
  r.offset = s.d;
  if (s.status != StepStatus::Appended) {
    if (identical) return noMovement(1.0);
    r.status = s.status;
    return r;
  }
  const int d = s.d;
  if (d == 0 || (identical && d < 0)) return noMovement(s.score);  // in-place change only

  if (d > 0) {
    const long long curOrigin = refOrigin_ + d;
    const long long body = moved_ ? bodyRows_ : (long long)(H - bot);
    const long long start = body - curOrigin;
    if (start < top) {  // would leave a gap (only possible after a footer estimate shrank)
      r.status = StepStatus::NoMatch;
      return r;
    }
    estTop_ = top;
    estBot_ = bot;
    if (!moved_) {
      moved_ = true;
      flushPending(H - bot);  // first frame minus its (now known) footer
    }
    const long long before = bodyRows_;
    emitRows(f, int(start), H - bot);
    r.appendedRows = int(bodyRows_ - before);
    refOrigin_ = curOrigin;
    ref_ = cur;
    storeFooter(f);
    r.status = limited_ ? StepStatus::LimitReached : StepStatus::Appended;
  } else {
    estTop_ = top;
    estBot_ = bot;
    if (!moved_) {
      moved_ = true;
      flushPending(H - bot);
    }
    refOrigin_ += d;  // later frames append only rows below what is already emitted
    ref_ = cur;
    storeFooter(f);
    r.status = StepStatus::Reversed;
  }
  r.fixedTop = estTop_;
  r.fixedBottom = estBot_;
  return r;
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
      out.status = StepStatus::Appended;
      out.d = bestD;
      out.score = sb;
      out.evidence = evidence(bestD);
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
    if (topSet.size() <= 256) {
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
      if (decided) {
        out.status = StepStatus::Appended;
        out.d = exactD;
        out.score = scoreOf(exactD);
        out.evidence = evidence(exactD);
        return out;
      }
    }
    if (!hasHint) return out;
    int c1 = topSet.front();
    for (int d : topSet)
      if (std::abs(d - hint) < std::abs(c1 - hint) ||
          (std::abs(d - hint) == std::abs(c1 - hint) && scoreOf(d) > scoreOf(c1)))
        c1 = d;
    const int dist = std::abs(c1 - hint);
    const int tol = std::max(opts_.hintTolerancePx, int(std::lround(opts_.hintTolerance * hint)));
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
      if (unique) {
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
