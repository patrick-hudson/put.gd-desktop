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
#include <QDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QThread>
#include <memory>
#include <QFileDialog>
#include <QScreen>
#include <QShortcut>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace putgd {
using editor::Tool;
namespace {
QFrame* separator(QWidget* parent){auto* line=Graphite::rule(parent,Qt::Vertical);line->setFixedHeight(22);return line;}
// A colour choice: its colour in a rounded square, a ring when chosen, the
// accent ring under keyboard focus. The custom picker shows a colour wheel.
// The 32 px button is the hit target; the painted square stays 22 px. Its
// edge is the 3:1 control outline, on whole pixels, so white stays visible
// on the light panel and near-black on the dark one.
class Swatch final:public QToolButton {
public:
 Swatch(const QColor& color,QWidget* parent,bool custom=false):QToolButton(parent),color_(color),custom_(custom){setFixedSize(32,32);setFocusPolicy(Qt::StrongFocus);setCursor(Qt::PointingHandCursor);}
protected:
 void paintEvent(QPaintEvent*)override{
  QPainter p(this);p.setRenderHint(QPainter::Antialiasing);const auto& t=Theme::current();const QRectF box=QRectF(rect()).adjusted(5.5,5.5,-5.5,-5.5);
  if(custom_){QConicalGradient wheel(box.center(),0);const QList<QColor> stops{QColor("#ef4444"),QColor("#f59e0b"),QColor("#facc15"),QColor("#22c55e"),QColor("#3b82f6"),QColor("#a855f7"),QColor("#ef4444")};for(int i=0;i<stops.size();++i)wheel.setColorAt(i/6.0,stops[i]);p.setBrush(wheel);}else p.setBrush(color_);
  p.setPen(QPen(t.controlOutline,1));p.drawRoundedRect(box,Graphite::Metric::swatchRadius,Graphite::Metric::swatchRadius);
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
 // The tool is Redact; the marks it makes are redactions.
 struct Entry{Tool tool;const char* icon;const char* label;const char* key;const char* detail;};
 const Entry entries[]={{Tool::Select,"select","Select and move","V",""},{Tool::Redact,"redact","Redact","X","covers with an opaque redaction"},{Tool::Blur,"blur","Blur","B","a visual effect only"},{Tool::Crop,"crop","Crop","C",""},
  {Tool::Arrow,"annotate-arrow","Arrow","A",""},{Tool::Rectangle,"rectangle","Rectangle","R",""},{Tool::Highlight,"highlight","Highlight","H",""},{Tool::Text,"text","Text","T",""}};
 for(const auto& entry:entries){
  auto* button=new QToolButton(toolbar);button->setCheckable(true);button->setIcon(VisualStyle::icon(entry.icon));button->setIconSize(QSize(20,20));button->setFixedSize(36,36);button->setFocusPolicy(Qt::StrongFocus);
  const auto label=QString::fromUtf8(entry.label),detail=QString::fromUtf8(entry.detail);
  button->setToolTip(detail.isEmpty()?QString("%1 (%2)").arg(label,entry.key):QString("%1 (%2) — %3").arg(label,entry.key,detail));button->setAccessibleName(label);button->setAccessibleDescription(detail);
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
 // Document stays pinned while there is room; short bodies scroll it with
 // the properties so the footer never squeezes them out.
 auto* side=new Graphite::Surface("surface",Qt::LeftEdge,this);side->setObjectName("editorSide");side->setFixedWidth(Graphite::scaled(256));
 side_=side;side->installEventFilter(this);
 auto* sideLayout=new QVBoxLayout(side);sideLayout_=sideLayout;sideLayout->setContentsMargins(1,0,0,0);sideLayout->setSpacing(0);
 auto* panel=new Graphite::Surface("surface",{},side);panel->setObjectName("editorOptions");
 auto* panelScroll=new QScrollArea(side);propertiesScroll_=panelScroll;panelScroll->setObjectName("editorPropertiesScroll");panelScroll->setWidgetResizable(true);panelScroll->setFocusPolicy(Qt::NoFocus);panelScroll->setFrameShape(QFrame::NoFrame);panelScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);panelScroll->setWidget(panel);sideLayout->addWidget(panelScroll,1);
 body->addWidget(side);
 auto* properties=new QVBoxLayout(panel);properties_=properties;properties->setContentsMargins(18,16,16,16);properties->setSpacing(12);
 toolTitle_=new QLabel(panel);toolTitle_->setObjectName("editorToolTitle");VisualStyle::role(toolTitle_,"sectionTitle");properties->addWidget(toolTitle_);
 toolHelp_=new QLabel(panel);toolHelp_->setObjectName("editorToolHelp");toolHelp_->setWordWrap(true);VisualStyle::role(toolHelp_,"muted");properties->addWidget(toolHelp_);
 hint_=new QLabel(panel);hint_->setWordWrap(true);VisualStyle::role(hint_,"warning");properties->addWidget(hint_);
 // Without a pointer: the tool's mark in the middle of the image (Insert).
 placeRow_=new QWidget(panel);auto* placeLayout=new QHBoxLayout(placeRow_);placeLayout->setContentsMargins(0,0,0,0);
 place_=new QPushButton(placeRow_);place_->setObjectName("placeMark");placeLayout->addWidget(place_);placeLayout->addStretch(1);properties->addWidget(placeRow_);
 // Divides the tool's help from its options; hidden when it has none.
 optionsRule_=Graphite::rule(panel);optionsRule_->setObjectName("editorOptionsRule");properties->addWidget(optionsRule_);
 QVBoxLayout* layout=nullptr;
 colorRow_=column(panel,layout);layout->addWidget(fieldLabel("Color",colorRow_));
 swatchGroup_=new QButtonGroup(this);
 auto* grid=new QGridLayout;grid->setContentsMargins(0,0,0,0);grid->setSpacing(4);int cell=0;
 const QStringList colorNames{"Red","Orange","Yellow","Green","Blue","White","Black"};
 for(const auto& color:{QColor(220,38,38),QColor(245,158,11),QColor(250,204,21),QColor(34,197,94),QColor(59,130,246),QColor(255,255,255),QColor(17,19,23)}){
  auto* swatch=makeSwatch(color);swatch->setAccessibleName("Color "+colorNames[cell]);swatch->setAccessibleDescription(color.name());swatch->setToolTip(colorNames[cell]+" ("+color.name()+")");swatches_.append(swatch);grid->addWidget(swatch,cell/4,cell%4);++cell;
 }
 // The custom picker follows the palette, making two even rows of four rather
 // than six and an orphaned pair; recent custom colours get their own row.
 customColor_=new Swatch({},colorRow_,true);customColor_->setObjectName("customColor");
 customColor_->setToolTip("Custom color…");customColor_->setAccessibleName("Custom color");grid->addWidget(customColor_,cell/4,cell%4);
 grid->setColumnStretch(4,1);layout->addLayout(grid);
 connect(customColor_,&QToolButton::clicked,this,&EditorWindow::pickCustomColor);
 auto* recent=new QWidget(colorRow_);recentLayout_=new QHBoxLayout(recent);recentLayout_->setContentsMargins(0,0,0,0);recentLayout_->setSpacing(4);recentLayout_->addStretch(1);recent->setObjectName("recentColors");recent->setAccessibleName("Recent custom colors");layout->addWidget(recent);
 properties->addWidget(colorRow_);
 widthRow_=column(panel,layout);layout->addWidget(fieldLabel("Line width",widthRow_));
 width_=VisualStyle::choiceBox(widthRow_);width_->addItem("Thin · 2 px",2);width_->addItem("Medium · 4 px",4);width_->addItem("Thick · 8 px",8);width_->setCurrentIndex(1);width_->setAccessibleName("Line width");layout->addWidget(width_);
 properties->addWidget(widthRow_);
 blurRow_=column(panel,layout);auto* blurLabelRow=new QHBoxLayout;blurLabelRow->setContentsMargins(0,0,0,0);blurLabelRow->addWidget(fieldLabel("Blur radius —",blurRow_));layout->addLayout(blurLabelRow);
 strength_=new QSlider(Qt::Horizontal,blurRow_);strength_->setRange(editor::EditDocument::minimumBlur,editor::EditDocument::maximumBlur);strength_->setValue(12);strength_->setAccessibleName("Blur radius");strength_->setObjectName("blurStrength");
 const auto strengthDescription=QString("Blur radius in image pixels, %1 to %2").arg(strength_->minimum()).arg(strength_->maximum());strength_->setAccessibleDescription(strengthDescription);
 auto* strengthRow=new QHBoxLayout;strengthRow->setContentsMargins(0,0,0,0);strengthRow->setSpacing(8);strengthRow->addWidget(strength_,1);
 strengthValue_=new QLabel(QString("%1 px").arg(strength_->value()),blurRow_);strengthValue_->setObjectName("blurStrengthValue");strengthValue_->setAccessibleName("Blur radius value");strengthValue_->setAccessibleDescription(strengthDescription);strengthValue_->setAlignment(Qt::AlignRight|Qt::AlignVCenter);strengthValue_->setMinimumWidth(strengthValue_->fontMetrics().horizontalAdvance(QString("%1 px").arg(strength_->maximum())));blurLabelRow->addWidget(strengthValue_);layout->addLayout(strengthRow);
 connect(strength_,&QSlider::valueChanged,strengthValue_,[this](int value){strengthValue_->setText(QString("%1 px").arg(value));});
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
  fields->addWidget(geometryLabels_[i],i,0);fields->addWidget(box,i,1);
  connect(box,&QSpinBox::valueChanged,this,&EditorWindow::applyGeometry);
 }
 layout->addLayout(fields);properties->addWidget(geometryRow_);
 remove_=new QPushButton("Remove mark",panel);remove_->setObjectName("removeMark");remove_->setIcon(VisualStyle::icon("remove"));remove_->setToolTip("Remove the selected mark (Delete)");VisualStyle::role(remove_,"quiet");properties->addWidget(remove_,0,Qt::AlignLeft);
 auto* keyboardHelp=new QToolButton(panel);keyboardHelp->setObjectName("editorKeyboardHelp");keyboardHelp->setText("Keyboard help");keyboardHelp->setAccessibleName("Keyboard help");Graphite::disclosure(keyboardHelp);properties->addWidget(keyboardHelp,0,Qt::AlignLeft);
 auto* keyboardText=new QLabel("Click a mark, or use [ and ] to cycle marks. Arrows move; Shift+arrows resize; Ctrl uses 10 px steps. Tab focuses properties; Delete removes; Escape deselects. Insert places a mark. Ctrl+Z undoes; Ctrl+Shift+Z redoes.",panel);keyboardText->setObjectName("editorKeyboardHelpText");keyboardText->setWordWrap(true);keyboardText->setTextFormat(Qt::PlainText);VisualStyle::role(keyboardText,"secondary");keyboardText->hide();properties->addWidget(keyboardText);
 connect(keyboardHelp,&QToolButton::toggled,this,[this,keyboardText](bool open){keyboardText->setVisible(open);if(open)QTimer::singleShot(0,this,[this,keyboardText]{if(!keyboardText->isHidden())propertiesScroll_->ensureWidgetVisible(keyboardText,0,12);});});
 properties->addStretch(1);
 auto* document=new Graphite::Surface("surface",Qt::TopEdge,side);document->setObjectName("editorDocument");document_=document;
 auto* documentLayout=new QVBoxLayout(document);documentLayout->setContentsMargins(18,12,16,14);documentLayout->setSpacing(6);
 documentLayout->addWidget(fieldLabel("Document",document));
 documentInfo_=new QLabel(document);documentInfo_->setObjectName("editorDocumentInfo");documentInfo_->setWordWrap(true);VisualStyle::role(documentInfo_,"secondary");documentLayout->addWidget(documentInfo_);
 // Save a copy… feedback, next to the button that made it.
 savedLabel_=new Graphite::ElidedLabel(document);savedLabel_->setObjectName("editorSaved");savedLabel_->setTextFormat(Qt::PlainText);savedLabel_->setContextMenuPolicy(Qt::ActionsContextMenu);savedLabel_->hide();documentLayout->addWidget(savedLabel_);
 saveTarget_=new Graphite::ElidedLabel(document);saveTarget_->setObjectName("editorSaveTarget");saveTarget_->setTextFormat(Qt::PlainText);saveTarget_->setRevealable(true);VisualStyle::role(saveTarget_,"error");saveTarget_->hide();documentLayout->addWidget(saveTarget_);
 // A failure wraps rather than elides: its reason is what the user needs.
 saveError_=new QLabel(document);saveError_->setObjectName("editorSaveError");saveError_->setTextFormat(Qt::PlainText);saveError_->setWordWrap(true);VisualStyle::role(saveError_,"error");saveError_->hide();documentLayout->addWidget(saveError_);
 auto* copyPath=new QAction("Copy full path",savedLabel_);savedLabel_->addAction(copyPath);
 connect(copyPath,&QAction::triggered,this,&EditorWindow::copySavedPath);
 auto* savedActions=new QHBoxLayout;savedActions->setContentsMargins(0,0,0,0);savedActions->setSpacing(8);
 // Framed like Choose another location… in the same place, so the actions
 // line up with the text above rather than floating on a quiet inset.
 showFolder_=new QPushButton("Show in folder",document);showFolder_->setObjectName("showSavedCopy");showFolder_->hide();savedActions->addWidget(showFolder_);
 // The elided name's full path, without a pointer: a focusable button.
 copyPath_=new QPushButton("Copy path",document);copyPath_->setObjectName("copySavedPath");copyPath_->setToolTip("Copy the saved copy's full path");copyPath_->hide();savedActions->addWidget(copyPath_);
 chooseAnother_=new QPushButton("Choose another location…",document);chooseAnother_->setObjectName("chooseAnotherLocation");chooseAnother_->hide();savedActions->addWidget(chooseAnother_);
 savedActions->addStretch(1);documentLayout->addLayout(savedActions);
 folderError_=new QLabel("Couldn't open the folder. Copy its path instead",document);folderError_->setObjectName("editorFolderError");folderError_->setTextFormat(Qt::PlainText);folderError_->setWordWrap(true);VisualStyle::role(folderError_,"error");folderError_->hide();documentLayout->addWidget(folderError_);
 sideLayout->addWidget(document);
 root->addLayout(body,1);

