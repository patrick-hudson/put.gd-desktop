#include "ReleaseLink.h"
#include <QDate>
#include <QRegularExpression>
namespace putgd {
namespace {
const QString releases="https://github.com/patrick-hudson/put.gd-desktop/releases/tag/v";
}
std::optional<ReleaseVersion> parseReleaseVersion(const QString& version){
 // The grammar of tools/releases/release_identity.py and GenerateBuildInfo.cmake.
 static const QRegularExpression calendar(R"(^([2-9][0-9])\.([1-9][0-9]{2,3})\.([1-9][0-9]{0,8})(-nightly)?$)");
 static const QRegularExpression legacy(R"(^0\.(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8}))"
                                        R"((?:-rc\.(?<candidate>[1-9][0-9]{0,8})|-nightly\.(?<nightly>[1-9][0-9]{0,8}))?$)");
 if(const auto match=calendar.match(version);match.hasMatch()) {
  const auto year=match.captured(1).toInt(),date=match.captured(2).toInt();
  // 26 is the year the scheme started; the date must exist.
  if(year<26||!QDate(2000+year,date/100,date%100).isValid())return std::nullopt;
  const bool nightly=!match.captured(4).isEmpty();
  return ReleaseVersion{nightly?"nightly":"stable",{qulonglong(year),qulonglong(date),match.captured(3).toULongLong(),nightly?0ULL:2ULL,0}};
 }
 const auto match=legacy.match(version);if(!match.hasMatch())return std::nullopt;
 ReleaseVersion result{"stable",{0,match.captured(1).toULongLong(),match.captured(2).toULongLong(),2,0}};
 if(!match.captured("nightly").isEmpty()){result.kind="nightly";result.key[3]=0;result.key[4]=match.captured("nightly").toULongLong();}
 else if(!match.captured("candidate").isEmpty()){result.kind="candidate";result.key[3]=1;result.key[4]=match.captured("candidate").toULongLong();}
 return result;
}
QJsonObject ReleaseLink::toJson() const {return {{"version",version},{"kind",kind},{"url",url.toString()}};}
ReleaseLink releaseLink(const QJsonObject& build){
 ReleaseLink link{build["version"].toString(),"none",{}};
 if(const auto version=parseReleaseVersion(link.version);version&&build["dirty"]!=true){
  link.kind=version->kind;
  link.url=QUrl(releases+link.version);
 }
 return link;
}
}
