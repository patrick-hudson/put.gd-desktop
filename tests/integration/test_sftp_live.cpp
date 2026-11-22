// SFTP destination (#1871) against a real OpenSSH server: the production SftpProvider,
// putgd-sftp-worker, putgd-sftp-askpass and the user's own ssh. Opt-in, not a ctest: it
// needs the storage lab (tools/lab/storage_servers, PR #1877). Source its env file and run
//   PUTGD_SFTP_LAB_ALIAS=putgd-sftp-lab PUTGD_SFTP_LAB_PP_ALIAS=putgd-sftp-lab-pp test_sftp_live
// where the two aliases are ~/.ssh/config Host entries for the key user with the plain key
// and with the passphrase key (IdentityFile, IdentitiesOnly yes). Secret files are read here
// only and never printed.
#include "storage/sftp/SftpProvider.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
using namespace putgd;
namespace {
QString env(const char* name) { return qEnvironmentVariable(name); }
QString secretFile(const char* variable) {
    QFile file(env(variable));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(file.readAll()).trimmed();
}
}
class SftpLiveTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    CredentialStore credentials_;
    std::unique_ptr<SftpProvider> provider_;
    QString knownHosts_;
    QString agent_;  // the test's own ssh-agent socket
    qint64 agentPid_ = 0;
    QJsonObject settings(const QString& server, const QString& user, const QString& folder, bool link = false) const {
        auto s = sftp::defaults();
        s["server"] = server;
        s["user"] = user;
        s["folder"] = folder;
        if (link) s["publicUrlBase"] = env("PUTGD_LAB_SFTP_PUBLIC_URL") + "/";
        return s;
    }
    QString hostPort(const char* portVariable = "PUTGD_LAB_SFTP_PORT") const { return env("PUTGD_LAB_STORAGE_HOST") + ":" + env(portVariable); }
    Destination destination(const QJsonObject& s, const QString& id = "lab") const {
        return {"sftp", id, "r1", s, QJsonObject{{"mode", "session"}, {"reference", QJsonValue::Null}}};
    }
    void secrets(const QString& id, const QString& password, const QString& passphrase) {
        Credentials c;
        c.values = {{"password", password}, {"passphrase", passphrase}};
        credentials_.setSession(id, c);
    }
    std::pair<QJsonObject, Error> test(const Destination& d) {
        QJsonObject out;
        Error error;
        provider_->testDestination(d, [](const QJsonObject&, std::function<void(Error)> ack) { ack({}); }, [&](QJsonObject r, Error e) { out = r; error = e; });
        return {out, error};
    }
    Error trust(const Destination& d, const QString& fingerprint) {
        Error error;
        provider_->setup("trustHostKey", d, {{"fingerprint", fingerprint}}, error);
        return error;
    }
    // The server's key, trusted as a user does: Test shows it, Trust key keeps exactly it.
    void trustServer(const Destination& d) {
        auto [result, error] = test(d);
        if (error.code != "ssh_host_unknown_key") return;
        QVERIFY(sftp::fingerprintValid(error.details["fingerprint"].toString()));
        QVERIFY(trust(d, error.details["fingerprint"].toString()).code.isEmpty());
    }
    Artifact artifact(const QByteArray& bytes, const QString& name) {
        Artifact a;
        a.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        a.path = dir_.filePath(a.id);
        QFile file(a.path);
        file.open(QIODevice::WriteOnly);
        file.write(bytes);
        file.close();
        a.name = name;
        a.bytes = bytes.size();
        a.sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        return a;
    }
    UploadResult upload(const Destination& d, const Artifact& a, std::shared_ptr<std::atomic_bool> cancel = {}, std::function<void(const UploadProgress&)> progress = {}) {
        UploadRequest request;
        request.destination = d;
        request.artifact = a;
        request.candidate = a.name;
        request.publicationId = a.id;
        request.condition = "absent";
        request.cancellation = cancel ? cancel : std::make_shared<std::atomic_bool>(false);
        request.progress = std::move(progress);
        UploadResult result;
        provider_->upload(request, [&](UploadResult r) { result = r; });
        return result;
    }
    QString check(const Destination& d, const QString& name) {
        QString status;
        provider_->checkName(d, name, [&](NameCheck c) { status = c.status; });
        return status;
    }
    QString removeReceipt(const StorageReceipt& receipt) {
        DeleteResult result;
        DeleteRequest request{"d", receipt, {}, std::make_shared<std::atomic_bool>(false)};
        provider_->remove(request, {}, [&](DeleteResult r) { result = r; });
        return result.status;
    }
    static QString name(const QString& suffix) { return "putgd-live-" + QUuid::createUuid().toString(QUuid::Id128).left(12) + suffix; }