 // Rows: a preparation failure; the destination when it doesn't fit beside
 // the actions; then the destination (or progress) and the actions. Column
 // 0 is the one stretch, so a destination that fits there is never elided.
 auto* footer=new Graphite::Surface("bg",Qt::TopEdge,this);footer->setObjectName("editorFooter");
 footerGrid_=new QGridLayout(footer);footerGrid_->setContentsMargins(14,10,14,10);footerGrid_->setHorizontalSpacing(8);footerGrid_->setVerticalSpacing(8);footerGrid_->setColumnStretch(0,1);
 notice_=new QWidget(footer);notice_->setObjectName("editorPreparationFailure");
 auto* noticeLayout=new QGridLayout(notice_);noticeLayout->setContentsMargins(0,0,0,0);noticeLayout->setSpacing(8);
 noticeText_=new QLabel(notice_);noticeText_->setObjectName("editorPreparationReason");noticeText_->setTextFormat(Qt::PlainText);noticeText_->setWordWrap(true);VisualStyle::role(noticeText_,"error");noticeLayout->addWidget(noticeText_,0,0,1,3);noticeLayout->setColumnStretch(2,1);
 keepEditing_=new QPushButton("Keep editing",notice_);keepEditing_->setObjectName("keepEditing");noticeLayout->addWidget(keepEditing_,1,0);
 saveInstead_=new QPushButton("Save a copy…",notice_);saveInstead_->setObjectName("saveCopyInstead");saveInstead_->setToolTip("Save the edited image to a file instead (Ctrl+S)");noticeLayout->addWidget(saveInstead_,1,1);
 notice_->hide();footerGrid_->addWidget(notice_,0,0,1,4);
 // Plain, wrapping and focusable: the full name reads without a pointer.
 destination_=new QLabel(footer);destination_->setObjectName("editorDestination");destination_->setTextFormat(Qt::PlainText);VisualStyle::role(destination_,"secondary");
 destination_->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);destination_->setFocusPolicy(Qt::StrongFocus);destination_->hide();footerGrid_->addWidget(destination_,2,0);
 message_=new QLabel(footer);message_->setWordWrap(true);message_->hide();footerGrid_->addWidget(message_,2,0);
 save_=new QPushButton("Save a copy…",footer);save_->setObjectName("editorSaveCopy");save_->setIcon(VisualStyle::icon("save"));save_->setToolTip("Save the edited image to a file (Ctrl+S)");footerGrid_->addWidget(save_,2,1);
 cancel_=new QPushButton("Cancel",footer);cancel_->setObjectName("editorCancel");VisualStyle::role(cancel_,"quiet");footerGrid_->addWidget(cancel_,2,2);
 upload_=new QPushButton("Upload",footer);upload_->setObjectName("editorUpload");upload_->setIcon(VisualStyle::icon("upload"));VisualStyle::role(upload_,"primary");upload_->setToolTip("Upload the finished, sanitized image (Ctrl+Enter). The source file is not uploaded.");upload_->setAutoDefault(false);footerGrid_->addWidget(upload_,2,3);
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
   width_->setCurrentIndex(std::max(0,width_->findData(shape->width)));textSize_->setValue(shape->fontPixels);strength_->setValue(shape->strength);strengthValue_->setText(QString("%1 px").arg(shape->strength));
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
 connect(remove_,&QPushButton::clicked,this,[this]{if(!busy_&&canvas_->removeSelected())canvas_->setFocus();});
 connect(place_,&QPushButton::clicked,this,[this]{if(!busy_)canvas_->placeMark();});
 connect(showFolder_,&QPushButton::clicked,this,&EditorWindow::showSavedCopy);
 connect(copyPath_,&QPushButton::clicked,this,&EditorWindow::copySavedPath);
 connect(chooseAnother_,&QPushButton::clicked,this,&EditorWindow::saveCopy);
 connect(keepEditing_,&QPushButton::clicked,this,[this]{setNotice({});canvas_->setFocus();});
 connect(saveInstead_,&QPushButton::clicked,this,&EditorWindow::saveCopy);
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
 auto submit=[this]{if(busy_||savingCopy_)return;finishPendingEdits();emit uploadRequested();};
 bind({QKeySequence("Ctrl+Return"),QKeySequence("Ctrl+Enter")},submit);
 connect(upload_,&QPushButton::clicked,this,submit);
 connect(save_,&QPushButton::clicked,this,&EditorWindow::saveCopy);
 connect(chooser_,&FileDialog::stateChanged,this,[this]{if(auto* dialog=chooser_->fallbackDialog())dialog->setDefaultSuffix("png");});
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
 destination_->setText(destination);destination_->setAccessibleName(destination);destination_->setVisible(!destination.isEmpty()&&!busy_&&!savingCopy_);placeDestination();}

