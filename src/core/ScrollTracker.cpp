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
#include <bit>
#include <cmath>
#include <cstring>
#include <numeric>
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
// ad: cur's row y - d (null outside the frame).
void votes(const uint16_t* a, const uint16_t* b, const uint16_t* ta, const uint16_t* tb, const uint16_t* bd,
           const uint16_t* ad, int n, unsigned tol, unsigned strict, unsigned page, uint16_t* stay, uint16_t* move,
           uint16_t* astray) {
  int k = 0;
#if defined(__SSE2__)
  const __m128i t = _mm_set1_epi16(short(tol)), ts = _mm_set1_epi16(short(strict)), one = _mm_set1_epi16(1),
                zero = _mm_setzero_si128();
  for (; k + 8 <= n; k += 8) {
    const __m128i va = ld(a + k), vb = ld(b + k);
    const __m128i diff = absd(va, vb);
    const __m128i same = le(diff, t), exact = le(diff, ts);
    const __m128i here = bd ? le(absd(va, ld(bd + k)), t) : zero;
    const __m128i there = ad ? le(absd(vb, ld(ad + k)), t) : zero;
    const __m128i tex = _mm_cmpeq_epi16(_mm_or_si128(ld(ta + k), ld(tb + k)), one);
    // A flat cell votes too when it is not the page's own background (the
    // flat inside of a cookie banner, a side bar's colour): blank rows of
    // the page that line up by chance must not.
    const __m128i coloured = _mm_andnot_si128(le(absd(va, _mm_set1_epi16(short(page))), t), _mm_cmpeq_epi16(zero, zero));
    const __m128i s = _mm_andnot_si128(_mm_or_si128(here, there), _mm_and_si128(_mm_or_si128(tex, coloured), exact));
    const __m128i m = _mm_andnot_si128(same, _mm_and_si128(here, _mm_cmpeq_epi16(ld(ta + k), one)));
    // Neither where it was nor where the page moved it, where that can be
    // told: something that changes at its own place.
    const __m128i x = bd ? _mm_andnot_si128(_mm_or_si128(_mm_or_si128(here, there), same), tex) : zero;
    st(stay + k, _mm_sub_epi16(ld(stay + k), s));
    st(move + k, _mm_sub_epi16(ld(move + k), m));
    st(astray + k, _mm_sub_epi16(ld(astray + k), x));
  }
#endif
  for (; k < n; ++k) {
    const unsigned diff = absDiff(a[k], b[k]);
    const bool tex = ta[k] | tb[k];
    const bool coloured = absDiff(a[k], page) > tol;
    const bool here = bd && absDiff(a[k], bd[k]) <= tol;
    const bool there = ad && absDiff(b[k], ad[k]) <= tol;
    if ((tex || coloured) && diff <= strict && !here && !there) ++stay[k];
    else if (diff > tol && here && ta[k]) ++move[k];
    else if (bd && tex && diff > tol && !here && !there) ++astray[k];
  }
}

// Cells that show something outside what is pinned (open), and those of
// them strictly where they were.
void strictStill(const uint16_t* a, const uint16_t* b, const uint16_t* tex, const uint16_t* pin, int n, unsigned strict,
                 uint16_t* open, uint16_t* still, uint16_t* openSum, uint16_t* stillSum) {
  int k = 0;
#if defined(__SSE2__)
  const __m128i t = _mm_set1_epi16(short(strict)), one = _mm_set1_epi16(1);
  for (; k + 8 <= n; k += 8) {
    const __m128i o = _mm_andnot_si128(ld(pin + k), ld(tex + k));
    const __m128i s = _mm_and_si128(o, _mm_and_si128(le(absd(ld(a + k), ld(b + k)), t), one));
    st(open + k, o);
    st(still + k, s);
    st(openSum + k, _mm_add_epi16(ld(openSum + k), o));
    st(stillSum + k, _mm_add_epi16(ld(stillSum + k), s));
  }
#endif
  for (; k < n; ++k) {
    open[k] = uint16_t(tex[k] & !pin[k]);
    still[k] = uint16_t(open[k] & (absDiff(a[k], b[k]) <= strict));
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
  std::vector<uint16_t> band, valid, tex;  // count * bands

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
    std::vector<uint64_t> p(size_t(n) * words, 0), v(size_t(n) * words, 0);
    std::vector<uint16_t> bd(size_t(n) * bands, 0), vd(size_t(n) * bands, 0), td(size_t(n) * bands, 0);

    if (count > 0) {
      const size_t at = size_t(origin - nlo);
      std::memcpy(&p[at * words], prov.data(), prov.size() * 8);
      std::memcpy(&v[at * words], ver.data(), ver.size() * 8);
      std::memcpy(&bd[at * bands], band.data(), band.size() * 2);
      std::memcpy(&vd[at * bands], valid.data(), valid.size() * 2);
      std::memcpy(&td[at * bands], tex.data(), tex.size() * 2);
    }
    prov.swap(p);
    ver.swap(v);
    band.swap(bd);
    valid.swap(vd);
    tex.swap(td);
    origin = nlo;
    count = n;
  }
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
  uint16_t* bandRow(long long r) { return &band[size_t(r - origin) * bands]; }
  uint16_t* validRow(long long r) { return &valid[size_t(r - origin) * bands]; }
  const uint16_t* bandRow(long long r) const { return &band[size_t(r - origin) * bands]; }
  const uint16_t* validRow(long long r) const { return &valid[size_t(r - origin) * bands]; }
  uint16_t* texRow(long long r) { return &tex[size_t(r - origin) * bands]; }
  const uint16_t* texRow(long long r) const { return &tex[size_t(r - origin) * bands]; }
};

