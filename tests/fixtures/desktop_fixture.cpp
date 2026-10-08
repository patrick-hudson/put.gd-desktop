// Independent native Qt application: deterministic paint and real clipboard paste.
#include <QApplication>
#include <QClipboard>
#include <QCommandLineParser>
#include <QDir>
#include <QGuiApplication>
#include <QFile>
#include <QUrl>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMimeData>
#include <QPainter>
#include <QSaveFile>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <QWindow>

static bool saveJson(const QString& path, const QJsonObject& value) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write(QJsonDocument(value).toJson());
    return file.commit();
}

class Fixture final : public QWidget {
public:
    explicit Fixture(QString output) : output_(std::move(output)) {
        setWindowTitle("put.gd independent synthetic paint / paste fixture");
        setFocusPolicy(Qt::StrongFocus);
        resize(640, 420);
        move(120, 120);
    }
    void ready() {
        QJsonArray displays;
        for (const auto* screen : QGuiApplication::screens()) {
            const auto r = screen->geometry();
            displays.append(QJsonObject{{"name", screen->name()}, {"x", r.x()}, {"y", r.y()},
                {"width", r.width()}, {"height", r.height()},
                {"nativeWidth", qRound(r.width() * screen->devicePixelRatio())},
                {"nativeHeight", qRound(r.height() * screen->devicePixelRatio())}, {"scale", screen->devicePixelRatio()},
                {"rotation", int(screen->orientation())}});
        }
        const auto origin = mapToGlobal(QPoint(0, 0));
        const auto margins = windowHandle()->frameMargins();
        for (int index = 1; index <= 2; ++index) {
            QFile file(output_ + QString("/synthetic-%1.txt").arg(index));
            if (file.open(QIODevice::WriteOnly)) file.write(QString("putgd synthetic file %1\n").arg(index).toUtf8());
        }
        saveJson(output_ + "/ready.json", {{"schemaVersion", 1},
            {"session", qEnvironmentVariable("XDG_SESSION_TYPE")},
            {"desktop", qEnvironmentVariable("XDG_CURRENT_DESKTOP")},
            {"qt_platform", QGuiApplication::platformName()}, {"qt_version", qVersion()},
            {"display_layout", displays},
            {"frame", QJsonObject{{"x", frameGeometry().x()}, {"y", frameGeometry().y()},
                {"width", frameGeometry().width()}, {"height", frameGeometry().height()}}},
            {"decorationMargins", QJsonObject{{"left", margins.left()}, {"top", margins.top()},
                {"right", margins.right()}, {"bottom", margins.bottom()}}},
            {"paint", QJsonObject{{"x", origin.x()}, {"y", origin.y()}, {"width", 640}, {"height", 320}}}});
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(QRect(0, 0, 320, 160), QColor(220, 30, 40));
        painter.fillRect(QRect(320, 0, 320, 160), QColor(20, 180, 70));
        painter.fillRect(QRect(0, 160, 320, 160), QColor(35, 70, 220));
        painter.fillRect(QRect(320, 160, 320, 160), QColor(235, 190, 20));
        painter.fillRect(QRect(0, 320, width(), height() - 320), Qt::white);
        painter.setPen(Qt::black);
        painter.drawText(QRect(10, 330, width() - 20, 80), Qt::TextWordWrap,
                         "Synthetic fixture. Ctrl+V records actual clipboard MIME/image/text/files. " + status_);
    }
    void mousePressEvent(QMouseEvent*) override { setFocus(); }
    void keyPressEvent(QKeyEvent* event) override {
        if (event->modifiers() == (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier)) {
            if (event->key() == Qt::Key_T) {
                QApplication::clipboard()->setText("putgd synthetic clipboard text");
            } else if (event->key() == Qt::Key_I) {
                QImage image(96, 64, QImage::Format_RGB32);
                image.fill(QColor(220, 30, 40));
                QApplication::clipboard()->setImage(image);
            } else if (event->key() == Qt::Key_F) {
                auto* mime = new QMimeData;
                mime->setUrls({QUrl::fromLocalFile(output_ + "/synthetic-1.txt"),
                               QUrl::fromLocalFile(output_ + "/synthetic-2.txt")});
                QApplication::clipboard()->setMimeData(mime);
            } else { QWidget::keyPressEvent(event); return; }
            saveJson(output_ + "/source.json", {{"synthetic", true}, {"key", event->key()}});
            return;
        }
        if (event->matches(QKeySequence::Paste)) {
            const auto* mime = QApplication::clipboard()->mimeData();
            QJsonArray formats;
            for (const auto& format : mime->formats()) formats.append(format);
            QJsonObject result{{"schemaVersion", 1}, {"formats", formats}};
            if (mime->hasImage()) {
                const auto image = qvariant_cast<QImage>(mime->imageData());
                const bool saved = !image.isNull() && image.save(output_ + "/paste.png")
                    && image.save(output_ + "/paste.ppm", "PPM");
                result["image"] = QJsonObject{{"width", image.width()}, {"height", image.height()}, {"saved", saved}};
            }
            if (mime->hasText()) result["text"] = mime->text();
            if (mime->hasUrls()) {
                QJsonArray urls;
                for (const auto& url : mime->urls()) urls.append(url.toString());
                result["urls"] = urls;
            }
            saveJson(output_ + "/paste.json", result);
            status_ = "Paste recorded.";
            update();
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }
private:
    QString output_, status_;
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"output", "Private synthetic results directory", "directory"});
    parser.addOption({"convert", "Convert image to PPM (first positional argument is output)", "input"});
    parser.addPositionalArgument("ppm-output", "Output for --convert");
    parser.process(app);
    if (parser.isSet("convert")) {
        const auto positional = parser.positionalArguments();
        if (positional.size() != 1) return 2;
        const QImage image(parser.value("convert"));
        return !image.isNull() && image.save(positional[0], "PPM") ? 0 : 3;
    }
    const auto output = parser.value("output");
    if (output.isEmpty() || !QDir().mkpath(output)) return 2;
    Fixture fixture(output);
    fixture.show();
    QTimer::singleShot(1000, &fixture, [&fixture] { fixture.ready(); });
    return app.exec();
}
