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
// Why a helper program did not start, for the advice its message gives. Installation: the
// program is not a regular executable file, so reinstalling put.gd is the fix. Temporary:
// the system ran short of something that frees up again (file descriptors, memory,
// processes), so trying again helps. Unclear: neither is known (a missing loader or
// library, a refused permission, or a reason Qt reported wrongly), so the advice names both.
enum class StartFailure { Installation, Temporary, Unclear };
// `reason` is QProcess::errorString() after a failed start: Qt usually writes the system's
// strerror() text into it, in this process's own locale. Only facts that cannot change
// meanwhile are used: the program file, and a shortage the reason names.
StartFailure classifyStartFailure(const QString& program, const QString& reason);
// The message for a helper that did not start: `installationMessage` for a broken
// installation, otherwise whether trying again helps, about `subject` (what could not
// start, as the user knows it). It never repeats Qt's reason, which belongs in the error details.
QString startFailureMessage(StartFailure, const QString& installationMessage,
                            const QString& subject = QStringLiteral("put.gd couldn't start its upload helper"));
QJsonObject exchange(const QString& program, const QString& method, QJsonObject params, qint64 perRequest,
                     const ExchangePolicy& policy, std::atomic_bool& stopping, Error& error, bool* uncertain,
                     std::shared_ptr<std::atomic_bool> cancellation, std::function<void(const UploadProgress&)> progress,
                     DurableCheckpoint checkpoint, std::shared_ptr<std::atomic_bool> kill = {});
}
