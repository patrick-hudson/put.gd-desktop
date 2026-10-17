#include "SanitizedPng.h"
#include <QSet>
#include <array>
namespace putgd {
namespace {
const std::array<quint32,256>& crcTable(){
 static const auto table=[]{std::array<quint32,256> values{};for(quint32 n=0;n<256;++n){quint32 c=n;for(int k=0;k<8;++k)c=(c&1)?0xedb88320U^(c>>1):c>>1;values[n]=c;}return values;}();
 return table;
}
quint32 crc(const char* data,qsizetype size){
 quint32 c=0xffffffffU;const auto& table=crcTable();
 for(qsizetype i=0;i<size;++i)c=table[(c^quint8(data[i]))&0xff]^(c>>8);
 return c^0xffffffffU;
}
quint32 be32(const char* p){return (quint32(quint8(p[0]))<<24)|(quint32(quint8(p[1]))<<16)|(quint32(quint8(p[2]))<<8)|quint32(quint8(p[3]));}
Error rejected(){return {"edit_output_invalid","The editor returned an image that is not a sanitized PNG",{}};}
}
Error inspectSanitizedPng(const QByteArray& png,QSize* size){
 static const QByteArray signature=QByteArray::fromHex("89504e470d0a1a0a");
 static const QSet<QByteArray> allowed{"IHDR","PLTE","tRNS","gAMA","cHRM","sRGB","pHYs","IDAT","IEND"};
 static const QSet<QByteArray> single{"IHDR","PLTE","tRNS","gAMA","cHRM","sRGB","pHYs"};
 if(!png.startsWith(signature))return rejected();
 qsizetype offset=signature.size();QSet<QByteArray> seen;bool ended=false,data=false,dataClosed=false;quint32 width=0,height=0;
 while(offset<png.size()){
  if(ended||png.size()-offset<12)return rejected();
  const auto length=be32(png.constData()+offset);
  if(length>0x7fffffffU||qint64(length)>qint64(png.size()-offset-12))return rejected();
  const auto type=png.mid(offset+4,4);
  for(const char c:type)if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')))return rejected();
  if(!allowed.contains(type))return rejected();
  if(crc(png.constData()+offset+4,qsizetype(length)+4)!=be32(png.constData()+offset+8+length))return rejected();
  if(seen.isEmpty()&&type!="IHDR")return rejected();
  if(single.contains(type)&&seen.contains(type))return rejected();
  if(type=="IHDR"){
   if(length!=13)return rejected();
   width=be32(png.constData()+offset+8);height=be32(png.constData()+offset+12);
   if(width<1||height<1||width>0x7fffffffU||height>0x7fffffffU)return rejected();
  }else if(type=="IDAT"){if(dataClosed)return rejected();data=true;}
  else{if(data)dataClosed=true;if(type=="IEND"){if(length!=0||!data)return rejected();ended=true;}}
  seen.insert(type);offset+=12+qsizetype(length);
 }
 if(!ended)return rejected();
 if(size)*size=QSize(int(width),int(height));
 return {};
}
}
