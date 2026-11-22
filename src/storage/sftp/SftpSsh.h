#pragma once
#include "core/Error.h"
#include <QByteArray>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <sys/types.h>
// putgd-sftp-worker's side of the user's OpenSSH client (#1871): finding ssh, what
// `ssh -G` resolves, the options every connection runs with, the askpass socket, the
// host-key probe and put.gd's own known_hosts file, and reading ssh's startup errors.
namespace putgd::sftp {
// ssh's environment: the host's (AppImage library overrides undone, put.gd's own
// variables removed), LC_ALL=C so its messages read the same everywhere, and no terminal.
QProcessEnvironment sshEnvironment();
// The ssh program on that environment's PATH, or empty.
QString sshProgram(const QProcessEnvironment&);
Error sshMissing();
// The agent socket a desktop provides: SSH_AUTH_SOCK, else the usual sockets under
// $XDG_RUNTIME_DIR (GNOME Keyring/gcr, KDE's ssh-agent unit, gpg-agent). Empty: none.
QString agentSocket(const QProcessEnvironment&);
// Where the destination's settings point, as `ssh -G` resolves them.
struct Resolved {
    QString host, user, alias;
    int port = 22;
    QStringList identityFiles;     // the key files ssh would try that exist
    QStringList userKnownHosts;    // the user's known_hosts files, as ssh -G names them
    QString identityAgent, proxy;  // proxy: ProxyJump or ProxyCommand, as configured
    bool agent = false;            // an agent socket is there to use
};
// The ssh arguments that name the destination: [-p port] [-l user] -- host.
QStringList destinationArguments(const QJsonObject& settings);
Resolved resolve(const QString& ssh, const QProcessEnvironment&, const QJsonObject& settings, Error&);
// "Connects to files.example.com:2222 as alice with key id_ed25519 (agent)".
QString resolvedLine(const Resolved&, bool password);
QJsonObject toJson(const Resolved&);

// One ssh child: its own session (no controlling terminal), stdin and stdout for the
// SFTP stream, stderr kept (its last 16 KB) for the startup error.
class SshProcess {
public:
    SshProcess() = default;
    SshProcess(const SshProcess&) = delete;
    SshProcess& operator=(const SshProcess&) = delete;
    ~SshProcess();
    bool start(const QString& program, const QStringList& arguments, const QProcessEnvironment&, QString* error);
    int input() const { return in_; }
    int output() const { return out_; }
    int errors() const { return err_; }
    // Reads what stderr has now into errorText().
    void drainErrors();
    const QByteArray& errorText() const { return errText_; }
    bool running();
    // Closes stdin and waits up to `milliseconds` for the exit, then kills.
    void finish(int milliseconds);
    void kill();
    int exitCode() const { return exit_; }
private:
    pid_t pid_ = -1;
    int in_ = -1, out_ = -1, err_ = -1, exit_ = -1;
    bool reaped_ = false;
    QByteArray errText_;
};

// The askpass socket for one connection: ssh's askpass child asks it for the password or
// the key passphrase. Each answers once, and only the prompt it belongs to: a key
// passphrase never goes to a password prompt. A one-time code prompt gets nothing and is
// remembered, as is a security-key touch notice.
class Askpass {
public:
    Askpass(QString password, QString passphrase);
    ~Askpass();
    // Creates the socket in a new 0700 directory under $XDG_RUNTIME_DIR (else /tmp) and
    // starts answering; sets the variables ssh's environment needs.
    bool start(QProcessEnvironment& environment, QString program, QString* error);
    void stop();
    // What was asked: "password", "passphrase", "code", "touch", "confirm".
    QStringList asked() const;
    bool passwordRefused() const { return passwordRefused_; }
    bool passphraseRefused() const { return passphraseRefused_; }
    QString passphraseKey() const;
    // Called on the askpass thread when ssh asks for a security key touch.
    std::function<void()> onTouch;
private:
    void serve();
    QString password_, passphrase_;
    QByteArray token_;
    QString directory_, socket_;
    int listener_ = -1;
    std::atomic_bool stop_{false}, passwordUsed_{false}, passphraseUsed_{false};
    std::atomic_bool passwordRefused_{false}, passphraseRefused_{false};
    mutable std::mutex mutex_;
    QStringList asked_;
    QString passphraseKey_;
    std::unique_ptr<std::thread> thread_;
};

// The options every SFTP connection runs with, then the destination and the subsystem.
// `knownHosts` are the files host keys are checked against: the user's, then put.gd's.
QStringList connectionArguments(const QJsonObject& settings, const QStringList& knownHosts);
// ssh's startup failure as one of the codes the design lists, from its stderr.
// `context` carries what put.gd knows: the resolved user and port, whether a password or
// passphrase was set, whether an agent was found, what askpass was asked, put.gd's file.
struct FailureContext {
    Resolved resolved;
    bool password = false, passphrase = false;
    QStringList asked;
    QString passphraseKey, knownHostsFile;
    bool protocolNoise = false;  // the stream began with something that isn't SFTP
};
Error classify(const QByteArray& stderrText, const FailureContext&);
// The last line of ssh's stderr that says something, for the generic error.
QString lastLine(const QByteArray& stderrText);

// Host keys (#1871). Fetches the server's key over the real route (ProxyJump and all),
// with accept-new into a new temporary file and no authentication tried, so no
// credential is sent: the known_hosts line ssh wrote, or empty with `error`.
QString probeHostKey(const QString& ssh, const QProcessEnvironment&, const QJsonObject& settings, Error& error);
// put.gd's known_hosts: adds `line`, first removing the lines for the same host token
// whose key differs (a re-trust). Written whole through a temporary file, mode 0600.
Error trustLine(const QString& file, const QString& line);
}
