#pragma once
// The recursive settings overlay used by settings.patch and MCP patch_configuration:
// objects merge, arrays and scalars replace, JSON null is an explicit value.
#include "common/Json.h"
namespace putgd::settings {
json::Object applyPatch(const json::Object& document, const json::Object& patch);
}
