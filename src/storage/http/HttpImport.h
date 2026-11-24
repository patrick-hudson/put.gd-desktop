#pragma once
#include "HttpCommon.h"
#include "core/SettingsService.h"
#include "secrets/CredentialStore.h"
#include <chrono>
#include <functional>
namespace putgd::http {
// settings.importUploader (#1361): adds a ShareX .sxcu file as a custom uploader
// destination. The resident reads the file itself, so its token crosses no command line,
// socket or reply. Secret values go to the credential store first (the system keychain,
// or this session), and only then does the destination, with names and references, enter
// the configuration. Runs on the settings owner thread.
//
// params: path (absolute), expectedRevision, and optionally label, profileId,
// credentialMode ("keychain", the default, or "session") and allowInsecureHttp.
// The reply names the profile and which values were stored, never a value.
// `storeDeadline`: how long the import waits on the secret store, whose prompt may never
// be answered, before it gives up and lets the next import in.
inline constexpr std::chrono::milliseconds importStoreDeadline{120000};
void importUploaderFile(SettingsService&, CredentialStore&, const QJsonObject& params, std::function<void(Error, QJsonObject)> reply,
                        std::chrono::milliseconds storeDeadline = importStoreDeadline);
// What storeCredentials checks for a custom uploader profile: the credential-store keys
// its settings need, and that typed values are known names holding usable placeholders.
Error namedCredentials(const QJsonObject& profile, const QMap<QString, QString>& values, QStringList* names);
}
