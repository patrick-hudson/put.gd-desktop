// putgd.exe --edit-image: the Capture region & edit helper (docs/image-editor.md,
// "Process and privacy boundaries"; role "image-edit" in common/Protocol.h).
// It owns pixels and UI only: no settings, providers, credentials or history.
// The resident stages input/output files and supervises this process over the
// framed JSON channel on stdin/stdout; only the sanitized PNG leaves it.
#include "common/Files.h"
#include "common/Frames.h"
#include "common/Log.h"
#include "common/Modes.h"
#include "common/Paths.h"
#include "common/Sha256.h"
#include "common/Text.h"
#include "editor/EditDocument.h"
#include "editor/EditorCommands.h"
#include "editor/EditorWindow.h"
#include "editor/Export.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <windows.h>
#include <objidl.h>
#include <commctrl.h>
namespace Gdiplus { using std::max; using std::min; }
#include <gdiplus.h>

namespace putgd {
namespace {
using namespace editor;

constexpr UINT frameMessage = WM_APP + 10, submitMessage = WM_APP + 11, cancelMessage = WM_APP + 12, exportMessage = WM_APP + 13;
constexpr UINT_PTR startupTimer = 1;
constexpr int maximumPendingCommands = 16;
constexpr long long maximumImagePixels = 1000000000LL;
constexpr long long maximumOutputBytes = 1LL << 40;
constexpr int exitProtocol = 3;
constexpr wchar_t sinkClass[] = L"PutgdEditImageChannel";

[[noreturn]] void abortHelper(const char* reason) {
    // A protocol failure or a lost parent ends the helper at once; nothing is exported.
    log::warning("editor", reason);
    TerminateProcess(GetCurrentProcess(), exitProtocol);
    ExitProcess(exitProtocol);
}

// A non-negative integer sent as a JSON number or a decimal string.
bool count(const json::Value& value, long long minimum, long long maximum, long long& out) {
    if (value.isNumber()) {
        if (!json::integer(value, double(minimum), double(maximum))) return false;
        out = value.toInteger();
        return true;
    }
    if (!value.isString()) return false;
    const auto& s = value.stringRef();
    if (s.empty() || s.size() > 18 || (s.size() > 1 && s[0] == '0')) return false;
    for (const char c : s) if (c < '0' || c > '9') return false;
    out = std::stoll(s);
    return out >= minimum && out <= maximum;
}
bool sha256Text(const json::Value& value) {
    if (!value.isString() || value.stringRef().size() != 64) return false;
    for (const char c : value.stringRef()) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
std::string fullPath(const std::string& path) {
    const auto wide = paths::wide(path);
    const DWORD needed = GetFullPathNameW(wide.c_str(), 0, nullptr, nullptr);
    if (!needed) return {};
    std::wstring buffer(needed, L'\0');
    const DWORD written = GetFullPathNameW(wide.c_str(), needed, buffer.data(), nullptr);
    if (!written || written >= needed) return {};
    buffer.resize(written);
    return paths::narrow(buffer);
}
HANDLE openStaged(const std::string& path, DWORD access) {
    return CreateFileW(paths::wide(path).c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
}
bool plainFile(HANDLE handle, long long& size) {
    BY_HANDLE_FILE_INFORMATION info = {};
    if (!GetFileInformationByHandle(handle, &info)) return false;
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    if (info.nNumberOfLinks != 1) return false;
    size = (static_cast<long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    return true;
}

class Session {
public:
    int run();
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);

private:
    enum class Phase { Bootstrapped, Editing, Finalizing, Done };
    bool bootstrap(const protocol::Frame&);
    void readLoop();
    void heartbeatLoop();
    bool send(const std::string& type, const json::Object& payload = {});
    void onFrame(const protocol::Frame&);
    void activate();
    void onCommand(const json::Object&);
    json::Object execute(const std::string& name, const json::Object& params, std::string& code, std::string& message);
    Error saveCopyTo(const std::string& path, bool overwrite, json::Object* result);
    void submit();
    void exportNow();
    void cancel();
    bool writeOutput(const std::string& png);
    void checkpoint(json::Object payload);
    void checkpointFailure(const std::string& code, const std::string& message);
    void finish();
    std::vector<std::string> colors() const { return window_ ? window_->recentColors() : recentColors_; }

    HWND sink_ = nullptr;
    Phase phase_ = Phase::Bootstrapped;
    std::string nonce_, destination_, inputPath_, outputPath_, stagingDirectory_, inputBytes_;
    std::vector<std::string> recentColors_;
    long long imagePixels_ = 0, outputBytes_ = 0;
    HANDLE output_ = INVALID_HANDLE_VALUE;
    std::unique_ptr<EditDocument> document_;
    std::unique_ptr<EditorWindow> window_;
    std::mutex sendMutex_;
    bool heartbeats_ = false;
    std::atomic<int> pending_{0};
    HANDLE stopHeartbeat_ = nullptr;
    std::thread reader_, heartbeat_;
    int exitCode_ = exitProtocol;
    ULONGLONG started_ = 0;
};

LRESULT CALLBACK sinkProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
    auto* session = reinterpret_cast<Session*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (session && message >= WM_APP) return session->handle(message, wParam, lParam);
    if (session && message == WM_TIMER) return session->handle(message, wParam, lParam);
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool Session::send(const std::string& type, const json::Object& payload) {
    std::lock_guard lock(sendMutex_);
    if (type == "heartbeat" && !heartbeats_) return true;
    if (type == "checkpoint") heartbeats_ = false; // nothing follows the checkpoint
    if (!frames::helperChannel().send(type, payload)) abortHelper("The parent channel closed");
    return true;
}

bool Session::bootstrap(const protocol::Frame& frame) {
    const auto& p = frame.payload;
    if (frame.type != "bootstrap" || p.value("role") != "image-edit") return false;
    if (!p.value("nonce").isString() || p.value("nonce").stringRef().empty() || p.value("nonce").stringRef().size() > 128 || !text::printable(p.value("nonce").stringRef())) return false;
    if (!p.contains("generation")) return false;
    nonce_ = p.value("nonce").stringRef();
    const auto& input = p.value("input"), &output = p.value("output"), &limits = p.value("limits");
    long long inputSize = 0;
    if (!input.isObject() || !input["path"].isString() || !paths::isAbsolute(input["path"].stringRef()) || !count(input["bytes"], 1, maximumOutputBytes, inputSize) ||
        !sha256Text(input["sha256"]))
        return false;
    if (!output.isObject() || !output["path"].isString() || !paths::isAbsolute(output["path"].stringRef())) return false;
    if (!limits.isObject() || !count(limits["imagePixels"], 1, maximumImagePixels, imagePixels_) || !count(limits["outputBytes"], 1, maximumOutputBytes, outputBytes_)) return false;
    const auto& label = p.value("destinationLabel");
    if (!label.isString() || text::codePoints(label.stringRef()) < 0 || text::codePoints(label.stringRef()) > 200 || !text::printable(label.stringRef())) return false;
    destination_ = label.stringRef().empty() ? std::string("your destination") : label.stringRef();
    // At most six distinct lowercase #rrggbb strings, as the settings validator accepts.
    const auto& recent = p.value("recentColors");
    if (!recent.isArray() || recent.arrayRef().size() > 6) return false;
    for (const auto& color : recent.arrayRef()) {
        if (!color.isString() || !text::matchesColor(color.stringRef()) || std::find(recentColors_.begin(), recentColors_.end(), color.stringRef()) != recentColors_.end()) return false;
        recentColors_.push_back(color.stringRef());
    }
    if (!protocol::buildMatches(p.value("build").toObject())) log::warning("editor", "Build identity differs from the parent");

    inputPath_ = fullPath(input["path"].stringRef());
    outputPath_ = fullPath(output["path"].stringRef());
    if (inputPath_.empty() || outputPath_.empty() || text::lower(inputPath_) == text::lower(outputPath_)) return false;
    stagingDirectory_ = paths::parent(outputPath_);
    // The staged input: a plain file with the announced size and digest, read once.
    const long long maximumInput = std::min<long long>(imagePixels_ * 8 + 64LL * 1024 * 1024, maximumOutputBytes);
    HANDLE in = openStaged(inputPath_, GENERIC_READ);
    if (in == INVALID_HANDLE_VALUE) return false;
    long long size = 0;
    bool ok = plainFile(in, size) && size == inputSize && size <= maximumInput;
    if (ok) {
        try { inputBytes_.resize(size_t(size)); } catch (const std::bad_alloc&) { ok = false; }
        size_t offset = 0;
        while (ok && offset < inputBytes_.size()) {
            DWORD got = 0;
            ok = ReadFile(in, inputBytes_.data() + offset, DWORD(std::min<size_t>(inputBytes_.size() - offset, 1 << 20)), &got, nullptr) && got > 0;
            offset += got;
        }
    }
    CloseHandle(in);
    if (!ok || crypto::hex(crypto::Sha256::digest(inputBytes_)) != input["sha256"].stringRef()) { inputBytes_.clear(); return false; }
    // The staged output: an empty plain file, held open; its permissions stay as staged.
    output_ = openStaged(outputPath_, GENERIC_WRITE | GENERIC_READ);
    long long outputSize = -1;
    if (output_ == INVALID_HANDLE_VALUE || !plainFile(output_, outputSize) || outputSize != 0) return false;
    return true;
}

int Session::run() {
    started_ = GetTickCount64();
    auto& channel = frames::helperChannel();
    protocol::Frame first;
    std::string error;
    if (!channel.receive(first, protocol::editorStartupMs, &error)) { log::warning("editor", "No bootstrap", {{"reason", error}}); return exitProtocol; }
    if (!bootstrap(first)) { log::warning("editor", "Invalid bootstrap"); return exitProtocol; }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = &sinkProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = sinkClass;
    RegisterClassExW(&wc);
    // A message-only window receives channel frames, so they are also handled
    // while a modal dialog (colour picker, file chooser) runs its own loop.
    sink_ = CreateWindowExW(0, sinkClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), this);
    if (!sink_) return exitProtocol;
    send("ready", {{"nonce", nonce_}, {"build", protocol::buildIdentity()}});
    const auto elapsed = GetTickCount64() - started_;
    SetTimer(sink_, startupTimer, UINT(std::max<ULONGLONG>(1, protocol::editorStartupMs - std::min<ULONGLONG>(elapsed, protocol::editorStartupMs))), nullptr);
    stopHeartbeat_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    reader_ = std::thread([this] { readLoop(); });
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (window_ && window_->preTranslate(msg)) continue;
        if (window_ && window_->hwnd() && IsDialogMessageW(window_->hwnd(), &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (stopHeartbeat_) SetEvent(stopHeartbeat_);
    if (heartbeat_.joinable()) heartbeat_.join();
    if (reader_.joinable()) reader_.join(); // returned after checkpointAccepted
    window_.reset();
    document_.reset();
    if (output_ != INVALID_HANDLE_VALUE) CloseHandle(output_);
    if (sink_) DestroyWindow(sink_);
    if (stopHeartbeat_) CloseHandle(stopHeartbeat_);
    return exitCode_;
}

void Session::readLoop() {
    auto& channel = frames::helperChannel();
    while (true) {
        auto frame = std::make_unique<protocol::Frame>();
        std::string error;
        // Parent pipe EOF or a protocol violation: exit at once.
        if (!channel.receive(*frame, -1, &error)) abortHelper(error == "pipe_closed" ? "The parent channel closed" : "The parent sent an invalid frame");
        const bool last = frame->type == "checkpointAccepted";
        if (frame->type == "command") {
            const auto& id = frame->payload.value("requestId");
            if (id.isString() && !id.stringRef().empty() && id.stringRef().size() <= 64 && pending_.load() >= maximumPendingCommands) {
                send("commandResult", {{"requestId", id}, {"ok", false}, {"code", "editor_busy"}, {"message", "Too many editor commands are waiting"}});
                continue;
            }
            ++pending_;
        }
        if (!PostMessageW(sink_, frameMessage, 0, LPARAM(frame.get()))) abortHelper("The editor could not queue a frame");
        frame.release();
        if (last) return; // the parent may close the pipe now; that is not a failure
    }
}

void Session::heartbeatLoop() {
    while (WaitForSingleObject(stopHeartbeat_, protocol::heartbeatMs) == WAIT_TIMEOUT) send("heartbeat");
}

LRESULT Session::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case frameMessage: {
        std::unique_ptr<protocol::Frame> frame(reinterpret_cast<protocol::Frame*>(lParam));
        onFrame(*frame);
        return 0;
    }
    case submitMessage: submit(); return 0;
    case cancelMessage: cancel(); return 0;
    case exportMessage: exportNow(); return 0;
    case WM_TIMER:
        if (wParam == startupTimer) {
            KillTimer(sink_, startupTimer);
            if (phase_ == Phase::Bootstrapped) abortHelper("The editor was not activated in time");
        }
        return 0;
    default: return DefWindowProcW(sink_, message, wParam, lParam);
    }
}

void Session::onFrame(const protocol::Frame& frame) {
    const auto& p = frame.payload;
    if (p.contains("nonce") && p.value("nonce") != json::Value(nonce_)) abortHelper("Frame nonce mismatch");
    if (frame.type == "activate") {
        if (phase_ != Phase::Bootstrapped || document_) abortHelper("Unexpected activate");
        activate();
        return;
    }
    if (frame.type == "command") { --pending_; onCommand(p); return; }
    if (frame.type == "cancel") {
        if (phase_ == Phase::Bootstrapped) { phase_ = Phase::Editing; cancel(); }
        else if (phase_ == Phase::Editing) cancel();
        return;
    }
    if (frame.type == "checkpointAccepted") {
        if (phase_ != Phase::Done) abortHelper("Unexpected checkpointAccepted");
        finish();
        return;
    }
    abortHelper("Unexpected frame type");
}

void Session::activate() {
    KillTimer(sink_, startupTimer);
    {
        std::lock_guard lock(sendMutex_);
        heartbeats_ = true;
    }
    heartbeat_ = std::thread([this] { heartbeatLoop(); });
    long long outputSize = -1;
    if (!plainFile(output_, outputSize) || outputSize != 0) { phase_ = Phase::Editing; checkpointFailure("image_job_io", "The private output file changed"); return; }
    // Static PNG or JPEG only, chosen by signature and decoded exactly once here.
    Raster base;
    const auto error = exporter::decodeImage(inputBytes_, imagePixels_, base);
    std::string().swap(inputBytes_);
    if (error) { phase_ = Phase::Editing; checkpointFailure(error.code, error.message); return; }
    log::info("editor", "Editing", {{"width", base.width}, {"height", base.height}});
    document_ = std::make_unique<EditDocument>(std::move(base));
    EditorWindow::Callbacks callbacks;
    callbacks.upload = [this] { PostMessageW(sink_, submitMessage, 0, 0); };
    callbacks.cancel = [this] { PostMessageW(sink_, cancelMessage, 0, 0); };
    callbacks.saveCopy = [this](const std::string& path) { return saveCopyTo(path, true, nullptr); };
    window_ = std::make_unique<EditorWindow>(*document_, destination_, recentColors_, std::move(callbacks));
    if (!window_->create()) { window_.reset(); phase_ = Phase::Editing; checkpointFailure("edit_export_failed", "The editor window could not be created"); return; }
    phase_ = Phase::Editing;
    send("editing");
}

void Session::onCommand(const json::Object& p) {
    const auto& id = p.value("requestId");
    if (!id.isString() || id.stringRef().empty() || id.stringRef().size() > 64 || !p.value("command").isString() || !p.value("params").isObject())
        abortHelper("Malformed command frame");
    std::string code, message;
    auto result = execute(p.value("command").stringRef(), p.value("params").objectRef(), code, message);
    json::Object reply{{"requestId", id}, {"ok", code.empty()}};
    if (code.empty()) reply["result"] = std::move(result);
    else { reply["code"] = code; reply["message"] = message.substr(0, 512); }
    send("commandResult", reply);
}

json::Object Session::execute(const std::string& name, const json::Object& params, std::string& code, std::string& message) {
    auto denied = [&](const char* c, const std::string& m) { code = c; message = m; return json::Object{}; };
    if (phase_ == Phase::Bootstrapped || !document_ || !window_) {
        if (phase_ == Phase::Done) return denied("editor_finalizing", "The edit was already submitted or discarded");
        return denied("editor_starting", "The editor window is still opening");
    }
    const bool open = phase_ == Phase::Editing;
    const auto finalizing = [&] { return denied("editor_finalizing", "The edit was already submitted or discarded"); };
    if (name == "inspect") {
        if (!params.empty()) return denied("invalid_request", "inspect has no parameters");
        auto state = commands::state(*document_);
        state["phase"] = phase_ == Phase::Editing ? "editing" : phase_ == Phase::Finalizing ? "finalizing" : "closing";
        state["destination"] = destination_;
        return state;
    }
    if (name == "raise") {
        if (!params.empty()) return denied("invalid_request", "raise has no parameters");
        window_->raise();
        return {{"raised", true}};
    }
    if (name == "apply") {
        if (!open) return finalizing();
        if (params.size() != 1 || !params.value("commands").isArray()) return denied("invalid_request", "apply needs only a commands array");
        window_->abortGesture();
        json::Array created;
        const TextMeasurer measure = [](const std::string& t, const std::string& f, int px) { return exporter::measureText(t, f, px); };
        if (!commands::apply(*document_, params.value("commands").arrayRef(), measure, created, code, message)) return {};
        window_->documentEdited();
        return {{"created", created}, {"state", commands::state(*document_)}};
    }
    if (name == "preview") {
        int edge = commands::defaultPreviewEdge;
        if (!(params.empty() || (params.size() == 1 && params.contains("maxEdge")))) return denied("invalid_request", "preview accepts only maxEdge");
        if (params.contains("maxEdge")) {
            if (!json::integer(params.value("maxEdge"), commands::minimumPreviewEdge, commands::maximumPreviewEdge)) return denied("invalid_request", "maxEdge must be an integer from 64 to 1600");
            edge = params.value("maxEdge").toInt();
        }
        // The preview is drawn by the same sanitized render as Upload.
        std::string png;
        int width = 0, height = 0;
        if (const auto error = exporter::exportPreview(*document_, edge, png, width, height)) return denied("edit_export_failed", "The preview could not be rendered");
        if (png.size() > exporter::maximumPreviewBytes) return denied("preview_too_large", "Request a smaller maxEdge");
        const auto crop = document_->crop();
        return {{"mime", "image/png"}, {"png", text::base64(png)}, {"width", width}, {"height", height}, {"sourceWidth", crop.w}, {"sourceHeight", crop.h}};
    }
    if (name == "saveCopy") {
        if (!open) return finalizing();
        const auto& path = params.value("path");
        if (!path.isString() || params.size() > 2 || (params.size() == 2 && !params.value("overwrite").isBool()) || (params.size() == 2 && !params.contains("overwrite")))
            return denied("invalid_request", "saveCopy needs an absolute .png path and optional overwrite boolean");
        json::Object result;
        if (const auto error = saveCopyTo(path.stringRef(), params.value("overwrite").toBool(false), &result)) { code = error.code; message = error.message; return {}; }
        return result;
    }
    if (name == "fileDialog") {
        // #259: Save a copy… as the button opens it, its state, or a cancel; saveCopy saves to a path.
        const auto& action = params.value("action");
        if (params.size() != 1 || !action.isString() || (action.stringRef() != "open" && action.stringRef() != "inspect" && action.stringRef() != "cancel"))
            return denied("invalid_request", "fileDialog needs action open, inspect or cancel");
        if (action.stringRef() == "open") {
            if (!open) return finalizing();
            if (window_->saveCopyDialogOpen()) return denied("file_dialog_open", "Save a copy is already open");
            if (!window_->openSaveCopyDialog()) return denied("editor_busy", "The editor is busy; try again");
        }
        if (action.stringRef() == "cancel") window_->cancelSaveCopyDialog();
        return window_->saveCopyDialogState();
    }
    if (name == "submit") {
        if (!params.empty()) return denied("invalid_request", "submit has no parameters");
        if (!open) return finalizing();
        PostMessageW(sink_, submitMessage, 0, 0); // after this reply
        return {{"submitted", true}};
    }
    if (name == "cancel") {
        if (!params.empty()) return denied("invalid_request", "cancel has no parameters");
        if (!open) return finalizing();
        PostMessageW(sink_, cancelMessage, 0, 0);
        return {{"cancelled", true}};
    }
    return denied("invalid_request", "Unknown editor command");
}

Error Session::saveCopyTo(const std::string& requested, bool overwrite, json::Object* result) {
    auto invalid = [](const char* m) { return Error{"invalid_request", m, {}}; };
    if (!text::validUtf8(requested) || requested.size() > 4096 || requested.find('\0') != std::string::npos || !paths::isAbsolute(requested) ||
        !text::endsWith(text::lower(requested), ".png"))
        return invalid("saveCopy needs an absolute .png path and optional overwrite boolean");
    const auto path = fullPath(requested);
    if (path.empty() || paths::fileName(path).size() <= 4) return invalid("saveCopy needs an absolute .png path and optional overwrite boolean");
    // Never the private source, the handoff file or anything in editor staging.
    const auto lowered = text::lower(path);
    if (lowered == text::lower(inputPath_) || lowered == text::lower(outputPath_) || text::lower(paths::parent(path)) == text::lower(stagingDirectory_))
        return invalid("Choose a path outside private editor staging");
    const DWORD attributes = GetFileAttributesW(paths::wide(path).c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (!overwrite || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))))
        return {"file_exists", "The file exists; pass overwrite to replace it", {}};
    if (!files::isDirectory(paths::parent(path))) return {"save_failed", "The folder does not exist", {}};
    std::string png;
    int width = 0, height = 0;
    if (exporter::exportPng(*document_, SIZE_MAX, png, width, height)) return {"save_failed", "The copy could not be saved", {}};
    if (files::writeAtomically(path, png, false)) return {"save_failed", "The copy could not be saved", {}};
    if (result) *result = {{"saved", true}, {"path", path}, {"bytes", (long long)png.size()}, {"sha256", crypto::hex(crypto::Sha256::digest(png))}};
    return {};
}

void Session::submit() {
    if (phase_ != Phase::Editing || !window_) return;
    phase_ = Phase::Finalizing;
    window_->abortGesture();
    send("finalizing");
    window_->setBusy("Preparing the edited image…");
    // Let the busy state paint before the single full-resolution export.
    PostMessageW(sink_, exportMessage, 0, 0);
}

void Session::exportNow() {
    if (phase_ != Phase::Finalizing) return;
    std::string png;
    int width = 0, height = 0;
    if (const auto error = exporter::exportPng(*document_, size_t(outputBytes_), png, width, height)) {
        window_->setFailure("The edited image could not be prepared.");
        checkpointFailure("edit_export_failed", error.message);
        return;
    }
    if (!writeOutput(png)) {
        window_->setFailure("The edited image could not be handed off.");
        checkpointFailure("image_job_io", "The edited image could not be written");
        return;
    }
    const auto digest = crypto::hex(crypto::Sha256::digest(png));
    const long long bytes = (long long)png.size();
    std::string().swap(png);
    window_->setBusy("Handing off for upload…");
    checkpoint({{"bytes", bytes}, {"sha256", digest}, {"width", width}, {"height", height}});
}

void Session::cancel() {
    if (phase_ != Phase::Editing) return;
    if (window_) window_->setBusy("Discarding…");
    checkpointFailure("edit_cancelled", "Edit discarded; nothing was uploaded");
}

bool Session::writeOutput(const std::string& png) {
    // Written once to the staged handle, then flushed to disk.
    long long size = -1;
    if (output_ == INVALID_HANDLE_VALUE || !plainFile(output_, size) || size != 0) return false;
    LARGE_INTEGER zero = {};
    if (!SetFilePointerEx(output_, zero, nullptr, FILE_BEGIN)) return false;
    size_t offset = 0;
    while (offset < png.size()) {
        DWORD written = 0;
        if (!WriteFile(output_, png.data() + offset, DWORD(std::min<size_t>(png.size() - offset, 1 << 20)), &written, nullptr) || written == 0) return false;
        offset += written;
    }
    if (!FlushFileBuffers(output_)) return false;
    return plainFile(output_, size) && size == (long long)png.size();
}

void Session::checkpoint(json::Object payload) {
    payload["recentColors"] = json::stringArray(colors());
    phase_ = Phase::Done;
    if (stopHeartbeat_) SetEvent(stopHeartbeat_);
    send("checkpoint", payload);
}
void Session::checkpointFailure(const std::string& code, const std::string& message) { checkpoint({{"code", code}, {"message", message}}); }

void Session::finish() {
    exitCode_ = 0;
    if (output_ != INVALID_HANDLE_VALUE) { CloseHandle(output_); output_ = INVALID_HANDLE_VALUE; }
    if (window_ && window_->inModalLoop()) {
        // A dialog's own loop cannot be unwound from here; the handoff is complete.
        log::info("editor", "Handoff accepted");
        ExitProcess(0);
    }
    if (window_) { window_->allowClose(); ShowWindow(window_->hwnd(), SW_HIDE); }
    PostQuitMessage(0);
}
}

int editorMain(const std::vector<std::string>&) {
    // The channel first: it moves stdout aside so nothing else can write to it.
    frames::helperChannel();
    log::initialize("", "edit-image");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof controls;
    controls.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_UPDOWN_CLASS | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&controls);
    Gdiplus::GdiplusStartupInput startup;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok) { log::error("editor", "GDI+ is unavailable"); return exitProtocol; }
    int code = exitProtocol;
    {
        Session session;
        code = session.run();
    }
    Gdiplus::GdiplusShutdown(token);
    if (SUCCEEDED(com)) CoUninitialize();
    return code;
}
}
