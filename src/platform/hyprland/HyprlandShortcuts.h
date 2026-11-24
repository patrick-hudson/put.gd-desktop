#pragma once
#include "platform/wayland/ShortcutBackend.h"
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>

namespace putgd {
class HyprlandIpc;
// Global shortcuts on Hyprland 0.56+ (Lua configuration). The resident
// registers putgd:<action> with hyprland_global_shortcuts_manager_v1 on its own
// Wayland connection and binds each key at runtime over IPC:
//   eval hl.bind("CTRL + SHIFT + 1", hl.dsp.global("putgd:region"),
//                { description = "put.gd: Region capture" })
// A press arrives as the protocol's pressed event and becomes activated().
// Nothing is written to the user's configuration files.
//
// Rules, from the measurements on Omarchy 4.5 (#1275):
// - Hyprland fires every bind on a key, so a key another bind already uses is
//   reported as shortcut_conflict and never bound.
// - "reload" drops every runtime bind, other programs' too, so put.gd doesn't use
//   it to remove its own. hl.unbind("MODS + KEY") removes exactly the binds on that
//   chord, in every submap (measured on 0.56.2), so it is safe on a chord only
//   put.gd's bind holds. A chord something else also uses goes through reload,
//   which restores the configuration's binds, because unbind would take those too.
// - A bind handle's :remove() is never called: a stale handle crashed Hyprland.
// - Every configreloaded event (Omarchy reloads on theme and settings changes)
//   drops runtime binds; they are re-applied.
// - j/binds is the truth: only binds that read back are reported bound. put.gd's
//   are the ones whose description starts with "put.gd: ".
class HyprlandShortcuts final:public ShortcutBackend {
 Q_OBJECT
public:
 struct Chord {
  int modmask=0;QString key;  // xkb modifier bits; lower-case keysym name
  bool operator==(const Chord& other) const {return modmask==other.modmask&&key==other.key;}
 };
 explicit HyprlandShortcuts(HyprlandIpc* ipc,QObject* parent=nullptr);
 ~HyprlandShortcuts() override;
 QString name() const override {return QStringLiteral("hyprland-ipc");}
 bool available() const override;
 void bind(const QHash<QString,QString>& desired) override;
 QJsonObject status() const override;
 void setPaused(bool paused,std::function<void(Error)> reply) override;
 // Blocking: the event loop no longer runs when the resident quits.
 void clear() override;
 // Why shortcuts are unavailable, or empty.
 QString error() const;
 // The registration and bind state, for diagnostics.
 QJsonObject diagnostics() const;

 // Shared with the contract tests.
 static constexpr int shift=1,control=4,alt=8,super=64;
 static std::optional<Chord> chordFromPortable(const QString& portable,QString* error=nullptr);
 // A j/binds entry; nothing for mouse, catch-all and submap binds or unknown keys.
 static std::optional<Chord> chordFromBind(const QJsonObject& bind);
 // Hyprland 0.56's j/binds gives a "code:N" bind an empty key and keycode 0;
 // its plain-text "binds" reply keeps "SUPER + code:10". Adds that text as
 // "keyText" to each JSON entry with an empty key, when both lists agree.
 static QJsonArray withTextKeys(QJsonArray binds,const QByteArray& text);
 // "CTRL + SHIFT + 1" for hl.bind.
 static QString hyprlandKeys(const Chord&);
 static QString portable(const Chord&);
 static QString description(const QString& action);
 static bool ownDescription(const QString& description);
 static QString globalId(const QString& action) {return QStringLiteral("putgd:")+action;}
 static QString bindScript(const QString& action,const Chord&);
 static QString unbindScript(const Chord&);
 // Keeps Hyprland's layer animation off put.gd's selectors and recording overlays.
 static QString layerRuleScript();
 struct Plan {
  QHash<QString,Chord> apply;            // action -> chord to bind now
  QHash<QString,QJsonObject> conflicts;  // action -> the existing bind
  QHash<QString,QString> invalid;        // action -> why the key cannot be bound
  QSet<QString> present;                 // actions whose put.gd bind is already there
  QList<Chord> remove;                   // put.gd's chords to hl.unbind before binding
  bool reloadFirst=false;                // a put.gd bind must go and hl.unbind can't take it alone
 };
 static Plan plan(const QJsonArray& binds,const QHash<QString,QString>& desired);
private:
 struct Registry;
 void schedule(int delayMs=0);
 void apply();
 // j/binds with the text reply's key names merged in.
 void readBinds(std::function<void(QJsonArray,Error)>);
 void applyBinds(QJsonArray binds,int reloads);
 void evalNext(QList<QPair<QString,Chord>> queue);
 void readBack();
 void finish();
 void reloadThen(std::function<void(Error)>);
 void unbindThen(QList<Chord>,std::function<void()>);
 void configReloaded();
 void ensureLayerRule(std::function<void()> next);
 HyprlandIpc* ipc_;
 std::unique_ptr<Registry> registry_;
 QHash<QString,QString> requested_;
 QHash<QString,QJsonObject> conflicts_;QHash<QString,QString> invalid_,failed_;QHash<QString,QStringList> actual_;
 QSet<QString> bound_;
 QString evalError_,ipcError_;
 bool paused_=false,applying_=false,reapply_=false,layerRule_=false,applied_=false;
 QList<std::function<void(Error)>> pauseReplies_;
 QTimer debounce_;
 quint64 reloadsIssued_=0,reloadsSeen_=0,applies_=0,unbinds_=0;int ipcRetryMs_=0;
};
}
