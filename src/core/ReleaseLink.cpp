#include "ReleaseLink.h"
#include <QRegularExpression>
namespace putgd {
namespace {
const QString repository="https://github.com/patrick-hudson/put.gd-desktop";
// The grammar of tools/releases/release_identity.py and GenerateBuildInfo.cmake.
const QString base=R"((?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*))";
}
QJsonObject ReleaseLink::toJson() const {return {{"version",version},{"kind",kind},{"url",url.toString()}};}
ReleaseLink releaseLink(const QJsonObject& build){
 static const QRegularExpression release("^"+base+R"((?:-rc\.(?<candidate>[1-9][0-9]*)|-nightly\.(?<nightly>[1-9][0-9]*))?$)");
 static const QRegularExpression development("^"+base+R"(-dev\.(?:0|[1-9][0-9]*)\+g(?<short>[0-9a-f]{12})$)");
 static const QRegularExpression commit("^(?:[0-9a-f]{40}|[0-9a-f]{64})$");
 ReleaseLink link{build["version"].toString(),"none",{}};
 if(const auto match=release.match(link.version);match.hasMatch()){
  link.kind=!match.captured("candidate").isEmpty()?"candidate":!match.captured("nightly").isEmpty()?"nightly":"stable";
  link.url=QUrl(repository+"/releases/tag/v"+link.version);
 }else if(const auto match=development.match(link.version);match.hasMatch()&&build["dirty"]!=true){
  const auto sha=build["gitCommit"].toString();
  if(commit.match(sha).hasMatch()&&sha.startsWith(match.captured("short"))){link.kind="commit";link.url=QUrl(repository+"/commit/"+sha);}
 }
 return link;
}
}
