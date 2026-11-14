#include "HttpImport.h"
#include "HttpTemplate.h"
#include "core/Log.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <memory>
#include <utility>
namespace putgd::http {
namespace {
// Every secret a destination can have fits one credential-store entry: three row lists,
// the other secret values and the request body.
static_assert(4 * maximumRows + 1 <= maximumNamedValues);
static_assert(maximumSecretBytes <= maximumNamedValueChars);
Error invalid(const QString& message) { return {"invalid_request", message, {}}; }
// One import at a time for a configuration, from its first check to its saved profile.
// Two in flight would both find their destination ID free, store their values under
// the same reference, and leave the one that is saved holding the other file's token.
QSet<const void*>& importing() {
    static QSet<const void*> busy;
    return busy;
}
// Keychain entries whose write outlived its deadline. The import that asked gave up and
// released its turn, but the entry is not free until the secret store has answered:
// only then is what it wrote, if anything, removed again.
QSet<QString>& unsettled() {
    static QSet<QString> names;
    return names;
}
// A profile ID from the uploader's name: lower-case letters, digits and hyphens.
QString slug(const QString& name) {
    QString out;
    for (const auto c : name.toLower()) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out.append(c);
        else if (!out.isEmpty() && !out.endsWith('-')) out.append('-');
    }
    while (out.endsWith('-')) out.chop(1);
    return out.left(40).isEmpty() ? QStringLiteral("uploader") : out.left(40);
}
}
Error namedCredentials(const QJsonObject& profile, const QMap<QString, QString>& values, QStringList* names) {
    if (profile["provider"] != providerId) return {"setup_unsupported", "This destination does not store named values", {}};
    const auto settings = profile["settings"].toObject();
    const auto keys = secretKeys(settings);
    if (names) *names = keys;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (!keys.contains(it.key())) return {"credential_invalid", "This destination has no secret value named " + boundedMessage(it.key(), 80) + ". Save the destination's fields first.", {}};
        const bool data = it.key() == dataSecretKey();
        if (it.value().size() > (data ? maximumDataChars : maximumValueChars)) return {"credential_invalid", "The " + secretLabel(it.key()) + " is too long", {}};
        if (it.key().startsWith("headers/"))
            for (const auto c : it.value()) if (c.unicode() < 0x20 || c.unicode() == 0x7f) return {"credential_invalid", "The " + secretLabel(it.key()) + " has a line break or control character", {}};
        // The Data body is JSON or XML with braces of its own; every other value may hold placeholders.
        if (!data)
            if (const auto problem = templateProblem(it.value(), TemplateUse::Request); !problem.isEmpty())
                return {"credential_invalid", "The " + secretLabel(it.key()) + " cannot be used: " + problem, {}};
    }
    return {};
}
void importUploaderFile(SettingsService& settings, CredentialStore& credentials, const QJsonObject& params, std::function<void(Error, QJsonObject)> reply, std::chrono::milliseconds storeDeadline) {
    static const QStringList allowed{"path", "expectedRevision", "label", "profileId", "credentialMode", "allowInsecureHttp"};
    for (auto it = params.begin(); it != params.end(); ++it) if (!allowed.contains(it.key())) { reply(invalid("Unknown import field"), {}); return; }
    const auto path = params["path"].toString();
    // Absent means the keychain; anything present must say which, in so many words.
    const auto mode = params.contains("credentialMode") ? params["credentialMode"].toString() : QStringLiteral("keychain");
    static const QRegularExpression identifier("^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$");
    if (!params["path"].isString() || path.size() > 4096 || !QDir::isAbsolutePath(path) || !params["expectedRevision"].isString() ||
        (params.contains("label") && (!params["label"].isString() || params["label"].toString().trimmed().isEmpty() || params["label"].toString().size() > 128)) ||
        (params.contains("profileId") && (!params["profileId"].isString() || !identifier.match(params["profileId"].toString()).hasMatch())) ||
        !QStringList{"keychain", "session"}.contains(mode) || (params.contains("allowInsecureHttp") && !params["allowInsecureHttp"].isBool())) {
        reply(invalid("Give an absolute path, the expected revision, and optionally label, profileId, credentialMode and allowInsecureHttp"), {});
        return;
    }
    if (importing().contains(&settings)) {
        reply({"import_busy", "Another uploader is still being imported. Try again when it has finished.", {}}, {});
        return;
    }
    const auto metadata = settings.metadata();
    if (params["expectedRevision"].toString() != settings.revision() || params["expectedRevision"].toString() != metadata["observedRevision"].toString()) {
        reply({"config_conflict", "Configuration changed; read it again before importing", {{"observedRevision", metadata["observedRevision"]}}}, {});
        return;
    }
    const QFileInfo info(path);
    QFile file(path);
    if (!info.isFile() || info.size() > maximumSxcuBytes || !file.open(QIODevice::ReadOnly)) {
        reply({"sxcu_unreadable", "The .sxcu file could not be read, or is too large to be one", {}}, {});
        return;
    }
    auto bytes = file.read(maximumSxcuBytes + 1);
    file.close();
    auto imported = importSxcu(bytes);
    bytes.fill('\0');
    if (imported.error) { reply(imported.error, {}); return; }
    if (params["allowInsecureHttp"].toBool()) {
        imported.settings["allowInsecureHttp"] = true;
        // The caller has made that choice: the note asking for it no longer applies.
        imported.notes.removeAll(plainHttpNote());
        if (!transportAllowed(QUrl(imported.settings["requestUrl"].toString()), false)) imported.notes.append("This destination sends files and credentials over plain HTTP, as allowed for it.");
    }
    if (auto error = validateSettings(imported.settings)) { reply(error, {}); return; }

    auto document = settings.document();
    auto profiles = document["profiles"].toArray();
    if (profiles.size() >= 32) { reply({"config_invalid", "There are already 32 destinations; remove one first", {}}, {}); return; }
    QStringList ids;
    for (const auto& value : std::as_const(profiles)) ids.append(value.toObject()["id"].toString());
    // Nothing to store means nothing to unlock later: no keychain entry is named.
    const bool keychain = mode == "keychain" && !imported.secrets.isEmpty();
    // The keychain entry of an ID is the import's to write, and to remove again when the
    // destination cannot be saved, only while no destination names it. A destination can
    // name another ID's entry (it was made by hand, or its ID was changed); writing or
    // removing that entry would change that destination's credentials.
    const auto entry = [](const QString& candidate) { return "putgd/http/" + candidate; };
    const auto taken = [&](const QString& candidate) {
        if (!keychain) return false;
        for (const auto& value : std::as_const(profiles)) if (value.toObject()["credentials"].toObject()["reference"].toString() == entry(candidate)) return true;
        return unsettled().contains(entry(candidate));
    };
    auto id = params["profileId"].toString();
    if (!id.isEmpty() && ids.contains(id)) { reply({"profile_exists", "A destination with this ID already exists", {}}, {}); return; }
    if (!id.isEmpty() && taken(id)) {
        reply({"credential_reference_in_use", "The keychain entry this ID would use (" + entry(id) + ") already belongs to another destination, or is still being written. Choose another ID.", {}}, {});
        return;
    }
    if (id.isEmpty()) {
        const auto base = slug(imported.name);
        id = base;
        for (int n = 2; ids.contains(id) || taken(id); ++n) id = base + "-" + QString::number(n);
    }
    const auto label = params.contains("label") ? params["label"].toString().trimmed() : imported.name.isEmpty() ? QStringLiteral("Custom uploader") : imported.name;
    const QJsonObject reference = keychain ? QJsonObject{{"mode", "keychain"}, {"reference", entry(id)}} : QJsonObject{{"mode", "session"}, {"reference", QJsonValue::Null}};
    profiles.append(QJsonObject{{"id", id}, {"label", label}, {"provider", providerId}, {"settings", imported.settings}, {"credentials", reference}});
    QJsonArray stored;
    for (auto it = imported.secrets.cbegin(); it != imported.secrets.cend(); ++it) stored.append(secretLabel(it.key()));
    QJsonObject result{{"profileId", id}, {"label", label}, {"provider", providerId}, {"credentials", QJsonObject{{"mode", reference["mode"]}}},
                       {"storedValues", stored}, {"notes", QJsonArray::fromStringList(imported.notes)}};
    const auto expected = params["expectedRevision"].toString();
    QPointer<SettingsService> guarded(&settings);
    QPointer<CredentialStore> store(&credentials);
    const void* const turn = &settings;
    importing().insert(turn);
    const bool storesValues = !imported.secrets.isEmpty();
    // Every way out gives the turn back before it answers.
    const auto finish = [turn, reply](Error error, QJsonObject result) {
        importing().remove(turn);
        reply(error, result);
    };
    // A secret store can wait on a prompt that nobody answers. The import waits for it
    // this long, here and when it removes what it stored; then it gives its turn back.
    struct Wait { bool storing = true, abandoned = false; };
    const auto wait = std::make_shared<Wait>();
    const auto name = reference["reference"].toString();
    auto addProfile = [guarded, store, profiles, expected, result, finish, id, reference, storesValues, wait, name, storeDeadline](Error stored) mutable {
        if (wait->abandoned) {
            // The store answered after the import had given up. No destination was made,
            // so what it wrote is removed, and only then is the entry free again.
            if (stored || !store) { unsettled().remove(name); return; }
            store->erase(id, reference, [name](Error) { unsettled().remove(name); });
            return;
        }
        wait->storing = false;
        if (stored) { finish(stored, {}); return; }
        if (!guarded) { finish({"operation_cancelled", "The application is stopping", {}}, {}); return; }
        guarded->patchAsync({{"profiles", profiles}}, expected, [guarded, store, result, finish, id, reference, storesValues, storeDeadline](Result saved) mutable {
            if (saved.error && saved.written) {
                // The write went through and only reading the configuration back failed:
                // the destination is in the file, and it needs what was stored for it.
                auto failed = saved.error;
                failed.message = "The destination was saved, but the configuration could not be read back: " + failed.message;
                failed.details["destinationWritten"] = true;
                finish(failed, {});
                return;
            }
            if (saved.error) {
                // The destination was not created, so what this import stored for it goes
                // too: nothing is left under a reference no destination shows.
                auto failed = saved.error;
                if (!storesValues || !store || !guarded) { finish(failed, {}); return; }
                const auto answered = std::make_shared<bool>(false);
                auto cleaned = [failed, finish, answered](Error cleanup) mutable {
                    if (std::exchange(*answered, true)) return;
                    // Said only when the entry could not be removed.
                    if (cleanup) failed.details["credentialsStored"] = true;
                    finish(failed, {});
                };
                QTimer::singleShot(storeDeadline, guarded.data(), [cleaned]() mutable { cleaned({"credential_store_timeout", "The secret store did not answer in time", {}}); });
                store->erase(id, reference, cleaned);
                return;
            }
            if (guarded) result["metadata"] = guarded->metadata();
            logging::info(logApp(), "Custom uploader imported", {{"profileId", id}, {"storedValues", result["storedValues"].toArray().size()}, {"credentialMode", result["credentials"].toObject()["mode"]}});
            finish({}, result);
        });
    };
    if (!storesValues) { addProfile(Error{}); return; }
    Credentials value;
    value.values = imported.secrets;
    if (keychain)
        QTimer::singleShot(storeDeadline, &settings, [wait, finish, name] {
            if (!wait->storing) return;
            wait->abandoned = true;
            unsettled().insert(name);
            finish({"credential_store_timeout", "The system keychain did not answer in time, so nothing was imported. Unlock the keychain, or import with credentials for this session only, and try again.", {}}, {});
        });
    credentials.save(id, reference, std::move(value), addProfile);
}
}
