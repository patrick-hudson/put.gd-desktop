#pragma once
#include <cstdint>
struct xkb_context;
struct xkb_keymap;
namespace putgd {
// Consumes fd on every path. The returned map owns one reference; callers unref it.
xkb_keymap* readKdeKeyboardMap(xkb_context* context, uint32_t format, int fd, uint32_t size);
}
