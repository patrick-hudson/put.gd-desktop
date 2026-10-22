#pragma once
// System audio for the recording helper, the parts with no Windows headers (tested
// on Linux): converting a device's mix format to the 16-bit stereo the AAC encoder
// takes, a linear resampler for device rates the encoder does not offer, and the
// placement of capture packets on the clip timeline. Loopback capture delivers
// nothing while nothing plays, and packets can overlap after a glitch; the
// timeline turns that into contiguous samples: silence for gaps, overlaps cut.
#include <cstdint>
#include <vector>

namespace putgd::audio {
// Rates Media Foundation's AAC encoder accepts.
bool encoderRate(int rate);
// Interleaved float or 16-bit samples with any channel count to interleaved 16-bit
// stereo: mono is duplicated, extra channels are dropped (front left/right first).
void toStereo16(const float* in, size_t frames, int channels, std::vector<int16_t>& out);
void toStereo16(const int16_t* in, size_t frames, int channels, std::vector<int16_t>& out);

// Linear interpolation from one rate to another on interleaved stereo, keeping its
// phase across calls so consecutive packets join without clicks.
class Resampler {
public:
    Resampler(int from, int to) : from_(from), to_(to) {}
    void process(const std::vector<int16_t>& in, std::vector<int16_t>& out);
private:
    int from_, to_;
    double position_ = 0; // in input frames, relative to the start of the next call
    int16_t last_[2] = {0, 0};
    bool primed_ = false;
};

// Samples on the clip timeline. A packet starting at clip time `start` (in
// frames at `rate`) after the samples written so far gets silence before it; one
// starting earlier loses the part already covered.
class Timeline {
public:
    explicit Timeline(int rate) : rate_(rate) {}
    // Returns the stereo frames to append (silence first), and where they start.
    struct Piece { long long start = 0; std::vector<int16_t> samples; };
    Piece place(long long startFrame, const std::vector<int16_t>& stereo);
    // Silence up to `frame` (when nothing plays, and at the end of the clip).
    Piece fillTo(long long frame);
    long long written() const { return written_; }
    int rate() const { return rate_; }
    static long long frameAt(long long time100ns, int rate) { return (time100ns * rate + 5000000) / 10000000; }
private:
    int rate_;
    long long written_ = 0;
};
}
