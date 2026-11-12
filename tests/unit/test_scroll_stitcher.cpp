// The scroll stitcher and the control loop against a synthetic application
// (#1376): the research prototype's suite, trimmed to a few seconds, plus the
// loop's pointer rules and its scroll-to-top step, and the cases the real
// desktops taught (a stream that goes quiet, a slow application, numbered
// lines, an overscrolled view, Stop on the way up). `--full` (or
// PUTGD_SCROLL_STITCHER_FULL=1) runs the prototype's seed counts and sweeps.
// Plain checks with a summary table; exit code 1 on any unexpected failure.
// A wrong stitch is a valid-looking image, so every case compares pixels.
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "core/ScrollStitcher.h"
#include "fixtures/ScrollSynthetic.h"
#include "record/ScrollRun.h"

using namespace putgd::scroll;
using namespace putgd::record;
using namespace synth;
using putgd::scroll::toString;
using putgd::record::toString;

namespace {

struct Row {
  std::string name;
  bool pass;
  bool knownLimit;
  std::string detail;
};
std::vector<Row> g_rows;
bool g_quiet = false;
bool g_full = false;
bool g_lastSafeStop = false;
int g_lastWrongVerified = 0, g_lastWrongPartial = 0, g_lastWrongBlind = 0;

void report(const std::string& name, bool pass, const std::string& detail, bool knownLimit = false) {
  g_rows.push_back({name, pass, knownLimit, detail});
  if (g_quiet) return;
  std::printf("  %-44s %s  %s\n", name.c_str(), knownLimit ? "KNOWN-LIMIT" : (pass ? "PASS" : "FAIL"),
              detail.c_str());
  std::fflush(stdout);
}

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
  char buf[4096];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

void comparedColumns(int W, const StitchOptions& o, int& c0, int& c1) {
  Image one(W, 1, kBg);
  FrameSignatures s;
  s.compute(one.view(), o);
  c0 = s.colStart;
  c1 = s.colEnd;
}

struct Cmp {
  long long mismatches = 0;
  long long maskedMismatches = 0;
  int firstBad = -1;
  bool heightOk = false;
};

// Compares an output image with the expected one over the compared columns.
Cmp compare(const std::vector<uint8_t>& out, int outH, const Image& exp, int c0, int c1,
            const std::vector<uint8_t>* mask = nullptr) {
  Cmp c;
  c.heightOk = outH == exp.h && out.size() == size_t(exp.w) * outH * 4;
  const int H = std::min(outH, exp.h);
  if (out.size() < size_t(exp.w) * H * 4) return c;
  for (int y = 0; y < H; ++y) {
    const uint8_t* o = &out[size_t(y) * exp.w * 4];
    for (int x = c0; x < c1; ++x) {
      uint32_t v;
      std::memcpy(&v, o + 4 * x, 4);
      if ((v & 0xFFFFFF) != (exp.at(x, y) & 0xFFFFFF)) {
        if (mask && (*mask)[size_t(y) * exp.w + x]) {
          ++c.maskedMismatches;
        } else {
          ++c.mismatches;
          if (c.firstBad < 0) c.firstBad = y;
        }
      }
    }
  }
  if (!c.heightOk && c.firstBad < 0) c.firstBad = H;
  return c;
}

void addMask(std::vector<uint8_t>& mask, int W, int H, const std::vector<Rect>& rects, int pos) {
  for (const Rect& r : rects)
    for (int y = r.y; y < r.y + r.h; ++y) {
      const int oy = pos + y;  // content row y of a frame at scroll pos lands at output row pos + y
      if (oy < 0 || oy >= H) continue;
      for (int x = r.x; x < r.x + r.w && x < W; ++x) mask[size_t(oy) * W + x] = 1;
    }
}

std::vector<int> positions(int maxScroll, const std::function<int()>& step) {
  std::vector<int> p{0};
  while (p.back() < maxScroll) p.push_back(std::min(maxScroll, p.back() + std::max(1, step())));
  return p;
}

struct Drive {
  std::vector<StepResult> steps;
  std::vector<uint8_t> img;
  int outH = 0;
  int lowConf = 0, ambiguous = 0, noMatch = 0;
  std::vector<uint8_t> mask;
};

// Feeds frames at the given scroll positions; hint = true offset to the reference.
Drive drive(const Renderer& rend, const std::vector<int>& pos, bool useHint, StitchOptions o = {}) {
  Drive d;
  ScrollStitcher st(o);
  Image frame;
  int refPos = 0;
  const int W = rend.spec().width;
  const int maxOut = rend.spec().height + pos.back() + 10;
  d.mask.assign(size_t(W) * maxOut, 0);
  for (size_t i = 0; i < pos.size(); ++i) {
    rend.render(pos[i], int(i), frame);
    addMask(d.mask, W, maxOut, rend.noiseRects(), pos[i]);
    const int hint = (useHint && i > 0) ? pos[i] - refPos : kNoHint;
    StepResult r = st.addFrame(frame.view(), hint);
    d.steps.push_back(r);
    if (r.lowConfidence) ++d.lowConf;
    if (r.status == StepStatus::Ambiguous) ++d.ambiguous;
    if (r.status == StepStatus::NoMatch) ++d.noMatch;
    if (r.status == StepStatus::First || r.status == StepStatus::Appended ||
        r.status == StepStatus::NoMovement || r.status == StepStatus::Reversed)
      refPos = pos[i];
  }
  st.finish();
  d.outH = st.outputHeight();
  d.img = st.takeImage();
  return d;
}

// ------------------------------------------------------------------ stitcher tests

void testReconstruction() {
  std::printf("\n[stitcher] exact reconstruction\n");
  const StitchOptions o;
  struct Pat {
    const char* name;
    double lo, hi;
  } pats[] = {{"small15%", 0.15, 0.15}, {"step60%", 0.6, 0.6}, {"step90%", 0.9, 0.9}, {"random10-85%", 0.1, 0.85}};
  const std::vector<int> viewports = g_full ? std::vector<int>{300, 800, 1400} : std::vector<int>{300, 800};
  const uint64_t hintSeeds = g_full ? 10 : 3, plainSeeds = g_full ? 5 : 2;
  for (int V : viewports) {
    for (const Pat& pt : pats) {
      int ok = 0, total = 0, frames = 0, low = 0;
      std::string bad;
      for (uint64_t seed = 1; seed <= hintSeeds; ++seed) {
        DocSpec ds;
        ds.seed = seed;
        ds.viewport = V;
        Document doc = makeDocument(ds);
        ViewportSpec vs;
        vs.height = V;
        Renderer rend(doc.img, vs);
        Rng rng(seed * 31 + V);
        const int ch = rend.contentH();
        auto p = positions(rend.maxScroll(), [&] { return int(ch * (pt.lo + (pt.hi - pt.lo) * rng.unit())); });
        Drive d = drive(rend, p, true, o);
        int c0, c1;
        comparedColumns(vs.width, o, c0, c1);
        Cmp c = compare(d.img, d.outH, rend.expected(doc.img.h), c0, c1);
        ++total;
        frames += int(p.size());
        low += d.lowConf;
        if (c.mismatches == 0 && c.heightOk) ++ok;
        else bad += fmt(" seed%d(row%d,%s)", int(seed), c.firstBad, c.heightOk ? "h ok" : "h bad");
      }
      report(fmt("recon hint v=%d %s (%d seeds)", V, pt.name, int(hintSeeds)), ok == total,
             fmt("%d/%d exact, %d frames, %d low-confidence%s", ok, total, frames, low, bad.c_str()));
    }
  }
  // Without hints on content that has no blank/flat/periodic sections.
  for (int V : viewports) {
    for (double f : {0.15, 0.6}) {
      int ok = 0, total = 0, amb = 0;
      for (uint64_t seed = 1; seed <= plainSeeds; ++seed) {
        DocSpec ds;
        ds.seed = seed + 100;
        ds.viewport = V;
        ds.textOnly = true;
        Document doc = makeDocument(ds);
        ViewportSpec vs;
        vs.height = V;
        Renderer rend(doc.img, vs);
        auto p = positions(rend.maxScroll(), [&] { return int(rend.contentH() * f); });
        Drive d = drive(rend, p, false, o);
        int c0, c1;
        comparedColumns(vs.width, o, c0, c1);
        Cmp c = compare(d.img, d.outH, rend.expected(doc.img.h), c0, c1);
        ++total;
        amb += d.ambiguous + d.noMatch;
        if (c.mismatches == 0 && c.heightOk) ++ok;
      }
      report(fmt("recon no-hint text-only v=%d step%d%%", V, int(f * 100)), ok == total && amb == 0,
             fmt("%d/%d exact, %d ambiguous/nomatch", ok, total, amb));
    }
  }
}

void testSticky() {
  std::printf("\n[stitcher] sticky header / footer\n");
  const StitchOptions o;
  struct Cfg {
    const char* name;
    int hH, fH;
  } cfgs[] = {{"header only", 40, 0}, {"footer only", 0, 30}, {"header+footer", 40, 30}};
  for (int V : {300, 800}) {
    for (const Cfg& cf : cfgs) {
      int ok = 0;
      std::string det;
      for (uint64_t seed = 1; seed <= 3; ++seed) {
        DocSpec ds;
        ds.seed = seed;
        ds.viewport = V;
        Document doc = makeDocument(ds);
        ViewportSpec vs;
        vs.height = V;
        vs.headerH = cf.hH;
        vs.footerH = cf.fH;
        Renderer rend(doc.img, vs);
        auto p = positions(rend.maxScroll(), [&] { return int(rend.contentH() * 0.6); });
        Drive d = drive(rend, p, true, o);
        int c0, c1;
        comparedColumns(vs.width, o, c0, c1);
        Cmp c = compare(d.img, d.outH, rend.expected(doc.img.h), c0, c1);
        const StepResult& last = d.steps.back();
        const bool good = c.mismatches == 0 && c.heightOk && last.fixedTop == cf.hH && last.fixedBottom == cf.fH;
        ok += good;
        if (!good)
          det += fmt(" seed%d(bad row %d, top %d bot %d)", int(seed), c.firstBad, last.fixedTop, last.fixedBottom);
      }
      report(fmt("sticky %s v=%d", cf.name, V), ok == 3, fmt("%d/3 exact, fixed bands found%s", ok, det.c_str()));
    }
  }

  // Header over-estimated at step 1: content under the header is blank in both frames.
  {
    DocSpec ds;
    ds.seed = 4;
    ds.viewport = 300;
    ds.topBlank = 80;
    Document doc = makeDocument(ds);
    ViewportSpec vs;
    vs.height = 300;
    vs.headerH = 40;
    vs.footerH = 30;
    Renderer rend(doc.img, vs);
    std::vector<int> p{0, 30};
    while (p.back() < rend.maxScroll()) p.push_back(std::min(rend.maxScroll(), p.back() + 130));
    Drive d = drive(rend, p, true);
    int c0, c1;
    comparedColumns(vs.width, {}, c0, c1);
    Cmp c = compare(d.img, d.outH, rend.expected(doc.img.h), c0, c1);
    const int t1 = d.steps[1].fixedTop, tEnd = d.steps.back().fixedTop;
    report("header over-estimated at frame 2", t1 > 40 && tEnd == 40 && c.mismatches == 0 && c.heightOk,
           fmt("step1 top=%d (true 40), final top=%d, mismatches=%lld", t1, tEnd, c.mismatches));
  }
  // Footer over-estimated at step 1: content just above the footer blank in both frames.
  {
    DocSpec ds;
    ds.seed = 5;
    ds.viewport = 300;
    Document doc = makeDocument(ds);
    ViewportSpec vs;
    vs.height = 300;
    vs.headerH = 40;
    vs.footerH = 30;
    Renderer rend0(doc.img, vs);
    const int ch = rend0.contentH();
    fillRows(doc.img, ch - 40, ch + 40, kBg);
    Renderer rend(doc.img, vs);
    std::vector<int> p{0, 30};
    while (p.back() < rend.maxScroll()) p.push_back(std::min(rend.maxScroll(), p.back() + 130));
    Drive d = drive(rend, p, true);
    int c0, c1;
    comparedColumns(vs.width, {}, c0, c1);
    Cmp c = compare(d.img, d.outH, rend.expected(doc.img.h), c0, c1);
    const int b1 = d.steps[1].fixedBottom, bEnd = d.steps.back().fixedBottom;
    report("footer over-estimated at frame 2", b1 > 30 && bEnd == 30 && c.mismatches == 0 && c.heightOk,
           fmt("step1 bottom=%d (true 30), final bottom=%d, mismatches=%lld", b1, bEnd, c.mismatches));
  }
}

void testNoise() {
  std::printf("\n[stitcher] hover + caret + animated block + scrollbar\n");
  for (int V : {300, 800}) {
    for (int pat = 0; pat < 2; ++pat) {
      int ok = 0;
      long long masked = 0;
      std::string det;
      for (uint64_t seed = 1; seed <= 3; ++seed) {
        DocSpec ds;
        ds.seed = seed + 10;
        ds.viewport = V;
        Document doc = makeDocument(ds);
        ViewportSpec vs;
        vs.height = V;
        vs.headerH = 36;
        vs.footerH = 24;
        vs.hover = vs.caret = vs.anim = vs.scrollbar = true;
        Renderer rend(doc.img, vs);
        Rng rng(seed);
        const int ch = rend.contentH();
        auto p = positions(rend.maxScroll(), [&] { return pat == 0 ? int(ch * 0.6) : int(ch * (0.1 + 0.75 * rng.unit())); });
        Drive d = drive(rend, p, true);
        int c0, c1;
        comparedColumns(vs.width, {}, c0, c1);
        Cmp c = compare(d.img, d.outH, rend.expected(doc.img.h), c0, c1, &d.mask);
        masked += c.maskedMismatches;
        if (c.mismatches == 0 && c.heightOk) ++ok;
        else det += fmt(" seed%d(row %d, %lld px)", int(seed), c.firstBad, c.mismatches);
      }
      report(fmt("noise all-on v=%d %s", V, pat == 0 ? "step60%" : "random"), ok == 3 && masked > 0,
             fmt("%d/3 exact outside noise pixels (%lld noisy px in output)%s", ok, masked, det.c_str()));
    }
  }
}

// Two-frame helper: returns the second step and whether the output was untouched.
struct Pair {
  StepResult r;
  bool untouched;
  int outBefore, outAfter;
};
Pair pairStep(const Renderer& rend, int p0, int p1, int hint, int f0 = 0, int f1 = 1) {
  ScrollStitcher st;
  Image a, b;
  rend.render(p0, f0, a);
  rend.render(p1, f1, b);
  st.addFrame(a.view(), kNoHint);
  const int before = st.outputHeight();
  Pair pr;
  pr.r = st.addFrame(b.view(), hint);
  pr.outBefore = before;
  pr.outAfter = st.outputHeight();
  pr.untouched = before == pr.outAfter;
  return pr;
}

void testAmbiguity() {
  std::printf("\n[stitcher] periodic / blank / flat overlap, no overlap\n");
  {
    DocSpec ds;
    ds.seed = 2;
    ds.viewport = 300;
    ds.tableFactor = 3.0;
    Document doc = makeDocument(ds);
    ViewportSpec vs;
    vs.height = 300;
    Renderer rend(doc.img, vs);
    const Section* t = doc.find("table");
    const int p0 = t->y0 + 10, d = 100;
    Pair h = pairStep(rend, p0, p0 + d, d);
    report("periodic table, exact hint", h.r.status == StepStatus::Appended && h.r.offset == d,
           fmt("%s offset=%d lowConf=%d", toString(h.r.status), h.r.offset, h.r.lowConfidence));
    Pair h3 = pairStep(rend, p0, p0 + d, d + 3);
    report("periodic table, hint off by 3px", h3.r.status == StepStatus::Appended && h3.r.offset == d,
           fmt("%s offset=%d", toString(h3.r.status), h3.r.offset));
    Pair n = pairStep(rend, p0, p0 + d, kNoHint);
    report("periodic table, no hint", n.r.status == StepStatus::Ambiguous && n.untouched,
           fmt("%s, output %d->%d rows", toString(n.r.status), n.outBefore, n.outAfter));
    Pair w = pairStep(rend, p0, p0 + d, d + 12);
    report("periodic table, hint midway between periods", w.r.status == StepStatus::Ambiguous && w.untouched,
           fmt("%s (hint %d, period 24)", toString(w.r.status), d + 12));
  }
  {
    // Blank section: overlap is blank, new text enters at the bottom.
    DocSpec ds;
    ds.seed = 3;
    ds.viewport = 300;
    ds.blankFactor = 0.85;
    Document doc = makeDocument(ds);
    ViewportSpec vs;
    vs.height = 300;
    Renderer rend(doc.img, vs);
    const Section* b = doc.find("blank");
    const int p0 = b->y0 - 30, d = 90;
    Pair h = pairStep(rend, p0, p0 + d, d);
    report("blank overlap, hint", h.r.status == StepStatus::Appended && h.r.offset == d,
           fmt("%s offset=%d lowConf=%d evidence=%d", toString(h.r.status), h.r.offset, h.r.lowConfidence,
               h.r.evidenceRows));
    Pair n = pairStep(rend, p0, p0 + d, kNoHint);
    report("blank overlap, no hint", n.r.status == StepStatus::Ambiguous && n.untouched,
           fmt("%s, output untouched=%d", toString(n.r.status), n.untouched));
  }
  {
    DocSpec ds;
    ds.seed = 4;
    ds.viewport = 300;
    Document doc = makeDocument(ds);
    ViewportSpec vs;
    vs.height = 300;
    Renderer rend(doc.img, vs);
    const Section* f = doc.find("flat");
    const int p0 = f->y0 - 100, d = 200;
    Pair h = pairStep(rend, p0, p0 + d, d);
    report("flat block overlap, hint", h.r.status == StepStatus::Appended && h.r.offset == d,
           fmt("%s offset=%d lowConf=%d", toString(h.r.status), h.r.offset, h.r.lowConfidence));
    Pair n = pairStep(rend, p0, p0 + d, kNoHint);
    report("flat block overlap, no hint", n.r.status == StepStatus::Ambiguous && n.untouched,
           fmt("%s", toString(n.r.status)));
  }
  {
    DocSpec ds;
    ds.seed = 6;
    ds.viewport = 300;
    ds.textOnly = true;
    ds.paragraphsPerGap = 6;
    Document doc = makeDocument(ds);
    ViewportSpec vs;
    vs.height = 300;
    Renderer rend(doc.img, vs);
    int ok = 0, total = 0;
    std::string det;
    for (int p0 = 40; p0 < 1200; p0 += 97) {
      Pair n = pairStep(rend, p0, p0 + 320, kNoHint);
      Pair h = pairStep(rend, p0, p0 + 320, 320);
      total += 2;
      ok += (n.r.status == StepStatus::NoMatch && n.untouched);
      ok += (h.r.status == StepStatus::NoMatch && h.untouched);
      if (n.r.status != StepStatus::NoMatch) det += fmt(" p%d:%s", p0, toString(n.r.status));
    }
    report("no overlap (step 320 > band 300)", ok == total, fmt("%d/%d NoMatch, output untouched%s", ok, total, det.c_str()));
  }
}

// Lines of text that differ in a few characters only, all of them short: the
// numbered lines of a log, a listing or a plain text file in a wide window.
// `distinct` false: every line the same (content that repeats exactly).
Image numberedLines(int width, int lines, int pitch, bool distinct, bool rule = false) {
  Image img(width, lines * pitch + 8, kBg);
  // A vertical rule down the page, as an editor draws beside its line numbers or at its right margin.
  if (rule)
    for (int y = 0; y < img.h; ++y) img.at(400, y) = 0xC8C8C8;
  for (int i = 0; i < lines; ++i) {
    const int y = 4 + i * pitch;
    Rng same(77);  // the words every line shares
    drawTextLine(img, y, 130, std::min(width - 8, 310), same);  // with the digits: ink in two of eight bands
    if (!distinct) continue;
    // The line's number: four digit cells, each a small pattern of its digit.
    int n = i;
    for (int cell = 3; cell >= 0; --cell, n /= 10) {
      Rng digit(1000 + uint64_t(n % 10));
      drawTextLine(img, y, 70 + cell * 12, 70 + cell * 12 + 9, digit);
    }
  }
  return img;
}

void testNumberedLines() {
  std::printf("\n[stitcher] numbered lines (near-periodic text)\n");
  const int W = 1200, V = 700, pitch = 18;
  ViewportSpec vs;
  vs.width = W;
  vs.height = V;
  {
    // Real frames of a GTK 4 text view (KWin, 2026-11-10): at the true offset
    // every informative row is equal bit for bit, at the rivals a line pitch
    // away under 6% are. The tolerant rule alone called the step ambiguous.
    const Image doc = numberedLines(W, 400, pitch, true);
    Renderer rend(doc, vs);
    int ok = 0, total = 0;
    std::string det;
    for (int d : {13, 36, 38, 90, 450}) {
      for (int p0 : {0, 500, 2000}) {
        Pair n = pairStep(rend, p0, p0 + d, kNoHint);
        ++total;
        const bool good = n.r.status == StepStatus::Appended && n.r.offset == d && !n.r.lowConfidence;
        ok += good;
        if (!good) det += fmt(" p%d d%d:%s/%d", p0, d, toString(n.r.status), n.r.offset);
      }
    }
    report("numbered lines, no hint: the exact offset", ok == total, fmt("%d/%d verified at the true offset%s", ok, total, det.c_str()));
    // A wrong hint one line away does not win over the pixels.
    Pair w = pairStep(rend, 500, 538, 38 + pitch);
    report("numbered lines, hint one line off", w.r.status == StepStatus::Appended && w.r.offset == 38 && !w.r.lowConfidence,
           fmt("%s offset=%d lowConf=%d", toString(w.r.status), w.r.offset, w.r.lowConfidence));
    // The page did not move; a caret-sized change is in the frame.
    Pair still = pairStep(rend, 500, 500, kNoHint);
    report("numbered lines, no movement", still.r.status == StepStatus::NoMovement, fmt("%s", toString(still.r.status)));
  }
  {
    // The same beside a vertical rule (gnome-text-editor and Kate both draw
    // one). The rule is equal in every row, so under the tolerant rule every
    // offset at all explains the frame, hundreds of candidates, and the exact
    // rule has to judge them all: with the rule cut off at 256 candidates the
    // real editors ended as "lost track" on their first step.
    const Image doc = numberedLines(W, 400, pitch, true, true);
    Renderer rend(doc, vs);
    int ok = 0, total = 0;
    std::string det;
    for (int d : {13, 36, 38, 450}) {
      Pair n = pairStep(rend, 500, 500 + d, kNoHint);
      ++total;
      const bool good = n.r.status == StepStatus::Appended && n.r.offset == d && !n.r.lowConfidence;
      ok += good;
      if (!good) det += fmt(" d%d:%s/%d", d, toString(n.r.status), n.r.offset);
    }
    report("numbered lines beside a vertical rule, no hint", ok == total, fmt("%d/%d verified at the true offset%s", ok, total, det.c_str()));
  }
  {
    // Lines that are all the same repeat exactly: no pixel tells the offsets
    // apart, and the step stays ambiguous (never a guess).
    const Image doc = numberedLines(W, 400, pitch, false);
    Renderer rend(doc, vs);
    Pair n = pairStep(rend, 500, 538, kNoHint);
    report("identical lines, no hint", n.r.status == StepStatus::Ambiguous && n.untouched, fmt("%s, output untouched=%d", toString(n.r.status), n.untouched));
    Pair h = pairStep(rend, 500, 538, 38);
    report("identical lines, exact hint", h.r.status == StepStatus::Appended && h.r.offset == 38 && h.r.lowConfidence,
           fmt("%s offset=%d lowConf=%d", toString(h.r.status), h.r.offset, h.r.lowConfidence));
  }
  {
    // The whole file through the loop, as the real text views scroll: 37.5 px
    // a step (GTK 4 on KWin; whole pixels on screen), and 72 and 90 px in
    // turn (a Qt text view's four and five lines).
    for (int kind = 0; kind < 2; ++kind) {
      const Image doc = numberedLines(W, 300, pitch, true);
      FakeAppSpec as;
      as.pxPerNotch = kind ? 81 : 37.5;
      as.jitter = kind ? 0.111 : 0.0;
      FakeClock clock;
      FakeApp app(doc, vs, as, clock);
      RunOptions so;
      so.pointer = true;
      so.away = RunPoint{-12, V / 3};
      so.fromTop = true;
      int wrong = 0, refPos = 0;
      so.onStep = [&](const StepResult& r, int, int) {
        const int pos = app.framePositions.empty() ? 0 : app.framePositions.back();
        const bool accepted = r.status == StepStatus::Appended || r.status == StepStatus::Reversed || r.status == StepStatus::LimitReached;
        if (accepted && r.offset != pos - refPos) ++wrong;
        if (accepted || r.status == StepStatus::NoMovement) refPos = pos;
      };
      ScrollRun run(app, app, clock, so);
      const RunResult res = run.run();
      const std::vector<uint8_t> img = run.stitcher().takeImage();
      int c0, c1;
      comparedColumns(W, so.stitch, c0, c1);
      const Cmp c = compare(img, run.stats().outputRows, doc, c0, c1);
      report(kind ? "session numbered lines, 72 to 90 px a step" : "session numbered lines, 37.5 px a step",
             res == RunResult::EndOfContent && c.mismatches == 0 && c.heightOk && wrong == 0 && run.stats().steps < 60,
             fmt("%s, rows %d of %d, steps %d, wrong offsets %d, first bad %d", toString(res), run.stats().outputRows, doc.h, run.stats().steps, wrong, c.firstBad));
    }
  }
}

void testEndsAndBounce() {
  std::printf("\n[stitcher] end, bounce, negative offsets, limits, in-place changes\n");
  DocSpec ds;
  ds.seed = 7;
  ds.viewport = 300;
  Document doc = makeDocument(ds);
  ViewportSpec vs;
  vs.height = 300;
  vs.caret = vs.anim = true;
  Renderer rend(doc.img, vs);
  {
    const int end = rend.maxScroll();
    Pair e = pairStep(rend, end, end, 120);
    report("end of document (same frame, caret+anim)", e.r.status == StepStatus::NoMovement && e.untouched,
           fmt("%s changedRows=%d", toString(e.r.status), e.r.changedRows));
  }
  {
    ViewportSpec v2 = vs;
    v2.caret = v2.anim = false;
    Renderer r2(doc.img, v2);
    const std::vector<int> p{0, 150, 120, 270, 420};  // 150 -> 120 is a bounce back by 30
    Drive d = drive(r2, p, true);
    int c0, c1;
    comparedColumns(vs.width, {}, c0, c1);
    Cmp c = compare(d.img, d.outH, r2.expected(420 + r2.contentH()), c0, c1);
    const StepResult& b = d.steps[2];
    report("bounce back by 30 then continue", b.status == StepStatus::Reversed && b.offset == -30 &&
                                                   d.steps[3].appendedRows == 120 && c.mismatches == 0 && c.heightOk,
           fmt("%s offset=%d, next appended %d (150-30 already emitted), exact=%d", toString(b.status), b.offset,
               d.steps[3].appendedRows, c.mismatches == 0 && c.heightOk));
    Pair neg = pairStep(r2, 1500, 1420, kNoHint);
    report("negative offset mid-document", neg.r.status == StepStatus::Reversed && neg.r.offset == -80 && neg.untouched,
           fmt("%s offset=%d", toString(neg.r.status), neg.r.offset));
  }
  {
    ViewportSpec v2 = vs;
    v2.caret = v2.anim = false;
    v2.headerH = 30;
    v2.footerH = 20;
    Renderer r2(doc.img, v2);
    StitchOptions o;
    o.maxOutputRows = 1000;
    auto p = positions(r2.maxScroll(), [&] { return int(r2.contentH() * 0.6); });
    Drive d = drive(r2, p, true, o);
    bool sawLimit = false;
    for (auto& s : d.steps) sawLimit |= s.status == StepStatus::LimitReached;
    int c0, c1;
    comparedColumns(vs.width, o, c0, c1);
    Cmp c = compare(d.img, d.outH, r2.expected(1000 - 50), c0, c1);
    report("maxOutputRows=1000", sawLimit && d.outH == 1000 && c.mismatches == 0 && c.heightOk,
           fmt("LimitReached=%d, height=%d, prefix+footer exact=%d", sawLimit, d.outH, c.mismatches == 0));
  }
  {
    ViewportSpec v2 = vs;
    v2.caret = v2.anim = false;
    v2.footerH = 80;
    Renderer r2(doc.img, v2);
    StitchOptions o;
    o.maxOutputRows = 20;
    Drive d = drive(r2, {0, 100}, true, o);
    Image first; r2.render(0, 0, first);
    Image prefix(v2.width, 20, kBg);
    std::copy_n(first.px.begin(), prefix.px.size(), prefix.px.begin());
    const auto cmp = compare(d.img, d.outH, prefix, 0, v2.width);
    report("footer larger than output cap", cmp.heightOk && cmp.mismatches == 0, fmt("height=%d, exact prefix=%d", d.outH, cmp.heightOk && cmp.mismatches == 0));
  }
  {
    ViewportSpec v2 = vs;
    v2.caret = v2.anim = false;
    Renderer r2(doc.img, v2);
    Image a, b;
    r2.render(600, 0, a);
    b = a;
    for (int y = 140; y < 150; ++y)
      for (int x = 0; x < b.w; ++x) b.at(x, y) = 0x804020;  // 10-row in-place change
    ScrollStitcher st;
    st.addFrame(a.view());
    StepResult r = st.addFrame(b.view(), 120);
    report("small in-place change (10 rows)", r.status == StepStatus::NoMovement,
           fmt("%s changedRows=%d", toString(r.status), r.changedRows));
    Image c = a;
    for (int y = 60; y < 200; ++y)
      for (int x = 0; x < c.w; ++x) c.at(x, y) = 0x808080 ^ uint32_t((x * 13 + y * 7) & 0x3F);
    ScrollStitcher st2;
    st2.addFrame(a.view());
    const int before = st2.outputHeight();
    StepResult r2s = st2.addFrame(c.view(), 120);
    const bool safe = r2s.status != StepStatus::Appended && st2.outputHeight() == before;
    report("large in-place change (140 rows)", safe,
           fmt("%s (never appends; session cannot recover this, see notes)", toString(r2s.status)), true);
  }
  {
    // Blank section taller than the viewport.
    DocSpec d2 = ds;
    d2.blankFactor = 2.5;
    Document doc2 = makeDocument(d2);
    ViewportSpec v2;
    v2.height = 300;
    v2.scrollbar = true;
    Renderer rs(doc2.img, v2);
    const Section* bl = doc2.find("blank");
    std::vector<int> p;
    for (int q = bl->y0 - 200; q < bl->y1 + 200; q += 100) p.push_back(q);
    // positions() expects to start at 0: drive from the first position by shifting
    ScrollStitcher st;
    Image f;
    int refPos = p[0];
    bool allOk = true;
    int lowc = 0;
    for (size_t i = 0; i < p.size(); ++i) {
      rs.render(p[i], int(i), f);
      StepResult r = st.addFrame(f.view(), i ? p[i] - refPos : kNoHint);
      if (i && (r.status != StepStatus::Appended || r.offset != p[i] - refPos)) allOk = false;
      lowc += r.lowConfidence;
      refPos = p[i];
    }
    report("blank taller than viewport, scrollbar + hint", allOk,
           fmt("all steps appended at the hint (%d low-confidence)", lowc));
    ViewportSpec v3 = v2;
    v3.scrollbar = false;
    Renderer rn(doc2.img, v3);
    const int q = bl->y0 + 50;
    Pair pr = pairStep(rn, q, q + 100, 100);
    report("blank taller than viewport, no scrollbar", pr.r.status == StepStatus::NoMovement,
           fmt("%s: identical frames are indistinguishable from the end", toString(pr.r.status)), true);
  }
  {
    // Short lines only: all ink within two of eight bands.
    Image narrow(480, 3000, kBg);
    Rng rng(11);
    for (int y = 20; y + 16 < narrow.h; y += 16) drawTextLine(narrow, y, 30, 30 + rng.range(30, 90), rng);
    ViewportSpec v2;
    v2.height = 300;
    Renderer rr(narrow, v2);
    Pair pr = pairStep(rr, 500, 620, 120);
    report("short lines only (ink in <= 2 bands)", pr.r.status == StepStatus::Appended && pr.r.offset == 120,
           fmt("%s offset=%d changedRows=%d", toString(pr.r.status), pr.r.offset, pr.r.changedRows),
           pr.r.status != StepStatus::Appended);
  }
}

void testMemory() {
  std::printf("\n[stitcher] memory\n");
  for (auto [W, H] : {std::pair{1280, 800}, std::pair{3840, 2160}}) {
    if (!g_full && W > 1280) continue;
    Image f(W, H, kBg);
    Rng rng(1);
    for (int y = 10; y + 16 < H; y += 16) drawTextLine(f, y, 30, W - 30, rng);
    FrameSignatures s;
    s.compute(f.view(), {});
    report(fmt("signature bytes %dx%d", W, H), s.bytes() == size_t(H) * 166,
           fmt("%zu bytes = %.0f B/row (frame itself %zu bytes)", s.bytes(), double(s.bytes()) / H, size_t(W) * H * 4));
  }
}

// ------------------------------------------------------------------ margin rule and proof

// A wide window of short numbered lines, as an editor or a terminal shows a
// file: the digits (four cells from x = 33) lie inside the ignored side margin
// of a 1,900 px frame (76 px), and the words every line shares fill the first
// of the eight bands and no other. `words` false: nothing right of the margin.
// `proportional`: the digits have widths of their own, as in a proportional
// font, so the words stand a pixel or two further left or right from line to
// line, and two lines are alike right of the margin when their numbers are
// made of the same digits (0784 and 0874).
Image marginNumberedLines(int width, int lines, int pitch, bool distinct, bool words = true, bool proportional = false) {
  Image img(width, lines * pitch + 8, kBg);
  for (int i = 0; i < lines; ++i) {
    const int y = 4 + i * pitch;
    Rng same(77);
    int shift = 0;
    if (proportional)
      for (int n = i, cell = 0; cell < 4; ++cell, n /= 10) shift += (n % 10) % 3;
    if (words) drawTextLine(img, y, 84 + shift, 230 + shift, same);
    Rng row(55);  // "Row", the same on every line
    drawTextLine(img, y, 5, 28, row);
    if (!distinct) continue;
    int n = i;
    for (int cell = 3; cell >= 0; --cell, n /= 10) {
      Rng digit(1000 + uint64_t(n % 10));
      drawTextLine(img, y, 33 + cell * 7, 33 + cell * 7 + 6, digit);
    }
  }
  return img;
}

// The same file in a 1,260 px window (margin 50 px) with the number cut by the
// margin's edge, as Qt drew `Row 0042` on Budgie: thousands, hundreds and tens
// inside the margin, the units digit and the words beside it.
Image straddlingNumberedLines(int width, int lines, int pitch) {
  Image img(width, lines * pitch + 8, kBg);
  for (int i = 0; i < lines; ++i) {
    const int y = 4 + i * pitch;
    Rng same(77), row(55);
    drawTextLine(img, y, 62, 208, same);
    drawTextLine(img, y, 5, 22, row);
    int n = i;
    for (int cell = 3; cell >= 0; --cell, n /= 10) {
      Rng digit(1000 + uint64_t(n % 10));
      drawTextLine(img, y, 30 + cell * 7, 30 + cell * 7 + 6, digit);  // 30, 37, 44 | 51
    }
  }
  return img;
}

// Two frames given as images.
Pair pairFrames(const Image& a, const Image& b, int hint, int unc = 0) {
  ScrollStitcher st;
  FrameSignatures sa, sb;
  sa.compute(a.view(), st.options());
  sb.compute(b.view(), st.options());
  st.addFrame(a.view(), sa, kNoHint);
  Pair pr;
  pr.outBefore = st.outputHeight();
  pr.r = st.addFrame(b.view(), sb, hint, unc);
  pr.outAfter = st.outputHeight();
  pr.untouched = pr.outBefore == pr.outAfter;
  return pr;
}

// The bar-coded ruler page of the lab guests (ruler-vm.html) at 1280 px: rows
// of 40 lines, two separator lines, 28 lines of the row's number as 16 cells,
// ten fine lines.
Image vmRuler(int width, int rows) {
  Image img(width, rows * 40, 0xFAFAFA);
  const int cw = (width - 76) / 16;
  for (int y = 0; y < img.h; ++y) {
    const int row = y / 40, line = y % 40;
    uint32_t b;
    if (line < 2) b = 65535;
    else if (line < 30) b = uint32_t(row);
    else {
      uint32_t x = uint32_t(row) * 2654435761u + uint32_t(line) * 40503u + 0x9e3779b9u;
      x ^= x >> 15; x *= 0x2c1b3c6du; x ^= x >> 12; x *= 0x297a2d39u; x ^= x >> 15;
      b = x & 65535;
      if (b == 0 || b == 65535) b = 0x5a5au ^ uint32_t(line);
    }
    for (int k = 0; k < 16; ++k)
      if ((b >> (15 - k)) & 1)
        for (int x = 64 + k * cw; x < 64 + (k + 1) * cw; ++x) img.at(x, y) = 0x101010;
  }
  return img;
}

void testMarginRule() {
  std::printf("\n[stitcher] wide selection, short lines: the margins decide\n");
  const int W = 1900, V = 700, pitch = 18;
  ViewportSpec vs;
  vs.width = W;
  vs.height = V;
  {
    // Real frames of a full-width Qt text view (Plasma, 2026-11-10): the
    // compared columns were equal at every multiple of the line pitch, the
    // digits lay in the margin, and the first step ended the capture.
    const Image doc = marginNumberedLines(W, 400, pitch, true);
    Renderer rend(doc, vs);
    int ok = 0, total = 0;
    std::string det;
    for (int d : {13, 36, 38, 76, 90, 450}) {
      for (int p0 : {0, 500, 2000}) {
        Pair n = pairStep(rend, p0, p0 + d, kNoHint);
        ++total;
        const bool good = n.r.status == StepStatus::Appended && n.r.offset == d && !n.r.lowConfidence;
        ok += good;
        if (!good) det += fmt(" p%d d%d:%s/%d", p0, d, toString(n.r.status), n.r.offset);
      }
    }
    report("line numbers in the side margin, no hint: the exact offset", ok == total, fmt("%d/%d verified at the true offset%s", ok, total, det.c_str()));
    // Frames alike in every compared column (a step of whole lines): still the true offset, and never "no movement".
    Pair whole = pairStep(rend, 504, 504 + 72, kNoHint);
    report("line numbers in the side margin, a step of whole lines", whole.r.status == StepStatus::Appended && whole.r.offset == 72 && !whole.r.lowConfidence,
           fmt("%s offset=%d lowConf=%d", toString(whole.r.status), whole.r.offset, whole.r.lowConfidence));
    Pair back = pairStep(rend, 504 + 72, 504, kNoHint);
    report("line numbers in the side margin, whole lines back", back.r.status == StepStatus::Reversed && back.r.offset == -72,
           fmt("%s offset=%d", toString(back.r.status), back.r.offset));
    Pair wrong = pairStep(rend, 500, 538, 38 + pitch);
    report("line numbers in the side margin, hint one line off", wrong.r.status == StepStatus::Appended && wrong.r.offset == 38 && !wrong.r.lowConfidence,
           fmt("%s offset=%d lowConf=%d", toString(wrong.r.status), wrong.r.offset, wrong.r.lowConfidence));
    Pair still = pairStep(rend, 500, 500, kNoHint);
    report("line numbers in the side margin, no movement", still.r.status == StepStatus::NoMovement, fmt("%s", toString(still.r.status)));
  }
  {
    // Nothing but the numbers, all of it inside the margin: the compared columns are blank.
    const Image doc = marginNumberedLines(W, 400, pitch, true, false);
    Renderer rend(doc, vs);
    int ok = 0, total = 0;
    std::string det;
    for (int d : {13, 36, 90, 450}) {
      Pair n = pairStep(rend, 500, 500 + d, kNoHint);
      ++total;
      const bool good = n.r.status == StepStatus::Appended && n.r.offset == d && !n.r.lowConfidence;
      ok += good;
      if (!good) det += fmt(" d%d:%s/%d", d, toString(n.r.status), n.r.offset);
    }
    report("all the ink inside the side margin, no hint", ok == total, fmt("%d/%d verified at the true offset%s", ok, total, det.c_str()));
  }
  {
    // A number cut in two by the margin's edge (Budgie, lab guest of
    // 2026-11-11: a 1,260 px Qt text view ended as "lost track" after one
    // viewport, 3 of 3, `Ambiguous offset 4 ... score 1.00 evidence 455`).
    // Ten lines away only the margin differs, three lines away mostly the
    // compared columns: the first step of a capture, which has no hint, must
    // still be the true offset, proved.
    const int w = 1260, v = 600, p = 17;
    ViewportSpec narrow;
    narrow.width = w;
    narrow.height = v;
    const Image doc = straddlingNumberedLines(w, 1500, p);
    Renderer rend(doc, narrow);
    int ok = 0, total = 0;
    std::string det;
    for (int d : {13, 17, 51, 55, 306, 340})
      for (int p0 : {0, 4 + 96 * p, 4 + 1017 * p + 5}) {
        const Pair n = pairStep(rend, p0, p0 + d, kNoHint);
        ++total;
        const bool good = n.r.status == StepStatus::Appended && n.r.offset == d && !n.r.lowConfidence;
        ok += good;
        if (!good) det += fmt(" p%d d%d:%s/%d", p0, d, toString(n.r.status), n.r.offset);
      }
    report("a number cut by the margin's edge, no hint: the exact offset", ok == total, fmt("%d/%d verified at the true offset%s", ok, total, det.c_str()));
    const Pair back = pairStep(rend, 4 + 300 * p, 4 + 297 * p, kNoHint);
    report("a number cut by the margin's edge, three lines back", back.r.status == StepStatus::Reversed && back.r.offset == -3 * p, fmt("%s offset=%d", toString(back.r.status), back.r.offset));
    // The whole file through the loop, as the lab ran it: from the top, with
    // the pointer rules, three lines and the document's margin for the first notch.
    FakeAppSpec as;
    as.pxPerNotch = 3 * p;
    FakeClock clock;
    FakeApp app(doc, narrow, as, clock);
    RunOptions so;
    so.stitch.maxOutputRows = doc.h + 100;
    so.pointer = true;
    so.away = RunPoint{w + 16, v / 3};
    so.fromTop = true;
    ScrollRun run(app, app, clock, so);
    const RunResult res = run.run();
    const std::vector<uint8_t> img = run.stitcher().takeImage();
    const Cmp c = compare(img, run.stats().outputRows, doc, 0, w);
    report("a number cut by the margin's edge, the whole file", res == RunResult::EndOfContent && c.mismatches == 0 && c.heightOk && run.stats().lowConfidenceSteps == 0,
           fmt("%s, rows %d of %d, steps %d, low-confidence %d, first bad %d", toString(res), run.stats().outputRows, doc.h, run.stats().steps, run.stats().lowConfidenceSteps, c.firstBad));
  }
  {
    // What the margins must not decide. Lines that are all the same, numbers
    // and all: nothing tells the offsets apart.
    const Image same = marginNumberedLines(W, 400, pitch, false);
    Renderer rend(same, vs);
    Pair n = pairStep(rend, 500, 538, kNoHint);
    report("identical lines in a wide window, no hint", n.r.status == StepStatus::Ambiguous && n.untouched, fmt("%s, output untouched=%d", toString(n.r.status), n.untouched));
    // A fixed side bar inside the margin (marks every 10 px, the same in both
    // frames) beside lines that repeat: it lines up with itself at 20 px and
    // says nothing about the page, which moved 38.
    Image a, b;
    rend.render(500, 0, a);
    rend.render(538, 1, b);
    for (Image* f : {&a, &b})
      for (int y = 0; y < V; ++y)
        for (int x = 60; x < 72; ++x) f->at(x, y) = (y % 10) < 3 ? 0x404040 : 0xD0D0D0;
    Pair bar = pairFrames(a, b, kNoHint);
    report("a fixed side bar in the margin proves nothing", bar.r.status == StepStatus::Ambiguous && bar.untouched, fmt("%s offset=%d", toString(bar.r.status), bar.r.offset));
    // A scrollbar thumb in the right margin that moved with the page: a long
    // one, and a short one that lines up with itself exactly at a rival offset
    // (34 px the other way). Forty rows that are all alike prove nothing.
    for (int length : {300, 40}) {
      rend.render(500, 0, a);
      rend.render(538, 1, b);
      int at = 100;
      for (Image* f : {&a, &b}) {
        for (int y = at; y < at + length; ++y)
          for (int x = W - 10; x < W - 2; ++x) f->at(x, y) = 0x808080;
        at += 34;
      }
      Pair thumb = pairFrames(a, b, kNoHint);
      report(length > 100 ? "a long scrollbar thumb in the margin proves nothing" : "a short scrollbar thumb in the margin proves nothing", thumb.r.status == StepStatus::Ambiguous && thumb.untouched,
             fmt("%s offset=%d", toString(thumb.r.status), thumb.r.offset));
    }
  }
  {
    // The whole file through the loop, full width compared: every line number
    // is in the image once. 37.5 px a step (GTK 4), and 72 to 90 (Qt).
    for (int kind = 0; kind < 2; ++kind) {
      const Image doc = marginNumberedLines(W, 300, pitch, true);
      FakeAppSpec as;
      as.pxPerNotch = kind ? 81 : 37.5;
      as.jitter = kind ? 0.111 : 0.0;
      FakeClock clock;
      FakeApp app(doc, vs, as, clock);
      RunOptions so;
      so.pointer = true;
      so.away = RunPoint{-12, V / 3};
      so.fromTop = true;
      int wrong = 0, refPos = 0;
      so.onStep = [&](const StepResult& r, int, int) {
        const int pos = app.framePositions.empty() ? 0 : app.framePositions.back();
        const bool accepted = r.status == StepStatus::Appended || r.status == StepStatus::Reversed || r.status == StepStatus::LimitReached;
        if (accepted && r.offset != pos - refPos) ++wrong;
        if (accepted || r.status == StepStatus::NoMovement) refPos = pos;
      };
      ScrollRun run(app, app, clock, so);
      const RunResult res = run.run();
      const std::vector<uint8_t> img = run.stitcher().takeImage();
      const Cmp c = compare(img, run.stats().outputRows, doc, 0, W);
      report(kind ? "session wide window of numbered lines, 72 to 90 px a step" : "session wide window of numbered lines, 37.5 px a step",
             res == RunResult::EndOfContent && c.mismatches == 0 && c.heightOk && wrong == 0 && run.stats().steps < 60,
             fmt("%s, rows %d of %d, steps %d, wrong offsets %d, first bad %d", toString(res), run.stats().outputRows, doc.h, run.stats().steps, wrong, c.firstBad));
    }
  }
}

void testProof() {
  std::printf("\n[stitcher] an offset is appended only when the pixels prove it\n");
  {
    // A Qt text view on Hyprland (lab guest, 2026-11-10): numbered lines 14 px
    // apart in a 940 px frame, the page three lines further down, and the
    // mouse pointer the compositor painted into the second frame. Every row
    // but the pointer's passed the tolerant rule at the same place (the lines
    // differ in their digits only), the step was called a change in place,
    // and three lines were missing from an image reported as complete.
    const int W = 940, V = 600, pitch = 14;
    Image doc(W, 400 * pitch + 8, kBg);
    for (int i = 0; i < 400; ++i) {
      Rng same(77);
      drawTextLine(doc, 6 + i * pitch, 110, 300, same);
      int n = i;
      for (int cell = 3; cell >= 0; --cell, n /= 10) {
        Rng digit(1000 + uint64_t(n % 10));
        drawTextLine(doc, 6 + i * pitch, 60 + cell * 10, 60 + cell * 10 + 8, digit);
      }
    }
    ViewportSpec vs;
    vs.width = W;
    vs.height = V;
    Renderer rend(doc, vs);
    auto pointer = [](Image& f) {
      for (int y = 0; y < 20; ++y)
        for (int x = 0; x <= y * 12 / 20; ++x) f.at(470 + x, 200 + y) = x == 0 || x == y * 12 / 20 ? 0xFFFFFF : 0x000000;
    };
    int ok = 0, total = 0;
    std::string det;
    for (int p : {0, 700, 2000})
      for (int d : {14, 42, 336}) {
        Image a, b;
        rend.render(p, 0, a);
        rend.render(p + d, 1, b);
        pointer(b);
        const Pair n = pairFrames(a, b, kNoHint);
        ++total;
        const bool good = n.r.status == StepStatus::Appended && n.r.offset == d && n.outAfter == n.outBefore + d;
        ok += good;
        if (!good) det += fmt(" p%d d%d:%s/%d", p, d, toString(n.r.status), n.r.offset);
      }
    report("short lines scrolled under a painted pointer: not a change in place", ok == total, fmt("%d/%d appended at the true offset%s", ok, total, det.c_str()));
    Image a, b;
    rend.render(700, 0, a);
    rend.render(700, 1, b);
    pointer(b);
    const Pair still = pairFrames(a, b, kNoHint);
    report("short lines, only the pointer appeared: no movement", still.r.status == StepStatus::NoMovement && still.untouched, fmt("%s", toString(still.r.status)));
  }
  {
    // The frames of the neon guest's third browser run (KWin 6.8.1, build
    // 289a61189), positions from its helper log: 0, 35, the end (no match),
    // back at 0, 625 (a frame from the middle of the animation), the end.
    // The last pair has no overlap at all (975 px in an 800 px viewport); 25
    // rows of row 35's number stripes matched row 40's at 775 and 200 px
    // went missing from an image reported as complete.
    const int W = 1280, V = 800;
    const Image doc = vmRuler(W, 60);
    auto frame = [&](int pos) {
      Image f(W, V);
      std::memcpy(f.px.data(), doc.row(pos), size_t(W) * V * 4);
      return f;
    };
    ScrollStitcher st;
    std::string trace;
    auto step = [&](int pos, int hint, int unc = 0) {
      const Image f = frame(pos);
      const StepResult r = st.addFrame(f.view(), hint);
      (void)unc;
      trace += fmt(" %d:%s/%d", pos, toString(r.status), r.offset);
      return r;
    };
    step(0, kNoHint);
    const StepResult first = step(35, kNoHint);
    const StepResult far = step(1600, 490);
    const StepResult back = step(0, 0);
    const StepResult mid = step(625, 245);
    const int before = st.outputHeight();
    const StepResult end = step(1600, 446);
    const bool refused = end.status != StepStatus::Appended && end.status != StepStatus::Reversed && st.outputHeight() == before;
    st.finish();
    const std::vector<uint8_t> img = st.takeImage();
    Image prefix(W, st.outputHeight());
    std::memcpy(prefix.px.data(), doc.row(0), size_t(W) * prefix.h * 4);
    const Cmp c = compare(img, st.outputHeight(), prefix, 0, W);
    report("guest replay: a jump past the viewport is not matched on a sliver",
           first.status == StepStatus::Appended && first.offset == 35 && far.status == StepStatus::NoMatch && back.status == StepStatus::Reversed && back.offset == -35 &&
               mid.status == StepStatus::Appended && mid.offset == 625 && refused && c.mismatches == 0 && c.heightOk && st.outputHeight() == 625 + V,
           fmt("steps%s; image %d rows, first bad %d", trace.c_str(), st.outputHeight(), c.firstBad));
    // The same for every pair of ruler positions further apart than the
    // viewport allows, whatever the loop expected: never an offset.
    int accepted = 0, pairs = 0;
    std::string det;
    for (int p = 0; p + V <= doc.h; p += 120)
      for (int phase : {0, 25})
        for (int q = p + phase + 780; q + V <= doc.h; q += 20)
          for (int hint : {kNoHint, 446}) {
            const Pair n = pairFrames(frame(p + phase), frame(q), hint);
            ++pairs;
            if (n.r.status == StepStatus::Appended || n.r.status == StepStatus::Reversed || !n.untouched) {
              ++accepted;
              if (det.size() < 200) det += fmt(" %d->%d hint %d: %s/%d", p + phase, q, hint, toString(n.r.status), n.r.offset);
            }
          }
    report("ruler: no pair further apart than the viewport is ever matched", accepted == 0 && pairs > 300, fmt("%d of %d pairs accepted%s", accepted, pairs, det.c_str()));
    // And the ruler still stitches at every distance the loop plans (up to three quarters of the viewport).
    int ok = 0, total = 0;
    for (int p : {0, 35, 625, 990})
      for (int d : {1, 35, 147, 206, 480, 600}) {
        const Pair n = pairFrames(frame(p), frame(p + d), kNoHint);
        ++total;
        const bool good = n.r.status == StepStatus::Appended && n.r.offset == d && !n.r.lowConfidence;
        ok += good;
        if (!good) det += fmt(" p%d d%d:%s/%d", p, d, toString(n.r.status), n.r.offset);
      }
    report("ruler: planned distances are verified", ok == total, fmt("%d/%d%s", ok, total, det.c_str()));
  }
  {
    // After a real Plasma run (2026-11-10): a GTK 4 text view of numbered
    // lines, 1900 x 1080, the page 637 px further down, and a stack of
    // notification popups that stayed where it was (rows 261 to 993, in two of
    // the eight bands). At the true offset the rows under and beside the
    // popups are not equal bit for bit; 983 px the other way the overlap was
    // 97 rows clear of them, the exact rule called that proved, and 90 lines
    // went missing, twice, from an image reported as complete. Here the same
    // geometry: the margins prove the true offset, and nothing else is taken.
    const int W = 1900, V = 1080, pitch = 18;
    ViewportSpec vs;
    vs.width = W;
    vs.height = V;
    auto withPopups = [&](const Image& doc, int pos) {
      Renderer rend(doc, vs);
      Image f;
      rend.render(pos, 0, f);
      Rng rng(9);
      for (int y = 261; y < 993; ++y)
        for (int x = 1544; x < 1823; ++x) f.at(x, y) = rng.next() % 5 ? 0x2E2C29 : 0xEFEFEF;
      return f;
    };
    {
      // From line 784 down, as in the run: 90 lines on, 0874 to 0879 are the
      // same digits as 0784 to 0789.
      const Image doc = marginNumberedLines(W, 1500, pitch, true, true, true);
      int ok = 0, total = 0;
      std::string det;
      for (int p : {784 * pitch + 4, 3000, 20000})
        for (int hint : {kNoHint, 638}) {
          const Pair n = pairFrames(withPopups(doc, p), withPopups(doc, p + 637), hint, 17);
          ++total;
          const bool good = n.r.status == StepStatus::Appended && n.r.offset == 637;
          ok += good;
          if (!good) det += fmt(" p%d hint %d:%s/%d", p, hint, toString(n.r.status), n.r.offset);
        }
      report("desktop replay: popups over numbered lines, the true offset", ok == total, fmt("%d/%d%s", ok, total, det.c_str()));
    }
    {
      // Without the numbers nothing proves an offset: never one the other way.
      const Image doc = marginNumberedLines(W, 1500, pitch, false);
      const Pair n = pairFrames(withPopups(doc, 3000), withPopups(doc, 3637), kNoHint);
      // A hint that may be 17 px off cannot choose between lines 18 px apart; an exact one can, with low confidence.
      const Pair u = pairFrames(withPopups(doc, 3000), withPopups(doc, 3637), 638, 17);
      const Pair h = pairFrames(withPopups(doc, 3000), withPopups(doc, 3637), 638, 0);
      report("desktop replay: popups over identical lines, no hint", n.r.status == StepStatus::Ambiguous && n.untouched, fmt("%s offset=%d", toString(n.r.status), n.r.offset));
      report("desktop replay: popups over identical lines, uncertain hint", u.r.status == StepStatus::Ambiguous && u.untouched, fmt("%s offset=%d", toString(u.r.status), u.r.offset));
      report("desktop replay: popups over identical lines, exact hint", h.r.status == StepStatus::Appended && h.r.offset == 637 && h.r.lowConfidence,
             fmt("%s offset=%d lowConf=%d", toString(h.r.status), h.r.offset, h.r.lowConfidence));
    }
  }
}

// ------------------------------------------------------------------ the layout inside the rectangle changes

// What the captured rectangle shows when the page's viewport is only a part of
// it: the document from row `pos` in the viewport's rectangle, the window's
// background everywhere else.
struct ViewRect {
  int x = 0, y = 0, w = 0, h = 0;
};
constexpr uint32_t kWindowBg = 0x666666;
Image layoutFrame(const Image& doc, int W, int V, int pos, ViewRect v) {
  Image f(W, V, kWindowBg);
  for (int y = 0; y < v.h && pos + y < doc.h; ++y)
    std::memcpy(&f.at(v.x, v.y + y), doc.row(pos + y), size_t(std::min(v.w, doc.w)) * 4);
  return f;
}
void shrinkTo(Image& f, ViewRect v) {
  for (int y = 0; y < f.h; ++y)
    for (int x = 0; x < f.w; ++x)
      if (x < v.x || x >= v.x + v.w || y < v.y || y >= v.y + v.h) f.at(x, y) = kWindowBg;
}
// The first output row that is not the document's row of that number, over
// every column; -1 when the image is the document's first rows and nothing else.
int firstForeignRow(const std::vector<uint8_t>& img, int outH, const Image& doc) {
  if (img.size() != size_t(doc.w) * size_t(std::max(outH, 0)) * 4) return 0;
  for (int y = 0; y < outH; ++y)
    if (y >= doc.h || std::memcmp(&img[size_t(y) * doc.w * 4], doc.row(y), size_t(doc.w) * 4) != 0) return y;
  return -1;
}

struct LayoutDrive {
  std::string trace;
  int outH = 0, foreign = -1;
  int acceptedAfter = 0;   // frames from the change on that the stitcher took for the page going on
  bool changeSaid = false;
};
// Frames at `pos`, the full rectangle until frame `changeAt`, the viewport
// `after` from there on; hints are the true distances, as a loop that knows
// its wheel would give them.
LayoutDrive driveLayout(const Image& doc, int W, int V, const std::vector<int>& pos, size_t changeAt, ViewRect after) {
  LayoutDrive d;
  ScrollStitcher st;
  int refPos = pos[0];
  for (size_t i = 0; i < pos.size(); ++i) {
    const Image f = layoutFrame(doc, W, V, pos[i], i < changeAt ? ViewRect{0, 0, W, V} : after);
    const StepResult r = st.addFrame(f.view(), i ? pos[i] - refPos : kNoHint);
    d.trace += fmt(" %d:%s/%d+%d", pos[i], toString(r.status), r.offset, r.appendedRows);
    const bool accepted = r.status == StepStatus::Appended || r.status == StepStatus::NoMovement || r.status == StepStatus::Reversed;
    if (accepted || r.status == StepStatus::First) refPos = pos[i];
    if (i >= changeAt && accepted) ++d.acceptedAfter;
    if (r.status == StepStatus::LayoutChanged) d.changeSaid = true;
  }
  st.finish();
  d.outH = st.outputHeight();
  d.foreign = firstForeignRow(st.takeImage(), d.outH, doc);
  return d;
}

struct LayoutSession {
  RunResult result = RunResult::NoFrames;
  int outH = 0, foreign = -1, steps = 0;
  std::string error;
  std::vector<uint8_t> img;
  bool sinkAgrees = false;  // the rows a sink was left with are the image
};
// The control loop on the ruler: `overlay` paints what is beside the page. The
// rows go to a sink, as in the helper, with the rule's way to take rows back.
LayoutSession layoutSession(const Image& doc, int W, int V, double px, std::function<void(Image&, int)> overlay, bool fromTop = false, int startPos = 0) {
  ViewportSpec vs;
  vs.width = W;
  vs.height = V;
  FakeAppSpec spec;
  spec.pxPerNotch = px;
  spec.overlay = std::move(overlay);
  spec.startPos = startPos;
  FakeClock clock;
  FakeApp app(doc, vs, spec, clock);
  RunOptions opts;
  opts.fromTop = fromTop;
  const char* traced = std::getenv("PUTGD_SCROLL_STITCHER_TRACE");
  if (traced && std::strstr("layout", traced))
    opts.onStep = [&](const StepResult& r, int hint, int notches) {
      std::printf("    trace layout: t=%lld pos %d %s offset %d hint %d notches %d score %.2f evidence %d appended %d fixed %d/%d\n", (long long)clock.nowMs(), app.framePositions.empty() ? 0 : app.framePositions.back(),
                  toString(r.status), r.offset, hint, notches, r.score, r.evidenceRows, r.appendedRows, r.fixedTop, r.fixedBottom);
    };
  LayoutSession s;
  ScrollRun run(app, app, clock, opts, [&](const uint8_t* row, int width) { s.img.insert(s.img.end(), row, row + size_t(width) * 4); },
                [&](long long rows) { s.img.resize(s.img.size() - size_t(rows) * W * 4); });
  s.result = run.run();
  s.outH = run.stats().outputRows;
  s.steps = run.stats().steps;
  s.error = run.stats().lastError;
  s.sinkAgrees = s.img.size() == size_t(W) * size_t(s.outH) * 4;
  s.foreign = s.sinkAgrees ? firstForeignRow(s.img, s.outH, doc) : 0;
  return s;
}

// Acceptance of 2026-11-11 (GNOME and Plasma): a 24,000 px ruler in a 960x600
// rectangle, and the window under the rectangle resized while the capture ran.
// The frames kept their size; the page's viewport inside them did not. Rows
// from before and after such a change are not one picture of one page.
void testLayoutChange() {
  std::printf("\n[stitcher] the page's viewport changes inside a frame of constant size\n");
  const int W = 960, V = 600;
  const Image doc = vmRuler(W, 120);  // 4,800 rows
  struct Change {
    const char* name;
    ViewRect after;
  } changes[] = {{"narrower and shorter", {0, 0, 760, 500}}, {"narrower", {0, 0, 760, 600}}, {"shorter", {0, 0, 960, 500}},
                 {"narrower by less than a band", {0, 0, 880, 600}}, {"shorter from the top", {0, 100, 960, 500}}};
  for (const Change& ch : changes)
    for (int step : {40, 60, 240, 360}) {
      std::vector<int> pos{0, 60};
      while (pos.back() + step + V <= 3000) pos.push_back(pos.back() + step);
      const size_t changeAt = 5;
      const LayoutDrive d = driveLayout(doc, W, V, pos, changeAt, ch.after);
      const int before = pos[changeAt - 1] + V;
      // Nothing but the document's first rows, and none of them from after the
      // change (a frame that shows less of the page shows other rows of it).
      // A band that comes up at the top hides rows that are in the image
      // already: whatever becomes of the run, no row of it is foreign.
      const bool fromTop = ch.after.y > 0;
      report(fmt("viewport %s after 4 steps, step %d", ch.name, step), d.foreign < 0 && (fromTop || (d.outH <= before && d.outH >= pos[changeAt - 2] + V && d.changeSaid)),
             fmt("%d rows (%d before the change), first foreign row %d, %d frames accepted after it, said %d;%s", d.outH, before, d.foreign, d.acceptedAfter, d.changeSaid, d.trace.substr(0, 400).c_str()));
    }
  {
    // The way back after a refused step shows the new layout where the last
    // stitched frame was: that is not "the reference found again".
    for (const Change& ch : changes) {
      ScrollStitcher st;
      const ViewRect full{0, 0, W, V};
      st.addFrame(layoutFrame(doc, W, V, 0, full).view(), kNoHint);
      st.addFrame(layoutFrame(doc, W, V, 60, full).view(), kNoHint);
      st.addFrame(layoutFrame(doc, W, V, 420, full).view(), 360);
      const int before = st.outputHeight();
      const StepResult back = st.addFrame(layoutFrame(doc, W, V, 420, ch.after).view(), 0);
      const StepResult on = st.addFrame(layoutFrame(doc, W, V, 780, ch.after).view(), 360);
      st.finish();
      const int outH = st.outputHeight();
      const int foreign = firstForeignRow(st.takeImage(), outH, doc);
      report(fmt("viewport %s where the page stands: not the reference", ch.name), foreign < 0 && outH <= before && on.status != StepStatus::Appended,
             fmt("in place %s, next %s/%d, %d rows (%d before), first foreign row %d", toString(back.status), toString(on.status), on.offset, outH, before, foreign));
    }
  }

  std::printf("\n[session] the page's viewport changes during the capture\n");
  for (const Change& ch : changes)
    for (double px : {40.0, 60.0, 100.0})
      for (int at : {3, 9}) {
        const ViewRect after = ch.after;
        const LayoutSession s = layoutSession(doc, W, V, px, [after, at](Image& f, int injections) {
          if (injections >= at) shrinkTo(f, after);
        });
        const bool fromTop = ch.after.y > 0;
        report(fmt("session viewport %s at wheel event %d, px=%g", ch.name, at, px), s.foreign < 0 && s.outH >= V && (fromTop || (s.result == RunResult::LostTrack && s.outH < doc.h)),
               fmt("%s, %d rows of %d, first foreign row %d, steps %d [%s]", toString(s.result), s.outH, doc.h, s.foreign, s.steps, s.error.c_str()));
      }

  std::printf("\n[session] what is not a change of layout\n");
  {
    // The rectangle is larger than the page's viewport from the first frame:
    // margins and a band below it that never move.
    const ViewRect view{60, 0, 800, 520};
    const LayoutSession s = layoutSession(doc, W, V, 60, [view](Image& f, int) {
      Image page = f;
      std::fill(f.px.begin(), f.px.end(), kWindowBg);
      for (int y = 0; y < view.h; ++y) std::memcpy(&f.at(view.x, view.y + y), page.row(y), size_t(view.w) * 4);
    });
    // The page as that viewport shows it (the fake page scrolls as far as a
    // full viewport would, so its last rows are never seen), between the
    // margins, then the band.
    Image want(W, doc.h, kWindowBg);
    for (int y = 0; y < doc.h - (V - view.h); ++y) std::memcpy(&want.at(view.x, y), doc.row(y), size_t(view.w) * 4);
    const int bad = s.sinkAgrees ? firstForeignRow(s.img, s.outH, want) : 0;
    report("session margins and a band that were always there", s.result == RunResult::EndOfContent && s.outH == doc.h && bad < 0,
           fmt("%s, %d rows, first wrong row %d, steps %d [%s]", toString(s.result), s.outH, bad, s.steps, s.error.c_str()));
  }
  {
    // The acceptance run itself (2026-11-11): the capture began at the end of
    // the page with scroll-to-top on, and the window was resized while the
    // page was on its way up, before the capture's first frame. Every frame
    // that was stitched showed the new layout, so there is no change to see:
    // the image is the page as the rectangle showed it from then on, every row
    // once, between the window's background. That is a whole capture.
    const ViewRect view{0, 0, 760, 500};
    const LayoutSession s = layoutSession(doc, W, V, 60, [view](Image& f, int injections) {
      if (injections >= 2) shrinkTo(f, view);
    }, true, 4000);
    Image want(W, doc.h, kWindowBg);
    for (int y = 0; y < doc.h - (V - view.h); ++y) std::memcpy(&want.at(0, y), doc.row(y), size_t(view.w) * 4);
    const int bad = s.sinkAgrees ? firstForeignRow(s.img, s.outH, want) : 0;
    report("session viewport resized on the way to the top: the layout the capture saw", s.result == RunResult::EndOfContent && s.outH == doc.h && bad < 0,
           fmt("%s, %d rows, first wrong row %d, steps %d [%s]", toString(s.result), s.outH, bad, s.steps, s.error.c_str()));
  }
  for (int at : {1, 2}) {
    // A bar the page pins to the bottom once it has scrolled: not in the first
    // frame, in every frame after wheel event `at`.
    const int bar = 40;
    Image footer(W, bar, 0x806020);
    Rng rng(5);
    drawTextLine(footer, 12, 40, 600, rng, 0xFFFFFF);
    LayoutSession s = layoutSession(doc, W, V, 60, [&](Image& f, int injections) {
      if (injections >= at) std::memcpy(&f.at(0, V - bar), footer.row(0), size_t(W) * bar * 4);
    });
    if (at == 1 && s.sinkAgrees && s.outH == doc.h) {
      // The document as far as the bar lets it be seen, then the bar, once.
      Image want(W, doc.h);
      std::memcpy(want.px.data(), doc.row(0), size_t(W) * (doc.h - bar) * 4);
      std::memcpy(&want.at(0, doc.h - bar), footer.row(0), size_t(W) * bar * 4);
      s.foreign = firstForeignRow(s.img, s.outH, want);
    }
    if (at == 1)
      report("session a bar pinned from the first scroll on", s.result == RunResult::EndOfContent && s.foreign < 0 && s.outH == doc.h,
             fmt("%s, %d rows of %d, first foreign row %d, steps %d [%s]", toString(s.result), s.outH, doc.h, s.foreign, s.steps, s.error.c_str()));
    else
      report("session a bar pinned after rows were taken from under it", s.result == RunResult::LostTrack && s.foreign < 0 && s.outH < doc.h,
             fmt("%s, %d rows of %d, first foreign row %d, steps %d [%s]", toString(s.result), s.outH, doc.h, s.foreign, s.steps, s.error.c_str()));
  }
  {
    // A scrollbar in the side margin that fades in with the first wheel event
    // and out again a while after the last, and a text cursor that blinks.
    int lastInjections = -1, framesSince = 0;
    const LayoutSession s = layoutSession(doc, W, V, 60, [&](Image& f, int injections) {
      framesSince = injections == lastInjections ? framesSince + 1 : 0;
      lastInjections = injections;
      if (injections >= 2 && framesSince < 12)
        for (int y = 100 + injections * 5; y < 220 + injections * 5 && y < f.h; ++y)
          for (int x = W - 12; x < W - 2; ++x) f.at(x, y) = 0x9A9A9A;
      if ((framesSince / 5) % 2 == 0)
        for (int y = 300; y < 316; ++y) f.at(500, y) = 0x000000;
    });
    report("session a scrollbar that fades and a cursor that blinks", s.result == RunResult::EndOfContent && s.outH == doc.h,
           fmt("%s, %d rows of %d, steps %d [%s]", toString(s.result), s.outH, doc.h, s.steps, s.error.c_str()));
  }
}

// ------------------------------------------------------------------ live pages

uint32_t rgb(int r, int g, int b) { return uint32_t((std::clamp(r, 0, 255) << 16) | (std::clamp(g, 0, 255) << 8) | std::clamp(b, 0, 255)); }

// The control loop on a page that does what web pages do (acceptance of
// 2026-11-12, a pull request on GitHub in Firefox: 16 of 16 captures ended as
// "lost track" a few steps in, with the browser's link panel stitched into
// the image several times). `scene` paints it; the rows go to a sink.
struct LiveSession {
  RunResult result = RunResult::NoFrames;
  int outH = 0, steps = 0, scrollBacks = 0;
  std::string error;
  std::vector<uint8_t> img;
  std::vector<FakeApp::Event> events;
  bool sinkAgrees = false;
};
LiveSession liveSession(const Image& doc, int W, int V, double px, std::function<void(Image&, const FakeApp&, int, int64_t)> scene, RunOptions opts = {}, int startPos = 0) {
  ViewportSpec vs;
  vs.width = W;
  vs.height = V;
  FakeAppSpec spec;
  spec.pxPerNotch = px;
  spec.scene = std::move(scene);
  spec.startPos = startPos;
  FakeClock clock;
  FakeApp app(doc, vs, spec, clock);
  const char* traced = std::getenv("PUTGD_SCROLL_STITCHER_TRACE");
  if (traced && std::strstr("live", traced))
    opts.onStep = [&](const StepResult& r, int hint, int notches) {
      std::printf("    trace live: t=%lld pos %d %s offset %d hint %d notches %d score %.2f evidence %d appended %d fixed %d/%d\n", (long long)clock.nowMs(), app.framePositions.empty() ? 0 : app.framePositions.back(),
                  toString(r.status), r.offset, hint, notches, r.score, r.evidenceRows, r.appendedRows, r.fixedTop, r.fixedBottom);
    };
  LiveSession s;
  ScrollRun run(app, app, clock, opts, [&](const uint8_t* row, int width) { s.img.insert(s.img.end(), row, row + size_t(width) * 4); },
                [&](long long rows) { s.img.resize(s.img.size() - size_t(rows) * W * 4); });
  s.result = run.run();
  s.outH = run.stats().outputRows;
  s.steps = run.stats().steps;
  s.scrollBacks = run.stats().scrollBacks;
  s.error = run.stats().lastError;
  s.events = app.events;
  s.sinkAgrees = s.img.size() == size_t(W) * size_t(s.outH) * 4;
  return s;
}
// The first row of the image that differs from `want` in columns [x0, x1); -1 when none does.
int firstWrongRow(const LiveSession& s, const Image& want, int x0, int x1, int rows) {
  if (!s.sinkAgrees) return 0;
  for (int y = 0; y < rows; ++y)
    if (y >= s.outH || y >= want.h || std::memcmp(&s.img[(size_t(y) * want.w + x0) * 4], want.row(y) + x0, size_t(x1 - x0) * 4) != 0) return y;
  return -1;
}
// A bar with something on it: a colour, a line of text, a border below.
Image barImage(int W, int rows, uint32_t color, uint64_t seed) {
  Image bar(W, rows, color);
  Rng rng(seed);
  drawTextLine(bar, std::max(2, rows / 2 - 8), 24, W - 200, rng, 0xFFFFFF);
  for (int x = 0; x < W; ++x) bar.at(x, rows - 1) = 0x30363D;
  return bar;
}
// A browser's link panel: at the bottom left of the viewport while the
// pointer has been still over the page for 80 ms, and for 250 ms after it left.
struct LinkPanel {
  Image panel;
  bool visible = false;
  explicit LinkPanel(int width, bool shownAtStart = false) : panel(width, 22, 0xF9F9FA), visible(shownAtStart) {
    Rng rng(11);
    drawTextLine(panel, 3, 6, width - 10, rng, 0x15141A);
    for (int x = 0; x < width; ++x) panel.at(x, 0) = 0xB1B1B3;
    for (int y = 0; y < panel.h; ++y) panel.at(width - 1, y) = 0xB1B1B3;
  }
  void paint(Image& f, const FakeApp& app, int64_t t) {
    const bool inside = app.pointerKnown && app.pointerX >= 0 && app.pointerX < f.w && app.pointerY >= 0 && app.pointerY < f.h;
    if (inside && t - app.pointerMovedAt >= 80) visible = true;
    if (!inside && app.pointerKnown && t - app.pointerMovedAt >= 250) visible = false;
    if (!visible) return;
    for (int y = 0; y < panel.h; ++y) std::memcpy(&f.at(0, f.h - 2 - panel.h + y), panel.row(y), size_t(panel.w) * 4);
  }
};

void testLivePages() {
  std::printf("\n[session] live pages: a header that comes late, a link panel, a line at the edge\n");
  const int W = 960, V = 600;
  const Image doc = vmRuler(W, 120);  // 4,800 rows
  {
    // The page's sticky header: not there at the top of the page, over the
    // first rows of the viewport from 150 px on. The image is the page; the
    // header, which a browser's own full-page picture does not show either,
    // is in none of its rows.
    for (int headerRows : {48, 80, 120})
      for (double px : {60.0, 100.0, 138.0}) {
        const Image header = barImage(W, headerRows, 0x24292F, 3);
        const LiveSession s = liveSession(doc, W, V, px, [&](Image& f, const FakeApp&, int pos, int64_t) {
          if (pos >= 150) std::memcpy(&f.at(0, 0), header.row(0), size_t(W) * header.h * 4);
        });
        const int bad = firstWrongRow(s, doc, 0, W, doc.h);
        report(fmt("session sticky header of %d rows that comes after 150 px, px=%g", headerRows, px), s.result == RunResult::EndOfContent && s.outH == doc.h && bad < 0,
               fmt("%s, %d rows of %d, first wrong row %d, steps %d, scroll-backs %d [%s]", toString(s.result), s.outH, doc.h, bad, s.steps, s.scrollBacks, s.error.c_str()));
      }
  }
  {
    // The same header whose text changes once on the way (the section the
    // page is in): the estimate falls back to nothing and the header is found again.
    const Image one = barImage(W, 64, 0x24292F, 3), two = barImage(W, 64, 0x24292F, 4);
    const LiveSession s = liveSession(doc, W, V, 100, [&](Image& f, const FakeApp&, int pos, int64_t) {
      if (pos >= 150) std::memcpy(&f.at(0, 0), (pos >= 2000 ? two : one).row(0), size_t(W) * one.h * 4);
    });
    const int bad = firstWrongRow(s, doc, 0, W, doc.h);
    report("session sticky header that comes late and changes its text", s.result == RunResult::EndOfContent && s.outH == doc.h && bad < 0,
           fmt("%s, %d rows of %d, first wrong row %d, steps %d, scroll-backs %d [%s]", toString(s.result), s.outH, doc.h, bad, s.steps, s.scrollBacks, s.error.c_str()));
  }
  for (bool restAway : {false, true}) {
    // The link panel a browser shows while the pointer rests on the page. With
    // the pointer parked in the rectangle (no place outside it) the panel is
    // on the screen in every frame, and the edge guard alone keeps it out of
    // every row but the last frame's; with the pointer waiting outside it
    // never shows.
    LinkPanel panel(360);
    RunOptions opts;
    opts.pointer = true;
    if (restAway) {
      opts.away = RunPoint{W / 2, -16};
      opts.restAway = true;
      opts.awayLingerMs = 450;
    }
    const LiveSession s = liveSession(doc, W, V, 100, [&](Image& f, const FakeApp& app, int, int64_t t) { panel.paint(f, app, t); }, opts);
    // Without a place outside, the last viewport is the screen as it was: the panel is in its last rows.
    const int clean = restAway ? doc.h : doc.h - 24;
    const int bad = firstWrongRow(s, doc, 0, W, clean);
    bool visits = true;
    for (size_t i = 0; restAway && i < s.events.size(); ++i)
      if (s.events[i].kind == 's') visits = visits && i + 1 < s.events.size() && s.events[i + 1].kind == 'm' && s.events[i + 1].a == W / 2 && s.events[i + 1].b == -16;
    report(restAway ? "session link panel under a resting pointer: the pointer waits outside" : "session link panel under a pointer that cannot leave: not in the body", s.result == RunResult::EndOfContent && s.outH == doc.h && bad < 0 && visits,
           fmt("%s, %d rows of %d, first wrong row %d of the first %d, pointer leaves after every wheel step %d, steps %d [%s]", toString(s.result), s.outH, doc.h, bad, clean, visits, s.steps, s.error.c_str()));
  }
  {
    // The user's own pointer rested on a link when the capture began: the
    // panel is on the screen and goes a quarter of a second after the pointer left.
    LinkPanel panel(360, true);
    RunOptions opts;
    opts.pointer = true;
    opts.away = RunPoint{W / 2, -16};
    opts.restAway = true;
    opts.awayLingerMs = 450;
    const LiveSession s = liveSession(doc, W, V, 100, [&](Image& f, const FakeApp& app, int, int64_t t) { panel.paint(f, app, t); }, opts);
    const int bad = firstWrongRow(s, doc, 0, W, doc.h);
    report("session link panel on the screen when the capture begins", s.result == RunResult::EndOfContent && s.outH == doc.h && bad < 0,
           fmt("%s, %d rows of %d, first wrong row %d, steps %d [%s]", toString(s.result), s.outH, doc.h, bad, s.steps, s.error.c_str()));
  }
  {
    // A window's border in the rectangle's last row, blended with what is
    // under it, so that it is never the same row twice and no footer: it was
    // a line across the image at the end of every step.
    const LiveSession s = liveSession(doc, W, V, 100, [&](Image& f, const FakeApp&, int, int64_t) {
      for (int x = 0; x < W; ++x) {
        const uint32_t v = f.at(x, V - 1);
        f.at(x, V - 1) = rgb(36 + ((v >> 16) & 255) / 64, 36 + ((v >> 8) & 255) / 64, 40 + (v & 255) / 64);
      }
    });
    const int bad = firstWrongRow(s, doc, 0, W, doc.h - 1);
    report("session a blended line in the last row of the rectangle", s.result == RunResult::EndOfContent && s.outH == doc.h && bad < 0,
           fmt("%s, %d rows of %d, first wrong row %d (the image's last row is the line), steps %d [%s]", toString(s.result), s.outH, doc.h, bad, s.steps, s.error.c_str()));
  }

  std::printf("\n[session] a column that scrolls at a pace of its own\n");
  // The acceptance fixture: a 100 px strip at the right that scrolls at a
  // third of the page's rate (a code editor's minimap), one colour a row.
  const auto strip = [&](Image& f, int first) {
    for (int y = 0; y < f.h; ++y) {
      const int n = first + y;
      const uint32_t c = rgb(30 + (n * 17) % 190, 40 + (n * 23) % 180, 50 + (n * 31) % 170);
      for (int x = W - 100; x < W; ++x) f.at(x, y) = c;
    }
  };
  for (double px : {40.0, 60.0, 100.0})
    for (int start : {0, 3000}) {
      const LiveSession s = liveSession(doc, W, V, px, [&](Image& f, const FakeApp&, int pos, int64_t) { strip(f, pos / 3); }, [&] { RunOptions o; o.fromTop = start > 0; return o; }(), start);
      // The first viewport, whole, with the strip as it stood beside it: nothing else is one picture.
      Image want(W, V);
      std::memcpy(want.px.data(), doc.row(0), size_t(W) * V * 4);
      strip(want, 0);
      const int bad = firstWrongRow(s, want, 0, W, s.outH);
      report(fmt("session minimap at a third of the page's rate, px=%g, from %d", px, start), s.result == RunResult::LostTrack && s.outH == V && bad < 0 && s.error.find("pace of its own") != std::string::npos,
             fmt("%s, %d rows (the first viewport has %d), first wrong row %d, steps %d [%s]", toString(s.result), s.outH, V, bad, s.steps, s.error.c_str()));
    }
  {
    // What must go on to the end as before. A strip that never moves (a side
    // bar): the page beside it is whole.
    const LiveSession s = liveSession(doc, W, V, 60, [&](Image& f, const FakeApp&, int, int64_t) { strip(f, 0); });
    const int bad = firstWrongRow(s, doc, 0, W - 100, doc.h);
    report("session a strip that stays put is not a pace of its own", s.result == RunResult::EndOfContent && s.outH == doc.h && bad < 0,
           fmt("%s, %d rows of %d, first wrong row %d beside the strip, steps %d [%s]", toString(s.result), s.outH, doc.h, bad, s.steps, s.error.c_str()));
  }
  for (int stick : {90, 150, 400}) {
    // A sticky side bar: it scrolls with the page until `stick`, then stays;
    // in one step it has moved part of the way.
    const LiveSession s = liveSession(doc, W, V, 60, [&](Image& f, const FakeApp&, int pos, int64_t) { strip(f, std::min(pos, stick)); });
    const int bad = firstWrongRow(s, doc, 0, W - 100, doc.h);
    report(fmt("session a side bar that sticks after %d px is not a pace of its own", stick), s.result == RunResult::EndOfContent && s.outH == doc.h && bad < 0,
           fmt("%s, %d rows of %d, first wrong row %d beside the strip, steps %d [%s]", toString(s.result), s.outH, doc.h, bad, s.steps, s.error.c_str()));
  }
  {
    // Something that changes without moving (a video in a column): no
    // distance explains it, the tolerant rule goes on as before.
    const LiveSession s = liveSession(doc, W, V, 60, [&](Image& f, const FakeApp&, int, int64_t t) {
      for (int y = 100; y < 400; ++y)
        for (int x = W - 100; x < W - 40; ++x) f.at(x, y) = rgb(int((t / 50 * 37 + y * 3) & 255), int((x * 5 + t / 50 * 11) & 255), 90);
    });
    const int bad = firstWrongRow(s, doc, 0, W - 100, doc.h);
    report("session a video in a column is not a pace of its own", s.result == RunResult::EndOfContent && s.outH == doc.h && bad < 0,
           fmt("%s, %d rows of %d, first wrong row %d beside it, steps %d [%s]", toString(s.result), s.outH, doc.h, bad, s.steps, s.error.c_str()));
  }

  std::printf("\n[stitcher] the edge guard\n");
  {
    // Something over the last rows of one frame only (a panel, a toast): the
    // next frame shows those rows without it, and the image has them from there.
    for (int step : {60, 240, 360}) {
      ScrollStitcher st;
      std::vector<int> pos{0};
      while (pos.back() + step + V <= 3000) pos.push_back(pos.back() + step);
      int refPos = 0;
      for (size_t i = 0; i < pos.size(); ++i) {
        Image f(W, V);
        std::memcpy(f.px.data(), doc.row(pos[i]), size_t(W) * V * 4);
        if (i % 3 == 1)
          for (int y = V - 30; y < V - 4; ++y)
            for (int x = 0; x < 200; ++x) f.at(x, y) = 0xF9F9FA;
        const StepResult r = st.addFrame(f.view(), i ? pos[i] - refPos : kNoHint);
        if (r.status == StepStatus::First || r.status == StepStatus::Appended) refPos = pos[i];
      }
      // The last frame once more, as the loop sees it when the page has ended.
      Image f(W, V);
      std::memcpy(f.px.data(), doc.row(pos.back()), size_t(W) * V * 4);
      st.addFrame(f.view(), 0);
      st.finish();
      const int outH = st.outputHeight();
      const int foreign = firstForeignRow(st.takeImage(), outH, doc);
      report(fmt("a panel over the last rows of every third frame, step %d", step), foreign < 0 && outH == pos.back() + V, fmt("%d rows (%d), first foreign row %d", outH, pos.back() + V, foreign));
    }
  }
}

// ------------------------------------------------------------------ session tests

struct SessCfg {
  std::string name;
  int V = 800;
  int W = 480;
  double px = 100;
  int hH = 0, fH = 0;
  bool noise = false;
  FakeAppSpec app;
  RunOptions so;
  DocSpec ds;
  RunResult expect = RunResult::EndOfContent;
  bool expectFull = true;   // output must equal the whole document
  bool known = false;
  std::function<bool(const RunStats&, const FakeApp&)> extra;
  std::string extraName;
};

void runSession(SessCfg cfg) {
  cfg.ds.viewport = cfg.V;
  cfg.ds.width = cfg.W;
  Document doc = makeDocument(cfg.ds);
  ViewportSpec vs;
  vs.width = cfg.W;
  vs.height = cfg.V;
  vs.headerH = cfg.hH;
  vs.footerH = cfg.fH;
  vs.hover = vs.caret = vs.anim = vs.scrollbar = cfg.noise;
  if (cfg.app.pxPerNotch == FakeAppSpec{}.pxPerNotch) cfg.app.pxPerNotch = cfg.px;
  FakeClock clock;
  FakeApp app(doc.img, vs, cfg.app, clock);
  // Ground truth per stitch attempt: classify wrong offsets by confidence.
  int refPos = 0, wrongVerified = 0, wrongPartial = 0, wrongBlind = 0, blind = 0;
  // PUTGD_SCROLL_STITCHER_TRACE=<part of a session's name>: every stitch attempt of those sessions.
  const char* traced = std::getenv("PUTGD_SCROLL_STITCHER_TRACE");
  const bool trace = traced && cfg.name.find(traced) != std::string::npos;
  cfg.so.onStep = [&](const StepResult& r, int hint, int notches) {
    const int pos = app.framePositions.empty() ? 0 : app.framePositions.back();
    if (trace)
      std::printf("    trace %s: t=%lld %s offset %d (true %d) hint %d notches %d score %.2f evidence %d appended %d fixed %d/%d%s\n", cfg.name.c_str(), (long long)clock.nowMs(),
                  toString(r.status), r.offset, pos - refPos, hint, notches, r.score, r.evidenceRows, r.appendedRows, r.fixedTop, r.fixedBottom, r.lowConfidence ? " low" : "");
    const bool accepted = r.status == StepStatus::Appended || r.status == StepStatus::Reversed ||
                          r.status == StepStatus::LimitReached;
    if (accepted && r.lowConfidence && r.evidenceRows == 0) ++blind;
    if (accepted && r.offset != pos - refPos) {
      if (!r.lowConfidence) ++wrongVerified;
      else if (r.evidenceRows > 0) ++wrongPartial;
      else ++wrongBlind;
    }
    if (accepted || r.status == StepStatus::NoMovement) refPos = pos;
  };
  ScrollRun sess(app, app, clock, cfg.so);
  const auto t0 = std::chrono::steady_clock::now();
  const RunResult res = sess.run(cfg.app.cancelFlag);
  const double wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const RunStats& st = sess.stats();
  std::vector<uint8_t> img = sess.stitcher().takeImage();
  const int outH = st.outputRows;
  const Renderer& rend = app.renderer();
  int c0, c1;
  comparedColumns(vs.width, cfg.so.stitch, c0, c1);
  std::vector<uint8_t> mask(size_t(vs.width) * std::max(1, outH + cfg.V), 0);
  for (int p : app.framePositions) addMask(mask, vs.width, outH + cfg.V, rend.noiseRects(), p);
  const int docRows = cfg.expectFull ? doc.img.h : std::max(0, outH - cfg.hH - cfg.fH);
  Cmp c = compare(img, outH, rend.expected(docRows), c0, c1, &mask);
  const bool exact = c.mismatches == 0 && c.heightOk;
  bool ok = res == cfg.expect && exact;
  std::string extra;
  if (cfg.extra) {
    const bool e = cfg.extra(st, app);
    ok = ok && e;
    extra = fmt(" %s=%d", cfg.extraName.c_str(), e);
  }
  {
    Cmp pc = compare(img, outH, rend.expected(std::max(0, outH - cfg.hH - cfg.fH)), c0, c1, &mask);
    g_lastSafeStop = res != RunResult::EndOfContent && pc.mismatches == 0 && pc.heightOk;
  }
  g_lastWrongVerified = wrongVerified;
  g_lastWrongPartial = wrongPartial;
  g_lastWrongBlind = wrongBlind;
  extra += fmt(" wrong steps v/p/b %d/%d/%d of %d blind", wrongVerified, wrongPartial, wrongBlind, blind);
  ok = ok && wrongVerified == 0;
  report("session " + cfg.name, ok,
         fmt("%s, %s (rows %d/%d), steps %d, notches %d, px/notch %.1f-%.1f, retries %d, back %d, "
             "falseEnd %d, fake %.1fs, wall %.0fms%s%s",
             toString(res), exact ? "exact" : fmt("BAD row %d", c.firstBad).c_str(), outH,
             cfg.expectFull ? rend.expected(doc.img.h).h : outH, st.steps, st.notchesInjected, st.minPxPerNotch,
             st.maxPxPerNotch, st.retries, st.scrollBacks, st.falseEnds, st.elapsedMs / 1000.0, wallMs,
             extra.c_str(), st.lastError.empty() ? "" : (" [" + st.lastError + "]").c_str()),
         cfg.known);
}

void testSessions() {
  std::printf("\n[session] fake app\n");
  for (int V : {300, 800}) {
    for (double px : {40.0, 53.0, 100.0, 120.0}) {
      SessCfg c;
      c.name = fmt("smooth v=%d px=%g", V, px);
      c.V = V;
      c.px = px;
      c.ds.seed = uint64_t(px) + V;
      runSession(c);
    }
  }
  {
    SessCfg c;
    c.name = "header+footer+noise v=800 px=53";
    c.px = 53;
    c.hH = 48;
    c.fH = 28;
    c.noise = true;
    runSession(c);
  }
  {
    SessCfg c;
    c.name = "noise v=300 px=40";
    c.V = 300;
    c.px = 40;
    c.hH = 30;
    c.noise = true;
    runSession(c);
  }
  {
    SessCfg c;
    c.name = "dropped event (3rd injection)";
    c.app.dropInjection = 2;
    c.extraName = "falseEnds>=1";
    c.extra = [](const RunStats& s, const FakeApp&) { return s.falseEnds >= 1; };
    runSession(c);
  }
  {
    SessCfg c;
    c.name = "dropped first injection";
    c.app.dropInjection = 0;
    runSession(c);
  }
  {
    SessCfg c;
    c.name = "lazy load at 60% (250ms after end)";
    c.ds.seed = 21;
    Document probe = makeDocument([&] { DocSpec d = c.ds; d.viewport = c.V; return d; }());
    c.app.lazyLoadAt = int(probe.img.h * 0.6);
    c.extraName = "falseEnds>=1";
    c.extra = [](const RunStats& s, const FakeApp&) { return s.falseEnds >= 1; };
    runSession(c);
  }
  {
    SessCfg c;
    c.name = "lazy load slower than confirm delay";
    c.ds.seed = 22;
    Document probe = makeDocument([&] { DocSpec d = c.ds; d.viewport = c.V; return d; }());
    c.app.lazyLoadAt = int(probe.img.h * 0.6);
    c.app.lazyDelayMs = 2000;
    c.expectFull = false;
    c.known = true;
    runSession(c);
  }
  {
    SessCfg c;
    c.name = "elastic bounce at end";
    c.app.elastic = true;
    c.extraName = "overscrollFramesSeen";
    c.extra = [](const RunStats&, const FakeApp& a) { return a.overscrollFrames > 0; };
    runSession(c);
  }
  {
    SessCfg c;
    c.name = "damage-only frame stream";
    c.app.damageOnly = true;
    c.px = 120;
    runSession(c);
  }
  {
    SessCfg c;
    c.name = "slow animation 400-700ms";
    c.app.animMinMs = 400;
    c.app.animMaxMs = 700;
    c.px = 53;
    runSession(c);
  }
  {
    SessCfg c;
    c.name = "accelerating app (x2.5 for >=3 notches)";
    c.px = 40;
    c.app.accelFactor = 2.5;
    c.extraName = "scrollBacks>=1";
    c.extra = [](const RunStats& s, const FakeApp&) { return s.scrollBacks >= 1; };
    runSession(c);
  }
  {
    SessCfg c;
    c.name = "accelerating app, scroll-back disabled";
    c.px = 40;
    c.app.accelFactor = 2.5;
    c.so.allowScrollBack = false;
    c.expect = RunResult::LostTrack;
    c.expectFull = false;
    runSession(c);
  }
  {
    SessCfg c;
    c.name = "one notch larger than overlap";
    c.V = 300;
    c.px = 290;
    c.expect = RunResult::LostTrack;
    c.expectFull = false;
    runSession(c);
  }
  static std::atomic<bool> cancel{false};
  {
    SessCfg c;
    c.name = "cancel after 5 injections";
    c.app.cancelAfterInjections = 5;
    c.app.cancelFlag = &cancel;
    c.expect = RunResult::Stopped;
    c.expectFull = false;
    runSession(c);
  }
  {
    SessCfg c;
    c.name = "total time limit 3s";
    c.so.totalTimeLimitMs = 3000;
    c.expect = RunResult::TimeLimit;
    c.expectFull = false;
    runSession(c);
  }
  {
    SessCfg c;
    c.name = "injector failure at 4th injection";
    c.app.failInjection = 3;
    c.expect = RunResult::InjectorFailed;
    c.expectFull = false;
    runSession(c);
  }
  {
    SessCfg c;
    c.name = "maxOutputRows=2500";
    c.hH = 40;
    c.fH = 20;
    c.so.stitch.maxOutputRows = 2500;
    c.expect = RunResult::LimitReached;
    c.expectFull = false;
    c.extraName = "height==2500";
    c.extra = [](const RunStats& s, const FakeApp&) { return s.outputRows == 2500; };
    runSession(c);
  }
  {
    SessCfg c;
    c.name = "blank taller than viewport, scrollbar";
    c.V = 300;
    c.ds.blankFactor = 2.0;
    c.app.pxPerNotch = 60;
    c.noise = true;
    runSession(c);
  }
  {
    SessCfg c;
    c.name = "blank taller than viewport, no scrollbar";
    c.V = 300;
    c.ds.blankFactor = 2.0;
    c.px = 60;
    c.expectFull = false;
    c.known = true;
    runSession(c);
  }
  {
    // Randomised sweeps: seeds x px-per-notch x viewport, random features.
    for (int jitter = 0; jitter < 2; ++jitter) {
      int ok = 0, total = 0, wv = 0, wp = 0, wb = 0, badOnlyBlind = 0, safeStops = 0;
      std::string bad;
      const int seeds = g_full ? (jitter ? 8 : 16) : (jitter ? 1 : 2);
      for (uint64_t seed = 1; seed <= uint64_t(seeds); ++seed)
        for (double px : {40.0, 53.0, 100.0, 120.0})
          for (int V : {300, 800}) {
            Rng rng(seed * 1000 + uint64_t(px) + V);
            SessCfg c;
            c.V = V;
            c.W = seed > 8 ? (seed % 2 ? 640 : 1024) : 480;
            c.px = px;
            c.ds.seed = seed * 77 + V;
            c.noise = rng.unit() < 0.5;
            c.hH = rng.unit() < 0.5 ? rng.range(20, 60) : 0;
            c.fH = rng.unit() < 0.5 ? rng.range(16, 40) : 0;
            c.app.dropInjection = rng.unit() < 0.3 ? rng.range(0, 6) : -1;
            c.app.elastic = rng.unit() < 0.3;
            c.app.damageOnly = !c.noise && rng.unit() < 0.5;
            c.app.jitter = jitter ? 0.1 : 0.0;
            c.app.seed = seed;
            c.name = fmt("s%d w%d px%g v%d n%d h%d f%d drop%d el%d dmg%d", int(seed), c.W, px, V, c.noise, c.hH, c.fH,
                         c.app.dropInjection, c.app.elastic, c.app.damageOnly);
            const size_t before = g_rows.size();
            g_quiet = true;
            runSession(c);
            g_quiet = false;
            ++total;
            wv += g_lastWrongVerified;
            wp += g_lastWrongPartial;
            wb += g_lastWrongBlind;
            if (g_rows.back().pass) ok++;
            else if (g_lastWrongVerified == 0 && g_lastWrongPartial == 0 && g_lastWrongBlind > 0) ++badOnlyBlind;
            else if (g_lastSafeStop) {
              ++safeStops;
              bad += " [stopped early: " + g_rows.back().name + ": " + g_rows.back().detail + "]";
            }
            else bad += " [" + g_rows.back().name + ": " + g_rows.back().detail + "]";
            g_rows.resize(before);  // fold into one summary row
          }
      if (jitter)
        report(fmt("session sweep, px/notch jitter +-10%% (%d runs)", total), ok == total,
               fmt("%d/%d exact; %d inexact only through blind steps; %d stopped early with an exact prefix; "
                   "wrong steps verified/partial/blind %d/%d/%d%s",
                   ok, total, badOnlyBlind, safeStops, wv, wp, wb, bad.c_str()),
               ok != total && ok + badOnlyBlind + safeStops == total && wv == 0);
      else if (false) {
      } else
        report(fmt("session sweep, constant px/notch (%d runs)", total), ok == total,
               fmt("%d/%d exact + EndOfContent, wrong steps verified/partial/blind %d/%d/%d%s", ok, total, wv, wp,
                   wb, bad.c_str()));
    }
  }
  {
    // Document shorter than the viewport: nothing can scroll.
    Image small(480, 200, kBg);
    Rng rng(5);
    for (int y = 10; y < 180; y += 16) drawTextLine(small, y, 30, 400, rng);
    ViewportSpec vs;
    vs.height = 300;
    FakeClock clock;
    FakeApp app(small, vs, {}, clock);
    ScrollRun s(app, app, clock, {});
    const RunResult r = s.run();
    report("session document shorter than viewport", r == RunResult::EndOfContent && s.stats().outputRows == 300,
           fmt("%s, rows %d, steps %d", toString(r), s.stats().outputRows, s.stats().steps));
  }
}

void testReviewFrameProofs() {
  {
    DocSpec ds; ds.width = 480; ds.viewport = 600;
    const auto doc = makeDocument(ds);
    ViewportSpec vs; vs.width = 480; vs.height = 600;
    FakeClock clock; FakeAppSpec spec;
    spec.startPos = 1500; spec.pxPerNotch = 53;
    spec.wheelArea = {360, 0, 120, 600};
    FakeApp app(doc.img, vs, spec, clock);
    RunOptions opts; opts.fromTop = true; opts.pointer = true; opts.away = RunPoint{-12, 200};
    ScrollRun run(app, app, clock, opts);
    const auto result = run.run();
    const auto image = run.stitcher().takeImage();
    int c0, c1; comparedColumns(480, opts.stitch, c0, c1);
    const auto cmp = compare(image, run.stats().outputRows, app.renderer().expected(doc.img.h), c0, c1);
    report("from top resolves right-edge target before stitching", result == RunResult::EndOfContent && run.stats().reachedTop && run.stats().toTopMoved && run.stats().parkPosition == 1 && cmp.heightOk && cmp.mismatches == 0,
           fmt("%s, rows=%d, exact=%d", toString(result), run.stats().outputRows, cmp.heightOk && cmp.mismatches == 0));
  }
  struct Source : RunFrames, RunInjector {
    FakeClock clock;
    Image original{480, 600, kBg}, changed{480, 600, kBg};
    bool resize = false, retry = false;
    int wheels = 0, reads = 0, fresh = 0;
    Source() {
      original = marginNumberedLines(480, 100, 18, false);
      changed = original;
      std::fill(changed.px.begin(), changed.px.end(), uint8_t(127));
    }
    bool scroll(int) override { ++wheels; reads = 0; return true; }
    bool movePointer(int, int) override { return true; }
    std::optional<RunFrame> nextFrame(int ms) override {
      clock.sleepMs(ms);
      ++reads;
      auto v = (retry && wheels == 2 ? changed : original).view();
      if (resize && wheels) v.height = 100;
      return RunFrame{v, clock.nowMs()};
    }
    std::optional<RunFrame> freshFrame(int) override {
      ++fresh;
      if (wheels >= 2) return std::nullopt;
      return RunFrame{original.view(), clock.nowMs()};
    }
  };
  {
    Source source; source.resize = true;
    ScrollRun run(source, source, source.clock);
    const auto result = run.run();
    report("resized stream fails with LayoutChanged", result == RunResult::LayoutChanged && source.reads == 1, fmt("%s, reads after resize=%d", toString(result), source.reads));
  }
  {
    Source source; source.retry = true;
    RunOptions opts; opts.pointer = false;
    opts.onStep = [&](const StepResult& r, int, int) {
      if (r.status == StepStatus::NoMatch || r.status == StepStatus::Ambiguous) source.retry = false;
    };
    ScrollRun run(source, source, source.clock, opts);
    const auto result = run.run();
    report("retry stillness needs fresh proof", result != RunResult::EndOfContent && source.fresh >= 2, toString(result));
  }
}

// ------------------------------------------------------------------ pointer rules, scroll to top

// The loop's own rules about the pointer (the issue's "Control loop" section),
// which the prototype did not model.
void testPointerRules() {
  std::printf("\n[session] pointer rules and scroll to top\n");
  const int W = 480, V = 600;
  const RunPoint away{-12, V / 3};
  auto base = [&](const char* name) {
    SessCfg c;
    c.name = name;
    c.V = V;
    c.W = W;
    c.px = 53;
    c.so.pointer = true;
    c.so.away = away;
    return c;
  };
  {
    // Rules 1, 2 and 4: the first frame is taken with the pointer outside, the
    // pointer is put back before every injection, and the hover band under it
    // never reaches the image (the comparison has no mask for it).
    SessCfg c = base("pointer parked, hover under it kept out");
    c.app.pointerHover = true;
    c.extraName = "awayFirst,parkBeforeEveryWheel";
    c.extra = [&](const RunStats&, const FakeApp& a) {
      const RunPoint park = ScrollRun::parkPoint(0, W, V);
      bool ok = !a.events.empty() && a.events[0].kind == 'm' && a.events[0].a == away.x && a.events[0].b == away.y;
      int wheels = 0;
      for (size_t i = 0; i < a.events.size(); ++i) {
        if (a.events[i].kind != 's') continue;
        ++wheels;
        ok = ok && i > 0 && a.events[i - 1].kind == 'm' && a.events[i - 1].a == park.x && a.events[i - 1].b == park.y;
      }
      return ok && wheels > 3;
    };
    runSession(c);
  }
  {
    // Rule 7: the centre does not scroll, the right edge does.
    SessCfg c = base("first notch moves nothing at the centre, right edge scrolls");
    c.app.wheelArea = {W - W / 4, 0, W / 4, V};
    c.extraName = "parkPosition==1";
    c.extra = [](const RunStats& s, const FakeApp&) { return s.parkPosition == 1; };
    runSession(c);
  }
  {
    SessCfg c = base("only the lower half scrolls");
    c.app.wheelArea = {0, V / 2, W - W / 4, V / 2};
    c.extraName = "parkPosition==2";
    c.extra = [](const RunStats& s, const FakeApp&) { return s.parkPosition == 2; };
    runSession(c);
  }
  {
    // Nothing scrolls anywhere: three positions, two single notches at each
    // (one wheel step can be lost on a real desktop), then the error.
    SessCfg c = base("nothing scrolls at any park position");
    c.app.wheelArea = {W, V, 1, 1};
    c.expect = RunResult::NoMovement;
    c.expectFull = false;
    c.extraName = "sixSingleNotches";
    c.extra = [](const RunStats& s, const FakeApp& a) {
      int wheels = 0;
      bool single = true;
      for (const auto& e : a.events)
        if (e.kind == 's') { ++wheels; single = single && e.a == 1; }
      return wheels == 6 && single && s.parkPosition == 2;
    };
    runSession(c);
  }
  {
    // Step 0: the page is open halfway down; the image still starts at row 0.
    SessCfg c = base("scroll to top first, page opened halfway down");
    c.so.fromTop = true;
    c.app.startPos = 2600;
    c.extraName = "reachedTop,wheelUpBy10";
    c.extra = [](const RunStats& s, const FakeApp& a) {
      bool ups = false, order = true, down = false;
      for (const auto& e : a.events) {
        if (e.kind != 's') continue;
        if (e.a < 0 && !down) ups = ups || e.a == -10;
        if (e.a > 0) down = true;
        if (e.a == -10 && down) order = false;  // no scroll-to-top step once capturing began
      }
      return s.reachedTop && s.toTopMoved && s.toTopSteps >= 2 && ups && order;
    };
    runSession(c);
  }
  {
    // Step 0 on a page already at the top: nothing moves, twice, and the capture goes on.
    SessCfg c = base("scroll to top first, already at the top");
    c.so.fromTop = true;
    c.extraName = "twoStepsUp";
    c.extra = [](const RunStats& s, const FakeApp&) { return s.reachedTop && !s.toTopMoved && s.toTopSteps == 2; };
    runSession(c);
  }
  {
    // The first wheel step of the capture goes nowhere, as on a real desktop
    // right after the pointer arrived: neither "nothing scrolls" nor "already
    // at the top" is concluded from one still step.
    SessCfg c = base("first wheel step lost, plain");
    c.app.dropInjection = 0;
    c.extraName = "parkPosition==0";
    c.extra = [](const RunStats& s, const FakeApp&) { return s.parkPosition == 0; };
    runSession(c);
    SessCfg t = base("first wheel step lost, scroll to top first");
    t.so.fromTop = true;
    t.app.startPos = 2600;
    t.app.dropInjection = 0;
    t.extraName = "reachedTop";
    t.extra = [](const RunStats& s, const FakeApp&) { return s.reachedTop && s.toTopMoved; };
    runSession(t);
  }
  {
    // From-top off: the image starts at the row that was on screen.
    const int start = 1500;
    DocSpec ds;
    ds.viewport = V;
    ds.width = W;
    Document doc = makeDocument(ds);
    ViewportSpec vs;
    vs.width = W;
    vs.height = V;
    FakeAppSpec as;
    as.pxPerNotch = 53;
    as.startPos = start;
    FakeClock clock;
    FakeApp app(doc.img, vs, as, clock);
    RunOptions so;
    so.pointer = true;
    so.away = away;
    ScrollRun run(app, app, clock, so);
    const RunResult res = run.run();
    const std::vector<uint8_t> img = run.stitcher().takeImage();
    int c0, c1;
    comparedColumns(W, so.stitch, c0, c1);
    Image tail(W, doc.img.h - start);
    std::memcpy(tail.px.data(), doc.img.row(start), size_t(W) * tail.h * 4);
    const Cmp c = compare(img, run.stats().outputRows, tail, c0, c1);
    report("session from-top off, starts at the row on screen", res == RunResult::EndOfContent && c.mismatches == 0 && c.heightOk,
           fmt("%s, rows %d of %d, first bad %d", toString(res), run.stats().outputRows, tail.h, c.firstBad));
  }
  {
    // A stream that goes quiet while the application scrolls on (KWin's region
    // stream did, on a real desktop) looks like content that stopped moving.
    // The loop asks for a fresh frame before it makes anything of stillness:
    // the capture goes on and is exact, from the first step or from a later one.
    for (int from : {0, 5}) {
      SessCfg c = base(from ? "stream goes quiet at the sixth step" : "stream goes quiet at the first step");
      c.app.quietFromInjection = from;
      c.extraName = "quietStreamSeen";
      c.extra = [](const RunStats& s, const FakeApp& a) { return s.quietStream >= 1 && a.freshFrames >= 1; };
      runSession(c);
    }
    // The rule the real-desktop round asked for: a stream that goes quiet is
    // never proof that the content ended. With no fresh frame to be had, the
    // capture ends as "lost track" with the exact rows it has, whenever the
    // stream stops: mid-page, or from the very first step.
    for (int from : {5, 1, 0}) {
      SessCfg c = base(from == 5 ? "quiet stream, no fresh frame: never end of content" : from ? "quiet stream from the second step, no fresh frame" : "quiet stream from the first step, no fresh frame");
      c.app.quietFromInjection = from;
      c.app.quietHasFresh = false;
      c.expectFull = false;
      // Nothing was ever seen to scroll: that is "no movement", an error with no image.
      c.expect = from ? RunResult::LostTrack : RunResult::NoMovement;
      c.extraName = "notEndOfContent,unproven";
      c.extra = [](const RunStats& s, const FakeApp& a) { return a.freshFrames == 0 && s.unprovenStill >= 1 && s.outputRows < 3485; };
      runSession(c);
    }
    // The same without the pointer rules (the prototype's loop).
    SessCfg plain;
    plain.name = "quiet stream, no fresh frame, no pointer rules";
    plain.app.quietFromInjection = 4;
    plain.app.quietHasFresh = false;
    plain.expect = RunResult::LostTrack;
    plain.expectFull = false;
    runSession(plain);
  }
  {
    // An application that takes its time. The old loop called a step still
    // 120 ms after the wheel event; a page that repaints later than that (a
    // slow painter, a busy desktop, KWin's stream delivering a repaint late)
    // then looked like the end. The page is given time to begin to move, and
    // the image is whole and exact.
    for (int delay : {150, 300, 900}) {
      SessCfg c = base(delay == 150 ? "application answers after 150 ms" : delay == 300 ? "application answers after 300 ms" : "application answers after 900 ms");
      c.app.reactionDelayMs = delay;
      runSession(c);
      SessCfg t = base(delay == 150 ? "answers after 150 ms, scroll to top first" : delay == 300 ? "answers after 300 ms, scroll to top first" : "answers after 900 ms, scroll to top first");
      t.app.reactionDelayMs = delay;
      t.so.fromTop = true;
      t.app.startPos = 1700;
      runSession(t);
    }
  }
  {
    // A view that swallows the first steps down after wheel-up steps against
    // its top (GTK 4, overscrolled by up to 100 px; 37.5 px a step). After a
    // way up that moved the page, two still steps were reported as the end of
    // a 1,500-line file on a real desktop. From the top, and from halfway down.
    for (int start : {0, 2600}) {
      SessCfg c = base(start ? "overscrolled view, page opened halfway down" : "overscrolled view, page at its top");
      c.px = 37.5;
      c.app.pxPerNotch = 37.5;
      c.app.overscrollPx = 100;
      c.app.startPos = start;
      c.so.fromTop = true;
      c.extraName = "parkPosition==0";
      c.extra = [](const RunStats& s, const FakeApp&) { return s.parkPosition == 0; };
      runSession(c);
    }
    // A page that scrolled up and then never down: not the end, and not an error either.
    SessCfg c = base("page scrolls up and then not down");
    c.app.overscrollPx = 100000;
    c.app.startPos = 2600;
    c.so.fromTop = true;
    c.expect = RunResult::LostTrack;
    c.expectFull = false;
    c.extraName = "oneViewport";
    c.extra = [&](const RunStats& s, const FakeApp&) { return s.outputRows == V; };
    runSession(c);
  }
  {
    // Stop while the page is still on its way to the top: the image is the
    // viewport as it came to rest, and nothing is injected after the flag.
    static std::atomic<bool> stop{false};
    DocSpec ds;
    ds.viewport = V;
    ds.width = W;
    Document doc = makeDocument(ds);
    ViewportSpec vs;
    vs.width = W;
    vs.height = V;
    FakeAppSpec as;
    as.pxPerNotch = 53;
    as.startPos = 2600;
    as.cancelAfterInjections = 1;  // the second wheel-up step sets the flag
    as.cancelFlag = &stop;
    FakeClock clock;
    FakeApp app(doc.img, vs, as, clock);
    RunOptions so;
    so.pointer = true;
    so.away = away;
    so.fromTop = true;
    ScrollRun run(app, app, clock, so);
    const RunResult res = run.run(&stop);
    const std::vector<uint8_t> img = run.stitcher().takeImage();
    const int at = app.positionNow();
    int wheels = 0;
    for (const auto& e : app.events) wheels += e.kind == 's';
    int c0, c1;
    comparedColumns(W, so.stitch, c0, c1);
    Image view(W, V);
    if (at >= 0 && at + V <= doc.img.h) std::memcpy(view.px.data(), doc.img.row(at), size_t(W) * V * 4);
    const Cmp c = compare(img, run.stats().outputRows, view, c0, c1);
    report("session stop during scroll to top keeps the viewport",
           res == RunResult::Stopped && run.stats().stoppedAtTop && run.stats().outputRows == V && c.mismatches == 0 && c.heightOk && wheels == 2 && app.events.back().kind == 's' && at > 0 && at < 2600,
           fmt("%s, rows %d, page at %d, wheel events %d, first bad %d", toString(res), run.stats().outputRows, at, wheels, c.firstBad));
  }
  {
    // A desktop that paints the pointer into the frames (Hyprland composites a
    // software cursor; lab guest of 2026-11-11: Stop on the way to the top
    // returned the right rows with the arrow in them). Here whatever is under
    // the pointer is painted over, a band across the page. With the rule the
    // pointer is outside whenever no wheel step is being sent, and the kept
    // viewport is the page alone; the wheel steps still arrive, and the last
    // thing sent is the pointer's way out, with no wheel step behind it.
    for (int rule = 1; rule >= 0; --rule) {
      static std::atomic<bool> stop;
      stop.store(false);
      DocSpec ds;
      ds.viewport = V;
      ds.width = W;
      Document doc = makeDocument(ds);
      ViewportSpec vs;
      vs.width = W;
      vs.height = V;
      // A start from which the page comes to rest with paper under the pointer, so the band shows.
      const RunPoint park = ScrollRun::parkPoint(0, W, V);
      int start = 2600;
      const auto paperUnderPointer = [&](int at) {
        for (int y = at + park.y - 9; y < at + park.y + 9; ++y)
          for (int x = 0; x < W - 12; ++x)
            if (doc.img.at(x, y) == kBg) return true;
        return false;
      };
      while (!paperUnderPointer(start - 2 * 10 * 53)) start += 53;
      FakeAppSpec as;
      as.pxPerNotch = 53;
      as.startPos = start;
      as.pointerHover = true;
      as.wheelArea = Rect{0, 0, W, V};  // the wheel reaches the page only with the pointer on it
      as.cancelAfterInjections = 1;
      as.cancelFlag = &stop;
      FakeClock clock;
      FakeApp app(doc.img, vs, as, clock);
      RunOptions so;
      so.pointer = true;
      so.away = away;
      so.fromTop = true;
      so.restAwayToTop = rule;
      ScrollRun run(app, app, clock, so);
      const RunResult res = run.run(&stop);
      const std::vector<uint8_t> img = run.stitcher().takeImage();
      const int at = app.positionNow();
      int wheels = 0, lastWheel = -1;
      for (size_t i = 0; i < app.events.size(); ++i)
        if (app.events[i].kind == 's') { ++wheels; lastWheel = int(i); }
      // Behind the last wheel step: nothing, or with the rule the one motion out.
      const bool tail = rule ? lastWheel + 2 == int(app.events.size()) && app.events.back().kind == 'm' && app.events.back().a == away.x : lastWheel + 1 == int(app.events.size());
      Image view(W, V);
      if (at >= 0 && at + V <= doc.img.h) std::memcpy(view.px.data(), doc.img.row(at), size_t(W) * V * 4);
      const Cmp c = compare(img, run.stats().outputRows, view, 0, W);
      const bool common = res == RunResult::Stopped && run.stats().stoppedAtTop && run.stats().outputRows == V && c.heightOk && wheels == 2 && tail && at == start - 2 * 10 * 53;
      if (rule)
        report("session stop during scroll to top, painted pointer: the viewport without it", common && c.mismatches == 0,
               fmt("%s, rows %d, page at %d, wheel events %d, events %zu, first bad %d", toString(res), run.stats().outputRows, at, wheels, app.events.size(), c.firstBad));
      else
        // Without the rule the band under the parked pointer is in the image: the test sees what it is for.
        report("session stop during scroll to top, painted pointer, rule off: the pointer's band is in the image", common && c.mismatches > 0,
               fmt("%s, rows %d, mismatching pixels %lld, first bad %d", toString(res), run.stats().outputRows, c.mismatches, c.firstBad));
    }
  }
  {
    // Stop between the pointer's entry and its wheel step: no wheel step is
    // sent, the pointer leaves, and the viewport is again the page alone.
    static std::atomic<bool> stop;
    stop.store(false);
    struct StopClock final : FakeClock {
      int settles = 0;
      void sleepMs(int ms) override {
        if (ms == RunOptions().pointerSettleMs && ++settles == 2) stop.store(true);
        FakeClock::sleepMs(ms);
      }
    } clock;
    DocSpec ds;
    ds.viewport = V;
    ds.width = W;
    Document doc = makeDocument(ds);
    ViewportSpec vs;
    vs.width = W;
    vs.height = V;
    FakeAppSpec as;
    as.pxPerNotch = 53;
    as.startPos = 2600;
    as.pointerHover = true;
    as.wheelArea = Rect{0, 0, W, V};
    FakeApp app(doc.img, vs, as, clock);
    RunOptions so;
    so.pointer = true;
    so.away = away;
    so.fromTop = true;
    so.restAwayToTop = true;
    ScrollRun run(app, app, clock, so);
    const RunResult res = run.run(&stop);
    const std::vector<uint8_t> img = run.stitcher().takeImage();
    const int at = app.positionNow();
    int wheels = 0;
    for (const auto& e : app.events) wheels += e.kind == 's';
    Image view(W, V);
    if (at >= 0 && at + V <= doc.img.h) std::memcpy(view.px.data(), doc.img.row(at), size_t(W) * V * 4);
    const Cmp c = compare(img, run.stats().outputRows, view, 0, W);
    report("session stop while the pointer enters, painted pointer: no wheel step, the pointer leaves",
           res == RunResult::Stopped && run.stats().stoppedAtTop && run.stats().outputRows == V && c.mismatches == 0 && c.heightOk && wheels == 1 && at == 2600 - 10 * 53 &&
               app.events.back().kind == 'm' && app.events.back().a == away.x,
           fmt("%s, rows %d, page at %d, wheel events %d, last event %c %d, first bad %d", toString(res), run.stats().outputRows, at, wheels, app.events.back().kind, app.events.back().a, c.firstBad));
  }
  {
    // The same on a stream that has gone quiet (KWin's region stream sent
    // nothing for two batches of wheel steps and Stop returned the viewport
    // from before them; real desktop run of 2026-11-10): the image is a frame
    // grabbed from the screen after the last step, where the page then is.
    for (int fresh = 1; fresh >= 0; --fresh) {
      static std::atomic<bool> stop;
      stop.store(false);
      DocSpec ds;
      ds.viewport = V;
      ds.width = W;
      Document doc = makeDocument(ds);
      ViewportSpec vs;
      vs.width = W;
      vs.height = V;
      FakeAppSpec as;
      as.pxPerNotch = 53;
      as.startPos = 2600;
      as.reactionDelayMs = 200;        // the page answers after Stop has arrived
      as.quietFromInjection = 0;       // and the stream sends nothing from the first step on
      as.quietHasFresh = fresh;
      as.cancelAfterInjections = 1;
      as.cancelFlag = &stop;
      FakeClock clock;
      FakeApp app(doc.img, vs, as, clock);
      RunOptions so;
      so.pointer = true;
      so.away = away;
      so.fromTop = true;
      ScrollRun run(app, app, clock, so);
      const RunResult res = run.run(&stop);
      const std::vector<uint8_t> img = run.stitcher().takeImage();
      const int at = app.positionNow();
      int wheels = 0;
      for (const auto& e : app.events) wheels += e.kind == 's';
      Image view(W, V);
      if (at >= 0 && at + V <= doc.img.h) std::memcpy(view.px.data(), doc.img.row(at), size_t(W) * V * 4);
      const Cmp c = compare(img, run.stats().outputRows, view, 0, W);
      if (fresh)
        report("session stop during scroll to top, quiet stream: the viewport on the screen",
               res == RunResult::Stopped && run.stats().stoppedAtTop && run.stats().stopViewportProven && run.stats().outputRows == V && c.mismatches == 0 && c.heightOk && wheels == 2 &&
                   app.events.back().kind == 's' && at == 2600 - 2 * 10 * 53 && app.freshFrames >= 2,
               fmt("%s, rows %d, page at %d, wheel events %d, fresh frames %d, first bad %d", toString(res), run.stats().outputRows, at, wheels, app.freshFrames, c.firstBad));
      else
        // No fresh frame to be had: the last picture seen is kept and not called proven.
        report("session stop during scroll to top, quiet stream without a fresh frame: unproven",
               res == RunResult::Stopped && run.stats().outputRows == V && !run.stats().stopViewportProven && wheels == 2, fmt("%s, rows %d, proven %d", toString(res), run.stats().outputRows, run.stats().stopViewportProven));
    }
  }
  {
    // Where the desktop can be told that the wheel has stopped, the view
    // springs back before the first frame and no step down is spent on the
    // overscroll: one rest, after the last step up and before the first down.
    SessCfg c = base("overscrolled view, wheel rest after the way up");
    c.px = 37.5;
    c.app.pxPerNotch = 37.5;
    c.app.overscrollPx = 100;
    c.app.restEndsOverscroll = true;
    c.app.startPos = 2600;
    c.so.fromTop = true;
    c.extraName = "oneRestBetweenUpAndDown,noStillSteps";
    c.extra = [](const RunStats& s, const FakeApp& a) {
      int rests = 0, restAt = -1, lastUp = -1, firstDown = -1;
      for (size_t i = 0; i < a.events.size(); ++i) {
        const auto& e = a.events[i];
        if (e.kind == 'r') { ++rests; restAt = int(i); }
        if (e.kind == 's' && e.a < 0 && firstDown < 0) lastUp = int(i);
        if (e.kind == 's' && e.a > 0 && firstDown < 0) firstDown = int(i);
      }
      return rests == 1 && lastUp < restAt && restAt < firstDown && s.steps <= 16;
    };
    runSession(c);
  }
  {
    // A slow guest: a frame every 170 ms, the page answers after 350 ms, a
    // scroll animation of 300 to 500 ms, and more distance per notch in a
    // burst. The picture between two frames of one animation is not settled.
    for (double accel : {1.0, 2.2}) {
      SessCfg c = base(accel > 1 ? "slow stream, 170 ms a frame, bursts scroll further" : "slow stream, 170 ms a frame");
      c.V = 800;
      c.app.frameIntervalMs = 170;
      c.app.reactionDelayMs = 350;
      c.app.animMinMs = 300;
      c.app.animMaxMs = 500;
      c.app.accelFactor = accel;
      c.so.fromTop = true;
      c.app.startPos = 900;
      c.extraName = "pxPerNotchSteady";
      c.extra = [accel](const RunStats& s, const FakeApp&) { return accel > 1 || (s.minPxPerNotch > 50 && s.maxPxPerNotch < 56); };
      runSession(c);
    }
  }
  {
    // The way to the top takes what the page needs. It had 15 s whatever the
    // page; a 27,000 px text view on a Hyprland guest needed 17 s, and the
    // capture began at line 78 and was reported as the end of the content
    // (final pass of 2026-11-11). A page at 4 px a notch, opened at its end:
    // at the old pace (every step checked, ten notches a step) the way up is
    // longer than 15 s and now reaches the top; the image is whole from row 0.
    int64_t checkedMs = 0;
    DocSpec page;
    page.viewport = V;
    page.width = W;
    const int bottom = makeDocument(page).img.h - V;
    SessCfg slow = base("way to the top longer than the old 15 s, every step checked");
    slow.px = 4;
    slow.so.fromTop = true;
    slow.so.toTopCheckEvery = 1;
    slow.so.toTopMaxNotches = slow.so.toTopNotches;
    slow.app.startPos = bottom;
    slow.extraName = "reachedTop,longerThan15s";
    slow.extra = [&](const RunStats& s, const FakeApp& a) {
      checkedMs = s.toTopMs;
      bool tens = true;
      for (const auto& e : a.events)
        if (e.kind == 's' && e.a < 0) tens = tens && e.a == -10;
      return s.reachedTop && s.toTopGaveUp.empty() && s.toTopMs > 15000 && tens;
    };
    runSession(slow);
    // The same page with the loop's own pace: the steps grow while the page
    // moves and only every fourth waits for the picture to settle. Much
    // sooner at the top, which is still proved with plain steps of ten.
    SessCfg quick = base("way to the top, steps grow while the page moves");
    quick.px = 4;
    quick.so.fromTop = true;
    quick.app.startPos = bottom;
    quick.extraName = "reachedTop,sooner,provedWithPlainSteps";
    quick.extra = [&](const RunStats& s, const FakeApp& a) {
      int largest = 0, lastUp = 0, beforeLast = 0;
      for (const auto& e : a.events) {
        if (e.kind != 's' || e.a >= 0) continue;
        largest = std::max(largest, -e.a);
        beforeLast = lastUp;
        lastUp = -e.a;
      }
      return s.reachedTop && s.toTopGaveUp.empty() && s.toTopMs * 3 < checkedMs && largest == RunOptions().toTopMaxNotches && lastUp == 10 && beforeLast == 10;
    };
    runSession(quick);
  }
  {
    // The top is never reached: the time for the way up ends first (a page
    // that keeps growing upward does the same). The capture goes on from
    // where the page is and is exact from there, and its end is "top not
    // reached", never "end of content"; with a height cap or Stop instead of
    // the end the result is theirs, and the stats still say that the top
    // was not proved.
    for (int cap : {0, 1500}) {
      DocSpec ds;
      ds.viewport = V;
      ds.width = W;
      Document doc = makeDocument(ds);
      ViewportSpec vs;
      vs.width = W;
      vs.height = V;
      FakeAppSpec as;
      as.pxPerNotch = 2;
      as.startPos = doc.img.h - V;
      FakeClock clock;
      FakeApp app(doc.img, vs, as, clock);
      RunOptions so;
      so.pointer = true;
      so.away = away;
      so.fromTop = true;
      so.toTopLimitMs = 3000;
      if (cap) so.stitch.maxOutputRows = cap;
      int first = -1;
      so.onProgress = [&](const RunProgress& p) {
        if (!p.scrollingToTop && first < 0) first = app.framePositions.back();
      };
      ScrollRun run(app, app, clock, so);
      const RunResult res = run.run();
      const RunStats& st = run.stats();
      const std::vector<uint8_t> img = run.stitcher().takeImage();
      int c0, c1;
      comparedColumns(W, so.stitch, c0, c1);
      const int rows = std::max(0, std::min(st.outputRows, doc.img.h - std::max(0, first)));
      Image tail(W, std::max(1, rows));
      if (first >= 0 && rows > 0) std::memcpy(tail.px.data(), doc.img.row(first), size_t(W) * rows * 4);
      const Cmp c = compare(img, st.outputRows, tail, c0, c1);
      const bool exact = first > 0 && c.mismatches == 0 && c.heightOk;
      const bool said = !st.reachedTop && !st.toTopGaveUp.empty() && st.toTopMs >= so.toTopLimitMs && st.toTopMs < so.toTopLimitMs + 3000;
      if (cap)
        report("session top not reached, then the height cap: the cap's result, the top still not proved", res == RunResult::LimitReached && exact && said && st.outputRows == cap,
               fmt("%s, rows %d from row %d, first bad %d, way up %lld ms [%s]", toString(res), st.outputRows, first, c.firstBad, (long long)st.toTopMs, st.toTopGaveUp.c_str()));
      else
        report("session top not reached in its time: never end of content", res == RunResult::TopNotReached && exact && said && first + st.outputRows == doc.img.h,
               fmt("%s, rows %d from row %d of %d, first bad %d, way up %lld ms [%s]", toString(res), st.outputRows, first, doc.img.h, c.firstBad, (long long)st.toTopMs, st.toTopGaveUp.c_str()));
    }
  }
  {
    // A page that never comes to rest (a video in a fixed player) cannot be
    // told from one that still scrolls. The way up ends after the time such a
    // page gets, says so, and whatever the capture then makes of the page is
    // not "end of content".
    DocSpec ds;
    ds.viewport = V;
    ds.width = W;
    Document doc = makeDocument(ds);
    ViewportSpec vs;
    vs.width = W;
    vs.height = V;
    FakeAppSpec as;
    as.pxPerNotch = 53;
    as.startPos = 1500;
    as.restlessRows = 200;
    FakeClock clock;
    FakeApp app(doc.img, vs, as, clock);
    RunOptions so;
    so.pointer = true;
    so.away = away;
    so.fromTop = true;
    ScrollRun run(app, app, clock, so);
    const RunResult res = run.run();
    const RunStats& st = run.stats();
    report("session page never at rest: the way to the top gives up and says so",
           res != RunResult::EndOfContent && !st.reachedTop && st.toTopGaveUp.find("rest") != std::string::npos && st.toTopMs >= so.toTopRestlessMs && st.toTopMs < so.toTopRestlessMs + 4000,
           fmt("%s, rows %d, way up %lld ms in %d steps [%s]", toString(res), st.outputRows, (long long)st.toTopMs, st.toTopSteps, st.toTopGaveUp.c_str()));
  }
  {
    // Stop is honoured before the next injection: nothing is sent after it.
    static std::atomic<bool> stop{false};
    SessCfg c = base("stop: no injection after the flag");
    c.app.cancelAfterInjections = 4;
    c.app.cancelFlag = &stop;
    c.expect = RunResult::Stopped;
    c.expectFull = false;
    c.extraName = "fiveWheelEvents";
    c.extra = [](const RunStats&, const FakeApp& a) {
      int wheels = 0;
      for (const auto& e : a.events) wheels += e.kind == 's';
      return wheels == 5 && a.events.back().kind == 's';  // the fifth set the flag; nothing follows it
    };
    runSession(c);
  }
}

}  // namespace

int main(int argc, char** argv) {
  g_full = (argc > 1 && std::strcmp(argv[1], "--full") == 0) || std::getenv("PUTGD_SCROLL_STITCHER_FULL");
  testReconstruction();
  testSticky();
  testNoise();
  testAmbiguity();
  testNumberedLines();
  testMarginRule();
  testProof();
  testEndsAndBounce();
  testMemory();
  testSessions();
  testPointerRules();
  testReviewFrameProofs();
  testLayoutChange();
  testLivePages();

  int pass = 0, fail = 0, known = 0;
  std::printf("\n==================== summary ====================\n");
  for (const Row& r : g_rows) {
    if (r.knownLimit) ++known;
    else if (r.pass) ++pass;
    else ++fail;
    if (!r.pass && !r.knownLimit) std::printf("FAIL  %s  %s\n", r.name.c_str(), r.detail.c_str());
  }
  std::printf("%d passed, %d failed, %d known limitations (reported, not counted)\n", pass, fail, known);
  return fail == 0 ? 0 : 1;
}
