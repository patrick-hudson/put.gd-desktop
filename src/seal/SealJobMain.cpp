#include "SealJobMain.h"
#include "SealCrypto.h"
#include "SealPage.h"
#include <QCryptographicHash>
#include <QFile>
#include <QScopeGuard>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <unistd.h>
namespace putgd::seal {
namespace {
int reply(const QJsonObject& object) {
 const auto bytes=QJsonDocument(object).toJson(QJsonDocument::Compact)+'\n';
 std::fwrite(bytes.constData(),1,size_t(bytes.size()),stdout);std::fflush(stdout);
 return object["ok"].toBool()?0:1;
}
int refuse(const Error& error) {return reply({{"ok",false},{"error",error.toJson()}});}
// A new private file; never one that already exists or a link.
bool create(QFile& file,const QString& path) {
 const int fd=::open(QFile::encodeName(path).constData(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
 return fd>=0&&file.open(fd,QIODevice::WriteOnly,QFileDevice::AutoCloseHandle);
}
// Writes to a file and hashes what it wrote.
struct Output {
 QFile file;QCryptographicHash hash{QCryptographicHash::Sha256};qint64 bytes=0;bool ok=true;
 bool write(const char* data,qint64 size){if(!ok)return false;ok=file.write(data,size)==size;hash.addData(QByteArrayView(data,size));bytes+=size;return ok;}
 QJsonObject result(){return {{"bytes",double(bytes)},{"sha256",QString::fromLatin1(hash.result().toHex())}};}
};
// Base64 of a byte stream in 3-byte groups.
struct Base64Sink {
 Output& out;QByteArray pending;
 bool write(const char* data,qint64 size){pending.append(data,size);const auto whole=pending.size()-pending.size()%3;
  if(whole<=0)return true;const auto text=pending.left(whole).toBase64();pending.remove(0,whole);return out.write(text.constData(),text.size());}
 bool finish(){const auto text=pending.toBase64();pending.clear();return out.write(text.constData(),text.size());}
};
}
bool isSealJob(int argc,char** argv) {return argc==2&&std::strcmp(argv[1],"--seal-job")==0;}
int runSealJob() {
 QByteArray input;char chunk[4096];
 while(const auto count=std::fread(chunk,1,sizeof(chunk),stdin)){input.append(chunk,qsizetype(count));if(input.size()>65536)return refuse({"seal_failed","The sealing request is too large.",{}});}
 auto request=QJsonDocument::fromJson(input).object();OPENSSL_cleanse(input.data(),size_t(input.size()));
 // A passphrase that is there but not text is refused, never read as none.
 if(request.contains("passphrase")&&!request["passphrase"].isString())return refuse({"seal_failed","The sealing request is invalid.",{}});
 const auto passphrase=request["passphrase"].toString();request.remove("passphrase");
 const bool split=request["form"].toString()=="split";
 if(!viewerIntact())return refuse({"seal_viewer_changed","This build's sealed-link viewer doesn't match its pinned hash.",{}});
 const qint64 bytes=qint64(request["bytes"].toDouble(-1));
 if(!request["input"].isString()||!request["page"].isString()||bytes<0||bytes>sealLimit||(!split&&bytes>inlineLimit)||(request["form"]!="inline"&&!split))
  return refuse({"seal_failed","The sealing request is invalid.",{}});
 if(split&&(!request["data"].isString()||!validBinName(request["bin"].toString())))return refuse({"seal_failed","The sealing request is invalid.",{}});
 QFile source(request["input"].toString());
 if(!source.open(QIODevice::ReadOnly)||source.size()!=bytes)return refuse({"seal_failed","Couldn't read the file to seal.",{}});
 auto key=randomKey();if(key.isEmpty())return refuse({"seal_failed","Couldn't make a key.",{}});
 const auto wipeKey=qScopeGuard([&key]{OPENSSL_cleanse(key.data(),size_t(key.size()));});
 const Meta meta{request["name"].toString(),request["type"].toString(),bytes};
 Output page;if(!create(page.file,request["page"].toString()))return refuse({"seal_failed","Couldn't create the sealed page.",{}});
 // What this job created goes again unless it answers ok; an existing file was never ours.
 bool done=false;QStringList created{request["page"].toString()};
 const auto discard=qScopeGuard([&]{if(!done)for(const auto& path:std::as_const(created))QFile::remove(path);});
 const auto prefix=pagePrefix(split),suffix=pageSuffix(),open=dataOpen(split,request["bin"].toString());
 page.write(prefix.constData(),prefix.size());page.write(open.constData(),open.size());
 QJsonObject result{{"ok",true}};
 if(split) {
  Output data;if(!create(data.file,request["data"].toString()))return refuse({"seal_failed","Couldn't create the sealed data file.",{}});
  created.append(request["data"].toString());
  if(auto e=seal(source,meta,key,passphrase,[&data](const char* d,qint64 n){return data.write(d,n);}))return refuse(e);
  if(!data.ok||!data.file.flush()||::fsync(data.file.handle())!=0)return refuse({"seal_failed","Couldn't write the sealed data file.",{}});
  result["data"]=data.result();
 }else {
  Base64Sink text{page,{}};
  if(auto e=seal(source,meta,key,passphrase,[&text](const char* d,qint64 n){return text.write(d,n);}))return refuse(e);
  text.finish();
 }
 const auto close=dataClose();page.write(close.constData(),close.size());page.write(suffix.constData(),suffix.size());
 if(!page.ok||!page.file.flush()||::fsync(page.file.handle())!=0)return refuse({"seal_failed","Couldn't write the sealed page.",{}});
 result["page"]=page.result();result["key"]=fragment(key);
 done=true;
 return reply(result);
}
}
