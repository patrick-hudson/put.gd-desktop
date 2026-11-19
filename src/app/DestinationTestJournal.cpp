#include "DestinationTestJournal.h"
#include <QDir>
#include <algorithm>
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
    if (!receipt.value("directUrl").isString()) return receipt;
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
// The receipts a destination test's checkpoint can hold.
QList<QJsonObject> receipts(const QJsonObject& checkpoint) {
    QList<QJsonObject> found;
    for (const auto* name : {"receipt", "target"})
        if (checkpoint.value(name).isObject()) found.append(checkpoint.value(name).toObject());
    const auto result = checkpoint.value("result").toObject();
    if (result.value("receipt").isObject()) found.append(result.value("receipt").toObject());
    const auto target = result.value("cleanupProgress").toObject().value("target");
    if (target.isObject()) found.append(target.toObject());
    return found;
}
// Which test an earlier journal belongs to, from its own records; empty when they don't say or
// disagree. An S3 test saves its destination on its upload intent and multipart probe records.
// A B2 test from an earlier build saved none, but each B2 receipt names its file and bucket IDs.
// Anything shaped like a Custom uploader's records vetoes the rewrite: its deletion URL may
// carry a token the server needs, whatever the journal is labelled.
QString journalProvider(const QJsonArray& checkpoints) {
    static const QStringList savesDestination{"synthetic_upload_intent", "synthetic_multipart_intent", "synthetic_multipart_created", "synthetic_multipart_finished"};
    QString saved;
    bool b2Receipts = true, anyReceipt = false;
    for (const auto& value : checkpoints) {
        const auto checkpoint = value.toObject();
        const auto phase = checkpoint.value("phase").toString();
        if (phase == "upload_intent" || checkpoint.value("result").toObject().contains("link")) return {};
        if (savesDestination.contains(phase)) {
            const auto provider = checkpoint.value("destination").toObject().value("providerId").toString();
            if (provider.isEmpty() || (!saved.isEmpty() && provider != saved)) return {};
            saved = provider;
        }
        for (const auto& receipt : receipts(checkpoint)) {
            anyReceipt = true;
            const auto reference = receipt.value("objectReference").toObject();
            const auto label = receipt.value("destination").toObject().value("providerId").toString();
            for (const auto* field : {"url", "deletionUrl", "deletionUrlLeftOut", "nonce"})
                if (reference.contains(field)) return {};
            if (label == "custom-http") return {};
            b2Receipts = b2Receipts && !reference.value("fileId").toString().isEmpty() && !reference.value("bucketId").toString().isEmpty();
        }
    }
    if (!saved.isEmpty()) return (saved == "s3" && !(anyReceipt && b2Receipts)) || saved == "backblaze-b2" ? saved : QString();
    return anyReceipt && b2Receipts ? QStringLiteral("backblaze-b2") : QString();
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
        const bool valid = std::all_of(checkpoints.begin(), checkpoints.end(), [](const QJsonValue& value) { return value.isObject(); });
        if (!valid || journalProvider(checkpoints).isEmpty()) continue;
        bool changed = false;
        for (qsizetype i = 0; i < checkpoints.size(); ++i) {
            const auto before = checkpoints[i].toObject();
            const auto after = withoutSignedTestLinks(before);
            if (after != before) { checkpoints[i] = after; changed = true; }
        }
        if (!changed || QFileInfo(entry.filePath()).isSymLink()) continue;
        QSaveFile out(entry.filePath());
        out.setDirectWriteFallback(false);
        if (!out.open(QIODevice::WriteOnly) || !out.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) continue;
        const auto bytes = QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"checkpoints", checkpoints}}).toJson(QJsonDocument::Compact);
        if (out.write(bytes) == bytes.size()) out.commit();
    }
}
void DestinationTestTrail::record(const QJsonObject& checkpoint) {
    std::lock_guard lock(mutex_);
    const auto phase = checkpoint.value("phase").toString();
    if (phase == "synthetic_upload_intent") intent_ = true;
    else if (phase == "synthetic_uploaded") receipt_ = checkpoint.value("receipt").toObject();
    else if (phase == "synthetic_multipart_intent") multipartOpen_ = true;
    else if (phase == "synthetic_multipart_finished") { multipartOpen_ = false; multipartFinished_ = true; abort_ = checkpoint.value("abort"); }
    else if (phase == "synthetic_large_intent") largeOpen_ = true;
    else if (phase == "synthetic_large_finished") { largeOpen_ = false; largeFinished_ = true; cancel_ = checkpoint.value("cancel"); }
    // A cleanup checkpoint carries its target.
    if (checkpoint.value("target").isObject()) cleanup_ = checkpoint;
}
QJsonObject DestinationTestTrail::completed(QJsonObject result) const {
    std::lock_guard lock(mutex_);
    if (result.contains("uploadStatus") || !intent_) return result;
    if (receipt_.isEmpty()) result["uploadStatus"] = "unknown";
    else {
        result["uploadStatus"] = "uploaded";
        result["receipt"] = receipt_;
        result["cleanupStatus"] = cleanup_.value("phase") == "done" ? "removed" : "unknown";
        if (!cleanup_.isEmpty()) result["cleanupProgress"] = cleanup_;
    }
    if (multipartOpen_) result["multipart"] = QJsonObject{{"status", "unknown"}};
    else if (multipartFinished_) result["multipart"] = QJsonObject{{"status", abort_ == "aborted" ? "ok" : "unknown"}, {"abort", abort_}};
    if (largeOpen_) result["largeFile"] = QJsonObject{{"start", "unknown"}};
    else if (largeFinished_) result["largeFile"] = QJsonObject{{"start", "ok"}, {"cancel", cancel_}};
    return result;
}
}