void EditorWindow::placeDestination(){
 if(!footerGrid_||destination_->text().isEmpty())return;
 // Beside the actions while the whole name fits there; otherwise a wrapping
 // line of its own above them.
 const auto margins=footerGrid_->contentsMargins();const int spacing=footerGrid_->horizontalSpacing();
 int room=width()-margins.left()-margins.right();
 for(auto* button:{save_,cancel_,upload_})if(!button->isHidden())room-=button->sizeHint().width()+spacing;
 const bool above=destination_->fontMetrics().horizontalAdvance(destination_->text())>room;
 if(above==destinationAbove_)return;
 destinationAbove_=above;destination_->setWordWrap(above);footerGrid_->removeWidget(destination_);
 if(above)footerGrid_->addWidget(destination_,1,0,1,4);else footerGrid_->addWidget(destination_,2,0);
}
void EditorWindow::resizeEvent(QResizeEvent* event){QWidget::resizeEvent(event);placeDestination();}
bool EditorWindow::eventFilter(QObject* object,QEvent* event){
 if(object==side_&&(event->type()==QEvent::Resize||event->type()==QEvent::LayoutRequest))QTimer::singleShot(0,this,&EditorWindow::placeDocument);
 return QWidget::eventFilter(object,event);
}
void EditorWindow::placeDocument(){
 if(!document_||!side_->isVisible())return;
 // Leave a useful properties viewport when the footer or enlarged type takes room.
 const bool scroll=side_->height()<Graphite::scaled(420);
 if(scroll==documentScrolling_)return;
 documentScrolling_=scroll;
 sideLayout_->removeWidget(document_);properties_->removeWidget(document_);
 if(scroll){properties_->insertWidget(properties_->count()-1,document_);document_->layout()->setContentsMargins(0,12,0,0);}
 else{sideLayout_->addWidget(document_);document_->layout()->setContentsMargins(18,12,16,14);}
 document_->show();
}

