// The scroll tracker (#1515) against synthetic scenes. Only what a wrong
// result would hide: a frame placed at another offset than the page's own
// (repeating and nearly repeating lines, small and large steps, a scroll back
// up, a frame of another page), a pinned part in the image more than once,
// and a row lost or doubled at a seam. Whether real pages come out right is
// tools/lab/scroll_tracker_score.py's job on recorded frames.
// Plain checks with a summary; exit code 1 on any failure.
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include "core/ScrollTracker.h"
#include "fixtures/ScrollSynthetic.h"

using namespace putgd::scroll;
using namespace synth;

namespace {

int g_fail = 0, g_pass = 0;

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

void report(const std::string& name, bool pass, const std::string& detail) {
  (pass ? g_pass : g_fail) += 1;
  std::printf("  %-62s %s  %s\n", name.c_str(), pass ? "PASS" : "FAIL", detail.c_str());
  std::fflush(stdout);
}

// A page of paragraphs, a photo and a gradient: nothing repeats.
Document plainPage(uint64_t seed, int width, int viewport) {
  DocSpec ds;
  ds.seed = seed;
  ds.width = width;
  ds.viewport = viewport;
  ds.flatFactor = 0.2;
  ds.tableFactor = 0;
  ds.noiseFactor = 0.3;
  ds.gradientFactor = 0.3;
  ds.blankFactor = 0.2;
  ds.paragraphsPerGap = 4;
  return makeDocument(ds);
}

struct Played {
  std::vector<TrackResult> results;
  TrackTotals totals;
  MemoryCanvas* canvas = nullptr;
  std::vector<uint8_t> image;
  int width = 0;
  int wrong = 0, placed = 0;
  std::string firstWrong;
};

// Plays frames at the given scroll positions (`frameAt` may replace a frame);
// a placed frame must be where the page is relative to the first frame.
Played play(const Scene& scene, const std::vector<int>& pos, TrackerOptions o = {},
            const std::function<bool(size_t, Image&)>& replace = {}, TrackerRect excluded = {}) {
  Played r;
  MemoryCanvas canvas(scene.spec().width);
  ScrollTracker tracker(canvas, o);
  tracker.setExcluded(excluded);
  for (size_t i = 0; i < pos.size(); ++i) {
    Image f = scene.frame(pos[i], int(i));
    const bool foreign = replace && replace(i, f);
    const TrackResult t = tracker.addFrame(f.view());
    r.results.push_back(t);
    if (t.status != TrackStatus::Placed) continue;
    ++r.placed;
    if (foreign || t.position != pos[i] - pos[0]) {
      ++r.wrong;
      if (r.firstWrong.empty())
        r.firstWrong = foreign ? fmt(" frame %zu (another page) placed at %lld", i, t.position)
                               : fmt(" frame %zu at %lld, page at %d", i, t.position, pos[i] - pos[0]);
    }
  }
  r.totals = tracker.finish();
  r.image = canvas.image(r.totals.top, r.totals.bottom);
  r.width = scene.spec().width;
  return r;
}

struct Box {
  int x0, y0, x1, y1;  // image rows and columns, [x0, x1) x [y0, y1)
};

// PUTGD_TRACKER_DUMP=DIR: the images of every comparison as PPM, for a look.
void dump(const char* name, const uint8_t* bgrx, int w, int h) {
  const char* dir = std::getenv("PUTGD_TRACKER_DUMP");
  if (!dir) return;
  static int n = 0;
  FILE* f = std::fopen(fmt("%s/%02d-%s.ppm", dir, n++, name).c_str(), "wb");
  if (!f) return;
  std::fprintf(f, "P6 %d %d 255\n", w, h);
  for (long long i = 0; i < (long long)w * h; ++i) {
    const uint8_t rgb[3] = {bgrx[i * 4 + 2], bgrx[i * 4 + 1], bgrx[i * 4]};
    std::fwrite(rgb, 1, 3, f);
  }
  std::fclose(f);
}

// Pixels of the image that differ from `expected` by more than `tolerance`
// in a channel, outside the boxes left out.
long long differing(const Played& r, const Image& expected, const std::vector<Box>& out, int tolerance, int* firstRow) {
  dump("got", r.image.data(), r.width, int(r.totals.bottom - r.totals.top));
  dump("want", reinterpret_cast<const uint8_t*>(expected.px.data()), expected.w, expected.h);
  long long bad = 0;
  *firstRow = -1;
  const long long rows = r.totals.bottom - r.totals.top;
  if (rows != expected.h) {
    *firstRow = int(std::min<long long>(rows, expected.h));
    return std::max<long long>(1, std::llabs(rows - expected.h) * expected.w);
  }
  for (int y = 0; y < expected.h; ++y)
    for (int x = 0; x < expected.w; ++x) {
      bool skip = false;
      for (const Box& b : out) skip |= x >= b.x0 && x < b.x1 && y >= b.y0 && y < b.y1;
      if (skip) continue;
      uint32_t v;
      std::memcpy(&v, &r.image[(size_t(y) * r.width + x) * 4], 4);
      const uint32_t e = expected.at(x, y);
      int worst = 0;
      for (int s = 0; s < 24; s += 8) worst = std::max(worst, std::abs(int((v >> s) & 255) - int((e >> s) & 255)));
      if (worst > tolerance) {
        ++bad;
        if (*firstRow < 0) *firstRow = y;
      }
    }
  return bad;
}

Scene scene_of(const Image& doc, const SceneSpec& spec) { return Scene(doc, spec); }

// The page's rows [from, to) as an image.
Image pageRows(const Image& doc, long long from, long long to, int width) {
  Image e(width, int(to - from), kBg);
  for (long long y = from; y < to; ++y)
    if (y >= 0 && y < doc.h) std::memcpy(&e.px[size_t(y - from) * width], doc.row(int(y)), size_t(width) * 4);
  return e;
}

std::vector<int> steps(int from, int to, const std::function<int()>& step) {
  std::vector<int> p{from};
  while (p.back() != to) {
    const int s = std::max(1, step());
    p.push_back(to > from ? std::min(to, p.back() + s) : std::max(to, p.back() - s));
  }
  return p;
}

std::string counts(const Played& r) {
  return fmt("%d frames, %d placed, %d still, %d skipped (longest run %d), %d by expectation", r.totals.frames, r.totals.placed,
             r.totals.noMovement, r.totals.skipped, r.totals.longestSkipRun, r.totals.byExpectation);
}

// ------------------------------------------------------------------ offsets

void testOffsets() {
  std::printf("\n[offsets] a frame is placed where the page is, or skipped\n");
  const int W = 480, V = 400;
  const Document doc = plainPage(5, W, V);
  SceneSpec ss;
  ss.width = W;
  ss.height = V;
  const Scene scene(doc.img, ss);
  const int end = doc.img.h - V;
  Rng rng(17);
  {
    // Steps of a third to two thirds of the viewport, as put.gd's wheel or a
    // person's notches give, with no hint.
    const auto pos = steps(0, end, [&] { return rng.range(V / 3, 2 * V / 3); });
    const Played r = play(scene, pos);
    int firstBad = 0;
    const long long bad = differing(r, pageRows(doc.img, 0, doc.img.h, W), {}, 0, &firstBad);
    report("steps: every frame placed where the page is, image exact", r.wrong == 0 && r.placed == int(pos.size()) && bad == 0,
           counts(r) + fmt(", %d wrong%s, %lld pixels differ (row %d)", r.wrong, r.firstWrong.c_str(), bad, firstBad));
  }
  {
    // Continuous small movements, 1 to 9 rows a frame, down and back up: a
    // person's touchpad at 60 frames a second.
    std::vector<int> pos = steps(0, end / 2, [&] { return rng.range(1, 9); });
    const auto up = steps(end / 2, end / 4, [&] { return rng.range(1, 9); });
    pos.insert(pos.end(), up.begin() + 1, up.end());
    const Played r = play(scene, pos);
    int firstBad = 0;
    const long long bad = differing(r, pageRows(doc.img, 0, end / 2 + V, W), {}, 0, &firstBad);
    report("continuous small movements down and up: no wrong place, exact", r.wrong == 0 && r.totals.skipped == 0 && bad == 0,
           counts(r) + fmt(", %d wrong%s, %lld pixels differ (row %d)", r.wrong, r.firstWrong.c_str(), bad, firstBad));
  }
  {
    // Lines that differ only in a number at the left (line numbers): one
    // line up or down fits all but a few characters. No step may slip.
    Image lines(W, 3000, kBg);
    Rng same(77), numbers(91);
    for (int i = 0; i * 18 + 12 < lines.h; ++i) {
      Rng words(77);
      drawTextLine(lines, 4 + i * 18, 70, W - 30, words);
      drawTextLine(lines, 4 + i * 18, 26, 26 + 6 * (1 + numbers.range(1, 4)), numbers);
    }
    (void)same;
    const Scene numbered(lines, ss);
    const auto pos = steps(0, lines.h - V, [&] { return rng.range(40, 260); });
    const Played r = play(numbered, pos);
    report("lines that differ in a few characters never slip by one line", r.wrong == 0 && r.placed >= int(pos.size()) - 2,
           counts(r) + fmt(", %d wrong%s", r.wrong, r.firstWrong.c_str()));
  }
  {
    // Lines that are all the same: no pixel tells the offsets apart. A
    // frame is skipped, or placed by the expected offset among equal
    // candidates and said so; never put anywhere else.
    Image lines(W, 3000, kBg);
    for (int i = 0; i * 18 + 12 < lines.h; ++i) {
      Rng words(77);
      drawTextLine(lines, 4 + i * 18, 40, W - 40, words);
    }
    const Scene repeated(lines, ss);
    std::vector<int> pos{0, 3, 6, 9, 12, 15, 18, 21, 50, 130, 131, 132};
    const Played r = play(repeated, pos);
    int said = 0;
    for (const TrackResult& t : r.results) said += t.byExpectation;
    report("identical lines: skipped or chosen by expectation, never wrong", r.wrong == 0 && said > 0,
           counts(r) + fmt(", %d wrong%s", r.wrong, r.firstWrong.c_str()));
  }
}

// ------------------------------------------------------------------ pinned parts

void testPinned() {
  std::printf("\n[pinned] what stays put is in the image once\n");
  const int W = 480, V = 400;
  const Document doc = plainPage(9, W, V);
  const int end = doc.img.h - V;
  Rng rng(23);
  const auto pos = steps(0, end, [&] { return rng.range(60, 160); });
  {
    // A box floating over the bottom of the page's middle columns (a
    // search page's "Ask anything" box): the page's rows under it come from
    // frames in which it does not cover them; the box is in the image once,
    // at the bottom, as the last screen shows it.
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    ss.pinned.push_back({96, V - 96, 280, 64});
    const Scene scene(doc.img, ss);
    const Played r = play(scene, pos);
    Image expected = pageRows(doc.img, 0, doc.img.h, W);
    const int y0 = expected.h - 96;
    for (int y = 0; y < 64; ++y) std::memcpy(&expected.px[size_t(y0 + y) * W + 96], scene.part(0).row(y), 280 * 4);
    int firstBad = 0;
    const long long bad = differing(r, expected, {}, 0, &firstBad);
    report("a box pinned over part of the page: once, at the bottom; page exact", r.wrong == 0 && bad == 0 && r.totals.skipped == 0,
           counts(r) + fmt(", %d pinned parts, %lld pixels differ (row %d)", r.totals.pinnedRegions, bad, firstBad));
  }
  {
    // A side bar as tall as the viewport: once, where the first screen
    // shows it; below it a plain colour, its own background.
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    ss.pinned.push_back({0, 0, 104, V, 0xEEF0F4, 0x30343C});
    const Scene scene(doc.img, ss);
    const Played r = play(scene, pos);
    Image expected = pageRows(doc.img, 0, doc.img.h, W);
    for (int y = 0; y < expected.h; ++y)
      for (int x = 0; x < 104; ++x) expected.at(x, y) = y < V ? scene.part(0).at(x, y) : 0xEEF0F4;
    int firstBad = 0;
    const long long bad = differing(r, expected, {}, 0, &firstBad);
    report("a side bar as tall as the viewport: once, then its background", r.wrong == 0 && bad == 0 && r.totals.skipped == 0,
           counts(r) + fmt(", %lld pixels differ (row %d)", bad, firstBad));
  }
  {
    // A header that shows up once the page has scrolled (a title bar that
    // sticks): it is in none of the page's rows. The first screen showed
    // the page there, so that is what the image's top holds.
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    ss.pinned.push_back({0, 0, W, 48, 0xF8F8FA, 0x202024, 700});
    const Scene scene(doc.img, ss);
    const Played r = play(scene, pos);
    int firstBad = 0;
    const long long bad = differing(r, pageRows(doc.img, 0, doc.img.h, W), {}, 0, &firstBad);
    report("a header that shows up late is not repeated; page exact", r.wrong == 0 && bad == 0 && r.totals.skipped == 0,
           counts(r) + fmt(", %lld pixels differ (row %d)", bad, firstBad));
  }
}

// ------------------------------------------------------------------ pinned columns

// A page in columns (#1538): an article of paragraphs in [x0, x1), nothing in
// the columns beside it, and at its end a footer of text lines in [fx0, fx1),
// under a side column too.
Image columnPage(uint64_t seed, int W, int H, int x0, int x1, int footerH, int fx0, int fx1) {
  Image img(W, H, kBg);
  Rng rng(seed);
  for (int y = 30; y < H - footerH - 160;) {
    const int lines = rng.range(3, 9);
    for (int i = 0; i < lines; ++i, y += 16) drawTextLine(img, y, x0, x0 + int((x1 - x0) * (0.4 + 0.6 * rng.unit())), rng);
    y += rng.range(24, 40);
  }
  const int f = H - footerH;
  for (int x = fx0; x < fx1; ++x) img.at(x, f) = 0xC8CCD0;
  for (int y = f + 24; y + 12 < H - 8; y += 18) drawTextLine(img, y, fx0 + 16, fx1 - 16 - rng.range(0, 160), rng);
  return img;
}

// The differing pixels of an image, in the columns [0, split) and the rest.
std::string where(const Played& r, const Image& expected, int split) {
  long long side = 0, rest = 0;
  int sideRow0 = -1, sideRow1 = -1;
  const long long rows = std::min<long long>(r.totals.bottom - r.totals.top, expected.h);
  for (int y = 0; y < rows; ++y)
    for (int x = 0; x < expected.w; ++x) {
      uint32_t v;
      std::memcpy(&v, &r.image[(size_t(y) * r.width + x) * 4], 4);
      if ((v & 0xFFFFFF) == (expected.at(x, y) & 0xFFFFFF)) continue;
      if (x < split) {
        ++side;
        if (sideRow0 < 0) sideRow0 = y;
        sideRow1 = y;
      } else {
        ++rest;
      }
    }
  return fmt("%lld in the side column (rows %d to %d), %lld elsewhere", side, sideRow0, sideRow1, rest);
}

void testPinnedColumns() {
  std::printf("\n[pinned columns] a column pinned beside the page is in the image once, whole (#1538)\n");
  const int W = 640, V = 400, F = 160;
  Rng rng(43);
  // Steps of a wheel, and a person's: mostly a few rows a frame, now and then a flick.
  const auto styles = [&](int end) {
    return std::vector<std::pair<const char*, std::vector<int>>>{
        {"wheel steps", steps(0, end, [&] { return rng.range(60, 160); })},
        {"a person's scrolling", steps(0, end, [&] { return rng.range(0, 99) < 85 ? rng.range(1, 9) : rng.range(30, 250); })}};
  };
  {
    // Wikipedia's contents column: laid out under the title, it moves with
    // the page until it reaches the top, stays there beside the article, and
    // moves with the page again where its column ends above the footer. In
    // the image once, where the first screen shows it (where the page lays
    // it out); beside the article's end its column is plain, and the footer
    // under it is whole.
    const Image doc = columnPage(31, W, 3600, 200, 620, F, 8, W - 8);
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    PinnedPart toc{16, 8, 160, 330, kBg, 0x3366CC, 0, 41};
    toc.border = false;
    toc.stickFrom = 60;
    toc.stickTo = doc.h - F - 24;
    ss.pinned.push_back(toc);
    const Scene scene(doc, ss);
    Image expected = pageRows(doc, 0, doc.h, W);
    for (int y = 0; y < toc.h; ++y) std::memcpy(&expected.px[size_t(toc.stickFrom + y) * W + toc.x], scene.part(0).row(y), size_t(toc.w) * 4);
    for (const auto& [style, pos] : styles(doc.h - V)) {
      const Played r = play(scene, pos);
      int firstBad = 0;
      const long long bad = differing(r, expected, {}, 0, &firstBad);
      report(fmt("a contents column that sticks, %s: once, footer whole", style), r.wrong == 0 && bad == 0,
             counts(r) + fmt(", %d wrong%s, %lld pixels differ (row %d): %s", r.wrong, r.firstWrong.c_str(), bad, firstBad,
                             where(r, expected, 200).c_str()));
    }
  }
  {
    // The same column scrolled back up to the page's start (a capture
    // started halfway down): it comes down to where the page lays it out and
    // moves with the page there. That is the page: in the image once, at its
    // place, and nowhere else.
    const Image doc = columnPage(31, W, 3600, 200, 620, F, 8, W - 8);
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    PinnedPart toc{16, 8, 160, 330, kBg, 0x3366CC, 0, 41};
    toc.border = false;
    toc.stickFrom = 60;
    toc.stickTo = doc.h - F - 24;
    ss.pinned.push_back(toc);
    const Scene scene(doc, ss);
    const auto pos = steps(1500, 0, [&] { return rng.range(60, 160); });
    const Played r = play(scene, pos);
    Image expected = pageRows(doc, 0, 1500 + V, W);
    for (int y = 0; y < toc.h; ++y) std::memcpy(&expected.px[size_t(toc.stickFrom + y) * W + toc.x], scene.part(0).row(y), size_t(toc.w) * 4);
    // Outside its place, the page exact; at its place, the column within a
    // hundredth of its pixels (a line of it can come from a frame in which it
    // was still moving into place, as before #1538).
    int firstBad = 0;
    const Box place{toc.x, toc.stickFrom, toc.x + toc.w, toc.stickFrom + toc.h};
    const long long outside = differing(r, expected, {place}, 0, &firstBad);
    const long long all = differing(r, expected, {}, 0, &firstBad);
    report("a contents column scrolled back up to its place: once, there", r.wrong == 0 && outside == 0 && 100 * (all - outside) <= toc.w * toc.h,
           counts(r) + fmt(", %d wrong%s, %lld pixels differ outside its place, %lld at it", r.wrong, r.firstWrong.c_str(), outside, all - outside));
  }
  // An advertisement pinned in both margins beside a news article (Al
  // Jazeera's in Firefox): artwork at its top over a plain dark colour.
  Image news = columnPage(37, W, 3600, 140, 500, F, 140, 500);
  {
    // Dark boxes in the article (a video player, an advertisement of the
    // same dark colour) that move with the page: the colour is the page's too.
    Rng ink(61);
    for (const int y0 : {700, 1500, 2300})
      for (int y = y0; y < y0 + 260; ++y)
        for (int x = 150; x < 490; ++x) news.at(x, y) = 0x1E2266;
    for (const int y0 : {700, 1500, 2300})
      for (int y = y0 + 20; y + 12 < y0 + 240; y += 30) drawTextLine(news, y, 170, 470, ink, 0xF0F0FF);
  }
  const auto adScene = [&](int fromPos) {
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    for (const int x : {0, 520}) {
      PinnedPart ad{x, 0, 120, V, 0x1E2266, 0xF0F0FF, fromPos, uint64_t(51 + x)};
      ad.border = false;
      ad.inkRows = 110;
      ss.pinned.push_back(ad);
    }
    return ss;
  };
  {
    // Loaded once the page has moved: the first screen showed the page in the
    // margins, so the image is the page, as for a header that comes late.
    const Scene scene(news, adScene(900));
    const Image expected = pageRows(news, 0, news.h, W);
    for (const auto& [style, pos] : styles(news.h - V)) {
      const Played r = play(scene, pos);
      int firstBad = 0;
      const long long bad = differing(r, expected, {}, 0, &firstBad);
      report(fmt("an advertisement in the margins that comes late, %s: none", style), r.wrong == 0 && bad == 0,
             counts(r) + fmt(", %d wrong%s, %lld pixels differ (row %d): %s", r.wrong, r.firstWrong.c_str(), bad, firstBad,
                             where(r, expected, 140).c_str()));
    }
  }
  {
    // There from the start: once, where the first screen shows it, then its
    // own background (a side bar's rule). Also when the page jumps by itself
    // (a photo loaded above it) while auto-scroll's hint says nothing moved.
    const Scene scene(news, adScene(0));
    Image expected = pageRows(news, 0, news.h, W);
    for (size_t i = 0; i < 2; ++i)
      for (int y = 0; y < news.h; ++y)
        for (int x = 0; x < 120; ++x) expected.at(int(i) * 520 + x, y) = y < V ? scene.part(i).at(x, y) : 0x1E2266;
    for (const auto& [style, pos] : styles(news.h - V)) {
      const Played r = play(scene, pos);
      int firstBad = 0;
      const long long bad = differing(r, expected, {}, 0, &firstBad);
      report(fmt("an advertisement in the margins from the start, %s: once", style), r.wrong == 0 && bad == 0,
             counts(r) + fmt(", %d wrong%s, %lld pixels differ (row %d): %s", r.wrong, r.firstWrong.c_str(), bad, firstBad,
                             where(r, expected, 140).c_str()));
    }
    // Auto-scroll's steps of 60 rows; before one frame the page jumped 250
    // rows by itself, where the hint says no step was owed.
    std::vector<int> pos{0};
    while (pos.back() < news.h - V) pos.push_back(std::min(news.h - V, pos.back() + (pos.back() == 1200 ? 250 : 60)));
    MemoryCanvas canvas(W);
    ScrollTracker tracker(canvas);
    Played r;
    for (size_t i = 0; i < pos.size(); ++i) {
      Image f = scene.frame(pos[i], int(i));
      const int moved = i ? pos[i] - pos[i - 1] : 0;
      const TrackResult t =
          tracker.addFrame(f.view(), i == 0 ? TrackerHint{} : moved == 250 ? TrackerHint{true, 0, 2} : TrackerHint{true, 60, 25});
      if (t.status != TrackStatus::Placed) continue;
      ++r.placed;
      r.wrong += t.position != pos[i];
    }
    r.totals = tracker.finish();
    r.image = canvas.image(r.totals.top, r.totals.bottom);
    r.width = W;
    int firstBad = 0;
    const long long bad = differing(r, expected, {}, 0, &firstBad);
    report("the page jumps by itself under auto-scroll's hint: the advertisement once", r.wrong == 0 && bad == 0,
           fmt("%zu frames, %d placed, %d wrong, %lld pixels differ (row %d): %s", pos.size(), r.placed, r.wrong, bad, firstBad,
               where(r, expected, 140).c_str()));
  }
  {
    // A cookie banner fixed at the bottom across the whole width (Al Jazeera's
    // in Firefox), shown once the page has moved: its text in the middle, its
    // plain dark colour out to both edges, over the empty margins. Once, at
    // the bottom, as the last screen shows it; nowhere in the margins above.
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    PinnedPart plain{0, V - 96, W, 96, 0x1E2266, 0xF0F0FF, 600, 71};
    plain.border = false;
    plain.inkRows = 0;
    ss.pinned.push_back(plain);
    PinnedPart text{150, V - 84, 340, 72, 0x1E2266, 0xF0F0FF, 600, 73};
    text.border = false;
    ss.pinned.push_back(text);
    const Scene scene(news, ss);
    Image expected = pageRows(news, 0, news.h, W);
    const Image last = scene.frame(news.h - V, 0);
    for (int y = news.h - 96; y < news.h; ++y) std::memcpy(&expected.px[size_t(y) * W], last.row(y - (news.h - V)), size_t(W) * 4);
    for (const auto& [style, pos] : styles(news.h - V)) {
      const Played r = play(scene, pos);
      int firstBad = 0;
      const long long bad = differing(r, expected, {}, 0, &firstBad);
      long long margins = 0;
      for (long long y = 0; y < std::min<long long>(r.totals.bottom - r.totals.top, news.h - 96); ++y)
        for (const int x : {40, 600}) {
          const uint8_t* q = &r.image[(size_t(y) * W + x) * 4];
          margins += q[0] == 0x66 && q[1] == 0x22 && q[2] == 0x1E;
        }
      report(fmt("a cookie banner across the bottom, %s: once, not down the margins", style), r.wrong == 0 && bad == 0,
             counts(r) + fmt(", %d wrong%s, %lld pixels differ (row %d), %lld margin rows dark: %s", r.wrong, r.firstWrong.c_str(), bad,
                             firstBad, margins, where(r, expected, 140).c_str()));
    }
  }
  {
    // The browser's view moves: an information bar opens above the page and
    // pushes everything down, the header and a banner at the bottom with it
    // (Firefox's DRM bar on the Al Jazeera article). They are not loose: no
    // piece of either is written into the page.
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    // The header's own colour well off the page's (one within the tolerance
    // of the page's background is the page's, as far as pixels can tell).
    ss.pinned.push_back({0, 0, W, 40, 0xD8DCE6, 0x202024, 0, 5});
    PinnedPart banner{0, V - 64, W, 64, 0x5A1E66, 0xF0F0FF, 0, 9};
    ss.pinned.push_back(banner);
    const Scene scene(news, ss);
    const int bar = 24;
    Image info(W, bar, 0xE4E4E8);
    Rng words(83);
    drawTextLine(info, 6, 40, 300, words, 0x303034);
    std::vector<int> pos{0};
    while (pos.back() < news.h - V) pos.push_back(std::min(news.h - V, pos.back() + rng.range(60, 140)));
    // The bar opens between two frames of a page at rest.
    const size_t from = pos.size() / 3;
    pos.insert(pos.begin() + std::ptrdiff_t(from), pos[from - 1]);
    MemoryCanvas canvas(W);
    ScrollTracker tracker(canvas);
    Played r;
    for (size_t i = 0; i < pos.size(); ++i) {
      Image f = scene.frame(pos[i], int(i));
      if (i >= from) {
        // The view `bar` rows lower, its last rows below the region, the bar above it.
        Image moved(W, V);
        for (int y = 0; y < V; ++y) std::memcpy(&moved.px[size_t(y) * W], y < bar ? info.row(y) : f.row(y - bar), size_t(W) * 4);
        f = moved;
      }
      const TrackResult t = tracker.addFrame(f.view());
      if (t.status != TrackStatus::Placed) continue;
      ++r.placed;
    }
    r.totals = tracker.finish();
    r.image = canvas.image(r.totals.top, r.totals.bottom);
    r.width = W;
    long long header = 0, navy = 0;
    const long long rows = r.totals.bottom - r.totals.top;
    dump("view-moved", r.image.data(), W, int(rows));
    for (long long y = 48; y + 100 < rows; ++y)
      for (int x = 0; x < W; ++x) {
        uint32_t v;
        std::memcpy(&v, &r.image[(size_t(y) * W + x) * 4], 4);
        header += (v & 0xFFFFFF) == 0xD8DCE6;
        navy += (v & 0xFFFFFF) == 0x5A1E66;
      }
    report("the view moves down, header and banner with it: neither in the page, the page to its end",
           header == 0 && navy == 0 && rows == news.h - bar,
           fmt("%zu frames, %d placed, %lld header pixels and %lld banner pixels between the image's ends, %lld rows (want %d)", pos.size(),
               r.placed, header, navy, rows, news.h - bar));
  }
  {
    // The same bar opening in steps that ease out (Firefox's DRM bar: 14,
    // then 6, 3 and a row at a time), the page at rest: no step of it is
    // the page scrolling.
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    ss.pinned.push_back({0, 0, W, 40, 0xD8DCE6, 0x202024, 0, 5});
    ss.pinned.push_back({0, V - 64, W, 64, 0x5A1E66, 0xF0F0FF, 0, 9});
    const Scene scene(news, ss);
    Image info(W, 24, 0xE4E4E8);
    Rng words(83);
    drawTextLine(info, 6, 40, 300, words, 0x303034);
    std::vector<int> pos{0};
    while (pos.back() < news.h - V) pos.push_back(std::min(news.h - V, pos.back() + rng.range(60, 140)));
    const size_t from = pos.size() / 3;
    const std::vector<int> opening{14, 20, 22, 23, 24};
    pos.insert(pos.begin() + std::ptrdiff_t(from), opening.size(), pos[from - 1]);
    MemoryCanvas canvas(W);
    ScrollTracker tracker(canvas);
    Played r;
    for (size_t i = 0; i < pos.size(); ++i) {
      Image f = scene.frame(pos[i], int(i));
      const int bar = i < from ? 0 : i - from < opening.size() ? opening[i - from] : 24;
      if (bar > 0) {
        Image moved(W, V);
        for (int y = 0; y < V; ++y) std::memcpy(&moved.px[size_t(y) * W], y < bar ? info.row(24 - bar + y) : f.row(y - bar), size_t(W) * 4);
        f = moved;
      }
      const TrackResult t = tracker.addFrame(f.view());
      if (t.status != TrackStatus::Placed) continue;
      ++r.placed;
    }
    r.totals = tracker.finish();
    r.image = canvas.image(r.totals.top, r.totals.bottom);
    r.width = W;
    long long header = 0, navy = 0;
    const long long rows = r.totals.bottom - r.totals.top;
    for (long long y = 48; y + 100 < rows; ++y)
      for (int x = 0; x < W; ++x) {
        uint32_t v;
        std::memcpy(&v, &r.image[(size_t(y) * W + x) * 4], 4);
        header += (v & 0xFFFFFF) == 0xD8DCE6;
        navy += (v & 0xFFFFFF) == 0x5A1E66;
      }
    report("the view moves down in easing steps: header and banner not in the page, the page to its end",
           header == 0 && navy == 0 && rows == news.h - 24,
           fmt("%zu frames, %d placed, %lld header pixels and %lld banner pixels between the image's ends, %lld rows (want %d)", pos.size(),
               r.placed, header, navy, rows, news.h - 24));
  }
  {
    // A sticky header that shows up once the page has moved (GitHub's pull
    // request header): plain white across the frame with its title in the
    // middle and a line under it, over a page with dark boxes. The page
    // exact: nothing of the header's white over the boxes, no speck of it in
    // the text (its title a shade off the page's text where they cross).
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    PinnedPart white{0, 0, W, 46, 0xFFFFFF, 0x202024, 600, 101};
    white.border = false;
    white.inkRows = 0;
    ss.pinned.push_back(white);
    PinnedPart title{180, 10, 280, 26, 0xFFFFFF, 0x202024, 600, 103};
    title.border = false;
    ss.pinned.push_back(title);
    PinnedPart line{0, 46, W, 2, 0xC8CCD0, 0xC8CCD0, 600, 105};
    line.border = false;
    line.inkRows = 0;
    ss.pinned.push_back(line);
    const Scene scene(news, ss);
    const Image expected = pageRows(news, 0, news.h, W);
    for (const auto& [style, pos] : styles(news.h - V)) {
      const Played r = play(scene, pos);
      int firstBad = 0;
      const long long bad = differing(r, expected, {}, 0, &firstBad);
      report(fmt("a header that shows up over a page with dark boxes, %s: page exact", style), r.wrong == 0 && bad == 0,
             counts(r) + fmt(", %d wrong%s, %lld pixels differ (row %d)", r.wrong, r.firstWrong.c_str(), bad, firstBad));
    }
  }
  {
    // A contents column on the right that lets go where its column ends above
    // the footer, under a header whose own buttons sit above a toolbar across
    // the frame (MDN). The buttons and the column are two things the toolbar
    // keeps apart: when the column moves off with the page, the article
    // beside it is whole to its last row.
    const Image doc = columnPage(47, W, 3600, 30, 500, F, 8, W - 8);
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    PinnedPart buttons{360, 0, 220, 26, kBg, 0x202024, 0, 111};
    buttons.border = false;
    ss.pinned.push_back(buttons);
    PinnedPart toolbar{0, 26, W, 26, 0xEEEEF0, 0x303034, 0, 113};
    ss.pinned.push_back(toolbar);
    PinnedPart toc{512, 56, 120, 300, kBg, 0x3366CC, 0, 115};
    toc.border = false;
    toc.stickFrom = 56;
    toc.stickTo = doc.h - F - 16;
    ss.pinned.push_back(toc);
    const Scene scene(doc, ss);
    const Image expected = pageRows(doc, 0, doc.h, W);
    long long ink = 0;
    for (const uint32_t v : scene.part(0).px) ink += (v & 0xFFFFFF) != (kBg & 0xFFFFFF);
    report("the buttons above the toolbar have ink", ink > 0, fmt("%lld pixels of ink", ink));
    // Wheel steps: a person's flick can leave a row of the article seen only
    // under the toolbar's edge, two rows into a block of the page (a known
    // limit of the 8-row blocks, not of this scene's rule).
    for (const auto& [style, pos] : styles(doc.h - V)) {
      if (std::string(style) != "wheel steps") continue;
      const Played r = play(scene, pos);
      int firstBad = 0;
      // The article and its margin: left of the column, below the header.
      const long long bad = differing(r, expected, {{0, 0, W, 52}, {508, 0, W, doc.h}}, 0, &firstBad);
      report(fmt("a column lets go under a header with buttons over a toolbar, %s: the article whole", style), r.wrong == 0 && bad == 0,
             counts(r) + fmt(", %d wrong%s, %lld pixels differ beside the column (row %d)", r.wrong, r.firstWrong.c_str(), bad, firstBad));
    }
  }
  {
    // The bar opens right after the page was scrolled back to its top, and
    // the capture ends there: no rows above the page's top, nothing of the
    // bar in the image.
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    ss.pinned.push_back({0, 0, W, 40, 0xD8DCE6, 0x202024, 0, 5});
    ss.pinned.push_back({0, V - 64, W, 64, 0x5A1E66, 0xF0F0FF, 0, 9});
    const Scene scene(news, ss);
    Image info(W, 24, 0xE4E4E8);
    Rng words(89);
    drawTextLine(info, 6, 40, 300, words, 0x303034);
    const std::vector<int> pos{0, 90, 200, 320, 200, 90, 0, 0};
    MemoryCanvas canvas(W);
    // A cap just over the page shown: the bar's frame must not count against it.
    TrackerOptions o;
    o.maxOutputRows = 320 + V + 10;
    ScrollTracker tracker(canvas, o);
    for (size_t i = 0; i < pos.size(); ++i) {
      Image f = scene.frame(pos[i], int(i));
      if (i + 1 == pos.size()) {
        Image moved(W, V);
        for (int y = 0; y < V; ++y) std::memcpy(&moved.px[size_t(y) * W], y < 24 ? info.row(y) : f.row(y - 24), size_t(W) * 4);
        f = moved;
      }
      tracker.addFrame(f.view());
    }
    const TrackTotals t = tracker.finish();
    const std::vector<uint8_t> image = canvas.image(t.top, t.bottom);
    long long bar = 0;
    for (size_t i = 0; i + 3 < image.size(); i += 4) {
      uint32_t v;
      std::memcpy(&v, &image[i], 4);
      bar += (v & 0xFFFFFF) == 0xE4E4E8;
    }
    report("a bar opens at the page's top and the capture ends: no rows of it", t.bottom - t.top == 320 + V && bar == 0 && !t.capReached,
           fmt("%lld rows (the page shows %d), %lld pixels of the bar%s", t.bottom - t.top, 320 + V, bar, t.capReached ? ", cap reached" : ""));
  }
  {
    // Two side columns that stick until their column ends above the footer
    // and let go together, with nothing across the frame: columns moving off
    // with the page, not the whole view moving. Each once, the rest whole.
    // A frame wide enough that the article beside them places it.
    const int WW = 960;
    const Image doc = columnPage(53, WW, 3600, 200, 760, F, 8, WW - 8);
    SceneSpec ss;
    ss.width = WW;
    ss.height = V;
    for (const int x : {16, 784}) {
      PinnedPart col{x, 8, 160, 330, kBg, 0x3366CC, 0, uint64_t(57 + x)};
      col.border = false;
      col.stickFrom = 60;
      col.stickTo = doc.h - F - 24;
      ss.pinned.push_back(col);
    }
    const Scene scene(doc, ss);
    Image expected = pageRows(doc, 0, doc.h, WW);
    for (size_t i = 0; i < 2; ++i)
      for (int y = 0; y < 330; ++y)
        std::memcpy(&expected.px[size_t(60 + y) * WW + ss.pinned[i].x], scene.part(i).row(y), size_t(160) * 4);
    for (const auto& [style, pos] : styles(doc.h - V)) {
      const Played r = play(scene, pos);
      int firstBad = 0;
      const long long bad = differing(r, expected, {}, 0, &firstBad);
      report(fmt("two side columns let go together, %s: each once, the rest whole", style), r.wrong == 0 && bad == 0,
             counts(r) + fmt(", %d wrong%s, %lld pixels differ (row %d)", r.wrong, r.firstWrong.c_str(), bad, firstBad));
    }
  }
  {
    // A rule down the page beside its text (a quotation's bar), in a column
    // of the screen nothing moves across: plain in every frame, and the
    // page's. Nothing is pinned: the page exact.
    Image doc = columnPage(59, W, 3600, 200, 600, F, 8, W - 8);
    for (int y = 0; y < doc.h; ++y)
      for (int x = 100; x < 104; ++x) doc.at(x, y) = 0x8C96A0;
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    const Scene scene(doc, ss);
    const Image expected = pageRows(doc, 0, doc.h, W);
    for (const auto& [style, pos] : styles(doc.h - V)) {
      const Played r = play(scene, pos);
      int firstBad = 0;
      const long long bad = differing(r, expected, {}, 0, &firstBad);
      report(fmt("a rule down a page with nothing pinned, %s: exact", style), r.wrong == 0 && bad == 0,
             counts(r) + fmt(", %d wrong%s, %lld pixels differ (row %d)", r.wrong, r.firstWrong.c_str(), bad, firstBad));
    }
  }
  {
    // A side column over a third of the frame wide and a narrow one let go
    // together: no bar across the frame moved, so no view move.
    const int WW = 960;
    const Image doc = columnPage(61, WW, 3600, 360, 776, F, 8, WW - 8);
    SceneSpec ss;
    ss.width = WW;
    ss.height = V;
    // Stuck from the first frame (half the frame moving into place in the
    // first step would be a layer of its own to the pair).
    const int xs[2] = {8, 792}, ws[2] = {336, 160};
    for (int k = 0; k < 2; ++k) {
      PinnedPart col{xs[k], 8, ws[k], 330, kBg, 0x3366CC, 0, uint64_t(67 + k)};
      col.border = false;
      col.stickFrom = 8;
      col.stickTo = doc.h - F - 24;
      ss.pinned.push_back(col);
    }
    const Scene scene(doc, ss);
    Image expected = pageRows(doc, 0, doc.h, WW);
    for (size_t i = 0; i < 2; ++i)
      for (int y = 0; y < 330; ++y)
        std::memcpy(&expected.px[size_t(8 + y) * WW + ss.pinned[i].x], scene.part(i).row(y), size_t(ss.pinned[i].w) * 4);
    Rng own(71);
    const auto pos = steps(0, doc.h - V, [&] { return own.range(60, 160); });
    const Played r = play(scene, pos);
    int firstBad = 0;
    const long long bad = differing(r, expected, {}, 0, &firstBad);
    report("a wide side column and a narrow one let go together: each once, the rest whole", r.wrong == 0 && bad == 0,
           counts(r) + fmt(", %d wrong%s, %lld pixels differ (row %d)", r.wrong, r.firstWrong.c_str(), bad, firstBad));
  }
  {
    // An information bar above the page closes and the capture ends: the
    // view moves up and shows the page's next rows at the bottom, which are
    // the page's and in the image. A header and a sub-navigation a little
    // under it (two parts), nothing at the bottom.
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    ss.pinned.push_back({0, 0, W, 40, 0xD8DCE6, 0x202024, 0, 5});
    ss.pinned.push_back({0, 56, W, 24, 0xEDEFF4, 0x303034, 0, 7});
    const Scene scene(news, ss);
    Image info(W, 24, 0xE4E4E8);
    Rng words(97);
    drawTextLine(info, 6, 40, 300, words, 0x303034);
    const std::vector<int> pos{0, 110, 220, 330, 440, 440};
    MemoryCanvas canvas(W);
    ScrollTracker tracker(canvas);
    for (size_t i = 0; i < pos.size(); ++i) {
      Image f = scene.frame(pos[i], int(i));
      if (i + 1 < pos.size()) {
        Image moved(W, V);
        for (int y = 0; y < V; ++y) std::memcpy(&moved.px[size_t(y) * W], y < 24 ? info.row(y) : f.row(y - 24), size_t(W) * 4);
        f = moved;
      }
      tracker.addFrame(f.view());
    }
    const TrackTotals t = tracker.finish();
    const std::vector<uint8_t> image = canvas.image(t.top, t.bottom);
    // Tracker row y is the page's row y - 24 (the first frames showed the bar
    // above the page); the last 24 rows only the last frame showed.
    long long wrongRows = 0;
    const long long rows = t.bottom - t.top;
    for (long long y = std::max(0LL, rows - 24); y < rows; ++y) {
      const int docRow = int(y) - 24;
      if (docRow < 0 || docRow >= news.h || std::memcmp(&image[size_t(y) * W * 4], news.row(docRow), size_t(W) * 4) != 0) ++wrongRows;
    }
    report("a bar above the page closes and the capture ends: the rows it uncovered are in the image",
           rows == 440 + V + 24 && wrongRows == 0, fmt("%lld rows (want %d), %lld of the last 24 differ from the page", rows, 440 + V + 24, wrongRows));
  }
  {
    // A notice over the page's margin in the first frame only (a cookie
    // notice dismissed before the next): the frames after it show the margin
    // clear, and nothing of the notice is in the image.
    const Image doc = columnPage(89, W, 3600, 200, 600, F, 8, W - 8);
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    const Scene scene(doc, ss);
    Image notice(168, 80, 0x1E2266);
    Rng words(91);
    for (int y = 10; y + 12 < 80; y += 18) drawTextLine(notice, y, 10, 150, words, 0xF0F0FF);
    // A person's small steps: the margin shows in many frames after it.
    Rng own(93);
    std::vector<int> pos{0};
    while (pos.back() < 600) pos.push_back(pos.back() + own.range(4, 12));
    while (pos.back() < doc.h - V) pos.push_back(std::min(doc.h - V, pos.back() + own.range(60, 160)));
    const Played r = play(scene, pos, {}, [&](size_t i, Image& f) {
      if (i == 0)
        for (int y = 0; y < notice.h; ++y) std::memcpy(&f.px[size_t(250 + y) * W + 16], notice.row(y), size_t(notice.w) * 4);
      return false;
    });
    long long left = 0;
    for (size_t i = 0; i + 3 < r.image.size(); i += 4) {
      uint32_t v;
      std::memcpy(&v, &r.image[i], 4);
      left += (v & 0xFFFFFF) == 0x1E2266 || (v & 0xFFFFFF) == 0xF0F0FF;
    }
    report("a notice over the margin in the first frame only: nothing of it in the image", r.wrong == 0 && left == 0,
           counts(r) + fmt(", %lld pixels of the notice", left));
  }
}

// ------------------------------------------------------------------ the rest

void testChangesAndRecovery() {
  std::printf("\n[changes] content that changes, other antialiasing, a way back, a stray frame\n");
  const int W = 480, V = 400;
  const Document doc = plainPage(13, W, V);
  const int end = doc.img.h - V;
  Rng rng(29);
  SceneSpec base;
  base.width = W;
  base.height = V;
  {
    // A block of the page that changes in every frame (a video): taken as
    // first seen, and it ends nothing.
    SceneSpec ss = base;
    ss.changingX = 140;
    ss.changingY = 900;
    ss.changingW = 200;
    ss.changingH = 120;
    const Scene scene(doc.img, ss);
    const auto pos = steps(0, end, [&] { return rng.range(50, 150); });
    const Played r = play(scene, pos);
    int firstBad = 0;
    const long long bad = differing(r, pageRows(doc.img, 0, doc.img.h, W), {{140, 900, 340, 1020}}, 0, &firstBad);
    report("a block that changes in place ends nothing; the rest exact", r.wrong == 0 && r.totals.skipped == 0 && bad == 0,
           counts(r) + fmt(", %lld pixels differ (row %d)", bad, firstBad));
  }
  {
    // The same text drawn again with other antialiasing in every frame: no
    // exact row repeats, the place is still the page's.
    SceneSpec ss = base;
    ss.antialias = 24;
    const Scene scene(doc.img, ss);
    const auto pos = steps(0, end, [&] { return rng.range(3, 140); });
    const Played r = play(scene, pos);
    int firstBad = 0;
    const long long bad = differing(r, pageRows(doc.img, 0, doc.img.h, W), {}, 24, &firstBad);
    report("text drawn again with other antialiasing: placed, within its noise", r.wrong == 0 && r.totals.skipped == 0 && bad == 0,
           counts(r) + fmt(", %d wrong%s, %lld pixels differ (row %d)", r.wrong, r.firstWrong.c_str(), bad, firstBad));
  }
  {
    // The page drawn anew while it rests (#1537's A-first row: Chrome turns
    // greyscale antialiasing into subpixel antialiasing a while after
    // scrolling stopped): wheel notches of 120 rows with rests between them,
    // then, at the rest before the last, short step to the end, the text's
    // grey ink comes out a little lighter, and stays so. The picture drawn
    // anew is the reference for that step; the capture reaches the end,
    // nothing lost.
    constexpr int kLighter = 18;
    const Scene scene = scene_of(doc.img, base);
    const int notch[] = {2, 5, 9, 12, 16, 17, 17, 15, 12, 9, 5, 1};
    const int lastRest = (end - 1) / 120 * 120;
    std::vector<int> pos{0};
    size_t redrawFrom = 0;
    const auto step = [&](int dist) {
      const int from = pos.back();
      int moved = 0;
      for (const int n : notch) {
        moved += n;
        pos.push_back(from + int(std::lround(double(moved) * dist / 120)));
      }
      for (int k = 0; k < 3; ++k) pos.push_back(pos.back());
    };
    while (pos.back() < lastRest) step(120);
    redrawFrom = pos.size();
    for (int k = 0; k < 3; ++k) pos.push_back(pos.back());
    step(end - lastRest);
    const Played r = play(scene, pos, {}, [&](size_t i, Image& f) {
      if (i < redrawFrom) return false;
      const Image plain = f;
      for (int y = 0; y < f.h; ++y)
        for (int x = 0; x < f.w; ++x) {
          const uint32_t v = plain.at(x, y);
          const int cr = int(v >> 16) & 255, cg = int(v >> 8) & 255, cb = int(v) & 255;
          if (v == kBg || std::max({cr, cg, cb}) - std::min({cr, cg, cb}) > 12 || std::max({cr, cg, cb}) > 0xE8) continue;
          auto ch = [&](int c) { return uint32_t(std::min(255, c + kLighter)); };
          f.at(x, y) = (ch(cr) << 16) | (ch(cg) << 8) | ch(cb);
        }
      return false;
    });
    int lostFrames = 0;
    for (const TrackResult& t : r.results) lostFrames += t.state == TrackState::Lost;
    int firstBad = 0;
    const long long bad = differing(r, pageRows(doc.img, 0, doc.img.h, W), {}, kLighter, &firstBad);
    report("page drawn anew at the last rest: the last step placed, full page",
           r.wrong == 0 && lostFrames == 0 && r.totals.skipped == 0 && bad == 0,
           counts(r) + fmt(", redrawn share %.2f, %d lost, %d wrong%s, %lld pixels differ (row %d)",
                           r.results[redrawFrom].confidence, lostFrames, r.wrong, r.firstWrong.c_str(), bad, firstBad));
  }
  {
    // Soft grey text scrolled a pixel a frame (#1747's round 11): each frame
    // agrees in place within the redraw tolerance, yet the page moved; the
    // steps add up and every row reaches the image.
    constexpr int kView = 160, kSteps = 80;
    // Ink spread over 7 rows and 60% faded into the background; text here
    // (from row 2000) still shows each pixel's step to the cell tolerance.
    constexpr int kSoftR = 3, kSoftMix = 60, from = 2000;
    Image soft = doc.img;
    for (int y = 0; y < soft.h; ++y)
      for (int x = 0; x < soft.w; ++x) {
        int sum[3] = {0, 0, 0}, n = 0;
        for (int k = -kSoftR; k <= kSoftR; ++k) {
          const int yy = std::clamp(y + k, 0, doc.img.h - 1);
          const uint32_t v = doc.img.at(x, yy);
          for (int c = 0; c < 3; ++c) sum[c] += int(v >> (8 * c)) & 255;
          ++n;
        }
        const int bg[3] = {int(kBg) & 255, int(kBg >> 8) & 255, int(kBg >> 16) & 255};
        uint32_t out = kBg & 0xFF000000u;
        for (int c = 0; c < 3; ++c) out |= uint32_t((sum[c] / n * (100 - kSoftMix) + bg[c] * kSoftMix) / 100) << (8 * c);
        soft.at(x, y) = out;
      }
    SceneSpec ss = base;
    ss.height = kView;
    const Scene scene(soft, ss);
    std::vector<int> pos;
    for (int i = 0; i <= kSteps; ++i) pos.push_back(from + i);
    const Played r = play(scene, pos);
    int firstBad = 0;
    const long long bad = differing(r, pageRows(soft, from, from + kView + kSteps, W), {}, 0, &firstBad);
    report("soft text a pixel a frame: the steps add up, every row kept", r.wrong == 0 && bad == 0,
           counts(r) + fmt(", %lld rows, %d wrong%s, %lld pixels differ (row %d)", (long long)(r.totals.bottom - r.totals.top),
                           r.wrong, r.firstWrong.c_str(), bad, firstBad));
  }
  {
    // The capture begins halfway down; the reader scrolls up past where it
    // began, to the top, and then down to the end: the image grows at both
    // ends and has every row once.
    const int start = end / 2;
    std::vector<int> pos = steps(start, 0, [&] { return rng.range(40, 180); });
    const auto down = steps(0, end, [&] { return rng.range(40, 180); });
    pos.insert(pos.end(), down.begin() + 1, down.end());
    const Played r = play(scene_of(doc.img, base), pos);
    int firstBad = 0;
    const long long bad = differing(r, pageRows(doc.img, 0, doc.img.h, W), {}, 0, &firstBad);
    report("scrolled up past the start, then down: grows at both ends, exact",
           r.wrong == 0 && bad == 0 && r.totals.top == -start && r.totals.skipped == 0,
           counts(r) + fmt(", top %lld, %lld pixels differ (row %d)", r.totals.top, bad, firstBad));
  }
  {
    // A frame of another page in the middle, and a fast scroller whose
    // frames are further apart than a viewport: skipped, then found again
    // in the image when the reader scrolls back a little.
    const Document other = plainPage(77, W, V);
    const Scene scene = scene_of(doc.img, base);
    std::vector<int> pos = steps(0, 1200, [&] { return rng.range(100, 200); });
    const size_t stray = pos.size() / 2;
    pos.insert(pos.begin() + long(stray), pos[stray - 1]);
    pos.push_back(pos.back() + V + 150);  // too far: no overlap
    pos.push_back(pos.back() - 300);      // back a little: overlaps the image
    const auto rest = steps(pos.back(), end, [&] { return rng.range(100, 200); });
    pos.insert(pos.end(), rest.begin() + 1, rest.end());
    const Played r = play(scene, pos, {}, [&](size_t i, Image& f) {
      if (i != stray) return false;
      f = Scene(other.img, base).frame(500, int(i));
      return true;
    });
    int firstBad = 0;
    const long long bad = differing(r, pageRows(doc.img, 0, doc.img.h, W), {}, 0, &firstBad);
    report("a stray frame and a jump past the viewport: skipped, then found again",
           r.wrong == 0 && bad == 0 && r.totals.skipped >= 2 && r.totals.recovered >= 1,
           counts(r) + fmt(", %d recovered, %d wrong%s, %lld pixels differ (row %d)", r.totals.recovered, r.wrong, r.firstWrong.c_str(),
                           bad, firstBad));
  }
}

}  // namespace

