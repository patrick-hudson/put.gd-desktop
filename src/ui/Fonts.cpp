#include "Fonts.h"
#include "Ui.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>

// Q_INIT_RESOURCE must be outside putgd's namespace.
static void initializeFontResources() { Q_INIT_RESOURCE(fonts); }

namespace putgd::Fonts {
// The installed copies of the bundled fonts (share/putgd/fonts next to bin/, in
// the DEB, RPM and AppImage alike). FreeType maps a font file read-only, so its
// pages are shared page cache; a font read from the executable's resources is
// decompressed into a heap copy that stays private to each interface,
// editor and recording process for its lifetime. The bytes are the same files
// either way, so glyphs render identically; the resource copy remains the
// fallback for a build that runs without the installed data directory.
QString installedDirectory(const QString& binDirectory) {
    const auto candidate = QDir::cleanPath(QDir(binDirectory).filePath(QStringLiteral("../share/putgd/fonts")));
    return QFileInfo(candidate).isDir() ? candidate : QString();
}
QString source(const QString& installedDirectory, const QString& name) {
    if (!installedDirectory.isEmpty()) {
        const QFileInfo file(QDir(installedDirectory).filePath(name));
        if (file.isFile()) return file.absoluteFilePath();
    }
    return QStringLiteral(":/fonts/") + name;
}
namespace {
int addFont(const QString& directory, const QString& name) {
    const auto path = source(directory, name);
    const int id = QFontDatabase::addApplicationFont(path);
    // An unreadable installed file still leaves the resource copy.
    return id >= 0 || path.startsWith(QLatin1Char(':')) ? id : QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/") + name);
}
struct Families {
    QString ui = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
    QString mono = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    Families() {
        initializeFontResources();
        const auto directory = installedDirectory(QCoreApplication::applicationDirPath());
        const int regular = addFont(directory, QStringLiteral("IBMPlexSans-Regular.ttf"));
        addFont(directory, QStringLiteral("IBMPlexSans-Medium.ttf"));
        addFont(directory, QStringLiteral("IBMPlexSans-SemiBold.ttf"));
        const int fixed = addFont(directory, QStringLiteral("IBMPlexMono-Regular.ttf"));
        const auto sans = QFontDatabase::applicationFontFamilies(regular), monos = QFontDatabase::applicationFontFamilies(fixed);
        if (!sans.isEmpty()) ui = sans.front();
        if (!monos.isEmpty()) mono = monos.front();
        Ui::setFamilies(ui, mono);
    }
};
const Families& families() { static const Families value; return value; }
}
void load() { families(); }
QString ui() { return families().ui; }
QString mono() { return families().mono; }
}
