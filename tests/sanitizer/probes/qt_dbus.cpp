#include <QCoreApplication>
#include <QDBusConnection>
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    auto bus = QDBusConnection::sessionBus();
    return bus.isConnected() ? 0 : 1;
}
