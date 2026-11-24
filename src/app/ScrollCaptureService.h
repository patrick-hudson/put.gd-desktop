#pragma once
#include "ScrollBackend.h"
#include "RecordingService.h"
#include <QElapsedTimer>
#include <QHash>
#include <QTimer>
#include <functional>
namespace putgd {
// One private scroll session. The operation journal is committed before the
// backend may acquire pixels. Terminal descriptors remain inspectable.
class ScrollCaptureService final : public QObject {
 Q_OBJECT
public:
 using Host = RecordingService::Host;
 ScrollCaptureService(QString root, Host, QObject* parent=nullptr);
 ~ScrollCaptureService() override;
 void setBackend(std::unique_ptr<ScrollBackend>);
 void setShell(RecordingShell* shell) { shell_=shell; }
 bool active() const { return !id_.isEmpty(); }
 QString activeId() const { return id_; }
 QJsonObject capabilities() const;
 void command(const QString&,const QJsonObject&,JsonReply);
 void invoke(bool edit,JsonReply);
 void shutdown();
 // Capturing begins no sooner than this long from now: whoever heard
 // preparing() and took something off the screen (put.gd's own notifications)
 // gives it the time to go. `start` and `select` wait, they are not refused.
 // A later call only lengthens the hold, unless it says `fromNow`: what was
 // waited for has happened, and this much more is all that is needed.
 void holdStart(int ms,bool fromNow=false);
 void failPreparation(Error);
signals:
 // An image that is kept and is not the whole page: reason is its endReason
 // (height_cap, top_not_reached), message the note on its history entry.
 void warning(const QString& id,const QString& message,const QString& reason);
 // A session was accepted; nothing has been captured yet. Emitted before the
 // backend is asked, so a listener can clear the screen of put.gd's own things.
 void preparing();
 // The session reached its end (done, failed or cancelled).
 void ended();
private:
 void prepare(QJsonObject,bool edit,bool autoStart,JsonReply);
 void prepareProbed(QJsonObject,bool,bool,JsonReply);
 void placeShellBar();
 void event(QJsonObject);
 void paused(const QJsonObject&);
 void complete(QJsonObject);
 void rememberResult();
 void terminate(Error,bool cancelled=false);
 void update(QString);
 QString root_,id_,directory_,pending_;
 Host host_;
 Operation op_;
 QJsonObject view_,shownFrame_;
 QHash<QString,QJsonObject> results_;
 QStringList resultOrder_;
 std::unique_ptr<ScrollBackend> backend_;
 RecordingShell* shell_=nullptr;
 int requestedCap_=30000;
 bool shellPlacing_=false;
 // `start` asked for while the Shell was still placing the bar (GNOME): it
 // runs when the placement is done. And the hold of holdStart().
 JsonReply startWaiting_;
 QElapsedTimer clock_;qint64 notBefore_=0;bool held_=false;
 // The command the hold keeps back, and the timer that lets it go.
 std::function<void()> heldRun_;QTimer holdTimer_;
 void releaseHeld();
 void maybeStartHelper();
 bool helperAutoStart_=false;
 bool shellAutoStart_=false;
 bool shellSelecting_=false;
 bool edit_=false;
 // A Cancel is on its way to the backend, which answers it only when its
 // worker has ended. From here on it alone decides how the session ends: no
 // other command is passed on, and what an earlier one is answered or the
 // backend announces meanwhile (a Stop refused or finished, a finished image)
 // changes nothing. The session ends cancelled and the image is discarded.
 // A Stop whose validated image was admitted before the Cancel arrived has
 // ended the session already: that Cancel is answered scroll_not_found.
 bool cancelling_=false;
};
}