void EditorWindow::revealDocument(){
 // Save feedback must be visible even when Document now lives below the fold.
 QTimer::singleShot(0,this,[this]{placeDocument();if(documentScrolling_)propertiesScroll_->ensureWidgetVisible(document_,0,12);});
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
 else hint_->setText({});
 if(tool==Tool::Redact&&!shape){hint_->setText("The covered pixels are removed from saved and uploaded images.");VisualStyle::role(hint_,"secondary");}
 else VisualStyle::role(hint_,"warning");
 hint_->setVisible(!hint_->text().isEmpty());
 {
  // The panel names the tool (or the selected mark) and how to use it.
  static const QStringList titles{"Select","Redact","Blur","Crop","Arrow","Rectangle","Highlight","Text"};
  static const QStringList help{"Click a mark, or use [ and ] to cycle marks. Arrows move; Shift+arrows resize. Tab focuses properties; Escape deselects.","Drag over private information to cover it with a redaction, an opaque black box.","Drag over an area to blur it.","Drag to choose the area to keep, or set it below, then apply the crop.","Drag from the tail to the point of the arrow.","Drag to frame a detail with an outline.","Drag across text to highlight it without covering it.","Click the image to place a label. Enter finishes the text."};
  int index=int(tool);QString title=titles.value(index);
  // A selected mark names itself, whichever tool picked it up.
  if(shape){const QHash<int,QString> kinds{{int(editor::Shape::Kind::Redact),"Redaction"},{int(editor::Shape::Kind::Blur),"Blur"},{int(editor::Shape::Kind::Arrow),"Arrow"},{int(editor::Shape::Kind::Rectangle),"Rectangle"},{int(editor::Shape::Kind::Highlight),"Highlight"},{int(editor::Shape::Kind::Text),"Text"}};title=kinds.value(int(shape->kind),"Mark")+" selected";}
  toolTitle_->setText(title);toolHelp_->setText(shape?QStringLiteral("Drag or use arrow keys to move."):help.value(index));
  const auto bounds=doc_.crop();documentInfo_->setText(QString("%1 × %2 px · PNG output\n%3").arg(bounds.width()).arg(bounds.height()).arg(doc_.modified()?"Edited":"Unchanged"));
 }
 {
  static const QHash<int,QPair<const char*,const char*>> placing{{int(Tool::Redact),{"Place redaction","redact"}},{int(Tool::Blur),{"Place blur","blur"}},{int(Tool::Arrow),{"Place arrow","annotate-arrow"}},
   {int(Tool::Rectangle),{"Place rectangle","rectangle"}},{int(Tool::Highlight),{"Place highlight","highlight"}},{int(Tool::Text),{"Place text label","text"}}};
  const auto entry=placing.value(int(tool));placeRow_->setVisible(entry.first);
  if(entry.first){place_->setText(QString::fromUtf8(entry.first));place_->setIcon(VisualStyle::icon(entry.second));place_->setToolTip(QString("%1 in the middle of the image, without the pointer (Insert)").arg(QString::fromUtf8(entry.first)));}
 }
 syncGeometry();syncSaved();remove_->setVisible(shape&&tool!=Tool::Crop);
 bool options=false;for(auto* row:{colorRow_,widthRow_,blurRow_,textRow_,cropRow_,geometryRow_})options=options||!row->isHidden();
 optionsRule_->setVisible(options);
 undo_->setEnabled(!busy_&&doc_.canUndo());redo_->setEnabled(!busy_&&doc_.canRedo());
 // Cancel asks about discarding edits once there are any; it says so.
 const bool edited=doc_.modified();
 if(cancel_->text()!=(edited?"Discard…":"Cancel")){
  cancel_->setText(edited?"Discard…":"Cancel");cancel_->setToolTip(edited?"Discard your edits and close without uploading (asks first)":"Close the editor without uploading");placeDestination();
 }
 const int percent=int(std::lround(canvas_->zoom()*100));
 status_->setText(QString("%1%").arg(percent));status_->setAccessibleName(QString("Zoom %1%").arg(percent));status_->setToolTip("Zoom: Ctrl+plus / Ctrl+minus. Fit: Ctrl+0. Actual size: Ctrl+1.\nCanvas pan: Alt+arrows; Alt+Shift+arrows pan farther.");status_->setAccessibleDescription(status_->toolTip());
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

void EditorWindow::finishPendingEdits(){
 if(canvas_->editingText())canvas_->finishText(true);
 if(canvas_->cropPending())canvas_->applyCrop();
}

QString EditorWindow::pngCopyPath(const QString& path){
 return path.endsWith(".png",Qt::CaseInsensitive)?path:path+".png";
}
bool EditorWindow::saveCopy(){
 if(busy_||savingCopy_||chooser_->isOpen())return false;
 finishPendingEdits();
 auto folder=QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);if(folder.isEmpty())folder=QDir::homePath();
 chooseCopy(QDir(folder).filePath("put.gd-edit-"+QDateTime::currentDateTime().toString("yyyy-MM-dd-HH-mm-ss")+".png"));
 return true;
}
void EditorWindow::chooseCopy(const QString& path){
 const QFileInfo target(path);
 chooser_->open({FileDialog::Mode::Save,"Save a copy",target.absolutePath(),target.fileName(),{{"PNG image",{"*.png"}}}},[this](const FileDialog::Result& result){
  if(busy_||savingCopy_)return;
  if(result.failed()){showSaveFailed({},result.error);return;}
  if(!result.chosen())return;
  const auto normalized=pngCopyPath(result.path);
  // A native chooser confirmed its chosen name. Confirm a changed target there too.
  if(normalized!=result.path&&QFileInfo::exists(normalized)){chooseCopy(normalized);return;}
  writeCopy(normalized);
 });
 if(auto* dialog=chooser_->fallbackDialog())dialog->setDefaultSuffix("png");
}
void EditorWindow::setSavingCopy(bool saving){
 savingCopy_=saving;
 for(auto* button:{save_,saveInstead_,chooseAnother_})button->setEnabled(!saving&&!busy_);
 upload_->setEnabled(!saving&&!busy_);cancel_->setEnabled(!saving&&!busy_);
 if(saving){
  for(auto* widget:std::initializer_list<QWidget*>{destination_,saveError_,folderError_,savedLabel_,saveTarget_,showFolder_,copyPath_,chooseAnother_})widget->hide();
  message_->setText("Saving a copy…");VisualStyle::role(message_,"secondary");message_->show();
 }else{message_->hide();destination_->setVisible(!busy_&&!destination_->text().isEmpty());}
 placeDestination();
}
bool EditorWindow::writeCopy(const QString& path,CopyDone done){
 if(busy_||savingCopy_)return false;
 finishPendingEdits();
 const auto target=pngCopyPath(path);const auto generation=doc_.generation();
 // Qt's image/list copies detach on mutation; no live document reaches the worker.
 auto snapshot=doc_;
 setSavingCopy(true);lastSave_={{"status","saving"},{"path",target}};
 struct Result {QByteArray png;QString reason;};
 auto result=std::make_shared<Result>();
 auto* thread=QThread::create([snapshot=std::move(snapshot),target,result]{
  result->png=snapshot.exportPng();
  if(result->png.isEmpty()){result->reason="the edited image could not be encoded";return;}
  QSaveFile file(target);file.setDirectWriteFallback(false);
  if(!file.open(QIODevice::WriteOnly)||file.write(result->png)!=result->png.size()||!file.commit()){result->reason=file.errorString();result->png.clear();}
 });
 connect(thread,&QThread::finished,this,[this,target,generation,result,done=std::move(done)]{
  setSavingCopy(false);
  if(result->png.isEmpty())showSaveFailed(target,result->reason);
  else{showSaved(target);savedGeneration_=generation;syncSaved();}
  if(done)done(result->png,result->reason);
 });
 connect(thread,&QThread::finished,thread,&QObject::deleteLater);
 thread->start();return true;
}

