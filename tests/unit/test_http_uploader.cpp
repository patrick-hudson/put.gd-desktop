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
    void responseValues_data() {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("body");
        QTest::addColumn<QString>("expected");
        // moc misreads raw string literals here, so quotes() writes the double quotes.
        const QString json = quotes("{'files':[{'url':'https://h.test/u/a.png','id':7,'ok':true}],'data':{'link':'https://h.test/l','a b':'spaced'},'error':'quota'}");
        const QString xml = quotes("<?xml version='1.0'?><r:upload xmlns:r='urn:x'><file id='1'><url>https://h.test/1</url></file><file id='2' kind='main'><url>https://h.test/2</url><note> two </note></file><delete href='https://h.test/d'/></r:upload>");
        const QString html = quotes("<p>ok</p><a class='link' href='https://h.test/v/x1'>open</a> <a href='https://h.test/del/x1'>delete</a>");
        QTest::newRow("json array") << "{json:files[0].url}" << json << "https://h.test/u/a.png";
        QTest::newRow("json missing") << "{json:files[1].url}" << json << "";
        QTest::newRow("xml path") << "{xml:/upload/file/url}" << xml << "https://h.test/1";
        QTest::newRow("regex group") << "{regex:href=\"([^\"]+)\"|1}" << html << "https://h.test/v/x1";
        QTest::newRow("header") << "{header:location}" << "" << "https://files.example.test/u/a.png";
        QTest::newRow("mixed") << "{json:data.link}?d={header:X-Delete-Key}" << json << "https://h.test/l?d=d3l";
    }
    void responseValues() {
        QFETCH(QString, text);
        QFETCH(QString, body);
        QFETCH(QString, expected);
        QVERIFY2(templateProblem(text, TemplateUse::Response).isEmpty(), qPrintable(templateProblem(text, TemplateUse::Response)));
        QCOMPARE(renderResponse(text, reply(body)), expected);
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
        QVERIFY2(notes.contains("ThumbnailURL repeated the value of argument 3") && notes.contains("ErrorMessage repeated the value of argument 3"), qPrintable(notes));
        QVERIFY(!notes.contains(key));
        QVERIFY(!QJsonDocument(result.settings).toJson().contains(key.toUtf8()));
        QVERIFY2(!validateSettings(result.settings), qPrintable(validateSettings(result.settings).message));
        // The link itself cannot be left out: the file is refused, naming the row and never its value.
        auto shared = file;
        shared["URL"] = "https://media.example.test/{json:name}?t=" + key;
        const auto refused = importSxcu(sxcu(shared));
        QCOMPARE(refused.error.code, QString("sxcu_unsupported"));
        QVERIFY2(refused.error.message.contains("URL repeats the value of argument 3"), qPrintable(refused.error.message));
        QVERIFY(!refused.error.message.contains(key));
        QVERIFY(refused.secrets.isEmpty() && refused.settings.isEmpty());
    }
    // #1570: a key the import takes out of an address, and the value of a row that is not
    // sent, are credentials like a stored row: what is shown or recorded never repeats them.
    // The thumbnail URL and the error message are left out with a note; a link that repeats
    // one is refused, as for a stored row (it would be refused at every upload).
    void importKeepsExtractedAndUnsentValuesOutOfWhatIsShown_data() {
        QTest::addColumn<QJsonObject>("file");
        QTest::addColumn<QString>("label"); // the value the notes and the refusal name
        const QString key = token;
        const QJsonObject file{{"Version", "17.0.0"}, {"DestinationType", "ImageUploader"}, {"RequestMethod", "POST"}, {"RequestURL", "https://media.example.test/upload"},
                               {"Body", "MultipartFormData"}, {"FileFormName", "file"}, {"URL", "https://media.example.test/{json:u}"},
                               {"ThumbnailURL", "{json:thumb}?t=" + key}, {"ErrorMessage", "{json:error} (" + key + ")"},
                               {"DeletionURL", "https://media.example.test/delete/{json:id}/" + key}};
        // The issue's file: the token is taken out of the deletion URL.
        QTest::newRow("deletion URL key") << file << "secret value deletion-key";
        auto request = file;
        request["RequestURL"] = "https://media.example.test/upload/" + key;
        QTest::newRow("request URL key") << request << "secret value url-key";
        // An argument that is not sent, because the file has no body, is not stored.
        auto argument = file;
        argument["Body"] = "None";
        argument["Arguments"] = QJsonObject{{"token", key}};
        argument.remove("DeletionURL");
        QTest::newRow("unsent argument") << argument << "argument 1";
        // Nor is a header put.gd sets itself, here Content-Type without a binary body.
        auto header = argument;
        header.remove("Arguments");
        header["Headers"] = QJsonObject{{"Content-Type", key}};
        QTest::newRow("unsent header") << header << "header 1";
    }
    void importKeepsExtractedAndUnsentValuesOutOfWhatIsShown() {
        QFETCH(QJsonObject, file);
        QFETCH(QString, label);
        const QString key = token;
        const auto result = importSxcu(sxcu(file));
        QVERIFY2(!result.error, qPrintable(result.error.message));
        QCOMPARE(result.settings["thumbnailUrl"].toString(), QString());
        QCOMPARE(result.settings["errorMessage"].toString(), QString());
        const auto notes = result.notes.join(' ');
        QVERIFY2(notes.contains("ThumbnailURL repeated the value of " + label) && notes.contains("ErrorMessage repeated the value of " + label), qPrintable(notes));
        QVERIFY(!notes.contains(key));
        QVERIFY(!QJsonDocument(result.settings).toJson().contains(key.toUtf8()));
        QVERIFY2(!validateSettings(result.settings), qPrintable(validateSettings(result.settings).message));
        auto shared = file;
        shared["URL"] = "https://media.example.test/{json:u}?t=" + key;
        const auto refused = importSxcu(sxcu(shared));
        QCOMPARE(refused.error.code, QString("sxcu_unsupported"));
        QVERIFY2(refused.error.message.contains("URL repeats the value of " + label), qPrintable(refused.error.message));
        QVERIFY(!refused.error.message.contains(key));
        QVERIFY(refused.secrets.isEmpty() && refused.settings.isEmpty());
    }
    // Sol review of #1614: the whole text that is kept counts, and a credential is named
    // only by a label that cannot hold it.
    void importLeavesOutWhatRepeatsACredential_data() {
        QTest::addColumn<QJsonObject>("file");
        QTest::addColumn<QString>("field"); // the setting that is left out
        QTest::addColumn<QString>("note");
        const QString key = token;
        const QJsonObject base{{"Version", "17.0.0"}, {"DestinationType", "ImageUploader"}, {"RequestMethod", "POST"}, {"RequestURL", "https://media.example.test/upload"},
                               {"Body", "MultipartFormData"}, {"FileFormName", "file"}, {"Arguments", QJsonObject{{"token", key}}}, {"URL", "{json:u}"}};
        auto help = base;
        help["ErrorMessage"] = "{json:error} (" + key + ") see https://help.example.test/errors";
        QTest::newRow("error message before an address") << help << "errorMessage" << "ErrorMessage repeated the value of argument 1";
        auto host = base;
        host["ThumbnailURL"] = "https://" + key + ".cdn.example.test/{json:t}";
        QTest::newRow("thumbnail host") << host << "thumbnailUrl" << "ThumbnailURL repeated the value of argument 1";
        auto inside = base;
        inside["ThumbnailURL"] = "{json:files." + key + "}";
        QTest::newRow("inside a placeholder") << inside << "thumbnailUrl" << "ThumbnailURL repeated the value of argument 1";
        auto escaped = base;
        escaped["ThumbnailURL"] = "{json:t}?k=" + key.left(10) + "\\" + key.mid(10);
        QTest::newRow("across an escape") << escaped << "thumbnailUrl" << "ThumbnailURL repeated the value of argument 1";
        auto deletion = base;
        deletion["DeletionURL"] = "{json:url}/delete?k={json:" + key + "}";
        QTest::newRow("deletion URL placeholder") << deletion << "deletionUrl" << "DeletionURL repeated the value of argument 1";
        // A query value a Parameters row of the same name replaced is still a credential.
        auto displaced = base;
        displaced.remove("Arguments");
        displaced["RequestURL"] = "https://media.example.test/upload?token=" + key;
        displaced["Parameters"] = QJsonObject{{"token", "replacement-value"}};
        displaced["ThumbnailURL"] = "{json:t}?k=" + key;
        QTest::newRow("displaced query value") << displaced << "thumbnailUrl" << "ThumbnailURL repeated the value of parameter 1";
        // A form body text that is not sent is looked for too.
        auto data = base;
        data["Data"] = "{\"token\":\"" + key + "\"}";
        data.remove("Arguments");
        data["ErrorMessage"] = "{json:error} " + key;
        QTest::newRow("unsent body text") << data << "errorMessage" << "ErrorMessage repeated the value of request body";
    }
    void importLeavesOutWhatRepeatsACredential() {
        QFETCH(QJsonObject, file);
        QFETCH(QString, field);
        QFETCH(QString, note);
        const QString key = token;
        const auto result = importSxcu(sxcu(file));
        QVERIFY2(!result.error, qPrintable(result.error.message));
        QCOMPARE(result.settings[field].toString(), QString());
        const auto notes = result.notes.join(' ');
        QVERIFY2(notes.contains(note), qPrintable(notes));
        QVERIFY(!notes.contains(key) && !result.name.contains(key));
        QVERIFY(!QJsonDocument(result.settings).toJson().contains(key.toUtf8()));
        QVERIFY2(!validateSettings(result.settings), qPrintable(validateSettings(result.settings).message));
    }
    void importRefusesWhatRepeatsACredential_data() {
        QTest::addColumn<QJsonObject>("file");
        QTest::addColumn<QString>("message");
        const QString key = token;
        const QJsonObject base{{"Version", "17.0.0"}, {"DestinationType", "ImageUploader"}, {"RequestMethod", "POST"}, {"RequestURL", "https://media.example.test/upload"},
                               {"Body", "MultipartFormData"}, {"FileFormName", "file"}, {"Arguments", QJsonObject{{"token", key}}}, {"URL", "{json:u}"}};
        auto user = base;
        user["URL"] = "https://" + key + "@media.example.test/{json:u}";
        QTest::newRow("link user information") << user << "URL repeats the value of argument 1";
        auto request = base;
        request["RequestURL"] = "https://upload:" + key + "@media.example.test/upload";
        QTest::newRow("request URL user information") << request << "RequestURL repeats the value of argument 1";
        // A credential that runs across the "?" the request URL is split at.
        auto across = base;
        across["Headers"] = QJsonObject{{"X-Api-Key", "segment123?q=v"}};
        across["RequestURL"] = "https://media.example.test/segment123?q=v";
        QTest::newRow("across the query") << across << "RequestURL repeats the value of header 1";
        // The row's name holds its own value: it is named by its place.
        auto named = base;
        named["Arguments"] = QJsonObject{{key, key}};
        named["URL"] = "{json:u}?k=" + key;
        QTest::newRow("label holds the credential") << named << "URL repeats the value of argument 1";
        auto form = base;
        form["FileFormName"] = key;
        QTest::newRow("file form name") << form << "FileFormName repeats the value of argument 1";
        auto rowName = base;
        rowName["Headers"] = QJsonObject{{"X-" + key, "on"}};
        QTest::newRow("row name") << rowName << "The name of the header 1 repeats the value of argument 1";
        // A name is text, not a template: braces in it are looked through.
        auto braces = base;
        braces["Arguments"] = QJsonObject{{"token", "Bearer demo1234"}, {"f{demo1234}", "on"}};
        QTest::newRow("row name in braces") << braces << "The name of the argument 1 repeats the value of argument 2";
        // No text of the file is quoted, a placeholder's name included.
        auto unknown = base;
        unknown["Headers"] = QJsonObject{{"X-Mode", "{" + key + "}"}};
        QTest::newRow("unknown placeholder") << unknown << "value of header 1 uses a placeholder put.gd cannot send";
        auto shortName = base;
        shortName["Arguments"] = QJsonObject{{"token", "Bearer demo1234"}};
        shortName["Headers"] = QJsonObject{{"X-demo1234", "on"}};
        QTest::newRow("short credential in a name") << shortName << "The name of the header 1 repeats the value of argument 1";
        auto method = base;
        method["RequestMethod"] = key;
        QTest::newRow("method") << method << "RequestMethod is not one put.gd supports";
        // A replaced query value and a body text that is not sent have the row limits.
        auto longQuery = base;
        longQuery["RequestURL"] = "https://media.example.test/upload?token=" + QString(maximumValueChars + 1, 'q');
        longQuery["Parameters"] = QJsonObject{{"token", "replacement-value"}};
        QTest::newRow("long replaced query value") << longQuery << "value of parameter 1 is too long";
        auto longData = base;
        longData["Data"] = QString(maximumDataChars + 1, 'd');
        QTest::newRow("long unsent body") << longData << "request body text is too long";
        // Rows that are not sent count toward the limit, so their number is bounded.
        QJsonObject many;
        for (int i = 0; i <= maximumRows; ++i) many["a" + QString::number(i)] = "v";
        auto unsent = base;
        unsent["Body"] = "None";
        unsent["Arguments"] = many;
        QTest::newRow("too many unsent arguments") << unsent << "more than 32 arguments";
    }
    void importRefusesWhatRepeatsACredential() {
        QFETCH(QJsonObject, file);
        QFETCH(QString, message);
        const auto refused = importSxcu(sxcu(file));
        QVERIFY2(refused.error.message.contains(message), qPrintable(refused.error.message));
        QVERIFY(!refused.error.message.contains(QString(token).left(16)) && !refused.error.message.contains("demo1234"));
        QVERIFY(refused.secrets.isEmpty() && refused.settings.isEmpty());
    }
    // The query split is looked across with the whole-word rule for short values.
    void importDoesNotRefuseAShortValueInsideAWordAcrossTheQuery() {
        const QJsonObject file{{"Version", "17.0.0"}, {"DestinationType", "ImageUploader"}, {"RequestMethod", "POST"}, {"RequestURL", "https://media.example.test/multipart?query=x"},
                               {"Body", "MultipartFormData"}, {"FileFormName", "file"}, {"Headers", QJsonObject{{"X-Api-Key", "part?q"}}}, {"URL", "{json:u}"}};
        const auto result = importSxcu(sxcu(file));
        QVERIFY2(!result.error, qPrintable(result.error.message));
    }
    // A deletion URL that is left out takes the keys taken out of it along: nothing is
    // stored that no address uses.
    void importStoresNoKeyOfALeftOutDeletionUrl() {
        const QString key = token;
        const QJsonObject file{{"Version", "17.0.0"}, {"DestinationType", "ImageUploader"}, {"RequestMethod", "POST"}, {"RequestURL", "https://media.example.test/upload"},
                               {"Body", "MultipartFormData"}, {"FileFormName", "file"}, {"Arguments", QJsonObject{{"token", key}}}, {"URL", "{json:u}"},
                               {"DeletionURL", "https://media.example.test/delete/a1b2c3d4e5f60718293a/{json:" + key + "}"}};
        const auto result = importSxcu(sxcu(file));
        QVERIFY2(!result.error, qPrintable(result.error.message));
        QCOMPARE(result.settings["deletionUrl"].toString(), QString());
        QCOMPARE(result.secrets.keys(), QStringList({"arguments/token"}));
        QCOMPARE(result.settings["secrets"].toArray(), QJsonArray());
        QVERIFY2(!validateSettings(result.settings), qPrintable(validateSettings(result.settings).message));
    }
    // A Name that repeats a credential gives way to a fixed name, not to the host, which is
    // written in lower case and may repeat a credential written that way.
    void importNamesADestinationWithoutACredential() {
        const QString lower = "abcd1234efgh5678ijkl";
        const QJsonObject file{{"Version", "17.0.0"}, {"Name", lower}, {"DestinationType", "ImageUploader"}, {"RequestMethod", "POST"},
                               {"RequestURL", "https://" + lower.toUpper() + ".example.test/upload"}, {"Body", "MultipartFormData"}, {"FileFormName", "file"},
                               {"Arguments", QJsonObject{{"token", lower}}}, {"URL", "{json:u}"}};
        auto result = importSxcu(sxcu(file));
        QVERIFY2(!result.error, qPrintable(result.error.message));
        QCOMPARE(result.name, QString("Imported uploader"));
        auto unnamed = file;
        unnamed.remove("Name");
        result = importSxcu(sxcu(unnamed));
        QVERIFY2(!result.error, qPrintable(result.error.message));
        QCOMPARE(result.name, QString("Imported uploader"));
    }
    // A key the deletion URL shares with the request URL is stored for each, and a
    // deletion key that only begins like the request key stays whole.
    void importStoresADeletionKeyApartFromTheRequestKey() {
        const QString first = "a1b2c3d4e5f60718", longer = first + "9z8y7x6w"; // synthetic runs
        QJsonObject file{{"Version", "17.0.0"}, {"DestinationType", "ImageUploader"}, {"RequestMethod", "POST"}, {"RequestURL", "https://media.example.test/upload/" + first},
                         {"Body", "Binary"}, {"URL", "{json:url}"}, {"DeletionURL", "https://media.example.test/delete/{json:id}/" + longer}};
        auto result = importSxcu(sxcu(file));
        QVERIFY2(!result.error, qPrintable(result.error.message));
        QCOMPARE(result.settings["deletionUrl"].toString(), QString("https://media.example.test/delete/{json:id}/{secret:secrets/deletion-key-2}"));
        QCOMPARE(result.secrets.value("secrets/url-key"), first);
        QCOMPARE(result.secrets.value("secrets/deletion-key-2"), longer);
        file["DeletionURL"] = "https://media.example.test/delete/{json:id}/" + first;
        result = importSxcu(sxcu(file));
        QCOMPARE(result.settings["deletionUrl"].toString(), QString("https://media.example.test/delete/{json:id}/{secret:secrets/deletion-key-2}"));
        QCOMPARE(result.secrets.value("secrets/deletion-key-2"), first);
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
        QTest::newRow("query value") << "https://h.example.test/get?k=" + t + "&file=a" << "arguments/token";
        QTest::newRow("percent-encoded") << "https://h.example.test/get?k=" + QString::fromLatin1(QUrl::toPercentEncoding(spelled)) << "headers/Authorization";
        QTest::newRow("encoded twice") << "https://h.example.test/get?k=" + QString::fromLatin1(QUrl::toPercentEncoding(QString::fromLatin1(QUrl::toPercentEncoding(spelled)))) << "headers/Authorization";
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
        // A key that is part of a stored body or of a cookie is found by itself.
        QCOMPARE(storedValueIn("https://h.example.test/f/a.png?k=" + inBody, stored), QString("data"));
        QCOMPARE(storedValueIn("https://h.example.test/f/" + inCookie + "/a.png", stored), QString("headers/Cookie"));
        QCOMPARE(scrubStoredValues("key " + inBody + " is not valid", stored), QString("key <redacted> is not valid"));
        QCOMPARE(scrubStoredValues("session " + inCookie + " expired", stored), QString("session <redacted> expired"));
        // So is a short value that the stored text itself names as a credential.
        QCOMPARE(scrubStoredValues("wrong password hunter2x", stored), QString("wrong password <redacted>"));
        QCOMPARE(storedValueIn("https://h.example.test/f/a.png?p=hunter2x", stored), QString("data"));
        // And a key written in base64, either alphabet, wherever it starts in the text.
        for (const auto& prefix : QStringList{"", "a", "ab"})
            for (const auto options : {QByteArray::Base64Encoding, QByteArray::Base64UrlEncoding}) {
                const auto encoded = QString::fromLatin1((prefix + inBody + "!").toUtf8().toBase64(options | QByteArray::OmitTrailingEquals));
                QVERIFY2(!storedValueIn("https://h.example.test/f/a.png?k=" + encoded, stored).isEmpty(), qPrintable(encoded));
                QVERIFY2(!scrubStoredValues("refused " + encoded, stored).contains(encoded), qPrintable(encoded));
            }
    }
    // A part of a credential written out in an address, here the token of "Bearer <token>",
    // never stays in the settings.
    void importStoresAPartOfACredentialFoundInAnAddress() {
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
    void importKeepsEveryValueOutOfSettings() {
        const auto result = importSxcu(sxcu(zipline()));
        QVERIFY2(!result.error, qPrintable(result.error.message));
        const auto s = result.settings;
        QVERIFY2(!validateSettings(s), qPrintable(validateSettings(s).message));
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
        QTest::newRow("truncated") << sxcu(base).left(40) << "sxcu_invalid" << "JSON object";
        QTest::newRow("select") << with({{"Arguments", QJsonObject{{"album", "{select:a|b}"}}}}) << "sxcu_unsupported" << "value of argument 1 uses a placeholder put.gd cannot send";
        QTest::newRow("managed header") << with({{"Headers", QJsonObject{{"Content-Length", "12"}, {"authorization", token}}}}) << "" << "left out";
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
    void importLegacyFields() {
        // Before 12.4: RequestType, no Body, RegexList and ResponseType.
        const QJsonObject file{{"Name", "legacy"}, {"RequestType", "POST"}, {"RequestURL", "https://old.example.test/upload.php?key=abc123&mode=json"},
                               {"FileFormName", "upload"}, {"Arguments", QJsonObject{{"title", "$filename$"}, {"secret", "s3cr3t-value"}}},
                               {"RegexList", QJsonArray{"\"link\":\"(.+?)\""}}, {"URL", "$regex:1,1$"}, {"DeletionURL", "https://old.example.test/del/$json:id$"}};
        const auto result = importSxcu(sxcu(file));
        QVERIFY2(!result.error, qPrintable(result.error.message));
        const auto s = result.settings;
        // The query moved to parameters, each a secret.
        QCOMPARE(s["requestUrl"].toString(), QString("https://old.example.test/upload.php"));
        QCOMPARE(result.secrets.value("parameters/key"), QString("abc123"));
        QCOMPARE(result.secrets.value("parameters/mode"), QString("json"));
        const auto bytes = QJsonDocument(s).toJson();
        QVERIFY(!bytes.contains("abc123"));
        QVERIFY(!bytes.contains("s3cr3t-value"));
    }
    void importTextBodies() {
        const QJsonObject paste{{"Version", "15.0.0"}, {"Name", "paste"}, {"DestinationType", "TextUploader"}, {"RequestMethod", "POST"},
                                {"RequestURL", "https://paste.example.test/api"}, {"Body", "JSON"}, {"Data", QString("{\"key\":\"%1\",\"text\":\"{input}\"}").arg(token)},
                                {"Arguments", QJsonObject{{"ignored", "x"}}}, {"URL", "https://paste.example.test/{json:id}"}};
        const auto result = importSxcu(sxcu(paste));
        QVERIFY2(!result.error, qPrintable(result.error.message));
        // The Data body is a secret as a whole; Arguments are not sent with a JSON body.
        QCOMPARE(result.settings["dataSecret"].toBool(), true);
        QCOMPARE(result.settings["data"].toString(), QString());
        QVERIFY(result.secrets.value(dataSecretKey()).contains(token));
        QVERIFY(!QJsonDocument(result.settings).toJson().contains(token));
    }
    void settingsRules_data() {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QJsonValue>("value");
        QTest::addColumn<QString>("fragment"); // empty: accepted
        QTest::newRow("defaults") << "" << QJsonValue() << "";
        QTest::newRow("secret row with value") << "headers" << QJsonValue(QJsonArray{QJsonObject{{"name", "Authorization"}, {"secret", true}, {"value", token}}}) << "unless secret";
        QTest::newRow("secret value with a value") << "secrets" << QJsonValue(QJsonArray{QJsonObject{{"name", "url-key"}, {"secret", true}, {"value", token}}}) << "no value in settings";
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
        // Plain HTTP beyond loopback needs the destination's own switch.
        settings = defaults();
        settings["requestUrl"] = "http://192.168.68.71:18361/api/upload";
        QCOMPARE(validateSettings(settings).code, QString("http_insecure_url"));
        settings["allowInsecureHttp"] = true;
        QVERIFY(!validateSettings(settings));
    }
    // Credentials follow a redirect only to the same scheme, host and port, and plain
    // HTTP is refused beyond loopback unless the destination allows it.
    void hostsAndTransports() {
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
    }
};
QTEST_APPLESS_MAIN(HttpUploaderRules)
#include "test_http_uploader.moc"
