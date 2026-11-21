#include "Zsync.h"
#include <QCryptographicHash>
#include <QEventLoop>
#include <QFile>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <algorithm>
#include <bit>
#include <cstring>
#include <memory>
namespace putgd::zsync {
namespace {
// zsync 0.6.2's rolling checksum over a block (librcksum rcksum_calc_rsum_block):
// a is the byte sum, b the sum of each byte times its distance from the end,
// both modulo 2^16.
struct Rsum {quint16 a=0,b=0;};
Rsum rsum(const uchar* data,qint64 length) {
 quint32 a=0,b=0;
 for(qint64 i=0;i<length;++i){a+=data[i];b+=quint32(length-i)*data[i];}
 return {quint16(a),quint16(b)};
}
// One byte on (UPDATE_RSUM): out leaves the window, in joins it.
void roll(Rsum& r,uchar out,uchar in,int shift) {
 r.a=quint16(r.a+in-out);
 r.b=quint16(r.b+r.a-(quint32(out)<<shift));
}
QByteArray md4(const uchar* data,qint64 length) {
 return QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char*>(data),length),QCryptographicHash::Md4);
}
bool number(const QByteArray& text,qint64& value) {
 if(text.isEmpty()||text.size()>18)return false;
 for(const char c:text)if(c<'0'||c>'9')return false;
 value=text.toLongLong();return true;
}
// "bytes a-b/total" for a part of a file of length bytes.
bool contentRange(const QByteArray& header,qint64 length,qint64& first,qint64& last) {
 const auto value=header.trimmed();
 if(!value.startsWith("bytes "))return false;
 const auto spec=value.mid(6).trimmed();const auto slash=spec.indexOf('/'),dash=spec.indexOf('-');
 qint64 total=0;
 if(slash<0||dash<0||dash>slash||!number(spec.left(dash),first)||!number(spec.mid(dash+1,slash-dash-1),last)||!number(spec.mid(slash+1),total))return false;
 return total==length&&first<=last&&last<length;
}
constexpr qint64 maximumHeaderBytes=64*1024,partHeaderBytes=8*1024;
// Runs per Range request and requests in flight.
constexpr int maximumRanges=32,parallelRequests=4;
// No bytes for this long ends a request; the whole fetch ends after deadlineMs.
constexpr int stallMs=60000,deadlineMs=15*60*1000;
}

bool parse(const QByteArray& data,Control& control,QString& reason) {
 control={};
 const auto end=data.indexOf("\n\n");
 if(end<0||end>maximumHeaderBytes){reason="bad_header";return false;}
 QHash<QByteArray,QByteArray> fields;
 for(const auto& line:data.left(end).split('\n')) {
  const auto colon=line.indexOf(':');
  if(colon<=0){reason="bad_header";return false;}
  const auto key=line.left(colon).trimmed();
  if(fields.contains(key)){reason="bad_header";return false;}
  fields.insert(key,line.mid(colon+1).trimmed());
 }
 // Fields this client doesn't know are refused unless the file lists them as
 // safe to ignore, as zsync 0.6.2 does. Recompressed (gzip) targets are not
 // supported.
 const auto safe=fields.value("Safe").split(' ');
 static const QList<QByteArray> known{"zsync","Filename","MTime","Blocksize","Length","Hash-Lengths","URL","SHA-1","Safe"};
 for(auto it=fields.cbegin();it!=fields.cend();++it)if(!known.contains(it.key())&&!safe.contains(it.key())){reason="unsupported_header";return false;}
 if(!fields.contains("zsync")){reason="bad_header";return false;}
 qint64 blocksize=0,length=0;
 if(!number(fields.value("Blocksize"),blocksize)||blocksize<512||blocksize>(1<<20)||(blocksize&(blocksize-1))
    ||!number(fields.value("Length"),length)||length<=0){reason="bad_header";return false;}
 control.filename=QString::fromUtf8(fields.value("Filename"));
 control.sha1=QString::fromLatin1(fields.value("SHA-1")).toLower();
 if(control.filename.isEmpty()||control.sha1.size()!=40||QByteArray::fromHex(control.sha1.toLatin1()).toHex()!=control.sha1.toLatin1()){reason="bad_header";return false;}
 // zsync's defaults when the field is missing.
 int seq=1,rsumBytes=4,checksumBytes=16;
 if(fields.contains("Hash-Lengths")) {
  const auto parts=fields.value("Hash-Lengths").split(',');qint64 values[3]{};
  if(parts.size()!=3||!number(parts[0],values[0])||!number(parts[1],values[1])||!number(parts[2],values[2])){reason="bad_header";return false;}
  seq=int(values[0]);rsumBytes=int(values[1]);checksumBytes=int(values[2]);
 }
 // zsync accepts rsum_bytes 1 too; one byte can't tell blocks apart, and no
 // zsyncmake writes it.
 if(seq<1||seq>2||rsumBytes<2||rsumBytes>4||checksumBytes<3||checksumBytes>16){reason="unsupported_hash_lengths";return false;}
 control.blocksize=int(blocksize);control.length=length;control.blocks=(length+blocksize-1)/blocksize;
 control.seqMatches=seq;control.rsumBytes=rsumBytes;control.checksumBytes=checksumBytes;
 control.records=data.mid(end+2);
 if(control.records.size()!=control.blocks*(rsumBytes+checksumBytes)){reason="bad_records";control.records.clear();return false;}
 return true;
}

