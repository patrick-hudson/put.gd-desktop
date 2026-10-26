#pragma once
#include "editor/EditorCanvas.h"
#include "Graphite.h"
#include <QHash>
#include <QJsonObject>
#include <QWidget>
class QLabel;class QPushButton;class QToolButton;class QComboBox;class QSlider;class QButtonGroup;class QFontComboBox;class QSpinBox;class QHBoxLayout;

namespace putgd {
class FileDialog;
// One themed review window for one image. It owns no storage, credentials or
// history; the hosting helper turns its explicit Upload into a private export.
class EditorWindow final:public QWidget {
 Q_OBJECT
public:
 EditorWindow(editor::EditDocument&,QWidget* parent=nullptr);
 editor::EditorCanvas& canvas() {return *canvas_;}
 void setBusy(const QString& message);
 void setFailure(const QString& message);
 // Asks before discarding edits; emits cancelRequested when the user agrees.
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
 // A copy written by the window or by editor.saveCopy: its name and Show in
 // folder beside the Document details, the full path in the tooltip,
 // accessible description and context menu. A failure keeps the draft and
 // offers Choose another location….
 void showSaved(const QString& path);
 void showSaveFailed(const QString& path,const QString& reason);
 // Show in folder for the last saved copy (the host's file manager or portal).
 bool showSavedCopy();
 // editor.view: the tool, selection, crop draft, zoom and pan as the window
 // shows them, and one control per call through the same methods as the
 // window's own controls (see ImageEditor.cpp for the actions).
 QJsonObject viewState() const;
 QJsonObject viewCommand(const QJsonObject& params,QString& code,QString& message);
signals:
 void uploadRequested();
 void cancelRequested();
protected:
 void closeEvent(QCloseEvent*) override;
 void showEvent(QShowEvent*) override;
private:
 void refresh();
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
 QToolButton *undo_=nullptr,*redo_=nullptr;
 QWidget *colorRow_=nullptr,*widthRow_=nullptr,*blurRow_=nullptr,*textRow_=nullptr,*cropRow_=nullptr;
 QComboBox* width_=nullptr;
 QSlider* strength_=nullptr;
 QLabel *hint_=nullptr,*status_=nullptr,*message_=nullptr,*toolTitle_=nullptr,*toolHelp_=nullptr,*documentInfo_=nullptr,*destination_=nullptr;
 QLabel *geometryTitle_=nullptr,*geometryLabels_[4]={},*savedLabel_=nullptr,*saveError_=nullptr;
 QSpinBox* geometry_[4]={};
 QWidget *geometryRow_=nullptr,*placeRow_=nullptr,*sizeLabels_=nullptr;
 bool syncingGeometry_=false;
 Graphite::WindowHeader* header_=nullptr;
 QPushButton *upload_=nullptr,*save_=nullptr,*cancel_=nullptr,*place_=nullptr,*showFolder_=nullptr,*chooseAnother_=nullptr;
 FileDialog* chooser_=nullptr;
 // The last Save a copy outcome and Show in folder result, for editor.view.
 QJsonObject lastSave_,lastShowInFolder_;
 QColor annotationColor_=QColor(220,38,38),highlightColor_=QColor(250,204,21);
 bool closing_=false,busy_=false,shown_=false;
};
}
