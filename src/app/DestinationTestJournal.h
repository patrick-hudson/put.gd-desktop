#pragma once
#include <QJsonObject>
#include <QString>

namespace putgd {
// One checkpoint of a destination test for `provider` without the query and fragment of any
// receipt link it holds (#1680): the receipt, a cleanup target, and both again inside a finished
// result. An expiring S3 link's query carries the access key ID and a signature, a B2 authorized
// link's a download token. `provider` is the one the resident started the test for, never what a
// receipt says about itself: for "s3" and "backblaze-b2" every such link is stripped, and for any
// other provider the checkpoint is returned unchanged.
QJsonObject withoutSignedTestLinks(QJsonObject checkpoint, const QString& provider);
// Rewrites the journals of earlier S3 and B2 tests in `directory` the same way. The resident
// calls it while holding the journal lock and before it admits an S3 or B2 destination test.
// Best effort: only regular files named <uuid>.json holding the journal schema are read, and a
// file that is a symbolic link, too large, unreadable or not that schema is left as it is. A
// journal is rewritten only when its own records show it is an S3 or B2 test: the saved intent's
// destination, or B2's file and bucket IDs in every receipt. Any sign of a Custom uploader
// receipt, or records that disagree, leave it as it is.
void scrubDestinationTestJournals(const QString& directory);
}
