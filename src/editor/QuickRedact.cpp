#include <climits>
#include "QuickRedact.h"
#include <QElapsedTimer>
#include <QChar>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QPair>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <array>
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
// read as 0, 1, 1, 5 and 8 in a word that has a digit and no other letter, and
// @ as 0 between two digits or a digit and a dot (198.51.10@.223, #1913).
QString digitView(const QString& original) {
 static const QHash<QChar,QChar> confused{{'O','0'},{'l','1'},{'I','1'},{'i','1'},{'S','5'},{'B','8'}};
 if(!hasDigit(original))return original;
 // #1820: between two digits, recognition reads a printed hyphen as a hyphen-like dash or a minus sign
 // (4111–1111–1111–1111). An em dash, or a dash beside anything else, stays as read.
 QString word=original;
 for(qsizetype i=1;i+1<word.size();++i){const auto c=word.at(i).unicode();if((c==0x2010||c==0x2011||c==0x2012||c==0x2013||c==0x2212)&&word.at(i-1).isDigit()&&word.at(i+1).isDigit())word[i]=QChar('-');}
 for(const auto c:word)if(c.isLetter()&&!confused.contains(c))return word;
 QString out=word;for(auto& c:out)c=confused.value(c,c);
 for(qsizetype i=1;i+1<out.size();++i)if(out.at(i)=='@'&&(out.at(i-1).isDigit()||out.at(i-1)=='.')&&(out.at(i+1).isDigit()||out.at(i+1)=='.'))out[i]='0';
 return out;
}
// Inside an IPv6 address the alphabet is hex: O, l, I, i and S read as 0, 1, 1, 1 and 5, and @ as 0
// (fe8@::…, fe80::4fic:…; #1913).
QString hexView(const QString& word) {
 static const QHash<QChar,QChar> confused{{'O','0'},{'l','1'},{'I','1'},{'i','1'},{'S','5'},{'@','0'}};
 if(!word.contains(':'))return word;
 for(const auto c:word)if(c.isLetter()&&!confused.contains(c)&&!QStringLiteral("abcdefABCDEF").contains(c))return word;
 QString out=word;for(auto& c:out)c=confused.value(c,c);
 // ...and only where every group stays a group of four or fewer: "alice@2001:db8::1" is a name, then
 // an address (Sol on #1916).
 {static const QRegularExpression groups(QStringLiteral("[^:\\[\\]/]{5,}"));
  // Inside its brackets when it has them ("[2001:db8::AB0D]:54321": the port is no group).
  auto head=out.left(out.indexOf('/')<0?out.size():out.indexOf('/'));
  if(head.contains(']'))head=head.left(head.indexOf(']'));
  if(groups.match(head).hasMatch())return word;}
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
// #1930: only a word close beside it (a gap no wider than the line is tall)
// and only from a word wide enough to hold an address. Read off a bar chart,
// "s@525" and "ia" 648 px apart on one row were taken for s@525.ia; a chart
// legend's square colour swatch read as "W@W" (a 25 x 25 box) before its label
// "Egress" for W@W.Egress. Even "i@i" is 1.3 times as wide as it is tall (13 x 10
// at 10 px).
QList<QString> emailView(const QList<QString>& plain,const QList<RecognizedWord>& candidates) {
 static const QRegularExpression letters(QStringLiteral("^[A-Za-z]{2,24}$"));
 QList<QString> out=plain;
 // #1942: an "l" just before the "@" read as "|" (tanaka.|@mail.aperture.com).
 for(auto& word:out){const auto at=word.indexOf('@');if(at>1&&word.at(at-1)=='|'&&(word.at(at-2).isLetterOrNumber()||word.at(at-2).isMark()||word.at(at-2).isLowSurrogate()||QStringLiteral("._%+-").contains(word.at(at-2))))word[at-1]='l';}
 for(qsizetype i=0;i+1<plain.size();++i){
  const auto at=out.at(i).indexOf('@');
  const auto& word=candidates.at(i).rect;const auto& next=candidates.at(i+1).rect;
  const bool beside=next.left()-(word.right()+1)<=qMax(word.height(),next.height());
  const bool wide=word.width()*10>word.height()*11;
  if(at>0&&!out.at(i).mid(at).contains('.')&&beside&&wide&&candidates.at(i+1).line==candidates.at(i).line&&letters.match(plain.at(i+1)).hasMatch())out[i]+='.';
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
 static const QRegularExpression part(QStringLiteral("^[\\p{L}\\p{M}\\p{N}._%+-]+$")),address(QStringLiteral("^[\\p{L}\\p{M}\\p{N}._%+-]+@"));
 QList<bool> local(words.size()+1,false);
 for(int i=int(words.size())-1;i>0;--i){
  const auto& before=candidates.at(i-1);const auto& after=candidates.at(i);
  if(after.line!=before.line||after.rect.left()>before.rect.right()||!part.match(words.at(i-1)).hasMatch())continue;
  // The last label of a domain whose dot was restored ("alice@example." "com") starts no address.
  static const QRegularExpression restored(QStringLiteral("^[\\p{L}\\p{N}][\\p{L}\\p{M}\\p{N}._%+-]*@[A-Za-z0-9-]+\\.$"));
  if(i>1&&candidates.at(i-2).line==before.line&&restored.match(words.at(i-2)).hasMatch())continue;
  local[i]=address.match(words.at(i)).hasMatch()||(part.match(words.at(i)).hasMatch()&&local.at(i+1));
 }
 for(int i=1;i<words.size();++i)if(local.at(i)&&!local.at(i-1)&&!words.at(i-1).front().isLetterOrNumber())local[i]=false;
 return local;
}
// #1942: recognition splits an address at its "@" or before a dot of its domain
// ("zoe.sato8" "@wayne-corp.ai", "diego-reyes@" "mail.fabrikam.com", "kenji@gmail"
// ".com"), leaving a gap a quarter of the line's height (at most 0.47 measured), while
// a word before an @-mention stands at least half a line's height off (0.53 in 95%).
// Two words on a line split so, at most 0.45 line heights apart, read as one.
bool splitAddress(const QString& left,const QString& right,const QRect& a,const QRect& b) {
 if(left.isEmpty()||right.size()<2||b.left()-(a.right()+1)>0.45*qMax(a.height(),b.height()))return false;
 // As the email rule reads them: a local part's tail after any opening punctuation ("<jane", "jane+"),
 // and a domain's first label.
 static const QRegularExpression local(QStringLiteral("[\\p{L}\\p{N}][\\p{L}\\p{M}\\p{N}._%+-]*$")),domain(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9-]*\\.[A-Za-z0-9]")),
  complete(QStringLiteral("@[A-Za-z0-9.-]*\\.[A-Za-z]{2,}$")),country(QStringLiteral("^\\.[A-Za-z]{2}[,;:)>\\]]?$"));
 if(right.front()=='@')return !left.contains('@')&&local.match(left).hasMatch()&&domain.match(right.mid(1)).hasMatch();
 if(left.back()=='@')return left.count('@')==1&&local.match(left.chopped(1)).hasMatch()&&domain.match(right).hasMatch();
 // Before a domain dot only while the domain is still unfinished ("kenji@gmail" ".com"): a whole
 // address is not extended into a name after it ("alice@example.com" ".env").
 // A country code may still follow a whole domain ("alice@example.co" ".uk", Sol on #1956).
 return left.count('@')==1&&left.indexOf('@')>0&&(!complete.match(left).hasMatch()||country.match(right).hasMatch())&&right.front()=='.'&&right.at(1).isLetterOrNumber();
}
// Every character keeps its word owner. Both views retain an unsupported
// line boundary; only the wrapped view removes a supported boundary entirely.
struct Joined {QString text;QList<int> owner;};
Joined join(const QList<QString>& words,const QList<RecognizedWord>& candidates,bool wrapped,const QString& category,const QSet<int>& full,bool splits=true) {
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
  // Not after a whole address that ends a sentence ("alice@example.net." then "jane+" "tag@…"),
  // when this word starts a local-part join: then it begins another address.
  QString domain=i>0?words.at(i-1).mid(words.at(i-1).indexOf('@')+1):QString();domain.chop(1);
  // #1942: only after a local part: a handle ("@support.") ends its sentence and starts no address.
  // And only before a label: a letter, or a digit that goes on to a dot ("jane@mx." "2fa.example.com");
  // a sentence's period then "2026" is no domain (Sol on #1956).
  static const QRegularExpression labels(QStringLiteral("^[0-9][A-Za-z0-9-]*\\.[A-Za-z]"));
  const bool label=i>0&&!words.at(i).isEmpty()&&(words.at(i).front().isLetter()||labels.match(words.at(i)).hasMatch());
  const bool dotted=i>0&&!boundary&&category=="email"&&words.at(i-1).indexOf('@')>0&&words.at(i-1).endsWith('.')&&label&&!(local.value(i+1)&&domain.contains('.'));
  if(i>0&&!wrap&&!colon&&!dotted&&!local.at(i)&&!(splits&&category=="email"&&!boundary&&splitAddress(words.at(i-1),words.at(i),candidates.at(i-1).rect,candidates.at(i).rect))){joined.text+=boundary&&!evidence?'\n':' ';joined.owner.append(-1);}
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
// loose (#1894): one in four, for passwords and connection strings, which use symbols freely.
bool tokenPiece(const QString& word,bool loose=false) {
 if(word.isEmpty())return false;
 // An assignment is one, whatever its value's symbols (PASSWORD='R!$^&*()#', Sol on #1903).
 static const QRegularExpression assigned(QStringLiteral("^(?:\\d{1,5}|[{\\[(,]{1,3})?[\"'`\u201C\u2018]?[A-Za-z_][A-Za-z0-9_.-]{1,63}[\"'`\u201D\u2019]?[=:]\\S"));
 if(loose&&assigned.match(word).hasMatch())return true;
 // #1820: quotes around a word are punctuation, not misread characters ("“AKIA…" in an editor's string).
 static const QString quotes=QStringLiteral("\"'`\u201C\u201D\u2018\u2019\u00AB\u00BB");
 qsizetype from=0,to=word.size();
 while(from<to&&quotes.contains(word.at(from)))++from;
 while(to>from&&quotes.contains(word.at(to-1)))--to;
 if(from==to)return false;
 // Only around a token-like inside: 8 characters or more with a digit or a capital ("“note”" stays a word).
 {bool marked=false;for(qsizetype i=from;i<to;++i)marked=marked||word.at(i).isDigit()||word.at(i).isUpper();if(to-from<8||!marked){from=0;to=word.size();}}
 // Loose: closing quotes and brackets ending the word ("fox\"}") are a value's end, not noise (Sol on #1916).
 // A word of closers alone ("}]") is no piece: it would end a detached value's segment.
 static const QString closers=QStringLiteral("\"'`}])\u201D\u2019,;");
 if(loose&&std::all_of(word.cbegin(),word.cend(),[](QChar c){return closers.contains(c);}))return false;
 if(loose)while(to>from+1&&closers.contains(word.at(to-1)))--to;
 int noise=0;
 for(qsizetype i=from;i<to;++i){const auto c=word.at(i);if(!tokenCharacter(c)){if(c.isSpace())return false;++noise;}}
 return noise<=qMax(1,int(to-from)/(loose?4:8));
}
// Hex, as recognition misreads it: l, i, I, |, ! for 1; O, o, ©, ° for 0; S
// for 5; t and £ for f; € for e. At least 32 characters, at most one in twelve
// outside hex once mapped, and both digits and letters.
const QHash<QChar,QChar>& hexConfusions() {
 static const QHash<QChar,QChar> confused{{'l','1'},{'i','1'},{'I','1'},{'|','1'},{'!','1'},{'O','0'},{'o','0'},{QChar(0xa9),'0'},{QChar(0xb0),'0'},{'S','5'},{'t','f'},{QChar(0xa3),'f'},{QChar(0x20ac),'e'}};
 return confused;
}
bool hexRun(const QString& run) {
 const auto& confused=hexConfusions();
 if(run.size()<32)return false;
 int other=0,digits=0,letters=0,read=0;
 for(const auto raw:run){const auto c=confused.value(raw,raw);if(c>='0'&&c<='9')++digits;else if((c>='a'&&c<='f')||(c>='A'&&c<='F'))++letters;else ++other;if(c>='0'&&c<='9'&&raw!='o')++read;}
 // #1940: and one character in eight a digit other than a lowercase o: a receipt's leaders read as
 // "ccooecoo" are hex only through their o, while hex keeps its digits 2 to 9 however 0 and 1 are read.
 return other*12<=run.size()&&digits*5>=run.size()&&letters*10>=run.size()&&read*8>=run.size();
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
 // #1897: counted in an array (ASCII) and a hash for the rest: the fallback tries many windows.
 std::array<int,128> ascii{};QHash<QChar,int> other;int characters=0;
 for(const auto raw:run)if(raw.isLetterOrNumber()){const auto c=raw.toLower();if(c.unicode()<128)++ascii[c.unicode()];else ++other[c];++characters;}
 if(characters<24||characters*10<int(run.size())*9)return false;
 // #1940: what recognition makes of drawn noise is no token: its commonest letters as read (case kept,
 // up to four of them making 85 % of its letters, which are four in five of its letters and digits) all
 // from one of these: a meter's bars (htop and btop's [|||||] read as IIIITTTT EEE) or a receipt's dotted
 // leaders (ccooecoo, eeeneeee: small round letters, lowercase), a price or a percentage beside them;
 // a binary literal (0 and 1 85 % of it, or 60 % with a 0b literal in it: "mask = 0b1111…0000"); or one
 // two-letter syllable repeated (hahaha, OROROR). A token whose recognition collapsed (above) repeats
 // other letters: a, d, h, C ("oho aeaaaaaaeae aad", "APC CCEEEECEEEEC").
 {QString kept;QHash<QChar,int> exact;int letters=0,binary=0;
  for(const auto c:run)if(c.isLetterOrNumber()){kept+=c;if(c.isLetter()){++exact[c];++letters;}if(c=='0'||c=='1')++binary;}
  static const QRegularExpression literal(QStringLiteral("0b[01]{16,}"));
  if(binary*100>=kept.size()*85||(binary*10>=kept.size()*6&&literal.match(run).hasMatch()))return false;
  QList<QPair<int,QChar>> ranked;for(auto it=exact.cbegin();it!=exact.cend();++it)ranked.append({it.value(),it.key()});
  std::sort(ranked.begin(),ranked.end(),[](const auto& a,const auto& b){return a.first>b.first||(a.first==b.first&&a.second<b.second);});
  QString top;int share=0;for(const auto& [count,c]:std::as_const(ranked)){if(share*100>=letters*85||top.size()==4)break;top+=c;share+=count;}
  if(letters*5>=kept.size()*4&&share*100>=letters*85)for(const auto& set:{QStringLiteral("IlLiT|EFtfjJ"),QStringLiteral("ceosnmruvwt")})
   if(std::all_of(top.cbegin(),top.cend(),[&](QChar c){return set.contains(c);}))return false;
  const auto lower=kept.toLower();qsizetype alternating=0;
  for(qsizetype i=2;i<lower.size();++i)if(lower.at(i)==lower.at(i-2)&&lower.at(i)!=lower.at(i-1))++alternating;
  if(alternating*10>=(lower.size()-2)*8)return false;}
 QHash<QChar,int> counts=other;for(int i=0;i<128;++i)if(ascii[size_t(i)])counts.insert(QChar(i),ascii[size_t(i)]);
 // #1895: but a run almost all of mask characters (x or 0, nine in ten) is a placeholder, not a token: see placeholder().
 for(const QChar mask:{QChar('x'),QChar('0')})if(counts.value(mask)*10>=characters*9)return false;
 // ...or one that holds twenty x in a row, which no key does (a placeholder before prose)...
 {static const QRegularExpression xs(QStringLiteral("[xX]{20}"));if(xs.match(run).hasMatch())return false;}
 // ...or one after a prefix: the part after the last separator (sk_live_XXXX…) at least 16
 // characters, nine in ten of one mask character.
 {qsizetype cut=-1;for(const QChar c:{QChar('_'),QChar('-'),QChar('.'),QChar('='),QChar(':')})cut=qMax(cut,run.lastIndexOf(c));
  const auto tail=run.mid(cut+1);
  if(cut>=0&&tail.size()>=16)for(const QChar mask:{QChar('x'),QChar('0')})if(tail.count(mask,Qt::CaseInsensitive)*10>=tail.size()*9)return false;}
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
 // Letters in lowercase runs of five or more ([a-z]{5,}), counted in one pass.
 qsizetype lowercase=0,current=0;
 for(qsizetype i=0;i<=run.size();++i){const bool lower=i<run.size()&&run.at(i)>='a'&&run.at(i)<='z';if(lower)++current;else{if(current>=5)lowercase+=current;current=0;}}
 const auto count=[&](auto test){return qsizetype(std::count_if(run.cbegin(),run.cend(),test));};
 const auto capitals=count([](QChar c){return c.isUpper();}),numerals=count([](QChar c){return c.isDigit();});
 const auto stops=count([](QChar c){return c=='.'||c==':'||c==',';}),dashes=count([](QChar c){return c=='-';});
 return noise*10<=run.size()&&upper&&lower&&digit&&changes*3>=run.size()&&lowercase*10<=run.size()*3
  &&capitals*5>=run.size()&&numerals*5<=run.size()*2&&stops*80<=run.size()&&dashes*24<=run.size();
}
// #1895: a masked or placeholder value, eight characters or more of X, x, *, •, 0 or O alone (one
// misread character in eight allowed), shows no secret.
bool placeholder(const QString& body) {
 static const QString mask=QStringLiteral("Xx*\u2022\u00B70O");
 if(body.size()<8)return false;
 const auto other=std::count_if(body.cbegin(),body.cend(),[](QChar c){return !mask.contains(c);});
 return other*8<=body.size();
}
// #1894: a masked secret shows a run of four or more of one mask character that no key uses
// (sk-proj-••••a1b2, glptt-7******130f6), or six or more X or x (Z4CP2X4yXXXXXXXXICC==): what is
// left is meant to be seen. A key's own alphabet can hold four X in a row (Sol on #1903).
bool masked(const QString& text) {
 static const QRegularExpression run(QStringLiteral("(X{6,}|x{6,}|\\*{4,}|\u2022{4,}|\u00B7{4,}|\u25CF{4,})"));
 return run.match(text).hasMatch();
}
// Known prefixes, tolerant of a lost or misread separator and of the usual
// letter confusions, followed by a run of the token alphabet; and the nominal
// length of the whole token, which bounds how far the mark extends. A prefix
// counts only at the start of a recognized word, and when its separator was
// lost, only where the body begins a word of its own ("ghp" "x8Fq…", not
// "ghost…"). A JSON web token needs a dot (or a comma read for one).
// exact: the kind has one length (an AWS key ID, a classic GitHub token, a fine-grained one,
// a Stripe webhook secret); the others vary, and their length is only a bound.
// hex: the body is lowercase hex (a DigitalOcean or Shopify token), so inside a word it needs a digit
// and a letter rather than a capital.
struct Prefix {QRegularExpression pattern;int length;bool boundary;bool dotted=false;bool exact=false;bool hex=false;};
const QList<Prefix>& prefixes() {
 static const QList<Prefix> list{
  {QRegularExpression(R"((A[KA4][Il1|]A|AS[Il1|]A|ABIA|ACCA|AGPA|AIDA|AIPA|ANPA|ANVA|APKA|AROA)()([A-Z0-9]{12,}))"),20,false,false,true},
  {QRegularExpression(R"(([gaq]h[pousr])([_,.\-]?)([A-Za-z0-9]{24,}))"),40,true,false,true},
  {QRegularExpression(R"((github[_,.]?pat)([_,.]?)([A-Za-z0-9_]{20,}))"),93,true,false,true},
  // #1894: the l read as L, I or 1 ("g" "Lpat-…" in a dark editor).
  {QRegularExpression(R"((g[lLI1|](?:pat|dt|rt|ptt|ft|cbt|imt|oas|soat|agent))([-_.,]?)([A-Za-z0-9_-]{15,}))"),26,true},
  {QRegularExpression(R"((x[o0O][xX]{1,2}[abposr])([-_.,]?)([A-Za-z0-9-]{10,}))"),56,true},
  {QRegularExpression(R"((xapp)([-_.,]?)(\d[-_.,]?[A-Za-z0-9-]{10,}))"),100,true},
  // #1894: a publishable key (pk_live_, pk_test_) is public by design and stays visible.
  {QRegularExpression(R"(((?:sk|rk)[_,.]?(?:live|test))([_,.]?)([A-Za-z0-9]{10,}))"),40,true},
  {QRegularExpression(R"((whsec)([_,.]?)([A-Za-z0-9]{20,}))"),38,true,false,true},
  // #1894: more providers' distinctive prefixes, their shapes as gitleaks' default rules give them
  // (MIT, github.com/gitleaks/gitleaks), tolerant of a lost or misread separator as above.
  // OpenAI (sk-proj-, sk-svcacct-, sk-admin-) and Anthropic (sk-ant-api03-, sk-ant-admin01-).
  {QRegularExpression(R"((sk-(?:proj|svcacct|admin|ant-(?:api|admin)\d\d))([-_.,]?)([A-Za-z0-9_-]{20,}))"),164,true},
  // Google API keys, Hugging Face, npm and PyPI tokens.
  {QRegularExpression(R"((AIza)()([A-Za-z0-9_-]{30,}))"),39,false,false,true},
  {QRegularExpression(R"((hf)([_,.]?)([A-Za-z0-9]{30,}))"),37,true,false,true},
  {QRegularExpression(R"((npm)([_,.]?)([A-Za-z0-9]{30,}))"),40,true,false,true},
  {QRegularExpression(R"((pypi)([-_.,]?)(AgEIcHlwaS5vcmc[A-Za-z0-9_-]{20,}))"),180,true},
  // SendGrid: SG. then two dotted parts.
  {QRegularExpression(R"((SG)([.,])([A-Za-z0-9_-]{16,}[.,][A-Za-z0-9_-]{20,}))"),69,true},
  // DigitalOcean (dop_v1_, doo_v1_, dor_v1_) and Shopify (shpat_, shpca_, shppa_, shpss_): hex bodies.
  {QRegularExpression(R"((do[opr][_,.]?v1)([_,.]?)([a-fA-F0-9]{40,}))"),71,true,false,true,true},
  {QRegularExpression(R"((shp(?:at|ca|pa|ss))([_,.]?)([a-fA-F0-9]{24,}))"),38,true,false,true,true},
  // Twilio API key SIDs (SK and 32 hex) and Mailgun keys (key- and 32 hex).
  {QRegularExpression(R"((SK)()([a-f0-9]{32}))"),34,false,false,true,true},
  {QRegularExpression(R"((key)(-)([a-f0-9]{32}))"),36,true,false,true,true},
  // Square access tokens, Docker Hub, Grafana, HashiCorp Vault, Postman, New Relic, Linear, Atlassian
  // and Sentry tokens.
  {QRegularExpression(R"((EAAA)()([A-Za-z0-9_-]{50,}))"),64,false},
  {QRegularExpression(R"((dckr[_,.]?pat)([_,.]?)([A-Za-z0-9_-]{20,}))"),36,true},
  {QRegularExpression(R"((gl(?:sa|c))([_,.]?)([A-Za-z0-9_+/=-]{30,}))"),46,true},
  {QRegularExpression(R"((hv[sbr])([.,])([A-Za-z0-9_-]{40,}))"),120,true},
  {QRegularExpression(R"((PMAK)(-)([a-fA-F0-9]{20,}-?[a-fA-F0-9]{20,}))"),64,true},
  {QRegularExpression(R"((NRAK)(-)([A-Z0-9]{20,}))"),31,true},
  {QRegularExpression(R"((lin[_,.]?api)([_,.]?)([A-Za-z0-9]{30,}))"),48,true},
  {QRegularExpression(R"((ATATT)()([A-Za-z0-9_=-]{40,}))"),192,false},
  {QRegularExpression(R"((sntry[su])([_,.]?)([A-Za-z0-9_=+/-]{30,}))"),100,true},
  // A JSON web token's header is base64url of "{"…", so it starts with eyJ.
  // #1875: a text caret just left of the token reads as a character joined before it
  // ("leyJhbGci…", "|eyJ…", "JeyJ…" in the corpus), or merged with its e into a k or b (kyJ…
  // behind a browser field's caret, byJ… behind Kate's); that reading is taken as the prefix
  // only before a whole "yJ", which no word holds.
  {QRegularExpression(R"(((?:[lI|1!jJ[{](?=eyJ))?ey[JjIl1)\]}|]|[kb]yJ)()([A-Za-z0-9_\-.,|!\]})]{24,}))"),400,false,true},
 };
 return list;
}
// Whether a word is a known prefix's head on its own ("AKIA", "ghp", "glpat"): the prefix pattern takes
// the whole word as its head before a body.
bool prefixHead(const QString& word) {
 for(const auto& prefix:prefixes()){
  const auto m=prefix.pattern.match(word+QString(48,'A')+QLatin1String(".AAAAAAAAAAAAAAAAAAAAAAAAAAAA"),0,QRegularExpression::NormalMatch,QRegularExpression::AnchorAtOffsetMatchOption);
  if(m.hasMatch()&&m.capturedLength(1)+m.capturedLength(2)>=word.size())return true;
 }
 return false;
}
// heads (#1897): for each prefix, where its head (the pattern's first group) occurs in the segment's
// text or its bodies view, sorted; filled once per segment, so a prefix whose head is absent costs nothing.
// counts (#1897): running totals over the text of capitals, lowercase, digits, letters and digits, and
// characters that are not hex once recognition's confusions are mapped, so a window can be ruled out
// for the run tests in constant time before they read it.
struct Counts {QList<int> upper,lower,digit,alnum,nonHex,hexDigit;};
struct Segment {QList<int> words;QString text;QList<int> starts;mutable QList<QList<int>> heads;mutable bool headsFound=false;mutable Counts counts;};
void findHeads(const Segment& segment);
// A mark in a segment, by piece; a prefixed token also keeps where its prefix
// starts in the segment's text and its nominal length (0 for a hex or base64 run).
// payload: a JSON web token found by its payload, its header misread or on the line above.
struct Found {int from=0;int to=0;int prefix=-1;int length=0;bool dotted=false;bool payload=false;};
// The token segments of one line: maximal runs of token pieces, joined.
// #1895: a gap of more than six characters between two pieces ends a segment. Recognition splits a
// token with gaps of a word space, or a few characters where it dropped some (a JWT on the corpus:
// three and a third), while a table's label cell ("Git SHA", "AWS") sits a column away from its value
// (14 and 24 characters on Patrick's decoy page).
bool prefixHead(const QString& word);
// skip (#1940): words that are public by what the screen shows around them (publicWords()) end a
// segment as a word outside the token alphabet does.
QList<Segment> segments(const QList<int>& line,const QList<QString>& plain,const QList<RecognizedWord>& candidates,bool loose=false,const QSet<int>* skip=nullptr) {
 QList<Segment> out;Segment current;
 const auto close=[&]{if(!current.words.isEmpty())out.append(current);current={};};
 const auto glyph=[&](int w){return double(candidates.at(w).rect.width())/qMax<qsizetype>(1,plain.at(w).size());};
 for(const int index:line){
  if((skip&&skip->contains(index))||!tokenPiece(plain.at(index),loose)){close();continue;}
  if(!current.words.isEmpty()){
   const int previous=current.words.last();
   const int gap=candidates.at(index).rect.left()-candidates.at(previous).rect.right();
   // After a plain label word (letters alone, AWS, Key) three characters already part it from a
   // value; between token pieces six (Sol on #1901).
   const auto& left=plain.at(previous);
   // ...unless it is a key's own prefix, split from its body (AKIA, ghp; Sol on #1901).
   // A prefix head after a separator in the word counts too (ID:AKIA; #1914).
   const auto tail=left.mid(qMax(left.lastIndexOf(':'),left.lastIndexOf('='))+1);
   const bool label=left.size()<=12&&std::all_of(left.cbegin(),left.cend(),[](QChar c){return c.isLetter()||c==':';})&&!prefixHead(left)&&!(tail.size()<left.size()&&prefixHead(tail));
   if(gap>(label?3:6)*qMax(glyph(previous),glyph(index)))close();
  }
  current.starts.append(int(current.text.size()));current.words.append(index);current.text+=plain.at(index);
 }
 close();return out;
}
int pieceAt(const Segment& segment,qsizetype offset) {
 const auto it=std::upper_bound(segment.starts.cbegin(),segment.starts.cend(),int(offset));
 return qMax(0,int(it-segment.starts.cbegin())-1);
}
bool pieceStart(const Segment& segment,int offset) {return std::binary_search(segment.starts.cbegin(),segment.starts.cend(),offset);}
// #1897: one combined pattern of every prefix's head, built once: its first group, read from the
// pattern (escapes and character classes skipped while counting brackets).
QString headOf(const QString& pattern) {
 int depth=0;bool inClass=false;
 for(qsizetype i=0;i<pattern.size();++i){
  const auto c=pattern.at(i);
  if(c=='\\'){++i;continue;}
  if(inClass){if(c==']')inClass=false;continue;}
  if(c=='[')inClass=true;
  else if(c=='(')++depth;
  else if(c==')'&&--depth==0)return pattern.mid(1,i-1);
 }
 return pattern;
}
// A subject made valid UTF-16 once (a lone surrogate becomes U+FFFD, the same length), so the many
// searches over it skip Qt's check of the whole string each time (Sol on #1902).
QString wholeText(QString text) {
 for(qsizetype k=0;k<text.size();++k){
  const bool paired=(text.at(k).isHighSurrogate()&&k+1<text.size()&&text.at(k+1).isLowSurrogate())||(text.at(k).isLowSurrogate()&&k>0&&text.at(k-1).isHighSurrogate());
  if(text.at(k).isSurrogate()&&!paired)text[k]=QChar(QChar::ReplacementCharacter);
 }
 return text;
}
constexpr auto unchecked=QRegularExpression::DontCheckSubjectStringMatchOption;
// The segment's text as the prefix bodies read it: a misread character as x, and a slash between two
// letters or digits (#1839).
QString bodiesOf(const QString& text) {
 QString bodies=text;
 for(qsizetype i=0;i<bodies.size();++i){
  auto& c=bodies[i];
  if(!tokenCharacter(c))c='x';
  else if(c=='/'&&i>0&&i+1<text.size()&&text.at(i-1).isLetterOrNumber()&&text.at(i+1).isLetterOrNumber())c='x';
 }
 return bodies;
}
void findHeads(const Segment& segment) {
 if(segment.headsFound)return;
 segment.headsFound=true;
 const auto& list=prefixes();
 static const QRegularExpression combined=[&]{
  QStringList parts;for(const auto& prefix:list)parts.append(QLatin1Char('(')+headOf(prefix.pattern.pattern())+QLatin1Char(')'));
  return QRegularExpression(parts.join('|'));
 }();
 // A combined pattern that does not compile filters nothing: every prefix is then searched.
 if(!combined.isValid()||combined.captureCount()!=list.size())return;
 segment.heads=QList<QList<int>>(list.size());
 // Each prefix's head alone, to test every head at a place the combined pattern found one: where
 // two heads start together (glcbt and glc), the first alternative must not hide the other.
 static const QList<QRegularExpression> each=[&]{QList<QRegularExpression> out;for(const auto& prefix:list)out.append(QRegularExpression(headOf(prefix.pattern.pattern())));return out;}();
 const auto text=wholeText(segment.text);const auto bodies=wholeText(bodiesOf(segment.text));
 for(const QString* subject:{&text,&bodies}){
  // Every start, overlapping heads too: a match moves the search on by one character.
  for(qsizetype at=0;at<subject->size();){
   const auto m=combined.match(*subject,at,QRegularExpression::NormalMatch,unchecked);if(!m.hasMatch())break;
   const auto start=m.capturedStart();
   for(int i=0;i<list.size();++i)if(each.at(i).match(*subject,start,QRegularExpression::NormalMatch,QRegularExpression::AnchorAtOffsetMatchOption|unchecked).hasMatch())segment.heads[i].append(int(start));
   at=start+1;
  }
 }
 for(auto& positions:segment.heads){std::sort(positions.begin(),positions.end());positions.erase(std::unique(positions.begin(),positions.end()),positions.end());}
}
// Marks found in one segment: prefixed tokens first, then the longest hex or
// base64 run of whole pieces, then whatever is left either side of it.
void segmentMatches(const Segment& segment,int first,int last,QList<Found>& found) {
 if(first>last)return;
 const auto text=segment.text.mid(segment.starts.at(first),(last+1<segment.starts.size()?segment.starts.at(last+1):segment.text.size())-segment.starts.at(first));
 // A misread character inside a body reads as a letter for the body's sake,
 // and so does a slash between two letters or digits (#1839: putgd-ocr read
 // the J of "h J9kL" as "/" in Noto Sans and Liberation Serif at 9 and 10 px).
 const int base=segment.starts.at(first);
 findHeads(segment);
 // Built only once a prefix's head is in range: most parts of a long line have none.
 QString checkedText,bodies;bool views=false;
 for(int index=0;index<prefixes().size();++index){
  const auto& prefix=prefixes().at(index);
  // #1897: only a prefix whose head occurs in this part of the segment is searched for.
  if(!segment.heads.isEmpty()){
   const auto& at=segment.heads.at(index);
   const auto it=std::lower_bound(at.cbegin(),at.cend(),base);
   if(it==at.cend()||*it>=base+text.size())continue;
  }
  if(!views){checkedText=wholeText(text);bodies=wholeText(bodiesOf(text));views=true;}
  QRegularExpressionMatch match;
  // Each start is tried in turn: a rejected candidate ("gho" in "ghost…") must not swallow a real
  // prefix inside its body, so the search resumes one character after it.
  const auto& subject=prefix.dotted?checkedText:bodies;
  for(qsizetype next=0;next<subject.size();){
   const auto candidate=prefix.pattern.match(subject,next,QRegularExpression::NormalMatch,unchecked);
   if(!candidate.hasMatch())break;
   next=candidate.capturedStart()+1;
   // A prefix starts a piece, or follows a separator inside one (#1820: KEY_ID=AKIA…, "AKIA…", https://ghp_…@host).
   const int at=base+int(candidate.capturedStart());
   // Curly quotes too: an editor's string literal reads as "“AKIA…" (#1820).
   static const QString separators=QStringLiteral("=:/@\"'([`\u201C\u201D\u2018\u2019\u00AB\u00BB");
   const bool afterSeparator=!pieceStart(segment,at);
   // #1940: an AWS key ID's prefix read across two words (recognition splits "AK" "IA…" often) is a key,
   // except a card receipt's label and value: the word "AID" alone, then a card AID as printed and a
   // body half 0, 1, 8 and 9 ("AID" "A0000000031010" joined into AIDA…).
   if(index==0&&pieceAt(segment,at)!=pieceAt(segment,at+int(candidate.capturedLength(1))-1)&&[&]{const int p=pieceAt(segment,at);const int e=p+1<segment.starts.size()?segment.starts.at(p+1):int(segment.text.size());if(segment.text.mid(segment.starts.at(p),e-segment.starts.at(p))!=QLatin1String("AID"))return false;
    // ...and the next word a card AID as printed: A, then digits (0 as read: O, Q, D) and nothing else.
    static const QRegularExpression aid(QStringLiteral("^A[0-9OQD]{9,31}$"));
    const int q=p+1;if(q>=segment.starts.size())return false;
    const int f=q+1<segment.starts.size()?segment.starts.at(q+1):int(segment.text.size());
    return aid.match(segment.text.mid(segment.starts.at(q),f-segment.starts.at(q))).hasMatch();}()){
    const auto body=candidate.captured(3).left(16);
    if(std::count_if(body.cbegin(),body.cend(),[](QChar c){return c=='0'||c=='1'||c=='8'||c=='9';})*2>=body.size())continue;
   }
   // #1875: a JSON web token's payload starts with eyJ after the header's dot, so a token whose
   // header recognition misread past the prefix ("|eyfhbGci….eyJzdWIi…") is found by its payload.
   const bool payload=prefix.dotted&&at>0&&(segment.text.at(at-1)=='.'||segment.text.at(at-1)==',');
   if(afterSeparator&&!payload&&!(at>0&&separators.contains(segment.text.at(at-1))))continue;
   // Inside a word, a token's body has a digit and a capital, as real keys do; a URL slug
   // (/glagent-installation-guide, “glagent-installation-guide-v2”) has neither or only a digit.
   // So does a JSON web token read with a stray character before it, which shares a word with
   // the prefix and two more characters ("ley" "January" is two words).
   const bool stray=prefix.dotted&&!candidate.captured(1).startsWith(QLatin1String("ey"));
   if(stray&&pieceAt(segment,at+int(candidate.capturedLength(1))+1)!=pieceAt(segment,at))continue;
   if(afterSeparator||stray){const auto body=candidate.captured(3);if(!hasDigit(body)||std::none_of(body.cbegin(),body.cend(),[&](QChar c){return prefix.hex?c.isLetter():c.isUpper();}))continue;}
   // And both read as a JWT part up to the next dot: base64url of JSON, at least 12 characters,
   // a fifth of them capitals and few long lowercase runs, not a dotted name ("archive.eyJanuary2026.pdf").
   if(payload||stray){
    const auto body=candidate.captured(3);const auto part=body.left(body.indexOf(QRegularExpression(QStringLiteral("[.,]"))));
    static const QRegularExpression words(QStringLiteral("[a-z]{5,}"));
    qsizetype lowercase=0;for(auto it=words.globalMatch(part);it.hasNext();)lowercase+=it.next().capturedLength();
    if(part.size()<12||std::count_if(part.cbegin(),part.cend(),[](QChar c){return c.isUpper();})*5<part.size()||lowercase*10>part.size()*3)continue;
   }
   if(prefix.boundary&&candidate.capturedLength(2)==0&&!pieceStart(segment,base+int(candidate.capturedStart(3))))continue;
   // #1895: a placeholder ("sk_live_XXXXXXXX…", "AKIAXXXXXXXXXXXXXXXX") is no secret: a body, up to the
   // kind's length, of mask characters alone.
   // A kind of one length is judged on its body up to that length (a word can run on past it); any
   // other on its whole body, so an unmasked tail keeps it a key (Sol on #1901).
   {const auto body=candidate.captured(3);
    // ...and never past the piece it starts in: the body pattern runs on through the words joined
    // after it ("…XXXX" "was" "a" "redacted" "example"; Sol on #1901).
    const int bodyAt=base+int(candidate.capturedStart(3)),piece=pieceAt(segment,bodyAt);
    const int pieceEnd=piece+1<segment.starts.size()?segment.starts.at(piece+1):int(segment.text.size());
    const auto own=body.left(qMax(0,pieceEnd-bodyAt));
    const auto judged=prefix.exact?own.left(qMax(8,prefix.length-int(candidate.capturedLength(1)+candidate.capturedLength(2)))):own;
    if(placeholder(judged)||masked(judged))continue;}
   if(prefix.dotted&&!candidate.captured(3).contains('.')&&!candidate.captured(3).contains(','))continue;
   match=candidate;break;
  }
  if(!match.hasMatch())continue;
  const int start=pieceAt(segment,base+match.capturedStart());
  int end=start;
  if(prefix.exact){
   // A kind of one length ends there (#1839): its pieces are those that start before it has its
   // length, counted from the prefix (which can follow a label in its word, "KEY_ID=AKIA…"), and
   // a piece of one or two characters just past it, which a doubled character leaves. The body
   // pattern runs on through the words joined after a whole token ("ghp_…" "was" "leaked"), so
   // its end is not the token's; pieces after the core are kept or left out as words at a
   // segment's end.
   const int from=base+int(match.capturedStart());
   const auto size=[&](int piece){return (piece+1<segment.starts.size()?segment.starts.at(piece+1):int(segment.text.size()))-segment.starts.at(piece);};
   while(end<last){
    const int offset=segment.starts.at(end+1)-from;
    if(offset<prefix.length||(offset<prefix.length+2&&size(end+1)<=2))++end;else break;
   }
  }else{
   // Otherwise the pieces up to half again its usual length, and all the body pattern took.
   while(end<last&&segment.starts.at(end+1)-segment.starts.at(start)<prefix.length*3/2)++end;
   end=qMax(end,pieceAt(segment,base+match.capturedEnd()-1));
  }
  segmentMatches(segment,first,start-1,found);const int at=base+int(match.capturedStart());
  const bool payload=prefix.dotted&&at>0&&(segment.text.at(at-1)=='.'||segment.text.at(at-1)==',');
  found.append({start,end,at,prefix.length,prefix.dotted,payload});segmentMatches(segment,end+1,last,found);return;
 }
 // Runs of up to 256 characters are tried, so a long line costs little on the
 // interface thread; a longer blob passes on a part of it and is marked whole.
 // Necessary conditions of hexRun, base64Run and repeatedRun from the running totals: a window that
 // fails all three is not read.
 if(segment.counts.upper.isEmpty()){
  auto& c=segment.counts;const auto n=segment.text.size();
  for(auto* list:{&c.upper,&c.lower,&c.digit,&c.alnum,&c.nonHex,&c.hexDigit})list->fill(0,n+1);
  for(qsizetype i=0;i<n;++i){
   const auto ch=segment.text.at(i);const auto mapped=hexConfusions().value(ch,ch);
   const bool hex=(mapped>='0'&&mapped<='9')||(mapped>='a'&&mapped<='f')||(mapped>='A'&&mapped<='F');
   c.upper[i+1]=c.upper[i]+(ch.isUpper()?1:0);c.lower[i+1]=c.lower[i]+(ch.isLower()?1:0);c.digit[i+1]=c.digit[i]+(ch.isDigit()?1:0);
   c.alnum[i+1]=c.alnum[i]+(ch.isLetterOrNumber()?1:0);c.nonHex[i+1]=c.nonHex[i]+(hex?0:1);
   // hexRun counts digits after recognition's confusions (l, |, O… read as digits; Sol on #1902).
   c.hexDigit[i+1]=c.hexDigit[i]+(mapped>='0'&&mapped<='9'?1:0);
  }
 }
 const auto possible=[&](int from,int to){
  const auto& c=segment.counts;const int size=to-from;
  const int upper=c.upper[to]-c.upper[from],lower=c.lower[to]-c.lower[from],digits=c.digit[to]-c.digit[from];
  const bool hex=size>=32&&(c.nonHex[to]-c.nonHex[from])*12<=size&&(c.hexDigit[to]-c.hexDigit[from])*5>=size;
  const bool base64=size>=32&&upper>0&&lower>0&&digits>0&&upper*5>=size&&digits*5<=size*2;
  const bool repeated=(c.alnum[to]-c.alnum[from])>=24&&(c.alnum[to]-c.alnum[from])*10>=size*9;
  return hex||base64||repeated;
 };
 // #1897: each piece looks only as far as 256 characters reach (the window's end moves on with it), so
 // a line of thousands of pieces costs about its length, not its square.
 int bestFirst=-1,bestLast=-1,bestLength=0;
 const auto endOf=[&](int j){return j+1<segment.starts.size()?segment.starts.at(j+1):int(segment.text.size());};
 int reach=first;
 for(int i=first;i<=last;++i){
  reach=qMax(reach,i);
  while(reach<last&&endOf(reach+1)-segment.starts.at(i)<=256)++reach;
  for(int j=reach;j>=i;--j){
  const int from=segment.starts.at(i),to=endOf(j);
  if(to-from>256&&j>i)continue;
  // No run test passes under 24 characters (a repeated run's least), and none beats the best so far.
  if(to-from<=bestLength||to-from<24)break;
  if(!possible(from,to))continue;
  const auto run=segment.text.mid(from,to-from);
  if(hexRun(run)||base64Run(run)||repeatedRun(run)){bestFirst=i;bestLast=j;bestLength=to-from;break;}
  }
 }
 if(bestFirst<0)return;
 segmentMatches(segment,first,bestFirst-1,found);found.append({bestFirst,bestLast,-1,0});segmentMatches(segment,bestLast+1,last,found);
}
bool otherMatch(const QList<int>& words,const QList<QString>& plain,const QList<RecognizedWord>& candidates,QSet<int>* cards=nullptr);
// #1894: a hex or base64 run that is public or no secret by its shape: a subresource integrity digest
// (sha512-…), an Ethereum address (0x and 40 hex; a private key has 64) or a Twilio account SID (AC and
// 32 hex, an identifier).
bool publicRun(const QString& run) {
 static const QRegularExpression shapes(QStringLiteral("(?:^|[^A-Za-z0-9])(?:sha(?:256|384|512)-[A-Za-z0-9+/]|0x[0-9a-fA-F]{40}(?![0-9a-fA-F])|AC[0-9a-f]{32}(?![0-9a-f]))"));
 if(shapes.match(run).hasMatch())return true;
 // #1940: from the run's start, or after a label's ":" or "=" there: a W3C traceparent (version, trace
 // ID, span ID and flags, nothing after them).
 static const QRegularExpression anchored(QStringLiteral("^(?:[A-Za-z_-]{1,24}[:=])?[\"']?0[0-9a-f]-[0-9a-f]{32}-[0-9a-f]{16}-0[0-9a-f][\"',;]*$"));
 return anchored.match(run).hasMatch();
}
// A JSON web token that carries no signature: its header decodes to JSON with "alg":"none" and its
// signature part is empty (Sol on #1903). Anything recognition garbled is no such token.
bool unsignedJwt(const QString& text) {
 // The token itself, out of its quotes and the punctuation or words after it (Sol on #1903).
 static const QRegularExpression span(QStringLiteral("ey[A-Za-z0-9_-]+\\.[A-Za-z0-9_-]*\\.[A-Za-z0-9_-]*"));
 const auto m=span.match(text);if(!m.hasMatch())return false;
 const auto parts=m.captured().split('.');
 if(parts.size()!=3||!parts.at(2).isEmpty())return false;
 // The header's own alg, top level, is "none".
 const auto header=QJsonDocument::fromJson(QByteArray::fromBase64(parts.at(0).toLatin1(),QByteArray::Base64UrlEncoding));
 return header.isObject()&&header.object().value(QLatin1String("alg")).toString().compare(QLatin1String("none"),Qt::CaseInsensitive)==0;
}
// Whether a line names a digest (a part of one of its words, split at anything but a letter or digit
// and at a lowercase-to-capital change, is a hash word) and no secret.
bool digestLine(const QList<int>& line,const QList<QString>& plain) {
 static const QSet<QString> hashes{"sha","sha1","sha224","sha256","sha384","sha512","md5","commit","digest","checksum","hash","etag","integrity","blake2b","blake3",
  // #1940: the checksum tools' own names (MD5sum: in an ISO's page, sha256sum in its command).
  "md5sum","sha1sum","sha224sum","sha256sum","sha384sum","sha512sum","shasum","b2sum","b3sum","cksum"};
 static const QSet<QString> secrets{"secret","key","token","password","passwd","pwd","signing","private","hmac","auth","credential","credentials","apikey","api"};
 // A secret word counts inside a camelCase name too (clientSecret, clientAPISecret); a hash word only
 // as a word of its own, so a token's tail such as "shaB7m2…" is no label (Sol on #1901).
 static const QRegularExpression words(QStringLiteral("[^A-Za-z0-9]+")),camel(QStringLiteral("[^A-Za-z0-9]+|(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])"));
 for(const int w:line)for(const auto& part:plain.at(w).split(camel,Qt::SkipEmptyParts))if(secrets.contains(part.toLower()))return false;
 bool hash=false;
 // A camelCase label in label position, ending in ":" or "=" (gitSha:, commitHash=), names a digest too.
 // ...with a value after it on the line, so a token's tail "shaB7m2QaK8V=" is no label.
 // The value after it must look like a digest: hex of 7 or more, or sha…, not an inline comment (#1914).
 static const QRegularExpression digestValue(QStringLiteral("^[\"']?(?:[0-9a-fA-F]{7,}|sha[0-9]*[-:])"));
 for(int i=0;i+1<line.size();++i){const auto& t=plain.at(line.at(i));if((t.endsWith(':')||t.endsWith('='))&&digestValue.match(plain.at(line.at(i+1))).hasMatch())for(const auto& part:t.split(camel,Qt::SkipEmptyParts))hash=hash||hashes.contains(part.toLower());}
 for(const int w:line)for(auto part:plain.at(w).split(words,Qt::SkipEmptyParts)){
  part=part.toLower();
  if(secrets.contains(part))return false;
  hash=hash||hashes.contains(part);
 }
 return hash;
}
// #1894: whether an assignment's name names a secret, by its parts (split at anything but a letter or
// digit and between a lowercase letter and a capital): TOKEN, SECRET, PASSWORD, PASSWD, PWD, PASS,
// PASSPHRASE, CREDENTIAL(S), DSN, APIKEY, or API, ACCESS, SECRET, PRIVATE, SIGNING, ENCRYPTION,
// MASTER or ACCOUNT before KEY, CLIENT or APP before SECRET, AUTH before TOKEN, and AUTHORIZATION. A
// name whose last part says it is a setting about the secret (PASSWORD_MIN_LENGTH, TOKEN_TTL,
// SECRET_FILE) is not, and nor is the shell's PWD.
bool secretName(const QString& name) {
 QStringList parts;QString part;
 for(qsizetype i=0;i<name.size();++i){
  const auto c=name.at(i);
  if(!c.isLetterOrNumber()){if(!part.isEmpty())parts.append(part.toLower());part.clear();continue;}
  // A lowercase-to-capital change, and an acronym's end before a word (APIKey, DBPassword; Sol on #1903).
  const bool acronymEnd=c.isUpper()&&i>0&&name.at(i-1).isUpper()&&i+1<name.size()&&name.at(i+1).isLower();
  if(((c.isUpper()&&i>0&&name.at(i-1).isLower())||acronymEnd)&&!part.isEmpty()){parts.append(part.toLower());part.clear();}
  part+=c;
 }
 if(!part.isEmpty())parts.append(part.toLower());
 if(parts.isEmpty())return false;
 static const QSet<QString> settings{"ttl","length","len","min","max","timeout","expiry","expires","expiration","file","path","dir","enabled","enable","disabled","header","type","mode","count","id","name","user","username","host","port","size","prefix","format","version","field","rotation","policy","required","hint","days","age","lifetime","endpoint","url","uri","hash","algorithm","alg","strength","reset","manager","store","provider","source","env","var","label","placeholder","length"};
 if(settings.contains(parts.last()))return false;
 // A public value says so: NEXT_PUBLIC_…, MAPBOX_PUBLIC_TOKEN, PUBLISHABLE_KEY, PUBLIC_KEY; never a
 // password or a private one (PUBLIC_DB_PASSWORD; Sol on #1903).
 static const QSet<QString> privateParts{"password","passwords","passwd","pwd","pass","passphrase","private","privatekey"};
 const bool privatePart=std::any_of(parts.cbegin(),parts.cend(),[](const QString& p){return privateParts.contains(p);});
 if(!privatePart&&(parts.contains(QLatin1String("public"))||parts.contains(QLatin1String("publishable"))))return false;
 if(parts.size()==1&&(parts.first()=="pwd"||parts.first()=="oldpwd"))return false;
 static const QSet<QString> alone{"token","tokens","secret","secrets","password","passwords","passwd","pwd","pass","passphrase","pin","credential","credentials","dsn","apikey","privatekey","accesskey","secretkey","authtoken","authorization"};
 static const QSet<QString> beforeKey{"api","access","secret","private","signing","encryption","master","account","app"};
 for(int i=0;i<parts.size();++i){
  if(alone.contains(parts.at(i)))return true;
  if(i+1<parts.size()&&parts.at(i+1)=="key"&&beforeKey.contains(parts.at(i)))return true;
  if(i+1<parts.size()&&parts.at(i+1)=="secret"&&(parts.at(i)=="client"||parts.at(i)=="app"))return true;
  if(i+1<parts.size()&&parts.at(i+1)=="token"&&parts.at(i)=="auth")return true;
 }
 return false;
}
// The value of a secret-named assignment, quotes and a trailing comma or semicolon taken off, shows a
// secret unless it is empty or under four characters, digits alone, a placeholder (placeholder()), a
// reference to one (${…}, $(…), {{…}}, <…>, %…%, $NAME) or a word that is no secret (true, none,
// required…).
// Whether a name is a password's (a PIN too): its value may be digits alone.
bool passwordName(const QString& name) {
 const auto lower=name.toLower();
 return lower.contains(QLatin1String("pass"))||lower.contains(QLatin1String("pwd"))||lower.contains(QLatin1String("pin"));
}
// numeric: a value of digits alone counts (a password-family name, a URL's password; Sol on #1903).
// name (#1940): the setting's name, for the checks that only a setting's value takes; empty for a URL's
// password.
// declaration (#1940): the value follows a name and a ":" unquoted, as a type follows a field's name.
bool secretValue(QString value,bool numeric=false,const QString& name={},bool declaration=false) {
 static const QString quotes=QStringLiteral("\"'`“”‘’");
 const bool quoted=!value.isEmpty()&&quotes.contains(value.front());
 while(!value.isEmpty()&&(quotes.contains(value.back())||value.back()==','||value.back()==';'))value.chop(1);
 while(!value.isEmpty()&&quotes.contains(value.front()))value.remove(0,1);
 if(value.size()<4||placeholder(value)||masked(value))return false;
 if(!numeric&&std::all_of(value.cbegin(),value.cend(),[](QChar c){return c.isDigit();}))return false;
 if(unsignedJwt(value))return false;
 // A reference names where the secret is kept: ${…}, $(…), {{…}}, <…>, %…%, $NAME, process.env.X,
 // os.environ[…], ENV['…'], getenv(…), a 1Password op:// or Vault reference, a cloud secret's ARN.
 static const QRegularExpression reference(QStringLiteral("^(?:\\$[({]|\\{\\{|<|%[A-Za-z_][A-Za-z0-9_]*%$|\\$[A-Za-z_][A-Za-z0-9_]*$|process\\.env\\b|os\\.environ|os\\.getenv|ENV\\[|(?:System\\.)?getenv\\(|env\\(|op://|vault:|arn:aws:)"),QRegularExpression::CaseInsensitiveOption);
 if(reference.match(value).hasMatch())return false;
 // #1940: a setting's value (never a URL's password) shows no secret when it is a whole call that reads
 // the secret by name from where it is kept, with nothing after it but error handling
 // (std::env::var("DB_PASSWORD")?;, os.Getenv("API_KEY"), never …unwrap_or("hunter2")), a type in a
 // declaration (api_key: Option<String>, password: string; never API_KEY=Bytes or "bytes"), or, for
 // a count of tokens a model reports (total_tokens: 12,450), an unquoted number with thousands separators.
 if(!name.isEmpty()&&!quoted){
  static const QRegularExpression code(QStringLiteral("^(?:(?:std::)?env::var(?:_os)?|os\\.(?:Getenv|LookupEnv)|Environment\\.GetEnvironmentVariable|ENV\\.fetch|Deno\\.env\\.get)\\([\"'][A-Za-z_][A-Za-z0-9_]*[\"']\\)(?:\\.unwrap\\(\\)|\\.expect\\([\"'][^\"']*[\"']\\))?[?;,)]*$|^import\\.meta\\.env\\.[A-Za-z_][A-Za-z0-9_]*[;,]?$"));
  if(code.match(value).hasMatch())return false;
  static const QRegularExpression type(QStringLiteral("^&?(?:'static )?(?:(?:Option|Optional|Vec|Box|Arc|Rc|Cow|Secret|SecretBox|Result|List|Mapping)<[A-Za-z0-9_&<>:', ]+>|String|str|string|bytes|Bytes|SecretString|SecretStr|QString|std::string|char\\*|int|number|bool|any|object)[?]?[,;]?$"));
  if(declaration&&type.match(value).hasMatch())return false;
  static const QRegularExpression thousands(QStringLiteral("^\\d{1,3}(?:,\\d{3})+$")),counted(QStringLiteral("(?i)^(?:[a-z0-9]+[_.-])*(?:total|prompt|completion|max|input|output|cached|reasoning)[_.-]?tokens$"));
  if(!numeric&&counted.match(name).hasMatch()&&thousands.match(value).hasMatch())return false;
 }
 // A URL with no password in it (redis://localhost:6379/0) is an address, not a secret; one with a
 // password is matched as a URL.
 // A URL with a key in its userinfo (a Sentry DSN, https://<key>@host/1) is a secret (acceptance on #1903).
 // ...but not a username alone (postgresql://reporting@localhost): the userinfo must look like a key, 12
 // characters or more with a digit.
 static const QRegularExpression address(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]{1,24}://(?!(?=[^\\s/?#@:]*\\d)[^\\s/?#@:]{12,}@)"));
 if(address.match(value).hasMatch())return false;
 static const QSet<QString> words{"true","false","null","none","nil","yes","no","on","off","required","optional","undefined","empty","redacted","hidden","secret","password","token","string","example","changeme"};
 if(words.contains(value.toLower()))return false;
 // A placeholder written out, made of filler words alone: YOUR_TOKEN_HERE, your-api-key-here,
 // not-a-real-token. One filler word inside a real value (H7-example-r9F4) does not make it one.
 static const QSet<QString> filler{"your","here","example","placeholder","changeme","dummy","fake","redacted","replace","insert","sample","not","a","real","the","my","api","key","token","secret","password","access","value","goes","put","this","will","work","test"};
 static const QSet<QString> marker{"your","here","example","placeholder","changeme","dummy","fake","redacted","replace","insert","sample"};
 const auto lower=value.toLower();
 {const auto parts=lower.split(QRegularExpression(QStringLiteral("[^a-z0-9]+")),Qt::SkipEmptyParts);
  const bool allFiller=!parts.isEmpty()&&std::all_of(parts.cbegin(),parts.cend(),[](const QString& p){return filler.contains(p);});
  const bool notReal=lower.contains(QLatin1String("not-a-real"))||lower.contains(QLatin1String("not_a_real"));
  if(allFiller&&(notReal||std::any_of(parts.cbegin(),parts.cend(),[](const QString& p){return marker.contains(p);})))return false;}
 // A publishable key is public by design.
 static const QRegularExpression publishable(QStringLiteral("^(?:pk[_.](?:live|test)_|pk\\.ey|sb_publishable_)"));
 if(publishable.match(value).hasMatch())return false;
 // A known prefix with far too short a body is a near miss, not a key (token='ghp_t37p9f').
 static const QRegularExpression nearMiss(QStringLiteral("^(?:gh[pousr]_|github_pat_|glpat-|xox[a-z]-|sk_live_|sk_test_|rk_live_|sk-(?:proj|ant|admin|svcacct)-?|AIza|AKIA|ASIA|hf_|npm_|dop_v1_|shpat_|SG\\.)"));
 if(nearMiss.match(value).hasMatch()&&value.size()<24)return false;
 return true;
}
// #1894: what a segment shows of a secret with no prefix of its own, as the pieces to cover:
//  - a URL that carries a password (scheme://user:password@…, the @ sometimes lost to recognition: a
//    password is anything but a port after the user's colon), from its scheme to the segment's end;
//  - a webhook URL whose holder can post with it (Slack, Discord, Microsoft Teams, Zapier, Google
//    Chat, Azure Logic Apps), likewise, and a Telegram bot token;
//  - a secret-named assignment: NAME=value or name: value in one piece covers that piece and what is
//    joined to it (recognition splits a value with no word space), "name:" or "name =" then a value
//    piece covers the value alone, and only when it ends the segment (so "Password: must be at least
//    8 characters" stays visible). Authorization's scheme word (Bearer, Basic…) stays visible.
QList<QList<int>> structuralMatches(const Segment& segment,const QList<QString>& plain,const QList<RecognizedWord>& candidates) {
 QList<QList<int>> out;
 const int pieces=int(segment.words.size());
 const auto glyph=[&](int piece){const int w=segment.words.at(piece);return double(candidates.at(w).rect.width())/qMax<qsizetype>(1,plain.at(w).size());};
 // A piece joined to the one before with no word space: recognition's own split.
 const auto glued=[&](int piece){
  const auto& a=candidates.at(segment.words.at(piece-1)).rect;const auto& b=candidates.at(segment.words.at(piece)).rect;
  return b.left()-a.right()<=0.6*qMax(glyph(piece-1),glyph(piece));
 };
 const auto from=[&](int first,int last){QList<int> words;for(int i=first;i<=last;++i)words.append(segment.words.at(i));return words;};
 const auto pieceEnd=[&](int piece){return piece+1<pieces?segment.starts.at(piece+1):int(segment.text.size());};
 // URLs with a password, webhooks and bot tokens.
 static const QList<QRegularExpression> urls{
  QRegularExpression(QStringLiteral("(?i)(?:jdbc:)?(?:postgres(?:ql)?|mysql|mariadb|mongodb(?:\\+srv)?|rediss?|amqps?|mssql|sqlserver|oracle|db2|cockroachdb|clickhouse|cassandra|couchdb|neo4j(?:\\+s)?|bolt|nats|kafka|mqtts?|memcached|elasticsearch|snowflake|ftps?|sftp|smtps?|imaps?|pop3s?|ldaps?|https?|wss?|ssh|git|svn|socks5h?|proxy):/{1,2}[^\\s/:@\\[\\]]{0,64}:(?!(?=[0-9]{1,5}(?:[/?#,;\"'`)\\]>\u201D\u2019]|$))(?![^\\s/?#@]*@))[^\\s/@]{3,}")),
  // The credential-bearing path whole: team, hook and secret (Sol on #1903).
  QRegularExpression(QStringLiteral("(?i)hooks[.,]?s[a-z1|]{1,3}ck[.,]?com/(?:services|workflows|triggers)/[A-Za-z0-9]{6,}/[A-Za-z0-9]{6,}/(?:[0-9]{6,}/)?[A-Za-z0-9]{12,}")),
  QRegularExpression(QStringLiteral("(?i)discord(?:app)?[.,]?com/api/webhooks/[0-9]{6,}/[A-Za-z0-9_-]{8,}")),
  QRegularExpression(QStringLiteral("(?i)webhook[.,]?office[.,]?com/webhookb2/[A-Za-z0-9@-]{8,}")),
  QRegularExpression(QStringLiteral("(?i)hooks[.,]?zapier[.,]?com/hooks/catch/[A-Za-z0-9]{3,}/[A-Za-z0-9]{3,}")),
  QRegularExpression(QStringLiteral("(?i)chat[.,]?googleapis[.,]?com/[^\\s?&]{0,200}[?&](?:[^\\s&]{0,100}&){0,16}(?:key|token)=[A-Za-z0-9_-]{8,}")),
  QRegularExpression(QStringLiteral("(?i)logic[.,]?azure[.,]?com[^\\s?&]{0,200}[?&](?:[^\\s&]{0,100}&){0,16}sig=[A-Za-z0-9_%-]{8,}")),
  // A Sentry DSN: its key is the URL's userinfo (https://<key>@o1.ingest.sentry.io/<project>). Kept at
  // this place in the list: sentry below names it.
  QRegularExpression(QStringLiteral("(?i)https?:/{1,2}[0-9a-f]{12,}(?::[0-9a-f]+)?@(?:[A-Za-z0-9.-]{0,100}[.])?sentry[.,]io/[0-9]+")),
  // A Telegram bot token: the bot's id, a colon and 35 characters (core.telegram.org/bots/api).
  // #1940: a bot's id has no leading zero; an xxd row's offset ("00000010:" and its groups) does.
  QRegularExpression(QStringLiteral("(?<![0-9])[1-9][0-9]{5,11}:[A-Za-z0-9_-]{30,}")),
 };
 // The Sentry DSN pattern: its scheme starts a word as a password URL's does, its key is judged on its
 // own (not on any query after it), and one match a segment is enough (Sol on #1903).
 const QRegularExpression* sentry=nullptr;for(const auto& pattern:urls)if(pattern.pattern().contains(QLatin1String("sentry")))sentry=&pattern;
 for(const auto& pattern:urls){
  for(auto it=pattern.globalMatch(segment.text);it.hasNext();){
   const auto m=it.next();
   // A URL's scheme starts a piece or follows a separator ("DATABASE_URL=postgres://…", a CSV row's comma),
   // never inside a word.
   static const QString separators=QStringLiteral("=:,\"'`(<[“‘");
   const int at=int(m.capturedStart());
   if((&pattern==&urls.first()||&pattern==sentry)&&!pieceStart(segment,at)&&!(at>0&&separators.contains(segment.text.at(at-1))))continue;
   if(&pattern==sentry){
    const auto text=m.captured();const auto key=text.mid(text.indexOf(QLatin1String("//"))+2);
    if(masked(key.left(key.indexOf('@'))))continue;
    out.append(from(pieceAt(segment,m.capturedStart()),pieces-1));break;
   }
   // A URL's password as read, from the user's colon to the @ (or the end, the @ lost): a
   // placeholder or a reference is no secret; digits alone are a password like any other. Digits
   // after the colon that end a recognized word are a port, the @ never lost (Sol on #1903).
   if(&pattern==&urls.first()){
    const auto text=m.captured();const auto scheme=text.indexOf(QLatin1String("//"));
    const auto colon=text.indexOf(':',scheme<0?0:scheme+2);const auto at2=text.indexOf('@',colon);
    const auto password=text.mid(colon+1,at2<0?-1:at2-colon-1);
    if(at2<0){
     qsizetype d=colon+1;while(d<text.size()&&text.at(d).isDigit()&&d-colon<=5)++d;
     // ...unless the next word starts with the @ ("alice:1234" "@db"): then the digits are a password.
     const int after=int(m.capturedStart()+d);
     if(d>colon+1&&d-colon<=6&&pieceStart(segment,after)&&!(after<segment.text.size()&&segment.text.at(after)=='@'))continue;
    }
    if(!secretValue(password,true))continue;
   }
   // A webhook whose credentials are all masked stays visible: the last path part, read to the end
   // of its recognized word ("…/B5bIUb" "************"), and every key, token or sig parameter in
   // the URL; one unmasked credential keeps it covered (Sol on #1903).
   if(&pattern!=&urls.first()){
    const int endPiece=pieceAt(segment,int(m.capturedEnd()-1));
    const int urlEnd=endPiece+1<segment.starts.size()?segment.starts.at(endPiece+1):int(segment.text.size());
    const auto url=segment.text.mid(m.capturedStart(),urlEnd-m.capturedStart());
    static const QRegularExpression param(QStringLiteral("[?&](?:key|token|sig)=([^&\\s]+)"),QRegularExpression::CaseInsensitiveOption);
    QStringList credentials;for(auto p=param.globalMatch(url);p.hasNext();)credentials.append(p.next().captured(1));
    if(credentials.isEmpty()){const auto path=url.left(url.indexOf('?')<0?url.size():url.indexOf('?'));credentials.append(path.mid(path.lastIndexOf('/')+1));}
    static const QRegularExpression stars(QStringLiteral("[*\u2022]{4,}"));
    if(std::all_of(credentials.cbegin(),credentials.cend(),[](const QString& c){return masked(c)||stars.match(c).hasMatch();}))continue;
   }
   out.append(from(pieceAt(segment,m.capturedStart()),pieces-1));
  }
 }
 // Assignments inside a piece, at its start or after a separator of JSON, CSV or a query string
 // ({"…","TOKEN":"…"}, NAME,value,owner, ?api_key=…&): the piece is covered when the value there is a
 // secret's.
 // A quoted value is read to its closing quote, whatever it holds ({"password":"!&%?"}; Sol on #1903).
 static const QRegularExpression inner(QStringLiteral("(?:^|[,{;&?(\\[])\\s?[\"']?([A-Za-z_][A-Za-z0-9_.-]{1,63})[\"']?\\s?([=:,])\\s?(?:\"([^\"]{4,200})(?:\"|$)|'([^']{4,200})(?:'|$)|([^,\"'&;}\\s]{4,}))"));
 // ...and as read word by word, unquoted, so a value whose closing quote recognition put in the next
 // word, or a query assignment inside a quoted URL (…&password=…), is still found (Sol on #1903).
 static const QRegularExpression bare(QStringLiteral("(?:^|[,{;&?(\\[])\\s?[\"']?([A-Za-z_][A-Za-z0-9_.-]{1,63})[\"']?\\s?([=:,])\\s?[\"']?([^,\"'&;}\\s]{4,})"));
 for(int p=0;p<pieces;++p){
  const auto text=segment.text.mid(segment.starts.at(p),pieceEnd(p)-segment.starts.at(p));
  bool covered=false;
  // Each pattern is tried again from inside the value it just read, so an assignment within that value
  // (a query's ?password=… inside a quoted URL) is found too (#1915).
  for(const auto* pattern:{&inner,&bare})for(qsizetype at=0;at<text.size()&&!covered;){
   const auto m=pattern->match(text,at);if(!m.hasMatch())break;
   at=qMax(at+1,m.capturedStart(2)+1);
   // After a comma (a CSV row) the value needs a digit and a letter, so "Password,please" stays visible.
   const auto value=pattern==&bare?m.captured(3):!m.captured(3).isEmpty()?m.captured(3):!m.captured(4).isEmpty()?m.captured(4):m.captured(5);
   if(m.captured(2)==QLatin1String(",")&&!(hasDigit(value)&&std::any_of(value.cbegin(),value.cend(),[](QChar c){return c.isLetter();})))continue;
   const bool declared=m.captured(2)==QLatin1String(":")&&(pattern==&inner?m.capturedLength(5)>0:!QStringLiteral("\"'").contains(text.at(qMax(0,m.capturedStart(3)-1))));
   if(secretName(m.captured(1))&&secretValue(value,passwordName(m.captured(1)),m.captured(1),declared)){
    int last=p;while(last+1<pieces&&glued(last+1))++last;
    // A quoted value whose closing quote is in a later word ("N4!&v" "blue-lake\"}") runs to it (Sol on #1916).
    const QChar quote=m.capturedLength(3)>0?QChar('"'):QChar('\'');
    if(pattern==&inner&&(m.capturedLength(3)>0||m.capturedLength(4)>0)&&m.capturedEnd()==text.size()&&!m.captured().endsWith(quote)){
     for(int q=last+1;q<pieces&&q<=last+6;++q)if(segment.text.mid(segment.starts.at(q),pieceEnd(q)-segment.starts.at(q)).contains(quote)){last=q;break;}
    }
    out.append(from(p,last));covered=true;}
  }
 }
 // Assignments, from a piece's start (after a line number recognition joined to it).
 static const QRegularExpression assignment(QStringLiteral("^(?:[0-9]{1,5})?[\"'`“‘]?([A-Za-z_][A-Za-z0-9_.-]{1,63})[\"'`”’]?\\s?([=:]|$)"));
 for(int p=0;p<pieces;++p){
  const auto m=assignment.match(segment.text.mid(segment.starts.at(p),pieceEnd(p)-segment.starts.at(p)));
  if(!m.hasMatch()||!secretName(m.captured(1)))continue;
  // The value in the same piece: that piece and what recognition split off it.
  const int valueAt=segment.starts.at(p)+int(m.capturedEnd());
  if(valueAt<pieceEnd(p)){
   int last=p;while(last+1<pieces&&glued(last+1))++last;
   if(!secretValue(segment.text.mid(valueAt,pieceEnd(last)-valueAt),passwordName(m.captured(1)),m.captured(1),m.captured(2)==QLatin1String(":")))continue;
   out.append(from(p,last));continue;
  }
  // The value in the next piece (after a lone "=" or ":" piece too), the last on the segment. A name
  // with no separator of its own needs that lone one.
  int first=p+1;
  const bool lone=first<pieces&&(plain.at(segment.words.at(first))=="="||plain.at(segment.words.at(first))==":");
  if(m.capturedLength(2)==0&&!lone)continue;
  if(lone)++first;
  if(first>=pieces)continue;
  // Authorization: Bearer <token> — the scheme stays visible.
  static const QSet<QString> schemes{"bearer","basic","token","bot","digest"};
  if(m.captured(1).compare(QLatin1String("authorization"),Qt::CaseInsensitive)==0&&first+1<pieces&&schemes.contains(plain.at(segment.words.at(first)).toLower()))++first;
  int last=first;while(last+1<pieces&&glued(last+1))++last;
  if(last!=pieces-1)continue;
  const bool declared=m.captured(2)==QLatin1String(":")||(lone&&plain.at(segment.words.at(p+1))==QLatin1String(":"));
  if(!secretValue(segment.text.mid(segment.starts.at(first),pieceEnd(last)-segment.starts.at(first)),passwordName(m.captured(1)),m.captured(1),declared))continue;
  out.append(from(first,last));
 }
 return out;
}
// #1940: words that show public data rather than a key, by what the screen shows around them. They take
// no part in the hex, base64 and repeated runs; a known prefix's token or a JSON web token in one is
// never public, and a secret-named setting, a password URL and a PEM block are found as before.
//  - a hex dump's row: an offset (00000010:, 0010) and six byte or word groups or more (xxd, hexdump,
//    Wireshark), or twelve byte groups or more without one; and a fingerprint of ten groups of four;
//  - a digest (32, 40, 56, 64, 96 or 128 hex characters, one more or less as read) that starts its row
//    before a ref (refs/…, HEAD) or a file name (sha256sum's "<hex>  name.iso"), and the digest rows that
//    follow a checksum or git plumbing command (sha256sum, git rev-parse, show-ref, ls-remote…) up to
//    the first row of anything else;
//  - the value of a label that says it is an identifier, a public key or a signature (Event ID:,
//    trace_id=, x-amz-id-2:, Change-Id:, tree, parent, PublicKey =, h1:, a DKIM header's b= and bh=),
//    in that label's alphabet and up to its usual length; a
//    label of no fixed length runs on over the rows below it that hold its alphabet alone. A qualifier
//    before "ID" or "Key" that is not in the list ("Secret ID:", "Access Key:") makes no label.
QSet<int> publicWords(const QMap<int,QList<int>>& lines,const QList<QString>& plain,const QList<RecognizedWord>& candidates) {
 QSet<int> out;
 const auto keys=lines.keys();
 const auto mapped=[](const QString& word){QString m=word;for(auto& c:m){c=hexConfusions().value(c,c);if(c==QChar('@'))c=QChar('0');}return m;};
 const auto isHex=[](QChar c){return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F');};
 const auto hexish=[&](const QString& word){const auto m=mapped(word);if(m.isEmpty())return false;qsizetype other=0;for(const auto c:m)if(!isHex(c))++other;return other*8<=m.size();};
 const auto allHex=[&](const QString& word){const auto m=mapped(word);return !m.isEmpty()&&std::all_of(m.cbegin(),m.cend(),isHex);};
 const auto prefixed=[&](int w){
  Segment one;one.words={w};one.starts={0};one.text=plain.at(w);QList<Found> found;segmentMatches(one,0,0,found);
  return std::any_of(found.cbegin(),found.cend(),[](const Found& f){return f.length>0;});
 };
 const auto mark=[&](int w){if(!prefixed(w))out.insert(w);};
 // Words that are the evidence for a public one (its label, the file name or ref after a digest): they
 // are no unknown neighbour (below).
 QSet<int> known;
 const auto glyph=[&](int w){return double(candidates.at(w).rect.width())/qMax<qsizetype>(1,plain.at(w).size());};
 const auto gap=[&](int a,int b){return double(candidates.at(b).rect.left()-candidates.at(a).rect.right())/qMax(1.0,qMax(glyph(a),glyph(b)));};
 // The words of a row from `from` on, up to a gap of four characters (a column or a pane beside it).
 const auto column=[&](const QList<int>& row,int from){int end=from;while(end<row.size()&&(end==from||gap(row.at(end-1),row.at(end))<=4.0))++end;return end;};
 // The first word after an editor's line number or a lone mark.
 const auto leadOf=[&](const QList<int>& line){
  static const QRegularExpression number(QStringLiteral("^\\d{1,5}$"));
  if(line.size()>1&&(number.match(plain.at(line.first())).hasMatch()||std::none_of(plain.at(line.first()).cbegin(),plain.at(line.first()).cend(),[](QChar c){return c.isLetterOrNumber();})))return 1;
  return 0;
 };
 // The words of a digest starting at word i: up to four, joined while hex, to a digest's length.
 const auto digestAt=[&](const QList<int>& line,int i){
  QString joined;
  for(int j=i;j<line.size()&&j<i+4;++j){
   const auto& t=plain.at(line.at(j));
   if(!hexish(t)||(j>i&&gap(line.at(j-1),line.at(j))>1.5))return 0;
   joined+=t;
   if(joined.size()>129)return 0;
   for(const int length:{32,40,56,64,96,128})if(std::abs(int(joined.size())-length)<=2&&hasDigit(mapped(joined)))return j-i+1;
  }
  return 0;
 };
 static const QRegularExpression command(QStringLiteral("\\b(?:md[5S]|sha[0-9SlI]{0,3}|b2|b3|ck)sum\\b|\\bgit\\b.*\\b(?:rev-parse|show-ref|ls-remote|rev-list|merge-base|hash-object|write-tree|cat-file|submodule)\\b|\\b(?:podman|docker)\\b.*(?:--no-trunc|\\binspect\\b|--digests)"),QRegularExpression::CaseInsensitiveOption);
 static const QRegularExpression file(QStringLiteral("^\\*?[^\\s=:]*[A-Za-z0-9_)-]\\.[A-Za-z][A-Za-z0-9]{0,11}$|^[^\\s=:]*/[^\\s=:]+$"));
 for(int k=0;k<keys.size();++k){
  const auto& line=lines[keys.at(k)];const int n=int(line.size());const int lead=leadOf(line);
  // A W3C traceparent, a word of its own.
  for(const int w:line){
   static const QRegularExpression shape(QStringLiteral("^[\"']?0[0-9a-f]-[0-9a-f]{32}-[0-9a-f]{16}-0[0-9a-f][\"',;]*$"));
   if(shape.match(plain.at(w)).hasMatch())mark(w);
  }
  // A subresource integrity digest (sha512-…, its 512 as recognition reads it: shaS12-, sha51i2-) and the
  // words of its base64 joined after it, up to the digest's length.
  for(int i=0;i<n;++i){
   static const QRegularExpression sri(QStringLiteral("^[\"'{(]?(?:integrity:)?sha(256|384|512|[0-9SlIi|]{3,4})-([A-Za-z0-9+/=]*)[\"',;})]*$"));
   const auto m=sri.match(plain.at(line.at(i)));if(!m.hasMatch())continue;
   const int length=m.captured(1)==QLatin1String("256")?44:m.captured(1)==QLatin1String("384")?64:88;
   qsizetype total=m.capturedLength(2);mark(line.at(i));
   static const QRegularExpression piece(QStringLiteral("^[A-Za-z0-9+/=]+[\"',;})]*$"));
   for(int j=i+1;j<n&&gap(line.at(j-1),line.at(j))<=2.0&&piece.match(plain.at(line.at(j))).hasMatch()&&total+plain.at(line.at(j)).size()<=length+4;++j){total+=plain.at(line.at(j)).size();mark(line.at(j));i=j;}
  }
  // A hex dump's row.
  {// The offset as read: one character of it may be misread ("Q0O0bO" for 000000b0).
   const auto offsetWord=[&](const QString& word){
    auto m=mapped(word);if(m.endsWith(':'))m.chop(1);
    if(m.size()<4||m.size()>10||!hasDigit(m))return false;
    return std::count_if(m.cbegin(),m.cend(),[&](QChar c){return !isHex(c);})<=1;
   };
   const bool hasOffset=lead<n&&offsetWord(plain.at(line.at(lead)));
   const int start=hasOffset?lead+1:lead;int groups=0,fours=0,odd=0,j=start;
   for(;j<n;++j){
    const auto& t=plain.at(line.at(j));
    if((t.size()==2||t.size()==4)&&allHex(t)){++groups;fours+=t.size()==4?1:0;continue;}
    if(t.size()<=4&&(odd+1)*4<=groups+4){++odd;continue;}
    break;
   }
   // The words after the groups, in their column, are the row's own bytes as text (the ASCII column),
   // however read.
   if((hasOffset&&groups>=6)||(!hasOffset&&groups>=12&&fours==0)){for(int i=hasOffset?lead:start;i<j;++i)mark(line.at(i));for(int i=j;i<column(line,j);++i)known.insert(line.at(i));}}
  // A fingerprint: ten groups of four, no more.
  for(int i=0;i+10<=n;++i){
   const auto four=[&](int at){return at>=0&&at<n&&plain.at(line.at(at)).size()==4&&allHex(plain.at(line.at(at)));};
   if(four(i-1))continue;
   int j=i;while(four(j))++j;
   if(j-i==10){for(int g=i;g<j;++g)mark(line.at(g));}
   i=qMax(i,j-1);
  }
  // A digest before a ref or a file name in its own column (within four characters of it: a file name
  // in a pane beside it is no evidence), the rest of that column at most a word in brackets
  // ("(v2.2.1)", git submodule status) or a ref's "^{}".
  if(const int d=digestAt(line,lead);d>0&&lead+d<n&&gap(line.at(lead+d-1),line.at(lead+d))<=4.0){
   const int end=column(line,lead+d);
   const auto& next=plain.at(line.at(lead+d));
   static const QRegularExpression aside(QStringLiteral("^\\(.*\\)$"));
   const bool rest=end==lead+d+1||(end==lead+d+2&&aside.match(plain.at(line.at(lead+d+1))).hasMatch());
   if(rest&&(next.startsWith(QLatin1String("refs/"))||next.startsWith(QLatin1String("HEAD"))||file.match(next).hasMatch())){
    for(int i=lead;i<lead+d;++i)mark(line.at(i));
    for(int i=lead+d;i<end;++i)known.insert(line.at(i));
   }
  }
  // The digest rows after a checksum or git plumbing command, in the command's own column: the row is
  // cut into columns at gaps of four characters (panes side by side), and a column whose words, after a
  // line number, hold the whole command starts it. Each digest row below has its digest where that
  // column starts (within four characters, after a line number), right below the row before (within
  // two and a half of its heights), and the same form as the first: a digest alone, or a digest and
  // one more word in its column (the file name), never an "=". Anything else ends the rows.
  {const auto columnsOf=[&](const QList<int>& row){QList<int> starts{0};for(int i=1;i<row.size();++i)if(gap(row.at(i-1),row.at(i))>4.0)starts.append(i);return starts;};
   const auto skipNumber=[&](const QList<int>& row,int at,int end){static const QRegularExpression number(QStringLiteral("^\\d{1,5}$"));return at+1<end&&number.match(plain.at(row.at(at))).hasMatch()?at+1:at;};
   // A line number is skipped on the digest rows only when the command's row has one too (one gutter),
   // the rows are measured from the command's own column, and an "=" anywhere on a row ends them.
   int commandStart=-1,commandWord=-1;bool numbered=false;
   for(const int c:columnsOf(line)){
    const int end=column(line,c);const int at=skipNumber(line,c,end);
    QStringList words;for(int i=at;i<end;++i)words.append(plain.at(line.at(i)));
    if(command.match(words.join(' ')).hasMatch()){commandStart=candidates.at(line.at(at)).rect.left();commandWord=at;numbered=at>c;break;}
   }
   if(commandStart>=0){
    int above=candidates.at(line.at(commandWord)).rect.top(),height=candidates.at(line.at(commandWord)).rect.height(),form=-1;
    for(int m=k+1;m<keys.size()&&m<=k+200;++m){
     const auto& row=lines[keys.at(m)];int at=-1,end=0;
     for(const int c:columnsOf(row)){
      const int e=column(row,c);const int a=numbered?skipNumber(row,c,e):c;
      if(std::abs(candidates.at(row.at(a)).rect.left()-commandStart)<=4*glyph(row.at(a))){at=a;end=e;break;}
     }
     if(at<0)break;
     const auto& box=candidates.at(row.at(at)).rect;
     if(box.top()<=above||box.top()-above>2.5*qMax(height,box.height()))break;
     const int d=digestAt(row,at);
     if(d==0||at+d<end-1)break;
     const int shape=at+d<end?1:0;
     if(form>=0&&shape!=form)break;
     if(std::any_of(row.cbegin(),row.cend(),[&](int w){return plain.at(w).contains('=');}))break;
     form=shape;above=box.top();height=box.height();
     for(int i=at;i<at+d;++i)mark(row.at(i));
     for(int i=at+d;i<end;++i)known.insert(row.at(i));
    }
   }}
  // Labelled public values: kind 0 hex (with dashes: UUIDs), kind 1 base64; a length of 0 runs on.
  struct Label {int kind;int length;bool dkim=false;bool bare=false;};
  static const QHash<QString,Label> labels{
   {"id",{0,36}},{"eventid",{0,36}},{"traceid",{0,36}},{"spanid",{0,36}},{"parentspanid",{0,36}},{"parentid",{0,36}},{"requestid",{0,36}},{"xrequestid",{0,36}},{"req",{0,36}},{"reqid",{0,36}},
   {"correlationid",{0,36}},{"xcorrelationid",{0,36}},{"bootid",{0,36}},{"machineid",{0,36}},{"invocationid",{0,36}},{"xamzrequestid",{0,36}},{"traceparent",{0,56}},{"changeid",{0,42}},
   {"tree",{0,64,false,true}},{"parent",{0,64,false,true}},{"object",{0,64,false,true}},{"rev",{0,64}},{"revision",{0,64}},
   {"xamzid2",{1,80}},{"xamzcfid",{1,60}},{"publickey",{1,48}},{"pubkey",{1,48}},{"h1",{1,48}},{"xgooglesmtpsource",{1,0}},{"bh",{1,48,true}},{"b",{1,0,true}},
   {"messageid",{1,100}},
  };
  const auto normal=[](const QString& name){QString out;for(const auto c:name)if(c.isLetterOrNumber())out+=c.toLower();return out;};
  const auto inAlphabet=[&](QString t,int kind){
   static const QString quotes=QStringLiteral("\"'`“”‘’");
   while(!t.isEmpty()&&(quotes.contains(t.back())||t.back()==','||t.back()==';'))t.chop(1);
   while(!t.isEmpty()&&quotes.contains(t.front()))t.remove(0,1);
   if(t.isEmpty()||t.contains(':'))return false;
   qsizetype other=0;
   if(kind==0){const auto m=mapped(t);for(const auto c:m)if(!isHex(c)&&c!='-')++other;return other*8<=m.size()&&hasDigit(m);}
   for(qsizetype i=0;i<t.size();++i){const auto c=t.at(i);if(!(c.isLetterOrNumber()&&c.unicode()<128)&&!QStringLiteral("+/_-").contains(c)&&!(c=='='&&t.mid(i).count('=')==t.size()-i))++other;}
   return other*8<=t.size();
  };
  const auto dkim=[&]{
   for(int m=qMax(0,k-6);m<=k;++m){QStringList words;for(const int w:lines[keys.at(m)])words.append(plain.at(w));const auto text=words.join(' ');
    if(text.contains(QLatin1String("DKIM-Signature"),Qt::CaseInsensitive)||text.contains(QLatin1String("ARC-"))||text.contains(QLatin1String("bh=")))return true;}
   return false;
  };
  for(int i=lead;i<n;++i){
   const auto& t=plain.at(line.at(i));
   // The label's name, and where its value starts: in this word after its ":" or "=", or the next word
   // (after a lone "=" or ":").
   qsizetype sep=-1;for(qsizetype c=0;c<t.size()&&sep<0;++c)if(t.at(c)==':'||t.at(c)=='=')sep=c;
   QString name;int valueWord=-1;QString first;
   if(sep>0&&sep+1<t.size()){name=t.left(sep);valueWord=i;first=t.mid(sep+1);}
   else if(sep>0&&sep+1==t.size()){name=t.left(sep);valueWord=i+1;}
   else if(sep<0&&i+2<n&&(plain.at(line.at(i+1))==QLatin1String("=")||plain.at(line.at(i+1))==QLatin1String(":"))){name=t;valueWord=i+2;}
   else if(sep<0&&i==lead&&i+1<n){name=t;valueWord=i+1;}
   else continue;
   if(valueWord>=n)continue;
   auto key=normal(name);int labelStart=i;
   // "Event" "ID:", "public" "key =": the word before qualifies the label; an unknown one makes none.
   if(i>lead&&(key==QLatin1String("id")||key==QLatin1String("key"))){
    const auto& before=plain.at(line.at(i-1));
    if(std::all_of(before.cbegin(),before.cend(),[](QChar c){return c.isLetter();})&&before.size()<=16&&gap(line.at(i-1),line.at(i))<=1.5){
     key=normal(before)+key;if(!labels.contains(key))continue;
     labelStart=i-1;
    }
   }
   // Recognition's usual misreads of these labels: a doubled x (xX-amz-id-2), i read as t, 1 or l
   // (x-amz-td-2, tracelId, traceld), and an I read beside an i (requestiId).
   auto it=labels.constFind(key);
   if(it==labels.cend()){
    auto fuzzy=key;
    if(fuzzy.startsWith(QLatin1String("xx")))fuzzy.remove(0,1);
    static const QRegularExpression id(QStringLiteral("(?:[itl1]{1,2}|li|il)d(?=2?$)"));
    fuzzy.replace(id,QStringLiteral("id"));
    it=labels.constFind(fuzzy);
   }
   if(it==labels.cend())continue;
   const auto& label=it.value();
   if(label.bare!=(sep<0&&valueWord==i+1))continue;
   if(label.dkim&&!dkim())continue;
   if(first.isEmpty())first=plain.at(line.at(valueWord));
   if(!inAlphabet(first,label.kind))continue;
   qsizetype total=first.size();const auto bound=[&](qsizetype more){return label.length==0||total+more<=label.length*115/100+2;};
   if(!bound(0))continue;
   QList<int> value{line.at(valueWord)};int j=valueWord+1;
   for(;j<n;++j){
    const auto& w=plain.at(line.at(j));
    if(gap(line.at(j-1),line.at(j))>2.0||!inAlphabet(w,label.kind)||!bound(w.size()))break;
    total+=w.size();value.append(line.at(j));
   }
   for(const int w:value)mark(w);
   for(int l=labelStart;l<valueWord;++l)known.insert(line.at(l));
   // A value of no fixed length that ends its row runs on over the rows of its alphabet alone below, as
   // a wrap leaves them: each row it leaves reaches the right edge of the rows around it (within three
   // characters), so a row after a short last one ("client-key-data:" read without its label) is not.
   if(j==n&&label.length==0){
    int right=candidates.at(line.last()).rect.right();
    for(int m=k+1;m<keys.size()&&m<=k+60;++m)right=qMax(right,candidates.at(lines[keys.at(m)].last()).rect.right());
    const auto full=[&](const QList<int>& row){return candidates.at(row.last()).rect.right()>=right-3*glyph(row.last());};
    // ...each right below the one before (within two and a half of its heights) and starting within four
    // characters of the label's row or of the value's start, never in another pane.
    const auto& labelBox=candidates.at(line.first()).rect;const auto& valueBox=candidates.at(line.at(valueWord)).rect;
    for(int m=k+1;m<keys.size()&&m<=k+60&&full(lines[keys.at(m-1)]);++m){
     const auto& row=lines[keys.at(m)];
     const auto& above=candidates.at(lines[keys.at(m-1)].first()).rect;const auto& box=candidates.at(row.first()).rect;const double g=glyph(row.first());
     if(box.top()<=above.top()||box.top()-above.top()>2.5*qMax(above.height(),box.height())||(std::abs(box.left()-labelBox.left())>4*g&&std::abs(box.left()-valueBox.left())>4*g))break;
     if(!std::all_of(row.cbegin(),row.cend(),[&](int w){return inAlphabet(plain.at(w),label.kind);}))break;
     for(const int w:row)mark(w);
    }
   }
   i=qMax(i,j-1);
  }
 }
 // Fails closed: where a public word would have joined a token segment with a word that is neither
 // public nor its evidence and looks like a secret of its own (eight letters and digits or more, with
 // both, after any label in it: a recovery code, a UUID after "API Key:"), that segment's words all stay
 // as they were, so what covered the unknown word before still covers it. A timestamp and an algorithm's
 // name (rsa-sha256) or a memory address (0x4a34d54d92037984) are no such word.
 // A segment that ends its row counts the next row's first segment with it, when that starts its row:
 // a token's wrapped remainder ("KO4T-GZUD" below an X-Amz-Cf-Id at a row's end) was covered with it.
 // Judged on the words as first found, so one segment put back does not make its neighbours' words unknown.
 const auto found=out;
 const auto unknownWord=[&](int w){
  if(found.contains(w)||known.contains(w))return false;
  static const QRegularExpression label(QStringLiteral("^[\"'`“]?[A-Za-z_][A-Za-z0-9_.-]{0,40}[\"'`”]?\\s?[=:]")),time(QStringLiteral("\\d\\d:\\d\\d")),algorithm(QStringLiteral("(?i)sha\\d|md5|rsa|ed25519|x86_64|aarch64|^@?0x[0-9a-f]{1,16}$"));
  auto value=plain.at(w);const auto m=label.match(value);if(m.hasMatch())value=value.mid(m.capturedLength());
  static const QRegularExpression date(QStringLiteral("^\\d{4}-\\d{1,2}-\\d"));
  if(time.match(value).hasMatch()||date.match(value).hasMatch()||algorithm.match(value).hasMatch())return false;
  int alnum=0;bool digit=false,letter=false;for(const auto c:value)if(c.isLetterOrNumber()){++alnum;digit=digit||c.isDigit();letter=letter||c.isLetter();}
  return alnum>=8&&digit&&letter;
 };
 QHash<int,QList<Segment>> plainSegments;for(const int key:keys)plainSegments.insert(key,segments(lines[key],plain,candidates));
 for(int k=0;k<keys.size();++k){
  const auto& line=lines[keys.at(k)];const auto& rowSegments=plainSegments[keys.at(k)];
  for(const auto& segment:rowSegments){
   if(std::none_of(segment.words.cbegin(),segment.words.cend(),[&](int w){return found.contains(w);}))continue;
   bool unknown=std::any_of(segment.words.cbegin(),segment.words.cend(),unknownWord);
   // A known prefix's token or a JSON web token in the segment as joined ("ghp_" "<body>" split by
   // recognition after a public label) keeps it as it was too.
   if(!unknown){QList<Found> tokens;segmentMatches(segment,0,int(segment.words.size())-1,tokens);unknown=std::any_of(tokens.cbegin(),tokens.cend(),[](const Found& f){return f.length>0;});}
   if(!unknown&&segment.words.last()==line.last()&&k+1<keys.size()){
    const auto& next=plainSegments[keys.at(k+1)];
    if(!next.isEmpty()&&next.first().words.first()==lines[keys.at(k+1)].first())unknown=std::any_of(next.first().words.cbegin(),next.first().words.cend(),unknownWord);
   }
   if(unknown)for(const int w:segment.words)out.remove(w);
  }
 }
 return out;
}
QList<QList<int>> keyMatches(const QList<RecognizedWord>& candidates,const QList<QString>& plain) {
 QMap<int,QList<int>> lines;for(int i=0;i<candidates.size();++i)lines[candidates.at(i).line].append(i);
 const auto lineText=[&](const QList<int>& line){QStringList words;for(const int i:line)words.append(plain.at(i));return words.join(' ');};
 static const QRegularExpression begin(QStringLiteral("[-—–~_=.]{2,} ?BEGIN\\b[A-Z0-9 ]*PRIVATE ?KEY")),end(QStringLiteral("[-—–~_=.]{2,} ?END\\b[A-Z0-9 ]*PRIVATE ?KEY"));
 QList<QList<int>> out;
 const auto shown=publicWords(lines,plain,candidates);
 // #1897: what a line gives the wrap checks is the same whichever token above reaches it, so it is
 // worked out once: its segments, whether its first holds a card or an address, and that segment's
 // own matches. Every line still starts its own chain (Sol on #1902), at the cost of these lookups.
 struct LineFacts {QList<Segment> segments;bool otherDone=false,other=false,ownDone=false;QList<Found> own;bool pem=false,statement=false,numbered=false,digit=false,upper=false,longWord=false;qsizetype lowercase=0;};
 QHash<int,LineFacts> facts;
 // The two costly facts are worked out only when a chain asks for them (Sol on #1902).
 const auto otherOf=[&](LineFacts& f){if(!f.otherDone){f.otherDone=true;f.other=!f.segments.isEmpty()&&otherMatch(f.segments.first().words,plain,candidates);}return f.other;};
 const auto ownOf=[&](LineFacts& f)->const QList<Found>&{if(!f.ownDone){f.ownDone=true;if(!f.segments.isEmpty())segmentMatches(f.segments.first(),0,int(f.segments.first().words.size())-1,f.own);}return f.own;};
 const auto factsOf=[&](int key)->LineFacts&{
  auto it=facts.find(key);
  if(it==facts.end()){
   LineFacts f;f.segments=segments(lines[key],plain,candidates,false,&shown);
   const auto& line=lines[key];
   // A wrapped remainder ends where an inline comment starts ("…Ui5" "=" "#" "development"): the
   // comment's words neither join the cover nor decide whether the line is a remainder (Sol on #1916).
   if(!f.segments.isEmpty()){
    auto& first=f.segments.first();
    for(int i=1;i<first.words.size();++i){
     const auto& t=plain.at(first.words.at(i));
     // A comment marker as a word of its own: "//Q1WEr3…" inside an open quoted value is the value.
     if(t==QLatin1String("#")||t==QLatin1String("//")){first.text.truncate(first.starts.at(i));first.words.resize(i);first.starts.resize(i);first.heads.clear();first.headsFound=false;first.counts={};break;}
    }
   }
   if(!f.segments.isEmpty()){
    const auto& tail=f.segments.first().text;for(const auto c:tail){f.digit=f.digit||c.isDigit();f.upper=f.upper||c.isUpper();}
    static const QRegularExpression prose(QStringLiteral("[a-z]{5,}")),word(QStringLiteral("[a-z]{4,}"));
    for(auto it=prose.globalMatch(tail);it.hasNext();)f.lowercase+=it.next().capturedLength();
    f.longWord=word.match(tail).hasMatch();
   }
   const auto text=lineText(line);f.pem=begin.match(text).hasMatch()||end.match(text).hasMatch();
   // #1894: a line that starts a new statement is no token's remainder: an assignment ("PORT=8080",
   // "host:" after an editor's line number) or a line with a hash word. A wrapped remainder is base64,
   // hex or a JWT's part, with an "=" only as padding at its end. Base64 padding before closing
   // punctuation ("…L=\",") is a remainder's end; an empty snake-case assignment ("DATABASE_URL=") is
   // a new statement. Judged on the line's first recognized word after a line number, not on the
   // words joined (Sol on #1901: "…==”" "// note").
   static const QRegularExpression assignment(QStringLiteral("^[A-Za-z_][A-Za-z0-9_.-]*[=:](?![=:\"'`)\\]};,\u201D\u2019]*$)")),number(QStringLiteral("^\\d{1,5}$"));
   static const QRegularExpression empty(QStringLiteral("^(?:[A-Z][A-Z0-9]*(?:_[A-Z0-9]+)+|[a-z][a-z0-9]*(?:_[a-z0-9]+)+)[=:][\"'`\u201C\u201D]*[;,]?$"));
   f.numbered=line.size()>1&&number.match(plain.at(line.first())).hasMatch();
   const int lead=f.numbered||(line.size()>1&&plain.at(line.first()).size()==1&&!plain.at(line.first()).front().isLetterOrNumber())?1:0;
   const auto& firstWord=plain.at(line.at(lead));
   // A name, then "=" or ":" as a word of its own ("DEBUG_MODE" "=" "0", Sol on #1901).
   static const QRegularExpression name(QStringLiteral("^[A-Za-z_][A-Za-z0-9_.-]*$"));
   // ...with a value after it, not an inline comment ("…Ui5" "=" "#" "dev" is padding); an empty
   // snake-case name with its "=" alone ("DATABASE_URL" "=") is a new statement too (#1914).
   const bool operatorWord=lead+1<line.size()&&(plain.at(line.at(lead+1))==QLatin1String("=")||plain.at(line.at(lead+1))==QLatin1String(":"));
   const bool commentAfter=lead+2<line.size()&&(plain.at(line.at(lead+2)).startsWith('#')||plain.at(line.at(lead+2)).startsWith(QLatin1String("//")));
   static const QRegularExpression snake(QStringLiteral("^(?:[A-Z][A-Z0-9]*(?:_[A-Z0-9]+)+|[a-z][a-z0-9]*(?:_[a-z0-9]+)+)$"));
   const bool separated=operatorWord&&name.match(firstWord).hasMatch()&&(lead+2<line.size()&&!commentAfter?true:snake.match(firstWord).hasMatch());
   f.statement=assignment.match(firstWord).hasMatch()||empty.match(firstWord).hasMatch()||separated||digestLine(line,plain);
   it=facts.insert(key,std::move(f));
  }
  return it.value();
 };
 const auto keys=lines.keys();
 for(int k=0;k<keys.size();++k){
  const auto& line=lines[keys.at(k)];
  // A PEM private-key block: its BEGIN line, every line after it up to its END
  // line (or while the lines are token-shaped, if END was not read), and END.
  if(begin.match(lineText(line)).hasMatch()){
   QList<int> block=line;
   // #1894: with its END line found below (within 100 lines), the block is every line in between,
   // whatever recognition made of a body line ("MIIEow…" read with a stray mark kept the first one bare).
   int closingAt=-1;for(int j=k+1;j<keys.size()&&j<=k+100&&closingAt<0;++j)if(end.match(lineText(lines[keys.at(j)])).hasMatch())closingAt=j;
   if(closingAt>0){for(int j=k+1;j<=closingAt;++j)block+=lines[keys.at(j)];k=closingAt;out.append(block);continue;}
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
  // #1894: a certificate or a public key block is public: its lines stay visible up to its END line.
  {static const QRegularExpression publicBegin(QStringLiteral("[-\u2014\u2013~_=.]{2,} ?BEGIN\\b[A-Z0-9 ]*(?:CERTIFICATE|PUBLIC ?KEY)[A-Z0-9 ]*")),publicEnd(QStringLiteral("[-\u2014\u2013~_=.]{2,} ?END\\b[A-Z0-9 ]*(?:CERTIFICATE|PUBLIC ?KEY)[A-Z0-9 ]*"));
   // Its own END (CERTIFICATE with CERTIFICATE), and no BEGIN of any block between: otherwise it is
   // read line by line as any text (Sol on #1903).
   const auto opened=publicBegin.match(lineText(line));
   if(opened.hasMatch()){
    // The whole label (TRUSTED CERTIFICATE is not CERTIFICATE).
    const auto labelOf=[](const QString& marker,const QString& word){const auto at=marker.indexOf(word);return marker.mid(at+word.size()).simplified();};
    const auto label=labelOf(opened.captured(),QStringLiteral("BEGIN"));
    int j=k+1;bool closed=false;
    for(;j<keys.size()&&j<=k+100;++j){
     const auto text=lineText(lines[keys.at(j)]);
     if(begin.match(text).hasMatch()||publicBegin.match(text).hasMatch())break;
     const auto shut=publicEnd.match(text);
     if(shut.hasMatch()){closed=labelOf(shut.captured(),QStringLiteral("END"))==label;break;}
    }
    if(closed){k=j;continue;}
   }}
  for(const auto& segment:segments(line,plain,candidates,true))out+=structuralMatches(segment,plain,candidates);
  const auto lineSegments=segments(line,plain,candidates,false,&shown);
  // Whether the row names a digest at all, worked out once; where each word sits on it, for the label
  // beside a run (below).
  std::optional<bool> rowDigest;QHash<int,int> position;
  // Whether the row holds an SSH public key's type word, worked out once (Sol on #1954: a row of thousands
  // of segments read it once a segment).
  const bool sshPublic=std::any_of(line.cbegin(),line.cend(),[&](int w){static const QRegularExpression ssh(QStringLiteral("^[\"'`\u201C\u2018]?(?:ssh-(?:rsa|ed25519|dss)|ecdsa-sha2-|sk-(?:ssh|ecdsa)-)"));return ssh.match(plain.at(w)).hasMatch();});
  for(int s=0;s<lineSegments.size();++s){
   const auto& segment=lineSegments.at(s);
   QList<Found> found;segmentMatches(segment,0,int(segment.words.size())-1,found);
   // #1895: a hex run with a hash word on its line (a "Git SHA" or "SHA-256" label cell, "commit",
   // "sha256:") is a digest, not a key, unless a secret word is on the line too. With no word either
   // way it stays covered: a hash and a hex secret look the same.
   // #1894: public material stays visible: a run or a JSON web token right after a publishable key's
   // prefix (pk_live_, pk_test_, sb_publishable_, Mapbox's pk.) or inside a data: URI, and the base64
   // of an SSH public key (ssh-ed25519 AAAA…).
   {static const QRegularExpression publishable(QStringLiteral("(?:pk[_,.](?:live|test)[_,.]|sb[_,.]publishable[_,.]|(?:^|[^A-Za-z0-9])pk[.,]|data:[a-z/+.-]+;base64,)$"),QRegularExpression::CaseInsensitiveOption);
    found.erase(std::remove_if(found.begin(),found.end(),[&](const Found& f){
     if(f.length>0&&!f.dotted)return false;
     const int at=f.prefix>=0?f.prefix:segment.starts.at(f.from);
     // ...or holding one in its first piece ("STRIPE_PUBLISHABLE_KEY=pk_live_…" is one word).
     static const QRegularExpression publishableIn(QStringLiteral("(?:^|[^A-Za-z0-9])(?:pk[_,.](?:live|test)[_,.]|sb[_,.]publishable[_,.]|pk[.,]ey)"),QRegularExpression::CaseInsensitiveOption);
         const int runEnd=f.to+1<segment.starts.size()?segment.starts.at(f.to+1):int(segment.text.size());
     const int firstEnd=f.from+1<segment.starts.size()?segment.starts.at(f.from+1):int(segment.text.size());
     const auto run=segment.text.mid(at,qMax(0,runEnd-at));
     // Each exemption only for the public payload itself: the publishable prefix in the run's first
     // piece, an SSH key's AAAA… blob (Sol on #1903).
     static const QRegularExpression blob(QStringLiteral("^[\"'`]?(?:ssh-(?:rsa|ed25519|dss)|ecdsa-sha2-[a-z0-9]+|sk-(?:ssh|ecdsa)-[a-z0-9@.-]+)?AAAA[A-Za-z0-9+/]"));
     // #1940: a public run may follow up to two short label words the run took in ("4" "integrity"
     // "sha512-…"): letters or digits alone, under 12, lowercase after the first.
     bool shown=f.length==0&&publicRun(run);
     for(int p=f.from;!shown&&f.length==0&&p<f.to&&p<f.from+2;++p){
      static const QRegularExpression label(QStringLiteral("^[A-Za-z0-9][a-z0-9]{0,11}$"));
      if(!label.match(plain.at(segment.words.at(p))).hasMatch())break;
      const auto rest=segment.text.mid(segment.starts.at(p+1),qMax(0,runEnd-segment.starts.at(p+1)));
      shown=publicRun(rest);
     }
     return publishable.match(segment.text.left(at)).hasMatch()||publishableIn.match(segment.text.mid(at,qMax(0,firstEnd-at))).hasMatch()||(sshPublic&&f.length==0&&blob.match(run).hasMatch())||shown||(f.dotted&&unsignedJwt(segment.text.mid(f.prefix,runEnd-f.prefix)));
    }),found.end());}
   // #1940: a hash word names a digest in the run's own segment or in the label beside it: up to three
   // words and 32 characters either side ("SHA-256", "Git SHA", "checksum":, ENV['hash'] ||=), none
   // holding hex of its own and none a column away (30 characters), never in another window on the same
   // row (tmux, tiling: a "commit <sha>" left of a hex secret ends at its sha). A secret word anywhere on
   // the row still keeps the run covered.
   if(!found.isEmpty()&&!rowDigest)rowDigest=digestLine(line,plain);
   if(!found.isEmpty()&&*rowDigest){
   if(position.isEmpty())for(int j=0;j<line.size();++j)position.insert(line.at(j),j);
   QList<int> near=segment.words;
   {static const QRegularExpression hex(QStringLiteral("[0-9a-fA-F]{7,}"));
    const auto glyphOf=[&](int w){return double(candidates.at(w).rect.width())/qMax<qsizetype>(1,plain.at(w).size());};
    const auto beside=[&](int from,int step){
     qsizetype characters=0;
     for(int j=from+step,count=0;j>=0&&j<line.size()&&count<3;j+=step,++count){
      const int w=line.at(j),inner=line.at(j-step);
      const int gap=step<0?candidates.at(inner).rect.left()-candidates.at(w).rect.right():candidates.at(w).rect.left()-candidates.at(inner).rect.right();
      characters+=plain.at(w).size();
      if(hex.match(plain.at(w)).hasMatch()||characters>32||gap>30*qMax(glyphOf(w),glyphOf(inner)))break;
      if(step<0)near.prepend(w);else near.append(w);
     }
    };
    beside(position.value(segment.words.first()),-1);beside(position.value(segment.words.last()),1);}
   if(digestLine(near,plain))found.erase(std::remove_if(found.begin(),found.end(),[&](const Found& f){
    if(f.length>0)return false;
    const int from=segment.starts.at(f.from),to=f.to+1<segment.starts.size()?segment.starts.at(f.to+1):int(segment.text.size());
    return hexRun(segment.text.mid(from,to-from));
   }),found.end());
   }
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
    int core=int(segment.words.size()),coreEnd=-1;Found last;for(const auto& f:std::as_const(found)){core=qMin(core,f.from);if(f.to>coreEnd){coreEnd=f.to;last=f;}}
    int a=0,b=int(segment.words.size())-1;
    while(a<core&&plainWord(a))++a;
    // #1875: a JSON web token found by its payload lost its head to recognition. The short words
    // just before what is kept that join into the head as read ("ley" "JhbGci0i", "ley" "Jhb",
    // "ky" "JhbGci…", "Jew" "hbGci…", "ley)" "hbGci…") are the token's: kept, up to three words and
    // nine characters, unless the first word kept already starts with the head ("eyewear" "|eyfhbGci…").
    static const QRegularExpression head(QStringLiteral("^(?:[lI|1!jJ[{]?[\"'\u201C\u2018]?e[yw]|[kb]y)[A-Za-z0-9_)\\]}|-]*$"));
    static const QRegularExpression headStart(QStringLiteral("^(?:[lI|1!jJ[{]?[\"'\u201C\u2018]?e[yw]|[kb]y)"));
    if(a<int(segment.words.size())&&!headStart.match(plain.at(segment.words.at(a))).hasMatch()
       &&std::any_of(found.cbegin(),found.cend(),[&](const Found& f){return f.from==core&&f.payload;})){
     QString joined;
     for(int j=a-1;j>=qMax(0,a-3);--j){joined.prepend(plain.at(segment.words.at(j)));if(joined.size()>9)break;if(head.match(joined).hasMatch()){a=j;break;}}
    }
    while(b>coreEnd&&plainWord(b))--b;
    QList<int> words=segment.words.mid(a,b-a+1);
    // A token the line wrapped: the next line's first segment, when it starts
    // the line and is token-shaped, belongs to it.
    // #1820: a token soft-wrapped over several lines (a JWT in an editor) takes each following line
    // while that line is all token and the token runs on; a short remainder ("08edc") counts too.
    // #1850: the quote the token's line opens and leaves open ("SIGNING_HEX =" then "\"6311…343"),
    // which a short remainder of the token closes on the next line ("edc\"", "edc\")," ).
    // Recognition reads the closing quote straight or curly ("edc\u201D" in 82 of the 101 misses
    // measured with a straight one only), so each opening quote closes with any of its forms.
    // A quote is open when its family (straight and curly forms together, and a prime with the single
    // quotes) is counted an odd number of times on the token's line.
    QString closing;
    {
     QString first;for(const int w:words)first+=plain.at(w);
     const auto odd=[&](const QString& family){qsizetype n=0;for(const auto c:std::as_const(first))if(family.contains(c))++n;return n%2==1;};
     for(const auto& family:{QStringLiteral("\"\u201C\u201D"),QStringLiteral("'\u2018\u2019\u2032"),QStringLiteral("`")})if(odd(family)){closing=family;break;}
    }
    // #1856: a prefixed token of bounded length takes wrapped remainders only
    // up to half again its nominal length, counted from its prefix (a JSON web
    // token has no bound), and never a line that holds a card, an address or
    // another token of its own: a token ending its line took the two card lines
    // below it as one key. A hex or base64 run that holds a card itself is card
    // lines read as a key (#1846): it takes only more lines of card digits,
    // which hold the misread cards (Luhn fails) it covers, or the tail of a
    // hex fragment after the card. A hex or base64 run after the card on its line is a token of its
    // own, followed as any is.
    // #1859: and only a line that a wrap would leave: right below, within about
    // a line's pitch (word boxes are shorter than the pitch), starting no
    // further right than the line above (an editor's wrap indent is up to four
    // characters).
    bool ownLine=false,hexAfter=false;
    if(last.length==0){
     const auto run=segment.words.mid(last.from,last.to-last.from+1);QSet<int> cards;
     if(otherMatch(run,plain,candidates,&cards)&&!cards.isEmpty()){
      qsizetype after=-1;for(qsizetype i=0;i<run.size();++i)if(cards.contains(run.at(i)))after=i;
      QString rest;for(qsizetype i=after+1;i<run.size();++i)rest+=plain.at(run.at(i));
      ownLine=!hexRun(rest)&&!base64Run(rest);
      // Hex as recognition misreads it, after any opening quote.
      static const QString quotes=QStringLiteral("\"'`\u201C\u2018\u00AB");
      qsizetype from=0;while(from<rest.size()&&quotes.contains(rest.at(from)))++from;
      hexAfter=from<rest.size()&&std::all_of(rest.cbegin()+from,rest.cend(),[](QChar c){c=hexConfusions().value(c,c);return c.isDigit()||(c>='a'&&c<='f')||(c>='A'&&c<='F');});
     }
    }
    const auto box=[&](const QList<int>& line){QRect r;int characters=0,inked=0;for(const int i:line){r=r.united(candidates.at(i).rect);characters+=int(plain.at(i).size());inked+=candidates.at(i).rect.width();}return std::pair{r,double(inked)/qMax(1,characters)};};
    int used=last.length>0?int(segment.text.size())-last.prefix:0;
    QString ending=segment.text;
    // A JSON web token with its header, payload and signature dots read is whole: a token on the
    // next line is another one (Sol on #1883). Counted from its prefix, so a dotted label before it
    // ("auth.session.jwt=") does not count, plus the header's dot of a token found by its payload.
    int dots=0;if(last.dotted){const auto token=segment.text.mid(last.prefix);dots=int(token.count('.')+token.count(','))+(last.payload?1:0);}
    for(int follow=k+1;follow<keys.size()&&words.last()==lines[keys.at(follow-1)].last();++follow){
     const auto& nextLine=lines[keys.at(follow)];
     {const auto [above,character]=box(lines[keys.at(follow-1)]);const auto below=box(nextLine).first;
      if(below.top()<=above.top()||below.top()-above.top()>3*qMax(above.height(),below.height())||below.left()>above.left()+4.5*character)break;}
     auto& nextFacts=factsOf(keys.at(follow));const auto& next=nextFacts.segments;
     // A remainder of letters alone that closes the quote the token opened: the next line's first
     // word, at most seven token characters, then the closing quote and only closing brackets or
     // separators ("edc\");"). Checked first, since its punctuation can keep it from being a token piece.
     if(!closing.isEmpty()&&follow==k+1){
      const auto& tailWord=plain.at(nextLine.first());
      qsizetype at=-1;for(qsizetype i=0;i<tailWord.size()&&at<0;++i)if(closing.contains(tailWord.at(i)))at=i;
      static const QRegularExpression alphabet(QStringLiteral("^[A-Za-z0-9+/=_-]{1,7}$")),after(QStringLiteral("^[)\\]};,]*$"));
      if(at>0&&alphabet.match(tailWord.left(at)).hasMatch()&&after.match(tailWord.mid(at+1)).hasMatch()){words.append(nextLine.first());break;}
     }
     if(next.isEmpty()||next.first().words.first()!=nextLine.first()||nextFacts.pem)break;
     // A new statement, or beside a line number on the token's own line another numbered line.
     {static const QRegularExpression number(QStringLiteral("^\\d{1,5}$"));
      if(nextFacts.statement||(nextFacts.numbered&&number.match(plain.at(line.first())).hasMatch()))break;}
     // The remainder of a token, not a line of words: a digit or a capital,
     // and few letters in lowercase runs of five or more.
     const auto& tail=next.first().text;const bool digit=nextFacts.digit,upper=nextFacts.upper;const qsizetype lowercase=nextFacts.lowercase;
     // A short remainder is one recognized word on its own, as a wrap leaves it, never words run together ("id42 ok").
     // ...and it ends the token where a wrap would: a closing quote, bracket or separator ("08edc\"").
     static const QString terminators=QStringLiteral("\"'`)]};,\u201D\u2019\u00BB");
     const auto& lastWord=plain.at(next.first().words.last());
     const bool shortTail=tail.size()>=3&&tail.size()<8&&digit&&!nextFacts.longWord&&next.first().words.size()==1&&!lastWord.isEmpty()&&terminators.contains(lastWord.back());
     if(!((tail.size()>=8||shortTail)&&(digit||upper)&&lowercase*10<=tail.size()*3&&tail.size()<=qMax(8,int(segment.text.size()))+2))break;
     if(last.length>0&&!last.dotted&&used+tail.size()>last.length*3/2)break;
     if(ownLine){
      // More card digits, or the hex run's own wrapped tail ("5ad015a3"), never words such as "SIGNING_HEX =".
      static const QRegularExpression hexTail(QStringLiteral("^[0-9A-Fa-f]+[\"'`)\\]};,\u201D\u2019\u00BB]*$"));
      int count=0;for(const int w:next.first().words)count+=digits(digitView(plain.at(w)));
      if(count<13&&!(hexAfter&&digit&&hexTail.match(tail).hasMatch()))break;
     }
     else if(otherOf(nextFacts))break;
     {const auto& own=ownOf(nextFacts);
      // A JSON web token wrapped just after one of its dots starts its next line with "eyJ"; one
      // wrapped inside its header (read as a JWT or a base64 run above) shows its payload's eyJ on
      // the next line (#1875).
      const bool part=last.dotted&&(ending.endsWith('.')||ending.endsWith(','));
      if(std::any_of(own.cbegin(),own.cend(),[&](const Found& f){return f.length>0&&!(((part&&dots<=2)||((last.length==0||(last.dotted&&dots<2))&&f.payload))&&f.dotted);}))break;}
     used+=int(tail.size());ending=tail;
     // A remainder ends at the quote that closes the token ("…==”" "// note" stays out).
     {bool closed=false;
      for(const int w:next.first().words){words.append(w);const auto& t=plain.at(w);if(!closing.isEmpty()&&!t.isEmpty()&&(closing.contains(t.back())||(t.size()>1&&closing.contains(t.at(t.size()-2))&&QStringLiteral(",;)]}").contains(t.back())))){closed=true;break;}}
      if(closed)break;}
     if(next.first().words.last()!=nextLine.last())break;
    }
    out.append(words);
   }
  }
 }
 // #1894: matches that share a word are one secret (a token and the assignment it is the value of),
 // joined in one pass over their words (#1897: overlapping wrap chains make many such matches).
 {QList<int> parent(out.size());std::iota(parent.begin(),parent.end(),0);
  const auto root=[&](int i){while(parent[i]!=i){parent[i]=parent[parent[i]];i=parent[i];}return i;};
  QHash<int,int> owner;
  for(int i=0;i<out.size();++i)for(const int w:out.at(i)){const auto it=owner.constFind(w);if(it==owner.cend())owner.insert(w,i);else parent[root(i)]=root(it.value());}
  QMap<int,QList<int>> merged;
  for(int i=0;i<out.size();++i){auto& words=merged[root(i)];words+=out.at(i);}
  QList<QList<int>> joined;
  for(auto words:std::as_const(merged)){std::sort(words.begin(),words.end());words.erase(std::unique(words.begin(),words.end()),words.end());joined.append(words);}
  out=joined;}
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
// #1895: an ISBN-13 (978 or 979, its check digit right) is a book's number, never a card or a phone.
bool isbn13(const QString& text) {
 QString d;for(const auto c:text)if(c.isDigit())d+=c;
 if(d.size()!=13||!(d.startsWith(QLatin1String("978"))||d.startsWith(QLatin1String("979"))))return false;
 int sum=0;for(int i=0;i<13;++i)sum+=d.at(i).digitValue()*(i%2?3:1);
 return sum%10==0;
}
// 0: as read; 1: digits; 2: hex; 3: email.
const QList<Rule>& rules() {
 static const QList<Rule> list{
  // #1942: a local part may hold any letter (josé, zoë). No address starts right after another "@"
  // (a fediverse handle, @me@mastodon.social), ends in a systemd unit's or an image file's suffix
  // (user@1000.service, icon@2x.png), or goes on with ":" and a path (git@github.com:org/repo.git;
  // a port, @db.internal:5432/app, still ends one, and so does a word, jane@example.com:urgent).
  // The last label ends the domain, so a shorter domain is never taken (user@foo.bar.service).
  {"email",QRegularExpression(R"((?<![\p{L}\p{M}\p{N}_.%+@-])[\p{L}\p{N}][\p{L}\p{M}\p{N}._%+-]*@[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?)*\.(?!(?i:service|socket|mount|automount|timer|slice|scope|device|swap|path|png|jpe?g|gif|svg|webp|ico|bmp|tiff?|avif|heic)(?![\w-]))[A-Za-z]{2,}(?![\w-]|\.[A-Za-z0-9]|:(?!\d+/)[\w.~+-]*(?:/|\.git(?![\w-]))))"),{},3},
  {"card",QRegularExpression(R"((?<![\d-])(?<!\d\.)\d(?:[ -]?\d){12,18}(?!\d))"),[](const QString& m){QString d;for(const auto c:m)if(c.isDigit())d+=c;return d.size()>=13&&d.size()<=19&&luhn(d)&&!isbn13(d);},1},
  {"ip",QRegularExpression(R"((?<![\d.vV])(?:(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)\.){3}(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)(?:/\d{1,2}|:\d{1,5})?(?!\d|\.\d))"),{},1},
  {"ip",QRegularExpression(R"((?<![\w:])(?:[0-9A-Fa-f]{0,4}:){2,7}[0-9A-Fa-f]{0,4}(?![\w:]))"),validIpv6,2},
  {"phone",QRegularExpression(R"((?<![\w+.-])(?<!(?<![A-Za-z0-9])\d\d:)(?:\+\d{1,3}[ .-]?)?(?:\(\d{1,4}\)[ .-]?)?\d{1,4}(?:[ .-]?\d{2,4}){1,5}(?![\w-]|\.\d|:\d\d))"),[](const QString& m){
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
    // #1895: an ISBN-13 is no number to call, nor is a date with the hour after it (2026-11-22 14,
    // cut at the time's colon). A ZIP+4 code is left out where its line says so (findOrdered).
    static const QRegularExpression dated(R"(^\d{4}-\d{1,2}-\d{1,2}(?:[ T]\d{1,2})?$)");
    if(isbn13(m)||dated.match(m).hasMatch())return false;
    const int count=digits(m);return count>=7&&count<=15&&!date.match(m).hasMatch()&&!dotted.match(m).hasMatch();},1},
 };
 return list;
}
// #1856: words that hold a card number or an address of their own, read as the
// card and email rules read them (an address's lost dot restored, #1840).
bool otherMatch(const QList<int>& words,const QList<QString>& plain,const QList<RecognizedWord>& candidates,QSet<int>* cards) {
 QList<QString> asRead,numbers;QList<RecognizedWord> boxes;
 for(const int w:words){asRead.append(plain.at(w));numbers.append(digitView(plain.at(w)));boxes.append(candidates.at(w));}
 const auto email=emailView(asRead,boxes);
 bool found=false;
 // Cards first, so every card's words are collected before an address answers.
 for(const auto* kind:{"card","email"})for(const auto& rule:rules()){
  if(rule.category!=QLatin1String(kind))continue;
  // #1942: without the gap joins, which guess at an address: a key's next line ("A91de" "@b3fa.aFe")
  // must not read as an address and stop the key's cover (Sol on #1956).
  const auto joined=join(rule.view==1?numbers:email,boxes,false,rule.category,{},false);
  for(auto it=rule.pattern.globalMatch(joined.text);it.hasNext();){
   const auto m=it.next();if(rule.valid&&!rule.valid(m.captured()))continue;
   found=true;if(!cards||rule.category!="card")return true;
   for(const int i:owners(joined,m.capturedStart(),m.capturedLength()))cards->insert(words.at(i));
  }
 }
 return found;
}
}
QStringList categories() {return {"email","phone","ip","card","key","name","list"};}
QStringList keyShapes() {return {"AWS access key IDs","GitHub tokens (ghp_, gho_, ghu_, ghs_, ghr_, github_pat_)","GitLab tokens (glpat- and the other gl prefixes)","Slack tokens (xox…, xapp-)","Stripe secret keys (sk_, rk_, whsec_)","OpenAI, Anthropic, Google, Hugging Face, npm, PyPI, SendGrid, DigitalOcean, Shopify, Twilio, Mailgun, Square, Docker Hub, Grafana, Vault, Postman, New Relic, Linear, Atlassian and Sentry tokens","Passwords in connection strings (postgres://user:password@…)","Values of secret-named settings (PASSWORD=, API_KEY:, \"client_secret\": …)","Slack, Discord, Teams, Zapier, Google Chat and Azure webhook URLs, and Telegram bot tokens","JSON web tokens","PEM private-key blocks","Unbroken hex or base64 runs of 32 characters or more, except digests beside a hash word"};}
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
// #1942: the login as a prompt word is compared: case folded, with I, l, 1 and | read
// as one character and O and 0 as another; logins of 3 characters or more.
QString promptLogin(const QString& login) {
 QString out;for(const auto c:literalView(login))out+=confusion(c);
 return points(out)>=3?out:QString();
}
bool promptHolds(const QString& word,const QString& login) {
 static const QRegularExpression prompt(QStringLiteral("^\\S{0,2}?(\\S+?)[A-Za-z0-9][A-Za-z0-9.-]*:[~/]\\S*$"));
 if(!prompt.match(word).hasMatch())return false;
 QString folded;for(const auto c:word)folded+=confusion(c);
 // Up to two glyphs of a window edge or caret before it, read as | l I 1 j ! ( or [ (folded to l).
 static const QString edge=QStringLiteral("lj!([");
 qsizetype from=0;
 while(from<2&&from<folded.size()&&!folded.mid(from).startsWith(login)&&edge.contains(folded.at(from)))++from;
 if(!folded.mid(from).startsWith(login))return false;
 // An "@" recognition kept before the host's colon names the login: it must follow it at once (Sol on #1956: with login
 // alice, malice@host and alicedev@host are other logins).
 const auto at=folded.indexOf('@');
 if(at>=0&&at<folded.indexOf(':'))return at==from+login.size();
 // The login, then the host: at least one more character before the colon.
 return from+login.size()<folded.indexOf(':');
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
namespace {
// #1911: the second read's word boxes, and whether one of them overlaps a
// first-read word on its row (the second word's height holds its centre, and
// their columns meet), which the second read then replaces.
QList<QRect> secondBoxes(const QList<RecognizedWord>& candidates){
 QList<QRect> out;for(const auto& w:candidates)if(w.second)out.append(w.rect);
 return out;
}
bool overlappedBySecond(const QRect& own,const QList<QRect>& seconds){
 const auto centre=own.center().y();
 return std::any_of(seconds.cbegin(),seconds.cend(),[&](const QRect& r){return centre>=r.top()&&centre<=r.bottom()&&r.left()<=own.right()&&r.right()>=own.left();});
}
// The words a match was found among: the first read alone, or for a match
// holding a second-read word, the page with the second read in place. Asked
// only of the words on a match's rows, so a dense page stays cheap.
struct MatchView {
 QList<QRect> seconds;bool second=false;
 MatchView(const QList<RecognizedWord>& candidates,const Match& match):seconds(secondBoxes(candidates)),
  second(std::any_of(match.words.cbegin(),match.words.cend(),[&](int w){return w>=0&&w<candidates.size()&&candidates.at(w).second;})){}
 bool sees(const RecognizedWord& word)const{
  if(seconds.isEmpty())return true;
  return second?(word.second||!overlappedBySecond(word.rect,seconds)):!word.second;
 }
};
}
namespace {
// Recognition may give one visual line as two layout blocks, so the end of a
// line arrives after the lines below it. Matching runs over lines as they
// appear: words grouped by the height of their centres (within half the
// median word height) and ordered left to right; the matches are mapped back.
Result findRead(const QList<RecognizedWord>& candidates,const QStringList& chosen,const Sources& sources,int budgetMs,const std::function<void(const Result&)>& progress,const std::atomic_bool* cancel,const PatternMatcher& matcher) {
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
   // #1942: a shell prompt whose "@" recognition read as a letter or two, or lost
   // ("mriveraPmint:~$", "hsatofinova:~$", "ychenorion:~$"), runs the login into the host,
   // so no word edge ends it. A prompt-shaped word (host, ":", then "~" or "/") that starts
   // with the login, after at most two characters of window edge or caret ("|lsoconnor@…"),
   // is the login's.
   if(const auto login=promptLogin(sources.login);!login.isEmpty())
    for(int w=0;w<ordered.size();++w)if(!taken.contains(w)&&promptHolds(literal.at(w),login))claim("name",{w},taken,result.matches);
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
}
// #1911: words from the second read of an uncertain token's line are matched
// apart from the first read's. Side by side on one row the two readings of a
// line would be taken for one ("SHORT_SHA=2f6424a SHORT_SHA=2f6424a"). The
// second read is matched in the first read's page instead: its words take the
// place of the first read's they overlap, so the words around them (a key's
// name beside or above it, a PEM block's header) still count; read on their own, PEM
// public-key lines were taken for keys. Only matches holding a second-read
// word are added to the first read's; they get what is left of the budget.
Result find(const QList<RecognizedWord>& candidates,const QStringList& chosen,const Sources& sources,int budgetMs,const std::function<void(const Result&)>& progress,const std::atomic_bool* cancel,const PatternMatcher& matcher) {
 QList<int> firstRead,secondRead;
 for(int i=0;i<candidates.size();++i)(candidates.at(i).second?secondRead:firstRead).append(i);
 if(secondRead.isEmpty())return findRead(candidates,chosen,sources,budgetMs,progress,cancel,matcher);
 QElapsedTimer clock;clock.start();
 // The page with the second read in place: a first-read word leaves when a
 // second-read word overlaps it on its row (centre within that word's height),
 // so a token's name beside it stays.
 QList<int> replaced;
 const auto seconds=secondBoxes(candidates);
 for(const int i:std::as_const(firstRead))if(!overlappedBySecond(candidates.at(i).rect,seconds))replaced.append(i);
 replaced+=secondRead;std::sort(replaced.begin(),replaced.end());
 const auto subset=[&](const QList<int>& indexes){QList<RecognizedWord> out;out.reserve(indexes.size());for(const int i:indexes)out.append(candidates.at(i));return out;};
 const auto back=[&](Result result,const QList<int>& indexes,bool onlySecond){
  for(auto& match:result.matches)for(auto& word:match.words)word=indexes.at(word);
  if(onlySecond)result.matches.removeIf([&](const Match& m){return std::none_of(m.words.cbegin(),m.words.cend(),[&](int w){return candidates.at(w).second;});});
  return result;
 };
 const auto merged=[](Result into,const Result& more){
  into.matches+=more.matches;
  for(const int p:more.skippedPatterns)if(!into.skippedPatterns.contains(p))into.skippedPatterns.append(p);
  into.nameUnchecked|=more.nameUnchecked;into.unfinished|=more.unfinished;
  std::stable_sort(into.matches.begin(),into.matches.end(),[](const Match& a,const Match& b){return a.words.first()<b.words.first();});
  return into;
 };
 std::function<void(const Result&)> firstProgress,secondProgress;
 if(progress)firstProgress=[&](const Result& partial){progress(back(partial,firstRead,false));};
 const auto one=back(findRead(subset(firstRead),chosen,sources,budgetMs,firstProgress,cancel,matcher),firstRead,false);
 // The first read's matches are published before the second is matched (Sol on #1920).
 if(progress)progress(one);
 if(cancel&&cancel->load(std::memory_order_relaxed))return one;
 // A second-read match the first read already covers, same kind over the same
 // place, is one finding, not two.
 // Each of its words must lie inside one word of that match, give or take 2 px:
 // a match's bounding box would take in a gap between its rows, and a longer
 // reading reaches past the first one's cover (Sol on #1920).
 const auto under=[&](const QRect& word,const Match& f){
  return std::any_of(f.words.cbegin(),f.words.cend(),[&](int w){return candidates.at(w).rect.adjusted(-2,-2,2,2).contains(word);});
 };
 const auto fresh=[&](Result second){
  second.matches.removeIf([&](const Match& m){
   return std::any_of(one.matches.cbegin(),one.matches.cend(),[&](const Match& f){
    return f.category==m.category&&std::all_of(m.words.cbegin(),m.words.cend(),[&](int w){return under(candidates.at(w).rect,f);});});});
  return second;
 };
 if(progress)secondProgress=[&](const Result& partial){progress(merged(one,fresh(back(partial,replaced,true))));};
 const int left=budgetMs>0?std::max(0,budgetMs-int(clock.elapsed())):budgetMs;
 return merged(one,fresh(back(findRead(subset(replaced),chosen,sources,left,secondProgress,cancel,matcher),replaced,true)));
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
    // #1895: a ZIP+4-shaped number after a ZIP word or a state code, and any number on a line that
    // names an ISBN, are no phone numbers; 9XXXX-XXXX alone stays one (a Brazilian mobile).
    if(rule.category=="phone"){
     static const QRegularExpression zip(R"(^\d{5}-\d{4}$)"),zipWord(QStringLiteral("^(?:zip|zip:|zipcode|zip-code|postal|postcode)$"),QRegularExpression::CaseInsensitiveOption);
     static const QSet<QString> states{"AL","AK","AZ","AR","CA","CO","CT","DE","FL","GA","HI","ID","IL","IN","IA","KS","KY","LA","ME","MD","MA","MI","MN","MS","MO","MT","NE","NV","NH","NJ","NM","NY","NC","ND","OH","OK","OR","PA","RI","SC","SD","TN","TX","UT","VT","VA","WA","WV","WI","WY","DC","PR"};
     const int first=words.first();const int line=candidates.at(first).line;
     const auto number=joined.text.mid(found.capturedStart(),length);
     if(zip.match(number).hasMatch()){
      // A ZIP+4 is no phone, except one shaped as a Brazilian mobile (9XXXX-XXXX) with no ZIP word or
      // state code before it.
      QString before=first>0&&candidates.at(first-1).line==line?candidates.at(first-1).text:QString();
      while(before.endsWith(',')||before.endsWith(':'))before.chop(1);
      if(!number.startsWith('9')||zipWord.match(before).hasMatch()||states.contains(before))continue;
     }
     // Inside an ISBN's own number ("ISBN 978 4 579 64374 5", "ISBN-13: 978-…"): the words after an
     // ISBN word made of digits and hyphens, up to 13 digits.
     {QSet<int> isbnWords;
      for(int w=0;w<candidates.size();++w){
       if(candidates.at(w).line!=line||!candidates.at(w).text.startsWith(QLatin1String("ISBN"),Qt::CaseInsensitive))continue;
       static const QRegularExpression part(QStringLiteral("^[0-9Xx-]+[,.;]?$"));
       // The number ends at its own punctuation ("0-306-40615-2,") or at 10 or 13 digits.
       int count=0;for(int v=w+1;v<candidates.size()&&candidates.at(v).line==line&&count<13&&part.match(candidates.at(v).text).hasMatch();++v){
        isbnWords.insert(v);count+=digits(candidates.at(v).text)+int(candidates.at(v).text.count('X')+candidates.at(v).text.count('x'));
        const auto& t=candidates.at(v).text;if(t.endsWith(',')||t.endsWith(';')||t.endsWith('.')||count==10||count==13)break;}
      }
      if(!isbnWords.isEmpty()&&std::all_of(words.cbegin(),words.cend(),[&](int w){return isbnWords.contains(w);}))continue;}
    }
    // #1942: a mail header's message identifiers (Message-ID, In-Reply-To, References)
    // are shaped like addresses and are none.
    if(rule.category=="email"){
     // The header's field name, colon and all, starts the line; the word in prose does not count.
     static const QRegularExpression header(QStringLiteral("^(?:Message-ID|In-Reply-To|References|X-Message-ID|Resent-Message-ID):$"),QRegularExpression::CaseInsensitiveOption);
     const int line=candidates.at(words.first()).line;int first=-1;
     for(int w=0;w<candidates.size();++w)if(candidates.at(w).line==line&&(first<0||candidates.at(w).rect.left()<candidates.at(first).rect.left()))first=w;
     // Recognition may give the colon as a word of its own ("References" ":"), Sol on #1956.
     int second=-1;
     for(int w=0;w<candidates.size();++w)if(w!=first&&candidates.at(w).line==line&&candidates.at(w).rect.left()>=candidates.at(first).rect.left()&&(second<0||candidates.at(w).rect.left()<candidates.at(second).rect.left()))second=w;
     const auto& field=candidates.at(first).text;
     const bool apart=second>=0&&candidates.at(second).text==QLatin1String(":");
     if(!words.contains(first)&&(header.match(field).hasMatch()||(apart&&header.match(field+QLatin1Char(':')).hasMatch())))continue;
    }
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
// Whether the unmatched word `other` beside the matched box `box` (direction
// `dir`, -1 left or +1 right) is part of the same secret, joined to it by
// punctuation recognition dropped: "jane.doe@example.com" read as "jane" and
// "doe@example.com" with a word space's gap between the boxes (#1854). It is
// when, between the two boxes, there is a blob of ink (4-connected, since a
// dot can touch the next glyph at a corner) that touches neither box's columns
// and is at most half the row's height, such as a dot, comma, hyphen or
// underscore, and no blank run from the matched ink to the neighbour's box is
// wider than a quarter glyph, as a word space is. The edge of a glyph a box
// cut off is part of that glyph's blob, and a whole glyph is taller. Blobs are
// read at half the text's contrast and at a quarter, so a solid dot whose
// anti-aliased edge touches a letter, and a faint 2 px dot, are both seen.
// Paper and blank columns are measured as reach() does. Joining only adds to a
// cover; without the picture nothing is joined.
bool joined(const QImage& image,const QRect& box,const QRect& other,int dir,double glyph,int pad){
 if(image.isNull())return false;
 const int edge=dir>0?box.right():box.left(),near=dir>0?other.left():other.right(),at=dir*(near-edge),slack=int(std::ceil(glyph));
 if(at<2)return false;
 const QRect window=QRect(QPoint(qMin(edge-dir*slack,near+dir*slack),box.top()-pad),QPoint(qMax(edge-dir*slack,near+dir*slack),box.bottom()+pad)).intersected(image.rect());
 const auto index=[&](int x){return dir*(x-edge);}; // columns counted from the edge (0) towards the neighbour
 if(window.isEmpty()||!window.contains(QPoint(edge,window.top()))||!window.contains(QPoint(near,window.top())))return false;
 const QRect around=window.adjusted(0,-pad,0,pad).intersected(image.rect());
 const auto band=image.copy(around).convertToFormat(QImage::Format_ARGB32);
 const int first=window.top()-around.top(),last=window.bottom()-around.top();
 QList<int> channels[3];
 for(int y=0;y<band.height();++y){
  const auto* line=reinterpret_cast<const QRgb*>(band.constScanLine(y));
  for(int x=0;x<band.width();++x){
   if(qAlpha(line[x])!=255)return false;
   if(y>=first&&y<=last)continue;
   channels[0].append(qRed(line[x]));channels[1].append(qGreen(line[x]));channels[2].append(qBlue(line[x]));
  }
 }
 if(channels[0].isEmpty())return false;
 int paper[3];
 for(int k=0;k<3;++k){auto c=channels[k];std::nth_element(c.begin(),c.begin()+c.size()/2,c.end());paper[k]=c.at(c.size()/2);}
 const auto distance=[&](QRgb c){return qMax(std::abs(qRed(c)-paper[0]),qMax(std::abs(qGreen(c)-paper[1]),std::abs(qBlue(c)-paper[2])));};
 qsizetype stray=0;for(qsizetype i=0;i<channels[0].size();++i)if(qMax(std::abs(channels[0].at(i)-paper[0]),qMax(std::abs(channels[1].at(i)-paper[1]),std::abs(channels[2].at(i)-paper[2])))>24)++stray;
 if(stray*4>channels[0].size())return false;
 // Each pixel's distance from the paper over the mark's rows, by column index.
 const int lo=qMin(index(window.left()),index(window.right())),hi=qMax(index(window.left()),index(window.right()));
 const int rows=last-first+1,width=hi-lo+1;
 QList<int> ink(rows*width,0);int contrast=0;
 for(int y=0;y<rows;++y){
  const auto* line=reinterpret_cast<const QRgb*>(band.constScanLine(first+y));
  for(int x=0;x<band.width();++x){const int d=distance(line[x]);ink[y*width+index(around.left()+x)-lo]=d;contrast=qMax(contrast,d);}
 }
 const int faint=std::clamp(contrast*8/100,4,24);
 const auto blank=[&](int i){for(int y=0;y<rows;++y)if(ink.at(y*width+i-lo)>faint)return false;return true;};
 int i=0;while(i>lo&&blank(i))--i; // the edge word's last ink
 if(blank(i))return false;
 const int space=qMax(1,int(glyph/4));
 for(int run=0;++i<=at;)if(blank(i)){if(++run>space)return false;}else run=0;
 for(const int share:{2,4}){
  QList<bool> seen(rows*width,false);
  const auto solid=[&](int y,int x){return ink.at(y*width+x-lo)*share>=contrast;};
  for(int y=0;y<rows;++y)for(int x=1;x<at;++x){
   if(!solid(y,x)||seen.at(y*width+x-lo))continue;
   int left=x,right=x,top=y,bottom=y,pixels=0;QList<QPair<int,int>> todo{{x,y}};seen[y*width+x-lo]=true;
   while(!todo.isEmpty()){
    const auto [px,py]=todo.takeLast();++pixels;
    left=qMin(left,px);right=qMax(right,px);top=qMin(top,py);bottom=qMax(bottom,py);
    for(const auto& [dx,dy]:{QPair<int,int>{1,0},{-1,0},{0,1},{0,-1}}){
     const int nx=px+dx,ny=py+dy;
     if(ny<0||ny>=rows||nx<lo||nx>hi||seen.at(ny*width+nx-lo)||!solid(ny,nx))continue;
     seen[ny*width+nx-lo]=true;todo.append({nx,ny});
    }
   }
   // At half the contrast a lone pixel can be the core of a letter's edge (the foot of a "j"),
   // so a blob there needs two.
   if(left>=1&&right<at&&(bottom-top+1)*2<=box.height()&&(share==4||pixels>=2))return true;
  }
 }
 return false;
}
}
// One box per visual row of the match, as find groups rows (centres within
// half the median word height), never by the recognizer's line id: a match
// that spans two rows makes two marks, so nothing between them is covered.
QList<MarkBox> markBoxes(const QList<RecognizedWord>& candidates,const Match& match,const QRect& bounds,const QImage& image) {
 QList<int> words;for(const int index:match.words)if(index>=0&&index<candidates.size())words.append(index);
 // #1911: neighbours come from the words the match was found among.
 const MatchView view(candidates,match);
 std::stable_sort(words.begin(),words.end(),[&](int a,int b){return candidates.at(a).rect.center().y()<candidates.at(b).rect.center().y();});
 QList<int> heights;for(const int w:words)heights.append(candidates.at(w).rect.height());std::sort(heights.begin(),heights.end());
 const double tolerance=qMax(2.0,heights.isEmpty()?2.0:heights.at(heights.size()/2)*0.5);
 // whole: the visual row's box, whose padded height every run keeps; inner: the run's sides that
 // face another run of the same row.
 struct Row {QRect box;double centre=0;int count=0;int first=-1;int last=-1;QList<int> members;QRect whole;bool innerLeft=false,innerRight=false;};
 QList<Row> grouped;
 for(const int w:words){
  const auto& word=candidates.at(w);const double y=word.rect.center().y();
  if(grouped.isEmpty()||std::abs(y-grouped.last().centre)>tolerance)grouped.append(Row{});
  auto& row=grouped.last();++row.count;row.centre+=(y-row.centre)/row.count;row.members.append(w);
 }
 // #1846: a row's matched words more than three line heights apart are in separate columns (two
 // windows side by side, a table), so each run of them gets a mark of its own and the space
 // between the columns stays readable. Words inside a token, a number or a sentence are never that
 // far apart.
 QList<Row> rows;
 for(auto row:std::as_const(grouped)){
  for(const int w:std::as_const(row.members))row.whole=row.whole.united(candidates.at(w).rect);
  std::stable_sort(row.members.begin(),row.members.end(),[&](int a,int b){return candidates.at(a).rect.left()<candidates.at(b).rect.left();});
  QList<int> rowHeights;for(const int w:std::as_const(row.members))rowHeights.append(candidates.at(w).rect.height());std::sort(rowHeights.begin(),rowHeights.end());
  const int column=3*qMax(1,rowHeights.at(rowHeights.size()/2));
  Row part;part.centre=row.centre;part.whole=row.whole;int reached=0;
  for(const int w:std::as_const(row.members)){
   const auto& rect=candidates.at(w).rect;
   if(part.first>=0&&rect.left()-reached>column){part.innerRight=true;rows.append(part);part=Row{};part.centre=row.centre;part.whole=row.whole;part.innerLeft=true;}
   part.box=part.box.united(rect);++part.count;
   if(part.first<0||rect.left()<candidates.at(part.first).rect.left())part.first=w;
   if(part.last<0||rect.right()>candidates.at(part.last).rect.right())part.last=w;
   reached=qMax(part.first==w?rect.right():reached,rect.right());
  }
  rows.append(part);
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
  const auto& box=row.box;const int pad=qMax(1,(row.whole.height()+3)/4);
  const double before=glyph(row.first),after=glyph(row.last);
  int left=box.left()-qMax(pad,int(std::ceil(before*1.5)));
  int right=box.right()+qMax(pad,int(std::ceil(after*1.5)));
  std::optional<QRect> leftNeighbour,rightNeighbour;
  for(qsizetype i=0;i<candidates.size();++i){
   // #1911: only words of the match's own read; the other reading of the line overlaps it.
   if(match.words.contains(int(i)))continue;
   const auto& other=candidates.at(i).rect;
   if(other.isEmpty()||std::abs(other.center().y()-row.centre)>tolerance||!view.sees(candidates.at(i)))continue;
   // Only a word the margin, plus a glyph for the boxes' error, could reach.
   if(other.right()<box.left()&&other.right()>=left-int(std::ceil(before))){if(!leftNeighbour||other.right()>leftNeighbour->right())leftNeighbour=other;}
   else if(other.left()>box.right()&&other.left()<=right+int(std::ceil(after))){if(!rightNeighbour||other.left()<rightNeighbour->left())rightNeighbour=other;}
  }
  // A side split off from the rest of its row keeps the whole margin (#1846): the neighbour test
  // was measured on row ends, and a word there may be matched ink the box missed.
  // The scan covers the run's width and the whole row's height, as the mark does.
  const QRect scan(QPoint(box.left(),row.whole.top()),QPoint(box.right(),row.whole.bottom()));
  if(leftNeighbour&&!row.innerLeft)if(const auto limit=reach(image,scan,*leftNeighbour,-1,before,pad))left=qMax(left,*limit);
  if(rightNeighbour&&!row.innerRight)if(const auto limit=reach(image,scan,*rightNeighbour,1,after,pad))right=qMin(right,*limit);
  // A neighbour joined to the match by a dropped dot (joined()) is covered with it, and so is the
  // next one it joins; from the last, the margin and reach() go on as above. The side's edge only
  // ever moves outward.
  QRect rowBox=row.whole;
  for(const int dir:{-1,1}){
   if(dir<0?row.innerLeft:row.innerRight)continue;
   QSet<int> taken;int edgeWord=dir<0?row.first:row.last;QRect grown=box;
   for(;;){
    const double g=glyph(edgeWord);const int margin=qMax(pad,int(std::ceil(g*1.5)));
    int nearest=-1;
    for(qsizetype i=0;i<candidates.size();++i){
     if(match.words.contains(int(i))||taken.contains(int(i)))continue;
     const auto& other=candidates.at(i).rect;
     if(other.isEmpty()||std::abs(other.center().y()-row.centre)>tolerance||!view.sees(candidates.at(i)))continue;
     const QRect best=nearest<0?other:candidates.at(nearest).rect;
     if(dir<0&&other.right()<grown.left()&&other.right()>=grown.left()-margin-int(std::ceil(g))&&other.right()>=best.right())nearest=int(i);
     if(dir>0&&other.left()>grown.right()&&other.left()<=grown.right()+margin+int(std::ceil(g))&&other.left()<=best.left())nearest=int(i);
    }
    const QRect scanned(QPoint(grown.left(),rowBox.top()),QPoint(grown.right(),rowBox.bottom()));
    if(nearest>=0&&joined(image,scanned,candidates.at(nearest).rect,dir,g,pad)){
     grown=grown.united(candidates.at(nearest).rect);rowBox=rowBox.united(candidates.at(nearest).rect);taken.insert(nearest);edgeWord=nearest;continue;
    }
    if(taken.isEmpty())break;
    int edge=dir<0?grown.left()-margin:grown.right()+margin;
    if(nearest>=0)if(const auto limit=reach(image,scanned,candidates.at(nearest).rect,dir,g,pad))edge=dir<0?qMax(edge,*limit):qMin(edge,*limit);
    if(dir<0)left=qMin(left,edge);else right=qMax(right,edge);
    break;
   }
  }
  const auto mark=QRect(QPoint(left,rowBox.top()-pad),QPoint(right,rowBox.bottom()+pad)).intersected(bounds);if(!mark.isEmpty())out.append({mark,box});
 }
 return out;
}
QList<QRect> marks(const QList<RecognizedWord>& candidates,const Match& match,const QRect& bounds,const QImage& image) {
 QList<QRect> out;for(const auto& box:markBoxes(candidates,match,bounds,image))out.append(box.mark);
 return out;
}
}
