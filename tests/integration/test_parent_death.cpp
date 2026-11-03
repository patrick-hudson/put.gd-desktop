#include "ui/ChildProcess.h"
#include "../fixtures/Waits.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QtTest>
#include <cstdio>
#include <cstring>
#include <sys/wait.h>

using namespace putgd;

class ParentDeathTest : public QObject {
    Q_OBJECT
private slots:
    void killedParentLeavesNoHelper_data() {
        QTest::addColumn<bool>("beforeArming");
        QTest::newRow("after-exec") << false;
        QTest::newRow("before-prctl") << true;
    }
    void killedParentLeavesNoHelper() {
        QFETCH(bool, beforeArming);
        // Adopt the orphan ourselves so the assertion proves it exited and was
        // reaped, independent of how promptly the container's PID 1 reaps it.
        QVERIFY(::prctl(PR_SET_CHILD_SUBREAPER, 1, 0L, 0L, 0L) == 0);
        QProcess parent;
        pid_t child = -1;
        auto cleanup = qScopeGuard([&] {
            if (parent.state() != QProcess::NotRunning) {
                parent.kill();
                parent.waitForFinished(1000);
            }
            if (child > 0) {
                ::kill(child, SIGKILL);
                ::waitpid(child, nullptr, 0);
            }
            ::prctl(PR_SET_CHILD_SUBREAPER, 0, 0L, 0L, 0L);
        });
        parent.start(QCoreApplication::applicationFilePath(), {beforeArming ? "--parent-race" : "--parent"});
        QVERIFY(parent.waitForStarted(1000));
        QVERIFY(parent.waitForReadyRead(1000));
        const auto bytes = parent.read(sizeof(child));
        QCOMPARE(bytes.size(), qsizetype(sizeof(child)));
        std::memcpy(&child, bytes.constData(), sizeof(child));
        QVERIFY(child > 0);
        QVERIFY(::kill(child, 0) == 0);
        QElapsedTimer elapsed;
        elapsed.start();
        if (!beforeArming) parent.kill(); // No application destructor runs.
        QVERIFY(parent.state() == QProcess::NotRunning || parent.waitForFinished(1000));
        parent.closeWriteChannel(); // Release the race fixture only after parent death.
        int status = 0;
        pid_t reaped = 0;
        PUTGD_TRY_VERIFY_WITH_TIMEOUT(reaped != 0 || (reaped = ::waitpid(child, &status, WNOHANG)) != 0,
                                     qMax(qint64(0), 1000 - elapsed.elapsed()));
        QCOMPARE(reaped, child);
        child = -1;
        QVERIFY(elapsed.elapsed() < 1000);
        if (beforeArming) {
            QVERIFY(WIFEXITED(status));
            QCOMPARE(WEXITSTATUS(status), 127);
        } else {
            QVERIFY(WIFSIGNALED(status));
            QCOMPARE(WTERMSIG(status), SIGTERM);
        }
    }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().contains("--parent-race")) {
        QProcess helper;
        terminateWithParent(helper);
        const auto modifier = helper.childProcessModifier();
        int ready[2];
        if (::pipe(ready) != 0) return 4;
        const auto child = ::fork();
        if (child < 0) return 4;
        if (child > 0) {
            ::close(ready[1]);
            char gate;
            if (::read(ready[0], &gate, 1) != 1) ::_exit(5);
            ::_exit(0);
        }
        // Exercise the same child callback with the parent already dead. The
        // test keeps stdin open until it has observed the fixture's exit.
        const auto pid = ::getpid();
        if (::write(STDOUT_FILENO, &pid, sizeof(pid)) != sizeof(pid)) ::_exit(5);
        ::close(ready[0]);
        if (::write(ready[1], "x", 1) != 1) ::_exit(5);
        ::close(ready[1]);
        char gate;
        if (::read(STDIN_FILENO, &gate, 1) != 0) ::_exit(6);
        modifier();
        ::_exit(7); // The getppid guard must exit before returning here.
    }
    if (app.arguments().contains("--parent")) {
        QProcess helper;
        // Always use a stand-in so this also runs without gsettings or schemas.
        // It goes through exactly the modifier used by all three UI spawns.
        const auto sleep = QStandardPaths::findExecutable("sleep");
        if (sleep.isEmpty()) return 2;
        terminateWithParent(helper);
        helper.start(sleep, {"60"});
        if (!helper.waitForStarted(1000)) return 3;
        const auto pid = static_cast<pid_t>(helper.processId());
        std::fwrite(&pid, sizeof(pid), 1, stdout);
        std::fflush(stdout);
        return app.exec();
    }
    ParentDeathTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_parent_death.moc"
