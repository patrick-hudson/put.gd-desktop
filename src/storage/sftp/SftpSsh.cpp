#include "SftpSsh.h"
#include "SftpCommon.h"
#include "core/HostEnvironment.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
namespace putgd::sftp {
namespace {
QString expandHome(const QString& path, const QProcessEnvironment& env) {
    if (path == QLatin1String("~")) return env.value("HOME", QDir::homePath());
    if (path.startsWith(QLatin1String("~/"))) return env.value("HOME", QDir::homePath()) + path.mid(1);
    return path;
}
bool isSocket(const QString& path) {
    struct stat info {};
    return !path.isEmpty() && ::stat(QFile::encodeName(path).constData(), &info) == 0 && S_ISSOCK(info.st_mode);
}
QString quoted(const QString& path) { return "\"" + path + "\""; }
// What every connection and the probe share: no forwarding, no terminal, no local commands,
// no shared master connection, and host keys only from known_hosts files: never DNS, never
// skipped for localhost, no second check by IP address, no keys learned from the server.
QStringList baseOptions() {
    return {"-T", "-x", "-a", "-o", "ClearAllForwardings=yes", "-o", "RequestTTY=no", "-o", "RemoteCommand=none", "-o", "PermitLocalCommand=no",
            "-o", "EscapeChar=none", "-o", "ControlMaster=no", "-o", "ControlPath=none", "-o", "UpdateHostKeys=no",
            "-o", "NoHostAuthenticationForLocalhost=no", "-o", "VerifyHostKeyDNS=no", "-o", "CheckHostIP=no",
            // A server's own prompts (one-time codes, keyboard-interactive passwords) never reach askpass.
            "-o", "KbdInteractiveAuthentication=no"};
}
// The parent process of `pid`, from /proc/<pid>/stat (the field after the command's closing parenthesis).
pid_t parentOf(pid_t pid) {
    QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!stat.open(QIODevice::ReadOnly)) return -1;
    const auto text = stat.read(4096);
    const auto close = text.lastIndexOf(')');
    if (close < 0) return -1;
    const auto fields = text.mid(close + 2).split(' ');
    return fields.size() > 1 ? pid_t(fields[1].toLongLong()) : -1;
}
// ssh's stderr without its debug lines: what classify() reads.
QString withoutDebug(const QByteArray& raw) {
    QStringList kept;
    for (const auto& line : QString::fromUtf8(raw).split('\n'))
        if (!line.startsWith(QLatin1String("debug"))) kept.append(line);
    return kept.join('\n');
}
}
QProcessEnvironment sshEnvironment() {
    auto env = host::environment();
    for (const auto& key : env.keys())
        if (key.startsWith(QLatin1String("PUTGD_")) || key == QLatin1String("SSH_ASKPASS") || key == QLatin1String("SSH_ASKPASS_REQUIRE")) env.remove(key);
    env.insert("LC_ALL", "C");
    return env;
}
QString sshProgram(const QProcessEnvironment& env) { return host::findProgram("ssh", env); }
Error sshMissing() {
    return {"ssh_not_installed",
            "put.gd uploads over SFTP with your OpenSSH client, and ssh isn't installed. Install it (openssh-client on Debian, Ubuntu and Mint, "
            "openssh-clients on Fedora, openssh on Arch), then test again.",
            {}};
}
QString agentSocket(const QProcessEnvironment& env) {
    const auto set = env.value("SSH_AUTH_SOCK");
    if (isSocket(set)) return set;
    const auto runtime = env.value("XDG_RUNTIME_DIR");
    if (runtime.isEmpty()) return {};
    for (const auto* name : {"gcr/ssh", "keyring/ssh", "ssh-agent.socket", "openssh_agent", "gnupg/S.gpg-agent.ssh"}) {
        const auto path = runtime + "/" + QLatin1String(name);
        if (isSocket(path)) return path;
    }
    return {};
}
QString Resolved::token() const { return hostToken(host, port, hostKeyAlias); }
QStringList destinationArguments(const QJsonObject& settings) {
    const auto address = parseServer(settings["server"].toString());
    QStringList args;
    if (!address) return args;
    if (address->port > 0) args << "-p" << QString::number(address->port);
    if (!settings["user"].toString().isEmpty()) args << "-l" << settings["user"].toString();
    args << "--" << address->host;
    return args;
}
QStringList identityArguments(const QJsonObject& settings) {
    const auto file = keyFile(settings);
    if (file.isEmpty()) return {};
    return {"-i", file, "-o", "IdentitiesOnly=yes"};
}
Resolved resolve(const QString& ssh, const QProcessEnvironment& env, const QJsonObject& settings, Error& error) {
    Resolved resolved;
    resolved.chosenKey = keyFile(settings);
    QProcess process;
    process.setProcessEnvironment(env);
    process.setStandardInputFile(QProcess::nullDevice());
    // With the chosen key, as a connection runs: ssh -G then lists it first among the key files,
    // and its passphrase prompt is one the askpass answers.
    process.start(ssh, QStringList{"-G"} + identityArguments(settings) + destinationArguments(settings));
    if (!process.waitForStarted(10000)) { error = sshMissing(); return resolved; }
    if (!process.waitForFinished(15000)) {
        process.kill();
        process.waitForFinished(2000);
        error = {"ssh_failed", "ssh timed out while reading your SSH settings. Try again.", {}};
        return resolved;
    }
    const auto address = parseServer(settings["server"].toString());
    resolved.alias = address ? address->host : QString();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        error = {"ssh_config_invalid", "ssh couldn't read your SSH settings for this server. Run ssh -G " + resolved.alias + " in a terminal to see what it says.", {}};
        return resolved;
    }
    QString agentSetting;
    for (const auto& raw : QString::fromUtf8(process.readAllStandardOutput()).split('\n')) {
        const auto line = raw.trimmed();
        const auto space = line.indexOf(' ');
        if (space <= 0) continue;
        const auto key = line.left(space).toLower(), value = line.mid(space + 1).trimmed();
        if (key == QLatin1String("hostname")) resolved.host = value;
        else if (key == QLatin1String("user")) resolved.user = value;
        else if (key == QLatin1String("port")) resolved.port = value.toInt();
        else if (key == QLatin1String("hostkeyalias") && value != QLatin1String("none")) resolved.hostKeyAlias = value;
        else if (key == QLatin1String("identityfile")) {
            const auto path = expandHome(value, env);
            if (QFileInfo(path).isFile() && !resolved.identityFiles.contains(path)) resolved.identityFiles.append(path);
        } else if (key == QLatin1String("userknownhostsfile")) {
            // Paths are split on spaces, as ssh -G prints them; a path with a space in it isn't supported.
            if (value.compare(QLatin1String("none"), Qt::CaseInsensitive) == 0) resolved.userKnownHostsNone = true;
            else for (const auto& file : value.split(' ', Qt::SkipEmptyParts)) resolved.userKnownHosts.append(expandHome(file, env));
        } else if (key == QLatin1String("globalknownhostsfile")) {
            for (const auto& file : value.split(' ', Qt::SkipEmptyParts)) if (file != QLatin1String("none")) resolved.globalKnownHosts.append(expandHome(file, env));
        } else if (key == QLatin1String("identityagent")) agentSetting = value;
        else if (key == QLatin1String("proxyjump") && value != QLatin1String("none")) resolved.proxyKind = QStringLiteral("ProxyJump");
        else if (key == QLatin1String("proxycommand") && value != QLatin1String("none") && resolved.proxyKind.isEmpty()) resolved.proxyKind = QStringLiteral("ProxyCommand");
    }
    if (agentSetting.compare(QLatin1String("none"), Qt::CaseInsensitive) == 0) resolved.agent = false;
    else if (!agentSetting.isEmpty() && agentSetting != QLatin1String("SSH_AUTH_SOCK")) {
        resolved.identityAgent = expandHome(agentSetting, env);
        resolved.agent = isSocket(resolved.identityAgent);
    } else resolved.agent = !agentSocket(env).isEmpty();
    if (resolved.userKnownHosts.isEmpty() && !resolved.userKnownHostsNone)
        resolved.userKnownHosts = {expandHome("~/.ssh/known_hosts", env), expandHome("~/.ssh/known_hosts2", env)};
    // A path still holding a %-token, a ${VARIABLE} or a ~user can't be read the way ssh reads it:
    // put.gd then doesn't know what the user trusts, and fails closed.
    for (const auto& file : resolved.userKnownHosts + resolved.globalKnownHosts)
        if (file.contains('%') || file.contains(QLatin1String("${")) || file.startsWith('~')) resolved.knownHostsUnresolved = true;
    return resolved;
}
QString resolvedLine(const Resolved& r, bool password) {
    auto line = "Connects to " + (r.host.contains(':') ? "[" + r.host + "]" : r.host) + (r.port != 22 ? ":" + QString::number(r.port) : QString()) + " as " + r.user;
    if (!r.chosenKey.isEmpty()) line += " with key " + QFileInfo(r.chosenKey).fileName();
    else if (!r.identityFiles.isEmpty()) line += " with key " + QFileInfo(r.identityFiles.first()).fileName() + (r.agent ? QStringLiteral(" (agent)") : QString());
    else if (r.agent) line += QStringLiteral(" with the agent's keys");
    else if (password) line += QStringLiteral(" with a password");
    else line += QStringLiteral(" with no key: no SSH agent or key file was found");
    if (r.proxyKind == QLatin1String("ProxyJump")) line += QStringLiteral(" through a jump host");
    else if (!r.proxyKind.isEmpty()) line += QStringLiteral(" through a proxy command");
    return line;
}
QJsonObject toJson(const Resolved& r) {
    QJsonArray keys;
    for (const auto& file : r.identityFiles) keys.append(QFileInfo(file).fileName());
    return {{"host", r.host}, {"port", r.port}, {"user", r.user}, {"alias", r.alias}, {"keyFiles", keys}, {"agent", r.agent}, {"proxy", r.proxyKind},
            {"chosenKey", r.chosenKey.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(QFileInfo(r.chosenKey).fileName())}};
}
KnownLookup lookupHost(const QStringList& files, const QString& token, const QProcessEnvironment& env) {
    KnownLookup found;
    const auto keygen = host::findProgram("ssh-keygen", env);
    if (keygen.isEmpty()) { found.error = {"ssh_known_hosts_unreadable", "Can't check known_hosts files because ssh-keygen isn't installed.", {}}; return found; }
    for (const auto& file : files) {
        struct stat info {};
        if (::stat(QFile::encodeName(file).constData(), &info) != 0) {
            if (errno == ENOENT) continue;  // a file that isn't there holds no key
            found.error = {"ssh_known_hosts_unreadable", "put.gd can't read the known_hosts file " + file + ".", {}};
            return found;
        }
        // Only a readable regular file can say "no entry"; anything else decides nothing.
        if (!S_ISREG(info.st_mode) || ::access(QFile::encodeName(file).constData(), R_OK) != 0) {
            found.error = {"ssh_known_hosts_unreadable", "Can't read the known_hosts file " + file + " because it isn't a readable regular file.", {}};
            return found;
        }
        QProcess lookup;
        lookup.setProcessEnvironment(env);
        lookup.setStandardInputFile(QProcess::nullDevice());
        lookup.start(keygen, {"-F", token, "-f", file});
        const bool ran = lookup.waitForFinished(10000) && lookup.exitStatus() == QProcess::NormalExit;
        // 0: found, 1: not found; anything else is a lookup put.gd can't rely on.
        if (!ran || (lookup.exitCode() != 0 && lookup.exitCode() != 1)) {
            if (lookup.state() != QProcess::NotRunning) { lookup.kill(); lookup.waitForFinished(2000); }
            found.error = {"ssh_known_hosts_unreadable", "put.gd couldn't check the known_hosts file " + file + " with ssh-keygen.", {}};
            return found;
        }
        if (lookup.exitCode() != 0) continue;
        for (const auto& raw : QString::fromUtf8(lookup.readAllStandardOutput()).split('\n')) {
            const auto line = raw.trimmed();
            if (line.isEmpty() || line.startsWith('#')) continue;
            found.lines.append(line);
            if (found.file.isEmpty()) found.file = file;
        }
    }
    return found;
}
KnownLookup userKnownLookup(const Resolved& r, const QProcessEnvironment& env) {
    if (r.knownHostsUnresolved) return {{}, {}, {"ssh_known_hosts_unreadable", "Your SSH settings name a known_hosts file with a token put.gd can't expand, so it can't tell which keys you trust.", {}}};
    return lookupHost(r.userKnownHosts + r.globalKnownHosts, r.token(), env);
}
Error checkPinFile(const QString& file) {
    const auto refuse = [&](const QString& why) { return Error{"sftp_known_hosts_failed", "put.gd won't use its known_hosts file " + file + ": " + why, {}}; };
    struct stat info {};
    const auto folder = QFile::encodeName(QFileInfo(file).absolutePath());
    if (::lstat(folder.constData(), &info) == 0 && (!S_ISDIR(info.st_mode) || info.st_uid != ::getuid())) return refuse(QStringLiteral("its folder isn't a private folder of yours"));
    if (::lstat(QFile::encodeName(file).constData(), &info) != 0) return errno == ENOENT ? Error{} : refuse(QStringLiteral("it can't be checked"));
    if (!S_ISREG(info.st_mode) || info.st_uid != ::getuid() || (info.st_mode & 0777) != 0600) return refuse(QStringLiteral("it isn't a private regular file of yours"));
    return {};
}
QString knownHostsOption(const Resolved& r, const QString& putgdFile, bool userKnows) {
    if (r.userKnownHostsNone) return QStringLiteral("none");
    QStringList files;
    for (const auto& file : r.userKnownHosts) files.append(quoted(file));
    // The user's own trust decides alone once they have any key for this server.
    if (!userKnows) files.append(quoted(putgdFile));
    return files.join(' ');
}

