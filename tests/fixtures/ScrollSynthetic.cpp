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
  const double specials = (sp.textOnly ? 0 : sp.flatFactor + sp.tableFactor + sp.blankFactor) +
                          sp.noiseFactor + sp.gradientFactor;
  const int maxH = sp.topBlank + int(specials * V) + 7 * sp.paragraphsPerGap * (9 * 16 + 40) + 64;
  Document doc;
  doc.img = Image(W, maxH, kBg);
  Image& img = doc.img;
  int y = sp.topBlank;
  auto paras = [&](int n) {
    const int y0 = y;
    for (int i = 0; i < n; ++i) y = paragraph(img, y, rng, W);
    doc.sections.push_back({"text", y0, y});
  };
  paras(sp.paragraphsPerGap);
  if (!sp.textOnly) {
    const int h = int(sp.flatFactor * V);
    fillRows(img, y, y + h, rgb(60, 110, 170));
    doc.sections.push_back({"flat", y, y + h});
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
    doc.sections.push_back({"table", y, y + rows * rowH});
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
    doc.sections.push_back({"noise", y, y + h});
    y += h;
    paras(sp.paragraphsPerGap);
  }
  {  // wide gradient: every row distinct, no horizontal edges
    const int h = int(sp.gradientFactor * V);
    for (int yy = 0; yy < h; ++yy)
      for (int x = 0; x < W; ++x)
        img.at(x, y + yy) = rgb(40 + x / 4, 30 + yy % 180, 40 + (yy / 180) * 30 + x / 8);
    doc.sections.push_back({"gradient", y, y + h});
    y += h;
    paras(sp.paragraphsPerGap);
  }
  if (!sp.textOnly) {
    const int h = int(sp.blankFactor * V);
    doc.sections.push_back({"blank", y, y + h});
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
  const int top = vs.headerH, ch = contentH();
  hoverY_ = top + ch / 4;
  caretY_ = top + ch / 2;
  animY_ = top + int(ch * 0.8) - 10;
  caretX_ = int(vs.width * 0.31);
  animX_ = int(vs.width * 0.62);
}

int Renderer::maxScroll(int docLen) const {
  if (docLen < 0) docLen = doc_.h;
  return std::max(0, docLen - contentH());
}

void Renderer::render(int pos, int frameIndex, Image& out, int docLen) const {
  const int W = vs_.width, H = vs_.height, hH = vs_.headerH, fH = vs_.footerH;
  if (docLen < 0) docLen = doc_.h;
  if (out.w != W || out.h != H) out = Image(W, H);
  for (int y = 0; y < H; ++y) {
    uint32_t* o = &out.px[size_t(y) * W];
    if (y < hH) {
      std::memcpy(o, header_.row(y), size_t(W) * 4);
    } else if (y >= H - fH) {
      std::memcpy(o, footer_.row(y - (H - fH)), size_t(W) * 4);
    } else {
      const int dr = pos + y - hH;
      if (dr >= 0 && dr < docLen) std::memcpy(o, doc_.row(dr), size_t(W) * 4);
      else std::fill(o, o + W, kBg);
    }
  }
  const int ch = contentH();
  if (vs_.hover) {
    for (int y = hoverY_; y < std::min(hoverY_ + 18, H - fH); ++y)
      for (int x = 0; x < W - 12; ++x)
        if (out.at(x, y) == kBg) out.at(x, y) = rgb(228, 236, 247);
  }
  if (vs_.caret && frameIndex % 2 == 0) {
    for (int y = caretY_; y < caretY_ + 16; ++y)
      for (int x = caretX_; x < caretX_ + 2; ++x) out.at(x, y) = rgb(20, 20, 20);
  }
  if (vs_.anim) {
    Rng r(uint64_t(frameIndex) * 7919 + 17);
    for (int by = 0; by < 20; by += 4)
      for (int bx = 0; bx < 20; bx += 4) {
        const uint32_t c = uint32_t(r.next()) & 0xFFFFFF;
        for (int y = by; y < by + 4; ++y)
          for (int x = bx; x < bx + 4; ++x) out.at(animX_ + x, animY_ + y) = c;
      }
  }
  if (vs_.scrollbar) {
    const int ms = std::max(1, maxScroll(docLen));
    const int thumb = std::max(20, int((long long)ch * ch / std::max(ch, docLen)));
    const int thumbTop = hH + int((long long)std::clamp(pos, 0, ms) * (ch - thumb) / ms);
    for (int y = hH; y < H - fH; ++y)
      for (int x = W - 10; x < W - 3; ++x)
        out.at(x, y) = (y >= thumbTop && y < thumbTop + thumb) ? rgb(150, 150, 150) : rgb(225, 225, 225);
  }
}

