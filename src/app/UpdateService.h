#pragma once
#include "OperationService.h"
#include "core/SettingsService.h"
#include "core/Update.h"
#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <functional>
namespace putgd {
// The daily update check (#293). The resident keeps only this timer and the
// last result; each check runs `putgd-cli --check-update CHANNEL` as a
// short-lived child, so the resident never loads Qt Network or TLS. The first
// check waits 5 to 15 minutes after start; later ones follow the last attempt
// by 24 hours, plus or minus 2, which is kept in update-check.json in the state
// folder so a restart does not check again early. Stable builds notify once per
// new version, unless the system's updater follows put.gd's repository (it
// notifies itself); nightly builds only report it in Settings.
class UpdateService final:public QObject {
 Q_OBJECT
public:
 static constexpr qint64 intervalMs=24LL*60*60*1000;
 static constexpr qint64 jitterMs=2LL*60*60*1000;
 static constexpr qint64 startupMinimumMs=5LL*60*1000,startupMaximumMs=15LL*60*1000;
 // The child's own transfer timeout is 20 s.
 static constexpr int childTimeoutMs=40000;
 UpdateService(SettingsService&,QString stateRoot,QString program,QObject* parent=nullptr);
 // Reads the saved state and schedules the next check; call once at startup.
 void start();
 QJsonObject status() const;
 // Checks now, whatever the schedule or the check setting says, and replies
 // with status(). A failed check replies with its error.
 void check(JsonReply);
 // What updating this installation means here: the system's software
 // updater when the package's repository file is in use, else a download.
 QJsonObject action() const;
 // Starts that updater (Update Manager, Discover or GNOME Software).
 void openSoftwareUpdates(JsonReply) const;
 // Test seams.
 void setClock(std::function<QDateTime()> clock) {clock_=std::move(clock);}
 void setRandom(std::function<qint64(qint64,qint64)> random) {random_=std::move(random);}
 void setBuild(update::Build build) {build_=std::move(build);}
 void setProgram(QString program,QStringList prefix={}) {program_=std::move(program);prefix_=std::move(prefix);}
 void setSystem(std::function<QJsonObject(const QString&)> actionFor,std::function<bool(const QJsonObject&)> open) {actionFor_=std::move(actionFor);openUpdater_=std::move(open);}
 qint64 nextCheckInMs() const {return timer_.isActive()?timer_.remainingTime():-1;}
signals:
 void changed();
 // A newer stable version that has not been announced before.
 void updateAvailable(const QJsonObject& result);
private:
 QString channel() const;
 bool notifies(const QJsonObject& action) const;
 bool enabled() const;
 void schedule();
 void run(bool manual);
 void finished(bool manual,const QByteArray& output,const Error& failure);
 void save();
 QDateTime now() const {return clock_?clock_():QDateTime::currentDateTimeUtc();}
 qint64 random(qint64 low,qint64 high) const;
 SettingsService& settings_;
 QString statePath_,program_;
 QStringList prefix_;
 update::Build build_;
 QTimer timer_,deadline_;
 QPointer<QProcess> process_;
 QList<JsonReply> waiting_;
 bool manualRunning_=false;
 QDateTime startedAt_,attemptedAt_,checkedAt_,nextAt_;
 qint64 startupDelayMs_=0,jitter_=0;
 QJsonObject result_,error_;
 QString notifiedVersion_,attemptedChannel_;
 std::function<QDateTime()> clock_;
 std::function<qint64(qint64,qint64)> random_;
 std::function<QJsonObject(const QString&)> actionFor_;
 std::function<bool(const QJsonObject&)> openUpdater_;
};
}
