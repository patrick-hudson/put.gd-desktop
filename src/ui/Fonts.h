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
// The installed copies of the bundled fonts for executables in binDirectory:
// <dataDirectory>/putgd/fonts, resolved beside bin/ when dataDirectory is
// relative (the DEB, RPM and AppImage layouts) and as given when absolute.
// Empty when that directory is absent. The one-argument form uses the data
// directory the build was configured with.
QString installedDirectory(const QString& binDirectory, const QString& dataDirectory);
QString installedDirectory(const QString& binDirectory);
// Where a font is loaded from: the installed file when the directory has it,
// otherwise the copy compiled into the executable's resources.
QString source(const QString& installedDirectory, const QString& name);
}
