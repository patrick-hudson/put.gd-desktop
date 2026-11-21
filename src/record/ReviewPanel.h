#pragma once
#include "core/KeptSpans.h"
#include "core/RecordingModel.h"
#include "MaskNames.h"
#include <QFrame>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <memory>
#include <optional>

class QLineEdit;
class QVBoxLayout;
class QKeyEvent;
class QLayout;
class QScrollArea;
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
// Above it, once anything is edited: Edits, every trim, cut and cover in
// source order, in its own scroll area of about 4½ rows (icon, name, span, and
// a 28 px trailing icon button for the row's verb, in space the row always
// keeps, shown on the selected or focused row and after 150 ms of hover); the
// details of what is selected sit below the list.
// Controls update in place while what they are made of stays the same; the
// blur strength stepper coalesces its steps for 350 ms into one edit.
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
        QString placingKind;  // "solid" | "blur" while a cover is being placed: Cut waits (W21)
    };
    explicit ReviewPanel(std::shared_ptr<MaskNames> names, QWidget* parent = nullptr);
    void setState(const State&);
    const State& state() const { return state_; }
    // {mode: empty|selection|cut|cover|edits, title, rows: [{kind, id, name,
    //  span: {start, end}, selected}], buttons: [{name, enabled}]}
    QJsonObject inspect() const;
    // The button Enter presses in the current mode (Restore for a cut), or null.
    QWidget* defaultControl() const;
    // The control the Edits overlay focuses: defaultControl(), else the first
    // enabled control (a row of Edits, a field, a button), else null.
    QWidget* firstControl() const;
    // recording.review setCoverEdge: commits a time to the selected cover's
    // Shows from (start) or Until field as typing it does (frame snapping, then
    // coverEdgeEdited). False when no cover's fields are shown.
    bool commitCoverEdge(bool start, qint64 sourceUs);
    // The Edits row of an edit (kind "trim" with no id), or null.
    QWidget* rowFor(const QString& kind, const QString& id) const;
    // Proposes a strength still coalescing (a selection change, or the window before an upload).
    void flushStrength();
    // Ctrl+Z during the strength's coalescing takes back only that step (Q18).
    bool cancelPendingStrength();
    void commitFields();
    // Q20: the row of a cut or cover selected elsewhere scrolls into view.
    void revealRow(const QString& kind, const QString& id);
    // Q22: the item hovered elsewhere looks hovered in its row (kind "" for none).
    void setLinkedHover(const QString& kind, const QString& id);
    // The height the panel may take (the folded overlay's room); 0: no limit.
    void setAvailableHeight(int height);
    QSize sizeHint() const override;
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
    // The strength being stepped, at once, for the picture (R29); -1: put back.
    void coverStrengthPreview(const QString& id, int strength);
    // A row in Edits was activated (click / Enter): select it.
    void editActivated(const QString& kind, const QString& id);
    // A row's verb: Reset (trim), Restore (cut), Remove (cover).
    void resetTrim();
    void removeCover(const QString& id);
    // Enter in a time field or the strength (or Esc on unchanged text): the
    // window hands the keys back to the timeline or the Edits overlay (W17).
    void focusReturnRequested();
    // The pointer is over an Edits row (kind "" when it left), for hover linking (Q22).
    void rowHovered(const QString& kind, const QString& id);
protected:
    void paintEvent(QPaintEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;
private:
    QString topStructure(const State&) const;
    QString listStructure(const State&) const;
    QString neighbourOf(const QString& rowKey) const;
    void updateInPlace();
    void updateRows();
    void updateSpanForm();
    void fitRows();
    void rebuild(bool rebuildTop = true, bool rebuildList = true);
    void buildTop(QVBoxLayout*);
    void buildList(QVBoxLayout*);
    void addEdge(QVBoxLayout*, const RecordingMask&, bool start);
    void commitEdge(QLineEdit* field, const QString& id, bool start, qint64 sourceUs);
    const RecordingMask* selectedCover() const;
    const RecordingCut* selectedCut() const;
    bool anyEdit() const;
    std::shared_ptr<MaskNames> names_;
    State state_;
    QString mode_ = QStringLiteral("empty"), title_;
    QWidget *top_ = nullptr, *list_ = nullptr, *rule_ = nullptr;
    QVBoxLayout* root_ = nullptr;
    QWidget* defaultControl_ = nullptr;
    bool building_ = false;
    QString builtTop_, builtList_;  // what the controls on screen are made of
    std::optional<QString> focusAfter_;  // the row to focus after a Restore or Remove ("" : the timeline)
    QElapsedTimer activated_;
    QString activatedKey_;
    QPointer<QWidget> rowsArea_;
    int rowsNatural_ = 0, availableHeight_ = 0;
    QTimer strengthTimer_;
    std::optional<std::pair<QString, int>> strengthPending_;
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
    // The widget it ends 8 px above (the transport).
    void setFloor(QWidget* floor);
    // Presses there only close the overlay (the picture); elsewhere they also act.
    void setConsumeArea(QWidget* area);
    void dismiss();
    bool isOpen() const;
signals:
    void closed();  // after the panel was given back
    void dismissedByPointer();  // closed by a press outside (the keys go back to the timeline)
protected:
    void keyPressEvent(QKeyEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;
    void paintEvent(QPaintEvent*) override;
private:
    void place();
    void giveBack();
    QPointer<QWidget> window_, toggle_, previousParent_, floor_, consumeArea_;
    QPointer<QLayout> previousLayout_;
    QPointer<ReviewPanel> panel_;
    QScrollArea* scroll_ = nullptr;
    // Where the panel lived, to put it back: a box layout index and stretch, or
    // a grid cell, and the width limits and visibility it had.
    int boxIndex_ = -1, boxStretch_ = 0, gridRow_ = -1, gridColumn_ = 0, gridRowSpan_ = 1, gridColumnSpan_ = 1;
    Qt::Alignment alignment_;
    int minimumWidth_ = 0, maximumWidth_ = QWIDGETSIZE_MAX;
    bool wasHidden_ = false;
};
}
