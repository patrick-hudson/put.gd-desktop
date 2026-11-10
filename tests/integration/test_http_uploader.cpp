// Custom HTTP uploader (#1361): the production provider and putgd-http-worker against the
// scripted loopback receiver, and the journal, retry and unknown-outcome behaviour through
// the real operation and history services. Synthetic token and payloads only.
#include "fixtures/U2Fixtures.h"
#include "fixtures/Waits.h"
#include "storage/http/HttpProvider.h"
#include "storage/http/HttpTemplate.h"
#include <QDirIterator>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <future>
#include <tuple>
using namespace putgd;
using namespace putgd::test;
namespace {
const QString token = "putgd-synthetic-uploader-token-not-real";
QJsonArray rows(std::initializer_list<QJsonObject> list) { QJsonArray out; for (const auto& row : list) out.append(row); return out; }
QJsonObject secretRow(const QString& name) { return {{"name", name}, {"secret", true}}; }
QJsonObject plainRow(const QString& name, const QString& value) { return {{"name", name}, {"secret", false}, {"value", value}}; }
class HttpFixture {
public:
    QProcess process;
    QTemporaryDir directory;
    CredentialStore credentials;
    HttpUploaderProvider provider;
    QString base;
    Destination destination;
    HttpFixture() : provider(credentials) {
        process.start("python3", {HTTP_FIXTURE_PATH});
        if (!process.waitForStarted() || !process.waitForReadyRead()) qFatal("fixture failed to start");
        base = "http://127.0.0.1:" + QString::fromUtf8(process.readLine()).trimmed();
        destination = {"custom-http", "fixture-profile", "revision-one", settings("json"), {{"mode", "session"}, {"reference", QJsonValue::Null}}};
        store({{"headers/Authorization", "Bearer " + token}});
    }
    ~HttpFixture() { process.terminate(); process.waitForFinished(); }
    // A multipart upload to the reply style named, with the token in a secret header.
    QJsonObject settings(const QString& style) const {
        auto s = http::defaults();
        s["requestUrl"] = base + "/upload/" + style;
        s["headers"] = rows({secretRow("Authorization")});
        s["requestTimeoutMs"] = 5000;
        s["connectTimeoutMs"] = 2000;
        if (style == "json" || style == "see" || style == "away" || style == "moved" || style == "confirm" || style == "pretend") { s["responseUrl"] = "{json:files[0].url}"; s["thumbnailUrl"] = "{json:files[0].thumb}"; s["deletionUrl"] = "{json:files[0].delete}"; }
        s["errorMessage"] = "{json:error}";
        return s;
    }
    void store(const QMap<QString, QString>& values) {
        Credentials value;
        value.values = values;
        credentials.setSession(destination.profileId, value);
    }
    QJsonObject control(const QString& path, const QJsonObject& value = {}, bool post = false) {
        QTcpSocket socket;
        const QUrl address(base);
        socket.connectToHost(address.host(), quint16(address.port()));
        if (!socket.waitForConnected(5000)) qFatal("fixture control connection failed");
        const auto body = post ? QJsonDocument(value).toJson(QJsonDocument::Compact) : QByteArray{};
        socket.write((post ? QByteArray("POST ") : QByteArray("GET ")) + path.toUtf8() + " HTTP/1.1\r\nHost: " + address.authority().toUtf8() +
                     "\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
        QByteArray response;
        QElapsedTimer deadline;
        deadline.start();
        while (deadline.elapsed() < 5000) {
            if (!socket.bytesAvailable()) socket.waitForReadyRead(100);
            response += socket.readAll();
            const auto boundary = response.indexOf("\r\n\r\n");
            if (boundary < 0) continue;
            qint64 length = -1;
            for (auto header : response.left(boundary).split('\n'))
                if (header.trimmed().toLower().startsWith("content-length:")) length = header.trimmed().mid(15).trimmed().toLongLong();
            if (length >= 0 && response.size() - boundary - 4 >= length) return QJsonDocument::fromJson(response.mid(boundary + 4, length)).object();
        }
        qFatal("fixture control response timed out");
        return {};
    }
    void reset(const QJsonObject& value = {}) { control("/__reset", value, true); }
    void rules(std::initializer_list<QJsonObject> list) { QJsonArray out; for (const auto& rule : list) out.append(rule); control("/__rules", {{"rules", out}}, true); }
    QJsonObject state() { return control("/__state"); }
    QJsonArray events(const QString& call) {
        QJsonArray out;
        for (const auto& e : state()["events"].toArray()) if (e.toObject()["call"] == call) out.append(e);
        return out;
    }
    int stored() { int count = 0; for (const auto& file : state()["files"].toObject()) if (!file.toObject()["deleted"].toBool()) ++count; return count; }
    Artifact artifact(const QString& name, const QByteArray& bytes, const QString& mime, const QString& category) {
        Artifact out;
        out.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        out.path = fixture(directory.path(), out.id, bytes);
        out.name = name;
        out.mime = mime;
        out.category = category;
        out.bytes = bytes.size();
        out.sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        return out;
    }
    Artifact image(const QByteArray& bytes = "\x89PNG synthetic image bytes \x01\x02\x03") { return artifact("shot one.png", bytes, "image/png", "images"); }
    Artifact text(const QByteArray& bytes = "line one\n<two> & \"three\"\n") { return artifact("note.txt", bytes, "text/plain", "text"); }
    UploadResult upload(const Artifact& artifact, const Destination& target, std::shared_ptr<std::atomic_bool> cancel = {}, QStringList* phases = nullptr) {
        NameCheck check;
        provider.checkName(target, artifact.name, [&](NameCheck result) { check = result; });
        if (check.status != "absent") return {"failed", {}, check.error};
        UploadRequest request{target, artifact, artifact.name, "publication-" + artifact.id, "absent", check.objectReference, check.concurrencyToken, cancel};
        if (phases) request.progress = [phases](const UploadProgress& progress) { if (phases->isEmpty() || phases->last() != progress.phase) phases->append(progress.phase); };
        UploadResult out;
        provider.upload(request, [&](UploadResult result) { out = result; });
        return out;
    }
    UploadResult upload(const Artifact& artifact) { return upload(artifact, destination); }
    DeleteResult remove(const StorageReceipt& receipt, const QJsonObject& progress = {}, DurableCheckpoint checkpoint = {}) {
        DeleteResult out;
        provider.remove({"delete-" + receipt.publicationId, receipt, progress, {}}, checkpoint ? checkpoint : [](const QJsonObject&, std::function<void(Error)> ack) { ack({}); },
                        [&](DeleteResult result) { out = result; });
        return out;
    }
    // A plain GET of a link, as a browser would make it: status and body.
    QPair<int, QByteArray> fetch(const QString& link) {
        const QUrl url(link);
        QTcpSocket socket;
        socket.connectToHost(url.host(), quint16(url.port()));
        if (!socket.waitForConnected(5000)) return {0, {}};
        socket.write("GET " + url.toEncoded(QUrl::RemoveScheme | QUrl::RemoveAuthority) + " HTTP/1.1\r\nHost: " + url.authority().toUtf8() + "\r\nConnection: close\r\n\r\n");
        QByteArray response;
        while (socket.waitForReadyRead(3000)) response += socket.readAll();
        response += socket.readAll();
        const auto boundary = response.indexOf("\r\n\r\n");
        return {response.mid(9, 3).toInt(), boundary < 0 ? QByteArray() : response.mid(boundary + 4)};
    }
    bool tokenOnDisk(const QString& root) {
        QDirIterator files(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            QFile file(files.next());
            if (file.open(QIODevice::ReadOnly) && file.readAll().contains(token.toUtf8())) return true;
        }
        return false;
    }
};
}
class HttpUploaderTest : public QObject {
    Q_OBJECT
    static QJsonObject configuration(HttpFixture& f, const QString& policy = "rename") {
        auto doc = putgd::test::configuration(f.directory.path());
        auto naming = doc["naming"].toObject();
        naming["mode"] = "original";
        naming["collisionPolicy"] = policy;
        doc["naming"] = naming;
        doc["profiles"] = QJsonArray{QJsonObject{{"id", f.destination.profileId}, {"label", "Scripted receiver"}, {"provider", "custom-http"}, {"settings", f.destination.settings}, {"credentials", f.destination.credentialReference}}};
        auto actions = doc["actions"].toObject();
        for (const auto& key : Settings::actionNames()) {
            auto action = actions[key].toObject();
            action["profileId"] = f.destination.profileId;
            actions[key] = action;
        }
        doc["actions"] = actions;
        return doc;
    }
    static Operation waitOperation(OperationService& operations, const QString& id) {
        Operation result;
        const bool terminal = waitFor([&] {
            HistoryStore store(operations.databasePath());
            result = store.operation(id);
            return QStringList{"completed", "partial", "failed", "cancelled"}.contains(result.state);
        }, 20000);
        if (!terminal) QTest::qFail("Operation did not reach a terminal state", __FILE__, __LINE__);
        return result;
    }
private slots:
    void capabilitiesAreHonest() {
        HttpFixture f;
        const auto caps = f.provider.capabilities(f.destination);
        QVERIFY(caps.serverNamesObjects);
        QVERIFY(!caps.conditionalCreate);
        QVERIFY(!caps.conditionalReplace);
        QVERIFY(!caps.stat);
        QVERIFY(!caps.versionAddressing);
        QVERIFY(caps.remove); // this destination names a deletion URL
        QVERIFY(caps.safeRemoval());
        auto plain = f.destination;
        plain.settings = f.settings("text");
        QVERIFY(!f.provider.capabilities(plain).remove);
        QVERIFY(!f.provider.capabilities({}).remove);
        StatResult stat;
        f.provider.stat({}, [&](StatResult r) { stat = r; });
        QCOMPARE(stat.status, QString("unknown"));
        f.provider.reconcile({}, [&](StatResult r) { stat = r; });
        QCOMPARE(stat.status, QString("unknown"));
        QCOMPARE(stat.error.code, QString("unsupported_capability"));
        // Two checks of one name are two objects: the server names what it stores.
        NameCheck first, second;
        f.provider.checkName(f.destination, "a.png", [&](NameCheck r) { first = r; });
        f.provider.checkName(f.destination, "a.png", [&](NameCheck r) { second = r; });
        QCOMPARE(first.status, QString("absent"));
        QVERIFY(first.objectReference != second.objectReference);
        QVERIFY(f.provider.serializationKey(f.destination, first.objectReference, "a.png") != f.provider.serializationKey(f.destination, second.objectReference, "a.png"));
        QCOMPARE(f.events("upload").size(), 0);
    }
    // A diagnostics package says which fields a destination sets and where its request
    // goes: never a row value, a path someone typed a key into, or a reply template's text.
    void diagnosticsShowTheShapeOfSettingsNotTheirText() {
        HttpFixture f;
        auto settings = http::defaults();
        settings["requestUrl"] = "https://media.example.test:8443/hooks/typed-in-key/{filename}";
        settings["headers"] = QJsonArray{QJsonObject{{"name", "Authorization"}, {"secret", true}}, QJsonObject{{"name", "X-Album"}, {"secret", false}, {"value", "private-album"}}};
        settings["data"] = "{\"album\":\"private-album\"}";
        settings["responseUrl"] = "https://cdn.example.test/typed-in-key/{json:files[0].name}";
        settings["deletionUrl"] = "{json:url}/delete/{secret:headers/Authorization}";
        settings["errorMessage"] = "typed-in-key {regex:error=(\\\\w+)|1}";
        const auto shown = f.provider.diagnosticSettings(settings);
        QCOMPARE(shown["requestUrl"].toString(), QString("https://media.example.test:8443/<set: filename>"));
        QCOMPARE(shown["headers"].toArray(), QJsonArray({QJsonObject{{"name", "Authorization"}, {"secret", true}}, QJsonObject{{"name", "X-Album"}, {"secret", false}}}));
        QCOMPARE(shown["data"].toString(), QString("<set>"));
        QCOMPARE(shown["responseUrl"].toString(), QString("<set: json>"));
        QCOMPARE(shown["deletionUrl"].toString(), QString("<set: json, secret>"));
        QCOMPARE(shown["errorMessage"].toString(), QString("<set: regex>"));
        QCOMPARE(shown["thumbnailUrl"].toString(), QString());
        const auto bytes = QJsonDocument(shown).toJson();
        QVERIFY(!bytes.contains("typed-in-key") && !bytes.contains("private-album") && !bytes.contains("files[0]"));
        QCOMPARE(QStringList(shown.keys()), QStringList(settings.keys()));
    }
    void bodyTypes_data() {
        QTest::addColumn<QString>("body");
        QTest::addColumn<QString>("method");
        QTest::addColumn<bool>("textFile");
        QTest::newRow("multipart file") << "multipart" << "POST" << false;
        QTest::newRow("multipart text as file") << "multipart" << "POST" << true;
        QTest::newRow("multipart fields only") << "multipart-fields" << "POST" << true;
        QTest::newRow("binary") << "binary" << "PUT" << false;
        QTest::newRow("binary patch") << "binary" << "PATCH" << false;
        QTest::newRow("form") << "form" << "POST" << true;
        QTest::newRow("json") << "json" << "POST" << true;
        QTest::newRow("xml") << "xml" << "POST" << true;
        QTest::newRow("none") << "none" << "GET" << true;
    }
    void bodyTypes() {
        QFETCH(QString, body);
        QFETCH(QString, method);
        QFETCH(bool, textFile);
        HttpFixture f;
        f.reset();
        auto s = f.settings("json");
        s["requestMethod"] = method;
        s["body"] = body == "multipart-fields" ? QString("multipart") : body;
        const auto artifact = textFile ? f.text() : f.image();
        const auto content = QString::fromUtf8(textFile ? QByteArray("line one\n<two> & \"three\"\n") : QByteArray());
        if (body == "multipart") s["arguments"] = rows({plainRow("name", "{filename}"), plainRow("album", "lab")});
        if (body == "multipart-fields") { s["fileFormName"] = ""; s["arguments"] = rows({plainRow("content", "{input}"), plainRow("name", "{filename}")}); }
        if (body == "binary") s["parameters"] = rows({plainRow("name", "{filename}")});
        if (body == "form") s["arguments"] = rows({plainRow("content", "{input}"), plainRow("name", "{filename}")});
        if (body == "json") s["data"] = "{\"content\":\"{input}\",\"name\":\"{filename}\",\"fixed\":{\"n\":1}}";
        if (body == "xml") s["data"] = "<paste name=\"{filename}\">{input}</paste>";
        if (body == "none") s["parameters"] = rows({plainRow("content", "{input}"), plainRow("name", "{filename}")});
        QVERIFY2(!http::validateSettings(s), qPrintable(http::validateSettings(s).message));
        auto destination = f.destination;
        destination.settings = s;
        QStringList phases;
        const auto uploaded = f.upload(artifact, destination, {}, &phases);
        QVERIFY2(uploaded.status == "uploaded", qPrintable(uploaded.error.code + ": " + uploaded.error.message));
        const auto event = f.events("upload")[0].toObject();
        QCOMPARE(event["method"].toString(), method);
        QCOMPARE(event["kind"].toString(), body == "multipart-fields" ? QString("multipart") : body);
        QCOMPARE(event["headers"].toObject()["authorization"].toString(), "Bearer " + token);
        QVERIFY(event["headers"].toObject()["user-agent"].toString().startsWith("put.gd/"));
        if (body == "xml") {
            // The receiver keeps an XML body as sent: the text was escaped for XML.
            QCOMPARE(f.fetch(uploaded.receipt.directUrl).second, QByteArray("<paste name=\"note.txt\">line one\n&lt;two&gt; &amp; &quot;three&quot;\n</paste>"));
        } else {
            // What the receiver stored is the file, or its text sent through {input}.
            QCOMPARE(event["sha256"].toString(), artifact.sha256);
            QCOMPARE(event["fileName"].toString(), artifact.name);
        }
        if (body == "multipart") {
            QCOMPARE(event["fields"].toObject()["__file_field"].toString(), QString("file"));
            QCOMPARE(event["fields"].toObject()["album"].toString(), QString("lab"));
            QCOMPARE(event["fields"].toObject()["name"].toString(), artifact.name);
            QCOMPARE(event["fileType"].toString(), artifact.mime);
        }
        if (body == "binary") QCOMPARE(event["headers"].toObject()["content-type"].toString(), artifact.mime);
        if (body == "json") QCOMPARE(event["fields"].toObject()["content"].toString(), content);
        // The receipt carries what the reply named.
        QCOMPARE(uploaded.receipt.objectReference["url"].toString(), uploaded.receipt.directUrl);
        QVERIFY(uploaded.receipt.objectReference["deletionUrl"].toString().contains("/delete/"));
        QVERIFY(uploaded.receipt.objectReference["thumbnailUrl"].toString().endsWith("/thumb"));
        QVERIFY(!uploaded.receipt.validate());
        // A file body reports its phases; a text request has nothing to count.
        if (body == "multipart" || body == "binary") QCOMPARE(phases, QStringList({"preparing", "uploading", "finalizing"}));
        QCOMPARE(f.stored(), 1);
    }
    void replyStyles_data() {
        QTest::addColumn<QString>("style");
        QTest::addColumn<QString>("url");
        QTest::addColumn<QString>("deletion");
        QTest::newRow("json") << "json" << "{json:files[0].url}" << "{json:files[0].delete}";
        QTest::newRow("old json") << "json" << http::migrateOldSyntax("$json:files[0].url$") << http::migrateOldSyntax("$json:files[0].delete$");
        QTest::newRow("xml") << "xml" << "{xml:/upload/file/url}" << "{xml:/upload/file/delete/@href}";
        QTest::newRow("regex") << "html" << "{regex:class=\"link\" href=\"([^\"]+)\"|1}" << "{regex:class=\"remove\" href=\"([^\"]+)\"|1}";
        QTest::newRow("header") << "header" << "{header:Location}" << "{header:X-Delete}";
        QTest::newRow("whole reply") << "text" << "" << "";
        QTest::newRow("response placeholder") << "text" << "{response}" << "";
        QTest::newRow("built around an id") << "bare" << "/f/{json:id}/{json:name}" << "";
        QTest::newRow("redirected reply") << "see" << "{json:files[0].url}" << "{json:files[0].delete}";
        QTest::newRow("response url") << "see" << "{responseurl}" << "";
    }
    void replyStyles() {
        QFETCH(QString, style);
        QFETCH(QString, url);
        QFETCH(QString, deletion);
        HttpFixture f;
        f.reset();
        auto destination = f.destination;
        auto s = f.settings(style);
        s["responseUrl"] = url;
        s["deletionUrl"] = deletion;
        s["thumbnailUrl"] = "";
        QVERIFY2(!http::validateSettings(s), qPrintable(http::validateSettings(s).message));
        destination.settings = s;
        const auto artifact = f.image();
        const auto uploaded = f.upload(artifact, destination);
        QVERIFY2(uploaded.status == "uploaded", qPrintable(uploaded.error.code + ": " + uploaded.error.message));
        const auto link = uploaded.receipt.directUrl;
        if (url == "{responseurl}") QVERIFY2(link.contains("/result/"), qPrintable(link));
        else {
            QVERIFY2(link.startsWith(f.base + "/f/"), qPrintable(link));
            const auto fetched = f.fetch(link);
            QCOMPARE(fetched.first, 200);
            QCOMPARE(qint64(fetched.second.size()), artifact.bytes);
        }
        QCOMPARE(uploaded.receipt.objectReference.contains("deletionUrl"), !deletion.isEmpty());
        QCOMPARE(f.provider.receiptCapabilities(uploaded.receipt).remove, !deletion.isEmpty());
        if (deletion.isEmpty()) QVERIFY(f.provider.receiptCapabilities(uploaded.receipt).removeUnavailable.contains("deletion URL"));
    }
    void tokenGoesWhereItIsConfigured_data() {
        QTest::addColumn<QString>("auth");
        QTest::newRow("header") << "header";
        QTest::newRow("query") << "query";
        QTest::newRow("argument") << "argument";
    }
    void tokenGoesWhereItIsConfigured() {
        QFETCH(QString, auth);
        HttpFixture f;
        f.reset({{"auth", auth}});
        auto destination = f.destination;
        auto s = f.settings("json");
        s["headers"] = QJsonArray{};
        if (auth == "header") { s["headers"] = rows({secretRow("Authorization")}); f.store({{"headers/Authorization", token}}); }
        if (auth == "query") { s["parameters"] = rows({secretRow("key")}); f.store({{"parameters/key", token}}); }
        if (auth == "argument") { s["arguments"] = rows({secretRow("key"), plainRow("name", "{filename}")}); f.store({{"arguments/key", token}}); }
        destination.settings = s;
        const auto uploaded = f.upload(f.image(), destination);
        QVERIFY2(uploaded.status == "uploaded", qPrintable(uploaded.error.code + ": " + uploaded.error.message));
        const auto event = f.events("upload")[0].toObject();
        QCOMPARE(event["headers"].toObject().contains("authorization"), auth == "header");
        QCOMPARE(event["query"].toObject().contains("key"), auth == "query");
        QCOMPARE(event["fields"].toObject().contains("key"), auth == "argument");
        // The receipt and its destination hold names, never the value.
        QVERIFY(!QJsonDocument(uploaded.receipt.toJson()).toJson().contains(token.toUtf8()));
    }
    void missingValuesStopBeforeAnyRequest() {
        HttpFixture f;
        f.reset();
        f.credentials.forget(f.destination.profileId, f.destination.credentialReference);
        NameCheck check;
        f.provider.checkName(f.destination, "a.png", [&](NameCheck r) { check = r; });
        QCOMPARE(check.status, QString("inconclusive"));
        QCOMPARE(check.error.code, QString("credential_setup_required"));
        f.store({{"headers/Other", "x"}});
        f.provider.checkName(f.destination, "a.png", [&](NameCheck r) { check = r; });
        QCOMPARE(check.error.code, QString("credential_setup_required"));
        QVERIFY(check.error.message.contains("header Authorization"));
        QCOMPARE(f.provider.readiness(f.destination).code, QString("credential_setup_required"));
        // No request URL yet is a setup state, not a crash or a request.
        auto unset = f.destination;
        unset.settings["requestUrl"] = "";
        QCOMPARE(f.provider.readiness(unset).code, QString("http_setup_required"));
        QCOMPARE(f.events("upload").size(), 0);
    }
    void refusalsAreDefiniteAndNeverEchoTheToken() {
        HttpFixture f;
        f.reset();
        // A wrong token: the receiver repeats it in its error body.
        f.store({{"headers/Authorization", "Bearer wrong-" + token}});
        auto refused = f.upload(f.image());
        QCOMPARE(refused.status, QString("failed"));
        QCOMPARE(refused.error.code, QString("http_authentication_failed"));
        QVERIFY2(refused.error.message.contains("<redacted>"), qPrintable(refused.error.message));
        QVERIFY(!refused.error.message.contains(token));
        QVERIFY(!QJsonDocument(refused.error.details).toJson().contains(token.toUtf8()));
        QCOMPARE(refused.error.details["httpStatus"].toInt(), 401);
        f.store({{"headers/Authorization", "Bearer " + token}});
        for (const auto& [status, code, fragment] : QList<std::tuple<int, QString, QString>>{{400, "http_request_refused", "quota is full"}, {413, "http_too_large", "quota is full"},
                 {429, "http_rate_limited", "quota is full"}, {500, "http_server_error", "quota is full"}, {503, "http_server_error", "quota is full"}}) {
            f.rules({{{"call", "upload"}, {"status", status}, {"body", QJsonObject{{"error", "quota is full"}}}}});
            const auto result = f.upload(f.image());
            QCOMPARE(result.status, QString("failed"));
            QCOMPARE(result.error.code, code);
            QVERIFY2(result.error.message.contains(fragment) && result.error.message.contains(QString::number(status)), qPrintable(result.error.message));
        }
        // An error body that is not what the template expects still fails cleanly.
        f.rules({{{"call", "upload"}, {"status", 418}, {"body", "<html>teapot</html>"}, {"type", "text/html"}}});
        const auto odd = f.upload(f.image());
        QCOMPARE(odd.status, QString("failed"));
        QVERIFY(odd.error.message.contains("418"));
        QCOMPARE(f.stored(), 0);
    }
    void whatThisUploaderTakes() {
        HttpFixture f;
        f.reset();
        auto images = f.destination;
        images.settings["accepts"] = QJsonArray{"image"};
        auto refused = f.upload(f.text(), images);
        QCOMPARE(refused.status, QString("failed"));
        QCOMPARE(refused.error.code, QString("http_type_unsupported"));
        QVERIFY(refused.error.message.contains("images only"));
        QCOMPARE(f.upload(f.image(), images).status, QString("uploaded"));
        // A request that only carries text cannot send an image.
        auto paste = f.destination;
        paste.settings["body"] = "form";
        paste.settings["arguments"] = rows({plainRow("content", "{input}")});
        refused = f.upload(f.image(), paste);
        QCOMPARE(refused.status, QString("failed"));
        QCOMPARE(refused.error.code, QString("http_text_only"));
        QCOMPARE(f.events("upload").size(), 1);
    }
    void lostAndLateRepliesAreUnknown() {
        HttpFixture f;
        f.reset();
        // Stored, then the connection drops: the reply is lost.
        f.rules({{{"call", "upload"}, {"store", true}, {"drop", true}}});
        auto lost = f.upload(f.image());
        QCOMPARE(lost.status, QString("unknown"));
        QCOMPARE(lost.error.code, QString("http_outcome_unknown"));
        QVERIFY2(lost.error.message.contains("second copy"), qPrintable(lost.error.message));
        QCOMPARE(f.stored(), 1);
        // Dropped before storing looks the same from here, and is reported the same.
        f.rules({{{"call", "upload"}, {"drop", true}}});
        lost = f.upload(f.image());
        QCOMPARE(lost.status, QString("unknown"));
        QCOMPARE(f.stored(), 1);
        // Exactly one request each: the worker never sends an upload twice by itself.
        QCOMPARE(f.events("upload").size(), 2);
        // A gateway that gave up says nothing about what was stored.
        for (const int status : {408, 502, 504}) {
            f.rules({{{"call", "upload"}, {"store", true}, {"status", status}}});
            const auto result = f.upload(f.image());
            QCOMPARE(result.status, QString("unknown"));
            QCOMPARE(result.error.code, QString("http_outcome_unknown"));
        }
        // A reply slower than the request timeout.
        auto slow = f.destination;
        slow.settings["requestTimeoutMs"] = 1000;
        f.rules({{{"call", "upload"}, {"delay", 3}}});
        QElapsedTimer clock;
        clock.start();
        const auto late = f.upload(f.image(), slow);
        QCOMPARE(late.status, QString("unknown"));
        QVERIFY2(late.error.message.contains("request timeout"), qPrintable(late.error.message));
        QVERIFY(clock.elapsed() < 2900);
        // A 2xx without a link where the destination looks: stored, but unreadable.
        auto nolink = f.destination;
        nolink.settings = f.settings("nolink");
        const auto unread = f.upload(f.image(), nolink);
        QCOMPARE(unread.status, QString("unknown"));
        QCOMPARE(unread.error.code, QString("http_response_unreadable"));
    }
    void unreachableServerIsDefinite() {
        HttpFixture f;
        f.reset();
        // A port nothing listens on: the request never left.
        QTcpServer closed;
        QVERIFY(closed.listen(QHostAddress::LocalHost));
        const auto port = closed.serverPort();
        closed.close();
        auto away = f.destination;
        away.settings["requestUrl"] = QString("http://127.0.0.1:%1/upload/json").arg(port);
        const auto refused = f.upload(f.image(), away);
        QCOMPARE(refused.status, QString("failed"));
        QCOMPARE(refused.error.code, QString("http_connect_failed"));
        QCOMPARE(f.events("upload").size(), 0);
    }
    void cancelledAfterSendingIsUnknown() {
        HttpFixture f;
        f.reset();
        f.rules({{{"call", "upload"}, {"delay", 4}}});
        auto cancel = std::make_shared<std::atomic_bool>(false);
        auto pending = std::async(std::launch::async, [&] { return f.upload(f.image(), f.destination, cancel); });
        // The receiver has the whole request once it records the upload.
        QVERIFY(waitFor([&] { return f.events("upload").size() == 1; }));
        cancel->store(true);
        QVERIFY(pending.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
        QCOMPARE(pending.get().status, QString("unknown"));
    }
    void redirectsKeepCredentialsOnTheirHost() {
        HttpFixture f;
        f.reset();
        auto destination = f.destination;
        // 303 to a result on another host name: followed with a GET that carries nothing.
        destination.settings = f.settings("away");
        auto uploaded = f.upload(f.image(), destination);
        QVERIFY2(uploaded.status == "uploaded", qPrintable(uploaded.error.code + ": " + uploaded.error.message));
        auto result = f.events("result");
        QCOMPARE(result.size(), 1);
        QVERIFY(result[0].toObject()["headers"].toObject()["host"].toString().startsWith("localhost"));
        QVERIFY(!result[0].toObject()["headers"].toObject().contains("authorization"));
        // On its own host the follow-up may identify itself.
        destination.settings = f.settings("see");
        uploaded = f.upload(f.image(), destination);
        QCOMPARE(uploaded.status, QString("uploaded"));
        QVERIFY(f.events("result")[1].toObject()["headers"].toObject().contains("authorization"));
        // 307 on the same host: the same request is sent again, there.
        f.reset();
        destination.settings = f.settings("moved");
        uploaded = f.upload(f.image(), destination);
        QVERIFY2(uploaded.status == "uploaded", qPrintable(uploaded.error.code + ": " + uploaded.error.message));
        QCOMPARE(f.events("upload").size(), 2);
        QCOMPARE(f.stored(), 1);
        // 307 to another host: refused, and the file and token never go there.
        f.reset();
        destination.settings = f.settings("moved-away");
        const auto refused = f.upload(f.image(), destination);
        QCOMPARE(refused.status, QString("failed"));
        QCOMPARE(refused.error.code, QString("http_redirect_refused"));
        QVERIFY2(refused.error.message.contains("another host"), qPrintable(refused.error.message));
        QCOMPARE(f.events("upload").size(), 1);
        QCOMPARE(f.stored(), 0);
        // A redirect loop ends, and nothing was stored.
        f.reset();
        destination.settings = f.settings("loop");
        const auto loop = f.upload(f.image(), destination);
        QCOMPARE(loop.error.code, QString("http_redirect_refused"));
        QVERIFY(f.events("upload").size() <= 7);
    }
    void deletionIsOnePreparedCheckpointedRequest() {
        HttpFixture f;
        f.reset();
        const auto uploaded = f.upload(f.image());
        QCOMPARE(uploaded.status, QString("uploaded"));
        QStringList checkpoints;
        auto removed = f.remove(uploaded.receipt, {}, [&](const QJsonObject& progress, std::function<void(Error)> ack) {
            checkpoints.append(progress["phase"].toString());
            // The request must not exist yet when its intent is acknowledged.
            if (progress["phase"] == "delete_intent") QCOMPARE(f.events("delete").size(), 0);
            QCOMPARE(progress["target"].toObject(), uploaded.receipt.toJson());
            ack({});
        });
        QCOMPARE(removed.status, QString("removed"));
        QCOMPARE(checkpoints, QStringList({"delete_intent", "done"}));
        // The deletion request carries the deletion URL and nothing else.
        const auto event = f.events("delete")[0].toObject();
        QVERIFY(!event["headers"].toObject().contains("authorization"));
        QCOMPARE(f.fetch(uploaded.receipt.directUrl).first, 404);
        QCOMPARE(f.stored(), 0);
        // Done stays done; a fresh request for a file that is gone reports it missing.
        QCOMPARE(f.remove(uploaded.receipt, removed.progress).status, QString("removed"));
        QCOMPARE(f.remove(uploaded.receipt).status, QString("missing"));
        QCOMPARE(f.events("delete").size(), 2);
    }
    void deletionUrlNamesAStoredValueInsteadOfRecordingIt() {
        // As XBackBone's deletion URL does: a stored value is part of the address.
        HttpFixture f;
        f.reset();
        const QString key = "k-" + token;
        auto destination = f.destination;
        destination.settings["secrets"] = rows({secretRow("delete-key")});
        destination.settings["deletionUrl"] = "{json:files[0].delete}?key={secret:secrets/delete-key}";
        // The request also names {input}, which is empty for a file that is not text.
        destination.settings["arguments"] = rows({plainRow("text", "{input}"), plainRow("file", "{filename}")});
        QVERIFY2(!http::validateSettings(destination.settings), qPrintable(http::validateSettings(destination.settings).message));
        f.store({{"headers/Authorization", "Bearer " + token}, {"secrets/delete-key", key}});
        const auto uploaded = f.upload(f.image(), destination);
        QVERIFY2(uploaded.status == "uploaded", qPrintable(uploaded.error.code + ": " + uploaded.error.message));
        QCOMPARE(f.events("upload")[0].toObject()["fields"].toObject()["text"].toString(), QString());
        // The receipt, which History keeps, has the name and not the value.
        const auto recorded = uploaded.receipt.objectReference["deletionUrl"].toString();
        QVERIFY2(recorded.endsWith("?key={secret:secrets/delete-key}"), qPrintable(recorded));
        QVERIFY(!QJsonDocument(uploaded.receipt.toJson()).toJson().contains(token.toUtf8()));
        // Without the stored value nothing is sent.
        f.credentials.forget(destination.profileId, destination.credentialReference);
        auto result = f.remove(uploaded.receipt);
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.error.code, QString("credential_setup_required"));
        QCOMPARE(f.events("delete").size(), 0);
        // With it, the deletion carries the value, and only there.
        f.store({{"headers/Authorization", "Bearer " + token}, {"secrets/delete-key", key}});
        result = f.remove(uploaded.receipt);
        QCOMPARE(result.status, QString("removed"));
        const auto event = f.events("delete")[0].toObject();
        QCOMPARE(event["query"].toObject()["key"].toString(), key);
        QVERIFY(!event["headers"].toObject().contains("authorization"));
        QVERIFY(!QJsonDocument(result.progress).toJson().contains(token.toUtf8()));
        QCOMPARE(f.stored(), 0);
    }
    void deletionIsConfirmedOnTheServersOwnPageAndThenVerified() {
        HttpFixture f;
        f.reset();
        // The deletion URL shows "are you sure?", as XBackBone's does: its form is sent once.
        auto destination = f.destination;
        destination.settings = f.settings("confirm");
        auto uploaded = f.upload(f.image(), destination);
        QCOMPARE(uploaded.status, QString("uploaded"));
        auto result = f.remove(uploaded.receipt);
        QVERIFY2(result.status == "removed", qPrintable(result.error.code + ": " + result.error.message));
        QCOMPARE(f.events("delete").size(), 1);
        const auto confirmed = f.events("confirm");
        QCOMPARE(confirmed.size(), 1);
        QCOMPARE(confirmed[0].toObject()["fields"].toObject()["csrf"].toString(), QString("synthetic&csrf"));
        QVERIFY(!confirmed[0].toObject()["headers"].toObject().contains("authorization"));
        QCOMPARE(f.fetch(uploaded.receipt.directUrl).first, 404);
        QCOMPARE(f.stored(), 0);
        // A server that answers 200 and deletes nothing is not believed.
        destination.settings = f.settings("pretend");
        uploaded = f.upload(f.image(), destination);
        QCOMPARE(uploaded.status, QString("uploaded"));
        result = f.remove(uploaded.receipt);
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.error.code, QString("http_delete_unconfirmed"));
        QVERIFY2(result.error.message.contains("still serves the file"), qPrintable(result.error.message));
        QCOMPARE(f.fetch(uploaded.receipt.directUrl).first, 200);
        QCOMPARE(f.stored(), 1);
    }
    void deletionFailuresAndLostReplies() {
        HttpFixture f;
        f.reset();
        const auto uploaded = f.upload(f.image());
        // A journal that cannot be saved: no request is sent.
        auto result = f.remove(uploaded.receipt, {}, [](const QJsonObject&, std::function<void(Error)> ack) { ack({"disk_full", "synthetic checkpoint failure", {}}); });
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.error.code, QString("disk_full"));
        QCOMPARE(f.events("delete").size(), 0);
        // The server refuses: nothing changed.
        f.rules({{{"call", "delete"}, {"status", 403}}});
        result = f.remove(uploaded.receipt);
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.error.code, QString("http_delete_refused"));
        QCOMPARE(f.stored(), 1);
        // Deleted, but the connection closes before the reply. Some Qt Network versions
        // repeat a request that has no body, find the file gone, and that is this
        // deletion. Others report the lost reply, and the explicit retry finds it gone.
        f.rules({{{"call", "delete"}, {"store", true}, {"drop", true}}});
        result = f.remove(uploaded.receipt);
        QCOMPARE(f.stored(), 0);
        if (result.status == "unknown") {
            QCOMPARE(result.error.code, QString("http_outcome_unknown"));
            QCOMPARE(result.progress["phase"].toString(), QString("delete_intent"));
            f.rules({});
            result = f.remove(uploaded.receipt, result.progress);
        }
        QCOMPARE(result.status, QString("removed"));
        // No reply however often it is asked: unknown, and the explicit retry finds it gone.
        const auto second = f.upload(f.image());
        f.rules({{{"call", "delete"}, {"store", true}, {"drop", true}, {"remaining", 8}}});
        result = f.remove(second.receipt);
        QCOMPARE(result.status, QString("unknown"));
        QCOMPARE(result.error.code, QString("http_outcome_unknown"));
        QCOMPARE(result.progress["phase"].toString(), QString("delete_intent"));
        QCOMPARE(f.stored(), 0);
        f.rules({});
        QCOMPARE(f.remove(second.receipt, result.progress).status, QString("removed"));
        // Progress recorded for another upload is never applied to this one.
        const auto other = f.upload(f.image());
        QCOMPARE(f.remove(other.receipt, result.progress).status, QString("changed"));
        QCOMPARE(f.stored(), 1);
        // An upload that returned no deletion URL: reported as unsupported, no request.
        auto plain = f.destination;
        plain.settings = f.settings("text");
        const auto nameless = f.upload(f.image(), plain);
        QCOMPARE(nameless.status, QString("uploaded"));
        const auto before = f.events("delete").size();
        result = f.remove(nameless.receipt);
        QCOMPARE(result.status, QString("unsupported_safe_removal"));
        QCOMPARE(result.error.code, QString("http_delete_unsupported"));
        QCOMPARE(f.events("delete").size(), before);
    }
    void testConnectionSaysWhatItDid() {
        HttpFixture f;
        f.reset();
        QStringList phases;
        const auto checkpoint = [&](const QJsonObject& journal, std::function<void(Error)> ack) { phases.append(journal["phase"].toString()); ack({}); };
        QJsonObject result;
        Error error;
        f.provider.testDestination(f.destination, checkpoint, [&](QJsonObject r, Error e) { result = r; error = e; });
        QVERIFY2(!error, qPrintable(error.code + ": " + error.message));
        QCOMPARE(result["uploadStatus"].toString(), QString("uploaded"));
        QCOMPARE(result["publicReachable"].toBool(), true);
        QCOMPARE(result["linkReturnsFile"].toBool(), true);
        QCOMPARE(result["deletionOffered"].toBool(), true);
        QCOMPARE(result["cleanupStatus"].toString(), QString("removed"));
        QCOMPARE(result["kind"].toString(), QString("text"));
        QCOMPARE(phases, QStringList({"upload_intent", "uploaded", "delete_intent", "done"}));
        QCOMPARE(f.stored(), 0);
        // No deletion URL: the test passes and says the file stays.
        auto plain = f.destination;
        plain.settings = f.settings("text");
        f.provider.testDestination(plain, checkpoint, [&](QJsonObject r, Error e) { result = r; error = e; });
        QVERIFY2(!error, qPrintable(error.code + ": " + error.message));
        QCOMPARE(result["cleanupStatus"].toString(), QString("unsupported"));
        QCOMPARE(result["deletionOffered"].toBool(), false);
        QCOMPARE(f.stored(), 1);
        // An image-only uploader is tested with a synthetic image.
        auto images = f.destination;
        images.settings["accepts"] = QJsonArray{"image"};
        f.provider.testDestination(images, checkpoint, [&](QJsonObject r, Error e) { result = r; error = e; });
        QVERIFY2(!error, qPrintable(error.code + ": " + error.message));
        QCOMPARE(result["kind"].toString(), QString("image"));
        QCOMPARE(f.events("upload").last().toObject()["fileType"].toString(), QString("image/png"));
        // A reply the Response fields cannot read: the start of it comes back, without the token.
        auto unread = f.destination;
        unread.settings = f.settings("nolink");
        f.provider.testDestination(unread, checkpoint, [&](QJsonObject r, Error e) { result = r; error = e; });
        QCOMPARE(error.code, QString("http_response_unreadable"));
        QVERIFY(result["replyExcerpt"].toString().contains("ok"));
        // A returned link that does not open.
        f.rules({{{"call", "file"}, {"status", 404}}});
        f.provider.testDestination(plain, checkpoint, [&](QJsonObject r, Error e) { result = r; error = e; });
        QCOMPARE(error.code, QString("public_url_unreachable"));
        QCOMPARE(result["linkStatus"].toInt(), 404);
    }
    // The shared pipeline: journal, explicit retry and history deletion.
    void lostReplyWaitsForAnExplicitRetryThatMayDuplicate() {
        HttpFixture f;
        f.reset();
        ProviderRegistry registry;
        auto provider = std::make_shared<HttpUploaderProvider>(f.credentials);
        registry.add("custom-http", provider);
        SettingsService settings(f.directory.filePath("config.json"), [&](auto id, auto value) { return registry.validate(id, value); });
        const auto saved = settings.save(configuration(f, "skip"), {});
        QVERIFY2(saved, qPrintable(saved.error.code + ": " + saved.error.message));
        Source source;
        source.payload.kind = "files";
        source.payload.files = {fixture(f.directory.path(), "report.pdf", "synthetic report")};
        OperationService operations(f.directory.filePath("data"), settings, registry, source);
        HistoryService history(operations);
        PUTGD_TRY_VERIFY(operations.ready());
        f.rules({{{"call", "upload"}, {"store", true}, {"drop", true}}});
        QString id;
        operations.invoke("clipboard", [&](auto error, auto result) { QVERIFY(!error); id = result["operationId"].toString(); });
        PUTGD_TRY_VERIFY(!id.isEmpty());
        auto operation = waitOperation(operations, id);
        QCOMPARE(operation.state, QString("partial"));
        QCOMPARE(operation.items[0].uploadStatus, QString("unknown"));
        QCOMPARE(operation.items[0].error.code, QString("http_outcome_unknown"));
        QCOMPARE(f.stored(), 1);
        // Nothing is sent again on its own.
        PUTGD_COMPARE_STEADY(f.events("upload").size(), 1, 700);
        // History says what Retry will do for this destination.
        QJsonObject detail;
        history.detail(operation.items[0].artifact.id, [&](Error e, QJsonObject data) { QVERIFY(!e); detail = data; });
        PUTGD_TRY_VERIFY(!detail.isEmpty());
        QCOMPARE(detail["uploadStatus"].toString(), QString("unknown"));
        QVERIFY(detail["capabilities"].toObject()["serverNamesObjects"].toBool());
        // The explicit retry sends the file again: the server now holds two copies.
        bool accepted = false;
        operations.retry(id, [&](Error e, QJsonObject) { QVERIFY2(!e, qPrintable(e.code)); accepted = true; });
        PUTGD_TRY_VERIFY(accepted);
        PUTGD_TRY_VERIFY_WITH_TIMEOUT(HistoryStore(operations.databasePath()).operation(id).state == "completed", 20000);
        operation = HistoryStore(operations.databasePath()).operation(id);
        QCOMPARE(operation.items[0].uploadStatus, QString("uploaded"));
        QCOMPARE(f.events("upload").size(), 2);
        QCOMPARE(f.stored(), 2);
        QCOMPARE(f.fetch(operation.items[0].receipt.directUrl).second, QByteArray("synthetic report"));
        // The token is in no file the resident wrote: configuration, journal or artifacts.
        QVERIFY(!f.tokenOnDisk(f.directory.path()));
    }
    void sameNamesAreSeparateUploadsAndHistoryDeletesByReference() {
        HttpFixture f;
        f.reset();
        ProviderRegistry registry;
        registry.add("custom-http", std::make_shared<HttpUploaderProvider>(f.credentials));
        SettingsService settings(f.directory.filePath("config.json"), [&](auto id, auto value) { return registry.validate(id, value); });
        // "skip" would drop the second file of a name on a destination with names of its own.
        QVERIFY(settings.save(configuration(f, "skip"), {}));
        QDir(f.directory.path()).mkpath("a");
        QDir(f.directory.path()).mkpath("b");
        Source source;
        source.payload.kind = "files";
        source.payload.files = {fixture(f.directory.filePath("a"), "same.txt", "first"), fixture(f.directory.filePath("b"), "same.txt", "second")};
        OperationService operations(f.directory.filePath("data"), settings, registry, source);
        HistoryService history(operations);
        PUTGD_TRY_VERIFY(operations.ready());
        QString id;
        operations.invoke("clipboard", [&](auto error, auto result) { QVERIFY(!error); id = result["operationId"].toString(); });
        PUTGD_TRY_VERIFY(!id.isEmpty());
        const auto operation = waitOperation(operations, id);
        QCOMPARE(operation.state, QString("completed"));
        QCOMPARE(operation.items.size(), 2);
        for (const auto& item : operation.items) QCOMPARE(item.uploadStatus, QString("uploaded"));
        QVERIFY(operation.items[0].receipt.directUrl != operation.items[1].receipt.directUrl);
        QCOMPARE(f.stored(), 2);
        // Deleting one from History: prepared with its effect, then exactly that upload goes.
        const auto target = operation.items[0];
        QJsonObject preparation;
        history.prepareDelete({target.artifact.id}, [&](Error e, QJsonObject data) { QVERIFY2(!e, qPrintable(e.code + ": " + e.message)); preparation = data; });
        PUTGD_TRY_VERIFY(!preparation.isEmpty());
        QVERIFY(preparation["targets"].toArray()[0].toObject()["effect"].toString().contains("deletion URL"));
        bool confirmed = false;
        history.deleteConfirmed({target.artifact.id}, preparation["acknowledgement"].toString(), [&](Error e, QJsonObject) { QVERIFY2(!e, qPrintable(e.code + ": " + e.message)); confirmed = true; });
        PUTGD_TRY_VERIFY(confirmed);
        PUTGD_TRY_COMPARE_WITH_TIMEOUT(HistoryStore(operations.databasePath()).detail(target.artifact.id)["remoteStatus"].toString(), QString("removed"), 20000);
        QCOMPARE(f.fetch(target.receipt.directUrl).first, 404);
        QCOMPARE(f.fetch(operation.items[1].receipt.directUrl).second, QByteArray("second"));
        QCOMPARE(f.stored(), 1);
    }
    void historyRefusesDeletionWithoutADeletionUrl() {
        HttpFixture f;
        f.reset();
        f.destination.settings = f.settings("text");
        ProviderRegistry registry;
        registry.add("custom-http", std::make_shared<HttpUploaderProvider>(f.credentials));
        SettingsService settings(f.directory.filePath("config.json"), [&](auto id, auto value) { return registry.validate(id, value); });
        QVERIFY(settings.save(configuration(f), {}));
        Source source;
        source.payload.kind = "files";
        source.payload.files = {fixture(f.directory.path(), "note.txt", "kept on the server")};
        OperationService operations(f.directory.filePath("data"), settings, registry, source);
        HistoryService history(operations);
        PUTGD_TRY_VERIFY(operations.ready());
        QString id;
        operations.invoke("clipboard", [&](auto error, auto result) { QVERIFY(!error); id = result["operationId"].toString(); });
        PUTGD_TRY_VERIFY(!id.isEmpty());
        const auto operation = waitOperation(operations, id);
        QCOMPARE(operation.items[0].uploadStatus, QString("uploaded"));
        Error refusal;
        bool answered = false;
        history.prepareDelete({operation.items[0].artifact.id}, [&](Error e, QJsonObject) { refusal = e; answered = true; });
        PUTGD_TRY_VERIFY(answered);
        QCOMPARE(refusal.code, QString("unsupported_capability"));
        QVERIFY2(refusal.message.contains("deletion URL"), qPrintable(refusal.message));
        QJsonObject detail;
        history.detail(operation.items[0].artifact.id, [&](Error e, QJsonObject data) { QVERIFY(!e); detail = data; });
        PUTGD_TRY_VERIFY(!detail.isEmpty());
        QCOMPARE(detail["capabilities"].toObject()["delete"].toBool(), false);
        QVERIFY(detail["capabilities"].toObject()["removeUnavailable"].toString().contains("deletion URL"));
        QCOMPARE(f.events("delete").size(), 0);
    }
};
QTEST_GUILESS_MAIN(HttpUploaderTest)
#include "test_http_uploader.moc"
