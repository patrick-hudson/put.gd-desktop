#include "fixtures/ScrollSynthetic.h"

#include <cstring>

namespace synth {

static uint32_t rgb(int r, int g, int b) {
  auto c = [](int v) { return uint32_t(std::clamp(v, 0, 255)); };
  return (c(r) << 16) | (c(g) << 8) | c(b);
}

void fillRows(Image& img, int y0, int y1, uint32_t color) {
  for (int y = std::max(0, y0); y < std::min(img.h, y1); ++y)
    std::fill(&img.px[size_t(y) * img.w], &img.px[size_t(y) * img.w] + img.w, color);
}

// One text line: 11 rows of glyph-like random ink, words of random width.
void drawTextLine(Image& img, int y, int x0, int x1, Rng& rng, uint32_t ink) {
  int x = x0;
  while (x < x1) {
    const int wordW = rng.range(10, 48);
    const int end = std::min(x1, x + wordW);
    for (int gx = x; gx < end; gx += 6) {
      for (int r = 0; r < 11; ++r) {
        const uint64_t bits = rng.next();
        for (int c = 0; c < 5 && gx + c < end; ++c)
          if (((bits >> c) & 3) == 0 || ((bits >> (c + 8)) & 7) == 0)  // density ~0.4
            if (y + r >= 0 && y + r < img.h) img.at(gx + c, y + r) = ink;
      }
    }
    x = end + rng.range(5, 8);
  }
}

namespace {

int paragraph(Image& img, int y, Rng& rng, int width) {
  const int lines = rng.range(3, 9);
  const int left = 30, maxRight = width - 30;
  for (int i = 0; i < lines; ++i) {
    const int right = left + int((maxRight - left) * (0.4 + 0.6 * rng.unit()));
    drawTextLine(img, y, left, right, rng);
    y += 16;
  }
  return y + rng.range(24, 40);
}

}  // namespace

Document makeDocument(const DocSpec& sp) {
  Rng rng(sp.seed);
  const int V = sp.viewport, W = sp.width;
  const double specials = sp.flatFactor + sp.tableFactor + sp.blankFactor + sp.noiseFactor + sp.gradientFactor;
  const int maxH = sp.topBlank + int(specials * V) + 7 * sp.paragraphsPerGap * (9 * 16 + 40) + 64;
  Document doc;
  doc.img = Image(W, maxH, kBg);
  Image& img = doc.img;
  int y = sp.topBlank;
  auto paras = [&](int n) {
    for (int i = 0; i < n; ++i) y = paragraph(img, y, rng, W);
  };
  paras(sp.paragraphsPerGap);
  {
    const int h = int(sp.flatFactor * V);
    fillRows(img, y, y + h, rgb(60, 110, 170));
    y += h;
    paras(sp.paragraphsPerGap);

    // Perfectly periodic table: identical 24-row rows.
    const int rowH = 24;
    Image tmpl(W, rowH, kBg);
    for (int x = 24; x < W - 24; ++x) tmpl.at(x, 0) = rgb(200, 200, 200);
    Rng trng(sp.seed ^ 0xABCDEF);
    const int cellW = (W - 48) / 4;
    for (int c = 0; c < 4; ++c) {
      const int cx = 24 + c * cellW;
      for (int r = 0; r < rowH; ++r) tmpl.at(cx, r) = rgb(200, 200, 200);
      drawTextLine(tmpl, 7, cx + 8, cx + cellW - 10, trng);
    }
    const int rows = std::max(1, int(sp.tableFactor * V) / rowH);
    for (int i = 0; i < rows; ++i)
      for (int r = 0; r < rowH; ++r)
        std::memcpy(&img.px[size_t(y + i * rowH + r) * W], tmpl.row(r), size_t(W) * 4);
    y += rows * rowH;
    paras(sp.paragraphsPerGap);
  }
  {  // photo-like noise
    const int h = int(sp.noiseFactor * V);
    const double ph = rng.unit() * 6.28;
    for (int yy = 0; yy < h; ++yy)
      for (int x = 0; x < W; ++x) {
        const double base = 128 + 70 * std::sin(x * 0.045 + ph) * std::cos(yy * 0.06 + ph * 0.5);
        img.at(x, y + yy) = rgb(int(base) + rng.range(-30, 30), int(base * 0.8) + rng.range(-30, 30),
                                int(255 - base) + rng.range(-30, 30));
      }
    y += h;
    paras(sp.paragraphsPerGap);
  }
  {  // wide gradient: every row distinct, no horizontal edges
    const int h = int(sp.gradientFactor * V);
    for (int yy = 0; yy < h; ++yy)
      for (int x = 0; x < W; ++x)
        img.at(x, y + yy) = rgb(40 + x / 4, 30 + yy % 180, 40 + (yy / 180) * 30 + x / 8);
    y += h;
    paras(sp.paragraphsPerGap);
  }
  {
    const int h = int(sp.blankFactor * V);
    y += h;
    paras(sp.paragraphsPerGap);
  }
  paras(1);
  img.h = y;
  img.px.resize(size_t(W) * y);
  return doc;
}

// ---------------------------------------------------------------- renderer

Renderer::Renderer(const Image& doc, ViewportSpec vs) : doc_(doc), vs_(vs) {
  Rng rng(vs.seed);
  if (vs.headerH > 0) {
    header_ = Image(vs.width, vs.headerH, rgb(230, 230, 240));
    drawTextLine(header_, std::max(0, (vs.headerH - 11) / 2), 40, vs.width / 2, rng, rgb(40, 40, 90));
    fillRows(header_, vs.headerH - 1, vs.headerH, rgb(180, 180, 200));
  }
  if (vs.footerH > 0) {
    footer_ = Image(vs.width, vs.footerH, rgb(43, 47, 58));
    drawTextLine(footer_, std::max(0, (vs.footerH - 11) / 2), 60, vs.width - 80, rng, rgb(220, 220, 220));
  }
}

void Renderer::render(int pos, Image& out) const {
  const int W = vs_.width, H = vs_.height, hH = vs_.headerH, fH = vs_.footerH;
  if (out.w != W || out.h != H) out = Image(W, H);
  for (int y = 0; y < H; ++y) {
    uint32_t* o = &out.px[size_t(y) * W];
    if (y < hH) {
      std::memcpy(o, header_.row(y), size_t(W) * 4);
    } else if (y >= H - fH) {
      std::memcpy(o, footer_.row(y - (H - fH)), size_t(W) * 4);
    } else {
      const int dr = pos + y - hH;
      if (dr >= 0 && dr < doc_.h) std::memcpy(o, doc_.row(dr), size_t(W) * 4);
      else std::fill(o, o + W, kBg);
    }
  }
}

Image Renderer::expected(int docRows) const {
  const int W = vs_.width, hH = vs_.headerH, fH = vs_.footerH;
  docRows = std::clamp(docRows, 0, doc_.h);
  Image e(W, hH + docRows + fH);
  for (int y = 0; y < hH; ++y) std::memcpy(&e.px[size_t(y) * W], header_.row(y), size_t(W) * 4);
  std::memcpy(&e.px[size_t(hH) * W], doc_.px.data(), size_t(docRows) * W * 4);
  for (int y = 0; y < fH; ++y)
    std::memcpy(&e.px[size_t(hH + docRows + y) * W], footer_.row(y), size_t(W) * 4);
  return e;
}

// ---------------------------------------------------------------- scene

Scene::Scene(const Image& doc, SceneSpec spec) : doc_(doc), spec_(std::move(spec)) {
  for (const PinnedPart& p : spec_.pinned) {
    Image part(p.w, p.h, p.background);
    Rng rng(p.seed);
    for (int x = 0; x < p.w; ++x) {
      part.at(x, 0) = 0x9097A6;
      part.at(x, p.h - 1) = 0x9097A6;
    }
    for (int y = 0; y < p.h; ++y) {
      part.at(0, y) = 0x9097A6;
      part.at(p.w - 1, y) = 0x9097A6;
    }
    for (int y = 10; y + 12 < p.h; y += 22) drawTextLine(part, y, 10, std::max(12, p.w - 14), rng, p.ink);
    parts_.push_back(std::move(part));
  }
}

Image Scene::frame(int pos, int frame) const {
  const int W = spec_.width, H = spec_.height;
  Image out(W, H, kBg);
  for (int y = 0; y < H; ++y) {
    const int dr = pos + y;
    if (dr >= 0 && dr < doc_.h) std::memcpy(&out.px[size_t(y) * W], doc_.row(dr), size_t(std::min(W, doc_.w)) * 4);
  }
  if (spec_.antialias > 0) {
    // Edge pixels of the ink (next to the background) brighter or darker by a
    // little, differently in every frame: the same text drawn again.
    for (int y = 0; y < H; ++y)
      for (int x = 1; x + 1 < W; ++x) {
        const uint32_t v = out.at(x, y);
        if (v == kBg || (out.at(x - 1, y) != kBg && out.at(x + 1, y) != kBg)) continue;
        uint64_t hsh = uint64_t(pos + y) * 0x9E3779B97F4A7C15ull ^ uint64_t(x) * 0xC2B2AE3D27D4EB4Full ^ uint64_t(frame) * 0x165667B19E3779F9ull;
        hsh ^= hsh >> 29;
        const int delta = int(hsh % uint64_t(2 * spec_.antialias + 1)) - spec_.antialias;
        auto ch = [&](int shift) { return std::clamp(int((v >> shift) & 255) + delta, 0, 255); };
        out.at(x, y) = (uint32_t(ch(16)) << 16) | (uint32_t(ch(8)) << 8) | uint32_t(ch(0));
      }
  }
  if (spec_.changingX >= 0) {
    for (int y = 0; y < H; ++y) {
      const int dr = pos + y;
      if (dr < spec_.changingY || dr >= spec_.changingY + spec_.changingH) continue;
      for (int x = spec_.changingX; x < std::min(W, spec_.changingX + spec_.changingW); ++x)
        out.at(x, y) = rgb((frame * 29 + x) & 255, (dr * 3 + frame * 17) & 255, (x ^ dr ^ frame * 5) & 255);
    }
  }
  for (size_t i = 0; i < spec_.pinned.size(); ++i) {
    const PinnedPart& p = spec_.pinned[i];
    if (pos < p.fromPos) continue;
    for (int y = 0; y < p.h && p.y + y < H; ++y)
      std::memcpy(&out.px[size_t(p.y + y) * W + p.x], parts_[i].row(y), size_t(std::min(p.w, W - p.x)) * 4);
  }
  return out;
}

// ---------------------------------------------------------------- fake app

FakeApp::FakeApp(const Image& doc, ViewportSpec vs, FakeAppSpec spec, FakeClock& clock)
    : doc_(doc), renderer_(doc, vs), spec_(spec), clock_(clock), rng_(spec.seed) {
  target_ = spec_.startPos;
}

bool FakeApp::movePointer(int x, int y) {
  events.push_back({'m', x, y});
  return true;
}

int FakeApp::maxScroll() const { return renderer_.maxScroll(); }

double FakeApp::posAt(int64_t t) const {
  if (segs_.empty()) return spec_.startPos;
  for (auto it = segs_.rbegin(); it != segs_.rend(); ++it) {
    if (it->t0 > t) continue;
    if (t >= it->t0 + it->dur) return it->to;
    const double u = double(t - it->t0) / it->dur;
    const double e = 1 - (1 - u) * (1 - u) * (1 - u);  // ease-out cubic
    return it->from + (it->to - it->from) * e;
  }
  return segs_.front().from;
}

bool FakeApp::scroll(int n) {
  const int idx = injections++;
  events.push_back({'s', n, 0});
  if (spec_.quietFromInjection >= 0 && idx == spec_.quietFromInjection) quiet_ = true;
  if (spec_.cancelFlag && spec_.cancelAfterInjections >= 0 && idx >= spec_.cancelAfterInjections)
    spec_.cancelFlag->store(true);
  if (spec_.busyMsPerNotch > 0) {
    // What it has not painted yet it paints when it has worked through all it was sent.
    const int64_t now = clock_.nowMs();
    while (!segs_.empty() && segs_.back().t0 > now) segs_.pop_back();
    const bool idle = busyUntil_ <= now;
    busyUntil_ = std::max(busyUntil_, now) + int64_t(std::abs(n)) * spec_.busyMsPerNotch;
    const double nt = std::clamp(std::round(target_ + n * spec_.pxPerNotch), 0.0, double(maxScroll()));
    if (spec_.busyShowsFirst && idle && std::abs(n) > 1) {
      const double first = std::clamp(std::round(target_ + (n > 0 ? 1 : -1) * spec_.pxPerNotch), 0.0, double(maxScroll()));
      segs_.push_back({now + spec_.busyMsPerNotch, 1, posAt(now), first});
      segs_.push_back({busyUntil_, 1, first, nt});
      target_ = nt;
      return true;
    }
    segs_.push_back({busyUntil_, 1, posAt(now), nt});
    target_ = nt;
    return true;
  }
  const int64_t start = clock_.nowMs() + spec_.reactionDelayMs;
  const double cur = posAt(start);
  const double nt = std::clamp(std::round(target_ + n * spec_.pxPerNotch), 0.0, double(maxScroll()));
  segs_.push_back({start, rng_.range(spec_.animMinMs, spec_.animMaxMs), cur, nt});
  target_ = nt;
  return true;
}

std::optional<putgd::record::RunFrame> FakeApp::freshFrame(int timeoutMs) {
  if (!spec_.quietHasFresh) return std::nullopt;
  ++freshFrames;
  quiet_ = false;  // a new link: the stream sends again
  return nextFrame(timeoutMs);
}

std::optional<putgd::record::RunFrame> FakeApp::nextFrame(int timeoutMs) {
  const int64_t limit = clock_.nowMs() + std::max(0, timeoutMs);
  if (quiet_) {
    clock_.set(limit);
    return std::nullopt;
  }
  const int64_t t = lastFrameT_ < 0 ? clock_.nowMs() : std::max(lastFrameT_ + spec_.frameIntervalMs, clock_.nowMs());
  if (t > limit) {
    clock_.set(limit);
    return std::nullopt;
  }
  clock_.set(t);
  lastFrameT_ = t;
  ++frameIndex_;
  const int pos = int(std::lround(posAt(t)));
  renderer_.render(pos, scratch_);
  for (int y = 0; y < std::min(scratch_.h, spec_.restlessRows); ++y)
    for (int x = 0; x < scratch_.w; ++x) scratch_.at(x, y) = rgb((frameIndex_ * 37 + y) & 255, (x * 3 + frameIndex_ * 11) & 255, 90);
  std::swap(scratch_, delivered_);
  framePositions.push_back(pos);
  return putgd::record::RunFrame{delivered_.view(), t};
}

}  // namespace synth
