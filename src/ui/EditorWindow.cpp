#include "EditorWindow.h"
#include "FileDialog.h"
#include "Theme.h"
#include "RedesignTheme.h"
#include "VisualStyle.h"
#include "Graphite.h"
#include "platform/HostOpen.h"
#include <QAction>
#include <QClipboard>
#include <QFileInfo>
#include <QGridLayout>
#include <QGuiApplication>
#include <QJsonArray>
#include <cmath>
#include <QPainter>
#include <QScrollArea>
#include <QScrollBar>
#include "core/Settings.h"
#include <QButtonGroup>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QFontComboBox>
#include <QPointer>
#include <QAbstractSpinBox>
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
// The 32 px button is the hit target; the painted square stays 22 px.
class Swatch final:public QToolButton {
public:
 Swatch(const QColor& color,QWidget* parent,bool custom=false):QToolButton(parent),color_(color),custom_(custom){setFixedSize(32,32);setFocusPolicy(Qt::StrongFocus);setCursor(Qt::PointingHandCursor);}
protected:
 void paintEvent(QPaintEvent*)override{
  QPainter p(this);p.setRenderHint(QPainter::Antialiasing);const auto& t=Theme::current();const QRectF box=QRectF(rect()).adjusted(5,5,-5,-5);
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
// Tool names in editor.view, in Tool order.
const QStringList toolNames{"select","redact","blur","crop","arrow","rectangle","highlight","text"};
QJsonValue rectValue(const std::optional<QRect>& r){if(!r)return QJsonValue();return QJsonArray{r->x(),r->y(),r->width(),r->height()};}
bool integerIn(const QJsonValue& value,double minimum,double maximum,int& out){
 if(!value.isDouble())return false;const double number=value.toDouble();
 if(!std::isfinite(number)||std::floor(number)!=number||number<minimum||number>maximum)return false;
 out=int(number);return true;
}
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
 // Only the zoom sits beside the zoom controls; the size is in Document.
 tools->addSpacing(6);status_=new QLabel(toolbar);status_->setObjectName("editorZoom");status_->setMinimumWidth(Graphite::scaled(40));VisualStyle::role(status_,"meta");tools->addWidget(status_);
 root->addWidget(toolbar);

 // The canvas owns the space; the properties panel beside it keeps one width
 // whatever the tool, so the canvas never moves under the pointer.
 auto* body=new QHBoxLayout;body->setContentsMargins(0,0,0,0);body->setSpacing(0);
 canvas_=new editor::EditorCanvas(doc_,this);body->addWidget(canvas_,1);
 // The side column: scrolling tool properties above the Document details,
 // which stay pinned just above the footer's Save a copy… and Upload.
 auto* side=new Graphite::Surface("surface",Qt::LeftEdge,this);side->setObjectName("editorSide");side->setFixedWidth(Graphite::scaled(256));
 auto* sideLayout=new QVBoxLayout(side);sideLayout->setContentsMargins(1,0,0,0);sideLayout->setSpacing(0);
 auto* panel=new Graphite::Surface("surface",{},side);panel->setObjectName("editorOptions");
 auto* panelScroll=new QScrollArea(side);panelScroll->setObjectName("editorPropertiesScroll");panelScroll->setWidgetResizable(true);panelScroll->setFrameShape(QFrame::NoFrame);panelScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);panelScroll->setWidget(panel);sideLayout->addWidget(panelScroll,1);
 body->addWidget(side);
 auto* properties=new QVBoxLayout(panel);properties->setContentsMargins(18,16,16,16);properties->setSpacing(12);
 toolTitle_=new QLabel(panel);toolTitle_->setObjectName("editorToolTitle");VisualStyle::role(toolTitle_,"sectionTitle");properties->addWidget(toolTitle_);
 toolHelp_=new QLabel(panel);toolHelp_->setObjectName("editorToolHelp");toolHelp_->setWordWrap(true);VisualStyle::role(toolHelp_,"muted");properties->addWidget(toolHelp_);
 hint_=new QLabel(panel);hint_->setWordWrap(true);VisualStyle::role(hint_,"warning");properties->addWidget(hint_);
 // Without a pointer: the tool's mark in the middle of the image (Insert).
 placeRow_=new QWidget(panel);auto* placeLayout=new QHBoxLayout(placeRow_);placeLayout->setContentsMargins(0,0,0,0);
 place_=new QPushButton(placeRow_);place_->setObjectName("placeMark");placeLayout->addWidget(place_);placeLayout->addStretch(1);properties->addWidget(placeRow_);
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
 // Position and size of the selected mark or the pending crop, in image
 // pixels; each committed value is one undoable step.
 geometryRow_=column(panel,layout);geometryRow_->setObjectName("editorGeometry");
 geometryTitle_=fieldLabel("Position and size",geometryRow_);layout->addWidget(geometryTitle_);
 auto* fields=new QGridLayout;fields->setContentsMargins(0,0,0,0);fields->setHorizontalSpacing(8);fields->setVerticalSpacing(4);
 const char* names[4]={"geometryX","geometryY","geometryWidth","geometryHeight"};
 for(int i=0;i<4;++i){
  geometryLabels_[i]=fieldLabel({},geometryRow_);
  auto* box=new QSpinBox(geometryRow_);box->setObjectName(names[i]);box->setSuffix(" px");box->setKeyboardTracking(false);box->setMinimumWidth(Graphite::scaled(64));
  geometryLabels_[i]->setBuddy(box);geometry_[i]=box;
  fields->addWidget(geometryLabels_[i],(i/2)*2,i%2);fields->addWidget(box,(i/2)*2+1,i%2);
  connect(box,&QSpinBox::valueChanged,this,&EditorWindow::applyGeometry);
 }
 layout->addLayout(fields);properties->addWidget(geometryRow_);
 properties->addStretch(1);
 auto* document=new Graphite::Surface("surface",Qt::TopEdge,side);document->setObjectName("editorDocument");
 auto* documentLayout=new QVBoxLayout(document);documentLayout->setContentsMargins(18,12,16,14);documentLayout->setSpacing(6);
 documentLayout->addWidget(fieldLabel("Document",document));
 documentInfo_=new QLabel(document);documentInfo_->setObjectName("editorDocumentInfo");documentInfo_->setWordWrap(true);VisualStyle::role(documentInfo_,"secondary");documentLayout->addWidget(documentInfo_);
 // Save a copy… feedback, next to the button that made it.
 savedLabel_=new Graphite::ElidedLabel(document);savedLabel_->setObjectName("editorSaved");savedLabel_->setTextFormat(Qt::PlainText);savedLabel_->setContextMenuPolicy(Qt::ActionsContextMenu);savedLabel_->hide();documentLayout->addWidget(savedLabel_);
 // A failure wraps rather than elides: its reason is what the user needs.
 saveError_=new QLabel(document);saveError_->setObjectName("editorSaveError");saveError_->setTextFormat(Qt::PlainText);saveError_->setWordWrap(true);VisualStyle::role(saveError_,"error");saveError_->hide();documentLayout->addWidget(saveError_);
 auto* copyPath=new QAction("Copy full path",savedLabel_);savedLabel_->addAction(copyPath);
 connect(copyPath,&QAction::triggered,this,[this]{const auto path=lastSave_["path"].toString();if(!path.isEmpty())QGuiApplication::clipboard()->setText(path);});
 auto* savedActions=new QHBoxLayout;savedActions->setContentsMargins(0,0,0,0);savedActions->setSpacing(8);
 showFolder_=new QPushButton("Show in folder",document);showFolder_->setObjectName("showSavedCopy");VisualStyle::role(showFolder_,"quiet");showFolder_->hide();savedActions->addWidget(showFolder_);
 chooseAnother_=new QPushButton("Choose another location…",document);chooseAnother_->setObjectName("chooseAnotherLocation");chooseAnother_->hide();savedActions->addWidget(chooseAnother_);
 savedActions->addStretch(1);documentLayout->addLayout(savedActions);
 sideLayout->addWidget(document);
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
 connect(place_,&QPushButton::clicked,this,[this]{if(!busy_)canvas_->placeMark();});
 connect(showFolder_,&QPushButton::clicked,this,&EditorWindow::showSavedCopy);
 connect(chooseAnother_,&QPushButton::clicked,this,&EditorWindow::saveCopy);
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
  static const QStringList help{"Click a mark to select it, or press Tab. Drag it or use the arrow keys to move it; drag its handles or use Shift+arrow keys to resize.","Drag over private information to cover it with an opaque box.","Drag over an area to blur it.","Drag to choose the area to keep, or set it below, then apply the crop.","Drag from the tail to the point of the arrow.","Drag to frame a detail with an outline.","Drag across text to highlight it without covering it.","Click the image to place a label. Enter finishes the text."};
  int index=int(tool);QString title=titles.value(index);
  if(shape&&tool==Tool::Select){const QHash<int,QString> kinds{{int(editor::Shape::Kind::Redact),"Redaction"},{int(editor::Shape::Kind::Blur),"Blur"},{int(editor::Shape::Kind::Arrow),"Arrow"},{int(editor::Shape::Kind::Rectangle),"Rectangle"},{int(editor::Shape::Kind::Highlight),"Highlight"},{int(editor::Shape::Kind::Text),"Text"}};title=kinds.value(int(shape->kind),"Mark")+" selected";}
  toolTitle_->setText(title);toolHelp_->setText(help.value(index));
  const auto bounds=doc_.crop();documentInfo_->setText(QString("%1 × %2 px · PNG output\n%3").arg(bounds.width()).arg(bounds.height()).arg(doc_.modified()?"Edited":"Unchanged"));
 }
 {
  static const QHash<int,QPair<const char*,const char*>> placing{{int(Tool::Redact),{"Place redaction","redact"}},{int(Tool::Blur),{"Place blur","blur"}},{int(Tool::Arrow),{"Place arrow","annotate-arrow"}},
   {int(Tool::Rectangle),{"Place rectangle","rectangle"}},{int(Tool::Highlight),{"Place highlight","highlight"}},{int(Tool::Text),{"Place text label","text"}}};
  const auto entry=placing.value(int(tool));placeRow_->setVisible(entry.first);
  if(entry.first){place_->setText(QString::fromUtf8(entry.first));place_->setIcon(VisualStyle::icon(entry.second));place_->setToolTip(QString("%1 in the middle of the image, without the pointer (Insert)").arg(QString::fromUtf8(entry.first)));}
 }
 syncGeometry();
 undo_->setEnabled(!busy_&&doc_.canUndo());redo_->setEnabled(!busy_&&doc_.canRedo());
 const int percent=int(std::lround(canvas_->zoom()*100));
 status_->setText(QString("%1%").arg(percent));status_->setAccessibleName(QString("Zoom %1%").arg(percent));status_->setToolTip("Zoom");
}

