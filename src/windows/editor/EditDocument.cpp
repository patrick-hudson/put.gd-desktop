#include "editor/EditDocument.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace putgd::editor {
namespace {
// Sliding-window box blur of one line with clamped edges (the Qt reference's boxLine).
void boxLine(uint32_t* line, int count, ptrdiff_t step, int radius, std::vector<uint32_t>& scratch) {
    if (count < 2 || radius < 1) return;
    scratch.resize(size_t(count));
    for (int i = 0; i < count; ++i) scratch[size_t(i)] = line[ptrdiff_t(i) * step];
    const int64_t window = 2 * int64_t(radius) + 1;
    int64_t a = 0, r = 0, g = 0, b = 0;
    auto at = [&](int i) { return scratch[size_t(std::clamp(i, 0, count - 1))]; };
    for (int i = -radius; i <= radius; ++i) { const auto p = at(i); a += alphaOf(p); r += redOf(p); g += greenOf(p); b += blueOf(p); }
    for (int i = 0; i < count; ++i) {
        line[ptrdiff_t(i) * step] = argb(int(a / window), int(r / window), int(g / window), int(b / window));
        const auto out = at(i - radius), in = at(i + radius + 1);
        a += alphaOf(in) - alphaOf(out); r += redOf(in) - redOf(out); g += greenOf(in) - greenOf(out); b += blueOf(in) - blueOf(out);
    }
}
// Overlap weights of a 1-D area-average resample: for each target cell, the
// source cells it covers and their fractional coverage.
struct Tap { int source; float weight; };
std::vector<std::vector<Tap>> taps(int sourceCount, int targetCount) {
    std::vector<std::vector<Tap>> out(static_cast<size_t>(targetCount));
    const double ratio = double(sourceCount) / targetCount;
    for (int t = 0; t < targetCount; ++t) {
        const double begin = t * ratio, end = (t + 1) * ratio;
        for (int s = int(std::floor(begin)); s < int(std::ceil(end)) && s < sourceCount; ++s) {
            const double w = std::min(end, double(s + 1)) - std::max(begin, double(s));
            if (w > 0) out[size_t(t)].push_back({s, float(w / ratio)});
        }
    }
    return out;
}
}

Rect Rect::intersected(const Rect& o) const {
    const int l = std::max(x, o.x), t = std::max(y, o.y), r = std::min(right(), o.right()), b = std::min(bottom(), o.bottom());
    if (r <= l || b <= t) return {};
    return {l, t, r - l, b - t};
}
Rect Rect::spanning(Point a, Point b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::abs(a.x - b.x) + 1, std::abs(a.y - b.y) + 1};
}

Raster::Raster(int w, int h, bool hasAlpha) : width(w), height(h), alpha(hasAlpha) {
    if (w > 0 && h > 0) pixels.assign(size_t(w) * size_t(h), hasAlpha ? 0u : 0xFF000000u);
}

std::string colorName(Rgb c) {
    static const char digits[] = "0123456789abcdef";
    std::string out = "#000000";
    for (int i = 0; i < 6; ++i) out[size_t(1 + i)] = digits[(c >> (20 - 4 * i)) & 15];
    return out;
}
bool parseColor(const std::string& text, Rgb& out) {
    if (text.size() != 7 || text[0] != '#') return false;
    Rgb value = 0;
    for (size_t i = 1; i < 7; ++i) {
        const char c = text[i];
        int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) return false;
        value = value << 4 | Rgb(digit);
    }
    out = value;
    return true;
}

size_t utf16Length(const std::string& s) {
    size_t units = 0;
    for (const unsigned char c : s) {
        if ((c & 0xC0) == 0x80) continue;      // continuation byte
        units += (c >= 0xF0) ? 2 : 1;           // 4-byte sequences are surrogate pairs
    }
    return units;
}
std::string truncateUtf16(const std::string& s, size_t limit) {
    size_t units = 0, i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const size_t length = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        const size_t cost = length == 4 ? 2 : 1;
        if (units + cost > limit) break;
        units += cost; i += length;
    }
    return s.substr(0, std::min(i, s.size()));
}

Rect Shape::bounds() const {
    if (kind == Kind::Arrow) { const int pad = std::max(12, width * 5); return Rect::spanning(from, to).grown(pad, pad, pad, pad); }
    if (kind == Kind::Rectangle) return rect.grown(width, width, width, width);
    if (kind == Kind::Text) { const int pad = std::max(2, fontPixels / 6); return rect.grown(pad, pad, pad, pad); }
    return rect;
}

EditDocument::EditDocument(Raster base) : base_(std::make_shared<const Raster>(std::move(base))) { state_.crop = base_->rect(); }

