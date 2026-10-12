#pragma once
#include "S3Driver.h"
#include <cstdint>
namespace putgd {
inline constexpr std::uint32_t s3ModuleContractVersion = 1;
struct S3ModuleApi {
    std::uint32_t version;
    const char* buildIdentity;
    bool safeDeleteTransport;
    S3Driver* (*create)(CredentialLookup, ReadinessCheck);
    void (*destroy)(S3Driver*);
};
// Resident private module owner. It admits no untrusted search path or plugin discovery.
class S3Module final {
public:
    static std::shared_ptr<S3Module> acquire();
    static QString defaultPath();
    explicit S3Module(QString absolutePath = defaultPath());
    ~S3Module();
    void post(std::function<void()>);
    // Creation is worker-only. Destruction runs directly there, or queues and drains
    // for external callers; clients and SDK runtime stay on their original owner.
    S3Driver* createDriver(CredentialLookup, ReadinessCheck, Error&);
    void destroyDriver(S3Driver*);
    int liveDriverCount() const;
    bool workerStarted() const;
    S3Module(const S3Module&) = delete;
    S3Module& operator=(const S3Module&) = delete;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
