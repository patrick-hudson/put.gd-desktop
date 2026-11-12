// The scroll stitcher and the control loop against a synthetic application
// (#1376). Only what a wrong result would hide: an image that is reported as
// the whole page while rows are missing (no overlap matched on a sliver, a
// guess among equal offsets, a top that was never proved, an end that was
// never proved), the output bound, a stream whose frames change size, and no
// wheel step after Stop. Whether stitches come out right on real pages is the
// acceptance drivers' job on every desktop.
// Plain checks with a summary table; exit code 1 on any failure.
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
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
  std::string detail;
};
std::vector<Row> g_rows;

void report(const std::string& name, bool pass, const std::string& detail) {
  g_rows.push_back({name, pass, detail});
  std::printf("  %-44s %s  %s\n", name.c_str(), pass ? "PASS" : "FAIL", detail.c_str());
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
  int firstBad = -1;
  bool heightOk = false;
};

// Compares an output image with the expected one over the compared columns.
Cmp compare(const std::vector<uint8_t>& out, int outH, const Image& exp, int c0, int c1) {
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
        ++c.mismatches;
        if (c.firstBad < 0) c.firstBad = y;
      }
    }
  }
  if (!c.heightOk && c.firstBad < 0) c.firstBad = H;
  return c;
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
};

// Feeds frames at the given scroll positions; hint = true offset to the reference.
Drive drive(const Renderer& rend, const std::vector<int>& pos, StitchOptions o = {}) {
  Drive d;
  ScrollStitcher st(o);
  Image frame;
  int refPos = 0;
  for (size_t i = 0; i < pos.size(); ++i) {
    rend.render(pos[i], frame);
    const int hint = i > 0 ? pos[i] - refPos : kNoHint;
    StepResult r = st.addFrame(frame.view(), hint);
    d.steps.push_back(r);
    if (r.status == StepStatus::First || r.status == StepStatus::Appended ||
        r.status == StepStatus::NoMovement || r.status == StepStatus::Reversed)
      refPos = pos[i];
  }
  st.finish();
  d.outH = st.outputHeight();
  d.img = st.takeImage();
  return d;
}

// Two frames: the second step and whether the output was untouched.
struct Pair {
  StepResult r;
  bool untouched;
};
Pair pairFrames(const Image& a, const Image& b, int hint) {
  ScrollStitcher st;
  FrameSignatures sa, sb;
  sa.compute(a.view(), st.options());
  sb.compute(b.view(), st.options());
  st.addFrame(a.view(), sa, kNoHint);
  const int before = st.outputHeight();
  Pair pr;
  pr.r = st.addFrame(b.view(), sb, hint, 0);
  pr.untouched = before == st.outputHeight();
  return pr;
}

