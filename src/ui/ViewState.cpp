#include "ViewState.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>
#include <cmath>
namespace putgd {
namespace {
const QStringList s3Keys{"bucket","region","endpoint","objectPrefix","storageClass","aclMode","publicUrlBase","addressing","connectTimeoutMs","requestTimeoutMs","lowSpeedBytesPerSecond","lowSpeedTimeSeconds"};
const QStringList limitKeys{"maxFiles","pendingOperations","concurrentUploads","maxFileBytes","maxBatchBytes","pendingArtifactBytes","pendingImageBytes","clipboardTextBytes","imagePixels","imageBytes"};
const QStringList retentionKeys{"failedArtifactDays","recentActivityLimit","previewCacheBytes","historyRetentionDays"};
const QStringList filterKeys{"search","category","mime","profileId","uploadStatus","localStatus","remoteStatus","from","to"};
bool fields(const QJsonValue& value,const QStringList& allowed){if(!value.isObject())return false;const auto object=value.toObject();for(auto it=object.begin();it!=object.end();++it)if(!allowed.contains(it.key()))return false;return true;}
bool scalars(const QJsonValue& value,const QStringList& allowed,bool strings=false){if(!fields(value,allowed))return false;const auto object=value.toObject();for(auto v:object)if(strings?!v.isString():(v.isArray()||v.isObject()||v.isUndefined()))return false;return true;}
bool stringArray(const QJsonValue& value,int maximum=1000){if(!value.isArray()||value.toArray().size()>maximum)return false;for(auto v:value.toArray())if(!v.isString())return false;return true;}
bool geometry(const QJsonValue& v){if(!v.isArray()||v.toArray().size()!=4)return false;for(auto n:v.toArray())if(!n.isDouble()||std::floor(n.toDouble())!=n.toDouble()||std::abs(n.toDouble())>2147483647)return false;return v.toArray()[2].toInt()>0&&v.toArray()[3].toInt()>0;}
// Raw JSON is deliberately classified independently of syntactic validity. Only
// a flat object whose named fields are known nonsecret S3 settings can leave the
// helper. Incomplete values/trailing commas are drafts; unknown or incomplete
// field names and nested values cannot be classified and keep the window open.
bool rawS3(const QString& text){
 int i=0;auto space=[&]{while(i<text.size()&&text[i].isSpace())++i;};
 auto quoted=[&](QString& decoded){if(i>=text.size()||text[i]!='"')return false;const int start=i++;bool escaped=false;while(i<text.size()){const auto c=text[i++];if(!escaped&&c=='"'){auto array=QJsonDocument::fromJson(("["+text.mid(start,i-start)+"]").toUtf8());if(!array.isArray())return false;decoded=array.array().first().toString();return true;}if(!escaped&&c=='\\')escaped=true;else escaped=false;}return false;};
 space();if(i==text.size())return true;if(text[i++]!='{')return false;
 while(true){space();if(i==text.size())return true;if(text[i]=='}'){++i;space();return i==text.size();}QString key;if(!quoted(key)||!s3Keys.contains(key))return false;space();if(i==text.size())return true;if(text[i++]!=':')return false;space();if(i==text.size())return true;
  if(text[i]=='"'){QString ignored;if(!quoted(ignored))return true;}else{while(i<text.size()&&text[i]!=','&&text[i]!='}'){if(text[i]=='{'||text[i]=='['||text[i]==']'||text[i]=='"'||text[i]==':')return false;++i;}}
  space();if(i==text.size())return true;if(text[i]=='}'){++i;space();return i==text.size();}if(text[i++]!=',')return false;
 }
}
bool safeDocument(const QJsonValue& value){
 if(!fields(value,{"schemaVersion","actions","profiles","archive","naming","limits","retention","preferences"}))return false;
 const auto d=value.toObject();
 if(d["schemaVersion"]!=1||!fields(d["actions"],{"region","window","desktop","clipboard"}))return false;
 for(auto action:d["actions"].toObject())if(!scalars(action,{"destination","profileId","shortcut"}))return false;
 if(!d["profiles"].isArray()||d["profiles"].toArray().size()>32)return false;
 for(auto v:d["profiles"].toArray()){if(!fields(v,{"id","label","provider","settings","credentials"}))return false;auto profile=v.toObject();if(!profile["id"].isString()||!profile["label"].isString()||profile["provider"]!="s3"||!scalars(profile["settings"],s3Keys)||!scalars(profile["credentials"],{"mode","reference"}))return false;}
 if(!fields(d["archive"],{"root","keepUploads","folders"}))return false;
 const auto archive=d["archive"].toObject();if(!archive["root"].isString()||!archive["keepUploads"].isBool()||!scalars(archive["folders"],{"images","videos","text","other"}))return false;
 return scalars(d["naming"],{"mode","collisionPolicy","fileTemplate","captureTemplate","imageTemplate","textTemplate"})&&scalars(d["limits"],limitKeys)&&scalars(d["retention"],retentionKeys)&&scalars(d["preferences"],{"notifications","launchAtLogin"});
}
bool profileEditor(const QJsonValue& value){if(value.isNull())return true;if(!fields(value,{"id","label","credentialMode","reference","acl","addressing","s3","providerJson"}))return false;const auto o=value.toObject();for(const auto& k:QStringList{"id","label","credentialMode","reference","acl","addressing","providerJson"})if(!o[k].isString())return false;return scalars(o["s3"],s3Keys,true)&&rawS3(o["providerJson"].toString());}
bool settingsState(const QJsonObject& s){
 if(!fields(s,{"draft","numbers","profileEditors","baseRevision","observedRevision","dirty","conflict","initialProfile","profileIndex","section","advanced","geometry","setupJobs"}))return false;
 if(!safeDocument(s["draft"])||!scalars(s["numbers"],limitKeys+retentionKeys,true)||!geometry(s["geometry"]))return false;
 for(const auto& key:QStringList{"dirty","conflict","initialProfile","advanced"})if(!s[key].isBool())return false;
 for(const auto& key:QStringList{"baseRevision","observedRevision"})if(!s[key].isString())return false;
 if(!s["profileIndex"].isDouble()||!s["section"].isDouble()||!s["profileEditors"].isArray()||s["profileEditors"].toArray().size()>32)return false;
 for(auto v:s["profileEditors"].toArray())if(!profileEditor(v))return false;
 if(!s["setupJobs"].isArray())return false;
 for(auto v:s["setupJobs"].toArray()){if(!scalars(v,{"setupId","kind","profileId"},true))return false;auto job=v.toObject();if(job["setupId"].toString().isEmpty()||!QStringList{"storeCredentials","unlockCredentials","testDestination"}.contains(job["kind"].toString()))return false;}
 return true;
}
bool historyState(const QJsonObject& s){if(!fields(s,{"query","filters","selectedIds","deletionId","filtersExpanded","managementExpanded","receiptExpanded","geometry"})||!fields(s["query"],filterKeys+QStringList{"limit","offset"})||!scalars(s["filters"],filterKeys,true)||!stringArray(s["selectedIds"])||!s["deletionId"].isString()||!geometry(s["geometry"]))return false;auto q=s["query"].toObject();for(auto it=q.begin();it!=q.end();++it)if(it.key()=="limit"||it.key()=="offset"){if(!it.value().isDouble()||std::floor(it.value().toDouble())!=it.value().toDouble()||it.value().toDouble()<0||it.value().toDouble()>2147483647)return false;}else if(!it.value().isString())return false;for(const auto& key:QStringList{"filtersExpanded","managementExpanded","receiptExpanded"})if(!s[key].isBool())return false;return true;}
}
Error validateUiState(const QJsonObject& state){
 if(QJsonDocument(state).toJson(QJsonDocument::Compact).size()>maximumUiStateBytes)return {"ui_state_too_large","The draft exceeds the 8 MiB UI state limit. Keep this window open and reduce the draft before closing.",{}};
 for(const auto& key:QStringList{"settings","history","activity"})if(state[key].isObject()&&state[key].toObject().contains("stateError")){
  const auto reason=state[key].toObject()["stateError"].toString();
  if(reason=="ui_state_too_large")return {"ui_state_too_large","The draft exceeds the 8 MiB UI state limit. Keep this window open and reduce the draft before closing.",{}};
  if(reason=="request_awaiting_acceptance")return {"ui_request_pending","A request is awaiting resident acceptance. Wait for its result before closing this window.",{}};
  return {"ui_state_unclassified","The draft contains unclassified provider settings. Keep this window open and correct these fields before closing.",{}};
 }
 bool valid=fields(state,{"schemaVersion","settings","history","activity"})&&state["schemaVersion"]==1;
 if(state.contains("settings"))valid=valid&&state["settings"].isObject()&&settingsState(state["settings"].toObject());
 if(state.contains("history"))valid=valid&&state["history"].isObject()&&historyState(state["history"].toObject());
 if(state.contains("activity")){const auto s=state["activity"].toObject();valid=valid&&fields(state["activity"],{"selectedId","selectedItemId","collapsedIds","geometry"})&&s["selectedId"].isString()&&s["selectedItemId"].isString()&&stringArray(s["collapsedIds"],10000)&&geometry(s["geometry"]);}
 return valid?Error{}:Error{"ui_state_invalid","UI state contains unsupported fields, credential entry data, or unclassified provider settings. Keep the draft open and correct these fields before closing.",{}};
}
}
