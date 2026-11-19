#include "DestinationCheck.h"
#include <QCryptographicHash>
#include <QJsonDocument>
namespace putgd::destinationcheck {
QStringList connectionKeys(const QString& provider) {
 // S3: where requests go (endpoint, bucket, region, addressing), what a policy can scope to
 // (objectPrefix), what the server may refuse (storageClass, aclMode), and the link the
 // test opened (linkMode, publicUrlBase). Timeouts and link validity days only tune it.
 if(provider=="s3")return {"endpoint","bucket","region","addressing","objectPrefix","storageClass","aclMode","linkMode","publicUrlBase"};
 if(provider=="backblaze-b2")return {"apiUrl","bucket","objectPrefix","linkMode","publicUrlBase"};
 // Proton: the signed-in account and the upload folder. Its folder name is only shown, and
 // share preferences apply after an upload.
 if(provider=="proton-drive")return {"accountRootUid","folderUid"};
 // A custom uploader: its request, and how the reply gives the link and the deletion URL
 // the test followed, and the file types it accepts, which decide what the test sends. A
 // thumbnail and the error message text are not.
 if(provider=="custom-http")return {"requestMethod","requestUrl","allowInsecureHttp","body","fileFormName","headers","parameters","arguments","secrets","data","dataSecret","responseUrl","deletionUrl","accepts"};
 return {};
}
QString fingerprint(const QString& provider,const QJsonObject& settings,const QJsonObject& credentials) {
 QJsonObject connection;
 const auto keys=connectionKeys(provider);
 // An unknown provider is fingerprinted by all of its settings: a change is never missed.
 if(keys.isEmpty())connection=settings;
 else for(const auto& key:keys)if(settings.contains(key))connection.insert(key,settings.value(key));
 const QJsonObject canonical{{"provider",provider},{"settings",connection},{"credentials",QJsonObject{{"mode",credentials.value("mode")},{"reference",credentials.value("reference")}}}};
 // QJsonObject keeps its keys sorted, so equal connections give equal bytes.
 return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(canonical).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
}
QString fingerprint(const QJsonObject& profile) {
 return fingerprint(profile.value("provider").toString(),profile.value("settings").toObject(),profile.value("credentials").toObject());
}
bool rejectsDestination(const Error& error) {
 static const QStringList codes{
  "s3_authentication_failed","s3_permission_denied","s3_bucket_not_found",
  "b2_authentication_failed","b2_permission_denied","b2_key_restricted","b2_bucket_not_found",
  "proton_credentials_invalid","proton_account_changed","proton_folder_unavailable",
  // The Proton helper's own codes, which only the Proton provider passes on.
  "authentication_required","permission_denied","account_changed","destination_unavailable",
  "http_authentication_failed"};
 return codes.contains(error.code);
}
}
