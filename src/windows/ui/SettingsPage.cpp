// Settings: Capture (default output and per-action exceptions), General (local
// copies, file names, clipboard, startup, appearance), Shortcuts (recorded on the
// control channel) and Advanced (limits, retention, logs, updates, diagnostics,
// config file). Valid edits autosave through the window's settings.patch queue;
// invalid text stays local with a message next to the field.
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "common/Paths.h"
#include "common/Shortcut.h"
#include "common/Text.h"
#include "core/DiagnosticsPackage.h"
#include "core/Naming.h"
#include "core/Settings.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <optional>

namespace putgd::ui {
namespace {
enum : int {
    idSections = 100, idDefaultDestination, idDefaultSharing, idKeepLocal, idNamingMode, idCollision, idClipboard,
    idNotifications, idLaunch, idUnlockStartup, idTheme, idDebug, idClearLogs, idDiagnostics, idShowDiagnostics,
    idUpdateCheck, idUpdateChannel, idCheckNow, idReload, idResetLimits, idCrashDumps, idDiagnosticsUpload,
    idActionDestination = 200, idActionSharing = 220, idRecord = 240, idClear = 260, idShortcutEdit = 280,
    idField = 300, idBrowse = 400
};
constexpr UINT_PTR timerRecord = 1;
enum Section { Capture = 0, General = 1, Shortcuts = 2, Advanced = 3 };
const wchar_t* sectionNames[] = {L"Capture", L"General", L"Shortcuts", L"Advanced"};
const char* sectionKeys[] = {"capture", "general", "shortcuts", "advanced"};
const std::vector<std::string> shortcutActions{"region", "edit", "window", "desktop", "clipboard", "record"};
constexpr long long tebibyte = 1099511627776LL;

enum class Kind { Folder, OptionalFolder, Template, Integer, OptionalInteger, Bytes, ByteLimit, Megapixels };
struct Spec {
    std::string path;
    Kind kind;
    Section section;
    const wchar_t* caption;
    long long minimum = 0, maximum = 0;
    const wchar_t* cue = L"";
    long long fallback = 0; // displayed when an optional key is absent
};
const std::vector<Spec> specs{
    {"/archive/root", Kind::Folder, General, L"Main folder"},
    {"/archive/folders/images", Kind::OptionalFolder, General, L"Images folder"},
    {"/archive/folders/videos", Kind::OptionalFolder, General, L"Videos folder"},
    {"/archive/folders/text", Kind::OptionalFolder, General, L"Text folder"},
    {"/archive/folders/other", Kind::OptionalFolder, General, L"Other files folder"},
    {"/naming/captureTemplate", Kind::Template, General, L"Screen captures", 0, 0, L"Screen-Capture-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext}"},
    {"/naming/imageTemplate", Kind::Template, General, L"Clipboard images", 0, 0, L"Image-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext}"},
    {"/naming/textTemplate", Kind::Template, General, L"Clipboard text", 0, 0, L"Text-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext}"},
    {"/naming/fileTemplate", Kind::Template, General, L"Copied & uploaded files", 0, 0, L"{name}.{ext}"},
    {"/limits/maxFileBytes", Kind::ByteLimit, Advanced, L"Largest file", 1, tebibyte, L"No limit"},
    {"/limits/maxBatchBytes", Kind::ByteLimit, Advanced, L"Largest batch", 1, tebibyte, L"No limit"},
    {"/limits/pendingArtifactBytes", Kind::ByteLimit, Advanced, L"Waiting uploads", 1, tebibyte, L"No limit"},
    {"/limits/maxFiles", Kind::Integer, Advanced, L"Files in a batch", 1, 10000},
    {"/limits/concurrentUploads", Kind::Integer, Advanced, L"Uploads at once", 1, 32},
    {"/limits/pendingOperations", Kind::Integer, Advanced, L"Waiting operations", 1, 1024},
    {"/limits/clipboardTextBytes", Kind::Bytes, Advanced, L"Clipboard text limit", 1, tebibyte},
    {"/limits/imageBytes", Kind::Bytes, Advanced, L"Largest image", 1, tebibyte},
    {"/limits/imagePixels", Kind::Megapixels, Advanced, L"Largest image size (megapixels)", 1, 1000000000},
    {"/limits/pendingImageBytes", Kind::Bytes, Advanced, L"Memory for queued images", 1, tebibyte, L"", 67108864LL},
    {"/retention/failedArtifactDays", Kind::Integer, Advanced, L"Keep failed uploads for (days)", 0, 36500},
    {"/retention/recentActivityLimit", Kind::Integer, Advanced, L"Items in recent list", 1, 10000},
    {"/retention/previewCacheBytes", Kind::Bytes, Advanced, L"Thumbnail cache", 0, tebibyte},
    {"/retention/historyRetentionDays", Kind::OptionalInteger, Advanced, L"Keep history for (days)", 1, 36500, L"Forever"},
    {"/retention/logDays", Kind::Integer, Advanced, L"Keep logs for (days)", 1, 365, L"", 14},
    {"/retention/logBytes", Kind::Bytes, Advanced, L"Log folder size", 1048576, 1073741824, L"", 52428800LL},
};
bool limitField(const std::string& path) { return text::startsWith(path, "/limits/"); }

json::Value at(const json::Object& document, const std::string& path) {
    const auto parts = text::split(path.substr(1), '/');
    json::Value current(document);
    for (const auto& part : parts) {
        if (!current.isObject()) return {};
        json::Value next = current.objectRef().value(part);
        current = std::move(next);
    }
    return current;
}
bool present(const json::Object& document, const std::string& path) {
    const auto parts = text::split(path.substr(1), '/');
    const json::Object* object = &document;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (!object->contains(parts[i])) return false;
        if (i + 1 < parts.size()) { if (!object->value(parts[i]).isObject()) return false; object = &object->value(parts[i]).objectRef(); }
    }
    return true;
}
json::Object patchAt(const std::string& path, const json::Value& value) {
    auto parts = text::split(path.substr(1), '/');
    json::Value current = value;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) current = json::Object{{*it, current}};
    return current.toObject();
}

