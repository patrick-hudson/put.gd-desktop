#pragma once
// The Win32 "Edit & upload" window: tool bar, style strip, the interactive
// canvas and the Save a copy / Cancel / Upload footer (docs/image-editor.md,
// "User workflow"). It edits the EditDocument it is given; EditorMain owns the
// protocol, the document and finalization.
#include "common/Error.h"
#include "core/FileDialogState.h"
#include "editor/EditDocument.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>

namespace putgd::editor {
class Canvas;

enum class Tool { Select, Redact, Blur, Crop, Arrow, Rectangle, Highlight, Text };
struct ToolStyle {
    Rgb color = defaultAnnotationColor;
    int width = 4;
    int strength = 12;
    int fontPixels = 28;
    std::string fontFamily;
};

class EditorWindow {
public:
    struct Callbacks {
        std::function<void()> upload;                          // Upload or Ctrl+Enter
        std::function<void()> cancel;                          // Cancel, or close after confirming
        std::function<Error(const std::string& path)> saveCopy; // Save a copy… after the dialog
    };
    EditorWindow(EditDocument&, std::string destinationLabel, std::vector<std::string> recentColors, Callbacks);
    ~EditorWindow();
    EditorWindow(const EditorWindow&) = delete;
    EditorWindow& operator=(const EditorWindow&) = delete;

    bool create();
    HWND hwnd() const { return hwnd_; }
    void raise();
    // Call before changing the document from outside (agent commands): a
    // pointer gesture in progress is cancelled so its draft never lands.
    void abortGesture();
    // The document changed outside the window (agent commands): refresh views.
    void documentEdited();
    // Commits a pending text entry and applies a pending crop (UI submit).
    void commitPendingInput();
    // Disables every control and shows a status (finalization).
    void setBusy(const std::string& message);
    void setFailure(const std::string& message);
    void setMessage(const std::string& message);
    // Up to six custom colours, most recent first ("#rrggbb").
    std::vector<std::string> recentColors() const { return recentColors_; }
    // Window-level shortcuts; true when the message was consumed.
    bool preTranslate(MSG&);
    // Cancels open modal dialogs (file chooser, colour picker, confirmation).
    void closeDialogs();
    bool inModalLoop() const { return modal_ > 0; }
    // editor.fileDialog (docs/file-dialogs.md): Save a copy… as its button opens it,
    // shown from the next message so the command answers first.
    bool openSaveCopyDialog(); // false while it is open or the window is busy
    void cancelSaveCopyDialog();
    bool saveCopyDialogOpen() const { return saveDialog_.open; }
    json::Object saveCopyDialogState() const { return saveDialog_.toJson(); }
    void allowClose() { closing_ = true; }

    // Canvas notifications.
    void canvasChanged();
    void canvasSelectionChanged();
    void canvasViewChanged();
    void canvasToolRequested(Tool);
    TextSize measure(const std::string& text, const std::string& family, int pixels) const;

private:
    static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
    static HRESULT CALLBACK taskDialogCallback(HWND, UINT, WPARAM, LPARAM, LONG_PTR);
    LRESULT handle(UINT, WPARAM, LPARAM);
    void createControls();
    void applyFonts();
    void layout();
    int layoutOptions(int top, int height);
    void selectTool(Tool);
    void syncStyle(bool applyToSelection);
    void refresh();
    void useColor(Rgb);
    void pickCustomColor();
    void rebuildRecentColors();
    void saveCopy();
    void requestCancel();
    void undo();
    void redo();
    void drawSwatch(const DRAWITEMSTRUCT&);
    void populateFonts();
    void selectFontFamily(const std::string&);
    std::string currentFontFamily() const;
    int currentWidth() const;
    int currentFontPixels() const;
    int scaled(int value) const { return MulDiv(value, int(dpi_), 96); }
    HWND control(int id) const { return GetDlgItem(hwnd_, id); }
    bool highlightContext() const;

    EditDocument& doc_;
    std::string destination_;
    std::vector<std::string> recentColors_;
    Callbacks callbacks_;
    HWND hwnd_ = nullptr, tooltip_ = nullptr;
    std::unique_ptr<Canvas> canvas_;
    HFONT font_ = nullptr, boldFont_ = nullptr;
    UINT dpi_ = 96;
    Rgb annotationColor_ = defaultAnnotationColor, highlightColor_ = defaultHighlightColor, shownColor_ = defaultAnnotationColor;
    std::vector<std::string> fontFamilies_;
    std::string defaultFamily_;
    bool busy_ = false, closing_ = false, syncing_ = false, failure_ = false;
    int modal_ = 0;
    HWND taskDialog_ = nullptr, lastFocus_ = nullptr;
    void* fileDialog_ = nullptr; // IFileDialog* while Save a copy is open
    FileDialogState saveDialog_;
    bool saveDialogCancelled_ = false;
    std::wstring message_;
};
}
