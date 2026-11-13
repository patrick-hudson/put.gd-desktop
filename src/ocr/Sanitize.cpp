#include "Sanitize.h"
#include <QChar>
#include <QList>
#include <QStringDecoder>

namespace putgd::ocr {
QString sanitizeUntrusted(const QString& text,Untrusted kind){
 const auto points=text.toUcs4();
 QList<char32_t> kept;kept.reserve(points.size());
 for(auto c:points){
  if(c==0x2028||c==0x2029){kept.append(U'\n');continue;}
  if(c==U'\t'||c==U'\n'){kept.append(c);continue;}
  if(c<0x20||(c>=0x7f&&c<=0x9f))continue;
  if(QChar::category(c)==QChar::Other_Format){
   if(kind==Untrusted::RecognizedText&&(c==0x200c||c==0x200d))kept.append(c);
   continue;
  }
  kept.append(c);
 }
 return QString::fromUcs4(kept.constData(),kept.size()).normalized(QString::NormalizationForm_C);
}
bool decodeUtf8(const QByteArray& bytes,QString& text){
 QStringDecoder decoder(QStringDecoder::Utf8,QStringDecoder::Flag::Stateless);
 QString decoded=decoder.decode(bytes);
 if(decoder.hasError())return false;
 text=std::move(decoded);return true;
}
QString clampUtf8(const QString& text,qsizetype maximumBytes,bool& cut){
 qsizetype bytes=0,end=0;
 for(qsizetype i=0;i<text.size();){
  const auto c=text.at(i);qsizetype units=1,size=1;
  if(c.isHighSurrogate()&&i+1<text.size()&&text.at(i+1).isLowSurrogate()){units=2;size=4;}
  else if(c.unicode()>=0x800)size=3;else if(c.unicode()>=0x80)size=2;
  if(bytes+size>maximumBytes){cut=true;return text.left(end);}
  bytes+=size;i+=units;end=i;
 }
 return text;
}
}
