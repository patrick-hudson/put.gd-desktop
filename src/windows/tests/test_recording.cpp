// Portable checks of the Windows recording contract (core/Recording), the same
// rules the Linux recording_model_contract holds the resident to.
#include "core/Recording.h"
#include "core/RecordingFrames.h"
#include "core/Mp4.h"
#include "core/BarPlacement.h"
#include "core/ReviewTimeline.h"
#include "core/Audio.h"
#include <vector>
#include <cstdio>
#include <string>

using namespace putgd;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static json::Object object(const char* text) { return *json::parseObject(text); }

static void targetsAndConfigs() {
    RecordingTarget t;
    CHECK(!recording::parseTarget(object(R"({"kind":"region","rect":[10,20,300,200]})"), t));
    CHECK(t.kind == "region" && t.rect && *t.rect == (RecordingRect{10, 20, 300, 200}));
    CHECK(json::serialize(t.toJson()) == R"({"kind":"region","rect":[10,20,300,200]})");
    CHECK(!recording::parseTarget(object(R"({"kind":"region"})"), t) && !t.rect);
    CHECK(!recording::parseTarget(object(R"({"kind":"monitor","identity":"1"})"), t) && t.identity == "1");
    CHECK(recording::parseTarget(object(R"({"kind":"desktop","rect":[0,0,1,1]})"), t).code == "invalid_request");
    CHECK(recording::parseTarget(object(R"({"kind":"region","rect":[0,0,0,5]})"), t).code == "invalid_request");
    CHECK(recording::parseTarget(object(R"({"kind":"region","identity":"x"})"), t).code == "invalid_request");
    CHECK(recording::parseTarget(object(R"({"kind":"screen"})"), t).code == "invalid_request");
    CHECK(recording::parseTarget(json::Value("region"), t).code == "invalid_request");

    RecordingConfig c;
    CHECK(!recording::parseConfig(object(R"({"format":"mp4"})"), c) && c.codec == "h264" && c.fps == 30 && c.mime() == "video/mp4");
    CHECK(!recording::parseConfig(object(R"({"format":"mp4","codec":"h265","fps":60})"), c) && c.codec == "h265" && c.fps == 60);
    CHECK(!recording::parseConfig(object(R"({"format":"gif","codec":""})"), c) && c.codec.empty() && c.animation() && c.mime() == "image/gif");
    CHECK(recording::parseConfig(object(R"({"format":"mp4","codec":"vp9"})"), c).code == "invalid_request");
    CHECK(recording::parseConfig(object(R"({"format":"mov"})"), c).code == "invalid_request");
    CHECK(recording::parseConfig(object(R"({"format":"mp4","fps":0})"), c).code == "invalid_request");
    CHECK(recording::parseConfig(object(R"({"format":"mp4","fps":61})"), c).code == "invalid_request");
    CHECK(recording::parseConfig(object(R"({"format":"mp4","audio":{"microphone":true}})"), c).code == "recording_unsupported");
    CHECK(recording::parseConfig(object(R"({"format":"gif","audio":{"system":true}})"), c).code == "invalid_request");
    CHECK(recording::parseConfig(object(R"({"format":"mp4","extra":1})"), c).code == "invalid_request");
    CHECK(!recording::parseConfig(object(R"({"format":"webm","audio":{"system":true},"maxEdge":640})"), c) && c.audioSystem && c.maxEdge == 640);
    // The serialized config round-trips through parseConfig once the resident's fields are removed.
    auto round = c.toJson(); round.remove("mime"); round.remove("budgetBytes");
    RecordingConfig again;
    CHECK(!recording::parseConfig(round, again) && json::serialize(again.toJson()) == json::serialize(c.toJson()));
    // Frame slots are centred on their instants: 30 fps, period 333333 (100 ns).
    CHECK(recording::frameSlot(-5000000, 333333) == 0 && recording::frameSlot(0, 333333) == 0);
    CHECK(recording::frameSlot(166666, 333333) == 0 && recording::frameSlot(166667, 333333) == 1);
    CHECK(recording::frameSlot(333333, 333333) == 1 && recording::frameSlot(499999, 333333) == 1 && recording::frameSlot(500000, 333333) == 2);
    CHECK(recording::frameSlot(10000000, 333333) == 30 && recording::frameSlot(123, 0) == 0);
    // The bar's and the review's menu keys.
    CHECK(recording::formatKey(again) == "webm/vp9" && recording::formatLabel("webm/vp9") == "WebM (VP9)");
    CHECK(!recording::parseConfig(object(R"({"format":"gif","codec":""})"), c) && recording::formatKey(c) == "gif" && recording::formatLabel("gif") == "GIF");
    CHECK(recording::formatLabel("mp4/h265") == "MP4 (H.265)" && recording::formatLabel("mp4") == "MP4 (H.264)");
}

