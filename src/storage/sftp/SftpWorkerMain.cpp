// putgd-sftp-worker (#1871): SFTP requests for the resident over the user's own ssh.
// One worker serves one destination session: it reads JSON request lines on stdin, keeps
// one ssh connection for them, and exits when stdin closes or after a minute with no
// request. Each request line carries an "id"; the worker answers it with events
// (progress, checkpoint, notice) and one completion line with the same id. Control lines:
// {"cancel":id}, which holds from the moment it arrives, queued or running, and
// {"ack":bool,"id":id} for a checkpoint.
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
#include <QStringDecoder>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <deque>
#include <mutex>
#include <set>
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
    std::set<qint64> cancelledIds;
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
            std::lock_guard lock(mutex);
            if (value.contains("cancel")) {
                // A cancel holds for its request whether it is running or still queued.
                const auto id = qint64(value["cancel"].toDouble());
                if (id == current.load()) cancelled = true;
                else if (cancelledIds.size() < 256) cancelledIds.insert(id);
                continue;
            }
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
        const auto mine = [&] { return std::find_if(acks.begin(), acks.end(), [&](const QJsonObject& ack) { return qint64(ack["id"].toDouble()) == id; }); };
        if (!changed.wait_for(lock, std::chrono::minutes(5), [&] { return mine() != acks.end() || gone; }))
            return {"sftp_checkpoint_timeout", "The resident did not save the journal in time", {}};
        const auto found = mine();
        if (found == acks.end()) return {"cancelled", "The resident stopped this request", {}};
        const auto ack = *found;
        acks.erase(found);
        if (ack["ack"].toBool()) return {};
        const auto e = ack["error"].toObject();
        return {e["code"].toString("checkpoint_failed"), e["message"].toString("The journal could not be saved"), {}};
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
// What a recorded upload needs for its identity check: put.gd never records one without them.
bool complete(const wire::Attributes& a) { return a.hasSize() && a.hasOwner() && a.hasTimes() && a.hasPermissions(); }
bool sameFile(const wire::Attributes& a, const wire::Attributes& b) {
    return complete(a) && complete(b) && a.regularFile() && b.regularFile() && a.size == b.size && a.mtime == b.mtime && a.uid == b.uid;
}
QString statusName(quint32 code) {
    switch (code) {
    case wire::Eof: return QStringLiteral("end of file");
    case wire::NoSuchFile: return QStringLiteral("no such file");
    case wire::PermissionDenied: return QStringLiteral("permission denied");
    case wire::BadMessage: return QStringLiteral("bad message");
    case wire::Unsupported: return QStringLiteral("not supported");
    default: return QStringLiteral("failure");
    }
}

// The connection a session keeps: ssh, its SFTP client, where the login folder is, and the
// server identity: the resolved host, port and user, and the known_hosts lines that hold a key
// for it in the files this connection's ssh checked (`knownError` when they couldn't be read).
struct Connection {
    QByteArray key;
    std::unique_ptr<SshProcess> ssh;
    std::unique_ptr<Client> client;
    std::unique_ptr<QTemporaryDir> pinFolder;  // the recorded lines a pinned connection checks
    Resolved resolved;
    QString home;
    bool homeValid = false;  // the login folder's path came back as UTF-8
    QStringList knownLines;
    Error knownError;
    bool alive() { return ssh && client && !client->broken() && ssh->running(); }
    void close() {
        client.reset();
        if (ssh) ssh->finish(2000);
        ssh.reset();
        pinFolder.reset();
    }
    // Ends ssh at once: what it still holds for the server (a cancelled upload's writes) is dropped.
    void kill() {
        client.reset();
        if (ssh) ssh->kill();
        ssh.reset();
        pinFolder.reset();
    }
    // The fingerprint shown for the server: computed from the recorded line, for display only.
    QString fingerprint() const {
        for (const auto& line : knownLines) if (!line.startsWith('@')) if (const auto f = fingerprintOfLine(line); !f.isEmpty()) return f;
        return {};
    }
    QJsonObject server() const {
        return {{"host", resolved.host}, {"port", resolved.port}, {"user", resolved.user}, {"knownHosts", QJsonArray::fromStringList(knownLines)}, {"hostKeyFingerprint", fingerprint()}};
    }
};

