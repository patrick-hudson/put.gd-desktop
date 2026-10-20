#include "RedesignTheme.h"
#include "VisualStyle.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QPalette>
#include <QPointer>
#include <QResource>
#include <QApplication>
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
struct Root {
    QPointer<QWidget> widget;
};
QList<Root>& roots()
{
    static QList<Root> list;
    return list;
}
void setPalette(QWidget& root)
{
    const auto& p = Theme::current();
    auto palette = root.palette();
    palette.setColor(QPalette::Window, p.bg);
    palette.setColor(QPalette::WindowText, p.text);
    palette.setColor(QPalette::Base, p.surfaceRaised);
    palette.setColor(QPalette::AlternateBase, p.surface);
    palette.setColor(QPalette::Text, p.text);
    palette.setColor(QPalette::Button, p.surface);
    palette.setColor(QPalette::ButtonText, p.text);
    palette.setColor(QPalette::Highlight, p.selected);
    palette.setColor(QPalette::HighlightedText, p.text);
    palette.setColor(QPalette::PlaceholderText, p.textMuted);
    palette.setColor(QPalette::Link, p.link);
    palette.setColor(QPalette::Disabled, QPalette::Text, p.textMuted);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, p.textMuted);
    root.setPalette(palette);
}
void style(QWidget& root, const QString& extra)
{
    QFont font(uiFontFamily());
    font.setPixelSize(14);
    root.setFont(font);
    setPalette(root);
    root.setProperty("redesignRoot", true);
    root.setAttribute(Qt::WA_StyledBackground, true);
    QFile file(QStringLiteral(":/redesign/redesign.qss"));
    if (!file.open(QIODevice::ReadOnly)) return;
    const auto qss = QString::fromUtf8(file.readAll()) + extra;
    // Both schemes, set once: a switch re-polishes the marked widgets (#237).
    root.setStyleSheet(VisualStyle::schemeScoped([&qss](Theme::Scheme scheme) {
        auto stylesheet = Theme::stylesheet(qss, scheme);
        stylesheet.replace(QStringLiteral("%UI_FONT%"), uiFontFamily());
        stylesheet.replace(QStringLiteral("%MONO_FONT%"), monoFontFamily());
        stylesheet.replace(QStringLiteral("%INDICATORS%"), indicators(scheme));
        return stylesheet;
    }));
    // Existing popup containers can retain the application theme's menu
    // delegate and margins. Refresh both after applying the scoped theme.
    for (auto* combo : root.findChildren<QComboBox*>()) {
        combo->ensurePolished();
        QEvent changed(QEvent::StyleChange);
        QCoreApplication::sendEvent(combo, &changed);
    }
    // Custom-painted views read the tokens when they paint.
    for (auto* child : root.findChildren<QWidget*>()) child->update();
    root.update();
}
}

QString uiFontFamily() { return fonts().ui; }
QString monoFontFamily() { return fonts().mono; }

QString indicators(Theme::Scheme scheme)
{
    const auto folder = VisualStyle::indicators(scheme);
    return folder.isEmpty() ? QStringLiteral(":/redesign") : folder;
}

void apply(QWidget& root, const QString& extraTemplate)
{
    auto& list = roots();
    list.removeIf([&](const Root& entry) { return !entry.widget || entry.widget == &root; });
    list.append({&root});
    // A switch sets each root's palette; the marked widgets re-polish after.
    static const bool following = [] {
        VisualStyle::followScheme(*qApp, [] {
            for (const auto& entry : roots()) if (entry.widget) setPalette(*entry.widget);
        });
        return true;
    }();
    Q_UNUSED(following);
    style(root, extraTemplate);
}
}
