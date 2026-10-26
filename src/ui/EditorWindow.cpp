#include "EditorWindow.h"
#include "FileDialog.h"
#include "Theme.h"
#include "RedesignTheme.h"
#include "VisualStyle.h"
#include "Graphite.h"
#include <QGridLayout>
#include <QPainter>
#include <QScrollArea>
#include "core/Settings.h"
#include <QButtonGroup>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QFontComboBox>
#include <QDateTime>
#include <QDir>
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
QFrame* separator(QWidget* parent){auto* line=Graphite::rule(parent,Qt::Vertical);line->setFixedHeight(22);return line;}
// A colour choice: its colour in a rounded square, a ring when chosen, the
// accent ring under keyboard focus. The custom picker shows a colour wheel.
class Swatch final:public QToolButton {
public:
 Swatch(const QColor& color,QWidget* parent,bool custom=false):QToolButton(parent),color_(color),custom_(custom){setFixedSize(30,30);setFocusPolicy(Qt::StrongFocus);setCursor(Qt::PointingHandCursor);}
protected:
 void paintEvent(QPaintEvent*)override{
  QPainter p(this);p.setRenderHint(QPainter::Antialiasing);const auto& t=Theme::current();const QRectF box=QRectF(rect()).adjusted(4,4,-4,-4);
  if(custom_){QConicalGradient wheel(box.center(),0);const QList<QColor> stops{QColor("#ef4444"),QColor("#f59e0b"),QColor("#facc15"),QColor("#22c55e"),QColor("#3b82f6"),QColor("#a855f7"),QColor("#ef4444")};for(int i=0;i<stops.size();++i)wheel.setColorAt(i/6.0,stops[i]);p.setBrush(wheel);}else p.setBrush(color_);
  p.setPen(QPen(Graphite::mix(t.borderStrong,t.surface,0.2),1));p.drawRoundedRect(box,Graphite::Metric::swatchRadius,Graphite::Metric::swatchRadius);
  if(isChecked()){p.setPen(QPen(t.text,2));p.setBrush(Qt::NoBrush);p.drawRoundedRect(QRectF(rect()).adjusted(1,1,-1,-1),7,7);}
  else if(underMouse()){p.setPen(QPen(t.borderStrong,1));p.setBrush(Qt::NoBrush);p.drawRoundedRect(QRectF(rect()).adjusted(1.5,1.5,-1.5,-1.5),7,7);}
  if(hasFocus()&&window()->testAttribute(Qt::WA_KeyboardFocusChange)){p.setPen(QPen(t.accent,1.5));p.setBrush(Qt::NoBrush);p.drawRoundedRect(QRectF(rect()).adjusted(0.75,0.75,-0.75,-0.75),7,7);}
 }
private:QColor color_;bool custom_;
};
QLabel* fieldLabel(const QString& text,QWidget* parent){auto* label=new QLabel(text,parent);VisualStyle::role(label,"meta");return label;}
QWidget* column(QWidget* parent,QVBoxLayout*& layout){auto* widget=new QWidget(parent);layout=new QVBoxLayout(widget);layout->setContentsMargins(0,0,0,0);layout->setSpacing(8);return widget;}
}

