// Scrolling capture's matching engine (#1515). See ScrollTracker.h for what it
// does, and for the MIT notice of wayscrollshot and mark-shot, whose band
// matcher (src/stitch/band, crates/shot-stitch) this file ports:
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
// What was taken, and where it went:
// - profile.rs: brightness per row in column bands from 4% to 96% of the
//   width (Signature, bandOf); here the mean of R+2G+B over every pixel of a
//   band rather than of up to nine sampled columns, and in sixteenths.
// - compare.rs: a row counts only when it is textured (differs from the row
//   above), and an overlap must be long enough (bandScore's evidence rules).
// - adjacent.rs and matching.rs (Candidates::visit): candidates are ordered
//   by their distance from the expected offset, and among the acceptable ones
//   the nearest wins (decide: here only among candidates that are equal, so
//   that lines which differ in a few characters cannot slip by one line).
// - motion.rs: bands that do not move while the rest does do not vote; here
//   per block of the frame (learn, the pinned mask) instead of per band.
// - fixed.rs: what stays at the top and the bottom is kept once; shrinks at
//   once, grows after two observations (pinAfterSteps); here any pinned block.
// - matching.rs find_known, long_image.rs: a frame that does not follow the
//   last one is looked up in the image already built, which grows at both
//   ends (searchStore, Store).
#include "ScrollTracker.h"

#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cmath>
#include <cstring>
#include <numeric>
#include <tuple>
#include <map>
#include <unordered_map>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace putgd::scroll {

const char* toString(TrackStatus s) {
  switch (s) {
    case TrackStatus::Placed: return "placed";
    case TrackStatus::NoMovement: return "no_movement";
    case TrackStatus::Skipped: return "skipped";
    case TrackStatus::CapReached: return "cap_reached";
    case TrackStatus::Invalid: return "invalid";
  }
  return "?";
}

const char* toString(SkipReason r) {
  switch (r) {
    case SkipReason::None: return "none";
    case SkipReason::NoEvidence: return "no_evidence";
    case SkipReason::NoMatch: return "no_match";
    case SkipReason::Ambiguous: return "ambiguous";
    case SkipReason::Contradicted: return "contradicted";
  }
  return "?";
}

const char* toString(TrackState s) {
  switch (s) {
    case TrackState::Waiting: return "waiting";
    case TrackState::Following: return "following";
    case TrackState::Lost: return "lost";
    case TrackState::Capped: return "capped";
  }
  return "?";
}

// ------------------------------------------------------------------ canvas

uint8_t* MemoryCanvas::rowPtr(long long row, bool create) {
  const long long chunk = row >= 0 ? row / kChunkRows : -((-row + kChunkRows - 1) / kChunkRows);
  if (!cached_ || chunk != cachedChunk_) {
    auto it = chunks_.find(chunk);
    if (it == chunks_.end()) {
      if (!create) return nullptr;
      it = chunks_.emplace(chunk, std::vector<uint8_t>(size_t(kChunkRows) * width_ * 4, 0)).first;
    }
    cached_ = it->second.data();
    cachedChunk_ = chunk;
  }
  return cached_ + size_t(row - chunk * kChunkRows) * width_ * 4;
}

void MemoryCanvas::writeSpan(long long row, int x0, int x1, const uint8_t* bgrx) {
  x0 = std::max(0, x0);
  x1 = std::min(width_, x1);
  if (x1 <= x0) return;
  std::memcpy(rowPtr(row, true) + size_t(x0) * 4, bgrx, size_t(x1 - x0) * 4);
  if (rows_ == 0) {
    top_ = row;
    rows_ = 1;
  } else if (row < top_) {
    rows_ += top_ - row;
    top_ = row;
  } else if (row >= top_ + rows_) {
    rows_ = row + 1 - top_;
  }
}

bool MemoryCanvas::readRow(long long row, uint8_t* bgrx) const {
  if (rows_ == 0 || row < top_ || row >= top_ + rows_) return false;
  const uint8_t* src = const_cast<MemoryCanvas*>(this)->rowPtr(row, false);
  if (src) std::memcpy(bgrx, src, size_t(width_) * 4);
  else std::memset(bgrx, 0, size_t(width_) * 4);
  return true;
}

std::vector<uint8_t> MemoryCanvas::image(long long from, long long to) const {
  std::vector<uint8_t> out;
  if (to <= from) return out;
  out.assign(size_t(to - from) * width_ * 4, 0);
  for (long long r = from; r < to; ++r) readRow(r, &out[size_t(r - from) * width_ * 4]);
  return out;
}

// ------------------------------------------------------------------ signatures

// Per frame: the mean of R+2G+B over each cell (cellPx columns of one row) and
// each voting band, in sixteenths (0..16320), and whether it is textured.
struct ScrollTracker::Signature {
  int h = 0, cells = 0, bands = 0;
  std::vector<uint16_t> cell, cellTex;  // h * cells
  std::vector<uint16_t> band, bandTex;  // h * bands
  std::vector<int> bandLo;              // bands + 1 cell boundaries
  const uint16_t* cellRow(int y) const { return &cell[size_t(y) * cells]; }
  const uint16_t* cellTexRow(int y) const { return &cellTex[size_t(y) * cells]; }
  const uint16_t* bandRow(int y) const { return &band[size_t(y) * bands]; }
  const uint16_t* bandTexRow(int y) const { return &bandTex[size_t(y) * bands]; }
};

namespace {

inline unsigned absDiff(unsigned a, unsigned b) { return a > b ? a - b : b - a; }
// Cells with texture a row needs to be taken for a line along the frame's edge (#1796).
constexpr int kEdgeLineCells = 16;

// Rows of a tile: what may disagree as one piece (a sticky header strip that
// moved by its own distance, a box, a side bar) without the page around it
// failing the acceptance.
constexpr int kTileRows = 32;

uint32_t px(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v & 0x00FFFFFFu;
}

// The loops over a row's lanes, eight at a time with SSE2 and one at a time
// without. Lanes are 16 bits; masks are 0 or 1.
#if defined(__SSE2__)
inline __m128i ld(const uint16_t* p) { return _mm_loadu_si128(reinterpret_cast<const __m128i*>(p)); }
inline void st(uint16_t* p, __m128i v) { _mm_storeu_si128(reinterpret_cast<__m128i*>(p), v); }
inline __m128i absd(__m128i a, __m128i b) { return _mm_or_si128(_mm_subs_epu16(a, b), _mm_subs_epu16(b, a)); }
// 0xFFFF where a <= t
inline __m128i le(__m128i a, __m128i t) { return _mm_cmpeq_epi16(_mm_subs_epu16(a, t), _mm_setzero_si128()); }
inline unsigned hsum(__m128i v) {
  alignas(16) uint16_t x[8];
  _mm_store_si128(reinterpret_cast<__m128i*>(x), v);
  return unsigned(x[0]) + x[1] + x[2] + x[3] + x[4] + x[5] + x[6] + x[7];
}
#endif

// out = 1 where the cell differs from the row above or below by thr or more.
void textureMask(const uint16_t* r, const uint16_t* up, const uint16_t* dn, int n, unsigned thr, uint16_t* out) {
  int k = 0;
#if defined(__SSE2__)
  const __m128i t = _mm_set1_epi16(short(thr - 1)), one = _mm_set1_epi16(1);
  for (; k + 8 <= n; k += 8) {
    const __m128i v = ld(r + k);
    const __m128i flat = _mm_and_si128(le(absd(v, ld(up + k)), t), le(absd(v, ld(dn + k)), t));
    st(out + k, _mm_andnot_si128(flat, one));
  }
#endif
  for (; k < n; ++k) out[k] = uint16_t(absDiff(r[k], up[k]) >= thr || absDiff(r[k], dn[k]) >= thr);
}

// out = 1 where the lane votes: not pinned, and not where it was.
void inclusion(const uint16_t* a, const uint16_t* b, const uint16_t* pin, int n, unsigned tol, uint16_t* out) {
  int k = 0;
#if defined(__SSE2__)
  const __m128i t = _mm_set1_epi16(short(tol)), one = _mm_set1_epi16(1);
  for (; k + 8 <= n; k += 8) {
    const __m128i still = _mm_and_si128(le(absd(ld(a + k), ld(b + k)), t), one);
    st(out + k, _mm_andnot_si128(_mm_or_si128(still, ld(pin + k)), one));
  }
#endif
  for (; k < n; ++k) out[k] = uint16_t(!pin[k] && absDiff(a[k], b[k]) > tol);
}

// accA += agreeing lanes, accI += informative lanes (textured on either side,
// voting on both); returns the agreeing lanes.
unsigned accumulate(const uint16_t* a, const uint16_t* b, const uint16_t* ta, const uint16_t* tb, const uint16_t* ia,
                    const uint16_t* ib, int n, unsigned tol, uint16_t* accA, uint16_t* accI) {
  unsigned total = 0;
  int k = 0;
#if defined(__SSE2__)
  const __m128i t = _mm_set1_epi16(short(tol));
  __m128i sum = _mm_setzero_si128();
  for (; k + 8 <= n; k += 8) {
    const __m128i use = _mm_and_si128(_mm_and_si128(_mm_or_si128(ld(ta + k), ld(tb + k)), ld(ia + k)), ld(ib + k));
    const __m128i ok = _mm_and_si128(use, le(absd(ld(a + k), ld(b + k)), t));
    st(accA + k, _mm_add_epi16(ld(accA + k), ok));
    st(accI + k, _mm_add_epi16(ld(accI + k), use));
    sum = _mm_add_epi16(sum, ok);
  }
  total = hsum(sum);
#endif
  for (; k < n; ++k) {
    const unsigned use = unsigned(ta[k] | tb[k]) & ia[k] & ib[k];
    const unsigned ok = use & unsigned(absDiff(a[k], b[k]) <= tol);
    accA[k] = uint16_t(accA[k] + ok);
    accI[k] = uint16_t(accI[k] + use);
    total += ok;
  }
  return total;
}

// Lanes where x is set and y is not.
unsigned countAndNot(const uint16_t* x, const uint16_t* y, int n) {
  unsigned total = 0;
  int k = 0;
#if defined(__SSE2__)
  __m128i sum = _mm_setzero_si128();
  for (; k + 8 <= n; k += 8) sum = _mm_add_epi16(sum, _mm_andnot_si128(ld(y + k), ld(x + k)));
  total = hsum(sum);
#endif
  for (; k < n; ++k) total += unsigned(x[k]) & unsigned(!y[k]);
  return total;
}

// The pinned mask's votes for one row: stay where the textured lane is
// strictly where it was (a pinned part is drawn again the same; a photo's
// smooth cells only look alike) and not where the page moved it; move where
// it is where the page moved it and not where it was. bd: ref's row y + d,
// ad: cur's row y - d (null outside the frame); xd, xa: 1 where those rows
// are under the excluded rectangle, where neither test can tell anything.
void votes(const uint16_t* a, const uint16_t* b, const uint16_t* ta, const uint16_t* tb, const uint16_t* bd,
           const uint16_t* ad, const uint16_t* xd, const uint16_t* xa, int n, unsigned tol, unsigned strict, unsigned page,
           uint16_t* stay, uint16_t* stayFlat, uint16_t* move, uint16_t* astray) {
  int k = 0;
#if defined(__SSE2__)
  const __m128i t = _mm_set1_epi16(short(tol)), ts = _mm_set1_epi16(short(strict)), one = _mm_set1_epi16(1),
                zero = _mm_setzero_si128();
  for (; k + 8 <= n; k += 8) {
    const __m128i va = ld(a + k), vb = ld(b + k);
    const __m128i diff = absd(va, vb);
    const __m128i same = le(diff, t), exact = le(diff, ts);
    const __m128i okD = bd ? _mm_cmpeq_epi16(ld(xd + k), zero) : zero, okA = ad ? _mm_cmpeq_epi16(ld(xa + k), zero) : zero;
    const __m128i here = bd ? _mm_and_si128(okD, le(absd(va, ld(bd + k)), t)) : zero;
    const __m128i there = ad ? _mm_and_si128(okA, le(absd(vb, ld(ad + k)), t)) : zero;
    const __m128i tex = _mm_cmpeq_epi16(_mm_or_si128(ld(ta + k), ld(tb + k)), one);
    // A flat cell votes too, apart, when it is not the page's own background
    // (the flat inside of a cookie banner, a side bar's colour): blank rows
    // of the page that line up by chance must not, and two grey code
    // blocks that do count toward pinning only, not toward this pair.
    const __m128i coloured = _mm_andnot_si128(le(absd(va, _mm_set1_epi16(short(page))), t), _mm_cmpeq_epi16(zero, zero));
    const __m128i away = _mm_and_si128(_mm_andnot_si128(_mm_or_si128(here, there), exact), _mm_or_si128(okD, okA));
    // Stays only where both frames show something: a blank cell beside a
    // line of text over a blank part of the frame before stood nowhere.
    const __m128i both = _mm_cmpeq_epi16(_mm_and_si128(ld(ta + k), ld(tb + k)), one);
    const __m128i s = _mm_and_si128(away, both);
    const __m128i sf = _mm_andnot_si128(tex, _mm_and_si128(away, coloured));
    const __m128i m = _mm_andnot_si128(same, _mm_and_si128(here, _mm_cmpeq_epi16(ld(ta + k), one)));
    // Neither where it was nor where the page moved it, where that can be
    // told: something that changes at its own place.
    const __m128i x = bd ? _mm_and_si128(okD, _mm_andnot_si128(_mm_or_si128(_mm_or_si128(here, there), same), tex)) : zero;
    st(stay + k, _mm_sub_epi16(ld(stay + k), s));
    st(stayFlat + k, _mm_sub_epi16(ld(stayFlat + k), sf));
    st(move + k, _mm_sub_epi16(ld(move + k), m));
    st(astray + k, _mm_sub_epi16(ld(astray + k), x));
  }
#endif
  for (; k < n; ++k) {
    const unsigned diff = absDiff(a[k], b[k]);
    const bool tex = ta[k] | tb[k];
    const bool coloured = absDiff(a[k], page) > tol;
    const bool okD = bd && !xd[k], okA = ad && !xa[k];
    const bool here = okD && absDiff(a[k], bd[k]) <= tol;
    const bool there = okA && absDiff(b[k], ad[k]) <= tol;
    const bool away = diff <= strict && !here && !there && (okD || okA);
    if (away && (ta[k] & tb[k])) ++stay[k];
    else if (away && !tex && coloured) ++stayFlat[k];
    else if (diff > tol && here && ta[k]) ++move[k];
    else if (okD && tex && diff > tol && !here && !there) ++astray[k];
  }
}

// Cells that show something outside what is pinned (open), and those of
// them strictly where they were and showing something there too (a blank
// cell beside a line of text counts as showing something; over a blank part
// of the frame before it, it is not a sign that anything stood still).
void strictStill(const uint16_t* a, const uint16_t* b, const uint16_t* tex, const uint16_t* texB, const uint16_t* pin, int n,
                 unsigned strict, uint16_t* open, uint16_t* still, uint16_t* openSum, uint16_t* stillSum) {
  int k = 0;
#if defined(__SSE2__)
  const __m128i t = _mm_set1_epi16(short(strict)), one = _mm_set1_epi16(1);
  for (; k + 8 <= n; k += 8) {
    const __m128i o = _mm_andnot_si128(ld(pin + k), ld(tex + k));
    const __m128i s = _mm_and_si128(_mm_and_si128(o, ld(texB + k)), _mm_and_si128(le(absd(ld(a + k), ld(b + k)), t), one));
    st(open + k, o);
    st(still + k, s);
    st(openSum + k, _mm_add_epi16(ld(openSum + k), o));
    st(stillSum + k, _mm_add_epi16(ld(stillSum + k), s));
  }
#endif
  for (; k < n; ++k) {
    open[k] = uint16_t(tex[k] & !pin[k]);
    still[k] = uint16_t(open[k] & texB[k] & (absDiff(a[k], b[k]) <= strict));
    openSum[k] = uint16_t(openSum[k] + open[k]);
    stillSum[k] = uint16_t(stillSum[k] + still[k]);
  }
}

// Of the lanes not pinned: differing ones, textured ones, and textured ones
// that are where they were.
void stillCounts(const uint16_t* a, const uint16_t* b, const uint16_t* ta, const uint16_t* tb, const uint16_t* pin,
                 int n, unsigned tol, unsigned& differ, unsigned& inf, unsigned& agree) {
  int k = 0;
#if defined(__SSE2__)
  const __m128i t = _mm_set1_epi16(short(tol)), one = _mm_set1_epi16(1);
  __m128i sd = _mm_setzero_si128(), si = sd, sa = sd;
  for (; k + 8 <= n; k += 8) {
    const __m128i open = _mm_andnot_si128(ld(pin + k), one);
    const __m128i same = _mm_and_si128(le(absd(ld(a + k), ld(b + k)), t), one);
    const __m128i tex = _mm_and_si128(_mm_or_si128(ld(ta + k), ld(tb + k)), open);
    sd = _mm_add_epi16(sd, _mm_andnot_si128(same, open));
    si = _mm_add_epi16(si, tex);
    sa = _mm_add_epi16(sa, _mm_and_si128(tex, same));
  }
  differ += hsum(sd);
  inf += hsum(si);
  agree += hsum(sa);
#endif
  for (; k < n; ++k) {
    if (pin[k]) continue;
    const bool same = absDiff(a[k], b[k]) <= tol;
    differ += !same;
    if (!(ta[k] | tb[k])) continue;
    ++inf;
    agree += same;
  }
}

#if defined(__SSE2__)
// R+2G+B over each 8-pixel cell of a row, times two (sixteenths of the mean).
void cellSums8(const uint8_t* p, int cells, uint16_t* out) {
  const __m128i bgr = _mm_set1_epi32(0x00FFFFFF), green = _mm_set1_epi32(0x0000FF00), zero = _mm_setzero_si128();
  int c = 0;
  for (; c + 2 <= cells; c += 2) {
    const uint8_t* q = p + size_t(c) * 32;
    const __m128i v0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(q));
    const __m128i v1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(q + 16));
    const __m128i v2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(q + 32));
    const __m128i v3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(q + 48));
    // Each SAD gives two 64-bit sums; green is added once more.
    const __m128i s0 = _mm_add_epi64(_mm_add_epi64(_mm_sad_epu8(_mm_and_si128(v0, bgr), zero), _mm_sad_epu8(_mm_and_si128(v1, bgr), zero)),
                                     _mm_add_epi64(_mm_sad_epu8(_mm_and_si128(v0, green), zero), _mm_sad_epu8(_mm_and_si128(v1, green), zero)));
    const __m128i s1 = _mm_add_epi64(_mm_add_epi64(_mm_sad_epu8(_mm_and_si128(v2, bgr), zero), _mm_sad_epu8(_mm_and_si128(v3, bgr), zero)),
                                     _mm_add_epi64(_mm_sad_epu8(_mm_and_si128(v2, green), zero), _mm_sad_epu8(_mm_and_si128(v3, green), zero)));
    // [s0.lo + s0.hi, s1.lo + s1.hi] in the two 64-bit lanes.
    const __m128i both = _mm_add_epi64(_mm_unpacklo_epi64(s0, s1), _mm_unpackhi_epi64(s0, s1));
    out[c] = uint16_t(unsigned(_mm_cvtsi128_si32(both)) * 2u);
    out[c + 1] = uint16_t(unsigned(_mm_cvtsi128_si32(_mm_srli_si128(both, 8))) * 2u);
  }
  for (; c < cells; ++c) {
    const __m128i v0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + size_t(c) * 32));
    const __m128i v1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + size_t(c) * 32 + 16));
    __m128i sum = _mm_add_epi64(_mm_sad_epu8(_mm_and_si128(v0, bgr), zero), _mm_sad_epu8(_mm_and_si128(v1, bgr), zero));
    sum = _mm_add_epi64(sum, _mm_add_epi64(_mm_sad_epu8(_mm_and_si128(v0, green), zero),
                                           _mm_sad_epu8(_mm_and_si128(v1, green), zero)));
    const unsigned total = unsigned(_mm_cvtsi128_si32(sum)) + unsigned(_mm_cvtsi128_si32(_mm_srli_si128(sum, 8)));
    out[c] = uint16_t(total * 2u);
  }
}
#endif


void computeSignature(const FrameView& f, int cellPx, const std::vector<int>& bandLo, int cellTexture,
                      int bandTexture, ScrollTracker::Signature& s) {
  const int w = f.width, h = f.height, cells = (w + cellPx - 1) / cellPx, bands = int(bandLo.size()) - 1;
  s.h = h;
  s.cells = cells;
  s.bands = bands;
  s.bandLo = bandLo;
  s.cell.resize(size_t(h) * cells);
  s.cellTex.resize(size_t(h) * cells);
  s.band.resize(size_t(h) * bands);
  s.bandTex.resize(size_t(h) * bands);

  const int full = w / cellPx;
  // The mean of a band's cells by a multiplication (2^32 / cells, rounded up).
  std::vector<uint64_t> recip(static_cast<size_t>(bands));
  for (int b = 0; b < bands; ++b) {
    const uint64_t n = uint64_t(bandLo[b + 1] - bandLo[b]);
    recip[size_t(b)] = ((uint64_t(1) << 32) + n - 1) / n;
  }
  for (int y = 0; y < h; ++y) {
    const uint8_t* p = f.row(y);
    uint16_t* out = &s.cell[size_t(y) * cells];
    // R+2G+B over a cell from 32-bit loads: blue and red add up in two
    // 16-bit halves at once, green beside them.
    int c0 = 0;
#if defined(__SSE2__)
    if (cellPx == 8) {
      cellSums8(p, full, out);
      c0 = full;
    }
#endif
    for (int c = c0; c < full; ++c) {
      const uint8_t* q = p + size_t(c) * cellPx * 4;
      uint32_t br = 0, g = 0;
      for (int x = 0; x < cellPx; ++x) {
        uint32_t v;
        std::memcpy(&v, q + 4 * x, 4);
        br += v & 0x00FF00FFu;
        g += (v >> 8) & 0xFFu;
      }
      out[c] = uint16_t(((br & 0xFFFFu) + (br >> 16) + 2u * g) * 16u / unsigned(cellPx));
    }
    if (full < cells) {
      unsigned sum = 0;
      const int n = w - full * cellPx;
      for (int x = full * cellPx; x < w; ++x) sum += unsigned(p[4 * x]) + 2u * p[4 * x + 1] + p[4 * x + 2];
      out[full] = uint16_t(sum * 16u / unsigned(n));
    }
    uint16_t* bo = &s.band[size_t(y) * bands];
    for (int b = 0; b < bands; ++b) {
      unsigned sum = 0;
      for (int c = bandLo[b]; c < bandLo[b + 1]; ++c) sum += out[c];
      bo[b] = uint16_t((uint64_t(sum) * recip[size_t(b)]) >> 32);
    }
  }
  for (int y = 0; y < h; ++y) {
    const uint16_t* r = s.cellRow(y);
    const uint16_t* up = y > 0 ? s.cellRow(y - 1) : r;
    const uint16_t* dn = y + 1 < h ? s.cellRow(y + 1) : r;
    textureMask(r, up, dn, cells, unsigned(cellTexture), &s.cellTex[size_t(y) * cells]);
    const uint16_t* br = s.bandRow(y);
    const uint16_t* bu = y > 0 ? s.bandRow(y - 1) : br;
    const uint16_t* bd = y + 1 < h ? s.bandRow(y + 1) : br;
    textureMask(br, bu, bd, bands, unsigned(bandTexture), &s.bandTex[size_t(y) * bands]);

  }
}

// One pair of rows: agreeing and informative lanes (all lanes 16 bits; the
// texture and inclusion lanes are 0 or 1). Eight lanes at a time with SSE2.
inline void lanes(const uint16_t* a, const uint16_t* b, const uint16_t* ta, const uint16_t* tb,
                  const uint16_t* ia, const uint16_t* ib, int n, unsigned tol, unsigned& agree, unsigned& inf) {
  unsigned A = 0, I = 0;
  int k = 0;
#if defined(__SSE2__)
  {
    const __m128i vtol = _mm_set1_epi16(short(std::min(tol, 0x7FFFu)));
    const __m128i zero = _mm_setzero_si128();
    __m128i accA = zero, accI = zero;
    for (; k + 8 <= n; k += 8) {
      const __m128i va = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + k));
      const __m128i vb = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + k));
      const __m128i d = _mm_or_si128(_mm_subs_epu16(va, vb), _mm_subs_epu16(vb, va));
      const __m128i ok = _mm_cmpeq_epi16(_mm_subs_epu16(d, vtol), zero);
      const __m128i tex = _mm_or_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(ta + k)),
                                       _mm_loadu_si128(reinterpret_cast<const __m128i*>(tb + k)));
      const __m128i use = _mm_and_si128(_mm_and_si128(tex, _mm_loadu_si128(reinterpret_cast<const __m128i*>(ia + k))),
                                        _mm_loadu_si128(reinterpret_cast<const __m128i*>(ib + k)));
      accI = _mm_add_epi16(accI, use);
      accA = _mm_add_epi16(accA, _mm_and_si128(use, ok));
    }
    alignas(16) uint16_t va[8], vi[8];
    _mm_store_si128(reinterpret_cast<__m128i*>(va), accA);
    _mm_store_si128(reinterpret_cast<__m128i*>(vi), accI);
    for (int j = 0; j < 8; ++j) {
      A += va[j];
      I += vi[j];
    }
  }
#endif
  for (; k < n; ++k) {
    const unsigned d = absDiff(a[k], b[k]);
    const unsigned use = unsigned(ta[k] | tb[k]) & ia[k] & ib[k];
    I += use;
    A += use & unsigned(d <= tol);
  }
  agree += A;
  inf += I;
}

}  // namespace

// ------------------------------------------------------------------ the page store

// Per page row: which cells are written (provisional: seen once; verified:
// two frames agreed), and the row's band signature where it is known.
struct ScrollTracker::Store {
  int cells = 0, bands = 0, words = 0;
  long long origin = 0, count = 0;  // rows [origin, origin + count)
  std::vector<uint64_t> prov, ver;  // count * words
  // count * cells: plain cells, by vote (see flushPending): the colour most
  // placed frames showed (R+2G+B over 4, in its 256 steps) and its lead
  // (up to 127, and the top bit where it was seen in the page's area).
  std::vector<uint8_t> plainVal, plainVotes;
  std::vector<uint64_t> hid;         // count * words: under the excluded rectangle in some placed frame
  std::vector<uint64_t> vis;         // count * words: outside it in some placed frame
  std::vector<uint16_t> band, valid, tex;  // count * bands
  // Page bands observed changing in place while the rest of the frame agreed
  // (#1537: a playing video, an animated banner): dynHits counts changes, dyn
  // marks the band dynamic once it has changed repeatedly (its valid bit is
  // cleared so store comparisons ignore it, while canvas keeps its pixels).
  std::vector<uint8_t> dynHits, dyn;       // count * bands
  std::vector<uint8_t> shown;              // count: a placed frame covered the row

  void init(int c, int b) {
    cells = c;
    bands = b;
    words = (c + 63) / 64;
  }
  bool has(long long r) const { return r >= origin && r < origin + count; }
  void ensure(long long lo, long long hi) {  // rows [lo, hi)
    if (count > 0 && lo >= origin && hi <= origin + count) return;
    long long nlo = count > 0 ? std::min(lo, origin) : lo, nhi = count > 0 ? std::max(hi, origin + count) : hi;
    const long long slack = std::max<long long>(512, (nhi - nlo) / 2);
    if (count > 0 && nlo < origin) nlo -= slack;
    if (count == 0 || nhi > origin + count) nhi += slack;
    const long long n = nhi - nlo;
    std::vector<uint64_t> p(size_t(n) * words, 0), v(size_t(n) * words, 0), h(size_t(n) * words, 0),
        o(size_t(n) * words, 0);
    std::vector<uint8_t> pv(size_t(n) * cells, 0), pc(size_t(n) * cells, 0);
    std::vector<uint16_t> bd(size_t(n) * bands, 0), vd(size_t(n) * bands, 0), td(size_t(n) * bands, 0);
    std::vector<uint8_t> dh(size_t(n) * bands, 0), dy(size_t(n) * bands, 0);
    std::vector<uint8_t> sh(size_t(n), 0);

    if (count > 0) {
      const size_t at = size_t(origin - nlo);
      std::memcpy(&p[at * words], prov.data(), prov.size() * 8);
      std::memcpy(&v[at * words], ver.data(), ver.size() * 8);
      std::memcpy(&pv[at * cells], plainVal.data(), plainVal.size());
      std::memcpy(&pc[at * cells], plainVotes.data(), plainVotes.size());
      std::memcpy(&h[at * words], hid.data(), hid.size() * 8);
      std::memcpy(&o[at * words], vis.data(), vis.size() * 8);
      std::memcpy(&bd[at * bands], band.data(), band.size() * 2);
      std::memcpy(&vd[at * bands], valid.data(), valid.size() * 2);
      std::memcpy(&td[at * bands], tex.data(), tex.size() * 2);
      std::memcpy(&dh[at * bands], dynHits.data(), dynHits.size());
      std::memcpy(&dy[at * bands], dyn.data(), dyn.size());
      std::memcpy(&sh[at], shown.data(), shown.size());
    }
    prov.swap(p);
    ver.swap(v);
    plainVal.swap(pv);
    plainVotes.swap(pc);
    hid.swap(h);
    vis.swap(o);
    band.swap(bd);
    valid.swap(vd);
    tex.swap(td);
    dynHits.swap(dh);
    dyn.swap(dy);
    shown.swap(sh);
    origin = nlo;
    count = n;
  }
  // A plain cell's colour by vote, where no frame wrote more (see flushPending).
  bool plain(long long r, int c) const { return has(r) && plainVotes[size_t(r - origin) * cells + size_t(c)] != 0; }
  // The lead of the colour voted at a plain cell (0 where none).
  int lead(long long r, int c) const { return has(r) ? plainVotes[size_t(r - origin) * cells + size_t(c)] & 0x7F : 0; }
  int level(long long r, int c) const {
    if (!has(r)) return 0;
    const size_t i = size_t(r - origin) * words + size_t(c >> 6);
    const uint64_t bit = 1ull << (c & 63);
    return (ver[i] & bit) ? 2 : (prov[i] & bit) ? 1 : 0;
  }
  void set(long long r, int c, int lvl) {
    const size_t i = size_t(r - origin) * words + size_t(c >> 6);
    const uint64_t bit = 1ull << (c & 63);
    if (lvl >= 1) prov[i] |= bit;
    if (lvl >= 2) ver[i] |= bit;
  }
  // Under the excluded rectangle in a placed frame and never outside it:
  // only the rectangle kept it from being seen.
  bool hidden(long long r, int c) const {
    if (!has(r)) return false;
    const size_t i = size_t(r - origin) * words + size_t(c >> 6);
    return ((hid[i] & ~vis[i]) >> (c & 63)) & 1;
  }
  uint16_t* bandRow(long long r) { return &band[size_t(r - origin) * bands]; }
  uint16_t* validRow(long long r) { return &valid[size_t(r - origin) * bands]; }
  const uint16_t* bandRow(long long r) const { return &band[size_t(r - origin) * bands]; }
  const uint16_t* validRow(long long r) const { return &valid[size_t(r - origin) * bands]; }
  uint16_t* texRow(long long r) { return &tex[size_t(r - origin) * bands]; }
  const uint16_t* texRow(long long r) const { return &tex[size_t(r - origin) * bands]; }
  uint8_t* dynHitsRow(long long r) { return &dynHits[size_t(r - origin) * bands]; }
  const uint8_t* dynRow(long long r) const { return &dyn[size_t(r - origin) * bands]; }
  bool isDyn(long long r, int k) const { return has(r) && dyn[size_t(r - origin) * bands + size_t(k)] != 0; }
  bool markDyn(long long r, int k) {
    if (!has(r)) return false;
    const size_t i = size_t(r - origin) * bands + size_t(k);
    const bool added = dyn[i] == 0;
    dyn[i] = 1;
    valid[i] = 0;
    return added;
  }
};

// ------------------------------------------------------------------ tracker

ScrollTracker::ScrollTracker(TrackerCanvas& canvas, TrackerOptions options)
    : canvas_(canvas), opts_(options), last_(std::make_unique<Signature>()), work_(std::make_unique<Signature>()),
      prev_(std::make_unique<Signature>()), store_(std::make_unique<Store>()) {
  opts_.cellPx = std::clamp(opts_.cellPx, 1, 64);
  opts_.blockRows = std::clamp(opts_.blockRows, 1, 64);
  opts_.bands = std::clamp(opts_.bands, 1, 64);
}

const ScrollTracker::Signature* ScrollTracker::prevSig() const {
  if (!prevHasPos_) return nullptr;
  return prevIsLast_ ? last_.get() : prev_.get();
}

ScrollTracker::~ScrollTracker() = default;

std::vector<uint8_t> ScrollTracker::pinnedMask(int& blockCols, int& blockRowsOut) const {
  blockCols = cells_;
  blockRowsOut = blockRowCount_;
  return closed_;
}

