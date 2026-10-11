#include "StorageReceipt.h"
namespace putgd {
QJsonObject Destination::toJson() const {return {{"providerId",providerId},{"profileId",profileId},{"profileRevision",profileRevision},{"settings",settings},{"credentials",credentialReference}};}
Destination Destination::fromJson(const QJsonObject& o) {return {o["providerId"].toString(),o["profileId"].toString(),o["profileRevision"].toString(),o["settings"].toObject(),o["credentials"].toObject()};}
QJsonObject StorageReceipt::toJson() const {return {{"schemaVersion",schemaVersion},{"destination",destination.toJson()},{"objectReference",objectReference},{"concurrencyToken",concurrencyToken},{"publicationId",publicationId},{"directUrl",directUrl}};}
StorageReceipt StorageReceipt::fromJson(const QJsonObject& o) {return {o["schemaVersion"].toInt(),Destination::fromJson(o["destination"].toObject()),o["objectReference"].toObject(),o["concurrencyToken"].toString(),o["publicationId"].toString(),o["directUrl"].toString()};}
Error StorageReceipt::validate() const {if(schemaVersion!=1||destination.providerId.isEmpty()||destination.profileId.isEmpty()||destination.profileRevision.isEmpty()||objectReference.isEmpty()||publicationId.isEmpty()) return {"receipt_invalid","Provider returned an incomplete receipt",{}}; return {};}
}
