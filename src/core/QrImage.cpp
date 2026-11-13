#include "QrImage.h"
#include "qrcodegen/qrcodegen.hpp"
#include <QDateTime>
#include <QStringList>
#include <QtEndian>
#include <zlib.h>
#include <cstring>
#include <vector>
namespace putgd::qr {
namespace {
void chunk(QByteArray& png,const char* type,const QByteArray& data) {
 char length[4];qToBigEndian<quint32>(quint32(data.size()),length);png.append(length,4);
 const auto start=png.size();png.append(type,4);png.append(data);
 auto crc=crc32(0L,Z_NULL,0);crc=crc32(crc,reinterpret_cast<const Bytef*>(png.constData()+start),uInt(png.size()-start));
 char sum[4];qToBigEndian<quint32>(quint32(crc),sum);png.append(sum,4);
}
}
bool offered(const QJsonObject& detail) {return detail["linkAvailable"].toBool()||detail["linkRefreshable"].toBool();}
Error unavailable(const QJsonObject& detail) {
 const auto share=detail["shareStatus"].toString(),remote=detail["remoteStatus"].toString();
 const auto expires=QDateTime::fromString(detail["linkExpiresAt"].toString(),Qt::ISODateWithMs);
 const auto refused=[](const char* reason,const char* message){return Error{"link_unavailable",message,{{"reason",reason}}};};
 if(share=="private")return refused("private","This file is private, so it has no link for a QR code");
 if(share=="revoked")return refused("revoked","This link was revoked");
 if(QStringList{"removed","already_missing"}.contains(remote))return refused("removed","The uploaded file was removed");
 if(expires.isValid()&&expires<=QDateTime::currentDateTimeUtc()&&!detail["linkRefreshable"].toBool())return refused("expired","This link has expired and cannot be refreshed");
 return refused("none","This file has no link to show as a QR code");
}
Error encode(const QByteArray& bytes,Matrix& matrix) {
 matrix={};
 if(bytes.isEmpty())return {"qr_empty","There is no link to put in a QR code",{}};
 if(bytes.size()>maximumBytes)return {"qr_too_long","This link is too long for a QR code",{{"bytes",double(bytes.size())},{"maximumBytes",double(maximumBytes)}}};
 try {
  // Byte mode at exactly M: boostEcl off, so the level never rises with spare room.
  const std::vector<std::uint8_t> data(bytes.cbegin(),bytes.cend());
  const auto code=qrcodegen::QrCode::encodeSegments({qrcodegen::QrSegment::makeBytes(data)},qrcodegen::QrCode::Ecc::MEDIUM,qrcodegen::QrCode::MIN_VERSION,qrcodegen::QrCode::MAX_VERSION,-1,false);
  matrix.size=code.getSize();matrix.modules=QByteArray(qsizetype(matrix.size)*matrix.size,'\0');
  for(int y=0;y<matrix.size;++y)for(int x=0;x<matrix.size;++x)if(code.getModule(x,y))matrix.modules[qsizetype(y)*matrix.size+x]=1;
 } catch(const std::exception&) {
  matrix={};return {"qr_too_long","This link is too long for a QR code",{{"bytes",double(bytes.size())},{"maximumBytes",double(maximumBytes)}}};
 }
 return {};
}
int modulePixels(const Matrix& matrix,int area) {
 if(matrix.isNull())return minimumModulePixels;
 return qMax(minimumModulePixels,qMax(area,windowArea)/matrix.edge());
}
int exportScale(const Matrix& matrix) {
 if(matrix.isNull())return 1;
 return (exportEdge+matrix.edge()-1)/matrix.edge();
}
QImage image(const Matrix& matrix,int scale) {
 if(matrix.isNull()||scale<1)return {};
 const int edge=matrix.edge()*scale;QImage out(edge,edge,QImage::Format_Grayscale8);out.fill(0xff);
 for(int y=0;y<matrix.size;++y)for(int x=0;x<matrix.size;++x)if(matrix.dark(x,y))
  for(int row=0;row<scale;++row){auto* line=out.scanLine((y+quietZone)*scale+row);memset(line+(x+quietZone)*scale,0,size_t(scale));}
 return out;
}
QByteArray png(const Matrix& matrix) {
 if(matrix.isNull())return {};
 const int scale=exportScale(matrix),edge=matrix.edge()*scale,rowBytes=(edge+7)/8;
 // Filter byte 0, then one bit a pixel, most significant first: 1 is white.
 QByteArray raw;raw.reserve(qsizetype(rowBytes+1)*edge);
 for(int py=0;py<edge;++py) {
  raw.append('\0');QByteArray row(rowBytes,'\0');const int my=py/scale-quietZone;
  for(int px=0;px<edge;++px)if(!matrix.dark(px/scale-quietZone,my))row[px/8]=char(quint8(row[px/8])|(0x80>>(px%8)));
  raw.append(row);
 }
 uLongf packedSize=compressBound(uLong(raw.size()));QByteArray packed(qsizetype(packedSize),'\0');
 if(compress2(reinterpret_cast<Bytef*>(packed.data()),&packedSize,reinterpret_cast<const Bytef*>(raw.constData()),uLong(raw.size()),9)!=Z_OK)return {};
 packed.resize(qsizetype(packedSize));
 QByteArray header(13,'\0');qToBigEndian<quint32>(quint32(edge),header.data());qToBigEndian<quint32>(quint32(edge),header.data()+4);
 header[8]=1;header[9]=0; // bit depth 1, greyscale; compression, filter and interlace 0
 QByteArray out=QByteArray::fromHex("89504e470d0a1a0a");
 chunk(out,"IHDR",header);chunk(out,"IDAT",packed);chunk(out,"IEND",{});
 return out;
}
}
