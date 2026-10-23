#include "resident/Clipboard.h"
#include "common/Log.h"
#include "common/Paths.h"
#include <shellapi.h>
#include <shlobj.h>

namespace putgd {
Clipboard::Clipboard(HWND owner) : owner_(owner) {
    pngFormat_ = RegisterClipboardFormatW(L"PNG");
    AddClipboardFormatListener(owner_);
}
Clipboard::~Clipboard() { RemoveClipboardFormatListener(owner_); }
unsigned long long Clipboard::revision() const { return GetClipboardSequenceNumber(); }
bool Clipboard::open() {
    for (int attempt = 0; attempt < 20; ++attempt) { if (OpenClipboard(owner_)) return true; Sleep(10); }
    return false;
}
std::vector<std::string> Clipboard::formats() const {
    std::vector<std::string> result;
    if (IsClipboardFormatAvailable(CF_HDROP)) result.push_back("text/uri-list");
    if ((pngFormat_ && IsClipboardFormatAvailable(pngFormat_)) || IsClipboardFormatAvailable(CF_DIBV5) || IsClipboardFormatAvailable(CF_DIB)) result.push_back("image/png");
    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) result.push_back("text/plain");
    return result;
}
namespace {
std::string globalBytes(HANDLE handle, size_t maximum, bool& tooLarge) {
    tooLarge = false;
    const SIZE_T size = GlobalSize(handle);
    if (size > maximum) { tooLarge = true; return {}; }
    const void* data = GlobalLock(handle);
    if (!data) return {};
    std::string bytes(static_cast<const char*>(data), size);
    GlobalUnlock(handle);
    return bytes;
}
HGLOBAL globalFrom(const std::string& bytes) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes.size() ? bytes.size() : 1);
    if (!h) return nullptr;
    void* p = GlobalLock(h);
    if (!p) { GlobalFree(h); return nullptr; }
    memcpy(p, bytes.data(), bytes.size());
    GlobalUnlock(h);
    return h;
}
}
ClipboardOffer Clipboard::read(long long maximumTextBytes, long long maximumImageBytes, int maximumFiles, Error& error) {
    ClipboardOffer offer;
    offer.revision = revision();
    if (!open()) { error = {"clipboard_busy", "The clipboard is in use by another application", {}}; return offer; }
    auto fail = [&](Error e) { CloseClipboard(); error = std::move(e); return offer; };
    if (IsClipboardFormatAvailable(CF_HDROP)) {
        if (HANDLE h = GetClipboardData(CF_HDROP)) {
            auto* drop = static_cast<HDROP>(h);
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            if (count > UINT(maximumFiles)) return fail({"input_excessive", "Too many files on the clipboard", {}});
            for (UINT i = 0; i < count; ++i) {
                std::wstring path(32768, L'\0');
                const UINT n = DragQueryFileW(drop, i, path.data(), 32767);
                offer.files.push_back(paths::narrow(std::wstring(path.data(), n)));
            }
            if (!offer.files.empty()) offer.kind = "files";
        }
    }
    if (offer.kind.empty() && pngFormat_ && IsClipboardFormatAvailable(pngFormat_)) {
        bool tooLarge = false;
        if (HANDLE h = GetClipboardData(pngFormat_)) offer.bytes = globalBytes(h, size_t(maximumImageBytes), tooLarge);
        if (tooLarge) return fail({"input_excessive", "Clipboard image exceeds the configured size", {}});
        if (!offer.bytes.empty()) { offer.kind = "image"; offer.imageFormat = "png"; }
    }
    if (offer.kind.empty() && (IsClipboardFormatAvailable(CF_DIBV5) || IsClipboardFormatAvailable(CF_DIB))) {
        bool tooLarge = false;
        if (HANDLE h = GetClipboardData(IsClipboardFormatAvailable(CF_DIBV5) ? CF_DIBV5 : CF_DIB)) offer.bytes = globalBytes(h, size_t(maximumImageBytes), tooLarge);
        if (tooLarge) return fail({"input_excessive", "Clipboard image exceeds the configured size", {}});
        if (!offer.bytes.empty()) { offer.kind = "image"; offer.imageFormat = "dib"; }
    }
    if (offer.kind.empty() && IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            const SIZE_T size = GlobalSize(h);
            if (size > SIZE_T(maximumTextBytes) * 2 + 2) return fail({"input_excessive", "Clipboard text exceeds the configured size", {}});
            if (const auto* data = static_cast<const wchar_t*>(GlobalLock(h))) { offer.bytes = paths::narrow(std::wstring(data, wcsnlen(data, size / 2))); GlobalUnlock(h); }
            if (static_cast<long long>(offer.bytes.size()) > maximumTextBytes) return fail({"input_excessive", "Clipboard text exceeds the configured size", {}});
            if (!offer.bytes.empty()) offer.kind = "text";
        }
    }
    CloseClipboard();
    if (offer.kind.empty()) { offer.kind = "none"; error = {"source_empty", "The clipboard holds no files, image or text", {}}; }
    return offer;
}
Error Clipboard::writeText(const std::string& utf8, unsigned long long& revision) {
    if (!open()) return {"clipboard_busy", "The clipboard is in use by another application", {}};
    EmptyClipboard();
    const auto wide = paths::wide(utf8);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t));
    bool ok = false;
    if (h) {
        if (auto* p = static_cast<wchar_t*>(GlobalLock(h))) { memcpy(p, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t)); GlobalUnlock(h); ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr; }
        if (!ok) GlobalFree(h);
    }
    CloseClipboard();
    if (!ok) return {"copy_failed", "Cannot write the clipboard", {}};
    revision = this->revision();
    lastCopy_ = revision;
    return {};
}
Error Clipboard::writeImage(const std::string& png, const std::string& dib, unsigned long long& revision) {
    if (!open()) return {"clipboard_busy", "The clipboard is in use by another application", {}};
    EmptyClipboard();
    bool ok = false;
    if (!png.empty() && pngFormat_) { if (HGLOBAL h = globalFrom(png)) { if (SetClipboardData(pngFormat_, h)) ok = true; else GlobalFree(h); } }
    if (!dib.empty()) { if (HGLOBAL h = globalFrom(dib)) { if (SetClipboardData(CF_DIBV5, h)) ok = true; else GlobalFree(h); } }
    CloseClipboard();
    if (!ok) return {"copy_failed", "Cannot write the clipboard", {}};
    revision = this->revision();
    lastCopy_ = revision;
    return {};
}
Error Clipboard::writeFiles(const std::vector<std::string>& files, unsigned long long& revision) {
    if (!open()) return {"clipboard_busy", "The clipboard is in use by another application", {}};
    EmptyClipboard();
    std::wstring list;
    for (const auto& f : files) { list += paths::wide(f); list += L'\0'; }
    list += L'\0';
    const size_t bytes = sizeof(DROPFILES) + list.size() * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (h) {
        if (auto* drop = static_cast<DROPFILES*>(GlobalLock(h))) {
            memset(drop, 0, sizeof(DROPFILES)); drop->pFiles = sizeof(DROPFILES); drop->fWide = TRUE;
            memcpy(reinterpret_cast<char*>(drop) + sizeof(DROPFILES), list.data(), list.size() * sizeof(wchar_t));
            GlobalUnlock(h);
            ok = SetClipboardData(CF_HDROP, h) != nullptr;
        }
        if (!ok) GlobalFree(h);
    }
    CloseClipboard();
    if (!ok) return {"copy_failed", "Cannot write the clipboard", {}};
    revision = this->revision();
    lastCopy_ = revision;
    return {};
}
void Clipboard::handleUpdate() {
    const auto now = revision();
    if (lastCopy_ && *lastCopy_ == now) return;
    if (userChange_) userChange_(now);
}
}
