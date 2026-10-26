#pragma once
// Launch at login: the user's Run key (HKCU\Software\Microsoft\Windows\CurrentVersion\Run),
// the standard per-user autostart mechanism, with a value put.gd manages.
#include "common/Error.h"
#include <string>
namespace putgd::startup {
Error apply(const std::string& executable, bool enabled);
bool enabled();
}