// Lines that are all the same: the words every line shares and nothing else.
Image identicalLines(int width, int lines, int pitch) {
  Image img(width, lines * pitch + 8, kBg);
  for (int i = 0; i < lines; ++i) {
    Rng same(77);
    drawTextLine(img, 4 + i * pitch, 130, std::min(width - 8, 310), same);
  }
  return img;
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

// ------------------------------------------------------------------ stitcher

void testNeverGuessed() {
  std::printf("\n[stitcher] an offset is appended only when the pixels prove it\n");
  {
    // The frames of the neon guest's third browser run (KWin 6.8.1): a pair
    // with no overlap (975 px in an 800 px viewport) was matched on 25 rows of
    // ruler stripes and 200 px went missing from an image reported as
    // complete. No pair further apart than the viewport is ever matched,
    // whatever the loop expected.
    const int W = 1280, V = 800;
    const Image doc = vmRuler(W, 60);
    auto frame = [&](int pos) {
      Image f(W, V);
      std::memcpy(f.px.data(), doc.row(pos), size_t(W) * V * 4);
      return f;
    };
    int accepted = 0, pairs = 0;
    std::string det;
    for (int p = 0; p + V <= doc.h; p += 240)
      for (int phase : {0, 25})
        for (int q = p + phase + 780; q + V <= doc.h; q += 40)
          for (int hint : {kNoHint, 446}) {
            const Pair n = pairFrames(frame(p + phase), frame(q), hint);
            ++pairs;
            if (n.r.status == StepStatus::Appended || n.r.status == StepStatus::Reversed || !n.untouched) {
              ++accepted;
              if (det.size() < 200) det += fmt(" %d->%d hint %d: %s/%d", p + phase, q, hint, toString(n.r.status), n.r.offset);
            }
          }
    report("ruler: no pair further apart than the viewport is ever matched", accepted == 0 && pairs > 100, fmt("%d of %d pairs accepted%s", accepted, pairs, det.c_str()));
  }
  {
    // Lines that are all the same repeat exactly: no pixel tells the offsets
    // apart, and the step stays ambiguous (never a guess).
    ViewportSpec vs;
    vs.width = 1200;
    vs.height = 700;
    const Image doc = identicalLines(vs.width, 400, 18);
    Renderer rend(doc, vs);
    Image a, b;
    rend.render(500, a);
    rend.render(538, b);
    const Pair n = pairFrames(a, b, kNoHint);
    report("identical lines, no hint", n.r.status == StepStatus::Ambiguous && n.untouched, fmt("%s, output untouched=%d", toString(n.r.status), n.untouched));
  }
}

void testOutputBound() {
  std::printf("\n[stitcher] the output bound\n");
  DocSpec ds;
  ds.seed = 7;
  ds.viewport = 300;
  Document doc = makeDocument(ds);
  {
    ViewportSpec vs;
    vs.height = 300;
    vs.headerH = 30;
    vs.footerH = 20;
    Renderer r2(doc.img, vs);
    StitchOptions o;
    o.maxOutputRows = 1000;
    auto p = positions(r2.maxScroll(), [&] { return int(r2.contentH() * 0.6); });
    Drive d = drive(r2, p, o);
    bool sawLimit = false;
    for (auto& s : d.steps) sawLimit |= s.status == StepStatus::LimitReached;
    int c0, c1;
    comparedColumns(vs.width, o, c0, c1);
    Cmp c = compare(d.img, d.outH, r2.expected(1000 - 50), c0, c1);
    report("maxOutputRows=1000", sawLimit && d.outH == 1000 && c.mismatches == 0 && c.heightOk,
           fmt("LimitReached=%d, height=%d, prefix+footer exact=%d", sawLimit, d.outH, c.mismatches == 0));
  }
  {
    ViewportSpec vs;
    vs.height = 300;
    vs.footerH = 80;
    Renderer r2(doc.img, vs);
    StitchOptions o;
    o.maxOutputRows = 20;
    Drive d = drive(r2, {0, 100}, o);
    Image first;
    r2.render(0, first);
    Image prefix(vs.width, 20, kBg);
    std::copy_n(first.px.begin(), prefix.px.size(), prefix.px.begin());
    const auto cmp = compare(d.img, d.outH, prefix, 0, vs.width);
    report("footer larger than output cap", cmp.heightOk && cmp.mismatches == 0, fmt("height=%d, exact prefix=%d", d.outH, cmp.heightOk && cmp.mismatches == 0));
  }
}

// ------------------------------------------------------------------ the loop

struct SessCfg {
  std::string name;
  int V = 600;
  int W = 480;
  FakeAppSpec app;
  RunOptions so;
  RunResult expect = RunResult::EndOfContent;
  std::function<bool(const RunStats&, const FakeApp&)> extra;
  std::string extraName;
};

// One capture of a synthetic page: the expected result, an exact prefix of
// the page, never an offset other than the page's own, and `extra`.
void runSession(SessCfg cfg) {
  DocSpec ds;
  ds.viewport = cfg.V;
  ds.width = cfg.W;
  Document doc = makeDocument(ds);
  ViewportSpec vs;
  vs.width = cfg.W;
  vs.height = cfg.V;
  FakeClock clock;
  FakeApp app(doc.img, vs, cfg.app, clock);
  int refPos = 0, wrong = 0;
  cfg.so.onStep = [&](const StepResult& r, int, int) {
    const int pos = app.framePositions.empty() ? 0 : app.framePositions.back();
    const bool accepted = r.status == StepStatus::Appended || r.status == StepStatus::Reversed ||
                          r.status == StepStatus::LimitReached;
    if (accepted && !r.lowConfidence && r.offset != pos - refPos) ++wrong;
    if (accepted || r.status == StepStatus::NoMovement) refPos = pos;
  };
  ScrollRun sess(app, app, clock, cfg.so);
  const RunResult res = sess.run(cfg.app.cancelFlag);
  const RunStats& st = sess.stats();
  std::vector<uint8_t> img = sess.stitcher().takeImage();
  const int outH = st.outputRows;
  int c0, c1;
  comparedColumns(vs.width, cfg.so.stitch, c0, c1);
  const Cmp c = compare(img, outH, app.renderer().expected(outH), c0, c1);
  const bool exact = c.mismatches == 0 && c.heightOk;
  bool ok = res == cfg.expect && exact && wrong == 0;
  std::string extra;
  if (cfg.extra) {
    const bool e = cfg.extra(st, app);
    ok = ok && e;
    extra = fmt(" %s=%d", cfg.extraName.c_str(), e);
  }
  report("session " + cfg.name, ok,
         fmt("%s, %s (rows %d of %d), steps %d, wrong offsets %d%s%s", toString(res), exact ? "exact" : fmt("BAD row %d", c.firstBad).c_str(), outH,
             doc.img.h, st.steps, wrong, extra.c_str(), st.lastError.empty() ? "" : (" [" + st.lastError + "]").c_str()));
}

void testLoop() {
  std::printf("\n[session] the end and the top are proved, Stop is final\n");
  const int W = 480, V = 600;
  const RunPoint away{-12, V / 3};
  auto base = [&](const char* name) {
    SessCfg c;
    c.name = name;
    c.V = V;
    c.W = W;
    c.app.pxPerNotch = 53;
    c.so.pointer = true;
    c.so.away = away;
    return c;
  };
  {
    // A stream that goes quiet (KWin's region stream did, on a real desktop)
    // is never proof that the content ended: with no fresh frame to be had,
    // the capture ends as "lost track" with the exact rows it has.
    SessCfg c = base("quiet stream, no fresh frame: never end of content");
    c.app.quietFromInjection = 5;
    c.app.quietHasFresh = false;
    c.expect = RunResult::LostTrack;
    c.extraName = "unproven";
    c.extra = [](const RunStats& s, const FakeApp& a) { return a.freshFrames == 0 && s.unprovenStill >= 1; };
    runSession(c);
  }
  {
    // A page that works through its wheel notches one by one and paints only
    // once none is waiting is slow to answer, not at its end (Cinnamon,
    // 2026-11-12: `end_of_content` with 1,020 of 24,000 rows). The image is
    // the whole page, or the end is an honest one with an exact prefix.
    int complete = 0, honest = 0, total = 0;
    std::string bad;
    for (int first = 0; first < 2; ++first) {
      DocSpec ds;
      ds.seed = 891;
      ds.viewport = V;
      const Document doc = makeDocument(ds);
      ViewportSpec vs;
      vs.height = V;
      FakeAppSpec spec;
      spec.pxPerNotch = 40;
      spec.busyMsPerNotch = 250;
      spec.busyShowsFirst = first;
      spec.frameIntervalMs = 33;
      FakeClock clock;
      FakeApp app(doc.img, vs, spec, clock);
      ScrollRun run(app, app, clock, {});
      const RunResult res = run.run();
      const int outH = run.stats().outputRows;
      const std::vector<uint8_t> img = run.stitcher().takeImage();
      int c0, c1;
      comparedColumns(vs.width, {}, c0, c1);
      const Cmp whole = compare(img, outH, app.renderer().expected(doc.img.h), c0, c1);
      const Cmp prefix = compare(img, outH, app.renderer().expected(outH), c0, c1);
      ++total;
      if (res == RunResult::EndOfContent && whole.mismatches == 0 && whole.heightOk) ++complete;
      else if (res != RunResult::EndOfContent && prefix.mismatches == 0 && prefix.heightOk) ++honest;
      else bad += fmt(" [%s: %s, %d rows of %d, first bad %d]", first ? "first shown" : "busy", toString(res), outH, doc.img.h, whole.firstBad);
    }
    report(fmt("session a page slower than the wait is not at its end (%d runs)", total), complete + honest == total,
           fmt("%d complete, %d honest ends with an exact prefix%s", complete, honest, bad.c_str()));
  }
  {
    // The top is never reached: the time for the way up ends first. The
    // capture goes on from where the page is and is exact from there, and its
    // end is "top not reached", never "end of content" (an image that began
    // at line 78 was reported as the whole page, 2026-11-11).
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
    report("session top not reached in its time: never end of content", res == RunResult::TopNotReached && exact && said && first + st.outputRows == doc.img.h,
           fmt("%s, rows %d from row %d of %d, first bad %d, way up %lld ms [%s]", toString(res), st.outputRows, first, doc.img.h, c.firstBad, (long long)st.toTopMs, st.toTopGaveUp.c_str()));
  }
  {
    // A page that never comes to rest (a video in a fixed player): the way up
    // ends after the time such a page gets, says so, and the capture is not
    // "end of content".
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
    c.extraName = "fiveWheelEvents";
    c.extra = [](const RunStats&, const FakeApp& a) {
      int wheels = 0;
      for (const auto& e : a.events) wheels += e.kind == 's';
      return wheels == 5 && a.events.back().kind == 's';  // the fifth set the flag; nothing follows it
    };
    runSession(c);
  }
  {
    // A stream whose frames change size is a change of layout, never a stitch.
    struct Source : RunFrames, RunInjector {
      FakeClock clock;
      Image original = identicalLines(480, 100, 18);
      int wheels = 0, reads = 0;
      bool scroll(int) override { ++wheels; reads = 0; return true; }
      bool movePointer(int, int) override { return true; }
      std::optional<RunFrame> nextFrame(int ms) override {
        clock.sleepMs(ms);
        ++reads;
        auto v = original.view();
        if (wheels) v.height = 100;
        return RunFrame{v, clock.nowMs()};
      }
      std::optional<RunFrame> freshFrame(int) override { return RunFrame{original.view(), clock.nowMs()}; }
    } source;
    ScrollRun run(source, source, source.clock);
    const auto result = run.run();
    report("resized stream fails with LayoutChanged", result == RunResult::LayoutChanged && source.reads == 1, fmt("%s, reads after resize=%d", toString(result), source.reads));
  }
}

}  // namespace

int main() {
  testNeverGuessed();
  testOutputBound();
  testLoop();
  int pass = 0, fail = 0;
  std::printf("\n==================== summary ====================\n");
  for (const Row& r : g_rows) {
    if (r.pass) ++pass;
    else {
      ++fail;
      std::printf("FAIL  %s  %s\n", r.name.c_str(), r.detail.c_str());
    }
  }
  std::printf("%d passed, %d failed\n", pass, fail);
  return fail == 0 ? 0 : 1;
}
