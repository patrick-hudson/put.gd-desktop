#pragma once
#include "EditDocument.h"
#include <QJsonArray>
#include <QJsonObject>

namespace putgd::editor {
// JSON contract shared by `editor.*` session methods and the editor helper.
// Coordinates are integer source-image pixels; rectangles are [x, y, width, height].
namespace Commands {
constexpr int maximumCommands=100;
constexpr int maximumPreviewEdge=1600;
QString kindName(Shape::Kind);
QJsonObject shapeJson(const Shape&);
QJsonObject state(const EditDocument&);
// Validates every command before changing the document. Commands are
// add {shape}, update {shapeId, shape}, remove {shapeId}, crop {rect},
// resetCrop, undo and redo. Each change remains one ordinary undo step.
// On error the document is unchanged and `code` names the first problem.
bool apply(EditDocument&,const QJsonArray& commands,QJsonArray& created,QString& code,QString& message);
}
}
