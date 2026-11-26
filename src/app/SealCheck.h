#pragma once
#include "core/StorageProvider.h"
#include <QJsonObject>
namespace putgd {
// Sealed links (#2079): Test upload and link's extra check. It uploads a small sealed test
// page (seal::probePage) through the destination's own upload, fetches its link as a visitor
// with `putgd-cli --seal-probe`, and removes it again. It passes only when the link answers
// 200 with Content-Type text/html, no Content-Disposition attachment and exactly the page's
// bytes: a browser then opens it as a page. Synchronous: call it from a worker thread.
// Returns {state: passed|failed|cancelled, reason?, cleanup: removed|left|failed|unknown}.
// The destination the check talks to, as every other request names it: the saved profile at the
// settings revision the Test was accepted at (workers refuse a request without one).
Destination sealCheckDestination(const QString& profileId,const QJsonObject& profile,const QString& revision);
QJsonObject runSealCheck(StorageProvider& provider,const Destination& destination,const QString& stagingDirectory,
                         const QString& cliPath,std::shared_ptr<std::atomic_bool> cancelled);
}
