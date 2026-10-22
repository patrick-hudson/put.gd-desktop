#include "core/UpdatePlan.h"
#include "common/Text.h"
#include <cctype>

namespace putgd::update {
namespace {
bool digits(std::string_view s, size_t minimum, size_t maximum) {
    if (s.size() < minimum || s.size() > maximum || s[0] == '0') return false;
    for (const char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}
// YY.MDD.N with the grammar of GenerateBuildInfo.cmake (the date itself was checked there).
bool calendar(std::string_view v) {
    const auto parts = text::split(v, '.');
    return parts.size() == 3 && digits(parts[0], 2, 2) && parts[0] >= "26" && digits(parts[1], 3, 4) && digits(parts[2], 1, 9);
}
// ^[A-Za-z0-9][A-Za-z0-9._+~-]{0,199}$, as the Linux installer checks release file names.
bool safeName(std::string_view name) {
    if (name.empty() || name.size() > 200 || !std::isalnum(static_cast<unsigned char>(name[0]))) return false;
    for (const char c : name) if (!std::isalnum(static_cast<unsigned char>(c)) && !text::containsChar("._+~-", c)) return false;
    return true;
}
}
std::string releaseChannel(const std::string& version) {
    if (text::endsWith(version, "-nightly")) return calendar(std::string_view(version).substr(0, version.size() - 8)) ? "nightly" : "";
    return calendar(version) ? "stable" : "";
}
std::string packageTarget(bool msi) { return msi ? "windows-msi" : "windows-zip"; }
std::string validBase(std::string base) {
    if (!text::endsWith(base, "/")) base += '/';
    const auto url = text::parseUrl(base);
    const bool loopback = url.scheme == "http" && (url.host == "127.0.0.1" || url.host == "localhost" || url.host == "::1" || url.host == "[::1]");
    if (!url.valid || url.host.empty() || url.hasQuery || url.hasFragment || !url.userInfo.empty() || (url.scheme != "https" && !loopback)) return {};
    return base;
}
Error releaseFile(const std::string& base, const std::string& tag, const std::string& name, std::string& url) {
    const auto checked = validBase(base);
    if (checked.empty()) return {"update_download_failed", "The release download location must be an https URL, or http on a loopback address", {}};
    if (!text::startsWith(tag, "v") || releaseChannel(tag.substr(1)).empty() || !safeName(name))
        return {"update_install_failed", "Invalid release file", {{"tag", tag.substr(0, 64)}, {"name", name.substr(0, 200)}}};
    url = checked + tag + "/" + name;
    return {};
}
std::string sumsLine(const std::string& sums, const std::string& name) {
    std::string found;
    for (auto line : text::split(sums, '\n')) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // "<64 hex> <space or *><name>"
        if (line.size() < 67 || line[64] != ' ' || (line[65] != ' ' && line[65] != '*') || line.substr(66) != name) continue;
        const auto digest = text::lower(line.substr(0, 64));
        if (!text::fromHex(digest)) continue;
        if (!found.empty()) return {};
        found = digest;
    }
    return found;
}
std::string trustedComment(const std::string& tag, const std::string& channel) { return "put.gd " + tag + " " + channel; }
Error releaseAsset(const json::Object& release, const std::string& target, json::Object& asset) {
    asset = release.value("assets")[target].toObject();
    const auto name = asset.value("name").toString();
    const auto suffix = target == "windows-msi" ? ".msi" : ".zip";
    if (asset.empty() || !safeName(name) || !text::endsWith(name, suffix))
        return {"update_install_unavailable", "The release names no Windows package for this installation", {{"reason", "no_package"}, {"packageTarget", target}}};
    if (!json::integer(asset.value("size"), 1, double(maximumPackageBytes)))
        return {"update_verification_failed", "The release's package has an implausible size", {{"size", asset.value("size")}}};
    if (asset.contains("sha256") && (!asset.value("sha256").isString() || asset.value("sha256").stringRef().size() != 64 || !text::fromHex(asset.value("sha256").stringRef())))
        return {"update_feed_invalid", "The update feed lists an invalid SHA-256", {}};
    return {};
}
bool sameDirectory(std::string a, std::string b) {
    for (auto* s : {&a, &b}) while (!s->empty() && (s->back() == '\\' || s->back() == '/')) s->pop_back();
    return !a.empty() && text::lower(text::replace(a, "/", "\\")) == text::lower(text::replace(b, "/", "\\"));
}
}
