// The scroll stitcher and the control loop against a synthetic application
// (#1376): the research prototype's suite, trimmed to a few seconds, plus the
// loop's pointer rules and its scroll-to-top step. `--full` (or
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
    report(fmt("signature bytes %dx%d", W, H), s.bytes() == size_t(H) * 157,
           fmt("%zu bytes = %.0f B/row (frame itself %zu bytes)", s.bytes(), double(s.bytes()) / H, size_t(W) * H * 4));
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
  cfg.so.onStep = [&](const StepResult& r, int, int) {
    const int pos = app.framePositions.empty() ? 0 : app.framePositions.back();
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
            else if (g_lastSafeStop) ++safeStops;
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
    // Nothing scrolls anywhere: three positions, three single notches, then the error.
    SessCfg c = base("nothing scrolls at any park position");
    c.app.wheelArea = {W, V, 1, 1};
    c.expect = RunResult::NoMovement;
    c.expectFull = false;
    c.extraName = "threeSingleNotches";
    c.extra = [](const RunStats& s, const FakeApp& a) {
      int wheels = 0;
      bool single = true;
      for (const auto& e : a.events)
        if (e.kind == 's') { ++wheels; single = single && e.a == 1; }
      return wheels == 3 && single && s.parkPosition == 2;
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
    // Step 0 on a page already at the top: nothing moves, and the capture goes on.
    SessCfg c = base("scroll to top first, already at the top");
    c.so.fromTop = true;
    c.extraName = "oneStepUp";
    c.extra = [](const RunStats& s, const FakeApp&) { return s.reachedTop && !s.toTopMoved && s.toTopSteps == 1; };
    runSession(c);
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
  testEndsAndBounce();
  testMemory();
  testSessions();
  testPointerRules();

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
