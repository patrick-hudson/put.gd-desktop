// Lab-only stdin broker. S3 format is identical to src/secrets/CredentialStore.cpp.
// Proton relay locks exactly as src/storage/proton/ProtonProvider.cpp does.
#include <qt6keychain/keychain.h>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <cerrno>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr qsizetype limit = 262144;
const QRegularExpression labReference(QStringLiteral("^putgd-lab-[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"));
bool text(const QJsonValue& value, qsizetype maximum, bool empty = false) {
    if (!value.isString()) return false;
    const auto string = value.toString();
    if (string.size() > maximum || (!empty && string.isEmpty())) return false;
    for (auto character : string) if (character.unicode() < 32) return false;
    return true;
}
bool keys(const QJsonObject& value, const QStringList& expected) {
    if (value.size() != expected.size()) return false;
    for (auto it = value.begin(); it != value.end(); ++it) if (!expected.contains(it.key())) return false;
    return true;
}
bool finish(QKeychain::Job& job) {
    job.setInsecureFallback(false);
    job.setAutoDelete(false);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    bool completed = false;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, [&] { completed = true; loop.quit(); });
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(20000);
    job.start();
    if (!completed) loop.exec();
    return completed;
}
QByteArray s3(const QJsonObject& request) {
    const auto op = request["op"].toString();
    const auto reference = request["reference"].toString();
    if (!text(request["reference"], 256) || !labReference.match(reference).hasMatch()) return {};
    if (op == "import") {
        if (!keys(request, {"provider", "op", "reference", "credentials"}) || !request["credentials"].isObject()) return {};
        const auto credentials = request["credentials"].toObject();
        if (!keys(credentials, {"accessKey", "secretKey", "sessionToken"}) ||
            !text(credentials["accessKey"], 4096) || !text(credentials["secretKey"], 4096) ||
            !text(credentials["sessionToken"], 16384, true)) return {};
        QKeychain::ReadPasswordJob existing("putgd");
        existing.setKey(reference);
        if (!finish(existing) || existing.error() != QKeychain::EntryNotFound) return {};
        QKeychain::WritePasswordJob write("putgd");
        write.setKey(reference);
        write.setBinaryData(QJsonDocument(credentials).toJson(QJsonDocument::Compact));
        if (!finish(write) || write.error()) return {};
    } else if (op == "delete") {
        if (!keys(request, {"provider", "op", "reference"})) return {};
        QKeychain::DeletePasswordJob remove("putgd");
        remove.setKey(reference);
        if (!finish(remove) || (remove.error() && remove.error() != QKeychain::EntryNotFound)) return {};
    } else return {};
    return "{\"ok\":true}\n";
}
QByteArray proton(QJsonObject request) {
    if (!text(request["reference"], 256) || !text(request["broker"], 4096)) return {};
    if (request["op"] == "export") {
        struct stat outputInfo {};
        if (::fstat(STDOUT_FILENO, &outputInfo) != 0 ||
            (!S_ISFIFO(outputInfo.st_mode) && !S_ISSOCK(outputInfo.st_mode))) return {};
    }
    const auto program = request.take("broker").toString();
    request.remove("provider");
    const QFileInfo executable(program);
    if (!executable.isAbsolute() || !executable.isFile() || executable.isSymLink() ||
        !executable.isExecutable() || executable.ownerId() != getuid() ||
        executable.permissions() & (QFile::WriteGroup | QFile::WriteOther) ||
        executable.canonicalFilePath() != executable.absoluteFilePath()) return {};
    const auto lockRoot = QString("/tmp/putgd-proton-accounts-%1").arg(getuid());
    if (::mkdir(QFile::encodeName(lockRoot).constData(), 0700) != 0 && errno != EEXIST) return {};
    const QFileInfo directory(lockRoot);
    if (!directory.isDir() || directory.isSymLink() || directory.ownerId() != getuid() ||
        directory.permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup |
                                  QFile::ReadOther | QFile::WriteOther | QFile::ExeOther) ||
        directory.canonicalFilePath() != directory.absoluteFilePath()) return {};
    const auto accountKey = QString::fromLatin1(QCryptographicHash::hash(request["reference"].toString().toUtf8(), QCryptographicHash::Sha256).toHex());
    QLockFile lock(QDir(lockRoot).filePath(accountKey + ".lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(2000)) return {};
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.setStandardErrorFile(QProcess::nullDevice());
    process.start(program, QStringList{});
    if (!process.waitForStarted(5000)) return {};
    const auto input = QJsonDocument(request).toJson(QJsonDocument::Compact);
    if (input.size() > limit || process.write(input) != input.size()) { process.kill(); process.waitForFinished(); return {}; }
    process.closeWriteChannel();
    QByteArray output;
    QElapsedTimer deadline;
    deadline.start();
    bool failed = false;
    while (process.state() != QProcess::NotRunning) {
        process.waitForReadyRead(100);
        output += process.readAllStandardOutput();
        if (output.size() > limit || deadline.elapsed() > 25000) { failed = true; process.kill(); process.waitForFinished(); break; }
    }
    output += process.readAllStandardOutput();
    if (failed || output.size() > limit || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) return {};
    QJsonParseError error;
    const auto result = QJsonDocument::fromJson(output, &error);
    if (error.error != QJsonParseError::NoError || !result.isObject() || result.object()["ok"] != true) return {};
    return output; // Private host pipe; public Python wrapper never prints it.
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QFile input;
    QFile output;
    if (!input.open(STDIN_FILENO, QIODevice::ReadOnly) || !output.open(STDOUT_FILENO, QIODevice::WriteOnly)) return 1;
    QByteArray raw;
    while (raw.size() <= limit) {
        const auto chunk = input.read(limit + 1 - raw.size());
        if (chunk.isEmpty()) break;
        raw += chunk;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(raw, &error);
    QByteArray result;
    if (argc == 1 && raw.size() <= limit && error.error == QJsonParseError::NoError && document.isObject()) {
        const auto request = document.object();
        if (request["provider"] == "s3") result = s3(request);
        else if (request["provider"] == "proton") result = proton(request);
    }
    if (result.isEmpty()) {
        output.write("{\"ok\":false,\"status\":\"credential_provisioning_failed\"}\n");
        return 1;
    }
    output.write(result);
    return 0;
}
