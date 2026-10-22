#pragma once
// Durable records shared with the Linux journal format: artifacts, local copies,
// destinations, receipts, operation items and operations (docs/architecture.md).
#include "common/Error.h"
#include <optional>
#include <string>
#include <vector>

namespace putgd {
struct Artifact {
    std::string id, path, name, mime, category, sha256;
    long long bytes = 0;
    bool originalFile = false;
    std::string previewPath;
    bool namingPrepared = false;
    json::Object toJson() const;
    static Artifact fromJson(const json::Object&);
};
struct LocalCopy {
    std::string status = "disabled", path; Error error;
    json::Object toJson() const;
    static LocalCopy fromJson(const json::Object&);
};
struct Destination {
    std::string providerId, profileId, profileRevision;
    json::Object settings, credentialReference;
    json::Object toJson() const;
    static Destination fromJson(const json::Object&);
};
struct StorageReceipt {
    int schemaVersion = 1;
    Destination destination;
    json::Object objectReference;
    std::string concurrencyToken, publicationId, directUrl;
    json::Object toJson() const;
    static StorageReceipt fromJson(const json::Object&);
    Error validate() const;
};
struct UploadProgress {
    std::string phase = "uploading"; // preparing, uploading, finalizing
    std::optional<long long> bytesTransferred, bytesTotal;
    bool valid() const;
    json::Object toJson() const;
    static std::optional<UploadProgress> fromJson(const json::Object&);
};
struct OperationItem {
    Artifact artifact;
    std::string uploadStatus = "stored", candidate, condition, concurrencyToken, publicationId;
    json::Object objectReference;
    StorageReceipt receipt;
    LocalCopy local;
    std::string remoteStatus = "not_requested", shareStatus = "not_requested", shareIntent;
    json::Object shareIntentOptions;
    Error error;
    std::string phase;
    std::optional<UploadProgress> uploadProgress;
    // The provider's resume journal of a resumable upload (an S3 multipart upload
    // ID), durable before the provider relies on it; empty when none remains.
    json::Object uploadJournal;
    json::Object toJson() const;
    static OperationItem fromJson(const json::Object&);
    bool linkAvailable() const;
};
struct Operation {
    std::string id, action, state = "accepted", settingsRevision, created;
    json::Object settingsDocument;
    Destination destination;
    std::vector<OperationItem> items;
    unsigned long long invocation = 0, clipboardRevision = 0;
    std::string copyStatus = "pending";
    bool cancelled = false;
    std::string phase = "accepted";
    std::optional<int> requestTimeoutMs;
    Error error;
    json::Object toJson() const;
    json::Object publicJson() const; // no settings document, no artifact paths
    static Operation fromJson(const json::Object&);
    static bool activeState(const std::string&);
    static bool terminalUpload(const std::string& uploadStatus);
};
}
