#pragma once
#include "core/KeptSpans.h"
#include "core/RecordingModel.h"
#include "MaskNames.h"
#include <QFrame>
#include <QJsonObject>
#include <QWidget>
#include <memory>
#include <optional>

namespace putgd::record {
// #1613: the review window's right panel, 280 px wide, from the tool row to the
// footer. Spec: put.gd-evidence record 2026-11-17-recording-review-redesign,
// design.md "Right panel"; pictures after-open.png / after-edit.png, source
// mock/review4.html. The top part follows what is selected:
//  - nothing edited and nothing selected: "No edits yet" · "Upload sends the
//    whole clip." · the five gestures;
//  - a time selection: its span and length · Cut · Redact · Blur;
//  - a cut: its span and "n s left out of the clip" · Restore (default) ·
//    Play across;
//  - a cover: its name · the privacy sentence (Blur: the warning sentence) ·
//    Shows from → / Until → with m:ss.s fields and Set to playhead · Redact |
//    Blur (Blur adds its strength).
// Below, once anything is edited: Edits, every trim, cut and cover in source
// order (icon, name, span; a selected row shows its verb in place of the span
// unless the part above already offers it; other rows show it on hover).
//
// It shows and proposes only: every control emits a signal, the window turns
// it into an edit or a seek, and setState() is what it then shows.
class ReviewPanel final : public QWidget {
    Q_OBJECT
public:
    struct State {
        RecordingEdit edit;
        qint64 durationUs = 0;
        int fps = 30;
        recording::KeptSpans kept;
        std::optional<recording::Span> timeSelection;
        QString selectedKind, selectedId;  // "trim" | "cut" | "cover" | ""
        // The selected cover's first and last kept covered frame (source
        // time), computed by the window; absent: "Excluded from output".
        std::optional<qint64> coverFirstKept, coverLastKept;
        qint64 playheadUs = 0;
    };
    explicit ReviewPanel(std::shared_ptr<MaskNames> names, QWidget* parent = nullptr);
    void setState(const State&);
    const State& state() const { return state_; }
    // {mode: empty|selection|cut|cover|edits, title, rows: [{kind, id, name,
    //  span: {start, end}, selected}], buttons: [{name, enabled}]}
    QJsonObject inspect() const;
    // The button Enter presses in the current mode (Restore for a cut), or null.
    QWidget* defaultControl() const;
signals:
    void cutRequested();                 // a time selection's Cut
    void coverRequested(const QString& kind);  // "solid" (Redact) | "blur"
    void restoreCut(const QString& id);
    void playAcross(const QString& id);
    // Shows from → / Until →: seek to the first / last kept covered frame.
    void seekCoverEdge(const QString& id, bool start);
    void coverEdgeToPlayhead(const QString& id, bool start);
    void coverEdgeEdited(const QString& id, bool start, qint64 sourceUs);
    void coverKindChanged(const QString& id, const QString& kind);
    void coverStrengthChanged(const QString& id, int strength);
    // A row in Edits was activated (click / Enter): select it.
    void editActivated(const QString& kind, const QString& id);
    // A row's verb: Reset (trim), Restore (cut), Remove (cover).
    void resetTrim();
    void removeCover(const QString& id);
private:
    void rebuild();
    std::shared_ptr<MaskNames> names_;
    State state_;
};

// #1613: below ~1180×720 the panel folds into the tool row's Edits toggle and
// opens as this overlay: 300 px wide, anchored under the toggle, scrolls,
// takes focus on the panel's default control, and Esc or a click outside
// closes it with focus back on the toggle. It reparents the window's one
// ReviewPanel while open and gives it back on close.
class EditsOverlay final : public QFrame {
    Q_OBJECT
public:
    explicit EditsOverlay(QWidget* window);
    void openFor(ReviewPanel* panel, QWidget* toggle);
    void dismiss();
    bool isOpen() const;
signals:
    void closed();  // after the panel was given back
protected:
    void keyPressEvent(QKeyEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;
};
}
