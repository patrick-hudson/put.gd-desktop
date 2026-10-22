// Destinations: the profiles list (add, remove, reorder, make default), the S3
// form and the Proton Drive form. Valid non-secret edits autosave through the
// window's settings.patch queue; credentials go only to setup.storeCredentials and
// link passwords only to setup.storeSharePassword, and neither is kept.
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "common/Text.h"
#include "common/Uuid.h"
#include "core/S3Settings.h"
#include "core/Settings.h"
#include <algorithm>
#include <cstdio>
#include <set>

namespace putgd::ui {
namespace {
enum : int {
    idList = 100, idAdd, idRemove, idUp, idDown, idDefault, idAddS3, idAddProton,
    idLabel = 200, idBucket, idRegion, idEndpoint, idPrefix, idStorageClass, idAcl, idPublicUrl, idAddressing,
    idConnect, idRequest, idLowSpeed, idLowSpeedTime, idAccess, idSecret, idShowSecret, idToken, idKeychain, idSession,
    idStore, idUnlock, idTest,
    idProtonConnect = 300, idProtonRefresh, idProtonFolder, idProtonNewFolder, idProtonFolderName, idProtonCreateFolder, idProtonCancelFolder,
    idProtonShare, idProtonPrivate, idProtonExpiration, idProtonTimeout, idProtonPassword, idProtonStorePassword, idProtonClearPassword
};
constexpr UINT_PTR timerSetup = 1;

// Alt+Up / Alt+Down move the selected destination, as in the Linux list.
LRESULT CALLBACK listKeys(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (message == WM_SYSKEYDOWN && (wParam == VK_UP || wParam == VK_DOWN)) {
        PostMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(wParam == VK_UP ? idUp : idDown, BN_CLICKED), reinterpret_cast<LPARAM>(hwnd));
        return 0;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}
const std::vector<std::string> textFields{"label", "bucket", "region", "endpoint", "objectPrefix", "publicUrlBase",
                                          "connectTimeoutMs", "requestTimeoutMs", "lowSpeedBytesPerSecond", "lowSpeedTimeSeconds"};
const std::vector<std::string> numberFields{"connectTimeoutMs", "requestTimeoutMs", "lowSpeedBytesPerSecond", "lowSpeedTimeSeconds"};

std::string derivedEndpoint(const std::string& region) { return region.empty() ? std::string() : "https://s3." + region + ".amazonaws.com"; }

std::wstring urlError(const std::string& value, bool optional) {
    if (value.empty()) return optional ? std::wstring() : L"Enter the endpoint address, such as https://s3.us-east-1.amazonaws.com.";
    const auto url = text::parseUrl(value);
    if (!url.valid || url.host.empty() || !url.userInfo.empty() || url.hasQuery || url.hasFragment ||
        (url.scheme != "https" && !(url.scheme == "http" && s3::loopbackHost(url.host))))
        return optional ? L"Use an https:// address without a query, or leave it empty to use the bucket address."
                        : L"Use an https:// address without user name, query or fragment.";
    return {};
}

constexpr const char* protonProvider = "proton-drive";

// 32 lowercase hex characters from a random UUID (credential and link-password references).
std::string randomHex32() {
    auto id = uuid::create();
    id.erase(std::remove(id.begin(), id.end(), '-'), id.end());
    return text::lower(id);
}

// requestTimeoutMs shown as seconds with up to three decimals ("120", "90.5").
std::string secondsText(const json::Value& value) {
    if (!value.isNumber()) return value.toString();
    const long long ms = value.toInteger(-1);
    if (ms < 0) return {};
    std::string out = std::to_string(ms / 1000);
    if (ms % 1000) {
        char fraction[8]{};
        std::snprintf(fraction, sizeof fraction, ".%03lld", ms % 1000);
        std::string f = fraction;
        while (!f.empty() && f.back() == '0') f.pop_back();
        out += f;
    }
    return out;
}

// "120", "90.5" or "0,25" -> milliseconds; anything else stays a string so validation reports it.
json::Value parseSeconds(const std::string& raw) {
    std::string value = raw;
    std::replace(value.begin(), value.end(), ',', '.');
    const auto dot = value.find('.');
    const std::string whole = value.substr(0, dot), fraction = dot == std::string::npos ? std::string() : value.substr(dot + 1);
    auto digits = [](const std::string& s) { return std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }); };
    if (whole.empty() || whole.size() > 6 || !digits(whole) || !digits(fraction) || fraction.size() > 3 || (dot != std::string::npos && fraction.empty()))
        return json::Value(raw);
    long long ms = std::stoll(whole) * 1000;
    if (!fraction.empty()) ms += std::stoll(fraction + std::string(3 - fraction.size(), '0'));
    return json::Value(ms);
}

// Setup errors: the Proton codes show the resident's message (it names the problem);
// everything else keeps the window's standard wording.
std::wstring setupError(const Reply& reply) {
    static const std::map<std::string, const wchar_t*> fallback{
        {"proton_runtime_unavailable", L"The Proton Drive helper is not installed."},
        {"proton_account_busy", L"This Proton account is busy with another request. Try again shortly."},
        {"account_reference_in_use", L"Another destination already uses this account."},
        {"keychain_unavailable", L"The private keychain is unavailable."},
        {"authentication_required", L"Sign in to Proton again with Connect account."},
        {"proton_setup_required", L"Connect the Proton account first."},
        {"proton_account_changed", L"The signed-in Proton account changed. Refresh folders or reconnect."},
        {"proton_folder_unavailable", L"The selected Proton folder is not available. Choose another folder."},
        {"setup_unavailable", L"This setup step is not available."},
        {"setup_busy", L"Another setup step is running for this destination. Wait for it to finish."}};
    if (auto it = fallback.find(reply.error.code); it != fallback.end()) return reply.error.message.empty() ? std::wstring(it->second) : wide(reply.error.message);
    return UiSession::describe(reply);
}

std::wstring stepText(const json::Value& value) {
    if (value.isString()) return wide(value.stringRef());
    if (value.isBool()) return value.toBool() ? L"passed" : L"failed";
    if (value.isObject()) {
        const auto& o = value.objectRef();
        std::string status = o.value("status").toString();
        if (status.empty() && o.value("ok").isBool()) status = o.value("ok").toBool() ? "passed" : "failed";
        std::wstring text = wide(status.empty() ? std::string("done") : status);
        const auto message = o.value("message").toString(o.value("error")["message"].toString());
        if (!message.empty()) text += L" \x2014 " + wide(message);
        return text;
    }
    return L"not run";
}

class DestinationsPageImpl final : public Page {
public:
    using Page::Page;
    HWND create(HWND parent) override;
    void activated() override {
        if (draft_.empty() && provisional_.empty()) rebuild();
        refreshHint();
        loadCapabilities();
        maybeInspect();
    }
    void deactivated() override { hideRevealed(); }
    void settingsChanged(bool external) override;
    void invalidated(const std::string& kind) override { if (kind == "setup") refreshHint(); }
    bool validationFailed(const std::string& path, const std::wstring& message) override;
    bool dirty() const override;
    void discardDraft() override { provisional_.clear(); errors_.clear(); clearSecrets(); }
    bool pendingSecrets() const override {
        return GetWindowTextLengthW(protonPassword_) || (!revealed_ && (GetWindowTextLengthW(access_) || GetWindowTextLengthW(secret_) || GetWindowTextLengthW(token_)));
    }
    void clearSecrets() override;
    json::Object saveState() const override { return selected_ >= 0 ? json::Object{{"profileIndex", selected_}} : json::Object{}; }
    void restoreState(const json::Object& state) override { if (state.value("profileIndex").isNumber()) restoreIndex_ = state.value("profileIndex").toInt(); }
    void dpiChanged() override { panel_.layout(); form_.layout(); }
    // ui.moveDestination: Move up / Move down on the selected (or given) destination.
    bool viewCommand(const std::string& name, const json::Object& params, const CommandReply& answer) override;
private:
    int layoutPage(int width, int height);
    int layoutForm(int width, int height);
    void command(int id, int code, HWND control);
    void rebuild();
    void refreshList();
    void select(int index);
    void populate();
    void fieldChanged(int id);
    void readForm();
    std::map<std::string, std::wstring> validate(const json::Object& profile) const;
    json::Array savableProfiles() const;
    json::Object documentProfile(const std::string& id) const;
    bool saved(const std::string& id) const { return !documentProfile(id).empty(); }
    bool isDefault(const std::string& id) const;
    json::Object* current();
    void showErrors();
    void addS3();
    void remove();
    void move(int delta);
    void makeDefault();
    void store();
    void unlock();
    void test();
    void toggleSecret();
    void hideRevealed();
    void refreshHint();
    void startJob(const std::string& kind, const Reply& reply, const std::string& profileId);
    void poll();
    void updateButtons();
    // ---- Proton Drive ----
    void addProton();
    void populateProton(const json::Object& profile);
    void loadCapabilities();
    bool protonKnownUnavailable() const;
    bool protonAvailable() const;
    bool jobRunning(const std::string& profileId, std::initializer_list<const char*> kinds) const;
    void beginSetup(const std::string& kind, json::Object input = {}, bool withPassword = false);
    void maybeInspect();
    void createFolder();
    void clearLinkPassword();
    void saveSettings(const std::string& profileId, const json::Object& settings);
    void applyAccount(const std::string& profileId, const json::Object& result);
    void applyFolder(const std::string& profileId, const json::Object& result);
    void applyLinkPassword(const std::string& profileId, const json::Object& result);
    std::string folderName(const std::string& uid) const;

    Panel panel_, form_;
    HWND list_{}, add_{}, remove_{}, up_{}, down_{}, default_{};
    HWND empty_{}, title_{}, provider_{}, general_{};
    std::map<std::string, HWND> fields_, labels_, errorLabels_;
    HWND storageClass_{}, acl_{}, addressing_{};
    HWND advanced_{}, credentialsHeading_{}, accessLabel_{}, secretLabel_{}, tokenLabel_{}, access_{}, secret_{}, show_{}, token_{};
    HWND keychain_{}, session_{}, store_{}, unlock_{}, hint_{}, testHeading_{}, test_{}, testResult_{};
    json::Array draft_;
    std::set<std::string> provisional_;
    std::map<std::string, std::map<std::string, std::wstring>> errors_;
    std::map<std::string, std::wstring> testResults_, generalErrors_;
    int selected_ = -1, restoreIndex_ = -1;
    bool reordering_ = false; // a settings.reorderProfiles request is on its way
    bool populating_ = false, revealed_ = false, typedVisible_ = false, secretStored_ = false;
    unsigned long long revealToken_ = 0, hintToken_ = 0;
    struct Job { std::string setupId, kind, profileId; };
    std::vector<Job> jobs_;
    bool polling_ = false;

