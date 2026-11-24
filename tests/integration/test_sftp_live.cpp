// SFTP destination (#1871) against a real OpenSSH server: the production SftpProvider,
// putgd-sftp-worker, putgd-sftp-askpass and the user's own ssh. Opt-in, not a ctest: it
// needs the storage lab (tools/lab/storage_servers, PR #1877). Source its env file and run
//   PUTGD_SFTP_LAB_ALIAS=putgd-sftp-lab PUTGD_SFTP_LAB_PP_ALIAS=putgd-sftp-lab-pp test_sftp_live
// where the two aliases are ~/.ssh/config Host entries for the key user with the plain key
// and with the passphrase key (IdentityFile, IdentitiesOnly yes). PUTGD_SFTP_LAB_SLOW_ALIAS,
// optional, names the plain-key alias reached through a ProxyCommand that paces what ssh sends
// to about 50 KB/s (#1888). Secret files are read here only and never printed.
#include "storage/sftp/SftpProvider.h"
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScopeGuard>
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
    QJsonObject journal_;  // the last upload journal entry
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
        // As the resident does: every journal entry is acknowledged once it is saved.
        request.checkpoint = [this](const QJsonObject& journal, std::function<void(Error)> ack) { journal_ = journal; ack({}); };
        UploadResult result;
        provider_->upload(request, [&](UploadResult r) { result = r; });
        return result;
    }
    QString check(const Destination& d, const QString& name) {
        QString status;
        provider_->checkName(d, name, [&](NameCheck c) { status = c.status; });
        return status;
    }
    QString removeReceipt(const StorageReceipt& receipt, SftpProvider* provider = nullptr, Error* error = nullptr) {
        DeleteResult result;
        DeleteRequest request{"d", receipt, {}, std::make_shared<std::atomic_bool>(false)};
        (provider ? provider : provider_.get())->remove(request, [this](const QJsonObject& journal, std::function<void(Error)> ack) { journal_ = journal; ack({}); }, [&](DeleteResult r) { result = r; });
        if (error) *error = result.error;
        return result.status;
    }
    static QString name(const QString& suffix) { return "putgd-live-" + QUuid::createUuid().toString(QUuid::Id128).left(12) + suffix; }
