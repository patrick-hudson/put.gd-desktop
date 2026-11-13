#pragma once
#include "editor/EditorCanvas.h"
#include "EditorText.h"
#include "Ui.h"
#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QWidget>
#include <functional>
class QTimer;class QThread;class QSplitter;class QVBoxLayout;class QScrollArea;class QLabel;class QPushButton;class QToolButton;class QComboBox;class QSlider;class QButtonGroup;class QFontComboBox;class QSpinBox;class QHBoxLayout;class QGridLayout;class QBoxLayout;class QDialog;

namespace putgd {
class FileDialog;
// One themed review window for one image. It owns no storage, credentials or
// history; the hosting helper turns its explicit Upload into a private export.
class EditorWindow final:public QWidget {
 Q_OBJECT
public:
 EditorWindow(editor::EditDocument&,QWidget* parent=nullptr);
 ~EditorWindow() override;
 editor::EditorCanvas& canvas() {return *canvas_;}
 void setBusy(const QString& message);
 void setFailure(const QString& message);
 // A recoverable Upload failure (encoding, size): editing resumes with every
 // control back, and the reason sits above the actions with Keep editing and
 // Save a copy….
 void showPreparationFailed(const QString& reason);
 // Finishes text entry and applies a pending crop. Upload and Save a copy…
 // both run it first, so each exports exactly what the window shows.
 void finishPendingEdits();
 // Cancel / Discard…: with edits, opens the discard confirmation (not modal
 // to the event loop, so editor.view can read and answer it); emits
 // cancelRequested at once without edits, or when the user agrees.
 void requestCancel();
 void allowClose() {closing_=true;}
 void selectTool(editor::Tool);
 // Agent commands change the document directly; refresh selection, tiles and controls.
 void documentEdited();
 static QSize initialSize(const QSize& image,const QSize& availableScreen);
 // Custom colors picked in the editor, most recent first (#rrggbb). The host
 // seeds them from settings and reports them back when the editor closes.
 void setRecentColors(const QStringList&);
 QStringList recentColors() const {return recentColors_;}
 // The output Upload uses, as the resident phrases it, shown in the footer.
 void setDestination(const QString&);
 // Save a copy…: the desktop's chooser (#259), then the sanitized PNG; false
 // while busy or while the chooser is already open. editor.fileDialog drives it.
 bool saveCopy();
 FileDialog& fileDialog() {return *chooser_;}
 // Snapshots pending edits, then renders and writes off the GUI thread.
 // Completion preserves editor.saveCopy's saved bytes/digest reply.
 using CopyDone=std::function<void(const QByteArray&,const QString&)>;
 bool writeCopy(const QString& path,CopyDone done={});
 bool savingCopy() const {return savingCopy_;}
 static QString pngCopyPath(const QString& path);
 // A copy written by the window or by editor.saveCopy: its name, Show in
 // folder and Copy path over the canvas, the full path in the
 // tooltip and accessible description. It is recorded against the
 // document's generation: after another edit it reads "Earlier copy saved"
 // while the header keeps "Not uploaded". A failure (an empty path: the
 // chooser itself failed) keeps the draft and offers Choose another location….
 void showSaved(const QString& path);
 void showSaveFailed(const QString& path,const QString& reason);
 // Show in folder for the last saved copy (the host's file manager or portal).
 bool showSavedCopy();
 // Copy path: the last saved copy's full path to the clipboard.
 bool copySavedPath();
 void dismissSavedCopy();
 // editor.view: the tool, selection, crop draft, zoom and pan as the window
 // shows them, and one control per call through the same methods as the
 // window's own controls (see ImageEditor.cpp for the actions).
 QJsonObject viewState() const;
 QJsonObject viewCommand(const QJsonObject& params,QString& code,QString& message);
 // Copy image (Ctrl+Shift+C): the host hands the PNG from exportForCopy to
 // the resident, which owns the clipboard; the window shows the outcome.
 bool requestCopy();
 // `stage` runs on the worker after encoding (the host writes and hashes the
 // staged file there); `done` gets the PNG, its size and stage's result.
 using ExportDone=std::function<void(const QByteArray& png,const QSize& size,const QString& staged)>;
 bool exportForCopy(ExportDone done,std::function<QString(const QByteArray&)> stage={});
 bool copying() const {return copying_;}
 void showCopied();
 void showCopyFailed(const QString& reason);
 // Copy text (#1428): the tool's recognition and selection.
 EditorTextRecognition& text() {return *text_;}
 // The resident owns the clipboard: the host sends it the text and `done`
 // gets its answer (copy_suppressed when the user copied something since).
 using TextCopier=std::function<void(const QString& text,std::function<void(Error)> done)>;
 void setTextCopier(TextCopier copier) {textCopier_=std::move(copier);}
 // Copy selection, Copy all text and a code's Copy and Open link, as the
 // panel's buttons. `done` gets {copyStatus, characters} or the launch result.
 using TextDone=std::function<void(Error,QJsonObject)>;
 void copyRecognizedText(bool all,TextDone done={});
 void copyCode(int index,TextDone done={});
 void openCode(int index,TextDone done={});
 // After an agent changed the text selection: the tint and the panel follow.
 void textSelectionEdited();
 // Quick redact (#1428): suggested Redact marks for these categories, added as
 // one undo step once the current revision is recognized (starting or joining
 // its job). edit_stale when a Redact mark or the crop changes first. `done`
 // gets {created: [shape ids], counts: {category: matches}}.
 void quickRedact(const QStringList& categories,TextDone done={});
signals:
 void uploadRequested();
 void cancelRequested();
 void copyRequested();
protected:
 void closeEvent(QCloseEvent*) override;
 void showEvent(QShowEvent*) override;
 void resizeEvent(QResizeEvent*) override;
 bool eventFilter(QObject*,QEvent*) override;
private:
 void chooseCopy(const QString& path);
 void setSavingCopy(bool saving);
 void refresh();
 void syncSaved();
 void placeDestination();
 void placeSaveFeedback();
 void placeTools();
 void placeDocument();
 void placeProperties();
 // The preparation failure strip; empty hides it. While it shows, its own
 // Save a copy… stands in for the footer's.
 void setNotice(const QString& reason);
 void answerDiscard(bool discard);
 void syncGeometry();
 void applyGeometry();
 QList<QWidget*> actionable() const;
 void syncStyle(bool applyToSelection);
 QToolButton* makeSwatch(const QColor&);
 void useColor(const QColor&);
 void pickCustomColor();
 void rebuildRecentColors();
 editor::EditDocument& doc_;
 editor::EditorCanvas* canvas_=nullptr;
 QButtonGroup* tools_=nullptr;
 QHash<int,QToolButton*> toolButtons_;
 QList<QToolButton*> swatches_,recentSwatches_;
 QStringList recentColors_;
 QButtonGroup* swatchGroup_=nullptr;
 QHBoxLayout* recentLayout_=nullptr;
 QToolButton* customColor_=nullptr;
 QFontComboBox* font_=nullptr;
 QSpinBox* textSize_=nullptr;
 QToolButton* status_=nullptr;
 QToolButton *undo_=nullptr,*redo_=nullptr;
 QToolButton* savedMore_=nullptr;
 QWidget *colorRow_=nullptr,*widthRow_=nullptr,*blurRow_=nullptr,*textRow_=nullptr,*cropRow_=nullptr,*blurStyleRow_=nullptr,*stepRow_=nullptr;
 QToolButton *blurStyle_=nullptr,*pixelateStyle_=nullptr;
 QLabel* stepLabel_=nullptr;QSpinBox* stepNumber_=nullptr;QPointer<QThread> copyThread_;bool syncingStep_=false;int stepPixels_=editor::defaultStepPixels;quint64 copyFeedback_=0;
 QComboBox* width_=nullptr;
 QSlider* strength_=nullptr;
 Ui::ElidedLabel* saveTarget_=nullptr;
 QSpinBox* strengthValue_=nullptr;
 Ui::ElidedLabel* toolTitle_=nullptr;
 QLabel *hint_=nullptr,*message_=nullptr,*toolHelp_=nullptr,*documentInfo_=nullptr;
 Ui::ElidedLabel* destination_=nullptr;QLabel* sharingMode_=nullptr;QWidget* blurWarning_=nullptr;
 QLabel *geometryTitle_=nullptr,*geometryLabels_[4]={};Ui::ElidedLabel *savedLabel_=nullptr,*saveError_=nullptr,*folderError_=nullptr;
 QSpinBox* geometry_[4]={};
 QToolButton* geometryDisclosure_=nullptr;
 QWidget *geometryFields_=nullptr,*geometryRow_=nullptr,*placeRow_=nullptr,*sizeLabels_=nullptr,*optionsRule_=nullptr;
 bool syncingGeometry_=false;
 Ui::WindowHeader* header_=nullptr;
 QPushButton *copyImage_=nullptr;QJsonObject lastCopy_;
 QGridLayout* toolbarGrid_=nullptr;QBoxLayout *toolsLayout_=nullptr,*actionsLayout_=nullptr;QWidget* actionsSeparator_=nullptr;bool toolbarWrapped_=false;
 QPushButton *upload_=nullptr,*save_=nullptr,*cancel_=nullptr,*place_=nullptr,*showFolder_=nullptr,*copyPath_=nullptr,*chooseAnother_=nullptr,*remove_=nullptr;
 QSplitter* splitter_=nullptr;int propertiesWidth_=240;
 QWidget *canvasViewport_=nullptr,*side_=nullptr,*document_=nullptr;QScrollArea* propertiesScroll_=nullptr;
 QVBoxLayout *properties_=nullptr;
 // Feedback overlays the canvas without changing its geometry or zoom.
 Ui::Banner* saveFeedback_=nullptr;QTimer* saveFeedbackTimer_=nullptr;bool saveFeedbackVisible_=false;
 QWidget* notice_=nullptr;QGridLayout* footerGrid_=nullptr;
 QLabel* noticeText_=nullptr;QPushButton *keepEditing_=nullptr,*saveInstead_=nullptr;
 bool destinationAbove_=false;
 QPointer<QDialog> discard_;
 FileDialog* chooser_=nullptr;
 // The last Save a copy outcome and Show in folder result, for editor.view,
 // and the document generation the saved copy holds.
 QJsonObject lastSave_,lastShowInFolder_;QString lastCopiedPath_;quint64 savedGeneration_=0,folderRequest_=0;
 QColor annotationColor_=QColor(220,38,38),highlightColor_=QColor(250,204,21);
 bool closing_=false,busy_=false,shown_=false,savingCopy_=false,copying_=false;
 // Copy text: state, Copy selection and Copy all text, the note, codes, the cut line.
 EditorTextRecognition* text_=nullptr;TextCopier textCopier_;
 QWidget *copyTextRow_=nullptr,*codeList_=nullptr;QVBoxLayout* codeLayout_=nullptr;quint64 codesShown_=~quint64(0);
 QLabel *textState_=nullptr,*textTruncated_=nullptr,*textFeedback_=nullptr,*codesTitle_=nullptr;QPushButton *copySelection_=nullptr,*copyAllText_=nullptr;
 void syncText();void rebuildCodes();void textOutcome(const QString& what,Error error,int characters,TextDone done);
 QWidget* quickRedactRow_=nullptr;QToolButton* quickRedact_=nullptr;QLabel* quickRedactResult_=nullptr;bool quickRedacting_=false;
};
}
