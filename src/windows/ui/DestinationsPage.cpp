// Destinations: the profiles list (add, remove, reorder, make default) and the S3
// form. Valid non-secret edits autosave through the window's settings.patch
// queue; credentials go only to setup.storeCredentials and are never kept.
#include "ui/MainWindow.h"
#include "common/Text.h"
#include "core/S3Settings.h"
#include "core/Settings.h"
#include <algorithm>
#include <set>

namespace putgd::ui {
namespace {
enum : int {
    idList = 100, idAdd, idRemove, idUp, idDown, idDefault, idAddS3, idAddProton,
    idLabel = 200, idBucket, idRegion, idEndpoint, idPrefix, idStorageClass, idAcl, idPublicUrl, idAddressing,
    idConnect, idRequest, idLowSpeed, idLowSpeedTime, idAccess, idSecret, idShowSecret, idToken, idKeychain, idSession,
    idStore, idUnlock, idTest
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
    void activated() override { if (draft_.empty() && provisional_.empty()) rebuild(); refreshHint(); }
    void deactivated() override { hideRevealed(); }
    void settingsChanged(bool external) override;
    void invalidated(const std::string& kind) override { if (kind == "setup") refreshHint(); }
    bool validationFailed(const std::string& path, const std::wstring& message) override;
    bool dirty() const override;
    void discardDraft() override { provisional_.clear(); errors_.clear(); clearSecrets(); }
    bool pendingSecrets() const override { return !revealed_ && (GetWindowTextLengthW(access_) || GetWindowTextLengthW(secret_) || GetWindowTextLengthW(token_)); }
    void clearSecrets() override;
    json::Object saveState() const override { return selected_ >= 0 ? json::Object{{"profileIndex", selected_}} : json::Object{}; }
    void restoreState(const json::Object& state) override { if (state.value("profileIndex").isNumber()) restoreIndex_ = state.value("profileIndex").toInt(); }
    void dpiChanged() override { panel_.layout(); form_.layout(); }
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
    bool populating_ = false, revealed_ = false, typedVisible_ = false, secretStored_ = false;
    unsigned long long revealToken_ = 0, hintToken_ = 0;
    struct Job { std::string setupId, kind, profileId; };
    std::vector<Job> jobs_;
    bool polling_ = false;
};

HWND DestinationsPageImpl::create(HWND parent) {
    panel_.onLayout = [this](int w, int h) { return layoutPage(w, h); };
    panel_.onCommand = [this](int id, int code, HWND control) { command(id, code, control); };
    panel_.onTimer = [this](UINT_PTR id) { if (id == timerSetup) poll(); };
    HWND p = panel_.create(parent, false);
    list_ = listBox(p, idList);
    SetWindowSubclass(list_, listKeys, 1, 0);
    add_ = button(p, idAdd, L"Add destination\x2026");
    remove_ = button(p, idRemove, L"Remove\x2026");
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
    title_ = heading(f, L"");
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
    unlock_ = button(f, idUnlock, L"Unlock saved credentials");
    hint_ = ui::label(f, L"");
    form_.markMuted(hint_);
    testHeading_ = heading(f, L"Test connection");
    test_ = button(f, idTest, L"Test connection");
    setTooltip(main_.tooltip(), test_, L"Uploads a small synthetic file, checks its public link and removes it again.");
    testResult_ = ui::label(f, L"");
    return p;
}

int DestinationsPageImpl::layoutPage(int width, int height) {
    const UINT d = panel_.dpi();
    const int pad = scale(12, d), gap = scale(8, d), left = scale(250, d);
    Rows rows(d, pad, pad, left, 0);
    const int buttonHeight = rows.buttonHeight();
    const int listBottom = height - pad - 3 * buttonHeight - 3 * gap;
    place(list_, pad, pad, left, listBottom - pad);
    int y = listBottom + gap;
    place(add_, pad, y, left, buttonHeight);
    y += buttonHeight + gap;
    const int half = (left - gap) / 2;
    place(up_, pad, y, half, buttonHeight);
    place(down_, pad + half + gap, y, half, buttonHeight);
    y += buttonHeight + gap;
    place(default_, pad, y, half, buttonHeight);
    place(remove_, pad + half + gap, y, half, buttonHeight);
    place(form_.hwnd(), pad + left + pad, 0, width - left - 2 * pad, height);
    return height;
}

int DestinationsPageImpl::layoutForm(int width, int height) {
    const UINT d = form_.dpi();
    Rows rows(d, scale(8, d), scale(12, d) + form_.offsetY(), std::max(scale(300, d), width - scale(28, d)), 210);
    auto* profile = current();
    const bool any = profile != nullptr;
    const bool s3 = any && profile->value("provider") == "s3";
    show(empty_, !any);
    for (HWND h : {title_, provider_, labels_["label"], fields_["label"]}) show(h, any);
    for (auto& [key, label] : errorLabels_) show(label, false);
    for (HWND h : {credentialsHeading_, accessLabel_, secretLabel_, tokenLabel_, access_, secret_, show_, token_, keychain_, session_, store_, unlock_, hint_,
                   advanced_, storageClass_, acl_, addressing_, testHeading_, test_, testResult_})
        show(h, s3);
    for (const auto& key : textFields) if (key != "label") { show(labels_[key], s3); show(fields_[key], s3); }
    for (const auto* key : {"storageClass", "aclMode", "addressing"}) show(labels_[key], s3);
    if (!any) { rows.text(empty_, 2); return rows.y() - form_.offsetY(); }
    const int line = lineHeight(d);
    rows.heading(title_);
    rows.text(provider_);
    if (GetWindowTextLengthW(general_)) { show(general_, true); rows.text(general_, 2); } else show(general_, false);
    auto field = [&](const std::string& key, int fieldWidth = 0) { rows.field(labels_[key], fields_[key], fieldWidth); rows.error(errorLabels_[key]); };
    field("label", 360);
    if (!s3) return rows.y() - form_.offsetY();
    field("bucket", 360);
    field("region", 240);
    field("endpoint");
    field("objectPrefix");
    rows.field(labels_["aclMode"], acl_, 360);
    rows.error(errorLabels_["aclMode"]);
    field("publicUrlBase");
    rows.heading(advanced_);
    rows.field(labels_["storageClass"], storageClass_, 280);
    rows.error(errorLabels_["storageClass"]);
    rows.field(labels_["addressing"], addressing_, 280);
    rows.error(errorLabels_["addressing"]);
    for (const auto& key : numberFields) field(key, 160);
    rows.heading(credentialsHeading_);
    rows.field(accessLabel_, access_, 360);
    rows.field(secretLabel_, secret_, 360, show_, 80);
    rows.field(tokenLabel_, token_, 360);
    rows.line({{keychain_, 380}}, true, rows.fieldHeight());
    rows.line({{session_, 380}}, true, rows.fieldHeight());
    rows.line({{store_, 150}, {unlock_, 190}}, true);
    if (GetWindowTextLengthW(hint_)) rows.block(hint_, line * 2 + 2);
    rows.heading(testHeading_);
    rows.line({{test_, 160}}, true);
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
        if (profile.value("provider") != "s3") text += L" (not available on Windows)";
        else if (provisional_.count(id)) text += L" (not saved)";
        else if (isDefault(id)) text += L" (default)";
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
    selected_ = index;
    populate();
    refreshHint();
    updateButtons();
}

void DestinationsPageImpl::populate() {
    populating_ = true;
    auto* profile = current();
    if (profile) {
        const auto id = profile->value("id").toString();
        const auto& settings = profile->value("settings").objectRef();
        setText(title_, profile->value("label").toString(id));
        setTextW(provider_, profile->value("provider") == "s3" ? L"Amazon S3 or S3-compatible service"
                                                               : wide(profile->value("provider").toString()) + L" \x2014 not available on Windows");
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
    enable(remove_, any);
    enable(up_, any && selected_ > 0);
    enable(down_, any && size_t(selected_ + 1) < draft_.size());
    enable(default_, stored && s3 && !isDefault(id));
    enable(store_, stored && s3);
    enable(unlock_, stored && s3 && profile->value("credentials")["mode"] == "keychain");
    bool testRunning = false;
    for (const auto& job : jobs_) testRunning = testRunning || (job.profileId == id && job.kind == "testDestination");
    enable(test_, stored && s3 && !testRunning);
    setTextW(test_, testRunning ? L"Testing\x2026" : L"Test connection");
    enable(show_, s3 && (stored || GetWindowTextLengthW(secret_)));
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
    main_.flushSettings([this, order](bool saved) {
        if (!saved) { rebuild(); return; }
        main_.session().call("settings.reorderProfiles", {{"order", json::stringArray(order)}, {"expectedRevision", main_.session().revision()}}, [this](const Reply& reply) {
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
    for (HWND h : {access_, secret_, token_}) wipe(h);
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
        main_.status(UiSession::describe(reply), true);
        if (kind == "testDestination") { testResults_[profileId] = L"Connection test did not start. " + UiSession::describe(reply); populate(); }
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
        testResults_[id] = L"Testing the connection\x2026";
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
            // Move it to the back so several jobs share the poll.
            auto copy = *it;
            jobs_.erase(it);
            jobs_.push_back(copy);
            return;
        }
        jobs_.erase(it);
        if (jobs_.empty()) KillTimer(panel_.hwnd(), timerSetup);
        Error error = reply.ok ? Error::fromJson(reply.result.value("error").toObject()) : reply.error;
        if (!reply.ok && reply.error.code == "setup_not_found") error.message = "This setup result is no longer available; the action was not repeated.";
        const bool success = reply.ok && error.code.empty() && state != "failed";
        const auto result = reply.result.value("result").toObject();
        Reply failure;
        failure.error = error;
        if (job.kind == "testDestination") {
            std::wstring text = success ? L"Connection successful. Test upload, public link and cleanup passed."
                                        : L"Connection test failed. " + UiSession::describe(failure);
            if (!result.empty())
                text += L"\nUpload: " + stepText(result.value("upload")) + L"\nPublic link: " + stepText(result.value("publicGet")) + L"\nCleanup: " + stepText(result.value("cleanup"));
            if (!success && error.code == "credential_setup_required") text += L"\nStore or unlock credentials, then test again.";
            testResults_[job.profileId] = text;
            main_.status(success ? L"Connection test passed." : L"Connection test failed.", !success);
        } else if (job.kind == "storeCredentials") {
            main_.status(success ? L"Credentials stored." : L"Credentials were not stored. " + UiSession::describe(failure), !success);
        } else if (job.kind == "unlockCredentials") {
            main_.status(success ? L"Saved credentials unlocked." : L"Saved credentials were not unlocked. " + UiSession::describe(failure), !success);
        }
        auto* now = current();
        if (now && now->value("id").toString() == job.profileId) {
            setTextW(testResult_, testResults_[job.profileId]);
            refreshHint();
        }
        updateButtons();
        form_.layout();
    });
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
        AppendMenuW(menu, MF_STRING | MF_GRAYED, idAddProton, L"Proton Drive (not available on Windows)");
        RECT r{};
        GetWindowRect(add_, &r);
        const int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, main_.hwnd(), nullptr);
        DestroyMenu(menu);
        if (chosen == idAddS3) addS3();
        return;
    }
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
