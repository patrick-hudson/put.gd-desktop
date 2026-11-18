#include "QuickRedact.h"
#include <QElapsedTimer>
#include <QChar>
#include <QHash>
#include <QMap>
#include <QPair>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <functional>
#include <cmath>
#include <iterator>
#include <numeric>

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
 QString out=word;for(auto& c:out)c=confused.value(c,c);
 // A colon of "::" is read as a dash or a dot beside the other one; a stray
 // one beside a whole "::" is dropped rather than making ":::".
 for(qsizetype i=0;i<out.size();++i)if((out[i]=='-'||out[i]=='.')&&((i>0&&out[i-1]==':')||(i+1<out.size()&&out[i+1]==':'))){
  const bool tripled=(i>1&&out[i-1]==':'&&out[i-2]==':')||(i+2<out.size()&&out[i+1]==':'&&out[i+2]==':')||(i>0&&i+1<out.size()&&out[i-1]==':'&&out[i+1]==':');
  if(tripled){out.remove(i,1);--i;}else out[i]=':';
 }
 return out;
}
// An email whose last dot was lost: the word with "@" and no dot after it,
// followed on its line by a word of letters, reads as one address.
QList<QString> emailView(const QList<QString>& plain,const QList<RecognizedWord>& candidates) {
 static const QRegularExpression letters(QStringLiteral("^[A-Za-z]{2,24}$"));
 QList<QString> out=plain;
 for(qsizetype i=0;i+1<plain.size();++i){
  const auto at=plain.at(i).indexOf('@');
  if(at>0&&!plain.at(i).mid(at).contains('.')&&candidates.at(i+1).line==candidates.at(i).line&&letters.match(plain.at(i+1)).hasMatch())out[i]+='.';
 }
 return out;
}
// OCR supplies line identities and word rectangles, not block identities. Treat
// aligned, vertically adjacent lines as one block; a paragraph gap starts another.
struct Line {QRect box;int characters=0;int width=0;int block=0;};
QSet<int> wrappingLines(const QList<RecognizedWord>& candidates) {
 QMap<int,Line> lines;
 for(const auto& word:candidates){auto& line=lines[word.line];line.box=line.box.united(word.rect);line.characters+=word.text.size();line.width+=word.rect.width();}
 int block=0;const Line* previous=nullptr;
 for(auto& line:lines){
  const auto average=double(line.width)/qMax(1,line.characters);
  if(previous&&(line.box.top()<=previous->box.top()||line.box.top()-previous->box.top()>2*qMax(line.box.height(),previous->box.height())||std::abs(line.box.left()-previous->box.left())>1.5*average))++block;
  line.block=block;previous=&line;
 }
 QMap<int,Line> blocks;
 for(const auto& line:std::as_const(lines)){auto& group=blocks[line.block];group.box=group.box.united(line.box);group.characters+=line.characters;group.width+=line.width;}
 QMap<int,int> peers;
 for(const auto& line:std::as_const(lines)){const auto& group=blocks[line.block];const double character=double(group.width)/qMax(1,group.characters);if(group.box.right()-line.box.right()<=0.5*character)++peers[line.block];}
 QSet<int> full;
 for(auto it=lines.cbegin();it!=lines.cend();++it){
  const auto& line=it.value();const auto& group=blocks[line.block];const double character=double(group.width)/qMax(1,group.characters);
  const auto next=std::next(it);if(next==lines.cend()||next->block!=line.block)continue;
  const int otherPeers=peers[line.block]-(group.box.right()-line.box.right()<=0.5*character?1:0);
  // A candidate may fall 1.5 characters short. Supporting lines must actually
  // reach the column (within half a character), so short prose is not evidence.
  if(otherPeers>=2&&group.box.right()-line.box.right()<=1.5*character)full.insert(it.key());
 }
 return full;
}
bool wrapEvidence(const QString& left,const QString& right,const QString& category,bool full) {
 if(left.isEmpty()||right.isEmpty())return false;
 if(full)return true;
 // Name and list terms (#1618) join across a line that ends or starts with a
 // character that joins their parts.
 const QString punctuation=category=="email"?QStringLiteral("@.-_+"):category=="ip"?QStringLiteral(".:"):category=="phone"||category=="card"?QStringLiteral(" .-()"):category=="name"||category=="list"?QStringLiteral("-_."):QString();
 if(punctuation.contains(left.back())||punctuation.contains(right.front()))return true;
 if(category!="key")return false;
 static const QRegularExpression alphabet(QStringLiteral("^[A-Za-z0-9+/_=.-]+$"));
 return alphabet.match(left).hasMatch()&&alphabet.match(right).hasMatch();
}
// Every character keeps its word owner. Both views retain an unsupported
// line boundary; only the wrapped view removes a supported boundary entirely.
struct Joined {QString text;QList<int> owner;};
Joined join(const QList<QString>& words,const QList<RecognizedWord>& candidates,bool wrapped,const QString& category,const QSet<int>& full) {
 Joined joined;
 for(int i=0;i<words.size();++i){
  const bool boundary=i>0&&candidates.at(i).line!=candidates.at(i-1).line;
  const bool evidence=boundary&&wrapEvidence(words.at(i-1),words.at(i),category,full.contains(candidates.at(i-1).line));
  const bool wrap=wrapped&&evidence;
  // An IPv6 address read as two words split at a colon is one address.
  static const QRegularExpression fragment(QStringLiteral("^[0-9A-Fa-f:]+$"));
  const bool colon=i>0&&!boundary&&category=="ip"&&(words.at(i-1).endsWith(':')||words.at(i).startsWith(':'))&&fragment.match(words.at(i-1)).hasMatch()&&fragment.match(words.at(i)).hasMatch();
  // The email view restored a lost dot after the domain: its last label follows at once.
  const bool dotted=i>0&&!boundary&&category=="email"&&words.at(i-1).contains('@')&&words.at(i-1).endsWith('.');
  if(i>0&&!wrap&&!colon&&!dotted){joined.text+=boundary&&!evidence?'\n':' ';joined.owner.append(-1);}
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
// Keys and tokens are matched line by line, as recognition really returns
// them: a long run is often split into several words with erratic boxes and
// gaps no wider than a word space, a dot or underscore may be lost or read as
// another sign, and letters are confused (l and 1, O and 0, a and g). So the
// words of one line that are made of the token alphabet (allowing a little
// recognition noise) are joined without spaces into segments, and a segment
// is marked when it carries a known prefix, starts a JSON web token, or has
// the shape of a hex or base64 run (see base64Run for what keeps ordinary
// interface text out). A PEM block is found by its BEGIN and END lines.
bool tokenCharacter(QChar c) {return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||QStringLiteral("+/=_-.:,").contains(c);}
// A word that can be part of a token: the token alphabet, with at most one
// misread character (a bracket, an accented letter, a currency sign) in eight,
// and always one.
bool tokenPiece(const QString& word) {
 if(word.isEmpty())return false;
 int noise=0;
 for(const auto c:word)if(!tokenCharacter(c)){if(c.isSpace())return false;++noise;}
 return noise<=qMax(1,int(word.size())/8);
}
// Hex, as recognition misreads it: l, i, I, |, ! for 1; O, o, ©, ° for 0; S
// for 5; t and £ for f; € for e. At least 32 characters, at most one in twelve
// outside hex once mapped, and both digits and letters.
bool hexRun(const QString& run) {
 static const QHash<QChar,QChar> confused{{'l','1'},{'i','1'},{'I','1'},{'|','1'},{'!','1'},{'O','0'},{'o','0'},{QChar(0xa9),'0'},{QChar(0xb0),'0'},{'S','5'},{'t','f'},{QChar(0xa3),'f'},{QChar(0x20ac),'e'}};
 if(run.size()<32)return false;
 int other=0,digits=0,letters=0;
 for(const auto raw:run){const auto c=confused.value(raw,raw);if(c>='0'&&c<='9')++digits;else if((c>='a'&&c<='f')||(c>='A'&&c<='F'))++letters;else ++other;}
 return other*12<=run.size()&&digits*5>=run.size()&&letters*10>=run.size();
}
// Base64 or base64url of random bytes: upper and lower case and digits, and a
// change of character class at least every third character on average.
// A token whose recognition collapsed into a run of two or three repeated
// letters: on the final candidate, with the page cut into eight strips, a
// GitHub token of 36 identical letters came back as "oho aeaaaaaaeae aad …"
// and "APC CCEEEECEEEEC…", the prefix gone. A masked or placeholder secret
// ("xxxxxxxxxxxxxxxx") has the same shape. At least 24 letters or digits, the
// three commonest characters (ignoring case) at least 85 % of them and the
// commonest alone half; prose never repeats so few letters for so long, and
// rules of dashes or dots are not letters.
bool repeatedRun(const QString& run) {
 QHash<QChar,int> counts;int characters=0;
 for(const auto c:run)if(c.isLetterOrNumber()){++counts[c.toLower()];++characters;}
 if(characters<24||characters*10<int(run.size())*9)return false;
 QList<int> sizes=counts.values();std::sort(sizes.begin(),sizes.end(),std::greater<int>());
 int top=0;for(int i=0;i<qMin(3,int(sizes.size()));++i)top+=sizes.at(i);
 return top*100>=characters*85&&sizes.first()*2>=characters;
}
bool base64Run(const QString& run) {
 if(run.size()<32)return false;
 int noise=0,changes=0,previous=-1;bool upper=false,lower=false,digit=false;
 for(const auto c:run){
  const int kind=c.isUpper()?0:c.isLower()?1:c.isDigit()?2:3;
  if(!tokenCharacter(c))++noise;
  upper=upper||kind==0;lower=lower||kind==1;digit=digit||kind==2;
  if(previous>=0&&kind!=previous)++changes;previous=kind;
 }
 // Not interface text run together, measured on recognized tokens and on
 // ordinary lines (#1428): random base64 is about 40 % capitals and 16 %
 // digits, base64 of text about 30 % capitals; real token runs kept at least
 // 25 % capitals, at most 31 % digits, under 30 % of their letters in lowercase
 // runs of five or more, and no dot, colon or comma. Short capitalized words
 // with numbers ("Team Alpha 3, Team Bravo 2") had at most 15 % capitals, a
 // hardware line ("2.6GHz 6-Core 32GB") 44 % digits and dots, prose 35 % and
 // more of its letters in long lowercase runs. A dot is allowed once in 80
 // characters, a dash once in 24.
 static const QRegularExpression words(QStringLiteral("[a-z]{5,}"));
 qsizetype lowercase=0;for(auto it=words.globalMatch(run);it.hasNext();)lowercase+=it.next().capturedLength();
 const auto count=[&](auto test){return qsizetype(std::count_if(run.cbegin(),run.cend(),test));};
 const auto capitals=count([](QChar c){return c.isUpper();}),numerals=count([](QChar c){return c.isDigit();});
 const auto stops=count([](QChar c){return c=='.'||c==':'||c==',';}),dashes=count([](QChar c){return c=='-';});
 return noise*10<=run.size()&&upper&&lower&&digit&&changes*3>=run.size()&&lowercase*10<=run.size()*3
  &&capitals*5>=run.size()&&numerals*5<=run.size()*2&&stops*80<=run.size()&&dashes*24<=run.size();
}
// Known prefixes, tolerant of a lost or misread separator and of the usual
// letter confusions, followed by a run of the token alphabet; and the nominal
// length of the whole token, which bounds how far the mark extends. A prefix
// counts only at the start of a recognized word, and when its separator was
// lost, only where the body begins a word of its own ("ghp" "x8Fq…", not
// "ghost…"). A JSON web token needs a dot (or a comma read for one).
struct Prefix {QRegularExpression pattern;int length;bool boundary;bool dotted=false;};
const QList<Prefix>& prefixes() {
 static const QList<Prefix> list{
  {QRegularExpression(R"((A[KA4][Il1|]A|AS[Il1|]A|ABIA|ACCA|AGPA|AIDA|AIPA|ANPA|ANVA|APKA|AROA)()([A-Z0-9]{12,}))"),20,false},
  {QRegularExpression(R"(([gaq]h[pousr])([_,.\-]?)([A-Za-z0-9]{24,}))"),40,true},
  {QRegularExpression(R"((github[_,.]?pat)([_,.]?)([A-Za-z0-9_]{20,}))"),93,true},
  {QRegularExpression(R"((gl(?:pat|dt|rt|ptt|ft|cbt|imt|oas|soat|agent))([-_.,]?)([A-Za-z0-9_-]{15,}))"),26,true},
  {QRegularExpression(R"((x[o0O][xX]{1,2}[abposr])([-_.,]?)([A-Za-z0-9-]{10,}))"),56,true},
  {QRegularExpression(R"((xapp)([-_.,]?)(\d[-_.,]?[A-Za-z0-9-]{10,}))"),100,true},
  {QRegularExpression(R"(((?:sk|pk|rk)[_,.]?(?:live|test))([_,.]?)([A-Za-z0-9]{10,}))"),40,true},
  {QRegularExpression(R"((whsec)([_,.]?)([A-Za-z0-9]{20,}))"),38,true},
  // A JSON web token's header is base64url of "{"…", so it starts with eyJ.
  {QRegularExpression(R"((ey[JjIl1)\]}|])()([A-Za-z0-9_\-.,|!\]})]{24,}))"),400,false,true},
 };
 return list;
}
struct Segment {QList<int> words;QString text;QList<int> starts;};
// The token segments of one line: maximal runs of token pieces, joined.
QList<Segment> segments(const QList<int>& line,const QList<QString>& plain) {
 QList<Segment> out;Segment current;
 const auto close=[&]{if(!current.words.isEmpty())out.append(current);current={};};
 for(const int index:line){
  if(!tokenPiece(plain.at(index))){close();continue;}
  current.starts.append(int(current.text.size()));current.words.append(index);current.text+=plain.at(index);
 }
 close();return out;
}
int pieceAt(const Segment& segment,qsizetype offset) {
 int piece=0;for(int i=0;i<segment.starts.size();++i)if(segment.starts.at(i)<=offset)piece=i;return piece;
}
// Marks found in one segment: prefixed tokens first, then the longest hex or
// base64 run of whole pieces, then whatever is left either side of it.
void segmentMatches(const Segment& segment,int first,int last,QList<QPair<int,int>>& found) {
 if(first>last)return;
 const auto text=segment.text.mid(segment.starts.at(first),(last+1<segment.starts.size()?segment.starts.at(last+1):segment.text.size())-segment.starts.at(first));
 // A misread character inside a body reads as a letter for the body's sake.
 QString bodies=text;for(auto& c:bodies)if(!tokenCharacter(c))c='x';
 const int base=segment.starts.at(first);
 for(const auto& prefix:prefixes()){
  QRegularExpressionMatch match;
  for(auto it=prefix.pattern.globalMatch(prefix.dotted?text:bodies);it.hasNext();){
   const auto candidate=it.next();
   if(!segment.starts.contains(base+int(candidate.capturedStart())))continue;
   if(prefix.boundary&&candidate.capturedLength(2)==0&&!segment.starts.contains(base+int(candidate.capturedStart(3))))continue;
   if(prefix.dotted&&!candidate.captured(3).contains('.')&&!candidate.captured(3).contains(','))continue;
   match=candidate;break;
  }
  if(!match.hasMatch())continue;
  const int start=pieceAt(segment,base+match.capturedStart());
  // The token's pieces, up to half again its nominal length.
  int end=start;
  while(end<last&&segment.starts.at(end+1)-segment.starts.at(start)<prefix.length*3/2)++end;
  end=qMax(end,pieceAt(segment,base+match.capturedEnd()-1));
  segmentMatches(segment,first,start-1,found);found.append({start,end});segmentMatches(segment,end+1,last,found);return;
 }
 // Runs of up to 256 characters are tried, so a long line costs little on the
 // interface thread; a longer blob passes on a part of it and is marked whole.
 int bestFirst=-1,bestLast=-1,bestLength=0;
 for(int i=first;i<=last;++i)for(int j=last;j>=i;--j){
  const int from=segment.starts.at(i),to=j+1<segment.starts.size()?segment.starts.at(j+1):int(segment.text.size());
  if(to-from>256&&j>i)continue;
  if(to-from<=bestLength)break;
  const auto run=segment.text.mid(from,to-from);
  if(hexRun(run)||base64Run(run)||repeatedRun(run)){bestFirst=i;bestLast=j;bestLength=to-from;break;}
 }
 if(bestFirst<0)return;
 segmentMatches(segment,first,bestFirst-1,found);found.append({bestFirst,bestLast});segmentMatches(segment,bestLast+1,last,found);
}
QList<QList<int>> keyMatches(const QList<RecognizedWord>& candidates,const QList<QString>& plain) {
 QMap<int,QList<int>> lines;for(int i=0;i<candidates.size();++i)lines[candidates.at(i).line].append(i);
 const auto lineText=[&](const QList<int>& line){QStringList words;for(const int i:line)words.append(plain.at(i));return words.join(' ');};
 static const QRegularExpression begin(QStringLiteral("[-—–~_=.]{2,} ?BEGIN\\b[A-Z0-9 ]*PRIVATE ?KEY")),end(QStringLiteral("[-—–~_=.]{2,} ?END\\b[A-Z0-9 ]*PRIVATE ?KEY"));
 QList<QList<int>> out;
 const auto keys=lines.keys();
 for(int k=0;k<keys.size();++k){
  const auto& line=lines[keys.at(k)];
  // A PEM private-key block: its BEGIN line, every line after it up to its END
  // line (or while the lines are token-shaped, if END was not read), and END.
  if(begin.match(lineText(line)).hasMatch()){
   QList<int> block=line;
   while(k+1<keys.size()){
    const auto& next=lines[keys.at(k+1)];const auto text=lineText(next);
    const bool closing=end.match(text).hasMatch();
    QString joined;bool pieces=true;for(const int i:next){pieces=pieces&&tokenPiece(plain.at(i));joined+=plain.at(i);}
    if(!closing&&!(pieces&&joined.size()>=16))break;
    block+=next;++k;if(closing)break;
   }
   out.append(block);continue;
  }
  if(end.match(lineText(line)).hasMatch()){out.append(line);continue;}
  for(const auto& segment:segments(line,plain)){
   QList<QPair<int,int>> found;segmentMatches(segment,0,int(segment.words.size())-1,found);
   if(found.isEmpty())continue;
   // A segment with a token in it is marked whole: recognition splits a token
   // into pieces whose ends often fail a test on their own. Only ordinary words
   // at either end, outside what matched, are left out ("Bearer", "(expires
   // next week)."): no digit, no capital after the first letter, under 12 letters.
   {
    const auto plainWord=[&](int piece){
     const auto& text=plain.at(segment.words.at(piece));QString letters;for(const auto c:text)if(c.isLetterOrNumber())letters+=c;
     if(letters.size()>=12)return false;
     for(qsizetype i=0;i<letters.size();++i)if(letters.at(i).isDigit()||(i>0&&letters.at(i).isUpper()))return false;
     return true;
    };
    int core=int(segment.words.size()),coreEnd=-1;for(const auto& [from,to]:std::as_const(found)){core=qMin(core,from);coreEnd=qMax(coreEnd,to);}
    int a=0,b=int(segment.words.size())-1;
    while(a<core&&plainWord(a))++a;
    while(b>coreEnd&&plainWord(b))--b;
    QList<int> words=segment.words.mid(a,b-a+1);
    // A token the line wrapped: the next line's first segment, when it starts
    // the line and is token-shaped, belongs to it.
    if(words.last()==line.last()&&k+1<keys.size()){
     const auto next=segments(lines[keys.at(k+1)],plain);
     const auto nextText=lineText(lines[keys.at(k+1)]);
     if(!next.isEmpty()&&next.first().words.first()==lines[keys.at(k+1)].first()&&!begin.match(nextText).hasMatch()&&!end.match(nextText).hasMatch()){
      // The remainder of a token, not a line of words: a digit or a capital,
      // and few letters in lowercase runs of five or more.
      const auto& tail=next.first().text;bool digit=false,upper=false;for(const auto c:tail){digit=digit||c.isDigit();upper=upper||c.isUpper();}
      static const QRegularExpression prose(QStringLiteral("[a-z]{5,}"));qsizetype lowercase=0;for(auto it=prose.globalMatch(tail);it.hasNext();)lowercase+=it.next().capturedLength();
      if(tail.size()>=8&&(digit||upper)&&lowercase*10<=tail.size()*3&&tail.size()<qMax(8,int(segment.text.size())))words+=next.first().words;
     }
    }
    out.append(words);
   }
  }
 }
 return out;
}
struct Rule {QString category;QRegularExpression pattern;std::function<bool(const QString&)> valid;int view=0;};
// 0: as read; 1: digits; 2: hex; 3: email.
const QList<Rule>& rules() {
 static const QList<Rule> list{
  {"email",QRegularExpression(R"((?<![\w.%+-])[A-Za-z0-9][A-Za-z0-9._%+-]*@[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?)*\.[A-Za-z]{2,}(?![\w-]))"),{},3},
  {"card",QRegularExpression(R"((?<![\d-])\d(?:[ -]?\d){12,18}(?!\d))"),[](const QString& m){QString d;for(const auto c:m)if(c.isDigit())d+=c;return d.size()>=13&&d.size()<=19&&luhn(d);},1},
  {"ip",QRegularExpression(R"((?<![\d.])(?:(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)\.){3}(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)(?:/\d{1,2}|:\d{1,5})?(?!\d|\.\d))"),{},1},
  {"ip",QRegularExpression(R"((?<![\w:])(?:[0-9A-Fa-f]{0,4}:){2,7}[0-9A-Fa-f]{0,4}(?![\w:]))"),validIpv6,2},
  {"phone",QRegularExpression(R"((?<![\w+.-])(?:\+\d{1,3}[ .-]?)?(?:\(\d{1,4}\)[ .-]?)?\d{1,4}(?:[ .-]?\d{2,4}){1,5}(?![\w-]|\.\d))"),[](const QString& m){
    // At least 7 digits and at most 15 (E.164); a date or an IPv4 address is not a number to call.
    static const QRegularExpression date(R"(^(?:\d{4}[-./]\d{1,2}[-./]\d{1,2}|\d{1,2}[-./]\d{1,2}[-./]\d{2,4})$)"),dotted(R"(^\d{1,3}(?:\.\d{1,3}){3}$)");
    // Separated by dots alone, a number is a phone in the 3-3-4 grouping or in
    // pairs (01.23.45.67.89); 26.1112.4660 is a version.
    static const QRegularExpression dots(R"(^\d+(?:\.\d+)+$)"),grouped(R"(^(?:\d{3}\.\d{3}\.\d{4}|\d{2}(?:\.\d{2}){3,})$)");
    if(dots.match(m).hasMatch()&&!grouped.match(m).hasMatch())return false;
    // More than ten digits unbroken is written only after a "+" (16-446655440000 was a UUID's tail).
    static const QRegularExpression longGroup(R"(\d{11,})");
    if(!m.startsWith('+')&&longGroup.match(m).hasMatch())return false;
    const int count=digits(m);return count>=7&&count<=15&&!date.match(m).hasMatch()&&!dotted.match(m).hasMatch();},1},
 };
 return list;
}
}
QStringList categories() {return {"email","phone","ip","card","key","name","list"};}
QStringList keyShapes() {return {"AWS access key IDs","GitHub tokens (ghp_, gho_, ghu_, ghs_, ghr_, github_pat_)","GitLab tokens (glpat- and the other gl prefixes)","Slack tokens (xox…, xapp-)","Stripe keys (sk_, pk_, rk_, whsec_)","JSON web tokens","PEM private-key blocks","Unbroken hex or base64 runs of 32 characters or more"};}
bool luhn(const QString& number) {
 int sum=0;bool twice=false;int count=0;
 for(auto it=number.crbegin();it!=number.crend();++it){
  if(!it->isDigit())continue;int d=it->digitValue();++count;
  if(twice){d*=2;if(d>9)d-=9;}sum+=d;twice=!twice;
 }
 return count>0&&sum%10==0;
}
namespace {
QList<Match> findOrdered(const QList<RecognizedWord>& candidates,const QStringList& chosen);
// #1618, decision 3: name and list terms. Literal text, never a regex: an
// entry's parts (split at spaces, - _ . and lower-to-upper changes between its
// letters and digits; punctuation before the first or after the last stays
// part of the entry) must appear in order, joined by nothing or by one space
// or word gap, one of - _ . and one space or word gap, each optional, starting
// and ending at a word edge or beside a character that is not a letter or
// digit. Both sides are compared after NFKC, full case folding, one apostrophe
// and, on Latin, Greek and Cyrillic letters, accent stripping; a term of 6
// letters or more also folds the usual recognition confusions. Lengths count
// code points.
bool stripsMarks(QChar::Script script) {return script==QChar::Script_Latin||script==QChar::Script_Greek||script==QChar::Script_Cyrillic;}
QString literalView(const QString& word) {
 QList<char32_t> kept;QChar::Script base=QChar::Script_Unknown;
 for(const auto c:normalized(word).normalized(QString::NormalizationForm_KD).toUcs4()){
  const auto category=QChar::category(char32_t(c));
  const bool mark=category==QChar::Mark_NonSpacing||category==QChar::Mark_SpacingCombining||category==QChar::Mark_Enclosing;
  // Accents go only where the script uses them as accents; elsewhere (Indic
  // vowel signs, say) a mark is a letter of its own.
  if(mark){if(stripsMarks(base))continue;}else base=QChar::script(char32_t(c));
  kept.append(c==0x2019||c==0x2018||c==0x02bc||c==0x201b||c==0x2032||c==0xff07?char32_t('\''):char32_t(c));
 }
 // Qt 6.4 folds case one character to one; upper case first expands ß to SS
 // (full folding), and folding the lower case settles the rest.
 return QString::fromUcs4(kept.constData(),kept.size()).normalized(QString::NormalizationForm_C).toUpper().toLower().toCaseFolded();
}
char32_t pointAt(const QString& text,qsizetype i) {
 const auto c=text.at(i);
 if(c.isHighSurrogate()&&i+1<text.size()&&text.at(i+1).isLowSurrogate())return QChar::surrogateToUcs4(c,text.at(i+1));
 return c.unicode();
}
bool insidePair(const QString& text,qsizetype i) {return i>0&&i<text.size()&&text.at(i).isLowSurrogate()&&text.at(i-1).isHighSurrogate();}
qsizetype points(const QString& text) {qsizetype n=0;for(qsizetype i=0;i<text.size();++i)if(!insidePair(text,i))++n;return n;}
qsizetype letters(const QString& text) {qsizetype n=0;for(const auto c:text.toUcs4())if(QChar::isLetter(char32_t(c)))++n;return n;}
// I, l, 1 and | read as one character, as do O and 0 (case is already folded).
QChar confusion(QChar c) {return c=='i'||c=='l'||c=='1'||c=='|'?QChar('l'):c=='o'||c=='0'?QChar('o'):c;}
QStringList termParts(const QString& term) {
 const auto text=normalized(term).trimmed();
 qsizetype first=-1,last=-1;
 for(qsizetype i=0;i<text.size();++i)if(!insidePair(text,i)&&QChar::isLetterOrNumber(pointAt(text,i))){if(first<0)first=i;last=i;}
 // Nothing to find without a letter or digit (the validator refuses it).
 if(first<0)return {};
 if(text.at(last).isHighSurrogate())++last;
 QStringList parts;QString current;
 for(qsizetype i=first;i<=last;++i){
  const auto c=text.at(i);
  if(c.isSpace()||c=='-'||c=='_'||c=='.'){if(!current.isEmpty())parts.append(current);current.clear();continue;}
  if(c.isUpper()&&i>0&&text.at(i-1).isLower()&&!current.isEmpty()){parts.append(current);current.clear();}
  current+=c;
 }
 if(!current.isEmpty())parts.append(current);
 // Punctuation before the first letter or after the last stays literal: an
 // entry ".env" is not "env". Spaces there were trimmed.
 const auto edge=[](QString s){s.remove(QRegularExpression(QStringLiteral("\\s")));return s;};
 parts.first().prepend(edge(text.left(first)));parts.last().append(edge(text.mid(last+1)));
 for(auto& part:parts)part=literalView(part);
 parts.removeAll(QString());
 return parts;
}
// One spelling to look for: its parts, whether the join before each part
// after the first is a comma ("Hudson, Patrick"), and whether up to four digits
// may follow (usernames often end in digits).
struct Form {QStringList parts;QList<bool> comma;bool digits=false;bool fold=false;qsizetype weight=0;};
Form form(QStringList parts,bool digits=false,QList<bool> comma={}) {
 Form out;out.parts=std::move(parts);out.digits=digits;
 while(out.comma.size()+1<out.parts.size())out.comma.append(comma.value(out.comma.size(),false));
 qsizetype count=0;for(const auto& part:std::as_const(out.parts)){out.weight+=points(part);count+=letters(part);}
 out.fold=count>=6;
 return out;
}
bool real(const Joined& joined,qsizetype i) {return i>=0&&i<joined.text.size()&&joined.owner.at(i)>=0;}
// Whether the code point starting at `i`, or ending just before `i`, is a
// recognized letter or digit, or a mark that belongs to one (an Indic vowel
// sign is part of its word).
bool wordPoint(char32_t c) {return QChar::isLetterOrNumber(c)||QChar::isMark(c);}
bool wordCharacterAt(const Joined& joined,qsizetype i) {return real(joined,i)&&wordPoint(pointAt(joined.text,i));}
bool wordCharacterBefore(const Joined& joined,qsizetype i) {
 if(!real(joined,i-1))return false;
 return wordPoint(insidePair(joined.text,i-1)?pointAt(joined.text,i-2):pointAt(joined.text,i-1));
}
// Where a part starting at `at` can end: the comparison, with `rn` and `m`
// read for each other when folding.
void partEnds(const Joined& joined,qsizetype at,const QString& part,qsizetype k,bool fold,QList<qsizetype>& out) {
 if(k==part.size()){out.append(at);return;}
 if(!real(joined,at))return;
 const auto c=joined.text.at(at);
 if(fold){
  if(part.at(k)=='m'&&c=='r'&&real(joined,at+1)&&joined.text.at(at+1)=='n')partEnds(joined,at+2,part,k+1,fold,out);
  if(c=='m'&&k+1<part.size()&&part.at(k)=='r'&&part.at(k+1)=='n')partEnds(joined,at+1,part,k+2,fold,out);
  if(confusion(c)==confusion(part.at(k)))partEnds(joined,at+1,part,k+1,fold,out);
 }else if(c==part.at(k))partEnds(joined,at+1,part,k+1,fold,out);
}
// Where the next part may start after a join at `at`: one space or word gap,
// one of - _ . (with `comma`, a comma, which is then required) and one space
// or word gap, each optional. So "Patrick - Hudson" joins and
// "Patrick -- Hudson" does not.
QList<qsizetype> joinEnds(const Joined& joined,qsizetype at,bool comma) {
 const auto space=[&](qsizetype i){return i<joined.text.size()&&joined.text.at(i)==' ';};
 const auto separator=[&](qsizetype i){return real(joined,i)&&(comma?joined.text.at(i)==',':QStringLiteral("-_.").contains(joined.text.at(i)));};
 QList<qsizetype> out;
 for(const int before:{0,1}){
  if(before&&!space(at))continue;
  const auto p=at+before;
  for(const int middle:{0,1}){
   if(middle?!separator(p):comma)continue;
   const auto q=p+middle;
   out.append(q);if(space(q))out.append(q+1);
  }
 }
 return out;
}
// The longest end of `form` starting at `at`, or -1.
qsizetype formEnd(const Joined& joined,qsizetype at,const Form& form,int part=0) {
 QList<qsizetype> ends;partEnds(joined,at,form.parts.at(part),0,form.fold,ends);
 qsizetype best=-1;
 for(const auto end:std::as_const(ends)){
  if(part+1<form.parts.size()){for(const auto next:joinEnds(joined,end,form.comma.at(part)))best=qMax(best,formEnd(joined,next,form,part+1));continue;}
  auto stop=end;
  if(form.digits)for(int d=0;d<4&&real(joined,stop)&&joined.text.at(stop).isDigit();++d)++stop;
  // A term ends at a word edge or beside a character that is not a letter or digit.
  if(!wordCharacterAt(joined,stop)||!wordCharacterBefore(joined,stop))best=qMax(best,stop);
 }
 return best;
}
// Every occurrence, overlapping ones too: claim() keeps a word in one match.
QList<QList<int>> formHits(const Joined& joined,const Form& form) {
 QList<QList<int>> out;
 if(form.parts.isEmpty())return out;
 for(qsizetype at=0;at<joined.text.size();++at){
  if(insidePair(joined.text,at)||!real(joined,at)||(wordCharacterBefore(joined,at)&&wordCharacterAt(joined,at)))continue;
  const auto end=formEnd(joined,at,form);
  if(end>at)out.append(owners(joined,at,end-at));
 }
 return out;
}
// Decision 3's name forms: the whole name, "last, first" (first name only),
// first initial plus last name, the login (with the part rules, so jane.doe
// is also jane-doe), and each part of 3 characters or more on its own. A form
// of one part needs 3 characters too. The login and initial-plus-last forms may
// end in up to four digits.
QList<Form> nameForms(const QString& login,const QString& realName) {
 QList<Form> out;
 const auto add=[&](Form f){if(!f.parts.isEmpty()&&(f.parts.size()>1||points(f.parts.first())>=3))out.append(std::move(f));};
 const auto words=normalized(realName).split(QRegularExpression(QStringLiteral("\\s+")),Qt::SkipEmptyParts);
 if(!words.isEmpty()){
  add(form(termParts(realName)));
  if(words.size()>=2){
   const auto last=termParts(words.last()),first=termParts(words.first());
   if(!last.isEmpty()&&!first.isEmpty()){
    QList<bool> comma(last.size()-1,false);comma.append(true);
    add(form(last+first,false,comma));
    // A first name of one letter is its own initial: the whole name covers it.
    const auto initial=first.first().left(1);
    if(letters(first.join(QString()))>=2&&!initial.isEmpty()&&initial.at(0).isLetter()&&points(last.join(QString()))>=3)add(form(QStringList{initial}+last,true));
   }
  }
  for(const auto& word:words)for(const auto& part:termParts(word))if(points(part)>=3)add(form({part}));
 }
 add(form(termParts(login),true));
 // Longest first, so a whole name is one match rather than one per part.
 std::stable_sort(out.begin(),out.end(),[](const Form& a,const Form& b){return a.weight>b.weight;});
 return out;
}
// A hit keeps the words no earlier match claimed, as for keys: dropping it
// would leave a name bare because a neighbour took one of its words.
void claim(const QString& category,QList<int> words,QSet<int>& taken,QList<Match>& matches) {
 words.erase(std::remove_if(words.begin(),words.end(),[&](int w){return taken.contains(w);}),words.end());
 if(words.isEmpty())return;
 std::sort(words.begin(),words.end());words.erase(std::unique(words.begin(),words.end()),words.end());
 for(const int w:words)taken.insert(w);
 matches.append({category,words});
}
// Decision 6: a pattern sees each line's words joined with one space, and the
// next line after "\n" only where the wrap evidence joins them; elsewhere a
// line starts a new text. A lone surrogate becomes U+FFFD, so the text is valid
// UTF-16 and Qt need not check it on every call.
QList<Joined> patternTexts(const QList<QString>& plain,const QList<RecognizedWord>& candidates,const QSet<int>& full) {
 QList<Joined> out;
 for(int i=0;i<plain.size();++i){
  const bool boundary=i>0&&candidates.at(i).line!=candidates.at(i-1).line;
  if(i==0||(boundary&&!wrapEvidence(plain.at(i-1),plain.at(i),QStringLiteral("list"),full.contains(candidates.at(i-1).line))))out.append(Joined{});
  else{out.last().text+=boundary?'\n':' ';out.last().owner.append(-1);}
  auto word=plain.at(i);
  for(qsizetype k=0;k<word.size();++k){
   const bool paired=(word.at(k).isHighSurrogate()&&k+1<word.size()&&word.at(k+1).isLowSurrogate())||(word.at(k).isLowSurrogate()&&k>0&&word.at(k-1).isHighSurrogate());
   if(word.at(k).isSurrogate()&&!paired)word[k]=QChar(QChar::ReplacementCharacter);
  }
  out.last().text+=word;for(qsizetype k=0;k<word.size();++k)out.last().owner.append(i);
 }
 return out;
}
// Runaway patterns (decision 6), with a bound that holds for any pattern.
//
// QRegularExpression is PCRE2, but Qt sets no match limits, and libpcre2-16
// cannot be linked in every shipped build: the AppImage carries the official Qt
// 6.12, whose QtCore has PCRE2 built in. PCRE2's start-of-pattern items set
// the same limits from inside the pattern, and they can only lower the
// library's. The validator refuses a pattern that starts with "(*", so a user
// cannot raise them or turn off (*NOTEMPTY).
//
// PCRE2 counts its match limit again at every start position of one unanchored
// call, so one call over a long line can still run for minutes. Matching
// therefore steps through the start positions itself, one code point at a
// time, with the pattern anchored there (\G, which keeps JIT; a pattern whose
// wrapped form does not compile, such as an (?x) pattern ending in a comment,
// is anchored with AnchorAtOffsetMatchOption on the interpreter instead). Each
// call is then one attempt at one position, bounded by LIMIT_MATCH and
// LIMIT_DEPTH, and the clock is checked before every call. (*NOTEMPTY) makes
// an attempt that could match empty text find a longer match at the same
// position, or none: zero-length hits are skipped without stepping inside a
// surrogate pair. The text is valid UTF-16 (patternTexts), so Qt does not
// check it again on every call; that check alone cost 25 µs a position.
//
// Measured on m74 (Mint 22.3, Qt 6.4.2, Ryzen): an ordinary pattern costs about
// 0.2 µs a position (4 ms over 20 000 characters); the slowest single call,
// a runaway pattern stopped by the limit, 0.8 ms with JIT and 9 ms on the
// interpreter. All patterns together therefore end within the budget plus one
// such call.
const QString& patternLimits() {static const QString value=QStringLiteral("(*LIMIT_MATCH=100000)(*LIMIT_DEPTH=10000)(*NOTEMPTY)");return value;}
// A start-of-pattern item of the user's own could override the limits.
bool ownLimits(const QString& pattern) {return pattern.startsWith(QStringLiteral("(*"));}
// The matches mapped back to the caller's word order, sorted by first word.
Result mapped(Result result,const QList<int>& original) {
 for(auto& match:result.matches){for(auto& word:match.words)word=original.at(word);std::sort(match.words.begin(),match.words.end());}
 std::sort(result.matches.begin(),result.matches.end(),[](const Match& a,const Match& b){return a.words.first()<b.words.first();});
 return result;
}
}
QList<Match> find(const QList<RecognizedWord>& candidates,const QStringList& chosen) {return find(candidates,chosen,{},0).matches;}
Result find(const QList<RecognizedWord>& candidates,const QStringList& chosen,const Sources& sources,int budgetMs) {return find(candidates,chosen,sources,budgetMs,{});}
// Recognition may give one visual line as two layout blocks, so the end of a
// line arrives after the lines below it. Matching runs over lines as they
// appear: words grouped by the height of their centres (within half the
// median word height) and ordered left to right; the matches are mapped back.
Result find(const QList<RecognizedWord>& candidates,const QStringList& chosen,const Sources& sources,int budgetMs,const std::function<void(const Result&)>& progress) {
 QElapsedTimer clock;clock.start();
 QList<int> order(candidates.size());std::iota(order.begin(),order.end(),0);
 QList<int> heights;for(const auto& word:candidates)heights.append(word.rect.height());std::sort(heights.begin(),heights.end());
 const double tolerance=qMax(2.0,heights.isEmpty()?2.0:heights.at(heights.size()/2)*0.5);
 std::stable_sort(order.begin(),order.end(),[&](int a,int b){return candidates.at(a).rect.center().y()<candidates.at(b).rect.center().y();});
 QList<QList<int>> rows;double centre=0;
 for(const int i:order){
  const double y=candidates.at(i).rect.center().y();
  if(rows.isEmpty()||std::abs(y-centre)>tolerance){rows.append(QList<int>{i});centre=y;}
  else{rows.last().append(i);centre+=(y-centre)/rows.last().size();}
 }
 QList<RecognizedWord> ordered;QList<int> original;
 for(int r=0;r<rows.size();++r){
  auto row=rows.at(r);std::stable_sort(row.begin(),row.end(),[&](int a,int b){return candidates.at(a).rect.left()<candidates.at(b).rect.left();});
  for(const int i:row){auto word=candidates.at(i);word.line=r;ordered.append(word);original.append(i);}
 }
 Result result;
 result.matches=findOrdered(ordered,chosen);
 // #1618: name, then list, after the five kinds; a word belongs to one match.
 const bool name=chosen.contains("name"),list=chosen.contains("list");
 const int patternCount=list?int(sources.patterns.size()):0;
 // What is found so far, with every pattern not yet finished counted as skipped.
 const auto publish=[&](int finished){
  if(!progress)return;
  auto partial=mapped(result,original);
  for(int p=finished;p<patternCount;++p)partial.skippedPatterns.append(p);
  progress(partial);
 };
 if(name||list){
  QSet<int> taken;for(const auto& match:std::as_const(result.matches))for(const int w:match.words)taken.insert(w);
  const auto full=wrappingLines(ordered);
  QList<QString> plain,literal;for(const auto& word:ordered){plain.append(normalized(word.text));literal.append(literalView(word.text));}
  if(name){
   const auto joined=join(literal,ordered,false,QStringLiteral("name"),full);
   for(const auto& f:nameForms(sources.login,sources.realName))for(const auto& words:formHits(joined,f))claim("name",words,taken,result.matches);
  }
  if(list){
   // A list entry matches only whole, with the variations above.
   const auto joined=join(literal,ordered,false,QStringLiteral("list"),full);
   QList<Form> forms;for(const auto& term:sources.terms){auto f=form(termParts(term));if(!f.parts.isEmpty())forms.append(f);}
   std::stable_sort(forms.begin(),forms.end(),[](const Form& a,const Form& b){return a.weight>b.weight;});
   for(const auto& f:std::as_const(forms))for(const auto& words:formHits(joined,f))claim("list",words,taken,result.matches);
  }
  publish(0);
  if(list&&patternCount){
   // Patterns: case as written, Unicode properties, empty hits skipped; a hit
   // marks every word it touches. See patternLimits() for the bound.
   const auto texts=patternTexts(plain,ordered,full);
   // No budget left (or none given) skips every pattern still to run.
   const auto late=[&]{return budgetMs<=0||clock.hasExpired(budgetMs);};
   bool expired=false;
   for(int p=0;p<patternCount;++p){
    const auto& source=sources.patterns.at(p);
    if(expired||late()){expired=true;result.skippedPatterns.append(p);publish(p+1);continue;}
    const auto options=QRegularExpression::UseUnicodePropertiesOption;
    // Patterns arrive validated; one that is not (or sets its own limits) is
    // left out rather than wrapped into something it never said, like a)(b.
    if(ownLimits(source)||!QRegularExpression(source,options).isValid()){publish(p+1);continue;}
    QRegularExpression pattern(patternLimits()+QStringLiteral("\\G(?:")+source+QStringLiteral(")"),options);
    QRegularExpression::MatchOptions matchOptions=QRegularExpression::DontCheckSubjectStringMatchOption;
    if(!pattern.isValid()){pattern=QRegularExpression(patternLimits()+source,options);matchOptions|=QRegularExpression::AnchorAtOffsetMatchOption;}
    if(!pattern.isValid()){publish(p+1);continue;}
    bool stopped=false;
    for(const auto& text:texts){
     for(qsizetype at=0;at<text.text.size();){
      if(late()){stopped=expired=true;break;}
      const auto found=pattern.match(text.text,at,QRegularExpression::NormalMatch,matchOptions);
      // A match that ran into the limits is invalid: the pattern is skipped,
      // and what it found before is kept.
      if(!found.isValid()){stopped=true;break;}
      if(found.hasMatch()&&found.capturedLength()>0){
       claim("list",owners(text,found.capturedStart(),found.capturedLength()),taken,result.matches);
       at=found.capturedEnd();continue;
      }
      at+=text.text.at(at).isHighSurrogate()&&at+1<text.text.size()&&text.text.at(at+1).isLowSurrogate()?2:1;
     }
     if(stopped)break;
    }
    if(stopped)result.skippedPatterns.append(p);
    publish(p+1);
   }
  }
 }
 return mapped(std::move(result),original);
}
namespace {
QList<Match> findOrdered(const QList<RecognizedWord>& candidates,const QStringList& chosen) {
 QList<QString> plain,numbers,hex;
 for(const auto& word:candidates){const auto text=normalized(word.text);plain.append(text);numbers.append(digitView(text));hex.append(hexView(text));}
 const auto email=emailView(plain,candidates);
 const QList<QString>* views[4]={&plain,&numbers,&hex,&email};
 const auto full=wrappingLines(candidates);
 QSet<int> taken;QList<Match> matches;
 // Keys and tokens first: the most specific kind, matched line by line.
 // A key match that overlaps an earlier one keeps its other words: dropping it
 // would leave a token bare because a neighbour claimed one of its words.
 if(chosen.contains("key"))for(auto words:keyMatches(candidates,plain)){
  words.erase(std::remove_if(words.begin(),words.end(),[&](int w){return taken.contains(w);}),words.end());
  if(words.isEmpty())continue;
  for(int w:words)taken.insert(w);matches.append({"key",words});
 }
 // The rules run in order of specificity; a word belongs to the first match.
 for(const auto& rule:rules()){
  if(!chosen.contains(rule.category))continue;
  QList<QList<int>> pending;
  for(const bool wrapped:{false,true}){
   const auto joined=join(*views[rule.view],candidates,wrapped,rule.category,full);
   qsizetype offset=0;
   while(offset<joined.text.size()){
    auto found=rule.pattern.match(joined.text,offset);if(!found.hasMatch())break;
    auto length=found.capturedLength();
    // A greedy phone span may contain two separate numbers. Retry prefixes at
    // word boundaries, then resume at the accepted prefix's end.
    if(rule.category=="phone"&&rule.valid&&!rule.valid(found.captured())&&digits(found.captured())>15){
     for(qsizetype end=found.capturedEnd()-1;end>found.capturedStart();--end){
      if(!joined.text.at(end).isSpace())continue;
      const auto prefix=joined.text.mid(found.capturedStart(),end-found.capturedStart());
      const auto shorter=rule.pattern.match(prefix);
      if(shorter.hasMatch()&&shorter.capturedStart()==0&&shorter.capturedLength()==prefix.size()&&rule.valid(prefix)){length=prefix.size();break;}
     }
    }
    offset=found.capturedStart()+length;
    if(rule.valid&&!rule.valid(joined.text.mid(found.capturedStart(),length)))continue;
    auto words=owners(joined,found.capturedStart(),length);
    if(words.isEmpty())continue;
    // Merge both views before reserving any word, so a wrapped continuation
    // cannot be discarded merely because its prefix matched the plain view.
    for(qsizetype i=0;i<pending.size();){
     const auto& prior=pending.at(i);
     if(std::any_of(prior.cbegin(),prior.cend(),[&](int w){return words.contains(w);})){for(int w:prior)if(!words.contains(w))words.append(w);pending.removeAt(i);i=0;}else ++i;
    }
    std::sort(words.begin(),words.end());pending.append(words);
   }
  }
  for(const auto& words:pending){
   if(std::any_of(words.cbegin(),words.cend(),[&](int w){return taken.contains(w);}))continue;
   for(int w:words)taken.insert(w);matches.append({rule.category,words});
  }
 }
 std::sort(matches.begin(),matches.end(),[](const Match& a,const Match& b){return a.words.first()<b.words.first();});
 return matches;
}
}
QList<QRect> marks(const QList<RecognizedWord>& candidates,const Match& match,const QRect& bounds) {
 QMap<int,QRect> lines;
 for(const int index:match.words)if(index>=0&&index<candidates.size()){const auto& word=candidates.at(index);lines[word.line]=lines.value(word.line).united(word.rect);}
 QList<QRect> out;
 // A quarter of the line height above and below; beside it, at least one and
 // a half characters, since a recognized box can stop a whole glyph short of
 // the first or last character (measured: 7 px at 13 px type).
 QMap<int,int> characters;for(const int index:match.words)if(index>=0&&index<candidates.size())characters[candidates.at(index).line]+=int(candidates.at(index).text.size());
 for(auto it=lines.cbegin();it!=lines.cend();++it){const auto& box=it.value();const int pad=qMax(1,(box.height()+3)/4);const int side=qMax(pad,box.width()*3/qMax(2,2*characters.value(it.key())));const auto mark=box.adjusted(-side,-pad,side,pad).intersected(bounds);if(!mark.isEmpty())out.append(mark);}
 return out;
}
}
