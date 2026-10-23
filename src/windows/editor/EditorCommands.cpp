#include "editor/EditorCommands.h"
#include "common/Text.h"
#include <climits>
#include <cmath>
#include <initializer_list>
#include <string_view>

namespace putgd::editor::commands {
namespace {
struct KindEntry { const char* name; Shape::Kind kind; };
constexpr KindEntry kinds[] = {{"redact", Shape::Kind::Redact}, {"blur", Shape::Kind::Blur}, {"arrow", Shape::Kind::Arrow},
                               {"rectangle", Shape::Kind::Rectangle}, {"text", Shape::Kind::Text}, {"highlight", Shape::Kind::Highlight}};
bool kindFrom(const json::Value& value, Shape::Kind& out) {
    if (!value.isString()) return false;
    for (const auto& entry : kinds) if (value.stringRef() == entry.name) { out = entry.kind; return true; }
    return false;
}
bool integer(const json::Value& value, long long minimum, long long maximum, int& out) {
    if (!value.isNumber()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number || number < double(minimum) || number > double(maximum)) return false;
    out = int(number);
    return true;
}
json::Array rectJson(const Rect& r) { return {r.x, r.y, r.w, r.h}; }
json::Array pointJson(const Point& p) { return {p.x, p.y}; }
bool rectFrom(const json::Value& value, Rect& out) {
    if (!value.isArray()) return false;
    const auto& a = value.arrayRef();
    int x = 0, y = 0, w = 0, h = 0;
    if (a.size() != 4 || !integer(a[0], -1000000, 1000000, x) || !integer(a[1], -1000000, 1000000, y) || !integer(a[2], 1, 1000000, w) || !integer(a[3], 1, 1000000, h)) return false;
    out = {x, y, w, h};
    return true;
}
bool pointFrom(const json::Value& value, Point& out) {
    if (!value.isArray()) return false;
    const auto& a = value.arrayRef();
    int x = 0, y = 0;
    if (a.size() != 2 || !integer(a[0], -1000000, 1000000, x) || !integer(a[1], -1000000, 1000000, y)) return false;
    out = {x, y};
    return true;
}
bool only(const json::Object& object, std::initializer_list<std::string_view> allowed) {
    for (const auto& [key, value] : object) {
        bool known = false;
        for (const auto name : allowed) known = known || key == name;
        if (!known) return false;
    }
    return true;
}
bool blank(const std::string& s) {
    for (const unsigned char c : s) if (!(c == ' ' || c == '\t' || c == '\v' || c == '\f')) return false;
    return true;
}
struct Failure { std::string code, message; };
// Fields absent from `object` keep the values already in `shape`.
bool shapeFrom(const json::Object& object, Shape& shape, const Rect& image, bool creating, const TextMeasurer& measure, Failure& failure) {
    auto bad = [&](const char* message) { failure = {"edit_command_invalid", message}; return false; };
    if (!only(object, {"kind", "rect", "from", "to", "at", "color", "width", "strength", "fontPixels", "font", "text"})) return bad("Shape has unsupported fields");
    if (creating) {
        if (!kindFrom(object.value("kind"), shape.kind)) return bad("Shape kind must be redact, blur, arrow, rectangle, text or highlight");
        if (shape.kind == Shape::Kind::Redact) shape.color = 0x000000;
        if (shape.kind == Shape::Kind::Highlight) shape.color = defaultHighlightColor;
    } else if (object.contains("kind")) {
        Shape::Kind kind;
        if (!kindFrom(object.value("kind"), kind) || kind != shape.kind) return bad("An existing shape cannot change kind");
    }
    const bool arrow = shape.kind == Shape::Kind::Arrow, text = shape.kind == Shape::Kind::Text;
    if (object.contains("color")) {
        if (shape.kind == Shape::Kind::Redact || shape.kind == Shape::Kind::Blur) return bad("Redact is always opaque black and Blur has no color");
        Rgb color = 0;
        if (!parseColor(object.value("color").toString(), color)) return bad("Color must be #RRGGBB");
        shape.color = color;
    }
    if (object.contains("width") && !integer(object.value("width"), 1, 64, shape.width)) return bad("Width must be an integer from 1 to 64");
    if (object.contains("strength") && !integer(object.value("strength"), EditDocument::minimumBlur, EditDocument::maximumBlur, shape.strength)) return bad("Blur strength must be an integer from 2 to 64");
    if (object.contains("fontPixels") && !integer(object.value("fontPixels"), minimumFontPixels, maximumFontPixels, shape.fontPixels)) return bad("Text size must be an integer from 8 to 200 pixels");
    if (object.contains("font")) {
        if (!text) return bad("Only text shapes have a font");
        if (!object.value("font").isString() || !validFontFamily(object.value("font").stringRef()))
            return bad("font must be a family name of at most 100 printable characters; empty means the default");
        shape.fontFamily = object.value("font").stringRef();
    }
    if (object.contains("text")) {
        if (!text) return bad("Only text shapes have text");
        const auto& value = object.value("text");
        if (!value.isString() || !text::validUtf8(value.stringRef()) || blank(value.stringRef()) || utf16Length(value.stringRef()) > size_t(EditDocument::maximumTextCharacters) ||
            text::containsChar(value.stringRef(), '\n') || text::containsChar(value.stringRef(), '\r'))
            return bad("Text must be one nonempty line of at most 500 characters");
        shape.text = value.stringRef();
    }
    if (arrow) {
        if (object.contains("rect") || object.contains("at")) return bad("Arrows use from and to points");
        if (object.contains("from") && !pointFrom(object.value("from"), shape.from)) return bad("from must be [x, y]");
        if (object.contains("to") && !pointFrom(object.value("to"), shape.to)) return bad("to must be [x, y]");
        if (creating && (!object.contains("from") || !object.contains("to"))) return bad("Arrows need from and to points");
        if (shape.from == shape.to) return bad("Arrow points must differ");
    } else if (text) {
        if (object.contains("rect") || object.contains("from") || object.contains("to")) return bad("Text uses an at point; its box follows the text");
        Point at{shape.rect.x, shape.rect.y};
        if (object.contains("at") && !pointFrom(object.value("at"), at)) return bad("at must be [x, y]");
        if (creating && (!object.contains("at") || !object.contains("text"))) return bad("Text needs at and text");
        const auto size = measure ? measure(shape.text, shape.fontFamily, shape.fontPixels) : TextSize{};
        shape.rect = {at.x, at.y, std::max(1, size.width + 1), std::max(1, size.height)};
    } else {
        if (object.contains("from") || object.contains("to") || object.contains("at")) return bad("Box shapes use rect");
        if (object.contains("rect") && !rectFrom(object.value("rect"), shape.rect)) return bad("rect must be [x, y, width, height] with positive size");
        if (creating && !object.contains("rect")) return bad("Box shapes need rect");
    }
    const auto bounds = arrow ? Rect::spanning(shape.from, shape.to) : shape.rect;
    if (!bounds.intersects(image)) return bad("Shape lies outside the image");
    // Redaction is clamped so its stored geometry equals the exported coverage.
    if (shape.kind == Shape::Kind::Redact || shape.kind == Shape::Kind::Blur) shape.rect = EditDocument::clampRect(shape.rect, image);
    return true;
}
}

bool validFontFamily(const std::string& family) {
    if (!text::validUtf8(family) || text::codePoints(family) > maximumFontFamilyCharacters) return false;
    // Printable: no C0/C1 controls and no line or paragraph separators.
    for (size_t i = 0; i < family.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(family[i]);
        if (c < 0x20 || c == 0x7F) return false;
        if (c == 0xC2 && i + 1 < family.size() && static_cast<unsigned char>(family[i + 1]) >= 0x80 && static_cast<unsigned char>(family[i + 1]) <= 0x9F) return false;
        if (c == 0xE2 && i + 2 < family.size() && static_cast<unsigned char>(family[i + 1]) == 0x80 &&
            (static_cast<unsigned char>(family[i + 2]) == 0xA8 || static_cast<unsigned char>(family[i + 2]) == 0xA9)) return false;
    }
    return true;
}

std::string kindName(Shape::Kind kind) {
    for (const auto& entry : kinds) if (entry.kind == kind) return entry.name;
    return {};
}

json::Object shapeJson(const Shape& shape) {
    json::Object result{{"id", json::Value(double(shape.id))}, {"kind", kindName(shape.kind)}};
    using K = Shape::Kind;
    if (shape.kind == K::Arrow) { result["from"] = pointJson(shape.from); result["to"] = pointJson(shape.to); }
    else result["rect"] = rectJson(shape.rect);
    if (shape.kind == K::Arrow || shape.kind == K::Rectangle || shape.kind == K::Text || shape.kind == K::Highlight) result["color"] = colorName(shape.color);
    if (shape.kind == K::Arrow || shape.kind == K::Rectangle) result["width"] = shape.width;
    if (shape.kind == K::Blur) result["strength"] = shape.strength;
    if (shape.kind == K::Text) {
        result["text"] = shape.text;
        result["fontPixels"] = shape.fontPixels;
        result["font"] = shape.fontFamily;
        result["at"] = pointJson({shape.rect.x, shape.rect.y});
    }
    return result;
}

json::Object state(const EditDocument& doc) {
    json::Array shapes;
    for (const auto& shape : doc.shapes()) shapes.push_back(shapeJson(shape));
    return {{"width", doc.width()}, {"height", doc.height()}, {"crop", rectJson(doc.crop())}, {"shapes", shapes},
            {"canUndo", doc.canUndo()}, {"canRedo", doc.canRedo()}, {"historyTruncated", doc.historyTruncated()}, {"modified", doc.modified()}};
}

bool apply(EditDocument& doc, const json::Array& commands, const TextMeasurer& measure, json::Array& created, std::string& code, std::string& message) {
    created.clear();
    if (commands.empty() || commands.size() > size_t(maximumCommands)) { code = "edit_command_invalid"; message = "Supply 1 to 100 commands"; return false; }
    // The base raster is shared; only geometry and history are copied.
    auto draft = doc;
    json::Array ids;
    Failure failure;
    for (size_t index = 0; index < commands.size(); ++index) {
        auto stop = [&](const std::string& c, const std::string& m) { code = c; message = "Command " + std::to_string(index) + ": " + m; return false; };
        if (!commands[index].isObject()) return stop("edit_command_invalid", "Each command must be an object");
        const auto& command = commands[index].objectRef();
        const auto op = command.value("op").toString();
        int shapeId = 0;
        if (op == "add") {
            if (!only(command, {"op", "shape"}) || !command.value("shape").isObject()) return stop("edit_command_invalid", "add needs only a shape object");
            Shape shape;
            if (!shapeFrom(command.value("shape").objectRef(), shape, draft.imageRect(), true, measure, failure)) return stop(failure.code, failure.message);
            ids.push_back(json::Value(double(draft.add(shape))));
        } else if (op == "update") {
            if (!only(command, {"op", "shapeId", "shape"}) || !integer(command.value("shapeId"), 1, INT_MAX, shapeId) || !command.value("shape").isObject())
                return stop("edit_command_invalid", "update needs shapeId and a shape object");
            const auto* current = draft.find(uint64_t(shapeId));
            if (!current) return stop("edit_shape_missing", "No shape has this id");
            auto shape = *current;
            if (!shapeFrom(command.value("shape").objectRef(), shape, draft.imageRect(), false, measure, failure)) return stop(failure.code, failure.message);
            draft.replace(shape);
        } else if (op == "remove") {
            if (!only(command, {"op", "shapeId"}) || !integer(command.value("shapeId"), 1, INT_MAX, shapeId)) return stop("edit_command_invalid", "remove needs shapeId");
            if (!draft.remove(uint64_t(shapeId))) return stop("edit_shape_missing", "No shape has this id");
        } else if (op == "crop") {
            Rect rect;
            if (!only(command, {"op", "rect"}) || !rectFrom(command.value("rect"), rect)) return stop("edit_command_invalid", "crop needs rect [x, y, width, height]");
            if (!rect.intersects(draft.imageRect())) return stop("edit_command_invalid", "Crop lies outside the image");
            draft.setCrop(rect);
        } else if (op == "resetCrop") {
            if (!only(command, {"op"})) return stop("edit_command_invalid", "resetCrop has no fields");
            draft.setCrop(draft.imageRect());
        } else if (op == "undo" || op == "redo") {
            if (!only(command, {"op"})) return stop("edit_command_invalid", op + " has no fields");
            if (!(op == "undo" ? draft.undo() : draft.redo())) return stop("edit_history_empty", "Nothing to " + op);
        } else {
            return stop("edit_command_invalid", "op must be add, update, remove, crop, resetCrop, undo or redo");
        }
    }
    doc = std::move(draft);
    created = std::move(ids);
    return true;
}
}
