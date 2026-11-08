#pragma once
#include "core/Error.h"
#include <QByteArray>
#include <QDeadlineTimer>
#include <QJsonDocument>
#include <QObject>
#include <QString>
#include <QTimer>
#include <functional>

class QSocketNotifier;
namespace putgd {
// Hyprland's IPC, spoken directly on its sockets (never through hyprctl, so an
// AppImage's library path never reaches a child process):
//   $XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket.sock
//     one plain-text request per connection ("j/activewindow", "j/clients",
//     "j/binds", "eval <lua>", "reload"); the reply ends when Hyprland closes it.
//   .../.socket2.sock
//     a line stream of "event>>data" (configreloaded>> after every reload).
class HyprlandIpc final:public QObject {
 Q_OBJECT
public:
 using Reply=std::function<void(QByteArray,Error)>;
 using JsonReply=std::function<void(QJsonDocument,Error)>;
 explicit HyprlandIpc(QObject* parent=nullptr);
 ~HyprlandIpc() override;
 // Empty unless HYPRLAND_INSTANCE_SIGNATURE and XDG_RUNTIME_DIR name a socket.
 static QString requestSocket();
 static QString eventSocket();
 // Bounded: an unreachable socket, a silent Hyprland (2 s) or an oversized
 // reply (8 MiB) fails with ipc_unavailable. Replies run on this object's thread.
 void request(const QString& text,Reply);
 // "j/..." requests parsed as JSON; anything else fails with ipc_reply.
 void json(const QString& text,JsonReply);
 // For the quit path, where the event loop no longer runs.
 static Error requestBlocking(const QString& text,QByteArray* reply,int timeoutMs=1000);
 // Starts the event stream; it reconnects after Hyprland restarts.
 void listen();
 bool listening() const;
 // "ok" is Hyprland's success reply to eval, reload and dispatch.
 static bool okReply(const QByteArray&);
 // A Lua string literal for any text: quotes, backslashes, control characters
 // and non-ASCII bytes are escaped, so it can never close the literal early.
 static QString luaString(const QString&);
signals:
 // name is the part before ">>", data the rest of the line.
 void event(const QString& name,const QString& data);
 void listeningChanged(bool);
private:
 void request(const QString& text,Reply,QDeadlineTimer deadline);
 void connectEvents();
 void dropEvents();
 void readEvents();
 int eventFd_=-1;QSocketNotifier* eventNotifier_=nullptr;QByteArray eventBuffer_;QTimer reconnect_;bool listen_=false;
};
}
