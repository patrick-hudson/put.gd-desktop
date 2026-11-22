// putgd-sftp-worker (#1871): SFTP requests for the resident over the user's own ssh.
// One worker serves one destination session: it reads JSON request lines on stdin, keeps
// one ssh connection for them, and exits when stdin closes or after a minute with no
// request. Each request line carries an "id"; the worker answers it with events
// (progress, checkpoint) and one completion line with the same id. Control lines:
// {"cancel":id} and {"ack":bool,"id":id} for a checkpoint.
#include "SftpClient.h"
#include "SftpCommon.h"
#include "SftpSsh.h"
#include "core/BuildInfo.h"
#include "core/ByteSize.h"
#include "core/CaBundle.h"
#include "core/StorageProvider.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QTimer>
#include <QUuid>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
namespace putgd::sftp {
namespace {
constexpr int idleExitMs = 60000;  // the resident reuses a worker only within 50 s of its last answer
constexpr qint64 reconcileReadBytes = 256LL * 1024 * 1024;

class Stdio {
public:
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<QJsonObject> requests, acks;
    bool gone = false;
    std::atomic<qint64> current{0};
    std::atomic_bool cancelled{false};
    std::mutex output;
    void emitLine(const QJsonObject& value) {
        auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
        bytes.append('\n');
        std::lock_guard lock(output);
        std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
        std::fflush(stdout);
    }
    void readLoop() {
        QByteArray line;
        while (true) {
            const int c = std::fgetc(stdin);
            if (c == EOF) break;
            if (c != '\n') {
                if (line.size() < 1024 * 1024) line.append(char(c));
                continue;
            }
            auto value = QJsonDocument::fromJson(line).object();
            line.fill('\0');
            line.clear();
            if (value.contains("cancel")) {
                if (qint64(value["cancel"].toDouble()) == current.load()) cancelled = true;
                continue;
            }
            std::lock_guard lock(mutex);
            if (value.contains("ack")) acks.push_back(value);
            else if (value["method"].isString()) requests.push_back(value);
            changed.notify_all();
        }
        cancelled = true;
        std::lock_guard lock(mutex);
        gone = true;
        changed.notify_all();
    }
    // Waits for the resident to save a journal entry before a step relies on it.
    Error checkpoint(qint64 id, const QJsonObject& journal) {
        emitLine({{"id", double(id)}, {"event", "checkpoint"}, {"journal", journal}});
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, std::chrono::minutes(5), [&] { return !acks.empty() || gone; }))
            return {"sftp_checkpoint_timeout", "The resident did not save the test journal in time", {}};
        if (acks.empty()) return {"cancelled", "The resident stopped this request", {}};
        const auto ack = acks.front();
        acks.pop_front();
        if (ack["ack"].toBool()) return {};
        const auto e = ack["error"].toObject();
        return {e["code"].toString("checkpoint_failed"), e["message"].toString("The test journal could not be saved"), {}};
    }
};
Stdio io;

QString askpassProgram() {
#ifdef PUTGD_SFTP_BUILD_DIR
    if (QDir::cleanPath(QCoreApplication::applicationDirPath()) == QString::fromUtf8(PUTGD_SFTP_BUILD_DIR)) return QString::fromUtf8(PUTGD_SFTP_ASKPASS_PATH);
#endif
    return QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("putgd-sftp-askpass");
}
QByteArray bytesOf(const QString& path) { return path.toUtf8(); }
QString textOf(const QByteArray& path) { return QString::fromUtf8(path); }

// The connection a session keeps: ssh, its SFTP client and where the login folder is.
struct Connection {
    QByteArray key;
    std::unique_ptr<SshProcess> ssh;
    std::unique_ptr<Client> client;
    Resolved resolved;
    QString home;
    bool alive() { return ssh && client && !client->broken() && ssh->running(); }
    void close() {
        client.reset();
        if (ssh) ssh->finish(2000);
        ssh.reset();
    }
};

