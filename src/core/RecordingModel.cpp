#include "RecordingModel.h"
#include <QHash>
#include <QJsonArray>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace putgd {
namespace {
Error invalid(const QString& message, const QString& field = {}) {
    QJsonObject details;
    if (!field.isEmpty()) details["field"] = field;
    return {"invalid_request", message, details};
}
bool integer(const QJsonValue& value, qint64 min, qint64 max, qint64* out = nullptr) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number || number < double(min) || number > double(max)) return false;
    if (out) *out = qint64(number);
    return true;
}
bool rect(const QJsonValue& value, QRect& out) {
    if (!value.isArray() || value.toArray().size() != 4) return false;
    qint64 parts[4];
    const auto array = value.toArray();
    for (int i = 0; i < 4; ++i) if (!integer(array[i], -1000000, 1000000, &parts[i])) return false;
    out = QRect(int(parts[0]), int(parts[1]), int(parts[2]), int(parts[3]));
    return out.width() >= 1 && out.height() >= 1;
}
QJsonArray rectJson(const QRect& r) { return {r.x(), r.y(), r.width(), r.height()}; }
bool knownKeys(const QJsonObject& object, const QStringList& allowed) {
    for (auto it = object.begin(); it != object.end(); ++it) if (!allowed.contains(it.key())) return false;
    return true;
}
}

QJsonObject RecordingTarget::toJson() const {
    QJsonObject o{{"kind", kind}};
    if (!identity.isEmpty()) o["identity"] = identity;
    if (kind == "region") o["rect"] = rectJson(rect);
    return o;
}
QString RecordingConfig::mime() const {
    if (format == "mp4") return "video/mp4";
    if (format == "webm") return "video/webm";
    if (format == "gif") return "image/gif";
    return "image/webp";
}
QString RecordingConfig::extension() const { return format; }
QJsonObject RecordingConfig::toJson() const {
    QJsonObject o{{"format", format}, {"codec", codec}, {"fps", fps},
        {"audio", QJsonObject{{"system", audio.system}, {"microphone", audio.microphone}}}, {"mime", mime()}};
    if (maxEdge) o["maxEdge"] = *maxEdge;
    return o;
}
QJsonObject RecordingMask::toJson() const {
    QJsonObject o{{"id", id}, {"kind", kind}, {"rect", rectJson(rect)}, {"start", double(start)}, {"end", double(end)}};
    if (kind == "blur") o["strength"] = strength;
    return o;
}
std::optional<std::pair<qint64, qint64>> RecordingEdit::exportInterval(const RecordingMask& mask, qint64 trimStart, qint64 trimEnd) {
    const auto start = std::max(mask.start, trimStart), end = std::min(mask.end, trimEnd);
    if (end <= start) return std::nullopt;
    return std::make_pair(start - trimStart, end - trimStart);
}
QJsonObject RecordingEdit::toJson() const {
    QJsonArray array;
    for (const auto& mask : masks) array.append(mask.toJson());
    return {{"revision", double(revision)}, {"trimStart", double(trimStart)},
        {"trimEnd", trimEnd ? QJsonValue(double(*trimEnd)) : QJsonValue(QJsonValue::Null)}, {"masks", array}};
}