// The incoming frame's lanes that do not vote, at band and at cell level:
// pinned blocks, and whatever stood still since the reference frame (a
// pinned part not yet learned, the flat inside of a header lying over text
// the page moved under it, or a coincidence). The same for the reference
// frame's rows, since the mask is in the frame's coordinates. Called only
// for a frame that moved: a frame that did not is all still.
void ScrollTracker::computeExcluded(const Signature& cur, const Signature& ref, long long refPos) {
  const int B = cur.bands, C = cur.cells, H = cur.h;
  bandIncl_.resize(size_t(H) * B);
  cellIncl_.resize(size_t(H) * C);
  texturedRows_.assign(size_t(H) + 1, 0);
  std::vector<uint16_t> openRow(static_cast<size_t>(C)), stillRow(static_cast<size_t>(C)),
      openSum(static_cast<size_t>(C), 0), stillSum(static_cast<size_t>(C), 0);
  texTile_.assign(size_t((H + kTileRows - 1) / kTileRows) * B, 0);
  stillTile_.assign(texTile_.size(), 0);
  texCellTile_.assign(texTile_.size(), 0);
  for (int y = 0; y < H; ++y) {
    const int brow = std::min(y / opts_.blockRows, blockRowCount_ - 1);
    const uint16_t* tex = cur.bandTexRow(y);
    const uint16_t* pin = &bandPinned16_[size_t(brow) * B];
    const long long pRef = refPos + y;
    const uint8_t* sd = store_->has(pRef) ? store_->dynRow(pRef) : nullptr;
    const unsigned textured = countAndNot(tex, pin, B);
    texturedRows_[size_t(y) + 1] = texturedRows_[size_t(y)] + (textured >= 2);
    uint16_t* tt = &texTile_[size_t(y / kTileRows) * B];
    for (int k = 0; k < B; ++k) tt[k] = uint16_t(tt[k] + (tex[k] & !pin[k] & !(sd && sd[k])));
    // Cells that stood still, strictly (what is pinned is drawn again the
    // same; lines of text that only look alike are not): per tile and band.
    // Per cell over the tile's rows, then per band once a tile row ends.
    strictStill(cur.cellRow(y), ref.cellRow(y), cur.cellTexRow(y), ref.cellTexRow(y), &closed16_[size_t(brow) * C], C,
                unsigned(opts_.cellTolerance) / 8, openRow.data(), stillRow.data(), openSum.data(), stillSum.data());
    if (sd)
      for (int k = 0; k < B; ++k)
        if (sd[k])
          for (int c = cur.bandLo[size_t(k)]; c < cur.bandLo[size_t(k) + 1]; ++c) {
            openSum[size_t(c)] = uint16_t(openSum[size_t(c)] - openRow[size_t(c)]);
            stillSum[size_t(c)] = uint16_t(stillSum[size_t(c)] - stillRow[size_t(c)]);
          }
    if ((y + 1) % kTileRows == 0 || y + 1 == H) {
      uint16_t* ts = &stillTile_[size_t(y / kTileRows) * B];
      uint16_t* tc = &texCellTile_[size_t(y / kTileRows) * B];
      for (int k = 0; k < B; ++k) {
        unsigned n = 0, t = 0;
        for (int c = cur.bandLo[k]; c < cur.bandLo[k + 1]; ++c) {
          t += openSum[size_t(c)];
          n += stillSum[size_t(c)];
        }
        ts[k] = uint16_t(n);
        tc[k] = uint16_t(t);
      }
      std::fill(openSum.begin(), openSum.end(), 0);
      std::fill(stillSum.begin(), stillSum.end(), 0);
    }
    inclusion(cur.cellRow(y), ref.cellRow(y), &closed16_[size_t(brow) * C], C, unsigned(opts_.cellTolerance),
              &cellIncl_[size_t(y) * C]);
    inclusion(cur.bandRow(y), ref.bandRow(y), &bandPinned16_[size_t(brow) * B], B, unsigned(bandTol_),
              &bandIncl_[size_t(y) * B]);
  }
  // The coarse look's rows: rows of this frame that show something, spread
  // over its height (every k-th row would alias with the lines of a text).
  std::vector<int> rich;
  for (int y = 0; y < H; ++y)
    if (texturedRows_[size_t(y) + 1] > texturedRows_[size_t(y)]) rich.push_back(y);
  anchors_.clear();
  const size_t n = rich.size();
  if (n > 0) {
    const size_t step = std::max<size_t>(1, size_t(double(n) * 0.6180339887));
    size_t at = 0;
    std::vector<uint8_t> used(n, 0);
    for (size_t i = 0; i < n && anchors_.size() < 64; ++i) {
      while (used[at]) at = (at + 1) % n;
      used[at] = 1;
      anchors_.push_back(rich[at]);
      at = (at + step) % n;
    }
  }
  refreshRefIncl(refPos);
}

void ScrollTracker::refreshRefIncl(long long refPos) {
  const int H = height_, B = int(bandLo_.size()) - 1, C = cells_;
  refBandIncl_ = bandIncl_;
  refCellIncl_ = cellIncl_;
  for (int y = 0; y < H; ++y) {
    const long long p = refPos + y;
    if (!store_->has(p)) continue;
    const uint8_t* sd = store_->dynRow(p);
    uint16_t* rb = &refBandIncl_[size_t(y) * B];
    uint16_t* rc = &refCellIncl_[size_t(y) * C];
    for (int k = 0; k < B; ++k) {
      if (!sd[k]) continue;
      rb[k] = 0;
      for (int c = bandLo_[size_t(k)]; c < bandLo_[size_t(k) + 1]; ++c) rc[c] = 0;
    }
  }
}


void ScrollTracker::Tally::clear(int bandCount, int height) {
  bands = bandCount;
  const size_t tiles = size_t((height + kTileRows - 1) / kTileRows) * size_t(bandCount);
  agree.assign(tiles, 0);
  inf.assign(tiles, 0);
  agreeSum = infSum = 0;
  rows = texturedRows = 0;
}

void ScrollTracker::Tally::total() {
  agreeSum = infSum = 0;
  for (size_t i = 0; i < agree.size(); ++i) {
    agreeSum += agree[i];
    infSum += inf[i];
  }
}

// Bands of cur row y against ref row y + d (cur row y shows what ref row
// y + d showed), over every row of the overlap, per tile.
void ScrollTracker::bandTally(const Signature& cur, const Signature& ref, int d, Tally& t) const {
  const int H = cur.h, B = cur.bands;
  const int y0 = std::max(0, -d), y1 = std::min(H, H - d);
  t.clear(B, H);
  const unsigned tol = unsigned(bandTol_);
  for (int y = y0; y < y1; ++y) {
    const size_t tile = size_t(y / kTileRows) * B;
    const unsigned agreeing = accumulate(cur.bandRow(y), ref.bandRow(y + d), cur.bandTexRow(y), ref.bandTexRow(y + d),
                                         &bandIncl_[size_t(y) * B], &refBandIncl_[size_t(y + d) * B], B, tol, &t.agree[tile],
                                         &t.inf[tile]);
    t.rows += agreeing >= 2;
  }
  if (y0 < y1) t.texturedRows = texturedRows_[size_t(y1)] - texturedRows_[size_t(y0)];
  t.total();
}

// The same for cells, on every second row, per tile of bands: the finer
// check that decides between neighbouring offsets and between lines of text
// that differ in a few characters.
void ScrollTracker::cellTally(const Signature& cur, const Signature& ref, int d, Tally& t) const {
  const int H = cur.h, B = cur.bands, C = cur.cells;
  const int y0 = std::max(0, -d), y1 = std::min(H, H - d);
  t.clear(B, H);
  const unsigned tol = unsigned(opts_.cellTolerance);
  std::vector<uint16_t> ca(static_cast<size_t>(C)), ci(static_cast<size_t>(C));
  const auto fold = [&](int tileRow) {
    for (int k = 0; k < B; ++k) {
      unsigned a = 0, i = 0;
      for (int c = cur.bandLo[k]; c < cur.bandLo[k + 1]; ++c) {
        a += ca[size_t(c)];
        i += ci[size_t(c)];
      }
      t.agree[size_t(tileRow) * B + k] = uint16_t(t.agree[size_t(tileRow) * B + k] + a);
      t.inf[size_t(tileRow) * B + k] = uint16_t(t.inf[size_t(tileRow) * B + k] + i);
    }
    std::fill(ca.begin(), ca.end(), 0);
    std::fill(ci.begin(), ci.end(), 0);
  };
  int tileRow = y0 / kTileRows;
  for (int y = y0; y < y1; y += 2) {
    if (y / kTileRows != tileRow) {
      fold(tileRow);
      tileRow = y / kTileRows;
    }
    accumulate(cur.cellRow(y), ref.cellRow(y + d), cur.cellTexRow(y), ref.cellTexRow(y + d), &cellIncl_[size_t(y) * C],
               &refCellIncl_[size_t(y + d) * C], C, tol, ca.data(), ci.data());
  }
  if (y0 < y1) fold(tileRow);
  t.total();
}

// Whether the frame shows what ref showed (no movement), by the share of
// textured cells that are where they were, pinned blocks (and dynamic page
// bands, where enough static content remains) left out.
// A browser can draw a page's text anew while it rests: Chrome turns greyscale
// antialiasing into subpixel (LCD) antialiasing some time after scrolling
// stopped, and keeps drawing it so (#1537's A-first row: 16% of the textured
// cells changed in place, every later frame in the new look). Each cell moves
// a little: within twice the cell tolerance nearly all agree where they were,
// where a page moved by even a pixel or a playing video agrees on far fewer.
constexpr unsigned kRedrawTolerance = 2;

bool ScrollTracker::stillFrame(const Signature& cur, const Signature& ref, double* share, bool* redrawn) const {
  const int H = cur.h, C = cur.cells, B = cur.bands;
  unsigned agree = 0, inf = 0, differ = 0;
  unsigned looseAgree = 0, looseInf = 0, looseDiffer = 0;
  const unsigned looseTol = kRedrawTolerance * unsigned(opts_.cellTolerance);
  if (redrawn) *redrawn = false;
  const int lo = cellLo_, n = cellHi_ - cellLo_;
  std::vector<uint16_t> pinRow;
  bool skippedDyn = false;
  for (int y = 0; y < H; ++y) {
    const int brow = std::min(y / opts_.blockRows, blockRowCount_ - 1);
    const uint16_t* pin = &closed16_[size_t(brow) * C + lo];
    const long long p = lastPos_ + y;
    if (store_->has(p)) {
      const uint8_t* sd = store_->dynRow(p);
      bool rowDyn = false;
      for (int k = 0; k < B; ++k) rowDyn |= sd[k] != 0;
      if (rowDyn) {
        skippedDyn = true;
        pinRow.assign(pin, pin + n);
        for (int k = 0; k < B; ++k)
          if (sd[k])
            for (int c = cur.bandLo[size_t(k)]; c < cur.bandLo[size_t(k) + 1]; ++c) pinRow[size_t(c - lo)] = 1;
        pin = pinRow.data();
      }
    }
    stillCounts(cur.cellRow(y) + lo, ref.cellRow(y) + lo, cur.cellTexRow(y) + lo, ref.cellTexRow(y) + lo, pin, n,
                unsigned(opts_.cellTolerance), differ, inf, agree);
    if (redrawn)
      stillCounts(cur.cellRow(y) + lo, ref.cellRow(y) + lo, cur.cellTexRow(y) + lo, ref.cellTexRow(y) + lo, pin, n,
                  looseTol, looseDiffer, looseInf, looseAgree);
  }
  if (skippedDyn && inf < 32) {
    agree = inf = differ = 0;
    looseAgree = looseInf = looseDiffer = 0;
    for (int y = 0; y < H; ++y) {
      const int brow = std::min(y / opts_.blockRows, blockRowCount_ - 1);
      stillCounts(cur.cellRow(y) + lo, ref.cellRow(y) + lo, cur.cellTexRow(y) + lo, ref.cellTexRow(y) + lo,
                  &closed16_[size_t(brow) * C + lo], n, unsigned(opts_.cellTolerance), differ, inf, agree);
      if (redrawn)
        stillCounts(cur.cellRow(y) + lo, ref.cellRow(y) + lo, cur.cellTexRow(y) + lo, ref.cellTexRow(y) + lo,
                    &closed16_[size_t(brow) * C + lo], n, looseTol, looseDiffer, looseInf, looseAgree);
    }
  }
  const double s = inf > 0 ? double(agree) / inf : (differ == 0 ? 1.0 : 0.0);
  if (share) *share = s;
  if (inf < 32) return differ * 50u < unsigned(H) * unsigned(n) / 20u + 1;  // blank: nothing changed
  if (s >= opts_.stillShare) return true;
  if (redrawn) *redrawn = double(looseAgree) / looseInf >= opts_.stillShare;
  return false;
}

namespace {
// Peaks and fine candidates kept beyond the usual few when they are about as
// good as the best (content that repeats), at most this many, within this
// share of the best.
constexpr size_t kSimilarMax = 16;
constexpr double kSimilarShare = 0.02;
// A frame still to this share of its textured cells is the same picture;
// below it, a movement whose cells agree by kStillMargin more explains it
// better.
constexpr double kStillSure = 0.995;
constexpr double kStillMargin = 0.01;
// A frame that repeats itself within this share of bands of how well the
// pair matches cannot be placed by the pair after skipped frames.
constexpr double kRepeatMargin = 0.05;
// A movement whose candidate agrees in kWholeCells of its cells and bands,
// and the whole overlap in kWholeShare, leaving at most half the disagreement
// that standing still leaves, is the page's though it covers too little to be
// accepted (content repeating at the step's distance; a sticky header not
// quite all pinned leaves the whole overlap at 0.95).
constexpr double kWholeCells = 0.99;
constexpr double kWholeShare = 0.90;
// Offsets closer than this are the same place.
constexpr int kSamePlaceRows = 8;
int sliverRows(const TrackerOptions& o, int h) {
  return std::max(o.minOverlapRows, int(std::ceil(o.minOverlapFraction * h)));
}
}  // namespace

// Scores a candidate fully: bands and cells over every row, and whether it
// is acceptable as the page's movement.
// A tile stood still when at least two of its textured cells, and half of
// them, are strictly where they were (what is pinned is drawn again the
// same; photo-like noise only sometimes looks alike).
// Whether what moves with a candidate is a box: in its rows of tiles, at
// least a quarter of the tiles do not move with it (they show something
// else, stood still, or are pinned: an inner scroll area beside text or
// between side bars). A page between a header and a cookie banner that
// nothing has pinned yet moves across the width.
bool ScrollTracker::boxed(const Candidate& c) const {
  const int B = int(bandLo_.size()) - 1, tileBlocks = std::max(1, kTileRows / opts_.blockRows);
  int moving = 0, other = 0;
  for (size_t r = 0; r * size_t(B) < c.moving.size(); ++r) {
    int m = 0, o = 0;
    for (int k = 0; k < B; ++k) {
      const uint8_t v = c.moving[r * size_t(B) + size_t(k)];
      bool pinned = false;
      for (int br = int(r) * tileBlocks; br < std::min(blockRowCount_, (int(r) + 1) * tileBlocks); ++br)
        pinned |= bandPinned16_[size_t(br) * B + size_t(k)] != 0;
      m += v == 1;
      o += v == 2 || (v == 0 && pinned);
    }
    if (m == 0) continue;
    moving += m;
    other += o;
  }
  return 4 * other >= moving + other;
}

// Rows of a frame that show something outside what is pinned or excluded
// (two textured bands or more).
int ScrollTracker::texturedRowCount(const Signature& sig) const {
  const int B = sig.bands;
  int n = 0;
  for (int y = 0; y < sig.h; ++y) {
    const uint16_t* pin = &bandPinned16_[size_t(std::min(y / opts_.blockRows, blockRowCount_ - 1)) * B];
    n += countAndNot(sig.bandTexRow(y), pin, B) >= 2;
  }
  return n;
}

bool ScrollTracker::stillTile(size_t i) const { return stillTile_[i] >= 2 && 2 * stillTile_[i] >= texCellTile_[i]; }

void ScrollTracker::judge(Candidate& c, const Tally& bands, const Tally& cells, int y0, int y1,
                          bool allowAllMovingCols) const {
  const int B = bands.bands;
  const size_t tiles = bands.inf.size();
  unsigned ka = 0, ki = 0, ca = 0, ci = 0;
  for (size_t i = 0; i < tiles; ++i) {
    if (!bands.keptTile(i)) continue;
    ka += bands.agree[i];
    ki += bands.inf[i];
    ca += cells.agree[i];
    ci += cells.inf[i];
  }
  // The layer that moved by d, by area: the tiles that move with it, of the
  // tiles of the overlap that show something (that moves, or that stood
  // still). Dense text in a small box must not outweigh a sparser page.
  const int tr0 = y0 / kTileRows, tr1 = (std::max(y0, y1 - 1)) / kTileRows;
  c.moving.assign(tiles, 0);
  int moving = 0, shown = 0, other = 0;
  for (int tr = tr0; tr <= tr1; ++tr)
    for (int k = 0; k < B; ++k) {
      const size_t i = size_t(tr) * B + k;
      const bool m = bands.inf[i] >= 2 && 2 * bands.agree[i] >= bands.inf[i];
      const bool still = stillTile(i);
      c.moving[i] = m ? 1 : (bands.inf[i] >= 2 || still) ? 2 : 0;
      moving += m;
      shown += m || bands.inf[i] >= 2 || still;
      other += !m && !still && bands.inf[i] >= 2;
    }
  // And whether it moved over the frame's height in its columns: per row of
  // tiles in the columns that mostly agree, moving tiles against tiles that
  // stood still. An inner scroll area (a page's own example box under the
  // pointer) moves in a few rows while the text above and below it in the
  // same columns stands still; a page moves everywhere but under a header.
  std::vector<unsigned> colA(size_t(B), 0), colI(size_t(B), 0);
  for (size_t i = 0; i < tiles; ++i) {
    colA[i % size_t(B)] += bands.agree[i];
    colI[i % size_t(B)] += bands.inf[i];
  }
  int movedRows = 0, stillRows = 0;
  auto countCoverage = [&](bool allMovingCols) {
    movedRows = stillRows = 0;
    for (int tr = tr0; tr <= tr1; ++tr) {
      int m = 0, st = 0;
      for (int k = 0; k < B; ++k) {
        if (colI[size_t(k)] == 0) continue;
        if (allMovingCols ? (colA[size_t(k)] == 0) : (2 * colA[size_t(k)] < colI[size_t(k)])) continue;
        const size_t i = size_t(tr) * B + k;
        if (c.moving[i] == 1) ++m;
        else if (stillTile(i)) ++st;
      }
      movedRows += m > st;
      stillRows += st > m;
    }
  };
  countCoverage(false);
  // When dynamic bands exist or during the low-floor dynamic retry, a large
  // full-width playing video (>50% of viewport height) can drop every column
  // below 50% agreement even though the surrounding page moves cleanly and
  // nothing stands still; fall back to columns with agreeing bands (#1537).
  if (movedRows + stillRows == 0 && (hasDyn_ || allowAllMovingCols)) countCoverage(true);
  c.bands = bands.share();
  c.cells = cells.share();
  c.area = shown ? double(moving) / shown : 0.0;
  c.other = shown ? double(other) / shown : 0.0;
  c.movingTiles = moving;
  c.bandsKept = ki ? double(ka) / ki : 0.0;
  c.cellsKept = ci ? double(ca) / ci : 0.0;
  c.informative = int(bands.infSum);
  c.rows = bands.rows;
  c.coverage = movedRows + stillRows ? double(movedRows) / (movedRows + stillRows) : 0.0;
  const bool agree = (c.bandsKept >= opts_.acceptBands && c.cellsKept >= opts_.acceptCells) ||
                     (c.bandsKept >= opts_.acceptBandsWithCells && c.cellsKept >= opts_.acceptCellsStrong);
  const bool base = c.area >= opts_.minArea && int(ki) >= 4 * opts_.minEvidenceRows && y1 - y0 >= sliverRows(opts_, height_);
  c.accepted = base && agree && c.coverage >= opts_.minRowCoverage && c.rows >= opts_.minEvidenceRows;
  // For decide's last lines of a page: at least half the rows of evidence,
  // and either acceptable but for its rows, or agreeing strongly whatever its
  // row coverage. Under a header and a footer the page's last lines fill one
  // row of tiles, while the edges of their text the pinned mask does not
  // quite cover stand still in two (seed 51: 3 px agreeing in every lane,
  // coverage 0.33).
  const bool strong = c.bandsKept >= 0.95 && c.cellsKept >= opts_.acceptCellsStrong;
  c.fewRows = base && !c.accepted && 2 * c.rows >= opts_.minEvidenceRows &&
              ((agree && c.coverage >= opts_.minRowCoverage) || strong);
}

// The coarse look at a candidate: the share of agreeing bands over every
// 16th row of the overlap, the first quarter of them (every 64th) before it
// is given up.
double ScrollTracker::coarseShare(const Signature& cur, const Signature& ref, int d, double abortBelow,
                                  const std::vector<uint16_t>& incl) const {
  const int H = cur.h, B = cur.bands;
  const unsigned tol = unsigned(bandTol_);
  unsigned agree = 0, inf = 0;
  int used = 0;
  for (int y : anchors_) {
    if (y + d < 0 || y + d >= H) continue;
    lanes(cur.bandRow(y), ref.bandRow(y + d), cur.bandTexRow(y), ref.bandTexRow(y + d), &incl[size_t(y) * B],
          &refBandIncl_[size_t(y + d) * B], B, tol, agree, inf);
    ++used;
    if ((used == 8 || used == 24) && inf >= 24 && double(agree) / inf < abortBelow) break;
    if (used == 64) break;
  }
  return inf >= 16 ? double(agree) / inf : 0.0;
}

// Every offset that keeps more than a sliver of overlap, coarse first, then
// the local best of each basin on every row, then cells.
std::vector<ScrollTracker::Candidate> ScrollTracker::searchPair(const Signature& cur, const Signature& ref,
                                                                 int expected, double floorOverride) const {
  std::vector<Candidate> out;
  searchPass(cur, ref, expected, bandIncl_, out, floorOverride);
  // A second layer: when the best candidate leaves much of the frame neither
  // moving with it nor standing still, something else moved too (the page
  // under a dense inner box, or the box over a sparse page). Its tiles are
  // left out of a second coarse look, so the other layer is found, and the
  // decision chooses between them.
  int first = -1;
  for (size_t i = 0; i < out.size(); ++i)
    if (out[i].accepted && (first < 0 || out[i].cells > out[size_t(first)].cells)) first = int(i);
  if (first < 0 || out[size_t(first)].other < opts_.secondLayer) return out;
  const int B = cur.bands, H = cur.h;
  std::vector<uint16_t> masked = bandIncl_;
  const std::vector<uint8_t>& m = out[size_t(first)].moving;
  for (int y = 0; y < H; ++y)
    for (int k = 0; k < B; ++k)
      if (m[size_t(y / kTileRows) * B + k] == 1) masked[size_t(y) * B + k] = 0;
  searchPass(cur, ref, expected, masked, out, floorOverride);
  return out;
}

// One look: every offset that keeps more than a sliver of overlap, coarsely
// (with `incl` for this frame's lanes), then the local best of each basin
// on every row, then cells; the candidates are added to `out`.
void ScrollTracker::searchPass(const Signature& cur, const Signature& ref, int expected,
                               const std::vector<uint16_t>& incl, std::vector<Candidate>& out,
                               double floorOverride) const {
  const int H = cur.h;
  // A limit, not a defect: the pair follows moves up to H less a sliver
  // (minOverlapRows, or minOverlapFraction of H). At the product's smallest
  // region (64 rows) that is 16 rows; a larger move is looked up in the
  // image, which needs the same overlap, so a region that small follows a
  // wheel only when the application animates its notches in small frames.
  const int dmax = H - sliverRows(opts_, H);
  if (dmax < 1) return;
  const int n = 2 * dmax + 1;
  std::vector<float> coarse(size_t(n), 0.f);
  const double floor = floorOverride > 0.0 ? floorOverride : opts_.coarseFloor;
  for (int d = -dmax; d <= dmax; ++d)
    if (d != 0) coarse[size_t(d + dmax)] = float(coarseShare(cur, ref, d, floor, incl));
  std::vector<int> peaks = pickPeaks(coarse, floor, std::clamp(expected, -dmax, dmax) + dmax);
  // Each basin's best on every row; the most promising ones then by cells.
  struct Fine {
    int d = 0;
    Tally bands;
  };
  std::vector<Fine> fine;
  Tally t, tc;
  for (int p : peaks) {
    const int at = p - dmax;
    Fine best;
    double bestShare = -1;
    for (int d = std::max(-dmax, at - 1); d <= std::min(dmax, at + 1); ++d) {
      if (d == 0) continue;
      bandTally(cur, ref, d, t);
      if (t.share() > bestShare) {
        bestShare = t.share();
        best.d = d;
        best.bands = t;
      }
    }
    if (bestShare >= 0) fine.push_back(std::move(best));
  }
  // Best by bands, in steps of 0.01 (a comparison within a tolerance would
  // not be a strict weak ordering); equal ones nearest the expected offset.
  const auto fineKey = [&](const Fine& f) {
    return std::make_tuple(-std::llround(f.bands.share() * 100), std::abs(f.d - expected), f.d);
  };
  std::sort(fine.begin(), fine.end(), [&](const Fine& a, const Fine& b) { return fineKey(a) < fineKey(b); });
  if (!fine.empty()) {
    const double top = fine[0].bands.share();
    std::erase_if(fine, [&](const Fine& f) { return f.bands.share() < top - 0.4; });
  }
  // The five best, and every other one about as good as the best (content
  // that repeats: a far offset that fits as well must be judged, or the one
  // nearest the expected offset wins unopposed; #1515's acceptance lost four
  // rows of a pull request's test table so).
  if (fine.size() > 5) {
    const double top = fine[0].bands.share();
    size_t keep = 5;
    while (keep < fine.size() && keep < kSimilarMax && fine[keep].bands.share() >= top - kSimilarShare) ++keep;
    fine.resize(keep);
  }
  for (const Fine& f : fine) {
    cellTally(cur, ref, f.d, tc);
    Candidate c;
    c.d = f.d;
    judge(c, f.bands, tc, std::max(0, -f.d), std::min(H, H - f.d), floorOverride > 0.0);
    out.push_back(c);
  }
  // The best by cells, and any that ties with it, to the row: a neighbour
  // that agrees in more cells is the place (whole pixels, never one off).
  // At most kWalk rows each way: the basins come from a look at every
  // offset, so the place is a few rows from one, and each step costs a full
  // look by cells and bands.
  constexpr int kWalk = 32;
  double best = -1;
  for (const Candidate& c : out) best = std::max(best, c.cells);
  for (Candidate& c : out) {
    if (c.cells < best - opts_.tieCells) continue;
    for (int step : {-1, 1}) {
      for (int d = c.d + step, walked = 0; d != 0 && std::abs(d) <= dmax && walked < kWalk; d += step, ++walked) {
        cellTally(cur, ref, d, tc);
        if (tc.share() <= c.cells) break;
        bandTally(cur, ref, d, t);
        Candidate n;
        n.d = d;
        judge(n, t, tc, std::max(0, -d), std::min(H, H - d), floorOverride > 0.0);
        c = n;
      }
    }
  }
  std::sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) { return a.d < b.d; });
  out.erase(std::unique(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) { return a.d == b.d; }), out.end());
}

// Local maxima of a coarse score above the floor, best first, a few of them,
// and none far below the best.
std::vector<int> ScrollTracker::pickPeaks(const std::vector<float>& coarse, double floor, long long center) const {
  const int n = int(coarse.size());
  std::vector<int> peaks;
  for (int i = 0; i < n; ++i) {
    const float v = coarse[size_t(i)];
    if (v < floor) continue;
    bool peak = true;
    for (int j = std::max(0, i - 3); j <= std::min(n - 1, i + 3) && peak; ++j)
      if (j != i && (coarse[size_t(j)] > v || (coarse[size_t(j)] == v && j < i))) peak = false;
    if (peak) peaks.push_back(i);
  }
  // Equal ones (content that repeats) nearest the expected place first, so
  // that the candidates the expected offset must choose between are kept.
  // In steps of 0.02, as a strict weak ordering.
  const auto peakKey = [&](int i) { return std::make_tuple(-std::lround(coarse[size_t(i)] * 50.0), std::llabs(i - center), i); };
  std::sort(peaks.begin(), peaks.end(), [&](int a, int b) { return peakKey(a) < peakKey(b); });
  if (!peaks.empty()) {
    const float best = coarse[size_t(peaks[0])];
    std::erase_if(peaks, [&](int i) { return coarse[size_t(i)] < best - 0.35f; });
  }
  // Six, and every other one about as good as the best (see searchPass).
  if (peaks.size() > 6) {
    const float best = coarse[size_t(peaks[0])];
    size_t keep = 6;
    while (keep < peaks.size() && keep < kSimilarMax && coarse[size_t(peaks[keep])] >= best - float(kSimilarShare)) ++keep;
    peaks.resize(keep);
  }
  return peaks;
}

// The decision among the candidates: the best by cells; equal ones (content
// that repeats) are decided by the expected offset only when it singles one
// out, otherwise the frame is skipped. Returns the index, or -1.
int ScrollTracker::decide(std::vector<Candidate>& cands, int expected, int uncertainty, bool hinted,
                          bool* byExpectation, SkipReason* why, int* unexplainedPick) const {
  *byExpectation = false;
  if (unexplainedPick) *unexplainedPick = -1;
  std::vector<int> ok;
  int maxInf = 0;
  for (size_t i = 0; i < cands.size(); ++i) {
    maxInf = std::max(maxInf, cands[i].informative);
    if (cands[i].accepted) ok.push_back(int(i));
  }
  // The page's last lines above a blank tail: as the page scrolls on, fewer
  // rows of it are left to compare, and a match that agrees in every lane
  // can fall short of minEvidenceRows only by its rows (13 of 16 at seed 51
  // under a header and a footer: every later frame went unmatched and the
  // capture lost the page where it simply ended, #1543's review). With at
  // least half the rows it is taken where the hint says the page is and it
  // is the only one there: the content and the driver's step agree.
  if (ok.empty() && hinted) {
    int only = -1, near = 0;
    for (size_t i = 0; i < cands.size(); ++i)
      if (cands[i].fewRows && std::abs(cands[i].d - expected) <= uncertainty) {
        only = int(i);
        ++near;
      }
    if (near == 1) {
      ok.push_back(only);
      *byExpectation = true;
    }
  }
  if (ok.empty()) {
    *why = maxInf < 4 * opts_.minEvidenceRows ? SkipReason::NoEvidence : SkipReason::NoMatch;
    return -1;
  }
  // Which layer is the page. Another layer may move too: an inner scroll
  // area, a strip at a pace of its own, a header and side bars on their way
  // into place while the page scrolls.
  // 1. Layers: candidates that move mostly the same tiles are one layer at
  //    offsets that repeat its content.
  const int B = int(bandLo_.size()) - 1;
  std::vector<int> layerOf(cands.size(), -1);
  int layers = 0;
  for (int i : ok) {
    for (int j : ok) {
      if (layerOf[size_t(j)] < 0 || j == i) continue;
      const std::vector<uint8_t>& a = cands[size_t(i)].moving;
      const std::vector<uint8_t>& b = cands[size_t(j)].moving;
      int both = 0;
      for (size_t t = 0; t < a.size() && t < b.size(); ++t) both += a[t] == 1 && b[t] == 1;
      if (2 * both >= std::min(cands[size_t(i)].movingTiles, cands[size_t(j)].movingTiles)) {
        layerOf[size_t(i)] = layerOf[size_t(j)];
        break;
      }
    }
    if (layerOf[size_t(i)] < 0) layerOf[size_t(i)] = layers++;
  }
  // 2. Per column (band), the layer whose tiles span the most of the
  //    column's height (from the first moving tile to the last, as a share
  //    of the overlap) holds it: the page reaches above and below an inner
  //    box however sparse it is there, side bars are narrower than the page,
  //    a header that moves is a row or two of each column. The page is the
  //    layer that holds the most columns; ties go to the larger area.
  std::vector<int> held(size_t(layers), 0);
  std::vector<double> area(static_cast<size_t>(layers), 0.0), extent(static_cast<size_t>(layers));
  for (int i : ok) area[size_t(layerOf[size_t(i)])] = std::max(area[size_t(layerOf[size_t(i)])], cands[size_t(i)].area);
  for (int k = 0; k < B; ++k) {
    std::fill(extent.begin(), extent.end(), 0.0);
    for (int i : ok) {
      // Segments of the column between tiles that disagree with the layer
      // (they show something that does not move with it): within each, the
      // rows from the first moving tile to the last; blank tiles neither
      // count nor interrupt, a single moving tile is no segment.
      const std::vector<uint8_t>& m = cands[size_t(i)].moving;
      int span = 0, first = -1, last = -1, count = 0;
      const auto close = [&] {
        if (count >= 2) span += last - first + 1;
        first = last = -1;
        count = 0;
      };
      for (size_t t = size_t(k), r = 0; t < m.size(); t += size_t(B), ++r) {
        if (m[t] == 2) {
          close();
        } else if (m[t] == 1) {
          if (first < 0) first = int(r);
          last = int(r);
          ++count;
        }
      }
      close();
      const int overlapRows = std::max(1, (height_ - std::abs(cands[size_t(i)].d)) / kTileRows);
      const double share = double(span) / overlapRows;
      double& e = extent[size_t(layerOf[size_t(i)])];
      e = std::max(e, share >= 0.25 ? share : 0.0);
    }
    const double most = *std::max_element(extent.begin(), extent.end());
    if (most > 0)
      for (int l = 0; l < layers; ++l) held[size_t(l)] += extent[size_t(l)] >= most;
  }
  int page = 0;
  for (int l = 1; l < layers; ++l)
    if (held[size_t(l)] > held[size_t(page)] || (held[size_t(l)] == held[size_t(page)] && area[size_t(l)] > area[size_t(page)]))
      page = l;
  // The page keeps the pace it had: when exactly one layer continues the
  // last movement and the columns' layer does not come near it, the one that
  // continues is the page, whatever the columns say (an inner box as wide as
  // the article, with sparse text above and below it, or a code block that
  // scrolled on its own and then with the page). A smooth scroll changes
  // pace by half from frame to frame; a side bar on its way into place moves
  // by part of the page's step, which may happen to be near the last one.
  if (layers > 1 && lastMove_ != 0) {
    const int near = std::max(8, std::abs(lastMove_) / 5), far = std::max(8, std::abs(lastMove_) / 2);
    int consistent = -1, count = 0;
    bool columnsNear = false;
    std::vector<uint8_t> seen(size_t(layers), 0);
    for (int i : ok) {
      const int l = layerOf[size_t(i)];
      if (l == page && std::abs(cands[size_t(i)].d - lastMove_) <= far) columnsNear = true;
      if (seen[size_t(l)] || std::abs(cands[size_t(i)].d - lastMove_) > near) continue;
      seen[size_t(l)] = 1;
      consistent = l;
      ++count;
    }
    if (count == 1 && !columnsNear) page = consistent;
  }
  // The page's layer must explain the frame: tiles that neither move with
  // any layer found nor stood still (a page further away than the frame is
  // tall shows nothing of the last one; a column that sticks may still match)
  // must not outnumber its own moving tiles.
  {
    int best = -1;
    for (int i : ok)
      if (layerOf[size_t(i)] == page && (best < 0 || cands[size_t(i)].movingTiles > cands[size_t(best)].movingTiles)) best = i;
    const std::vector<uint8_t>& m = cands[size_t(best)].moving;
    int shown = 0, unexplained = 0;
    for (size_t t = 0; t < m.size(); ++t) {
      if (m[t] == 0) continue;
      ++shown;
      if (m[t] != 2 || stillTile(t)) continue;
      bool explained = false;
      for (int i : ok) explained |= t < cands[size_t(i)].moving.size() && cands[size_t(i)].moving[t] == 1;
      unexplained += !explained;
    }
    if (unexplained > cands[size_t(best)].movingTiles && 5 * unexplained >= 2 * shown) {
      // Boundary 32-row tiles containing both static text and a playing video
      // edge can lower cellsKept below recoverCells (0.95) even when every
      // static row matches exactly; 0.80 still rejects weak matches (#1537).
      if (unexplainedPick && cands[size_t(best)].cellsKept >= 0.80 &&
          cands[size_t(best)].coverage >= opts_.minRowCoverage) {
        // Report only if there is no tied rival offset in the page's layer.
        bool rival = false;
        for (int i : ok)
          if (i != best && layerOf[size_t(i)] == page && std::abs(cands[size_t(i)].d - cands[size_t(best)].d) >= kSamePlaceRows &&
              cands[size_t(i)].cells >= cands[size_t(best)].cells - opts_.tieCells)
            rival = true;
        if (!rival) *unexplainedPick = best;
      }
      *why = SkipReason::NoMatch;
      return -1;
    }
  }
  // 3. Within the page's layer, cells decide; then the expected offset.
  std::erase_if(ok, [&](int i) { return layerOf[size_t(i)] != page; });
  std::sort(ok.begin(), ok.end(), [&](int a, int b) { return cands[size_t(a)].cells > cands[size_t(b)].cells; });
  const double best = cands[size_t(ok[0])].cells;
  std::vector<int> tied;
  for (int i : ok)
    if (cands[size_t(i)].cells >= best - opts_.tieCells) tied.push_back(i);
  if (tied.size() == 1) return tied[0];
  // Content repeats: the nearest to the expected offset, if no other is near it.
  std::sort(tied.begin(), tied.end(), [&](int a, int b) {
    return std::abs(cands[size_t(a)].d - expected) < std::abs(cands[size_t(b)].d - expected);
  });
  const int first = std::abs(cands[size_t(tied[0])].d - expected);
  const int second = std::abs(cands[size_t(tied[1])].d - expected);
  // Without the driver's hint the expectation is the last movement, which
  // says something only about the very next frame.
  const bool expectation = hinted || framesSincePlaced_ == 1;
  if (expectation && first <= uncertainty && second > uncertainty && second > 2 * first) {
    *byExpectation = true;
    return tied[0];
  }
  *why = SkipReason::Ambiguous;
  return -1;
}