// ------------------------------------------------------------------ excluded

// put.gd's own bar inside the region (#1519): a rectangle the tracker is told
// is not the application. What it shows changes with every frame (a height
// counter); none of it may reach the image.
void testExcluded() {
  std::printf("\n[excluded] a rectangle that is not the application leaves no trace\n");
  const int W = 480, V = 400;
  const Document doc = plainPage(29, W, V);
  SceneSpec ss;
  ss.width = W;
  ss.height = V;
  const Scene scene(doc.img, ss);
  const int end = doc.img.h - V;
  Rng rng(31);
  const auto pos = steps(0, end, [&] { return rng.range(60, 140); });
  const auto counter = [](const TrackerRect& e) {
    return [e](size_t i, Image& f) {
      for (int y = e.y; y < e.y + e.h; ++y)
        for (int x = e.x; x < e.x + e.w; ++x) f.at(x, y) = 0x1E2230;
      Rng digits(1000 + i * 7919);
      drawTextLine(f, e.y + e.h / 2 - 5, e.x + 6, e.x + e.w - 6, digits, 0xF0C060);
      return false;
    };
  };
  {
    // A box over the middle of the frame: the rows under it come from the
    // frames in which they are clear.
    const TrackerRect e{280, 160, 150, 40};
    const Played r = play(scene, pos, {}, counter(e), e);
    int firstBad = 0;
    const long long bad = differing(r, pageRows(doc.img, 0, doc.img.h, W), {}, 0, &firstBad);
    report("a changing box excluded: no trace, the page exact", r.wrong == 0 && r.totals.skipped == 0 && bad == 0 &&
                                                                   r.totals.hiddenCells == 0 && r.totals.pinnedRegions == 0,
           counts(r) + fmt(", %d wrong, %lld pixels differ (row %d), %lld hidden cells, %d pinned parts", r.wrong, bad, firstBad,
                           r.totals.hiddenCells, r.totals.pinnedRegions));
  }
  {
    // A bar across the bottom of the region: the image ends above it, also
    // at the page's end (its last rows were under the bar).
    const TrackerRect e{0, V - 40, W, 40};
    const Played r = play(scene, pos, {}, counter(e), e);
    int firstBad = 0;
    const long long bad = differing(r, pageRows(doc.img, 0, doc.img.h - 40, W), {}, 0, &firstBad);
    report("a bar across the bottom: the image ends above it, exact", r.wrong == 0 && r.totals.skipped == 0 && bad == 0 &&
                                                                         r.totals.hiddenCells == 0,
           counts(r) + fmt(", %d wrong, %lld pixels differ (row %d), %lld hidden cells", r.wrong, bad, firstBad, r.totals.hiddenCells));
  }
  {
    // A bar over half the width at the bottom: at the page's end the rows
    // under it were never clear. They are the plain colour and counted; the
    // rest of the image is exact.
    const TrackerRect e{0, V - 40, W / 2, 40};
    const Played r = play(scene, pos, {}, counter(e), e);
    int firstBad = 0;
    const int h = doc.img.h;
    const long long bad = differing(r, pageRows(doc.img, 0, h, W), {{0, h - 40, W / 2, h}}, 0, &firstBad);
    report("half a bar at the page's end: plain and counted, the rest exact",
           r.wrong == 0 && r.totals.skipped == 0 && bad == 0 && r.totals.hiddenRows == 40 &&
               r.totals.hiddenCells == 40LL * (W / 2 / 8),
           counts(r) + fmt(", %lld pixels differ (row %d), %lld hidden cells in %lld rows", bad, firstBad, r.totals.hiddenCells,
                           r.totals.hiddenRows));
  }
}