    // Proton Drive controls (shown only for a proton-drive profile).
    HWND protonAccountHeading_{}, protonAccountLabel_{}, protonAccount_{}, protonConnect_{}, protonRefresh_{};
    HWND protonWhereHeading_{}, protonFolderLabel_{}, protonFolder_{}, protonNewFolder_{}, protonFolderStatus_{};
    HWND protonFolderNameLabel_{}, protonFolderName_{}, protonCreateFolder_{}, protonCancelFolder_{};
    HWND protonAfterHeading_{}, protonShare_{}, protonPrivate_{};
    HWND protonLinkHeading_{}, protonExpirationLabel_{}, protonExpiration_{}, protonExpirationHint_{}, protonTimeoutLabel_{}, protonTimeout_{};
    HWND protonPasswordLabel_{}, protonPassword_{}, protonStorePassword_{}, protonClearPassword_{}, protonPasswordState_{};
    // What the last connectAccount/inspectAccount reported, per profile.
    struct ProtonAccount { std::string root, email; std::vector<std::pair<std::string, std::string>> folders; };
    std::map<std::string, ProtonAccount> accounts_;
    std::map<std::string, std::wstring> accountStatus_; // transient account line (connecting, sign-in, failure)
    std::vector<std::pair<std::string, std::string>> shownFolders_; // the Upload folder choices: uid, name
    std::set<std::string> requestedFolders_, authOpened_;
    std::set<std::pair<std::string, std::string>> starting_; // profileId, kind: setup requests sent, not yet answered
    json::Object providers_; // capabilities.providers
    bool capabilitiesLoaded_ = false, capabilitiesRequested_ = false, newFolderOpen_ = false;
};

HWND DestinationsPageImpl::create(HWND parent) {
    panel_.onLayout = [this](int w, int h) { return layoutPage(w, h); };
    panel_.onCommand = [this](int id, int code, HWND control) { command(id, code, control); };
    panel_.onTimer = [this](UINT_PTR id) { if (id == timerSetup) poll(); };
    HWND p = panel_.create(parent, false);
    list_ = listBox(p, idList, true);
    SetWindowSubclass(list_, listKeys, 1, 0);
    add_ = button(p, idAdd, L"Add destination\x2026");
    theme::setRole(add_, theme::Role::Primary);
    remove_ = button(p, idRemove, L"Remove\x2026");
    theme::setRole(remove_, theme::Role::Danger);
    up_ = button(p, idUp, L"Move up");
    down_ = button(p, idDown, L"Move down");
    default_ = button(p, idDefault, L"Make default");
    setTooltip(main_.tooltip(), default_, L"Use this destination for every capture that follows the default output.");
    setTooltip(main_.tooltip(), up_, L"The order is also the order of every Destination list. Shortcut: Alt+Up.");
    setTooltip(main_.tooltip(), down_, L"The order is also the order of every Destination list. Shortcut: Alt+Down.");

    form_.onLayout = [this](int w, int h) { return layoutForm(w, h); };
    form_.onCommand = [this](int id, int code, HWND control) { command(id, code, control); };
    HWND f = form_.create(p, true);
    ShowWindow(f, SW_SHOW);
    empty_ = ui::label(f, L"No destinations yet. Choose Add destination to upload captures to S3 or an S3-compatible service.");
    form_.markMuted(empty_);
    title_ = heading(f, L"");
    setFont(title_, FontRole::Subtitle, form_.dpi());
    provider_ = ui::label(f, L"Amazon S3 or S3-compatible service");
    form_.markMuted(provider_);
    general_ = ui::label(f, L"");
    form_.markError(general_, true);
    const std::vector<std::tuple<std::string, int, const wchar_t*, const wchar_t*>> specs{
        {"label", idLabel, L"Destination name", L""},
        {"bucket", idBucket, L"Bucket", L""},
        {"region", idRegion, L"Region", L"us-east-1"},
        {"endpoint", idEndpoint, L"Endpoint", L"https://s3.us-east-1.amazonaws.com"},
        {"objectPrefix", idPrefix, L"Folder in bucket", L"Optional. Leave empty to upload to the bucket root."},
        {"publicUrlBase", idPublicUrl, L"Link domain", L"Optional, such as https://cdn.example.com"},
        {"connectTimeoutMs", idConnect, L"Connect timeout (ms)", L""},
        {"requestTimeoutMs", idRequest, L"Request timeout (ms)", L""},
        {"lowSpeedBytesPerSecond", idLowSpeed, L"Minimum transfer speed (bytes/s)", L""},
        {"lowSpeedTimeSeconds", idLowSpeedTime, L"Low-speed window (s)", L""}};
    for (const auto& [key, id, caption, cue] : specs) {
        labels_[key] = ui::label(f, caption);
        fields_[key] = edit(f, id, std::find(numberFields.begin(), numberFields.end(), key) != numberFields.end() ? ES_NUMBER : 0);
        if (*cue) setCue(fields_[key], cue);
        errorLabels_[key] = ui::label(f, L"");
        form_.markError(errorLabels_[key], true);
    }
    SendMessageW(fields_["label"], EM_LIMITTEXT, 128, 0);
    SendMessageW(fields_["bucket"], EM_LIMITTEXT, 63, 0);
    labels_["storageClass"] = ui::label(f, L"Storage class");
    storageClass_ = combo(f, idStorageClass);
    setChoices(storageClass_, {{L"Standard", "STANDARD"}, {L"Reduced redundancy", "REDUCED_REDUNDANCY"}, {L"Standard-IA", "STANDARD_IA"},
                               {L"One Zone-IA", "ONEZONE_IA"}, {L"Intelligent-Tiering", "INTELLIGENT_TIERING"}, {L"Glacier Flexible Retrieval", "GLACIER"},
                               {L"Glacier Deep Archive", "DEEP_ARCHIVE"}, {L"Glacier Instant Retrieval", "GLACIER_IR"}});
    setTooltip(main_.tooltip(), storageClass_, L"Cold storage classes may not allow an immediate public link.");
    labels_["aclMode"] = ui::label(f, L"Who can open files");
    acl_ = combo(f, idAcl);
    setChoices(acl_, {{L"Request public read access", "public-read"}, {L"Follow the bucket's policy (send no ACL)", "omit"}});
    labels_["addressing"] = ui::label(f, L"Addressing");
    addressing_ = combo(f, idAddressing);
    setChoices(addressing_, {{L"Automatic", "auto"}, {L"Bucket in URL path", "path"}, {L"Bucket in hostname", "virtual"}});
    for (const auto* key : {"storageClass", "aclMode", "addressing"}) { errorLabels_[key] = ui::label(f, L""); form_.markError(errorLabels_[key], true); }
    advanced_ = heading(f, L"Advanced connection settings");
    credentialsHeading_ = heading(f, L"Credentials");
    accessLabel_ = ui::label(f, L"Access key ID");
    access_ = edit(f, idAccess);
    secretLabel_ = ui::label(f, L"Secret access key");
    secret_ = password(f, idSecret);
    show_ = button(f, idShowSecret, L"Show");
    tokenLabel_ = ui::label(f, L"Session token");
    token_ = password(f, idToken);
    setCue(token_, L"Optional, for temporary AWS credentials.");
    keychain_ = radio(f, idKeychain, L"Remember in Windows Credential Manager", true);
    session_ = radio(f, idSession, L"This session only (forgotten when put.gd exits)", false);
    store_ = button(f, idStore, L"Store credentials");
    theme::setRole(store_, theme::Role::Primary);
    unlock_ = button(f, idUnlock, L"Unlock saved credentials");
    hint_ = ui::label(f, L"");
    form_.markMuted(hint_);

    // Proton Drive: Account, Where files go, After upload, Link settings.
    protonAccountHeading_ = heading(f, L"Account");
    protonAccountLabel_ = ui::label(f, L"Status");
    protonAccount_ = ui::label(f, L"Not connected");
    protonConnect_ = button(f, idProtonConnect, L"Connect account\x2026");
    theme::setRole(protonConnect_, theme::Role::Primary);
    setTooltip(main_.tooltip(), protonConnect_, L"Sign in to Proton in your browser. The session stays in Windows Credential Manager under put.gd's own name.");
    protonRefresh_ = button(f, idProtonRefresh, L"Refresh folders");
    setTooltip(main_.tooltip(), protonRefresh_, L"Reload the available Proton folders without changing the selected upload folder.");
    protonWhereHeading_ = heading(f, L"Where files go");
    protonFolderLabel_ = ui::label(f, L"Upload folder");
    protonFolder_ = combo(f, idProtonFolder);
    setTooltip(main_.tooltip(), protonFolder_, L"Choose My files or a top-level folder in it, or create a new folder. The choice applies to future uploads.");
    protonNewFolder_ = button(f, idProtonNewFolder, L"New folder\x2026");
    protonFolderStatus_ = ui::label(f, L"");
    form_.markMuted(protonFolderStatus_);
    protonFolderNameLabel_ = ui::label(f, L"Folder name");
    protonFolderName_ = edit(f, idProtonFolderName);
    SendMessageW(protonFolderName_, EM_LIMITTEXT, 255, 0);
    protonCreateFolder_ = button(f, idProtonCreateFolder, L"Create folder");
    theme::setRole(protonCreateFolder_, theme::Role::Primary);
    protonCancelFolder_ = button(f, idProtonCancelFolder, L"Cancel");
    protonAfterHeading_ = heading(f, L"After upload");
    protonShare_ = radio(f, idProtonShare, L"Create a share link and copy it", true);
    setTooltip(main_.tooltip(), protonShare_, L"Create a share link for future Proton uploads and copy it. Link expiration and password apply to new shares.");
    protonPrivate_ = radio(f, idProtonPrivate, L"Keep private, copy nothing", false);
    setTooltip(main_.tooltip(), protonPrivate_, L"Upload privately without creating a link or changing the clipboard. Existing member or folder access still applies.");
    protonLinkHeading_ = heading(f, L"Link settings");
    protonExpirationLabel_ = ui::label(f, L"Link expiration (days)");
    protonExpiration_ = edit(f, idProtonExpiration, ES_NUMBER | WS_GROUP); // WS_GROUP ends the radio group above
    SendMessageW(protonExpiration_, EM_LIMITTEXT, 3, 0);
    setTooltip(main_.tooltip(), protonExpiration_, L"Applies to newly created links; existing shares are unchanged.");
    protonExpirationHint_ = ui::label(f, L"0 = No expiration", 0, SS_CENTERIMAGE);
    form_.markMuted(protonExpirationHint_);
    protonTimeoutLabel_ = ui::label(f, L"Request deadline (s)");
    protonTimeout_ = edit(f, idProtonTimeout);
    SendMessageW(protonTimeout_, EM_LIMITTEXT, 10, 0);
    setTooltip(main_.tooltip(), protonTimeout_, L"Overall Proton request deadline in seconds, with millisecond precision. Default 120 seconds; up to 86400.");
    protonPasswordLabel_ = ui::label(f, L"New link password");
    protonPassword_ = password(f, idProtonPassword);
    setCue(protonPassword_, L"Optional, for future share links");
    protonStorePassword_ = button(f, idProtonStorePassword, L"Store link password");
    setTooltip(main_.tooltip(), protonStorePassword_, L"Stores the password in Windows Credential Manager; settings keep only its reference. Existing links are unchanged.");
    protonClearPassword_ = button(f, idProtonClearPassword, L"Use no link password");
    protonPasswordState_ = ui::label(f, L"");
    form_.markMuted(protonPasswordState_);
    for (const auto* key : {"linkExpirationDays", "protonRequestTimeoutMs", "protonFolder"}) { errorLabels_[key] = ui::label(f, L""); form_.markError(errorLabels_[key], true); }

    testHeading_ = heading(f, L"Test connection");
    test_ = button(f, idTest, L"Test connection");
    setTooltip(main_.tooltip(), test_, L"Uploads a small synthetic file, checks its public link and removes it again.");
    testResult_ = ui::label(f, L"");
    return p;
}

