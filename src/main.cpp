#include "storage/s3/SdkRuntime.h"
#include <QApplication>
#include <QLabel>
#include <QMainWindow>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("putgd"));
    QCoreApplication::setApplicationName(QStringLiteral("putgd"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    putgd::SdkRuntime sdk;
    QMainWindow window;
    window.setWindowTitle(QStringLiteral("put.gd"));
    window.setCentralWidget(new QLabel(QStringLiteral("put.gd development build"), &window));
    window.resize(720, 480);
    window.show();
    return app.exec();
}
