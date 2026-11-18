#pragma once
#include "RecordingModel.h"
#include <QList>
#include <optional>

namespace putgd::recording {
// #1613: a half-open stretch of time [start, end).
struct Span {
    qint64 start = 0, end = 0;
    qint64 length() const { return end - start; }
    bool empty() const { return end <= start; }
    bool contains(qint64 t) const { return start <= t && t < end; }
    bool operator==(const Span&) const = default;
};

// #1613: the footage an edit keeps, and the one mapping between source time
// (the recorded clip's timeline, what the ruler shows) and output time (the
// shared clip's timeline, what the transport clock shows). The kept spans are
// [trimStart, trimEnd) minus every cut, sorted and disjoint; output time lays
// them end to end from zero. Playback, preview, export, the GIF/WebP limits,
// export progress and the transport clock all read this one map, so they can
// never disagree about what is in the clip.
//
// Pure arithmetic on microseconds: no frame grid. Callers that need whole
// frames (the exporter, "first/last kept frame") apply 1/fps themselves.
class KeptSpans {
public:
    KeptSpans() = default;
    KeptSpans(const RecordingEdit&, qint64 durationUs);
    const QList<Span>& spans() const { return spans_; }
    bool empty() const { return spans_.isEmpty(); }
    // Total kept time; RecordingEdit::exportDuration returns this.
    qint64 outputDuration() const { return outputDuration_; }
    bool kept(qint64 sourceUs) const;
    // A kept source time's output time. A source time that is not kept maps to
    // the output time of the next kept footage after it (the seam it falls
    // into), or outputDuration() when nothing is kept after it.
    qint64 toOutput(qint64 sourceUs) const;
    // The source time of an output time; out-of-range values clamp to
    // [0, outputDuration()]. outputDuration() maps to the last span's end. An
    // output time on a seam maps to the start of the span after it.
    qint64 toSource(qint64 outputUs) const;
    // The first kept source time at or after t; nullopt when none.
    std::optional<qint64> nextKept(qint64 sourceUs) const;
    // The end (exclusive) of the last kept footage at or before t, i.e. the
    // largest e <= t such that [e - x, e) is kept for some x > 0; nullopt when
    // none. "The last kept frame before t" is that minus one frame.
    std::optional<qint64> keptEndAtOrBefore(qint64 sourceUs) const;
    // The kept parts of a source interval, in source time (empty when none).
    QList<Span> keptParts(qint64 start, qint64 end) const;
    // The same parts mapped to output time, with abutting parts joined (a
    // cover over a cut becomes one output interval).
    QList<Span> outputParts(qint64 start, qint64 end) const;
    // Output times where two kept spans meet (one per removed cut between
    // them), ascending. Not the clip's ends.
    QList<qint64> seams() const;
private:
    QList<Span> spans_;
    qint64 outputDuration_ = 0;
};
}
