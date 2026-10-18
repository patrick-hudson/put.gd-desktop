#pragma once
#include <QByteArray>
#include <QList>
#include <functional>
namespace putgd {
// X11 counterpart of kdeClipboardImportFormats, in the existing X11 text order.
// The discovery callback lists Qt image types, which instantiates every
// installed image plugin. Defer it for PNG and for offers whose other targets
// are recognized text/file-list formats or selection atoms such as TARGETS,
// TIMESTAMP, STRING or COMPOUND_TEXT; those have no '/' and are never MIME
// types. Unknown MIME names retain discovery, including aliases outside image/.
inline QList<QByteArray> x11ClipboardImportTargets(const QList<QByteArray>& offered,
                                                   const std::function<QList<QByteArray>()>& discover) {
 QList<QByteArray> targets;
 if(offered.contains("text/uri-list"))targets.append("text/uri-list");
 if(offered.contains("image/png")) {
  // Only a URI list with no local files advances to the next target, so
  // alternative image types after PNG were never imported.
  targets.append("image/png");
 } else {
  bool standardOnly=true;
  for(const auto& name:offered) {
   if(name.contains('/')&&name!="text/uri-list"&&name!="text/plain;charset=utf-8"&&name!="text/plain") {
    standardOnly=false;break;
   }
  }
  if(!standardOnly) {
   auto imageTypes=discover();imageTypes.removeAll("image/png");imageTypes.prepend("image/png");
   for(const auto& mime:imageTypes)if(offered.contains(mime))targets.append(mime);
  }
 }
 for(const char* name:{"UTF8_STRING","text/plain;charset=utf-8","text/plain"})
  if(offered.contains(name))targets.append(name);
 return targets;
}
}
