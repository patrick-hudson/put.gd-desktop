// Quick redact (#1428, decision 5): one table over the matching function. A
// row is recognized words in reading order ("|" starts a new line), the
// categories asked for, and every match expected as "category: words".
#include "editor/QuickRedact.h"
#include "fixtures/Waits.h"
#include <QElapsedTimer>
#include <QPainter>
#include <QtTest>
#include <tuple>
using namespace putgd::editor;
namespace {
QList<RecognizedWord> words(const QString& page) {
 QList<RecognizedWord> out;int line=0,x=0;
 for(const auto& token:page.split(' ',Qt::SkipEmptyParts)){
  if(token=="|"){++line;x=0;continue;}
  out.append({token,QRect(x,line*30,int(token.size())*8,20),90,line});x+=int(token.size())*8+8;
 }
 return out;
}
QStringList found(const QList<RecognizedWord>& candidates,const QStringList& categories) {
 QStringList out;
 for(const auto& match:quickRedact::find(candidates,categories)){QStringList text;for(const int w:match.words)text.append(candidates.at(w).text);out.append(match.category+": "+text.join(' '));}
 return out;
}
const QStringList all{"email","phone","ip","card","key"};
QStringList found(const QList<RecognizedWord>& candidates,const quickRedact::Result& result) {
 QStringList out;
 for(const auto& match:result.matches){QStringList text;for(const int w:match.words)text.append(candidates.at(w).text);out.append(match.category+": "+text.join(' '));}
 return out;
}
quickRedact::Sources account(const QString& login,const QString& realName) {quickRedact::Sources s;s.login=login;s.realName=realName;return s;}
quickRedact::Sources terms(const QStringList& list) {quickRedact::Sources s;s.terms=list;return s;}
quickRedact::Sources patterns(const QStringList& list) {quickRedact::Sources s;s.patterns=list;return s;}
}
Q_DECLARE_METATYPE(putgd::editor::quickRedact::Sources)
class QuickRedactTest final:public QObject {
 Q_OBJECT
private slots:
 void categoriesMatchTheirShapes_data() {
  QTest::addColumn<QString>("page");QTest::addColumn<QStringList>("categories");QTest::addColumn<QStringList>("expected");
  QTest::newRow("two phones on one line")<<"2125550100 4155550100"<<QStringList{"phone"}<<QStringList{"phone: 2125550100","phone: 4155550100"};
  const auto prefix=QString("gh")+"p_"+"aB3dE5fG7hJ9kL1mN3pQ"+"5rS7tU9vW1xY3zA5",tail=QString("q7Xb9Lr2Vt");
  QTest::newRow("wrapped token with valid prefix")<<prefix+" | "+tail<<all<<QStringList{"key: "+prefix+" "+tail};
  QTest::newRow("email after unrelated numeric line")<<"ALPHA BRAVO CHARLIE | SYNTHETIC TEXT 1428 | hello@example.test"<<QStringList{"email"}<<QStringList{"email: hello@example.test"};
  QTest::newRow("email after prose line")<<"Please email | john@example.com"<<QStringList{"email"}<<QStringList{"email: john@example.com"};
  QTest::newRow("email hard wrapped at terminal column")<<"ABCDEFGHIJKLMNO | PQRSTUVWXYZABCD | Contact john.sm | ith@example.com"<<QStringList{"email"}<<QStringList{"email: john.sm ith@example.com"};
  QTest::newRow("email")<<"Write to jane.doe+invoices@example.co.uk today"<<all<<QStringList{"email: jane.doe+invoices@example.co.uk"};
  // #1840: a word ended by a dot with a word space after it is not part of the address.
  QTest::newRow("a sentence before an address stays out")<<"Contact us. bob@example.com today"<<QStringList{"email"}<<QStringList{"email: bob@example.com"};
  QTest::newRow("email wrapped across lines")<<"Contact jane.doe@ | example.com for access"<<all<<QStringList{"email: jane.doe@ example.com"};
  QTest::newRow("international phone across words")<<"Call +44 20 7946 0958 now"<<all<<QStringList{"phone: +44 20 7946 0958"};
  QTest::newRow("national phone with parentheses")<<"Office (555) 123-4567 ext"<<all<<QStringList{"phone: (555) 123-4567"};
  QTest::newRow("phone with O and l confused")<<"Mobile 555 O12 34l7"<<all<<QStringList{"phone: 555 O12 34l7"};
  QTest::newRow("phone with S and B confused")<<"Dial 555 S1B 4567"<<all<<QStringList{"phone: 555 S1B 4567"};
  QTest::newRow("six digits are not a phone")<<"Ticket 123 456 closed"<<all<<QStringList{};
  QTest::newRow("a date is not a phone")<<"Released 2026-11-12 and 12/11/2026"<<all<<QStringList{};
  QTest::newRow("IPv4 with S and l confused")<<"Server 192.168.l.S0 answered"<<all<<QStringList{"ip: 192.168.l.S0"};
  QTest::newRow("IPv4 with a port")<<"Listening on 10.0.0.2:8443"<<all<<QStringList{"ip: 10.0.0.2:8443"};
  QTest::newRow("IPv6 compressed")<<"Route via 2001:db8::8a2e:370 here"<<all<<QStringList{"ip: 2001:db8::8a2e:370"};
  QTest::newRow("IPv6 with O confused")<<"Peer fe80::1ff:fe23:4567:89Oa up"<<all<<QStringList{"ip: fe80::1ff:fe23:4567:89Oa"};
  QTest::newRow("code scope and time are not IPv6")<<"std::vector at 12:30:45 a::b"<<all<<QStringList{};
  QTest::newRow("card in groups, Luhn valid")<<"Card 4111 1111 1111 1111 expires"<<all<<QStringList{"card: 4111 1111 1111 1111"};
  // #1820 acceptance: shapes found on real screens.
  QTest::newRow("AWS key after an equals sign")<<"export AWS_ACCESS_KEY_ID=AKIAIOSFODNN7EXAMPLE"<<all<<QStringList{"key: AWS_ACCESS_KEY_ID=AKIAIOSFODNN7EXAMPLE"};
  // The token's whole segment is covered, its identifier-like name too, as for any token (#1618).
  QTest::newRow("AWS key in quotes")<<"AWS_KEY = \"AKIAIOSFODNN7EXAMPLE\""<<all<<QStringList{"key: AWS_KEY = \"AKIAIOSFODNN7EXAMPLE\""};
  {const auto token=QString("gh")+"p_"+QString("aB3dE5fG7hJ9kL1mN3pQ5rS7tU9vW1xY3z5A");
   QTest::newRow("GitHub token inside a URL")<<"git clone https://"+token+"@github.com/example/acme-build-7.git"<<QStringList{"key"}<<QStringList{"key: https://"+token+"@github.com/example/acme-build-7.git"};}
  QTest::newRow("dashed cards in a table")<<"Visa 4111-1111-1111-1111 | MC 5555-5555-5555-4444"<<QStringList{"card"}<<QStringList{"card: 4111-1111-1111-1111","card: 5555-5555-5555-4444"};
  QTest::newRow("card with en dashes and a minus sign")<<QString("Visa 4111\u20131111\u20131111\u20131111 MC 5555\u22125555\u22125555\u22124444")<<QStringList{"card"}<<QStringList{QString("card: 4111\u20131111\u20131111\u20131111"),QString("card: 5555\u22125555\u22125555\u22124444")};
  QTest::newRow("card inside a CSV row")<<"2,5555-5555-5555-4444,5.00,21/11/2026"<<all<<QStringList{"card: 2,5555-5555-5555-4444,5.00,21/11/2026"};
  QTest::newRow("email in quotes")<<"OWNER = \"ops-alerts@example.com\""<<all<<QStringList{"email: \"ops-alerts@example.com\""};
  {// An editor's soft wrap: equal-width lines, the first after "token:".
   // Built in pieces so no JWT-shaped literal sits in the source (the repository's secret scan).
   const QString jwt=QString("e")+"yJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."+"e"+"yJzdWIiOiIxMjM0NTY3ODkwIiwibmFtZSI6IkV4YW1wbGUifQ."+"SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c";
   const auto a=jwt.left(44),b=jwt.mid(44,44),c=jwt.mid(88);
   QTest::newRow("JWT wrapped over three lines")<<"token: "+a+" | "+b+" | "+c<<QStringList{"key"}<<QStringList{"key: "+a+" "+b+" "+c};}
  // Sol on #1826 (S19): a token ending its line takes no ordinary words from the next line.
  QTest::newRow("a token takes no short next line")<<"SECRET 9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a | id42 ok | INFO 200"<<QStringList{"key"}<<QStringList{"key: SECRET 9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a"};
  // Sol on #1826: a prefix inside a URL path needs a digit in its body.
  QTest::newRow("a URL slug is not a token")<<"see https://docs.example.test/glagent-installation-guide"<<QStringList{"key"}<<QStringList{};
  QTest::newRow("an email after an em dash")<<QString("Write \u2014alice@example.com now")<<all<<QStringList{QString("email: \u2014alice@example.com")};
  QTest::newRow("two phones joined by an em dash")<<QString("Call 202-555-0143\u2014415-555-0100")<<all<<QStringList{QString("phone: 202-555-0143\u2014415-555-0100")};
  QTest::newRow("a dialling prefix is not a card")<<"Call 0044 207946 0958 now"<<all<<QStringList{"phone: 0044 207946 0958"};
  QTest::newRow("hex secret wrapped with a short tail")<<"SECRET = \"9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a | 08edc\""<<QStringList{"key"}<<QStringList{"key: SECRET = \"9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a 08edc\""};
  // #1850, Kate on k72: a 64-character hex secret soft-wrapped so its last three letters and the
  // closing quote start the next line. The quote the first line left open closes there.
  const auto signing=QString("\"6311d6de7be2f57b64c74d411a0b3")+"7935201bc28fc32d62a927d6cfb5d343";
  QTest::newRow("hex secret wrapped with a letter-only tail")<<"SIGNING_HEX = | "+signing+" | edc\" | OWNER = \"ops\""<<QStringList{"key"}<<QStringList{"key: "+signing+" edc\""};
  QTest::newRow("hex secret wrapped with a letter-only tail and a comma")<<"KEYS = [ | "+signing+" | edc\", | ]"<<QStringList{"key"}<<QStringList{"key: "+signing+" edc\","};
  QTest::newRow("hex secret wrapped, its closing quote read curly")<<"SIGNING_HEX = | "+signing+" | edc\u201D | OWNER"<<QStringList{"key"}<<QStringList{"key: "+signing+" edc\u201D"};
  QTest::newRow("a tail closing a call stays with its token")<<"verify( | "+signing+" | edc\"); | done"<<QStringList{"key"}<<QStringList{"key: "+signing+" edc\");"};
  const auto signingSingle=QString("\u2018")+signing.mid(1);
  QTest::newRow("a tail closing a curly single quote")<<"KEY = | "+signingSingle+" | edc\u2019 | OWNER"<<QStringList{"key"}<<QStringList{"key: "+signingSingle+" edc\u2019"};
  QTest::newRow("a tail under a token with no open quote stays out")<<"SECRET "+signing.mid(1)+" | edc\" | later"<<QStringList{"key"}<<QStringList{"key: SECRET "+signing.mid(1)};
  QTest::newRow("a tail under a closed quote stays out")<<"SECRET "+signing+"\" | edc\" | later"<<QStringList{"key"}<<QStringList{"key: SECRET "+signing+"\""};
  QTest::newRow("a tail under a quote closed curly stays out")<<"SECRET "+signing+"\u201D | edc\" | later"<<QStringList{"key"}<<QStringList{"key: SECRET "+signing+"\u201D"};
  {// #1856: a token that ends its line takes no line below it that holds a card,
   // an address or a key of its own; each is its own match. Built in pieces for
   // the repository's secret scan.
   const auto token=QString("gh")+"p_"+"aB3dE5fG7hJ9kL1mN3pQ"+"5rS7tU9vW1xY3zA5";
   const auto hex=QString("\"6311d6de7be2f57b64c74d411a0b3")+"7935201bc28fc32d62a927d6cfb5d343";
   QTest::newRow("a token ending its line takes no card lines")<<"Bearer "+token+" | 4539 1488 0343 6467 | 4111 1111 1111 1111 | SIGNING_HEX = "+hex<<all
    <<QStringList{"key: "+token,"card: 4539 1488 0343 6467","card: 4111 1111 1111 1111","key: SIGNING_HEX = "+hex};
   QTest::newRow("a token ending its line takes no address line")<<"Bearer "+token+" | ops-alerts@example.com"<<all<<QStringList{"key: "+token,"email: ops-alerts@example.com"};
   const auto aws=QString("AKIA")+"IOSFODNN7EXAMPLE";
   QTest::newRow("a token ending its line takes no key line")<<"Bearer "+token+" | "+aws<<QStringList{"key"}<<QStringList{"key: "+token,"key: "+aws};
   const QString jwt=QString("e")+"yJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."+"e"+"yJzdWIiOiIxMjM0NTY3ODkwIiwibmFtZSI6IkV4YW1wbGUifQ."+"SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c";
   const auto a=jwt.left(37),b=jwt.mid(37,47),c=jwt.mid(84);
   // Card lines read as one key still take the next line: its misread card (Luhn fails) stays covered.
   QTest::newRow("card lines read as a key keep a misread card below")<<"Card 4539 1488 0343 6467 Card 5425 2334 3010 9903 | Visa 4111 1171 1171 1171 MC 5555 5555 5555 4444"<<QStringList{"card","key"}
    <<QStringList{"key: Card 4539 1488 0343 6467 Card 5425 2334 3010 9903 Visa 4111 1171 1171 1171 MC 5555 5555 5555 4444"};
   QTest::newRow("JWT wrapped just after a dot")<<"id_token: "+a+" | "+b+" | "+c<<QStringList{"key"}<<QStringList{"key: "+a+" "+b+" "+c};
   // Sol on #1863: the dot lets a JWT's own next part through, not another key...
   QTest::newRow("a JWT part ending in a dot takes no key line")<<"id_token: "+a+" | "+aws<<QStringList{"key"}<<QStringList{"key: "+a,"key: "+aws};
   // ...a card earlier on the line leaves a wrapped JWT whole...
   QTest::newRow("a card before a wrapped JWT")<<"Card 4111 1111 1111 1111 id_token: "+a+" | "+b+" | "+c<<QStringList{"card","key"}<<QStringList{"key: 4111 1111 1111 1111 id_token: "+a+" "+b+" "+c};
   // ...a JWT has no length bound...
   QString payload;for(int i=0;i<12;++i)payload+=QString("e")+"yJzdWIiOiIxMjM0NTY3ODkwIiwibmFtZSI6IkV4YW1wbGUifQ";
   const QString big=jwt.left(37)+payload+"."+jwt.mid(88);
   QTest::newRow("a long JWT wrapped over four lines")<<"id_token: "+big.left(200)+" | "+big.mid(200,205)+" | "+big.mid(405,205)+" | "+big.mid(610)<<QStringList{"key"}<<QStringList{"key: "+big.left(200)+" "+big.mid(200,205)+" "+big.mid(405,205)+" "+big.mid(610)};
   // ...a hex run after a card on its line keeps its wrapped tail...
   QTest::newRow("a hex run after a card keeps its wrapped tail")<<"4111 1111 1111 1111 a3bf4f1b2b0b822cd15d6c15b0f00a9f86d081884c7d659a2feaa0c5 | 5ad015a3"<<QStringList{"card","key"}
    <<QStringList{"key: 4111 1111 1111 1111 a3bf4f1b2b0b822cd15d6c15b0f00a9f86d081884c7d659a2feaa0c5 5ad015a3"};
   QTest::newRow("a short hex run after a card keeps its wrapped tail")<<"4111 1111 1111 1111 a3bf4f1b2b0b822cd15d6c15b0f0 | 0a9f86d08188"<<QStringList{"card","key"}
    <<QStringList{"key: 4111 1111 1111 1111 a3bf4f1b2b0b822cd15d6c15b0f0 0a9f86d08188"};
   QTest::newRow("a quoted misread hex run after a card keeps its wrapped tail")<<QString("4111 1111 1111 1111 \u201Ca3bf4f1b2b0b822cd15d6cI5b0f0 | 0a9f86d08188\u201D")<<QStringList{"card","key"}
    <<QStringList{QString("key: 4111 1111 1111 1111 \u201Ca3bf4f1b2b0b822cd15d6cI5b0f0 0a9f86d08188\u201D")};
   QTest::newRow("a hex run in guillemets after a card keeps its wrapped tail")<<QString("4111 1111 1111 1111 \u00ABa3bf4f1b2b0b822cd15d6c15b0f0 | 0a9f86d08188\u00BB")<<QStringList{"card","key"}
    <<QStringList{QString("key: 4111 1111 1111 1111 \u00ABa3bf4f1b2b0b822cd15d6c15b0f0 0a9f86d08188\u00BB")};
   QTest::newRow("card lines read as a key take no number below them")<<"Card 4539 1488 0343 6467 Card 5425 2334 3010 9903 | 20261122"<<QStringList{"card","key"}
    <<QStringList{"key: Card 4539 1488 0343 6467 Card 5425 2334 3010 9903"};
   // ...and an address whose dot recognition lost is still an address.
   QTest::newRow("a token ending its line takes no address that lost its dot")<<"Bearer "+token+" | ops@EXAMPLE COM"<<all<<QStringList{"key: "+token,"email: ops@EXAMPLE COM"};}
  // m74 acceptance: a card cell right after an IPv4 cell, as recognition read a table row.
  // m74 xed: a string literal read with a straight and a curly quote before the key.
  QTest::newRow("AWS key after two quotes")<<QString("AWS_KEY = \"\u201CAKIAIOSFODNN7EXAMPLE\"")<<QStringList{"key"}<<QStringList{QString("key: AWS_KEY = \"\u201CAKIAIOSFODNN7EXAMPLE\"")};
  // Sol on #1826 (S20-S24).
  QTest::newRow("card after a No. label")<<"Ref No.4111 1111 1111 1111 ok"<<QStringList{"card"}<<QStringList{"card: No.4111 1111 1111 1111"};
  QTest::newRow("order IDs are not a card")<<"Order IDs: 120004 120005 120006 120007"<<QStringList{"card"}<<QStringList{};
  QTest::newRow("a quoted note after a key stays a word")<<QString("key AKIAIOSFODNN7EXAMPLE \u201Cnote\u201D")<<QStringList{"key"}<<QStringList{"key: AKIAIOSFODNN7EXAMPLE"};
  QTest::newRow("a quoted versioned slug is not a token")<<QString("see \u201Cglagent-installation-guide-v2\u201D")<<QStringList{"key"}<<QStringList{};
  {// Built in pieces: a long hex run beside a short label reads as a secret to the repository's scan.
   const QString digest=QString("9f86d081884c7d659a2feaa0c55ad015")+"a3bf4f1b2b0b822cd15d6c15b0f00a";
   QTest::newRow("a short word below a digest stays a word")<<digest+" | "+"I"+"D42"<<QStringList{"key"}<<QStringList{"key: "+digest};}
  QTest::newRow("card after an IPv4 address")<<"Ada 555.010.0123 198.51.100.7 5555-5555-5555-4444 renewal | Ben 203.0.113.9 3782-822463-10005 VIP"<<QStringList{"card"}<<QStringList{"card: 5555-5555-5555-4444","card: 3782-822463-10005"};
  QTest::newRow("IPv6 with a prefix length")<<"addr 2001:db8::8a2e:370:7334/64 up"<<all<<QStringList{"ip: 2001:db8::8a2e:370:7334/64"};
  QTest::newRow("a Luhn-failing card is not a phone")<<"Test 4111 1111 1111 1112 rejected"<<all<<QStringList{};
  QTest::newRow("a Luhn-failing Amex is not a phone")<<"Test 3782 822463 10006 rejected"<<all<<QStringList{};
  QTest::newRow("card with dashes and O confused")<<"Paid 5500-0000-0000-0OO4 ok"<<all<<QStringList{"card: 5500-0000-0000-0OO4"};
  QTest::newRow("Luhn invalid is no card")<<"Ref 4111 1111 1111 1112 x"<<QStringList{"card"}<<QStringList{};
  // Token shapes are put together here, so that no literal in the source looks
  // like a credential to a secret scanner. All of them are made up.
  const auto aws=QString("AK")+"IA"+"IOSFODNN7"+"EXAMPLE";
  const auto github=QString("gh")+"p_"+"aB3dE5fG7hJ9kL1mN3pQ"+"5rS7tU9vW1xY3zA5";
  const auto gitlab=QString("gl")+"pat-"+"xY7zA9bC1d"+"E3fG5hJ7kL";
  const auto slack=QString("xo")+"xb-"+"1234567890-"+"abcdefghij";
  const auto stripe=QString("sk")+"_li"+"ve_"+"4eC39HqLyjWD"+"arjtT1zdp7dc";
  const auto jwt=QString("ey")+"JhbGciOiJIUzI1NiJ9."+"ey"+"JzdWIiOiIxIn0."+"c2lnbmF0dXJlMTIz";
  const auto begin=QString("-----BEGIN ")+"PRIV"+"ATE KEY-----",end=QString("-----END ")+"PRIV"+"ATE KEY-----",body=QString("MIIEvQIBADAN")+"BgkqhkiG9w0BAQEFAASC";
  const auto hexRun=QString("9f86d081884c7d659a2feaa0c55ad015")+"a3bf4f1b2b0b822cd15d6c15b0f00a08";
  const auto base64Run=QString("dGhpc0lzQVNl")+"Y3JldEtleVZhbHVlMTIzNDU2Nzg5";
  QTest::newRow("AWS access key ID")<<"id = "+aws<<all<<QStringList{"key: "+aws};
  QTest::newRow("GitHub token")<<"value "+github<<all<<QStringList{"key: "+github};
  QTest::newRow("GitLab token")<<"use "+gitlab<<all<<QStringList{"key: "+gitlab};
  {const auto split=QString("g")+" Lpat-"+"sFemwPgF-3ZHngBqpURT";
   QTest::newRow("GitLab token read with its l as L, split after g")<<"4 GITLAB_TOKEN="+split<<all<<QStringList{"key: 4 GITLAB_TOKEN="+split};}
  // As recognition really returns them (#1428, the candidate pages measured on
  // fe838a950 and the realistic-token pages): long runs split into pieces with
  // misread characters, a separator lost or doubled, dashes read as other signs.
  const auto jwtPieces=QString("ey) hbGciOiJub25lin0. ")+"ey"+"jzdWliOijzeWS50aGVOaWwMtaW52YWxpZCJ}9. bm9OLWEtc2InbmF OdXj!";
  QTest::newRow("JWT split, its head and a brace misread")<<"Bearer "+jwtPieces<<all<<QStringList{"key: "+jwtPieces};
  const auto hexPieces=QString("024c56c5a7981f8")+" b29f289263d07 104 "+"457e2flccc84t490342a07aa"+" 75204e936";
  QTest::newRow("hex digest split, l and t misread")<<hexPieces<<all<<QStringList{"key: "+hexPieces};
  QTest::newRow("base64 split into five pieces")<<"c3ludGhid GijlG xvbmeg cnVulHNoY XBIIGZpeHR1cmU="<<all<<QStringList{"key: c3ludGhid GijlG xvbmeg cnVulHNoY XBIIGZpeHR1cmU="};
  const auto pemBegin=QString("--~-BEGIN ")+"PRIV"+"ATE KEY----",pemEnd=QString("-—--END ")+"PRIV"+"ATE KEY----";
  QTest::newRow("PEM block, dashes misread, body damaged")<<pemBegin+" | c3ludG hid GijlGludmFsaWwQga2V51G1hd | "+pemEnd<<all<<QStringList{"key: "+pemBegin+" c3ludG hid GijlGludmFsaWwQga2V51G1hd "+pemEnd};
  const auto githubLost=QString("ah")+"p a8Fq2LmZ0pQ7rS1tU3vW5xY7zA9bC1dE";
  QTest::newRow("GitHub token, g read as a and underscore lost")<<githubLost<<all<<QStringList{"key: "+githubLost};
  const auto gitlabSplit=QString("gl")+" pat- XLyKTIkTpZUib]6-4_AQ";
  QTest::newRow("GitLab token split, a bracket misread")<<gitlabSplit<<all<<QStringList{"key: "+gitlabSplit};
  const auto awsMisread=QString("AA")+"IA"+"ZEHSNS2XQPNJJIBK";
  QTest::newRow("AWS key ID, K read as A")<<"id "+awsMisread<<all<<QStringList{"key: "+awsMisread};
  const auto slackDoubled=QString("xO")+"Xxb-24096 7033545-243386374822-yl4Zv4tiJ8kaVNCEEOaAduls";
  QTest::newRow("Slack token, a letter doubled and split")<<slackDoubled<<all<<QStringList{"key: "+slackDoubled};
  QTest::newRow("IPv6 split at the double colon")<<"Peer 2001:db8: :42 up"<<all<<QStringList{"ip: 2001:db8: :42"};
  QTest::newRow("IPv6 with a dash read beside the double colon")<<"Route 2001:db8::-4? up"<<all<<QStringList{"ip: 2001:db8::-4?"};
  QTest::newRow("email with its last dot lost")<<"Write corpus@example invalid"<<all<<QStringList{"email: corpus@example invalid"};
  // On the final candidate (#1462) with the page cut into eight strips, the
  // 36-letter GitHub fixture at 14 px came back with its prefix gone.
  QTest::newRow("token collapsed into repeated letters, prefix lost")<<"oho aeaaaaaaeae aad ad aaa ae aa aaa aa aaaaaaaad"<<all<<QStringList{"key: oho aeaaaaaaeae aad ad aaa ae aa aaa aa aaaaaaaad"};
  QTest::newRow("token collapsed into two letters, prefix misread")<<"APC CCEEEECEEEECEEECEEEECEEECEEEECEEETT"<<all<<QStringList{"key: APC CCEEEECEEEECEEECEEEECEEECEEEECEEETT"};
  QTest::newRow("a UUID split before its last group is no phone")<<"UUID 550e8400-e29b-41d4-a7 16-446655440000 assigned to device VM-07."<<all<<QStringList{};
  // Ordinary lines that resembled tokens or numbers once words were joined.
  QTest::newRow("a version is not a phone")<<"Release 26.1112.4660 for Ubuntu 24.04"<<all<<QStringList{};
  QTest::newRow("capitalized words with numbers are no token")<<"Scores: Team Alpha 3, Team Bravo 2, Team Charlie 0 after 90 minutes"<<all<<QStringList{};
  QTest::newRow("a hardware line is no token")<<"MacBookPro16,1 Intel Core i7-9750H 2.6GHz 6-Core 32GB DDR4 2666MHz"<<all<<QStringList{};
  QTest::newRow("a prefix inside a word is no token")<<"ghosts haunt the lighthouse and they Included 24 photos of 2026 for everyone"<<all<<QStringList{};
  // #1839: a token at its usual length ends there; the ordinary words after
  // it are left out, read whole, split or with a letter doubled, as putgd-ocr
  // read the line in Liberation Serif 14 px, Adwaita Sans 32 px and Liberation
  // Sans 32 px. A token read short keeps a word after it in its usual length.
  QTest::newRow("words after a whole token")<<"Your token "+github+" was leaked today"<<QStringList{"key"}<<QStringList{"key: "+github};
  const auto githubThree=QString("gh")+"p_aB3dESfG7hJ9kL"+" 1mN3pQ5rS7tU9vW"+" 1xY3zA5";
  QTest::newRow("words after a token read in three pieces")<<"Your token "+githubThree+" leaked today"<<QStringList{"key"}<<QStringList{"key: "+githubThree};
  const auto githubTail=QString("gh")+"p_aB3dE5fG7hJ9kLIMN3pQ5rS7tU9VW1xY"+" 3ZA5";
  QTest::newRow("words after a token whose tail is a word")<<"Your token "+githubTail+" leaked today"<<QStringList{"key"}<<QStringList{"key: "+githubTail};
  const auto githubLong=QString("gh")+"p_aB3dE5fG"+" 7hJ9kLImMN3pQ5rS7tU9VW1xY3ZA5";
  QTest::newRow("words after a token read a letter long")<<"Your token "+githubLong+" leaked today"<<QStringList{"key"}<<QStringList{"key: "+githubLong};
  const auto githubShort=QString("gh")+"p_aB3dESfG7h9kLImN3pQ5rS7tUSwW"+" 1xY3zA5";
  const auto githubLabel=QString("GITHUB_TOKEN=gh")+"p_aB3dE5fG7hJ9kL1mN3pQ5rS7tU9vW";
  QTest::newRow("a token after a label in its word, its tail split off")<<githubLabel+" xyzabcd was leaked today"<<QStringList{"key"}<<QStringList{"key: "+githubLabel+" xyzabcd"};
  // Sol on #1839: a doubled character leaves the token's last letter or two as a piece of its own
  // just past its length; a JWT has no one length; a rejected "gho" in a URL hides no real prefix.
  const auto githubDoubled=QString("gh")+"p_aB3dE5fG7hJ9kL1mN3pQ5rS7tU9vW1xY3zAA";
  QTest::newRow("a doubled character leaves a last letter past the length")<<"Your token "+githubDoubled+" q was leaked today"<<QStringList{"key"}<<QStringList{"key: "+githubDoubled+" q"};
  QTest::newRow("a doubled character leaves two last letters past the length")<<"Your token "+githubDoubled+" qz was leaked today"<<QStringList{"key"}<<QStringList{"key: "+githubDoubled+" qz"};
  QString jwtLong=QString("ey")+"JhbGciOiJIUzI1NiJ9.";for(int i=0;i<18;++i)jwtLong+=QString("ey")+"JzdWIiOiIxMjM0NTY3ODkwIn0";jwtLong+=QString(".c2lnbmF0dXJl")+"MTIzNDU2Nzg5MA";
  QTest::newRow("a long JWT keeps its last lowercase piece")<<"Bearer "+jwtLong+" abc"<<QStringList{"key"}<<QStringList{"key: "+jwtLong+" abc"};
  QTest::newRow("a token after a URL path with a gho word")<<"see https://example.test/ghost/settingsconfiguration/"+github<<QStringList{"key"}<<QStringList{"key: https://example.test/ghost/settingsconfiguration/"+github};
  QTest::newRow("a token read short keeps the next word")<<"Your token "+githubShort+" was leaked today"<<QStringList{"key"}<<QStringList{"key: "+githubShort+" was"};
  const auto githubSlash=QString("gh")+"p_aB3dESfG7h/9kLImN3pQ5rS7tU9vW1x"+"\u00a53zA5";
  QTest::newRow("words after a token with J read as a slash")<<"Your token "+githubSlash+" was leaked today"<<QStringList{"key"}<<QStringList{"key: "+githubSlash};
  QTest::newRow("Slack token")<<"chat "+slack+" here"<<all<<QStringList{"key: "+slack+" here"};
  QTest::newRow("Stripe key")<<stripe+" set"<<all<<QStringList{"key: "+stripe+" set"};
  QTest::newRow("JWT")<<"Bearer "+jwt<<all<<QStringList{"key: "+jwt};
  // #1875: a stray character read joined before an unlabelled token's eyJ is covered with it.
  for(const auto stray:{QString("l"),QString("I"),QString("|"),QString("1"),QString("J"),QString("j"),QString("["),QString("{")})
   QTest::newRow(qPrintable("unlabelled JWT read with a stray "+stray))<<"$ echo "+stray+jwt<<all<<QStringList{"key: "+stray+jwt};
  // As putgd-ocr read xed's caret and an unlabelled JWT over three rows on m74 (PR #1863's acceptance).
  const auto caretRead=QString("l")+"eyJhbGci0iJIUZIINiIsInR5cCI6IkpXVCI9. "+"ey"+"JzdWIi0iIxXMjMONTY30Dkwliw- | ibmFtZSI6IkpvaG4gRG9LIiwiaWFOI "
   +"joxNTE2MjM5MDIyfQ.SfLKxwRISMeKKF2Q- | T4fwpMeJ f36POk6yJV_adQssw5c";
  QTest::newRow("unlabelled JWT behind an editor's caret, over three rows")<<caretRead<<all<<QStringList{"key: "+QString(caretRead).remove("| ")};
  // The caret and the header's J both misread: the payload's eyJ after the dot finds the token.
  const auto headerMisread=QString("|eyf")+"hbGciOiJIUzI1NiJ9."+"ey"+"JzdWIiOiIxIn0."+"c2lnbmF0dXJlMTIz";
  QTest::newRow("unlabelled JWT whose header is misread")<<"Seen "+headerMisread<<all<<QStringList{"key: "+headerMisread};
  for(const auto head:{QString("ey"),QString("ley"),QString("Jey")}){
   const auto split=head+" JhbGciOiJIUzI1NiJ9."+"ey"+"JzdWIiOiIxIn0."+"c2lnbmF0dXJlMTIz";
   QTest::newRow(qPrintable("a JWT head read as a word of its own: "+head))<<"Seen "+split<<all<<QStringList{"key: "+split};
  }
  const auto jewSplit=QString("Jew hbGciOiJIUzI1NiJ9.")+"ey"+"JzdWIiOiIxIn0."+"c2lnbmF0dXJlMTIz";
  const auto headTwoWords=QString("ley JhbGci0i JSUZI1NiIsInR5cCI6IkpXVCJ9.")+"ey"+"IzdWIiOiIxIn0."+"c2lnbmF0dXJlMTIz";
  QTest::newRow("a JWT head split over two words, the payload's J misread")<<"Seen "+headTwoWords<<all<<QStringList{"key: "+headTwoWords};
  QTest::newRow("a JWT head read with its y and J misread")<<"Seen "+jewSplit<<all<<QStringList{"key: "+jewSplit};
  // As putgd-ocr read the caret merged with the e: in a Firefox text field on m74 and k72, and in Kate on k72.
  const auto browserField=QString("k")+"yJhbGci0iJSUZI1INiIsInR5cCI6IkpXVCIsImtpZC1I6ImxhYiO3In0."+"ey | "
   +"JzdW1i0iJzdmMtcmVwb3JOcyIsImF1ZC16ImFwaSIsImV4cCI6MTcSNTAW | MDAWMHO . s8wEdbt4pQJ gmIWOmIms9mbTEGLVEOMDMU7KMdNucvI";
  QTest::newRow("a JWT behind a text field's caret read as kyJ")<<browserField<<all<<QStringList{"key: "+QString(browserField).remove("| ")};
  const auto kateRead=QString("b")+"yJhbGc iOiJIUZIANiIsInR5cCI6IkpXVCI9. "+"ey"+"JzdWIiOiIxMjMONTY30Dkw | TiwibmFtZSI6IkpvaG4gRG9LIiwiawFOI j oxNTE2MjJM5MDIyfQ.Sf LKxwRJISM";
  QTest::newRow("a JWT behind an editor's caret read as byJ")<<kateRead<<all<<QStringList{"key: "+QString(kateRead).remove("| ")};
  // Sol on #1883: a JWT wrapped inside its header keeps its continuation, the first fragment
  // there included; a head split over short words or merged into "ky" is covered; a dotted
  // name or prose with a date is no JWT; a whole JWT takes no short word before it.
  const auto wrapHead=QString("ey")+"JhbGciOiJSUzI1NiIsInR5cCI6IkpXVCIsImtpZCI6Imxh | Nkl YWItMDci9."+"ey"+"JzdWIiOiIxIn0.c2lnbmF0";
  QTest::newRow("a JWT wrapped inside its header keeps its next line")<<"token: "+wrapHead<<all<<QStringList{"key: "+QString(wrapHead).remove("| ")};
  const auto headThree=QString("ley Jhb GciOiJIUzI1NiJ9.")+"ey"+"JzdWIiOiIxIn0."+"c2lnbmF0dXJlMTIz";
  QTest::newRow("a JWT head split over three words")<<"Seen "+headThree<<all<<QStringList{"key: "+headThree};
  const auto kySplit=QString("ky JhbGciOiJIUzI1NiJ9.")+"ey"+"JzdWIiOiIxIn0."+"c2lnbmF0dXJlMTIz";
  QTest::newRow("a caret merged into ky and split off")<<"Seen "+kySplit<<all<<QStringList{"key: "+kySplit};
  QTest::newRow("a dotted name is no JWT payload")<<"see archive.eyJanuary2026.ReleaseNotes.pdf today"<<all<<QStringList{};
  QTest::newRow("prose with a date after a stray-read prefix is no JWT")<<"leyJanuary 2026. Release notes for the new version"<<all<<QStringList{};
  QTest::newRow("a whole JWT takes no short word before it")<<"Seen ley "+jwt<<all<<QStringList{"key: "+jwt};
  const auto parenHead=QString("ley) hbGciOiJIUzI1NiJ9.")+"ey"+"JzdWIiOiIxIn0."+"c2lnbmF0dXJlMTIz";
  QTest::newRow("a JWT head split off with its J read as a parenthesis")<<"Seen "+parenHead<<all<<QStringList{"key: "+parenHead};
  QTest::newRow("a word before a JWT whose head is in its own word stays out")<<"See eyewear "+headerMisread<<all<<QStringList{"key: "+headerMisread};
  QTest::newRow("a whole JWT does not take a second one on the next line")<<"Seen "+jwt+" | Seen "+headerMisread<<all<<QStringList{"key: "+jwt,"key: "+headerMisread};
  QTest::newRow("a whole JWT ending in a comma does not take one on the next line")<<"Seen "+jwt+", | Seen "+headerMisread<<all<<QStringList{"key: "+jwt+",","key: "+headerMisread};
  {const auto header=QString("ey")+"JhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.",rest=QString("ey")+"JzdWIiOiIxIn0.c2lnbmF0dXJlMTIz";
   QTest::newRow("a JWT after a dotted label wrapped after its first dot")<<"auth.session.jwt="+header+" | "+rest<<all<<QStringList{"key: auth.session.jwt="+header+" "+rest};}
  QTest::newRow("a stray character before eyJ in plain words stays out")<<"ley January, Jey June, |eyJuly and 1eyJacket sizes"<<all<<QStringList{};
  QTest::newRow("words around a stray-read JWT prefix stay out")<<"the ley lines, they said, run under leyden jars and eyJanuary"<<all<<QStringList{};
  QTest::newRow("PEM private key block")<<begin+" | "+body+" | Zm9vYmFy "+end+" done"<<all<<QStringList{"key: "+begin+" "+body+" Zm9vYmFy "+end+" done"};
  // #1894: developer secrets with no prefix of their own, and more providers' prefixes. Every value is
  // made up and put together from parts.
  {const auto pg=QString("postgres://app_user:")+"o6!y*G3Sgq"+"jRr8#AzU@10.32.190.22:5432/prod";
   QTest::newRow("a connection string with a password")<<"url "+pg<<all<<QStringList{"key: "+pg};
   QTest::newRow("a connection string split by recognition")<<"postgres://app_user :06! y*G3Sgq"+QString("jRr8#AzU@10.32.190.22:5432/prod")<<all<<QStringList{"key: postgres://app_user :06! y*G3Sgq"+QString("jRr8#AzU@10.32.190.22:5432/prod")};
   const auto mongo=QString("mongodb+srv: //admin: bgE40V20UD6 LDHj 2c Luster®.z6ohj .mongodb.net/?retrywrites=true");
   QTest::newRow("a connection string whose @ was lost")<<mongo<<all<<QStringList{"key: "+mongo};
   const auto redis=QString("redis://:Kx82mPq0")+"LzR7@cache.internal:6379/0";
   QTest::newRow("a connection string with a password and no user")<<redis<<all<<QStringList{"key: "+redis};
   QTest::newRow("a URL after a variable's name")<<"DATABASE_URL="+pg<<all<<QStringList{"key: DATABASE_URL="+pg};}
  QTest::newRow("URLs with a port and no password stay visible")<<"http://localhost:3000/api ssh://git@github.com:22/x postgres://localhost/db https://example.com:8443"<<QStringList{"key"}<<QStringList{};
  QTest::newRow("a connection string with a reference stays visible")<<"postgres://app:${DB_PASSWORD}@db/prod"<<all<<QStringList{};
  QTest::newRow("a secret-named value in a word")<<"DB_PASSWORD="+QString("Tr0ub4dor")+"&3xyz!Q"<<all<<QStringList{"key: DB_PASSWORD="+QString("Tr0ub4dor")+"&3xyz!Q"};
  QTest::newRow("a secret-named value after its name")<<"password: "+QString("Hn4!vQ8z")+"R2#wLp"<<all<<QStringList{"key: "+QString("Hn4!vQ8z")+"R2#wLp"};
  QTest::newRow("a quoted value in JSON")<<"\"client_secret\": \""+QString("q9Lm2Vx7")+"Tz4Rb8Nc\","<<all<<QStringList{"key: \""+QString("q9Lm2Vx7")+"Tz4Rb8Nc\","};
  QTest::newRow("a name and value around a lone equals sign")<<"API_KEY = "+QString("q9Lm2Vx7")+"Tz4Rb8Nc"<<all<<QStringList{"key: "+QString("q9Lm2Vx7")+"Tz4Rb8Nc"};
  QTest::newRow("basic credentials after Authorization")<<"Authorization: Basic "+QString("dXNlcjpw")+"YXNz"<<all<<QStringList{"key: "+QString("dXNlcjpw")+"YXNz"};
  {const auto hf=QString("h")+"f_"+"g5EeE5B7YX3uvHNiAwxFEk"+"oni8gX0280Yi";
   const auto line=QString("{\"ts\":\"2026-03-03T14:31:31.651Z\",\"level\":\"info\",\"HUB_TOKEN\":\"")+"q9Lm2Vx7Tz"+"4Rb8Nc1Kp6\"}";
   QTest::newRow("a secret-named value inside a JSON log line")<<line<<all<<QStringList{"key: "+line};
   QTest::newRow("a Hugging Face token with digits")<<"token "+hf<<all<<QStringList{"key: "+hf};
   QTest::newRow("a CSV row of name and value")<<"SESSION_SECRET,"+QString("q9Lm2Vx7Tz")+"4Rb8Nc1Kp6,diego"<<all<<QStringList{"key: SESSION_SECRET,"+QString("q9Lm2Vx7Tz")+"4Rb8Nc1Kp6,diego"};
   QTest::newRow("a Laravel app key")<<"APP_KEY=base64:"+QString("eIwf0hlV/SnhB94o69WQ")+"EVPBCenpTKzLUJHUNmtrnxA="<<all<<QStringList{"key: APP_KEY=base64:"+QString("eIwf0hlV/SnhB94o69WQ")+"EVPBCenpTKzLUJHUNmtrnxA="};
   QTest::newRow("a public value under a token's name stays visible")<<"NEXT_PUBLIC_MAPBOX_TOKEN="+QString("pk")+".eyJ1IjoiLuHcBDPMWxGwuC521h"<<QStringList{"key"}<<QStringList{};}
  QTest::newRow("a sentence after a secret's name stays visible")<<"Password: must be at least 8 characters"<<all<<QStringList{};
  QTest::newRow("settings about a secret stay visible")<<"LOG_LEVEL=debug PORT=8080 TOKEN_TTL=3600 PASSWORD_MIN_LENGTH=12 AUTH_ENABLED=true PWD=/home/lab"<<all<<QStringList{};
  QTest::newRow("a secret's reference or placeholder stays visible")<<"DB_PASSWORD=${DB_PASSWORD} | API_KEY=<your-key> | SECRET_KEY=changeme | token: "+QString(12,'*')<<all<<QStringList{};
  QTest::newRow("references, placeholders and addresses under a secret's name stay visible")<<"password = process.env.STRIPE_SECRET_KEY | config.token = 'os.environ[\"API_KEY\"]' | token = \"YOUR_TOKEN_HERE\" | export OPENAI_API_KEY=your-api-key-here | \"password\": \"op://Prod/Stripe/credential\" | export token=redis://localhost:6379/0 | token='gh"+QString("p_t37p9f'")<<all<<QStringList{};
  {const auto publishable=QString("pk")+"_live_"+"51VEWWvHWLlOupEdQRNvTZ07KS22Md1OXbXWv5bVFQII5SBGHsW3TbeJfMB5bHr2b";
   const auto ssh=QString("ssh-ed25519 ")+"AAAAC3NzaC1lZDI1NTE5AAAA"+"IMFh+rSm1njPFDmeeN0SmccpgHSVGnjCMHgYrnNgpDrh mei@laptop";
   const auto cert=QString("-----BEGIN CERTIFICATE----- | ")+"MIIDdzCCAl+gAwIBAgIEAgAAuTANBgkqhkiG9w0BAQUFADBaMQswCQYDVQQGEwJJ | -----END CERTIFICATE-----";
   QTest::newRow("a publishable key stays visible")<<"STRIPE_PUBLISHABLE_KEY="+publishable<<QStringList{"key"}<<QStringList{};
   QTest::newRow("an SSH public key stays visible")<<ssh<<QStringList{"key"}<<QStringList{};
   QTest::newRow("a certificate stays visible")<<cert<<QStringList{"key"}<<QStringList{};
   QTest::newRow("a data URI stays visible")<<"logo data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAADhewiA+uumqndKMBzpqOlp0iuvjFtxLSzdO3JL"<<QStringList{"key"}<<QStringList{};}
  QTest::newRow("ordinary config stays visible")<<"host: localhost port: 5432 user: app_user"<<all<<QStringList{};
  {const auto slack=QString("https://hooks.slack.com/services/")+"TZSAMVP0C/BG3X3JY75/"+"B5bIUbqVUVxFD1Os2MNmZXOA";
   const auto misread=QString("https://hooks.slLack.com/services/")+"TZSAMVPOC/BG3X3JY75/"+"B5bIUDQVUVXFD10s2MNMZXOA";
   const auto discord=QString("https://discord.com/api/webhooks/")+"1187362049118123456/"+"Zq8Lm2Vx7Tz4Rb8Nc1Kp6Wd3Hs5Jg0Fy";
   QTest::newRow("a Slack webhook")<<"hook "+slack<<all<<QStringList{"key: "+slack};
   QTest::newRow("a Slack webhook as recognition misreads it")<<misread<<all<<QStringList{"key: "+misread};
   QTest::newRow("a Discord webhook")<<"# Discord: "+discord<<all<<QStringList{"key: "+discord};
   QTest::newRow("a Slack page that is no webhook stays visible")<<"https://api.slack.com/apps https://hooks.slack.com/"<<all<<QStringList{};}
  {const auto openai=QString("sk-")+"proj-"+"Vq7Zr2Lp9XkT4mWbT3Bl"+"bkFJn8Yc5Rd1Hs6Qe3Ug";
   const auto anthropic=QString("sk-")+"ant-api03-"+"cw6rFL9ges3oH4nsYPqw"+"uf75KZuFk6ZvBsNH-ggw";
   const auto google=QString("AI")+"za"+"2XIThrWWmz_SKMCC_ZsP"+"r3kk-HGDl9_ykQY";
   const auto npm=QString("np")+"m_"+"GbdijE3mhEah5i8WSEqty"+"Q7boviOYApBTro7";
   const auto hf=QString("h")+"f_"+"FBWMJXMsTboQsSZOBrA"+"xzNjqLmWpKrTs";
   const auto sendgrid=QString("S")+"G."+"vtP4VGTvg-q_NZDzD8ANfn."+"CJiRpIZuptzIZxIryKyh9ZOkDtq57uMVk4Jp8d3HZGn";
   const auto ocean=QString("do")+"p_v1_"+"d88268ac6371c4bc2e05f571e4c11a98"+"c13bea97aee322da81676ac7047c5f0d";
   const auto shopify=QString("sh")+"pat_"+"e33b58c5264392f6"+"d0492555b8ad9c1e";
   const auto twilio=QString("S")+"K"+"b692cc1a230bd2f8"+"57b7d5b2d55685a1";
   const auto mailgun=QString("ke")+"y-"+"7896b99d719f5a08"+"c8b0d388fced1bb0";
   QTest::newRow("an OpenAI key")<<"OPENAI_API_KEY="+openai<<all<<QStringList{"key: OPENAI_API_KEY="+openai};
   QTest::newRow("an Anthropic key")<<"key "+anthropic<<all<<QStringList{"key: "+anthropic};
   QTest::newRow("a Google API key")<<"key "+google<<all<<QStringList{"key: "+google};
   QTest::newRow("an npm token")<<"token "+npm<<all<<QStringList{"key: "+npm};
   QTest::newRow("a Hugging Face token")<<"token "+hf<<all<<QStringList{"key: "+hf};
   QTest::newRow("a SendGrid key split after SG.")<<"SENDGRID_KEY=S"+QString("G. ")+sendgrid.mid(3)<<all<<QStringList{"key: SENDGRID_KEY=S"+QString("G. ")+sendgrid.mid(3)};
   QTest::newRow("a DigitalOcean token after its name")<<"DO_TOKEN="+ocean<<all<<QStringList{"key: DO_TOKEN="+ocean};
   QTest::newRow("a Shopify token")<<"token "+shopify<<all<<QStringList{"key: "+shopify};
   QTest::newRow("a Twilio key")<<"sid "+twilio<<all<<QStringList{"key: "+twilio};
   QTest::newRow("a Mailgun key")<<"mail "+mailgun<<all<<QStringList{"key: "+mailgun};
   QTest::newRow("words that start like a prefix stay visible")<<"TASK SKU-1234 key-value hfold npmrc AIzone SG.config"<<all<<QStringList{};}
  {const auto begin=QString("-----BEGIN RSA ")+"PRIV"+"ATE KEY-----",end=QString("-----END RSA ")+"PRIV"+"ATE KEY-----";
   const auto damaged=QString("MITEOWIBAAKCAQEAHr€Lx")+"O6iBCd5V$NA LeO1 Lp§Mw";
   QTest::newRow("a PEM block takes a damaged first body line")<<begin+" | "+damaged+" | q7XbSLr2Vt4kKps8NmiZc6wd3Hs5JgoFy | "+end<<all<<QStringList{"key: "+begin+" "+damaged+" q7XbSLr2Vt4kKps8NmiZc6wd3Hs5JgoFy "+end};}
  // Sol on #1903.
  {const QStringList urls{QString("postgres://app:%40Ri")+"dge7@db/prod",QString("redis://:82")+"9413@db/0",QString("postgres://alice:Hi")+"ll:8x@db/prod"};
   for(const auto& url:urls)QTest::newRow(qPrintable("a connection string's password: "+url.left(10)))<<"url "+url<<QStringList{"key"}<<QStringList{"key: "+url};}
  QTest::newRow("a port at the end of a word is no password")<<"http://localhost:3000 ready"<<QStringList{"key"}<<QStringList{};
  QTest::newRow("a numeric password")<<"PASSWORD="+QString("82")+"9413"<<QStringList{"key"}<<QStringList{"key: PASSWORD="+QString("82")+"9413"};
  QTest::newRow("a value with a filler word inside")<<"SESSION_SECRET=H7-ex"+QString("ample-r9F4")<<QStringList{"key"}<<QStringList{"key: SESSION_SECRET=H7-ex"+QString("ample-r9F4")};
  QTest::newRow("a password of symbols in one word")<<"PASSWORD='R!$^"+QString("&*()#'")<<QStringList{"key"}<<QStringList{"key: PASSWORD='R!$^"+QString("&*()#'")};
  {const auto aws=QString("AK")+"IA"+"QXXXXZ7TBNV2XK4L";QTest::newRow("an AWS key with four X in it")<<"id "+aws<<QStringList{"key"}<<QStringList{"key: "+aws};}
  QTest::newRow("acronym names")<<"APIKey="+QString("q9Lm2Vx7")+"Tz4Rb8Nc | DBPassword: Hn4!"+QString("vQ8zR2#wLp")<<QStringList{"key"}<<QStringList{"key: APIKey="+QString("q9Lm2Vx7")+"Tz4Rb8Nc","key: Hn4!"+QString("vQ8zR2#wLp")};
  QTest::newRow("a public-named password")<<"PUBLIC_DB_PASSWORD="+QString("Tr0ub4dor")+"&3xyz!Q"<<QStringList{"key"}<<QStringList{"key: PUBLIC_DB_PASSWORD="+QString("Tr0ub4dor")+"&3xyz!Q"};
  {const auto cert=QString("-----BEGIN CERTIFICATE-----"),priv=QString("-----BEGIN RSA PRIV")+"ATE KEY-----",privEnd=QString("-----END RSA PRIV")+"ATE KEY-----";
   const auto body=QString("MIIEowIBAAKCAQEAHrLx06iBCd5V")+"NAle01lpMwSBDLe";
   QTest::newRow("a private key after a certificate missing its END")<<cert+" | MIIDdzCCAl+gAwIBAgIEAgAAuTAN | "+priv+" | "+body+" | "+privEnd+" | -----END PUBLIC KEY-----"<<QStringList{"key"}<<QStringList{"key: "+priv+" "+body+" "+privEnd};}
  {const auto grafana=QString("bt")+"Zq8Lm2Vx7Tz4Rb8Nc1Kp6Wd3Hs5Jg0Fy";
   QTest::newRow("a Grafana token split after glc")<<"token gl"+QString("c ")+grafana<<QStringList{"key"}<<QStringList{"key: gl"+QString("c ")+grafana};}
  {const auto bot=QString("123456:")+"ABC-DEF1234ghIkl-zyx57W2v1u123ew11";
   QTest::newRow("a Telegram bot token")<<"bot "+bot<<QStringList{"key"}<<QStringList{"key: "+bot};
   QTest::newRow("a Telegram bot API URL")<<"https://api.telegram.org/bot"+bot+"/getMe"<<QStringList{"key"}<<QStringList{"key: https://api.telegram.org/bot"+bot+"/getMe"};}
  QTest::newRow("a Slack webhook cut short or masked stays visible")<<"https://hooks.slack.com/services/T00000000 https://hooks.slack.com/services/T00000000/B00000000/"+QString(24,'X')<<QStringList{"key"}<<QStringList{};
  {const auto none=QString("eyJ0eXAiOiJKV1QiLCJhbGciOiJub25lIn0.")+"eyJzdWIiOiIxIn0.";
   QTest::newRow("an unsigned JWT with typ first stays visible")<<"TOKEN="+none<<QStringList{"key"}<<QStringList{};}
  // Sol on #1903, round 2.
  QTest::newRow("a numeric password split before its @")<<"postgres://alice:12"+QString("34 @db/prod")<<QStringList{"key"}<<QStringList{"key: postgres://alice:12"+QString("34 @db/prod")};
  QTest::newRow("a password before a bracket and its @")<<"url postgres://alice:12"+QString("34)@db/prod")<<QStringList{"key"}<<QStringList{"key: postgres://alice:12"+QString("34)@db/prod")};
  QTest::newRow("a symbol password in nested JSON")<<"[{\"password\":\"R!$^"+QString("&*()#\"}]")<<QStringList{"key"}<<QStringList{"key: [{\"password\":\"R!$^"+QString("&*()#\"}]")};
  QTest::newRow("a JSON password whose closing quote is its own word")<<"{\"password\":\"Hn4!vQ8"+QString("zR2#wLp \"}")<<QStringList{"key"}<<QStringList{"key: {\"password\":\"Hn4!vQ8"+QString("zR2#wLp")};
  QTest::newRow("a password in a query inside a quoted URL")<<"{\"url\":\"https://db.example.test/x?user=a&password="+QString("Hn4vQ8zR2wLp\"}")<<QStringList{"key"}<<QStringList{"key: {\"url\":\"https://db.example.test/x?user=a&password="+QString("Hn4vQ8zR2wLp\"}")};
  QTest::newRow("an @ in a URL's query is no password")<<"http://localhost:3000?owner=ops@example.test"<<QStringList{"key"}<<QStringList{};
  {const auto dsn=QString("https://8f2c41d0")+"7b9e4a63@o41872.ingest.sentry.io/552";
   QTest::newRow("a Sentry DSN")<<"dsn "+dsn<<QStringList{"key"}<<QStringList{"key: "+dsn};
   QTest::newRow("a Sentry DSN split after its scheme by recognition")<<"SENTRY_DSN=https: "+dsn.mid(6)<<QStringList{"key"}<<QStringList{"key: SENTRY_DSN=https: "+dsn.mid(6)};
   QTest::newRow("a secret-named URL with a key in its userinfo")<<"ANALYTICS_TOKEN=https://"+QString("q9Lm2Vx7Tz4R")+"@collect.example.test/1"<<QStringList{"key"}<<QStringList{"key: ANALYTICS_TOKEN=https://"+QString("q9Lm2Vx7Tz4R")+"@collect.example.test/1"};}
  QTest::newRow("a database URL with a username alone stays visible")<<"DATABASE_DSN=postgresql://reporting@localhost/analytics"<<QStringList{"key"}<<QStringList{};
  QTest::newRow("a Sentry-like host or path that is no DSN stays visible")<<"https://8f2c41d07b9e4a63@notsentry.io/552 https://example.test/docs/https://8f2c41d07b9e4a63@o1.ingest.sentry.io/1"<<QStringList{"key"}<<QStringList{};
  {const auto dsn=QString("https://8f2c41d0")+"7b9e4a63@o41872.ingest.sentry.io/552";
   QTest::newRow("a Sentry DSN in a CSV row read as one word")<<"production,"+dsn<<QStringList{"key"}<<QStringList{"key: production,"+dsn};}
  // #1913, #1914, #1915: follow-ups to #1894.
  QTest::newRow("a bracketed IPv6 address with a port and prefix")<<"listen [2001:db8::84]:443/64 now"<<QStringList{"ip"}<<QStringList{"ip: [2001:db8::84]:443/64"};
  QTest::newRow("a bracketed IPv6 address with a port")<<"peer [2001:db8::c4]:443"<<QStringList{"ip"}<<QStringList{"ip: [2001:db8::c4]:443"};
  QTest::newRow("an IPv6 address with 0 read as @")<<"route fe8@::5921:35ff:fe2a:ffb5/64"<<QStringList{"ip"}<<QStringList{"ip: fe8@::5921:35ff:fe2a:ffb5/64"};
  QTest::newRow("an IPv6 address with 1 read as i")<<"link fe80::4fic:afff:fe60:3513"<<QStringList{"ip"}<<QStringList{"ip: fe80::4fic:afff:fe60:3513"};
  QTest::newRow("an IPv4 CIDR with 0 read as @")<<"net 198.51.10@.223/24"<<QStringList{"ip"}<<QStringList{"ip: 198.51.10@.223/24"};
  QTest::newRow("an address is still an address")<<"mail ops@10.0.0.2 now"<<QStringList{"ip","email"}<<QStringList{"ip: ops@10.0.0.2"};
  {const auto first=QString("{\"url\":\"https://db.example.test/x?password=")+"Hn4vQ8zR2wLp\"}";
   QTest::newRow("a password as the first query parameter in JSON")<<first<<QStringList{"key"}<<QStringList{"key: "+first};
   const auto split=QString("{\"password\":\"v!&R8z")+"2wLp";
   QTest::newRow("a quoted password whose closing quote is split off")<<split+" \"}"<<QStringList{"key"}<<QStringList{"key: "+split};}
  QTest::newRow("a quoted URL with a port stays visible")<<"\"http://localhost:3000\" (http://localhost:8080)"<<QStringList{"key"}<<QStringList{};
  QTest::newRow("a numeric PIN")<<"DB_PIN="+QString("82")+"9413"<<QStringList{"key"}<<QStringList{"key: DB_PIN="+QString("82")+"9413"};
  for(const auto& form:{QString("“PASSWORD”=R!$^&*()#"),QString("12PASSWORD=R!$^&*()#"),QString("{\"password\":\"R!$^&*()#\"}")})
   QTest::newRow(qPrintable("a symbol password in "+form.left(4)))<<form<<QStringList{"key"}<<QStringList{"key: "+form};
  {const auto gitlab=QString("gl")+"pat-"+"xY7zA9bC1dE3fG5hJ7kL";
   QTest::newRow("a GitLab token before a masked word")<<"token "+gitlab+" XXXXXX"<<QStringList{"key"}<<QStringList{"key: "+gitlab+" XXXXXX"};}
  {const auto trusted=QString("-----BEGIN TRUSTED CERTIFICATE-----"),priv=QString("-----BEGIN RSA PRIV")+"ATE KEY-----",privEnd=QString("-----END RSA PRIV")+"ATE KEY-----";
   QTest::newRow("a trusted certificate does not close on another label")<<trusted+" | MIIDdzCCAl+gAwIBAgIEAgAAuTAN | -----END CERTIFICATE----- | "+priv+" | MIIEowIBAAKCAQEAHrLx06iBCd5VNAle01lpMw | "+privEnd<<QStringList{"key"}<<QStringList{"key: "+priv+" MIIEowIBAAKCAQEAHrLx06iBCd5VNAle01lpMw "+privEnd};}
  {const auto none=QString("eyJ0eXAiOiJKV1QiLCJhbGciOiJub25lIn0.")+"eyJzdWIiOiIxIn0.";
   QTest::newRow("an unsigned JWT in quotes before words stays visible")<<"TOKEN=\""+none+"\", see docs"<<QStringList{"key"}<<QStringList{};}
  {const auto azure=QString("https://prod-12.westus.logic.azure.com/workflows/abc/triggers/manual/paths/invoke?api-version=2016-06-01&a=1&b=2&c=3&sp=%2Ftriggers&sv=1.0&sig=")+"q9Lm2Vx7Tz4Rb8Nc1Kp6Wd3Hs5";
   QTest::newRow("an Azure webhook with many parameters")<<azure<<QStringList{"key"}<<QStringList{"key: "+azure};}
  {const auto chat=QString("https://chat.googleapis.com/v1/spaces/AAA/messages?key=")+"AIzaSyDdI0hCZtE6vySjMm"+"&token="+QString(16,'X');
   QTest::newRow("a Google Chat webhook with one real credential")<<chat<<QStringList{"key"}<<QStringList{"key: "+chat};}
  QTest::newRow("hex run with no word about it")<<"value "+hexRun<<all<<QStringList{"key: value "+hexRun};
  // #1895: Patrick's decoy page. A hex run beside a hash word is a digest; a secret word beside it wins.
  {const auto sha1=QString("efd984b6b1813c2c344524c7")+"8d8f21a76afb9fab",sha256=QString("47a74f445ca32dc645e3be45b37a6937")+"d105dae65638df71a251c0d0608b117d";
   QTest::newRow("a hex digest after its hash word stays visible")<<"sha256 "+hexRun<<all<<QStringList{};
   QTest::newRow("a Git SHA cell stays visible")<<"Git SHA "+sha1<<all<<QStringList{};
   QTest::newRow("a SHA-256 cell stays visible")<<"SHA-256 "+sha256<<all<<QStringList{};
   QTest::newRow("a commit line stays visible")<<"commit "+sha1+" (HEAD -> nightly)"<<all<<QStringList{};
   QTest::newRow("a digest with its algorithm in the word stays visible")<<"image@sha256:"+sha256<<all<<QStringList{};
   QTest::newRow("a secret word beats a hash word")<<"SECRET_SHA256="+sha256<<all<<QStringList{"key: SECRET_SHA256="+sha256};
   QTest::newRow("a signing key beside a digest stays covered")<<"signing key "+sha256+" sha256"<<all<<QStringList{"key: key "+sha256+" sha256"};}
  QTest::newRow("a version is not an IP address")<<"Version string v5.8.4.103"<<all<<QStringList{};
  QTest::newRow("a date and time is not a phone")<<"Date/time 2026-11-22 14:03:59.123"<<all<<QStringList{};
  QTest::newRow("a ZIP+4 code is not a phone")<<"Room 555-B, Qty 1,234,567, ZIP 71674-4468"<<all<<QStringList{};
  QTest::newRow("an ISBN-13 is not a phone or card")<<"ISBN-13 978-4-579-64374-5 and 9784579643745"<<all<<QStringList{};
  // Sol on #1901.
  QTest::newRow("a Brazilian mobile shaped like a ZIP+4 is a phone")<<"Celular 91234-5678"<<all<<QStringList{"phone: 91234-5678"};
  QTest::newRow("a ZIP+4 after a state code is not a phone")<<"Austin, TX 78701-4468"<<all<<QStringList{};
  QTest::newRow("a phone after a numbered label is a phone")<<"Phone2:202-555-0143"<<all<<QStringList{"phone: Phone2:202-555-0143"};
  QTest::newRow("a spaced ISBN is not a phone")<<"ISBN 978 4 579 64374 5"<<all<<QStringList{};
  {const auto hex=QString("47a74f445ca32dc645e3be45b37a6937")+"d105dae65638df71a251c0d0608b117d";
   QTest::newRow("a camelCase secret name beats a hash word")<<"clientSecret = "+hex+" // SHA-256"<<all<<QStringList{"key: clientSecret = "+hex+" // SHA-256"};}
  {const auto masked=QString("sk")+"_li"+"ve_"+QString(32,'X')+"9aB3dE5fG7hJ";
   // #1894: a run of X no key holds marks a masked display, whose tail is meant to be seen.
   QTest::newRow("a masked key with a tail shown stays visible")<<"key "+masked<<all<<QStringList{};}
  QTest::newRow("half zeros and the rest a token stays covered")<<"TOKEN "+QString(16,'0')+"RRRRRRRRaaaabbbb"<<QStringList{"key"}<<QStringList{"key: TOKEN "+QString(16,'0')+"RRRRRRRRaaaabbbb"};
  // Sol on #1901, round 2.
  QTest::newRow("a Brazilian mobile after a country code word is a phone")<<"Celular BR 91234-5678"<<all<<QStringList{"phone: 91234-5678"};
  QTest::newRow("a bare ZIP+4 is not a phone")<<"Ship to 71674-4468"<<all<<QStringList{};
  QTest::newRow("a phone after a two-digit label is a phone")<<"Phone12:202-555-0143"<<all<<QStringList{"phone: Phone12:202-555-0143"};
  QTest::newRow("a phone after an ISBN-10 ending in X is a phone")<<"ISBN-10: 0-8044-2957-X 202-555-0143"<<all<<QStringList{"phone: 202-555-0143"};
  QTest::newRow("a phone after an ISBN-10 on its line is a phone")<<"ISBN-10: 0-306-40615-2, 202-555-0143"<<all<<QStringList{"phone: 202-555-0143"};
  {const auto sha=QString("efd984b6b1813c2c344524c7")+"8d8f21a76afb9fab";
   QTest::newRow("a camelCase digest label leaves its hash visible")<<"gitSha: "+sha<<all<<QStringList{};}
  QTest::newRow("a phone after an ISBN on its line is a phone")<<"ISBN 978-4-579-64374-5, phone 202-555-0143"<<all<<QStringList{"phone: 202-555-0143"};
  {const auto hex=QString("47a74f445ca32dc645e3be45b37a6937")+"d105dae65638df71a251c0d0608b117d";
   QTest::newRow("an acronym secret name beats a hash word")<<"clientAPISecret = "+hex+" // SHA-256"<<all<<QStringList{"key: clientAPISecret = "+hex+" // SHA-256"};}
  QTest::newRow("a placeholder Stripe key before prose stays visible")<<QString("sk")+"_li"+"ve_"+QString(32,'X')+" was a redacted example"<<all<<QStringList{};
  QTest::newRow("a placeholder after its name stays visible")<<"API_TOKEN="+QString(32,'X')<<QStringList{"key"}<<QStringList{};
  QTest::newRow("a phone after a label's colon is still a phone")<<"Tel:+1 202 555 0143"<<all<<QStringList{"phone: Tel:+1 202 555 0143"};
  {const auto stripeMask=QString("sk")+"_li"+"ve_"+QString(24,'X'),awsMask=QString("AK")+"IA"+QString(16,'X'),partly=QString("sk")+"_li"+"ve_"+"51HqZK"+QString(18,'X');
   QTest::newRow("a placeholder Stripe key stays visible")<<"Placeholder key "+stripeMask<<all<<QStringList{};
   QTest::newRow("a placeholder AWS key stays visible")<<"Placeholder AWS "+awsMask<<all<<QStringList{};
   QTest::newRow("asterisks and zeros are placeholders too")<<"token "+QString(32,'*')+" id "+QString(32,'0')<<all<<QStringList{};
   QTest::newRow("a masked key stays visible")<<"key "+partly<<all<<QStringList{};}
  QTest::newRow("base64 run")<<"value "+base64Run<<all<<QStringList{"key: value "+base64Run};
  QTest::newRow("a long path is not a run")<<"cd /usr/share/tesseract-ocr/5/tessdata/configs"<<all<<QStringList{};
  QTest::newRow("only the categories asked for")<<"mail a@example.com or call +1 202 555 0143"<<QStringList{"phone"}<<QStringList{"phone: +1 202 555 0143"};
  QTest::newRow("Unicode digits and full-width forms")<<"Call ٥٥٥ ١٢٣ ٤٥٦٧ or ５５５-１２３-４５６７"<<all<<QStringList{"phone: ٥٥٥ ١٢٣ ٤٥٦٧","phone: ５５５-１２３-４５６７"};
  QTest::newRow("ordinary prose matches nothing")<<"The quick brown fox jumps over 3 lazy dogs in 2026. | Version 1.2 shipped on Monday."<<all<<QStringList{};
 }
 void categoriesMatchTheirShapes() {
  QFETCH(QString,page);QFETCH(QStringList,categories);QFETCH(QStringList,expected);
  QCOMPARE(found(words(page),categories),expected);
 }
 // #1618, decision 3: the account's names and the user's own list. Words
 // are literal, never a regex, compared ignoring case and accents.
 // Sol on #1826 (S17): two 8-digit phone numbers in table columns read as 4-4-4-4; the wide gap between the
 // columns keeps them two numbers, both found. The same groups evenly spaced are one card-shaped number.
 void columnsOfPhoneNumbersAreNotOneCard() {
  const auto row=[](int gap){QList<RecognizedWord> out;int x=0;for(const auto* text:{"7946","0958","7946","0959"}){out.append({QString::fromLatin1(text),QRect(x,0,32,20),90,0});x+=32+(out.size()==2?gap:8);}return out;};
  QCOMPARE(found(row(120),QStringList{"phone"}),(QStringList{"phone: 7946 0958","phone: 7946 0959"}));
  QCOMPARE(found(row(8),QStringList{"phone"}),QStringList{});
 }
 // #1859: a token that ends its line takes a token-shaped next line only where a wrap leaves it:
 // right below, about a line height down, starting no further right than the line above.
 void aTokenTakesOnlyALineAWrapLeaves() {
  const auto token=QString("gh")+"p_"+"aB3dE5fG7hJ9kL1mN3pQ"+"5rS7tU9vW";
  const auto page=[&](int dx,int dy){auto out=words("Your token "+token+" | 1xY3zA5bC7");out.last().rect.translate(dx,dy);return out;};
  QCOMPARE(found(page(0,0),QStringList{"key"}),QStringList{"key: "+token+" 1xY3zA5bC7"});
  QCOMPARE(found(page(0,350),QStringList{"key"}),QStringList{"key: "+token});
  QCOMPARE(found(page(600,0),QStringList{"key"}),QStringList{"key: "+token});
  // Sol on #1863: word boxes are shorter than the line pitch (rows 50 px apart, boxes 20 px high)...
  QCOMPARE(found(page(0,20),QStringList{"key"}),QStringList{"key: "+token+" 1xY3zA5bC7"});
  // ...and a wide gap between words above is no character width.
  auto spread=page(0,0);spread[2].rect.translate(400,0);spread.last().rect.translate(60,0);
  QCOMPARE(found(spread,QStringList{"key"}),QStringList{"key: "+token});
 }
 // #1895: a table's label cell a column away from its value is not part of the token.
 void aLabelCellStaysOutOfTheToken() {
  const auto aws=QString("AK")+"IA"+"IOSFODNN7"+"EXAMPLE";
  auto page=words("AWS "+aws);
  QCOMPARE(found(page,QStringList{"key"}),QStringList{"key: AWS "+aws});
  page.last().rect.translate(32,0);
  QCOMPARE(found(page,QStringList{"key"}),QStringList{"key: "+aws});
  // A key split after its own prefix by the same gap stays one token.
  {const auto split=words(QString("id AK")+"IA IOSFODNN7"+"EXAMPLE");auto moved=split;moved.last().rect.translate(32,0);
   QCOMPARE(found(moved,QStringList{"key"}),QStringList{QString("key: AK")+"IA IOSFODNN7"+"EXAMPLE"});}
  {const auto split=words(QString("ID:AK")+"IA IOSFODNN7"+"EXAMPLE");auto moved=split;moved.last().rect.translate(32,0);
   QCOMPARE(found(moved,QStringList{"key"}),QStringList{QString("key: ID:AK")+"IA IOSFODNN7"+"EXAMPLE"});}
  page.last().rect.translate(118,0);
  QCOMPARE(found(page,QStringList{"key"}),QStringList{"key: "+aws});
 }
 // #1894: in an editor's .env file the lines after a token are new assignments, numbered or not, never
 // the token's wrapped remainder; a remainder with no "=" inside still is.
 void aNewAssignmentIsNoWrappedRemainder() {
  const auto secret=QString("wJalrXUtnFEMI/K7MDENG/")+"bPxRfiCYzK9q2Lm4ab";
  const auto session=QString("SESSION_SECRET=")+"q9Lm2Vx7Tz4Rb8Nc1Kp6";
  QCOMPARE(found(words("3 AWS_SECRET_ACCESS_KEY="+secret+" | 4 "+session+" | 5 PORT=8080"),QStringList{"key"}).first(),"key: 3 AWS_SECRET_ACCESS_KEY="+secret);
  QCOMPARE(found(words("AWS_SECRET_ACCESS_KEY="+secret+" | "+session),QStringList{"key"}).first(),"key: AWS_SECRET_ACCESS_KEY="+secret);
  // A line number recognition misread as a sign ("&" for 8) still starts a new line.
  QCOMPARE(found(words("7 AWS_SECRET_ACCESS_KEY="+secret+" | & "+session),QStringList{"key"}).first(),"key: 7 AWS_SECRET_ACCESS_KEY="+secret);
  QCOMPARE(found(words("AWS_SECRET_ACCESS_KEY="+secret+" | Xc4Vb9Nm1QwEr3Ty8Ui5=="),QStringList{"key"}),QStringList{"key: AWS_SECRET_ACCESS_KEY="+secret+" Xc4Vb9Nm1QwEr3Ty8Ui5=="});
  // Sol on #1901: padding before closing punctuation is a remainder's end; an empty assignment and a
  // digest line are not remainders.
  QCOMPARE(found(words("KEY=\""+secret+" | Xc4Vb9Nm1QwEr3Ty8Ui5=\","),QStringList{"key"}),QStringList{"key: KEY=\""+secret+" Xc4Vb9Nm1QwEr3Ty8Ui5=\","});
  QCOMPARE(found(words("AWS_SECRET_ACCESS_KEY="+secret+" | DATABASE_URL="),QStringList{"key"}),QStringList{"key: AWS_SECRET_ACCESS_KEY="+secret});
  QCOMPARE(found(words("AWS_SECRET_ACCESS_KEY="+secret+" | DATABASE_URL=\"\""),QStringList{"key"}),QStringList{"key: AWS_SECRET_ACCESS_KEY="+secret});
  QCOMPARE(found(words("KEY=\""+secret+" | Xc4Vb9Nm1QwEr3Ty8Ui5==\u201D // note"),QStringList{"key"}),QStringList{"key: KEY=\""+secret+" Xc4Vb9Nm1QwEr3Ty8Ui5==\u201D"});
  QCOMPARE(found(words("AWS_SECRET_ACCESS_KEY="+secret+" | DEBUG_MODE = 0"),QStringList{"key"}),QStringList{"key: AWS_SECRET_ACCESS_KEY="+secret});
  // #1914: an inline comment after padding read as its own word, and an empty detached assignment.
  QCOMPARE(found(words("KEY="+secret+" | Xc4Vb9Nm1QwEr3Ty8Ui5 = # dev"),QStringList{"key"}).first(),"key: KEY="+secret+" Xc4Vb9Nm1QwEr3Ty8Ui5 = # dev");
  QCOMPARE(found(words("AWS_SECRET_ACCESS_KEY="+secret+" | DATABASE_URL ="),QStringList{"key"}),QStringList{"key: AWS_SECRET_ACCESS_KEY="+secret});
  QCOMPARE(found(words("KEY="+secret+" | shaB7m2QaK8V= # dev"),QStringList{"key"}).first(),"key: KEY="+secret+" shaB7m2QaK8V= # dev");
  QCOMPARE(found(words("KEY="+secret+" | Xc4Vb9Nm1QwEr3Ty8Ui5 ="),QStringList{"key"}),QStringList{"key: KEY="+secret+" Xc4Vb9Nm1QwEr3Ty8Ui5 ="});
  QCOMPARE(found(words("KEY="+secret+" | shaB7m2QaK8V="),QStringList{"key"}),QStringList{"key: KEY="+secret+" shaB7m2QaK8V="});
  QCOMPARE(found(words("KEY=\""+secret+" | shaB7m2QaK8V\""),QStringList{"key"}),QStringList{"key: KEY=\""+secret+" shaB7m2QaK8V\""});
  QCOMPARE(found(words("AWS_SECRET_ACCESS_KEY="+secret+" | SHA1 "+QString("efd984b6b1813c2c344524c7")+"8d8f21a76afb9fab"),QStringList{"key"}),QStringList{"key: AWS_SECRET_ACCESS_KEY="+secret});
 }
 void namesAndListsMatchTheirSpellings_data() {
  QTest::addColumn<QString>("page");QTest::addColumn<QStringList>("categories");QTest::addColumn<quickRedact::Sources>("sources");QTest::addColumn<QStringList>("expected");
  const QStringList name{"name"},list{"list"};
  const auto me=account("phudson","Patrick Hudson");const auto entry=terms({"Patrick Hudson"});
  // The Patrick Hudson table: as a list entry and as the account's name.
  for(const auto& [row,page,marked]:QList<std::tuple<const char*,QString,QString>>{
   {"spaced","My name is Patrick Hudson","Patrick Hudson"},{"dashed","my github username is patrick-hudson","patrick-hudson"},
   {"underscored","user patrick_hudson here","patrick_hudson"},{"run together","PatrickHudson","PatrickHudson"},
   {"before an @","mail patrick.hudson@x.io today","patrick.hudson@x.io"},{"i read as l","Signed in as Patrlck Hudson","Patrlck Hudson"},
   {"split by recognition","by patrick- hudson","patrick- hudson"},{"a spaced dash","Patrick - Hudson wrote","Patrick - Hudson"},
   {"curly apostrophe","Patrick Hudson’s notes","Patrick Hudson’s"}}){
   QTest::addRow("list entry: %s",row)<<page<<list<<entry<<QStringList{"list: "+marked};
   QTest::addRow("name: %s",row)<<page<<name<<me<<QStringList{"name: "+marked};
  }
  QTest::newRow("name: last, first")<<"Author: Hudson, Patrick"<<name<<me<<QStringList{"name: Hudson, Patrick"};
  QTest::newRow("name: initial and last name")<<"phudson p.hudson P. Hudson p-hudson"<<name<<me<<QStringList{"name: phudson","name: p.hudson","name: P. Hudson","name: p-hudson"};
  QTest::newRow("name: up to four digits after a login")<<"phudson2 phudson1234 phudson12345"<<name<<me<<QStringList{"name: phudson2","name: phudson1234"};
  QTest::newRow("name: each part alone")<<"Patrick wrote to Hudson"<<name<<me<<QStringList{"name: Patrick","name: Hudson"};
  QTest::newRow("name: a part inside a path and a prompt")<<"cd /home/patrick/project | patrick@d69:~$ ls"<<name<<me<<QStringList{"name: /home/patrick/project","name: patrick@d69:~$"};
  QTest::newRow("name: a wrapped line with the dash at its end")<<"see patrick- | hudson now"<<name<<me<<QStringList{"name: patrick- hudson"};
  QTest::newRow("name: a wrapped line at the column")<<"ABCDEFGHIJKLMNO | PQRSTUVWXYZABCD | Contact Patrick | Hudson today"<<name<<me<<QStringList{"name: Patrick Hudson"};
  QTest::newRow("name: lines without wrap evidence are separate")<<"Patrick | Hudson"<<list<<entry<<QStringList{};
  // Near misses (the over-redaction guard): only the surname in Hudson Bay.
  QTest::newRow("name: near misses")<<"Patricks studied the pattern of pat near Hudson Bay and hudsonia"<<name<<me<<QStringList{"name: Hudson"};
  QTest::newRow("list entry: near misses")<<"patricks pattern pat hudsonia patrick-hudsonx"<<list<<entry<<QStringList{};
  QTest::newRow("list entry: matched only whole")<<"Patrick alone, and Hudson, Patrick"<<list<<entry<<QStringList{};
  QTest::newRow("name: a login")<<"cd /home/user/project | user@host:~$ | user, users username1 user1234 user12345"<<name<<account("user",{})
   <<QStringList{"name: /home/user/project","name: user@host:~$","name: user,","name: user1234"};
  QTest::newRow("name: a login and a real name the same")<<"patrick wrote"<<name<<account("patrick","patrick")<<QStringList{"name: patrick"};
  QTest::newRow("name: a part of two letters is skipped")<<"Al went home with Smith and Al Smith"<<name<<account("asmith","Al Smith")<<QStringList{"name: Smith","name: Al Smith"};
  QTest::newRow("name: no account")<<"Patrick Hudson"<<name<<account({},{})<<QStringList{};
  QTest::newRow("accents either way")<<"Jose and José"<<list<<terms({"José"})<<QStringList{"list: Jose","list: José"};
  QTest::newRow("accents in the text")<<"José wrote"<<list<<terms({"Jose"})<<QStringList{"list: José"};
  QTest::newRow("regex characters are literal")<<"x+y? xxy [ACME]* ACME srvdata srv_data srvXdata"<<list<<terms({"x+y?","[ACME]*","srv.data"})
   <<QStringList{"list: x+y?","list: [ACME]*","list: srvdata","list: srv_data"};
  QTest::newRow("a term and a pattern in a code comment")<<"# settings for the acme-build-7 pipeline (JIRA-2210)"<<list<<[]{auto s=terms({"acme-build-7"});s.patterns={"\\bJIRA-\\d+\\b"};return s;}()<<QStringList{"list: acme-build-7","list: (JIRA-2210)"};
  QTest::newRow("a short term is matched exactly")<<"B0b and Bob"<<list<<terms({"Bob"})<<QStringList{"list: Bob"};
  QTest::newRow("a long term reads rn for m")<<"Harnmond and Hammond"<<list<<terms({"Hammond"})<<QStringList{"list: Harnmond","list: Hammond"};
  QTest::newRow("an empty list")<<"Patrick Hudson and anything"<<list<<quickRedact::Sources{}<<QStringList{};
  QTest::newRow("an email wins over a name")<<"mail patrick.hudson@x.io or Patrick"<<QStringList{"email","name","list"}<<quickRedact::Sources{"phudson","Patrick Hudson",{"Patrick Hudson"},{}}
   <<QStringList{"email: patrick.hudson@x.io","name: Patrick"};
  QTest::newRow("a word belongs to one match")<<"Patrick Hudson"<<QStringList{"name","list"}<<quickRedact::Sources{"phudson","Patrick Hudson",{"Hudson"},{}}<<QStringList{"name: Patrick Hudson"};
  QTest::newRow("only the categories asked for")<<"Patrick Hudson"<<QStringList{"email"}<<me<<QStringList{};
  // Decision 6: patterns, case as written, on the joined line text.
  QTest::newRow("pattern: a ticket across a space")<<"see JIRA- 1234 now"<<list<<patterns({R"(\bJIRA-\s*\d+\b)"})<<QStringList{"list: JIRA- 1234"};
  QTest::newRow("pattern: case as written")<<"ACME-12 acme-7"<<list<<patterns({R"(\bacme-\d+)"})<<QStringList{"list: acme-7"};
  QTest::newRow("pattern: (?i)")<<"ACME-12 acme-7"<<list<<patterns({R"((?i)\bacme-\d+)"})<<QStringList{"list: ACME-12","list: acme-7"};
  QTest::newRow("pattern: a hit spanning two words")<<"run build 42 ok"<<list<<patterns({R"(build \d+)"})<<QStringList{"list: build 42"};
  QTest::newRow("pattern: a hit inside a word marks the word")<<"host db7.corp.example up"<<list<<patterns({R"((?i)\b[a-z0-9-]+\.corp\.example\b)"})<<QStringList{"list: db7.corp.example"};
  QTest::newRow("pattern: zero-length hits are skipped")<<"abc 12 JIRA"<<list<<patterns({R"(\d*)",R"((?=JIRA))"})<<QStringList{"list: 12"};
  QTest::newRow("pattern: \\s reaches the next line")<<"ticket ABC- | 99 | ticket ABC | 98"<<list<<patterns({R"(ABC-\n\d+)",R"(ABC\s+98)"})<<QStringList{"list: ABC- 99","list: ABC 98"};
  QTest::newRow("pattern: Unicode properties")<<"Öffnung ÄRGER x"<<list<<patterns({R"(\b\p{Lu}{5}\b)"})<<QStringList{"list: ÄRGER"};
  // Round 1 (#1631), one row per finding.
  QTest::newRow("a pattern's own start-of-pattern items run as typed")<<"JIRA-1"<<list<<patterns({"(*LIMIT_MATCH=99999999)JIRA-\\d"})<<QStringList{"list: JIRA-1"};
  QTest::newRow("M3: an empty alternative still finds the match at that position")<<"see JIRA-7"<<list<<patterns({R"(|JIRA-\d)"})<<QStringList{"list: JIRA-7"};
  QTest::newRow("M3: patterns step over characters outside the BMP")<<QString::fromUtf8("\xF0\x9F\x98\x80 JIRA-5")<<list<<patterns({R"(\p{So}|JIRA-\d)"})
   <<QStringList{QString::fromUtf8("list: \xF0\x9F\x98\x80"),"list: JIRA-5"};
  QTest::newRow("M4: punctuation at an entry's edge stays")<<"env .env /app/.env envy"<<list<<terms({".env"})<<QStringList{"list: .env","list: /app/.env"};
  QTest::newRow("M4: trailing punctuation too")<<"env env. env.x"<<list<<terms({"env."})<<QStringList{"list: env.","list: env.x"};
  QTest::newRow("M4: an entry of punctuation alone matches nothing")<<". .. a.b"<<list<<terms({"."})<<QStringList{};
  QTest::newRow("M5: the login follows the part rules")<<"jane-doe janedoe2 jane_doe jane.doex"<<name<<account("jane.doe",{})<<QStringList{"name: jane-doe","name: janedoe2","name: jane_doe"};
  QTest::newRow("M5: a lone one-letter name builds no form")<<"J went to j and J."<<name<<account("zz","J")<<QStringList{};
  QTest::newRow("M5: last, first leaves out a middle name")<<"Doe, Jane wrote"<<name<<account("zz","Jane Q Doe")<<QStringList{"name: Doe, Jane"};
  QTest::newRow("M6: a doubled separator does not join")<<"Patrick -- Hudson and Patrick - - Hudson"<<list<<entry<<QStringList{};
  QTest::newRow("M6: one separator between spaces joins")<<"Patrick - Hudson and Patrick _ Hudson"<<list<<entry<<QStringList{"list: Patrick - Hudson","list: Patrick _ Hudson"};
  QTest::newRow("M7: overlapping occurrences")<<"ha ha ha"<<list<<terms({"ha ha"})<<QStringList{"list: ha ha","list: ha"};
  QTest::newRow("M8: full case folding")<<QString::fromUtf8("STRASSE Strasse Stra\xC3\x9F" "e")<<list<<terms({QString::fromUtf8("Stra\xC3\x9F" "e")})
   <<QStringList{"list: STRASSE","list: Strasse",QString::fromUtf8("list: Stra\xC3\x9F" "e")};
  QTest::newRow("M8: an Indic vowel sign is a letter, not an accent")<<QString::fromUtf8("\xE0\xA4\x95 \xE0\xA4\x95\xE0\xA4\xBF")<<list<<terms({QString::fromUtf8("\xE0\xA4\x95\xE0\xA4\xBF")})
   <<QStringList{QString::fromUtf8("list: \xE0\xA4\x95\xE0\xA4\xBF")};
  QTest::newRow("M8: a term does not end inside a letter's vowel sign")<<QString::fromUtf8("\xE0\xA4\x95\xE0\xA4\xBF \xE0\xA4\x95")<<list<<terms({QString::fromUtf8("\xE0\xA4\x95")})
   <<QStringList{QString::fromUtf8("list: \xE0\xA4\x95")};
  QTest::newRow("M8: a letter outside the BMP is a word character")<<QString::fromUtf8("\xF0\xA0\x80\x80" "abc abc")<<list<<terms({"abc"})<<QStringList{"list: abc"};
  // Round 2 (#1631), one row per finding.
  QTest::newRow("M12: \\G anchors at the start of a search, as typed")<<"x ID-5 | ID-6 y"<<list<<patterns({R"(\GID-\d+)"})<<QStringList{"list: ID-6"};
  QTest::newRow("M12: (*SKIP) skips what it passed")<<"ID-1 D-2"<<list<<patterns({R"(ID(*SKIP)(*FAIL)|D-\d)"})<<QStringList{"list: D-2"};
  QTest::newRow("M4: a space inside an entry is a join")<<"see # 42 and #42 and #-42 and 42"<<list<<terms({"# 42"})<<QStringList{"list: # 42","list: #42","list: #-42"};
  QTest::newRow("M8: default case folding keeps dotless i apart")<<QString::fromUtf8("kir k\xC4\xB1r KIR")<<list<<terms({QString::fromUtf8("k\xC4\xB1r")})<<QStringList{QString::fromUtf8("list: k\xC4\xB1r")};
  QTest::newRow("M8: capital sharp s folds to ss")<<QString::fromUtf8("STRASSE STRA\xE1\xBA\x9E" "E")<<list<<terms({QString::fromUtf8("stra\xC3\x9F" "e")})<<QStringList{"list: STRASSE",QString::fromUtf8("list: STRA\xE1\xBA\x9E" "E")};
  QTest::newRow("M11: up to two join characters")<<"patrick__hudson patrick--hudson patrick._hudson patrick - hudson patrick___hudson"<<list<<entry
   <<QStringList{"list: patrick__hudson","list: patrick--hudson","list: patrick._hudson","list: patrick - hudson"};
  // Round 3 (#1631).
  QTest::newRow("M4: U+0085 inside an entry is a join too")<<QString::fromUtf8("see # 42 and #42")<<list<<terms({QString::fromUtf8("#\xC2\x85" "42")})<<QStringList{"list: # 42","list: #42"};
  // Round 4 (S4): a lower-to-upper change outside the BMP splits an entry too.
  QTest::newRow("S4: Deseret camel case")<<QString::fromUtf8("\xF0\x90\x90\xA8-\xF0\x90\x90\x81 and \xF0\x90\x90\xA8\xF0\x90\x90\x81")<<list<<terms({QString::fromUtf8("\xF0\x90\x90\xA8\xF0\x90\x90\x81")})
   <<QStringList{QString::fromUtf8("list: \xF0\x90\x90\xA8-\xF0\x90\x90\x81"),QString::fromUtf8("list: \xF0\x90\x90\xA8\xF0\x90\x90\x81")};
  QTest::newRow("terms and patterns together")<<"Hammond filed JIRA-9"<<list<<quickRedact::Sources{{},{},{"Hammond"},{R"(JIRA-\d)"}}<<QStringList{"list: Hammond","list: JIRA-9"};
 }
 void namesAndListsMatchTheirSpellings() {
  QFETCH(QString,page);QFETCH(QStringList,categories);QFETCH(quickRedact::Sources,sources);QFETCH(QStringList,expected);
  const auto candidates=words(page);
  const auto result=quickRedact::find(candidates,categories,sources);
  QCOMPARE(found(candidates,result),expected);QCOMPARE(result.skippedPatterns,QList<int>{});
 }
 // A pattern that backtracks for minutes stops within the budget, is named as
 // skipped, and the other patterns' marks are kept.
 void runawayPatternIsSkipped() {
  const auto page="JIRA-7 "+QString(5000,'a')+"b TICKET9 | "+QString(40,'a')+"b";
  const auto candidates=words(page);
  QElapsedTimer clock;clock.start();
  const auto result=quickRedact::find(candidates,{"list"},patterns({R"(\bJIRA-\d+\b)","(a+)+$",R"(\bTICKET\d+\b)"}),2000);
  QVERIFY2(clock.elapsed()<2000,qPrintable(QString::number(clock.elapsed())));
  QCOMPARE(found(candidates,result),(QStringList{"list: JIRA-7","list: TICKET9"}));
  QCOMPARE(result.skippedPatterns,QList<int>{1});
 }
 // Round 3 (M12, M13, M14): no windows. Each line is a subject, so a ticket
 // anywhere on a long line is found, a lookbehind sees the real line, and
 // repeated groups pair from the line's start.
 void eachLineIsOneSubject() {
  const auto filler=[](int n){return QString(n,'w');};
  // Where round 2's 1000-character windows cut.
  const auto lost=words(filler(245)+" "+filler(245)+" "+filler(245)+" "+filler(242)+" JIRA-7");
  QCOMPARE(found(lost,quickRedact::find(lost,{"list"},patterns({R"(\bJIRA-\d+)"}))),QStringList{"list: JIRA-7"});
  QStringList tagged;for(int i=0;i<300;++i)tagged.append(QStringLiteral("tag"));tagged.append("ID-4");
  const auto behind=words("tag ID-3 | "+tagged.join(' '));
  QCOMPARE(found(behind,quickRedact::find(behind,{"list"},patterns({R"((?<=^tag )ID-\d+)"}))),QStringList{"list: ID-3"});
  QStringList digits;for(int i=0;i<201;++i)digits.append(QStringLiteral("123456"));
  const auto pairs=words(digits.join(' '));
  const auto paired=quickRedact::find(pairs,{"list"},patterns({R"(\d{6} \d{6})"}));
  // 100 pairs from the start; the last number has no partner.
  QCOMPARE(paired.matches.size(),100);
  for(int i=0;i<100;++i)QCOMPARE(paired.matches.at(i).words,(QList<int>{2*i,2*i+1}));
 }
 // The lab recheck (31de163ea): an ordinary paragraph wrap carries no wrap
 // evidence, yet \s must reach the next line. Each pair of consecutive lines is
 // a subject joined with "\n"; a literal space does not match across a wrap
 // (QuickRedact.h says so), and nothing spans three lines.
 void patternsReachTheNextLine() {
  const auto wrap=words("Then we used quick | redact on the capture");
  QCOMPARE(found(wrap,quickRedact::find(wrap,{"list"},patterns({R"((?i)\bquick\sredact\b)"}))),QStringList{"list: quick redact"});
  QCOMPARE(found(wrap,quickRedact::find(wrap,{"list"},patterns({"quick redact"}))),QStringList{});
  const auto three=words("one ID- | 42 | and | ID- | x | 7");
  QCOMPARE(found(three,quickRedact::find(three,{"list"},patterns({R"(ID-\s\d+)",R"(ID-\s\w\s\d)"}))),QStringList{"list: ID- 42"});
  // A hit inside one line is claimed once, from the line: the pair adds none.
  const auto inside=words("JIRA-1 JIRA-2 | JIRA-3");
  const auto result=quickRedact::find(inside,{"list"},patterns({R"(JIRA-\d)"}));
  QCOMPARE(found(inside,result),(QStringList{"list: JIRA-1","list: JIRA-2","list: JIRA-3"}));
 }
 // Round 4 (M3): \C can match half of a character outside the BMP; the next
 // search starts on a whole one, so the pattern finishes and keeps its hits.
 void halfCharacterHitsKeepThePattern() {
  const auto candidates=words(QString::fromUtf8("\xF0\x9F\x98\x80x and JIRA-1"));
  const auto result=quickRedact::find(candidates,{"list"},patterns({R"(\C)"}));
  QCOMPARE(result.skippedPatterns,QList<int>{});
  QCOMPARE(found(candidates,result),(QStringList{QString::fromUtf8("list: \xF0\x9F\x98\x80x"),"list: and","list: JIRA-1"}));
 }
 // Round 4 (S3): a pair's hits that do not cross its break are left out where
 // they are found, so they never count toward a cap.
 void pairHitsOutsideTheBreakAreLeftOut() {
  QList<quickRedact::PatternOutcome> out;
  quickRedact::matchPatterns({"ab","cd","ab\ncd"},{R"(\S)",R"(b\nc)"},[&](quickRedact::PatternOutcome outcome){out.append(outcome);},{-1,-1,2});
  QCOMPARE(out.size(),2);
  QCOMPARE(out.at(0).hits,(QList<quickRedact::PatternHit>{{0,0,1},{0,1,1},{1,0,1},{1,1,1}}));
  QCOMPARE(out.at(1).hits,(QList<quickRedact::PatternHit>{{2,1,3}}));
 }
 // Round 5 (M3): \K after \C leaves an empty match inside a surrogate pair;
 // the retry starts on a whole character, so the pattern finishes and its
 // ticket lands.
 void emptyMatchInsideAPairStillFinishes() {
  const auto candidates=words(QStringLiteral("😀 JIRA-1"));
  const auto result=quickRedact::find(candidates,{"list"},patterns({QStringLiteral("JIRA-\\d|(?=😀)\\C\\K")}));
  QCOMPARE(result.skippedPatterns,QList<int>{});
  QCOMPARE(found(candidates,result),QStringList{"list: JIRA-1"});
 }
 // Round 5 (R1): an empty retry (\K after a letter) is never reported as a
 // hit; the search moves on and finds the ticket.
 void anEmptyRetryIsNeverAHit() {
  QList<quickRedact::PatternOutcome> out;
  quickRedact::matchPatterns({"a JIRA-1"},{R"((?=a)|a\K|JIRA-\d)"},[&](quickRedact::PatternOutcome outcome){out.append(outcome);});
  QCOMPARE(out.size(),1);QVERIFY(out.first().finished);
  QCOMPARE(out.first().hits,(QList<quickRedact::PatternHit>{{0,2,6}}));
 }
 // Round 5: ordinary patterns on text full of emoji and joined emoji
 // sequences, on one line and across a wrap. Every hit lands, and every range
 // starts and ends on a whole character at a word edge.
 void ordinaryPatternsAmongEmoji() {
  const QString family=QStringLiteral("👨‍👩‍👧"),flag=QStringLiteral("🏳️‍🌈");
  const QString first=family+" JIRA-12 🎉JIRA-3 😀 quick",second="redact "+flag+" JIRA-77👍 x";
  const QStringList list{R"(\bJIRA-\d+\b)",R"((?i)\bquick\sredact\b)"};
  const auto candidates=words(first+" | "+second);
  const auto result=quickRedact::find(candidates,{"list"},patterns(list));
  QCOMPARE(result.skippedPatterns,QList<int>{});
  QCOMPARE(found(candidates,result),(QStringList{"list: JIRA-12",QStringLiteral("list: 🎉JIRA-3"),"list: quick redact",QStringLiteral("list: JIRA-77👍")}));
  // The ranges themselves, over the subjects find sends: the two lines and their pair.
  const QStringList subjects{first,second,first+"\n"+second};
  QList<quickRedact::PatternOutcome> out;
  quickRedact::matchPatterns(subjects,list,[&](quickRedact::PatternOutcome outcome){out.append(outcome);},{-1,-1,first.size()});
  QStringList texts;
  for(const auto& outcome:out){
   QVERIFY(outcome.finished);
   for(const auto& hit:outcome.hits){
    const auto& text=subjects.at(hit.subject);const auto end=hit.start+hit.length;
    QVERIFY(!text.at(hit.start).isLowSurrogate());QVERIFY(end==text.size()||!text.at(end).isLowSurrogate());
    texts.append(text.mid(hit.start,hit.length));
   }
  }
  QCOMPARE(texts,(QStringList{"JIRA-12","JIRA-3","JIRA-77","quick\nredact"}));
 }
 // CodeRabbit 4612083918: a match across two visual rows makes two marks, one
 // per row, even when recognition gave both rows one line id; each mark comes
 // with its own words' box. One row with two line ids is one mark.
 void marksFollowVisualRows() {
  const QList<RecognizedWord> two{{"ALPHA-",QRect(0,0,48,20),90,0},{"BETA",QRect(200,40,32,20),90,0}};
  const auto boxes=quickRedact::markBoxes(two,{"list",{0,1}},QRect(0,0,400,200));
  QCOMPARE(boxes.size(),2);
  QCOMPARE(boxes.at(0).words,QRect(0,0,48,20));QCOMPARE(boxes.at(1).words,QRect(200,40,32,20));
  QVERIFY(!boxes.at(0).mark.intersects(boxes.at(1).mark));
  const QList<RecognizedWord> one{{"split",QRect(0,0,40,20),90,0},{"line",QRect(48,1,32,20),90,1}};
  const auto row=quickRedact::markBoxes(one,{"list",{0,1}},QRect(0,0,400,200));
  QCOMPARE(row.size(),1);QCOMPARE(row.first().words,QRect(0,0,80,21));
  QCOMPARE(quickRedact::marks(two,{"list",{0,1}},QRect(0,0,400,200)).size(),2);
 }
 // #1838: the side margin is one and a half of the edge word's own glyphs, not of
 // the row's span with its gaps counted as characters, and it stops short of an
 // unmatched word on the row. The geometry is #1806's capture: before, the cover
 // reached 10 px into "Card".
 void marksLeaveNeighboursReadable() {
  const QList<RecognizedWord> row{{"Card",QRect(24,100,88,30),90,0},{"4111",QRect(140,100,88,30),90,0},{"1111",QRect(250,100,88,30),90,0},
                                  {"1111",QRect(360,100,88,30),90,0},{"1111",QRect(470,100,88,30),90,0},{"exp",QRect(600,100,66,30),90,0}};
  const QRect digits(140,100,418,30);
  const auto boxes=quickRedact::markBoxes(row,{"card",{1,2,3,4}},QRect(0,0,800,300));
  QCOMPARE(boxes.size(),1);QCOMPARE(boxes.first().words,digits);
  const auto mark=boxes.first().mark;
  QVERIFY2(mark.left()>row.at(0).rect.right()+1,qPrintable(QString::number(mark.left())));
  QVERIFY2(mark.right()<row.at(5).rect.left()-1,qPrintable(QString::number(mark.right())));
  QVERIFY(mark.contains(digits));
  // With room, the margin is still a glyph and a half (22 px glyphs) on each side.
  const QList<RecognizedWord> alone{row.at(1),row.at(2),row.at(3),row.at(4)};
  const auto free=quickRedact::markBoxes(alone,{"card",{0,1,2,3}},QRect(0,0,800,300)).first().mark;
  QCOMPARE(free.left(),digits.left()-33);QCOMPARE(free.right(),digits.right()+33);
  // A neighbour touching the match never shrinks the cover below the matched box,
  // and a word on another row does not limit it.
  const QList<RecognizedWord> tight{{"x",QRect(130,100,9,30),90,0},row.at(1),{"y",QRect(229,100,9,30),90,0},{"below",QRect(0,160,800,30),90,1}};
  const auto close=quickRedact::markBoxes(tight,{"card",{1}},QRect(0,0,800,300)).first().mark;
  QCOMPARE(close.left(),140);QCOMPARE(close.right(),227);QVERIFY(close.top()<100&&close.bottom()>129);
 }
 // #1838, measured on Liberation Sans at 13 px: the last "1111" box ends 4 px
 // before its ink and the box of "is" begins 6 px before its own, so a cover
 // that stops at the neighbour's box leaves a sliver of the digit. With the
 // picture, the cover runs across the space and stops before the "i". The
 // background is noisy and the glyphs are solid, so the ink is the commonest
 // exact colour: the paper must still be found.
 void marksCrossTheSpaceInThePicture() {
  QImage picture(200,120,QImage::Format_RGB32);
  quint32 seed=1838;
  for(int y=0;y<picture.height();++y)for(int x=0;x<picture.width();++x){seed=seed*1664525u+1013904223u;const int d=int((seed>>24)%13)-6;picture.setPixel(x,y,qRgb(236+d,236+d,236+d));}
  const auto bars=[&](QImage& image,std::initializer_list<QPair<int,int>> columns){for(const auto& [from,to]:columns)for(int x=from;x<=to;++x)for(int y=81;y<=89;++y)image.setPixel(x,y,qRgb(40,40,40));};
  bars(picture,{{96,98},{103,105},{110,112},{117,120},{126,127},{129,133}});
  const QList<RecognizedWord> row{{"1111",QRect(94,79,23,17),90,0},{"is",QRect(120,79,12,17),90,0}};
  const auto mark=quickRedact::markBoxes(row,{"card",{0}},picture.rect(),picture).first().mark;
  QVERIFY2(mark.right()>=120&&mark.right()<126,qPrintable(QString::number(mark.right())));
  QVERIFY(mark.left()<=96);
  // No blank column between the words (a textured background): the whole
  // margin, a glyph and a half, stays.
  QImage checks(200,120,QImage::Format_RGB32);
  for(int y=0;y<checks.height();++y)for(int x=0;x<checks.width();++x)checks.setPixel(x,y,(x+y)%2?qRgb(255,255,255):qRgb(0,0,0));
  QCOMPARE(quickRedact::markBoxes(row,{"card",{0}},checks.rect(),checks).first().mark.right(),116+9);
 }
 // Sol review of #1842: the cover never stops at a gap inside the match, and
 // a neighbour's box is trusted only where the picture agrees with it.
 void marksNeverStopInsideTheMatch() {
  const auto page=[](QImage::Format format,QRgb paper){QImage image(200,60,format);image.fill(paper);return image;};
  const auto ink=[](QImage& image,int x0,int x1,int y0,int y1){for(int x=x0;x<=x1;++x)for(int y=y0;y<=y1;++y)image.setPixel(x,y,qRgba(0,0,0,255));};
  const auto right=[](const QList<RecognizedWord>& row,const QImage& image){return quickRedact::markBoxes(row,{"list",{0}},image.rect(),image).first().mark.right();};
  // "abcd" with its box ending at 99 (10 px glyphs, a 15 px margin); its last
  // glyph's ink is at 103-105 after a 3 px gap; the neighbour's ink starts at 107.
  QImage wide=page(QImage::Format_RGB32,qRgb(255,255,255));
  ink(wide,62,99,12,27);ink(wide,103,105,12,27);ink(wide,107,120,12,27);
  // S1: the neighbour's box where its ink starts, then 6 px early, in the gap
  // inside the match: both times the glyph at 103-105 is covered and 107 is not.
  QCOMPARE(right({{"abcd",QRect(60,10,40,20),90,0},{"next",QRect(107,10,20,20),90,0}},wide),105);
  QCOMPARE(right({{"abcd",QRect(60,10,40,20),90,0},{"next",QRect(101,10,20,20),90,0}},wide),105);
  // Round 3: a narrow last glyph (102-103) within a pixel of the neighbour's
  // box edge (103), after a narrower blank than the space behind it (104-108),
  // is the match's; the neighbour's ink at 109 stays.
  QImage narrow=page(QImage::Format_RGB32,qRgb(255,255,255));
  ink(narrow,62,99,12,27);ink(narrow,102,103,12,27);ink(narrow,109,120,12,27);
  QCOMPARE(right({{"abcd",QRect(60,10,40,20),90,0},{"next",QRect(103,10,20,20),90,0}},narrow),107);
  // S8: a picture cut off just past the neighbour's first ink still shows the space.
  QCOMPARE(right({{"abcd",QRect(60,10,40,20),90,0},{"next",QRect(107,10,20,20),90,0}},wide.copy(0,0,115,60)),105);
  // S6: a box that agrees with the picture is not crossed for an equally near
  // gap after the neighbour's first stroke (space 102-103, stroke 104, gap 105).
  QImage stroke=page(QImage::Format_RGB32,qRgb(255,255,255));
  ink(stroke,62,101,12,27);ink(stroke,104,104,12,27);ink(stroke,106,120,12,27);
  QCOMPARE(right({{"abcd",QRect(60,10,40,20),90,0},{"next",QRect(104,10,20,20),90,0}},stroke),102);
  // S2: matched ink below the neighbour's rows runs to 106, right up to the
  // neighbour's box; no blank column is left, so the whole margin stays.
  QImage low=page(QImage::Format_RGB32,qRgb(255,255,255));
  ink(low,62,99,12,17);ink(low,100,106,22,27);ink(low,107,126,10,17);
  QVERIFY(right({{"abcd",QRect(60,10,40,20),90,0},{"up",QRect(107,10,20,8),90,0}},low)>=106);
  // S5: ink filling most of the mark's rows is not taken for paper.
  QImage heavy=page(QImage::Format_RGB32,qRgb(255,255,255));
  ink(heavy,62,99,10,29);ink(heavy,99,101,5,34);ink(heavy,102,102,8,31);ink(heavy,104,114,8,31);
  QVERIFY(right({{"abcd",QRect(60,10,40,20),90,0},{"next",QRect(104,10,11,20),90,0}},heavy)>=102);
  // S3: opaque ink on a half-transparent white picture: what shows between the
  // words depends on the viewer, so the whole margin stays.
  QImage clear=page(QImage::Format_ARGB32,qRgba(255,255,255,128));
  ink(clear,62,99,12,27);ink(clear,107,120,12,27);
  QCOMPARE(right({{"abcd",QRect(60,10,40,20),90,0},{"next",QRect(107,10,20,20),90,0}},clear),99+15);
 }
 // #1846: one match whose words sit in two columns of a row (two windows side by side, as k72's
 // capture showed) gets a mark per column, so no mark spans the space between them; words a word
 // space apart keep one mark.
 void marksSplitAtColumns() {
  const QList<RecognizedWord> row{{"4111",QRect(0,0,32,20),90,0},{"1111",QRect(40,0,32,20),90,0},{"1111",QRect(80,0,32,20),90,0},{"1111",QRect(120,0,32,20),90,0},
   {"MC",QRect(700,0,16,20),90,0},{"5555",QRect(724,0,32,20),90,0},{"5555",QRect(764,0,32,20),90,0},{"5555",QRect(804,0,32,20),90,0},{"4444",QRect(844,0,32,20),90,0}};
  const auto boxes=quickRedact::markBoxes(row,{"key",{0,1,2,3,5,6,7,8}},QRect(0,0,1000,100));
  QCOMPARE(boxes.size(),2);
  QCOMPARE(boxes.at(0).words,QRect(0,0,152,20));QCOMPARE(boxes.at(1).words,QRect(724,0,152,20));
  // Neither mark spans the gap between the columns; a mark's inner margin may reach a word beside it.
  for(const auto& box:boxes)QVERIFY(!box.mark.intersects(QRect(200,0,480,20)));
  const auto together=quickRedact::markBoxes(row,{"key",{0,1,2,3}},QRect(0,0,1000,100));
  QCOMPARE(together.size(),1);
 }
 // Sol on #1846: a side split off from its row keeps the whole margin, since a box can stop short
 // of its last glyph (ink at x 103-105 past a box ending at 99, a neighbour box from 103 whose ink
 // starts at 109), and every run keeps the padded height of the whole visual row.
 void splitMarksKeepTheirMargins() {
  QImage image(1000,100,QImage::Format_RGB32);image.fill(Qt::white);
  {QPainter p(&image);p.fillRect(QRect(0,22,100,16),Qt::black);p.fillRect(QRect(103,22,3,16),Qt::black);p.fillRect(QRect(109,22,12,16),Qt::black);p.fillRect(QRect(600,22,60,16),Qt::black);}
  const QList<RecognizedWord> row{{"4111",QRect(0,20,100,20),90,0},{"MC",QRect(103,20,18,20),90,0},{"5555",QRect(600,20,60,20),90,0}};
  const auto boxes=quickRedact::markBoxes(row,{"key",{0,2}},QRect(0,0,1000,100),image);
  QCOMPARE(boxes.size(),2);
  QVERIFY(boxes.at(0).mark.right()>=105);
  const QList<RecognizedWord> stepped{{"4111",QRect(0,20,40,20),90,0},{"5555",QRect(600,30,40,20),90,0}};
  const auto tall=quickRedact::markBoxes(stepped,{"key",{0,1}},QRect(0,0,1000,100));
  QCOMPARE(tall.size(),2);
  for(const auto& box:tall){QVERIFY(box.mark.top()<=13);QVERIFY(box.mark.bottom()>=57);}
  // The outer edge's neighbour scan covers the row's height too (Sol's geometry): a stroke at
  // y 46-47 below a short run is matched ink, not blank paper.
  QImage high(200,80,QImage::Format_RGB32);high.fill(Qt::white);
  {QPainter p(&high);p.fillRect(QRect(QPoint(10,24),QPoint(19,45)),Qt::black);p.fillRect(QRect(QPoint(98,30),QPoint(109,39)),Qt::black);
   p.fillRect(QRect(QPoint(116,30),QPoint(124,39)),Qt::black);p.fillRect(QRect(QPoint(113,46),QPoint(115,47)),Qt::black);}
  const QList<RecognizedWord> edge{{"41",QRect(QPoint(10,24),QPoint(19,45)),90,0},{"555",QRect(QPoint(98,30),QPoint(109,39)),90,0},{"ok",QRect(QPoint(116,30),QPoint(124,39)),90,0}};
  const auto split=quickRedact::markBoxes(edge,{"key",{0,1}},QRect(0,0,200,80),high);
  QCOMPARE(split.size(),2);
  QVERIFY(split.at(1).mark.right()>=115);
 }
 // #1854, as putgd-ocr read "write to jane.doe@example.com before" in Red Hat
 // Display at 10 px on a dark panel: the dot was dropped and "jane" and
 // "doe@example.com" came back 2 px apart, like every word space on the line.
 // The dot's ink lies between the boxes and no blank run there is wider than
 // a gap between the address's own glyphs, so "jane" is covered with the
 // match; "to" and "before" stay readable.
 void marksCoverAWordJoinedByADroppedDot() {
  QImage dark(260,80,QImage::Format_RGB32);dark.fill(qRgb(30,30,30));
  const auto ink=[&](int x0,int x1,int y0=50,int y1=59){for(int x=x0;x<=x1;++x)for(int y=y0;y<=y1;++y)dark.setPixel(x,y,qRgb(220,220,220));};
  ink(95,103);
  // The dot (125-126, its two bottom rows) touches the "e" and the "d" only at
  // their corners.
  ink(106,107);ink(109,112);ink(114,117);ink(119,124,50,57);
  ink(125,126,58,59);
  ink(127,131,50,57);for(int x=133;x<216;x+=6)ink(x,x+4);
  ink(219,245);
  const QList<RecognizedWord> row{{"to",QRect(95,50,9,10),90,0},{"jane",QRect(106,50,19,10),90,0},{"doe@example.com",QRect(127,50,89,10),90,0},{"before",QRect(219,50,27,10),90,0}};
  const auto boxes=quickRedact::markBoxes(row,{"email",{2}},dark.rect(),dark);
  QCOMPARE(boxes.size(),1);
  const auto mark=boxes.first().mark;
  QVERIFY2(mark.left()<=106&&mark.left()>103,qPrintable(QString::number(mark.left())));
  QVERIFY2(mark.right()>=215&&mark.right()<219,qPrintable(QString::number(mark.right())));
  // What a deleted suggestion is remembered by stays the matched word's box.
  QCOMPARE(boxes.first().words,row.at(2).rect);
  // Without the dot's ink the gap is a word space: "jane" stays readable.
  for(int y=58;y<=59;++y)for(int x=125;x<=126;++x)dark.setPixel(x,y,qRgb(30,30,30));
  QVERIFY(quickRedact::markBoxes(row,{"email",{2}},dark.rect(),dark).first().mark.left()>124);
  // The same line on a white panel, as this fix's desktop fixture drew it: the
  // dot's anti-aliased ink is about a third of the text's contrast.
  QImage light(260,80,QImage::Format_RGB32);light.fill(Qt::white);
  for(int y=0;y<light.height();++y)for(int x=0;x<light.width();++x)if(x!=125&&x!=126&&qRed(dark.pixel(x,y))>100)light.setPixel(x,y,qRgb(0,0,0));
  for(int y=58;y<=59;++y)for(int x=125;x<=126;++x)light.setPixel(x,y,qRgb(170,170,170));
  const auto leftOf=[&](const QImage& image){return quickRedact::markBoxes(row,{"email",{2}},image.rect(),image).first().mark.left();};
  QVERIFY2(leftOf(light)<=106,qPrintable(QString::number(leftOf(light))));
  // Sol round 2 on #1864: a solid dot whose faint corner pixel (125,57) touches
  // the "e" at a quarter of the contrast is still a dot at half.
  for(int y=58;y<=59;++y)for(int x=125;x<=126;++x)light.setPixel(x,y,qRgb(0,0,0));
  light.setPixel(125,57,qRgb(170,170,170));
  QVERIFY2(leftOf(light)<=106,qPrintable(QString::number(leftOf(light))));
  // Sol round 3 on #1864: an ordinary 1 px word space after "More", with a lone dark pixel of its
  // "e" (105) that a fainter one (104) ties back to the letter. That pixel is no dropped dot, so
  // "More" is not joined; the margin may still take its last letter, as reach() does on nightly.
  QImage more(260,80,QImage::Format_RGB32);more.fill(Qt::white);
  for(int x=80;x<=103;++x)for(int y=50;y<=59;++y)if(x%5!=4)more.setPixel(x,y,qRgb(0,0,0));
  more.setPixel(104,55,qRgb(180,180,180));more.setPixel(105,55,qRgb(100,100,100));
  for(int x=107;x<200;x+=6)for(int i=0;i<5;++i)for(int y=50;y<=59;++y)more.setPixel(x+i,y,qRgb(0,0,0));
  const QList<RecognizedWord> plain{{"More",QRect(80,50,24,10),90,0},{"jane@example.com",QRect(107,50,96,10),90,0}};
  const auto kept=quickRedact::markBoxes(plain,{"email",{1}},more.rect(),more).first().mark.left();
  QVERIFY2(kept>90,qPrintable(QString::number(kept)));
  // Without the picture nothing is joined.
  QVERIFY(quickRedact::markBoxes(row,{"email",{2}},light.rect()).first().mark.left()>124);
 }
 // CodeRabbit 4613753565: the first initial is a whole character, even outside the BMP.
 void anInitialOutsideTheBmp() {
  const auto candidates=words(QStringLiteral("\U00010400Stone wrote"));
  QCOMPARE(found(candidates,quickRedact::find(candidates,{"name"},account("zz",QStringLiteral("\U00010400\U00010401 Stone")))),QStringList{QStringLiteral("name: \U00010400Stone")});
 }
 // CodeRabbit 4613753585: patterns see the recognized text as shown, the exact
 // regex as typed: case, accents and full-width digits are not folded for them.
 void patternsSeeTheTextAsShown() {
  const auto candidates=words(QStringLiteral("Café | cafe | abc-1 | XYZ-2 | １２３"));
  const auto result=quickRedact::find(candidates,{"list"},patterns({QStringLiteral("^Café$"),R"([A-Z]{3}-\d+)",QStringLiteral("^１２３$")}));
  QCOMPARE(result.skippedPatterns,QList<int>{});
  QCOMPARE(found(candidates,result),(QStringList{QStringLiteral("list: Café"),"list: XYZ-2",QStringLiteral("list: １２３")}));
 }
 // CodeRabbit 4613753575: each subject is made valid once; a lone surrogate in
 // it does not stop the pattern.
 void aLoneSurrogateDoesNotStopAPattern() {
  QList<quickRedact::PatternOutcome> out;
  quickRedact::matchPatterns({QString(QChar(0xd800))+" JIRA-1"},{R"(JIRA-\d)"},[&](quickRedact::PatternOutcome outcome){out.append(outcome);});
  QCOMPARE(out.size(),1);QVERIFY(out.first().finished);QCOMPARE(out.first().hits,(QList<quickRedact::PatternHit>{{0,2,6}}));
 }
 // A pattern that does not compile is named as skipped, never silently empty.
 void aPatternThatDoesNotCompileIsSkipped() {
  const auto candidates=words("JIRA-1 TICKET2");
  const auto result=quickRedact::find(candidates,{"list"},patterns({"(abc",R"(TICKET\d)"}));
  QCOMPARE(found(candidates,result),QStringList{"list: TICKET2"});QCOMPARE(result.skippedPatterns,QList<int>{0});
 }
 // The matcher hook: find sends the subjects and patterns and claims the
 // ranges it gets back; a pattern it does not report is skipped.
 void aMatcherRunsThePatterns() {
  const auto candidates=words("Hammond filed JIRA-9 | and TICKET4");
  QStringList seenSubjects,seenPatterns;
  const quickRedact::PatternMatcher matcher=[&](const QStringList& subjects,const QList<qsizetype>& breaks,const QStringList& list){
   seenSubjects=subjects;seenPatterns=list;
   QList<quickRedact::PatternOutcome> out;
   quickRedact::matchPatterns(subjects,list,[&](quickRedact::PatternOutcome outcome){if(outcome.pattern==0)out.append(outcome);},breaks);
   return out;
  };
  const auto result=quickRedact::find(candidates,{"list"},quickRedact::Sources{{},{},{"Hammond"},{R"(JIRA-\d)",R"(TICKET\d)"}},2000,{},nullptr,matcher);
  QCOMPARE(seenSubjects,(QStringList{"Hammond filed JIRA-9","and TICKET4","Hammond filed JIRA-9\nand TICKET4"}));QCOMPARE(seenPatterns.size(),2);
  QCOMPARE(found(candidates,result),(QStringList{"list: Hammond","list: JIRA-9"}));QCOMPARE(result.skippedPatterns,QList<int>{1});
  // No patterns: the matcher is never asked.
  bool asked=false;
  quickRedact::find(candidates,{"list","name"},quickRedact::Sources{"someone","",{"Hammond"},{}},2000,{},nullptr,[&](const QStringList&,const QList<qsizetype>&,const QStringList&){asked=true;return QList<quickRedact::PatternOutcome>{};});
  QVERIFY(!asked);
 }
 // M10: a long entry whose last part misses costs one pass, not one for each
 // way of joining its parts.
 void longNearMissIsFast() {
  QStringList parts;for(char c='a';c<'a'+20;++c)parts.append(QString(QChar(c)));
  const auto entry=parts.join(' ');
  QStringList near=parts.mid(0,19);near.append("x");
  QStringList page;for(int i=0;i<50;++i)page.append(near.join(' '));
  const auto candidates=words(page.join(' '));
  QElapsedTimer clock;clock.start();
  const auto result=quickRedact::find(candidates,{"list"},terms({entry}));
  QVERIFY2(clock.elapsed()<putgd::test::scaled(200),qPrintable(QString::number(clock.elapsed())));
  QVERIFY(result.matches.isEmpty());
 }
 // #1897: lines built to make the key rules work hard stay fast with every kind asked for: long runs of
 // token heads, assignments, URL schemes and token-shaped pieces, as one word and as many.
 void pathologicalLinesStayFast_data() {
  QTest::addColumn<QString>("page");
  const auto repeat=[](const QString& unit,int count){QString out;for(int i=0;i<count;++i)out+=unit;return out;};
  QTest::newRow("JWT heads in one word")<<repeat("eyJ.",4000);
  QTest::newRow("token heads in one word")<<repeat(QString("gh")+"p_",3000)+repeat("sk-",3000)+repeat("AK"+QString("IA"),3000);
  QTest::newRow("token heads as words")<<repeat(QString("gh")+"p_a1B2 ",3000);
  QTest::newRow("assignments in one word")<<repeat(",TOKEN=a1B2c3D4",2000);
  QTest::newRow("URL schemes in one word")<<repeat("postgres://u:",2000);
  QTest::newRow("a long base64 word")<<repeat("aB3dE5fG7hJ9kL1mN3pQ+/",1000);
  QTest::newRow("many token-shaped words")<<repeat("aB3dE5fG7h ",4000);
  QTest::newRow("many token-shaped lines")<<repeat("aB3dE5fG7hJ9kL1mN3pQ5rS7tU9vW1xY3 | ",300);
  QTest::newRow("a PEM begin and many lines")<<QString("-----BEGIN RSA PRIV")+"ATE KEY----- | "+repeat("aB3dE5fG7hJ9kL1mN3pQ5rS7tU9vW1xY3 | ",400);
 }
 // Sol on #1902: the faster search finds what the plain one did.
 void theFastSearchFindsWhatThePlainOneDid() {
  // A hex run whose digits are partly recognition's confusions (| and O read for 1 and 0).
  const auto confused=QString("abcdef||Olabcdef")+"abcdef||Olabcdef";
  QCOMPARE(found(words("value "+confused),QStringList{"key"}),QStringList{"key: value "+confused});
  // A token wrapped over three lines: the third line is covered, though the first token's wrap budget ends before it.
  const auto stripe=QString("sk")+"_li"+"ve_"+"4eC39HqLyjWDarjtT1zdp7dcQ2",hex=QString("9f86d081884c7d659a2feaa0c55ad015");
  QStringList covered;for(const auto& m:found(words(stripe+" | "+hex+" | x7Kq2Lm9Vb4N"),QStringList{"key"}))covered.append(m);
  QVERIFY2(covered.join(' ').contains("x7Kq2Lm9Vb4N"),qPrintable(covered.join(" / ")));
  // Every line starts its own chain: the second line's run reaches a tail the first line's could not (Sol on #1902).
  {const auto page=QString("9f86d081884c7d659a2feaa0c55ad015 | 6311d6de7be2f57b64c74d411a0b379352 | x7Kq2Lm9Vb4N | ")+"z7Kq2Lm9Vb4Nqr7mvs1fg2py6nwj3zr4t51";
   QStringList all;for(const auto& m:found(words(page),QStringList{"key"}))all.append(m);
   QVERIFY2(all.join(' ').contains("z7Kq2Lm9Vb4Nqr7mvs1fg2py6nwj3zr4t51"),qPrintable(all.join(" / ")));}
 }
 void pathologicalLinesStayFast() {
  QFETCH(QString,page);
  const auto candidates=words(page);
  QElapsedTimer clock;clock.start();
  const auto result=quickRedact::find(candidates,{"email","phone","ip","card","key"});
  qInfo("%s: %lld ms, %lld words, %lld matches",QTest::currentDataTag(),clock.elapsed(),qint64(candidates.size()),qint64(result.size()));
  QVERIFY2(clock.elapsed()<putgd::test::scaled(1000),qPrintable(QString::number(clock.elapsed())));
 }
 // M2: progress after the terms and after the patterns, the rest skipped, so a
 // caller that stops waiting keeps what was found.
 void progressCarriesFinishedPatterns() {
  const auto candidates=words("Hammond filed JIRA-9 and TICKET4");
  QList<quickRedact::Result> seen;
  const auto result=quickRedact::find(candidates,{"list"},quickRedact::Sources{{},{},{"Hammond"},{R"(JIRA-\d)",R"(TICKET\d)"}},2000,[&](const quickRedact::Result& partial){seen.append(partial);});
  QCOMPARE(seen.size(),2);
  QCOMPARE(found(candidates,seen.at(0)),QStringList{"list: Hammond"});QCOMPARE(seen.at(0).skippedPatterns,(QList<int>{0,1}));
  QCOMPARE(seen.at(1).matches,result.matches);QCOMPARE(seen.at(1).skippedPatterns,QList<int>{});
 }
 void noBudgetSkipsEveryPattern() {
  const auto candidates=words("Hammond filed JIRA-9");
  const auto result=quickRedact::find(candidates,{"list"},quickRedact::Sources{{},{},{"Hammond"},{R"(JIRA-\d)","x"}},0);
  QCOMPARE(found(candidates,result),QStringList{"list: Hammond"});
  QCOMPARE(result.skippedPatterns,(QList<int>{0,1}));
 }
 // #1840, as putgd-ocr read "Please write to jane.doe@example.com before
 // Friday" and "Contact us. bob@example.com today" in Red Hat Display at 10 px:
 // the address split after its dot, or with the dot lost, into two words whose
 // boxes overlap. The match takes both words and the cover reaches over the
 // whole address's ink (x 72 to 176); "us.", a word space before its address,
 // stays out.
 void emailSplitInItsLocalPartIsCoveredWhole_data() {
  QTest::addColumn<QString>("first");QTest::addColumn<QRect>("box");
  QTest::newRow("split at the dot")<<"jane."<<QRect(73,50,22,14);
  QTest::newRow("dot lost")<<"jane"<<QRect(73,50,20,14);
 }
 void emailSplitInItsLocalPartIsCoveredWhole() {
  QFETCH(QString,first);QFETCH(QRect,box);
  const QList<RecognizedWord> page{{"Please",QRect(6,52,29,9),90,0},{"write",QRect(37,53,22,8),90,0},{"to",QRect(61,50,9,14),90,0},{first,box,90,0},
   {"doe@example.com",QRect(91,52,86,10),90,0},{"before",QRect(179,52,30,9),90,0},{"Friday",QRect(211,52,27,10),90,0},
   {"Contact",QRect(6,96,36,9),90,1},{"us.",QRect(44,98,12,7),90,1},{"bob@example.com",QRect(58,96,85,10),90,1}};
  QCOMPARE(found(page,QStringList{"email"}),(QStringList{"email: "+first+" doe@example.com","email: bob@example.com"}));
  const auto matches=quickRedact::find(page,{"email"});
  QRect covered;for(const auto& mark:quickRedact::marks(page,matches.first(),QRect(0,0,260,140)))covered=covered.united(mark);
  QVERIFY(covered.left()<=72&&covered.right()>=176);
  // Split twice, each pair of boxes overlapping by a pixel; "Mail" only touches.
  const QList<RecognizedWord> twice{{"Mail",QRect(0,0,40,20),90,0},{"jane.",QRect(40,0,40,20),90,0},{"q.",QRect(79,0,16,20),90,0},{"doe@example.com",QRect(94,0,120,20),90,0}};
  QCOMPARE(found(twice,QStringList{"email"}),QStringList{"email: jane. q. doe@example.com"});
  // Sol on #1840: split after a plus; a fragment the address can't start with stays out.
  const QList<RecognizedWord> plus{{"jane+",QRect(0,0,40,20),90,0},{"tag@example.com",QRect(39,0,120,20),90,0}};
  QCOMPARE(found(plus,QStringList{"email"}),QStringList{"email: jane+ tag@example.com"});
  // Sol on #1840: an address ending a sentence before a split one keeps them apart.
  const QList<RecognizedWord> after{{"alice@example.net.",QRect(0,0,140,20),90,0},{"jane+",QRect(148,0,40,20),90,0},{"tag@example.com",QRect(187,0,120,20),90,0}};
  QCOMPARE(found(after,QStringList{"email"}),(QStringList{"email: alice@example.net.","email: jane+ tag@example.com"}));
  // ...while a dot recognition lost after a domain is still restored before one.
  const QList<RecognizedWord> lost{{"alice@example",QRect(0,0,104,20),90,0},{"com",QRect(112,0,24,20),90,0},{"bob@example.org",QRect(135,0,120,20),90,0}};
  QCOMPARE(found(lost,QStringList{"email"}),(QStringList{"email: alice@example com","email: bob@example.org"}));
  const QList<RecognizedWord> handle{{"@support.",QRect(0,0,72,20),90,0},{"jane+",QRect(80,0,40,20),90,0},{"tag@example.com",QRect(119,0,120,20),90,0}};
  QCOMPARE(found(handle,QStringList{"email"}),QStringList{"email: @support. jane+ tag@example.com"});
  const QList<RecognizedWord> dash{{"-to",QRect(0,0,24,20),90,0},{"bob@example.com",QRect(23,0,120,20),90,0}};
  QCOMPARE(found(dash,QStringList{"email"}),QStringList{"email: bob@example.com"});
  // A word another kind matches stays with that kind: the card keeps its last group.
  const QList<RecognizedWord> card{{"4111",QRect(0,0,32,20),90,0},{"1111",QRect(40,0,32,20),90,0},{"1111",QRect(80,0,32,20),90,0},{"1111",QRect(120,0,32,20),90,0},{"doe@example.com",QRect(150,0,120,20),90,0}};
  QCOMPARE(found(card,QStringList{"email","card"}),(QStringList{"card: 4111 1111 1111 1111","email: doe@example.com"}));
 }
 // The five kinds are unchanged with the new sources beside them.
 void builtInKindsIgnoreTheSources() {
  const auto candidates=words("Write to jane.doe@example.co.uk or call +44 20 7946 0958 | Patrick Hudson");
  const auto result=quickRedact::find(candidates,all,quickRedact::Sources{"phudson","Patrick Hudson",{"Patrick Hudson"},{"Patrick"}});
  QCOMPARE(result.matches,quickRedact::find(candidates,all));
 }
};
QTEST_APPLESS_MAIN(QuickRedactTest)
#include "test_quick_redact.moc"