bool reuse(const Control& control,QFile& seed,QFile& output,Assembly& assembly,const std::function<void(qint64,qint64)>& progress,QString& reason) {
 const int bs=control.blocksize,seq=control.seqMatches,shift=std::countr_zero(unsigned(bs));
 const qint64 blocks=control.blocks,context=qint64(bs)*seq,record=control.rsumBytes+control.checksumBytes;
 const quint16 aMask=control.rsumBytes<3?0:control.rsumBytes==3?0xff:0xffff;
 const auto* records=reinterpret_cast<const uchar*>(control.records.constData());
 // The target's rolling checksums, as zsync reads them: the stored trailing
 // bytes of a-then-b, big-endian. One more zero entry ends the last block's pair.
 std::vector<Rsum> target(size_t(blocks)+1);
 for(qint64 id=0;id<blocks;++id) {
  uchar full[4]{};std::memcpy(full+4-control.rsumBytes,records+id*record,size_t(control.rsumBytes));
  target[size_t(id)]={quint16(((full[0]<<8)|full[1])&aMask),quint16((full[2]<<8)|full[3])};
 }
 const auto checksum=[&](qint64 id){return records+id*record+control.rsumBytes;};
 // With two blocks per match the key is both blocks' b; with one, its a and b.
 const auto key=[&](const Rsum& first,const Rsum& second)->quint32{return seq>1?(quint32(first.b)<<16)|second.b:(quint32(first.a&aMask)<<16)|first.b;};
 int bits=10;while((qint64(1)<<bits)<blocks*2&&bits<24)++bits;
 const auto bucket=[&](quint32 value){return size_t((value*2654435761u)>>(32-bits));};
 std::vector<qint32> head(size_t(1)<<bits,-1),next(size_t(blocks),-1);
 // Reverse order, so each chain lists blocks in file order. A block is a
 // first block of a pair only when a block follows it.
 for(qint64 id=blocks-seq;id>=0;--id){auto& first=head[bucket(key(target[size_t(id)],target[size_t(id)+1]))];next[size_t(id)]=first;first=qint32(id);}
 assembly.known.assign(size_t(blocks),0);assembly.reusedBytes=0;
 const auto weak=[&](qint64 id,const Rsum& r){return target[size_t(id)].a==(r.a&aMask)&&target[size_t(id)].b==r.b;};
 bool writeFailed=false;
 const auto place=[&](qint64 id,const uchar* data){
  if(assembly.known[size_t(id)])return;
  const auto bytes=qMin<qint64>(bs,control.length-id*bs);
  if(!output.seek(id*bs)||output.write(reinterpret_cast<const char*>(data),bytes)!=bytes){writeFailed=true;return;}
  assembly.known[size_t(id)]=1;assembly.reusedBytes+=bytes;
 };

 // The seed streams through a window of chunk bytes; past its end come
 // context zero bytes, as zsync pads it, so a short last block can match.
 const qint64 size=seed.size(),chunk=qMax<qint64>(4*1024*1024,16*context);
 std::vector<uchar> buffer(size_t(chunk+context));
 qint64 base=0,filled=0,x=0,end=-1;bool readFailed=false;
 const auto fill=[&](qint64 need){
  if(x+need<=filled)return true;
  if(x>0){std::memmove(buffer.data(),buffer.data()+x,size_t(filled-x));filled-=x;base+=x;x=0;}
  while(filled<need) {
   if(end>=0)return false;
   const auto got=seed.read(reinterpret_cast<char*>(buffer.data())+filled,chunk-filled);
   if(got<0){readFailed=true;return false;}
   if(got==0){end=base+filled;std::memset(buffer.data()+filled,0,size_t(context));filled+=context;}
   else filled+=got;
  }
  return true;
 };
 if(!seed.seek(0)){reason="seed_unreadable";return false;}
 Rsum r0,r1;qint64 nextId=-1,reported=0;
 // How the window moved: by a byte (rolled), or by this many matched blocks.
 int advanced=-1;
 while(fill(context+1)&&!(end>=0&&base+x>=end)) {
  const uchar* data=buffer.data()+x;
  if(advanced!=0) {
   r0=advanced==1&&seq>1?r1:rsum(data,bs);
   if(seq>1)r1=rsum(data+bs,bs);
  }
  if(progress&&base+x>=reported){progress(base+x,size);reported=base+x+chunk;}
  QByteArray first,second;
  const auto strong=[&](qint64 id,int which){
   auto& digest=which?second:first;
   if(digest.isEmpty())digest=md4(data+qint64(which)*bs,bs);
   return std::memcmp(digest.constData(),checksum(id),size_t(control.checksumBytes))==0;
  };
  int matched=0;
  // After a match, the target's next block is tried first, on its own.
  if(nextId>=0&&nextId<blocks&&!assembly.known[size_t(nextId)]&&weak(nextId,r0)&&strong(nextId,0)){place(nextId,data);++nextId;matched=1;}
  if(!matched) {
   nextId=-1;
   auto* link=&head[bucket(key(r0,r1))];
   while(*link>=0) {
    const qint64 id=*link;
    // Blocks already found leave their chain.
    if(assembly.known[size_t(id)]){*link=next[size_t(id)];continue;}
    if(weak(id,r0)&&(seq<2||weak(id+1,r1))&&strong(id,0)&&(seq<2||strong(id+1,1))) {
     place(id,data);if(seq>1)place(id+1,data+bs);
     if(!matched){matched=seq;nextId=id+seq;}
    }
    link=&next[size_t(id)];
   }
  }
  if(writeFailed){reason="write_failed";return false;}
  if(matched){x+=qint64(matched)*bs;advanced=matched;continue;}
  roll(r0,data[0],data[bs],shift);
  if(seq>1)roll(r1,data[bs],data[2*bs],shift);
  ++x;advanced=0;
 }
 if(readFailed){reason="seed_unreadable";return false;}
 if(progress)progress(size,size);
 return true;
}

