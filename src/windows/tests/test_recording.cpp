// Portable checks of the Windows recording contract (core/Recording), the same
// rules the Linux recording_model_contract holds the resident to.
#include "core/Recording.h"
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

int main() {
    targetsAndConfigs();
    transitions();
    edits();
    sizesAndBudgets();
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("recording contract ok\n");
    return 0;
}
