#pragma once
#include <QByteArray>
#include <QStringList>
#include <functional>
namespace putgd {
// The discovery callback instantiates Qt image plugins. Defer it for PNG and
// offers containing only already-recognized standard text/file-list formats.
// Unknown MIME names retain discovery, including plugin aliases outside image/.
inline QStringList kdeClipboardImportFormats(const QStringList& offered,
                                             const std::function<QList<QByteArray>()>& discover) {
 QStringList formats;
 if(offered.contains("text/uri-list"))formats.append("text/uri-list");
 if(offered.contains("image/png")) {
  // PNG is always the first image in the old ordering. Its decode/transfer
  // errors are terminal; only a URI list with no local files advances here.
  formats.append("image/png");
 } else {
  bool standardOnly=true;
  for(const auto& type:offered) {
   if(type!="text/uri-list"&&type!="text/plain;charset=utf-8"&&type!="text/plain"&&type!="UTF8_STRING") {
    standardOnly=false;break;
   }
  }
  if(!standardOnly) {
   auto imageTypes=discover();imageTypes.removeAll("image/png");imageTypes.prepend("image/png");
   for(const auto& mime:imageTypes)if(offered.contains(QString::fromLatin1(mime)))formats.append(QString::fromLatin1(mime));
  }
 }
 for(const char* mime:{"text/plain;charset=utf-8","text/plain","UTF8_STRING"})
  if(offered.contains(QString::fromLatin1(mime)))formats.append(QString::fromLatin1(mime));
 return formats;
}
}
