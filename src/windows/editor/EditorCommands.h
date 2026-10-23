#pragma once
// JSON contract shared by the `editor.*` session methods and the Windows editor
// helper (docs/image-editor.md, "Agent and MCP control"). A port of the Qt
// reference src/editor/EditorCommands: same fields, limits and error codes.
// Coordinates are integer source-image pixels; rectangles are [x, y, width, height].
#include "common/Json.h"
#include "editor/EditDocument.h"
#include <string>

namespace putgd::editor::commands {
inline constexpr int maximumCommands = 100;
inline constexpr int minimumPreviewEdge = 64, defaultPreviewEdge = 1024, maximumPreviewEdge = 1600;
std::string kindName(Shape::Kind);
json::Object shapeJson(const Shape&);
// width, height, crop, shapes, canUndo, canRedo, historyTruncated, modified.
json::Object state(const EditDocument&);
// Validates every command on a draft copy (the base raster is shared, only
// geometry is copied) and swaps it in when all succeed. Commands are
// add {shape}, update {shapeId, shape}, remove {shapeId}, crop {rect},
// resetCrop, undo and redo; each change remains one ordinary undo step.
// On error the document is unchanged and `code` names the first problem
// (edit_command_invalid, edit_shape_missing or edit_history_empty).
bool apply(EditDocument&, const json::Array& commands, const TextMeasurer&, json::Array& created, std::string& code, std::string& message);
// A font family name the contract accepts: at most 100 printable characters.
bool validFontFamily(const std::string&);
}
