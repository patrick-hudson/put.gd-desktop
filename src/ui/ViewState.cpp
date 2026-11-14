#include "ViewState.h"
#include <QJsonArray>
#include <QDir>
#include <QJsonDocument>
#include <QStringList>
#include <cmath>
namespace putgd {
namespace {
const QStringList& s3Keys() {static const QStringList value{"linkMode","linkValidityDays","bucket","region","endpoint","objectPrefix","storageClass","aclMode","publicUrlBase","addressing","connectTimeoutMs","requestTimeoutMs","lowSpeedBytesPerSecond","lowSpeedTimeSeconds"};return value;}
const QStringList& protonKeys() {static const QStringList value{"accountRootUid","folderUid","folderName","shareByDefault","linkExpirationDays","sharePasswordReference","requestTimeoutMs"};return value;}
const QStringList& b2Keys() {static const QStringList value{"apiUrl","bucket","objectPrefix","linkMode","linkValidityDays","publicUrlBase","connectTimeoutMs","requestTimeoutMs"};return value;}
const QStringList& providerKeys(const QString& provider){return provider=="s3"?s3Keys():provider=="backblaze-b2"?b2Keys():protonKeys();}
// A custom HTTP uploader (#1361) keeps lists of rows. A row marked secret may carry
// its name only, so a typed or imported token can never ride in a checkpoint.
const QStringList& uploaderScalars() {static const QStringList value{"allowInsecureHttp","body","connectTimeoutMs","data","dataSecret","deletionUrl","errorMessage","fileFormName","requestMethod","requestTimeoutMs","requestUrl","responseUrl","thumbnailUrl"};return value;}
const QStringList& limitKeys() {static const QStringList value{"maxFiles","pendingOperations","concurrentUploads","maxFileBytes","maxBatchBytes","pendingArtifactBytes","pendingImageBytes","clipboardTextBytes","imagePixels","imageBytes","recordingAnimationSeconds","recordingAnimationEdge"};return value;}
const QStringList& retentionKeys() {static const QStringList value{"failedArtifactDays","recentActivityLimit","previewCacheBytes","historyRetentionDays"};return value;}
const QStringList& filterKeys() {static const QStringList value{"search","category","mime","profileId","uploadStatus","localStatus","remoteStatus","from","to"};return value;}
bool fields(const QJsonValue& value,const QStringList& allowed){if(!value.isObject())return false;const auto object=value.toObject();for(auto it=object.begin();it!=object.end();++it)if(!allowed.contains(it.key()))return false;return true;}
bool scalars(const QJsonValue& value,const QStringList& allowed,bool strings=false){if(!fields(value,allowed))return false;const auto object=value.toObject();for(auto v:object)if(strings?!v.isString():(v.isArray()||v.isObject()||v.isUndefined()))return false;return true;}
bool stringArray(const QJsonValue& value,int maximum=1000);
// Recent editor colors (a short string list), the diagnostics object (#74, #122)
// and the appearance object (#60) are the only non-scalar preferences.
bool appearanceDraft(const QJsonValue& value){
 if(!fields(value,{"theme","accent","overrides"}))return false;
 const auto appearance=value.toObject();const auto overrides=appearance["overrides"];
 if(!appearance["theme"].isString()||!(appearance["accent"].isUndefined()||appearance["accent"].isNull()||appearance["accent"].isString()))return false;
 if(overrides.isUndefined())return true;
 if(!fields(overrides,{"dark","light"}))return false;
 for(const auto& scheme:overrides.toObject())if(!scalars(scheme,QStringList(scheme.toObject().keys()),true)||scheme.toObject().size()>64)return false;
 return true;
}
bool preferencesDraft(const QJsonValue& value){
 auto object=value.toObject();const auto colors=object.take("editorRecentColors");const auto diagnostics=object.take("diagnostics");const auto appearance=object.take("appearance");const auto updates=object.take("updates");const auto languages=object.take("textRecognitionLanguages");
 return value.isObject()&&(languages.isUndefined()||stringArray(languages,3))&&scalars(object,{"scrollCaptureMaxHeight","scrollCaptureFromTop","notifications","launchAtLogin","uploadClipboardBehavior","regionSelectionMode","captureDelaySeconds","captureDelayShortcuts","recordingFrameRate","recordingMicrophone","recordingMicrophoneDevice","recordingClickHighlights","unlockCredentialsAtStartup","fileManagerFolders","fileManagerMenu"})&&(colors.isUndefined()||stringArray(colors,6))
  &&(diagnostics.isUndefined()||scalars(diagnostics,{"debugLogging","packageFolder"}))&&(appearance.isUndefined()||appearanceDraft(appearance))&&(updates.isUndefined()||scalars(updates,{"check","channel"}));
}
bool stringArray(const QJsonValue& value,int maximum){if(!value.isArray()||value.toArray().size()>maximum)return false;for(auto v:value.toArray())if(!v.isString())return false;return true;}
bool geometry(const QJsonValue& v){if(!v.isArray()||v.toArray().size()!=4)return false;for(auto n:v.toArray())if(!n.isDouble()||std::floor(n.toDouble())!=n.toDouble()||std::abs(n.toDouble())>2147483647)return false;return v.toArray()[2].toInt()>0&&v.toArray()[3].toInt()>0;}
// Raw JSON is deliberately classified independently of syntactic validity. Only
// a flat object whose named fields are known nonsecret S3 settings can leave the
// helper. Incomplete values/trailing commas are drafts; unknown or incomplete
// field names and nested values cannot be classified and keep the window open.
bool rawS3(const QString& text){
 int i=0;auto space=[&]{while(i<text.size()&&text[i].isSpace())++i;};
 auto quoted=[&](QString& decoded){if(i>=text.size()||text[i]!='"')return false;const int start=i++;bool escaped=false;while(i<text.size()){const auto c=text[i++];if(!escaped&&c=='"'){auto array=QJsonDocument::fromJson(("["+text.mid(start,i-start)+"]").toUtf8());if(!array.isArray())return false;decoded=array.array().first().toString();return true;}if(!escaped&&c=='\\')escaped=true;else escaped=false;}return false;};
 space();if(i==text.size())return true;if(text[i++]!='{')return false;
 while(true){space();if(i==text.size())return true;if(text[i]=='}'){++i;space();return i==text.size();}QString key;if(!quoted(key)||!s3Keys().contains(key))return false;space();if(i==text.size())return true;if(text[i++]!=':')return false;space();if(i==text.size())return true;
  if(text[i]=='"'){QString ignored;if(!quoted(ignored))return true;}else{while(i<text.size()&&text[i]!=','&&text[i]!='}'){if(text[i]=='{'||text[i]=='['||text[i]==']'||text[i]=='"'||text[i]==':')return false;++i;}}
  space();if(i==text.size())return true;if(text[i]=='}'){++i;space();return i==text.size();}if(text[i++]!=',')return false;
 }
}
bool uploaderSettings(const QJsonValue& value){
 if(!value.isObject())return false;
 auto settings=value.toObject();
 const auto accepts=settings.take("accepts");
 if(!accepts.isUndefined()&&!stringArray(accepts,3))return false;
 for(const auto* list:{"headers","parameters","arguments"}){
  const auto rows=settings.take(list);
  if(rows.isUndefined())continue;
  if(!rows.isArray()||rows.toArray().size()>32)return false;
  for(const auto& entry:rows.toArray()){
   const auto row=entry.toObject();
   if(!entry.isObject()||!row["name"].isString()||!row["secret"].isBool())return false;
   if(row["secret"].toBool()?row.size()!=2:(row.size()!=3||!row["value"].isString()))return false;
  }
 }
 // Other secret values: names only.
 if(const auto extra=settings.take("secrets");!extra.isUndefined()){
  if(!extra.isArray()||extra.toArray().size()>32)return false;
  for(const auto& entry:extra.toArray()){const auto row=entry.toObject();if(!entry.isObject()||row.size()!=2||!row["name"].isString()||row["secret"]!=true)return false;}
 }
 // A secret request body has no text in settings.
 if(settings["dataSecret"].toBool()&&!settings["data"].toString().isEmpty())return false;
 return scalars(settings,uploaderScalars());
}
bool providerSettings(const QString& provider,const QJsonValue& settings){return provider=="custom-http"?uploaderSettings(settings):scalars(settings,providerKeys(provider));}
bool safeProfile(const QJsonValue& value){
 if(!fields(value,{"id","label","provider","settings","credentials"}))return false;
 const auto profile=value.toObject();const auto provider=profile["provider"].toString();
 return profile["id"].isString()&&profile["label"].isString()&&QStringList{"s3","proton-drive","backblaze-b2","custom-http"}.contains(provider)
  &&providerSettings(provider,profile["settings"])&&scalars(profile["credentials"],{"mode","reference"});
}
bool safeDocument(const QJsonValue& value){
 if(!fields(value,{"schemaVersion","actions","profiles","archive","naming","limits","retention","preferences","captureDefaults"}))return false;
 const auto d=value.toObject();
 if(d.contains("captureDefaults")&&!scalars(d["captureDefaults"],{"destination","profileId","publicationMode"}))return false;
 if(d["schemaVersion"]!=1||!fields(d["actions"],{"region","window","desktop","clipboard","edit","scroll","record","monitor","repeat","text"}))return false;
 for(auto action:d["actions"].toObject())if(!scalars(action,{"destination","profileId","shortcut","publicationMode"}))return false;
 if(!d["profiles"].isArray()||d["profiles"].toArray().size()>32)return false;
 for(auto v:d["profiles"].toArray())if(!safeProfile(v))return false;
 if(!fields(d["archive"],{"root","keepUploads","folders"}))return false;
 const auto archive=d["archive"].toObject();if(!archive["root"].isString()||!archive["keepUploads"].isBool()||!scalars(archive["folders"],{"images","videos","text","other"}))return false;
 return scalars(d["naming"],{"mode","collisionPolicy","fileTemplate","captureTemplate","imageTemplate","textTemplate","recordingTemplate"})&&scalars(d["limits"],limitKeys())&&scalars(d["retention"],retentionKeys())&&preferencesDraft(d["preferences"]);
}
bool protonEditor(const QJsonValue& value){
 if(!fields(value,{"accountRootUid","accountEmail","folders","folderIndex","share","expiration","timeout"}))return false;
 const auto editor=value.toObject();const auto folders=editor["folders"];
 if(!editor["accountRootUid"].isString()||!editor["share"].isBool()||!editor["expiration"].isString()||!editor["timeout"].isString()||!folders.isArray()||folders.toArray().size()>10003)return false;
 if(editor.contains("accountEmail")&&(!editor["accountEmail"].isString()||editor["accountEmail"].toString().size()>320||editor["accountEmail"].toString().contains('\n')||editor["accountEmail"].toString().contains('\r')||editor["accountEmail"].toString().contains(QChar(0))))return false;
 const auto index=editor["folderIndex"].toDouble(-2);
 if(index!=std::floor(index)||index<-1||index>=folders.toArray().size())return false;
 for(const auto& folder:folders.toArray())if(!scalars(folder,{"uid","name","label"},true))return false;
 return true;
}
bool profileEditor(const QJsonValue& value,bool includeProviders=true){
 if(value.isNull())return true;
 auto allowed=QStringList{"id","label","credentialMode","reference","acl","addressing","s3","providerJson","proton"};if(includeProviders)allowed.append("providers");
 if(!fields(value,allowed))return false;
 const auto editor=value.toObject();
 for(const auto& key:QStringList{"id","label","credentialMode","reference","acl","addressing","providerJson"})if(!editor[key].isString())return false;
 if(!scalars(editor["s3"],s3Keys(),true)||!rawS3(editor["providerJson"].toString()))return false;
 if(editor.contains("proton")&&!protonEditor(editor["proton"]))return false;
 if(!editor.contains("providers"))return true;
 if(!fields(editor["providers"],{"s3","proton-drive","backblaze-b2","custom-http"}))return false;
 const auto providers=editor["providers"].toObject();
 for(auto it=providers.begin();it!=providers.end();++it){
  if(!fields(it.value(),{"settings","credentials","editor"}))return false;
  const auto retained=it.value().toObject();
  if(!providerSettings(it.key(),retained["settings"])||!scalars(retained["credentials"],{"mode","reference"})
     ||!retained["editor"].isObject()||!profileEditor(retained["editor"],false))return false;
 }
 return true;
}

// Derived, read-only presentation of Settings → Capture for CLI/MCP readers;
// restoring a view recomputes it from the draft.
bool captureRouting(const QJsonValue& value){
 if(!fields(value,{"default","region","window","desktop","clipboard","edit","scroll","record","monitor"})||!value.toObject().contains("default"))return false;
 const auto routes=value.toObject();
 for(auto it=routes.begin();it!=routes.end();++it){
  const bool action=it.key()!="default";const QStringList text{"profileId","destination","publicationMode","sharing","effectivePublicationMode","status"};QStringList flags{"destinationEnabled","sharingEnabled"};if(action)flags.append("inherited");
  if(!fields(it.value(),text+flags))return false;
  const auto route=it.value().toObject();
  for(const auto& key:text)if(!route[key].isString())return false;
  for(const auto& key:std::as_const(flags))if(!route[key].isBool())return false;
  if(!QStringList{"default","share","private"}.contains(route["publicationMode"].toString())||!QStringList{"","share","private"}.contains(route["effectivePublicationMode"].toString()))return false;
 }
 return true;
}
bool settingsState(const QJsonObject& s){
 if(s.contains("autosaveBlocked")&&!s["autosaveBlocked"].isBool())return false;
 if(!fields(s,{"autosaveBlocked","draft","numbers","profileEditors","baseRevision","observedRevision","dirty","conflict","initialProfile","profileIndex","section","advanced","resourceLimits","desktopSupportDetails","geometry","setupJobs","provisionalProfile","captureRouting"}))return false;
 if(s.contains("resourceLimits")&&!s["resourceLimits"].isBool())return false;
 if(s.contains("desktopSupportDetails")&&!s["desktopSupportDetails"].isBool())return false;
 if(s.contains("captureRouting")&&!captureRouting(s["captureRouting"]))return false;
 if(!safeDocument(s["draft"])||!scalars(s["numbers"],limitKeys()+retentionKeys(),true)||!geometry(s["geometry"]))return false;
 for(const auto& key:QStringList{"dirty","conflict","initialProfile","advanced"})if(!s[key].isBool())return false;
 for(const auto& key:QStringList{"baseRevision","observedRevision"})if(!s[key].isString())return false;
 if(!s["profileIndex"].isDouble()||!s["section"].isDouble()||!s["profileEditors"].isArray()||s["profileEditors"].toArray().size()>32)return false;
 for(auto v:s["profileEditors"].toArray())if(!profileEditor(v))return false;
 if(s.contains("provisionalProfile")){
  const auto provisional=s["provisionalProfile"];
  if(!fields(provisional,{"profile","editor"}))return false;
  if(!provisional.toObject().isEmpty()&&(!safeProfile(provisional.toObject()["profile"])||!profileEditor(provisional.toObject()["editor"])))return false;
 }
 if(!s["setupJobs"].isArray())return false;
 for(auto v:s["setupJobs"].toArray()){if(!scalars(v,{"setupId","kind","profileId","revision"},true))return false;auto job=v.toObject();if(job["setupId"].toString().isEmpty()||!QStringList{"storeCredentials","unlockCredentials","testDestination","connectAccount","inspectAccount","createFolder","storeSharePassword"}.contains(job["kind"].toString()))return false;}
 return true;
}
bool historyState(const QJsonObject& s){if(s.contains("savedPath")&&(!s["savedPath"].isString()||s["savedPath"].toString().contains(QChar(0))||(!s["savedPath"].toString().isEmpty()&&!QDir::isAbsolutePath(s["savedPath"].toString()))))return false;for(const auto& key:QStringList{"gridMode","showCancelled","detailsOpen","detailsFromGallery","detailsWanted"})if(s.contains(key)&&!s[key].isBool())return false;if(s.contains("selectedItemId")&&!s["selectedItemId"].isString())return false;if(!fields(s,{"query","filters","selectedIds","deletionId","sharingId","filtersExpanded","managementExpanded","receiptExpanded","geometry","gridMode","showCancelled","selectedItemId","detailsOpen","detailsFromGallery","detailsWanted","galleryScroll","listScroll","savedPath"})||!fields(s["query"],filterKeys()+QStringList{"limit","offset"})||!scalars(s["filters"],filterKeys(),true)||!stringArray(s["selectedIds"])||!s["deletionId"].isString()||(s.contains("sharingId")&&!s["sharingId"].isString())||!geometry(s["geometry"]))return false;auto q=s["query"].toObject();for(auto it=q.begin();it!=q.end();++it)if(it.key()=="limit"||it.key()=="offset"){if(!it.value().isDouble()||std::floor(it.value().toDouble())!=it.value().toDouble()||it.value().toDouble()<0||it.value().toDouble()>2147483647)return false;}else if(!it.value().isString())return false;for(const auto& key:QStringList{"filtersExpanded","managementExpanded","receiptExpanded"})if(!s[key].isBool())return false;for(const auto& key:QStringList{"galleryScroll","listScroll"})if(s.contains(key)&&(!s[key].isDouble()||s[key].toDouble()<0||s[key].toDouble()>2147483647||std::floor(s[key].toDouble())!=s[key].toDouble()))return false;return true;}
}
// The resolved appearance (#60), reported read-only for CLI/MCP readers.
bool appearanceState(const QJsonValue& value){
 if(!fields(value,{"theme","desktop","desktopSource","scheme","tokens"}))return false;
 const auto a=value.toObject();
 if(!QStringList{"system","dark","light"}.contains(a["theme"].toString())||!QStringList{"dark","light"}.contains(a["scheme"].toString()))return false;
 if(!(a["desktop"].isNull()||QStringList{"dark","light"}.contains(a["desktop"].toString()))||!a["desktopSource"].isString()||!a["tokens"].isObject()||a["tokens"].toObject().size()>64)return false;
 for(const auto& token:a["tokens"].toObject())if(!token.isString()||token.toString().size()!=7||!token.toString().startsWith('#'))return false;
 return true;
}
Error validateUiState(const QJsonObject& state){
 if(QJsonDocument(state).toJson(QJsonDocument::Compact).size()>maximumUiStateBytes)return {"ui_state_too_large","The draft exceeds the 8 MB UI state limit. Keep this window open and reduce the draft before closing.",{}};
 for(const auto& key:QStringList{"settings","history","activity"})if(state[key].isObject()&&state[key].toObject().contains("stateError")){
  const auto reason=state[key].toObject()["stateError"].toString();
  if(reason=="ui_state_too_large")return {"ui_state_too_large","The draft exceeds the 8 MB UI state limit. Keep this window open and reduce the draft before closing.",{}};
  if(reason=="request_awaiting_acceptance")return {"ui_request_pending","A request is awaiting resident acceptance. Wait for its result before closing this window.",{}};
  return {"ui_state_unclassified","The draft contains unclassified provider settings. Keep this window open and correct these fields before closing.",{}};
 }
 bool valid=fields(state,{"schemaVersion","settings","history","activity","main","appearance"})&&state["schemaVersion"]==1&&(!state.contains("appearance")||appearanceState(state["appearance"]));
 if(state.contains("main")){const auto main=state["main"].toObject();valid=valid&&fields(state["main"],{"page","geometry"})&&geometry(main["geometry"])&&main["page"].isDouble()&&main["page"].toDouble()==main["page"].toInt()&&main["page"].toInt()>=0&&main["page"].toInt()<=2;}
 if(state.contains("settings"))valid=valid&&state["settings"].isObject()&&settingsState(state["settings"].toObject());
 if(state.contains("history"))valid=valid&&state["history"].isObject()&&historyState(state["history"].toObject());
 if(state.contains("activity")){const auto s=state["activity"].toObject();valid=valid&&fields(state["activity"],{"selectedId","selectedItemId","collapsedIds","geometry"})&&s["selectedId"].isString()&&s["selectedItemId"].isString()&&stringArray(s["collapsedIds"],10000)&&geometry(s["geometry"]);}
 return valid?Error{}:Error{"ui_state_invalid","UI state contains unsupported fields, credential entry data, or unclassified provider settings. Keep the draft open and correct these fields before closing.",{}};
}
}