SshProcess::~SshProcess() { kill(); }
bool SshProcess::start(const QString& program, const QStringList& arguments, const QProcessEnvironment& environment, QString* error) {
    int in[2], out[2], err[2];
    if (::pipe2(in, O_CLOEXEC) != 0) return false;
    if (::pipe2(out, O_CLOEXEC) != 0) { ::close(in[0]); ::close(in[1]); return false; }
    if (::pipe2(err, O_CLOEXEC) != 0) { ::close(in[0]); ::close(in[1]); ::close(out[0]); ::close(out[1]); return false; }
    QList<QByteArray> words{QFile::encodeName(program)}, variables;
    for (const auto& argument : arguments) words.append(argument.toUtf8());
    for (const auto& variable : environment.toStringList()) variables.append(variable.toLocal8Bit());
    std::vector<char*> argv, envp;
    for (auto& word : words) argv.push_back(word.data());
    for (auto& variable : variables) envp.push_back(variable.data());
    argv.push_back(nullptr);
    envp.push_back(nullptr);
    posix_spawn_file_actions_t files;
    posix_spawn_file_actions_init(&files);
    posix_spawn_file_actions_adddup2(&files, in[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&files, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&files, err[1], STDERR_FILENO);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    sigset_t none, defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    posix_spawnattr_setsigmask(&attributes, &none);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    // A new session: ssh has no controlling terminal, so it never prompts on one.
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSID);
    pid_t pid = -1;
    const int failed = ::posix_spawn(&pid, argv[0], &files, &attributes, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&files);
    posix_spawnattr_destroy(&attributes);
    ::close(in[0]);
    ::close(out[1]);
    ::close(err[1]);
    if (failed) {
        ::close(in[1]); ::close(out[0]); ::close(err[0]);
        if (error) *error = QString::fromLocal8Bit(std::strerror(failed));
        return false;
    }
    pid_ = pid;
    in_ = in[1];
    out_ = out[0];
    err_ = err[0];
    for (const int fd : {in_, out_, err_}) ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    return true;
}
void SshProcess::drainErrors() {
    char buffer[4096];
    while (err_ >= 0) {
        const auto count = ::read(err_, buffer, sizeof buffer);
        if (count > 0) {
            errText_.append(buffer, qsizetype(count));
            if (errText_.size() > 65536) errText_.remove(0, errText_.size() - 65536);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count == 0) { ::close(err_); err_ = -1; }
        break;
    }
}
bool SshProcess::running() {
    if (pid_ <= 0 || reaped_) return false;
    int status = 0;
    const auto ended = ::waitpid(pid_, &status, WNOHANG);
    if (ended == pid_) {
        reaped_ = true;
        exit_ = WIFEXITED(status) ? WEXITSTATUS(status) : 255;
    } else if (ended < 0 && errno != EINTR) reaped_ = true;
    return !reaped_;
}
void SshProcess::finish(int milliseconds) {
    if (in_ >= 0) { ::close(in_); in_ = -1; }
    for (int waited = 0; running() && waited < milliseconds; waited += 10) {
        drainErrors();
        ::usleep(10000);
    }
    kill();
}
void SshProcess::kill() {
    if (pid_ > 0 && !reaped_) {
        ::kill(pid_, SIGTERM);
        for (int i = 0; i < 50 && running(); ++i) ::usleep(10000);
        if (running()) ::kill(pid_, SIGKILL);
        int status = 0;
        while (!reaped_ && ::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
        reaped_ = true;
    }
    drainErrors();
    for (int* fd : {&in_, &out_, &err_}) if (*fd >= 0) { ::close(*fd); *fd = -1; }
}

Askpass::Askpass(QString password, QString passphrase, QString user, QStringList hosts, QStringList identityFiles)
    : password_(std::move(password)), passphrase_(std::move(passphrase)), user_(std::move(user)), hosts_(std::move(hosts)), identityFiles_(std::move(identityFiles)) {}
Askpass::~Askpass() { stop(); }
bool Askpass::start(QProcessEnvironment& environment, QString program, QString* error) {
    auto base = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (base.isEmpty() || !QFileInfo(base).isDir()) base = QDir::tempPath();
    auto pattern = QFile::encodeName(base + "/putgd-sftp-XXXXXX");
    if (!::mkdtemp(pattern.data())) { if (error) *error = QStringLiteral("could not create the askpass folder"); return false; }
    directory_ = QFile::decodeName(pattern);
    ::chmod(pattern.constData(), 0700);
    socket_ = directory_ + "/a";
    const auto path = QFile::encodeName(socket_);
    sockaddr_un address{};
    if (size_t(path.size()) >= sizeof address.sun_path) { if (error) *error = QStringLiteral("the askpass socket path is too long"); return false; }
    listener_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.constData(), size_t(path.size()));
    if (listener_ < 0 || ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 || ::listen(listener_, 4) != 0) {
        if (error) *error = QStringLiteral("could not open the askpass socket");
        return false;
    }
    ::chmod(path.constData(), 0600);
    QByteArray random(32, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(random.data()), 8);
    token_ = random.toHex();
    environment.insert("SSH_ASKPASS", program);
    environment.insert("SSH_ASKPASS_REQUIRE", "force");
    environment.insert(askpassSocketVariable, socket_);
    environment.insert(askpassTokenVariable, QString::fromLatin1(token_));
    thread_ = std::make_unique<std::thread>([this] { serve(); });
    return true;
}
void Askpass::stop() {
    stop_ = true;
    if (thread_ && thread_->joinable()) thread_->join();
    thread_.reset();
    if (listener_ >= 0) { ::close(listener_); listener_ = -1; }
    if (!socket_.isEmpty()) ::unlink(QFile::encodeName(socket_).constData());
    if (!directory_.isEmpty()) ::rmdir(QFile::encodeName(directory_).constData());
    socket_.clear();
    directory_.clear();
    password_.fill(QChar(0));
    password_.clear();
    passphrase_.fill(QChar(0));
    passphrase_.clear();
    token_.fill('\0');
}
QStringList Askpass::asked() const {
    std::lock_guard lock(mutex_);
    return asked_;
}
QString Askpass::passphraseKey() const {
    std::lock_guard lock(mutex_);
    return passphraseKey_;
}
void Askpass::serve() {
    int answered = 0;
    while (!stop_ && answered < 6) {
        pollfd wait{listener_, POLLIN, 0};
        if (::poll(&wait, 1, 100) <= 0) continue;
        const int client = ::accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) continue;
        ++answered;
        // Only this user's processes: the socket's folder is 0700, and the peer is checked too.
        ucred peer{};
        socklen_t size = sizeof peer;
        if (::getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 || peer.uid != ::getuid()) { ::close(client); continue; }
        QByteArray request;
        char buffer[1024];
        for (int rounds = 0; rounds < 50 && request.size() < 8192 && request.count('\n') < 3; ++rounds) {
            pollfd readable{client, POLLIN, 0};
            if (::poll(&readable, 1, 100) <= 0) continue;
            const auto count = ::read(client, buffer, sizeof buffer);
            if (count <= 0) break;
            request.append(buffer, qsizetype(count));
        }
        const auto lines = request.split('\n');
        QByteArray reply = "no\n";
        if (lines.size() >= 3 && lines[0].size() == token_.size()) {
            unsigned char diff = 0;
            for (qsizetype i = 0; i < token_.size(); ++i) diff |= uchar(lines[0][i] ^ token_[i]);
            if (diff == 0) {
                // Only ssh's own askpass child may be answered: a jump host's ssh or a ProxyCommand
                // child under it is another process with another parent.
                const bool fromTarget = sshPid_.load() > 0 && parentOf(peer.pid) == sshPid_.load();
                auto match = matchPrompt(QString::fromUtf8(lines[1]), lines[2], user_, hosts_, identityFiles_);
                if (!fromTarget && (match.kind == PromptKind::Password || match.kind == PromptKind::Passphrase)) match = {};
                QString what = QStringLiteral("other"), *secret = nullptr;
                std::atomic_bool* used = nullptr;
                switch (match.kind) {
                case PromptKind::Password: what = QStringLiteral("password"); secret = &password_; used = &passwordUsed_; break;
                case PromptKind::Passphrase: {
                    what = QStringLiteral("passphrase"); secret = &passphrase_; used = &passphraseUsed_;
                    std::lock_guard lock(mutex_);
                    passphraseKey_ = match.keyFile;
                    break;
                }
                case PromptKind::Touch: what = QStringLiteral("touch"); break;
                case PromptKind::Notice: what = QStringLiteral("notice"); break;
                case PromptKind::Ambiguous: what = QStringLiteral("ambiguous"); break;
                case PromptKind::Other: break;
                }
                {
                    std::lock_guard lock(mutex_);
                    if (!asked_.contains(what)) asked_.append(what);
                }
                if (match.kind == PromptKind::Touch && onTouch) onTouch();
                if (secret && !secret->isEmpty() && !used->exchange(true)) {
                    auto bytes = secret->toUtf8();
                    reply = "ok\n" + bytes;
                    bytes.fill('\0');
                }
            }
        }
        qsizetype sent = 0;
        while (sent < reply.size()) {
            const auto count = ::send(client, reply.constData() + sent, size_t(reply.size() - sent), MSG_NOSIGNAL);
            if (count <= 0) break;
            sent += count;
        }
        reply.fill('\0');
        ::close(client);
    }
}

