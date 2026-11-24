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
// variables and any askpass removed), LC_ALL=C so its messages read the same everywhere.
QProcessEnvironment sshEnvironment();
// The ssh program on that environment's PATH, or empty.
QString sshProgram(const QProcessEnvironment&);
Error sshMissing();
// The agent socket a desktop provides: SSH_AUTH_SOCK, else the usual sockets under
// $XDG_RUNTIME_DIR (GNOME Keyring/gcr, KDE's ssh-agent unit, gpg-agent). Empty: none.
QString agentSocket(const QProcessEnvironment&);
// Where the destination's settings point, as `ssh -G` resolves them.
struct Resolved {
    QString host, user, alias, hostKeyAlias;
    int port = 22;
    QStringList identityFiles;     // the key files ssh would try that exist, tilde-expanded
    QStringList userKnownHosts;    // the user's known_hosts files, as ssh -G names them
    QStringList globalKnownHosts;
    bool userKnownHostsNone = false;  // UserKnownHostsFile none
    bool knownHostsUnresolved = false;  // a known_hosts path with a token put.gd can't expand
    QString identityAgent;
    QString proxyKind;             // "ProxyJump", "ProxyCommand" or empty; never the command itself
    bool agent = false;            // an agent socket is there to use
    QString chosenKey;             // the destination's own SSH key file (#1892), passed with -i; empty: none
    // The known_hosts name of this server: the HostKeyAlias, the host, or [host]:port.
    QString token() const;
};
// The ssh arguments that name the destination: [-p port] [-l user] -- host.
QStringList destinationArguments(const QJsonObject& settings);
// The destination's own key file (#1892): -i <file> -o IdentitiesOnly=yes, so ssh offers that
// key (and the files ~/.ssh/config names) rather than every key in the agent first. Empty
// when the destination names none.
QStringList identityArguments(const QJsonObject& settings);
Resolved resolve(const QString& ssh, const QProcessEnvironment&, const QJsonObject& settings, Error&);
// "Connects to files.example.com:2222 as alice with key id_ed25519 (agent)"; a key file chosen
// in Settings (#1892) is the one named.
QString resolvedLine(const Resolved&, bool password);
QJsonObject toJson(const Resolved&);
// The known_hosts lines that hold a key for `token` in `files`, by ssh-keygen -F (hashed names,
// patterns and @cert-authority lines included), and the first file that had one. `error` is set
// when a lookup failed: callers fail closed then.
struct KnownLookup { QStringList lines; QString file; Error error; };
KnownLookup lookupHost(const QStringList& files, const QString& token, const QProcessEnvironment&);
// The user's own files (UserKnownHostsFile and GlobalKnownHostsFile): their entry decides alone.
KnownLookup userKnownLookup(const Resolved&, const QProcessEnvironment&);
// put.gd's own known_hosts may be handed to ssh: a regular file of this user's, mode 0600, in a
// folder that is not a symbolic link; a file that isn't there yet is fine.
Error checkPinFile(const QString& file);
// The UserKnownHostsFile value a connection checks host keys against: the user's files, and
// put.gd's own file only when the user's files know nothing of this server. "none" when the
// user turned known_hosts files off.
QString knownHostsOption(const Resolved&, const QString& putgdFile, bool userKnows);

// One ssh child: its own session (no controlling terminal), stdin and stdout for the
// SFTP stream, stderr kept (its last 64 KB) for the startup error and the host key it used.
class SshProcess {
public:
    SshProcess() = default;
    SshProcess(const SshProcess&) = delete;
    SshProcess& operator=(const SshProcess&) = delete;
    ~SshProcess();
    bool start(const QString& program, const QStringList& arguments, const QProcessEnvironment&, QString* error);
    pid_t pid() const { return pid_; }
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
// the key passphrase. Each answers once, and only its own exact prompt (matchPrompt): the
// password only "<user>@<host>'s password: " for this target, the passphrase only
// "Enter passphrase for key '<file>': " for a key file its settings name. Anything else is
// remembered and gets nothing; a security-key touch notice is reported through onTouch.
class Askpass {
public:
    Askpass(QString password, QString passphrase, QString user, QStringList hosts, QStringList identityFiles);
    ~Askpass();
    // Creates the socket in a new 0700 directory under $XDG_RUNTIME_DIR (else /tmp) and
    // starts answering; sets the variables ssh's environment needs.
    bool start(QProcessEnvironment& environment, QString program, QString* error);
    // Stops answering and forgets both secrets.
    void stop();
    // The ssh process whose own askpass may be answered: the asking process's parent must be it.
    void setSshPid(pid_t pid) { sshPid_ = pid; }
    // What was asked: "password", "passphrase", "other", "touch".
    QStringList asked() const;
    QString passphraseKey() const;
    std::function<void()> onTouch;
private:
    void serve();
    QString password_, passphrase_, user_;
    QStringList hosts_, identityFiles_;
    QByteArray token_;
    QString directory_, socket_;
    int listener_ = -1;
    std::atomic_bool stop_{false}, passwordUsed_{false}, passphraseUsed_{false};
    mutable std::mutex mutex_;
    QStringList asked_;
    QString passphraseKey_;
    std::unique_ptr<std::thread> thread_;
    std::atomic<pid_t> sshPid_{0};
};

// The options every SFTP connection runs with, then the destination and the subsystem. With
// `pinnedOnly`, the global known_hosts is off too: only the given file's keys are accepted.
QStringList connectionArguments(const QJsonObject& settings, const QString& knownHostsOption, bool pinnedOnly = false);
// ssh's startup failure as one of the codes the design lists. Messages carry no raw ssh
// output: only the code, put.gd's own words and fields put.gd parsed (a known_hosts path).
struct FailureContext {
    Resolved resolved;
    bool password = false, passphrase = false;
    QStringList asked;
    QString passphraseKey;
    // The user's known_hosts file that holds a key for this server, when one does: a changed key
    // is then in that file; otherwise only put.gd's own file can hold it.
    QString userFile;
    // The connection checked the key a receipt recorded: a host-key failure means another server.
    bool pinned = false;
    bool protocolNoise = false;  // the stream began with something that isn't SFTP
    int exitCode = -1;
};
Error classify(const QByteArray& stderrText, const FailureContext&);

// Host keys (#1871). Fetches the server's key over the real route (ProxyJump and all),
// with accept-new into a new temporary file, no authentication tried and no askpass, so no
// credential is sent: the known_hosts line ssh wrote, or empty with `error`.
QString probeHostKey(const QString& ssh, const QJsonObject& settings, Error& error);
// put.gd's known_hosts, in its own 0700 folder: adds `line`, replacing this host's earlier
// line only while it still holds `previous` (the fingerprint the destination recorded; empty:
// none). Locked against other workers, written whole, mode 0600; a symbolic link or other
// non-regular file there is refused.
Error trustLine(const QString& file, const QString& line, const QString& previous);
}