// ------------------------------------------------------------------ tracker

ScrollTracker::ScrollTracker(TrackerCanvas& canvas, TrackerOptions options)
    : canvas_(canvas), opts_(options), last_(std::make_unique<Signature>()), work_(std::make_unique<Signature>()),
      store_(std::make_unique<Store>()) {
  opts_.cellPx = std::clamp(opts_.cellPx, 1, 64);
  opts_.blockRows = std::clamp(opts_.blockRows, 1, 64);
  opts_.bands = std::clamp(opts_.bands, 1, 64);
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
void ScrollTracker::computeExcluded(const Signature& cur, const Signature& ref) {
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
    const unsigned textured = countAndNot(tex, pin, B);
    texturedRows_[size_t(y) + 1] = texturedRows_[size_t(y)] + (textured >= 2);
    uint16_t* tt = &texTile_[size_t(y / kTileRows) * B];
    for (int k = 0; k < B; ++k) tt[k] = uint16_t(tt[k] + (tex[k] & !pin[k]));
    // Cells that stood still, strictly (what is pinned is drawn again the
    // same; lines of text that only look alike are not): per tile and band.
    // Per cell over the tile's rows, then per band once a tile row ends.
    strictStill(cur.cellRow(y), ref.cellRow(y), cur.cellTexRow(y), &closed16_[size_t(brow) * C], C,
                unsigned(opts_.cellTolerance) / 8, openRow.data(), stillRow.data(), openSum.data(), stillSum.data());
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
                                         &bandIncl_[size_t(y) * B], &bandIncl_[size_t(y + d) * B], B, tol, &t.agree[tile],
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
               &cellIncl_[size_t(y + d) * C], C, tol, ca.data(), ci.data());
  }
  if (y0 < y1) fold(tileRow);
  t.total();
}

// Whether the frame shows what ref showed (no movement), by the share of
// textured cells that are where they were, pinned blocks left out.
bool ScrollTracker::stillFrame(const Signature& cur, const Signature& ref, double* share) const {
  const int H = cur.h, C = cur.cells;
  unsigned agree = 0, inf = 0, differ = 0;
  const int lo = cellLo_, n = cellHi_ - cellLo_;
  for (int y = 0; y < H; ++y) {
    const int brow = std::min(y / opts_.blockRows, blockRowCount_ - 1);
    stillCounts(cur.cellRow(y) + lo, ref.cellRow(y) + lo, cur.cellTexRow(y) + lo, ref.cellTexRow(y) + lo,
                &closed16_[size_t(brow) * C + lo], n, unsigned(opts_.cellTolerance), differ, inf, agree);
  }
  const double s = inf > 0 ? double(agree) / inf : (differ == 0 ? 1.0 : 0.0);
  if (share) *share = s;
  if (inf < 32) return differ * 50u < unsigned(H) * unsigned(n) / 20u + 1;  // blank: nothing changed
  return s >= opts_.stillShare;
}

namespace {
int sliverRows(const TrackerOptions& o, int h) {
  return std::max(o.minOverlapRows, int(std::ceil(o.minOverlapFraction * h)));
}
}  // namespace

// Scores a candidate fully: bands and cells over every row, and whether it
// is acceptable as the page's movement.
// A tile stood still when at least two of its textured cells, and half of
// them, are strictly where they were (what is pinned is drawn again the
// same; photo-like noise only sometimes looks alike).
bool ScrollTracker::stillTile(size_t i) const { return stillTile_[i] >= 2 && 2 * stillTile_[i] >= texCellTile_[i]; }

void ScrollTracker::judge(Candidate& c, const Tally& bands, const Tally& cells, int y0, int y1) const {
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
  for (int tr = tr0; tr <= tr1; ++tr) {
    int m = 0, st = 0;
    for (int k = 0; k < B; ++k) {
      if (colI[size_t(k)] == 0 || 2 * colA[size_t(k)] < colI[size_t(k)]) continue;
      const size_t i = size_t(tr) * B + k;
      if (c.moving[i] == 1) ++m;
      else if (stillTile(i)) ++st;
    }
    movedRows += m > st;
    stillRows += st > m;
  }
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
  c.accepted = c.area >= opts_.minArea && c.bandsKept >= opts_.acceptBands && c.cellsKept >= opts_.acceptCells &&
               c.coverage >= opts_.minRowCoverage && int(ki) >= 4 * opts_.minEvidenceRows &&
               c.rows >= opts_.minEvidenceRows && y1 - y0 >= sliverRows(opts_, height_);
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
          &bandIncl_[size_t(y + d) * B], B, tol, agree, inf);
    ++used;
    if ((used == 8 || used == 24) && inf >= 24 && double(agree) / inf < abortBelow) break;
    if (used == 64) break;
  }
  return inf >= 16 ? double(agree) / inf : 0.0;
}

// Every offset that keeps more than a sliver of overlap, coarse first, then
// the local best of each basin on every row, then cells.
std::vector<ScrollTracker::Candidate> ScrollTracker::searchPair(const Signature& cur, const Signature& ref,
                                                                 int expected) const {
  std::vector<Candidate> out;
  searchPass(cur, ref, expected, bandIncl_, out);
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
  searchPass(cur, ref, expected, masked, out);
  return out;
}

