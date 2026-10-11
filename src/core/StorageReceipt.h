#pragma once
#include "Error.h"
namespace putgd {
struct Destination {
 QString providerId,profileId,profileRevision;
 QJsonObject settings,credentialReference;
 QJsonObject toJson() const;
 static Destination fromJson(const QJsonObject&);
};
struct StorageReceipt {
 int schemaVersion=1;
 Destination destination;
 QJsonObject objectReference;
 QString concurrencyToken,publicationId,directUrl;
 QJsonObject toJson() const;
 static StorageReceipt fromJson(const QJsonObject&);
 Error validate() const;
};
}
