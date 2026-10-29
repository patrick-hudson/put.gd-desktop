#pragma once
#include <QColor>
#include <QImage>
#include <QString>

namespace putgd::IconArt {
// Software rendering also serves the Core-only StatusNotifier resident.
// -1 is solid; -2 tints the glyph error and adds a badge when not recording.
// 0..100 fills bottom to top. Badge priority: recording dot (1), paused bars (2), error.
QImage liftTray(const QColor& color, int percent = -1, int scale = 1, int recording = 0);
QImage liftApp(const QColor& color, int scale = 1);
// 24-unit outline glyphs; secondary supplies an optional 18% interior wash.
// Brand and tray retain their solid identity artwork.
QImage symbol(const QString& name, const QColor& color, int scale = 1, const QColor& secondary = {});
}
