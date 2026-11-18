// The custom uploader destination (src/storage/http, #1361): a ShareX .sxcu file read from
// disk by settings.importUploaderFile, the destination settings rules it feeds, the
// placeholder engine that reads a server's reply, and the text scrubbing that keeps stored
// credentials out of what is shown. A person picks the file, but anything can be in it.
// The first byte selects the entry point.
#include "FuzzSupport.h"
#include "core/SettingsService.h"
#include "secrets/CredentialStore.h"
#include "storage/http/HttpCommon.h"
#include "storage/http/HttpImport.h"
#include "storage/http/HttpTemplate.h"
#include <QDir>
#include <cctype>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QUrl>
using namespace putgd;
using namespace putgd::http;

namespace {
// What settings.importUploaderFile runs on: a configuration file of its own in a
// directory of its own (several workers run at once), and a credential store that holds
// session values only.
struct Resident {
    QTemporaryDir directory;
    SettingsService settings{directory.filePath("config.json")};
    CredentialStore credentials;
    QString file = directory.filePath("uploader.sxcu");
    QJsonObject initial = SettingsService::defaults();
    Resident() {
        const auto saved = settings.save(initial, {});
        if (saved.error) fuzz::bug("the starting configuration was refused");
    }
};
Resident* resident = nullptr;
}

extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv) {
    fuzz::application(argc, argv);
    resident = new Resident;
    return 0;
}

