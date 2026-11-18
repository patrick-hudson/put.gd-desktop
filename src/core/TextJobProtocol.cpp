#include "TextJobProtocol.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QtEndian>
#include <algorithm>
#include <optional>
#include <functional>
#include <cstring>

namespace putgd {
QPoint TextCode::centre() const {
 if(quad.isEmpty())return {};
 qint64 x=0,y=0;for(const auto& point:quad){x+=point.x();y+=point.y();}
 return {int(x/quad.size()),int(y/quad.size())};
}
int TextResult::textCodes() const {return int(std::count_if(codes.begin(),codes.end(),[](const TextCode& code){return !code.binary&&!code.text.isEmpty();}));}
namespace textjob {
namespace {
QString decimal(qint64 value){return QString::number(value);}
// A canonical nonnegative decimal string, as the image worker's descriptors are.
bool number(const QJsonValue& value,qint64& out){bool ok=false;if(!value.isString())return false;out=value.toString().toLongLong(&ok);return ok&&out>=0&&QString::number(out)==value.toString();}
bool integer(const QJsonValue& value,qint64 minimum,qint64 maximum,qint64& out){if(!value.isDouble())return false;const double number=value.toDouble();out=value.toInteger();return number==double(out)&&out>=minimum&&out<=maximum;}
bool keys(const QJsonObject& object,const QStringList& names){if(object.size()!=names.size())return false;for(const auto& name:names)if(!object.contains(name))return false;return true;}
QJsonObject limits(){return {{"outputBytes",decimal(maximumOutputBytes)},{"textBytes",decimal(maximumTextBytes)},{"words",decimal(maximumWords)},{"wordBytes",decimal(maximumWordBytes)},{"codes",decimal(maximumCodes)},{"codeBytes",decimal(maximumCodeBytes)}};}
QJsonArray strings(const QStringList& values){QJsonArray array;for(const auto& value:values)array.append(value);return array;}
bool languageList(const QJsonValue& value,int minimum,QStringList& out){
 out.clear();if(!value.isArray()||value.toArray().size()<minimum||value.toArray().size()>maximumLanguages)return false;
 for(const auto& entry:value.toArray()){if(!entry.isString()||!validLanguage(entry.toString())||out.contains(entry.toString()))return false;out.append(entry.toString());}
 return true;
}
// The longest prefix of at most `bytes` UTF-8 bytes that ends on a code point.
QString cutUtf8(const QString& text,qint64 bytes,bool& cut){
 if(text.toUtf8().size()<=bytes)return text;
 cut=true;qsizetype low=0,high=text.size();
 while(low<high){const auto middle=(low+high+1)/2;if(text.left(middle).toUtf8().size()<=bytes)low=middle;else high=middle-1;}
 if(low>0&&text.at(low-1).isHighSurrogate())--low;
 return text.left(low);
}
QJsonObject wordJson(const TextWord& word){return {{"text",word.text},{"rect",QJsonArray{word.rect.x(),word.rect.y(),word.rect.width(),word.rect.height()}},{"confidence",word.confidence}};}
QJsonObject codeJson(const TextCode& code){
 QJsonArray quad;for(const auto& point:code.quad)quad.append(QJsonArray{point.x(),point.y()});
 QJsonObject object{{"format",code.format},{"kind",code.binary?"binary":"text"},{"length",double(code.length)},{"quad",quad}};
 if(!code.binary)object["text"]=code.text;
 return object;
}
QByteArray serialize(const TextResult& result){
 QJsonArray tops;for(const int top:result.lineTops)tops.append(top);
 return QJsonDocument(QJsonObject{{"text",result.text},{"lineTops",tops},{"words",wordsJson(result.words)},{"candidates",wordsJson(result.candidates)},{"codes",codesJson(result.codes)},
  {"truncated",result.truncated},{"banded",result.banded},{"languagesUsed",strings(result.languagesUsed)},{"languagesSkipped",strings(result.languagesSkipped)},
  {"timings",QJsonObject{{"engineStartMs",double(result.engineStartMs)},{"recognizeMs",double(result.recognizeMs)}}}}).toJson(QJsonDocument::Compact);
}
void boundWords(QList<TextWord>& words,bool& cut){
 if(words.size()>maximumWords){words.resize(maximumWords);cut=true;}
 for(auto& word:words)word.text=cutUtf8(word.text,maximumWordBytes,cut);
}
void trimLines(TextResult& result){const auto lines=result.text.isEmpty()?0:result.text.count('\n')+1;if(result.lineTops.size()>lines)result.lineTops.resize(lines);while(result.lineTops.size()<lines)result.lineTops.append(result.lineTops.isEmpty()?0:result.lineTops.last());}
bool decodeWords(const QJsonValue& value,bool wanted,const InputHeader& input,QList<TextWord>& out){
 if(!value.isArray()||value.toArray().size()>maximumWords||(!wanted&&!value.toArray().isEmpty()))return false;
 for(const auto& entry:value.toArray()){
  const auto object=entry.toObject();const auto rect=object["rect"].toArray();qint64 x=0,y=0,w=0,h=0,confidence=0;
  if(!entry.isObject()||!keys(object,{"text","rect","confidence"})||!object["text"].isString()||object["text"].toString().isEmpty()||object["text"].toString().toUtf8().size()>maximumWordBytes
     ||rect.size()!=4||!integer(rect[0],0,input.width-1,x)||!integer(rect[1],0,input.height-1,y)||!integer(rect[2],1,input.width-x,w)||!integer(rect[3],1,input.height-y,h)||!integer(object["confidence"],0,100,confidence))return false;
  const auto text=sanitizeText(object["text"].toString()).remove('\n');
  if(!text.isEmpty())out.append({text,QRect(int(x),int(y),int(w),int(h)),int(confidence)});
 }
 return true;
}
bool format(char32_t c){return QChar::category(c)==QChar::Other_Format;}
QString sanitize(const QString& input,bool joiners){
 QString out;out.reserve(input.size());
 const auto text=input.normalized(QString::NormalizationForm_C);
 for(qsizetype i=0;i<text.size();++i){
  char32_t c=text.at(i).unicode();
  if(text.at(i).isHighSurrogate()&&i+1<text.size()&&text.at(i+1).isLowSurrogate()){c=QChar::surrogateToUcs4(text.at(i),text.at(i+1));++i;}
  else if(text.at(i).isSurrogate())continue; // a lone surrogate is not text
  if(c==0x2028||c==0x2029){out.append('\n');continue;}
  if(c=='\t'||c=='\n'){out.append(QChar(char16_t(c)));continue;}
  // Other_Control is C0, DEL and C1; Other_Format holds the bidirectional
  // controls and the zero-width, invisible and tag characters.
  if(QChar::category(c)==QChar::Other_Control||c==0xFFFE||c==0xFFFF)continue;
  if(format(c)&&!(joiners&&(c==0x200C||c==0x200D)))continue;
  out.append(QChar::fromUcs4(c));
 }
 return out;
}
struct Tongue {const char* locale;const char* language;};
// ISO 639-1 to the Tesseract data file of that language.
constexpr Tongue tongues[]={{"af","afr"},{"ar","ara"},{"az","aze"},{"be","bel"},{"bg","bul"},{"bn","ben"},{"ca","cat"},{"cs","ces"},{"cy","cym"},{"da","dan"},{"de","deu"},{"el","ell"},{"en","eng"},{"es","spa"},{"et","est"},{"eu","eus"},{"fa","fas"},{"fi","fin"},{"fr","fra"},{"ga","gle"},{"gl","glg"},{"he","heb"},{"hi","hin"},{"hr","hrv"},{"hu","hun"},{"hy","hye"},{"id","ind"},{"is","isl"},{"it","ita"},{"ja","jpn"},{"ka","kat"},{"kk","kaz"},{"ko","kor"},{"lt","lit"},{"lv","lav"},{"mk","mkd"},{"ms","msa"},{"mt","mlt"},{"nb","nor"},{"nl","nld"},{"nn","nor"},{"no","nor"},{"pl","pol"},{"pt","por"},{"ro","ron"},{"ru","rus"},{"sk","slk"},{"sl","slv"},{"sq","sqi"},{"sr","srp"},{"sv","swe"},{"sw","swa"},{"ta","tam"},{"te","tel"},{"th","tha"},{"tr","tur"},{"uk","ukr"},{"ur","urd"},{"uz","uzb"},{"vi","vie"}};
}
QStringList redactCategories(){return {"email","phone","ip","card","key","name","list"};}
// #1618: Quick redact terms and patterns. A message never repeats the value,
// since a configuration error may be logged and these are the user's own words.
// The rules, in order, are the same in the editor and the MCP server (the
// shared table: tests/fixtures/text/redact-validation.json). Lengths count
// code points, and the line breaks are \n, \r, U+2028 and U+2029.
namespace {
bool lineBreak(QChar c) {return c=='\n'||c=='\r'||c==QChar(0x2028)||c==QChar(0x2029);}
qsizetype codePoints(const QString& text) {return QStringView(text).toUcs4().size();}
// The shape check: patterns that can run for a very long time, refused when
// saved. It reads a pattern that already compiled, so it never decides what is
// valid; where it cannot tell what an atom matches it assumes everything, which
// only ever refuses more.
//
// Refused: a back reference, a recursion or a subroutine call; and a group
// repeated without a bound ( *, +, {n,} ) that itself holds a repeat without a
// bound, the shape of (a+)+ and (\w+\s?)*. Allowed: such a group whose every
// alternative starts or ends with a required character that none of the inner
// repeats can match, since each pass then has one place to end: (?:\.\w+)+,
// (?:[a-z0-9-]+\.)+, (?:\w+\s)+. Possessive repeats and atomic groups never
// give back, so they do not count.
class Shape {
public:
 explicit Shape(const QString& pattern):p_(pattern.toUcs4()) {
  for(qsizetype i=0;i+2<p_.size();++i)if(p_[i]=='('&&p_[i+1]=='?'){
   // Inline options anywhere: case folding and extended mode, taken as global
   // (case-insensitive compares more, so it can only refuse more).
   qsizetype k=i+2;bool off=false;
   for(;k<p_.size()&&(QChar::isLetter(char32_t(p_[k]))||p_[k]=='-'||p_[k]=='^');++k){if(p_[k]=='-')off=true;if(!off&&p_[k]=='i')caseless_=true;if(!off&&p_[k]=='x')extended_=true;}
  }
  body();
 }
 bool backReference() const {return back_;}
 bool nestedRepeat() const {return nested_;}
private:
 using Test=std::function<bool(char32_t)>;
 struct Edge {bool set=false;Test test;};
 struct Body {bool unbounded=false;QList<Test> repeated,atoms;QList<std::pair<Edge,Edge>> alternatives;};
 struct Item {enum {Atom,Group,Assert} kind=Assert;Test test;Body inner;bool atomic=false;};
 struct Quantifier {bool present=false;qsizetype min=1,max=1;bool possessive=false;bool unbounded() const {return present&&max<0&&!possessive;}};
 QList<uint> p_;qsizetype i_=0;bool caseless_=false,extended_=false,back_=false,nested_=false;
 static Test all() {return [](char32_t){return true;};}
 Test literal(char32_t c) const {
  if(!caseless_)return [c](char32_t x){return x==c;};
  return [c](char32_t x){return x==c||QChar::toCaseFolded(x)==QChar::toCaseFolded(c);};
 }
 bool at(char32_t c,qsizetype k=0) const {return i_+k<p_.size()&&p_[i_+k]==c;}
 void skipSpace() {
  if(!extended_)return;
  while(i_<p_.size()){
   if(QChar::isSpace(char32_t(p_[i_]))){++i_;continue;}
   if(p_[i_]=='#'){i_=p_.size();return;} // patterns have no line breaks
   return;
  }
 }
 // What an escape outside a class matches, or none for an assertion.
 std::optional<Test> escape() {
  const char32_t c=char32_t(p_[i_++]);
  switch(c){
  case 'd':return Test([](char32_t x){return QChar::isDigit(x);});
  case 'D':return Test([](char32_t x){return !QChar::isDigit(x);});
  case 's':return Test([](char32_t x){return QChar::isSpace(x);});
  case 'S':return Test([](char32_t x){return !QChar::isSpace(x);});
  case 'w':return Test([](char32_t x){return QChar::isLetterOrNumber(x)||QChar::isMark(x)||x=='_';});
  case 'W':return Test([](char32_t x){return !(QChar::isLetterOrNumber(x)||QChar::isMark(x)||x=='_');});
  case 't':return literal('\t');case 'n':return literal('\n');case 'r':return literal('\r');case 'f':return literal('\f');case 'e':return literal(0x1b);case 'a':return literal(0x07);
  case 'b':case 'B':case 'A':case 'z':case 'Z':case 'G':case 'K':return std::nullopt;
  case 'k':back_=true;skipName();return all();
  case 'g':back_=true;skipName();return all();
  case 'x':{
   QString hex;
   if(at('{')){++i_;while(i_<p_.size()&&!at('}'))hex+=QChar(char16_t(p_[i_++]));++i_;}
   else for(int n=0;n<2&&i_<p_.size()&&QStringLiteral("0123456789abcdefABCDEF").contains(QChar(char16_t(p_[i_])));++n)hex+=QChar(char16_t(p_[i_++]));
   bool ok=false;const auto v=hex.toUInt(&ok,16);return ok?literal(char32_t(v)):all();
  }
  case 'p':case 'P':if(at('{')){while(i_<p_.size()&&!at('}'))++i_;++i_;}else ++i_;return all();
  case 'c':++i_;return all();
  case 'o':if(at('{')){while(i_<p_.size()&&!at('}'))++i_;++i_;}return all();
  case 'Q':return std::nullopt; // handled by the caller
  default:
   if(c>='1'&&c<='9'){back_=true;while(i_<p_.size()&&QChar::isDigit(char32_t(p_[i_])))++i_;return all();}
   if(c=='0'){while(i_<p_.size()&&p_[i_]>='0'&&p_[i_]<='7')++i_;return all();}
   if(QChar::isLetterOrNumber(c))return all(); // \h, \v, \R, \N, \X and the rest
   return literal(c);
  }
 }
 void skipName() {
  if(at('{')||at('<')||at('\'')){const char32_t close=at('{')?'}':at('<')?'>':'\'';++i_;while(i_<p_.size()&&!at(close))++i_;++i_;return;}
  if(at('-')||at('+'))++i_;
  while(i_<p_.size()&&QChar::isDigit(char32_t(p_[i_])))++i_;
 }
 // A character class, as a test; unknown parts match everything.
 Test bracket() {
  bool negated=false,unknown=false;QList<Test> parts;
  if(at('^')){negated=true;++i_;}
  bool first=true;
  while(i_<p_.size()&&(first||!at(']'))){
   first=false;
   if(at('[')&&at(':',1)){while(i_<p_.size()&&!(at(':')&&at(']',1)))++i_;i_+=2;unknown=true;continue;}
   char32_t low=0;bool single=false;
   if(at('\\')){
    ++i_;const char32_t c=char32_t(p_[i_]);
    if(c=='Q'||c=='E'){++i_;continue;}
    if(QChar::isLetterOrNumber(c)){
     // \d, \s, \w and their opposites, and \t, \n, \x41 and the like, are
     // known; the rest, or one that starts a range, matches everything.
     const bool known=QStringLiteral("dDsSwWtnrfeax").contains(QChar(char16_t(c)));
     const auto t=escape();if(t)parts.append(*t);
     if(!known||!t||(at('-')&&!at(']',1)))unknown=true;
     continue;
    }
    low=c;++i_;single=true;
   }else{low=char32_t(p_[i_++]);single=true;}
   if(single&&at('-')&&i_+1<p_.size()&&!at(']',1)){
    ++i_;char32_t high=char32_t(p_[i_]);
    if(high=='\\'){++i_;high=char32_t(p_[i_]);if(QChar::isLetterOrNumber(high)){unknown=true;++i_;continue;}}
    ++i_;
    const bool fold=caseless_;
    parts.append([low,high,fold](char32_t x){if(x>=low&&x<=high)return true;if(!fold)return false;const auto u=QChar::toUpper(x),l=QChar::toLower(x);return (u>=low&&u<=high)||(l>=low&&l<=high);});
   }else parts.append(literal(low));
  }
  ++i_;
  if(unknown)return all();
  Test any=[parts](char32_t x){for(const auto& t:parts)if(t(x))return true;return false;};
  if(negated)return [any](char32_t x){return !any(x);};
  return any;
 }
 Quantifier quantifier() {
  skipSpace();Quantifier q;
  if(at('*')){q={true,0,-1};++i_;}
  else if(at('+')){q={true,1,-1};++i_;}
  else if(at('?')){q={true,0,1};++i_;}
  else if(at('{')){
   qsizetype k=i_+1;QString min,max;bool comma=false;
   while(k<p_.size()&&(QChar::isDigit(char32_t(p_[k]))||p_[k]==','||p_[k]==' ')){if(p_[k]==',')comma=true;else if(p_[k]!=' ')(comma?max:min)+=QChar(char16_t(p_[k]));++k;}
   if(k<p_.size()&&p_[k]=='}'&&(!min.isEmpty()||!max.isEmpty())){
    q.present=true;q.min=min.isEmpty()?0:min.toLongLong();q.max=comma?(max.isEmpty()?-1:max.toLongLong()):q.min;i_=k+1;
   }
  }
  if(q.present){if(at('+')){q.possessive=true;++i_;}else if(at('?'))++i_;}
  return q;
 }
 // One item: an atom, a group or an assertion. False at the end of a sequence.
 bool item(Item& out) {
  skipSpace();
  if(i_>=p_.size()||at('|')||at(')'))return false;
  const char32_t c=char32_t(p_[i_]);
  out=Item{};
  if(c=='('){++i_;group(out);return true;}
  if(c=='['){++i_;out.kind=Item::Atom;out.test=bracket();return true;}
  if(c=='^'||c=='$'){++i_;return true;}
  if(c=='.'){++i_;out.kind=Item::Atom;out.test=all();return true;}
  if(c=='\\'){
   ++i_;
   if(at('Q')){
    // \Q...\E: each character a literal; the last one is the item.
    // \Q...\E: literal characters, as a group of one alternative from the
    // first to the last.
    ++i_;qsizetype end=i_;while(end<p_.size()&&!(p_[end]=='\\'&&end+1<p_.size()&&p_[end+1]=='E'))++end;
    if(end>i_){
     out.kind=Item::Group;
     for(qsizetype k=i_;k<end;++k)out.inner.atoms.append(literal(char32_t(p_[k])));
     out.inner.alternatives.append({Edge{true,literal(char32_t(p_[i_]))},Edge{true,literal(char32_t(p_[end-1]))}});
    }
    i_=qMin(p_.size(),end+2);return true;
   }
   if(at('E')){++i_;return true;}
   const auto t=escape();
   if(t){out.kind=Item::Atom;out.test=*t;}
   return true;
  }
  ++i_;out.kind=Item::Atom;out.test=literal(c);return true;
 }
 void group(Item& out) {
  out.kind=Item::Group;
  bool lookaround=false;
  if(at('*')){
   // (*VERB) or (*name: ...) groups.
   ++i_;QString name;while(i_<p_.size()&&(QChar::isLetter(char32_t(p_[i_]))||p_[i_]=='_'))name+=QChar(char16_t(p_[i_++]));
   static const QStringList groups{"pla","plb","nla","nlb","napla","naplb","positive_lookahead","negative_lookahead","positive_lookbehind","negative_lookbehind","non_atomic_positive_lookahead","non_atomic_positive_lookbehind","atomic","sr","asr","script_run","atomic_script_run"};
   if(at(':')&&groups.contains(name)){++i_;out.atomic=name.contains("atomic")||name=="asr";lookaround=name.contains('l')&&name!="atomic";}
   else{while(i_<p_.size()&&!at(')'))++i_;++i_;out.kind=Item::Assert;return;}
  }else if(at('?')){
   ++i_;
   if(at('#')){while(i_<p_.size()&&!at(')'))++i_;++i_;out.kind=Item::Assert;return;}
   if(at('R')||(i_<p_.size()&&QChar::isDigit(char32_t(p_[i_])))||at('&')||((at('+')||at('-'))&&i_+1<p_.size()&&QChar::isDigit(char32_t(p_[i_+1])))){back_=true;while(i_<p_.size()&&!at(')'))++i_;++i_;out.kind=Item::Atom;out.test=all();return;}
   if(at('P')&&(at('=',1)||at('>',1))){back_=true;while(i_<p_.size()&&!at(')'))++i_;++i_;out.kind=Item::Atom;out.test=all();return;}
   if(at('P')&&at('<',1)){i_+=2;while(i_<p_.size()&&!at('>'))++i_;++i_;}
   else if(at('<')&&(at('=',1)||at('!',1))){i_+=2;lookaround=true;}
   else if(at('<')||at('\'')){const char32_t close=at('<')?'>':'\'';++i_;while(i_<p_.size()&&!at(close))++i_;++i_;}
   else if(at('=')||at('!')){++i_;lookaround=true;}
   else if(at('>')){++i_;out.atomic=true;}
   else if(at(':')||at('|')){++i_;}
   else if(at('(')){
    // A condition: skip it (it only tests, it repeats nothing).
    ++i_;int depth=1;while(i_<p_.size()&&depth){if(at('('))++depth;else if(at(')'))--depth;++i_;}
   }else{
    // Options: (?i) applies to what follows, (?i:...) is a group.
    while(i_<p_.size()&&!at(')')&&!at(':'))++i_;
    if(at(')')){++i_;out.kind=Item::Assert;return;}
    ++i_;
   }
  }
  out.inner=body();
  if(at(')'))++i_;
  if(lookaround){out.kind=Item::Assert;}
 }
 // A sequence of alternatives up to the closing parenthesis.
 Body body() {
  Body b;
  while(true){
   Edge first,last;bool optionalBefore=false;bool seenRequired=false;
   Item it;
   while(item(it)){
    const auto q=quantifier();
    const bool required=!q.present||q.min>=1;
    Test edge;bool hasEdge=false;
    if(it.kind==Item::Atom){
     b.atoms.append(it.test);
     if(q.unbounded()){b.unbounded=true;b.repeated.append(it.test);}
     edge=it.test;hasEdge=true;
    }else if(it.kind==Item::Group){
     const auto& in=it.inner;
     if(!it.atomic){
      if(q.unbounded()){
       if(in.unbounded&&!passesEnd(in))nested_=true;
       b.unbounded=true;b.repeated+=in.atoms;
      }else{b.unbounded=b.unbounded||in.unbounded;b.repeated+=in.repeated;}
     }
     b.atoms+=in.atoms;
     // A group's edges are its alternatives' edges together, where every
     // alternative has one.
     bool firsts=!in.alternatives.isEmpty(),lasts=firsts;
     QList<Test> f,l;for(const auto& [a,z]:in.alternatives){firsts=firsts&&a.set;lasts=lasts&&z.set;if(a.set)f.append(a.test);if(z.set)l.append(z.test);}
     const auto any=[](QList<Test> tests){return Test([tests](char32_t x){for(const auto& t:tests)if(t(x))return true;return false;});};
     const Edge start=firsts?Edge{true,any(f)}:Edge{},end=lasts?Edge{true,any(l)}:Edge{};
     if(required){if(!seenRequired&&!optionalBefore)first=start;last=end;seenRequired=true;}
     else{if(!seenRequired)optionalBefore=true;last={};}
     continue;
    }else continue; // assertions are transparent
    if(hasEdge&&required){if(!seenRequired&&!optionalBefore)first={true,edge};last={true,edge};seenRequired=true;}
    else{if(!seenRequired)optionalBefore=true;last={};}
   }
   b.alternatives.append({first,last});
   if(at('|')){++i_;continue;}
   break;
  }
  return b;
 }
 // Whether every pass of a group has one place to end: each alternative starts
 // or ends with a required character none of the inner repeats can match.
 static bool disjoint(const Test& edge,const QList<Test>& repeated) {
  static const QList<char32_t> probes=[]{
   QList<char32_t> list{0x09,0x0a,0x0d,0x100,0x3b1,0x391,0x430,0x410,0x5d0,0x627,0x915,0x93f,0x4e00,0x3042,0xac00,0x2013,0x2014,0x2019,0x201c,0x20ac,0x1f600,0x1d538};
   for(char32_t c=0x20;c<0x7f;++c)list.append(c);
   for(char32_t c=0xa0;c<=0xff;++c)list.append(c);
   return list;}();
  for(const auto c:probes)if(edge(c))for(const auto& t:repeated)if(t(c))return false;
  return true;
 }
 static bool passesEnd(const Body& in) {
  for(const auto& [first,last]:in.alternatives)
   if(!((first.set&&disjoint(first.test,in.repeated))||(last.set&&disjoint(last.test,in.repeated))))return false;
  return true;
 }
};
// \C matches one UTF-16 unit, half a character outside the BMP.
bool halfCharacter(const QString& pattern) {
 for(qsizetype i=0;i+1<pattern.size();++i){
  if(pattern.at(i)!='\\')continue;
  if(pattern.at(i+1)=='Q'){const auto end=pattern.indexOf(QStringLiteral("\\E"),i+2);if(end<0)return false;i=end+1;continue;}
  if(pattern.at(i+1)=='C')return true;
  ++i;
 }
 return false;
}
}
QString redactTermError(const QString& term){
 if(std::any_of(term.cbegin(),term.cend(),lineBreak))return QStringLiteral("An entry has a line break");
 const auto t=term.trimmed();
 if(t.isEmpty())return QStringLiteral("An entry is empty");
 if(codePoints(t)>maxRedactTermLength)return QStringLiteral("An entry is longer than %1 characters").arg(maxRedactTermLength);
 // Matching splits an entry at its letters and digits; punctuation alone
 // would match nothing reliably and everything nearby.
 const auto points=QStringView(t).toUcs4();
 if(std::none_of(points.cbegin(),points.cend(),[](uint c){return QChar::isLetterOrNumber(char32_t(c));}))return QStringLiteral("An entry needs a letter or digit");
 return {};
}
QString redactPatternError(const QString& pattern){
 if(std::any_of(pattern.cbegin(),pattern.cend(),lineBreak))return QStringLiteral("A pattern has a line break");
 const auto t=pattern.trimmed();
 if(t.isEmpty())return QStringLiteral("A pattern is empty");
 if(codePoints(t)>maxRedactPatternLength)return QStringLiteral("A pattern is longer than %1 characters").arg(maxRedactPatternLength);
 // Quick redact puts its own start-of-pattern items first (match and depth
 // limits); one of the user's own could override them.
 if(t.startsWith(QStringLiteral("(*")))return QStringLiteral("Patterns can't set their own matching limits");
 if(halfCharacter(pattern))return QStringLiteral("Patterns can't match half a character");
 // Compiled as Quick redact runs it (src/editor/QuickRedact.cpp). PCRE2's
 // messages describe the syntax, not the text, and the offset counts from the
 // start of the pattern.
 const QRegularExpression compiled(pattern,QRegularExpression::UseUnicodePropertiesOption);
 if(!compiled.isValid())return QStringLiteral("This pattern does not compile: %1 at offset %2").arg(compiled.errorString()).arg(compiled.patternErrorOffset());
 if(compiled.match(QString()).hasMatch())return QStringLiteral("This pattern matches empty text");
 const Shape shape(pattern);
 if(shape.backReference())return QStringLiteral("This pattern refers back to an earlier group, which can run for a very long time. Simplify it.");
 if(shape.nestedRepeat())return QStringLiteral("This pattern can run for a very long time (a repeated group inside a repeat). Simplify it.");
 return {};
}
QStringList childErrorCodes(){return {"input_excessive","text_job_io","language_unavailable","engine_unavailable","helper_sandbox_unavailable"};}
QStringList parentErrorCodes(){return {"text_job_timeout","text_job_failed","text_incompatible","text_busy","text_helper_missing"};}
Error failure(const QString& code,const QString& language){
 if(code=="cancelled")return {code,"Text recognition cancelled",{}};
 if(code=="input_excessive")return {code,"The image is too large for text recognition",{}};
 if(code=="text_job_io")return {code,"Text recognition could not read the image or write its result",{}};
 if(code=="language_unavailable")return {code,"Text recognition data for this language is not installed",validLanguage(language)?QJsonObject{{"language",language}}:QJsonObject{}};
 if(code=="engine_unavailable")return {code,"Text recognition needs the Tesseract English data. Install tesseract-ocr-eng (Debian, Ubuntu, Mint), tesseract-langpack-eng (Fedora) or tesseract-data-eng (Arch)",{}};
 if(code=="helper_sandbox_unavailable")return {code,"Text recognition is unavailable: this system cannot confine the recognition helper",{}};
 if(code=="text_job_timeout")return {code,"Text recognition timed out",{}};
 if(code=="text_incompatible")return {code,"The text recognition helper does not match this build of put.gd",{}};
 if(code=="text_busy")return {code,busyNotification,{}};
 if(code=="text_helper_missing")return {code,"Text recognition needs the putgd-ocr helper. Reinstall put.gd",{}};
 return {"text_job_failed","Text recognition failed",{}};
}
QByteArray encodeInputHeader(const InputHeader& header){
 QByteArray bytes(inputHeaderBytes,'\0');std::memcpy(bytes.data(),inputMagic,4);
 const quint32 words[]={inputVersion,quint32(header.width),quint32(header.height),quint32(header.stride)};
 for(int i=0;i<4;++i)qToLittleEndian<quint32>(words[i],bytes.data()+4+4*i);
 return bytes;
}
bool admitted(int width,int height,int stride){return width>=1&&height>=1&&width<=maximumEdge&&height<=maximumEdge&&stride>=width&&stride<=maximumEdge+3&&qint64(width)*height<=maximumInputPixels;}
qint64 inputBytes(const InputHeader& header){return inputHeaderBytes+qint64(header.stride)*header.height;}
bool decodeInputHeader(const QByteArray& bytes,qint64 total,InputHeader& header){
 if(bytes.size()!=inputHeaderBytes||std::memcmp(bytes.constData(),inputMagic,4)!=0||qFromLittleEndian<quint32>(bytes.constData()+4)!=inputVersion)return false;
 for(qsizetype i=20;i<inputHeaderBytes;++i)if(bytes.at(i)!='\0')return false;
 const auto width=qFromLittleEndian<quint32>(bytes.constData()+8),height=qFromLittleEndian<quint32>(bytes.constData()+12),stride=qFromLittleEndian<quint32>(bytes.constData()+16);
 if(width>quint32(maximumEdge)||height>quint32(maximumEdge)||stride>quint32(maximumEdge+3)||!admitted(int(width),int(height),int(stride)))return false;
 header={int(width),int(height),int(stride)};return inputBytes(header)==total;
}
QJsonObject envelope(const QString& nonce){return {{"role",role},{"nonce",nonce},{"generation",generation}};}
bool validEnvelope(const QJsonObject& payload,const QString& nonce){return !nonce.isEmpty()&&payload["role"]==QLatin1String(role)&&payload["nonce"]==nonce&&payload["generation"]==QLatin1String(generation);}
QJsonObject bootstrap(const QString& nonce,const QJsonObject& build,const InputHeader& input,const Options& options){
 auto payload=envelope(nonce);payload["build"]=build;payload["task"]="recognize";
 payload["input"]=QJsonObject{{"bytes",decimal(inputBytes(input))},{"width",decimal(input.width)},{"height",decimal(input.height)},{"stride",decimal(input.stride)}};
 payload["options"]=QJsonObject{{"words",options.words},{"candidates",options.candidates},{"languages",strings(options.languages)},{"languagesRequired",options.languagesRequired}};
 payload["limits"]=limits();return payload;
}
bool decodeBootstrap(const QJsonObject& payload,const QJsonObject& build,QString& nonce,InputHeader& input,Options& options){
 const auto described=payload["input"].toObject(),wanted=payload["options"].toObject();qint64 bytes=0,width=0,height=0,stride=0;
 if(!keys(payload,{"role","nonce","generation","build","task","input","options","limits"})||!payload["nonce"].isString()||!validEnvelope(payload,payload["nonce"].toString())
    ||!payload["build"].isObject()||payload["build"].toObject()!=build||payload["task"]!=QLatin1String("recognize")||payload["limits"].toObject()!=limits()
    ||!keys(described,{"bytes","width","height","stride"})||!number(described["bytes"],bytes)||!number(described["width"],width)||!number(described["height"],height)||!number(described["stride"],stride)
    ||width>maximumEdge||height>maximumEdge||stride>maximumEdge+3||!admitted(int(width),int(height),int(stride))
    ||!keys(wanted,{"words","candidates","languages","languagesRequired"})||!wanted["words"].isBool()||!wanted["candidates"].isBool()||!wanted["languagesRequired"].isBool()||!languageList(wanted["languages"],1,options.languages))return false;
 input={int(width),int(height),int(stride)};if(inputBytes(input)!=bytes)return false;
 options.words=wanted["words"].toBool();options.candidates=wanted["candidates"].toBool();options.languagesRequired=wanted["languagesRequired"].toBool();nonce=payload["nonce"].toString();return true;
}
QJsonObject ready(const QString& nonce,const QJsonObject& build){auto payload=envelope(nonce);payload["build"]=build;return payload;}
bool validReady(const QJsonObject& payload,const QString& nonce,const QJsonObject& build){return payload.size()==4&&validEnvelope(payload,nonce)&&payload["build"].isObject()&&payload["build"].toObject()==build;}
QJsonObject checkpoint(const QString& nonce,const QByteArray& output){
 auto payload=envelope(nonce);payload["ok"]=true;payload["bytes"]=decimal(output.size());
 payload["sha256"]=QString::fromLatin1(QCryptographicHash::hash(output,QCryptographicHash::Sha256).toHex());return payload;
}
QJsonObject checkpointFailure(const QString& nonce,const QString& code,const QString& language){
 auto payload=envelope(nonce);payload["ok"]=false;payload["code"]=code;
 if(code=="language_unavailable"&&validLanguage(language))payload["language"]=language;
 return payload;
}
bool decodeCheckpoint(const QJsonObject& payload,const QString& nonce,Error& error,qint64& bytes,QString& sha256){
 error={};bytes=0;sha256.clear();
 if(!validEnvelope(payload,nonce)||!payload["ok"].isBool())return false;
 if(!payload["ok"].toBool()){
  const auto code=payload["code"].toString();const bool named=payload.contains("language");
  if(payload.size()!=(named?6:5)||!childErrorCodes().contains(code)||(named&&(code!="language_unavailable"||!payload["language"].isString()||!validLanguage(payload["language"].toString()))))return false;
  error=failure(code,payload["language"].toString());return true;
 }
 const auto hash=payload["sha256"].toString();
 if(payload.size()!=6||!number(payload["bytes"],bytes)||bytes<2||bytes>maximumOutputBytes||hash.size()!=64||QByteArray::fromHex(hash.toLatin1()).toHex()!=hash.toLatin1())return false;
 sha256=hash;return true;
}
QJsonArray wordsJson(const QList<TextWord>& words){QJsonArray array;for(const auto& word:words)array.append(wordJson(word));return array;}
QJsonArray codesJson(const QList<TextCode>& codes){QJsonArray array;for(const auto& code:codes)array.append(codeJson(code));return array;}
QStringList codeFormats(){return {"QRCode","MicroQRCode","DataMatrix","Aztec"};}
QByteArray encodeResult(TextResult result,const Options& options){
 bool cut=result.truncated;
 if(!options.words)result.words.clear();
 if(!options.candidates)result.candidates.clear();
 result.text=cutUtf8(result.text,maximumTextBytes,cut);trimLines(result);
 boundWords(result.words,cut);boundWords(result.candidates,cut);
 if(result.codes.size()>maximumCodes){result.codes.resize(maximumCodes);cut=true;}
 for(auto& code:result.codes)if(!code.binary)code.text=cutUtf8(code.text,maximumCodeBytes,cut);
 result.truncated=cut;auto bytes=serialize(result);
 // The output cap governs: candidates go first, then words, then codes, then text.
 while(bytes.size()>maximumOutputBytes){
  result.truncated=true;
  if(!result.candidates.isEmpty())result.candidates.resize(result.candidates.size()/2);
  else if(!result.words.isEmpty())result.words.resize(result.words.size()/2);
  else if(!result.codes.isEmpty())result.codes.resize(result.codes.size()/2);
  else{bool unused=false;result.text=cutUtf8(result.text,result.text.toUtf8().size()/2,unused);trimLines(result);}
  bytes=serialize(result);
 }
 return bytes;
}
bool decodeResult(const QByteArray& json,const InputHeader& input,const Options& options,TextResult& result){
 result={};if(json.size()>maximumOutputBytes)return false;
 QJsonParseError parse;const auto document=QJsonDocument::fromJson(json,&parse);const auto object=document.object();
 if(parse.error!=QJsonParseError::NoError||!document.isObject()||!keys(object,{"text","lineTops","words","candidates","codes","truncated","banded","languagesUsed","languagesSkipped","timings"})
    ||!object["text"].isString()||object["text"].toString().toUtf8().size()>maximumTextBytes||!object["lineTops"].isArray()||!object["truncated"].isBool()||!object["banded"].isBool()
    ||!languageList(object["languagesUsed"],0,result.languagesUsed)||!languageList(object["languagesSkipped"],0,result.languagesSkipped)||(options.languagesRequired&&!result.languagesSkipped.isEmpty()))return false;
 const auto timings=object["timings"].toObject();
 if(!keys(timings,{"engineStartMs","recognizeMs"})||!integer(timings["engineStartMs"],0,3600000,result.engineStartMs)||!integer(timings["recognizeMs"],0,3600000,result.recognizeMs))return false;
 const auto raw=object["text"].toString();const auto lines=raw.isEmpty()?QStringList{}:raw.split('\n');const auto tops=object["lineTops"].toArray();
 if(tops.size()!=lines.size())return false;
 QStringList kept;
 for(qsizetype i=0;i<lines.size();++i){
  qint64 top=0;if(!integer(tops[i],0,input.height,top))return false;
  // A line separator inside a line becomes lines of its own, at the same height.
  for(const auto& line:sanitizeText(lines[i]).split('\n')){kept.append(line);result.lineTops.append(int(top));}
 }
 result.text=kept.join('\n');
 if(!decodeWords(object["words"],options.words,input,result.words)||!decodeWords(object["candidates"],options.candidates,input,result.candidates))return false;
 if(!object["codes"].isArray()||object["codes"].toArray().size()>maximumCodes)return false;
 for(const auto& entry:object["codes"].toArray()){
  const auto code=entry.toObject();const bool binary=code["kind"]==QLatin1String("binary");const auto quad=code["quad"].toArray();TextCode decoded;
  if(!entry.isObject()||!keys(code,binary?QStringList{"format","kind","length","quad"}:QStringList{"format","kind","length","quad","text"})||!codeFormats().contains(code["format"].toString())
     ||(!binary&&(code["kind"]!=QLatin1String("text")||!code["text"].isString()||code["text"].toString().toUtf8().size()>maximumCodeBytes))||!integer(code["length"],0,65536,decoded.length)||quad.size()!=4)return false;
  for(const auto& corner:quad){qint64 x=0,y=0;const auto point=corner.toArray();if(point.size()!=2||!integer(point[0],0,input.width,x)||!integer(point[1],0,input.height,y))return false;decoded.quad.append({int(x),int(y)});}
  decoded.format=code["format"].toString();decoded.binary=binary;if(!binary)decoded.text=sanitizeCodeContent(code["text"].toString());
  result.codes.append(decoded);
 }
 result.truncated=object["truncated"].toBool();result.banded=object["banded"].toBool();return true;
}
QString sanitizeText(const QString& text){return sanitize(text,true);}
QString sanitizeCodeContent(const QString& text){return sanitize(text,false);}
QString copyText(const TextResult& result){
 const auto lines=result.text.isEmpty()?QStringList{}:result.text.split('\n');QList<QStringList> before(lines.size()+1);
 for(const auto& code:result.codes){
  if(code.binary||code.text.isEmpty())continue;
  qsizetype line=0;while(line<lines.size()&&result.lineTops.value(line)<=code.centre().y())++line;
  before[line].append(code.text);
 }
 QStringList out;for(qsizetype i=0;i<=lines.size();++i){out.append(before[i]);if(i<lines.size())out.append(lines[i]);}
 return out.join('\n');
}
QString notification(const TextResult& result,const QString& copyStatus,const QStringList& missing){
 const auto plural=[](qsizetype count,const QString& noun){return QStringLiteral("%1 %2%3").arg(count).arg(noun,count==1?QString():QStringLiteral("s"));};
 const auto characters=result.text.toUcs4().size();const int codes=result.textCodes();QString message;
 if(!characters&&!codes)message=QStringLiteral("No text found");
 else if(copyStatus=="suppressed")return QStringLiteral("Text not copied: the clipboard changed");
 else if(copyStatus!="copied")return QStringLiteral("Text could not be copied");
 else if(characters&&codes)message=QStringLiteral("Copied %1 and %2").arg(plural(characters,"character"),plural(codes,"code"));
 else message=QStringLiteral("Copied %1").arg(characters?plural(characters,"character"):plural(codes,"code"));
 if(!missing.isEmpty())message+=missing.size()==1?QStringLiteral(" (%1 data is missing)").arg(missing.first()):QStringLiteral(" (%1 and %2 data are missing)").arg(missing.mid(0,missing.size()-1).join(", "),missing.last());
 if(result.truncated)message+=QStringLiteral("\nThe text was cut at a size or time limit.");
 return message;
}
bool validLanguage(const QString& identifier){
 // \z, not $: a trailing newline is not part of an identifier.
 static const QRegularExpression pattern(QStringLiteral("\\A[a-z]{3,4}(_[a-z0-9]+){0,2}\\z"));
 return identifier.size()<=32&&identifier!="osd"&&identifier!="equ"&&pattern.match(identifier).hasMatch();
}
bool validLanguages(const QStringList& identifiers){
 if(identifiers.isEmpty()||identifiers.size()>maximumLanguages)return false;
 for(qsizetype i=0;i<identifiers.size();++i)if(!validLanguage(identifiers[i])||identifiers.indexOf(identifiers[i])!=i)return false;
 return true;
}
QList<DataDirectory> dataDirectories(const QString& home,const QString& applicationDirectory){
 QList<DataDirectory> directories;
 if(QDir::isAbsolutePath(home))directories.append({QDir::cleanPath(home+"/.local/share/putgd/tessdata"),"user"});
 // Beside the executables, where the AppImage carries its English data.
 if(QDir::isAbsolutePath(applicationDirectory))directories.append({QDir::cleanPath(applicationDirectory+"/../share/putgd/tessdata"),"packaged"});
 // Debian, Ubuntu and Mint; Fedora; Arch.
 for(const auto* path:{"/usr/share/tesseract-ocr/5/tessdata","/usr/share/tesseract/tessdata","/usr/share/tessdata"})directories.append({QString::fromLatin1(path),"system"});
 return directories;
}
QList<Language> installedLanguages(const QList<DataDirectory>& directories){
 QList<Language> languages;QStringList seen;
 for(const auto& directory:directories){
  const auto files=QDir(directory.path).entryInfoList({"*.traineddata"},QDir::Files|QDir::Readable,QDir::Name);
  for(const auto& file:files){
   const auto id=file.completeBaseName();
   if(languages.size()>=1024||!validLanguage(id)||seen.contains(id)||file.size()<1)continue;
   seen.append(id);languages.append({id,directory.kind,file.absoluteFilePath(),file.size()});
  }
 }
 std::sort(languages.begin(),languages.end(),[](const Language& a,const Language& b){return a.id<b.id;});
 return languages;
}
QString languageForLocale(const QString& locale){
 const auto name=locale.section('.',0,0).section('@',0,0);const auto language=name.section('_',0,0).toLower(),territory=name.section('_',1,1).toUpper();
 if(language=="zh")return territory=="TW"||territory=="HK"||territory=="MO"?QStringLiteral("chi_tra"):QStringLiteral("chi_sim");
 for(const auto& tongue:tongues)if(language==QLatin1String(tongue.locale))return QString::fromLatin1(tongue.language);
 return {};
}
QStringList defaultLanguages(const QStringList& preference,const QString& locale,const QList<Language>& installed){
 if(validLanguages(preference))return preference;
 QStringList languages;const auto local=languageForLocale(locale);
 if(!local.isEmpty()&&std::any_of(installed.begin(),installed.end(),[&](const Language& language){return language.id==local;}))languages.append(local);
 if(!languages.contains("eng"))languages.append("eng");
 return languages;
}
}
}