int DestinationsPageImpl::layoutPage(int width, int height) {
    const UINT d = panel_.dpi();
    const int margin = scale(24, d), gap = scale(8, d), left = scale(260, d);
    Rows rows(d, margin, 0, left, 0);
    const int buttonHeight = rows.buttonHeight();
    const int top = scale(4, d), bottom = height - scale(12, d);
    const int listBottom = bottom - 3 * buttonHeight - 2 * gap - scale(12, d);
    place(list_, margin, top, left, listBottom - top);
    SetPropW(list_, L"putgd.dividerRight", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(scale(14, d))));
    int y = listBottom + scale(12, d);
    place(add_, margin, y, left, buttonHeight);
    y += buttonHeight + gap;
    const int half = (left - gap) / 2;
    place(up_, margin, y, half, buttonHeight);
    place(down_, margin + half + gap, y, half, buttonHeight);
    y += buttonHeight + gap;
    place(default_, margin, y, half, buttonHeight);
    place(remove_, margin + half + gap, y, half, buttonHeight);
    const int formX = margin + left + scale(28, d);
    place(form_.hwnd(), formX, 0, width - formX, height);
    return height;
}

int DestinationsPageImpl::layoutForm(int width, int height) {
    const UINT d = form_.dpi();
    Rows rows(d, 0, scale(4, d) + form_.offsetY(), std::clamp(width - scale(24, d), scale(300, d), scale(700, d)), 200);
    auto* profile = current();
    const bool any = profile != nullptr;
    const bool s3 = any && profile->value("provider") == "s3";
    const bool proton = any && profile->value("provider") == protonProvider;
    show(empty_, !any);
    for (HWND h : {title_, provider_, labels_["label"], fields_["label"]}) show(h, any);
    for (auto& [key, label] : errorLabels_) show(label, false);
    for (HWND h : {credentialsHeading_, accessLabel_, secretLabel_, tokenLabel_, access_, secret_, show_, token_, keychain_, session_, store_, unlock_, hint_,
                   advanced_, storageClass_, acl_, addressing_})
        show(h, s3);
    for (HWND h : {testHeading_, test_, testResult_}) show(h, s3 || proton);
    for (HWND h : {protonAccountHeading_, protonAccountLabel_, protonAccount_, protonConnect_, protonRefresh_, protonWhereHeading_, protonFolderLabel_, protonFolder_,
                   protonNewFolder_, protonAfterHeading_, protonShare_, protonPrivate_, protonLinkHeading_, protonExpirationLabel_, protonExpiration_,
                   protonExpirationHint_, protonTimeoutLabel_, protonTimeout_, protonPasswordLabel_, protonPassword_, protonStorePassword_, protonClearPassword_,
                   protonPasswordState_})
        show(h, proton);
    for (HWND h : {protonFolderNameLabel_, protonFolderName_, protonCreateFolder_, protonCancelFolder_}) show(h, proton && newFolderOpen_);
    show(protonFolderStatus_, proton && GetWindowTextLengthW(protonFolderStatus_) > 0);
    for (const auto& key : textFields) if (key != "label") { show(labels_[key], s3); show(fields_[key], s3); }
    for (const auto* key : {"storageClass", "aclMode", "addressing"}) show(labels_[key], s3);
    if (!any) { rows.text(empty_, 2); return rows.y() - form_.offsetY(); }
    const int line = lineHeight(d);
    rows.heading(title_);
    rows.space(-10);
    rows.text(provider_);
    rows.space(6);
    if (GetWindowTextLengthW(general_)) { show(general_, true); rows.text(general_, 2); } else show(general_, false);
    auto field = [&](const std::string& key, int fieldWidth = 0) { rows.field(labels_[key], fields_[key], fieldWidth); rows.error(errorLabels_[key]); };
    field("label", 460);
    if (proton) {
        rows.heading(protonAccountHeading_);
        const int statusHeight = textHeight(protonAccount_, rows.controlWidth()) + rows.px(2);
        place(protonAccountLabel_, rows.x(), rows.y(), rows.px(188), line + rows.px(2));
        rows.block(protonAccount_, statusHeight);
        rows.line({{protonConnect_, -1}, {protonRefresh_, -1}}, true);
        rows.heading(protonWhereHeading_);
        rows.field(protonFolderLabel_, protonFolder_, 340, protonNewFolder_, -1);
        rows.error(errorLabels_["protonFolder"]);
        if (GetWindowTextLengthW(protonFolderStatus_)) rows.block(protonFolderStatus_, textHeight(protonFolderStatus_, rows.controlWidth()) + rows.px(2));
        if (newFolderOpen_) {
            rows.field(protonFolderNameLabel_, protonFolderName_, 340);
            rows.line({{protonCreateFolder_, -1}, {protonCancelFolder_, -1}}, true);
        }
        rows.heading(protonAfterHeading_);
        rows.line({{protonShare_, 380}}, true, rows.fieldHeight());
        rows.line({{protonPrivate_, 380}}, true, rows.fieldHeight());
        rows.heading(protonLinkHeading_);
        rows.field(protonExpirationLabel_, protonExpiration_, 100, protonExpirationHint_, 200);
        rows.error(errorLabels_["linkExpirationDays"]);
        rows.field(protonTimeoutLabel_, protonTimeout_, 140);
        rows.error(errorLabels_["protonRequestTimeoutMs"]);
        rows.field(protonPasswordLabel_, protonPassword_, 340);
        rows.line({{protonStorePassword_, -1}, {protonClearPassword_, -1}}, true);
        rows.block(protonPasswordState_, textHeight(protonPasswordState_, rows.controlWidth()) + rows.px(2));
    }
    if (!s3) {
        if (!proton) return rows.y() - form_.offsetY();
        rows.heading(testHeading_);
        rows.line({{test_, -1}}, true);
        const auto& result = testResults_[profile->value("id").toString()];
        const int resultLines = 1 + int(std::count(result.begin(), result.end(), L'\n'));
        rows.block(testResult_, std::max(line * (resultLines + 1), textHeight(testResult_, rows.controlWidth()) + rows.px(2)));
        return rows.y() - form_.offsetY();
    }
    field("bucket", 460);
    field("region", 240);
    field("endpoint", 460);
    field("objectPrefix", 460);
    rows.field(labels_["aclMode"], acl_, 460);
    rows.error(errorLabels_["aclMode"]);
    field("publicUrlBase", 460);
    rows.heading(advanced_);
    rows.field(labels_["storageClass"], storageClass_, 300);
    rows.error(errorLabels_["storageClass"]);
    rows.field(labels_["addressing"], addressing_, 300);
    rows.error(errorLabels_["addressing"]);
    for (const auto& key : numberFields) field(key, 160);
    rows.heading(credentialsHeading_);
    rows.field(accessLabel_, access_, 460);
    rows.field(secretLabel_, secret_, 460 - 88, show_, 80);
    rows.field(tokenLabel_, token_, 460);
    rows.line({{keychain_, 380}}, true, rows.fieldHeight());
    rows.line({{session_, 380}}, true, rows.fieldHeight());
    rows.line({{store_, -1}, {unlock_, -1}}, true);
    if (GetWindowTextLengthW(hint_)) rows.block(hint_, line * 2 + 2);
    rows.heading(testHeading_);
    rows.line({{test_, -1}}, true);
    const auto& result = testResults_[profile->value("id").toString()];
    const int resultLines = 1 + int(std::count(result.begin(), result.end(), L'\n'));
    rows.block(testResult_, line * (resultLines + 1));
    return rows.y() - form_.offsetY();
}

json::Object DestinationsPageImpl::documentProfile(const std::string& id) const {
    for (const auto& entry : main_.session().document().value("profiles").arrayRef())
        if (entry.objectRef().value("id") == id.c_str()) return entry.objectRef();
    return {};
}

