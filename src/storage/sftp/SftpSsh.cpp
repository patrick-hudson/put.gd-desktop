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
// What every connection, the probe and ssh -G share: no forwarding, no terminal, no
// local commands, no shared master connection.
QStringList baseOptions() {
    return {"-T", "-x", "-a", "-o", "ClearAllForwardings=yes", "-o", "RequestTTY=no", "-o", "RemoteCommand=none", "-o", "PermitLocalCommand=no",
            "-o", "EscapeChar=none", "-o", "ControlMaster=no", "-o", "ControlPath=none", "-o", "UpdateHostKeys=no"};
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
QStringList destinationArguments(const QJsonObject& settings) {
    const auto address = parseServer(settings["server"].toString());
    QStringList args;
    if (!address) return args;
    if (address->port > 0) args << "-p" << QString::number(address->port);
    if (!settings["user"].toString().isEmpty()) args << "-l" << settings["user"].toString();
    args << "--" << address->host;
    return args;
}
Resolved resolve(const QString& ssh, const QProcessEnvironment& env, const QJsonObject& settings, Error& error) {
    Resolved resolved;
    QProcess process;
    process.setProcessEnvironment(env);
    process.setStandardInputFile(QProcess::nullDevice());
    process.start(ssh, QStringList{"-G"} + destinationArguments(settings));
    if (!process.waitForStarted(10000)) { error = sshMissing(); return resolved; }
    if (!process.waitForFinished(15000)) {
        process.kill();
        process.waitForFinished(2000);
        error = {"ssh_failed", "ssh did not finish reading your SSH settings", {}};
        return resolved;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        error = {"ssh_config_invalid", "ssh couldn't read your SSH settings for this server: " + lastLine(process.readAllStandardError()), {}};
        return resolved;
    }
    const auto address = parseServer(settings["server"].toString());
    resolved.alias = address ? address->host : QString();
    QString agentSetting;
    for (const auto& raw : QString::fromUtf8(process.readAllStandardOutput()).split('\n')) {
        const auto line = raw.trimmed();
        const auto space = line.indexOf(' ');
        if (space <= 0) continue;
        const auto key = line.left(space).toLower(), value = line.mid(space + 1).trimmed();
        if (key == QLatin1String("hostname")) resolved.host = value;
        else if (key == QLatin1String("user")) resolved.user = value;
        else if (key == QLatin1String("port")) resolved.port = value.toInt();
        else if (key == QLatin1String("identityfile")) {
            const auto path = expandHome(value, env);
            if (QFileInfo(path).isFile() && !resolved.identityFiles.contains(path)) resolved.identityFiles.append(path);
        } else if (key == QLatin1String("userknownhostsfile")) {
            for (const auto& file : value.split(' ', Qt::SkipEmptyParts)) resolved.userKnownHosts.append(expandHome(file, env));
        } else if (key == QLatin1String("identityagent")) agentSetting = value;
        else if ((key == QLatin1String("proxyjump") || key == QLatin1String("proxycommand")) && value != QLatin1String("none")) resolved.proxy = value;
    }
    if (agentSetting.compare(QLatin1String("none"), Qt::CaseInsensitive) == 0) resolved.agent = false;
    else if (!agentSetting.isEmpty() && agentSetting != QLatin1String("SSH_AUTH_SOCK")) {
        resolved.identityAgent = expandHome(agentSetting, env);
        resolved.agent = isSocket(resolved.identityAgent);
    } else resolved.agent = !agentSocket(env).isEmpty();
    if (resolved.userKnownHosts.isEmpty()) resolved.userKnownHosts = {expandHome("~/.ssh/known_hosts", env), expandHome("~/.ssh/known_hosts2", env)};
    return resolved;
}
QString resolvedLine(const Resolved& r, bool password) {
    auto line = "Connects to " + (r.host.contains(':') ? "[" + r.host + "]" : r.host) + (r.port != 22 ? ":" + QString::number(r.port) : QString()) + " as " + r.user;
    if (!r.identityFiles.isEmpty()) line += " with key " + QFileInfo(r.identityFiles.first()).fileName() + (r.agent ? QStringLiteral(" (agent)") : QString());
    else if (r.agent) line += QStringLiteral(" with the agent's keys");
    else if (password) line += QStringLiteral(" with a password");
    else line += QStringLiteral(" with no key: no SSH agent or key file was found");
    if (!r.proxy.isEmpty()) line += " through " + r.proxy;
    return line;
}
QJsonObject toJson(const Resolved& r) {
    return {{"host", r.host}, {"port", r.port}, {"user", r.user}, {"alias", r.alias},
            {"keyFiles", QJsonValue(QJsonArray::fromStringList([&] { QStringList names; for (const auto& f : r.identityFiles) names.append(QFileInfo(f).fileName()); return names; }()))},
            {"agent", r.agent}, {"proxy", r.proxy}};
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
            if (errText_.size() > 16384) errText_.remove(0, errText_.size() - 16384);
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

Askpass::Askpass(QString password, QString passphrase) : password_(std::move(password)), passphrase_(std::move(passphrase)) {}
Askpass::~Askpass() {
    stop();
    password_.fill(QChar(0));
    passphrase_.fill(QChar(0));
}
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
                const auto kind = QString::fromUtf8(lines[1]);
                const auto prompt = QString::fromUtf8(lines[2]);
                const auto lower = prompt.toLower();
                QString what;
                QString* secret = nullptr;
                std::atomic_bool* used = nullptr;
                std::atomic_bool* refused = nullptr;
                if (kind == QLatin1String("none")) what = lower.contains("touch") || lower.contains("presence") ? QStringLiteral("touch") : QStringLiteral("notice");
                else if (kind == QLatin1String("confirm")) what = QStringLiteral("confirm");
                else if (lower.contains("passphrase")) {
                    what = QStringLiteral("passphrase");
                    secret = &passphrase_;
                    used = &passphraseUsed_;
                    refused = &passphraseRefused_;
                    static const QRegularExpression keyFile(QStringLiteral("for (?:key )?'?([^':]+)'?:"));
                    const auto match = keyFile.match(prompt);
                    std::lock_guard lock(mutex_);
                    if (match.hasMatch()) passphraseKey_ = match.captured(1).trimmed();
                } else if (lower.contains("password") && !lower.contains("one-time") && !lower.contains("otp")) {
                    what = QStringLiteral("password");
                    secret = &password_;
                    used = &passwordUsed_;
                    refused = &passwordRefused_;
                } else what = QStringLiteral("code");
                {
                    std::lock_guard lock(mutex_);
                    if (!asked_.contains(what)) asked_.append(what);
                }
                if (what == QLatin1String("touch") && onTouch) onTouch();
                if (secret) {
                    if (!secret->isEmpty() && !used->exchange(true)) {
                        auto bytes = secret->toUtf8();
                        reply = "ok\n" + bytes;
                        bytes.fill('\0');
                    } else *refused = true;
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

QStringList connectionArguments(const QJsonObject& settings, const QStringList& knownHosts) {
    QStringList files;
    for (const auto& file : knownHosts) files.append(quoted(file));
    return baseOptions() + QStringList{"-o", "StrictHostKeyChecking=yes", "-o", "NumberOfPasswordPrompts=1", "-o", "BatchMode=no",
                                       "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=4", "-o", "ConnectTimeout=20",
                                       "-o", "UserKnownHostsFile=" + files.join(' '), "-s"} +
           destinationArguments(settings) + QStringList{"sftp"};
}
QString lastLine(const QByteArray& text) {
    const auto lines = QString::fromUtf8(text).split('\n', Qt::SkipEmptyParts);
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        auto line = it->trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1String("debug")) || line.startsWith(QLatin1String("Warning: Permanently added"))) continue;
        line.replace(QRegularExpression(QStringLiteral("[\\x00-\\x1f\\x7f]")), QStringLiteral(" "));
        return line.left(300);
    }
    return QStringLiteral("no error output");
}
Error classify(const QByteArray& raw, const FailureContext& c) {
    const auto text = QString::fromUtf8(raw);
    const auto host = c.resolved.alias.isEmpty() ? c.resolved.host : c.resolved.alias;
    const auto port = c.resolved.port;
    if (text.contains(QLatin1String("REMOTE HOST IDENTIFICATION HAS CHANGED")) || text.contains(QRegularExpression(QStringLiteral("Host key for \\S+ has changed")))) {
        static const QRegularExpression offending(QStringLiteral("Offending \\S+ key in (.+):(\\d+)"));
        const auto match = offending.match(text);
        const auto file = match.hasMatch() ? match.captured(1).trimmed() : QString();
        if (!file.isEmpty() && !c.knownHostsFile.isEmpty() && QFileInfo(file).absoluteFilePath() == QFileInfo(c.knownHostsFile).absoluteFilePath())
            return {"ssh_trusted_key_changed",
                    "The server's host key changed since you trusted it in put.gd. Ask your server's admin whether it was replaced; trust the new key in "
                    "Settings → Destinations only if they confirm it.",
                    {}};
        const auto target = port != 22 ? "[" + host + "]:" + QString::number(port) : host;
        return {"ssh_host_key_changed",
                "The server's host key no longer matches the one in " + (file.isEmpty() ? QStringLiteral("your known_hosts file") : file + " line " + match.captured(2)) +
                    ". That can mean someone is intercepting the connection. If your server's admin replaced the key, remove the old one with ssh-keygen -R " + target +
                    " and test again.",
                {{"knownHostsFile", file}}};
    }
    if (text.contains(QRegularExpression(QStringLiteral("No \\S+ host key is known for"))) || text.contains(QLatin1String("Host key verification failed")))
        return {"ssh_host_unknown_key", "put.gd doesn't know this server's host key yet. Test the destination in Settings to see the key, and trust it only if it matches your server's.", {}};
    if (text.contains(QLatin1String("Could not resolve hostname")) || text.contains(QLatin1String("Name or service not known")) ||
        text.contains(QLatin1String("Temporary failure in name resolution")))
        return {"ssh_host_not_found", "ssh couldn't find the server " + c.resolved.host + ". Check its name, or the Host alias in ~/.ssh/config.", {}};
    if (text.contains(QLatin1String("Connection refused")))
        return {"ssh_connection_refused", QString("The server refused the connection on port %1. Check the server name and port, and that its SSH service runs.").arg(port), {}};
    if (text.contains(QLatin1String("timed out")))
        return {"ssh_connection_timeout", QString("The server did not answer on port %1. Check the server name and port, and your network.").arg(port), {}};
    if (c.asked.contains(QLatin1String("code")))
        return {"ssh_needs_one_time_code", "The server asks for a one-time code, which put.gd can't answer. Sign in with a key or your SSH agent instead.", {}};
    if (text.contains(QLatin1String("Permission denied")) || text.contains(QLatin1String("Too many authentication failures"))) {
        const auto user = c.resolved.user;
        if (c.asked.contains(QLatin1String("passphrase")) && !c.passphrase)
            return {"ssh_auth_failed", "Your key " + QFileInfo(c.passphraseKey).fileName() + " needs its passphrase: enter it in Key passphrase, or add the key to your SSH agent.", {}};
        if (c.asked.contains(QLatin1String("password")) && !c.password)
            return {"ssh_auth_failed", "The server asked " + user + " for a password and refused the keys put.gd offered. Enter the password in Password, or add your key to the server.", {}};
        if (!c.resolved.agent && c.resolved.identityFiles.isEmpty() && !c.password)
            return {"ssh_auth_failed", "The server refused to sign in " + user + ". No SSH agent was found and no key file or password is set: start ssh-agent with your key, or enter a password.", {}};
        return {"ssh_auth_failed", "The server refused to sign in " + user + " with the " +
                    (c.password ? QStringLiteral("password") : QStringLiteral("keys")) + " put.gd offered. Check the user name, and that the server accepts your key.", {}};
    }
    if (text.contains(QLatin1String("subsystem request failed")) || c.protocolNoise)
        return {"ssh_no_sftp",
                "The server has no SFTP service for this account. Ask its admin to enable the sftp subsystem. If your shell prints text when it starts "
                "(in .bashrc or similar), that text breaks SFTP: print it only in interactive shells.",
                {}};
    return {"ssh_failed", "ssh failed: " + lastLine(raw), {}};
}

