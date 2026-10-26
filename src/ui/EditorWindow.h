#pragma once
#include "editor/EditorCanvas.h"
#include "Graphite.h"
#include <QHash>
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
signals:
 void uploadRequested();
 void cancelRequested();
protected:
 void closeEvent(QCloseEvent*) override;
 void showEvent(QShowEvent*) override;
private:
 void refresh();
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
 Graphite::WindowHeader* header_=nullptr;
 QPushButton *upload_=nullptr,*save_=nullptr,*cancel_=nullptr;
 FileDialog* chooser_=nullptr;
 QColor annotationColor_=QColor(220,38,38),highlightColor_=QColor(250,204,21);
 bool closing_=false,busy_=false,shown_=false;
};
}
