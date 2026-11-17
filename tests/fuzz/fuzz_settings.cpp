// The configuration file (core/Settings.cpp), which the resident reloads whenever the
// file changes, and which a person or an agent may have edited into any shape.
// A document that validates must build settings and keep validating once normalized.
#include "FuzzSupport.h"
#include "core/Settings.h"
#include <QJsonDocument>
using namespace putgd;

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    qInstallMessageHandler(fuzz::quiet);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    fuzz::Input in(data, size);
    const auto mode = in.byte();
    const auto document = QJsonDocument::fromJson(in.rest());
    if (!document.isObject()) return 0;
    const auto object = document.object();
    // Profile validators the way the resident wires them: they may refuse any profile.
    const ProfileValidator profiles = (mode & 1) ? ProfileValidator([](const QString& id, const QJsonObject& p) -> Error {
        return p.size() % 3 == 0 && !id.isEmpty() ? Error{"profile_rejected", "x", {}} : Error{};
    }) : ProfileValidator{};
    const auto error = Settings::validate(object, profiles);
    if (!error) {
        const auto settings = Settings::fromValidated(object);
        Settings::validate(settings.document, profiles);
        for (const auto& action : Settings::invocableActionNames()) Settings::effectiveAction(object, action);
        settings.effectiveArtifactLimits();
    }
    Settings::normalized(object);
    QJsonArray order;
    for (const auto& key : object["profiles"].toObject().keys()) order.append(key);
    QJsonObject reordered;
    Settings::reorderProfiles(object, (mode & 2) ? QJsonArray{} : order, reordered);
    return 0;
}
