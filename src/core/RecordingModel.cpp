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
    if (kind == "region" && rect.isValid() && !rect.isEmpty()) o["rect"] = rectJson(rect);
    return o;
}
QString RecordingConfig::mime() const {
    if (format == "mp4") return "video/mp4";
    if (format == "webm") return "video/webm";
    if (format == "gif") return "image/gif";
    return "image/webp";
}
QString RecordingConfig::extension() const { return format; }
QJsonObject RecordingAudio::toJson() const {
    QJsonObject o{{"system", system}, {"microphone", microphone}};
    if (!microphoneDevice.isEmpty()) o["microphoneDevice"] = microphoneDevice;
    return o;
}
QJsonObject RecordingConfig::toJson() const {
    QJsonObject o{{"format", format}, {"codec", codec}, {"fps", fps}, {"adaptiveFps", adaptiveFps},
        {"audio", audio.toJson()}, {"clickHighlights", clickHighlights}, {"mime", mime()}};
    if (maxEdge) o["maxEdge"] = *maxEdge;
    if (budgetBytes > 0) o["budgetBytes"] = double(budgetBytes);
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
QString stateName(RecordingState state) {
    switch (state) {
    case RecordingState::Preparing: return QStringLiteral("preparing");
    case RecordingState::Ready: return QStringLiteral("ready");
    case RecordingState::Starting: return QStringLiteral("starting");
    case RecordingState::Recording: return QStringLiteral("recording");
    case RecordingState::Paused: return QStringLiteral("paused");
    case RecordingState::Stopping: return QStringLiteral("stopping");
    case RecordingState::Reviewing: return QStringLiteral("reviewing");
    case RecordingState::Exporting: return QStringLiteral("exporting");
    case RecordingState::Submitted: return QStringLiteral("submitted");
    case RecordingState::Cancelled: return QStringLiteral("cancelled");
    case RecordingState::Failed: return QStringLiteral("failed");
    }
    return {};
}
QStringList states() {
    QStringList names;
    for (int state = int(RecordingState::Preparing); state <= int(RecordingState::Failed); ++state) names.append(stateName(RecordingState(state)));
    return names;
}
QStringList targetKinds() { return {"monitor", "desktop", "region", "window"}; }
QStringList formats() { return {"mp4", "webm", "gif", "webp"}; }
QStringList codecs(const QString& format) {
    if (format == "mp4") return {"h264", "h265"};
    if (format == "webm") return {"vp9"};
    return {};
}
bool terminal(RecordingState state) { return state == RecordingState::Submitted || state == RecordingState::Cancelled || state == RecordingState::Failed; }
bool allowedStep(RecordingState from, RecordingState to) {
    using S = RecordingState;
    if (terminal(from)) return false;
    // fail() and cancelActive() end the session from any state.
    if (to == S::Failed || to == S::Cancelled) return true;
    switch (from) {
    // prepared(): the helper's prepare result, or the shell placed the bar.
    case S::Preparing: return to == S::Ready;
    // recording.start; or Record on the helper's bar (the `started` event).
    case S::Ready: return to == S::Starting || to == S::Recording;
    // The start reply, or the bar's `started` event while the start is in flight.
    case S::Starting: return to == S::Recording;
    // Pause; recording.stop; the helper's `stopped` event (its budget, a lost source, Stop on the bar).
    case S::Recording: return to == S::Paused || to == S::Stopping || to == S::Reviewing;
    case S::Paused: return to == S::Recording || to == S::Stopping || to == S::Reviewing;
    // The stop reply or the `stopped` event.
    case S::Stopping: return to == S::Reviewing;
    // saveCopy, upload or a retried export.
    case S::Reviewing: return to == S::Exporting;
    // A saved copy or a failed export goes back to review; an upload hands the clip over.
    case S::Exporting: return to == S::Reviewing || to == S::Submitted;
    default: return false;
    }
}
Error checkTransition(RecordingState state, const QString& command) {
    using S = RecordingState;
    static const QList<S> live{S::Preparing, S::Ready, S::Starting, S::Recording, S::Paused, S::Stopping, S::Reviewing, S::Exporting};
    static const QHash<QString, QList<S>> allowed{
        {"start", {S::Ready}},
        {"pause", {S::Recording}},
        {"resume", {S::Paused}},
        {"stop", {S::Recording, S::Paused}},
        {"cancel", live},
        // Completes an open region selection with a rectangle, as a drag would.
        {"select", {S::Preparing}},
        {"edit", {S::Reviewing}},
        {"preview", {S::Reviewing}},
        // Drives the review window's controls (#283); inspect also while exporting.
        {"review", {S::Reviewing, S::Exporting}},
        {"saveCopy", {S::Reviewing}},
        {"upload", {S::Reviewing}},
        {"inspect", live + QList<S>{S::Submitted, S::Cancelled, S::Failed}},
        {"raise", {S::Ready, S::Starting, S::Recording, S::Paused, S::Stopping, S::Reviewing, S::Exporting}},
        {"overlay", {S::Ready, S::Starting, S::Recording, S::Paused}},
        // The ready bar's audio toggles (#1364): what the clip records is fixed at Record.
        {"audio", {S::Ready}}};
    if (!allowed.contains(command)) return {"unknown_method", "Unknown recording command", {}};
    const auto name = stateName(state);
    if (!allowed[command].contains(state)) return {"recording_state", command == QLatin1String("audio") ? QStringLiteral("The audio sources can be changed only while the recording is ready; it is %1").arg(name)
                                                                                                         : QStringLiteral("Cannot %1 a recording that is %2").arg(command, name), {{"state", name}, {"command", command}}};
    return {};
}
QString phaseName(HelperPhase phase) {
    switch (phase) {
    case HelperPhase::Idle: return QStringLiteral("idle");
    case HelperPhase::Preparing: return QStringLiteral("preparing");
    case HelperPhase::Ready: return QStringLiteral("ready");
    case HelperPhase::Starting: return QStringLiteral("starting");
    case HelperPhase::Recording: return QStringLiteral("recording");
    case HelperPhase::Paused: return QStringLiteral("paused");
    case HelperPhase::Stopping: return QStringLiteral("stopping");
    case HelperPhase::Recorded: return QStringLiteral("recorded");
    case HelperPhase::Exporting: return QStringLiteral("exporting");
    case HelperPhase::Cancelled: return QStringLiteral("cancelled");
    case HelperPhase::Failed: return QStringLiteral("failed");
    }
    return {};
}
std::optional<RecordingState> residentState(HelperPhase phase) {
    switch (phase) {
    case HelperPhase::Idle: return std::nullopt;
    case HelperPhase::Preparing: return RecordingState::Preparing;
    case HelperPhase::Ready: return RecordingState::Ready;
    case HelperPhase::Starting: return RecordingState::Starting;
    case HelperPhase::Recording: return RecordingState::Recording;
    case HelperPhase::Paused: return RecordingState::Paused;
    case HelperPhase::Stopping: return RecordingState::Stopping;
    case HelperPhase::Recorded: return RecordingState::Reviewing;
    case HelperPhase::Exporting: return RecordingState::Exporting;
    case HelperPhase::Cancelled: return RecordingState::Cancelled;
    case HelperPhase::Failed: return RecordingState::Failed;
    }
    return std::nullopt;
}
Error checkHelperCommand(HelperPhase phase, const QString& command) {
    using P = HelperPhase;
    static const QList<P> prepared{P::Ready, P::Starting, P::Recording, P::Paused, P::Stopping, P::Recorded, P::Exporting, P::Cancelled, P::Failed};
    static const QHash<QString, QList<P>> accepted{
        {"prepare", {P::Idle}},
        {"select", {P::Preparing}},
        {"start", {P::Ready}},
        {"pause", {P::Recording}},
        {"resume", {P::Paused}},
        {"stop", {P::Recording, P::Paused}},
        {"cancel", {P::Preparing, P::Ready, P::Starting, P::Recording, P::Paused, P::Stopping, P::Recorded, P::Exporting}},
        {"audio", {P::Ready}},
        {"raise", {P::Ready, P::Starting, P::Recording, P::Paused, P::Stopping, P::Recorded, P::Exporting}},
        {"overlay", {P::Ready, P::Starting, P::Recording, P::Paused}},
        {"review", {P::Recorded, P::Exporting}},
        {"preview", {P::Recorded}},
        // The resident's edit, saveCopy and upload.
        {"setEdit", {P::Recorded}},
        {"export", {P::Recorded}},
        {"savedCopy", prepared},
        {"notice", prepared},
        {"click", prepared}};
    if (!accepted.contains(command)) return {"unknown_method", "Unknown recording helper command", {}};
    if (!accepted[command].contains(phase)) return {"recording_state", QStringLiteral("The recording helper cannot %1 while %2").arg(command, phaseName(phase)), {{"phase", phaseName(phase)}, {"command", command}}};
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
        // Without a rect the helper asks the user to drag the area; release keeps
        // the bounds in the ready state.
        if (object.contains("rect") && !rect(object["rect"], target.rect)) return invalid("A region rect is [x, y, width, height] with positive size", "target.rect");
    } else if (object.contains("rect")) return invalid("Only region targets take a rect", "target.rect");
    // Monitor and window identities are optional: without one the helper takes
    // the primary monitor on X11, or lets the compositor's picker choose on Wayland.
    if ((target.kind == "desktop" || target.kind == "region") && !target.identity.isEmpty()) return invalid("Desktop and region targets take no identity", "target.identity");
    out = target;
    return {};
}
Error parseConfig(const QJsonObject& params, RecordingConfig& out) {
    if (!knownKeys(params, {"format", "codec", "fps", "adaptiveFps", "audio", "clickHighlights", "maxEdge"})) return invalid("Unknown recording configuration field");
    RecordingConfig config;
    config.format = params["format"].toString();
    if (!formats().contains(config.format)) return invalid("Format must be mp4, webm, gif or webp", "format");
    const auto available = codecs(config.format);
    // A serialized config round-trips an empty codec for GIF/WebP.
    if (params.contains("codec") && !(params["codec"].isString() && params["codec"].toString().isEmpty() && available.isEmpty())) {
        if (!params["codec"].isString() || !available.contains(params["codec"].toString())) return invalid("Codec is not valid for this format", "codec");
        config.codec = params["codec"].toString();
    } else config.codec = available.isEmpty() ? QString{} : available.first();
    qint64 fps = 30;
    if (params.contains("fps") && !integer(params["fps"], 1, 60, &fps)) return invalid("fps must be an integer from 1 to 60", "fps");
    config.fps = int(fps);
    if (params.contains("adaptiveFps") && !params["adaptiveFps"].isBool()) return invalid("adaptiveFps must be a boolean", "adaptiveFps");
    config.adaptiveFps = params["adaptiveFps"].toBool(true);
    if (params.contains("audio")) {
        if (!params["audio"].isObject() || !knownKeys(params["audio"].toObject(), {"system", "microphone", "microphoneDevice"})) return invalid("audio takes system and microphone booleans and an optional microphoneDevice", "audio");
        const auto audio = params["audio"].toObject();
        if (audio.contains("system") && !audio["system"].isBool()) return invalid("audio.system must be a boolean", "audio.system");
        if (audio.contains("microphone") && !audio["microphone"].isBool()) return invalid("audio.microphone must be a boolean", "audio.microphone");
        if (audio.contains("microphoneDevice") && !validMicrophoneDevice(audio["microphoneDevice"])) return invalid("audio.microphoneDevice is the name of an input device, at most 256 characters", "audio.microphoneDevice");
        config.audio.system = audio["system"].toBool();
        config.audio.microphone = audio["microphone"].toBool();
        config.audio.microphoneDevice = audio["microphoneDevice"].toString();
        if (config.silent() && config.audio.system) return invalid("GIF and WebP are silent formats; audio cannot be included", "audio.system");
        if (config.silent() && config.audio.microphone) return invalid("GIF and WebP are silent formats; audio cannot be included", "audio.microphone");
    }
    if (params.contains("clickHighlights") && !params["clickHighlights"].isBool()) return invalid("clickHighlights must be a boolean", "clickHighlights");
    config.clickHighlights = params["clickHighlights"].toBool();
    if (params.contains("maxEdge")) {
        qint64 edge = 0;
        if (!integer(params["maxEdge"], 16, 8192, &edge)) return invalid("maxEdge must be an integer from 16 to 8192", "maxEdge");
        config.maxEdge = int(edge);
    }
    out = config;
    return {};
}
bool validMicrophoneDevice(const QJsonValue& value) {
    if (!value.isString()) return false;
    const auto name = value.toString();
    if (name.size() > 256) return false;
    // A source name never holds control characters; refuse them before they reach a pipeline description.
    for (const QChar c : name) if (c.unicode() < 0x20 || c.unicode() == 0x7f) return false;
    return true;
}
bool monitorSourceName(const QString& name) { return name.endsWith(QStringLiteral(".monitor"), Qt::CaseInsensitive) || name == QStringLiteral("@DEFAULT_MONITOR@"); }
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
        return {"recording_animation_too_long", QStringLiteral("%1 exports are limited to %2 s; trim the clip to %2 s or less").arg(config.format.toUpper()).arg(limits.animationMaxUs / 1000000), {{"retainedUs", double(retained)}, {"limitUs", double(limits.animationMaxUs)}}};
    const auto size = outputSize(canvas, config.maxEdge);
    const int edge = std::max(size.width(), size.height());
    if (edge > limits.animationMaxEdge)
        return {"recording_animation_too_large", QStringLiteral("%1 exports are limited to %2 px; set maxEdge to %2 or less").arg(config.format.toUpper()).arg(limits.animationMaxEdge), {{"edge", edge}, {"limitEdge", limits.animationMaxEdge}}};
    return {};
}
bool clampAnimationSize(RecordingConfig& config, const QSize& canvas, const RecordingLimits& limits) {
    if (!config.animation() || canvas.isEmpty()) return false;
    const auto size = outputSize(canvas, config.maxEdge);
    if (std::max(size.width(), size.height()) <= limits.animationMaxEdge) return false;
    config.maxEdge = limits.animationMaxEdge;
    return true;
}
}
}
