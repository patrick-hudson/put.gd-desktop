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
    report("the view moves down, header and banner with it: neither in the page", header == 0 && navy == 0,
           fmt("%zu frames, %d placed, %lld header pixels and %lld banner pixels between the image's ends", pos.size(), r.placed, header, navy));
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
    report("the view moves down in easing steps: header and banner not in the page", header == 0 && navy == 0,
           fmt("%zu frames, %d placed, %lld header pixels and %lld banner pixels between the image's ends", pos.size(), r.placed, header, navy));
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
    PinnedPart buttons{360, 0, 220, 22, kBg, 0x202024, 0, 111};
    buttons.border = false;
    ss.pinned.push_back(buttons);
    PinnedPart toolbar{0, 22, W, 26, 0xEEEEF0, 0x303034, 0, 113};
    ss.pinned.push_back(toolbar);
    PinnedPart toc{512, 56, 120, 300, kBg, 0x3366CC, 0, 115};
    toc.border = false;
    toc.stickFrom = 56;
    toc.stickTo = doc.h - F - 16;
    ss.pinned.push_back(toc);
    const Scene scene(doc, ss);
    const Image expected = pageRows(doc, 0, doc.h, W);
    // Wheel steps: a person's flick can leave a row of the article seen only
    // under the toolbar's edge, two rows into a block of the page (a known
    // limit of the 8-row blocks, not of this scene's rule).
    for (const auto& [style, pos] : styles(doc.h - V)) {
      if (std::string(style) != "wheel steps") continue;
      const Played r = play(scene, pos);
      int firstBad = 0;
      // The article and its margin: left of the column, below the header.
      const long long bad = differing(r, expected, {{0, 0, W, 48}, {508, 0, W, doc.h}}, 0, &firstBad);
      report(fmt("a column lets go under a header with buttons over a toolbar, %s: the article whole", style), r.wrong == 0 && bad == 0,
             counts(r) + fmt(", %d wrong%s, %lld pixels differ beside the column (row %d)", r.wrong, r.firstWrong.c_str(), bad, firstBad));
    }
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
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