void EditorWindow::syncGeometry(){
 const auto geometry=canvas_->selectionGeometry();geometryRow_->setVisible(geometry.has_value());
 if(!geometry)return;
 const auto* shape=canvas_->cropPending()?nullptr:doc_.find(canvas_->selected());
 const bool arrow=shape&&shape->kind==editor::Shape::Kind::Arrow,text=shape&&shape->kind==editor::Shape::Kind::Text;
 geometryTitle_->setText(canvas_->cropPending()?"Crop area":arrow?"Tail and head":"Position and size");
 static const QStringList boxLabels{"X","Y","Width","Height"},arrowLabels{"Tail X","Tail Y","Head X","Head Y"};
 static const QStringList boxNames{"X position","Y position","Width","Height"},arrowNames{"Arrow tail X","Arrow tail Y","Arrow head X","Arrow head Y"};
 const auto r=*geometry;const int values[4]={r.x(),r.y(),arrow?r.x()+r.width():r.width(),arrow?r.y()+r.height():r.height()};
 const auto image=doc_.size();const int extent=std::max(image.width(),image.height());
 syncingGeometry_=true;
 for(int i=0;i<4;++i){
  auto* box=geometry_[i];
  geometryLabels_[i]->setText((arrow?arrowLabels:boxLabels)[i]);box->setAccessibleName((arrow?arrowNames:boxNames)[i]+" in image pixels");
  // Text follows its size control; only its position is edited here.
  const bool shown=i<2||!text;geometryLabels_[i]->setVisible(shown);box->setVisible(shown);
  // The range always holds the true value, even for marks partly off the image.
  const int low=(i<2||arrow)?-extent:1,high=2*extent;
  box->setRange(std::min(low,values[i]),std::max(high,values[i]));
  if(box->value()!=values[i])box->setValue(values[i]);
 }
 syncingGeometry_=false;
}