bool DestinationsPageImpl::isDefault(const std::string& id) const {
    const auto& document = main_.session().document();
    if (document.contains("captureDefaults")) {
        const auto output = document.value("captureDefaults").toObject();
        return output.value("destination") == "upload" && output.value("profileId") == id.c_str();
    }
    const auto region = Settings::effectiveAction(document, "region");
    return region.value("destination") == "upload" && region.value("profileId") == id.c_str();
}

json::Object* DestinationsPageImpl::current() {
    if (selected_ < 0 || size_t(selected_) >= draft_.size()) return nullptr;
    return &draft_[size_t(selected_)].objectRef();
}

bool DestinationsPageImpl::dirty() const {
    if (!provisional_.empty()) return true;
    for (const auto& [id, fields] : errors_) if (!fields.empty()) return true;
    return false;
}

void DestinationsPageImpl::rebuild() {
    std::string keep = current() ? current()->value("id").toString() : std::string();
    json::Array next;
    for (const auto& entry : main_.session().document().value("profiles").arrayRef()) {
        const auto id = entry.objectRef().value("id").toString();
        errors_.erase(id);
        next.push_back(entry);
    }
    for (const auto& entry : draft_)
        if (provisional_.count(entry.objectRef().value("id").toString())) next.push_back(entry);
    draft_ = std::move(next);
    int index = -1;
    for (size_t i = 0; i < draft_.size(); ++i) if (draft_[i].objectRef().value("id") == keep.c_str()) index = int(i);
    if (index < 0 && restoreIndex_ >= 0 && size_t(restoreIndex_) < draft_.size()) index = restoreIndex_;
    restoreIndex_ = -1;
    if (index < 0 && !draft_.empty()) index = 0;
    selected_ = index;
    refreshList();
    populate();
}

void DestinationsPageImpl::settingsChanged(bool external) {
    for (auto it = provisional_.begin(); it != provisional_.end();) it = saved(*it) ? provisional_.erase(it) : std::next(it);
    if (external) {
        hideRevealed();
        rebuild();
        refreshHint();
        maybeInspect();
        return;
    }
    refreshList();
    updateButtons();
}

