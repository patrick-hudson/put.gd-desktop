// Deterministic synthetic document, viewport renderer and fake scrolling app
// used by test_scroll_stitcher.cpp (#1376), and the scenes of
// test_scroll_tracker.cpp (#1515): parts pinned over the page, a header that
// shows up late, a block that changes in place, text drawn again with other
// antialiasing. Not part of the product code.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "record/ScrollRun.h"

namespace synth {

struct Image {
  int w = 0, h = 0;
  std::vector<uint32_t> px;  // 0x00RRGGBB: bytes B,G,R,X in memory (little-endian)
  Image() = default;
  Image(int w_, int h_, uint32_t fill = 0) : w(w_), h(h_), px(size_t(w_) * h_, fill) {}
  uint32_t& at(int x, int y) { return px[size_t(y) * w + x]; }
  uint32_t at(int x, int y) const { return px[size_t(y) * w + x]; }
  const uint32_t* row(int y) const { return &px[size_t(y) * w]; }
  putgd::scroll::FrameView view() const {
    return {reinterpret_cast<const uint8_t*>(px.data()), w, h, std::ptrdiff_t(w) * 4};
  }
};

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
  uint64_t next() {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  int range(int lo, int hi) { return lo + int(next() % uint64_t(hi - lo + 1)); }  // inclusive
  double unit() { return double(next() >> 11) / double(1ull << 53); }
};

constexpr uint32_t kBg = 0xF5F5F5;

struct DocSpec {
  uint64_t seed = 1;
  int width = 480;
  int viewport = 800;   // section sizes below are multiples of this
  int topBlank = 30;
  double flatFactor = 0.8;   // large flat colour block
  double tableFactor = 0.5;  // perfectly periodic table rows
  double noiseFactor = 0.6;  // photo-like noise
  double gradientFactor = 0.6;
  double blankFactor = 0.6;  // long blank section
  int paragraphsPerGap = 2;  // text paragraphs between special sections
};

struct Document {
  Image img;
};

Document makeDocument(const DocSpec& spec);
void fillRows(Image& img, int y0, int y1, uint32_t color);
void drawTextLine(Image& img, int y, int x0, int x1, Rng& rng, uint32_t ink = 0x23232A);

struct ViewportSpec {
  int width = 480, height = 800;
  int headerH = 0, footerH = 0;
  uint64_t seed = 7;
};

class Renderer {
 public:
  Renderer(const Image& doc, ViewportSpec vs);
  int contentH() const { return vs_.height - vs_.headerH - vs_.footerH; }
  int maxScroll() const { return std::max(0, doc_.h - contentH()); }
  // Rows beyond the document are background.
  void render(int pos, Image& out) const;
  // header + doc[0, docRows) + footer
  Image expected(int docRows) const;

 private:
  const Image& doc_;
  ViewportSpec vs_;
  Image header_, footer_;
};

// A part pinned over the page, in viewport coordinates: a flat box with a
// border and a few lines of text.
struct PinnedPart {
  int x = 0, y = 0, w = 0, h = 0;
  uint32_t background = 0x2B2F3A;
  uint32_t ink = 0xE6E6E6;
  int fromPos = 0;  // shows once the page is scrolled at least this far (a header that comes late)
  uint64_t seed = 11;
};

struct SceneSpec {
  int width = 480, height = 400;
  std::vector<PinnedPart> pinned;
  // A block of the page (document coordinates) that changes with every frame.
  int changingX = -1, changingY = 0, changingW = 0, changingH = 0;
  // Text drawn again with other antialiasing: the ink's edge pixels shift in
  // brightness by up to this much from frame to frame.
  int antialias = 0;
};

class Scene {
 public:
  Scene(const Image& doc, SceneSpec spec);
  // The viewport at scroll position pos; frame numbers the picture (what
  // changes in place, and the antialiasing, change with it).
  Image frame(int pos, int frame) const;
  const SceneSpec& spec() const { return spec_; }
  const Image& doc() const { return doc_; }
  // A pinned part as drawn (its own pixels, w x h).
  const Image& part(size_t i) const { return parts_[i]; }

 private:
  const Image& doc_;
  SceneSpec spec_;
  std::vector<Image> parts_;
};

class FakeClock : public putgd::record::RunClock {
 public:
  int64_t nowMs() override { return t_; }
  void sleepMs(int ms) override { t_ += ms; }
  void set(int64_t t) { t_ = std::max(t_, t); }

 private:
  int64_t t_ = 1000;
};

class FakeApp;

struct FakeAppSpec {
  double pxPerNotch = 100;
  int animMinMs = 150, animMaxMs = 350;
  int frameIntervalMs = 50;      // 20 fps capture
  int reactionDelayMs = 16;      // event to first movement
  int cancelAfterInjections = -1;
  std::atomic<bool>* cancelFlag = nullptr;
  uint64_t seed = 3;
  int startPos = 0;
  // A damage-driven stream that goes quiet: from this injection on it sends no
  // frame although the application scrolls, until a fresh frame is asked for
  // (KWin's region stream on a real desktop). quietHasFresh false: the source
  // has no fresh frame to give.
  int quietFromInjection = -1;
  bool quietHasFresh = true;
  // This many rows at the top of the viewport change with every frame (a
  // video in a fixed player): the picture never comes to rest.
  int restlessRows = 0;
  // The application works through its wheel notches one after another, this
  // long each, in the order they came, and paints only once none is waiting:
  // a widget that rebuilds a large picture for every wheel event (the
  // tester's non-fixed fixture on Cinnamon, 2026-11-12). 0: off.
  int busyMsPerNotch = 0;
  // With busyMsPerNotch: the first notch of a step that finds it idle is
  // painted as soon as it is worked through, the rest only once none waits.
  bool busyShowsFirst = false;
};

class FakeApp : public putgd::record::RunInjector, public putgd::record::RunFrames {
 public:
  FakeApp(const Image& doc, ViewportSpec vs, FakeAppSpec spec, FakeClock& clock);
  bool scroll(int notches) override;
  bool movePointer(int x, int y) override;
  std::optional<putgd::record::RunFrame> nextFrame(int timeoutMs) override;
  std::optional<putgd::record::RunFrame> freshFrame(int timeoutMs) override;
  int freshFrames = 0;

  const Renderer& renderer() const { return renderer_; }
  int positionNow() { return int(std::lround(posAt(clock_.nowMs()))); }
  std::vector<int> framePositions;  // every delivered frame
  int injections = 0;
  // Every injected event in order: 'm' x y (pointer), 's' notches 0 (wheel).
  struct Event {
    char kind;
    int a, b;
  };
  std::vector<Event> events;

 private:
  struct Seg {
    int64_t t0;
    int dur;
    double from, to;
  };
  double posAt(int64_t t) const;
  int maxScroll() const;

  const Image& doc_;
  Renderer renderer_;
  FakeAppSpec spec_;
  FakeClock& clock_;
  Rng rng_;
  std::vector<Seg> segs_;
  double target_ = 0;
  int64_t lastFrameT_ = -1;
  int frameIndex_ = 0;
  Image scratch_, delivered_;
  bool quiet_ = false;
  int64_t busyUntil_ = 0;
};

}  // namespace synth