std::string exactBytes(long long bytes) {
    static const std::pair<long long, const char*> units[] = {{tebibyte, "TiB"}, {1073741824LL, "GiB"}, {1048576LL, "MiB"}, {1024LL, "KiB"}};
    if (bytes > 0)
        for (const auto& [size, name] : units) if (bytes % size == 0) return std::to_string(bytes / size) + " " + name;
    return std::to_string(bytes) + " B";
}
std::optional<long long> parseBytes(const std::string& raw) {
    auto value = text::lower(text::trim(raw));
    size_t split = 0;
    while (split < value.size() && (std::isdigit(static_cast<unsigned char>(value[split])) || value[split] == '.')) ++split;
    if (!split) return std::nullopt;
    const auto number = value.substr(0, split);
    const auto unit = text::trim(value.substr(split));
    if (std::count(number.begin(), number.end(), '.') > 1) return std::nullopt;
    const double amount = std::strtod(number.c_str(), nullptr);
    double multiplier = 0;
    const std::pair<const char*, double> units[] = {{"", 1}, {"b", 1}, {"byte", 1}, {"bytes", 1}, {"k", 1024}, {"kib", 1024}, {"kb", 1000},
                                                    {"m", 1048576}, {"mib", 1048576}, {"mb", 1e6}, {"g", 1073741824.0}, {"gib", 1073741824.0},
                                                    {"gb", 1e9}, {"t", 1099511627776.0}, {"tib", 1099511627776.0}, {"tb", 1e12}};
    for (const auto& [name, factor] : units) if (unit == name) multiplier = factor;
    if (multiplier == 0) return std::nullopt;
    const double bytes = std::round(amount * multiplier);
    if (!std::isfinite(bytes) || bytes > 9.0e15) return std::nullopt;
    return static_cast<long long>(bytes);
}
std::optional<long long> parseInteger(const std::string& raw) {
    const auto value = text::trim(raw);
    if (value.empty() || value.size() > 15) return std::nullopt;
    for (char c : value) if (c < '0' || c > '9') return std::nullopt;
    return std::stoll(value);
}
std::string displayValue(const Spec& spec, const json::Value& value, bool exists) {
    switch (spec.kind) {
    case Kind::Folder: case Kind::OptionalFolder: case Kind::Template: return value.toString();
    case Kind::Integer: return exists ? std::to_string(value.toInteger()) : std::to_string(spec.fallback);
    case Kind::OptionalInteger: return value.isNull() ? std::string() : std::to_string(value.toInteger());
    case Kind::Bytes: return exactBytes(exists ? value.toInteger() : spec.fallback);
    case Kind::ByteLimit: return value.isNull() ? std::string() : value.isString() ? std::string("Auto") : exactBytes(value.toInteger());
    case Kind::Megapixels: {
        char buffer[32];
        std::snprintf(buffer, sizeof buffer, "%g", double(value.toInteger()) / 1e6);
        return buffer;
    }
    }
    return {};
}
// The configuration value for the text, or an error message.
std::pair<std::optional<json::Value>, std::wstring> parseValue(const Spec& spec, const std::string& raw) {
    const auto value = text::trim(raw);
    const auto range = L" Use " + std::to_wstring(spec.minimum) + L" to " + std::to_wstring(spec.maximum) + L".";
    switch (spec.kind) {
    case Kind::Folder:
        if (value.empty() || !paths::isAbsolute(value)) return {std::nullopt, L"Choose a full folder path, such as C:\\Users\\you\\Documents\\put.gd."};
        return {json::Value(value), {}};
    case Kind::OptionalFolder:
        if (value.empty()) return {json::Value(), {}};
        if (!paths::isAbsolute(value)) return {std::nullopt, L"Choose a full folder path, or leave it empty to use the main folder."};
        return {json::Value(value), {}};
    case Kind::Template:
        if (value.empty()) return {std::nullopt, L"Enter a file name pattern."};
        if (auto error = naming::validateTemplate(value)) return {std::nullopt, wide(error.message)};
        return {json::Value(value), {}};
    case Kind::Integer: case Kind::OptionalInteger: {
        if (value.empty() && spec.kind == Kind::OptionalInteger) return {json::Value(), {}};
        const auto number = parseInteger(value);
        if (!number || *number < spec.minimum || *number > spec.maximum) return {std::nullopt, L"Enter a whole number." + range};
        return {json::Value(*number), {}};
    }
    case Kind::Bytes: case Kind::ByteLimit: {
        if (spec.kind == Kind::ByteLimit) {
            if (value.empty() || text::lower(value) == "no limit") return {json::Value(), {}};
            if (text::lower(value) == "auto") return {json::Value("auto"), {}};
        }
        const auto bytes = parseBytes(value);
        if (!bytes || *bytes < spec.minimum || *bytes > spec.maximum)
            return {std::nullopt, L"Enter a size from " + wide(exactBytes(spec.minimum)) + L" to " + wide(exactBytes(spec.maximum)) + L", such as 500 MiB" +
                                      (spec.kind == Kind::ByteLimit ? L", Auto, or leave it empty for no limit." : L".")};
        return {json::Value(*bytes), {}};
    }
    case Kind::Megapixels: {
        char* end = nullptr;
        const double megapixels = std::strtod(value.c_str(), &end);
        const double pixels = std::round(megapixels * 1e6);
        if (value.empty() || !end || *end || !std::isfinite(pixels) || pixels < 1 || pixels > 1e9) return {std::nullopt, L"Enter megapixels from 0.000001 to 1000."};
        return {json::Value(static_cast<long long>(pixels)), {}};
    }
    }
    return {std::nullopt, L"Not valid."};
}

bool modifierKey(UINT vk) {
    return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN || vk == VK_LSHIFT || vk == VK_RSHIFT ||
           vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU;
}
uint32_t heldModifiers() {
    uint32_t modifiers = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= shortcut::modControl;
    if (GetKeyState(VK_MENU) & 0x8000) modifiers |= shortcut::modAlt;
    if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= shortcut::modShift;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) modifiers |= shortcut::modWin;
    return modifiers;
}

class SettingsPageImpl final : public Page {
public:
    using Page::Page;
    HWND create(HWND parent) override;
    void activated() override { refreshUpdateStatus(); }
    void deactivated() override { if (recording_ >= 0) finishRecording(std::nullopt); }
    void settingsChanged(bool external) override;
    void invalidated(const std::string& kind) override { if (kind == "update") refreshUpdateStatus(); }
    bool validationFailed(const std::string& path, const std::wstring& message) override;
    bool dirty() const override { return !invalid_.empty() || recording_ >= 0; }
    void discardDraft() override;
    json::Object saveState() const override { return {{"section", sectionKeys[section_]}}; }
    void restoreState(const json::Object& state) override;
    void showSection(const std::string& name) override;
    void dpiChanged() override { for (auto& panel : panels_) panel.layout(); page_.layout(); }
    // Shortcut capture, from the read-only edit's subclass procedure.
    LRESULT shortcutKey(HWND edit, UINT message, WPARAM wParam, LPARAM lParam, bool& handled);
private:
    int layoutPage(int width, int height);
    int layoutSection(int section, int width, int height);
    void command(int id, int code, HWND control);
    void setSection(int section);
    void populate(bool external);
    void populateCapture();
    void populateShortcuts();
    void fieldChanged(size_t index);
    void setError(const std::string& path, const std::wstring& message);
    void captureDefaultChanged();
    void actionChanged(size_t index, bool sharingOnly);
    std::vector<Choice> destinationChoices(bool withDefault) const;
    std::vector<Choice> sharingChoices(const std::string& current) const;
    void startRecording(size_t index);
    void finishRecording(std::optional<std::string> chord);
    void refreshUpdateStatus();
    void showDiagnosticsState();
    void checkUploadTarget();
    void showUpdateStatus(const json::Object& status);
    void saveDiagnostics();
    void updateEnabled();

    Panel page_;
    std::array<Panel, 4> panels_;
    HWND sections_{};
    int section_ = Capture;
    bool populating_ = false;
    std::map<std::string, std::wstring> invalid_;
    std::map<std::string, HWND> errors_;
    struct FieldControls { HWND label, edit, browse; };
    std::vector<FieldControls> fields_;
    // Capture
    HWND captureHeading_{}, defaultLabel_{}, defaultDestination_{}, sharingLabel_{}, defaultSharing_{}, captureNote_{}, exceptionsHeading_{}, exceptionsNote_{};
    struct ActionRow { std::string name; HWND label, destination, sharing; };
    std::vector<ActionRow> actions_;
    // General
    HWND localHeading_{}, keepLocal_{}, namesHeading_{}, namingLabel_{}, naming_{}, collisionLabel_{}, collision_{}, tokens_{};
    HWND clipboardHeading_{}, clipboardLabel_{}, clipboard_{}, startupHeading_{}, notifications_{}, launch_{}, unlockStartup_{};
    HWND appearanceHeading_{}, themeLabel_{}, theme_{};
    // Shortcuts
    HWND shortcutsNote_{};
    struct ShortcutRow { std::string name; HWND label, edit, record, clear; };
    std::vector<ShortcutRow> shortcuts_;
    int recording_ = -1;
    bool awaitingAck_ = false;
    unsigned long long recordToken_ = 0;
    // Advanced
    HWND uploadsHeading_{}, limitsNote_{}, resetLimits_{}, imagesHeading_{}, retentionHeading_{}, logsHeading_{}, debug_{}, clearLogs_{};
    HWND diagnostics_{}, diagnosticsResult_{}, showDiagnostics_{}, updatesHeading_{}, updateCheck_{}, channelLabel_{}, channel_{}, checkNow_{}, updateStatus_{};
    HWND crashDumps_{}, logsStatus_{}, uncleanExit_{}, diagnosticsUpload_{}, uploadNote_{};
    json::Object uploadTarget_;      // diagnostics.uploadTarget's answer for the default destination
    bool checkingTarget_ = false;
    unsigned long long targetCheck_ = 0;
    HWND configHeading_{}, configPath_{}, reload_{};
    std::string diagnosticsPath_;
    int updatesY_ = 0;
    bool checking_ = false;
};