void DestinationsPageImpl::refreshList() {
    populating_ = true;
    SendMessageW(list_, LB_RESETCONTENT, 0, 0);
    for (const auto& entry : draft_) {
        const auto& profile = entry.objectRef();
        const auto id = profile.value("id").toString();
        std::wstring text = wide(profile.value("label").toString(id));
        const bool proton = profile.value("provider") == protonProvider;
        if (profile.value("provider") != "s3" && !proton) text += L" (not available on Windows)";
        else if (provisional_.count(id)) text += L" (not saved)";
        else if (isDefault(id)) text += L" (default)";
        if (proton) {
            // Row state as on Linux: Signed out / Choose a folder / Connected.
            const auto& settings = profile.value("settings");
            text += settings["accountRootUid"].toString().empty() ? L" (signed out)" : settings["folderUid"].toString().empty() ? L" (choose a folder)" : L" (connected)";
        }
        if (errors_.count(id) && !errors_[id].empty()) text += L" \x2014 needs attention";
        SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    if (selected_ >= 0) SendMessageW(list_, LB_SETCURSEL, WPARAM(selected_), 0);
    populating_ = false;
    updateButtons();
}

void DestinationsPageImpl::select(int index) {
    if (index == selected_) return;
    hideRevealed();
    clearSecrets();
    newFolderOpen_ = false;
    selected_ = index;
    populate();
    refreshHint();
    updateButtons();
    maybeInspect();
    main_.viewStateChanged();
}

void DestinationsPageImpl::populate() {
    populating_ = true;
    auto* profile = current();
    if (profile) {
        const auto id = profile->value("id").toString();
        const auto& settings = profile->value("settings").objectRef();
        setText(title_, profile->value("label").toString(id));
        const bool proton = profile->value("provider") == protonProvider;
        if (proton)
            setTextW(provider_, std::wstring(L"Third-party Proton Drive integration. Share links open a Proton page.") +
                                    (protonKnownUnavailable() ? L" The Proton Drive helper is not installed, so this destination cannot connect or upload." : L""));
        else
            setTextW(provider_, profile->value("provider") == "s3" ? L"Amazon S3 or S3-compatible service"
                                                                   : wide(profile->value("provider").toString()) + L" \x2014 not available on Windows");
        setTooltip(main_.tooltip(), test_, proton ? L"Checks the signed-in Proton account and selected folder without uploading. Upload and sharing are not tested."
                                                  : L"Uploads a small synthetic file, checks its public link and removes it again.");
        if (proton) populateProton(*profile);
        syncText(fields_["label"], profile->value("label").toString());
        for (const auto& key : textFields) {
            if (key == "label") continue;
            const auto& value = settings.value(key);
            syncText(fields_[key], value.isNumber() ? std::to_string(value.toInteger()) : value.toString());
        }
        ui::select(storageClass_, settings.value("storageClass").toString("STANDARD"));
        ui::select(acl_, settings.value("aclMode").toString("public-read"));
        ui::select(addressing_, settings.value("addressing").toString("auto"));
        const bool session = profile->value("credentials")["mode"] == "session";
        setChecked(keychain_, !session);
        setChecked(session_, session);
        setTextW(testResult_, testResults_[id]);
        setTextW(general_, generalErrors_[id]);
        showErrors();
    }
    populating_ = false;
    updateButtons();
    form_.layout();
}

void DestinationsPageImpl::showErrors() {
    auto* profile = current();
    const auto id = profile ? profile->value("id").toString() : std::string();
    for (auto& [key, label] : errorLabels_) {
        const auto& fieldErrors = errors_[id];
        const auto it = fieldErrors.find(key);
        setTextW(label, it == fieldErrors.end() ? std::wstring() : it->second);
    }
}

std::map<std::string, std::wstring> DestinationsPageImpl::validate(const json::Object& profile) const {
    std::map<std::string, std::wstring> errors;
    const auto label = text::trim(profile.value("label").toString());
    if (label.empty() || text::codePoints(label) > 128) errors["label"] = L"Enter a name of up to 128 characters.";
    if (profile.value("provider") == protonProvider) {
        const auto& settings = profile.value("settings").objectRef();
        if (!json::integer(settings.value("linkExpirationDays"), 0, 365))
            errors["linkExpirationDays"] = L"Enter a whole number of days from 0 to 365. 0 means no expiration.";
        if (!json::integer(settings.value("requestTimeoutMs"), 1000, 86400000))
            errors["protonRequestTimeoutMs"] = L"Enter a deadline from 1 to 86400 seconds, with up to three decimals.";
        if (!settings.value("folderUid").toString().empty() && settings.value("accountRootUid").toString().empty())
            errors["protonFolder"] = L"Connect the account before choosing a folder.";
        return errors;
    }
    if (profile.value("provider") != "s3") return errors;
    const auto& settings = profile.value("settings").objectRef();
    const auto bucket = settings.value("bucket").toString();
    if (bucket.size() < 3 || bucket.size() > 63 || text::containsChar(bucket, '/') || text::containsChar(bucket, ':') || text::containsChar(bucket, ' '))
        errors["bucket"] = bucket.empty() ? L"Enter a bucket name to finish setting up this destination." : L"Bucket names are 3 to 63 characters without spaces, slashes or colons.";
    if (settings.value("region").toString().empty()) errors["region"] = L"Enter a region, such as us-east-1.";
    if (auto error = urlError(settings.value("endpoint").toString(), false); !error.empty()) errors["endpoint"] = error;
    if (settings.value("objectPrefix").toString().size() > 1022) errors["objectPrefix"] = L"The folder is limited to 1022 bytes.";
    if (settings.value("publicUrlBase").isString())
        if (auto error = urlError(settings.value("publicUrlBase").toString(), true); !error.empty()) errors["publicUrlBase"] = error;
    for (const auto& key : numberFields)
        if (!json::integer(settings.value(key), 1, 2147483647.0)) errors[key] = L"Enter a whole number from 1 to 2147483647.";
    if (settings.value("addressing") == "virtual" && text::containsChar(bucket, '.'))
        errors["addressing"] = L"Bucket in hostname does not work with dotted bucket names. Use Automatic or Bucket in URL path.";
    if (errors.empty() && s3::validateSettings(settings)) errors["endpoint"] = L"These settings are not valid for S3. Check the endpoint, addressing and link domain.";
    return errors;
}

void DestinationsPageImpl::readForm() {
    auto* profile = current();
    if (!profile) return;
    (*profile)["label"] = text::trim(text(fields_["label"]));
    if (profile->value("provider") == protonProvider) {
        auto settings = profile->value("settings").toObject();
        if (SendMessageW(protonFolder_, CB_GETCURSEL, 0, 0) != CB_ERR) {
            const auto uid = selected(protonFolder_);
            settings["folderUid"] = uid;
            const auto name = folderName(uid);
            if (uid.empty() || name.empty()) settings.remove("folderName");
            else settings["folderName"] = name;
        }
        settings["shareByDefault"] = !checked(protonPrivate_);
        const auto days = text::trim(text(protonExpiration_));
        bool digits = !days.empty() && days.size() <= 3;
        for (char c : days) digits = digits && c >= '0' && c <= '9';
        settings["linkExpirationDays"] = digits ? json::Value(std::stoll(days)) : json::Value(days);
        settings["requestTimeoutMs"] = parseSeconds(text::trim(text(protonTimeout_)));
        (*profile)["settings"] = settings;
        return;
    }
    if (profile->value("provider") != "s3") return;
    auto settings = profile->value("settings").toObject();
    for (const auto& key : {"bucket", "region", "endpoint", "objectPrefix"}) settings[key] = text::trim(text(fields_[key]));
    const auto base = text::trim(text(fields_["publicUrlBase"]));
    settings["publicUrlBase"] = base.empty() ? json::Value() : json::Value(base);
    for (const auto& key : numberFields) {
        const auto raw = text::trim(text(fields_[key]));
        bool digits = !raw.empty() && raw.size() <= 10;
        for (char c : raw) digits = digits && c >= '0' && c <= '9';
        settings[key] = digits ? json::Value(std::stoll(raw)) : json::Value(raw);
    }
    settings["storageClass"] = selected(storageClass_);
    settings["aclMode"] = selected(acl_);
    settings["addressing"] = selected(addressing_);
    (*profile)["settings"] = settings;
}

json::Array DestinationsPageImpl::savableProfiles() const {
    json::Array out;
    for (const auto& entry : draft_) {
        const auto& profile = entry.objectRef();
        const auto id = profile.value("id").toString();
        if (validate(profile).empty()) out.push_back(entry);
        else if (auto stored = documentProfile(id); !stored.empty()) out.push_back(stored);
    }
    return out;
}

void DestinationsPageImpl::fieldChanged(int id) {
    auto* profile = current();
    if (!profile || populating_) return;
    if (id == idRegion) {
        // Keep the AWS endpoint in step with the region unless it was customised.
        const auto previous = profile->value("settings")["region"].toString();
        const auto endpoint = text::trim(text(fields_["endpoint"]));
        if (endpoint.empty() || endpoint == derivedEndpoint(previous)) {
            populating_ = true;
            setText(fields_["endpoint"], derivedEndpoint(text::trim(text(fields_["region"]))));
            populating_ = false;
        }
    }
    readForm();
    const auto profileId = profile->value("id").toString();
    errors_[profileId] = validate(*profile);
    generalErrors_.erase(profileId);
    setTextW(general_, L"");
    showErrors();
    if (id == idLabel) setText(title_, profile->value("label").toString(profileId));
    if (errors_[profileId].empty()) {
        main_.queuePatch({{"profiles", savableProfiles()}});
    }
    refreshList();
    form_.layout();
}

bool DestinationsPageImpl::validationFailed(const std::string& path, const std::wstring& message) {
    if (!text::startsWith(path, "/profiles")) return false;
    auto* profile = current();
    if (!profile) return false;
    generalErrors_[profile->value("id").toString()] = message;
    setTextW(general_, message);
    form_.layout();
    return true;
}

void DestinationsPageImpl::updateButtons() {
    auto* profile = current();
    const bool any = profile != nullptr;
    const auto id = any ? profile->value("id").toString() : std::string();
    const bool stored = any && saved(id);
    const bool s3 = any && profile->value("provider") == "s3";
    const bool proton = any && profile->value("provider") == protonProvider;
    enable(remove_, any);
    enable(up_, any && selected_ > 0);
    enable(down_, any && size_t(selected_ + 1) < draft_.size());
    enable(default_, stored && (s3 || proton) && !isDefault(id));
    enable(store_, stored && s3);
    enable(unlock_, stored && s3 && profile->value("credentials")["mode"] == "keychain");
    bool testRunning = false;
    for (const auto& job : jobs_) testRunning = testRunning || (job.profileId == id && job.kind == "testDestination");
    enable(test_, stored && (s3 || proton) && !testRunning);
    setTextW(test_, testRunning ? L"Testing\x2026" : L"Test connection");
    enable(show_, s3 && (stored || GetWindowTextLengthW(secret_)));
    if (proton) {
        const auto& settings = profile->value("settings");
        const bool connected = !settings["accountRootUid"].toString().empty();
        const bool busy = jobRunning(id, {"connectAccount", "inspectAccount", "createFolder", "storeSharePassword"});
        const bool usable = stored && !busy && !protonKnownUnavailable();
        enable(protonConnect_, usable);
        enable(protonRefresh_, usable && connected);
        enable(protonFolder_, connected && !busy);
        enable(protonNewFolder_, usable && connected);
        enable(protonCreateFolder_, usable && connected && GetWindowTextLengthW(protonFolderName_) > 0);
        enable(protonStorePassword_, usable && GetWindowTextLengthW(protonPassword_) > 0);
        enable(protonClearPassword_, stored && !busy && !settings["sharePasswordReference"].toString().empty());
    }
}

void DestinationsPageImpl::addS3() {
    int n = 1;
    auto used = [this](const std::string& id) {
        for (const auto& entry : draft_) if (entry.objectRef().value("id") == id.c_str()) return true;
        return false;
    };
    while (used("s3-" + std::to_string(n))) ++n;
    const auto id = "s3-" + std::to_string(n);
    json::Object profile{{"id", id}, {"label", n == 1 ? std::string("S3 destination") : "S3 destination " + std::to_string(n)}, {"provider", "s3"},
                         {"settings", json::Object{{"bucket", ""}, {"region", "us-east-1"}, {"endpoint", derivedEndpoint("us-east-1")}, {"objectPrefix", ""},
                                                   {"storageClass", "STANDARD"}, {"aclMode", "public-read"}, {"publicUrlBase", json::Value()}, {"addressing", "auto"},
                                                   {"connectTimeoutMs", 5000}, {"requestTimeoutMs", 60000}, {"lowSpeedBytesPerSecond", 1}, {"lowSpeedTimeSeconds", 30}}},
                         {"credentials", json::Object{{"mode", "keychain"}, {"reference", "putgd/" + id}}}};
    draft_.push_back(profile);
    provisional_.insert(id);
    errors_[id] = validate(profile);
    hideRevealed();
    clearSecrets();
    selected_ = int(draft_.size()) - 1;
    refreshList();
    populate();
    refreshHint();
    SetFocus(fields_["bucket"]);
    main_.status(L"Enter the bucket and region. The destination saves once its settings are valid.");
}

void DestinationsPageImpl::remove() {
    auto* profile = current();
    if (!profile) return;
    const auto id = profile->value("id").toString();
    const auto name = wide(profile->value("label").toString(id));
    if (!confirm(main_.hwnd(), L"Remove destination", L"Remove " + name + L"?",
                 L"Captures that use it lose their destination; review Settings > Capture afterwards. Existing uploads, local files and remote objects stay.", L"Remove"))
        return;
    hideRevealed();
    clearSecrets();
    const bool wasSaved = saved(id);
    draft_.erase(draft_.begin() + selected_);
    provisional_.erase(id);
    errors_.erase(id);
    testResults_.erase(id);
    accounts_.erase(id);
    accountStatus_.erase(id);
    newFolderOpen_ = false;
    if (wasSaved) {
        json::Object patch{{"profiles", savableProfiles()}};
        const auto& document = main_.session().document();
        json::Object actions;
        for (const auto& [name, value] : document.value("actions").objectRef())
            if (value.objectRef().value("profileId") == id.c_str()) actions[name] = json::Object{{"profileId", json::Value()}};
        if (!actions.empty()) patch["actions"] = actions;
        if (document.value("captureDefaults")["profileId"] == id.c_str()) patch["captureDefaults"] = json::Object{{"profileId", json::Value()}};
        main_.queuePatch(patch);
    }
    selected_ = draft_.empty() ? -1 : std::min<int>(selected_, int(draft_.size()) - 1);
    refreshList();
    populate();
    refreshHint();
}

bool DestinationsPageImpl::viewCommand(const std::string& name, const json::Object& params, const CommandReply& answer) {
    if (name != "moveDestination") return false;
    if (draft_.empty() && provisional_.empty()) rebuild();
    const auto profileId = params.value("profileId").toString();
    if (!profileId.empty()) {
        int index = -1;
        for (size_t i = 0; i < draft_.size(); ++i) if (draft_[i].objectRef().value("id") == profileId.c_str()) index = int(i);
        if (index < 0) { answer({"profile_not_found", "No destination has that ID.", {{"profileId", profileId}}}, {}); return true; }
        if (index != selected_) { select(index); SendMessageW(list_, LB_SETCURSEL, WPARAM(index), 0); }
    }
    if (selected_ < 0) { answer({"destination_not_selected", "Select a destination first.", {}}, {}); return true; }
    const int delta = params.value("direction") == "up" ? -1 : 1;
    const int target = selected_ + delta;
    if (target < 0 || size_t(target) >= draft_.size())
        { answer({"destination_move_invalid", delta < 0 ? "That destination is already at the top." : "That destination is already at the bottom.", {}}, {}); return true; }
    move(delta);
    std::vector<std::string> order;
    for (const auto& entry : draft_) order.push_back(entry.objectRef().value("id").toString());
    answer({}, {{"order", json::stringArray(order)}, {"selected", draft_[size_t(selected_)].objectRef().value("id").toString()}, {"saving", reordering_ || main_.settingsBusy()}});
    return true;
}

void DestinationsPageImpl::move(int delta) {
    const int target = selected_ + delta;
    if (selected_ < 0 || target < 0 || size_t(target) >= draft_.size()) return;
    std::swap(draft_[size_t(selected_)], draft_[size_t(target)]);
    selected_ = target;
    refreshList();
    std::vector<std::string> order;
    for (const auto& entry : draft_) {
        const auto id = entry.objectRef().value("id").toString();
        if (saved(id)) order.push_back(id);
    }
    std::vector<std::string> currentOrder;
    for (const auto& entry : main_.session().document().value("profiles").arrayRef()) currentOrder.push_back(entry.objectRef().value("id").toString());
    if (order == currentOrder) return;
    reordering_ = true;
    main_.flushSettings([this, order](bool saved) {
        if (!saved) { reordering_ = false; rebuild(); return; }
        main_.session().call("settings.reorderProfiles", {{"order", json::stringArray(order)}, {"expectedRevision", main_.session().revision()}}, [this](const Reply& reply) {
            reordering_ = false;
            if (!reply.ok) {
                // The previous order comes back; a conflict asks for Reload.
                rebuild();
                main_.handleSettingsError(reply);
                return;
            }
            main_.session().call("settings.get", {}, [this](const Reply& current) {
                if (current.ok) main_.adoptSettings(current.result.value("document").toObject(), current.result.value("metadata").toObject());
            });
        });
    });
}

void DestinationsPageImpl::makeDefault() {
    auto* profile = current();
    if (!profile) return;
    const auto id = profile->value("id").toString();
    const auto& document = main_.session().document();
    json::Object patch{{"captureDefaults", json::Object{{"destination", "upload"}, {"profileId", id},
                                                        {"publicationMode", document.value("captureDefaults")["publicationMode"].toString("default")}}}};
    if (!document.contains("captureDefaults")) {
        // Adopt the shared default: actions that all routed alike now follow it.
        const auto first = Settings::effectiveAction(document, "region");
        bool same = true;
        for (const auto& name : Settings::actionNames()) {
            const auto action = Settings::effectiveAction(document, name);
            same = same && action.value("destination") == first.value("destination") && action.value("profileId") == first.value("profileId") &&
                   action.value("publicationMode").toString("default") == first.value("publicationMode").toString("default");
        }
        json::Object actions;
        for (const auto& name : Settings::actionNames()) {
            if (same || Settings::effectiveAction(document, name).value("profileId").isNull())
                actions[name] = json::Object{{"destination", "default"}, {"profileId", json::Value()}, {"publicationMode", "default"}};
        }
        patch["actions"] = actions;
    }
    main_.queuePatch(patch);
    main_.status(L"Default destination changed. Per-action exceptions keep their own destination.");
}

void DestinationsPageImpl::clearSecrets() {
    if (!access_) return;
    ++revealToken_;
    revealed_ = typedVisible_ = false;
    for (HWND h : {access_, secret_, token_, protonPassword_}) wipe(h);
    SendMessageW(secret_, EM_SETREADONLY, FALSE, 0);
    SendMessageW(secret_, EM_SETPASSWORDCHAR, 0x25CF, 0);
    setTextW(show_, L"Show");
    InvalidateRect(secret_, nullptr, TRUE);
}

void DestinationsPageImpl::hideRevealed() {
    if (!revealed_) return;
    ++revealToken_;
    revealed_ = false;
    wipe(secret_);
    SendMessageW(secret_, EM_SETREADONLY, FALSE, 0);
    SendMessageW(secret_, EM_SETPASSWORDCHAR, 0x25CF, 0);
    setTextW(show_, L"Show");
    InvalidateRect(secret_, nullptr, TRUE);
}

void DestinationsPageImpl::toggleSecret() {
    if (revealed_) { hideRevealed(); return; }
    if (GetWindowTextLengthW(secret_)) {
        typedVisible_ = !typedVisible_;
        SendMessageW(secret_, EM_SETPASSWORDCHAR, typedVisible_ ? 0 : 0x25CF, 0);
        setTextW(show_, typedVisible_ ? L"Hide" : L"Show");
        InvalidateRect(secret_, nullptr, TRUE);
        return;
    }
    auto* profile = current();
    if (!profile || !saved(profile->value("id").toString())) {
        main_.status(L"Save valid destination settings before showing credentials.", true);
        return;
    }
    const auto id = profile->value("id").toString();
    const auto token = ++revealToken_;
    main_.session().call("setup.revealCredentialSecret", {{"profileId", id}, {"expectedRevision", main_.session().revision()}}, [this, id, token](const Reply& reply) {
        auto* now = current();
        if (token != revealToken_ || !now || now->value("id").toString() != id || !IsWindowVisible(panel_.hwnd())) {
            return;
        }
        if (!reply.ok) {
            main_.status(reply.error.code == "credential_setup_required" || reply.error.code == "credentials_not_loaded"
                             ? L"Saved credentials are not loaded. Choose Unlock saved credentials first."
                             : UiSession::describe(reply), true);
            return;
        }
        std::string secret = reply.result.value("secretKey").toString();
        SendMessageW(secret_, EM_SETPASSWORDCHAR, 0, 0);
        setText(secret_, secret);
        SecureZeroMemory(secret.data(), secret.size());
        SendMessageW(secret_, EM_SETREADONLY, TRUE, 0);
        revealed_ = true;
        setTextW(show_, L"Hide");
        InvalidateRect(secret_, nullptr, TRUE);
    });
}

void DestinationsPageImpl::refreshHint() {
    auto* profile = current();
    setTextW(hint_, L"");
    setCue(access_, L"");
    setCue(secret_, L"");
    setCue(token_, L"Optional, for temporary AWS credentials.");
    secretStored_ = false;
    if (!profile || profile->value("provider") != "s3") { form_.layout(); return; }
    const auto id = profile->value("id").toString();
    if (!saved(id)) {
        setTextW(hint_, L"Credentials can be stored once the destination is saved.");
        form_.layout();
        return;
    }
    const auto token = ++hintToken_;
    main_.session().call("setup.credentialHint", {{"profileId", id}, {"expectedRevision", main_.session().revision()}}, [this, id, token](const Reply& reply) {
        auto* now = current();
        if (token != hintToken_ || !now || now->value("id").toString() != id) return;
        if (!reply.ok) { setTextW(hint_, L""); form_.layout(); return; }
        // Field names: Linux accessKeyHint/tokenStored; the Windows resident accessKeyPrefix/hasSecret/hasSessionToken/stored.
        const auto& hint = reply.result;
        const bool keychain = hint.value("mode") == "keychain";
        const auto key = wide(hint.value("accessKeyHint").toString(hint.value("accessKeyPrefix").toString()));
        const bool tokenStored = hint.value("tokenStored").toBool(hint.value("hasSessionToken").toBool());
        const bool secret = hint.value("hasSecret").toBool(true);
        if (hint.value("available").toBool()) {
            const auto location = keychain ? std::wstring(L"Saved in Windows Credential Manager") : std::wstring(L"Available for this session");
            setTextW(hint_, key + L" \x00B7 " + location + (secret ? L" \x00B7 Secret key stored" : L"") + (tokenStored ? L" \x00B7 Session token stored" : L""));
            setCue(access_, key + L" \x00B7 " + location);
            if (secret) setCue(secret_, L"\x2022\x2022\x2022\x2022\x2022\x2022\x2022\x2022 \x00B7 Stored");
            if (tokenStored) setCue(token_, L"\x2022\x2022\x2022\x2022\x2022\x2022\x2022\x2022 \x00B7 Stored");
            secretStored_ = secret;
        } else if (keychain) {
            setTextW(hint_, hint.value("stored").toBool(true) ? L"Saved credentials are not loaded. Choose Unlock saved credentials to use them."
                                                              : L"No saved credentials yet. Enter them and choose Store credentials.");
        } else {
            setTextW(hint_, L"No credentials loaded for this session.");
        }
        updateButtons();
        form_.layout();
    });
}

void DestinationsPageImpl::startJob(const std::string& kind, const Reply& reply, const std::string& profileId) {
    if (!reply.ok) {
        main_.status(setupError(reply), true);
        if (kind == "testDestination") { testResults_[profileId] = L"Connection test did not start. " + setupError(reply); populate(); }
        else if (kind == "connectAccount" || kind == "inspectAccount") {
            if (kind == "connectAccount") accountStatus_[profileId] = L"Connection failed: " + setupError(reply);
            else accountStatus_.erase(profileId);
            auto* now = current();
            if (now && now->value("id").toString() == profileId) populate();
        }
        return;
    }
    const auto setupId = reply.result.value("setupId").toString();
    if (setupId.empty()) return;
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [&](const Job& j) { return j.profileId == profileId && j.kind == kind; }), jobs_.end());
    jobs_.push_back({setupId, kind, profileId});
    SetTimer(panel_.hwnd(), timerSetup, 500, nullptr);
    updateButtons();
}

