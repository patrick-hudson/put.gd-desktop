#include "RedesignTheme.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPointer>
#include <QResource>
#include <QTemporaryDir>
#include <QWidget>
#include <functional>
#include <memory>

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
// Indicator glyphs painted from the tokens, at 1x and @2x, so they change with
// the scheme. They live in a private temporary folder for this process.
void paintIndicators(const QString& folder, const Theme::Palette& p)
{
    auto save = [&](const QString& name, QSize size, const std::function<void(QPainter&)>& draw) {
        for (int scale : {1, 2}) {
            QImage image(size * scale, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.scale(scale, scale);
            draw(painter);
            painter.end();
            image.save(folder + "/" + name + (scale == 2 ? "@2x.png" : ".png"), "PNG");
        }
    };
    auto stroke = [](QPainter& painter, const QColor& color, qreal width, const QPainterPath& path) {
        painter.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    };
    save("check", {17, 17}, [&](QPainter& painter) {
        QPainterPath tick;
        tick.moveTo(4, 8.5); tick.lineTo(7, 11.5); tick.lineTo(13, 5.5);
        stroke(painter, p.onAccent, 1.8, tick);
    });
    save("chevron", {12, 12}, [&](QPainter& painter) {
        QPainterPath chevron;
        chevron.moveTo(2, 4); chevron.lineTo(6, 8); chevron.lineTo(10, 4);
        stroke(painter, p.textSecondary, 1.7, chevron);
    });
    for (const bool checked : {false, true}) {
        save(checked ? "radio-checked" : "radio", {18, 18}, [&](QPainter& painter) {
            painter.setPen(QPen(p.borderStrong, 1));
            painter.setBrush(p.surfaceRaised);
            painter.drawEllipse(QPointF(9, 9), 8, 8);
            if (!checked) return;
            painter.setPen(Qt::NoPen);
            painter.setBrush(p.accent);
            painter.drawEllipse(QPointF(9, 9), 4, 4);
        });
    }
}
struct Root {
    QPointer<QWidget> widget;
    QString extra;
};
QList<Root>& roots()
{
    static QList<Root> list;
    return list;
}
void style(QWidget& root, const QString& extra)
{
    const auto scheme = Theme::scheme();
    const auto& p = Theme::current();
    QFont font(uiFontFamily());
    font.setPixelSize(14);
    root.setFont(font);
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
    root.setProperty("redesignRoot", true);
    root.setProperty("themeScheme", Theme::name(scheme));
    root.setAttribute(Qt::WA_StyledBackground, true);
    QFile file(QStringLiteral(":/redesign/redesign.qss"));
    if (!file.open(QIODevice::ReadOnly)) return;
    auto stylesheet = Theme::stylesheet(QString::fromUtf8(file.readAll()) + extra, scheme);
    stylesheet.replace(QStringLiteral("%UI_FONT%"), uiFontFamily());
    stylesheet.replace(QStringLiteral("%MONO_FONT%"), monoFontFamily());
    stylesheet.replace(QStringLiteral("%INDICATORS%"), indicators(scheme));
    root.setStyleSheet(stylesheet);
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
    static std::unique_ptr<QTemporaryDir> folder;
    static QHash<int, QString> painted;
    if (const auto found = painted.constFind(int(scheme)); found != painted.cend()) return *found;
    if (!folder) folder = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/putgd-theme-XXXXXX");
    if (!folder->isValid()) return QStringLiteral(":/redesign");
    const auto path = folder->filePath(Theme::name(scheme));
    QDir().mkpath(path);
    paintIndicators(path, Theme::palette(scheme));
    painted.insert(int(scheme), path);
    return path;
}

void apply(QWidget& root, const QString& extraTemplate)
{
    auto& list = roots();
    list.removeIf([&](const Root& entry) { return !entry.widget || entry.widget == &root; });
    list.append({&root, extraTemplate});
    static const bool following = [] {
        QObject::connect(&Theme::Notifier::instance(), &Theme::Notifier::changed, [] {
            for (const auto& entry : roots()) if (entry.widget) style(*entry.widget, entry.extra);
        });
        return true;
    }();
    Q_UNUSED(following);
    style(root, extraTemplate);
}
}