EditorWindow::EditorWindow(editor::EditDocument& doc,QWidget* parent):QWidget(parent),doc_(doc){
 setObjectName("putgdWindow");setWindowTitle("Edit image — put.gd");setWindowIcon(VisualStyle::icon("brand"));
 setAttribute(Qt::WA_DeleteOnClose,false);
 chooser_=new FileDialog(this);
 auto* root=new QVBoxLayout(this);root->setContentsMargins(0,0,0,0);root->setSpacing(0);
 // The document header: the truthful draft state, not a claim about the source.
 header_=Graphite::WindowHeader::install(this,root);header_->setDocument("Edit image","Not uploaded");

 auto* toolbar=new Graphite::Surface("bg",Qt::BottomEdge,this);toolbar->setObjectName("editorToolbar");
 auto* tools=new QHBoxLayout(toolbar);tools->setContentsMargins(12,8,12,8);tools->setSpacing(4);
 tools_=new QButtonGroup(this);tools_->setExclusive(true);
 struct Entry{Tool tool;const char* icon;const char* label;const char* key;};
 const Entry entries[]={{Tool::Select,"select","Select and move","V"},{Tool::Redact,"redact","Redact — opaque black box","X"},{Tool::Blur,"blur","Blur — visual effect only","B"},{Tool::Crop,"crop","Crop","C"},
  {Tool::Arrow,"annotate-arrow","Arrow","A"},{Tool::Rectangle,"rectangle","Rectangle","R"},{Tool::Highlight,"highlight","Highlight","H"},{Tool::Text,"text","Text","T"}};
 for(const auto& entry:entries){
  auto* button=new QToolButton(toolbar);button->setCheckable(true);button->setIcon(VisualStyle::icon(entry.icon));button->setIconSize(QSize(20,20));button->setFixedSize(36,36);button->setFocusPolicy(Qt::StrongFocus);
  const auto label=QString::fromUtf8(entry.label);button->setToolTip(QString("%1 (%2)").arg(label,entry.key));button->setAccessibleName(label);
  VisualStyle::role(button,"quiet");tools_->addButton(button,int(entry.tool));toolButtons_.insert(int(entry.tool),button);tools->addWidget(button);
  auto* shortcut=new QShortcut(QKeySequence(QString::fromLatin1(entry.key)),this);shortcut->setContext(Qt::WindowShortcut);
  connect(shortcut,&QShortcut::activated,this,[this,tool=entry.tool]{if(!busy_&&!canvas_->editingText())selectTool(tool);});
  if(entry.tool==Tool::Crop){tools->addSpacing(4);tools->addWidget(separator(toolbar));tools->addSpacing(4);}
 }
 tools->addStretch(1);
 auto toolButton=[&](const char* icon,const QString& tip){auto* button=new QToolButton(toolbar);button->setIcon(VisualStyle::icon(icon));button->setIconSize(QSize(20,20));button->setFixedSize(36,36);button->setFocusPolicy(Qt::StrongFocus);button->setToolTip(tip);button->setAccessibleName(tip);VisualStyle::role(button,"quiet");tools->addWidget(button);return button;};
 undo_=toolButton("undo","Undo (Ctrl+Z)");redo_=toolButton("redo","Redo (Ctrl+Shift+Z)");
 tools->addSpacing(4);tools->addWidget(separator(toolbar));tools->addSpacing(4);
 auto* fit=toolButton("zoom-fit","Fit to window (Ctrl+0)");auto* actual=toolButton("zoom-actual","Actual size, 100% (Ctrl+1)");
 tools->addSpacing(8);status_=new QLabel(toolbar);VisualStyle::role(status_,"meta");tools->addWidget(status_);
 root->addWidget(toolbar);

 // The canvas owns the space; the properties panel beside it keeps one width
 // whatever the tool, so the canvas never moves under the pointer.
 auto* body=new QHBoxLayout;body->setContentsMargins(0,0,0,0);body->setSpacing(0);
 canvas_=new editor::EditorCanvas(doc_,this);body->addWidget(canvas_,1);
 auto* panel=new Graphite::Surface("surface",Qt::LeftEdge,this);panel->setObjectName("editorOptions");
 auto* panelScroll=new QScrollArea(this);panelScroll->setObjectName("editorPropertiesScroll");panelScroll->setWidgetResizable(true);panelScroll->setFrameShape(QFrame::NoFrame);panelScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);panelScroll->setWidget(panel);panelScroll->setFixedWidth(Graphite::scaled(256));body->addWidget(panelScroll);
 auto* properties=new QVBoxLayout(panel);properties->setContentsMargins(18,16,16,16);properties->setSpacing(12);
 toolTitle_=new QLabel(panel);toolTitle_->setObjectName("editorToolTitle");VisualStyle::role(toolTitle_,"sectionTitle");properties->addWidget(toolTitle_);
 toolHelp_=new QLabel(panel);toolHelp_->setObjectName("editorToolHelp");toolHelp_->setWordWrap(true);VisualStyle::role(toolHelp_,"muted");properties->addWidget(toolHelp_);
 hint_=new QLabel(panel);hint_->setWordWrap(true);VisualStyle::role(hint_,"warning");properties->addWidget(hint_);
 properties->addWidget(Graphite::rule(panel));
 QVBoxLayout* layout=nullptr;
 colorRow_=column(panel,layout);layout->addWidget(fieldLabel("Color",colorRow_));
 swatchGroup_=new QButtonGroup(this);
 auto* grid=new QGridLayout;grid->setContentsMargins(0,0,0,0);grid->setSpacing(4);int cell=0;
 for(const auto& color:{QColor(220,38,38),QColor(245,158,11),QColor(250,204,21),QColor(34,197,94),QColor(59,130,246),QColor(255,255,255),QColor(17,19,23)}){
  auto* swatch=makeSwatch(color);swatches_.append(swatch);grid->addWidget(swatch,cell/6,cell%6);++cell;
 }
 // The custom picker follows the palette; recent custom colours get their own row.
 customColor_=new Swatch({},colorRow_,true);customColor_->setObjectName("customColor");
 customColor_->setToolTip("Custom color…");customColor_->setAccessibleName("Custom color");grid->addWidget(customColor_,cell/6,cell%6);
 grid->setColumnStretch(6,1);layout->addLayout(grid);
 connect(customColor_,&QToolButton::clicked,this,&EditorWindow::pickCustomColor);
 auto* recent=new QWidget(colorRow_);recentLayout_=new QHBoxLayout(recent);recentLayout_->setContentsMargins(0,0,0,0);recentLayout_->setSpacing(4);recentLayout_->addStretch(1);recent->setObjectName("recentColors");recent->setAccessibleName("Recent custom colors");layout->addWidget(recent);
 properties->addWidget(colorRow_);
 widthRow_=column(panel,layout);layout->addWidget(fieldLabel("Line width",widthRow_));
 width_=VisualStyle::choiceBox(widthRow_);width_->addItem("Thin",2);width_->addItem("Medium",4);width_->addItem("Thick",8);width_->setCurrentIndex(1);width_->setAccessibleName("Line width");layout->addWidget(width_);
 properties->addWidget(widthRow_);
 blurRow_=column(panel,layout);layout->addWidget(fieldLabel("Blur strength",blurRow_));
 strength_=new QSlider(Qt::Horizontal,blurRow_);strength_->setRange(editor::EditDocument::minimumBlur,editor::EditDocument::maximumBlur);strength_->setValue(12);strength_->setAccessibleName("Blur strength");layout->addWidget(strength_);
 properties->addWidget(blurRow_);
 textRow_=column(panel,layout);layout->addWidget(fieldLabel("Font",textRow_));
 font_=new QFontComboBox(textRow_);font_->setObjectName("textFont");font_->setAccessibleName("Font");font_->setCurrentFont(QFont());font_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);font_->setMinimumContentsLength(8);layout->addWidget(font_);
 layout->addWidget(fieldLabel("Size in image pixels",textRow_));
 // Image pixels; applied when typing finishes (Enter or leaving the field) or on each step.
 textSize_=new QSpinBox(textRow_);textSize_->setObjectName("textSize");textSize_->setRange(editor::minimumFontPixels,editor::maximumFontPixels);textSize_->setValue(28);textSize_->setSuffix(" px");
 textSize_->setKeyboardTracking(false);textSize_->setAccessibleName("Text size in image pixels");layout->addWidget(textSize_);
 properties->addWidget(textRow_);
 cropRow_=column(panel,layout);
 auto* applyCrop=new QPushButton("Apply crop",cropRow_);applyCrop->setObjectName("applyCrop");applyCrop->setToolTip("Apply crop (Enter)");layout->addWidget(applyCrop);
 auto* cancelCrop=new QPushButton("Cancel crop",cropRow_);cancelCrop->setObjectName("cancelCrop");cancelCrop->setToolTip("Cancel crop (Esc)");VisualStyle::role(cancelCrop,"quiet");layout->addWidget(cancelCrop);
 properties->addWidget(cropRow_);
 properties->addStretch(1);
 auto* documentHeading=new QLabel("Document",panel);VisualStyle::role(documentHeading,"micro");properties->addWidget(documentHeading);
 documentInfo_=new QLabel(panel);documentInfo_->setObjectName("editorDocumentInfo");documentInfo_->setWordWrap(true);VisualStyle::role(documentInfo_,"secondary");properties->addWidget(documentInfo_);
 root->addLayout(body,1);

 auto* footer=new Graphite::Surface("bg",Qt::TopEdge,this);footer->setObjectName("editorFooter");
 auto* actions=new QHBoxLayout(footer);actions->setContentsMargins(14,10,14,10);actions->setSpacing(8);
 destination_=new Graphite::ElidedLabel(footer);destination_->setObjectName("editorDestination");destination_->setTextFormat(Qt::PlainText);VisualStyle::role(destination_,"secondary");destination_->hide();actions->addWidget(destination_,1);
 message_=new QLabel(footer);message_->setWordWrap(true);message_->hide();actions->addWidget(message_,1);
 actions->addStretch(1);
 save_=new QPushButton("Save a copy…",footer);save_->setIcon(VisualStyle::icon("save"));save_->setToolTip("Save the edited image to a file (Ctrl+S)");actions->addWidget(save_);
 cancel_=new QPushButton("Cancel",footer);cancel_->setToolTip("Discard this edit without uploading");VisualStyle::role(cancel_,"quiet");actions->addWidget(cancel_);
 upload_=new QPushButton("Upload",footer);upload_->setIcon(VisualStyle::icon("upload"));VisualStyle::role(upload_,"primary");upload_->setToolTip("Upload the finished, sanitized image (Ctrl+Enter). The source file is not uploaded.");upload_->setAutoDefault(false);actions->addWidget(upload_);
 root->addWidget(footer);

 RedesignTheme::apply(*this);

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
 auto* swatch=new Swatch(color,colorRow_);swatch->setCheckable(true);
 swatch->setProperty("swatchColor",color);swatch->setAccessibleName("Color "+color.name());swatch->setToolTip(color.name());
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
 for(const auto& name:recentColors_){auto* swatch=makeSwatch(QColor(name));swatch->setToolTip("Recent custom color "+name);recentSwatches_.append(swatch);recentLayout_->insertWidget(recentLayout_->count()-1,swatch);}
 recentLayout_->parentWidget()->setVisible(!recentColors_.isEmpty());
}