class Worker {
public:
    QJsonObject run(const QJsonObject& request, Error& error, bool& uncertain) {
        request_ = request;
        id_ = qint64(request["id"].toDouble());
        settings_ = Destination::fromJson(request["destination"].toObject()).settings;
        destination_ = Destination::fromJson(request["destination"].toObject());
        knownHosts_ = request["knownHostsFile"].toString();
        const auto method = request["method"].toString();
        if (method == QLatin1String("inspect")) return inspect(error);
        if (method == QLatin1String("trust")) return trust(error);
        // Every other method reaches the server.
        if (auto e = validate(settings_, true)) { error = e; return {}; }
        if (method == QLatin1String("connect")) { connect(error, false); return {}; }
        if (method == QLatin1String("checkName")) return checkName(error);
        if (method == QLatin1String("upload")) return upload(error, uncertain);
        if (method == QLatin1String("stat")) return identity(StorageReceipt::fromJson(request["receipt"].toObject()), error);
        if (method == QLatin1String("reconcile")) return reconcile(error);
        if (method == QLatin1String("remove")) return remove(error, uncertain);
        if (method == QLatin1String("createFolder")) return createFolder(error);
        if (method == QLatin1String("testDestination")) return testDestination(error);
        error = {"sftp_protocol", "Unknown worker method", {}};
        return {};
    }
    void closeConnection() { connection_.close(); }

private:
    QJsonObject request_, settings_;
    Destination destination_;
    QString knownHosts_;
    qint64 id_ = 0;
    Connection connection_;