std::vector<Range> missing(const Control& control,const Assembly& assembly) {
 std::vector<Range> ranges;
 for(qint64 id=0;id<control.blocks;++id) {
  if(assembly.known[size_t(id)])continue;
  const qint64 start=id*control.blocksize,stop=qMin(control.length,start+control.blocksize);
  if(!ranges.empty()&&ranges.back().end==start)ranges.back().end=stop;else ranges.push_back({start,stop});
 }
 return ranges;
}

namespace {
// One Range request and its response: a single 206 part, or
// multipart/byteranges parts, each written where it belongs. Only bytes of the
// runs it asked for are written, each run in order from its start.
struct Request {
 struct Wanted {qint64 start,end,filled;};
 std::vector<Wanted> wanted;
 QNetworkReply* reply=nullptr;
 enum State {Waiting,Boundary,Headers,Data,Trailing,Done} state=Waiting;
 QByteArray boundary,pending;qint64 offset=0,remaining=0;
 QString failure;QJsonObject details;bool finished=false;
 void fail(const QString& reason,const QJsonObject& more={}) {
  if(!failure.isEmpty())return;
  failure=reason;details=more;if(reply&&reply->isRunning())reply->abort();
 }
 // A part [first, last] must lie within what this request asked for.
 bool part(qint64 first,qint64 last) {
  if(first<wanted.front().start||last>=wanted.back().end){fail("range_invalid",{{"part",QString("%1-%2").arg(first).arg(last)}});return false;}
  offset=first;remaining=last-first+1;state=Data;return true;
 }
 // Starts on the first body bytes (or at the end): the status says whether
 // the server sends ranges at all.
 bool begin(qint64 length) {
  if(state!=Waiting)return true;
  const auto status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if(status==200){fail("range_refused",{{"httpStatus",status}});return false;}
  if(status!=206){fail("range_failed",{{"httpStatus",status?QJsonValue(status):QJsonValue()}});return false;}
  const auto type=reply->rawHeader("Content-Type").trimmed();
  if(type.toLower().startsWith("multipart/byteranges")) {
   const auto at=type.toLower().indexOf("boundary=");
   if(at<0){fail("range_invalid",{{"contentType",QString::fromLatin1(type.left(200))}});return false;}
   boundary=type.mid(at+9);const auto semicolon=boundary.indexOf(';');if(semicolon>=0)boundary.truncate(semicolon);
   boundary=boundary.trimmed();if(boundary.size()>=2&&boundary.startsWith('"')&&boundary.endsWith('"'))boundary=boundary.mid(1,boundary.size()-2);
   if(boundary.isEmpty()||boundary.size()>200){fail("range_invalid",{{"contentType",QString::fromLatin1(type.left(200))}});return false;}
   state=Boundary;return true;
  }
  qint64 first=0,last=0;
  if(!contentRange(reply->rawHeader("Content-Range"),length,first,last)){fail("range_invalid",{{"contentRange",QString::fromLatin1(reply->rawHeader("Content-Range").left(200))}});return false;}
  return part(first,last);
 }
};
}

