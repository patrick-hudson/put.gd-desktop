#include "VisualStyle.h"
#include <QApplication>
#include <QAbstractItemView>
#include <QFrame>
#include <QHash>
#include <QFormLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QScreen>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
namespace putgd::VisualStyle {
namespace {
class StateDelegate final:public QStyledItemDelegate {
public:StateDelegate(QList<int> columns,QObject* parent):QStyledItemDelegate(parent),columns_(std::move(columns)){}
 void paint(QPainter* painter,const QStyleOptionViewItem& option,const QModelIndex& index)const override {
  if(!columns_.contains(index.column())||index.data().toString().isEmpty()){QStyledItemDelegate::paint(painter,option,index);return;}
  auto value=index.data().toString();
  // A local-save cell can contain a path; keep that evidence as ordinary text.
  if(value.contains(' ')||value.contains('/')){QStyledItemDelegate::paint(painter,option,index);return;}
  QStyleOptionViewItem base(option);initStyleOption(&base,index);base.text.clear();base.widget->style()->drawControl(QStyle::CE_ItemViewItem,&base,painter,base.widget);
  QColor foreground("#c5cfd0"),background("#2a3438");
  if(QStringList{"completed","uploaded","copied","saved","removed"}.contains(value)){foreground=QColor("#a9f1cb");background=QColor("#1c3d32");}
  else if(QStringList{"failed","denied","interrupted"}.contains(value)){foreground=QColor("#ffc1b9");background=QColor("#4b2c2d");}
  else if(QStringList{"partial","uncertain","pending","running","accepted","superseded","version_retained"}.contains(value)){foreground=QColor("#f8dda8");background=QColor("#423a28");}
  value.replace('_',' ');value=value.left(1).toUpper()+value.mid(1);
  painter->save();painter->setRenderHint(QPainter::Antialiasing);auto bounds=option.rect.adjusted(8,7,-8,-7);bounds.setWidth(qMin(bounds.width(),option.fontMetrics.horizontalAdvance(value)+22));painter->setPen(Qt::NoPen);painter->setBrush(background);painter->drawRoundedRect(bounds,5,5);painter->setPen(foreground);painter->drawText(bounds.adjusted(10,0,-8,0),Qt::AlignVCenter|Qt::AlignLeft,option.fontMetrics.elidedText(value,Qt::ElideRight,qMax(0,bounds.width()-18)));painter->restore();
 }
 QSize sizeHint(const QStyleOptionViewItem& option,const QModelIndex& index)const override {auto size=QStyledItemDelegate::sizeHint(option,index);size.setHeight(qMax(42,size.height()));if(columns_.contains(index.column()))size.setWidth(size.width()+26);return size;}
private:QList<int> columns_;
};
}
void role(QWidget* widget,const char* value){widget->setProperty("visualRole",value);widget->style()->unpolish(widget);widget->style()->polish(widget);}
void feedback(QLabel* label,const QString& text,bool error){label->setProperty("feedbackError",error);label->setText(text);label->style()->unpolish(label);label->style()->polish(label);}
void apply(QApplication& app){
 if(app.property("putgdVisualStyle").toBool())return;
 app.setProperty("putgdVisualStyle",true);
 QPalette palette=app.palette();palette.setColor(QPalette::Window,QColor("#111a1d"));palette.setColor(QPalette::WindowText,QColor("#edf4f2"));palette.setColor(QPalette::Base,QColor("#111b1f"));palette.setColor(QPalette::AlternateBase,QColor("#1d2b2e"));palette.setColor(QPalette::Text,QColor("#edf4f2"));palette.setColor(QPalette::Button,QColor("#27363a"));palette.setColor(QPalette::ButtonText,QColor("#edf4f2"));palette.setColor(QPalette::Highlight,QColor("#a9f1cb"));palette.setColor(QPalette::HighlightedText,QColor("#10291d"));palette.setColor(QPalette::PlaceholderText,QColor("#a8b9b7"));palette.setColor(QPalette::Disabled,QPalette::Text,QColor("#81908f"));palette.setColor(QPalette::Disabled,QPalette::ButtonText,QColor("#81908f"));app.setPalette(palette);
 app.setStyleSheet(QStringLiteral(R"QSS(
 QWidget#putgdWindow { background: #111a1d; color: #edf4f2; }
 QWidget { color: #edf4f2; font-size: 13px; }
 QLabel { background: transparent; }
 QLabel[visualRole="eyebrow"] { color: #a9f1cb; font-size: 11px; font-weight: 600; }
 QLabel[visualRole="title"] { font-size: 27px; font-weight: 600; }
 QLabel[visualRole="subtitle"] { color: #a8b9b7; font-size: 13px; }
 QLabel[visualRole="sectionTitle"] { font-size: 16px; font-weight: 600; }
 QLabel[visualRole="status"] { color: #c8dcd6; background: #19312c; border: 1px solid #2a4c41; border-radius: 8px; padding: 10px; }
 QLabel[visualRole="status"][feedbackError="true"] { color: #ffc1b9; background: #39282b; border-color: #805254; }
 QLabel[visualRole="binding"] { color: #a8b9b7; font-size: 12px; }
 QLabel[visualRole="binding"][bindingTone="active"] { color: #a9f1cb; }
 QLabel[visualRole="binding"][bindingTone="warning"] { color: #f8dda8; }
 QLabel[visualRole="binding"][bindingTone="error"] { color: #ffc1b9; }
 QLabel[visualRole="muted"] { color: #a8b9b7; }
 QFrame[visualRole="card"] { background: #1a2529; border: 1px solid #2c3b40; border-radius: 11px; }
 QFrame[visualRole="card"] > QWidget { background: transparent; }
 QScrollArea, QStackedWidget { background: transparent; border: none; }
 QScrollArea > QWidget > QWidget { background: transparent; }
 QListWidget#settingsNavigation { background: #152125; border: 1px solid #2b3b3e; border-radius: 10px; padding: 8px; outline: none; }
 QListWidget#settingsNavigation::item { padding: 14px 10px; border: 2px solid transparent; border-radius: 7px; margin-bottom: 5px; }
 QListWidget#settingsNavigation::item:selected { background: #284438; color: #b5f8d3; }
 QListWidget#settingsNavigation::item:hover { background: #233338; }
 QListWidget#settingsNavigation:focus::item:selected { border: 2px solid #a9f1cb; }
 QLineEdit, QComboBox, QPlainTextEdit { background: #111b1f; border: 1px solid #415259; border-radius: 6px; padding: 8px 10px; selection-background-color: #a9f1cb; selection-color: #10231b; }
 QLineEdit:focus, QComboBox:focus, QPlainTextEdit:focus { border: 1px solid #a9f1cb; background: #152923; }
 QLineEdit:disabled, QComboBox:disabled, QPlainTextEdit:disabled { color: #81908f; border-color: #2a393d; background: #192326; }
 QComboBox { padding-right: 26px; }
 QComboBox::drop-down { border: none; width: 24px; }
 QComboBox QAbstractItemView { background: #1a292b; border: 1px solid #6b9484; selection-background-color: #2d4b3e; }
 QPushButton, QToolButton { background: #27363a; border: 1px solid #43585b; border-radius: 6px; padding: 8px 13px; font-weight: 500; }
 QPushButton:hover, QToolButton:hover { background: #334b48; border-color: #6a9b88; }
 QPushButton:pressed, QToolButton:pressed { background: #152b22; }
 QPushButton:focus, QToolButton:focus { border: 2px solid #a9f1cb; padding: 7px 12px; }
 QPushButton:disabled, QToolButton:disabled { color: #81908f; background: #202c30; border-color: #2d3c40; }
 QPushButton[visualRole="primary"] { background: #a9f1cb; color: #10291d; border-color: #a9f1cb; font-weight: 600; }
 QPushButton[visualRole="primary"]:hover { background: #c5ffdf; }
 QPushButton[visualRole="primary"]:focus { border: 2px solid #edf4f2; }
 QPushButton[visualRole="primary"]:disabled { background: #34473e; color: #8a9c94; border-color: #43534b; }
 QPushButton[visualRole="danger"] { color: #ffc1b9; background: #38282b; border-color: #775153; }
 QCheckBox { spacing: 10px; padding: 5px 0; }
 QCheckBox::indicator { width: 18px; height: 18px; border: 1px solid #6a8580; border-radius: 4px; background: #121e20; }
 QCheckBox::indicator:checked { background: #a9f1cb; border: 4px solid #365c49; }
 QCheckBox::indicator:focus { border: 2px solid #edf4f2; }
 QTableView, QTreeView { background: #182428; alternate-background-color: #1d2b2e; border: 1px solid #32454b; border-radius: 8px; gridline-color: #2c3c40; selection-background-color: #2d5041; selection-color: #edf4f2; outline: none; }
 QTableView::item, QTreeView::item { padding: 6px; border-bottom: 1px solid #2b3a3f; }
 QTableView::item:focus, QTreeView::item:focus { border: 1px solid #a9f1cb; }
 QHeaderView { background: #172327; }
 QHeaderView::section { background: #223236; color: #b9cdc7; border: none; border-bottom: 1px solid #42554f; padding: 11px 9px; font-weight: 600; }
 QTableCornerButton::section { background: #223236; border: none; }
 QScrollBar:vertical { width: 10px; background: #142023; margin: 2px; }
 QScrollBar:horizontal { height: 10px; background: #142023; margin: 2px; }
 QScrollBar::handle { background: #49665c; border-radius: 4px; min-height: 28px; min-width: 28px; }
 QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
 QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
 QMenu, QMessageBox { background: #19272b; }
 QMenu { border: 1px solid #466159; padding: 6px; }
 QMenu::item { padding: 10px 24px; border-radius: 5px; }
 QMenu::item:selected { background: #31533f; }
 QMenu::separator { height: 1px; background: #3b5149; margin: 5px 10px; }
 QToolTip { color: #edf4f2; background: #243c34; border: 1px solid #709686; padding: 7px; }
 QSplitter::handle { background: #243b34; }
 )QSS"));
}
QIcon icon(const QString& name,const QColor& color){
 static QHash<QString,QIcon> cache;const auto key=name+color.name(QColor::HexArgb);if(cache.contains(key))return cache.value(key);QIcon result;for(int scale:{1,2,3}){QPixmap pixmap(24*scale,24*scale);pixmap.setDevicePixelRatio(scale);pixmap.fill(Qt::transparent);QPainter p(&pixmap);p.setRenderHint(QPainter::Antialiasing);p.setPen(QPen(color,1.7,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));p.setBrush(Qt::NoBrush);
 if(name=="brand"||name=="upload"){p.drawRoundedRect(QRectF(3,3,18,18),5,5);p.drawLine(12,17,12,7);p.drawLine(12,7,8,11);p.drawLine(12,7,16,11);}
 else if(name=="history"){p.drawEllipse(QRectF(4,4,16,16));p.drawLine(12,7,12,12);p.drawLine(12,12,16,14);}
 else if(name=="shortcuts"){p.drawRoundedRect(QRectF(2,5,20,14),3,3);for(int y:{9,12})for(int x:{6,10,14,18})p.drawPoint(x,y);p.drawLine(7,16,17,16);}
 else if(name=="destinations"){p.drawEllipse(QRectF(4,3,16,6));p.drawArc(QRectF(4,9,16,6),180*16,180*16);p.drawArc(QRectF(4,15,16,6),180*16,180*16);p.drawLine(4,6,4,18);p.drawLine(20,6,20,18);}
 else if(name=="capture"||name=="region"){p.drawRoundedRect(QRectF(5,5,14,14),2,2);p.drawLine(2,8,2,2);p.drawLine(2,2,8,2);p.drawLine(16,22,22,22);p.drawLine(22,22,22,16);}
 else if(name=="window"||name=="desktop"){p.drawRoundedRect(QRectF(3,4,18,14),2,2);if(name=="window")p.drawLine(3,8,21,8);else {p.drawLine(12,18,12,21);p.drawLine(8,21,16,21);}}
 else if(name=="clipboard"){p.drawRoundedRect(QRectF(5,5,14,16),2,2);p.drawRoundedRect(QRectF(8,2,8,5),1,1);p.drawLine(8,11,16,11);p.drawLine(8,15,14,15);}
 else if(name=="images"){p.drawRoundedRect(QRectF(3,3,18,18),3,3);p.drawEllipse(QRectF(7,7,3,3));p.drawLine(4,18,11,12);p.drawLine(11,12,16,16);p.drawLine(16,16,20,12);}
 else if(name=="activity"){QPainterPath path;path.moveTo(2,13);path.lineTo(7,13);path.lineTo(10,5);path.lineTo(14,20);path.lineTo(17,11);path.lineTo(22,11);p.drawPath(path);}
 else if(name=="preferences"){for(int y:{6,12,18})p.drawLine(3,y,21,y);p.setBrush(QColor("#19272b"));p.drawEllipse(QPointF(8,6),2.5,2.5);p.drawEllipse(QPointF(16,12),2.5,2.5);p.drawEllipse(QPointF(10,18),2.5,2.5);}
 else {p.drawRoundedRect(QRectF(5,2,14,20),2,2);p.drawLine(8,8,16,8);p.drawLine(8,12,16,12);p.drawLine(8,16,13,16);}
 p.end();result.addPixmap(pixmap);}cache.insert(key,result);return result;
}
QWidget* header(const QString& title,const QString& subtitle,QWidget* parent){auto* widget=new QWidget(parent);auto* layout=new QVBoxLayout(widget);layout->setContentsMargins(0,0,0,10);layout->setSpacing(6);auto* brand=new QLabel("P U T . G D   /   SHARE IN A MOMENT",widget);role(brand,"eyebrow");layout->addWidget(brand);auto* heading=new QLabel(title,widget);role(heading,"title");layout->addWidget(heading);auto* description=new QLabel(subtitle,widget);description->setWordWrap(true);role(description,"subtitle");layout->addWidget(description);return widget;}
QFrame* card(const QString& title,const QString& subtitle,QWidget* parent){auto* frame=new QFrame(parent);role(frame,"card");auto* layout=new QVBoxLayout(frame);layout->setContentsMargins(20,18,20,18);layout->setSpacing(14);auto* heading=new QLabel(title,frame);role(heading,"sectionTitle");layout->addWidget(heading);if(!subtitle.isEmpty()){auto* description=new QLabel(subtitle,frame);description->setWordWrap(true);role(description,"subtitle");layout->addWidget(description);}return frame;}
void styleForm(QFormLayout* form){form->setContentsMargins(0,0,0,0);form->setHorizontalSpacing(20);form->setVerticalSpacing(12);form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);form->setRowWrapPolicy(QFormLayout::WrapLongRows);form->setLabelAlignment(Qt::AlignLeft|Qt::AlignVCenter);}
void decorateView(QAbstractItemView* view,const QList<int>& columns){view->setItemDelegate(new StateDelegate(columns,view));view->setAlternatingRowColors(true);view->setIconSize(QSize(22,22));view->setTextElideMode(Qt::ElideMiddle);}
}