// One look: every offset that keeps more than a sliver of overlap, coarsely
// (with `incl` for this frame's lanes), then the local best of each basin
// on every row, then cells; the candidates are added to `out`.
void ScrollTracker::searchPass(const Signature& cur, const Signature& ref, int expected,
                               const std::vector<uint16_t>& incl, std::vector<Candidate>& out) const {
  const int H = cur.h;
  const int dmax = H - sliverRows(opts_, H);
  if (dmax < 1) return;
  const int n = 2 * dmax + 1;
  std::vector<float> coarse(size_t(n), 0.f);
  const double floor = opts_.coarseFloor;
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
  std::sort(fine.begin(), fine.end(), [&](const Fine& a, const Fine& b) {
    if (std::abs(a.bands.share() - b.bands.share()) > 0.01) return a.bands.share() > b.bands.share();
    return std::abs(a.d - expected) < std::abs(b.d - expected);
  });
  if (!fine.empty()) {
    const double top = fine[0].bands.share();
    std::erase_if(fine, [&](const Fine& f) { return f.bands.share() < top - 0.4; });
  }
  if (fine.size() > 5) fine.resize(5);
  for (const Fine& f : fine) {
    cellTally(cur, ref, f.d, tc);
    Candidate c;
    c.d = f.d;
    judge(c, f.bands, tc, std::max(0, -f.d), std::min(H, H - f.d));
    out.push_back(c);
  }
  // The best by cells, and any that ties with it, to the row: a neighbour
  // that agrees in more cells is the place (whole pixels, never one off).
  double best = -1;
  for (const Candidate& c : out) best = std::max(best, c.cells);
  for (Candidate& c : out) {
    if (c.cells < best - opts_.tieCells) continue;
    for (int step : {-1, 1}) {
      for (int d = c.d + step; d != 0 && std::abs(d) <= dmax; d += step) {
        cellTally(cur, ref, d, tc);
        if (tc.share() <= c.cells) break;
        bandTally(cur, ref, d, t);
        Candidate n;
        n.d = d;
        judge(n, t, tc, std::max(0, -d), std::min(H, H - d));
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
  std::sort(peaks.begin(), peaks.end(), [&](int a, int b) {
    const float va = coarse[size_t(a)], vb = coarse[size_t(b)];
    if (std::abs(va - vb) > 0.02f) return va > vb;
    return std::llabs(a - center) < std::llabs(b - center);
  });
  if (!peaks.empty()) {
    const float best = coarse[size_t(peaks[0])];
    std::erase_if(peaks, [&](int i) { return coarse[size_t(i)] < best - 0.35f; });
  }
  if (peaks.size() > 6) peaks.resize(6);
  return peaks;
}

// The decision among the candidates: the best by cells; equal ones (content
// that repeats) are decided by the expected offset only when it singles one
// out, otherwise the frame is skipped. Returns the index, or -1.
int ScrollTracker::decide(std::vector<Candidate>& cands, int expected, int uncertainty, bool hinted,
                          bool* byExpectation, SkipReason* why) const {
  *byExpectation = false;
  std::vector<int> ok;
  int maxInf = 0;
  for (size_t i = 0; i < cands.size(); ++i) {
    maxInf = std::max(maxInf, cands[i].informative);
    if (cands[i].accepted) ok.push_back(int(i));
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
  // last movement, it is the page, whatever the columns say (an inner box as
  // wide as the article, with sparse text above and below it, or a code
  // block that scrolled on its own and then with the page).
  if (layers > 1 && lastMove_ != 0) {
    const int near = std::max(8, std::abs(lastMove_) / 5);
    int consistent = -1, count = 0;
    std::vector<uint8_t> seen(size_t(layers), 0);
    for (int i : ok) {
      const int l = layerOf[size_t(i)];
      if (seen[size_t(l)] || std::abs(cands[size_t(i)].d - lastMove_) > near) continue;
      seen[size_t(l)] = 1;
      consistent = l;
      ++count;
    }
    if (count == 1) page = consistent;
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
    if ((used == 16 || used == 40) && inf >= 32 && double(agree) / inf < abortBelow) break;
    if (used == 64) break;
  }
  return inf >= 16 ? double(agree) / inf : 0.0;
}

// Cells of the frame placed at pos against the canvas's written rows (read
// back, signatures computed the same way).
void ScrollTracker::storeCellTally(const Signature& cur, long long pos, Tally& t) const {
  const int H = cur.h, C = cur.cells, P = opts_.cellPx, B = cur.bands;
  t.clear(B, H);
  std::vector<uint8_t> row(size_t(width_) * 4);
  std::vector<uint16_t> sig(static_cast<size_t>(C));
  const unsigned tol = unsigned(opts_.cellTolerance);
  for (int y = 0; y < H; ++y) {
    const long long p = pos + y;
    if (!store_->has(p) || !canvas_.readRow(p, row.data())) continue;
    for (int c = cellLo_; c < cellHi_; ++c) {
      const int x0 = c * P, x1 = std::min(width_, x0 + P);
      unsigned sum = 0;
      for (int x = x0; x < x1; ++x) sum += unsigned(row[4 * x]) + 2u * row[4 * x + 1] + row[4 * x + 2];
      sig[size_t(c)] = uint16_t(sum * 16u / unsigned(x1 - x0));
    }
    const uint16_t* a = cur.cellRow(y);
    const uint16_t* ta = cur.cellTexRow(y);
    const uint16_t* inc = &cellIncl_[size_t(y) * C];
    uint16_t* tA = &t.agree[size_t(y / kTileRows) * B];
    uint16_t* tI = &t.inf[size_t(y / kTileRows) * B];
    for (int k = 0; k < B; ++k)
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

// The rows a frame placed at pos shares with the image must agree with it
// (the second opinion after the pair: wayscrollshot's check against the
// long image). Lenient: content that changes in place is taken as first seen.
bool ScrollTracker::contradicted(const Signature& cur, long long pos) const {
  Tally t;
  storeBandTally(cur, pos, 1, t);
  // By area: dense text in a box that moved on its own must not outvote a
  // sparser page.
  int kept = 0, shown = 0;
  for (size_t i = 0; i < t.inf.size(); ++i) {
    if (t.inf[i] < 2) continue;
    ++shown;
    kept += t.keptTile(i);
  }
  return shown >= 16 && double(kept) / shown < opts_.keepShare;
}

// Looks the frame up in the image already built: every position where it
// keeps more than a sliver of overlap with the extent (near the expected one
// first), by bands against the store, then cells against the canvas.
std::vector<ScrollTracker::Candidate> ScrollTracker::searchStore(const Signature& cur, long long expectedPos,
                                                                 bool wide) const {
  std::vector<Candidate> out;
  const int H = cur.h;
  const int sliver = sliverRows(opts_, H);
  long long lo = top_ - H + sliver, hi = bottom_ - sliver;
  if (!wide) {
    lo = std::max(lo, expectedPos - 3LL * H);
    hi = std::min(hi, expectedPos + 3LL * H);
  }
  if (hi < lo) return out;
  const double floor = opts_.coarseFloor;
  const long long n = hi - lo + 1;
  std::vector<float> coarse(size_t(n), 0.f);
  for (long long pos = lo; pos <= hi; ++pos) coarse[size_t(pos - lo)] = float(storeCoarseShare(cur, pos, floor));
  std::vector<long long> peaks;
  for (int i : pickPeaks(coarse, floor, std::clamp(expectedPos, lo, hi) - lo)) peaks.push_back(lo + i);
  if (peaks.size() > 4) peaks.resize(4);
  Tally t, tc;
  for (long long p : peaks) {
    long long bestPos = p;
    double bestBands = -1;
    for (long long pos = std::max(lo, p - 3); pos <= std::min(hi, p + 3); ++pos) {
      storeBandTally(cur, pos, 1, t);
      if (t.share() > bestBands) {
        bestBands = t.share();
        bestPos = pos;
      }
    }
    Candidate best;
    best.cells = -1;
    for (long long pos = bestPos - 1; pos <= bestPos + 1; ++pos) {
      storeCellTally(cur, pos, tc);
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
  std::fill(stillNow_.begin(), stillNow_.end(), 0);
  if (std::abs(d) < opts_.learnMinStep) {
    blocked_ = closed_;
    return;
  }
  ++learnedSteps_;
  std::vector<uint16_t> stay(size_t(blockRowCount_) * C, 0), move(size_t(blockRowCount_) * C, 0),
      astray(size_t(blockRowCount_) * C, 0);
  const unsigned tol = unsigned(opts_.cellTolerance);
  for (int y = 0; y < H; ++y) {
    const bool fwd = y + d >= 0 && y + d < H, back = y - d >= 0 && y - d < H;
    // Where neither frame shows where the page moved this row, a cell that
    // looks the same at the same place is a coincidence, not evidence.
    if (!fwd && !back) continue;
    const size_t base = size_t(std::min(y / R, blockRowCount_ - 1)) * C;
    votes(cur.cellRow(y), ref.cellRow(y), cur.cellTexRow(y), ref.cellTexRow(y), fwd ? ref.cellRow(y + d) : nullptr,
          back ? cur.cellRow(y - d) : nullptr, C, tol, tol / 8, pageCell_, &stay[base], &move[base], &astray[base]);
  }
  bool changed = false;
  for (size_t i = 0; i < stay.size(); ++i) {
    const unsigned s = stay[i], m = move[i], x = astray[i];
    // A block that keeps changing at its own place while the page moves (an
    // animated advertisement in a column that sticks) is not the page either.
    if (x >= 3 && m == 0 && s < x) {
      wandering_[i] = uint8_t(std::min(255, wandering_[i] + 1));
      if (wandering_[i] >= 3 && !pinned_[i]) {
        pinned_[i] = 1;
        changed = true;
      }
    } else if (m >= 3) {
      wandering_[i] = 0;
    }
    if (s >= 3 && s >= 3 * m) {
      stayed_[i] = uint8_t(std::min(255, stayed_[i] + 1));
      if (stayed_[i] >= opts_.pinAfterSteps && !pinned_[i]) {
        pinned_[i] = 1;
        changed = true;
      }
      stillNow_[i] = 1;
    } else if (m >= 3 && m >= 3 * s) {
      stayed_[i] = 0;
      if (pinned_[i]) {
        pinned_[i] = 0;
        changed = true;
      }
    } else if (s >= 2 && s > m) {
      stillNow_[i] = 1;
    }
  }
  if (changed) closeMask();
  // What a frame offers the image: not what is pinned, nor what stood still
  // in this pair, its short gaps in a column closed (a side bar that has
  // just stuck holds back the flat rows between its items too) and widened
  // by a block.
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
}

// The closed mask: pinned blocks, plus the holes of a pinned part (the flat
// inside of a box has nothing that could vote) up to closeFraction of the
// frame, within one connected part.
void ScrollTracker::close(const std::vector<uint8_t>& src, std::vector<uint8_t>& out) const {
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
  for (int c = 0; c < C; ++c) {
    int lastR = -1, lastL = -1;
    for (int r = 0; r < R; ++r) {
      const int l = label[size_t(r) * C + c];
      if (l < 0) continue;
      if (lastL == l && r - lastR > 1 && r - lastR - 1 <= vGap)
        for (int k = lastR + 1; k < r; ++k) out[size_t(k) * C + c] = 1;
      lastR = r;
      lastL = l;
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
  const int C = cells_, R = blockRowCount_;
  close(pinned_, closed_);
  closed16_.assign(closed_.begin(), closed_.end());
  const int B = int(bandLo_.size()) - 1;
  bandPinned16_.assign(size_t(R) * B, 0);
  for (int r = 0; r < R; ++r)
    for (int k = 0; k < B; ++k)
      for (int c = bandLo_[size_t(k)]; c < bandLo_[size_t(k) + 1]; ++c)
        if (closed_[size_t(r) * C + c]) {
          bandPinned16_[size_t(r) * B + k] = 1;
          break;
        }
}

// The topmost and the bottommost frames are kept for what is pinned, which
// is in the image once, as they show it. While the mask is young the whole
// frame is kept; afterwards only its blocks that are pinned or stood still
// (a frame copy per frame of a 60 Hz stream would cost more than the rest).
void ScrollTracker::keepCopy(const FrameView& f, FrameCopy& copy, long long pos, bool all, int fromRow) {
  const int C = cells_, R = blockRowCount_, P = opts_.cellPx, BR = opts_.blockRows;
  copy.px.resize(size_t(width_) * height_ * 4);
  copy.tag.resize(size_t(R) * C, 0);
  ++copy.serial;
  copy.pos = pos;
  if (all) {
    for (int y = 0; y < height_; ++y) std::memcpy(&copy.px[size_t(y) * width_ * 4], f.row(y), size_t(width_) * 4);
    std::fill(copy.tag.begin(), copy.tag.end(), copy.serial);
    return;
  }
  for (int r = fromRow / BR; r < R; ++r)
    for (int c = 0; c < C;) {
      const size_t bi = size_t(r) * C + c;
      if (!blocked_[bi]) {
        ++c;
        continue;
      }
      int e = c;
      while (e < C && blocked_[size_t(r) * C + e]) copy.tag[size_t(r) * C + e++] = copy.serial;
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
    uint64_t* w = &out[size_t(br) * words];
    for (int c = 0; c < C; ++c) w[c >> 6] |= uint64_t(row[c] != 0) << (c & 63);
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

// What the frame shows clear goes into the image in two ways. A cell that
// two placed frames show the same is written at once (verified). A cell seen
// once waits for the next placed frame: it is written as seen (provisional)
// only if that pair does not find it standing still, so that a side bar
// caught as it moved into place, a header before it was learned, or a box
// in a frame's new rows never becomes the page. Never from a block that is
// pinned or stood still in the last pair.
void ScrollTracker::compose(const FrameView& f, const Signature& cur, const Signature* ref, int d, long long pos,
                            TrackResult& r) {
  const int H = height_, C = cells_;
  const long long cap = opts_.maxOutputRows;
  const long long top0 = top_, bot0 = bottom_;
  long long lo = hasFrame_ ? std::min(top_, pos) : pos, hi = hasFrame_ ? std::max(bottom_, pos + H) : pos + H;
  // The cap: rows outside [allowLo_, allowHi_) are not written.
  if (!hasFrame_) {
    allowLo_ = pos;
    allowHi_ = pos + cap;
  }
  if (hi - lo > cap) {
    capped_ = true;
    if (pos + H > bot0 && hasFrame_) {
      allowLo_ = top0;
      allowHi_ = top0 + cap;
    } else if (pos < top0 && hasFrame_) {
      allowHi_ = bot0;
      allowLo_ = bot0 - cap;
    }
  } else {
    allowLo_ = hi - cap;
    allowHi_ = lo + cap;
  }
  store_->ensure(std::max(allowLo_, pos), std::min(allowHi_, pos + H));
  const int words = store_->words;
  std::vector<uint64_t> blocked;
  blockedWords(blocked);
  std::vector<uint64_t> full(static_cast<size_t>(words), ~0ull), mask(static_cast<size_t>(words));
  if (C % 64) full[size_t(words - 1)] = (1ull << (C % 64)) - 1;
  const unsigned tol = unsigned(opts_.cellTolerance);
  // 1. Verified: the cells this frame and the last one show the same.
  if (ref) {
    for (int y = std::max(0, -d); y < std::min(H, H - d); ++y) {
      const long long p = pos + y;
      if (p < allowLo_ || p >= allowHi_) continue;
      const int brow = std::min(y / opts_.blockRows, blockRowCount_ - 1);
      const int bref = std::min((y + d) / opts_.blockRows, blockRowCount_ - 1);
      const size_t at = size_t(p - store_->origin) * words;
      const uint64_t* blk = &blocked[size_t(brow) * words];
      const uint64_t* blkRef = &blocked[size_t(bref) * words];
      const uint16_t* a = cur.cellRow(y);
      const uint16_t* b = ref->cellRow(y + d);
      bool any = false;
      for (int w = 0; w < words; ++w) {
        uint64_t need = ~(store_->ver[at + size_t(w)] | blk[w] | blkRef[w]) & full[size_t(w)];
        uint64_t take = 0;
        while (need) {
          const int bit = std::countr_zero(need);
          need &= need - 1;
          const int c = w * 64 + bit;
          if (absDiff(a[c], b[c]) <= tol) take |= 1ull << bit;
        }
        mask[size_t(w)] = take;
        any |= take != 0;
      }
      if (any) writeCells(p, f.row(y), cur.bandRow(y), cur.bandTexRow(y), mask.data(), 2, blk);
    }
  }
  // 2. The last frame's cells that were seen once: as seen, where this pair
  // did not find them standing still and nothing better was written.
  flushPending(blocked);
  // 3. This frame's cells that nothing is written for yet wait for the next:
  //    their pixels (cell spans, not whole rows), the row's bands, and which
  //    cells they are.
  pendingPos_ = pos;
  pendingRows_.clear();
  const int B = cur.bands, P = opts_.cellPx;
  for (int y = 0; y < H; ++y) {
    const long long p = pos + y;
    if (p < allowLo_ || p >= allowHi_) continue;
    const int brow = std::min(y / opts_.blockRows, blockRowCount_ - 1);
    const size_t at = size_t(p - store_->origin) * words;
    bool open = false;
    for (int w = 0; w < words; ++w) {
      mask[size_t(w)] = ~(store_->prov[at + size_t(w)] | blocked[size_t(brow) * words + size_t(w)]) & full[size_t(w)];
      open |= mask[size_t(w)] != 0;
    }
    if (!open) continue;
    const size_t slot = pendingRows_.size();
    pendingRows_.push_back(y);
    pendingPixels_.resize((slot + 1) * size_t(width_) * 4);
    pendingBands_.resize((slot + 1) * size_t(B) * 2);
    pendingOpen_.resize((slot + 1) * size_t(words));
    std::memcpy(&pendingOpen_[slot * size_t(words)], mask.data(), size_t(words) * 8);
    const uint8_t* row = f.row(y);
    uint8_t* to = &pendingPixels_[slot * size_t(width_) * 4];
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
    std::memcpy(&pendingBands_[slot * size_t(B) * 2], cur.bandRow(y), size_t(B) * 2);
    std::memcpy(&pendingBands_[slot * size_t(B) * 2 + size_t(B)], cur.bandTexRow(y), size_t(B) * 2);
  }
  // The extent follows the frame even where a row has nothing written yet.
  top_ = hasFrame_ ? std::max(allowLo_, std::min(top_, pos)) : std::max(allowLo_, pos);
  bottom_ = hasFrame_ ? std::min(allowHi_, std::max(bottom_, pos + H)) : std::min(allowHi_, pos + H);
  r.addedTop = int(std::max<long long>(0, top0 - top_));
  r.addedBottom = int(std::max<long long>(0, bottom_ - bot0));
  if (!hasFrame_) {
    r.addedTop = 0;
    r.addedBottom = int(bottom_ - top_);
  }
}

// Writes the waiting cells of the last placed frame as it saw them, where
// they are not blocked now and nothing is written for them yet.
void ScrollTracker::flushPending(const std::vector<uint64_t>& blocked) {
  const int words = store_->words, C = cells_, B = int(bandLo_.size()) - 1;
  std::vector<uint64_t> full(static_cast<size_t>(words), ~0ull), mask(static_cast<size_t>(words));
  if (C % 64) full[size_t(words - 1)] = (1ull << (C % 64)) - 1;
  for (size_t slot = 0; slot < pendingRows_.size(); ++slot) {
    const int y = pendingRows_[slot];
    const long long p = pendingPos_ + y;
    if (!store_->has(p) || p < allowLo_ || p >= allowHi_) continue;
    const int brow = std::min(y / opts_.blockRows, blockRowCount_ - 1);
    const size_t at = size_t(p - store_->origin) * words;
    const uint64_t* blk = &blocked[size_t(brow) * words];
    const uint64_t* open = &pendingOpen_[slot * size_t(words)];
    bool any = false;
    for (int w = 0; w < words; ++w) {
      mask[size_t(w)] = ~(store_->prov[at + size_t(w)] | blk[w]) & full[size_t(w)] & open[w];
      any |= mask[size_t(w)] != 0;
    }
    if (!any) continue;
    const uint16_t* bands = &pendingBands_[slot * size_t(B) * 2];
    writeCells(p, &pendingPixels_[slot * size_t(width_) * 4], bands, bands + B, mask.data(), 1, blk);
  }
  pendingRows_.clear();
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
  if (first || pos <= topCopy_.pos) keepCopy(f, topCopy_, pos, first || young, 0);
  // Only parts that start in the lower half are drawn from the bottommost
  // frame (see finish).
  if (first || pos >= bottomCopy_.pos) keepCopy(f, bottomCopy_, pos, first || young, height_ / 2 - opts_.blockRows);
  const int frames = std::max(1, framesSincePlaced_);
  if (!first) velocity_ = double(d) / frames;
  if (!first && d != 0) lastMove_ = d;
  lastPos_ = pos;
  std::swap(last_, work_);
  framesSincePlaced_ = 0;
  hintSum_ = 0;
  hintUnc_ = 0;
  hintKnown_ = true;
  r.position = pos;
  r.offset = d;
  r.status = capped_ ? TrackStatus::CapReached : TrackStatus::Placed;
  ++totals_.placed;
  if (capped_) totals_.capReached = true;
}

TrackResult ScrollTracker::addFrame(const FrameView& frame, TrackerHint hint) {
  TrackResult r;
  ++totals_.frames;
  if (finished_ || !frame.data || frame.width <= 0 || frame.height <= 0 ||
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
    pinned_.assign(blocks, 0);
    closed_.assign(blocks, 0);
    stillNow_.assign(blocks, 0);
    closed16_.assign(blocks, 0);
    blocked_.assign(blocks, 0);
    bandPinned16_.assign(size_t(blockRowCount_) * B, 0);
    store_->init(cells_, B);
  }
  computeSignature(frame, opts_.cellPx, bandLo_, opts_.cellTexture, opts_.bandTexture, *work_);
  Signature& cur = *work_;
  ++framesSincePlaced_;
  if (hint.known) {
    hintSum_ += hint.offset;
    hintUnc_ += std::max(0, hint.uncertainty);
  } else {
    hintKnown_ = false;
  }
  if (!hasFrame_) {
    placeFrame(frame, cur, nullptr, 0, 0, r);
    r.confidence = 1;
    r.state = state_ = TrackState::Following;
    return r;
  }
  const Signature& ref = *last_;
  computeExcluded(cur, ref);
  const bool hinted = hintKnown_;
  const int expected = hinted ? int(hintSum_) : int(std::lround(velocity_ * framesSincePlaced_));
  const int uncertainty = hinted ? int(hintUnc_ + std::lround(0.08 * std::abs(hintSum_))) + 2
                                 : std::max(6, int(std::lround(0.25 * std::abs(expected))));
  double share = 0;
  const auto noMovement = [&] {
    // The page is where it was; this frame is its latest picture, so that
    // what changes in place (an inner scroll area, a video) is compared with
    // how it looks now, not with how it looked frames ago.
    std::swap(last_, work_);
    r.status = TrackStatus::NoMovement;
    r.position = lastPos_;
    r.confidence = share;
    ++totals_.noMovement;
    framesSincePlaced_ = 0;
    hintSum_ = 0;
    hintUnc_ = 0;
    hintKnown_ = true;
    velocity_ = 0;
    skipRun_ = 0;
    r.state = state_ = TrackState::Following;
    return r;
  };
  if (stillFrame(cur, ref, &share) && (!hinted || std::abs(expected) <= uncertainty)) return noMovement();
  std::vector<Candidate> cands = searchPair(cur, ref, expected);
  bool byExp = false;
  SkipReason why = SkipReason::None;
  int pick = decide(cands, expected, uncertainty, hinted, &byExp, &why);
  long long pos = 0;
  int d = 0;
  bool recovered = false;
  // Most of the frame stood still and what moved does not span it: an inner
  // scroll area under the pointer (a page's code block), not the page.
  if (pick >= 0 && share >= opts_.stillFallback && cands[size_t(pick)].coverage < opts_.stillCoverage) return noMovement();
  if (pick >= 0) {
    d = cands[size_t(pick)].d;
    pos = lastPos_ + d;
    if (contradicted(cur, pos)) {
      pick = -1;
      why = SkipReason::Contradicted;
    }
  }
  // Nothing moved as the page, and most of the frame stood still: the page
  // did not move, and something changed in place (a video, an inner scroll
  // area, an animation). Not a lookup: a reader who jumped leaves nothing
  // where it was.
  if (pick < 0 && why != SkipReason::Ambiguous && share >= opts_.stillFallback) return noMovement();
  if (pick < 0 && why != SkipReason::NoEvidence) {
    // Not where the last frame says: look in the image (the user scrolled back).
    const long long expPos = lastPos_ + expected;
    std::vector<Candidate> found = searchStore(cur, expPos, false);
    bool fb = false;
    SkipReason fw = SkipReason::None;
    int fp = decide(found, expected, uncertainty, hinted, &fb, &fw);
    if (fp < 0 && fw != SkipReason::Ambiguous && skipRun_ % 4 == 3) {
      found = searchStore(cur, expPos, true);
      fp = decide(found, expected, uncertainty, hinted, &fb, &fw);
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
  if (pick < 0 && why == SkipReason::NoEvidence && hinted) {
    // Nothing to compare (a blank stretch): the driver's hint bridges it.
    d = expected;
    pos = lastPos_ + d;
    r.onHint = true;
    ++totals_.onHint;
    pick = 0;
  }
  if (pick < 0) {
    r.status = TrackStatus::Skipped;
    r.reason = why;
    r.position = lastPos_;
    ++totals_.skipped;
    ++skipRun_;
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
  learn(cur, ref, d);
  placeFrame(frame, cur, &ref, d, pos, r);
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
    flushPending(blocked);
  }
  // The pinned parts: connected regions of the closed mask.
  std::vector<int> label(size_t(R) * C, -1);
  struct Part {
    int r0 = 1 << 30, r1 = -1, c0 = 1 << 30, c1 = -1;
    long long blocks = 0;
    uint32_t colour = 0;
  };
  std::vector<Part> parts;
  std::vector<int> stack;
  for (int i = 0; i < R * C; ++i) {
    if (!closed_[size_t(i)] || label[size_t(i)] >= 0) continue;
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
        if (closed_[size_t(k)] && label[size_t(k)] < 0) {
          label[size_t(k)] = id;
          stack.push_back(k);
        }
      }
    }
  }
  totals_.pinnedRegions = int(parts.size());
  // Each part's own background: its most common colour in the first screen.
  for (size_t id = 0; id < parts.size(); ++id) {
    std::unordered_map<uint32_t, int> count;
    for (int r = parts[id].r0; r <= parts[id].r1; ++r)
      for (int c = parts[id].c0; c <= parts[id].c1; ++c) {
        if (label[size_t(r) * C + c] != int(id)) continue;
        for (int y = r * BR; y < std::min(height_, (r + 1) * BR); y += 2)
          for (int x = c * P; x < std::min(width_, (c + 1) * P); x += 2) {
            const size_t bi = size_t(r) * C + c;
            const FrameCopy& from = topCopy_.tag[bi] == topCopy_.serial ? topCopy_ : bottomCopy_;
            if (from.tag[bi] != from.serial) continue;
            ++count[px(&from.px[(size_t(y) * width_ + x) * 4])];
          }
      }
    int best = -1;
    for (const auto& [k, v] : count)
      if (v > best) {
        best = v;
        parts[id].colour = k;
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
  for (size_t a = 0; a < parts.size(); ++a)
    for (size_t b = a + 1; b < parts.size(); ++b) {
      const Part& A = parts[a];
      const Part& Bp = parts[b];
      const int overlap = std::min(A.c1, Bp.c1) - std::max(A.c0, Bp.c0) + 1;
      const int narrower = std::min(A.c1 - A.c0, Bp.c1 - Bp.c0) + 1;
      const int gap = std::max(A.r0, Bp.r0) - std::min(A.r1, Bp.r1) - 1;
      if (overlap * 2 >= narrower && gap <= R / 4) group[size_t(find(int(a)))] = find(int(b));
    }
  std::vector<int> groupTop(parts.size(), R);
  for (size_t i = 0; i < parts.size(); ++i) groupTop[size_t(find(int(i)))] = std::min(groupTop[size_t(find(int(i)))], parts[i].r0);
  std::vector<uint8_t> topAnchored(parts.size());
  for (size_t i = 0; i < parts.size(); ++i) topAnchored[i] = groupTop[size_t(find(int(i)))] * BR * 2 < height_;
  std::vector<uint8_t> overlaid(size_t(R) * C, 0);
  auto overlay = [&](const FrameCopy& copy, bool topSide) {
    const long long pos = copy.pos;
    for (int r = 0; r < R; ++r)
      for (int c = 0; c < C;) {
        auto take = [&](int cc) {
          const int l = label[size_t(r) * C + cc];
          if (l < 0 || copy.tag[size_t(r) * C + cc] != copy.serial) return false;
          return bool(topAnchored[size_t(l)]) == topSide;
        };
        if (!take(c)) {
          ++c;
          continue;
        }
        int e = c;
        while (e < C && take(e)) overlaid[size_t(r) * C + e++] = 1;
        for (int y = r * BR; y < std::min(height_, (r + 1) * BR); ++y) {
          const long long p = pos + y;
          if (p < top_ || p >= bottom_) continue;
          const int x0 = c * P, x1 = std::min(width_, e * P);
          canvas_.writeSpan(p, x0, x1, &copy.px[(size_t(y) * width_ + x0) * 4]);
          for (int k = c; k < e; ++k) store_->set(p, k, 1);
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
  for (const FrameCopy* copy : {&topCopy_, &bottomCopy_})
    for (int y = 0; y < height_; ++y) {
      const long long p = copy->pos + y;
      if (p < top_ || p >= bottom_) continue;
      const int r = std::min(y / BR, R - 1);
      const bool topSide = copy == &topCopy_;
      for (int c = 0; c < C;) {
        // Never under a part pinned at the other end: a side bar is at the top once.
        auto hole = [&](int cc) {
          const int l = label[size_t(r) * C + cc];
          return store_->level(p, cc) == 0 && copy->tag[size_t(r) * C + cc] == copy->serial &&
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
  const uint32_t page = pageColour_;
  std::vector<uint32_t> colOf(size_t(C), page);
  for (int c = 0; c < C; ++c) {
    long long most = 0;
    for (int r = 0; r < R; ++r) {
      const int l = label[size_t(r) * C + c];
      if (l >= 0 && parts[size_t(l)].blocks > most) {
        most = parts[size_t(l)].blocks;
        colOf[size_t(c)] = parts[size_t(l)].colour;
      }
    }
  }
  std::vector<uint8_t> fill(size_t(width_) * 4);
  store_->ensure(top_, bottom_);
  for (long long p = top_; p < bottom_; ++p)
    for (int c = 0; c < C;) {
      if (store_->level(p, c) > 0) {
        ++c;
        continue;
      }
      int e = c;
      while (e < C && store_->level(p, e) == 0) {
        const uint32_t v = colOf[size_t(e)];
        for (int x = e * P; x < std::min(width_, (e + 1) * P); ++x) std::memcpy(&fill[size_t(x) * 4], &v, 4);
        ++e;
      }
      const int x0 = c * P, x1 = std::min(width_, e * P);
      canvas_.writeSpan(p, x0, x1, &fill[size_t(x0) * 4]);
      c = e;
    }
  return totals_;
}

}  // namespace putgd::scroll
