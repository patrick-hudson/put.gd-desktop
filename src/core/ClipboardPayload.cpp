#include "ClipboardPayload.h"
#include <QUrl>
namespace putgd {
PayloadResult ClipboardPayload::fromMime(const QMimeData& data,const ResourceLimits& limits) {
 PayloadResult r;
 if(data.hasUrls()) {
  bool foreign=false;
  for(const auto& url:data.urls()) {if(url.isLocalFile())r.payload.files.append(url.toLocalFile());else foreign=true;}
  if(!r.payload.files.isEmpty()) {r.payload.kind="files";if(foreign)r.error={"source_unsupported","A file batch contains a non-local URI",{}};else if(r.payload.files.size()>limits.maxFiles)r.error={"input_excessive","Too many files",{}};return r;}
 }
 if(data.hasImage()) {
  r.payload.image=qvariant_cast<QImage>(data.imageData()); r.payload.kind="image"; r.payload.mime="image/png";
  const auto& im=r.payload.image;
  if(im.isNull()) r.error={"source_unsupported","Invalid image",{}};
  else if(qint64(im.width())*im.height()>limits.imagePixels || im.sizeInBytes()>limits.imageBytes) r.error={"input_excessive","Image exceeds resource limits",{}};
  return r;
 }
 if(data.hasText() && !data.text().isEmpty()) {
  r.payload.kind="text"; r.payload.mime="text/plain";
  if(data.text().size()>limits.clipboardTextBytes) {r.error={"input_excessive","Text exceeds resource limits",{}};return r;}
  r.payload.bytes=data.text().toUtf8();
  if(r.payload.bytes.size()>limits.clipboardTextBytes) r.error={"input_excessive","Text exceeds resource limits",{}};
  return r;
 }
 r.error={"source_unsupported","Clipboard is empty or unsupported",{}}; return r;
}
}