LRESULT CALLBACK shortcutEditProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    bool handled = false;
    const LRESULT result = reinterpret_cast<SettingsPageImpl*>(data)->shortcutKey(hwnd, message, wParam, lParam, handled);
    return handled ? result : DefSubclassProc(hwnd, message, wParam, lParam);
}

HWND SettingsPageImpl::create(HWND parent) {
    page_.onLayout = [this](int w, int h) { return layoutPage(w, h); };
    page_.onCommand = [this](int id, int code, HWND control) { command(id, code, control); };
    HWND p = page_.create(parent, false);
    sections_ = theme::pivot(p, idSections, {sectionNames[0], sectionNames[1], sectionNames[2], sectionNames[3]});
    for (int i = 0; i < 4; ++i) {
        auto& panel = panels_[size_t(i)];
        panel.onLayout = [this, i](int w, int h) { return layoutSection(i, w, h); };
        panel.onCommand = [this](int id, int code, HWND control) { command(id, code, control); };
        panel.onTimer = [this](UINT_PTR id) {
            if (id != timerRecord) return;
            KillTimer(panels_[Shortcuts].hwnd(), timerRecord);
            if (recording_ < 0) return;
            if (awaitingAck_) {
                // No pause acknowledgement in time: ask for an ordered restore; a late reply cannot start capture.
                ++recordToken_;
                awaitingAck_ = false;
                main_.shortcutRecording(false, {});
                const auto index = size_t(recording_);
                recording_ = -1;
                main_.setKeyCapture(nullptr);
                populateShortcuts();
                updateEnabled();
                setError("/actions/" + shortcuts_[index].name + "/shortcut", L"put.gd did not pause its shortcuts in time. Try again.");
            } else {
                finishRecording(std::nullopt);
            }
        };
        panel.create(p, true);
    }
    // Capture
    HWND c = panels_[Capture].hwnd();
    captureHeading_ = heading(c, L"Default capture output");
    defaultLabel_ = ui::label(c, L"Destination");
    defaultDestination_ = combo(c, idDefaultDestination);
    sharingLabel_ = ui::label(c, L"Sharing");
    defaultSharing_ = combo(c, idDefaultSharing);
    captureNote_ = ui::label(c, L"Actions set to Use default follow this output. Keep a local copy is in General and works with any destination.");
    panels_[Capture].markMuted(captureNote_);
    exceptionsHeading_ = heading(c, L"Per-action exceptions");
    exceptionsNote_ = ui::label(c, L"Use default follows future default changes. An explicit choice keeps its own destination and sharing.");
    panels_[Capture].markMuted(exceptionsNote_);
    const auto actionNames = Settings::actionNames();
    for (size_t i = 0; i < actionNames.size(); ++i) {
        ActionRow row{actionNames[i], ui::label(c, MainWindow::actionLabel(actionNames[i])), combo(c, idActionDestination + int(i)), combo(c, idActionSharing + int(i))};
        actions_.push_back(row);
        errors_["/actions/" + actionNames[i]] = ui::label(c, L"");
        panels_[Capture].markError(errors_["/actions/" + actionNames[i]], true);
    }
    errors_["/captureDefaults"] = ui::label(c, L"");
    panels_[Capture].markError(errors_["/captureDefaults"], true);

    // Fields shared by General and Advanced.
    for (size_t i = 0; i < specs.size(); ++i) {
        HWND host = panels_[size_t(specs[i].section)].hwnd();
        const bool folder = specs[i].kind == Kind::Folder || specs[i].kind == Kind::OptionalFolder;
        FieldControls controls{ui::label(host, specs[i].caption, 0, 0), edit(host, idField + int(i)), folder ? button(host, idBrowse + int(i), L"Browse\x2026") : nullptr};
        if (*specs[i].cue) setCue(controls.edit, specs[i].cue);
        fields_.push_back(controls);
        errors_[specs[i].path] = ui::label(host, L"");
        panels_[size_t(specs[i].section)].markError(errors_[specs[i].path], true);
    }
    // General
    HWND g = panels_[General].hwnd();
    localHeading_ = heading(g, L"Local copies");
    keepLocal_ = check(g, idKeepLocal, L"Keep a local copy");
    setTooltip(main_.tooltip(), keepLocal_, L"Save every new capture and upload into the folders below, independently of the upload destination.");
    namesHeading_ = heading(g, L"File names");
    namingLabel_ = ui::label(g, L"Names");
    naming_ = combo(g, idNamingMode);
    setChoices(naming_, {{L"Keep original names", "original"}, {L"Random names", "generated"}, {L"Custom template", "template"}});
    collisionLabel_ = ui::label(g, L"When a name exists");
    collision_ = combo(g, idCollision);
    setChoices(collision_, {{L"Add a unique suffix", "rename"}, {L"Skip duplicate names", "skip"}, {L"Replace the checked object", "overwrite"}});
    tokens_ = ui::label(g, L"Patterns use {name} {ext} {yyyy} {MM} {dd} {HH} {mm} {ss} {uuid}. Copied & uploaded files use their pattern only with Custom template.");
    panels_[General].markMuted(tokens_);
    errors_["/naming"] = ui::label(g, L"");
    panels_[General].markError(errors_["/naming"], true);
    clipboardHeading_ = heading(g, L"After uploading");
    clipboardLabel_ = ui::label(g, L"Links");
    clipboard_ = combo(g, idClipboard);
    setChoices(clipboard_, {{L"Copy each link as it finishes", "each_link"}, {L"Copy a list when uploads finish", "list"}});
    startupHeading_ = heading(g, L"Startup and notifications");
    notifications_ = check(g, idNotifications, L"Show a notification when a link is ready");
    launch_ = check(g, idLaunch, L"Launch at login");
    unlockStartup_ = check(g, idUnlockStartup, L"Unlock saved credentials when put.gd starts");
    appearanceHeading_ = heading(g, L"Appearance");
    themeLabel_ = ui::label(g, L"Theme");
    theme_ = combo(g, idTheme);
    setChoices(theme_, {{L"Match Windows", "system"}, {L"Dark", "dark"}, {L"Light", "light"}});
    errors_["/preferences"] = ui::label(g, L"");
    panels_[General].markError(errors_["/preferences"], true);

    // Shortcuts
    HWND s = panels_[Shortcuts].hwnd();
    shortcutsNote_ = ui::label(s, L"Press Record, then the key combination. Escape cancels. Global shortcuts pause while recording. Changes save automatically.");
    panels_[Shortcuts].markMuted(shortcutsNote_);
    for (size_t i = 0; i < shortcutActions.size(); ++i) {
        ShortcutRow row{shortcutActions[i], ui::label(s, MainWindow::actionLabel(shortcutActions[i])), edit(s, idShortcutEdit + int(i), ES_READONLY),
                        button(s, idRecord + int(i), L"Record"), button(s, idClear + int(i), L"Clear")};
        setCue(row.edit, L"Not assigned");
        SetWindowSubclass(row.edit, shortcutEditProc, 1, reinterpret_cast<DWORD_PTR>(this));
        shortcuts_.push_back(row);
        errors_["/actions/" + shortcutActions[i] + "/shortcut"] = ui::label(s, L"");
        panels_[Shortcuts].markError(errors_["/actions/" + shortcutActions[i] + "/shortcut"], true);
    }
    setTooltip(main_.tooltip(), shortcuts_.back().edit, L"Screen recording is not available on Windows yet; this key is kept in the shared configuration.");

    // Advanced
    HWND a = panels_[Advanced].hwnd();
    uploadsHeading_ = heading(a, L"Uploads");
    limitsNote_ = ui::label(a, L"Sizes accept values such as 500 MiB or 2 GiB. Auto or empty sets no limit of put.gd's own; free disk space and the destination's maximum always apply.");
    panels_[Advanced].markMuted(limitsNote_);
    imagesHeading_ = heading(a, L"Images and clipboard");
    resetLimits_ = button(a, idResetLimits, L"Reset limits to defaults\x2026");
    retentionHeading_ = heading(a, L"Retention");
    logsHeading_ = heading(a, L"Logs and diagnostics");
    debug_ = check(a, idDebug, L"Debug logging (turns itself off after 24 hours)");
    clearLogs_ = button(a, idClearLogs, L"Clear logs\x2026");
    theme::setRole(clearLogs_, theme::Role::Danger);
    crashDumps_ = check(a, idCrashDumps, L"Save crash dumps (Windows Error Reporting)");
    setTooltip(main_.tooltip(), crashDumps_, L"Windows writes a minidump to put.gd's crashdumps folder when put.gd crashes. Dumps stay on this PC and are never put in a diagnostics package.");
    logsStatus_ = ui::label(a, L"");
    panels_[Advanced].markMuted(logsStatus_);
    uncleanExit_ = ui::label(a, L"");
    diagnosticsUpload_ = check(a, idDiagnosticsUpload, L"Also upload the package and copy a link");
    uploadNote_ = ui::label(a, L"");
    panels_[Advanced].markMuted(uploadNote_);
    diagnostics_ = button(a, idDiagnostics, L"Save diagnostics package\x2026");
    setTooltip(main_.tooltip(), diagnostics_, L"Saves a redacted package with logs and settings, without credentials or file contents.");
    diagnosticsResult_ = ui::label(a, L"", 0, SS_PATHELLIPSIS);
    showDiagnostics_ = button(a, idShowDiagnostics, L"Show in folder");
    updatesHeading_ = heading(a, L"Updates");
    updateCheck_ = check(a, idUpdateCheck, L"Check for updates once a day");
    channelLabel_ = ui::label(a, L"Channel");
    channel_ = combo(a, idUpdateChannel);
    setChoices(channel_, {{L"This build's channel", "build"}, {L"Stable", "stable"}, {L"Nightly", "nightly"}});
    checkNow_ = button(a, idCheckNow, L"Check now");
    updateStatus_ = ui::label(a, L"");
    panels_[Advanced].markMuted(updateStatus_);
    configHeading_ = heading(a, L"Config file");
    configPath_ = ui::label(a, L"", 0, SS_PATHELLIPSIS | SS_NOPREFIX);
    panels_[Advanced].markMuted(configPath_);
    reload_ = button(a, idReload, L"Reload from file");
    setTooltip(main_.tooltip(), reload_, L"Discard unsaved edits and read the settings file again.");
    for (const auto* key : {"/retention", "/limits", "/archive"}) {
        HWND host = panels_[std::string(key) == "/archive" ? General : Advanced].hwnd();
        errors_[key] = ui::label(host, L"");
        panels_[std::string(key) == "/archive" ? General : Advanced].markError(errors_[key], true);
    }
    populate(true);
    setSection(section_);
    return p;
}

