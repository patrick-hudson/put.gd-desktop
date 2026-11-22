#include <climits>
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
#include <optional>

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
QString digitView(const QString& original) {
 static const QHash<QChar,QChar> confused{{'O','0'},{'l','1'},{'I','1'},{'S','5'},{'B','8'}};
 if(!hasDigit(original))return original;
 // #1820: between two digits, recognition reads a printed hyphen as a hyphen-like dash or a minus sign
 // (4111–1111–1111–1111). An em dash, or a dash beside anything else, stays as read.
 QString word=original;
 for(qsizetype i=1;i+1<word.size();++i){const auto c=word.at(i).unicode();if((c==0x2010||c==0x2011||c==0x2012||c==0x2013||c==0x2212)&&word.at(i-1).isDigit()&&word.at(i+1).isDigit())word[i]=QChar('-');}
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
// #1840: recognition may split an address in its local part, at a dot or with
// the dot lost ("jane." or "jane", then "doe@example.com"). Measured on
// putgd-ocr, the two boxes then overlap (by 1 to 20 px), while the space after a
// sentence before an address stays open (a quarter of a character or more) and
// ordinary words before one at most touch. A word of local-part characters
// joins the next word on its line when their boxes overlap and that word goes
// on to the "@", itself or through more such words; the joined part starts with
// a letter or digit, as the email rule needs. Entry i is true when word i joins
// word i - 1.
QList<bool> localJoins(const QList<QString>& words,const QList<RecognizedWord>& candidates) {
 static const QRegularExpression part(QStringLiteral("^[A-Za-z0-9._%+-]+$")),address(QStringLiteral("^[A-Za-z0-9._%+-]+@"));
 QList<bool> local(words.size()+1,false);
 for(int i=int(words.size())-1;i>0;--i){
  const auto& before=candidates.at(i-1);const auto& after=candidates.at(i);
  if(after.line!=before.line||after.rect.left()>before.rect.right()||!part.match(words.at(i-1)).hasMatch())continue;
  local[i]=address.match(words.at(i)).hasMatch()||(part.match(words.at(i)).hasMatch()&&local.at(i+1));
 }
 for(int i=1;i<words.size();++i)if(local.at(i)&&!local.at(i-1)&&!words.at(i-1).front().isLetterOrNumber())local[i]=false;
 return local;
}
// Every character keeps its word owner. Both views retain an unsupported
// line boundary; only the wrapped view removes a supported boundary entirely.
struct Joined {QString text;QList<int> owner;};
Joined join(const QList<QString>& words,const QList<RecognizedWord>& candidates,bool wrapped,const QString& category,const QSet<int>& full) {
 Joined joined;
 const auto local=category=="email"?localJoins(words,candidates):QList<bool>(words.size()+1,false);
 for(int i=0;i<words.size();++i){
  const bool boundary=i>0&&candidates.at(i).line!=candidates.at(i-1).line;
  const bool evidence=boundary&&wrapEvidence(words.at(i-1),words.at(i),category,full.contains(candidates.at(i-1).line));
  const bool wrap=wrapped&&evidence;
  // An IPv6 address read as two words split at a colon is one address.
  static const QRegularExpression fragment(QStringLiteral("^[0-9A-Fa-f:]+$"));
  const bool colon=i>0&&!boundary&&category=="ip"&&(words.at(i-1).endsWith(':')||words.at(i).startsWith(':'))&&fragment.match(words.at(i-1)).hasMatch()&&fragment.match(words.at(i)).hasMatch();
  // The email view restored a lost dot after the domain: its last label follows at once.
  // Not when this word starts a local-part join: then it begins another address.
  const bool dotted=i>0&&!boundary&&category=="email"&&words.at(i-1).contains('@')&&words.at(i-1).endsWith('.')&&!local.value(i+1);
  if(i>0&&!wrap&&!colon&&!dotted&&!local.at(i)){joined.text+=boundary&&!evidence?'\n':' ';joined.owner.append(-1);}
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
 // #1820: quotes around a word are punctuation, not misread characters ("“AKIA…" in an editor's string).
 static const QString quotes=QStringLiteral("\"'`\u201C\u201D\u2018\u2019\u00AB\u00BB");
 qsizetype from=0,to=word.size();
 while(from<to&&quotes.contains(word.at(from)))++from;
 while(to>from&&quotes.contains(word.at(to-1)))--to;
 if(from==to)return false;
 // Only around a token-like inside: 8 characters or more with a digit or a capital ("“note”" stays a word).
 {bool marked=false;for(qsizetype i=from;i<to;++i)marked=marked||word.at(i).isDigit()||word.at(i).isUpper();if(to-from<8||!marked){from=0;to=word.size();}}
 int noise=0;
 for(qsizetype i=from;i<to;++i){const auto c=word.at(i);if(!tokenCharacter(c)){if(c.isSpace())return false;++noise;}}
 return noise<=qMax(1,int(to-from)/8);
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
   // A prefix starts a piece, or follows a separator inside one (#1820: KEY_ID=AKIA…, "AKIA…", https://ghp_…@host).
   const int at=base+int(candidate.capturedStart());
   // Curly quotes too: an editor's string literal reads as "“AKIA…" (#1820).
   static const QString separators=QStringLiteral("=:/@\"'([`\u201C\u201D\u2018\u2019\u00AB\u00BB");
   const bool afterSeparator=!segment.starts.contains(at);
   if(afterSeparator&&!(at>0&&separators.contains(segment.text.at(at-1))))continue;
   // Inside a word, a token's body has a digit and a capital, as real keys do; a URL slug
   // (/glagent-installation-guide, “glagent-installation-guide-v2”) has neither or only a digit.
   if(afterSeparator){const auto body=candidate.captured(3);if(!hasDigit(body)||std::none_of(body.cbegin(),body.cend(),[](QChar c){return c.isUpper();}))continue;}
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
    // #1820: a token soft-wrapped over several lines (a JWT in an editor) takes each following line
    // while that line is all token and the token runs on; a short remainder ("08edc") counts too.
    for(int follow=k+1;follow<keys.size()&&words.last()==lines[keys.at(follow-1)].last();++follow){
     const auto& nextLine=lines[keys.at(follow)];
     const auto next=segments(nextLine,plain);
     const auto nextText=lineText(nextLine);
     if(next.isEmpty()||next.first().words.first()!=nextLine.first()||begin.match(nextText).hasMatch()||end.match(nextText).hasMatch())break;
     // The remainder of a token, not a line of words: a digit or a capital,
     // and few letters in lowercase runs of five or more.
     const auto& tail=next.first().text;bool digit=false,upper=false;for(const auto c:tail){digit=digit||c.isDigit();upper=upper||c.isUpper();}
     static const QRegularExpression prose(QStringLiteral("[a-z]{5,}")),word(QStringLiteral("[a-z]{4,}"));qsizetype lowercase=0;for(auto it=prose.globalMatch(tail);it.hasNext();)lowercase+=it.next().capturedLength();
     // A short remainder is one recognized word on its own, as a wrap leaves it, never words run together ("id42 ok").
     // ...and it ends the token where a wrap would: a closing quote, bracket or separator ("08edc\"").
     static const QString terminators=QStringLiteral("\"'`)]};,\u201D\u2019\u00BB");
     const auto& lastWord=plain.at(next.first().words.last());
     const bool shortTail=tail.size()>=3&&tail.size()<8&&digit&&!word.match(tail).hasMatch()&&next.first().words.size()==1&&!lastWord.isEmpty()&&terminators.contains(lastWord.back());
     if(!((tail.size()>=8||shortTail)&&(digit||upper)&&lowercase*10<=tail.size()*3&&tail.size()<=qMax(8,int(segment.text.size()))+2))break;
     words+=next.first().words;
     if(next.first().words.last()!=nextLine.last())break;
    }
    out.append(words);
   }
  }
 }
 return out;
}
struct Rule {QString category;QRegularExpression pattern;std::function<bool(const QString&)> valid;int view=0;};
// Grouped as a card is printed (4-4-4-4, with an optional short fifth group, or 4-6-5), with one separator
// throughout and a first digit that isn't 0 (a card number never starts with 0; 0044 … is a dialling prefix):
// a card number, valid or not, never a phone number (#1820).
bool cardShaped(const QString& text){
 static const QRegularExpression shape(R"(^(?:[1-9]\d{3}([ -])\d{4}\1\d{4}\1\d{4}(?:\1\d{1,3})?|[1-9]\d{3}([ -])\d{6}\2\d{5})$)");
 return shape.match(text.trimmed()).hasMatch();
}
// 0: as read; 1: digits; 2: hex; 3: email.
const QList<Rule>& rules() {
 static const QList<Rule> list{
  {"email",QRegularExpression(R"((?<![\w.%+-])[A-Za-z0-9][A-Za-z0-9._%+-]*@[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?)*\.[A-Za-z]{2,}(?![\w-]))"),{},3},
  {"card",QRegularExpression(R"((?<![\d-])(?<!\d\.)\d(?:[ -]?\d){12,18}(?!\d))"),[](const QString& m){QString d;for(const auto c:m)if(c.isDigit())d+=c;return d.size()>=13&&d.size()<=19&&luhn(d);},1},
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
    if(cardShaped(m))return false;
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
 // Default case folding, which keeps kır and kir apart, plus the one full
 // folding expansion Qt's one-to-one folding lacks after NFKC: ß and ẞ to ss.
 auto folded=QString::fromUcs4(kept.constData(),kept.size()).normalized(QString::NormalizationForm_C).toCaseFolded();
 folded.replace(QChar(0x00df),QStringLiteral("ss"));folded.replace(QChar(0x1e9e),QStringLiteral("ss"));
 return folded;
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
 // By code point, so a lower-to-upper change outside the BMP (Deseret 𐐨𐐁) splits too.
 char32_t previous=0;
 for(qsizetype i=first;i<=last;){
  const auto c=pointAt(text,i);const qsizetype width=c>0xffff?2:1;
  if(QChar::isSpace(c)||c=='-'||c=='_'||c=='.'){if(!current.isEmpty())parts.append(current);current.clear();previous=c;i+=width;continue;}
  if(QChar::isUpper(c)&&QChar::isLower(previous)&&!current.isEmpty()){parts.append(current);current.clear();}
  current+=text.mid(i,width);previous=c;i+=width;
 }
 if(!current.isEmpty())parts.append(current);
 // Punctuation before the first letter or after the last stays literal: an
 // entry ".env" is not "env". Whitespace there is a join like any other, so
 // "# 42" is the parts "#" and "42" and matches "# 42" and "#42".
 // One whitespace test everywhere, Qt's (as the validator trims): U+0085 too.
 const auto chunks=[](const QString& edge){QStringList out{QString()};for(const auto c:edge){if(c.isSpace())out.append(QString());else out.last()+=c;}return out;};
 auto lead=chunks(text.left(first)),trail=chunks(text.mid(last+1));
 parts.first().prepend(lead.takeLast());parts.last().append(trail.takeFirst());
 lead.removeAll(QString());trail.removeAll(QString());
 parts=lead+parts+trail;
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
// Where the next part may start after a join at `at` (decision 3): nothing, up
// to two characters from space - _ . (a word gap counts as a space), or a
// space, one of - _ . and a space ("Patrick - Hudson"). With `comma`, a comma
// with an optional space either side ("Hudson, Patrick"). So "patrick__hudson"
// joins and "Patrick -- Hudson" (four characters) does not. Each offset comes
// once.
QList<qsizetype> joinEnds(const Joined& joined,qsizetype at,bool comma) {
 const auto space=[&](qsizetype i){return i<joined.text.size()&&joined.text.at(i)==' ';};
 const auto mark=[&](qsizetype i){return real(joined,i)&&QStringLiteral("-_.").contains(joined.text.at(i));};
 const auto joiner=[&](qsizetype i){return space(i)||mark(i);};
 QList<qsizetype> out;
 const auto add=[&](qsizetype i){if(!out.contains(i))out.append(i);};
 if(comma){
  const auto p=space(at)?at+1:at;
  if(real(joined,p)&&joined.text.at(p)==','){add(p+1);if(space(p+1))add(p+2);}
  return out;
 }
 add(at);
 if(joiner(at)){add(at+1);if(joiner(at+1))add(at+2);}
 if(space(at)&&mark(at+1)&&space(at+2))add(at+3);
 return out;
}
// The longest end of `form` starting at `at`, or -1. Each (part, offset) is
// worked out once, so a long term with a near miss stays linear in its joins.
qsizetype formEnd(const Joined& joined,qsizetype at,const Form& form,int part,QHash<quint64,qsizetype>& seen) {
 const auto key=(quint64(part)<<40)|quint64(at);
 if(const auto it=seen.constFind(key);it!=seen.cend())return it.value();
 QList<qsizetype> ends;partEnds(joined,at,form.parts.at(part),0,form.fold,ends);
 qsizetype best=-1;
 for(const auto end:std::as_const(ends)){
  if(part+1<form.parts.size()){for(const auto next:joinEnds(joined,end,form.comma.at(part)))best=qMax(best,formEnd(joined,next,form,part+1,seen));continue;}
  auto stop=end;
  if(form.digits)for(int d=0;d<4&&real(joined,stop)&&joined.text.at(stop).isDigit();++d)++stop;
  // A term ends at a word edge or beside a character that is not a letter or digit.
  if(!wordCharacterAt(joined,stop)||!wordCharacterBefore(joined,stop))best=qMax(best,stop);
 }
 seen.insert(key,best);
 return best;
}
// Every occurrence, overlapping ones too: claim() keeps a word in one match.
QList<QList<int>> formHits(const Joined& joined,const Form& form) {
 QList<QList<int>> out;
 if(form.parts.isEmpty())return out;
 for(qsizetype at=0;at<joined.text.size();++at){
  if(insidePair(joined.text,at)||!real(joined,at)||(wordCharacterBefore(joined,at)&&wordCharacterAt(joined,at)))continue;
  QHash<quint64,qsizetype> seen;const auto end=formEnd(joined,at,form,0,seen);
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
 QStringList words{QString()};for(const auto c:normalized(realName)){if(c.isSpace())words.append(QString());else words.last()+=c;}
 words.removeAll(QString());
 if(!words.isEmpty()){
  add(form(termParts(realName)));
  if(words.size()>=2){
   const auto last=termParts(words.last()),first=termParts(words.first());
   if(!last.isEmpty()&&!first.isEmpty()){
    QList<bool> comma(last.size()-1,false);comma.append(true);
    add(form(last+first,false,comma));
    // A first name of one letter is its own initial: the whole name covers it.
    // The first character, whole: a letter outside the BMP is two UTF-16 units.
    const auto initial=first.first().left(first.first().at(0).isHighSurrogate()&&first.first().size()>1?2:1);
    if(letters(first.join(QString()))>=2&&!initial.isEmpty()&&QChar::isLetter(pointAt(initial,0))&&points(last.join(QString()))>=3)add(form(QStringList{initial}+last,true));
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
// Decision 6: the subjects a pattern sees (QuickRedact.h): every line, then
// every pair of consecutive lines in reading order, joined with "\n". A pair's
// hit counts only when it crosses that break, so a hit inside one line is
// claimed once, from the line, and pairs inside a line come out as they do in
// the line alone. A lone surrogate becomes U+FFFD (it can match nothing either
// way).
struct Subject {Joined text;qsizetype lineBreak=-1;};
QList<Subject> patternSubjects(const QList<QString>& plain,const QList<RecognizedWord>& candidates) {
 QList<Joined> lines;
 for(int i=0;i<plain.size();++i){
  if(i==0||candidates.at(i).line!=candidates.at(i-1).line)lines.append(Joined{});
  else{lines.last().text+=' ';lines.last().owner.append(-1);}
  auto word=plain.at(i);
  for(qsizetype k=0;k<word.size();++k){
   const bool paired=(word.at(k).isHighSurrogate()&&k+1<word.size()&&word.at(k+1).isLowSurrogate())||(word.at(k).isLowSurrogate()&&k>0&&word.at(k-1).isHighSurrogate());
   if(word.at(k).isSurrogate()&&!paired)word[k]=QChar(QChar::ReplacementCharacter);
  }
  lines.last().text+=word;for(qsizetype k=0;k<word.size();++k)lines.last().owner.append(i);
 }
 QList<Subject> out;
 for(const auto& line:std::as_const(lines))out.append({line});
 for(qsizetype k=0;k+1<lines.size();++k){
  Subject pair{lines.at(k),lines.at(k).text.size()};
  pair.text.text+='\n';pair.text.owner.append(-1);pair.text.text+=lines.at(k+1).text;pair.text.owner+=lines.at(k+1).owner;
  out.append(pair);
 }
 return out;
}
// The matches mapped back to the caller's word order, sorted by first word.
Result mapped(Result result,const QList<int>& original) {
 for(auto& match:result.matches){for(auto& word:match.words)word=original.at(word);std::sort(match.words.begin(),match.words.end());}
 std::sort(result.matches.begin(),result.matches.end(),[](const Match& a,const Match& b){return a.words.first()<b.words.first();});
 return result;
}
bool lowSurrogateAt(const QString& text,qsizetype i) {return i>0&&i<text.size()&&text.at(i).isLowSurrogate()&&text.at(i-1).isHighSurrogate();}
}
// PCRE2's own start-of-pattern limits come first: they can stop a search, never
// change a match, and stop the classic runaway shapes ((a+)+$) in about a
// millisecond, so the patterns after one still run. The time bound is the
// child's deadline kill (src/ui/QuickRedactRunner.h), not these.
void matchPatterns(const QStringList& subjects,const QStringList& patterns,const std::function<void(PatternOutcome)>& done,const QList<qsizetype>& lineBreaks) {
 static const QString limits=QStringLiteral("(*LIMIT_MATCH=10000)(*LIMIT_DEPTH=10000)");
 for(int p=0;p<patterns.size();++p){
  PatternOutcome outcome;outcome.pattern=p;
  const auto options=QRegularExpression::UseUnicodePropertiesOption;
  const QRegularExpression pattern(limits+patterns.at(p),options);
  // After an empty match, global matching tries for a longer one at the same
  // place (PCRE2_NOTEMPTY_ATSTART, anchored) before moving on, as
  // QRegularExpression::globalMatch does; its iterator cannot tell a stopped
  // search from the end, so the loop is written out.
  const QRegularExpression longer(limits+QStringLiteral("(*NOTEMPTY_ATSTART)")+patterns.at(p),options);
  bool finished=pattern.isValid()&&longer.isValid();
  for(int s=0;finished&&s<subjects.size();++s){
   // Made valid UTF-16 once (a lone surrogate becomes U+FFFD, the same length),
   // so no search has to check the whole subject again.
   auto text=subjects.at(s);
   for(qsizetype k=0;k<text.size();++k){
    const bool paired=(text.at(k).isHighSurrogate()&&k+1<text.size()&&text.at(k+1).isLowSurrogate())||(text.at(k).isLowSurrogate()&&k>0&&text.at(k-1).isHighSurrogate());
    if(text.at(k).isSurrogate()&&!paired)text[k]=QChar(QChar::ReplacementCharacter);
   }
   constexpr auto checked=QRegularExpression::DontCheckSubjectStringMatchOption;
   // Global iteration as Qt's: the next start is the previous match's end;
   // after an empty match, a non-empty one is tried at the same place
   // (anchored, PCRE2_NOTEMPTY_ATSTART), else the start moves one whole code
   // point. Every start, retries included, is first moved off the second half
   // of a surrogate pair (\C and \K can leave a match there), and an empty
   // match is never reported.
   const auto whole=[&](qsizetype i){return lowSurrogateAt(text,i)?i+1:i;};
   const auto nextPoint=[&](qsizetype i){return i+(i+1<text.size()&&text.at(i).isHighSurrogate()&&text.at(i+1).isLowSurrogate()?2:1);};
   for(qsizetype at=0;(at=whole(at))<=text.size();){
    auto found=pattern.match(text,at,QRegularExpression::NormalMatch,checked);
    if(!found.isValid()){finished=false;break;}
    if(!found.hasMatch())break;
    if(found.capturedLength()==0){
     const auto empty=whole(found.capturedStart());
     if(empty>text.size())break;
     const auto retry=longer.match(text,empty,QRegularExpression::NormalMatch,QRegularExpression::AnchorAtOffsetMatchOption|checked);
     if(!retry.isValid()){finished=false;break;}
     // An empty retry (\K can make one) counts as none: move on.
     if(!retry.hasMatch()||retry.capturedLength()==0){at=nextPoint(empty);continue;}
     found=retry;
    }
    // A pair's hit counts only where it crosses the break (QuickRedact.h); the
    // rest are left out here, so they never count toward a caller's cap.
    const auto lineBreak=lineBreaks.value(s,-1);
    if(lineBreak<0||(found.capturedStart()<=lineBreak&&found.capturedEnd()>lineBreak))outcome.hits.append({s,found.capturedStart(),found.capturedLength()});
    at=found.capturedEnd();
   }
  }
  outcome.finished=finished;
  if(!finished)outcome.hits.clear();
  if(done)done(std::move(outcome));
 }
}
QList<Match> find(const QList<RecognizedWord>& candidates,const QStringList& chosen) {return find(candidates,chosen,{},0).matches;}
Result find(const QList<RecognizedWord>& candidates,const QStringList& chosen,const Sources& sources,int budgetMs) {return find(candidates,chosen,sources,budgetMs,{});}
// Recognition may give one visual line as two layout blocks, so the end of a
// line arrives after the lines below it. Matching runs over lines as they
// appear: words grouped by the height of their centres (within half the
// median word height) and ordered left to right; the matches are mapped back.
Result find(const QList<RecognizedWord>& candidates,const QStringList& chosen,const Sources& sources,int budgetMs,const std::function<void(const Result&)>& progress,const std::atomic_bool* cancel,const PatternMatcher& matcher) {
 QElapsedTimer clock;clock.start();
 const auto cancelled=[cancel]{return cancel&&cancel->load(std::memory_order_relaxed);};
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
 const auto skipRest=[&](int from){for(int p=from;p<patternCount;++p)if(!result.skippedPatterns.contains(p))result.skippedPatterns.append(p);};
 // What is found so far, with every pattern not yet finished counted as skipped.
 const auto publish=[&](bool patternsDone){
  if(!progress)return;
  auto partial=mapped(result,original);
  if(!patternsDone)for(int p=0;p<patternCount;++p)partial.skippedPatterns.append(p);
  progress(partial);
 };
 if(name||list){
  QSet<int> taken;for(const auto& match:std::as_const(result.matches))for(const int w:match.words)taken.insert(w);
  const auto full=wrappingLines(ordered);
  QList<QString> plain,literal;for(const auto& word:ordered){plain.append(normalized(word.text));literal.append(literalView(word.text));}
  if(name&&!cancelled()){
   const auto joined=join(literal,ordered,false,QStringLiteral("name"),full);
   for(const auto& f:nameForms(sources.login,sources.realName))for(const auto& words:formHits(joined,f))claim("name",words,taken,result.matches);
  }
  if(list&&!cancelled()){
   // A list entry matches only whole, with the variations above.
   const auto joined=join(literal,ordered,false,QStringLiteral("list"),full);
   QList<Form> forms;for(const auto& term:sources.terms){auto f=form(termParts(term));if(!f.parts.isEmpty())forms.append(f);}
   std::stable_sort(forms.begin(),forms.end(),[](const Form& a,const Form& b){return a.weight>b.weight;});
   for(const auto& f:std::as_const(forms)){if(cancelled())break;for(const auto& words:formHits(joined,f))claim("list",words,taken,result.matches);}
  }
  if(cancelled()){skipRest(0);return mapped(std::move(result),original);}
  publish(false);
  if(patternCount){
   // Patterns see the recognized text as shown (the exact regex as typed): no
   // NFKC, digit or case folding, which are for list words and names only.
   QList<QString> shown;for(const auto& word:ordered)shown.append(word.text);
   const auto subjects=patternSubjects(shown,ordered);
   QStringList texts;QList<qsizetype> breaks;for(const auto& subject:subjects){texts.append(subject.text.text);breaks.append(subject.lineBreak);}
   // The outcomes, by pattern. In process (no matcher), the budget is read
   // between patterns; a pattern still running is not stopped.
   QHash<int,PatternOutcome> outcomes;
   if(matcher){for(auto& outcome:matcher(texts,breaks,sources.patterns))if(outcome.pattern>=0&&outcome.pattern<patternCount)outcomes.insert(outcome.pattern,std::move(outcome));}
   else{
    QStringList pending=sources.patterns;
    for(int p=0;p<patternCount;++p){
     if(budgetMs<=0||clock.hasExpired(budgetMs)||cancelled())break;
     matchPatterns(texts,{pending.at(p)},[&](PatternOutcome outcome){outcome.pattern=p;outcomes.insert(p,std::move(outcome));},breaks);
    }
   }
   // Claimed in pattern order, so an earlier pattern's hit wins a word.
   for(int p=0;p<patternCount;++p){
    const auto it=outcomes.constFind(p);
    if(it==outcomes.cend()||!it->finished){result.skippedPatterns.append(p);continue;}
    for(const auto& hit:it->hits){
     if(hit.subject<0||hit.subject>=subjects.size())continue;
     const auto& subject=subjects.at(hit.subject);
     if(hit.start<0||hit.length<=0||hit.start+hit.length>subject.text.text.size())continue;
     // A pair's hit only when it crosses the line break.
     if(subject.lineBreak>=0&&!(hit.start<=subject.lineBreak&&hit.start+hit.length>subject.lineBreak))continue;
     claim("list",owners(subject.text,hit.start,hit.length),taken,result.matches);
    }
   }
   publish(true);
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
 // #1840: the words an email match took in by a local-part join are claimed
 // last, and only where no other match holds them, so a join never costs
 // another kind its match or an address its match.
 const auto local=chosen.contains("email")?localJoins(email,candidates):QList<bool>();
 QList<QPair<int,QList<int>>> soft;
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
    // A card-grouped run whose words are evenly spaced is one printed number, not two to split; a wider
    // gap between its words is two fields (two phone numbers in a table), split as before (#1820).
    const auto evenlySpaced=[&]{
     const auto words=owners(joined,found.capturedStart(),found.capturedLength());
     int widest=0,narrowest=INT_MAX;
     for(qsizetype i=1;i<words.size();++i){const auto& a=candidates.at(words.at(i-1)).rect;const auto& b=candidates.at(words.at(i)).rect;if(a.y()!=b.y()&&a.bottom()<b.top())return false;const int gap=qMax(0,b.left()-a.right());widest=qMax(widest,gap);narrowest=qMin(narrowest,gap);}
     return words.size()<2||widest<=2*qMax(narrowest,2)+2;
    };
    if(rule.category=="phone"&&rule.valid&&!rule.valid(found.captured())&&digits(found.captured())>15&&!(cardShaped(found.captured())&&evenlySpaced())){
     const auto accept=[&](qsizetype end){
      const auto prefix=joined.text.mid(found.capturedStart(),end-found.capturedStart());
      const auto shorter=rule.pattern.match(prefix);
      if(shorter.hasMatch()&&shorter.capturedStart()==0&&shorter.capturedLength()==prefix.size()&&rule.valid(prefix)){length=prefix.size();return true;}
      return false;
     };
     // #1820: the widest gap between the span's words first, where two fields (table columns) meet.
     bool split=false;
     {const auto words=owners(joined,found.capturedStart(),found.capturedLength());int widest=-1,after=-1;
      for(qsizetype i=1;i<words.size();++i){const int gap=candidates.at(words.at(i)).rect.left()-candidates.at(words.at(i-1)).rect.right();if(gap>widest){widest=gap;after=words.at(i);}}
      if(after>=0)for(qsizetype end=found.capturedStart();end<found.capturedEnd();++end)if(joined.owner.at(end)==after){qsizetype space=end-1;while(space>found.capturedStart()&&!joined.text.at(space).isSpace())--space;if(space>found.capturedStart()&&joined.text.at(space).isSpace())split=accept(space);break;}}
     if(!split)for(qsizetype end=found.capturedEnd()-1;end>found.capturedStart();--end){
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
  for(auto words:pending){
   QList<int> joined;
   if(rule.category=="email")for(const int w:std::as_const(words))if(local.value(w+1)&&words.contains(w+1))joined.append(w);
   words.erase(std::remove_if(words.begin(),words.end(),[&](int w){return joined.contains(w);}),words.end());
   if(words.isEmpty()||std::any_of(words.cbegin(),words.cend(),[&](int w){return taken.contains(w);}))continue;
   for(int w:words)taken.insert(w);matches.append({rule.category,words});
   if(!joined.isEmpty())soft.append({int(matches.size())-1,joined});
  }
 }
 for(const auto& [index,words]:std::as_const(soft)){
  auto& match=matches[index].words;
  for(const int w:words)if(!taken.contains(w)){taken.insert(w);match.append(w);}
  std::sort(match.begin(),match.end());
 }
 std::sort(matches.begin(),matches.end(),[](const Match& a,const Match& b){return a.words.first()<b.words.first();});
 return matches;
}
}
namespace {
// How far a cover may reach from the matched box `box` towards the nearest
// unmatched word's box `other` (direction `dir`, -1 left or +1 right), or
// nullopt to keep the whole margin. Recognized boxes can each be off by about
// a glyph (#1838: at 13 px a box ended 4 px short of its digit and the next
// word's box began 6 px before its "i"), so with the image the cover runs to
// the far side of the blank space before the neighbour, checked over every row
// the mark covers, and leaves that space's last column uncovered.
// - The neighbour's box edge is trusted only where the picture agrees: within
//   a pixel of it, ink starts right after a blank column. The space is the
//   blank run just before that ink.
// - Otherwise the edge may sit in the space, in a gap inside the match, or on
//   a matched glyph: the cover goes past the blank or ink there and the ink
//   after it, to the next blank run. A misplaced box can then cost the
//   neighbour a glyph, never show a matched one.
// The paper is the median of the line spacing outside the mark. With no clean
// spacing, no blank run in reach, or any transparency (what shows there
// depends on the viewer), nothing is clipped. Without the image, the
// neighbour's box is the limit.
std::optional<int> reach(const QImage& image,const QRect& box,const QRect& other,int dir,double glyph,int pad){
 const int edge=dir>0?box.right():box.left(),neighbour=dir>0?other.left():other.right();
 if(image.isNull()){const int gap=qMax(1,box.height()/10);return dir>0?qMax(edge,neighbour-1-gap):qMin(edge,neighbour+1+gap);}
 const int slack=int(std::ceil(glyph));
 // From the matched box's own edge column, which a box can overshoot onto the space.
 // Its far end is clipped to the image; the edge itself is inside it.
 const QRect window=QRect(QPoint(qMin(edge,neighbour+dir*slack),box.top()-pad),QPoint(qMax(edge,neighbour+dir*slack),box.bottom()+pad)).intersected(image.rect());
 if(window.isEmpty()||edge<window.left()||edge>window.right())return std::nullopt;
 const QRect around=window.adjusted(0,-pad,0,pad).intersected(image.rect());
 const auto band=image.copy(around).convertToFormat(QImage::Format_ARGB32);
 const int first=window.top()-around.top(),last=window.bottom()-around.top();
 QList<int> channels[3];
 for(int y=0;y<band.height();++y){
  const auto* line=reinterpret_cast<const QRgb*>(band.constScanLine(y));
  for(int x=0;x<band.width();++x){
   if(qAlpha(line[x])!=255)return std::nullopt;
   if(y>=first&&y<=last)continue;
   channels[0].append(qRed(line[x]));channels[1].append(qGreen(line[x]));channels[2].append(qBlue(line[x]));
  }
 }
 if(channels[0].isEmpty())return std::nullopt;
 int paper[3];
 for(int k=0;k<3;++k){auto c=channels[k];std::nth_element(c.begin(),c.begin()+c.size()/2,c.end());paper[k]=c.at(c.size()/2);}
 const auto distance=[&](int r,int g,int b){return qMax(std::abs(r-paper[0]),qMax(std::abs(g-paper[1]),std::abs(b-paper[2])));};
 // Spacing with much else in it (a neighbouring line's ink, a pattern) is no
 // measure of the paper.
 qsizetype stray=0;for(qsizetype i=0;i<channels[0].size();++i)if(distance(channels[0].at(i),channels[1].at(i),channels[2].at(i))>24)++stray;
 if(stray*4>channels[0].size())return std::nullopt;
 // A column is blank when no pixel the mark would cover is further from the
 // paper than a faint anti-aliased edge: 8% of the ink's contrast, within 4 to 24.
 int contrast=0;
 for(int y=first;y<=last;++y){const auto* line=reinterpret_cast<const QRgb*>(band.constScanLine(y));for(int x=0;x<band.width();++x)contrast=qMax(contrast,distance(qRed(line[x]),qGreen(line[x]),qBlue(line[x])));}
 const int faint=std::clamp(contrast*8/100,4,24);
 // Columns counted from the matched box's edge (0) towards the neighbour.
 const int count=band.width();
 QList<bool> blank(count,true);
 for(int y=first;y<=last;++y){
  const auto* line=reinterpret_cast<const QRgb*>(band.constScanLine(y));
  for(int i=0;i<count;++i){const QRgb c=line[dir>0?i:count-1-i];if(blank[i]&&distance(qRed(c),qGreen(c),qBlue(c))>faint)blank[i]=false;}
 }
 const int at=dir*(neighbour-edge);
 int end=-1; // the last blank column of the space
 const auto runBefore=[&](int i){int n=0;while(i-1-n>=0&&blank[i-1-n])++n;return n;};
 // The box edge agrees with the picture when an ink start (ink right after a
 // blank column) is within a pixel of it: boxes are often a pixel off. That
 // ink is the neighbour's only if the blank before it is at least as wide as
 // the next one after it: a word space is wider than a gap between glyphs, so
 // a narrower one makes it more likely a last matched glyph.
 for(int d=0;d<=1&&end<0;++d)for(const int i:{at-d,at+d}){
  if(end>=0||i<1||i>=count||blank[i]||!blank[i-1])continue;
  int j=i;while(j<count&&!blank[j])++j;
  int after=0;while(j+after<count&&blank[j+after])++after;
  if(runBefore(i)>=after)end=i-1;
 }
 if(end<0){
  // Past whatever lies at the edge and the ink after it, to the next blank run.
  int i=at;
  while(i<count&&blank[i])++i;
  while(i<count&&!blank[i])++i;
  if(i>=count)return std::nullopt;
  while(i+1<count&&blank[i+1])++i;
  end=i;
 }
 return edge+dir*qMax(0,end-1);
}
}
// One box per visual row of the match, as find groups rows (centres within
// half the median word height), never by the recognizer's line id: a match
// that spans two rows makes two marks, so nothing between them is covered.
QList<MarkBox> markBoxes(const QList<RecognizedWord>& candidates,const Match& match,const QRect& bounds,const QImage& image) {
 QList<int> words;for(const int index:match.words)if(index>=0&&index<candidates.size())words.append(index);
 std::stable_sort(words.begin(),words.end(),[&](int a,int b){return candidates.at(a).rect.center().y()<candidates.at(b).rect.center().y();});
 QList<int> heights;for(const int w:words)heights.append(candidates.at(w).rect.height());std::sort(heights.begin(),heights.end());
 const double tolerance=qMax(2.0,heights.isEmpty()?2.0:heights.at(heights.size()/2)*0.5);
 struct Row {QRect box;double centre=0;int count=0;int first=-1;int last=-1;};
 QList<Row> rows;
 for(const int w:words){
  const auto& word=candidates.at(w);const double y=word.rect.center().y();
  if(rows.isEmpty()||std::abs(y-rows.last().centre)>tolerance)rows.append(Row{});
  auto& row=rows.last();row.box=row.box.united(word.rect);++row.count;row.centre+=(y-row.centre)/row.count;
  if(row.first<0||word.rect.left()<candidates.at(row.first).rect.left())row.first=w;
  if(row.last<0||word.rect.right()>candidates.at(row.last).rect.right())row.last=w;
 }
 // A word's glyph width from its own box: the row's span would count the
 // gaps between words as characters (#1838).
 const auto glyph=[&](int w){
  const auto& word=candidates.at(w);
  const auto characters=std::count_if(word.text.cbegin(),word.text.cend(),[](QChar c){return !c.isLowSurrogate()&&!c.isSpace();});
  return double(word.rect.width())/double(qMax<qsizetype>(1,characters));
 };
 QList<MarkBox> out;
 // A quarter of the line height above and below; beside it, one and a half
 // of the edge word's glyphs, since a recognized box can stop a whole glyph
 // short of the first or last character (measured: 7 px at 13 px type). On a
 // side with another word on the row that is not part of the match, the
 // margin ends where reach() says, so that word stays readable; it never cuts
 // into the matched words' own box.
 for(const auto& row:std::as_const(rows)){
  const auto& box=row.box;const int pad=qMax(1,(box.height()+3)/4);
  const double before=glyph(row.first),after=glyph(row.last);
  int left=box.left()-qMax(pad,int(std::ceil(before*1.5)));
  int right=box.right()+qMax(pad,int(std::ceil(after*1.5)));
  std::optional<QRect> leftNeighbour,rightNeighbour;
  for(qsizetype i=0;i<candidates.size();++i){
   if(match.words.contains(int(i)))continue;
   const auto& other=candidates.at(i).rect;
   if(other.isEmpty()||std::abs(other.center().y()-row.centre)>tolerance)continue;
   // Only a word the margin, plus a glyph for the boxes' error, could reach.
   if(other.right()<box.left()&&other.right()>=left-int(std::ceil(before))){if(!leftNeighbour||other.right()>leftNeighbour->right())leftNeighbour=other;}
   else if(other.left()>box.right()&&other.left()<=right+int(std::ceil(after))){if(!rightNeighbour||other.left()<rightNeighbour->left())rightNeighbour=other;}
  }
  if(leftNeighbour)if(const auto limit=reach(image,box,*leftNeighbour,-1,before,pad))left=qMax(left,*limit);
  if(rightNeighbour)if(const auto limit=reach(image,box,*rightNeighbour,1,after,pad))right=qMin(right,*limit);
  const auto mark=QRect(QPoint(left,box.top()-pad),QPoint(right,box.bottom()+pad)).intersected(bounds);if(!mark.isEmpty())out.append({mark,box});
 }
 return out;
}
QList<QRect> marks(const QList<RecognizedWord>& candidates,const Match& match,const QRect& bounds,const QImage& image) {
 QList<QRect> out;for(const auto& box:markBoxes(candidates,match,bounds,image))out.append(box.mark);
 return out;
}
}
