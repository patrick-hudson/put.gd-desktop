#include "EditorWindow.h"
#include "FileDialog.h"
#include "Theme.h"
#include "Ui.h"
#include "BlurRadius.h"
#include "QuickRedactRunner.h"
#include "core/TextJobProtocol.h"
#include "platform/HostOpen.h"
#include <QAccessible>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QFileInfo>
#include <QGridLayout>
#include <QGuiApplication>
#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <QPainter>
#include <QScrollArea>
#include <QScrollBar>
#include "core/Settings.h"
#include <QButtonGroup>
#include <QCloseEvent>
#include <QColorDialog>
#include <QCheckBox>
#include <QComboBox>
#include <QFontComboBox>
#include <QMap>
#include <QDeadlineTimer>
#include <QPointer>
#include <QAbstractSpinBox>
#include <QDateTime>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QDialog>
#include <QMessageBox>
#include <QMenu>
#include <QActionGroup>
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
#include <QSplitter>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace putgd {
using editor::Tool;
using Ui::Space;
namespace {
// The Quick redact menu's words for each category; a selected suggested mark names its category the same way.
QString quickRedactLabel(const QString& category){
 static const QHash<QString,QString> labels{{"email","Email addresses"},{"phone","Phone numbers"},{"ip","IP addresses"},{"card","Card numbers"},{"key","Keys and tokens"},{"name","My username and name"},{"list","My redaction list"}};
 return labels.value(category,category);
}
// Each mark with the union of the recognized words' boxes it was padded from (unpadded), one per visual row of the match
// (quickRedact::markBoxes): a match across two rows is two marks, never one box over the pixels between them.
QList<QPair<QRect,QRect>> markedWords(const QList<editor::RecognizedWord>& candidates,const editor::quickRedact::Match& match,const QRect& bounds,const QImage& image){
 QList<QPair<QRect,QRect>> out;
 for(const auto& box:editor::quickRedact::markBoxes(candidates,match,bounds,image))out.append({box.mark,box.words});
 return out;
}
QFrame* separator(QWidget* parent){auto* line=Ui::rule(parent,Qt::Vertical);line->setFixedHeight(Ui::scaled(22));return line;}
// A colour choice: its colour in a rounded square, a ring when chosen, the
// accent ring under keyboard focus. The custom picker stays neutral.
// The 32 px button is the hit target; the painted square stays 24 px. Its
// edge is the 3:1 control outline, on whole pixels, so white stays visible
// on the light panel and near-black on the dark one.
class Swatch final:public QToolButton {
public:
 Swatch(const QColor& color,QWidget* parent,bool custom=false):QToolButton(parent),color_(color),custom_(custom){setFixedSize(Ui::scaled(32),Ui::scaled(32));setFocusPolicy(Qt::StrongFocus);setCursor(Qt::PointingHandCursor);}
protected:
 void paintEvent(QPaintEvent*)override{
  QPainter p(this);p.setRenderHint(QPainter::Antialiasing);const auto& t=Theme::current();const qreal inset=(width()-Ui::scaled(24))/2.0+0.5;const QRectF box=QRectF(rect()).adjusted(inset,inset,-inset,-inset);
  p.setBrush(custom_?t.surfaceRaised:color_);
  p.setPen(QPen(t.controlOutline,1));p.drawRoundedRect(box,Ui::Metric::swatchRadius,Ui::Metric::swatchRadius);
  if(custom_)Ui::icon("add",t.textSecondary).paint(&p,QRect(qRound(box.center().x()-Ui::scaled(14)/2.0),qRound(box.center().y()-Ui::scaled(14)/2.0),Ui::scaled(14),Ui::scaled(14)));
  if(isChecked()){p.setPen(QPen(t.text,2));p.setBrush(Qt::NoBrush);p.drawRoundedRect(QRectF(rect()).adjusted(1,1,-1,-1),7,7);}
  else if(underMouse()){p.setPen(QPen(t.borderStrong,1));p.setBrush(Qt::NoBrush);p.drawRoundedRect(QRectF(rect()).adjusted(1.5,1.5,-1.5,-1.5),7,7);}
  if(hasFocus()&&window()->testAttribute(Qt::WA_KeyboardFocusChange)){p.setPen(QPen(t.accent,1.5));p.setBrush(Qt::NoBrush);p.drawRoundedRect(QRectF(rect()).adjusted(0.75,0.75,-0.75,-0.75),7,7);}
 }
private:QColor color_;bool custom_;
};
class KeyboardHelp final:public QWidget {
public:
 explicit KeyboardHelp(QWidget* parent):QWidget(parent){grid_=new QGridLayout(this);grid_->setContentsMargins(0,0,0,0);grid_->setHorizontalSpacing(Ui::space(Space::s));grid_->setVerticalSpacing(Ui::space(Space::s));grid_->setColumnStretch(1,1);setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);}
 void addShortcut(QLabel* key,QLabel* meaning){auto* pair=new QWidget(this);auto* line=new QBoxLayout(QBoxLayout::LeftToRight,pair);line->setContentsMargins(0,0,0,0);line->addWidget(meaning);line->addWidget(key);rows_.append({pair,key,meaning});grid_->addWidget(pair,rows_.size()-1,0,1,2);reflow();}
protected:
 void resizeEvent(QResizeEvent* event)override{QWidget::resizeEvent(event);reflow();}
 void showEvent(QShowEvent* event)override{QWidget::showEvent(event);reflow();}
private:
 struct Pair{QWidget* widget;QLabel* key;QLabel* meaning;};QGridLayout* grid_;QList<Pair> rows_;
 void reflow(){const int room=contentsRect().width(),gap=Ui::space(Space::m);bool anyStacked=false;for(const auto& pair:rows_){Ui::role(pair.meaning,"body");Ui::role(pair.key,"secondary");Ui::setMono(pair.key,Ui::Type::technical);pair.key->setMinimumWidth(0);pair.key->setMaximumWidth(QWIDGETSIZE_MAX);const int keyWidth=pair.key->fontMetrics().horizontalAdvance(pair.key->text());const bool stacked=pair.meaning->fontMetrics().horizontalAdvance(pair.meaning->text())+gap+keyWidth>room;anyStacked|=stacked;auto* line=static_cast<QBoxLayout*>(pair.widget->layout());line->setDirection(stacked?QBoxLayout::TopToBottom:QBoxLayout::LeftToRight);line->setSpacing(stacked?1:gap);line->setStretch(0,stacked?0:1);line->setStretch(1,0);if(!stacked)pair.key->setFixedWidth(keyWidth);pair.widget->setProperty("stacked",stacked);}setProperty("stacked",anyStacked);}
};
QLabel* fieldLabel(const QString& text,QWidget* parent){auto* label=new QLabel(text,parent);Ui::role(label,"caption");return label;}
QWidget* column(QWidget* parent,QVBoxLayout*& layout){auto* widget=new QWidget(parent);layout=new QVBoxLayout(widget);layout->setContentsMargins(0,0,0,0);layout->setSpacing(Ui::space(Space::s));return widget;}
// Tool names in editor.view, in Tool order.
const QStringList toolNames{"select","redact","blur","crop","arrow","rectangle","highlight","text","step","pen","ellipse","line","copyText","style"};
QJsonValue rectValue(const std::optional<QRect>& r){if(!r)return QJsonValue();return QJsonArray{r->x(),r->y(),r->width(),r->height()};}
bool integerIn(const QJsonValue& value,double minimum,double maximum,int& out){
 if(!value.isDouble())return false;const double number=value.toDouble();
 if(!std::isfinite(number)||std::floor(number)!=number||number<minimum||number>maximum)return false;
 out=int(number);return true;
}
}

