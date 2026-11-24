#pragma once
#include <QJsonObject>
#include <QString>
#include <mutex>

namespace putgd {
// One checkpoint of a storage destination test (S3, B2, or any other provider whose test goes
// through the resident's storage test path) without the query and fragment of any receipt link
// it holds (#1680): the receipt, a cleanup target, and both again inside a finished result. An
// expiring S3 link's query carries the access key ID and a signature, a B2 authorized link's a
// download token. It applies whatever the receipts say about themselves. The Custom uploader's
// test writes its journal on its own path and never comes here.
QJsonObject withoutSignedTestLinks(QJsonObject checkpoint);
// Rewrites the journals of earlier S3 and B2 tests in `directory` the same way. The resident
// calls it while holding the journal lock and before it admits an S3 or B2 destination test.
// Best effort: only regular files named <uuid>.json holding the journal schema are read, and a
// file that is a symbolic link, too large, unreadable or not that schema is left as it is. A
// journal is rewritten only when its own records show it is an S3 or B2 test: the destination
// its S3 upload intent and multipart probe records save, or non-empty B2 file and bucket IDs in
// every receipt. Anything shaped like a Custom uploader's records (an upload_intent record, a
// link in the result, a url, deletion URL or nonce in a receipt), or records that disagree,
// leave it as it is.
void scrubDestinationTestJournals(const QString& directory);
// What a destination test's checkpoints show once they are on disk (#1690), for a test whose
// worker ended without a result. Each step reports only what its own terminal checkpoint shows:
// an upload confirmed by its receipt, a cleanup that reached "done" or recorded a retained delete
// marker, a large file whose start was recorded. Everything else it started is unknown.
class DestinationTestTrail {
public:
    // Called with each checkpoint after it was written.
    void record(const QJsonObject& checkpoint);
    // `result` when the worker sent one, otherwise what the recorded checkpoints show.
    QJsonObject completed(QJsonObject result) const;
private:
    mutable std::mutex mutex_;
    bool intent_ = false, multipartOpen_ = false, multipartFinished_ = false, largeOpen_ = false, largeFinished_ = false;
    QJsonObject receipt_, cleanup_;
    QString largeFileId_;
    QJsonValue abort_, cancel_;
};
}
