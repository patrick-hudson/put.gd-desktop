#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

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
// Whether a finished destination test for `provider` (#1690) can have left nothing at the
// destination that only its journal records: every step that could create something was recorded
// before it was sent and ended with explicit evidence. The upload failed definitely or was
// removed; a multipart (S3) or large-file (B2) upload was aborted or never created; and no S3
// delete marker put.gd made can remain. A result without an outcome after an intent, such as a
// worker that exited, is never settled. `checkpoints` ends with the resident's finished record.
bool destinationTestSettled(const QString& provider, const QJsonArray& checkpoints);
// The checkpoints saved in the journal at `path`, or none when it can't be read as a journal.
QJsonArray destinationTestCheckpoints(const QString& path);
// Removes a settled test's journal: a regular file only, never a symbolic link.
void removeDestinationTestJournal(const QString& path);
}
