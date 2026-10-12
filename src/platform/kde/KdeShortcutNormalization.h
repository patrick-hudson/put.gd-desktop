#pragma once
#include <QKeySequence>
#include <QString>
#include <xkbcommon/xkbcommon.h>

namespace putgd {
struct NormalizedKdeShortcut {
 QKeySequence sequence;
 QString error;
};
// Normalize printable ASCII punctuation/digits to KWin's current-layout wire
// chord. Letters and special keys retain their established literal wire value;
// this does not implement or certify Qt's private non-Latin translation.
// The borrowed map is immutable; no application, display, or retained state is used.
// Empty sequences disable an action. Unsupported/ambiguous mappings fail closed.
NormalizedKdeShortcut normalizeKdeShortcut(const QKeySequence&,xkb_keymap*,xkb_layout_index_t);
}