static void transitions() {
    CHECK(!recording::checkTransition("ready", "start"));
    CHECK(recording::checkTransition("recording", "start").code == "recording_state");
    CHECK(!recording::checkTransition("paused", "stop"));
    CHECK(!recording::checkTransition("reviewing", "upload"));
    CHECK(recording::checkTransition("exporting", "upload").code == "recording_state");
    CHECK(!recording::checkTransition("exporting", "cancel"));
    CHECK(recording::checkTransition("submitted", "cancel").code == "recording_state");
    CHECK(!recording::checkTransition("failed", "inspect"));
    CHECK(recording::checkTransition("ready", "jump").code == "unknown_method");
    CHECK(recording::terminal("submitted") && recording::terminal("cancelled") && !recording::terminal("reviewing"));
}

static void edits() {
    const long long duration = 30000000; // 30 s
    const RecordingSize canvas{1280, 720};
    int next = 0;
    auto id = [&] { return "mask-" + std::to_string(++next); };
    RecordingEdit edit;
    CHECK(edit.unedited(duration));
    auto commands = json::parse(R"([{"op":"setTrim","start":10000000,"end":20000000},
        {"op":"addMask","mask":{"kind":"blur","rect":[0,0,100,100],"start":12000000,"end":15000000}}])").toArray();
    CHECK(!recording::applyEditCommands(edit, commands, duration, canvas, 30, id));
    CHECK(edit.revision == 2 && edit.trimStart == 10000000 && edit.trimEnd == 20000000 && edit.masks.size() == 1 && edit.masks[0].id == "mask-1");
    CHECK(!edit.unedited(duration));
    // A mask at 12-15 s with a 10-30 s trim is active at output 2-5 s.
    const auto interval = RecordingEdit::exportInterval(edit.masks[0], 10000000, 30000000);
    CHECK(interval && interval->first == 2000000 && interval->second == 5000000);
    CHECK(!RecordingEdit::exportInterval(edit.masks[0], 16000000, 30000000));
    // Atomic: a failing command leaves the edit and its revision unchanged.
    const auto before = json::serialize(edit.toJson());
    auto bad = json::parse(R"([{"op":"resetTrim"},{"op":"addMask","mask":{"rect":[1200,0,100,100],"start":0,"end":1}}])").toArray();
    CHECK(recording::applyEditCommands(edit, bad, duration, canvas, 30, id).code == "invalid_request");
    CHECK(json::serialize(edit.toJson()) == before);
    auto unknown = json::parse(R"([{"op":"removeMask","maskId":"nope"}])").toArray();
    CHECK(recording::applyEditCommands(edit, unknown, duration, canvas, 30, id).code == "recording_mask_unknown");
    auto tooShort = json::parse(R"([{"op":"setTrim","start":0,"end":10000}])").toArray();
    CHECK(recording::applyEditCommands(edit, tooShort, duration, canvas, 30, id).code == "invalid_request");
    auto reset = json::parse(R"([{"op":"resetTrim"},{"op":"removeMask","maskId":"mask-1"}])").toArray();
    CHECK(!recording::applyEditCommands(edit, reset, duration, canvas, 30, id) && edit.unedited(duration) && edit.revision == 3);
    // Trim end at the clip end is stored as null.
    auto toEnd = json::parse(R"([{"op":"setTrim","start":0,"end":30000000}])").toArray();
    CHECK(!recording::applyEditCommands(edit, toEnd, duration, canvas, 30, id) && !edit.trimEnd);
    CHECK(json::serialize(edit.toJson()) == R"({"masks":[],"revision":4,"trimEnd":null,"trimStart":0})");
    // The helper reads the resident's edit back exactly.
    auto masked = json::parse(R"([{"op":"setTrim","start":1000000,"end":9000000},{"op":"addMask","mask":{"kind":"solid","rect":[1,2,30,40],"start":2000000,"end":3000000}}])").toArray();
    CHECK(!recording::applyEditCommands(edit, masked, duration, canvas, 30, id));
    RecordingEdit parsed;
    CHECK(recording::parseEdit(edit.toJson(), parsed) && json::serialize(parsed.toJson()) == json::serialize(edit.toJson()));
    auto broken = edit.toJson(); broken["masks"] = json::Array{json::Object{{"id", "x"}, {"kind", "blur"}, {"rect", json::Array{0, 0, 0, 1}}, {"start", 0}, {"end", 1}}};
    CHECK(!recording::parseEdit(broken, parsed));
}

