#pragma once
#include <QColor>
#include <QImage>
#include <QString>

namespace putgd::IconArt {
// Software rendering also serves the Core-only StatusNotifier resident.
// -1 is solid, -2 adds a static error badge; 0..100 fills bottom to top.
// recording: 0 none, 1 recording dot, 2 paused bars; independent of progress/error.
QImage liftTray(const QColor& color, int percent = -1, int scale = 1, int recording = 0);
QImage liftApp(const QColor& color, int scale = 1);
// 24-unit outline glyphs; secondary supplies an optional 18% interior wash.
// Brand and tray retain their solid identity artwork.
QImage symbol(const QString& name, const QColor& color, int scale = 1, const QColor& secondary = {});
}
