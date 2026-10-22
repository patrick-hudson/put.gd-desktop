#include "core/HistoryFilters.h"
#include "common/Text.h"
#include <cctype>
#include <cstdio>

namespace putgd::historyfilter {
namespace {
bool leap(int year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }
int daysIn(int year, int month) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && leap(year) ? 29 : days[month - 1];
}
// RFC 6838 restricted-name characters.
bool tokenChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '!' || c == '#' || c == '$' || c == '&' || c == '-' || c == '^' || c == '_' || c == '.' || c == '+'; }
}

std::optional<Day> parseDay(const std::string& raw) {
    const auto value = text::trim(raw);
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') return std::nullopt;
    for (size_t i : {0, 1, 2, 3, 5, 6, 8, 9}) if (!std::isdigit(static_cast<unsigned char>(value[i]))) return std::nullopt;
    Day day{std::stoi(value.substr(0, 4)), std::stoi(value.substr(5, 2)), std::stoi(value.substr(8, 2))};
    if (day.year < 1970 || day.month < 1 || day.month > 12 || day.day < 1 || day.day > daysIn(day.year, day.month)) return std::nullopt;
    return day;
}

std::string isoUtc(int year, int month, int day, int hour, int minute, int second, int millisecond) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", year, month, day, hour, minute, second, millisecond);
    return buffer;
}

std::optional<std::string> normalizeMime(const std::string& raw) {
    const auto value = text::lower(text::trim(raw));
    const auto slash = value.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= value.size() || value.size() > 127) return std::nullopt;
    for (size_t i = 0; i < value.size(); ++i) if (i != slash && !tokenChar(value[i])) return std::nullopt;
    return value;
}
}