void EditorWindow::showSaved(const QString& path){
 const QFileInfo file(path);
 ++folderRequest_;lastShowInFolder_={};folderError_->hide();
 lastSave_={{"status","saved"},{"path",file.absoluteFilePath()},{"name",file.fileName()}};savedGeneration_=doc_.generation();
 savedLabel_->setToolTip(file.absoluteFilePath());savedLabel_->setAccessibleDescription(file.absoluteFilePath());savedLabel_->show();
 saveError_->hide();saveTarget_->hide();showFolder_->show();copyPath_->show();chooseAnother_->hide();
 syncSaved();revealDocument();
}
void EditorWindow::syncSaved(){
 if(lastSave_["status"]!="saved")return;
 // The copy holds the draft as it was then; after another edit it is an earlier copy.
 const bool current=savedGeneration_==doc_.generation();const auto name=lastSave_["name"].toString();
 savedLabel_->setText(current?"Saved "+name:"Earlier copy saved: "+name);VisualStyle::role(savedLabel_,current?"success":"secondary");
 savedLabel_->setAccessibleName(current?"Saved a copy":"Saved an earlier copy");
 header_->setDocument("Edit image",current?"Not uploaded · Copy saved":"Not uploaded");
}
void EditorWindow::showSaveFailed(const QString& path,const QString& reason){
 // The draft is untouched; the reason and a way on sit beside Save a copy….
 // An empty path is the chooser's own failure, before any file was named.
 const QFileInfo file(path);
 auto why=reason.trimmed();if(why.endsWith('.'))why.chop(1);if(!why.isEmpty())why[0]=why[0].toLower();
 lastSave_={{"status","failed"},{"stage",path.isEmpty()?"chooser":"write"},{"path",path.isEmpty()?QJsonValue():QJsonValue(file.absoluteFilePath())},{"name",path.isEmpty()?QJsonValue():QJsonValue(file.fileName())},{"reason",reason}};
 ++folderRequest_;lastShowInFolder_={};folderError_->hide();
 // The file and kept draft are also in the accessible description.
 saveTarget_->setText(path.isEmpty()?QStringLiteral("Couldn't save the copy"):QString("Couldn't save ‹%1›").arg(file.fileName()));
 saveTarget_->setRevealText(path.isEmpty()?QStringLiteral("Couldn't save the copy"):file.absoluteFilePath());saveTarget_->setVisible(!path.isEmpty());
 saveError_->setText(path.isEmpty()?QString("Couldn't save the copy: %1.").arg(why):reason.trimmed());
 const auto where=path.isEmpty()?QStringLiteral("the copy"):file.absoluteFilePath();
 saveTarget_->setToolTip(where);saveTarget_->setAccessibleDescription(where+". Press Enter to show the full path.");
 saveError_->setToolTip(path.isEmpty()?QStringLiteral("Your edits are kept."):QString("%1\nYour edits are kept.").arg(where));saveError_->setAccessibleName("Save a copy failed");
 saveError_->setAccessibleDescription(QString("Couldn't save %1: %2. Your edits are kept.").arg(where,why));
 saveError_->show();savedLabel_->hide();showFolder_->hide();copyPath_->hide();chooseAnother_->show();
 header_->setDocument("Edit image","Not uploaded");revealDocument();
}
bool EditorWindow::copySavedPath(){
 if(lastSave_["status"]!="saved")return false;
 lastCopiedPath_=lastSave_["path"].toString();QGuiApplication::clipboard()->setText(lastCopiedPath_);
 // Brief confirmation on the button itself; its width holds still.
 const int before=copyPath_->sizeHint().width();copyPath_->setText("Path copied");copyPath_->setMinimumWidth(std::max(before,copyPath_->sizeHint().width()));
 QTimer::singleShot(1600,copyPath_,[button=copyPath_]{button->setText("Copy path");});
 return true;
}
bool EditorWindow::showSavedCopy(){
 if(lastSave_["status"]!="saved")return false;
 QPointer<EditorWindow> self(this);
 const auto path=lastSave_["path"].toString();const auto request=++folderRequest_;
 folderError_->hide();
 host::showInFolder(path,[self,request](const QJsonObject& result){
  if(!self||self->folderRequest_!=request)return;
  self->lastShowInFolder_=result;self->folderError_->setVisible(!result["opened"].toBool());if(!result["opened"].toBool())self->revealDocument();
 });
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
 // Submitted or discarded: an open discard question has no answer left.
 if(auto* box=discard_.data()){discard_=nullptr;box->close();}
 // Each control is disabled on its own, so the document details, the header
 // and this message keep full contrast while the image is prepared.
 for(auto* widget:actionable())widget->setEnabled(false);
 setNotice({});destination_->hide();message_->setText(message);VisualStyle::role(message_,"secondary");message_->show();setCursor(Qt::BusyCursor);
}
void EditorWindow::setFailure(const QString& message){message_->setText(message);VisualStyle::role(message_,"error");message_->show();}
void EditorWindow::showPreparationFailed(const QString& reason){
 // Nothing was handed off: the draft and its controls come back as they were.
 busy_=false;for(auto* widget:actionable())widget->setEnabled(true);unsetCursor();
 message_->hide();destination_->setVisible(!destination_->text().isEmpty());
 setNotice(reason);refresh();keepEditing_->setFocus();
}
void EditorWindow::setNotice(const QString& reason){
 noticeText_->setText(reason);noticeText_->setAccessibleName(reason.isEmpty()?QString():QStringLiteral("Upload failed"));noticeText_->setAccessibleDescription(reason);
 notice_->setVisible(!reason.isEmpty());save_->setVisible(reason.isEmpty());placeDestination();
}

