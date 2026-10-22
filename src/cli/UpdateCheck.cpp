#include "UpdateCheck.h"
#include "core/BuildInfo.h"
#include "core/CaBundle.h"
#include "core/SystemUpdates.h"
#include "core/Update.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <cstdio>
#include <cstring>
namespace putgd {
namespace {
// The feed is a few KiB; anything near this is not the feed.
constexpr qint64 maximumFeedBytes=256*1024;
constexpr int transferTimeoutMs=20000;
int print(const QJsonObject& object) {
 const auto bytes=QJsonDocument(object).toJson(QJsonDocument::Compact)+'\n';std::fwrite(bytes.constData(),1,bytes.size(),stdout);std::fflush(stdout);
 return object["ok"].toBool()?0:1;
}
int finish(const Error& error,const QJsonObject& result={}) {return print({{"schemaVersion",1},{"ok",!error},{"result",error?QJsonObject{}:result},{"error",error.toJson()}});}
QJsonObject manifest() {return QJsonDocument::fromJson(buildManifestJson()).object();}
// --open-software-updates: Update Manager, Discover or GNOME Software, for a
// DEB/RPM install whose repository file from pkg.put.gd is in use.
int openUpdater(int argc,char** argv) {
 QCoreApplication app(argc,argv);
 const auto build=update::build(manifest());
 const auto action=update::systemAction(build.target);
 if(action["kind"]!="software-updates")return finish({"software_updates_unavailable","This installation does not update from put.gd's repository",{{"repository",action["repository"]}}});
 if(action["updater"].isNull())return finish({"software_updates_unavailable","No software updater is installed",{{"command",action["command"]}}});
 if(!update::openSoftwareUpdates(action))return finish({"software_updates_failed","Cannot start the software updater",{{"updater",action["updater"].toObject()["id"]}}});
 return finish({},{{"opened",action["updater"].toObject()["id"]},{"action",action}});
}
}
bool isUpdateMode(int argc,char** argv) {
 return (argc==2&&(std::strcmp(argv[1],"--update-info")==0||std::strcmp(argv[1],"--open-software-updates")==0))||(argc==3&&std::strcmp(argv[1],"--check-update")==0);
}
int runUpdateMode(int argc,char** argv) {
 // --update-info: what this build would check, without any network access.
 if(argc==2&&std::strcmp(argv[1],"--update-info")==0){std::printf("%s\n",QJsonDocument(update::build(manifest()).toJson()).toJson(QJsonDocument::Compact).constData());return 0;}
 if(argc==2)return openUpdater(argc,argv);
 QCoreApplication app(argc,argv);QCoreApplication::setOrganizationName("putgd");QCoreApplication::setApplicationName("putgd-cli");
 const auto build=update::build(manifest());
 if(!build.supported)return finish({"update_check_unavailable","This build does not check for updates",{{"reason",build.reason}}});
 useHostCaBundle();
 Error error;const auto result=fetchUpdate(build,QString::fromLocal8Bit(argv[2]),error);
 return finish(error,result);
}
QJsonObject fetchUpdate(const update::Build& build,const QString& requested,Error& error) {
 // How this installation updates: the system's updater while the package's
 // repository file is in use, else a download. Read here, not in the resident.
 // A repository install follows its repository's channel, whichever the
 // resident asked for: that's what apt and dnf install (#386).
 auto action=update::systemAction(build.target);
 const auto followed=action["kind"]=="software-updates"?action["channel"].toString():QString();
 const bool repository=followed=="stable"||followed=="nightly";
 const auto channel=repository?followed:requested;
 const auto url=update::feedUrl(channel,error);if(error)return {};
 // The same request from every installation: no query, cookies, version or
 // locale. The version comparison happens here, on this computer.
 QNetworkRequest request(url);
 request.setHeader(QNetworkRequest::UserAgentHeader,"put.gd-update-check/1");
 request.setRawHeader("Accept","application/json");request.setRawHeader("Accept-Language","en");
 request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,QNetworkRequest::Manual);
 request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,QNetworkRequest::Manual);
 request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,QNetworkRequest::AlwaysNetwork);
 request.setAttribute(QNetworkRequest::CacheSaveControlAttribute,false);
 request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::NoLessSafeRedirectPolicy);
 request.setMaximumRedirectsAllowed(3);request.setTransferTimeout(transferTimeoutMs);
 QNetworkAccessManager network;QEventLoop loop;
 auto* reply=network.get(request);QByteArray body;bool tooLarge=false;
 QObject::connect(reply,&QNetworkReply::readyRead,reply,[&]{body+=reply->readAll();if(body.size()>maximumFeedBytes){tooLarge=true;reply->abort();}});
 QObject::connect(reply,&QNetworkReply::finished,&loop,&QEventLoop::quit);
 if(!reply->isFinished())loop.exec();
 if(reply->isOpen())body+=reply->readAll();reply->deleteLater();
 const auto status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
 if(tooLarge){error={"update_feed_invalid","The update feed is larger than 256 KiB",{}};return {};}
 if(reply->error()!=QNetworkReply::NoError||status!=200){
  error={"update_feed_unavailable","Cannot read the update feed",{{"httpStatus",status?QJsonValue(status):QJsonValue()},{"network",reply->error()==QNetworkReply::NoError?QString():reply->errorString().left(200)}}};
  return {};
 }
 auto result=update::evaluate(body,build,channel,error);if(error)return {};
 result["checkedAt"]=QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
 result["channelSource"]=repository?"repository":"setting";
 // Moved to the other channel's repository, whose release is older than the
 // installed package: the system updater keeps this version until a newer one.
 if(repository&&channel!=build.channel&&result["latest"].isString()){
  const auto order=update::compareVersions(result["latest"].toString(),build.version);
  if(order&&*order<0)action["older"]=QJsonObject{{"version",result["latest"]},{"command",update::installFromRepositoryCommand(action["manager"].toString(),channel)}};
 }
 result["action"]=action;
 return result;
}
}
