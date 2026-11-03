#include "ui/ChildProcess.h"
#include <QCoreApplication>
#include <QProcess>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QtTest>
#include <cstdio>
#include <sys/wait.h>

using namespace putgd;

class ParentDeathTest : public QObject {
    Q_OBJECT
private slots:
    void killedParentLeavesNoHelper() {
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
        parent.start(QCoreApplication::applicationFilePath(), {"--parent"});
        QVERIFY(parent.waitForStarted(1000));
        QVERIFY(parent.waitForReadyRead(1000));
        bool ok = false;
        child = parent.readLine().trimmed().toInt(&ok);
        QVERIFY(ok && child > 0);
        QVERIFY(::kill(child, 0) == 0);
        parent.kill(); // SIGKILL: no QProcess or application destructor runs.
        QVERIFY(parent.waitForFinished(1000));
        int status = 0;
        pid_t reaped = 0;
        QTRY_VERIFY_WITH_TIMEOUT(reaped != 0 || (reaped = ::waitpid(child, &status, WNOHANG)) != 0, 1000);
        QCOMPARE(reaped, child);
        child = -1;
        QVERIFY(WIFSIGNALED(status));
        QCOMPARE(WTERMSIG(status), SIGTERM);
    }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().contains("--parent")) {
        QProcess helper;
        // Always use a stand-in so this also runs without gsettings or schemas.
        // It goes through exactly the modifier used by all three UI spawns.
        const auto sleep = QStandardPaths::findExecutable("sleep");
        if (sleep.isEmpty()) return 2;
        terminateWithParent(helper);
        helper.start(sleep, {"60"});
        if (!helper.waitForStarted(1000)) return 3;
        std::printf("%lld\n", static_cast<long long>(helper.processId()));
        std::fflush(stdout);
        return app.exec();
    }
    ParentDeathTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_parent_death.moc"