const Shape* EditDocument::find(uint64_t id) const {
    for (const auto& shape : state_.shapes) if (shape.id == id) return &shape;
    return nullptr;
}
Rect EditDocument::clampRect(const Rect& rect, const Rect& bounds) { return rect.intersected(bounds); }

int64_t EditDocument::bytesOf(const State& state) {
    int64_t total = int64_t(sizeof(State)) + int64_t(state.shapes.size()) * int64_t(sizeof(Shape));
    for (const auto& shape : state.shapes) total += int64_t(shape.text.size() + shape.fontFamily.size());
    return total;
}
void EditDocument::push(State before) {
    if (before == state_) return;
    for (const auto& state : redo_) historyBytes_ -= bytesOf(state);
    redo_.clear();
    historyBytes_ += bytesOf(before);
    undo_.push_back(std::move(before));
    // Evicting the oldest step only shortens undo; the visible state never changes.
    while (!undo_.empty() && (int(undo_.size()) > maximumHistory || historyBytes_ > maximumHistoryBytes)) {
        historyBytes_ -= bytesOf(undo_.front());
        undo_.erase(undo_.begin());
        truncated_ = true;
    }
}
uint64_t EditDocument::add(Shape shape) {
    shape.id = nextId_++;
    shape.text = truncateUtf16(shape.text, maximumTextCharacters);
    shape.strength = std::clamp(shape.strength, minimumBlur, maximumBlur);
    auto before = state_;
    state_.shapes.push_back(shape);
    if (!gesture_) push(std::move(before));
    return shape.id;
}
bool EditDocument::replace(const Shape& changed) {
    for (auto& shape : state_.shapes) if (shape.id == changed.id) {
        auto next = changed;
        next.text = truncateUtf16(next.text, maximumTextCharacters);
        next.strength = std::clamp(next.strength, minimumBlur, maximumBlur);
        if (shape == next) return false;
        auto before = state_;
        shape = next;
        if (!gesture_) push(std::move(before));
        return true;
    }
    return false;
}
void EditDocument::preview(const Shape& shape) { const bool live = gesture_; gesture_ = true; replace(shape); gesture_ = live; }
bool EditDocument::remove(uint64_t id) {
    for (size_t i = 0; i < state_.shapes.size(); ++i) if (state_.shapes[i].id == id) {
        auto before = state_;
        state_.shapes.erase(state_.shapes.begin() + ptrdiff_t(i));
        if (!gesture_) push(std::move(before));
        return true;
    }
    return false;
}
bool EditDocument::setCrop(const Rect& requested) {
    const auto crop = clampRect(requested, imageRect());
    if (crop.empty() || crop == state_.crop) return false;
    auto before = state_;
    state_.crop = crop;
    if (!gesture_) push(std::move(before));
    return true;
}
void EditDocument::beginGesture() { if (!gesture_) { gestureStart_ = state_; gesture_ = true; } }
void EditDocument::commitGesture() { if (!gesture_) return; gesture_ = false; push(std::exchange(gestureStart_, {})); }
void EditDocument::cancelGesture() { if (!gesture_) return; gesture_ = false; state_ = std::exchange(gestureStart_, {}); }
bool EditDocument::undo() {
    if (gesture_ || undo_.empty()) return false;
    historyBytes_ -= bytesOf(undo_.back());
    auto previous = std::move(undo_.back());
    undo_.pop_back();
    historyBytes_ += bytesOf(state_);
    redo_.push_back(std::exchange(state_, std::move(previous)));
    return true;
}
bool EditDocument::redo() {
    if (gesture_ || redo_.empty()) return false;
    historyBytes_ -= bytesOf(redo_.back());
    auto next = std::move(redo_.back());
    redo_.pop_back();
    historyBytes_ += bytesOf(state_);
    undo_.push_back(std::exchange(state_, std::move(next)));
    return true;
}

void EditDocument::blurInPlace(Raster& image, const Rect& requested, int radius) {
    const auto area = clampRect(requested, image.rect());
    if (area.empty() || radius < 1 || image.isNull()) return;
    // Three box passes approximate a Gaussian; each pass reads only this area.
    const int box = std::max(1, radius / 2);
    std::vector<uint32_t> scratch;
    const ptrdiff_t stride = image.width;
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = area.y; y < area.bottom(); ++y) boxLine(image.row(y) + area.x, area.w, 1, box, scratch);
        for (int x = area.x; x < area.right(); ++x) boxLine(image.row(area.y) + x, area.h, stride, box, scratch);
    }
}

void EditDocument::clearHiddenColor(Raster& image) {
    if (!image.alpha) return;
    for (auto& p : image.pixels) if (alphaOf(p) == 0) p = 0;
}