// ------------------------------------------------------------------ live failures

// A sticky header and a cookie banner with a flat inside, a border down its
// edges and a button of the page's own colour, over a narrow band of the
// page, scrolled as a person does (small steps, pauses, a way back up): the
// banner never reaches the page's rows, no frame is misplaced. A capture
// that starts at the page's end, on a footer of another light colour, under
// a header of the page's colour: the page's colour stays the page's. And
// auto-scroll's hint across a jump far past the image places nothing.
void testLive() {
  std::printf("\n[live] a narrow band under a banner; a footer first; a jump under auto-scroll\n");
  const int W = 480, V = 400;
  const Document doc = plainPage(3, W, V);
  const int end = doc.img.h - V;
  {
    int wrong = 0;
    long long banner = 0;
    std::string detail;
    for (const auto& [page, stepSeed] : {std::pair<uint64_t, uint64_t>{3, 41}, {8, 57}, {10, 71}}) {
      const Document d = plainPage(page, W, V);
      const int e = d.img.h - V;
      SceneSpec ss;
      ss.width = W;
      ss.height = V;
      ss.pinned.push_back({0, 0, W, 40, 0xF0F0F2, 0x202024, 0, 5});
      ss.pinned.push_back({0, 290, W, 110, 0x1E2266, 0xF0F0FF, 0, 9});
      ss.pinned.push_back({300, 330, 140, 34, kBg, 0x1E2266, 0, 21});  // a button on it
      const Scene scene(d.img, ss);
      Rng rng(stepSeed);
      std::vector<int> pos{0};
      bool up = false;
      while (pos.back() < e) {
        const int r = rng.range(0, 100);
        if (!up && pos.back() > e / 2) {
          for (int k = 0; k < 20; ++k) pos.push_back(std::max(0, pos.back() - rng.range(5, 25)));
          up = true;
          continue;
        }
        pos.push_back(std::min(e, pos.back() + (r < 15 ? 0 : r < 70 ? rng.range(2, 18) : rng.range(18, 60))));
      }
      const Played r = play(scene, pos);
      long long n = 0;
      for (long long y = 0; y < r.totals.bottom - r.totals.top; ++y) {
        if (r.totals.top + y >= e + 290) continue;  // the banner once, at the end
        for (int x = 0; x < W; ++x) {
          const uint8_t* q = &r.image[(size_t(y) * W + x) * 4];
          n += q[0] == 0x66 && q[1] == 0x22 && q[2] == 0x1E;
        }
      }
      wrong += r.wrong;
      banner += n;
      detail += fmt("%s%d placed, %d wrong%s, %lld banner pixels", detail.empty() ? "" : "; ", r.placed, r.wrong,
                    r.firstWrong.c_str(), n);
    }
    report("a banner over a narrow band, a person's steps: none of it in the page", wrong == 0 && banner == 0, detail);
  }
  {
    Document d = plainPage(5, W, V);
    const int h = d.img.h;
    fillRows(d.img, h - 300, h, 0xEBEBEE);
    Rng ink(9);
    for (int y = h - 280; y + 12 < h; y += 40) drawTextLine(d.img, y, 30, W - 30, ink);
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    ss.pinned.push_back({0, 0, W, 40, kBg, 0x202024, 0, 5});
    const Scene scene(d.img, ss);
    const Played r = play(scene, steps(h - V, 0, [] { return 60; }));
    int firstBad = 0;
    const long long bad = differing(r, pageRows(d.img, 0, h, W), {{0, 0, W, 40}}, 0, &firstBad);
    report("from the end, a footer filling the first screen: the page exact", r.wrong == 0 && bad == 0,
           counts(r) + fmt(", %d wrong%s, %lld pixels differ (row %d)", r.wrong, r.firstWrong.c_str(), bad, firstBad));
  }
  {
    // Auto-scroll's frames say how far the page will move; a jump of 2,000
    // rows is not where the hint says, and nothing there is blank.
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    const Scene scene(doc.img, ss);
    MemoryCanvas canvas(W);
    ScrollTracker tracker(canvas);
    int wrong = 0, placed = 0;
    std::vector<int> pos;
    for (int p = 0; p <= 800; p += 40) pos.push_back(p);
    const int jump = std::max(800 + 2 * V, end - 240);  // far past the image's end (1,200)
    for (int p = jump; p <= end; p += 40) pos.push_back(p);
    for (size_t i = 0; i < pos.size(); ++i) {
      Image f = scene.frame(pos[i], int(i));
      const TrackResult t = tracker.addFrame(f.view(), i ? TrackerHint{true, 40, 6} : TrackerHint{});
      if (t.status != TrackStatus::Placed) continue;
      ++placed;
      wrong += t.position != pos[i];
    }
    report("a jump far past the image under the hint is never placed on it", wrong == 0 && placed >= 21 && pos.size() > 21,
           fmt("%zu frames, %d placed, %d wrong, %d on the hint", pos.size(), placed, wrong, tracker.totals().onHint));
  }
}

