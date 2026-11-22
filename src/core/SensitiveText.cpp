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
 }
 if(parts.size()<2)return parts.join(QString());
 const auto last=parts.takeLast();return parts.join(", ")+" and "+last;
}
QString coveredSentence(const QJsonObject& covered){
 // In the editor's order, whatever order the counts arrive in.
 QList<QPair<QString,int>> found;
 for(const auto* category:{"email","phone","ip","card","key","name","list"})if(covered[category].toInt()>0)found.append({QString::fromLatin1(category),covered[category].toInt()});
 const auto text=describeSensitive(found,true);
 return text.isEmpty()?QString():QStringLiteral("Covered %1.").arg(text);
}
}
