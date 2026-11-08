#pragma once
#include "platform/wayland/ShortcutBackend.h"
#include <QFileSystemWatcher>
#include <QHash>
#include <QJsonArray>
#include <QProcessEnvironment>
#include <QQueue>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <optional>

class QProcess;
namespace putgd {
// Budgie 10.10's labwc bridge turns each relocatable media-keys custom keybinding
// into an rc.xml <keybind bridge="ID">. put.gd writes one entry per action at
// .../custom-keybindings/customputgd<action>/ (the bridge removes stale entries
// only when their ID contains "custom"), whose command calls the resident over
// D-Bus. The bridge detects no conflicts, so every key is compared with rc.xml
// first, and rc.xml stays the authority for what is reported as bound.
class LabwcBridgeBackend final:public ShortcutBackend {
 Q_OBJECT
public:
 struct Keybind {QString key,bridge,command;};
 // configDirectory: $XDG_CONFIG_HOME; empty means the user's.
 explicit LabwcBridgeBackend(QObject* parent=nullptr,QString configDirectory={});
 ~LabwcBridgeBackend() override;
 QString name()const override{return QStringLiteral("labwc-bridge");}
 bool available()const override{return available_;}
 void bind(const QHash<QString,QString>&) override;
 QJsonObject status()const override;
 void setPaused(bool,std::function<void(Error)>) override;
 void clear() override;
 // Whether this session has the bridge: the media-keys schema answers and
 // rc.xml exists. reply(available, reason).
 static void probe(std::function<void(bool,QString)> reply,QString configDirectory={});
 QString rcPath()const{return rcPath_;}
 // Shared with the contract tests.
 static QString gtkAccelerator(const QString& portable,QString* error=nullptr);
 static QString labwcKey(const QString& gtk);
 // Modifier set and lower-case keysym, so C-S-F9, S-C-f9 and S-C-F9 compare equal.
 static QString normalizedLabwcKey(const QString& labwc);
 static QList<Keybind> readKeybinds(const QString& path,QString* error=nullptr);
 static QString trigger(const QString& action);
 static QString pathFor(const QString& action);
 static QStringList parseStrv(const QString& gvariant);
 static QString strv(const QStringList&);
private:
 struct Job {QStringList arguments;std::function<void(int,QString)> done;};
 void run(QStringList arguments,std::function<void(int,QString)> done={});
 void next();
 void apply();
 void reread();
 void report();
 QString configDirectory_,rcPath_;QProcessEnvironment environment_;
 bool available_=false,paused_=false,applying_=false,reapply_=false;
 QHash<QString,QString> requested_;QHash<QString,QString> written_;QSet<QString> pending_;
 QHash<QString,QJsonObject> conflicts_;QHash<QString,QString> invalid_;
 QList<Keybind> keybinds_;QString rcError_,lastError_;
 QQueue<Job> jobs_;QProcess* process_=nullptr;
 QFileSystemWatcher watcher_;QTimer poll_,patience_;
};
}
