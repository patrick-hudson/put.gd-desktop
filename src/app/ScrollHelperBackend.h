#pragma once
#include "ScrollBackend.h"
#include "HelperSupervisor.h"
#include <QObject>

namespace putgd {
// The production scroll-capture backend (#1376): one transient `putgd-record`
// process per capture, started as a scroll session, over private UiControl
// pipes. It is the same supervision as RecordingHelperBackend's
// (HelperSupervisor) with its own policy: build-identity handshake,
// request/result commands, unsolicited events, bounded deadlines and a required
// normal exit. A command that misses its deadline ends the helper, which may
// still be turning the wheel. What the desktop offers comes from a separate
// `putgd-record --scroll-capabilities` run, cached until the resident exits (a
// failed probe for a minute). Pixels never cross these pipes: the helper writes
// one PNG into the staging directory and reports its path, size and hash.
class ScrollHelperBackend final : public QObject, public ScrollBackend {
    Q_OBJECT
public:
    explicit ScrollHelperBackend(QString helperPath = {}, QObject* parent = nullptr);
    // Tests shorten these; production keeps the defaults (scroll::helper*Ms).
    void setTimeouts(int startupMs, int commandMs);
    // {available, reason?, message?, injector, frames, selection, maxHeight}.
    // Besides the four reasons of ScrollBackend.h, before the first probe has
    // answered: scroll_not_probed, scroll_probe_pending; and scroll_probe_failed.
    QJsonObject capabilities() const override { return helper_.capabilities(); }
    void refreshCapabilities(std::function<void()> done) override { helper_.refreshCapabilities(std::move(done)); }
    void setEventSink(Event sink) override { helper_.setEventSink(std::move(sink)); }
    void prepare(const QString& id, const QJsonObject& params, const QString& stagingDirectory, Reply) override;
    void control(const QString& id, const QString& command, const QJsonObject& params, Reply) override;
    void discard(const QString& id) override { helper_.discard(id); }
    qint64 processId(const QString& id) const override { return helper_.processId(id); }
    QJsonObject diagnostics() const { return helper_.diagnostics(); }
    // The resolved interface scheme, "dark" or "light": the helper's bar uses its tokens.
    void setAppearance(const QString& scheme) { helper_.setAppearance(scheme); }
private:
    HelperSupervisor helper_;
    int commandMs_ = scroll::helperCommandMs;
};
}
