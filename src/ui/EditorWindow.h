#pragma once
#include "editor/EditorCanvas.h"
#include <QHash>
#include <QWidget>
class QLabel;class QPushButton;class QToolButton;class QComboBox;class QSlider;class QButtonGroup;

namespace putgd {
// One themed review window for one image. It owns no storage, credentials or
// history; the hosting helper turns its explicit Upload into a private export.
class EditorWindow final:public QWidget {
 Q_OBJECT
public:
 EditorWindow(editor::EditDocument&,const QString& destination,QWidget* parent=nullptr);
 editor::EditorCanvas& canvas() {return *canvas_;}
 void setBusy(const QString& message);
 void setFailure(const QString& message);
 // Asks before discarding edits; emits cancelRequested when the user agrees.
 void requestCancel();
 void allowClose() {closing_=true;}
 void selectTool(editor::Tool);
 // Agent commands change the document directly; refresh selection, tiles and controls.
 void documentEdited();
signals:
 void uploadRequested();
 void cancelRequested();
protected:
 void closeEvent(QCloseEvent*) override;
 void showEvent(QShowEvent*) override;
private:
 void refresh();
 void syncStyle();
 void saveCopy();
 editor::EditDocument& doc_;
 editor::EditorCanvas* canvas_=nullptr;
 QButtonGroup* tools_=nullptr;
 QHash<int,QToolButton*> toolButtons_;
 QList<QToolButton*> swatches_;
 QToolButton *undo_=nullptr,*redo_=nullptr;
 QWidget *colorRow_=nullptr,*widthRow_=nullptr,*blurRow_=nullptr,*textRow_=nullptr,*cropRow_=nullptr;
 QComboBox *width_=nullptr,*textSize_=nullptr;
 QSlider* strength_=nullptr;
 QLabel *hint_=nullptr,*status_=nullptr,*message_=nullptr;
 QPushButton *upload_=nullptr,*save_=nullptr,*cancel_=nullptr;
 QColor annotationColor_=QColor(220,38,38),highlightColor_=QColor(250,204,21);
 bool closing_=false,busy_=false,shown_=false;
};
}