EditorWindow::EditorWindow(editor::EditDocument& doc,QWidget* parent):QWidget(parent),doc_(doc){
 Ui::styleWindow(*this);
 setObjectName("putgdWindow");setWindowTitle("Edit image — put.gd");setWindowIcon(Ui::icon("brand"));
 setAttribute(Qt::WA_DeleteOnClose,false);
 chooser_=new FileDialog(this);
 text_=new EditorTextRecognition(doc_,this);
 auto* root=new QVBoxLayout(this);root->setContentsMargins(0,0,0,0);root->setSpacing(0);
 // The document header: the truthful draft state, not a claim about the source.
 header_=Ui::WindowHeader::install(this,root);header_->setDocument("Edit image","Not uploaded");

 auto* toolbar=new Ui::Surface("bg",Qt::BottomEdge,this);toolbar->setObjectName("editorToolbar");toolbar->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);toolbar->installEventFilter(this);
 // Tools at the left, history and zoom at the right; when even folded tools
 // and no gaps cannot hold both on one row (large text, a narrow window),
 // history and zoom move to a second row instead of squeezing a label.
 toolbarGrid_=new QGridLayout(toolbar);toolbarGrid_->setContentsMargins(Ui::space(Space::m),Ui::space(Space::s),Ui::space(Space::m),Ui::space(Space::s));toolbarGrid_->setHorizontalSpacing(0);toolbarGrid_->setVerticalSpacing(Ui::space(Space::xs));
 auto* tools=new QHBoxLayout;tools->setContentsMargins(0,0,0,0);tools->setSpacing(Ui::space(Space::xs));toolsLayout_=tools;
 auto* history=new QHBoxLayout;history->setContentsMargins(0,0,0,0);history->setSpacing(Ui::space(Space::xs));actionsLayout_=history;
 toolbarGrid_->addLayout(tools,0,0);toolbarGrid_->setColumnStretch(1,1);toolbarGrid_->addLayout(history,0,2);
 tools_=new QButtonGroup(this);tools_->setExclusive(true);
 // The tool is Redact; the marks it makes are redactions.
 struct Entry{Tool tool;const char* icon;const char* label;const char* key;const char* detail;};
 const Entry entries[]={{Tool::Select,"select","Select and move","V",""},{Tool::Crop,"crop","Crop","C",""},
  // #1893: Style, beside Crop: like it, it changes the document, not a mark.
  {Tool::Style,"capture-style","Style","S","padding, a backdrop, rounded corners and a shadow"},
  {Tool::Arrow,"annotate-arrow","Arrow","A",""},{Tool::Line,"line","Line","L",""},{Tool::Rectangle,"rectangle","Rectangle","R",""},{Tool::Ellipse,"ellipse","Ellipse","E",""},
  {Tool::Pen,"pen","Pen","P",""},{Tool::Highlight,"highlight","Highlight","H",""},{Tool::Text,"text","Text","T",""},{Tool::Step,"step","Step number","N",""},
  {Tool::Redact,"redact","Redact","X","covers with an opaque redaction"},{Tool::Blur,"blur","Blur","B","a visual effect only"},
  // #1428: Copy text, after Blur behind its own separator.
  {Tool::CopyText,"copy-text","Copy text","G","reads the text in the image"}};
 for(const auto& entry:entries){
  if(entry.tool==Tool::Redact||entry.tool==Tool::CopyText){tools->addSpacing(Ui::space(Space::xs));tools->addWidget(separator(toolbar));tools->addSpacing(Ui::space(Space::xs));}
  auto* button=new QToolButton(toolbar);button->setCheckable(true);button->setIcon(Ui::icon(entry.icon));button->setProperty("uiIconBase",QSize(20,20));button->setIconSize(QSize(Ui::scaled(20),Ui::scaled(20)));button->setFixedHeight(Ui::scaled(36));button->setFocusPolicy(Qt::StrongFocus);
  const auto label=QString::fromUtf8(entry.label),detail=QString::fromUtf8(entry.detail);
  button->setText(entry.tool==Tool::Select?QStringLiteral("Select"):label);button->setToolButtonStyle(Qt::ToolButtonIconOnly);button->setFixedWidth(Ui::scaled(36));
  button->setToolTip(detail.isEmpty()?QString("%1 (%2)").arg(label,entry.key):QString("%1 (%2) — %3").arg(label,entry.key,detail));button->setAccessibleName(label);button->setAccessibleDescription(detail);
  Ui::role(button,"quiet");tools_->addButton(button,int(entry.tool));toolButtons_.insert(int(entry.tool),button);tools->addWidget(button);
  auto* shortcut=new QShortcut(QKeySequence(QString::fromLatin1(entry.key)),this);shortcut->setContext(Qt::WindowShortcut);
  connect(shortcut,&QShortcut::activated,this,[this,tool=entry.tool]{if(!busy_&&!canvas_->editingText())selectTool(tool);});
 }
 history->addSpacing(Ui::space(Space::xs));actionsSeparator_=separator(toolbar);history->addWidget(actionsSeparator_);history->addSpacing(Ui::space(Space::xs));
 auto toolButton=[&](const char* icon,const QString& tip){auto* button=new QToolButton(toolbar);button->setIcon(Ui::icon(icon));button->setProperty("uiIconBase",QSize(20,20));button->setIconSize(QSize(Ui::scaled(20),Ui::scaled(20)));button->setFixedSize(Ui::scaled(36),Ui::scaled(36));button->setFocusPolicy(Qt::StrongFocus);button->setToolTip(tip);button->setAccessibleName(tip);Ui::role(button,"quiet");history->addWidget(button);return button;};
 undo_=toolButton("undo","Undo (Ctrl+Z)");redo_=toolButton("redo","Redo (Ctrl+Shift+Z)");
 history->addSpacing(Ui::space(Space::xs));history->addWidget(separator(toolbar));history->addSpacing(Ui::space(Space::xs));
 // One zoom menu reports the live percentage; document size stays in the footer.
 status_=new QToolButton(toolbar);status_->setObjectName("editorZoom");status_->setAccessibleName("Zoom");status_->setToolButtonStyle(Qt::ToolButtonTextOnly);status_->setPopupMode(QToolButton::InstantPopup);status_->setFocusPolicy(Qt::StrongFocus);Ui::role(status_,"quiet");history->addWidget(status_);
 auto* zoomMenu=new QMenu(status_);auto* zoomChoices=new QActionGroup(zoomMenu);zoomChoices->setExclusive(true);
 for(const int percent:{0,50,100,200}){auto* action=zoomMenu->addAction(percent?QString("%1%").arg(percent):QStringLiteral("Fit"));action->setCheckable(true);action->setData(percent);zoomChoices->addAction(action);connect(action,&QAction::triggered,this,[this,percent]{if(percent)canvas_->setZoom(percent/100.0);else canvas_->fit();});}
 zoomMenu->addSeparator();
 for(const bool in:{true,false}){auto* action=zoomMenu->addAction(in?"Zoom in":"Zoom out");auto keys=QKeySequence::keyBindings(in?QKeySequence::ZoomIn:QKeySequence::ZoomOut);if(in&&!keys.contains(QKeySequence("Ctrl+=")))keys.append(QKeySequence("Ctrl+="));action->setShortcuts(keys);connect(action,&QAction::triggered,this,[this,in]{canvas_->zoomBy(in?1.25:0.8);});}
 zoomMenu->actions()[0]->setShortcut(QKeySequence("Ctrl+0"));
 zoomMenu->actions()[2]->setShortcut(QKeySequence("Ctrl+1"));
 // Share each action between the menu and the editor window. A separate
 // WindowShortcut plus a menu WidgetShortcut becomes ambiguous when Qt
 // restores focus to the menu's tool button, even after the popup closes.
 for(auto* action:zoomMenu->actions())if(!action->shortcuts().isEmpty()){action->setShortcutContext(Qt::WindowShortcut);addAction(action);}
 status_->setMenu(zoomMenu);
 root->addWidget(toolbar);

 // The canvas owns the space; the properties panel beside it keeps one width
 // whatever the tool, so the canvas never moves under the pointer.
 splitter_=new QSplitter(Qt::Horizontal,this);splitter_->setObjectName("editorSplitter");splitter_->setChildrenCollapsible(false);splitter_->setHandleWidth(1);
 canvasViewport_=new QWidget(splitter_);canvasViewport_->setObjectName("editorCanvasViewport");auto* canvasLayout=new QVBoxLayout(canvasViewport_);canvasLayout->setContentsMargins(0,0,0,0);canvasLayout->setSpacing(0);
 canvas_=new editor::EditorCanvas(doc_,canvasViewport_);canvasLayout->addWidget(canvas_);splitter_->addWidget(canvasViewport_);canvas_->installEventFilter(this);canvas_->setTextLayer(&text_->layer());
 // Properties scroll independently; document metadata stays in the footer.
 auto* side=new Ui::Surface("surface",{},splitter_);side->setObjectName("editorSide");side->setMinimumWidth(std::max(240,Ui::scaled(180)));
 propertiesWidth_=std::max(240,Ui::scaled(180));
 side_=side;side->installEventFilter(this);
 auto* sideLayout=new QVBoxLayout(side);sideLayout->setContentsMargins(0,0,0,0);sideLayout->setSpacing(0);
 auto* panel=new Ui::Surface("surface",{},side);panel->setObjectName("editorOptions");
 auto* panelScroll=new QScrollArea(side);propertiesScroll_=panelScroll;panelScroll->setObjectName("editorPropertiesScroll");panelScroll->setWidgetResizable(true);panelScroll->setFocusPolicy(Qt::NoFocus);panelScroll->setFrameShape(QFrame::NoFrame);panelScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);panelScroll->setWidget(panel);sideLayout->addWidget(panelScroll,1);
 splitter_->addWidget(side);splitter_->setStretchFactor(0,1);splitter_->setStretchFactor(1,0);
 connect(splitter_,&QSplitter::splitterMoved,this,[this]{propertiesWidth_=side_->width();});
 auto* properties=new QVBoxLayout(panel);properties_=properties;properties->setContentsMargins(Ui::space(Space::l),Ui::space(Space::l),Ui::space(Space::l),Ui::space(Space::l));properties->setSpacing(Ui::space(Space::m));
 QVBoxLayout* headingLayout=nullptr;auto* heading=column(panel,headingLayout);properties->addWidget(heading);
 auto* titleRow=new QHBoxLayout;titleRow->setContentsMargins(0,0,0,0);titleRow->setSpacing(Ui::space(Space::s));headingLayout->addLayout(titleRow);
 toolTitle_=new Ui::ElidedLabel(heading);toolTitle_->setElideMode(Qt::ElideRight);toolTitle_->setObjectName("editorToolTitle");toolTitle_->setWordWrap(false);Ui::role(toolTitle_,"sectionTitle");titleRow->addWidget(toolTitle_,1);
 toolHelp_=new QLabel(heading);toolHelp_->setObjectName("editorToolHelp");toolHelp_->setMinimumWidth(0);toolHelp_->setWordWrap(true);Ui::role(toolHelp_,"muted");headingLayout->addWidget(toolHelp_);
 blurWarning_=Ui::blurWarning([this]{if(!busy_){selectTool(Tool::Redact);}},panel);
 blurWarning_->findChild<QLabel*>("blurWarningText")->setObjectName("editorToolWarning");properties->addWidget(blurWarning_);
 hint_=Ui::label({},"secondary",panel);hint_->setWordWrap(true);properties->addWidget(hint_);
 // A selected Quick redact mark says which kind of text suggested it (#1618).
 suggested_=Ui::label({},"secondary",panel);suggested_->setObjectName("editorSuggested");suggested_->setWordWrap(true);suggested_->hide();properties->addWidget(suggested_);
 // Without a pointer: the tool's mark in the middle of the image (Insert).
 placeRow_=new QWidget(panel);auto* placeLayout=new QHBoxLayout(placeRow_);placeLayout->setContentsMargins(0,0,0,0);placeLayout->setSpacing(Ui::space(Space::s));
 place_=new QPushButton(placeRow_);place_->setObjectName("placeMark");place_->setProperty("uiIconBase",QSize(20,20));place_->setIconSize(QSize(Ui::scaled(20),Ui::scaled(20)));placeLayout->addWidget(place_);placeLayout->addStretch(1);properties->addWidget(placeRow_);
 // Divides the tool's help from its options; hidden when it has none.
 optionsRule_=new QWidget(panel);optionsRule_->setObjectName("editorOptionsRule");auto* ruleLayout=new QVBoxLayout(optionsRule_);ruleLayout->setContentsMargins(0,0,0,Ui::space(Space::m));ruleLayout->setSpacing(0);ruleLayout->addWidget(Ui::rule(optionsRule_));properties->addWidget(optionsRule_);
 QVBoxLayout* layout=nullptr;
 colorRow_=column(panel,layout);layout->addWidget(fieldLabel("Color",colorRow_));
 swatchGroup_=new QButtonGroup(this);
 auto* grid=new QGridLayout;grid->setContentsMargins(0,0,0,0);grid->setSpacing(Ui::space(Space::xs));int cell=0;
 const QStringList colorNames{"Red","Orange","Yellow","Green","Blue","White","Black"};
 for(const auto& color:{QColor(220,38,38),QColor(245,158,11),QColor(250,204,21),QColor(34,197,94),QColor(59,130,246),QColor(255,255,255),QColor(17,19,23)}){
  auto* swatch=makeSwatch(color);swatch->setObjectName("colorSwatch"+colorNames[cell]);swatch->setAccessibleName("Color "+colorNames[cell]);swatch->setAccessibleDescription(color.name());swatch->setToolTip(colorNames[cell]+" ("+color.name()+")");swatches_.append(swatch);grid->addWidget(swatch,cell/4,cell%4);++cell;
 }
 // The custom picker follows the palette, making two even rows of four rather
 // than six and an orphaned pair; recent custom colours get their own row.
 customColor_=new Swatch({},colorRow_,true);customColor_->setObjectName("customColor");
 customColor_->setToolTip("Custom color");customColor_->setAccessibleName("Custom color");grid->addWidget(customColor_,cell/4,cell%4);
 grid->setColumnStretch(4,1);layout->addLayout(grid);
 connect(customColor_,&QToolButton::clicked,this,&EditorWindow::pickCustomColor);
 auto* recent=new QWidget(colorRow_);recentLayout_=new QHBoxLayout(recent);recentLayout_->setContentsMargins(0,0,0,0);recentLayout_->setSpacing(Ui::space(Space::xs));recentLayout_->addStretch(1);recent->setObjectName("recentColors");recent->setAccessibleName("Recent custom colors");layout->addWidget(recent);
 properties->addWidget(colorRow_);
 widthRow_=column(panel,layout);layout->addWidget(fieldLabel("Line width",widthRow_));
 width_=Ui::choiceBox(widthRow_);width_->setObjectName("lineWidth");width_->addItem("Thin · 2 px",2);width_->addItem("Medium · 4 px",4);width_->addItem("Thick · 8 px",8);width_->setCurrentIndex(1);width_->setAccessibleName("Line width");layout->addWidget(width_);
 properties->addWidget(widthRow_);
 auto* blurRadius=new BlurRadius(editor::EditDocument::minimumBlur,editor::EditDocument::maximumBlur,12,false,panel);blurRow_=blurRadius;Ui::role(blurRadius->label(),"caption");blurRadius->layout()->setSpacing(Ui::space(Space::s));
 strength_=blurRadius->slider();strength_->setObjectName("blurStrength");strengthValue_=blurRadius->spinBox();strengthValue_->setObjectName("blurStrengthValue");
 // Blur or Pixelate: two looks of the same visual effect, neither a redaction.
 blurStyleRow_=column(panel,layout);blurStyleRow_->setObjectName("editorBlurStyle");layout->addWidget(fieldLabel("Style",blurStyleRow_));
 {
  auto* track=new Ui::SegmentTrack(blurStyleRow_);track->setObjectName("blurStyleTrack");track->setAccessibleName("Blur style");auto* group=new QButtonGroup(this);group->setExclusive(true);
  blurStyle_=new QToolButton(track);blurStyle_->setObjectName("blurStyleBlur");blurStyle_->setText("Blur");blurStyle_->setAccessibleName("Blur style: blur");
  pixelateStyle_=new QToolButton(track);pixelateStyle_->setObjectName("blurStylePixelate");pixelateStyle_->setText("Pixelate");pixelateStyle_->setAccessibleName("Blur style: pixelate");
  for(auto* button:{blurStyle_,pixelateStyle_}){button->setCheckable(true);button->setFocusPolicy(Qt::StrongFocus);group->addButton(button);track->addButton(button);}
  blurStyle_->setChecked(true);
  auto* row=new QHBoxLayout;row->setContentsMargins(0,0,0,0);row->addWidget(track);row->addStretch(1);layout->addLayout(row);
  connect(group,&QButtonGroup::buttonClicked,this,[this]{syncStyle(true);});
 }
 properties->addWidget(blurStyleRow_);
 properties->addWidget(blurRow_);
 // Step: the number the next click places, or the selected step's number.
 stepRow_=column(panel,layout);stepRow_->setObjectName("editorStep");stepLabel_=fieldLabel("Next number",stepRow_);layout->addWidget(stepLabel_);
 stepNumber_=new QSpinBox(stepRow_);stepNumber_->setObjectName("stepNumber");stepNumber_->setRange(1,editor::maximumStepNumber);stepNumber_->setKeyboardTracking(false);stepNumber_->setAccessibleName("Next step number");stepLabel_->setBuddy(stepNumber_);
 {auto* row=new QHBoxLayout;row->setContentsMargins(0,0,0,0);row->addWidget(stepNumber_);row->addStretch(1);layout->addLayout(row);}
 properties->addWidget(stepRow_);
 connect(stepNumber_,&QSpinBox::valueChanged,this,[this](int value){
  if(syncingStep_||busy_)return;
  if(const auto* shape=doc_.find(canvas_->selected());shape&&shape->kind==editor::Shape::Kind::Step){auto next=*shape;next.number=value;if(doc_.replace(next)){canvas_->documentChanged();refresh();}return;}
  canvas_->setNextStepNumber(value);refresh();
 });
 textRow_=column(panel,layout);layout->addWidget(fieldLabel("Font",textRow_));
 font_=new QFontComboBox(textRow_);font_->setObjectName("textFont");font_->setAccessibleName("Font");font_->setCurrentFont(QFont());font_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);font_->setMinimumContentsLength(8);layout->addWidget(font_);
 layout->addWidget(fieldLabel("Size in image pixels",textRow_));
 // Image pixels; applied when typing finishes (Enter or leaving the field) or on each step.
 textSize_=new QSpinBox(textRow_);textSize_->setObjectName("textSize");textSize_->setRange(editor::minimumFontPixels,editor::maximumFontPixels);textSize_->setValue(28);textSize_->setSuffix(" px");
 textSize_->setKeyboardTracking(false);textSize_->setAccessibleName("Text size in image pixels");layout->addWidget(textSize_);
 properties->addWidget(textRow_);
 // Copy text (#1428): what recognition found, the two copies, what it reads, codes.
 copyTextRow_=column(panel,layout);copyTextRow_->setObjectName("editorCopyText");
 textState_=new QLabel(copyTextRow_);textState_->setObjectName("editorTextState");Ui::role(textState_,"body");textState_->setWordWrap(true);layout->addWidget(textState_);
 {
  // One above the other: side by side they are wider than the panel.
  auto* buttons=new QVBoxLayout;buttons->setContentsMargins(0,0,0,0);buttons->setSpacing(Ui::space(Space::xs));
  copySelection_=new QPushButton("Copy selection",copyTextRow_);copySelection_->setObjectName("editorCopySelection");copySelection_->setToolTip("Copy selected text (Ctrl+C)");Ui::role(copySelection_,"primary");
  copyAllText_=new QPushButton("Copy all text",copyTextRow_);copyAllText_->setObjectName("editorCopyAllText");copyAllText_->setToolTip("Copy all recognized text and QR code content");Ui::role(copyAllText_,"quiet");
  buttons->addWidget(copySelection_,0,Qt::AlignLeft);buttons->addWidget(copyAllText_,0,Qt::AlignLeft);layout->addLayout(buttons);
  connect(copySelection_,&QPushButton::clicked,this,[this]{copyRecognizedText(false);});connect(copyAllText_,&QPushButton::clicked,this,[this]{copyRecognizedText(true);});
 }
 textFeedback_=new QLabel(copyTextRow_);textFeedback_->setObjectName("editorTextFeedback");textFeedback_->setWordWrap(true);textFeedback_->hide();layout->addWidget(textFeedback_);
 textTruncated_=Ui::label("The text was cut at a size or time limit.","secondary",copyTextRow_);textTruncated_->setObjectName("editorTextTruncated");textTruncated_->setWordWrap(true);textTruncated_->hide();layout->addWidget(textTruncated_);
 {auto* note=Ui::label("Reads the original image. Marks you added are not included, and Blur does not hide text from it.","secondary",copyTextRow_);note->setObjectName("editorTextNote");note->setWordWrap(true);layout->addWidget(note);}
 codesTitle_=fieldLabel("Codes",copyTextRow_);codesTitle_->setObjectName("editorCodesTitle");layout->addWidget(codesTitle_);
 codeList_=new QWidget(copyTextRow_);codeList_->setObjectName("editorCodes");codeLayout_=new QVBoxLayout(codeList_);codeLayout_->setContentsMargins(0,0,0,0);codeLayout_->setSpacing(Ui::space(Space::s));layout->addWidget(codeList_);
 properties->addWidget(copyTextRow_);
 // Quick redact (#1428) in the Redact tool's panel: suggestions, never a verdict.
 quickRedactRow_=column(panel,layout);quickRedactRow_->setObjectName("editorQuickRedact");
 {
  quickRedact_=new QToolButton(quickRedactRow_);quickRedact_->setObjectName("quickRedact");quickRedact_->setText("Quick redact");quickRedact_->setIcon(Ui::icon("redact"));quickRedact_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  quickRedact_->setPopupMode(QToolButton::InstantPopup);quickRedact_->setAccessibleName("Quick redact");quickRedact_->setToolTip("Add suggested Redact marks by type");Ui::role(quickRedact_,"quiet");
  auto* menu=new QMenu(quickRedact_);menu->setObjectName("quickRedactMenu");menu->setToolTipsVisible(true);
  for(const auto& category:editor::quickRedact::categories()){auto* action=menu->addAction(quickRedactLabel(category));action->setObjectName("quickRedact_"+category);if(category=="key")action->setToolTip(editor::quickRedact::keyShapes().join("\n"));if(category=="list")quickRedactList_=action;connect(action,&QAction::triggered,this,[this,category]{quickRedact({category});});}
  menu->addSeparator();auto* every=menu->addAction("All of these");every->setObjectName("quickRedact_all");connect(every,&QAction::triggered,this,[this]{quickRedact(editor::quickRedact::categories());});
  // Takes back every suggestion still there (one undo step); marks you drew stay.
  menu->addSeparator();quickRedactRemove_=menu->addAction("Remove suggestions");quickRedactRemove_->setObjectName("quickRedact_remove");quickRedactRemove_->setToolTip("Clear Quick redact suggestions, keeping marks you drew");connect(quickRedactRemove_,&QAction::triggered,this,[this]{removeSuggestions();});
  setRedactList({},{});
  quickRedact_->setMenu(menu);
  auto* row=new QHBoxLayout;row->setContentsMargins(0,0,0,0);row->addWidget(quickRedact_);row->addStretch(1);layout->addLayout(row);
  auto* caution=Ui::label("Check the image before uploading. Detection can miss text.","secondary",quickRedactRow_);caution->setObjectName("quickRedactCaution");caution->setWordWrap(true);layout->addWidget(caution);
  quickRedactResult_=new QLabel(quickRedactRow_);quickRedactResult_->setObjectName("quickRedactResult");quickRedactResult_->setWordWrap(true);Ui::role(quickRedactResult_,"body");quickRedactResult_->hide();layout->addWidget(quickRedactResult_);
 }
 properties->addWidget(quickRedactRow_);
 connect(text_,&EditorTextRecognition::changed,this,[this]{canvas_->update();refresh();});
 connect(canvas_,&editor::EditorCanvas::textSelectionChanged,this,[this]{refresh();});
 connect(canvas_,&editor::EditorCanvas::textCopyRequested,this,[this]{copyRecognizedText(false);});
 // #1893: Style: on or off for this image, the look and the padding. Backdrop colours, corners and
 // shadow come from Settings → Capture → Capture style (Saved look) or a preset.
 styleRow_=column(panel,layout);styleRow_->setObjectName("editorStyle");
 {
  styleEnabled_=new QCheckBox("Style this image",styleRow_);styleEnabled_->setObjectName("editorStyleEnabled");styleEnabled_->setAccessibleDescription("Add a backdrop, padding, rounded corners, and a shadow");layout->addWidget(styleEnabled_);
  layout->addWidget(fieldLabel("Look",styleRow_));
  styleLook_=Ui::choiceBox(styleRow_);styleLook_->setObjectName("editorStyleLook");styleLook_->setAccessibleName("Style look");
  for(const auto& preset:CaptureStyle::presets())styleLook_->addItem(preset.label,preset.id);
  layout->addWidget(styleLook_);
  layout->addWidget(fieldLabel("Padding",styleRow_));
  auto* track=new Ui::SegmentTrack(styleRow_);track->setObjectName("editorStylePaddingTrack");track->setAccessibleName("Style padding");auto* group=new QButtonGroup(this);group->setExclusive(true);
  const QStringList labels{"Small","Medium","Large"};
  for(int index=0;index<3;++index){auto* button=new QToolButton(track);button->setObjectName("editorStylePadding"+labels[index]);button->setText(labels[index]);button->setAccessibleName("Padding: "+labels[index].toLower());button->setCheckable(true);button->setFocusPolicy(Qt::StrongFocus);group->addButton(button,index);track->addButton(button);stylePadding_[index]=button;}
  auto* row=new QHBoxLayout;row->setContentsMargins(0,0,0,0);row->addWidget(track);row->addStretch(1);layout->addLayout(row);
  styleSize_=Ui::label({},"secondary",styleRow_);styleSize_->setObjectName("editorStyleSize");styleSize_->setWordWrap(true);layout->addWidget(styleSize_);
  connect(styleEnabled_,&QCheckBox::toggled,this,[this](bool on){if(!syncingStyle_)changeCaptureStyle([on](CaptureStyle& style){style.enabled=on;});});
  connect(styleLook_,&QComboBox::activated,this,[this](int index){
   if(syncingStyle_)return;const auto id=styleLook_->itemData(index).toString();
   changeCaptureStyle([this,id](CaptureStyle& style){const auto padding=style.padding;style=id=="saved"?savedStyle_:CaptureStyle::preset(id);style.enabled=true;if(id!="saved"&&id!="clear")style.padding=padding;});
  });
  connect(group,&QButtonGroup::idClicked,this,[this](int index){if(!syncingStyle_)changeCaptureStyle([index](CaptureStyle& style){style.padding=CaptureStyle::paddingNames().value(index);style.enabled=true;});});
 }
 properties->addWidget(styleRow_);
 cropRow_=new QWidget(panel);auto* cropButtons=new QHBoxLayout(cropRow_);cropButtons->setContentsMargins(0,0,0,0);cropButtons->setSpacing(Ui::space(Space::s));
 auto* applyCrop=new QPushButton("Apply crop",cropRow_);applyCrop->setObjectName("applyCrop");applyCrop->setToolTip("Apply crop (Enter)");Ui::role(applyCrop,"primary");cropButtons->addWidget(applyCrop);
 auto* cancelCrop=new QPushButton("Cancel crop",cropRow_);cancelCrop->setObjectName("cancelCrop");cancelCrop->setToolTip("Cancel crop (Esc)");Ui::role(cancelCrop,"quiet");cropButtons->addWidget(cancelCrop);
 // Position and size of the selected mark or the pending crop, in image
 // pixels; each committed value is one undoable step.
 geometryRow_=column(panel,layout);geometryRow_->setObjectName("editorGeometry");
 geometryDisclosure_=new QToolButton(geometryRow_);geometryDisclosure_->setObjectName("editorGeometryDisclosure");geometryDisclosure_->setProperty("uiFlush",true);geometryDisclosure_->setText("Position and size");Ui::disclosure(geometryDisclosure_);layout->addWidget(geometryDisclosure_,0,Qt::AlignLeft);
 geometryTitle_=fieldLabel("Crop area",geometryRow_);layout->addWidget(geometryTitle_);
 geometryFields_=new QWidget(geometryRow_);geometryFields_->setObjectName("editorGeometryFields");
 auto* fields=new QGridLayout(geometryFields_);fields->setContentsMargins(0,0,0,0);fields->setHorizontalSpacing(Ui::space(Space::s));fields->setVerticalSpacing(Ui::space(Space::s));
 const char* names[4]={"geometryX","geometryY","geometryWidth","geometryHeight"};
 for(int i=0;i<4;++i){
  geometryLabels_[i]=fieldLabel({},geometryFields_);
  auto* box=new QSpinBox(geometryFields_);box->setObjectName(names[i]);box->setSuffix(" px");box->setKeyboardTracking(false);box->setMinimumWidth(Ui::scaled(64));
  geometryLabels_[i]->setBuddy(box);geometry_[i]=box;
  fields->addWidget(geometryLabels_[i],(i/2)*2,i%2);fields->addWidget(box,(i/2)*2+1,i%2);
  connect(box,&QSpinBox::valueChanged,this,&EditorWindow::applyGeometry);
 }
 layout->addWidget(geometryFields_);properties->addWidget(geometryRow_);properties->addWidget(cropRow_);
 connect(geometryDisclosure_,&QToolButton::toggled,this,[this]{syncGeometry();});
 remove_=new QPushButton(heading);remove_->setObjectName("removeMark");remove_->setAccessibleName("Remove mark");remove_->setProperty("uiDensity","compact");remove_->setIcon(Ui::icon("remove"));remove_->setProperty("uiIconBase",QSize(20,20));remove_->setIconSize(QSize(Ui::scaled(20),Ui::scaled(20)));remove_->setFixedSize(Ui::scaled(28),Ui::scaled(28));remove_->setToolTip("Remove selected mark (Delete)");Ui::role(remove_,"quiet");remove_->installEventFilter(this);titleRow->addWidget(remove_,0,Qt::AlignTop);
 auto* keyboardHelp=new QToolButton(panel);keyboardHelp->setObjectName("editorKeyboardHelp");keyboardHelp->setProperty("uiFlush",true);keyboardHelp->setText("Keyboard help");keyboardHelp->setAccessibleName("Keyboard help");Ui::disclosure(keyboardHelp);properties->addWidget(keyboardHelp,0,Qt::AlignLeft);
 auto* keyboardText=new KeyboardHelp(panel);keyboardText->setObjectName("editorKeyboardHelpText");
 const QList<QPair<QString,QString>> shortcuts{{"[ / ]","Cycle marks"},{"Arrows","Move"},{"Shift+arrows","Resize"},{"Ctrl+arrows","Move 10 px"},{"Ctrl+Shift+arrows","Resize 10 px"},{"Alt+arrows","Pan canvas"},{"Alt+Shift+arrows","Pan canvas farther"},{"Tab","Focus properties"},{"Delete","Remove mark"},{"Escape","Deselect"},{"Insert","Place mark"},{"Shift+drag","Square, circle, 45° line"},{"Ctrl+Z","Undo"},{"Ctrl+Shift+Z","Redo"},{"Ctrl+Shift+C","Copy image"}};
 QStringList keyboardSummary;
 for(const auto& [keys,description]:shortcuts){auto* key=new QLabel(keys,keyboardText);Ui::setMono(key);Ui::role(key,"secondary");auto* meaning=new QLabel(description,keyboardText);meaning->setWordWrap(true);Ui::role(meaning,"secondary");keyboardText->addShortcut(key,meaning);keyboardSummary.append(keys+": "+description);}
 keyboardText->setAccessibleName("Keyboard help");keyboardText->setProperty("text",keyboardSummary.join('\n'));keyboardText->hide();properties->addWidget(keyboardText);
 connect(keyboardHelp,&QToolButton::toggled,this,[this,keyboardHelp,keyboardText](bool open){keyboardText->setVisible(open);if(open)QTimer::singleShot(0,this,[this,keyboardHelp,keyboardText]{if(!keyboardText->isHidden())propertiesScroll_->verticalScrollBar()->setValue(keyboardHelp->mapTo(propertiesScroll_->widget(),QPoint()).y()-Ui::space(Ui::Space::m));});});
 for(auto* control:panel->findChildren<QWidget*>())if(qobject_cast<QPushButton*>(control)||qobject_cast<QComboBox*>(control)||qobject_cast<QSpinBox*>(control)||(qobject_cast<QToolButton*>(control)&&!dynamic_cast<Swatch*>(control))){control->setProperty("uiDensity","compact");if(qobject_cast<QSpinBox*>(control))control->setMinimumHeight(32);control->style()->unpolish(control);control->style()->polish(control);}
 properties->addStretch(1);
 root->addWidget(splitter_,1);

 // The summary shares the action row; save feedback overlays the canvas.
 auto* footer=new Ui::Surface("bg",Qt::TopEdge,this);footer->setObjectName("editorFooter");footer->setMinimumHeight(Ui::scaled(56));
 footerGrid_=new QGridLayout(footer);footerGrid_->setContentsMargins(Ui::space(Space::l),Ui::space(Space::s),Ui::space(Space::l),Ui::space(Space::s));footerGrid_->setHorizontalSpacing(Ui::space(Space::s));footerGrid_->setVerticalSpacing(Ui::space(Space::xs));footerGrid_->setColumnStretch(0,1);
 auto* document=new QWidget(footer);document->setObjectName("editorDocument");document->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);document_=document;
 auto* documentLayout=new QHBoxLayout(document);documentLayout->setContentsMargins(0,0,0,0);documentLayout->setSpacing(0);
 documentInfo_=new QLabel(document);documentInfo_->setObjectName("editorDocumentInfo");Ui::role(documentInfo_,"meta");documentLayout->addWidget(documentInfo_);documentLayout->addSpacing(Ui::space(Space::l));footerGrid_->addWidget(document,2,0);
 auto* warning=new Ui::Banner(Ui::Banner::Tone::Warning,{},footer);notice_=warning;notice_->setObjectName("editorPreparationFailure");auto* noticeContent=new QWidget(notice_);warning->setContentWidget(noticeContent);
 auto* noticeLayout=new QGridLayout(noticeContent);noticeLayout->setContentsMargins(0,0,0,0);noticeLayout->setSpacing(Ui::space(Space::s));
 noticeText_=new QLabel(notice_);noticeText_->setObjectName("editorPreparationReason");noticeText_->setTextFormat(Qt::PlainText);noticeText_->setWordWrap(true);Ui::role(noticeText_,"secondary");noticeLayout->addWidget(noticeText_,0,0,1,3);noticeLayout->setColumnStretch(2,1);
 keepEditing_=new QPushButton("Keep editing",notice_);keepEditing_->setObjectName("keepEditing");noticeLayout->addWidget(keepEditing_,1,0);
 saveInstead_=new QPushButton("Save a copy…",notice_);saveInstead_->setObjectName("saveCopyInstead");saveInstead_->setToolTip("Save edited image to a file (Ctrl+S)");noticeLayout->addWidget(saveInstead_,1,1);
 notice_->hide();footerGrid_->addWidget(notice_,0,0,1,5);
 // Elision keeps the actions reachable; keyboard activation reveals the name.
 destination_=new Ui::ElidedLabel(document);destination_->setElideMode(Qt::ElideRight);destination_->setRevealable(true);destination_->setObjectName("editorDestination");destination_->setTextFormat(Qt::PlainText);Ui::role(destination_,"meta");
 destination_->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);destination_->setFocusPolicy(Qt::StrongFocus);destination_->hide();documentLayout->addWidget(destination_);
 documentLayout->addStretch();
 auto* uploadGroup=new QWidget(footer);uploadGroup->setObjectName("editorUploadGroup");auto* uploadRow=new QHBoxLayout(uploadGroup);uploadRow->setContentsMargins(Ui::space(Space::l)-Ui::space(Space::s),0,0,0);uploadRow->setSpacing(Ui::space(Space::s));
 sharingMode_=new QLabel(uploadGroup);sharingMode_->setObjectName("editorSharingMode");sharingMode_->setTextFormat(Qt::PlainText);Ui::role(sharingMode_,"body");sharingMode_->setSizePolicy(QSizePolicy::Minimum,QSizePolicy::Preferred);sharingMode_->hide();uploadRow->addWidget(sharingMode_);
 // Saving locally never hides the separate upload destination.
 saveFeedback_=new Ui::Banner(Ui::Banner::Tone::Info,{},canvasViewport_);saveFeedback_->setObjectName("editorSaveFeedback");saveFeedback_->hide();saveFeedback_->installEventFilter(this);
 saveFeedbackTimer_=new QTimer(this);saveFeedbackTimer_->setObjectName("editorSaveFeedbackTimer");saveFeedbackTimer_->setSingleShot(true);
 connect(saveFeedbackTimer_,&QTimer::timeout,this,[this]{
  if(!saveFeedbackVisible_||lastSave_.value("status")!="saved"||(!lastShowInFolder_.isEmpty()&&!lastShowInFolder_.value("opened").toBool()))return;
  auto* focus=QApplication::focusWidget();bool hovered=saveFeedback_->underMouse();for(auto* child:saveFeedback_->findChildren<QWidget*>())hovered=hovered||child->underMouse();
  if(hovered||(focus&&(focus==saveFeedback_||saveFeedback_->isAncestorOf(focus)))){saveFeedbackTimer_->start(500);return;}
  dismissSavedCopy();
 });
 auto* feedbackContent=new QWidget(saveFeedback_);saveFeedback_->setContentWidget(feedbackContent);
 auto* feedback=new QGridLayout(feedbackContent);feedback->setContentsMargins(0,0,0,0);feedback->setSpacing(Ui::space(Space::s));feedback->setColumnStretch(0,1);
 auto* result=new QWidget(saveFeedback_);result->setObjectName("editorSaveResult");auto* resultRow=new QHBoxLayout(result);resultRow->setContentsMargins(0,0,0,0);resultRow->setSpacing(Ui::space(Space::s));
 auto* actions=new QWidget(saveFeedback_);actions->setObjectName("editorSaveActions");auto* recoveryRow=new QHBoxLayout(actions);recoveryRow->setContentsMargins(0,0,0,0);recoveryRow->setSpacing(Ui::space(Space::s));
 savedLabel_=new Ui::ElidedLabel(saveFeedback_);savedLabel_->setObjectName("editorSaved");savedLabel_->setTextFormat(Qt::PlainText);savedLabel_->setElideMode(Qt::ElideRight);savedLabel_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);savedLabel_->setContextMenuPolicy(Qt::ActionsContextMenu);savedLabel_->hide();resultRow->addWidget(savedLabel_,1);
 saveTarget_=new Ui::ElidedLabel(saveFeedback_);saveTarget_->setObjectName("editorSaveTarget");saveTarget_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);saveTarget_->setTextFormat(Qt::PlainText);saveTarget_->setElideMode(Qt::ElideMiddle);Ui::role(saveTarget_,"secondary");saveTarget_->hide();resultRow->addWidget(saveTarget_,1);
 saveError_=new Ui::ElidedLabel(saveFeedback_);saveError_->setObjectName("editorSaveError");saveError_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);saveError_->setTextFormat(Qt::PlainText);saveError_->setElideMode(Qt::ElideRight);saveError_->setRevealActionName("Save failure details");saveError_->setRevealTitle("Save to");Ui::role(saveError_,"body");saveError_->hide();resultRow->insertWidget(0,saveError_,2);
 // Details leads the actions group in every result, never ahead of the message.
 auto* details=Ui::toolButton("Details",{},"quiet",saveFeedback_);details->setObjectName("editorSaveDetails");recoveryRow->insertWidget(0,details);connect(details,&QToolButton::clicked,this,[this]{if(auto* popup=saveError_->fullTextPopup();popup&&popup->isVisible())popup->close();else saveError_->reveal();});
 auto* copyPath=new QAction("Copy full path",savedLabel_);savedLabel_->addAction(copyPath);connect(copyPath,&QAction::triggered,this,&EditorWindow::copySavedPath);
 showFolder_=new QPushButton("Show in folder",saveFeedback_);showFolder_->setObjectName("showSavedCopy");showFolder_->hide();Ui::role(showFolder_,"quiet");recoveryRow->addWidget(showFolder_);
 copyPath_=new QPushButton("Copy path",saveFeedback_);copyPath_->setObjectName("copySavedPath");copyPath_->setToolTip("Copy full file path");copyPath_->hide();Ui::role(copyPath_,"quiet");recoveryRow->addWidget(copyPath_);
 savedMore_=Ui::toolButton({}, {},"quiet",saveFeedback_);savedMore_->setIcon(Ui::chromeIcon("more"));savedMore_->setObjectName("editorSavedMore");savedMore_->setAccessibleName("Saved copy actions");savedMore_->setToolTip("Saved copy actions");savedMore_->setPopupMode(QToolButton::InstantPopup);savedMore_->hide();recoveryRow->addWidget(savedMore_);
 auto* savedMenu=new QMenu(savedMore_);savedMore_->setMenu(savedMenu);
 auto* showSaved=savedMenu->addAction("Show in folder");showSaved->setObjectName("showSavedCopyMenu");connect(showSaved,&QAction::triggered,this,&EditorWindow::showSavedCopy);
 auto* copySaved=savedMenu->addAction("Copy path");copySaved->setObjectName("copySavedPathMenu");connect(copySaved,&QAction::triggered,this,&EditorWindow::copySavedPath);
 chooseAnother_=new QPushButton("Choose another location…",saveFeedback_);chooseAnother_->setObjectName("chooseAnotherLocation");chooseAnother_->hide();Ui::role(chooseAnother_,"quiet");recoveryRow->addWidget(chooseAnother_);
 saveFeedback_->setDismissible(true,"Dismiss save feedback");auto* dismiss=saveFeedback_->dismissButton();dismiss->setFocusPolicy(Qt::StrongFocus);dismiss->setObjectName("dismissEditorSavedCopy");connect(dismiss,&QToolButton::clicked,this,&EditorWindow::dismissSavedCopy);setTabOrder(showFolder_,copyPath_);setTabOrder(copyPath_,dismiss);setTabOrder(chooseAnother_,dismiss);
 setTabOrder(savedMore_,dismiss);
 for(auto* button:saveFeedback_->findChildren<QAbstractButton*>()){button->setProperty("uiDensity","compact");button->style()->unpolish(button);button->style()->polish(button);}
 folderError_=new Ui::ElidedLabel(saveFeedback_);folderError_->setText("Couldn't open the folder. Select Copy path instead.");folderError_->setObjectName("editorFolderError");folderError_->setTextFormat(Qt::PlainText);Ui::role(folderError_,"body");folderError_->hide();resultRow->addWidget(folderError_,1);feedback->addWidget(result,0,0);feedback->addWidget(actions,0,1);

 message_=new QLabel(result);message_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);message_->hide();resultRow->insertWidget(0,message_,1);
 // Copy image: the same sanitized PNG as Upload and Save a copy, on the
 // clipboard; nothing uploads and History gains nothing.
 copyImage_=new QPushButton("Copy image",footer);copyImage_->setObjectName("editorCopyImage");copyImage_->setIcon(Ui::icon("copy"));copyImage_->setProperty("uiIconBase",QSize(20,20));copyImage_->setIconSize(QSize(Ui::scaled(20),Ui::scaled(20)));Ui::role(copyImage_,"quiet");copyImage_->setToolTip("Copy edited image to clipboard (Ctrl+Shift+C)");copyImage_->setAccessibleDescription(copyImage_->toolTip());
 save_=new QPushButton("Save a copy…",footer);save_->setObjectName("editorSaveCopy");save_->setIcon(Ui::icon("save"));save_->setProperty("uiIconBase",QSize(20,20));save_->setIconSize(QSize(Ui::scaled(20),Ui::scaled(20)));save_->setToolTip("Save edited image to a file (Ctrl+S)");footerGrid_->addWidget(copyImage_,2,1);footerGrid_->addWidget(save_,2,2);
 cancel_=new QPushButton("Cancel",footer);cancel_->setObjectName("editorCancel");Ui::role(cancel_,"quiet");footerGrid_->addWidget(cancel_,2,3);
 // #2079: Seal, beside Upload: this upload only, starting from the destination's Seal links setting.
 seal_=new QPushButton("Seal",uploadGroup);seal_->setObjectName("editorSeal");seal_->setCheckable(true);seal_->setIcon(Ui::icon("lock"));seal_->setProperty("uiIconBase",QSize(20,20));seal_->setIconSize(QSize(Ui::scaled(20),Ui::scaled(20)));Ui::role(seal_,"quiet");seal_->setAutoDefault(false);seal_->hide();uploadRow->addWidget(seal_);
 connect(seal_,&QPushButton::toggled,this,[this](bool on){seal_->setAccessibleDescription(on?QStringLiteral("On"):QStringLiteral("Off"));if(!sealAvailable_&&!on)seal_->setEnabled(false);emit sealToggled(on);});
 upload_=new QPushButton("Upload",uploadGroup);upload_->setObjectName("editorUpload");upload_->setIcon(Ui::icon("upload"));upload_->setProperty("uiIconBase",QSize(20,20));upload_->setIconSize(QSize(Ui::scaled(20),Ui::scaled(20)));Ui::role(upload_,"primary");upload_->setToolTip("Upload edited image (Ctrl+Enter)");upload_->setAutoDefault(false);uploadRow->addWidget(upload_);footerGrid_->addWidget(uploadGroup,2,4);
 root->addWidget(footer);

 connect(tools_,&QButtonGroup::idClicked,this,[this](int id){selectTool(Tool(id));});
 connect(canvas_,&editor::EditorCanvas::toolRequested,this,[this](Tool tool){if(auto* button=toolButtons_.value(int(tool)))button->setChecked(true);refresh();});
 connect(canvas_,&editor::EditorCanvas::changed,this,&EditorWindow::refresh);
 connect(canvas_,&editor::EditorCanvas::viewChanged,this,&EditorWindow::refresh);
 connect(canvas_,&editor::EditorCanvas::selectionChanged,this,[this]{
  // A selected annotation shows, and is edited by, its own style controls.
  if(const auto* shape=doc_.find(canvas_->selected())){
   if(shape->kind==editor::Shape::Kind::Highlight)highlightColor_=shape->color;else if(shape->kind!=editor::Shape::Kind::Redact&&shape->kind!=editor::Shape::Kind::Blur)annotationColor_=shape->color;
   width_->blockSignals(true);textSize_->blockSignals(true);font_->blockSignals(true);strength_->blockSignals(true);
   width_->setCurrentIndex(std::max(0,width_->findData(shape->width)));textSize_->setValue(shape->fontPixels);strength_->setValue(shape->strength);strengthValue_->setValue(shape->strength);
   if(shape->kind==editor::Shape::Kind::Blur)(shape->pixelate?pixelateStyle_:blurStyle_)->setChecked(true);
   if(shape->kind==editor::Shape::Kind::Step)stepPixels_=shape->rect.width();
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
 // Platform bindings and explicit aliases can coincide; identical QShortcuts
 // would be ambiguous and neither would fire, so register each key once.
 QList<QKeySequence> bound;
 auto bind=[this,&bound](QList<QKeySequence> keys,auto slot){
  for(const auto& key:keys){if(key.isEmpty()||bound.contains(key))continue;bound.append(key);auto* shortcut=new QShortcut(key,this);shortcut->setContext(Qt::WindowShortcut);connect(shortcut,&QShortcut::activated,this,slot);}
 };
 auto standard=[](QKeySequence::StandardKey key,QList<QKeySequence> extra={}){return QKeySequence::keyBindings(key)+extra;};
 bind(standard(QKeySequence::Undo,{QKeySequence("Ctrl+Z")}),undo);bind(standard(QKeySequence::Redo,{QKeySequence("Ctrl+Shift+Z"),QKeySequence("Ctrl+Y")}),redo);
 bind(standard(QKeySequence::Save),[this]{if(!busy_)saveCopy();});
 // Upload is never plain Enter, which completes text entry and crops.
 auto submit=[this]{if(busy_||savingCopy_||copying_)return;finishPendingEdits();emit uploadRequested();};
 bind({QKeySequence("Ctrl+Return"),QKeySequence("Ctrl+Enter")},submit);
 connect(upload_,&QPushButton::clicked,this,submit);
 connect(save_,&QPushButton::clicked,this,&EditorWindow::saveCopy);
 connect(copyImage_,&QPushButton::clicked,this,&EditorWindow::requestCopy);
 bind({QKeySequence("Ctrl+Shift+C")},[this]{requestCopy();});
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

void EditorWindow::setSavedStyle(const CaptureStyle& style){
 savedStyle_=style;
 const int saved=styleLook_->findData("saved");
 // The look from Settings joins the list when it is not a preset.
 if(style.presetId()=="custom"){if(saved<0)styleLook_->addItem("Saved look","saved");}
 else if(saved>=0)styleLook_->removeItem(saved);
 syncCaptureStyle();
}
void EditorWindow::changeCaptureStyle(const std::function<void(CaptureStyle&)>& change){
 if(busy_)return;
 auto style=doc_.captureStyle();change(style);
 if(doc_.setCaptureStyle(style))documentEdited();else syncCaptureStyle();
}
void EditorWindow::syncCaptureStyle(){
 const auto& style=doc_.captureStyle();
 syncingStyle_=true;
 styleEnabled_->setChecked(style.enabled);
 auto id=style.presetId();if(id=="custom"&&style.sameLook(savedStyle_))id="saved";
 // A look that is neither a preset nor the saved one (an agent's own) shows no choice.
 styleLook_->setCurrentIndex(styleLook_->findData(id));
 const int padding=CaptureStyle::paddingNames().indexOf(style.padding);
 for(int index=0;index<3;++index)stylePadding_[index]->setChecked(index==padding);
 syncingStyle_=false;
 for(auto* control:std::initializer_list<QWidget*>{styleEnabled_,styleLook_,stylePadding_[0],stylePadding_[1],stylePadding_[2]})control->setEnabled(!busy_);
 const auto geometry=doc_.styleGeometry();const auto crop=doc_.crop().size();
 if(!style.enabled)styleSize_->setText("Off: the image is saved and uploaded as it is.");
 else if(!geometry.valid())styleSize_->setText("Too large to style: the image is saved and uploaded as it is.");
 else{
  auto text=QString("%1 × %2 px becomes %3 × %4 px.").arg(crop.width()).arg(crop.height()).arg(geometry.canvas.width()).arg(geometry.canvas.height());
  if(style.backdrop=="picture"&&doc_.stylePicture().isNull())text+=" The backdrop picture can't be read, so Graphite is used.";
  styleSize_->setText(text);
 }
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
 const auto color=QColorDialog::getColor(highlight?highlightColor_:annotationColor_,this,"Custom color — put.gd");
 if(!color.isValid())return;
 // Palette colors are already one click away; only custom colors are remembered.
 bool palette=false;for(auto* swatch:std::as_const(swatches_))palette=palette||swatch->property("swatchColor").value<QColor>()==color;
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
 // Retired swatches stay alive until deferred deletion, but must stop matching selectors immediately.
 for(auto* swatch:std::exchange(recentSwatches_,{})){swatchGroup_->removeButton(swatch);swatch->setObjectName({});swatch->hide();recentLayout_->removeWidget(swatch);swatch->deleteLater();}
 for(const auto& name:recentColors_){auto* swatch=makeSwatch(QColor(name));swatch->setObjectName("recentColorSwatch_"+name.mid(1));swatch->setToolTip("Recent custom color "+name);recentSwatches_.append(swatch);recentLayout_->insertWidget(recentLayout_->count()-1,swatch);}
 recentLayout_->parentWidget()->setVisible(!recentColors_.isEmpty());
}

void EditorWindow::setDestination(const QString& destination,bool links){
 auto summary=destination;QString mode;
 if(summary.startsWith("Private upload to ")){summary=summary.mid(18);mode="Private upload";}
 else if(summary.startsWith("Upload to ")){summary=summary.mid(10);if(!links)mode="Upload only";else{if(summary.endsWith(" · Shared link"))summary.chop(14);mode="Shared link";}}
 sharingMode_->setText(mode);sharingMode_->setAccessibleName(mode);
 destination_->setText(summary);destination_->setAccessibleName(summary);// No upload route (local archive, or no output): the summary already says what the button does.
 const auto description=(mode.isEmpty()?summary:"Upload edited image to "+summary+(mode=="Private upload"?QString(" as a private upload"):mode=="Upload only"?QString(" without a link"):QString(" with a shared link")))+" (Ctrl+Enter)";upload_->setToolTip(description);upload_->setAccessibleDescription(description);placeDestination();}

void EditorWindow::placeDestination(){
 if(!footerGrid_)return;
 save_->setVisible(notice_->isHidden());
 destination_->setVisible(!destination_->text().isEmpty());
 sharingMode_->setVisible(!sharingMode_->text().isEmpty());
 const auto margins=footerGrid_->contentsMargins();const int spacing=footerGrid_->horizontalSpacing();
 auto* uploadGroup=findChild<QWidget*>("editorUploadGroup");sharingMode_->setMinimumWidth(sharingMode_->sizeHint().width());
 int room=width()-margins.left()-margins.right()-uploadGroup->sizeHint().width()-spacing;
 for(auto* button:{copyImage_,save_,cancel_})if(!button->isHidden())room-=button->sizeHint().width()+spacing;
 auto* documentLayout=static_cast<QHBoxLayout*>(document_->layout());
 documentLayout->setContentsMargins(0,0,Ui::space(Space::xl)-spacing,0);documentLayout->invalidate();
 const bool above=document_->sizeHint().width()>room;
 destinationAbove_=above;
 documentLayout->setContentsMargins(0,0,above?0:Ui::space(Space::xl)-spacing,0);documentLayout->setStretch(2,above?1:0);documentLayout->setStretch(3,above?0:1);documentLayout->invalidate();
 const int slot=Ui::scaled(Ui::Metric::control);
 footerGrid_->setRowMinimumHeight(1,above?slot:0);document_->setMinimumHeight(slot);
 footerGrid_->removeWidget(document_);footerGrid_->addWidget(document_,above?1:2,0,1,above?5:1);
 // Reflow the footer before its new row can squeeze the sharing controls.
 footerGrid_->activate();layout()->activate();
 const bool feedback=saveFeedbackVisible_&&!busy_&&!savingCopy_;
 saveFeedback_->setVisible(feedback||!message_->isHidden());
 auto* result=findChild<QWidget*>("editorSaveResult");auto* actions=findChild<QWidget*>("editorSaveActions");
 result->setVisible(feedback||!message_->isHidden());actions->setVisible(feedback);
 const bool folderFailed=lastSave_.value("status")=="saved"&&!lastShowInFolder_.isEmpty()&&!lastShowInFolder_.value("opened").toBool();
 savedLabel_->setVisible(feedback&&lastSave_.value("status")=="saved"&&!folderFailed);
 saveError_->setVisible(feedback&&lastSave_.value("status")=="failed");saveTarget_->hide();folderError_->setVisible(feedback&&folderFailed);
 chooseAnother_->setVisible(feedback&&lastSave_.value("status")=="failed");
 findChild<QToolButton*>("editorSaveDetails")->setVisible(feedback&&(lastSave_.value("status")=="failed"||(!lastShowInFolder_.isEmpty()&&!lastShowInFolder_.value("opened").toBool())));
 const auto tone=!message_->isHidden()?(message_->property("visualRole")=="error"?Ui::Banner::Tone::Error:Ui::Banner::Tone::Info):folderFailed||lastSave_.value("status")=="failed"?Ui::Banner::Tone::Error:lastSave_.value("status")=="saved"&&savedGeneration_==doc_.generation()?Ui::Banner::Tone::Success:Ui::Banner::Tone::Info;
 saveFeedback_->setTone(tone);
 saveFeedback_->setAccessibleName(!message_->isHidden()?message_->text():folderFailed?folderError_->text():lastSave_.value("status")=="failed"?saveError_->accessibleDescription():savedLabel_->text());
 saveFeedback_->dismissButton()->setVisible(feedback||(!message_->isHidden()&&!busy_&&!savingCopy_));
 placeSaveFeedback();
}
void EditorWindow::placeTools(){
 auto apply=[&](bool allLabels){for(auto it=toolButtons_.cbegin();it!=toolButtons_.cend();++it){auto* button=it.value();const bool label=allLabels||it.key()==int(Tool::Redact)||it.key()==int(Tool::Blur);button->setMinimumWidth(0);button->setMaximumWidth(QWIDGETSIZE_MAX);button->setToolButtonStyle(label?Qt::ToolButtonTextBesideIcon:Qt::ToolButtonIconOnly);if(!label)button->setFixedWidth(Ui::scaled(36));}};
 auto* toolbar=findChild<QWidget*>("editorToolbar");const auto margins=toolbarGrid_->contentsMargins();
 const int room=toolbar->width()-margins.left()-margins.right();
 auto spacing=[&](int gap){toolsLayout_->setSpacing(gap);actionsLayout_->setSpacing(gap);toolsLayout_->invalidate();actionsLayout_->invalidate();};
 auto wrap=[&](bool below){
  if(toolbarWrapped_==below)return;
  toolbarWrapped_=below;toolbarGrid_->removeItem(actionsLayout_);
  if(below)toolbarGrid_->addLayout(actionsLayout_,1,0,1,3,Qt::AlignRight);else toolbarGrid_->addLayout(actionsLayout_,0,2);
  // A wrapped row starts at its own edge, without the dividing rule.
  actionsSeparator_->setVisible(!below);toolbarGrid_->invalidate();
 };
 // One row's width whatever the current placement: a wrapped row hides its rule.
 auto oneRow=[&]{return toolsLayout_->sizeHint().width()+actionsLayout_->sizeHint().width()+(actionsSeparator_->isHidden()?actionsSeparator_->sizeHint().width()+actionsLayout_->spacing():0);};
 // Measure every label in the current font before folding the ordinary tools.
 apply(true);spacing(Ui::space(Space::xs));
 if(oneRow()>room)apply(false);
 // Compact large text keeps both privacy labels before spending space on gaps.
 if(oneRow()>room)spacing(0);
 // Then history and zoom take a second row, and the gaps come back if they fit.
 const bool below=oneRow()>room;
 if(below){spacing(Ui::space(Space::xs));if(toolsLayout_->sizeHint().width()>room)spacing(0);}
 wrap(below);
}
void EditorWindow::placeSaveFeedback(){
 if(!saveFeedback_||saveFeedback_->isHidden())return;
 // Overlay controls follow their text height rather than decorative text scaling.
 for(auto* button:saveFeedback_->findChildren<QAbstractButton*>())button->setFixedHeight(std::max(32,button->fontMetrics().height()+2*int(Space::xs)));
 const int target=saveFeedback_->dismissButton()->height();saveFeedback_->dismissButton()->setFixedWidth(target);savedMore_->setFixedSize(target,target);
 const auto inset=saveFeedback_->layout()->contentsMargins();saveFeedback_->layout()->setContentsMargins(inset.left(),int(Space::xs),int(Space::m),int(Space::xs));
 const int w=std::max(0,canvas_->width()-2*Ui::space(Space::l));saveFeedback_->resize(w,saveFeedback_->height());
 auto* result=findChild<QWidget*>("editorSaveResult");auto* actions=findChild<QWidget*>("editorSaveActions");
 auto* grid=qobject_cast<QGridLayout*>(result->parentWidget()->layout());
 const int room=w-saveFeedback_->layout()->contentsMargins().left()-saveFeedback_->layout()->contentsMargins().right()-(saveFeedback_->dismissButton()->isHidden()?0:saveFeedback_->dismissButton()->width()+Ui::space(Space::s));
 const bool saved=lastSave_.value("status")=="saved"&&saveFeedbackVisible_;
 const int savedWidth=std::max(savedLabel_->minimumSizeHint().width(),std::min(savedLabel_->fontMetrics().horizontalAdvance(savedLabel_->text()),Ui::scaled(320)));
 const bool overflow=saved&&!savedLabel_->isHidden()&&savedWidth+showFolder_->sizeHint().width()+copyPath_->sizeHint().width()+2*Ui::space(Space::s)>room;
 showFolder_->setVisible(saved&&!overflow);copyPath_->setVisible(saved&&!overflow);savedMore_->setVisible(overflow);
 // Keep enough of the outcome readable before moving recovery actions below it.
 int natural=0,count=0;for(int i=0;i<result->layout()->count();++i)if(auto* widget=result->layout()->itemAt(i)->widget();widget&&!widget->isHidden()){auto* label=qobject_cast<QLabel*>(widget);natural+=label?label->fontMetrics().horizontalAdvance(label->text()):widget->sizeHint().width();++count;}
 natural+=std::max(0,count-1)*result->layout()->spacing();
 const int resultWidth=std::max(result->minimumSizeHint().width(),std::min(natural,Ui::scaled(320)));
 const bool stacked=!overflow&&!actions->isHidden()&&resultWidth+actions->sizeHint().width()+Ui::space(Space::s)>room;
 int row=0,column=0,rowSpan=0,columnSpan=0;grid->getItemPosition(grid->indexOf(actions),&row,&column,&rowSpan,&columnSpan);
 if(row!=(stacked?1:0)){grid->removeWidget(actions);grid->addWidget(actions,stacked?1:0,stacked?0:1,Qt::AlignLeft);}
 // Measure the reflowed content before the outer widget's cached row catches up.
 grid->invalidate();saveFeedback_->layout()->invalidate();
 const auto margins=saveFeedback_->layout()->contentsMargins();
 const int contentHeight=std::max(grid->minimumSize().height(),grid->hasHeightForWidth()?grid->heightForWidth(std::max(0,room)):0);
 const int h=margins.top()+margins.bottom()+std::max(contentHeight,saveFeedback_->dismissButton()->isHidden()?0:saveFeedback_->dismissButton()->height());
 saveFeedback_->setGeometry(Ui::space(Space::l),canvasViewport_->height()-Ui::space(Space::l)-h,w,h);saveFeedback_->layout()->activate();saveFeedback_->raise();
}
void EditorWindow::placeProperties(){
 if(!splitter_)return;
 // The splitter's hit area belongs to the properties share, not the canvas.
 side_->setMaximumWidth(std::max(side_->minimumWidth(),width()/3-splitter_->handleWidth()));
 const int panel=std::clamp(propertiesWidth_,side_->minimumWidth(),side_->maximumWidth());
 splitter_->setSizes({std::max(0,splitter_->width()-splitter_->handleWidth()-panel),panel});
}
void EditorWindow::dismissSavedCopy(){saveFeedbackTimer_->stop();if(auto* popup=saveError_->fullTextPopup())popup->close();saveFeedbackVisible_=false;if(!busy_&&!savingCopy_)message_->hide();placeDestination();}
void EditorWindow::resizeEvent(QResizeEvent* event){QWidget::resizeEvent(event);placeTools();placeProperties();placeDestination();}
bool EditorWindow::eventFilter(QObject* object,QEvent* event){
 if(object==remove_&&(event->type()==QEvent::Enter||event->type()==QEvent::Leave)){const bool hover=event->type()==QEvent::Enter;Ui::role(remove_,hover?"danger":"quiet");remove_->setIcon(Ui::icon("remove",hover?Theme::current().danger:Theme::current().textSecondary));}
 if(object->objectName()=="editorToolbar"&&event->type()==QEvent::FontChange)QTimer::singleShot(0,this,&EditorWindow::placeTools);
 if(object==side_&&(event->type()==QEvent::Resize||event->type()==QEvent::LayoutRequest))QTimer::singleShot(0,this,&EditorWindow::placeDocument);
 if((object==canvas_&&event->type()==QEvent::Resize)||(object==saveFeedback_&&event->type()==QEvent::LayoutRequest))QTimer::singleShot(0,this,&EditorWindow::placeSaveFeedback);
 return QWidget::eventFilter(object,event);
}
void EditorWindow::placeDocument(){
 if(!document_||!side_->isVisible())return;
 // Narrow panels stack complete fields instead of clipping their text or steppers.
 auto* fields=qobject_cast<QGridLayout*>(geometryFields_->layout());
 const int available=propertiesScroll_->viewport()->width()-properties_->contentsMargins().left()-properties_->contentsMargins().right();
 int fieldWidth=0;for(auto* field:geometry_)fieldWidth=std::max(fieldWidth,field->sizeHint().width());
 const bool stacked=available<fieldWidth*2+fields->horizontalSpacing();
 if(geometryRow_->property("stacked").toBool()!=stacked){
  geometryRow_->setProperty("stacked",stacked);
  for(int i=0;i<4;++i){fields->removeWidget(geometryLabels_[i]);fields->removeWidget(geometry_[i]);const int row=stacked?i*2:(i/2)*2,col=stacked?0:i%2;fields->addWidget(geometryLabels_[i],row,col);fields->addWidget(geometry_[i],row+1,col);}
 }
 auto* cropButtons=qobject_cast<QBoxLayout*>(cropRow_->layout());
 const int cropWidth=findChild<QPushButton*>("applyCrop")->sizeHint().width()+findChild<QPushButton*>("cancelCrop")->sizeHint().width()+cropButtons->spacing();
 cropButtons->setDirection(cropWidth>available?QBoxLayout::TopToBottom:QBoxLayout::LeftToRight);
}

void EditorWindow::documentEdited(){canvas_->documentChanged();refresh();}

void EditorWindow::syncStyle(bool applyToSelection){
 editor::ToolStyle style;
 const auto* shape=doc_.find(canvas_->selected());
 const bool highlight=canvas_->tool()==Tool::Highlight||(shape&&shape->kind==editor::Shape::Kind::Highlight);
 style.color=highlight?highlightColor_:annotationColor_;style.width=width_->currentData().toInt();style.fontPixels=textSize_->value();style.fontFamily=font_->currentFont().family();style.strength=strength_->value();style.pixelate=pixelateStyle_->isChecked();style.stepPixels=stepPixels_;
 if(shape&&shape->kind==editor::Shape::Kind::Redact)style.color=Qt::black;
 canvas_->setStyle(style);if(applyToSelection)canvas_->applyStyleToSelection();
 // An exclusive group cannot uncheck everything, which a color off the palette needs.
 swatchGroup_->setExclusive(false);
 for(auto* swatch:swatches_+recentSwatches_){swatch->blockSignals(true);swatch->setChecked(swatch->property("swatchColor").value<QColor>()==style.color);swatch->blockSignals(false);}
 swatchGroup_->setExclusive(true);
 refresh();
}

void EditorWindow::refresh(){
 // Every edit passes here: a Redact or crop change is a new recognition revision.
 text_->documentChanged();text_->setActive(canvas_->tool()==Tool::CopyText);
 const auto tool=canvas_->tool();const auto* shape=doc_.find(canvas_->selected());
 auto kind=[&](editor::Shape::Kind k){return shape&&shape->kind==k;};
 using Kind=editor::Shape::Kind;
 const bool annotation=(shape&&shape->kind!=Kind::Redact&&shape->kind!=Kind::Blur)
  ||tool==Tool::Arrow||tool==Tool::Rectangle||tool==Tool::Text||tool==Tool::Highlight||tool==Tool::Line||tool==Tool::Ellipse||tool==Tool::Pen||tool==Tool::Step;
 colorRow_->setVisible(annotation&&tool!=Tool::Crop);
 const bool stroked=tool==Tool::Arrow||tool==Tool::Rectangle||tool==Tool::Line||tool==Tool::Ellipse||tool==Tool::Pen||kind(Kind::Arrow)||kind(Kind::Rectangle)||kind(Kind::Line)||kind(Kind::Ellipse)||kind(Kind::Pen);
 widthRow_->setVisible(tool!=Tool::Crop&&stroked);
 const bool blur=tool!=Tool::Crop&&(tool==Tool::Blur||kind(Kind::Blur));const bool pixelate=pixelateStyle_->isChecked();
 blurRow_->setVisible(blur);blurStyleRow_->setVisible(blur);
 {
  // Pixelation is the same visual effect as blur, with the same warning.
  auto* radius=static_cast<BlurRadius*>(blurRow_);const auto name=pixelate?QStringLiteral("Block size"):QStringLiteral("Blur radius");
  if(radius->label()->text()!=name){radius->label()->setText(name);strengthValue_->setAccessibleName(name);strength_->setAccessibleName(name);
   const auto description=QString("%1 in image pixels, %2 to %3").arg(name).arg(editor::EditDocument::minimumBlur).arg(editor::EditDocument::maximumBlur);strengthValue_->setAccessibleDescription(description);strengthValue_->setToolTip(description);strength_->setAccessibleDescription(description);}
  blurWarning_->findChild<QLabel*>("editorToolWarning")->setText(pixelate?QStringLiteral("Pixelated text can sometimes be recovered."):Ui::blurWarningText());
 }
 {
  const bool selectedStep=kind(Kind::Step)&&tool!=Tool::Crop;
  stepRow_->setVisible(tool!=Tool::Crop&&(tool==Tool::Step||selectedStep));
  stepLabel_->setText(selectedStep?QStringLiteral("Number"):QStringLiteral("Next number"));stepNumber_->setAccessibleName(selectedStep?QStringLiteral("Step number"):QStringLiteral("Next step number"));
  syncingStep_=true;stepNumber_->setValue(selectedStep?shape->number:canvas_->nextStepNumber());syncingStep_=false;
 }
 textRow_->setVisible(tool!=Tool::Crop&&(tool==Tool::Text||kind(editor::Shape::Kind::Text)));
 copyTextRow_->setVisible(tool==Tool::CopyText);if(tool==Tool::CopyText)syncText();
 quickRedactRow_->setVisible(tool==Tool::Redact);quickRedact_->setEnabled(!busy_&&!quickRedacting_);
 cropRow_->setVisible(tool==Tool::Crop);
 styleRow_->setVisible(tool==Tool::Style);if(tool==Tool::Style)syncCaptureStyle();
 blurWarning_->setVisible(tool!=Tool::Crop&&(tool==Tool::Blur||kind(editor::Shape::Kind::Blur)));
 hint_->setText(tool==Tool::Redact&&!shape?QStringLiteral("The covered pixels are removed from saved and uploaded images."):QString{});
 hint_->setVisible(!hint_->text().isEmpty());
 if(suggested_&&quickRedactRemove_){const bool suggestedMark=shape&&tool!=Tool::Crop&&!shape->suggested.isEmpty();suggested_->setText(suggestedMark?"Suggested: "+quickRedactLabel(shape->suggested):QString{});suggested_->setVisible(suggestedMark);quickRedactRemove_->setEnabled(!busy_&&std::any_of(doc_.shapes().cbegin(),doc_.shapes().cend(),[](const editor::Shape& mark){return !mark.suggested.isEmpty();}));}
 {
  // The panel names the tool (or the selected mark) and how to use it.
  static const QStringList titles{"Select","Redact","Blur","Crop","Arrow","Rectangle","Highlight","Text","Step number","Pen","Ellipse","Line","Copy text","Style"};
  static const QStringList help{"Click a mark to select it.","Drag over private information to cover it with a redaction, an opaque black box.","Drag over an area to blur it.","Drag to choose the area to keep, or set it below, then apply the crop.","Drag from the tail to the point of the arrow.","Drag to frame a detail with an outline.","Drag across text to highlight it; the box snaps to the lines. Click a word, double-click a line, Shift-click to add a word. Hold Alt for a free box.","Click the image to place a label. Enter finishes the text.",
   "Click to drop the next numbered step. Numbers count up; set the next one below.","Drag to draw freehand.","Drag to circle a detail with an outline. Hold Shift for a circle.","Drag to draw a straight line. Hold Shift to keep it at 45°.",
   "Drag across words to select them. Shift+click extends, double-click takes the line, Ctrl+A takes everything, Ctrl+C copies.",
   "Puts the image on a backdrop with padding, rounded corners and a shadow when you save or upload it. The image itself isn't changed."};
  int index=int(tool);QString title=titles.value(index);
  if(tool==Tool::Blur&&pixelate)title=QStringLiteral("Pixelate");
  // A selected mark names itself, whichever tool picked it up.
  if(shape&&tool!=Tool::Crop){const QHash<int,QString> kinds{{int(editor::Shape::Kind::Redact),"Redaction"},{int(editor::Shape::Kind::Blur),"Blur"},{int(editor::Shape::Kind::Arrow),"Arrow"},{int(editor::Shape::Kind::Rectangle),"Rectangle"},{int(editor::Shape::Kind::Highlight),"Highlight"},{int(editor::Shape::Kind::Text),"Text"},
   {int(Kind::Step),"Step"},{int(Kind::Pen),"Pen stroke"},{int(Kind::Ellipse),"Ellipse"},{int(Kind::Line),"Line"}};title=kinds.value(int(shape->kind),"Mark");if(shape->kind==Kind::Blur&&shape->pixelate)title=QStringLiteral("Pixelation");}
  toolTitle_->setText(title);toolTitle_->setAccessibleName(title+(shape&&tool!=Tool::Crop?" selected":" tool"));toolHelp_->setText(shape&&tool!=Tool::Crop?QStringLiteral("Drag to move it."):tool==Tool::Blur&&pixelate?QStringLiteral("Drag over an area to pixelate it."):help.value(index));
  const auto bounds=doc_.crop();const auto exported=doc_.exportSize();
  // #1893: a styled document names the size it exports at.
  documentInfo_->setText(exported!=bounds.size()?QString("Document %1 × %2 px · Styled %3 × %4 px · %5").arg(bounds.width()).arg(bounds.height()).arg(exported.width()).arg(exported.height()).arg(doc_.modified()?"Edited":"Unchanged")
                                               :QString("Document %1 × %2 px · %3").arg(bounds.width()).arg(bounds.height()).arg(doc_.modified()?"Edited":"Unchanged"));
 }
 {
  static const QHash<int,QPair<const char*,const char*>> placing{{int(Tool::Redact),{"Place redaction","redact"}},{int(Tool::Blur),{"Place blur","blur"}},{int(Tool::Arrow),{"Place arrow","annotate-arrow"}},
   {int(Tool::Rectangle),{"Place rectangle","rectangle"}},{int(Tool::Highlight),{"Place highlight","highlight"}},{int(Tool::Text),{"Place text label","text"}},
   {int(Tool::Step),{"Place step","step"}},{int(Tool::Pen),{"Place pen stroke","pen"}},{int(Tool::Ellipse),{"Place ellipse","ellipse"}},{int(Tool::Line),{"Place line","line"}}};
  auto entry=placing.value(int(tool));placeRow_->setVisible(entry.first);
  if(entry.first&&tool==Tool::Blur&&pixelate)entry={"Place pixelation","blur"};
  if(entry.first){place_->setText(QString::fromUtf8(entry.first));place_->setIcon(Ui::icon(entry.second));place_->setToolTip(QString("%1 in the center of the image (Insert)").arg(QString::fromUtf8(entry.first)));}
 }
 syncGeometry();syncSaved();remove_->setVisible(shape&&tool!=Tool::Crop);
 bool options=false;for(auto* row:{colorRow_,widthRow_,blurRow_,textRow_,cropRow_,geometryRow_,stepRow_,copyTextRow_,quickRedactRow_,styleRow_})options=options||!row->isHidden();
 optionsRule_->setVisible(options);
 undo_->setEnabled(!busy_&&doc_.canUndo());redo_->setEnabled(!busy_&&doc_.canRedo());
 // Cancel asks about discarding edits once there are any; it says so.
 const bool edited=doc_.modified();
 if(cancel_->text()!=(edited?"Discard…":"Cancel")){
  cancel_->setText(edited?"Discard…":"Cancel");cancel_->setToolTip(edited?"Discard edits and close":"Close editor");placeDestination();
 }
 const int percent=int(std::lround(canvas_->zoom()*100));
 status_->setText(canvas_->fitted()?QString("Fit · %1%").arg(percent):QString("%1%").arg(percent));status_->setAccessibleName("Zoom");placeTools();
 for(auto* action:status_->menu()->actions())if(action->isCheckable())action->setChecked(action->data().toInt()==(canvas_->fitted()?0:percent));
 status_->setToolTip("Zoom (Ctrl+plus, Ctrl+minus), fit (Ctrl+0), or 100% (Ctrl+1)");status_->setAccessibleDescription(QString("%1% · %2").arg(percent).arg(canvas_->fitted()?"Fit to window":"Custom zoom")+". "+status_->toolTip());
}

void EditorWindow::syncGeometry(){
 const auto geometry=canvas_->selectionGeometry();geometryRow_->setVisible(geometry.has_value());
 if(!geometry)return;
 const auto* shape=canvas_->cropPending()?nullptr:doc_.find(canvas_->selected());
 const bool arrow=shape&&shape->twoPoint(),line=shape&&shape->kind==editor::Shape::Kind::Line,text=shape&&shape->kind==editor::Shape::Kind::Text,step=shape&&shape->kind==editor::Shape::Kind::Step;
 geometryTitle_->setVisible(canvas_->cropPending());geometryDisclosure_->setVisible(!canvas_->cropPending());
 geometryFields_->setVisible(canvas_->cropPending()||geometryDisclosure_->isChecked());
 static const QStringList boxLabels{"X","Y","Width","Height"},arrowLabels{"Tail X","Tail Y","Head X","Head Y"};
 static const QStringList boxNames{"X position","Y position","Width","Height"},arrowNames{"Arrow tail X","Arrow tail Y","Arrow head X","Arrow head Y"};
 static const QStringList lineLabels{"Start X","Start Y","End X","End Y"},lineNames{"Line start X","Line start Y","Line end X","Line end Y"},stepLabels{"X","Y","Size","Size"},stepNames{"X position","Y position","Diameter","Diameter"};
 const auto r=*geometry;const int values[4]={r.x(),r.y(),arrow?r.x()+r.width():r.width(),arrow?r.y()+r.height():r.height()};
 const auto image=doc_.size();const int extent=std::max(image.width(),image.height());
 syncingGeometry_=true;
 for(int i=0;i<4;++i){
  auto* box=geometry_[i];
  geometryLabels_[i]->setText((line?lineLabels:arrow?arrowLabels:step?stepLabels:boxLabels)[i]);box->setAccessibleName((line?lineNames:arrow?arrowNames:step?stepNames:boxNames)[i]+" in image pixels");
  // Text follows its size control; only its position is edited here. A step's one size is its diameter.
  const bool shown=(i<2||!text)&&!(step&&i==3);geometryLabels_[i]->setVisible(shown);box->setVisible(shown);
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
 const bool arrow=shape&&shape->twoPoint();const bool step=shape&&shape->kind==editor::Shape::Kind::Step;
 const int x=geometry_[0]->value(),y=geometry_[1]->value(),a=geometry_[2]->value(),b=step?a:geometry_[3]->value();
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
 if(busy_||savingCopy_||copying_||chooser_->isOpen())return false;
 finishPendingEdits();
 auto folder=QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);if(folder.isEmpty())folder=QDir::homePath();
 chooseCopy(QDir(folder).filePath("put.gd-edit-"+QDateTime::currentDateTime().toString("yyyy-MM-dd-HH-mm-ss")+".png"));
 return true;
}
void EditorWindow::chooseCopy(const QString& path){
 const QFileInfo target(path);
 chooser_->open({FileDialog::Mode::Save,"Save a copy — put.gd",target.absolutePath(),target.fileName(),{{"PNG image",{"*.png"}}}},[this](const FileDialog::Result& result){
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
 if(saving)if(auto* popup=saveError_->fullTextPopup())popup->close();
 if(saving)saveFeedbackTimer_->stop();
 savingCopy_=saving;
 for(auto* button:{save_,saveInstead_,chooseAnother_})button->setEnabled(!saving&&!busy_);
 upload_->setEnabled(!saving&&!busy_);cancel_->setEnabled(!saving&&!busy_);
 if(saving){
  savedLabel_->hide();saveError_->hide();saveTarget_->hide();
  message_->setText("Saving a copy…");Ui::role(message_,"secondary");message_->show();
 }else message_->hide();
 placeDestination();
}
bool EditorWindow::writeCopy(const QString& path,CopyDone done){
 if(busy_||savingCopy_||copying_)return false;
 finishPendingEdits();
 const auto target=pngCopyPath(path);const auto generation=doc_.generation();
 // Qt's image/list copies detach on mutation; no live document reaches the worker.
 auto snapshot=doc_.snapshotForExport();
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

bool EditorWindow::requestCopy(){
 if(busy_||savingCopy_||copying_)return false;
 emit copyRequested();return true;
}
bool EditorWindow::exportForCopy(ExportDone done,std::function<QString(const QByteArray&)> stage){
 if(busy_||savingCopy_||copying_)return false;
 finishPendingEdits();
 // The same snapshot and render as Save a copy, off the GUI thread.
 auto snapshot=doc_.snapshotForExport();
 copying_=true;lastCopy_={{"status","copying"}};
 for(auto* button:{save_,saveInstead_,chooseAnother_,upload_,cancel_,copyImage_,seal_})button->setEnabled(false);
 copyImage_->setMinimumWidth(copyImage_->sizeHint().width());copyImage_->setText("Copying…");
 struct Result {QByteArray png;QSize size;QString staged;};
 auto result=std::make_shared<Result>();
 auto* thread=QThread::create([snapshot=std::move(snapshot),result,stage=std::move(stage)]{result->png=snapshot.exportPng();result->size=snapshot.exportSize();if(stage&&!result->png.isEmpty())result->staged=stage(result->png);});
 // The window joins this worker before it goes, so `stage` never outlives the
 // host's staging; a callback for a closed window is never made.
 copyThread_=thread;
 connect(thread,&QThread::finished,this,[result,done=std::move(done)]{if(done)done(result->png,result->size,result->staged);});
 connect(thread,&QThread::finished,thread,&QObject::deleteLater);
 thread->start();return true;
}
void EditorWindow::showCopied(){
 copying_=false;lastCopy_={{"status","copied"}};
 for(auto* button:{save_,saveInstead_,chooseAnother_,upload_,cancel_,copyImage_})button->setEnabled(!busy_);seal_->setEnabled(!busy_&&(sealAvailable_||seal_->isChecked()));
 // Brief confirmation on the button itself; its width holds still.
 copyImage_->setText("Image copied");copyImage_->setAccessibleName("Image copied");
 QAccessibleEvent event(copyImage_,QAccessible::NameChanged);QAccessible::updateAccessibility(&event);
 const auto generation=++copyFeedback_;
 QTimer::singleShot(1600,copyImage_,[this,generation]{if(generation!=copyFeedback_||copying_)return;copyImage_->setText("Copy image");copyImage_->setAccessibleName("Copy image");});
}
void EditorWindow::showCopyFailed(const QString& reason){
 copying_=false;lastCopy_={{"status","failed"},{"reason",reason}};++copyFeedback_;
 for(auto* button:{save_,saveInstead_,chooseAnother_,upload_,cancel_,copyImage_})button->setEnabled(!busy_);seal_->setEnabled(!busy_&&(sealAvailable_||seal_->isChecked()));
 copyImage_->setText("Copy image");copyImage_->setAccessibleName("Copy image");
 setFailure("Couldn't copy the image: "+reason);
}

void EditorWindow::showSaved(const QString& path){
 if(auto* popup=saveError_->fullTextPopup())popup->close();
 const QFileInfo file(path);
 ++folderRequest_;lastShowInFolder_={};folderError_->hide();
 lastSave_={{"status","saved"},{"path",file.absoluteFilePath()},{"name",file.fileName()}};savedGeneration_=doc_.generation();
 savedLabel_->setToolTip(file.absoluteFilePath());savedLabel_->setAccessibleDescription(file.absoluteFilePath());savedLabel_->show();
 saveError_->hide();saveTarget_->hide();showFolder_->show();copyPath_->show();chooseAnother_->hide();
 saveFeedbackVisible_=true;syncSaved();placeDestination();saveFeedbackTimer_->start(6000);
}
void EditorWindow::syncSaved(){
 if(lastSave_.value("status")!="saved")return;
 // The copy holds the draft as it was then; after another edit it is an earlier copy.
 const bool current=savedGeneration_==doc_.generation();const auto name=lastSave_["name"].toString();
 savedLabel_->setText(current?"Saved "+name:"Earlier copy saved: "+name);Ui::role(savedLabel_,current?"success":"secondary");
 savedLabel_->setToolTip(lastSave_["path"].toString());
 savedLabel_->setAccessibleName(current?"Saved a copy":"Saved an earlier copy");
 if(lastShowInFolder_.isEmpty()||lastShowInFolder_.value("opened").toBool())saveFeedback_->setAccessibleName(savedLabel_->text());
 header_->setDocument("Edit image","Not uploaded");
}
void EditorWindow::showSaveFailed(const QString& path,const QString& reason){
 saveFeedbackTimer_->stop();
 if(auto* popup=saveError_->fullTextPopup())popup->close();
 // The draft is untouched; the reason and a way on sit beside Save a copy….
 // An empty path is the chooser's own failure, before any file was named.
 const QFileInfo file(path);
 auto why=reason.trimmed();if(why.endsWith('.'))why.chop(1);if(!why.isEmpty())why[0]=why[0].toLower();
 lastSave_={{"status","failed"},{"stage",path.isEmpty()?"chooser":"write"},{"path",path.isEmpty()?QJsonValue():QJsonValue(file.absoluteFilePath())},{"name",path.isEmpty()?QJsonValue():QJsonValue(file.fileName())},{"reason",reason}};
 ++folderRequest_;lastShowInFolder_={};folderError_->hide();
 // The file and kept draft are also in the accessible description.
 saveTarget_->setText(file.fileName());
 saveTarget_->hide();
 const auto where=path.isEmpty()?QStringLiteral("the copy"):file.absoluteFilePath();
 saveError_->setText(QString("Couldn't save: %1").arg(reason.trimmed()));saveError_->setRevealText(where+"\n\n"+saveError_->text());
 saveTarget_->setToolTip(where);saveTarget_->setAccessibleDescription(where);
 saveError_->setToolTip(path.isEmpty()?QString():QString("%1").arg(where));saveError_->setAccessibleName("Save a copy failed");
 saveError_->setAccessibleDescription(QString("Couldn't save %1: %2.").arg(where,why));
 saveError_->show();savedLabel_->hide();showFolder_->hide();copyPath_->hide();chooseAnother_->show();
 header_->setDocument("Edit image","Not uploaded");saveFeedbackVisible_=true;placeDestination();saveFeedback_->announce(saveError_->accessibleDescription());
}
bool EditorWindow::copySavedPath(){
 if(lastSave_.value("status")!="saved")return false;
 lastCopiedPath_=lastSave_["path"].toString();QGuiApplication::clipboard()->setText(lastCopiedPath_);
 // Brief confirmation on the button itself; its width holds still.
 const int before=copyPath_->sizeHint().width();copyPath_->setText("Path copied");copyPath_->setMinimumWidth(std::max(before,copyPath_->sizeHint().width()));
 QTimer::singleShot(1600,copyPath_,[button=copyPath_]{button->setText("Copy path");});
 return true;
}
bool EditorWindow::showSavedCopy(){
 if(lastSave_.value("status")!="saved")return false;
 QPointer<EditorWindow> self(this);
 saveFeedbackTimer_->stop();
 const auto path=lastSave_["path"].toString();const auto request=++folderRequest_;
 folderError_->hide();
 host::showInFolder(path,[self,request](const QJsonObject& result){
  if(!self||self->folderRequest_!=request)return;
  self->lastShowInFolder_=result;self->saveError_->setText("Couldn't open folder");self->saveError_->setRevealText(self->lastSave_["path"].toString()+"\n\nCouldn't open the folder. Select Copy path instead.");
  if(!result["opened"].toBool()){self->saveFeedbackTimer_->stop();self->saveFeedbackVisible_=true;}else if(self->saveFeedbackVisible_)self->saveFeedbackTimer_->start(6000);
  self->syncSaved();self->placeDestination();if(!result["opened"].toBool())self->saveFeedback_->announce(self->folderError_->text());
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
 saveFeedbackTimer_->stop();
 if(auto* popup=saveError_->fullTextPopup())popup->close();
 chooser_->cancel();busy_=true;
 // Submitted or discarded: an open discard question has no answer left.
 if(auto* box=discard_.data()){discard_=nullptr;box->close();}
 // Each control is disabled on its own, so the document details, the header
 // and this message keep full contrast while the image is prepared.
 for(auto* widget:actionable())widget->setEnabled(false);
 setNotice({});message_->setText(message);Ui::role(message_,"secondary");message_->show();placeDestination();setCursor(Qt::BusyCursor);
}
void EditorWindow::setFailure(const QString& message){saveFeedbackTimer_->stop();message_->setText(message);Ui::role(message_,"error");message_->show();placeDestination();saveFeedback_->announce(message);}
void EditorWindow::showPreparationFailed(const QString& reason){
 // Nothing was handed off: the draft and its controls come back as they were.
 busy_=false;for(auto* widget:actionable())widget->setEnabled(true);unsetCursor();
 // Seal keeps its own rule: On only where the destination can seal, Off always.
 seal_->setEnabled(sealAvailable_||seal_->isChecked());
 message_->hide();destination_->setVisible(!destination_->text().isEmpty());
 setNotice(reason);refresh();keepEditing_->setFocus();
}
void EditorWindow::setNotice(const QString& reason){
 noticeText_->setText(reason);noticeText_->setAccessibleName(reason.isEmpty()?QString():QStringLiteral("Upload failed"));noticeText_->setAccessibleDescription(reason);
 notice_->setVisible(!reason.isEmpty());save_->setVisible(reason.isEmpty());placeDestination();
}

void EditorWindow::setSeal(const QJsonObject& seal) {
 if(seal.isEmpty()){seal_->hide();return;}
 sealAvailable_=seal.value("available").toBool();sealReason_=seal.value("reason").toString();
 {const QSignalBlocker block(seal_);seal_->setChecked(seal.value("on").toBool());}
 // Off is always possible; On only where the destination passed its sealed-link check.
 seal_->setEnabled(sealAvailable_||seal_->isChecked());
 seal_->setToolTip(sealAvailable_?QStringLiteral("Seal this upload. Only someone with the full link can open it"):sealReason_);
 seal_->setAccessibleDescription(seal_->isChecked()?QStringLiteral("On"):QStringLiteral("Off"));
 seal_->show();
}
bool EditorWindow::setSealOn(bool on,QString* refusal) {
 if(seal_->isHidden()){*refusal=QStringLiteral("This edit has no upload destination.");return false;}
 if(on&&!sealAvailable_){*refusal=sealReason_;return false;}
 // Asking for the state it already has changes nothing and succeeds.
 if(on==seal_->isChecked())return true;
 if(!seal_->isEnabled()){*refusal=QStringLiteral("Seal can't be changed right now.");return false;}
 seal_->setChecked(on);if(!sealAvailable_)seal_->setEnabled(false);return true;
}
QJsonObject EditorWindow::sealState() const {
 QJsonObject out{{"shown",!seal_->isHidden()},{"on",seal_->isChecked()},{"available",sealAvailable_},{"enabled",seal_->isEnabled()}};
 if(!sealReason_.isEmpty())out["reason"]=sealReason_;return out;
}
QJsonObject EditorWindow::viewState() const {
 const auto& style=canvas_->style();const auto selected=canvas_->selected();
 QJsonObject message;if(!message_->isHidden())message={{"text",message_->text()},{"tone",message_->property("visualRole").toString()}};
 auto save=lastSave_;if(save.value("status")=="saved")save["current"]=savedGeneration_==doc_.generation();
 return {{"scheme",Theme::name(Theme::scheme())},{"tool",toolNames.value(int(canvas_->tool()))},{"selectedShapeId",selected?QJsonValue(double(selected)):QJsonValue()},
  {"geometryDetails",QJsonObject{{"open",geometryDisclosure_->isChecked()},{"visible",geometryFields_->isVisibleTo(this)}}},
  {"geometry",rectValue(canvas_->selectionGeometry())},{"cropPending",canvas_->cropPending()},{"selection",canvas_->selectionSummary()},
  {"zoomPercent",std::round(canvas_->zoom()*1000)/10},{"fitted",canvas_->fitted()},{"visibleRect",rectValue(canvas_->visibleImageRect())},
  {"propertiesWidth",side_->width()},{"canvasSize",QJsonArray{canvas_->width(),canvas_->height()}},{"editingText",canvas_->editingText()},{"textOutsideImage",canvas_->textOutsideImage()},{"busy",busy_},{"savingCopy",savingCopy_},
  {"style",QJsonObject{{"color",style.color.name()},{"width",style.width},{"strength",style.strength},{"blurStyle",style.pixelate?"pixelate":"blur"},{"fontPixels",style.fontPixels},{"font",style.fontFamily},{"stepSize",style.stepPixels}}},
  {"nextStepNumber",canvas_->nextStepNumber()},{"copying",copying_},{"lastCopy",lastCopy_.isEmpty()?QJsonValue():QJsonValue(lastCopy_)},
  {"message",message.isEmpty()?QJsonValue():QJsonValue(message)},{"lastSave",save.isEmpty()?QJsonValue():QJsonValue(save)},
  {"saveFeedbackVisible",saveFeedbackVisible_&&!busy_&&!savingCopy_},{"saveDetailsOpen",saveError_->fullTextPopup()&&saveError_->fullTextPopup()->isVisible()},
  {"lastShowInFolder",lastShowInFolder_.isEmpty()?QJsonValue():QJsonValue(lastShowInFolder_)},{"lastCopiedPath",lastCopiedPath_.isEmpty()?QJsonValue():QJsonValue(lastCopiedPath_)},
  {"seal",sealState()},{"cancelLabel",cancel_->text()},{"destination",QJsonObject{{"text",destination_->text()+(sharingMode_->text().isEmpty()?QString():" · "+sharingMode_->text())},{"sharingMode",sharingMode_->text()},{"above",destinationAbove_}}},
  {"preparationFailure",notice_->isHidden()?QJsonValue():QJsonValue(QJsonObject{{"text",noticeText_->text()}})},
  {"keyboardHelp",QJsonObject{{"open",findChild<QToolButton*>("editorKeyboardHelp")->isChecked()},{"text",findChild<QWidget*>("editorKeyboardHelpText")->property("text").toString()}}},
  {"discardConfirmation",QJsonObject{{"open",bool(discard_)},{"text",discard_?QJsonValue(discard_->property("confirmationText").toString()):QJsonValue()}}}};
}

QJsonObject EditorWindow::viewCommand(const QJsonObject& params,QString& code,QString& message){
 auto fail=[&](const QString& c,const QString& m){code=c;message=m;return QJsonObject{};};
 auto only=[&](QStringList keys){keys<<"action";for(auto it=params.begin();it!=params.end();++it)if(!keys.contains(it.key()))return false;return true;};
 if(params.contains("action")&&!params["action"].isString())return fail("invalid_request","action must be a string");
 const auto action=params["action"].toString("inspect");
 if(action=="inspect"){if(!only({}))return fail("invalid_request","inspect has no other parameters");return viewState();}
 if(busy_)return fail("editor_finalizing","The edit was already submitted or discarded");
 // A drag in progress owns the document's current undo step: a change made now would join it, and abandoning the drag
 // would roll it back after the agent was told it worked. Every action that edits the document or changes what is selected waits for the drag.
 if(QStringList{"removeMark","removeSuggestions","place","geometry","applyCrop","blurStyle","stepNumber","select","selectNext","selectPrevious","selectAffectedText"}.contains(action)&&doc_.inGesture())return fail("editor_busy","A mark is being dragged. Try again when the drag ends.");
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
 }else if(action=="geometryDetails"){
  if(!only({}))return fail("invalid_request","geometryDetails has no other parameters");
  geometryDisclosure_->toggle();
 }else if(action=="tool"){
  if(!only({"tool"})||!toolNames.contains(params["tool"].toString()))return fail("invalid_request","tool must be select, redact, blur, crop, arrow, line, rectangle, ellipse, pen, highlight, text, step, copyText or style");
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
 }else if(action=="removeSuggestions"){
  // Quick redact's "Remove suggestions": every suggested mark, one undo step; marks drawn by hand stay (#1618).
  if(!only({}))return fail("invalid_request","removeSuggestions has no other parameters");
  if(removeSuggestions()==0)return fail("edit_shape_missing","There are no suggested marks to remove");
 }else if(action=="selectAffectedText"){
  if(!only({}))return fail("invalid_request","selectAffectedText has no other parameters");
  if(!canvas_->selectAffectedText())return fail("edit_shape_missing","No annotation text will be clipped");
 }else if(action=="selectNext"||action=="selectPrevious"){
  if(!only({}))return fail("invalid_request",action+" has no other parameters");
  if(!canvas_->selectAdjacent(action=="selectNext"?1:-1,true))return fail("edit_shape_missing","There is no mark to select");
 }else if(action=="place"){
  if(!only({}))return fail("invalid_request","place has no other parameters");
  // Text needs typing; agents add labels with editor.apply.
  const auto tool=canvas_->tool();
  if(tool==Tool::Select||tool==Tool::Style||tool==Tool::Crop||tool==Tool::Text)return fail("invalid_request","Choose redact, blur, arrow, line, rectangle, ellipse, pen, highlight or step first; add text with editor.apply");
  canvas_->placeMark();
 }else if(action=="blurStyle"){
  // The Blur / Pixelate choice, as its buttons set it (and restyle a selected blur).
  if(!only({"style"})||!QStringList{"blur","pixelate"}.contains(params["style"].toString()))return fail("invalid_request","blurStyle needs style blur or pixelate");
  (params["style"]=="pixelate"?pixelateStyle_:blurStyle_)->click();
 }else if(action=="stepNumber"){
  // The Next number field (or a selected step's Number), as typed there.
  int number=0;if(!only({"number"})||!integerIn(params["number"],1,editor::maximumStepNumber,number))return fail("invalid_request","stepNumber needs number from 1 to 999");
  if(stepRow_->isHidden())return fail("invalid_request","Choose the Step tool or select a step first");
  stepNumber_->setValue(number);
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
 }else if(action=="propertiesWidth"){
  int pixels=0;if(!only({"pixels"})||!integerIn(params["pixels"],200,10000,pixels))return fail("invalid_request","propertiesWidth needs pixels from 200 to 10000; the panel is capped at one third of the window");
  propertiesWidth_=pixels;placeProperties();
 }else if(action=="pan"){
  const auto d=params["delta"].toArray();int dx=0,dy=0;
  if(!only({"delta"})||!params["delta"].isArray()||d.size()!=2||!integerIn(d[0],-100000,100000,dx)||!integerIn(d[1],-100000,100000,dy))return fail("invalid_request","pan needs delta [dx, dy] in canvas pixels");
  canvas_->panBy(QPointF(dx,dy));
 }else if(action=="saveDetails"){
  if(!only({}))return fail("invalid_request","saveDetails has no other parameters");
  if(!findChild<QToolButton*>("editorSaveDetails")->isVisibleTo(this))return fail("editor_no_save_failure","No save failure details are showing");
  findChild<QToolButton*>("editorSaveDetails")->click();
 }else if(action=="dismissSavedCopy"){
  if(!only({}))return fail("invalid_request","dismissSavedCopy has no other parameters");
  dismissSavedCopy();
 }else if(action=="showSavedCopy"||action=="copySavedPath"){
  if(!only({}))return fail("invalid_request",action+" has no other parameters");
  if(!(action=="showSavedCopy"?showSavedCopy():copySavedPath()))return fail("editor_no_saved_copy","Save a copy first");
 }else if(action=="discard"){
  // As Cancel / Discard… does: asks first when there are edits.
  if(!only({}))return fail("invalid_request","discard has no other parameters");
  requestCancel();if(busy_)return viewState();
 }else return fail("invalid_request","action must be inspect, tool, select, removeMark, removeSuggestions, selectNext, selectPrevious, selectAffectedText, place, geometry, applyCrop, cancelCrop, zoom, fit, actualSize, pan, propertiesWidth, blurStyle, stepNumber, keyboardHelp, geometryDetails, showSavedCopy, copySavedPath, saveDetails, dismissSavedCopy, discard, keepEditing or confirmDiscard");
 refresh();return viewState();
}

void EditorWindow::requestCancel(){
 if(busy_||savingCopy_||copying_)return;
 if(discard_){discard_->raise();discard_->activateWindow();return;}
 if(!doc_.modified()){emit cancelRequested();return;}
 const auto text=lastSave_.value("status")=="saved"?QStringLiteral("Discard edits and close?\nYour unsaved edits will be lost. Copies you saved stay."):QStringLiteral("Discard edits and close?\nYour unsaved edits will be lost.");
 auto* box=Ui::confirm(this,"Discard edits — put.gd",text,"Discard edits",QStringLiteral("Keep editing"),true);
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
 QWidget::showEvent(event);if(!shown_){shown_=true;placeProperties();canvas_->fit();canvas_->setFocus();}
}
EditorWindow::~EditorWindow(){if(copyThread_)copyThread_->wait();}
namespace {
QString codeKind(const QString& format){
 static const QHash<QString,QString> names{{"QRCode","QR code"},{"MicroQRCode","Micro QR code"},{"DataMatrix","Data Matrix"},{"Aztec","Aztec code"}};
 return names.value(format,format);
}
}
void EditorWindow::textSelectionEdited(){canvas_->update();refresh();}
void EditorWindow::syncText(){
 const auto& layer=text_->layer();const auto state=text_->state();const int words=int(layer.words().size());
 QString line;
 if(state=="recognizing")line=QStringLiteral("Recognizing…");
 else if(state=="failed")line=text_->error().message;
 else if(state=="completed")line=words?QLocale().toString(words)+(words==1?" word":" words"):QStringLiteral("No text found");
 textState_->setText(line);textState_->setVisible(!line.isEmpty());
 copySelection_->setEnabled(!busy_&&layer.hasSelection());
 bool codeText=false;for(const auto& code:layer.codes())codeText=codeText||!code.binary;
 copyAllText_->setEnabled(!busy_&&(words>0||codeText));
 textTruncated_->setVisible(state=="completed"&&text_->truncated());
 if(codesShown_!=layer.identity())rebuildCodes();
}
// One row a code: its kind, its content on one elided line (a link whole and
// wrapped, its host in ASCII, so it is in view before Open link), Copy, and
// Open link for an http or https link. A binary code shows its kind and length.
void EditorWindow::rebuildCodes(){
 codesShown_=text_->layer().identity();
 while(auto* item=codeLayout_->takeAt(0)){if(auto* widget=item->widget())widget->deleteLater();delete item;}
 const auto& codes=text_->layer().codes();codesTitle_->setVisible(!codes.isEmpty());codeList_->setVisible(!codes.isEmpty());
 for(int index=0;index<codes.size();++index){
  const auto& code=codes.at(index);const auto link=EditorTextRecognition::openableLink(code.binary?QString():code.text);
  auto* row=new QWidget(codeList_);row->setObjectName(QString("editorCode%1").arg(index));auto* box=new QVBoxLayout(row);box->setContentsMargins(0,0,0,0);box->setSpacing(Ui::space(Space::xs));
  auto* kind=fieldLabel(codeKind(code.format)+(code.binary?QString(" · binary, %1 bytes").arg(code.length):QString()),row);box->addWidget(kind);
  if(!code.binary){
   if(link.isValid()){auto* content=Ui::label(link.toString(QUrl::FullyEncoded),"body",row);content->setObjectName("editorCodeLink");content->setWordWrap(true);content->setTextFormat(Qt::PlainText);content->setTextInteractionFlags(Qt::TextSelectableByMouse);box->addWidget(content);}
   else{auto* content=new Ui::ElidedLabel(row);content->setObjectName("editorCodeContent");content->setTextFormat(Qt::PlainText);content->setElideMode(Qt::ElideRight);content->setText(EditorTextRecognition::shown(code.text));Ui::role(content,"body");box->addWidget(content);}
  }
  auto* buttons=new QHBoxLayout;buttons->setContentsMargins(0,0,0,0);buttons->setSpacing(Ui::space(Space::s));
  auto* copy=new QPushButton("Copy",row);copy->setObjectName("editorCopyCode");copy->setEnabled(!code.binary);Ui::role(copy,"quiet");copy->setProperty("uiDensity","compact");buttons->addWidget(copy);
  connect(copy,&QPushButton::clicked,this,[this,index]{copyCode(index);});
  if(link.isValid()||code.binary){auto* open=new QPushButton(Ui::icon("open-external"),"Open link",row);open->setObjectName("editorOpenCode");open->setEnabled(link.isValid());Ui::role(open,"quiet");open->setProperty("uiDensity","compact");buttons->addWidget(open);connect(open,&QPushButton::clicked,this,[this,index]{openCode(index);});}
  buttons->addStretch(1);box->addLayout(buttons);codeLayout_->addWidget(row);
 }
}
void EditorWindow::textOutcome(const QString& what,Error error,int characters,TextDone done){
 const auto status=!error?QStringLiteral("copied"):error.code=="copy_suppressed"?QStringLiteral("suppressed"):QStringLiteral("failed");
 textFeedback_->setText(status=="copied"?QString("Copied %1").arg(what):status=="suppressed"?QStringLiteral("Text not copied because your clipboard changed"):"Couldn't copy the text: "+error.message);
 Ui::role(textFeedback_,status=="copied"?"secondary":"danger");textFeedback_->show();
 if(done){if(status=="failed")done(error,{});else done({},{{"copyStatus",status},{"characters",characters}});}
}
void EditorWindow::copyRecognizedText(bool all,TextDone done,bool agent){
 const auto identity=text_->identity();
 if(!identity){if(done)done({"text_unavailable","There is no recognized text to copy",{}},{});return;}
 const auto text=all?text_->allText(agent):text_->layer().selectedText();
 if(text.isEmpty()){if(done)done({"text_unavailable",all?"No text was found":"Select words to copy",{}},{});return;}
 if(!textCopier_){textOutcome({},{"clipboard_unavailable","The clipboard is not available",{}},0,done);return;}
 const int characters=int(textjob::characters(text));const auto what=QLocale().toString(characters)+(characters==1?" character":" characters");
 textCopier_(text,[g=QPointer<EditorWindow>(this),what,characters,done](Error error){if(g)g->textOutcome(what,error,characters,done);else if(done)done(error,{});});
}
void EditorWindow::copyCode(int index,TextDone done,bool agent){
 const auto& codes=text_->layer().codes();
 if(index<0||index>=codes.size()){if(done)done({"invalid_request","There is no code with that index",{}},{});return;}
 if(codes.at(index).binary){if(done)done({"code_binary","A binary code is neither copied nor shown",{}},{});return;}
 if(agent&&!codes.at(index).secret.isEmpty()){if(done)done({"code_withheld","A code that holds a secret is not copied for an agent",{}},{});return;}
 if(!textCopier_){textOutcome({},{"clipboard_unavailable","The clipboard is not available",{}},0,done);return;}
 const auto text=codes.at(index).text;const int characters=int(textjob::characters(text));
 textCopier_(text,[g=QPointer<EditorWindow>(this),characters,done](Error error){if(g)g->textOutcome("the code",error,characters,done);else if(done)done(error,{});});
}
void EditorWindow::openCode(int index,TextDone done){
 const auto& codes=text_->layer().codes();
 if(index<0||index>=codes.size()){if(done)done({"invalid_request","There is no code with that index",{}},{});return;}
 const auto link=EditorTextRecognition::openableLink(codes.at(index).binary?QString():codes.at(index).text);
 if(!link.isValid()){if(done)done({"link_unsupported","Only an http or https link opens",{}},{});return;}
 // At once, with no dialog: the whole link is already in the code's row.
 host::openLink(link,[g=QPointer<EditorWindow>(this),done](const QJsonObject& result){
  if(g&&!result["opened"].toBool()){g->textFeedback_->setText("Couldn't open the link.");Ui::role(g->textFeedback_,"danger");g->textFeedback_->show();}
  if(done){if(result["opened"].toBool())done({},result);else done({"link_not_opened","Couldn't open the link on this desktop.",result},{});}
 });
}
bool EditorWindow::parseRedactText(const QJsonObject& entry,QStringList& categories,QStringList& terms,QStringList& patterns,QString& code,QString& message){
 categories.clear();terms.clear();patterns.clear();
 const auto bad=[&](const QString& text){code="edit_command_invalid";message=text;return false;};
 const auto kinds=editor::quickRedact::categories();
 const auto categoryError=QStringLiteral("redactText takes categories: 1 to %1 of %2 and %3").arg(kinds.size()).arg(kinds.mid(0,kinds.size()-1).join(", ")).arg(kinds.last());
 for(auto it=entry.begin();it!=entry.end();++it)if(!QStringList{"op","categories","terms","patterns"}.contains(it.key()))return bad(categoryError+", with optional terms and patterns");
 const auto list=entry["categories"].toArray();
 if(!entry["categories"].isArray()||list.isEmpty()||list.size()>kinds.size())return bad(categoryError);
 for(const auto& category:list){if(!category.isString()||!kinds.contains(category.toString())||categories.contains(category.toString()))return bad(categoryError);categories.append(category.toString());}
 const auto strings=[&](const char* key,QStringList& into,int most,QString(*problem)(const QString&),const char* noun)->bool{
  if(!entry.contains(key))return true;
  const auto values=entry[key].toArray();
  if(!entry[key].isArray()||values.isEmpty()||values.size()>most)return bad(QStringLiteral("redactText %1 are 1 to %2 %3").arg(key).arg(most).arg(noun));
  int index=0;
  for(const auto& value:values){
   // An error names the entry's position, never its text.
   if(!value.isString())return bad(QStringLiteral("redactText %1 must be strings (entry %2)").arg(key).arg(index));
   const auto error=problem(value.toString());if(!error.isEmpty())return bad(QStringLiteral("redactText %1 entry %2: %3").arg(key).arg(index).arg(error));
   into.append(value.toString());++index;
  }
  return true;
 };
 if(!strings("terms",terms,textjob::maxRedactTerms,textjob::redactTermError,"words or phrases of up to 100 characters on one line")||!strings("patterns",patterns,textjob::maxRedactPatterns,Settings::quickRedactPatternError,"regular expressions of up to 500 characters"))return false;
 if((entry.contains("terms")||entry.contains("patterns"))&&!categories.contains("list"))return bad("redactText terms and patterns need the list category");
 return true;
}
QJsonObject EditorWindow::applyCommands(const QJsonArray& commands,QString& code,QString& message){
 if(doc_.inGesture()){code="editor_busy";message="A mark is being dragged. Try again when the drag ends.";return {};}
 QJsonArray created;
 if(!editor::Commands::apply(doc_,commands,created,code,message))return {};
 documentEdited();
 return {{"created",created},{"state",editor::Commands::state(doc_)}};
}
void EditorWindow::setRedactList(QStringList terms,QStringList patterns){
 redactTerms_=std::move(terms);redactPatterns_=std::move(patterns);
 // Nothing saved: the entry says where to add words and stays off (agents can still pass terms).
 const bool empty=redactTerms_.isEmpty()&&redactPatterns_.isEmpty();
 if(quickRedactList_){quickRedactList_->setEnabled(!empty);quickRedactList_->setToolTip(empty?QStringLiteral("Add words or patterns in Settings → Capture → Text recognition"):QStringLiteral("Use words and patterns from Settings → Capture → Text recognition"));}
}
int EditorWindow::removeSuggestions(){
 if(busy_)return 0;
 const auto removed=doc_.removeSuggested();
 if(removed){quickRedactResult_->setText(QString("Removed %1 suggested %2").arg(removed).arg(removed==1?"redaction":"redactions"));Ui::role(quickRedactResult_,"body");quickRedactResult_->show();documentEdited();}
 return removed;
}
void EditorWindow::quickRedact(const QStringList& categories,TextDone done,QStringList terms,QStringList patterns,QStringList first){
 if(quickRedacting_){if(done)done({"editor_busy","Quick redact is already waiting for recognition",{}},{});return;}
 if(doc_.inGesture()){if(done)done({"editor_busy","A mark is being dragged. Try again when the drag ends.",{}},{});return;}
 // The list alone, with nothing saved and nothing given: there is nothing to find, so no recognition starts (and none can fail).
 if(categories==QStringList{"list"}&&redactTerms_.isEmpty()&&redactPatterns_.isEmpty()&&terms.isEmpty()&&patterns.isEmpty()){
  if(busy_){if(done)done({"editor_finalizing","The edit was already submitted or discarded",{}},{});return;}
  quickRedactResult_->setText("Your redaction list is empty. Add words in Settings → Capture → Text recognition.");Ui::role(quickRedactResult_,"body");quickRedactResult_->show();
  if(done)done({},{{"created",QJsonArray{}},{"counts",QJsonObject{{"list",0}}}});
  return;
 }
 quickRedacting_=true;quickRedactResult_->setText("Recognizing…");Ui::role(quickRedactResult_,"body");quickRedactResult_->show();refresh();
 const QDeadlineTimer dragWait(quickRedactGestureWaitMs_);
 text_->whenRecognized([g=QPointer<EditorWindow>(this),categories,done,dragWait,terms=std::move(terms),patterns=std::move(patterns),first=std::move(first)](Error error){
  if(!g)return;
  const auto stopWith=[&](const QString& text,const char* role,Error failure){g->quickRedacting_=false;g->quickRedactResult_->setText(text);Ui::role(g->quickRedactResult_,role);g->refresh();if(done)done(failure,{});};
  if(g->busy_){stopWith("Quick redact added no marks because the edit was submitted or discarded.","danger",{"editor_finalizing","The edit was already submitted or discarded",{}});return;}
  if(error){stopWith(error.code=="edit_stale"?QStringLiteral("A redaction or the crop changed, so nothing was added. Try again."):error.message,"danger",error);return;}
  // The saved list and an agent's own terms, for this run (#1618). Never stored or logged. The account is
  // looked up by the runner, on its worker thread, only when name is chosen.
  editor::quickRedact::Sources sources;
  sources.terms=g->redactTerms_+terms;sources.patterns=g->redactPatterns_+patterns;
  // Matching runs off the UI thread: a pattern can be slow. The document may change meanwhile.
  const auto revision=g->text_->revision();
  const auto candidates=g->text_->candidates();
  // #1941: the codes recognition decoded on the same page; one whose content holds a 2FA secret, a Wi-Fi
  // password or a key is covered with Keys and tokens. Its content is read only to decide, here.
  const auto codes=g->text_->secretCodes();
  const std::function<void(editor::quickRedact::Result)> applyFound=[g=QPointer<EditorWindow>(g),candidates,codes,categories,revision,done,dragWait](editor::quickRedact::Result found){
   if(!g)return;
   // The marks join the document as one undo step of their own, so they wait for a drag in progress to end.
   // The revision is checked then, not now: a drag can add a Redact mark, and an edit that was undone is still a new revision.
   auto apply=std::make_shared<std::function<void()>>();
   *apply=[g,candidates,codes,categories,revision,done,dragWait,found,self=std::weak_ptr<std::function<void()>>(apply)]{
    if(!g)return;
    // A drag that outlasts the wait ends it: the agent supervisor has answered editor_timeout by then, and marks added later
    // would be ones nobody asked for.
    if(g->doc_.inGesture()&&!dragWait.hasExpired()){if(auto keep=self.lock())QTimer::singleShot(40,g.data(),[keep]{(*keep)();});return;}
    g->quickRedacting_=false;
    const auto fail=[&](const QString& text,Error failure){g->quickRedactResult_->setText(text);Ui::role(g->quickRedactResult_,"danger");g->refresh();if(done)done(failure,{});};
    if(g->doc_.inGesture()){fail("A mark was still being dragged, so nothing was added. Try again.",{"editor_busy","A mark was still being dragged while Quick redact waited",{}});return;}
    if(g->busy_){fail("Quick redact added no marks because the edit was submitted or discarded.",{"editor_finalizing","The edit was already submitted or discarded",{}});return;}
    if(g->text_->revision()!=revision){fail("A redaction or the crop changed, so nothing was added. Try again.",{"edit_stale","A Redact mark or the crop changed while Quick redact was matching",{}});return;}
    // Every match is ordinary Redact marks, all added as one undo step. A mark the user deleted from an earlier run is not added back.
    QJsonObject counts;for(const auto& category:categories)counts[category]=0;QJsonArray created;
    g->doc_.beginGesture();
    for(const auto& match:found.matches){
     bool any=false;
     // Each padded mark comes with the recognized words' own box it was built from: that, never the padded mark, is what a
    // deleted suggestion is remembered by (padding makes two neighbouring one-character words look like one suggestion).
    for(const auto& [rect,words]:markedWords(candidates,match,g->doc_.imageRect(),g->doc_.base())){
      if(g->doc_.dismissed(words,match.category))continue;
      editor::Shape shape;shape.kind=editor::Shape::Kind::Redact;shape.rect=rect;shape.suggested=match.category;shape.suggestedAt=words;created.append(double(g->doc_.add(shape)));any=true;
     }
     if(any)counts[match.category]=counts[match.category].toInt()+1;
    }
    // #1941: a code holding a secret gets one solid mark over the whole symbol, finder patterns and all,
    // and a fifth of its size around it (at least 4 px), so what is shared cannot be scanned: a code
    // decodes through damage to part of it. Only its kind is reported, never its content.
    QJsonObject codeKinds;
    if(categories.contains(QLatin1String("key")))for(const auto& code:codes){
     const auto& kind=code.kind;
     const auto symbol=code.quad.boundingRect();
     const int margin=qMax(4,qMax(symbol.width(),symbol.height())/5);
     const auto rect=symbol.adjusted(-margin,-margin,margin,margin).intersected(g->doc_.imageRect());
     if(rect.isEmpty()||g->doc_.dismissed(symbol,QStringLiteral("key")))continue;
     editor::Shape shape;shape.kind=editor::Shape::Kind::Redact;shape.rect=rect;shape.suggested=QStringLiteral("key");shape.suggestedAt=symbol;created.append(double(g->doc_.add(shape)));
     counts["key"]=counts["key"].toInt()+1;codeKinds[kind]=codeKinds[kind].toInt()+1;
    }
    g->doc_.commitGesture();
    const auto added=created.size();
    QStringList lines{added?QString("Added %1 suggested %2").arg(added).arg(added==1?"redaction":"redactions"):QStringLiteral("No matches found. Check the image manually.")};
    QJsonArray skipped;for(const int index:found.skippedPatterns){skipped.append(index);lines<<QString("Pattern %1 didn't finish and was skipped.").arg(index+1);}
    // The account lookup did not answer in time: the other categories ran without it (#1618).
    if(found.nameUnchecked)lines<<QStringLiteral("Skipped 'My username and name'. Account lookup timed out.");
    g->quickRedactResult_->setText(lines.join('\n'));Ui::role(g->quickRedactResult_,"body");
    g->documentEdited();
    QJsonObject result{{"created",created},{"counts",counts}};if(!codeKinds.isEmpty())result["codes"]=codeKinds;if(!skipped.isEmpty())result["skippedPatterns"]=skipped;if(found.nameUnchecked)result["nameUnchecked"]=true;if(found.unfinished)result["unfinished"]=true;
    if(done)done({},result);
   };
   (*apply)();
  };
  // #1820: `first` is matched on its own, before the rest, so a word one of its kinds finds is never
  // claimed by another kind first. Each other kind is then matched on its own too, on the same words, so
  // no kind drops another's overlapping match. A later match whose words `first` took entirely is covered
  // already and is not added; one it took only part of stays, overlap and all.
  QStringList head,rest;for(const auto& category:categories)(first.contains(category)?head:rest).append(category);
  if(head.isEmpty()||rest.isEmpty()){ui::runQuickRedact(g,candidates,categories,sources,applyFound);return;}
  ui::runQuickRedact(g,candidates,head,sources,[g=QPointer<EditorWindow>(g),candidates,rest,sources,applyFound](editor::quickRedact::Result before){
   if(!g)return;
   QSet<int> taken;for(const auto& match:std::as_const(before.matches))for(const int word:match.words)taken.insert(word);
   auto merged=std::make_shared<editor::quickRedact::Result>(std::move(before));
   auto next=std::make_shared<std::function<void(int)>>();
   *next=[g,candidates,rest,sources,applyFound,taken,merged,weak=std::weak_ptr<std::function<void(int)>>(next)](int index){
    if(!g)return;
    if(index>=rest.size()){applyFound(*merged);return;}
    auto keep=weak.lock();
    ui::runQuickRedact(g,candidates,{rest[index]},sources,[keep,index,taken,merged](editor::quickRedact::Result after){
     for(const auto& match:std::as_const(after.matches))if(!std::all_of(match.words.cbegin(),match.words.cend(),[&](int word){return taken.contains(word);}))merged->matches.append(match);
     for(const int pattern:std::as_const(after.skippedPatterns))if(!merged->skippedPatterns.contains(pattern))merged->skippedPatterns.append(pattern);
     merged->nameUnchecked=merged->nameUnchecked||after.nameUnchecked;merged->unfinished=merged->unfinished||after.unfinished;
     if(keep)(*keep)(index+1);
    });
   };
   (*next)(0);
  });
 });
}
}
