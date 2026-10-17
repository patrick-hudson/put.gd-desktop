#pragma once
#include <QColor>
#include <QImage>
#include <QString>

namespace putgd::IconArt {
// Software rendering also serves the Core-only StatusNotifier resident.
// A negative percentage means a solid status glyph; 0..100 fills bottom to top.
QImage liftTray(const QColor& color, int percent = -1, int scale = 1);
QImage liftApp(const QColor& color, int scale = 1);
QImage symbol(const QString& name, const QColor& color, int scale = 1);
}
