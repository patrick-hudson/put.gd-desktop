#include <cmath>
#include "SettingsWindow.h"
#include "ErrorLogPage.h"
#include "CustomUploaderPanel.h"
#include "MainWindow.h"
#include "storage/proton/ProtonProvider.h"
#include "storage/s3/PublicUrl.h"
#include "storage/s3/R2Settings.h"
#include "storage/b2/B2Common.h"
#include "platform/HostOpen.h"
#include "core/HostEnvironment.h"
#include "core/DestinationCheck.h"
#include <QUrl>
#include "ViewState.h"
#include <QTimer>
#include <QSignalBlocker>
#include "ShortcutEditor.h"
#include "PreferenceRows.h"
#include "core/ByteSize.h"
#include "core/TextJobProtocol.h"
#include <QLocale>
#include <QProgressBar>
#include "Ui.h"
#include <QApplication>
#include <QAbstractItemModel>
#include <QListWidget>
#include <QStackedWidget>
#include <QToolButton>
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCompleter>
#include "FileDialog.h"
#include <QFormLayout>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <QClipboard>
#include <QMenu>
#include <QActionGroup>
#include <QRadioButton>
#include <QInputDialog>
#include <QMessageBox>
#include <memory>
#include <QStyle>
#include <QAbstractSpinBox>
#include <utility>
#include <tuple>
#include <QTabBar>
#include <QWheelEvent>
#include <QRegularExpression>
#include <QDrag>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QMouseEvent>
#include <QFileInfo>
#include <QDir>
#include <QLocale>
#include <QDateTime>
#include <QDialog>
#include <QContextMenuEvent>
#include <QToolTip>
#include <QHelpEvent>
#include <QStyleOptionFrame>
#include <QStyleOptionButton>
#include <QWidgetItem>
#include <QFontMetricsF>
#include <QtMath>
#include <QTextLayout>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextCursor>
#include <QAbstractTextDocumentLayout>
#include <QDoubleValidator>
#include <QScreen>
#include <QWindow>
#include <QGuiApplication>
#include <QScopedValueRollback>
#include <QCoreApplication>
namespace putgd {
using Ui::Space;
using Ui::space;
namespace {
// Give feedback the full row once its recovery actions would squeeze it.
class SettingsStatusRow final:public Ui::Surface {
public:
 explicit SettingsStatusRow(QWidget* message,bool destination=false):Ui::Surface("bg",Qt::TopEdge){line_=new QBoxLayout(QBoxLayout::LeftToRight,this);line_->setContentsMargins(space(destination?Space::xxl:Space::l),space(Space::s),space(Space::l),space(Space::s));setMinimumHeight(Ui::scaled(56));line_->setSpacing(space(Space::m));line_->addWidget(message,1);buttons_=new QWidget(this);actions_=new QHBoxLayout(buttons_);actions_->setContentsMargins(0,0,0,0);actions_->setSpacing(space(Space::s));line_->addWidget(buttons_,0,Qt::AlignTop|Qt::AlignRight);}
 QHBoxLayout* actions()const{return actions_;}
 // A conflict takes the row as a warning banner that carries its own Reload action.
 void addBanner(QWidget* banner){line_->insertWidget(0,banner,1);}
protected:
 bool event(QEvent* event)override{if(event->type()==QEvent::Resize||event->type()==QEvent::LayoutRequest){int visible=0;for(int i=0;i<actions_->count();++i)if(auto* widget=actions_->itemAt(i)->widget();widget&&!widget->isHidden())++visible;const bool stack=width()-2*space(Space::l)-buttons_->sizeHint().width()-space(Space::m)<Ui::scaled(visible>1?480:300);const auto direction=stack?QBoxLayout::TopToBottom:QBoxLayout::LeftToRight;if(line_->direction()!=direction)line_->setDirection(direction);setProperty("stacked",stack);}return Ui::Surface::event(event);}
private:QBoxLayout* line_;QWidget* buttons_;QHBoxLayout* actions_;
};
class CompactPageHeading final:public QObject {
public:
 CompactPageHeading(QScrollArea* scroll,QWidget* heading,QLabel* description,bool provider=false):QObject(scroll),scroll_(scroll),heading_(heading),description_(description),provider_(provider){scroll->viewport()->installEventFilter(this);apply();}
 void refresh(){apply();}
protected:
 bool eventFilter(QObject*,QEvent* event)override{if(event->type()==QEvent::Resize||event->type()==QEvent::Show)apply();return false;}
private:
 void apply(){const bool compact=scroll_->viewport()->height()<Ui::scaled(560);auto* body=scroll_->widget();const QMargins insets(space(Space::xxl),qMin(Ui::pageHeaderTop(),space(Space::l)),space(Space::xxl),space(Space::xl));if(body->layout()->contentsMargins()!=insets)body->layout()->setContentsMargins(insets);body->layout()->setSpacing(space(Space::xxl));if(!provider_)for(auto* column:body->findChildren<QWidget*>(Qt::FindDirectChildrenOnly))if(auto* box=qobject_cast<QVBoxLayout*>(column->layout());box&&box->count()>1&&box->itemAt(1)->spacerItem()&&box->itemAt(1)->spacerItem()->sizeHint().height()!=space(compact?Space::l:Space::xl)){box->itemAt(1)->spacerItem()->changeSize(0,space(compact?Space::l:Space::xl),QSizePolicy::Minimum,QSizePolicy::Fixed);box->invalidate();}if(provider_){description_->setVisible(true);description_->setText(description_->property(compact?"compactText":"fullText").toString());}else description_->hide();if(heading_->property("compactHeading").toBool()!=compact){heading_->setProperty("compactHeading",compact);Ui::role(heading_,provider_?"editableTitle":"title");}}
 QScrollArea* scroll_;QWidget* heading_;QLabel* description_;bool provider_;
};
constexpr int protonFolderNameRole=Qt::UserRole+1;
void addProtonFolder(QComboBox* combo,const QString& uid,const QString& name){
 combo->addItem(name.isEmpty()?(uid.isEmpty()?"Choose a folder after connecting":"Saved Proton folder"):name,uid);
 combo->setItemData(combo->count()-1,name,protonFolderNameRole);
}
QString title(const QString& s){return s.left(1).toUpper()+s.mid(1);}
// Settings tabs, in order: General, Capture, Shortcuts, Advanced.
constexpr int advancedTab=3;
const QString themePath=QStringLiteral("/preferences/appearance/theme");
// The shared command names (main window, menu, tray and shortcuts).
QString actionLabel(const QString& action){return Settings::actionLabel(action);}
// Sharing and link copying live together under the default capture output.
QString sharingLabel(const QString& mode){return mode=="share"?QString("Create a share link"):mode=="private"?QString("Keep private"):QString("Destination default");}
QString storageClassLabel(const QString& value){
 static const QHash<QString,QString> labels{{"STANDARD","Standard storage"},{"REDUCED_REDUNDANCY","Reduced redundancy storage"},{"STANDARD_IA","Standard infrequent access"},{"ONEZONE_IA","One Zone infrequent access"},{"INTELLIGENT_TIERING","Intelligent-Tiering storage"},{"GLACIER","Glacier Flexible Retrieval"},{"DEEP_ARCHIVE","Glacier Deep Archive"},{"GLACIER_IR","Glacier Instant Retrieval"},{"EXPRESS_ONEZONE","S3 Express One Zone"},{"OUTPOSTS","S3 Outposts storage"},{"SNOW","S3 Snow storage"},{"FSX_OPENZFS","FSx OpenZFS storage"}};return labels.value(value,value);
}
QComboBox* choices(QWidget* parent,const QString& name,const QStringList& values){auto* c=Ui::choiceBox(parent);c->setObjectName(name);c->setAccessibleName(name);c->addItems(values);return c;}
// QFormLayout can compress a wrapped field below its layout's minimum height.
class FormRow final:public QWidget {
protected:
 bool event(QEvent* event)override{const bool handled=QWidget::event(event);if(layout()&&(event->type()==QEvent::Resize||event->type()==QEvent::LayoutRequest||event->type()==QEvent::Show)){const int floor=property("preferenceRow").toBool()&&!property("preferenceNaturalHeight").toBool()?Ui::scaled(Ui::Metric::control)+2*space(Space::s):0;const int height=std::max({floor,layout()->totalMinimumSize().height(),layout()->totalHeightForWidth(width())});if(minimumHeight()!=height)setMinimumHeight(height);}return handled;}
};
// Endpoint display wraps long host names without changing selectable text.
class EndpointLabel final:public QLabel {
public:
 int heightForWidth(int width)const override{auto document=makeDocument(width);return qCeil(document->size().height());}
 QSize minimumSizeHint()const override{return {0,fontMetrics().height()};}
protected:
 void paintEvent(QPaintEvent*)override{auto document=makeDocument(width());QPainter painter(this);QAbstractTextDocumentLayout::PaintContext context;context.palette=palette();context.palette.setColor(QPalette::Text,palette().color(QPalette::WindowText));if(hasSelectedText()){QAbstractTextDocumentLayout::Selection selection;selection.cursor=QTextCursor(document.get());selection.cursor.setPosition(selectionStart());selection.cursor.setPosition(selectionStart()+selectedText().size(),QTextCursor::KeepAnchor);selection.format.setBackground(palette().highlight());selection.format.setForeground(palette().highlightedText());context.selections.append(selection);}document->documentLayout()->draw(&painter,context);}
 void mousePressEvent(QMouseEvent* event)override{if(event->button()!=Qt::LeftButton){QLabel::mousePressEvent(event);return;}anchor_=position(event->position());setSelection(anchor_,0);selecting_=true;update();}
 void mouseMoveEvent(QMouseEvent* event)override{if(!selecting_){QLabel::mouseMoveEvent(event);return;}const int end=position(event->position());setSelection(qMin(anchor_,end),qAbs(end-anchor_));update();}
 void mouseReleaseEvent(QMouseEvent* event)override{if(event->button()==Qt::LeftButton&&selecting_){selecting_=false;if(qApp->clipboard()->supportsSelection())qApp->clipboard()->setText(selectedText(),QClipboard::Selection);return;}QLabel::mouseReleaseEvent(event);}
private:
 int anchor_=0;bool selecting_=false;
 int position(const QPointF& point)const{auto document=makeDocument(width());document->size();return qBound(0,document->documentLayout()->hitTest(point,Qt::FuzzyHit),int(text().size()));}
 std::unique_ptr<QTextDocument> makeDocument(int width)const{auto document=std::make_unique<QTextDocument>();document->setDocumentMargin(0);document->setDefaultFont(font());QTextOption option;option.setWrapMode(QTextOption::WrapAnywhere);option.setAlignment(alignment());document->setDefaultTextOption(option);document->setPlainText(text());document->setTextWidth(qMax(1,width));return document;}
};
// Captions request their painted height after the row assigns the wrap width.
class PreferenceCaption final:public QLabel {
public:
 explicit PreferenceCaption(const QString& text):QLabel(text){setProperty("uiStatus",true);setWordWrap(true);Ui::role(this,"secondary");}
 void refreshHeight(){const int needed=qMax(fontMetrics().height(),heightForWidth(width()));if(minimumHeight()!=needed)setMinimumHeight(needed);}
protected:
 void resizeEvent(QResizeEvent* event)override{QLabel::resizeEvent(event);refreshHeight();}
};
// Keep the real checkbox's text and accessibility contract while placing its indicator at the row edge.
class PreferenceCheck final:public QCheckBox {
public:
 using QCheckBox::QCheckBox;
 QSize sizeHint()const override{return property("preferenceIndicator").toBool()?QSize(Ui::scaled(24),Ui::scaled(Ui::Metric::control)):QCheckBox::sizeHint();}
protected:
 void paintEvent(QPaintEvent* event)override{if(!property("preferenceIndicator").toBool()){QCheckBox::paintEvent(event);return;}QStyleOptionButton option;initStyleOption(&option);option.text.clear();const int side=style()->pixelMetric(QStyle::PM_IndicatorWidth,&option,this);option.rect=QRect((width()-side)/2,(height()-side)/2,side,side);QPainter p(this);style()->drawPrimitive(QStyle::PE_IndicatorCheckBox,&option,&p,this);if(hasFocus()&&Ui::keyboardFocus(this)){p.setPen(Theme::current().accent);p.setBrush(Qt::NoBrush);p.drawRoundedRect(QRectF(rect()).adjusted(.5,.5,-.5,-.5),4,4);}}
 bool hitButton(const QPoint& point)const override{return rect().contains(point);}
};
// Moving a widget between layouts: delete its old item before adding it to the new layout.
// A QWidgetItemV2 only registers with a widget that has no item, and deleting the old item
// afterwards unregisters the widget, so the new item's height-for-width cache never clears and
// a wrapped row keeps an old height (#1721).
// These containers change minimum height while hidden; avoid QWidgetItemV2's stale width cache.
void addPreferenceWidget(QBoxLayout* layout,QWidget* widget){layout->addWidget(widget);delete layout->takeAt(layout->count()-1);layout->addItem(new QWidgetItem(widget));}
class PreferenceColumnSizer final:public QObject {
public:
 PreferenceColumnSizer(QWidget* column,QWidget* viewport):QObject(column),column_(column),viewport_(viewport){viewport->installEventFilter(this);apply();}
protected:
 bool eventFilter(QObject*,QEvent* event)override{if(event->type()==QEvent::Resize||event->type()==QEvent::Show)apply();return false;}
private:
 void apply(){column_->setFixedWidth(qMax(0,qMin(Ui::scaled(760),viewport_->width()-2*space(Space::xxl))));}
 QWidget* column_;QWidget* viewport_;
};
class PreferenceLabel final:public QLabel {
public:
 QSize sizeHint()const override{return {fontMetrics().horizontalAdvance(text()),fontMetrics().height()};}
 PreferenceLabel(PreferenceCheck* check):QLabel(check->text()),check_(check){setBuddy(check);setWordWrap(true);check->installEventFilter(this);setEnabled(check->isEnabled());}
protected:
 bool eventFilter(QObject*,QEvent* event)override{if(event->type()==QEvent::EnabledChange)setEnabled(check_->isEnabled());return false;}
private:PreferenceCheck* check_;
};
// The whole checkbox row activates on a completed click; help keeps its own target.
class PreferenceCheckRow final:public QObject {
public:
 PreferenceCheckRow(PreferenceCheck* check,QWidget* row):QObject(row),check_(check),row_(row){row_->installEventFilter(this);for(auto* child:row_->findChildren<QWidget*>())child->installEventFilter(this);}
protected:
 bool eventFilter(QObject* watched,QEvent* event)override{
  auto* widget=qobject_cast<QWidget*>(watched);if(!widget)return false;
  if(event->type()==QEvent::UngrabMouse||event->type()==QEvent::Hide)pressed_=false;
  if(event->type()!=QEvent::MouseButtonPress&&event->type()!=QEvent::MouseButtonRelease)return false;
  auto* mouse=static_cast<QMouseEvent*>(event);if(mouse->button()!=Qt::LeftButton)return false;
  const auto point=row_->mapFromGlobal(mouse->globalPosition().toPoint());auto* target=row_->childAt(point);bool button=false;for(auto* child=target;child&&child!=row_;child=child->parentWidget())button|=qobject_cast<QAbstractButton*>(child)!=nullptr;
  if(event->type()==QEvent::MouseButtonPress){pressed_=check_->isEnabled()&&!button&&row_->rect().contains(point);press_=point;return pressed_;}
  const bool activate=pressed_&&check_->isEnabled()&&!button&&row_->rect().contains(point)&&(point-press_).manhattanLength()<=QApplication::startDragDistance();const bool handled=pressed_;pressed_=false;if(activate){check_->setFocus(Qt::MouseFocusReason);check_->click();}return handled;
 }
private:PreferenceCheck* check_;QWidget* row_;bool pressed_=false;QPoint press_;
};
// Measure the caption before wrapping; its help stays beside the first line.
class PreferenceCheckCaption final:public QWidget {
public:
 PreferenceCheckCaption(QWidget* label,QWidget* help):label_(static_cast<QLabel*>(label)),help_(help){label_->setParent(this);help_->setParent(this);label_->show();help_->show();label_->installEventFilter(this);help_->installEventFilter(this);label_->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Preferred);setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Preferred);auto policy=sizePolicy();policy.setHeightForWidth(true);setSizePolicy(policy);}
 QSize sizeHint()const override{return {label_->fontMetrics().horizontalAdvance(label_->text())+space(Space::xs)+help_->size().width(),qMax(label_->fontMetrics().height(),help_->size().height())};}
 QSize minimumSizeHint()const override{return {help_->size().width()+space(Space::xs),sizeHint().height()};}
 int heightForWidth(int width)const override{const int available=qMax(1,width-space(Space::xs)-help_->width());const auto metrics=label_->fontMetrics();const int height=available>=metrics.horizontalAdvance(label_->text())?metrics.height():metrics.boundingRect(QRect(0,0,available,10000),Qt::TextWordWrap,label_->text()).height();return qMax(help_->height(),height);}
protected:
 bool event(QEvent* event)override{const bool handled=QWidget::event(event);if(event->type()==QEvent::LayoutRequest)updateGeometry();return handled;}
 bool eventFilter(QObject*,QEvent* event)override{if(event->type()==QEvent::FontChange||event->type()==QEvent::StyleChange)updateGeometry();return false;}
 void resizeEvent(QResizeEvent* event)override{QWidget::resizeEvent(event);const auto helpSize=help_->size();const int advance=label_->fontMetrics().horizontalAdvance(label_->text());const int available=qMax(1,width()-helpSize.width()-space(Space::xs));const int textWidth=qMin(advance,available);label_->setWordWrap(available<advance);const int textHeight=qMax(label_->fontMetrics().height(),label_->heightForWidth(textWidth));const int textY=qMax(0,(height()-textHeight)/2);label_->setGeometry(0,textY,textWidth,textHeight);QTextLayout text(label_->text(),label_->font());QTextOption option;option.setWrapMode(QTextOption::WordWrap);text.setTextOption(option);text.beginLayout();auto line=text.createLine();line.setLineWidth(textWidth);text.endLayout();const int firstLine=available>=advance?advance:qRound(line.naturalTextWidth());help_->setGeometry(firstLine+space(Space::xs),qMax(0,textY+(label_->fontMetrics().height()-helpSize.height())/2),helpSize.width(),helpSize.height());}
private:QLabel* label_;QWidget* help_;
};
void boxPreferenceRows(QFormLayout* form){
 form->setVerticalSpacing(0);form->setHorizontalSpacing(0);
 for(int i=0;i<form->rowCount();++i){
  auto* item=form->itemAt(i,QFormLayout::FieldRole);if(!item)continue;if(item->layout()){const bool shown=form->isRowVisible(i);auto taken=form->takeRow(i);auto* wrapper=new FormRow;wrapper->setLayout(taken.fieldItem->layout());delete taken.labelItem;form->insertRow(i,wrapper);form->setRowVisible(wrapper,shown);item=form->itemAt(i,QFormLayout::FieldRole);}if(!item->widget())continue;auto* field=item->widget();auto* labelItem=form->itemAt(i,QFormLayout::LabelRole);auto* label=labelItem?labelItem->widget():nullptr;
  auto* check=dynamic_cast<PreferenceCheck*>(field->findChild<QCheckBox*>());
  auto* column=qobject_cast<QVBoxLayout*>(field->layout());
  if(label&&!field->layout()){const bool shown=form->isRowVisible(i);auto taken=form->takeRow(i);delete taken.labelItem;delete taken.fieldItem;auto* wrapper=new FormRow;auto* box=new QVBoxLayout(wrapper);box->addWidget(field);field->setProperty("preferenceRowContainer",QVariant::fromValue(static_cast<QObject*>(wrapper)));wrapper->setProperty("preferenceReadout",true);wrapper->setProperty("preferenceResponsive",field->property("preferenceResponsive"));field=wrapper;form->insertRow(i,field);form->setRowVisible(field,shown);labelItem=nullptr;column=box;}
  if(label||check){
   if(!column){auto* value=new QWidget;value->setLayout(field->layout());column=new QVBoxLayout(field);column->addWidget(value);}
   auto* value=new FormRow;value->setObjectName(field->objectName()+"PreferenceValue");QList<QWidget*> fullRows,errors;auto* values=new QVBoxLayout(value);values->setContentsMargins(0,0,0,0);values->setSpacing(space(Space::xs));while(auto* part=column->takeAt(0)){if(auto* widget=part->widget()){delete part;if(widget->property("visualRole")=="error")errors.append(widget);else if(widget->objectName()=="captureOutputBlock"){auto* notice=widget->findChild<QLabel*>("captureOutputNotice");widget->layout()->removeWidget(notice);values->addWidget(notice);fullRows.append(widget);}else values->addWidget(widget);}else if(auto* nested=part->layout())values->addLayout(nested);else values->addItem(part);}
   if(check){label=new PreferenceLabel(check);check->setProperty("preferenceIndicator",true);check->setFixedWidth(Ui::scaled(24));if(auto* line=qobject_cast<QHBoxLayout*>(values->itemAt(0)->layout())){for(int n=line->count()-1;n>=0;--n){auto* part=line->itemAt(n);if(part->spacerItem()){delete line->takeAt(n);continue;}if(auto* help=part->widget();help&&help!=check){line->removeWidget(help);label=new PreferenceCheckCaption(label,help);}}line->insertStretch(0,1);}}
   if(label){field->setProperty("preferenceLabel",QVariant::fromValue(static_cast<QObject*>(label)));label->setMinimumWidth(0);label->setMaximumWidth(QWIDGETSIZE_MAX);label->setMinimumHeight(0);label->setMaximumHeight(QWIDGETSIZE_MAX);for(auto* text:label->findChildren<QLabel*>())text->setWordWrap(true);}
   if(labelItem){const bool shown=form->isRowVisible(i);auto taken=form->takeRow(i);delete taken.labelItem;delete taken.fieldItem;form->insertRow(i,field);form->setRowVisible(field,shown);}
   auto* captions=new QWidget;auto* captionBox=new QVBoxLayout(captions);captionBox->setContentsMargins(0,0,0,0);captionBox->setSpacing(space(Space::xs));captionBox->addWidget(label);captionBox->setAlignment(Qt::AlignTop);label->show();for(auto* hint:value->findChildren<QLabel*>(Qt::FindDirectChildrenOnly))if((check&&hint->objectName().endsWith("Hint"))||hint->objectName()=="debugLoggingStatus"){values->removeWidget(hint);captionBox->addWidget(hint);}label=captions;
   const auto edits=value->findChildren<QLineEdit*>();const bool numeric=!value->findChildren<QAbstractSpinBox*>().isEmpty()||std::any_of(edits.cbegin(),edits.cend(),[](auto* edit){return edit->property("defaultText").isValid()||edit->property("hasUnit").toBool();});
   const bool technicalField=field->property("preferenceStacked").toBool()||(!field->property("preferenceInline").toBool()&&!check&&!edits.isEmpty()&&!numeric&&value->findChildren<QComboBox*>().isEmpty());
   const bool stacked=false;
   if(!stacked)while(captionBox->count()>1){auto* hint=captionBox->takeAt(1);fullRows.append(hint->widget());delete hint;}
   captions->layout()->itemAt(0)->widget()->setMinimumHeight(stacked?0:Ui::scaled(Ui::Metric::control));
   const bool choice=!check&&!stacked&&(!value->findChildren<QComboBox*>().isEmpty()||field->property("preferenceInline").toBool());const bool compactValue=numeric&&!choice&&value->findChildren<QAbstractSpinBox*>().isEmpty();if(compactValue){for(auto* edit:edits)if(!qobject_cast<QComboBox*>(edit->parentWidget())&&!qobject_cast<QAbstractSpinBox*>(edit->parentWidget())){edit->setFixedWidth(Ui::scaled(120));edit->setAlignment(Qt::AlignRight);}value->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Preferred);for(auto* suffix:value->findChildren<QLabel*>())if(suffix->property("visualRole")=="unitSuffix")suffix->parentWidget()->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Preferred);}
   field->setProperty("preferenceStacked",stacked);value->setProperty("preferenceChoiceValue",choice);value->setProperty("preferenceInlineValue",!stacked&&!check&&!compactValue);
   if(choice)for(auto* line:value->findChildren<QHBoxLayout*>()){for(int n=line->count()-1;n>=0;--n)if(line->itemAt(n)->spacerItem())delete line->takeAt(n);for(int n=0;n<line->count();++n)if(auto* part=line->itemAt(n)->widget();part&&(qobject_cast<QComboBox*>(part)||part->findChild<QComboBox*>()))line->setStretch(n,1);}
   if(stacked){column->addWidget(label);column->addWidget(value);}
   else{auto* line=new QHBoxLayout;line->setContentsMargins(0,0,0,0);line->setSpacing(space(Space::l));line->addWidget(label,check||choice||compactValue?1:0,Qt::AlignTop);line->addWidget(value,check||choice||compactValue?0:1,compactValue?Qt::AlignRight|Qt::AlignTop:Qt::AlignTop);if(check)value->setFixedWidth(Ui::scaled(24));column->addLayout(line);}
   for(auto* full:fullRows)column->addWidget(full);for(auto* error:errors)column->addWidget(error);label->show();value->show();if(check)new PreferenceCheckRow(check,field);
   for(auto* control:value->findChildren<QWidget*>())if((qobject_cast<QLineEdit*>(control)&&!qobject_cast<QAbstractSpinBox*>(control->parentWidget()))||qobject_cast<QComboBox*>(control)){control->setProperty("uiFullWidth",true);control->setMinimumWidth(0);if(!numeric||qobject_cast<QComboBox*>(control))control->setMaximumWidth(QWIDGETSIZE_MAX);if(technicalField&&qobject_cast<QLineEdit*>(control))Ui::role(control,"technical");if(auto* combo=qobject_cast<QComboBox*>(control)){combo->setProperty("preferenceChoice",choice);combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);}for(auto* line:value->findChildren<QLayout*>())line->setAlignment(control,{});control->show();}
   if(!check&&!choice&&!stacked&&!numeric)for(auto* text:value->findChildren<QLabel*>()){{const bool left=text->objectName()=="archiveRootHint"||text->objectName()=="updateStatus"||text->objectName().endsWith("Applicability");text->setAlignment((left?Qt::AlignLeft:Qt::AlignRight)|Qt::AlignVCenter);text->setProperty("preferenceReadoutText",!left);}if(text->property("visualRole")!="technical")Ui::role(text,"secondary");}
  }
  if(field->property("preferenceRowInset").toBool())continue;if(field->findChild<QFormLayout*>())continue;
  if(column){column->setSizeConstraint(QLayout::SetMinimumSize);column->setContentsMargins(space(Space::l),space(Space::s),space(Space::l),space(Space::s));column->setSpacing(space(field->property("preferenceStacked").toBool()?Space::s:Space::xs));}else if(!field->findChild<QFormLayout*>())field->setContentsMargins(space(Space::l),space(Space::m),space(Space::l),space(Space::m));
  if(auto* disclosure=qobject_cast<QToolButton*>(field);disclosure&&disclosure->property("uiDisclosure").toBool()){disclosure->setProperty("preferenceDisclosure",true);disclosure->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);}
  field->setProperty("preferenceRow",true);field->setMinimumHeight(std::max({field->minimumHeight(),Ui::scaled(Ui::Metric::control)+2*space(Space::s),field->layout()?field->layout()->totalMinimumSize().height():0}));
 }
}
// Connection settings are bounded by the provider's integer millisecond/count limits.
qint64 connectionUnit(const QString& key){return key.endsWith("Ms")?1000:key=="lowSpeedBytesPerSecond"?1024:1;}
// Any progress (0 KB/s) is the provider's lowest stall threshold, 1 byte per second.
QJsonValue connectionValue(const QString& key,const QString& text){const auto value=ByteSize::fromDecimal(text,connectionUnit(key));if(key=="lowSpeedBytesPerSecond"&&value.isDouble()&&value.toInteger()==0)return 1;return value;}

class ConnectionSpin final:public QDoubleSpinBox {
public:
 // Rates are whole KB/s; the lowest reads "Any progress" rather than a byte fraction.
 explicit ConnectionSpin(qint64 factor,QWidget* parent=nullptr):QDoubleSpinBox(parent){setLocale(QLocale::c());setDecimals(factor==1000?3:0);setRange(factor==1024?0.0:1.0/factor,factor==1024?std::floor(2147483647.0/factor):2147483647.0/factor);if(factor==1024)setSpecialValueText("Any progress");setKeyboardTracking(false);}
 QLineEdit* editor()const{return lineEdit();}
protected:
 QString textFromValue(double value)const override{auto text=QString::number(value,'f',decimals());if(text.contains('.')){while(text.endsWith('0'))text.chop(1);if(text.endsWith('.'))text.chop(1);}return text;}
};
// A shown value that still matches the loaded one keeps the stored, possibly finer, value.
QString inputText(QLineEdit* field){if(auto* spin=qobject_cast<QDoubleSpinBox*>(field->parentWidget())){const auto text=spin->cleanText();if(spin->property("exactText").isValid()&&text==spin->property("exactShown").toString())return spin->property("exactText").toString();return !spin->specialValueText().isEmpty()&&text==spin->specialValueText()?QString::number(spin->minimum()):text;}return field->text();}
void setInputText(QLineEdit* field,const QString& text){if(auto* spin=qobject_cast<QDoubleSpinBox*>(field->parentWidget())){bool valid=false;const double number=text.toDouble(&valid);// Round to the shown precision first: Qt 6.10 keeps unrounded values that cleanText then prints in full.
 spin->setProperty("exactText",QVariant());if(valid){const double scale=std::pow(10.0,spin->decimals());const double shown=std::max(spin->minimum(),std::round(number*scale)/scale);if(shown<=spin->maximum()){spin->setValue(shown);spin->setProperty("exactText",text);spin->setProperty("exactShown",spin->cleanText());return;}}}field->setText(text);}
class FieldActionsSizer final:public QObject {
public:
 FieldActionsSizer(QWidget* owner,QBoxLayout* line,QWidget* value,QWidget* actions):QObject(owner),line_(line),actions_(actions){owner->installEventFilter(this);value->installEventFilter(this);actions->installEventFilter(this);}
protected:
 bool eventFilter(QObject*,QEvent* event)override{if(event->type()==QEvent::Resize||event->type()==QEvent::Show||event->type()==QEvent::LayoutRequest){auto* parent=line_->parentWidget();if(parent&&parent!=widthOwner_){widthOwner_=parent;parent->installEventFilter(this);}const int room=parent?parent->contentsRect().width():line_->geometry().width();const bool stacked=room<Ui::scaled(240)+actions_->sizeHint().width()+space(Space::s);line_->setDirection(stacked?QBoxLayout::TopToBottom:QBoxLayout::LeftToRight);line_->setStretch(0,1);line_->setStretch(1,0);line_->setAlignment(actions_,Qt::AlignRight);}return false;}
private:QBoxLayout* line_;QWidget* actions_;QWidget* widthOwner_=nullptr;
};
class AccountActionSizer final:public QObject {
public:
 AccountActionSizer(QWidget* row,QBoxLayout* line,QLayout* captions,QWidget* action):QObject(row),row_(row),line_(line),captions_(captions),action_(action){row->installEventFilter(this);action->installEventFilter(this);}
protected:
 bool eventFilter(QObject*,QEvent* event)override{if(event->type()==QEvent::Resize||event->type()==QEvent::Show||event->type()==QEvent::LayoutRequest||event->type()==QEvent::FontChange){const auto margins=row_->layout()->contentsMargins();const int room=row_->width()-margins.left()-margins.right();const int needed=captions_->sizeHint().width()+action_->sizeHint().width()+space(Space::l);line_->setDirection(room>=needed?QBoxLayout::LeftToRight:QBoxLayout::TopToBottom);}return false;}
private:QWidget* row_;QBoxLayout* line_;QLayout* captions_;QWidget* action_;
};
class PreferenceChoiceRow final:public QObject {
public:
 PreferenceChoiceRow(QWidget* value,QWidget* field,QBoxLayout* line,int width,int gutter,bool compact):QObject(value),value_(value),field_(field),line_(line),compact_(compact){field->installEventFilter(this);setWidths(width,gutter);}
 void setWidths(int width,int gutter,bool stacked=false){width_=width;gutter_=gutter;stacked_=stacked;apply();}
protected:
 bool eventFilter(QObject*,QEvent* event)override{if(event->type()==QEvent::Resize||event->type()==QEvent::Show)apply();return false;}
private:
 void apply(){const auto margins=field_->layout()->contentsMargins();const int available=qMax(0,field_->width()-margins.left()-margins.right());const bool stacked=!compact_&&stacked_;const auto direction=stacked?QBoxLayout::TopToBottom:QBoxLayout::LeftToRight;if(line_->direction()!=direction)line_->setDirection(direction);auto* captions=line_->itemAt(0)->widget();captions->setMinimumWidth(stacked||compact_?0:gutter_-space(Space::l));captions->setMaximumWidth(stacked||compact_?QWIDGETSIZE_MAX:gutter_-space(Space::l));if(auto* label=qobject_cast<QWidget*>(field_->property("preferenceLabel").value<QObject*>()))label->setMinimumHeight(stacked?0:Ui::scaled(Ui::Metric::control));value_->setMinimumWidth(0);value_->setMaximumWidth(compact_?qMin(width_,available):QWIDGETSIZE_MAX);if(compact_)value_->setMinimumWidth(qMin(width_,available));line_->setStretch(0,compact_?1:0);line_->setStretch(1,compact_?0:1);field_->setProperty("stacked",stacked);// A read-only value sits under its label when stacked, at the row's end inline.
 for(auto* text:value_->findChildren<QLabel*>())if(text->property("preferenceReadoutText").toBool())text->setAlignment((stacked?Qt::AlignLeft:Qt::AlignRight)|Qt::AlignVCenter);line_->setSpacing(space(stacked?Space::s:Space::l));line_->setAlignment(value_,compact_?Qt::AlignRight|Qt::AlignTop:Qt::AlignTop);
  // A newly shown hint can leave an existing value rectangle with undersized
  // child geometry. Refresh the inner layout once it has enough room.
  if(auto* layout=value_->layout();layout&&value_->isVisible()&&value_->height()>=layout->totalMinimumSize().height())
   for(int i=0;i<layout->count();++i)if(auto* item=layout->itemAt(i);!item->isEmpty()&&item->geometry().height()<item->minimumSize().height()){layout->invalidate();break;}
 }
 QWidget* value_;QWidget* field_;QBoxLayout* line_;bool compact_;bool stacked_=false;int width_=0,gutter_=0;
};
// Keep half the row for the captions and permit two helper lines before stacking.
class ConnectionTestSizer final:public QObject {
public:
 ConnectionTestSizer(QWidget* row,QBoxLayout* line,QLabel* caption,QWidget* control):QObject(row),row_(row),line_(line),caption_(caption),control_(control){row->installEventFilter(this);caption->installEventFilter(this);control->installEventFilter(this);}
protected:
 bool eventFilter(QObject*,QEvent* event)override{if(event->type()==QEvent::Resize||event->type()==QEvent::Show||event->type()==QEvent::LayoutRequest){const int available=row_->width()-2*space(Space::l),gap=space(Space::l),control=control_->sizeHint().width(),width=available-gap-control;const bool stack=width<(available-gap)/2||caption_->heightForWidth(qMax(1,width))>2*caption_->fontMetrics().height();const auto direction=stack?QBoxLayout::TopToBottom:QBoxLayout::LeftToRight;if(line_->direction()!=direction)line_->setDirection(direction);row_->setProperty("stacked",stack);}return false;}
private:QWidget* row_;QBoxLayout* line_;QLabel* caption_;QWidget* control_;
};
class PreferenceRadio final:public QRadioButton {
public:using QRadioButton::QRadioButton;
 QSize sizeHint()const override{return QSize(Ui::scaled(24),Ui::scaled(Ui::Metric::control));}
protected:
 void paintEvent(QPaintEvent*)override{QStyleOptionButton option;initStyleOption(&option);option.text.clear();option.rect=QRect(contentsRect().topLeft(),QSize(width(),Ui::scaled(18)));QPainter painter(this);style()->drawControl(QStyle::CE_RadioButton,&option,&painter,this);}
 bool hitButton(const QPoint& point)const override{return rect().contains(point);}
};
class FormInput final:public QLineEdit {
public:using QLineEdit::QLineEdit;
 QSize sizeHint()const override{auto size=QLineEdit::sizeHint();if(!qobject_cast<QComboBox*>(parentWidget()))size.setWidth(qMin(maximumWidth(),Ui::scaled(480)));return size;}
};
QLineEdit* input(QWidget* parent,const QString& name){auto* e=new FormInput(parent);e->setObjectName(name);e->setAccessibleName(name);return e;}
// #1699: Cloudflare R2's Account ID, Jurisdiction and Bucket, which name one account endpoint and its
// bucket: a bucket never saves beside an account it wasn't entered with. An S3 or B2 destination has
// no Account ID or Jurisdiction, so for it this group is its Bucket alone.
QJsonObject destinationDefaults(const QString& provider,const QString& id);
const QStringList& r2Group(){static const QStringList group{"settings/accountId","settings/jurisdiction","settings/bucket"};return group;}
// Backblaze B2 settings edited through the S3 panel's shared rows (#282).
const QStringList& b2SharedKeys(){static const QStringList keys{"bucket","objectPrefix","publicUrlBase","connectTimeoutMs","requestTimeoutMs"};return keys;}
QJsonObject s3Profile(const QString& id){
 return {{"id",id},{"label","S3 destination"},{"provider","s3"},
  {"settings",QJsonObject{{"bucket",""},{"region","us-east-1"},{"endpoint","https://s3.us-east-1.amazonaws.com"},{"objectPrefix",""},{"storageClass","STANDARD"},{"aclMode","omit"},{"linkMode","public"},{"linkValidityDays",7},{"publicUrlBase",QJsonValue::Null},{"addressing","auto"},{"connectTimeoutMs",5000},{"requestTimeoutMs",60000},{"lowSpeedBytesPerSecond",1},{"lowSpeedTimeSeconds",30}}},
  {"credentials",QJsonObject{{"mode","keychain"},{"reference","putgd/"+id}}}};
}
// Drag and drop only proposes a move; the window reorders its draft and rebuilds
// the rows, so Qt never moves or removes items itself.
// Names get two lines; status wraps independently below them.
class DestinationRowDelegate final:public QStyledItemDelegate {
public:
 using QStyledItemDelegate::QStyledItemDelegate;
 QStringList nameLines(const QString& name,int width)const{const QFontMetrics metrics(Ui::labelFont(true));if(metrics.horizontalAdvance(name)<=width)return {name};int split=0;while(split<name.size()&&metrics.horizontalAdvance(name.left(split+1))<=width)++split;split=qMax(1,split);const int space=name.lastIndexOf(' ',split);if(space>0)split=space;return {name.left(split).trimmed(),metrics.elidedText(name.mid(split).trimmed(),Qt::ElideRight,width)};}
 QStyleOptionViewItem shown(const QStyleOptionViewItem& base,const QModelIndex& index)const{auto option=base;initStyleOption(&option,index);return option;}
 int textWidth(int width)const{return qMax(24,width-28-Ui::scaled(Ui::Metric::icon)-10);}
 int blockHeight(const QStringList& lines,int width)const{const QFontMetrics body(Ui::labelFont(true)),meta(Ui::font(Ui::Type::metadata));return int(nameLines(lines.value(0),width).size())*body.height()+(lines.value(1).isEmpty()?0:space(Space::xs)+PreferenceRows::destinationStateLines(lines.value(1),width).size()*meta.height());}
 QSize sizeHint(const QStyleOptionViewItem& option,const QModelIndex& index)const override{const int width=textWidth(option.rect.width()>0?option.rect.width():option.widget?option.widget->contentsRect().width():Ui::railWidth()-2*space(Space::l));return {0,qMax(Ui::scaled(56),blockHeight(index.data().toString().split('\n'),width)+2*space(Space::m))};}
 void paint(QPainter* p,const QStyleOptionViewItem& base,const QModelIndex& index)const override{
  auto option=shown(base,index);const auto lines=option.text.split(QChar::LineSeparator);const auto& t=Theme::current();
  const bool selected=option.state&QStyle::State_Selected,hover=option.state&QStyle::State_MouseOver,enabled=option.state&QStyle::State_Enabled;
  p->save();p->setRenderHint(QPainter::Antialiasing);const QRectF box=QRectF(option.rect).adjusted(2,2,-2,-2);
  if(selected||hover){p->setPen(Qt::NoPen);p->setBrush(selected?t.selected:Ui::mix(t.surface,t.surfaceRaised,0.7));p->drawRoundedRect(box,6,6);}
  if(selected)p->fillRect(QRectF(box.left(),box.top()+8,2,box.height()-16),t.accent);
  if(selected&&option.widget&&Ui::keyboardFocus(option.widget)){p->setPen(QPen(t.accent,1));p->setBrush(Qt::NoBrush);p->drawRoundedRect(box.adjusted(0.5,0.5,-0.5,-0.5),6,6);}
  const int width=textWidth(option.rect.width()),block=blockHeight(lines,width);const qreal top=option.rect.center().y()+0.5-block/2.0;
  qreal x=box.left()+12;if(!option.icon.isNull()){const int side=Ui::scaled(Ui::Metric::icon);option.icon.paint(p,QRect(int(x),qRound(top+block/2.0-side/2.0),side,side),Qt::AlignCenter,!enabled?QIcon::Disabled:selected?QIcon::Selected:QIcon::Normal);x+=Ui::scaled(Ui::Metric::icon)+10;}
  const QRectF text(x,top,width,block);const QFontMetrics body(Ui::labelFont(selected));
  const auto names=nameLines(lines.value(0),width);for(int line=0;line<names.size();++line)Ui::paintText(*p,QRectF(text.left(),top+line*body.height(),text.width(),body.height()),names[line],enabled?(selected?t.text:t.textSecondary):t.textDisabled,Ui::labelFont(selected),Qt::AlignLeft|Qt::AlignVCenter);
  const auto states=PreferenceRows::destinationStateLines(lines.value(1),width);const auto meta=Ui::font(Ui::Type::metadata);const int metaHeight=QFontMetrics(meta).height();for(int line=0;line<states.size();++line)Ui::paintText(*p,QRectF(text.left(),top+names.size()*body.height()+space(Space::xs)+line*metaHeight,text.width(),metaHeight),states[line],enabled?t.textMuted:t.textDisabled,meta,Qt::AlignLeft|Qt::AlignVCenter);
  p->restore();
 }
protected:
 void initStyleOption(QStyleOptionViewItem* option,const QModelIndex& index)const override{
  QStyledItemDelegate::initStyleOption(option,index);
  // displayText() has already turned the label/state newline into a line separator.
  auto lines=option->text.split(QChar::LineSeparator);if(lines.size()<2)return;
  option->features&=~QStyleOptionViewItem::WrapText;

  option->text=lines.join(QChar::LineSeparator);
 }
};
// Configuration metadata: shown with display-only break points (after a slash
// and every 16 hex digits), but anything copied from it, by keyboard, menu or
// the X11 selection, is the exact original text.
class MetadataLabel final:public QLabel {
public:
 using QLabel::QLabel;
 void setExact(const QString& original){
  original_=original;inserted_.clear();QString shown;static const QRegularExpression hex(QStringLiteral("[0-9a-fA-F]"));int run=0;
  for(int i=0;i<original.size();++i){const auto c=original[i];shown+=c;inserted_.append(false);run=hex.match(QString(c)).hasMatch()?run+1:0;
   const bool nextHex=i+1<original.size()&&hex.match(QString(original[i+1])).hasMatch();
   if(c=='/'||(run==16&&nextHex)){shown+=QChar(0x200B);inserted_.append(true);if(run==16)run=0;}}
  setText(shown);setAccessibleName(original);setToolTip(original);
 }
 QString exactSelection()const{
  const int start=selectionStart();if(start<0)return {};const int length=selectedText().size();QString out;
  for(int i=start;i<start+length&&i<inserted_.size();++i)if(!inserted_[i])out+=text().at(i);
  return out;
 }
protected:
 void keyPressEvent(QKeyEvent* event)override{if(event->matches(QKeySequence::Copy)&&hasSelectedText()){QApplication::clipboard()->setText(exactSelection());return;}QLabel::keyPressEvent(event);}
 void mouseReleaseEvent(QMouseEvent* event)override{QLabel::mouseReleaseEvent(event);if(hasSelectedText()&&QApplication::clipboard()->supportsSelection())QApplication::clipboard()->setText(exactSelection(),QClipboard::Selection);}
 void contextMenuEvent(QContextMenuEvent* event)override{
  QMenu menu(this);auto* copy=menu.addAction("Copy",[this]{QApplication::clipboard()->setText(hasSelectedText()?exactSelection():original_);});copy->setShortcut(QKeySequence::Copy);
  menu.addAction("Select all",[this]{setSelection(0,text().size());});menu.exec(event->globalPos());
 }
private:
 QString original_;QVector<bool> inserted_;
};
class DestinationList final:public QListWidget {
public:
 std::function<void(int from,int to)> moveRequested;
 DestinationList(){setDragEnabled(true);setAcceptDrops(true);setDragDropMode(QAbstractItemView::InternalMove);setDefaultDropAction(Qt::MoveAction);setDropIndicatorShown(false);setItemDelegate(new DestinationRowDelegate(this));setTextElideMode(Qt::ElideRight);}
 // A slim, slightly translucent label beside and below the pointer, so the drop
 // line within half a row of it stays visible (#133).
 static QPixmap dragImage(const QListWidgetItem& item,const QFont& font,qreal ratio){
  const QFontMetrics metrics(font);const auto label=item.text().section('\n',0,0);
  const int height=metrics.height()+16,width=qMin(220,8+16+8+metrics.horizontalAdvance(label)+12);
  QPixmap image(QSize(width,height)*ratio);image.setDevicePixelRatio(ratio);image.fill(Qt::transparent);
  QPainter painter(&image);painter.setRenderHint(QPainter::Antialiasing);painter.setOpacity(0.86);
  painter.setPen(Qt::NoPen);painter.setBrush(Theme::current().surfaceRaised);painter.drawRoundedRect(QRectF(0,0,width,height),7,7);
  item.icon().paint(&painter,QRect(8,(height-16)/2,16,16));
  painter.setFont(font);painter.setPen(Theme::current().text);
  painter.drawText(QRect(32,0,width-32-8,height),Qt::AlignVCenter|Qt::AlignLeft,metrics.elidedText(label,Qt::ElideRight,width-32-8));
  return image;
 }
 static constexpr QPoint dragOffset{14,12};
protected:
 void startDrag(Qt::DropActions)override{
  auto* item=currentItem();if(!item)return;dragRow_=row(item);
  auto* drag=new QDrag(this);drag->setMimeData(model()->mimeData(selectedIndexes()));
  drag->setPixmap(dragImage(*item,font(),devicePixelRatioF()));drag->setHotSpot(-dragOffset);
  drag->exec(Qt::MoveAction);
  dragRow_=-1;setGap(-1);
 }
 void dragEnterEvent(QDragEnterEvent* event)override{if(event->source()!=this){event->ignore();return;}QListWidget::dragEnterEvent(event);event->acceptProposedAction();}
 void dragMoveEvent(QDragMoveEvent* event)override{
  if(event->source()!=this){event->ignore();return;}
  QListWidget::dragMoveEvent(event);event->setDropAction(Qt::MoveAction);event->accept();setGap(gapAt(event->position().toPoint()));
 }
 void dragLeaveEvent(QDragLeaveEvent* event)override{QListWidget::dragLeaveEvent(event);setGap(-1);}
 void dropEvent(QDropEvent* event)override{
  stopAutoScroll();setState(NoState);const int gap=gapAt(event->position().toPoint()),from=dragRow_;setGap(-1);
  if(event->source()!=this||from<0){event->ignore();return;}
  event->setDropAction(Qt::MoveAction);event->accept();
  const int to=gap>from?gap-1:gap;
  // Rebuild the rows after the drag has finished with them.
  if(to!=from&&moveRequested)QTimer::singleShot(0,this,[this,from,to]{if(moveRequested)moveRequested(from,to);});
 }
 void keyPressEvent(QKeyEvent* event)override{
  if(event->modifiers()==Qt::AltModifier&&(event->key()==Qt::Key_Up||event->key()==Qt::Key_Down)){
   const int from=currentRow(),to=from+(event->key()==Qt::Key_Up?-1:1);
   if(from>=0&&to>=0&&to<count()&&moveRequested)moveRequested(from,to);
   event->accept();return;
  }
  QListWidget::keyPressEvent(event);
 }
 void paintEvent(QPaintEvent* event)override{
  QListWidget::paintEvent(event);
  if(gap_<0||!count())return;
  // A theme-accent insertion line between rows, with end caps like a caret.
  const auto above=gap_>0?visualItemRect(item(gap_-1)):QRect{},below=gap_<count()?visualItemRect(item(gap_)):QRect{};
  const int y=gap_==0?below.top():gap_==count()?above.bottom()+1:(above.bottom()+1+below.top())/2;
  const auto rect=gap_<count()?below:above;
  QPainter painter(viewport());painter.setRenderHint(QPainter::Antialiasing);painter.setPen(QPen(Theme::current().accent,2,Qt::SolidLine,Qt::RoundCap));
  painter.drawLine(rect.left()+6,y,rect.right()-6,y);painter.setBrush(Theme::current().accent);painter.drawEllipse(QPoint(rect.left()+5,y),3,3);
 }
private:
 int gapAt(const QPoint& point)const{
  const auto index=indexAt(point);if(!index.isValid())return point.y()<0||!count()?0:visualItemRect(item(0)).top()>point.y()?0:count();
  const auto rect=visualRect(index);return point.y()<rect.center().y()?index.row():index.row()+1;
 }
 void setGap(int gap){if(gap_==gap)return;gap_=gap;viewport()->update();}
 int dragRow_=-1,gap_=-1;
};
// The destination name as a page title. Unfocused, a name wider than the field
// is painted elided with the full name in its tooltip; focused, it is an
// ordinary QLineEdit, with Qt's selection, caret and scrolling. Its preferred
// width follows the name while the page actions keep their trailing edge.
class DestinationTitle final:public QLineEdit {
public:
 QSize minimumSizeHint()const override{return {0,QLineEdit::minimumSizeHint().height()};}
 explicit DestinationTitle(QWidget* parent):QLineEdit(parent){setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Fixed);connect(this,&QLineEdit::textChanged,this,[this]{updateGeometry();update();});}
 bool elided()const{return !text().isEmpty()&&fontMetrics().horizontalAdvance(text())>textArea().width();}
 QSize sizeHint()const override{
  QStyleOptionFrame option;initStyleOption(&option);const auto shown=text().isEmpty()?placeholderText():text();const auto margins=textMargins();
  const QSize contents(fontMetrics().horizontalAdvance(shown)+8+margins.left()+margins.right(),QLineEdit::sizeHint().height());
  return {style()->sizeFromContents(QStyle::CT_LineEdit,&option,contents,this).width(),QLineEdit::sizeHint().height()};
 }
protected:
 void paintEvent(QPaintEvent* event)override{
  if(hasFocus()||!elided()){QLineEdit::paintEvent(event);return;}
  QStyleOptionFrame option;initStyleOption(&option);QPainter painter(this);
  style()->drawPrimitive(QStyle::PE_PanelLineEdit,&option,&painter,this);
  const auto area=textArea();painter.setFont(font());painter.setPen(palette().color(isEnabled()?QPalette::Active:QPalette::Disabled,QPalette::Text));
  painter.drawText(area,Qt::AlignLeft|Qt::AlignVCenter,fontMetrics().elidedText(text(),Qt::ElideRight,area.width()));
 }
 bool event(QEvent* event)override{
  if(event->type()==QEvent::ToolTip&&!hasFocus()&&elided()){QToolTip::showText(static_cast<QHelpEvent*>(event)->globalPos(),Ui::tooltipText(text()),this);return true;}
  return QLineEdit::event(event);
 }
private:
 // QLineEdit's own text rectangle: the style's contents, the text margins and its 2 px side margin.
 QRect textArea()const{QStyleOptionFrame option;initStyleOption(&option);return style()->subElementRect(QStyle::SE_LineEditContents,&option,this).marginsRemoved(textMargins()).adjusted(2,1,-2,-1);}
};
// What a chooser that could not open says, before what stays as it was.
QString chooserFailureText(const QString& error){return "The folder chooser couldn't open"+(error.isEmpty()?QString("."):": "+error.left(1).toLower()+error.mid(1)+(error.endsWith('.')?"":"."));}
QString capitalized(const QString& text){return text.isEmpty()?text:text.left(1).toUpper()+text.mid(1);}
QString localTime(const QString& iso){const auto time=QDateTime::fromString(iso,Qt::ISODateWithMs);return time.isValid()?QLocale().toString(time.toLocalTime(),QLocale::ShortFormat):iso;}
QString operationLine(const QJsonObject& op){
 static const QHash<QString,QString> actions{{"region","Region capture"},{"window","Window capture"},{"monitor","Monitor capture"},{"desktop","Desktop capture"},{"clipboard","Clipboard upload"},{"edit","Edited capture"},{"record","Screen recording"},{"file","File upload"}};
 const int items=op["items"].toInt();
 return actions.value(op["action"].toString(),op["action"].toString())+" · "+op["state"].toString()+" · "+localTime(op["created"].toString())+(items>1?QString(" · %1 files").arg(items):QString{});
}
// Create diagnostics package (#122): previews what diagnostics.bundle would save,
// where, and with which redaction, then saves it locally. When a working upload
// destination exists it can also upload the saved package and copy a link (#123, #158).
class DiagnosticsPackageDialog final:public QDialog {
public:
 using Reply=std::function<void(Error,QJsonObject)>;
 std::function<void(const QString&)> folderChosen; // an empty folder means the default
 DiagnosticsPackageDialog(UiSession& session,QWidget* parent):QDialog(parent),session_(session){
  setObjectName("diagnosticsPackageDialog");setWindowTitle("Create diagnostics package");setAccessibleName("Create diagnostics package");setModal(true);const auto* display=parent?parent->screen():QGuiApplication::primaryScreen();setMinimumWidth(qMin(Ui::scaled(560),display?display->availableGeometry().width():Ui::scaled(560)));
  // The body scrolls inside the screen's height; Cancel and Save stay pinned below it.
  auto* root=new QVBoxLayout(this);root->setContentsMargins(0,0,0,0);root->setSpacing(0);
  scroll_=new QScrollArea(this);scroll_->setObjectName("diagnosticsPackageScroll");scroll_->setWidgetResizable(true);scroll_->setFrameShape(QFrame::NoFrame);scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);root->addWidget(scroll_,1);
  auto* body=new QWidget;body->setObjectName("diagnosticsPackageBody");scroll_->setWidget(body);
  auto* layout=new QVBoxLayout(body);layout->setContentsMargins(space(Space::xl),space(Space::xl),space(Space::xl),space(Space::xl));layout->setSpacing(space(Space::m));
  auto* heading=new QLabel("Create diagnostics package",this);Ui::role(heading,"sectionTitle");layout->addWidget(heading);
  auto* intro=new QLabel("A .zip a maintainer can use to work out what went wrong. It is always saved on this computer; nothing is uploaded unless you choose to below.",this);intro->setWordWrap(true);Ui::role(intro,"muted");layout->addWidget(intro);
  auto* form=new QFormLayout;Ui::styleForm(form);form->setVerticalSpacing(space(Space::m));layout->addLayout(form);
  auto label=[this](const char* name){auto* l=new QLabel(this);l->setObjectName(name);l->setWordWrap(true);l->setTextFormat(Qt::PlainText);l->setTextInteractionFlags(Qt::TextSelectableByMouse);return l;};
  scope_=label("packageScope");form->addRow("Covers",scope_);
  contents_=label("packageContents");contents_->setTextFormat(Qt::RichText);
  operations_=label("packageOperations");
  auto* never=new QLabel("Credentials, tokens, signed links, file contents, screenshots and clipboard text are never included.",this);never->setWordWrap(true);Ui::role(never,"muted");form->addRow("Never",never);
  auto* names=new QWidget(this);auto* namesLayout=new QVBoxLayout(names);namesLayout->setContentsMargins(0,0,0,0);namesLayout->setSpacing(space(Space::xs));
  includeNames_=new QCheckBox("Include file names",this);includeNames_->setObjectName("includeFileNames");includeNames_->setAccessibleName("Include file names");namesLayout->addWidget(includeNames_);
  redaction_=label("packageRedaction");Ui::role(redaction_,"muted");namesLayout->addWidget(redaction_);form->addRow("Names",names);
  auto* where=new QWidget(this);auto* whereLayout=new QHBoxLayout(where);whereLayout->setContentsMargins(0,0,0,0);whereLayout->setSpacing(space(Space::s));
  folder_=label("packageFolder");Ui::role(folder_,"technical");whereLayout->addWidget(folder_,1);
  auto* choose=new QPushButton("Choose folder…",this);choose->setObjectName("choosePackageFolder");whereLayout->addWidget(choose);
  useDefault_=new QPushButton("Use default",this);useDefault_->setObjectName("defaultPackageFolder");useDefault_->setToolTip("Save to <archive root>/diagnostics, where put.gd keeps the newest 10 packages for 30 days.");whereLayout->addWidget(useDefault_);
  form->addRow("Save to",where);
  // A chooser that could not open says so beside Save to, with Try again; the folder stays.
  chooserFailure_=new QWidget(this);chooserFailure_->setObjectName("packageFolderChooserFailure");auto* failureLayout=new QHBoxLayout(chooserFailure_);failureLayout->setContentsMargins(0,0,0,0);failureLayout->setSpacing(space(Space::s));
  chooserError_=label("packageFolderChooserError");Ui::role(chooserError_,"error");failureLayout->addWidget(chooserError_,1);
  auto* retry=new QPushButton("Try again",this);retry->setObjectName("retryPackageFolderChooser");failureLayout->addWidget(retry,0,Qt::AlignTop);form->addRow(QString(),chooserFailure_);chooserFailure_->hide();
  connect(retry,&QPushButton::clicked,this,[this]{chooseFolder();});
  // Also upload and copy a link (#123): offered only once diagnostics.uploadTarget
  // says the destination works; until then, or instead, a line says why not.
  auto* extras=new QWidget(this);extras->setObjectName("diagnosticsPackageExtras");auto* extrasLayout=new QVBoxLayout(extras);extrasLayout->setContentsMargins(0,0,0,0);extrasLayout->setSpacing(space(Space::xs));layout->addWidget(extras);
  upload_=new QCheckBox("Also upload and copy a link",this);upload_->setObjectName("uploadPackage");upload_->setAccessibleName("Also upload and copy a link");upload_->hide();extrasLayout->addWidget(upload_);
  uploadNote_=label("packageUploadNote");Ui::role(uploadNote_,"muted");extrasLayout->addWidget(uploadNote_);
  auto* inventory=new QToolButton(this);inventory->setObjectName("packageContentsToggle");inventory->setText("Package contents");Ui::disclosure(inventory);layout->addWidget(inventory);layout->addWidget(contents_);layout->addWidget(operations_);contents_->hide();operations_->hide();connect(inventory,&QToolButton::toggled,contents_,&QWidget::setVisible);connect(inventory,&QToolButton::toggled,operations_,&QWidget::setVisible);connect(inventory,&QToolButton::toggled,this,[this]{fit();});
  result_=label("packageResult");Ui::role(result_,"status");result_->hide();layout->addWidget(result_);
  rule_=Ui::rule(this);root->addWidget(rule_);
  auto* buttons=footer_=new QHBoxLayout;buttons->setContentsMargins(space(Space::l),space(Space::s),space(Space::l),space(Space::s));buttons->setSpacing(space(Space::s));buttons->addStrut(Ui::scaled(56)-2*space(Space::s));show_=new QPushButton("Show in folder",this);show_->setObjectName("showPackageInFolder");show_->hide();buttons->addWidget(show_);buttons->addStretch();
  close_=new QPushButton("Cancel",this);close_->setObjectName("closePackage");buttons->addWidget(close_);
  save_=new QPushButton("Save package",this);save_->setObjectName("savePackage");Ui::role(save_,"primary");save_->setDefault(true);buttons->addWidget(save_);root->addLayout(buttons);
  connect(includeNames_,&QCheckBox::toggled,this,[this]{refreshPreview();});
  connect(upload_,&QCheckBox::toggled,this,[this]{if(saved_.isEmpty()&&!saving_)save_->setText(saveLabel());});
  chooser_=new FileDialog(this);
  connect(choose,&QPushButton::clicked,this,[this]{chooseFolder();});
  connect(useDefault_,&QPushButton::clicked,this,[this]{setFolder({},true);});
  connect(close_,&QPushButton::clicked,this,&QDialog::reject);
  connect(save_,&QPushButton::clicked,this,[this]{save({});});
  connect(show_,&QPushButton::clicked,this,[this]{reveal({});});
 }
 // Each opening starts with names hashed and without uploading, the privacy defaults.
 void setScope(const QString& operation,const QString& folder){operation_=operation;custom_=QFileInfo(folder).isDir()?folder:QString{};{const QSignalBlocker block(includeNames_);includeNames_->setChecked(false);}{const QSignalBlocker block(upload_);upload_->setChecked(false);}chooserFailure_->hide();saved_={};error_={};result_->hide();show_->hide();close_->setText("Cancel");checkUploadTarget();save_->setText(saveLabel());refreshPreview();}
 void setIncludeNames(bool include){if(includeNames_->isChecked()!=include)includeNames_->setChecked(include);}
 Error setFolder(const QString& folder,bool remember){
  if(!folder.isEmpty()&&!QFileInfo(folder).isDir())return {"diagnostics_output_unavailable","Choose an existing folder.",{{"path",folder}}};
  custom_=folder;if(remember&&folderChosen)folderChosen(folder);refreshPreview();return {};
 }
 // Choose folder…: the desktop's folder chooser (#259); false while it is open.
 bool chooseFolder(){return chooser_->open({FileDialog::Mode::Folder,"Save diagnostics packages to",folder_->text(),{},{}},[this](const FileDialog::Result& result){chooserFinished(result);});}
 // A cancel changes nothing and says nothing; a failure keeps the folder and offers Try again.
 void chooserFinished(const FileDialog::Result& result){
  chooserFailure_->setVisible(result.failed());if(result.failed())chooserError_->setText(chooserFailureText(result.error)+" The package still saves to the folder shown.");
  if(result.chosen())setFolder(result.path,true);fit();
 }
 FileDialog& folderDialog(){return *chooser_;}
 // Ticks Also upload and copy a link; only a destination that passed the check can.
 Error setUpload(bool upload){
  if(upload&&!uploadAvailable())return checkingTarget_?Error{"diagnostics_upload_checking","Still checking the upload destination; inspect until uploadTarget.checking is false.",{}}
   :Error{"diagnostics_upload_unavailable",target_["message"].toString("No working upload destination."),{{"reason",target_["reason"]}}};
  upload_->setChecked(upload);return {};
 }
 bool uploading()const{return uploadAvailable()&&upload_->isChecked();}
 // Answers once the preview that is current when it arrives has come back.
 void whenPreviewed(Reply reply){if(!previewing_)reply(error_,state());else previewReplies_.append(std::move(reply));}
 QJsonObject params(bool preview)const{
  QJsonObject p{{"includeNames",includeNames_->isChecked()}};if(!operation_.isEmpty())p["operation"]=operation_;if(!custom_.isEmpty())p["output"]=custom_;
  if(preview)p["preview"]=true;else p["notify"]=true;
  if(!preview&&uploading()){p["upload"]=true;p["copyLink"]=true;p["profileId"]=target_["profileId"].toString();}
  return p;
 }
 // Where the package would upload, and whether that destination works now. The
 // check can take minutes (a Test connection); the dialog stays usable meanwhile.
 void checkUploadTarget(){
  const auto sequence=++targetSequence_;checkingTarget_=true;target_={};
  upload_->hide();uploadNote_->setText("Checking the upload destination…");uploadNote_->show();
  session_.request("diagnostics.uploadTarget",{},this,[this,sequence](Error error,QJsonObject target){
   if(sequence!=targetSequence_)return;
   checkingTarget_=false;target_=error?QJsonObject{{"available",false},{"reason",error.code},{"message",error.message}}:target;
   if(uploadAvailable()&&saved_.isEmpty()&&!saving_){
    upload_->setText("Also upload to "+target_["label"].toString()+" and copy a link");upload_->show();
    uploadNote_->setText(target_["warning"].toString());
   } else if(!uploadAvailable()){upload_->hide();uploadNote_->setText("Upload unavailable: "+unavailableReason());}
   if(saved_.isEmpty()&&!saving_)save_->setText(saveLabel());
   fit();
  });
 }
 void refreshPreview(){
  const auto sequence=++previewSequence_;previewing_=true;save_->setEnabled(false);
  redaction_->setText(includeNames_->isChecked()?"File names, object keys, links, buckets and labels are kept as they are.":"File names, object keys, links, buckets and labels are replaced by hashes; the same name gets the same hash throughout the package.");
  session_.request("diagnostics.bundle",params(true),this,[this,sequence](Error error,QJsonObject preview){
   if(sequence!=previewSequence_)return;
   previewing_=false;error_=error;preview_=error?QJsonObject{}:preview;showPreview();
   const auto replies=std::exchange(previewReplies_,{});for(const auto& reply:replies)reply(error_,state());
  });
 }
 void save(Reply reply){
  if(saving_||previewing_||!saved_.isEmpty()){if(reply)reply({"diagnostics_busy",saving_?"The package is being saved.":previewing_?"The preview is still loading.":"This package is already saved.",{}},state());return;}
  saving_=true;save_->setEnabled(false);uploadRequested_=uploading();upload_->setEnabled(false);save_->setText(uploadRequested_?"Saving and uploading…":"Saving…");
  session_.request("diagnostics.bundle",params(false),this,[this,reply](Error error,QJsonObject saved){
   saving_=false;error_=error;upload_->setEnabled(true);
   if(error){save_->setText(saveLabel());save_->setEnabled(true);Ui::feedback(result_,error.message,true);result_->show();}
   else {
    saved_=saved;save_->setText("Saved");close_->setText("Close");show_->show();
    auto text="Saved "+QFileInfo(saved["path"].toString()).fileName()+" ("+ByteSize::summary(saved["bytes"].toInteger())+") in "+QDir::toNativeSeparators(QFileInfo(saved["path"].toString()).absolutePath())+".";
    // The local copy is kept whatever happens to the upload.
    const auto uploadError=saved["uploadError"].toObject();
    if(uploadError["code"]=="diagnostics_link_unavailable")text+="\nUploaded to "+saved["upload"].toObject()["label"].toString()+", but this destination gives it no link.";
    // The diagnostics_* messages already say the package wasn't uploaded; a provider's doesn't.
    else if(!uploadError.isEmpty())text+="\n"+(uploadError["code"].toString().startsWith("diagnostics_")?QString{}:QString("Not uploaded: "))+uploadError["message"].toString()+(uploadError["message"].toString().endsWith('.')?"":".")+" The saved package is kept.";
    else if(saved.contains("link"))text+="\nUploaded to "+saved["upload"].toObject()["label"].toString()+(saved["linkCopied"].toBool()?". Link copied: ":". Link (not copied): ")+saved["link"].toString();
    if(uploadError.isEmpty()||uploadError["code"]=="diagnostics_link_unavailable")Ui::success(result_,text);else Ui::feedback(result_,text,true);
    result_->setTextInteractionFlags(Qt::TextSelectableByMouse);result_->show();
   }
   fit();if(reply)reply(error,state());
  });
 }
 void reveal(Reply reply){
  if(saved_.isEmpty()){if(reply)reply({"diagnostics_not_saved","Save the package first.",{}},state());return;}
  session_.request("diagnostics.reveal",{{"path",saved_["path"]}},this,[this,reply](Error error,QJsonObject result){if(error){Ui::feedback(result_,error.message,true);result_->show();}if(reply)reply(error,result);});
 }
 // Grow with the wrapped body up to the screen's height; past that the body scrolls.
 void fit(){
  auto* body=scroll_->widget();body->layout()->activate();const int bodyWidth=scroll_->viewport()->width()>0?scroll_->viewport()->width():width();
  const int bodyHeight=body->layout()->hasHeightForWidth()?body->layout()->totalHeightForWidth(bodyWidth):body->layout()->totalSizeHint().height();
  body->setMinimumHeight(bodyHeight);const int footer=footer_->totalSizeHint().height()+1;
  const auto* screen=windowHandle()&&windowHandle()->screen()?windowHandle()->screen():parentWidget()&&parentWidget()->screen()?parentWidget()->screen():QGuiApplication::primaryScreen();
  // Room for the desktop's title bar and panels around the dialog.
  const int cap=screen?qMax(1,screen->availableGeometry().height()-Ui::scaled(64)):QWIDGETSIZE_MAX;
  const int wanted=qMin(cap,bodyHeight+footer);setMaximumHeight(cap);setMinimumHeight(qMin(wanted,Ui::scaled(320)));
  if(height()<wanted||height()>cap)resize(width(),wanted);
  rule_->setVisible(bodyHeight+footer>height());
 }
 QJsonObject state()const{
  QJsonObject state{{"open",isVisible()},{"includeNames",includeNames_->isChecked()},{"folder",folder_->text()},{"defaultFolder",custom_.isEmpty()},{"previewing",previewing_},{"saving",saving_},
   {"scope",scope_->text()},{"contents",contents_->text()},{"operations",operations_->text()},{"redaction",redaction_->text()},{"preview",preview_}};
  if(!operation_.isEmpty())state["operation"]=operation_;
  auto target=target_;target["checking"]=checkingTarget_;state["uploadTarget"]=target;state["upload"]=saving_?uploadRequested_:uploading();
  if(!saved_.isEmpty())state["saved"]=saved_;
  if(error_)state["error"]=error_.toJson();
  return state;
 }
private:
 void showPreview(){
  useDefault_->setVisible(!custom_.isEmpty());
  if(preview_.isEmpty()){
   scope_->setText(error_?error_.message:QString{});contents_->clear();operations_->clear();folder_->setText(custom_);fit();return;
  }
  const auto scope=preview_["scope"].toObject();const int count=preview_["operations"].toInt(),records=preview_["logRecords"].toInt();
  scope_->setText(scope.contains("operationId")?QString("This upload, with the log from ten minutes before it to ten minutes after its last record."):
   QString("Failed, partial and interrupted uploads since %1, and the log from then on.").arg(localTime(scope["from"].toString())));
  QStringList files;
  const QHash<QString,QString> about{{"manifest.json","contents and checksums"},{"environment.json","system, desktop and Qt versions"},{"diagnostics.json","put.gd's current state"},
   {"config.redacted.json","settings without credentials"},{"coredumps.json","system crash reports, stack traces only"},{"operations.json",(count==1?QString("1 upload"):QString("%1 uploads").arg(count))},{"logs/putgd.jsonl",QString("%1 log records").arg(records)+(preview_["droppedLogRecords"].toInt()?QString(", %1 older ones left out to stay under 20 MB").arg(preview_["droppedLogRecords"].toInt()):QString{})}};
  for(const auto& name:preview_["files"].toArray())files.append(QString(R"(<span style="font-family: '%1'; font-size: %2px">%3</span>: %4)").arg(Ui::monoFamily().toHtmlEscaped()).arg(Ui::scaled(Ui::Type::metadata)).arg(name.toString().toHtmlEscaped(),about.value(name.toString()).toHtmlEscaped()));
  contents_->setText(files.join("<br>"));
  QStringList lines;const auto list=preview_["operationList"].toArray();
  for(int i=0;i<list.size()&&i<5;++i)lines.append(operationLine(list[i].toObject()));
  if(list.size()>5)lines.append(QString("and %1 more").arg(list.size()-5));
  operations_->setText(lines.isEmpty()?QString("No failed uploads in this period; the log is still included."):lines.join('\n'));
  folder_->setText(QDir::toNativeSeparators(preview_["folder"].toString()));
  save_->setEnabled(saved_.isEmpty()&&!saving_);fit();
 }
 bool uploadAvailable()const{return !checkingTarget_&&target_["available"].toBool();}
 // Why the package can't be uploaded now, as the dialog says it before saving.
 QString unavailableReason()const{
  const auto reason=target_["reason"].toString();const auto label=target_["label"].toString();
  auto sentence=[](QString text){text=text.trimmed();return text.isEmpty()||text.endsWith('.')?text:text+".";};
  if(reason=="diagnostics_no_destination")return "no upload destination is set up.";
  if(reason=="diagnostics_destination_failing")return "the last upload to "+label+" failed with a credential or configuration error. Fix the destination in Settings, then try again.";
  if(reason=="diagnostics_destination_unavailable"){const auto detail=target_["details"].toObject()["error"].toObject()["message"].toString();return "Test connection to "+label+" failed"+(detail.isEmpty()?QString("."):": "+sentence(detail));}
  return sentence(target_["message"].toString("no working upload destination"));
 }
 QString saveLabel()const{return uploading()?"Save and upload":"Save package";}
 UiSession& session_;
 QLabel *scope_,*contents_,*operations_,*redaction_,*folder_,*result_;QCheckBox* includeNames_;QPushButton *useDefault_,*show_,*close_,*save_;FileDialog* chooser_=nullptr;
 QScrollArea* scroll_=nullptr;QFrame* rule_=nullptr;QHBoxLayout* footer_=nullptr;QWidget* chooserFailure_=nullptr;QLabel* chooserError_=nullptr;
 QCheckBox* upload_;QLabel* uploadNote_;QJsonObject target_;bool checkingTarget_=false,uploadRequested_=false;quint64 targetSequence_=0;
 QString operation_,custom_;QJsonObject preview_,saved_;Error error_;bool previewing_=false,saving_=false;quint64 previewSequence_=0;QList<Reply> previewReplies_;
};
// A status set directly, not from an Error: no error code behind it.
QLabel* plain(QLabel* label){label->setProperty("errorCode",{});label->setProperty("errorField",{});label->setProperty("setupProgress",{});label->setToolTip({});label->setAccessibleDescription({});return label;}
// Shows a status row only while its message or one of its actions is shown,
// so an empty row leaves no gap in the page's spacing.
// #1704: a paste, by key, middle click or the context menu's Paste, and leaving a field. Typed
// text waits for a pause; a paste saves at once, and leaving a multi-line box saves what it holds.
class TypingWatch final:public QObject {
public:
 TypingWatch(QWidget* field,std::function<void(QWidget*)> pasted,std::function<void(QWidget*)> left):QObject(field),field_(field),pasted_(std::move(pasted)),left_(std::move(left)){
  field->installEventFilter(this);if(auto* area=qobject_cast<QAbstractScrollArea*>(field))area->viewport()->installEventFilter(this);}
protected:
 bool eventFilter(QObject*,QEvent* event)override{
  switch(event->type()){
  case QEvent::KeyPress:if(static_cast<QKeyEvent*>(event)->matches(QKeySequence::Paste))pasted_(field_);break;
  case QEvent::MouseButtonRelease:if(static_cast<QMouseEvent*>(event)->button()==Qt::MiddleButton)pasted_(field_);break;
  case QEvent::FocusOut:left_(field_);break;
  case QEvent::ContextMenu:{
   QMenu* menu=nullptr;if(auto* line=qobject_cast<QLineEdit*>(field_))menu=line->createStandardContextMenu();else if(auto* text=qobject_cast<QPlainTextEdit*>(field_))menu=text->createStandardContextMenu();
   if(!menu)break;
   // Qt 6.4's line edit names no action, so Paste is also known by its shortcut or its label. The
   // field's own handler would paste before any later one runs, so this one marks it, then pastes.
   for(auto* action:menu->actions()){
    // Qt 6.4 puts the shortcut in the label ("&Paste\tCtrl+V") and sets none on the action.
    const auto label=action->text().section('\t',0,0);const bool paste=action->objectName()=="edit-paste"||action->shortcut().matches(QKeySequence(QKeySequence::Paste))==QKeySequence::ExactMatch||label==QCoreApplication::translate("QLineEdit","&Paste")||label==QCoreApplication::translate("QWidgetTextControl","&Paste");
    if(!paste)continue;
    QObject::disconnect(action,&QAction::triggered,nullptr,nullptr);
    QObject::connect(action,&QAction::triggered,this,[this]{pasted_(field_);if(auto* line=qobject_cast<QLineEdit*>(field_))line->paste();else if(auto* text=qobject_cast<QPlainTextEdit*>(field_))text->paste();});
   }
   menu->exec(static_cast<QContextMenuEvent*>(event)->globalPos());delete menu;return true;}
  default:break;
  }
  return false;
 }
private:
 QWidget* field_;std::function<void(QWidget*)> pasted_,left_;
};
class VisibleWithParts final:public QObject {
public:
 VisibleWithParts(QWidget* row,QList<QWidget*> parts):QObject(row),row_(row),parts_(std::move(parts)){for(auto* part:std::as_const(parts_))part->installEventFilter(this);update();}
protected:
 bool eventFilter(QObject* watched,QEvent* event)override{if(event->type()==QEvent::ShowToParent||event->type()==QEvent::HideToParent)update();return QObject::eventFilter(watched,event);}
private:
 void update(){bool shown=false;for(auto* part:std::as_const(parts_))shown|=part->isVisibleTo(row_);row_->setVisible(shown);}
 QWidget* row_;QList<QWidget*> parts_;
};
QString settingHelp(const QString& path,const QString& objectName){
 static const QHash<QString,QString> s3Help{
  {"endpoint","The HTTPS address of the S3 service, without the bucket name or object prefix. It starts with the standard AWS endpoint for the selected region. Use your service's endpoint for compatible storage. A custom value is preserved when you change the region."},
  {"storageClass","The S3 storage class requested for new uploads. STANDARD is the default. Other classes can have different access, retrieval and storage costs; use a value supported by your destination. This setting doesn't change the class of existing objects."},
  {"publicUrlBase","An optional HTTPS base URL for copied direct links, such as your own CDN or custom domain. The object path is appended to this URL. Leave it empty to use the destination's direct URL. A custom domain doesn't make private objects public or configure DNS or a bucket policy."},
  {"connectTimeoutMs","The connection timeout shown in seconds, with millisecond precision. The default is 5 seconds. It limits how long a storage request waits to establish a connection; it is separate from the overall request timeout below."},
  {"requestTimeoutMs","The request timeout shown in seconds, with millisecond precision. The default is 60 seconds. It controls the storage client's request timeout. Allow enough time for your connection and file sizes; a timed-out upload may still need reconciliation before its remote result is known."},
  {"lowSpeedBytesPerSecond","The minimum sustained transfer rate in whole KB/s (1 KB = 1024 bytes). The default, Any progress, treats any data still moving as progress. A transfer below this rate for the low-speed window is considered stalled. This is a stall threshold, not an upload speed limit or a file-size limit."},
  {"lowSpeedTimeSeconds","The number of seconds a transfer may remain below the minimum transfer rate before it is considered stalled. The default is 30 seconds. It works with Minimum transfer and doesn't cap an upload's total duration."}};
 static const QHash<QString,QString> numberHelp{
  {"pendingImageBytes","Maximum total decoded image content retained by jobs waiting for or undergoing encoding. The default is 64 MB. An acquisition that can't fit fails with image_memory_limit; it isn't silently deferred or changed. The limit is released after disk persistence or queued cancellation. This bounds source payloads, not total app RSS, compositor frames, encoding workspace or the current native clipboard representation."},
  {"maxFiles","The maximum number of files accepted from one clipboard action. The default is 100. A batch above this limit is rejected before uploading its files. This is a per-action limit and doesn't restrict the total number of history entries."},
  {"pendingOperations","The maximum number of queued operations waiting for processing, in addition to those currently running. The default is 8. When the queue is full, a new action is rejected rather than accumulating more pending work."},
  {"concurrentUploads","How many files upload at the same time, across every capture and batch. The default is 3. A new capture starts while a large or slow upload continues, and a large file uses one upload however long it takes. Each S3 upload runs in its own small worker process (about 40 MB); Proton Drive uploads one file at a time per account whatever this is set to. Higher values help many small files more than one large one."},
  {"maxFileBytes","The largest individual uploaded file, in GB. Auto, the default, sets no size limit of put.gd's own: uploads stream from disk, so memory doesn't grow with file size. Whatever this is set to, a file larger than the destination accepts (5120 GB for S3) or than the free disk space allows is rejected before uploading; put.gd always keeps 5% of the disk, up to 4 GB and at least 1 GB, free for the system. No limit (an empty field) also sets none. Fractional GB values are accepted when they can be represented exactly in stored units."},
  {"maxBatchBytes","The maximum combined size of one clipboard file batch, in GB. Auto, the default, and No limit (an empty field) set none of put.gd's own; free disk space still bounds every batch. This applies to the whole batch in addition to the individual file-size limit."},
  {"pendingArtifactBytes","The maximum acquired content waiting in the pending artifact store on disk, in GB. Auto, the default, and No limit (an empty field) set none of put.gd's own: free disk space is the bound, keeping 5% of the disk (up to 4 GB, at least 1 GB) free for the system. New uploads are rejected when they would exceed this limit. This is a disk-content ceiling, not memory allocated at startup."},
  {"clipboardTextBytes","The largest text payload accepted from the clipboard, in MB. The default is 16 MB. Larger text is rejected before uploading. One MB equals 1,048,576 bytes; this limit doesn't apply to ordinary file uploads."},
  {"imagePixels","The largest image, shown in megapixels (one million pixels), calculated as width × height. The default is 100 megapixels. This protects against oversized decoded images independently of their encoded file size."},
  {"imageBytes","The maximum accepted image payload size, in MB. The default is 512 MB. It works alongside the image-pixel limit; an image must satisfy both limits. One MB equals 1,048,576 bytes. This is a ceiling, not a persistent allocation."},
  {"failedArtifactDays","How many days failed or interrupted local artifacts are kept for explicit retry before cleanup may remove them. The default is 7 days. Set 0 to disable age-based removal of failed artifacts. This controls local retry content and doesn't delete remote objects."},
  {"recentActivityLimit","The maximum number of recent operation snapshots available to History. The default is 100. This bounds the activity list; it doesn't set the complete upload-history retention period below."},
  {"previewCacheBytes","The maximum total size of generated image previews stored on disk, in MB. The default is 256 MB. Set 0 to disable stored previews. This is a disk-cache limit, not a memory allowance; previews may be regenerated or removed by cleanup."},
  {"historyRetentionDays","How many days completed history metadata is retained before automatic pruning. Choose Forever to keep history indefinitely, or enter a number of days. Removing old history metadata doesn't delete remote uploads or your saved local files."}};
 static const QHash<QString,QString> descriptions{
  {"unlockCredentialsAtStartup","Reads saved destination credentials from the desktop keyring once when put.gd starts, so uploads work without pressing Unlock. If the keyring is locked, the desktop shows its own unlock prompt then. Uploads and hotkeys never unlock the keyring."},
  {"checkForUpdates","Once a day put.gd asks app.put.gd for the newest version on its channel. The request is the same from every computer: no version, account, locale or ID. The comparison happens here, and nothing is installed without you. Stable builds also show a notification once for each new version; nightly builds show it only here. Check now works with daily checks off."},
  {"updateChannel","Starts on the channel of this build: a nightly build follows nightly, a release or release candidate follows stable. Choose the other one to watch it instead. Changing it doesn't change what is installed: an AppImage then offers to switch to that channel's newest release, which you start yourself. A DEB or RPM that updates from put.gd's apt or dnf repository follows that repository's channel instead, which only you change, in a terminal."},
  {"updateStatus","The result of the last check and when the next one runs. Download opens the new package for this installation in your browser. When this installation updates from put.gd's apt or dnf repository, Open software updates opens your system's updater instead."},
  {"updateInstall","Download and install fetches the new AppImage, checks it against the release's SHA256SUMS and puts it in place of the one put.gd runs from, at the same path, so login startup and menu entries keep working. It uses appimageupdatetool or zsync2 when one is installed, which downloads only what changed, and otherwise the whole file. The current version is kept beside it as a hidden backup until the new one has started. When you watch the other channel, the same steps switch to that channel's release, checked with its own signing key; an older version is installed only after you confirm."},
  {"otherChannel","This installation updates from put.gd's apt or dnf repository, so your system decides which channel it installs. These commands put the package's copy of the other channel's repository file in place, then install that channel's package. put.gd never runs them for you."},
  {"debugLogging","Adds detailed debug records to put.gd's log on this computer, for a maintainer working out a problem. It turns itself off 24 hours after you turn it on; restarting doesn't extend it. Credentials, tokens and file contents are never logged."},
  {"logSummary","put.gd keeps a log of what it did in its state folder, at most 14 days and 50 MB by default (Advanced settings in the configuration file). Clear logs deletes every log file and starts a new one; uploads, history, settings and saved diagnostics packages stay."},
  {"uncleanExit","When put.gd starts and finds it didn't shut down cleanly last time, it records why: a signal such as a logout, or an unknown cause such as a crash, a forced kill or a power loss. The newest one is shown here and included in diagnostics packages."},
  {"fileManagerMenu","Adds Upload with put.gd to the right-click menu of the file managers on this computer: Files (Scripts), Dolphin, Nemo, Thunar (Send To), Caja (Scripts), PCManFM, Pantheon Files and COSMIC Files. Turning it off removes only the entries this AppImage wrote."},
  {"fileManagerFolders","How a folder you choose Upload with put.gd for is uploaded. Upload as a .zip makes one <folder name>.zip with everything inside it, and copies one link. Upload each file uploads every file in it and its subfolders, one link each; symbolic links are skipped, and the file limit under Advanced (100 by default, up to 10,000) counts every file, so a large folder needs a .zip or a higher limit."},
  {"regionSelectionMode","Live desktop keeps the screen updating while you drag; close an open menu first. Screenshot first captures every monitor before the selector opens and cuts your region from that picture, so open menus and tooltips are included. Applies to new captures."},
  {"captureDelaySeconds","Waits this long before Capture region, Repeat last region, Capture window, Capture monitor, Capture desktop, Capture region & edit and Copy text from region, when you start one from the tray or this window. Capture scrolling region and Record screen never wait. A pill at the top of the screen counts down; click it to cancel. A region is chosen first and captured when the countdown ends, so there is time to open a menu or hover a button. Capture delay in the tray and in this window's status bar sets the same choice."},
  {"captureDelayShortcuts","Makes captures you start with a hotkey wait for the capture delay too. Off, a hotkey always captures at once, whatever the delay is set to."},
  {"recordingMicrophoneDevice","The input the recording bar's Microphone toggle opens. System default follows the sound settings; an output's monitor is never used as a microphone. The microphone is opened only while the toggle is on and released when the recording stops."},
  {"recordingClickHighlights","Draws a ring at the pointer on each click in new recordings, so a viewer can see what was pressed. Where the desktop reports clicks, the ring is drawn into the clip only; on Plasma, KWin's own click effect is switched on for the length of the recording and also shows on the screen."},
  {"recordingFrameRate","Frames per second for new recordings. 60 looks smoother and takes about twice the work to convert and encode. When the computer can't keep up during a recording, put.gd lowers the rate for the rest of it (60 to 30, then 15) so the desktop you're recording stays responsive; the clip keeps its full length. Applies to new recordings."},
  {"uploadClipboardBehavior","Copy each link writes successful upload links individually. Copy a list writes one newline-separated list in submission order after overlapping uploads finish. Copying something yourself pauses automatic copies; links stay available in History. Applies to new operations."},
  {"destination","Choose a destination for this action or turn uploads off. Local copies are set separately, under Local copies. Applies to new captures, without changing accepted jobs."},
  {"profileId","Choose a saved destination for uploads, or No destination to turn uploads off. Local copies are set separately, under Local copies. With both outputs off, capture actions are unavailable. Changing this doesn't move earlier uploads or alter accepted jobs."},
  {"publicationMode","Use the destination default, create a share link, or keep uploads private. Whether a share link is copied, and how, follows After uploading in Settings → Capture. Private mode creates no share link and leaves the clipboard unchanged; existing member or folder permissions still apply. Unsupported modes are rejected before uploading."},
  {"mode","Keep original names preserves the names of copied files. Random names uses a UUID or a string of random characters, plus the extension. Custom template renames copied and uploaded files too. Generated captures use their templates unless Random names is selected. Names remain fixed on retry."},
  {"randomStyle","A UUID is 36 characters and practically never repeats. Random characters makes a shorter name of the length and characters you choose; lowercase letters are always used. Shorter names are easier to share but repeat sooner, and a repeat follows When a name exists."},
  {"randomLength","Characters before the extension, 4 to 64. Twelve lowercase letters, capitals and numbers give more than 10^21 names."},
  {"collisionPolicy","For remote duplicates, add a unique suffix (default; a random-characters name gets a new random name instead), skip the file, or replace the checked object where safely supported. Replacement may change content served by an existing link. Local files always preserve existing files and add a suffix."},
  {"label","Display name used in destination selectors. Renaming it doesn't rename a bucket, change credentials or move uploaded files."},
  {"aclMode","Use bucket policy sends no ACL with each upload (the default); the bucket's policy decides who can open files. Request public read asks S3 to make each new object publicly readable through an object ACL, which needs ACLs enabled on the bucket and the s3:PutObjectAcl permission. Neither choice changes the bucket policy, and the policy or Block Public Access can still refuse access, so test the resulting link."},
  {"addressing","Automatic lets the client select addressing (default). Virtual puts the bucket in the hostname; path puts it in the URL path. Some S3-compatible endpoints require path addressing."},
  {"reference","Identifier for the credentials in the desktop secret service, not an access key. Changing it selects a different stored entry. Session mode stores no reference. Unlock saved credentials to inspect an existing entry."},
  {"protonExpiration","Share-link expiration in days. Zero means no expiration. Applies to newly created links; it doesn't update existing shares or delete stored files."},
  {"protonRequestTimeout","Overall Proton request deadline in seconds, with millisecond precision. Default 120 seconds. Allow time for large transfers. A timeout may require inspecting the remote result before retrying."},
  {"protonSharePassword","Password for future Proton share links. Store link password explicitly; the configuration stores only its secret-store reference. Use no link password clears that reference for future links. Existing links are unchanged."},
  {"linkMode","Public links never expire and require public read access. Private bucket gives S3 uploads a signed GET link and Backblaze B2 uploads a download authorization link, lasting 1–7 days. This setting does not change bucket policy. S3 uses its endpoint host; temporary credentials may expire sooner."},
  {"customEndpoint","Show and edit the S3 service endpoint. An unchanged AWS endpoint follows the selected region; custom endpoints are preserved. Unchecking hides this editor without changing an existing custom endpoint."}};
 const auto key=path.section('/',-1);


 if(objectName.startsWith("proton"))return descriptions.value(objectName);
 if(s3Help.contains(key))return s3Help.value(key);
 if(numberHelp.contains(key))return numberHelp.value(key);
 if(path.startsWith("/naming/")&&key.endsWith("Template"))return "Names "+QString(objectName=="fileTemplate"?"copied and uploaded files when Custom template is selected":objectName=="captureTemplate"?"screen captures":objectName=="imageTemplate"?"clipboard images":"clipboard text")+". Tokens: {name} is the source stem, {ext} its extension without a dot, {yyyy}, {MM}, {dd}, {HH}, {mm}, {ss} use local action time, and {uuid} is unique per item. Include a dot before {ext}; paths and unknown tokens are rejected. Existing history and accepted jobs are unchanged.";
 if(objectName=="credentialMode")return descriptions.value(objectName);
 return descriptions.value(key,descriptions.value(objectName));
}
QStringList s3Regions(){
 // Offline suggestions from the AWS S3 regional endpoint table, checked 2026-10-08.
 return {"us-east-1","us-east-2","us-west-1","us-west-2","af-south-1","ap-east-1","ap-east-2","ap-south-1","ap-south-2","ap-southeast-1","ap-southeast-2","ap-southeast-3","ap-southeast-4","ap-southeast-5","ap-southeast-6","ap-southeast-7","ap-northeast-1","ap-northeast-2","ap-northeast-3","ca-central-1","ca-west-1","eu-central-1","eu-central-2","eu-west-1","eu-west-2","eu-west-3","eu-south-1","eu-south-2","eu-north-1","il-central-1","mx-central-1","me-south-1","me-central-1","sa-east-1","us-gov-east-1","us-gov-west-1"};
}
}
namespace PreferenceRows {
// The painter uses fractional advances; reserve the rounding pixel before wrapping.
QStringList destinationStateLines(const QString& state,int width){QStringList escaped;for(const auto& fact:state.split(" · ",Qt::SkipEmptyParts))escaped.append(fact.toHtmlEscaped());QTextDocument text;text.setHtml(Ui::separatedLines(escaped,Ui::font(Ui::Type::metadata),qMax(0,width-1)));return text.toPlainText().split('\n');}
QLabel* caption(const QString& text){auto* label=Ui::label(text,"secondary");label->setProperty("uiStatus",true);label->setWordWrap(true);return label;}
QFormLayout* card(QVBoxLayout* page,const QString& heading,const QString& description,bool headingInside){
 auto* card=new Ui::Surface("surface");card->setProperty("preferenceCard",true);auto* box=new QVBoxLayout(card);box->setSizeConstraint(QLayout::SetMinimumSize);box->setContentsMargins(0,0,0,0);box->setSpacing(0);auto* section=new FormRow;section->setProperty("preferenceSection",true);section->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Minimum);auto* column=new QVBoxLayout(section);column->setSizeConstraint(QLayout::SetMinimumSize);column->setContentsMargins(0,0,0,0);column->setSpacing(space(Space::s));addPreferenceWidget(column,Ui::label(heading,"sectionTitle"));if(!description.isEmpty())column->addWidget(caption(description));
 if(headingInside){column->setContentsMargins(space(Space::l),space(Space::l),space(Space::l),space(Space::s));box->addWidget(section);addPreferenceWidget(page,card);}else{addPreferenceWidget(column,card);page->addWidget(section);card->setProperty("preferenceSectionContainer",QVariant::fromValue(static_cast<QObject*>(section)));}
 auto* form=new QFormLayout;Ui::styleForm(form);form->setContentsMargins(0,0,0,0);box->addLayout(form);return form;
}
QWidget* sectionFor(QFormLayout* form){auto* card=form->parentWidget();auto* section=card->property("preferenceSectionContainer").value<QObject*>();return section?static_cast<QWidget*>(section):card;}
QWidget* rowFor(QWidget* field){auto* row=field->property("preferenceRowContainer").value<QObject*>();return row?static_cast<QWidget*>(row):field;}
QWidget* labelFor(QFormLayout* form,QWidget* row){if(auto* label=form->labelForField(row))return label;return qobject_cast<QWidget*>(row->property("preferenceLabel").value<QObject*>());}

QWidget* action(QFormLayout* form,const QString& heading,const QString& hint,QWidget* control){
 auto* row=new FormRow;auto* column=new QVBoxLayout(row);column->setContentsMargins(0,0,0,0);auto* line=new QHBoxLayout;line->setSpacing(space(Space::l));auto* captions=new QVBoxLayout;captions->setSpacing(space(Space::xs));captions->setAlignment(Qt::AlignVCenter);auto* label=Ui::label(heading,"body");label->setWordWrap(true);captions->addWidget(label);if(!hint.isEmpty())captions->addWidget(caption(hint));line->addLayout(captions,1);line->addWidget(control,0,Qt::AlignRight|Qt::AlignVCenter);column->addLayout(line);row->setProperty("preferenceLabel",QVariant::fromValue(static_cast<QObject*>(label)));form->addRow(row);return row;
}
QWidget* disclosure(QFormLayout* form,const QString& heading,QLabel* summary,QToolButton* toggle,bool inlineSummary){
 auto* row=action(form,heading,{},toggle);auto* line=qobject_cast<QHBoxLayout*>(row->layout()->itemAt(0)->layout());if(inlineSummary){summary->setAlignment(Qt::AlignRight|Qt::AlignVCenter);line->insertWidget(1,summary,0,Qt::AlignRight|Qt::AlignVCenter);}else{auto* captions=qobject_cast<QVBoxLayout*>(line->itemAt(0)->layout());captions->addWidget(summary);}toggle->setText({});toggle->setProperty("uiFlush",true);toggle->setFixedSize(Ui::scaled(14),Ui::scaled(Ui::Metric::control));
 class Click final:public QObject{public:Click(QWidget* row,QToolButton* toggle):QObject(row),toggle_(toggle){row->installEventFilter(this);for(auto* label:row->findChildren<QLabel*>())label->installEventFilter(this);}protected:bool eventFilter(QObject*,QEvent* event)override{if(event->type()==QEvent::MouseButtonRelease&&static_cast<QMouseEvent*>(event)->button()==Qt::LeftButton&&toggle_->isEnabled()){toggle_->click();return true;}return false;}private:QToolButton* toggle_;};new Click(row,toggle);return row;
}
QWidget* radio(QRadioButton* radio,const QString& explanation,const QString& detail){auto* row=new FormRow;row->setProperty("inlineHelpRow",true);auto* column=new QVBoxLayout(row);column->setContentsMargins(0,0,0,0);column->setSpacing(space(Space::s));auto* box=new QHBoxLayout;box->setSpacing(space(Space::m));auto* text=new QVBoxLayout;text->setSpacing(space(Space::xs));text->setAlignment(Qt::AlignTop);auto* heading=new QHBoxLayout;heading->setSpacing(space(Space::xs));auto* title=Ui::label(radio->text(),"body");title->setBuddy(radio);title->setWordWrap(true);heading->addWidget(title,0,Qt::AlignTop);auto* help=detail.isEmpty()?nullptr:Ui::helpButton(radio->text(),detail,radio,row);if(help){help->setFixedHeight(Ui::scaled(Ui::Type::bodyLine));help->setProperty("uiIconRaise",1);heading->addWidget(help,0,Qt::AlignTop);}radio->setAccessibleDescription(explanation+(detail.isEmpty()?QString{}:" "+detail));heading->addStretch(1);if(help)text->addLayout(heading);else{heading->removeWidget(title);delete heading;text->addWidget(title,0,Qt::AlignTop);}text->addWidget(new PreferenceCaption(explanation));box->addLayout(text,1);box->insertWidget(0,radio,0,Qt::AlignTop);radio->setContentsMargins(0,qMax(0,(QFontMetrics(Ui::font(Ui::Type::body)).height()-Ui::scaled(18))/2),0,0);column->addLayout(box);radio->setFixedWidth(Ui::scaled(24));radio->setAccessibleName(radio->text());if(help)QObject::connect(help,&QToolButton::toggled,row,[row,radio,column,text]{if(auto* note=row->findChild<QLabel*>(radio->objectName()+"HelpText")){column->removeWidget(note);text->addWidget(note);note->setProperty("uiStatus",true);Ui::role(note,"secondary");}});
 class Click final:public QObject{public:Click(QWidget* row,QRadioButton* radio,QToolButton* help):QObject(row),row_(row),radio_(radio),help_(help){row->installEventFilter(this);for(auto* label:row->findChildren<QLabel*>())label->installEventFilter(this);}protected:bool eventFilter(QObject* watched,QEvent* event)override{if(event->type()!=QEvent::MouseButtonPress&&event->type()!=QEvent::MouseButtonRelease)return false;auto* mouse=static_cast<QMouseEvent*>(event);if(mouse->button()!=Qt::LeftButton||!radio_->isEnabled())return false;auto* target=qobject_cast<QWidget*>(watched);const auto at=target->mapTo(row_,mouse->position().toPoint());if(help_&&help_->geometry().contains(at))return false;if(event->type()==QEvent::MouseButtonPress){pressed_=true;return true;}if(!std::exchange(pressed_,false))return false;if(target->rect().contains(mouse->position().toPoint())){radio_->setFocus(Qt::MouseFocusReason);radio_->click();}return true;}private:QWidget* row_;QRadioButton* radio_;QToolButton* help_;bool pressed_=false;};new Click(row,radio,help);return row;}
void box(QFormLayout* form){boxPreferenceRows(form);}
void alignChoices(QWidget* family,bool reserveHelp){
 class Family final:public QObject {
 public:
  explicit Family(QWidget* root,bool reserveHelp):QObject(root),root_(root),reserveHelp_(reserveHelp){if(reserveHelp_)root_->installEventFilter(this);for(auto* combo:root->findChildren<QComboBox*>())if(combo->property("preferenceChoice").toBool()){combo->installEventFilter(this);connect(combo->model(),&QAbstractItemModel::dataChanged,this,[this]{schedule();});connect(combo->model(),&QAbstractItemModel::rowsInserted,this,[this]{schedule();});connect(combo->model(),&QAbstractItemModel::rowsRemoved,this,[this]{schedule();});connect(combo->model(),&QAbstractItemModel::modelReset,this,[this]{schedule();});}for(auto* field:root->findChildren<QWidget*>())if(field->property("preferenceChoiceValue").toBool()||field->property("preferenceInlineValue").toBool()){auto* row=field->parentWidget();row->installEventFilter(this);if(auto* caption=row->property("preferenceLabel").value<QObject*>())caption->installEventFilter(this);if(field->property("preferenceShortcutValue").toBool())for(auto* edit:field->findChildren<QLineEdit*>())connect(edit,&QLineEdit::textChanged,this,[this]{schedule();});}
   for(auto* widget:root->findChildren<QWidget*>())if(auto* editor=dynamic_cast<ShortcutEditor*>(widget))editor->setPromptChanged([guard=QPointer<Family>(this)]{if(guard)guard->schedule();});
   schedule();}
 protected:
  // Model/text/prompt changes plus font, visibility and size events cover the sizing inputs.
  // LayoutRequest is also produced by apply itself, so observing it feeds the next pass forever.
  bool eventFilter(QObject*,QEvent* e)override{if(e->type()==QEvent::FontChange||e->type()==QEvent::Show||e->type()==QEvent::Resize)schedule();return false;}
 private:
  void schedule(){if(pending_)return;pending_=true;QTimer::singleShot(0,this,[this]{pending_=false;int captionWidth=0,shortcutWidth=0,helpWidth=0;
   const auto values=root_->findChildren<QWidget*>();for(auto* row:values)if(auto* caption=qobject_cast<QWidget*>(row->property("preferenceLabel").value<QObject*>());caption&&row->property("preferenceRow").toBool()&&(!reserveHelp_||row->property("preferenceCaption").isValid())){if(reserveHelp_){const QFontMetrics metrics(Ui::font());if(row->property("preferenceCaption").isValid())captionWidth=qMax(captionWidth,metrics.horizontalAdvance(row->property("preferenceCaption").toString()));else if(auto* text=qobject_cast<QLabel*>(caption))captionWidth=qMax(captionWidth,metrics.horizontalAdvance(text->text()));else for(auto* text:caption->findChildren<QLabel*>())captionWidth=qMax(captionWidth,metrics.horizontalAdvance(text->text()));}else{captionWidth=qMax(captionWidth,caption->sizeHint().width());if(auto* text=qobject_cast<QLabel*>(caption))captionWidth=qMax(captionWidth,text->fontMetrics().horizontalAdvance(text->text()));}for(auto* help:caption->findChildren<QToolButton*>())helpWidth=qMax(helpWidth,help->width()+space(Space::xs));}
   for(auto* value:values)if(value->property("preferenceShortcutValue").toBool())for(auto* edit:value->findChildren<QLineEdit*>()){auto* keycap=edit->parentWidget()->layout();const auto margins=keycap->contentsMargins();shortcutWidth=qMax(shortcutWidth,edit->fontMetrics().horizontalAdvance(edit->text().isEmpty()?edit->placeholderText():edit->text())+margins.left()+margins.right()+keycap->spacing()+Ui::scaled(24)+space(Space::xs));}
   // A card stays inline when every row fits: free text needs scaled(280), a fixed choice only its widest option.
   const int gutter=qMin(220,qMax(0,captionWidth-(reserveHelp_?0:helpWidth)))+helpWidth+space(Space::l);QHash<QWidget*,bool> cardStacked;const auto cardFor=[](QWidget* value){for(auto* owner=value->parentWidget();owner;owner=owner->parentWidget())if(owner->property("preferenceCard").toBool())return owner;return static_cast<QWidget*>(nullptr);};for(auto* value:values)if(value->property("preferenceInlineValue").toBool()||value->property("preferenceChoiceValue").toBool()){auto* field=value->parentWidget();auto* card=cardFor(value);const auto margins=field->layout()->contentsMargins();const int room=field->width()-margins.left()-margins.right();const auto combos=value->findChildren<QComboBox*>();const bool fixedChoice=value->property("preferenceChoiceValue").toBool()&&!combos.isEmpty()&&std::none_of(combos.cbegin(),combos.cend(),[](auto* combo){return combo->isEditable();})&&value->findChildren<QLineEdit*>().isEmpty();const int need=qMax(value->minimumSizeHint().width(),fixedChoice?qMin(Ui::scaled(280),value->sizeHint().width()):Ui::scaled(280));if(value->isVisibleTo(root_))cardStacked[card]|=room<gutter+need;}
   for(auto* value:values)if(value->property("preferenceInlineValue").toBool()||value->property("preferenceChoiceValue").toBool()){const bool compact=value->property("preferenceShortcutValue").toBool();const int measured=compact?shortcutWidth:Ui::scaled(280);auto* row=static_cast<PreferenceChoiceRow*>(value->property("preferenceChoiceSizer").value<QObject*>());const bool created=!row;if(!row){auto* field=value->parentWidget();auto* line=qobject_cast<QBoxLayout*>(field->layout()->itemAt(0)->layout());if(!line)continue;row=new PreferenceChoiceRow(value,field,line,measured,gutter,compact);value->setProperty("preferenceChoiceSizer",QVariant::fromValue(static_cast<QObject*>(row)));}row->setWidths(measured,gutter,cardStacked.value(cardFor(value)));if(reserveHelp_&&created)value->parentWidget()->layout()->activate();}});}
  QWidget* root_;bool reserveHelp_;bool pending_=false;
 };
 new Family(family,reserveHelp);
}
}
SettingsWindow::SettingsWindow(UiSession& settings,QWidget* parent):RetainedWindow(parent),settings_(settings) {
 setObjectName("settingsPanel");Ui::apply(*qApp);
 // Since 2026-10-26 a sidebar names the four categories; the pages
 // stay one QTabWidget (its tab bar hidden) so state, agents and tests keep
 // the same section indexes.
 auto* outer=new QHBoxLayout(this);outer->setContentsMargins(0,0,0,0);outer->setSpacing(0);
 auto* rail=new Ui::Surface("surface",Qt::RightEdge,this);rail->setObjectName("settingsRail");settingsRail_=rail;auto* railBox=new QVBoxLayout(rail);railBox->setContentsMargins(space(Space::l),space(Space::l),space(Space::l),space(Space::l));railBox->setSpacing(space(Space::m));
 // No title: the header navigation already names the page.
 
 auto* navigation=new QListWidget(rail);settingsNavigation_=navigation;navigation->setObjectName("settingsNavigation");navigation->setAccessibleName("Settings categories");navigation->setFrameShape(QFrame::NoFrame);navigation->setItemDelegate(new Ui::NavigationDelegate(navigation));navigation->setMouseTracking(true);navigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);navigation->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
 for(const auto& [glyph,label]:QList<QPair<QString,QString>>{{"preferences","General"},{"region","Capture"},{"shortcuts","Hotkeys"},{"settings","Advanced"},{"alert","Error log"}})new QListWidgetItem(Ui::icon(glyph),label,navigation);
 navigation->setFixedHeight(5*Ui::scaled(Ui::Metric::navRow)+4);Ui::transparentView(navigation);railBox->addWidget(navigation);railBox->addStretch();outer->addWidget(rail);
 auto* content=new QWidget(this);outer->addWidget(content,1);auto* layout=new QVBoxLayout(content);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);
 tabs_=new QTabWidget(this);tabs_->setObjectName("settingsTabs");tabs_->setAccessibleName("Settings categories");tabs_->tabBar()->installEventFilter(this);tabs_->tabBar()->hide();tabs_->setDocumentMode(true);layout->addWidget(tabs_,1);
 connect(navigation,&QListWidget::currentRowChanged,tabs_,[this](int row){if(row>=0)tabs_->setCurrentIndex(row);});connect(tabs_,&QTabWidget::currentChanged,this,[this]{clearCompletedFeedback();});connect(tabs_,&QTabWidget::currentChanged,navigation,[navigation](int index){const QSignalBlocker block(navigation);navigation->setCurrentRow(index);});
 const QHash<QString,QString> pageIntros{{"General","Startup, notifications, appearance and updates."},{"Capture","Output, sharing, region selection, local copies and file names."},{"Shortcuts","Keys that start captures and uploads from anywhere, even while put.gd is in the background."},{"Advanced","Limits, history retention, diagnostics and configuration."}};
 // Pages keep their original keys in object names (settingsPageHeadingShortcuts)
 // and in the hidden tab bar's text; only the visible heading and rail name change.
 const QHash<QString,QString> pageNames{{"Shortcuts","Hotkeys"}};
 QHash<QString,QHBoxLayout*> pageTitles;
 auto scrollPage=[&](const QString& title){auto* body=new Ui::Surface("preferencePage");auto* box=new QVBoxLayout(body);box->setContentsMargins(space(Space::xxl),qMin(Ui::pageHeaderTop(),space(Space::l)),space(Space::xxl),space(Space::xl));box->setSpacing(space(Space::xxl));box->setAlignment(Qt::AlignTop);auto* header=new QWidget(body);header->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Maximum);header->setMaximumWidth(Ui::scaled(760));auto* headerBox=new QVBoxLayout(header);headerBox->setContentsMargins(0,0,0,0);headerBox->setSpacing(space(Space::s));auto* heading=new QLabel(pageNames.value(title,title),header);Ui::role(heading,"title");heading->setMinimumHeight(Ui::scaled(Ui::Metric::control));heading->setAlignment(Qt::AlignLeft|Qt::AlignVCenter);auto* titleRow=pageTitles[title]=new QHBoxLayout;titleRow->setContentsMargins(0,0,0,0);titleRow->setSpacing(space(Space::l));titleRow->addWidget(heading,1,Qt::AlignVCenter);headerBox->addLayout(titleRow);auto* intro=new QLabel(pageIntros.value(title),header);intro->setWordWrap(true);Ui::role(intro,"muted");headerBox->addWidget(intro);addPreferenceWidget(box,header);auto* scroll=new QScrollArea;scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);scroll->setWidget(body);heading->setObjectName("settingsPageHeading"+title);intro->setObjectName("settingsPageDescription"+title);new CompactPageHeading(scroll,heading,intro);Ui::responsiveColumns(titleRow,scroll->viewport(),400);tabs_->addTab(scroll,title);return box;};
 auto* general=scrollPage("General");auto* capture=scrollPage("Capture");auto* shortcutsPage=scrollPage("Shortcuts");auto* advancedPage=scrollPage("Advanced");
 // #1753: the Error log, a page of its own after Advanced (index 4), so earlier pages keep their indexes.
 errorLogPage_=new ErrorLogPage(settings_,this);tabs_->addTab(errorLogPage_,"Errors");
 connect(errorLogPage_,&ErrorLogPage::destinationRequested,this,&SettingsWindow::destinationSettingsRequested);
 connect(errorLogPage_,&ErrorLogPage::historyRequested,this,&SettingsWindow::historyItemRequested);
 connect(errorLogPage_,&ErrorLogPage::unresolvedChanged,navigation,[navigation](int open){auto* item=navigation->item(4);if(!item)return;item->setData(Ui::NavigationDelegate::TrailingRole,open>0?QString::number(open):QString{});item->setData(Qt::AccessibleDescriptionRole,open>0?QStringLiteral("%1 open").arg(open):QString{});});
 // The rail counts open problems from the first snapshot, before the page has loaded.
 if(const int open=settings_.errorLog()["unresolved"].toInt();open>0)emit errorLogPage_->unresolvedChanged(open);
 navigation->setCurrentRow(0);
 QList<QFormLayout*> preferenceForms;
 auto section=[&](QVBoxLayout* box,const QString& heading,const QString& description=QString{}){if(box==general||box==capture||box==advancedPage){auto* form=PreferenceRows::card(box,heading,description);preferenceForms.append(form);return form;}auto* card=Ui::card(heading,description,this);card->layout()->setSizeConstraint(QLayout::SetMinimumSize);box->addWidget(card);auto* form=new QFormLayout;Ui::styleForm(form);qobject_cast<QVBoxLayout*>(card->layout())->addLayout(form);return form;};
 auto bind=[&](QWidget* widget,const QString& path){widget->setProperty("configPath",path);};
 auto setting=[&](QFormLayout* form,const QString& label,QWidget* control,const QString& path,const QString& hint=QString{},const QString& unit=QString{}){
  if(!qobject_cast<QCheckBox*>(control))control->setAccessibleName(label);if(qobject_cast<QLineEdit*>(control)||qobject_cast<QComboBox*>(control)||qobject_cast<QAbstractSpinBox*>(control))control->setMinimumHeight(Ui::scaled(Ui::Metric::control));if(qobject_cast<QLineEdit*>(control))control->setMaximumWidth(Ui::scaled(480));if(qobject_cast<QAbstractSpinBox*>(control))control->setMaximumWidth(qMax(Ui::scaled(120),control->minimumSizeHint().width()));bind(control,path);const bool separateHints=form->property("separateFieldHints").toBool();QLabel* fieldHint=nullptr;auto* wrapper=new FormRow;wrapper->setProperty("preferenceCaption",label);wrapper->setProperty("visualRole","settingRow");auto* column=new QVBoxLayout(wrapper);column->setSizeConstraint(QLayout::SetMinimumSize);column->setContentsMargins(0,0,0,separateHints?space(Space::s):0);column->setSpacing(space(Space::xs));auto* row=new QHBoxLayout;row->setContentsMargins(0,0,0,0);row->setSpacing(space(Space::s));if(unit.isEmpty()){row->addWidget(control,1);if(qobject_cast<QLineEdit*>(control))row->setAlignment(control,Qt::AlignLeft|Qt::AlignVCenter);}
  else{auto* unitField=new QWidget;auto* unitLayout=new QHBoxLayout(unitField);unitLayout->setContentsMargins(0,0,0,0);unitLayout->setSpacing(0);control->setProperty("hasUnit",true);unitLayout->addWidget(control,1);auto* suffix=new QLabel(unit);suffix->setProperty("visualRole","unitSuffix");unitLayout->addWidget(suffix);row->addWidget(unitField,1);}
  row->addStrut(Ui::scaled(Ui::Metric::control));column->addLayout(row);auto* error=new QLabel;error->setWordWrap(true);error->setObjectName(control->objectName()+"Error");error->setProperty("visualRole","error");error->hide();column->addWidget(error);errors_[control]=error;
  if(!hint.isEmpty()){QLabel* text=control->objectName()=="s3Bucket"?new Ui::ElidedLabel:new QLabel; text->setText(hint);if(auto* elided=qobject_cast<Ui::ElidedLabel*>(text))elided->setElideMode(Qt::ElideMiddle);text->setObjectName(control->objectName()+"Hint");text->setWordWrap(control->objectName()!="s3Bucket");Ui::role(text,"meta");if(separateHints){fieldHint=text;text->setProperty("separateFieldHint",true);text->setContentsMargins(0,0,0,space(Space::s));column->setContentsMargins(0,0,0,0);}else{column->addWidget(text);text->ensurePolished();text->show();wrapper->setMinimumHeight(control->minimumHeight()+space(Space::xs)+text->minimumSizeHint().height());}}
  const auto help=settingHelp(path,control->objectName());const bool advancedRoutine=tabs_->widget(advancedTab)->isAncestorOf(form->parentWidget())&&path.section('/',-1)!="historyRetentionDays"&&control->objectName()!="debugLogging";const bool routine=(path.startsWith("/naming/")&&path.endsWith("Template"))||advancedRoutine||QStringList{"maxFileBytes","maxBatchBytes","maxFiles","pendingOperations","imageBytes","imagePixels","clipboardTextBytes","concurrentUploads"}.contains(path.section('/',-1));if(auto* check=qobject_cast<QCheckBox*>(control)){
   form->addRow(wrapper);check->setAccessibleName(check->text());if(!help.isEmpty()&&!routine&&control->objectName()!="debugLogging")row->addWidget(Ui::helpButton(check->text(),help,control,wrapper),0,Qt::AlignLeft);check->setAccessibleDescription(help.isEmpty()?hint:help+(hint.isEmpty()?QString{}:" "+hint));check->setProperty("settingDescription",check->accessibleDescription());row->setStretchFactor(check,0);row->addStretch(1);
  }else form->addRow(Ui::settingLabel(label,routine?QString{}:help,control,this),wrapper);if(routine){control->setToolTip(help);control->setAccessibleDescription(help);control->setProperty("settingDescription",help);}if(help.isEmpty()){const auto description=qobject_cast<QCheckBox*>(control)?label+(hint.isEmpty()?QString{}:". "+hint):hint;control->setAccessibleDescription(description);control->setProperty("settingDescription",description);}if(fieldHint)form->addRow(static_cast<QWidget*>(nullptr),fieldHint);return wrapper;
 };
 auto* preferences=section(general,"Startup and notifications");
 login_=new PreferenceCheck("Launch at login");login_->setObjectName("launchAtLogin");setting(preferences,"Startup",login_,"/preferences/launchAtLogin");
 unlockAtStartup_=new PreferenceCheck("Unlock saved credentials when put.gd starts");unlockAtStartup_->setObjectName("unlockCredentialsAtStartup");setting(preferences,"Credentials",unlockAtStartup_,"/preferences/unlockCredentialsAtStartup");
 notifications_=new PreferenceCheck("Show desktop notifications");notifications_->setObjectName("notifications");setting(preferences,"Notifications",notifications_,"/preferences/notifications",{});
 auto* look=section(general,"Appearance");
 appearance_=choices(this,"appearanceTheme",{});appearance_->addItem("Match the desktop","system");appearance_->addItem("Dark","dark");appearance_->addItem("Light","light");setting(look,"Theme",appearance_,"/preferences/appearance/theme");
 // "Upload with put.gd" in file managers (#1105). DEB and RPM install system-wide
 // entries that are always on; only the AppImage's per-user entries switch off.
 auto* fileManager=section(general,"File manager","Upload with put.gd in your file manager's right-click menu.");
 fileManagerMenu_=new PreferenceCheck("Add Upload with put.gd to the right-click menu");fileManagerMenu_->setObjectName("fileManagerMenu");
 const bool appImage=host::installationClass()=="appimage";
 setting(fileManager,"File manager menu",fileManagerMenu_,"/preferences/fileManagerMenu",appImage?QString{}:QStringLiteral("Always on: the installed package adds it for everyone."));
 fileManagerMenu_->setEnabled(appImage);
 fileManagerFolders_=choices(this,"fileManagerFolders",{});fileManagerFolders_->addItem("Upload as a .zip","zip");fileManagerFolders_->addItem("Upload each file","files");setting(fileManager,"Folders",fileManagerFolders_,"/preferences/fileManagerFolders");
 // Updates (#293): the daily check, its channel and the last result. The
 // resident runs the check; this page shows it and starts Check now.
 auto* updatesForm=section(general,"Updates","Nothing installs automatically.");updatesIntro_=qobject_cast<QLabel*>(PreferenceRows::sectionFor(updatesForm)->layout()->itemAt(1)->widget());updatesIntro_->setObjectName("updatesIntro");
 auto* privacy=new QToolButton;privacy->setObjectName("updatePrivacyDetails");privacy->setText("What the check sends");Ui::disclosure(privacy);updatesForm->addRow(privacy);auto* privacyText=new QLabel("GET /update/v1/stable.json or /update/v1/nightly.json on app.put.gd. Headers: User-Agent: put.gd-update-check/1; Accept: application/json; Accept-Language: en. No query or cookies. The server receives your network IP address. Version comparison happens on this computer.");privacyText->setObjectName("updatePrivacyFields");privacyText->setWordWrap(true);Ui::role(privacyText,"muted");updatesForm->addRow(privacyText);privacyText->hide();connect(privacy,&QToolButton::toggled,privacyText,&QWidget::setVisible);
 updatesForm_=updatesForm;checkUpdates_=new PreferenceCheck("Check for updates once a day");checkUpdates_->setObjectName("checkForUpdates");updateRows_.append(setting(updatesForm,"Updates",checkUpdates_,"/preferences/updates/check"));connect(checkUpdates_,&QCheckBox::toggled,this,[this]{updatesIntro_->setText(checkUpdates_->isChecked()?"Checks app.put.gd once a day. Nothing installs automatically.":"Checks app.put.gd when you choose Check now. Nothing installs automatically.");});
 updateChannel_=choices(this,"updateChannel",{});updateChannel_->addItem("Stable","stable");updateChannel_->addItem("Nightly","nightly");auto* channelRow=setting(updatesForm,"Channel",updateChannel_,"/preferences/updates/channel");updateRows_.append(channelRow);
 // A repository install shows its repository's channel and can't choose (#386).
 updateChannelSource_=new QLabel;updateChannelSource_->setObjectName("updateChannelSource");updateChannelSource_->setWordWrap(true);Ui::role(updateChannelSource_,"muted");updateChannelSource_->hide();channelRow->layout()->addWidget(updateChannelSource_);
 auto* updateRow=new FormRow;auto* updateLayout=new QHBoxLayout(updateRow);updateLayout->setContentsMargins(0,0,0,0);updateLayout->setSpacing(space(Space::s));
 updateStatus_=new QLabel;updateStatus_->setObjectName("updateStatus");updateStatus_->setWordWrap(true);updateStatus_->setAlignment(Qt::AlignLeft|Qt::AlignTop);updateStatus_->setContentsMargins(0,qMax(0,(Ui::scaled(Ui::Metric::control)-QFontMetrics(Ui::font(Ui::Type::body)).height())/2),0,0);updateStatus_->setAccessibleName("Update status");
 // Focusable, so a deep link can land on it when no update action applies.
 updateStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);updateLayout->addWidget(updateStatus_,0,Qt::AlignTop);
 checkNow_=new QPushButton("Check now");checkNow_->setObjectName("checkForUpdatesNow");updateLayout->addWidget(checkNow_,0,Qt::AlignTop);updateLayout->addStretch();
 updatesForm->addRow(Ui::settingLabel("Status",settingHelp({},"updateStatus"),updateStatus_,this),updateRow);
 auto* updateActions=new QWidget;auto* updateActionsLayout=new QHBoxLayout(updateActions);updateActionsLayout->setContentsMargins(0,0,0,0);updateActionsLayout->setSpacing(space(Space::s));
 // A repository install (#293 phase 2) updates through the system's software
 // updater, so it gets that and the apt/dnf command instead of Download.
 openUpdates_=new QPushButton(Ui::icon("download"),"Open software updates");openUpdates_->setObjectName("openSoftwareUpdates");Ui::role(openUpdates_,"primary");updateActionsLayout->addWidget(openUpdates_);
 // An AppImage installs the update itself (#293 phase 3): fetched, checked
 // against the release's SHA256SUMS and put in place of the running image.
 installUpdate_=new QPushButton(Ui::icon("download"),"Download and install");installUpdate_->setObjectName("installUpdate");Ui::role(installUpdate_,"primary");updateActionsLayout->addWidget(installUpdate_);installUpdate_->hide();
 restartUpdate_=new QPushButton(Ui::icon("redo"),"Restart put.gd");restartUpdate_->setObjectName("restartAfterUpdate");Ui::role(restartUpdate_,"primary");updateActionsLayout->addWidget(restartUpdate_);restartUpdate_->hide();
 downloadUpdate_=new QPushButton(Ui::icon("download"),"Download");downloadUpdate_->setObjectName("downloadUpdate");Ui::role(downloadUpdate_,"primary");updateActionsLayout->addWidget(downloadUpdate_);
 releaseNotes_=new QPushButton("Release notes");releaseNotes_->setObjectName("updateReleaseNotes");Ui::role(releaseNotes_,"link");updateActionsLayout->addWidget(releaseNotes_);updateActionsLayout->addStretch();
 updateActionsRow_=updateActions;updatesForm->addRow(QString(),updateActions);updateActions->hide();
 auto* updateBrowser=new Ui::BrowserFeedback;updateBrowser->setObjectName("updateBrowserFeedback");updatesForm->addRow(QString(),updateBrowser);
 installRow_=new QWidget;auto* installLayout=new QVBoxLayout(installRow_);installLayout->setContentsMargins(0,0,0,0);installLayout->setSpacing(space(Space::s));
 installProgress_=new QProgressBar;installProgress_->setObjectName("updateInstallProgress");installProgress_->setAccessibleName("Update install progress");installProgress_->setTextVisible(false);installProgress_->setMaximumWidth(320);installLayout->addWidget(installProgress_);
 installStatus_=new QLabel;installStatus_->setObjectName("updateInstallStatus");installStatus_->setWordWrap(true);installStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse);installStatus_->setAccessibleName("Update install status");installLayout->addWidget(installStatus_);
 updatesForm->addRow(Ui::settingLabel("Install",settingHelp({},"updateInstall"),installStatus_,this),installRow_);updatesForm->setRowVisible(installRow_,false);
 connect(installUpdate_,&QPushButton::clicked,this,[this]{
  // A channel switch (#344) names its channel; an older version is confirmed first.
  const auto offer=settings_.diagnostics()["update"].toObject()["install"].toObject()["switch"].toObject();
  QJsonObject params{{"confirm",true}};
  if(!offer.isEmpty()){
   params["channel"]=offer["to"];
   if(offer["older"].toBool()){
    const auto image=QFileInfo(settings_.diagnostics()["update"].toObject()["install"].toObject()["image"].toString()).fileName();
    const auto target=offer["to"].toString()+" "+offer["version"].toString();
    if(!Ui::confirm(this,"Switch to an older version","Switch to "+target+", which is older than this build ("+
       settings_.diagnostics()["update"].toObject()["build"].toObject()["version"].toString()+")?\n\nput.gd replaces "+image+" with "+target+
       ", checked against the "+offer["to"].toString()+" release's signature, at the same path. An older version may not open settings or history saved by a newer one. "
       "This version is kept as a hidden backup until the new one has started.","Switch to "+target))return;
    params["allowOlder"]=true;
   }
  }
  installUpdate_->setEnabled(false);Ui::feedback(installStatus_,"Starting…");updatesForm_->setRowVisible(installRow_,true);
  settings_.request("update.install",params,this,[g=QPointer<SettingsWindow>(this)](Error error,QJsonObject){
   if(!g)return;
   if(error){g->installUpdate_->setEnabled(true);Ui::feedback(g->installStatus_,error.message+".",true);return;}
   g->settings_.refreshSnapshot([g](Error){if(g)g->refreshUpdateStatus();});
  });
 });
 connect(restartUpdate_,&QPushButton::clicked,this,[this]{
  restartUpdate_->setEnabled(false);
  settings_.request("update.restart",{{"confirm",true}},this,[g=QPointer<SettingsWindow>(this)](Error error,QJsonObject){
   if(!g)return;g->restartUpdate_->setEnabled(true);
   if(error)Ui::feedback(g->installStatus_,error.message+".",true);else Ui::feedback(g->installStatus_,"Restarting…");
  });
 });
 auto* commandRow=new QWidget;auto* commandLayout=new QHBoxLayout(commandRow);commandLayout->setContentsMargins(0,0,0,0);commandLayout->setSpacing(space(Space::s));
 updateCommand_=new QLineEdit;updateCommand_->setObjectName("updateCommand");updateCommand_->setReadOnly(true);updateCommand_->setAccessibleName("Update command");Ui::role(updateCommand_,"technical");commandLayout->addWidget(updateCommand_,1);
 auto* copyCommand=new QPushButton("Copy");copyCommand->setObjectName("copyUpdateCommand");copyCommand->setAccessibleName("Copy the update command");commandLayout->addWidget(copyCommand);
 updateCommandRow_=commandRow;updatesForm->addRow(Ui::settingLabel("Or in a terminal",settingHelp({},"updateCommand"),updateCommand_,this),commandRow);updatesForm->setRowVisible(commandRow,false);
 // Moving a repository install to the other channel (#386): the commands to
 // run in a terminal. put.gd never runs a privileged command itself.
 otherChannelRow_=new QWidget;auto* otherLayout=new QVBoxLayout(otherChannelRow_);otherLayout->setContentsMargins(0,0,0,0);otherLayout->setSpacing(space(Space::s));
 otherChannelText_=new QLabel;otherChannelText_->setObjectName("otherChannelText");otherChannelText_->setWordWrap(true);otherLayout->addWidget(otherChannelText_);
 auto* commandsLine=new QHBoxLayout;commandsLine->setContentsMargins(0,0,0,0);commandsLine->setSpacing(space(Space::s));
 otherChannelCommands_=new QLabel;otherChannelCommands_->setObjectName("otherChannelCommands");otherChannelCommands_->setTextFormat(Qt::PlainText);otherChannelCommands_->setWordWrap(true);
 otherChannelCommands_->setTextInteractionFlags(Qt::TextSelectableByMouse);otherChannelCommands_->setAccessibleName("Commands that move to the other channel");Ui::role(otherChannelCommands_,"technical");commandsLine->addWidget(otherChannelCommands_,1);
 auto* copyOther=new QPushButton("Copy");copyOther->setObjectName("copyOtherChannelCommands");copyOther->setAccessibleName("Copy the commands that move to the other channel");commandsLine->addWidget(copyOther,0,Qt::AlignTop);
 otherLayout->addLayout(commandsLine);
 auto* channelDocs=new QPushButton("How switching channels works");channelDocs->setObjectName("otherChannelDocs");Ui::role(channelDocs,"link");otherLayout->addWidget(channelDocs,0,Qt::AlignLeft);
 updatesForm->addRow(Ui::settingLabel("Other channel",settingHelp({},"otherChannel"),otherChannelText_,this),otherChannelRow_);updatesForm->setRowVisible(otherChannelRow_,false);
 connect(copyOther,&QPushButton::clicked,this,[this]{qApp->clipboard()->setText(otherChannelCommands_->text());});
 auto* channelBrowser=new Ui::BrowserFeedback;channelBrowser->setObjectName("channelBrowserFeedback");otherLayout->addWidget(channelBrowser);
 connect(channelDocs,&QPushButton::clicked,this,[this,channelBrowser]{const QUrl url(settings_.diagnostics()["update"].toObject()["action"].toObject()["otherChannel"].toObject()["docs"].toString());if(url.scheme()=="https")host::openLink(url,channelBrowser->opening(url));});
 connect(openUpdates_,&QPushButton::clicked,this,[this]{
  openUpdates_->setEnabled(false);
  settings_.request("update.openSoftwareUpdates",{},this,[g=QPointer<SettingsWindow>(this)](Error error,QJsonObject){if(!g)return;g->openUpdates_->setEnabled(true);if(error)Ui::feedback(g->updateStatus_,error.message+".");});
 });
 connect(copyCommand,&QPushButton::clicked,this,[this]{qApp->clipboard()->setText(updateCommand_->text());});
 connect(checkNow_,&QPushButton::clicked,this,[this]{checkForUpdates({});});
 connect(downloadUpdate_,&QPushButton::clicked,this,[this,updateBrowser]{const QUrl url(settings_.diagnostics()["update"].toObject()["result"].toObject()["downloadUrl"].toString());if(url.scheme()=="https")host::openLink(url,updateBrowser->opening(url));});
 connect(releaseNotes_,&QPushButton::clicked,this,[this,updateBrowser]{const QUrl url(settings_.diagnostics()["update"].toObject()["result"].toObject()["notesUrl"].toString());if(url.scheme()=="https")host::openLink(url,updateBrowser->opening(url));});

 auto* captureDefaults=section(capture,"Capture output","Where captures and clipboard uploads go, and whether they get a share link.");
 captureProfile_=choices(this,"defaultCaptureDestination",{});auto* destinationRow=setting(captureDefaults,"Destination",captureProfile_,{});
 // With no destination and no local copy every capture fails: say so right under
 // Destination, where it is seen first, with the two ways out (#596 review).
 captureOutputBlock_=new QWidget;captureOutputBlock_->setObjectName("captureOutputBlock");auto* blockLayout=new QVBoxLayout(captureOutputBlock_);blockLayout->setContentsMargins(0,space(Space::xs),0,0);blockLayout->setSpacing(space(Space::s));
 captureOutputNotice_=Ui::warningLabel({});captureOutputNotice_->setObjectName("captureOutputNotice");captureOutputNotice_->setWordWrap(true);blockLayout->addWidget(captureOutputNotice_);
 auto* blockActions=new QHBoxLayout;blockActions->setContentsMargins(0,0,0,0);blockActions->setSpacing(space(Space::s));
 auto* enableLocal=new QPushButton("Keep a local copy");enableLocal->setObjectName("enableLocalCopies");enableLocal->setAccessibleName("Turn on Keep a local copy");blockActions->addWidget(enableLocal);
 chooseOutput_=new QPushButton("Choose a destination");chooseOutput_->setObjectName("chooseCaptureDestination");blockActions->addWidget(chooseOutput_);blockActions->addStretch();blockLayout->addLayout(blockActions);
 qobject_cast<QVBoxLayout*>(destinationRow->layout())->addWidget(captureOutputBlock_);captureOutputBlock_->hide();
 connect(enableLocal,&QPushButton::clicked,this,[this]{keep_->setChecked(true);keep_->setFocus(Qt::OtherFocusReason);});
 connect(chooseOutput_,&QPushButton::clicked,this,[this]{if(captureProfile_->count()>1){captureProfile_->setFocus(Qt::OtherFocusReason);captureProfile_->showPopup();}else emit destinationsRequested();});
 captureSharing_=choices(this,"defaultCaptureSharing",{});for(const auto& mode:QStringList{"default","share","private"})captureSharing_->addItem(sharingLabel(mode),mode);setting(captureDefaults,"Sharing",captureSharing_,{});
 uploadClipboard_=choices(this,"uploadClipboardBehavior",{});uploadClipboard_->addItem("Copy each link as it finishes","each_link");uploadClipboard_->addItem("Copy a list when uploads finish","list");setting(captureDefaults,"After uploading",uploadClipboard_,"/preferences/uploadClipboardBehavior");
 captureExceptions_=new QToolButton;captureExceptions_->setObjectName("captureExceptionsToggle");captureExceptions_->setAccessibleName("Different output for some actions");Ui::disclosure(captureExceptions_);auto* exceptionsSummary=Ui::label({},"secondary");exceptionsSummary->setObjectName("captureExceptionsSummary");PreferenceRows::disclosure(captureDefaults,"Different output for some actions",exceptionsSummary,captureExceptions_,true);
 // Only exceptions occupy rows; the hidden inheritance boxes keep the existing
 // config, view-state and agent paths.
 auto* exceptionBody=new QWidget;exceptionBody->setObjectName("captureExceptions");exceptionBody->setMaximumWidth(Ui::scaled(760));auto* exceptionBox=new QVBoxLayout(exceptionBody);exceptionBox->setContentsMargins(0,0,0,0);exceptionBox->setSpacing(space(Space::m));qobject_cast<QVBoxLayout*>(PreferenceRows::sectionFor(captureDefaults)->layout())->addWidget(exceptionBody);exceptionBox->setContentsMargins(0,space(Space::xs),0,0);exceptionBody->hide();
 auto* exceptionRows=captureExceptionRows_=new QGridLayout;exceptionRows->setContentsMargins(0,0,0,0);exceptionRows->setHorizontalSpacing(space(Space::m));exceptionRows->setVerticalSpacing(space(Space::s));exceptionBox->addLayout(exceptionRows);auto* addException=new QToolButton;addException->setObjectName("addCaptureException");addException->setText("Add an exception…");addException->setIcon(Ui::icon("add"));addException->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);Ui::role(addException,"quiet");addException->setProperty("uiFlush",true);addException->setProperty("preferenceAddRow",true);addException->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);addException->setProperty("alignLeft",true);addException->setProperty("noChevron",true);addException->setPopupMode(QToolButton::InstantPopup);auto* exceptionMenu=new QMenu(addException);addException->setMenu(exceptionMenu);captureDefaults->addRow(addException);addException->hide();
 connect(captureExceptions_,&QToolButton::toggled,this,[=,this](bool open){bool overrides=false;for(auto* inherit:std::as_const(useCaptureDefault_))overrides|=!inherit->isChecked();exceptionBody->setVisible(open&&overrides);addException->setVisible(open);emit viewStateChanged();});
 for(const auto& action:Settings::presentationOrder()){
  auto* inherit=useCaptureDefault_[action]=new QCheckBox(action=="record"?"Use Capture region output":"Use default",exceptionBody);inherit->setObjectName("useDefault"+title(action));inherit->setAccessibleName(actionLabel(action)+(action=="record"?": use Capture region output":": use default output"));inherit->hide();
  auto* line=new QWidget;line->setObjectName("captureRow"+title(action));auto* lineLayout=new QHBoxLayout(line);lineLayout->setContentsMargins(0,0,0,0);lineLayout->setSpacing(space(Space::m));
  auto* summary=captureSummary_[action]=Ui::warningLabel({});summary->setObjectName("captureSummary"+title(action));summary->setWordWrap(true);summary->setTextInteractionFlags(Qt::TextSelectableByMouse);Ui::role(summary,"secondary");
  auto* override=captureOverride_[action]=new QCheckBox("Use a different output");override->setObjectName("override"+title(action));override->setAccessibleName(actionLabel(action)+": use a different output");override->setParent(exceptionBody);override->hide();
  auto* name=new QLabel(QString(actionLabel(action)).replace('&',QStringLiteral("&&")));name->setObjectName("captureRowName"+title(action));name->setBuddy(override);Ui::role(name,"strong");lineLayout->addWidget(name);
  auto* editor=captureEditors_[action]=new QWidget;editor->setObjectName("captureEditor"+title(action));auto* fields=new QFormLayout(editor);Ui::styleForm(fields);preferenceForms.append(fields);fields->setContentsMargins(0,0,0,space(Space::s));
  actionProfiles_[action]=choices(this,"profile"+title(action),{});publicationModes_[action]=choices(this,"publication"+title(action),{});for(const auto& mode:QStringList{"default","share","private"})publicationModes_[action]->addItem(sharingLabel(mode),mode);
  for(auto [combo,caption,path]:{std::tuple{actionProfiles_[action],QString("Destination"),"/actions/"+action+"/profileId"},std::tuple{publicationModes_[action],QString("Sharing"),"/actions/"+action+"/publicationMode"}}){
   auto* wrapper=new QWidget(editor);auto* column=new QVBoxLayout(wrapper);column->setSizeConstraint(QLayout::SetMinimumSize);column->setContentsMargins(0,0,0,0);column->setSpacing(space(Space::s));column->addWidget(combo);fields->addRow(Ui::settingLabel(caption,{},combo,editor),wrapper);bind(combo,path);
   auto* error=new QLabel;error->setWordWrap(true);error->setObjectName(combo->objectName()+"Error");error->setProperty("visualRole","error");error->hide();column->addWidget(error);errors_[combo]=error;}
  // The explanation behind a failing output, for view state and agents; the row's summary says it.
  auto* status=captureStatus_[action]=new QLabel(editor);status->setObjectName("captureStatus"+title(action));status->setWordWrap(true);status->hide();
  QWidget* effective=summary;if(action=="record"){auto* inheritance=new QLabel("Record screen: Same as Capture region");inheritance->setObjectName("recordingOutputInheritance");inheritance->setWordWrap(true);Ui::role(inheritance,"muted");captureDefaults->addRow(inheritance);}

  auto* row=new Ui::Surface("surface",{},exceptionBody);row->setProperty("preferenceCard",true);row->setObjectName("captureException"+title(action));auto* box=new QVBoxLayout(row);box->setContentsMargins(0,0,0,0);box->setSpacing(0);auto* header=new QWidget;header->setProperty("preferenceRow",true);auto* heading=new QHBoxLayout(header);heading->setContentsMargins(space(Space::l),space(Space::m),space(Space::l),space(Space::m));heading->setSpacing(space(Space::l));auto* caption=new QVBoxLayout;caption->setContentsMargins(0,0,0,0);caption->setSpacing(space(Space::xs));caption->addWidget(line);caption->addWidget(effective);heading->addLayout(caption,1);auto* remove=new QPushButton("Remove",row);remove->setObjectName("remove"+title(action)+"Exception");remove->setAccessibleName("Remove "+actionLabel(action).toLower()+" exception");Ui::role(remove,"quiet");heading->addWidget(remove);box->addWidget(header);box->addWidget(editor);exceptionRows->addWidget(row,exceptionRows->rowCount(),0);editor->hide();row->hide();
  auto* add=exceptionMenu->addAction(QString(actionLabel(action)).replace('&',QStringLiteral("&&")));add->setObjectName("add"+title(action)+"Exception");connect(add,&QAction::triggered,this,[this,inherit]{inherit->setChecked(false);captureExceptions_->setChecked(true);});connect(remove,&QPushButton::clicked,inherit,[inherit]{inherit->setChecked(true);});connect(inherit,&QCheckBox::toggled,add,[add](bool inherited){add->setVisible(inherited);});
  connect(override,&QCheckBox::toggled,this,[inherit](bool on){inherit->setChecked(!on);});
  connect(inherit,&QCheckBox::toggled,this,[this,inherit]{if(!loading_)queueCaptureEdit(inherit);});
 }
 captureProfile_->setModel(actionProfiles_["region"]->model());
 for(auto* combo:{captureProfile_,captureSharing_})connect(combo,&QComboBox::currentIndexChanged,this,[this,combo]{if(!loading_)queueCaptureEdit(combo);});
 // Local copies sit with the destination (one output decision on one page); the
 // config paths and autosave are unchanged.
 // #629: Live desktop (default) or Screenshot first, for region screenshots.
 auto* selectionForm=section(capture,"Region selection","Capture region and Capture region & edit. Recording always selects over the live desktop.");
 regionSelection_=choices(this,"regionSelectionMode",{});regionSelection_->addItem("Live desktop","live");regionSelection_->addItem("Screenshot first","screenshot_first");setting(selectionForm,"While selecting",regionSelection_,"/preferences/regionSelectionMode");
 // #1363: the delay the tray and the dashboard already offer, and whether shortcuts wait for it too.
 auto* delayForm=section(capture,"Capture delay","Screenshots and Copy text can wait a few seconds first, with a countdown you can cancel. Scrolling capture and recording never wait.");
 captureDelay_=choices(this,"captureDelaySeconds",{});for(const int seconds:Settings::captureDelayChoices())captureDelay_->addItem(seconds?QStringLiteral("%1 seconds").arg(seconds):QStringLiteral("None"),seconds);setting(delayForm,"Wait before capturing",captureDelay_,"/preferences/captureDelaySeconds");
 captureDelayShortcuts_=new PreferenceCheck("Also delay captures started by a hotkey");captureDelayShortcuts_->setObjectName("captureDelayShortcuts");setting(delayForm,"Hotkeys",captureDelayShortcuts_,"/preferences/captureDelayShortcuts",{});
 // #984: the rate of new recordings; the recorder lowers it while one runs if the machine can't keep up.
 auto* scrollForm=section(capture,"Scrolling capture","Select scrollable content and capture a tall image.");
 scrollHeight_=new QSpinBox(this);scrollHeight_->setObjectName("scrollCaptureMaxHeight");scrollHeight_->setRange(2000,60000);scrollHeight_->setSingleStep(1000);setting(scrollForm,"Maximum height (px)",scrollHeight_,"/preferences/scrollCaptureMaxHeight");
 scrollAutoScroll_=new PreferenceCheck("Start with auto-scroll");scrollAutoScroll_->setObjectName("scrollCaptureAutoScroll");setting(scrollForm,"Who scrolls",scrollAutoScroll_,"/preferences/scrollCaptureAutoScroll","Off: the capture waits for you to scroll. On: put.gd turns the page, and moving the mouse pauses it.");
 scrollFromTop_=new PreferenceCheck("Auto-scroll goes to the top first");scrollFromTop_->setObjectName("scrollCaptureFromTop");setting(scrollForm,"Start position",scrollFromTop_,"/preferences/scrollCaptureFromTop");
 // #1428: the languages Copy text reads. Up to three, from the installed
 // Tesseract data; Automatic is the desktop's language when installed, then English.
 auto* textForm=section(capture,"Text recognition","Copy text reads the text and QR codes in a region you select. Nothing is saved or uploaded.");
 for(const auto* caption:{"Language","Second language","Third language"}){
  auto* language=choices(this,QStringLiteral("textRecognitionLanguage%1").arg(textLanguages_.size()+1),{});textLanguages_.append(language);
  setting(textForm,caption,language,"/preferences/textRecognitionLanguages");
 }
 // A second or third language only follows a chosen first one.
 connect(textLanguages_.first(),&QComboBox::currentIndexChanged,this,[this]{const bool chosen=!textLanguages_.first()->currentData().toString().isEmpty();for(auto* language:textLanguages_.mid(1))language->setEnabled(chosen);});
 textRecognitionStatus_=Ui::label({},"body");textRecognitionStatus_->setObjectName("textRecognitionStatus");textRecognitionStatus_->setAccessibleName("Text recognition status");Ui::role(textRecognitionStatus_,"muted");textRecognitionStatus_->setWordWrap(true);textForm->addRow(textRecognitionStatus_);
 // #1618: Quick redact's saved list. Words are matched ignoring case, patterns are regular expressions as written; both stay in config.json on this machine.
 {const auto lines=[this](const QString& name,const QString& placeholder){auto* edit=new QPlainTextEdit(this);edit->setObjectName(name);edit->setPlaceholderText(placeholder);edit->setTabChangesFocus(true);edit->setLineWrapMode(QPlainTextEdit::NoWrap);edit->setMinimumHeight(Ui::scaled(88));edit->setMaximumHeight(Ui::scaled(128));return edit;};
  const auto note=[&](const QString& name,const QString& text){auto* label=Ui::label(text,"body");label->setObjectName(name);Ui::role(label,"muted");label->setWordWrap(true);textForm->addRow(label);};
  quickRedactTerms_=lines("quickRedactTerms","One word or phrase per line");
  setting(textForm,"Always suggest redacting",quickRedactTerms_,"/preferences/quickRedactTerms");
  note("quickRedactTermsHint","Quick redact's \"My redaction list\" covers these wherever recognized text has them, ignoring case. They stay in this configuration file and are never uploaded or logged.");
  quickRedactPatterns_=lines("quickRedactPatterns","One regular expression per line, for example \\bJIRA-\\d+\\b");
  setting(textForm,"Patterns (regular expressions)",quickRedactPatterns_,"/preferences/quickRedactPatterns");
  note("quickRedactPatternsHint","Case matters unless a pattern starts with (?i). A pattern that matches empty text is refused.");}
 auto* recordingForm=section(capture,"Screen recording","Record screen, from the tray, the dashboard or its hotkey.");
 recordingFrameRate_=choices(this,"recordingFrameRate",{});recordingFrameRate_->addItem("30 frames per second",30);recordingFrameRate_->addItem("60 frames per second",60);setting(recordingForm,"Frame rate",recordingFrameRate_,"/preferences/recordingFrameRate");
 // #1364: which input the recording bar's Microphone toggle opens, and rings at clicks.
 recordingMicrophone_=choices(this,"recordingMicrophoneDevice",{});recordingMicrophone_->addItem("System default",QString());setting(recordingForm,"Microphone",recordingMicrophone_,"/preferences/recordingMicrophoneDevice");connect(recordingMicrophone_,&QComboBox::activated,this,[this]{refreshRecordingInputs(true);});
 recordingMicrophoneStatus_=Ui::label({},"body");recordingMicrophoneStatus_->setObjectName("recordingMicrophoneStatus");Ui::role(recordingMicrophoneStatus_,"muted");recordingMicrophoneStatus_->setWordWrap(true);recordingMicrophoneStatus_->hide();recordingForm->addRow(recordingMicrophoneStatus_);
 recordingClicks_=new PreferenceCheck("Draw a ring at each click");recordingClicks_->setObjectName("recordingClickHighlights");setting(recordingForm,"Click highlights",recordingClicks_,"/preferences/recordingClickHighlights",{});
 recordingClicksStatus_=Ui::label({},"body");recordingClicksStatus_->setObjectName("recordingClickHighlightsStatus");Ui::role(recordingClicksStatus_,"muted");recordingClicksStatus_->setWordWrap(true);recordingClicksStatus_->hide();recordingForm->addRow(recordingClicksStatus_);
 auto* files=section(capture,"Local copies","Captures and clipboard uploads saved on this computer, with or without uploading.");
 keep_=new PreferenceCheck("Keep a local copy");keep_->setObjectName("keepUploadCopies");setting(files,"Each capture",keep_,"/archive/keepUploads",{});
 root_=input(this,"archiveRoot");auto* rootRow=setting(files,"Save to",root_,"/archive/root","Use absolute folder paths.");rootRow->setMinimumHeight(0);connect(root_,&QLineEdit::textChanged,this,[this]{inlineError(root_,{});});
 folderChooser_=new FileDialog(this);
 auto browse=[this](QLineEdit* field){chooseOutputFolder(field);};
 // Every folder row browses the same way, beside its field.
 // A chooser that could not open says so under its row, with Try again; the folder stays as it was.
 auto chooserFailure=[=,this](QWidget* row,QLineEdit* field){auto* failure=new QWidget;failure->setObjectName(field->objectName()+"ChooserFailure");auto* line=new QHBoxLayout(failure);line->setContentsMargins(0,0,0,0);line->setSpacing(space(Space::s));auto* message=new QLabel;message->setObjectName(field->objectName()+"ChooserError");message->setWordWrap(true);Ui::role(message,"error");line->addWidget(message,1);auto* retry=new QPushButton("Try again");retry->setObjectName(field->objectName()+"ChooserRetry");retry->setAccessibleName("Try opening the folder chooser again");line->addWidget(retry,0,Qt::AlignTop);qobject_cast<QVBoxLayout*>(row->layout())->insertWidget(1,failure);failure->hide();chooserFailures_[field]=failure;connect(retry,&QPushButton::clicked,this,[=]{browse(field);});};
 auto* chooseRoot=new QPushButton("Browse…");chooseRoot->setAccessibleName("Choose default output folder");qobject_cast<QHBoxLayout*>(rootRow->layout()->itemAt(0)->layout())->addWidget(chooseRoot);connect(chooseRoot,&QPushButton::clicked,this,[=,this]{browse(root_);});chooserFailure(rootRow,root_);
 auto* folderToggle=new QToolButton;folderToggle->setObjectName("categoryFoldersToggle");folderToggle->setText("Different folders by file type");Ui::disclosure(folderToggle);files->addRow(folderToggle);auto* folderPanel=new QWidget;folderPanel->setObjectName("categoryFolders");auto* categoryFolderForm=new QFormLayout(folderPanel);Ui::styleForm(categoryFolderForm);preferenceForms.append(categoryFolderForm);categoryFolderForm->setContentsMargins(0,0,0,0);files->addRow(folderPanel);folderPanel->hide();connect(folderToggle,&QToolButton::toggled,this,[this,folderPanel](bool expanded){folderPanel->setVisible(expanded);emit viewStateChanged();});
 const QHash<QString,QString> categoryLabels{{"images","Images"},{"videos","Recordings"},{"text","Clipboard text"},{"other","Other files"}};for(const auto& category:QStringList{"images","videos","text","other"}){auto* field=folders_[category]=input(this,"folder"+title(category));field->setPlaceholderText("Same as Save to");connect(field,&QLineEdit::textChanged,folderToggle,[folderToggle](const QString& text){if(!text.isEmpty())folderToggle->setChecked(true);});auto* row=setting(categoryFolderForm,categoryLabels.value(category),field,"/archive/folders/"+category);auto* button=new QPushButton("Browse…");button->setAccessibleName("Choose "+categoryLabels.value(category).toLower()+" folder");qobject_cast<QHBoxLayout*>(row->layout()->itemAt(0)->layout())->addWidget(button);connect(button,&QPushButton::clicked,this,[=]{browse(field);});chooserFailure(row,field);}
 connect(keep_,&QCheckBox::toggled,this,[this]{if(!loading_)updateCaptureInheritance();});
 auto* namingForm=section(capture,"File names");naming_=choices(this,"namingMode",{});naming_->addItem("Keep original names","original");naming_->addItem("Random names","generated");naming_->addItem("Custom template","template");setting(namingForm,"Naming",naming_,"/naming/mode");namingForm_=namingForm;
 randomStyle_=choices(this,"randomStyle",{});randomStyle_->addItem("UUID","uuid");randomStyle_->addItem("Random characters","string");setting(namingForm,"Random name",randomStyle_,"/naming/randomStyle");
 randomLength_=new QSpinBox(this);randomLength_->setObjectName("randomLength");randomLength_->setRange(4,64);setting(namingForm,"Length",randomLength_,"/naming/randomLength",{},"characters");
 randomUppercase_=new PreferenceCheck("Include capital letters");randomUppercase_->setObjectName("randomUppercase");setting(namingForm,"Capitals",randomUppercase_,"/naming/randomUppercase");
 randomDigits_=new PreferenceCheck("Include numbers");randomDigits_->setObjectName("randomDigits");setting(namingForm,"Numbers",randomDigits_,"/naming/randomDigits");
 connect(naming_,&QComboBox::currentIndexChanged,this,&SettingsWindow::showRandomNameRows);connect(randomStyle_,&QComboBox::currentIndexChanged,this,&SettingsWindow::showRandomNameRows);
 collision_=choices(this,"collisionPolicy",{"rename","skip","overwrite"});for(int i=0;i<collision_->count();++i)collision_->setItemData(i,collision_->itemText(i));collision_->setItemText(0,"Add a unique suffix");collision_->setItemText(1,"Skip duplicate names");collision_->setItemText(2,"Replace the checked object");setting(namingForm,"When a name exists",collision_,"/naming/collisionPolicy");
 auto* patternToggle=new QToolButton;patternToggle->setObjectName("customFileNamePatternToggle");patternToggle->setText("Custom file-name pattern");Ui::disclosure(patternToggle);auto* patternHeading=new QWidget;auto* patternHeadingLine=new QHBoxLayout(patternHeading);patternHeadingLine->setContentsMargins(0,0,0,0);patternHeadingLine->setSpacing(space(Space::xs));patternHeadingLine->addWidget(patternToggle);patternHeadingLine->addWidget(Ui::helpButton("Custom file-name pattern",settingHelp("/naming/fileTemplate","fileTemplate"),patternToggle,patternHeading));patternHeadingLine->addStretch(1);namingForm->addRow(patternHeading);auto* patternPanel=new QWidget;patternPanel->setObjectName("customFileNamePattern");auto* patternForm=new QFormLayout(patternPanel);Ui::styleForm(patternForm);preferenceForms.append(patternForm);namingForm->addRow(patternPanel);patternPanel->hide();connect(patternToggle,&QToolButton::toggled,patternPanel,&QWidget::setVisible);connect(naming_,&QComboBox::currentIndexChanged,patternToggle,[this,patternToggle]{if(naming_->currentData()=="template")patternToggle->setChecked(true);});
 for(const auto& key:QStringList{"fileTemplate","captureTemplate","imageTemplate","textTemplate"}){templates_[key]=input(this,key);setting(patternForm,key=="fileTemplate"?"Copied & uploaded files":key=="captureTemplate"?"Screen captures":key=="imageTemplate"?"Clipboard images":"Clipboard text",templates_[key],"/naming/"+key);auto* note=PreferenceRows::caption({});note->setObjectName(key+"Applicability");qobject_cast<QVBoxLayout*>(errors_[templates_[key]]->parentWidget()->layout())->addWidget(note);auto applicability=[this,key,note]{const auto mode=naming_->currentData().toString();const bool inactive=mode=="generated"||(key=="fileTemplate"&&mode!="template");note->setText(mode=="generated"?"Inactive while Random names is selected.":"Used only with Custom template naming.");note->setVisible(inactive);};connect(naming_,&QComboBox::currentIndexChanged,note,applicability);applicability();}
 // The rail, the page heading and its intro already name the place: the
// editor follows under one rule, without a nested "Hotkeys" heading.
 auto* shortcutCard=new QFrame;shortcutCard->setObjectName("shortcutsCard");shortcutCard->setMaximumWidth(Ui::scaled(760));auto* shortcutBox=new QVBoxLayout(shortcutCard);shortcutBox->setContentsMargins(0,0,0,0);shortcutBox->setSpacing(0);shortcutsPage->addWidget(shortcutCard);shortcuts_=new ShortcutEditor(this);shortcutBox->addWidget(shortcuts_);pageTitles["Shortcuts"]->addWidget(shortcuts_->refreshButton(),0,Qt::AlignTop);qobject_cast<QVBoxLayout*>(pageTitles["Shortcuts"]->parentWidget()->layout())->addWidget(shortcuts_->statusLabel());shortcuts_->findChild<QToolButton*>("shortcutDetailsToggle")->hide();shortcuts_->setRecordingControl([session=&settings_,editor=shortcuts_](bool active,std::function<void(Error)> reply){session->setShortcutRecording(active,editor,std::move(reply));});shortcuts_->setRefreshAction([this]{settings_.refreshSnapshot();});
 for(const auto& action:Settings::shortcutOrder()){auto* edit=shortcuts_->findChild<QLineEdit*>("shortcut"+title(action));bind(edit,"/actions/"+action+"/shortcut");}
 // The reset action stays with the disclosed limits.
 auto* resetAll=new QPushButton("Reset resource and history limits…");resetAll->setObjectName("resetAdvancedDefaults");resetAll->setToolTip("Resets the upload, image, memory, cache and history limits. Diagnostics and the config file are unchanged.");
 // One column, like the other pages: two narrow columns crowded labels and values.
 resourceLimitsToggle_=new QToolButton;resourceLimitsToggle_->setObjectName("resourceLimitsToggle");resourceLimitsToggle_->setText("Resource limits");Ui::disclosure(resourceLimitsToggle_);resourceLimitsToggle_->setProperty("uiFlush",true);resourceLimits_=new QWidget;resourceLimits_->setObjectName("resourceLimits");auto* resources=new QVBoxLayout(resourceLimits_);resources->setContentsMargins(space(Space::l),space(Space::m),space(Space::l),space(Space::m));resources->setSpacing(space(Space::xl));advancedPage->addWidget(resourceLimits_);resourceLimits_->hide();connect(resourceLimitsToggle_,&QToolButton::toggled,resourceLimits_,&QWidget::setVisible);connect(resourceLimitsToggle_,&QToolButton::toggled,this,&SettingsWindow::viewStateChanged);
 Ui::role(resetAll,"quiet");auto* left=resources;auto* right=advancedPage;
 const auto defaults=Settings::defaults();
 auto number=[&](QFormLayout* form,const QString& key,const QString& name,const QString& unit){auto* edit=numbers_[key]=input(this,key);QWidget* editor=edit;if(key=="historyRetentionDays"){auto* combo=choices(this,"historyRetentionChoice",{});combo->setEditable(true);combo->setInsertPolicy(QComboBox::NoInsert);combo->addItem("Forever",QString{});combo->addItem("30 days","30");combo->addItem("90 days","90");combo->addItem("1 year","365");combo->setLineEdit(edit);editor=combo;connect(combo,&QComboBox::activated,this,[this,combo,edit](int index){if(index==0)combo->setCurrentIndex(0);edit->setText(index==0?combo->itemText(0):combo->itemData(index).toString());queueEdit(edit);});connect(edit,&QLineEdit::textChanged,combo,[combo](const QString& value){if(value.isEmpty()){combo->setCurrentIndex(-1);combo->setCurrentIndex(0);}});}
  else if(key=="failedArtifactDays"){auto* combo=choices(this,"failedArtifactChoice",{});combo->addItem("Forever","0");for(const int days:{1,3,7,14,30,90})combo->addItem(days==1?QString("1 day"):QString("%1 days").arg(days),QString::number(days));
   // Retain the numeric binding and agent identity behind the named choice.
   edit->hide();editor=combo;connect(combo,&QComboBox::currentIndexChanged,this,[combo,edit](int index){if(index>=0)edit->setText(combo->itemData(index).toString());});connect(edit,&QLineEdit::textChanged,combo,[combo](const QString& value){bool valid=false;const auto days=value.toLongLong(&valid);if(!valid||days<0)return;const auto stored=QString::number(days);const QSignalBlocker block(combo);int index=combo->findData(stored);if(index<0){combo->addItem(QString("%1 days").arg(days),stored);index=combo->count()-1;}combo->setCurrentIndex(index);});}
  // File, batch and waiting-upload sizes: Auto follows this computer's memory, an empty field is No limit.
  else if(Settings::artifactByteLimitKeys().contains(key)){auto* combo=choices(this,key+"Choice",{});combo->setEditable(true);combo->setInsertPolicy(QComboBox::NoInsert);combo->addItem("Auto","Auto");combo->addItem("No limit",QString{});combo->setLineEdit(edit);edit->setPlaceholderText("No limit");editor=combo;connect(combo,&QComboBox::activated,this,[this,combo,edit](int index){edit->setText(combo->itemData(index).toString());queueEdit(edit);});}
  if(editor!=edit&&key!="historyRetentionDays"&&key!="failedArtifactDays")editor->setProperty("formWidth",120);const bool retention=defaults["retention"].toObject().contains(key);auto* wrap=setting(form,name,editor,QString(retention?"/retention/":"/limits/")+key,{},key=="failedArtifactDays"?QString{}:unit);if(editor!=edit){errors_[edit]=errors_[editor];edit->setAccessibleName(editor->accessibleName());edit->setAccessibleDescription(editor->accessibleDescription());edit->setProperty("settingDescription",editor->property("settingDescription"));bind(edit,QString(retention?"/retention/":"/limits/")+key);}if(key.endsWith("Bytes")){const auto description=editor->accessibleDescription()+" Size labels use 1024 math: 1 MB = 1,048,576 bytes; 1 GB = 1,073,741,824 bytes.";editor->setAccessibleDescription(description);edit->setAccessibleDescription(description);editor->setProperty("settingDescription",description);edit->setProperty("settingDescription",description);}const QJsonValue value=retention?defaults["retention"].toObject()[key]:defaults["limits"].toObject()[key];const qint64 factor=displayUnit(key);const auto text=value.isNull()?QString{}:value.isString()?QStringLiteral("Auto"):ByteSize::exact(value.toInteger(),factor);edit->setProperty("defaultText",text);if(!retention)connect(edit,&QLineEdit::textChanged,this,[this]{updateResourceLimits();});if(!unit.isEmpty()){for(auto* suffix:wrap->findChildren<QLabel*>())if(suffix->property("visualRole")=="unitSuffix"){suffix->setObjectName(key+"Unit");auto updateUnit=[edit,editor,suffix]{const auto value=edit->text().trimmed();bool numeric=false;value.toDouble(&numeric);const bool showUnit=numeric&&edit->objectName()!="failedArtifactDays";suffix->setVisible(showUnit);editor->setProperty("hasUnit",showUnit);editor->update();};connect(edit,&QLineEdit::textChanged,this,updateUnit);updateUnit();}}auto* reset=new QPushButton("Reset");reset->setAccessibleName("Reset "+name+" to default");reset->setToolTip("Default: "+(text.isEmpty()?QString(key=="historyRetentionDays"?"Forever":"No limit"):text+(unit.isEmpty()||text=="Auto"?QString{}:" "+unit)));auto* row=qobject_cast<QHBoxLayout*>(wrap->layout()->itemAt(0)->layout());
  // Exact value boxes stay compact; their suffixes carry the units. A changed value
  // gets Reset beside it, an unchanged one nothing.
  if(!qobject_cast<QComboBox*>(editor))editor->setFixedWidth(Ui::scaled(120));for(int i=0;i<row->count();++i)row->setStretch(i,0);
  // What Auto resolves to on this computer (#149), from settings.get effectiveLimits.
  if(Settings::artifactByteLimitKeys().contains(key)){auto* resolved=limitHints_[key]=new QLabel;resolved->setObjectName(key+"Resolved");Ui::role(resolved,"muted");resolved->setWordWrap(true);resolved->hide();
   // Below the value and its unit, so a long hint never stretches that control row.
   qobject_cast<QVBoxLayout*>(wrap->layout())->insertWidget(1,resolved);connect(edit,&QLineEdit::textChanged,this,[this]{updateLimitHints();});}
  row->addWidget(reset);row->addStretch(1);auto update=[=]{const auto value=edit->text()=="Forever"?(key=="failedArtifactDays"?QString("0"):QString{}):edit->text();const bool changed=value!=text;reset->setVisible(changed);wrap->setProperty("changed",changed);wrap->style()->unpolish(wrap);wrap->style()->polish(wrap);};connect(edit,&QLineEdit::textChanged,this,update);connect(reset,&QPushButton::clicked,this,[=,this]{edit->setText(text);queueEdit(edit);});update();};
 auto* uploads=section(left,"Uploads");number(uploads,"concurrentUploads","Uploads at once",{});number(uploads,"maxFileBytes","Largest file","GB");number(uploads,"maxBatchBytes","Largest batch","GB");number(uploads,"pendingArtifactBytes","Space for queued files","GB");number(uploads,"maxFiles","Files in a batch",{});number(uploads,"pendingOperations","Queued operations",{});
 uploadReserveHint_=new QLabel;uploadReserveHint_->setObjectName("uploadReserveHint");uploadReserveHint_->setWordWrap(true);Ui::role(uploadReserveHint_,"muted");qobject_cast<QVBoxLayout*>(errors_[numbers_["pendingArtifactBytes"]]->parentWidget()->layout())->insertWidget(1,uploadReserveHint_);
 auto* images=section(left,"Images and clipboard");number(images,"imageBytes","Largest image","MB");number(images,"imagePixels","Largest image size","megapixels");number(images,"clipboardTextBytes","Clipboard text limit","MB");
 auto* memory=section(left,"Memory and cache");number(memory,"pendingImageBytes","Memory for queued images","MB");number(memory,"previewCacheBytes","Thumbnail cache","MB");
 auto* retention=section(right,"History");number(retention,"historyRetentionDays","Keep history for","days");number(retention,"failedArtifactDays","Keep files for retry","days");auto* retryConsequence=PreferenceRows::caption("After this period, local retry files may be removed. Uploaded files aren't affected.");retryConsequence->setObjectName("failedArtifactConsequence");qobject_cast<QVBoxLayout*>(errors_[numbers_["failedArtifactDays"]]->parentWidget()->layout())->addWidget(retryConsequence);number(retention,"recentActivityLimit","Items in recent list",{});retention->addRow(resourceLimitsToggle_);retention->addRow(resourceLimits_);
 resources->addWidget(resetAll,0,Qt::AlignRight);connect(resetAll,&QPushButton::clicked,this,[this]{if(!Ui::confirm(this,"Reset limits to defaults","Reset the limits on this page to their defaults: uploads, images and clipboard, memory and cache, and history retention?\n\nDebug logging, diagnostics, the config file and every other setting stay as they are.","Reset limits"))return;// #1704: the reset limits save as one operation.
  QStringList paths;{const QScopedValueRollback quiet(loading_,true);for(auto* field:std::as_const(numbers_)){field->setText(field->property("defaultText").toString());if(const auto path=field->property("configPath").toString();!path.isEmpty())paths.append(path);}}
  for(auto* field:std::as_const(numbers_))inlineError(field,{});queueGlobal("limits",paths,findControl<QPushButton*>("resetAdvancedDefaults"));});
 // Diagnostics (#122): what the persistent log holds and the package a maintainer needs.
 // Advanced, above Config file (#238); Reset limits to defaults leaves it alone.
 auto* diagnosticsForm=section(right,"Diagnostics","Logs and diagnostics packages stay on this computer.");
 debugLogging_=new PreferenceCheck("Keep detailed debug logs for 24 hours");debugLogging_->setObjectName("debugLogging");auto* debugRow=setting(diagnosticsForm,"Debug logging",debugLogging_,"/preferences/diagnostics/debugLogging");
 debugLoggingStatus_=new PreferenceCaption({});debugLoggingStatus_->setObjectName("debugLoggingStatus");debugLoggingStatus_->setWordWrap(true);Ui::role(debugLoggingStatus_,"muted");qobject_cast<QVBoxLayout*>(debugRow->layout())->addWidget(debugLoggingStatus_);
 logSummary_=new QLabel;logSummary_->setObjectName("logSummary");logSummary_->setWordWrap(true);logSummary_->setAccessibleName("Log folder size, file count and oldest record");Ui::role(logSummary_,"secondary");
 clearLogs_=new QPushButton("Clear logs…");clearLogs_->setObjectName("clearLogs");auto* logsRow=PreferenceRows::action(diagnosticsForm,"Logs",{},clearLogs_);auto* logCaptions=qobject_cast<QBoxLayout*>(logsRow->layout()->itemAt(0)->layout()->itemAt(0)->layout());logCaptions->addWidget(logSummary_);
 loggingWarning_=new QLabel;loggingWarning_->setObjectName("loggingDegraded");loggingWarning_->setWordWrap(true);Ui::role(loggingWarning_,"warning");loggingWarning_->hide();diagnosticsForm->addRow(loggingWarning_);
 uncleanExit_=new QLabel;uncleanExit_->setObjectName("uncleanExit");uncleanExit_->setWordWrap(true);uncleanExit_->setAccessibleName("Last unclean exit");Ui::role(uncleanExit_,"secondary");auto* exitRow=PreferenceRows::action(diagnosticsForm,"Last unclean exit",{},uncleanExit_);auto* exitLine=qobject_cast<QBoxLayout*>(exitRow->layout()->itemAt(0)->layout());exitLine->setAlignment(uncleanExit_,Qt::AlignRight|Qt::AlignBaseline);exitLine->setAlignment(exitLine->itemAt(0)->layout(),Qt::AlignBaseline);uncleanExit_->setAlignment(Qt::AlignRight);
 for(auto* value:{logSummary_,uncleanExit_}){const auto help=settingHelp({},value->objectName());value->setToolTip(help);value->setAccessibleDescription(help);value->setProperty("settingDescription",help);}
 auto* createPackage=new QPushButton("Create…");createPackage->setObjectName("createDiagnosticsPackage");createPackage->setAccessibleName("Create diagnostics package");createPackage->setToolTip("Preview and save a redacted .zip of recent failures, settings and logs. Nothing is uploaded.");Ui::role(createPackage,"secondary");PreferenceRows::action(diagnosticsForm,"Diagnostics package","A redacted .zip you can send to a maintainer.",createPackage);
 connect(clearLogs_,&QPushButton::clicked,this,[this]{if(!Ui::confirm(this,"Clear logs","Delete every put.gd log file? Uploads, history, settings and saved diagnostics packages stay.","Clear logs",true))return;clearLogs({});});
 connect(createPackage,&QPushButton::clicked,this,[this]{openDiagnosticsPackage();});

 auto* supportToggle=new QToolButton;supportToggle->setObjectName("desktopSupportDetailsToggle");supportToggle->setText("Desktop support details");Ui::disclosure(supportToggle);supportToggle->setProperty("uiFlush",true);diagnosticsForm->addRow(supportToggle);
 capabilities_=new FormRow;capabilities_->setObjectName("desktopSupportDetails");capabilities_->setAccessibleName("Desktop support diagnostics");auto* supportGrid=new QGridLayout(capabilities_);supportGrid->setContentsMargins(space(Space::l),space(Space::m),space(Space::l),space(Space::m));supportGrid->setHorizontalSpacing(0);supportGrid->setVerticalSpacing(space(Space::s));supportGrid->setColumnStretch(1,1);int supportRow=0;
 const auto supportEntry=[&](QLabel* key,QLabel* value){for(auto* label:{key,value}){label->setMinimumHeight(Ui::lineHeight(Ui::Type::body));label->setAlignment(Qt::AlignLeft|Qt::AlignBottom);}supportGrid->addWidget(key,supportRow,0,Qt::AlignLeft|Qt::AlignBottom);supportGrid->addWidget(value,supportRow++,1,Qt::AlignLeft|Qt::AlignBottom);};
 for(const auto& entry:QList<QPair<QString,QString>>{{"region","Region capture"},{"shortcuts","Hotkeys"},{"tray","Tray"}}){auto* label=Ui::label(entry.second,"technical");Ui::role(label,"secondary");label->setWordWrap(true);label->setProperty("desktopSupportKey",true);auto* value=Ui::label("Not reported","body");value->setObjectName("desktopSupport"+title(entry.first));value->setWordWrap(true);value->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);supportEntry(label,value);}
 // A capture kind the desktop can't provide gets its own row with the adapter's reason (#1295); hidden while available.
 for(const auto& entry:QList<QPair<QString,QString>>{{"window","Window capture"},{"desktop","Desktop capture"},{"monitor","Monitor capture"},{"delay","Delayed capture"}}){auto* label=Ui::label(entry.second,"technical");Ui::role(label,"secondary");label->setWordWrap(true);label->setProperty("desktopSupportKey",true);label->setObjectName("desktopSupport"+title(entry.first)+"GapLabel");auto* value=Ui::label({},"body");value->setObjectName("desktopSupport"+title(entry.first)+"Gap");value->setProperty("desktopSupportWraps",true);value->setWordWrap(true);value->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);supportEntry(label,value);supportGrid->setAlignment(label,Qt::AlignLeft|Qt::AlignTop);/* The reason takes the column's width: left-aligned it wrapped at its own narrow hint. */supportGrid->setAlignment(value,Qt::AlignTop);label->hide();value->hide();}
 // #1364: whether recordings can highlight clicks here, by which route, or why not.
 {auto* label=Ui::label("Click highlights","technical");Ui::role(label,"secondary");label->setWordWrap(true);label->setProperty("desktopSupportKey",true);label->setObjectName("desktopSupportClickHighlightsLabel");auto* value=Ui::label("Not reported","body");value->setObjectName("desktopSupportClickHighlights");value->setWordWrap(true);value->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);supportEntry(label,value);supportGrid->setAlignment(label,Qt::AlignLeft|Qt::AlignTop);
  // The route and a target the desktop leaves out make several lines: the value takes its whole column
  // (a cell with an alignment gets the label's own narrow size hint) and the sizer below its wrapped height.
  supportGrid->setAlignment(value,Qt::Alignment());value->setProperty("desktopSupportWraps",true);}
 // Retain the richer integration readouts for existing agent consumers.
 for(const auto& key:QStringList{"integration","window","desktop"}){auto* value=new QLabel(capabilities_);value->setObjectName("desktopSupport"+title(key));value->hide();}
 configPath_=new Ui::ElidedLabel;configPath_->setObjectName("configPath");configPath_->setTextFormat(Qt::PlainText);configPath_->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);Ui::role(configPath_,"path");static_cast<Ui::ElidedLabel*>(configPath_)->setElideMode(Qt::ElideMiddle);auto* configLabel=Ui::label("Configuration file","secondary");configLabel->setObjectName("configPathLabel");configLabel->setBuddy(configPath_);configLabel->setWordWrap(true);configLabel->setProperty("desktopSupportKey",true);configLabel->setContentsMargins(0,space(Space::xs),0,0);configPath_->setContentsMargins(0,space(Space::xs),0,0);supportEntry(configLabel,configPath_);
 auto* configActions=new QHBoxLayout;configActions->setContentsMargins(0,0,0,0);configActions->setSpacing(0);auto* revealConfig=new QPushButton("Show in folder");revealConfig->setObjectName("showConfigInFolder");auto* editor=new QPushButton("Open in editor");editor->setObjectName("openConfigEditor");auto* reload=new QPushButton("Reload from file…");reload->setObjectName("reloadSettings");reload->setToolTip("Replace this window's settings with the configuration file as it is now.");auto* copyPath=new QPushButton("Copy path");copyPath->setObjectName("copyConfigPath");copyPath->hide();configActions->addWidget(revealConfig);auto* fileDot1=Ui::label(" · ","secondary");configActions->addWidget(fileDot1);configActions->addWidget(editor);auto* fileDot2=Ui::label(" · ","secondary");configActions->addWidget(fileDot2);configActions->addWidget(reload);configActions->addSpacing(space(Space::s));configActions->addWidget(copyPath);configActions->addStretch();auto* fileActions=new QWidget;fileActions->setLayout(configActions);supportGrid->addWidget(fileActions,supportRow++,1);class FileActionsSizer final:public QObject{public:FileActionsSizer(QWidget* row,QBoxLayout* line,QLabel* a,QLabel* b):QObject(row),row_(row),line_(line),a_(a),b_(b){row->installEventFilter(this);}protected:bool eventFilter(QObject*,QEvent* event)override{if(event->type()==QEvent::Resize||event->type()==QEvent::Show||event->type()==QEvent::LayoutRequest){int needed=0;for(int i=0;i<line_->count();++i)if(auto* widget=line_->itemAt(i)->widget();widget&&(!widget->isHidden()||widget==a_||widget==b_))needed+=widget->sizeHint().width();const bool stack=row_->width()<needed;line_->setDirection(stack?QBoxLayout::TopToBottom:QBoxLayout::LeftToRight);a_->setVisible(!stack);b_->setVisible(!stack);}return false;}private:QWidget* row_;QBoxLayout* line_;QLabel* a_;QLabel* b_;};new FileActionsSizer(fileActions,configActions,fileDot1,fileDot2);
 connect(revealConfig,&QPushButton::clicked,this,[this]{diagnosticsCommand({{"action","showConfigFolder"}},[g=QPointer<SettingsWindow>(this)](Error error,QJsonObject){if(g&&error)g->status(error);});});
 for(const auto& entry:QList<QPair<QString,QString>>{{"accepted","Accepted revision"},{"observed","Observed revision"}}){auto* label=Ui::label(entry.second,"secondary");label->setObjectName(entry.first+"RevisionLabel");label->setWordWrap(true);label->setProperty("desktopSupportKey",true);auto* value=new MetadataLabel;value->setObjectName(entry.first+"Revision");value->setWordWrap(true);value->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);Ui::role(value,"path");value->setProperty("feedbackTone","warning");Ui::role(value,"path");supportEntry(label,value);}
 metadata_=new MetadataLabel(capabilities_);metadata_->setObjectName("settingsMetadata");metadata_->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);metadata_->hide();
 auto* editorFailure=new QLabel("Couldn't open the config file in an editor. Copy its path and open it manually.");editorFailure->setObjectName("configEditorFailure");editorFailure->setWordWrap(true);Ui::role(editorFailure,"error");supportGrid->addWidget(editorFailure,supportRow++,1);editorFailure->hide();connect(copyPath,&QPushButton::clicked,this,[this]{qApp->clipboard()->setText(settings_.path());});connect(editor,&QPushButton::clicked,this,[this,editorFailure,copyPath]{host::openFile(settings_.path(),[failure=QPointer<QLabel>(editorFailure),copy=QPointer<QPushButton>(copyPath)](const QJsonObject& result){if(failure)failure->setVisible(!result["opened"].toBool());if(copy)copy->setVisible(!result["opened"].toBool());});});connect(reload,&QPushButton::clicked,this,&SettingsWindow::reloadFromFile);
 auto* closing=new QWidget;auto* closingBox=new QVBoxLayout(closing);closingBox->setContentsMargins(0,space(Space::xs),0,0);closingBox->setSpacing(space(Space::m));auto* rule=new Ui::Surface("surface",Qt::TopEdge);rule->setFixedHeight(1);closingBox->addWidget(rule);supportGrid->addWidget(closing,supportRow++,0,1,2);
 auto* diagnose=new QPushButton("Refresh desktop support");diagnose->setObjectName("refreshDesktopSupport");auto* copyDiagnostics=new QPushButton("Copy details");copyDiagnostics->setObjectName("copyDesktopDetails");auto* diagnosticActions=new QHBoxLayout;diagnosticActions->setContentsMargins(0,0,0,0);diagnosticActions->setSpacing(space(Space::s));diagnosticActions->addWidget(diagnose);diagnosticActions->addStretch();diagnosticActions->addWidget(copyDiagnostics);for(auto* button:{revealConfig,editor,reload,copyPath,diagnose,copyDiagnostics}){Ui::role(button,"textAction");button->setProperty("uiFlush",true);button->setProperty("uiDensity","compact");}closingBox->addLayout(diagnosticActions);connect(diagnose,&QPushButton::clicked,this,[this]{settings_.refreshSnapshot();});connect(copyDiagnostics,&QPushButton::clicked,this,[this]{qApp->clipboard()->setText(QString::fromUtf8(QJsonDocument(settings_.diagnostics()).toJson(QJsonDocument::Indented))+"\n"+metadataText_);});
 class SupportKeysSizer final:public QObject{public:SupportKeysSizer(QWidget* panel,QGridLayout* grid):QObject(panel),panel_(panel),grid_(grid){panel->installEventFilter(this);for(auto* label:panel->findChildren<QLabel*>())if(label->property("desktopSupportWraps").toBool())label->installEventFilter(this);}protected:bool eventFilter(QObject*,QEvent* event)override{if(event->type()==QEvent::Resize||event->type()==QEvent::Show||event->type()==QEvent::LayoutRequest||event->type()==QEvent::FontChange){QList<QLabel*> keys;int widest=0;for(auto* label:panel_->findChildren<QLabel*>())if(label->property("desktopSupportKey").toBool()){keys.append(label);widest=qMax(widest,qCeil(QFontMetricsF(label->font()).horizontalAdvance(label->text()))+1);}const int width=qMin(widest+space(Space::l),qMax(160,int(panel_->width()*0.32)));if(grid_->columnMinimumWidth(0)!=width)grid_->setColumnMinimumWidth(0,width);for(auto* label:keys){if(label->width()!=width-space(Space::l))label->setFixedWidth(width-space(Space::l));label->setMinimumHeight(qMax(Ui::lineHeight(Ui::Type::body),label->heightForWidth(label->width())));}
  // A capture gap's reason (#1295) and the click highlights' route and limits (#1364) wrap over several
  // lines; each row takes the wrapped height.
  for(auto* label:panel_->findChildren<QLabel*>())if(label->property("desktopSupportWraps").toBool()&&label->wordWrap()&&!label->isHidden()&&label->width()>0){const int height=qMax(Ui::lineHeight(Ui::Type::body),label->heightForWidth(label->width()));if(label->minimumHeight()!=height)label->setMinimumHeight(height);}}return false;}private:QWidget* panel_;QGridLayout* grid_;};new SupportKeysSizer(capabilities_,supportGrid);
 diagnosticsForm->addRow(capabilities_);capabilities_->setProperty("preferenceRowInset",true);capabilities_->hide();connect(supportToggle,&QToolButton::toggled,this,[this](bool open){capabilities_->setVisible(open);emit viewStateChanged();});
 destinationPage_=new QWidget(this);destinationPage_->setObjectName("destinationsPage");destinationPage_->installEventFilter(this);auto* destinationLayout=new QHBoxLayout(destinationPage_);destinationLayout->setContentsMargins(0,0,0,0);destinationLayout->setSpacing(0);
 auto* destinationRail=new Ui::Surface("surface",Qt::RightEdge);destinationRail->setObjectName("destinationRail");destinationRail_=destinationRail;auto* railLayout=new QVBoxLayout(destinationRail);railLayout->setContentsMargins(space(Space::l),space(Space::l),space(Space::l),space(Space::l));railLayout->setSpacing(space(Space::m));auto* destinationList=new DestinationList;destinationList_=destinationList;destinationList_->setObjectName("destinationList");destinationList_->setAccessibleName("Destinations");destinationList_->setIconSize(QSize(Ui::scaled(Ui::Metric::icon),Ui::scaled(Ui::Metric::icon)));destinationList_->setWordWrap(false);destinationList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);railLayout->addWidget(destinationList_,1);auto* add=new QPushButton(Ui::icon("add"),"Add destination");addDestination_=add;Ui::role(add,"quiet");add->setProperty("alignLeft",true);add->setProperty("uiNavRow",true);add->setProperty("uiFlush",true);add->setProperty("noChevron",true);add->setProperty("uiIconBase",QSize(16,16));add->setIconSize(QSize(Ui::scaled(16),Ui::scaled(16)));add->setMinimumHeight(Ui::scaled(36));add->setAccessibleName("Add destination");add->setObjectName("addS3Destination");railLayout->addWidget(add);destinationLayout->addWidget(destinationRail);destinationList_->setFrameShape(QFrame::NoFrame);destinationList_->setMouseTracking(true);Ui::transparentView(destinationList_);
 auto* destinationContent=new QWidget;destinationContent->setObjectName("destinationContent");auto* destinationContentLayout=new QVBoxLayout(destinationContent);destinationContentLayout->setContentsMargins(0,0,0,0);destinationContentLayout->setSpacing(0);destinationLayout->addWidget(destinationContent,1);
 auto* destinationBody=new Ui::Surface("preferencePage");auto* pageLayout=new QVBoxLayout(destinationBody);pageLayout->setContentsMargins(space(Space::xxl),qMin(Ui::pageHeaderTop(),space(Space::l)),space(Space::xxl),space(Space::xl));pageLayout->setSpacing(space(Space::xxl));pageLayout->setAlignment(Qt::AlignTop);auto* destinationScroll=new QScrollArea;destinationScroll->setObjectName("destinationScroll");destinationScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);destinationScroll->setWidgetResizable(true);destinationScroll->setFrameShape(QFrame::NoFrame);destinationScroll->setWidget(destinationBody);destinationContentLayout->addWidget(destinationScroll,1);
 auto* emptyDestinations=new QLabel("No destinations yet\nAdd a destination to start uploading.");emptyDestinations->setObjectName("destinationsEmpty");emptyDestinations->setWordWrap(true);emptyDestinations->setAlignment(Qt::AlignCenter);Ui::role(emptyDestinations,"muted");destinationLayout->addWidget(emptyDestinations,1);
 profile_=choices(this,"destinationProfile",{});profile_->hide();profileId_=input(this,"destinationId");profileId_->hide();providerJson_=new QPlainTextEdit(this);providerJson_->setObjectName("providerSettingsJson");providerJson_->hide();
 auto* destinationHeader=new QWidget;destinationHeader->setObjectName("destinationHeader");destinationHeader->setMaximumWidth(Ui::scaled(760));auto* headerLayout=new QGridLayout(destinationHeader);headerLayout->setContentsMargins(0,0,0,0);headerLayout->setHorizontalSpacing(space(Space::m));headerLayout->setVerticalSpacing(0);headerLayout->setColumnStretch(0,1);headerLayout->setRowMinimumHeight(0,Ui::scaled(Ui::Metric::control));pageLayout->addWidget(destinationHeader);auto* titleLine=new QHBoxLayout;titleLine->setContentsMargins(0,0,0,0);titleLine->setSpacing(space(Space::s));headerLayout->addLayout(titleLine,0,0);
 auto* titleActions=new QHBoxLayout;titleActions->setSpacing(space(Space::s));
 profileLabel_=new DestinationTitle(this);profileLabel_->setFixedHeight(Ui::scaled(Ui::Metric::control));profileLabel_->setObjectName("destinationLabel");profileLabel_->setProperty("visualRole","editableTitle");Ui::showTextStartWhenUnfocused(profileLabel_);profileLabel_->setPlaceholderText("Destination name");profileLabel_->setAccessibleName("Destination name");bind(profileLabel_,"/profiles/{profile}/label");titleLine->addWidget(profileLabel_,0,Qt::AlignVCenter);profileLabel_->setAccessibleDescription("Rename this destination without changing existing uploads.");profileLabel_->setProperty("settingDescription",profileLabel_->accessibleDescription());auto* providerState=PreferenceRows::caption({});providerState->setObjectName("destinationProviderState");providerState->setProperty("uiStatus",false);Ui::role(providerState,"secondary");providerState->setWordWrap(false);providerState->setSizePolicy(QSizePolicy::Minimum,QSizePolicy::Preferred);titleLine->addWidget(providerState,0,Qt::AlignVCenter);auto* addCredentials=new QPushButton("Add credentials…");addCredentials->setObjectName("addDestinationCredentials");Ui::role(addCredentials,"textAction");addCredentials->setProperty("uiFlush",true);addCredentials->hide();titleLine->addWidget(addCredentials,0,Qt::AlignVCenter);titleLine->addStretch(1);connect(addCredentials,&QPushButton::clicked,this,[this,destinationScroll]{QTimer::singleShot(0,this,[this,destinationScroll]{QWidget* field=access_->text().isEmpty()?access_:secret_;
   // A custom uploader has no key pair: go to its first secret value that still needs one, or to Store when all are typed.
   if(providerType_->currentData()==CustomUploaderPanel::provider()){field=uploader_?uploader_->credentialField():nullptr;if(!field)field=findControl<QPushButton*>("storeCredentials");}
   destinationScroll->widget()->layout()->activate();field->setFocus(Qt::TabFocusReason);destinationScroll->ensureWidgetVisible(field,space(Space::xl),space(Space::xl));const auto centre=field->mapTo(destinationScroll->widget(),field->rect().center());destinationScroll->ensureVisible(centre.x(),centre.y(),field->width()/2+space(Space::xl),field->height()/2+space(Space::xl));});});
 auto* testScope=new QLabel;testScope->setObjectName("destinationTestScope");testScope->setWordWrap(true);testScope->setTextFormat(Qt::PlainText);testScope->setProperty("uiStatus",true);Ui::role(testScope,"secondary");auto* test=new QPushButton("Test upload and link");test->setObjectName("testDestination");test->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Fixed);defaultButton_=new QPushButton("Make default");defaultButton_->setObjectName("makeDefaultDestination");titleActions->addWidget(defaultButton_);auto* more=new QPushButton;more->setIcon(Ui::chromeIcon("more"));more->setToolTip("Destination actions");more->setProperty("noChevron",true);more->setAccessibleName("Destination actions");more->setObjectName("destinationActions");more->setFixedSize(Ui::scaled(36),Ui::scaled(36));titleActions->addWidget(more);
 headerLayout->addLayout(titleActions,0,1,Qt::AlignRight|Qt::AlignVCenter);
 auto* description=new QLabel("Amazon S3 or compatible storage. Existing uploads keep their original destination.");description->setWordWrap(true);description->setObjectName("destinationProviderDescription");Ui::role(description,"bodySecondary");description->setContentsMargins(0,space(Space::s),0,0);headerLayout->addWidget(description,1,0,1,2);
 auto* destinationMenu=new QMenu(more);more->setMenu(destinationMenu);auto* rename=destinationMenu->addAction("Rename destination");rename->setObjectName("renameDestination");connect(rename,&QAction::triggered,this,[this]{QTimer::singleShot(0,this,[this]{profileLabel_->setFocus(Qt::ShortcutFocusReason);profileLabel_->selectAll();});});destinationMenu->addSeparator();auto* serviceMenu=destinationMenu->addMenu("Storage service");auto* services=new QActionGroup(serviceMenu);services->setObjectName("storageServices");services->setExclusive(true);for(const auto& service:QList<QPair<QString,QString>>{{"Amazon S3 / compatible","s3"},{"Proton Drive","proton-drive"},{"Backblaze B2","backblaze-b2"},{"Cloudflare R2",r2::providerId},{"Custom uploader (HTTP)",CustomUploaderPanel::provider()}}){auto* action=serviceMenu->addAction(service.first);action->setCheckable(true);action->setData(service.second);services->addAction(action);connect(action,&QAction::triggered,this,[this,action]{providerType_->setCurrentIndex(providerType_->findData(action->data()));});}auto* remove=new QPushButton("Remove destination",this);remove->setObjectName("removeDestination");remove->hide();connect(destinationMenu->addAction("Remove destination…"),&QAction::triggered,remove,&QPushButton::click);destinationMenu->addSeparator();moveUp_=destinationMenu->addAction("Move up");moveDown_=destinationMenu->addAction("Move down");
 for(auto* move:{moveUp_,moveDown_}){move->setObjectName(move==moveUp_?"moveDestinationUp":"moveDestinationDown");move->setShortcut(QKeySequence(Qt::ALT|(move==moveUp_?Qt::Key_Up:Qt::Key_Down)));move->setShortcutContext(Qt::WidgetShortcut);destinationList_->addAction(move);connect(move,&QAction::triggered,this,[this,move]{QJsonObject result;if(auto error=moveSelectedDestination(move==moveUp_?-1:1,{},result))status(error);});}
 destinationList->moveRequested=[this](int from,int to){if(auto error=moveDestination(from,to))status(error);};
 destinationMenu->addSeparator();connect(destinationMenu->addAction("Copy details"),&QAction::triggered,this,[this]{qApp->clipboard()->setText(QString::fromUtf8(QJsonDocument(editedProfile()).toJson(QJsonDocument::Indented)));});
 auto* dev=new QPushButton("Import development S3",this);dev->setObjectName("importDevelopmentS3");dev->hide();auto* addMenu=new QMenu(add);add->setMenu(addMenu);connect(addMenu->addAction("Amazon S3 / S3-compatible"),&QAction::triggered,this,[this]{addS3Profile();});connect(addMenu->addAction("Proton Drive"),&QAction::triggered,this,[this]{const auto count=profile_->count();addS3Profile("Proton Drive");if(profile_->count()==count)return;providerType_->setCurrentIndex(providerType_->findData("proton-drive"));queueEdit(profileLabel_);});connect(addMenu->addAction("Backblaze B2"),&QAction::triggered,this,[this]{const auto count=profile_->count();addS3Profile("Backblaze B2");if(profile_->count()==count)return;providerType_->setCurrentIndex(providerType_->findData("backblaze-b2"));queueEdit(profileLabel_);});
 // #1678: Cloudflare R2, its own destination type over the S3 API.
 {auto* r2Add=addMenu->addAction("Cloudflare R2");r2Add->setObjectName("addCloudflareR2");connect(r2Add,&QAction::triggered,this,[this]{const auto count=profile_->count();addS3Profile("Cloudflare R2",{},r2::providerId);if(profile_->count()==count)return;providerType_->setCurrentIndex(providerType_->findData(r2::providerId));queueEdit(profileLabel_);});}
 // #1361: one HTTP request the user describes, or imports from a ShareX .sxcu file.
 {auto* custom=addMenu->addAction("Custom uploader (HTTP)");custom->setObjectName("addCustomUploader");connect(custom,&QAction::triggered,this,[this]{const auto count=profile_->count();addS3Profile("Custom uploader");if(profile_->count()==count)return;providerType_->setCurrentIndex(providerType_->findData(CustomUploaderPanel::provider()));queueEdit(profileLabel_);});}
#ifndef NDEBUG
 connect(addMenu->addAction("Import development S3"),&QAction::triggered,dev,&QPushButton::click);
#endif
 auto* destinationDetails=PreferenceRows::card(pageLayout,"Where files go");destinationDetails->parentWidget()->setObjectName("destinationEditor");
 providerType_=choices(this,"storageProvider",{});providerType_->addItem("Amazon S3","s3");providerType_->addItem("Proton Drive","proton-drive");providerType_->addItem("Backblaze B2","backblaze-b2");providerType_->addItem("Cloudflare R2",r2::providerId);providerType_->addItem("Custom uploader (HTTP)",CustomUploaderPanel::provider());connect(providerType_,&QComboBox::currentIndexChanged,this,[this,services]{for(auto* action:services->actions())action->setChecked(action->data()==providerType_->currentData());});setting(destinationDetails,"Storage service",providerType_,"/profiles/{profile}");
 auto* s3Service=choices(this,"s3Service",{"Amazon S3","Other S3-compatible"});s3Service->setAccessibleName("S3-compatible service");auto* s3ServiceRow=setting(destinationDetails,"S3 service",s3Service,{});s3Service->setAccessibleName("S3-compatible service");
 // #1678: Cloudflare R2 names its account and jurisdiction; put.gd builds the endpoint from them.
 r2AccountId_=input(this,"r2AccountId");r2AccountId_->setPlaceholderText("32 characters, such as 0123456789abcdef0123456789abcdef");
 auto* r2AccountRow=setting(destinationDetails,"Account ID",r2AccountId_,"/profiles/{profile}/settings/accountId","R2 overview → Account details. Pasting the bucket's S3 API address here also fills Jurisdiction and Bucket.");r2AccountRow->setProperty("preferenceInline",true);
 r2Jurisdiction_=choices(this,"r2Jurisdiction",{});r2Jurisdiction_->addItem("Default","default");r2Jurisdiction_->addItem("EU","eu");r2Jurisdiction_->addItem("FedRAMP","fedramp");
 auto* r2JurisdictionRow=setting(destinationDetails,"Jurisdiction",r2Jurisdiction_,"/profiles/{profile}/settings/jurisdiction");
 // The dashboard's S3 API address fills Account ID, Jurisdiction and Bucket at once, read from the
 // raw text and only when every part is valid: on a paste (the queue's TypingWatch marks it) or when
 // edited text is finished, never part-way through typing it. Connected before the queue's own
 // handlers, so the edit they then queue is the field group with all three filled in (#1704).
 const auto readS3ApiAddress=[this]{
  if(loading_||providerType_->currentData()!=r2::providerId)return;
  const auto address=r2::parseS3ApiAddress(r2AccountId_->text());if(!address)return;
  // An address with no bucket, for another account or jurisdiction than the one the bucket shown came
  // with, leaves Bucket empty: saving that bucket with this account would be wrong.
  const auto from=address->accountId+"|"+address->jurisdiction;
  const auto bucketFrom=r2BucketFrom_.value(profileId_->text());
  const bool otherAccount=!bucketFrom.section('|',0,0).isEmpty()&&bucketFrom!=from;
  const QSignalBlocker account(r2AccountId_),jurisdiction(r2Jurisdiction_),bucket(s3_["bucket"]);
  r2AccountId_->setText(address->accountId);r2Jurisdiction_->setCurrentIndex(r2Jurisdiction_->findData(address->jurisdiction));if(!address->bucket.isEmpty())s3_["bucket"]->setText(address->bucket);else if(otherAccount)s3_["bucket"]->clear();
  if(!address->bucket.isEmpty()||otherAccount)r2BucketFrom_[profileId_->text()]=from;
 };
 connect(r2AccountId_,&QLineEdit::textChanged,this,[this,readS3ApiAddress]{if(pasting_==r2AccountId_)readS3ApiAddress();});
 connect(r2AccountId_,&QLineEdit::editingFinished,this,[this,readS3ApiAddress]{if(r2AccountId_->isModified())readS3ApiAddress();});
 // An S3 destination on Cloudflare R2 offers to become a Cloudflare R2 destination: same name, ID,
 // routes and credentials. Only this click converts it.
 auto* moveToR2=new QPushButton("Move to Cloudflare R2",this);moveToR2->setObjectName("moveToCloudflareR2");moveToR2->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Fixed);
 auto* moveToR2Row=setting(destinationDetails,"Cloudflare R2",moveToR2,{},"This destination uses Cloudflare R2. As a Cloudflare R2 destination it keeps its name, routes and credentials, and asks for the Account ID instead of Region and Endpoint. Uploads already in History keep their recorded destination.");
 moveToR2->setAccessibleName(moveToR2->text());moveToR2Row->setObjectName("moveToCloudflareR2Row");moveToR2Row->setProperty("preferenceInline",true);
 // Left-aligned in its row, as the other row actions are.
 for(auto* line:moveToR2Row->findChildren<QHBoxLayout*>())if(line->indexOf(moveToR2)>=0)line->setAlignment(moveToR2,Qt::AlignLeft|Qt::AlignVCenter);
 connect(moveToR2,&QPushButton::clicked,this,&SettingsWindow::moveToCloudflareR2);
 const QHash<QString,QString> s3Labels{{"bucket","Bucket"},{"region","Region"},{"endpoint","Endpoint"},{"objectPrefix","Folder in bucket"},{"publicUrlBase","Link domain"},{"storageClass","Storage class"},{"connectTimeoutMs","Connect timeout"},{"requestTimeoutMs","Request timeout"},{"lowSpeedBytesPerSecond","Minimum transfer speed"},{"lowSpeedTimeSeconds","Low-speed window"}};
 for(const auto& key:QStringList{"bucket","region","endpoint","objectPrefix","publicUrlBase","storageClass","connectTimeoutMs","requestTimeoutMs","lowSpeedBytesPerSecond","lowSpeedTimeSeconds"})s3_[key]=input(this,"s3"+title(key));
 auto* bucketHint=setting(destinationDetails,"Bucket",s3_["bucket"],"/profiles/{profile}/settings/bucket","Required before uploads can go here.");bucketHint->setProperty("preferenceInline",true);auto* bucketText=bucketHint->findChild<QLabel*>("s3BucketHint");bucketText->setTextFormat(Qt::PlainText);// Cloudflare R2 has no URI scheme of its own, so its place is said in words rather than as s3://.
 auto updateBucketHint=[this,bucketText]{const auto place=s3_["bucket"]->text()+"/"+s3_["objectPrefix"]->text();static_cast<Ui::ElidedLabel*>(bucketText)->setText(s3_["bucket"]->text().isEmpty()?QString("Required before uploads can go here."):providerType_->currentData()==r2::providerId?"Uploads go to "+place+" on Cloudflare R2":"Uploads go to "+QString(providerType_->currentData()=="backblaze-b2"?"b2://":"s3://")+place);};connect(providerType_,&QComboBox::currentIndexChanged,this,updateBucketHint);connect(s3_["bucket"],&QLineEdit::textChanged,this,updateBucketHint);connect(s3_["bucket"],&QLineEdit::textChanged,this,[this]{if(!loading_&&providerType_->currentData()==r2::providerId)r2BucketFrom_[profileId_->text()]=r2AccountId_->text()+"|"+r2Jurisdiction_->currentData().toString();});connect(s3_["objectPrefix"],&QLineEdit::textChanged,this,updateBucketHint);updateBucketHint();s3_["bucket"]->setPlaceholderText("my-screenshots");
 // A Cloudflare R2 destination saves as one profile once every field is valid (#1699): this line says
 // when what the form shows isn't saved and won't be as it is, and why.
 r2UnitStatus_=new QLabel;r2UnitStatus_->setObjectName("r2UnitStatus");r2UnitStatus_->setWordWrap(true);Ui::role(r2UnitStatus_,"warning");destinationDetails->addRow(QString(),r2UnitStatus_);r2UnitStatus_->hide();
 regionChoices_=choices(this,"s3RegionChoices",s3Regions());regionChoices_->setEditable(true);regionChoices_->setInsertPolicy(QComboBox::NoInsert);regionChoices_->setLineEdit(s3_["region"]);setting(destinationDetails,"Region",regionChoices_,"/profiles/{profile}/settings/region");bind(s3_["region"],"/profiles/{profile}/settings/region");
 errors_[s3_["region"]]=errors_[regionChoices_];
 setting(destinationDetails,"Folder in bucket",s3_["objectPrefix"],"/profiles/{profile}/settings/objectPrefix","Optional. Existing uploads stay where they are.")->setProperty("preferenceInline",true);
 s3_["objectPrefix"]->setPlaceholderText("Bucket root");
 auto* links=PreferenceRows::card(pageLayout,"Links");linksPanel_=links->parentWidget();// What the upload asks S3 for, not a promise about who can open the link.
 acl_=choices(this,"s3AclMode",{"omit","public-read"});for(int i=0;i<acl_->count();++i)acl_->setItemData(i,acl_->itemText(i));acl_->setItemText(0,"Use bucket policy");acl_->setItemText(1,"Request public read");auto* aclRow=setting(links,"Upload access request",acl_,"/profiles/{profile}/settings/aclMode","Bucket policy also controls access. Test the resulting link.");auto* aclHint=aclRow->findChild<QLabel*>("s3AclModeHint");b2LinkMode_=choices(this,"b2LinkMode",{});b2LinkMode_->addItem("Public bucket: links that never expire","public");b2LinkMode_->addItem("Private bucket: links that expire","authorized");auto* b2ModeRow=setting(links,"Who can open files",b2LinkMode_,"/profiles/{profile}/settings/linkMode");b2LinkDays_=new QSpinBox;b2LinkDays_->setObjectName("b2LinkDays");b2LinkDays_->setRange(1,b2::maximumLinkDays);b2LinkDays_->setSuffix(" days");auto* b2DaysRow=setting(links,"Links last",b2LinkDays_,"/profiles/{profile}/settings/linkValidityDays","Links expire after 1–7 days; files stay in the bucket. S3 uses the endpoint host, ignoring Link domain. Temporary credentials can expire sooner than the chosen duration.");auto* b2DaysHint=b2DaysRow->findChild<QLabel*>("b2LinkDaysHint");connect(b2LinkMode_,&QComboBox::activated,this,[this]{queueEdit(b2LinkMode_);queueEdit(b2LinkDays_);if(providerType_->currentData()=="backblaze-b2")queueEdit(s3_["publicUrlBase"]);updateProviderVisibility_();});connect(b2LinkDays_,&QSpinBox::editingFinished,this,[this]{queueEdit(b2LinkDays_);});auto* linkDomainRow=setting(links,"Link domain",s3_["publicUrlBase"],"/profiles/{profile}/settings/publicUrlBase");s3_["publicUrlBase"]->setPlaceholderText("https://i.example.com");linkExample_=new Ui::ElidedLabel;linkExample_->setObjectName("linkExample");linkExample_->setWordWrap(false);static_cast<Ui::ElidedLabel*>(linkExample_)->setElideMode(Qt::ElideMiddle);linkExample_->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);linkExample_->setAccessibleName("Example link");Ui::role(linkExample_,"secondary");Ui::setMono(linkExample_,Ui::Type::technical);auto* exampleLine=new QWidget;exampleLine->setObjectName("linkExampleLine");auto* exampleBox=new QHBoxLayout(exampleLine);exampleBox->setContentsMargins(0,0,0,0);exampleBox->setSpacing(space(Space::xs));exampleBox->addWidget(Ui::label("Example:","secondary"));exampleBox->addWidget(linkExample_,1);linkDomainRow->layout()->addWidget(exampleLine);
 // R2's S3 endpoint never serves public reads (#1679), so public links there need the bucket's
 // r2.dev address or a custom domain: an empty Link domain is called out before any test.
 auto* r2LinkWarning=new QLabel("Cloudflare R2 doesn't serve public links from its S3 endpoint. Enter the bucket's r2.dev address or a custom domain, or choose Private bucket: links that expire.");r2LinkWarning->setObjectName("r2LinkDomainWarning");r2LinkWarning->setWordWrap(true);Ui::role(r2LinkWarning,"warning");links->addRow(QString(),r2LinkWarning);
 auto* advanced=new QToolButton;advanced->setObjectName("advancedSettings");advanced->setText("Advanced connection settings");advanced->setAccessibleName("Advanced connection settings");Ui::disclosure(advanced);advanced->setProperty("uiFlush",true);pageLayout->addWidget(advanced);auto* connectionSummary=new QLabel;connectionSummary->setObjectName("connectionSummary");connectionSummary->setWordWrap(true);Ui::role(connectionSummary,"muted");pageLayout->addWidget(connectionSummary);auto* advancedRow=PreferenceRows::disclosure(destinationDetails,"Advanced connection settings",connectionSummary,advanced);advancedRow->setObjectName("advancedConnectionRow");auto* advancedCaption=static_cast<QLabel*>(advancedRow->property("preferenceLabel").value<QObject*>());auto* connectionPanel=new QWidget;connectionPanel->setMaximumWidth(Ui::scaled(760));auto* connectionBox=new QVBoxLayout(connectionPanel);connectionBox->setContentsMargins(0,0,0,0);auto* connection=new QFormLayout;Ui::styleForm(connection);connectionBox->addLayout(connection);pageLayout->addWidget(connectionPanel);connectionPanel_=connectionPanel;
 endpointHint_=new EndpointLabel;endpointHint_->setObjectName("s3EndpointValue");endpointHint_->setWordWrap(true);endpointHint_->setTextInteractionFlags(Qt::TextSelectableByMouse);endpointHint_->setAccessibleName("Endpoint in use");Ui::role(endpointHint_,"technical");auto* endpointCaption=new QLabel("Endpoint");connection->addRow(endpointCaption,endpointHint_);Ui::alignFormValue(endpointCaption,endpointHint_);
 auto* endpointWarning=new QLabel("Backblaze B2's S3 API lacks the conditional writes put.gd needs, so uploads to it fail. Use the Backblaze B2 storage service instead.");endpointWarning->setObjectName("s3EndpointWarning");endpointWarning->setWordWrap(true);Ui::role(endpointWarning,"warning");destinationDetails->addRow(QString(),endpointWarning);auto* customEndpoint=new QCheckBox("Use a custom endpoint");customEndpoint->setObjectName("customEndpoint");auto* customEndpointRow=Ui::settingRow("Use a custom endpoint",settingHelp({},"customEndpoint"),customEndpoint,this);connection->addRow(QString(),customEndpointRow);setting(connection,"Endpoint",s3_["endpoint"],"/profiles/{profile}/settings/endpoint");// Picking a service switches to it (#1699): Amazon S3 sets an AWS region and its standard endpoint and
 // keeps the other service's region and endpoint, which come back when Other S3-compatible is picked.
 connect(s3Service,&QComboBox::activated,this,[=,this](int index){
  auto* region=s3_["region"];auto* endpoint=s3_["endpoint"];
  const bool aws=s3AmazonEndpoint(endpoint->text());if((index==0)==aws)return;
  const auto set=[](QLineEdit* field,const QString& value){const QSignalBlocker block(field);field->setText(value);};
  if(index==0){
   s3OtherService_={region->text(),endpoint->text()};
   const auto awsRegion=s3Regions().contains(region->text())?region->text():QStringLiteral("us-east-1");
   set(region,awsRegion);region->setProperty("previousRegion",awsRegion);set(endpoint,"https://s3."+awsRegion+".amazonaws.com");customEndpoint->setChecked(false);queueEdit(region);
  }else{
   advanced->setChecked(true);customEndpoint->setChecked(true);endpoint->setPlaceholderText("https://your-storage-endpoint");
   if(!s3OtherService_.second.isEmpty()){set(region,s3OtherService_.first);region->setProperty("previousRegion",s3OtherService_.first);set(endpoint,s3OtherService_.second);queueEdit(region);}
   else{set(endpoint,QString());endpoint->setFocus();}
  }
  updateProviderVisibility_();
 });

 QHash<QString,QWidget*> connectionRows;QList<std::function<void()>> connectionDefaults;addressing_=choices(this,"s3Addressing",{"path","virtual","auto"});for(int i=0;i<addressing_->count();++i)addressing_->setItemData(i,addressing_->itemText(i));addressing_->setItemText(0,"Bucket in URL path");addressing_->setItemText(1,"Bucket in hostname");addressing_->setItemText(2,"Automatic");
 for(const auto& key:QStringList{"storageClass","connectTimeoutMs","requestTimeoutMs","lowSpeedBytesPerSecond","lowSpeedTimeSeconds"}){auto* field=s3_[key];QWidget* control=field;const auto suffix=key.endsWith("Ms")||key=="lowSpeedTimeSeconds"?QString("seconds"):key=="lowSpeedBytesPerSecond"?QString("KB/s"):QString{};if(!suffix.isEmpty()){auto* spin=new ConnectionSpin(connectionUnit(key),this);spin->setSuffix(" "+suffix);spin->setObjectName(field->objectName()+"Control");spin->editor()->setObjectName(field->objectName());delete field;field=s3_[key]=spin->editor();bind(field,"/profiles/{profile}/settings/"+key);control=spin;}
  auto* row=setting(connection,s3Labels[key],control,"/profiles/{profile}/settings/"+key);if(control!=field){errors_[field]=errors_[control];errors_[field]->setObjectName(field->objectName()+"Error");field->setAccessibleName(control->accessibleName());field->setAccessibleDescription(control->accessibleDescription());field->setToolTip(control->toolTip());field->setProperty("settingDescription",control->property("settingDescription"));}connectionRows.insert(key,row);const QJsonValue value=s3Profile("default")["settings"].toObject()[key];const auto text=value.isDouble()?ByteSize::exact(value.toInteger(),connectionUnit(key)):value.toString();auto* reset=new QPushButton("Reset");reset->setObjectName(field->objectName()+"Reset");reset->hide();reset->setAccessibleName("Reset "+s3Labels[key]);reset->setToolTip("Default: "+(key=="lowSpeedBytesPerSecond"?QString("Any progress"):text+(suffix.isEmpty()?QString{}:" "+suffix)));auto* line=qobject_cast<QHBoxLayout*>(row->layout()->itemAt(0)->layout());if(control==field){field->setMinimumWidth(0);field->setMaximumWidth(Ui::scaled(320));}for(int i=0;i<line->count();++i)line->setStretch(i,0);line->setStretch(0,control==field?1:0);line->addStretch(control==field?0:1);line->addWidget(reset);auto update=[=]{const bool changed=(control==field?QJsonValue(inputText(field)):connectionValue(key,inputText(field)))!=value;reset->setVisible(changed);if(row->property("changed").toBool()!=changed){row->setProperty("changed",changed);row->style()->unpolish(row);row->style()->polish(row);}};connectionDefaults.append(update);connect(field,&QLineEdit::textChanged,this,update);connect(reset,&QPushButton::clicked,this,[=,this]{setInputText(field,text);queueEdit(field);});}

 auto* addressRow=setting(connection,"Addressing",addressing_,"/profiles/{profile}/settings/addressing");auto* resetAddress=new QPushButton("Reset");resetAddress->setAccessibleName("Reset addressing to automatic");resetAddress->setToolTip("Default: Automatic");resetAddress->hide();auto* addressLine=qobject_cast<QHBoxLayout*>(addressRow->layout()->itemAt(0)->layout());for(int i=0;i<addressLine->count();++i)addressLine->setStretch(i,0);addressLine->addStretch(1);addressLine->addWidget(resetAddress);connect(addressing_,&QComboBox::currentIndexChanged,this,[=,this]{const bool changed=addressing_->currentData()!="auto";resetAddress->setVisible(changed);addressRow->setProperty("changed",changed);addressRow->style()->unpolish(addressRow);addressRow->style()->polish(addressRow);});connect(resetAddress,&QPushButton::clicked,this,[this]{addressing_->setCurrentIndex(addressing_->findData("auto"));});reference_=input(this,"credentialReference");setting(connection,"Secret-store reference",reference_,"/profiles/{profile}/credentials/reference");
 auto* credentialForm=PreferenceRows::card(pageLayout,"Credentials","Credentials never enter settings or history. Typed replacements are cleared after use and when this window closes or reloads.");credentialForm->setProperty("separateFieldHints",false);credentialForm->setVerticalSpacing(space(Space::xs));auto* credentialsEditor=credentialForm->parentWidget();credentialsEditor->setObjectName("credentialsEditor");
 // One status row says which key is stored and where; the fields below only take a replacement.
 credentialHint_=new QLabel;credentialHint_->setObjectName("storedCredentialHint");credentialHint_->setWordWrap(true);credentialHint_->setAccessibleName("Stored credential identification");auto* currentKeyRow=new FormRow;auto* currentKeyBox=new QVBoxLayout(currentKeyRow);currentKeyBox->setContentsMargins(0,0,0,0);auto* currentKeyLine=new QHBoxLayout;currentKeyLine->setSpacing(space(Space::l));auto* currentKeyCaption=Ui::label("Current key","body");currentKeyCaption->setBuddy(credentialHint_);currentKeyLine->addWidget(currentKeyCaption,0,Qt::AlignBaseline);currentKeyLine->addWidget(credentialHint_,1,Qt::AlignBaseline);credentialHint_->setAlignment(Qt::AlignRight|Qt::AlignVCenter);Ui::role(credentialHint_,"bodySecondary");currentKeyBox->addLayout(currentKeyLine);credentialHint_->setProperty("preferenceRowContainer",QVariant::fromValue(static_cast<QObject*>(currentKeyRow)));credentialForm->addRow(currentKeyRow);credentialForm_=credentialForm;credentialForm->setRowVisible(PreferenceRows::rowFor(credentialHint_),false);
 access_=input(this,"credentialAccessKey");secret_=input(this,"credentialSecretKey");token_=input(this,"credentialSessionToken");secret_->setEchoMode(QLineEdit::Password);token_->setEchoMode(QLineEdit::Password);auto* accessRow=setting(credentialForm,"Access key ID",access_,{});auto* secretRow=setting(credentialForm,"Secret access key",secret_,{});showSecret_=new QPushButton("Show",this);showSecret_->setObjectName("showCredentialSecret");showSecret_->setCheckable(true);showSecret_->hide();auto* reveal=secret_->addAction(Ui::icon("eye"),QLineEdit::TrailingPosition);reveal->setObjectName("showCredentialSecretAction");reveal->setText("Show secret access key");reveal->setToolTip(reveal->text());reveal->setCheckable(true);connect(reveal,&QAction::toggled,showSecret_,&QPushButton::setChecked);connect(showSecret_,&QPushButton::toggled,reveal,&QAction::setChecked);connect(showSecret_,&QPushButton::toggled,this,&SettingsWindow::toggleCredentialSecret);connect(showSecret_,&QPushButton::toggled,reveal,[this,reveal](bool){reveal->setText((showSecret_->isChecked()?QString("Hide "):QString("Show "))+secret_->accessibleName().toLower());reveal->setToolTip(reveal->text());});token_->setPlaceholderText("Paste a session token (optional)");auto* tokenRow=setting(credentialForm,"Session token",token_,{});credentialMode_=choices(this,"credentialMode",{"keychain","session"});for(int i=0;i<credentialMode_->count();++i)credentialMode_->setItemData(i,credentialMode_->itemText(i));credentialMode_->setItemText(0,"System keychain");credentialMode_->setItemText(1,"This session only");auto* storageRow=setting(credentialForm,"Credential storage",credentialMode_,"/profiles/{profile}/credentials");credentialStorageNotice_=new QLabel("Existing credentials aren't copied to the new storage location.");credentialStorageNotice_->setObjectName("credentialStorageNotice");credentialStorageNotice_->setWordWrap(true);Ui::role(credentialStorageNotice_,"muted");storageRow->layout()->addWidget(credentialStorageNotice_);credentialStorageNotice_->hide();connect(credentialMode_,&QComboBox::currentIndexChanged,this,[this]{if(loading_)return;credentialStorageNotice_->show();if(credentialMode_->currentData()=="keychain"&&reference_->text().isEmpty())reference_->setText("putgd/"+profileId_->text());});
 // Content-sized and in the field column, 8 px apart side by side or stacked.
 auto* credentialActionRow=new QWidget;credentialActionRow->setObjectName("credentialActions");auto* credentialActions=new QHBoxLayout(credentialActionRow);credentialActions->setContentsMargins(0,0,0,0);credentialActions->setSpacing(space(Space::s));auto* credentials=credentialStoreButton_=new QPushButton;credentials->setObjectName("storeCredentials");Ui::role(credentials,"primary");auto* credentialLifetime=PreferenceRows::caption({});auto credentialLabel=[this,credentials,credentialLifetime]{credentialLifetime->setText(credentialMode_->currentData()=="session"?"Will be kept until put.gd closes":"Will be saved in the desktop keychain");credentials->setText(credentialMode_->currentData()=="session"?"Use for this session":"Save to keychain");const auto help="Choose "+credentials->text()+" to apply typed credentials after destination edits save. "+(credentialMode_->currentData()=="session"?QString("They stay in memory until put.gd exits."):QString("They are saved in the desktop secret service."));for(auto* field:QList<QWidget*>{credentialMode_,access_,secret_,token_}){field->setToolTip(help);field->setAccessibleDescription(help);field->setProperty("settingDescription",help);}};connect(credentialMode_,&QComboBox::currentIndexChanged,credentials,credentialLabel);credentialLabel();auto* unlock=new QPushButton("Unlock saved credentials");unlock->setObjectName("unlockCredentials");unlock->hide();for(auto* button:{credentials,unlock}){button->setMinimumHeight(Ui::scaled(Ui::Metric::control));button->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Fixed);credentialActions->addWidget(button,0,Qt::AlignRight);}auto* applyCredentials=PreferenceRows::action(credentialForm,{}, {},credentialActionRow);auto* applyCaptions=qobject_cast<QBoxLayout*>(applyCredentials->layout()->itemAt(0)->layout()->itemAt(0)->layout());auto* applyHeading=applyCaptions->itemAt(0)->widget();applyHeading->hide();applyCaptions->addWidget(credentialLifetime);auto* applyLine=qobject_cast<QBoxLayout*>(applyCredentials->layout()->itemAt(0)->layout());Ui::responsiveColumns(applyLine,destinationScroll->viewport(),640);Ui::responsiveColumns(credentialActions,destinationScroll->viewport(),600);credentialStoreReason_=new QLabel;credentialStoreReason_->setObjectName("credentialStoreReason");credentialStoreReason_->setWordWrap(true);Ui::role(credentialStoreReason_,"muted");applyCaptions->addWidget(credentialStoreReason_);
 // #1361: the custom uploader's Request and Response cards sit above Credentials,
 // where its secret values are stored. Its rows use this window's row style.
 uploader_=new CustomUploaderPanel(this);
 uploader_->build([&](QFormLayout* form,const QString& label,QWidget* control,const QString& hint){return setting(form,label,control,{},hint);});
 pageLayout->insertWidget(pageLayout->indexOf(PreferenceRows::sectionFor(credentialForm)),uploader_);
 connect(uploader_,&CustomUploaderPanel::edited,this,[this]{if(loading_)return;dirty();queueEdit(uploader_,"/profiles/{profile}/settings");});
 connect(uploader_,&CustomUploaderPanel::secretsEdited,this,[this]{if(!loading_){updateStoreButtons();refreshDestinationList();emit viewStateChanged();}});
 connect(uploader_,&CustomUploaderPanel::imported,this,[this](const QString& name){if(!name.isEmpty()&&profileLabel_->text()=="Custom uploader"){profileLabel_->setText(name.left(128));queueEdit(profileLabel_);}});
 protonPanel_=new QWidget;protonPanel_->setObjectName("protonDestinationEditor");auto* protonLayout=new QVBoxLayout(protonPanel_);protonLayout->setContentsMargins(0,0,0,0);protonLayout->setSpacing(space(Space::xxl));pageLayout->addWidget(protonPanel_);auto* protonForm=PreferenceRows::card(protonLayout,"Account");auto* folderPrerequisite=new QLabel("Choose an upload folder after connecting");folderPrerequisite->setObjectName("protonFolderPrerequisite");folderPrerequisite->setWordWrap(true);Ui::role(folderPrerequisite,"muted");protonLayout->addWidget(folderPrerequisite);auto* folderForm=PreferenceRows::card(protonLayout,"Where files go");folderForm->parentWidget()->setObjectName("protonFolderSection");protonFolderStatus_=new QLabel;protonFolderStatus_->setObjectName("protonFolderStatus");protonFolderStatus_->setWordWrap(true);protonFolderStatus_->hide();folderForm->addRow(protonFolderStatus_);auto* publicationForm=PreferenceRows::card(protonLayout,"Links");auto* protonAdvancedPanel=new QWidget;auto* protonAdvancedBox=new QVBoxLayout(protonAdvancedPanel);protonAdvancedBox->setContentsMargins(0,0,0,0);auto* protonAdvancedForm=new QFormLayout;Ui::styleForm(protonAdvancedForm);protonAdvancedBox->addLayout(protonAdvancedForm);protonLayout->addWidget(protonAdvancedPanel);protonAdvancedForm->parentWidget()->setObjectName("protonAdvancedSettings");// The account and its Reconnect action share the field column.
 auto* accountField=new QWidget;auto* accountLayout=new QVBoxLayout(accountField);accountLayout->setContentsMargins(0,0,0,0);accountLayout->setSpacing(space(Space::s));
 protonAccount_=new QLabel("Signed out");protonAccount_->setObjectName("protonAccountStatus");protonAccount_->setWordWrap(true);protonAccount_->setAccessibleName("Proton account");protonAccount_->setProperty("uiStatus",true);Ui::role(protonAccount_,"secondary");accountLayout->addWidget(protonAccount_);protonBrowserRetry_=new QPushButton("Try opening browser again");protonBrowserRetry_->setObjectName("retryProtonBrowser");accountLayout->addWidget(protonBrowserRetry_,0,Qt::AlignLeft);protonBrowserRetry_->hide();connect(protonBrowserRetry_,&QPushButton::clicked,this,&SettingsWindow::retryProtonBrowser);auto* connectProton=new QPushButton("Connect account…");connectProton->setObjectName("connectProtonAccount");connectProton->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Fixed);auto* accountRow=new FormRow;auto* accountRowBox=new QVBoxLayout(accountRow);accountRowBox->setContentsMargins(0,0,0,0);auto* accountLine=new QHBoxLayout;accountLine->setSpacing(space(Space::l));auto* accountText=new QVBoxLayout;accountText->setSpacing(space(Space::xs));accountText->addWidget(Ui::label("Proton account","body"));accountText->addWidget(accountField);accountLine->addLayout(accountText,1);auto* accountActions=new QWidget;accountActions->setObjectName("protonAccountActions");auto* accountButtons=new QVBoxLayout(accountActions);accountButtons->setContentsMargins(0,0,0,0);accountButtons->setSpacing(space(Space::s));accountButtons->addWidget(connectProton);accountButtons->addWidget(protonBrowserRetry_);accountLine->addWidget(accountActions,0,Qt::AlignRight|Qt::AlignVCenter);accountRowBox->addLayout(accountLine);protonForm->addRow(accountRow);new AccountActionSizer(accountRow,accountLine,accountText,accountActions);
 auto* inspectProton=new QPushButton("Refresh folders");inspectProton->setObjectName("inspectProtonAccount");inspectProton->setAccessibleName("Refresh folders");inspectProton->setText({});inspectProton->setIcon(Ui::icon("refresh"));inspectProton->setFixedSize(Ui::scaled(36),Ui::scaled(36));inspectProton->setProperty("uiIconBase",QSize(20,20));inspectProton->setIconSize(QSize(Ui::scaled(20),Ui::scaled(20)));inspectProton->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Fixed);inspectProton->setToolTip("Reload the available Proton folders without changing the selected upload folder.");
 protonFolder_=choices(this,"protonFolder",{});auto* folderRow=setting(folderForm,"Upload folder",protonFolder_,"/profiles/{profile}/settings","Choose a top-level folder for future uploads.");folderRow->setProperty("preferenceStacked",true);auto* createProton=new QPushButton("New folder…");createProton->setObjectName("createProtonFolder");createProton->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Fixed);
 // The folder fills the value column beside Refresh folders and New folder…, 8 px
 // apart; the two actions move under the folder when the page is narrow.
 {auto* line=qobject_cast<QHBoxLayout*>(folderRow->layout()->itemAt(0)->layout());auto* folderPicker=new QWidget;auto* pickerLayout=new QHBoxLayout(folderPicker);pickerLayout->setContentsMargins(0,0,0,0);pickerLayout->setSpacing(space(Space::s));while(auto* item=line->takeAt(0)){if(auto* widget=item->widget()){const auto alignment=item->alignment();delete item;pickerLayout->addWidget(widget,0,alignment);}else if(item->spacerItem())delete item;else pickerLayout->addItem(item);}pickerLayout->setStretchFactor(protonFolder_,1);line->addWidget(folderPicker,1);auto* folderActions=new QWidget;folderActions->setObjectName("protonFolderActions");auto* actionsLayout=new QHBoxLayout(folderActions);actionsLayout->setContentsMargins(0,0,0,0);actionsLayout->setSpacing(space(Space::s));actionsLayout->addStretch();actionsLayout->addWidget(inspectProton);actionsLayout->addWidget(createProton);line->addWidget(folderActions);new FieldActionsSizer(folderRow,line,folderPicker,folderActions);}protonShare_=new QCheckBox("Create a share link");protonShare_->setObjectName("protonShareDefault");protonShare_->setParent(this);bind(protonShare_,"/profiles/{profile}/settings/shareByDefault");auto* privateMode=new PreferenceRadio(sharingLabel("private"));privateMode->setObjectName("protonKeepPrivate");auto* publicMode=new PreferenceRadio(sharingLabel("share"));publicMode->setObjectName("protonCreateShareLink");protonShare_->hide();auto* shareChoice=PreferenceRows::radio(publicMode,"Anyone with the link can view it; they also need any password set under Advanced link settings. Links are copied as set under Settings → Capture → After uploading. Expiration and password apply to future shares; existing links stay unchanged.",{});publicationForm->addRow(shareChoice);publicationForm->addRow(PreferenceRows::radio(privateMode,"No share link is created and the clipboard stays unchanged. Existing member or folder access still applies.",{}));connect(publicMode,&QRadioButton::clicked,this,[this]{protonShare_->setChecked(true);});connect(privateMode,&QRadioButton::clicked,this,[this]{protonShare_->setChecked(false);});connect(protonShare_,&QCheckBox::toggled,this,[=,this](bool share){publicMode->setChecked(share);privateMode->setChecked(!share);if(!loading_)updateCaptureInheritance();});
 protonExpiration_=new QSpinBox;protonExpiration_->setObjectName("protonExpiration");protonExpiration_->setRange(0,365);protonExpiration_->setSpecialValueText("No expiration");protonExpiration_->setSuffix(" days");setting(protonAdvancedForm,"Link expiration",protonExpiration_,"/profiles/{profile}/settings/linkExpirationDays");protonTimeout_=new ConnectionSpin(1000);protonTimeout_->setObjectName("protonRequestTimeout");protonTimeout_->setDecimals(3);protonTimeout_->setRange(1,ProtonProvider::MaximumRequestTimeoutMs/1000);protonTimeout_->setSuffix(" seconds");setting(protonAdvancedForm,"Request deadline",protonTimeout_,"/profiles/{profile}/settings/requestTimeoutMs");protonPassword_=input(this,"protonSharePassword");protonPassword_->setEchoMode(QLineEdit::Password);setting(protonAdvancedForm,"New link password",protonPassword_,{});auto* storePassword=new QPushButton("Store link password");storePassword->setObjectName("storeProtonPassword");protonAdvancedForm->addRow(storePassword);passwordStoreReason_=new QLabel;passwordStoreReason_->setObjectName("passwordStoreReason");passwordStoreReason_->setWordWrap(true);Ui::role(passwordStoreReason_,"muted");protonAdvancedForm->addRow(passwordStoreReason_);auto* clearPassword=new QPushButton("Use no link password");clearPassword->setObjectName("clearProtonPassword");protonAdvancedForm->addRow(clearPassword);
 connect(connectProton,&QPushButton::clicked,this,[this]{protonBrowserOpened_=false;pendingProtonUrl_.clear();pendingProtonSetup_.clear();protonBrowserOpening_=false;protonBrowserRetry_->hide();sizeProtonAccountButtons();beginSetup("connectAccount");});connect(inspectProton,&QPushButton::clicked,this,[this]{beginSetup("inspectAccount");});connect(createProton,&QPushButton::clicked,this,[this]{bool ok=false;const auto name=QInputDialog::getText(this,"New Proton folder","Folder name",QLineEdit::Normal,"put.gd",&ok);if(ok&&!name.trimmed().isEmpty())beginSetup("createFolder",{{"name",name.trimmed()}});});
 connect(storePassword,&QPushButton::clicked,this,[this]{if(!admitSetup())return;const auto reference="putgd/proton/link/"+QUuid::createUuid().toString(QUuid::Id128);auto value=protonPassword_->text();beginSetup("storeSharePassword",{{"password",value},{"reference",reference}});value.fill(QChar{});});connect(clearPassword,&QPushButton::clicked,this,[this]{auto profiles=draft_["profiles"].toArray();if(profileIndex_<0||profileIndex_>=profiles.size())return;auto p=profiles[profileIndex_].toObject();auto s=p["settings"].toObject();s["sharePasswordReference"]=QJsonValue::Null;p["settings"]=s;profiles[profileIndex_]=p;draft_["profiles"]=profiles;queueEdit(protonShare_,"/profiles/{profile}/settings");});
 auto formRow=[this](QWidget* field)->QWidget*{return errors_.contains(field)?errors_[field]->parentWidget():field;};
 const auto relabel=[](QFormLayout* form,QWidget* row,const QString& text){auto* holder=PreferenceRows::labelFor(form,row);if(auto* label=holder?holder->findChild<QLabel*>():nullptr)label->setText(text);};
 updateProviderVisibility_=[=,this]{for(const auto& update:connectionDefaults)update();const auto provider=providerType_->currentData().toString();for(auto* action:services->actions())action->setChecked(action->data()==provider);const bool s3=provider=="s3";const bool b2=provider=="backblaze-b2";const bool r2=provider==r2::providerId;const bool storage=s3||b2||r2;const bool http=provider==CustomUploaderPanel::provider();const bool authorized=(b2&&b2LinkMode_->currentData()=="authorized")||((s3||r2)&&b2LinkMode_->currentData()=="presigned");const bool expanded=advanced->isChecked();updateR2Feedback();s3_["publicUrlBase"]->setPlaceholderText(r2?"https://pub-….r2.dev":"https://i.example.com");links->setRowVisible(r2LinkWarning,r2&&!authorized&&s3_["publicUrlBase"]->text().trimmed().isEmpty());advancedCaption->setText(storage?"Advanced connection settings":"Advanced link settings");advanced->setAccessibleName(advancedCaption->text());description->setProperty("compactText",s3?"Amazon S3":r2?"Cloudflare R2":b2?"Backblaze B2 · Native API":http?"Custom uploader · HTTP":"Proton Drive · Third-party integration");description->setProperty("fullText",s3?"Amazon S3 or compatible storage. Existing uploads keep their original destination.":r2?"Cloudflare R2 through its S3 API. Existing uploads keep their original destination.":b2?"Backblaze B2 through its native API. Existing uploads keep their original destination.":http?"Your own server or any service with a ShareX custom uploader, through one HTTP request. Existing uploads keep their original destination.":"Third-party Proton Drive integration. Share links open a Proton page.");description->setText(description->property("fullText").toString());for(auto* field:{s3_["bucket"],s3_["objectPrefix"]})destinationDetails->setRowVisible(formRow(field),storage);PreferenceRows::sectionFor(destinationDetails)->setVisible(storage);destinationDetails->setRowVisible(formRow(providerType_),false);destinationDetails->setRowVisible(s3ServiceRow,s3);destinationDetails->setRowVisible(r2AccountRow,r2);destinationDetails->setRowVisible(r2JurisdictionRow,r2);destinationDetails->setRowVisible(moveToR2Row,s3&&r2::fromS3Settings(editedProfile()["settings"].toObject()).has_value());destinationDetails->setRowVisible(formRow(regionChoices_),s3);connection->setRowVisible(PreferenceRows::rowFor(endpointHint_),(s3&&!customEndpoint->isChecked())||r2);destinationDetails->setRowVisible(endpointWarning,s3&&s3_["endpoint"]->text().contains("backblazeb2.com"));connection->setRowVisible(customEndpointRow,s3);connection->setRowVisible(formRow(s3_["endpoint"]),s3&&customEndpoint->isChecked());PreferenceRows::sectionFor(links)->setVisible(storage);links->setRowVisible(aclRow,s3&&!authorized);aclHint->setVisible(s3&&!authorized);links->setRowVisible(b2ModeRow,storage);links->setRowVisible(b2DaysRow,authorized);if(b2DaysHint){const auto hint=s3||r2?QString("Uses the S3 endpoint host, ignoring Link domain. Temporary credentials can expire sooner than the chosen 1–7 days. Files stay in the bucket."):QString("Links expire after 1–7 days; files stay in the bucket.");b2DaysHint->setText(hint);b2LinkDays_->setAccessibleDescription(hint);b2LinkDays_->setToolTip(hint);b2LinkDays_->setProperty("settingDescription",hint);}links->setRowVisible(formRow(s3_["publicUrlBase"]),storage&&!authorized);auto* exampleHost=(s3||r2)&&authorized?b2DaysRow:linkDomainRow;if(exampleLine->parentWidget()!=exampleHost){exampleHost->layout()->addWidget(exampleLine);exampleLine->show();}connectionPanel_->setVisible(storage&&expanded);for(auto it=connectionRows.begin();it!=connectionRows.end();++it)connection->setRowVisible(it.value(),s3||(r2&&it.key()!="storageClass")||it.key()=="connectTimeoutMs"||it.key()=="requestTimeoutMs");connection->setRowVisible(addressRow,s3);PreferenceRows::sectionFor(credentialForm)->setVisible(storage||http);credentialForm->setRowVisible(tokenRow,s3);credentialForm->setRowVisible(accessRow,storage);credentialForm->setRowVisible(secretRow,storage);currentKeyCaption->setText(http?"Stored values":"Current key");uploader_->setVisible(http);relabel(credentialForm,accessRow,b2?"Application key ID":"Access key ID");relabel(credentialForm,secretRow,b2?"Application key":"Secret access key");access_->setAccessibleName(b2?"Application key ID":"Access key ID");secret_->setAccessibleName(b2?"Application key":"Secret access key");protonPanel_->setVisible(provider=="proton-drive");if(auto* footer=findControl<QWidget*>("destinationTestSection");footer&&footer->parentWidget()!=(storage||http?credentialForm->parentWidget():protonForm->parentWidget())){auto* previous=storage||http?protonForm:credentialForm;auto taken=previous->takeRow(footer);delete taken.fieldItem;delete taken.labelItem;(storage||http?credentialForm:protonForm)->addRow(footer);footer->show();}if(auto* footer=findControl<QWidget*>("destinationTestSection"))footer->show();test->setVisible(storage||http||provider=="proton-drive");test->setText(storage||http?"Test upload and link":"Check account and folder");sizeProtonAccountButtons();test->setAccessibleName(test->text());testScope->setText(storage?"Tests an upload, its link and cleanup.":http?"Uploads a small test file and opens its link. The file is deleted when the server returns a deletion URL.":"Checks account and folder access. Upload and sharing aren't tested.");showSecret_->setAccessibleName((showSecret_->isChecked()?QString("Hide "):QString("Show "))+secret_->accessibleName().toLower());reveal->setText(showSecret_->accessibleName());reveal->setToolTip(reveal->text());test->setToolTip(s3||r2?"Checks an upload, link access and cleanup. Expiring links must work while unsigned access is denied.":http?"Uploads a small synthetic file with this destination's request, opens the returned link, and deletes the file when a deletion URL came back.":b2?"Checks the application key, a test upload, the link put.gd hands out, cleanup and a large-file start.":"Checks the signed-in Proton account and selected folder without uploading. Upload and sharing aren't tested.");if(advancedRow->parentWidget()!=(storage?destinationDetails->parentWidget():publicationForm->parentWidget())){auto* previous=storage?publicationForm:destinationDetails;auto taken=previous->takeRow(advancedRow);delete taken.fieldItem;delete taken.labelItem;auto* target=storage?destinationDetails:publicationForm;int row=-1;QFormLayout::ItemRole role;target->getWidgetPosition(storage?connectionPanel_:protonAdvancedPanel,&row,&role);target->insertRow(row<0?target->rowCount():row,advancedRow);}advancedRow->show();advanced->setVisible(true);if(http)advancedRow->hide();QStringList departures;if(s3&&s3_["storageClass"]->text()!="STANDARD")departures.append(storageClassLabel(s3_["storageClass"]->text()));if(s3&&addressing_->currentData()!="auto")departures.append(addressing_->currentText());for(const auto& timeout:{QString("connectTimeoutMs"),QString("requestTimeoutMs")})if(inputText(s3_[timeout]).toDouble()!=(timeout=="connectTimeoutMs"?5:60))departures.append(inputText(s3_[timeout])+(timeout=="connectTimeoutMs"?" s connect":" s request"));connectionSummary->setText(departures.join(" · "));connectionSummary->setVisible(storage&&!expanded&&!departures.isEmpty());protonAdvancedForm->parentWidget()->setVisible(provider=="proton-drive"&&expanded);endpointHint_->setText(r2?r2::endpoint(r2AccountId_->text(),r2Jurisdiction_->currentData().toString()):s3_["endpoint"]->text());updateBucketHint();{const QSignalBlocker block(s3Service);const auto endpoint=s3_["endpoint"]->text();s3Service->setCurrentIndex(s3AmazonEndpoint(endpoint)?0:1);}if(b2){const auto settings=editedProfile()["settings"].toObject();static_cast<Ui::ElidedLabel*>(linkExample_)->setText(b2::fileUrl(settings,"https://f000.backblazeb2.com",b2::fileName(settings,"Screenshot.png"))+(authorized?QStringLiteral("?Authorization=…"):QString()));}else{auto settings=editedProfile()["settings"].toObject();if(r2)settings=r2::s3Settings(settings);if((s3||r2)&&authorized)settings["publicUrlBase"]=QJsonValue::Null;static_cast<Ui::ElidedLabel*>(linkExample_)->setText(publicObjectUrl(settings,s3_["objectPrefix"]->text()+"Screenshot.png")+((s3||r2)&&authorized?QStringLiteral("?X-Amz-Signature=…"):QString()));}};
 // Keep advanced controls and the connection test inside the provider's cards.
 pageLayout->removeWidget(advanced);pageLayout->removeWidget(connectionPanel_);destinationDetails->addRow(connectionPanel_);protonLayout->removeWidget(protonAdvancedForm->parentWidget());publicationForm->addRow(protonAdvancedForm->parentWidget());
 for(const auto& key:QStringList{"publicUrlBase","bucket","objectPrefix","endpoint"})connect(s3_[key],&QLineEdit::textChanged,this,[this]{if(!loading_&&updateProviderVisibility_)updateProviderVisibility_();});connect(r2AccountId_,&QLineEdit::textChanged,this,[this]{if(!loading_&&updateProviderVisibility_)updateProviderVisibility_();});
 connect(advanced,&QToolButton::toggled,this,[this]{updateProviderVisibility_();emit viewStateChanged();});connect(customEndpoint,&QCheckBox::toggled,this,[this]{updateProviderVisibility_();});
 connect(providerType_,&QComboBox::currentIndexChanged,this,&SettingsWindow::switchProvider);
 connect(destinationList_,&QListWidget::currentRowChanged,this,[this](int index){if(!loading_)profile_->setCurrentIndex(index);refreshUploadedCount();});
 // Kept current while Destinations shows: a new upload or a History change counts at once.
 connect(&settings_,&UiSession::statusChanged,this,[this]{if(destinationPage_&&destinationPage_->isVisible())refreshUploadedCount();});connect(profile_,&QComboBox::currentIndexChanged,this,[this]{if(loading_)return;flushTyping();storeProfile();loadProfile();refreshDestinationList();if(destinationPage_->isVisible())activateDestinations();emit viewStateChanged();});
 connect(defaultButton_,&QPushButton::clicked,this,[this]{if(checkState(editedProfile())!="passed"&&!Ui::confirm(this,"Use an untested destination","This connection has not passed a test with its current settings. Use \""+profileLabel_->text()+"\" as the capture default? Per-action exceptions will stay unchanged.","Make default"))return;{const QSignalBlocker block(captureProfile_);captureProfile_->setCurrentIndex(captureProfile_->findData(profileId_->text()));}queueCaptureEdit(defaultButton_);});
 connect(remove,&QPushButton::clicked,this,[this]{flushTyping();
 // #1759: the confirmation says how many History items went to it (counted when it was selected); their files stay there.
 const auto all=candidate()["profiles"].toArray();const int at=profile_->currentIndex();if(at<0||at>=all.size())return;const auto label=profileLabel_->text();
 const int uploaded=uploadedCounts_.value(all[at].toObject()["id"].toString(),-1);
 const auto files=uploaded<0?QStringLiteral("Files already uploaded to it stay there, and History keeps them."):uploaded==0?QStringLiteral("No History items were uploaded to it."):QStringLiteral("%1 History %2 uploaded to it; %3 there, and History keeps %4. put.gd can delete %4 there again only if you add a destination for the same place.").arg(uploaded).arg(uploaded==1?"item was":"items were",uploaded==1?"its file stays":"their files stay",uploaded==1?"it":"them");
 if(!Ui::confirm(this,"Remove destination","Remove \""+label+"\" from settings?\n"+files,"Remove destination",true))return;storeProfile();draft_=candidate();auto profiles=draft_["profiles"].toArray();const int index=profile_->currentIndex();if(index<0||index>=profiles.size())return;const auto id=profiles[index].toObject()["id"].toString();// Routes to it go back to what the file has: no destination where the file routes to it too.
  const auto accepted=acceptedDocument();const auto back=[&](const QJsonValue& route){const auto object=route.toObject();const auto named=object.value("profileId").toString();return object.value("destination")=="upload"&&named!=id?named:QString();};
  if(captureProfile_->currentData()==id){const QSignalBlocker block(captureProfile_);captureProfile_->setCurrentIndex(qMax(0,captureProfile_->findData(back(accepted.value("captureDefaults")))));updateCaptureInheritance();}
  {// Quietly: the removal's own fallback, worked out from the file when it is sent, covers these.
   const QScopedValueRollback quiet(loading_,true);for(const auto& action:Settings::presentationOrder())if(actionProfiles_[action]->currentData()==id)actionProfiles_[action]->setCurrentIndex(qMax(0,actionProfiles_[action]->findData(back(accepted.value("actions").toObject().value(action)))));}
  updateCaptureInheritance();captureDefaultsEnabled_=true;captureRoutingEdited_=true;
  // Routes the draft itself holds, such as a recording route the form hasn't touched, stop naming it.
  {auto actions=draft_.value("actions").toObject();const auto saved=accepted.value("actions").toObject();for(auto it=actions.begin();it!=actions.end();++it){auto action=it.value().toObject();if(action.value("profileId")!=QJsonValue(id))continue;const auto file=saved.value(it.key()).toObject();if(file.value("profileId")!=QJsonValue(id)&&file.contains("destination")){for(const auto* key:{"destination","profileId","publicationMode"}){if(file.contains(key))action[key]=file.value(key);else action.remove(key);}}else{action["destination"]="local";action["profileId"]=QJsonValue::Null;}it.value()=action;}draft_["actions"]=actions;}
  profiles.removeAt(index);if(index<profileEditors_.size())profileEditors_.removeAt(index);if(index<credentialDrafts_.size())credentialDrafts_.removeAt(index);draft_["profiles"]=profiles;rebuildProfiles(qMin(index,int(profiles.size())-1));queueRemoval(id);refreshDestinationList();});
 connect(dev,&QPushButton::clicked,this,&SettingsWindow::importDevelopmentProfile);
 connect(s3_["region"],&QLineEdit::textChanged,this,[this](const QString& region){if(loading_)return;auto* endpoint=s3_["endpoint"];const auto previous=s3_["region"]->property("previousRegion").toString();if(endpoint->text()=="https://s3."+previous+".amazonaws.com")endpoint->setText("https://s3."+region+".amazonaws.com");s3_["region"]->setProperty("previousRegion",region);updateProviderVisibility_();});
 // A Region message follows Endpoint: moving a destination to Cloudflare R2, which ignores Region,
 // clears it at once, under the field, in the footer and in the destination status. It is only
 // ever cleared here; a save reports a Region that is still wrong.
 // A message that clears goes at once; a new one waits until typing pauses, so "us-" doesn't flash.
 auto* settle=new QTimer(this);settle->setSingleShot(true);settle->setInterval(400);connect(settle,&QTimer::timeout,this,[this]{if(!loading_)updateRegionFeedback();});
 for(auto* field:{s3_["endpoint"],s3_["region"]}){
  connect(field,&QLineEdit::textChanged,this,[this,settle]{if(loading_)return;if(updateRegionFeedback(true))settle->stop();else settle->start();});
  connect(field,&QLineEdit::editingFinished,this,[this,settle]{if(loading_)return;settle->stop();updateRegionFeedback();});
 }
 connect(credentials,&QPushButton::clicked,this,[this]{if(!admitSetup())return;resetCredentialSecretDisplay();
  // #1361: a custom uploader stores the values typed into its Secret rows, by name.
  if(providerType_->currentData()==CustomUploaderPanel::provider()){QJsonObject values;const auto typed=uploader_->typedSecrets();for(auto it=typed.cbegin();it!=typed.cend();++it)values[it.key()]=it.value();if(values.isEmpty()){status({"credential_invalid","Type a value into a row marked Secret first; those are the values stored here.",{}});return;}beginSetup("storeCredentials",{{"values",values}});return;}
  QJsonObject value{{"accessKey",access_->text()},{"secretKey",secret_->text()},{"sessionToken",token_->text()}};beginSetup("storeCredentials",value);});connect(unlock,&QPushButton::clicked,this,[this]{beginSetup("unlockCredentials");});connect(test,&QPushButton::clicked,this,[this]{beginSetup("testDestination");});
 // Each page's status line carries its recovery actions beside the message:
 // Reload from file… for a conflict, Retry saving for a refused save. Saved
 // notes stay meta type; errors take body type (the style's meta role).
 // #1675: a configuration file the resident refused has its own line, which form feedback never
 // writes or clears; only a successful reload or save does (configurationChanged).
 for(auto** line:{&fileError_,&destinationFileError_}){*line=new QLabel;(*line)->setObjectName(line==&fileError_?"configFileError":"destinationConfigFileError");(*line)->setAccessibleName("Configuration file status");(*line)->setWordWrap(true);(*line)->setTextFormat(Qt::PlainText);Ui::role(*line,"error");(*line)->hide();}
 showFileError();
 status_=new QLabel;status_->setObjectName("settingsStatus");status_->setAccessibleName("Settings status");status_->setWordWrap(true);Ui::role(status_,"meta");destinationStatus_=new QLabel;destinationStatus_->setObjectName("destinationStatus");destinationStatus_->setWordWrap(true);destinationStatus_->setAccessibleName("Destination status");Ui::role(destinationStatus_,"muted");
 // Settings' status is a footer under the scrolling page, set off by a hairline.
 QList<SettingsStatusRow*> feedbackRows;QList<QHBoxLayout*> statusRows;for(int i=0;i<2;++i){auto* message=new QWidget;auto* text=new QVBoxLayout(message);text->setContentsMargins(0,0,0,0);text->setSpacing(space(Space::xs));auto* holder=new QWidget;auto* held=new QVBoxLayout(holder);held->setContentsMargins(0,0,0,0);held->addWidget(i?destinationStatus_:status_);text->addWidget(holder);text->addWidget(i?destinationFileError_:fileError_);auto* row=new SettingsStatusRow(message,i==1);row->setObjectName(i?"destinationStatusRow":"settingsStatusRow");feedbackRows.append(row);statusRows.append(row->actions());if(i){destinationContentLayout->addWidget(row);}else layout->addWidget(row);}
 // Council 22: a conflict is a warning, not a failure: a banner with Reload from file…, not red text.
 for(int i=0;i<2;++i){auto* banner=new Ui::Banner(Ui::Banner::Tone::Warning,conflictMessage());banner->setObjectName(i?"destinationConflictBanner":"settingsConflictBanner");banner->setAction("Reload from file…");auto* reload=banner->actionButton();reload->setObjectName(i?"reloadDestinationConflict":"reloadConflict");reload->setAccessibleName("Reload settings from file");reload->setToolTip("Replace this window's settings with the configuration file as it is now. Asks first, naming the unsaved edits it discards.");reload->hide();banner->hide();reloadConflict_.insert(i,reload);conflictBanners_.insert(i,banner);feedbackRows[i]->addBanner(banner);connect(reload,&QPushButton::clicked,this,&SettingsWindow::reloadFromFile);}
 for(int i=0;i<2;++i){auto* retry=new QPushButton("Retry saving");retry->setObjectName(i?"retryDestinationAutosave":"retryAutosave");retry->hide();retryAutosave_.append(retry);statusRows[i]->addWidget(retry,0,Qt::AlignTop);connect(retry,&QPushButton::clicked,this,&SettingsWindow::retrySaving);}
 for(int i=0;i<2;++i)new VisibleWithParts(feedbackRows[i],{i?destinationStatus_:status_,conflictBanners_[i],retryAutosave_[i],i?destinationFileError_:fileError_});
 auto* testSection=new FormRow;testSection->setObjectName("destinationTestSection");auto* testBox=new QVBoxLayout(testSection);testBox->setContentsMargins(space(Space::l),space(Space::s),space(Space::l),space(Space::s));testBox->setSpacing(space(Space::s));auto* testLine=new QHBoxLayout;testLine->setSpacing(space(Space::l));auto* testText=new QVBoxLayout;testText->setSpacing(space(Space::xs));testText->setAlignment(Qt::AlignVCenter);testText->addWidget(Ui::label("Connection test","body"));testText->addWidget(testScope);testLine->addLayout(testText,1);testLine->addWidget(test,0,Qt::AlignRight|Qt::AlignVCenter);testBox->addLayout(testLine);new ConnectionTestSizer(testSection,testLine,testScope,test);credentialForm->addRow(testSection);testSection->setProperty("preferenceRow",true);testSection->setProperty("preferenceRowInset",true);testSection->setProperty("preferenceNaturalHeight",true);
 connect(scrollHeight_,&QSpinBox::valueChanged,this,[this]{queueEdit(scrollHeight_);});
 connect(randomLength_,&QSpinBox::valueChanged,this,[this]{queueEdit(randomLength_);});
 // #1704: typed text is queued once typing pauses or the field is left; a choice, a toggle, a
 // paste or a value set for the user is queued at once.
 typingPause_=new QTimer(this);typingPause_->setSingleShot(true);typingPause_->setInterval(600);connect(typingPause_,&QTimer::timeout,this,&SettingsWindow::flushTyping);
 const auto typing=[this](QWidget* control){if(!control->hasFocus()||pasting_==control)return false;if(typingControl_&&typingControl_!=control)flushTyping();typingControl_=control;typingPause_->start();return true;};
 // A paste is marked for the change it makes, then forgotten.
 const auto pasted=[this](QWidget* control){pasting_=control;QTimer::singleShot(0,this,[this]{pasting_=nullptr;});};
 const auto leftField=[this](QWidget* control){if(control==typingControl_)flushTyping();};
 for(auto* edit:findChildren<QLineEdit*>()){if(edit==access_||edit==secret_||edit==token_||edit==protonPassword_||edit->property("configPath").toString().isEmpty())continue;new TypingWatch(edit,pasted,leftField);connect(edit,&QLineEdit::textChanged,this,[this,edit,typing]{if(loading_)return;dirty();if(!edit->isReadOnly()&&typing(edit))return;
  // Endpoint follows a Region being typed and saves with it.
  if(edit==s3_.value("endpoint")&&s3_.value("region")&&s3_.value("region")->hasFocus())return;queueEdit(edit);});connect(edit,&QLineEdit::editingFinished,this,[this,edit]{queueEdit(edit);});}
 for(auto* edit:{access_,secret_,token_,protonPassword_})connect(edit,&QLineEdit::textChanged,this,&SettingsWindow::viewStateChanged);
 for(auto* combo:findChildren<QComboBox*>())if(combo!=profile_&&!combo->property("configPath").toString().isEmpty())connect(combo,&QComboBox::currentIndexChanged,this,[this,combo]{queueEdit(combo);});
 for(auto* box:{quickRedactTerms_,quickRedactPatterns_}){new TypingWatch(box,pasted,leftField);connect(box,&QPlainTextEdit::textChanged,this,[this,box,typing]{if(loading_)return;dirty();if(!typing(box))queueEdit(box);});}
 for(auto* check:findChildren<QCheckBox*>())if(!check->property("configPath").toString().isEmpty())connect(check,&QCheckBox::toggled,this,[this,check]{queueEdit(check);});
 // #237: a picked theme applies at once and saves without the autosave delay,
 // so the resident passes it to putgd-record straight away too.
 connect(appearance_,&QComboBox::currentIndexChanged,this,[this]{
  if(loading_||resetting_)return;
  appearancePreviewed_=true;emit appearancePreview(appearance_->currentData().toString());
  // Nothing to save, or saving is blocked until Retry saving or Reload: the
  // accepted theme stays.
  if(!queued(themePath)||autosaveBlocked_||conflict_)endAppearancePreview();
 });
 for(auto* spin:findChildren<QAbstractSpinBox*>())connect(spin,&QAbstractSpinBox::editingFinished,this,[this,spin]{queueEdit(spin);});
 // Editable values use the interface face; read-only commands and paths keep mono.
 // Trailing actions share their field's line box, including enlarged text.
 for(auto* button:findChildren<QPushButton*>())if(button->property("uiDensity")!="compact"&&button->maximumHeight()>=Ui::scaled(Ui::Metric::control)){auto policy=button->sizePolicy();policy.setVerticalPolicy(QSizePolicy::Preferred);button->setSizePolicy(policy);button->setMinimumHeight(Ui::scaled(Ui::Metric::control));}
 // The form owns the helper line, so its height participates in row allocation.
 for(int row=0;row<credentialForm->rowCount();++row)if(auto* item=credentialForm->itemAt(row,QFormLayout::FieldRole);item&&item->widget()){auto* field=item->widget();if(field->property("visualRole")=="settingRow"||field->property("separateFieldHint").toBool())continue;if(auto* box=field->layout()){auto margins=box->contentsMargins();margins.setBottom(margins.bottom()+space(Space::s));box->setContentsMargins(margins);field->setMinimumHeight(box->totalMinimumSize().height());}else{field->setContentsMargins(0,0,0,space(Space::s));field->setMinimumHeight(Ui::scaled(Ui::Metric::control)+space(Space::s));}}
 // Keep each existing field as the form's row, so autosave and row visibility keep their identities.
 auto privacyRow=updatesForm->takeRow(privacy);auto privacyNote=updatesForm->takeRow(privacyText);delete privacyRow.fieldItem;delete privacyNote.fieldItem;updatesForm->addRow(privacy);updatesForm->addRow(privacyText);
 for(auto* form:preferenceForms)PreferenceRows::box(form);PreferenceRows::alignChoices(tabs_);
 for(auto* box:{general,capture,shortcutsPage,advancedPage}){box->setSizeConstraint(QLayout::SetMinimumSize);auto* body=box->parentWidget();auto* scroll=qobject_cast<QScrollArea*>(body->parentWidget()->parentWidget());auto* column=new FormRow;column->setObjectName(box==general?"generalPreferenceColumn":box==capture?"capturePreferenceColumn":box==shortcutsPage?"shortcutsPreferenceColumn":"advancedPreferenceColumn");column->setMaximumWidth(Ui::scaled(760));auto* centered=new QVBoxLayout(column);centered->setContentsMargins(0,0,0,0);centered->setSpacing(0);while(auto* item=box->takeAt(0)){if(auto* widget=item->widget()){widget->setMaximumWidth(Ui::scaled(760));if(centered->count())centered->addSpacing(space(centered->count()==1?Space::xl:Space::xxl));addPreferenceWidget(centered,widget);delete item;}else if(auto* nested=item->layout())centered->addLayout(nested);else centered->addItem(item);}column->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Minimum);box->addWidget(column,0,Qt::AlignLeft);if(scroll){scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);new PreferenceColumnSizer(column,scroll->viewport());}}
 for(auto* field:destinationBody->findChildren<QWidget*>())if((qobject_cast<QLineEdit*>(field)&&!qobject_cast<QAbstractSpinBox*>(field->parentWidget()))||qobject_cast<QComboBox*>(field)){field->setProperty("uiFullWidth",true);if(auto* combo=qobject_cast<QComboBox*>(field))combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);field->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);field->setMaximumWidth(QWIDGETSIZE_MAX);for(auto* line:destinationBody->findChildren<QLayout*>())line->setAlignment(field,{});}
 for(auto* form:destinationBody->findChildren<QFormLayout*>()){for(int row=0;row<form->rowCount();++row)if(auto* item=form->itemAt(row,QFormLayout::FieldRole);item&&item->widget())item->widget()->setProperty("preferenceResponsive",true);PreferenceRows::box(form);}PreferenceRows::alignChoices(destinationBody,true);
 for(auto* field:destinationBody->findChildren<QWidget*>())if(qobject_cast<QLineEdit*>(field)&&field!=profileLabel_&&!qobject_cast<QComboBox*>(field->parentWidget())&&!qobject_cast<QAbstractSpinBox*>(field->parentWidget()))Ui::role(field,QStringList{"s3PublicUrlBase","s3Endpoint"}.contains(field->objectName())?"technical":"body");
 for(auto* spin:destinationBody->findChildren<QAbstractSpinBox*>()){spin->ensurePolished();spin->setFixedWidth(qMax(Ui::scaled(120),spin->sizeHint().width()));}
 for(auto* label:destinationBody->findChildren<QLabel*>())if(label->property("visualRole")=="meta"||label->property("visualRole")=="muted"){label->setProperty("uiStatus",true);Ui::role(label,"secondary");}


 auto* destinationColumn=new FormRow;destinationColumn->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Minimum);destinationColumn->setObjectName("destinationPreferenceColumn");auto* destinationColumnBox=new QVBoxLayout(destinationColumn);destinationColumnBox->setContentsMargins(0,0,0,0);destinationColumnBox->setSpacing(space(Space::xxl));while(auto* item=pageLayout->takeAt(0)){if(auto* widget=item->widget()){addPreferenceWidget(destinationColumnBox,widget);delete item;}else destinationColumnBox->addItem(item);}pageLayout->addWidget(destinationColumn,0,Qt::AlignLeft);destinationScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);new PreferenceColumnSizer(destinationColumn,destinationScroll->viewport());
 const auto sharedForms=tabs_->findChildren<QFormLayout*>();for(int page=0;page<tabs_->count();++page)if(auto* scroll=qobject_cast<QScrollArea*>(tabs_->widget(page)))Ui::alignForms(scroll->widget(),sharedForms);Ui::alignChoices(tabs_,sharedForms);
 for(auto* field:findChildren<QLineEdit*>())if(field!=profileLabel_&&!qobject_cast<QComboBox*>(field->parentWidget())&&!qobject_cast<QAbstractSpinBox*>(field->parentWidget())&&field->maximumWidth()==QWIDGETSIZE_MAX&&!field->property("uiFullWidth").toBool())field->setMaximumWidth(Ui::scaled(480));
 for(auto* box:{general,capture,shortcutsPage,advancedPage})for(int i=0;i<box->count();++i)if(auto* widget=box->itemAt(i)->widget())widget->setMaximumWidth(Ui::scaled(760));
 connect(&settings_,&UiSession::accepted,this,[this]{configurationChanged();refreshEffectiveLimits();});connect(&settings_,&UiSession::statusChanged,this,[this]{configurationChanged();refreshDiagnostics();});connect(&settings_,&UiSession::destinationStatusChanged,this,[this]{refreshDestinationList();updateProtonAccountDisplay();});
 // The resident's startup unlock can end after Destinations opened; show the key it read (#600).
 connect(&settings_,&UiSession::credentialsChanged,this,[this]{refreshCredentialHint();});connect(tabs_,&QTabWidget::currentChanged,this,&SettingsWindow::viewStateChanged);
 // #348: Updates stays in view while a freshly shown window settles its size and
 // rows, until the user scrolls or leaves the page.
 if(auto* scroll=qobject_cast<QScrollArea*>(tabs_->widget(0))){
  connect(scroll->verticalScrollBar(),&QScrollBar::rangeChanged,this,[this]{if(revealUpdates_)revealUpdates();});
  connect(scroll->verticalScrollBar(),&QScrollBar::actionTriggered,this,[this]{revealUpdates_=false;});
  generalPage_=scroll;updatesCard_=updatesForm_->parentWidget();scroll->installEventFilter(this);updatesCard_->installEventFilter(this);
 }
 connect(this,&SettingsWindow::viewStateChanged,this,&SettingsWindow::updateStoreButtons);
 setupTimer_=new QTimer(this);setupTimer_->setInterval(500);connect(setupTimer_,&QTimer::timeout,this,&SettingsWindow::pollSetup);
 for(auto* label:findChildren<QLabel*>())label->setTextFormat(Qt::PlainText);
 // The wheel scrolls the page: a number under the pointer must not change, or take focus, as the page moves past it.
 for(auto* spin:findChildren<QAbstractSpinBox*>()){spin->setFocusPolicy(Qt::StrongFocus);spin->installEventFilter(this);}
 refresh();refreshDiagnostics();refreshDestinationList();updateProviderVisibility_();refreshEffectiveLimits();fitRails();
}
SettingsWindow::~SettingsWindow(){
 revealUpdates_=false;
 for(auto* child:findChildren<QObject*>())disconnect(child,nullptr,this,nullptr);
 if(destinationPage_)for(auto* child:destinationPage_->findChildren<QObject*>())disconnect(child,nullptr,this,nullptr);
 disconnect(&settings_,nullptr,this,nullptr);
}
bool SettingsWindow::eventFilter(QObject* watched,QEvent* event){
 if(watched==destinationPage_&&event->type()==QEvent::Hide)resetCredentialSecretDisplay();
 // A destination opened before its page was on screen scrolls to its Endpoint problem now (#1676).
 if(watched==destinationPage_&&event->type()==QEvent::Show&&!loading_)showS3SettingsProblems();
 if(watched==destinationPage_&&event->type()==QEvent::Hide)s3_["endpoint"]->setProperty("revealedFor",{});
 if(watched==tabs_->tabBar()&&event->type()==QEvent::Wheel){event->ignore();return true;}
 if(event->type()==QEvent::Wheel&&qobject_cast<QAbstractSpinBox*>(watched)){event->ignore();return true;}
 if(revealUpdates_&&(event->type()==QEvent::Move||event->type()==QEvent::Resize)&&watched==updatesCard_)revealUpdates();
 if(event->type()==QEvent::Hide&&watched==generalPage_&&!event->spontaneous())revealUpdates_=false;
 return RetainedWindow::eventFilter(watched,event);
}
void SettingsWindow::refreshMetadata(){if(configPath_)static_cast<Ui::ElidedLabel*>(configPath_)->setText(settings_.path());metadataText_="Configuration: "+settings_.path()+"\nAccepted revision: "+settings_.revision()+"\nObserved file revision: "+settings_.metadata()["observedRevision"].toString();
  // Paths and 64-digit revisions may break at a slash or every 16 digits, so a
  // narrow page wraps them instead of growing; the accessible name stays exact.
  static_cast<MetadataLabel*>(metadata_)->setExact(metadataText_);
  const auto accepted=settings_.revision(),observed=settings_.metadata()["observedRevision"].toString();const bool differs=accepted!=observed;
  for(const auto& key:QStringList{"accepted","observed"}){auto* value=static_cast<MetadataLabel*>(capabilities_->findChild<QLabel*>(key+"Revision"));value->setExact(key=="accepted"?accepted:observed);value->setVisible(differs);capabilities_->findChild<QLabel*>(key+"RevisionLabel")->setVisible(differs);}
}
void SettingsWindow::dirty(){if(loading_)return;dirty_=true;emit viewStateChanged();}
// #1675: Region feedback comes only from the shown destination's current Endpoint and Region,
// checked as the provider checks them, here and nowhere else: no save answer writes or restores
// it. It sets or clears the message under Region, in the footer and in the destination status.
void SettingsWindow::updateR2Feedback(){
 if(!r2AccountId_||!r2UnitStatus_)return;
 const bool r2=providerType_->currentData()==r2::providerId;
 const auto settings=r2?editedProfile()["settings"].toObject():QJsonObject{};
 const QList<QPair<QString,QWidget*>> fields{{"accountId",r2AccountId_},{"jurisdiction",r2Jurisdiction_},{"bucket",s3_["bucket"]},{"publicUrlBase",s3_["publicUrlBase"]}};
 QString problem;
 for(const auto& [key,control]:fields){
  const auto message=r2?r2::problem(settings,key):QString();if(problem.isEmpty())problem=message;
  // Only this check writes or clears these messages while the destination is Cloudflare R2.
  if(r2||control->property("r2Feedback").toBool())inlineError(control,message);control->setProperty("r2Feedback",r2&&!message.isEmpty());
 }
 // The line under Bucket: the queue holds Account ID, Jurisdiction and Bucket back, as one group, because
 // they aren't valid yet or were refused (#1704's record of what isn't saved).
 QString unitLine;
 if(r2){
  const auto shown=editedProfile();
  const auto reason=notSaved_.value(shown["id"].toString()+"|"+r2Group().join('+'));
  if(!reason.isEmpty())unitLine=localProblem(shown,r2Group()).isEmpty()?"Account ID, Jurisdiction and Bucket aren't saved: "+reason:QStringLiteral("Account ID, Jurisdiction and Bucket save together once all three are valid.");
 }
 r2UnitStatus_->setText(unitLine);r2UnitStatus_->setVisible(!unitLine.isEmpty());
 if(problem.isEmpty()&&!unitLine.isEmpty())problem=unitLine;
 if(conflict_)return;
 for(auto* label:{status_,destinationStatus_}){if(!label)continue;
  if(!problem.isEmpty()){plain(label);label->setProperty("errorCode",QStringLiteral("s3_settings_invalid"));label->setProperty("errorField",QStringLiteral("r2"));label->setToolTip(QStringLiteral("s3_settings_invalid"));Ui::feedback(label,problem,true);}
  else if(label->property("errorField").toString()=="r2"){label->setProperty("errorField",QString{});Ui::feedback(plain(label),{});}
 }
}
bool SettingsWindow::updateRegionFeedback(bool clearOnly){
 // #1676: Endpoint feedback comes from the same check of the shown destination, never from an answer.
 const bool s3=providerType_->currentData()=="s3";
 const auto problem=s3&&!s3CloudflareR2Endpoint(s3_["endpoint"]->text())?s3RegionProblem(s3_["region"]->text(),s3_["endpoint"]->text()):QString();
 const auto endpointProblem=s3?s3EndpointProblem(s3_["endpoint"]->text(),true):QString();
 // The Endpoint message quotes what was typed: shown as text, never as markup.
 if(auto* label=errors_.value(s3_["endpoint"]))label->setTextFormat(Qt::PlainText);
 if(clearOnly){if(problem.isEmpty())inlineError(s3_["region"],{});if(endpointProblem.isEmpty())inlineError(s3_["endpoint"],{});
  for(auto* label:{status_,destinationStatus_})if(label){const auto field=label->property("errorField").toString();if((field=="region"&&problem.isEmpty())||(field=="endpoint"&&endpointProblem.isEmpty())){label->setProperty("errorField",QString{});Ui::feedback(plain(label),{});}}
  if(!problem.isEmpty()||!endpointProblem.isEmpty())return false;}
 inlineError(s3_["region"],problem);inlineError(s3_["endpoint"],endpointProblem);
 if(conflict_)return true;
 const auto message=problem.isEmpty()?endpointProblem:problem;const QString field=problem.isEmpty()?"endpoint":"region";
 for(auto* label:{status_,destinationStatus_}){if(!label)continue;
  if(!message.isEmpty()){plain(label);label->setTextFormat(Qt::PlainText);label->setProperty("errorCode",QStringLiteral("s3_settings_invalid"));label->setProperty("errorField",field);label->setToolTip(QStringLiteral("s3_settings_invalid"));Ui::feedback(label,message,true);}
  else if(QStringList{"region","endpoint"}.contains(label->property("errorField").toString())){label->setProperty("errorField",QString{});Ui::feedback(plain(label),{});}
 }
 return true;
}
// #1675: the refused-file line follows the configuration's validation state alone.
void SettingsWindow::showFileError(){
 const auto error=settings_.metadata()["validation"].toObject()["error"].toObject();const auto code=error["code"].toString();const bool refused=!code.isEmpty()&&code!="config_missing";
 for(auto* line:{fileError_,destinationFileError_})if(line){line->setText(refused?"The configuration file wasn't loaded: "+error["message"].toString():QString());line->setToolTip(refused?code:QString());line->setVisible(refused);}
}
bool SettingsWindow::formOwnsFeedback(QWidget* control)const{
 // Cloudflare R2's fields are the form's too (#1699): updateR2Feedback() alone writes their messages.
 const bool r2=providerType_->currentData()==r2::providerId&&(control==r2AccountId_||control==r2Jurisdiction_||control==s3_.value("bucket")||control==s3_.value("publicUrlBase"));
 return control&&(r2||control==s3_.value("region")||control==s3_.value("endpoint"));}
void SettingsWindow::status(const Error& error){
 // A save or test answer about a Cloudflare R2 field doesn't decide what the form shows (updateR2Feedback).
 if(error.code=="s3_settings_invalid"&&providerType_->currentData()==r2::providerId&&QStringList{"accountId","jurisdiction","bucket","publicUrlBase"}.contains(error.details["field"].toString())){updateR2Feedback();return;}
 // A save or test answer about Region or Endpoint shows nothing: the form's own check, after typing pauses,
 // is the only thing that publishes Region feedback (updateRegionFeedback).
 // It still ends whatever the footer was showing, such as "Testing upload, link and cleanup…".
 if(error.code=="s3_settings_invalid"&&QStringList{"region","endpoint"}.contains(error.details["field"].toString())){
  for(auto* label:{status_,destinationStatus_}){if(!label)continue;
   // Marked with its field, so the form's own check clears it once the field is fixed or another destination shows.
   if(label->property("setupProgress").toBool()){plain(label);label->setProperty("errorCode",error.code);label->setProperty("errorField",error.details["field"].toString());Ui::feedback(label,error.message,true);}
   else if(!QStringList{"region","endpoint"}.contains(label->property("errorField").toString()))Ui::feedback(plain(label),{});}
  return;}
 auto e=error;
 if(e.code=="s3_settings_invalid"&&providerType_->currentData()=="s3"){
  const auto bucket=s3_["bucket"]->text();QString reason;
  if(bucket.isEmpty())reason="Enter a bucket name to finish setting up this destination.";
  else if(bucket.contains('/')||bucket.contains(':')||bucket.contains(' '))reason="Enter only the bucket name, without a URL, folder path or spaces.";
  else if(bucket.toUtf8().size()<3||bucket.toUtf8().size()>63)reason="Bucket names must contain 3–63 characters.";
  if(!reason.isEmpty()){e.message="Invalid S3 settings: "+reason;inlineError(s3_["bucket"],reason);}
  // #1675, #1676: a refused Region or Endpoint is shown at that field.
  else if(const auto field=e.details["field"].toString();s3_.contains(field))inlineError(s3_[field],e.message);
 }
 // People read the message; the code and path go to the tooltip, the
 // accessible description and errorCode, for a report or an agent.
 const auto detail=e?(e.code+(e.details["path"].toString().isEmpty()?QString{}:" · "+e.details["path"].toString())):QString{};
 for(auto* label:{status_,destinationStatus_}){if(!label)continue;label->setProperty("setupProgress",{});label->setProperty("errorCode",e?e.code:QString{});label->setProperty("errorField",e?e.details["field"].toString():QString{});label->setToolTip(detail);label->setAccessibleDescription(detail);}
 if(destinationStatus_)Ui::feedback(destinationStatus_,e.message,bool(e));
 if(e.code=="config_missing"&&settings_.revision().isEmpty()){Ui::feedback(status_,"Changes save automatically when valid.");placeConflictBanners();return;}
 Ui::feedback(status_,e?e.message:"Done.",bool(e));
 placeConflictBanners();
}
// The adapter's reason a capture kind is unavailable, or empty while it is available or unreported (#1295).
static QString captureGap(const QJsonValue& value){const auto status=value.toObject();if(!value.isObject()||status["available"].toBool(true))return {};const auto error=status["error"];const auto message=error.isObject()?error.toObject()["message"].toString():error.toString();return message.isEmpty()?QString("This desktop doesn't provide it."):message;}
// #1364: the inputs to choose from and whether this desktop can highlight clicks.
// Both are asked of the resident; neither opens a device.
void SettingsWindow::showEvent(QShowEvent* event){RetainedWindow::showEvent(event);refreshRecordingInputs(true);}
void SettingsWindow::refreshRecordingInputs(bool force){
 // Every status change comes through here. The sound server is asked only while this page can show
 // the answer and not more often than every ten seconds: each ask runs the recording helper once.
 if(!isVisible())return;
 if(!force&&recordingInputsAsked_.isValid()&&recordingInputsAsked_.elapsed()<recordingInputsEveryMs)return;
 recordingInputsAsked_.start();
 const QString device=recordingMicrophone_->currentData().toString();
 QJsonObject params;if(!device.isEmpty())params["device"]=device;
 settings_.request("recording.microphones",params,this,[g=QPointer<SettingsWindow>(this)](Error error,QJsonObject microphones){if(g&&!error)g->showRecordingMicrophones(microphones);});
 // What the desktop offers for click highlights does not change while Settings is open; its capability
 // answer also asks the sound server, so it is fetched once.
 if(std::exchange(clickHighlightsAsked_,true))return;
 settings_.request("recording.capabilities",{},this,[g=QPointer<SettingsWindow>(this)](Error error,QJsonObject capabilities){if(!g)return;if(error){g->clickHighlightsAsked_=false;return;}g->showClickHighlights(capabilities["clickHighlights"].toObject());});
}
void SettingsWindow::showRecordingMicrophones(const QJsonObject& microphones){
 const QString chosen=recordingMicrophone_->currentData().toString();
 // The same devices as last time (and the choice still among them): the list stays as it is, open or not.
 if(const auto devices=microphones["devices"].toArray();!std::exchange(recordingDevicesShown_,true)||devices!=recordingDevices_||recordingMicrophone_->findData(chosen)<0){
  recordingDevices_=devices;
  const bool wasLoading=std::exchange(loading_,true);
  recordingMicrophone_->clear();recordingMicrophone_->addItem("System default",QString());
  bool listed=chosen.isEmpty();
  for(const auto& value:devices){const auto device=value.toObject();const auto id=device["id"].toString();if(id.isEmpty())continue;recordingMicrophone_->addItem(device["label"].toString(id),id);recordingMicrophone_->setItemData(recordingMicrophone_->count()-1,id,Qt::ToolTipRole);listed=listed||id==chosen;}
  if(!listed)recordingMicrophone_->addItem(chosen+" (not connected)",chosen);
  recordingMicrophone_->setCurrentIndex(std::max(0,recordingMicrophone_->findData(chosen)));
  loading_=wasLoading;
 }
 // What the bar's toggle would do right now with this choice.
 const bool available=microphones["available"].toBool();
 recordingMicrophoneStatus_->setText(available?QStringLiteral("Records from %1.").arg(microphones["device"].toObject()["label"].toString()):QStringLiteral("No microphone available: %1").arg(microphones["message"].toString("no input device was found.").replace(QStringLiteral("choose a device in Settings"),QStringLiteral("choose a device above"))));
 recordingMicrophoneStatus_->setProperty("microphoneAvailable",available);
 recordingMicrophoneStatus_->show();
}
void SettingsWindow::showClickHighlights(const QJsonObject& capability){
 const bool available=capability["available"].toBool();
 const QString route=capability["route"].toString();
 const QString how=route=="kwin-mouseclick-effect"?QStringLiteral("drawn by KWin's Mouse Click Animation effect, switched on while recording"):route=="x11-xinput2"?QStringLiteral("drawn by put.gd from the X server's button events"):route=="gnome-shell-extension"?QStringLiteral("drawn by put.gd from the Shell extension's click reports"):route=="hyprland-mouse-binds"?QStringLiteral("drawn by put.gd from Hyprland mouse binds that pass the click on"):route=="labwc-mouse-binds"?QStringLiteral("drawn by put.gd from labwc mouse binds added while recording, for clicks on windows"):QStringLiteral("drawn by put.gd");
 QString text=available?"Available · "+how:"Unavailable · "+capability["message"].toString("this desktop reports no pointer clicks");
 // A kind of recording this desktop leaves out is named here with the adapter's reason, not left to be found at Record.
 const auto targets=capability["targets"].toArray();
 if(available&&!targets.isEmpty()&&!targets.contains("window"))text+=QStringLiteral("\nNot in window recordings: %1").arg(capability["targetReasons"].toObject()["window"].toString(QStringLiteral("this desktop gives no clicks for a window recording.")));
 if(auto* value=capabilities_->findChild<QLabel*>("desktopSupportClickHighlights")){value->setText(text);value->show();if(auto* label=capabilities_->findChild<QLabel*>("desktopSupportClickHighlightsLabel"))label->show();}
 recordingClicks_->setEnabled(available||recordingClicks_->isChecked());
 recordingClicksStatus_->setText(available?QString{}:QStringLiteral("Not available on this desktop: %1").arg(capability["message"].toString("it reports no pointer clicks.")));
 recordingClicksStatus_->setVisible(!available);
 recordingClicks_->setProperty("clickHighlightsAvailable",available);
}
void SettingsWindow::showTextLanguages(){
 if(textLanguages_.isEmpty())return;
 textCapabilities_=settings_.diagnostics()["capabilities"].toObject()["text"].toObject();
 QList<textjob::Language> installed;for(const auto& value:textCapabilities_["languages"].toArray()){const auto language=value.toObject();installed.append({language["id"].toString(),language["kind"].toString(),{},language["bytes"].toInteger()});}
 // "German (deu)"; the identifier alone when Qt does not know the code.
 const auto name=[](const QString& id){const auto language=QLocale::codeToLanguage(id.section('_',0,0));return language==QLocale::AnyLanguage||language==QLocale::C?id:QLocale::languageToString(language)+" ("+id+")";};
 const auto listed=[&installed](const QString& id){return std::any_of(installed.begin(),installed.end(),[&](const textjob::Language& language){return language.id==id;});};
 QStringList automatic;for(const auto& id:textjob::defaultLanguages({},QLocale::system().name(),installed))automatic<<name(id);
 QStringList saved;
 if(loading_)saved=Settings::fromValidated(draft_).textRecognitionLanguages;
 else for(auto* combo:std::as_const(textLanguages_))saved.append(combo->currentData().toString());
 QStringList missing;
 for(qsizetype i=0;i<textLanguages_.size();++i){
  auto* combo=textLanguages_[i];const QSignalBlocker block(combo);combo->clear();
  combo->addItem(i==0?"Automatic: "+automatic.join(", then "):QString("None"),QString());
  for(const auto& language:std::as_const(installed))combo->addItem(name(language.id),language.id);
  // A saved language whose data went away stays chosen, and says so.
  const auto chosen=saved.value(i);if(!chosen.isEmpty()&&!listed(chosen)){combo->addItem(name(chosen)+" (not installed)",chosen);missing<<name(chosen);}
  combo->setCurrentIndex(qMax(0,combo->findData(chosen)));combo->setEnabled(i==0||!saved.value(0).isEmpty());
 }
 const bool unavailable=textCapabilities_.contains("available")&&!textCapabilities_["available"].toBool();
 textRecognitionStatus_->setText(unavailable?"Text recognition is unavailable. "+textCapabilities_["error"].toObject()["message"].toString()
  :!missing.isEmpty()?missing.join(", ")+(missing.size()==1?" data is not installed, so that language is skipped.":" data is not installed, so those languages are skipped.")
  :QString("More languages come from your distribution's Tesseract language packages, or from .traineddata files in ~/.local/share/putgd/tessdata."));
 textRecognitionStatus_->setProperty("textRecognitionUnavailable",unavailable);
}
void SettingsWindow::refreshDiagnostics(){
 refreshRecordingInputs();
 // The installed languages or the engine's state changed while Settings is open.
 if(!loading_&&settings_.diagnostics()["capabilities"].toObject()["text"].toObject()!=textCapabilities_)showTextLanguages();
 refreshLoggingStatus();refreshUpdateStatus();const auto data=settings_.diagnostics();const auto source=data["capabilities"].toObject()["source"].toObject();const auto capture=source["capture"].toObject();const auto shortcuts=data.contains("shortcuts")?data["shortcuts"].toObject():source["shortcuts"].toObject();
 const auto availability=[](const QJsonValue& value){const auto state=value.isObject()?value.toObject().value("available"):value;return state.isBool()?QString(state.toBool()?"Available":"Unavailable"):QString("Not reported");};
 const auto set=[this](const QString& key,const QString& value){capabilities_->findChild<QLabel*>("desktopSupport"+title(key))->setText(value);};
 const auto adapter=source["adapter"].toString();const QHash<QString,QString> adapters{{"gnome-wayland","GNOME · Wayland"},{"kde-wayland","KDE Plasma · Wayland"},{"budgie-wayland","Budgie · Wayland"},{"wayland-data-control","Wayland · Limited"},{"x11","X11"},{"x11-core","X11"},{"cinnamon-x11","Cinnamon · X11"},{"cinnamon-core-x11","Cinnamon · X11"},{"portal","Desktop portal"}};set("integration",adapters.value(adapter,adapter.isEmpty()?QString("Not reported"):adapter));for(const auto& action:QStringList{"region","window","desktop"})set(action,availability(capture.contains(action)?capture[action]:source[action]));QString shortcutState=availability(shortcuts);if(shortcuts["available"].toBool())for(const auto& binding:shortcuts["bindings"].toObject())if(binding.toObject()["state"]=="pending"){shortcutState="Waiting for confirmation";break;}set("shortcuts",shortcutState);set("tray",availability(data["trayAvailable"]));shortcuts_->setStatus(shortcuts);
 for(const auto& kind:QStringList{"window","desktop","monitor","delay"}){const auto reason=captureGap(capture.contains(kind)?capture[kind]:source[kind]);auto* label=capabilities_->findChild<QLabel*>("desktopSupport"+title(kind)+"GapLabel");auto* value=capabilities_->findChild<QLabel*>("desktopSupport"+title(kind)+"Gap");value->setText(reason.isEmpty()?QString{}:"Unavailable · "+reason);label->setVisible(!reason.isEmpty());value->setVisible(!reason.isEmpty());}
}

void SettingsWindow::refresh(bool preserveDrafts){
 if(!preserveDrafts){serviceSwitched_.clear();r2BucketFrom_.clear();}
 if(preserveDrafts)rememberCredentialInputs();else{clearCredentialInputs();profileEditors_={};provisionalProfile_={};}
 autosaveBlocked_=false;restoredDraft_=false;restoredBase_={};for(auto* button:std::as_const(retryAutosave_))button->hide();reorderPending_=false;
 loading_=true;draft_=settings_.document().isEmpty()?Settings::defaults():settings_.document();baseDocument_=draft_;baseRevision_=settings_.revision();observed_=settings_.metadata()["observedRevision"].toString();dirty_=conflict_=false;refreshMetadata();
 Ui::feedback(plain(status_),{});plain(destinationStatus_);showConflictActions();
 // Destinations are created only by an explicit Add action. Keep the legacy
 // first-launch draft flag only for restoring an older UI checkpoint.
 initialProfile_=false;
 populate();for(auto* field:errors_.keys())inlineError(field,{});showS3SettingsProblems();refreshDestinationList();refreshCredentialHint();
}
void SettingsWindow::populate(){
 loading_=true;const auto profiles=draft_["profiles"].toArray();const auto actions=draft_["actions"].toObject();
 auto selected=profile_->currentData().toString();profileIndex_=-1;profile_->clear();for(auto p:profiles)profile_->addItem(p.toObject()["label"].toString(),p.toObject()["id"].toString());int selectedIndex=profile_->findData(selected);if(selectedIndex>=0)profile_->setCurrentIndex(selectedIndex);loadProfile();
 captureDefaultsEnabled_=draft_.contains("captureDefaults");captureRoutingEdited_=false;recordingRoutingEdited_=false;
 auto base=captureDefaultsEnabled_?draft_["captureDefaults"].toObject():Settings::effectiveAction(draft_,"region");
 for(const auto& name:Settings::presentationOrder()){
  const auto effective=Settings::effectiveAction(draft_,name);const auto raw=actions[name].toObject();
  bool inherit=name=="record"?!raw.contains("destination"):captureDefaultsEnabled_?raw["destination"]=="default":effective["destination"]==base["destination"]&&effective["profileId"]==base["profileId"]&&effective["publicationMode"].toString("default")==base["publicationMode"].toString("default");
  useCaptureDefault_[name]->setChecked(inherit);
  publicationModes_[name]->setCurrentIndex(publicationModes_[name]->findData(effective["publicationMode"].toString("default")));auto* c=actionProfiles_[name];c->clear();c->addItem("No destination",QString{});for(auto p:profiles)c->addItem(p.toObject()["label"].toString(),p.toObject()["id"].toString());c->setCurrentIndex(c->findData(effective["destination"]=="local"?QString{}:effective["profileId"].toString()));
 }
 captureProfile_->setCurrentIndex(captureProfile_->findData(base["destination"]=="local"?QString{}:base["profileId"].toString()));captureSharing_->setCurrentIndex(captureSharing_->findData(base["publicationMode"].toString("default")));
 bool exceptions=false;for(auto* inherit:std::as_const(useCaptureDefault_))exceptions|=!inherit->isChecked();captureExceptions_->setChecked(exceptions);
 shortcuts_->setActions(actions);auto archive=draft_["archive"].toObject();root_->setText(archive["root"].toString());keep_->setChecked(archive["keepUploads"].toBool());for(auto it=folders_.begin();it!=folders_.end();++it)it.value()->setText(archive["folders"].toObject()[it.key()].toString());if(auto* toggle=findChild<QToolButton*>("categoryFoldersToggle")){bool overrides=false;for(auto* field:std::as_const(folders_))overrides|=!field->text().isEmpty();toggle->setChecked(overrides);}
 naming_->setCurrentIndex(naming_->findData(draft_["naming"].toObject()["mode"].toString()));const auto effective=Settings::fromValidated(draft_);const QHash<QString,QString> formats{{"fileTemplate",effective.fileTemplate},{"captureTemplate",effective.captureTemplate},{"imageTemplate",effective.imageTemplate},{"textTemplate",effective.textTemplate}};for(auto it=templates_.begin();it!=templates_.end();++it)it.value()->setText(formats.value(it.key()));collision_->setCurrentIndex(collision_->findData(draft_["naming"].toObject()["collisionPolicy"].toString()));randomStyle_->setCurrentIndex(randomStyle_->findData(effective.randomName.style));randomLength_->setValue(effective.randomName.length);randomUppercase_->setChecked(effective.randomName.uppercase);randomDigits_->setChecked(effective.randomName.digits);showRandomNameRows();auto prefs=draft_["preferences"].toObject();uploadClipboard_->setCurrentIndex(uploadClipboard_->findData(effective.uploadClipboardBehavior));showTextLanguages();{const auto joined=[](const QStringList& lines){return lines.join(QChar(0x0A));};quickRedactTerms_->setPlainText(joined(effective.quickRedactTerms));quickRedactPatterns_->setPlainText(joined(effective.quickRedactPatterns));}regionSelection_->setCurrentIndex(regionSelection_->findData(effective.regionSelectionMode));captureDelay_->setCurrentIndex(captureDelay_->findData(effective.captureDelaySeconds));captureDelayShortcuts_->setChecked(effective.captureDelayShortcuts);fileManagerFolders_->setCurrentIndex(fileManagerFolders_->findData(effective.fileManagerFolders));fileManagerMenu_->setChecked(effective.fileManagerMenu||!fileManagerMenu_->isEnabled());recordingFrameRate_->setCurrentIndex(recordingFrameRate_->findData(effective.recordingFrameRate));{int chosen=recordingMicrophone_->findData(effective.recordingMicrophoneDevice);if(chosen<0){recordingMicrophone_->addItem(effective.recordingMicrophoneDevice+" (not connected)",effective.recordingMicrophoneDevice);chosen=recordingMicrophone_->count()-1;}recordingMicrophone_->setCurrentIndex(chosen);}scrollHeight_->setValue(effective.scrollCaptureMaxHeight);scrollFromTop_->setChecked(effective.scrollCaptureFromTop);scrollAutoScroll_->setChecked(effective.scrollCaptureAutoScroll);recordingClicks_->setChecked(effective.recordingClickHighlights);appearance_->setCurrentIndex(appearance_->findData(effective.appearanceTheme));notifications_->setChecked(prefs["notifications"].toBool());login_->setChecked(prefs["launchAtLogin"].toBool());debugLogging_->setChecked(prefs["diagnostics"].toObject()["debugLogging"].toBool());unlockAtStartup_->setChecked(effective.unlockCredentialsAtStartup);checkUpdates_->setChecked(effective.checkForUpdates);updateChannelSetting_=effective.updateChannel;updateChannelShown_.clear();showUpdateChannel();
 for(auto it=numbers_.begin();it!=numbers_.end();++it){QJsonValue value=draft_["limits"].toObject().contains(it.key())?draft_["limits"].toObject().value(it.key()):draft_["retention"].toObject().value(it.key());if(it.key()=="pendingImageBytes"&&value.isUndefined())value=effective.limits.pendingImageBytes;it.value()->setText(value.isNull()?QString{}:value.isString()?QStringLiteral("Auto"):ByteSize::exact(value.toInteger(),displayUnit(it.key())));}
 updateCaptureInheritance();loading_=false;updateResourceLimits();
}
void SettingsWindow::refreshEffectiveLimits(){
 settings_.request("settings.get",{},this,[this](Error error,QJsonObject data){if(error)return;effectiveLimits_=data["effectiveLimits"].toObject();updateLimitHints();});
}
// "No limit on this computer · keeps 4 GB free" below a field set to Auto: none of put.gd's own,
// with the free-disk reserve that bounds every import.
void SettingsWindow::updateResourceLimits(){
 if(!resourceLimitsToggle_||loading_)return;for(auto it=numbers_.begin();it!=numbers_.end();++it)if(!Settings::defaults()["retention"].toObject().contains(it.key())&&it.value()->property("invalid").toBool()){resourceLimitsToggle_->setChecked(true);break;}
}
void SettingsWindow::updateLimitHints(){
 const auto disk=effectiveLimits_["disk"].toObject();const auto reserve=disk["reserveBytes"].toInteger(-1);
 bool shared=true;const auto first=effectiveLimits_.value("maxFileBytes");shared=!first.isUndefined();for(const auto& key:Settings::artifactByteLimitKeys())shared&=numbers_.value(key)->text().trimmed().compare("auto",Qt::CaseInsensitive)==0&&effectiveLimits_.value(key)==first;
 uploadReserveHint_->setText(shared?(first.isNull()?QString("No limit on this computer"):first.isDouble()?ByteSize::summary(first.toInteger())+" on this computer":QString{})+(reserve>0?" · keeps "+ByteSize::summary(reserve)+" free":QString{}):QString{});uploadReserveHint_->setVisible(!uploadReserveHint_->text().isEmpty());
 for(auto it=limitHints_.begin();it!=limitHints_.end();++it){
  const bool automatic=numbers_.value(it.key())->text().trimmed().compare("auto",Qt::CaseInsensitive)==0;const auto value=effectiveLimits_.value(it.key());
  QString text;
  if(automatic&&value.isNull())text="No limit on this computer"+(reserve>0?" · keeps "+ByteSize::summary(reserve)+" free":QString{});
  else if(automatic&&value.isDouble())text=ByteSize::summary(value.toInteger())+" on this computer";
  it.value()->setText(text);it.value()->setVisible(!shared&&!text.isEmpty());
  it.value()->setToolTip(text.isEmpty()?QString{}:QString("On this computer, Auto sets no size limit of put.gd's own. Each upload still needs room on the disk: put.gd keeps %1 free (5% of the disk, between 1 and 4 GB), and the destination's own maximum object size applies.").arg(ByteSize::summary(qMax<qint64>(reserve,0))));
 }
}
void SettingsWindow::addS3Profile(const QString& defaultLabel,const QString& name,const QString& provider){
 flushTyping();storeProfile();draft_=candidate();auto profiles=draft_["profiles"].toArray();if(profiles.size()>=32){status({"profile_limit","At most 32 destinations are supported",{}});return;}
 bool accepted=false;// An agent names the destination itself (ui.r2Destination add); a person types it in the dialog.
 const auto destinationName=name.isEmpty()?QInputDialog::getText(this,"Add destination","Destination name",QLineEdit::Normal,defaultLabel,&accepted).trimmed():(accepted=true,name.trimmed());if(!accepted||destinationName.isEmpty())return;
 QJsonObject restored=provisionalProfile_.value("profile").toObject();
 QString id=restored.value("id").toString();int number=1;
 // #1704: a new ID is one neither this window nor the file has, so a destination being removed
 // keeps its own and the new one is an add.
 const auto takenId=[&](const QString& candidateId){return acceptedHas(candidateId)||std::any_of(profiles.begin(),profiles.end(),[&](const QJsonValue& p){return p.toObject()["id"].toString()==candidateId;});};
 if(id.isEmpty()||takenId(id))do{auto slug=destinationName.toLower();static const QRegularExpression runOfOthers("[^a-z0-9]+");slug.replace(runOfOthers,"-");slug=slug.left(48);if(slug.isEmpty()||!slug[0].isLetterOrNumber())slug="destination";id=slug+(number==1?QString{}:"-"+QString::number(number));++number;}while(takenId(id));
 while(profileEditors_.size()<profiles.size())profileEditors_.append(QJsonValue::Null);
 credentialDrafts_.resize(profiles.size()+1);
 // A Cloudflare R2 destination starts as one (#1699), never as an S3 draft that is then switched.
 if(restored.isEmpty()){auto profile=s3Profile(id);if(!provider.isEmpty()){const auto defaults=destinationDefaults(provider,id);profile["provider"]=provider;profile["settings"]=defaults["settings"];profile["credentials"]=defaults["credentials"];}profile["label"]=destinationName;profiles.append(profile);profileEditors_.append(QJsonValue::Null);}
 else{
  restored["id"]=id;profiles.append(restored);auto editor=provisionalProfile_["editor"].toObject();editor["id"]=id;profileEditors_.append(editor);
  credentialDrafts_.last()=std::move(provisionalCredentials_);provisionalProfile_={};provisionalCredentials_.clear();
 }
 draft_["profiles"]=profiles;initialProfile_=false;rebuildProfiles(profiles.size()-1);
 dirty();queueWholeDestination(id,defaultButton_);refreshDestinationList();
}
void SettingsWindow::resetDraft(bool reloadFile){
 if(resetting_||saving_||setupAccepting_)return;
 typingPause_->stop();typingControl_=nullptr;saveQueue_.clear();notSaved_.clear();clearCredentialInputs();resetting_=true;setEnabled(false);destinationPage_->setEnabled(false);emit viewStateChanged();
 endAppearancePreview();
 const auto finish=[g=QPointer<SettingsWindow>(this),reloadFile](Error error){
  if(!g)return;
  g->resetting_=false;g->setEnabled(true);g->destinationPage_->setEnabled(true);
  if(error){g->configurationChanged();g->status(error);emit g->viewStateChanged();return;}
  g->refresh();emit g->viewStateChanged();
  if(reloadFile)Ui::feedback(plain(g->status_),"Settings reloaded from file.");else g->close();
 };
 if(reloadFile)settings_.reloadAsync([finish](Result result){finish(result.error);});
 else settings_.refreshSnapshot(finish);
}

void SettingsWindow::rebuildProfiles(int selected,const QString& defaultProfile,bool keepEditor){const auto captureProfile=captureProfile_->currentData().toString();const bool previous=loading_;loading_=true;const auto profiles=draft_["profiles"].toArray();profileIndex_=-1;profile_->clear();for(auto p:profiles)profile_->addItem(p.toObject()["label"].toString(),p.toObject()["id"].toString());profile_->setCurrentIndex(selected);if(keepEditor&&selected>=0&&selected<profiles.size()&&profiles[selected].toObject()["id"].toString()==profileId_->text())profileIndex_=selected;else loadProfile();for(auto* combo:actionProfiles_){auto id=combo->currentData().toString();combo->clear();combo->addItem("No destination",QString{});for(auto p:profiles)combo->addItem(p.toObject()["label"].toString(),p.toObject()["id"].toString());int index=combo->findData(id.isEmpty()?defaultProfile:id);combo->setCurrentIndex(index<0?0:index);}captureProfile_->setCurrentIndex(captureProfile_->findData(captureProfile.isEmpty()?defaultProfile:captureProfile));if(!defaultProfile.isEmpty()){captureDefaultsEnabled_=true;captureRoutingEdited_=true;}updateCaptureInheritance();loading_=previous;}
void SettingsWindow::loadProfile(){
 const auto shownProfile=profileId_->text();
 credentialStorageNotice_->hide();if(destinationStatus_&&!conflict_)Ui::feedback(plain(destinationStatus_),{});bool previous=loading_;loading_=true;profileIndex_=profile_->currentIndex();auto profiles=draft_["profiles"].toArray();auto p=profileIndex_>=0&&profileIndex_<profiles.size()?profiles[profileIndex_].toObject():QJsonObject{};profileId_->setText(p["id"].toString());profileLabel_->setText(p["label"].toString());const auto s=p["settings"].toObject();bool s3=p["provider"]=="s3";const bool b2=p["provider"]=="backblaze-b2";const bool r2=p["provider"]==r2::providerId;const bool storage=s3||b2||r2;const bool http=p["provider"]==CustomUploaderPanel::provider();
 // The form shows this destination's request; another destination's notice does not follow it.
 uploader_->setSettings(http?s:CustomUploaderPanel::defaultSettings());if(shownProfile!=p["id"].toString()){uploader_->setNotice({});s3OtherService_={};}
 providerType_->setCurrentIndex(providerType_->findData(p["provider"].toString()));const bool proton=p["provider"]=="proton-drive";protonPanel_->setVisible(proton);protonAccountUid_=s["accountRootUid"].toString();protonAccountEmail_.clear();protonShare_->setChecked(s["shareByDefault"].toBool(true));protonExpiration_->setValue(s["linkExpirationDays"].toInt());protonTimeout_->setValue(s["requestTimeoutMs"].toInt(120000)/1000.0);protonFolder_->clear();addProtonFolder(protonFolder_,s["folderUid"].toString(),s["folderName"].toString());
 findControl<QPushButton*>("storeCredentials")->setVisible(storage||http);findControl<QPushButton*>("testDestination")->setVisible(storage||proton||http);access_->setVisible(storage);secret_->setVisible(storage);token_->setVisible(s3);for(auto it=s3_.begin();it!=s3_.end();++it){auto v=s[it.key()];setInputText(it.value(),v.isDouble()?ByteSize::exact(v.toInteger(),connectionUnit(it.key())):v.toString());allowControl(it.value(),s3||(b2&&b2SharedKeys().contains(it.key()))||(r2&&r2::settingKeys().contains(it.key())));}allowControl(b2LinkMode_,storage);allowControl(b2LinkDays_,storage);b2LinkMode_->setItemData(1,s3||r2?"presigned":"authorized");b2LinkMode_->setCurrentIndex(qMax(0,b2LinkMode_->findData(s["linkMode"].toString("public"))));b2LinkDays_->setValue(s["linkValidityDays"].toInt(b2::maximumLinkDays));s3_["region"]->setProperty("previousRegion",s["region"].toString());allowControl(regionChoices_,s3);allowControl(r2AccountId_,r2);allowControl(r2Jurisdiction_,r2);r2AccountId_->setText(r2?s["accountId"].toString():QString());if(r2&&!r2BucketFrom_.contains(p["id"].toString()))r2BucketFrom_[p["id"].toString()]=s["accountId"].toString()+"|"+s["jurisdiction"].toString("default");r2Jurisdiction_->setCurrentIndex(qMax(0,r2Jurisdiction_->findData(s["jurisdiction"].toString("default"))));allowControl(acl_,s3);allowControl(addressing_,s3);acl_->setCurrentIndex(acl_->findData(s["aclMode"].toString()));addressing_->setCurrentIndex(addressing_->findData(s["addressing"].toString()));allowControl(providerJson_,!storage);providerJson_->setPlainText(proton||b2||r2||http?QString{}:QString::fromUtf8(QJsonDocument(s).toJson(QJsonDocument::Indented)));auto ref=p["credentials"].toObject();credentialMode_->setCurrentIndex(credentialMode_->findData(ref["mode"].toString()));reference_->setText(ref["reference"].toString());if(profileIndex_>=0&&profileIndex_<profileEditors_.size()&&profileEditors_[profileIndex_].toObject().contains("id"))applyProfileEditor(profileEditors_[profileIndex_].toObject());restoreCredentialInputs();if(updateProviderVisibility_)updateProviderVisibility_();showS3SettingsProblems();loading_=previous;updateProtonAccountDisplay();updateSetupButton();refreshDestinationList();refreshCredentialHint();}
QJsonObject SettingsWindow::editedProfile()const{auto profiles=draft_["profiles"].toArray();if(profileIndex_<0||profileIndex_>=profiles.size())return {};auto p=profiles[profileIndex_].toObject();p["id"]=profileId_->text();p["label"]=profileLabel_->text();auto s=p["settings"].toObject();if(p["provider"]=="s3"){for(auto it=s3_.begin();it!=s3_.end();++it){if(it.key().endsWith("Ms")||it.key().startsWith("lowSpeed")){auto value=connectionValue(it.key(),inputText(it.value()));s[it.key()]=value;}else if(it.key()=="publicUrlBase"&&it.value()->text().isEmpty())s[it.key()]=QJsonValue::Null;else s[it.key()]=it.value()->text();}const auto linkMode=b2LinkMode_->currentData().toString();if(s.contains("linkMode")||linkMode!="public")s["linkMode"]=linkMode;if(s.contains("linkValidityDays")||linkMode!="public"||b2LinkDays_->value()!=7)s["linkValidityDays"]=b2LinkDays_->value();s["aclMode"]=acl_->currentData().toString();s["addressing"]=addressing_->currentData().toString();}else if(p["provider"]=="proton-drive"){s["accountRootUid"]=protonAccountUid_;s["folderUid"]=protonFolder_->currentData().toString();const auto folderName=protonFolder_->currentData(protonFolderNameRole).toString();if(folderName.isEmpty())s.remove("folderName");else s["folderName"]=folderName;s["shareByDefault"]=protonShare_->isChecked();s["linkExpirationDays"]=protonExpiration_->hasAcceptableInput()?QJsonValue(protonExpiration_->value()):QJsonValue(protonExpiration_->text());s["requestTimeoutMs"]=protonTimeout_->hasAcceptableInput()?QJsonValue(qRound64(protonTimeout_->value()*1000)):QJsonValue(protonTimeout_->text());}else if(p["provider"]==r2::providerId){
  // #1678: what the R2 form shows; the endpoint, region and addressing come from r2::s3Settings().
  QJsonObject r2;for(const auto& key:r2::settingKeys())if(s3_.contains(key)){auto* field=s3_.value(key);if(key.endsWith("Ms")||key.startsWith("lowSpeed"))r2[key]=connectionValue(key,inputText(field));else if(key=="publicUrlBase"&&field->text().isEmpty())r2[key]=QJsonValue::Null;else r2[key]=field->text();}
  r2["accountId"]=r2AccountId_->text();r2["jurisdiction"]=r2Jurisdiction_->currentData().toString();r2["linkMode"]=b2LinkMode_->currentData().toString();r2["linkValidityDays"]=b2LinkDays_->value();s=r2;
 }else if(p["provider"]=="backblaze-b2"){for(const auto& key:b2SharedKeys()){auto* field=s3_.value(key);if(key.endsWith("Ms"))s[key]=ByteSize::fromDecimal(inputText(field),1000);else if(key=="publicUrlBase"&&field->text().isEmpty())s[key]=QJsonValue::Null;else s[key]=field->text();}s["linkMode"]=b2LinkMode_->currentData().toString();s["linkValidityDays"]=b2LinkDays_->value();if(s["linkMode"]=="authorized")s["publicUrlBase"]=QJsonValue::Null;}else if(p["provider"]==CustomUploaderPanel::provider()){s=uploader_->settings();}else {auto parsed=QJsonDocument::fromJson(providerJson_->toPlainText().toUtf8());if(parsed.isObject())s=parsed.object();else s={{"invalidJson",true}};}p["settings"]=s;p["credentials"]=QJsonObject{{"mode",credentialMode_->currentData().toString()},{"reference",credentialMode_->currentData().toString()=="session"?QJsonValue(QJsonValue::Null):QJsonValue(reference_->text())}};return p;}
void SettingsWindow::storeProfile(){
 auto profiles=draft_["profiles"].toArray();if(profileIndex_<0||profileIndex_>=profiles.size())return;
 while(profileEditors_.size()<profiles.size())profileEditors_.append(QJsonValue::Null);
 rememberCredentialInputs();profileEditors_[profileIndex_]=retainedProfileEditor();
 const auto p=editedProfile();
 for(auto* combo:std::as_const(actionProfiles_)){const int index=profileIndex_+1;if(index<combo->count()){combo->setItemData(index,p["id"].toString());combo->setItemText(index,p["label"].toString());}}
 profile_->setItemText(profileIndex_,p["label"].toString());profile_->setItemData(profileIndex_,p["id"].toString());
 profiles[profileIndex_]=p;draft_["profiles"]=profiles;
}
QJsonObject SettingsWindow::candidate() const {
 auto doc=draft_;const auto effective=Settings::fromValidated(doc);auto profiles=doc["profiles"].toArray();
 if(profileIndex_>=0&&profileIndex_<profiles.size())profiles[profileIndex_]=editedProfile();
 doc["profiles"]=profiles;
 auto actions=shortcuts_->actions(doc["actions"].toObject());
 for(const auto& name:Settings::presentationOrder()){
  auto action=actions[name].toObject();
  if(captureRoutingEdited_&&(name!="record"||recordingRoutingEdited_)){
   if(publicationModes_[name]->currentData()!="default"||action.contains("publicationMode"))action["publicationMode"]=publicationModes_[name]->currentData().toString();
   auto id=actionProfiles_[name]->currentData().toString();if(profileIndex_>=0&&profileIndex_<draft_["profiles"].toArray().size()&&id==draft_["profiles"].toArray()[profileIndex_].toObject()["id"].toString())id=profileId_->text();
   action["destination"]=id.isEmpty()?"local":"upload";action["profileId"]=id.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(id);
   if(useCaptureDefault_[name]->isChecked()){if(name=="record"){for(const auto& key:QStringList{"destination","profileId","publicationMode"})action.remove(key);}else if(captureDefaultsEnabled_){action["destination"]="default";action["profileId"]=QJsonValue::Null;if(action.contains("publicationMode"))action["publicationMode"]="default";}}
  }
  actions[name]=action;
 }
 if(captureRoutingEdited_)doc["captureDefaults"]=QJsonObject{{"destination",captureProfile_->currentData().toString().isEmpty()?"local":"upload"},{"profileId",captureProfile_->currentData().toString().isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(captureProfile_->currentData().toString())},{"publicationMode",captureSharing_->currentData().toString()}};
 doc["actions"]=actions;auto archive=doc["archive"].toObject();archive["root"]=root_->text();archive["keepUploads"]=keep_->isChecked();QJsonObject folders;
 for(auto it=folders_.begin();it!=folders_.end();++it)folders[it.key()]=it.value()->text().isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(it.value()->text());
 archive["folders"]=folders;doc["archive"]=archive;auto naming=doc["naming"].toObject();naming["mode"]=naming_->currentData().toString();naming["collisionPolicy"]=collision_->currentData().toString();
 {const RandomNameSettings standard;if(naming.contains("randomStyle")||randomStyle_->currentData()!=standard.style)naming["randomStyle"]=randomStyle_->currentData().toString();if(naming.contains("randomLength")||randomLength_->value()!=standard.length)naming["randomLength"]=randomLength_->value();if(naming.contains("randomUppercase")||randomUppercase_->isChecked()!=standard.uppercase)naming["randomUppercase"]=randomUppercase_->isChecked();if(naming.contains("randomDigits")||randomDigits_->isChecked()!=standard.digits)naming["randomDigits"]=randomDigits_->isChecked();}
 const QHash<QString,QString> defaultTemplates{{"fileTemplate",effective.fileTemplate},{"captureTemplate",effective.captureTemplate},{"imageTemplate",effective.imageTemplate},{"textTemplate",effective.textTemplate}};
 for(auto it=templates_.begin();it!=templates_.end();++it)if(naming.contains(it.key())||it.value()->text()!=defaultTemplates.value(it.key()))naming[it.key()]=it.value()->text();
 doc["naming"]=naming;auto preferences=doc["preferences"].toObject();preferences["notifications"]=notifications_->isChecked();preferences["launchAtLogin"]=login_->isChecked();if(preferences.contains("unlockCredentialsAtStartup")||!unlockAtStartup_->isChecked())preferences["unlockCredentialsAtStartup"]=unlockAtStartup_->isChecked();
 if(preferences.contains("uploadClipboardBehavior")||uploadClipboard_->currentData()!="each_link")preferences["uploadClipboardBehavior"]=uploadClipboard_->currentData().toString();
 // An untouched config keeps no regionSelectionMode; it reads as "live".
 if(preferences.contains("regionSelectionMode")||regionSelection_->currentData()!="live")preferences["regionSelectionMode"]=regionSelection_->currentData().toString();
 // Both are absent from an untouched config; the menu switch is the AppImage's alone.
 if(preferences.contains("fileManagerFolders")||fileManagerFolders_->currentData()!="zip")preferences["fileManagerFolders"]=fileManagerFolders_->currentData().toString();
 if(fileManagerMenu_->isEnabled()&&(preferences.contains("fileManagerMenu")||!fileManagerMenu_->isChecked()))preferences["fileManagerMenu"]=fileManagerMenu_->isChecked();
 // An untouched config keeps neither capture delay key: no delay, shortcuts immediate.
 if(preferences.contains("captureDelaySeconds")||captureDelay_->currentData().toInt()!=0)preferences["captureDelaySeconds"]=captureDelay_->currentData().toInt();
 if(preferences.contains("captureDelayShortcuts")||captureDelayShortcuts_->isChecked())preferences["captureDelayShortcuts"]=captureDelayShortcuts_->isChecked();
 // A configuration from before scroll capture keeps neither key; each reads as its default. Writing
 // one regardless would leave the draft different from the saved file after every save, which then
 // reads as unsaved and refuses every action.
 if(preferences.contains("scrollCaptureMaxHeight")||scrollHeight_->value()!=effective.scrollCaptureMaxHeight)preferences["scrollCaptureMaxHeight"]=scrollHeight_->value();
 if(preferences.contains("scrollCaptureFromTop")||scrollFromTop_->isChecked()!=effective.scrollCaptureFromTop)preferences["scrollCaptureFromTop"]=scrollFromTop_->isChecked();
 if(preferences.contains("scrollCaptureAutoScroll")||scrollAutoScroll_->isChecked())preferences["scrollCaptureAutoScroll"]=scrollAutoScroll_->isChecked();
 // An untouched config keeps no recordingFrameRate; it reads as 30.
 if(preferences.contains("recordingFrameRate")||recordingFrameRate_->currentData().toInt()!=30)preferences["recordingFrameRate"]=recordingFrameRate_->currentData().toInt();
 // #1428: Automatic keeps no key; otherwise the chosen languages in order, each once.
 {QJsonArray languages;for(auto* combo:textLanguages_){const auto id=combo->currentData().toString();if(id.isEmpty()){if(combo==textLanguages_.first())break;continue;}if(!languages.contains(id))languages.append(id);}
  if(languages.isEmpty())preferences.remove("textRecognitionLanguages");else preferences["textRecognitionLanguages"]=languages;}
 // #1618: one entry per line; blank lines are dropped, words are trimmed, and each entry is kept once. An empty box keeps no key.
 {// Each line's own text: toPlainText() would turn a no-break space into an ordinary one, changing what a word matches.
  const auto entries=[](QPlainTextEdit* edit,bool words){QJsonArray out;QStringList seen;for(auto block=edit->document()->begin();block.isValid();block=block.next()){auto line=block.text();if(line.endsWith('\r'))line.chop(1);if(words)line=line.trimmed();if(line.trimmed().isEmpty())continue;if(seen.contains(line))continue;seen<<line;out.append(line);}return out;};
  const auto terms=entries(quickRedactTerms_,true),patterns=entries(quickRedactPatterns_,false);
  if(terms.isEmpty())preferences.remove("quickRedactTerms");else preferences["quickRedactTerms"]=terms;
  if(patterns.isEmpty())preferences.remove("quickRedactPatterns");else preferences["quickRedactPatterns"]=patterns;}
 // #1364: both are absent from an untouched config (the system default input, no rings).
 if(preferences.contains("recordingMicrophoneDevice")||!recordingMicrophone_->currentData().toString().isEmpty())preferences["recordingMicrophoneDevice"]=recordingMicrophone_->currentData().toString();
 if(preferences.contains("recordingClickHighlights")||recordingClicks_->isChecked())preferences["recordingClickHighlights"]=recordingClicks_->isChecked();
 // An untouched config keeps no appearance object; it reads as "system".
 if(preferences.contains("appearance")||appearance_->currentData()!="system"){auto appearance=preferences["appearance"].toObject();appearance["theme"]=appearance_->currentData().toString();preferences["appearance"]=appearance;}
 // An untouched config keeps no updates object; it reads as a daily check on this build's channel.
 // The stored value stays until the choice shown is changed; then the build's
 // own channel is stored as "build", so a DEB and an AppImage that share this
 // file each keep following their own (#344), and the other one by name.
 const auto chosen=updateChannel_->currentData().toString();const bool changed=!updateChannelShown_.isEmpty()&&chosen!=updateChannelShown_;
 const auto channel=!changed?updateChannelSetting_:chosen==buildUpdateChannel()?QString("build"):chosen;
 // Both keys are optional in a stored object; one it leaves out stays out until it differs from
 // its default, or the draft never matches the file and every action reports unsaved settings.
 // Each is written on its own: a saved edit sends only the field changed, so writing the other
 // too would leave the draft different from the file. Which keys are stored comes from the file
 // this draft is based on too (a saved edit can add or empty the object): the restored checkpoint's
 // base, or the file as of this window's last accepted save, never a newer one saved elsewhere,
 // whose keys a replay would fill with this draft's older values. A stored object stays, even empty.
 {const auto accepted=(restoredDraft_?restoredBase_:baseDocument_)["preferences"].toObject();
  const bool stored=preferences.contains("updates")||accepted.contains("updates");
  auto updates=preferences.contains("updates")?preferences.value("updates").toObject():accepted.value("updates").toObject();
  if(updates.contains("check")||!checkUpdates_->isChecked())updates["check"]=checkUpdates_->isChecked();
  if(updates.contains("channel")||channel!="build")updates["channel"]=channel;
  if(stored||!updates.isEmpty())preferences["updates"]=updates;}
 // debugLogging is always written with the object; packageFolder stays as the draft holds it.
 if(preferences.contains("diagnostics")||debugLogging_->isChecked()){auto diagnostics=preferences["diagnostics"].toObject();diagnostics["debugLogging"]=debugLogging_->isChecked();preferences["diagnostics"]=diagnostics;}
 doc["preferences"]=preferences;auto limits=doc["limits"].toObject(),retention=doc["retention"].toObject();
 for(auto it=numbers_.begin();it!=numbers_.end();++it){
  bool ok=false;const auto n=it.value()->text().toLongLong(&ok);QJsonValue value=displayUnit(it.key())!=1?ByteSize::fromDecimal(it.value()->text(),displayUnit(it.key())):(ok?QJsonValue(n):QJsonValue(it.value()->text()));
  if(it.key()=="historyRetentionDays"&&(it.value()->text().isEmpty()||it.value()->text()=="Forever"))value=QJsonValue::Null;
  if(it.key()=="failedArtifactDays"&&it.value()->text()=="Forever")value=0;
  if(Settings::artifactByteLimitKeys().contains(it.key())){const auto text=it.value()->text().trimmed();if(text.isEmpty())value=QJsonValue::Null;else if(text.compare("auto",Qt::CaseInsensitive)==0)value=QStringLiteral("auto");}
  if(it.key()=="pendingImageBytes"&&!limits.contains(it.key())&&value==QJsonValue(effective.limits.pendingImageBytes))continue;
  if(limits.contains(it.key())||it.key()=="pendingImageBytes")limits[it.key()]=value;else retention[it.key()]=value;
 }
 doc["limits"]=limits;doc["retention"]=retention;return doc;
}
bool SettingsWindow::selectedDestinationChanged()const{
 const auto selected=editedProfile();for(const auto& value:settings_.document()["profiles"].toArray()){const auto accepted=value.toObject();if(accepted["id"]==selected["id"])return accepted!=selected;}return true;
}

namespace {
// #1704: a save operation names values by path, inside a destination ("settings/region") or in the
// document ("/preferences/notifications"); "" is the whole destination.
QStringList pathParts(const QString& path){return path.split('/',Qt::SkipEmptyParts);}
QJsonValue valueAt(QJsonValue value,const QStringList& parts){for(const auto& part:parts){const auto object=value.toObject();if(!value.isObject()||!object.contains(part))return QJsonValue(QJsonValue::Undefined);value=object.value(part);}return value;}
// An undefined value removes the key, and never adds the objects around it.
QJsonValue withValue(const QJsonValue& value,const QStringList& parts,int index,const QJsonValue& replacement){
 if(index==parts.size())return replacement;
 auto object=value.toObject();const auto& key=parts[index];
 if(replacement.isUndefined()&&!object.contains(key))return value;
 if(index+1==parts.size()&&replacement.isUndefined()){object.remove(key);return object;}
 object[key]=withValue(object.value(key),parts,index+1,replacement);return object;
}
// A new destination's settings and credentials for a service.
QJsonObject destinationDefaults(const QString& provider,const QString& id){
 if(provider==r2::providerId)return {{"settings",r2::defaults()},{"credentials",QJsonObject{{"mode","keychain"},{"reference","putgd/r2/"+id}}}};
 if(provider=="proton-drive")return {{"settings",ProtonProvider::defaults()},{"credentials",QJsonObject{{"mode","keychain"},{"reference","putgd/proton/"+QUuid::createUuid().toString(QUuid::Id128)}}}};
 if(provider=="backblaze-b2")return {{"settings",b2::defaults()},{"credentials",QJsonObject{{"mode","keychain"},{"reference","putgd/b2/"+id}}}};
 if(provider==CustomUploaderPanel::provider())return {{"settings",CustomUploaderPanel::defaultSettings()},{"credentials",QJsonObject{{"mode","keychain"},{"reference","putgd/http/"+id}}}};
 return s3Profile(id);
}
bool within(const QString& path,const QString& ancestor){return ancestor.isEmpty()||path==ancestor||path.startsWith(ancestor+"/");}
// Fields that save only together. #1676: Region and Endpoint, so a fix to one is never refused
// because of the other's saved value and then left looking saved.
// Cloudflare R2's group belongs to R2 destinations alone (#1699): an S3 or B2 Bucket stays a field of its own.
QList<QStringList> fieldGroups(const QString& provider){QList<QStringList> groups{{"settings/endpoint","settings/region"}};if(provider==r2::providerId)groups.append(r2Group());return groups;}
QStringList fieldGroup(const QString& path,const QString& provider){for(const auto& group:fieldGroups(provider))if(group.contains(path))return group;return {path};}
}
QStringList SettingsWindow::profileOrder()const{QStringList ids;for(const auto& p:draft_["profiles"].toArray())ids.append(p.toObject()["id"].toString());return ids;}
void SettingsWindow::applyProfileOrder(const QStringList& order){
 // Unsaved edits, retained provider editors and credential inputs follow their
 // destination; the selection follows it by ID.
 storeProfile();auto profiles=draft_["profiles"].toArray();const auto selected=profileId_->text();
 while(profileEditors_.size()<profiles.size())profileEditors_.append(QJsonValue::Null);
 if(credentialDrafts_.size()<profiles.size())credentialDrafts_.resize(profiles.size());
 QVector<int> sources;for(const auto& id:order)for(int i=0;i<profiles.size();++i)if(!sources.contains(i)&&profiles[i].toObject()["id"]==id){sources.append(i);break;}
 for(int i=0;i<profiles.size();++i)if(!sources.contains(i))sources.append(i);
 QJsonArray ordered,editors;QVector<QHash<QString,CredentialInputs>> credentials;int selectedIndex=-1;
 for(const int i:sources){if(profiles[i].toObject()["id"]==selected)selectedIndex=ordered.size();ordered.append(profiles[i]);editors.append(profileEditors_[i]);credentials.append(std::move(credentialDrafts_[i]));}
 for(qsizetype i=profiles.size();i<profileEditors_.size();++i)editors.append(profileEditors_[i]);
 for(qsizetype i=profiles.size();i<credentialDrafts_.size();++i)credentials.append(std::move(credentialDrafts_[i]));
 draft_["profiles"]=ordered;profileEditors_=editors;credentialDrafts_=std::move(credentials);
 // Only the rows move: the shown destination's editor stays live (#1713).
 rebuildProfiles(selectedIndex,{},true);refreshDestinationList();
}
Error SettingsWindow::moveDestination(int from,int to){
 auto order=profileOrder();
 if(from<0||to<0||from>=order.size()||to>=order.size()||from==to)return {"destination_move_invalid","That destination can't move further in this direction.",{{"from",from},{"to",to}}};
 if(resetting_||!typingPause_)return {"ui_busy","Settings are reloading. Try again when they finish.",{}};
 if(conflict_)return {"config_conflict","Settings changed elsewhere — reload to continue.",{}};
 if(setupAccepting_||!setupJobs_.isEmpty())return {"setup_pending","Wait for destination setup to finish before reordering destinations.",{}};
 reorderPending_=true;
 order.move(from,to);applyProfileOrder(order);
 // #1704: the order goes as a list of IDs, applied to the accepted list when it is sent, so an
 // edit that was refused earlier cannot block it and it never carries another edit's copy.
 dirty();queueOrder();
 emit viewStateChanged();return {};
}
QString SettingsWindow::buildUpdateChannel() const {
 const auto update=settings_.diagnostics()["update"].toObject();
 return update["supported"].toBool()?update["build"].toObject()["channel"].toString():QString();
}
void SettingsWindow::showUpdateChannel(){
 // A repository install follows its repository's channel (#386): shown, not
 // chosen, and the stored choice stays as it is for an AppImage sharing it.
 const auto update=settings_.diagnostics()["update"].toObject();
 const bool repository=update["supported"].toBool()&&update["channelSource"]=="repository";
 updateChannel_->setEnabled(!repository);Ui::setLocked(updateChannel_,repository);updateChannelSource_->setVisible(repository);
 if(repository){
  const auto channel=update["channel"].toString();
  updateChannelSource_->setText("Set by your system's put.gd repository, "+update["action"].toObject()["file"].toString("which only you change")+".");
  const QSignalBlocker block(updateChannel_);updateChannel_->setCurrentIndex(qMax(0,updateChannel_->findData(channel)));updateChannelShown_=channel;
  return;
 }
 // Once the choice has been changed here, it stays as chosen.
 if(!updateChannelShown_.isEmpty()&&updateChannel_->currentData()!=updateChannelShown_)return;
 const auto channel=updateChannelSetting_=="stable"||updateChannelSetting_=="nightly"?updateChannelSetting_:buildUpdateChannel();
 if(channel.isEmpty())return;
 const QSignalBlocker block(updateChannel_);updateChannel_->setCurrentIndex(qMax(0,updateChannel_->findData(channel)));updateChannelShown_=channel;
}
void SettingsWindow::refreshUpdateStatus(){
 const auto update=settings_.diagnostics()["update"].toObject();const auto build=update["build"].toObject();const auto result=update["result"].toObject();
 const bool supported=update["supported"].toBool();
 updatesIntro_->setVisible(supported);updatesIntro_->setText(checkUpdates_->isChecked()?"Checks app.put.gd once a day. Nothing installs automatically.":"Checks app.put.gd when you choose Check now. Nothing installs automatically.");
 // The choice shows this build's channel until it is changed (#344).
 showUpdateChannel();
 // A build that never checks has nothing to choose: only the reason shows.
 const bool development=settings_.build()["version"].toString().contains("-dev");for(auto* row:std::as_const(updateRows_))updatesForm_->setRowVisible(row,supported||(update.isEmpty()&&!development));
 // The status keeps the row's width beside Check now, so it never folds into a narrow column.
 if(auto* row=qobject_cast<QHBoxLayout*>(updateStatus_->parentWidget()->layout())){row->setStretch(0,1);row->setStretch(2,0);}
 checkNow_->setVisible(supported);checkNow_->setEnabled(supported&&!update["checking"].toBool());
 const bool available=supported&&result["updateAvailable"].toBool();
 const auto action=update["action"].toObject();const bool managed=action["kind"]=="software-updates";
 // An AppImage watching the other channel switches to its release (#344).
 const auto install=update["install"].toObject();const auto offer=install["switch"].toObject();
 const bool switching=supported&&build["packageTarget"]=="appimage"&&install["kind"]=="switch";
 updateActionsRow_->setVisible(available||!offer.isEmpty()||install["restartRequired"].toBool());
 downloadUpdate_->setVisible(!managed);openUpdates_->setVisible(managed&&action["updater"].isObject());
 if(managed){openUpdates_->setToolTip("Opens "+action["updater"].toObject()["name"].toString());updateCommand_->setText(action["command"].toString());}
 // Moved to the other channel's repository, whose release is older: the
 // command that installs it anyway (#386).
 const auto older=managed?action["older"].toObject():QJsonObject{};
 if(!older.isEmpty())updateCommand_->setText(older["command"].toString());
 updatesForm_->setRowVisible(updateCommandRow_,(available&&managed)||!older.isEmpty());
 const auto other=managed&&supported&&update["channelSource"]=="repository"?action["otherChannel"].toObject():QJsonObject{};
 updatesForm_->setRowVisible(otherChannelRow_,!other.isEmpty());
 if(!other.isEmpty()){
  const auto to=other["channel"].toString();
  otherChannelText_->setText(to=="stable"?
   QString("To move to stable, run these commands in a terminal. Stable is usually older than a nightly, so the last command installs it as a downgrade. Leave it out to keep this version until a newer stable release is out; your system updater then installs it."):
   QString("To move to nightly, run these commands in a terminal. Nightly builds come from the newest code every day, so they're usually newer than the stable release."));
  QStringList commands;for(const auto& command:other["commands"].toArray())commands.append(command.toString());
  otherChannelCommands_->setText(commands.join(" &&\n"));
 }
 if(update.isEmpty()){Ui::feedback(updateStatus_,settings_.build()["version"].toString().contains("-dev")?"Update checks aren't available for development builds":"Update status isn't available yet.");return;}
 if(!supported){
  static const QHash<QString,QString> reasons{
   {"development_build","Update checks aren't available for development builds"},
   {"no_package","This build wasn't made by a put.gd package (for example, a build from source), so it doesn't check for updates."},
   {"disabled_at_build","Update checks were turned off when this build was made, for example by a distribution's own package."}};
  Ui::feedback(updateStatus_,reasons.value(update["reason"].toString(),"This build doesn't check for updates."));return;
 }
 const auto channel=update["channel"].toString();// A non-breaking hyphen keeps "26.1026.1-nightly" on one line.
 const auto current=build["version"].toString().replace('-',QChar(0x2011));
 QString text;
 if(update["checking"].toBool())text="Checking…";
 else if(switching&&!offer.isEmpty()){
  const auto target=offer["to"].toString()+" "+offer["version"].toString();
  text=offer["older"].toBool()?capitalized(target)+" is older than this "+build["channel"].toString()+" build ("+current+"). Switching installs the older version in its place.":
   capitalized(target)+" is available. This is "+build["channel"].toString()+" "+current+"; switching replaces it with the "+offer["to"].toString()+" release.";
 }
 else if(switching&&!result.isEmpty()&&result["latest"].isNull())
  text="There's no "+channel+" release yet. This "+build["channel"].toString()+" build ("+current+") stays as it is, and doesn't get "+build["channel"].toString()+
   " updates while "+capitalized(channel)+" is chosen. put.gd offers the switch once the first "+channel+" release is out.";
 else if(!older.isEmpty())
  text="Your system now updates put.gd from the "+channel+" repository. "+capitalized(channel)+" "+older["version"].toString()+" is older than this "+build["channel"].toString()+" build ("+current+
   "), so the system updater keeps this version until a newer "+channel+" release is out. To install "+channel+" "+older["version"].toString()+" now, run the command below.";
 else if(managed&&channel!=build["channel"].toString()&&!result.isEmpty()&&result["latest"].isNull())
  text="There's no "+channel+" release yet. Your system updates put.gd from the "+channel+" repository, so this "+build["channel"].toString()+" build ("+current+
   ") stays as it is until the first "+channel+" release, which the system updater then installs.";
 else if(available&&managed)text="put.gd "+result["latest"].toString()+" is available from put.gd's "+action["channel"].toString()+" repository. This is "+current+".";
 else if(available)text="put.gd "+result["latest"].toString()+" is available. This is "+current+".";
 else if(!result.isEmpty()&&result["latest"].isNull())text="No "+channel+" release has been published yet. This is "+current+".";
 else if(!result.isEmpty())text="Up to date: "+current+" is the newest "+channel+" version.";
 else text="Not checked yet. This is "+current+".";
 if(!update["checking"].toBool()){
  if(!update["checkedAt"].isNull())text+=" Checked "+localTime(update["checkedAt"].toString())+".";
  const auto error=update["error"].toObject();
  if(!error.isEmpty())text+=" The last check failed: "+error["message"].toString()+".";
  if(!update["nextCheckAt"].isNull())text+=" Next check "+localTime(update["nextCheckAt"].toString())+".";
  else if(!update["check"].toBool())text+=" Daily checks are off.";
 }
 if(available||!offer.isEmpty())Ui::success(updateStatus_,text);else Ui::feedback(updateStatus_,text);
 downloadUpdate_->setText("Download "+result["latest"].toString());
 refreshInstallStatus(update,available||!offer.isEmpty(),managed);
}
void SettingsWindow::refreshInstallStatus(const QJsonObject& update,bool available,bool managed){
 const auto install=update["install"].toObject();const auto state=install["state"].toString();
 const bool running=install["running"].toBool(),restart=install["restartRequired"].toBool();
 // The version put.gd installed is the one running now: the update is done.
 const auto installed=install["installed"].toObject();const auto installedVersion=installed["version"].toString();
 const bool updated=!running&&!restart&&!installedVersion.isEmpty()&&installedVersion==update["build"].toObject()["version"].toString();
 const bool installable=available&&!managed&&install["available"].toBool();
 installUpdate_->setVisible(installable&&!restart);installUpdate_->setEnabled(!running);
 const auto offer=install["switch"].toObject();
 installUpdate_->setText(running?QString(offer.isEmpty()?"Installing…":"Switching…"):offer.isEmpty()?QString("Download and install"):
  "Switch to "+offer["to"].toString()+" "+offer["version"].toString()+(offer["older"].toBool()?" (older)":""));
 // Download stays for anyone who would rather replace the file by hand.
 Ui::role(downloadUpdate_,installable?"secondary":"primary");
 downloadUpdate_->setVisible(!managed&&!restart);releaseNotes_->setVisible(!restart||available);
 restartUpdate_->setVisible(restart&&!running);
 installProgress_->setVisible(running);
 const bool shown=running||restart||state=="failed"||updated;
 updatesForm_->setRowVisible(installRow_,shown);
 if(!shown)return;
 const auto bytes=install["bytes"].toInteger(),total=install["total"].toInteger();
 if(running){
  if(total>0&&(install["phase"]=="zsync"||install["phase"]=="download")){installProgress_->setRange(0,1000);installProgress_->setValue(int(qBound(qint64(0),bytes*1000/total,qint64(1000))));}
  else installProgress_->setRange(0,0);
  const auto phase=install["phase"].toString();const auto method=install["method"].toString();
  const auto amount=total>0?" "+ByteSize::summary(bytes)+" of "+ByteSize::summary(total):QString();
  QString text=phase=="zsync"?"Updating with "+method+", which downloads only what changed:"+amount:
   phase=="download"?(method=="mirror"?QString("Downloading from pkg.put.gd:"):QString("Downloading from the GitHub release:"))+amount:
   phase=="verifying"?QString("Checking against the release's SHA256SUMS…"):phase=="installing"?QString("Putting the new version in place…"):QString("Checking the release…");
  Ui::feedback(installStatus_,text);return;
 }
 // A switch says so (#344): the record names both channels.
 const auto from=installed["previousChannel"].toString(),to=installed["channel"].toString();
 const bool switched=!from.isEmpty()&&!to.isEmpty()&&from!=to;
 if(restart){
  const auto how=installed["method"].toString();
  QString text=(switched?"Switched to "+to+" ":QString("Installed "))+installed["version"].toString()+(how=="zsync"?QString(" with a zsync update"):how=="download"?QString(" from a full download"):QString())+". Restart put.gd to use it.";
  if(install["backup"].isString())text+=" The previous version is kept as "+QFileInfo(install["backup"].toString()).fileName()+" until the new one has started.";
  Ui::success(installStatus_,text);return;
 }
 const auto error=install["error"].toObject();
 if(state!="failed"&&updated){
  const auto how=installed["method"].toString();
  Ui::success(installStatus_,(switched?"Switched from "+from+" "+installed["previousVersion"].toString()+" to "+to+" ":"Updated from "+installed["previousVersion"].toString()+" to ")+installed["version"].toString()+
   (how=="zsync"?QString(" with a zsync update"):how=="download"?QString(" from a full download"):QString())+" on "+localTime(installed["installedAt"].toString())+".");
  return;
 }
 Ui::feedback(installStatus_,"The update wasn't installed: "+error["message"].toString()+".",true);
}
void SettingsWindow::checkForUpdates(std::function<void(Error,QJsonObject)> done){
 checkNow_->setEnabled(false);Ui::feedback(updateStatus_,"Checking…");
 settings_.request("update.check",{},this,[g=QPointer<SettingsWindow>(this),done](Error error,QJsonObject result){
  if(!g)return;
  g->settings_.refreshSnapshot([g,done,error,result](Error){if(!g)return;g->refreshUpdateStatus();if(done)done(error,result);});
 });
}
void SettingsWindow::refreshLoggingStatus(){
 const auto data=settings_.diagnostics();const auto logging=data["logging"].toObject();
 const auto expires=logging["debugLoggingExpiresAt"].toString();
 debugLoggingStatus_->setText(logging["debugLogging"].toBool()&&!expires.isEmpty()?"On. Turns itself off at "+localTime(expires)+".":debugLogging_->isChecked()?QString("On. Turns itself off 24 hours after it was turned on."):QString("Records detailed logs for a maintainer and turns itself off after 24 hours."));debugLoggingStatus_->setText(debugLoggingStatus_->text()+" Credentials, tokens and file contents are never logged.");static_cast<PreferenceCaption*>(debugLoggingStatus_)->refreshHeight();
 if(!logging["available"].toBool(true)||logging.isEmpty())logSummary_->setText(logging.isEmpty()?QString("Log status isn't available yet."):"The log folder isn't available: "+logging["directory"].toString());
 else{
  const int files=logging["files"].toInt();const auto oldest=logging["oldestRecord"].toString();const auto retention=logging["retention"].toObject();
  logSummary_->setText((files==1?QString("1 file"):QString("%1 files").arg(files))+" · "+ByteSize::summary(logging["bytes"].toInteger())+(oldest.isEmpty()?QString{}:" · oldest record "+localTime(oldest))+
   (retention.isEmpty()?QString{}:QString(" · kept %1 days, up to %2").arg(retention["days"].toInt()).arg(ByteSize::summary(retention["bytes"].toInteger()))));
 }
 logSummary_->setToolTip(logSummary_->text()+(logging["directory"].toString().isEmpty()?QString{}:"\n"+logging["directory"].toString()));
 const bool degraded=data["loggingDegraded"].toBool()||logging["degraded"].toBool();
 static const QHash<QString,QString> reasons{{"unavailable","the log folder can't be used"},{"write_failed","writing to it failed"},{"disk_full","the disk is full"},{"queue_full","records arrived faster than they could be written"}};
 const auto dropped=logging["droppedRecords"].toInteger();
 loggingWarning_->setText(degraded?"Logging is degraded: "+reasons.value(logging["degradedReason"].toString(),logging["degradedReason"].toString("unknown reason"))+(dropped?QString(". %1 records were dropped").arg(dropped):QString{})+". Uploads and captures aren't affected.":QString{});
 loggingWarning_->setVisible(degraded);
 const auto unclean=data["uncleanExit"].toObject();const auto last=unclean["last"].toObject();
 if(last.isEmpty())uncleanExit_->setText(unclean.isEmpty()?QString("Not available yet."):QString("None recorded."));
 else{
  const auto version=last["previousVersion"].toString();const auto when=localTime(last["previousStart"].toString());
  const auto cause=last["cause"]=="signal"?QString("ended by signal %1 (for example a logout)").arg(last["signal"].toInt()):QString("stopped unexpectedly: a crash, a forced kill or a power loss");
  uncleanExit_->setText("put.gd"+(version.isEmpty()?QString{}:" "+version)+" started "+when+" and "+cause+". Found at the next start, "+localTime(last["detectedAt"].toString())+(unclean["detected"].toBool()?" (this start).":"."));
  Ui::role(uncleanExit_,last["cause"]=="signal"?"muted":"warning");
 }
}
void SettingsWindow::clearLogs(std::function<void(Error,QJsonObject)> done){
 clearLogs_->setEnabled(false);
 settings_.request("logs.clear",{},this,[g=QPointer<SettingsWindow>(this),done](Error error,QJsonObject result){
  if(g){
   g->clearLogs_->setEnabled(true);
   if(error)g->status(error);else Ui::feedback(plain(g->status_),QString("Logs cleared: %1 files, %2 removed.").arg(result["removedFiles"].toInt()).arg(ByteSize::summary(result["removedBytes"].toInteger())));
   // The log's new figures come with the next snapshot.
   g->settings_.refreshSnapshot([g,done,error,result](Error){if(g)g->refreshLoggingStatus();if(done)done(error,result);});return;
  }
  if(done)done(error,result);
 });
}
// The dialog remembers a chosen folder in preferences.diagnostics.packageFolder; an
// empty folder clears it, so packages go to <archive root>/diagnostics again.
void SettingsWindow::choosePackageFolder(const QString& folder){
 auto preferences=draft_["preferences"].toObject();auto diagnostics=preferences["diagnostics"].toObject();
 diagnostics["debugLogging"]=debugLogging_->isChecked();diagnostics["packageFolder"]=folder.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(folder);
 preferences["diagnostics"]=diagnostics;draft_["preferences"]=preferences;queueEdit(debugLogging_,"/preferences/diagnostics");
}
void SettingsWindow::openDiagnosticsPackage(const QString& operationId){
 auto* dialog=static_cast<DiagnosticsPackageDialog*>(packageDialog_.data());
 if(!dialog){dialog=new DiagnosticsPackageDialog(settings_,this);dialog->folderChosen=[this](const QString& folder){choosePackageFolder(folder);};packageDialog_=dialog;}
 dialog->setScope(operationId,candidate()["preferences"].toObject()["diagnostics"].toObject()["packageFolder"].toString());
 dialog->open();dialog->raise();dialog->activateWindow();
 // Once shown, the body knows its width and the dialog its screen.
 dialog->fit();QTimer::singleShot(0,dialog,[dialog=QPointer<DiagnosticsPackageDialog>(dialog)]{if(dialog)dialog->fit();});
}
QJsonObject SettingsWindow::diagnosticsState()const{
 const auto logging=settings_.diagnostics()["logging"].toObject();
 QJsonObject state{{"debugLogging",QJsonObject{{"enabled",debugLogging_->isChecked()},{"expiresAt",logging["debugLoggingExpiresAt"]},{"text",debugLoggingStatus_->text()}}},
  {"logs",QJsonObject{{"files",logging["files"]},{"bytes",logging["bytes"]},{"oldestRecord",logging["oldestRecord"]},{"text",logSummary_->text()}}},
  {"loggingDegraded",QJsonObject{{"degraded",loggingWarning_->isVisible()||settings_.diagnostics()["loggingDegraded"].toBool()},{"text",loggingWarning_->text()}}},
  {"uncleanExit",QJsonObject{{"last",settings_.diagnostics()["uncleanExit"].toObject()["last"]},{"text",uncleanExit_->text()}}},
  {"autosave",autosaveText()},{"desktopSupport",QJsonObject{{"expanded",findChild<QToolButton*>("desktopSupportDetailsToggle")->isChecked()},{"details",settings_.diagnostics()}}}};
 auto* dialog=static_cast<DiagnosticsPackageDialog*>(packageDialog_.data());
 state["package"]=dialog&&dialog->isVisible()?dialog->state():QJsonObject{{"open",false}};
 return state;
}
bool SettingsWindow::chooseOutputFolder(QLineEdit* field){
 return folderChooser_->open({FileDialog::Mode::Folder,"Choose output folder",field->text(),{},{}},[this,field=QPointer<QLineEdit>(field)](const FileDialog::Result& result){if(field)outputFolderChosen(field,result);});
}
// A cancel says nothing; a failure is not a cancel: it is shown under the row
// whose Browse… opened it, the folder unchanged, with Try again.
void SettingsWindow::outputFolderChosen(QLineEdit* field,const FileDialog::Result& result){
 if(auto* failure=chooserFailures_.value(field)){failure->setVisible(result.failed());if(result.failed())failure->findChild<QLabel*>()->setText(chooserFailureText(result.error)+" The folder is unchanged.");}
 if(result.chosen()){field->setText(result.path);queueEdit(field);}
}
void SettingsWindow::folderChooserFinished(const QString& dialog,const QString& field,const FileDialog::Result& result){
 if(dialog=="diagnosticsFolder"){if(auto* package=static_cast<DiagnosticsPackageDialog*>(packageDialog_.data()))package->chooserFinished(result);return;}
 if(auto* target=field.isEmpty()||field=="root"?root_:folders_.value(field))outputFolderChosen(target,result);
}
void SettingsWindow::fileDialogCommand(const QString& dialog,const QString& action,const QString& field,std::function<void(Error,QJsonObject)> reply){
 // #259: the folder choosers as their buttons open them; the reply is the chooser's state.
 FileDialog* chooser=folderChooser_;
 if(dialog=="diagnosticsFolder"){
  auto* package=static_cast<DiagnosticsPackageDialog*>(packageDialog_.data());
  if(!package||!package->isVisible()){reply({"diagnostics_dialog_closed","Open Create diagnostics package first (ui.diagnostics openPackage).",{}},{});return;}
  chooser=&package->folderDialog();
  if(action=="open"&&!chooser->isOpen())package->chooseFolder();
 }else if(action=="open"&&!chooser->isOpen()){
  QLineEdit* target=field.isEmpty()||field=="root"?root_:folders_.value(field);
  if(!target){reply({"invalid_request","field is root, images, videos, text or other.",{}},{});return;}
  tabs_->setCurrentIndex(1);chooseOutputFolder(target);
 }else if(action=="open"){reply({"file_dialog_open","Choose output folder is already open.",{}},chooser->state());return;}
 if(action=="cancel")chooser->cancel();
 reply({},chooser->state());
}
void SettingsWindow::diagnosticsCommand(const QJsonObject& params,std::function<void(Error,QJsonObject)> reply){
 const auto action=params["action"].toString();
 auto* dialog=static_cast<DiagnosticsPackageDialog*>(packageDialog_.data());const bool open=dialog&&dialog->isVisible();
 auto withState=[g=QPointer<SettingsWindow>(this),reply](Error error,QJsonObject result){if(!g){reply(error,result);return;}auto state=g->diagnosticsState();if(!result.isEmpty())state["result"]=result;reply(error,state);};
 if(action=="inspect"){reply({},diagnosticsState());return;}
 if(action=="setDesktopDetails"){if(!params["enabled"].isBool()){reply({"invalid_request","setDesktopDetails takes enabled, true or false.",{}},{});return;}tabs_->setCurrentIndex(advancedTab);findChild<QToolButton*>("desktopSupportDetailsToggle")->setChecked(params["enabled"].toBool());reply({},diagnosticsState());return;}
 if(action=="copyDetails"){findChild<QPushButton*>("copyDesktopDetails")->click();reply({},diagnosticsState());return;}
 if(action=="showConfigFolder"){host::showInFolder(settings_.path(),[withState](const QJsonObject& result){withState(result["opened"].toBool()?Error{}:Error{"folder_open_failed","Couldn't show the configuration file in its folder.",{}},result);});return;}
 if(action=="refreshDesktopSupport"){settings_.refreshSnapshot([g=QPointer<SettingsWindow>(this),withState](Error error){if(g)g->refreshDiagnostics();withState(error,{});});return;}
 if(action=="setDebugLogging"){
  if(!params["enabled"].isBool()){reply({"invalid_request","setDebugLogging takes enabled, true or false.",{}},{});return;}
  // The checkbox autosaves like any setting; inspect shows when the resident accepted it.
  tabs_->setCurrentIndex(advancedTab);debugLogging_->setChecked(params["enabled"].toBool());flushAutosave();refreshLoggingStatus();reply({},diagnosticsState());return;
 }
 if(action=="clearLogs"){tabs_->setCurrentIndex(advancedTab);clearLogs(withState);return;}
 if(action=="openPackage"){tabs_->setCurrentIndex(advancedTab);openDiagnosticsPackage(params["operation"].toString());dialog=static_cast<DiagnosticsPackageDialog*>(packageDialog_.data());if(params.contains("includeNames"))dialog->setIncludeNames(params["includeNames"].toBool());dialog->whenPreviewed([withState](Error error,QJsonObject){withState(error,{});});return;}
 if(!open){reply({"diagnostics_dialog_closed","Open Create diagnostics package first (action openPackage).",{}},{});return;}
 if(action=="setPackage"){
  if(params.contains("folder"))if(auto error=dialog->setFolder(params["folder"].toString(),true)){reply(error,{});return;}
  if(params.contains("includeNames"))dialog->setIncludeNames(params["includeNames"].toBool());
  if(params.contains("upload"))if(auto error=dialog->setUpload(params["upload"].toBool())){reply(error,diagnosticsState());return;}
  dialog->whenPreviewed([withState](Error error,QJsonObject){withState(error,{});});return;
 }
 if(action=="savePackage"){
  // An upload can take minutes: answer once it starts; inspect shows saving until it ends.
  if(dialog->uploading()){
   auto answered=std::make_shared<bool>(false);
   dialog->save([answered,withState](Error error,QJsonObject){if(std::exchange(*answered,true))return;withState(error,{});});
   if(!std::exchange(*answered,true))reply({},diagnosticsState());return;
  }
  dialog->save([withState](Error error,QJsonObject){withState(error,{});});return;
 }
 if(action=="showInFolder"){dialog->reveal(withState);return;}
 if(action=="closePackage"){dialog->reject();reply({},diagnosticsState());return;}
 reply({"invalid_request","Unknown diagnostics action.",{}},{});
}
Error SettingsWindow::moveSelectedDestination(int delta,const QString& profileId,QJsonObject& result){
 if(!profileId.isEmpty()){const int index=profileOrder().indexOf(profileId);if(index<0)return {"profile_not_found","No destination has that ID.",{{"profileId",profileId}}};if(index!=profile_->currentIndex())profile_->setCurrentIndex(index);}
 const int from=profile_->currentIndex();if(from<0)return {"destination_not_selected","Select a destination first.",{}};
 if(auto error=moveDestination(from,from+delta))return error;
 result={{"selected",profileId_->text()},{"order",QJsonArray::fromStringList(profileOrder())},{"saving",queued("order")}};return {};
}
void SettingsWindow::restoreReorderBaseline(){
 if(!std::exchange(reorderPending_,false))return;
 saveQueue_.erase(std::remove_if(saveQueue_.begin(),saveQueue_.end(),[](const SaveOperation& op){return op.key=="order";}),saveQueue_.end());
 showFileOrder();
}
// The order the file has now, with each destination only this window has (an add not saved yet)
// kept after the one it follows here (#1713).
void SettingsWindow::showFileOrder(){
 // Only destinations this window knows as saved take the file's places: a row added here keeps
 // its place even when another client has since saved a destination under the same ID.
 QSet<QString> known;for(const auto& value:baseDocument_["profiles"].toArray())known.insert(value.toObject()["id"].toString());
 QStringList order;for(const auto& value:acceptedDocument()["profiles"].toArray())if(const auto id=value.toObject()["id"].toString();known.contains(id))order.append(id);
 const auto shown=profileOrder();
 for(int i=0;i<shown.size();++i)if(!order.contains(shown[i])){int at=0;for(int j=i-1;j>=0;--j)if(const int k=order.indexOf(shown[j]);k>=0){at=k+1;break;}order.insert(at,shown[i]);}
 QStringList wanted;for(const auto& id:std::as_const(order))if(shown.contains(id))wanted.append(id);
 if(wanted!=shown)applyProfileOrder(order);
}
qint64 SettingsWindow::displayUnit(const QString& key){return key=="imagePixels"?1000000:key=="maxFileBytes"||key=="maxBatchBytes"||key=="pendingArtifactBytes"?ByteSize::GiB:key.endsWith("Bytes")?ByteSize::MiB:1;}
// #1675, #1676: a Region or Endpoint saved before today's rules shows its problem when the
// destination opens, with the Endpoint row in view. Region feedback is updateRegionFeedback's;
// an Endpoint path is refused on every host.
void SettingsWindow::showS3SettingsProblems(){
 // Another destination: forget the scroll and close the sections this opened by itself, so
 // they don't stay open for B2, Proton Drive or another S3 destination.
 auto* field=s3_["endpoint"];
 if(field->property("shownFor")!=profileId_->text()){
  field->setProperty("shownFor",profileId_->text());field->setProperty("revealedFor",{});
  for(const auto* name:{"customEndpoint","advancedSettings"})if(auto* toggle=findControl<QAbstractButton*>(name);toggle&&toggle->property("openedForProblem").toBool()){toggle->setProperty("openedForProblem",false);toggle->setChecked(false);}
 }
 updateRegionFeedback();
 const auto endpoint=providerType_->currentData()=="s3"?s3EndpointProblem(s3_["endpoint"]->text()):QString();
 if(endpoint.isEmpty())return;
 // findControl: the destination page may live in the main window, outside this one.
 for(const auto* name:{"advancedSettings","customEndpoint"})if(auto* toggle=findControl<QAbstractButton*>(name);toggle&&!toggle->isChecked()){toggle->setChecked(true);toggle->setProperty("openedForProblem",true);}
 // Scrolled into view once per destination shown, after the opened section is laid out; a later
 // reload, such as after saving another field, leaves the page where the user put it.
 if(s3_["endpoint"]->property("revealedFor")==profileId_->text())return;
 QTimer::singleShot(0,this,[this,id=profileId_->text()]{auto* field=s3_["endpoint"];if(profileId_->text()!=id||!field->isVisible())return;
  for(auto* parent=field->parentWidget();parent;parent=parent->parentWidget())if(auto* area=qobject_cast<QScrollArea*>(parent)){if(area->widget()&&area->widget()->layout())area->widget()->layout()->activate();if(auto* label=errors_.value(field))area->ensureWidgetVisible(label,0,Ui::scaled(24));area->ensureWidgetVisible(field,0,Ui::scaled(24));field->setProperty("revealedFor",id);break;}});
}
void SettingsWindow::inlineError(QWidget* control,const QString& message){if(!control)return;if(control==root_)if(auto* hint=findChild<QLabel*>("archiveRootHint"))hint->setVisible(message.isEmpty()&&(root_->text().isEmpty()||!QDir::isAbsolutePath(root_->text())));control->setProperty("invalid",!message.isEmpty());if(!message.isEmpty())updateResourceLimits();control->style()->unpolish(control);control->style()->polish(control);if(auto* label=errors_.value(control)){Ui::role(label,"error");label->setText(message);label->setVisible(!message.isEmpty());}const auto description=control->property("settingDescription").isValid()?control->property("settingDescription").toString():control->toolTip();control->setAccessibleDescription(message.isEmpty()?description:description+"\n"+message);}
// #1704: every edit becomes one operation in a single queue. A destination's edits merge into one
// operation keyed by its ID; the order, a removal and each setting are operations of their own.
// One request is in flight at a time, and each operation is applied to the latest accepted
// document when it is sent.
// The style row shows with Random names; length and characters only for Random characters.
void SettingsWindow::showRandomNameRows(){
 if(!namingForm_)return;
 const auto rowOf=[this](QWidget* widget){while(widget&&namingForm_->indexOf(widget)<0)widget=widget->parentWidget();return widget;};
 const bool random=naming_->currentData()=="generated";const bool string=random&&randomStyle_->currentData()=="string";
 if(auto* row=rowOf(randomStyle_))namingForm_->setRowVisible(row,random);
 for(QWidget* control:{static_cast<QWidget*>(randomLength_),static_cast<QWidget*>(randomUppercase_),static_cast<QWidget*>(randomDigits_)})if(auto* row=rowOf(control))namingForm_->setRowVisible(row,string);
 emit viewStateChanged();
}
void SettingsWindow::queueEdit(QWidget* control,QString path){
 if(loading_||resetting_||!typingPause_)return;
 if(path.isEmpty())path=control->property("configPath").toString();
 if(path.isEmpty())return;
 if(control&&control==typingControl_){typingPause_->stop();typingControl_=nullptr;}
 if(path.startsWith("/actions/")&&!path.endsWith("/shortcut")){
  const auto action=path.split('/')[2];if(useCaptureDefault_.contains(action)){const QSignalBlocker block(useCaptureDefault_[action]);useCaptureDefault_[action]->setChecked(false);queueCaptureEdit(control);return;}
 }
 static const QString destination=QStringLiteral("/profiles/{profile}");
 if(path==destination||path.startsWith(destination+"/")){queueDestinationEdit(control,path.mid(destination.size()+1));return;}
 if(path=="/profiles"){queueOrder();return;}
 queueGlobal(path,{path},control);
}
bool SettingsWindow::queued(const QString& key)const{
 const auto touches=[&key](const SaveOperation& op){if(op.key==key)return true;for(auto it=op.values.cbegin();it!=op.values.cend();++it)if(it.key().startsWith('/')&&within(key,it.key()))return true;return false;};
 return (saving_&&touches(sending_))||std::any_of(saveQueue_.cbegin(),saveQueue_.cend(),touches);
}
void SettingsWindow::enqueue(SaveOperation op){
 for(auto& queuedOp:saveQueue_)if(queuedOp.key==op.key){queuedOp=std::move(op);checkQueue();return;}
 saveQueue_.append(std::move(op));checkQueue();
}
// An operation that goes back after a refusal or a failure goes first, merged under any newer
// one for the same key: the newer values win and the older one's other values are kept, so there
// is never more than one operation per key.
void SettingsWindow::requeue(SaveOperation op){
 for(int i=0;i<saveQueue_.size();++i)if(saveQueue_[i].key==op.key){op=merged(std::move(op),saveQueue_.takeAt(i));break;}
 saveQueue_.prepend(std::move(op));checkQueue();
}
SettingsWindow::SaveOperation SettingsWindow::merged(SaveOperation older,const SaveOperation& newer)const{
 using Kind=SaveOperation::Kind;
 if(older.kind!=newer.kind||older.kind==Kind::Order)return newer;
 for(auto it=newer.values.cbegin();it!=newer.values.cend();++it){
  const auto& path=it.key();
  for(auto o=older.values.begin();o!=older.values.end();)if(within(o.key(),path)){older.controls.remove(o.key());o=older.values.erase(o);}else ++o;
  QString around;bool nested=false;for(auto o=older.values.cbegin();o!=older.values.cend();++o)if(within(path,o.key())){around=o.key();nested=true;}
  if(nested)older.values[around]=withValue(older.values.value(around),pathParts(path.mid(around.size())),0,it.value());else older.values[path]=it.value();
  if(newer.controls.contains(path))older.controls[path]=newer.controls.value(path);
 }
 if(older.kind==Kind::Fields){
  const auto touched=[&newer](const QString& group){for(const auto& field:group.split('+'))for(auto it=newer.values.cbegin();it!=newer.values.cend();++it)if(within(field,it.key())||within(it.key(),field))return true;return false;};
  older.profile=newer.profile;
  older.stripped.erase(std::remove_if(older.stripped.begin(),older.stripped.end(),touched),older.stripped.end());
  for(const auto& group:newer.stripped)if(!older.stripped.contains(group))older.stripped.append(group);
  older.probing.erase(std::remove_if(older.probing.begin(),older.probing.end(),touched),older.probing.end());
 }
 return older;
}
void SettingsWindow::checkQueue(){
 for(int i=0;i<saveQueue_.size();++i)for(int j=i+1;j<saveQueue_.size();){
  if(saveQueue_[j].key!=saveQueue_[i].key){++j;continue;}
  Q_ASSERT_X(false,"SettingsWindow","two save operations for one key");
  // A release build merges them, the later one's values winning, rather than send both.
  saveQueue_[i]=merged(std::move(saveQueue_[i]),saveQueue_.takeAt(j));
 }
}
bool SettingsWindow::acceptedHas(const QString& id)const{for(const auto& value:acceptedDocument()["profiles"].toArray())if(value.toObject()["id"]==id)return true;return false;}
// A route that names a destination the resident doesn't have yet waits until it does.
// An operation that sets an action to inherit the default output.
bool SettingsWindow::setsInheritance(const SaveOperation& op){
 for(auto it=op.values.cbegin();it!=op.values.cend();++it)if(it.key().startsWith("/actions/")&&((it.key().endsWith("/destination")&&it.value()==QJsonValue("default"))||it.value().toObject().value("destination")==QJsonValue("default")))return true;
 return false;
}
bool SettingsWindow::waitsForDestination(const SaveOperation& op,const QJsonObject& accepted)const{
 // A removal whose add's answer was lost waits until the file has been read again.
 if(op.kind==SaveOperation::Kind::Remove)return op.reconciling;
 // A Cloudflare R2 destination is added, or switched to, only once Account ID, Jurisdiction and
 // Bucket are valid (#1699): nothing the resident must refuse is sent, and its routes wait with it.
 const auto held=[this](const SaveOperation& fields){return fields.kind==SaveOperation::Kind::Fields&&fields.profile["provider"]==r2::providerId&&(fields.adding||fields.values.contains(QString()))&&!localProblem(fields.profile,r2Group()).isEmpty();};
 if(op.kind==SaveOperation::Kind::Fields)return held(op);
 if(op.kind!=SaveOperation::Kind::Global)return false;
 // An action set to inherit the default output waits while that default output is still queued.
 if(!accepted.value("captureDefaults").isObject()&&setsInheritance(op)&&std::any_of(saveQueue_.cbegin(),saveQueue_.cend(),[](const SaveOperation& other){return other.key=="routing:/captureDefaults";}))return true;
 QSet<QString> ids;for(const auto& value:accepted["profiles"].toArray())ids.insert(value.toObject()["id"].toString());
 // A route to a destination whose switch to Cloudflare R2 waits, waits too: captures never follow
 // it to the service it is leaving.
 for(const auto& queuedOp:std::as_const(saveQueue_))if(held(queuedOp))ids.remove(queuedOp.id);
 for(auto it=op.values.cbegin();it!=op.values.cend();++it){
  QString id;if(it.key()=="/captureDefaults")id=it.value().toObject()["profileId"].toString();else if(it.key().startsWith("/actions/")||it.key().startsWith("/captureDefaults/")){if(it.key().endsWith("/profileId"))id=it.value().toString();else if(it.value().isObject())id=it.value().toObject()["profileId"].toString();}
  if(!id.isEmpty()&&!ids.contains(id))return true;
 }
 return false;
}
// #1704: an edit applies to a destination the resident has, an add to one it doesn't. An edit of a
// destination removed elsewhere is dropped and said so; an add whose ID was taken elsewhere is
// renamed first.
bool SettingsWindow::meetsPreconditions(SaveOperation& op){
 // A removal of an add that was in flight goes only if that add was saved.
 if(op.kind==SaveOperation::Kind::Remove){
  if(!op.adding)return true;
  // An add whose outcome was unknown: removed only if the file holds exactly what was sent.
  if(op.profile.isEmpty())return false;
  for(const auto& value:acceptedDocument()["profiles"].toArray())if(value.toObject()["id"]==op.id&&value.toObject()==op.profile){op.adding=false;return true;}
  return false;
 }
 if(op.kind!=SaveOperation::Kind::Fields)return true;
 const bool has=acceptedHas(op.id);
 if(!op.adding&&!has){
  QString label=op.profile["label"].toString();if(label.isEmpty())label=op.id;
  const auto message=label+" was removed elsewhere; your changes to it weren't saved.";
  if(!removedElsewhere_.contains(message))removedElsewhere_.append(message);
  for(auto it=notSaved_.begin();it!=notSaved_.end();)if(it.key().startsWith(op.id+"|"))it=notSaved_.erase(it);else ++it;
  dropRoutesTo(op.id);answered_=true;return false;
 }
 if(op.adding&&has){
  QStringList taken=profileOrder();for(const auto& value:acceptedDocument()["profiles"].toArray())taken.append(value.toObject()["id"].toString());
  QString fresh;for(int n=2;fresh.isEmpty()||taken.contains(fresh);++n)fresh=op.id.left(60)+"-"+QString::number(n);
  const auto from=op.id;renameAddedDestination(from,fresh);
  for(const auto& queuedOp:std::as_const(saveQueue_))if(queuedOp.key=="destination:"+fresh)op=merged(std::move(op),queuedOp);
  saveQueue_.erase(std::remove_if(saveQueue_.begin(),saveQueue_.end(),[&fresh](const SaveOperation& queuedOp){return queuedOp.key=="destination:"+fresh;}),saveQueue_.end());
  // The draft and every queued operation are renamed; this one, taken out of the queue, too.
  const auto renamed=[&](QJsonObject profile){profile["id"]=fresh;auto credentials=profile["credentials"].toObject();const auto reference=credentials["reference"].toString();if(reference.endsWith("/"+from)){credentials["reference"]=reference.chopped(from.size())+fresh;profile["credentials"]=credentials;}return profile;};
  op.id=fresh;op.key="destination:"+fresh;op.profile=renamed(op.profile);if(op.values.contains(QString()))op.values[QString()]=renamed(op.values.value(QString()).toObject());
 }
 return true;
}
// A new destination's ID, taken elsewhere before it was saved, changes everywhere this window holds it.
void SettingsWindow::renameAddedDestination(const QString& from,const QString& to){
 const auto renamed=[&](QJsonObject profile){profile["id"]=to;auto credentials=profile["credentials"].toObject();const auto reference=credentials["reference"].toString();if(reference.endsWith("/"+from)){credentials["reference"]=reference.chopped(from.size())+to;profile["credentials"]=credentials;}return profile;};
 auto profiles=draft_["profiles"].toArray();for(int i=0;i<profiles.size();++i)if(profiles[i].toObject()["id"]==from)profiles[i]=renamed(profiles[i].toObject());draft_["profiles"]=profiles;
 auto actions=draft_.value("actions").toObject();for(auto it=actions.begin();it!=actions.end();++it){auto action=it.value().toObject();if(action.value("profileId")==QJsonValue(from)){action["profileId"]=to;it.value()=action;}}draft_["actions"]=actions;
 if(auto capture=draft_.value("captureDefaults").toObject();capture.value("profileId")==QJsonValue(from)){capture["profileId"]=to;draft_["captureDefaults"]=capture;}
 // Its editor, and every service it retains to switch back to, keep references made from the new ID.
 const auto moved=[&](QString reference){return reference.endsWith("/"+from)?reference.chopped(from.size())+to:reference;};
 for(int i=0;i<profileEditors_.size();++i){auto editor=profileEditors_[i].toObject();if(editor["id"]!=from)continue;editor["id"]=to;editor["reference"]=moved(editor["reference"].toString());
  auto providers=editor["providers"].toObject();for(auto it=providers.begin();it!=providers.end();++it){auto retained=it.value().toObject();auto credentials=retained["credentials"].toObject();if(credentials.contains("reference")&&credentials["reference"].isString())credentials["reference"]=moved(credentials["reference"].toString());retained["credentials"]=credentials;auto inner=retained["editor"].toObject();if(inner.contains("reference")){inner["reference"]=moved(inner["reference"].toString());if(inner.contains("id"))inner["id"]=to;retained["editor"]=inner;}it.value()=retained;}
  editor["providers"]=providers;profileEditors_[i]=editor;}
 {const QScopedValueRollback quiet(loading_,true);
  for(auto* combo:QList<QComboBox*>{profile_,captureProfile_}+actionProfiles_.values())if(const int index=combo->findData(from);index>=0)combo->setItemData(index,to);
  if(profileId_->text()==from){profileId_->setText(to);if(reference_->text().endsWith("/"+from))reference_->setText(reference_->text().chopped(from.size())+to);}}
 for(auto& op:saveQueue_){
  if(op.id==from&&op.kind==SaveOperation::Kind::Fields){op.id=to;op.key="destination:"+to;op.profile=renamed(op.profile);if(op.values.contains(QString()))op.values[QString()]=renamed(op.values.value(QString()).toObject());}
  op.order.replaceInStrings(QRegularExpression("^"+QRegularExpression::escape(from)+"$"),to);
  for(auto it=op.values.begin();it!=op.values.end();++it){if(it.value()==QJsonValue(from)&&it.key().endsWith("/profileId"))it.value()=to;else if(it.value().isObject()&&it.value().toObject()["profileId"]==from){auto object=it.value().toObject();object["profileId"]=to;it.value()=object;}}
 }
 QMap<QString,QString> keys;for(auto it=notSaved_.cbegin();it!=notSaved_.cend();++it)keys.insert(it.key().startsWith(from+"|")?to+it.key().mid(from.size()):it.key(),it.value());notSaved_=keys;
 refreshDestinationList();
}
void SettingsWindow::queueDestinationEdit(QWidget* control,const QString& edited){
 if(profileIndex_<0)return;
 if(providerType_->currentData()=="proton-drive")updateProtonAccountDisplay();
 const auto profile=editedProfile();const auto id=profile["id"].toString();if(id.isEmpty())return;
 // A destination switched to another service here isn't saved field by field: until its new service
 // is saved, each edit sends all of it, so a field never lands in the old service's settings.
 // Only a service changed in this window counts: one another writer changed is never undone from here.
 QString path=edited;for(const auto& value:acceptedDocument()["profiles"].toArray())if(value.toObject()["id"]==id){if(!serviceSwitched_.contains(id))continue;
  // The switch is done once the file has the service it was switched to. Only an edit of that service
  // sends all of it; a form rebuilt to another service drops the mark (#1699).
  if(value.toObject()["provider"]==serviceSwitched_.value(id)||profile["provider"]!=serviceSwitched_.value(id))serviceSwitched_.remove(id);else path.clear();}
 const QString key="destination:"+id;
 SaveOperation op;op.kind=SaveOperation::Kind::Fields;op.key=key;op.id=id;op.adding=!acceptedHas(id);
 for(const auto& queuedOp:std::as_const(saveQueue_))if(queuedOp.key==key)op=queuedOp;
 // Groups held back for another service don't hold back this one's fields (#1699).
 if(op.profile["provider"]!=profile["provider"]){op.stripped.clear();op.probing.clear();}
 op.profile=profile;
 const auto group=fieldGroup(path,profile["provider"].toString());
 // A group this edit contains that can't be saved as typed keeps its accepted value: it is never
 // sent, it never blocks another destination, and it is named until it is fixed (#1701).
 bool blocked=false;
 for(const auto& checked:fieldGroups(profile["provider"].toString())){
  if(!std::any_of(checked.cbegin(),checked.cend(),[&](const QString& field){return within(field,path);}))continue;
  const auto groupKey=id+"|"+checked.join('+');
  if(const auto problem=localProblem(profile,checked);!problem.isEmpty()){notSaved_[groupKey]=problem;if(checked==group)blocked=true;}
  else notSaved_.remove(groupKey);
 }
 if(blocked){for(const auto& field:group){op.values.remove(field);op.controls.remove(field);}}
 else{
  notSaved_.remove(id+"|"+group.join('+'));notSaved_.remove(id+"|*");op.stripped.removeAll(group.join('+'));op.probing.removeAll(group.join('+'));
  SaveOperation edit;edit.kind=op.kind;edit.profile=profile;for(const auto& field:group){edit.values[field]=valueAt(profile,pathParts(field));edit.controls[field]=control;}
  op=merged(std::move(op),edit);
 }
 enqueue(std::move(op));
 if(!blocked&&!formOwnsFeedback(control))inlineError(control,{});
 dirty();drainSaves();refreshDestinationList();
 if(updateProviderVisibility_)updateProviderVisibility_();
}
void SettingsWindow::queueWholeDestination(const QString& id,QWidget* control){
 QJsonObject profile;const auto profiles=draft_["profiles"].toArray();
 for(int i=0;i<profiles.size();++i)if(profiles[i].toObject()["id"]==id)profile=i==profileIndex_?editedProfile():profiles[i].toObject();
 if(profile.isEmpty())return;
 const QString key="destination:"+id;
 // A removal of the same ID not sent yet is replaced by this; one in flight lands first, so this adds.
 saveQueue_.erase(std::remove_if(saveQueue_.begin(),saveQueue_.end(),[&](const SaveOperation& op){return op.kind==SaveOperation::Kind::Remove&&op.id==id;}),saveQueue_.end());
 SaveOperation op;op.kind=SaveOperation::Kind::Fields;op.key=key;op.id=id;op.adding=!acceptedHas(id)||removalInFlight(id);
 for(const auto& queuedOp:std::as_const(saveQueue_))if(queuedOp.key==key)op=queuedOp;
 op.profile=profile;op.values.clear();op.controls.clear();op.probing.clear();op.values[QString()]=profile;op.controls[QString()]=control;
 notSaved_.remove(id+"|*");
 for(const auto& checked:fieldGroups(profile["provider"].toString())){const auto groupKey=id+"|"+checked.join('+');if(const auto problem=localProblem(profile,checked);!problem.isEmpty())notSaved_[groupKey]=problem;else notSaved_.remove(groupKey);}
 enqueue(std::move(op));dirty();drainSaves();
}
// The destination and its queued edits go. The routes the file still has on it fall back when the
// removal is sent; a route the user chose to it is now the form's, no destination. An add that was
// never sent is only dropped here: nothing of it is in the file, and an ID used meanwhile by
// someone else is never removed.
void SettingsWindow::queueRemoval(const QString& id){
 using Kind=SaveOperation::Kind;
 const bool addInFlight=saving_&&sending_.kind==Kind::Fields&&sending_.id==id&&sending_.adding,saved=acceptedHas(id)&&!addInFlight;
 saveQueue_.erase(std::remove_if(saveQueue_.begin(),saveQueue_.end(),[&](const SaveOperation& op){return op.kind==Kind::Fields&&op.id==id;}),saveQueue_.end());
 for(auto it=notSaved_.begin();it!=notSaved_.end();)if(it.key().startsWith(id+"|"))it=notSaved_.erase(it);else ++it;
 // A queued route to it is cancelled: the route stays what the file has, with nothing queued.
 saveQueue_.erase(std::remove_if(saveQueue_.begin(),saveQueue_.end(),[&](const SaveOperation& op){return op.key.startsWith("routing:")&&std::any_of(op.values.cbegin(),op.values.cend(),[&](const QJsonValue& value){return value==QJsonValue(id)||(value.isObject()&&value.toObject().value("profileId")==QJsonValue(id));});}),saveQueue_.end());
 if(!saved&&!addInFlight){dirty();drainSaves();return;}
 SaveOperation op;op.kind=Kind::Remove;op.key="remove:"+id;op.id=id;op.adding=addInFlight;
 enqueue(std::move(op));dirty();drainSaves();
}
// An add the resident refused outright, with nothing of it queued or in flight to try again.
bool SettingsWindow::refusedAdd(const QString& id)const{
 if(!notSaved_.contains(id+"|*")||acceptedHas(id))return false;
 if(saving_&&sending_.kind==SaveOperation::Kind::Fields&&sending_.id==id)return false;
 return !std::any_of(saveQueue_.cbegin(),saveQueue_.cend(),[&id](const SaveOperation& op){return op.kind==SaveOperation::Kind::Fields&&op.id==id;});
}
bool SettingsWindow::removalInFlight(const QString& id)const{return saving_&&sending_.kind==SaveOperation::Kind::Remove&&sending_.id==id;}
// Routes to a destination that will never be saved go too, and the footer says so.
void SettingsWindow::dropRoutesTo(const QString& id,const QString& reason){
 const auto names=[&id](const QJsonValue& value){return value==QJsonValue(id)||(value.isObject()&&value.toObject().value("profileId")==QJsonValue(id));};
 const auto before=saveQueue_.size();
 saveQueue_.erase(std::remove_if(saveQueue_.begin(),saveQueue_.end(),[&](const SaveOperation& op){return op.key.startsWith("routing:")&&std::any_of(op.values.cbegin(),op.values.cend(),names);}),saveQueue_.end());
 // Actions set to inherit a default output the file doesn't have, and that is no longer queued,
 // have nothing to inherit: the resident would refuse them.
 if(!acceptedDocument().value("captureDefaults").isObject()&&!queued("/captureDefaults"))saveQueue_.erase(std::remove_if(saveQueue_.begin(),saveQueue_.end(),[](const SaveOperation& op){return op.key.startsWith("routing:/actions/")&&setsInheritance(op);}),saveQueue_.end());
 if(saveQueue_.size()!=before){const auto message=reason.isEmpty()?QStringLiteral("A route to a destination removed elsewhere wasn't saved."):reason;if(!removedElsewhere_.contains(message))removedElsewhere_.append(message);answered_=true;}
}
void SettingsWindow::queueOrder(){
 SaveOperation op;op.kind=SaveOperation::Kind::Order;op.key="order";op.order=profileOrder();op.controls[QString()]=destinationList_;
 enqueue(std::move(op));drainSaves();
}
// Capture routing: only the routes that differ from the accepted document, or that a queued or
// sent operation is changing, so sending them never puts back a route changed elsewhere that this
// window didn't touch.
QString SettingsWindow::routeOf(const QString& path){
 if(path.startsWith("/captureDefaults"))return QStringLiteral("/captureDefaults");
 if(path.startsWith("/actions/"))return path.section('/',0,2);
 return path;
}
void SettingsWindow::queueRouting(QWidget* control,const QString& onlyFor){
 const auto doc=candidate(),accepted=acceptedDocument();
 QSet<QString> removing;for(const auto& op:std::as_const(saveQueue_))if(op.kind==SaveOperation::Kind::Remove)removing.insert(op.id);if(saving_&&sending_.kind==SaveOperation::Kind::Remove)removing.insert(sending_.id);
 QMap<QString,QStringList> changed;
 for(const auto& path:routingPaths()){
  const auto route=routeOf(path);
  // A route that only falls back because its destination is being removed is the removal's,
  // worked out from the file when it is sent.
  const auto named=valueAt(accepted,pathParts(route)).toObject().value("profileId").toString(),shown=valueAt(doc,pathParts(route)).toObject().value("profileId").toString();
  // An action inheriting a default output the file doesn't have yet goes with that default output.
  const bool inherits=route.startsWith("/actions/")&&!accepted.value("captureDefaults").isObject()&&valueAt(doc,pathParts(route)).toObject().value("destination")==QJsonValue("default");
  const auto inherited=valueAt(doc,pathParts(QStringLiteral("/captureDefaults"))).toObject().value("profileId").toString();
  // After an add is saved, only the routes to it.
  if(!onlyFor.isEmpty()&&shown!=onlyFor&&!(inherits&&inherited==onlyFor))continue;
  if(removing.contains(named)&&shown.isEmpty()&&!queued(path))continue;
  // A route to an add refused outright waits for nothing: it goes once that add is saved.
  if(refusedAdd(shown)||(inherits&&refusedAdd(inherited))){const auto message=QStringLiteral("A route to a destination that wasn't saved isn't saved yet.");if(!removedElsewhere_.contains(message)){removedElsewhere_.append(message);answered_=true;}continue;}
  // A route set back to the accepted one still goes when a queued or sent operation is changing it.
  // With no default output in the file, the sharing mode shown is the Region action's: it goes
  // only if the user changed it.
  if(path=="/captureDefaults/publicationMode"&&!accepted.value("captureDefaults").isObject()&&valueAt(doc,pathParts(path))==QJsonValue(Settings::effectiveAction(accepted,"region").value("publicationMode").toString("default"))&&!queued(path))continue;
  if(valueAt(doc,pathParts(path))!=valueAt(accepted,pathParts(path))||queued(path))changed[route].append(path);
 }
 // Each field of a route goes only when it changed; destination and profileId always together. A
 // default output the file doesn't have yet is made from those when it is sent, with the default
 // sharing mode unless the user chose one (#1704, CodeRabbit).
 for(auto it=changed.begin();it!=changed.end();++it){
  auto& paths=it.value();
  if(paths.contains(it.key()+"/destination")||paths.contains(it.key()+"/profileId"))for(const auto* key:{"/destination","/profileId"})if(!paths.contains(it.key()+key))paths.append(it.key()+key);
 }
 if(!onlyFor.isEmpty()){for(auto it=changed.cbegin();it!=changed.cend();++it)queueGlobal("routing:"+it.key(),it.value(),control);if(changed.isEmpty())drainSaves();return;}
 for(auto& op:saveQueue_)if(op.key.startsWith("routing:")){const auto keep=changed.value(op.key.mid(8));for(auto it=op.values.begin();it!=op.values.end();)if(!keep.contains(it.key())){op.controls.remove(it.key());it=op.values.erase(it);}else ++it;}
 saveQueue_.erase(std::remove_if(saveQueue_.begin(),saveQueue_.end(),[](const SaveOperation& op){return op.key.startsWith("routing:")&&op.values.isEmpty();}),saveQueue_.end());
 if(changed.isEmpty()){dirty();drainSaves();return;}
 // Each route is an operation of its own, so one that waits for its destination holds no other.
 // The default output goes first: an action that inherits it needs it in the file.
 if(changed.contains("/captureDefaults"))queueGlobal("routing:/captureDefaults",changed.value("/captureDefaults"),control);
 for(auto it=changed.cbegin();it!=changed.cend();++it)if(it.key()!="/captureDefaults")queueGlobal("routing:"+it.key(),it.value(),control);
}
QStringList SettingsWindow::routingPaths()const{
 QStringList paths;for(const auto* key:{"destination","profileId","publicationMode"})paths.append(QString("/captureDefaults/")+key);
 for(const auto& name:Settings::presentationOrder())for(const auto* key:{"destination","profileId","publicationMode"})paths.append("/actions/"+name+"/"+key);
 return paths;
}
void SettingsWindow::queueGlobal(const QString& key,const QStringList& paths,QWidget* control){
 const auto doc=candidate();
 SaveOperation op;op.kind=SaveOperation::Kind::Global;op.key=key;
 for(const auto& queuedOp:std::as_const(saveQueue_))if(queuedOp.key==key)op=queuedOp;
 for(const auto& path:paths){
  // This is the newest value at this path: a queued operation sends none of its own there.
  for(auto& other:saveQueue_){if(other.key==key||other.kind==SaveOperation::Kind::Fields||other.kind==SaveOperation::Kind::Order)continue;
   // A removal keeps its routes, which it can't be saved without: they become the newest too.
   for(auto it=other.values.begin();it!=other.values.end();){if(within(it.key(),path)&&other.kind!=SaveOperation::Kind::Remove){other.controls.remove(it.key());it=other.values.erase(it);continue;}if(within(path,it.key())||within(it.key(),path))it.value()=valueAt(doc,pathParts(it.key()));++it;}}
  op.values[path]=valueAt(doc,pathParts(path));op.controls[path]=control;notSaved_.remove("|"+path);
 }
 // It goes last, after the destinations its routes may name.
 saveQueue_.erase(std::remove_if(saveQueue_.begin(),saveQueue_.end(),[&key](const SaveOperation& other){return other.key==key||(other.kind==SaveOperation::Kind::Global&&other.values.isEmpty());}),saveQueue_.end());
 saveQueue_.append(std::move(op));
 if(control&&!formOwnsFeedback(control))inlineError(control,{});
 dirty();drainSaves();
}
QString SettingsWindow::localProblem(const QJsonObject& profile,const QStringList& group)const{
 // Cloudflare R2's group is sent only when all three are valid; until then it keeps its saved values.
 if(profile["provider"]==r2::providerId&&group==r2Group()){
  for(const auto* field:{"accountId","jurisdiction","bucket"})if(const auto problem=r2::problem(profile["settings"].toObject(),field);!problem.isEmpty())return problem;
  return {};}
 if(profile["provider"]!="s3"||!group.contains("settings/region"))return {};
 const auto settings=profile["settings"].toObject();const auto endpoint=settings["endpoint"].toString();
 // The same check the form shows (updateRegionFeedback): aws-global only with Amazon S3 (#1712).
 if(!s3CloudflareR2Endpoint(endpoint))if(const auto problem=s3RegionProblem(settings["region"].toString(),endpoint);!problem.isEmpty())return problem;
 return s3EndpointProblem(endpoint,true);
}
QJsonObject SettingsWindow::acceptedDocument()const{return settings_.document().isEmpty()?Settings::defaults():settings_.document();}
// A group that can't be saved goes with the value the resident has, or the provider's default.
QJsonValue SettingsWindow::fallbackValue(const SaveOperation& op,const QJsonObject& accepted,const QString& path)const{
 for(const auto& value:accepted["profiles"].toArray()){const auto profile=value.toObject();if(profile["id"]==op.id&&profile["provider"]==op.profile["provider"])return valueAt(profile,pathParts(path));}
 return valueAt(destinationDefaults(op.profile["provider"].toString(),op.id),pathParts(path));
}
QMap<QString,QJsonValue> SettingsWindow::sendableValues(const SaveOperation& op,const QJsonObject& accepted,QJsonObject* insertion)const{
 QStringList groups=op.stripped;
 for(auto it=notSaved_.cbegin();it!=notSaved_.cend();++it)if(it.key().startsWith(op.id+"|")){const auto group=it.key().section('|',1);if(group!="*"&&!groups.contains(group))groups.append(group);}
 auto values=op.values;
 for(const auto& group:std::as_const(groups))for(const auto& field:group.split('+')){
  const auto fallback=fallbackValue(op,accepted,field);
  for(auto it=values.begin();it!=values.end();){if(it.key()==field){it=values.erase(it);continue;}if(within(field,it.key()))it.value()=withValue(it.value(),pathParts(field.mid(it.key().size())),0,fallback);++it;}
  if(insertion)*insertion=withValue(*insertion,pathParts(field),0,fallback).toObject();
 }
 return values;
}
QJsonObject SettingsWindow::applyOperation(QJsonObject doc,const SaveOperation& op)const{
 using Kind=SaveOperation::Kind;
 auto profiles=doc["profiles"].toArray();
 const auto indexOf=[&profiles](const QString& id){for(int i=0;i<profiles.size();++i)if(profiles[i].toObject()["id"]==id)return i;return -1;};
 if(op.kind==Kind::Fields){
  const int index=indexOf(op.id);
  if(index<0){
   // A destination the resident doesn't have yet goes in at its place in the list shown.
   auto profile=op.profile;sendableValues(op,doc,&profile);profile["id"]=op.id;
   // Right after the nearest destination above it that is accepted, else before the nearest below.
   const auto order=profileOrder();const int at=int(order.indexOf(op.id));int position=-1;
   for(int i=at-1;i>=0&&position<0;--i)if(const int index=indexOf(order[i]);index>=0)position=index+1;
   for(int i=at+1;at>=0&&i<order.size()&&position<0;++i)if(const int index=indexOf(order[i]);index>=0)position=index;
   // With no saved neighbour at all it goes first, before anything added elsewhere meanwhile.
   profiles.insert(position<0?(at<0?int(profiles.size()):0):position,profile);
  }else{
   QJsonValue profile=profiles[index];auto values=sendableValues(op,doc,nullptr);
   if(!op.probing.isEmpty())for(auto it=values.begin();it!=values.end();)if(fieldGroup(it.key(),op.profile["provider"].toString()).join('+')!=op.probing.first())it=values.erase(it);else ++it;
   for(auto it=values.cbegin();it!=values.cend();++it)profile=withValue(profile,pathParts(it.key()),0,it.value());
   auto object=profile.toObject();object["id"]=op.id;profiles[index]=object;
  }
  doc["profiles"]=profiles;return doc;
 }
 if(op.kind==Kind::Order){
  QJsonArray ordered;QSet<int> placed;
  for(const auto& id:op.order)if(const int i=indexOf(id);i>=0&&!placed.contains(i)){placed.insert(i);ordered.append(profiles[i]);}
  for(int i=0;i<profiles.size();++i)if(!placed.contains(i))ordered.append(profiles[i]);
  doc["profiles"]=ordered;return doc;
 }
 if(op.kind==Kind::Remove){
  if(const int index=indexOf(op.id);index>=0){profiles.removeAt(index);doc["profiles"]=profiles;}
  // The routes that still name it fall back to no destination, as the form shows them.
  auto actions=doc.value("actions").toObject();for(auto it=actions.begin();it!=actions.end();++it){auto action=it.value().toObject();if(action.value("profileId")==QJsonValue(op.id)){action["destination"]="local";action["profileId"]=QJsonValue::Null;it.value()=action;}}doc["actions"]=actions;
  if(auto capture=doc.value("captureDefaults").toObject();capture.value("profileId")==QJsonValue(op.id)){capture["destination"]="local";capture["profileId"]=QJsonValue::Null;doc["captureDefaults"]=capture;}
 }
 QJsonValue value=doc;for(auto it=op.values.cbegin();it!=op.values.cend();++it)value=withValue(value,pathParts(it.key()),0,it.value());
 auto result=value.toObject();
 // A default output made by this request takes the sharing mode the window showed for it, the
 // Region action's in the file as it is now.
 if(auto capture=result.value("captureDefaults").toObject();result.value("captureDefaults").isObject()&&!capture.contains("publicationMode")){capture["publicationMode"]=Settings::effectiveAction(doc,"region").value("publicationMode").toString("default");result["captureDefaults"]=capture;}
 return result;
}
void SettingsWindow::updateCaptureInheritance(){
 auto describe=[this](QLabel* label,bool upload){
  const bool local=keep_->isChecked();
  const auto text=upload?(local?QString("Uploads to the selected destination and keeps a local copy."):QString("Uploads to the selected destination; no local copy is kept.")):(local?QString("Uploads are off; keeps a local copy in your folders."):QString("New captures have no output. Choose a destination or turn on Keep a local copy."));
  if(label==captureOutputNotice_){label->setText(text);Ui::role(label,!upload&&!local?"warning":"secondary");}else Ui::feedback(label,text,!upload&&!local);
 };
 const auto defaultLabel=[this](QComboBox* profile){const auto resolved=destinationDefaultSharing(profile->currentData().toString());return resolved.isEmpty()?sharingLabel("default"):QString("Destination default — ")+(resolved=="private"?"private upload":"shared link");};
 captureSharing_->setItemText(captureSharing_->findData("default"),defaultLabel(captureProfile_));
 const bool upload=!captureProfile_->currentData().toString().isEmpty();captureSharing_->setEnabled(upload);describe(captureOutputNotice_,upload);
 // The default's summary only repeats the controls around it; it shows while
 // there is no output, and its actions add one.
 const bool blocked=!upload&&!keep_->isChecked()&&!changesUnsaved();captureOutputBlock_->setVisible(blocked);captureOutputNotice_->setVisible(blocked);
 if(blocked){captureOutputNotice_->setText("New captures have no output. Choose a destination or turn on Keep a local copy.");Ui::role(captureOutputNotice_,"warning");}
 chooseOutput_->setText(captureProfile_->count()>1?"Choose a destination":"Add a destination…");
 for(const auto& name:Settings::presentationOrder()){
  auto *profile=actionProfiles_[name],*sharing=publicationModes_[name];
  // Hidden inherited controls retain the resolved values for agents.
  const bool inherit=useCaptureDefault_[name]->isChecked();
  if(inherit||(name=="record"&&!recordingRoutingEdited_&&draft_["actions"].toObject()[name].toObject()["destination"]=="default")){const QSignalBlocker a(profile),b(sharing);auto* sourceProfile=inherit&&name=="record"?actionProfiles_["region"]:captureProfile_;auto* sourceSharing=inherit&&name=="record"?publicationModes_["region"]:captureSharing_;profile->setCurrentIndex(profile->findData(sourceProfile->currentData()));sharing->setCurrentIndex(sharing->findData(sourceSharing->currentData()));}
  // Inherited values are locked (readable, quietly framed); sharing without an
  // upload is unavailable.
  const bool actionUpload=!profile->currentData().toString().isEmpty();profile->setEnabled(!inherit);Ui::setLocked(profile,inherit);sharing->setEnabled(!inherit&&actionUpload);Ui::setLocked(sharing,inherit&&actionUpload);for(auto* control:{profile,sharing})control->setMinimumHeight(qMax(Ui::scaled(Ui::Metric::control),control->sizeHint().height()));
  sharing->setItemText(sharing->findData("default"),defaultLabel(profile));
  const auto inherited=[inherit,name](QComboBox* combo){return inherit?(name=="record"?": inherited from Capture region, ":": inherited from default, ")+combo->currentText():QString{};};
  profile->setAccessibleName(actionLabel(name)+" destination"+inherited(profile));sharing->setAccessibleName(actionLabel(name)+" sharing"+inherited(sharing));
  // The status line feeds view state and agents; the row's summary shows it.
  describe(captureStatus_[name],actionUpload);captureStatus_[name]->hide();
  // Inherited rows defer to the default; errors belong to real overrides.
  // Only an overridden action shows its two fields.
  {const QSignalBlocker block(captureOverride_[name]);captureOverride_[name]->setChecked(!inherit);}findChild<QWidget*>("captureException"+title(name))->setVisible(!inherit);findChild<QAction*>("add"+title(name)+"Exception")->setVisible(inherit);captureEditors_[name]->setVisible(!inherit);captureEditors_[name]->layout()->invalidate();captureEditors_[name]->setMinimumHeight(captureEditors_[name]->layout()->minimumSize().height());captureEditors_[name]->updateGeometry();
  bool unsupportedPrivate=false;
  if(actionUpload&&sharing->currentData()=="private")for(const auto& value:draft_["profiles"].toArray())if(value.toObject()["id"].toString()==profile->currentData().toString())unsupportedPrivate=value.toObject()["provider"]!="proton-drive";
  const bool fails=(!actionUpload&&!keep_->isChecked())||unsupportedPrivate;
  // An inherited failure is the default's, explained once under Destination above.
  const bool needsOutput=outputRecoveryActions_.contains(name)&&fails;
  captureSummary_[name]->setProperty("outputNeedsAttention",needsOutput);
  // An exception with no output is a warning, as under Destination: the glyph carries the tone, the words stay neutral.
  Ui::feedback(captureSummary_[name],inherit?(name=="record"?outputSummary(profile,sharing):fails?QString("Nothing is saved or uploaded"):QString("Same as above")):outputSummary(profile,sharing));Ui::role(captureSummary_[name],!inherit&&fails?"warning":"secondary");
 }
 int overrides=0;for(auto* inherit:std::as_const(useCaptureDefault_))overrides+=!inherit->isChecked();const auto summary=overrides?QString("%1 %2").arg(overrides).arg(overrides==1?"exception":"exceptions"):QString("None");findChild<QLabel*>("captureExceptionsSummary")->setText(summary);captureExceptions_->setAccessibleDescription(summary);findChild<QToolButton*>("addCaptureException")->setEnabled(overrides<Settings::presentationOrder().size());findChild<QLabel*>("recordingOutputInheritance")->setVisible(useCaptureDefault_["record"]->isChecked()&&(actionProfiles_["region"]->currentData()!=captureProfile_->currentData()||outputSummary(actionProfiles_["region"],publicationModes_["region"])!=outputSummary(captureProfile_,captureSharing_)));
 findChild<QWidget*>("captureExceptions")->setVisible(captureExceptions_->isChecked()&&overrides>0);captureExceptionRows_->invalidate();captureExceptionRows_->parentWidget()->layout()->invalidate();captureExceptionRows_->parentWidget()->updateGeometry();
}
// The output a capture gets, in the status band's words: Shared link or Private
// upload · destination, + local copy; Local only; or why the action fails.
QString SettingsWindow::outputSummary(QComboBox* profile,QComboBox* sharing)const{
 const auto id=profile->currentData().toString();const bool local=keep_->isChecked();
 if(id.isEmpty())return local?QString("Local only"):QString("Nothing is saved or uploaded");
 const auto mode=sharing->currentData().toString();const bool privateUpload=(mode=="default"?destinationDefaultSharing(id):mode)=="private";
 return QString(privateUpload?"Private upload":"Shared link")+" · "+profile->currentText()+(local?QString(" + local copy"):QString{});
}
QString SettingsWindow::destinationDefaultSharing(const QString& profileId)const{
 // Mirrors the providers' defaultPublicationMode: only Proton can default to private.
 const auto profiles=draft_["profiles"].toArray();
 for(int i=0;i<profiles.size();++i){
  if(profiles[i].toObject()["id"]!=profileId)continue;
  const auto p=i==profileIndex_?editedProfile():profiles[i].toObject();
  return p["provider"]=="proton-drive"&&!p["settings"].toObject()["shareByDefault"].toBool(true)?"private":"share";
 }
 return {};
}
QJsonObject SettingsWindow::captureRoutingState()const{
 auto route=[this](QComboBox* profile,QComboBox* sharing,QLabel* status){
  const auto id=profile->currentData().toString(),mode=sharing->currentData().toString();
  return QJsonObject{{"profileId",id},{"destination",profile->currentText()},{"publicationMode",mode},{"sharing",sharing->currentText()},{"effectivePublicationMode",id.isEmpty()?QString{}:mode=="default"?destinationDefaultSharing(id):mode},{"destinationEnabled",profile->isEnabledTo(this)},{"sharingEnabled",sharing->isEnabledTo(this)},{"status",status->text()}};
 };
 QJsonObject routing{{"default",route(captureProfile_,captureSharing_,captureOutputNotice_)}};
 for(const auto& name:Settings::presentationOrder()){auto action=route(actionProfiles_[name],publicationModes_[name],captureStatus_[name]);action["inherited"]=useCaptureDefault_[name]->isChecked();routing[name]=action;}
 return routing;
}
void SettingsWindow::queueCaptureEdit(QWidget* control){
 if(loading_||resetting_||!typingPause_)return;
 if(control==useCaptureDefault_.value("record")||control==actionProfiles_.value("record")||control==publicationModes_.value("record"))recordingRoutingEdited_=true;
 captureDefaultsEnabled_=true;captureRoutingEdited_=true;updateCaptureInheritance();
 if(initialProfile_&&captureProfile_->currentData().toString().isEmpty()&&std::all_of(actionProfiles_.cbegin(),actionProfiles_.cend(),[](auto* combo){return combo->currentData().toString().isEmpty();})){
  // Park unfinished first-launch connection details outside saved routing.
  storeProfile();provisionalProfile_={{"profile",editedProfile()},{"editor",retainedProfileEditor()}};
  if(profileIndex_>=0&&profileIndex_<credentialDrafts_.size())provisionalCredentials_=std::move(credentialDrafts_[profileIndex_]);
  loading_=true;clearCurrentCredentialInputs();draft_["profiles"]=QJsonArray{};profileEditors_={};credentialDrafts_.clear();initialProfile_=false;rebuildProfiles(-1);loading_=false;
 }
 queueRouting(control);
}
QString SettingsWindow::autosaveText()const{
 if(conflict_)return "Settings changed elsewhere — reload to continue.";
 if(autosaveBlocked_)return "Couldn’t save changes — retry saving.";
 if(!protonPassword_->text().isEmpty())return "Choose "+findControl<QPushButton*>("storeProtonPassword")->text()+" to apply the link password";
 if(!access_->text().isEmpty()||(!savedSecretShown_&&!secret_->text().isEmpty())||!token_->text().isEmpty())return "Choose "+credentialStoreButton_->text()+" to apply credentials";
 return {};
}
void SettingsWindow::clearCompletedFeedback(){
 if(!status_)return;
 if(status_->property("feedbackTone").toString()=="error"||!setupJobs_.isEmpty()||saving_||conflict_||autosaveBlocked_)return;
 Ui::feedback(plain(status_),{});
}
// Deep links finish the move: the page from its top, then the control in view
// and focused once the page has laid out.
void SettingsWindow::revealAndFocus(QWidget* target){
 QTimer::singleShot(0,this,[this,target=QPointer<QWidget>(target)]{
  if(!target)return;
  if(auto* scroll=qobject_cast<QScrollArea*>(tabs_->currentWidget())){if(auto* layout=scroll->widget()->layout())layout->activate();scroll->ensureWidgetVisible(target,24,24);}
  target->setFocus(Qt::TabFocusReason);target->setAttribute(Qt::WA_KeyboardFocusChange);target->window()->setAttribute(Qt::WA_KeyboardFocusChange);target->update();
 });
}
void SettingsWindow::showLocalCopySettings(){
 tabs_->setCurrentIndex(1);revealAndFocus(keep_);
}
void SettingsWindow::showCaptureSettings(const QStringList& affectedActions){
 outputRecoveryActions_=affectedActions;updateCaptureInheritance();
 tabs_->setCurrentIndex(1);if(auto* scroll=qobject_cast<QScrollArea*>(tabs_->widget(1)))scroll->verticalScrollBar()->setValue(0);
 QStringList targets=affectedActions;if(targets.isEmpty())for(const auto& action:Settings::presentationOrder())if(!useCaptureDefault_[action]->isChecked())targets<<action;
 QWidget* target=captureProfile_;bool overriding=false;
 for(const auto& action:std::as_const(targets))if(actionProfiles_.contains(action)&&!useCaptureDefault_[action]->isChecked()){target=actionProfiles_[action];overriding=true;break;}
 captureExceptions_->setChecked(overriding);
 revealAndFocus(target);
}
void SettingsWindow::showUpdates(){
 tabs_->setCurrentIndex(0);revealUpdates_=true;revealUpdates();
 // The action the update state offers, else Check now, else the status itself.
 QTimer::singleShot(0,this,[this]{
  QWidget* target=updateStatus_;
  for(auto* button:{restartUpdate_,installUpdate_,openUpdates_,downloadUpdate_,checkNow_})if(button->isVisibleTo(this)&&button->isEnabled()){target=button;break;}
  revealAndFocus(target);
 });
}
void SettingsWindow::revealUpdates(){
 // The status, then its actions and an install's progress below it (#293 phase 3).
 if(auto* scroll=qobject_cast<QScrollArea*>(generalPage_)){scroll->ensureWidgetVisible(updateStatus_);for(auto* row:{updateActionsRow_,installRow_,findChild<QWidget*>("updateBrowserFeedback")})if(row&&row->isVisible())scroll->ensureWidgetVisible(row);}
}
// Everything the draft holds that the accepted document doesn't, path by path: a new destination
// whole, each changed field of the others, each one gone, the order, and each changed setting.
// Used where nothing is queued yet: Retry saving after a restored checkpoint, or after a conflict.
void SettingsWindow::queueDraftDifferences(const QJsonObject& base,bool withOrder){
 // The draft is read while it still counts as restored: its Updates keys come from its own base (#1732).
 storeProfile();const auto draft=candidate();restoredDraft_=false;const auto accepted=base.isEmpty()?acceptedDocument():base;
 std::function<void(const QJsonValue&,const QJsonValue&,const QString&,QStringList&)> leaves=[&](const QJsonValue& now,const QJsonValue& before,const QString& path,QStringList& out){
  if(now==before)return;
  if(now.isObject()&&before.isObject()){const auto a=now.toObject(),b=before.toObject();auto keys=a.keys();for(const auto& key:b.keys())if(!a.contains(key))keys.append(key);for(const auto& key:std::as_const(keys))leaves(a.value(key),b.value(key),path+"/"+key,out);return;}
  out.append(path);
 };
 if(!initialProfile_){
  QHash<QString,QJsonObject> saved;QStringList savedOrder;for(const auto& value:accepted["profiles"].toArray()){const auto profile=value.toObject();saved.insert(profile["id"].toString(),profile);savedOrder.append(profile["id"].toString());}
  QStringList drafted;
  for(const auto& value:draft["profiles"].toArray()){
   const auto profile=value.toObject();const auto id=profile["id"].toString();drafted.append(id);
   if(!saved.contains(id)){queueWholeDestination(id,nullptr);for(auto& op:saveQueue_)if(op.key=="destination:"+id)op.adding=true;continue;}
   QStringList changed;leaves(profile,saved.value(id),QString(),changed);if(changed.isEmpty())continue;
   SaveOperation op;op.kind=SaveOperation::Kind::Fields;op.key="destination:"+id;op.id=id;
   for(const auto& queuedOp:std::as_const(saveQueue_))if(queuedOp.key==op.key)op=queuedOp;
   SaveOperation edit;edit.kind=op.kind;edit.profile=profile;
   for(auto path:std::as_const(changed)){
    path=path.mid(1);const auto group=fieldGroup(path,profile["provider"].toString());const auto groupKey=id+"|"+group.join('+');
    // A group that fails the form's own check stays as typed and isn't sent, as when it is typed (#1712).
    if(!notSaved_.contains(groupKey))if(const auto problem=localProblem(profile,group);!problem.isEmpty())notSaved_[groupKey]=problem;
    if(!notSaved_.contains(groupKey))for(const auto& field:group)edit.values[field]=valueAt(profile,pathParts(field));}
   op=merged(std::move(op),edit);if(!op.values.isEmpty())enqueue(std::move(op));
  }
  for(const auto& id:std::as_const(savedOrder))if(!drafted.contains(id))queueRemoval(id);
  QStringList kept;for(const auto& id:std::as_const(drafted))if(saved.contains(id))kept.append(id);QStringList was;for(const auto& id:std::as_const(savedOrder))if(drafted.contains(id))was.append(id);
  if(withOrder&&kept!=was)queueOrder();
 }
 QStringList routes,settings;
 for(const auto& key:draft.keys()+accepted.keys()){
  if(key=="profiles"||(key=="captureDefaults"&&!captureDefaultsEnabled_))continue;
  QStringList changed;leaves(draft.value(key),accepted.value(key),"/"+key,changed);
  for(const auto& path:std::as_const(changed)){auto& out=(path.startsWith("/captureDefaults")||(path.startsWith("/actions/")&&!path.endsWith("/shortcut")))?routes:settings;if(!out.contains(path))out.append(path);}
 }
 // A route object the base lacks goes field by field, the pair together, never as a whole object.
 {QStringList expanded;for(const auto& path:std::as_const(routes)){if(path!=routeOf(path)){if(!expanded.contains(path))expanded.append(path);continue;}
   const auto object=valueAt(draft,pathParts(path)).toObject();const auto shown=path=="/captureDefaults"?Settings::effectiveAction(accepted,"region").value("publicationMode").toString("default"):QStringLiteral("default");
   for(const auto* key:{"destination","profileId","publicationMode"})if(object.contains(key)&&!(QString(key)=="publicationMode"&&object.value(key)==QJsonValue(shown)))expanded.append(path+"/"+key);}
  routes=expanded;}
 // A route's destination and profile go together; the default output goes first, since an action
 // inheriting it needs it in the file.
 {QMap<QString,QStringList> byRoute;for(const auto& path:std::as_const(routes))byRoute[routeOf(path)].append(path);
  for(auto it=byRoute.begin();it!=byRoute.end();++it)if(it.value().contains(it.key()+"/destination")||it.value().contains(it.key()+"/profileId"))for(const auto* key:{"/destination","/profileId"})if(!it.value().contains(it.key()+key))it.value().append(it.key()+key);
  if(byRoute.contains("/captureDefaults"))queueGlobal("routing:/captureDefaults",byRoute.value("/captureDefaults"),nullptr);
  for(auto it=byRoute.cbegin();it!=byRoute.cend();++it)if(it.key()!="/captureDefaults")queueGlobal("routing:"+it.key(),it.value(),nullptr);}
 if(!settings.isEmpty())queueGlobal("restored",settings,nullptr);
}
// A restored checkpoint's draft against the configuration saved with it: only the user's own
// changes become operations, and edits queued since go on top of them.
bool SettingsWindow::replayRestoredDraft(){
 // An older checkpoint saved no base, and the file has changed since: what the user changed can't
 // be told from what changed elsewhere, so nothing is guessed.
 if(restoredBase_.isEmpty()){status({"config_conflict",QStringLiteral("Changes made before an update can't be merged automatically; Reload discards them."),{}});return false;}
 auto later=std::exchange(saveQueue_,{});
 // A kept destination order is never replayed; a move made since restoring is queued already (#1716).
 queueDraftDifferences(restoredBase_,false);
 for(auto& op:later){bool merged_=false;for(auto& queuedOp:saveQueue_)if(queuedOp.key==op.key){queuedOp=merged(std::move(queuedOp),op);merged_=true;break;}if(!merged_)saveQueue_.append(std::move(op));}
 checkQueue();return true;
}
void SettingsWindow::flushTyping(){
 if(!typingControl_)return;
 QWidget* control=typingControl_.data();typingControl_=nullptr;if(typingPause_)typingPause_->stop();queueEdit(control);
}
bool SettingsWindow::flushAutosave(){
 flushTyping();
 if(!autosaveBlocked_&&!conflict_&&!saveQueue_.isEmpty()&&(setupAccepting_||!setupJobs_.isEmpty())){const auto waiting=QStringLiteral("Waiting for destination setup to finish before saving your edits.");if(status_->text()!=waiting||status_->property("feedbackTone").toString()!="neutral")Ui::feedback(plain(status_),waiting);return false;}
 // The window stays open until the queue is sent, unless saving waits for Retry saving or a
 // reload, or what is left waits for a destination that couldn't be saved.
 if(autosaveBlocked_||conflict_)return true;
 drainSaves();return !saving_;
}
// The one place a save is sent: after a reply, when setup or a connection test ends, on Retry
// saving, and whenever an edit is queued while nothing is in flight.
void SettingsWindow::drainSaves(){
 if(saving_||autosaveBlocked_||conflict_||resetting_||setupAccepting_||!setupJobs_.isEmpty())return;
 for(int i=0;i<saveQueue_.size();){
  const auto accepted=acceptedDocument();
  if(waitsForDestination(saveQueue_[i],accepted)){++i;continue;}
  auto op=saveQueue_.takeAt(i);i=0;
  if(!meetsPreconditions(op))continue;
  const auto doc=applyOperation(acceptedDocument(),op);
  if(doc==acceptedDocument()){operationSettled(op);continue;}
  saving_=true;sending_=op;emit viewStateChanged();
  settings_.saveAsync(doc,observed_,[g=QPointer<SettingsWindow>(this),op,doc](Result result){if(g)g->saveAnswered(op,result,doc);});
  return;
 }
 // A switch the resident accepted is done: a later service change elsewhere is never undone from here.
 for(auto it=serviceSwitched_.begin();it!=serviceSwitched_.end();){bool done=true;for(const auto& value:acceptedDocument()["profiles"].toArray())if(value.toObject()["id"]==it.key()&&value.toObject()["provider"]!=it.value())done=false;if(done)it=serviceSwitched_.erase(it);else ++it;}
 settleDirty();updateR2Feedback();if(std::exchange(answered_,false))showUnsavedDestinations();emit viewStateChanged();
}
// What an operation's end, accepted or with nothing to send, settles beyond the accepted document.
void SettingsWindow::operationSettled(const SaveOperation& op){
 // A saved order is the one a later conflicting move goes back to (#1713).
 // A newer order settled: an older one's refusal no longer stands.
 if(op.kind==SaveOperation::Kind::Order){notSaved_.remove("|order");if(!queued("order"))reorderPending_=false;}
 if(op.values.contains(themePath)&&!queued(themePath))endAppearancePreview();
 if(uploader_&&op.kind==SaveOperation::Kind::Fields&&std::any_of(op.controls.cbegin(),op.controls.cend(),[this](const QPointer<QWidget>& control){return control==uploader_;})&&!queued(op.key))uploader_->showProblem({},{});
}
// A reply changes the accepted state and the saved or "wasn't saved" line. It never changes a
// field's text, cursor or message, since it can arrive while the next value is being typed; it may
// update the destination list's state and the stored-credential hint.
void SettingsWindow::saveAnswered(SaveOperation op,const Result& result,const QJsonObject& sent){
 saving_=false;sending_={};answered_=true;
 // A switch the resident accepted is done before anything else is sent: a later service change
 // made elsewhere is never undone from here (#1699).
 if(result&&op.kind==SaveOperation::Kind::Fields)for(const auto& value:settings_.document()["profiles"].toArray())if(value.toObject()["id"]==op.id&&serviceSwitched_.contains(op.id)&&value.toObject()["provider"]==serviceSwitched_.value(op.id))serviceSwitched_.remove(op.id);
 if(!result){
  const auto& error=result.error;const bool refused=error.code=="config_invalid"||error.code=="s3_settings_invalid"||error.code.startsWith("http_");
  // An add removed here while it was in flight, and not saved: nothing of it is kept, and the
  // removal queued for it goes too (meetsPreconditions).
  if(op.kind==SaveOperation::Kind::Fields&&!profileOrder().contains(op.id)){
   // Its removal waited on the add being saved. Refused or met with a conflict, the add wasn't
   // saved and nothing is removed. Otherwise its outcome is unknown: the removal goes only if the
   // file then holds exactly the destination that was sent, so nobody else's is ever removed.
   const bool unknown=!refused&&error.code!="config_conflict";QJsonObject added;for(const auto& value:sent["profiles"].toArray())if(value.toObject()["id"]==op.id)added=value.toObject();
   bool reconciling=false;
   for(auto it=saveQueue_.begin();it!=saveQueue_.end();)if(it->kind==SaveOperation::Kind::Remove&&it->id==op.id&&it->adding){if(unknown&&!added.isEmpty()){it->profile=added;it->reconciling=reconciling=true;++it;}else it=saveQueue_.erase(it);}else ++it;
   // The file this window knows predates the add: the removal is decided once it is read again.
   if(reconciling)settings_.refreshSnapshot([g=QPointer<SettingsWindow>(this)](Error e){if(g&&!e)g->reconciled();});
   if(refused)drainSaves();else autosaveFailure(error);return;}
  if(refused){saveRefused(std::move(op),error,sent);drainSaves();return;}
  // Anything else stops the queue with this operation first in it, for Retry saving.
  requeue(std::move(op));autosaveFailure(error);return;
 }
 baseRevision_=settings_.revision();baseDocument_=settings_.document();observed_=settings_.metadata()["observedRevision"].toString();
 if(!settings_.document()["profiles"].toArray().isEmpty())initialProfile_=false;
 if(op.kind==SaveOperation::Kind::Fields){
  notSaved_.remove(op.id+"|*");
  // Saved: what comes next for it is an edit, and a removal waiting on this add may go.
  for(auto& queuedOp:saveQueue_)if(queuedOp.key==op.key||(queuedOp.kind==SaveOperation::Kind::Remove&&queuedOp.id==op.id))queuedOp.adding=false;
  // Routes to it that the form holds and that waited for nothing go now.
  if(op.adding)queueRouting(nullptr,op.id);
  // One group of a refusal that named no field went through; the next is sent on its own.
  if(!op.probing.isEmpty()){const auto group=op.probing.takeFirst();op.adding=false;for(auto it=op.values.begin();it!=op.values.end();)if(fieldGroup(it.key(),op.profile["provider"].toString()).join('+')==group){op.controls.remove(it.key());it=op.values.erase(it);}else ++it;if(!op.values.isEmpty())requeue(op);}
 }
 autosaveBlocked_=false;for(auto* button:std::as_const(retryAutosave_))button->hide();
 operationSettled(op);
 refreshMetadata();refreshDestinationList();if(op.kind==SaveOperation::Kind::Fields&&op.id==profileId_->text())refreshCredentialHint();
 drainSaves();emit viewStateChanged();
}
void SettingsWindow::strip(SaveOperation& op,const QString& group)const{
 if(!op.stripped.contains(group))op.stripped.append(group);
 for(const auto& field:group.split('+')){op.values.remove(field);op.controls.remove(field);}
 op.probing.removeAll(group);
}
void SettingsWindow::saveRefused(SaveOperation op,const Error& error,const QJsonObject& sent){
 using Kind=SaveOperation::Kind;
 const auto field=error.details["field"].toString();
 static const QRegularExpression destinationPath("^/profiles/\\d+(?:/(.*))?$");
 const auto at=error.details["path"].toString();const auto match=destinationPath.match(at);
 if(op.kind==Kind::Fields){
  // A destination no longer in this window has nothing left to say.
  if(!profileOrder().contains(op.id))return;
  const auto id=op.id;const auto submitted=op.profile;
  const SaveOperation* newer=nullptr;for(const auto& queuedOp:std::as_const(saveQueue_))if(queuedOp.key==op.key)newer=&queuedOp;
  const auto edited=[&](const QString& group){if(!newer)return false;for(const auto& part:group.split('+'))for(auto it=newer->values.cbegin();it!=newer->values.cend();++it)if(within(part,it.key())||within(it.key(),part))return true;return false;};
  QString path=error.code=="s3_settings_invalid"&&!field.isEmpty()?"settings/"+field:match.hasMatch()&&!match.captured(1).isEmpty()?match.captured(1):QString();
  // While groups are sent one at a time, the one sent is the one refused.
  if(path.isEmpty()&&!op.probing.isEmpty())path=op.probing.first().section('+',0,0);
  bool report=false;QString refusedGroup;
  if(!path.isEmpty()){
   // Only the refused group stops, unless it was edited again since; the rest is sent again.
   refusedGroup=fieldGroup(path,op.profile["provider"].toString()).join('+');
   // While groups go one at a time, a refusal about a group this operation is itself repairing
   // (a saved Region today's rules refuse, while a fix for it waits its turn) sends that repair first.
   if(!op.probing.isEmpty()&&op.probing.first()!=refusedGroup&&op.probing.contains(refusedGroup)&&!op.promoted.contains(refusedGroup)){op.promoted.append(refusedGroup);op.probing.removeAll(refusedGroup);op.probing.prepend(refusedGroup);requeue(std::move(op));return;}
   if(!edited(refusedGroup)){strip(op,refusedGroup);notSaved_[op.id+"|"+refusedGroup]=error.message;report=true;}
   // What would go next is what was refused, such as an edit of a destination whose saved Region
   // today's rules refuse: it stops, said so, and the rest of the queue goes on.
   SaveOperation next=op;if(newer)next=merged(next,*newer);
   if(applyOperation(acceptedDocument(),next)==sent){notSaved_[id+"|*"]=error.message;report=true;saveQueue_.erase(std::remove_if(saveQueue_.begin(),saveQueue_.end(),[&](const SaveOperation& queuedOp){return queuedOp.key==op.key;}),saveQueue_.end());}
   else requeue(std::move(op));
  }else{
   QStringList fresh;for(auto it=op.values.cbegin();it!=op.values.cend();++it)if(const auto group=fieldGroup(it.key(),op.profile["provider"].toString()).join('+');!group.isEmpty()&&!edited(group)&&!fresh.contains(group))fresh.append(group);
   const bool whole=op.values.contains(QString())||!acceptedHas(op.id);
   QStringList sentGroups;for(auto it=op.values.cbegin();it!=op.values.cend();++it)if(const auto group=fieldGroup(it.key(),op.profile["provider"].toString()).join('+');!sentGroups.contains(group))sentGroups.append(group);
   if(!whole&&newer&&fresh.size()<sentGroups.size()){
    // What it refused may be a value edited again since: the newer one goes with the rest, and
    // nothing is called unsaved yet.
    requeue(std::move(op));return;
   }
   if(!whole&&fresh.size()>1){
    // A refusal that names no field: the groups not edited since go again one at a time, so the
    // refused one is found and the others save.
    op.probing=fresh;requeue(std::move(op));return;
   }
   if(!whole&&fresh.size()==1){refusedGroup=fresh.first();strip(op,refusedGroup);notSaved_[op.id+"|"+refusedGroup]=error.message;report=true;
    SaveOperation next=op;if(newer)next=merged(next,*newer);
    if(applyOperation(acceptedDocument(),next)==sent){notSaved_[id+"|*"]=error.message;saveQueue_.erase(std::remove_if(saveQueue_.begin(),saveQueue_.end(),[&](const SaveOperation& queuedOp){return queuedOp.key==op.key;}),saveQueue_.end());}
    else requeue(std::move(op));}
   else if(newer){
    // What it refused was edited again since: the newer edit decides.
    op.values.clear();op.controls.clear();requeue(std::move(op));
   }else{notSaved_[op.id+"|*"]=error.message;report=true;}
  }
  if(!report)return;
  // A destination that isn't shown is named in the footer.
  QString label=error.details["profileLabel"].toString();for(const auto& value:draft_["profiles"].toArray())if(label.isEmpty()&&value.toObject()["id"]==id)label=value.toObject()["label"].toString();if(label.isEmpty())label=id;
  // An add refused outright takes the routes waiting on it, so none waits for it indefinitely.
  if(refusedAdd(id))dropRoutesTo(id,"A route to "+label+" wasn't saved: "+label+" wasn't saved.");
  if(id!=profileId_->text()){
   for(auto* line:{status_,destinationStatus_}){plain(line);line->setProperty("errorCode",error.code);Ui::feedback(line,label+" wasn't saved: "+error.message,true);}
   return;
  }
  // The shown destination: the footer says why, about the values that were sent. A field's own
  // message stays the form's, so a refusal never lands under a value being typed.
  if(error.code=="s3_settings_invalid"&&!QStringList{"region","endpoint"}.contains(field)){
   QString bucket=submitted["settings"].toObject()["bucket"].toString();for(const auto& value:sent["profiles"].toArray())if(value.toObject()["id"]==id)bucket=value.toObject()["settings"].toObject()["bucket"].toString();QString reason;
   if(field.isEmpty()||field=="bucket"){if(bucket.isEmpty())reason="Enter a bucket name to finish setting up this destination.";else if(bucket.contains('/')||bucket.contains(':')||bucket.contains(' '))reason="Enter only the bucket name, without a URL, folder path or spaces.";else if(bucket.toUtf8().size()<3||bucket.toUtf8().size()>63)reason="Bucket names must contain 3–63 characters.";}
   const auto message=reason.isEmpty()?error.message:"Invalid S3 settings: "+reason;
   for(auto* line:{status_,destinationStatus_}){plain(line);line->setProperty("errorCode",error.code);line->setProperty("errorField",field.isEmpty()&&!reason.isEmpty()?QStringLiteral("bucket"):field);line->setToolTip(error.code);Ui::feedback(line,message,true);}
   placeConflictBanners();return;
  }
  if(error.code.startsWith("http_")&&uploader_)uploader_->showProblem(field,error.message);
  status(error);return;
 }
 QWidget* control=nullptr;
 if(op.kind==Kind::Global){
  // The refused path stops; the operation's other paths are sent again.
  QString refused;for(auto it=op.values.cbegin();it!=op.values.cend();++it)if(!at.isEmpty()&&(within(at,it.key())||within(it.key(),at)))refused=it.key();
  if(refused.isEmpty()&&op.values.size()==1)refused=op.values.firstKey();
  const bool theme=op.values.contains(themePath);
  if(!refused.isEmpty()){control=op.controls.value(refused).data();op.values.remove(refused);op.controls.remove(refused);if(!queued(refused))notSaved_["|"+refused]=error.message;if(!op.values.isEmpty())requeue(std::move(op));}
  else notSaved_["|"+op.key]=error.message;
  if(theme&&!queued(themePath))endAppearancePreview();
  if(control&&!formOwnsFeedback(control)&&control!=typingControl_)inlineError(control,error.message);
  status(error);return;
 }
 // A refused order goes back to the accepted one; a refused removal stays in the window.
 if(op.kind==Kind::Order&&!queued("order"))restoreReorderBaseline();
 else notSaved_["|"+op.key]=error.message;
 status(error);
}
// #1701: a destination that couldn't be saved is named once the queue is sent, so a save that
// went through never hides one that didn't. The shown one's own line says it already.
void SettingsWindow::showUnsavedDestinations(){
 if(conflict_||autosaveBlocked_)return;
 QString text;
 if(!removedElsewhere_.isEmpty())text=removedElsewhere_.takeFirst();
 const auto shown=profileId_->text();
 for(auto it=notSaved_.cbegin();text.isEmpty()&&it!=notSaved_.cend();++it){
  const auto id=it.key().section('|',0,0);if(id.isEmpty()||id==shown)continue;
  QString label=id;for(const auto& value:draft_["profiles"].toArray())if(value.toObject()["id"]==id)label=value.toObject()["label"].toString();
  text=label+" wasn't saved: "+it.value();
 }
 if(!text.isEmpty()){for(auto* line:{status_,destinationStatus_})if(line->text()!=text){plain(line);line->setProperty("errorCode",QStringLiteral("settings_not_saved"));Ui::feedback(line,text,true);}return;}
 // Region and Endpoint feedback belongs to the form (updateRegionFeedback); an answer leaves it.
 if(!dirty_)for(auto* label:{status_,destinationStatus_})if(!QStringList{"region","endpoint"}.contains(label->property("errorField").toString()))Ui::feedback(plain(label),{});
}
// Unsaved: queued, refused, being typed, or a draft only the form holds, such as one restored
// from a checkpoint, which is never sent by itself.
void SettingsWindow::settleDirty(){dirty_=savePending()||!notSaved_.isEmpty()||typingControl_||candidate()!=settings_.document();}
void SettingsWindow::autosaveFailure(const Error& error){
 autosaveBlocked_=true;if(error.code=="config_conflict")conflict_=true;
 // The theme shown goes back to the accepted one; the queue keeps the pick for Retry saving.
 endAppearancePreview();
 // A conflicting reorder is not kept: the list goes back to the saved order.
 const bool reordered=conflict_&&reorderPending_;if(reordered)restoreReorderBaseline();
 auto message=error;if(conflict_)message.message=conflictMessage()+QString(reordered?" The previous destination order is restored.":"");else message.message+=" Your edits remain in this window. Retry saving when ready; the request won't repeat automatically.";status(message);
 for(auto* button:std::as_const(retryAutosave_))button->setVisible(true);showConflictActions();
 settleDirty();emit viewStateChanged();
}
// #1704: Retry saving sends the kept queue. After a conflict the resident reads the file again
// first, and saving stays stopped, with the file's own line saying why, unless that file is valid:
// a refused file's revision must never let these edits overwrite it.
// The file has been read again since an add's answer was lost: removals waiting on it are decided.
void SettingsWindow::reconciled(){for(auto& op:saveQueue_)op.reconciling=false;}
void SettingsWindow::retrySaving(){
 if(resetting_)return;
 flushTyping();
 // An add's answer was lost and the file hasn't been read since: Retry waits until it is. If that
 // fails, the removal stays pending and Retry saving stays.
 if(!conflict_&&std::any_of(saveQueue_.cbegin(),saveQueue_.cend(),[](const SaveOperation& op){return op.reconciling;})){
  for(auto* button:std::as_const(retryAutosave_))button->setEnabled(false);
  settings_.refreshSnapshot([g=QPointer<SettingsWindow>(this)](Error e){
   if(!g)return;
   for(auto* button:std::as_const(g->retryAutosave_))button->setEnabled(true);
   if(e){g->status(e);emit g->viewStateChanged();return;}
   g->reconciled();g->retrySaving();});
  return;}
 if(!conflict_){autosaveBlocked_=false;for(auto* button:std::as_const(retryAutosave_))button->hide();if(restoredDraft_){if(!replayRestoredDraft()){autosaveBlocked_=true;for(auto* button:std::as_const(retryAutosave_))button->show();emit viewStateChanged();return;}}else if(!savePending())queueDraftDifferences({});drainSaves();return;}
 for(auto* button:std::as_const(retryAutosave_))button->setEnabled(false);
 settings_.reloadAsync([g=QPointer<SettingsWindow>(this)](Result result){
  if(!g)return;
  for(auto* button:std::as_const(g->retryAutosave_))button->setEnabled(true);
  g->showFileError();
  const auto code=g->settings_.metadata()["validation"].toObject()["error"].toObject()["code"].toString();
  if(!code.isEmpty()&&code!="config_missing"){emit g->viewStateChanged();return;}
  if(!result){g->status(result.error);emit g->viewStateChanged();return;}
  g->reconciled();g->retryAfterConflict();g->drainSaves();
 });
}
// Retry saving after a conflict: the window takes the file as it is now with its queued edits on
// top, and those edits are applied to it; a value that couldn't be saved stays as typed.
void SettingsWindow::retryAfterConflict(){
 // Anything typed while the file was read again goes into the queue first.
 flushTyping();
 // A restored checkpoint's own edits: what its draft holds that the configuration it was based on
 // doesn't. In a running window the queue already holds every edit, and the rest of the file is
 // someone else's.
 if(restoredDraft_&&!replayRestoredDraft()){conflict_=true;showConflictActions();emit viewStateChanged();return;}
 // A queued value equal to what this window last had accepted changes nothing of the user's, so it
 // never puts back what was changed elsewhere since.
 {QHash<QString,QJsonObject> before;for(const auto& value:baseDocument_["profiles"].toArray())before.insert(value.toObject()["id"].toString(),value.toObject());
  for(auto& op:saveQueue_){
   if(op.kind==SaveOperation::Kind::Global){
    // A route's destination and profileId stay together: both go, or neither.
    const auto unchanged=[&](const QString& path){return !op.values.contains(path)||valueAt(baseDocument_,pathParts(path))==op.values.value(path);};
    for(auto it=op.values.begin();it!=op.values.end();){const auto& path=it.key();bool drop=valueAt(baseDocument_,pathParts(path))==it.value();
     if(drop&&(path.endsWith("/destination")||path.endsWith("/profileId"))&&op.key.startsWith("routing:")){const auto route=path.section('/',0,-2);drop=unchanged(route+"/destination")&&unchanged(route+"/profileId");}
     if(drop){op.controls.remove(it.key());it=op.values.erase(it);}else ++it;}}
   else if(op.kind==SaveOperation::Kind::Fields&&before.contains(op.id)){
    // A group, such as Region with Endpoint, goes whole if any of it changed.
    const auto unchanged=[&](const QString& path){return !op.values.contains(path)||valueAt(before.value(op.id),pathParts(path))==op.values.value(path);};
    for(auto it=op.values.begin();it!=op.values.end();){const auto group=fieldGroup(it.key(),op.profile["provider"].toString());const bool drop=!it.key().isEmpty()&&std::all_of(group.cbegin(),group.cend(),unchanged);if(drop){op.controls.remove(it.key());it=op.values.erase(it);}else ++it;}}
  }
  saveQueue_.erase(std::remove_if(saveQueue_.begin(),saveQueue_.end(),[](const SaveOperation& op){return op.kind==SaveOperation::Kind::Global&&op.values.isEmpty();}),saveQueue_.end());}
 // A destination added here and never saved, with nothing queued for it, stays: it goes again as
 // an add. One this window had saved and the file no longer has was removed elsewhere.
 QSet<QString> known;for(const auto& value:baseDocument_["profiles"].toArray())known.insert(value.toObject()["id"].toString());
 for(const auto& id:profileOrder())if(!known.contains(id)&&!std::any_of(saveQueue_.cbegin(),saveQueue_.cend(),[&id](const SaveOperation& op){return op.kind==SaveOperation::Kind::Fields&&op.id==id;})){
  queueWholeDestination(id,nullptr);for(auto& op:saveQueue_)if(op.key=="destination:"+id)op.adding=true;}
 // Edits of destinations removed elsewhere are dropped; adds whose ID was taken get a fresh one.
 // By key: dropping one may drop routes queued before it, so positions move.
 {QStringList keys;for(const auto& op:std::as_const(saveQueue_))keys.append(op.key);
  for(const auto& key:std::as_const(keys)){int i=0;while(i<saveQueue_.size()&&saveQueue_[i].key!=key)++i;if(i==saveQueue_.size())continue;auto op=saveQueue_.takeAt(i);if(meetsPreconditions(op))saveQueue_.insert(qMin(i,int(saveQueue_.size())),std::move(op));}}
 // A route to a destination that neither the file has nor this window adds isn't kept.
 {QSet<QString> adding;for(const auto& op:std::as_const(saveQueue_))if(op.kind==SaveOperation::Kind::Fields&&op.adding)adding.insert(op.id);
  QSet<QString> missing;for(const auto& op:std::as_const(saveQueue_))if(op.key.startsWith("routing:"))for(auto it=op.values.cbegin();it!=op.values.cend();++it){const auto id=it.value().isObject()?it.value().toObject().value("profileId").toString():it.key().endsWith("/profileId")?it.value().toString():QString();if(!id.isEmpty()&&!acceptedHas(id)&&!adding.contains(id))missing.insert(id);}
  for(const auto& id:std::as_const(missing))dropRoutesTo(id);}
 checkQueue();
 storeProfile();
 const auto shown=profileId_->text();const auto before=candidate();const auto previousOrder=profileOrder();
 auto rebuilt=acceptedDocument();for(const auto& op:std::as_const(saveQueue_))rebuilt=applyOperation(rebuilt,op);
 auto profiles=rebuilt["profiles"].toArray();
 for(auto it=notSaved_.cbegin();it!=notSaved_.cend();++it){
  const auto id=it.key().section('|',0,0),group=it.key().section('|',1);if(id.isEmpty()||group=="*")continue;
  QJsonObject typed;for(const auto& value:before["profiles"].toArray())if(value.toObject()["id"]==id)typed=value.toObject();
  for(int i=0;i<profiles.size();++i)if(profiles[i].toObject()["id"]==id){QJsonValue profile=profiles[i];for(const auto& field:group.split('+'))profile=withValue(profile,pathParts(field),0,valueAt(typed,pathParts(field)));profiles[i]=profile;}
 }
 // A route to a destination this window added and hasn't saved yet stays in the form, to go once
 // that destination is saved. One removed elsewhere is in neither the file nor the rebuilt list.
 QSet<QString> kept;for(const auto& value:std::as_const(profiles))kept.insert(value.toObject()["id"].toString());
 for(const auto& path:routingPaths()){const auto route=routeOf(path);const auto id=valueAt(before,pathParts(route)).toObject().value("profileId").toString();if(!id.isEmpty()&&kept.contains(id)&&!acceptedHas(id))rebuilt=withValue(rebuilt,pathParts(path),0,valueAt(before,pathParts(path))).toObject();}
 // A setting the resident refused stays as typed too, so the form still shows what isn't saved.
 for(auto it=notSaved_.cbegin();it!=notSaved_.cend();++it)if(it.key().startsWith("|/")){const auto path=it.key().mid(1);rebuilt=withValue(rebuilt,pathParts(path),0,valueAt(before,pathParts(path))).toObject();}
 rebuilt["profiles"]=profiles;
 // Editors and typed credentials follow their destination by ID; a destination's editor is
 // rebuilt from its new values, keeping the other services it retains.
 QHash<QString,QJsonValue> editors;QHash<QString,QHash<QString,CredentialInputs>> credentials;
 for(int i=0;i<previousOrder.size();++i){if(i<profileEditors_.size()){auto editor=profileEditors_[i].toObject();editor.remove("id");editors.insert(previousOrder[i],editor.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(editor));}if(i<credentialDrafts_.size())credentials.insert(previousOrder[i],std::move(credentialDrafts_[i]));}
 profileEditors_={};credentialDrafts_.clear();
 for(const auto& value:std::as_const(profiles)){const auto id=value.toObject()["id"].toString();profileEditors_.append(editors.value(id,QJsonValue::Null));credentialDrafts_.append(credentials.take(id));}
 loading_=true;draft_=rebuilt;baseDocument_=settings_.document();baseRevision_=settings_.revision();observed_=settings_.metadata()["observedRevision"].toString();conflict_=false;autosaveBlocked_=false;reorderPending_=false;
 for(auto* button:std::as_const(retryAutosave_))button->hide();
 populate();loading_=true;profile_->setCurrentIndex(profile_->findData(shown));loadProfile();loading_=false;
 Ui::feedback(plain(status_),{});plain(destinationStatus_);showConflictActions();updateRegionFeedback();refreshMetadata();refreshDestinationList();refreshCredentialHint();
}
void SettingsWindow::endAppearancePreview(){
 if(!std::exchange(appearancePreviewed_,false))return;
 emit appearancePreview({});
}
QWidget* SettingsWindow::settingsField(const QString& field)const{
 if(field=="accountId")return r2AccountId_;
 if(field=="jurisdiction")return r2Jurisdiction_;
 return s3_.value(field);
}
Error SettingsWindow::moveToCloudflareR2(){
 const Error unavailable{"control_unavailable","Move to Cloudflare R2 isn't offered for this destination.",{}};
 if(loading_||providerType_->currentData()!="s3")return unavailable;
 flushTyping();storeProfile();
 auto profiles=draft_["profiles"].toArray();if(profileIndex_<0||profileIndex_>=profiles.size())return unavailable;
 auto profile=profiles[profileIndex_].toObject();
 const auto settings=r2::fromS3Settings(profile["settings"].toObject());
 if(!settings){status(unavailable);return unavailable;}
 // What Cloudflare R2 would refuse, such as a Link domain it can't use, is said first and nothing
 // moves: a refused move would otherwise save without that value while the form still shows it.
 if(const auto invalid=r2::validate(*settings)){const Error error{"r2_move_refused","Before moving to Cloudflare R2: "+invalid.message,invalid.details};status(error);return error;}
 // In place: the same ID, so routes, shortcuts and the default still name it, and the same
 // credential reference, so its stored keys stay with it.
 profile["provider"]=r2::providerId;profile["settings"]=*settings;
 while(profileEditors_.size()<profiles.size())profileEditors_.append(QJsonValue::Null);
 profileEditors_[profileIndex_]=QJsonValue::Null;
 profiles[profileIndex_]=profile;draft_["profiles"]=profiles;
 // One Fields operation on the existing ID: the service and its settings change together.
 serviceSwitched_[profile["id"].toString()]=r2::providerId;
 // The bucket now shown came with the moved account and jurisdiction (#1699).
 r2BucketFrom_[profile["id"].toString()]=(*settings)["accountId"].toString()+"|"+(*settings)["jurisdiction"].toString();
 for(auto it=notSaved_.begin();it!=notSaved_.end();)if(it.key().startsWith(profile["id"].toString()+"|"))it=notSaved_.erase(it);else ++it;
 loadProfile();dirty();queueEdit(providerType_,"/profiles/{profile}");return {};
}

void SettingsWindow::refreshCredentialHint(){
 if(!credentialHint_)return;
 findControl<QPushButton*>("unlockCredentials")->hide();credentialHint_->clear();credentialForm_->setRowVisible(PreferenceRows::rowFor(credentialHint_),false);access_->setPlaceholderText({});secret_->setPlaceholderText({});token_->setPlaceholderText("Paste a session token (optional)");
 uploader_->setStored({},false);
 if(!QStringList{"s3","backblaze-b2",r2::providerId,CustomUploaderPanel::provider()}.contains(providerType_->currentData().toString())||initialProfile_||profileIndex_<0)return;
 const auto id=profileId_->text();const auto revision=settings_.revision();
 // The saved credentials are this destination's only while it is saved as the service shown: a
 // switch not saved yet (such as S3 to Cloudflare R2) shows no other service's key (#1699).
 const auto savedAs=[this](const QString& profileId){for(const auto& value:settings_.document()["profiles"].toArray())if(value.toObject()["id"]==profileId)return value.toObject()["provider"].toString();return QString();};
 if(savedAs(id)!=providerType_->currentData().toString())return;
 settings_.request("setup.credentialHint",{{"profileId",id},{"expectedRevision",revision}},this,[g=QPointer<SettingsWindow>(this),id,revision,savedAs](Error error,QJsonObject hint){
  if(!g||g->profileId_->text()!=id||g->settings_.revision()!=revision||!QStringList{"s3","backblaze-b2",r2::providerId,CustomUploaderPanel::provider()}.contains(g->providerType_->currentData().toString())||savedAs(id)!=g->providerType_->currentData().toString())return;
  if(error)return;
  // #1361: a custom uploader stores named values. The hint says which are loaded, never a value.
  if(g->providerType_->currentData()==CustomUploaderPanel::provider()){
   QStringList stored;for(const auto& value:hint["storedValues"].toArray())stored.append(value.toString());
   const int missing=int(hint["missingValues"].toArray().size());const bool keychain=hint["mode"]=="keychain";const bool loaded=hint["available"].toBool();const bool needed=!stored.isEmpty()||missing>0;
   g->uploader_->setStored(stored,loaded||!keychain);
   g->findControl<QPushButton*>("unlockCredentials")->setVisible(keychain&&!loaded&&needed);
   g->credentialAvailability_[id]=!needed||missing==0||(keychain&&!loaded);g->refreshDestinationList();
   g->credentialForm_->setRowVisible(PreferenceRows::rowFor(g->credentialHint_),true);
   const auto where=keychain?QStringLiteral("In the system keychain"):QStringLiteral("For this session");
   if(!needed)Ui::feedback(g->credentialHint_,"This request has no secret values");
   else if(keychain&&!loaded)Ui::feedback(g->credentialHint_,"Locked in the system keychain");
   else Ui::feedback(g->credentialHint_,(stored.isEmpty()?QStringLiteral("None stored"):QString("%1 stored").arg(stored.size()))+(missing?QString(" · %1 not stored yet").arg(missing):QString{})+(stored.isEmpty()?QString{}:" · "+where));
   g->updateStoreButtons();
   return;
  }
  const bool keychain=hint["mode"]=="keychain";g->findControl<QPushButton*>("unlockCredentials")->setVisible(keychain&&!hint["available"].toBool());g->credentialAvailability_[id]=hint["available"].toBool()||keychain;g->refreshDestinationList();g->credentialForm_->setRowVisible(PreferenceRows::rowFor(g->credentialHint_),true);
  if(hint["available"].toBool()){
   // Only the short identifying prefix, never the key.
   Ui::feedback(g->credentialHint_,hint["accessKeyHint"].toString()+" · Secret key stored"+(hint["tokenStored"].toBool()?QString(" · Session token stored"):QString{})+" · "+(keychain?"In the system keychain":"For this session"));
   g->access_->setPlaceholderText("Type a new key ID to replace it");g->secret_->setPlaceholderText("Type a new secret to replace it");if(hint["tokenStored"].toBool())g->token_->setPlaceholderText("Type a new token to replace it");
  }else Ui::feedback(g->credentialHint_,keychain?"Locked in the system keychain":"None stored");
 });
}
void SettingsWindow::refreshDestinationList(const QJsonObject& document){
 if(!destinationList_)return;
 const QSignalBlocker block(destinationList_);const auto doc=document.isEmpty()?candidate():document;const auto profiles=doc["profiles"].toArray();
 findControl<QWidget*>("destinationContent")->setVisible(!profiles.isEmpty());findControl<QWidget*>("destinationScroll")->setVisible(!profiles.isEmpty());findControl<QLabel*>("destinationsEmpty")->setVisible(profiles.isEmpty());
 // Rows are updated in place: rebuilding them here, which runs when a press changes the
 // selection, would drop the view's pressed row, so the drag that follows never starts.
 while(destinationList_->count()>profiles.size())delete destinationList_->takeItem(destinationList_->count()-1);int row=0;const auto actions=doc["actions"].toObject();bool selectedDefault=false;
 for(const auto& value:profiles){const auto profile=value.toObject();bool isDefault=true;if(doc.contains("captureDefaults")){const auto capture=doc["captureDefaults"].toObject();isDefault=capture["destination"]=="upload"&&capture["profileId"]==profile["id"];}else for(const auto& action:Settings::actionNames())if(actions[action].toObject()["profileId"]!=profile["id"]||actions[action].toObject()["destination"]!="upload")isDefault=false;const auto s=profile["settings"].toObject();const bool proton=profile["provider"]=="proton-drive";QString state=proton?(s["accountRootUid"].toString().isEmpty()?"Signed out":s["folderUid"].toString().isEmpty()?"Choose a folder":checkText(profile,"Account and folder checked","Check failed","Needs checking","Configured")):profile["provider"]==CustomUploaderPanel::provider()?(s["requestUrl"].toString().isEmpty()?"Needs a request URL":(profile["id"]==profileId_->text()&&uploader_&&(!uploader_->typedSecrets().isEmpty()||!uploader_->missingSecrets().isEmpty()))||(credentialAvailability_.contains(profile["id"].toString())&&!credentialAvailability_.value(profile["id"].toString()))?"Needs credentials":checkText(profile,"Connected","Test failed","Needs testing","Ready to test")):(s["bucket"].toString().isEmpty()?"Needs a bucket":credentialAvailability_.contains(profile["id"].toString())&&!credentialAvailability_.value(profile["id"].toString())?"Needs credentials":checkText(profile,"Connected","Test failed","Needs testing","Ready to test"));if(isDefault)state="Default · "+state;auto* item=row<destinationList_->count()?destinationList_->item(row):new QListWidgetItem(destinationList_);++row;item->setIcon(Ui::icon(proton?"proton":profile["provider"]==CustomUploaderPanel::provider()?"upload":"bucket"));item->setText(profile["label"].toString()+"\n"+state);item->setToolTip(profile["label"].toString()+"\n"+state);if(profile["id"]==profileId_->text()){selectedDefault=isDefault;if(auto* status=findControl<QLabel*>("destinationProviderState")){status->setText(state);findControl<QPushButton*>("addDestinationCredentials")->setVisible(state.contains("Needs credentials"));const bool needsSetup=state.contains("Needs ")||state.contains(" failed")||state.contains("Signed out")||state.contains("Choose a folder");Ui::role(status,needsSetup?"version":"secondary");}defaultButton_->setText(checkState(profile)=="passed"?"Make default":"Make default…");}}
 destinationList_->setCurrentRow(profileIndex_);const bool has=profileIndex_>=0;if(moveUp_){moveUp_->setEnabled(profileIndex_>0);moveDown_->setEnabled(has&&profileIndex_<int(profiles.size())-1);}defaultButton_->setEnabled(has&&!selectedDefault&&!(providerType_->currentData()=="s3"&&s3_["bucket"]->text().isEmpty()));
 // The sidebar already identifies the default; only offer an action for others.
 defaultButton_->setVisible(has&&!selectedDefault);Ui::setLocked(defaultButton_,false);defaultButton_->setToolTip(selectedDefault?"Captures and clipboard uploads use this destination by default.":"Use this destination for captures and clipboard uploads by default.");
 fitRails();
}
QJsonObject SettingsWindow::profileEditorState()const{
 QJsonObject s3{{"linkMode",b2LinkMode_->currentData().toString()},{"linkValidityDays",QString::number(b2LinkDays_->value())}};for(auto it=s3_.begin();it!=s3_.end();++it)s3[it.key()]=it.value()->text();
 QJsonArray folders;for(int i=0;i<protonFolder_->count();++i)folders.append(QJsonObject{{"uid",protonFolder_->itemData(i).toString()},{"name",protonFolder_->itemData(i,protonFolderNameRole).toString()},{"label",protonFolder_->itemText(i)}});
 const QJsonObject proton{{"accountRootUid",protonAccountUid_},{"accountEmail",protonAccountEmail_},{"folders",folders},{"folderIndex",protonFolder_->currentIndex()},{"share",protonShare_->isChecked()},{"expiration",protonExpiration_->findChild<QLineEdit*>()->text()},{"timeout",protonTimeout_->findChild<QLineEdit*>()->text()}};
 return {{"proton",proton},{"r2",QJsonObject{{"accountId",r2AccountId_->text()},{"jurisdiction",r2Jurisdiction_->currentData().toString()}}},{"id",profileId_->text()},{"label",profileLabel_->text()},{"credentialMode",credentialMode_->currentData().toString()},{"reference",reference_->text()},{"acl",acl_->currentData().toString()},{"addressing",addressing_->currentData().toString()},{"s3",s3},{"providerJson",providerJson_->toPlainText()}};
}
void SettingsWindow::applyProfileEditor(const QJsonObject& state){
 profileId_->setText(state["id"].toString());profileLabel_->setText(state["label"].toString());credentialMode_->setCurrentIndex(credentialMode_->findData(state["credentialMode"].toString()));reference_->setText(state["reference"].toString());acl_->setCurrentIndex(acl_->findData(state["acl"].toString()));addressing_->setCurrentIndex(addressing_->findData(state["addressing"].toString()));const auto s3=state["s3"].toObject();if(s3.contains("linkMode"))b2LinkMode_->setCurrentIndex(qMax(0,b2LinkMode_->findData(s3["linkMode"].toString())));if(s3.contains("linkValidityDays"))b2LinkDays_->setValue(s3["linkValidityDays"].toString().toInt());for(auto it=s3_.begin();it!=s3_.end();++it)it.value()->setText(s3[it.key()].toString());s3_["region"]->setProperty("previousRegion",s3_["region"]->text());if(state.contains("r2")){const auto r2=state["r2"].toObject();r2AccountId_->setText(r2["accountId"].toString());r2Jurisdiction_->setCurrentIndex(qMax(0,r2Jurisdiction_->findData(r2["jurisdiction"].toString())));}providerJson_->setPlainText(state["providerJson"].toString());
 if(state.contains("proton")){
  const auto proton=state["proton"].toObject();protonAccountUid_=proton["accountRootUid"].toString();protonAccountEmail_=proton["accountEmail"].toString();protonFolder_->clear();
  const auto folders=proton["folders"].toArray();const int selected=proton["folderIndex"].toInt(-1);int selectedIndex=-1;
  for(int i=0;i<folders.size();++i){const auto folder=folders[i].toObject();if(folder["uid"]=="__refresh__")continue;if(i==selected)selectedIndex=protonFolder_->count();protonFolder_->addItem(folder["label"].toString(),folder["uid"].toString());protonFolder_->setItemData(protonFolder_->count()-1,folder["name"].toString(),protonFolderNameRole);}
  protonFolder_->setCurrentIndex(selectedIndex);protonShare_->setChecked(proton["share"].toBool());
  protonExpiration_->findChild<QLineEdit*>()->setText(proton["expiration"].toString());protonTimeout_->findChild<QLineEdit*>()->setText(proton["timeout"].toString());
 }
 updateProtonAccountDisplay();
}
QJsonObject SettingsWindow::saveViewState()const{
 if(saving_||setupAccepting_||resetting_)return {{"stateError","request_awaiting_acceptance"}};
 QJsonObject numbers;for(auto it=numbers_.begin();it!=numbers_.end();++it)numbers[it.key()]=it.value()->text()=="Forever"?(it.key()=="failedArtifactDays"?QString("0"):QString{}):it.value()->text();
 auto editors=profileEditors_;const auto count=draft_["profiles"].toArray().size();while(editors.size()<count)editors.append(QJsonValue::Null);if(profileIndex_>=0&&profileIndex_<count)editors[profileIndex_]=retainedProfileEditor();
 QJsonObject state{{"draft",candidate()},{"numbers",numbers},{"profileEditors",editors},{"baseRevision",baseRevision_},{"observedRevision",observed_},{"dirty",dirty_},{"autosaveBlocked",autosaveBlocked_},{"conflict",conflict_},{"initialProfile",initialProfile_},{"profileIndex",profile_->currentIndex()},{"section",tabs_->currentIndex()},{"advanced",findControl<QToolButton*>("advancedSettings")->isChecked()},{"resourceLimits",resourceLimitsToggle_->isChecked()},{"desktopSupportDetails",findChild<QToolButton*>("desktopSupportDetailsToggle")->isChecked()},{"geometry",QJsonArray{x(),y(),width(),height()}},{"setupJobs",setupJobs_},{"provisionalProfile",provisionalProfile_},{"captureRouting",captureRoutingState()}};
 // The configuration the draft was based on, when it is known (#1704).
 if(const auto base=restoredDraft_?restoredBase_:baseDocument_.isEmpty()?acceptedDocument():baseDocument_;!base.isEmpty())state["baseDocument"]=base;
 // Do not even export unclassified provider text to the resident. A content-free
 // rejection keeps the actual draft in this window until the user corrects it.
 if(auto error=validateUiState({{"schemaVersion",1},{"settings",state}}))return {{"stateError",error.code}};
 return state;
}
bool SettingsWindow::restoreViewState(const QJsonObject& state){
 if(validateUiState({{"schemaVersion",1},{"settings",state}}))return false;
 QSignalBlocker stateSignals(this);protonBrowserOpened_=false;pendingProtonUrl_.clear();pendingProtonSetup_.clear();protonBrowserOpening_=false;protonBrowserRetry_->hide();clearCredentialInputs();loading_=true;draft_=state["draft"].toObject();profileEditors_=state["profileEditors"].toArray();provisionalProfile_=state["provisionalProfile"].toObject();baseRevision_=state["baseRevision"].toString();observed_=state["observedRevision"].toString();initialProfile_=state["initialProfile"].toBool();
 populate();loading_=true;profile_->setCurrentIndex(state["profileIndex"].toInt(-1));loadProfile();const auto numbers=state["numbers"].toObject();for(auto it=numbers_.begin();it!=numbers_.end();++it)it.value()->setText(numbers[it.key()].toString());
 findChild<QToolButton*>("desktopSupportDetailsToggle")->setChecked(state["desktopSupportDetails"].toBool());resourceLimitsToggle_->setChecked(state["resourceLimits"].toBool());findControl<QToolButton*>("advancedSettings")->setChecked(state["advanced"].toBool());tabs_->setCurrentIndex(state["section"].toInt());const auto box=state["geometry"].toArray();setGeometry(box[0].toInt(),box[1].toInt(),box[2].toInt(),box[3].toInt());
 dirty_=state["dirty"].toBool();restoredDraft_=dirty_;restoredBase_=state.contains("baseDocument")?state["baseDocument"].toObject():baseRevision_==settings_.revision()?settings_.document():QJsonObject{};autosaveBlocked_=state["autosaveBlocked"].toBool();for(auto* button:std::as_const(retryAutosave_))button->setVisible(autosaveBlocked_||state["conflict"].toBool());conflict_=state["conflict"].toBool();baseDocument_=settings_.document();showConflictActions();loading_=false;updateResourceLimits();setupJobs_=state["setupJobs"].toArray();updateSetupButton();if(!setupJobs_.isEmpty()){setupTimer_->start();pollSetup();}
 // A kept destination order isn't restored: the list shows the file's, and the draft's other
 // edits replay on Retry saving (#1716). A checkpoint that held only a move then holds nothing,
 // so it isn't kept as unsaved.
 if(restoredDraft_){showFileOrder();
  if(candidate()==settings_.document()&&saveQueue_.isEmpty()&&notSaved_.isEmpty()){restoredDraft_=false;dirty_=false;autosaveBlocked_=false;conflict_=false;for(auto* button:std::as_const(retryAutosave_))button->hide();}}
 configurationChanged();stateSignals.unblock();emit viewStateChanged();return true;
}

void SettingsWindow::switchProvider(){
 if(loading_)return;
 const auto selected=providerType_->currentData().toString();
 storeProfile();
 auto profiles=draft_["profiles"].toArray();
 if(profileIndex_<0||profileIndex_>=profiles.size())return;
 auto profile=profiles[profileIndex_].toObject();
 const auto providers=profileEditors_[profileIndex_].toObject()["providers"].toObject();
 const auto retained=providers[selected].toObject();
 profile["provider"]=selected;
 if(retained.isEmpty()){
  const auto defaults=destinationDefaults(selected,profile["id"].toString());profile["settings"]=defaults["settings"];profile["credentials"]=defaults["credentials"];
 }else{
  profile["settings"]=retained["settings"];profile["credentials"]=retained["credentials"];
 }
 // ID and name belong to the destination; raw provider fields belong to its service.
 auto editor=retained["editor"].toObject();
 if(!editor.isEmpty()){editor["id"]=profile["id"];editor["label"]=profile["label"];}
 editor["providers"]=providers;profileEditors_[profileIndex_]=editor;
 profiles[profileIndex_]=profile;draft_["profiles"]=profiles;serviceSwitched_[profile["id"].toString()]=selected;
 // What wasn't saved belonged to the old service's fields: the new service's are checked afresh.
 for(auto it=notSaved_.begin();it!=notSaved_.end();)if(it.key().startsWith(profile["id"].toString()+"|"))it=notSaved_.erase(it);else ++it;
 loadProfile();dirty();queueEdit(providerType_,"/profiles/{profile}");
}
QJsonObject SettingsWindow::retainedProfileEditor()const{
 auto editor=profileEditorState();
 auto providers=profileIndex_>=0&&profileIndex_<profileEditors_.size()?profileEditors_[profileIndex_].toObject()["providers"].toObject():QJsonObject{};
 const auto profile=editedProfile();
 providers[profile["provider"].toString()]=QJsonObject{{"settings",profile["settings"]},{"credentials",profile["credentials"]},{"editor",editor}};
 editor["providers"]=providers;return editor;
}
void SettingsWindow::resetCredentialSecretDisplay(){
 ++secretRevealSerial_;
 if(savedSecretShown_){savedSecretShown_=false;secret_->setText(QString{});}
 if(secret_){secret_->setReadOnly(false);secret_->setEchoMode(QLineEdit::Password);}
 if(showSecret_){showSecret_->setChecked(false);showSecret_->setText("Show");showSecret_->setAccessibleName("Show "+secret_->accessibleName().toLower());}
}
void SettingsWindow::toggleCredentialSecret(bool shown){
 if(!shown){resetCredentialSecretDisplay();return;}
 showSecret_->setText("Hide");showSecret_->setAccessibleName("Hide "+secret_->accessibleName().toLower());
 if(!secret_->text().isEmpty()){secret_->setEchoMode(QLineEdit::Normal);return;}
 // Amazon S3 and Cloudflare R2 (#1699) keep a secret key the resident can show.
 const auto reveals=[this]{return providerType_->currentData()=="s3"||providerType_->currentData()==r2::providerId;};
 if(!reveals()||selectedDestinationChanged()||saving_||conflict_||initialProfile_){
  resetCredentialSecretDisplay();status({"save_first","Save valid destination settings before showing credentials",{}});return;
 }
 const auto serial=++secretRevealSerial_;const auto id=profileId_->text(),revision=settings_.revision();
 secret_->setReadOnly(true);
 settings_.request("setup.revealCredentialSecret",{{"profileId",id},{"expectedRevision",revision}},this,
  [this,serial,id,revision,reveals](Error error,QJsonObject value){
   if(serial!=secretRevealSerial_)return;
   if(profileId_->text()!=id||settings_.revision()!=revision||!reveals()||selectedDestinationChanged()){
    resetCredentialSecretDisplay();return;
   }
   if(error){resetCredentialSecretDisplay();status(error);return;}
   savedSecretShown_=true;secret_->setEchoMode(QLineEdit::Normal);secret_->setText(value["secretKey"].toString());
  });
}
void SettingsWindow::rememberCredentialInputs(){
 resetCredentialSecretDisplay();
 const auto profiles=draft_["profiles"].toArray();
 if(profileIndex_<0||profileIndex_>=profiles.size())return;
 credentialDrafts_.resize(profiles.size());
 credentialDrafts_[profileIndex_][profiles[profileIndex_].toObject()["provider"].toString()]={access_->text(),secret_->text(),token_->text(),protonPassword_->text(),uploader_->typedSecrets()};
}
void SettingsWindow::restoreCredentialInputs(){
 resetCredentialSecretDisplay();
 const auto profiles=draft_["profiles"].toArray();
 CredentialInputs inputs;
 if(profileIndex_>=0&&profileIndex_<profiles.size()&&profileIndex_<credentialDrafts_.size())
  inputs=credentialDrafts_[profileIndex_].value(profiles[profileIndex_].toObject()["provider"].toString());
 access_->setText(inputs.access);secret_->setText(inputs.secret);token_->setText(inputs.token);protonPassword_->setText(inputs.password);uploader_->setTypedSecrets(inputs.values);
}
void SettingsWindow::clearCurrentCredentialInputs(){
 resetCredentialSecretDisplay();
 const auto profiles=draft_["profiles"].toArray();
 if(profileIndex_>=0&&profileIndex_<profiles.size()&&profileIndex_<credentialDrafts_.size())
  credentialDrafts_[profileIndex_].remove(profiles[profileIndex_].toObject()["provider"].toString());
 access_->clear();secret_->clear();token_->clear();protonPassword_->clear();uploader_->clearTypedSecrets();
}
void SettingsWindow::importDevelopmentProfile(){
 if(initialProfile_&&s3_["bucket"]->text().isEmpty()){
  draft_["profiles"]=QJsonArray{};profileEditors_={};credentialDrafts_.clear();profileIndex_=-1;for(auto* combo:std::as_const(actionProfiles_))combo->setCurrentIndex(0);
 }else storeProfile();
 draft_=candidate();auto profiles=draft_["profiles"].toArray();const auto development=Settings::developmentTemplate()["profiles"].toArray().first();
 int index=-1;for(int i=0;i<profiles.size();++i)if(profiles[i].toObject()["id"]=="development-s3")index=i;
 if(index<0){index=profiles.size();profiles.append(development);}else profiles[index]=development;
 while(profileEditors_.size()<profiles.size())profileEditors_.append(QJsonValue::Null);
 profileEditors_[index]=QJsonValue::Null;credentialDrafts_.resize(profiles.size());credentialDrafts_[index].clear();
 draft_["profiles"]=profiles;initialProfile_=false;rebuildProfiles(index,"development-s3");dirty();queueWholeDestination("development-s3",defaultButton_);queueRouting(defaultButton_);refreshDestinationList();
}

void SettingsWindow::clearCredentialInputs(){
 resetCredentialSecretDisplay();
 credentialDrafts_.clear();provisionalCredentials_.clear();
 if(protonPassword_)protonPassword_->clear();
 access_->clear();secret_->clear();token_->clear();if(uploader_)uploader_->clearTypedSecrets();
}
void SettingsWindow::closeEvent(QCloseEvent* event){clearCredentialInputs();RetainedWindow::closeEvent(event);}
Error SettingsWindow::actionSaveError()const{
 if(!conflict_&&!autosaveBlocked_&&!dirty_)return {};
 // A failed save or a conflict has already said why; otherwise the draft just isn't saved yet.
 const auto message=status_->property("feedbackTone")=="error"&&!status_->text().isEmpty()?status_->text():QStringLiteral("Some changes in Settings aren’t saved yet. Finish or undo them, then try again.");
 return {"settings_not_saved",message,{}};
}
bool SettingsWindow::hasCredentialInputs()const{
 if(!access_->text().isEmpty()||(!savedSecretShown_&&!secret_->text().isEmpty())||!token_->text().isEmpty()||!protonPassword_->text().isEmpty()||(uploader_&&!uploader_->typedSecrets().isEmpty()))return true;
 for(int index=0;index<credentialDrafts_.size();++index){
  const auto& destination=credentialDrafts_[index];
  for(auto it=destination.cbegin();it!=destination.cend();++it){
   if(index==profileIndex_&&it.key()==providerType_->currentData().toString())continue;
   const auto& inputs=it.value();
   if(!inputs.empty())return true;
  }
 }
 for(const auto& inputs:provisionalCredentials_)
  if(!inputs.empty())return true;
 return false;
}
bool SettingsWindow::hasRetainedDrafts()const{
 if(!provisionalProfile_.isEmpty()||hasCredentialInputs())return true;
 for(const auto& editor:profileEditors_)if(editor.toObject()["providers"].toObject().size()>1)return true;
 return false;
}
void SettingsWindow::configurationChanged(){
 if(baseRevision_!=settings_.revision()||observed_!=settings_.metadata()["observedRevision"].toString())resetCredentialSecretDisplay();
 refreshMetadata();
 showFileError();
 if(saving_||resetting_)return;
 if(conflict_||((dirty_||hasRetainedDrafts())&&(baseRevision_!=settings_.revision()||observed_!=settings_.metadata()["observedRevision"].toString()))){
  // Keep a conflict message that is already shown, such as a restored destination order.
  // A move not saved yet goes back, as when a save's answer is the conflict (autosaveFailure).
  const bool reordered=!conflict_&&reorderPending_;
  const bool shown=conflict_&&!status_->text().isEmpty();
  // Already in a conflict, with no move waiting: the list shows the order the file has now, which a
  // rollback at the conflict's answer could only take from what the window knew then.
  if(conflict_&&!reorderPending_)showFileOrder();
  conflict_=true;if(reordered)restoreReorderBaseline();
  if(!shown)status({"config_conflict",conflictMessage()+QString(reordered?" The previous destination order is restored.":""),{}});for(auto* button:std::as_const(retryAutosave_))button->show();showConflictActions();emit viewStateChanged();return;}
 if(!dirty_&&(baseRevision_!=settings_.revision()||observed_!=settings_.metadata()["observedRevision"].toString()))refresh();
 auto error=settings_.metadata()["validation"].toObject()["error"].toObject();if(error["code"].toString()=="config_missing")status({error["code"].toString(),error["message"].toString(),error["details"].toObject()});
}
QString SettingsWindow::checkState(const QJsonObject& profile)const{
 const auto record=settings_.destinationStatus()[profile["id"].toString()].toObject();
 const auto state=record["state"].toString();
 if((state!="passed"&&state!="failed")||record["fingerprint"].toString()!=destinationcheck::fingerprint(profile))return QStringLiteral("untested");
 return state;
}
QString SettingsWindow::checkText(const QJsonObject& profile,const QString& passed,const QString& testFailed,const QString& uploadFailed,const QString& untested)const{
 const auto state=checkState(profile);
 if(state=="passed")return passed;
 if(state=="failed")return settings_.destinationStatus()[profile["id"].toString()].toObject()["source"]=="upload"?uploadFailed:testFailed;
 return untested;
}
void SettingsWindow::updateProtonAccountDisplay(){
 const bool connected=!protonAccountUid_.isEmpty();const bool tested=checkState(editedProfile())=="passed";
 PreferenceRows::sectionFor(findControl<QWidget*>("protonFolderSection")->findChild<QFormLayout*>())->setVisible(connected);findControl<QWidget*>("protonFolderPrerequisite")->setVisible(!connected);
 protonAccount_->setText(tested?QString("Account and folder checked · Upload and sharing aren't tested"):connected?(protonAccountEmail_.isEmpty()?QString("Account configured"):protonAccountEmail_):QString("Not connected"));
 findControl<QPushButton*>("connectProtonAccount")->setText(connected?"Reconnect account…":"Connect Proton Drive…");sizeProtonAccountButtons();
}
// Proton's account buttons share the widest visible label; both Connect labels count, so reconnecting never resizes them.
void SettingsWindow::sizeProtonAccountButtons(){
 auto* connect=findControl<QPushButton*>("connectProtonAccount");auto* test=findControl<QPushButton*>("testDestination");const bool proton=providerType_->currentData()=="proton-drive";
 for(auto* button:{connect,protonBrowserRetry_,test}){button->setMinimumWidth(0);button->setMaximumWidth(QWIDGETSIZE_MAX);button->ensurePolished();}
 if(!proton)return;
 const auto shown=connect->text();int width=qMax(test->sizeHint().width(),protonBrowserRetry_->isHidden()?0:protonBrowserRetry_->sizeHint().width());for(const auto& text:{"Connect Proton Drive…","Reconnect account…"}){connect->setText(text);width=qMax(width,connect->sizeHint().width());}connect->setText(shown);
 for(auto* button:{connect,protonBrowserRetry_,test})button->setFixedWidth(width);
}
// An explicit Proton action (connect) writes what it found; the passive check when Destinations
// opens only refreshes the folder list and never changes or saves the destination.
bool SettingsWindow::applyProtonAccountResult(const QString& id,const QJsonObject& result,bool write){
 auto profiles=draft_["profiles"].toArray();int index=-1;for(int i=0;i<profiles.size();++i)if(profiles[i].toObject()["id"]==id){index=i;break;}
 if(index<0||profiles[index].toObject()["provider"]!="proton-drive"||result["accountRootUid"].toString().isEmpty())return false;
 storeProfile();profiles=draft_["profiles"].toArray();auto profile=profiles[index].toObject();auto settings=profile["settings"].toObject();
 if(index>=profileEditors_.size()||!profileEditors_[index].toObject().contains("id"))return false;
 auto editor=profileEditors_[index].toObject();const auto previous=editor["proton"].toObject();const auto root=result["accountRootUid"].toString();
 static const QRegularExpression controlCharacter("[\\x00-\\x1f\\x7f]");
 auto email=result["accountEmail"].toString();if(email.size()>320||email.contains(controlCharacter))email.clear();
 if(email.isEmpty()&&previous["accountRootUid"]==root)email=previous["accountEmail"].toString();
 // The passive check never changes the account: one that differs is reported as changed.
 if(!write&&settings["accountRootUid"].toString()!=root)return false;
 if(settings["accountRootUid"]!=root){settings["folderUid"]="";settings.remove("folderName");}
 settings["accountRootUid"]=root;const auto selected=settings["folderUid"].toString();QJsonArray folders;int selectedIndex=0;
 folders.append(QJsonObject{{"uid",""},{"name",""},{"label","Choose a folder"}});
 for(const auto& value:result["folders"].toArray()){
  const auto folder=value.toObject();const auto uid=folder["uid"].toString(),name=folder["name"].toString();if(uid.isEmpty())continue;
  if(uid==selected){selectedIndex=folders.size();if(write)settings["folderName"]=name;else{const auto saved=settings.value("folderName").toString();folders.append(QJsonObject{{"uid",uid},{"name",saved},{"label",saved.isEmpty()?QString("Saved Proton folder"):saved}});continue;}}
  folders.append(QJsonObject{{"uid",uid},{"name",name},{"label",name.isEmpty()?QString("Saved Proton folder"):name}});
 }
 if(!selected.isEmpty()&&selectedIndex==0){selectedIndex=folders.size();const auto name=settings.value("folderName").toString();folders.append(QJsonObject{{"uid",selected},{"name",name},{"label",name.isEmpty()?QString("Saved Proton folder"):name}});}
 auto proton=previous;proton["accountRootUid"]=root;proton["accountEmail"]=email;proton["folders"]=folders;proton["folderIndex"]=selectedIndex;editor["proton"]=proton;
 auto providers=editor["providers"].toObject();auto retained=providers["proton-drive"].toObject();auto providerEditor=retained["editor"].toObject();providerEditor["proton"]=proton;retained["editor"]=providerEditor;retained["settings"]=settings;providers["proton-drive"]=retained;editor["providers"]=providers;profileEditors_[index]=editor;
 profile["settings"]=settings;profiles[index]=profile;draft_["profiles"]=profiles;
 if(index==profileIndex_){const bool previousLoading=loading_;loading_=true;applyProfileEditor(editor);loading_=previousLoading;}
 // Only the settings go, matched to the destination by ID.
 if(write){QJsonObject saved;for(const auto& value:settings_.document()["profiles"].toArray())if(value.toObject()["id"]==id)saved=value.toObject();
  if(saved.isEmpty()||saved["settings"]!=settings){
   SaveOperation op;op.kind=SaveOperation::Kind::Fields;op.key="destination:"+id;op.id=id;op.adding=!acceptedHas(id);
   for(const auto& queuedOp:std::as_const(saveQueue_))if(queuedOp.key==op.key)op=queuedOp;
   SaveOperation edit;edit.kind=op.kind;edit.profile=profile;edit.values["settings"]=settings;edit.controls["settings"]=protonFolder_;
   op=merged(std::move(op),edit);enqueue(std::move(op));dirty();drainSaves();
  }}
 refreshDestinationList();return true;
}
void SettingsWindow::activateDestinations(bool forceRefresh){refreshUploadedCount();
 if(loading_||selectedDestinationChanged()||saving_||conflict_||initialProfile_||setupAccepting_||!setupJobs_.isEmpty())return;
 if(providerType_->currentData()!="proton-drive"||protonAccountUid_.isEmpty())return;
 if(!settings_.diagnostics()["capabilities"].toObject()["providers"].toObject().contains("proton-drive"))return;
 const auto key=profileId_->text()+"/"+protonAccountUid_;
 if(!forceRefresh&&requestedFolders_.contains(key))return;
 requestedFolders_.insert(key);beginSetup("inspectAccount");
}
void SettingsWindow::setupFeedback(const QString& message,bool success){
 for(auto* label:{status_,destinationStatus_}){
  plain(label);if(success)Ui::success(label,message);else{Ui::feedback(label,message);label->setProperty("setupProgress",true);}
 }
}
QString SettingsWindow::setupBlockedReason()const{
 if(conflict_)return "Resolve the settings conflict before saving credentials";
 if(autosaveBlocked_)return "Retry saving your changes before storing. Typed values are kept here.";
 if(savePending())return "Wait for settings to finish saving. Typed values are kept here.";
 if(initialProfile_||profile_->currentIndex()<0||selectedDestinationChanged())return "Finish valid destination edits to continue. Typed values are kept here.";
 if(setupAccepting_||!setupJobs_.isEmpty())return "Wait for destination setup to finish. Typed values are kept here.";
 return {};
}
bool SettingsWindow::admitSetup(){
 const auto reason=setupBlockedReason();if(reason.isEmpty())return true;
 updateSetupButton();status({"save_first",reason,{}});return false;
}
void SettingsWindow::beginSetup(const QString& kind,const QJsonObject& input){
 if(selectedDestinationChanged()||saving_||conflict_||initialProfile_||profile_->currentIndex()<0){status({"save_first","Finish valid destination edits before connecting. Changes save automatically.",{}});return;}
 const auto profileId=profileId_->text(),revision=settings_.revision();QJsonObject params{{"profileId",profileId},{"expectedRevision",revision}};if(kind=="storeCredentials")params["credentials"]=input;else if(kind=="createFolder"||kind=="storeSharePassword")params["input"]=input;
 const auto testMessage=providerType_->currentData()=="proton-drive"?QString("Checking the Proton account and selected folder…"):QString("Testing upload, link and cleanup…");
 if(kind=="testDestination"){testAccepting_=true;setupFeedback(testMessage);}
 updateSetupButton();
 if(kind=="connectAccount"||kind=="inspectAccount")folderRequests_.insert(profileId);
 ++setupAccepting_;updateSetupButton();emit viewStateChanged();
 settings_.request("setup."+kind,params,this,[this,kind,profileId,revision,testMessage](Error e,QJsonObject data){
  folderRequests_.remove(profileId);--setupAccepting_;if(kind=="testDestination")testAccepting_=false;
  if(e){updateSetupButton();status(e);if(kind=="connectAccount"&&profileId==profileId_->text())protonAccount_->setText("Connection failed: "+e.message);emit viewStateChanged();return;}
  // Only an admitted request consumes typed secrets; a refusal preserves them.
  if(profileId==profileId_->text()){
   // A custom uploader's values often come from an imported file: they are kept until the store has succeeded.
   if(kind=="storeCredentials"&&providerType_->currentData()!=CustomUploaderPanel::provider())clearCurrentCredentialInputs();
   else if(kind=="storeSharePassword"){protonPassword_->clear();rememberCredentialInputs();}
  }
  for(int i=setupJobs_.size()-1;i>=0;--i){const auto old=setupJobs_[i].toObject();if(old["kind"]==kind&&old["profileId"]==profileId)setupJobs_.removeAt(i);}
  setupJobs_.append(QJsonObject{{"setupId",data["setupId"]},{"kind",kind},{"profileId",profileId},{"revision",revision},{"fingerprint",destinationcheck::fingerprint(editedProfile())}});
  // Say what is happening; the confirmed result replaces it. Folder loading is
  // shown beside the folder, so the page status doesn't repeat it.
  static const QHash<QString,QString> progress{{"connectAccount","Connecting account…"},{"storeCredentials","Storing credentials…"},{"unlockCredentials","Unlocking saved credentials…"},{"createFolder","Creating folder…"},{"storeSharePassword","Storing link password…"}};
  updateSetupButton();if(kind=="inspectAccount")Ui::feedback(plain(destinationStatus_),{});else setupFeedback(kind=="testDestination"?testMessage:progress.value(kind,"Working…"));
  if(kind=="connectAccount"&&profileId==profileId_->text())protonAccount_->setText("Connecting to Proton…");
  emit viewStateChanged();setupTimer_->start();pollSetup();
 });
}
// ui.customUploader (#1361): the open Custom uploader form, driven as a user drives it.
// The reply is the form's state: fields and which secret values are typed or stored,
// never a value.
void SettingsWindow::uploaderCommand(const QJsonObject& params,std::function<void(Error,QJsonObject)> reply){
 const auto action=params["action"].toString();
 const auto requested=params["profileId"].toString();
 if(!requested.isEmpty()&&requested!=profileId_->text()){const int index=profile_->findData(requested);if(index<0){reply({"profile_not_found","No destination has this ID.",{}},{});return;}profile_->setCurrentIndex(index);}
 if(profileIndex_<0||providerType_->currentData()!=CustomUploaderPanel::provider()){reply({"uploader_not_selected","Select a custom uploader destination in Destinations first, or name it with profileId.",{}},{});return;}
 const auto answer=[this,reply](Error error){
  auto state=uploader_->state();state["profileId"]=profileId_->text();state["label"]=profileLabel_->text();
  state["storeEnabled"]=findControl<QPushButton*>("storeCredentials")->isEnabled();state["storeBlocked"]=setupBlockedReason();
  state["saved"]=!selectedDestinationChanged()&&!savePending();state["status"]=destinationStatus_?destinationStatus_->text():QString{};
  state["storedHint"]=credentialHint_->text();
  reply(error,state);
 };
 if(action=="inspect"){answer({});return;}
 if(action=="import"){answer(uploader_->importFile(params["path"].toString()));return;}
 if(action=="openImport"){uploader_->openImport();answer({});return;}
 if(action=="closeImport"){if(auto* dialog=uploader_->findChild<QDialog*>("importSxcuDialog"))dialog->reject();answer({});return;}
 if(action=="store"||action=="test"){
  auto* button=findControl<QPushButton*>(action=="store"?"storeCredentials":"testDestination");
  if(!button->isEnabled()||!button->isVisible()){answer({"setup_busy",action=="store"&&!setupBlockedReason().isEmpty()?setupBlockedReason():QStringLiteral("That action is not available right now."),{}});return;}
  button->click();answer({});return;
 }
 reply({"invalid_request","Unknown custom uploader action.",{}},{});
}
void SettingsWindow::r2Command(const QJsonObject& params,std::function<void(Error,QJsonObject)> reply){
 const auto action=params["action"].toString();
 const auto shown=[this](QWidget* widget){return widget&&widget->isVisibleTo(destinationPage_);};
 // A person can only act on a control they can see and use; neither can an agent.
 const auto usable=[this,&shown](QWidget* widget){return shown(widget)&&widget->isEnabled();};
 const Error unavailable{"control_unavailable","That control isn't shown or can't be used for this destination right now.",{}};
 // Destinations as a person sees it: open, and not behind a dialog that blocks its controls.
 if(!destinationPage_||!destinationPage_->isVisible()){reply({"ui_view_closed","Open Destinations before driving it.",{}},{});return;}
 if(QApplication::activeModalWidget()){reply(unavailable,{});return;}
 if(action=="add"){
  // Add destination → Cloudflare R2, named as the dialog would be.
  // The Add button as a person finds it: shown and usable, or the agent can't use it either.
  if(!usable(addDestination_)){reply(unavailable,{});return;}
  const auto count=profile_->count();addS3Profile("Cloudflare R2",params["label"].toString(),r2::providerId);
  if(profile_->count()==count){reply({"profile_limit","The destination could not be added.",{}},{});return;}
  providerType_->setCurrentIndex(providerType_->findData(r2::providerId));queueEdit(profileLabel_);
 }else{
  // Another destination is picked in the destination list, as a person picks it: only while the list
  // is shown and usable (#1699).
  const auto requested=params["profileId"].toString();
  if(!requested.isEmpty()&&requested!=profileId_->text()){const int index=profile_->findData(requested);if(index<0){reply({"profile_not_found","No destination has this ID.",{}},{});return;}if(!usable(destinationList_)){reply(unavailable,{});return;}profile_->setCurrentIndex(index);}
 }
 const auto provider=providerType_->currentData().toString();
 // Move to Cloudflare R2 that isn't offered for the selected destination, whatever its service, is an
 // unavailable control.
 if(action=="move"){auto* move=findControl<QPushButton*>("moveToCloudflareR2");if(profileIndex_<0||provider!="s3"||!usable(move)){reply(unavailable,{});return;}if(const auto error=moveToCloudflareR2()){reply(error,{});return;}}
 // After a Move the destination is Cloudflare R2: what is selected now decides the rest.
 if(const auto now=providerType_->currentData().toString();profileIndex_<0||(now!=r2::providerId&&!(now=="s3"&&action=="inspect"))){reply({"r2_not_selected","Select a Cloudflare R2 destination in Destinations first, or name it with profileId.",{}},{});return;}
 // What a person does: type a value and leave the field, pick an item, step a number or open a section.
 if(action=="set"){
  const auto field=params["field"].toString(),value=params["value"].toString();
  auto* advanced=findControl<QToolButton*>("advancedSettings");
  if(field=="advanced"){if(!usable(advanced)){reply(unavailable,{});return;}if(advanced->isChecked()!=(value=="open"))advanced->click();}
  else if(field=="jurisdiction"||field=="linkMode"){
   auto* box=field=="jurisdiction"?r2Jurisdiction_:b2LinkMode_;const int index=box->findData(value);
   if(index<0){reply({"invalid_request",field=="jurisdiction"?QStringLiteral("Jurisdiction is default, eu or fedramp."):QStringLiteral("Who can open files is public or presigned."),{}},{});return;}
   if(!usable(box)){reply(unavailable,{});return;}box->setCurrentIndex(index);emit box->activated(index);
  }else if(field=="linkValidityDays"){
   bool number=false;const int days=value.toInt(&number);
   if(!number||days<b2LinkDays_->minimum()||days>b2LinkDays_->maximum()){reply({"invalid_request","Links last 1–7 days.",{}},{});return;}
   if(!usable(b2LinkDays_)){reply(unavailable,{});return;}b2LinkDays_->setValue(days);emit b2LinkDays_->editingFinished();
  }else{
   auto* edit=field=="accountId"?r2AccountId_:s3_.value(field);
   if(!usable(edit)){reply(unavailable,{});return;}
   edit->setFocus();edit->setText(value);edit->setModified(true);emit edit->editingFinished();
  }
 }
 // A paste into Account ID, as Ctrl+V does: marked as a paste, so the S3 API address is read and the
 // three fields are queued as one group.
 if(action=="paste"){if(!usable(r2AccountId_)){reply(unavailable,{});return;}const QScopedValueRollback paste(pasting_,QPointer<QWidget>(r2AccountId_));r2AccountId_->setText(params["text"].toString());}
 const auto message=[this,&shown](QWidget* control){auto* label=errors_.value(control);return shown(label)?label->text():QString();};
 auto* warning=findControl<QLabel*>("r2LinkDomainWarning");auto* hint=findControl<QLabel*>("s3BucketHint");
 const bool r2=providerType_->currentData()==r2::providerId;
 const auto settings=editedProfile()["settings"].toObject();
 QJsonObject state{{"profileId",profileId_->text()},{"label",profileLabel_->text()},{"provider",providerType_->currentData().toString()},
  {"saved",!selectedDestinationChanged()&&!savePending()},{"moveAvailable",usable(findControl<QPushButton*>("moveToCloudflareR2"))}};
 if(r2){
  for(const auto& key:QStringList{"accountId","jurisdiction","bucket","objectPrefix","linkMode","linkValidityDays","publicUrlBase"})state[key]=settings[key];
  // Advanced connection settings as shown, in seconds and KB/s.
  for(const auto& key:QStringList{"connectTimeoutMs","requestTimeoutMs","lowSpeedBytesPerSecond","lowSpeedTimeSeconds"})state[key]=s3_[key]->text();
  state["advancedOpen"]=findControl<QToolButton*>("advancedSettings")->isChecked();
  // The endpoint requests go to, as Advanced connection settings shows it, and each field's message.
  state["endpoint"]=r2::endpoint(r2AccountId_->text(),r2Jurisdiction_->currentData().toString());
  state["bucketHint"]=hint?hint->text():QString();state["linkDomainWarning"]=shown(warning)?QJsonValue(warning->text()):QJsonValue();
  state["unitStatus"]=shown(r2UnitStatus_)?QJsonValue(r2UnitStatus_->text()):QJsonValue();
  state["errors"]=QJsonObject{{"accountId",message(r2AccountId_)},{"jurisdiction",message(r2Jurisdiction_)},{"bucket",message(s3_["bucket"])},{"publicUrlBase",message(s3_["publicUrlBase"])}};
 }
 reply({},state);
}
void SettingsWindow::allowControl(QWidget* control,bool allowed){control->setProperty("providerEnabled",allowed);control->setEnabled(allowed&&!setupBusy_);}
void SettingsWindow::updateSetupButton(){
 const auto id=profileId_->text();bool foldersLoading=folderRequests_.contains(id);
 for(const auto& value:std::as_const(setupJobs_)){const auto job=value.toObject();if(job["profileId"]==id&&(job["kind"]=="connectAccount"||job["kind"]=="inspectAccount"))foldersLoading=true;}
 protonFolderStatus_->setText(foldersLoading?(pendingProtonUrl_.isEmpty()?QString("Loading folders…"):QString("Waiting for Proton sign-in…")):QString{});protonFolderStatus_->setVisible(foldersLoading&&providerType_->currentData()=="proton-drive");
 const bool busy=setupAccepting_||!setupJobs_.isEmpty();
 // While setup runs only its actionable controls are disabled: names, account
 // identity, loading text and help stay readable.
 setupBusy_=busy;
 for(auto* container:{findControl<QWidget*>("destinationEditor"),findControl<QWidget*>("credentialsEditor"),linksPanel_,connectionPanel_,static_cast<QWidget*>(protonPanel_),static_cast<QWidget*>(uploader_),findControl<QWidget*>("protonAdvancedSettings")})
  for(auto* control:container->findChildren<QWidget*>()){
   const bool editor=qobject_cast<QAbstractButton*>(control)||qobject_cast<QComboBox*>(control)||qobject_cast<QAbstractSpinBox*>(control)||qobject_cast<QPlainTextEdit*>(control)||(qobject_cast<QLineEdit*>(control)&&!qobject_cast<QComboBox*>(control->parentWidget())&&!qobject_cast<QAbstractSpinBox*>(control->parentWidget()));
   if(editor&&!control->property("settingHelpFor").isValid()){const auto allowed=control->property("providerEnabled");const bool applies=!allowed.isValid()||allowed.toBool();control->setEnabled(!busy&&applies);
    // A value held while setup runs stays readable; its actions are unavailable.
    Ui::setLocked(control,busy&&applies&&!qobject_cast<QPushButton*>(control)&&!qobject_cast<QToolButton*>(control));}
  }
 protonBrowserRetry_->setEnabled(!pendingProtonUrl_.isEmpty()&&!protonBrowserOpening_);
 allowControl(findControl<QPushButton*>("inspectProtonAccount"),!protonAccountUid_.isEmpty());
 if(!busy&&typingPause_&&!saveQueue_.isEmpty()&&!conflict_&&!resetting_)drainSaves();
 profile_->setEnabled(!busy);findControl<QPushButton*>("addS3Destination")->setEnabled(!busy);findControl<QPushButton*>("removeDestination")->setEnabled(!busy);findControl<QPushButton*>("importDevelopmentS3")->setEnabled(!busy);
 updateStoreButtons();
 const bool pending=std::any_of(setupJobs_.cbegin(),setupJobs_.cend(),[](const QJsonValue& value){return value.toObject()["kind"]=="testDestination";});
 findControl<QPushButton*>("testDestination")->setEnabled(!testAccepting_&&!pending);
}
void SettingsWindow::updateStoreButtons(){
 const auto reason=setupBlockedReason();
 for(auto* button:{findControl<QPushButton*>("storeCredentials"),findControl<QPushButton*>("storeProtonPassword")})button->setEnabled(reason.isEmpty());
 for(auto* label:{credentialStoreReason_,passwordStoreReason_}){label->setText(reason);label->setVisible(!reason.isEmpty());}
}
void SettingsWindow::retryProtonBrowser(){
 if(pendingProtonUrl_.isEmpty()||protonBrowserOpening_)return;
 const auto id=pendingProtonSetup_;protonBrowserOpening_=true;protonBrowserRetry_->setEnabled(false);protonAccount_->setText("Finish signing in to Proton in your browser.");
 host::openLink(pendingProtonUrl_,[guard=QPointer<SettingsWindow>(this),id](const QJsonObject& result){if(guard)guard->protonBrowserFinished(id,result);});
}
void SettingsWindow::protonBrowserFinished(const QString& id,const QJsonObject& result){
 if(pendingProtonSetup_.isEmpty()||pendingProtonSetup_!=id)return;
 protonBrowserOpening_=false;const bool failed=!result["opened"].toBool();protonBrowserRetry_->setEnabled(true);protonBrowserRetry_->setVisible(failed);sizeProtonAccountButtons();if(failed)protonAccount_->setText("Couldn't open the default browser for Proton sign-in.");
}
void SettingsWindow::pollSetup(){
 if(setupPollActive_||setupJobs_.isEmpty()){if(setupJobs_.isEmpty())setupTimer_->stop();return;}
 setupPollActive_=true;const auto id=setupJobs_.first().toObject()["setupId"].toString();
 settings_.request("setup.inspect",{{"id",id}},this,[this,id](Error e,QJsonObject data){
  setupPollActive_=false;
  int index=0;while(index<setupJobs_.size()&&setupJobs_[index].toObject()["setupId"]!=id)++index;
  if(index==setupJobs_.size())return;
  const auto job=setupJobs_[index].toObject();const auto event=data["event"].toObject();
  if(!e&&event["event"]=="auth"&&!protonBrowserOpened_){const QUrl url(event["url"].toString());if(url.scheme()=="https"&&url.host()=="account.proton.me"){protonBrowserOpened_=true;pendingProtonUrl_=url;pendingProtonSetup_=id;retryProtonBrowser();updateSetupButton();}}
  if(e.code=="setup_not_found"||(!e&&data["state"]!="running")){
   // The resident recorded what the test found (#1692); its snapshot brings the row's state.
   if(job["kind"]=="testDestination")settings_.refreshSnapshot();
   if(pendingProtonSetup_==id){pendingProtonUrl_.clear();pendingProtonSetup_.clear();protonBrowserOpening_=false;protonBrowserRetry_->hide();sizeProtonAccountButtons();}
   setupJobs_.removeAt(index);updateSetupButton();if(setupJobs_.isEmpty())setupTimer_->stop();
   if(e)e.message="This setup result is no longer available; the action was not repeated.";
   else{const auto error=data["error"].toObject();e={error["code"].toString(),error["message"].toString(),error["details"].toObject()};}
   // A test is for the shown destination while its connection is the one tested, the rule
   // its row follows: a rename or a reorder during the test keeps it current (#1692).
   const bool currentTest=job["profileId"]==profileId_->text()&&job["fingerprint"]==destinationcheck::fingerprint(editedProfile());
   if(e){if(job["kind"]=="testDestination"&&!currentTest)e.message="Connection test failed for the original destination settings. "+e.message;
    // A custom uploader's test shows the start of a reply it could not read, so the Response fields can be fixed.
    if(const auto excerpt=data["result"].toObject()["replyExcerpt"].toString();!excerpt.isEmpty())e.message+=" The reply began: "+excerpt;
    status(e);if(data["kind"]=="connectAccount"&&data["profileId"]==profileId_->text())protonAccount_->setText("Connection failed: "+e.message);}else {
    if(data["kind"]=="storeCredentials"&&data["profileId"]==profileId_->text()&&providerType_->currentData()==CustomUploaderPanel::provider()){clearCurrentCredentialInputs();uploader_->setNotice({});}
    if(data["kind"]=="storeCredentials"||data["kind"]=="unlockCredentials")refreshCredentialHint();
    const auto result=data["result"].toObject();const auto kind=data["kind"].toString();
    if(kind=="connectAccount"||kind=="inspectAccount"){
     const auto revision=job["revision"].toString();
     if(conflict_||(!revision.isEmpty()&&revision!=settings_.revision())||!applyProtonAccountResult(data["profileId"].toString(),result,kind=="connectAccount")){Ui::feedback(plain(status_),"Account details changed while loading. Refresh the original destination to continue.",true);emit viewStateChanged();return;}
     requestedFolders_.insert(data["profileId"].toString()+"/"+result["accountRootUid"].toString());
     if(kind=="connectAccount")setupFeedback("Account connected. Folders loaded.",true);else{Ui::success(plain(status_),"Folders loaded.");Ui::feedback(plain(destinationStatus_),{});}emit viewStateChanged();
     if(data["profileId"]!=profileId_->text()&&destinationPage_->isVisible())activateDestinations();
     return;
    }
    if(QStringList{"createFolder","storeSharePassword"}.contains(kind)&&(data["profileId"]!=profileId_->text()||providerType_->currentData()!="proton-drive"||conflict_)){Ui::feedback(plain(status_),"Setup completed for the original destination. Refresh that destination before applying its result.",true);emit viewStateChanged();return;}

    if(kind=="createFolder"){protonAccountUid_=result["accountRootUid"].toString();addProtonFolder(protonFolder_,result["uid"].toString(),result["name"].toString());protonFolder_->setCurrentIndex(protonFolder_->count()-1);dirty();queueEdit(protonFolder_);}
    else if(kind=="storeSharePassword"){auto profiles=draft_["profiles"].toArray();if(profileIndex_>=0&&profileIndex_<profiles.size()){auto p=profiles[profileIndex_].toObject();auto s=p["settings"].toObject();s["sharePasswordReference"]=result["reference"];p["settings"]=s;profiles[profileIndex_]=p;draft_["profiles"]=profiles;dirty();queueEdit(protonFolder_);}}
    if(kind=="testDestination")setupFeedback(currentTest?(result["testScope"]=="account-folder"?"Connection successful. Account and selected folder are accessible. Upload and sharing were not tested.":result["cleanupStatus"]=="unsupported"?(result["deletionLeftOut"].toString().isEmpty()?"Connection successful. The test upload and its link work. The test file stays on the server: this uploader returned no deletion URL.":result["deletionLeftOut"]=="other_host"?"Connection successful. The test upload and its link work. The test file stays on the server: the deletion URL it returned is on another host than the request URL, so put.gd did not keep it.":"Connection successful. The test upload and its link work. The test file stays on the server: put.gd did not keep the deletion URL it returned."):"Connection successful. Test upload, link and cleanup passed."):"Connection test succeeded for the original destination settings. Test again to check the current settings.",true);
    else{static const QHash<QString,QString> results{{"storeCredentials","Credentials stored."},{"unlockCredentials","Saved credentials unlocked."},{"createFolder","Folder created."},{"storeSharePassword","Link password stored."}};setupFeedback(results.value(kind,"Done.")+(QStringList{"createFolder","storeSharePassword"}.contains(kind)&&dirty_?" Changes save automatically.":""),true);}
   }
   emit viewStateChanged();
  }else{
   if(e)status(e);
   // Retry only the read-only inspection. Keep other accepted jobs moving too.
   if(setupJobs_.size()>1){const QJsonValue pending=setupJobs_[index];setupJobs_.removeAt(index);setupJobs_.append(pending);}
  }
 });
}
void SettingsWindow::showConflictActions(){placeConflictBanners();if(captureOutputBlock_)updateCaptureInheritance();}
QString SettingsWindow::conflictMessage(){return QStringLiteral("Settings changed elsewhere. Your edits are kept here: Retry saving applies them to the file as it is now, and Reload discards them.");}
// The banner says what the status line would; the line keeps its words for agents and other errors.
void SettingsWindow::placeConflictBanners(){
 for(int i=0;i<conflictBanners_.size();++i){auto* label=i?destinationStatus_:status_;if(!label)continue;const bool reported=label->property("errorCode").toString()=="config_conflict"&&!label->text().isEmpty();
  // The line comes back before the banner goes: the row shows while either part does.
  if(conflict_)conflictBanners_[i]->setText(reported?label->text():conflictMessage());label->parentWidget()->setVisible(!conflict_||!reported);reloadConflict_[i]->setVisible(conflict_);conflictBanners_[i]->setVisible(conflict_);}
}
// What a reload discards, in the words of the page that shows it.
QStringList SettingsWindow::draftEdits()const{
 QStringList edits;const auto add=[&edits](const QString& text){if(!edits.contains(text))edits.append(text);};
 const auto draft=candidate();auto base=baseDocument_.isEmpty()?settings_.document():baseDocument_;if(base.isEmpty())base=Settings::defaults();
 // Settings controls by their bound path; their accessible names are the visible labels.
 QHash<QString,QString> names;QList<QWidget*> controls=findChildren<QWidget*>();if(destinationPage_)controls+=destinationPage_->findChildren<QWidget*>();
 for(auto* control:std::as_const(controls)){const auto path=control->property("configPath").toString();if(path.isEmpty()||path.contains("{profile}")||names.contains(path)||(qobject_cast<QLineEdit*>(control)&&qobject_cast<QComboBox*>(control->parentWidget())))continue;auto name=qobject_cast<QAbstractButton*>(control)?qobject_cast<QAbstractButton*>(control)->text():control->accessibleName();name=name.section(':',0,0);if(!name.isEmpty())names.insert(path,name);}
 const auto describe=[&](const QString& path)->QString{
  const auto parts=path.split('/',Qt::SkipEmptyParts);const auto group=parts.value(0);
  if(group=="actions"&&parts.size()>2&&parts[2]=="shortcut")return "Hotkeys → "+actionLabel(parts[1]);
  if(group=="actions")return "Capture → "+actionLabel(parts.value(1))+" output";
  if(group=="captureDefaults")return QString("Capture → Capture output");
  for(auto prefix=path;!prefix.isEmpty();prefix=prefix.left(qMax(0,int(prefix.lastIndexOf('/'))))){
   if(names.contains(prefix)){const auto page=group=="limits"||group=="retention"||path.startsWith("/preferences/diagnostics")?QString("Advanced"):group=="archive"||group=="naming"||path=="/preferences/uploadClipboardBehavior"||path=="/preferences/regionSelectionMode"||path=="/preferences/captureDelaySeconds"||path=="/preferences/captureDelayShortcuts"||path=="/preferences/recordingFrameRate"||path=="/preferences/recordingMicrophoneDevice"||path=="/preferences/recordingClickHighlights"||path=="/preferences/textRecognitionLanguages"||path=="/preferences/quickRedactTerms"||path=="/preferences/quickRedactPatterns"?QString("Capture"):QString("General");return page+" → "+names.value(prefix);}
  }
  return "Settings → "+parts.join(" › ");
 };
 // Leaf differences, outside the destination list.
 std::function<void(const QJsonValue&,const QJsonValue&,const QString&)> compare=[&](const QJsonValue& now,const QJsonValue& before,const QString& path){
  if(now==before)return;
  if(now.isObject()&&before.isObject()){const auto a=now.toObject(),b=before.toObject();auto keys=a.keys();for(const auto& key:b.keys())if(!a.contains(key))keys.append(key);for(const auto& key:std::as_const(keys))compare(a.value(key),b.value(key),path+"/"+key);return;}
  add(describe(path));
 };
 for(const auto& key:draft.keys())if(key!="profiles")compare(draft.value(key),base.value(key),"/"+key);
 for(const auto& key:base.keys())if(key!="profiles"&&!draft.contains(key))compare({},base.value(key),"/"+key);
 // Destinations by name: new, changed, removed, reordered.
 const auto now=draft["profiles"].toArray(),before=base["profiles"].toArray();QStringList nowIds,beforeIds;QHash<QString,QJsonObject> previous;
 for(const auto& value:before){const auto profile=value.toObject();beforeIds.append(profile["id"].toString());previous.insert(profile["id"].toString(),profile);}
 for(const auto& value:now){const auto profile=value.toObject();const auto id=profile["id"].toString();nowIds.append(id);const QString label="Destinations → “"+profile["label"].toString()+"”";if(!previous.contains(id))add(label+" (new)");else if(previous.value(id)!=profile)add(label);}
 for(const auto& id:beforeIds)if(!nowIds.contains(id))add("Destinations → “"+previous.value(id)["label"].toString()+"” (removed)");
 QStringList kept;for(const auto& id:nowIds)if(beforeIds.contains(id))kept.append(id);QStringList was;for(const auto& id:beforeIds)if(nowIds.contains(id))was.append(id);if(kept!=was)add("Destinations → order");
 if(hasCredentialInputs())add("Credentials typed but not stored");
 return edits;
}
void SettingsWindow::reloadFromFile(){
 const auto edits=draftEdits();
 const QString text="Replace this window's settings with the configuration file as it is now?\n\n"+(edits.isEmpty()?QString("There are no unsaved edits to discard."):"These unsaved edits are discarded:\n• "+edits.join("\n• "));
 if(!Ui::confirm(this,"Reload settings from file",text,"Reload from file",!edits.isEmpty()))return;
 resetDraft(true);
}
void SettingsWindow::fitRails(){
 const int width=Ui::railWidth();
 if(settingsRail_)settingsRail_->setFixedWidth(width);if(destinationRail_){int widest=0;const QFontMetrics name(Ui::labelFont(true)),state(Ui::font(Ui::Type::metadata));for(int row=0;row<destinationList_->count();++row){const auto lines=destinationList_->item(row)->text().split('\n');widest=qMax(widest,qMax(name.horizontalAdvance(lines.value(0)),state.horizontalAdvance(lines.value(1))));}const int natural=widest+2*space(Space::l)+28+Ui::scaled(Ui::Metric::icon)+10;destinationRail_->setFixedWidth(qBound(width,natural,width+2*space(Space::xl)));}
}
// How many History items went to the destination shown, for Remove destination's confirmation (#1759).
void SettingsWindow::refreshUploadedCount(){
 if(!destinationList_)return;const auto profiles=candidate()["profiles"].toArray();const int index=destinationList_->currentRow();if(index<0||index>=profiles.size())return;
 const auto id=profiles[index].toObject()["id"].toString();
 // Each destination keeps its own latest request, so a reply isn't dropped for another's.
 // Until it answers, the confirmation uses its unknown-count wording, not an older count.
 const auto ticket=++countSerial_;countRequests_[id]=ticket;uploadedCounts_.remove(id);
 settings_.request("history.counts",{{"profileId",id},{"uploadStatus","uploaded"}},this,[g=QPointer<SettingsWindow>(this),id,ticket](Error e,QJsonObject counts){if(g&&!e&&ticket==g->countRequests_.value(id))g->uploadedCounts_[id]=counts["total"].toInt();});
}
void SettingsWindow::showErrorLog(const QJsonObject& target){
 tabs_->setCurrentWidget(errorLogPage_);
 errorLogPage_->showEntry(target);
}
void SettingsWindow::errorLogCommand(const QJsonObject& params,std::function<void(Error,QJsonObject)> reply){
 if(params["action"]!="inspect")tabs_->setCurrentWidget(errorLogPage_);
 errorLogPage_->command(params,std::move(reply));
}
bool SettingsWindow::selectDestination(const QString& profileId){
 if(!destinationList_||profileId.isEmpty())return false;
 const auto profiles=candidate()["profiles"].toArray();
 for(int row=0;row<profiles.size()&&row<destinationList_->count();++row)if(profiles[row].toObject()["id"]==profileId){destinationList_->setCurrentRow(row);return true;}
 return false;
}
}