private slots:
    void initTestCase() {
        if (env("PUTGD_LAB_STORAGE_HOST").isEmpty()) QSKIP("Source the storage lab's storage-servers.env first");
        knownHosts_ = dir_.filePath("ssh/known_hosts");
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
        QCOMPARE(QFileInfo(QFileInfo(knownHosts_).absolutePath()).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::ExeOther), QFileDevice::Permissions{});
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
        // The receipt names the server it was verified as; History checks it before anything else.
        // The receipt records the known_hosts line that verified the server; the fingerprint is shown from it.
        QVERIFY(!uploaded.receipt.objectReference["knownHosts"].toArray().isEmpty());
        QVERIFY(sftp::fingerprintValid(uploaded.receipt.objectReference["hostKeyFingerprint"].toString()));
        QCOMPARE(uploaded.receipt.objectReference["user"].toString(), env("PUTGD_LAB_SFTP_KEY_USER"));
        // An upload whose outcome was lost is never claimed without this attempt's own evidence.
        {
            UploadRequest lost;
            lost.destination = d;
            lost.artifact = a;
            lost.candidate = a.name;
            lost.publicationId = a.id;
            lost.condition = "absent";
            lost.resume = {{"schemaVersion", 1}, {"phase", "opening"}, {"path", uploaded.receipt.objectReference["path"]}, {"server", QJsonObject{
                {"host", uploaded.receipt.objectReference["host"]}, {"port", uploaded.receipt.objectReference["port"]}, {"user", uploaded.receipt.objectReference["user"]},
                {"knownHosts", uploaded.receipt.objectReference["knownHosts"]}}}};
            StatResult result;
            provider_->reconcile(lost, [&](StatResult r) { result = r; });
            QCOMPARE(result.status, QString("unknown"));
        }
        // A record whose recorded key isn't the server's: ssh itself refuses, and nothing is deleted.
        {
            auto forged = uploaded.receipt;
            const auto host = forged.objectReference["knownHosts"].toArray().first().toString().section(' ', 0, 0);
            forged.objectReference["knownHosts"] = QJsonArray{host + " ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFIE1eeuTiZD8O0K7jUXHFl1IBU5HCpFTlDsB6zPqGo0"};
            SftpProvider fresh(credentials_, knownHosts_);
            Error refused;
            removeReceipt(forged, &fresh, &refused);
            QCOMPARE(refused.code, QString("sftp_server_changed"));
            QCOMPARE(check(d, a.name), QString("present"));
        }
        // A new session proves the server with the recorded line alone, then deletes.
        {
            SftpProvider fresh(credentials_, knownHosts_);
            QCOMPARE(removeReceipt(uploaded.receipt, &fresh), QString("removed"));
        }
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
        // It says the partial file was removed because the server confirmed it.
        QVERIFY2(result.error.message.contains("was removed"), qPrintable(result.error.message));
        QCOMPARE(check(d, a.name), QString("absent"));
    }
    // #1888: on a slow link CLOSE and REMOVE wait behind the writes already in ssh's buffers; the
    // partial file is then removed over a new connection, and the cancel says so.
    void cancelOnASlowLinkRemovesThePartialFile() {
        if (env("PUTGD_SFTP_LAB_SLOW_ALIAS").isEmpty()) QSKIP("Set PUTGD_SFTP_LAB_SLOW_ALIAS to a paced ~/.ssh/config Host alias");
        qunsetenv("SSH_AUTH_SOCK");
        const auto d = destination(settings(env("PUTGD_SFTP_LAB_SLOW_ALIAS"), {}, env("PUTGD_LAB_SFTP_UPLOAD_DIR")), "slow");
        secrets("slow", {}, {});
        trustServer(d);
        QByteArray bytes(16 * 1024 * 1024, Qt::Uninitialized);
        QRandomGenerator::global()->fillRange(reinterpret_cast<quint32*>(bytes.data()), bytes.size() / 4);
        const auto a = artifact(bytes, name(".bin"));
        bytes.clear();
        auto cancel = std::make_shared<std::atomic_bool>(false);
        QElapsedTimer took;  // from the cancel: it settles inside the resident's 45 s cancel grace
        const auto result = upload(d, a, cancel, [&](const UploadProgress& p) {
            if (p.bytesTransferred.value_or(0) > 256 * 1024 && !cancel->exchange(true)) took.start();
        });
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.error.code, QString("cancelled"));
        QVERIFY2(result.error.message.contains("was removed"), qPrintable(result.error.message));
        QVERIFY2(took.isValid() && took.elapsed() < 45000, qPrintable(QString::number(took.elapsed())));
        qInfo("cancel settled %lld ms after it was asked for", took.elapsed());
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
    // #1892: the key file chosen in Settings, outside ~/.ssh, with no agent at all: ssh gets it with
    // -i and IdentitiesOnly=yes. Plain, then the passphrase key with and without its passphrase.
    void keyFileChosenInSettingsWithoutAnAgent() {
        qunsetenv("SSH_AUTH_SOCK");
        // No desktop agent socket either: the agent lookup finds nothing in an empty runtime folder.
        const auto runtime = qgetenv("XDG_RUNTIME_DIR");
        QTemporaryDir noAgent;
        qputenv("XDG_RUNTIME_DIR", noAgent.path().toUtf8());
        const auto restore = qScopeGuard([&] { qputenv("XDG_RUNTIME_DIR", runtime); });
        QTemporaryDir keys;
        const auto copy = [&](const QString& from, const QString& name) {
            const auto to = keys.filePath(name);
            QVERIFY(QFile::copy(from, to));
            QVERIFY(QFile::setPermissions(to, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        };
        copy(env("PUTGD_LAB_SFTP_KEY_FILE"), "deploy");
        copy(env("PUTGD_LAB_SFTP_KEY_FILE") + ".pub", "deploy.pub");
        copy(env("PUTGD_LAB_SFTP_PASSPHRASE_KEY_FILE"), "locked");
        auto s = settings(hostPort(), env("PUTGD_LAB_SFTP_KEY_USER"), env("PUTGD_LAB_SFTP_UPLOAD_DIR"));
        s["keyFile"] = keys.filePath("deploy");
        const auto plain = destination(s, "chosen");
        secrets("chosen", {}, {});
        trustServer(plain);
        Error error;
        const auto resolved = provider_->setup("inspectAccount", plain, {}, error);
        QVERIFY(error.code.isEmpty());
        QVERIFY2(resolved["line"].toString().endsWith(" with key deploy"), qPrintable(resolved["line"].toString()));
        auto [r1, e1] = test(plain);
        QVERIFY2(e1.code.isEmpty(), qPrintable(e1.code + ": " + e1.message));
        const auto a = artifact("chosen key", name(".txt"));
        const auto uploaded = upload(plain, a);
        QCOMPARE(uploaded.status, QString("uploaded"));
        QCOMPARE(removeReceipt(uploaded.receipt), QString("removed"));
        // The public half, picked by mistake, and a key others can read: put.gd says why before ssh runs.
        s["keyFile"] = keys.filePath("deploy.pub");
        auto [r2, e2] = test(destination(s, "chosen"));
        QCOMPARE(e2.code, QString("ssh_key_public"));
        QCOMPARE(r2["steps"].toArray().size(), 1);
        QVERIFY(QFile::setPermissions(keys.filePath("deploy"), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther));
        s["keyFile"] = keys.filePath("deploy");
        // Another destination: "chosen" still holds its signed-in connection, which needs no key file.
        secrets("open", {}, {});
        auto [r3, e3] = test(destination(s, "open"));
        QCOMPARE(e3.code, QString("ssh_key_permissions"));
        // The passphrase key: asked for by name, then answered from this session's Key passphrase.
        s["keyFile"] = keys.filePath("locked");
        const auto locked = destination(s, "locked");
        secrets("locked", {}, {});
        auto [r4, e4] = test(locked);
        QCOMPARE(e4.code, QString("ssh_auth_failed"));
        QVERIFY2(e4.message.contains("locked needs its passphrase"), qPrintable(e4.message));
        secrets("locked", {}, "not-the-passphrase");
        auto [r5, e5] = test(locked);
        QCOMPARE(e5.code, QString("ssh_auth_failed"));
        QVERIFY2(e5.message.contains("Key passphrase"), qPrintable(e5.message));
        secrets("locked", {}, secretFile("PUTGD_LAB_SFTP_PASSPHRASE_FILE"));
        auto [r6, e6] = test(locked);
        QVERIFY2(e6.code.isEmpty(), qPrintable(e6.code + ": " + e6.message));
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
        QCOMPARE(provider_->setup("createFolder", d, {{"name", missing}, {"path", result["createFolder"].toString()}}, created)["path"].toString(), missing);
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
