#pragma once
#include "HyprlandIpc.h"
#include "core/RecordingClicks.h"
#include <QJsonArray>
#include <QObject>

namespace putgd {
// Click highlights on Hyprland (#1364). Three runtime binds, written through
// `eval` like put.gd's shortcut binds and never into a file, fire on the bare
// mouse buttons without consuming them (`non_consuming`, whatever modifiers are
// held), so the click still reaches the window under the pointer. Their Lua
// callback reads the cursor and sends it as a custom event on Hyprland's event
// socket, which the resident reads. No process starts per click.
//
// The binds do nothing unless the global `putgd_clicks` holds this recording's
// token, so one a crash left behind is inert. They are removed at Stop with
// `hl.unbind`, by the adapter's rule: only on a chord nothing else holds, and
// never through a bind handle. Where the user has a bind of their own on a
// bare mouse button, put.gd's stay, inert, until Hyprland next reloads.
class HyprlandClicks final : public QObject, public RecordingClicks {
    Q_OBJECT
public:
    explicit HyprlandClicks(QObject* parent = nullptr);
    ~HyprlandClicks() override;
    QJsonObject clickCapability() const override;
    void watchClicks(const QJsonObject& target, RecordingClicks::Sink, RecordingClicks::Reply) override;
    void unwatchClicks() override;
    static constexpr const char* description = "put.gd: click highlight";
    // The Lua that arms the binds for `token`, adding them when `bind`.
    static QString armScript(const QString& token, bool bind);
    static QString disarmScript();
    // The bare mouse chords put.gd may unbind: those no other bind holds.
    static QStringList removable(const QJsonArray& binds);
    // "token button x y" from a `custom` event; false for anyone else's.
    static bool parseEvent(const QString& data, const QString& token, int* button, double* x, double* y);
private:
    void arm(RecordingClicks::Reply reply);
    void press(int button, double x, double y);
    HyprlandIpc ipc_;
    RecordingClicks::Sink sink_;
    QString token_, window_;
    quint64 generation_ = 0;
    bool lua_ = true;
    QString luaError_;
};
}
