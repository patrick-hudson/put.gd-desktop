#pragma once
#include "core/StorageProvider.h"
#include <QStringList>
namespace putgd::worker {
struct ExchangePolicy {
    QString prefix, name, outputName, unknownCode, startMessage, stalledMessage, missingMessage;
    QStringList checkpointMethods;
    bool mutation = true;
    bool reclassifyOnError = false;
};
QJsonObject exchange(const QString& program, const QString& method, QJsonObject params, qint64 perRequest,
                     const ExchangePolicy& policy, std::atomic_bool& stopping, Error& error, bool* uncertain,
                     std::shared_ptr<std::atomic_bool> cancellation, std::function<void(const UploadProgress&)> progress,
                     DurableCheckpoint checkpoint);
}