int SettingsPageImpl::layoutPage(int width, int height) {
    const UINT d = page_.dpi();
    const int margin = scale(24, d) - scale(12, d), pivotHeight = scale(40, d); // the pivot's items carry 12 px of padding
    place(sections_, margin, 0, std::min(width - 2 * margin, theme::pivotWidth(sections_) + scale(8, d)), pivotHeight);
    for (auto& panel : panels_) place(panel.hwnd(), 0, pivotHeight + scale(6, d), width, height - pivotHeight - scale(6, d));
    return height;
}

int SettingsPageImpl::layoutSection(int section, int width, int height) {
    if (fields_.size() != specs.size() || shortcuts_.size() != shortcutActions.size() || !configPath_) return 0; // still creating controls
    auto& panel = panels_[size_t(section)];
    const UINT d = panel.dpi();
    const int margin = scale(24, d);
    Rows rows(d, margin, scale(14, d) + panel.offsetY(), std::clamp(width - 2 * margin, scale(320, d), scale(780, d)), 220);
    const int line = lineHeight(d);
    auto field = [&](size_t index, int fieldWidth = 0) {
        const auto& controls = fields_[index];
        rows.field(controls.label, controls.edit, fieldWidth, controls.browse, controls.browse ? -1 : 0);
        rows.error(errors_[specs[index].path]);
    };
    auto fieldsOf = [&](const char* prefix, int fieldWidth) {
        for (size_t i = 0; i < specs.size(); ++i) if (text::startsWith(specs[i].path, prefix)) field(i, fieldWidth);
    };
    auto wrapped = [&](HWND h, int lines) { rows.text(h, lines); };
    switch (section) {
    case Capture:
        rows.heading(captureHeading_);
        rows.field(defaultLabel_, defaultDestination_, 360);
        rows.field(sharingLabel_, defaultSharing_, 360);
        rows.error(errors_["/captureDefaults"]);
        wrapped(captureNote_, 2);
        rows.heading(exceptionsHeading_);
        wrapped(exceptionsNote_, 2);
        for (const auto& row : actions_) {
            rows.field(row.label, row.destination, 300, row.sharing, 230);
            rows.error(errors_["/actions/" + row.name]);
        }
        break;
    case General:
        rows.heading(localHeading_);
        rows.line({{keepLocal_, 360}}, true, rows.fieldHeight());
        fieldsOf("/archive/", 0);
        rows.error(errors_["/archive"]);
        rows.heading(namesHeading_);
        rows.field(namingLabel_, naming_, 300);
        rows.field(collisionLabel_, collision_, 300);
        rows.error(errors_["/naming"]);
        fieldsOf("/naming/", 0);
        wrapped(tokens_, 2);
        rows.heading(clipboardHeading_);
        rows.field(clipboardLabel_, clipboard_, 300);
        rows.heading(startupHeading_);
        for (HWND h : {notifications_, launch_, unlockStartup_}) rows.line({{h, 420}}, true, rows.fieldHeight());
        rows.heading(appearanceHeading_);
        rows.field(themeLabel_, theme_, 300);
        rows.error(errors_["/preferences"]);
        break;
    case Shortcuts:
        wrapped(shortcutsNote_, 2);
        rows.space(8);
        for (const auto& row : shortcuts_) {
            const int gap = rows.px(8), buttonWidth = rows.px(96);
            rows.field(row.label, row.edit, 240);
            const int y = rows.y() - rows.px(8) - rows.fieldHeight() - (rows.buttonHeight() - rows.fieldHeight()) / 2;
            const int x = rows.controlX() + std::min(rows.px(240), rows.controlWidth() - 2 * (buttonWidth + gap)) + gap;
            place(row.record, x, y, buttonWidth, rows.buttonHeight());
            place(row.clear, x + buttonWidth + gap, y, buttonWidth, rows.buttonHeight());
            rows.space(2);
            rows.error(errors_["/actions/" + row.name + "/shortcut"]);
        }
        break;
    case Advanced:
        rows.heading(uploadsHeading_);
        wrapped(limitsNote_, 2);
        for (const auto* key : {"/limits/maxFileBytes", "/limits/maxBatchBytes", "/limits/pendingArtifactBytes", "/limits/maxFiles", "/limits/concurrentUploads", "/limits/pendingOperations"})
            for (size_t i = 0; i < specs.size(); ++i) if (specs[i].path == key) field(i, 200);
        rows.heading(imagesHeading_);
        for (const auto* key : {"/limits/clipboardTextBytes", "/limits/imageBytes", "/limits/imagePixels", "/limits/pendingImageBytes"})
            for (size_t i = 0; i < specs.size(); ++i) if (specs[i].path == key) field(i, 200);
        rows.error(errors_["/limits"]);
        rows.line({{resetLimits_, -1}}, true);
        rows.heading(retentionHeading_);
        for (const auto* key : {"/retention/failedArtifactDays", "/retention/recentActivityLimit", "/retention/previewCacheBytes", "/retention/historyRetentionDays"})
            for (size_t i = 0; i < specs.size(); ++i) if (specs[i].path == key) field(i, 200);
        rows.error(errors_["/retention"]);
        rows.heading(logsHeading_);
        for (const auto* key : {"/retention/logDays", "/retention/logBytes"})
            for (size_t i = 0; i < specs.size(); ++i) if (specs[i].path == key) field(i, 200);
        rows.line({{debug_, 420}}, true, rows.fieldHeight());
        rows.line({{crashDumps_, 420}}, true, rows.fieldHeight());
        rows.block(logsStatus_, line * 2 + 2);
        if (!ui::text(uncleanExit_).empty()) rows.block(uncleanExit_, line * 2 + 2);
        show(uncleanExit_, !ui::text(uncleanExit_).empty());
        rows.line({{diagnosticsUpload_, 420}}, true, rows.fieldHeight());
        if (!ui::text(uploadNote_).empty()) rows.block(uploadNote_, line * 2 + 2);
        show(uploadNote_, !ui::text(uploadNote_).empty());
        rows.line({{clearLogs_, -1}, {diagnostics_, -1}}, true);
        if (!diagnosticsPath_.empty()) {
            const int showWidth = buttonWidth(showDiagnostics_);
            const int labelWidth = std::max(rows.px(100), rows.controlWidth() - showWidth - rows.px(8));
            place(diagnosticsResult_, rows.controlX(), rows.y() + (rows.buttonHeight() - line) / 2, labelWidth, line + 2);
            place(showDiagnostics_, rows.controlX() + labelWidth + rows.px(8), rows.y(), showWidth, rows.buttonHeight());
            rows.space(0);
            rows.line({}, true);
        }
        show(diagnosticsResult_, !diagnosticsPath_.empty());
        show(showDiagnostics_, !diagnosticsPath_.empty());
        updatesY_ = rows.y() - panel.offsetY();
        rows.heading(updatesHeading_);
        rows.line({{updateCheck_, 360}}, true, rows.fieldHeight());
        rows.field(channelLabel_, channel_, 240, checkNow_, -1);
        rows.block(updateStatus_, line * 2 + 2);
        rows.heading(configHeading_);
        rows.block(configPath_, line * 2 + 2);
        rows.line({{reload_, -1}}, true);
        break;
    }
    return rows.y() - panel.offsetY() + rows.px(16);
}