namespace {
// A value that looks like a credential: long, one word, with a letter and a digit, and no
// placeholder in it. The reader names such a value in a stored secret and nowhere else.
bool credentialLike(const QString& value) {
    if (value.size() < 20 || value.size() > 200) return false;
    bool letter = false, digit = false;
    for (const auto c : value) {
        if (c.isLetter() && c.unicode() < 128) letter = true;
        else if (c.isDigit() && c.unicode() < 128) digit = true;
        else if (c != '-' && c != '_') return false;
    }
    return letter && digit;
}

QStringList credentialValues(const QByteArray& json) {
    QStringList out;
    const auto object = QJsonDocument::fromJson(json).object();
    for (auto it = object.begin(); it != object.end(); ++it) {
        const auto key = it.key().toLower();
        if (key != "headers" && key != "parameters" && key != "arguments") continue;
        const auto rows = it.value().toObject();
        for (auto row = rows.begin(); row != rows.end(); ++row)
            if (row.value().isString() && credentialLike(row.value().toString())) out.append(row.value().toString());
    }
    return out;
}

void sxcu(const QByteArray& json) {
    const auto imported = importSxcu(json);
    const auto values = credentialValues(json);
    if (imported.error) {
        // A refused file hands nothing on.
        FUZZ_REQUIRE(imported.secrets.isEmpty() && imported.settings.isEmpty() && imported.notes.isEmpty());
        const auto shown = imported.error.message + QJsonDocument(imported.error.details).toJson();
        for (const auto& value : values) FUZZ_REQUIRE(!shown.contains(value));
        return;
    }
    // Whatever was read as a secret is named by the settings, and a credential-like secret,
    // once stored, stands nowhere in the settings, the notes or the destination's name,
    // which are what is kept in the configuration and shown. (As a whole word: a value
    // that merely begins like another is not that value.)
    // An address is read from its first "/" on: what stands between the scheme and that
    // (user information, a port) is no part of a path or a query, and the reader leaves it.
    auto kept = imported.settings;
    for (const auto* key : {"requestUrl", "responseUrl", "thumbnailUrl", "deletionUrl"}) {
        const auto address = kept[key].toString();
        const auto scheme = address.indexOf("://");
        const auto path = scheme < 0 ? -1 : address.indexOf('/', scheme + 3);
        kept[key] = scheme < 0 ? address : path < 0 ? QString() : address.mid(path);
    }
    const auto shown = QJsonDocument(kept).toJson(QJsonDocument::Compact) + imported.notes.join('\n').toUtf8() + imported.name.toUtf8();
    const auto leaked = [&](const QString& value) {
        const auto needle = value.toUtf8();
        const auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'; };
        for (qsizetype at = shown.indexOf(needle); at >= 0; at = shown.indexOf(needle, at + 1)) {
            const auto end = at + needle.size();
            if ((at > 0 && word(shown[at - 1])) || (end < shown.size() && word(shown[end]))) continue;
            std::fprintf(stderr, "settings, notes and name: %s\nsecret: %s\n", shown.constData(), needle.constData());
            fuzz::bug("a secret value is in the settings, notes or name");
        }
    };
    const auto keys = secretKeys(imported.settings);
    for (auto it = imported.secrets.cbegin(); it != imported.secrets.cend(); ++it) {
        FUZZ_REQUIRE(keys.contains(it.key()));
        // The keys the import takes out of an address ("secrets/...") are left out until #1570.
        if (!it.key().startsWith("secrets/") && credentialLike(it.value())) leaked(it.value());
    }
    // What the import hands to importUploaderFile is what the destination's own rules take,
    // once the caller has allowed plain HTTP, which a file may need and may not decide for
    // itself (a refusal there is a reply to the person, so this is a property of the reader).
    auto allowed = imported.settings;
    allowed["allowInsecureHttp"] = true;
    const auto invalid = validateSettings(allowed);
    if (invalid) std::fprintf(stderr, "refused: %s\n%s\n", invalid.message.toUtf8().constData(), shown.constData());
    FUZZ_REQUIRE(!invalid);
    QStringList names;
    FUZZ_REQUIRE(!namedCredentials({{"provider", providerId}, {"settings", imported.settings}}, imported.secrets, &names));
    FUZZ_REQUIRE(names == keys);
}

void templates(fuzz::Input& in) {
    const auto text = QString::fromUtf8(in.part());
    const auto body = QString::fromUtf8(in.part());
    const auto path = QString::fromUtf8(in.part());
    QString problem;
    parseTemplate(text, &problem);
    templateProblem(text, TemplateUse::Request);
    templateProblem(text, TemplateUse::Response);
    usesOldSyntax(text);
    const auto migrated = migrateOldSyntax(text, QStringList{path, body});
    templateProblem(migrated, TemplateUse::Response);
    secretReferences(text);
    RequestValues request;
    request.fileName = path;
    request.input = body;
    request.hasInput = in.byte() & 1;
    request.encode = RequestValues::Encode(in.byte() % 4);
    request.sampleSecrets = in.byte() & 1;
    renderRequest(text, request, &problem);
    QHash<QString, QString> stored{{"arguments/token", path}, {"secrets/url-key", body}};
    request.secrets = &stored;
    renderRequest(text, request, &problem);
    ResponseValues reply;
    reply.body = body;
    reply.headers = {{"Location", path.toUtf8()}, {"X-Name", body.toUtf8()}};
    reply.responseUrl = path;
    reply.fileName = text;
    renderResponse(text, reply);
    reply.keepSecrets = true;
    reply.secrets = &stored;
    renderResponse(text, reply);
    // Literal text written for a template renders back as itself.
    const auto escaped = escapeTemplate(text);
    FUZZ_REQUIRE(templateProblem(escaped, TemplateUse::Request).isEmpty());
    FUZZ_REQUIRE(renderResponse(escaped, ResponseValues{}) == text);
    RequestValues plain;
    QString lost;
    FUZZ_REQUIRE(renderRequest(escaped, plain, &lost) == text);
    jsonPathSupported(path);
    jsonPathValue(body, path);
    xpathSupported(path);
    xpathValue(body, path);
}

void text(fuzz::Input& in) {
    const auto first = QString::fromUtf8(in.part());
    const auto second = QString::fromUtf8(in.part());
    const auto value = QString::fromUtf8(in.part());
    const auto maximum = 1 + int(in.number(599)); // a caller asks for at least one character
    const auto line = singleLine(first);
    for (const auto c : line) FUZZ_REQUIRE(c.unicode() >= 0x20 && !(c.unicode() >= 0x7f && c.unicode() < 0xa0));
    FUZZ_REQUIRE(boundedMessage(first, maximum).size() <= std::max(maximum, 1));
    QHash<QString, QString> stored{{"headers/Authorization", value}, {"arguments/folder", second.left(8)}};
    const auto scrubbed = scrubStoredValues(first, stored);
    // A server's text, with a stored credential in it, never comes back holding it.
    if (credentialLike(value) && !value.contains("redacted") && !value.contains("withheld")) FUZZ_REQUIRE(!scrubbed.contains(value));
    storedValueIn(first, stored);
    nameStoredValues(first, stored);
    const QUrl address(first, QUrl::TolerantMode), origin(second, QUrl::TolerantMode);
    returnedAddressProblem(address, origin);
    transportAllowed(address, in.byte() & 1);
    sameHost(address, origin);
    loopbackHost(first);
    loopbackAddress(first);
    leadsToThisMachine(first, {second}, {value});
    requestOrigin(QJsonObject{{"requestUrl", first}});
    artifactKind(first, second);
    QList<QPair<QByteArray, QByteArray>> headers{{first.toUtf8(), second.toUtf8()}, {"Content-Length", value.toUtf8()}};
    incompleteReply(int(in.number(600)), in.byte() & 1 ? "HEAD" : "GET", headers, qint64(in.number(65535)));
}

// settings.importUploaderFile end to end: the parameters as a client sends them, the file as
// the person picked it, the answer, and the configuration it leaves.
void importFile(fuzz::Input& in) {
    const auto flags = in.byte();
    const auto document = QJsonDocument::fromJson(in.part());
    const auto bytes = in.rest();
    auto& r = *resident;
    {
        QFile out(r.file);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) fuzz::bug("scratch file");
        out.write(bytes);
    }
    // The destinations already there are all the 32 it may hold: start over.
    if (r.settings.document()["profiles"].toArray().size() >= 8 && !r.settings.save(r.initial, r.settings.revision()).error) {}
    QJsonObject params;
    if (flags & 1) params = {{"path", r.file}, {"expectedRevision", r.settings.revision()}, {"credentialMode", "session"}};
    if (flags & 2) params["allowInsecureHttp"] = true;
    const auto given = document.object();
    for (auto it = given.begin(); it != given.end(); ++it) {
        // The path is the file the fuzzer wrote, or one it names that cannot hold a secret of ours.
        if (it.key() == "path" && (flags & 4)) continue;
        params[it.key()] = it.value();
    }
    int answers = 0;
    Error error;
    QJsonObject result;
    http::importUploaderFile(r.settings, r.credentials, params, [&](Error e, QJsonObject o) { ++answers; error = e; result = o; }, std::chrono::milliseconds(1000));
    QElapsedTimer waited;
    waited.start();
    while (!answers && waited.elapsed() < 3000) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    FUZZ_REQUIRE(answers == 1);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
    FUZZ_REQUIRE(answers == 1);
    // The answer names the profile and the kinds of value stored, never a value.
    const auto shown = error.message + QJsonDocument(error.details).toJson() + QJsonDocument(result).toJson();
    for (const auto& value : credentialValues(bytes)) FUZZ_REQUIRE(!shown.contains(value));
    if (!error) {
        FUZZ_REQUIRE(result["profileId"].isString());
        bool found = false;
        for (const auto& profile : r.settings.document()["profiles"].toArray()) found |= profile.toObject()["id"] == result["profileId"];
        FUZZ_REQUIRE(found);
    }
}

