// Nextcloud and WebDAV destinations (#1872) against real servers: the production provider and
// putgd-webdav-worker, never a mock. Each case runs only when its server is named:
//   PUTGD_TEST_NEXTCLOUD_URL, PUTGD_TEST_NEXTCLOUD_USER, PUTGD_TEST_NEXTCLOUD_PASSWORD_FILE
//   PUTGD_TEST_WEBDAV_URL (the collection), PUTGD_TEST_WEBDAV_USER, PUTGD_TEST_WEBDAV_PASSWORD_FILE,
//   PUTGD_TEST_WEBDAV_PUBLIC_URL (where it serves those files without signing in)
//   PUTGD_TEST_NEXTCLOUD_UNTRUSTED_URL: the same server behind a certificate this machine doesn't trust
//   PUTGD_TEST_NEXTCLOUD_SIGNIN_URL_FILE: Login Flow v2 writes its browser page here; something
//     else (tools/lab, a person) completes it within five minutes
//   PUTGD_TEST_NEXTCLOUD_EXPECT: quota, sharing_off, password or expiry, for a server set up so
// Passwords are read from their files inside this process and never printed. A lab certificate
// authority is trusted the way the AppImage trusts the host's: PUTGD_CA_BUNDLE.
#include "storage/dav/DavProvider.h"
#include "core/CaBundle.h"
#include "core/Operation.h"
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <atomic>
#include <thread>
using namespace putgd;
namespace {
QString env(const char* name) { return qEnvironmentVariable(name); }
QString secret(const char* name) {
    QFile file(env(name));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(file.readAll()).trimmed();
}
QByteArray synthetic(qint64 bytes, quint32 seed) {
    QByteArray out(bytes, Qt::Uninitialized);
    quint32 x = seed;
    for (qint64 i = 0; i < bytes; ++i) { x = x * 1664525u + 1013904223u; out[i] = char(x >> 24); }
    return out;
}
// A GET as a visitor: no credentials, no cookies kept; the lab CA trusted like the worker trusts it.
QByteArray fetch(const QUrl& url, int* status = nullptr, const QByteArray& authorization = {}) {
    QNetworkAccessManager nam;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    if (!authorization.isEmpty()) request.setRawHeader("Authorization", authorization);
    auto* reply = nam.get(request);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    if (status) *status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto body = reply->readAll();
    reply->deleteLater();
    return body;
}
// A request with the account's own password, standing in for another app that changes a file.
int send(const QByteArray& verb, const QUrl& url, const QString& user, const QString& password, const QByteArray& body = {}) {
    QNetworkAccessManager nam;
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", "Basic " + (user + ':' + password).toUtf8().toBase64());
    request.setRawHeader("OCS-APIRequest", "true");
    if (verb == "POST") request.setRawHeader("Content-Type", "application/x-www-form-urlencoded");
    auto* reply = nam.sendCustomRequest(request, verb, body);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();
    return status;
}
DurableCheckpoint journal(QJsonArray& kept) {
    return [&kept](const QJsonObject& value, std::function<void(Error)> ack) { kept.append(value); ack({}); };
}
}
class DavIntegrationTest final : public QObject {
    Q_OBJECT
    QTemporaryDir files_;
    CredentialStore credentials_;
    QString run_ = "putgd-it-" + QUuid::createUuid().toString(QUuid::Id128).left(12);
    Destination nextcloud(const QString& server, const QString& folder) {
        auto settings = dav::nextcloudDefaults();
        settings["server"] = server;
        settings["folder"] = folder;
        Destination d{dav::nextcloudId, "it-nextcloud", "r1", settings, {{"mode", "session"}, {"reference", QJsonValue::Null}}};
        // Bound to this server, as a store from Settings binds them.
        credentials_.setSession(d.profileId, {env("PUTGD_TEST_NEXTCLOUD_USER"), secret("PUTGD_TEST_NEXTCLOUD_PASSWORD_FILE"), dav::binding(settings, dav::nextcloudId)});
        return d;
    }
    Destination webdav() {
        auto settings = dav::webdavDefaults();
        settings["address"] = dav::collectionAddress(env("PUTGD_TEST_WEBDAV_URL"));
        if (!env("PUTGD_TEST_WEBDAV_PUBLIC_URL").isEmpty()) settings["linkBase"] = dav::collectionAddress(env("PUTGD_TEST_WEBDAV_PUBLIC_URL"));
        Destination d{dav::webdavId, "it-webdav", "r1", settings, {{"mode", "session"}, {"reference", QJsonValue::Null}}};
        credentials_.setSession(d.profileId, {env("PUTGD_TEST_WEBDAV_USER"), secret("PUTGD_TEST_WEBDAV_PASSWORD_FILE"), dav::binding(settings, dav::webdavId)});
        return d;
    }
    Artifact artifact(const QString& name, const QByteArray& bytes, const QString& mime = "application/octet-stream") {
        Artifact a;
        a.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        a.path = files_.filePath(a.id);
        QFile file(a.path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) qFatal("synthetic file");
        a.name = name;
        a.mime = mime;
        a.bytes = bytes.size();
        a.sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        return a;
    }
    UploadResult upload(DavProvider& provider, const Destination& d, const Artifact& a, std::shared_ptr<std::atomic_bool> cancel = {}, std::function<void(const UploadProgress&)> progress = {}) {
        UploadRequest request;
        request.destination = d;
        request.artifact = a;
        request.candidate = a.name;
        request.publicationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        request.condition = "absent";
        request.cancellation = cancel ? cancel : std::make_shared<std::atomic_bool>(false);
        request.progress = std::move(progress);
        request.checkpoint = [](const QJsonObject&, std::function<void(Error)> ack) { ack({}); };
        UploadResult out;
        provider.upload(request, [&](UploadResult r) { out = r; });
        return out;
    }
    ShareResult share(DavProvider& provider, const StorageReceipt& receipt, const QString& action = "create") {
        ShareRequest request{"share-" + run_, action, receipt, {}, {}, std::make_shared<std::atomic_bool>(false)};
        ShareResult out;
        QJsonArray kept;
        provider.share(request, journal(kept), [&](ShareResult r) { out = r; });
        return out;
    }
    DeleteResult remove(DavProvider& provider, const StorageReceipt& receipt) {
        DeleteRequest request{"delete-" + run_, receipt, {}, std::make_shared<std::atomic_bool>(false)};
        DeleteResult out;
        QJsonArray kept;
        provider.remove(request, journal(kept), [&](DeleteResult r) { out = r; });
        return out;
    }
    QJsonObject test(DavProvider& provider, const Destination& d, Error& error) {
        QJsonObject out;
        QJsonArray kept;
        provider.testDestination(d, journal(kept), [&](QJsonObject r, Error e) { out = r; error = e; });
        // No checkpoint keeps a share token: it is a secret in the link's path.
        for (const auto& value : kept) if (QJsonDocument(value.toObject()).toJson().contains("/s/")) qWarning() << "journal kept a share link";
        return out;
    }
private slots:
    // This process's own visitor requests trust the lab CA as the worker does.
    void initTestCase() { useHostCaBundle(); }
    void nextcloudTestUploadShareDelete() {
        if (env("PUTGD_TEST_NEXTCLOUD_URL").isEmpty()) QSKIP("PUTGD_TEST_NEXTCLOUD_URL names no Nextcloud server");
        const auto expect = env("PUTGD_TEST_NEXTCLOUD_EXPECT");
        DavProvider provider(credentials_, dav::nextcloudId);
        const auto d = nextcloud(env("PUTGD_TEST_NEXTCLOUD_URL"), "putgd-it/" + run_);
        Error error;
        const auto tested = test(provider, d, error);
        if (expect == "sharing_off") { QCOMPARE(error.code, QStringLiteral("nextcloud_sharing_disabled")); return; }
        if (expect == "password") {
            QCOMPARE(error.code, QStringLiteral("nextcloud_share_password_required"));
            // A definite share failure after a confirmed upload leaves the file uploaded without a
            // link, in the state History offers Finish share link for.
            const auto up = upload(provider, d, artifact("password.png", synthetic(4096, 9), "image/png"));
            QCOMPARE(up.status, QStringLiteral("uploaded"));
            const auto refused = share(provider, up.receipt);
            QCOMPARE(refused.status, QStringLiteral("private"));
            QCOMPARE(refused.error.code, QStringLiteral("nextcloud_share_password_required"));
            QVERIFY(shareResultMatchesReceipt(refused, up.receipt));
            OperationItem item;
            item.uploadStatus = "uploaded";
            item.shareIntent = "create";
            item.shareStatus = refused.status;
            item.receipt = refused.receipt;
            item.remoteStatus = "present";
            QVERIFY(item.linkFinishable());
            QCOMPARE(remove(provider, up.receipt).status, QStringLiteral("removed"));
            send("DELETE", QUrl(env("PUTGD_TEST_NEXTCLOUD_URL") + "/remote.php/webdav/putgd-it/" + run_), env("PUTGD_TEST_NEXTCLOUD_USER"), secret("PUTGD_TEST_NEXTCLOUD_PASSWORD_FILE"));
            return;
        }
        if (expect == "quota") {
            const auto refused = upload(provider, d, artifact("quota.bin", synthetic(6 * 1024 * 1024, 7)));
            QCOMPARE(refused.status, QStringLiteral("failed"));
            QCOMPARE(refused.error.code, QStringLiteral("webdav_quota"));
            QVERIFY2(refused.error.message.contains(" quota") && refused.error.message.contains("B "), qPrintable(refused.error.message));
            return;
        }
        QVERIFY2(!error, qPrintable(error.code + ": " + error.message));
        QVERIFY2(tested["publicReachable"].toBool(), QJsonDocument(tested).toJson(QJsonDocument::Compact).constData());
        QCOMPARE(tested["cleanupStatus"].toString(), QStringLiteral("removed"));
        QVERIFY(!tested["accountName"].toString().isEmpty());
        QVERIFY(!tested["link"].toString().contains(QRegularExpression("/s/[A-Za-z0-9]{6,}")));

        const auto bytes = synthetic(300 * 1024, 1872);
        const auto png = artifact("Screenshot é #1.png", bytes, "image/png");
        const auto uploaded = upload(provider, d, png);
        QVERIFY2(uploaded.status == "uploaded", qPrintable(uploaded.status + " " + uploaded.error.message));
        QVERIFY(!uploaded.receipt.concurrencyToken.isEmpty());
        QVERIFY(uploaded.receipt.directUrl.isEmpty());
        // The same name again is a collision, never a replacement.
        QCOMPARE(upload(provider, d, png).status, QStringLiteral("collision"));
        const auto shared = share(provider, uploaded.receipt);
        QVERIFY2(shared.status == "shared", qPrintable(shared.status + " " + shared.error.message));
        // Links are compared, never printed: a share token is a secret.
        QVERIFY(shared.receipt.directUrl.startsWith(env("PUTGD_TEST_NEXTCLOUD_URL") + "/s/"));
        if (expect == "expiry") QVERIFY2(!shared.expiresAt.isEmpty(), "an enforced expiry is recorded");
        int status = 0;
        QCOMPARE(fetch(QUrl(dav::shareDownload(env("PUTGD_TEST_NEXTCLOUD_URL"), dav::shareToken(shared.receipt.directUrl))), &status), bytes);
        QCOMPARE(status, 200);
        // An inspection finds the link that exists; a second create hands out the same one.
        QVERIFY(share(provider, uploaded.receipt, "inspect").receipt.directUrl == shared.receipt.directUrl);
        QVERIFY(share(provider, uploaded.receipt).receipt.directUrl == shared.receipt.directUrl);
        // A link made by hand for the same file is not put.gd's: inspection never takes it.
        QVERIFY(send("POST", QUrl(env("PUTGD_TEST_NEXTCLOUD_URL") + "/ocs/v2.php/apps/files_sharing/api/v1/shares"), env("PUTGD_TEST_NEXTCLOUD_USER"),
                     secret("PUTGD_TEST_NEXTCLOUD_PASSWORD_FILE"), "shareType=3&permissions=1&path=" + QUrl::toPercentEncoding(uploaded.receipt.objectReference["path"].toString())) / 100 == 2);
        QVERIFY(share(provider, uploaded.receipt, "inspect").receipt.directUrl == shared.receipt.directUrl);
        // A name that is taken is reported present, with what it would be checked against.
        NameCheck taken;
        provider.checkName(d, png.name, [&](NameCheck c) { taken = c; });
        QCOMPARE(taken.status, QStringLiteral("present"));
        QVERIFY(!taken.objectReference["target"].toString().isEmpty());
        // The share does not change the file's ETag: delete still matches.
        StatResult own;
        provider.stat(uploaded.receipt, [&](StatResult r) { own = r; });
        QCOMPARE(own.status, QStringLiteral("own"));
        QCOMPARE(remove(provider, uploaded.receipt).status, QStringLiteral("removed"));
        fetch(QUrl(shared.receipt.directUrl), &status);
        QCOMPARE(status, 404);

        // A file changed on the server after its upload is refused and kept.
        const auto other = upload(provider, d, artifact("changed.txt", "first\n", "text/plain"));
        QCOMPARE(other.status, QStringLiteral("uploaded"));
        const auto direct = QUrl(env("PUTGD_TEST_NEXTCLOUD_URL") + "/remote.php/webdav/" + dav::encodePath(d.settings["folder"].toString()) + "/changed.txt");
        const int overwritten = send("PUT", direct, env("PUTGD_TEST_NEXTCLOUD_USER"), secret("PUTGD_TEST_NEXTCLOUD_PASSWORD_FILE"), "second\n");
        QVERIFY2(overwritten / 100 == 2, qPrintable(QString::number(overwritten)));
        // The other app's write took: the server now holds its bytes.
        QCOMPARE(fetch(direct, &status, "Basic " + (env("PUTGD_TEST_NEXTCLOUD_USER") + ':' + secret("PUTGD_TEST_NEXTCLOUD_PASSWORD_FILE")).toUtf8().toBase64()), QByteArray("second\n"));
        StatResult after;
        provider.stat(other.receipt, [&](StatResult r) { after = r; });
        QVERIFY2(after.status == "foreign", qPrintable(after.status + " recorded " + other.receipt.concurrencyToken + " " + QJsonDocument(other.receipt.objectReference).toJson(QJsonDocument::Compact)));
        QCOMPARE(remove(provider, other.receipt).status, QStringLiteral("changed"));
        fetch(direct, &status, "Basic " + (env("PUTGD_TEST_NEXTCLOUD_USER") + ':' + secret("PUTGD_TEST_NEXTCLOUD_PASSWORD_FILE")).toUtf8().toBase64());
        QCOMPARE(status, 200);
        // The run's folder goes, with what is left in it.
        send("DELETE", QUrl(env("PUTGD_TEST_NEXTCLOUD_URL") + "/remote.php/webdav/putgd-it/" + run_), env("PUTGD_TEST_NEXTCLOUD_USER"), secret("PUTGD_TEST_NEXTCLOUD_PASSWORD_FILE"));
    }
    void nextcloudChunkedUploadAndCancel() {
        if (env("PUTGD_TEST_NEXTCLOUD_URL").isEmpty() || !env("PUTGD_TEST_NEXTCLOUD_EXPECT").isEmpty()) QSKIP("needs a plain Nextcloud server");
        DavProvider provider(credentials_, dav::nextcloudId);
        const auto d = nextcloud(env("PUTGD_TEST_NEXTCLOUD_URL"), "putgd-it/" + run_ + "-large");
        // Above 50 MB: chunked upload v2, assembled with Overwrite: F.
        const auto bytes = synthetic(60LL * 1024 * 1024 + 123, 60);
        qint64 most = 0;
        const auto large = upload(provider, d, artifact("large.bin", bytes), {}, [&](const UploadProgress& p) { if (p.bytesTransferred) most = qMax(most, *p.bytesTransferred); });
        QVERIFY2(large.status == "uploaded", qPrintable(large.status + " " + large.error.code + " " + large.error.message));
        QCOMPARE(most, qint64(bytes.size()));
        const auto shared = share(provider, large.receipt);
        QCOMPARE(shared.status, QStringLiteral("shared"));
        int status = 0;
        const auto fetched = fetch(QUrl(dav::shareDownload(env("PUTGD_TEST_NEXTCLOUD_URL"), dav::shareToken(shared.receipt.directUrl))), &status);
        QCOMPARE(status, 200);
        QCOMPARE(QCryptographicHash::hash(fetched, QCryptographicHash::Sha256), QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
        // A second chunked upload of the same name meets the assembled file: a collision at the MOVE.
        QCOMPARE(upload(provider, d, artifact("large.bin", synthetic(55LL * 1024 * 1024, 61))).status, QStringLiteral("collision"));
        QCOMPARE(remove(provider, large.receipt).status, QStringLiteral("removed"));
        // Cancel mid-upload: nothing is left at the name.
        auto cancel = std::make_shared<std::atomic_bool>(false);
        const auto cancelled = upload(provider, d, artifact("cancelled.bin", bytes), cancel, [&](const UploadProgress& p) { if (p.bytesTransferred && *p.bytesTransferred > 20LL * 1024 * 1024) cancel->store(true); });
        QVERIFY2(cancelled.status == "failed" || cancelled.status == "unknown", qPrintable(cancelled.status));
        NameCheck check;
        provider.checkName(d, "cancelled.bin", [&](NameCheck c) { check = c; });
        QCOMPARE(check.status, QStringLiteral("absent"));
        send("DELETE", QUrl(env("PUTGD_TEST_NEXTCLOUD_URL") + "/remote.php/webdav/putgd-it/" + run_ + "-large"), env("PUTGD_TEST_NEXTCLOUD_USER"), secret("PUTGD_TEST_NEXTCLOUD_PASSWORD_FILE"));
    }
    void nextcloudUntrustedCertificate() {
        if (env("PUTGD_TEST_NEXTCLOUD_UNTRUSTED_URL").isEmpty()) QSKIP("PUTGD_TEST_NEXTCLOUD_UNTRUSTED_URL names no server");
        DavProvider provider(credentials_, dav::nextcloudId);
        const auto d = nextcloud(env("PUTGD_TEST_NEXTCLOUD_UNTRUSTED_URL"), "putgd-it/" + run_);
        Error error;
        test(provider, d, error);
        QCOMPARE(error.code, QStringLiteral("webdav_tls_failed"));
        QVERIFY2(error.message.contains("doesn't trust") && error.message.contains("SHA-256") && error.message.contains("sudo"), qPrintable(error.message));
    }
    void nextcloudSignIn() {
        if (env("PUTGD_TEST_NEXTCLOUD_SIGNIN_URL_FILE").isEmpty()) QSKIP("PUTGD_TEST_NEXTCLOUD_SIGNIN_URL_FILE is not set");
        DavProvider provider(credentials_, dav::nextcloudId);
        auto settings = dav::nextcloudDefaults();
        // A pasted Files link reduces to the server.
        settings["server"] = dav::nextcloudServer(env("PUTGD_TEST_NEXTCLOUD_URL") + "/index.php/apps/files/?dir=/");
        settings["folder"] = "putgd-it/" + run_ + "-signin";
        const Destination d{dav::nextcloudId, "it-signin", "r1", settings, {{"mode", "session"}, {"reference", QJsonValue::Null}}};
        const auto background = [](std::function<QJsonObject(Error&)> work, std::function<void(Error, QJsonObject)> done) {
            QJsonObject result;
            Error e;
            std::thread worker([&] { result = work(e); });
            while (!worker.joinable()) {}
            worker.join();
            done(e, result);
        };
        QString page;
        Error error;
        QJsonObject account;
        provider.setup("connectAccount", d, {}, background, [&](Error e, QJsonObject r) { error = e; account = r; },
                       [&](QJsonObject event) {
                           page = event["url"].toString();
                           QFile out(env("PUTGD_TEST_NEXTCLOUD_SIGNIN_URL_FILE"));
                           if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) out.write(page.toUtf8() + '\n');
                       }, std::make_shared<std::atomic_bool>(false));
        QVERIFY2(!error, qPrintable(error.code + ": " + error.message));
        QVERIFY(page.startsWith(settings["server"].toString() + "/"));
        QVERIFY(!account.contains("appPassword"));
        QVERIFY(!account["userId"].toString().isEmpty());
        const auto stored = credentials_.cached(d.profileId, d.credentialReference);
        QVERIFY(stored && !stored->secretKey.isEmpty());
        // The app password works for an upload and a link, then Sign out revokes it.
        Error tested;
        test(provider, d, tested);
        QVERIFY2(!tested, qPrintable(tested.code + ": " + tested.message));
        const auto password = stored->secretKey, login = stored->accessKey;
        QJsonObject signedOut;
        provider.setup("signOut", d, {}, background, [&](Error e, QJsonObject r) { error = e; signedOut = r; }, {}, {});
        QVERIFY(!error);
        QVERIFY(signedOut["revoked"].toBool());
        QVERIFY(!credentials_.cached(d.profileId, d.credentialReference));
        int status = 0;
        fetch(QUrl(settings["server"].toString() + "/remote.php/dav/files/" + account["userId"].toString() + "/"), &status, "Basic " + (login + ':' + password).toUtf8().toBase64());
        QCOMPARE(status, 401);
        send("DELETE", QUrl(env("PUTGD_TEST_NEXTCLOUD_URL") + "/remote.php/webdav/putgd-it/" + run_ + "-signin"), env("PUTGD_TEST_NEXTCLOUD_USER"), secret("PUTGD_TEST_NEXTCLOUD_PASSWORD_FILE"));
    }
    void webdavTestUploadDelete() {
        if (env("PUTGD_TEST_WEBDAV_URL").isEmpty()) QSKIP("PUTGD_TEST_WEBDAV_URL names no WebDAV server");
        DavProvider provider(credentials_, dav::webdavId);
        auto d = webdav();
        Error error;
        const auto tested = test(provider, d, error);
        QVERIFY2(!error, qPrintable(error.code + ": " + error.message));
        QVERIFY(tested["publicReachable"].toBool());
        QVERIFY2(tested["cleanupStatus"] == "removed", QJsonDocument(tested).toJson(QJsonDocument::Compact).constData());
        qInfo() << "conditionalCreate" << tested["conditionalCreate"] << "conditionalDelete" << tested["conditionalDelete"] << "warnings" << tested["warnings"];
        const auto bytes = synthetic(200 * 1024, 2);
        const auto uploaded = upload(provider, d, artifact(run_ + " shot.png", bytes, "image/png"));
        QVERIFY2(uploaded.status == "uploaded", qPrintable(uploaded.status + " " + uploaded.error.message));
        QVERIFY(uploaded.receipt.directUrl.startsWith(d.settings["linkBase"].toString(d.settings["address"].toString())));
        int status = 0;
        QCOMPARE(fetch(QUrl(uploaded.receipt.directUrl), &status), bytes);
        QCOMPARE(upload(provider, d, artifact(run_ + " shot.png", bytes, "image/png")).status, QStringLiteral("collision"));
        QCOMPARE(remove(provider, uploaded.receipt).status, QStringLiteral("removed"));
        // A missing folder is refused with Create folder offered, which then works.
        d.settings["address"] = d.settings["address"].toString() + run_ + "/";
        test(provider, d, error);
        QCOMPARE(error.code, QStringLiteral("webdav_folder_missing"));
        QVERIFY(error.details["createFolder"].toBool());
        QJsonObject created;
        provider.setup("createFolder", d, {}, [](auto work, auto done) { Error e; auto r = work(e); done(e, r); }, [&](Error e, QJsonObject r) { error = e; created = r; }, {}, {});
        QVERIFY2(!error, qPrintable(error.message));
        QVERIFY(created["created"].toBool());
        send("DELETE", QUrl(d.settings["address"].toString()), env("PUTGD_TEST_WEBDAV_USER"), secret("PUTGD_TEST_WEBDAV_PASSWORD_FILE"));
        // A wrong password says so.
        d.settings["address"] = dav::collectionAddress(env("PUTGD_TEST_WEBDAV_URL"));
        credentials_.setSession(d.profileId, {env("PUTGD_TEST_WEBDAV_USER"), "putgd-synthetic-wrong-password", dav::binding(d.settings, dav::webdavId)});
        test(provider, d, error);
        QCOMPARE(error.code, QStringLiteral("webdav_auth_failed"));
    }
};
QTEST_GUILESS_MAIN(DavIntegrationTest)
#include "test_dav.moc"
