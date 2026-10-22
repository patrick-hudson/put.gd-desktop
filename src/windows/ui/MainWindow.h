#pragma once
// The single put.gd window of the UI helper: a navigation rail (History,
// Destinations, Settings, then the capture actions) beside the current page. It owns the
// control-channel lifecycle (Protocol.h, role "ui"), the view commands the resident
// forwards (ui.moveDestination, ui.diagnostics, ui.historySaveDiagnostics,
// ui.fileDialog), the view-state checkpoints, the shared 400 ms settings autosave
// and the close checkpoint handshake.
#include "common/Frames.h"
#include "ui/Controls.h"
#include "ui/UiSession.h"
#include <array>
#include <deque>
#include <map>
#include <memory>

namespace putgd::ui {
inline constexpr UINT controlFrameMessage = WM_APP + 11;  // lParam: protocol::Frame* (owned by the receiver)
inline constexpr UINT controlClosedMessage = WM_APP + 12; // the resident's pipe closed
inline constexpr UINT deferredMessage = WM_APP + 13;      // lParam: std::function<void()>*

class MainWindow;
// A view command's answer: an error (empty code for success) and the result object.
using CommandReply = std::function<void(Error, json::Object)>;

// One tab. Pages create their own child panels and talk to the resident through
// main().session(); settings edits go through main().queuePatch().
class Page {
public:
    explicit Page(MainWindow& main) : main_(main) {}
    virtual ~Page() = default;
    Page(const Page&) = delete;
    virtual HWND create(HWND parent) = 0;
    virtual void activated() {}
    virtual void deactivated() {}
    // The cached settings changed: external (another writer or Reload) or after this window's own save.
    virtual void settingsChanged(bool external) {}
    // invalidate {kind}: history, operations, setup, update (settings is handled by the window).
    virtual void invalidated(const std::string& kind) {}
    // A rejected settings change: true when the page showed it next to a field.
    virtual bool validationFailed(const std::string& path, const std::wstring& message) { return false; }
    // Local edits that are not saved (invalid values, an unsaved destination, recording a shortcut).
    virtual bool dirty() const { return false; }
    virtual void discardDraft() {}
    // Typed credentials that were neither stored nor cleared.
    virtual bool pendingSecrets() const { return false; }
    virtual void clearSecrets() {}
    virtual json::Object saveState() const { return {}; }
    virtual void restoreState(const json::Object&) {}
    // History: "history" or "activity". Settings: capture, general, shortcuts, advanced, updates.
    virtual void showSection(const std::string&) {}
    // A view command (docs/ui-process-protocol.md); true when this page handles it.
    // `answer` is called exactly once, possibly later.
    virtual bool viewCommand(const std::string& name, const json::Object& params, const CommandReply& answer) { return false; }
    virtual void dpiChanged() {}
protected:
    MainWindow& main_;
};
std::unique_ptr<Page> createHistoryPage(MainWindow&);
std::unique_ptr<Page> createDestinationsPage(MainWindow&);
std::unique_ptr<Page> createSettingsPage(MainWindow&);

class MainWindow {
public:
    enum PageIndex { HistoryPage = 0, DestinationsPage = 1, SettingsPage = 2 };
    MainWindow(UiSession&, frames::Channel&);
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    // Creates the window hidden; the first activate frame shows it.
    bool create(const std::string& initialView, const json::Object& state);
    HWND hwnd() const { return hwnd_; }
    UiSession& session() { return session_; }
    UINT dpi() const { return windowDpi(hwnd_); }
    HWND tooltip() const { return tooltip_; }
    // The message loop's pre-translation (tab switching, shortcut capture, dialog keys).
    bool preTranslate(MSG&);

