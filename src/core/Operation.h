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
 Error error;
 QJsonObject toJson() const;
 static OperationItem fromJson(const QJsonObject&);
 UploadRequest uploadRequest(const Destination&) const;
};
struct Operation {
 QString id,action,state="accepted",settingsRevision,created;
 Settings settings; Destination destination;
 QList<OperationItem> items;
 quint64 invocation=0,clipboardRevision=0;
 QString copyStatus="pending";
 bool cancelled=false;
 Error error;
 QJsonObject toJson() const;
 QJsonObject publicJson() const;
 static Operation fromJson(const QJsonObject&);
};
}
