#pragma once
#include <QJsonObject>
#include <QString>

namespace putgd {
// One checkpoint of an S3 or Backblaze B2 destination test without the query and fragment of
// any receipt link it holds (#1680): the receipt, a cleanup target, and both again inside a
// finished result. An expiring S3 link's query carries the access key ID and a signature, a B2
// authorized link's a download token. Only receipts whose destination is S3 or B2 are touched;
// a Custom uploader's deletion URL may carry its own token and is left as the server gave it.
QJsonObject withoutSignedTestLinks(QJsonObject checkpoint);
// Rewrites the journals of earlier S3 and B2 tests in `directory` the same way. The resident
// calls it while holding the journal lock and before it admits a destination test. Best effort:
// only regular files named <uuid>.json holding the journal schema are read, and a file that is
// a symbolic link, too large, unreadable or not that schema is left as it is.
void scrubDestinationTestJournals(const QString& directory);
}
