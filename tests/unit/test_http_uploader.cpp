// Custom HTTP uploader rules (#1361): .sxcu files, both placeholder generations, the
// JSON path and XPath subsets, and settings validation. No network, no files.
#include "storage/http/HttpCommon.h"
#include "storage/http/HttpTemplate.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>
using namespace putgd;
using namespace putgd::http;
namespace {
const char* token = "PUTGD-SYNTHETIC-SXCU-TOKEN-0123456789";
QByteArray sxcu(QJsonObject object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
QJsonObject zipline() {
    return {{"Version", "17.0.0"}, {"Name", "Zipline"}, {"DestinationType", "ImageUploader, TextUploader, FileUploader"}, {"RequestMethod", "POST"},
            {"RequestURL", "https://files.example.test/api/upload"}, {"Headers", QJsonObject{{"authorization", token}, {"x-zipline-format", "random"}}},
            {"Body", "MultipartFormData"}, {"FileFormName", "file"}, {"URL", "{json:files[0].url}"}, {"ThumbnailURL", "{json:files[0].url}"},
            {"DeletionURL", "{json:deletesUrl}"}, {"ErrorMessage", "{json:error}"}};
}
QJsonObject row(const QString& name, const QJsonObject& settings, const QString& list) {
    for (const auto& value : settings[list].toArray()) if (value.toObject()["name"] == name) return value.toObject();
    return {};
}
QString quotes(QString text) { return text.replace('\'', '"'); }
ResponseValues reply(const QString& body, const QString& url = "https://files.example.test/api/upload") {
    return {body, {{"Location", "https://files.example.test/u/a.png"}, {"X-Delete-Key", "d3l"}, {"content-type", "application/json"}}, url, "shot one.png"};
}
}
class HttpUploaderRules : public QObject {
    Q_OBJECT
private slots:
    void templateSyntax_data() {
        QTest::addColumn<QString>("text");
        QTest::addColumn<bool>("request");
        QTest::addColumn<QString>("problem"); // a fragment of the expected problem; empty when usable
        QTest::newRow("plain") << "Bearer abc" << true << "";
        QTest::newRow("filename") << "{filename}" << true << "";
        QTest::newRow("nested base64") << "Basic {base64:user:{filename}}" << true << "";
        QTest::newRow("random") << "{random:a.example|b.example}" << true << "";
        QTest::newRow("random needs two") << "{random:only}" << true << "two choices";
        QTest::newRow("escaped braces") << "\\{not a placeholder\\}" << true << "";
        QTest::newRow("unclosed") << "{json:url" << false << "closing brace";
        QTest::newRow("empty name") << "{}" << false << "no name";
        QTest::newRow("unknown") << "{clipboard}" << true << "not supported";
        QTest::newRow("select is interactive") << "{select:a|b}" << true << "asks the user";
        QTest::newRow("prompt is interactive") << "{prompt:Title|default}" << true << "asks the user";
        QTest::newRow("inputbox is interactive") << "{inputbox:Title}" << true << "asks the user";
        QTest::newRow("outputbox is interactive") << "{outputbox:Result|{response}}" << false << "asks the user";
        QTest::newRow("json in request") << "{json:url}" << true << "reads the reply";
        QTest::newRow("input in response") << "{input}" << false << "{input}";
        QTest::newRow("json") << "{json:files[0].url}" << false << "";
        QTest::newRow("json with input") << "{json:{response}|data.link}" << false << "";
        QTest::newRow("json wildcard") << "{json:files[*].url}" << false << "JSON path";
        QTest::newRow("json recursive") << "{json:$..url}" << false << "JSON path";
        QTest::newRow("json filter") << "{json:files[?(@.ok)].url}" << false << "JSON path";
        QTest::newRow("xml") << "{xml:/files/file[1]/url}" << false << "";
        QTest::newRow("xml function") << "{xml:string(/a/b)}" << false << "XPath";
        QTest::newRow("xml axis") << "{xml:/a/following-sibling::b}" << false << "XPath";
        QTest::newRow("regex") << "{regex:href=\"(.+?)\"|1}" << false << "";
        QTest::newRow("regex invalid") << "{regex:(unclosed|1}" << false << "regular expression";
        QTest::newRow("header") << "{header:Location}" << false << "";
        QTest::newRow("header needs name") << "{header}" << false << "needs a parameter";
        QTest::newRow("response") << "{response}" << false << "";
        QTest::newRow("responseurl") << "{responseurl}" << false << "";
        QTest::newRow("case") << "{JSON:url}{ResponseURL}" << false << "";
        QTest::newRow("secret in request") << "https://h.test/hook/{secret:secrets/url-key}" << true << "";
        QTest::newRow("secret in response") << "{json:url}/delete/{secret:arguments/token}" << false << "";
        QTest::newRow("secret needs a name") << "{secret}" << false << "needs a parameter";
        QTest::newRow("secret takes one name") << "{secret:a|b}" << false << "one stored value";
        QTest::newRow("secret name is literal") << "{secret:{json:key}}" << false << "one stored value";
        // One parameter too many is a mistake to report, not something to ignore.
        QTest::newRow("header takes one name") << "{header:Location|extra}" << false << "at most 1 parameter";
        QTest::newRow("filename takes none") << "{filename:png}" << true << "takes no parameter";
        QTest::newRow("regex takes three") << "{regex:{response}|a(b)|1|2}" << false << "at most 3 parameters";
        QTest::newRow("json takes text and path") << "{json:{response}|url}" << false << "";
        QTest::newRow("random takes many") << "{random:a|b|c|d|e}" << true << "";
        // A reply value keeps its stored values named; inside another placeholder a name has nothing to stand for.
        QTest::newRow("nested secret in response") << "{json:url}/{base64:{secret:secrets/t}}" << false << "stands on its own";
        QTest::newRow("nested secret in request") << "{base64:{secret:secrets/t}}" << true << "";
    }
    void templateSyntax() {
        QFETCH(QString, text);
        QFETCH(bool, request);
        QFETCH(QString, problem);
        const auto found = templateProblem(text, request ? TemplateUse::Request : TemplateUse::Response);
        if (problem.isEmpty()) QVERIFY2(found.isEmpty(), qPrintable(found));
        else QVERIFY2(found.contains(problem), qPrintable(found));
        // A problem never repeats the value: it may hold a credential.
        if (text.size() > 12) QVERIFY(!found.contains(text));
    }
    void responseValues_data() {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("body");
        QTest::addColumn<QString>("expected");
        // moc misreads raw string literals here, so quotes() writes the double quotes.
        const QString json = quotes("{'files':[{'url':'https://h.test/u/a.png','id':7,'ok':true}],'data':{'link':'https://h.test/l','a b':'spaced'},'error':'quota'}");
        const QString xml = quotes("<?xml version='1.0'?><r:upload xmlns:r='urn:x'><file id='1'><url>https://h.test/1</url></file><file id='2' kind='main'><url>https://h.test/2</url><note> two </note></file><delete href='https://h.test/d'/></r:upload>");
        const QString html = quotes("<p>ok</p><a class='link' href='https://h.test/v/x1'>open</a> <a href='https://h.test/del/x1'>delete</a>");
        QTest::newRow("json array") << "{json:files[0].url}" << json << "https://h.test/u/a.png";
        QTest::newRow("json dollar") << "{json:$.data.link}" << json << "https://h.test/l";
        QTest::newRow("json bracket name") << "{json:data['a b']}" << json << "spaced";
        QTest::newRow("json number") << "https://h.test/f/{json:files[0].id}" << json << "https://h.test/f/7";
        QTest::newRow("json bool") << "{json:files[0].ok}" << json << "true";
        QTest::newRow("json missing") << "{json:files[1].url}" << json << "";
        QTest::newRow("json object is no text") << "{json:data}" << json << "";
        QTest::newRow("json not json") << "{json:url}" << "<html>" << "";
        QTest::newRow("json given input") << "{json:{response}|error}" << json << "quota";
        QTest::newRow("xml path") << "{xml:/upload/file/url}" << xml << "https://h.test/1";
        QTest::newRow("xml index") << "{xml:/upload/file[2]/url}" << xml << "https://h.test/2";
        QTest::newRow("xml last") << "{xml:/upload/file[last()]/url}" << xml << "https://h.test/2";
        QTest::newRow("xml attribute predicate") << "{xml:/upload/file[@kind='main']/url}" << xml << "https://h.test/2";
        QTest::newRow("xml descendant") << "{xml://note}" << xml << " two ";
        QTest::newRow("xml attribute") << "{xml:/upload/delete/@href}" << xml << "https://h.test/d";
        QTest::newRow("xml descendant attribute") << "{xml://file[2]/@id}" << xml << "2";
        QTest::newRow("xml text") << "{xml://file[1]/url/text()}" << xml << "https://h.test/1";
        QTest::newRow("xml prefix ignored") << "{xml:/r:upload/file/url}" << xml << "https://h.test/1";
        QTest::newRow("xml wildcard") << "{xml:/*/delete/@href}" << xml << "https://h.test/d";
        QTest::newRow("xml nested descendants once each") << "{xml://upload//url}" << xml << "https://h.test/1";
        QTest::newRow("xml any descendant") << "{xml://*//url}" << xml << "https://h.test/1";
        QTest::newRow("xml missing") << "{xml:/upload/nothing}" << xml << "";
        QTest::newRow("xml malformed") << "{xml:/a}" << "<a><b></a>" << "";
        QTest::newRow("regex group") << "{regex:href=\"([^\"]+)\"|1}" << html << "https://h.test/v/x1";
        // The backslash escapes the next character of a placeholder, so a regex needs it doubled.
        QTest::newRow("regex whole") << "{regex:https://h\\\\.test/del/\\\\w+}" << html << "https://h.test/del/x1";
        QTest::newRow("regex single backslash is an escape") << "{regex:del/\\w+}" << html << "";
        QTest::newRow("regex named") << "{regex:class=\"link\" href=\"(?<u>[^\"]+)\"|u}" << html << "https://h.test/v/x1";
        QTest::newRow("regex no match") << "{regex:nothing(here)|1}" << html << "";
        QTest::newRow("regex given input") << "{regex:{header:Location}|/u/(.+)$|1}" << html << "a.png";
        QTest::newRow("header") << "{header:location}" << "" << "https://files.example.test/u/a.png";
        QTest::newRow("header missing") << "{header:X-None}" << "" << "";
        QTest::newRow("response") << "{response}" << "https://h.test/raw\n" << "https://h.test/raw\n";
        QTest::newRow("responseurl") << "{responseurl}" << "" << "https://files.example.test/api/upload";
        QTest::newRow("filename is encoded") << "https://h.test/{filename}" << "" << "https://h.test/shot%20one.png";
        QTest::newRow("mixed") << "{json:data.link}?d={header:X-Delete-Key}" << json << "https://h.test/l?d=d3l";
        QTest::newRow("escape") << "\\{json:url\\}" << json << "{json:url}";
    }
    void responseValues() {
        QFETCH(QString, text);
        QFETCH(QString, body);
        QFETCH(QString, expected);
        QVERIFY2(templateProblem(text, TemplateUse::Response).isEmpty(), qPrintable(templateProblem(text, TemplateUse::Response)));
        QCOMPARE(renderResponse(text, reply(body)), expected);
    }
    void requestValues() {
        RequestValues values;
        values.fileName = "a \"b\".png";
        QCOMPARE(renderRequest("name={filename}", values), QString("name=a \"b\".png"));
        values.encode = RequestValues::Encode::Url;
        QCOMPARE(renderRequest("/put/{filename}", values), QString("/put/a%20%22b%22.png"));
        values.encode = RequestValues::Encode::Json;
        QCOMPARE(renderRequest("{filename}", values), QString("a \\\"b\\\".png"));
        values.encode = RequestValues::Encode::None;
        QCOMPARE(renderRequest("Basic {base64:user:pass}", values), QString("Basic dXNlcjpwYXNz"));
        const auto picked = renderRequest("{random:one|two|three}", values);
        QVERIFY(QStringList({"one", "two", "three"}).contains(picked));
        // {input} needs text; a file that is not text reports it instead of sending nothing.
        QString problem;
        QCOMPARE(renderRequest("{input}", values, &problem), QString());
        QVERIFY(problem.contains("{input}"));
        values.input = "line one\n<two> & \"three\"";
        values.hasInput = true;
        problem.clear();
        QCOMPARE(renderRequest("{input}", values, &problem), values.input);
        QVERIFY(problem.isEmpty());
        values.encode = RequestValues::Encode::Json;
        QCOMPARE(renderRequest("{input}", values), QString("line one\\n<two> & \\\"three\\\""));
        values.encode = RequestValues::Encode::Xml;
        QCOMPARE(renderRequest("{input}", values), QString("line one\n&lt;two&gt; &amp; &quot;three&quot;"));
        QVERIFY(readsInput("x {base64:{input}}"));
        QVERIFY(!readsInput("{filename}"));
    }
    void secretReferencesStayNamesUntilTheyAreNeeded() {
        const QString deletion = "{json:url}/delete/{secret:arguments/token}";
        QCOMPARE(secretReferences(deletion), QStringList({"arguments/token"}));
        QCOMPARE(secretReferences("{json:url}"), QStringList());
        QCOMPARE(secretReferences("{base64:{secret:headers/X}}:{secret:secrets/k}"), QStringList({"headers/X", "secrets/k"}));
        const QHash<QString, QString> stored{{"arguments/token", "tok_ABC123"}};
        auto values = reply(quotes("{'url':'https://h.test/u/a|b'}"));
        // Recorded form: the reply's part is filled in and escaped, the stored value is still a name.
        values.keepSecrets = true;
        const auto recorded = renderResponse(deletion, values);
        QCOMPARE(recorded, QString("https://h.test/u/a\\|b/delete/{secret:arguments/token}"));
        QVERIFY(!recorded.contains("tok_ABC123"));
        // Filled in only when it is sent.
        ResponseValues sending;
        sending.secrets = &stored;
        QCOMPARE(renderResponse(recorded, sending), QString("https://h.test/u/a|b/delete/tok_ABC123"));
        // Without stored values and without keeping names, nothing is made up.
        QCOMPARE(renderResponse(deletion, reply(quotes("{'url':'https://h.test/u/a'}"))), QString("https://h.test/u/a/delete/"));
        // A request names it the same way, and says when the value is missing.
        RequestValues request;
        request.secrets = &stored;
        QString problem;
        QCOMPARE(renderRequest("/hook/{secret:arguments/token}", request, &problem), QString("/hook/tok_ABC123"));
        QVERIFY(problem.isEmpty());
        renderRequest("/hook/{secret:secrets/other}", request, &problem);
        QVERIFY(problem.contains("missing"));
    }
    void importNamesARepeatedTokenInsteadOfRepeatingIt() {
        // XBackBone 3.8.2 writes its upload token into the deletion URL as well.
        const QString key = "token_1a2b3c4d5e6f7a8b9c0d1e2f3a4b5c6d";
        const QJsonObject file{{"Version", "16.1.0"}, {"Name", "admin-ShareX"}, {"DestinationType", "ImageUploader, TextUploader, FileUploader"}, {"RequestMethod", "POST"},
                               {"RequestURL", "https://media.example.test/upload"}, {"Body", "MultipartFormData"},
                               {"Arguments", QJsonObject{{"file", "{filename}"}, {"text", "{input}"}, {"token", key}}}, {"FileFormName", "upload"},
                               {"URL", "{json:url}"}, {"ThumbnailURL", "{json:url}/raw"}, {"DeletionURL", "{json:url}/delete/" + key}};
        const auto result = importSxcu(sxcu(file));
        QVERIFY2(!result.error, qPrintable(result.error.message));
        const auto s = result.settings;
        QCOMPARE(s["deletionUrl"].toString(), QString("{json:url}/delete/{secret:arguments/token}"));
        QCOMPARE(s["thumbnailUrl"].toString(), QString("{json:url}/raw"));
        QCOMPARE(row("file", s, "arguments"), QJsonObject({{"name", "file"}, {"secret", false}, {"value", "{filename}"}}));
        QCOMPARE(row("text", s, "arguments"), QJsonObject({{"name", "text"}, {"secret", false}, {"value", "{input}"}}));
        QCOMPARE(row("token", s, "arguments"), QJsonObject({{"name", "token"}, {"secret", true}}));
        QCOMPARE(result.secrets.keys(), QStringList({"arguments/token"}));
        QVERIFY(!QJsonDocument(s).toJson().contains(key.toUtf8()));
        QVERIFY2(!validateSettings(s), qPrintable(validateSettings(s).message));
        // A key that appears only inside an address becomes a secret value of its own.
        const QJsonObject hook{{"Version", "17.0.0"}, {"DestinationType", "FileUploader"}, {"RequestURL", "https://hooks.example.test/api/upload/9f8e7d6c5b4a39281706f5e4d3c2b1a0"},
                               {"Body", "Binary"}, {"URL", "{json:url}"}, {"DeletionURL", "https://hooks.example.test/delete/{json:id}/a1b2c3d4e5f60718293a4b5c6d7e8f90"}};
        const auto hooked = importSxcu(sxcu(hook));
        QVERIFY2(!hooked.error, qPrintable(hooked.error.message));
        QCOMPARE(hooked.settings["requestUrl"].toString(), QString("https://hooks.example.test/api/upload/{secret:secrets/url-key}"));
        QCOMPARE(hooked.settings["deletionUrl"].toString(), QString("https://hooks.example.test/delete/{json:id}/{secret:secrets/deletion-key-2}"));
        QCOMPARE(hooked.settings["secrets"].toArray(), QJsonArray({QJsonObject{{"name", "url-key"}, {"secret", true}}, QJsonObject{{"name", "deletion-key-2"}, {"secret", true}}}));
        QCOMPARE(hooked.secrets.value("secrets/url-key"), QString("9f8e7d6c5b4a39281706f5e4d3c2b1a0"));
        QCOMPARE(hooked.secrets.value("secrets/deletion-key-2"), QString("a1b2c3d4e5f60718293a4b5c6d7e8f90"));
        const auto bytes = QJsonDocument(hooked.settings).toJson();
        QVERIFY(!bytes.contains("9f8e7d6c") && !bytes.contains("a1b2c3d4"));
        QVERIFY2(!validateSettings(hooked.settings), qPrintable(validateSettings(hooked.settings).message));
        // An ordinary path stays as written.
        auto plain = hook;
        plain["RequestURL"] = "https://hooks.example.test/api/v2/files/upload";
        plain["DeletionURL"] = "{json:deletion_url}";
        const auto kept = importSxcu(sxcu(plain));
        QCOMPARE(kept.settings["requestUrl"].toString(), QString("https://hooks.example.test/api/v2/files/upload"));
        QVERIFY(kept.settings["secrets"].toArray().isEmpty());
        QVERIFY(kept.secrets.isEmpty());
    }
    // A stored value is named only where it stands as literal text after the host, and it
    // never reaches what is shown or recorded: the link, its thumbnail, an error message.
    void importNamesStoredValuesOnlyAsLiteralText() {
        const QString key = token;
        QJsonObject file{{"Version", "17.0.0"}, {"DestinationType", "ImageUploader"}, {"RequestMethod", "POST"}, {"RequestURL", "https://media.example.test/upload/images"},
                         {"Body", "MultipartFormData"}, {"FileFormName", "file"},
                         {"Arguments", QJsonObject{{"domain", "media.example.test"}, {"folder", "images"}, {"token", key}}},
                         {"URL", "https://media.example.test/{json:images.url}"}, {"ThumbnailURL", "{json:thumb}?t=" + key}, {"ErrorMessage", "{json:error} (" + key + ")"},
                         {"DeletionURL", "https://media.example.test/delete/{json:images.id}/" + key}};
        const auto result = importSxcu(sxcu(file));
        QVERIFY2(!result.error, qPrintable(result.error.message));
        // The host is no key, a placeholder's own text is left alone, and an ordinary word
        // that a row also holds ("images", the folder) stays an ordinary word.
        QCOMPARE(result.settings["requestUrl"].toString(), QString("https://media.example.test/upload/images"));
        QCOMPARE(result.settings["responseUrl"].toString(), QString("https://media.example.test/{json:images.url}"));
        QCOMPARE(result.settings["deletionUrl"].toString(), QString("https://media.example.test/delete/{json:images.id}/{secret:arguments/token}"));
        // What would be shown or recorded is left out and said so, without the value.
        QCOMPARE(result.settings["thumbnailUrl"].toString(), QString());
        QCOMPARE(result.settings["errorMessage"].toString(), QString());
        const auto notes = result.notes.join(' ');
        QVERIFY2(notes.contains("ThumbnailURL repeated the value of argument token") && notes.contains("ErrorMessage repeated the value of argument token"), qPrintable(notes));
        QVERIFY(!notes.contains(key));
        QVERIFY(!QJsonDocument(result.settings).toJson().contains(key.toUtf8()));
        QVERIFY2(!validateSettings(result.settings), qPrintable(validateSettings(result.settings).message));
        // The link itself cannot be left out: the file is refused, naming the row and never its value.
        auto shared = file;
        shared["URL"] = "https://media.example.test/{json:name}?t=" + key;
        const auto refused = importSxcu(sxcu(shared));
        QCOMPARE(refused.error.code, QString("sxcu_unsupported"));
        QVERIFY2(refused.error.message.contains("URL repeats the value of argument token"), qPrintable(refused.error.message));
        QVERIFY(!refused.error.message.contains(key));
        QVERIFY(refused.secrets.isEmpty() && refused.settings.isEmpty());
        // More key-like runs than a destination can store: refused, not left in the address.
        auto crowded = file;
        QString address = "https://media.example.test/delete";
        for (int i = 0; i < maximumRows + 2; ++i) address += QString("/k%1abcdefghijklmnopqrstuvwxyz").arg(i, 2, 10, QChar('0'));
        crowded["DeletionURL"] = address;
        const auto tooMany = importSxcu(sxcu(crowded));
        QCOMPARE(tooMany.error.code, QString("sxcu_unsupported"));
        QVERIFY2(tooMany.error.message.contains("more key-like values"), qPrintable(tooMany.error.message));
        // A numeric type with a bit put.gd does not know is not quietly dropped.
        auto typed = file;
        typed["DestinationType"] = 65;
        QCOMPARE(importSxcu(sxcu(typed)).error.code, QString("sxcu_invalid"));
        typed["DestinationType"] = 5;
        QCOMPARE(importSxcu(sxcu(typed)).settings["accepts"].toArray(), QJsonArray({"image", "file"}));
    }
    // By hand as well: only the deletion URL may name a stored value among the reply values.
    void storedValuesStayOutOfWhatIsShownOrRecorded() {
        auto settings = defaults();
        settings["requestUrl"] = "https://h.example.test/up";
        settings["arguments"] = QJsonArray{QJsonObject{{"name", "token"}, {"secret", true}}};
        settings["deletionUrl"] = "{json:url}/delete/{secret:arguments/token}";
        QVERIFY2(!validateSettings(settings), qPrintable(validateSettings(settings).message));
        for (const auto* field : {"responseUrl", "thumbnailUrl", "errorMessage"}) {
            auto shown = settings;
            shown[field] = "{json:url}?t={secret:arguments/token}";
            const auto error = validateSettings(shown);
            QVERIFY2(error.message.contains("must not contain a stored secret"), qPrintable(QString(field) + ": " + error.message));
            QCOMPARE(error.details["field"].toString(), QString(field));
        }
    }
    // Review of #1371: a server can repeat a stored value in any spelling.
    void serverTextLosesEverySpellingOfAStoredValue() {
        const QString spelled = "pg-synthetic/value+with=signs&01";
        const QHash<QString, QString> stored{{"headers/Authorization", "Bearer " + spelled}, {"arguments/mode", "json"}, {"arguments/flag", "1"}, {"arguments/pin", "k4x9"}};
        const auto percent = QString::fromLatin1(QUrl::toPercentEncoding(spelled));
        for (const auto& spelling : QStringList{spelled, percent, percent.toLower(), QString(spelled).replace("/", "\\/"), QString(spelled).replace("/", "%2f"), QString(spelled).replace("+", "%2B").replace("/", "\\/")}) {
            const auto scrubbed = scrubStoredValues("refused: " + spelling + " is not valid", stored);
            QVERIFY2(!scrubbed.contains(spelling) && !scrubbed.contains(spelled), qPrintable(scrubbed));
            QCOMPARE(scrubbed, QString("refused: <redacted> is not valid"));
        }
        // The whole stored value goes as one. A setting such as "json" or "1" is no credential
        // and stays readable; a four-character value of a row named like a credential goes.
        QCOMPARE(scrubStoredValues("Bearer " + spelled + " sent bad json at line 1", stored), QString("<redacted> sent bad json at line 1"));
        QCOMPARE(scrubStoredValues("pin k4x9 is wrong", stored), QString("pin <redacted> is wrong"));
        // Encoded twice: there is nothing to rewrite in place, so the text is withheld.
        const auto twice = QString::fromLatin1(QUrl::toPercentEncoding(percent));
        const auto withheld = scrubStoredValues("refused: " + twice, stored);
        QVERIFY2(withheld.startsWith("<withheld") && !withheld.contains(twice), qPrintable(withheld));
        QCOMPARE(scrubStoredValues("nothing of ours here", stored), QString("nothing of ours here"));
    }
    void storedValuesInAddresses_data() {
        QTest::addColumn<QString>("address");
        QTest::addColumn<QString>("key"); // the stored value it holds; empty for none
        const QString t = token;
        const QString spelled = "pg-synthetic/value+with=signs&01";
        QTest::newRow("clean") << "https://cdn.example.test/u/a.png" << "";
        QTest::newRow("host is where it points") << "https://media.example.test/u/a.png" << "";
        QTest::newRow("inside a longer word") << "https://h.example.test/uploads/a.png" << "";
        QTest::newRow("an ordinary stored word") << "https://h.example.test/upload/a.png" << "";
        QTest::newRow("a short credential as a whole word") << "https://h.example.test/a.png?p=k4x9" << "arguments/pin";
        QTest::newRow("a short credential inside a word") << "https://h.example.test/xk4x9y.png" << "";
        QTest::newRow("under six characters") << "https://h.example.test/a.png?public=true" << "";
        QTest::newRow("query value") << "https://h.example.test/get?k=" + t + "&file=a" << "arguments/token";
        QTest::newRow("long value anywhere") << "https://h.example.test/x" + t + "y/a.png" << "arguments/token";
        QTest::newRow("percent-encoded") << "https://h.example.test/get?k=" + QString::fromLatin1(QUrl::toPercentEncoding(spelled)) << "headers/Authorization";
        QTest::newRow("percent-encoded lower case") << "https://h.example.test/get?k=" + QString::fromLatin1(QUrl::toPercentEncoding(spelled)).toLower() << "headers/Authorization";
        QTest::newRow("encoded in part") << "https://h.example.test/get?k=" + QString(spelled).replace("/", "%2f") << "headers/Authorization";
        QTest::newRow("encoded twice") << "https://h.example.test/get?k=" + QString::fromLatin1(QUrl::toPercentEncoding(QString::fromLatin1(QUrl::toPercentEncoding(spelled)))) << "headers/Authorization";
        QTest::newRow("path from the root") << "/get/" + t << "arguments/token";
        QTest::newRow("fragment") << "https://h.example.test/a.png#" + t << "arguments/token";
    }
    void storedValuesInAddresses() {
        QFETCH(QString, address);
        QFETCH(QString, key);
        const QHash<QString, QString> stored{{"arguments/token", token}, {"arguments/kind", "upload"}, {"arguments/flag", "true"}, {"arguments/domain", "media.example.test"},
                                             {"arguments/pin", "k4x9"}, {"headers/Authorization", "Bearer pg-synthetic/value+with=signs&01"}};
        QCOMPARE(storedValueIn(address, stored), key);
    }
    // Second review of #1371. What is looked for is what is credential-like, not every
    // stored value: an imported argument such as a folder name is stored by default, and
    // a link that repeats it is an ordinary link. A key inside a stored body or a cookie
    // is found although it is only part of what is stored.
    void onlyCredentialsAreLookedFor() {
        const QString inBody = "putgdsyntheticbodypart0001", inCookie = "putgdsyntheticcookie0002";
        const QHash<QString, QString> stored{{"arguments/folder", "screenshots"}, {"arguments/album", "holiday-pictures"}, {"headers/X-Format", "random-name"},
                                             {"data", "{\"apikey\":\"" + inBody + "\",\"folder\":\"shots\",\"password\":\"hunter2x\"}"},
                                             {"headers/Cookie", "session=" + inCookie + "; path=/"}};
        // Ordinary words pass, in an address and in a server's message.
        QCOMPARE(storedValueIn("https://h.example.test/screenshots/abc.png", stored), QString());
        QCOMPARE(storedValueIn("https://h.example.test/u/holiday-pictures/random-name.png", stored), QString());
        QCOMPARE(scrubStoredValues("folder screenshots not found", stored), QString("folder screenshots not found"));
        // A key that is part of a stored body or of a cookie is found by itself.
        QCOMPARE(storedValueIn("https://h.example.test/f/a.png?k=" + inBody, stored), QString("data"));
        QCOMPARE(storedValueIn("https://h.example.test/f/" + inCookie + "/a.png", stored), QString("headers/Cookie"));
        QCOMPARE(scrubStoredValues("key " + inBody + " is not valid", stored), QString("key <redacted> is not valid"));
        QCOMPARE(scrubStoredValues("session " + inCookie + " expired", stored), QString("session <redacted> expired"));
        // So is a short value that the stored text itself names as a credential.
        QCOMPARE(scrubStoredValues("wrong password hunter2x", stored), QString("wrong password <redacted>"));
        QCOMPARE(storedValueIn("https://h.example.test/f/a.png?p=hunter2x", stored), QString("data"));
        // A name decides only when it says "credential": an author is not an authorization,
        // keywords are not a key, and a switch under a credential's name is a switch.
        const QHash<QString, QString> named{{"arguments/author", "alice"}, {"arguments/keywords", "holiday"}, {"headers/Authorization", "tk91"}, {"parameters/api_key", "zq84"},
                                            {"arguments/upload_token", "true"}, {"data", "{\"use_token\": true, \"token_ttl\": 3600, \"auth\": \"Bearer w7m2\"}"}};
        QCOMPARE(storedValueIn("https://h.example.test/alice/holiday/true/3600/a.png", named), QString());
        QCOMPARE(storedValueIn("https://h.example.test/a.png?a=tk91", named), QString("headers/Authorization"));
        QCOMPARE(storedValueIn("https://h.example.test/zq84/a.png", named), QString("parameters/api_key"));
        QCOMPARE(scrubStoredValues("author alice sent Bearer w7m2 with keywords holiday, ttl 3600, true", named), QString("author alice sent <redacted> with keywords holiday, ttl 3600, true"));
        // The token of "Bearer <token>" is found alone, however short.
        QCOMPARE(scrubStoredValues("token w7m2 is not valid", named), QString("token <redacted> is not valid"));
        // And a key written in base64, either alphabet, wherever it starts in the text.
        for (const auto& prefix : QStringList{"", "a", "ab"})
            for (const auto options : {QByteArray::Base64Encoding, QByteArray::Base64UrlEncoding}) {
                const auto encoded = QString::fromLatin1((prefix + inBody + "!").toUtf8().toBase64(options | QByteArray::OmitTrailingEquals));
                QVERIFY2(!storedValueIn("https://h.example.test/f/a.png?k=" + encoded, stored).isEmpty(), qPrintable(encoded));
                QVERIFY2(!scrubStoredValues("refused " + encoded, stored).contains(encoded), qPrintable(encoded));
            }
    }
    // The import follows the same rule: a row's ordinary word may stand in the link.
    void importKeepsOrdinaryWordsOrdinary() {
        const QJsonObject file{{"Version", "17.0.0"}, {"DestinationType", "ImageUploader"}, {"RequestMethod", "POST"}, {"RequestURL", "https://media.example.test/upload/screenshots"},
                               {"Body", "MultipartFormData"}, {"FileFormName", "file"}, {"Arguments", QJsonObject{{"folder", "screenshots"}, {"token", token}}},
                               {"URL", "https://media.example.test/screenshots/{json:name}"}, {"ThumbnailURL", "https://media.example.test/screenshots/thumb/{json:name}"},
                               {"DeletionURL", "https://media.example.test/screenshots/delete/{json:id}"}};
        const auto result = importSxcu(sxcu(file));
        QVERIFY2(!result.error, qPrintable(result.error.message));
        QCOMPARE(result.settings["requestUrl"].toString(), QString("https://media.example.test/upload/screenshots"));
        QCOMPARE(result.settings["responseUrl"].toString(), QString("https://media.example.test/screenshots/{json:name}"));
        QCOMPARE(result.settings["thumbnailUrl"].toString(), QString("https://media.example.test/screenshots/thumb/{json:name}"));
        QCOMPARE(result.settings["deletionUrl"].toString(), QString("https://media.example.test/screenshots/delete/{json:id}"));
        // The folder is still stored, as every imported argument is: it is only not looked for.
        QCOMPARE(result.secrets.value("arguments/folder"), QString("screenshots"));
        // A part of a credential written out in an address, here the token of "Bearer <token>",
        // cannot be named by its row. It becomes a stored value of its own, and the note says so.
        const QJsonObject bearer{{"Version", "17.0.0"}, {"DestinationType", "ImageUploader"}, {"RequestMethod", "POST"}, {"RequestURL", "https://media.example.test/upload"},
                                 {"Body", "MultipartFormData"}, {"FileFormName", "file"}, {"Headers", QJsonObject{{"Authorization", QString("Bearer ") + token}}},
                                 {"URL", "{json:url}"}, {"DeletionURL", QString("https://media.example.test/delete/{json:id}?t=") + token}};
        const auto parted = importSxcu(sxcu(bearer));
        QVERIFY2(!parted.error, qPrintable(parted.error.message));
        QCOMPARE(parted.settings["deletionUrl"].toString(), QString("https://media.example.test/delete/{json:id}?t={secret:secrets/deletion-key}"));
        QCOMPARE(parted.secrets.value("secrets/deletion-key"), QString(token));
        QVERIFY(!QJsonDocument(parted.settings).toJson().contains(token));
        QVERIFY2(parted.notes.join(' ').contains("taken for a key and is now stored as deletion-key"), qPrintable(parted.notes.join(' ')));
        QVERIFY2(!validateSettings(parted.settings), qPrintable(validateSettings(parted.settings).message));
    }
    // An address with no path ends its host at "?" or "#": what follows is scanned too.
    void importScansAnAddressWithNoPath() {
        const QString run = "0123456789abcdef0123";
        QJsonObject file{{"Version", "17.0.0"}, {"DestinationType", "ImageUploader"}, {"RequestMethod", "POST"}, {"RequestURL", "https://media.example.test/upload"},
                         {"Body", "Binary"}, {"URL", "{json:url}"}, {"DeletionURL", QString("https://i.example.test?k=") + run}};
        auto result = importSxcu(sxcu(file));
        QVERIFY2(!result.error, qPrintable(result.error.message));
        QCOMPARE(result.settings["deletionUrl"].toString(), QString("https://i.example.test?k={secret:secrets/deletion-key}"));
        QCOMPARE(result.secrets.value("secrets/deletion-key"), run);
        QVERIFY(!QJsonDocument(result.settings).toJson().contains(run.toUtf8()));
        file["DeletionURL"] = QString("https://i.example.test#") + run;
        result = importSxcu(sxcu(file));
        QVERIFY(!QJsonDocument(result.settings).toJson().contains(run.toUtf8()));
        // A stored token repeated right after the host is named there as well.
        file["Arguments"] = QJsonObject{{"token", token}};
        file["Body"] = "MultipartFormData";
        file["DeletionURL"] = QString("https://i.example.test?t=") + token;
        result = importSxcu(sxcu(file));
        QCOMPARE(result.settings["deletionUrl"].toString(), QString("https://i.example.test?t={secret:arguments/token}"));
    }
    void deletionAddressNamesAStoredValueOnlyWhenItIsExact() {
        const QString t = token;
        const QHash<QString, QString> stored{{"parameters/key", t}, {"headers/Authorization", "Bearer other-synthetic-value-01"}};
        const QString address = "https://h.example.test/delete/7?k=" + t;
        const auto named = nameStoredValues(address, stored);
        QCOMPARE(named, QString("https://h.example.test/delete/7?k={secret:parameters/key}"));
        // It renders back to the address when the value is at hand, and holds none without it.
        ResponseValues filled;
        filled.secrets = &stored;
        QCOMPARE(renderResponse(named, filled), address);
        QCOMPARE(secretReferences(named), QStringList({"parameters/key"}));
        // A word of a stored value, another spelling beside an exact one, or nothing at all: no name.
        QCOMPARE(nameStoredValues("https://h.example.test/delete/7?k=other-synthetic-value-01", stored), QString());
        QCOMPARE(nameStoredValues(address + "&again=" + QString(t).replace("-", "%2D"), stored), QString());
        QCOMPARE(nameStoredValues("https://h.example.test/delete/7", stored), QString());
    }
    void thisMachineInEverySpelling_data() {
        QTest::addColumn<QString>("host");
        QTest::addColumn<bool>("loopback");
        for (const auto* host : {"localhost", "LOCALHOST", "localhost.", "api.localhost", "127.0.0.1", "127.9.8.7", "127.1", "2130706433", "0x7f000001", "0177.0.0.1", "0x7f.1",
                                 "::1", "[::1]", "0:0:0:0:0:0:0:1", "::", "0.0.0.0", "0", "::ffff:127.0.0.1",
                                 // The same mapped address written in hexadecimal groups, and in full.
                                 "::ffff:7f00:1", "[::ffff:7f00:1]", "0:0:0:0:0:ffff:7f00:1", "::FFFF:7F00:0001", "::ffff:0:0", "::7f00:1", "::127.0.0.1"})
            QTest::newRow(host) << host << true;
        for (const auto* host : {"192.168.1.5", "example.test", "128.0.0.1", "1.2.3.4", "::2", "2001:db8::1", "localhost.example.test", "notlocalhost", "127.0.0.1.example.test", "",
                                 "::ffff:c0a8:105", "::ffff:192.168.1.5", "2001:db8::7f00:1", "::ffff:7f00:1:1", "1::1"})
            QTest::newRow(*host ? host : "empty") << host << false;
    }
    void thisMachineInEverySpelling() {
        QFETCH(QString, host);
        QFETCH(bool, loopback);
        QCOMPARE(loopbackAddress(host), loopback);
    }
    // Where a name leads is the resolver's answer, not its spelling: these all lead here.
    void namesThatResolveToThisMachine() {
        const QStringList own{"127.0.0.1", "::1", "192.168.68.74", "fe80::1c2d:3e4f:5a6b:7c8d%enp3s0", "2001:db8:5::74"};
        // name, what it resolves to, whether that is this machine
        const QList<std::tuple<QString, QStringList, bool>> cases{
            {"ip6-localhost", {"::1"}, true}, {"ip6-loopback", {"::1"}, true}, {"localhost4", {"127.0.0.1"}, true}, {"localhost6", {"::1"}, true},
            {"localhost.localdomain", {"127.0.0.1"}, true}, {"mint-builder", {"127.0.1.1"}, true}, {"localtest.me", {"127.0.0.1"}, true},
            {"mapped.example.test", {"::ffff:7f00:1"}, true}, {"lan-name.example.test", {"192.168.68.74"}, true}, {"v6-name.example.test", {"2001:0db8:0005:0000:0000:0000:0000:0074"}, true},
            {"link-local.example.test", {"fe80::1c2d:3e4f:5a6b:7c8d"}, true}, {"one-of-two.example.test", {"203.0.113.9", "127.0.0.1"}, true}, {"zero.example.test", {"0.0.0.0"}, true},
            {"192.168.68.74", {}, true}, {"[2001:db8:5::74]", {}, true}, {"::ffff:192.168.68.74", {}, true},
            {"cdn.example.test", {"203.0.113.9", "2001:db8:9::1"}, false}, {"192.168.68.75", {}, false}, {"other.example.test", {"192.168.68.75"}, false}, {"unresolved.example.test", {}, false}};
        for (const auto& [host, resolved, here] : cases) QVERIFY2(leadsToThisMachine(host, resolved, own) == here, qPrintable(host));
    }
    void addressesAReplyMayName() {
        const QUrl far("https://public.example.test"), near("http://127.0.0.1:9000");
        QVERIFY(returnedAddressProblem(QUrl("https://cdn.example.test/a.png"), far).isEmpty());
        QVERIFY(returnedAddressProblem(QUrl("https://shown.example.test@real.example.test/a.png"), far).contains("user name"));
        QVERIFY(returnedAddressProblem(QUrl("http://127.0.0.1:8080/admin"), far).contains("this computer"));
        QVERIFY(returnedAddressProblem(QUrl("http://localhost:8080/admin"), far).contains("this computer"));
        QVERIFY(returnedAddressProblem(QUrl("http://[::1]:8080/admin"), far).contains("this computer"));
        // However the URL class writes a mapped address back, it is this computer.
        QVERIFY2(returnedAddressProblem(QUrl("http://[::ffff:7f00:1]:8080/admin"), far).contains("this computer"), qPrintable(QUrl("http://[::ffff:7f00:1]:8080/admin").host()));
        QVERIFY2(returnedAddressProblem(QUrl("http://[0:0:0:0:0:ffff:127.0.0.1]/admin"), far).contains("this computer"), qPrintable(QUrl("http://[0:0:0:0:0:ffff:127.0.0.1]/admin").host()));
        // A destination on this machine may be answered with addresses on this machine.
        QVERIFY(returnedAddressProblem(QUrl("http://localhost:8080/f/a.png"), near).isEmpty());
        QCOMPARE(requestOrigin({{"requestUrl", "https://media.example.test:8443/api/{filename}"}}), QUrl("https://media.example.test:8443"));
        QCOMPARE(requestOrigin({{"requestUrl", "https://media.example.test"}}), QUrl("https://media.example.test"));
        QVERIFY(!requestOrigin({{"requestUrl", ""}}).isValid() || requestOrigin({{"requestUrl", ""}}).isEmpty());
    }
    void importFindsShorterKeysAndSaysWhatItKept() {
        QJsonObject file{{"Version", "17.0.0"}, {"DestinationType", "ImageUploader"}, {"RequestMethod", "POST"}, {"RequestURL", "https://hooks.example.test/upload/9f3a1c2b7e4d5a60"},
                         {"Body", "Binary"}, {"URL", "{json:url}"}};
        auto result = importSxcu(sxcu(file));
        QVERIFY2(!result.error, qPrintable(result.error.message));
        // Sixteen letters and digits are a key.
        QCOMPARE(result.settings["requestUrl"].toString(), QString("https://hooks.example.test/upload/{secret:secrets/url-key}"));
        QCOMPARE(result.secrets.value("secrets/url-key"), QString("9f3a1c2b7e4d5a60"));
        QVERIFY(!result.notes.join(' ').contains("keeps part"));
        // Eight could be one: it stays as written, and the note says where it is, not what.
        file["RequestURL"] = "https://hooks.example.test/api/v2/upload/9f3a1c2b";
        file["DeletionURL"] = "https://hooks.example.test/delete/{json:id}?k=4d5a60ab";
        result = importSxcu(sxcu(file));
        QVERIFY2(!result.error, qPrintable(result.error.message));
        QCOMPARE(result.settings["requestUrl"].toString(), QString("https://hooks.example.test/api/v2/upload/9f3a1c2b"));
        const auto notes = result.notes.join(' ');
        QVERIFY2(notes.contains("The request URL keeps part 4 after its host as written") && notes.contains("The deletion URL keeps part 4 after its host as written"), qPrintable(notes));
        QVERIFY(!notes.contains("9f3a1c2b") && !notes.contains("4d5a60ab"));
        // An ordinary path gets no note: a number is an ID, hyphenated words are a name.
        file["RequestURL"] = "https://hooks.example.test/api/v2/albums/1234567890123456/upload-endpoint-new";
        file.remove("DeletionURL");
        result = importSxcu(sxcu(file));
        QCOMPARE(result.settings["requestUrl"].toString(), QString("https://hooks.example.test/api/v2/albums/1234567890123456/upload-endpoint-new"));
        QVERIFY(result.secrets.isEmpty());
        QVERIFY2(!result.notes.join(' ').contains("keeps part"), qPrintable(result.notes.join(' ')));
    }
    void oldPlaceholders_data() {
        QTest::addColumn<QString>("old");
        QTest::addColumn<QStringList>("regexList");
        QTest::addColumn<QString>("converted");
        QTest::newRow("json") << "$json:files[0].url$" << QStringList{} << "{json:files[0].url}";
        QTest::newRow("two") << "https://h.test/$json:id$/$json:name$" << QStringList{} << "https://h.test/{json:id}/{json:name}";
        QTest::newRow("response") << "$response$" << QStringList{} << "{response}";
        QTest::newRow("header") << "$header:Location$" << QStringList{} << "{header:Location}";
        QTest::newRow("literal braces") << "{\"k\":\"$json:v$\"}" << QStringList{} << "\\{\"k\":\"{json:v}\"\\}";
        QTest::newRow("escaped dollar") << "cost \\$5 $json:v$" << QStringList{} << "cost $5 {json:v}";
        QTest::newRow("regex list") << "$regex:1|1$" << QStringList{"href=\"(.+?)\""} << "{regex:href=\"(.+?)\"|1}";
        QTest::newRow("regex list comma") << "$regex:2,1$" << QStringList{"a", "b{2}|c"} << "{regex:b\\{2\\}\\|c|1}";
        QTest::newRow("regex list whole") << "$regex:1$" << QStringList{"\\d+"} << "{regex:\\\\d+}";
        QTest::newRow("no placeholder") << "https://h.test/plain" << QStringList{} << "https://h.test/plain";
    }
    void oldPlaceholders() {
        QFETCH(QString, old);
        QFETCH(QStringList, regexList);
        QFETCH(QString, converted);
        QCOMPARE(migrateOldSyntax(old, regexList), converted);
        if (old.contains('$') && !old.contains("\\$")) QVERIFY(usesOldSyntax(old));
        QVERIFY2(templateProblem(converted, TemplateUse::Response).isEmpty(), qPrintable(templateProblem(converted, TemplateUse::Response)));
    }
    void oldRegexListStillMatches() {
        const auto converted = migrateOldSyntax("$regex:1|1$", {"href=\"(.+?)\""});
        QCOMPARE(renderResponse(converted, reply("<a href=\"https://h.test/x\">")), QString("https://h.test/x"));
        // An unpaired dollar leaves an unclosed placeholder, which is reported.
        QVERIFY(!templateProblem(migrateOldSyntax("$json:url"), TemplateUse::Response).isEmpty());
    }
    void importKeepsEveryValueOutOfSettings() {
        const auto result = importSxcu(sxcu(zipline()));
        QVERIFY2(!result.error, qPrintable(result.error.message));
        QCOMPARE(result.name, QString("Zipline"));
        const auto s = result.settings;
        QVERIFY2(!validateSettings(s), qPrintable(validateSettings(s).message));
        QCOMPARE(s["requestUrl"].toString(), QString("https://files.example.test/api/upload"));
        QCOMPARE(s["body"].toString(), QString("multipart"));
        QCOMPARE(s["fileFormName"].toString(), QString("file"));
        QCOMPARE(s["accepts"].toArray(), QJsonArray({"image", "text", "file"}));
        QCOMPARE(s["responseUrl"].toString(), QString("{json:files[0].url}"));
        QCOMPARE(s["deletionUrl"].toString(), QString("{json:deletesUrl}"));
        QCOMPARE(s["errorMessage"].toString(), QString("{json:error}"));
        // Both headers are secret by default, the harmless one included.
        QCOMPARE(row("authorization", s, "headers"), QJsonObject({{"name", "authorization"}, {"secret", true}}));
        QCOMPARE(row("x-zipline-format", s, "headers"), QJsonObject({{"name", "x-zipline-format"}, {"secret", true}}));
        QCOMPARE(result.secrets.value("headers/authorization"), QString(token));
        QCOMPARE(result.secrets.value("headers/x-zipline-format"), QString("random"));
        QCOMPARE(secretKeys(s), QStringList({"headers/authorization", "headers/x-zipline-format"}));
        QVERIFY(!QJsonDocument(s).toJson().contains(token));
        for (const auto& note : result.notes) QVERIFY(!note.contains(token));
    }
    void importCases_data() {
        QTest::addColumn<QByteArray>("file");
        QTest::addColumn<QString>("errorCode");
        QTest::addColumn<QString>("fragment"); // of the error message, or of a note when imported
        auto base = zipline();
        const auto with = [&](std::initializer_list<QPair<QString, QJsonValue>> changes) { auto o = base; for (const auto& [k, v] : changes) { if (v.isUndefined()) o.remove(k); else o[k] = v; } return sxcu(o); };
        QTest::newRow("not json") << QByteArray("RequestURL=https://x") << "sxcu_invalid" << "JSON object";
        QTest::newRow("array") << QByteArray("[1,2]") << "sxcu_invalid" << "JSON object";
        QTest::newRow("truncated") << sxcu(base).left(40) << "sxcu_invalid" << "JSON object";
        QTest::newRow("too large") << QByteArray(300 * 1024, ' ') << "sxcu_invalid" << "too large";
        QTest::newRow("no url") << with({{"RequestURL", QJsonValue::Undefined}}) << "sxcu_invalid" << "RequestURL";
        QTest::newRow("shortener") << with({{"DestinationType", "URLShortener"}}) << "sxcu_unsupported_type" << "URL shortener";
        QTest::newRow("sharing") << with({{"DestinationType", "URLSharingService"}}) << "sxcu_unsupported_type" << "URL sharing service";
        QTest::newRow("shortener and sharing") << with({{"DestinationType", "URLShortener, URLSharingService"}}) << "sxcu_unsupported_type" << "not an uploader";
        QTest::newRow("shortener flag") << with({{"DestinationType", 8}}) << "sxcu_unsupported_type" << "URL shortener";
        QTest::newRow("unknown type") << with({{"DestinationType", "VideoUploader"}}) << "sxcu_invalid" << "destination type";
        QTest::newRow("uploader and shortener") << with({{"DestinationType", "ImageUploader, URLShortener"}}) << "" << "uploader part only";
        QTest::newRow("no type") << with({{"DestinationType", QJsonValue::Undefined}}) << "" << "names no destination type";
        QTest::newRow("delete method") << with({{"RequestMethod", "DELETE"}}) << "sxcu_unsupported" << "DELETE";
        QTest::newRow("unknown body") << with({{"Body", "Protobuf"}}) << "sxcu_invalid" << "request body";
        QTest::newRow("select") << with({{"Arguments", QJsonObject{{"album", "{select:a|b}"}}}}) << "sxcu_unsupported" << "asks the user";
        QTest::newRow("prompt in header") << with({{"Headers", QJsonObject{{"X-Key", "{prompt:Key}"}}}}) << "sxcu_unsupported" << "asks the user";
        QTest::newRow("bad url template") << with({{"URL", "{json:files[*].url}"}}) << "sxcu_unsupported" << "JSON path";
        QTest::newRow("unclosed") << with({{"URL", "{json:url"}}) << "sxcu_unsupported" << "closing brace";
        QTest::newRow("bad header name") << with({{"Headers", QJsonObject{{"Bad Header", "x"}}}}) << "sxcu_invalid" << "header name";
        QTest::newRow("headers not object") << with({{"Headers", "authorization: x"}}) << "sxcu_invalid" << "Headers";
        QTest::newRow("user info") << with({{"RequestURL", "https://user:pw@files.example.test/up"}}) << "sxcu_unsupported" << "user name";
        QTest::newRow("ftp") << with({{"RequestURL", "ftp://files.example.test/up"}}) << "sxcu_unsupported" << "http(s)";
        QTest::newRow("plain http imports with a note") << with({{"RequestURL", "http://files.example.test/up"}}) << "" << "plain HTTP";
        QTest::newRow("no scheme") << with({{"RequestURL", "files.example.test/up"}}) << "" << "https:// was added";
        QTest::newRow("managed header") << with({{"Headers", QJsonObject{{"Content-Length", "12"}, {"authorization", token}}}}) << "" << "left out";
        QTest::newRow("name pattern") << with({{"Arguments", QJsonObject{{"name", "shot-%y%mo%d"}}}}) << "" << "name pattern";
    }
    void importCases() {
        QFETCH(QByteArray, file);
        QFETCH(QString, errorCode);
        QFETCH(QString, fragment);
        const auto result = importSxcu(file);
        QCOMPARE(result.error.code, errorCode);
        if (!errorCode.isEmpty()) {
            QVERIFY2(result.error.message.contains(fragment), qPrintable(result.error.message));
            QVERIFY(result.settings.isEmpty());
            QVERIFY(result.secrets.isEmpty());
            QVERIFY(!result.error.message.contains(token));
            return;
        }
        QVERIFY2(result.notes.join('\n').contains(fragment), qPrintable(result.notes.join('\n')));
        QVERIFY(!QJsonDocument(result.settings).toJson().contains(token));
        QVERIFY(!result.notes.join('\n').contains(token));
    }
    void importOlderFile() {
        // Chibisafe's generator still writes the $…$ form; 13.x files carry a Version.
        const QJsonObject file{{"Version", "13.7.0"}, {"Name", "chibisafe"}, {"DestinationType", "ImageUploader, FileUploader"}, {"RequestMethod", "POST"},
                               {"RequestURL", "https://safe.example.test/api/upload"}, {"Headers", QJsonObject{{"x-api-key", token}}}, {"Body", "MultipartFormData"},
                               {"FileFormName", "file"}, {"URL", "$json:url$"}, {"ThumbnailURL", "$json:thumb$"}};
        const auto result = importSxcu(sxcu(file));
        QVERIFY2(!result.error, qPrintable(result.error.message));
        QCOMPARE(result.settings["responseUrl"].toString(), QString("{json:url}"));
        QCOMPARE(result.settings["thumbnailUrl"].toString(), QString("{json:thumb}"));
        QCOMPARE(result.settings["accepts"].toArray(), QJsonArray({"image", "file"}));
        QVERIFY(result.notes.join(' ').contains("older"));
        // No Version at all: the placeholders themselves tell the generations apart.
        auto unversioned = file;
        unversioned.remove("Version");
        QCOMPARE(importSxcu(sxcu(unversioned)).settings["responseUrl"].toString(), QString("{json:url}"));
        auto modern = unversioned;
        modern["URL"] = "{json:url}";
        modern["ThumbnailURL"] = "";
        QCOMPARE(importSxcu(sxcu(modern)).settings["responseUrl"].toString(), QString("{json:url}"));
    }
    void importLegacyFields() {
        // Before 12.4: RequestType, no Body, RegexList and ResponseType.
        const QJsonObject file{{"Name", "legacy"}, {"RequestType", "POST"}, {"RequestURL", "https://old.example.test/upload.php?key=abc123&mode=json"},
                               {"FileFormName", "upload"}, {"Arguments", QJsonObject{{"title", "$filename$"}, {"secret", "s3cr3t-value"}}},
                               {"RegexList", QJsonArray{"\"link\":\"(.+?)\""}}, {"URL", "$regex:1,1$"}, {"DeletionURL", "https://old.example.test/del/$json:id$"}};
        const auto result = importSxcu(sxcu(file));
        QVERIFY2(!result.error, qPrintable(result.error.message));
        const auto s = result.settings;
        QCOMPARE(s["requestMethod"].toString(), QString("POST"));
        QCOMPARE(s["body"].toString(), QString("multipart"));
        QCOMPARE(s["fileFormName"].toString(), QString("upload"));
        // The query moved to parameters, each a secret.
        QCOMPARE(s["requestUrl"].toString(), QString("https://old.example.test/upload.php"));
        QCOMPARE(result.secrets.value("parameters/key"), QString("abc123"));
        QCOMPARE(result.secrets.value("parameters/mode"), QString("json"));
        // Only the bare file-name placeholder stays in settings.
        QCOMPARE(row("title", s, "arguments"), QJsonObject({{"name", "title"}, {"secret", false}, {"value", "{filename}"}}));
        QCOMPARE(row("secret", s, "arguments"), QJsonObject({{"name", "secret"}, {"secret", true}}));
        QCOMPARE(s["responseUrl"].toString(), QString("{regex:\"link\":\"(.+?)\"|1}"));
        QCOMPARE(s["deletionUrl"].toString(), QString("https://old.example.test/del/{json:id}"));
        QCOMPARE(renderResponse(s["responseUrl"].toString(), reply(quotes("{'link':'https://old.example.test/i/9','id':'9'}"))), QString("https://old.example.test/i/9"));
        const auto bytes = QJsonDocument(s).toJson();
        QVERIFY(!bytes.contains("abc123"));
        QVERIFY(!bytes.contains("s3cr3t-value"));
        // ResponseType named where the link was when URL was empty.
        QJsonObject redirect{{"RequestType", "POST"}, {"RequestURL", "https://old.example.test/u"}, {"FileFormName", "f"}, {"ResponseType", "RedirectionURL"}};
        QCOMPARE(importSxcu(sxcu(redirect)).settings["responseUrl"].toString(), QString("{responseurl}"));
        redirect["ResponseType"] = "LocationHeader";
        QCOMPARE(importSxcu(sxcu(redirect)).settings["responseUrl"].toString(), QString("{header:Location}"));
        redirect["ResponseType"] = "Text";
        QCOMPARE(importSxcu(sxcu(redirect)).settings["responseUrl"].toString(), QString());
    }
    void importTextBodies() {
        const QJsonObject paste{{"Version", "15.0.0"}, {"Name", "paste"}, {"DestinationType", "TextUploader"}, {"RequestMethod", "POST"},
                                {"RequestURL", "https://paste.example.test/api"}, {"Body", "JSON"}, {"Data", QString("{\"key\":\"%1\",\"text\":\"{input}\"}").arg(token)},
                                {"Arguments", QJsonObject{{"ignored", "x"}}}, {"URL", "https://paste.example.test/{json:id}"}};
        const auto result = importSxcu(sxcu(paste));
        QVERIFY2(!result.error, qPrintable(result.error.message));
        QCOMPARE(result.settings["body"].toString(), QString("json"));
        QCOMPARE(result.settings["accepts"].toArray(), QJsonArray({"text"}));
        // The Data body is a secret as a whole; Arguments are not sent with a JSON body.
        QCOMPARE(result.settings["dataSecret"].toBool(), true);
        QCOMPARE(result.settings["data"].toString(), QString());
        QVERIFY(result.secrets.value(dataSecretKey()).contains(token));
        QVERIFY(result.settings["arguments"].toArray().isEmpty());
        QVERIFY(result.notes.join(' ').contains("Arguments"));
        QVERIFY(!QJsonDocument(result.settings).toJson().contains(token));
        const QJsonObject form{{"Version", "15.0.0"}, {"DestinationType", "TextUploader"}, {"RequestURL", "https://paste.example.test/new"}, {"Body", "FormURLEncoded"},
                               {"Arguments", QJsonObject{{"content", "{input}"}, {"name", "{filename}"}, {"key", token}}}};
        const auto encoded = importSxcu(sxcu(form));
        QVERIFY2(!encoded.error, qPrintable(encoded.error.message));
        QCOMPARE(encoded.settings["body"].toString(), QString("form"));
        QCOMPARE(row("content", encoded.settings, "arguments")["value"].toString(), QString("{input}"));
        QCOMPARE(encoded.secrets.keys(), QStringList({"arguments/key"}));
        // A modern file without Body has none, and GET carries no body at all.
        const QJsonObject get{{"Version", "15.0.0"}, {"DestinationType", "TextUploader"}, {"RequestMethod", "GET"}, {"RequestURL", "https://paste.example.test/add"},
                              {"Parameters", QJsonObject{{"content", "{input}"}}}, {"Body", "MultipartFormData"}};
        const auto none = importSxcu(sxcu(get));
        QVERIFY2(!none.error, qPrintable(none.error.message));
        QCOMPARE(none.settings["body"].toString(), QString("none"));
        QCOMPARE(none.settings["requestMethod"].toString(), QString("GET"));
    }
    void settingsRules_data() {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QJsonValue>("value");
        QTest::addColumn<QString>("fragment"); // empty: accepted
        QTest::newRow("defaults") << "" << QJsonValue() << "";
        QTest::newRow("url") << "requestUrl" << QJsonValue("https://h.example.test/up") << "";
        QTest::newRow("url with filename") << "requestUrl" << QJsonValue("https://h.example.test/put/{filename}") << "";
        QTest::newRow("loopback http") << "requestUrl" << QJsonValue("http://127.0.0.1:8080/up") << "";
        QTest::newRow("plain http") << "requestUrl" << QJsonValue("http://h.example.test/up") << "plain HTTP";
        QTest::newRow("query in url") << "requestUrl" << QJsonValue("https://h.example.test/up?key=1") << "Parameters";
        QTest::newRow("placeholder host") << "requestUrl" << QJsonValue("https://{filename}.example.test/up") << "host name";
        QTest::newRow("user info") << "requestUrl" << QJsonValue("https://u:p@h.example.test/up") << "user name";
        QTest::newRow("method") << "requestMethod" << QJsonValue("DELETE") << "method";
        QTest::newRow("body") << "body" << QJsonValue("yaml") << "body";
        QTest::newRow("accepts empty") << "accepts" << QJsonValue(QJsonArray{}) << "at least one";
        QTest::newRow("accepts unknown") << "accepts" << QJsonValue(QJsonArray{"video"}) << "at least one";
        QTest::newRow("secret row with value") << "headers" << QJsonValue(QJsonArray{QJsonObject{{"name", "Authorization"}, {"secret", true}, {"value", token}}}) << "unless secret";
        QTest::newRow("secret row") << "headers" << QJsonValue(QJsonArray{QJsonObject{{"name", "Authorization"}, {"secret", true}}}) << "";
        QTest::newRow("plain row") << "headers" << QJsonValue(QJsonArray{QJsonObject{{"name", "X-Format"}, {"secret", false}, {"value", "png"}}}) << "";
        QTest::newRow("plain row no value") << "headers" << QJsonValue(QJsonArray{QJsonObject{{"name", "X-Format"}, {"secret", false}}}) << "unless secret";
        QTest::newRow("duplicate header") << "headers" << QJsonValue(QJsonArray{QJsonObject{{"name", "X-A"}, {"secret", true}}, QJsonObject{{"name", "x-a"}, {"secret", true}}}) << "twice";
        QTest::newRow("managed header") << "headers" << QJsonValue(QJsonArray{QJsonObject{{"name", "Host"}, {"secret", true}}}) << "cannot be set";
        QTest::newRow("content type") << "headers" << QJsonValue(QJsonArray{QJsonObject{{"name", "Content-Type"}, {"secret", false}, {"value", "image/png"}}}) << "binary body only";
        QTest::newRow("header line break") << "headers" << QJsonValue(QJsonArray{QJsonObject{{"name", "X-A"}, {"secret", false}, {"value", "a\r\nX-B: c"}}}) << "control characters";
        QTest::newRow("argument placeholder") << "arguments" << QJsonValue(QJsonArray{QJsonObject{{"name", "t"}, {"secret", false}, {"value", "{json:x}"}}}) << "reads the reply";
        QTest::newRow("secret data with text") << "dataSecret" << QJsonValue(true) << "";
        QTest::newRow("response template") << "responseUrl" << QJsonValue("{json:a[*]}") << "JSON path";
        QTest::newRow("timeout") << "requestTimeoutMs" << QJsonValue(10) << "timeouts";
        QTest::newRow("secret value name") << "secrets" << QJsonValue(QJsonArray{QJsonObject{{"name", "url-key"}, {"secret", true}}}) << "";
        QTest::newRow("secret value with a value") << "secrets" << QJsonValue(QJsonArray{QJsonObject{{"name", "url-key"}, {"secret", true}, {"value", token}}}) << "no value in settings";
        QTest::newRow("secret value not secret") << "secrets" << QJsonValue(QJsonArray{QJsonObject{{"name", "url-key"}, {"secret", false}}}) << "no value in settings";
        QTest::newRow("secret value odd name") << "secrets" << QJsonValue(QJsonArray{QJsonObject{{"name", "a b"}, {"secret", true}}}) << "unique name";
        QTest::newRow("reference to nothing") << "deletionUrl" << QJsonValue("{json:url}/delete/{secret:arguments/token}") << "does not store";
        QTest::newRow("insecure switch type") << "allowInsecureHttp" << QJsonValue("yes") << "true or false";
    }
    void settingsRules() {
        QFETCH(QString, field);
        QFETCH(QJsonValue, value);
        QFETCH(QString, fragment);
        auto settings = defaults();
        if (!field.isEmpty()) settings[field] = value;
        const auto error = validateSettings(settings);
        if (fragment.isEmpty()) QVERIFY2(!error, qPrintable(error.message));
        else QVERIFY2(error.message.contains(fragment), qPrintable(error.code + ": " + error.message));
        // A refusal never repeats a value.
        QVERIFY(!error.message.contains(token));
    }
    void settingsRejectUnknownAndSecretBody() {
        auto settings = defaults();
        settings["token"] = token;
        QCOMPARE(validateSettings(settings).code, QString("http_settings_invalid"));
        settings = defaults();
        settings["body"] = "json";
        settings["dataSecret"] = true;
        settings["data"] = QString("{\"key\":\"%1\"}").arg(token);
        QVERIFY(validateSettings(settings).message.contains("credential store"));
        settings["data"] = "";
        QVERIFY(!validateSettings(settings));
        QCOMPARE(secretKeys(settings), QStringList({"data"}));
        // A reference is good once the value it names is a secret row.
        settings = defaults();
        settings["deletionUrl"] = "{json:url}/delete/{secret:arguments/token}";
        settings["arguments"] = QJsonArray{QJsonObject{{"name", "token"}, {"secret", true}}};
        QVERIFY2(!validateSettings(settings), qPrintable(validateSettings(settings).message));
        settings["arguments"] = QJsonArray{QJsonObject{{"name", "token"}, {"secret", false}, {"value", "plain"}}};
        QVERIFY(validateSettings(settings).message.contains("does not store"));
        settings = defaults();
        settings["requestUrl"] = "https://h.example.test/hook/{secret:secrets/url-key}";
        settings["secrets"] = QJsonArray{QJsonObject{{"name", "url-key"}, {"secret", true}}};
        QVERIFY2(!validateSettings(settings), qPrintable(validateSettings(settings).message));
        QCOMPARE(secretKeys(settings), QStringList({"secrets/url-key"}));
        QCOMPARE(secretLabel("secrets/url-key"), QString("secret value url-key"));
        // GET has no body.
        settings = defaults();
        settings["requestMethod"] = "GET";
        QVERIFY(validateSettings(settings).message.contains("GET"));
        settings["body"] = "none";
        QVERIFY(!validateSettings(settings));
        // Plain HTTP beyond loopback needs the destination's own switch.
        settings = defaults();
        settings["requestUrl"] = "http://192.168.68.71:18361/api/upload";
        QCOMPARE(validateSettings(settings).code, QString("http_insecure_url"));
        settings["allowInsecureHttp"] = true;
        QVERIFY(!validateSettings(settings));
    }
    void kindsAndHosts() {
        QCOMPARE(artifactKind("image/png", "images"), QString("image"));
        QCOMPARE(artifactKind("text/plain", "text"), QString("text"));
        QCOMPARE(artifactKind("video/mp4", "videos"), QString("file"));
        QCOMPARE(artifactKind("application/zip", "other"), QString("file"));
        auto settings = defaults();
        settings["accepts"] = QJsonArray{"image"};
        QVERIFY(acceptsKind(settings, "image"));
        QVERIFY(!acceptsKind(settings, "text"));
        QVERIFY(!acceptsKind(settings, "file"));
        settings["accepts"] = QJsonArray{"file"};
        QVERIFY(acceptsKind(settings, "image"));
        QVERIFY(sameHost(QUrl("https://a.test/x"), QUrl("https://A.test/y")));
        QVERIFY(sameHost(QUrl("http://a.test/x"), QUrl("https://a.test/x")));
        QVERIFY(!sameHost(QUrl("https://a.test/x"), QUrl("http://a.test/x")));
        QVERIFY(!sameHost(QUrl("https://a.test/x"), QUrl("https://b.test/x")));
        QVERIFY(!sameHost(QUrl("https://a.test/x"), QUrl("https://a.test:8443/x")));
        QVERIFY(!sameHost(QUrl("http://127.0.0.1:8000/x"), QUrl("http://localhost:8000/x")));
        QVERIFY(transportAllowed(QUrl("https://a.test"), false));
        QVERIFY(transportAllowed(QUrl("http://localhost:1"), false));
        QVERIFY(!transportAllowed(QUrl("http://a.test"), false));
        QVERIFY(transportAllowed(QUrl("http://a.test"), true));
        QVERIFY(!transportAllowed(QUrl("ftp://a.test"), true));
        QCOMPARE(boundedMessage("line one\r\nline\ttwo\x01"), QString("line one line two"));
        QCOMPARE(boundedMessage(QString(500, 'x'), 20).size(), 20);
        QCOMPARE(secretLabel("headers/Authorization"), QString("header Authorization"));
        QCOMPARE(secretLabel("data"), QString("request body"));
    }
};
QTEST_APPLESS_MAIN(HttpUploaderRules)
#include "test_http_uploader.moc"
