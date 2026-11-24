// Lab driver for the scroll capture's helper side (#1376): one capture through
// the production ScrollHelperBackend and the real putgd-record, with no
// resident. It prints every backend event as a JSON line and, with --ruler,
// checks the PNG against the bar-coded page of scroll_page_fixture. Lab input:
// auto-scroll turns the wheel on whatever is under the rectangle, so point it
// only at a window opened for the run. Never desktop proof by itself.
//
//   test_scroll_capture_driver --helper PATH --rect X,Y,W,H --out DIR
//       [--driver user|putgd] [--auto-at-ms N] [--from-top] [--max-height N]
//       [--stop-after-ms N] [--stop-file PATH] [--finish-key TEXT] [--ruler FIRST,LAST[,HEADER,FOOTER]]
//
// --auto-at-ms turns auto-scroll on that long after capturing began;
// --stop-after-ms is Done, and so is PATH appearing (--stop-file).
#include "app/ScrollHelperBackend.h"
#include "fixtures/ScrollRuler.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <cstdio>

using namespace putgd;

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addOptions({{"helper", "putgd-record to run", "path"}, {"rect", "the region, desktop pixels", "x,y,w,h"}, {"out", "an existing private directory for the PNG", "dir"},
        {"from-top", "scroll to the top first"}, {"max-height", "height cap", "rows", "30000"}, {"stop-after-ms", "send stop this long after capturing began", "ms", "0"},
        {"ruler", "verify the ruler page: FIRST,LAST[,HEADER,FOOTER]", "rows"}, {"driver", "who scrolls at the start: user or putgd", "who", "user"},
        {"auto-at-ms", "turn auto-scroll on this long after capturing began", "ms", "0"}, {"finish-key", "the key the bar names", "text", ""},
        {"stop-file", "Done once this file exists", "path"}});
    parser.process(app);
    const auto r = parser.value("rect").split(',');
    if (r.size() != 4 || !parser.isSet("helper") || !parser.isSet("out")) { std::fprintf(stderr, "usage: --helper PATH --rect X,Y,W,H --out DIR\n"); return 2; }
    ScrollHelperBackend backend(parser.value("helper"));
    const QString id = QStringLiteral("lab");
    int code = 3;
    QJsonObject finished;
    const auto print = [](const QJsonObject& object) { std::printf("%s\n", QJsonDocument(object).toJson(QJsonDocument::Compact).constData()); std::fflush(stdout); };
    backend.setEventSink([&](QJsonObject event) {
        print(event);
        const auto name = event["event"].toString();
        if (name != "finished" && name != "failed" && name != "cancelled") return;
        if (name == "finished") { finished = event; code = 0; } else code = 1;
        backend.discard(id);
        QTimer::singleShot(1500, &app, [&] { app.exit(code); });
    });
    bool probed = false;
    backend.refreshCapabilities([&] { probed = true; print(QJsonObject{{"capabilities", backend.capabilities()}}); });
    const QJsonObject params{{"rect", QJsonArray{r[0].toInt(), r[1].toInt(), r[2].toInt(), r[3].toInt()}}, {"maxHeight", parser.value("max-height").toInt()}, {"fromTop", parser.isSet("from-top")}, {"autoStart", true}, {"selection", "helper"},
        {"driver", parser.value("driver")}, {"finishKey", parser.value("finish-key")}};
    backend.prepare(id, params, parser.value("out"), [&](Error error, QJsonObject result) {
        if (error) { print(QJsonObject{{"prepare", error.toJson()}}); backend.discard(id); QTimer::singleShot(1500, &app, [&] { app.exit(1); }); return; }
        print(QJsonObject{{"prepare", result}});
        const int stopAfter = parser.value("stop-after-ms").toInt();
        if (stopAfter > 0) QTimer::singleShot(stopAfter, &app, [&] { backend.control(id, "stop", {}, [&](Error e, QJsonObject reply) { print(QJsonObject{{"stop", e ? e.toJson() : reply}}); }); });
        if (parser.isSet("stop-file")) {
            auto* poll = new QTimer(&app);
            const QString file = parser.value("stop-file");
            QObject::connect(poll, &QTimer::timeout, &app, [&, poll, file] {
                if (!QFileInfo::exists(file)) return;
                poll->stop();
                backend.control(id, "stop", {}, [&](Error e, QJsonObject reply) { print(QJsonObject{{"stop", e ? e.toJson() : reply}}); });
            });
            poll->start(100);
        }
        const int autoAt = parser.value("auto-at-ms").toInt();
        if (autoAt > 0) QTimer::singleShot(autoAt, &app, [&] { backend.control(id, "autoScroll", {{"on", true}}, [&](Error e, QJsonObject reply) { print(QJsonObject{{"autoScroll", e ? e.toJson() : reply}}); }); });
    });
    QTimer::singleShot(900000, &app, [&] { std::fprintf(stderr, "no end within 900 s\n"); app.exit(4); });
    const int ended = app.exec();
    Q_UNUSED(probed);
    if (ended != 0 || !parser.isSet("ruler")) return ended;
    const auto rows = parser.value("ruler").split(',');
    QImage image;
    if (rows.size() < 2 || !image.load(finished["path"].toString(), "PNG")) { std::fprintf(stderr, "the PNG could not be read\n"); return 5; }
    namespace ruler = putgd::test::ruler;
    const int header = rows.value(2).toInt(), footer = rows.value(3).toInt();
    const int body = image.height() - header - footer;
    // A stopped or capped capture ends inside a row: check what is there.
    const bool whole = finished["endReason"].toString() == "end_of_content" || (finished["endReason"].toString() == "done" && finished["reachedEnd"].toBool());
    const int last = whole ? rows[1].toInt() : rows[0].toInt() + (body - 1) / ruler::rowHeight;
    const auto verdict = ruler::verify(image, header, footer, rows[0].toInt(), last, 0, whole ? ruler::rowHeight - 1 : (body - 1) % ruler::rowHeight);
    std::printf("{\"ruler\":{\"ok\":%s,\"rows\":%d,\"width\":%d,\"height\":%d,\"problem\":\"%s\"}}\n", verdict.ok ? "true" : "false", verdict.rows, image.width(), image.height(), qPrintable(verdict.problem));
    return verdict.ok ? 0 : 6;
}
