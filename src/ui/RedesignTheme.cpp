#include "RedesignTheme.h"

#include <QFile>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QFont>
#include <QFontDatabase>
#include <QPalette>
#include <QResource>
#include <QWidget>

// Q_INIT_RESOURCE must be outside putgd's namespace.
static void initializeRedesignResources()
{
    Q_INIT_RESOURCE(redesign);
}

namespace putgd::RedesignTheme {
namespace {
struct Fonts {
    QString ui = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
    QString mono = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();

    Fonts()
    {
        initializeRedesignResources();
        const int regular = QFontDatabase::addApplicationFont(QStringLiteral(":/redesign/fonts/IBMPlexSans-Regular.ttf"));
        QFontDatabase::addApplicationFont(QStringLiteral(":/redesign/fonts/IBMPlexSans-SemiBold.ttf"));
        const int fixed = QFontDatabase::addApplicationFont(QStringLiteral(":/redesign/fonts/IBMPlexMono-Regular.ttf"));
        const auto sansFamilies = QFontDatabase::applicationFontFamilies(regular);
        const auto monoFamilies = QFontDatabase::applicationFontFamilies(fixed);
        if (!sansFamilies.isEmpty()) ui = sansFamilies.front();
        if (!monoFamilies.isEmpty()) mono = monoFamilies.front();
    }
};
const Fonts& fonts()
{
    static const Fonts result;
    return result;
}
}

QString uiFontFamily() { return fonts().ui; }
QString monoFontFamily() { return fonts().mono; }

void apply(QWidget& root)
{
    QFont font(uiFontFamily());
    font.setPixelSize(14);
    root.setFont(font);
    auto palette = root.palette();
    palette.setColor(QPalette::Window, bg());
    palette.setColor(QPalette::WindowText, text());
    palette.setColor(QPalette::Base, surfaceRaised());
    palette.setColor(QPalette::AlternateBase, surface());
    palette.setColor(QPalette::Text, text());
    palette.setColor(QPalette::Button, surface());
    palette.setColor(QPalette::ButtonText, text());
    palette.setColor(QPalette::Highlight, selected());
    palette.setColor(QPalette::HighlightedText, text());
    palette.setColor(QPalette::PlaceholderText, textMuted());
    palette.setColor(QPalette::Link, link());
    palette.setColor(QPalette::Disabled, QPalette::Text, textMuted());
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, textMuted());
    root.setPalette(palette);
    root.setProperty("redesignRoot", true);
    root.setAttribute(Qt::WA_StyledBackground, true);
    QFile file(QStringLiteral(":/redesign/redesign.qss"));
    if (file.open(QIODevice::ReadOnly)) {
        auto stylesheet = QString::fromUtf8(file.readAll());
        stylesheet.replace(QStringLiteral("%UI_FONT%"), uiFontFamily());
        stylesheet.replace(QStringLiteral("%MONO_FONT%"), monoFontFamily());
        root.setStyleSheet(stylesheet);
        // Existing popup containers can retain the application theme's menu
        // delegate and margins. Refresh both after applying the scoped theme.
        for (auto* combo : root.findChildren<QComboBox*>()) {
            combo->ensurePolished();
            QEvent changed(QEvent::StyleChange);
            QCoreApplication::sendEvent(combo, &changed);
        }
    }
}
}
