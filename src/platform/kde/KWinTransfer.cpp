#include "KWinTransfer.h"
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cmath>
#include <limits>
namespace putgd {
KWinImageLayout validateKWinMetadata(const QVariantMap& m,const ResourceLimits& limits,Error& error){
 error={};KWinImageLayout out;auto bad=[&]{error={"capture_metadata","KWin returned invalid raw image metadata",{}};return KWinImageLayout{};};
 if(m.value("type").metaType().id()!=QMetaType::QString||m.value("type").toString()!="raw")return bad();
 for(const char* key:{"width","height","stride","format"})if(m.value(key).metaType().id()!=QMetaType::UInt)return bad();
 const auto width=m.value("width").toUInt(),height=m.value("height").toUInt(),stride=m.value("stride").toUInt(),format=m.value("format").toUInt();
 if(!width||!height)return bad();
 if(limits.imagePixels<1||width>quint64(limits.imagePixels)/height){error={"input_excessive","KWin image dimensions exceed resource limits",{}};return {};}
 if(width>INT_MAX||height>INT_MAX||stride>INT_MAX||format<=QImage::Format_Indexed8||format>=QImage::NImageFormats)return bad();
 QImage probe(1,1,QImage::Format(format));if(probe.isNull()||probe.depth()<8)return bad();
 const auto row=(quint64(width)*probe.depth()+7)/8;
 const int alignment=probe.depth()>=64?int(alignof(quint64)):4;
 if(stride<row||stride%alignment||!stride)return bad();
 if(limits.imageBytes<1||stride>quint64(limits.imageBytes)/height){error={"input_excessive","KWin raw image exceeds resource limits",{}};return {};}
 const auto scale=m.value("scale");if(scale.metaType().id()!=QMetaType::Double||!std::isfinite(scale.toDouble())||scale.toDouble()<=0)return bad();
 out={int(width),int(height),int(stride),QImage::Format(format),scale.toDouble(),qint64(stride)*height};return out;
}
QImage decodeKWinImage(const QVariantMap& metadata,const QByteArray& bytes,const ResourceLimits& limits,Error& error){
 const auto layout=validateKWinMetadata(metadata,limits,error);if(error)return {};
 if(bytes.size()!=layout.bytes){error={"capture_transfer","KWin image transfer is truncated or contains surplus bytes",{}};return {};}
 const QImage view(reinterpret_cast<const uchar*>(bytes.constData()),layout.width,layout.height,layout.stride,layout.format);
 auto image=view.copy();if(image.isNull()){error={"capture_transfer","Cannot allocate the KWin image",{}};return {};}
 image.setDevicePixelRatio(layout.scale);return image;
}
}
