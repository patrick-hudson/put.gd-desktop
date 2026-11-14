// The scroll tracker (#1515) against synthetic scenes. Only what a wrong
// result would hide: a frame placed at another offset than the page's own
// (repeating and nearly repeating lines, small and large steps, a scroll back
// up, a frame of another page), a pinned part in the image more than once,
// and a row lost or doubled at a seam. Whether real pages come out right is
// tools/lab/scroll_tracker_score.py's job on recorded frames.
// Plain checks with a summary; exit code 1 on any failure.
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
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
            const std::function<bool(size_t, Image&)>& replace = {}) {
  Played r;
  MemoryCanvas canvas(scene.spec().width);
  ScrollTracker tracker(canvas, o);
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

int main() {
  testOffsets();
  testPinned();
  testChangesAndRecovery();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