QString probeHostKey(const QString& ssh, const QProcessEnvironment& env, const QJsonObject& settings, Error& error) {
    QTemporaryDir folder;
    if (!folder.isValid()) { error = {"ssh_failed", "put.gd couldn't create a temporary file for the host key", {}}; return {}; }
    const auto file = folder.filePath("known_hosts");
    QProcess process;
    process.setProcessEnvironment(env);
    process.setStandardInputFile(QProcess::nullDevice());
    // accept-new into a file of its own: the key ssh got is written there and nowhere else,
    // and with no authentication method allowed no credential reaches the server.
    process.start(ssh, baseOptions() + QStringList{"-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=accept-new", "-o", "HashKnownHosts=no", "-o", "CheckHostIP=no",
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
    context.resolved.host = address ? address->host : settings["server"].toString();
    context.resolved.port = address && address->port ? address->port : 22;
    const auto stderrText = process.readAllStandardError();
    error = classify(stderrText, context);
    if (error.code == QLatin1String("ssh_auth_failed") || error.code == QLatin1String("ssh_host_unknown_key"))
        error = {"ssh_failed", "ssh ended before the server sent its host key: " + lastLine(stderrText), {}};
    return {};
}
Error trustLine(const QString& path, const QString& line) {
    const auto host = hostOfLine(line), fingerprint = fingerprintOfLine(line);
    if (host.isEmpty() || fingerprint.isEmpty()) return {"sftp_protocol", "The host key line is not one put.gd can read", {}};
    QStringList kept;
    QFile existing(path);
    if (existing.exists()) {
        if (!existing.open(QIODevice::ReadOnly)) return {"sftp_known_hosts_failed", "put.gd couldn't read its known_hosts file " + path, {}};
        for (const auto& raw : QString::fromUtf8(existing.readAll()).split('\n')) {
            const auto trimmed = raw.trimmed();
            if (trimmed.isEmpty()) continue;
            if (hostOfLine(trimmed) == host) continue;  // this host's earlier key: replaced
            kept.append(trimmed);
        }
    }
    kept.append(line.trimmed());
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) return {"sftp_known_hosts_failed", "put.gd couldn't write its known_hosts file " + path, {}};
    out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    out.write((kept.join('\n') + '\n').toUtf8());
    if (!out.commit()) return {"sftp_known_hosts_failed", "put.gd couldn't write its known_hosts file " + path, {}};
    return {};
}
}
