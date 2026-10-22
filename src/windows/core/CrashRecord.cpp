#include "core/CrashRecord.h"
#include "common/Text.h"
#include "core/LogFormat.h"
#include <cstdlib>

namespace putgd::crash {
bool isRecordLine(std::string_view line) { return text::startsWith(line, "putgd-crash\t"); }
const char* exceptionName(unsigned long code) {
    switch (code) {
    case 0xC0000005UL: return "EXCEPTION_ACCESS_VIOLATION";
    case 0xC0000006UL: return "EXCEPTION_IN_PAGE_ERROR";
    case 0xC000001DUL: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case 0xC0000025UL: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
    case 0xC000008CUL: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case 0xC000008EUL: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case 0xC0000094UL: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case 0xC0000095UL: return "EXCEPTION_INT_OVERFLOW";
    case 0xC0000096UL: return "EXCEPTION_PRIV_INSTRUCTION";
    case 0xC00000FDUL: return "EXCEPTION_STACK_OVERFLOW";
    case 0xC0000409UL: return "STATUS_STACK_BUFFER_OVERRUN";
    case 0xC0000374UL: return "STATUS_HEAP_CORRUPTION";
    case 0x80000003UL: return "EXCEPTION_BREAKPOINT";
    case 0x20474343UL: return "GCC_EXCEPTION"; // an uncaught C++ exception (libgcc's SEH code)
    case 0xE06D7363UL: return "MSVC_CXX_EXCEPTION";
    default: return "";
    }
}
json::Object parseRecord(std::string_view textIn) {
    json::Object record; json::Array frames, modules;
    bool begun = false, complete = false;
    for (const auto& raw : text::split(textIn, '\n')) {
        auto parts = text::split(raw, '\t');
        if (parts.size() < 2 || parts[0] != "putgd-crash") continue;
        const auto& kind = parts[1];
        auto number = [&](size_t i) { return i < parts.size() ? double(std::strtoll(parts[i].c_str(), nullptr, 10)) : 0.0; };
        auto field = [&](size_t i) { return i < parts.size() ? parts[i].substr(0, 512) : std::string(); };
        if (kind == "begin" && parts.size() >= 6) {
            begun = true;
            record["recordVersion"] = number(2); record["process"] = field(3); record["pid"] = number(4); record["thread"] = number(5);
        } else if (!begun) continue;
        else if (kind == "version") record["version"] = field(2);
        else if (kind == "exception") { record["exceptionCode"] = field(2); record["exceptionName"] = field(3); record["faultAddress"] = field(4); record["access"] = field(5); }
        else if (kind == "time") record["time"] = logformat::iso((long long)number(2) * 1000);
        else if (kind == "pc") record["pc"] = field(2);
        else if (kind == "frame" && parts.size() >= 6 && frames.size() < 256) {
            json::Object frame{{"index", number(2)}, {"address", field(3)}, {"offset", field(5)}};
            frame["module"] = parts[4] == "-" ? json::Value() : json::Value(number(4));
            frames.push_back(frame);
        } else if (kind == "module" && parts.size() >= 6 && modules.size() < 256) modules.push_back(json::Object{{"index", number(2)}, {"loadAddress", field(3)}, {"imageId", field(4)}, {"file", field(5)}});
        else if (kind == "end") { complete = true; break; }
    }
    if (!begun) return {};
    record["frames"] = frames; record["modules"] = modules; record["complete"] = complete;
    return record;
}
}
