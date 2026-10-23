#pragma once
// Structural PNG check shared by the editor export and the resident's handoff
// acceptance (docs/image-editor.md): only IHDR, PLTE, tRNS, gAMA, cHRM, sRGB,
// pHYs, IDAT and a final IEND, valid CRCs, no trailing bytes.
#include "common/Error.h"
#include <string>
#include <string_view>
namespace putgd::png {
inline constexpr std::string_view signature{"\x89PNG\r\n\x1a\n", 8};
bool hasSignature(std::string_view bytes);
// Width/height from IHDR; false when the header is malformed.
bool dimensions(std::string_view bytes, int& width, int& height);
Error inspect(std::string_view bytes, int* width = nullptr, int* height = nullptr);
// Drops every chunk outside the allow list (text, EXIF, ICC, time, private) and
// trailing bytes; chunk CRCs are preserved. Empty when the input is not a PNG.
std::string stripDisallowedChunks(std::string_view bytes);
unsigned long crc32(std::string_view bytes);
}
