#include "ReleaseLink.h"
#include <QRegularExpression>
namespace putgd {
namespace {
const QString releases="https://github.com/patrick-hudson/put.gd-desktop/releases/tag/v";
}
QJsonObject ReleaseLink::toJson() const {return {{"version",version},{"kind",kind},{"url",url.toString()}};}
ReleaseLink releaseLink(const QJsonObject& build){
 // The grammar of tools/releases/release_identity.py and GenerateBuildInfo.cmake.
 static const QRegularExpression release(R"(^(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*))"
                                         R"((?:-rc\.(?<candidate>[1-9][0-9]*)|-nightly\.(?<nightly>[1-9][0-9]*))?$)");
 ReleaseLink link{build["version"].toString(),"none",{}};
 if(const auto match=release.match(link.version);match.hasMatch()&&build["dirty"]!=true){
  link.kind=!match.captured("candidate").isEmpty()?"candidate":!match.captured("nightly").isEmpty()?"nightly":"stable";
  link.url=QUrl(releases+link.version);
 }
 return link;
}
}
