#pragma once
#include <string>
namespace putgd::uuid {
// Random UUID without braces, lowercase: 8-4-4-4-12.
std::string create();
bool valid(const std::string&);
}