    QByteArray connectionKey() const {
        const auto secrets = request_["credentials"].toObject();
        return QCryptographicHash::hash(QJsonDocument(QJsonObject{{"settings", settings_}, {"credentials", secrets}, {"knownHosts", knownHosts_}}).toJson(QJsonDocument::Compact),
                                        QCryptographicHash::Sha256);
    }
    Client& client() { return *connection_.client; }
    // A connection for this request: the session's, when it still runs with the same
    // settings and secrets, else a new one. `probe` fetches an unknown or changed host
    // key, so a test can show it.
    bool connect(Error& error, bool probe) {
        const auto key = connectionKey();
        if (connection_.key == key && connection_.alive()) {
            connection_.client->setCancellation(&io.cancelled);
            return true;
        }
        connection_.close();
        auto env = sshEnvironment();
        const auto program = sshProgram(env);
        if (program.isEmpty()) { error = sshMissing(); return false; }
        auto resolved = resolve(program, env, settings_, error);
        if (error) return false;
        if (!env.contains("SSH_AUTH_SOCK") || env.value("SSH_AUTH_SOCK").isEmpty())
            if (const auto socket = agentSocket(env); !socket.isEmpty()) env.insert("SSH_AUTH_SOCK", socket);
        const auto secrets = request_["credentials"].toObject();
        const auto password = secrets[passwordKey()].toString(), passphrase = secrets[passphraseKey()].toString();
        Askpass askpass(password, passphrase);
        // A FIDO key waits for a touch while ssh signs in; the resident shows this while it waits.
        askpass.onTouch = [id = id_] { io.emitLine({{"id", double(id)}, {"event", "notice"}, {"notice", "touch"}, {"message", "Touch your security key"}}); };
        QString why;
        if (!askpass.start(env, askpassProgram(), &why)) { error = {"ssh_failed", "put.gd couldn't prepare its password prompt: " + why, {}}; return false; }
        auto ssh = std::make_unique<SshProcess>();
        if (!ssh->start(program, connectionArguments(settings_, resolved.userKnownHosts + QStringList{knownHosts_}), env, &why)) {
            error = {"ssh_failed", "put.gd couldn't start ssh: " + why, {}};
            return false;
        }
        auto client = std::make_unique<Client>(*ssh, &io.cancelled);
        bool noise = false;
        if (auto e = client->handshake(noise)) {
            if (e.code == QLatin1String("cancelled")) { error = e; return false; }
            ssh->finish(3000);
            FailureContext context{resolved, !password.isEmpty(), !passphrase.isEmpty(), askpass.asked(), askpass.passphraseKey(), knownHosts_, noise};
            error = classify(ssh->errorText(), context);
            if (error.code == QLatin1String("ssh_failed") && e.code == QLatin1String("sftp_protocol")) error.message = e.message;
            if (askpass.asked().contains(QLatin1String("touch"))) error.details["touched"] = true;
            if (probe && (error.code == QLatin1String("ssh_host_unknown_key") || error.code == QLatin1String("ssh_trusted_key_changed"))) {
                Error probeError;
                const auto line = probeHostKey(program, env, settings_, probeError);
                if (!line.isEmpty()) {
                    error.details["fingerprint"] = fingerprintOfLine(line);
                    error.details["host"] = hostOfLine(line);
                    if (settings_["hostKeyFingerprint"].isString()) error.details["previousFingerprint"] = settings_["hostKeyFingerprint"];
                    error.message = error.code == QLatin1String("ssh_host_unknown_key")
                                        ? "The server's host key is " + fingerprintOfLine(line) + ". Trust it only if it matches the key your server's admin gave you."
                                        : "The server's host key changed since you trusted it in put.gd: it was " + settings_["hostKeyFingerprint"].toString("not recorded") + " and is now " +
                                              fingerprintOfLine(line) + ". Trust the new key only if your server's admin confirms it was replaced.";
                }
            }
            return false;
        }
        askpass.stop();
        connection_.key = key;
        connection_.ssh = std::move(ssh);
        connection_.client = std::move(client);
        connection_.resolved = resolved;
        QByteArray home;
        Client::Status status;
        if (auto e = connection_.client->realpath(".", home, status)) { error = e; connection_.close(); return false; }
        connection_.home = status.code == wire::Ok ? textOf(home) : QString();
        return true;
    }
    QString absoluteFolder() const {
        const auto folder = remoteFolder(settings_["folder"].toString());
        if (folder.startsWith('/')) return folder;
        const auto home = connection_.home.isEmpty() ? QStringLiteral(".") : connection_.home;
        if (folder.isEmpty()) return home;
        return (home.endsWith('/') ? home : home + "/") + folder;
    }
    static QString join(const QString& folder, const QString& name) { return (folder.endsWith('/') ? folder : folder + "/") + name; }
    QString user() const { return connection_.resolved.user; }
    // A refused request about `path`, in the words a user can act on.
    Error refusal(const Client::Status& status, const QString& path, const QString& action, qint64 needed = 0) {
        if (status.code == wire::NoSuchFile) return {"sftp_folder_missing", "The folder " + path + " doesn't exist on the server.", {{"path", path}}};
        if (status.code == wire::PermissionDenied)
            return {"sftp_permission_denied", "The server refused to " + action + " " + path + ": permission denied. Check that " + user() + " can write there.", {{"path", path}}};
        // SFTP version 3 reports a full disk as a plain failure; the free space says which it was.
        if (status.code == wire::Failure && connection_.client && !connection_.client->broken())
            if (const auto fs = client().statvfs(bytesOf(absoluteFolder()))) {
                const auto free = qint64(fs->available * (fs->fragmentSize ? fs->fragmentSize : fs->blockSize));
                if (free < qMax<qint64>(needed, 64 * 1024))
                    return {"sftp_disk_full", "The server has no room for this file in " + absoluteFolder() + " (" + ByteSize::fileSummary(free) + " free).", {{"path", path}}};
            }
        const auto said = status.message.trimmed().isEmpty() ? QStringLiteral("it failed") : status.message.trimmed().left(200);
        return {"sftp_failed", "The server refused to " + action + " " + path + ": " + said + ".", {{"path", path}, {"status", double(status.code)}}};
    }
    // The destination folder exists and is a folder.
    bool folderReady(Error& error) {
        const auto folder = absoluteFolder();
        wire::Attributes attributes;
        Client::Status status;
        if (auto e = client().lstat(bytesOf(folder), attributes, status, true)) { error = e; return false; }
        if (status.code == wire::NoSuchFile) {
            error = {"sftp_folder_missing", "The folder " + folder + " doesn't exist on the server. Create it there, or choose Create folder in Settings.", {{"path", folder}}};
            return false;
        }
        if (status.code != wire::Ok) { error = refusal(status, folder, QStringLiteral("open")); return false; }
        if (attributes.hasPermissions() && !attributes.directory()) {
            error = {"sftp_not_folder", folder + " on the server is a file, not a folder. Choose another Folder on server.", {{"path", folder}}};
            return false;
        }
        return true;
    }
    QJsonObject inspect(Error& error) {
        auto env = sshEnvironment();
        const auto program = sshProgram(env);
        if (program.isEmpty()) { error = sshMissing(); return {{"sshInstalled", false}}; }
        if (auto e = validate(settings_, true)) { error = e; return {{"sshInstalled", true}}; }
        const auto resolved = resolve(program, env, settings_, error);
        if (error) return {{"sshInstalled", true}};
        const auto secrets = request_["credentials"].toObject();
        return {{"sshInstalled", true}, {"resolved", toJson(resolved)}, {"line", resolvedLine(resolved, !secrets[passwordKey()].toString().isEmpty())}};
    }
    // Trust: fetches the key again over the real route and keeps it only when it is the
    // one the user (or an agent) was shown.
    QJsonObject trust(Error& error) {
        if (auto e = validate(settings_, true)) { error = e; return {}; }
        const auto expected = request_["fingerprint"].toString();
        if (!fingerprintValid(expected)) { error = {"sftp_settings_invalid", "Give the fingerprint exactly as put.gd showed it, such as ED25519 SHA256:….", {}}; return {}; }
        auto env = sshEnvironment();
        const auto program = sshProgram(env);
        if (program.isEmpty()) { error = sshMissing(); return {}; }
        const auto line = probeHostKey(program, env, settings_, error);
        if (error) return {};
        const auto fingerprint = fingerprintOfLine(line);
        if (fingerprint != expected) {
            error = {"ssh_host_key_mismatch", "The server now shows the host key " + fingerprint + ", not " + expected + ". Nothing was trusted; test again and compare the new key.",
                     {{"fingerprint", fingerprint}}};
            return {};
        }
        if (auto e = trustLine(knownHosts_, line)) { error = e; return {}; }
        connection_.close();
        return {{"fingerprint", fingerprint}, {"host", hostOfLine(line)}};
    }
    QJsonObject checkName(Error& error) {
        if (!connect(error, false) || !folderReady(error)) return {};
        const auto path = join(absoluteFolder(), request_["candidate"].toString());
        wire::Attributes attributes;
        Client::Status status;
        if (auto e = client().lstat(bytesOf(path), attributes, status)) { error = e; return {}; }
        if (status.code == wire::NoSuchFile) return {{"status", "absent"}, {"objectReference", QJsonObject{{"path", path}}}};
        if (status.code == wire::Ok) return {{"status", "present"}, {"objectReference", QJsonObject{{"path", path}}}};
        error = refusal(status, path, QStringLiteral("check"));
        return {};
    }
    // Removes a file this request created and could not finish; best effort.
    void discard(const QByteArray& handle, const QString& path) {
        if (!connection_.client || connection_.client->broken()) return;
        const std::atomic_bool never{false};
        client().setCancellation(&never);
        Client::Status status;
        if (!handle.isEmpty()) client().close(handle, status);
        client().remove(bytesOf(path), status);
        client().setCancellation(&io.cancelled);
    }
    QJsonObject upload(Error& error, bool& uncertain) {
        uncertain = false;
        const auto artifact = Artifact::fromJson(request_["artifact"].toObject());
        const auto candidate = request_["candidate"].toString();
        if (request_["condition"].toString() != QLatin1String("absent")) {
            error = {"unsupported_capability", "SFTP uploads never replace a file on the server; choose rename or skip for name collisions", {}};
            return {};
        }
        QFile source(artifact.path);
        if (!source.open(QIODevice::ReadOnly) || source.size() != artifact.bytes) { error = {"artifact_invalid", "The file to upload is no longer available", {}}; return {}; }
        io.emitLine({{"id", double(id_)}, {"event", "progress"}, {"phase", "preparing"}, {"bytesTransferred", 0}, {"bytesTotal", double(artifact.bytes)}});
        if (!connect(error, false) || !folderReady(error)) return {};
        const auto path = join(absoluteFolder(), candidate);
        QByteArray handle;
        Client::Status status;
        // The final name, created only if nothing has it: never a temporary name renamed over it,
        // which several servers would let replace another file.
        if (auto e = client().openExclusive(bytesOf(path), handle, status)) { error = e; uncertain = client().broken() && e.code != QLatin1String("cancelled"); return {}; }
        if (status.code != wire::Ok) {
            wire::Attributes there;
            Client::Status seen;
            if (!client().lstat(bytesOf(path), there, seen) && seen.code == wire::Ok)
                return {{"status", "collision"}, {"error", QJsonObject{}}};
            error = refusal(status, path, QStringLiteral("create"), artifact.bytes);
            return {};
        }
        // From here on the file exists: any failure removes it.
        QCryptographicHash hash(QCryptographicHash::Sha256);
        QElapsedTimer last;
        last.start();
        const auto sent = [&](qint64 done) {
            if (last.elapsed() < 250 && done < artifact.bytes) return;
            last.restart();
            io.emitLine({{"id", double(id_)}, {"event", "progress"}, {"phase", "uploading"}, {"bytesTransferred", double(done)}, {"bytesTotal", double(artifact.bytes)}});
        };
        if (auto e = client().write(handle, source, artifact.bytes, hash, sent, status)) {
            const bool cancelled = e.code == QLatin1String("cancelled");
            if (cancelled) e.message = QStringLiteral("Cancelled; the partial file was removed from the server");
            discard(handle, path);
            // A connection that ended mid-file may leave part of it there.
            uncertain = client().broken() && !cancelled;
            if (uncertain) e.details["path"] = path;
            error = e;
            return {};
        }
        if (status.code != wire::Ok) {
            error = refusal(status, path, QStringLiteral("write"), artifact.bytes);
            discard(handle, path);
            return {};
        }
        const auto sha = QString::fromLatin1(hash.result().toHex());
        if (!artifact.sha256.isEmpty() && sha != artifact.sha256) {
            discard(handle, path);
            error = {"artifact_changed", "The file changed while it was uploading; nothing was kept on the server", {}};
            return {};
        }
        io.emitLine({{"id", double(id_)}, {"event", "progress"}, {"phase", "finalizing"}, {"bytesTransferred", double(artifact.bytes)}, {"bytesTotal", double(artifact.bytes)}});
        // Every byte is on the server: a cancel from here on is too late, and the upload finishes.
        const std::atomic_bool finishing{false};
        client().setCancellation(&finishing);
        struct Restore { Client& c; ~Restore() { c.setCancellation(&io.cancelled); } } restore{client()};
        if (client().has("fsync@openssh.com")) {
            if (auto e = client().fsync(handle, status)) { error = e; uncertain = true; return {}; }
            if (status.code != wire::Ok && status.code != wire::Unsupported) { error = refusal(status, path, QStringLiteral("save")); discard(handle, path); return {}; }
        }
        if (auto e = client().close(handle, status)) { error = e; uncertain = true; return {}; }
        if (status.code != wire::Ok) { error = refusal(status, path, QStringLiteral("save"), artifact.bytes); discard({}, path); return {}; }
        wire::Attributes stored;
        if (auto e = client().lstat(bytesOf(path), stored, status)) { error = e; uncertain = true; return {}; }
        if (status.code != wire::Ok || !stored.hasSize() || qint64(stored.size) != artifact.bytes) {
            error = {"sftp_failed", "The server kept " + path + " at another size than was sent", {{"path", path}}};
            uncertain = true;
            return {};
        }
        StorageReceipt receipt;
        receipt.destination = destination_;
        receipt.objectReference = reference(path, stored, sha);
        receipt.concurrencyToken = sha;
        receipt.publicationId = request_["publicationId"].toString();
        receipt.directUrl = linkFor(settings_, candidate);
        return {{"status", "uploaded"}, {"receipt", receipt.toJson()}};
    }
    QJsonObject reference(const QString& path, const wire::Attributes& a, const QString& sha) const {
        return {{"path", path}, {"location", location(settings_, path)}, {"size", double(a.size)}, {"mtime", double(a.mtime)}, {"uid", double(a.uid)}, {"sha256", sha}};
    }
    // Hashes the file at `path` up to `bound` bytes; false when it could not be read.
    bool readBack(const QString& path, qint64 bound, QString& sha, Error& error) {
        QByteArray handle;
        Client::Status status;
        if (auto e = client().openRead(bytesOf(path), handle, status)) { error = e; return false; }
        if (status.code != wire::Ok) return false;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        qint64 size = 0;
        const auto failed = client().readHash(handle, bound, hash, size, status);
        Client::Status closed;
        if (!failed) client().close(handle, closed);
        if (failed) { error = failed; return false; }
        if (status.code != wire::Ok) return false;
        sha = QString::fromLatin1(hash.result().toHex());
        return true;
    }
    // Whether the recorded file is still the one put.gd stored: a regular file of the same
    // size, change time and owner, and, up to 32 MB, the same bytes.
    QJsonObject identity(const StorageReceipt& receipt, Error& error) {
        if (!connect(error, false)) return {};
        const auto object = receipt.objectReference;
        const auto path = object["path"].toString();
        if (path.isEmpty()) { error = {"receipt_invalid", "This upload's record names no file", {}}; return {}; }
        wire::Attributes a;
        Client::Status status;
        if (auto e = client().lstat(bytesOf(path), a, status)) { error = e; return {}; }
        if (status.code == wire::NoSuchFile) return {{"status", "missing"}, {"receipt", receipt.toJson()}};
        if (status.code != wire::Ok) { error = refusal(status, path, QStringLiteral("check")); return {}; }
        const bool same = a.regularFile() && qint64(a.size) == qint64(object["size"].toDouble()) && qint64(a.mtime) == qint64(object["mtime"].toDouble()) &&
                          qint64(a.uid) == qint64(object["uid"].toDouble());
        if (!same) return {{"status", "foreign"}, {"receipt", receipt.toJson()}};
        if (qint64(a.size) <= readBackBytes) {
            QString sha;
            if (!readBack(path, readBackBytes, sha, error)) { if (!error) error = {"sftp_failed", "The server didn't let put.gd read " + path + " back to check it", {{"path", path}}}; return {}; }
            if (sha != object["sha256"].toString()) return {{"status", "foreign"}, {"receipt", receipt.toJson()}};
        }
        return {{"status", "own"}, {"receipt", receipt.toJson()}};
    }
    QJsonObject reconcile(Error& error) {
        const auto artifact = Artifact::fromJson(request_["artifact"].toObject());
        if (!connect(error, false) || !folderReady(error)) return {};
        const auto path = request_["objectReference"].toObject()["path"].toString(join(absoluteFolder(), request_["candidate"].toString()));
        wire::Attributes a;
        Client::Status status;
        if (auto e = client().lstat(bytesOf(path), a, status)) { error = e; return {}; }
        if (status.code == wire::NoSuchFile) return {{"status", "missing"}};
        if (status.code != wire::Ok) { error = refusal(status, path, QStringLiteral("check")); return {}; }
        if (!a.regularFile() || qint64(a.size) != artifact.bytes) return {{"status", "foreign"}};
        if (artifact.bytes > reconcileReadBytes) { error = {"sftp_reconcile_unknown", "put.gd can't tell whether " + path + " is this upload without reading all of it", {{"path", path}}}; return {{"status", "unknown"}}; }
        QString sha;
        if (!readBack(path, reconcileReadBytes, sha, error)) { if (!error) error = {"sftp_failed", "The server didn't let put.gd read " + path + " back", {{"path", path}}}; return {}; }
        if (sha != artifact.sha256) return {{"status", "foreign"}};
        StorageReceipt receipt;
        receipt.destination = destination_;
        receipt.objectReference = reference(path, a, sha);
        receipt.concurrencyToken = sha;
        receipt.publicationId = request_["publicationId"].toString();
        receipt.directUrl = linkFor(settings_, path.section('/', -1));
        return {{"status", "own"}, {"receipt", receipt.toJson()}};
    }
    QJsonObject remove(Error& error, bool& uncertain) {
        const auto receipt = StorageReceipt::fromJson(request_["receipt"].toObject());
        const auto checked = identity(receipt, error);
        if (error) return {{"status", "failed"}};
        if (checked["status"] == "missing") return {{"status", "missing"}};
        if (checked["status"] != "own") return {{"status", "changed"}};
        // Between that check and this removal another program could replace the file; SFTP
        // has no conditional remove. The window is one round trip.
        Client::Status status;
        const auto path = receipt.objectReference["path"].toString();
        if (auto e = client().remove(bytesOf(path), status)) { error = e; uncertain = true; return {{"status", "unknown"}}; }
        if (status.code == wire::Ok) return {{"status", "removed"}};
        if (status.code == wire::NoSuchFile) return {{"status", "missing"}};
        error = refusal(status, path, QStringLiteral("delete"));
        return {{"status", "failed"}};
    }
    QJsonObject createFolder(Error& error) {
        if (!connect(error, false)) return {};
        const auto folder = absoluteFolder();
        // Each missing level, from the top: mkdir -p, with the server's umask deciding modes.
        QString path;
        for (const auto& part : folder.split('/', Qt::SkipEmptyParts)) {
            path = (path.isEmpty() && folder.startsWith('/') ? QStringLiteral("/") : path.isEmpty() ? QString() : path + "/") + part;
            if (path.startsWith(QLatin1String("//"))) path.remove(0, 1);
            wire::Attributes a;
            Client::Status status;
            if (auto e = client().lstat(bytesOf(path), a, status, true)) { error = e; return {}; }
            if (status.code == wire::Ok) {
                if (a.hasPermissions() && !a.directory()) { error = {"sftp_not_folder", path + " on the server is a file, not a folder", {{"path", path}}}; return {}; }
                continue;
            }
            if (status.code != wire::NoSuchFile) { error = refusal(status, path, QStringLiteral("open")); return {}; }
            if (auto e = client().mkdir(bytesOf(path), status)) { error = e; return {}; }
            if (status.code != wire::Ok) { error = refusal(status, path, QStringLiteral("create the folder")); return {}; }
        }
        return {{"path", folder}};
    }
    // Fetches the link anonymously, as anyone given it would: the bytes it returns, or why not.
    struct Fetched { int status = 0; QByteArray body; QString problem; };
    Fetched fetch(const QString& link, qint64 bound) {
        Fetched result;
        QNetworkAccessManager network;
        QNetworkRequest request{QUrl(link, QUrl::StrictMode)};
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
        request.setTransferTimeout(20000);
        auto* reply = network.get(request);
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
            result.body += reply->read(bound + 1 - result.body.size());
            if (result.body.size() > bound) reply->abort();
        });
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QTimer::singleShot(30000, &loop, [reply] { reply->abort(); });
        loop.exec();
        result.body += reply->read(bound + 1 - result.body.size());
        result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError && !result.status) result.problem = reply->errorString().left(200);
        reply->deleteLater();
        return result;
    }
    QJsonObject testDestination(Error& error) {
        QJsonArray steps;
        QJsonObject out;
        const auto step = [&](const QString& name, const QString& status, const QString& detail) { steps.append(QJsonObject{{"step", name}, {"status", status}, {"detail", detail}}); out["steps"] = steps; };
        if (!connect(error, true)) {
            const bool key = error.code.startsWith(QLatin1String("ssh_host")) || error.code == QLatin1String("ssh_trusted_key_changed");
            const bool reached = key || error.code == QLatin1String("ssh_auth_failed") || error.code == QLatin1String("ssh_needs_one_time_code") || error.code == QLatin1String("ssh_no_sftp");
            step("connect", reached ? "passed" : "failed", reached ? QStringLiteral("Reached the server") : error.message);
            if (reached) step("hostKey", key ? "failed" : "passed", key ? error.message : QStringLiteral("The host key matches"));
            if (reached && !key) step("signIn", error.code == QLatin1String("ssh_no_sftp") ? "passed" : "failed", error.code == QLatin1String("ssh_no_sftp") ? QStringLiteral("Signed in") : error.message);
            if (error.code == QLatin1String("ssh_no_sftp")) step("sftp", "failed", error.message);
            return out;
        }
        const auto& r = connection_.resolved;
        step("connect", "passed", "Reached " + (r.host.contains(':') ? "[" + r.host + "]" : r.host) + (r.port != 22 ? ":" + QString::number(r.port) : QString()));
        step("hostKey", "passed", QStringLiteral("The host key matches a trusted one"));
        step("signIn", "passed", "Signed in as " + r.user);
        if (!folderReady(error)) {
            step("folder", "failed", error.message);
            if (error.code == QLatin1String("sftp_folder_missing")) out["createFolder"] = absoluteFolder();
            return out;
        }
        step("folder", "passed", absoluteFolder() + " is a folder");
        // A synthetic file under a name nothing else uses, recorded before it is sent (#1690).
        QByteArray bytes = "put.gd SFTP destination test " + QUuid::createUuid().toByteArray(QUuid::WithoutBraces) + "\n";
        for (int i = 0; i < 32; ++i) bytes += QByteArray::number(QRandomGenerator::global()->generate64(), 36) + "\n";
        const auto name = "putgd-test-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".txt";
        const auto path = join(absoluteFolder(), name);
        if (auto e = io.checkpoint(id_, {{"schemaVersion", 1}, {"phase", "upload_intent"}, {"destination", destination_.toJson()}, {"path", path}})) { error = e; return out; }
        QByteArray handle;
        Client::Status status;
        if (auto e = client().openExclusive(bytesOf(path), handle, status)) { error = e; return out; }
        if (status.code != wire::Ok) { error = refusal(status, path, QStringLiteral("create"), bytes.size()); step("upload", "failed", error.message); return out; }
        QByteArray copy = bytes;
        QBuffer buffer(&copy);
        buffer.open(QIODevice::ReadOnly);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (auto e = client().write(handle, buffer, bytes.size(), hash, {}, status)) { discard(handle, path); error = e; step("upload", "failed", e.message); return out; }
        if (status.code != wire::Ok) { error = refusal(status, path, QStringLiteral("write"), bytes.size()); discard(handle, path); step("upload", "failed", error.message); return out; }
        if (client().has("fsync@openssh.com")) client().fsync(handle, status);
        if (auto e = client().close(handle, status); e || status.code != wire::Ok) { error = e ? e : refusal(status, path, QStringLiteral("save")); discard({}, path); step("upload", "failed", error.message); return out; }
        wire::Attributes stored;
        client().lstat(bytesOf(path), stored, status);
        StorageReceipt receipt;
        receipt.destination = destination_;
        receipt.objectReference = reference(path, stored, QString::fromLatin1(hash.result().toHex()));
        receipt.publicationId = name;
        receipt.directUrl = linkFor(settings_, name);
        out["receipt"] = receipt.toJson();
        if (auto e = io.checkpoint(id_, {{"schemaVersion", 1}, {"phase", "uploaded"}, {"receipt", receipt.toJson()}})) error = e;
        step("upload", "passed", "Uploaded a " + ByteSize::fileSummary(bytes.size()) + " test file as " + location(settings_, path));
        Error linkError;
        if (receipt.directUrl.isEmpty()) {
            out["publicReachable"] = false;
            step("link", "skipped", QStringLiteral("No Link address is set, so uploads give no link"));
        } else {
            const auto fetched = fetch(receipt.directUrl, bytes.size());
            const bool same = fetched.status >= 200 && fetched.status < 300 && fetched.body == bytes;
            out["publicReachable"] = same;
            out["linkStatus"] = fetched.status;
            if (same) step("link", "passed", receipt.directUrl + " returned the same bytes");
            else {
                linkError = fetched.status >= 200 && fetched.status < 300
                                ? Error{"sftp_link_mismatch", "The upload worked, but " + receipt.directUrl + " returned other content. Check that Link address serves Folder on server.", {{"httpStatus", fetched.status}}}
                                : Error{"public_url_unreachable", "The upload worked, but its link " + receipt.directUrl + " did not open (" +
                                            (fetched.status ? "HTTP " + QString::number(fetched.status) : fetched.problem) + "). Check Link address.", {{"httpStatus", fetched.status}}};
                step("link", "failed", linkError.message);
            }
        }
        // Cleanup: this request created the file, so it is removed after the same identity check History uses.
        const auto checked = identity(receipt, error);
        if (!error && checked["status"] == "own") {
            client().remove(bytesOf(path), status);
            out["cleanupStatus"] = status.code == wire::Ok ? "removed" : status.code == wire::NoSuchFile ? "missing" : "failed";
        } else out["cleanupStatus"] = checked["status"] == "missing" ? "missing" : "failed";
        step("cleanup", out["cleanupStatus"] == "failed" ? "failed" : "passed", out["cleanupStatus"] == "failed" ? "Couldn't delete the test file " + path : QStringLiteral("Deleted the test file"));
        if (!error && out["cleanupStatus"] == "failed") error = {"destination_test_incomplete", "The test file " + path + " couldn't be deleted; delete it on the server", {{"path", path}}};
        if (!error && linkError) error = linkError;
        out["summary"] = receipt.directUrl.isEmpty() ? QStringLiteral("Test upload passed: connected, signed in, uploaded and deleted a test file. Uploads give no link without a Link address.")
                                                     : QStringLiteral("Test upload and link passed: connected, signed in, uploaded a test file, opened its link and deleted it.");
        return out;
    }
};
}
}

