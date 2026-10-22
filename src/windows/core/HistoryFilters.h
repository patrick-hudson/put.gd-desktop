#pragma once
// History's date and content-type filters (history.list from, to and mime). The
// window takes calendar days in the user's time zone and sends the UTC instants
// that bound them, in the journal's ISO form, so string comparison is exact.
#include <optional>
#include <string>

namespace putgd::historyfilter {
struct Day { int year = 0, month = 0, day = 0; };
// "YYYY-MM-DD" for a real calendar day (years 1970-9999); nothing otherwise.
std::optional<Day> parseDay(const std::string& text);
// "YYYY-MM-DDTHH:MM:SS.mmmZ", the form files::nowIsoUtc writes into the journal.
std::string isoUtc(int year, int month, int day, int hour, int minute, int second, int millisecond);
// A content type such as image/png, lower-cased; nothing for anything else.
std::optional<std::string> normalizeMime(const std::string& text);
}
