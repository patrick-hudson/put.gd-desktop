#pragma once
#include "S3Driver.h"
namespace putgd {
// Internal constructor seam for deterministic supervision tests. Production
// callers use zero: no settings/environment/command-line override exists.
struct S3ProcessLimits { qint64 cancellationGraceMs = 0; qint64 childSilenceMs = 0; qint64 checkpointDeadlineMs = 0; };
// `stop` ends the startup handshake at once (the helper is killed); the error is then
// "cancelled" (Quit or Cancel during a cold start, #1602).
S3Driver* createS3ProcessDriver(const QString& modulePath, CredentialLookup, ReadinessCheck, Error&, S3ProcessLimits = {}, std::shared_ptr<std::atomic_bool> stop = {});
}
