#include "StorageProvider.h"
namespace putgd {
bool shareResultMatchesReceipt(const ShareResult& result,const StorageReceipt& original) {
 return !result.receipt.validate()&&result.receipt.destination.toJson()==original.destination.toJson()
  &&result.receipt.objectReference==original.objectReference&&result.receipt.concurrencyToken==original.concurrencyToken
  &&result.receipt.publicationId==original.publicationId&&QStringList{"shared","revoked","private","changed","unknown","failed"}.contains(result.status);
}

QJsonObject ProviderCapabilities::toJson() const {return {{"conditionalCreate",conditionalCreate},{"conditionalReplace",conditionalReplace},{"conditionalDelete",conditionalDelete},{"versionAddressing",versionAddressing},{"stat",stat},{"delete",remove},{"urls",urls},{"maximumNameBytes",maximumNameBytes},{"deletionEffect",deletionEffect},{"sharing",sharing},{"privateUploads",privateUploads}};}
void ProviderRegistry::add(const QString& id,std::shared_ptr<StorageProvider> p) {providers_.insert(id,std::move(p));}
std::shared_ptr<StorageProvider> ProviderRegistry::find(const QString& id) const {return providers_.value(id);}
Error ProviderRegistry::validate(const QString& id,const QJsonObject& s) const {auto p=find(id); return p?p->validateSettings(s):Error{"provider_unavailable","No validator registered for this provider",{}};}
Error ProviderRegistry::validateCredentials(const QString& id,const QJsonObject& reference) const {auto p=find(id);return p?p->validateCredentials(reference):Error{"provider_unavailable","No validator registered for this provider",{}};}
QJsonObject ProviderRegistry::capabilities() const {QJsonObject o; for(auto it=providers_.cbegin();it!=providers_.cend();++it) o[it.key()]=it.value()->capabilities({}).toJson();return o;}
QJsonObject ProviderRegistry::diagnostics() const {QJsonObject o; for(auto it=providers_.cbegin();it!=providers_.cend();++it) o[it.key()]=it.value()->diagnostics();return o;}
}