// Bands of the frame placed at pos against the store's rows.
void ScrollTracker::storeBandTally(const Signature& cur, long long pos, int rowStep, Tally& t) const {
  const int H = cur.h, B = cur.bands;
  t.clear(B, H);
  const unsigned tol = unsigned(bandTol_);
  for (int y = 0; y < H; y += rowStep) {
    const long long p = pos + y;
    if (!store_->has(p)) continue;
    const uint16_t* sv = store_->validRow(p);
    const uint16_t* sb = store_->bandRow(p);
    const uint16_t* a = cur.bandRow(y);
    const uint16_t* ta = cur.bandTexRow(y);
    const uint16_t* st = store_->texRow(p);
    const uint16_t* inc = &bandIncl_[size_t(y) * B];
    const uint16_t* pa = &bandPinned16_[size_t(std::min(y / opts_.blockRows, blockRowCount_ - 1)) * B];
    uint16_t* tA = &t.agree[size_t(y / kTileRows) * B];
    uint16_t* tI = &t.inf[size_t(y / kTileRows) * B];
    unsigned rowAgree = 0, rowInf = 0, rowTex = 0;
    for (int k = 0; k < B; ++k) {
      const unsigned use = unsigned(ta[k] | st[k]) & sv[k] & inc[k];
      const unsigned ok = use & unsigned(absDiff(a[k], sb[k]) <= tol);
      tI[k] = uint16_t(tI[k] + use);
      tA[k] = uint16_t(tA[k] + ok);
      rowAgree += ok;
      rowInf += use;
      rowTex += unsigned(ta[k]) & unsigned(!pa[k]) & sv[k];
    }
    t.agreeSum += rowAgree;
    t.infSum += rowInf;
    t.rows += rowAgree >= 2;
    t.texturedRows += rowTex >= 2;
  }
}

// The coarse look at a position in the image: bands against the store over
// every 16th row, the first half before it is given up.
double ScrollTracker::storeCoarseShare(const Signature& cur, long long pos, double abortBelow) const {
  const int B = cur.bands;
  const unsigned tol = unsigned(bandTol_);
  unsigned agree = 0, inf = 0;
  int used = 0;
  for (int y : anchors_) {
    const long long p = pos + y;
    if (!store_->has(p)) continue;
    lanes(cur.bandRow(y), store_->bandRow(p), cur.bandTexRow(y), store_->texRow(p), &bandIncl_[size_t(y) * B],
          store_->validRow(p), B, tol, agree, inf);
    ++used;
    // A position whose first anchors agree on less than half the floor is
    // given up early (the page's own place agrees on nearly all of them).
    if (used == 6 && inf >= 12 && 2.0 * agree < abortBelow * inf) break;
    if ((used == 16 || used == 40) && inf >= 32 && double(agree) / inf < abortBelow) break;
    if (used == 64) break;
  }
  return inf >= 16 ? double(agree) / inf : 0.0;
}

// The cell signatures of the canvas's written rows [from, from + n), read
// back and computed the same way as a frame's (have: 0 where not written).
void ScrollTracker::storeCellSigs(long long from, int n, std::vector<uint16_t>& sigs, std::vector<uint8_t>& have) const {
  const int C = cells_, P = opts_.cellPx;
  sigs.assign(size_t(n) * C, 0);
  have.assign(size_t(n), 0);
  std::vector<uint8_t> row(size_t(width_) * 4);
  for (int i = 0; i < n; ++i) {
    const long long p = from + i;
    if (!store_->has(p) || !canvas_.readRow(p, row.data())) continue;
    have[size_t(i)] = 1;
    uint16_t* sig = &sigs[size_t(i) * C];
    for (int c = cellLo_; c < cellHi_; ++c) {
      const int x0 = c * P, x1 = std::min(width_, x0 + P);
      unsigned sum = 0;
      for (int x = x0; x < x1; ++x) sum += unsigned(row[4 * x]) + 2u * row[4 * x + 1] + row[4 * x + 2];
      sig[c] = uint16_t(sum * 16u / unsigned(x1 - x0));
    }
  }
}

// Cells of the frame placed at pos against the canvas's written rows, from
// signatures of the rows [from, from + n) computed once (storeCellSigs).
void ScrollTracker::storeCellTally(const Signature& cur, long long pos, const std::vector<uint16_t>& sigs,
                                   const std::vector<uint8_t>& have, long long from, Tally& t) const {
  const int H = cur.h, C = cur.cells, B = cur.bands;
  t.clear(B, H);
  const unsigned tol = unsigned(opts_.cellTolerance);
  const long long n = (long long)have.size();
  for (int y = 0; y < H; ++y) {
    const long long p = pos + y;
    if (p < from || p >= from + n || !have[size_t(p - from)]) continue;
    const uint16_t* sig = &sigs[size_t(p - from) * C];
    const uint16_t* a = cur.cellRow(y);
    const uint16_t* ta = cur.cellTexRow(y);
    const uint16_t* inc = &cellIncl_[size_t(y) * C];
    uint16_t* tA = &t.agree[size_t(y / kTileRows) * B];
    uint16_t* tI = &t.inf[size_t(y / kTileRows) * B];
    const uint8_t* sd = store_->has(p) ? store_->dynRow(p) : nullptr;
    for (int k = 0; k < B; ++k) {
      if (sd && sd[k]) continue;
      for (int c = cur.bandLo[k]; c < cur.bandLo[k + 1]; ++c) {
        if (store_->level(p, c) == 0 || !inc[c] || !ta[c]) continue;
        const unsigned ok = absDiff(a[c], sig[size_t(c)]) <= tol;
        ++tI[k];
        tA[k] = uint16_t(tA[k] + ok);
        ++t.infSum;
        t.agreeSum += ok;
      }
    }
  }
}

// The rows a frame placed at pos shares with the image must agree with it
// (the second opinion after the pair: wayscrollshot's check against the
// long image). Dynamic bands (such as playing video) are excluded here.
bool ScrollTracker::contradicted(const Signature& cur, long long pos, double keepShare) const {
  Tally t;
  storeBandTally(cur, pos, 1, t);
  // By area: dense text in a box that moved on its own must not outvote a
  // sparser page.
  int kept = 0, shown = 0;
  const int B = cur.bands;
  int tileRows = 0;
  for (size_t i = 0; i < t.inf.size(); ++i) {
    if (t.inf[i] < 2) continue;
    ++shown;
    kept += t.keptTile(i);
  }
  for (size_t r = 0; r * size_t(B) < t.inf.size(); ++r)
    for (int k = 0; k < B; ++k)
      if (t.inf[r * size_t(B) + size_t(k)] >= 2) {
        ++tileRows;
        break;
      }
  // Over a third of the frame's rows at least, as a place found again: a
  // sliver of the image at the frame's top (the first frames' cells still
  // wait to be written) under something that has just appeared there (a
  // header that sticks once the page moved) says nothing about the place.
  return shown >= 16 && 3 * tileRows * kTileRows >= cur.h && double(kept) / shown < keepShare;
}

// The share of textured cells outside what is pinned, excluded or dynamic,
// over the whole overlap, that agree between the frame and the reference at d
// (the still lanes included).
double ScrollTracker::wholeCells(const Signature& cur, const Signature& ref, int d) const {
  const int H = cur.h, C = cur.cells, B = cur.bands;
  const unsigned tol = unsigned(opts_.cellTolerance);
  unsigned agree = 0, inf = 0;
  for (int y = std::max(0, -d); y < std::min(H, H - d); ++y) {
    const uint16_t* pa = &closed16_[size_t(std::min(y / opts_.blockRows, blockRowCount_ - 1)) * C];
    const uint16_t* pb = &closed16_[size_t(std::min((y + d) / opts_.blockRows, blockRowCount_ - 1)) * C];
    const uint16_t *a = cur.cellRow(y), *b = ref.cellRow(y + d), *ta = cur.cellTexRow(y), *tb = ref.cellTexRow(y + d);
    const long long p = lastPos_ + y + d;
    const uint8_t* sd = store_->has(p) ? store_->dynRow(p) : nullptr;
    for (int k = 0; k < B; ++k) {
      if (sd && sd[k]) continue;
      for (int c = cur.bandLo[size_t(k)]; c < cur.bandLo[size_t(k) + 1]; ++c) {
        if (pa[c] || pb[c] || !(ta[c] | tb[c])) continue;
        ++inf;
        agree += absDiff(a[c], b[c]) <= tol;
      }
    }
  }
  return inf ? double(agree) / inf : 0.0;
}

// How well rows [y0, y1) of a frame repeat elsewhere in it: the best share
// of agreeing bands between those rows and the rows s above or below them,
// over shifts of a sliver or more (textured lanes, what is pinned left out,
// every second row).
double ScrollTracker::selfRepeat(const Signature& sig, int y0, int y1, long long sigPos) const {
  const int H = sig.h, B = sig.bands, far = sliverRows(opts_, H);
  const unsigned tol = unsigned(bandTol_);
  y0 = std::clamp(y0, 0, H);
  y1 = std::clamp(y1, y0, H);
  double best = 0;
  for (int s = far; s < H; ++s)
    for (const int dir : {-1, 1}) {
      unsigned agree = 0, inf = 0;
      for (int y = y0; y < y1; y += 2) {
        const int z = y + dir * s;
        if (z < 0 || z >= H) continue;
        const uint16_t* pa = &bandPinned16_[size_t(std::min(y / opts_.blockRows, blockRowCount_ - 1)) * B];
        const uint16_t* pb = &bandPinned16_[size_t(std::min(z / opts_.blockRows, blockRowCount_ - 1)) * B];
        const uint8_t* da = hasDyn_ && store_->has(sigPos + y) ? store_->dynRow(sigPos + y) : nullptr;
        const uint8_t* db = hasDyn_ && store_->has(sigPos + z) ? store_->dynRow(sigPos + z) : nullptr;
        const uint16_t *a = sig.bandRow(y), *b = sig.bandRow(z), *ta = sig.bandTexRow(y), *tb = sig.bandTexRow(z);
        for (int k = 0; k < B; ++k) {
          if (pa[k] || pb[k] || (da && da[k]) || (db && db[k]) || !(ta[k] | tb[k])) continue;
          ++inf;
          agree += absDiff(a[k], b[k]) <= tol;
        }
      }
      if (inf >= unsigned(2 * opts_.minEvidenceRows)) best = std::max(best, double(agree) / inf);
    }
  return best;
}

// Whether a place found again (after skipped frames) has a look-alike: the
// whole image is looked through, and another accepted place that fits within
// recoverTie of the place's own fit there (its cells against the image; 0
// when the lookup does not find the place itself) means the frame could be
// either, or neither (content the image does not hold, such as numbered
// paragraphs below a blank stretch that look like those above it). Run only
// for a place about to be taken, so its cost (a coarse look at every row of
// the image, then up to 16 places by cells) does not come every lost frame.
bool ScrollTracker::lookAlike(const Signature& cur, long long pos, const std::vector<long long>& known) const {
  const std::vector<Candidate> found = searchStore(cur, pos, top_ - height_, bottom_, 16);
  // Each on the same rows of the frame, those the place has inside the
  // image: at the image's edge a few rows agree all too easily, and the
  // others, on all their rows, would show more of what differs.
  const int y0 = int(std::clamp<long long>(top_ - pos, 0, height_)), y1 = int(std::clamp<long long>(bottom_ - pos, 0, height_));
  StoreFit own;
  const double fit = storeCells(cur, pos, y0, y1, &own);
  // Where the image has the place written (it can judge), the frame must
  // agree with it on enough rows: an exact match on a few rows (the rest
  // of the place blank) is no evidence. Where it holds next to nothing
  // written yet (the first frames' cells wait), the pair's word stands.
  if (own.written * 10 >= own.cells * 3 && own.rows < opts_.minEvidenceRows) return true;
  const int d = int(pos - lastPos_);
  for (const Candidate& f : found) {
    if (!f.accepted || std::abs(f.d - d) < kSamePlaceRows) continue;
    // A place the pair found too is another layer of this frame, not a look-alike.
    if (std::any_of(known.begin(), known.end(), [&](long long k) { return std::llabs(k - (lastPos_ + f.d)) < kSamePlaceRows; })) continue;
    if (storeCells(cur, lastPos_ + f.d, y0, y1, nullptr) >= fit - opts_.recoverTie) return true;
  }
  return false;
}

// The share of the frame's informative cells of rows [y0, y1) that agree with
// the image's written cells when the frame is at pos (0 with none).
double ScrollTracker::storeCells(const Signature& cur, long long pos, int y0, int y1, StoreFit* fitOut) const {
  StoreFit local;
  StoreFit& fo = fitOut ? *fitOut : local;
  fo = {};
  if (y1 <= y0) return 0;
  std::vector<uint16_t> sigs;
  std::vector<uint8_t> have;
  storeCellSigs(pos + y0, y1 - y0, sigs, have);
  const int C = cur.cells, B = cur.bands;
  const unsigned tol = unsigned(opts_.cellTolerance);
  unsigned agree = 0, inf = 0;
  for (int y = y0; y < y1; ++y) {
    const long long p = pos + y;
    const uint8_t* sd = store_->has(p) ? store_->dynRow(p) : nullptr;
    if (!have[size_t(y - y0)]) {
      for (int k = 0; k < B; ++k)
        if (!sd || !sd[k]) fo.cells += cur.bandLo[size_t(k) + 1] - cur.bandLo[size_t(k)];
      continue;
    }
    const uint16_t* sig = &sigs[size_t(y - y0) * C];
    const uint16_t* a = cur.cellRow(y);
    const uint16_t* ta = cur.cellTexRow(y);
    const uint16_t* inc = &cellIncl_[size_t(y) * C];
    unsigned rowAgree = 0;
    for (int k = 0; k < B; ++k) {
      if (sd && sd[k]) continue;
      for (int c = cur.bandLo[size_t(k)]; c < cur.bandLo[size_t(k) + 1]; ++c) {
        ++fo.cells;
        if (store_->level(p, c) == 0) continue;
        ++fo.written;
        if (!inc[c] || !ta[c]) continue;
        ++inf;
        const bool ok = absDiff(a[c], sig[size_t(c)]) <= tol;
        agree += ok;
        rowAgree += ok;
      }
    }
    fo.rows += rowAgree >= 2;
  }
  return inf ? double(agree) / inf : 0.0;
}

// Records page bands that keep changing between consecutive frames while the
// rest of the frame agrees (#1537: a playing video, an animated banner).
// Returns true when at least one new page band becomes dynamic.
bool ScrollTracker::observeDynamic(const Signature& cur, const Signature& other, long long curPos, long long otherPos,
                                   const std::vector<Candidate>* cands, int pick, bool verifyAgreement) {
  // Two observed changes at a page band mark it dynamic (or one change between
  // other and cur when both already disagree with what store_ saved: three
  // distinct pictures of the same page band).
  constexpr int kDynHits = 2;
  constexpr int kDynTileRows = 3;
  const long long dpLL = curPos - otherPos;
  if (std::llabs(dpLL) >= height_) return false;
  const int dp = int(dpLL);
  const int H = cur.h, B = cur.bands, R = opts_.blockRows;
  const int y0 = std::max(0, -dp), y1 = std::min(H, H - dp);
  if (y1 - y0 < sliverRows(opts_, H)) return false;

  const unsigned bTol = unsigned(bandTol_), cTol = unsigned(opts_.cellTolerance);
  const int tileRows = (H + kTileRows - 1) / kTileRows;
  const size_t tiles = size_t(tileRows) * size_t(B);
  std::vector<uint16_t> tileChanged(tiles, 0), tileAgreed(tiles, 0), tileInf(tiles, 0);
  std::vector<uint16_t> tileCellA(tiles, 0), tileCellI(tiles, 0);
  std::vector<uint8_t> rowChanged(size_t(H) * size_t(B), 0);
  int agreeRows = 0;

  // At rest (dp == 0) with verifyAgreement: require that the agreeing pixels in
  // the last placed frame were themselves proved to move with the page on that
  // placement (`proofAt_ == frameSerial_`, with `frameSerial_ >= 2`). Checking
  // `!= 0` is not enough because late sticky chrome can appear over screen
  // blocks that showed the page on an earlier step (#1537).
  const int C = cells_;
  bool useProof = false;
  if (dp == 0 && verifyAgreement) {
    if (frameSerial_ < 2) return false;
    for (uint32_t at : proofAt_)
      if (at == frameSerial_) {
        useProof = true;
        break;
      }
    if (!useProof) return false;
  }
  const auto bandInPageArea = [&](int brow, int k) {
    if (!useProof) return true;
    const uint32_t* pr = &proofAt_[size_t(brow) * size_t(C)];
    for (int c = cur.bandLo[size_t(k)]; c < cur.bandLo[size_t(k) + 1]; ++c)
      if (pr[c] == frameSerial_) return true;
    return false;
  };

  auto otherLayerTile = [&](size_t t) {
    if (!cands) return false;
    for (size_t i = 0; i < cands->size(); ++i) {
      if (int(i) == pick || !(*cands)[i].accepted) continue;
      if (t < (*cands)[i].moving.size() && (*cands)[i].moving[t] == 1) return true;
    }
    return false;
  };

  for (int y = y0; y < y1; ++y) {
    const int yo = y + dp;
    const long long p = curPos + y;
    const int brow = std::min(y / R, blockRowCount_ - 1);
    const int boref = std::min(yo / R, blockRowCount_ - 1);
    const uint16_t* pa = &bandPinned16_[size_t(brow) * B];
    const uint16_t* pb = &bandPinned16_[size_t(boref) * B];
    const uint16_t* ba = cur.bandRow(y);
    const uint16_t* bb = other.bandRow(yo);
    const uint16_t* bs = other.bandRow(y);
    const uint16_t* ta = cur.bandTexRow(y);
    const uint16_t* tb = other.bandTexRow(yo);
    const uint16_t* ca = cur.cellRow(y);
    const uint16_t* cb = other.cellRow(yo);
    const uint16_t* cta = cur.cellTexRow(y);
    const uint16_t* ctb = other.cellTexRow(yo);
    const size_t tileBase = size_t(y / kTileRows) * size_t(B);
    const uint8_t* sd = store_->has(p) ? store_->dynRow(p) : nullptr;
    int rowAg = 0;
    for (int k = 0; k < B; ++k) {
      if (pa[k] || pb[k] || (sd && sd[k])) continue;
      const bool tex = (ta[k] | tb[k]) != 0;
      const bool bandOk = absDiff(ba[k], bb[k]) <= bTol;
      if (!tex && bandOk) continue;
      if (dp != 0 && absDiff(ba[k], bs[k]) <= bTol) continue;
      const size_t t = tileBase + size_t(k);
      if (tex && bandOk && !verifyAgreement) {
        ++tileInf[t];
        ++tileAgreed[t];
        ++rowAg;
        continue;
      }
      unsigned cellA = 0, cellI = 0, allC = 0, allDiff = 0;
      for (int c = cur.bandLo[size_t(k)]; c < cur.bandLo[size_t(k) + 1]; ++c) {
        const bool ok = absDiff(ca[c], cb[c]) <= cTol;
        ++allC;
        allDiff += !ok;
        if (!(cta[c] | ctb[c])) continue;
        ++cellI;
        cellA += ok;
      }
      if (tex) {
        ++tileInf[t];
        tileCellA[t] = uint16_t(tileCellA[t] + cellA);
        tileCellI[t] = uint16_t(tileCellI[t] + cellI);
      }
      if (tex && bandOk) {
        if (bandInPageArea(brow, k)) {
          ++tileAgreed[t];
          ++rowAg;
        }
      } else if (!bandOk && ((cellI > 0 && 2 * (cellI - cellA) >= cellI) || (allC > 0 && 2 * allDiff >= allC))) {
        ++tileChanged[t];
        rowChanged[size_t(y) * size_t(B) + size_t(k)] = 1;
      }
    }
    agreeRows += rowAg >= 2;
  }

  if (verifyAgreement) {
    int agreedTiles = 0, shownTiles = 0;
    unsigned ka = 0, ki = 0, caSum = 0, ciSum = 0;
    for (size_t t = 0; t < tiles; ++t) {
      if (tileInf[t] < 2 && tileChanged[t] < kDynTileRows) continue;
      ++shownTiles;
      if (tileInf[t] >= 2 && 2 * tileAgreed[t] >= tileInf[t]) {
        ++agreedTiles;
        ka += tileAgreed[t];
        ki += tileInf[t];
        caSum += tileCellA[t];
        ciSum += tileCellI[t];
      }
    }
    if (agreeRows < opts_.minEvidenceRows || shownTiles == 0) return false;
    const double area = double(agreedTiles) / shownTiles;
    const double bandsKept = ki ? double(ka) / ki : 0.0;
    const double cellsKept = ciSum ? double(caSum) / ciSum : 0.0;
    const double minCells = dp == 0 ? opts_.recoverCells : 0.80;
    if (area < opts_.minArea || bandsKept < opts_.acceptBands || cellsKept < minCells) return false;
  }

  bool anyMarked = false;
  std::vector<uint8_t> hitDone(size_t(H) * size_t(B), 0);
  const auto recordHit = [&](int y, int k) {
    const size_t idx = size_t(y) * size_t(B) + size_t(k);
    if (hitDone[idx] || !rowChanged[idx]) return;
    hitDone[idx] = 1;
    const long long p = curPos + y;
    if (!store_->has(p) || store_->isDyn(p, k)) return;
    const bool storeDiff = store_->validRow(p)[k] &&
                           absDiff(cur.bandRow(y)[k], store_->bandRow(p)[k]) > bTol &&
                           absDiff(other.bandRow(y + dp)[k], store_->bandRow(p)[k]) > bTol;
    const int step = storeDiff ? kDynHits : 1;
    uint8_t& hits = store_->dynHitsRow(p)[k];
    hits = uint8_t(std::min(255, int(hits) + step));
    if (hits >= kDynHits) anyMarked |= store_->markDyn(p, k);
  };
  for (int tr = y0 / kTileRows; tr <= (y1 - 1) / kTileRows; ++tr) {
    const int rStart = std::max(y0, tr * kTileRows);
    const int rEnd = std::min(y1, (tr + 1) * kTileRows);
    for (int k = 0; k < B; ++k) {
      const size_t t = size_t(tr) * size_t(B) + size_t(k);
      if (tileChanged[t] < kDynTileRows || tileChanged[t] <= tileAgreed[t] || otherLayerTile(t)) continue;
      for (int y = rStart; y < rEnd; ++y)
        if (rowChanged[size_t(y) * size_t(B) + size_t(k)]) recordHit(y, k);
      // Extend across the tile boundary while contiguous changed rows belong
      // to the same video whose top/bottom edge falls mid-tile (#1537).
      for (int y = rStart - 1; y >= std::max(y0, rStart - kTileRows); --y) {
        if (!rowChanged[size_t(y) * size_t(B) + size_t(k)]) break;
        if (otherLayerTile(size_t(y / kTileRows) * size_t(B) + size_t(k))) break;
        recordHit(y, k);
      }
      for (int y = rEnd; y < std::min(y1, rEnd + kTileRows); ++y) {
        if (!rowChanged[size_t(y) * size_t(B) + size_t(k)]) break;
        if (otherLayerTile(size_t(y / kTileRows) * size_t(B) + size_t(k))) break;
        recordHit(y, k);
      }
    }
  }
  hasDyn_ |= anyMarked;
  return anyMarked;
}

// Writes each dynamic page region (a playing video, an animated banner) from
// a single placed frame so its pixels are never mixed across frames (#1537).
// When a placed frame shows strictly more unblocked rows of a dynamic region
// than any earlier snapshot, it replaces the region's pixels cleanly; when a
// dynamic region is taller than the viewport, later frames only fill rows not
// yet written.
void ScrollTracker::snapshotDynamic(const FrameView& frame, long long pos, const Signature& cur, const Signature& ref,
                                    int d) {
  if (!hasDyn_ || !store_) return;
  const int H = height_, B = int(bandLo_.size()) - 1, P = opts_.cellPx;
  const int words = store_->words;
  const long long winLo = std::max({allowLo_, top_, pos - H});
  const long long winHi = std::min({allowHi_, bottom_, pos + 2LL * H});
  if (winHi <= winLo) return;
  const int nRows = int(winHi - winLo);
  // Close small vertical gaps (<= 16 rows) within a dynamic band so a single
  // momentarily matching row inside a video stays part of the same component.
  for (int k = 0; k < B; ++k) {
    int lastDyn = -1;
    for (int r = 0; r < nRows; ++r) {
      const long long p = winLo + r;
      if (!store_->has(p) || !store_->isDyn(p, k)) continue;
      if (lastDyn >= 0 && r - lastDyn > 1 && r - lastDyn - 1 <= 16)
        for (int g = lastDyn + 1; g < r; ++g) store_->markDyn(winLo + g, k);
      lastDyn = r;
    }
  }
  std::vector<uint8_t> seen(size_t(nRows) * size_t(B), 0);
  std::vector<std::pair<int, int>> stack;
  std::vector<uint64_t> blocked;
  bool haveBlocked = false;
  for (int r0 = 0; r0 < nRows; ++r0) {
    const long long pr0 = winLo + r0;
    if (!store_->has(pr0)) continue;
    for (int k0 = 0; k0 < B; ++k0) {
      const size_t idx0 = size_t(r0) * size_t(B) + size_t(k0);
      if (seen[idx0] || !store_->isDyn(pr0, k0)) continue;
      int minR = r0, maxR = r0, minK = k0, maxK = k0;
      seen[idx0] = 1;
      stack.clear();
      stack.push_back({r0, k0});
      while (!stack.empty()) {
        const auto [r, k] = stack.back();
        stack.pop_back();
        minR = std::min(minR, r);
        maxR = std::max(maxR, r);
        minK = std::min(minK, k);
        maxK = std::max(maxK, k);
        const int nb[4][2] = {{r - 1, k}, {r + 1, k}, {r, k - 1}, {r, k + 1}};
        for (const auto& q : nb) {
          if (q[0] < 0 || q[0] >= nRows || q[1] < 0 || q[1] >= B) continue;
          const size_t ni = size_t(q[0]) * size_t(B) + size_t(q[1]);
          if (seen[ni]) continue;
          const long long np = winLo + q[0];
          if (!store_->has(np) || !store_->isDyn(np, q[1])) continue;
          seen[ni] = 1;
          stack.push_back({q[0], q[1]});
        }
      }
      const long long p0 = winLo + minR, p1 = winLo + maxR + 1;
      const long long c0 = std::max({p0, pos, allowLo_}), c1 = std::min({p1, pos + H, allowHi_});
      if (c1 <= c0) continue;
      if (!haveBlocked) {
        blockedWords(blocked);
        haveBlocked = true;
      }
      const int C = cells_;
      const int bandColLo = bandLo_[size_t(minK)], bandColHi = bandLo_[size_t(maxK + 1)];
      const int writeLo = minK == 0 ? 0 : bandColLo;
      const int writeHi = maxK == B - 1 ? C : bandColHi;
      // Score each candidate frame by the exact number of unblocked cells it
      // can write across [writeLo, writeHi) over the confirmed dynamic rows
      // [p0, p1), so clearing an exclusion in an outer margin or widening the
      // dynamic region replaces an earlier partial snapshot without letting a
      // permanently pinned scrollbar zero out the score (#1537).
      long long frameCovered = 0;
      for (long long p = c0; p < c1; ++p) {
        const int y = int(p - pos);
        const int brow = std::min(y / opts_.blockRows, blockRowCount_ - 1);
        const uint64_t* blk = &blocked[size_t(brow) * size_t(words)];
        for (int c = writeLo; c < writeHi; ++c)
          if (!((blk[c >> 6] >> (c & 63)) & 1ull)) ++frameCovered;
      }
      // Also consider the retained topmost (`topCopy_`) and bottommost
      // (`bottomCopy_`) frames over the confirmed dynamic rows [p0, p1) so a
      // video that started leaving the top or bottom of the viewport before
      // being marked dynamic is still written cleanly from one frame (#1537).
      const auto scoreCopy = [&](const FrameCopy& copy, long long r0, long long r1) {
        long long cov = 0;
        for (long long p = r0; p < r1; ++p) {
          const int y = int(p - copy.pos);
          const int brow = std::min(y / opts_.blockRows, blockRowCount_ - 1);
          for (int c = writeLo; c < writeHi; ++c) {
            const size_t bi = size_t(brow) * size_t(C) + size_t(c);
            if (copy.tag[bi] == copy.serial && !closed_[bi] && !excl_[bi] && !ghost_[bi]) ++cov;
          }
        }
        return cov;
      };
      long long topCovered = 0, botCovered = 0;
      long long tc0 = 0, tc1 = 0, bc0 = 0, bc1 = 0;
      if (!topCopy_.tag.empty() && topCopy_.pos < pos) {
        tc0 = std::max({p0, topCopy_.pos, allowLo_});
        tc1 = std::min({p1, topCopy_.pos + H, allowHi_});
        if (tc1 > tc0) topCovered = scoreCopy(topCopy_, tc0, tc1);
      }
      if (!bottomCopy_.tag.empty() && bottomCopy_.pos > pos) {
        bc0 = std::max({p0, bottomCopy_.pos, allowLo_});
        bc1 = std::min({p1, bottomCopy_.pos + H, allowHi_});
        if (bc1 > bc0) botCovered = scoreCopy(bottomCopy_, bc0, bc1);
      }
      const FrameCopy* chosenCopy = nullptr;
      long long unblockedCells = frameCovered;
      long long w0 = c0, w1 = c1;
      if (topCovered > 0 && topCovered >= unblockedCells) {
        chosenCopy = &topCopy_;
        unblockedCells = topCovered;
        w0 = tc0;
        w1 = tc1;
      }
      if (botCovered > 0 && botCovered >= unblockedCells && (!chosenCopy || botCovered > unblockedCells)) {
        chosenCopy = &bottomCopy_;
        unblockedCells = botCovered;
        w0 = bc0;
        w1 = bc1;
      }
      if (unblockedCells <= 0) continue;
      long long prevBest = 0;
      DynSnap* prevSnap = nullptr;
      for (DynSnap& s : dynSnaps_)
        if (s.p0 < p1 && s.p1 > p0 && s.k0 <= maxK && s.k1 >= minK) {
          if (s.coveredCells > prevBest) {
            prevBest = s.coveredCells;
            prevSnap = &s;
          }
        }
      bool replace = unblockedCells > prevBest;
      // If a transient change (e.g. a lazy-loaded image fading in or a hover
      // popup dismissing) stops changing across 2 consecutive placed frames at
      // full coverage and differs from the saved snapshot, replace the saved
      // snapshot with the newly settled frame (#1537).
      if (!replace && prevSnap && !prevSnap->bands.empty() && d != 0 && frameCovered == prevBest && frameCovered > 0) {
        const int bTol = bandTol_;
        const int prevW = prevSnap->k1 - prevSnap->k0 + 1;
        int agreed = 0, total = 0, snapAgreed = 0, snapTotal = 0;
        for (long long p = c0; p < c1; ++p) {
          const int y = int(p - pos);
          const int oy = y + d;
          if (oy < 0 || oy >= H) continue;
          const uint16_t* ba = cur.bandRow(y);
          const uint16_t* bb = ref.bandRow(oy);
          const uint16_t* sb = (p >= prevSnap->p0 && p < prevSnap->p1)
                                   ? &prevSnap->bands[size_t(p - prevSnap->p0) * size_t(prevW)]
                                   : nullptr;
          for (int k = minK; k <= maxK; ++k) {
            ++total;
            if (absDiff(ba[k], bb[k]) <= unsigned(bTol)) ++agreed;
            if (sb && k >= prevSnap->k0 && k <= prevSnap->k1) {
              ++snapTotal;
              if (absDiff(ba[k], sb[k - prevSnap->k0]) <= unsigned(bTol)) ++snapAgreed;
            }
          }
        }
        const bool stable = total >= 8 && agreed * 100 >= total * 95;
        const bool differsFromSnap = snapTotal >= 8 && snapAgreed * 100 < snapTotal * 95;
        if (stable && differsFromSnap) {
          prevSnap->settledCount = uint8_t(std::min(255, int(prevSnap->settledCount) + 1));
          if (prevSnap->settledCount >= 2) {
            chosenCopy = nullptr;
            w0 = c0;
            w1 = c1;
            unblockedCells = frameCovered;
            replace = true;
          }
        } else {
          prevSnap->settledCount = 0;
        }
      }
      // When replacing from a retained copy (`topCopy_` / `bottomCopy_`), also
      // include contiguous boundary rows just outside [p0, p1) that were
      // already observed changing (`dynHits >= 1` across at least half the
      // dynamic bands) before scrolling off the viewport edge, stopping at any
      // static or zero-hit row and never locking them as verified (#1537).
      long long extW0 = w0, extW1 = w1;
      if (replace && chosenCopy) {
        const auto rowIsStatic = [&](long long p) {
          if (!store_->has(p)) return true;
          const size_t at = size_t(p - store_->origin) * size_t(words);
          const uint8_t* votes = &store_->plainVotes[size_t(p - store_->origin) * size_t(C)];
          int verCells = 0, plainConfirmed = 0;
          for (int c = bandColLo; c < bandColHi; ++c) {
            if ((store_->ver[at + size_t(c >> 6)] >> (c & 63)) & 1ull) ++verCells;
            if ((votes[c] & 0x7F) >= 2) ++plainConfirmed;
          }
          return verCells * 4 >= (bandColHi - bandColLo) || plainConfirmed * 2 >= (bandColHi - bandColLo);
        };
        const auto rowDynHits = [&](long long p) {
          const uint8_t* dh = store_->dynHitsRow(p);
          int hitBands = 0;
          for (int k = minK; k <= maxK; ++k) hitBands += dh[k] > 0;
          return hitBands;
        };
        if (chosenCopy == &topCopy_ && p0 <= pos + 8 && w0 > topCopy_.pos) {
          const long long limit = std::max({topCopy_.pos, allowLo_, p0 - 64});
          for (long long p = w0 - 1; p >= limit && !rowIsStatic(p); --p) {
            const int hits = rowDynHits(p);
            if (hits == 0) break;
            if (hits * 2 >= (maxK - minK + 1)) extW0 = p;
          }
        } else if (chosenCopy == &bottomCopy_ && p1 >= pos + H - 8 && w1 < bottomCopy_.pos + H) {
          const long long limit = std::min({bottomCopy_.pos + H, allowHi_, p1 + 64});
          for (long long p = w1; p < limit && !rowIsStatic(p); ++p) {
            const int hits = rowDynHits(p);
            if (hits == 0) break;
            if (hits * 2 >= (maxK - minK + 1)) extW1 = p + 1;
          }
        }
      }
      const long long srcPos = chosenCopy ? chosenCopy->pos : pos;
      for (long long p = extW0; p < extW1; ++p) {
        if (!store_->has(p)) continue;
        const bool confirmedRow = p >= p0 && p < p1;
        const int y = int(p - srcPos);
        const int brow = std::min(y / opts_.blockRows, blockRowCount_ - 1);
        const uint64_t* blk = &blocked[size_t(brow) * size_t(words)];
        const size_t at = size_t(p - store_->origin) * size_t(words);
        const uint8_t* row = chosenCopy ? &chosenCopy->px[size_t(y) * size_t(width_) * 4] : frame.row(y);
        int spanStart = -1;
        const auto flushSpan = [&](int endC) {
          if (spanStart < 0) return;
          const int x0 = spanStart * P, x1 = std::min(width_, endC * P);
          canvas_.writeSpan(p, x0, x1, row + size_t(x0) * 4);
          spanStart = -1;
        };
        for (int c = writeLo; c < writeHi; ++c) {
          const int w = c >> 6;
          const uint64_t bit = 1ull << (c & 63);
          const size_t bi = size_t(brow) * size_t(C) + size_t(c);
          const bool cellBlocked =
              chosenCopy ? (chosenCopy->tag[bi] != chosenCopy->serial || closed_[bi] || excl_[bi] || ghost_[bi])
                         : ((blk[w] & bit) != 0);
          const bool skip = cellBlocked || (!replace && (store_->prov[at + size_t(w)] & bit) != 0);
          if (skip) {
            flushSpan(c);
            continue;
          }
          store_->prov[at + size_t(w)] |= bit;
          if (confirmedRow)
            store_->ver[at + size_t(w)] |= bit;
          else
            store_->ver[at + size_t(w)] &= ~bit;
          if (spanStart < 0) spanStart = c;
        }
        flushSpan(writeHi);
      }
      if (replace) {
        std::vector<uint16_t> snapBands;
        const bool haveCopyBands = chosenCopy && chosenCopy->bands.size() == size_t(H) * size_t(B);
        if ((!chosenCopy || haveCopyBands) && w1 > w0) {
          const int bandW = maxK - minK + 1;
          snapBands.resize(size_t(w1 - w0) * size_t(bandW));
          for (long long p = w0; p < w1; ++p) {
            const int y = int(p - srcPos);
            const uint16_t* ba = chosenCopy ? &chosenCopy->bands[size_t(y) * size_t(B)] : cur.bandRow(y);
            std::memcpy(&snapBands[size_t(p - w0) * size_t(bandW)], ba + minK, size_t(bandW) * sizeof(uint16_t));
          }
        }
        std::erase_if(dynSnaps_, [&](const DynSnap& s) {
          return s.p0 < p1 && s.p1 > p0 && s.k0 <= maxK && s.k1 >= minK;
        });
        dynSnaps_.push_back({w0, w1, minK, maxK, unblockedCells, 0, std::move(snapBands)});
      }
    }
  }
}