Raster EditDocument::renderBase() const {
    const auto crop = state_.crop;
    const Raster& base = *base_;
    // A fresh raster carries no reader text, colour space or container data.
    Raster out;
    out.width = crop.w; out.height = crop.h; out.alpha = base.alpha;
    out.pixels.resize(size_t(crop.w) * size_t(crop.h));
    for (int y = 0; y < crop.h; ++y) std::memcpy(out.row(y), base.row(crop.y + y) + crop.x, size_t(crop.w) * sizeof(uint32_t));
    // 2. Integer source-pixel rectangles replace every covered value with opaque
    //    black: Source composition, no antialiasing.
    for (const auto& shape : state_.shapes) if (shape.kind == Shape::Kind::Redact) {
        const auto area = clampRect(shape.rect, crop).translated(-crop.x, -crop.y);
        for (int y = area.y; y < area.bottom(); ++y) std::fill_n(out.row(y) + area.x, size_t(area.w), 0xFF000000u);
    }
    // 3. Blur reads only already-redacted pixels, inside its own rectangle.
    for (const auto& shape : state_.shapes)
        if (shape.kind == Shape::Kind::Blur) blurInPlace(out, shape.rect.translated(-crop.x, -crop.y), shape.strength);
    return out;
}

Raster EditDocument::scaled(const Raster& source, const Rect& requested, int tw, int th) {
    const auto area = clampRect(requested, source.rect());
    Raster out;
    if (area.empty() || tw < 1 || th < 1) return out;
    tw = std::min(tw, area.w); th = std::min(th, area.h);
    out = Raster(tw, th, source.alpha);
    const auto horizontal = taps(area.w, tw), vertical = taps(area.h, th);
    std::vector<float> accumulator(static_cast<size_t>(tw) * 4);
    for (int ty = 0; ty < th; ++ty) {
        std::fill(accumulator.begin(), accumulator.end(), 0.f);
        for (const auto& vt : vertical[size_t(ty)]) {
            const uint32_t* src = source.row(area.y + vt.source) + area.x;
            for (int tx = 0; tx < tw; ++tx) {
                float a = 0, r = 0, g = 0, b = 0;
                for (const auto& ht : horizontal[size_t(tx)]) {
                    const uint32_t p = src[ht.source];
                    const float pa = float(alphaOf(p)) * ht.weight;
                    a += pa; r += float(redOf(p)) * pa; g += float(greenOf(p)) * pa; b += float(blueOf(p)) * pa;
                }
                float* cell = &accumulator[size_t(tx) * 4];
                cell[0] += a * vt.weight; cell[1] += r * vt.weight; cell[2] += g * vt.weight; cell[3] += b * vt.weight;
            }
        }
        uint32_t* dst = out.row(ty);
        for (int tx = 0; tx < tw; ++tx) {
            const float* cell = &accumulator[size_t(tx) * 4];
            const float a = cell[0];
            if (a <= 0.f) { dst[tx] = 0; continue; }
            auto channel = [&](float v) { return std::clamp(int(std::lround(v / a)), 0, 255); };
            const int alpha = source.alpha ? std::clamp(int(std::lround(a)), 0, 255) : 255;
            dst[tx] = argb(alpha, channel(cell[1]), channel(cell[2]), channel(cell[3]));
        }
    }
    return out;
}

Raster EditDocument::blurPreview(const Shape& shape, double scale) const {
    const auto area = clampRect(shape.rect, state_.crop);
    if (area.empty()) return {};
    // Bound preview work to about 1.5 megapixels regardless of the selected area.
    scale = std::min({1.0, scale, std::sqrt(blurPreviewPixels / (double(area.w) * area.h))});
    const int tw = std::max(1, int(std::lround(area.w * scale))), th = std::max(1, int(std::lround(area.h * scale)));
    Raster preview = scaled(*base_, area, tw, th);
    if (preview.isNull()) return {};
    const double sx = double(preview.width) / area.w, sy = double(preview.height) / area.h;
    for (const auto& other : state_.shapes) if (other.kind == Shape::Kind::Redact) {
        const auto overlap = other.rect.intersected(area);
        if (overlap.empty()) continue;
        // Map outward so a partly covered display pixel is black too.
        const int l = int(std::floor((overlap.x - area.x) * sx)), t = int(std::floor((overlap.y - area.y) * sy));
        const int r = int(std::ceil((overlap.right() - area.x) * sx)), b = int(std::ceil((overlap.bottom() - area.y) * sy));
        const auto mapped = Rect{l, t, r - l, b - t}.intersected(preview.rect());
        for (int y = mapped.y; y < mapped.bottom(); ++y) std::fill_n(preview.row(y) + mapped.x, size_t(mapped.w), 0xFF000000u);
    }
    blurInPlace(preview, preview.rect(), std::max(1, int(std::lround(shape.strength * std::min(sx, sy)))));
    return preview;
}
}