void SettingsPageImpl::setSection(int section) {
    section_ = std::clamp(section, 0, 3);
    SendMessageW(sections_, LB_SETCURSEL, WPARAM(section_), 0);
    for (int i = 0; i < 4; ++i) ShowWindow(panels_[size_t(i)].hwnd(), i == section_ ? SW_SHOW : SW_HIDE);
    panels_[size_t(section_)].layout();
    if (section_ != Shortcuts && recording_ >= 0) finishRecording(std::nullopt);
}

void SettingsPageImpl::showSection(const std::string& name) {
    if (name == "updates") {
        setSection(Advanced);
        panels_[Advanced].layout();
        panels_[Advanced].scrollTo(updatesY_);
        return;
    }
    for (int i = 0; i < 4; ++i) if (name == sectionKeys[i]) setSection(i);
}

void SettingsPageImpl::restoreState(const json::Object& state) {
    const auto section = state.value("section").toString();
    for (int i = 0; i < 4; ++i) if (section == sectionKeys[i]) section_ = i;
    if (sections_) setSection(section_);
}

std::vector<Choice> SettingsPageImpl::destinationChoices(bool withDefault) const {
    std::vector<Choice> choices;
    if (withDefault) choices.push_back({L"Use default", "default"});
    choices.push_back({L"No destination (uploads off)", "local"});
    for (const auto& entry : main_.session().document().value("profiles").arrayRef()) {
        const auto& profile = entry.objectRef();
        // Availability comes from the resident's capabilities (providers[id].available), not a provider list of our own.
        const auto& providers = main_.session().diagnostics().value("capabilities")["providers"];
        const auto& entryCaps = providers[profile.value("provider").toString()];
        const bool available = entryCaps.isObject() ? entryCaps["available"].toBool(false) : profile.value("provider") == "s3";
        choices.push_back({wide(profile.value("label").toString()) + (available ? L"" : L" (not available on Windows)"), "profile:" + profile.value("id").toString(), available});
    }
    return choices;
}

std::vector<Choice> SettingsPageImpl::sharingChoices(const std::string& current) const {
    std::vector<Choice> choices{{L"Use destination default", "default"}, {L"Share and copy a link", "share"}};
    if (current == "private") choices.push_back({L"Keep private, copy nothing (not supported by S3)", "private"});
    return choices;
}

void SettingsPageImpl::populateCapture() {
    const auto& document = main_.session().document();
    const bool shared = document.contains("captureDefaults");
    const auto output = shared ? document.value("captureDefaults").toObject() : Settings::effectiveAction(document, "region");
    auto route = [](const json::Object& o) {
        const auto profile = o.value("profileId").toString();
        return o.value("destination") == "upload" && !profile.empty() ? "profile:" + profile : std::string("local");
    };
    setChoices(defaultDestination_, destinationChoices(false));
    ui::select(defaultDestination_, route(output));
    const auto defaultMode = output.value("publicationMode").toString("default");
    setChoices(defaultSharing_, sharingChoices(defaultMode));
    ui::select(defaultSharing_, defaultMode);
    enable(defaultSharing_, route(output) != "local");
    for (auto& row : actions_) {
        if (invalid_.count("/actions/" + row.name)) continue;
        const auto action = document.value("actions")[row.name].toObject();
        const bool inherited = action.value("destination") == "default";
        setChoices(row.destination, destinationChoices(true));
        ui::select(row.destination, inherited ? std::string("default") : route(action));
        const auto effective = Settings::effectiveAction(document, row.name);
        const auto mode = effective.value("publicationMode").toString("default");
        setChoices(row.sharing, sharingChoices(mode));
        ui::select(row.sharing, mode);
        enable(row.sharing, !inherited && route(action) != "local");
    }
}

void SettingsPageImpl::populateShortcuts() {
    const auto& document = main_.session().document();
    for (size_t i = 0; i < shortcuts_.size(); ++i) {
        if (int(i) == recording_) continue;
        syncText(shortcuts_[i].edit, document.value("actions")[shortcuts_[i].name]["shortcut"].toString());
    }
}

void SettingsPageImpl::populate(bool external) {
    populating_ = true;
    const auto& document = main_.session().document();
    if (external) {
        invalid_.clear();
        for (auto& [path, label] : errors_) setTextW(label, L"");
    }
    for (size_t i = 0; i < specs.size(); ++i) {
        const auto& spec = specs[i];
        if (invalid_.count(spec.path)) continue;
        const bool exists = present(document, spec.path);
        const auto value = at(document, spec.path);
        const auto shown = displayValue(spec, value, exists);
        if (!external) {
            // Keep the user's own formatting when it means the same value.
            const auto parsed = parseValue(spec, text(fields_[i].edit)).first;
            if (parsed && (*parsed == value || (!exists && parsed->toInteger(-1) == spec.fallback))) continue;
        }
        syncText(fields_[i].edit, shown);
        if (spec.kind == Kind::OptionalFolder) {
            const auto category = spec.path.substr(spec.path.rfind('/') + 1);
            setCue(fields_[i].edit, L"Default: " + wide(paths::join(document.value("archive")["root"].toString(), category)));
        }
    }
    setChecked(keepLocal_, document.value("archive")["keepUploads"].toBool());
    ui::select(naming_, document.value("naming")["mode"].toString("original"));
    ui::select(collision_, document.value("naming")["collisionPolicy"].toString("rename"));
    const auto preferences = document.value("preferences").toObject();
    ui::select(clipboard_, preferences.value("uploadClipboardBehavior").toString("each_link"));
    setChecked(notifications_, preferences.value("notifications").toBool(true));
    setChecked(launch_, preferences.value("launchAtLogin").toBool(false));
    setChecked(unlockStartup_, preferences.value("unlockCredentialsAtStartup").toBool(true));
    ui::select(theme_, preferences.value("appearance")["theme"].toString("system"));
    setChecked(debug_, preferences.value("diagnostics")["debugLogging"].toBool(false));
    setChecked(crashDumps_, preferences.value("diagnostics")["crashDumps"].toBool(false));
    showDiagnosticsState();
    setChecked(updateCheck_, preferences.value("updates")["check"].toBool(true));
    ui::select(channel_, preferences.value("updates")["channel"].toString("build"));
    const auto& metadata = main_.session().metadata();
    auto revision = metadata.value("acceptedRevision").toString();
    if (revision.size() > 12) revision.resize(12);
    setTextW(configPath_, wide(metadata.value("path").toString()) + (revision.empty() ? L"" : L"\nAccepted revision " + wide(revision)));
    populateCapture();
    populateShortcuts();
    populating_ = false;
    updateEnabled();
    for (auto& panel : panels_) panel.layout();
}