// Looks the frame up in the image already built, at the positions [lo, hi]
// that keep more than a sliver of overlap with the extent (peaks nearest the
// expected one first), by bands against the store, then cells against the
// canvas.
std::vector<ScrollTracker::Candidate> ScrollTracker::searchStore(const Signature& cur, long long expectedPos, long long lo,
                                                                 long long hi, size_t maxPeaks) const {
  std::vector<Candidate> out;
  const int H = cur.h;
  const int sliver = sliverRows(opts_, H);
  lo = std::max(lo, top_ - H + sliver);
  hi = std::min(hi, bottom_ - sliver);
  if (hi < lo) return out;
  const double floor = opts_.coarseFloor;
  const long long n = hi - lo + 1;
  std::vector<float> coarse(size_t(n), 0.f);
  for (long long pos = lo; pos <= hi; ++pos) coarse[size_t(pos - lo)] = float(storeCoarseShare(cur, pos, floor));
  std::vector<long long> peaks;
  for (int i : pickPeaks(coarse, floor, std::clamp(expectedPos, lo, hi) - lo)) peaks.push_back(lo + i);
  if (peaks.size() > maxPeaks) peaks.resize(maxPeaks);
  Tally t, tc;
  std::vector<uint16_t> sigs;
  std::vector<uint8_t> have;
  for (long long p : peaks) {
    // The best of the peak's neighbours by bands on every second row, then
    // the best of that one's by cells.
    long long bestPos = p;
    double bestBands = -1;
    for (long long pos = std::max(lo, p - 3); pos <= std::min(hi, p + 3); ++pos) {
      storeBandTally(cur, pos, 2, t);
      if (t.share() > bestBands) {
        bestBands = t.share();
        bestPos = pos;
      }
    }
    Candidate best;
    best.cells = -1;
    storeCellSigs(bestPos - 1, H + 2, sigs, have);
    for (long long pos = bestPos - 1; pos <= bestPos + 1; ++pos) {
      storeCellTally(cur, pos, sigs, have, bestPos - 1, tc);
      if (tc.share() <= best.cells) continue;
      storeBandTally(cur, pos, 1, t);
      Candidate c;
      c.d = int(pos - lastPos_);
      judge(c, t, tc, int(std::clamp<long long>(top_ - pos, 0, H)), int(std::clamp<long long>(bottom_ - pos, 0, H)));
      best = c;
    }
    if (best.cells < 0) continue;
    bool dup = false;
    for (const Candidate& o : out) dup |= o.d == best.d;
    if (!dup) out.push_back(best);
  }
  return out;
}

// The pinned mask learns from a step that moved: per block, cells that
// stayed where they were while textured (and are not where the page moved
// them) vote for "stayed", cells that are where the page moved them and not
// where they were vote for "moved".
void ScrollTracker::learn(const Signature& cur, const Signature& ref, int d) {
  const int H = cur.h, C = cur.cells, R = opts_.blockRows;
  if (std::abs(d) < opts_.learnMinStep) {
    // Too small a step to learn from: what the last step held back stays held
    // back (a banner's flat inside does not become the page between steps).
    return;
  }
  std::fill(stillNow_.begin(), stillNow_.end(), 0);
  ++learnedSteps_;
  std::vector<uint16_t> stay(size_t(blockRowCount_) * C, 0), stayFlat(size_t(blockRowCount_) * C, 0),
      move(size_t(blockRowCount_) * C, 0), astray(size_t(blockRowCount_) * C, 0);
  const unsigned tol = unsigned(opts_.cellTolerance);
  for (int y = 0; y < H; ++y) {
    const bool fwd = y + d >= 0 && y + d < H, back = y - d >= 0 && y - d < H;
    // Where neither frame shows where the page moved this row, a cell that
    // looks the same at the same place is a coincidence, not evidence.
    if (!fwd && !back) continue;
    const size_t base = size_t(std::min(y / R, blockRowCount_ - 1)) * C;
    const auto exclAt = [&](int yy) { return &exclLane16_[size_t(std::min(yy / R, blockRowCount_ - 1)) * C]; };
    votes(cur.cellRow(y), ref.cellRow(y), cur.cellTexRow(y), ref.cellTexRow(y), fwd ? ref.cellRow(y + d) : nullptr,
          back ? cur.cellRow(y - d) : nullptr, fwd ? exclAt(y + d) : nullptr, back ? exclAt(y - d) : nullptr, C, tol, tol / 8,
          pageCell_, &stay[base], &stayFlat[base], &move[base], &astray[base]);
  }
  bool changed = false;
  // Record textured page motion first so this step's moving text also
  // establishes confirmed page columns immediately (#1537). Requiring `move`
  // to dominate both staying and astray rows prevents random coincidences in
  // an animated sticky ad from counting as scrolling page motion.
  for (size_t i = 0; i < stay.size(); ++i) {
    if (excl_[i] || ghost_[i]) continue;
    const unsigned all = stay[i] + stayFlat[i];
    if (move[i] >= 3 && move[i] >= 3 * (all + astray[i])) pageMoved_[i] = 1;
  }
  auto isWand = [&](size_t idx) {
    return !excl_[idx] && !ghost_[idx] && !pinned_[idx] && !closed_[idx] &&
           (wandering_[idx] > 0 || (astray[idx] >= 1 && move[idx] == 0 && stay[idx] < astray[idx]));
  };
  // Confirmed page membership carried across changing in-page videos (#1537):
  // down each column, any segment between pinned/excluded/loose parts that has
  // at least two non-wandering blocks with textured motion (`pageMoved_`)
  // belongs to the scrolling page. Transient `stillNow_` does not split the
  // segment, and membership does not expire after kAreaFrames while scrolling
  // back and forth.
  std::vector<uint8_t> confirmedPage(stay.size(), 0);
  for (int c = 0; c < C; ++c)
    for (int r = 0; r < blockRowCount_;) {
      const auto stuck = [&](int rr) {
        const size_t idx = size_t(rr) * C + c;
        return pinned_[idx] || closed_[idx] || ghost_[idx] || excl_[idx];
      };
      if (stuck(r)) {
        ++r;
        continue;
      }
      int e = r;
      int movedBlocks = 0;
      for (; e < blockRowCount_ && !stuck(e); ++e) {
        const size_t idx = size_t(e) * C + c;
        movedBlocks += pageMoved_[idx] != 0 && !isWand(idx);
      }
      if (movedBlocks >= 2)
        for (int k = r; k < e; ++k) confirmedPage[size_t(k) * C + c] = 1;
      r = e;
    }
  // Detect fixed/floating animated overlays sitting over the moving page:
  // an in-page video's boundary can only remain at the same viewport block
  // row while the page stays within ~2 block rows of where that boundary first
  // appeared there, whereas a viewport-fixed animated player or bottom ad
  // keeps the same boundary block row beside cleanly moving text across
  // >= 3 block rows of net page movement (#1537).
  const long long newPos = lastPos_ + d;
  for (size_t i = 0; i < stay.size(); ++i) {
    if (excl_[i] || ghost_[i]) {
      wanderingEdge_[i] = 0;
      wanderingEdgePos_[i] = 0;
      continue;
    }
    const unsigned s = stay[i], m = move[i], x = astray[i];
    if (x >= 3 && m == 0 && s < x) {
      const int br = int(i) / C;
      const bool aboveMoving = br > 0 && astray[i - size_t(C)] == 0 &&
                               (move[i - size_t(C)] >= 3 ||
                                (br > 1 && astray[i - size_t(2 * C)] == 0 && move[i - size_t(2 * C)] >= 3));
      const bool belowMoving =
          br + 1 < blockRowCount_ && astray[i + size_t(C)] == 0 &&
          (move[i + size_t(C)] >= 3 ||
           (br + 2 < blockRowCount_ && astray[i + size_t(2 * C)] == 0 && move[i + size_t(2 * C)] >= 3));
      if (aboveMoving || belowMoving) {
        if (wanderingEdge_[i] == 0) wanderingEdgePos_[i] = lastPos_;
        wanderingEdge_[i] = uint8_t(std::min(255, int(wanderingEdge_[i]) + 1));
      } else {
        wanderingEdge_[i] = 0;
        wanderingEdgePos_[i] = 0;
      }
    } else if (m >= 3 || x == 0) {
      wanderingEdge_[i] = 0;
      wanderingEdgePos_[i] = 0;
    }
  }
  // Mark any 4-connected wandering component with >= 2 stationary-edge blocks
  // whose edge stayed fixed across >= 3 block rows of page travel as a fixed
  // overlay so it does not inherit scrolling-page membership.
  std::vector<uint8_t> fixedOverlay(stay.size(), 0);
  {
    const long long minFixedTravel = 3LL * opts_.blockRows;
    std::vector<uint8_t> seenW(stay.size(), 0);
    std::vector<size_t> comp;
    for (size_t start = 0; start < stay.size(); ++start) {
      if (seenW[start] || !isWand(start)) continue;
      seenW[start] = 1;
      comp.clear();
      comp.push_back(start);
      int fixedEdges = 0;
      for (size_t head = 0; head < comp.size(); ++head) {
        const size_t curI = comp[head];
        fixedEdges += wanderingEdge_[curI] >= 2 && std::llabs(newPos - wanderingEdgePos_[curI]) >= minFixedTravel;
        const int br = int(curI) / C, c = int(curI) % C;
        const auto visit = [&](int nbr, int nc) {
          if (nbr < 0 || nbr >= blockRowCount_ || nc < 0 || nc >= C) return;
          const size_t ni = size_t(nbr) * C + size_t(nc);
          if (seenW[ni] || !isWand(ni)) return;
          seenW[ni] = 1;
          comp.push_back(ni);
        };
        visit(br - 1, c);
        visit(br + 1, c);
        visit(br, c - 1);
        visit(br, c + 1);
      }
      if (fixedEdges >= 2)
        for (size_t idx : comp) fixedOverlay[idx] = 1;
    }
  }
  // Carry confirmed page membership across the same 4-connected wandering
  // object (a full-width in-page playing video extending past text margins),
  // without leaking into a separate sticky rail or a fixed-edge floating
  // overlay over the article (#1537).
  std::vector<size_t> wandQ;
  for (size_t i = 0; i < stay.size(); ++i)
    if (confirmedPage[i] && isWand(i) && !fixedOverlay[i]) wandQ.push_back(i);
  for (size_t head = 0; head < wandQ.size(); ++head) {
    const size_t i = wandQ[head];
    const int br = int(i) / C, c = int(i) % C;
    const auto visit = [&](int nbr, int nc) {
      if (nbr < 0 || nbr >= blockRowCount_ || nc < 0 || nc >= C) return;
      const size_t ni = size_t(nbr) * C + size_t(nc);
      if (confirmedPage[ni] || fixedOverlay[ni] || !isWand(ni)) return;
      confirmedPage[ni] = 1;
      wandQ.push_back(ni);
    };
    visit(br - 1, c);
    visit(br + 1, c);
    visit(br, c - 1);
    visit(br, c + 1);
  }
  std::vector<uint8_t> weak(stay.size(), 0);
  for (size_t i = 0; i < stay.size(); ++i) {
    if (excl_[i] || ghost_[i]) {  // the excluded rectangle and a loose part learn nothing
      stayed_[i] = wandering_[i] = wanderingPage_[i] = wanderingEdge_[i] = 0;
      wanderingMinPos_[i] = wanderingMaxPos_[i] = wanderingEdgePos_[i] = 0;
      continue;
    }
    const unsigned s = stay[i], m = move[i], x = astray[i];
    // A block that keeps changing at its own place while the page moves (an
    // animated advertisement in a column that sticks or a floating player) is
    // not the page either. Inside the confirmed scrolling page, an in-page
    // playing video scrolled slowly or back and forth stays over the same
    // viewport block: require a full frame of net page displacement span before
    // pinning it unless its edge stayed stationary against moving text (#1537).
    if (x >= 3 && m == 0 && s < x) {
      if (fixedOverlay[i]) {
        wanderingPage_[i] = 0;
      } else if (wandering_[i] == 0) {
        wanderingPage_[i] = confirmedPage[i];
      } else {
        wanderingPage_[i] |= confirmedPage[i];
      }
      if (wandering_[i] == 0) {
        wanderingMinPos_[i] = std::min(lastPos_, newPos);
        wanderingMaxPos_[i] = std::max(lastPos_, newPos);
      } else {
        wanderingMinPos_[i] = std::min({wanderingMinPos_[i], lastPos_, newPos});
        wanderingMaxPos_[i] = std::max({wanderingMaxPos_[i], lastPos_, newPos});
      }
      wandering_[i] = uint8_t(std::min(255, wandering_[i] + 1));
      const long long span = wanderingMaxPos_[i] - wanderingMinPos_[i];
      if (wandering_[i] >= 3 && (!wanderingPage_[i] || span >= height_) && !pinned_[i]) {
        pinned_[i] = 1;
        changed = true;
      }
    } else if (m >= 3) {
      wandering_[i] = wanderingPage_[i] = wanderingEdge_[i] = 0;
      wanderingMinPos_[i] = wanderingMaxPos_[i] = wanderingEdgePos_[i] = 0;
    }
    const unsigned all = s + stayFlat[i];
    if (all >= 3 && all >= 3 * m) {
      stayed_[i] = uint8_t(std::min(255, stayed_[i] + 1));
      // A block that stays only flat (the plain inside of a banner) pins
      // after twice the steps: two plain boxes of a page that meet at one
      // place by chance do so for a step or two, a banner for every step.
      const int need = s >= 3 ? opts_.pinAfterSteps : 2 * opts_.pinAfterSteps;
      if (stayed_[i] >= need && !pinned_[i]) {
        pinned_[i] = 1;
        changed = true;
      }
      if (s >= 3 && s >= 3 * m) stillNow_[i] = 1;
      else if (s >= 2 && s > m) stillNow_[i] = 1;
    } else if (m >= 3 && m >= 3 * all) {
      stayed_[i] = 0;
      if (pinned_[i]) {
        pinned_[i] = 0;
        picAt_[i] = 0;
        changed = true;
      }
    } else {
      // Stays count in consecutive moving steps: look-alike parts of a page
      // that happen to meet at one place now and then (rows only the frame
      // before can test, where that test lands on a header) never add up.
      if (!pinned_[i]) stayed_[i] = 0;
      if (s >= 2 && s > m) stillNow_[i] = 1;
    }
    // A block whose only votes are rows that stayed, however few (a header's
    // bottom border, one row under its plain inside): the page is always
    // where it moved, so it stood still in this pair, where it is the edge
    // of something pinned or still (below). Held back as it is, not grown
    // into a region.
    if (!stillNow_[i] && m == 0 && x == 0 && all >= 1) weak[i] = 1;
  }
  if (changed) closeMask();
  for (size_t i = 0; i < weak.size(); ++i) {
    if (!weak[i]) continue;
    const int br = int(i) / C;
    const bool above = br > 0 && (closed_[i - size_t(C)] || stillNow_[i - size_t(C)]);
    const bool below = br + 1 < blockRowCount_ && (closed_[i + size_t(C)] || stillNow_[i + size_t(C)]);
    if (!above && !below) weak[i] = 0;
  }
  // The flat inside of something that stood still or is pinned (a cookie
  // banner's plain colour beside its text): a flat cell looks the same at
  // its place and where the page moved it, so no vote can tell it from the
  // page's background, and two frames always agree on it. From each region
  // that stood still or is pinned, blocks of its own background colour (the
  // most common flat cell in it) that are flat in both frames, strictly
  // where they were and not the page's colour are held back with it; a flat
  // part of the page beside a side bar has another colour and stays.
  {
    const int Rb = blockRowCount_, strict = int(tol / 8);
    // The page's colour: the first screen's most common one, and any colour
    // behind the text that moved with the page in this pair (an eighth of
    // it or more). A first screen can be mostly something else: a footer
    // where a capture starts at the end, a picture at the top. Counted when
    // first needed.
    std::array<uint8_t, 256> pageBin{};
    const int reach = int(tol / 64);
    {
      std::array<int, 256> behind{};
      int behindAll = 0;
      for (int br = 0; br < Rb; ++br)
        for (int c = 0; c < C; ++c) {
          const size_t i = size_t(br) * C + c;
          if (move[i] < 3 || move[i] < 3u * (stay[i] + stayFlat[i])) continue;
          for (int y = br * R; y < std::min(H, (br + 1) * R); y += 2)  // every other row is enough
            if (!cur.cellTexRow(y)[c]) {
              ++behind[size_t(std::min(255, cur.cellRow(y)[c] / 64))];
              ++behindAll;
            }
        }
      for (int b = 0; b < 256; ++b) {
        int n = 0;
        for (int k = std::max(0, b - reach); k <= std::min(255, b + reach); ++k) n += behind[size_t(k)];
        pageBin[size_t(b)] = behindAll > 0 && 8 * n >= behindAll;
        behindSeen_[size_t(b)] += uint64_t(behind[size_t(b)]);
      }
    }
    const auto pageLike = [&](int colour) {
      const int bin = std::min(255, colour / 64);
      if (absDiff(uint16_t(bin * 64 + 32), pageCell_) <= tol) return true;
      return pageBin[size_t(bin)] != 0;
    };
    // The page's own background so far (#1538): the colour most often behind
    // the text that moved with the page, over every pair learned from.
    int pageBg = -1;
    {
      uint64_t best = 0;
      for (int b = 0; b < 256; ++b) {
        uint64_t n = 0;
        for (int k = std::max(0, b - reach); k <= std::min(255, b + reach); ++k) n += behindSeen_[size_t(k)];
        if (n > best) {
          best = n;
          pageBg = b;
        }
      }
      pageBgBin_ = pageBg;
    }
    const auto pageBackground = [&](int colour) {
      const int bin = std::min(255, colour / 64);
      return absDiff(uint16_t(bin * 64 + 32), pageCell_) <= tol || (pageBg >= 0 && std::abs(bin - pageBg) <= reach);
    };
    // A block's flat colour: every cell of its rows one colour in both frames,
    // strictly where it was, not the first screen's commonest;
    // else -1 (flatRaw). flatAt: and no colour of the page's in this pair
    // either. Found when first asked (the flood looks only around what stood
    // still).
    std::vector<int> flatColour(stillNow_.size(), -2);
    const auto flatRaw = [&](size_t i) {
      int& out = flatColour[i];
      if (out != -2) return out;
      out = -1;
      const int br = int(i) / C, c = int(i) % C;
      if (excl_[i]) return out;
      int colour = -1;
      bool ok = true;
      // One colour down all its rows, in both frames: also the first block
      // of a banner, whose top row differs from the page above it (#1538).
      for (int y = br * R; ok && y < std::min(H, (br + 1) * R); ++y) {
        const uint16_t a = cur.cellRow(y)[c], b = ref.cellRow(y)[c];
        ok = int(absDiff(a, b)) <= strict && (colour < 0 || int(absDiff(a, uint16_t(colour))) <= strict);
        if (colour < 0) colour = a;
      }
      if (ok && absDiff(uint16_t(colour), pageCell_) > tol) out = colour;
      return out;
    };
    const auto flatAt = [&](size_t i) {
      const int colour = flatRaw(i);
      return colour >= 0 && !pageLike(colour) ? colour : -1;
    };

    std::vector<uint8_t> flooded(stillNow_.size(), 0);
    std::vector<int> floodId(stillNow_.size(), -1);
    int regions = 0;
    std::vector<uint8_t> seed(stillNow_.size(), 0), seen(stillNow_.size(), 0);
    for (size_t i = 0; i < seed.size(); ++i) seed[i] = stillNow_[i] || closed_[i];
    std::vector<int> part, stack;
    std::array<int, 256> count{};
    for (size_t s0 = 0; s0 < seed.size(); ++s0) {
      if (!seed[s0] || seen[s0]) continue;
      // One region that stood still or is pinned, and its background colour.
      part.clear();
      count.fill(0);
      stack.assign(1, int(s0));
      seen[s0] = 1;
      while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        part.push_back(i);
        const int br = i / C, c = i % C;
        for (int y = br * R; y < std::min(H, (br + 1) * R); y += 2)  // every other row is enough
          if (!cur.cellTexRow(y)[c]) ++count[size_t(std::min(255, cur.cellRow(y)[c] / 64))];
        const int nb[4] = {br > 0 ? i - C : -1, br + 1 < Rb ? i + C : -1, c > 0 ? i - 1 : -1, c + 1 < C ? i + 1 : -1};
        for (int k : nb)
          if (k >= 0 && seed[size_t(k)] && !seen[size_t(k)]) {
            seen[size_t(k)] = 1;
            stack.push_back(k);
          }
      }
      int bg = -1, best = 0;
      for (int v = 0; v < 256; ++v)
        if (count[size_t(v)] > best) {
          best = count[size_t(v)];
          bg = v * 64 + 32;
        }
      // A pinned part's own plain colour (#1538: a cookie banner's sides out
      // to the frame's edges over a page's empty margins) is held back with
      // it unless it is the page's own background so far, also where
      // something of that colour moves with the page in this pair (a dark
      // photo, an advertisement in the banner's colour).
      if (bg < 0 || pageBackground(bg)) continue;
      const auto flatOf = [&](size_t i) {
        const int colour = flatRaw(i);
        return colour >= 0 && !pageBackground(colour) ? colour : -1;
      };
      const int id = regions++;
      // Its own blocks of that colour are where its edge is looked for.
      for (int i : part)
        if (flatOf(size_t(i)) >= 0 && absDiff(uint16_t(flatOf(size_t(i))), uint16_t(bg)) <= tol) {
          flooded[size_t(i)] = 1;
          floodId[size_t(i)] = id;
        }
      // Its flat blocks of that colour, joined to it.
      stack = part;
      while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        const int br = i / C, c = i % C;
        const int nb[4] = {br > 0 ? i - C : -1, br + 1 < Rb ? i + C : -1, c > 0 ? i - 1 : -1, c + 1 < C ? i + 1 : -1};
        for (int k : nb) {
          if (k < 0 || seed[size_t(k)] || stillNow_[size_t(k)]) continue;
          const int fc = flatOf(size_t(k));
          if (fc < 0 || absDiff(uint16_t(fc), uint16_t(bg)) > tol) continue;
          stillNow_[size_t(k)] = 1;
          flooded[size_t(k)] = 1;
          floodId[size_t(k)] = id;
          stack.push_back(k);
        }
      }
    }
    // And its edge: a flat block beside a block of its background, of
    // another colour (the banner's border or shadow, half in a cell), one
    // block deep. Not beside a side bar's own edge: the page next to it is
    // the page.
    for (size_t i = 0; i < flooded.size(); ++i) {
      if (!flooded[i]) continue;
      const int br = int(i) / C, c = int(i) % C;
      const int nb[4] = {br > 0 ? int(i) - C : -1, br + 1 < Rb ? int(i) + C : -1, c > 0 ? int(i) - 1 : -1, c + 1 < C ? int(i) + 1 : -1};
      for (int k : nb)
        if (k >= 0 && !stillNow_[size_t(k)] && flatAt(size_t(k)) >= 0) stillNow_[size_t(k)] = 1;
    }
    // And what its flat inside surrounds (a white button on a dark banner,
    // of the page's own colour, which nothing votes on): what lies between
    // two of its blocks in a block row, up to 32 cells, or in a column, up
    // to 8 blocks.
    if (regions > 0) {
      for (int br = 0; br < Rb; ++br) {
        int lastC = -1, lastId = -1;
        for (int c = 0; c < C; ++c) {
          const int id = floodId[size_t(br) * C + c];
          if (id < 0) continue;
          if (id == lastId && c - lastC > 1 && c - lastC - 1 <= 32)
            for (int k = lastC + 1; k < c; ++k) stillNow_[size_t(br) * C + k] = 1;
          lastC = c;
          lastId = id;
        }
      }
      for (int c = 0; c < C; ++c) {
        int lastR = -1, lastId = -1;
        for (int br = 0; br < Rb; ++br) {
          const int id = floodId[size_t(br) * C + c];
          if (id < 0) continue;
          if (id == lastId && br - lastR > 1 && br - lastR - 1 <= 8)
            for (int k = lastR + 1; k < br; ++k) stillNow_[size_t(k) * C + c] = 1;
          lastR = br;
          lastId = id;
        }
      }
    }
  }
  // What a frame offers the image: not what is pinned, nor what stood still
  // in this pair, its short gaps in a column closed (a side bar that has
  // just stuck holds back the flat rows between its items too) and widened
  // by a block. A step over half the frame leaves rows in its middle that
  // neither frame shows twice, so nothing there can vote: the pair's still
  // parts are closed as pinned parts are, across those rows. Then the blocks
  // that stood still by a row or two (above), as they are: closed or widened,
  // a few such blocks of a side bar's items would hold back the page between
  // them. A pinned part that ends one cell short of the frame's edge holds
  // that cell back too: a banner's border down its edge looks the same
  // wherever the page moved it, so nothing votes on it, and two frames always
  // agree on it.
  // A pinned bar across a third of the frame or more and a third of its
  // height or less (a banner at the bottom, a header) offers the image
  // nothing in its whole rectangle (#1538): a banner's border down its side
  // looks the same at every row of it, so no pair of frames shows it standing
  // still, and it was in every row of the image. (It still votes: steps over
  // half the frame need every lane of evidence.)
  // Measured twice: by its pinned blocks alone, and by them with what stood
  // still joined to them (a header whose pinned part is its title, with its
  // bottom line across the whole width that has only stood still so far:
  // the plain white beside the title is the header's too).
  const auto bars = [&] {
    std::vector<uint8_t> joined(blocked_.size());
    for (size_t i = 0; i < joined.size(); ++i) joined[i] = blocked_[i] | closed_[i];
    // A line across the frame learned with gaps (where the page beside it
    // happened to look the same) is one line: gaps of up to 32 cells in a
    // block row are closed, as a banner's flat inside is.
    for (int r = 0; r < blockRowCount_; ++r) {
      uint8_t* row = &joined[size_t(r) * C];
      for (int c = 0, last = -1; c < C; ++c) {
        if (!row[c]) continue;
        if (last >= 0 && c - last > 1 && c - last - 1 <= 32) std::fill(row + last + 1, row + c, uint8_t(1));
        last = c;
      }
    }
    std::vector<uint8_t> hold(blocked_.size(), 0);
    std::vector<int> part(closed_.size());
    std::vector<int> stack;
    for (const std::vector<uint8_t>* in : {&closed_, &joined}) {
      std::fill(part.begin(), part.end(), -1);
      for (int i = 0; i < blockRowCount_ * C; ++i) {
        if (!closed_[size_t(i)] || part[size_t(i)] >= 0) continue;
        int r0 = blockRowCount_, r1 = -1, c0 = C, c1 = -1;
        part[size_t(i)] = i;
        stack.assign(1, i);
        while (!stack.empty()) {
          const int j = stack.back();
          stack.pop_back();
          const int r = j / C, c = j % C;
          r0 = std::min(r0, r);
          r1 = std::max(r1, r);
          c0 = std::min(c0, c);
          c1 = std::max(c1, c);
          const int nb[4] = {r > 0 ? j - C : -1, r + 1 < blockRowCount_ ? j + C : -1, c > 0 ? j - 1 : -1, c + 1 < C ? j + 1 : -1};
          for (int k : nb)
            if (k >= 0 && (*in)[size_t(k)] && part[size_t(k)] < 0) {
              part[size_t(k)] = i;
              stack.push_back(k);
            }
        }
        if ((c1 - c0 + 1) * 3 < C || (r1 - r0 + 1) * 3 > blockRowCount_) continue;
        for (int r = r0; r <= r1; ++r)
          for (int c = c0; c <= c1; ++c) hold[size_t(r) * C + c] = 1;
      }
    }
    for (size_t i = 0; i < hold.size(); ++i) blocked_[i] |= hold[i];
  };
  const auto lastly = [&] {
    bars();
    for (size_t i = 0; i < weak.size(); ++i) blocked_[i] |= weak[i];
    if (C < 3) return;
    for (int r = 0; r < blockRowCount_; ++r) {
      uint8_t* o = &blocked_[size_t(r) * C];
      const uint8_t* p = &closed_[size_t(r) * C];
      o[0] |= p[1];
      o[C - 1] |= p[C - 2];
    }
  };
  if (2 * std::abs(d) > H) {
    std::vector<uint8_t> either(pinned_.size());
    for (size_t i = 0; i < either.size(); ++i) either[i] = pinned_[i] | stillNow_[i];
    close(either, blocked_, true);
    lastly();
    return;
  }
  const int Rb = blockRowCount_;
  std::vector<uint8_t> still = stillNow_;
  for (int c = 0; c < C; ++c) {
    int last = -1;
    for (int r = 0; r < Rb; ++r) {
      if (!stillNow_[size_t(r) * C + c]) continue;
      if (last >= 0 && r - last > 1 && r - last - 1 <= 6)
        for (int k = last + 1; k < r; ++k) still[size_t(k) * C + c] = 1;
      last = r;
    }
  }
  std::vector<uint8_t> wide(still.size(), 0);
  for (int r = 0; r < Rb; ++r) {
    const uint8_t* in = &still[size_t(r) * C];
    uint8_t* o = &wide[size_t(r) * C];
    o[0] = in[0] | (C > 1 ? in[1] : 0);
    for (int c = 1; c + 1 < C; ++c) o[c] = in[c - 1] | in[c] | in[c + 1];
    if (C > 1) o[C - 1] = in[C - 2] | in[C - 1];
  }
  blocked_ = closed_;
  for (int r = 0; r < Rb; ++r) {
    uint8_t* o = &blocked_[size_t(r) * C];
    const uint8_t* mid = &wide[size_t(r) * C];
    const uint8_t* up = r > 0 ? mid - C : mid;
    const uint8_t* dn = r + 1 < Rb ? mid + C : mid;
    for (int c = 0; c < C; ++c) o[c] |= up[c] | mid[c] | dn[c];
  }
  lastly();
}

