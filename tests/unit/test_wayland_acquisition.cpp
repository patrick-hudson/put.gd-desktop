#include "record/WaylandAcquisition.h"
#include <QtTest>
using namespace putgd::record;

// The compositor-native Wayland paths (KWin screencast, Mutter screen cast)
// name a node on the session PipeWire daemon, so the probe only prefers them
// when the daemon's socket is where `pipewiresrc` will look for it.
class WaylandAcquisition : public QObject {
    Q_OBJECT
private slots:
    void findsTheSessionSocketLikePipewiresrc() {
        QTemporaryDir runtime, pipewireDir;
        QVERIFY(runtime.isValid() && pipewireDir.isValid());
        QProcessEnvironment env;
        env.insert("XDG_RUNTIME_DIR", runtime.path());
        QCOMPARE(pipewireSocket(env), QString());  // no daemon socket: the portal stays in charge
        QFile socket(runtime.filePath("pipewire-0")); QVERIFY(socket.open(QIODevice::WriteOnly)); socket.close();
        QCOMPARE(pipewireSocket(env), runtime.filePath("pipewire-0"));
        // PIPEWIRE_RUNTIME_DIR is looked at first, as PipeWire does; an empty one falls through.
        env.insert("PIPEWIRE_RUNTIME_DIR", pipewireDir.path());
        QCOMPARE(pipewireSocket(env), runtime.filePath("pipewire-0"));
        QFile other(pipewireDir.filePath("pipewire-0")); QVERIFY(other.open(QIODevice::WriteOnly)); other.close();
        QCOMPARE(pipewireSocket(env), pipewireDir.filePath("pipewire-0"));
        // PIPEWIRE_REMOTE names a different socket.
        env.insert("PIPEWIRE_REMOTE", "pipewire-1");
        QCOMPARE(pipewireSocket(env), QString());
        QFile named(runtime.filePath("pipewire-1")); QVERIFY(named.open(QIODevice::WriteOnly)); named.close();
        QCOMPARE(pipewireSocket(env), runtime.filePath("pipewire-1"));
    }
};
QTEST_GUILESS_MAIN(WaylandAcquisition)
#include "test_wayland_acquisition.moc"
