#pragma once
namespace putgd {
// Compiled immutable identity. These functions perform no IO or allocation.
const char* buildVersion() noexcept;
const char* buildManifestJson() noexcept;
}
