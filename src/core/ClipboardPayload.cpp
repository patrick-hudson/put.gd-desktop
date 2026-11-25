#include "ClipboardPayload.h"
#include <QUrl>
namespace putgd {
PayloadResult ClipboardPayload::fromMime(const QMimeData& data,const ResourceLimits& limits) {
 PayloadResult r;
 if(data.hasUrls()) {
  bool foreign=false;
  for(const auto& url:data.urls()) {if(url.isLocalFile())r.payload.files.append(url.toLocalFile());else foreign=true;}
  if(!r.payload.files.isEmpty()) {r.payload.kind="files";if(foreign)r.error={"source_unsupported","The clipboard contains a link that isn't a local file.",{}};else if(r.payload.files.size()>limits.maxFiles)r.error={"input_excessive","Too many files",{}};return r;}
 }
 if(data.hasImage()) {
  r.payload.image=qvariant_cast<QImage>(data.imageData()); r.payload.kind="image"; r.payload.mime="image/png";
  const auto& im=r.payload.image;
  if(im.isNull()) r.error={"source_unsupported","Couldn't read the image from the clipboard.",{}};
  else if(qint64(im.width())*im.height()>limits.imagePixels || im.sizeInBytes()>limits.imageBytes) r.error={"input_excessive","The clipboard image exceeds the image size limit.",{}};
  return r;
 }
 if(data.hasText() && !data.text().isEmpty()) {
  r.payload.kind="text"; r.payload.mime="text/plain";
  if(data.text().size()>limits.clipboardTextBytes) {r.error={"input_excessive","The clipboard text exceeds the text size limit.",{}};return r;}
  r.payload.bytes=data.text().toUtf8();
  if(r.payload.bytes.size()>limits.clipboardTextBytes) r.error={"input_excessive","The clipboard text exceeds the text size limit.",{}};
  return r;
 }
 r.error={"source_unsupported","The clipboard is empty or holds something put.gd can't upload.",{}}; return r;
}
}
