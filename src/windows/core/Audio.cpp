#include "core/Audio.h"
#include <algorithm>
#include <cmath>

namespace putgd::audio {
bool encoderRate(int rate) { return rate == 44100 || rate == 48000; }
void toStereo16(const float* in, size_t frames, int channels, std::vector<int16_t>& out) {
    out.resize(frames * 2);
    auto convert = [](float v) { return int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f)); };
    for (size_t i = 0; i < frames; ++i) {
        const float* f = in + i * size_t(channels);
        out[i * 2] = convert(f[0]);
        out[i * 2 + 1] = convert(channels > 1 ? f[1] : f[0]);
    }
}
void toStereo16(const int16_t* in, size_t frames, int channels, std::vector<int16_t>& out) {
    out.resize(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        const int16_t* f = in + i * size_t(channels);
        out[i * 2] = f[0];
        out[i * 2 + 1] = channels > 1 ? f[1] : f[0];
    }
}
void Resampler::process(const std::vector<int16_t>& in, std::vector<int16_t>& out) {
    out.clear();
    const size_t frames = in.size() / 2;
    if (frames == 0) return;
    if (!primed_) { last_[0] = in[0]; last_[1] = in[1]; primed_ = true; position_ = 1; }
    // Output frame k sits at input position position_ + k * step, where position 0 is
    // the last frame of the previous call and 1 the first frame of this one.
    const double step = double(from_) / double(to_);
    auto sample = [&](long long index, int channel) { return index <= 0 ? last_[channel] : in[size_t(index - 1) * 2 + size_t(channel)]; };
    double p = position_;
    while (p <= double(frames)) {
        const long long base = (long long)std::floor(p);
        const double t = p - double(base);
        for (int c = 0; c < 2; ++c) {
            const double a = sample(base, c), b = base + 1 <= (long long)frames ? sample(base + 1, c) : a;
            out.push_back(int16_t(std::lround(a + (b - a) * t)));
        }
        p += step;
    }
    position_ = p - double(frames);
    last_[0] = in[(frames - 1) * 2]; last_[1] = in[(frames - 1) * 2 + 1];
}
Timeline::Piece Timeline::place(long long start, const std::vector<int16_t>& stereo) {
    Piece piece{written_, {}};
    const long long frames = (long long)stereo.size() / 2;
    if (start > written_) piece.samples.assign(size_t(start - written_) * 2, 0);
    const long long skip = std::max(0LL, written_ - start);
    if (skip < frames) piece.samples.insert(piece.samples.end(), stereo.begin() + skip * 2, stereo.end());
    written_ += (long long)piece.samples.size() / 2;
    return piece;
}
Timeline::Piece Timeline::fillTo(long long frame) {
    Piece piece{written_, {}};
    if (frame > written_) piece.samples.assign(size_t(frame - written_) * 2, 0);
    written_ = std::max(written_, frame);
    return piece;
}
}
