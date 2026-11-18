#include "KeptSpans.h"
#include <algorithm>

namespace putgd::recording {
KeptSpans::KeptSpans(const RecordingEdit& edit, qint64 durationUs) {
    const qint64 trimStart = std::max<qint64>(0, edit.trimStart);
    const qint64 trimEnd = std::min(edit.exportEnd(durationUs), durationUs);
    if (trimEnd <= trimStart) return;
    // A cut list that is not in order (a hand-built edit) still yields sorted, disjoint spans.
    QList<RecordingCut> cuts = edit.cuts;
    std::sort(cuts.begin(), cuts.end(), [](const RecordingCut& a, const RecordingCut& b) { return a.start < b.start; });
    qint64 cursor = trimStart;
    for (const auto& cut : cuts) {
        if (cut.end <= cut.start) continue;
        if (cut.start > cursor) spans_.append({cursor, std::min(cut.start, trimEnd)});
        cursor = std::max(cursor, cut.end);
        if (cursor >= trimEnd) break;
    }
    if (cursor < trimEnd) spans_.append({cursor, trimEnd});
    for (const auto& span : std::as_const(spans_)) outputDuration_ += span.length();
}
bool KeptSpans::kept(qint64 sourceUs) const {
    for (const auto& span : spans_) if (span.contains(sourceUs)) return true;
    return false;
}
qint64 KeptSpans::toOutput(qint64 sourceUs) const {
    qint64 offset = 0;
    for (const auto& span : spans_) {
        if (sourceUs < span.end) return offset + std::max<qint64>(0, sourceUs - span.start);
        offset += span.length();
    }
    return outputDuration_;
}
qint64 KeptSpans::toSource(qint64 outputUs) const {
    if (spans_.isEmpty()) return 0;
    outputUs = std::clamp<qint64>(outputUs, 0, outputDuration_);
    if (outputUs == outputDuration_) return spans_.last().end;
    qint64 offset = 0;
    for (const auto& span : spans_) {
        if (outputUs < offset + span.length()) return span.start + (outputUs - offset);
        offset += span.length();
    }
    return spans_.last().end;
}
std::optional<qint64> KeptSpans::nextKept(qint64 sourceUs) const {
    for (const auto& span : spans_) if (sourceUs < span.end) return std::max(sourceUs, span.start);
    return std::nullopt;
}
std::optional<qint64> KeptSpans::keptEndAtOrBefore(qint64 sourceUs) const {
    for (auto it = spans_.crbegin(); it != spans_.crend(); ++it) if (it->start < sourceUs) return std::min(sourceUs, it->end);
    return std::nullopt;
}
QList<Span> KeptSpans::keptParts(qint64 start, qint64 end) const {
    QList<Span> parts;
    for (const auto& span : spans_) {
        const Span part{std::max(start, span.start), std::min(end, span.end)};
        if (!part.empty()) parts.append(part);
    }
    return parts;
}
QList<Span> KeptSpans::outputParts(qint64 start, qint64 end) const {
    QList<Span> parts;
    for (const auto& part : keptParts(start, end)) {
        const qint64 from = toOutput(part.start);
        const Span mapped{from, from + part.length()};
        if (!parts.isEmpty() && parts.last().end == mapped.start) parts.last().end = mapped.end;
        else parts.append(mapped);
    }
    return parts;
}
QList<qint64> KeptSpans::seams() const {
    QList<qint64> result;
    qint64 offset = 0;
    for (int i = 0; i + 1 < spans_.size(); ++i) {
        offset += spans_[i].length();
        result.append(offset);
    }
    return result;
}
}
