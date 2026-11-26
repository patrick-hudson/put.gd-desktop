#include "EncryptPage.h"
#include <QCryptographicHash>
namespace putgd::encrypt {
// Written by tools/encrypt/pin_viewer.py; a change to viewer.html changes it deliberately.
const char viewerSha256[]="3f271e733ddbb3ab7921d52a7dc8256a8eb6fdbb7a61409371a4961ef6f3d042";
extern const unsigned char encryptViewerBytes[];
extern const unsigned long long encryptViewerSize;
namespace {
// v1 format identifiers; they keep the feature's original name so existing vectors and links stay valid. The data element's id, putgd-sealed, is one of them.
const QByteArray marker=QByteArrayLiteral("<!--putgd-sealed-->");
const QByteArray inlineCsp=QByteArrayLiteral("connect-src 'none'");
const QByteArray splitCsp=QByteArrayLiteral("connect-src 'self'");
Error changed() {return {"encrypt_page_changed","This page isn't put.gd's encrypted-link viewer, or it was changed.",{}};}
bool base64Text(const QByteArray& text) {
 if(text.size()%4)return false;
 for(qsizetype i=0;i<text.size();++i){const char c=text[i];
  if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='+'||c=='/')continue;
  if(c=='='&&i>=text.size()-2&&(i==text.size()-1||text[text.size()-1]=='='))continue;
  return false;}
 return true;
}
}
QByteArray viewer() {return QByteArray(reinterpret_cast<const char*>(encryptViewerBytes),qsizetype(encryptViewerSize));}
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
QByteArray probePage() {
 static const QByteArray container=QByteArrayLiteral("UEdEU0VBTDEBAAAAABAAAAAAAAADa1R8loijOZDLopPqh3yF/iPaaYootZrv7DwsRokmQu+87nchyoEAAAAAAJnbfqrab+ZMLDuTno2a6/GmFds8T4HIhSKB9qlyVw85lZVDKp4CAkBDDzKkU2rir3YgnrQKW6hWxG0DrGZ9Z2AaKJtAHQgAFzvzWsFAoelvXhrP7xA5LHkYoGnN5FskiRVPGe20yQB8oR92ecQiaVO+dEDa9ZBpptDdmrKuPd7g1PzqfA==");
 return pagePrefix(false)+dataOpen(false)+container+dataClose()+pageSuffix();
}
QString probeFragment() {return QStringLiteral("v1.")+QString::fromLatin1(QByteArray::fromHex("b88f6e3d577beb7ae6550951a8c4970711c41bc0f48cbc52ada4f44856b24aa9").toBase64(QByteArray::Base64UrlEncoding|QByteArray::OmitTrailingEquals));}
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
