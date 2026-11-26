#include "SealPage.h"
#include <QCryptographicHash>
namespace putgd::seal {
// Written by tools/seal/pin_viewer.py; a change to viewer.html changes it deliberately.
const char viewerSha256[]="d5d4a75407088bdc26cb764bdd48c283ba0673aaced941ef8d0bd0eb5bb908ae";
extern const unsigned char sealViewerBytes[];
extern const unsigned long long sealViewerSize;
namespace {
const QByteArray marker=QByteArrayLiteral("<!--putgd-sealed-->");
const QByteArray inlineCsp=QByteArrayLiteral("connect-src 'none'");
const QByteArray splitCsp=QByteArrayLiteral("connect-src 'self'");
Error changed() {return {"seal_page_changed","This page isn't put.gd's sealed-link viewer, or it was changed.",{}};}
bool base64Text(const QByteArray& text) {
 if(text.size()%4)return false;
 for(qsizetype i=0;i<text.size();++i){const char c=text[i];
  if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='+'||c=='/')continue;
  if(c=='='&&i>=text.size()-2&&(i==text.size()-1||text[text.size()-1]=='='))continue;
  return false;}
 return true;
}
}
QByteArray viewer() {return QByteArray(reinterpret_cast<const char*>(sealViewerBytes),qsizetype(sealViewerSize));}
bool viewerIntact() {
 const auto page=viewer();
 return QCryptographicHash::hash(page,QCryptographicHash::Sha256).toHex()==QByteArray(viewerSha256)&&page.count(marker)==1&&page.count(inlineCsp)==1&&!page.contains(splitCsp);
}
QByteArray pagePrefix(bool split) {
 auto head=viewer();head.truncate(head.indexOf(marker));
 if(split)head.replace(inlineCsp,splitCsp);
 return head;
}
QByteArray pageSuffix() {const auto page=viewer();return page.mid(page.indexOf(marker)+marker.size());}
bool validBinName(const QString& name) {
 if(!name.endsWith(".bin"))return false;
 const auto stem=name.chopped(4);if(stem.isEmpty()||stem.size()>128)return false;
 for(const QChar c:stem)if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return false;
 return true;
}
QByteArray dataOpen(bool split,const QString& bin) {
 return split?"<script id=\"putgd-sealed\" type=\"application/octet-stream\" data-form=\"split\" data-bin=\""+bin.toLatin1()+"\">"
             :QByteArrayLiteral("<script id=\"putgd-sealed\" type=\"application/octet-stream\" data-form=\"inline\">");
}
Error parsePage(const QByteArray& page,ParsedPage& parsed) {
 if(!viewerIntact())return changed();
 const auto suffix=pageSuffix();
 for(const bool split:{false,true}) {
  const auto prefix=pagePrefix(split);
  if(!page.startsWith(prefix)||!page.endsWith(suffix)||page.size()<prefix.size()+suffix.size())continue;
  const auto element=page.mid(prefix.size(),page.size()-prefix.size()-suffix.size());
  if(!element.endsWith(dataClose()))return changed();
  if(split) {
   static const QByteArray open="<script id=\"putgd-sealed\" type=\"application/octet-stream\" data-form=\"split\" data-bin=\"";
   if(!element.startsWith(open))return changed();
   const auto rest=element.mid(open.size());const auto end=rest.indexOf("\">");if(end<0)return changed();
   const auto bin=QString::fromLatin1(rest.left(end));
   if(!validBinName(bin)||rest.mid(end+2)!=dataClose())return changed();
   parsed={true,bin,{}};return {};
  }
  const auto open=dataOpen(false);
  if(!element.startsWith(open))return changed();
  const auto body=element.mid(open.size(),element.size()-open.size()-dataClose().size());
  if(!base64Text(body))return changed();
  parsed={false,{},body};return {};
 }
 return changed();
}
}