QStringList connectionArguments(const QJsonObject& settings, const QString& knownHostsOption, bool pinnedOnly) {
    auto args = baseOptions() + QStringList{"-o", "StrictHostKeyChecking=yes", "-o", "NumberOfPasswordPrompts=1", "-o", "BatchMode=no",
                                            "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=4", "-o", "ConnectTimeout=20",
                                            "-o", "UserKnownHostsFile=" + knownHostsOption};
    // Checked against a receipt's recorded lines: nothing else may vouch for the server, neither the
    // global known_hosts nor a KnownHostsCommand from the user's ssh settings. (Certificate
    // authorities come only from known_hosts files, so the recorded lines are all of them.)
    if (pinnedOnly) args << "-o" << "GlobalKnownHostsFile=/dev/null" << "-o" << "KnownHostsCommand=none";
    return args + identityArguments(settings) + QStringList{"-s"} + destinationArguments(settings) + QStringList{"sftp"};
}
Error classify(const QByteArray& raw, const FailureContext& c) {
    const auto text = withoutDebug(raw);
    const auto port = c.resolved.port;
    const bool changed = text.contains(QLatin1String("REMOTE HOST IDENTIFICATION HAS CHANGED")) || text.contains(QRegularExpression(QStringLiteral("Host key for \\S+ has changed")));
    const bool unknown = text.contains(QRegularExpression(QStringLiteral("No \\S+ host key is known for"))) || text.contains(QLatin1String("Host key verification failed"));
    // Checked against the key a receipt recorded: any host-key failure means it isn't that server.
    if (c.pinned && (changed || unknown))
        return {"sftp_server_changed", "The server no longer holds the host key it had when this file was uploaded, so put.gd won't check or delete the file there.", {}};
    // ssh's own lines are not repeated: put.gd says which file it had ssh check.
    if (changed && c.userFile.isEmpty())
        return {"ssh_trusted_key_changed",
                "The server's host key changed since you trusted it in put.gd. Ask your server's admin whether it was replaced. Only select Trust new key in "
                "Destinations if they confirm it.",
                {}};
    if (changed)
        return {"ssh_host_key_changed",
                "The server's host key no longer matches the one in " + c.userFile + ". That can mean someone is intercepting the connection. If your server's admin "
                "replaced the key, remove the old one with ssh-keygen -R " + c.resolved.token() + " and test again.",
                {{"knownHostsFile", c.userFile}}};
    if (unknown)
        return {"ssh_host_unknown_key", "put.gd doesn't know this server's host key yet. Test this destination in Destinations to see the key, and select Trust key only if it matches your server's.", {}};
    if (text.contains(QLatin1String("Could not resolve hostname")) || text.contains(QLatin1String("Name or service not known")) ||
        text.contains(QLatin1String("Temporary failure in name resolution")))
        return {"ssh_host_not_found", "ssh couldn't find the server " + c.resolved.host + ". Check its name, or the Host alias in ~/.ssh/config.", {}};
    if (text.contains(QLatin1String("Connection refused")))
        return {"ssh_connection_refused", QString("The server refused the connection on port %1. Check Server and make sure its SSH service is running.").arg(port), {}};
    if (text.contains(QLatin1String("timed out")))
        return {"ssh_connection_timeout", QString("The server didn't respond on port %1. Check Server and your network.").arg(port), {}};
    // ssh skips a private key other users can read, then signs in without it: say how to fix the file.
    static const QRegularExpression tooOpen(QStringLiteral("Permissions 0[0-7]+ for '(.+)' are too open"));
    if (const auto open = tooOpen.match(text); open.hasMatch() && text.contains(QLatin1String("Permission denied")))
        return {"ssh_key_permissions", "ssh refused your key " + QFileInfo(open.captured(1)).fileName() + " because other users can read it. Make it private (chmod 600 on the file), then test again.", {}};
    static const QRegularExpression denied(QStringLiteral("Permission denied \\(([^)]*)\\)"));
    const auto methods = denied.match(text).captured(1).split(',', Qt::SkipEmptyParts);
    // put.gd turns keyboard-interactive off: a server that offers only that wants a prompt answered
    // (a one-time code, or a password typed into the server's own prompt).
    if (c.asked.contains(QLatin1String("other")) || (!methods.isEmpty() && methods == QStringList{QStringLiteral("keyboard-interactive")}))
        return {"ssh_needs_one_time_code",
                "The server asks for keyboard-interactive sign-in (a one-time code, or a password typed into its own prompt), which put.gd can't answer. "
                "Sign in with a key or your SSH agent, or ask the server's admin to allow password authentication.",
                {}};
    if (text.contains(QLatin1String("Permission denied")) || text.contains(QLatin1String("Too many authentication failures"))) {
        const auto user = c.resolved.user;
        if (c.asked.contains(QLatin1String("ambiguous")))
            return {"ssh_auth_failed",
                    "ssh asked for a key's passphrase, but two of your key files share the first 100 bytes of their path, which is all ssh shows, so put.gd "
                    "didn't answer. Give one key a shorter path, or add it to your SSH agent.",
                    {}};
        if (c.asked.contains(QLatin1String("passphrase")) && !c.passphrase)
            return {"ssh_auth_failed", "Your key " + QFileInfo(c.passphraseKey).fileName() + " needs its passphrase. Enter it in Key passphrase, or add the key to your SSH agent.", {}};
        if (c.asked.contains(QLatin1String("password")) && !c.password)
            return {"ssh_auth_failed", "The server asked " + user + " for a password and refused the keys put.gd offered. Enter the password in Password, or add your key to the server.", {}};
        // ssh says nothing (below debug level) about a wrong passphrase: one that was asked and given
        // may be wrong, or the server may not know the key.
        if (!c.resolved.chosenKey.isEmpty() && !c.password)
            return {"ssh_auth_failed", "The server refused to sign in " + user + " with the key " + QFileInfo(c.resolved.chosenKey).fileName() + ". " +
                        (c.asked.contains(QLatin1String("passphrase")) ? QStringLiteral("Check the Key passphrase, the user name, and ") : QStringLiteral("Check the user name, and ")) +
                        "that the key's public half is in that user's ~/.ssh/authorized_keys on the server.", {}};
        if (!c.resolved.agent && c.resolved.identityFiles.isEmpty() && !c.password)
            return {"ssh_auth_failed", "The server refused to sign in " + user + ". No SSH agent, SSH key, or Password is set. Start ssh-agent with your key, select an SSH key, or enter a Password.", {}};
        return {"ssh_auth_failed", "The server refused to sign in " + user + (c.password ? QStringLiteral(" with the password put.gd sent. Check User and Password.")
                               : QStringLiteral(" with the keys put.gd offered. Check User and make sure the server accepts your key.")), {}};
    }
    if (text.contains(QLatin1String("subsystem request failed")) || c.protocolNoise)
        return {"ssh_no_sftp",
                "The server has no SFTP service for this account. Ask its admin to enable the sftp subsystem. If your shell prints text on startup "
                "(for example in .bashrc), print it only in interactive shells so it doesn't break SFTP.",
                {}};
    return {"ssh_failed", QString("ssh stopped before SFTP started (exit status %1). Run ssh -v %2 in a terminal to see why.").arg(c.exitCode).arg(c.resolved.alias.isEmpty() ? c.resolved.host : c.resolved.alias),
            {{"exitStatus", c.exitCode}}};
}

