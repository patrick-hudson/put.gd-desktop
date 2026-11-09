#pragma once
#include "core/RecordingClicks.h"
#include <QDBusConnection>
#include <QObject>

namespace putgd {
// Click highlights on Plasma (#1364). KWin gives pointer buttons to effects
// only (`effects.mouseChanged`): a script has no button signal, and a scripted
// effect, which has the signal, has no D-Bus call to pass it on. So the ring is
// KWin's own: its Mouse Click Animation effect is loaded over D-Bus and
// switched on through its global shortcut for the length of the recording,
// then switched off and unloaded again. KWin draws the rings on the screen and
// they are part of every monitor and region stream; a window stream carries
// the window only, so window recordings have none.
//
// The effect starts switched off whenever it is loaded, and KWin has no call
// that reads whether it is on. An effect the user had loaded is therefore
// reloaded first (which switches it off) and left loaded and off afterwards.
class KWinClickEffect final : public QObject, public RecordingClicks {
    Q_OBJECT
public:
    explicit KWinClickEffect(QObject* parent = nullptr);
    ~KWinClickEffect() override;
    QJsonObject clickCapability() const override;
    void watchClicks(const QJsonObject& target, RecordingClicks::Sink, RecordingClicks::Reply) override;
    void unwatchClicks() override;
    static constexpr const char* effect = "mouseclick";
    static constexpr const char* shortcut = "ToggleMouseClick";
private:
    // One Effects call; `reply` gets the boolean answer, or false on an error.
    void effects(const QString& method, std::function<void(bool ok, bool value)> reply);
    void toggle(std::function<void(bool ok)> reply);
    QDBusConnection bus_;
    // What this recording changed: the effect is on, and whether it was loaded before.
    bool on_ = false, wasLoaded_ = false;
    quint64 generation_ = 0;
};
}
