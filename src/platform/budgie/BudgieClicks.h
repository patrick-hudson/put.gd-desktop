#pragma once
#include "core/RecordingClicks.h"
#include <QJsonArray>

namespace putgd {
// Click highlights on Budgie 10.10 (labwc) are not offered (#1364), and this
// says why wherever put.gd reports its capabilities. labwc has no IPC, script
// or event stream. Its one hook is a `<mousebind>` in rc.xml that runs a
// command: that file belongs to Budgie's labwc bridge, which keeps it in
// memory and writes it back without foreign entries at the next setting it
// mirrors, and put.gd's Budgie adapter writes only through Budgie's own
// settings. A command run by a mousebind also carries no pointer position,
// and the one place labwc reports it, an ext-image-copy-capture cursor
// session, goes silent while a capture asks for a painted cursor, as every
// put.gd recording there does (wlroots locks software cursors for it).
class BudgieClicks final : public RecordingClicks {
public:
    QJsonObject clickCapability() const override {
        return {{"available", false}, {"route", "none"}, {"drawnBy", "recorder"}, {"targets", QJsonArray{}}, {"reason", "labwc_no_click_events"},
                {"message", "Budgie's compositor (labwc) gives put.gd no pointer clicks: it has no event interface, its one mouse hook lives in a file Budgie's own bridge rewrites, and it reports no pointer position while a recording paints the cursor."}};
    }
    void watchClicks(const QJsonObject&, RecordingClicks::Sink, RecordingClicks::Reply reply) override {
        const auto capability = clickCapability();
        reply({capability["reason"].toString(), capability["message"].toString(), {}}, {});
    }
    void unwatchClicks() override {}
};
}