// The closed mask: pinned blocks, plus the holes of a pinned part (the flat
// inside of a box has nothing that could vote) up to closeFraction of the
// frame, within one connected part.
void ScrollTracker::close(const std::vector<uint8_t>& src, std::vector<uint8_t>& out, bool lists) const {
  const int C = cells_, R = blockRowCount_;
  out = src;
  std::vector<int> label(size_t(R) * C, -1);
  int next = 0;
  std::vector<int> stack;
  for (int i = 0; i < R * C; ++i) {
    if (!src[size_t(i)] || label[size_t(i)] >= 0) continue;
    label[size_t(i)] = next;
    stack.push_back(i);
    while (!stack.empty()) {
      const int j = stack.back();
      stack.pop_back();
      // One part: pinned blocks up to three blocks apart (the lines of a
      // side bar's text, with the flat rows between them).
      const int r = j / C, c = j % C;
      for (int rr = std::max(0, r - 3); rr <= std::min(R - 1, r + 3); ++rr)
        for (int cc = std::max(0, c - 3); cc <= std::min(C - 1, c + 3); ++cc) {
          const int k = rr * C + cc;
          if (src[size_t(k)] && label[size_t(k)] < 0) {
            label[size_t(k)] = next;
            stack.push_back(k);
          }
        }
    }
    ++next;
  }
  const int vGap = std::max(1, int(opts_.closeFraction * R));
  const int hGap = std::max(1, int(opts_.closeWidthFraction * C));
  // With `lists`, for a pair whose step is over half the frame (#1538): down
  // a column, the parts of one side bar's list are one thing however far
  // apart (narrow parts, under a third of the width, sharing columns, with
  // their left or right edges aligned). Such a step leaves the list's middle
  // rows in one frame only, never learned, between the rows learned above
  // and below them.
  std::vector<int> column(static_cast<size_t>(next));
  std::iota(column.begin(), column.end(), 0);
  if (lists) {
    std::vector<int> c0(static_cast<size_t>(next), C), c1(static_cast<size_t>(next), -1);
    for (int i = 0; i < R * C; ++i) {
      const int l = label[size_t(i)];
      if (l < 0) continue;
      c0[size_t(l)] = std::min(c0[size_t(l)], i % C);
      c1[size_t(l)] = std::max(c1[size_t(l)], i % C);
    }
    const auto find = [&](int i) {
      while (column[size_t(i)] != i) i = column[size_t(i)] = column[size_t(column[size_t(i)])];
      return i;
    };
    for (int a = 0; a < next; ++a)
      for (int b = a + 1; b < next; ++b) {
        const int wa = c1[size_t(a)] - c0[size_t(a)] + 1, wb = c1[size_t(b)] - c0[size_t(b)] + 1;
        const int overlap = std::min(c1[size_t(a)], c1[size_t(b)]) - std::max(c0[size_t(a)], c0[size_t(b)]) + 1;
        if (std::max(wa, wb) * 3 < C && overlap * 2 >= std::min(wa, wb) &&
            (std::abs(c0[size_t(a)] - c0[size_t(b)]) <= 2 || std::abs(c1[size_t(a)] - c1[size_t(b)]) <= 2))
          column[size_t(find(a))] = find(b);
      }
    for (int l = 0; l < next; ++l) column[size_t(l)] = find(l);
  }
  for (int c = 0; c < C; ++c) {
    int lastR = -1, lastL = -1;
    for (int r = 0; r < R; ++r) {
      const int l0 = label[size_t(r) * C + c];
      if (l0 < 0) continue;
      const int l = column[size_t(l0)];
      if (lastL == l && r - lastR > 1 && r - lastR - 1 <= vGap)
        for (int k = lastR + 1; k < r; ++k) out[size_t(k) * C + c] = 1;
      lastR = r;
      lastL = l;
    }
  }
  // Such a list's rows that no part of it holds, between rows that do: across
  // its whole width (an entry in those rows can be longer than any learned).
  if (lists) {
    std::vector<int> c0(static_cast<size_t>(next), C), c1(static_cast<size_t>(next), -1), parts(static_cast<size_t>(next), 0);
    std::vector<int> seenLabel(static_cast<size_t>(next), 0);
    for (int i = 0; i < R * C; ++i) {
      const int l0 = label[size_t(i)];
      if (l0 < 0) continue;
      const size_t g = size_t(column[size_t(l0)]);
      c0[g] = std::min(c0[g], i % C);
      c1[g] = std::max(c1[g], i % C);
      if (!seenLabel[size_t(l0)]) {
        seenLabel[size_t(l0)] = 1;
        ++parts[g];
      }
    }
    std::vector<uint8_t> holds(static_cast<size_t>(R));
    for (int g = 0; g < next; ++g) {
      if (parts[size_t(g)] < 2) continue;
      for (int r = 0; r < R; ++r) {
        holds[size_t(r)] = 0;
        for (int c = c0[size_t(g)]; c <= c1[size_t(g)] && !holds[size_t(r)]; ++c) {
          const int l0 = label[size_t(r) * C + c];
          holds[size_t(r)] = l0 >= 0 && column[size_t(l0)] == g;
        }
      }
      int last = -1;
      for (int r = 0; r < R; ++r) {
        if (!holds[size_t(r)]) continue;
        if (last >= 0 && r - last > 1 && r - last - 1 <= vGap)
          for (int k = last + 1; k < r; ++k)
            for (int c = c0[size_t(g)]; c <= c1[size_t(g)]; ++c) out[size_t(k) * C + c] = 1;
        last = r;
      }
    }
  }
  for (int r = 0; r < R; ++r) {
    int lastC = -1, lastL = -1;
    for (int c = 0; c < C; ++c) {
      const int l = label[size_t(r) * C + c];
      if (l < 0) continue;
      if (lastL == l && c - lastC > 1 && c - lastC - 1 <= hGap)
        for (int k = lastC + 1; k < c; ++k) out[size_t(r) * C + k] = 1;
      lastC = c;
      lastL = l;
    }
  }
  // One block around: the edges of a pinned part (the vertical border of a
  // box has no texture to vote with) are not the page's either.
  {
    const std::vector<uint8_t> core = out;
    for (int r = 0; r < R; ++r)
      for (int c = 0; c < C; ++c) {
        if (core[size_t(r) * C + c]) continue;
        bool near = false;
        for (int dr = -1; dr <= 1 && !near; ++dr)
          for (int dc = -1; dc <= 1 && !near; ++dc) {
            const int rr = r + dr, cc = c + dc;
            near = rr >= 0 && rr < R && cc >= 0 && cc < C && core[size_t(rr) * C + cc];
          }
        if (near) out[size_t(r) * C + c] = 1;
      }
  }
}

void ScrollTracker::closeMask() {
  close(pinned_, closed_);
  for (size_t i = 0; i < closed_.size() && i < excl_.size(); ++i)
    if (excl_[i]) closed_[i] = 0;
  refreshLanes();
}

namespace {
// Following a loose part (#1538): it is where it was pinned while this share
// of its textured cells shows what they showed there; it has moved to an
// offset where this share does, over at least kLooseCells cells; a few rows
// more than the page moved are looked through; below kPictureKeep of it in
// place, its picture is not refreshed (something changed in it, or covers it).
constexpr double kInPlace = 0.80;
constexpr double kLooseShare = 0.85;
constexpr int kLooseCells = 48;
constexpr int kLooseSlack = 8;
constexpr double kPictureKeep = 0.60;
// Pictures of parts that came loose kept for finish(): a page that lets go of
// its columns again and again keeps the latest.
constexpr size_t kKeptPictures = 16;
// Two plain cells are one colour within this many levels of grey: a plain
// cell is drawn the same in every frame, and a shade a few levels off the
// page's (a toolbar's background, a banner's border running through a
// margin's cell) is another colour, which loses the vote to the page's own.
constexpr int kPlainSame = 2;
// A move of the whole view goes on for at most this many placed frames that
// keep moving its way.
constexpr int kViewRunFrames = 8;
// A block of the screen is the page's area for this many placed frames after
// a pair showed the page in it (the page's last rows, seen in the last frame
// only, are written where the frames before showed the page).
constexpr uint32_t kAreaFrames = 8;
int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
}  // namespace

// Groups of the mask's connected parts that are one pinned thing: parts that
// share columns, one close above the other, or a side bar's list items with
// their edges aligned (as finish() groups them), both narrow or both wide. Returns the number of groups;
// groupOf is -1 off the mask.
int ScrollTracker::pinnedGroups(const std::vector<uint8_t>& mask, std::vector<int>& groupOf) const {
  const int C = cells_, R = blockRowCount_;
  groupOf.assign(size_t(R) * C, -1);
  struct Box {
    int r0 = 1 << 30, r1 = -1, c0 = 1 << 30, c1 = -1;
  };
  std::vector<Box> parts;
  std::vector<int> stack;
  for (int i = 0; i < R * C; ++i) {
    if (!mask[size_t(i)] || groupOf[size_t(i)] >= 0) continue;
    const int id = int(parts.size());
    parts.push_back({});
    groupOf[size_t(i)] = id;
    stack.assign(1, i);
    while (!stack.empty()) {
      const int j = stack.back();
      stack.pop_back();
      const int r = j / C, c = j % C;
      Box& b = parts.back();
      b.r0 = std::min(b.r0, r);
      b.r1 = std::max(b.r1, r);
      b.c0 = std::min(b.c0, c);
      b.c1 = std::max(b.c1, c);
      const int nb[4] = {r > 0 ? j - C : -1, r + 1 < R ? j + C : -1, c > 0 ? j - 1 : -1, c + 1 < C ? j + 1 : -1};
      for (int k : nb)
        if (k >= 0 && mask[size_t(k)] && groupOf[size_t(k)] < 0) {
          groupOf[size_t(k)] = id;
          stack.push_back(k);
        }
    }
  }
  // A part that fills far more of one block row than of the next is two
  // things that touch (a sticky header across the frame over the contents
  // and tools columns beside the page): from the row where its blocks double
  // or halve, by at least 16, the rows are a part of their own; and its
  // pieces that only the header joined are parts of their own too.
  {
    const size_t n = parts.size();
    std::vector<int> piece(groupOf.size(), -1);
    int pieces = 0;
    for (size_t i = 0; i < n; ++i) {
      int id = pieces++, prev = -1;
      for (int r = parts[i].r0; r <= parts[i].r1; ++r) {
        int count = 0;
        for (int c = parts[i].c0; c <= parts[i].c1; ++c) count += groupOf[size_t(r) * C + c] == int(i);
        if (count == 0) {
          prev = -1;
          continue;
        }
        const int more = std::max(count, prev), fewer = std::min(count, prev);
        if (prev > 0 && more >= 2 * fewer && more - fewer >= 16) id = pieces++;
        prev = count;
        for (int c = parts[i].c0; c <= parts[i].c1; ++c)
          if (groupOf[size_t(r) * C + c] == int(i)) piece[size_t(r) * C + c] = id;
      }
    }
    if (pieces > int(n)) {
      // Connected within a piece.
      parts.clear();
      std::fill(groupOf.begin(), groupOf.end(), -1);
      for (int i = 0; i < R * C; ++i) {
        if (piece[size_t(i)] < 0 || groupOf[size_t(i)] >= 0) continue;
        const int id = int(parts.size());
        parts.push_back({});
        groupOf[size_t(i)] = id;
        stack.assign(1, i);
        while (!stack.empty()) {
          const int j = stack.back();
          stack.pop_back();
          const int r = j / C, c = j % C;
          Box& b = parts.back();
          b.r0 = std::min(b.r0, r);
          b.r1 = std::max(b.r1, r);
          b.c0 = std::min(b.c0, c);
          b.c1 = std::max(b.c1, c);
          const int nb[4] = {r > 0 ? j - C : -1, r + 1 < R ? j + C : -1, c > 0 ? j - 1 : -1, c + 1 < C ? j + 1 : -1};
          for (int k : nb)
            if (k >= 0 && piece[size_t(k)] == piece[size_t(j)] && groupOf[size_t(k)] < 0) {
              groupOf[size_t(k)] = id;
              stack.push_back(k);
            }
        }
      }
    }
  }
  std::vector<int> up(parts.size());
  std::iota(up.begin(), up.end(), 0);
  const auto find = [&](int i) {
    while (up[size_t(i)] != i) i = up[size_t(i)] = up[size_t(up[size_t(i)])];
    return i;
  };
  for (size_t a = 0; a < parts.size(); ++a)
    for (size_t b = a + 1; b < parts.size(); ++b) {
      const Box& A = parts[a];
      const Box& B = parts[b];
      const int overlap = std::min(A.c1, B.c1) - std::max(A.c0, B.c0) + 1;
      const int narrower = std::min(A.c1 - A.c0, B.c1 - B.c0) + 1;
      const int gap = std::max(A.r0, B.r0) - std::min(A.r1, B.r1) - 1;
      const int wa = A.c1 - A.c0 + 1, wb = B.c1 - B.c0 + 1;
      const bool list = std::max(wa, wb) * 3 < C && (std::abs(A.c0 - B.c0) <= 2 || std::abs(A.c1 - B.c1) <= 2);
      // Never a bar across the frame with a column beside the page (the
      // browser's toolbar or a banner and a contents column): they move apart.
      const bool kind = (wa * 3 < C) == (wb * 3 < C);
      if (!kind || overlap * 2 < narrower || (gap > R / 4 && !list)) continue;
      // Nor across a bar that lies between them (MDN: the header's own
      // buttons above its toolbar across the frame, and the contents column
      // below it): the rows between them are not a gap, they are the bar's.
      bool across = false;
      if (gap > 0)
        for (size_t x = 0; x < parts.size() && !across; ++x) {
          const Box& X = parts[x];
          if (x == a || x == b || (X.c1 - X.c0 + 1) * 3 < C) continue;
          across = X.r1 >= std::min(A.r1, B.r1) + 1 && X.r0 <= std::max(A.r0, B.r0) - 1 && X.c1 >= std::max(A.c0, B.c0) &&
                   X.c0 <= std::min(A.c1, B.c1);
        }
      if (!across) up[size_t(find(int(a)))] = find(int(b));
    }
  std::vector<int> number(parts.size(), -1);
  int groups = 0;
  for (size_t i = 0; i < parts.size(); ++i)
    if (number[size_t(find(int(i)))] < 0) number[size_t(find(int(i)))] = groups++;
  for (int& g : groupOf)
    if (g >= 0) g = number[size_t(find(g))];
  return groups;
}

// The share of a loose part's textured cells that the frame shows the same,
// `s` rows below where it was pinned; those that lie outside the frame, or
// behind another part that stays on the screen (a contents column sliding up
// under the sticky header as the page's end carries it off), are not
// compared. Gives up (0) once it cannot reach `floor`.
double ScrollTracker::looseShare(const Loose& part, const Signature& cur, int s, double floor, int* compared) const {
  const unsigned tol = unsigned(opts_.cellTolerance);
  const size_t n = part.values.size();
  const long long allowed = (long long)((1.0 - floor) * double(n));
  long long agree = 0, miss = 0;
  int seen = 0;
  for (size_t k = 0; k < n; ++k) {
    const int y = part.ys[k] + s;
    if (y < 0 || y >= height_ || hiddenAt(part, y, part.cs[k])) continue;
    ++seen;
    if (absDiff(cur.cellRow(y)[part.cs[k]], part.values[k]) <= tol) {
      ++agree;
    } else if (++miss > allowed) {
      if (compared) *compared = seen;
      return 0;
    }
  }
  if (compared) *compared = seen;
  return seen ? double(agree) / seen : 0;
}

// Whether frame row y, cell c is a spot where the screen cannot show `part`:
// not its own place (a column's rectangle, a bar's blocks), not where a loose
// part lay in the last placed frame, and not the page's area there (what
// stays on the screen: a header and its plain top margin, a bar).
bool ScrollTracker::hiddenAt(const Loose& part, int y, int c) const {
  const int C = cells_, br = std::min(y / opts_.blockRows, blockRowCount_ - 1);
  const size_t i = size_t(br) * C + c;
  if (followArea_.empty()) return false;
  if ((followArea_[size_t(br) * store_->words + size_t(c >> 6)] >> (c & 63)) & 1) return false;
  if (i < ghostPrev_.size() && ghostPrev_[i]) return false;
  if (br < part.r0 || br > part.r1 || c < part.c0 || c > part.c1) return true;
  const int w = part.c1 - part.c0 + 1;
  if (w * 3 < C) return false;
  return !part.blocks[size_t(br - part.r0) * size_t(w) + size_t(c - part.c0)];
}

// The offset in [lo, hi] at which the frame shows a loose part, the best by
// share, at least kLooseShare over kLooseCells cells and minEvidenceRows rows
// of it; INT_MIN where there is none.
int ScrollTracker::findLoose(const Loose& part, const Signature& cur, int prefer, int lo, int hi) const {
  lo = std::max(lo, -height_ + 1);
  hi = std::min(hi, height_ - 1);
  int best = INT_MIN;
  double bestShare = 0;
  for (int s = lo; s <= hi; ++s) {
    int compared = 0;
    const double share = looseShare(part, cur, s, kLooseShare, &compared);
    // An equal share (a list of equal items a period apart) goes to the
    // offset nearest the expected one, not to the order of the search.
    if (share < kLooseShare || compared < kLooseCells || share < bestShare ||
        (share == bestShare && (best == INT_MIN || std::abs(s - prefer) >= std::abs(best - prefer))))
      continue;
    // Rows of it in the frame: enough of the part to tell its place (its
    // samples go cell by cell, so a row is counted once however many cells
    // of it there are).
    int rows = 0;
    std::vector<uint8_t> seen(size_t(height_), 0);
    for (size_t k = 0; k < part.ys.size(); ++k) {
      const int y = part.ys[k] + s;
      if (y < 0 || y >= height_ || seen[size_t(y)] || hiddenAt(part, y, part.cs[k])) continue;
      seen[size_t(y)] = 1;
      ++rows;
    }
    if (rows < opts_.minEvidenceRows) continue;
    best = s;
    bestShare = share;
  }
  return best;
}

// The blocks a loose part lies over in the frame, now: a narrow one (a side
// bar's list, under a third of the width) its whole rectangle, also the rows
// between its learned items that steps over half the frame never showed twice.
void ScrollTracker::markLoose(const Loose& part) {
  const int C = cells_, R = opts_.blockRows, Rb = blockRowCount_;
  const int w = part.c1 - part.c0 + 1;
  const bool column = w * 3 < C;
  for (int br = part.r0; br <= part.r1; ++br)
    for (int c = part.c0; c <= part.c1; ++c) {
      if (!column && !part.blocks[size_t(br - part.r0) * w + (c - part.c0)]) continue;
      const int y0 = br * R + part.shift, y1 = std::min(br * R + R, height_) - 1 + part.shift;
      if (y1 < 0 || y0 >= height_) continue;
      for (int b = std::max(0, floorDiv(y0, R)); b <= std::min(Rb - 1, floorDiv(y1, R)); ++b) ghost_[size_t(b) * C + c] = 1;
    }
}

// Before a placed frame is learned from and composed: the pinned parts that
// came loose since the last placed frame, and where the loose ones are now.
void ScrollTracker::followPinned(const Signature& cur, const Signature& ref, int d) {
  const int C = cells_, R = opts_.blockRows, H = height_;
  // Where a part can be seen (hiddenAt): the page's area and where the loose
  // parts lay, as the last placed frame left them.
  pageArea(followArea_);
  ghostPrev_ = ghost_;
  const std::vector<uint8_t>& ghostBefore = ghostPrev_;
  std::fill(ghost_.begin(), ghost_.end(), 0);
  viewMoved_ = false;
  bool maskChanged = false;
  // The loose parts: they move with the page at most, from where they were.
  for (size_t k = 0; k < loose_.size();) {
    Loose& part = loose_[k];
    const int s = findLoose(part, cur, part.shift - d, std::min(part.shift, part.shift - d) - kLooseSlack,
                            std::max(part.shift, part.shift - d) + kLooseSlack);
    const bool topSticky = part.r0 * opts_.blockRows * 2 < H;
    if (s == INT_MIN || (s != 0 && (topSticky ? s > 0 : s < 0))) {  // gone, no longer the same, or back past its pinned place
      loose_.erase(loose_.begin() + std::ptrdiff_t(k));
      continue;
    }
    if (s == 0) {  // stopped where it was pinned: pinned again
      const int w = part.c1 - part.c0 + 1;
      for (int br = part.r0; br <= part.r1; ++br)
        for (int c = part.c0; c <= part.c1; ++c) {
          const size_t i = size_t(br) * C + c;
          if (!part.blocks[size_t(br - part.r0) * w + (c - part.c0)] || excl_[i]) continue;
          pinned_[i] = 1;
          stayed_[i] = uint8_t(opts_.pinAfterSteps);
        }
      maskChanged = true;
      loose_.erase(loose_.begin() + std::ptrdiff_t(k));
      continue;
    }
    part.shift = s;
    markLoose(part);
    ++k;
  }
  // The pinned parts: still where they were pinned, or come loose. Their
  // pictures are of the blocks that are pinned themselves (not the holes and
  // the edge the closed mask adds: the page beside a part moves on), and a
  // part comes loose only from where the last placed frame showed it.
  bool anyClosed = false;
  for (uint8_t v : closed_) anyClosed |= v != 0;
  if (anyClosed) {
    const uint32_t last = frameSerial_, now = frameSerial_ + 1;
    std::vector<int> groupOf;
    const int groups = pinnedGroups(closed_, groupOf);
    std::vector<Loose> found(static_cast<size_t>(groups));
    for (Loose& g : found) {
      g.r0 = g.c0 = 1 << 30;
      g.r1 = g.c1 = -1;
    }
    for (size_t i = 0; i < groupOf.size(); ++i) {
      const int g = groupOf[i];
      if (g < 0) continue;
      Loose& part = found[size_t(g)];
      const int br = int(i) / C, c = int(i) % C;
      part.r0 = std::min(part.r0, br);
      part.r1 = std::max(part.r1, br);
      part.c0 = std::min(part.c0, c);
      part.c1 = std::max(part.c1, c);
      // Pinned since the last pair: the last placed frame shows it where it is pinned.
      if (pinned_[i] && !picAt_[i]) {
        picAt_[i] = last;
        for (int y = br * R; y < std::min(H, (br + 1) * R); ++y) {
          pinPic_[size_t(y) * C + c] = ref.cellRow(y)[c];
          pinTex_[size_t(y) * C + c] = uint8_t(ref.cellTexRow(y)[c] != 0);
        }
      }
    }
    // Where each part that can be followed is: 0 in place, the offset it
    // moved to, or INT_MIN (cannot tell).
    std::vector<int> shiftOf(static_cast<size_t>(groups), INT_MIN);
    // Found only further than the page moved: it can only have moved with the whole view.
    std::vector<int> viewOnly(static_cast<size_t>(groups), INT_MIN);
    std::vector<uint8_t> followable(static_cast<size_t>(groups), 0);
    for (int g = 0; g < groups; ++g) {
      Loose& part = found[size_t(g)];
      const int w = part.c1 - part.c0 + 1;
      part.blocks.assign(size_t(part.r1 - part.r0 + 1) * w, 0);
      size_t stale = 0;
      for (int br = part.r0; br <= part.r1; ++br)
        for (int c = part.c0; c <= part.c1; ++c) {
          const size_t i = size_t(br) * C + c;
          if (groupOf[i] != g) continue;
          part.blocks[size_t(br - part.r0) * w + (c - part.c0)] = 1;
          if (!pinned_[i] || !picAt_[i]) continue;
          for (int y = br * R; y < std::min(H, (br + 1) * R); ++y)
            if (pinTex_[size_t(y) * C + c]) {
              part.ys.push_back(y);
              part.cs.push_back(c);
              part.values.push_back(pinPic_[size_t(y) * C + c]);
              stale += picAt_[i] != last;
            }
        }
      if (int(part.values.size()) < kLooseCells) continue;  // too little of it to follow
      followable[size_t(g)] = 1;
      int compared = 0;
      const double here = looseShare(part, cur, 0, 0.0, &compared);
      int s = INT_MIN;
      if (here < kInPlace && 10 * stale <= part.values.size()) {
        s = findLoose(part, cur, -d, std::min(0, -d) - kLooseSlack, std::max(0, -d) + kLooseSlack);
        // Not within the page's move: perhaps the whole view moved (below).
        if (s == INT_MIN) {
          const int wide = findLoose(part, cur, -d, -H / 4, H / 4);
          if (wide != INT_MIN && wide != 0) viewOnly[size_t(g)] = wide;
        }
      }
      if (s == INT_MIN || s == 0) {
        // Where it was pinned: its picture as the frame shows it now (a
        // contents column's highlighted entry moves on).
        if (here >= kPictureKeep)
          for (int br = part.r0; br <= part.r1; ++br)
            for (int c = part.c0; c <= part.c1; ++c) {
              const size_t i = size_t(br) * C + c;
              if (groupOf[i] != g || !pinned_[i]) continue;
              picAt_[i] = now;
              for (int y = br * R; y < std::min(H, (br + 1) * R); ++y) {
                pinPic_[size_t(y) * C + c] = cur.cellRow(y)[c];
                pinTex_[size_t(y) * C + c] = uint8_t(cur.cellTexRow(y)[c] != 0);
              }
            }
        if (here >= kInPlace) shiftOf[size_t(g)] = 0;
        continue;
      }
      shiftOf[size_t(g)] = s;
    }
    // Every part that can be followed moved by one offset in one frame: the
    // browser's view moved (an information bar opened above the page and
    // pushed it down, fixed parts and all: Firefox's DRM bar on the Al
    // Jazeera article). Nothing came loose: where they lie now they are not
    // the page in this frame, and they stay pinned (learned again where they
    // stand from now on).
    // A bar across the frame (a header, a banner: two thirds of its width or
    // more) is among the parts that moved: side columns that let go together
    // where their column ends, however wide, move with the page as the view
    // does, and are no view move.
    const auto barPart = [&](int g) { return (found[size_t(g)].c1 - found[size_t(g)].c0 + 1) * 3 >= 2 * C; };
    int moved = 0, still = 0, common = INT_MIN;
    bool same = true, barMoved = false;
    for (int g = 0; g < groups; ++g) {
      if (!followable[size_t(g)]) continue;
      const int s = shiftOf[size_t(g)] != INT_MIN ? shiftOf[size_t(g)] : viewOnly[size_t(g)];
      if (s == INT_MIN || s == 0) {
        still += s == 0;
        continue;
      }
      ++moved;
      barMoved |= barPart(g);
      if (common == INT_MIN) common = s;
      same &= std::abs(s - common) <= 1;
    }
    // Or, frame to frame: more parts moved with the page by its step in this
    // pair than stayed where the last frame showed them, and none is in place
    // by its picture. An information bar that opens in steps moves the view
    // again before the parts' pictures are known where they stand now (the
    // second step of Firefox's DRM bar, whose sub-navigation would otherwise
    // pass for the page moving); what sticks to the bottom of the view stays.
    // A column that comes loose at its end moves alone: the header stays.
    int pairMoved = 0, pairStill = 0;
    bool pairBarMoved = false;
    if (d != 0) {
      const unsigned tol = unsigned(opts_.cellTolerance);
      for (int g = 0; g < groups; ++g) {
        if (!followable[size_t(g)]) continue;
        const Loose& part = found[size_t(g)];
        int stay = 0, withPage = 0, n = 0;
        for (int br = part.r0; br <= part.r1; ++br)
          for (int c = part.c0; c <= part.c1; ++c) {
            const size_t i = size_t(br) * C + c;
            if (groupOf[i] != g || !pinned_[i]) continue;
            for (int y = br * R; y < std::min(H, (br + 1) * R); ++y) {
              if (!cur.cellTexRow(y)[c]) continue;
              ++n;
              const uint16_t a = cur.cellRow(y)[c];
              if (absDiff(a, ref.cellRow(y)[c]) <= tol) ++stay;
              else if (y + d >= 0 && y + d < H && absDiff(a, ref.cellRow(y + d)[c]) <= tol) ++withPage;
            }
          }
        if (n < kLooseCells) continue;
        if (double(stay) >= kInPlace * n) {
          ++pairStill;
        } else if (withPage >= kLooseCells && double(withPage) >= kLooseShare * double(n - stay)) {
          ++pairMoved;
          pairBarMoved |= barPart(g);
        }
      }
    }
    bool viewMoved = (moved >= 2 && same && still == 0 && barMoved) ||
                     (pairMoved >= 2 && pairMoved > pairStill && still == 0 && pairBarMoved);
    // The view's move goes on while the frames keep moving its way (the bar's
    // opening eases out over a few frames of a pixel or two, too little to
    // tell apart), up to kViewRunFrames frames, while most of the parts are
    // still away from where they were pinned: no frame of it is the page
    // scrolling. Only a move against the way the page was scrolling goes on
    // (a bar opening above the page pushes it down while it scrolls up the
    // screen); one the same way cannot be told from the scrolling itself.
    int followed = 0;
    for (int g = 0; g < groups; ++g) followed += followable[size_t(g)];
    const int way = d > 0 ? 1 : -1;
    if (viewMoved) {
      if (viewRun_ == way || (lastMove_ != 0 && (lastMove_ > 0 ? 1 : -1) != way)) {
        viewRun_ = way;
        viewRunFrames_ = 0;
      }
    } else if (viewRun_ != 0 && d != 0 && way == viewRun_ && 2 * still < followed && ++viewRunFrames_ <= kViewRunFrames) {
      viewMoved = true;
    } else {
      viewRun_ = 0;
    }
    viewMoved_ = viewMoved;
    for (int g = 0; g < groups; ++g) {
      const int s = viewMoved && shiftOf[size_t(g)] == INT_MIN ? viewOnly[size_t(g)] : shiftOf[size_t(g)];
      if (s == INT_MIN || s == 0) continue;
      Loose& part = found[size_t(g)];
      if (viewMoved) {
        part.shift = s;
        markLoose(part);
        continue;
      }
      // Come loose: no longer pinned; drawn once by finish where it was.
      for (int br = part.r0; br <= part.r1; ++br)
        for (int c = part.c0; c <= part.c1; ++c) {
          const size_t i = size_t(br) * C + c;
          if (groupOf[i] != g) continue;
          pinned_[i] = stayed_[i] = wandering_[i] = wanderingPage_[i] = wanderingEdge_[i] = 0;
          wanderingMinPos_[i] = wanderingMaxPos_[i] = wanderingEdgePos_[i] = 0;
          picAt_[i] = 0;
          blocked_[i] = 0;
          released_[i] = 1;
        }
      maskChanged = true;
      // Back where the page lays it out: a part that sticks at the top lies
      // lower there (the page scrolled back up to its start), one that sticks
      // at the bottom higher. That is the page, as the page shows it; only
      // the other way (its column ended under it) it is not.
      const bool topSticky = part.r0 * R * 2 < H;
      if (topSticky ? s > 0 : s < 0) continue;
      part.shift = s;
      markLoose(part);
      // Its picture, for finish() to find where the first or the last screen shows it.
      if (releasedPics_.size() >= kKeptPictures) releasedPics_.erase(releasedPics_.begin());
      releasedPics_.push_back(part);
      loose_.push_back(std::move(part));
    }
  }
  if (maskChanged) closeMask();
  else if (ghost_ != ghostBefore) refreshLanes();
}

