// Deterministic synthetic document, viewport renderer and fake scrolling app
// used by test_scroll_stitcher.cpp (#1376). Not part of the product code.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
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

struct Section {
  std::string name;
  int y0, y1;
};

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
  bool textOnly = false;     // only text, noise and gradient (no flat/blank/periodic sections)
};

struct Document {
  Image img;
  std::vector<Section> sections;
  const Section* find(const std::string& n) const {
    for (auto& s : sections)
      if (s.name == n) return &s;
    return nullptr;
  }
};

Document makeDocument(const DocSpec& spec);
void fillRows(Image& img, int y0, int y1, uint32_t color);
void drawTextLine(Image& img, int y, int x0, int x1, Rng& rng, uint32_t ink = 0x23232A);

struct Rect {
  int x, y, w, h;
};

struct ViewportSpec {
  int width = 480, height = 800;
  int headerH = 0, footerH = 0;
  bool scrollbar = false, hover = false, caret = false, anim = false;
  uint64_t seed = 7;
};

class Renderer {
 public:
  Renderer(const Image& doc, ViewportSpec vs);
  int contentH() const { return vs_.height - vs_.headerH - vs_.footerH; }
  int maxScroll(int docLen = -1) const;
  // pos may exceed maxScroll (elastic overscroll): rows beyond docLen are background.
  void render(int pos, int frameIndex, Image& out, int docLen = -1) const;
  std::vector<Rect> noiseRects() const;  // viewport rects that may differ from the document
  const ViewportSpec& spec() const { return vs_; }
  const Image& header() const { return header_; }
  const Image& footer() const { return footer_; }
  // header + doc[0, docRows) + footer
  Image expected(int docRows) const;

 private:
  const Image& doc_;
  ViewportSpec vs_;
  Image header_, footer_;
  int hoverY_, caretX_, caretY_, animX_, animY_;
};

class FakeClock : public putgd::record::RunClock {
 public:
  int64_t nowMs() override { return t_; }
  void sleepMs(int ms) override { t_ += ms; }
  void set(int64_t t) { t_ = std::max(t_, t); }

 private:
  int64_t t_ = 1000;
};

struct FakeAppSpec {
  double pxPerNotch = 100;
  int animMinMs = 150, animMaxMs = 350;
  int frameIntervalMs = 50;      // 20 fps capture
  int reactionDelayMs = 16;      // event to first movement
  int dropInjection = -1;        // index of an injection the app ignores
  int failInjection = -1;        // index of an injection the injector reports as failed
  int lazyLoadAt = -1;           // doc rows >= this exist only after a delay at the end
  int lazyDelayMs = 250;
  bool elastic = false;          // overshoot past the end and spring back
  int elasticMaxPx = 60;
  double accelFactor = 1.0;      // px-per-notch multiplier for bursts of >= accelMinNotches
  int accelMinNotches = 3;
  double jitter = 0.0;           // random +-fraction on each injection's distance (line-height changes)
  bool damageOnly = false;       // deliver frames only when the picture changed
  int cancelAfterInjections = -1;
  std::atomic<bool>* cancelFlag = nullptr;
  uint64_t seed = 3;
  // Pointer rules (#1376). The loop moves a pointer; the app scrolls only when
  // it is inside wheelArea (an empty area: anywhere, pointer or not), paints a
  // hover band on the row under it, and starts scrolled to startPos.
  Rect wheelArea{0, 0, 0, 0};
  bool pointerHover = false;
  int startPos = 0;
  // A damage-driven stream that goes quiet: from this injection on it sends no
  // frame although the application scrolls, until a fresh frame is asked for
  // (KWin's region stream on a real desktop). quietHasFresh false: the source
  // has no fresh frame to give.
  int quietFromInjection = -1;
  bool quietHasFresh = true;
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
  int maxPositionSeen = 0;
  int overscrollFrames = 0;
  // Every injected event in order: 'm' x y (pointer) or 's' notches 0 (wheel).
  struct Event {
    char kind;
    int a, b;
  };
  std::vector<Event> events;
  bool pointerKnown = false;
  int pointerX = 0, pointerY = 0;

 private:
  struct Seg {
    int64_t t0;
    int dur;
    double from, to;
  };
  double posAt(int64_t t) const;
  int docLen() const { return loaded_ ? doc_.h : spec_.lazyLoadAt; }
  int maxScroll() const;
  void update(int64_t t);

  const Image& doc_;
  Renderer renderer_;
  FakeAppSpec spec_;
  FakeClock& clock_;
  Rng rng_;
  std::vector<Seg> segs_;
  double target_ = 0;
  bool loaded_ = true;
  int64_t reachedEndAt_ = -1;
  int64_t lastFrameT_ = -1;
  int frameIndex_ = 0;
  Image scratch_, delivered_;
  bool haveDelivered_ = false;
  bool quiet_ = false;
};

}  // namespace synth