static void sizesAndBudgets() {
    CHECK(recording::alignToFrame(1000000 / 30 * 10 + 10, 30) == 333333);
    CHECK(recording::alignToFrame(50000, 0) == 50000);
    CHECK(recording::evenArea({0, 0, 1121, 681}) == (RecordingRect{0, 0, 1120, 680}));
    const auto scaled = recording::outputSize({1920, 1080}, 1280);
    CHECK(scaled.w == 1280 && scaled.h == 720);
    CHECK(recording::outputSize({800, 600}, 1280).w == 800);
    RecordingConfig gif; gif.format = "gif"; gif.codec.clear();
    RecordingEdit edit; RecordingLimits limits;
    CHECK(recording::checkExportBudget(gif, edit, 61000000, {640, 480}, limits).code == "recording_animation_too_long");
    CHECK(recording::checkExportBudget(gif, edit, 10000000, {1920, 1080}, limits).code == "recording_animation_too_large");
    CHECK(recording::clampAnimationSize(gif, {1920, 1080}, limits) && gif.maxEdge == 1280);
    CHECK(!recording::checkExportBudget(gif, edit, 10000000, {1920, 1080}, limits));
    RecordingConfig mp4;
    CHECK(!recording::checkExportBudget(mp4, edit, 61000000, {3840, 2160}, limits));
    CHECK(recording::liveBitrate(1280, 720, 30) == 2764800);
    CHECK(recording::liveBitrate(64, 64, 1) == 1500000 && recording::liveBitrate(7680, 4320, 60) == 20000000);
}

static std::vector<uint8_t> checker(int w, int h) {
    std::vector<uint8_t> bits(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) { uint8_t v = ((x / 2 + y / 2) % 2) ? 255 : 0; uint8_t* p = &bits[(size_t(y) * w + x) * 4]; p[0] = p[1] = p[2] = v; p[3] = 255; }
    return bits;
}
static void frames() {
    const int w = 64, h = 48;
    auto bits = checker(w, h);
    const auto original = bits;
    recording::Frame frame{bits.data(), w, h, size_t(w) * 4};
    RecordingEdit edit;
    RecordingMask solid; solid.kind = "solid"; solid.rect = {4, 4, 8, 8}; solid.start = 1000000; solid.end = 2000000;
    RecordingMask blur; blur.kind = "blur"; blur.rect = {32, 8, 16, 16}; blur.start = 0; blur.end = 5000000; blur.strength = 16;
    edit.masks = {solid, blur};
    CHECK(recording::blurRadius(blur) == 7);
    RecordingMask narrow = blur; narrow.rect = {0, 0, 4, 100}; CHECK(recording::blurRadius(narrow) == 2);
    // At 0.5 s only the blur is active; the solid area and everything outside the blur are untouched.
    recording::applyMasks(frame, edit, 500000);
    auto at = [&](const std::vector<uint8_t>& b, int x, int y) { return b[(size_t(y) * w + x) * 4]; };
    CHECK(at(bits, 5, 5) == at(original, 5, 5));
    CHECK(at(bits, 31, 8) == at(original, 31, 8) && at(bits, 48, 8) == at(original, 48, 8) && at(bits, 40, 24) == at(original, 40, 24));
    bool smoothed = true;
    for (int y = 10; y < 22; ++y) for (int x = 34; x < 46; ++x) smoothed = smoothed && at(bits, x, y) > 40 && at(bits, x, y) < 215;
    CHECK(smoothed);
    // At 1.5 s the solid mask is an opaque black fill of exactly its rectangle.
    bits = original;
    recording::applyMasks(frame, edit, 1500000);
    bool black = true; for (int y = 4; y < 12; ++y) for (int x = 4; x < 12; ++x) black = black && at(bits, x, y) == 0;
    CHECK(black && at(bits, 12, 12) == at(original, 12, 12) && at(bits, 3, 3) == at(original, 3, 3));
    // A mask partly outside the frame is clipped, not refused.
    RecordingEdit edge; RecordingMask out = solid; out.rect = {60, 44, 10, 10}; edge.masks = {out};
    recording::applyMasks(frame, edge, 1500000);
    CHECK(at(bits, 63, 47) == 0);
    // Area-average downscale of a 2x2 checker cell pattern averages to grey.
    auto source = checker(64, 48);
    std::vector<uint8_t> small(size_t(16) * 12 * 4);
    recording::resample({source.data(), 64, 48, 256}, {small.data(), 16, 12, 64});
    CHECK(small[0] == 127 && small[3] == 255);
    // The export keeps the trim aligned to the frame grid.
    RecordingEdit trim; trim.trimStart = 1010000; trim.trimEnd = 3990000;
    const auto window = recording::exportWindow(trim, 8266658, 30);
    CHECK(window.first == 1000000 && window.second == 4000000);
    const auto all = recording::exportWindow(RecordingEdit{}, 8266658, 30);
    CHECK(all.first == 0 && all.second == 8266658);
}