// The lanes that never vote: the closed mask, the excluded rectangle and what
// a loose part lies over, per cell and per band (a band with any such cell in
// the block row).
void ScrollTracker::refreshLanes() {
  const int C = cells_, R = blockRowCount_;
  closed16_.assign(closed_.begin(), closed_.end());
  for (size_t i = 0; i < closed16_.size() && i < excl_.size(); ++i) closed16_[i] = uint16_t(closed16_[i] | excl_[i]);
  for (size_t i = 0; i < closed16_.size() && i < ghost_.size(); ++i) closed16_[i] = uint16_t(closed16_[i] | ghost_[i]);
  const int B = int(bandLo_.size()) - 1;
  bandPinned16_.assign(size_t(R) * B, 0);
  for (int r = 0; r < R; ++r)
    for (int k = 0; k < B; ++k)
      for (int c = bandLo_[size_t(k)]; c < bandLo_[size_t(k) + 1]; ++c)
        if (closed16_[size_t(r) * C + c]) {
          bandPinned16_[size_t(r) * B + k] = 1;
          break;
        }
}

void ScrollTracker::setExcluded(TrackerRect rect) {
  excludedRect_ = rect;
  if (cells_ > 0) applyExcluded();
}

// The excluded rectangle in blocks (rounded out), and what follows from it:
// nothing learned there, nothing pinned there, the lanes, and the frame rows
// that count for the extent.
void ScrollTracker::applyExcluded() {
  const int C = cells_, R = blockRowCount_, P = opts_.cellPx, BR = opts_.blockRows, words = (C + 63) / 64;
  excl_.assign(size_t(R) * C, 0);
  exclLane16_.assign(size_t(R) * C, 0);
  exclWords_.assign(size_t(R) * words, 0);
  effTop_ = 0;
  effBottom_ = height_;
  const TrackerRect& e = excludedRect_;
  if (!e.empty()) {
    // In 64 bits: x + w of a rectangle given in ints may not fit in one.
    const auto out = [](long long v, long long unit, int n) { return int(std::clamp<long long>(v / unit, 0, n)); };
    const int c0 = out(e.x, P, C), c1 = out((long long)e.x + e.w + P - 1, P, C);
    const int r0 = out(e.y, BR, R), r1 = out((long long)e.y + e.h + BR - 1, BR, R);
    for (int r = r0; r < r1; ++r)
      for (int c = c0; c < c1; ++c) {
        excl_[size_t(r) * C + c] = 1;
        exclLane16_[size_t(r) * C + c] = 1;
        exclWords_[size_t(r) * words + size_t(c >> 6)] |= 1ull << (c & 63);
      }
    if (c0 == 0 && c1 == C && r0 < r1) {
      // Rows covered across the whole width at either end are not the page's.
      if (r0 == 0) effTop_ = std::min(height_, r1 * BR);
      if (r1 == R) effBottom_ = std::max(0, r0 * BR);
      if (effBottom_ <= effTop_) {
        effTop_ = 0;
        effBottom_ = height_;
      }
    }
  }
  for (size_t i = 0; i < excl_.size(); ++i)
    if (excl_[i]) {
      pinned_[i] = stayed_[i] = wandering_[i] = wanderingPage_[i] = wanderingEdge_[i] = stillNow_[i] = 0;
      wanderingMinPos_[i] = wanderingMaxPos_[i] = wanderingEdgePos_[i] = 0;
      if (i < picAt_.size()) picAt_[i] = 0;
    }
  closeMask();
}

// The topmost and the bottommost frames are kept for what is pinned, which
// is in the image once, as they show it. While the mask is young the whole
// frame is kept; afterwards only its blocks that are pinned or stood still
// (a frame copy per frame of a 60 Hz stream would cost more than the rest).
void ScrollTracker::keepCopy(const FrameView& f, const Signature& cur, FrameCopy& copy, long long pos, bool all,
                             int fromRow) {
  const int C = cells_, R = blockRowCount_, P = opts_.cellPx, BR = opts_.blockRows;
  copy.px.resize(size_t(width_) * height_ * 4);
  copy.tag.resize(size_t(R) * C, 0);
  ++copy.serial;
  copy.pos = pos;
  if (all) {
    for (int y = 0; y < height_; ++y) std::memcpy(&copy.px[size_t(y) * width_ * 4], f.row(y), size_t(width_) * 4);
    copy.bands.resize(size_t(height_) * size_t(cur.bands));
    for (int y = 0; y < height_; ++y)
      std::memcpy(&copy.bands[size_t(y) * size_t(cur.bands)], cur.bandRow(y), size_t(cur.bands) * sizeof(uint16_t));
    std::fill(copy.tag.begin(), copy.tag.end(), copy.serial);
    // What the excluded rectangle covers is never drawn from a copy, nor
    // where a loose part lies.
    for (size_t i = 0; i < copy.tag.size() && i < excl_.size(); ++i)
      if (excl_[i] || ghost_[i]) copy.tag[i] = 0;
    return;
  }
  copy.bands.clear();
  for (int r = std::max(0, fromRow) / BR; r < R; ++r)
    for (int c = 0; c < C;) {
      const size_t bi = size_t(r) * C + c;
      if (!blocked_[bi] || excl_[bi] || ghost_[bi]) {
        if (ghost_[bi]) copy.tag[bi] = 0;
        ++c;
        continue;
      }
      int e = c;
      while (e < C && blocked_[size_t(r) * C + e] && !excl_[size_t(r) * C + e] && !ghost_[size_t(r) * C + e])
        copy.tag[size_t(r) * C + e++] = copy.serial;
      const int x0 = c * P, x1 = std::min(width_, e * P);
      for (int y = r * BR; y < std::min(height_, (r + 1) * BR); ++y)
        std::memcpy(&copy.px[(size_t(y) * width_ + x0) * 4], f.row(y) + size_t(x0) * 4, size_t(x1 - x0) * 4);
      c = e;
    }
}

// Per block row, the cells a frame does not offer (pinned, or stood still in
// the last pair), as words.
void ScrollTracker::blockedWords(std::vector<uint64_t>& out) const {
  const int C = cells_, words = store_->words;
  out.assign(size_t(blockRowCount_) * words, 0);
  for (int br = 0; br < blockRowCount_; ++br) {
    const uint8_t* row = &blocked_[size_t(br) * C];
    const uint8_t* loose = &ghost_[size_t(br) * C];
    uint64_t* w = &out[size_t(br) * words];
    for (int c = 0; c < C; ++c) w[c >> 6] |= uint64_t((row[c] | loose[c]) != 0) << (c & 63);
    if (!exclWords_.empty())
      for (int k = 0; k < words; ++k) w[k] |= exclWords_[size_t(br) * words + size_t(k)];
  }
}

// Writes the cells of `mask` (words) of page row p from a frame row, at
// `level` (1: seen once, 2: two frames agreed), and keeps the row's band
// signature where the frame showed the band clear.
void ScrollTracker::writeCells(long long p, const uint8_t* row, const uint16_t* bands, const uint16_t* tex,
                               const uint64_t* mask, int level, const uint64_t* blocked) {
  const int C = cells_, P = opts_.cellPx, words = store_->words, B = int(bandLo_.size()) - 1;
  const size_t at = size_t(p - store_->origin) * words;
  // Spans of consecutive cells, found word by word.
  int start = -1, end = -1;
  const auto flush = [&] {
    if (start < 0) return;
    const int x0 = start * P, x1 = std::min(width_, end * P);
    canvas_.writeSpan(p, x0, x1, row + size_t(x0) * 4);
    start = -1;
  };
  for (int w = 0; w < words; ++w) {
    store_->prov[at + size_t(w)] |= mask[w];
    if (level >= 2) store_->ver[at + size_t(w)] |= mask[w];
    uint64_t bits = mask[w];
    while (bits) {
      const int c = w * 64 + std::countr_zero(bits);
      bits &= bits - 1;
      if (c != end) {
        flush();
        start = c;
      }
      end = c + 1;
    }
  }
  flush();
  (void)C;
  uint16_t* sb = store_->bandRow(p);
  uint16_t* sv = store_->validRow(p);
  uint16_t* st = store_->texRow(p);
  for (int k = 0; k < B; ++k) {
    if (store_->isDyn(p, k)) continue;
    if (sv[k] && level < 2) continue;
    // Clear when none of the band's cells is blocked: test its words.
    const int lo = bandLo_[size_t(k)], hi = bandLo_[size_t(k) + 1];
    bool clear = true;
    for (int c = lo; c < hi && clear;) {
      const int w = c >> 6, upto = std::min(hi, (w + 1) * 64);
      const uint64_t range = (upto - c >= 64 ? ~0ull : ((1ull << (upto - c)) - 1)) << (c & 63);
      clear = !(blocked[w] & range);
      c = upto;
    }
    if (!clear) continue;
    sb[k] = bands[k];
    st[k] = tex[k];
    sv[k] = 1;
  }
}

// What the frame shows goes into the image only where it is the page (#1538):
// a cell proved to move with the page is written at once (verified); a cell
// seen once waits for the next placed frame and is written as seen
// (provisional) only where that pair does not find it standing still and the
// screen shows the page there; a plain cell is a vote for its colour, taken by
// finish() where nothing better was written. What stays on the screen is
// never written from a frame: finish() draws it once.
void ScrollTracker::compose(const FrameView& f, const Signature& cur, const Signature* ref, int d, long long pos,
                            TrackResult& r) {
  const int H = height_, C = cells_;
  const long long cap = opts_.maxOutputRows;
  const long long top0 = top_, bot0 = bottom_;
  // The image's extent before this frame (flushPending, when finishing).
  extentLo0_ = hasFrame_ ? top0 : pos;
  extentHi0_ = hasFrame_ ? bot0 : pos;
  // The frame's rows that count for the extent: not those the excluded
  // rectangle covers across the whole width at its top or bottom. A frame of
  // a move of the whole view shows no new rows of the page above (a bar
  // opened there), only below where the view moved up (a bar above the page
  // closed and uncovered the page's next rows), for the cap as for the image.
  long long fTop = pos + effTop_, fBottom = pos + effBottom_;
  if (viewMoved_ && hasFrame_) {
    fTop = std::max(fTop, top0);
    if (d <= 0) fBottom = std::min(fBottom, std::max(fTop, bot0));
  }
  long long lo = hasFrame_ ? std::min(top_, fTop) : fTop, hi = hasFrame_ ? std::max(bottom_, fBottom) : fBottom;
  // The cap: rows outside [allowLo_, allowHi_) are not written.
  if (!hasFrame_) {
    allowLo_ = fTop;
    allowHi_ = fTop + cap;
  }
  if (hi - lo > cap) {
    capped_ = true;
    if (fBottom > bot0 && hasFrame_) {
      allowLo_ = top0;
      allowHi_ = top0 + cap;
    } else if (fTop < top0 && hasFrame_) {
      allowHi_ = bot0;
      allowLo_ = bot0 - cap;
    }
  } else {
    allowLo_ = hi - cap;
    allowHi_ = lo + cap;
  }
  store_->ensure(std::max(allowLo_, pos), std::min(allowHi_, pos + H));
  for (long long p = std::max(allowLo_, fTop); p < std::min(allowHi_, fBottom); ++p) store_->shown[size_t(p - store_->origin)] = 1;
  const int words = store_->words;
  std::vector<uint64_t> blocked;
  blockedWords(blocked);
  // Where no frame proves anything: the excluded rectangle and a loose part.
  std::vector<uint64_t> veto(size_t(blockRowCount_) * size_t(words), 0);
  for (int br = 0; br < blockRowCount_; ++br)
    for (int c = 0; c < C; ++c)
      if (excl_[size_t(br) * C + c] || ghost_[size_t(br) * C + c]) veto[size_t(br) * words + size_t(c >> 6)] |= 1ull << (c & 63);
  std::vector<uint64_t> full(static_cast<size_t>(words), ~0ull), mask(static_cast<size_t>(words)),
      proved(static_cast<size_t>(words));
  if (C % 64) full[size_t(words - 1)] = (1ull << (C % 64)) - 1;
  const unsigned tol = unsigned(opts_.cellTolerance);
  // 1. Proved (#1538): a cell this frame shows where the last placed frame
  //    showed it, moved by the page's step, and not as that frame showed the
  //    same spot of the screen. Only the page does that: what stays on the
  //    screen (a header, a banner, a column that sticks, the browser's own
  //    bar) shows the same where it was, and a plain cell (white space, a
  //    banner's flat inside) looks the same either way, so neither proves
  //    anything. A proved cell is written, over anything weaker; the blocks
  //    of the screen it was proved in are the page's area (pageArea).
  //    A cell with texture that the pair shows the same both ways is the
  //    page repeating at the step's distance (a table of equal rows) or
  //    something that stays put: written too, as before (not while the mask
  //    is young: it waits with the frame's other cells). Neither ever from a
  //    spot learned to be pinned or standing still. A plain cell is left to
  //    the vote.
  const bool youngMask = learnedSteps_ <= 2 * opts_.pinAfterSteps;
  // A frame in which the whole view moved (an information bar opened above
  // the page) shows nothing the last one did not: the page did not scroll,
  // and what stays on the screen moved with it. Nothing is written from it.
  if (ref && d != 0 && !viewMoved_) {
    // A cell moves with the page only with its block: where most of a
    // block's textured cells do not show what the last frame showed the
    // step away (a sticky header that has just appeared over the page),
    // the one that does by chance (its text a shade off the page's there)
    // proves nothing.
    std::vector<uint64_t> lone(size_t(blockRowCount_) * words, 0);
    {
      std::vector<int16_t> lead(size_t(blockRowCount_) * C, 0);
      for (int y = std::max(0, -d); y < std::min(H, H - d); ++y) {
        const int brow = std::min(y / opts_.blockRows, blockRowCount_ - 1);
        const uint16_t* a = cur.cellRow(y);
        const uint16_t* b = ref->cellRow(y + d);
        const uint16_t* ta = cur.cellTexRow(y);
        const uint16_t* tb = ref->cellTexRow(y + d);
        int16_t* l = &lead[size_t(brow) * C];
        for (int c = 0; c < C; ++c)
          if (ta[c] || tb[c]) l[c] = int16_t(l[c] + (absDiff(a[c], b[c]) <= tol ? 1 : -1));
      }
      for (int br = 0; br < blockRowCount_; ++br)
        for (int c = 0; c < C; ++c)
          if (lead[size_t(br) * C + c] < 0) lone[size_t(br) * words + size_t(c >> 6)] |= 1ull << (c & 63);
    }
    std::vector<uint64_t> dynWords(static_cast<size_t>(words), 0);
    std::vector<uint64_t> blockProved(size_t(blockRowCount_) * size_t(words), 0);
    const int B = cur.bands;
    for (int y = std::max(0, -d); y < std::min(H, H - d); ++y) {
      const long long p = pos + y;
      if (p < allowLo_ || p >= allowHi_) continue;
      const int brow = std::min(y / opts_.blockRows, blockRowCount_ - 1);
      const int bref = std::min((y + d) / opts_.blockRows, blockRowCount_ - 1);
      const size_t at = size_t(p - store_->origin) * words;
      const uint64_t* vc = &veto[size_t(brow) * words];
      const uint64_t* vr = &veto[size_t(bref) * words];
      const uint64_t* blk = &blocked[size_t(brow) * words];
      const uint64_t* blkRef = &blocked[size_t(bref) * words];
      bool rowHasDyn = false;
      if (hasDyn_ && store_->has(p)) {
        const uint8_t* sd = store_->dynRow(p);
        for (int k = 0; k < B; ++k)
          if (sd[k]) {
            if (!rowHasDyn) {
              std::fill(dynWords.begin(), dynWords.end(), 0);
              rowHasDyn = true;
            }
            const int cLo = k == 0 ? 0 : bandLo_[size_t(k)];
            const int cHi = k == B - 1 ? C : bandLo_[size_t(k) + 1];
            for (int c = cLo; c < cHi; ++c) dynWords[size_t(c >> 6)] |= 1ull << (c & 63);
          }
      }
      const uint16_t* a = cur.cellRow(y);
      const uint16_t* b = ref->cellRow(y + d);
      const uint16_t* there = ref->cellRow(y);
      const uint16_t* ta = cur.cellTexRow(y);
      const uint16_t* tb = ref->cellTexRow(y + d);
      bool any = false;
      for (int w = 0; w < words; ++w) {
        // Never from a spot this pair or the last found pinned or standing
        // still, here or where the page moved it: what stays on the screen
        // moves with the page only with the whole view (Firefox's DRM bar
        // pushing the header down) or as a column that lets go. Also leave
        // dynamic page bands (#1537) to snapshotDynamic so video frames are
        // never mixed cell by cell.
        const uint64_t blockedMask = vc[w] | vr[w] | blk[w] | blkRef[w] | lone[size_t(brow) * words + size_t(w)] |
                                     (rowHasDyn ? dynWords[size_t(w)] : 0ull);
        const uint64_t alreadyVer = store_->ver[at + size_t(w)];
        uint64_t need = ~(alreadyVer | blockedMask) & full[size_t(w)];
        uint64_t take = 0, moved = 0;
        while (need) {
          const int bit = std::countr_zero(need);
          need &= need - 1;
          const int c = w * 64 + bit;
          if (absDiff(a[c], b[c]) > tol) continue;
          if (absDiff(a[c], there[c]) > tol) {
            take |= 1ull << bit;
            moved |= 1ull << bit;
          } else if ((ta[c] || tb[c]) && absDiff(a[c], b[c]) <= tol / 8) {
            // Repeating at the step's distance repeats exactly (a table's
            // equal rows): what stays put a shade off the page there (a
            // toolbar's edge, a banner that has just stopped) is not it.
            moved |= 1ull << bit;
            if (!youngMask) take |= 1ull << bit;
          }
        }
        // Refresh `proofAt_` on already-verified cells whenever either `brow`
        // or `bref` still lacks motion proof in this frame so revisiting a
        // captured section and pausing over a newly playing video stays
        // Following without scanning every verified row (#1537).
        uint64_t bp =
            (blockProved[size_t(brow) * words + size_t(w)] & blockProved[size_t(bref) * words + size_t(w)]) | moved;
        uint64_t verNeed = alreadyVer & ~bp & ~blockedMask & full[size_t(w)];
        while (verNeed) {
          const int bit = std::countr_zero(verNeed);
          verNeed &= verNeed - 1;
          const int c = w * 64 + bit;
          if (absDiff(a[c], b[c]) > tol) continue;
          if (absDiff(a[c], there[c]) > tol || ((ta[c] || tb[c]) && absDiff(a[c], b[c]) <= tol / 8))
            moved |= 1ull << bit;
        }
        blockProved[size_t(brow) * words + size_t(w)] |= moved;
        blockProved[size_t(bref) * words + size_t(w)] |= moved;
        mask[size_t(w)] = take;
        proved[size_t(w)] = moved;
        any |= take != 0;
      }
      // `proved` from here: the cells that showed the page here (moved with
      // it, or textured and agreeing outside what is learned to stay put).
      if (any) writeCells(p, f.row(y), cur.bandRow(y), cur.bandTexRow(y), mask.data(), 2, blk);
      for (int w = 0; w < words; ++w)
        for (uint64_t bits = proved[size_t(w)]; bits; bits &= bits - 1) {
          const int c = w * 64 + std::countr_zero(bits);
          proofAt_[size_t(brow) * C + c] = frameSerial_;
          proofAt_[size_t(bref) * C + c] = frameSerial_;
        }
    }
  }
  // 2. The last frame's cells that nothing proved, where this pair did not
  // find them standing still (flushPending): one seen once with texture (a
  // frame's new rows on a fast flick, a video that changes from frame to
  // frame) is written as seen where the screen showed the page there
  // (pageArea); a plain one is a vote for its colour. While the mask is young
  // they wait longer (at most 8 frames), until it can know what is pinned and
  // where the page is.
  // A bound, not a leak: while the mask is young (it stays so through a
  // scroll whose every step is under learnMinStep rows) each waiting frame
  // holds up to a whole frame of pixels, at most 8 of them, and spare_ keeps
  // their buffers: about 9 frames at the peak (at 3840x2160 some 300 MB),
  // whatever the session's length.
  if (!pending_.empty()) narrowPending(pending_.back(), blocked, cur, pos);
  const bool young = youngMask && pending_.size() < 8;
  if (!young) {
    std::vector<uint64_t> area;
    std::vector<uint64_t> near;
    pageArea(area, &near);
    flushPending(blocked, area, near, false);
  }
  // 3. This frame's cells that nothing is written for yet wait for the next:
  //    their pixels (cell spans, not whole rows), the row's bands, which cells
  //    they are and which of them are plain.
  if (spare_.empty()) {
    pending_.emplace_back();
  } else {
    pending_.push_back(std::move(spare_.back()));
    spare_.pop_back();
  }
  Pending& pend = pending_.back();
  pend.pos = pos;
  pend.rows.clear();
  const int B = cur.bands, P = opts_.cellPx;
  for (int y = 0; y < H; ++y) {
    const long long p = pos + y;
    if (p < allowLo_ || p >= allowHi_ || (viewMoved_ && !(d > 0 && p >= bot0))) continue;
    const int brow = std::min(y / opts_.blockRows, blockRowCount_ - 1);
    const size_t at = size_t(p - store_->origin) * words;
    // Page cells the excluded rectangle covered in a placed frame, and those
    // a placed frame showed outside it (pinned or not): finish() counts the
    // first that are not the second and that nothing was written for.
    for (int w = 0; w < words; ++w) {
      const uint64_t e = exclWords_[size_t(brow) * words + size_t(w)];
      store_->hid[at + size_t(w)] |= e;
      store_->vis[at + size_t(w)] |= ~e & full[size_t(w)];
    }
    bool open = false;
    const uint16_t* tRow = cur.cellTexRow(y);
    for (int w = 0; w < words; ++w) {
      const uint64_t blkW = blocked[size_t(brow) * words + size_t(w)];
      uint64_t m = ~(store_->prov[at + size_t(w)] | blkW) & full[size_t(w)];
      // Plain cells on an unverified provisional cell (e.g. a 1-frame transient
      // overlay edge flushed before a second plain frame arrived) still flow to
      // flushPending so 2+ plain votes can overrule it (#1537).
      uint64_t provUnver = store_->prov[at + size_t(w)] & ~store_->ver[at + size_t(w)] & ~blkW &
                           ~veto[size_t(brow) * words + size_t(w)] & full[size_t(w)];
      while (provUnver) {
        const int bit = std::countr_zero(provUnver);
        provUnver &= provUnver - 1;
        if (!tRow[w * 64 + bit]) m |= 1ull << bit;
      }
      mask[size_t(w)] = m;
      open |= m != 0;
    }
    if (!open) continue;
    // A plain cell that shows the colour its page cell already leads with is
    // one vote more, at once; only a new one or another colour waits for the
    // next pair (most of a frame's plain cells, every frame).
    {
      const uint16_t* t = tRow;
      const uint16_t* v = cur.cellRow(y);
      const size_t row = size_t(p - store_->origin) * C;
      uint8_t* val = &store_->plainVal[row];
      uint8_t* votes = &store_->plainVotes[row];
      const int same = kPlainSame;
      open = false;
      for (int w = 0; w < words; ++w) {
        for (uint64_t bits = mask[size_t(w)]; bits; bits &= bits - 1) {
          const int c = w * 64 + std::countr_zero(bits);
          if (t[c] || ((veto[size_t(brow) * words + size_t(w)] >> (c & 63)) & 1)) continue;
          if ((store_->prov[at + size_t(w)] >> (c & 63)) & 1ull) continue;
          const int count = votes[c] & 0x7F;
          if (count >= 1 && std::abs(int(val[c]) - std::min(255, v[c] / 64)) <= same) {
            votes[c] = uint8_t(std::min(127, count + 1) | (votes[c] & 0x80));
            mask[size_t(w)] &= ~(1ull << (c & 63));
          }
        }
        open |= mask[size_t(w)] != 0;
      }
      if (!open) continue;
    }
    const size_t slot = pend.rows.size();
    pend.rows.push_back(y);
    pend.pixels.resize((slot + 1) * size_t(width_) * 4);
    pend.bands.resize((slot + 1) * size_t(B) * 2);
    pend.open.resize((slot + 1) * size_t(words));
    std::memcpy(&pend.open[slot * size_t(words)], mask.data(), size_t(words) * 8);
    pend.plain.resize((slot + 1) * size_t(words));
    pend.cells.resize((slot + 1) * size_t(C));
    std::memcpy(&pend.cells[slot * size_t(C)], cur.cellRow(y), size_t(C) * 2);
    {
      uint64_t* pl = &pend.plain[slot * size_t(words)];
      std::fill(pl, pl + words, 0);
      const uint16_t* t = cur.cellTexRow(y);
      for (int c = 0; c < C; ++c)
        if (!t[c] && !((veto[size_t(brow) * words + size_t(c >> 6)] >> (c & 63)) & 1)) pl[c >> 6] |= 1ull << (c & 63);
    }
    const uint8_t* row = f.row(y);
    uint8_t* to = &pend.pixels[slot * size_t(width_) * 4];
    for (int w = 0; w < words; ++w) {
      uint64_t bits = mask[size_t(w)];
      while (bits) {
        const int c = w * 64 + std::countr_zero(bits);
        int e = c + 1;
        bits &= bits - 1;
        while (bits && w * 64 + std::countr_zero(bits) == e) {
          ++e;
          bits &= bits - 1;
        }
        const int x0 = c * P, x1 = std::min(width_, e * P);
        std::memcpy(to + size_t(x0) * 4, row + size_t(x0) * 4, size_t(x1 - x0) * 4);
      }
    }
    std::memcpy(&pend.bands[slot * size_t(B) * 2], cur.bandRow(y), size_t(B) * 2);
    std::memcpy(&pend.bands[slot * size_t(B) * 2 + size_t(B)], cur.bandTexRow(y), size_t(B) * 2);
  }
  if (pend.rows.empty()) {
    spare_.push_back(std::move(pend));
    pending_.pop_back();
  }
  // The extent follows the frame even where a row has nothing written yet.
  top_ = hasFrame_ ? std::max(allowLo_, std::min(top_, fTop)) : std::max(allowLo_, fTop);
  bottom_ = hasFrame_ ? std::min(allowHi_, std::max(bottom_, fBottom)) : std::min(allowHi_, fBottom);
  r.addedTop = int(std::max<long long>(0, top0 - top_));
  r.addedBottom = int(std::max<long long>(0, bottom_ - bot0));
  if (!hasFrame_) {
    r.addedTop = 0;
    r.addedBottom = int(bottom_ - top_);
  }
}

// The waiting cells of placed frames, where they are not blocked now and
// nothing is written for them yet, the oldest frame first: one with texture
// is written as seen where the screen shows the page there (`area`, per block
// row as words); a plain one is a vote for its colour (#1538). No pair tells
// whether a cell of one even colour moved with the page or stayed on the
// screen (white space, a banner's flat inside, a stripe down a banner's
// border), so none is written as the page; the colour most frames showed at
// that spot of the page leads (a page row passes under a bottom banner in a
// few frames and shows the page in the many others), its pixels are in the
// image (not for placing frames: the store does not count them as written),
// and finish() takes it where nothing was proved or seen.
// Whether a plain cell's colour (its value / 64) is the page's own background
// so far: the first screen's most common colour, or the one most often behind
// the text that moved with the page (learn).
bool ScrollTracker::pageBackgroundBin(int bin) const {
  const unsigned tol = unsigned(opts_.cellTolerance);
  const int reach = int(tol / 64);
  return absDiff(uint16_t(std::min(255, bin) * 64 + 32), uint16_t(pageCell_)) <= tol ||
         (pageBgBin_ >= 0 && std::abs(bin - pageBgBin_) <= reach);
}

void ScrollTracker::flushPending(const std::vector<uint64_t>& blocked, const std::vector<uint64_t>& area,
                                const std::vector<uint64_t>& near, bool finishing) {
  const int words = store_->words, C = cells_, B = int(bandLo_.size()) - 1, P = opts_.cellPx;
  const int same = kPlainSame;
  std::vector<uint64_t> full(static_cast<size_t>(words), ~0ull), mask(static_cast<size_t>(words));
  if (C % 64) full[size_t(words - 1)] = (1ull << (C % 64)) - 1;
  std::vector<uint8_t> flatCell(size_t(P) * 4);
  // Screen columns with nothing held back in them anywhere: what a frame
  // shows plain there can only be the page (a rule down the page, the page's
  // own margin), so its votes count as in the page's area.
  std::vector<uint64_t> clearCols(full);
  for (size_t k = 0; k < blocked.size(); ++k) clearCols[k % size_t(words)] &= ~blocked[k];
  for (const Pending& pend : pending_)
    for (size_t slot = 0; slot < pend.rows.size(); ++slot) {
      const int y = pend.rows[slot];
      const long long p = pend.pos + y;
      if (!store_->has(p) || p < allowLo_ || p >= allowHi_) continue;
      const int brow = std::min(y / opts_.blockRows, blockRowCount_ - 1);
      const size_t at = size_t(p - store_->origin) * words;
      const uint64_t* blk = &blocked[size_t(brow) * words];
      // The page's last rows, that only the last frame showed beyond what
      // the frames before it did (the frame Done takes, past a stream that
      // went stale): the page's wherever nothing stays, when the capture ends.
      const bool fresh = finishing && &pend == &pending_.back() && pend.pos == lastPos_ && (p < extentLo0_ || p >= extentHi0_);
      const uint64_t* page = fresh ? full.data() : &area[size_t(brow) * words];
      const uint64_t* beside = fresh ? full.data() : &near[size_t(brow) * words];
      const uint64_t* open = &pend.open[slot * size_t(words)];
      const uint64_t* plain = &pend.plain[slot * size_t(words)];
      const uint8_t* px = &pend.pixels[slot * size_t(width_) * 4];
      const uint8_t* counted = &store_->plainVotes[size_t(p - store_->origin) * C];
      std::vector<uint64_t> dynWords;
      bool rowHasDyn = false;
      if (hasDyn_) {
        const uint8_t* sd = store_->dynRow(p);
        for (int k = 0; k < B; ++k)
          if (sd[k]) {
            if (!rowHasDyn) {
              dynWords.assign(static_cast<size_t>(words), 0);
              rowHasDyn = true;
            }
            const int cLo = k == 0 ? 0 : bandLo_[size_t(k)];
            const int cHi = k == B - 1 ? C : bandLo_[size_t(k) + 1];
            for (int c = cLo; c < cHi; ++c) dynWords[size_t(c >> 6)] |= 1ull << (c & 63);
          }
      }
      bool any = false;
      for (int w = 0; w < words; ++w) {
        const uint64_t dw = rowHasDyn ? dynWords[size_t(w)] : 0ull;
        mask[size_t(w)] = ~(store_->prov[at + size_t(w)] | blk[w] | plain[w] | dw) & full[size_t(w)] & open[w] & page[w];
        // Not where two frames already showed the page plain (a side bar's
        // line caught once as it settled into place): two against one.
        for (uint64_t bits = mask[size_t(w)]; bits; bits &= bits - 1) {
          const int c = w * 64 + std::countr_zero(bits);
          if ((counted[c] & 0x7F) >= 2) mask[size_t(w)] &= ~(1ull << (c & 63));
        }
        any |= mask[size_t(w)] != 0;
      }
      if (any) {
        const uint16_t* bands = &pend.bands[slot * size_t(B) * 2];
        writeCells(p, px, bands, bands + B, mask.data(), 1, blk);
      }
      // The plain ones' votes: a count (up to 127) and whether the leading
      // colour was seen where the screen showed the page (the top bit), which
      // takes over from one seen elsewhere that it agrees with (a header's
      // background a shade off the page's). Plain votes also overrule a cell
      // seen once (a side bar's line caught as it moved into
      // place, a box a frame showed once): two that agree overrule it.
      uint8_t* val = &store_->plainVal[size_t(p - store_->origin) * C];
      uint8_t* votes = &store_->plainVotes[size_t(p - store_->origin) * C];
      int start = -1;
      const auto put = [&](int e) {
        if (start < 0) return;
        const int x0 = start * P, x1 = std::min(width_, e * P);
        canvas_.writeSpan(p, x0, x1, px + size_t(x0) * 4);
        start = -1;
      };
      for (int c = 0; c < C; ++c) {
        const int w = c >> 6;
        const uint64_t bit = 1ull << (c & 63);
        const bool inDyn = rowHasDyn && ((dynWords[size_t(w)] & bit) != 0);
        bool take = false;
        const uint8_t v = uint8_t(std::min(255, pend.cells[slot * size_t(C) + size_t(c)] / 64));
        const bool inArea = ((page[w] | clearCols[size_t(w)]) & bit) != 0;
        // Where the screen does not show the page, only the page's own
        // background counts: another colour there is the flat inside of what
        // stays on the screen (an advertisement's red beside its artwork,
        // seen once at each page row on a fast flick), never the page's.
        if (!inDyn && (open[w] & plain[w] & bit) && !((store_->ver[at + size_t(w)] | blk[w]) & bit) &&
            (inArea || pageBackgroundBin(v))) {
          int count = votes[c] & 0x7F;
          bool fromArea = (votes[c] & 0x80) != 0;
          if (count == 0) {
            take = true;
            count = 1;
            fromArea = inArea;
          } else if (std::abs(int(val[c]) - int(v)) <= same) {
            count = std::min(127, count + 1);
            if (inArea && !fromArea) {
              take = true;
              fromArea = true;
            }
          } else if (--count == 0) {
            take = true;
            count = 1;
            fromArea = inArea;
          }
          if (take) val[c] = v;
          votes[c] = uint8_t(count | (fromArea ? 0x80 : 0));
          // A cell seen once, overruled.
          if ((store_->prov[at + size_t(w)] & bit) && count >= 2 && std::abs(int(val[c]) - int(v)) <= same) {
            store_->prov[at + size_t(w)] &= ~bit;
            take = true;
          }
          if ((store_->prov[at + size_t(w)] & bit)) take = false;
        }
        if (take && !(votes[c] & 0x80) && !(beside[w] & bit)) {
          // Seen only where the screen does not show the page: the page's
          // background, flat (the cell's most common pixel), never a detail
          // of what stays on the screen there (a banner's border down its
          // side, one pixel of a margin's cell). One cell beside the page's
          // area it keeps its pixels: the page's own edge lies there (an
          // image's last column of pixels, a box's border).
          put(c);
          const int x0 = c * P, x1 = std::min(width_, x0 + P);
          uint32_t pick = 0;
          int most = 0;
          for (int x = x0; x < x1; ++x) {
            uint32_t a, b;
            std::memcpy(&a, px + size_t(x) * 4, 4);
            int n = 0;
            for (int k = x0; k < x1; ++k) {
              std::memcpy(&b, px + size_t(k) * 4, 4);
              n += a == b;
            }
            if (n > most) {
              most = n;
              pick = a;
            }
          }
          for (int x = x0; x < x1; ++x) std::memcpy(&flatCell[size_t(x - x0) * 4], &pick, 4);
          canvas_.writeSpan(p, x0, x1, flatCell.data());
          continue;
        }
        if (take) {
          if (start < 0) start = c;
        } else {
          put(c);
        }
      }
      put(C);
    }
  for (Pending& pend : pending_) spare_.push_back(std::move(pend));
  pending_.clear();
}

