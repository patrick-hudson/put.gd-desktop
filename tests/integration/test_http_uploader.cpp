// Custom HTTP uploader (#1361): the production provider and putgd-http-worker against the
// scripted loopback receiver, and the journal, retry and unknown-outcome behaviour through
// the real operation and history services. Synthetic token and payloads only.
#include "fixtures/U2Fixtures.h"
#include "fixtures/HttpControl.h"
#include "fixtures/Waits.h"
#include "storage/http/HttpImport.h"
#include "storage/http/HttpProvider.h"
#include "storage/http/HttpTemplate.h"
#include <QDirIterator>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
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
        if (style == "json" || style == "away" || style == "pretend") { s["responseUrl"] = "{json:files[0].url}"; s["thumbnailUrl"] = "{json:files[0].thumb}"; s["deletionUrl"] = "{json:files[0].delete}"; }
        s["errorMessage"] = "{json:error}";
        return s;
    }
    void store(const QMap<QString, QString>& values) {
        Credentials value;
        value.values = values;
        credentials.setSession(destination.profileId, value);
    }
    QJsonObject control(const QString& path, const QJsonObject& value = {}, bool post = false) { return test::httpControl(base, path, value, post); }
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
            return QStringList{"completed", "partial", "failed", "cancelled"}.contains(result.stateName());
        }, 20000);
        if (!terminal) QTest::qFail("Operation did not reach a terminal state", __FILE__, __LINE__);
        return result;
    }
