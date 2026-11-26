#include "SealCommands.h"
#include "SessionRequest.h"
#include "UpdateCheck.h"
#include "core/BuildInfo.h"
#include "core/CaBundle.h"
#include "seal/SealCrypto.h"
#include "seal/SealPage.h"
#include <QBuffer>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QUrl>
#include <QUuid>
#include <cstring>
namespace putgd::cli {
namespace {
constexpr qint64 maximumPageBytes=96LL*1024*1024;
constexpr qint64 maximumProbeBytes=1024*1024;
int finish(const Error& error,const QJsonObject& result={}) {
 output({{"schemaVersion",1},{"ok",!error},{"result",error?QJsonObject{}:result},{"error",error.toJson()}});
 return error?1:0;
}
// A URL without its query and fragment, for messages: the fragment is the key.
QString shown(const QUrl& url) {return url.adjusted(QUrl::RemoveQuery|QUrl::RemoveFragment|QUrl::RemoveUserInfo).toString();}
QNetworkRequest visitor(const QUrl& url) {
 QNetworkRequest request(url.adjusted(QUrl::RemoveFragment));
 request.setHeader(QNetworkRequest::UserAgentHeader,"put.gd-sealed-link/1");
 request.setRawHeader("Accept-Language","en");
 request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,QNetworkRequest::Manual);
 request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,QNetworkRequest::Manual);
 request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,QNetworkRequest::AlwaysNetwork);
 request.setAttribute(QNetworkRequest::CacheSaveControlAttribute,false);
 request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::NoLessSafeRedirectPolicy);
 request.setMaximumRedirectsAllowed(3);request.setTransferTimeout(60000);
 return request;
}
struct Fetched {int status=0;QByteArray contentType,disposition;qint64 bytes=0;bool tooLarge=false;QString network;QUrl finalUrl;};
// GETs url into `sink` (at most `limit` bytes).
Fetched fetch(const QUrl& url,qint64 limit,QIODevice& sink) {
 QNetworkAccessManager network;QEventLoop loop;Fetched out;
 auto* reply=network.get(visitor(url));bool writeFailed=false;
 QObject::connect(reply,&QNetworkReply::readyRead,reply,[&]{const auto data=reply->readAll();out.bytes+=data.size();
  if(out.bytes>limit){out.tooLarge=true;reply->abort();return;}if(sink.write(data)!=data.size()){writeFailed=true;reply->abort();}});
 QObject::connect(reply,&QNetworkReply::finished,&loop,&QEventLoop::quit);
 if(!reply->isFinished())loop.exec();
 if(reply->isOpen()&&!out.tooLarge&&!writeFailed){const auto data=reply->readAll();out.bytes+=data.size();if(out.bytes>limit)out.tooLarge=true;else if(sink.write(data)!=data.size())writeFailed=true;}
 out.status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
 out.contentType=reply->rawHeader("Content-Type");out.disposition=reply->rawHeader("Content-Disposition");
 out.finalUrl=reply->url();
 if(reply->error()!=QNetworkReply::NoError&&!out.tooLarge)out.network=networkErrorText(*reply);
 if(writeFailed)out.network="write_failed";
 reply->deleteLater();return out;
}
QString readPassphrase(const QString& path,Error& error) {
 if(path.isEmpty())return {};
 QFile file(path);if(!file.open(QIODevice::ReadOnly)){error={"seal_passphrase_file","Couldn't read the passphrase file.",{}};return {};}
 // The whole file is the passphrase and one optional line ending: nothing beyond it is ignored.
 const auto raw=file.read(seal::maximumPassphraseBytes+3);const bool more=!file.atEnd();
 auto text=QString::fromUtf8(raw);
 if(text.endsWith('\n'))text.chop(1);if(text.endsWith('\r'))text.chop(1);
 if(more||text.isEmpty()||text.contains('\n')||text.contains('\r')||text.normalized(QString::NormalizationForm_C).toUtf8().size()>seal::maximumPassphraseBytes){error={"seal_passphrase_file","The passphrase file must hold one line of 1 to 1024 bytes.",{}};return {};}
 return text;
}
QString safeName(QString name) {
 name=QFileInfo(name).fileName();
 for(auto& c:name)if(c.unicode()<0x20||c=='/'||c=='\\'||c.unicode()==0x7f)c='_';
 if(name.isEmpty()||name=="."||name=="..")name="sealed-file";
 return name.left(200);
}
int open(const QStringList& args) {
 QCommandLineParser parser;parser.addOption({{"o","output"},"Where to write the opened file","file"});parser.addOption({"passphrase-file","A file holding the link's passphrase","file"});
 parser.addPositionalArgument("url","The sealed link, including everything after #, or - to read it from standard input");
 if(!parser.parse(args)||parser.positionalArguments().size()!=1)return finish({"invalid_request","Usage: putgd-cli open URL|- [-o FILE] [--passphrase-file FILE]",{}});
 // "-" reads the link from standard input, so its key stays out of the command line and shell history.
 QString text=parser.positionalArguments()[0];
 if(text=="-"){QFile in;if(!in.open(stdin,QIODevice::ReadOnly))return finish({"invalid_request","Couldn't read the link.",{}});text=QString::fromUtf8(in.readLine(16384)).trimmed();}
 const QUrl url(text,QUrl::StrictMode);
 if(!url.isValid()||(url.scheme()!="https"&&url.scheme()!="http"))return finish({"invalid_request","The sealed link must be an http or https address.",{}});
 const auto key=seal::keyFromFragment(url.fragment(QUrl::FullyEncoded));
 if(key.isEmpty())return finish({"seal_key_missing","This link is missing its key. Use the full link, including everything after #.",{}});
 Error error;const auto passphrase=readPassphrase(parser.value("passphrase-file"),error);if(error)return finish(error);
 useHostCaBundle();
 QBuffer page;page.open(QIODevice::ReadWrite);
 const auto got=fetch(url,maximumPageBytes,page);
 if(got.tooLarge)return finish({"seal_page_changed","This page is larger than any sealed-link page.",{}});
 if(!got.network.isEmpty()||got.status!=200)return finish({"seal_download_failed","Couldn't download the sealed link.",{{"httpStatus",got.status},{"network",got.network},{"url",shown(url)}}});
 seal::ParsedPage parsed;if(auto e=seal::parsePage(page.data(),parsed))return finish(e);
 page.close();
 // The container, in a private temporary file.
 QTemporaryFile container(QDir::tempPath()+"/putgd-sealed-XXXXXX");if(!container.open())return finish({"seal_write_failed","Couldn't create a temporary file.",{}});
 if(parsed.split) {
  const auto binUrl=got.finalUrl.isValid()&&!got.finalUrl.isEmpty()?got.finalUrl.resolved(QUrl("./"+parsed.bin)):url.resolved(QUrl("./"+parsed.bin));
  const auto data=fetch(binUrl,seal::containerBytes(seal::sealLimit+seal::maximumMetaBytes+4)+1,container);
  if(data.tooLarge)return finish({"seal_damaged","This link is incomplete or damaged.",{}});
  if(!data.network.isEmpty()||data.status!=200)return finish({"seal_download_failed","Couldn't download the sealed file beside the page.",{{"httpStatus",data.status},{"network",data.network}}});
 }else {
  constexpr qsizetype step=4*1024*1024;
  for(qsizetype at=0;at<parsed.base64.size();at+=step) {
   const auto decoded=QByteArray::fromBase64Encoding(parsed.base64.mid(at,step),QByteArray::AbortOnBase64DecodingErrors);
   if(!decoded||container.write(decoded.decoded)!=decoded.decoded.size())return finish({"seal_damaged","This link is incomplete or damaged.",{}});
  }
  parsed.base64.clear();
 }
 if(!container.flush()||!container.seek(0))return finish({"seal_write_failed","Couldn't read the temporary file.",{}});
 QTemporaryFile plain(QDir::tempPath()+"/putgd-opened-XXXXXX");if(!plain.open())return finish({"seal_write_failed","Couldn't create a temporary file.",{}});
 seal::Meta meta;
 if(auto e=seal::open(container,container.size(),key,passphrase,[&plain](const char* d,qint64 n){return plain.write(d,n)==n;},&meta))return finish(e);
 if(!plain.flush())return finish({"seal_write_failed","Couldn't write the opened file.",{}});
 const auto target=QDir::current().absoluteFilePath(parser.isSet("output")?parser.value("output"):safeName(meta.name));
 if(QFileInfo::exists(target))return finish({"seal_output_exists","A file with that name is already there. Choose another with -o.",{{"path",target}}});
 plain.setAutoRemove(false);plain.close();
 if(!QFile::rename(plain.fileName(),target)){QFile::remove(plain.fileName());return finish({"seal_write_failed","Couldn't save the opened file.",{{"path",target}}});}
 QFile::setPermissions(target,QFile::ReadOwner|QFile::WriteOwner);
 return finish({},{{"path",target},{"name",meta.name},{"type",meta.type},{"bytes",double(meta.size)},{"form",parsed.split?"split":"inline"},{"viewerSha256",QString::fromLatin1(seal::viewerSha256)}});
}
int probe(const QStringList& args) {
 if(args.size()!=2)return finish({"invalid_request","Usage: putgd-cli --seal-probe URL (or - to read it from standard input)",{}});
 QString text=args[1];
 if(text=="-"){QFile in;if(!in.open(stdin,QIODevice::ReadOnly))return finish({"invalid_request","Couldn't read the address.",{}});text=QString::fromUtf8(in.readLine(16384)).trimmed();}
 const QUrl url(text,QUrl::StrictMode);
 if(!url.isValid()||(url.scheme()!="https"&&url.scheme()!="http"))return finish({"invalid_request","The address must be http or https.",{}});
 useHostCaBundle();
 QBuffer body;body.open(QIODevice::ReadWrite);
 const auto got=fetch(url,maximumProbeBytes,body);
 const auto type=QString::fromLatin1(got.contentType).section(';',0,0).trimmed().toLower();
 const auto disposition=QString::fromLatin1(got.disposition).section(';',0,0).trimmed().toLower();
 // Where the page ended up after redirects, scheme and host only: browsers decrypt only on secure pages.
 const auto final=got.finalUrl.isValid()&&!got.finalUrl.isEmpty()?got.finalUrl:url;
 return finish({},{{"httpStatus",got.status},{"network",got.network},{"tooLarge",got.tooLarge},{"contentType",type},{"attachment",disposition=="attachment"},{"finalScheme",final.scheme()},{"finalHost",final.host()},
  {"bytes",double(got.bytes)},{"sha256",QString::fromLatin1(QCryptographicHash::hash(body.data(),QCryptographicHash::Sha256).toHex())}});
}
int upload(const QStringList& args) {
 QCommandLineParser parser;parser.addOption({"seal","Seal these uploads"});parser.addOption({"passphrase-file","With --seal: a file holding the passphrase","file"});
 parser.addOption({"profile","The destination (default: the capture destination)","id"});parser.addOption({"socket","Session socket path","path"});parser.addOption({"request-id","Correlation ID","id"});
 parser.addPositionalArgument("paths","Files to upload","PATH...");
 const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);
 if(!parser.parse(args)||parser.positionalArguments().isEmpty())return fail(id,"invalid_request","Usage: putgd-cli upload [--seal] [--passphrase-file FILE] [--profile ID] PATH...");
 const auto requestId=parser.isSet("request-id")?parser.value("request-id"):id;
 if(parser.isSet("passphrase-file")&&!parser.isSet("seal"))return fail(requestId,"invalid_request","--passphrase-file needs --seal");
 QJsonArray paths;for(const auto& path:parser.positionalArguments())paths.append(QDir::cleanPath(QDir::current().absoluteFilePath(path)));
 QJsonObject params{{"paths",paths}};
 if(parser.isSet("profile"))params["profileId"]=parser.value("profile");
 if(parser.isSet("seal")) {
  QJsonObject seal{{"enabled",true}};
  Error error;const auto passphrase=readPassphrase(parser.value("passphrase-file"),error);if(error)return fail(requestId,error.code,error.message);
  if(!passphrase.isEmpty())seal["passphrase"]=passphrase;
  params["seal"]=seal;
 }
 return sendSessionRequest(parser.value("socket"),requestId,"upload.files",params);
}
}
bool isSealCommand(int argc,char** argv) {
 return argc>=2&&(std::strcmp(argv[1],"open")==0||std::strcmp(argv[1],"upload")==0||std::strcmp(argv[1],"--seal-probe")==0);
}
int runSealCommand(int argc,char** argv) {
 QCoreApplication app(argc,argv);QCoreApplication::setOrganizationName("putgd");QCoreApplication::setApplicationName("putgd-cli");QCoreApplication::setApplicationVersion(buildVersion());
 auto args=app.arguments();const auto command=args.value(1);args.removeAt(1);
 if(command=="--seal-probe")return probe(args);
 if(command=="open")return open(args);
 return upload(args);
}
}