static std::string box(const std::string& type, const std::string& body) {
    const uint32_t size = uint32_t(body.size() + 8);
    std::string out{char(size >> 24), char(size >> 16), char(size >> 8), char(size)};
    return out + type + body;
}
static std::string u32(uint32_t v) { return {char(v >> 24), char(v >> 16), char(v >> 8), char(v)}; }
static void mp4Boxes() {
    const std::string stco = box("stco", u32(0) + u32(2) + u32(64) + u32(1000));
    const std::string co64 = box("co64", u32(0) + u32(1) + u32(0) + u32(4000000000u));
    const std::string moov = box("moov", box("mvhd", std::string(20, '\0')) + box("trak", box("mdia", box("minf", box("stbl", stco)))) + box("trak", box("mdia", box("minf", box("stbl", co64)))));
    std::string shifted = moov;
    CHECK(mp4::shiftChunkOffsets(shifted, 100));
    CHECK(shifted.size() == moov.size() && shifted.find(u32(164) + u32(1100)) != std::string::npos && shifted.find(u32(0) + u32(4000000100u)) != std::string::npos);
    // A 32-bit offset that would overflow leaves the box unchanged.
    std::string big = box("moov", box("trak", box("mdia", box("minf", box("stbl", box("stco", u32(0) + u32(1) + u32(0xFFFFFFF0u)))))));
    const auto before = big;
    CHECK(!mp4::shiftChunkOffsets(big, 100) && big == before);
    std::string truncated = moov.substr(0, moov.size() - 3);
    CHECK(!mp4::shiftChunkOffsets(truncated, 1));
    const std::string file = box("ftyp", "isom") + box("mdat", std::string(50, 'x')) + moov;
    auto read = [&](uint64_t at, size_t n) { return file.substr(size_t(at), n); };
    const auto boxes = mp4::topLevel(file.size(), read);
    CHECK(boxes.size() == 3 && boxes[1].type == "mdat" && boxes[1].offset == 12 && boxes[2].type == "moov" && boxes[2].size == moov.size());
    CHECK(mp4::topLevel(file.size() + 4, read).empty());
}

static void barAndTimeline() {
    const std::vector<RecordingRect> one{{0, 0, 1280, 800}};
    // Below the region when there is room, kept on the screen horizontally.
    auto spot = recording::placeBar(600, 44, {60, 60, 800, 500}, one);
    CHECK(spot.outside && spot.rect == (RecordingRect{60, 570, 600, 44}) && spot.screen == 0);
    spot = recording::placeBar(600, 44, {900, 60, 300, 500}, one);
    CHECK(spot.outside && spot.rect.x == 680);
    // Above when the region reaches the bottom.
    spot = recording::placeBar(600, 44, {60, 300, 800, 480}, one);
    CHECK(spot.outside && spot.rect.y == 300 - 10 - 44);
    // A whole monitor: inside its bottom-right corner.
    spot = recording::placeBar(600, 44, {0, 0, 1280, 800}, one);
    CHECK(!spot.outside && spot.rect == (RecordingRect{1280 - 10 - 600, 800 - 10 - 44, 600, 44}));
    // A second monitor takes the bar when the first is full.
    const std::vector<RecordingRect> two{{0, 0, 1280, 800}, {1280, 0, 1920, 1080}};
    spot = recording::placeBar(600, 44, {0, 0, 1280, 800}, two);
    CHECK(spot.outside && spot.screen == 1 && spot.rect == (RecordingRect{1280 + 960 - 300, 10, 600, 44}));
    CHECK(recording::homeScreen({1200, 0, 400, 100}, two) == 1);
    CHECK(recording::clampToScreen({1250, 790, 100, 50}, one[0]) == (RecordingRect{1180, 750, 100, 50}));

    std::vector<RecordingMask> masks(5);
    const long long spans[5][2] = {{0, 10}, {5, 15}, {10, 20}, {6, 8}, {7, 9}};
    for (int i = 0; i < 5; ++i) { masks[size_t(i)].start = spans[i][0]; masks[size_t(i)].end = spans[i][1]; }
    const auto lanes = recording::maskLanes(masks);
    CHECK(lanes == (std::vector<int>{0, 1, 0, 2, 3}));
    masks.push_back(masks[4]);
    CHECK(recording::maskLanes(masks).back() == 3); // a fifth overlapping mask shares the last lane
    CHECK(recording::timeAt(100, 100, 501, 10000000) == 0 && recording::timeAt(600, 100, 501, 10000000) == 10000000 && recording::timeAt(350, 100, 501, 10000000) == 5000000);
    CHECK(recording::timeAt(50, 100, 501, 10000000) == 0 && recording::timeAt(900, 100, 501, 10000000) == 10000000);
    CHECK(recording::xAt(5000000, 100, 501, 10000000) == 350 && recording::xAt(20000000, 100, 501, 10000000) == 600);
    CHECK(recording::clockText(8266658) == "0:08.26" && recording::clockText(14700000, true) == "0:14.7" && recording::clockText(61230000) == "1:01.23");
}