    // ---- settings autosave (settings.patch with the cached observedRevision) ----
    void queuePatch(const json::Object& patch);
    // Sends queued edits now and calls `then` once nothing is pending (false when saving failed).
    void flushSettings(std::function<void(bool saved)> then);
    bool settingsBusy() const { return hasPending_ || inFlight_; }
    // Explicit Reload: drops every draft and re-reads settings.get.
    void reloadSettings();
    // Re-reads settings.get; a dirty draft keeps its values and shows the changed-on-disk bar.
    void fetchSettings();
    // Mutations that are not patches (settings.reorderProfiles, settings.reload) report their outcome here.
    void handleSettingsError(const Reply&);
    // Adopts an authoritative settings.get result after such a mutation (pages see an own change).
    void adoptSettings(const json::Object& document, const json::Object& metadata);

    // ---- actions ----
    void status(const std::wstring& text, bool error = false);
    void invokeAction(const std::string& action);
    void openEditor(const std::string& source, const std::string& path = {});
    void chooseImageToEdit();
    void showEditMenu(POINT screen);
    void showAbout();
    void quit();
    void showView(const std::string& view);
    void setPage(int index);
    int page() const { return page_; }
    Page* pageAt(int index) { return pages_[size_t(index)].get(); }
    void applyTheme(const std::string& themeSetting = {});
    // Keyboard input for this control bypasses dialog navigation (shortcut capture).
    void setKeyCapture(HWND control) { keyCapture_ = control; }
    void shortcutRecording(bool active, std::function<void(bool ok, const std::string& error)> reply);
    void post(std::function<void()>);
    static std::wstring actionLabel(const std::string& action);
    // What a route does (the rail tooltips and Settings → Capture's routing state).
    static std::wstring routeText(const json::Object& document, bool upload, const std::string& profileId, const std::string& publicationMode);
    // Pages call this after a change ui.viewState reflects; a checkpoint follows 250 ms later.
    void viewStateChanged();
    FileChooser::Options imageFileOptions() const;
private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    void layout();
    void paint();
    void onFrame(const protocol::Frame&);
    void handleCommand(const json::Object& payload);
    void sendCheckpoint();
    json::Object appearance() const;
    void sendPending();
    void finishSave();
    void broadcastSettings(bool external);
    bool anyDirty() const;
    void showNotice(const std::wstring& text, int action);
    void hideNotice();
    void updateRail();
    void updateSaveStatus();
    void requestClose();
    void sendCloseRequest();
    void finishClose();
    json::Object checkpoint() const;
    void restore(const json::Object& state);
    bool send(const std::string& type, const json::Object& payload = {});

    UiSession& session_;
    frames::Channel& channel_;
    HWND hwnd_ = nullptr, tooltip_ = nullptr, status_ = nullptr, saveStatus_ = nullptr;
    std::array<HWND, 3> nav_{};
    bool statusError_ = false, themeApplied_ = false;
    HWND notice_ = nullptr, noticeButton_ = nullptr, railHeading_ = nullptr, version_ = nullptr, about_ = nullptr, quit_ = nullptr;
    HWND keyCapture_ = nullptr, lastFocus_ = nullptr;
    std::map<std::string, HWND> rail_;
    std::array<std::unique_ptr<Page>, 3> pages_;
    std::array<HWND, 3> pageWindows_{};
    int page_ = 0;
    std::string view_;
    bool activated_ = false, exiting_ = false;
    FileChooser editImage_;
    std::string lastCheckpoint_;

    // autosave
    json::Object pending_, inFlightPatch_, failed_;
    bool hasPending_ = false, inFlight_ = false, blocked_ = false, fetching_ = false, fetchAgain_ = false, fetchAfterSave_ = false;
    std::vector<std::function<void(bool)>> idle_;
    std::wstring saveText_;
    int noticeAction_ = 0;

    // close handshake
    bool closing_ = false, cancelling_ = false, hiddenForCapture_ = false;
    unsigned long long closeSequence_ = 0;

    struct RecordingReply { unsigned long long sequence; std::function<void(bool, const std::string&)> callback; };
    std::deque<RecordingReply> recordingReplies_;
};
}