void DestinationsPageImpl::store() {
    auto* profile = current();
    if (!profile) return;
    const auto id = profile->value("id").toString();
    if (!saved(id)) { main_.status(L"Save valid destination settings before storing credentials.", true); return; }
    if (revealed_ || !GetWindowTextLengthW(access_) || !GetWindowTextLengthW(secret_)) {
        main_.status(L"Enter the access key ID and secret access key, then choose Store credentials.", true);
        return;
    }
    main_.flushSettings([this, id](bool savedSettings) {
        if (!savedSettings) { main_.status(L"Settings were not saved. Retry saving before storing credentials.", true); return; }
        auto* now = current();
        if (!now || now->value("id").toString() != id) return;
        json::Object credentials{{"accessKey", text::trim(text(access_))}, {"secretKey", text(secret_)}};
        if (GetWindowTextLengthW(token_)) credentials["sessionToken"] = text::trim(text(token_));
        main_.status(L"Storing credentials\x2026");
        main_.session().call("setup.storeCredentials", {{"profileId", id}, {"expectedRevision", main_.session().revision()}, {"credentials", credentials}},
                             [this, id](const Reply& reply) {
                                 if (reply.ok) {
                                     auto* now = current();
                                     if (now && now->value("id").toString() == id) clearSecrets();
                                 }
                                 startJob("storeCredentials", reply, id);
                             });
    });
}

void DestinationsPageImpl::unlock() {
    auto* profile = current();
    if (!profile) return;
    const auto id = profile->value("id").toString();
    main_.status(L"Unlocking saved credentials\x2026");
    main_.session().call("setup.unlockCredentials", {{"profileId", id}, {"expectedRevision", main_.session().revision()}},
                         [this, id](const Reply& reply) { startJob("unlockCredentials", reply, id); });
}

void DestinationsPageImpl::test() {
    auto* profile = current();
    if (!profile) return;
    const auto id = profile->value("id").toString();
    main_.flushSettings([this, id](bool savedSettings) {
        if (!savedSettings) { main_.status(L"Settings were not saved. Retry saving before testing.", true); return; }
        const auto stored = documentProfile(id);
        testResults_[id] = stored.value("provider") == protonProvider ? L"Testing connection: checking the Proton account and selected folder\x2026"
                                                                      : L"Testing the connection\x2026";
        populate();
        main_.session().call("setup.testDestination", {{"profileId", id}, {"expectedRevision", main_.session().revision()}},
                             [this, id](const Reply& reply) { startJob("testDestination", reply, id); });
    });
}