void EditorWindow::setDestination(const QString& destination){// The resident phrases it: "Upload to <destination>" or "Save to the local archive".
 static_cast<Graphite::ElidedLabel*>(destination_)->setText(destination);destination_->setVisible(!destination.isEmpty());}

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
 {
  // The panel names the tool (or the selected mark) and how to use it.
  static const QStringList titles{"Select","Redact","Blur","Crop","Arrow","Rectangle","Highlight","Text"};
  static const QStringList help{"Click a mark to select it. Drag to move it; drag its handles to resize.","Drag over private information to cover it with an opaque box.","Drag over an area to blur it.","Drag to choose the area to keep, then apply the crop.","Drag from the tail to the point of the arrow.","Drag to frame a detail with an outline.","Drag across text to highlight it without covering it.","Click the image to place a label. Enter finishes the text."};
  int index=int(tool);QString title=titles.value(index);
  if(shape&&tool==Tool::Select){const QHash<int,QString> kinds{{int(editor::Shape::Kind::Redact),"Redaction"},{int(editor::Shape::Kind::Blur),"Blur"},{int(editor::Shape::Kind::Arrow),"Arrow"},{int(editor::Shape::Kind::Rectangle),"Rectangle"},{int(editor::Shape::Kind::Highlight),"Highlight"},{int(editor::Shape::Kind::Text),"Text"}};title=kinds.value(int(shape->kind),"Mark")+" selected";}
  toolTitle_->setText(title);toolHelp_->setText(help.value(index));
  const auto bounds=doc_.crop();documentInfo_->setText(QString("%1 × %2 px\nPNG output · %3").arg(bounds.width()).arg(bounds.height()).arg(doc_.modified()?"Edited":"Unchanged"));
 }
 undo_->setEnabled(!busy_&&doc_.canUndo());redo_->setEnabled(!busy_&&doc_.canRedo());
 const auto crop=doc_.crop();
 status_->setText(QString("%1 × %2 px · %3%").arg(crop.width()).arg(crop.height()).arg(int(std::lround(canvas_->zoom()*100))));
}

