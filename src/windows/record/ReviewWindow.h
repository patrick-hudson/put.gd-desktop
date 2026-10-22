#pragma once
// The private review window every Stop opens (docs/recording.md, "Review
// window"): the frame at the playhead with its masks, one timeline (trim handles,
// a lane per overlapping mask, the playhead), a tool row and an action row. The
// window never owns the edit: each change is proposed to the resident, which
// applies it to the authoritative model and pushes the result back (setEdit).
// recording.review drives the same controls through drive().
#include "core/Recording.h"
#include "record/FrameSource.h"
#include <functional>
#include <memory>
#include <string>
#include <windows.h>

namespace putgd::record {
class ReviewWindow {
public:
    struct Callbacks {
        std::function<void(unsigned long long revision, const json::Array& commands)> edit;
        std::function<void(const std::string& path, unsigned long long revision)> saveCopy;
        std::function<void(unsigned long long revision)> upload;
        std::function<void()> discard;
        std::function<void(const json::Object& config)> configChanged;
    };
    ReviewWindow(std::string clip, RecordingSize canvas, long long durationUs, RecordingConfig, RecordingEdit, std::vector<std::string> formats, Callbacks);
    ~ReviewWindow();
    ReviewWindow(const ReviewWindow&) = delete;
    ReviewWindow& operator=(const ReviewWindow&) = delete;
    bool create();
    HWND hwnd() const { return hwnd_; }
    // Shows the window again (after a refused upload, with the resident's reason).
    void reopen(const std::string& error);
    void hide();
    void setEdit(const RecordingEdit&, long long animationMaxUs, int animationEdge);
    void setConfig(const RecordingConfig&);
    void setNotice(const std::string& text);
    void setExporting(bool exporting, const std::string& status);
    void setExportProgress(double fraction);
    // Keys while the window is in front; true when consumed.
    bool preTranslate(MSG&);
    // recording.review: one control action; the window's state after it, or an error.
    struct Drive { std::string code, message; json::Object state; };
    Drive drive(const json::Object& params);
    json::Object state() const;
    bool fileDialogOpen() const { return dialog_ != nullptr; }

    // Child windows call these.
    LRESULT frameMessage(HWND, UINT, WPARAM, LPARAM);
    LRESULT timelineMessage(HWND, UINT, WPARAM, LPARAM);
private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    void createControls();
    void layout();
    void refresh();
    void setPlayhead(long long us, bool decode = true);
    void play();
    void pause();
    void togglePlay() { playing_ ? pause() : play(); }
    void propose(json::Array commands);
    void setTrimIn();
    void setTrimOut();
    void resetTrim();
    void arm(const std::string& kind);
    void addMask(const std::string& kind, const RecordingRect& rect);
    void selectMask(const std::string& id);
    void setMaskTime(bool in);
    void setStrength(int strength);
    void removeMask();
    void chooseFormat(const std::string& format);
    void saveCopyDialog();
    void closeFileDialog();
    void upload();
    void discard();
    const RecordingMask* selected() const;
    json::Object maskJson(const RecordingMask&) const;
    // Geometry of the frame inside the view (letterboxed) and the timeline track.
    RECT videoRect() const;
    RECT trackRect() const;
    RecordingRect toCanvas(const RECT& view) const;
    RECT toView(const RecordingRect& canvas) const;
    void paintFrame(HDC, const RECT& client);
    void paintTimeline(HDC, const RECT& client);
    int scaled(int v) const { return MulDiv(v, int(dpi_), 96); }

    std::string clip_;
    RecordingSize canvas_;
    long long duration_;
    RecordingConfig config_;
    RecordingEdit edit_;
    std::vector<std::string> formats_;
    Callbacks callbacks_;
    std::unique_ptr<FrameSource> frames_;
    std::vector<uint8_t> frame_;
    long long frameTime_ = -1, playhead_ = 0, animationMaxUs_ = 60000000;
    int animationEdge_ = 1280;
    double speed_ = 1.0;
    bool playing_ = false, exporting_ = false, closing_ = false, selectNewest_ = false;
    unsigned long long request_ = 0;
    std::string selected_, armed_, status_, notice_, error_;
    double exportFraction_ = -1;
    // Gestures: a drag on the frame (new mask), on a trim handle or a mask end.
    enum class Drag { None, Mask, Scrub, TrimIn, TrimOut, MaskIn, MaskOut } drag_ = Drag::None;
    POINT dragFrom_{}, dragTo_{};
    long long dragTime_ = 0;
    UINT dpi_ = 96;
    HWND hwnd_ = nullptr, view_ = nullptr, timeline_ = nullptr, tooltip_ = nullptr;
    void* dialog_ = nullptr; // IFileDialog* while Save copy… is open
    std::string dialogLast_;
};
}
