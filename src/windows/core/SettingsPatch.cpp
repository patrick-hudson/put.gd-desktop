#include "core/SettingsPatch.h"
namespace putgd::settings {
json::Object applyPatch(const json::Object& document, const json::Object& patch) {
    auto result = document;
    for (const auto& pair : patch) {
        if (pair.second.isObject() && result.value(pair.first).isObject()) result[pair.first] = applyPatch(result.value(pair.first).objectRef(), pair.second.objectRef());
        else result[pair.first] = pair.second;
    }
    return result;
}
}