void settingsRules(const QByteArray& json) {
    const auto document = QJsonDocument::fromJson(json);
    if (!document.isObject()) return;
    auto object = document.object();
    // As the form saves them: the fields over the defaults, and as given.
    auto merged = defaults();
    for (auto it = object.begin(); it != object.end(); ++it) merged[it.key()] = it.value();
    for (const auto& candidate : {object, merged}) {
        const auto error = validateSettings(candidate);
        const auto keys = secretKeys(candidate);
        if (error) continue;
        QMap<QString, QString> values;
        for (const auto& key : keys) values.insert(key, QStringLiteral("value"));
        for (const auto& key : keys) secretLabel(key);
        acceptsKind(candidate, "image");
        acceptsKind(candidate, "file");
        requestOrigin(candidate);
        QStringList names;
        // A destination that validates has a complete set of stored values that its own rules accept,
        // unless a value they check (a placeholder) is the reason.
        namedCredentials({{"provider", providerId}, {"settings", candidate}}, values, &names);
        FUZZ_REQUIRE(names == keys);
    }
}
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    fuzz::Input in(data, size);
    switch (in.byte() % 5) {
    case 0: sxcu(in.rest()); break;
    case 1: settingsRules(in.rest()); break;
    case 2: templates(in); break;
    case 3: text(in); break;
    default: importFile(in); break;
    }
    return 0;
}
