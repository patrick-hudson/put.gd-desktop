#include "QuickRedact.h"
#include <QHash>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <functional>

namespace putgd::editor::quickRedact {
namespace {
// A word as matching sees it: NFKC, with every Unicode decimal digit as ASCII.
QString normalized(const QString& word) {
 auto text=word.normalized(QString::NormalizationForm_KC);
 for(auto& c:text)if(c.category()==QChar::Number_DecimalDigit&&c.unicode()>'9')c=QChar('0'+c.digitValue());
 return text;
}
bool hasDigit(const QString& text) {return std::any_of(text.cbegin(),text.cend(),[](QChar c){return c>='0'&&c<='9';});}
// Inside a number the shape fixes the alphabet to digits: O, l, I, S and B
// read as 0, 1, 1, 5 and 8 in a word that has a digit and no other letter.
QString digitView(const QString& word) {
 static const QHash<QChar,QChar> confused{{'O','0'},{'l','1'},{'I','1'},{'S','5'},{'B','8'}};
 if(!hasDigit(word))return word;
 for(const auto c:word)if(c.isLetter()&&!confused.contains(c))return word;
 QString out=word;for(auto& c:out)c=confused.value(c,c);return out;
}
// Inside an IPv6 address the alphabet is hex: O, l, I and S read as 0, 1, 1 and 5.
QString hexView(const QString& word) {
 static const QHash<QChar,QChar> confused{{'O','0'},{'l','1'},{'I','1'},{'S','5'}};
 if(!word.contains(':'))return word;
 for(const auto c:word)if(c.isLetter()&&!confused.contains(c)&&!QStringLiteral("abcdefABCDEF").contains(c))return word;
 QString out=word;for(auto& c:out)c=confused.value(c,c);return out;
}
// The candidates joined in reading order, with the word each character came
// from (-1 for a separator). Words are joined by one space; `wrapped` joins
// the last word of a line to the first of the next with none, for a token the
// line wrapped.
struct Joined {QString text;QList<int> owner;};
Joined join(const QList<QString>& words,const QList<RecognizedWord>& candidates,bool wrapped) {
 Joined joined;
 for(int i=0;i<words.size();++i){
  if(i>0&&!(wrapped&&candidates.at(i).line!=candidates.at(i-1).line)){joined.text+=' ';joined.owner.append(-1);}
  joined.text+=words.at(i);for(int k=0;k<words.at(i).size();++k)joined.owner.append(i);
 }
 return joined;
}
QList<int> owners(const Joined& joined,qsizetype start,qsizetype length) {
 QList<int> words;
 for(qsizetype i=start;i<start+length&&i<joined.owner.size();++i){const int word=joined.owner.at(i);if(word>=0&&(words.isEmpty()||words.last()!=word))words.append(word);}
 return words;
}
int digits(const QString& text) {return int(std::count_if(text.cbegin(),text.cend(),[](QChar c){return c>='0'&&c<='9';}));}
bool validIpv6(const QString& text) {
 if(text.count("::")>1||text.contains(":::"))return false;
 const bool compressed=text.contains("::");
 QStringList groups;for(const auto& part:text.split(':'))if(!part.isEmpty())groups.append(part);
 static const QRegularExpression hex("^[0-9A-Fa-f]{1,4}$");
 for(const auto& group:groups)if(!hex.match(group).hasMatch())return false;
 if(compressed?groups.size()>7:groups.size()!=8)return false;
 if(!compressed&&(text.startsWith(':')||text.endsWith(':')))return false;
 // Short forms like a::b are too common in code to call an address.
 return groups.size()>=3||(compressed&&std::any_of(groups.cbegin(),groups.cend(),[](const QString& g){return g.size()==4;}));
}
bool mixedRun(const QString& run) {
 // Random base64 of 32 or more characters has digits and both cases; a path,
 // a word or a long number in one case does not count. Hex counts as it is.
 static const QRegularExpression hex("^[0-9A-Fa-f]{32,}$");
 if(hex.match(run).hasMatch())return hasDigit(run)&&std::any_of(run.cbegin(),run.cend(),[](QChar c){return c.isLetter();});
 bool upper=false,lower=false;for(const auto c:run){upper=upper||c.isUpper();lower=lower||c.isLower();}
 return upper&&lower&&hasDigit(run);
}
struct Rule {QString category;QRegularExpression pattern;std::function<bool(const QString&)> valid;int view=0;};
// 0: as read; 1: digits; 2: hex.
const QList<Rule>& rules() {
 static const QList<Rule> list{
  {"key",QRegularExpression(R"((?<![A-Z0-9])(?:AKIA|ASIA|ABIA|ACCA|AGPA|AIDA|AIPA|ANPA|ANVA|APKA|AROA)[A-Z0-9]{16}(?![A-Z0-9]))"),{},0},
  {"key",QRegularExpression(R"((?<!\w)(?:gh[pousr]_[A-Za-z0-9]{36,255}|github_pat_[A-Za-z0-9_]{22,255})(?!\w))"),{},0},
  {"key",QRegularExpression(R"((?<![\w-])gl(?:pat|dt|rt|ptt|ft|cbt|imt|oas|soat|agent)-[A-Za-z0-9_-]{20,})"),{},0},
  {"key",QRegularExpression(R"((?<![\w-])(?:xox[abposr]-[A-Za-z0-9-]{10,}|xapp-\d-[A-Za-z0-9-]{10,}))"),{},0},
  {"key",QRegularExpression(R"((?<!\w)(?:(?:sk|pk|rk)_(?:live|test)_[A-Za-z0-9]{10,}|whsec_[A-Za-z0-9]{20,})(?!\w))"),{},0},
  {"key",QRegularExpression(R"((?<![\w-])eyJ[A-Za-z0-9_-]{4,}\.[A-Za-z0-9_-]{4,}\.[A-Za-z0-9_-]{4,}(?![\w-]))"),{},0},
  {"key",QRegularExpression(R"(-{2,} ?BEGIN [A-Z0-9 ]*PRIVATE KEY ?-{2,}(?: +[A-Za-z0-9+/=]{4,})*(?: +-{2,} ?END [A-Z0-9 ]*PRIVATE KEY ?-{2,})?)"),{},0},
  {"key",QRegularExpression(R"((?<![A-Za-z0-9+/=_-])[A-Za-z0-9+/_-]{32,}={0,2}(?![A-Za-z0-9+/=_-]))"),mixedRun,0},
  {"email",QRegularExpression(R"((?<![\w.%+-])[A-Za-z0-9][A-Za-z0-9._%+-]*@[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?)*\.[A-Za-z]{2,}(?![\w-]))"),{},0},
  {"card",QRegularExpression(R"((?<![\d-])\d(?:[ -]?\d){12,18}(?!\d))"),[](const QString& m){QString d;for(const auto c:m)if(c.isDigit())d+=c;return d.size()>=13&&d.size()<=19&&luhn(d);},1},
  {"ip",QRegularExpression(R"((?<![\d.])(?:(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)\.){3}(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)(?:/\d{1,2}|:\d{1,5})?(?!\d|\.\d))"),{},1},
  {"ip",QRegularExpression(R"((?<![\w:])(?:[0-9A-Fa-f]{0,4}:){2,7}[0-9A-Fa-f]{0,4}(?![\w:]))"),validIpv6,2},
  {"phone",QRegularExpression(R"((?<![\w+.-])(?:\+\d{1,3}[ .-]?)?(?:\(\d{1,4}\)[ .-]?)?\d{1,4}(?:[ .-]?\d{2,4}){1,5}(?![\w-]|\.\d))"),[](const QString& m){
    // At least 7 digits and at most 15 (E.164); a date or an IPv4 address is not a number to call.
    static const QRegularExpression date(R"(^(?:\d{4}[-./]\d{1,2}[-./]\d{1,2}|\d{1,2}[-./]\d{1,2}[-./]\d{2,4})$)"),dotted(R"(^\d{1,3}(?:\.\d{1,3}){3}$)");
    const int count=digits(m);return count>=7&&count<=15&&!date.match(m).hasMatch()&&!dotted.match(m).hasMatch();},1},
 };
 return list;
}
}
QStringList categories() {return {"email","phone","ip","card","key"};}
QStringList keyShapes() {return {"AWS access key IDs","GitHub tokens (ghp_, gho_, ghu_, ghs_, ghr_, github_pat_)","GitLab tokens (glpat- and the other gl prefixes)","Slack tokens (xox…, xapp-)","Stripe keys (sk_, pk_, rk_, whsec_)","JSON web tokens","PEM private-key blocks","Unbroken hex or base64 runs of 32 characters or more"};}
bool luhn(const QString& number) {
 int sum=0;bool twice=false;int count=0;
 for(auto it=number.crbegin();it!=number.crend();++it){
  if(!it->isDigit())continue;int d=it->digitValue();++count;
  if(twice){d*=2;if(d>9)d-=9;}sum+=d;twice=!twice;
 }
 return count>0&&sum%10==0;
}
QList<Match> find(const QList<RecognizedWord>& candidates,const QStringList& chosen) {
 QList<QString> plain,numbers,hex;
 for(const auto& word:candidates){const auto text=normalized(word.text);plain.append(text);numbers.append(digitView(text));hex.append(hexView(text));}
 const QList<QString>* views[3]={&plain,&numbers,&hex};
 QSet<int> taken;QList<Match> matches;
 // The rules run in order of specificity; a word belongs to the first match.
 for(const auto& rule:rules()){
  if(!chosen.contains(rule.category))continue;
  for(const bool wrapped:{false,true}){
   const auto joined=join(*views[rule.view],candidates,wrapped);
   for(auto it=rule.pattern.globalMatch(joined.text);it.hasNext();){
    const auto found=it.next();if(rule.valid&&!rule.valid(found.captured()))continue;
    const auto words=owners(joined,found.capturedStart(),found.capturedLength());
    if(words.isEmpty()||std::any_of(words.cbegin(),words.cend(),[&](int w){return taken.contains(w);}))continue;
    for(const int w:words)taken.insert(w);
    matches.append({rule.category,words});
   }
  }
 }
 std::sort(matches.begin(),matches.end(),[](const Match& a,const Match& b){return a.words.first()<b.words.first();});
 return matches;
}
QList<QRect> marks(const QList<RecognizedWord>& candidates,const Match& match,const QRect& bounds) {
 QMap<int,QRect> lines;
 for(const int index:match.words)if(index>=0&&index<candidates.size()){const auto& word=candidates.at(index);lines[word.line]=lines.value(word.line).united(word.rect);}
 QList<QRect> out;
 for(const auto& box:lines){const int pad=qMax(1,(box.height()+3)/4);const auto mark=box.adjusted(-pad,-pad,pad,pad).intersected(bounds);if(!mark.isEmpty())out.append(mark);}
 return out;
}
}
