#pragma once
#include "core/RecordingModel.h"
#include <QHash>

namespace putgd::record {
// Retain ordinal identities for the review's lifetime, including removed masks.
class MaskNames {
public:
    void observe(const QList<RecordingMask>& masks) {
        for (const auto& mask : masks) {
            if (!numbers_.contains(mask.id)) numbers_.insert(mask.id, next_++);
            const auto kind = mask.kind == "solid" ? QStringLiteral("Redaction") : QStringLiteral("Blur");
            names_.insert(mask.id, QStringLiteral("%1 %2").arg(kind).arg(numbers_.value(mask.id)));
        }
    }
    QString name(const RecordingMask& mask) const {
        return names_.value(mask.id);
    }
private:
    QHash<QString, int> numbers_;
    QHash<QString, QString> names_;
    int next_ = 1;
};
}