void EditorWindow::applyGeometry(){
 if(syncingGeometry_||busy_)return;
 const auto* shape=canvas_->cropPending()?nullptr:doc_.find(canvas_->selected());
 const bool arrow=shape&&shape->kind==editor::Shape::Kind::Arrow;
 const int x=geometry_[0]->value(),y=geometry_[1]->value(),a=geometry_[2]->value(),b=geometry_[3]->value();
 // A refused value (empty, or off the image) snaps back to the current one.
 if(!canvas_->setSelectionGeometry(arrow?QRect(x,y,a-x,b-y):QRect(x,y,a,b)))syncGeometry();
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
  if(png.isEmpty()){showSaveFailed(path,"the edited image could not be encoded");return;}
  QSaveFile file(path);file.setDirectWriteFallback(false);
  if(!file.open(QIODevice::WriteOnly)||file.write(png)!=png.size()||!file.commit()){showSaveFailed(path,file.errorString());return;}
  showSaved(path);
 });
}

void EditorWindow::showSaved(const QString& path){
 const QFileInfo file(path);
 lastSave_={{"status","saved"},{"path",file.absoluteFilePath()},{"name",file.fileName()}};
 savedLabel_->setText("Saved "+file.fileName());VisualStyle::role(savedLabel_,"success");
 savedLabel_->setToolTip(file.absoluteFilePath());savedLabel_->setAccessibleName("Saved a copy");savedLabel_->setAccessibleDescription(file.absoluteFilePath());savedLabel_->show();
 saveError_->hide();showFolder_->show();chooseAnother_->hide();
 header_->setDocument("Edit image","Not uploaded · Copy saved");
}
void EditorWindow::showSaveFailed(const QString& path,const QString& reason){
 // The draft is untouched; the reason and a way on sit beside Save a copy….
 const QFileInfo file(path);
 lastSave_={{"status","failed"},{"path",file.absoluteFilePath()},{"name",file.fileName()},{"reason",reason}};
 // Short enough for the pinned Document block at large text; the file and
 // the kept draft are in the tooltip and accessible description.
 saveError_->setText(QString("Couldn't save the copy: %1.").arg(reason));
 saveError_->setToolTip(QString("%1\nYour edits are kept.").arg(file.absoluteFilePath()));saveError_->setAccessibleName("Save a copy failed");
 saveError_->setAccessibleDescription(QString("Couldn't save %1: %2. Your edits are kept.").arg(file.absoluteFilePath(),reason));
 saveError_->show();savedLabel_->hide();showFolder_->hide();chooseAnother_->show();
 header_->setDocument("Edit image","Not uploaded");
}
bool EditorWindow::showSavedCopy(){
 if(lastSave_["status"]!="saved")return false;
 QPointer<EditorWindow> self(this);
 host::showInFolder(lastSave_["path"].toString(),[self](const QJsonObject& result){if(self)self->lastShowInFolder_=result;});
 return true;
}

