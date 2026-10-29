#include "Ui.h"
#include "Fonts.h"

#include <QApplication>
#include <QPointer>
#include <QWidget>

namespace putgd::Ui {
namespace {
QList<QPointer<QWidget>>& roots()
{
    static QList<QPointer<QWidget>> list;
    return list;
}
}

void styleWindow(QWidget& root, const QString& extraTemplate)
{
    // Roots carry the scheme's palette themselves: the application palette
    // reaches widgets only while the event loop runs.
    auto& list = roots();
    list.removeIf([&](const QPointer<QWidget>& entry) { return !entry || entry == &root; });
    list.append(&root);
    static const bool following = [] {
        followScheme(*qApp, [] {
            for (const auto& entry : roots()) if (entry) entry->setPalette(palette());
        });
        return true;
    }();
    Q_UNUSED(following);
    // The app style paints every control from the tokens; the root only
    // carries the bundled font and, for a window not yet styled, the style.
    Fonts::load();
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) apply(*app);
    root.setFont(font(Type::body));
    root.setPalette(palette());
    root.setProperty("uiRoot", true);
    // A remaining window-specific stylesheet keeps both schemes (#237).
    if (!extraTemplate.isEmpty())
        root.setStyleSheet(schemeScoped([&extraTemplate](Theme::Scheme scheme) {
            auto stylesheet = Theme::stylesheet(extraTemplate, scheme);
            stylesheet.replace(QStringLiteral("%UI_FONT%"), Fonts::ui());
            stylesheet.replace(QStringLiteral("%MONO_FONT%"), Fonts::mono());
            stylesheet.replace(QStringLiteral("%INDICATORS%"), indicators(scheme));
            return stylesheet;
        }));
    root.update();
}
}
