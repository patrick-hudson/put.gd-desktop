#pragma once
#include "Settings.h"
#include <QImage>
#include <QMimeData>
namespace putgd {
struct PayloadResult;
struct ClipboardPayload {
 QString kind;
 QStringList files;
 QByteArray bytes;
 QImage image;
 QString mime;
 // kind "files": a selected folder becomes one "<folder>.zip" artifact (#1105).
 bool zipFolders=false;
 static PayloadResult fromMime(const QMimeData&, const ResourceLimits&);
};
struct PayloadResult { ClipboardPayload payload; Error error; };
}