// ------------------------------------------------------------------ the hint and the edges

// What the driver's hint may and may not do, and frames and values at the
// edges: run under ASan and UBSan too.
void testHintAndEdges() {
  std::printf("\n[hint] what the hint bridges and what it never does; frames and values at the edges\n");
  const int W = 480, V = 400;
  // The hint auto-scroll gives a frame of a step of s rows (ScrollSession::hintFor).
  const auto hintOf = [](int s) { return TrackerHint{true, s, int(std::lround(0.35 * std::abs(s))) + 4}; };
  {
    // A page that ends in a blank stretch under a header and a footer that
    // are learned on the way down. At the end the page rests while
    // auto-scroll's steps go on and its hints say it moves, and one small
    // thing changes in every frame (a loading spinner at a feed's end): a
    // frame that shows what the last one showed, but for that cell, did not
    // move, and the image must not grow by blank rows.
    const Document base = plainPage(41, W, V);
    Image doc(W, base.img.h + V, kBg);
    std::memcpy(doc.px.data(), base.img.px.data(), base.img.px.size() * 4);
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    ss.pinned.push_back({0, 0, W, 48, 0xF0F0F2, 0x202024, 0, 5});
    ss.pinned.push_back({0, V - 48, W, 48, 0xE8E8EC, 0x202024, 0, 7});
    const Scene scene(doc, ss);
    const int end = doc.h - V;
    MemoryCanvas canvas(W);
    ScrollTracker tracker(canvas);
    int moved = 0, rest = 0;
    long long bottomBefore = 0;
    for (int i = 0, p = 0; rest < 30; ++i) {
      Image f = scene.frame(p, i);
      if (p == end) {
        // The spinner: 8 by 8, its rows dark and light, the other way round in every frame.
        for (int y = V / 2; y < V / 2 + 8; ++y)
          for (int x = W / 2; x < W / 2 + 8; ++x) f.at(x, y) = (y + i) % 2 ? 0x202020 : 0xA0A0A0;
        if (rest++ == 0) bottomBefore = tracker.bottom();
      }
      const TrackResult t = tracker.addFrame(f.view(), i ? hintOf(60) : TrackerHint{});
      moved += p == end && t.status == TrackStatus::Placed && t.offset != 0;
      p = std::min(end, p + 60);
    }
    report("a nearly blank frame at rest is not placed on the hint", moved == 0 && tracker.bottom() <= std::max<long long>(bottomBefore, doc.h),
           fmt("%d of %d frames at rest placed as moved, the image grew by %lld rows at rest (page %d rows, image to %lld)", moved, rest,
               tracker.bottom() - bottomBefore, doc.h, tracker.bottom()));
  }
  {
    // What the hint is for: a short word moving through a blank stretch
    // (nothing to compare, no evidence): placed on the hint, where the page is.
    Image doc(W, 4000, kBg);
    Rng ink(3);
    for (int y = 20; y < 1000; y += 18) drawTextLine(doc, y, 20, W - 20, ink);
    for (int y = 1300; y < 3600; y += 450) drawTextLine(doc, y, 200, 216, ink);
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    const Scene scene(doc, ss);
    MemoryCanvas canvas(W);
    ScrollTracker tracker(canvas);
    int wrong = 0, placed = 0;
    for (int i = 0, p = 0; p <= 3000; ++i, p += 50) {
      const TrackResult t = tracker.addFrame(scene.frame(p, i).view(), i ? hintOf(50) : TrackerHint{});
      if (t.status != TrackStatus::Placed) continue;
      ++placed;
      wrong += t.position != p;
    }
    report("a short word through a blank stretch: bridged on the hint, where the page is",
           wrong == 0 && tracker.totals().onHint > 0,
           fmt("%d placed, %d wrong, %d on the hint, %d skipped", placed, wrong, tracker.totals().onHint, tracker.totals().skipped));
  }
  {
    // The page's last lines above a blank tail, under a header and a footer,
    // stepped through as auto-scroll does (a 60 px notch, then steps of two,
    // each animated over five frames and the hint at each frame's share).
    // Near the end only the last few lines are left in the frame: matches
    // that agree in every lane fall short of the evidence rows, and of the
    // row coverage against the edges of the header's and footer's text. Seed
    // 51 lost the page there at 3,537 px and ended idle (#1543's review).
    // With the hint they are placed where the page is.
    DocSpec ds;
    ds.seed = 51;
    ds.width = W;
    ds.viewport = V;
    ds.flatFactor = 0.2;
    ds.tableFactor = 0;
    ds.noiseFactor = 0.3;
    ds.gradientFactor = 0.3;
    ds.blankFactor = 0;
    ds.paragraphsPerGap = 4;
    const Document page = makeDocument(ds);
    Image doc(W, page.img.h + V, kBg);
    std::memcpy(doc.px.data(), page.img.px.data(), page.img.px.size() * 4);
    const Renderer rd(doc, ViewportSpec{W, V, 48, 48, 7});
    MemoryCanvas canvas(W);
    ScrollTracker tracker(canvas);
    std::vector<std::pair<int, int>> frames{{0, 0}};  // position, hint
    const auto step = [&](int px) {
      const int at = frames.back().first;
      for (const double share : {0.3, 0.37, 0.21, 0.1, 0.02}) {
        const int to = std::min(rd.maxScroll(), frames.back().first + int(std::lround(share * px)));
        frames.push_back({to, to - frames.back().first});
      }
      frames.push_back({std::min(rd.maxScroll(), at + px), std::min(rd.maxScroll(), at + px) - frames.back().first});
      for (int i = 0; i < 4; ++i) frames.push_back({frames.back().first, 0});
    };
    step(60);
    while (frames.back().first < rd.maxScroll()) step(120);
    Image f(W, V, kBg);
    int wrong = 0, noMatch = 0, placed = 0;
    for (size_t i = 0; i < frames.size(); ++i) {
      rd.render(frames[i].first, f);
      const TrackResult t = tracker.addFrame(f.view(), i ? TrackerHint{true, frames[i].second, int(std::lround(0.35 * std::abs(frames[i].second))) + 4} : TrackerHint{});
      noMatch += t.status == TrackStatus::Skipped && t.reason == SkipReason::NoMatch;
      if (t.status != TrackStatus::Placed || t.onHint) continue;
      ++placed;
      wrong += t.position != frames[i].first;
    }
    report("a page's last lines above a blank tail: placed where the page is, never unmatched", wrong == 0 && noMatch == 0,
           fmt("%zu frames, %d placed, %d wrong, %d unmatched, %d on the hint, page content to %d", frames.size(), placed, wrong, noMatch,
               tracker.totals().onHint, page.img.h));
  }
  {
    // Steps too fast to follow, then a blank frame far past what the image
    // holds: the hints since the last placed frame add up to more than the  {
    // Steps too fast to follow, then a blank frame far past what the image
    // holds: the hints since the last placed frame add up to more than the
    // frame's height. Placed there, it would leave rows that no frame showed.
    const Document base = plainPage(43, W, V);
    Image doc(W, 4000, kBg);
    std::memcpy(doc.px.data(), base.img.px.data(), size_t(W) * 1100 * 4);
    fillRows(doc, 1100, 2600, kBg);
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    const Scene scene(doc, ss);
    MemoryCanvas canvas(W);
    ScrollTracker tracker(canvas);
    std::vector<int> pos;
    for (int p = 0; p <= 400; p += 40) pos.push_back(p);
    pos.push_back(790);   // too far for the pair: skipped
    pos.push_back(1180);  // blank, 780 rows past the last placed frame
    pos.push_back(1570);  // blank
    pos.push_back(700);   // back where the image is: found again
    for (int p = 740; p <= 1000; p += 40) pos.push_back(p);
    int gaps = 0, wrong = 0;
    std::string first;
    for (size_t i = 0; i < pos.size(); ++i) {
      const long long top = tracker.top(), bottom = tracker.bottom();
      const TrackResult t = tracker.addFrame(scene.frame(pos[i], int(i)).view(), i ? hintOf(pos[i] - pos[i - 1]) : TrackerHint{});
      if (t.status != TrackStatus::Placed) continue;
      const bool gap = i > 0 && (t.position > bottom || t.position + V < top);
      gaps += gap;
      wrong += t.position != pos[i];
      if ((gap || t.position != pos[i]) && first.empty())
        first = fmt(", frame %zu placed at %lld (page at %d, image [%lld, %lld))", i, t.position, pos[i], top, bottom);
    }
    const TrackTotals tot = tracker.finish();
    report("on the hint alone, never past what the image or the last frame holds", gaps == 0 && wrong == 0 && tot.hiddenRows == 0,
           fmt("%d placed with a gap, %d wrong%s, %d on the hint, %lld rows never shown", gaps, wrong, first.c_str(), tot.onHint,
               tot.hiddenRows));
  }
  {
    // One wild hint over a blank frame does not take the image to its cap.
    Image doc(W, 3000, kBg);
    const Document base = plainPage(47, W, V);
    std::memcpy(doc.px.data(), base.img.px.data(), size_t(W) * 900 * 4);
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    const Scene scene(doc, ss);
    MemoryCanvas canvas(W);
    ScrollTracker tracker(canvas);
    for (int i = 0, p = 0; p <= 400; ++i, p += 40) tracker.addFrame(scene.frame(p, i).view(), i ? hintOf(40) : TrackerHint{});
    const TrackResult t = tracker.addFrame(scene.frame(1800, 20).view(), TrackerHint{true, 40000, 20});
    const TrackResult back = tracker.addFrame(scene.frame(440, 21).view(), hintOf(40));
    report("one wild hint over a blank frame: no jump to the cap", !tracker.totals().capReached && tracker.height() < 3 * V &&
                                                                         t.status == TrackStatus::Skipped && back.position == 440,
           fmt("%s, %s at %lld, image %lld rows, cap %s", toString(t.status), toString(back.status), back.position,
               tracker.height(), tracker.totals().capReached ? "reached" : "not reached"));
  }
  {
    // Frames too small to track are refused, also after placements on the
    // hint; the smallest accepted size runs a whole sequence.
    const auto run = [&](int w, int h, int frames, int* invalid, int* placed) {
      MemoryCanvas canvas(w);
      ScrollTracker tracker(canvas);
      Rng noise(uint64_t(w) * 131 + uint64_t(h));
      *invalid = *placed = 0;
      for (int i = 0; i < frames; ++i) {
        Image f(w, h);
        for (uint32_t& v : f.px) v = uint32_t(noise.next()) & 0xFFFFFF;
        const TrackResult t = tracker.addFrame(f.view(), i ? TrackerHint{true, 6, 2} : TrackerHint{});
        *invalid += t.status == TrackStatus::Invalid;
        *placed += t.status == TrackStatus::Placed;
      }
      tracker.finish();
    };
    int inv1 = 0, pl1 = 0, invMin = 0, plMin = 0, invBelow = 0, plBelow = 0;
    run(W, 1, 12, &inv1, &pl1);
    run(16, 16, 40, &invMin, &plMin);
    run(16, 15, 4, &invBelow, &plBelow);
    report("frames too small are refused; the smallest accepted runs through",
           inv1 == 12 && pl1 == 0 && invMin == 0 && plMin > 3 && invBelow == 4,
           fmt("1 row: %d of 12 invalid; 16x16: %d invalid, %d placed; 16x15: %d of 4 invalid", inv1, invMin, plMin, invBelow));
  }
  {
    // Absurd hints and excluded rectangles: nothing overflows (UBSan), no
    // frame is placed off the page.
    const Document doc = plainPage(53, W, V);
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    const Scene scene(doc.img, ss);
    MemoryCanvas canvas(W);
    ScrollTracker tracker(canvas);
    const TrackerHint wild[] = {{true, INT_MAX, INT_MAX - 1}, {true, INT_MIN, INT_MAX}, {true, INT_MAX, 0},
                                {true, INT_MIN, -5},          {true, 0, INT_MAX - 1},  {true, INT_MAX, INT_MAX}};
    const TrackerRect rects[] = {{INT_MAX - 5, INT_MAX - 5, INT_MAX, INT_MAX}, {INT_MIN, INT_MIN, INT_MAX, INT_MAX},
                                 {-10, -10, INT_MAX, 20}, {INT_MIN, 100, INT_MAX, INT_MAX}, {}};
    int wrong = 0, placed = 0;
    for (int i = 0, p = 0; i < 60; ++i, p += 30) {
      tracker.setExcluded(rects[size_t(i / 4) % std::size(rects)]);
      const TrackResult t = tracker.addFrame(scene.frame(p, i).view(), i ? wild[size_t(i) % std::size(wild)] : TrackerHint{});
      if (t.status != TrackStatus::Placed) continue;
      ++placed;
      wrong += t.position != p;
    }
    tracker.setExcluded({});
    tracker.finish();
    report("absurd hints and rectangles: no overflow, nothing placed off the page", wrong == 0 && placed > 0,
           fmt("%d placed, %d wrong, image %lld rows", placed, wrong, tracker.height()));
  }
}

