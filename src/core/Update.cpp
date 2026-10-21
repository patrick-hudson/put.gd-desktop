#include "Update.h"
#include "ReleaseLink.h"
#include <QJsonDocument>
#include <QRegularExpression>
#include <tuple>
namespace putgd::update {
namespace {
const QString defaultFeedBase="https://app.put.gd/update/v1/";
const QString releasePages="https://github.com/patrick-hudson/put.gd-desktop/releases/tag/";
const QString releaseDownloads="https://github.com/patrick-hudson/put.gd-desktop/releases/download/";
const QString mirrorDownloads="https://pkg.put.gd/releases/";
const QStringList targets{"ubuntu24.04","ubuntu26.04","debian13","fedora44","appimage","flatpak"};
Error invalidFeed(const QString& message) {return {"update_feed_invalid",message,{}};}
struct Version {qulonglong major=0,minor=0,patch=0;int rank=0;qulonglong number=0;};
std::optional<Version> parse(const QString& text) {
 // The grammar of ReleaseLink and tools/releases/release_identity.py.
 static const QRegularExpression release(R"(^(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8}))"
                                         R"((?:-rc\.(?<candidate>[1-9][0-9]{0,8})|-nightly\.(?<nightly>[1-9][0-9]{0,8}))?$)");
 const auto match=release.match(text);if(!match.hasMatch())return std::nullopt;
 Version version{match.captured(1).toULongLong(),match.captured(2).toULongLong(),match.captured(3).toULongLong()};
 if(!match.captured("nightly").isEmpty()){version.rank=0;version.number=match.captured("nightly").toULongLong();}
 else if(!match.captured("candidate").isEmpty()){version.rank=1;version.number=match.captured("candidate").toULongLong();}
 else version.rank=2;
 return version;
}
}
QJsonObject Build::toJson() const {
 return {{"version",version},{"kind",kind},{"channel",channel.isEmpty()?QJsonValue():QJsonValue(channel)},{"packageTarget",target.isEmpty()?QJsonValue():QJsonValue(target)},
  {"supported",supported},{"reason",reason.isEmpty()?QJsonValue():QJsonValue(reason)}};
}
Build build(const QJsonObject& manifest) {return build(manifest,packageTarget(),checkBuilt());}
Build build(const QJsonObject& manifest,const QString& target,bool built) {
 const auto link=releaseLink(manifest);
 Build result;result.version=link.version;result.kind=link.kind;result.target=target;
 result.channel=link.kind=="nightly"?QString("nightly"):link.kind=="stable"||link.kind=="candidate"?QString("stable"):QString();
 // Flatpak updates itself from its repository; a distribution's own package
 // turns checks off at build time; untagged builds have no feed to follow.
 if(!built)result.reason="disabled_at_build";
 else if(target=="flatpak")result.reason="flatpak";
 else if(target.isEmpty()||!targets.contains(target))result.reason="no_package";
 else if(result.channel.isEmpty())result.reason="development_build";
 result.supported=result.reason.isEmpty();
 return result;
}
std::optional<int> compareVersions(const QString& left,const QString& right) {
 const auto a=parse(left),b=parse(right);if(!a||!b)return std::nullopt;
 const auto key=[](const Version& v){return std::tuple(v.major,v.minor,v.patch,v.rank,v.number);};
 return key(*a)<key(*b)?-1:key(*b)<key(*a)?1:0;
}
namespace {
// base (or the variable that replaces it) plus path: an https URL, or http on
// a loopback address, without query, fragment or user information.
QUrl located(const char* variable,const QString& defaultBase,const QString& path,const QString& code,Error& error) {
 auto base=qEnvironmentVariable(variable,defaultBase);if(!base.endsWith('/'))base+='/';
 const QUrl url(base+path,QUrl::StrictMode);
 // Qt Network stays out of this library: the resident links it.
 const bool loopback=url.scheme()=="http"&&QStringList{"localhost","127.0.0.1","::1"}.contains(url.host());
 if(!url.isValid()||url.host().isEmpty()||url.hasQuery()||url.hasFragment()||!url.userInfo().isEmpty()||(url.scheme()!="https"&&!loopback)){
  error={code,QString::fromLatin1(variable)+" must be an https URL, or http on a loopback address",{}};return {};
 }
 return url;
}
const QRegularExpression& safeFileName() {static const QRegularExpression name(R"(^[A-Za-z0-9][A-Za-z0-9._+~-]{0,199}$)");return name;}
QUrl releaseFile(const char* variable,const QString& base,const QString& tag,const QString& name,Error& error) {
 const auto version=tag.startsWith('v')?parse(tag.mid(1)):std::nullopt;
 if(!version||!safeFileName().match(name).hasMatch()){error={"update_install_failed","Invalid release file",{{"tag",tag.left(64)},{"name",name.left(200)}}};return {};}
 return located(variable,base,tag+"/"+name,"update_download_failed",error);
}
}
QUrl feedUrl(const QString& channel,Error& error) {
 if(channel!="stable"&&channel!="nightly"){error={"invalid_request","The update channel is stable or nightly",{}};return {};}
 return located("PUTGD_UPDATE_FEED_URL",defaultFeedBase,channel+".json","update_feed_unavailable",error);
}
QUrl releaseFileUrl(const QString& tag,const QString& name,Error& error) {return releaseFile("PUTGD_UPDATE_RELEASE_URL",releaseDownloads,tag,name,error);}
QUrl mirrorFileUrl(const QString& tag,const QString& name,Error& error) {return releaseFile("PUTGD_UPDATE_MIRROR_URL",mirrorDownloads,tag,name,error);}
QJsonObject evaluate(const QByteArray& feed,const Build& build,const QString& channel,Error& error) {
 QJsonParseError parseError;const auto document=QJsonDocument::fromJson(feed,&parseError);
 if(parseError.error!=QJsonParseError::NoError||!document.isObject()){error=invalidFeed("The update feed is not a JSON object");return {};}
 const auto root=document.object();
 if(root["schema"]!=1){error=invalidFeed("The update feed uses an unknown schema");return {};}
 if(root["channel"]!=channel){error=invalidFeed("The update feed is for another channel");return {};}
 QJsonObject result{{"channel",channel},{"current",build.version},{"packageTarget",build.target},{"latest",QJsonValue()},{"tag",QJsonValue()},{"publishedAt",QJsonValue()},
  {"notesUrl",QJsonValue()},{"downloadUrl",QJsonValue()},{"asset",QJsonValue()},{"updateAvailable",false}};
 const auto value=root["release"];
 if(value.isNull())return result;
 if(!value.isObject()){error=invalidFeed("The update feed's release is neither an object nor null");return {};}
 const auto release=value.toObject();const auto latest=release["version"].toString();
 const auto version=parse(latest);
 // A stable feed lists only stable versions and a nightly feed only nightlies.
 if(!version||(channel=="nightly")!=(version->rank==0)||(channel=="stable"&&version->rank!=2)){error=invalidFeed("The update feed names an invalid version for its channel");return {};}
 const auto tag="v"+latest;
 if(release["tag"]!=tag){error=invalidFeed("The update feed's tag does not match its version");return {};}
 result["latest"]=latest;result["tag"]=tag;
 if(release["publishedAt"].isString())result["publishedAt"]=release["publishedAt"].toString().left(64);
 // The links are built here, not taken from the feed: an unsigned feed may only
 // pick which of the project's own release files to offer.
 result["notesUrl"]=releasePages+tag;
 const auto asset=release["assets"].toObject()[build.target].toObject();const auto name=asset["name"].toString();
 const auto direct=releaseDownloads+tag+"/"+name;
 const bool own=safeFileName().match(name).hasMatch()&&asset["url"]==direct;
 result["downloadUrl"]=own?direct:QString("https://app.put.gd/dl/"+channel+"/"+build.target);
 // What an install fetches and checks (#293 phase 3). The size bounds the
 // download; the hash must also match the release's SHA256SUMS.
 static const QRegularExpression hex(R"(^[0-9a-f]{64}$)");
 if(own&&asset["size"].isDouble()&&asset["size"].toInteger()>0) {
  QJsonObject described{{"name",name},{"size",asset["size"].toInteger()}};
  if(hex.match(asset["sha256"].toString()).hasMatch())described["sha256"]=asset["sha256"].toString();
  result["asset"]=described;
 }
 const auto order=compareVersions(latest,build.version);
 result["updateAvailable"]=order&&*order>0;
 return result;
}
}
