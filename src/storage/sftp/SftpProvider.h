#pragma once
#include "SftpCommon.h"
#include "core/StorageProvider.h"
#include "secrets/CredentialStore.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <unordered_map>
namespace putgd {
class HelperProcess;
// SFTP to the user's own server (#1871). Requests run in putgd-sftp-worker, which runs the
// user's own ssh and speaks SFTP v3 over it; the resident links no SSH or network code.
// One worker serves one destination at a time and keeps its connection: a worker that
// answered within the last 50 seconds takes the next request to the same destination,
// and ends itself after a minute with none. Calls block the calling worker thread.
class SftpProvider final : public StorageProvider {
public:
    // knownHostsFile: put.gd's own known_hosts, which Trust writes and every connection reads
    // after the user's own files.
    SftpProvider(CredentialStore&, QString knownHostsFile);
    ~SftpProvider() override;
    bool reportsUploadProgress() const override { return true; }
    std::shared_ptr<void> anticipate(const Destination&) override;
    QJsonObject diagnostics() const override;
    ProviderCapabilities capabilities(const Destination&) const override;
    QJsonObject diagnosticSettings(const QJsonObject& settings) const override;
    QString serializationKey(const Destination&, const QJsonObject&, const QString&) const override;
    Error validateSettings(const QJsonObject&) const override;
    void checkName(const Destination&, const QString&, std::function<void(NameCheck)>, std::shared_ptr<std::atomic_bool> cancellation = {}) override;
    void upload(const UploadRequest&, std::function<void(UploadResult)>) override;
    void stat(const StorageReceipt&, std::function<void(StatResult)>) override;
    void statForRemoval(const StorageReceipt&, std::shared_ptr<std::atomic_bool> kill, std::function<void(StatResult)>) override;
    void reconcile(const UploadRequest&, std::function<void(StatResult)>) override;
    void remove(const DeleteRequest&, DurableCheckpoint, std::function<void(DeleteResult)>) override;
    // Explicit setup: connect, host key, sign-in, folder, a test upload, its link fetched
    // anonymously and compared, and the test file deleted. Each step says what it proved.
    // `notice` hears what the user must do meanwhile, such as "Touch your security key".
    void testDestination(const Destination&, DurableCheckpoint, std::function<void(QJsonObject, Error)>, std::function<void(const QString&)> notice = {});
    // Settings and agents: inspectAccount (what ssh -G resolves), trustHostKey (input
    // {"fingerprint"}: fetched again and kept only when it is that one) and createFolder.
    QJsonObject setup(const QString& kind, const Destination&, const QJsonObject& input, Error&);
    static QString workerPath();
    QString knownHostsFile() const { return knownHosts_; }
private:
    struct Session;
    QJsonObject call(const QString& method, const Destination&, QJsonObject params, Error&, bool* uncertain = nullptr,
                     std::shared_ptr<std::atomic_bool> cancellation = {}, std::function<void(const UploadProgress&)> progress = {},
                     DurableCheckpoint checkpoint = {}, std::shared_ptr<std::atomic_bool> kill = {}, std::function<void(const QString&)> notice = {});
    QString sessionKey(const Destination&) const;
    std::unique_ptr<Session> take(const QString& key);
    void park(const QString& key, std::unique_ptr<Session>);
    void retire(std::unique_ptr<Session>);
    CredentialStore& credentials_;
    QString knownHosts_;
    std::atomic_bool stopping_{false};
    std::atomic_int active_{0};
    std::atomic<qint64> nextId_{0};
    mutable std::mutex mutex_;
    std::condition_variable threadsDone_;
    int threads_ = 0;
    std::unordered_map<std::string, std::unique_ptr<Session>> idle_;
};
}
