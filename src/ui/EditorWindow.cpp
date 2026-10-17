#include "EditorWindow.h"
#include "RedesignTheme.h"
#include "VisualStyle.h"
#include "core/Settings.h"
#include <QButtonGroup>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QFontComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScreen>
#include <QShortcut>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QToolButton>
#include <QVBoxLayout>

namespace putgd {
using editor::Tool;
namespace {
const QString iconColor("#9bbcff");
// Scoped additions to the redesign theme for the editor's compact controls.
const char* editorStyle=R"QSS(
QWidget#editorToolbar, QWidget#editorOptions, QWidget#editorFooter { background: #181B21; }
QWidget#editorToolbar { border-bottom: 1px solid #2A2F38; }
QWidget#editorOptions { border-bottom: 1px solid #2A2F38; }
QWidget#editorFooter { border-top: 1px solid #2A2F38; }
QToolButton[visualRole="tool"] { padding: 4px 8px; min-width: 30px; }
QToolButton[visualRole="tool"]:checked { background: #232838; border-color: #6EA8FE; }
QToolButton[visualRole="swatch"] { min-width: 18px; max-width: 18px; min-height: 18px; max-height: 18px; padding: 0; border-radius: 10px; border: 2px solid #343A45; }
QToolButton[visualRole="swatch"]:checked { border: 2px solid #E7E9EE; }
QToolButton[visualRole="swatch"]:focus { border: 2px solid #6EA8FE; padding: 0; }
QToolButton[visualRole="customSwatch"] { min-width: 18px; max-width: 18px; min-height: 18px; max-height: 18px; padding: 0; border-radius: 10px; border: 2px solid #343A45;
 background: qconicalgradient(cx:0.5, cy:0.5, angle:0, stop:0 #ef4444, stop:0.17 #f59e0b, stop:0.33 #facc15, stop:0.5 #22c55e, stop:0.67 #3b82f6, stop:0.83 #a855f7, stop:1 #ef4444); }
QToolButton[visualRole="customSwatch"]:hover, QToolButton[visualRole="customSwatch"]:focus { border: 2px solid #6EA8FE; }
QFrame[visualRole="toolSeparator"] { background: #2A2F38; border: none; min-width: 1px; max-width: 1px; margin: 4px 6px; }
QSlider::groove:horizontal { height: 4px; background: #343A45; border-radius: 2px; }
QSlider::sub-page:horizontal { background: #6EA8FE; border-radius: 2px; }
QSlider::handle:horizontal { background: #E7E9EE; width: 14px; margin: -6px 0; border-radius: 7px; }
QLineEdit#annotationText { background: #20242C; border: 2px solid #6EA8FE; border-radius: 6px; padding: 2px 6px; }
)QSS";
QFrame* separator(QWidget* parent){auto* line=new QFrame(parent);VisualStyle::role(line,"toolSeparator");return line;}
QWidget* row(QWidget* parent,QHBoxLayout*& layout){auto* widget=new QWidget(parent);layout=new QHBoxLayout(widget);layout->setContentsMargins(0,0,0,0);layout->setSpacing(6);return widget;}
}

EditorWindow::EditorWindow(editor::EditDocument& doc,QWidget* parent):QWidget(parent),doc_(doc){
 setObjectName("putgdWindow");setWindowTitle("Edit image — put.gd");setWindowIcon(VisualStyle::icon("brand"));
 setAttribute(Qt::WA_DeleteOnClose,false);
 auto* root=new QVBoxLayout(this);root->setContentsMargins(0,0,0,0);root->setSpacing(0);

 auto* toolbar=new QWidget(this);toolbar->setObjectName("editorToolbar");toolbar->setAttribute(Qt::WA_StyledBackground);
 auto* tools=new QHBoxLayout(toolbar);tools->setContentsMargins(12,8,12,8);tools->setSpacing(4);
 tools_=new QButtonGroup(this);tools_->setExclusive(true);
 struct Entry{Tool tool;const char* icon;const char* label;const char* key;};
 const Entry entries[]={{Tool::Select,"select","Select and move","V"},{Tool::Redact,"redact","Redact — opaque black box","X"},{Tool::Blur,"blur","Blur — visual effect only","B"},{Tool::Crop,"crop","Crop","C"},
  {Tool::Arrow,"annotate-arrow","Arrow","A"},{Tool::Rectangle,"rectangle","Rectangle","R"},{Tool::Highlight,"highlight","Highlight","H"},{Tool::Text,"text","Text","T"}};
 for(const auto& entry:entries){
  auto* button=new QToolButton(toolbar);button->setCheckable(true);button->setIcon(VisualStyle::icon(entry.icon,iconColor));button->setIconSize(QSize(20,20));
  const auto label=QString::fromUtf8(entry.label);button->setToolTip(QString("%1 (%2)").arg(label,entry.key));button->setAccessibleName(label);
  VisualStyle::role(button,"tool");tools_->addButton(button,int(entry.tool));toolButtons_.insert(int(entry.tool),button);tools->addWidget(button);
  auto* shortcut=new QShortcut(QKeySequence(QString::fromLatin1(entry.key)),this);shortcut->setContext(Qt::WindowShortcut);
  connect(shortcut,&QShortcut::activated,this,[this,tool=entry.tool]{if(!busy_&&!canvas_->editingText())selectTool(tool);});
  if(entry.tool==Tool::Crop)tools->addWidget(separator(toolbar));
 }
 tools->addWidget(separator(toolbar));
 auto toolButton=[&](const char* icon,const QString& tip){auto* button=new QToolButton(toolbar);button->setIcon(VisualStyle::icon(icon,iconColor));button->setIconSize(QSize(20,20));button->setToolTip(tip);button->setAccessibleName(tip);VisualStyle::role(button,"tool");tools->addWidget(button);return button;};
 undo_=toolButton("undo","Undo (Ctrl+Z)");redo_=toolButton("redo","Redo (Ctrl+Shift+Z)");
 tools->addWidget(separator(toolbar));
 auto* fit=toolButton("zoom-fit","Fit to window (Ctrl+0)");auto* actual=toolButton("zoom-actual","Actual size, 100% (Ctrl+1)");
 tools->addStretch(1);
 status_=new QLabel(toolbar);VisualStyle::role(status_,"muted");tools->addWidget(status_);
 root->addWidget(toolbar);

 auto* options=new QWidget(this);options->setObjectName("editorOptions");options->setAttribute(Qt::WA_StyledBackground);
 auto* optionLayout=new QHBoxLayout(options);optionLayout->setContentsMargins(12,6,12,6);optionLayout->setSpacing(14);
 QHBoxLayout* layout=nullptr;
 colorRow_=row(options,layout);auto* colorLabel=new QLabel("Color",colorRow_);VisualStyle::role(colorLabel,"muted");layout->addWidget(colorLabel);
 swatchGroup_=new QButtonGroup(this);
 for(const auto& color:{QColor(220,38,38),QColor(245,158,11),QColor(250,204,21),QColor(34,197,94),QColor(59,130,246),QColor(255,255,255),QColor(17,19,23)}){
  auto* swatch=makeSwatch(color);swatches_.append(swatch);layout->addWidget(swatch);
 }
 // Recently picked custom colors sit between the palette and the picker.
 layout->addWidget(separator(colorRow_));
 auto* recent=new QWidget(colorRow_);recentLayout_=new QHBoxLayout(recent);recentLayout_->setContentsMargins(0,0,0,0);recentLayout_->setSpacing(6);layout->addWidget(recent);
 customColor_=new QToolButton(colorRow_);VisualStyle::role(customColor_,"customSwatch");customColor_->setObjectName("customColor");
 customColor_->setToolTip("Custom color…");customColor_->setAccessibleName("Custom color");layout->addWidget(customColor_);
 connect(customColor_,&QToolButton::clicked,this,&EditorWindow::pickCustomColor);
 optionLayout->addWidget(colorRow_);
 widthRow_=row(options,layout);auto* widthLabel=new QLabel("Line",widthRow_);VisualStyle::role(widthLabel,"muted");layout->addWidget(widthLabel);
 width_=VisualStyle::choiceBox(widthRow_);width_->addItem("Thin",2);width_->addItem("Medium",4);width_->addItem("Thick",8);width_->setCurrentIndex(1);width_->setAccessibleName("Line width");layout->addWidget(width_);
 optionLayout->addWidget(widthRow_);
 blurRow_=row(options,layout);auto* blurLabel=new QLabel("Strength",blurRow_);VisualStyle::role(blurLabel,"muted");layout->addWidget(blurLabel);
 strength_=new QSlider(Qt::Horizontal,blurRow_);strength_->setRange(editor::EditDocument::minimumBlur,editor::EditDocument::maximumBlur);strength_->setValue(12);strength_->setFixedWidth(140);strength_->setAccessibleName("Blur strength");layout->addWidget(strength_);
 optionLayout->addWidget(blurRow_);
 textRow_=row(options,layout);auto* fontLabel=new QLabel("Font",textRow_);VisualStyle::role(fontLabel,"muted");layout->addWidget(fontLabel);
 font_=new QFontComboBox(textRow_);font_->setObjectName("textFont");font_->setAccessibleName("Font");font_->setCurrentFont(QFont());font_->setMaximumWidth(220);layout->addWidget(font_);
 auto* sizeLabel=new QLabel("Size",textRow_);VisualStyle::role(sizeLabel,"muted");layout->addWidget(sizeLabel);
 // Image pixels; applied when typing finishes (Enter or leaving the field) or on each step.
 textSize_=new QSpinBox(textRow_);textSize_->setObjectName("textSize");textSize_->setRange(editor::minimumFontPixels,editor::maximumFontPixels);textSize_->setValue(28);textSize_->setSuffix(" px");
 textSize_->setKeyboardTracking(false);textSize_->setAccessibleName("Text size in image pixels");layout->addWidget(textSize_);
 optionLayout->addWidget(textRow_);
 cropRow_=row(options,layout);
 auto* cropHint=new QLabel("Drag to choose the area to keep.",cropRow_);VisualStyle::role(cropHint,"secondary");layout->addWidget(cropHint);
 auto* applyCrop=new QPushButton("Apply crop",cropRow_);applyCrop->setToolTip("Apply crop (Enter)");layout->addWidget(applyCrop);
 auto* cancelCrop=new QPushButton("Cancel",cropRow_);cancelCrop->setToolTip("Cancel crop (Esc)");layout->addWidget(cancelCrop);
 optionLayout->addWidget(cropRow_);
 hint_=new QLabel(options);VisualStyle::role(hint_,"warning");optionLayout->addWidget(hint_);
 optionLayout->addStretch(1);
 root->addWidget(options);

 canvas_=new editor::EditorCanvas(doc_,this);root->addWidget(canvas_,1);

 auto* footer=new QWidget(this);footer->setObjectName("editorFooter");footer->setAttribute(Qt::WA_StyledBackground);
 auto* actions=new QHBoxLayout(footer);actions->setContentsMargins(12,10,12,10);actions->setSpacing(8);
 message_=new QLabel(footer);message_->hide();actions->addWidget(message_,1);
 actions->addStretch(1);
 save_=new QPushButton("Save a copy…",footer);save_->setIcon(VisualStyle::icon("save",iconColor));save_->setToolTip("Save the edited image to a file (Ctrl+S)");actions->addWidget(save_);
 cancel_=new QPushButton("Cancel",footer);cancel_->setToolTip("Discard this edit without uploading");actions->addWidget(cancel_);
 upload_=new QPushButton("Upload",footer);upload_->setIcon(VisualStyle::icon("upload","#111317"));VisualStyle::role(upload_,"primary");upload_->setToolTip("Upload the edited image (Ctrl+Enter)");upload_->setAutoDefault(false);actions->addWidget(upload_);
 root->addWidget(footer);

 RedesignTheme::apply(*this);setStyleSheet(styleSheet()+QString::fromLatin1(editorStyle));
 // Every option row is still visible here. Reserve the tallest height so tool and
 // selection changes never resize, refit and shift the canvas under the pointer.
 options->ensurePolished();options->setFixedHeight(options->sizeHint().height());

 connect(tools_,&QButtonGroup::idClicked,this,[this](int id){selectTool(Tool(id));});
 connect(canvas_,&editor::EditorCanvas::toolRequested,this,[this](Tool tool){if(auto* button=toolButtons_.value(int(tool)))button->setChecked(true);refresh();});
 connect(canvas_,&editor::EditorCanvas::changed,this,&EditorWindow::refresh);
 connect(canvas_,&editor::EditorCanvas::viewChanged,this,&EditorWindow::refresh);
 connect(canvas_,&editor::EditorCanvas::selectionChanged,this,[this]{
  // A selected annotation shows, and is edited by, its own style controls.
  if(const auto* shape=doc_.find(canvas_->selected())){
   if(shape->kind==editor::Shape::Kind::Highlight)highlightColor_=shape->color;else if(shape->kind!=editor::Shape::Kind::Redact&&shape->kind!=editor::Shape::Kind::Blur)annotationColor_=shape->color;
   width_->blockSignals(true);textSize_->blockSignals(true);font_->blockSignals(true);strength_->blockSignals(true);
   width_->setCurrentIndex(std::max(0,width_->findData(shape->width)));textSize_->setValue(shape->fontPixels);strength_->setValue(shape->strength);
   if(shape->kind==editor::Shape::Kind::Text)font_->setCurrentFont(shape->fontFamily.isEmpty()?QFont():QFont(shape->fontFamily));
   width_->blockSignals(false);textSize_->blockSignals(false);font_->blockSignals(false);strength_->blockSignals(false);
   syncStyle(false);return;
  }
  refresh();
 });
 // Only a change to a style control edits the selected shape.
 connect(width_,&QComboBox::currentIndexChanged,this,[this]{syncStyle(true);});
 connect(textSize_,&QSpinBox::valueChanged,this,[this]{syncStyle(true);});
 connect(font_,&QFontComboBox::currentFontChanged,this,[this]{syncStyle(true);});
 connect(strength_,&QSlider::sliderReleased,this,[this]{syncStyle(true);});
 connect(strength_,&QSlider::valueChanged,this,[this]{if(!strength_->isSliderDown())syncStyle(true);});
 connect(applyCrop,&QPushButton::clicked,this,[this]{canvas_->applyCrop();});
 connect(cancelCrop,&QPushButton::clicked,this,[this]{canvas_->cancelCrop();});
 auto undo=[this]{if(busy_||canvas_->editingText())return;if(doc_.undo()){canvas_->documentChanged();refresh();}};
 auto redo=[this]{if(busy_||canvas_->editingText())return;if(doc_.redo()){canvas_->documentChanged();refresh();}};
 connect(undo_,&QToolButton::clicked,this,undo);connect(redo_,&QToolButton::clicked,this,redo);
 connect(fit,&QToolButton::clicked,canvas_,&editor::EditorCanvas::fit);
 connect(actual,&QToolButton::clicked,canvas_,&editor::EditorCanvas::actualSize);
 // Platform bindings and explicit aliases can coincide; identical QShortcuts
 // would be ambiguous and neither would fire, so register each key once.
 QList<QKeySequence> bound;
 auto bind=[this,&bound](QList<QKeySequence> keys,auto slot){
  for(const auto& key:keys){if(key.isEmpty()||bound.contains(key))continue;bound.append(key);auto* shortcut=new QShortcut(key,this);shortcut->setContext(Qt::WindowShortcut);connect(shortcut,&QShortcut::activated,this,slot);}
 };
 auto standard=[](QKeySequence::StandardKey key,QList<QKeySequence> extra={}){return QKeySequence::keyBindings(key)+extra;};
 bind(standard(QKeySequence::Undo,{QKeySequence("Ctrl+Z")}),undo);bind(standard(QKeySequence::Redo,{QKeySequence("Ctrl+Shift+Z"),QKeySequence("Ctrl+Y")}),redo);
 bind({QKeySequence("Ctrl+0")},[this]{canvas_->fit();});bind({QKeySequence("Ctrl+1")},[this]{canvas_->actualSize();});
 bind(standard(QKeySequence::ZoomIn,{QKeySequence("Ctrl+=")}),[this]{canvas_->zoomBy(1.25);});bind(standard(QKeySequence::ZoomOut),[this]{canvas_->zoomBy(0.8);});
 bind(standard(QKeySequence::Save),[this]{if(!busy_)saveCopy();});
 // Upload is never plain Enter, which completes text entry and crops.
 auto submit=[this]{if(busy_)return;if(canvas_->editingText())canvas_->finishText(true);if(canvas_->cropPending())canvas_->applyCrop();emit uploadRequested();};
 bind({QKeySequence("Ctrl+Return"),QKeySequence("Ctrl+Enter")},submit);
 connect(upload_,&QPushButton::clicked,this,submit);
 connect(save_,&QPushButton::clicked,this,&EditorWindow::saveCopy);
 connect(cancel_,&QPushButton::clicked,this,&EditorWindow::requestCancel);

 resize(initialSize(doc_.size(),screen()?screen()->availableGeometry().size():QSize(1280,800)));
 selectTool(Tool::Redact);
}

QSize EditorWindow::initialSize(const QSize& image,const QSize& available){
 // Prefer at least 900x620, but never exceed 90% of a smaller screen.
 const QSize wanted(image.width()+64,image.height()+150);
 const int maximumWidth=std::max(320,int(available.width()*0.9)),maximumHeight=std::max(240,int(available.height()*0.9));
 return {std::min(std::max(wanted.width(),900),maximumWidth),std::min(std::max(wanted.height(),620),maximumHeight)};
}

void EditorWindow::selectTool(Tool tool){
 if(auto* button=toolButtons_.value(int(tool)))button->setChecked(true);
 canvas_->setTool(tool);syncStyle(false);refresh();canvas_->setFocus();
}

QToolButton* EditorWindow::makeSwatch(const QColor& color){
 auto* swatch=new QToolButton(colorRow_);swatch->setCheckable(true);VisualStyle::role(swatch,"swatch");
 swatch->setStyleSheet(QString("QToolButton { background: %1; }").arg(color.name()));swatch->setProperty("swatchColor",color);swatch->setAccessibleName("Color "+color.name());swatch->setToolTip(color.name());
 swatchGroup_->addButton(swatch);connect(swatch,&QToolButton::clicked,this,[this,color]{useColor(color);});return swatch;
}

void EditorWindow::useColor(const QColor& color){
 const auto* shape=doc_.find(canvas_->selected());
 if(canvas_->tool()==Tool::Highlight||(shape&&shape->kind==editor::Shape::Kind::Highlight))highlightColor_=color;else annotationColor_=color;
 syncStyle(true);
}

void EditorWindow::pickCustomColor(){
 const auto* shape=doc_.find(canvas_->selected());
 const bool highlight=canvas_->tool()==Tool::Highlight||(shape&&shape->kind==editor::Shape::Kind::Highlight);
 // The dialog's own custom-color grid shows the same recent picks.
 for(int i=0;i<QColorDialog::customCount();++i)QColorDialog::setCustomColor(i,i<recentColors_.size()?QColor(recentColors_[i]):QColor(Qt::white));
 const auto color=QColorDialog::getColor(highlight?highlightColor_:annotationColor_,this,"Custom color");
 if(!color.isValid())return;
 // Palette colors are already one click away; only custom colors are remembered.
 bool palette=false;for(auto* swatch:swatches_)palette=palette||swatch->property("swatchColor").value<QColor>()==color;
 if(!palette){recentColors_.removeAll(color.name());recentColors_.prepend(color.name());while(recentColors_.size()>Settings::maximumRecentColors)recentColors_.removeLast();rebuildRecentColors();}
 useColor(color);
}

void EditorWindow::setRecentColors(const QStringList& colors){
 static const QRegularExpression hex("^#[0-9a-f]{6}$");
 recentColors_.clear();
 for(const auto& color:colors)if(hex.match(color).hasMatch()&&!recentColors_.contains(color)&&recentColors_.size()<Settings::maximumRecentColors)recentColors_.append(color);
 rebuildRecentColors();syncStyle(false);
}

void EditorWindow::rebuildRecentColors(){
 for(auto* swatch:std::exchange(recentSwatches_,{})){swatchGroup_->removeButton(swatch);swatch->deleteLater();}
 for(const auto& name:recentColors_){auto* swatch=makeSwatch(QColor(name));swatch->setToolTip("Recent: "+name);recentSwatches_.append(swatch);recentLayout_->addWidget(swatch);}
}

void EditorWindow::documentEdited(){canvas_->documentChanged();refresh();}

void EditorWindow::syncStyle(bool applyToSelection){
 editor::ToolStyle style;
 const auto* shape=doc_.find(canvas_->selected());
 const bool highlight=canvas_->tool()==Tool::Highlight||(shape&&shape->kind==editor::Shape::Kind::Highlight);
 style.color=highlight?highlightColor_:annotationColor_;style.width=width_->currentData().toInt();style.fontPixels=textSize_->value();style.fontFamily=font_->currentFont().family();style.strength=strength_->value();
 if(shape&&shape->kind==editor::Shape::Kind::Redact)style.color=Qt::black;
 canvas_->setStyle(style);if(applyToSelection)canvas_->applyStyleToSelection();
 // An exclusive group cannot uncheck everything, which a color off the palette needs.
 swatchGroup_->setExclusive(false);
 for(auto* swatch:swatches_+recentSwatches_){swatch->blockSignals(true);swatch->setChecked(swatch->property("swatchColor").value<QColor>()==style.color);swatch->blockSignals(false);}
 swatchGroup_->setExclusive(true);
 refresh();
}

void EditorWindow::refresh(){
 const auto tool=canvas_->tool();const auto* shape=doc_.find(canvas_->selected());
 auto kind=[&](editor::Shape::Kind k){return shape&&shape->kind==k;};
 const bool annotation=tool==Tool::Arrow||tool==Tool::Rectangle||tool==Tool::Text||tool==Tool::Highlight||kind(editor::Shape::Kind::Arrow)||kind(editor::Shape::Kind::Rectangle)||kind(editor::Shape::Kind::Text)||kind(editor::Shape::Kind::Highlight);
 colorRow_->setVisible(annotation&&tool!=Tool::Crop);
 widthRow_->setVisible(tool!=Tool::Crop&&(tool==Tool::Arrow||tool==Tool::Rectangle||kind(editor::Shape::Kind::Arrow)||kind(editor::Shape::Kind::Rectangle)));
 blurRow_->setVisible(tool!=Tool::Crop&&(tool==Tool::Blur||kind(editor::Shape::Kind::Blur)));
 textRow_->setVisible(tool!=Tool::Crop&&(tool==Tool::Text||kind(editor::Shape::Kind::Text)));
 cropRow_->setVisible(tool==Tool::Crop);
 if(tool==Tool::Blur||kind(editor::Shape::Kind::Blur))hint_->setText("Blur is a visual effect. Use Redact for passwords and private information.");
 else if(tool==Tool::Redact)hint_->setText({});
 else hint_->setText({});
 if(tool==Tool::Redact&&!shape){hint_->setText("Redact removes the covered pixels from the uploaded image.");VisualStyle::role(hint_,"secondary");}
 else VisualStyle::role(hint_,"warning");
 hint_->setVisible(!hint_->text().isEmpty());
 undo_->setEnabled(!busy_&&doc_.canUndo());redo_->setEnabled(!busy_&&doc_.canRedo());
 const auto crop=doc_.crop();
 status_->setText(QString("%1 × %2 px · %3%").arg(crop.width()).arg(crop.height()).arg(int(std::lround(canvas_->zoom()*100))));
}

void EditorWindow::saveCopy(){
 if(busy_)return;
 if(canvas_->editingText())canvas_->finishText(true);
 auto folder=QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);if(folder.isEmpty())folder=QDir::homePath();
 const auto suggested=QDir(folder).filePath("put.gd-edit-"+QDateTime::currentDateTime().toString("yyyy-MM-dd-HH-mm-ss")+".png");
 const auto path=QFileDialog::getSaveFileName(this,"Save a copy",suggested,"PNG image (*.png)");
 if(path.isEmpty())return;
 // The same sanitized export as Upload; QSaveFile replaces a chosen target atomically.
 const auto png=doc_.exportPng();
 QSaveFile file(path);file.setDirectWriteFallback(false);
 if(png.isEmpty()||!file.open(QIODevice::WriteOnly)||file.write(png)!=png.size()||!file.commit()){setFailure("The copy could not be saved.");return;}
 message_->setText("Saved a copy.");VisualStyle::role(message_,"success");message_->show();
}

void EditorWindow::setBusy(const QString& message){
 busy_=true;for(auto* widget:findChildren<QWidget*>())if(widget!=message_&&widget!=status_)widget->setEnabled(widget==this);
 message_->setText(message);VisualStyle::role(message_,"secondary");message_->show();setCursor(Qt::BusyCursor);
}
void EditorWindow::setFailure(const QString& message){message_->setText(message);VisualStyle::role(message_,"error");message_->show();}

void EditorWindow::requestCancel(){
 if(busy_)return;
 if(doc_.modified()){
  QMessageBox box(QMessageBox::Question,"Discard edits?","Close the editor without uploading? Your edits will be discarded.",QMessageBox::NoButton,this);
  auto* discard=box.addButton("Discard",QMessageBox::DestructiveRole);box.addButton("Keep editing",QMessageBox::RejectRole);
  VisualStyle::role(discard,"danger");box.exec();if(box.clickedButton()!=discard)return;
 }
 emit cancelRequested();
}

void EditorWindow::closeEvent(QCloseEvent* event){
 if(closing_){event->accept();return;}
 event->ignore();requestCancel();
}
void EditorWindow::showEvent(QShowEvent* event){
 QWidget::showEvent(event);if(!shown_){shown_=true;canvas_->fit();canvas_->setFocus();}
}
}
