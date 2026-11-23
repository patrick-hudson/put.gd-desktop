// Quick redact (#1428, decision 5): one table over the matching function. A
// row is recognized words in reading order ("|" starts a new line), the
// categories asked for, and every match expected as "category: words".
#include "editor/QuickRedact.h"
#include "fixtures/Waits.h"
#include <QCryptographicHash>
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
// #1940: hex and base64 that no scanner reads as a secret in source, made at run time from a seed.
QString hexOf(const char* seed,int length) {
 QByteArray out;for(int i=0;out.size()<length;++i)out+=QCryptographicHash::hash(QByteArray(seed)+QByteArray::number(i),QCryptographicHash::Sha256).toHex();
 return QString::fromLatin1(out.left(length));
}
QString base64Of(const char* seed,int length) {
 QByteArray out;for(int i=0;out.size()<length;++i)out+=QCryptographicHash::hash(QByteArray(seed)+QByteArray::number(i),QCryptographicHash::Sha256).toBase64(QByteArray::OmitTrailingEquals);
 return QString::fromLatin1(out.left(length));
}
quickRedact::Sources account(const QString& login,const QString& realName) {quickRedact::Sources s;s.login=login;s.realName=realName;return s;}
quickRedact::Sources terms(const QStringList& list) {quickRedact::Sources s;s.terms=list;return s;}
quickRedact::Sources patterns(const QStringList& list) {quickRedact::Sources s;s.patterns=list;return s;}
}
Q_DECLARE_METATYPE(putgd::editor::quickRedact::Sources)
class QuickRedactTest final:public QObject {
 Q_OBJECT
private slots:
 // #1911: the second read of an uncertain token's line is matched apart from
 // the first. Side by side on one row the two readings would be one line:
 // SHORT_SHA=... read twice became a key. A token only the second read got
 // right is still found, and its marks are sized against its own read's words.
 void secondReadIsMatchedApart() {
  const auto twice=[](const QString& a,const QString& b,bool mark){
   QList<RecognizedWord> out{{a,QRect(0,0,int(a.size())*8,20),40,0}};
   out.append({b,QRect(1,1,int(b.size())*8,20),70,0,mark});
   return out;
  };
  const QString sha=QStringLiteral("SHORT_SHA=")+QStringLiteral("2f6424a");
  QCOMPARE(found(twice(sha,sha,false),all),QStringList{"key: "+sha+" "+sha});
  QCOMPARE(found(twice(sha,sha,true),all),QStringList{});
  const QString token=QStringLiteral("gh")+QStringLiteral("p_")+QStringLiteral("a1B2c3D4e5").repeated(3)+QStringLiteral("F6g7H8");
  const QString misread=QStringLiteral("gnp_")+QStringLiteral("a1B2c3D4e5").repeated(2);
  QCOMPARE(found(twice(misread,misread,true),all),QStringList{});
  const auto both=twice(misread,token,true);
  const auto matches=quickRedact::find(both,all);
  QCOMPARE(matches.size(),qsizetype(1));QCOMPARE(matches.first().words,QList<int>{1});
  QCOMPARE(matches.first().category,QStringLiteral("key"));
  // The second read is matched among the first read's other lines: a line of
  // a public key read again stays part of its PEM block, not a key.
  {const QString body=QStringLiteral("MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAu1SU1LfVLPHCozMxH2Mo");
   QList<RecognizedWord> pem{{QStringLiteral("-----BEGIN"),QRect(0,0,80,20),90,0},{QStringLiteral("PUBLIC"),QRect(88,0,48,20),90,0},{QStringLiteral("KEY-----"),QRect(144,0,64,20),90,0},
                             {body.left(40)+QStringLiteral("§"),QRect(0,30,520,20),35,1},{QStringLiteral("-----END"),QRect(0,60,64,20),90,2},{QStringLiteral("PUBLIC"),QRect(72,60,48,20),90,2},{QStringLiteral("KEY-----"),QRect(128,60,64,20),90,2}};
   QCOMPARE(found(pem,all),QStringList{});
   pem.append({body,QRect(1,31,520,20),80,0,true});
   QCOMPARE(found(pem,all),QStringList{});}
  // Both reads right about the same token: one finding, not two (Sol on #1920).
  {const auto same=twice(token,token,true);
   QCOMPARE(found(same,all),QStringList{"key: "+token});}
  // A first-read match that wraps from the end of one row to the start of the
  // next spans a box over words it does not cover; a second-read match there
  // is its own finding (Sol on #1920).
  {auto wrapped=words(QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaa 123456 | 654321"));
   wrapped.append({QStringLiteral("111111"),QRect(60,0,48,20),70,0,true});
   wrapped.append({QStringLiteral("222222"),QRect(116,0,48,20),70,0,true});
   const auto result=quickRedact::find(wrapped,{"list"},patterns({R"(\d{6}\s\d{6})"}));
   QCOMPARE(found(wrapped,result),(QStringList{"list: 123456 654321","list: 111111 222222"}));}
  // A second read longer than a clipped first read reaches past its cover: both stay.
  {QList<RecognizedWord> clipped{{QStringLiteral("JIRA-123456789012"),QRect(0,0,136,20),40,0},{QStringLiteral("JIRA-123456789012345"),QRect(0,0,160,20),70,0,true}};
   QCOMPARE(found(clipped,quickRedact::find(clipped,{"list"},patterns({R"(JIRA-\d+)"}))),(QStringList{"list: JIRA-123456789012","list: JIRA-123456789012345"}));}
  // The first read's overlapping word is no neighbour of the second read's match.
  QImage image(400,40,QImage::Format_RGB32);image.fill(Qt::white);
  const auto boxes=quickRedact::markBoxes(both,matches.first(),image.rect(),image);
  QCOMPARE(boxes.size(),qsizetype(1));QVERIFY(boxes.first().mark.contains(both.at(1).rect));
 }
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
  // #1939 (#1931 fuzz group 2): a card beside another number on its row is still found.
  QTest::newRow("a card after a reference number")<<"Reference 12 4111 1111 1111 1111"<<QStringList{"card"}<<QStringList{"card: 4111 1111 1111 1111"};
  QTest::newRow("a card before its expiry and CVC")<<"4242 4242 4242 4242 12 / 34 123"<<QStringList{"card"}<<QStringList{"card: 4242 4242 4242 4242"};
  QTest::newRow("a card after an editor line number")<<"17 4111 1111 1111 1111"<<QStringList{"card"}<<QStringList{"card: 4111 1111 1111 1111"};
  QTest::newRow("a card after two short numbers")<<"12 34 4111 1111 1111 1111"<<QStringList{"card"}<<QStringList{"card: 4111 1111 1111 1111"};
  QTest::newRow("a card after a phone number")<<"Ada 202-555-0143 5555 5555 5555 4444"<<all<<QStringList{"phone: 202-555-0143","card: 5555 5555 5555 4444"};
  QTest::newRow("a tracking number holds no card")<<"Tracking 5744 7621 6827 5217 60 and 9400 1527 3806 7569 1933 50"<<QStringList{"card"}<<QStringList{};
  QTest::newRow("a tracking number after an order number holds no card")<<"13 43358981 5762 9815 7175 3077 6995 30"<<QStringList{"card"}<<QStringList{};
  QTest::newRow("an EAN after a date holds no card")<<"2026-11-22 1043533606843 12"<<QStringList{"card"}<<QStringList{};
  // A table's full rows read as one wrapped block: each row's card is found, without the next row's number.
  QTest::newRow("cards that end the rows of a table")<<"18 Sam +82 10-8045-8908 5294 2317 5284 0376 | 19 Ann +82 10-8045-8909 5219 7028 4410 6663 | 20 Bob +82 10-8045-8910 4444 6185 9101 3565"<<QStringList{"card"}
   <<QStringList{"card: 5294 2317 5284 0376","card: 5219 7028 4410 6663","card: 4444 6185 9101 3565"};
  QTest::newRow("a card after a signed amount")<<"Refund -123 38520000023237"<<QStringList{"card"}<<QStringList{"card: 38520000023237"};
  QTest::newRow("a diff's card with a three-digit first group")<<"-411 11111 1111 1111"<<QStringList{"card"}<<QStringList{"card: -411 11111 1111 1111"};
  // Sol on #1957: a phone between a CSV-glued address and a key keeps its middle.
  QTest::newRow("a phone between an address and a key")<<"alice@example.com,+1 202 555 0143(postgres://app:"+QString("12@34")+"@db.test/app)"<<QStringList{"email","phone","key"}
   <<QStringList{"email: alice@example.com,+1","phone: 202 555","key: 0143(postgres://app:"+QString("12@34")+"@db.test/app)"};
  QTest::newRow("cards in a diff")<<"-5493-9997-9808-2174 | +4111 1111 1111 1111 | {+5555-5555-5555-4444+}"<<QStringList{"card"}<<QStringList{"card: -5493-9997-9808-2174","card: +4111 1111 1111 1111","card: {+5555-5555-5555-4444+}"};
  QTest::newRow("a dashed card right after a phone")<<"+49 (0)30 6729 7202 5308-0894-0241-1905 Lyon"<<QStringList{"card"}<<QStringList{"card: 5308-0894-0241-1905"};
  QTest::newRow("a card right after a phone ending in four digits")<<"+39 380 719 1173 6011 9949 4130 7983 Porto"<<QStringList{"card"}<<QStringList{"card: 6011 9949 4130 7983"};
  QTest::newRow("a number after a dash is no card")<<"ID 12-4111111111111111"<<QStringList{"card"}<<QStringList{};
  // Sol on #1957: what the old rule covered stays covered.
  QTest::newRow("a card with a digit moved between its groups")<<"4111 111 11111 1111 | 4111 11 111111 1111 | 411 11111 1111 1111"<<QStringList{"card"}<<QStringList{"card: 4111 111 11111 1111","card: 4111 11 111111 1111","card: 411 11111 1111 1111"};
  QTest::newRow("a card read with its last digits split off")<<"25285945833551 94"<<QStringList{"card"}<<QStringList{"card: 25285945833551 94"};
  QTest::newRow("a card read with its groups joined, in either order")<<"4111 111111111111 | 411111111111 1111"<<QStringList{"card"}<<QStringList{"card: 4111 111111111111","card: 411111111111 1111"};
  QTest::newRow("an EAN before a date is no card")<<"6253096961579 2026-12-23"<<QStringList{"card"}<<QStringList{};
  QTest::newRow("an order number after a spaced colon is no phone")<<"Order : #10234567"<<QStringList{"phone"}<<QStringList{};
  QTest::newRow("a phone after a spaced colon and a hash is a phone")<<"Phone : #2125550100"<<QStringList{"phone"}<<QStringList{"phone: #2125550100"};
  QTest::newRow("a receipt number after a time is no phone")<<"13/11/2026 19:54:46 #4091291"<<all<<QStringList{};
  QTest::newRow("an account number that is a phone is a phone")<<"Account #2125550100"<<QStringList{"phone"}<<QStringList{"phone: #2125550100"};
  QTest::newRow("an Australian mobile before a card")<<"0412 007 273 4344519822181521"<<all<<QStringList{"phone: 0412 007 273","card: 4344519822181521"};
  QTest::newRow("a card after a long number and before a year")<<"12345678901234567 4111 1111 1111 1111 2026"<<QStringList{"card"}<<QStringList{"card: 4111 1111 1111 1111"};
  QTest::newRow("a phone before a month is a phone")<<"Call 202-555-0143 Nov 22"<<QStringList{"phone"}<<QStringList{"phone: 202-555-0143"};
  QTest::newRow("a phone after a slash is a phone")<<"Phone/202-555-0143"<<QStringList{"phone"}<<QStringList{"phone: Phone/202-555-0143"};
  QTest::newRow("a phone after a hash is a phone")<<"Phone #202-555-0143 Telefon #2125550100"<<QStringList{"phone"}<<QStringList{"phone: #202-555-0143","phone: #2125550100"};
  QTest::newRow("a phone after Total: or a product's slash is a phone")<<"Total: 2125550100 | Mobile/2025550143"<<QStringList{"phone"}<<QStringList{"phone: 2125550100","phone: Mobile/2025550143"};
  QTest::newRow("a labelled card keeps the earlier rule")<<"Card: 4110 4111 4112 4113"<<QStringList{"card"}<<QStringList{"card: 4110 4111 4112 4113"};
  QTest::newRow("a full-width card label counts too")<<QString("\uFF23\uFF41\uFF52\uFF44: 4110 4111 4112 4113")<<QStringList{"card"}<<QStringList{"card: 4110 4111 4112 4113"};
  QTest::newRow("an IPv6 address in two-digit groups with a port")<<"[02:1a:2b:ff:fe:3c:4d:5e]:443"<<QStringList{"ip"}<<QStringList{"ip: [02:1a:2b:ff:fe:3c:4d:5e]:443"};
  QTest::newRow("an address split after a dot")<<"LISTEN 148.98.250. 123:443"<<QStringList{"ip"}<<QStringList{"ip: 148.98.250. 123:443"};
  QTest::newRow("a phone after Total: is a phone")<<"Total: +1 202 555 0143"<<QStringList{"phone"}<<QStringList{"phone: +1 202 555 0143"};
  QTest::newRow("an address and a stray dotted number")<<"IP 192.0.2.1 .5"<<QStringList{"ip"}<<QStringList{"ip: 192.0.2.1"};
  QTest::newRow("an IPv6 address with an IPv4 tail")<<"Host 2001:db8:0:0:0:0:192.0.2.1"<<QStringList{"ip"}<<QStringList{"ip: 2001:db8:0:0:0:0:192.0.2.1"};
  QTest::newRow("loopback in eight two-digit groups")<<"IPv6 [00:00:00:00:00:00:00:01]"<<QStringList{"ip"}<<QStringList{"ip: [00:00:00:00:00:00:00:01]"};
  QTest::newRow("two cards in a row")<<"4111 1111 1111 1111 5555 5555 5555 4444"<<QStringList{"card"}<<QStringList{"card: 4111 1111 1111 1111","card: 5555 5555 5555 4444"};
  // #1939 (groups 5 and 12): counted rows, dates and counts are no cards or phones.
  QTest::newRow("a calendar week is no card or phone")<<"19 20 21 22 23 24 25"<<all<<QStringList{};
  QTest::newRow("a calendar week after its week number")<<"47 23 24 25 26 27 28 29"<<all<<QStringList{};
  QTest::newRow("a month's last days and the next grid's first are no phone")<<"16 17 18 17 and 23 24 25 24"<<all<<QStringList{};
  QTest::newRow("a partial week after its week number is no phone")<<"48 28 29 30"<<all<<QStringList{};
  QTest::newRow("calendar rows read as wrapped are no phone")<<"12 13 14 15 16 17 18 | 17 18 19 20 21 22 23 | 24 25 26 27 28 29 30"<<all<<QStringList{};
  QTest::newRow("a phone in pairs is a phone")<<"Tel 06 12 34 56 78 | Tel 06 12 13 14 78 | Tel +33 6 12 13 14 78 | Tel 06 42 43 44 36 | Tel 06 12 13 14 08"<<all<<QStringList{"phone: 06 12 34 56 78","phone: 06 12 13 14 78","phone: +33 6 12 13 14 78","phone: 06 42 43 44 36","phone: 06 12 13 14 08"};
  QTest::newRow("year headers are no card or phone")<<"2023 2024 2025 2026"<<all<<QStringList{};
  QTest::newRow("axis ticks are no phone")<<"0 20 40 60 80 100"<<all<<QStringList{};
  QTest::newRow("all zeros is no card")<<"0000 0000 0000 0000"<<QStringList{"card"}<<QStringList{};
  QTest::newRow("a screenshot name is no card or phone")<<"Screenshot From 2026-11-22 14-03-57.png"<<all<<QStringList{};
  QTest::newRow("a browser build date is no phone")<<"Mozilla/5.0 (X11; Linux x86_64; rv:120.0) Gecko/20100101 Firefox/120.0"<<all<<QStringList{};
  QTest::newRow("an order number is no phone")<<"Order #10234567 shipped"<<all<<QStringList{};
  // CodeRabbit on #1957: an order number with its "#" read apart; phones an exemption must not take.
  QTest::newRow("an order number after a lone hash is no phone")<<"Order # 10234567 shipped | Order \uFF03 10234568 shipped"<<QStringList{"phone"}<<QStringList{};
  QTest::newRow("a diff's added year row is no phone")<<"+2023 2024 2025"<<QStringList{"phone"}<<QStringList{};
  QTest::newRow("an international phone with counted groups is a phone")<<"Call +44 20 21 22 23 24"<<QStringList{"phone"}<<QStringList{"phone: +44 20 21 22 23 24"};
  QTest::newRow("a phone with its area code in parentheses is a phone")<<"Tel. (030) 123 234"<<QStringList{"phone"}<<QStringList{"phone: (030) 123 234"};
  QTest::newRow("a wrapped card after a row number")<<"18 4111 1111 | 1111 1111 xx | xxxxxxxxxxxx"<<QStringList{"card"}<<QStringList{"card: 4111 1111 1111 1111"};
  QTest::newRow("a file size in ls -l is no phone")<<"-rw-r--r-- 1 alice alice 12345678 Nov 22 12:00 big.iso"<<all<<QStringList{};
  QTest::newRow("a phone in a pane beside a free row is a phone")<<"Swap: 5953869 Telefon: +49 151 2083 0585"<<QStringList{"phone"}<<QStringList{"phone: 5953869","phone: +49 151 2083 0585"};
  QTest::newRow("a German phone with a slash after its area code")<<"Tel. +49 30 / 813854 or 030 / 813854"<<QStringList{"phone"}<<QStringList{"phone: +49 30 / 813854","phone: 030 / 813854"};
  QTest::newRow("an EAN under its bar code is no phone")<<"EAN 9 602850 531546"<<all<<QStringList{};
  QTest::newRow("colour channels are no phone")<<"rgb 147 19 65"<<all<<QStringList{};
  QTest::newRow("a count with spaced thousands is no phone")<<"Visitors 1 912 615"<<all<<QStringList{};
  QTest::newRow("a Spanish mobile in threes is a phone")<<"Movil 612 345 678"<<all<<QStringList{"phone: 612 345 678"};
  QTest::newRow("a tracking number holds no phone")<<"Track 0018 6608 9920 8904 6465 17"<<all<<QStringList{};
  QTest::newRow("an expiry and CVC are no phone")<<"Expires 04 / 27 CVC 123"<<all<<QStringList{};
  QTest::newRow("an amount before its currency is no phone")<<"Total 1 234 567 €"<<all<<QStringList{};
  QTest::newRow("an amount after its currency is no phone")<<"Total £1 234 567"<<all<<QStringList{};
  // #1939 (group 13): 11-digit mobiles written whole.
  QTest::newRow("a Chinese mobile written whole")<<"Mobile 13812345678"<<all<<QStringList{"phone: 13812345678"};
  QTest::newRow("a UK mobile written whole")<<"Mobile 07700900123"<<all<<QStringList{"phone: 07700900123"};
  QTest::newRow("11 digits that are no mobile stay no phone")<<"Order 98765432101"<<all<<QStringList{};
  // #1939 (group 14): a CSV word shared by two fields leaves neither half bare.
  QTest::newRow("a phone after an address in a CSV row")<<"alice@example.com,+1 202 555 0143,Springfield"<<QStringList{"email","phone"}<<QStringList{"email: alice@example.com,+1","phone: 202 555 0143,Springfield"};
  QTest::newRow("a card after an address in a CSV row")<<"bob@example.com,4111 1111 1111 1111"<<QStringList{"email","card"}<<QStringList{"email: bob@example.com,4111","card: 1111 1111 1111"};
  // #1939 (group 16): versions and section numbers shaped as addresses.
  QTest::newRow("a package version after an epoch is no address")<<"openssl 1:8.17.4.28-3.fc45"<<QStringList{"ip"}<<QStringList{};
  QTest::newRow("a package version before its release is no address")<<"libfoo 8.17.4.28-3.fc45"<<QStringList{"ip"}<<QStringList{};
  QTest::newRow("a section number is no address")<<"See Section 1.2.3.4 below"<<QStringList{"ip"}<<QStringList{};
  QTest::newRow("a version number is no address")<<"Version 2.11.7.54"<<QStringList{"ip"}<<QStringList{};
  QTest::newRow("an address range stays an address")<<"Pool 10.0.0.1-10.0.0.255"<<QStringList{"ip"}<<QStringList{"ip: 10.0.0.1-10.0.0.255"};
  QTest::newRow("an address split at a dot")<<"LISTEN 0 4096 148 .98.250.123:443 0.0.0.0:*"<<QStringList{"ip"}<<QStringList{"ip: 148 .98.250.123:443","ip: 0.0.0.0:*"};
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
   // Sol on #1957: a labelled card below a token stays a card line.
   QTest::newRow("a labelled card below a token is a card")<<"Bearer "+token+" | Visa 4110 4111 4112 4113"<<QStringList{"card","key"}<<QStringList{"key: "+token,"card: 4110 4111 4112 4113"};
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
  // #1942: any letter in a local part; no address in a systemd unit, an image name, a git remote,
  // a fediverse handle or a mail header's message ID; "|" read for the "l" before the "@";
  // an address recognition split at its "@" or before a domain dot.
  QTest::newRow("email with accented local parts")<<"josé_tanaka@tyrell.fr <zoë-jansen@oscorp.net>"<<all<<QStringList{"email: josé_tanaka@tyrell.fr","email: <zoë-jansen@oscorp.net>"};
  QTest::newRow("units, images, remotes and handles are no addresses")<<"user@1000.service getty@tty2.service Logo@2x.PNG icon@3x.png, git@github.com:org/repo.git follow @hana@mastodon.social"<<QStringList{"email"}<<QStringList{};
  // Sol on #1956: never a shorter domain inside one, and a path's first character may be a digit.
  QTest::newRow("no shorter domain inside a unit or a remote")<<"user@foo.bar.service git@source.example.com:org/repo.git git@github.com:1password/cli.git"<<QStringList{"email"}<<QStringList{};
  QTest::newRow("a real .target address and a word after a colon")<<"alice@mail.target alice@example.com:urgent"<<QStringList{"email"}<<QStringList{"email: alice@mail.target","email: alice@example.com:urgent"};
  QTest::newRow("a path unit and single-part remotes")<<"watch@home.path git@forge.example:repo.git git@forge.example:team+tools/repo.git"<<QStringList{"email"}<<QStringList{};
  QTest::newRow("an address ending a sentence before a number")<<"Mail jane@example.com. 2026 plans"<<QStringList{"email"}<<QStringList{"email: jane@example.com."};
  QTest::newRow("a header's colon read apart")<<"References : <CAF5x9q2@mail.gmail.com> | Message-ID: : <a1b2@mail.gmail.com>"<<QStringList{"email"}<<QStringList{};
  QTest::newRow("a domain label starting with a digit after a split dot")<<"jane@mx. 2fa.example.com"<<QStringList{"email"}<<QStringList{"email: jane@mx. 2fa.example.com"};
  QTest::newRow("a References word in prose")<<"References available from jane@example.com"<<QStringList{"email"}<<QStringList{"email: jane@example.com"};
  QTest::newRow("a message ID is no address")<<"Message-ID: <CAF5x9q2@mail.gmail.com> | In-Reply-To: <xyz@mail.example.com>"<<QStringList{"email"}<<QStringList{};
  QTest::newRow("an address on the next line of a header is one")<<"Message-ID: <CAF5x9q2@mail.gmail.com> | From: jane@example.com"<<QStringList{"email"}<<QStringList{"email: jane@example.com"};
  QTest::newRow("an address with a colon after it")<<"Mail jane@example.com: soon"<<QStringList{"email"}<<QStringList{"email: jane@example.com:"};
  QTest::newRow("an address before a port")<<"host jane@db.example.com:5432"<<QStringList{"email"}<<QStringList{"email: jane@db.example.com:5432"};
  QTest::newRow("l before the @ read as a bar")<<QString("Author: tanaka.|@mail.aperture.com jane+|@example.com jane\U00010400|@example.com")<<QStringList{"email"}<<QStringList{"email: tanaka.|@mail.aperture.com","email: jane+|@example.com",QString("email: jane\U00010400|@example.com")};
  QTest::newRow("an address split at its @ or a domain dot")<<"zoe.sato8 @wayne-corp.ai diego-reyes@ mail.fabrikam.com <kenji@gmail .com>"<<QStringList{"email"}<<QStringList{"email: zoe.sato8 @wayne-corp.ai","email: diego-reyes@ mail.fabrikam.com","email: <kenji@gmail .com>"};
  QTest::newRow("a split after punctuation or before a digit label")<<"<jane @example.com> jane+ @example.org jane@mx .2fa.example.com"<<QStringList{"email"}<<QStringList{"email: <jane @example.com>","email: jane+ @example.org","email: jane@mx .2fa.example.com"};
  QTest::newRow("a whole address takes no name after it")<<"alice@example.com .env"<<QStringList{"email"}<<QStringList{"email: alice@example.com"};
  QTest::newRow("a country code split from its domain")<<"alice@example.co .uk"<<QStringList{"email"}<<QStringList{"email: alice@example.co .uk"};
  // Sol on #1956: a key's next line read as "A91de @b3fa.aFe" still continues the key.
  {const QString hex=QStringLiteral("9f86d081884c7d65")+QStringLiteral("9a2feaa0c55ad015")+QStringLiteral("a3bf4f1b2b0b822c")+QStringLiteral("d15d6c15b0f0");
   QTest::newRow("a key's continuation is no split address")<<"secret: "+hex+" | A91de @b3fa.aFe"<<QStringList{"key"}<<QStringList{"key: secret: "+hex+" A91de @b3fa.aFe"};}
  // #1930: a chart legend's colour squares read as "@" beside amounts and model names are no addresses.
  QTest::newRow("legend swatches between amounts and names")<<"@ Opus 5.5 $8,520 @ Fable5.2 $5,535 @ Fable5.1 $590 @ Sonnet5.5 $325"<<all<<QStringList{};
  QTest::newRow("legend amount joined to the next swatch")<<"$5,535@Fable5.1 $590 535@Fable5.1 x@1.2 @ Sonnet5.5"<<all<<QStringList{};
  QTest::newRow("an address beside a legend still matches")<<"@ Opus 5.5 ops@example.com @ Fable5.2"<<all<<QStringList{"email: ops@example.com"};
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
  // #1965: a password may hold an @ itself; the last @ before the host ends it. An address and a path's @ are no password.
  {const auto at=QString("postgres://app:p@ss!w0")+"rd@db.internal:5432/app";
   QTest::newRow("a connection string whose password holds an @")<<at<<all<<QStringList{"key: "+at};
   const auto two=QString("DATABASE_URL=mysql://root:Kx9@mQ")+"2z@10.0.0.5/app";
   QTest::newRow("a connection string whose password starts with a short piece before an @")<<two<<all<<QStringList{"key: "+two};
   // ...judged on the password alone, in its own word, in one pass (Sol on #1967).
   const auto query=QString("https://app:Q7v")+"9r@db.test?display=XXXXXX&contact=a@b.test";
   QTest::newRow("a query's masked value does not hide the password")<<query<<QStringList{"key"}<<QStringList{"key: "+query};
   QString many=QString("https://app:Q7v")+"9r@db.test?q=";for(int i=0;i<16000;++i)many+=QStringLiteral("a@");
   QTest::newRow("a long query full of @ keeps the password covered")<<many<<QStringList{"key"}<<QStringList{"key: "+many};
   const auto lead=QString("postgres://app:@c9")+"v2@db.test/app";
   QTest::newRow("a password that starts with an @")<<lead<<QStringList{"key"}<<QStringList{"key: "+lead};
   QTest::newRow("a user with an empty password and its host")<<"postgres://app:@db.test/app"<<QStringList{"key"}<<QStringList{};
   QTest::newRow("an address later in the sentence makes no password")<<"See https://app:example@host for alice@example.com"<<QStringList{"key"}<<QStringList{};
   QTest::newRow("an address and a URL with an @ in its path are no password")<<"mail alice@example.com or see https://example.com:8443/a@b and ssh://git@github.com:22/x"<<QStringList{"key"}<<QStringList{};}
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
  QTest::newRow("a bracketed IPv6 address with O read for 0 and a five-digit port")<<"peer [2001:db8::ABOD]:54321"<<QStringList{"ip"}<<QStringList{"ip: [2001:db8::ABOD]:54321"};
  {const auto spaced=QString("{\"password\":\"N4!&v")+" red fox\"}";
   QTest::newRow("a quoted JSON password of three words")<<spaced<<QStringList{"key"}<<QStringList{"key: "+spaced};}
  {const auto entry=QString("[{\"password\":\"N4!&v")+" red fox\"},";
   QTest::newRow("a quoted JSON password ending an array entry")<<entry<<QStringList{"key"}<<QStringList{"key: "+entry};}
  QTest::newRow("a detached value before closing brackets")<<"\"password\": \"RedF"+QString("ox42\" }]")<<QStringList{"key"}<<QStringList{"key: \"RedF"+QString("ox42\"")};
  QTest::newRow("an IPv6 address after a user name")<<"ssh alice@2001:db8::1"<<QStringList{"ip"}<<QStringList{"ip: alice@2001:db8::1"};
  QTest::newRow("an IPv4 CIDR with a group read as @ or i")<<"nets 192.168.@.10/24 192.168.i.10/24"<<QStringList{"ip"}<<QStringList{"ip: 192.168.@.10/24","ip: 192.168.i.10/24"};
  {const auto spaced=QString("{\"password\":\"N4!&v")+" blue-lake\"}";
   QTest::newRow("a quoted JSON password with a space in it")<<spaced<<QStringList{"key"}<<QStringList{"key: "+spaced};}
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
  // #1941 (fuzz groups 1, 4, 8 and 10): secrets in forms the setting rule did not read. Built in pieces
  // at run time: the repository's secret scan reads every branch.
  {const auto pw=QString("Crane")+"7"+"Queue",strong=QString("Wel")+"come"+"2024!";const QStringList key{"key"};
   QTest::newRow("a MySQL password glued to -p")<<"$ mysql -u root -p"+pw+" -h db"<<key<<QStringList{"key: -p"+pw};
   QTest::newRow("a password flag with an equals sign")<<"$ mysqldump --user=app --password="+pw+" shop"<<key<<QStringList{"key: --password="+pw};
   QTest::newRow("a token flag and its value")<<"$ deploy --token "+pw+" --region eu"<<key<<QStringList{"key: "+pw};
   QTest::newRow("a password-stdin flag names no value")<<"$ docker login --password-stdin registry.example.com"<<key<<QStringList{};
   const auto user=QString("alice")+":"+pw,fetch=QString("cu")+"rl";
   QTest::newRow("curl's user and password")<<"$ "+fetch+" -u "+user+" https://api.example.com/v1/me"<<key<<QStringList{"key: "+user};
   QTest::newRow("docker's user and group are no password")<<"$ docker run -u 1000:1000 alpine"<<key<<QStringList{};
   QTest::newRow("redis-cli -a")<<"$ redis-cli -h cache -a "+pw+" ping"<<key<<QStringList{"key: "+pw};
   QTest::newRow("a registry login's -p")<<"$ docker login -u alice -p "+pw+" registry.example.com"<<key<<QStringList{"key: "+pw};
   QTest::newRow("sshpass -p")<<"$ sshpass -p "+pw+" ssh deploy@nas"<<key<<QStringList{"key: "+pw};
   QTest::newRow("a Java system property")<<"$ java -Dspring.datasource.password="+pw+" -jar app.jar"<<key<<QStringList{"key: -Dspring.datasource.password="+pw};
   QTest::newRow("nmcli's password word")<<"$ nmcli device wifi connect HomeNet password "+pw<<key<<QStringList{"key: "+pw};
   QTest::newRow("a .netrc password")<<"machine api.github.com login alice password "+pw<<key<<QStringList{"key: "+pw};
   QTest::newRow("SQL WITH PASSWORD")<<"CREATE USER app WITH PASSWORD '"+pw+"';"<<key<<QStringList{"key: '"+pw+"';"};
   QTest::newRow("a name glued to its program")<<"$ PGPASSWORD="+pw+" psql -h db"<<key<<QStringList{"key: PGPASSWORD="+pw};
   QTest::newRow("a word ending in pass is no name")<<"BYPASS=enabled"<<key<<QStringList{};
   const auto shortValue=QString("708dWgW5")+"kQ2z"+"P9mLx4"+"Rt7Bv",uuid=QString("4e4cff2d")+"-1a2b-4c3d-9e8f-"+"0a1b2c3d4e5f",secretLabel=QString("Secret")+" Ke"+"y: ",apiLabel=QString("API")+" Ke"+"y: ";
   QTest::newRow("a two-word label with a short value")<<secretLabel+shortValue<<key<<QStringList{"key: "+shortValue};
   QTest::newRow("a two-word label with a UUID")<<apiLabel+uuid<<key<<QStringList{"key: "+uuid};
   QTest::newRow("a password before more words")<<"Temporary password: "+strong+" (please change it)"<<key<<QStringList{"key: "+strong};
   QTest::newRow("a password before the sentence goes on")<<"Your temporary password is "+strong+". Change it soon."<<key<<QStringList{"key: "+strong+"."};
   QTest::newRow("a quoted password inside an object")<<"{ password: \""+pw+"\", port: 5432 }"<<key<<QStringList{"key: \""+pw+"\","};
   QTest::newRow("a password rule in prose stays visible")<<"Password: must be at least 8 characters"<<key<<QStringList{};
   QTest::newRow("a password policy stays visible")<<"The password policy requires 12 characters"<<key<<QStringList{};
   QTest::newRow("a Wi-Fi psk")<<"802-11-wireless-security.psk: orchard-lantern"<<key<<QStringList{"key: orchard-lantern"};
   QTest::newRow("a passphrase of words")<<"wpa_passphrase=correct horse battery staple"<<key<<QStringList{"key: wpa_passphrase=correct horse battery staple"};
   QTest::newRow("a German label")<<"Passwort: "+pw<<key<<QStringList{"key: "+pw};
   QTest::newRow("a French label of three words")<<"Mot de passe : "+strong<<key<<QStringList{"key: "+strong};
   QTest::newRow("a Russian label")<<QString::fromUtf8("Пароль: ")+pw<<key<<QStringList{"key: "+pw};
   QTest::newRow("a Chinese label glued to its value")<<QString::fromUtf8("密码：")+pw<<key<<QStringList{QString::fromUtf8("key: 密码：")+pw};
   QTest::newRow("a Polish label")<<QString::fromUtf8("Hasło: ")+pw<<key<<QStringList{"key: "+pw};
   QTest::newRow("an Arabic label")<<QString::fromUtf8("كلمة المرور: ")+pw<<key<<QStringList{"key: "+pw};
   const auto session=QString("k3j4h5g6f7d8s9a0")+"q1w2e3r4t5y6u7i8";
   QTest::newRow("a session cookie")<<"Cookie: sessionid="+session+";"<<key<<QStringList{"key: sessionid="+session+";"};
   // Group 4: diff signs.
   const auto aws=QString("AK")+"IAIOSFODNN7EXAMPL3";
   QTest::newRow("a key behind a diff's minus")<<"-"+aws<<key<<QStringList{"key: -"+aws};
   QTest::newRow("a key in a word diff")<<"{+"+aws+"+} [-REDACTED-]"<<key<<QStringList{"key: {+"+aws+"+}"};
   QTest::newRow("a setting behind a diff's plus")<<"+DB_PASSWORD="+pw+"!"<<key<<QStringList{"key: +DB_PASSWORD="+pw+"!"};
   // Group 10: token formats.
   const auto discord=QString("MTEyMzQ1Njc4OTAxMjM0NTY3")+".GhIjKl."+"aBcDeFgHiJkLmNoPqRsTuVwXyZ0123456789ab";
   QTest::newRow("a Discord bot token")<<"client.run('"+discord+"')"<<key<<QStringList{"key: client.run('"+discord+"')"};
   QTest::newRow("a dotted name is no Discord token")<<"see Microsoft.Extensions.ConfigurationBinderAndMore"<<key<<QStringList{};
   const auto age=QString("AGE-SECRET")+"-KEY-1"+"QPZRY9X8GF2TVDW0S3JN54KHCE6MUA7LQPZRY9X8GF2TVDW0S3JN54KHCE6MUA7LQPZRY9";
   QTest::newRow("an age secret key")<<age<<key<<QStringList{"key: "+age};
   const auto tailscale=QString("tskey")+"-auth-"+"kAbc123Def456GhiJ-AbcDef123456GhiJkl789MnoPqr012StuVwx";
   QTest::newRow("a Tailscale key")<<"$ tailscale up --authkey "+tailscale<<key<<QStringList{"key: "+tailscale};
   const auto bootstrap=QString("abcdef")+"."+"0123456789abcdef";
   QTest::newRow("a kubeadm token")<<"kubeadm token create printed "+bootstrap<<key<<QStringList{"key: "+bootstrap};
   // Group 8: 2FA setup keys, recovery codes and seed phrases.
   const auto totp=QString("JBSW Y3DP")+" EHPK 3PXP";
   QTest::newRow("a TOTP setup key under its label")<<"Can't scan it? Enter this key: | "+totp+" "+totp<<key<<QStringList{"key: "+totp+" "+totp};
   QTest::newRow("capitals with no key label stay visible")<<"Status | "+totp+" "+totp<<key<<QStringList{};
   QTest::newRow("recovery codes under their heading")<<"Recovery codes | 9f3k2-x7m1q ab4c5-dd8e2 | 4821 9034 | Download"<<key<<QStringList{"key: 9f3k2-x7m1q ab4c5-dd8e2","key: 4821 9034"};
   QTest::newRow("prose after a recovery heading stays visible")<<"Recovery codes | Store them somewhere safe."<<key<<QStringList{};
   QStringList seed,expected;const QStringList phrase{"abandon","ability","able","about","above","absent","absorb","abstract","absurd","abuse","access","accident"};
   for(int i=0;i<phrase.size();++i){seed<<QString::number(i+1)+". "+phrase.at(i);expected<<"key: "+QString::number(i+1)+". "+phrase.at(i);if(i%3==2&&i+1<phrase.size())seed<<"|";}
   QTest::newRow("a numbered seed phrase")<<"Your recovery phrase | "+seed.join(' ')<<key<<expected;
   QTest::newRow("a short numbered list stays visible")<<"1. apples 2. bread 3. milk"<<key<<QStringList{};
   // Sol on #1952.
   const auto tail=QString("-ZXQ4")+"V7A=",wrapped=QString("Q7Z1hMJZ")+"pR4tW9xL2kV8nB3cD6fG1hJ5mK0qS7uY";
   QTest::newRow("a base64 tail starting with a dash stays with its token")<<"SIGNING_KEY="+wrapped+" | "+tail<<key<<QStringList{"key: SIGNING_KEY="+wrapped+" "+tail};
   QTest::newRow("a setting about a method is still a secret's")<<"API_TOKEN_METHOD=orchard"<<key<<QStringList{"key: API_TOKEN_METHOD=orchard"};
   QTest::newRow("a password flag's plain value")<<"$ deploy --password OrchardLantern"<<key<<QStringList{"key: OrchardLantern"};
   QTest::newRow("a password flag's digits")<<"$ deploy --password 7654"<<key<<QStringList{"key: 7654"};
   QTest::newRow("a quoted passphrase after a flag")<<"$ deploy --password='correct horse battery staple' --yes"<<key<<QStringList{"key: --password='correct horse battery staple'"};
   QTest::newRow("a region-shaped password under its label")<<"Mot de passe : us-east-1"<<key<<QStringList{"key: us-east-1"};
   QTest::newRow("a passphrase of eight words")<<"wpa_passphrase=one two three four five six seven eight"<<key<<QStringList{"key: wpa_passphrase=one two three four five six seven eight"};
   QTest::newRow("a setup key of letters alone")<<"Setup key: | JBSW YDPE HPKP XPAB"<<key<<QStringList{"key: JBSW YDPE HPKP XPAB"};
   QTest::newRow("a setup key over two rows")<<"Enter this key: | JBSW Y3DP | EHPK 3PXP"<<key<<QStringList{"key: JBSW Y3DP EHPK 3PXP"};
   QTest::newRow("recovery codes on the heading's row")<<"Recovery codes: MTAX-CKAP GHRQ-PLKD"<<key<<QStringList{"key: MTAX-CKAP GHRQ-PLKD"};
   const auto sid=QString("12dsfFdsf")+"bn23dsk",letters=QString("KgmBntgvf")+"RQhGHgdKW";
   QTest::newRow("a cookie after a direction marker")<<"> Cookie: sid="+sid<<key<<QStringList{"key: sid="+sid};
   QTest::newRow("a cookie of letters alone")<<"Cookie: sid="+letters<<key<<QStringList{"key: sid="+letters};
   const auto lettersOnly=QString("AK")+"IA"+"PQRSTUVWXYZABCDE";
   QTest::newRow("a letters-only key behind a diff's minus")<<"-"+lettersOnly<<key<<QStringList{"key: -"+lettersOnly};
   const auto lettersToken=QString("abcdef")+"."+"abcdefghijklmnop";
   QTest::newRow("a card behind a diff's minus")<<"-4111-1111-1111-1111"<<QStringList{"card"}<<QStringList{"card: -4111-1111-1111-1111"};
   // Sol round 2 on #1952.
   const auto quoteTail=QString("-ABCD")+"EFG=\"";
   QTest::newRow("a tail ending in padding and a quote stays with its token")<<"SIGNING_KEY=\""+wrapped+" | "+quoteTail<<key<<QStringList{"key: SIGNING_KEY=\""+wrapped+" "+quoteTail};
   QTest::newRow("a quoted passphrase of short words after a flag")<<"$ deploy --password='go ahead and continue'"<<key<<QStringList{"key: --password='go ahead and continue'"};
   QTest::newRow("a password that starts like a call")<<"Mot de passe : getPassword()"+QString("Crane7!")<<key<<QStringList{"key: getPassword()Crane7!"};
   QTest::newRow("a passphrase with a slash")<<"wpa_passphrase=orchard lantern/river stone"<<key<<QStringList{"key: wpa_passphrase=orchard lantern/river stone"};
   QTest::newRow("a setup key after lowercase words")<<"enter this key JBSW YDPE HPKP XPAB"<<key<<QStringList{"key: JBSW YDPE HPKP XPAB"};
   const auto first=QString("Orchard")+"123",second=QString("alice:")+"lantern9";
   QTest::newRow("two commands on one line")<<"$ sshpass -p "+first+" "+fetch+" -u "+second+" https://example.test"<<key<<QStringList{"key: "+first,"key: "+second};
   {QStringList repeated,expected;for(int i=1;i<=12;++i){const auto w=i<12?QStringLiteral("abandon"):QStringLiteral("about");repeated<<QString::number(i)+". "+w;expected<<"key: "+QString::number(i)+". "+w;if(i%3==0&&i<12)repeated<<"|";}
    QTest::newRow("a seed phrase with repeated words")<<"Secret Recovery Phrase | "+repeated.join(' ')<<key<<expected;}
   QTest::newRow("a phone beside a recovery code")<<"Recovery codes: MTAX-CKAP Phone: 212 555 0100"<<QStringList{"key","phone"}<<QStringList{"key: MTAX-CKAP","phone: 212 555 0100"};
   // Sol round 3 on #1952.
   QTest::newRow("a quoted password with an escaped quote")<<"$ sshpass -p \"orchard\\\" lantern stone\" ssh nas"<<key<<QStringList{"key: \"orchard\\\" lantern stone\""};
   QTest::newRow("a call with mismatched quotes is a password")<<"Mot de passe : check(\"Crane7!')"<<key<<QStringList{"key: check(\"Crane7!')"};
   QTest::newRow("a password with a run of asterisks in it")<<"$ sshpass -p 'River7 **** orchard' ssh nas"<<key<<QStringList{"key: 'River7 **** orchard'"};
   QTest::newRow("a setup key after its heading in capitals")<<"ENTER THIS KEY JBSW YDPE HPKP XPAB"<<key<<QStringList{"key: JBSW YDPE HPKP XPAB"};
   QTest::newRow("recovery codes after a colon of its own")<<"Recovery codes : MTAX-CKAP GHRQ-PLKD"<<key<<QStringList{"key: MTAX-CKAP GHRQ-PLKD"};
   QTest::newRow("numbered recovery codes on the heading's row")<<"Recovery codes: 1. MTAX-CKAP 2. GHRQ-PLKD"<<key<<QStringList{"key: MTAX-CKAP GHRQ-PLKD"};
   // Numbered lists of short lowercase words with no phrase label stay visible (Sr Manager on #1941).
   {const auto numbered=[](const QStringList& items,int perRow){QStringList out;for(int i=0;i<items.size();++i){out<<QString::number(i+1)+". "+items.at(i);if((i+1)%perRow==0&&i+1<items.size())out<<"|";}return out.join(' ');};
    const QStringList shopping{"apples","bread","milk","eggs","butter","cheese","rice","beans","onions","garlic","lemons","coffee"};
    const QStringList cooking{"flour","sugar","salt","yeast","water","butter","milk","eggs","vanilla","cocoa","honey","cream","nuts","oats"};
    const QStringList spelling{"because","friend","enough","through","thought","caught","island","answer","believe","receive","weird","height"};
    const QStringList tags{"linux","rust","python","docker","cloud","security","backend","testing","release","design","network","mobile"};
    QTest::newRow("a shopping list stays visible")<<"Shopping list | "+numbered(shopping,1)<<key<<QStringList{};
    QTest::newRow("ingredients stay visible")<<"Ingredients | "+numbered(cooking,2)<<key<<QStringList{};
    QTest::newRow("a spelling list stays visible")<<"Spelling words for Friday | "+numbered(spelling,3)<<key<<QStringList{};
    QTest::newRow("a tag list stays visible")<<"Popular tags | "+numbered(tags,4)<<key<<QStringList{};
    QStringList expected;for(int i=0;i<shopping.size();++i)expected<<"key: "+QString::number(i+1)+". "+shopping.at(i);
    QTest::newRow("the same words under a phrase label are covered")<<"Write down your 12-word phrase | "+numbered(shopping,3)<<key<<expected;}
   // Sol round 4 on #1952, and #1940's find.
   QTest::newRow("a quote after an escaped backslash closes the password")<<"$ sshpass -p \"orchard\\\\\" lantern"<<key<<QStringList{"key: \"orchard\\\\\""};
   QTest::newRow("a setup key with a group that reads as a word")<<"Setup key: | JBSW SCAN HPKP XPAB"<<key<<QStringList{"key: JBSW SCAN HPKP XPAB"};
   QTest::newRow("a qualified call with mismatched quotes is a password")<<"Mot de passe : os.check(\"Crane7!')"<<key<<QStringList{"key: os.check(\"Crane7!')"};
   QTest::newRow("a password flag's value with asterisks in it")<<"$ deploy --password='River7 **** orchard' --yes"<<key<<QStringList{"key: --password='River7 **** orchard'"};
   const auto fallback=QString("Crane7")+"Queue";
   QTest::newRow("a hardcoded fallback after an environment read")<<"db_password = os.Getenv(\"DB_PASSWORD\")+\""+fallback+"\""<<key<<QStringList{"key: os.Getenv(\"DB_PASSWORD\")+\""+fallback+"\""};
   QTest::newRow("a fallback after process.env")<<"API_KEY=process.env.KEY||\""+fallback+"99\""<<key<<QStringList{"key: API_KEY=process.env.KEY||\""+fallback+"99\""};
   QTest::newRow("a fallback ending in an escaped quote")<<"API_KEY=process.env.KEY||\""+fallback+"\\\"\""<<key<<QStringList{"key: API_KEY=process.env.KEY||\""+fallback+"\\\"\""};
   QTest::newRow("a fallback holding an escaped quote")<<"API_KEY=process.env.KEY||\"a\\\""+fallback+"\""<<key<<QStringList{"key: API_KEY=process.env.KEY||\"a\\\""+fallback+"\""};
   QTest::newRow("a quoted password of several words")<<"Password: \"Red7fox"+QString(" blue jay\" today")<<key<<QStringList{"key: \"Red7fox blue jay\""};
   QTest::newRow("a quoted passphrase after a password label")<<"Password: \"correct horse battery staple\" today"<<key<<QStringList{"key: \"correct horse battery staple\""};
   // A quote a backslash escapes closes nothing, in a later word or the first (CodeRabbit on #1952).
   QTest::newRow("a quoted password holding an escaped quote")<<"Password: \"Red7fox"+QString(" blue\\\" jay\" today")<<key<<QStringList{"key: \"Red7fox blue\\\" jay\""};
   QTest::newRow("a quoted password whose first word ends in an escaped quote")<<"Password: \"Red7fox"+QString("\\\" blue jay\" today")<<key<<QStringList{"key: \"Red7fox\\\" blue jay\""};
   // The closing quote matches the opening one, and an escape recognition split off still escapes (Sol on #1952).
   QTest::newRow("a quoted password holding an apostrophe")<<"Password: \"rockin'"+QString(" blue jay\" today")<<key<<QStringList{"key: \"rockin' blue jay\""};
   QTest::newRow("a quoted password whose escape is a word of its own")<<"Password: \"Red7fox"+QString(" blue \\ \" jay\" today")<<key<<QStringList{"key: \"Red7fox blue \\ \" jay\""};
   // #1964: quotes pasted from a document or a chat: straight opened and curly closed, curly, German and French;
   // a straight single quote is not closed by an apostrophe inside.
   QTest::newRow("a password opened straight and closed curly")<<"Password: \"Red7fox"+QString(" blue jay” today")<<key<<QStringList{"key: \"Red7fox blue jay”"};
   QTest::newRow("a password in curly quotes")<<"Password: “Red7fox"+QString(" blue jay” today")<<key<<QStringList{"key: “Red7fox blue jay”"};
   QTest::newRow("a password in German quotes")<<"Passwort: „Red7fox"+QString(" blaue Katze“ heute")<<key<<QStringList{"key: „Red7fox blaue Katze“"};
   QTest::newRow("a password in French quotes")<<"Mot de passe : «Red7fox"+QString(" bleu geai» merci")<<key<<QStringList{"key: «Red7fox bleu geai»"};
   QTest::newRow("an apostrophe does not close a single-quoted password")<<"Password: 'it’s"+QString(" Red7 fox' ok")<<key<<QStringList{"key: 'it’s Red7 fox'"};
   QTest::newRow("curl's password quoted after the user")<<QString("$ curl -")+"u ali"+"ce:'orch"+QString("ard lantern' https://example.test")<<key<<QStringList{QString("key: ali")+"ce:'orch"+"ard lantern'"};
   QTest::newRow("curl's password in double quotes after the user")<<QString("$ curl -")+"u ali"+"ce:\"orch"+QString("ard lantern\" https://example.test")<<key<<QStringList{QString("key: ali")+"ce:\"orch"+"ard lantern\""};
   QTest::newRow("smbclient's password quoted after the user")<<"$ smbclient //nas/share -U alice%'orchard"+QString(" lantern'")<<key<<QStringList{"key: alice%'orchard lantern'"};
   QTest::newRow("a password flag in curly quotes")<<"$ mysqldump --password=“orchard"+QString(" lantern” app")<<key<<QStringList{"key: --password=“orchard lantern”"};
   // Sol on #1969: a straight closer is preferred to a curly one inside; an apostrophe between letters is no
   // closer; the whole quoted password is judged; a masked token keeps its first word; outer quotes come off.
   QTest::newRow("a curly quote inside a straight-quoted password")<<"Password: \"river”"+QString(" orange cedar\" today")<<key<<QStringList{"key: \"river” orange cedar\""};
   QTest::newRow("a curly quote in a later word of a straight-quoted password")<<"Password: \"Red7fox"+QString(" ri”ver cedar\" today")<<key<<QStringList{"key: \"Red7fox ri”ver cedar\""};
   QTest::newRow("sshpass with a curly quote inside a straight-quoted password")<<"$ sshpass -p \"river”"+QString(" orange cedar\" ssh host")<<key<<QStringList{"key: \"river” orange cedar\""};
   QTest::newRow("curl's curly single-quoted password with an apostrophe")<<QString("$ curl -")+"u ali"+"ce:‘it’s"+QString(" Red7 fox’ https://example.test")<<key<<QStringList{QString("key: ali")+"ce:‘it’s Red7 fox’"};
   QTest::newRow("curl's quoted password with a short first word")<<QString("$ curl -")+"u ali"+"ce:'red"+QString(" blue lantern' https://example.test")<<key<<QStringList{QString("key: ali")+"ce:'red blue lantern'"};
   QTest::newRow("curl's quoted password whose first word is a placeholder word")<<QString("$ curl -")+"u ali"+"ce:'example"+QString(" lantern' https://example.test")<<key<<QStringList{QString("key: ali")+"ce:'example lantern'"};
   QTest::newRow("curl's glued user with a quoted password")<<QString("$ curl -")+"uali"+"ce:'red"+QString(" blue lantern' https://example.test")<<key<<QStringList{QString("key: -uali")+"ce:'red blue lantern'"};
   QTest::newRow("curl's --user= with a quoted password")<<QString("$ curl --")+"user=ali"+"ce:'red"+QString(" blue lantern' https://example.test")<<key<<QStringList{QString("key: --user=ali")+"ce:'red blue lantern'"};
   QTest::newRow("smbclient's quoted password with a short first word")<<"$ smbclient //nas/share -U alice%'red"+QString(" blue lantern'")<<key<<QStringList{"key: alice%'red blue lantern'"};
   QTest::newRow("a masked quoted token keeps its first word")<<"$ deploy --token=“river7"+QString("X **** cedar” now")<<key<<QStringList{"key: --token=“river7X"};
   QTest::newRow("a placeholder in French quotes with spaces")<<"Mot de passe : « TODO » merci"<<key<<QStringList{};
   QTest::newRow("a version in German quotes")<<"Passwort: „12.3.4“ heute"<<key<<QStringList{};
   QTest::newRow("curl's quoted password holding a double slash")<<QString("$ curl -")+"u ali"+"ce:'orch"+QString("ard // lantern' https://example.test")<<key<<QStringList{QString("key: ali")+"ce:'orchard // lantern'"};
   QTest::newRow("smbclient's quoted password holding a double slash")<<"$ smbclient //nas/share -U alice%'orchard"+QString(" // lantern'")<<key<<QStringList{"key: alice%'orchard // lantern'"};
   QTest::newRow("a spaced token flag in mixed quotes")<<"$ deploy --token \u201Cred"+QString(" blue lantern\" now")<<key<<QStringList{"key: \u201Cred blue lantern\""};
   QTest::newRow("an operator inside a quoted password")<<"$ sshpass -p 'red"+QString(" & lantern' ssh host")<<key<<QStringList{"key: 'red & lantern'"};
   QTest::newRow("a quote in the next command does not close the password")<<"$ sshpass -p \"river\u201D"+QString(" ssh host && printf \"%s\" ready")<<key<<QStringList{"key: \"river\u201D"};
   QTest::newRow("a quoted password of several words after the word password")<<"password \"Red7fox"+QString(" blue jay\" today")<<key<<QStringList{"key: \"Red7fox blue jay\""};
   QTest::newRow("a recovery code with a misread character")<<"Recovery codes | 9f3k2-x7m1q 6p4w2-g8q/v"<<key<<QStringList{"key: 9f3k2-x7m1q 6p4w2-g8q/v"};
   QTest::newRow("a recovery code with a misread character beside its dash")<<"Recovery codes | 9f3k2-x7m1q 6p4w2-/8q7v"<<key<<QStringList{"key: 9f3k2-x7m1q 6p4w2-/8q7v"};
   QTest::newRow("a fallback with an apostrophe")<<"db_password = os.Getenv(\"DB_PASSWORD\")+\"it's"+QString("private7")+"\""<<key<<QStringList{"key: os.Getenv(\"DB_PASSWORD\")+\"it'sprivate7\""};
   QTest::newRow("an environment read alone stays visible")<<"db_password = os.Getenv(\"DB_PASSWORD\")"<<key<<QStringList{};
   QTest::newRow("a kubeadm token of letters")<<"kubeadm join worker --token "+lettersToken<<key<<QStringList{"key: "+lettersToken};}
  QTest::newRow("ordinary prose matches nothing")<<"The quick brown fox jumps over 3 lazy dogs in 2026. | Version 1.2 shipped on Monday."<<all<<QStringList{};
  // #1940 (#1931 fuzz groups 3, 6, 17, 19): public hashes, IDs, signatures and noise are no keys; what
  // looks the same beside a secret's name, or with a word that makes no label, stays covered.
  {const auto key=QStringList{"key"};
   const auto h32=hexOf("e",32),h40=hexOf("c",40),h64=hexOf("s",64),b44=base64Of("w",43)+"=",b76=base64Of("x",76);
   const auto uuid=hexOf("u",8)+"-"+hexOf("v",4)+"-4"+hexOf("v",3)+"-8"+hexOf("q",3)+"-"+hexOf("r",12);
   QTest::newRow("a hexdump row stays visible")<<"00000010 79 af 00 36 a6 c6 cd 45 2e c1 88 12 44 b6 b0 01 |y..6...E....D...|"<<key<<QStringList{};
   QTest::newRow("an xxd row stays visible")<<"00000010: cbc5 c7c6 75eb 2000 fa90 888b 0427 7ae7 ....u..."<<key<<QStringList{};
   QTest::newRow("a Wireshark row read with misread groups stays visible")<<"@010 8d de 16 d5 9d 10 e7 2c 1d ec 35 Ob de df 49 67"<<key<<QStringList{};
   QTest::newRow("a hex dump row without its offset stays visible")<<"1f 1c 5a 6b dd ba bd e8 e3 98 7c e4 82 41 6f 9a ..Zk......|..Ao."<<key<<QStringList{};
   QTest::newRow("a GPG fingerprint stays visible")<<"5978 DD4D B993 16BF 6C5D 43D4 DF67 319C DA35 2C4E"<<key<<QStringList{};
   QTest::newRow("sha256sum output below its command stays visible")<<"$ sha256sum web-1 web-2 | "+h64+" web-1 | "+hexOf("t",64)+" web-2"<<key<<QStringList{};
   QTest::newRow("a digest before its file name stays visible")<<h64+" ubuntu-26.04-desktop-amd64.iso"<<key<<QStringList{};
   QTest::newRow("a ref line stays visible")<<h40+" refs/heads/main"<<key<<QStringList{};
   QTest::newRow("a hex run after the checksum rows is covered")<<"$ sha256sum web-1 | "+h64+" web-1 | $ openssl rand -hex 32 | "+hexOf("k",64)<<key<<QStringList{"key: "+hexOf("k",64)};
   QTest::newRow("a hex run before a word that is no file stays covered")<<h64+" production"<<key<<QStringList{"key: "+h64+" production"};
   QTest::newRow("a Sentry event ID stays visible")<<"Event ID: "+h32+" 2026-11-03T18:33:29.939Z"<<key<<QStringList{};
   QTest::newRow("a log line's request and trace IDs stay visible")<<"2026-11-18T14:59:59.196Z DEBUG req="+uuid+" trace_id="+h32<<key<<QStringList{};
   QTest::newRow("a JSON log's trace ID stays visible")<<"\"traceId\": \""+h32+"\","<<key<<QStringList{};
   QTest::newRow("an S3 request ID header stays visible")<<"x-amz-id-2: "+b76<<key<<QStringList{};
   QTest::newRow("a security token header stays covered")<<"x-amz-security-token: "+b76<<key<<QStringList{"key: x-amz-security-token: "+b76};
   QTest::newRow("a Gerrit Change-Id stays visible")<<"Change-Id: I"+h40<<key<<QStringList{};
   QTest::newRow("git's tree and parent lines stay visible")<<"tree "+h40+" | parent "+hexOf("p",40)<<key<<QStringList{};
   QTest::newRow("a boot ID stays visible")<<"Boot ID: "+h32<<key<<QStringList{};
   QTest::newRow("a secret ID stays covered")<<"Secret ID: "+h32<<key<<QStringList{"key: Secret ID: "+h32};
   // Fails closed: a public ID beside a word that looks like a secret of its own stays covered with it.
   QTest::newRow("a token after an ID stays covered")<<"Event ID: "+h32+" "+QString("gh")+"p_"+base64Of("g",36).replace('+','a').replace('/','b')<<key<<QStringList{"key: Event ID: "+h32+" gh"+QString("p_")+base64Of("g",36).replace('+','a').replace('/','b')};
   QTest::newRow("a WireGuard public key stays visible")<<"PublicKey = "+b44<<key<<QStringList{};
   QTest::newRow("a WireGuard private key stays covered")<<"PrivateKey = "+b44<<key<<QStringList{"key: PrivateKey = "+b44};
   QTest::newRow("a go.sum hash stays visible")<<"golang.org/x/text v0.14.0 h1:"+b44<<key<<QStringList{};
   QTest::newRow("a DKIM signature stays visible")<<"h=to:subject:message-id:date:from:mime-version; bh="+b44+"; | b="+base64Of("d",60)+" | "+base64Of("f",60)<<key<<QStringList{};
   QTest::newRow("a traceparent stays visible")<<"traceparent: 00-"+h32+"-"+hexOf("z",16)+"-01"<<key<<QStringList{};
   QTest::newRow("a misread integrity digest stays visible")<<"4 integrity shaS12-"+base64Of("i",86)+"=="<<key<<QStringList{};
   const auto text=QString::fromLatin1(QByteArray("Test for ready meeting score rose and the plan we made").toBase64());
   // Sol on #1954: nothing shows that a decoded sentence is public (a passphrase is a sentence too), so
   // base64 of text stays covered.
   QTest::newRow("base64 of plain text stays covered")<<"payload: "+text<<key<<QStringList{"key: payload: "+text};
   QTest::newRow("htop bars stay visible")<<"TLTTTT TTT T TTT T TTT T TTT T TTT T TTT 78 | SOIIIITILIITTEEETTEEEEEEEEEEEEEEEEE"<<key<<QStringList{};
   QTest::newRow("receipt leaders stay visible")<<"Croissant ccooecoocoocoocoocococooccooo 3.20"<<key<<QStringList{};
   QTest::newRow("laughter stays visible")<<"11 lol hahahahahahahahahahahahaha"<<key<<QStringList{};
   QTest::newRow("a binary mask stays visible")<<"mask = 0b11111111111111111111111100000000"<<key<<QStringList{};
   QTest::newRow("a type after a key's name stays visible")<<"pub api_key: Option<String>,"<<key<<QStringList{};
   QTest::newRow("reading a password from the environment stays visible")<<"let db_password = std::env::var(\"DB_PASSWORD\")?;"<<key<<QStringList{};
   QTest::newRow("a token count stays visible")<<"total_tokens: 12,450"<<key<<QStringList{};
   QTest::newRow("a receipt's AID stays visible")<<"AID A0000000031010"<<key<<QStringList{};
   // Recognition's misreads of the same rows (#1931 recognized words).
   QTest::newRow("a hex dump row with a misread offset and text column stays visible")<<"Q0O0bO 42 8a 37 b9 a7 b5 a2 di b7 da 3a 6b Ff8 54 04 F7 B.7.......:K.T.. US300000000000c"<<key<<QStringList{};
   QTest::newRow("a doubled x in an S3 header stays visible")<<"xX-amz-id-2: "+b76<<key<<QStringList{};
   QTest::newRow("a trace ID label read with an l stays visible")<<"\"tracelId\": \""+h32+"\","<<key<<QStringList{};
   QTest::newRow("md5sum read as mdSsum still names its rows")<<"$ mdSsum a.iso | "+h32+" a.iso"<<key<<QStringList{};
   QTest::newRow("container IDs below podman ps --no-trunc stay visible")<<"$ podman ps --no-trunc --format {{.ID}} | "+h64+" web-1"<<key<<QStringList{};
   QTest::newRow("a JSON checksum stays visible")<<"\"checksum\": \""+h64+"\","<<key<<QStringList{};
   QTest::newRow("a Ruby hash setting stays visible")<<"ENV['hash'] ||= '"+h64+"'"<<key<<QStringList{};
   // Sol on #1954.
   QTest::newRow("a URL password that reads as a type stays covered")<<"postgres://alice:bytes@db/app"<<key<<QStringList{"key: postgres://alice:bytes@db/app"};
   QTest::newRow("an environment read with a literal fallback stays covered")<<"let db_password = std::env::var(\"DB\").unwrap_or(\"Crane7Queue\");"<<key<<QStringList{"key: std::env::var(\"DB\").unwrap_or(\"Crane7Queue\");"};
   QTest::newRow("a token with thousands commas stays covered")<<"API_TOKEN=123,456,789"<<key<<QStringList{"key: API_TOKEN=123,456,789"};
   {const auto body=base64Of("g",36).replace('+','a').replace('/','b');
    QTest::newRow("a token split at its prefix after a public label stays covered")<<"PublicKey = gh"+QString("p_ ")+body<<key<<QStringList{"key: PublicKey = gh"+QString("p_ ")+body};}
   {const auto credential=QString::fromLatin1((QByteArray("DB_PASS")+QByteArray("WORD=correct horse battery staple")).toBase64());
    QTest::newRow("base64 of a credential stays covered")<<"payload: "+credential<<key<<QStringList{"key: "+credential};}
   QTest::newRow("an AWS key ID with a legal body split after its AK stays covered")<<"AK IAQ"+QString("234567234567K7Z")<<key<<QStringList{"key: AK IAQ234567234567K7Z"};
   QTest::newRow("a word that only starts like a mangled symbol stays covered")<<"in _ZN9BgqRCJC4uqcdgOHvIZSoPYCce9auIA2mEv"<<key<<QStringList{"key: in _ZN9BgqRCJC4uqcdgOHvIZSoPYCce9auIA2mEv"};
   QTest::newRow("a quoted value that reads as a type stays covered")<<"password=\"bytes\""<<key<<QStringList{"key: password=\"bytes\""};
   QTest::newRow("access tokens with thousands commas stay covered")<<"ACCESS_TOKENS=123,456,789"<<key<<QStringList{"key: ACCESS_TOKENS=123,456,789"};
   {const auto pin=QString::fromLatin1(QByteArray("PIN: 123456 for the front door and spare office").toBase64());
    QTest::newRow("base64 of a PIN stays covered")<<"payload: "+pin<<key<<QStringList{"key: "+pin};}
   QTest::newRow("an AWS key ID read with 0, 1 and 8 split after its AK stays covered")<<"AK IA0"+QString("C1K1Z0M8L1X1W0Y")<<key<<QStringList{"key: AK IA0C1K1Z0M8L1X1W0Y"};
   {const auto typed=QString("API_")+"KEY"+QString("=")+"Bytes";
    QTest::newRow("a type after an equals sign stays covered")<<typed<<key<<QStringList{"key: "+typed};}
   QTest::newRow("a quoted JSON value that reads as a type stays covered")<<"{\"role\":\"admin\",\"authorization\":\"Bytes\"}"<<key<<QStringList{"key: {\"role\":\"admin\",\"authorization\":\"Bytes\"}"};
   QTest::newRow("a receipt AID that reads as letters stays covered")<<"AID A0C1K1Z0M8L1X1W0Y"<<key<<QStringList{"key: AID A0C1K1Z0M8L1X1W0Y"};
   QTest::newRow("digest rows after a numbered command line stay visible")<<"0001 sha256sum web-one | 0002 "+h64+" web-one"<<key<<QStringList{};
   QTest::newRow("a bare hex row after named checksum rows stays covered")<<"$ sha256sum web-one | "+h64+" web-one | "+hexOf("k",64)<<key<<QStringList{"key: "+h64+" web-one "+hexOf("k",64)};
   QTest::newRow("a numbered hex row after an unnumbered command stays covered")<<"$ sha256sum web-one | 123 "+hexOf("k",64)<<key<<QStringList{"key: 123 "+hexOf("k",64)};
   QTest::newRow("a certificate label over no certificate stays covered")<<"client-certificate-data: "+base64Of("q",60)<<key<<QStringList{"key: client-certificate-data: "+base64Of("q",60)};
   QTest::newRow("an AWS key ID split after its AK stays covered")<<"AK IA"+QString("Z7Q4R2N9KT6HB3XW")<<key<<QStringList{"key: AK IAZ7Q4R2N9KT6HB3XW"};
   QTest::newRow("recovery codes beside a CloudFront ID stay covered")<<"eoc6s-2itl4 n7asb-p8uxz X-Amz-Cf-Id: "+base64Of("y",54)+"=="<<key<<QStringList{"key: eoc6s-2itl4 n7asb-p8uxz X-Amz-Cf-Id: "+base64Of("y",54)+"=="};
   QTest::newRow("a token in a run with an integrity digest stays covered")<<"+API_TOKEN="+hexOf("a",40)+" \"shaS12-"+base64Of("i",86)+"==\""<<key<<QStringList{"key: +API_TOKEN="+hexOf("a",40)+" \"shaS12-"+base64Of("i",86)+"==\""};
   QTest::newRow("an AWS key ID in one word stays covered")<<"ID A"+QString("KIA")+"Z7Q4R2N9KT6HB3XW"<<key<<QStringList{"key: ID A"+QString("KIA")+"Z7Q4R2N9KT6HB3XW"};}
 }
 // #1940 (#1931 fuzz group 9): in tiled windows a hash word in one pane names no digest in another on
 // the same row: "commit <sha>" on the left leaves a hex secret on the right covered, while its own sha
 // stays visible.
 void aHashWordInAnotherPaneIsNoLabel() {
  const auto hex=hexOf("k",64),sha=hexOf("c",40);
  QList<RecognizedWord> row{{"commit",QRect(0,0,48,20),90,0},{sha,QRect(56,0,320,20),90,0},{hex,QRect(1000,0,512,20),90,0}};
  QCOMPARE(found(row,QStringList{"key"}),QStringList{"key: "+hex});
  // A secret word anywhere on the row still keeps every run covered.
  row.append({"token",QRect(1600,0,40,20),90,0});
  QCOMPARE(found(row,QStringList{"key"}),(QStringList{"key: commit "+sha,"key: "+hex}));
  // A checksum command in the right pane names the digest rows below it in that pane only (Sol on #1954):
  // a hex secret below in the left pane stays covered, the right pane's digest stays visible.
  const auto digest=hexOf("d",64),secret=hexOf("s",64);
  QList<RecognizedWord> panes{{"$",QRect(0,0,8,20),90,0},{"ls",QRect(16,0,16,20),90,0},{"$",QRect(1000,0,8,20),90,0},{"sha256sum",QRect(1016,0,72,20),90,0},{"a.iso",QRect(1096,0,40,20),90,0},
                              {secret,QRect(0,30,512,20),90,1},{digest,QRect(1000,30,512,20),90,1},{"a.iso",QRect(1520,30,40,20),90,1}};
  QCOMPARE(found(panes,QStringList{"key"}),QStringList{"key: "+secret});
  // ...and a git command in the other pane anchors nothing for it.
  QList<RecognizedWord> gitPanes{{"$",QRect(0,0,8,20),90,0},{"git",QRect(16,0,24,20),90,0},{"status",QRect(48,0,48,20),90,0},{"$",QRect(1000,0,8,20),90,0},{"sha256sum",QRect(1016,0,72,20),90,0},{"web-one",QRect(1096,0,56,20),90,0},
                                 {secret,QRect(0,30,512,20),90,1},{digest,QRect(1000,30,512,20),90,1},{"web-one",QRect(1520,30,56,20),90,1}};
  QCOMPARE(found(gitPanes,QStringList{"key"}),QStringList{"key: "+secret});
  // A file name in the pane beside a hex run is no evidence that the run is a digest (Sol on #1955).
  QList<RecognizedWord> named{{secret,QRect(0,0,512,20),90,0},{"ubuntu-26.04-desktop-amd64.iso",QRect(1000,0,240,20),90,0}};
  QCOMPARE(found(named,QStringList{"key"}),QStringList{"key: "+secret});
  // An assignment anywhere on a digest row, however far, ends the rows: the hex before it stays covered.
  QList<RecognizedWord> tabbed{{"$",QRect(0,0,8,20),90,0},{"sha256sum",QRect(16,0,72,20),90,0},{"web-one",QRect(96,0,56,20),90,0},
                               {secret,QRect(0,30,512,20),90,1},{"API_KEY=prod",QRect(900,30,96,20),90,1}};
  QVERIFY(found(tabbed,QStringList{"key"}).join(' ').contains(secret));
 }
 // #1940: a value of no fixed length (a DKIM signature, a kubeconfig certificate) runs on only over rows
 // a wrap leaves, each reaching the right edge: a key's row after the certificate's short last row is
 // not the certificate's.
 void aPublicValueRunsOnOnlyOverWrappedRows() {
  const auto signature="b="+base64Of("c",60),tail=base64Of("t",20),key=base64Of("p",60);
  QList<RecognizedWord> rows{{"DKIM-Signature:",QRect(0,0,120,20),90,0},{"v=1;",QRect(128,0,32,20),90,0},{signature,QRect(0,30,496,20),90,1},
                             {tail,QRect(0,60,160,20),90,2},{key,QRect(0,90,480,20),90,3}};
  QCOMPARE(found(rows,QStringList{"key"}),QStringList{"key: "+key});
 }
 void categoriesMatchTheirShapes() {
  QFETCH(QString,page);QFETCH(QStringList,categories);QFETCH(QStringList,expected);
  QCOMPARE(found(words(page),categories),expected);
 }
 // #1941: a value a column away from its label, in a form's or router's row, and a value under its label
 // (a password manager's field). A tiled window's word beside a label is not its value.
 void labelledValuesBesideAndBelow() {
  const auto pw=QString("tuna salad ")+"77",strong=QString("Sum")+"mer"+"2024!";
  const auto placed=[](const QList<QPair<QString,int>>& row,int line){QList<RecognizedWord> out;for(const auto& [text,x]:row)out.append({text,QRect(x,line*40,int(text.size())*8,20),90,line});return out;};
  QCOMPARE(found(placed({{"Password",24},{"tuna",280},{"salad",320},{"77",368}},0),QStringList{"key"}),QStringList{"key: "+pw});
  const auto lantern=QString("orchard-")+"lantern-"+"42";
  QCOMPARE(found(placed({{"WPA",24},{QString("Ke")+"y",56},{lantern,280}},0),QStringList{"key"}),QStringList{"key: "+lantern});
  // A word with no digit far from a label is no value ("Password    Show").
  QCOMPARE(found(placed({{"Password",24},{"Show",280}},0),QStringList{"key"}),QStringList{});
  // A label and its value below it, in a field.
  auto field=placed({{"Password",28}},0)+placed({{strong,38}},1);
  QCOMPARE(found(field,QStringList{"key"}),QStringList{"key: "+strong});
  // Not a value a column away or far below.
  field.last().rect.translate(400,0);QCOMPARE(found(field,QStringList{"key"}),QStringList{});
  auto far=placed({{"Password",28}},0)+placed({{strong,38}},4);QCOMPARE(found(far,QStringList{"key"}),QStringList{});
  // A translated label above its value (Sol on #1952).
  QCOMPARE(found(placed({{QString::fromUtf8("Hasło:"),28}},0)+placed({{strong,38}},1),QStringList{"key"}),QStringList{"key: "+strong});
  QCOMPARE(found(placed({{QString::fromUtf8("Mật"),28},{QString::fromUtf8("khẩu"),64}},0)+placed({{strong,38}},1),QStringList{"key"}),QStringList{"key: "+strong});
  // A right-to-left label alone on its row, written visually with its colon first.
  QCOMPARE(found(placed({{QString::fromUtf8(":סיסמה"),28}},0)+placed({{strong,38}},1),QStringList{"key"}),QStringList{"key: "+strong});
  // ...with its colon read as a word of its own.
  QCOMPARE(found(placed({{":",28},{QString::fromUtf8("סיסמה"),40}},0)+placed({{strong,38}},1),QStringList{"key"}),QStringList{"key: "+strong});
  // "Forgot password?" under a label is a link, not a value.
  QCOMPARE(found(placed({{"Password",28}},0)+placed({{"Forgot",38},{"password?",94}},1),QStringList{"key"}),QStringList{});
  // A label ending a tiled window's row: the other window's word is not its value.
  QCOMPARE(found(placed({{"git",10},{"log",42},{"token:",74},{"expired",800}},0),QStringList{"key"}),QStringList{});
 }
 // #1941 (Sol on #1952): a wallet's seed phrase beside a numbered list in another window that starts higher up:
 // the wallet's own label, below the list's start, still takes its column; the list stays visible.
 // #1965: recognition splits a connection string's password at its @ with no space: the password runs on
 // into the next box (Sol on #1967).
 void connectionStringPasswordSplitAtItsAt() {
  const QString first="postgres://app:p@",second=QString("ss!w0")+"rd@db.internal:5432/app";
  QList<RecognizedWord> page;
  page.append({first,QRect(10,10,int(first.size())*8,20),90,0});
  page.append({second,QRect(10+int(first.size())*8+1,10,int(second.size())*8,20),90,0});
  QCOMPARE(found(page,QStringList{"key"}),QStringList{"key: "+first+" "+second});
  // ...digits first, then the rest glued: still a password, not a port (CodeRabbit on #1967).
  const QString digits="postgres://app:1234",rest=QString("q@x")+"y@db/app";
  QList<RecognizedWord> glued;
  glued.append({digits,QRect(10,10,int(digits.size())*8,20),90,0});
  glued.append({rest,QRect(10+int(digits.size())*8+1,10,int(rest.size())*8,20),90,0});
  QCOMPARE(found(glued,QStringList{"key"}),QStringList{"key: "+digits+" "+rest});
  // ...but an @ that starts a word after a space is another word: an address beside a URL is no password.
  const QString url="https://app:example",mail="@alice@example.com";
  QList<RecognizedWord> spaced;
  spaced.append({url,QRect(10,10,int(url.size())*8,20),90,0});
  spaced.append({mail,QRect(10+int(url.size())*8+8,10,int(mail.size())*8,20),90,0});
  QCOMPARE(found(spaced,QStringList{"key"}),QStringList{});
  // ...and a port before a word space keeps the next word's addresses out of it (Sol on #1967).
  const QString host="http://localhost:3000",list="a@b.co,c@d.co";
  QList<RecognizedWord> port;
  port.append({host,QRect(10,10,int(host.size())*8,20),90,0});
  port.append({list,QRect(10+int(host.size())*8+8,10,int(list.size())*8,20),90,0});
  QCOMPARE(found(port,QStringList{"key"}),QStringList{});
 }
 void seedPhraseBesideAnotherList() {
  const QStringList seed{"abandon","ability","able","about","above","absent","absorb","abstract","absurd","abuse","access","accident"};
  const QStringList shopping{"apples","bread","milk","eggs","butter","cheese","rice","beans","onions","garlic","lemons","coffee"};
  QList<RecognizedWord> page;
  const auto put=[&](const QString& text,int x,int line){page.append({text,QRect(x,line*40,int(text.size())*8,20),90,line});};
  for(int i=0;i<12;++i){put(QString::number(i+1)+".",20,i);put(shopping.at(i),44,i);}
  put("Secret",900,2);put("Recovery",956,2);put("Phrase",1028,2);
  for(int i=0;i<12;++i){const int row=3+i/3,col=i%3;put(QString::number(i+1)+".",900+col*200,row);put(seed.at(i),924+col*200,row);}
  QStringList expected;for(int i=0;i<12;++i)expected<<"key: "+QString::number(i+1)+". "+seed.at(i);
  QCOMPARE(found(page,QStringList{"key"}),expected);
  // ...and with the other window on the right, nothing beside the label, and a "0." of its own.
  QList<RecognizedWord> right;
  const auto at=[&](const QString& text,int x,int line){right.append({text,QRect(x,line*40,int(text.size())*8,20),90,line});};
  at("Secret",24,0);at("Recovery",80,0);at("Phrase",152,0);
  for(int i=0;i<12;++i){const int row=1+i/3,col=i%3;at(QString::number(i+1)+".",24+col*200,row);at(seed.at(i),48+col*200,row);}
  at("0.",900,2);at("note",924,2);for(int i=0;i<5;++i){at(QString::number(i+1)+".",900,3+i);at(shopping.at(i),924,3+i);}
  QCOMPARE(found(right,QStringList{"key"}),expected);
  // ...and that window's list going on past 24 (CodeRabbit on #1952).
  at("25.",900,8);at("note",924,8);
  QCOMPARE(found(right,QStringList{"key"}),expected);
  // The m74 and k72 page: the phrase one word a row under its label, a shopping list beside it under its own
  // heading, whose "10." to "12." start left of the single-digit numbers.
  QList<RecognizedWord> page2;
  const auto put2=[&](const QString& text,int x,int line,int width=-1){page2.append({text,QRect(x,line*26,width<0?int(text.size())*9:width,20),90,line});};
  put2("Secret",30,0);put2("Recovery",118,0);put2("Phrase",239,0);put2("Shopping",416,0);put2("list",542,0);
  for(int i=0;i<12;++i){put2(QString::number(i+1)+".",i<9?36:28,1+i);put2(seed.at(i),60,1+i);put2(QString::number(i+1)+".",i<9?424:411,1+i);put2(shopping.at(i),452,1+i);}
  QCOMPARE(found(page2,QStringList{"key"}),expected);
  // ...and a phrase column just left of the next window's heading stays the phrase's: only a two-digit number
  // beside that window's own one-digit numbers is the other list's (Sol on #1952).
  QList<RecognizedWord> page3;
  const auto put3=[&](const QString& text,int x,int line){page3.append({text,QRect(x,line*26,int(text.size())*9,20),90,line});};
  put3("Secret",30,0);put3("Recovery",92,0);put3("Phrase",173,0);put3("Notes",416,0);
  for(int i=0;i<12;++i){const int row=1+i/3,col=i%3;put3(QString::number(i+1)+".",30+col*185,row);put3(seed.at(i),54+col*185,row);}
  QCOMPARE(found(page3,QStringList{"key"}),expected);
 }
 // #1941: a code's content holds a secret Keys and tokens covers: a 2FA enrolment, a Wi-Fi network's
 // password, or a key the rules find. An open network, a link or plain text is none.
 void codesHoldingSecrets() {
  const auto seed=QString("JBSWY3DP")+"EHPK3PXP";
  QCOMPARE(quickRedact::codeSecret("otpauth://totp/Example:alice@example.com?secret="+seed+"&issuer=Example"),QString("otp"));
  QCOMPARE(quickRedact::codeSecret("otpauth-migration://offline?data="+seed),QString("otp"));
  QCOMPARE(quickRedact::codeSecret("WIFI:T:WPA;S:HomeNet;P:"+QString("orchard")+"\\;lantern;;"),QString("wifi"));
  // A later empty P or T:nopass does not clear a password a reader takes (Sol on #1952).
  QCOMPARE(quickRedact::codeSecret("WIFI:T:WPA;P:"+QString("orchard")+"-lantern;S:HomeNet;P:;T:nopass;;"),QString("wifi"));
  // An escaped semicolon in the network's name makes no password field of its own.
  QVERIFY(quickRedact::codeSecret("WIFI:T:WPA;S:Cafe\\;P:decoy;;")!=QString("wifi"));
  // The whole content is read, however many lines it has.
  QCOMPARE(quickRedact::codeSecret("header"+QString(401,'\n')+"PASSWORD="+QString("Crane")+"7Queue"),QString("key"));
  QCOMPARE(quickRedact::codeSecret("WIFI:T:nopass;S:Cafe;P:;;"),QString());
  QCOMPARE(quickRedact::codeSecret("WIFI:S:Cafe;T:nopass;;"),QString());
  QCOMPARE(quickRedact::codeSecret("GITHUB_TOKEN="+QString("gh")+"p_"+"aB3dE5fG7hJ9kL1mN3pQ5rS7tU9vW1xY3zA5"),QString("key"));
  QCOMPARE(quickRedact::codeSecret("https://example.com/menu"),QString());
  QCOMPARE(quickRedact::codeSecret("Table 12, ask for the wine list"),QString());
 }
 // #1618, decision 3: the account's names and the user's own list. Words
 // are literal, never a regex, compared ignoring case and accents.
 // Sol on #1826 (S17): two 8-digit phone numbers in table columns read as 4-4-4-4; the wide gap between the
 // columns keeps them two numbers, both found. The same groups evenly spaced are one card-shaped number.
 // #1939 (#1931 fuzz group 5): numbers stop at a column gap of more than about two characters, so table
 // and log columns are not one number; a card printed with wide, even gaps is still one card.
 void numbersStopAtColumnGaps() {
  const auto row=[](const QList<QPair<QString,int>>& cells){QList<RecognizedWord> out;int x=0;for(const auto& [text,gap]:cells){out.append({text,QRect(x,0,int(text.size())*8,20),90,0});x+=int(text.size())*8+gap;}return out;};
  // A week of a month view, cells three characters apart: neither a card (Luhn holds) nor a phone.
  QCOMPARE(found(row({{"19",24},{"20",24},{"21",24},{"22",24},{"23",24},{"24",24},{"25",24}}),all),QStringList{});
  // An access log's status and size in columns.
  QCOMPARE(found(row({{"200",40},{"5123",8}}),all),QStringList{});
  // Two spaces in a monospace font stay a join.
  QCOMPARE(found(row({{"4111",16},{"1111",16},{"1111",16},{"1111",8}}),QStringList{"card"}),QStringList{"card: 4111 1111 1111 1111"});
  // Wide, even gaps: one card.
  QCOMPARE(found(row({{"4111",40},{"1111",40},{"1111",40},{"1111",8}}),QStringList{"card"}),QStringList{"card: 4111 1111 1111 1111"});
  // A reference number a column before a card.
  QCOMPARE(found(row({{"12",60},{"4111",8},{"1111",8},{"1111",8},{"1111",8}}),QStringList{"card"}),QStringList{"card: 4111 1111 1111 1111"});
  // A card with one wider gap (a box a little off) is still a card (Sol on #1957).
  QCOMPARE(found(row({{"4111",8},{"1111",24},{"1111",8},{"1111",8}}),QStringList{"card"}),QStringList{"card: 4111 1111 1111 1111"});
  // The values of a free row sit in columns under free's header, each an unbroken count; the same row
  // without the header is a pane beside a phone column (Sol on #1957), and its numbers stay phones.
  {auto mem=row({{"Mem:",40},{"16303428",40},{"8123456",40},{"1234567",8}});
   QCOMPARE(found(mem,QStringList{"phone"}).size(),qsizetype(3));
   auto header=row({{"total",80},{"used",40},{"free",8}});
   for(auto& w:mem){w.line=1;w.rect.translate(0,30);}
   header.append(mem);
   QCOMPARE(found(header,all),QStringList{});
   // ...also with the label's colon read as its own word (m74: Mem : 32755180 3379508 ...).
   auto split=row({{"total",80},{"used",40},{"free",8}});
   auto mem2=row({{"Mem",4},{":",40},{"32755180",40},{"3379508",40},{"8842284",40},{"1013460",8}});
   for(auto& w:mem2){w.line=1;w.rect.translate(0,30);}
   split.append(mem2);
   QCOMPARE(found(split,all),QStringList{});}
  // A long numeric row stays fast (Sol on #1957: the card retry once grew with the cube of its length).
  {QList<QPair<QString,int>> zeros;for(int i=0;i<2048;++i)zeros.append({QStringLiteral("0000"),8});
   QElapsedTimer timer;timer.start();found(row(zeros),QStringList{"card","phone","ip"});QVERIFY2(timer.elapsed()<2000,qPrintable(QString::number(timer.elapsed())));}
  // A clipped cell's box overlapping the card, or touching it, is another field.
  QCOMPARE(found(row({{"6677",8},{"7068",-20},{"5193",8},{"8349",8},{"3122",8},{"9635",8}}),QStringList{"card"}),QStringList{"card: 5193 8349 3122 9635"});
  QCOMPARE(found(row({{"6011",8},{"0293",8},{"7603",8},{"0105",1},{"23",8}}),QStringList{"card"}),QStringList{"card: 6011 0293 7603 0105"});
 }
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
  QCOMPARE(found(words("KEY="+secret+" | Xc4Vb9Nm1QwEr3Ty8Ui5 = # development"),QStringList{"key"}),QStringList{"key: KEY="+secret+" Xc4Vb9Nm1QwEr3Ty8Ui5 ="});
  QCOMPARE(found(words("AWS_SECRET_ACCESS_KEY="+secret+" | DATABASE_URL = # dev"),QStringList{"key"}),QStringList{"key: AWS_SECRET_ACCESS_KEY="+secret});
  QCOMPARE(found(words("KEY=\""+secret+" | Xc4Vb9Nm //Q1WEr3Ty8Ui5==\""),QStringList{"key"}),QStringList{"key: KEY=\""+secret+" Xc4Vb9Nm //Q1WEr3Ty8Ui5==\""});
  QCOMPARE(found(words("AWS_SECRET_ACCESS_KEY="+secret+" | DATABASE_URL ="),QStringList{"key"}),QStringList{"key: AWS_SECRET_ACCESS_KEY="+secret});
  QCOMPARE(found(words("KEY="+secret+" | shaB7m2QaK8V= # dev"),QStringList{"key"}),QStringList{"key: KEY="+secret+" shaB7m2QaK8V="});
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
  // #1942: a prompt whose "@" recognition read as letters runs the login into the host.
  QTest::newRow("name: a prompt with its @ misread")<<"phudsonPd69:~$ ls | |lphudsonfidevbox:~/src$ | phudson.notes:~ | phudsonia"<<name<<me<<QStringList{"name: phudsonPd69:~$","name: |lphudsonfidevbox:~/src$","name: phudson.notes:~"};
  // Sol on #1956: an "@" that recognition kept names another login.
  QTest::newRow("name: another login's prompt")<<"mphudson@host:/tmp | phudsondev@host:/tmp"<<name<<me<<QStringList{};
  QTest::newRow("name: an @ in the prompt's path")<<"phudsonPd69:~/work@v2"<<name<<me<<QStringList{"name: phudsonPd69:~/work@v2"};
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
  // #1940 (Sol on #1954): many short segments on one row, each with a run, beside hash words.
  QTest::newRow("many separated runs beside hash words")<<repeat("sha256 ~~ 9f86d081884c7d659a2feaa0c55ad015 ~~ ",1500);
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
  // #1930: a word of letters far along the row is no lost last label (read off a bar chart, 648 px apart).
  const QList<RecognizedWord> far{{"s@525",QRect(237,1227,326,85),0,0},{"ia",QRect(1211,1226,118,86),52,0}};
  QCOMPARE(found(far,QStringList{"email"}),QStringList{});
  // ...nor after a legend's square colour swatch read as "W@W", its label beside it.
  const QList<RecognizedWord> swatch{{"W@W",QRect(343,597,25,25),47,0},{"Egress",QRect(384,604,80,29),92,0},{"$25,794",QRect(481,604,100,25),93,0}};
  QCOMPARE(found(swatch,QStringList{"email"}),QStringList{});
  // Narrow addresses that lost their dot are still restored (Sol on #1932, as Tesseract read them):
  // "i@i" 13 x 10 at 10 px, a numeric name "a@3", and "j@j" before a short "io" six columns on.
  const QList<RecognizedWord> narrow{{"i@i",QRect(0,0,13,10),90,0},{"io",QRect(17,2,8,8),90,0}};
  QCOMPARE(found(narrow,QStringList{"email"}),QStringList{"email: i@i io"});
  const QList<RecognizedWord> numeric{{"a@3",QRect(0,0,26,12),90,0},{"com",QRect(30,3,22,9),90,0}};
  QCOMPARE(found(numeric,QStringList{"email"}),QStringList{"email: a@3 com"});
  const QList<RecognizedWord> serif{{"j@j",QRect(0,0,16,12),90,0},{"io",QRect(22,3,8,6),90,0}};
  QCOMPARE(found(serif,QStringList{"email"}),QStringList{"email: j@j io"});
  // #1942: a word before an @-mention stands at least half a line's height off; it joins nothing.
  const QList<RecognizedWord> mention{{"thanks",QRect(0,0,48,20),90,0},{"@jane.doe",QRect(60,0,72,20),90,0}};
  QCOMPARE(found(mention,QStringList{"email"}),QStringList{});
  const QList<RecognizedWord> split{{"zoe.sato8",QRect(0,0,72,22),90,0},{"@wayne-corp.ai",QRect(78,0,112,22),90,0}};
  QCOMPARE(found(split,QStringList{"email"}),QStringList{"email: zoe.sato8 @wayne-corp.ai"});
  const QList<RecognizedWord> handle{{"@support.",QRect(0,0,72,20),90,0},{"jane+",QRect(80,0,40,20),90,0},{"tag@example.com",QRect(119,0,120,20),90,0}};
  QCOMPARE(found(handle,QStringList{"email"}),QStringList{"email: jane+ tag@example.com"});
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