std::vector<Rect> Renderer::noiseRects() const {
  std::vector<Rect> r;
  if (vs_.hover) r.push_back({0, hoverY_, vs_.width, 18});
  if (vs_.caret) r.push_back({caretX_, caretY_, 2, 16});
  if (vs_.anim) r.push_back({animX_, animY_, 20, 20});
  return r;
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

// ---------------------------------------------------------------- fake app

FakeApp::FakeApp(const Image& doc, ViewportSpec vs, FakeAppSpec spec, FakeClock& clock)
    : doc_(doc), renderer_(doc, vs), spec_(spec), clock_(clock), rng_(spec.seed) {
  loaded_ = spec_.lazyLoadAt < 0;
  target_ = spec_.startPos;
}

bool FakeApp::movePointer(int x, int y) {
  events.push_back({'m', x, y});
  pointerKnown = true;
  pointerX = x;
  pointerY = y;
  pointerMovedAt = clock_.nowMs();
  return true;
}

bool FakeApp::rest() {
  if (!spec_.restEndsOverscroll) return false;
  events.push_back({'r', 0, 0});
  overscroll_ = 0;
  return true;
}

int FakeApp::maxScroll() const { return renderer_.maxScroll(docLen()); }

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

void FakeApp::update(int64_t t) {
  if (loaded_) return;
  const bool idle = segs_.empty() || t >= segs_.back().t0 + segs_.back().dur;
  if (reachedEndAt_ < 0 && idle && posAt(t) >= maxScroll() - 0.5) reachedEndAt_ = t;
  if (reachedEndAt_ >= 0 && t >= reachedEndAt_ + spec_.lazyDelayMs) loaded_ = true;
}

bool FakeApp::scroll(int n) {
  const int idx = injections++;
  events.push_back({'s', n, 0});
  if (spec_.quietFromInjection >= 0 && idx == spec_.quietFromInjection) quiet_ = true;
  if (spec_.cancelFlag && spec_.cancelAfterInjections >= 0 && idx >= spec_.cancelAfterInjections)
    spec_.cancelFlag->store(true);
  if (idx == spec_.failInjection) return false;
  if (idx == spec_.dropInjection) return true;  // delivered but ignored by the app
  if (spec_.wheelArea.w > 0 && pointerKnown) {
    const Rect& a = spec_.wheelArea;  // the wheel goes to whatever is under the pointer
    if (pointerX < a.x || pointerX >= a.x + a.w || pointerY < a.y || pointerY >= a.y + a.h) return true;
  }
  update(clock_.nowMs());
  const int64_t start = clock_.nowMs() + spec_.reactionDelayMs;
  const double cur = posAt(start);
  const double mult = std::abs(n) >= spec_.accelMinNotches ? spec_.accelFactor : 1.0;
  const double jit = spec_.jitter > 0 ? 1.0 + spec_.jitter * (2 * rng_.unit() - 1) : 1.0;
  double delta = n * spec_.pxPerNotch * mult * jit;
  if (spec_.overscrollPx > 0) {
    if (delta > 0 && overscroll_ > 0) {
      const double used = std::min(overscroll_, delta);
      overscroll_ -= used;
      delta -= used;
    } else if (delta < 0 && target_ + delta < 0) {
      overscroll_ = std::min<double>(spec_.overscrollPx, overscroll_ - (target_ + delta));
    }
  }
  double nt = std::round(target_ + delta);
  const int dur = rng_.range(spec_.animMinMs, spec_.animMaxMs);
  const double hi = maxScroll();
  nt = std::max(0.0, nt);
  if (nt > hi) {
    if (spec_.elastic && n > 0) {
      const double over = std::min<double>(spec_.elasticMaxPx, std::max(8.0, (nt - hi) * 0.5));
      const int d1 = int(dur * 0.6);
      segs_.push_back({start, d1, cur, hi + over});
      segs_.push_back({start + d1, 250, hi + over, hi});
      target_ = hi;
      return true;
    }
    nt = hi;
  }
  segs_.push_back({start, dur, cur, nt});
  target_ = nt;
  return true;
}

std::optional<putgd::record::RunFrame> FakeApp::freshFrame(int timeoutMs) {
  if (!spec_.quietHasFresh) return std::nullopt;
  ++freshFrames;
  quiet_ = false;  // a new link: the stream sends again
  haveDelivered_ = false;
  return nextFrame(timeoutMs);
}

std::optional<putgd::record::RunFrame> FakeApp::nextFrame(int timeoutMs) {
  const int64_t limit = clock_.nowMs() + std::max(0, timeoutMs);
  if (quiet_) {
    clock_.set(limit);
    return std::nullopt;
  }
  for (;;) {
    const int64_t t = lastFrameT_ < 0 ? clock_.nowMs()
                                      : std::max(lastFrameT_ + spec_.frameIntervalMs, clock_.nowMs());
    if (t > limit) {
      clock_.set(limit);
      return std::nullopt;
    }
    clock_.set(t);
    lastFrameT_ = t;
    update(t);
    const int pos = int(std::lround(posAt(t)));
    renderer_.render(pos, frameIndex_++, scratch_, docLen());
    if (spec_.pointerHover && pointerKnown && pointerX >= 0 && pointerX < scratch_.w) {
      // The row under the pointer is highlighted, as a list or a link would be.
      for (int y = std::max(0, pointerY - 9); y < std::min(scratch_.h, pointerY + 9); ++y)
        for (int x = 0; x < scratch_.w - 12; ++x)
          if (scratch_.at(x, y) == kBg) scratch_.at(x, y) = rgb(228, 236, 247);
    }
    for (int y = 0; y < std::min(scratch_.h, spec_.restlessRows); ++y)
      for (int x = 0; x < scratch_.w; ++x) scratch_.at(x, y) = rgb((frameIndex_ * 37 + y) & 255, (x * 3 + frameIndex_ * 11) & 255, 90);
    if (spec_.overlay) spec_.overlay(scratch_, injections);
    if (spec_.scene) spec_.scene(scratch_, *this, pos, t);
    if (spec_.damageOnly && haveDelivered_ && scratch_.px == delivered_.px) continue;
    std::swap(scratch_, delivered_);
    haveDelivered_ = true;
    framePositions.push_back(pos);
    maxPositionSeen = std::max(maxPositionSeen, pos);
    if (pos > maxScroll()) ++overscrollFrames;
    return putgd::record::RunFrame{delivered_.view(), t};
  }
}

}  // namespace synth
