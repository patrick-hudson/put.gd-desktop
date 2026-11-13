// Quick redact (#1428, decision 5): one table over the matching function. A
// row is recognized words in reading order ("|" starts a new line), the
// categories asked for, and every match expected as "category: words".
#include "editor/QuickRedact.h"
#include <QtTest>
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
}
class QuickRedactTest final:public QObject {
 Q_OBJECT
private slots:
 void categoriesMatchTheirShapes_data() {
  QTest::addColumn<QString>("page");QTest::addColumn<QStringList>("categories");QTest::addColumn<QStringList>("expected");
  QTest::newRow("two phones on one line")<<"2125550100 4155550100"<<QStringList{"phone"}<<QStringList{"phone: 2125550100","phone: 4155550100"};
  const auto prefix=QString("gh")+"p_"+QString(36,'A'),tail=QString(10,'B');
  QTest::newRow("wrapped token with valid prefix")<<prefix+" | "+tail<<all<<QStringList{"key: "+prefix+" "+tail};
  QTest::newRow("email after unrelated numeric line")<<"ALPHA BRAVO CHARLIE | SYNTHETIC TEXT 1428 | hello@example.test"<<QStringList{"email"}<<QStringList{"email: hello@example.test"};
  QTest::newRow("email after prose line")<<"Please email | john@example.com"<<QStringList{"email"}<<QStringList{"email: john@example.com"};
  QTest::newRow("email hard wrapped at terminal column")<<"ABCDEFGHIJKLMNO | PQRSTUVWXYZABCD | Contact john.sm | ith@example.com"<<QStringList{"email"}<<QStringList{"email: john.sm ith@example.com"};
  QTest::newRow("email")<<"Write to jane.doe+invoices@example.co.uk today"<<all<<QStringList{"email: jane.doe+invoices@example.co.uk"};
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
  // Ordinary lines that resembled tokens or numbers once words were joined.
  QTest::newRow("a version is not a phone")<<"Release 26.1112.4660 for Ubuntu 24.04"<<all<<QStringList{};
  QTest::newRow("capitalized words with numbers are no token")<<"Scores: Team Alpha 3, Team Bravo 2, Team Charlie 0 after 90 minutes"<<all<<QStringList{};
  QTest::newRow("a hardware line is no token")<<"MacBookPro16,1 Intel Core i7-9750H 2.6GHz 6-Core 32GB DDR4 2666MHz"<<all<<QStringList{};
  QTest::newRow("a prefix inside a word is no token")<<"ghosts haunt the lighthouse and they Included 24 photos of 2026 for everyone"<<all<<QStringList{};
  QTest::newRow("Slack token")<<"chat "+slack+" here"<<all<<QStringList{"key: "+slack+" here"};
  QTest::newRow("Stripe key")<<stripe+" set"<<all<<QStringList{"key: "+stripe+" set"};
  QTest::newRow("JWT")<<"Bearer "+jwt<<all<<QStringList{"key: "+jwt};
  QTest::newRow("PEM private key block")<<begin+" | "+body+" | Zm9vYmFy "+end+" done"<<all<<QStringList{"key: "+begin+" "+body+" Zm9vYmFy "+end+" done"};
  QTest::newRow("hex digest run")<<"sha256 "+hexRun<<all<<QStringList{"key: sha256 "+hexRun};
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
};
QTEST_APPLESS_MAIN(QuickRedactTest)
#include "test_quick_redact.moc"
