#pragma once
#include <QString>
#include <QStringList>

namespace putgd {
// A destination test's journal (destination-tests/<uuid>.json) only traces what the test may have
// left at the destination (#1680). Once the test has finished and nothing of it can remain there,
// the journal is removed. Otherwise it is kept, with every signed link query (an S3 signature and
// access key ID, a B2 download token) removed: the key and identity it holds are what matter.
void settleDestinationTestJournal(const QString& path);
// The journals in that folder now, for a later settleDestinationTestJournal each. Listed at startup,
// before any test can start, so a running test's journal is never among them.
QStringList destinationTestJournals(const QString& directory);
}
