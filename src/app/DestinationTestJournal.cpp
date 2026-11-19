#include "DestinationTestJournal.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUrl>
#include <QUrlQuery>

namespace putgd {
namespace {
// Nothing of the test can be left at the destination: its file was removed or never written, and a
// multipart (S3) or large-file (B2) upload it started was aborted or never created.
bool nothingLeft(const QJsonObject& result) {
    const auto upload = result.value("uploadStatus").toString();
    if (upload == "uploaded") {
        if (!QStringList{"removed", "missing"}.contains(result.value("cleanupStatus").toString())) return false;
    } else if (!upload.isEmpty() && upload != "failed" && upload != "collision") return false;
    if (result.contains("multipart")) {
        const auto probe = result.value("multipart").toObject();
        if (probe.contains("abort") ? probe.value("abort") != "aborted" : probe.value("status") == "unknown") return false;
    }
    if (result.contains("largeFile")) {
        const auto large = result.value("largeFile").toObject();
        if (large.value("start") == "ok" && large.value("cancel") != "cancelled") return false;
    }
    return true;
}
bool signedQuery(const QUrl& url) {
    const QUrlQuery query(url);
    return query.hasQueryItem("X-Amz-Signature") || query.hasQueryItem("X-Amz-Credential") || query.hasQueryItem("Authorization");
}
QJsonValue withoutSignedLinks(const QJsonValue& value, bool& changed) {
    if (value.isString()) {
        QUrl url(value.toString());
        if (!url.isValid() || !url.hasQuery() || !signedQuery(url)) return value;
        url.setQuery(QString());
        changed = true;
        return url.toString(QUrl::FullyEncoded);
    }
    if (value.isArray()) {
        auto array = value.toArray();
        for (qsizetype i = 0; i < array.size(); ++i) array[i] = withoutSignedLinks(array[i], changed);
        return array;
    }
    if (value.isObject()) {
        auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) it.value() = withoutSignedLinks(it.value(), changed);
        return object;
    }
    return value;
}
}
void settleDestinationTestJournal(const QString& path) {
    QFile file(path);
    // The journal writer bounds its files to 1 MiB; anything else is left as it is.
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) return;
    const auto document = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    const auto checkpoints = document.value("checkpoints").toArray();
    if (checkpoints.isEmpty()) return;
    const auto last = checkpoints.last().toObject();
    if (last.value("phase") == "finished" && nothingLeft(last.value("result").toObject())) {
        QFile::remove(path);
        return;
    }
    bool changed = false;
    const auto cleaned = withoutSignedLinks(document, changed).toObject();
    if (!changed) return;
    QSaveFile out(path);
    out.setDirectWriteFallback(false);
    if (!out.open(QIODevice::WriteOnly) || !out.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) return;
    const auto bytes = QJsonDocument(cleaned).toJson(QJsonDocument::Compact);
    if (out.write(bytes) == bytes.size()) out.commit();
}
QStringList destinationTestJournals(const QString& directory) {
    QStringList paths;
    for (const auto& name : QDir(directory).entryList({"*.json"}, QDir::Files)) paths.append(QDir(directory).filePath(name));
    return paths;
}
}
