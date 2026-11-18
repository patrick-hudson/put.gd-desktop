// #1613: renders the review panel in each state, in both themes, and the
// folded Edits overlay over a 1100x700 window, as PNGs for the pull request's
// pictures. Not shipped, not a test: `review_panel_render <output directory>`.
#include "record/ReviewPanel.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"
#include "ui/Ui.h"
#include <QApplication>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QTimer>

using namespace putgd;
using namespace putgd::record;

namespace {
constexpr qint64 second = 1000000, duration = 32 * second;
RecordingMask cover(const QString& id, const QString& kind, qint64 start, qint64 end, int strength = 16) {
    RecordingMask m;
    m.id = id; m.kind = kind; m.rect = QRect(10, 10, 100, 40); m.start = start; m.end = end; m.strength = strength;
    return m;
}
ReviewPanel::State edited() {
    ReviewPanel::State s;
    s.durationUs = duration;
    s.fps = 30;
    s.edit.trimStart = 2600000;
    s.edit.trimEnd = 29400000;
    s.edit.masks = {cover("m1", "solid", 8200000, 12600000), cover("m2", "blur", 22100000, 24900000, 24)};
    RecordingCut cut;
    cut.id = "c1"; cut.start = 13 * second; cut.end = 19500000;
    s.edit.cuts = {cut};
    return s;
}
void pump() { for (int i = 0; i < 4; ++i) QApplication::processEvents(); }
}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (argc < 2) return 2;
    const QDir out(QString::fromLocal8Bit(argv[1]));
    Fonts::load();
    Ui::install(app);
    struct Scene { QString name; ReviewPanel::State state; };
    ReviewPanel::State empty;
    empty.durationUs = duration;
    auto selection = edited();
    selection.timeSelection = recording::Span{8200000, 12600000};
    auto cut = edited();
    cut.selectedKind = "cut"; cut.selectedId = "c1";
    auto redaction = edited();
    redaction.selectedKind = "cover"; redaction.selectedId = "m1";
    redaction.coverFirstKept = 8200000; redaction.coverLastKept = 12566667;
    auto blur = edited();
    blur.selectedKind = "cover"; blur.selectedId = "m2";
    blur.coverFirstKept = 22100000; blur.coverLastKept = 24866667;
    auto excluded = edited();
    excluded.selectedKind = "cover"; excluded.selectedId = "m1";
    const QList<Scene> scenes{{"empty", empty}, {"selection", selection}, {"cut", cut}, {"redaction", redaction}, {"blur", blur}, {"excluded", excluded}};
    for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
        Theme::setScheme(scheme);
        const QString suffix = Theme::name(scheme).toLower();
        for (const auto& scene : scenes) {
            ReviewPanel panel(std::make_shared<MaskNames>());
            panel.setFixedSize(280, 640);
            panel.setState(scene.state);
            panel.show();
            pump();
            panel.grab().save(out.filePath(QStringLiteral("panel-%1-%2.png").arg(scene.name, suffix)));
        }
        // The folded overlay over a 1100x700 window, a cut selected.
        QWidget window;
        window.setFixedSize(1100, 700);
        window.setAutoFillBackground(true);
        QPalette palette = window.palette();
        palette.setColor(QPalette::Window, Theme::current().well);
        window.setPalette(palette);
        auto* toggle = new QPushButton(QStringLiteral("Edits"), &window);
        toggle->setIcon(Ui::icon("panel-right"));
        toggle->setGeometry(1100 - 12 - 90, 49, 90, 34);
        auto* panel = new ReviewPanel(std::make_shared<MaskNames>(), &window);
        panel->setState(cut);
        panel->hide();
        EditsOverlay overlay(&window);
        window.show();
        pump();
        overlay.openFor(panel, toggle);
        pump();
        window.grab().save(out.filePath(QStringLiteral("overlay-%1.png").arg(suffix)));
        overlay.dismiss();
    }
    return 0;
}