// The screen's page area now (#1538), per block row as words. Down each
// column of blocks, what is pinned, stood still, lies under a loose part or
// is excluded splits the column into stretches; a stretch in which a pair
// showed the page within the last kAreaFrames placed frames is the page's,
// all of it, out to the frame's edge (the page's last rows that only the last
// frame showed, below rows the page moved through). A cell seen only once is
// written only there: a flick's new rows beside the page's, never a column's
// or a bar's that no pair could learn (the middle of a contents column a step
// over half the frame jumped past is in a stretch that never showed the page).
// `near`: also one cell beside it, where nothing is known to stay (see
// flushPending).
void ScrollTracker::pageArea(std::vector<uint64_t>& out, std::vector<uint64_t>* near) const {
  const int C = cells_, R = blockRowCount_, words = store_->words;
  out.assign(size_t(R) * words, 0);
  const auto stuck = [&](size_t i) { return blocked_[i] || ghost_[i] || excl_[i]; };
  const auto has = [&](int r, int c) { return ((out[size_t(r) * words + size_t(c >> 6)] >> (c & 63)) & 1) != 0; };
  for (int c = 0; c < C; ++c)
    for (int r = 0; r < R;) {
      if (stuck(size_t(r) * C + c)) {
        ++r;
        continue;
      }
      int e = r;
      bool page = false;
      for (; e < R && !stuck(size_t(e) * C + c); ++e) {
        const uint32_t at = proofAt_[size_t(e) * C + c];
        page |= at && frameSerial_ - at <= kAreaFrames;
      }
      if (page)
        for (int k = r; k < e; ++k) out[size_t(k) * words + size_t(c >> 6)] |= 1ull << (c & 63);
      r = e;
    }
  if (!near) return;
  *near = out;
  for (int r = 0; r < R; ++r)
    for (int c = 0; c < C; ++c)
      if (!has(r, c) && !stuck(size_t(r) * C + c) && ((c > 0 && has(r, c - 1)) || (c + 1 < C && has(r, c + 1))))
        (*near)[size_t(r) * words + size_t(c >> 6)] |= 1ull << (c & 63);
}

// What a waiting frame's next pair found pinned or standing still is not
// written from it, whenever it is written. Nor, in the frame's first and last
// block rows, a cell with texture that the next frame, the page moved, shows
// the same at the same place of the screen: a line along the region's edge
// (the dark row under Firefox's page on Cinnamon) stays there, one row of a
// block that moves with the page, so the block is never held back, and it
// was written into the page's rows as seen once (#1796). The page's own new
// rows at the edge are where the next frame shows other rows, and that pair
// proves them where they moved to.
void ScrollTracker::narrowPending(Pending& pend, const std::vector<uint64_t>& blocked, const Signature& cur,
                                  long long pos) const {
  const int words = store_->words, C = cells_, R = opts_.blockRows;
  const unsigned tol = unsigned(opts_.cellTolerance);
  const bool moved = pend.pos != pos && !viewMoved_;
  for (size_t slot = 0; slot < pend.rows.size(); ++slot) {
    const int y = pend.rows[slot];
    const int brow = std::min(y / R, blockRowCount_ - 1);
    uint64_t* open = &pend.open[slot * size_t(words)];
    for (int w = 0; w < words; ++w) open[w] &= ~blocked[size_t(brow) * words + size_t(w)];
    if (!moved || (y >= R && y < (blockRowCount_ - 1) * R)) continue;
    // And the next frame must show something else where the page row moved
    // to: page content that repeats at the step (a rule every step rows)
    // agrees there too. Only cells in sight and not held back there are
    // judged, and a row the next frame cannot show is not judged at all: what
    // the next frame hides there may be the page's (a scroll bar's column
    // keeps its piece of the line).
    const long long ya = y - (pos - pend.pos);
    if (ya < 0 || ya >= height_) continue;
    const int browA = std::min(int(ya) / R, blockRowCount_ - 1);
    const uint64_t* blkA = &blocked[size_t(browA) * words];
    // A line along the edge: most of the row's cells with texture stay, not
    // a few cells of text that happen to look alike.
    const uint64_t* plain = &pend.plain[slot * size_t(words)];
    const uint16_t* was = &pend.cells[slot * size_t(C)];
    const uint16_t* now = cur.cellRow(y);
    const uint16_t* there = cur.cellRow(int(ya));
    std::vector<uint64_t> same(static_cast<size_t>(words), 0);
    int judged = 0, kept = 0;
    for (int w = 0; w < words; ++w)
      for (uint64_t bits = open[w] & ~plain[w]; bits; bits &= bits - 1) {
        const int c = w * 64 + std::countr_zero(bits);
        if ((blkA[w] >> (c & 63)) & 1) continue;
        ++judged;
        if (absDiff(was[c], now[c]) <= tol && absDiff(was[c], there[c]) > tol) {
          ++kept;
          same[size_t(w)] |= 1ull << (c & 63);
        }
      }
    if (kept >= kEdgeLineCells && 10 * kept >= 9 * judged)
      for (int w = 0; w < words; ++w) open[w] &= ~same[size_t(w)];
  }
}

void ScrollTracker::placeFrame(const FrameView& f, Signature& cur, const Signature* ref, int d, long long pos,
                               TrackResult& r) {
  ++frameSerial_;
  compose(f, cur, ref, d, pos, r);
  const bool first = !hasFrame_;
  hasFrame_ = true;
  if (first) {
    // The most common colour of the first screen: the plain colour of what
    // no frame showed clear, where no pinned part says otherwise.
    std::unordered_map<uint32_t, int> count;
    for (int y = 0; y < height_; y += 4)
      for (int x = 0; x < width_; x += 4) ++count[px(f.row(y) + size_t(x) * 4)];
    int best = -1;
    for (const auto& [k, v] : count)
      if (v > best) {
        best = v;
        pageColour_ = k;
      }
    pageCell_ = ((pageColour_ & 0xFFu) + 2u * ((pageColour_ >> 8) & 0xFFu) + ((pageColour_ >> 16) & 0xFFu)) * 16u;
  }
  const bool young = learnedSteps_ < opts_.pinAfterSteps + 1;
  // Nor the first or the last screen: a frame of a move of the whole view
  // shows what stays on the screen out of place.
  if (first || (!viewMoved_ && pos <= topCopy_.pos)) keepCopy(f, cur, topCopy_, pos, first || young, 0);
  // Only parts that start in the lower half are drawn from the bottommost
  // frame (see finish).
  if (first || (!viewMoved_ && pos >= bottomCopy_.pos))
    keepCopy(f, cur, bottomCopy_, pos, first || young, height_ / 2 - opts_.blockRows);
  const int frames = std::max(1, framesSincePlaced_);
  if (!first) velocity_ = double(d) / frames;
  if (!first && d != 0) lastMove_ = d;
  lastPos_ = pos;
  std::swap(last_, work_);
  framesSincePlaced_ = 0;
  skipsSincePlaced_ = 0;
  hintSum_ = 0;
  hintUnc_ = 0;
  hintKnown_ = true;
  r.position = pos;
  r.offset = d;
  r.status = capped_ ? TrackStatus::CapReached : TrackStatus::Placed;
  ++totals_.placed;
  if (capped_) totals_.capReached = true;
}

void ScrollTracker::forgetHints() {
  hintSum_ = 0;
  hintUnc_ = 0;
  hintKnown_ = true;
}

TrackResult ScrollTracker::addFrame(const FrameView& frame, TrackerHint hint) {
  TrackResult r;
  ++totals_.frames;
  // A frame narrower than two cells or lower than two block rows is too small
  // to track (the bottommost frame's copy starts a block row above its middle).
  if (finished_ || !frame.data || frame.width < 2 * opts_.cellPx || frame.height < 2 * opts_.blockRows ||
      (hasFrame_ && (frame.width != width_ || frame.height != height_))) {
    ++totals_.invalid;
    r.status = TrackStatus::Invalid;
    r.state = state_;
    return r;
  }
  if (capped_) {
    r.status = TrackStatus::CapReached;
    r.state = state_ = TrackState::Capped;
    return r;
  }
  if (!hasFrame_) {
    width_ = frame.width;
    height_ = frame.height;
    cells_ = (width_ + opts_.cellPx - 1) / opts_.cellPx;
    const int side = int(std::lround(opts_.sideMarginFraction * width_));
    cellLo_ = std::min(cells_ - 1, (side + opts_.cellPx - 1) / opts_.cellPx);
    cellHi_ = std::max(cellLo_ + 1, (width_ - side) / opts_.cellPx);
    const int n = cellHi_ - cellLo_, B = std::min(opts_.bands, n);
    bandLo_.resize(size_t(B) + 1);
    for (int b = 0; b <= B; ++b) bandLo_[size_t(b)] = cellLo_ + b * n / B;
    blockRowCount_ = (height_ + opts_.blockRows - 1) / opts_.blockRows;
    // A band averages more pixels than a cell, so the same picture drawn
    // again differs less in it: its tolerance is the cell's, scaled by the
    // square root of their widths, and no less than bandTolerance.
    bandTol_ = std::max(opts_.bandTolerance,
                        int(std::lround(opts_.cellTolerance * std::sqrt(double(B) / double(std::max(1, n))))));
    const size_t blocks = size_t(blockRowCount_) * cells_;
    stayed_.assign(blocks, 0);
    wandering_.assign(blocks, 0);
    wanderingMinPos_.assign(blocks, 0);
    wanderingMaxPos_.assign(blocks, 0);
    wanderingEdgePos_.assign(blocks, 0);
    wanderingPage_.assign(blocks, 0);
    wanderingEdge_.assign(blocks, 0);
    pageMoved_.assign(blocks, 0);
    pinned_.assign(blocks, 0);
    closed_.assign(blocks, 0);
    stillNow_.assign(blocks, 0);
    closed16_.assign(blocks, 0);
    blocked_.assign(blocks, 0);
    ghost_.assign(blocks, 0);
    proofAt_.assign(blocks, 0);
    released_.assign(blocks, 0);
    picAt_.assign(blocks, 0);
    pinPic_.assign(size_t(height_) * cells_, 0);
    pinTex_.assign(size_t(height_) * cells_, 0);
    bandPinned16_.assign(size_t(blockRowCount_) * B, 0);
    store_->init(cells_, B);
    applyExcluded();
  }
  computeSignature(frame, opts_.cellPx, bandLo_, opts_.cellTexture, opts_.bandTexture, *work_);
  Signature& cur = *work_;
  r.texturedRows = texturedRowCount(cur);
  ++framesSincePlaced_;
  // A hint further than a few frame heights says nothing the search could
  // use: offsets, their sum and the uncertainty are kept within four frame
  // heights, so that no arithmetic on them overflows.
  const long long room = 4LL * frame.height;
  if (hint.known) {
    hintSum_ = std::clamp(hintSum_ + std::clamp<long long>(hint.offset, -room, room), -room, room);
    hintUnc_ = std::min(room, hintUnc_ + std::clamp<long long>(hint.uncertainty, 0, room));
  } else {
    hintKnown_ = false;
  }
  if (!hasFrame_) {
    placeFrame(frame, cur, nullptr, 0, 0, r);
    prevIsLast_ = true;
    prevPos_ = 0;
    prevHasPos_ = true;
    prevPlaceKnown_ = true;
    prevViewMoved_ = false;
    r.confidence = 1;
    r.state = state_ = TrackState::Following;
    return r;
  }
  const Signature& ref = *last_;
  computeExcluded(cur, ref, lastPos_);
  const bool hinted = hintKnown_;
  const int expected = hinted ? int(hintSum_)
                              : int(std::lround(std::clamp(velocity_ * framesSincePlaced_, -double(room), double(room))));
  const int uncertainty = hinted ? int(hintUnc_ + std::lround(0.08 * std::abs(hintSum_))) + 2
                                 : std::max(6, int(std::lround(0.25 * std::abs(expected))));
  r.expected = expected;
  r.hinted = hinted;
  double share = 0;
  struct ObservedPair {
    long long aPos = 0, bPos = 0;
  };
  std::vector<ObservedPair> observedPairs;
  bool observedMarked = false;
  // The frame is the page drawn anew where it rests (stillFrame's redrawn).
  bool redrawn = false;
  const auto observeOnce = [&](const Signature& a, const Signature& b, long long aPos, long long bPos,
                               const std::vector<Candidate>* cs, int pk, bool verify) {
    if (observedMarked) return false;
    for (const ObservedPair& p : observedPairs)
      if (p.aPos == aPos && p.bPos == bPos) return false;
    observedPairs.push_back({aPos, bPos});
    const bool marked = observeDynamic(a, b, aPos, bPos, cs, pk, verify);
    observedMarked |= marked;
    return marked;
  };
  const auto noMovement = [&](bool still) {
    // A frame that stood still, or in which only a box moved (an inner
    // scroll area under the pointer): the page is where it was and this
    // frame is its latest picture, so that what changes in place (a video,
    // an inner scroll area) is compared with how it looks now, not frames
    // ago. A frame in which nothing could be proved while most of it stood
    // still is not made the reference, and what was expected since the last
    // placed frame stays: if the page did move after all, the next frame is
    // placed against the last placed one, never off by this frame's movement.
    if (prevHasPos_ && prevPos_ == lastPos_ && prevSig())
      observeOnce(cur, *prevSig(), lastPos_, lastPos_, nullptr, -1, !still || redrawn);
    if (still) {
      std::swap(last_, work_);
      prevIsLast_ = true;
      prevPos_ = lastPos_;
      prevHasPos_ = true;
      prevPlaceKnown_ = true;
      prevViewMoved_ = false;
      framesSincePlaced_ = 0;
      skipsSincePlaced_ = 0;
      hintSum_ = 0;
      hintUnc_ = 0;
      hintKnown_ = true;
      velocity_ = 0;
    } else {
      std::swap(prev_, work_);
      prevIsLast_ = false;
      prevPos_ = lastPos_;
      prevHasPos_ = true;
      prevPlaceKnown_ = true;
      prevViewMoved_ = false;
      r.hintsKept = true;
    }
    r.status = TrackStatus::NoMovement;
    r.position = lastPos_;
    r.confidence = share;
    ++totals_.noMovement;
    skipRun_ = 0;
    r.state = state_ = TrackState::Following;
    return r;
  };
  // The page drawn anew where it rests is the same picture: it becomes the
  // reference like a still frame, so the next movement is matched against
  // how the page looks now. Kept as an unproved frame, the next frames (drawn
  // the new way and moving) matched neither it nor the image, and the capture
  // lost the page at its last step (#1537's A-first row on Aurora and Mint).
  // Its changes are no playing video: only a verified look may mark them.
  bool still = stillFrame(cur, ref, &share, &redrawn);
  still = still || redrawn;
  // Still, unless the frame was not quite the same picture and a movement
  // explains it better: content that nearly repeats at the step's distance
  // (a pull request's table of near-equal rows, moved by two of them, 68
  // rows, looked still to 0.97 of its cells; the position stayed and every
  // later frame was off by the step, #1515's acceptance on Cinnamon).
  bool stillish = still && (!hinted || std::abs(expected) <= uncertainty);
  if (stillish && share >= kStillSure) return noMovement(true);
  std::vector<Candidate> cands = searchPair(cur, ref, expected);
  // Only drawn anew, not still at the cell tolerance: a movement that agrees
  // clearly better than standing still is the page's. Soft text scrolled a
  // pixel a frame agrees in place within the redraw tolerance too; made the
  // reference every frame, the steps never added up to a placeable move and
  // the page stood still in the image (#1747's round 11).
  if (redrawn)
    for (const Candidate& c : cands)
      if (c.d != 0 && c.cells >= share + kStillMargin) {
        still = stillish = redrawn = false;
        break;
      }
  bool byExp = false;
  SkipReason why = SkipReason::None;
  int unexplainedPick = -1;
  int pick = decide(cands, expected, uncertainty, hinted, &byExp, &why, &unexplainedPick);
  long long refusedCandPos = 0;
  bool refusedKnown = false;  // refusedCandPos was found in the image (or derives from a place that was)
  bool hasRefusedCand = false;
  // A large playing video (>50% of shown tiles) can lower coarseShare below
  // coarseFloor (0.30) or make decide() reject a strong pair match as
  // unexplained before those page bands are marked dynamic. Retry at minArea
  // against ref (and, when a consecutive skipped frame has a candidate offset,
  // against prev_) so consecutive moving frames establish the offset from the
  // surrounding page, mark the changing bands dynamic, and re-decide (#1537).
  const bool noMatch = why == SkipReason::NoMatch ||
                       (why == SkipReason::NoEvidence && texturedRows_.back() >= 4 * opts_.minEvidenceRows &&
                        texturedRowCount(ref) >= 4 * opts_.minEvidenceRows);
  if (pick < 0 && noMatch && unexplainedPick < 0 && !viewMoved_) {
    std::vector<Candidate> lowCands = searchPair(cur, ref, expected, opts_.minArea);
    bool lb = false;
    SkipReason lw = SkipReason::None;
    int lowUnexp = -1;
    (void)decide(lowCands, expected, uncertainty, hinted, &lb, &lw, &lowUnexp);
    if (lowUnexp >= 0) {
      cands = std::move(lowCands);
      unexplainedPick = lowUnexp;
    }
  }
  if (pick < 0 && noMatch && unexplainedPick >= 0) {
    const int ud = cands[size_t(unexplainedPick)].d;
    const long long upos = lastPos_ + ud;
    const bool usePrev = prevHasPos_ && prevSig();
    const Signature* other = usePrev ? prevSig() : &ref;
    const long long otherPos = usePrev ? prevPos_ : lastPos_;
    const bool otherViewMoved = usePrev ? prevViewMoved_ : viewMoved_;
    // Not once the page is lost (here and in the two retries for a refused
    // place below): the page may be anywhere by then, and the candidate is a
    // guess that agrees only as look-alikes do (a pull request's tables of
    // near-equal rows). Its marks repainted verified text with what a later
    // frame showed over those rows: GitHub's sticky header over a commit
    // hash, once the page was found again (#1795). A playing video is met
    // while following, as the retry at rest below has it.
    if (!otherViewMoved && skipRun_ < opts_.lostAfterSkips &&
        observeOnce(cur, *other, upos, otherPos, &cands, unexplainedPick, true)) {
      computeExcluded(cur, ref, lastPos_);
      cands = searchPair(cur, ref, expected);
      pick = decide(cands, expected, uncertainty, hinted, &byExp, &why);
      if (pick < 0 && why == SkipReason::NoMatch) {
        refusedCandPos = upos;
        hasRefusedCand = true;
      }
    } else {
      refusedCandPos = upos;
      hasRefusedCand = true;
    }
  } else if (pick < 0 && noMatch && unexplainedPick < 0 && !prevViewMoved_ && prevHasPos_ && !prevIsLast_ && prevSig()) {
    computeExcluded(cur, *prevSig(), prevPos_);
    std::vector<Candidate> prevCands = searchPair(cur, *prevSig(), 0, opts_.minArea);
    bool pb = false;
    SkipReason pw = SkipReason::None;
    int prevUnexp = -1;
    const int prevPick = decide(prevCands, 0, uncertainty, false, &pb, &pw, &prevUnexp);
    const int pIdx = prevPick >= 0 ? prevPick : prevUnexp;
    bool marked = false;
    long long upos = 0;
    if (pIdx >= 0) {
      upos = prevPos_ + prevCands[size_t(pIdx)].d;
      refusedKnown = prevPlaceKnown_;
      // Once lost, only from a place the image lookup found (#1795).
      if (skipRun_ < opts_.lostAfterSkips || prevPlaceKnown_)
        marked = observeOnce(cur, *prevSig(), upos, prevPos_, &prevCands, pIdx, true);
    }
    computeExcluded(cur, ref, lastPos_);
    if (marked) {
      cands = searchPair(cur, ref, expected);
      pick = decide(cands, expected, uncertainty, hinted, &byExp, &why);
      if (pick < 0 && why == SkipReason::NoMatch) {
        refusedCandPos = upos;
        hasRefusedCand = true;
      }
    } else if (pIdx >= 0) {
      refusedCandPos = upos;
      hasRefusedCand = true;
    }
  }
  // Content that repeats at the step's distance: what changed moved by d,
  // and what did not change looks the same where it was and where d puts it
  // (two rows of a table of near-equal rows), so it does not vote and the
  // movement covers too little of the frame to be accepted; the next frame,
  // the same picture as the reference, then looked still, and the page's
  // place stayed behind by d. A movement that the whole overlap agrees with,
  // every textured cell outside what is pinned, is the page's.
  // Also over a pick that agrees clearly less (another layer, a look-alike
  // of the moved rows that the coverage let through).
  // It must agree with the whole overlap in kWholeShare, and leave at most
  // half the disagreement that standing still leaves.
  // Only without the driver's hint: auto-scroll's hint chooses among look-
  // alikes itself, and a ruler's near-equal numbered rows must not be taken
  // for a move by their period.
  const auto wholeAgrees = [&](int d) {
    if (hinted) return false;
    const double w = wholeCells(cur, ref, d);
    return w >= kWholeShare && w > share && 1.0 - w <= 0.5 * (1.0 - share);
  };
  const auto wholeRescue = [&](int over) {
    int whole = -1;
    for (size_t i = 0; i < cands.size(); ++i) {
      const Candidate& c = cands[i];
      if (c.accepted || c.cells < kWholeCells || c.bands < kWholeCells || c.rows < opts_.minEvidenceRows) continue;
      if (over >= 0 && c.cells < cands[size_t(over)].cells + kStillMargin) continue;
      if (whole >= 0 && c.cells <= cands[size_t(whole)].cells) continue;
      if (wholeAgrees(c.d)) whole = int(i);
    }
    return whole;
  };
  bool wholeMove = false;
  if (why != SkipReason::Ambiguous && (!still || stillish)) {
    const int whole = wholeRescue(stillish ? -1 : pick);
    if (whole >= 0) {
      pick = whole;
      why = SkipReason::None;
      wholeMove = true;
    }
  }
  if (stillish && !wholeMove && (pick < 0 || cands[size_t(pick)].cells < share + kStillMargin)) return noMovement(true);
  // A page that repeats (a pull request's tables, one every 1,010 rows) fits
  // the pair at its place and a period away. Turning back more than half a
  // screen from one frame to the next, against the way the page was going,
  // is the look-alike when the pair also fits, nearly as well, at a place
  // that goes on the way the page went: GitHub's page, moved on 414 rows
  // after a pause, was placed 596 rows back at 0.987 of its cells against
  // 0.967 for the right place, and every later frame was a table short
  // (#1797). Not on the driver's hint, which says where the page is.
  if (pick >= 0 && !hinted && skipRun_ == 0 && lastMove_ != 0) {
    const Candidate& p = cands[size_t(pick)];
    if ((p.d > 0) != (lastMove_ > 0) && 2 * std::abs(p.d) > height_) {
      int ahead = -1;
      for (size_t i = 0; i < cands.size(); ++i) {
        const Candidate& c = cands[i];
        if (!c.accepted || c.d == 0 || (c.d > 0) != (lastMove_ > 0) || c.cells < opts_.recoverCells ||
            c.cells < p.cells - opts_.recoverTie)
          continue;
        // The page, not a part at a pace of its own: it moves at least as
        // much of the frame as the turn back does.
        if (c.movingTiles < p.movingTiles) continue;
        if (ahead < 0 || c.cells > cands[size_t(ahead)].cells) ahead = int(i);
      }
      if (ahead >= 0) pick = ahead;
    }
  }

  // Nothing to compare only when there is nothing: a frame and a reference
  // that both show enough are no blank stretch, whatever the search found
  // (a jump past what the last frame showed is no match, and is looked up).
  if (why == SkipReason::NoEvidence && texturedRows_.back() >= 4 * opts_.minEvidenceRows &&
      texturedRowCount(ref) >= 4 * opts_.minEvidenceRows)
    why = SkipReason::NoMatch;
  long long pos = 0;
  int d = 0;
  bool recovered = false;
  // Most of the frame stood still and what moved does not span it: an inner
  // scroll area under the pointer (a page's code block), not the page.
  // (Not a movement the whole overlap agrees with: that is no box. A box's
  // page stood still around it, and disagrees where the box's move puts it;
  // a table of near-equal rows moved by two of them agrees.)
  if (pick >= 0 && !wholeMove && share >= opts_.stillFallback && boxed(cands[size_t(pick)]) && !wholeAgrees(cands[size_t(pick)].d) &&
      cands[size_t(pick)].coverage < (share >= opts_.stillMostly ? opts_.stillCoverageMostly : opts_.stillCoverage))
    return noMovement(true);
  // The image's say on a place. Lenient while following (what changes in
  // place is taken as first seen); after skipped frames the frame must show
  // what the image holds there (recoverKeepShare): the page may be anywhere
  // by then, and content that only looks like the image's (paragraphs past
  // a blank stretch that differ from those above it in their numbers) was
  // "found again" among them (#1515's acceptance, Cinnamon and Hyprland).
  // And the cells of its own layer must agree nearly all (recoverCells,
  // within the tiles that move with it: another layer of the page, a side bar
  // at its own pace, disagrees anyway): bands cannot tell such look-alikes
  // apart, their few differing cells can.
  // And a third of its rows at least must lie inside the image: an overlap
  // of a few rows at the image's edge agrees all too easily. The same for a
  // place the image lookup finds: the frame is not where the last one says,
  // so it is found again wherever it is.
  // None of this where the driver's hint says where the page is (auto-scroll
  // knows its steps): the hint chooses among look-alikes, as before (the
  // ruler page's equal rows after one skipped frame of a step).
  const auto hintSays = [&](int at) { return hinted && std::abs(at - expected) <= uncertainty; };
  const auto refused = [&](const Candidate& c, bool found) {
    const bool strict = (found || skipRun_ > 0) && !hintSays(c.d);
    const long long at = lastPos_ + c.d, inside = std::min(bottom_, at + height_) - std::max(top_, at);
    // A place the image lookup finds is judged against the image as a whole
    // (it holds the page's layers as placed), the pair's against its own layer.
    // A frame placed where the image holds every row of it shows content the
    // image has: it must agree as a place found again does (a look-alike 579
    // rows back, at 0.88 of its cells, while the page went down, left a
    // GitHub pull request 952 rows short on Cinnamon).
    const bool known = at >= top_ && at + height_ <= bottom_;
    return (strict && ((found ? c.cells : c.cellsKept) < opts_.recoverCells || 3 * inside < height_)) ||
           (known && !hintSays(c.d) && c.cells < opts_.recoverCells) ||
           contradicted(cur, at, strict ? std::max(opts_.keepShare, opts_.recoverKeepShare * c.area) : opts_.keepShare);
  };
  // The pair's place, also when the image's check refuses it: the image
  // lookup that finds the same place again confirms it, as before.
  int pairD = INT_MIN;
  if (pick >= 0) {
    d = cands[size_t(pick)].d;
    pos = lastPos_ + d;
    pairD = d;
    if (refused(cands[size_t(pick)], false)) {
      bool retried = false;
      const bool usePrev = prevHasPos_ && prevSig();
      const Signature* other = usePrev ? prevSig() : &ref;
      const long long otherPos = usePrev ? prevPos_ : lastPos_;
      const bool otherViewMoved = usePrev ? prevViewMoved_ : viewMoved_;
      if (!otherViewMoved && skipRun_ < opts_.lostAfterSkips &&
          observeOnce(cur, *other, pos, otherPos, &cands, pick, true)) {
        refreshRefIncl(lastPos_);
        Tally tb, tc;
        bandTally(cur, ref, d, tb);
        cellTally(cur, ref, d, tc);
        Candidate refreshed;
        refreshed.d = d;
        judge(refreshed, tb, tc, std::max(0, -d), std::min(height_, height_ - d));
        if (refreshed.accepted && !refused(refreshed, false)) {
          cands[size_t(pick)] = std::move(refreshed);
          retried = true;
        }
      }
      if (!retried) {
        refusedCandPos = pos;
        hasRefusedCand = true;
        refusedKnown = false;
        pick = -1;
        why = SkipReason::Contradicted;
      }
    }
  }
  // Found again by the pair, against the last frame placed before the
  // skipped ones: the page may have gone anywhere since, and the pair sees
  // only offsets near that frame. The place must be the only one in the image
  // that fits (look-alike paragraphs below a blank stretch matched one
  // paragraph above it in the pair's window, and several others in the image).
  // The pair's own other offsets are look-alikes too, and the only ones to
  // be seen while the image holds nothing written yet (the first frames'
  // cells wait until the mask has learned a few steps).
  // Of the same layer: moving mostly the same tiles (another layer, a side
  // bar at its own pace, is no look-alike of the page).
  const auto sameLayer = [](const Candidate& a, const Candidate& b) {
    int both = 0;
    for (size_t t = 0; t < a.moving.size() && t < b.moving.size(); ++t) both += a.moving[t] == 1 && b.moving[t] == 1;
    return 2 * both >= std::min(a.movingTiles, b.movingTiles);
  };
  const auto pairLookAlike = [&] {
    const Candidate& p = cands[size_t(pick)];
    for (const Candidate& c : cands)
      if (c.accepted && std::abs(c.d - d) >= kSamePlaceRows && c.cells >= p.cells - opts_.recoverTie && sameLayer(c, p)) return true;
    return false;
  };
  // The pair's other accepted places: where the image fits the frame better
  // at one of them, that is another layer of the frame moving at its own
  // pace (a side bar on MDN fills much of it), not a look-alike.
  std::vector<long long> pairPlaces;
  for (const Candidate& c : cands)
    if (c.accepted) pairPlaces.push_back(lastPos_ + c.d);
  // And without the driver's hint, after skipped frames the page may have
  // moved by anything, also past the reference: where the frame or its
  // reference repeats itself (a pull request's tables of equal rows, 505
  // rows apart) about as well as the pair matches, the offset is known only
  // modulo that period, and the pair finds the look-alike one or two
  // periods short of a jump past the reference (#1515's re-check on
  // Cinnamon: 1,010 rows missing after a place "found again" at 0.80).
  // Still lost: scrolled back, the image lookup finds the place.
  // Compared on the rows the pair compared: the frame's [max(0, -d),
  // min(H, H - d)), the reference's those plus d.
  if (pick >= 0 && skipsSincePlaced_ > 0 && !hinted) {
    const int dd = cands[size_t(pick)].d, y0 = std::max(0, -dd), y1 = std::min(height_, height_ - dd);
    if (std::max(selfRepeat(cur, y0, y1, pos), selfRepeat(ref, y0 + dd, y1 + dd, lastPos_)) >= cands[size_t(pick)].bands - kRepeatMargin) {
      pick = -1;
      why = SkipReason::Ambiguous;
      pairD = INT_MIN;  // the image lookup does not confirm it either: there it is a place found again
    }
  }
  if (pick >= 0 && skipRun_ > 0 && !hintSays(d) && (pairLookAlike() || lookAlike(cur, pos, pairPlaces))) {
    pick = -1;
    why = SkipReason::Ambiguous;
  }
  // Nothing moved as the page, and most of the frame stood still: the page
  // did not move, and something changed in place (a video, an inner scroll
  // area, an animation). Not a lookup: a reader who jumped leaves nothing
  // where it was. So also when the frame stood still by stillFrame's rule
  // for a frame with next to nothing textured (a blank page end under a
  // header and a footer, one small cell changing: a spinner, a caret), where
  // the share of textured cells says nothing: such a frame is never placed
  // on the hint, so a page that rests there can end.
  // At rest over a large playing video (>50% of textured cells): share fell
  // below stillFallback, but consecutive frames show the rest of the frame
  // standing strictly still against both ref and prev while the video keeps
  // changing (#1537). Require share >= minArea against ref first so a foreign
  // page with a playing video never marks the real page's rows dynamic.
  if (pick < 0 && why != SkipReason::Ambiguous && !still && skipRun_ < opts_.lostAfterSkips &&
      share < opts_.stillFallback && share >= opts_.minArea) {
    if (prevHasPos_ && prevPos_ == lastPos_ && prevSig() &&
        observeOnce(cur, *prevSig(), lastPos_, lastPos_, nullptr, -1, true)) {
      still = stillFrame(cur, ref, &share);
      if (still) return noMovement(true);
    }
    if (!hasRefusedCand) {
      refusedCandPos = lastPos_;
      hasRefusedCand = true;
      refusedKnown = true;
    }
  }
  if (pick < 0 && why != SkipReason::Ambiguous && (share >= opts_.stillFallback || still)) return noMovement(false);
  if (pick < 0 && why != SkipReason::NoEvidence) {
    // Not where the last frame says: look in the image (the user scrolled
    // back), within three screens of where it was expected; then, from the
    // second frame skipped on, in one slice of the rest of the image, the
    // next one for the next frame. The work per frame stays bounded however
    // long the image (a lost frame comes every frame while a person looks
    // for the place), and the whole image is looked through within a few.
    const long long expPos = lastPos_ + expected;
    std::vector<Candidate> found = searchStore(cur, expPos, expPos - 3LL * height_, expPos + 3LL * height_, 4);
    bool fb = false;
    SkipReason fw = SkipReason::None;
    int fp = decide(found, expected, uncertainty, hinted, &fb, &fw);
    if (fp < 0 && fw != SkipReason::Ambiguous && skipRun_ >= 1) {
      const long long lo = top_ - height_, hi = bottom_;
      if (sweep_ < lo || sweep_ > hi) sweep_ = lo;
      found = searchStore(cur, expPos, sweep_, sweep_ + kSweepRows - 1, 2);
      sweep_ += kSweepRows;
      fp = decide(found, expected, uncertainty, hinted, &fb, &fw);
    }
    // What the image lookup finds elsewhere than the pair's place, or after
    // skipped frames, is checked as a place found again: by the strict check.
    // The pair's own place found again in the image is taken, as it was: the
    // pair agreed in every cell, and a part of the image written by a layer
    // of its own (a side bar at its own pace) made the check refuse it.
    const bool elsewhere = fp >= 0 && (pairD == INT_MIN || std::abs(found[size_t(fp)].d - pairD) >= kSamePlaceRows);
    if (fp >= 0 && (elsewhere || skipRun_ > 0) && !hintSays(found[size_t(fp)].d) && refused(found[size_t(fp)], true)) {
      bool retried = false;
      const long long fpos = lastPos_ + found[size_t(fp)].d;
      if (prevHasPos_ && prevSig() && !prevViewMoved_ && skipRun_ < opts_.lostAfterSkips &&
          observeOnce(cur, *prevSig(), fpos, prevPos_, &found, fp, true)) {
        refreshRefIncl(lastPos_);
        Tally tb, tc;
        std::vector<uint16_t> sigs;
        std::vector<uint8_t> have;
        storeCellSigs(fpos, height_, sigs, have);
        storeCellTally(cur, fpos, sigs, have, fpos, tc);
        storeBandTally(cur, fpos, 1, tb);
        Candidate refreshed;
        refreshed.d = found[size_t(fp)].d;
        judge(refreshed, tb, tc, int(std::clamp<long long>(top_ - fpos, 0, height_)),
              int(std::clamp<long long>(bottom_ - fpos, 0, height_)));
        if (refreshed.accepted && !refused(refreshed, true)) {
          found[size_t(fp)] = std::move(refreshed);
          retried = true;
        }
      }
      if (!retried) {
        if (!hasRefusedCand) {
          refusedCandPos = fpos;
          hasRefusedCand = true;
          refusedKnown = true;
        }
        fp = -1;
        fw = SkipReason::Contradicted;
      }
    }
    // A place found elsewhere in the image must be the only one there that fits.
    if (fp >= 0 && (elsewhere || skipRun_ > 0) && !hintSays(found[size_t(fp)].d) && lookAlike(cur, lastPos_ + found[size_t(fp)].d, {})) {
      fp = -1;
      fw = SkipReason::Ambiguous;
    }
    if (fp >= 0) {
      pick = fp;
      d = found[size_t(fp)].d;
      pos = lastPos_ + d;
      byExp = fb;
      recovered = true;
      cands = found;
    } else if (why == SkipReason::None || fw == SkipReason::Ambiguous) {
      why = fw;
    }
  }
  // Nothing to compare (a blank stretch): the driver's hint bridges it, where
  // the frame placed there overlaps what the image holds (else it would leave
  // rows no frame showed, and one wild hint could take the image to its cap)
  // and what the image already has there does not say otherwise.
  if (pick < 0 && why == SkipReason::NoEvidence && hinted && lastPos_ + expected + effTop_ < bottom_ &&
      lastPos_ + expected + effBottom_ > top_ && !contradicted(cur, lastPos_ + expected, opts_.keepShare)) {
    d = expected;
    pos = lastPos_ + d;
    r.onHint = true;
    ++totals_.onHint;
    pick = 0;
  }
  if (pick < 0) {
    if (hasRefusedCand) {
      std::swap(prev_, work_);
      prevIsLast_ = false;
      prevPos_ = refusedCandPos;
      prevHasPos_ = true;
      prevPlaceKnown_ = refusedKnown;
      prevViewMoved_ = false;
    } else {
      prevHasPos_ = false;
      prevViewMoved_ = false;
    }
    r.status = TrackStatus::Skipped;
    r.reason = why;
    r.position = lastPos_;
    ++totals_.skipped;
    ++skipRun_;
    ++skipsSincePlaced_;
    totals_.longestSkipRun = std::max(totals_.longestSkipRun, skipRun_);
    r.state = state_ = skipRun_ >= opts_.lostAfterSkips ? TrackState::Lost : state_;
    return r;
  }
  r.confidence = r.onHint ? 0.0 : cands[size_t(pick)].cells;
  r.byExpectation = byExp;
  totals_.byExpectation += byExp;
  (void)recovered;
  r.recovered = skipRun_ > 0;
  totals_.recovered += r.recovered;
  const long long prevLastPos = lastPos_;
  followPinned(cur, ref, d);
  learn(cur, ref, d);
  placeFrame(frame, cur, &ref, d, pos, r);
  if (d != 0 && !viewMoved_ && !r.onHint) observeOnce(*last_, *work_, pos, prevLastPos, &cands, pick, false);
  if (!viewMoved_ && !r.onHint && hasDyn_) snapshotDynamic(frame, pos, *last_, *work_, d);
  prevIsLast_ = true;
  prevPos_ = pos;
  prevHasPos_ = true;
  prevPlaceKnown_ = true;
  prevViewMoved_ = viewMoved_;
  skipRun_ = 0;
  r.state = state_ = capped_ ? TrackState::Capped : TrackState::Following;
  return r;
}

