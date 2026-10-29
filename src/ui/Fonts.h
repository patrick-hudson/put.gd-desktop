#pragma once
#include <QString>

namespace putgd::Fonts {
// Registers the bundled IBM Plex Sans and Mono (OFL) once and hands the
// families to the app style; the system families when unavailable.
// Only the interface and recording processes call it, so the resident never
// maps the fonts.
void load();
QString ui();
QString mono();
}