QJsonObject EditorWindow::viewState() const {
 const auto& style=canvas_->style();const auto selected=canvas_->selected();
 QJsonObject message;if(!message_->isHidden())message={{"text",message_->text()},{"tone",message_->property("visualRole").toString()}};
 auto save=lastSave_;if(save["status"]=="saved")save["current"]=savedGeneration_==doc_.generation();
 return {{"scheme",Theme::name(Theme::scheme())},{"tool",toolNames.value(int(canvas_->tool()))},{"selectedShapeId",selected?QJsonValue(double(selected)):QJsonValue()},
  {"geometry",rectValue(canvas_->selectionGeometry())},{"cropPending",canvas_->cropPending()},{"selection",canvas_->selectionSummary()},
  {"zoomPercent",std::round(canvas_->zoom()*1000)/10},{"fitted",canvas_->fitted()},{"visibleRect",rectValue(canvas_->visibleImageRect())},
  {"canvasSize",QJsonArray{canvas_->width(),canvas_->height()}},{"editingText",canvas_->editingText()},{"busy",busy_},{"savingCopy",savingCopy_},
  {"style",QJsonObject{{"color",style.color.name()},{"width",style.width},{"strength",style.strength},{"fontPixels",style.fontPixels},{"font",style.fontFamily}}},
  {"message",message.isEmpty()?QJsonValue():QJsonValue(message)},{"lastSave",save.isEmpty()?QJsonValue():QJsonValue(save)},
  {"lastShowInFolder",lastShowInFolder_.isEmpty()?QJsonValue():QJsonValue(lastShowInFolder_)},{"lastCopiedPath",lastCopiedPath_.isEmpty()?QJsonValue():QJsonValue(lastCopiedPath_)},
  {"cancelLabel",cancel_->text()},{"destination",QJsonObject{{"text",destination_->text()},{"above",destinationAbove_}}},
  {"preparationFailure",notice_->isHidden()?QJsonValue():QJsonValue(QJsonObject{{"text",noticeText_->text()}})},
  {"keyboardHelp",QJsonObject{{"open",findChild<QToolButton*>("editorKeyboardHelp")->isChecked()},{"text",findChild<QLabel*>("editorKeyboardHelpText")->text()}}},
  {"discardConfirmation",QJsonObject{{"open",bool(discard_)},{"text",discard_?QJsonValue(discard_->property("confirmationText").toString()):QJsonValue()}}}};
}

