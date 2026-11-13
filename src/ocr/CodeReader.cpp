#include "CodeReader.h"
#include "core/TextJobProtocol.h"
#include <QStringDecoder>
#include <ZXing/ReadBarcode.h>
#include <algorithm>
#include <string>

namespace putgd::ocr {
namespace {
// ZXing 2.x names the formats QRCode, MicroQRCode, DataMatrix and Aztec; 3.x
// returns a variant ("QR Code Model 2", "Aztec Code") with a spaced name. Only
// ReadBarcodes(ImageView, ReaderOptions) and the inferred result type are used,
// the same in 2.2.1, 2.3.0 and 3.1.1. rMQR, which 3.x reads as a QR variant,
// is not one of the four formats and is left out.
QString formatName(const std::string& name){
 if(name=="QRCode"||name=="QR Code"||name=="QR Code Model 1"||name=="QR Code Model 2")return QStringLiteral("QRCode");
 if(name=="MicroQRCode"||name=="Micro QR Code")return QStringLiteral("MicroQRCode");
 if(name=="DataMatrix"||name=="Data Matrix")return QStringLiteral("DataMatrix");
 if(name=="Aztec"||name=="Aztec Code"||name=="Aztec Rune")return QStringLiteral("Aztec");
 return {};
}
// Strict UTF-8: false for any malformed sequence, overlong form or surrogate.
bool decodeUtf8(const QByteArray& bytes,QString& text){
 QStringDecoder decoder(QStringDecoder::Utf8,QStringDecoder::Flag::Stateless);
 QString decoded=decoder.decode(bytes);
 if(decoder.hasError())return false;
 text=std::move(decoded);return true;
}
}
QList<Code> readCodes(const GreyView& image){
 QList<Code> codes;
 if(!image.valid())return codes;
 ZXing::ImageView view(image.data,image.width,image.height,ZXing::ImageFormat::Lum,image.stride);
 ZXing::ReaderOptions options;
 options.setFormats(ZXing::BarcodeFormat::QRCode|ZXing::BarcodeFormat::MicroQRCode|ZXing::BarcodeFormat::DataMatrix|ZXing::BarcodeFormat::Aztec);
 options.setTryHarder(true);options.setTryRotate(true);options.setTryInvert(true);options.setTryDownscale(true);
 options.setMaxNumberOfSymbols(255);
 const auto results=ZXing::ReadBarcodes(view,options);
 for(const auto& result:results){
  if(!result.isValid())continue;
  Code code;code.format=formatName(ZXing::ToString(result.format()));
  if(code.format.isEmpty())continue;
  const auto& bytes=result.bytes();code.length=qsizetype(bytes.size());
  const auto position=result.position();
  code.quad={QPoint(position.topLeft().x,position.topLeft().y),QPoint(position.topRight().x,position.topRight().y),
             QPoint(position.bottomRight().x,position.bottomRight().y),QPoint(position.bottomLeft().x,position.bottomLeft().y)};
  // Text only when ZXing found text and its rendering is valid UTF-8;
  // anything else is binary and reported by length alone.
  const auto type=result.contentType();
  if(type==ZXing::ContentType::Text||type==ZXing::ContentType::GS1||type==ZXing::ContentType::ISO15434){
   const auto text=result.text();QString decoded;
   if(decodeUtf8(QByteArray(text.data(),qsizetype(text.size())),decoded)){
    code.content=textjob::sanitizeCodeContent(decoded);code.text=!code.content.isEmpty();
   }
  }
  codes.append(std::move(code));
 }
 std::stable_sort(codes.begin(),codes.end(),[](const Code& a,const Code& b){
  const auto ca=a.centre(),cb=b.centre();return ca.y()!=cb.y()?ca.y()<cb.y():ca.x()<cb.x();
 });
 return codes;
}
}