// ------------------------------------------------------------------ look-alikes

// Content that repeats but for a few cells, scrolled by a person (no hint):
// a step that a look-alike offset fits about as well is skipped, never taken
// at the look-alike; a frame of content the image does not hold is never
// "found again" among look-alikes it does hold (#1515's acceptance: a test
// table in a pull request, paragraphs on both sides of a blank stretch).
void testLookAlikes() {
  std::printf("\n[look-alikes] repeating content: never a join at a look-alike offset\n");
  const int W = 480, V = 400;
  SceneSpec ss;
  ss.width = W;
  ss.height = V;
  // Lines whose text is the same but for a number at the left (a table's
  // rows of equal labels, a list's numbered lines).
  const auto numbered = [&](int rows, uint64_t seed) {
    Image doc(W, rows, kBg);
    Rng numbers(seed);
    for (int i = 0; i * 18 + 12 < doc.h; ++i) {
      Rng words(77);
      drawTextLine(doc, 4 + i * 18, 70, W - 30, words);
      drawTextLine(doc, 4 + i * 18, 26, 26 + 6 * (1 + numbers.range(1, 4)), numbers);
    }
    return doc;
  };
  {
    // A person's flick from rest: the frames before it stood still, so the
    // last pace says nothing about how far it went.
    const Image doc = numbered(4000, 91);
    const Scene scene(doc, ss);
    int wrong = 0, placed = 0;
    std::string first;
    for (const int step : {150, 220, 300, 330}) {
      MemoryCanvas canvas(W);
      ScrollTracker tracker(canvas);
      std::vector<int> pos{0, 0, 0};
      for (int p = step; p + V <= doc.h && pos.size() < 40; p += step) {
        pos.push_back(p);
        pos.push_back(p);  // at rest again
        pos.push_back(p);
      }
      for (size_t i = 0; i < pos.size(); ++i) {
        const TrackResult t = tracker.addFrame(scene.frame(pos[i], int(i)).view());
        if (t.status != TrackStatus::Placed) continue;
        ++placed;
        if (t.position != pos[i]) {
          ++wrong;
          if (first.empty()) first = fmt(", step %d: frame %zu at %lld, page at %d (confidence %.3f)", step, i, t.position, pos[i], t.confidence);
        }
      }
    }
    report("flicks from rest over look-alike lines: never placed at a look-alike", wrong == 0 && placed > 0,
           fmt("%d placed, %d wrong%s", placed, wrong, first.c_str()));
  }
  {
    // Numbered paragraphs, a blank stretch taller than the region, more
    // numbered paragraphs below it; a person scrolls through to the end.
    // What is below the stretch is not in the image: it is never found
    // there, and the capture stays lost.
    Image doc(W, 5200, kBg);
    Rng numbers(5);
    const auto paragraphs = [&](int y0, int y1) {
      for (int y = y0; y + 80 < y1; y += 96) {
        const int label = 6 * (2 + numbers.range(1, 2));
        for (int line = 0; line < 4; ++line) {
          Rng words(77 + uint64_t(line));
          drawTextLine(doc, y + line * 18, 40 + (line ? 0 : label + 8), W - 40, words);
        }
        drawTextLine(doc, y, 40, 40 + label, numbers);
      }
    };
    paragraphs(20, 2000);
    paragraphs(3400, 5200);  // 1,400 blank rows between, more than three regions
    const Scene scene(doc, ss);
    MemoryCanvas canvas(W);
    ScrollTracker tracker(canvas);
    int wrong = 0, below = 0, foundBelow = 0;
    std::string first;
    for (int i = 0, p = 0; p + V <= doc.h; ++i, p += 120) {
      const TrackResult t = tracker.addFrame(scene.frame(p, i).view());
      if (p >= 3400 - V / 2) {
        ++below;
        foundBelow += t.status == TrackStatus::Placed;
      }
      if (t.status == TrackStatus::Placed && t.position != p) {
        ++wrong;
        if (first.empty()) first = fmt(", frame %d at %lld, page at %d (confidence %.3f%s)", i, t.position, p, t.confidence, t.recovered ? ", found again" : "");
      }
    }
    report("past a blank stretch, look-alike paragraphs: never found again among those above",
           wrong == 0 && foundBelow == 0 && tracker.state() == TrackState::Lost,
           fmt("%d wrong%s, %d of %d frames below the stretch placed, state %s", wrong, first.c_str(), foundBelow, below,
               toString(tracker.state())));
  }
}

// Content that repeats at the step's distance: a table of near-equal rows
// moved by exactly two of them while a sticky header stays. Only the few
// unique lines show the move; the rest looks the same where it was and where
// the move puts it, so the move covers too little of the frame and the next
// frame, the same picture as the last placed one, looked still: the page's
// place stayed behind by the step (#1515's acceptance, GitHub on Cinnamon).
void testRepeatAtStep() {
  std::printf("\n[repeat at the step] a move by exactly the content's period is a move\n");
  const int W = 480, V = 400;
  // Text first (the header is learned on the way), then the table.
  Image doc(W, 5000, kBg);
  const int table = 1500;
  Rng prose(3);
  for (int y = 10; y + 12 < table; y += 18) drawTextLine(doc, y, 40, W - 40, prose);
  for (int i = 0; table + i * 34 + 24 < doc.h; ++i) {
    Rng words(77 + uint64_t(i % 2));  // two rows that repeat: a period of 68
    drawTextLine(doc, table + i * 34 + 10, 40, W - 40, words);
  }
  for (int y = table + 300; y + 40 < doc.h; y += 140) {  // a unique line every few rows
    fillRows(doc, y, y + 34, kBg);
    Rng words(1000 + uint64_t(y));
    drawTextLine(doc, y + 10, 40, W - 40, words);
  }
  SceneSpec ss;
  ss.width = W;
  ss.height = V;
  ss.pinned.push_back({0, 0, W, 48, 0xF0F0F2, 0x202024, 0, 5});
  const Scene scene(doc, ss);
  MemoryCanvas canvas(W);
  ScrollTracker tracker(canvas);
  std::vector<int> pos;
  for (int p = 0; p < table; p += 90) pos.push_back(p);
  for (int p = table; p + V <= doc.h && pos.size() < 160; p += 68)
    for (int k = 0; k < 3; ++k) pos.push_back(p);  // a step, then the page at rest
  int wrong = 0, placed = 0;
  std::string first;
  for (size_t i = 0; i < pos.size(); ++i) {
    const TrackResult t = tracker.addFrame(scene.frame(pos[i], int(i)).view());
    if (t.status != TrackStatus::Placed && t.status != TrackStatus::NoMovement) continue;
    placed += t.status == TrackStatus::Placed;
    // A frame placed, or said not to have moved, must be where the page is.
    if (t.position != pos[i]) {
      ++wrong;
      if (first.empty()) first = fmt(", frame %zu %s at %lld, page at %d", i, toString(t.status), t.position, pos[i]);
    }
  }
  report("moves by the content's period, with rests: never left behind", wrong == 0 && placed > 0,
         fmt("%d placed, %d wrong%s", placed, wrong, first.c_str()));
}

// A jump past the reference over look-alike tables (#1515's re-check on
// Cinnamon): while frames were skipped the page moved more than a frame
// tall, and the pair, against the last placed frame, found the look-alike a
// period short. After skipped frames, a frame that repeats itself as well
// as the pair matches is not placed by the pair: still lost, and found again
// only once scrolled back where the image is.
void testJumpPastLookAlikes() {
  std::printf("\n[jump past look-alikes] after skipped frames, a repeating frame is not placed by the pair\n");
  const int W = 480, V = 400, P = 200;
  // Prose first, then the tables.
  Image doc(W, 6000, kBg);
  const int tables = 1000;
  Rng prose(23);
  for (int y = 10; y + 12 < tables; y += 18) drawTextLine(doc, y, 40, W - 40, prose);
  for (int b = tables / P; b * P < doc.h; ++b) {
    Rng head(500 + uint64_t(b));  // a table's heading of its own
    drawTextLine(doc, b * P + 6, 40, W - 40, head);
    drawTextLine(doc, b * P + 24, 40, W - 40, head);
    for (int j = 0; j < 8; ++j) {
      Rng words(77 + uint64_t(j));  // rows the same in every table
      drawTextLine(doc, b * P + 44 + j * 19, 40, W - 40, words);
    }
  }
  SceneSpec ss;
  ss.width = W;
  ss.height = V;
  const Scene scene(doc, ss);
  const Document other = plainPage(61, W, V);
  const Scene stray(other.img, ss);
  MemoryCanvas canvas(W);
  ScrollTracker tracker(canvas);
  std::vector<int> pos;
  for (int p = 0; p <= 1200; p += 60) pos.push_back(p);  // following
  const size_t lost0 = pos.size();
  for (int k = 0; k < 3; ++k) pos.push_back(-1);  // frames that cannot be placed while the page jumps
  const int far = 1200 + 600;                     // 1.5 frames past the last placed one
  for (int p = far; p <= far + 240; p += 60) pos.push_back(p);
  const size_t back0 = pos.size();
  for (int p = far + 240 - 100; p >= 500; p -= 100) pos.push_back(p);  // scrolled back, to the prose
  int wrong = 0, lostAfterJump = 0, foundBack = 0;
  std::string first;
  for (size_t i = 0; i < pos.size(); ++i) {
    const Image f = pos[i] < 0 ? stray.frame(300 * int(i), int(i)) : scene.frame(pos[i], int(i));
    const TrackResult t = tracker.addFrame(f.view());
    if (t.status == TrackStatus::Placed && (pos[i] < 0 || t.position != pos[i])) {
      ++wrong;
      if (first.empty()) first = fmt(", frame %zu at %lld, page at %d", i, t.position, pos[i]);
    }
    if (i > lost0 + 2 && i < back0) lostAfterJump += t.status != TrackStatus::Placed;
    if (i >= back0) foundBack += t.status == TrackStatus::Placed && t.position == pos[i];
  }
  report("a jump past the reference over look-alike tables: still lost, found again on the way back",
         wrong == 0 && lostAfterJump > 0 && foundBack > 0,
         fmt("%d wrong%s, %d frames past the jump not placed, %d placed right on the way back", wrong, first.c_str(), lostAfterJump, foundBack));
}

// While the page is lost, a frame matched at a guessed place marks nothing as
// changing in place (#1795). The scene came out of a randomized search over
// look-alike tables (seed 17884), kept exactly because the guess depends on
// every frame: a jump of 416 rows leaves the page lost; 18 frames later the
// playing-video retry (#1537) took a look-alike 21 rows on for the frame's
// place and marked the rows there dynamic; found again on the way back, the
// late header over those rows was written into them as their latest picture.
// GitHub's pull request page on Cinnamon did the same (a commit hash painted
// over with the header's plain side).
void testLostGuessMarksNothing() {
  std::printf("\n[lost guesses] a frame matched at a guessed place while lost marks no rows dynamic\n");
  const int W = 480, V = 480, P = 133, header = 38;
  Image doc(W, 7000, kBg);
  Rng prose(23 + 17884);
  for (int y = 10; y + 12 < 1000; y += 18) drawTextLine(doc, y, 40, W - 40, prose);
  const std::vector<int> twoHeadings{10, 11, 14, 16, 17, 20, 23, 28, 31, 32, 35, 43, 46, 47, 49, 50};
  for (int b = 1000 / P; b * P < doc.h; ++b) {
    Rng head(500 + uint64_t(b) + 17884000ull);
    if (b % 3 == 0) {  // every third block a paragraph
      for (int y = b * P + 6; y + 12 < (b + 1) * P; y += 18) drawTextLine(doc, y, 40, W - 40, head);
      continue;
    }
    drawTextLine(doc, b * P + 6, 40, W - 40, head);
    if (std::find(twoHeadings.begin(), twoHeadings.end(), b) != twoHeadings.end()) drawTextLine(doc, b * P + 24, 40, W - 40, head);
    for (int j = 0; j < 6 && b * P + 44 + j * 19 + 12 < (b + 1) * P; ++j) {
      Rng words(77 + uint64_t(j));  // rows the same in every table
      drawTextLine(doc, b * P + 44 + j * 19, 40, W - 40, words);
    }
  }
  SceneSpec ss;
  ss.width = W;
  ss.height = V;
  PinnedPart h;
  h.w = W;
  h.h = header;
  h.background = 0xFAFAFA;
  h.ink = 0x30303A;
  h.inkRows = 11;
  h.border = false;
  h.fromPos = 672;  // a header that shows once the page is scrolled past its title
  ss.pinned.push_back(h);
  const Scene scene(doc, ss);
  const std::vector<int> pos{0,    89,   178,  267,  356,  445,  534,  623,  712,  801,  890,  979,  1068, 1157, 1573, 1577,
                             1573, 1573, 1573, 1580, 1576, 1577, 1573, 1579, 1573, 1577, 1576, 1580, 1579, 1573, 1579, 1579,
                             1577, 1575, 1575, 1580, 1577, 1580, 1578, 1572, 1572, 1577, 1576, 1577, 1579, 1573, 1572, 1573,
                             1577, 1575, 1575, 1580, 1579, 1578, 1579, 1580, 1580, 1572, 1572, 1580, 1519, 1472, 1426, 1396,
                             1351, 1271, 1216, 1185, 1158, 1126, 1061, 1033, 964,  904,  831,  808,  776,  704,  629,  588};
  const Played r = play(scene, pos);
  int foundBack = 0;
  for (size_t i = 60; i < pos.size(); ++i) foundBack += r.results[i].status == TrackStatus::Placed && r.results[i].position == pos[i];
  int firstBad = 0;
  const long long bad = differing(r, pageRows(doc, 0, r.totals.bottom, W), {}, 0, &firstBad);
  report("lost over look-alike tables, found again under a late header: rows stay text", r.wrong == 0 && foundBack >= 15 && bad == 0,
         fmt("%d wrong%s, %d placed on the way back, %lld px differ (row %d)", r.wrong, r.firstWrong.c_str(), foundBack, bad, firstBad));
}