namespace recording {
QStringList states() { return {"preparing", "ready", "starting", "recording", "paused", "stopping", "reviewing", "exporting", "submitted", "cancelled", "failed"}; }
QStringList targetKinds() { return {"monitor", "desktop", "region", "window"}; }
QStringList formats() { return {"mp4", "webm", "gif", "webp"}; }
QStringList codecs(const QString& format) {
    if (format == "mp4") return {"h264", "h265"};
    if (format == "webm") return {"vp9"};
    return {};
}
bool terminal(const QString& state) { return state == "submitted" || state == "cancelled" || state == "failed"; }
Error checkTransition(const QString& state, const QString& command) {
    static const QHash<QString, QStringList> allowed{
        {"start", {"ready"}},
        {"pause", {"recording"}},
        {"resume", {"paused"}},
        {"stop", {"recording", "paused"}},
        {"cancel", {"preparing", "ready", "starting", "recording", "paused", "stopping", "reviewing", "exporting"}},
        {"edit", {"reviewing"}},
        {"preview", {"reviewing"}},
        {"saveCopy", {"reviewing"}},
        {"upload", {"reviewing"}},
        {"inspect", states()},
        {"raise", {"ready", "starting", "recording", "paused", "stopping", "reviewing", "exporting"}},
        {"overlay", {"ready", "starting", "recording", "paused"}}};
    if (!allowed.contains(command)) return {"unknown_method", "Unknown recording command", {}};
    if (!allowed[command].contains(state)) return {"recording_state", QStringLiteral("Cannot %1 a recording that is %2").arg(command, state), {{"state", state}, {"command", command}}};
    return {};
}
Error parseTarget(const QJsonValue& value, RecordingTarget& out) {
    if (!value.isObject()) return invalid("A recording target must be an object", "target");
    const auto object = value.toObject();
    if (!knownKeys(object, {"kind", "identity", "rect"})) return invalid("Unknown recording target field", "target");
    RecordingTarget target;
    target.kind = object["kind"].toString();
    if (!targetKinds().contains(target.kind)) return invalid("Target kind must be monitor, desktop, region or window", "target.kind");
    if (object.contains("identity")) {
        if (!object["identity"].isString() || object["identity"].toString().isEmpty() || object["identity"].toString().size() > 256) return invalid("Target identity must be a nonempty string", "target.identity");
        target.identity = object["identity"].toString();
    }
    if (target.kind == "region") {
        if (!rect(object["rect"], target.rect)) return invalid("A region needs rect [x, y, width, height] with positive size", "target.rect");
    } else if (object.contains("rect")) return invalid("Only region targets take a rect", "target.rect");
    if ((target.kind == "monitor" || target.kind == "window") && target.identity.isEmpty()) return invalid("Monitor and window targets need an identity", "target.identity");
    if ((target.kind == "desktop" || target.kind == "region") && !target.identity.isEmpty()) return invalid("Desktop and region targets take no identity", "target.identity");
    out = target;
    return {};
}
Error parseConfig(const QJsonObject& params, RecordingConfig& out) {
    if (!knownKeys(params, {"format", "codec", "fps", "audio", "maxEdge"})) return invalid("Unknown recording configuration field");
    RecordingConfig config;
    config.format = params["format"].toString();
    if (!formats().contains(config.format)) return invalid("Format must be mp4, webm, gif or webp", "format");
    const auto available = codecs(config.format);
    if (params.contains("codec")) {
        if (!params["codec"].isString() || !available.contains(params["codec"].toString())) return invalid("Codec is not valid for this format", "codec");
        config.codec = params["codec"].toString();
    } else config.codec = available.isEmpty() ? QString{} : available.first();
    qint64 fps = 30;
    if (params.contains("fps") && !integer(params["fps"], 1, 60, &fps)) return invalid("fps must be an integer from 1 to 60", "fps");
    config.fps = int(fps);
    if (params.contains("audio")) {
        if (!params["audio"].isObject() || !knownKeys(params["audio"].toObject(), {"system", "microphone"})) return invalid("audio takes system and microphone booleans", "audio");
        const auto audio = params["audio"].toObject();
        if (audio.contains("system") && !audio["system"].isBool()) return invalid("audio.system must be a boolean", "audio.system");
        if (audio.contains("microphone") && !audio["microphone"].isBool()) return invalid("audio.microphone must be a boolean", "audio.microphone");
        config.audio.system = audio["system"].toBool();
        config.audio.microphone = audio["microphone"].toBool();
        if (config.audio.microphone) return {"recording_unsupported", "Microphone capture is not offered by this version", {{"field", "audio.microphone"}}};
        if (config.silent() && config.audio.system) return invalid("GIF and WebP are silent formats; audio cannot be included", "audio.system");
    }
    if (params.contains("maxEdge")) {
        qint64 edge = 0;
        if (!integer(params["maxEdge"], 16, 8192, &edge)) return invalid("maxEdge must be an integer from 16 to 8192", "maxEdge");
        config.maxEdge = int(edge);
    }
    out = config;
    return {};
}
qint64 alignToFrame(qint64 us, int fps) {
    if (fps < 1) return us;
    const double frame = 1000000.0 / fps;
    return qint64(std::llround(std::llround(us / frame) * frame));
}
namespace {
Error validateMask(const QJsonValue& value, RecordingMask& mask, qint64 duration, const QSize& canvas) {
    if (!value.isObject()) return invalid("A mask must be an object", "mask");
    const auto object = value.toObject();
    if (!knownKeys(object, {"kind", "rect", "start", "end", "strength"})) return invalid("Unknown mask field", "mask");
    RecordingMask result = mask;
    result.kind = object["kind"].toString("blur");
    if (result.kind != "blur" && result.kind != "solid") return invalid("Mask kind must be blur or solid", "mask.kind");
    if (!rect(object["rect"], result.rect)) return invalid("A mask needs rect [x, y, width, height] with positive size", "mask.rect");
    if (!canvas.isEmpty() && (result.rect.x() < 0 || result.rect.y() < 0 || result.rect.right() >= canvas.width() || result.rect.bottom() >= canvas.height()))
        return invalid("Mask rect must lie inside the output canvas", "mask.rect");
    if (!integer(object["start"], 0, duration, &result.start) || !integer(object["end"], 0, duration, &result.end) || result.end <= result.start)
        return invalid("Mask start/end must be microseconds within the clip with end after start", "mask.start");
    qint64 strength = 16;
    if (object.contains("strength") && !integer(object["strength"], 2, 64, &strength)) return invalid("Mask strength must be 2 to 64", "mask.strength");
    result.strength = int(strength);
    mask = result;
    return {};
}
}
Error applyEditCommands(RecordingEdit& edit, const QJsonArray& commands, qint64 duration, const QSize& canvas, int fps) {
    if (commands.isEmpty() || commands.size() > maximumEditCommands) return invalid("Send 1 to 100 edit commands", "commands");
    RecordingEdit next = edit;
    const qint64 minimum = fps > 0 ? 1000000 / fps : 1;
    for (const auto& value : commands) {
        if (!value.isObject()) return invalid("Each command must be an object", "commands");
        const auto command = value.toObject();
        const auto op = command["op"].toString();
        if (op == "setTrim") {
            if (!knownKeys(command, {"op", "start", "end"})) return invalid("setTrim takes start and end", "commands");
            qint64 start = 0, end = duration;
            if (!integer(command["start"], 0, duration, &start)) return invalid("Trim start must be microseconds within the clip", "start");
            if (command.contains("end") && !command["end"].isNull() && !integer(command["end"], 0, duration, &end)) return invalid("Trim end must be microseconds within the clip or null", "end");
            if (end - start < minimum) return invalid("The retained interval must contain at least one frame", "end");
            next.trimStart = start;
            if (command.contains("end") && !command["end"].isNull() && end != duration) next.trimEnd = end; else next.trimEnd.reset();
        } else if (op == "resetTrim") {
            if (!knownKeys(command, {"op"})) return invalid("resetTrim takes no fields", "commands");
            next.trimStart = 0; next.trimEnd.reset();
        } else if (op == "addMask") {
            if (!knownKeys(command, {"op", "mask"})) return invalid("addMask takes a mask", "commands");
            if (next.masks.size() >= maximumMasks) return invalid("At most 64 masks are supported", "mask");
            RecordingMask mask;
            if (auto error = validateMask(command["mask"], mask, duration, canvas)) return error;
            mask.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            next.masks.append(mask);
        } else if (op == "updateMask" || op == "removeMask") {
            if (!knownKeys(command, op == "updateMask" ? QStringList{"op", "maskId", "mask"} : QStringList{"op", "maskId"}) || !command["maskId"].isString()) return invalid("Mask commands need a maskId", "maskId");
            int index = -1;
            for (int i = 0; i < next.masks.size(); ++i) if (next.masks[i].id == command["maskId"].toString()) index = i;
            if (index < 0) return {"recording_mask_unknown", "No mask has this id", {{"maskId", command["maskId"].toString()}}};
            if (op == "removeMask") next.masks.removeAt(index);
            else if (auto error = validateMask(command["mask"], next.masks[index], duration, canvas)) return error;
        } else return invalid("Edit op must be setTrim, resetTrim, addMask, updateMask or removeMask", "op");
    }
    next.revision = edit.revision + 1;
    edit = next;
    return {};
}
QSize outputSize(const QSize& canvas, const std::optional<int>& maxEdge) {
    if (!maxEdge || canvas.isEmpty()) return canvas;
    const int edge = std::max(canvas.width(), canvas.height());
    if (edge <= *maxEdge) return canvas;
    const double scale = double(*maxEdge) / edge;
    // Encoders want even dimensions; keep at least two pixels per side.
    auto even = [](double v) { return std::max(2, int(std::lround(v / 2.0)) * 2); };
    return {even(canvas.width() * scale), even(canvas.height() * scale)};
}
Error checkExportBudget(const RecordingConfig& config, const RecordingEdit& edit, qint64 duration, const QSize& canvas, const RecordingLimits& limits) {
    const auto retained = edit.exportDuration(duration);
    if (retained <= 0) return invalid("The retained interval is empty", "trim");
    if (!config.animation()) return {};
    if (retained > limits.animationMaxUs)
        return {"recording_animation_too_long", "Trim the clip: animated exports are limited in duration", {{"retainedUs", double(retained)}, {"limitUs", double(limits.animationMaxUs)}}};
    const auto size = outputSize(canvas, config.maxEdge);
    const int edge = std::max(size.width(), size.height());
    if (edge > limits.animationMaxEdge)
        return {"recording_animation_too_large", "Set maxEdge: animated exports are limited in size", {{"edge", edge}, {"limitEdge", limits.animationMaxEdge}}};
    return {};
}
}
}
