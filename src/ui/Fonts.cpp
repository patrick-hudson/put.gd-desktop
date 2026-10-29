#include "Fonts.h"
#include "Ui.h"
#include <QFontDatabase>

// Q_INIT_RESOURCE must be outside putgd's namespace.
static void initializeFontResources() { Q_INIT_RESOURCE(fonts); }

namespace putgd::Fonts {
namespace {
struct Families {
    QString ui = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
    QString mono = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    Families() {
        initializeFontResources();
        const int regular = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/IBMPlexSans-Regular.ttf"));
        QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/IBMPlexSans-Medium.ttf"));
        QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/IBMPlexSans-SemiBold.ttf"));
        const int fixed = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/IBMPlexMono-Regular.ttf"));
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
