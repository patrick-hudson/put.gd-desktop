#include "CaptureStyle.h"
#include <QDir>
#include <QJsonValue>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace putgd {
namespace {
bool colorText(const QJsonValue& value) {
    static const QRegularExpression pattern(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    return value.isString() && pattern.match(value.toString()).hasMatch();
}
CaptureStyle look(QString backdrop, QString color, QString colorEnd, QString padding) {
    CaptureStyle style;style.enabled = true;style.backdrop = std::move(backdrop);style.color = std::move(color);
    style.colorEnd = std::move(colorEnd);style.padding = std::move(padding);return style;
}
}

QJsonObject CaptureStyle::toJson() const {
    QJsonObject backdropObject{{"kind", backdrop}, {"color", color}, {"colorEnd", colorEnd}};
    if (!picture.isEmpty()) backdropObject["picture"] = picture;
    return {{"enabled", enabled}, {"backdrop", backdropObject}, {"padding", padding}, {"corners", corners}, {"shadow", shadow}};
}

CaptureStyle CaptureStyle::fromJson(const QJsonObject& object) {
    CaptureStyle style;
    style.enabled = object["enabled"].toBool(false);
    const auto backdropObject = object["backdrop"].toObject();
    if (backdropObject.contains("kind")) style.backdrop = backdropObject["kind"].toString();
    if (backdropObject.contains("color")) style.color = backdropObject["color"].toString().toLower();
    if (backdropObject.contains("colorEnd")) style.colorEnd = backdropObject["colorEnd"].toString().toLower();
    style.picture = backdropObject["picture"].toString();
    if (object.contains("padding")) style.padding = object["padding"].toString();
    style.corners = object["corners"].toBool(true);
    style.shadow = object["shadow"].toBool(true);
    return style;
}

Error CaptureStyle::validate(const QJsonValue& value, const QString& pointer) {
    const auto invalid = [](const QString& at, const QString& message) { return Error{"config_invalid", message, {{"path", at}}}; };
    const auto wrong = QStringLiteral("Capture style is an object with enabled (true or false) and optional backdrop, padding, corners and shadow.");
    if (!value.isObject()) return invalid(pointer, wrong);
    const auto object = value.toObject();
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!QStringList{"enabled", "backdrop", "padding", "corners", "shadow"}.contains(it.key())) return invalid(pointer, wrong);
    if (!object["enabled"].isBool()) return invalid(pointer + "/enabled", "Choose true or false.");
    if (object.contains("padding") && !paddingNames().contains(object["padding"].toString()))
        return invalid(pointer + "/padding", "Padding is small, medium or large.");
    for (const auto* flag : {"corners", "shadow"})
        if (object.contains(flag) && !object[flag].isBool()) return invalid(pointer + "/" + flag, "Choose true or false.");
    if (object.contains("backdrop")) {
        const auto at = pointer + "/backdrop";
        const auto wrongBackdrop = QStringLiteral("The backdrop is an object with kind (color, gradient, picture or none), optional color and colorEnd (#rrggbb), and picture (an absolute path) for kind picture.");
        if (!object["backdrop"].isObject()) return invalid(at, wrongBackdrop);
        const auto backdrop = object["backdrop"].toObject();
        for (auto it = backdrop.begin(); it != backdrop.end(); ++it)
            if (!QStringList{"kind", "color", "colorEnd", "picture"}.contains(it.key())) return invalid(at, wrongBackdrop);
        const auto kind = backdrop["kind"].toString();
        if (!backdropNames().contains(kind)) return invalid(at + "/kind", "The backdrop is color, gradient, picture or none.");
        for (const auto* field : {"color", "colorEnd"})
            if (backdrop.contains(field) && !colorText(backdrop[field])) return invalid(at + "/" + field, "A color must be #rrggbb.");
        if (backdrop.contains("picture")) {
            const auto path = backdrop["picture"].toString();
            bool control = false;
            for (const auto character : path) if (character.category() == QChar::Other_Control) control = true;
            if (!backdrop["picture"].isString() || path.isEmpty() || path.size() > 4096 || control || !QDir::isAbsolutePath(path))
                return invalid(at + "/picture", "The backdrop picture is an absolute path of at most 4,096 characters.");
        }
        if (kind == "picture" && !backdrop.contains("picture")) return invalid(at + "/picture", "Choose a picture for a picture backdrop.");
    }
    return {};
}

bool CaptureStyle::sameLook(const CaptureStyle& other) const {
    if (backdrop != other.backdrop || padding != other.padding || corners != other.corners || shadow != other.shadow) return false;
    if (backdrop == "color") return color == other.color;
    if (backdrop == "gradient") return color == other.color && colorEnd == other.colorEnd;
    if (backdrop == "picture") return picture == other.picture;
    return true;
}

const QList<CaptureStyle::Preset>& CaptureStyle::presets() {
    // Council of 2026-11-22 (#1893): put.gd's own grey first; Clear is never the default,
    // because a shadow on transparency vanishes in dark chat apps.
    static const QList<Preset> list = [] {
        auto clear = look("none", "#383633", "#1c1c1b", "small");
        return QList<Preset>{{"graphite", "Graphite", look("gradient", "#383633", "#1c1c1b", "medium")},
                             {"amber", "Amber", look("gradient", "#f2c98a", "#c9733a", "medium")},
                             {"paper", "Paper", look("color", "#efeee9", "#1c1c1b", "medium")},
                             {"clear", "Clear", clear}};
    }();
    return list;
}

CaptureStyle CaptureStyle::preset(const QString& id) {
    for (const auto& entry : presets()) if (entry.id == id) return entry.style;
    return presets().first().style;
}

QString CaptureStyle::presetId() const {
    for (const auto& entry : presets()) if (entry.style.sameLook(*this)) return entry.id;
    return QStringLiteral("custom");
}

CaptureStyle::Geometry CaptureStyle::geometry(const QSize& capture, int level) const {
    Geometry result;
    if (!capture.isValid() || capture.isEmpty()) return result;
    if (level < 0) level = std::max<int>(0, paddingNames().indexOf(padding));
    const double shorter = std::min(capture.width(), capture.height());
    struct Rule { double share; int minimum, maximum; };
    static constexpr Rule rules[] = {{0.04, 16, 96}, {0.07, 24, 160}, {0.11, 40, 256}};
    const auto& rule = rules[std::clamp(level, 0, 2)];
    result.padding = std::clamp(int(std::lround(rule.share * shorter)), rule.minimum, rule.maximum);
    // Never more than a quarter of the shorter edge, so a thin strip never turns into a pill.
    result.radius = corners ? std::min(std::clamp(int(std::lround(0.012 * shorter)), 6, 32), int(shorter) / 4) : 0;
    // Derived from the padding, so the shadow (about 3 sigma below the offset) always ends inside it.
    result.sigma = shadow ? result.padding / 6.0 : 0;
    result.offsetY = shadow ? result.padding / 8.0 : 0;
    result.canvas = QSize(capture.width() + 2 * result.padding, capture.height() + 2 * result.padding);
    return result;
}

CaptureStyle::Geometry CaptureStyle::fitted(const QSize& capture, qint64 maxPixels, int maxEdge) const {
    for (int level = std::max<int>(0, paddingNames().indexOf(padding)); level >= 0; --level) {
        const auto candidate = geometry(capture, level);
        if (!candidate.valid()) return {};
        if (qint64(candidate.canvas.width()) * candidate.canvas.height() <= maxPixels && candidate.canvas.width() <= maxEdge && candidate.canvas.height() <= maxEdge)
            return candidate;
    }
    return {};
}
}
