#include "SensitiveText.h"
#include <QStringList>

namespace putgd {
QString describeSensitive(const QList<QPair<QString,int>>& found,bool counted){
 QStringList parts;
 for(const auto& [category,count]:found){
  if(count<1)continue;
  const bool one=count==1;const auto n=QString::number(count);
  const auto a=[&](const char* article){return counted?n+" ":QString::fromLatin1(article);};
  if(category=="card")parts<<(one?a("a ")+"card number":n+" card numbers");
  else if(category=="key")parts<<(one?a("a ")+"key or token":n+" keys or tokens");
  else if(category=="email")parts<<(one?a("an ")+"email address":n+" email addresses");
  else if(category=="phone")parts<<(one?a("a ")+"phone number":n+" phone numbers");
  else if(category=="ip")parts<<(one?a("an ")+"IP address":n+" IP addresses");
  else if(category=="name")parts<<QStringLiteral("your username or name");
  else if(category=="list")parts<<(one?a("an ")+"entry from your redaction list":n+" entries from your redaction list");
  else if(category=="qr_otp")parts<<(one?a("a ")+"2FA QR code":n+" 2FA QR codes");
  else if(category=="qr_wifi")parts<<(one?a("a ")+"Wi-Fi QR code":n+" Wi-Fi QR codes");
  else if(category=="qr_key")parts<<(one?a("a ")+"QR code with a key":n+" QR codes with keys");
 }
 if(parts.size()<2)return parts.join(QString());
 const auto last=parts.takeLast();return parts.join(", ")+" and "+last;
}
QList<QPair<QString,int>> withCodes(QList<QPair<QString,int>> found,const QJsonObject& codes){
 int total=0;for(const auto* kind:{"otp","wifi","key"})total+=qMax(0,codes[kind].toInt());
 if(total<1)return found;
 for(auto& [category,count]:found)if(category=="key"&&count>=total){
  count-=total;
  for(const auto* kind:{"otp","wifi","key"})if(codes[kind].toInt()>0)found.append({QStringLiteral("qr_")+QLatin1String(kind),codes[kind].toInt()});
  break;
 }
 return found;
}
QString coveredSentence(const QJsonObject& covered){
 // In the editor's order, whatever order the counts arrive in.
 QList<QPair<QString,int>> found;
 for(const auto* category:{"email","phone","ip","card","key","name","list"})if(covered[category].toInt()>0)found.append({QString::fromLatin1(category),covered[category].toInt()});
 const auto text=describeSensitive(found,true);
 return text.isEmpty()?QString():QStringLiteral("Covered %1.").arg(text);
}
}
