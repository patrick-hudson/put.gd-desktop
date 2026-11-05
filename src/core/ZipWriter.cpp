#include "ZipWriter.h"
#include <array>
namespace putgd {
namespace {
void put16(QByteArray& out,quint32 value) {out.append(char(value&0xff));out.append(char((value>>8)&0xff));}
void put32(QByteArray& out,quint32 value) {put16(out,value&0xffff);put16(out,value>>16);}
}
quint32 ZipWriter::crc32(const QByteArray& data) {
 static const auto table=[]{std::array<quint32,256> values{};for(quint32 n=0;n<256;++n){quint32 c=n;for(int k=0;k<8;++k)c=c&1?0xedb88320u^(c>>1):c>>1;values[n]=c;}return values;}();
 quint32 crc=0xffffffffu;
 for(const char byte:data)crc=table[(crc^quint8(byte))&0xff]^(crc>>8);
 return crc^0xffffffffu;
}
void ZipWriter::add(const QString& name,const QByteArray& data) {
 const auto path=name.toUtf8();const auto crc=crc32(data);
 // qCompress output is a 4-byte length, then a zlib stream: a 2-byte header,
 // raw deflate data and a 4-byte Adler-32. A zip entry wants the raw data.
 auto deflated=qCompress(data,6);
 quint16 method=8;QByteArray stored;
 if(deflated.size()>10&&deflated.size()-10<data.size())stored=deflated.mid(6,deflated.size()-10);
 else {method=0;stored=data;}
 const auto local=QDateTime(time_).toLocalTime();
 const quint16 dosTime=quint16((local.time().hour()<<11)|(local.time().minute()<<5)|(local.time().second()/2));
 const quint16 dosDate=quint16(((qMax(1980,local.date().year())-1980)<<9)|(local.date().month()<<5)|local.date().day());
 const quint32 offset=quint32(body_.size());
 // Bit 11: the name is UTF-8.
 QByteArray header;put32(header,0x04034b50);put16(header,20);put16(header,0x0800);put16(header,method);put16(header,dosTime);put16(header,dosDate);
 put32(header,crc);put32(header,quint32(stored.size()));put32(header,quint32(data.size()));put16(header,quint32(path.size()));put16(header,0);
 body_+=header;body_+=path;body_+=stored;
 QByteArray central;put32(central,0x02014b50);put16(central,0x0314);put16(central,20);put16(central,0x0800);put16(central,method);put16(central,dosTime);put16(central,dosDate);
 put32(central,crc);put32(central,quint32(stored.size()));put32(central,quint32(data.size()));put16(central,quint32(path.size()));put16(central,0);put16(central,0);
 put16(central,0);put16(central,0);put32(central,quint32(0100600)<<16);put32(central,offset);
 directory_+=central;directory_+=path;++entries_;
}
QByteArray ZipWriter::finish() {
 QByteArray end;put32(end,0x06054b50);put16(end,0);put16(end,0);put16(end,entries_);put16(end,entries_);
 put32(end,quint32(directory_.size()));put32(end,quint32(body_.size()));put16(end,0);
 return body_+directory_+end;
}
}
