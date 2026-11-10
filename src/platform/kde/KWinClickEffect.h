#pragma once
#include "core/RecordingClicks.h"
#include <QDBusConnection>
#include <QList>
#include <QObject>
#include <QVariantList>

namespace putgd {
// Click highlights on Plasma (#1364). KWin gives pointer buttons to effects
// only (`effects.mouseChanged`): a script has no button signal, and a scripted
// effect, which has the signal, has no D-Bus call to pass it on. So the ring is
// KWin's own: its Mouse Click Animation effect is switched on for the length of
// the recording. KWin draws the rings on the screen and they are part of every
// monitor and region stream; a window stream carries the window only, so
// window recordings have none.
//
// The user's own state comes back exactly. KWin says whether the effect is
// loaded (`Effects.isEffectLoaded`) and whether it is switched on (`enabled`
// in `Effects.supportInformation`); it is switched through its global shortcut
// (`ToggleMouseClick`), the one way there is. A recording loads the effect only
// when it was not loaded and toggles it only when it was off, checks that the
// toggle took, and afterwards undoes exactly what it did: an effect that was
// loaded and on is left alone throughout, one that was loaded and off is off
// again, one that was not loaded is unloaded. Where KWin does not report
// `enabled`, the effect is left as it is and the recording has no highlights.
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
    // `enabled` from Effects.supportInformation's text: 1 on, 0 off, -1 not reported.
    static int enabledIn(const QString& supportInformation);
private:
    // What KWin says about the effect now.
    struct State { bool answered = false, loaded = false, known = false, enabled = false; };
    using Done = std::function<void()>;
    using Job = std::function<void(Done)>;
    // Watches and unwatches run one after another, each to its end: Record
    // followed at once by Stop cannot leave the effect half switched.
    void enqueue(Job);
    void next();
    void call(const QString& service, const QString& path, const QString& interface, const QString& method, const QVariantList& arguments, std::function<void(bool ok, QVariantList)> reply);
    void effects(const QString& method, std::function<void(bool ok, QVariantList)> reply);
    void state(std::function<void(State)> reply);
    void toggle(std::function<void(State)> reply);
    void restore(Done done);
    QDBusConnection bus_;
    // What this recording changed, to be undone: it loaded the effect, it switched it on.
    bool loadedByUs_ = false, toggledByUs_ = false, on_ = false, busy_ = false;
    QList<Job> jobs_;
};
}
