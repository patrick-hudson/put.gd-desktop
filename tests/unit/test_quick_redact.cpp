// Quick redact (#1428, decision 5): one table over the matching function. A
// row is recognized words in reading order ("|" starts a new line), the
// categories asked for, and every match expected as "category: words".
#include "editor/QuickRedact.h"
#include "fixtures/Waits.h"
#include <QElapsedTimer>
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
 // #1618, decision 3: the account's names and the user's own list. Words
 // are literal, never a regex, compared ignoring case and accents.
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
  QTest::newRow("pattern: lines join only on wrap evidence")<<"ticket ABC- | 99 | ticket ABC | 98"<<list<<patterns({R"(ABC-\n\d+)",R"(ABC\s+98)"})<<QStringList{"list: ABC- 99"};
  QTest::newRow("pattern: one that does not compile matches nothing")<<"JIRA-1"<<list<<patterns({"(abc"})<<QStringList{};
  QTest::newRow("pattern: Unicode properties")<<"Öffnung ÄRGER x"<<list<<patterns({R"(\b\p{Lu}{5}\b)"})<<QStringList{"list: ÄRGER"};
  // Round 1 (#1631), one row per finding.
  QTest::newRow("M1: a pattern's own limits are left out")<<"JIRA-1"<<list<<patterns({"(*LIMIT_MATCH=99999999)JIRA-\\d"})<<QStringList{};
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
 // M2: all patterns together end within the budget plus one bounded call,
 // even when every start position costs just under PCRE2's limit (its count
 // starts again at each position of one unanchored call).
 void patternTimeIsBounded() {
  // 8000 runs on one line: one unanchored call over it took 1.7 s on
  // m74, which only stepping through the start positions can interrupt.
  QString line;for(int i=0;i<8000;++i)line+=QString(14,'a')+"b ";
  const auto candidates=words(line+" | JIRA-3");
  QStringList slow;for(int i=0;i<20;++i)slow.append("(a+)+$");
  slow.prepend(R"(JIRA-\d)");
  constexpr int budget=300;
  QElapsedTimer clock;clock.start();
  const auto result=quickRedact::find(candidates,{"list"},patterns(slow),budget);
  // 14 a's keep every position just under the limit (15 cost 0.4 s a pattern
  // on m74, 16 reach it at once), so this ran until the budget...
  QVERIFY2(clock.elapsed()>=budget,qPrintable(QString::number(clock.elapsed())));
  // ...and the bound is the budget plus one match call; the slack is for a loaded runner.
  QVERIFY2(clock.elapsed()<budget+putgd::test::scaled(250),qPrintable(QString::number(clock.elapsed())));
  QCOMPARE(found(candidates,result),QStringList{"list: JIRA-3"});
  QVERIFY(result.skippedPatterns.contains(20));QVERIFY(!result.skippedPatterns.contains(0));
  // Short runs on one long line: before stepping, one call took a second.
  QString runs;for(int i=0;i<300;++i)runs+=QString(18,'a')+"b ";
  const auto many=words(runs+" TICKET9");
  clock.restart();
  const auto stopped=quickRedact::find(many,{"list"},patterns({"(a+)+$",R"(TICKET\d)"}),2000);
  QVERIFY2(clock.elapsed()<2000,qPrintable(QString::number(clock.elapsed())));
  QCOMPARE(found(many,stopped),QStringList{"list: TICKET9"});
 }
 // M2: progress after the terms and after each pattern, the rest skipped, so a
 // caller that stops waiting keeps what was found.
 void progressCarriesFinishedPatterns() {
  const auto candidates=words("Hammond filed JIRA-9 and TICKET4");
  QList<quickRedact::Result> seen;
  const auto result=quickRedact::find(candidates,{"list"},quickRedact::Sources{{},{},{"Hammond"},{R"(JIRA-\d)",R"(TICKET\d)"}},2000,[&](const quickRedact::Result& partial){seen.append(partial);});
  QCOMPARE(seen.size(),3);
  QCOMPARE(found(candidates,seen.at(0)),QStringList{"list: Hammond"});QCOMPARE(seen.at(0).skippedPatterns,(QList<int>{0,1}));
  QCOMPARE(found(candidates,seen.at(1)),(QStringList{"list: Hammond","list: JIRA-9"}));QCOMPARE(seen.at(1).skippedPatterns,QList<int>{1});
  QCOMPARE(seen.at(2).matches,result.matches);QCOMPARE(seen.at(2).skippedPatterns,QList<int>{});
 }
 void noBudgetSkipsEveryPattern() {
  const auto candidates=words("Hammond filed JIRA-9");
  const auto result=quickRedact::find(candidates,{"list"},quickRedact::Sources{{},{},{"Hammond"},{R"(JIRA-\d)","x"}},0);
  QCOMPARE(found(candidates,result),QStringList{"list: Hammond"});
  QCOMPARE(result.skippedPatterns,(QList<int>{0,1}));
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
