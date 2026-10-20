#include "StorageProvider.h"
#include <cmath>
namespace putgd {
namespace {
constexpr qint64 maxProgressBytes=9007199254740991LL; // Exact JSON/SDK integer range.
std::optional<qint64> progressNumber(const QJsonValue& value){
 const auto number=value.toDouble(-1);
 if(!value.isDouble()||!std::isfinite(number)||number<0||number>maxProgressBytes||std::floor(number)!=number)return {};
 return qint64(number);
}
}
bool UploadProgress::valid()const{
 return QStringList{"preparing","uploading","finalizing"}.contains(phase)
  &&(!bytesTransferred||(*bytesTransferred>=0&&*bytesTransferred<=maxProgressBytes))
  &&(!bytesTotal||(bytesTransferred&&*bytesTotal>=0&&*bytesTotal<=maxProgressBytes&&*bytesTransferred<=*bytesTotal));
}
QJsonObject UploadProgress::toJson()const{
 QJsonObject value{{"phase",phase}};if(bytesTransferred)value["bytesTransferred"]=*bytesTransferred;if(bytesTotal)value["bytesTotal"]=*bytesTotal;return value;
}
std::optional<UploadProgress> UploadProgress::fromJson(const QJsonObject& value){
 if(value.isEmpty())return {};
 for(const auto& key:value.keys())if(!QStringList{"phase","bytesTransferred","bytesTotal"}.contains(key))return {};
 UploadProgress progress;progress.phase=value["phase"].toString();
 if(value.contains("bytesTransferred")){progress.bytesTransferred=progressNumber(value["bytesTransferred"]);if(!progress.bytesTransferred)return {};}
 if(value.contains("bytesTotal")){progress.bytesTotal=progressNumber(value["bytesTotal"]);if(!progress.bytesTotal)return {};}
 return progress.valid()?std::optional<UploadProgress>(progress):std::nullopt;
}

bool shareResultMatchesReceipt(const ShareResult& result,const StorageReceipt& original) {
 return !result.receipt.validate()&&result.receipt.destination.toJson()==original.destination.toJson()
  &&result.receipt.objectReference==original.objectReference&&result.receipt.concurrencyToken==original.concurrencyToken
  &&result.receipt.publicationId==original.publicationId&&QStringList{"shared","revoked","private","changed","unknown","failed"}.contains(result.status);
}

QJsonObject ProviderCapabilities::toJson() const {return {{"conditionalCreate",conditionalCreate},{"conditionalReplace",conditionalReplace},{"conditionalDelete",conditionalDelete},{"versionAddressing",versionAddressing},{"stat",stat},{"delete",remove},{"urls",urls},{"maximumNameBytes",maximumNameBytes},{"maximumObjectBytes",maximumObjectBytes>0?QJsonValue(double(maximumObjectBytes)):QJsonValue(QJsonValue::Null)},{"deletionEffect",deletionEffect},{"sharing",sharing},{"privateUploads",privateUploads}};}
void ProviderRegistry::add(const QString& id,std::shared_ptr<StorageProvider> p) {providers_.insert(id,std::move(p));}
std::shared_ptr<StorageProvider> ProviderRegistry::find(const QString& id) const {return providers_.value(id);}
Error ProviderRegistry::validate(const QString& id,const QJsonObject& s) const {auto p=find(id); return p?p->validateSettings(s):Error{"provider_unavailable","No validator registered for this provider",{}};}
Error ProviderRegistry::validateCredentials(const QString& id,const QJsonObject& reference) const {auto p=find(id);return p?p->validateCredentials(reference):Error{"provider_unavailable","No validator registered for this provider",{}};}
QJsonObject ProviderRegistry::capabilities() const {QJsonObject o; for(auto it=providers_.cbegin();it!=providers_.cend();++it) o[it.key()]=it.value()->capabilities({}).toJson();return o;}
QJsonObject ProviderRegistry::diagnostics() const {QJsonObject o; for(auto it=providers_.cbegin();it!=providers_.cend();++it) o[it.key()]=it.value()->diagnostics();return o;}
}
