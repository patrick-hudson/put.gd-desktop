#include "DestinationTestJournal.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrl>

namespace putgd {
namespace {
QJsonObject withoutLinkQuery(QJsonObject receipt) {
    const auto provider = receipt.value("destination").toObject().value("providerId").toString();
    if ((provider != "s3" && provider != "backblaze-b2") || !receipt.value("directUrl").isString()) return receipt;
    QUrl link(receipt.value("directUrl").toString());
    if (!link.hasQuery() && !link.hasFragment()) return receipt;
    link.setQuery(QString());
    link.setFragment(QString());
    receipt["directUrl"] = link.toString(QUrl::FullyEncoded);
    return receipt;
}
void scrubField(QJsonObject& object, const char* name) {
    if (object.value(name).isObject()) object[name] = withoutLinkQuery(object.value(name).toObject());
}
}
QJsonObject withoutSignedTestLinks(QJsonObject checkpoint) {
    scrubField(checkpoint, "receipt");
    scrubField(checkpoint, "target");
    if (checkpoint.value("result").isObject()) {
        auto result = checkpoint.value("result").toObject();
        scrubField(result, "receipt");
        if (result.value("cleanupProgress").isObject()) {
            auto progress = result.value("cleanupProgress").toObject();
            scrubField(progress, "target");
            result["cleanupProgress"] = progress;
        }
        checkpoint["result"] = result;
    }
    return checkpoint;
}
void scrubDestinationTestJournals(const QString& directory) {
    const QFileInfo folder(directory);
    if (!folder.exists() || folder.isSymLink() || !folder.isDir()) return;
    // The name the journal writer gives each test: QUuid::WithoutBraces.
    static const QRegularExpression journalName(QStringLiteral("\\A[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\.json\\z"));
    const auto entries = QDir(directory).entryInfoList(QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const auto& entry : entries) {
        // QSaveFile writes through a symbolic link, so one is never opened.
        if (entry.isSymLink() || !entry.isFile() || !journalName.match(entry.fileName()).hasMatch() || entry.size() > 1024 * 1024) continue;
        QFile file(entry.filePath());
        if (!file.open(QIODevice::ReadOnly)) continue;
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(file.read(1024 * 1024 + 1), &parse);
        file.close();
        if (parse.error != QJsonParseError::NoError || !document.isObject()) continue;
        const auto journal = document.object();
        if (journal.size() != 2 || journal.value("schemaVersion") != QJsonValue(1) || !journal.value("checkpoints").isArray()) continue;
        auto checkpoints = journal.value("checkpoints").toArray();
        bool valid = true, changed = false;
        for (qsizetype i = 0; i < checkpoints.size() && valid; ++i) {
            if (!checkpoints[i].isObject()) { valid = false; break; }
            const auto before = checkpoints[i].toObject();
            const auto after = withoutSignedTestLinks(before);
            if (after != before) { checkpoints[i] = after; changed = true; }
        }
        if (!valid || !changed || QFileInfo(entry.filePath()).isSymLink()) continue;
        QSaveFile out(entry.filePath());
        out.setDirectWriteFallback(false);
        if (!out.open(QIODevice::WriteOnly) || !out.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) continue;
        const auto bytes = QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"checkpoints", checkpoints}}).toJson(QJsonDocument::Compact);
        if (out.write(bytes) == bytes.size()) out.commit();
    }
}
}