QString probeHostKey(const QString& ssh, const QJsonObject& settings, Error& error) {
    QTemporaryDir folder;
    if (!folder.isValid()) { error = {"ssh_failed", "Couldn't create a temporary file for the host key. Check free disk space and permissions in your temp folder.", {}}; return {}; }
    const auto file = folder.filePath("known_hosts");
    QProcess process;
    // A fresh environment with no askpass: nothing can be asked, so nothing is answered.
    process.setProcessEnvironment(sshEnvironment());
    process.setStandardInputFile(QProcess::nullDevice());
    // accept-new into a file of its own: the key ssh got is written there and nowhere else, and
    // with no authentication method allowed no credential reaches the server. A ProxyJump hop's
    // own key is checked by the user's ssh settings, as in a terminal.
    process.start(ssh, baseOptions() + QStringList{"-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=accept-new", "-o", "HashKnownHosts=no",
                                                    "-o", "UserKnownHostsFile=" + quoted(file), "-o", "GlobalKnownHostsFile=/dev/null",
                                                    "-o", "PreferredAuthentications=none", "-o", "IdentityAgent=none", "-o", "ConnectTimeout=20", "-s"} +
                           destinationArguments(settings) + QStringList{"sftp"});
    if (!process.waitForStarted(10000)) { error = sshMissing(); return {}; }
    if (!process.waitForFinished(40000)) { process.kill(); process.waitForFinished(2000); }
    QFile written(file);
    if (written.open(QIODevice::ReadOnly))
        for (const auto& raw : QString::fromUtf8(written.read(65536)).split('\n')) {
            const auto line = raw.trimmed();
            if (!line.isEmpty() && !line.startsWith('#') && !fingerprintOfLine(line).isEmpty()) return line;
        }
    FailureContext context;
    const auto address = parseServer(settings["server"].toString());
    context.resolved.host = context.resolved.alias = address ? address->host : settings["server"].toString();
    context.resolved.port = address && address->port ? address->port : 22;
    context.exitCode = process.exitCode();
    error = classify(process.readAllStandardError(), context);
    if (error.code == QLatin1String("ssh_auth_failed") || error.code == QLatin1String("ssh_host_unknown_key") || error.code == QLatin1String("ssh_needs_one_time_code"))
        error = {"ssh_failed", "ssh ended before the server sent its host key. Run ssh -v " + context.resolved.alias + " in a terminal to see why.", {}};
    return {};
}
namespace {
Error knownHostsError(const QString& path, const QString& why) { return {"sftp_known_hosts_failed", "put.gd couldn't update its known_hosts file " + path + ": " + why, {}}; }
}
Error trustLine(const QString& path, const QString& line, const QString& previous) {
    const auto host = hostOfLine(line), fingerprint = fingerprintOfLine(line);
    if (host.isEmpty() || fingerprint.isEmpty()) return {"sftp_protocol", "Couldn't parse the server's host key line.", {}};
    // Its own folder, private: other users can't add keys put.gd would then trust. A symbolic
    // link, or anything but a folder of this user's, is refused.
    const auto folder = QFileInfo(path).absolutePath();
    struct stat place {};
    if (::lstat(QFile::encodeName(folder).constData(), &place) == 0) {
        if (!S_ISDIR(place.st_mode) || place.st_uid != ::getuid()) return knownHostsError(path, QStringLiteral("its folder isn't a folder of yours"));
    } else if (!QDir().mkpath(folder)) return knownHostsError(path, QStringLiteral("its folder can't be made"));
    if (::chmod(QFile::encodeName(folder).constData(), 0700) != 0) return knownHostsError(path, QStringLiteral("its folder can't be made private"));
    // One worker at a time reads, changes and writes the file.
    const int lock = ::open(QFile::encodeName(folder + "/known_hosts.lock").constData(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || ::flock(lock, LOCK_EX) != 0) { if (lock >= 0) ::close(lock); return knownHostsError(path, QStringLiteral("it can't be locked")); }
    struct Unlock { int fd; ~Unlock() { ::flock(fd, LOCK_UN); ::close(fd); } } unlock{lock};
    struct stat info {};
    const auto encoded = QFile::encodeName(path);
    QStringList kept;
    if (::lstat(encoded.constData(), &info) == 0) {
        if (!S_ISREG(info.st_mode)) return knownHostsError(path, QStringLiteral("it isn't a regular file"));
        // All of it is kept; a file too large to be put.gd's own is refused rather than cut short.
        if (info.st_size > 16 * 1024 * 1024) return knownHostsError(path, QStringLiteral("it is larger than 16 MB"));
        QFile existing(path);
        if (!existing.open(QIODevice::ReadOnly)) return knownHostsError(path, QStringLiteral("it can't be read"));
        const auto content = existing.readAll();
        if (content.size() != info.st_size) return knownHostsError(path, QStringLiteral("it changed while it was read"));
        for (const auto& raw : QString::fromUtf8(content).split('\n')) {
            const auto trimmed = raw.trimmed();
            if (trimmed.isEmpty()) continue;
            if (hostOfLine(trimmed) == host) {
                // A re-trust replaces only the key the destination recorded; another one there means
                // the file changed since the key was shown.
                const auto there = fingerprintOfLine(trimmed);
                if (there != fingerprint && there != previous)
                    return {"ssh_host_key_mismatch", "put.gd's known_hosts holds another key for this server than the one this destination trusted. Test again and compare the keys.", {}};
                continue;
            }
            kept.append(trimmed);
        }
    } else if (errno != ENOENT) return knownHostsError(path, QStringLiteral("it can't be checked"));
    kept.append(line.trimmed());
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) return knownHostsError(path, QStringLiteral("it can't be written"));
    out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    out.write((kept.join('\n') + '\n').toUtf8());
    if (!out.commit()) return knownHostsError(path, QStringLiteral("it can't be written"));
    if (::lstat(encoded.constData(), &info) != 0 || !S_ISREG(info.st_mode) || (info.st_mode & 0777) != 0600 || info.st_uid != ::getuid())
        return knownHostsError(path, QStringLiteral("it didn't end up as a private file"));
    return {};
}
}