void DestinationsPageImpl::poll() {
    if (jobs_.empty()) { KillTimer(panel_.hwnd(), timerSetup); return; }
    if (polling_) return;
    polling_ = true;
    const auto job = jobs_.front();
    main_.session().call("setup.inspect", {{"id", job.setupId}}, [this, job](const Reply& reply) {
        polling_ = false;
        auto it = std::find_if(jobs_.begin(), jobs_.end(), [&](const Job& j) { return j.setupId == job.setupId; });
        if (it == jobs_.end()) return;
        if (!reply.ok && reply.outcomeUnknown) return; // keep the accepted ID and ask again
        const auto state = reply.result.value("state").toString();
        if (reply.ok && (state == "running" || state == "accepted" || state == "pending" || state == "queued")) {
            // Proton sign-in: open the browser once per job, and only for Proton's account site.
            const auto event = reply.result.value("event").toObject();
            if (event.value("event") == "auth" && !authOpened_.count(job.setupId)) {
                const auto url = event.value("url").toString();
                if (text::startsWith(url, "https://account.proton.me/")) {
                    authOpened_.insert(job.setupId);
                    openUrl(url);
                    accountStatus_[job.profileId] = L"Finish signing in to Proton in your browser.";
                    auto* now = current();
                    if (now && now->value("id").toString() == job.profileId) { setTextW(protonAccount_, accountStatus_[job.profileId]); form_.layout(); }
                }
            }
            // Move it to the back so several jobs share the poll.
            auto copy = *it;
            jobs_.erase(it);
            jobs_.push_back(copy);
            return;
        }
        jobs_.erase(it);
        authOpened_.erase(job.setupId);
        if (jobs_.empty()) KillTimer(panel_.hwnd(), timerSetup);
        Error error = reply.ok ? Error::fromJson(reply.result.value("error").toObject()) : reply.error;
        if (!reply.ok && reply.error.code == "setup_not_found") error.message = "This setup result is no longer available; the action was not repeated.";
        const bool success = reply.ok && error.code.empty() && state != "failed";
        const auto result = reply.result.value("result").toObject();
        Reply failure;
        failure.error = error;
        if (job.kind == "testDestination") {
            const bool accountScope = result.value("testScope") == "account-folder";
            std::wstring text = success ? (accountScope ? L"Connection successful. Account and selected folder are accessible. Upload and sharing were not tested."
                                                        : L"Connection successful. Test upload, public link and cleanup passed.")
                                        : L"Connection test failed. " + setupError(failure);
            if (!result.empty() && !accountScope)
                text += L"\nUpload: " + stepText(result.value("upload")) + L"\nPublic link: " + stepText(result.value("publicGet")) + L"\nCleanup: " + stepText(result.value("cleanup"));
            if (!success && error.code == "credential_setup_required") text += L"\nStore or unlock credentials, then test again.";
            testResults_[job.profileId] = text;
            main_.status(success ? L"Connection test passed." : L"Connection test failed.", !success);
        } else if (job.kind == "connectAccount" || job.kind == "inspectAccount") {
            if (success && !result.value("accountRootUid").toString().empty()) {
                accountStatus_.erase(job.profileId);
                applyAccount(job.profileId, result);
                main_.status(L"Account and folders loaded.");
            } else {
                const auto message = success ? std::wstring(L"Proton did not report an account.") : setupError(failure);
                if (job.kind == "connectAccount") accountStatus_[job.profileId] = L"Connection failed: " + message;
                else accountStatus_.erase(job.profileId);
                main_.status((job.kind == "connectAccount" ? L"The Proton account was not connected. " : L"Folders were not loaded. ") + message, true);
            }
        } else if (job.kind == "createFolder") {
            if (success) { applyFolder(job.profileId, result); main_.status(L"Folder created and selected for future uploads."); }
            else main_.status(L"The folder was not created. " + setupError(failure), true);
        } else if (job.kind == "storeSharePassword") {
            if (success) { applyLinkPassword(job.profileId, result); main_.status(L"Link password stored. New share links use it."); }
            else main_.status(L"The link password was not stored. " + setupError(failure), true);
        } else if (job.kind == "storeCredentials") {
            main_.status(success ? L"Credentials stored." : L"Credentials were not stored. " + UiSession::describe(failure), !success);
        } else if (job.kind == "unlockCredentials") {
            main_.status(success ? L"Saved credentials unlocked." : L"Saved credentials were not unlocked. " + UiSession::describe(failure), !success);
        }
        auto* now = current();
        if (now && now->value("id").toString() == job.profileId) {
            setTextW(testResult_, testResults_[job.profileId]);
            if (now->value("provider") == protonProvider) populate();
            refreshHint();
        }
        updateButtons();
        form_.layout();
    });
}

// ---- Proton Drive ------------------------------------------------------------

void DestinationsPageImpl::loadCapabilities() {
    if (capabilitiesRequested_) return;
    capabilitiesRequested_ = true;
    main_.session().call("capabilities", {}, [this](const Reply& reply) {
        capabilitiesRequested_ = false;
        if (!reply.ok) return; // the Add menu keeps Proton off; the next activation asks again
        capabilitiesLoaded_ = true;
        providers_ = reply.result.value("providers").toObject();
        auto* profile = current();
        if (profile && profile->value("provider") == protonProvider) populate();
        updateButtons();
        maybeInspect();
    });
}

bool DestinationsPageImpl::protonAvailable() const {
    return capabilitiesLoaded_ && providers_.value(protonProvider)["available"].toBool();
}

// Only a loaded capabilities answer turns the setup buttons off; the resident refuses anyway.
bool DestinationsPageImpl::protonKnownUnavailable() const { return capabilitiesLoaded_ && !protonAvailable(); }

bool DestinationsPageImpl::jobRunning(const std::string& profileId, std::initializer_list<const char*> kinds) const {
    for (const auto* kind : kinds) {
        if (starting_.count({profileId, kind})) return true;
        for (const auto& job : jobs_)
            if (job.profileId == profileId && job.kind == kind) return true;
    }
    return false;
}

std::string DestinationsPageImpl::folderName(const std::string& uid) const {
    for (const auto& [folderUid, name] : shownFolders_) if (folderUid == uid) return name;
    return {};
}

void DestinationsPageImpl::addProton() {
    int n = 1;
    auto used = [this](const std::string& id) {
        for (const auto& entry : draft_) if (entry.objectRef().value("id") == id.c_str()) return true;
        return false;
    };
    while (used("proton-" + std::to_string(n))) ++n;
    const auto id = "proton-" + std::to_string(n);
    json::Object profile{{"id", id}, {"label", n == 1 ? std::string("Proton Drive") : "Proton Drive " + std::to_string(n)}, {"provider", protonProvider},
                         {"settings", json::Object{{"accountRootUid", ""}, {"folderUid", ""}, {"shareByDefault", true}, {"linkExpirationDays", 0},
                                                   {"sharePasswordReference", json::Value()}, {"requestTimeoutMs", 120000}}},
                         {"credentials", json::Object{{"mode", "keychain"}, {"reference", "putgd/proton/" + randomHex32()}}}};
    draft_.push_back(profile);
    provisional_.insert(id);
    errors_[id] = validate(profile);
    hideRevealed();
    clearSecrets();
    newFolderOpen_ = false;
    selected_ = int(draft_.size()) - 1;
    refreshList();
    populate();
    refreshHint();
    // The defaults are valid, so the destination saves at once; Connect account needs it saved.
    if (errors_[id].empty()) main_.queuePatch({{"profiles", savableProfiles()}});
    SetFocus(protonConnect_);
    main_.status(L"Choose Connect account and finish signing in to Proton in your browser.");
}

void DestinationsPageImpl::populateProton(const json::Object& profile) {
    const auto id = profile.value("id").toString();
    const auto& settings = profile.value("settings");
    const auto root = settings["accountRootUid"].toString();
    const auto folderUid = settings["folderUid"].toString();
    const auto account = accounts_.find(id);
    const bool sameAccount = !root.empty() && account != accounts_.end() && account->second.root == root;
    std::wstring status;
    if (auto it = accountStatus_.find(id); it != accountStatus_.end()) status = it->second;
    else if (root.empty()) status = L"Not connected";
    else if (sameAccount && !account->second.email.empty()) status = wide(account->second.email) + L" \x00B7 Connected";
    else status = L"Connected account";
    setTextW(protonAccount_, status);
    setTextW(protonConnect_, root.empty() ? L"Connect account\x2026" : L"Reconnect account\x2026");

    // Upload folder: My files (the account root), then the folders the account reported;
    // a saved folder that has not been listed yet keeps its saved name.
    shownFolders_.clear();
    if (!root.empty()) {
        shownFolders_.push_back({root, "My files"});
        if (sameAccount)
            for (const auto& folder : account->second.folders)
                if (folder.first != root) shownFolders_.push_back(folder);
    }
    const bool listed = std::any_of(shownFolders_.begin(), shownFolders_.end(), [&](const auto& f) { return f.first == folderUid; });
    if (!folderUid.empty() && !listed) shownFolders_.push_back({folderUid, settings["folderName"].toString()});
    std::vector<Choice> choices;
    for (const auto& [uid, name] : shownFolders_) choices.push_back({name.empty() ? std::wstring(L"Saved Proton folder") : wide(name), uid});
    setChoices(protonFolder_, choices);
    if (folderUid.empty() || !ui::select(protonFolder_, folderUid)) SendMessageW(protonFolder_, CB_SETCURSEL, WPARAM(-1), 0);
    const bool loading = jobRunning(id, {"connectAccount", "inspectAccount"});
    setTextW(protonFolderStatus_, loading ? L"Loading folders\x2026"
                                  : root.empty() ? L"Connect an account to choose a folder."
                                  : folderUid.empty() ? L"Choose where uploads go." : L"");

    const bool share = settings["shareByDefault"].toBool(true);
    setChecked(protonShare_, share);
    setChecked(protonPrivate_, !share);
    const auto& days = settings["linkExpirationDays"];
    syncText(protonExpiration_, days.isNumber() ? std::to_string(days.toInteger()) : days.toString());
    syncText(protonTimeout_, secondsText(settings["requestTimeoutMs"]));
    setTextW(protonPasswordState_, settings["sharePasswordReference"].toString().empty() ? L"New share links have no password."
                                                                                       : L"A link password is stored. New share links use it; existing links are unchanged.");
}