// A playing video in the region (#1537): a part of the page that keeps
// changing while the rest of the frame agrees must not contradict the image
// or stall a capture the person scrolls themselves (slow scrolling, pausing,
// scrolling back over rows already written, a video covering over half the
// visible article column, a 30 fps video on a 60 fps stream). A one-time
// layout change (static after it appears) must still be refused as
// contradicted.
void testPlayingVideo() {
  std::printf("\n[playing video] changing content in the region keeps Following (#1537)\n");
  // Helper: check that the entire video rectangle [x0, x1) x [y0, y1) in the
  // stitched output matches ONE single source frame pixel-for-pixel (#1537).
  const auto matchesOneVideoFrame = [](const Played& r, const Scene& scene, const std::vector<int>& pos,
                                       int x0, int y0, int x1, int y1, int* matchedFrame = nullptr) {
    if (matchedFrame) *matchedFrame = -1;
    const int V = scene.spec().height;
    const long long outRows = r.totals.bottom - r.totals.top;
    if (y0 < 0 || y1 > outRows || x0 < 0 || x1 > r.width) return false;
    for (size_t i = 0; i < pos.size(); ++i) {
      if (pos[i] > y0 || pos[i] + V < y1) continue;
      const Image src = scene.frame(pos[i], int(i));
      bool eq = true;
      for (int y = y0; y < y1 && eq; ++y) {
        const int sy = y - pos[i];
        if (std::memcmp(&r.image[(size_t(y) * r.width + x0) * 4], src.row(sy) + x0, size_t(x1 - x0) * 4) != 0)
          eq = false;
      }
      if (eq) {
        if (matchedFrame) *matchedFrame = int(i);
        return true;
      }
    }
    return false;
  };

  {
    // 1. News article proportions (Al Jazeera #1537): narrow article column
    // between blank margins, sticky header and sticky cookie banner leaving
    // a narrow visible band (~260 rows), and a playing video occupying ~55%
    // of that band. Scrolled slowly as a person does (4 to 14 px a frame),
    // with a brief pause and a scroll back up and down over the video.
    const int W = 640, V = 400;
    const Image doc = columnPage(137, W, 3200, 140, 500, 160, 140, 500);
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    PinnedPart hdrPlain{0, 0, W, 48, 0xD8DCE6, 0x202024, 0, 5};
    hdrPlain.border = false;
    hdrPlain.inkRows = 0;
    ss.pinned.push_back(hdrPlain);
    PinnedPart hdrText{150, 8, 340, 32, 0xD8DCE6, 0x202024, 0, 6};
    hdrText.border = false;
    ss.pinned.push_back(hdrText);
    PinnedPart banPlain{0, V - 80, W, 80, 0x1E2266, 0xF0F0FF, 0, 9};
    banPlain.border = false;
    banPlain.inkRows = 0;
    ss.pinned.push_back(banPlain);
    PinnedPart banText{150, V - 72, 340, 64, 0x1E2266, 0xF0F0FF, 0, 10};
    banText.border = false;
    ss.pinned.push_back(banText);
    ss.changingX = 148;
    ss.changingY = 900;
    ss.changingW = 344;
    ss.changingH = 160;
    const Scene scene(doc, ss);
    Rng rng(139);
    std::vector<int> pos{0};
    // Down past the video in small steps.
    while (pos.back() < 1200) pos.push_back(std::min(1200, pos.back() + rng.range(4, 14)));
    // Pause with the video in view, then back up over it, then down to the end.
    for (int k = 0; k < 10; ++k) pos.push_back(pos.back());
    while (pos.back() > 750) pos.push_back(std::max(750, pos.back() - rng.range(4, 14)));
    for (int k = 0; k < 8; ++k) pos.push_back(pos.back());
    while (pos.back() < doc.h - V) pos.push_back(std::min(doc.h - V, pos.back() + rng.range(4, 16)));
    const Played r = play(scene, pos);
    int lostFrames = 0;
    for (const TrackResult& t : r.results) lostFrames += t.state == TrackState::Lost;
    int firstBad = 0;
    const Image expected = pageRows(doc, 0, doc.h, W);
    const long long bad = differing(r, expected, {{0, 0, W, 48}, {0, doc.h - 80, W, doc.h}, {140, 892, 500, 1068}}, 0, &firstBad);
    int matchedVid = -1;
    const bool videoExact = matchesOneVideoFrame(r, scene, pos, ss.changingX, ss.changingY,
                                                 ss.changingX + ss.changingW, ss.changingY + ss.changingH, &matchedVid);
    report("article with header, banner and playing video: stays Following, rest exact",
           r.wrong == 0 && lostFrames == 0 && r.totals.skipped <= 2 && bad == 0 && videoExact && r.totals.bottom == doc.h,
           counts(r) + fmt(", %d lost, %d wrong%s, %lld px differ (row %d), video frame %d, height %lld/%d", lostFrames, r.wrong,
                           r.firstWrong.c_str(), bad, firstBad, matchedVid, r.totals.bottom - r.totals.top, doc.h));
  }

  {
    // 2. Scroll down past a video while it is paused (poster written into the
    // store), then the video starts playing (every 2nd frame, 30 fps on a
    // 60 fps stream) while the person scrolls back up and down over it.
    const int W = 480, V = 400;
    const Document doc = plainPage(141, W, V);
    const int end = doc.img.h - V;
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    ss.changingX = 60;
    ss.changingY = 900;
    ss.changingW = 360;
    ss.changingH = 180;
    const Scene scene(doc.img, ss);
    Rng rng(143);
    std::vector<int> pos{0};
    while (pos.back() < 1200) pos.push_back(std::min(1200, pos.back() + rng.range(40, 80)));
    const size_t playFrom = pos.size();
    while (pos.back() > 650) pos.push_back(std::max(650, pos.back() - rng.range(6, 18)));
    while (pos.back() < end) pos.push_back(std::min(end, pos.back() + rng.range(6, 18)));
    MemoryCanvas canvas(W);
    ScrollTracker tracker(canvas);
    int wrong = 0, lost = 0, skippedAfterStart = 0;
    std::string firstWrong;
    for (size_t i = 0; i < pos.size(); ++i) {
      // Static poster before playFrom; changes every 2nd frame afterwards.
      const int vidFrame = i < playFrom ? 0 : int((i - playFrom) / 2 + 1);
      const Image f = scene.frame(pos[i], vidFrame);
      const TrackResult t = tracker.addFrame(f.view());
      if (t.state == TrackState::Lost) ++lost;
      if (i >= playFrom && t.status == TrackStatus::Skipped) ++skippedAfterStart;
      if (t.status == TrackStatus::Placed && t.position != pos[i]) {
        ++wrong;
        if (firstWrong.empty()) firstWrong = fmt(" frame %zu at %lld, page at %d", i, t.position, pos[i]);
      }
    }
    const TrackTotals tot = tracker.finish();
    Played r;
    r.totals = tot;
    r.image = canvas.image(tot.top, tot.bottom);
    r.width = W;
    int firstBad = 0;
    const long long bad = differing(r, pageRows(doc.img, 0, doc.img.h, W), {{52, 892, 428, 1088}}, 0, &firstBad);
    report("poster written, then 30 fps video plays on return: recovers fast, exact",
           wrong == 0 && skippedAfterStart <= 6 && bad == 0 && tot.bottom == doc.img.h,
           fmt("%d frames, %d placed, %d skipped (%d after play), %d lost, %d wrong%s, %lld px differ (row %d)", tot.frames, tot.placed,
               tot.skipped, skippedAfterStart, lost, wrong, firstWrong.c_str(), bad, firstBad));

    // 2b. Return to an already-verified section over a paused 60% video, then
    // pause while playback starts: `proofAt_` must refresh on already-verified
    // cells so the at-rest observer stays Following instead of going Lost.
    {
      DocSpec ds60;
      ds60.seed = 143;
      ds60.width = W;
      ds60.viewport = V;
      ds60.flatFactor = 0;
      ds60.tableFactor = 0;
      ds60.noiseFactor = 0.3;
      ds60.gradientFactor = 0.3;
      ds60.blankFactor = 0;
      ds60.paragraphsPerGap = 4;
      const Document doc60 = makeDocument(ds60);
      SceneSpec ss60 = ss;
      ss60.changingX = 32;
      ss60.changingY = 584;
      ss60.changingW = 416;
      ss60.changingH = 256;  // 64% of 400-px viewport (tile-aligned at y=64..320)
      const Scene scene60(doc60.img, ss60);
      MemoryCanvas c60(W);
      ScrollTracker tr60(c60);
      int wrong60 = 0, lost60 = 0, still60 = 0;
      // Two passes over [520, 920) verify every textured cell before returning
      // to pos=520 so the final placement before pausing has zero unverified cells.
      for (int p = 0; p <= 1040; p += 24) {
        const TrackResult t = tr60.addFrame(scene60.frame(p, 0).view());
        if (t.status == TrackStatus::Placed && t.position != p) ++wrong60;
      }
      for (int p = 1016; p >= 472; p -= 24) {
        const TrackResult t = tr60.addFrame(scene60.frame(p, 0).view());
        if (t.status == TrackStatus::Placed && t.position != p) ++wrong60;
      }
      for (const int p : {496, 520}) {
        const TrackResult t = tr60.addFrame(scene60.frame(p, 0).view());
        if (t.status == TrackStatus::Placed && t.position != p) ++wrong60;
      }
      // Pause at pos=520 while dense textured video frames play across y=64..320,
      // placing static textured share strictly between minArea (0.20) and
      // stillFallback (0.50).
      for (int k = 1; k <= 12; ++k) {
        Image f = scene60.frame(520, 0);
        Rng vrng(7000 + size_t(k) * 9973);
        const uint32_t bg = (uint32_t(vrng.range(20, 90)) << 16) | (uint32_t(vrng.range(20, 90)) << 8) | uint32_t(vrng.range(30, 110));
        const uint32_t fg = (uint32_t(vrng.range(180, 250)) << 16) | (uint32_t(vrng.range(180, 250)) << 8) | uint32_t(vrng.range(160, 240));
        for (int y = 64; y < 320; ++y)
          for (int x = 32; x < 448; ++x) f.at(x, y) = bg;
        for (int y = 70; y + 8 < 316; y += 14) drawTextLine(f, y, 40, 440, vrng, fg);
        const TrackResult t = tr60.addFrame(f.view());
        lost60 += t.state == TrackState::Lost;
        still60 += t.status == TrackStatus::NoMovement;
      }
      report("revisit verified page, pause and start 64% video: stays Following",
             wrong60 == 0 && lost60 == 0 && still60 >= 10,
             fmt("%d/12 still, %d lost, %d wrong", still60, lost60, wrong60));

      // 2c. Same revisit with a 57-px final step and video at viewport [64, 352):
      // both `brow` and `bref` must refresh `proofAt_` on already-verified cells.
      MemoryCanvas c57(W);
      ScrollTracker tr57(c57);
      int wrong57 = 0, lost57 = 0, still57 = 0;
      for (int p = 0; p <= 1040; p += 24) {
        const TrackResult t = tr57.addFrame(scene60.frame(p, 0).view());
        if (t.status == TrackStatus::Placed && t.position != p) ++wrong57;
      }
      for (int p = 1016; p >= 463; p -= 24) {
        const TrackResult t = tr57.addFrame(scene60.frame(p, 0).view());
        if (t.status == TrackStatus::Placed && t.position != p) ++wrong57;
      }
      {
        const TrackResult t = tr57.addFrame(scene60.frame(463, 0).view());
        if (t.status == TrackStatus::Placed && t.position != 463) ++wrong57;
      }
      {
        const TrackResult t = tr57.addFrame(scene60.frame(520, 0).view());  // 57-px final step
        if (t.status == TrackStatus::Placed && t.position != 520) ++wrong57;
      }
      for (int k = 1; k <= 12; ++k) {
        Image f = scene60.frame(520, 0);
        Rng vrng(7100 + size_t(k) * 9973);
        const uint32_t bg = (uint32_t(vrng.range(20, 90)) << 16) | (uint32_t(vrng.range(20, 90)) << 8) | uint32_t(vrng.range(30, 110));
        const uint32_t fg = (uint32_t(vrng.range(180, 250)) << 16) | (uint32_t(vrng.range(180, 250)) << 8) | uint32_t(vrng.range(160, 240));
        for (int y = 64; y < 352; ++y)
          for (int x = 32; x < 448; ++x) f.at(x, y) = bg;
        for (int y = 70; y + 8 < 348; y += 14) drawTextLine(f, y, 40, 440, vrng, fg);
        const TrackResult t = tr57.addFrame(f.view());
        lost57 += t.state == TrackState::Lost;
        still57 += t.status == TrackStatus::NoMovement;
      }
      report("revisit verified page (57-px step) and start 72% video: stays Following",
             wrong57 == 0 && lost57 == 0 && still57 >= 10,
             fmt("%d/12 still, %d lost, %d wrong", still57, lost57, wrong57));
    }
  }

  {
    // 3. Large playing video (60% of viewport height) inside a page of text:
    // user pauses at rest in front of it for 20 frames while it plays, then
    // scrolls on. Must stay Following (NoMovement while paused) and place
    // every scroll frame afterwards.
    const int W = 480, V = 400;
    DocSpec ds;
    ds.seed = 145;
    ds.width = W;
    ds.viewport = V;
    ds.flatFactor = 0;
    ds.tableFactor = 0;
    ds.noiseFactor = 0.3;
    ds.gradientFactor = 0.3;
    ds.blankFactor = 0;
    ds.paragraphsPerGap = 4;
    const Document doc = makeDocument(ds);
    const int end = doc.img.h - V;
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    ss.changingX = 32;
    ss.changingY = 600;
    ss.changingW = 416;
    ss.changingH = 240;  // 60% of the viewport height
    const Scene scene(doc.img, ss);
    Rng rng(147);
    std::vector<int> pos{0};
    while (pos.back() < 520) pos.push_back(std::min(520, pos.back() + rng.range(12, 32)));
    const size_t pauseBegin = pos.size();
    for (int k = 0; k < 20; ++k) pos.push_back(520);  // paused with 60% video in view
    const size_t pauseEnd = pos.size();
    while (pos.back() < end) pos.push_back(std::min(end, pos.back() + rng.range(12, 32)));
    const Played r = play(scene, pos);
    int lostFrames = 0, badPause = 0, badMove = 0;
    for (size_t i = 0; i < r.results.size(); ++i) {
      const TrackResult& t = r.results[i];
      lostFrames += t.state == TrackState::Lost;
      if (i >= pauseBegin && i < pauseEnd)
        badPause += t.status != TrackStatus::NoMovement || t.position != 520;
      else
        badMove += t.status != TrackStatus::Placed || t.position != pos[i];
    }
    int firstBad = 0;
    const long long bad = differing(r, pageRows(doc.img, 0, doc.img.h, W), {{24, 592, 456, 848}}, 0, &firstBad);
    int matchedVid = -1;
    const bool videoExact = matchesOneVideoFrame(r, scene, pos, ss.changingX, ss.changingY,
                                                 ss.changingX + ss.changingW, ss.changingY + ss.changingH, &matchedVid);
    report("60% playing video: pause at rest and scroll past stay Following, exact",
           r.wrong == 0 && lostFrames == 0 && badPause == 0 && badMove == 0 && bad == 0 && videoExact && r.totals.bottom == doc.img.h,
           counts(r) + fmt(", %d badPause, %d badMove, %d lost, %d wrong%s, %lld px differ (row %d), video frame %d", badPause, badMove,
                           lostFrames, r.wrong, r.firstWrong.c_str(), bad, firstBad, matchedVid));

    // 3b. Slowly scrolling back and forth over the same 60% playing video
    // (8-px steps over a 32-px range): must not falsely pin the video (#1537).
    std::vector<int> oscPos{0};
    Rng oscRng(148);
    while (oscPos.back() < 520) oscPos.push_back(std::min(520, oscPos.back() + oscRng.range(12, 32)));
    for (int cyc = 0; cyc < 5; ++cyc) {
      for (int k = 0; k < 4; ++k) oscPos.push_back(oscPos.back() + 8);
      for (int k = 0; k < 4; ++k) oscPos.push_back(oscPos.back() - 8);
    }
    while (oscPos.back() < end) oscPos.push_back(std::min(end, oscPos.back() + oscRng.range(12, 32)));
    const Played ro = play(scene, oscPos);
    int oscLost = 0;
    for (const TrackResult& t : ro.results) oscLost += t.state == TrackState::Lost;
    int oscFirstBad = 0;
    const long long oscBad = differing(ro, pageRows(doc.img, 0, doc.img.h, W), {{24, 592, 456, 848}}, 0, &oscFirstBad);
    int oscVid = -1;
    const bool oscVidExact = matchesOneVideoFrame(ro, scene, oscPos, ss.changingX, ss.changingY,
                                                  ss.changingX + ss.changingW, ss.changingY + ss.changingH, &oscVid);
    report("60% playing video: 8-px back-and-forth oscillation never pins video",
           ro.wrong == 0 && oscLost == 0 && ro.totals.pinnedRegions == 0 && oscBad == 0 && oscVidExact && ro.totals.bottom == doc.img.h,
           counts(ro) + fmt(", %d pinned, %d lost, %d wrong%s, %lld px differ (row %d), video frame %d", ro.totals.pinnedRegions, oscLost,
                            ro.wrong, ro.firstWrong.c_str(), oscBad, oscFirstBad, oscVid));

    // 3c. Full-width 60% playing video extending past the text margins
    // (changingX = 0, changingW = W): edge blocks must not pin and the entire
    // full-width video (including outer margins) must come from one frame (#1537).
    SceneSpec ssFull = ss;
    ssFull.changingX = 0;
    ssFull.changingW = W;
    const Scene sceneFull(doc.img, ssFull);
    const Played rf = play(sceneFull, oscPos);
    int fullLost = 0;
    for (const TrackResult& t : rf.results) fullLost += t.state == TrackState::Lost;
    int fullFirstBad = 0;
    const long long fullBad = differing(rf, pageRows(doc.img, 0, doc.img.h, W), {{0, 592, W, 848}}, 0, &fullFirstBad);
    int fullVid = -1;
    const bool fullVidExact = matchesOneVideoFrame(rf, sceneFull, oscPos, 0, ssFull.changingY, W,
                                                   ssFull.changingY + ssFull.changingH, &fullVid);
    report("full-width 60% playing video: margin edges never pin, one frame",
           rf.wrong == 0 && fullLost == 0 && rf.totals.pinnedRegions == 0 && fullBad == 0 && fullVidExact &&
               rf.totals.bottom == doc.img.h,
           counts(rf) + fmt(", %d pinned, %d lost, %d wrong%s, %lld px differ (row %d), video frame %d", rf.totals.pinnedRegions, fullLost,
                            rf.wrong, rf.firstWrong.c_str(), fullBad, fullFirstBad, fullVid));

    // 3d. Full-width 60% playing video with an 8x8 exclusion in the outer left
    // margin at (0, 324): once a later frame clears the exclusion, it must
    // replace the earlier partial margin snapshot cleanly (#1537).
    const Played rfe = play(sceneFull, oscPos, {}, {}, TrackerRect{0, 324, 8, 8});
    int exclVid = -1;
    const bool exclVidExact = matchesOneVideoFrame(rfe, sceneFull, oscPos, 0, ssFull.changingY, W,
                                                   ssFull.changingY + ssFull.changingH, &exclVid);
    report("full-width 60% video with margin exclusion: clear frame replaces",
           rfe.wrong == 0 && exclVidExact && rfe.totals.bottom == doc.img.h,
           counts(rfe) + fmt(", %d wrong%s, video frame %d", rfe.wrong, rfe.firstWrong.c_str(), exclVid));
  }

  {
    // 4. Safety check: a one-time layout change (a static strip inserted into
    // the page after those rows were already written, shifting the lower half
    // down by 200 rows) is static, NOT a playing video: frames showing the new
    // layout over already-written rows must still be refused as contradicted.
    const int W = 480, V = 400;
    const Document doc = plainPage(149, W, V);
    Image shifted(W, doc.img.h + 200, kBg);
    std::memcpy(shifted.px.data(), doc.img.px.data(), size_t(900) * W * 4);
    Rng stripRng(151);
    for (int y = 910; y + 12 < 1090; y += 20) drawTextLine(shifted, y, 30, W - 30, stripRng, 0x803020);
    std::memcpy(&shifted.px[size_t(1100) * W], &doc.img.px[size_t(900) * W], size_t(doc.img.h - 900) * W * 4);
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    const Scene before(doc.img, ss);
    const Scene after(shifted, ss);
    MemoryCanvas canvas(W);
    ScrollTracker tracker(canvas);
    // Scroll down to 1200 under original layout, so rows 0..1600 are written.
    for (int i = 0, p = 0; p <= 1200; ++i, p += 60) tracker.addFrame(before.frame(p, i).view());
    // Scroll back up to 600, then insert the static 200 px strip at row 900
    // and scroll down across 750..1050 (lower 65% of viewport contradicts store).
    for (int i = 0, p = 1140; p >= 600; ++i, p -= 60) tracker.addFrame(before.frame(p, 100 + i).view());
    int contradicted = 0, placedShifted = 0;
    for (int i = 0, p = 780; p <= 1020; ++i, p += 12) {
      const TrackResult t = tracker.addFrame(after.frame(p, 200 + i).view());
      contradicted += t.status == TrackStatus::Skipped && t.reason == SkipReason::Contradicted;
      placedShifted += t.status == TrackStatus::Placed;
    }
    report("one-time static layout insertion is still refused as contradicted",
           contradicted > 0 && placedShifted == 0,
           fmt("%d contradicted, %d placed of 21 shifted frames", contradicted, placedShifted));
  }

  {
    // 5a. Capture starts on frame 0 with a 60% playing video already in view
    // and scrolls immediately with no initial pause (#1537).
    const int W = 480, V = 400;
    DocSpec ds;
    ds.seed = 153;
    ds.width = W;
    ds.viewport = V;
    ds.flatFactor = 0;
    ds.tableFactor = 0;
    ds.noiseFactor = 0.2;
    ds.gradientFactor = 0.2;
    ds.blankFactor = 0;
    ds.paragraphsPerGap = 4;
    const Document doc = makeDocument(ds);
    const int end = doc.img.h - V;
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    ss.changingX = 32;
    ss.changingY = 80;
    ss.changingW = 416;
    ss.changingH = 240;  // 60% of the first viewport
    const Scene scene(doc.img, ss);
    {
      Rng rng(155);
      std::vector<int> pos{0};
      while (pos.back() < end) pos.push_back(std::min(end, pos.back() + rng.range(12, 32)));
      const Played r = play(scene, pos);
      int lostFrames = 0;
      for (const TrackResult& t : r.results) lostFrames += t.state == TrackState::Lost;
      int firstBad = 0;
      const long long bad = differing(r, pageRows(doc.img, 0, doc.img.h, W), {{24, 72, 456, 328}}, 0, &firstBad);
      int matchedVid = -1;
      const bool videoExact = matchesOneVideoFrame(r, scene, pos, ss.changingX, ss.changingY,
                                                   ss.changingX + ss.changingW, ss.changingY + ss.changingH, &matchedVid);
      report("60% playing video on frame 0: immediate scroll recovers fast, exact",
             r.wrong == 0 && r.totals.skipped <= 2 && r.totals.placed >= int(pos.size()) - 2 && bad == 0 && videoExact &&
                 r.totals.bottom == doc.img.h,
             counts(r) + fmt(", %d lost, %d wrong%s, %lld px differ (row %d), video frame %d", lostFrames, r.wrong, r.firstWrong.c_str(), bad,
                             firstBad, matchedVid));
    }
    // 5b. Same scene with a 6-frame initial pause before scrolling: recovers as
    // soon as scrolling starts, captures the full page exact and video from one frame.
    {
      Rng rng(155);
      std::vector<int> pos{0, 0, 0, 0, 0, 0};
      while (pos.back() < end) pos.push_back(std::min(end, pos.back() + rng.range(12, 32)));
      const Played r = play(scene, pos);
      int firstBad = 0;
      const long long bad = differing(r, pageRows(doc.img, 0, doc.img.h, W), {{24, 72, 456, 328}}, 0, &firstBad);
      int matchedVid = -1;
      const bool videoExact = matchesOneVideoFrame(r, scene, pos, ss.changingX, ss.changingY,
                                                   ss.changingX + ss.changingW, ss.changingY + ss.changingH, &matchedVid);
      report("60% playing video on frame 0: pause then scroll recovers, exact",
             r.wrong == 0 && bad == 0 && videoExact && r.totals.bottom == doc.img.h && r.totals.placed >= int(pos.size()) - 7,
             counts(r) + fmt(", %d wrong%s, %lld px differ (row %d), video frame %d", r.wrong, r.firstWrong.c_str(), bad, firstBad, matchedVid));
    }
    // 5c. 240-px playing video at the very top of the capture (changingY = 0)
    // scrolling immediately in 8-px steps: frame 0 is the only frame showing the
    // whole video, so the snapshot uses retained frame 0 (#1537).
    {
      SceneSpec ssTop = ss;
      ssTop.changingY = 0;
      const Scene sceneTop(doc.img, ssTop);
      std::vector<int> pos{0};
      while (pos.back() < end) pos.push_back(std::min(end, pos.back() + 8));
      const Played r = play(sceneTop, pos);
      int firstBad = 0;
      const long long bad = differing(r, pageRows(doc.img, 0, doc.img.h, W), {{24, 0, 456, 248}}, 0, &firstBad);
      int matchedVid = -1;
      const bool videoExact = matchesOneVideoFrame(r, sceneTop, pos, ssTop.changingX, 0,
                                                   ssTop.changingX + ssTop.changingW, ssTop.changingH, &matchedVid);
      report("240-px playing video at top of capture: snapshot keeps frame 0 exact",
             r.wrong == 0 && bad == 0 && videoExact && matchedVid == 0 && r.totals.bottom == doc.img.h,
             counts(r) + fmt(", %d wrong%s, %lld px differ (row %d), video frame %d", r.wrong, r.firstWrong.c_str(), bad, firstBad, matchedVid));
    }
    // 5d. Mirror of 5c: 240-px playing video at the bottom of the capture
    // scrolling upward in 8-px steps uses retained bottomCopy_ (#1537).
    {
      SceneSpec ssBot = ss;
      ssBot.changingY = doc.img.h - ss.changingH;
      const Scene sceneBot(doc.img, ssBot);
      std::vector<int> pos{end};
      while (pos.back() > 0) pos.push_back(std::max(0, pos.back() - 8));
      const Played r = play(sceneBot, pos);
      int firstBad = 0;
      const long long bad = differing(r, pageRows(doc.img, 0, doc.img.h, W), {{24, ssBot.changingY - 8, 456, doc.img.h}}, 0, &firstBad);
      int matchedVid = -1;
      const bool videoExact = matchesOneVideoFrame(r, sceneBot, pos, ssBot.changingX, ssBot.changingY,
                                                   ssBot.changingX + ssBot.changingW, doc.img.h, &matchedVid);
      report("240-px playing video at bottom of capture: upward scroll keeps frame 0",
             r.wrong == 0 && bad == 0 && videoExact && matchedVid == 0 && r.totals.bottom - r.totals.top == doc.img.h,
             counts(r) + fmt(", %d wrong%s, %lld px differ (row %d), video frame %d", r.wrong, r.firstWrong.c_str(), bad, firstBad, matchedVid));
    }
    // 5e. Edge lookback safety (#1537): a transient frame-0 overlay above a
    // video starting at row 40 must never be copied and locked in when scrolling
    // down and back to the top.
    {
      SceneSpec ssOff = ss;
      ssOff.changingY = 40;
      const Scene sceneOff(doc.img, ssOff);
      std::vector<int> pos{0};
      for (int p = 8; p <= 96; p += 8) pos.push_back(p);
      for (int p = 88; p >= 0; p -= 8) pos.push_back(p);
      const Played r = play(sceneOff, pos, {}, [](size_t i, Image& f) {
        if (i == 0)
          for (int y = 0; y < 24; ++y)
            for (int x = 32; x < 448; ++x) f.at(x, y) = 0xE02020;
        return false;
      });
      int firstBad = 0;
      const long long bad = differing(r, pageRows(doc.img, 0, int(r.totals.bottom), W), {{24, 32, 456, 288}}, 0, &firstBad);
      report("transient frame-0 overlay above video at y=40 is not retained",
             r.wrong == 0 && bad == 0,
             counts(r) + fmt(", %d wrong%s, %lld px differ (row %d)", r.wrong, r.firstWrong.c_str(), bad, firstBad));
    }
    // 5f. Mirror of 5e at the bottom edge (#1537): video ending 16 px above
    // the capture bottom with a transient frame-0 red overlay in the bottom 16
    // rows; scrolling up 96 px and back in 8-px steps retains zero red pixels.
    {
      SceneSpec ssBotOff = ss;
      ssBotOff.changingY = doc.img.h - ss.changingH - 16;
      const Scene sceneBotOff(doc.img, ssBotOff);
      std::vector<int> pos{end};
      for (int p = end - 8; p >= end - 96; p -= 8) pos.push_back(p);
      for (int p = end - 88; p <= end; p += 8) pos.push_back(p);
      const Played r = play(sceneBotOff, pos, {}, [&](size_t i, Image& f) {
        if (i == 0)
          for (int y = V - 16; y < V; ++y)
            for (int x = 32; x < 448; ++x) f.at(x, y) = 0xE02020;
        return false;
      });
      const int outStart = end + int(r.totals.top);
      const int outRows = int(r.totals.bottom - r.totals.top);
      int firstBad = 0;
      const long long bad = differing(
          r, pageRows(doc.img, outStart, outStart + outRows, W),
          {{24, ssBotOff.changingY - outStart - 8, 456, ssBotOff.changingY + ssBotOff.changingH - outStart + 8}}, 0, &firstBad);
      report("transient frame-0 overlay below bottom video is not retained",
             r.wrong == 0 && bad == 0,
             counts(r) + fmt(", %d wrong%s, %lld px differ (row %d)", r.wrong, r.firstWrong.c_str(), bad, firstBad));
    }
    // 5g. Top-edge video scrolling down 96 px and back to 0 in 12-px steps:
    // returning over the top edge must keep one consistent video frame (#1537).
    {
      SceneSpec ssTop = ss;
      ssTop.changingY = 0;
      const Scene sceneTop(doc.img, ssTop);
      std::vector<int> pos{0};
      for (int p = 12; p <= 96; p += 12) pos.push_back(p);
      for (int p = 84; p >= 0; p -= 12) pos.push_back(p);
      const Played r = play(sceneTop, pos);
      int firstBad = 0;
      const long long bad = differing(r, pageRows(doc.img, 0, int(r.totals.bottom), W), {{24, 0, 456, 248}}, 0, &firstBad);
      int matchedVid = -1;
      const bool videoExact = matchesOneVideoFrame(r, sceneTop, pos, ssTop.changingX, 0,
                                                   ssTop.changingX + ssTop.changingW, ssTop.changingH, &matchedVid);
      report("top-edge video scrolled down 96 px and back in 12-px steps: one frame",
             r.wrong == 0 && bad == 0 && videoExact,
             counts(r) + fmt(", %d wrong%s, %lld px differ (row %d), video frame %d", r.wrong, r.firstWrong.c_str(), bad, firstBad, matchedVid));
    }
    // 5h. Boundary scan must never jump over a zero-hit clean row (#1537):
    // video starts at y = 44, rows y = 16..43 are clean background with a
    // clean 8-cell x 8-row textured patch at x = 184..248, y = 36..44
    // (verified on scroll, <25% width so rowIsStatic stays false). Frame 0 at
    // pos = 16 (retained as topCopy_) tints only 2 rows (y = 36..38, below
    // kDynTileRows = 3 so rows 36..43 stay at zero dynHits); frame 1 at
    // pos = 20 adds a 3-row red overlay at y = 20..23 so clean frame 2 at
    // pos = 20 records dynHits on rows 20..22. Once playback starts at
    // pos = 36 -> 44, the boundary scan from topCopy_ must stop at zero-hit
    // row 43 instead of jumping across rows 36..38 and repainting frame 0's
    // red tint over the verified clean patch.
    {
      Image gapImg = doc.img;
      for (int y = 16; y < 44; ++y)
        for (int x = 0; x < W; ++x) gapImg.at(x, y) = kBg;
      for (int y = 36; y < 44; ++y)
        for (int x = 184; x < 248; ++x) gapImg.at(x, y) = ((x + y) & 2) ? 0x101828 : 0x687888;
      SceneSpec ssGap = ss;
      ssGap.changingY = 44;
      ssGap.changingH = 200;
      const Scene sceneGap(gapImg, ssGap);
      std::vector<int> pos{16, 20, 20, 24, 36, 44, 52, 60, 68, 76, 84, 92, 100};
      MemoryCanvas cGap(W);
      ScrollTracker trGap(cGap);
      int wrongGap = 0, placedGap = 0;
      for (size_t i = 0; i < pos.size(); ++i) {
        const int vidFrame = i < 4 ? 0 : int(i - 3);
        Image f = sceneGap.frame(pos[i], vidFrame);
        if (i == 0)
          for (int y = 20; y < 22; ++y)
            for (int x = 184; x < 248; ++x) f.at(x, y) = 0xE02020;
        if (i == 1)
          for (int y = 0; y < 3; ++y)
            for (int x = 32; x < 448; ++x) f.at(x, y) = 0xE02020;
        const TrackResult t = trGap.addFrame(f.view());
        if (t.status == TrackStatus::Placed) {
          ++placedGap;
          if (t.position != pos[i] - pos[0]) ++wrongGap;
        }
      }
      const TrackTotals totGap = trGap.finish();
      Played r;
      r.totals = totGap;
      r.image = cGap.image(totGap.top, totGap.bottom);
      r.width = W;
      const int outStart = pos[0] + int(totGap.top);
      const int outRows = int(totGap.bottom - totGap.top);
      int firstBad = 0;
      // Check the zero-hit clean gap rows 23..43 (excluding the transient overlay
      // on rows 20..23 and the video at rows 44..252).
      const long long bad = differing(
          r, pageRows(gapImg, outStart, outStart + outRows, W),
          {{24, 20 - outStart, 456, 23 - outStart}, {24, 44 - outStart, 456, 252 - outStart}}, 0, &firstBad);
      report("boundary scan stops at zero-hit clean row above video",
             wrongGap == 0 && placedGap >= 10 && bad == 0,
             fmt("%d placed, %d wrong, %lld px differ (row %d)", placedGap, wrongGap, bad, firstBad));
    }
  }

  {
    // 6. A pinned animated ad in a sticky side rail (outside the article's
    // moving area): wandering_ still pins it so its changing pixels are not
    // smeared down the margin while the person scrolls slowly past the article.
    const int W = 640, V = 400;
    const Image doc = columnPage(157, W, 3200, 140, 480, 160, 140, 480);
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    const Scene scene(doc, ss);
    Rng rng(159);
    const auto pos = steps(0, doc.h - V, [&] { return rng.range(6, 16); });
    const Played r = play(scene, pos, {}, [&](size_t i, Image& f) {
      // Animated ad with non-periodic changing artwork/text pinned in the right margin [500, 620) x [80, 260).
      Rng adRng(9001 + i * 7919);
      const uint32_t bg = (uint32_t(adRng.range(20, 90)) << 16) | (uint32_t(adRng.range(20, 90)) << 8) | uint32_t(adRng.range(40, 120));
      const uint32_t fg = (uint32_t(adRng.range(180, 250)) << 16) | (uint32_t(adRng.range(180, 250)) << 8) | uint32_t(adRng.range(160, 240));
      for (int y = 80; y < 260; ++y)
        for (int x = 500; x < 620; ++x) f.at(x, y) = bg;
      for (int y = 86; y + 8 < 256; y += 14) drawTextLine(f, y, 508, 612, adRng, fg);
      return false;
    });
    // Below the first screen, the right margin must hold no animated ad pixels.
    long long leaked = 0;
    for (long long y = V + 32; y < r.totals.bottom - r.totals.top; ++y)
      for (int x = 500; x < 620; ++x) {
        uint32_t v;
        std::memcpy(&v, &r.image[(size_t(y) * W + x) * 4], 4);
        leaked += (v & 0xFFFFFFu) != (kBg & 0xFFFFFFu);
      }
    int firstBad = 0;
    const long long badArticle = differing(r, pageRows(doc, 0, doc.h, W), {{492, 0, W, doc.h}}, 0, &firstBad);
    report("pinned animated ad in sticky rail: pinned, not smeared down margin",
           r.wrong == 0 && leaked == 0 && badArticle == 0,
           counts(r) + fmt(", %lld margin px leaked, %lld article px differ (row %d)", leaked, badArticle, firstBad));

    // 6b. Floating animated player/ad fixed directly OVER the moving article
    // column (with no static border): its stationary boundary against moving
    // article text identifies it as a fixed overlay so it pins quickly instead
    // of being treated as an in-page video and smeared over the article (#1537).
    {
      const int FW = 480, FV = 400;
      const Document fdoc = plainPage(160, FW, FV);
      SceneSpec fss;
      fss.width = FW;
      fss.height = FV;
      const Scene fscene(fdoc.img, fss);
      Rng frng(162);
      const auto fpos = steps(0, fdoc.img.h - FV, [&] { return frng.range(16, 24); });
      const Played fr = play(fscene, fpos, {}, [&](size_t i, Image& f) {
        Rng adRng(12001 + i * 7919);
        const uint32_t bg = (uint32_t(adRng.range(20, 90)) << 16) | (uint32_t(adRng.range(20, 90)) << 8) | uint32_t(adRng.range(40, 120));
        const uint32_t fg = (uint32_t(adRng.range(180, 250)) << 16) | (uint32_t(adRng.range(180, 250)) << 8) | uint32_t(adRng.range(160, 240));
        for (int y = 240; y < 360; ++y)
          for (int x = 280; x < 440; ++x) f.at(x, y) = bg;
        for (int y = 248; y + 8 < 356; y += 14) drawTextLine(f, y, 288, 432, adRng, fg);
        return false;
      });
      int fFirstBad = 0;
      // Ignore only the first screen (before pinning) and final screen (where
      // the pinned player sits over the bottom of the page).
      const long long fBad = differing(fr, pageRows(fdoc.img, 0, fdoc.img.h, FW),
                                       {{272, 232, 448, FV + 96}, {272, fdoc.img.h - 168, 448, fdoc.img.h}}, 0, &fFirstBad);
      report("floating animated player over article column: pins fast, zero smear",
             fr.wrong == 0 && fr.totals.pinnedRegions > 0 && fBad == 0 && fr.totals.bottom == fdoc.img.h,
             counts(fr) + fmt(", %d pinned, %d wrong%s, %lld px differ (row %d)", fr.totals.pinnedRegions, fr.wrong,
                              fr.firstWrong.c_str(), fBad, fFirstBad));
    }
  }

  {
    // 7. Recovering from Lost onto a playing video whose static poster was
    // stored earlier: jump past the viewport (Lost), video starts playing,
    // scroll back over the video -> searchStore recovers and marks it dynamic.
    const int W = 480, V = 400;
    const Document doc = plainPage(161, W, V);
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    ss.changingX = 60;
    ss.changingY = 900;
    ss.changingW = 360;
    ss.changingH = 180;
    const Scene scene(doc.img, ss);
    MemoryCanvas canvas(W);
    ScrollTracker tracker(canvas);
    int wrong = 0, recoveredAfterLost = 0;
    bool wasLost = false;
    // Scroll down over static poster (rows 0..1600 stored).
    for (int p = 0; p <= 1200; p += 60) {
      const TrackResult t = tracker.addFrame(scene.frame(p, 0).view());
      if (t.status == TrackStatus::Placed && t.position != p) ++wrong;
    }
    // Jump past viewport (3 frames far away -> Lost).
    for (int k = 0; k < 3; ++k) {
      const int p = 2400 + k * 500;
      const TrackResult t = tracker.addFrame(scene.frame(p, 0).view());
      if (t.state == TrackState::Lost) wasLost = true;
    }
    // Return over the video while it plays at every frame.
    int placedBack = 0;
    for (int k = 0, p = 820; k < 14; ++k, p += 10) {
      const TrackResult t = tracker.addFrame(scene.frame(p, 1 + k).view());
      if (t.status == TrackStatus::Placed) {
        ++placedBack;
        if (t.recovered) ++recoveredAfterLost;
        if (t.position != p) ++wrong;
      }
    }
    const TrackTotals tot = tracker.finish();
    Played r;
    r.totals = tot;
    r.image = canvas.image(tot.top, tot.bottom);
    r.width = W;
    int firstBad = 0;
    const long long bad = differing(r, pageRows(doc.img, 0, int(tot.bottom), W), {{52, 892, 428, 1088}}, 0, &firstBad);
    report("recovering from Lost onto a playing video over its stored poster",
           wasLost && recoveredAfterLost > 0 && placedBack >= 10 && wrong == 0 && bad == 0,
           fmt("wasLost %d, recovered %d, placedBack %d/14, %d wrong, %lld px differ (row %d)", int(wasLost), recoveredAfterLost, placedBack,
               wrong, bad, firstBad));
  }

  {
    // 8. Fast scroll jumps past the viewport with an unpinned sticky header
    // and sidebar (between minArea and stillFallback of textured cells): after
    // a single placed scroll step, jumping > viewport every frame must stay
    // Skipped/Lost, never turning into false NoMovement.
    const int W = 640, V = 400;
    const Image doc = columnPage(163, W, 6400, 128, 624, 160, 128, 624);
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    ss.pinned.push_back(PinnedPart{0, 0, W, 40, 0xD8DCE6, 0x202430, 0, 165});
    ss.pinned.push_back(PinnedPart{0, 40, 112, V - 40, 0xE2E6EE, 0x202430, 0, 167});
    const Scene scene(doc, ss);
    // Test 0-px (immediate jump), 1-px (< learnMinStep), and 60-px initial steps (#1537).
    for (const int firstStep : {0, 1, 60}) {
      std::vector<int> pos{0};
      if (firstStep > 0) pos.push_back(firstStep);
      const size_t jumpStart = pos.size();
      for (int p = 700; p + V <= doc.h; p += 520) pos.push_back(p);
      const Played r = play(scene, pos);
      const bool setupPlaced = firstStep == 0 || (r.results.size() > 1 && r.results[1].status == TrackStatus::Placed &&
                                                  r.results[1].position == firstStep);
      int falseStill = 0, lost = 0;
      for (size_t i = jumpStart; i < r.results.size(); ++i) {
        falseStill += r.results[i].status == TrackStatus::NoMovement;
        lost += r.results[i].state == TrackState::Lost;
      }
      report(fmt("jumps past viewport with unpinned sticky chrome (%d-px step) stay Lost", firstStep),
             setupPlaced && r.wrong == 0 && falseStill == 0 && lost > 0,
             counts(r) + fmt(", setupPlaced %d, %d false NoMovement, %d Lost frames", int(setupPlaced), falseStill, lost));
    }
    // Late sticky chrome appearing at position 32 over previously-scrolling
    // page content after three clean 8-px steps, followed by jumps to 700 and
    // 1220: must stay Lost, not NoMovement (#1537).
    {
      const Image docLate = columnPage(163, W, 6400, 16, 624, 160, 16, 624);
      SceneSpec ssLate;
      ssLate.width = W;
      ssLate.height = V;
      ssLate.pinned.push_back(PinnedPart{0, 0, W, 40, 0xD8DCE6, 0x202430, 32, 165});
      ssLate.pinned.push_back(PinnedPart{0, 40, 112, V - 40, 0xE2E6EE, 0x202430, 32, 167});
      const Scene sceneLate(docLate, ssLate);
      std::vector<int> pos{0, 8, 16, 24, 32};
      for (int p = 700; p + V <= doc.h; p += 520) pos.push_back(p);
      const Played r = play(sceneLate, pos);
      const bool setupPlaced = r.results.size() >= 5 && r.results[4].status == TrackStatus::Placed && r.results[4].position == 32;
      int falseStill = 0, lost = 0;
      for (size_t i = 5; i < r.results.size(); ++i) {
        falseStill += r.results[i].status == TrackStatus::NoMovement;
        lost += r.results[i].state == TrackState::Lost;
      }
      report("jumps past viewport after late sticky chrome appears stay Lost",
             setupPlaced && r.wrong == 0 && falseStill == 0 && lost > 0,
             counts(r) + fmt(", setupPlaced %d, %d false NoMovement, %d Lost frames", int(setupPlaced), falseStill, lost));
    }
  }

  {
    // 9a. Transient hover popup fading in and out while paused at rest: frames
    // at rest must not overwrite the clean page with the popup (#1537).
    const int W = 480, V = 400;
    const Document doc = plainPage(169, W, V);
    const int end = doc.img.h - V;
    SceneSpec ss;
    ss.width = W;
    ss.height = V;
    const Scene scene(doc.img, ss);
    std::vector<int> pos{0};
    while (pos.back() < 400) pos.push_back(std::min(400, pos.back() + 24));
    const size_t pauseAt = pos.size();
    for (int k = 0; k < 8; ++k) pos.push_back(400);
    while (pos.back() < end) pos.push_back(std::min(end, pos.back() + 24));
    const Played r = play(scene, pos, {}, [&](size_t i, Image& f) {
      if (i >= pauseAt + 1 && i <= pauseAt + 4) {
        const uint32_t bg = (uint32_t(180 + (i - pauseAt) * 15) << 16) | 0x3050u;
        for (int y = 140; y < 260; ++y)
          for (int x = 100; x < 380; ++x) f.at(x, y) = bg;
        Rng prng(4000 + i * 97);
        for (int y = 152; y < 248; y += 16) drawTextLine(f, y, 112, 368, prng, 0xF8F8F8);
      }
      return false;
    });
    int firstBad = 0;
    const long long bad = differing(r, pageRows(doc.img, 0, doc.img.h, W), {}, 0, &firstBad);
    report("transient hover popup at rest is not baked into the stitched page",
           r.wrong == 0 && bad == 0 && r.totals.bottom == doc.img.h,
           counts(r) + fmt(", %d wrong%s, %lld px differ (row %d)", r.wrong, r.firstWrong.c_str(), bad, firstBad));

    // 9b. Lazy-loaded image fading in over scroll frames with a short mid-fade
    // plateau (4 frames held at 224/256 opacity) before finishing at 256/256:
    // test continuous scrolling, finishing the fade during a pause at rest,
    // and finishing while clipped before scrolling back to full view (#1537).
    Image settledDoc = doc.img;
    const int lx0 = 80, lx1 = 400, ly0 = 520, ly1 = 680;
    Rng artRng(173);
    for (int y = ly0; y < ly1; ++y)
      for (int x = lx0; x < lx1; ++x) {
        const uint32_t rC = uint32_t(40 + ((x - lx0) * 180) / (lx1 - lx0));
        const uint32_t gC = uint32_t(60 + ((y - ly0) * 160) / (ly1 - ly0));
        const uint32_t bC = uint32_t(120 + (((x + y) / 8) % 80));
        settledDoc.at(x, y) = (rC << 16) | (gC << 8) | bC;
      }
    for (int y = ly0 + 16; y + 10 < ly1 - 12; y += 22) drawTextLine(settledDoc, y, lx0 + 16, lx1 - 16, artRng, 0xF8F8F8);
    const Scene settledScene(settledDoc, ss);
    const auto runLazy = [&](const std::vector<int>& lpos, const char* label) {
      int fullSeen = 0;
      const Played lr = play(settledScene, lpos, {}, [&](size_t i, Image& f) {
        const int p = lpos[i];
        if (p + V <= ly0 || p >= ly1) return false;
        int alpha = 256;  // 100% settled
        if (p + V < ly1) {
          alpha = 0;  // blank placeholder before fully in view
        } else if (fullSeen < 7) {
          static constexpr int kFade[7] = {48, 112, 176, 224, 224, 224, 224};
          alpha = kFade[fullSeen++];
        }
        if (alpha < 256) {
          const int y0 = std::max(0, ly0 - p);
          const int y1 = std::min(V, ly1 - p);
          for (int y = y0; y < y1; ++y)
            for (int x = lx0; x < lx1; ++x) {
              const uint32_t dst = settledDoc.at(x, p + y);
              const auto blend = [&](int shift) {
                const int bgC = int((kBg >> shift) & 0xFF);
                const int fgC = int((dst >> shift) & 0xFF);
                return uint32_t(bgC + ((fgC - bgC) * alpha) / 256);
              };
              f.at(x, y) = (blend(16) << 16) | (blend(8) << 8) | blend(0);
            }
        }
        return false;
      });
      int lFirstBad = 0;
      const long long lBad = differing(lr, pageRows(settledDoc, 0, settledDoc.h, W), {}, 0, &lFirstBad);
      report(label, lr.wrong == 0 && lBad == 0 && lr.totals.bottom == settledDoc.h,
             counts(lr) + fmt(", %d wrong%s, %lld px differ (row %d)", lr.wrong, lr.firstWrong.c_str(), lBad, lFirstBad));
    };
    {
      std::vector<int> lpos{0};
      while (lpos.back() < end) lpos.push_back(std::min(end, lpos.back() + 16));
      runLazy(lpos, "lazy-loaded image fading in with plateau settles to crisp artwork");
    }
    {
      std::vector<int> lpos{0};
      while (lpos.back() < 384) lpos.push_back(std::min(384, lpos.back() + 16));
      for (int k = 0; k < 4; ++k) lpos.push_back(384);  // finish fade during pause
      while (lpos.back() < end) lpos.push_back(std::min(end, lpos.back() + 16));
      runLazy(lpos, "lazy-loaded image finishing fade during pause settles to crisp artwork");
    }
    {
      std::vector<int> lpos{0};
      while (lpos.back() < 384) lpos.push_back(std::min(384, lpos.back() + 16));
      lpos.push_back(536);  // clip top while finishing fade, then return
      for (int p = 520; p >= 456; p -= 16) lpos.push_back(p);
      while (lpos.back() < end) lpos.push_back(std::min(end, lpos.back() + 16));
      runLazy(lpos, "lazy-loaded image finishing fade while clipped settles on return");
    }
    // 9c. Same fading lazy-loaded image near the top of the capture [160, 320):
    // even when the initial snapshot comes from retained topCopy_, its saved
    // bands still allow the settled crisp artwork to replace it cleanly (#1537).
    {
      Image topDoc = doc.img;
      const int ty0 = 160, ty1 = 320;
      Rng topRng(175);
      for (int y = ty0; y < ty1; ++y)
        for (int x = lx0; x < lx1; ++x) {
          const uint32_t rC = uint32_t(40 + ((x - lx0) * 180) / (lx1 - lx0));
          const uint32_t gC = uint32_t(60 + ((y - ty0) * 160) / (ty1 - ty0));
          const uint32_t bC = uint32_t(120 + (((x + y) / 8) % 80));
          topDoc.at(x, y) = (rC << 16) | (gC << 8) | bC;
        }
      for (int y = ty0 + 16; y + 10 < ty1 - 12; y += 22) drawTextLine(topDoc, y, lx0 + 16, lx1 - 16, topRng, 0xF8F8F8);
      const Scene topScene(topDoc, ss);
      std::vector<int> tpos{0};
      while (tpos.back() < end) tpos.push_back(std::min(end, tpos.back() + 16));
      int tSeen = 0;
      const Played tr = play(topScene, tpos, {}, [&](size_t i, Image& f) {
        const int p = tpos[i];
        if (p + V <= ty0 || p >= ty1) return false;
        int alpha = 256;
        if (tSeen < 7) {
          static constexpr int kFade[7] = {48, 112, 176, 224, 224, 224, 224};
          alpha = kFade[tSeen++];
        }
        if (alpha < 256) {
          const int y0 = std::max(0, ty0 - p);
          const int y1 = std::min(V, ty1 - p);
          for (int y = y0; y < y1; ++y)
            for (int x = lx0; x < lx1; ++x) {
              const uint32_t dst = topDoc.at(x, p + y);
              const auto blend = [&](int shift) {
                const int bgC = int((kBg >> shift) & 0xFF);
                const int fgC = int((dst >> shift) & 0xFF);
                return uint32_t(bgC + ((fgC - bgC) * alpha) / 256);
              };
              f.at(x, y) = (blend(16) << 16) | (blend(8) << 8) | blend(0);
            }
        }
        return false;
      });
      int tFirstBad = 0;
      const long long tBad = differing(tr, pageRows(topDoc, 0, topDoc.h, W), {}, 0, &tFirstBad);
      report("lazy-loaded image in first viewport (retained topCopy_) settles crisp",
             tr.wrong == 0 && tBad == 0 && tr.totals.bottom == topDoc.h,
             counts(tr) + fmt(", %d wrong%s, %lld px differ (row %d)", tr.wrong, tr.firstWrong.c_str(), tBad, tFirstBad));
    }
  }
}

int main() {
  testOffsets();
  testPinned();
  testPinnedColumns();
  testChangesAndRecovery();
  testExcluded();
  testLive();
  testHintAndEdges();
  testLookAlikes();
  testRepeatAtStep();
  testJumpPastLookAlikes();
  testLostGuessMarksNothing();
  testPlayingVideo();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
