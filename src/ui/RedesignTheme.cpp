#include "RedesignTheme.h"
#include "Fonts.h"
#include "Graphite.h"
#include "VisualStyle.h"

#include <QApplication>
#include <QPointer>
#include <QWidget>

namespace putgd::RedesignTheme {
QString uiFontFamily() { return Fonts::ui(); }
QString monoFontFamily() { return Fonts::mono(); }

QString indicators(Theme::Scheme scheme)
{
    return VisualStyle::indicators(scheme);
}

namespace {
QList<QPointer<QWidget>>& roots()
{
    static QList<QPointer<QWidget>> list;
    return list;
}
}

void apply(QWidget& root, const QString& extraTemplate)
{
    // Roots carry the scheme's palette themselves: the application palette
    // reaches widgets only while the event loop runs.
    auto& list = roots();
    list.removeIf([&](const QPointer<QWidget>& entry) { return !entry || entry == &root; });
    list.append(&root);
    static const bool following = [] {
        VisualStyle::followScheme(*qApp, [] {
            for (const auto& entry : roots()) if (entry) entry->setPalette(Graphite::palette());
        });
        return true;
    }();
    Q_UNUSED(following);
    // The Graphite style paints every control from the tokens; the root only
    // carries the bundled font and, for a window not yet styled, the style.
    Fonts::load();
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) VisualStyle::apply(*app);
    root.setFont(Graphite::font(Graphite::Type::body));
    root.setPalette(Graphite::palette());
    root.setProperty("redesignRoot", true);
    // A remaining window-specific stylesheet keeps both schemes (#237).
    if (!extraTemplate.isEmpty())
        root.setStyleSheet(VisualStyle::schemeScoped([&extraTemplate](Theme::Scheme scheme) {
            auto stylesheet = Theme::stylesheet(extraTemplate, scheme);
            stylesheet.replace(QStringLiteral("%UI_FONT%"), uiFontFamily());
            stylesheet.replace(QStringLiteral("%MONO_FONT%"), monoFontFamily());
            stylesheet.replace(QStringLiteral("%INDICATORS%"), indicators(scheme));
            return stylesheet;
        }));
    root.update();
}
}
