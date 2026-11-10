#pragma once
#include "core/RecordingClicks.h"
#include <QFileSystemWatcher>
#include <QObject>
#include <QTimer>

namespace putgd {
// Click highlights on Budgie 10.10 (labwc), #1364.
//
// labwc has no IPC, script interface or event stream. A press reaches put.gd
// in two halves:
//
// - That it happened: for the length of a recording, `<mousebind>` entries in
//   labwc's rc.xml run
//     dbus-send --session --type=method_call --dest=org.putgd.App.Budgie
//       /org/putgd/Clicks org.putgd.App.Budgie.Click uint32:<button>
//   on a press over a window or its title bar. A bind in the `Client` or
//   `Titlebar` context does not take the press from the window (labwc swallows
//   only `Frame` and `All` binds). A later bind replaces an earlier one with
//   the same context, button and modifiers, so each of ours carries the
//   actions of the bind it stands in for: the user's own, or labwc's default
//   Focus and Raise.
// - Where: the command carries no position. The recording helper reads the
//   pointer's place from the capture's cursor session (ImageCopySource), so
//   the press goes to it as `{button, pointer: true}`.
//
// rc.xml belongs to Budgie's labwc bridge, which keeps the file's tree in
// memory and writes it back at the next setting it mirrors. So this watches
// the file: while a recording wants clicks, binds the bridge dropped are put
// back; at every other time, binds found there (after a crash, or written
// back by a bridge that read the file during a recording) are taken out.
// Every element put.gd adds carries `putgd="click-highlights"`, which labwc
// ignores and the bridge preserves; nothing else in the file is touched.
//
// Not covered, and why: a press on a layer-shell surface (Budgie's panel) or
// an X11 override-redirect window matches only an `All` bind, which would
// swallow it; a press with a modifier combination not listed below matches no
// bind (labwc compares modifiers exactly); a window recording has no pointer
// position (wlroots' window capture source has no cursor source).
class BudgieClicks final : public QObject, public RecordingClicks {
    Q_OBJECT
public:
    // rcPath: labwc's rc.xml, for tests; empty means the running labwc's.
    explicit BudgieClicks(QObject* parent = nullptr, QString rcPath = {});
    ~BudgieClicks() override;
    QJsonObject clickCapability() const override;
    void watchClicks(const QJsonObject& target, RecordingClicks::Sink, RecordingClicks::Reply) override;
    void unwatchClicks() override;
    // The D-Bus adaptor's entry: 1 left, 2 middle, 3 right.
    void pressed(uint button);
    // rc.xml with put.gd's mousebinds added (replacing any already there), or
    // taken out. Text outside put.gd's own elements comes back unchanged.
    static QString withBinds(const QString& rc, QString* error = nullptr);
    static QString withoutBinds(const QString& rc, QString* error = nullptr);
    static bool hasBinds(const QString& rc);
    static QString command(int button);
    static constexpr const char* marker = "click-highlights";
private:
    // Makes rc.xml match `watching_` and has labwc read it again when it changed.
    Error sync();
    void changed();
    QString rcPath_;
    qint64 labwc_ = 0;
    bool fixedPath_ = false, watching_ = false, exported_ = false;
    RecordingClicks::Sink sink_;
    QFileSystemWatcher watcher_;
    QTimer settle_;
    QObject* adaptor_ = nullptr;
};
}