QJsonObject EditorWindow::viewCommand(const QJsonObject& params,QString& code,QString& message){
 auto fail=[&](const QString& c,const QString& m){code=c;message=m;return QJsonObject{};};
 auto only=[&](QStringList keys){keys<<"action";for(auto it=params.begin();it!=params.end();++it)if(!keys.contains(it.key()))return false;return true;};
 if(params.contains("action")&&!params["action"].isString())return fail("invalid_request","action must be a string");
 const auto action=params["action"].toString("inspect");
 if(action=="inspect"){if(!only({}))return fail("invalid_request","inspect has no other parameters");return viewState();}
 if(busy_)return fail("editor_finalizing","The edit was already submitted or discarded");
 // Keep editing answers the discard question, else dismisses a preparation
 // failure: both buttons carry that label.
 if(action=="keepEditing"||action=="confirmDiscard"){
  if(!only({}))return fail("invalid_request",action+" has no other parameters");
  if(discard_){answerDiscard(action=="confirmDiscard");return viewState();}
  if(action=="keepEditing"&&!notice_->isHidden()){keepEditing_->click();return viewState();}
  return fail("invalid_request",action=="confirmDiscard"?"No discard confirmation is open":"Nothing is waiting for Keep editing");
 }
 if(discard_)return fail("editor_confirming","The discard confirmation is open; answer it first (keepEditing or confirmDiscard)");
 // Each action is the window's own control, so the user sees the same change.
 if(action=="keyboardHelp"){
  if(!only({}))return fail("invalid_request","keyboardHelp has no other parameters");
  findChild<QToolButton*>("editorKeyboardHelp")->toggle();
 }else if(action=="tool"){
  if(!only({"tool"})||!toolNames.contains(params["tool"].toString()))return fail("invalid_request","tool must be select, redact, blur, crop, arrow, rectangle, highlight or text");
  selectTool(Tool(toolNames.indexOf(params["tool"].toString())));
 }else if(action=="select"){
  if(!only({"shapeId"})||!params.contains("shapeId"))return fail("invalid_request","select needs shapeId, or null to clear the selection");
  int id=0;if(!params["shapeId"].isNull()&&!integerIn(params["shapeId"],1,INT_MAX,id))return fail("invalid_request","shapeId must be a shape id or null");
  if(id&&!doc_.find(quint64(id)))return fail("edit_shape_missing","No shape has this id");
  if(id&&canvas_->cropPending())selectTool(Tool::Select);
  canvas_->select(quint64(id));
 }else if(action=="removeMark"){
  if(!only({}))return fail("invalid_request","removeMark has no other parameters");
  if(!canvas_->removeSelected())return fail("editor_no_selection","Select a mark first");
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
 }else if(action=="showSavedCopy"||action=="copySavedPath"){
  if(!only({}))return fail("invalid_request",action+" has no other parameters");
  if(!(action=="showSavedCopy"?showSavedCopy():copySavedPath()))return fail("editor_no_saved_copy","Save a copy first");
 }else if(action=="discard"){
  // As Cancel / Discard… does: asks first when there are edits.
  if(!only({}))return fail("invalid_request","discard has no other parameters");
  requestCancel();if(busy_)return viewState();
 }else return fail("invalid_request","action must be inspect, tool, select, removeMark, selectNext, selectPrevious, place, geometry, applyCrop, cancelCrop, zoom, fit, actualSize, pan, keyboardHelp, showSavedCopy, copySavedPath, discard, keepEditing or confirmDiscard");
 refresh();return viewState();
}

