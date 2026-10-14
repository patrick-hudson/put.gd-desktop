#pragma once
#include "ArtifactStore.h"
#include "LocalArchive.h"
#include "StorageProvider.h"
#include <QJsonArray>
namespace putgd {
struct OperationItem {
 Artifact artifact; QString uploadStatus="stored",candidate,condition,concurrencyToken,publicationId;
 QJsonObject objectReference;
 StorageReceipt receipt; LocalCopy local;
 QString remoteStatus="not_requested";
 QString shareStatus="not_requested";
 QString shareIntent; // pending explicit create; inspection must not erase it
 QJsonObject shareIntentOptions; // non-secret options of that original intent
 Error error;
 QString phase;std::optional<UploadProgress> uploadProgress;
 QJsonObject toJson() const;
 static OperationItem fromJson(const QJsonObject&);
 UploadRequest uploadRequest(const Destination&,std::optional<int> requestTimeoutMs={}) const;
 bool linkAvailable() const;
};
struct Operation {
 QString id,action,state="accepted",settingsRevision,created;
 Settings settings; Destination destination;
 QList<OperationItem> items;
 quint64 invocation=0,clipboardRevision=0;
 QString copyStatus="pending";
 bool cancelled=false;
 QString phase="accepted";
 std::optional<int> requestTimeoutMs; // Explicit retry budget, separate from immutable settings.
 Error error;
 QJsonObject toJson() const;
 QJsonObject publicJson() const;
 static Operation fromJson(const QJsonObject&);
};
}