QList<QWidget*> EditorWindow::actionable() const {
 // Controls that act: tool and action buttons, fields, the canvas. Labels,
 // the header and its window controls are left alone.
 QList<QWidget*> result{canvas_};
 for(auto* widget:findChildren<QWidget*>()){
  if(header_->isAncestorOf(widget)||canvas_->isAncestorOf(widget))continue;
  if(qobject_cast<QAbstractButton*>(widget)||qobject_cast<QAbstractSpinBox*>(widget)||qobject_cast<QComboBox*>(widget)||(qobject_cast<QAbstractSlider*>(widget)&&!qobject_cast<QScrollBar*>(widget)))result.append(widget);
 }
 return result;
}
void EditorWindow::setBusy(const QString& message){
 chooser_->cancel();busy_=true;
 // Each control is disabled on its own, so the document details, the header
 // and this message keep full contrast while the image is prepared.
 for(auto* widget:actionable())widget->setEnabled(false);
 destination_->hide();message_->setText(message);VisualStyle::role(message_,"secondary");message_->show();setCursor(Qt::BusyCursor);
}
void EditorWindow::setFailure(const QString& message){message_->setText(message);VisualStyle::role(message_,"error");message_->show();}

QJsonObject EditorWindow::viewState() const {
 const auto& style=canvas_->style();const auto selected=canvas_->selected();
 QJsonObject message;if(!message_->isHidden())message={{"text",message_->text()},{"tone",message_->property("visualRole").toString()}};
 return {{"tool",toolNames.value(int(canvas_->tool()))},{"selectedShapeId",selected?QJsonValue(double(selected)):QJsonValue()},
  {"geometry",rectValue(canvas_->selectionGeometry())},{"cropPending",canvas_->cropPending()},{"selection",canvas_->selectionSummary()},
  {"zoomPercent",std::round(canvas_->zoom()*1000)/10},{"fitted",canvas_->fitted()},{"visibleRect",rectValue(canvas_->visibleImageRect())},
  {"canvasSize",QJsonArray{canvas_->width(),canvas_->height()}},{"editingText",canvas_->editingText()},{"busy",busy_},
  {"style",QJsonObject{{"color",style.color.name()},{"width",style.width},{"strength",style.strength},{"fontPixels",style.fontPixels},{"font",style.fontFamily}}},
  {"message",message.isEmpty()?QJsonValue():QJsonValue(message)},{"lastSave",lastSave_.isEmpty()?QJsonValue():QJsonValue(lastSave_)},
  {"lastShowInFolder",lastShowInFolder_.isEmpty()?QJsonValue():QJsonValue(lastShowInFolder_)}};
}