void EditorWindow::requestCancel(){
 if(busy_||savingCopy_)return;
 if(discard_){discard_->raise();discard_->activateWindow();return;}
 if(!doc_.modified()){emit cancelRequested();return;}
 const auto text=QString("Discard your edits and close the editor?\nNothing has been uploaded. Your marks and crop are lost%1.").arg(lastSave_["status"]=="saved"?"; copies you saved stay":"");
 auto* box=Graphite::confirm(this,"Discard edits?",text,"Discard edits",QStringLiteral("Keep editing"),true);
 auto* keep=box->button(QMessageBox::Cancel);keep->setObjectName("keepEditingConfirm");
 box->setWindowModality(Qt::WindowModal);box->setAttribute(Qt::WA_DeleteOnClose);
 connect(box,&QDialog::finished,this,[this,box](int result){
  if(discard_==box)discard_=nullptr;
  if(result==QMessageBox::Yes&&!busy_)emit cancelRequested();
 });
 discard_=box;box->open();keep->setFocus();
}
void EditorWindow::answerDiscard(bool discard){
 if(discard_)static_cast<QMessageBox*>(discard_.data())->button(discard?QMessageBox::Yes:QMessageBox::Cancel)->click();
}

void EditorWindow::closeEvent(QCloseEvent* event){
 if(closing_){event->accept();return;}
 event->ignore();requestCancel();
}
void EditorWindow::showEvent(QShowEvent* event){
 QWidget::showEvent(event);if(!shown_){shown_=true;canvas_->fit();canvas_->setFocus();}
}
}
