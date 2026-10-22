#include "core/Recording.h"
#include <algorithm>
#include <cmath>
#include <map>

namespace putgd {
namespace {
using json::Object; using json::Array; using json::Value;
Error invalid(const std::string& message, const std::string& field = {}) {
    Object details;
    if (!field.empty()) details["field"] = field;
    return {"invalid_request", message, details};
}
bool in(const std::vector<std::string>& list, const std::string& v) { return std::find(list.begin(), list.end(), v) != list.end(); }
bool integer(const Value& value, long long min, long long max, long long* out = nullptr) {
    if (!json::integer(value, double(min), double(max))) return false;
    if (out) *out = value.toInteger();
    return true;
}
bool rect(const Value& value, RecordingRect& out) {
    if (!value.isArray() || value.arrayRef().size() != 4) return false;
    long long parts[4];
    for (int i = 0; i < 4; ++i) if (!integer(value[size_t(i)], -1000000, 1000000, &parts[i])) return false;
    out = {parts[0], parts[1], parts[2], parts[3]};
    return out.w >= 1 && out.h >= 1;
}
bool knownKeys(const Object& object, const std::vector<std::string>& allowed) {
    for (const auto& pair : object) if (!in(allowed, pair.first)) return false;
    return true;
}
std::string upper(std::string s) { for (auto& c : s) if (c >= 'a' && c <= 'z') c = char(c - 32); return s; }
}

Object RecordingTarget::toJson() const {
    Object o{{"kind", kind}};
    if (!identity.empty()) o["identity"] = identity;
    if (kind == "region" && rect && !rect->empty()) o["rect"] = rect->toJson();
    return o;
}
std::string RecordingConfig::mime() const {
    if (format == "mp4") return "video/mp4";
    if (format == "webm") return "video/webm";
    if (format == "gif") return "image/gif";
    return "image/webp";
}
Object RecordingConfig::toJson() const {
    Object o{{"format", format}, {"codec", codec}, {"fps", fps}, {"audio", Object{{"system", audioSystem}, {"microphone", audioMicrophone}}}, {"mime", mime()}};
    if (maxEdge) o["maxEdge"] = *maxEdge;
    if (budgetBytes > 0) o["budgetBytes"] = double(budgetBytes);
    return o;
}
Object RecordingMask::toJson() const {
    Object o{{"id", id}, {"kind", kind}, {"rect", rect.toJson()}, {"start", double(start)}, {"end", double(end)}};
    if (kind == "blur") o["strength"] = strength;
    return o;
}
std::optional<std::pair<long long, long long>> RecordingEdit::exportInterval(const RecordingMask& mask, long long trimStart, long long trimEnd) {
    const auto start = std::max(mask.start, trimStart), end = std::min(mask.end, trimEnd);
    if (end <= start) return std::nullopt;
    return std::make_pair(start - trimStart, end - trimStart);
}
Object RecordingEdit::toJson() const {
    Array array;
    for (const auto& mask : masks) array.push_back(mask.toJson());
    return {{"revision", double(revision)}, {"trimStart", double(trimStart)}, {"trimEnd", trimEnd ? Value(double(*trimEnd)) : Value()}, {"masks", array}};
}

namespace recording {
const std::vector<std::string>& states() { static const std::vector<std::string> v{"preparing", "ready", "starting", "recording", "paused", "stopping", "reviewing", "exporting", "submitted", "cancelled", "failed"}; return v; }
const std::vector<std::string>& targetKinds() { static const std::vector<std::string> v{"monitor", "desktop", "region", "window"}; return v; }
const std::vector<std::string>& formats() { static const std::vector<std::string> v{"mp4", "webm", "gif", "webp"}; return v; }
std::vector<std::string> codecs(const std::string& format) {
    if (format == "mp4") return {"h264", "h265"};
    if (format == "webm") return {"vp9"};
    return {};
}
bool terminal(const std::string& state) { return state == "submitted" || state == "cancelled" || state == "failed"; }
Error checkTransition(const std::string& state, const std::string& command) {
    static const std::map<std::string, std::vector<std::string>> allowed{
        {"start", {"ready"}},
        {"pause", {"recording"}},
        {"resume", {"paused"}},
        {"stop", {"recording", "paused"}},
        {"cancel", {"preparing", "ready", "starting", "recording", "paused", "stopping", "reviewing", "exporting"}},
        {"select", {"preparing"}},
        {"edit", {"reviewing"}},
        {"preview", {"reviewing"}},
        {"review", {"reviewing", "exporting"}},
        {"saveCopy", {"reviewing"}},
        {"upload", {"reviewing"}},
        {"inspect", states()},
        {"raise", {"ready", "starting", "recording", "paused", "stopping", "reviewing", "exporting"}},
        {"overlay", {"ready", "starting", "recording", "paused"}}};
    const auto it = allowed.find(command);
    if (it == allowed.end()) return {"unknown_method", "Unknown recording command", {}};
    if (!in(it->second, state)) return {"recording_state", "Cannot " + command + " a recording that is " + state, {{"state", state}, {"command", command}}};
    return {};
}
Error parseTarget(const Value& value, RecordingTarget& out) {
    if (!value.isObject()) return invalid("A recording target must be an object", "target");
    const auto& object = value.objectRef();
    if (!knownKeys(object, {"kind", "identity", "rect"})) return invalid("Unknown recording target field", "target");
    RecordingTarget target;
    target.kind = object.value("kind").toString();
    if (!in(targetKinds(), target.kind)) return invalid("Target kind must be monitor, desktop, region or window", "target.kind");
    if (object.contains("identity")) {
        const auto& identity = object.value("identity");
        if (!identity.isString() || identity.stringRef().empty() || identity.stringRef().size() > 256) return invalid("Target identity must be a nonempty string", "target.identity");
        target.identity = identity.stringRef();
    }
    if (target.kind == "region") {
        if (object.contains("rect")) { RecordingRect r; if (!rect(object.value("rect"), r)) return invalid("A region rect is [x, y, width, height] with positive size", "target.rect"); target.rect = r; }
    } else if (object.contains("rect")) return invalid("Only region targets take a rect", "target.rect");
    if ((target.kind == "desktop" || target.kind == "region") && !target.identity.empty()) return invalid("Desktop and region targets take no identity", "target.identity");
    out = target;
    return {};
}
Error parseConfig(const Object& params, RecordingConfig& out) {
    if (!knownKeys(params, {"format", "codec", "fps", "audio", "maxEdge"})) return invalid("Unknown recording configuration field");
    RecordingConfig config;
    config.format = params.value("format").toString();
    if (!in(formats(), config.format)) return invalid("Format must be mp4, webm, gif or webp", "format");
    const auto available = codecs(config.format);
    const auto& codec = params.value("codec");
    // A serialized config round-trips an empty codec for GIF/WebP.
    if (params.contains("codec") && !(codec.isString() && codec.stringRef().empty() && available.empty())) {
        if (!codec.isString() || !in(available, codec.stringRef())) return invalid("Codec is not valid for this format", "codec");
        config.codec = codec.stringRef();
    } else config.codec = available.empty() ? std::string() : available.front();
    long long fps = 30;
    if (params.contains("fps") && !integer(params.value("fps"), 1, 60, &fps)) return invalid("fps must be an integer from 1 to 60", "fps");
    config.fps = int(fps);
    if (params.contains("audio")) {
        const auto& audio = params.value("audio");
        if (!audio.isObject() || !knownKeys(audio.objectRef(), {"system", "microphone"})) return invalid("audio takes system and microphone booleans", "audio");
        if (audio.objectRef().contains("system") && !audio["system"].isBool()) return invalid("audio.system must be a boolean", "audio.system");
        if (audio.objectRef().contains("microphone") && !audio["microphone"].isBool()) return invalid("audio.microphone must be a boolean", "audio.microphone");
        config.audioSystem = audio["system"].toBool();
        config.audioMicrophone = audio["microphone"].toBool();
        if (config.audioMicrophone) return {"recording_unsupported", "Microphone capture is not offered by this version", {{"field", "audio.microphone"}}};
        if (config.animation() && config.audioSystem) return invalid("GIF and WebP are silent formats; audio cannot be included", "audio.system");
    }
    if (params.contains("maxEdge")) {
        long long edge = 0;
        if (!integer(params.value("maxEdge"), 16, 8192, &edge)) return invalid("maxEdge must be an integer from 16 to 8192", "maxEdge");
        config.maxEdge = int(edge);
    }
    out = config;
    return {};
}
long long alignToFrame(long long us, int fps) {
    if (fps < 1) return us;
    const double frame = 1000000.0 / fps;
    return std::llround(double(std::llround(double(us) / frame)) * frame);
}
bool parseEdit(const Object& o, RecordingEdit& out) {
    RecordingEdit edit;
    long long revision = 0;
    if (!integer(o.value("revision"), 1, 4503599627370496LL, &revision) || !integer(o.value("trimStart"), 0, maximumClipUs, &edit.trimStart) || !o.value("masks").isArray()) return false;
    edit.revision = (unsigned long long)revision;
    if (!o.value("trimEnd").isNull()) { long long end = 0; if (!integer(o.value("trimEnd"), 1, maximumClipUs, &end)) return false; edit.trimEnd = end; }
    if (o.value("masks").arrayRef().size() > size_t(maximumMasks)) return false;
    for (const auto& value : o.value("masks").arrayRef()) {
        if (!value.isObject()) return false;
        const auto& m = value.objectRef();
        RecordingMask mask;
        mask.id = m.value("id").toString(); mask.kind = m.value("kind").toString();
        if (mask.id.empty() || (mask.kind != "blur" && mask.kind != "solid") || !rect(m.value("rect"), mask.rect)) return false;
        if (!integer(m.value("start"), 0, maximumClipUs, &mask.start) || !integer(m.value("end"), 0, maximumClipUs, &mask.end) || mask.end <= mask.start) return false;
        long long strength = 16;
        if (m.contains("strength") && !integer(m.value("strength"), 2, 64, &strength)) return false;
        mask.strength = int(strength);
        edit.masks.push_back(mask);
    }
    out = edit;
    return true;
}
namespace {
Error validateMask(const Value& value, RecordingMask& mask, long long duration, RecordingSize canvas) {
    if (!value.isObject()) return invalid("A mask must be an object", "mask");
    const auto& object = value.objectRef();
    if (!knownKeys(object, {"kind", "rect", "start", "end", "strength"})) return invalid("Unknown mask field", "mask");
    RecordingMask result = mask;
    result.kind = object.value("kind").toString("blur");
    if (result.kind != "blur" && result.kind != "solid") return invalid("Mask kind must be blur or solid", "mask.kind");
    if (!rect(object.value("rect"), result.rect)) return invalid("A mask needs rect [x, y, width, height] with positive size", "mask.rect");
    if (!canvas.empty() && (result.rect.x < 0 || result.rect.y < 0 || result.rect.x + result.rect.w > canvas.w || result.rect.y + result.rect.h > canvas.h))
        return invalid("Mask rect must lie inside the output canvas", "mask.rect");
    if (!integer(object.value("start"), 0, duration, &result.start) || !integer(object.value("end"), 0, duration, &result.end) || result.end <= result.start)
        return invalid("Mask start/end must be microseconds within the clip with end after start", "mask.start");
    long long strength = 16;
    if (object.contains("strength") && !integer(object.value("strength"), 2, 64, &strength)) return invalid("Mask strength must be 2 to 64", "mask.strength");
    result.strength = int(strength);
    mask = result;
    return {};
}
}
Error applyEditCommands(RecordingEdit& edit, const Array& commands, long long duration, RecordingSize canvas, int fps, const std::function<std::string()>& newId) {
    if (commands.empty() || commands.size() > size_t(maximumEditCommands)) return invalid("Send 1 to 100 edit commands", "commands");
    RecordingEdit next = edit;
    const long long minimum = fps > 0 ? 1000000 / fps : 1;
    for (const auto& value : commands) {
        if (!value.isObject()) return invalid("Each command must be an object", "commands");
        const auto& command = value.objectRef();
        const auto op = command.value("op").toString();
        if (op == "setTrim") {
            if (!knownKeys(command, {"op", "start", "end"})) return invalid("setTrim takes start and end", "commands");
            long long start = 0, end = duration;
            if (!integer(command.value("start"), 0, duration, &start)) return invalid("Trim start must be microseconds within the clip", "start");
            const bool hasEnd = command.contains("end") && !command.value("end").isNull();
            if (hasEnd && !integer(command.value("end"), 0, duration, &end)) return invalid("Trim end must be microseconds within the clip or null", "end");
            if (end - start < minimum) return invalid("The retained interval must contain at least one frame", "end");
            next.trimStart = start;
            if (hasEnd && end != duration) next.trimEnd = end; else next.trimEnd.reset();
        } else if (op == "resetTrim") {
            if (!knownKeys(command, {"op"})) return invalid("resetTrim takes no fields", "commands");
            next.trimStart = 0; next.trimEnd.reset();
        } else if (op == "addMask") {
            if (!knownKeys(command, {"op", "mask"})) return invalid("addMask takes a mask", "commands");
            if (next.masks.size() >= size_t(maximumMasks)) return invalid("At most 64 masks are supported", "mask");
            RecordingMask mask;
            if (auto error = validateMask(command.value("mask"), mask, duration, canvas)) return error;
            mask.id = newId();
            next.masks.push_back(mask);
        } else if (op == "updateMask" || op == "removeMask") {
            const std::vector<std::string> keys = op == "updateMask" ? std::vector<std::string>{"op", "maskId", "mask"} : std::vector<std::string>{"op", "maskId"};
            if (!knownKeys(command, keys) || !command.value("maskId").isString()) return invalid("Mask commands need a maskId", "maskId");
            const auto id = command.value("maskId").stringRef();
            const auto found = std::find_if(next.masks.begin(), next.masks.end(), [&](const RecordingMask& m) { return m.id == id; });
            if (found == next.masks.end()) return {"recording_mask_unknown", "No mask has this id", {{"maskId", id}}};
            if (op == "removeMask") next.masks.erase(found);
            else if (auto error = validateMask(command.value("mask"), *found, duration, canvas)) return error;
        } else return invalid("Edit op must be setTrim, resetTrim, addMask, updateMask or removeMask", "op");
    }
    next.revision = edit.revision + 1;
    edit = next;
    return {};
}
RecordingSize outputSize(RecordingSize canvas, const std::optional<int>& maxEdge) {
    if (!maxEdge || canvas.empty()) return canvas;
    const int edge = std::max(canvas.w, canvas.h);
    if (edge <= *maxEdge) return canvas;
    const double scale = double(*maxEdge) / edge;
    auto even = [](double v) { return std::max(2, int(std::lround(v / 2.0)) * 2); };
    return {even(canvas.w * scale), even(canvas.h * scale)};
}
Error checkExportBudget(const RecordingConfig& config, const RecordingEdit& edit, long long duration, RecordingSize canvas, const RecordingLimits& limits) {
    const auto retained = edit.exportDuration(duration);
    if (retained <= 0) return invalid("The retained interval is empty", "trim");
    if (!config.animation()) return {};
    const auto format = upper(config.format);
    if (retained > limits.animationMaxUs) {
        const auto seconds = std::to_string(limits.animationMaxUs / 1000000);
        return {"recording_animation_too_long", format + " exports are limited to " + seconds + " s; trim the clip to " + seconds + " s or less", {{"retainedUs", double(retained)}, {"limitUs", double(limits.animationMaxUs)}}};
    }
    const auto size = outputSize(canvas, config.maxEdge);
    const int edge = std::max(size.w, size.h);
    if (edge > limits.animationMaxEdge) {
        const auto limit = std::to_string(limits.animationMaxEdge);
        return {"recording_animation_too_large", format + " exports are limited to " + limit + " px; set maxEdge to " + limit + " or less", {{"edge", edge}, {"limitEdge", limits.animationMaxEdge}}};
    }
    return {};
}
bool clampAnimationSize(RecordingConfig& config, RecordingSize canvas, const RecordingLimits& limits) {
    if (!config.animation() || canvas.empty()) return false;
    const auto size = outputSize(canvas, config.maxEdge);
    if (std::max(size.w, size.h) <= limits.animationMaxEdge) return false;
    config.maxEdge = limits.animationMaxEdge;
    return true;
}
RecordingRect evenArea(RecordingRect r) { r.w -= r.w % 2; r.h -= r.h % 2; return r; }
long long liveBitrate(int width, int height, int fps) {
    const double bits = 0.1 * double(width) * double(height) * double(std::max(1, fps));
    return std::clamp<long long>((long long)bits, 1500000, 20000000);
}
}
}