QJsonObject EditorWindow::viewCommand(const QJsonObject& params,QString& code,QString& message){
 auto fail=[&](const QString& c,const QString& m){code=c;message=m;return QJsonObject{};};
 auto only=[&](QStringList keys){keys<<"action";for(auto it=params.begin();it!=params.end();++it)if(!keys.contains(it.key()))return false;return true;};
 if(params.contains("action")&&!params["action"].isString())return fail("invalid_request","action must be a string");
 const auto action=params["action"].toString("inspect");
 if(action=="inspect"){if(!only({}))return fail("invalid_request","inspect has no other parameters");return viewState();}
 if(busy_)return fail("editor_finalizing","The edit was already submitted or discarded");
 // Each action is the window's own control, so the user sees the same change.
 if(action=="tool"){
  if(!only({"tool"})||!toolNames.contains(params["tool"].toString()))return fail("invalid_request","tool must be select, redact, blur, crop, arrow, rectangle, highlight or text");
  selectTool(Tool(toolNames.indexOf(params["tool"].toString())));
 }else if(action=="select"){
  if(!only({"shapeId"})||!params.contains("shapeId"))return fail("invalid_request","select needs shapeId, or null to clear the selection");
  int id=0;if(!params["shapeId"].isNull()&&!integerIn(params["shapeId"],1,INT_MAX,id))return fail("invalid_request","shapeId must be a shape id or null");
  if(id&&!doc_.find(quint64(id)))return fail("edit_shape_missing","No shape has this id");
  if(id&&canvas_->cropPending())selectTool(Tool::Select);
  canvas_->select(quint64(id));
 }else if(action=="selectNext"||action=="selectPrevious"){
  if(!only({}))return fail("invalid_request",action+" has no other parameters");
  if(!canvas_->selectAdjacent(action=="selectNext"?1:-1,true))return fail("edit_shape_missing","There is no mark to select");
 }else if(action=="place"){
  if(!only({}))return fail("invalid_request","place has no other parameters");
  // Text needs typing; agents add labels with editor.apply.
  const auto tool=canvas_->tool();
  if(tool==Tool::Select||tool==Tool::Crop||tool==Tool::Text)return fail("invalid_request","Choose redact, blur, arrow, rectangle or highlight first; add text with editor.apply");
  canvas_->placeMark();
 }else if(action=="geometry"){
  const auto r=params["rect"].toArray();int v[4]={};
  if(!only({"rect"})||!params["rect"].isArray()||r.size()!=4)return fail("invalid_request","geometry needs rect [x, y, width, height]");
  for(int i=0;i<4;++i)if(!integerIn(r[i],-1000000,1000000,v[i]))return fail("invalid_request","rect values must be integers");
  if(!canvas_->selectionGeometry())return fail("editor_no_selection","Select a mark or choose Crop first");
  if(!canvas_->setSelectionGeometry(QRect(v[0],v[1],v[2],v[3])))return fail("invalid_request","That geometry is empty, unchanged or outside the image");
 }else if(action=="applyCrop"||action=="cancelCrop"){
  if(!only({}))return fail("invalid_request",action+" has no other parameters");
  if(!canvas_->cropPending())return fail("invalid_request","Choose the Crop tool first");
  if(action=="applyCrop")canvas_->applyCrop();else canvas_->cancelCrop();
 }else if(action=="zoom"){
  const double percent=params["percent"].toDouble();
  if(!only({"percent"})||!params["percent"].isDouble()||!std::isfinite(percent)||percent<2||percent>3200)return fail("invalid_request","zoom needs percent from 2 to 3200");
  canvas_->setZoom(percent/100);
 }else if(action=="fit"||action=="actualSize"){
  if(!only({}))return fail("invalid_request",action+" has no other parameters");
  if(action=="fit")canvas_->fit();else canvas_->actualSize();
 }else if(action=="pan"){
  const auto d=params["delta"].toArray();int dx=0,dy=0;
  if(!only({"delta"})||!params["delta"].isArray()||d.size()!=2||!integerIn(d[0],-100000,100000,dx)||!integerIn(d[1],-100000,100000,dy))return fail("invalid_request","pan needs delta [dx, dy] in canvas pixels");
  canvas_->panBy(QPointF(dx,dy));
 }else if(action=="showSavedCopy"){
  if(!only({}))return fail("invalid_request","showSavedCopy has no other parameters");
  if(!showSavedCopy())return fail("editor_no_saved_copy","Save a copy first");
 }else return fail("invalid_request","action must be inspect, tool, select, selectNext, selectPrevious, place, geometry, applyCrop, cancelCrop, zoom, fit, actualSize, pan or showSavedCopy");
 refresh();return viewState();
}

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