bool fetch(QNetworkAccessManager& network,const QUrl& url,const Control& control,const std::vector<Range>& ranges,QFile& output,Fetch& result,
           const std::function<void(qint64)>& progress,QString& reason,QJsonObject& details) {
 result={};
 if(ranges.empty())return true;
 qint64 wantedBytes=0;
 std::vector<std::unique_ptr<Request>> requests;
 for(size_t at=0;at<ranges.size();at+=maximumRanges) {
  auto request=std::make_unique<Request>();
  for(size_t i=at;i<qMin(ranges.size(),at+maximumRanges);++i){request->wanted.push_back({ranges[i].start,ranges[i].end,ranges[i].start});wantedBytes+=ranges[i].end-ranges[i].start;}
  requests.push_back(std::move(request));
 }
 qint64 written=0;QString failure;QJsonObject failureDetails;
 // Writes the bytes of [at, at+bytes) that some run still waits for.
 const auto deliver=[&](Request& request,qint64 at,const char* data,qint64 bytes){
  result.downloadedBytes+=bytes;
  for(auto& run:request.wanted) {
   const auto from=qMax(at,run.filled),to=qMin(at+bytes,run.end);
   // Bytes past a gap don't count: the run then ends short.
   if(from>=to||qMax(at,run.start)>run.filled)continue;
   if(!output.seek(from)||output.write(data+(from-at),to-from)!=to-from){request.fail("write_failed");return;}
   run.filled=to;written+=to-from;
  }
  if(progress)progress(written);
 };
 const auto feed=[&](Request& request,QByteArray chunk){
  qsizetype at=0;
  while(request.failure.isEmpty()) {
   if(request.state==Request::Data) {
    const auto take=qMin<qint64>(chunk.size()-at,request.remaining);
    if(take<=0)return;
    deliver(request,request.offset,chunk.constData()+at,take);
    at+=take;request.offset+=take;request.remaining-=take;
    if(!request.remaining)request.state=request.boundary.isEmpty()?Request::Trailing:Request::Boundary;
    continue;
   }
   if(at<chunk.size()){request.pending+=chunk.mid(at);chunk.clear();at=0;}
   if(request.state==Request::Trailing){if(!request.pending.isEmpty())request.fail("range_invalid",{{"reason","bytes after the range"}});return;}
   if(request.state==Request::Done){request.pending.clear();return;}
   if(request.state==Request::Boundary) {
    const auto marker="--"+request.boundary;const auto found=request.pending.indexOf(marker);
    if(found<0||request.pending.size()<found+marker.size()+2){if(request.pending.size()>partHeaderBytes)request.fail("range_invalid",{{"reason","no part boundary"}});return;}
    const auto tail=request.pending.mid(found+marker.size(),2);
    if(tail=="--"){request.state=Request::Done;request.pending.clear();return;}
    if(tail!="\r\n"){request.fail("range_invalid",{{"reason","bad part boundary"}});return;}
    request.pending.remove(0,found+marker.size()+2);request.state=Request::Headers;continue;
   }
   if(request.state==Request::Headers) {
    const auto found=request.pending.indexOf("\r\n\r\n");
    if(found<0){if(request.pending.size()>partHeaderBytes)request.fail("range_invalid",{{"reason","part headers too long"}});return;}
    QByteArray range;
    for(const auto& line:request.pending.left(found).split('\n'))if(line.toLower().startsWith("content-range:"))range=line.mid(14);
    qint64 first=0,last=0;
    if(!contentRange(range,control.length,first,last)){request.fail("range_invalid",{{"contentRange",QString::fromLatin1(range.trimmed().left(200))}});return;}
    chunk=request.pending.mid(found+4);at=0;request.pending.clear();
    if(!request.part(first,last))return;
    continue;
   }
   return;
  }
 };
 QEventLoop loop;size_t started=0;int active=0;
 const auto stop=[&](const QString& why,const QJsonObject& more){
  if(failure.isEmpty()){failure=why;failureDetails=more;}
  for(auto& request:requests)if(request->reply&&request->reply->isRunning())request->reply->abort();
 };
 std::function<void()> launch;
 const auto finish=[&](Request& request){
  if(request.finished)return;
  request.finished=true;--active;
  auto* reply=request.reply;
  if(request.failure.isEmpty()&&reply->error()==QNetworkReply::NoError&&request.begin(control.length))feed(request,reply->readAll());
  if(failure.isEmpty()) {
   if(!request.failure.isEmpty())stop(request.failure,request.details);
   else if(reply->error()!=QNetworkReply::NoError) {
    const auto timedOut=reply->error()==QNetworkReply::OperationCanceledError||reply->error()==QNetworkReply::TimeoutError;
    stop(timedOut?"stalled":"range_network",{{"network",reply->errorString().left(200)}});
   } else if(request.state!=Request::Done&&request.state!=Request::Trailing)stop("range_short",{{"state",int(request.state)}});
   else for(const auto& run:request.wanted)if(run.filled!=run.end){stop("range_short",{{"run",QString("%1-%2").arg(run.start).arg(run.end-1)},{"received",run.filled-run.start}});break;}
  }
  reply->deleteLater();request.reply=nullptr;
  if(failure.isEmpty()&&started<requests.size())launch();
  if(!active)loop.quit();
 };
 launch=[&]{
  while(failure.isEmpty()&&active<parallelRequests&&started<requests.size()) {
   auto& request=*requests[started++];
   QByteArray header="bytes=";
   for(const auto& run:request.wanted)header+=QByteArray::number(run.start)+'-'+QByteArray::number(run.end-1)+',';
   header.chop(1);
   QNetworkRequest get(url);
   get.setHeader(QNetworkRequest::UserAgentHeader,"put.gd-update/1");
   get.setRawHeader("Range",header);
   // The bytes as stored: a compressed answer would not be the image's ranges.
   get.setRawHeader("Accept-Encoding","identity");
   get.setAttribute(QNetworkRequest::CookieLoadControlAttribute,QNetworkRequest::Manual);
   get.setAttribute(QNetworkRequest::CookieSaveControlAttribute,QNetworkRequest::Manual);
   get.setAttribute(QNetworkRequest::CacheLoadControlAttribute,QNetworkRequest::AlwaysNetwork);
   get.setAttribute(QNetworkRequest::CacheSaveControlAttribute,false);
   get.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::NoLessSafeRedirectPolicy);
   get.setMaximumRedirectsAllowed(5);get.setTransferTimeout(stallMs);
   auto* reply=network.get(get);request.reply=reply;++active;++result.requests;
   QObject::connect(reply,&QNetworkReply::readyRead,reply,[&,reply,target=&request]{
    if(target->reply!=reply||!target->failure.isEmpty())return;
    if(!target->begin(control.length))return;
    feed(*target,reply->readAll());
   });
   QObject::connect(reply,&QNetworkReply::finished,&loop,[&,reply,target=&request]{if(target->reply==reply)finish(*target);});
  }
 };
 QTimer deadline;deadline.setSingleShot(true);
 QObject::connect(&deadline,&QTimer::timeout,&loop,[&]{stop("timeout",{});});
 deadline.start(deadlineMs);
 launch();
 if(active)loop.exec();
 if(!failure.isEmpty()){reason=failure;details=failureDetails;return false;}
 // Each fetched block against its MD4, the last one zero-padded as zsyncmake
 // hashed it.
 if(!output.flush()){reason="write_failed";return false;}
 QByteArray block(control.blocksize,'\0');
 const auto* records=reinterpret_cast<const uchar*>(control.records.constData());const auto record=control.rsumBytes+control.checksumBytes;
 for(const auto& run:ranges) {
  for(qint64 id=run.start/control.blocksize;id*control.blocksize<run.end;++id) {
   const auto bytes=qMin<qint64>(control.blocksize,control.length-id*control.blocksize);
   block.fill('\0');
   if(!output.seek(id*control.blocksize)||output.read(block.data(),bytes)!=bytes){reason="write_failed";return false;}
   const auto digest=QCryptographicHash::hash(block,QCryptographicHash::Md4);
   if(std::memcmp(digest.constData(),records+id*record+control.rsumBytes,size_t(control.checksumBytes))!=0){reason="range_checksum";details={{"block",id}};return false;}
  }
 }
 return true;
}

QString sha1(const QString& path) {
 QFile file(path);
 if(!file.open(QIODevice::ReadOnly))return {};
 QCryptographicHash hash(QCryptographicHash::Sha1);
 if(!hash.addData(&file))return {};
 return QString::fromLatin1(hash.result().toHex());
}
}