// ------------------------------------------------------------------ finish


TrackTotals ScrollTracker::finish() {
  if (finished_) return totals_;
  finished_ = true;
  totals_.top = top_;
  totals_.bottom = bottom_;
  if (!hasFrame_) return totals_;
  const int C = cells_, R = blockRowCount_, P = opts_.cellPx, BR = opts_.blockRows;
  closeMask();
  {
    // The last frame's cells seen once: as seen, unless pinned.
    std::vector<uint64_t> blocked;
    std::fill(stillNow_.begin(), stillNow_.end(), 0);
    blocked_ = closed_;
    blockedWords(blocked);
    std::vector<uint64_t> area;
    std::vector<uint64_t> near;
    pageArea(area, &near);
    flushPending(blocked, area, near, true);
  }
  // The pinned parts: connected regions of the closed mask, and where parts
  // that came loose were pinned (drawn once like the rest).
  std::vector<uint8_t> drawMask = closed_;
  for (size_t i = 0; i < drawMask.size() && i < released_.size(); ++i) drawMask[i] |= released_[i];
  std::vector<int> label(size_t(R) * C, -1);
  struct Part {
    int r0 = 1 << 30, r1 = -1, c0 = 1 << 30, c1 = -1;
    long long blocks = 0;
    bool splitOff = false;  // split from the part it touched, where the width jumps
    int pieceOf = -1;       // a piece of the split-off part this is close under
  };
  std::vector<Part> parts;
  std::vector<int> stack;
  for (int i = 0; i < R * C; ++i) {
    if (!drawMask[size_t(i)] || label[size_t(i)] >= 0) continue;
    const int id = int(parts.size());
    parts.push_back({});
    label[size_t(i)] = id;
    stack.push_back(i);
    while (!stack.empty()) {
      const int j = stack.back();
      stack.pop_back();
      const int r = j / C, c = j % C;
      Part& part = parts.back();
      part.r0 = std::min(part.r0, r);
      part.r1 = std::max(part.r1, r);
      part.c0 = std::min(part.c0, c);
      part.c1 = std::max(part.c1, c);
      ++part.blocks;
      const int nb[4][2] = {{r - 1, c}, {r + 1, c}, {r, c - 1}, {r, c + 1}};
      for (const auto& q : nb) {
        if (q[0] < 0 || q[0] >= R || q[1] < 0 || q[1] >= C) continue;
        const int k = q[0] * C + q[1];
        if (drawMask[size_t(k)] && label[size_t(k)] < 0) {
          label[size_t(k)] = id;
          stack.push_back(k);
        }
      }
    }
  }
  totals_.pinnedRegions = int(parts.size());
  // A part whose width jumps from one block row to the next is two things
  // that touch: a cookie box over the bottom of a side bar's column sticks
  // out of the side bar (Stack Overflow, 18 blocks wide to 53). From the row
  // where the width doubles or halves, by at least 16 blocks, the rows are a
  // part of their own (a list's items of other lengths are not).
  {
    const size_t n = parts.size();
    for (size_t i = 0; i < n; ++i) {
      int id = int(i), prevW = -1;
      for (int r = parts[i].r0; r <= parts[i].r1; ++r) {
        int a = C, b = -1;
        for (int c = parts[i].c0; c <= parts[i].c1; ++c)
          if (label[size_t(r) * C + c] == int(i)) {
            a = std::min(a, c);
            b = std::max(b, c);
          }
        if (b < 0) {
          prevW = -1;
          continue;
        }
        const int w = b - a + 1;
        const int wide = std::max(w, prevW), narrow = std::min(w, prevW);
        if (prevW > 0 && wide >= 2 * narrow && wide - narrow >= 16) {
          id = int(parts.size());
          parts.push_back({});
          parts.back().splitOff = true;
        }
        prevW = w;
        if (id == int(i)) continue;
        for (int c = a; c <= b; ++c)
          if (label[size_t(r) * C + c] == int(i)) label[size_t(r) * C + c] = id;
      }
    }
    if (parts.size() > n) {
      for (Part& part : parts) {
        const bool splitOff = part.splitOff;
        part = Part{};
        part.splitOff = splitOff;
      }
      for (int i = 0; i < R * C; ++i) {
        const int l = label[size_t(i)];
        if (l < 0) continue;
        Part& part = parts[size_t(l)];
        part.r0 = std::min(part.r0, i / C);
        part.r1 = std::max(part.r1, i / C);
        part.c0 = std::min(part.c0, i % C);
        part.c1 = std::max(part.c1, i % C);
        ++part.blocks;
      }
    }
  }
  // Parts that share columns and are close one above the other are one
  // pinned thing (the items of a side bar's list, with flat rows between
  // them): a group, anchored by its top edge. What starts in the top half of
  // the screen is pinned at the top and is in the image once, as the topmost
  // frame shows it; what starts lower (a floating box, a cookie bar) is
  // pinned at the bottom, as the bottommost frame shows it.
  std::vector<int> group(parts.size());
  std::iota(group.begin(), group.end(), 0);
  const auto find = [&](int i) {
    while (group[size_t(i)] != i) i = group[size_t(i)] = group[size_t(group[size_t(i)])];
    return i;
  };
  // The pieces of a thing split off in the lower half: parts close under it
  // and within its columns (a cookie banner's line of text and its buttons,
  // under its plain top), however narrow, are kept apart with it too.
  {
    std::vector<size_t> order(parts.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return parts[a].r0 < parts[b].r0; });
    for (size_t j : order) {
      Part& piece = parts[j];
      if (piece.splitOff || piece.r0 * BR * 2 < height_) continue;
      for (size_t i : order) {
        const Part& above = parts[i];
        if (i == j || !above.splitOff || above.r0 >= piece.r0 || above.r0 * BR * 2 < height_) continue;
        if (piece.c0 >= above.c0 && piece.c1 <= above.c1 && piece.r0 - above.r1 - 1 <= R / 4) {
          piece.pieceOf = int(i);
          break;
        }
      }
    }
  }
  for (size_t a = 0; a < parts.size(); ++a)
    for (size_t b = a + 1; b < parts.size(); ++b) {
      const Part& A = parts[a];
      const Part& Bp = parts[b];
      const int overlap = std::min(A.c1, Bp.c1) - std::max(A.c0, Bp.c0) + 1;
      const int narrower = std::min(A.c1 - A.c0, Bp.c1 - Bp.c0) + 1;
      const int gap = std::max(A.r0, Bp.r0) - std::min(A.r1, Bp.r1) - 1;
      // A part split off where the width jumped, that starts in the lower
      // half, is a thing of its own, not the next item of what is above it
      // (the cookie box under a side bar, an advertisement bar under a
      // column); items of one list, wide and narrow, are separate parts and
      // keep together.
      const Part& upper = A.r0 <= Bp.r0 ? A : Bp;
      const Part& lower = &upper == &A ? Bp : A;
      const int wu = upper.c1 - upper.c0 + 1, wl = lower.c1 - lower.c0 + 1;
      // So is one much wider than the part above it (a cookie banner under a
      // column of advertisements that sticks): twice as wide and 16 blocks.
      const bool apart =
          lower.r0 * BR * 2 >= height_ &&
          (lower.splitOff || wl >= 2 * wu + 16 || (lower.pieceOf >= 0 && upper.r0 < parts[size_t(lower.pieceOf)].r0));
      // Items of a side bar's list are one thing however far apart (the
      // items between them may not have been learned): narrow parts, under a
      // third of the width, with their left or right edges aligned.
      const bool list = std::max(wu, wl) * 3 < C && (std::abs(A.c0 - Bp.c0) <= 2 || std::abs(A.c1 - Bp.c1) <= 2);
      if (overlap * 2 >= narrower && (gap <= R / 4 || list) && !apart) group[size_t(find(int(a)))] = find(int(b));
    }
  std::vector<int> groupTop(parts.size(), R);
  for (size_t i = 0; i < parts.size(); ++i) groupTop[size_t(find(int(i)))] = std::min(groupTop[size_t(find(int(i)))], parts[i].r0);
  std::vector<uint8_t> topAnchored(parts.size());
  for (size_t i = 0; i < parts.size(); ++i) topAnchored[i] = groupTop[size_t(find(int(i)))] * BR * 2 < height_;
  // A pinned thing is in the image as a rectangle: its group's bounding
  // box of blocks, so that the ragged edge of a side bar's list (an item's
  // text a block longer than the mask) comes with it. Inside the screen that
  // shows it once, the rest of the box is what that screen showed there.
  std::vector<int> boxOf(size_t(R) * C, -1);
  {
    std::vector<Part> box(parts.size());
    for (size_t i = 0; i < parts.size(); ++i) {
      Part& b = box[size_t(find(int(i)))];
      b.r0 = std::min(b.r0, parts[i].r0);
      b.r1 = std::max(b.r1, parts[i].r1);
      b.c0 = std::min(b.c0, parts[i].c0);
      b.c1 = std::max(b.c1, parts[i].c1);
    }
    for (size_t i = 0; i < parts.size(); ++i)
      for (int r = parts[i].r0; r <= parts[i].r1; ++r)
        for (int c = parts[i].c0; c <= parts[i].c1; ++c)
          if (label[size_t(r) * C + c] == int(i)) boxOf[size_t(r) * C + c] = find(int(i));
    // Widened by a block, as the mask is: the last item's descenders.
    for (size_t g = 0; g < box.size(); ++g)
      if (find(int(g)) == int(g))
        for (int r = std::max(0, box[g].r0 - 1); r <= std::min(R - 1, box[g].r1 + 1); ++r)
          for (int c = std::max(0, box[g].c0 - 1); c <= std::min(C - 1, box[g].c1 + 1); ++c)
            if (boxOf[size_t(r) * C + c] < 0) boxOf[size_t(r) * C + c] = int(g);
  }
  // Where the first or the last screen shows each pinned thing (#1538): a
  // column that sticks lies lower on the first screen than where it stuck
  // (the page lays it out there), and is drawn from there, whole. Its picture
  // is what its blocks showed while pinned (and the pictures of parts that
  // came loose); where the screen shows it at its pinned place, or nowhere
  // else for sure, it is drawn at its pinned place.
  // A copy's cells (as the signature counts them), where it holds them; -1 elsewhere.
  const auto cellsOf = [&](const FrameCopy& copy) {
    std::vector<int> out(size_t(height_) * C, -1);
    if (copy.tag.empty()) return out;
    for (int y = 0; y < height_; ++y)
      for (int c = 0; c < C; ++c) {
        if (copy.tag[size_t(std::min(y / BR, R - 1)) * C + c] != copy.serial) continue;
        const int x0 = c * P, x1 = std::min(width_, x0 + P);
        unsigned sum = 0;
        for (int x = x0; x < x1; ++x) {
          const uint8_t* q = &copy.px[(size_t(y) * width_ + x) * 4];
          sum += unsigned(q[0]) + 2u * q[1] + q[2];
        }
        out[size_t(y) * C + c] = int(sum * 16u / unsigned(x1 - x0));
      }
    return out;
  };
  std::vector<int> topCells, bottomCells;
  std::vector<int> shiftOf(parts.size(), 0);
  {
    std::vector<std::vector<int>> ys(parts.size()), cs(parts.size());
    std::vector<std::vector<uint16_t>> vs(parts.size());
    const auto add = [&](int y, int c, uint16_t v) {
      if (y < 0 || y >= height_ || c < 0 || c >= C) return;
      const int l = label[size_t(std::min(y / BR, R - 1)) * C + c];
      if (l < 0) return;
      const size_t g = size_t(find(l));
      ys[g].push_back(y);
      cs[g].push_back(c);
      vs[g].push_back(v);
    };
    for (int i = 0; i < R * C; ++i) {
      if (!pinned_[size_t(i)] || !picAt_[size_t(i)]) continue;
      const int br = i / C, c = i % C;
      for (int y = br * BR; y < std::min(height_, (br + 1) * BR); ++y)
        if (pinTex_[size_t(y) * C + c]) add(y, c, pinPic_[size_t(y) * C + c]);
    }
    for (const Loose& part : releasedPics_)
      for (size_t k = 0; k < part.values.size(); ++k) add(part.ys[k], part.cs[k], part.values[k]);
    const unsigned tol = unsigned(opts_.cellTolerance);
    for (size_t g = 0; g < parts.size(); ++g) {
      if (find(int(g)) != int(g) || int(vs[g].size()) < kLooseCells) continue;
      const bool topSide = topAnchored[g];
      std::vector<int>& cells = topSide ? topCells : bottomCells;
      if (cells.empty()) cells = cellsOf(topSide ? topCopy_ : bottomCopy_);
      // Rows counted once each (the samples go cell by cell).
      std::vector<int> rowStamp(size_t(height_), 0);
      int stamp = 0;
      const auto shareAt = [&](int s, int* compared, int* rows) {
        int agree = 0, seen = 0;
        *rows = 0;
        ++stamp;
        for (size_t k = 0; k < vs[g].size(); ++k) {
          const int y = ys[g][k] + s;
          if (y < 0 || y >= height_) continue;
          const int v = cells[size_t(y) * C + cs[g][k]];
          if (v < 0) continue;
          ++seen;
          agree += absDiff(unsigned(v), vs[g][k]) <= tol;
          if (rowStamp[size_t(y)] != stamp) {
            rowStamp[size_t(y)] = stamp;
            ++*rows;
          }
        }
        *compared = seen;
        return seen ? double(agree) / seen : 0.0;
      };
      int compared = 0, rows = 0;
      if (shareAt(0, &compared, &rows) >= kInPlace && compared >= kLooseCells) continue;
      double best = 0;
      for (int s = -height_ + 1; s < height_; ++s) {
        const double share = shareAt(s, &compared, &rows);
        if (share < kLooseShare || share <= best || compared < kLooseCells || rows < opts_.minEvidenceRows) continue;
        best = share;
        shiftOf[g] = s;
      }
    }
  }
  // Per block of the first and the last screen, the pinned group drawn there.
  std::vector<int> drawnTop(size_t(R) * C, -1), drawnBottom(size_t(R) * C, -1);
  auto overlay = [&](const FrameCopy& copy, bool topSide) {
    const long long pos = copy.pos;
    std::vector<int>& drawn = topSide ? drawnTop : drawnBottom;
    for (int r = 0; r < R; ++r)
      for (int k = 0; k < BR && r * BR + k < height_; ++k)
        for (int c = 0; c < C;) {
          const int g = boxOf[size_t(r) * C + c];
          if (g < 0 || bool(topAnchored[size_t(g)]) != topSide) {
            ++c;
            continue;
          }
          // The copy's row that shows this row of the thing.
          const int y = r * BR + k + shiftOf[size_t(g)];
          const auto take = [&](int cc) {
            return boxOf[size_t(r) * C + cc] == g && y >= 0 && y < height_ &&
                   copy.tag[size_t(std::min(y / BR, R - 1)) * C + cc] == copy.serial;
          };
          if (!take(c)) {
            ++c;
            continue;
          }
          int e = c;
          while (e < C && take(e)) drawn[size_t(std::min(y / BR, R - 1)) * C + e++] = g;
          const long long p = pos + y;
          if (p >= top_ && p < bottom_) {
            const int x0 = c * P, x1 = std::min(width_, e * P);
            canvas_.writeSpan(p, x0, x1, &copy.px[(size_t(y) * width_ + x0) * 4]);
            for (int q = c; q < e; ++q) store_->set(p, q, 1);
          }
          c = e;
        }
  };
  overlay(topCopy_, true);
  overlay(bottomCopy_, false);
  // What no frame showed clear: within the first or the last screen as that
  // screen showed it (a header's edge that changed when it began to cast a
  // shadow); elsewhere a plain colour, the background of the pinned part in
  // that column (a side bar as tall as the screen), else the most common
  // colour of the first screen.
  // A pinned thing's own plain colour as the screen shows it, joined to it
  // (#1538): an advertisement's plain colour below its artwork, a banner's
  // sides. They are its, not the page's: drawn from the screen it is drawn
  // from, never from the other.
  const auto ownersIn = [&](const FrameCopy& copy, std::vector<int>& cells) {
    const unsigned tol = unsigned(opts_.cellTolerance);
    std::vector<int> owner = label;
    if (cells.empty()) cells = cellsOf(copy);
    const int strict = int(tol / 8);
    // A block's one colour down its rows in the copy, or -1.
    const auto even = [&](int i) {
      const int br = i / C, c = i % C;
      int colour = -1;
      for (int y = br * BR; y < std::min(height_, (br + 1) * BR); ++y) {
        const int v = cells[size_t(y) * C + c];
        if (v < 0 || (colour >= 0 && std::abs(v - colour) > strict)) return -1;
        if (colour < 0) colour = v;
      }
      return colour;
    };
    std::vector<std::array<int, 256>> counts(parts.size());
    for (auto& k : counts) k.fill(0);
    for (int i = 0; i < R * C; ++i)
      if (label[size_t(i)] >= 0) {
        const int v = even(i);
        if (v >= 0) ++counts[size_t(find(label[size_t(i)]))][size_t(std::min(255, v / 64))];
      }
    // Its background: the commonest plain cell of its picture (what its
    // pinned blocks showed in place), else of what the copy shows of it.
    std::vector<std::array<int, 256>> plain(parts.size());
    for (auto& k : plain) k.fill(0);
    for (int i = 0; i < R * C; ++i) {
      if (label[size_t(i)] < 0 || !pinned_[size_t(i)] || !picAt_[size_t(i)]) continue;
      const int br = i / C, c = i % C;
      for (int y = br * BR; y < std::min(height_, (br + 1) * BR); ++y)
        if (!pinTex_[size_t(y) * C + c]) ++plain[size_t(find(label[size_t(i)]))][size_t(std::min(255, pinPic_[size_t(y) * C + c] / 64))];
    }
    std::vector<int> bgOf(parts.size(), -1);
    for (size_t g = 0; g < parts.size(); ++g) {
      for (const auto* k : {&plain[g], &counts[g]}) {
        int best = 0;
        for (int b = 0; b < 256; ++b)
          if ((*k)[size_t(b)] > best) {
            best = (*k)[size_t(b)];
            bgOf[g] = b * 64 + 32;
          }
        if (best > 0) break;
      }
      if (bgOf[g] >= 0 && absDiff(uint16_t(bgOf[g]), pageCell_) <= tol) bgOf[g] = -1;
    }
    // And the block around each part (its edge, as the closed mask has it).
    for (int i = 0; i < R * C; ++i) {
      if (label[size_t(i)] < 0) continue;
      const int r = i / C, c = i % C;
      for (int rr = std::max(0, r - 1); rr <= std::min(R - 1, r + 1); ++rr)
        for (int cc = std::max(0, c - 1); cc <= std::min(C - 1, c + 1); ++cc)
          if (owner[size_t(rr) * C + cc] < 0) owner[size_t(rr) * C + cc] = find(label[size_t(i)]);
    }
    std::vector<int> stack;
    for (int i = 0; i < R * C; ++i)
      if (owner[size_t(i)] >= 0) stack.push_back(i);
    while (!stack.empty()) {
      const int i = stack.back();
      stack.pop_back();
      const int g = find(owner[size_t(i)]);
      if (bgOf[size_t(g)] < 0) continue;
      const int r = i / C, c = i % C;
      const int nb[4] = {r > 0 ? i - C : -1, r + 1 < R ? i + C : -1, c > 0 ? i - 1 : -1, c + 1 < C ? i + 1 : -1};
      for (int k : nb) {
        if (k < 0 || owner[size_t(k)] >= 0) continue;
        const int v = even(k);
        if (v < 0 || absDiff(uint16_t(v), uint16_t(bgOf[size_t(g)])) > tol) continue;
        owner[size_t(k)] = g;
        stack.push_back(k);
      }
    }
    // A block of a part's own background that the copy shows apart from it
    // (the rows between hold no copy: an advertisement's artwork above the
    // last screen's lower half): its too.
    for (int i = 0; i < R * C; ++i) {
      if (owner[size_t(i)] >= 0) continue;
      const int v = even(i);
      if (v < 0) continue;
      for (size_t g = 0; g < parts.size(); ++g)
        if (find(int(g)) == int(g) && bgOf[g] >= 0 && absDiff(uint16_t(v), uint16_t(bgOf[g])) <= unsigned(strict)) {
          owner[size_t(i)] = int(g);
          break;
        }
    }
    return owner;
  };
  for (const FrameCopy* copy : {&topCopy_, &bottomCopy_}) {
    const bool topSide = copy == &topCopy_;
    const std::vector<int> owner = ownersIn(*copy, topSide ? topCells : bottomCells);
    for (int y = 0; y < height_; ++y) {
      const long long p = copy->pos + y;
      if (p < top_ || p >= bottom_) continue;
      const int r = std::min(y / BR, R - 1);
      for (int c = 0; c < C;) {
        // Never under a part pinned at the other end: a side bar is at the top
        // once, and an advertisement's plain colour below its artwork (shown
        // only once the page had moved) is not drawn at the image's end.
        // Not a plain cell three frames or more agreed on: one screen's copy
        // does not outweigh them (the first screen's cookie banner or a notice
        // dismissed after it, over a margin the page showed in the frames
        // after it). One or two may have been a header not learned yet over a
        // heading only that screen showed clear.
        auto hole = [&](int cc) {
          const int l = owner[size_t(r) * C + cc];
          return store_->level(p, cc) == 0 && store_->lead(p, cc) < 3 && copy->tag[size_t(r) * C + cc] == copy->serial &&
                 (l < 0 || bool(topAnchored[size_t(l)]) == topSide);
        };
        if (!hole(c)) {
          ++c;
          continue;
        }
        int e = c;
        while (e < C && hole(e)) store_->set(p, e++, 1);
        const int x0 = c * P, x1 = std::min(width_, e * P);
        canvas_.writeSpan(p, x0, x1, &copy->px[(size_t(y) * width_ + x0) * 4]);
        c = e;
      }
    }
  }
  // Plain cells nothing better was written for: the colour most frames
  // showed there (flushPending), its pixels already in the image.
  for (long long p = top_; p < bottom_; ++p)
    for (int c = 0; c < C; ++c)
      if (store_->level(p, c) == 0 && store_->plain(p, c)) store_->set(p, c, 1);
  // What no frame showed clear: a plain colour, in each column the most
  // common colour of the cell over up to 16 written rows above it (below,
  // where there are none). Under a pinned group a third of the screen tall
  // in that column (a side bar), the group's own background instead: the
  // most common colour of the cell over all the rows the first or the last
  // screen drew it in, never a box's colour or its last item's.
  const uint32_t page = pageColour_;
  store_->ensure(top_, bottom_);
  for (long long p = top_; p < bottom_; ++p) {
    // A row no placed frame covered (none can be since a placement on the
    // hint must overlap the image; kept so that such a row is never plain
    // without saying so): every cell of it.
    if (!store_->shown[size_t(p - store_->origin)]) {
      totals_.hiddenCells += C;
      ++totals_.hiddenRows;
      continue;
    }
    bool any = false;
    for (int c = 0; c < C; ++c)
      if (store_->level(p, c) == 0 && store_->hidden(p, c)) {
        ++totals_.hiddenCells;
        any = true;
      }
    totals_.hiddenRows += any;
  }
  // The pinned group the first or the last screen drew at page cell (p, c),
  // and which of the two copies drew it; -1 for the page.
  const auto ownerOf = [&](long long p, int c, const FrameCopy** by) {
    for (const FrameCopy* copy : {&topCopy_, &bottomCopy_}) {
      const long long y = p - copy->pos;
      if (y < 0 || y >= height_) continue;
      const int g = (copy == &topCopy_ ? drawnTop : drawnBottom)[size_t(std::min(int(y) / BR, R - 1)) * C + c];
      if (g < 0) continue;
      if (by) *by = copy;
      return g;
    }
    return -1;
  };
  std::vector<uint8_t> fill(size_t(width_) * 4);
  std::vector<uint32_t> colour(size_t(C), page);
  std::vector<uint8_t> known(size_t(C), 0);
  std::map<long long, std::vector<uint8_t>> rows;  // rows read back, a few at a time
  const auto rowAt = [&](long long p) -> const std::vector<uint8_t>* {
    auto it = rows.find(p);
    if (it == rows.end()) {
      if (rows.size() > 64) rows.clear();
      std::vector<uint8_t> buf(size_t(width_) * 4);
      if (!canvas_.readRow(p, buf.data())) return nullptr;
      it = rows.emplace(p, std::move(buf)).first;
    }
    return &it->second;
  };
  const auto cellColour = [&](int c, long long from, long long step) {
    std::vector<uint32_t> v;
    const FrameCopy* by = nullptr;
    const int own = ownerOf(from, c, &by);
    if (own >= 0) {
      // Under a pinned group as tall as a third of the screen in this column
      // (a side bar): its own background, the most common colour of the cell
      // over all the rows the group was drawn in (not the colour of the last
      // item of its list, a button of its own).
      const std::vector<int>& drawn = by == &topCopy_ ? drawnTop : drawnBottom;
      int rows = 0;
      for (int r = 0; r < R; ++r) rows += drawn[size_t(r) * C + c] == own;
      if (rows * BR * 3 >= height_)
        for (int y = 0; y < height_; ++y) {
          if (drawn[size_t(std::min(y / BR, R - 1)) * C + c] != own) continue;
          for (int x = c * P; x < std::min(width_, (c + 1) * P); ++x) v.push_back(px(&by->px[(size_t(y) * width_ + x) * 4]));
        }
    }
    // Else the written rows above it (below, in the second pass).
    const bool fromGroup = !v.empty();
    for (long long p = from, seen = 0; !fromGroup && seen < 16 && p >= top_ && p < bottom_; p -= step) {
      if (store_->level(p, c) == 0) continue;
      const std::vector<uint8_t>* r = rowAt(p);
      if (!r) break;
      for (int x = c * P; x < std::min(width_, (c + 1) * P); ++x) v.push_back(px(&(*r)[size_t(x) * 4]));
      ++seen;
    }
    if (v.empty()) return page;
    std::sort(v.begin(), v.end());
    uint32_t best = v[0];
    size_t bestRun = 0;
    for (size_t i = 0; i < v.size();) {
      size_t j = i;
      while (j < v.size() && v[j] == v[i]) ++j;
      if (j - i > bestRun) {
        bestRun = j - i;
        best = v[i];
      }
      i = j;
    }
    return best;
  };
  const auto pass = [&](long long from, long long to, long long step) {
    std::fill(known.begin(), known.end(), 0);
    std::vector<long long> written(size_t(C), LLONG_MIN);
    for (long long p = from; p != to; p += step) {
      bool any = false;
      for (int c = 0; c < C; ++c) {
        if (store_->level(p, c) > 0) {
          written[size_t(c)] = p;
          known[size_t(c)] = 0;
          continue;
        }
        if (written[size_t(c)] == LLONG_MIN) continue;
        if (!known[size_t(c)]) {
          colour[size_t(c)] = cellColour(c, written[size_t(c)], step);
          known[size_t(c)] = 1;
        }
        any = true;
      }
      if (!any) continue;
      for (int c = 0; c < C;) {
        auto hole = [&](int cc) { return store_->level(p, cc) == 0 && known[size_t(cc)]; };
        if (!hole(c)) {
          ++c;
          continue;
        }
        int e = c;
        while (e < C && hole(e)) {
          const uint32_t v = colour[size_t(e)];
          for (int x = e * P; x < std::min(width_, (e + 1) * P); ++x) std::memcpy(&fill[size_t(x) * 4], &v, 4);
          ++e;
        }
        const int x0 = c * P, x1 = std::min(width_, e * P);
        canvas_.writeSpan(p, x0, x1, &fill[size_t(x0) * 4]);
        for (int k = c; k < e; ++k) store_->set(p, k, 1);
        c = e;
      }
    }
  };
  pass(top_, bottom_, 1);
  pass(bottom_ - 1, top_ - 1, -1);
  // A column with nothing written at all: the first screen's commonest colour.
  for (long long p = top_; p < bottom_; ++p)
    for (int c = 0; c < C;) {
      if (store_->level(p, c) > 0) {
        ++c;
        continue;
      }
      int e = c;
      while (e < C && store_->level(p, e) == 0) {
        for (int x = e * P; x < std::min(width_, (e + 1) * P); ++x) std::memcpy(&fill[size_t(x) * 4], &page, 4);
        ++e;
      }
      const int x0 = c * P, x1 = std::min(width_, e * P);
      canvas_.writeSpan(p, x0, x1, &fill[size_t(x0) * 4]);
      c = e;
    }
  return totals_;
}

}  // namespace putgd::scroll