void SettingsPageImpl::settingsChanged(bool external) { populate(external); }

void SettingsPageImpl::discardDraft() {
    invalid_.clear();
    for (auto& [path, label] : errors_) setTextW(label, L"");
    if (recording_ >= 0) finishRecording(std::nullopt);
}

void SettingsPageImpl::updateEnabled() {
    for (size_t i = 0; i < specs.size(); ++i)
        if (specs[i].path == "/naming/fileTemplate") enable(fields_[i].edit, selected(naming_) == "template");
    for (size_t i = 0; i < shortcuts_.size(); ++i) {
        enable(shortcuts_[i].record, recording_ < 0 || int(i) == recording_);
        enable(shortcuts_[i].clear, recording_ < 0);
        setTextW(shortcuts_[i].record, int(i) == recording_ ? L"Cancel" : L"Record");
    }
    enable(checkNow_, !checking_);
}

void SettingsPageImpl::setError(const std::string& path, const std::wstring& message) {
    auto it = errors_.find(path);
    if (it == errors_.end()) return;
    setTextW(it->second, message);
    for (int i = 0; i < 4; ++i) if (GetParent(it->second) == panels_[size_t(i)].hwnd()) panels_[size_t(i)].layout();
}

void SettingsPageImpl::fieldChanged(size_t index) {
    const auto& spec = specs[index];
    const auto [value, error] = parseValue(spec, text(fields_[index].edit));
    if (!value) {
        invalid_[spec.path] = error;
        setError(spec.path, error);
        return;
    }
    invalid_.erase(spec.path);
    setError(spec.path, L"");
    main_.queuePatch(patchAt(spec.path, *value));
    if (spec.path == "/archive/root") populate(false);
}

bool SettingsPageImpl::validationFailed(const std::string& path, const std::wstring& message) {
    std::string key = path;
    while (!key.empty()) {
        if (errors_.count(key)) {
            setError(key, message);
            for (int i = 0; i < 4; ++i)
                if (GetParent(errors_[key]) == panels_[size_t(i)].hwnd() && i != section_) setSection(i);
            return true;
        }
        const auto slash = key.rfind('/');
        if (slash == std::string::npos || slash == 0) break;
        key.resize(slash);
    }
    return false;
}

void SettingsPageImpl::captureDefaultChanged() {
    const auto& document = main_.session().document();
    const auto destination = selected(defaultDestination_);
    const bool upload = text::startsWith(destination, "profile:");
    json::Object output{{"destination", upload ? "upload" : "local"}, {"profileId", upload ? json::Value(destination.substr(8)) : json::Value()},
                        {"publicationMode", upload ? selected(defaultSharing_) : std::string("default")}};
    json::Object patch{{"captureDefaults", output}};
    if (!document.contains("captureDefaults")) {
        // Adopt the shared default; actions that all routed alike now follow it.
        const auto first = Settings::effectiveAction(document, "region");
        bool same = true;
        for (const auto& name : Settings::actionNames()) {
            const auto action = Settings::effectiveAction(document, name);
            same = same && action.value("destination") == first.value("destination") && action.value("profileId") == first.value("profileId") &&
                   action.value("publicationMode").toString("default") == first.value("publicationMode").toString("default");
        }
        if (same) {
            json::Object actions;
            for (const auto& name : Settings::actionNames())
                actions[name] = json::Object{{"destination", "default"}, {"profileId", json::Value()}, {"publicationMode", "default"}};
            patch["actions"] = actions;
        }
    }
    enable(defaultSharing_, upload);
    setError("/captureDefaults", L"");
    main_.queuePatch(patch);
}

void SettingsPageImpl::actionChanged(size_t index, bool sharingOnly) {
    auto& row = actions_[index];
    const auto& document = main_.session().document();
    const auto destination = selected(row.destination);
    json::Object action;
    json::Object patch;
    if (destination == "default") {
        action = {{"destination", "default"}, {"profileId", json::Value()}, {"publicationMode", "default"}};
        if (!document.contains("captureDefaults")) {
            const auto region = Settings::effectiveAction(document, "region");
            patch["captureDefaults"] = json::Object{{"destination", region.value("destination") == "upload" && !region.value("profileId").isNull() ? "upload" : "local"},
                                                    {"profileId", region.value("profileId")}, {"publicationMode", region.value("publicationMode").toString("default")}};
        }
    } else if (destination == "local") {
        action = {{"destination", "local"}, {"profileId", json::Value()}, {"publicationMode", "default"}};
    } else {
        action = {{"destination", "upload"}, {"profileId", destination.substr(8)}, {"publicationMode", selected(row.sharing)}};
    }
    (void)sharingOnly;
    enable(row.sharing, text::startsWith(destination, "profile:"));
    patch["actions"] = json::Object{{row.name, action}};
    setError("/actions/" + row.name, L"");
    main_.queuePatch(patch);
}

// ---- shortcut recording ------------------------------------------------------
void SettingsPageImpl::startRecording(size_t index) {
    if (recording_ >= 0) { finishRecording(std::nullopt); return; }
    recording_ = int(index);
    awaitingAck_ = true;
    const auto token = ++recordToken_;
    setError("/actions/" + shortcuts_[index].name + "/shortcut", L"");
    setTextW(shortcuts_[index].edit, L"Pausing global shortcuts\x2026");
    updateEnabled();
    SetTimer(panels_[Shortcuts].hwnd(), timerRecord, 12000, nullptr);
    main_.shortcutRecording(true, [this, token, index](bool ok, const std::string& error) {
        if (token != recordToken_ || !awaitingAck_) {
            // A late acknowledgement must not start a capture; restore the bindings.
            if (ok) main_.shortcutRecording(false, {});
            return;
        }
        awaitingAck_ = false;
        if (!ok) {
            KillTimer(panels_[Shortcuts].hwnd(), timerRecord);
            recording_ = -1;
            populateShortcuts();
            updateEnabled();
            setError("/actions/" + shortcuts_[index].name + "/shortcut", L"Shortcut recording is not available (" + wide(error) + L").");
            return;
        }
        setTextW(shortcuts_[index].edit, L"Press a key combination\x2026");
        main_.setKeyCapture(shortcuts_[index].edit);
        SetFocus(shortcuts_[index].edit);
        SetTimer(panels_[Shortcuts].hwnd(), timerRecord, 12000, nullptr);
    });
}

void SettingsPageImpl::finishRecording(std::optional<std::string> chord) {
    if (recording_ < 0) return;
    const auto index = size_t(recording_);
    const bool wasCapturing = !awaitingAck_;
    ++recordToken_;
    recording_ = -1;
    awaitingAck_ = false;
    KillTimer(panels_[Shortcuts].hwnd(), timerRecord);
    main_.setKeyCapture(nullptr);
    main_.shortcutRecording(false, [this](bool ok, const std::string& error) {
        if (!ok) main_.status(L"Global shortcuts could not be restored (" + wide(error) + L"). Restart put.gd if they stop working.", true);
    });
    (void)wasCapturing;
    const auto& name = shortcuts_[index].name;
    if (chord) {
        const auto& document = main_.session().document();
        for (const auto& other : shortcutActions) {
            if (other == name || chord->empty()) continue;
            if (document.value("actions")[other]["shortcut"].toString() == *chord) {
                populateShortcuts();
                updateEnabled();
                setError("/actions/" + name + "/shortcut", wide(*chord) + L" is already used by " + MainWindow::actionLabel(other) + L".");
                return;
            }
        }
        setText(shortcuts_[index].edit, *chord);
        main_.queuePatch({{"actions", json::Object{{name, json::Object{{"shortcut", *chord}}}}}});
    }
    populateShortcuts();
    if (chord) setText(shortcuts_[index].edit, *chord);
    updateEnabled();
}