static void audioTimeline() {
    std::vector<int16_t> out;
    const float mono[] = {0.5f, -1.5f};
    audio::toStereo16(mono, 2, 1, out);
    CHECK(out == (std::vector<int16_t>{16384, 16384, -32767, -32767}));
    const float six[] = {0.25f, -0.25f, 1, 1, 1, 1};
    audio::toStereo16(six, 1, 6, out);
    CHECK(out == (std::vector<int16_t>{8192, -8192}));
    const int16_t pcm[] = {1, 2, 3, 4};
    audio::toStereo16(pcm, 2, 2, out);
    CHECK(out == (std::vector<int16_t>{1, 2, 3, 4}));
    CHECK(audio::encoderRate(48000) && audio::encoderRate(44100) && !audio::encoderRate(96000));
    // Same rate: unchanged. Half rate: half the frames, continuous across calls.
    std::vector<int16_t> ramp;
    for (int i = 0; i < 100; ++i) { ramp.push_back(int16_t(i * 10)); ramp.push_back(int16_t(-i * 10)); }
    audio::Resampler same(48000, 48000);
    same.process(ramp, out);
    CHECK(out == ramp);
    audio::Resampler half(96000, 48000);
    std::vector<int16_t> first(ramp.begin(), ramp.begin() + 100), second(ramp.begin() + 100, ramp.end()), a, b;
    half.process(first, a); half.process(second, b);
    CHECK(a.size() / 2 + b.size() / 2 == 50);
    a.insert(a.end(), b.begin(), b.end());
    bool steps = true;
    for (size_t i = 2; i < a.size(); i += 2) steps = steps && a[i] - a[i - 2] == 20;
    CHECK(steps && a[0] == 0);
    // The timeline: a gap becomes silence, an overlap is cut, the end is filled.
    audio::Timeline line(48000);
    const std::vector<int16_t> tone(20, 7); // 10 stereo frames
    auto piece = line.place(0, tone);
    CHECK(piece.start == 0 && piece.samples.size() == 20 && line.written() == 10);
    piece = line.place(15, tone);
    CHECK(piece.start == 10 && piece.samples.size() == 30 && piece.samples[0] == 0 && piece.samples[9] == 0 && piece.samples[10] == 7 && line.written() == 25);
    piece = line.place(20, tone);
    CHECK(piece.start == 25 && piece.samples.size() == 10 && line.written() == 30);
    piece = line.place(5, tone);
    CHECK(piece.samples.empty() && line.written() == 30);
    piece = line.fillTo(40);
    CHECK(piece.start == 30 && piece.samples.size() == 20 && line.written() == 40);
    CHECK(audio::Timeline::frameAt(10000000, 48000) == 48000 && audio::Timeline::frameAt(333333, 48000) == 1600);
}

int main() {
    frames();
    audioTimeline();
    mp4Boxes();
    barAndTimeline();
    targetsAndConfigs();
    transitions();
    edits();
    sizesAndBudgets();
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("recording contract ok\n");
    return 0;
}