// Setup jobs need the destination saved first; edits are flushed before the call.
void DestinationsPageImpl::beginSetup(const std::string& kind, json::Object input, bool withPassword) {
    auto* profile = current();
    if (!profile) return;
    const auto id = profile->value("id").toString();
    if (!saved(id)) { main_.status(L"Finish valid destination settings first. Changes save automatically.", true); return; }
    if (withPassword && !GetWindowTextLengthW(protonPassword_)) { main_.status(L"Enter the new link password, then choose Store link password.", true); return; }
    if (jobRunning(id, {kind.c_str()})) return;
    if (kind == "connectAccount") {
        for (const auto& job : jobs_) if (job.profileId == id && job.kind == kind) authOpened_.erase(job.setupId);
        accountStatus_[id] = L"Connecting to Proton\x2026";
    }
    // Counts as running (buttons off, "Loading folders…") until the resident answers.
    starting_.insert({id, kind});
    main_.flushSettings([this, id, kind, input, withPassword](bool savedSettings) mutable {
        if (!savedSettings) {
            starting_.erase({id, kind});
            accountStatus_.erase(id);
            main_.status(L"Settings were not saved. Retry saving before continuing.", true);
            populate();
            return;
        }
        auto* now = current();
        if (!now || now->value("id").toString() != id) { starting_.erase({id, kind}); accountStatus_.erase(id); updateButtons(); return; }
        json::Object params{{"profileId", id}, {"expectedRevision", main_.session().revision()}};
        if (withPassword) {
            std::string secret = text(protonPassword_);
            wipe(protonPassword_);
            input["password"] = secret;
            input["reference"] = "putgd/proton/link/" + randomHex32();
            SecureZeroMemory(secret.data(), secret.size());
        }
        if (!input.empty()) params["input"] = input;
        main_.session().call("setup." + kind, params, [this, kind, id](const Reply& reply) {
            starting_.erase({id, kind});
            startJob(kind, reply, id);
            auto* current = this->current();
            if (current && current->value("id").toString() == id) populate();
        });
        populate();
    });
    populate();
}

// A connected, saved Proton destination loads its folders once per account when shown.
void DestinationsPageImpl::maybeInspect() {
    auto* profile = current();
    if (!profile || profile->value("provider") != protonProvider || !IsWindowVisible(panel_.hwnd())) return;
    const auto id = profile->value("id").toString();
    const auto root = profile->value("settings")["accountRootUid"].toString();
    if (root.empty() || !saved(id) || !protonAvailable() || jobRunning(id, {"connectAccount", "inspectAccount", "createFolder", "storeSharePassword"})) return;
    const auto key = id + "/" + root;
    if (requestedFolders_.count(key)) return;
    requestedFolders_.insert(key);
    beginSetup("inspectAccount");
}

void DestinationsPageImpl::createFolder() {
    const auto name = text::trim(text(protonFolderName_));
    bool printable = !name.empty();
    for (unsigned char c : name) printable = printable && c >= 0x20 && c != 0x7F;
    if (!printable || text::containsChar(name, '/')) { main_.status(L"Enter a folder name without slashes or control characters.", true); return; }
    newFolderOpen_ = false;
    form_.layout();
    beginSetup("createFolder", {{"name", name}});
}

void DestinationsPageImpl::clearLinkPassword() {
    auto* profile = current();
    if (!profile) return;
    auto settings = profile->value("settings").toObject();
    settings["sharePasswordReference"] = json::Value();
    saveSettings(profile->value("id").toString(), settings);
    main_.status(L"New share links will have no password. Existing links are unchanged.");
}

void DestinationsPageImpl::saveSettings(const std::string& profileId, const json::Object& settings) {
    for (auto& entry : draft_) {
        auto& profile = entry.objectRef();
        if (profile.value("id") != profileId.c_str()) continue;
        profile["settings"] = settings;
        errors_[profileId] = validate(profile);
        if (errors_[profileId].empty()) main_.queuePatch({{"profiles", savableProfiles()}});
        break;
    }
    refreshList();
    auto* now = current();
    if (now && now->value("id").toString() == profileId) populate();
}

void DestinationsPageImpl::applyAccount(const std::string& profileId, const json::Object& result) {
    json::Object* profile = nullptr;
    for (auto& entry : draft_) if (entry.objectRef().value("id") == profileId.c_str()) profile = &entry.objectRef();
    if (!profile || profile->value("provider") != protonProvider) return;
    const auto root = result.value("accountRootUid").toString();
    auto email = result.value("accountEmail").toString();
    if (email.size() > 320 || std::any_of(email.begin(), email.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; })) email.clear();
    auto& account = accounts_[profileId];
    if (email.empty() && account.root == root) email = account.email;
    account.root = root;
    account.email = email;
    account.folders.clear();
    for (const auto& value : result.value("folders").arrayRef()) {
        const auto& folder = value.objectRef();
        const auto uid = folder.value("uid").toString();
        if (!uid.empty() && uid != root) account.folders.push_back({uid, folder.value("name").toString()});
    }
    requestedFolders_.insert(profileId + "/" + root);
    auto settings = profile->value("settings").toObject();
    if (settings.value("accountRootUid").toString() != root) { settings["folderUid"] = ""; settings.remove("folderName"); }
    settings["accountRootUid"] = root;
    const auto selectedUid = settings.value("folderUid").toString();
    if (selectedUid == root) settings["folderName"] = "My files";
    else if (!selectedUid.empty()) {
        // Keep the saved folder when the account still lists it (taking a new name); otherwise clear it.
        auto it = std::find_if(account.folders.begin(), account.folders.end(), [&](const auto& f) { return f.first == selectedUid; });
        if (it == account.folders.end()) { settings["folderUid"] = ""; settings.remove("folderName"); }
        else if (it->second.empty()) settings.remove("folderName");
        else settings["folderName"] = it->second;
    }
    if (settings != profile->value("settings").toObject()) saveSettings(profileId, settings);
    else {
        refreshList();
        auto* now = current();
        if (now && now->value("id").toString() == profileId) populate();
    }
}

void DestinationsPageImpl::applyFolder(const std::string& profileId, const json::Object& result) {
    json::Object* profile = nullptr;
    for (auto& entry : draft_) if (entry.objectRef().value("id") == profileId.c_str()) profile = &entry.objectRef();
    const auto uid = result.value("uid").toString();
    if (!profile || profile->value("provider") != protonProvider || uid.empty()) return;
    auto settings = profile->value("settings").toObject();
    const auto root = result.value("accountRootUid").toString(settings.value("accountRootUid").toString());
    auto& account = accounts_[profileId];
    if (account.root != root) account = {root, {}, {}};
    const auto name = result.value("name").toString();
    if (std::none_of(account.folders.begin(), account.folders.end(), [&](const auto& f) { return f.first == uid; })) account.folders.push_back({uid, name});
    if (!root.empty()) settings["accountRootUid"] = root;
    settings["folderUid"] = uid;
    if (name.empty()) settings.remove("folderName");
    else settings["folderName"] = name;
    saveSettings(profileId, settings);
}

void DestinationsPageImpl::applyLinkPassword(const std::string& profileId, const json::Object& result) {
    const auto reference = result.value("reference").toString();
    for (const auto& entry : draft_) {
        const auto& profile = entry.objectRef();
        if (profile.value("id") != profileId.c_str() || profile.value("provider") != protonProvider || reference.empty()) continue;
        auto settings = profile.value("settings").toObject();
        settings["sharePasswordReference"] = reference;
        saveSettings(profileId, settings);
        return;
    }
}

void DestinationsPageImpl::command(int id, int code, HWND control) {
    if (populating_) return;
    switch (id) {
    case idList:
        if (code == LBN_SELCHANGE) select(int(SendMessageW(list_, LB_GETCURSEL, 0, 0)));
        return;
    case idAdd: {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, idAddS3, L"Amazon S3 or S3-compatible service");
        // Proton needs the private helper; until capabilities confirm it, the entry stays off.
        if (protonAvailable()) AppendMenuW(menu, MF_STRING, idAddProton, L"Proton Drive");
        else AppendMenuW(menu, MF_STRING | MF_GRAYED, idAddProton, L"Proton Drive (helper not installed)");
        if (!capabilitiesLoaded_) loadCapabilities();
        RECT r{};
        GetWindowRect(add_, &r);
        const int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, main_.hwnd(), nullptr);
        DestroyMenu(menu);
        if (chosen == idAddS3) addS3();
        else if (chosen == idAddProton && protonAvailable()) addProton();
        return;
    }
    case idProtonConnect: beginSetup("connectAccount"); return;
    case idProtonRefresh: beginSetup("inspectAccount"); return;
    case idProtonNewFolder:
        newFolderOpen_ = true;
        setText(protonFolderName_, "put.gd");
        form_.layout();
        SetFocus(protonFolderName_);
        SendMessageW(protonFolderName_, EM_SETSEL, 0, -1);
        updateButtons();
        return;
    case idProtonCancelFolder:
        newFolderOpen_ = false;
        form_.layout();
        SetFocus(protonNewFolder_);
        return;
    case idProtonCreateFolder: createFolder(); return;
    case idProtonFolderName: case idProtonPassword:
        if (code == EN_CHANGE) updateButtons();
        return;
    case idProtonFolder:
        if (code == CBN_SELCHANGE) fieldChanged(id);
        return;
    case idProtonShare: case idProtonPrivate:
        if (code == BN_CLICKED) fieldChanged(id);
        return;
    case idProtonExpiration: case idProtonTimeout:
        if (code == EN_CHANGE) fieldChanged(id);
        return;
    case idProtonStorePassword: beginSetup("storeSharePassword", {}, true); return;
    case idProtonClearPassword: clearLinkPassword(); return;
    case idRemove: remove(); return;
    case idUp: move(-1); return;
    case idDown: move(1); return;
    case idDefault: makeDefault(); return;
    case idShowSecret: toggleSecret(); return;
    case idStore: store(); return;
    case idUnlock: unlock(); return;
    case idTest: test(); return;
    case idSecret:
        if (code == EN_CHANGE) updateButtons();
        return;
    case idKeychain: case idSession:
        if (code == BN_CLICKED) {
            auto* profile = current();
            if (!profile) return;
            const bool keychain = checked(keychain_);
            const auto profileId = profile->value("id").toString();
            (*profile)["credentials"] = json::Object{{"mode", keychain ? "keychain" : "session"}, {"reference", keychain ? json::Value("putgd/" + profileId) : json::Value()}};
            if (validate(*profile).empty()) main_.queuePatch({{"profiles", savableProfiles()}});
            updateButtons();
            refreshHint();
        }
        return;
    case idStorageClass: case idAcl: case idAddressing:
        if (code == CBN_SELCHANGE) fieldChanged(id);
        return;
    default:
        if (id >= idLabel && id <= idLowSpeedTime && code == EN_CHANGE) fieldChanged(id);
    }
}
}

std::unique_ptr<Page> createDestinationsPage(MainWindow& main) { return std::make_unique<DestinationsPageImpl>(main); }
}