private slots:
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
        const auto bytes = QJsonDocument(shown).toJson();
        QVERIFY(!bytes.contains("typed-in-key") && !bytes.contains("private-album") && !bytes.contains("files[0]"));
    }
    void replyStyles_data() {
        QTest::addColumn<QString>("style");
        QTest::addColumn<QString>("url");
        QTest::addColumn<QString>("deletion");
        QTest::newRow("json") << "json" << "{json:files[0].url}" << "{json:files[0].delete}";
        QTest::newRow("header") << "header" << "{header:Location}" << "{header:X-Delete}";
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
        QVERIFY2(link.startsWith(f.base + "/f/"), qPrintable(link));
        const auto fetched = f.fetch(link);
        QCOMPARE(fetched.first, 200);
        QCOMPARE(qint64(fetched.second.size()), artifact.bytes);
        QCOMPARE(uploaded.receipt.objectReference.contains("deletionUrl"), !deletion.isEmpty());
        QCOMPARE(f.provider.receiptCapabilities(uploaded.receipt).remove, !deletion.isEmpty());
    }
    // A token sent as a query parameter is part of the request address: the receipt
    // and its destination hold its name, never the value.
    void aQueryTokenStaysOutOfTheReceipt() {
        HttpFixture f;
        f.reset({{"auth", "query"}});
        auto destination = f.destination;
        auto s = f.settings("json");
        s["headers"] = QJsonArray{};
        s["parameters"] = rows({secretRow("key")});
        f.store({{"parameters/key", token}});
        destination.settings = s;
        const auto uploaded = f.upload(f.image(), destination);
        QVERIFY2(uploaded.status == "uploaded", qPrintable(uploaded.error.code + ": " + uploaded.error.message));
        QVERIFY(f.events("upload")[0].toObject()["query"].toObject().contains("key"));
        QVERIFY(!QJsonDocument(uploaded.receipt.toJson()).toJson().contains(token.toUtf8()));
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
        QCOMPARE(f.stored(), 0);
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
        // The same for an upload that has no body to send, its text travelling in the
        // address: the transport would repeat such a request by itself.
        f.reset();
        auto bare = f.destination;
        bare.settings = f.settings("json");
        bare.settings["requestMethod"] = "GET";
        bare.settings["body"] = "none";
        bare.settings["parameters"] = rows({plainRow("content", "{input}"), plainRow("name", "{filename}")});
        QVERIFY2(!http::validateSettings(bare.settings), qPrintable(http::validateSettings(bare.settings).message));
        f.rules({{{"call", "upload"}, {"store", true}, {"drop", true}}});
        lost = f.upload(f.text(), bare);
        QVERIFY2(lost.status == "unknown", qPrintable(lost.status + " " + lost.receipt.directUrl + " " + lost.error.code));
        QCOMPARE(lost.error.code, QString("http_outcome_unknown"));
        QCOMPARE(f.events("upload").size(), 1);
        QCOMPARE(f.stored(), 1);
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
        // 307 to another host: refused, and the file and token never go there.
        f.reset();
        destination.settings = f.settings("moved-away");
        const auto refused = f.upload(f.image(), destination);
        QCOMPARE(refused.status, QString("failed"));
        QCOMPARE(refused.error.code, QString("http_redirect_refused"));
        QVERIFY2(refused.error.message.contains("another host"), qPrintable(refused.error.message));
        QCOMPARE(f.events("upload").size(), 1);
        QCOMPARE(f.stored(), 0);
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
    // A server that answers 200 to a deletion and deletes nothing is not believed: the
    // file stays public, so History must not call it deleted.
    void aDeletionTheServerDidNotDoIsNotBelieved() {
        HttpFixture f;
        f.reset();
        auto destination = f.destination;
        destination.settings = f.settings("pretend");
        const auto uploaded = f.upload(f.image(), destination);
        QCOMPARE(uploaded.status, QString("uploaded"));
        const auto result = f.remove(uploaded.receipt);
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.error.code, QString("http_delete_unconfirmed"));
        QVERIFY2(result.error.message.contains("still serves the file"), qPrintable(result.error.message));
        QCOMPARE(f.fetch(uploaded.receipt.directUrl).first, 200);
        QCOMPARE(f.stored(), 1);
    }
    // Deletion progress journaled for one upload is never applied to another.
    void deletionProgressOfAnotherUploadIsNotApplied() {
        HttpFixture f;
        f.reset();
        const auto first = f.upload(f.image());
        f.rules({{{"call", "delete"}, {"store", true}, {"drop", true}, {"remaining", 8}}});
        const auto lost = f.remove(first.receipt);
        QCOMPARE(lost.status, QString("unknown"));
        QCOMPARE(lost.progress["phase"].toString(), QString("delete_intent"));
        f.rules({});
        const auto other = f.upload(f.image());
        QCOMPARE(f.remove(other.receipt, lost.progress).status, QString("changed"));
        QCOMPARE(f.stored(), 1);
    }
    // Review of #1371. A server can repeat what it was sent inside the addresses it
    // returns. No address that is recorded, copied or shown holds a stored value, in any
    // spelling: the link is refused, a thumbnail dropped, and a deletion URL recorded
    // with the value's name where that is exact, or dropped.
    void storedValuesEchoedInReturnedAddressesAreNotRecorded_data() {
        QTest::addColumn<QString>("where");
        QTest::addColumn<QString>("encoding");
        QTest::addColumn<QString>("auth");
        QTest::newRow("url-percent-query") << "url" << "percent" << "query";
        QTest::newRow("thumb-raw-header") << "thumb" << "raw" << "header";
        QTest::newRow("delete-raw-query") << "delete" << "raw" << "query";
        QTest::newRow("delete-percent-header") << "delete" << "percent" << "header";
    }
    void storedValuesEchoedInReturnedAddressesAreNotRecorded() {
        QFETCH(QString, where);
        QFETCH(QString, encoding);
        QFETCH(QString, auth);
        // A credential with characters an address has to encode.
        const QString secret = "putgd-synthetic/credential+not=real&0123";
        const QList<QByteArray> spellings{secret.toUtf8(), QUrl::toPercentEncoding(secret), QUrl::toPercentEncoding(secret).toLower()};
        HttpFixture f;
        f.reset({{"auth", auth}, {"token", secret}});
        auto destination = f.destination;
        auto s = f.settings("json");
        // In a header the stored value is "Bearer <credential>"; as a parameter it is the credential itself.
        if (auth == "query") { s["headers"] = QJsonArray{}; s["parameters"] = rows({secretRow("key")}); f.store({{"parameters/key", secret}}); }
        else f.store({{"headers/Authorization", "Bearer " + secret}});
        destination.settings = s;
        const QJsonObject rule{{"call", "upload"}, {"store", true}, {"echo", where}, {"encoding", encoding}};
        f.rules({rule});
        const auto result = f.upload(f.image(), destination);
        const auto kept = QJsonDocument(QJsonObject{{"receipt", result.receipt.toJson()}, {"error", result.error.toJson()}}).toJson();
        for (const auto& spelling : spellings) QVERIFY2(!kept.contains(spelling), kept.constData());
        QCOMPARE(f.stored(), 1);
        if (where == "url") {
            // The file is on the server, but its link is not one put.gd may keep.
            QCOMPARE(result.status, QString("unknown"));
            QCOMPARE(result.error.code, QString("http_link_holds_secret"));
            QVERIFY(result.receipt.directUrl.isEmpty());
            // Test connection takes the same path: nothing of it reaches the result or its journal.
            QByteArray journal;
            QJsonObject tested;
            Error error;
            f.rules({rule});
            f.provider.testDestination(destination, [&](const QJsonObject& entry, std::function<void(Error)> ack) { journal += QJsonDocument(entry).toJson(); ack({}); },
                                       [&](QJsonObject r, Error e) { tested = r; error = e; });
            QCOMPARE(error.code, QString("http_link_holds_secret"));
            QVERIFY(!tested.contains("link"));
            const auto reported = journal + QJsonDocument(tested).toJson() + QJsonDocument(error.toJson()).toJson();
            for (const auto& spelling : spellings) QVERIFY2(!reported.contains(spelling), reported.constData());
            return;
        }
        QVERIFY2(result.status == "uploaded", qPrintable(result.error.code + ": " + result.error.message));
        if (where == "thumb") { QVERIFY(!result.receipt.objectReference.contains("thumbnailUrl")); return; }
        const auto recorded = result.receipt.objectReference["deletionUrl"].toString();
        if (auth == "query" && encoding == "raw") {
            // The stored value stands there exactly: it is recorded by name and filled in for the deletion.
            QVERIFY2(recorded.endsWith("?key={secret:parameters/key}"), qPrintable(recorded));
            const auto removed = f.remove(result.receipt);
            QVERIFY2(removed.status == "removed", qPrintable(removed.error.code + ": " + removed.error.message));
            // The request is the address as the server wrote it, byte for byte; the
            // server reads its own "+" and "&" there as a space and a separator.
            QCOMPARE(f.events("delete")[0].toObject()["query"].toObject()["key"].toString(), QString("putgd-synthetic/credential not=real"));
            QCOMPARE(f.stored(), 0);
        } else {
            // Only part of a stored value, or another spelling of it: there is no exact name to record.
            QVERIFY2(recorded.isEmpty(), qPrintable(recorded));
            QVERIFY(!f.provider.receiptCapabilities(result.receipt).remove);
        }
    }
    // A deletion URL may name a stored value, and its host may come from the reply. The
    // value goes to the request URL's own host or nowhere.
    void deletionUrlWithAStoredValueStaysOnTheRequestHost() {
        HttpFixture f;
        f.reset();
        const QString key = "putgd-synthetic-delete-key-0001";
        auto destination = f.destination;
        destination.settings["secrets"] = rows({secretRow("delete-key")});
        destination.settings["deletionUrl"] = "{json:files[0].delete}?key={secret:secrets/delete-key}";
        f.store({{"headers/Authorization", "Bearer " + token}, {"secrets/delete-key", key}});
        const QJsonObject elsewhere{{"call", "upload"}, {"store", true}, {"elsewhere", "delete"}};
        f.rules({elsewhere});
        const auto uploaded = f.upload(f.image(), destination);
        QVERIFY2(uploaded.status == "uploaded", qPrintable(uploaded.error.code + ": " + uploaded.error.message));
        QVERIFY2(!uploaded.receipt.objectReference.contains("deletionUrl"), qPrintable(uploaded.receipt.objectReference["deletionUrl"].toString()));
        QVERIFY(!f.provider.receiptCapabilities(uploaded.receipt).remove);
        QVERIFY(!QJsonDocument(uploaded.receipt.toJson()).toJson().contains(key.toUtf8()));
        // On the request's own host the name is recorded.
        const auto local = f.upload(f.image(), destination);
        QVERIFY(local.receipt.objectReference["deletionUrl"].toString().endsWith("?key={secret:secrets/delete-key}"));
        // A record that names another host all the same is refused when its deletion is asked for.
        auto moved = local.receipt;
        moved.objectReference["deletionUrl"] = moved.objectReference["deletionUrl"].toString().replace("127.0.0.1", "localhost");
        const auto refused = f.remove(moved);
        QCOMPARE(refused.status, QString("unsupported_safe_removal"));
        QCOMPARE(f.events("delete").size(), 0);
        QCOMPARE(f.remove(local.receipt).status, QString("removed"));
        QCOMPARE(f.events("delete")[0].toObject()["query"].toObject()["key"].toString(), key);
    }
    // {responseurl} is where a redirect led. Without a redirect it is the request's own
    // address, secret parameters included, and that is never a link.
    void theRequestAddressIsNeverALink() {
        HttpFixture f;
        f.reset({{"auth", "query"}});
        auto destination = f.destination;
        auto s = f.settings("nolink");
        s["headers"] = QJsonArray{};
        s["parameters"] = rows({secretRow("key")});
        s["responseUrl"] = "{responseurl}";
        f.store({{"parameters/key", token}});
        destination.settings = s;
        const auto result = f.upload(f.image(), destination);
        QCOMPARE(result.status, QString("unknown"));
        QCOMPARE(result.error.code, QString("http_response_unreadable"));
        QVERIFY(result.receipt.directUrl.isEmpty());
        QVERIFY(!QJsonDocument(QJsonObject{{"receipt", result.receipt.toJson()}, {"error", result.error.toJson()}}).toJson().contains(token.toUtf8()));
    }
    // A reply that ends early is not a reply: whatever its first bytes look like, the
    // outcome is unknown and no link is recorded.
    void aReplyCutOffAfterItsStatusLineIsUnknown_data() {
        QTest::addColumn<QString>("style");
        QTest::addColumn<QString>("cut");
        QTest::newRow("json-length") << "json" << "length";
        QTest::newRow("json-chunk") << "json" << "chunk";
    }
    void aReplyCutOffAfterItsStatusLineIsUnknown() {
        QFETCH(QString, style);
        QFETCH(QString, cut);
        HttpFixture f;
        f.reset();
        auto destination = f.destination;
        destination.settings = f.settings(style);
        if (style == "header") destination.settings["responseUrl"] = "{header:Location}";
        f.rules({{{"call", "upload"}, {"store", true}, {"cut", cut}}});
        const auto result = f.upload(f.image(), destination);
        QVERIFY2(result.status == "unknown", qPrintable(result.status + " " + result.receipt.directUrl + " " + result.error.code));
        QCOMPARE(result.error.code, QString("http_outcome_unknown"));
        QVERIFY2(result.error.message.contains("cut off") && result.error.message.contains("second copy"), qPrintable(result.error.message));
        QVERIFY(result.receipt.directUrl.isEmpty());
        QCOMPARE(f.stored(), 1);
    }
    // #1393. The short reply is already waiting when the client has sent the last byte of
    // the file. Qt, still finishing the request it wrote, takes the close that follows for
    // a dead connection, tries to send the request again, cannot, and reports that (error
    // 205), after it has handed over the status line, the headers and the bytes that came.
    // Under load the rows above took that path now and then, and a short reply that held
    // a whole link was recorded as an upload. This rule makes the server do it every
    // time. Whatever the transport reports, a reply shorter than the length it declared
    // is not a reply.
    void aShortReplyThatWaitedWhileTheFileWasSentIsUnknown() {
        HttpFixture f;
        f.reset();
        // The reply's JSON is whole and names a link; its Content-Length promises 64 bytes more.
        const QJsonObject body{{"files", QJsonArray{QJsonObject{{"url", f.base + "/f/synthetic/shot.png"}}}}};
        f.rules({{{"call", "upload"}, {"early", true}, {"drain", true}, {"status", 200}, {"cut", "length"}, {"body", body}}});
        // Large enough that the reply arrives while the file is still being sent.
        const auto result = f.upload(f.artifact("large.bin", QByteArray(16 * 1024 * 1024, 'x'), "application/octet-stream", "files"));
        QVERIFY2(result.status == "unknown", qPrintable(result.status + " " + result.receipt.directUrl + " " + result.error.code + ": " + result.error.message));
        QCOMPARE(result.error.code, QString("http_outcome_unknown"));
        QVERIFY2(result.error.message.contains("second copy"), qPrintable(result.error.message));
        QVERIFY(result.receipt.directUrl.isEmpty());
        QCOMPARE(f.events("upload").size(), 1);
    }
    // Server text that is kept (a message, an excerpt, a content type) has every spelling
    // of a stored value removed: as sent, percent-encoded, and as PHP writes JSON.
    void serverTextIsScrubbedInEverySpelling() {
        const QString secret = "putgd-synthetic/credential+not=real&0123";
        const QList<QString> spellings{secret, QString(secret).replace("/", "\\/"), QString::fromLatin1(QUrl::toPercentEncoding(secret)), QString::fromLatin1(QUrl::toPercentEncoding(secret).toLower())};
        HttpFixture f;
        auto destination = f.destination;
        destination.settings["errorMessage"] = "{response}";
        for (const auto& options : {QJsonObject{{"token", secret}, {"php", true}}, QJsonObject{{"token", secret}, {"echo", "percent"}}}) {
            f.reset(options);
            // A refused credential comes back in the server's own words.
            f.store({{"headers/Authorization", "Bearer wrong-" + secret}});
            const auto refused = f.upload(f.image(), destination);
            QCOMPARE(refused.error.code, QString("http_authentication_failed"));
            const auto kept = refused.error.message + QString::fromUtf8(QJsonDocument(refused.error.details).toJson());
            for (const auto& spelling : spellings) QVERIFY2(!kept.contains(spelling), qPrintable(kept));
            QVERIFY2(refused.error.message.contains("<redacted>"), qPrintable(refused.error.message));
        }
        // The content type of a reply with no link is the server's text too.
        f.reset({{"token", secret}});
        f.store({{"headers/Authorization", "Bearer " + secret}});
        f.rules({{{"call", "upload"}, {"store", true}, {"status", 200}, {"body", "stored"}, {"type", "text/x-" + QString::fromLatin1(QUrl::toPercentEncoding(secret))}}});
        const auto unread = f.upload(f.image());
        QCOMPARE(unread.error.code, QString("http_response_unreadable"));
        const auto details = QString::fromUtf8(QJsonDocument(unread.error.details).toJson());
        for (const auto& spelling : spellings) QVERIFY2(!details.contains(spelling), qPrintable(details));
    }
    // The other half: a key that is only part of a stored value, here inside a stored JSON
    // body, is found when the server writes it into the link.
    void aKeyEchoedOutOfAStoredBodyIsFound() {
        const QString part = "putgdsyntheticbodypart0001";
        HttpFixture f;
        f.reset({{"auth", "argument"}, {"token", part}});
        auto destination = f.destination;
        auto s = f.settings("json");
        s["headers"] = QJsonArray{};
        s["body"] = "json";
        s["dataSecret"] = true;
        QVERIFY2(!http::validateSettings(s), qPrintable(http::validateSettings(s).message));
        destination.settings = s;
        f.store({{"data", "{\"key\":\"" + part + "\",\"content\":\"{input}\",\"name\":\"{filename}\"}"}});
        f.rules({{{"call", "upload"}, {"store", true}, {"echo", "url"}}});
        const auto result = f.upload(f.text(), destination);
        const auto kept = QJsonDocument(QJsonObject{{"receipt", result.receipt.toJson()}, {"error", result.error.toJson()}}).toJson();
        QVERIFY2(!kept.contains(part.toUtf8()), kept.constData());
        QCOMPARE(result.status, QString("unknown"));
        QCOMPARE(result.error.code, QString("http_link_holds_secret"));
        QVERIFY2(result.error.message.contains("request body"), qPrintable(result.error.message));
        QCOMPARE(f.stored(), 1);
    }
    // A value echoed with a control character inside it must not come back together when
    // the message is made fit to show.
    void aStoredValueEchoedAroundAControlCharacterIsScrubbed() {
        HttpFixture f;
        f.reset({{"echo", "control"}});
        f.store({{"headers/Authorization", "Bearer wrong-" + token}});
        const auto refused = f.upload(f.image());
        QCOMPARE(refused.error.code, QString("http_authentication_failed"));
        const auto kept = refused.error.message + QString::fromUtf8(QJsonDocument(refused.error.details).toJson());
        QVERIFY2(!kept.contains(token) && !kept.contains("synthetic-uploader"), qPrintable(kept));
    }
    // A deletion URL that names a stored value is sent to the destination's own host and
    // follows no redirect away from it: the server could write the value into Location.
    void aDeletionThatNamesAStoredValueFollowsNoRedirectToAnotherHost() {
        HttpFixture f;
        f.reset();
        const QString part = "putgd-synthetic-delete-part-0002";
        auto destination = f.destination;
        destination.settings["secrets"] = rows({secretRow("delete-part")});
        destination.settings["deletionUrl"] = "{json:files[0].delete}?k={secret:secrets/delete-part}";
        f.store({{"headers/Authorization", "Bearer " + token}, {"secrets/delete-part", part}});
        const auto uploaded = f.upload(f.image(), destination);
        QVERIFY2(uploaded.status == "uploaded", qPrintable(uploaded.error.code + ": " + uploaded.error.message));
        f.rules({{{"call", "delete"}, {"status", 302}, {"location", QString(f.base).replace("127.0.0.1", "localhost") + "/delete/moved/away?k=" + part}}});
        const auto result = f.remove(uploaded.receipt);
        const auto requests = f.events("delete");
        for (const auto& request : requests)
            QVERIFY2(!request.toObject()["headers"].toObject()["host"].toString().startsWith("localhost"), QJsonDocument(request.toObject()).toJson().constData());
        QCOMPARE(requests.size(), 1);
        QCOMPARE(result.status, QString("unknown"));
        QCOMPARE(result.error.code, QString("http_outcome_unknown"));
        QVERIFY2(result.error.message.contains("another host"), qPrintable(result.error.message));
    }
    // An import never stores under, or cleans up, a keychain entry that another
    // destination already names.
    void anImportDoesNotTakeAnotherDestinationsKeychainEntry() {
        QTemporaryDir dir;
        CredentialStore credentials;
        ProviderRegistry registry;
        registry.add("custom-http", std::make_shared<HttpUploaderProvider>(credentials));
        SettingsService settings(dir.filePath("config.json"), [&](auto id, auto value) { return registry.validate(id, value); });
        auto document = putgd::test::configuration(dir.path(), true);
        auto existing = http::defaults();
        existing["requestUrl"] = "https://first.example.test/upload";
        document["profiles"] = QJsonArray{QJsonObject{{"id", "first"}, {"label", "First"}, {"provider", "custom-http"}, {"settings", existing},
                                                      {"credentials", QJsonObject{{"mode", "keychain"}, {"reference", "putgd/http/shared"}}}}};
        QVERIFY2(settings.save(document, {}), "the starting configuration was refused");
        const auto path = dir.filePath("uploader.sxcu");
        {
            QFile out(path);
            QVERIFY(out.open(QIODevice::WriteOnly));
            out.write(QJsonDocument(QJsonObject{{"Version", "17.0.0"}, {"Name", "Uploader"}, {"DestinationType", "ImageUploader"}, {"RequestMethod", "POST"},
                                                {"RequestURL", "https://media.example.test/upload"}, {"Headers", QJsonObject{{"Authorization", token}}},
                                                {"Body", "MultipartFormData"}, {"FileFormName", "file"}, {"URL", "{json:url}"}}).toJson());
        }
        bool done = false;
        Error error;
        http::importUploaderFile(settings, credentials, {{"path", path}, {"expectedRevision", settings.metadata()["observedRevision"].toString()}, {"credentialMode", "keychain"}, {"profileId", "shared"}},
                                 [&](Error e, QJsonObject) { error = e; done = true; });
        QVERIFY(waitFor([&] { return done; }, 10000));
        QCOMPARE(error.code, QString("credential_reference_in_use"));
        QCOMPARE(settings.document()["profiles"].toArray().size(), 1);
        QVERIFY(!credentials.cached("shared", {{"mode", "keychain"}, {"reference", "putgd/http/shared"}}).has_value());
    }
    // A deletion is confirmed by what the link serves before and after. A check whose
    // reply was cut short does not confirm a deletion the server did not do.
    void aDeletionIsNotConfirmedByALinkCheckThatWasCutShort() {
        HttpFixture f;
        f.reset();
        // This receiver answers the deletion politely and deletes nothing.
        auto destination = f.destination;
        destination.settings = f.settings("pretend");
        const auto uploaded = f.upload(f.image(), destination);
        QVERIFY2(uploaded.status == "uploaded", qPrintable(uploaded.error.code + ": " + uploaded.error.message));
        const QJsonObject whole{{"call", "file"}}, cut{{"call", "file"}, {"cut", "length"}};
        f.rules({cut, whole});
        const auto result = f.remove(uploaded.receipt);
        QVERIFY2(result.status == "failed", qPrintable(result.status + " " + result.error.code + ": " + result.error.message));
        QCOMPARE(result.error.code, QString("http_delete_unconfirmed"));
        QCOMPARE(f.stored(), 1);
    }
    // Two imports in flight must not share a destination's stored values: the second is
    // refused while the first is being saved.
    void importsAreOneAtATime() {
        QTemporaryDir dir;
        CredentialStore credentials;
        ProviderRegistry registry;
        registry.add("custom-http", std::make_shared<HttpUploaderProvider>(credentials));
        SettingsService settings(dir.filePath("config.json"), [&](auto id, auto value) { return registry.validate(id, value); });
        QVERIFY(settings.save(putgd::test::configuration(dir.path(), true), {}));
        const QString one = "PUTGD-SYNTHETIC-IMPORT-ONE-0123456789", two = "PUTGD-SYNTHETIC-IMPORT-TWO-0123456789";
        const auto file = [&](const QString& host, const QString& value) {
            const auto path = dir.filePath(host + ".sxcu");
            QFile out(path);
            if (!out.open(QIODevice::WriteOnly)) qFatal("cannot write the synthetic .sxcu file");
            out.write(QJsonDocument(QJsonObject{{"Version", "17.0.0"}, {"Name", "Shared name"}, {"DestinationType", "ImageUploader"}, {"RequestMethod", "POST"},
                                                {"RequestURL", "https://" + host + ".example.test/upload"}, {"Headers", QJsonObject{{"Authorization", value}}},
                                                {"Body", "MultipartFormData"}, {"FileFormName", "file"}, {"URL", "{json:url}"}}).toJson());
            return path;
        };
        struct Outcome { bool done = false; Error error; QJsonObject result; } first, second;
        const auto revision = settings.metadata()["observedRevision"].toString();
        const auto params = [&](const QString& path) {
            return QJsonObject{{"path", path}, {"expectedRevision", revision}, {"credentialMode", "session"}, {"profileId", "shared"}};
        };
        http::importUploaderFile(settings, credentials, params(file("one", one)), [&](Error e, QJsonObject r) { first = {true, e, r}; });
        http::importUploaderFile(settings, credentials, params(file("two", two)), [&](Error e, QJsonObject r) { second = {true, e, r}; });
        QVERIFY(waitFor([&] { return first.done && second.done; }, 10000));
        // Exactly one destination, holding the values of its own file.
        const auto profiles = settings.document()["profiles"].toArray();
        QCOMPARE(profiles.size(), 1);
        const auto profile = profiles[0].toObject();
        const bool firstWon = profile["settings"].toObject()["requestUrl"].toString().contains("one.example.test");
        const auto cached = credentials.cached(profile["id"].toString(), profile["credentials"].toObject());
        QVERIFY(cached.has_value());
        QCOMPARE(cached->values.value("headers/Authorization"), firstWon ? one : two);
        // The other was refused before it stored anything.
        QVERIFY2(!first.error && second.error, qPrintable(first.error.code + " / " + second.error.code));
        QCOMPARE(second.error.code, QString("import_busy"));
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
        QCOMPARE(operation.stateName(), QString("partial"));
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
        PUTGD_TRY_VERIFY_WITH_TIMEOUT(HistoryStore(operations.databasePath()).operation(id).stateName() == "completed", 20000);
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
        QCOMPARE(operation.stateName(), QString("completed"));
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
};
QTEST_GUILESS_MAIN(HttpUploaderTest)
#include "test_http_uploader.moc"
