#pragma once
#include "OperationService.h"
#include "core/SettingsService.h"
#include "core/Update.h"
#include <QDateTime>
#include <QHash>
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
//
// AppImage builds can also install the update (#293 phase 3), only on an
// explicit request (Settings' Download and install, update.install with
// confirm). `putgd-cli --install-update CHANNEL` does the download, checks and
// file swap; the resident keeps its progress lines and, at its next start,
// removes the backup of the old image once the new version runs from the path.
//
// An AppImage whose chosen channel isn't its own switches instead (#344): the
// other channel's newest release, named by the last check, installed the same
// way but checked with that channel's key and permalink. Its version may be
// older, which update.install must allow explicitly.
//
// A DEB or RPM whose pkg.put.gd repository file is in use follows that
// repository's channel instead of the setting (#386). Startup reads the local
// file before Settings opens (#491); each check refreshes it independently of
// the feed. put.gd only explains how to move channels, never changes the file.
class UpdateService final:public QObject {
 Q_OBJECT
public:
 // An install may download 115 MB over a slow line; the child stops stalled
 // transfers itself.
 static constexpr int installTimeoutMs=60*60*1000;
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
 // What updating this installation means here, from local repository state:
 // the system's software updater when the package's repository file is in
 // use, else a download. Available versions still require a feed check.
 QJsonObject action() const;
 // Starts that updater (Update Manager, Discover or GNOME Software) through
 // `putgd-cli --open-software-updates`, which reads the file again.
 void openSoftwareUpdates(JsonReply);
 // update.install {confirm: true, method?: auto|zsync|download, channel?,
 // allowOlder?}: starts the install child and replies with status() at once;
 // progress follows in status()["install"]. A switch must name its channel,
 // and allowOlder: true when its version is older than this build's.
 void install(const QJsonObject& params,JsonReply);
 // update.restart {confirm: true}: once an installed update waits for a
 // restart, starts the image again from the host and quits this resident.
 void restart(const QJsonObject& params,JsonReply);
 // Why nothing may be installed or restarted now (a recording or an upload is
 // running), or empty.
 void setBusy(std::function<QString()> busy) {busy_=std::move(busy);}
 void setQuit(std::function<void()> quit) {quit_=std::move(quit);}
 // Test seams.
 void setClock(std::function<QDateTime()> clock) {clock_=std::move(clock);}
 void setRandom(std::function<qint64(qint64,qint64)> random) {random_=std::move(random);}
 void setBuild(update::Build build) {build_=std::move(build);}
 void setProgram(QString program,QStringList prefix={}) {program_=std::move(program);prefix_=std::move(prefix);}
 // The AppImage this resident runs from ($APPIMAGE); empty outside one.
 void setImage(QString image) {image_=std::move(image);}
 // Starts the image once this process has exited; true when it started.
 void setRestarter(std::function<bool(qint64 pid,const QString& image)> restarter) {restarter_=std::move(restarter);}
 qint64 nextCheckInMs() const {return timer_.isActive()?timer_.remainingTime():-1;}
signals:
 void changed();
 // A newer stable version that has not been announced before.
 void updateAvailable(const QJsonObject& result);
 // The install child put a new image in place of this one.
 void imageReplaced();
private:
 QString channel() const;
 // The locally detected channel of a DEB/RPM repository, or empty.
 QString repositoryChannel() const;
 void refreshSystemAction();
 bool notifies(const QJsonObject& action) const;
 QJsonObject installStatus() const;
 QString installUnavailable() const;
 // {from, to, version, tag, older} when update.install would switch channels
 // to a release the last check found; else empty.
 QJsonObject switchOffer() const;
 bool restartRequired() const;
 void installLine(const QByteArray& line);
 void installFinished(const QByteArray& last,const Error& failure);
 bool enabled() const;
 void schedule();
 void run(bool manual);
 void finished(bool manual,const QByteArray& output,const Error& failure);
 void save();
 QDateTime now() const {return clock_?clock_():QDateTime::currentDateTimeUtc();}
 qint64 random(qint64 low,qint64 high) const;
 SettingsService& settings_;
 QString stateRoot_,statePath_,program_,image_;
 QStringList prefix_;
 update::Build build_;
 QTimer timer_,deadline_;
 QPointer<QProcess> process_;
 QList<JsonReply> waiting_;
 bool manualRunning_=false;
 QDateTime startedAt_,attemptedAt_,checkedAt_,nextAt_;
 qint64 startupDelayMs_=0,jitter_=0;
 QJsonObject result_,error_,systemAction_;
 QString attemptedChannel_;
 // The version last announced or seen in Settings, by channel.
 QHash<QString,QString> notified_;
 std::function<QDateTime()> clock_;
 std::function<qint64(qint64,qint64)> random_;
 // The install child, its live progress and update-install.json as last read.
 QPointer<QProcess> installer_;
 QTimer installDeadline_;
 QJsonObject install_,installState_;
 std::function<QString()> busy_;
 std::function<void()> quit_;
 std::function<bool(qint64,const QString&)> restarter_;
};
}