private slots:
    void initTestCase() {
        if (env("PUTGD_LAB_STORAGE_HOST").isEmpty()) QSKIP("Source the storage lab's storage-servers.env first");
        knownHosts_ = dir_.filePath("known_hosts");
        // The lab's public HTTPS location uses its own CA; the worker adds it as an AppImage adds the host's.
        qputenv("PUTGD_CA_BUNDLE", env("PUTGD_LAB_CA_FILE").toUtf8());
        provider_ = std::make_unique<SftpProvider>(credentials_, knownHosts_);
    }
    void unknownKeyIsShownTrustedExactlyThenTestPasses() {
        qunsetenv("SSH_AUTH_SOCK");
        secrets("lab", secretFile("PUTGD_LAB_SFTP_PW_FILE"), {});
        const auto d = destination(settings(hostPort(), env("PUTGD_LAB_SFTP_PW_USER"), env("PUTGD_LAB_SFTP_UPLOAD_DIR"), true));
        auto [result, error] = test(d);
        QCOMPARE(error.code, QString("ssh_host_unknown_key"));
        const auto fingerprint = error.details["fingerprint"].toString();
        QVERIFY(sftp::fingerprintValid(fingerprint));
        QVERIFY(error.message.contains(fingerprint));
        QCOMPARE(result["steps"].toArray().last().toObject()["step"].toString(), QString("hostKey"));
        // Another fingerprint is refused and nothing is written.
        QCOMPARE(trust(d, "ED25519 SHA256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA").code, QString("ssh_host_key_mismatch"));
        QVERIFY(!QFile::exists(knownHosts_));
        QVERIFY(trust(d, fingerprint).code.isEmpty());
        QVERIFY(QFile(knownHosts_).permissions() == (QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser | QFileDevice::WriteUser));
        std::tie(result, error) = test(d);
        QVERIFY2(error.code.isEmpty(), qPrintable(error.code + ": " + error.message));
        QVERIFY(result["publicReachable"].toBool());
        QCOMPARE(result["cleanupStatus"].toString(), QString("removed"));
        QStringList steps;
        for (const auto& step : result["steps"].toArray()) steps << step.toObject()["step"].toString() + "=" + step.toObject()["status"].toString();
        QCOMPARE(steps, QStringList({"connect=passed", "hostKey=passed", "signIn=passed", "folder=passed", "upload=passed", "link=passed", "cleanup=passed"}));
    }
    void passwordIsAskedForAndAnswered() {
        qunsetenv("SSH_AUTH_SOCK");
        const auto s = settings(hostPort(), env("PUTGD_LAB_SFTP_PW_USER"), env("PUTGD_LAB_SFTP_UPLOAD_DIR"));
        secrets("nopw", {}, {});
        auto [r1, none] = test(destination(s, "nopw"));
        QCOMPARE(none.code, QString("ssh_auth_failed"));
        QVERIFY2(none.message.contains("Password"), qPrintable(none.message));
        secrets("wrongpw", "not-the-password", {});
        auto [r2, wrong] = test(destination(s, "wrongpw"));
        QCOMPARE(wrong.code, QString("ssh_auth_failed"));
        // A passphrase never answers a password prompt.
        secrets("pponly", {}, secretFile("PUTGD_LAB_SFTP_PW_FILE"));
        auto [r3, misplaced] = test(destination(s, "pponly"));
        QCOMPARE(misplaced.code, QString("ssh_auth_failed"));
    }
    void agentKeysSignIn() {
        // The test's own agent with the lab key, as a desktop's agent would hold it.
        QProcess agent;
        agent.start("ssh-agent", {"-s"});
        QVERIFY(agent.waitForFinished(10000));
        const auto out = QString::fromUtf8(agent.readAllStandardOutput());
        agent_ = QRegularExpression("SSH_AUTH_SOCK=([^;]+);").match(out).captured(1);
        agentPid_ = QRegularExpression("SSH_AGENT_PID=(\\d+);").match(out).captured(1).toLongLong();
        QVERIFY(!agent_.isEmpty());
        qputenv("SSH_AUTH_SOCK", agent_.toUtf8());
        QProcess add;
        add.start("ssh-add", {"-q", env("PUTGD_LAB_SFTP_KEY_FILE")});
        QVERIFY(add.waitForFinished(10000) && add.exitCode() == 0);
        const auto d = destination(settings(hostPort(), env("PUTGD_LAB_SFTP_KEY_USER"), env("PUTGD_LAB_SFTP_UPLOAD_DIR"), true), "agent");
        secrets("agent", {}, {});
        auto [result, error] = test(d);
        QVERIFY2(error.code.isEmpty(), qPrintable(error.code + ": " + error.message));
        // Upload, collision, progress, then deletion and a changed file's refusal.
        QList<qint64> seen;
        const auto bytes = QByteArray(300 * 1024, 'p');
        const auto a = artifact(bytes, name(".png"));
        const auto uploaded = upload(d, a, {}, [&](const UploadProgress& p) { if (p.bytesTransferred) seen << *p.bytesTransferred; });
        QCOMPARE(uploaded.status, QString("uploaded"));
        QVERIFY(!seen.isEmpty());
        QCOMPARE(uploaded.receipt.directUrl, env("PUTGD_LAB_SFTP_PUBLIC_URL") + "/" + a.name);
        QCOMPARE(check(d, a.name), QString("present"));
        QCOMPARE(upload(d, a).status, QString("collision"));
        QCOMPARE(removeReceipt(uploaded.receipt), QString("removed"));
        QCOMPARE(check(d, a.name), QString("absent"));
        // The same name holding other bytes is not the recorded upload: deletion refuses it.
        const auto first = upload(d, a);
        QCOMPARE(first.status, QString("uploaded"));
        QCOMPARE(removeReceipt(first.receipt), QString("removed"));
        const auto other = upload(d, artifact(QByteArray(300 * 1024, 'q'), a.name));
        QCOMPARE(other.status, QString("uploaded"));
        QCOMPARE(removeReceipt(first.receipt), QString("changed"));
        QCOMPARE(check(d, a.name), QString("present"));
        QCOMPARE(removeReceipt(other.receipt), QString("removed"));
    }
    void cancelRemovesThePartialFile() {
        QVERIFY(!agent_.isEmpty());
        const auto d = destination(settings(hostPort(), env("PUTGD_LAB_SFTP_KEY_USER"), env("PUTGD_LAB_SFTP_UPLOAD_DIR")), "agent");
        QByteArray bytes(256 * 1024 * 1024, Qt::Uninitialized);
        QRandomGenerator::global()->fillRange(reinterpret_cast<quint32*>(bytes.data()), bytes.size() / 4);
        const auto a = artifact(bytes, name(".bin"));
        bytes.clear();
        auto cancel = std::make_shared<std::atomic_bool>(false);
        const auto result = upload(d, a, cancel, [&](const UploadProgress& p) { if (p.bytesTransferred.value_or(0) > 4 * 1024 * 1024) *cancel = true; });
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.error.code, QString("cancelled"));
        QCOMPARE(check(d, a.name), QString("absent"));
    }
    void keyFileAndPassphraseFromSshConfig() {
        if (env("PUTGD_SFTP_LAB_ALIAS").isEmpty()) QSKIP("Set PUTGD_SFTP_LAB_ALIAS and PUTGD_SFTP_LAB_PP_ALIAS to ~/.ssh/config Host aliases");
        qunsetenv("SSH_AUTH_SOCK");
        const auto plain = destination(settings(env("PUTGD_SFTP_LAB_ALIAS"), {}, env("PUTGD_LAB_SFTP_UPLOAD_DIR")), "alias");
        secrets("alias", {}, {});
        trustServer(plain);
        Error error;
        const auto resolved = provider_->setup("inspectAccount", plain, {}, error);
        QVERIFY(error.code.isEmpty());
        QVERIFY2(resolved["line"].toString().contains("as " + env("PUTGD_LAB_SFTP_KEY_USER") + " with key"), qPrintable(resolved["line"].toString()));
        auto [r1, e1] = test(plain);
        QVERIFY2(e1.code.isEmpty(), qPrintable(e1.code + ": " + e1.message));
        const auto pp = destination(settings(env("PUTGD_SFTP_LAB_PP_ALIAS"), {}, env("PUTGD_LAB_SFTP_UPLOAD_DIR")), "pp");
        secrets("pp", {}, {});
        auto [r2, e2] = test(pp);
        QCOMPARE(e2.code, QString("ssh_auth_failed"));
        QVERIFY2(e2.message.contains("Key passphrase"), qPrintable(e2.message));
        // A password never answers a passphrase prompt.
        secrets("pp", secretFile("PUTGD_LAB_SFTP_PASSPHRASE_FILE"), {});
        auto [r3, e3] = test(pp);
        QCOMPARE(e3.code, QString("ssh_auth_failed"));
        secrets("pp", {}, secretFile("PUTGD_LAB_SFTP_PASSPHRASE_FILE"));
        auto [r4, e4] = test(pp);
        QVERIFY2(e4.code.isEmpty(), qPrintable(e4.code + ": " + e4.message));
    }
    void serverRefusalsSayWhatHappened() {
        QVERIFY(!agent_.isEmpty());
        qputenv("SSH_AUTH_SOCK", agent_.toUtf8());
        const auto user = env("PUTGD_LAB_SFTP_KEY_USER");
        const auto small = artifact("tiny", name(".txt"));
        QCOMPARE(upload(destination(settings(hostPort(), user, env("PUTGD_LAB_SFTP_RO_DIR")), "agent"), small).error.code, QString("sftp_permission_denied"));
        // A full disk: the write fails, statvfs confirms no room, and the partial file is removed.
        const auto fullDestination = destination(settings(hostPort(), user, env("PUTGD_LAB_SFTP_FULL_DIR")), "agent");
        const auto big = artifact(QByteArray(2 * 1024 * 1024, 'f'), name(".bin"));
        const auto full = upload(fullDestination, big);
        QVERIFY2(full.error.code == "sftp_disk_full", qPrintable(full.error.code + ": " + full.error.message));
        QCOMPARE(check(fullDestination, big.name), QString("absent"));
        // A missing folder is reported with its path; Test offers Create folder, and uploads never create it.
        const auto missing = env("PUTGD_LAB_SFTP_UPLOAD_DIR") + "/" + name("") + "/shots";
        const auto d = destination(settings(hostPort(), user, missing), "agent");
        QCOMPARE(upload(d, small).error.code, QString("sftp_folder_missing"));
        auto [result, error] = test(d);
        QCOMPARE(error.code, QString("sftp_folder_missing"));
        QCOMPARE(result["createFolder"].toString(), missing);
        Error created;
        QCOMPARE(provider_->setup("createFolder", d, {{"name", missing}}, created)["path"].toString(), missing);
        QVERIFY(created.code.isEmpty());
        std::tie(result, error) = test(d);
        QVERIFY2(error.code.isEmpty(), qPrintable(error.code + ": " + error.message));
        // A server without the SFTP subsystem.
        const auto nosftp = destination(settings(hostPort("PUTGD_LAB_SFTP_NOSFTP_PORT"), user, env("PUTGD_LAB_SFTP_UPLOAD_DIR")), "nosftp");
        secrets("nosftp", {}, {});
        trustServer(nosftp);
        std::tie(result, error) = test(nosftp);
        QCOMPARE(error.code, QString("ssh_no_sftp"));
    }
    void cleanupTestCase() {
        if (agentPid_ > 0) QProcess::execute("kill", {QString::number(agentPid_)});
    }
};
QTEST_GUILESS_MAIN(SftpLiveTest)
#include "test_sftp_live.moc"