LRESULT SettingsPageImpl::shortcutKey(HWND edit, UINT message, WPARAM wParam, LPARAM lParam, bool& handled) {
    if (recording_ < 0 || awaitingAck_ || shortcuts_[size_t(recording_)].edit != edit) return 0;
    switch (message) {
    case WM_GETDLGCODE:
        handled = true;
        return DLGC_WANTALLKEYS;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        handled = true;
        const UINT vk = UINT(wParam);
        const auto modifiers = heldModifiers();
        if (modifierKey(vk)) {
            std::string partial = shortcut::toPortableText({modifiers, 'A'});
            if (partial.size() > 1) partial.resize(partial.size() - 1);
            setText(edit, partial.empty() ? std::string("Press a key combination\xE2\x80\xA6") : partial + "\xE2\x80\xA6");
            return 0;
        }
        if (vk == VK_ESCAPE && !modifiers) { finishRecording(std::nullopt); return 0; }
        const auto chord = shortcut::toPortableText({modifiers, vk});
        if (chord.empty()) {
            setError("/actions/" + shortcuts_[size_t(recording_)].name + "/shortcut", L"That key cannot be used for a shortcut. Try another combination.");
            return 0;
        }
        finishRecording(chord);
        return 0;
    }
    case WM_KEYUP: case WM_SYSKEYUP:
        handled = true;
        // Print Screen reaches applications only as a key release.
        if (wParam == VK_SNAPSHOT) {
            const auto chord = shortcut::toPortableText({heldModifiers(), VK_SNAPSHOT});
            if (!chord.empty()) finishRecording(chord);
        }
        return 0;
    case WM_CHAR: case WM_SYSCHAR: case WM_DEADCHAR: case WM_SYSDEADCHAR:
        handled = true;
        return 0;
    case WM_KILLFOCUS:
        // Focus moving to this row's Cancel button: its click ends the recording.
        if (reinterpret_cast<HWND>(wParam) != shortcuts_[size_t(recording_)].record) finishRecording(std::nullopt);
        return 0;
    }
    return 0;
}

// ---- updates and diagnostics -------------------------------------------------
void SettingsPageImpl::showUpdateStatus(const json::Object& status) {
    std::wstring text;
    const auto result = status.value("result").toObject();
    if (status.value("supported").isBool() && !status.value("supported").toBool()) {
        text = L"This build doesn't check for updates.";
        if (!status.value("reason").toString().empty()) text += L" (" + wide(status.value("reason").toString()) + L")";
    } else if (status.value("checking").toBool()) {
        text = L"Checking\x2026";
    } else if (result.value("updateAvailable").toBool()) {
        text = L"put.gd " + wide(result.value("latest").toString()) + L" is available.";
    } else if (result.value("latest").isString()) {
        text = L"Up to date: " + wide(result.value("current").toString(result.value("latest").toString()));
    } else {
        text = L"Not checked yet.";
    }
    if (status.value("error").isString()) text += L"\nThe last check failed: " + wide(status.value("error").toString());
    else if (status.value("checkedAt").isString()) text += L"\nChecked " + localTime(status.value("checkedAt").toString());
    setTextW(updateStatus_, text);
    panels_[Advanced].layout();
}

void SettingsPageImpl::refreshUpdateStatus() {
    main_.session().call("update.status", {}, [this](const Reply& reply) { if (reply.ok) showUpdateStatus(reply.result); });
}

// The log folder's state, the last unclean exit and the upload option (docs/diagnostics.md).
void SettingsPageImpl::showDiagnosticsState() {
    const auto& d = main_.session().diagnostics();
    const auto logging = d.value("logging").toObject();
    std::wstring logs = std::to_wstring(logging.value("files").toInt()) + L" log files, " + wide(text::byteSize(logging.value("bytes").toInteger()));
    if (logging.value("oldestRecord").isString()) logs += L", oldest record " + localTime(logging.value("oldestRecord").toString());
    if (logging.value("degraded").toBool())
        logs += L"\nLogging is degraded (" + wide(logging.value("degradedReason").toString()) + L"): " + std::to_wstring(logging.value("droppedRecords").toInteger()) + L" records dropped. Uploads and captures carry on.";
    setTextW(logsStatus_, logs);
    const auto last = d.value("uncleanExit")["last"].toObject();
    std::wstring unclean;
    if (!last.empty()) {
        const auto cause = last.value("cause").toString();
        unclean = L"Last unclean exit: put.gd " + wide(last.value("previousVersion").toString()) + L", started " + localTime(last.value("previousStart").toString()) + L", " +
            (cause == "crash" ? L"crashed (" + wide(last.value("exceptionName").toString(last.value("exceptionCode").toString())) + L")"
             : cause == "signal" ? L"stopped by Windows or a stop request (" + wide(last.value("detail").toString(last.value("event").toString())) + L")"
                                 : std::wstring(L"ended for an unknown reason (a forced kill, a crash or a power loss)"));
    }
    setTextW(uncleanExit_, unclean);
    // The upload is offered for the default destination; ticking it checks that destination.
    const auto profileId = diagnostics::defaultUploadProfile(Settings::fromValidated(main_.session().document()));
    enable(diagnosticsUpload_, !profileId.empty() && !checkingTarget_);
    if (profileId.empty()) { setChecked(diagnosticsUpload_, false); setTextW(uploadNote_, L"No upload destination is set up, so packages are saved on this PC only."); }
    else if (!checkingTarget_ && !checked(diagnosticsUpload_)) setTextW(uploadNote_, L"");
}
void SettingsPageImpl::checkUploadTarget() {
    if (!checked(diagnosticsUpload_)) { uploadTarget_ = {}; showDiagnosticsState(); panels_[Advanced].layout(); return; }
    checkingTarget_ = true;
    const auto sequence = ++targetCheck_;
    setTextW(uploadNote_, L"Checking the upload destination (a Test connection)\x2026");
    showDiagnosticsState(); panels_[Advanced].layout();
    main_.session().call("diagnostics.uploadTarget", {}, [this, sequence](const Reply& reply) {
        if (sequence != targetCheck_) return;
        checkingTarget_ = false;
        uploadTarget_ = reply.ok ? reply.result : json::Object{};
        if (!reply.ok) { setChecked(diagnosticsUpload_, false); setTextW(uploadNote_, L"Can't upload: " + UiSession::describe(reply)); }
        else if (!uploadTarget_.value("available").toBool()) {
            setChecked(diagnosticsUpload_, false);
            const auto reason = uploadTarget_.value("reason").toString();
            const auto label = wide(uploadTarget_.value("label").toString());
            setTextW(uploadNote_, reason == "diagnostics_no_destination" ? std::wstring(L"No upload destination is set up.")
                : reason == "diagnostics_destination_failing" ? L"The last upload to " + label + L" failed with a credential or configuration error. Fix the destination, then try again."
                : L"Test connection failed for " + label + L": " + wide(uploadTarget_.value("details")["error"]["message"].toString(uploadTarget_.value("message").toString())));
        } else setTextW(uploadNote_, L"Uploads to " + wide(uploadTarget_.value("label").toString()) + L". " + wide(uploadTarget_.value("warning").toString()));
        showDiagnosticsState();
        panels_[Advanced].layout();
    });
}
void SettingsPageImpl::saveDiagnostics() {
    const auto& document = main_.session().document();
    auto folder = document.value("preferences")["diagnostics"]["packageFolder"].toString();
    if (folder.empty()) folder = document.value("archive")["root"].toString();
    std::string chosen;
    if (!pickFolder(main_.hwnd(), L"Save diagnostics package to", folder, chosen)) return;
    main_.queuePatch({{"preferences", json::Object{{"diagnostics", json::Object{{"debugLogging", checked(debug_)}, {"packageFolder", chosen}}}}}});
    enable(diagnostics_, false);
    main_.status(L"Saving the diagnostics package\x2026");
    json::Object params{{"output", chosen}};
    const bool upload = checked(diagnosticsUpload_) && uploadTarget_.value("available").toBool();
    if (upload) { params["upload"] = true; params["copyLink"] = true; params["profileId"] = uploadTarget_.value("profileId"); }
    main_.flushSettings([this, params, upload](bool) {
        if (upload) main_.status(L"Saving and uploading the diagnostics package\x2026");
        main_.session().call("diagnostics.bundle", params, [this](const Reply& reply) {
            enable(diagnostics_, true);
            if (!reply.ok) { main_.status(UiSession::describe(reply), true); return; }
            diagnosticsPath_ = reply.result.value("path").toString();
            setTextW(diagnosticsResult_, L"Saved " + wide(diagnosticsPath_));
            // The saved file stays either way; an upload that fails only adds its reason.
            if (reply.result.contains("link")) setTextW(uploadNote_, L"Uploaded: " + wide(reply.result.value("link").toString()) + (reply.result.value("linkCopied").toBool() ? L" (link copied)" : L""));
            else if (reply.result.contains("uploadError")) setTextW(uploadNote_, L"Not uploaded: " + wide(reply.result.value("uploadError")["message"].toString()));
            main_.status(reply.result.contains("link") ? L"Diagnostics package saved and uploaded." : L"Diagnostics package saved.");
            panels_[Advanced].layout();
        });
    });
}

