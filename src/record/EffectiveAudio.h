#pragma once
#include "core/RecordingModel.h"

namespace putgd::record {
// Capture availability never changes when the export format changes.
inline bool effectiveAudio(const RecordingConfig& config, bool captured, bool wanted) {
    return captured && wanted && !config.silent();
}
}
