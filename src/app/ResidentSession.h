#pragma once
#include "LocalIpc.h"
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <memory>
class QSocketNotifier;
namespace putgd {
// #490: a resident belongs to the login session that started it. Cinnamon
// starts autostart entries inside the session's scope, and with logind's
// KillUserProcesses=no (the Mint and Ubuntu default) whatever is left there
// outlives the logout: the session stays "closing" while the process runs.
struct LoginSessionState {QString id;bool exists=false;QString state,type;};
// logind's view of sessions, read through sd-login (libsystemd, loaded at run
// time; every resident already maps it through libdbus). Tests supply a fake.
class LoginSessions {
public:
 virtual ~LoginSessions()=default;
 virtual bool available()const=0;
 // The session whose scope holds pid. Empty outside one: a process a systemd
 // unit started (GNOME's and Plasma's autostart) ends with that unit instead.
 virtual QString sessionOf(qint64 pid)const=0;
 virtual LoginSessionState state(const QString& id)const=0;
 // A descriptor that turns readable when any session changes; -1 without one.
 virtual int monitor()=0;
 virtual void flush()=0;
 static std::unique_ptr<LoginSessions> system();
};
// x11, wayland and mir sessions own a display. An SSH session (tty) that
// started a resident for a desktop's DISPLAY does not end that desktop.
bool graphicalSessionType(const QString& type);
// Whether a launcher must replace the resident it found instead of handing off.
struct ResidentSession {bool ended=false;QString reason;LoginSessionState session;};
ResidentSession residentSession(const LoginSessions&,qint64 residentPid,qint64 launcherPid);
// Ends the resident peer on socketPath: ui.quit first, then SIGTERM and SIGKILL
// to that exact process (PID and start time). done once it has exited.
struct Retirement {bool done=false;QString how;};
Retirement retireResident(const QString& socketPath,const LocalPeer&,int quitMs=10000,int terminateMs=3000);
// The resident's own session: ended() once logind reports it closing or gone.
class SessionWatch final:public QObject {
 Q_OBJECT
public:
 explicit SessionWatch(std::unique_ptr<LoginSessions> sessions=LoginSessions::system(),qint64 pid=0,QObject* parent=nullptr);
 ~SessionWatch() override;
 // {id, type, watched, reason}; reason says why a session is not watched.
 QJsonObject status()const{return status_;}
signals:
 void ended(const QString& reason,const QJsonObject& session);
private:
 void check();
 std::unique_ptr<LoginSessions> sessions_;
 QPointer<QSocketNotifier> notifier_;
 QString id_;
 QJsonObject status_;
 bool ended_=false;
};
}
