#include "GnomeProtocol.h"
#include "platform/Geometry.h"
#include <QBuffer>
#include <QImageReader>
#include <QJsonArray>
#include <QUrl>
#include <QSet>
#include <cmath>
#include <limits>
namespace putgd::gnome {
namespace {
PayloadResult fail(const QString& code,const QString& message){return {{},{code,message,{}}};}
QRectF rect(const QJsonValue& value,bool& valid){
 const auto a=value.toArray();if(a.size()!=4){valid=false;return {};}
 for(const auto& v:a)if(!v.isDouble()||!std::isfinite(v.toDouble()))valid=false;
 QRectF r(a[0].toDouble(),a[1].toDouble(),a[2].toDouble(),a[3].toDouble());
 if(r.width()<=0||r.height()<=0||std::abs(r.x())>INT_MAX/4||std::abs(r.y())>INT_MAX/4||r.width()>INT_MAX/4||r.height()>INT_MAX/4)valid=false;
 return r;
}
PayloadResult image(const QByteArray& bytes,const ResourceLimits& limits){
 if(bytes.size()>limits.imageBytes)return fail("input_excessive","GNOME image bytes exceed resource limits");
 QBuffer buffer;buffer.setData(bytes);buffer.open(QIODevice::ReadOnly);QImageReader reader(&buffer);const auto size=reader.size();
 if(!size.isValid())return fail("source_unsupported","GNOME image is not decodable");
 const qint64 pixels=qint64(size.width())*size.height();
 if(pixels>limits.imagePixels||pixels>limits.imageBytes/4)return fail("input_excessive","GNOME image dimensions exceed resource limits");
 PayloadResult result;result.payload.kind="image";result.payload.mime="image/png";result.payload.image=reader.read();
 if(result.payload.image.isNull())return fail("source_unsupported","GNOME image decoding failed");
 return result;
}
}
PayloadResult decodeTransfer(const QJsonObject& metadata,const QByteArray& bytes,const QString& requestId,const ResourceLimits& limits) {
 const auto length=metadata["bytes"];
 if(metadata["version"].toInt()!=protocolVersion||requestId.isEmpty()||metadata["requestId"].toString()!=requestId||!length.isDouble()||length.toDouble()!=bytes.size())return fail("bridge_protocol","GNOME transfer identity, version, or length is invalid");
 const auto mime=metadata["mime"].toString();
 if(mime=="application/x-putgd-capture"){
  if(bytes.size()>limits.imageBytes)return fail("input_excessive","GNOME capture bytes exceed resource limits");
  const auto entries=metadata["frames"].toArray();if(entries.isEmpty()||entries.size()>64)return fail("bridge_protocol","GNOME capture frame count is invalid");
  QList<CaptureFrame> frames;QSet<QString> identities;qint64 offset=0,pixels=0;bool valid=true;
  for(const auto& value:entries){
   const auto entry=value.toObject();const auto lengthValue=entry["length"].toDouble(-1);const auto start=entry["offset"].toDouble(-1);
   if(!entry["length"].isDouble()||!entry["offset"].isDouble()||start!=offset||lengthValue<=0||std::floor(lengthValue)!=lengthValue||lengthValue>bytes.size()-offset)return fail("bridge_protocol","GNOME capture byte segments are invalid");
   CaptureFrame frame;frame.identity=entry["identity"].toString();frame.logicalRect=rect(entry["logical"],valid);const auto native=rect(entry["native"],valid);
   if(!valid||native.x()!=std::floor(native.x())||native.y()!=std::floor(native.y())||native.width()!=std::floor(native.width())||native.height()!=std::floor(native.height()))return fail("bridge_protocol","GNOME capture rectangles are invalid");
   frame.nativeRect=native.toRect();frame.density=entry["density"].toDouble(0);frame.rotation=entry["rotation"].toInt(-1);
   if(frame.identity.isEmpty()||identities.contains(frame.identity)||!std::isfinite(frame.density)||frame.density<=0||frame.density>16||!(frame.rotation==0||frame.rotation==90||frame.rotation==180||frame.rotation==270))return fail("bridge_protocol","GNOME capture identity, density, or orientation is invalid");
   const qint64 count=qint64(frame.nativeRect.width())*frame.nativeRect.height();
   if(count>limits.imagePixels-pixels||count>limits.imageBytes/4-pixels)return fail("input_excessive","GNOME capture frames exceed resource limits");
   pixels+=count;
   auto decoded=image(bytes.mid(offset,qint64(lengthValue)),limits);if(decoded.error)return decoded;
   frame.pixels=std::move(decoded.payload.image);if(frame.pixels.size()!=frame.nativeRect.size())return fail("bridge_protocol","GNOME decoded dimensions do not match capture metadata");
   identities.insert(frame.identity);frames.append(std::move(frame));offset+=qint64(lengthValue);
  }
  const auto selection=rect(metadata["selection"],valid);if(!valid||offset!=bytes.size())return fail("bridge_protocol","GNOME selection or transfer coverage is invalid");
  PayloadResult result;result.payload.kind="image";result.payload.mime="image/png";result.payload.image=composeFrames(frames,selection,std::min(limits.imagePixels,limits.imageBytes/4),result.error);return result;
 }
 if(mime.startsWith("image/"))return image(bytes,limits);
 if(mime=="text/uri-list"){
  // URI-list bytes contain only references; the shared artifact service validates
  // every referenced local file before publishing the acquired batch.
  if(bytes.size()>limits.clipboardTextBytes)return fail("input_excessive","GNOME file URI list exceeds resource limits");
  QMimeData data;QList<QUrl> urls;for(auto line:bytes.split('\n')){line=line.trimmed();if(!line.isEmpty()&&!line.startsWith('#'))urls.append(QUrl::fromEncoded(line,QUrl::StrictMode));}
  data.setUrls(urls);return ClipboardPayload::fromMime(data,limits);
 }
 if(mime=="text/plain"||mime=="text/plain;charset=utf-8"||mime=="UTF8_STRING"){
  if(bytes.isEmpty())return fail("source_unsupported","GNOME clipboard is empty");
  if(bytes.size()>limits.clipboardTextBytes)return fail("input_excessive","GNOME clipboard text exceeds resource limits");
  PayloadResult result;result.payload.kind="text";result.payload.mime="text/plain";result.payload.bytes=bytes;return result;
 }
 return fail("source_unsupported","GNOME clipboard MIME type is unsupported");
}
}
