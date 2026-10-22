#include "resident/CredentialStore.h"
#include "common/Paths.h"
#include <wincred.h>
#include <windows.h>

namespace putgd {
namespace {
std::wstring target(const std::string& reference) { return paths::wide(reference); }
Error mapError(DWORD code) {
    if (code == ERROR_NOT_FOUND) return {"credential_missing", "No stored credentials for this reference; enter them in Settings", {}};
    if (code == ERROR_NO_SUCH_LOGON_SESSION) return {"credential_store_unavailable", "Windows Credential Manager is unavailable in this session", {}};
    if (code == ERROR_ACCESS_DENIED) return {"credential_denied", "Access to the stored credentials was denied", {}};
    return {"credential_store_unavailable", "Windows Credential Manager refused the request", {{"status", double(code)}}};
}
}
Error CredentialStore::save(const std::string& profileId, const std::string& reference, const Credentials& credentials) {
    const auto blob = json::serialize(json::Object{{"accessKey", credentials.accessKey}, {"secretKey", credentials.secretKey}, {"sessionToken", credentials.sessionToken}});
    if (blob.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) return {"credential_too_large", "Credentials exceed the Credential Manager record size", {}};
    auto name = target(reference);
    CREDENTIALW record{};
    record.Type = CRED_TYPE_GENERIC; record.TargetName = name.data(); record.Persist = CRED_PERSIST_LOCAL_MACHINE;
    record.CredentialBlobSize = DWORD(blob.size()); record.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(blob.data()));
    std::wstring user = L"put.gd"; record.UserName = user.data();
    std::wstring comment = L"put.gd destination credentials"; record.Comment = comment.data();
    if (!CredWriteW(&record, 0)) return mapError(GetLastError());
    std::lock_guard lock(mutex_);
    cache_[profileId] = credentials;
    ++generation_;
    return {};
}
Error CredentialStore::unlock(const std::string& profileId, const std::string& reference) {
    PCREDENTIALW record = nullptr;
    if (!CredReadW(target(reference).c_str(), CRED_TYPE_GENERIC, 0, &record)) return mapError(GetLastError());
    std::string blob(reinterpret_cast<const char*>(record->CredentialBlob), record->CredentialBlobSize);
    CredFree(record);
    const auto object = json::parseObject(blob);
    SecureZeroMemory(blob.data(), blob.size());
    if (!object || !object->value("accessKey").isString() || !object->value("secretKey").isString()) return {"credential_invalid", "The stored credential record is not usable", {}};
    Credentials credentials{object->value("accessKey").toString(), object->value("secretKey").toString(), object->value("sessionToken").toString()};
    std::lock_guard lock(mutex_);
    cache_[profileId] = credentials;
    ++generation_;
    return {};
}
void CredentialStore::setSession(const std::string& profileId, const Credentials& credentials) { std::lock_guard lock(mutex_); cache_[profileId] = credentials; ++generation_; }
std::optional<Credentials> CredentialStore::cached(const std::string& profileId) const {
    std::lock_guard lock(mutex_);
    const auto it = cache_.find(profileId);
    if (it == cache_.end()) return std::nullopt;
    return it->second;
}
void CredentialStore::forget(const std::string& profileId) { std::lock_guard lock(mutex_); cache_.erase(profileId); ++generation_; }
Error CredentialStore::remove(const std::string& reference) {
    if (!CredDeleteW(target(reference).c_str(), CRED_TYPE_GENERIC, 0) && GetLastError() != ERROR_NOT_FOUND) return mapError(GetLastError());
    return {};
}
bool CredentialStore::stored(const std::string& reference) const {
    PCREDENTIALW record = nullptr;
    if (!CredReadW(target(reference).c_str(), CRED_TYPE_GENERIC, 0, &record)) return false;
    CredFree(record);
    return true;
}
json::Object CredentialStore::diagnostics() const {
    std::lock_guard lock(mutex_);
    return {{"backend", "windows-credential-manager"}, {"cachedProfiles", double(cache_.size())}, {"generation", double(generation_)}};
}
}
