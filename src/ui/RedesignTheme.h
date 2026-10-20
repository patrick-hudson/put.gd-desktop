#pragma once

#include "Theme.h"
#include <QColor>
#include <QString>

class QWidget;

namespace putgd::RedesignTheme {
// Apply to a main window and its descendants. The root keeps following the
// active Theme scheme: a switch re-applies its palette and stylesheet (plus
// the optional extra stylesheet template, with the same %token% placeholders).
void apply(QWidget& root, const QString& extraTemplate = {});
QString uiFontFamily();
QString monoFontFamily();
// Folder of the check, radio and chevron indicator images painted for the
// scheme, which the redesign stylesheet names as %INDICATORS%.
QString indicators(Theme::Scheme);

inline QColor bg() { return Theme::current().bg; }
inline QColor surface() { return Theme::current().surface; }
inline QColor surfaceRaised() { return Theme::current().surfaceRaised; }
inline QColor selected() { return Theme::current().selected; }
inline QColor border() { return Theme::current().border; }
inline QColor borderStrong() { return Theme::current().borderStrong; }
inline QColor text() { return Theme::current().text; }
inline QColor textMuted() { return Theme::current().textMuted; }
inline QColor textSecondary() { return Theme::current().textSecondary; }
inline QColor accent() { return Theme::current().accent; }
inline QColor link() { return Theme::current().link; }
inline QColor success() { return Theme::current().success; }
inline QColor warning() { return Theme::current().warning; }
inline QColor danger() { return Theme::current().danger; }
// Over user media (a video poster's glyph and length, #112): fixed, see Theme.
inline QColor mediaScrim() { return Theme::mediaScrim(); }
inline QColor onMedia() { return Theme::onMedia(); }
}
