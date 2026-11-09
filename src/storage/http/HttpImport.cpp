#include "HttpImport.h"
#include "HttpTemplate.h"
#include "core/Log.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QPointer>
#include <QRegularExpression>
namespace putgd::http {
namespace {
Error invalid(const QString& message) { return {"invalid_request", message, {}}; }
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
void importUploaderFile(SettingsService& settings, CredentialStore& credentials, const QJsonObject& params, std::function<void(Error, QJsonObject)> reply) {
    static const QStringList allowed{"path", "expectedRevision", "label", "profileId", "credentialMode", "allowInsecureHttp"};
    for (auto it = params.begin(); it != params.end(); ++it) if (!allowed.contains(it.key())) { reply(invalid("Unknown import field"), {}); return; }
    const auto path = params["path"].toString();
    const auto mode = params["credentialMode"].toString("keychain");
    static const QRegularExpression identifier("^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$");
    if (!params["path"].isString() || path.size() > 4096 || !QDir::isAbsolutePath(path) || !params["expectedRevision"].isString() ||
        (params.contains("label") && (!params["label"].isString() || params["label"].toString().trimmed().isEmpty() || params["label"].toString().size() > 128)) ||
        (params.contains("profileId") && (!params["profileId"].isString() || !identifier.match(params["profileId"].toString()).hasMatch())) ||
        !QStringList{"keychain", "session"}.contains(mode) || (params.contains("allowInsecureHttp") && !params["allowInsecureHttp"].isBool())) {
        reply(invalid("Give an absolute path, the expected revision, and optionally label, profileId, credentialMode and allowInsecureHttp"), {});
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
    if (params["allowInsecureHttp"].toBool()) imported.settings["allowInsecureHttp"] = true;
    if (auto error = validateSettings(imported.settings)) { reply(error, {}); return; }

    auto document = settings.document();
    auto profiles = document["profiles"].toArray();
    if (profiles.size() >= 32) { reply({"config_invalid", "There are already 32 destinations; remove one first", {}}, {}); return; }
    QStringList ids;
    for (const auto& value : profiles) ids.append(value.toObject()["id"].toString());
    auto id = params["profileId"].toString();
    if (!id.isEmpty() && ids.contains(id)) { reply({"profile_exists", "A destination with this ID already exists", {}}, {}); return; }
    if (id.isEmpty()) {
        const auto base = slug(imported.name);
        id = base;
        for (int n = 2; ids.contains(id); ++n) id = base + "-" + QString::number(n);
    }
    const auto label = params.contains("label") ? params["label"].toString().trimmed() : imported.name.isEmpty() ? QStringLiteral("Custom uploader") : imported.name;
    // Nothing to store means nothing to unlock later: no keychain entry is named.
    const bool keychain = mode == "keychain" && !imported.secrets.isEmpty();
    const QJsonObject reference = keychain ? QJsonObject{{"mode", "keychain"}, {"reference", "putgd/http/" + id}} : QJsonObject{{"mode", "session"}, {"reference", QJsonValue::Null}};
    profiles.append(QJsonObject{{"id", id}, {"label", label}, {"provider", providerId}, {"settings", imported.settings}, {"credentials", reference}});
    QJsonArray stored;
    for (auto it = imported.secrets.cbegin(); it != imported.secrets.cend(); ++it) stored.append(secretLabel(it.key()));
    QJsonObject result{{"profileId", id}, {"label", label}, {"provider", providerId}, {"credentials", QJsonObject{{"mode", reference["mode"]}}},
                       {"storedValues", stored}, {"notes", QJsonArray::fromStringList(imported.notes)}};
    const auto expected = params["expectedRevision"].toString();
    QPointer<SettingsService> guarded(&settings);
    auto addProfile = [guarded, profiles, expected, result, reply, id](Error stored) mutable {
        if (stored) { reply(stored, {}); return; }
        if (!guarded) { reply({"operation_cancelled", "The application is stopping", {}}, {}); return; }
        guarded->patchAsync({{"profiles", profiles}}, expected, [guarded, result, reply, id](Result saved) mutable {
            if (saved.error) {
                // The stored values stay under this destination's own reference; importing again replaces them.
                saved.error.details["credentialsStored"] = true;
                reply(saved.error, {});
                return;
            }
            if (guarded) result["metadata"] = guarded->metadata();
            logging::info(logApp(), "Custom uploader imported", {{"profileId", id}, {"storedValues", result["storedValues"].toArray().size()}, {"credentialMode", result["credentials"].toObject()["mode"]}});
            reply({}, result);
        });
    };
    if (imported.secrets.isEmpty()) { addProfile(Error{}); return; }
    Credentials value;
    value.values = imported.secrets;
    credentials.save(id, reference, std::move(value), addProfile);
}
}
