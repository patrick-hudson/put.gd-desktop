#pragma once

#include <QColor>
#include <QString>

class QWidget;

namespace putgd::RedesignTheme {
// Apply only to the experiment's main window and its descendants.
void apply(QWidget& root);
QString uiFontFamily();
QString monoFontFamily();

inline QColor bg() { return QColor("#111317"); }
inline QColor surface() { return QColor("#181B21"); }
inline QColor surfaceRaised() { return QColor("#20242C"); }
inline QColor selected() { return QColor("#232838"); }
inline QColor border() { return QColor("#2A2F38"); }
inline QColor borderStrong() { return QColor("#343A45"); }
inline QColor text() { return QColor("#E7E9EE"); }
inline QColor textMuted() { return QColor("#9AA1AD"); }
inline QColor textSecondary() { return QColor("#C4C9D2"); }
inline QColor accent() { return QColor("#6EA8FE"); }
inline QColor link() { return QColor("#8DBBFF"); }
inline QColor success() { return QColor("#4CC38A"); }
inline QColor warning() { return QColor("#E5A443"); }
inline QColor danger() { return QColor("#F07A6A"); }
// Drawn over user media (a video poster's play glyph and length): the same in
// every theme, because the picture under it is not themed.
inline QColor mediaScrim() { return QColor(0, 0, 0, 150); }
inline QColor onMedia() { return QColor("#FFFFFF"); }
}