bool EditorWindow::saveCopy(){
 if(busy_||chooser_->isOpen())return false;
 if(canvas_->editingText())canvas_->finishText(true);
 auto folder=QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);if(folder.isEmpty())folder=QDir::homePath();
 const auto name="put.gd-edit-"+QDateTime::currentDateTime().toString("yyyy-MM-dd-HH-mm-ss")+".png";
 return chooser_->open({FileDialog::Mode::Save,"Save a copy",folder,name,{{"PNG image",{"*.png"}}}},[this](const QString& path){
  if(path.isEmpty()||busy_)return;
  // The same sanitized export as Upload; QSaveFile replaces a chosen target atomically.
  const auto png=doc_.exportPng();
  QSaveFile file(path);file.setDirectWriteFallback(false);
  if(png.isEmpty()||!file.open(QIODevice::WriteOnly)||file.write(png)!=png.size()||!file.commit()){setFailure("The copy could not be saved.");return;}
  message_->setText("Saved a copy.");VisualStyle::role(message_,"success");message_->show();
 });
}

void EditorWindow::setBusy(const QString& message){
 chooser_->cancel();busy_=true;for(auto* widget:findChildren<QWidget*>())if(widget!=message_&&widget!=status_)widget->setEnabled(widget==this);
 message_->setText(message);VisualStyle::role(message_,"secondary");message_->show();setCursor(Qt::BusyCursor);
}
void EditorWindow::setFailure(const QString& message){message_->setText(message);VisualStyle::role(message_,"error");message_->show();}

void EditorWindow::requestCancel(){
 if(busy_)return;
 if(doc_.modified()){
  QMessageBox box(QMessageBox::Question,"Discard edits?","Close the editor without uploading? Your edits will be discarded.",QMessageBox::NoButton,this);
  auto* discard=box.addButton("Discard edits",QMessageBox::DestructiveRole);auto* keep=box.addButton("Keep editing",QMessageBox::RejectRole);box.setDefaultButton(keep);box.setEscapeButton(keep);box.setIcon(QMessageBox::NoIcon);
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
