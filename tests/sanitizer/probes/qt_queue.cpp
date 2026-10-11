#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>
#include <QThread>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QThread thread;
    QObject worker;
    worker.moveToThread(&thread);
    thread.start();
    QMetaObject::invokeMethod(&worker, [&app] {
        QMetaObject::invokeMethod(&app, [&app] { app.quit(); }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
    const int result = app.exec();
    thread.quit();
    thread.wait();
    return result;
}