class Worker {
public:
    // `secrets` arrives apart from the request and is emptied here once read.
    QJsonObject run(const QJsonObject& request, QJsonObject& secrets, Error& error, bool& uncertain) {
        request_ = request;
        id_ = qint64(request["id"].toDouble());
        destination_ = Destination::fromJson(request["destination"].toObject());
        settings_ = destination_.settings;
        knownHosts_ = request["knownHostsFile"].toString();
        // Only methods that sign in get the password and passphrase; they're dropped after sign-in.
        password_ = secrets[passwordKey()].toString();
        passphrase_ = secrets[passphraseKey()].toString();
        secrets = {};
        hadPassword_ = !password_.isEmpty();
        // An upload keeps them until it ends: cleaning up a cancelled one may need a new connection.
        keepSecrets_ = request["method"].toString() == QLatin1String("upload");
        hadPassphrase_ = !passphrase_.isEmpty();
        // Computed once: a second connection in this request must not see the wiped secrets.
        requestKey_ = connectionKey();
        struct Forget { Worker& w; ~Forget() { w.password_.fill(QChar(0)); w.password_.clear(); w.passphrase_.fill(QChar(0)); w.passphrase_.clear(); } } forget{*this};
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
    QString knownHosts_, password_, passphrase_;
    QByteArray requestKey_;
    bool hadPassword_ = false, hadPassphrase_ = false, keepSecrets_ = false;
    // What stops this request's SFTP calls: the cancel, except while a cancelled upload cleans up.
    const std::atomic_bool* cancel_ = &io.cancelled;
    qint64 id_ = 0;
    Connection connection_;

    QByteArray connectionKey() const {
        return QCryptographicHash::hash(QJsonDocument(QJsonObject{{"settings", settings_}, {"password", password_}, {"passphrase", passphrase_}, {"knownHosts", knownHosts_}})
                                            .toJson(QJsonDocument::Compact),
                                        QCryptographicHash::Sha256);
    }
    Client& client() { return *connection_.client; }
    // A connection for this request: the session's, when it still runs with the same
    // settings and secrets, else a new one. `probe` fetches an unknown or changed host
    // key, so a test can show it.
    // `pinned`: the known_hosts lines a receipt recorded. ssh then checks the server against them
    // alone (no other user, global or put.gd file), so it proves the server still holds that key.
    bool connect(Error& error, bool probe, const QStringList& pinned = {}) {
        auto key = requestKey_;
        if (!pinned.isEmpty()) key += QCryptographicHash::hash(pinned.join('\n').toUtf8(), QCryptographicHash::Sha256);
        if (connection_.key == key && connection_.alive()) {
            connection_.client->setCancellation(cancel_);
            return true;
        }
        connection_.close();
        // A request cancelled while it waited starts nothing.
        if (cancel_->load()) { error = {"cancelled", "Cancelled before it started", {}}; return false; }
        auto env = sshEnvironment();
        const auto program = sshProgram(env);
        if (program.isEmpty()) { error = sshMissing(); return false; }
        // A chosen key file ssh would skip or refuse says why now, in put.gd's words (#1892).
        if (const auto key = keyFile(settings_); !key.isEmpty())
            if (auto e = keyFileProblem(key)) { error = e; return false; }
        auto resolved = resolve(program, env, settings_, error);
        if (error) return false;
        if (!env.contains("SSH_AUTH_SOCK") || env.value("SSH_AUTH_SOCK").isEmpty())
            if (const auto socket = agentSocket(env); !socket.isEmpty()) env.insert("SSH_AUTH_SOCK", socket);
        QString knownHosts;
        KnownLookup user;
        QStringList checked;  // the files this connection's ssh checks
        std::unique_ptr<QTemporaryDir> pinFolder;
        if (pinned.isEmpty()) {
            // The user's own known_hosts decides alone once it holds any key for this server; a
            // lookup that fails decides nothing, and nothing connects.
            user = userKnownLookup(resolved, env);
            if (user.error) { error = user.error; return false; }
            if (user.lines.isEmpty())
                if (auto e = checkPinFile(knownHosts_)) { error = e; return false; }
            knownHosts = knownHostsOption(resolved, knownHosts_, !user.lines.isEmpty());
            checked = resolved.userKnownHosts + resolved.globalKnownHosts;
            if (user.lines.isEmpty() && !resolved.userKnownHostsNone) checked.append(knownHosts_);
        } else {
            pinFolder = std::make_unique<QTemporaryDir>();
            QFile file(pinFolder->filePath("known_hosts"));
            if (!pinFolder->isValid() || !file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
                file.write((pinned.join('\n') + '\n').toUtf8()) < 0 || !file.flush()) {
                error = {"sftp_failed", "put.gd couldn't prepare the recorded host key for this check", {}};
                return false;
            }
            knownHosts = "\"" + file.fileName() + "\"";
        }
        QStringList hosts{resolved.host};
        if (!resolved.hostKeyAlias.isEmpty()) hosts.prepend(resolved.hostKeyAlias);
        // The chosen key's prompt names it exactly as given, which ssh -G's output may not keep (a
        // trailing space).
        auto keyFiles = resolved.identityFiles;
        if (!resolved.chosenKey.isEmpty() && !keyFiles.contains(resolved.chosenKey)) keyFiles.prepend(resolved.chosenKey);
        Askpass askpass(password_, passphrase_, resolved.user, hosts, keyFiles);
        // A FIDO key waits for a touch while ssh signs in; the resident shows this while it waits.
        askpass.onTouch = [id = id_] { io.emitLine({{"id", double(id)}, {"event", "notice"}, {"notice", "touch"}, {"message", "Touch your security key"}}); };
        QString why;
        if (!askpass.start(env, askpassProgram(), &why)) { error = {"ssh_failed", "put.gd couldn't prepare its password prompt: " + why, {}}; return false; }
        auto ssh = std::make_unique<SshProcess>();
        if (!ssh->start(program, connectionArguments(settings_, knownHosts, !pinned.isEmpty()), env, &why)) {
            error = {"ssh_failed", "put.gd couldn't start ssh: " + why, {}};
            return false;
        }
        askpass.setSshPid(ssh->pid());
        auto client = std::make_unique<Client>(*ssh, cancel_);
        bool noise = false;
        const auto failed = client->handshake(noise);
        // Signing in is over either way: nothing more is answered, and both secrets are forgotten
        // (an upload's at its end).
        askpass.stop();
        if (!keepSecrets_) forgetSecrets();
        if (failed) {
            if (failed.code == QLatin1String("cancelled")) { error = failed; return false; }
            ssh->finish(3000);
            FailureContext context{resolved, hadPassword_, hadPassphrase_, askpass.asked(), askpass.passphraseKey(), user.file, !pinned.isEmpty(), noise, ssh->exitCode()};
            error = classify(ssh->errorText(), context);
            if (error.code == QLatin1String("ssh_failed") && failed.code == QLatin1String("sftp_protocol")) error.message = failed.message;
            if (askpass.asked().contains(QLatin1String("touch"))) error.details["touched"] = true;
            if (probe && (error.code == QLatin1String("ssh_host_unknown_key") || error.code == QLatin1String("ssh_trusted_key_changed"))) {
                Error probeError;
                const auto line = probeHostKey(program, settings_, probeError);
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
        connection_.key = key;
        connection_.pinFolder = std::move(pinFolder);
        connection_.knownLines = pinned;
        connection_.knownError = {};
        if (pinned.isEmpty()) {
            // What ssh accepted this server by: every line for it in the files it checked.
            const auto lookup = lookupHost(checked, resolved.token(), env);
            connection_.knownLines = lookup.lines;
            connection_.knownError = lookup.error ? lookup.error : lookup.lines.isEmpty() ? Error{"sftp_failed", "put.gd couldn't find the known_hosts entry that verified this server, so it can't record the upload", {}} : Error{};
        }
        connection_.ssh = std::move(ssh);
        connection_.client = std::move(client);
        connection_.resolved = resolved;
        QByteArray home;
        Client::Status status;
        if (auto e = connection_.client->realpath(".", home, status)) { error = e; connection_.close(); return false; }
        connection_.homeValid = status.code == wire::Ok && !home.isEmpty() && utf8RoundTrip(home);
        connection_.home = connection_.homeValid ? textOf(home) : QString();
        return true;
    }
    void forgetSecrets() {
        password_.fill(QChar(0));
        password_.clear();
        passphrase_.fill(QChar(0));
        passphrase_.clear();
    }
    // A server path put.gd records: valid UTF-8 that comes back byte for byte.
    static bool utf8RoundTrip(const QByteArray& bytes) {
        QStringDecoder decode(QStringDecoder::Utf8);
        const QString text = decode(bytes);
        return !decode.hasError() && text.toUtf8() == bytes;
    }
    // The folder as the settings name it: the login folder joined to a relative path.
    QString namedFolder() const {
        const auto folder = remoteFolder(settings_["folder"].toString());
        if (folder.startsWith('/')) return folder;
        const auto home = connection_.home.isEmpty() ? QStringLiteral(".") : connection_.home;
        if (folder.isEmpty()) return home;
        return (home.endsWith('/') ? home : home + "/") + folder;
    }
    static QString join(const QString& folder, const QString& name) { return (folder.endsWith('/') ? folder : folder + "/") + name; }
    QString user() const { return connection_.resolved.user; }
    // A refused request about `path`, in the words a user can act on; the server's own status
    // text is never repeated.
    Error refusal(const Client::Status& status, const QString& path, const QString& action, qint64 needed = 0) {
        if (status.code == wire::NoSuchFile) return {"sftp_folder_missing", "The folder " + path + " doesn't exist on the server.", {{"path", path}}};
        if (status.code == wire::PermissionDenied)
            return {"sftp_permission_denied", "The server refused to " + action + " " + path + ": permission denied. Check that " + user() + " can write there.", {{"path", path}}};
        // SFTP version 3 reports a full disk as a plain failure; the free space says which it was.
        if (status.code == wire::Failure && connection_.client && !connection_.client->broken())
            if (const auto fs = client().statvfs(bytesOf(namedFolder()))) {
                const auto free = qint64(fs->available * (fs->fragmentSize ? fs->fragmentSize : fs->blockSize));
                if (free < qMax<qint64>(needed, 64 * 1024))
                    return {"sftp_disk_full", "The server has no room for this file in " + namedFolder() + " (" + ByteSize::fileSummary(free) + " free).", {{"path", path}}};
            }
        return {"sftp_failed", "The server refused to " + action + " " + path + " (" + statusName(status.code) + ").", {{"path", path}, {"status", double(status.code)}}};
    }
    // The destination folder, as the server resolves it (realpath): the canonical path every
    // name check, upload, receipt and deletion uses.
    bool folderReady(Error& error, QString& canonical) {
        const auto folder = namedFolder();
        QByteArray resolved;
        Client::Status status;
        if (auto e = client().realpath(bytesOf(folder), resolved, status)) { error = e; return false; }
        if (status.code == wire::NoSuchFile) {
            error = {"sftp_folder_missing", "The folder " + folder + " doesn't exist on the server. Create it there, or choose Create folder in Settings.", {{"path", folder}}};
            return false;
        }
        if (status.code != wire::Ok) { error = refusal(status, folder, QStringLiteral("open")); return false; }
        if (!utf8RoundTrip(resolved)) {
            error = {"sftp_path_unsupported", "The server names the folder " + folder + " with bytes that aren't UTF-8, which put.gd can't record. Choose a folder with a plain name.", {{"path", folder}}};
            return false;
        }
        wire::Attributes attributes;
        if (auto e = client().lstat(resolved, attributes, status, true)) { error = e; return false; }
        if (status.code == wire::NoSuchFile) {
            error = {"sftp_folder_missing", "The folder " + folder + " doesn't exist on the server. Create it there, or choose Create folder in Settings.", {{"path", folder}}};
            return false;
        }
        if (status.code != wire::Ok) { error = refusal(status, folder, QStringLiteral("open")); return false; }
        if (!attributes.directory()) {
            error = {"sftp_not_folder", folder + " on the server is not a folder. Choose another Folder on server.", {{"path", folder}}};
            return false;
        }
        canonical = textOf(resolved);
        return true;
    }
    QJsonObject inspect(Error& error) {
        auto env = sshEnvironment();
        const auto program = sshProgram(env);
        if (program.isEmpty()) { error = sshMissing(); return {{"sshInstalled", false}}; }
        if (auto e = validate(settings_, true)) { error = e; return {{"sshInstalled", true}}; }
        // A chosen key file ssh can't use is the answer, before ssh -G runs with it (#1892);
        // Settings shows it beside SSH key (details.field keyFile).
        if (const auto key = keyFile(settings_); !key.isEmpty())
            if (auto e = keyFileProblem(key)) { error = e; return {{"sshInstalled", true}}; }
        const auto resolved = resolve(program, env, settings_, error);
        if (error) return {{"sshInstalled", true}};
        // Inspect learns only whether a password is set, never the password.
        return {{"sshInstalled", true}, {"resolved", toJson(resolved)}, {"line", resolvedLine(resolved, request_["passwordSet"].toBool())}};
    }
    // Trust: fetches the key again over the real route and keeps it only when it is the one
    // the user (or an agent) was shown, for one literal host, and only where the user's own
    // known_hosts says nothing about this server.
    QJsonObject trust(Error& error) {
        if (auto e = validate(settings_, true)) { error = e; return {}; }
        const auto expected = request_["fingerprint"].toString();
        if (!fingerprintValid(expected)) { error = {"sftp_settings_invalid", "Give the fingerprint exactly as put.gd showed it, such as ED25519 SHA256:….", {}}; return {}; }
        auto env = sshEnvironment();
        const auto program = sshProgram(env);
        if (program.isEmpty()) { error = sshMissing(); return {}; }
        const auto resolved = resolve(program, env, settings_, error);
        if (error) return {};
        if (resolved.userKnownHostsNone) {
            error = {"ssh_known_hosts_off", "Your SSH settings turn known_hosts files off for this server (UserKnownHostsFile none), so put.gd can't keep a key for it.", {}};
            return {};
        }
        const auto token = resolved.token();
        if (!literalHostToken(token)) {
            error = {"ssh_host_alias_pattern", "This server's HostKeyAlias isn't one literal host name, so put.gd can't trust a key for it.", {}};
            return {};
        }
        const auto user = userKnownLookup(resolved, env);
        if (user.error) { error = user.error; return {}; }
        if (!user.lines.isEmpty()) {
            error = {"ssh_host_key_known",
                     "Your own known_hosts already has a key for " + token + ", and put.gd uses it. If the server's key changed, remove the old one with ssh-keygen -R " + token + " and test again.",
                     {}};
            return {};
        }
        const auto line = probeHostKey(program, settings_, error);
        if (error) return {};
        const auto fingerprint = fingerprintOfLine(line);
        if (hostOfLine(line) != token) {
            error = {"ssh_host_alias_pattern", "ssh recorded the key under another name than " + token + "; put.gd trusts a key only for that exact name.", {}};
            return {};
        }
        if (fingerprint != expected) {
            error = {"ssh_host_key_mismatch", "The server now shows the host key " + fingerprint + ", not " + expected + ". Nothing was trusted; test again and compare the new key.",
                     {{"fingerprint", fingerprint}}};
            return {};
        }
        if (auto e = checkPinFile(knownHosts_)) { error = e; return {}; }
        if (auto e = trustLine(knownHosts_, line, settings_["hostKeyFingerprint"].toString())) { error = e; return {}; }
        connection_.close();
        return {{"fingerprint", fingerprint}, {"host", token}};
    }
    // The server identity a receipt or name check records.
    QJsonObject reference(const QString& path) const {
        auto object = connection_.server();
        object["path"] = path;
        object["location"] = location(settings_, path);
        return object;
    }
    QJsonObject checkName(Error& error) {
        QString folder;
        if (!connect(error, false) || !folderReady(error, folder)) return {};
        const auto path = join(folder, request_["candidate"].toString());
        wire::Attributes attributes;
        Client::Status status;
        if (auto e = client().lstat(bytesOf(path), attributes, status)) { error = e; return {}; }
        if (status.code == wire::NoSuchFile) return {{"status", "absent"}, {"objectReference", reference(path)}};
        if (status.code == wire::Ok) return {{"status", "present"}, {"objectReference", reference(path)}};
        error = refusal(status, path, QStringLiteral("check"));
        return {};
    }
    // Removes a file this request created and could not finish. True only when the server
    // confirmed it is gone.
    enum class Gone { Removed, Already, Unconfirmed };
    // `opened`: what FSTAT said of the new file right after OPEN, when it said it. Then, if this
    // connection can't confirm the removal within 3 seconds (on a slow link CLOSE and REMOVE queue
    // behind the writes already in ssh's buffers, #1888), ssh is ended and a new connection removes
    // the file, but only while it is still a regular file of the same owner and no larger than
    // `bound`, the upload's size. Otherwise CLOSE and REMOVE share one 8-second budget.
    Gone discard(const QByteArray& handle, const QString& path, const wire::Attributes* opened = nullptr, qint64 bound = 0) {
        if (connection_.client && !connection_.client->broken()) {
            const std::atomic_bool never{false};
            client().setCancellation(&never);
            struct Restore { Client& c; ~Restore() { c.setCancellation(&io.cancelled); } } restore{client()};
            const qint64 limit = opened ? 3000 : 8000;
            QElapsedTimer budget;
            budget.start();
            const auto left = [&] { return int(qMax<qint64>(0, limit - budget.elapsed())); };
            Client::Status status;
            if (handle.isEmpty() || !client().close(handle, status, left()))
                if (left() > 0 && !client().remove(bytesOf(path), status, left())) {
                    if (status.code == wire::Ok) return Gone::Removed;
                    if (status.code == wire::NoSuchFile) return Gone::Already;
                }
        }
        if (!opened) return Gone::Unconfirmed;
        return discardFresh(path, *opened, bound);
    }
    Gone discardFresh(const QString& path, const wire::Attributes& opened, qint64 bound) {
        // The new connection is pinned to this attempt's server: only the known_hosts lines it was
        // verified by, and the same host, port and user (connectAs, as History's checks are).
        const auto attempt = connection_.server();
        connection_.kill();
        const std::atomic_bool never{false};
        cancel_ = &never;
        struct Restore { Worker& w; ~Restore() { w.cancel_ = &io.cancelled; if (w.connection_.client) w.connection_.client->setCancellation(&io.cancelled); } } restore{*this};
        Error error;
        if (!connectAs(attempt, error)) return Gone::Unconfirmed;
        wire::Attributes there;
        Client::Status status;
        if (client().lstat(bytesOf(path), there, status)) return Gone::Unconfirmed;
        if (status.code == wire::NoSuchFile) return Gone::Already;
        if (status.code != wire::Ok || !complete(there) || !there.regularFile() || there.uid != opened.uid || qint64(there.size) > bound) return Gone::Unconfirmed;
        if (client().remove(bytesOf(path), status, 8000)) return Gone::Unconfirmed;
        return status.code == wire::Ok ? Gone::Removed : status.code == wire::NoSuchFile ? Gone::Already : Gone::Unconfirmed;
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
        QString folder;
        if (!connect(error, false) || !folderReady(error, folder)) return {};
        // An upload is recorded with the known_hosts lines that verified its server; without them, nothing is sent.
        if (connection_.knownError) { error = connection_.knownError; return {}; }
        const auto path = join(folder, candidate);
        // The path is journaled before the open, so a lost reply leaves its name on record.
        if (auto e = io.checkpoint(id_, {{"schemaVersion", 1}, {"phase", "opening"}, {"path", path}, {"server", connection_.server()}})) { error = e; return {}; }
        QByteArray handle;
        Client::Status status;
        wire::Attributes opened;
        bool openedKnown = false;
        {
            // The final name, created only if nothing has it: never a temporary name renamed over it,
            // which several servers would let replace another file. Once OPEN is sent its reply is
            // awaited whatever a cancel says, so a handle is never lost: for up to 20 seconds, then the
            // cleanup (discard), inside the 45 the resident gives a cancelled request.
            if (io.cancelled) { error = {"cancelled", "Cancelled before anything was sent", {}}; return {}; }
            const std::atomic_bool never{false};
            client().setCancellation(&never);
            const auto failed = client().openExclusive(bytesOf(path), handle, status, 20000);
            // The new file's owner, which a cleanup over a new connection checks (#1888).
            // Awaited for at most 5 seconds, so a cancel's cleanup still starts inside the resident's
            // grace. No answer: the connection is unusable and the owner unknown, so nothing can
            // be removed safely and the outcome is unknown, with the path.
            Error unanswered;
            if (!failed && status.code == wire::Ok) {
                Client::Status seen;
                unanswered = client().fstat(handle, opened, seen, 5000);
                openedKnown = !unanswered && seen.code == wire::Ok && complete(opened) && opened.regularFile();
            }
            client().setCancellation(&io.cancelled);
            if (failed) { error = failed; error.details["path"] = path; uncertain = true; return {}; }
            if (unanswered) {
                error = {unanswered.code, "put.gd created " + path + " but the server stopped answering; check it on the server.", {{"path", path}}};
                uncertain = true;
                return {};
            }
        }
        if (status.code != wire::Ok) {
            wire::Attributes there;
            Client::Status seen;
            // Somebody else's file: a collision, never reconciled into ownership.
            if (!client().lstat(bytesOf(path), there, seen) && seen.code == wire::Ok) return {{"status", "collision"}};
            error = refusal(status, path, QStringLiteral("create"), artifact.bytes);
            return {};
        }
        // From here on the file exists and is ours: any failure removes it, and the message says
        // it was removed only when the server confirmed that.
        const auto abandon = [&](Error e) {
            const auto gone = discard(handle, path, openedKnown ? &opened : nullptr, artifact.bytes);
            const bool removed = gone != Gone::Unconfirmed;
            handle.clear();
            uncertain = !removed && e.code != QLatin1String("artifact_changed");
            if (removed) {
                // The server's own words: removed now, or already gone when REMOVE came.
                const auto what = gone == Gone::Removed ? QStringLiteral("the partial file was removed from the server") : QStringLiteral("the partial file was already gone from the server");
                if (e.code == QLatin1String("cancelled")) e.message = "Cancelled; " + what;
                else if (e.code == QLatin1String("artifact_changed")) e.message = "The file changed while it was uploading; " + what;
            } else {
                e.details["path"] = path;
                if (e.code == QLatin1String("cancelled")) e.message = "Cancelled; put.gd couldn't confirm the partial file " + path + " was removed. Check it on the server.";
                else if (e.code == QLatin1String("artifact_changed")) e.message = "The file changed while it was uploading; put.gd couldn't confirm the partial file " + path + " was removed. Check it on the server.";
                else e.message += " put.gd couldn't confirm the partial file " + path + " was removed.";
            }
            error = e;
            return QJsonObject{};
        };
        if (io.cancelled) return abandon({"cancelled", "Cancelled", {}});
        QCryptographicHash hash(QCryptographicHash::Sha256);
        QElapsedTimer last;
        last.start();
        const auto sent = [&](qint64 done) {
            if (last.elapsed() < 250 && done < artifact.bytes) return;
            last.restart();
            io.emitLine({{"id", double(id_)}, {"event", "progress"}, {"phase", "uploading"}, {"bytesTransferred", double(done)}, {"bytesTotal", double(artifact.bytes)}});
        };
        if (auto e = client().write(handle, source, artifact.bytes, hash, sent, status)) return abandon(e);
        if (status.code != wire::Ok) return abandon(refusal(status, path, QStringLiteral("write"), artifact.bytes));
        const auto sha = QString::fromLatin1(hash.result().toHex());
        if (!artifact.sha256.isEmpty() && sha != artifact.sha256) return abandon({"artifact_changed", "The file changed while it was uploading", {}});
        io.emitLine({{"id", double(id_)}, {"event", "progress"}, {"phase", "finalizing"}, {"bytesTransferred", double(artifact.bytes)}, {"bytesTotal", double(artifact.bytes)}});
        // Every byte is on the server: a cancel from here on is too late, and the upload finishes.
        const std::atomic_bool finishing{false};
        client().setCancellation(&finishing);
        // Restored through the client there is at the end: a cleanup over a new connection replaces it.
        struct Restore { Connection& c; ~Restore() { if (c.client) c.client->setCancellation(&io.cancelled); } } restore{connection_};
        if (client().has("fsync@openssh.com", "1")) {
            if (auto e = client().fsync(handle, status)) { error = e; error.details["path"] = path; uncertain = true; return {}; }
            if (status.code != wire::Ok) return abandon(refusal(status, path, QStringLiteral("save")));
        }
        // What this attempt wrote, from its own handle: the evidence a lost reply is reconciled by.
        wire::Attributes written;
        if (auto e = client().fstat(handle, written, status)) { error = e; error.details["path"] = path; uncertain = true; return {}; }
        if (status.code != wire::Ok || !complete(written) || qint64(written.size) != artifact.bytes)
            return abandon({"sftp_failed", "The server didn't report the size, time and owner of " + path + ", which put.gd needs to delete it safely later", {{"path", path}}});
        const QJsonObject evidence{{"size", double(written.size)}, {"mtime", double(written.mtime)}, {"uid", double(written.uid)}, {"sha256", sha}};
        if (auto e = io.checkpoint(id_, {{"schemaVersion", 1}, {"phase", "written"}, {"path", path}, {"server", connection_.server()}, {"written", evidence}}))
            return abandon(e);
        if (auto e = client().close(handle, status)) { error = e; error.details["path"] = path; uncertain = true; return {}; }
        handle.clear();
        if (status.code != wire::Ok) return abandon(refusal(status, path, QStringLiteral("save"), artifact.bytes));
        wire::Attributes stored;
        if (auto e = client().lstat(bytesOf(path), stored, status)) { error = e; error.details["path"] = path; uncertain = true; return {}; }
        // The file under the name is the one this handle wrote, or nothing is recorded.
        if (status.code != wire::Ok || !sameFile(written, stored)) {
            error = {"sftp_failed", "The file at " + path + " changed as it was saved, so put.gd didn't record the upload", {{"path", path}}};
            uncertain = true;
            return {};
        }
        StorageReceipt receipt;
        receipt.destination = destination_;
        receipt.objectReference = reference(path);
        for (auto it = evidence.begin(); it != evidence.end(); ++it) receipt.objectReference[it.key()] = it.value();
        receipt.concurrencyToken = sha;
        receipt.publicationId = request_["publicationId"].toString();
        receipt.directUrl = linkFor(settings_, candidate);
        return {{"status", "uploaded"}, {"receipt", receipt.toJson()}};
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
    // A connection to the server a record names: ssh checks the server against the known_hosts
    // lines recorded with it, and nothing else, so it proves the server still holds that key; the
    // destination must still resolve to the same host, port and user. The session's connection
    // serves when it was verified by exactly those lines (a test checking its own upload).
    bool connectAs(const QJsonObject& recorded, Error& error) {
        QStringList lines;
        for (const auto& value : recorded["knownHosts"].toArray()) lines.append(value.toString());
        if (lines.isEmpty()) { error = {"sftp_delete_unavailable", "This upload's record doesn't name the host key its server was verified by", {}}; return false; }
        const bool current = connection_.alive() && connection_.knownLines == lines && !connection_.knownError;
        if (!current && !connect(error, false, lines)) return false;
        const auto now = connection_.server();
        if (recorded["host"] != now["host"] || recorded["port"] != now["port"] || recorded["user"] != now["user"]) {
            error = {"sftp_server_changed",
                     "This file went to " + recorded["host"].toString() + " port " + QString::number(recorded["port"].toInt()) + " as " + recorded["user"].toString() +
                         ". The destination now reaches another server or user, so put.gd won't check or delete it there.",
                     {}};
            return false;
        }
        return true;
    }
    // What the receipt needs for its identity check; without it, deletion is unavailable.
    static bool recordable(const QJsonObject& object) {
        for (const auto* key : {"path", "host", "user", "sha256"}) if (object[key].toString().isEmpty()) return false;
        for (const auto* key : {"port", "size", "mtime", "uid"}) if (!object[key].isDouble()) return false;
        return !object["knownHosts"].toArray().isEmpty();
    }
    // Whether the recorded file is still the one put.gd stored: a regular file of the same size,
    // change time and owner, and, up to 32 MB, the same bytes. `seen` is what lstat found.
    QJsonObject identity(const StorageReceipt& receipt, Error& error, wire::Attributes* seen = nullptr) {
        const auto object = receipt.objectReference;
        if (!recordable(object)) { error = {"sftp_delete_unavailable", "This upload's record lacks what put.gd needs to check the file on the server", {}}; return {}; }
        if (!connectAs(object, error)) return {};
        const auto path = object["path"].toString();
        wire::Attributes a;
        Client::Status status;
        if (auto e = client().lstat(bytesOf(path), a, status)) { error = e; return {}; }
        if (status.code == wire::NoSuchFile) return {{"status", "missing"}, {"receipt", receipt.toJson()}};
        if (status.code != wire::Ok) { error = refusal(status, path, QStringLiteral("check")); return {}; }
        if (seen) *seen = a;
        const bool same = complete(a) && a.regularFile() && qint64(a.size) == qint64(object["size"].toDouble()) && qint64(a.mtime) == qint64(object["mtime"].toDouble()) &&
                          qint64(a.uid) == qint64(object["uid"].toDouble());
        if (!same) return {{"status", "foreign"}, {"receipt", receipt.toJson()}};
        if (qint64(a.size) <= readBackBytes) {
            QString sha;
            if (!readBack(path, readBackBytes, sha, error)) { if (!error) error = {"sftp_failed", "The server didn't let put.gd read " + path + " back to check it", {{"path", path}}}; return {}; }
            if (sha != object["sha256"].toString()) return {{"status", "foreign"}, {"receipt", receipt.toJson()}};
        }
        return {{"status", "own"}, {"receipt", receipt.toJson()}};
    }
    // An upload whose outcome is unknown is this attempt's only with this attempt's evidence: the
    // size, time and owner its own handle reported, and its bytes. Anything less is unknown.
    QJsonObject reconcile(Error& error) {
        const auto artifact = Artifact::fromJson(request_["artifact"].toObject());
        const auto journal = request_["resume"].toObject();
        const auto path = journal["path"].toString();
        if (path.isEmpty()) {
            // Nothing was journaled before the open: no name of this attempt can be on the server.
            if (journal.isEmpty()) return {{"status", "missing"}};
            error = {"sftp_reconcile_unknown", "put.gd can't tell where this upload went", {}};
            return {{"status", "unknown"}};
        }
        if (!connectAs(journal["server"].toObject(), error)) return {};
        wire::Attributes a;
        Client::Status status;
        if (auto e = client().lstat(bytesOf(path), a, status)) { error = e; return {}; }
        if (status.code == wire::NoSuchFile) return {{"status", "missing"}};
        if (status.code != wire::Ok) { error = refusal(status, path, QStringLiteral("check")); return {}; }
        const auto written = journal["written"].toObject();
        const auto unknown = [&] {
            error = {"sftp_reconcile_unknown", "put.gd can't tell whether " + path + " is this upload; check it on the server", {{"path", path}}};
            return QJsonObject{{"status", "unknown"}};
        };
        if (written.isEmpty() || qint64(written["size"].toDouble()) != artifact.bytes || written["sha256"].toString() != artifact.sha256) return unknown();
        if (!complete(a) || !a.regularFile() || qint64(a.size) != qint64(written["size"].toDouble()) || qint64(a.mtime) != qint64(written["mtime"].toDouble()) ||
            qint64(a.uid) != qint64(written["uid"].toDouble()))
            return unknown();
        if (artifact.bytes > reconcileReadBytes) return unknown();
        QString sha;
        if (!readBack(path, reconcileReadBytes, sha, error)) { if (!error) return unknown(); return {}; }
        if (sha != written["sha256"].toString()) return unknown();
        StorageReceipt receipt;
        receipt.destination = destination_;
        receipt.objectReference = reference(path);
        for (auto it = written.begin(); it != written.end(); ++it) receipt.objectReference[it.key()] = it.value();
        receipt.concurrencyToken = sha;
        receipt.publicationId = request_["publicationId"].toString();
        receipt.directUrl = linkFor(settings_, path.section('/', -1));
        return {{"status", "own"}, {"receipt", receipt.toJson()}};
    }
    QJsonObject remove(Error& error, bool& uncertain) {
        const auto receipt = StorageReceipt::fromJson(request_["receipt"].toObject());
        const auto path = receipt.objectReference["path"].toString();
        // The intent is saved before the removal; it stays on record if the outcome is unclear.
        // The identity check comes after it, so waiting for the journal never makes it stale.
        const QJsonObject intent{{"schemaVersion", 1}, {"phase", "remove_intent"}, {"path", path}};
        if (auto e = io.checkpoint(id_, intent)) { error = e; return {{"status", "failed"}}; }
        wire::Attributes checked;
        const auto found = identity(receipt, error, &checked);
        if (error) return {{"status", "failed"}};
        if (found["status"] == "missing") return {{"status", "missing"}};
        if (found["status"] != "own") return {{"status", "changed"}};
        // Checked again right before REMOVE: the read-back above took time. SFTP has no
        // conditional remove, so another program could still replace the file in the one round
        // trip between this check and the removal.
        wire::Attributes again;
        Client::Status status;
        if (auto e = client().lstat(bytesOf(path), again, status)) { error = e; uncertain = true; return {{"status", "unknown"}, {"progress", intent}}; }
        if (status.code == wire::NoSuchFile) return {{"status", "missing"}};
        if (status.code != wire::Ok || !sameFile(checked, again)) return {{"status", "changed"}};
        if (auto e = client().remove(bytesOf(path), status)) { error = e; uncertain = true; return {{"status", "unknown"}, {"progress", intent}}; }
        if (status.code == wire::Ok) return {{"status", "removed"}};
        if (status.code == wire::NoSuchFile) return {{"status", "missing"}};
        error = refusal(status, path, QStringLiteral("delete"));
        return {{"status", "failed"}};
    }
    QJsonObject createFolder(Error& error) {
        if (!connect(error, false)) return {};
        // A folder named relative to the login folder needs that folder's path: no "." stands in.
        if (!remoteFolder(settings_["folder"].toString()).startsWith('/') && !connection_.homeValid) {
            error = {"sftp_path_unsupported", "The server names your login folder with bytes that aren't UTF-8, or not at all, so put.gd can't tell where to create the folder. Give Folder on server as a full path.", {}};
            return {};
        }
        const auto folder = namedFolder();
        // Only the folder a test found missing: the path it reported must be the one made now.
        if (request_["path"].toString() != folder) {
            error = {"sftp_settings_invalid", "The folder to create isn't the one the test found missing. Test the destination again.", {{"field", "folder"}}};
            return {};
        }
        // Each missing level, from the top: mkdir -p, with the server's umask deciding modes.
        QString path;
        for (const auto& part : folder.split('/', Qt::SkipEmptyParts)) {
            path = (path.isEmpty() && folder.startsWith('/') ? QStringLiteral("/") : path.isEmpty() ? QString() : path + "/") + part;
            if (path.startsWith(QLatin1String("//"))) path.remove(0, 1);
            wire::Attributes a;
            Client::Status status;
            if (auto e = client().lstat(bytesOf(path), a, status, true)) { error = e; return {}; }
            if (status.code == wire::Ok) {
                if (!a.directory()) { error = {"sftp_not_folder", path + " on the server is not a folder", {{"path", path}}}; return {}; }
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
            // The chosen key file (#1892) ssh can't use: found before anything connects.
            if (error.details.value("field").toString() == QLatin1String("keyFile")) { step("signIn", "failed", error.message); return out; }
            const bool key = error.code.startsWith(QLatin1String("ssh_host")) || error.code == QLatin1String("ssh_trusted_key_changed");
            const bool reached = key || error.code == QLatin1String("ssh_auth_failed") || error.code == QLatin1String("ssh_key_permissions") ||
                                 error.code == QLatin1String("ssh_needs_one_time_code") || error.code == QLatin1String("ssh_no_sftp");
            step("connect", reached ? "passed" : "failed", reached ? QStringLiteral("Reached the server") : error.message);
            if (reached) step("hostKey", key ? "failed" : "passed", key ? error.message : QStringLiteral("The host key matches"));
            if (reached && !key) step("signIn", error.code == QLatin1String("ssh_no_sftp") ? "passed" : "failed", error.code == QLatin1String("ssh_no_sftp") ? QStringLiteral("Signed in") : error.message);
            if (error.code == QLatin1String("ssh_no_sftp")) step("sftp", "failed", error.message);
            return out;
        }
        const auto& r = connection_.resolved;
        step("connect", "passed", "Reached " + (r.host.contains(':') ? "[" + r.host + "]" : r.host) + (r.port != 22 ? ":" + QString::number(r.port) : QString()));
        step("hostKey", "passed", connection_.fingerprint().isEmpty() ? QStringLiteral("The host key matches a trusted one") : "The host key " + connection_.fingerprint() + " matches a trusted one");
        step("signIn", "passed", "Signed in as " + r.user);
        QString folder;
        if (!folderReady(error, folder)) {
            step("folder", "failed", error.message);
            if (error.code == QLatin1String("sftp_folder_missing")) out["createFolder"] = namedFolder();
            return out;
        }
        step("folder", "passed", folder + " is a folder");
        // As an upload: without the known_hosts entry that verified this server, nothing is created.
        if (connection_.knownError) { error = connection_.knownError; step("upload", "failed", error.message); return out; }
        // A synthetic file under a name nothing else uses, recorded before it is sent (#1690).
        QByteArray bytes = "put.gd SFTP destination test " + QUuid::createUuid().toByteArray(QUuid::WithoutBraces) + "\n";
        for (int i = 0; i < 32; ++i) bytes += QByteArray::number(QRandomGenerator::global()->generate64(), 36) + "\n";
        const auto name = "putgd-test-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".txt";
        const auto path = join(folder, name);
        if (auto e = io.checkpoint(id_, {{"schemaVersion", 1}, {"phase", "upload_intent"}, {"destination", destination_.toJson()}, {"path", path}})) { error = e; return out; }
        QByteArray handle;
        Client::Status status;
        if (auto e = client().openExclusive(bytesOf(path), handle, status)) {
            // The reply was lost: the test file may be there, and nothing confirms otherwise.
            error = e;
            error.details["path"] = path;
            out["cleanupStatus"] = "unconfirmed";
            out["cleanupPath"] = path;
            step("upload", "failed", e.message);
            return out;
        }
        if (status.code != wire::Ok) { error = refusal(status, path, QStringLiteral("create"), bytes.size()); step("upload", "failed", error.message); return out; }
        QByteArray copy = bytes;
        QBuffer buffer(&copy);
        buffer.open(QIODevice::ReadOnly);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        const auto fail = [&](Error e) {
            error = e;
            const auto gone = discard(handle, path);
            out["cleanupStatus"] = gone == Gone::Removed ? "removed" : gone == Gone::Already ? "missing" : "unconfirmed";
            if (gone == Gone::Unconfirmed) { out["cleanupPath"] = path; error.details["path"] = path; }
            step("upload", "failed", e.message);
            return out;
        };
        // The name holds another file now: nothing is removed, and the test file's fate is open.
        const auto replaced = [&](Error e) {
            error = e;
            error.details["path"] = path;
            out["cleanupStatus"] = "unconfirmed";
            out["cleanupPath"] = path;
            step("upload", "failed", e.message);
            return out;
        };
        if (auto e = client().write(handle, buffer, bytes.size(), hash, {}, status)) return fail(e);
        if (status.code != wire::Ok) return fail(refusal(status, path, QStringLiteral("write"), bytes.size()));
        if (client().has("fsync@openssh.com", "1")) {
            if (auto e = client().fsync(handle, status)) return fail(e);
            if (status.code != wire::Ok) return fail(refusal(status, path, QStringLiteral("save")));
        }
        wire::Attributes written;
        if (auto e = client().fstat(handle, written, status)) return fail(e);
        if (status.code != wire::Ok || !complete(written))
            return fail({"sftp_failed", "The server didn't report the size, time and owner of the test file, which put.gd needs to delete uploads safely", {}});
        if (auto e = client().close(handle, status)) { handle.clear(); return fail(e); }
        handle.clear();
        if (status.code != wire::Ok) return fail(refusal(status, path, QStringLiteral("save")));
        wire::Attributes stored;
        if (auto e = client().lstat(bytesOf(path), stored, status)) return fail(e);
        if (status.code != wire::Ok || !sameFile(written, stored))
            return replaced({"sftp_failed", "The test file at " + path + " changed as it was saved, so put.gd left it alone. Check it on the server.", {}});
        StorageReceipt receipt;
        receipt.destination = destination_;
        receipt.objectReference = reference(path);
        receipt.objectReference["size"] = double(written.size);
        receipt.objectReference["mtime"] = double(written.mtime);
        receipt.objectReference["uid"] = double(written.uid);
        receipt.objectReference["sha256"] = QString::fromLatin1(hash.result().toHex());
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
        // Cleanup: this request created the file; it is removed after the same identity check
        // History uses, and counts as removed only when the server confirms it.
        Error checkError;
        const auto checked = identity(receipt, checkError);
        // A name that holds another file now gets no REMOVE: its cleanup stays unconfirmed.
        if (checkError || checked["status"] != "own" && checked["status"] != "missing") out["cleanupStatus"] = "unconfirmed";
        else if (checked["status"] == "missing") out["cleanupStatus"] = "missing";
        else if (client().remove(bytesOf(path), status)) out["cleanupStatus"] = "unconfirmed";
        else out["cleanupStatus"] = status.code == wire::Ok ? "removed" : status.code == wire::NoSuchFile ? "missing" : "unconfirmed";
        const bool cleaned = out["cleanupStatus"] == "removed" || out["cleanupStatus"] == "missing";
        if (!cleaned) out["cleanupPath"] = path;
        step("cleanup", cleaned ? "passed" : "failed",
             out["cleanupStatus"] == "removed" ? QStringLiteral("Deleted the test file")
             : out["cleanupStatus"] == "missing" ? QStringLiteral("The test file was already gone when put.gd went to delete it")
                                                 : "Couldn't confirm the test file " + path + " was deleted; check it on the server");
        if (!error && !cleaned) error = {"destination_test_incomplete", "The test file " + path + " may still be on the server; delete it there", {{"path", path}}};
        if (!error && linkError) error = linkError;
        // The summary speaks for a test that passed every step only.
        if (!error)
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
    // A write to an ssh that has just exited (or to a resident that is gone) fails with EPIPE and
    // is handled as a lost connection; it must not end the worker. ssh itself starts with SIGPIPE
    // back at its default (SshProcess::start).
    std::signal(SIGPIPE, SIG_IGN);
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(workerName);
    putgd::useHostCaBundle();
    std::thread reader([] { io.readLoop(); });
    reader.detach();
    Worker worker;
    while (true) {
        QJsonObject request;
        qint64 id = 0;
        {
            std::unique_lock lock(io.mutex);
            if (!io.changed.wait_for(lock, std::chrono::milliseconds(idleExitMs), [] { return !io.requests.empty() || io.gone; })) break;
            if (io.requests.empty()) break;  // stdin closed
            request = io.requests.front();
            io.requests.pop_front();
            id = qint64(request["id"].toDouble());
            io.current = id;
            // A cancel that arrived while it was queued holds now.
            io.cancelled = io.cancelledIds.erase(id) > 0;
        }
        putgd::Error error;
        bool uncertain = false;
        if (request["schemaVersion"].toInt() != 1) error = {"sftp_protocol", "Invalid worker request", {}};
        // The secrets leave the request at once: only Worker::run holds them, until sign-in.
        auto secrets = request.take("credentials").toObject();
        const auto result = error ? QJsonObject{} : worker.run(request, secrets, error, uncertain);
        secrets = {};
        request = {};
        if (error)
            io.emitLine({{"id", double(id)}, {"ok", false}, {"error", QJsonObject{{"code", error.code}, {"message", error.message}, {"details", error.details}}}, {"uncertain", uncertain}, {"result", result}});
        else io.emitLine({{"id", double(id)}, {"ok", true}, {"result", result}});
        io.current = 0;
    }
    worker.closeConnection();
    std::_Exit(0);
}
