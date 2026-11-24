#pragma once
#include "core/Error.h"
#include <QList>
#include <QObject>
#include <QPoint>
#include <functional>
namespace putgd {
// Capture monitor (#1363) on compositors that give clients no pointer position
// and have no query of their own (labwc, Mir): which output the pointer is on.
// One ext-image-copy-capture cursor session per output reports where the
// cursor's hotspot lies in that output's buffer. No surface is mapped, no
// input is taken and pointer focus never moves, so a hover state under the
// pointer survives the probe. Each probe uses its own short Wayland
// connection and reads no pixels.
class CursorOutputProbe final:public QObject {
public:
 // The hotspot in an output's transformed buffer pixels. A cursor image that
 // straddles two outputs is reported for both; only one contains the hotspot.
 struct Position {QString output;QPoint buffer;};
 using Reply=std::function<void(QList<Position>,Error)>;
 explicit CursorOutputProbe(QObject* parent=nullptr);
 ~CursorOutputProbe() override;
 // Answers once, within about a second. A probe still running is ended with cancelled.
 void probe(Reply);
 void cancel();
 static constexpr int timeoutMs=1500;
private:
 struct Run;
 Run* run_=nullptr;
};
}