void SettingsPageImpl::command(int id, int code, HWND control) {
    if (id == idSections) {
        if (code == LBN_SELCHANGE) setSection(int(SendMessageW(sections_, LB_GETCURSEL, 0, 0)));
        return;
    }
    if (populating_) return;
    if (id >= idField && id < idField + int(specs.size())) {
        if (code == EN_CHANGE) fieldChanged(size_t(id - idField));
        return;
    }
    if (id >= idBrowse && id < idBrowse + int(specs.size())) {
        const auto index = size_t(id - idBrowse);
        std::string initial = text::trim(text(fields_[index].edit));
        if (initial.empty()) initial = main_.session().document().value("archive")["root"].toString();
        std::string chosen;
        if (pickFolder(main_.hwnd(), L"Choose a folder", initial, chosen)) setText(fields_[index].edit, chosen); // EN_CHANGE saves it
        return;
    }
    if (id >= idActionDestination && id < idActionDestination + int(actions_.size())) {
        if (code == CBN_SELCHANGE) actionChanged(size_t(id - idActionDestination), false);
        return;
    }
    if (id >= idActionSharing && id < idActionSharing + int(actions_.size())) {
        if (code == CBN_SELCHANGE) actionChanged(size_t(id - idActionSharing), true);
        return;
    }
    if (id >= idRecord && id < idRecord + int(shortcuts_.size())) {
        if (code == BN_CLICKED) {
            if (recording_ == id - idRecord) finishRecording(std::nullopt);
            else startRecording(size_t(id - idRecord));
        }
        return;
    }
    if (id >= idClear && id < idClear + int(shortcuts_.size())) {
        if (code == BN_CLICKED) {
            const auto& row = shortcuts_[size_t(id - idClear)];
            setText(row.edit, "");
            setError("/actions/" + row.name + "/shortcut", L"");
            main_.queuePatch({{"actions", json::Object{{row.name, json::Object{{"shortcut", ""}}}}}});
        }
        return;
    }
    const bool selection = code == CBN_SELCHANGE, click = code == BN_CLICKED;
    switch (id) {
    case idDefaultDestination: case idDefaultSharing: if (selection) captureDefaultChanged(); break;
    case idKeepLocal: if (click) main_.queuePatch({{"archive", json::Object{{"keepUploads", checked(keepLocal_)}}}}); break;
    case idNamingMode: if (selection) { main_.queuePatch({{"naming", json::Object{{"mode", selected(naming_)}}}}); updateEnabled(); } break;
    case idCollision:
        if (selection) {
            if (selected(collision_) == "overwrite" &&
                !confirm(main_.hwnd(), L"put.gd", L"Replace existing objects?", L"An upload with the same name replaces the checked object at the destination where the provider supports guarded overwrite.", L"Replace")) {
                populating_ = true;
                ui::select(collision_, main_.session().document().value("naming")["collisionPolicy"].toString("rename"));
                populating_ = false;
                break;
            }
            main_.queuePatch({{"naming", json::Object{{"collisionPolicy", selected(collision_)}}}});
        }
        break;
    case idClipboard: if (selection) main_.queuePatch({{"preferences", json::Object{{"uploadClipboardBehavior", selected(clipboard_)}}}}); break;
    case idNotifications: if (click) main_.queuePatch({{"preferences", json::Object{{"notifications", checked(notifications_)}}}}); break;
    case idLaunch: if (click) main_.queuePatch({{"preferences", json::Object{{"launchAtLogin", checked(launch_)}}}}); break;
    case idUnlockStartup: if (click) main_.queuePatch({{"preferences", json::Object{{"unlockCredentialsAtStartup", checked(unlockStartup_)}}}}); break;
    case idTheme:
        if (selection) {
            main_.queuePatch({{"preferences", json::Object{{"appearance", json::Object{{"theme", selected(theme_)}}}}}});
            main_.applyTheme(selected(theme_));
        }
        break;
    case idDebug: if (click) main_.queuePatch({{"preferences", json::Object{{"diagnostics", json::Object{{"debugLogging", checked(debug_)}}}}}}); break;
    case idUpdateCheck: if (click) main_.queuePatch({{"preferences", json::Object{{"updates", json::Object{{"check", checked(updateCheck_)}}}}}}); break;
    case idUpdateChannel: if (selection) main_.queuePatch({{"preferences", json::Object{{"updates", json::Object{{"channel", selected(channel_)}}}}}}); break;
    case idCheckNow:
        if (click) {
            checking_ = true;
            updateEnabled();
            setTextW(updateStatus_, L"Checking\x2026");
            main_.session().call("update.check", {}, [this](const Reply& reply) {
                checking_ = false;
                updateEnabled();
                if (reply.ok) showUpdateStatus(reply.result);
                else { setTextW(updateStatus_, UiSession::describe(reply)); panels_[Advanced].layout(); }
            });
        }
        break;
    case idClearLogs:
        if (click && confirm(main_.hwnd(), L"Clear logs", L"Delete every put.gd log file?", L"A new log starts right away. Diagnostics packages already saved are kept.", L"Clear logs"))
            main_.session().call("logs.clear", {}, [this](const Reply& reply) { main_.status(reply.ok ? L"Logs cleared." : UiSession::describe(reply), !reply.ok); });
        break;
    case idDiagnostics: if (click) saveDiagnostics(); break;
    case idCrashDumps: if (click) main_.queuePatch({{"preferences", json::Object{{"diagnostics", json::Object{{"debugLogging", checked(debug_)}, {"crashDumps", checked(crashDumps_)}}}}}}); break;
    case idDiagnosticsUpload: if (click) checkUploadTarget(); break;
    case idShowDiagnostics: if (click) revealInExplorer(diagnosticsPath_); break;
    case idReload:
        if (click) {
            main_.session().call("settings.reload", {}, [this](const Reply& reply) {
                if (!reply.ok) {
                    const auto path = reply.error.details.value("path").toString();
                    main_.status(L"The settings file is not valid" + (path.empty() ? std::wstring() : L" at " + wide(path)) + L". The last accepted settings stay active.", true);
                    return;
                }
                main_.reloadSettings();
            });
        }
        break;
    case idResetLimits:
        if (click && confirm(main_.hwnd(), L"Reset limits", L"Reset every limit to its default?", L"Only the limits change; other settings stay.", L"Reset")) {
            const auto defaults = Settings::defaults(paths::documentsFolder()).value("limits").toObject();
            for (const auto& spec : specs) if (limitField(spec.path)) invalid_.erase(spec.path);
            main_.queuePatch({{"limits", defaults}});
            main_.flushSettings([this](bool saved) { if (saved) populate(true); });
        }
        break;
    }
}
}

std::unique_ptr<Page> createSettingsPage(MainWindow& main) { return std::make_unique<SettingsPageImpl>(main); }
}
