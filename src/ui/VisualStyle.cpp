#include "VisualStyle.h"
#include "IconArt.h"
#include <QApplication>
#include <QAbstractItemView>
#include <QFrame>
#include <QComboBox>
#include <QStyleOptionComboBox>
#include <QHash>
#include <QHeaderView>
#include <QResizeEvent>
#include <QFormLayout>
#include <QLabel>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QScreen>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
namespace putgd::VisualStyle {
namespace {
// Fit the name column when the viewport changes, never while dragging a section.
// Stretch mode would steal the drag distance from the first column instead.
class ColumnSizer final:public QObject {
public:
 explicit ColumnSizer(QHeaderView* header):QObject(header),header_(header){
  header_->setSectionResizeMode(QHeaderView::Interactive);header_->setStretchLastSection(false);
  header_->viewport()->installEventFilter(this);
 }
protected:
 bool eventFilter(QObject* watched,QEvent* event)override {
  if(event->type()==QEvent::Resize){
   const auto* resize=static_cast<QResizeEvent*>(event);
   if(resize->size().width()!=resize->oldSize().width()&&header_->count()>0){
    int remaining=resize->size().width();
    for(int column=1;column<header_->count();++column)if(!header_->isSectionHidden(column))remaining-=header_->sectionSize(column);
    header_->resizeSection(0,qMax(header_->minimumSectionSize(),remaining));
   }
  }
  return QObject::eventFilter(watched,event);
 }
private:QHeaderView* header_;
};
class ChoiceBox final:public QComboBox {
public:using QComboBox::QComboBox;
protected:
 // Leave wheel input to the containing pane. The open popup handles its own scrolling.
 void wheelEvent(QWheelEvent* event)override {event->ignore();}
 void paintEvent(QPaintEvent* event)override {
  QComboBox::paintEvent(event);
  QStyleOptionComboBox option;initStyleOption(&option);
  const auto arrow=style()->subControlRect(QStyle::CC_ComboBox,&option,QStyle::SC_ComboBoxArrow,this);
  QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);painter.setPen(QPen(palette().color(isEnabled()?QPalette::Active:QPalette::Disabled,QPalette::Text),1.5,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
  const QPointF center=arrow.center();QPainterPath path;path.moveTo(center+QPointF(-3.5,-1.5));path.lineTo(center+QPointF(0,2));path.lineTo(center+QPointF(3.5,-1.5));painter.drawPath(path);
 }
};
class StateDelegate final:public QStyledItemDelegate {
public:StateDelegate(QList<int> columns,QObject* parent):QStyledItemDelegate(parent),columns_(std::move(columns)){}
 void paint(QPainter* painter,const QStyleOptionViewItem& option,const QModelIndex& index)const override {
  if(!columns_.contains(index.column())||index.data().toString().isEmpty()){QStyledItemDelegate::paint(painter,option,index);return;}
  auto value=index.data().toString();
  // A local-save cell can contain a path; keep that evidence as ordinary text.
  if(value.contains(' ')||value.contains('/')){QStyledItemDelegate::paint(painter,option,index);return;}
  QStyleOptionViewItem base(option);initStyleOption(&base,index);base.text.clear();base.widget->style()->drawControl(QStyle::CE_ItemViewItem,&base,painter,base.widget);
  QColor foreground("#cbd0dc"),background("#2b303b");
  if(QStringList{"completed","uploaded","copied","saved","removed"}.contains(value)){foreground=QColor("#9bbcff");background=QColor("#243452");}
  else if(QStringList{"failed","denied","interrupted"}.contains(value)){foreground=QColor("#ffb4ab");background=QColor("#4d2b2f");}
  else if(QStringList{"partial","uncertain","pending","running","accepted","superseded","version_retained"}.contains(value)){foreground=QColor("#f4d39a");background=QColor("#403727");}
  value.replace('_',' ');value=value.left(1).toUpper()+value.mid(1);
  painter->save();painter->setRenderHint(QPainter::Antialiasing);auto bounds=option.rect.adjusted(8,5,-8,-5);bounds.setWidth(qMin(bounds.width(),option.fontMetrics.horizontalAdvance(value)+22));painter->setPen(Qt::NoPen);painter->setBrush(background);painter->drawRoundedRect(bounds,5,5);painter->setPen(foreground);painter->drawText(bounds.adjusted(10,0,-8,0),Qt::AlignVCenter|Qt::AlignLeft,option.fontMetrics.elidedText(value,Qt::ElideRight,qMax(0,bounds.width()-18)));painter->restore();
 }
 QSize sizeHint(const QStyleOptionViewItem& option,const QModelIndex& index)const override {auto size=QStyledItemDelegate::sizeHint(option,index);size.setHeight(qMax(36,size.height()));if(columns_.contains(index.column()))size.setWidth(size.width()+26);return size;}
private:QList<int> columns_;
};
}
void configureColumns(QHeaderView* header){new ColumnSizer(header);}
QComboBox* choiceBox(QWidget* parent){return new ChoiceBox(parent);}
void role(QWidget* widget,const char* value){widget->setProperty("visualRole",value);widget->style()->unpolish(widget);widget->style()->polish(widget);}
namespace {
void setFeedback(QLabel* label,const QString& text,const char* tone){label->setProperty("feedbackTone",tone);label->setText(text);label->style()->unpolish(label);label->style()->polish(label);}
}
void feedback(QLabel* label,const QString& text,bool error){setFeedback(label,text,error?"error":"neutral");}
void success(QLabel* label,const QString& text){setFeedback(label,text,"success");}
QToolButton* helpButton(const QString& title,const QString& description,QWidget* control,QWidget* parent){
 control->setToolTip(description);control->setAccessibleDescription(description);
 auto* button=new QToolButton(parent);button->setObjectName(control->objectName()+"Help");button->setProperty("settingHelpFor",control->objectName());button->setProperty("visualRole","info");button->setIcon(icon("info"));button->setIconSize(QSize(18,18));button->setFixedSize(26,26);button->setFocusPolicy(Qt::StrongFocus);button->setAccessibleName("Help for "+title);button->setAccessibleDescription(description);button->setToolTip(description);
 QObject::connect(button,&QToolButton::clicked,button,[button,title,description]{
  auto* dialog=new QMessageBox(QMessageBox::NoIcon,title,title,QMessageBox::Close,button->window());dialog->setObjectName("settingHelpDialog");dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setTextFormat(Qt::PlainText);dialog->setInformativeText(description);dialog->open();
 });return button;
}
QWidget* settingLabel(const QString& title,const QString& description,QWidget* control,QWidget* parent){
 control->setAccessibleName(title);auto* row=new QWidget(parent);auto* layout=new QHBoxLayout(row);layout->setContentsMargins(0,0,0,0);layout->setSpacing(6);
 // A label with a buddy treats & as a mnemonic marker; show titles literally.
 auto* label=new QLabel(QString(title).replace('&',QLatin1String("&&")),row);label->setBuddy(control);layout->addWidget(label);layout->addWidget(helpButton(title,description,control,row));layout->addStretch();return row;
}
QWidget* settingRow(const QString& title,const QString& description,QWidget* control,QWidget* parent){
 control->setAccessibleName(title);auto* row=new QWidget(parent);auto* layout=new QHBoxLayout(row);layout->setContentsMargins(0,0,0,0);layout->setSpacing(6);layout->addWidget(control);layout->addWidget(helpButton(title,description,control,row));layout->addStretch();return row;
}
void apply(QApplication& app){
 if(app.property("putgdVisualStyle").toBool())return;
 app.setProperty("putgdVisualStyle",true);
 QPalette palette=app.palette();palette.setColor(QPalette::Window,QColor("#111318"));palette.setColor(QPalette::WindowText,QColor("#f2f3f7"));palette.setColor(QPalette::Base,QColor("#151820"));palette.setColor(QPalette::AlternateBase,QColor("#222630"));palette.setColor(QPalette::Text,QColor("#f2f3f7"));palette.setColor(QPalette::Button,QColor("#2b303b"));palette.setColor(QPalette::ButtonText,QColor("#f2f3f7"));palette.setColor(QPalette::Highlight,QColor("#9bbcff"));palette.setColor(QPalette::HighlightedText,QColor("#14233d"));palette.setColor(QPalette::PlaceholderText,QColor("#abb1bf"));palette.setColor(QPalette::Disabled,QPalette::Text,QColor("#737b8c"));palette.setColor(QPalette::Disabled,QPalette::ButtonText,QColor("#737b8c"));app.setPalette(palette);
 app.setStyleSheet(QStringLiteral(R"QSS(
 QWidget#putgdWindow { background: #111318; color: #f2f3f7; }
 QWidget { color: #f2f3f7; font-size: 13px; }
 QLabel { background: transparent; }
 QLabel[visualRole="eyebrow"] { color: #9bbcff; font-size: 11px; font-weight: 600; }
 QLabel[visualRole="title"] { font-size: 24px; font-weight: 600; }
 QLabel[visualRole="subtitle"] { color: #abb1bf; font-size: 13px; }
 QLabel[visualRole="sectionTitle"] { font-size: 15px; font-weight: 600; }
 QLabel[visualRole="status"] { color: #cbd0dc; background: #1b1e25; border: 1px solid #383f4d; border-radius: 6px; padding: 8px 10px; }
 QLabel[visualRole="status"][feedbackTone="success"] { color: #d8e5ff; background: #243452; border-color: #7e9acc; font-weight: 600; }
 QLabel[visualRole="status"][feedbackTone="error"] { color: #ffb4ab; background: #38252b; border-color: #805054; }
 QLabel[visualRole="binding"] { color: #abb1bf; font-size: 12px; }
 QLabel[visualRole="binding"][bindingTone="active"] { color: #9bbcff; }
 QLabel[visualRole="binding"][bindingTone="warning"] { color: #f4d39a; }
 QLabel[visualRole="binding"][bindingTone="error"] { color: #ffb4ab; }
 QLabel[visualRole="muted"] { color: #abb1bf; }
 QFrame[visualRole="card"] { background: #1b1e25; border: 1px solid #303644; border-radius: 8px; }
 QFrame[visualRole="card"] > QWidget { background: transparent; }
 QScrollArea, QStackedWidget { background: transparent; border: none; }
 QScrollArea > QWidget > QWidget { background: transparent; }
 QListWidget#settingsNavigation { background: #181b22; border: 1px solid #303644; border-radius: 8px; padding: 6px; outline: none; }
 QListWidget#settingsNavigation::item { padding: 10px 8px; border: 2px solid transparent; border-radius: 7px; margin-bottom: 3px; }
 QListWidget#settingsNavigation::item:selected { background: #293d60; color: #c9dbff; }
 QListWidget#settingsNavigation::item:hover { background: #262d3b; }
 QListWidget#settingsNavigation:focus::item:selected { border: 2px solid #9bbcff; }
 QLineEdit, QComboBox, QPlainTextEdit { background: #151820; border: 1px solid #454e60; border-radius: 6px; padding: 6px 8px; selection-background-color: #9bbcff; selection-color: #14233d; }
 QLineEdit:focus, QComboBox:focus, QPlainTextEdit:focus { border: 1px solid #9bbcff; background: #1e2739; }
 QLineEdit:disabled, QComboBox:disabled, QPlainTextEdit:disabled { color: #737b8c; border-color: #303644; background: #1b1e25; }
 QComboBox { padding-right: 26px; }
 QComboBox QLineEdit { background: transparent; border: none; padding: 0; }
 QComboBox::drop-down { border: none; width: 24px; }
 QComboBox::down-arrow { image: none; }
 QComboBox QAbstractItemView { background: #202530; border: 1px solid #687ea4; selection-background-color: #314970; selection-color: #f2f3f7; }
 QPushButton, QToolButton { background: #2b303b; border: 1px solid #454e60; border-radius: 6px; padding: 6px 11px; font-weight: 500; }
 QPushButton:hover, QToolButton:hover { background: #394458; border-color: #7e9acc; }
 QPushButton:pressed, QToolButton:pressed { background: #23324b; }
 QPushButton:focus, QToolButton:focus { border: 2px solid #9bbcff; padding: 5px 10px; }
 QPushButton:disabled, QToolButton:disabled { color: #737b8c; background: #202530; border-color: #303644; }
 QToolButton[visualRole="info"] { background: transparent; border: 1px solid transparent; padding: 0; }
 QToolButton[visualRole="info"]:hover { background: #293d60; border-color: #687ea4; }
 QToolButton[visualRole="info"]:focus { border: 2px solid #9bbcff; padding: 0; }
 QPushButton[visualRole="primary"] { background: #9bbcff; color: #14233d; border-color: #9bbcff; font-weight: 600; }
 QPushButton[visualRole="primary"]:hover { background: #b5ceff; }
 QPushButton[visualRole="primary"]:focus { border: 2px solid #f2f3f7; }
 QPushButton[visualRole="primary"]:disabled { background: #364357; color: #8996ad; border-color: #454e60; }
 QPushButton[visualRole="danger"] { color: #ffb4ab; background: #38252b; border-color: #805054; }
 QCheckBox { spacing: 8px; padding: 3px 0; }
 QCheckBox::indicator { width: 18px; height: 18px; border: 1px solid #65738c; border-radius: 4px; background: #151820; }
 QCheckBox::indicator:checked { background: #9bbcff; border: 4px solid #415d92; }
 QCheckBox::indicator:focus { border: 2px solid #f2f3f7; }
 QTableView, QTreeView { background: #1b1e25; alternate-background-color: #222630; border: 1px solid #383f4d; border-radius: 8px; gridline-color: #303644; selection-background-color: #314970; selection-color: #f2f3f7; outline: none; }
 QTableView::item, QTreeView::item { padding: 6px; border-bottom: 1px solid #303644; }
 QTableView::item:focus, QTreeView::item:focus { border: 1px solid #9bbcff; }
 QHeaderView { background: #181b22; }
 QHeaderView::section { background: #262d3b; color: #cbd0dc; border: none; border-bottom: 1px solid #454e60; padding: 8px 9px; font-weight: 600; }
 QTableCornerButton::section { background: #262d3b; border: none; }
 QScrollBar:vertical { width: 10px; background: #161920; margin: 2px; }
 QScrollBar:horizontal { height: 10px; background: #161920; margin: 2px; }
 QScrollBar::handle { background: #515e76; border-radius: 4px; min-height: 28px; min-width: 28px; }
 QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
 QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
 QMenu, QMessageBox { background: #1b1e25; }
 QMenu { border: 1px solid #454e60; padding: 6px; }
 QMenu::item { padding: 10px 24px; border-radius: 5px; }
 QMenu::item:selected { background: #314970; }
 QMenu::separator { height: 1px; background: #454e60; margin: 5px 10px; }
 QToolTip { color: #f2f3f7; background: #26344d; border: 1px solid #7e9acc; padding: 7px; }
 QSplitter::handle { background: #26344d; }
 )QSS"));
}
QIcon icon(const QString& name,const QColor& color,int percent){
 static QHash<QString,QIcon> cache;const auto key=name+color.name(QColor::HexArgb);
 const bool progress=name=="tray"&&percent>=0;
 if(!progress&&cache.contains(key))return cache.value(key);
 QIcon result;
 if(name=="brand") {
  for(int scale:{1,2,4,8}) {
   auto image=IconArt::liftApp(color,scale);image.setDevicePixelRatio(1);
   result.addPixmap(QPixmap::fromImage(image));
  }
 } else for(int scale:{1,2,3})result.addPixmap(QPixmap::fromImage(progress?IconArt::liftTray(color,percent,scale):IconArt::symbol(name,color,scale)));
 if(!progress)cache.insert(key,result);
 return result;
}
QWidget* header(const QString& title,const QString& subtitle,QWidget* parent){auto* widget=new QWidget(parent);auto* layout=new QVBoxLayout(widget);layout->setContentsMargins(0,0,0,4);layout->setSpacing(4);auto* heading=new QLabel(title,widget);role(heading,"title");layout->addWidget(heading);if(!subtitle.isEmpty()){auto* description=new QLabel(subtitle,widget);description->setWordWrap(true);role(description,"subtitle");layout->addWidget(description);}return widget;}
QFrame* card(const QString& title,const QString& subtitle,QWidget* parent){auto* frame=new QFrame(parent);role(frame,"card");auto* layout=new QVBoxLayout(frame);layout->setContentsMargins(16,14,16,14);layout->setSpacing(10);auto* heading=new QLabel(title,frame);role(heading,"sectionTitle");layout->addWidget(heading);if(!subtitle.isEmpty()){auto* description=new QLabel(subtitle,frame);description->setWordWrap(true);role(description,"subtitle");layout->addWidget(description);}return frame;}
void styleForm(QFormLayout* form){form->setContentsMargins(0,0,0,0);form->setHorizontalSpacing(16);form->setVerticalSpacing(8);form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);form->setRowWrapPolicy(QFormLayout::WrapLongRows);form->setLabelAlignment(Qt::AlignLeft|Qt::AlignVCenter);}
void decorateView(QAbstractItemView* view,const QList<int>& columns){view->setItemDelegate(new StateDelegate(columns,view));view->setAlternatingRowColors(true);view->setIconSize(QSize(22,22));view->setTextElideMode(Qt::ElideMiddle);}
}