int main(int argc, char** argv) {
    using namespace putgd::sftp;
    if (argc == 2 && std::strcmp(argv[1], "--version-json") == 0) { std::printf("%s\n", putgd::buildManifestJson()); return 0; }
    if (argc != 2 || std::strcmp(argv[1], "--session") != 0) return 2;
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(workerName);
    putgd::useHostCaBundle();
    std::thread reader([] { io.readLoop(); });
    reader.detach();
    Worker worker;
    while (true) {
        QJsonObject request;
        {
            std::unique_lock lock(io.mutex);
            if (!io.changed.wait_for(lock, std::chrono::milliseconds(idleExitMs), [] { return !io.requests.empty() || io.gone; })) break;
            if (io.requests.empty()) break;  // stdin closed
            request = io.requests.front();
            io.requests.pop_front();
        }
        const auto id = qint64(request["id"].toDouble());
        io.current = id;
        io.cancelled = false;
        putgd::Error error;
        bool uncertain = false;
        if (request["schemaVersion"].toInt() != 1) error = {"sftp_protocol", "Invalid worker request", {}};
        const auto result = error ? QJsonObject{} : worker.run(request, error, uncertain);
        request = {};
        if (error)
            io.emitLine({{"id", double(id)}, {"ok", false}, {"error", QJsonObject{{"code", error.code}, {"message", error.message}, {"details", error.details}}}, {"uncertain", uncertain}, {"result", result}});
        else io.emitLine({{"id", double(id)}, {"ok", true}, {"result", result}});
        io.current = 0;
    }
    worker.closeConnection();
    std::_Exit(0);
}
