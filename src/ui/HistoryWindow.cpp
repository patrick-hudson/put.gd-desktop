#include "HistoryWindow.h"
#include "FileDialog.h"
#include "QrCodeWindow.h"
#include "core/QrImage.h"
#include "ViewState.h"
#include "Ui.h"
#include "core/ByteSize.h"
#include <QListWidget>
#include <QAccessibleWidget>
#include <QCursor>
#include <QApplication>
#include <QAction>
#include <QKeySequence>
#include <QKeyEvent>
#include <QClipboard>
#include <QComboBox>
#include "platform/HostOpen.h"
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QFontMetricsF>
#include <QDateEdit>
#include <QCalendarWidget>
#include <QTextOption>
#include <QCheckBox>
#include <QScreen>
#include <QDialog>
#include <QScrollBar>
#include <QDir>
#include <QGridLayout>
#include <QHideEvent>
#include <QHelpEvent>
#include <QToolTip>
#include <QImageReader>
#include "PreviewReader.h"
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QLocale>
#include <QMenu>
#include <QMap>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPersistentModelIndex>
#include <QUuid>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScopedValueRollback>
#include <QSaveFile>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QStyleOptionButton>
#include <QStyleOptionToolButton>
#include <QTextDocument>
#include <memory>
#include <QTextCursor>
#include <QTextBlock>
#include <QTextLayout>
#include <QAbstractTextDocumentLayout>
#include <cmath>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <algorithm>
#include <utility>
namespace putgd {
namespace {
using Ui::Space;
using Ui::space;
QString originalDestination(const QJsonObject& receipt,const QJsonArray& profiles);
Qt::TextElideMode titleElision(const QString& name){return QFileInfo(name).suffix().isEmpty()?Qt::ElideRight:Qt::ElideMiddle;}
// The recorded link's confirmed expiry (#253): upcoming while the link works,
// or passed once the service or the clock says it has.
QString linkExpiry(const QJsonObject& detail){
 const auto expires=QDateTime::fromString(detail["linkExpiresAt"].toString(detail["shareExpiresAt"].toString()),Qt::ISODateWithMs);
 if(!expires.isValid())return {};
 const auto when=QLocale().toString(expires.toLocalTime(),QLocale::LongFormat);
 if(expires<=QDateTime::currentDateTimeUtc())return "Link expired on "+when;
 return detail["linkAvailable"].toBool()?"Link expires on "+when:QString{};
}
// Keep status labels together; diagnostic prose retains its natural wrapping.
QString statusPhrases(QString text,bool failed=false){
 auto tokens=text.split(" · ");for(int i=0;i<tokens.size();++i)if(!failed||i!=1||tokens[i]=="Saved locally")tokens[i].replace(' ',QChar(0x00a0));return tokens.join(" · ");
}
QFont outcomeFont(){auto font=Ui::font(Ui::scaled(100)>=125?Ui::Type::metadata:Ui::Type::technical);if(Ui::scaled(100)>=125)font.setWeight(QFont::Medium);return font;}
QString metadataSpan(const QString& text,const QColor& color,bool fact=false){const auto font=fact?outcomeFont():Ui::font(Ui::Type::technical);return "<span style=\"color:"+color.name()+";font-size:"+QString::number(font.pixelSize())+"px;font-weight:"+QString::number(font.weight())+"\">"+statusPhrases(text).toHtmlEscaped()+"</span>";}
bool outcomePart(const QJsonObject& record,const QString& part){for(const auto& outcome:HistoryModel::statusText(record).split(" · "))if(outcome==part||outcome.startsWith(part+".")||outcome.startsWith(part+" ("))return true;return false;}
QColor metadataColor(const QJsonObject& record,const QString& part,const QColor& neutral){
 const auto& t=Theme::current();if(HistoryModel::failed(record))return part==HistoryModel::failureLabel(record)?t.danger:neutral;
 const auto remote=HistoryModel::remoteStateText(record),copy=record["operation"].toObject()["copyStatus"].toString();const bool status=part==remote||part=="Shared link"||part=="Copied to clipboard"||part=="Couldn't copy result"||part=="Couldn't copy link"||part=="Result not copied"||part=="Link not copied";
 return status&&(part=="Deletion failed"||part=="Sharing failed"||part.startsWith("Couldn't copy"))?t.danger:status&&remote.isEmpty()&&copy=="copied"?t.success:neutral;
}
QString failureDetail(const QJsonObject& record){auto text=HistoryModel::statusText(record);const auto label=HistoryModel::failureLabel(record);if(text==label)return {};if(text.startsWith(label+" · "))text.remove(0,label.size()+3);return text;}
QString failureDescription(const QModelIndex& index){const auto record=index.data(HistoryModel::RecordRole).toJsonObject();QStringList parts{record["name"].toString(index.data().toString()),HistoryModel::failureLabel(record),index.data(HistoryModel::CreatedRole).toString(),failureDetail(record)};parts.removeAll({});return parts.join(". ");}
// Beside the actions the detail keeps one line; on a line of its own its facts wrap
// whole, and only a fact wider than the row elides.
void compactFailureDetail(QTextDocument& document,const QModelIndex& index,int width,int skip=0,bool wrap=false){
 const auto font=Ui::font(Ui::Type::technical);QStringList spans;for(const auto& part:failureDetail(index.data(HistoryModel::RecordRole).toJsonObject()).split(" · ",Qt::SkipEmptyParts).mid(skip))spans.append(metadataSpan(part,Theme::current().textSecondary,true));
 document.setDocumentMargin(0);document.setDefaultFont(font);document.setHtml(Ui::separatedLines(spans,font,width));auto lines=document.toPlainText().split('\n');auto shown=lines.value(0);if(lines.size()>1&&!wrap)shown+=QChar(0x2026);shown=QFontMetrics(font).elidedText(shown,Qt::ElideRight,qMax(0,width));
 QStringList shownLines{metadataSpan(shown,Theme::current().textSecondary,true)};if(wrap)for(int line=1;line<lines.size();++line)shownLines.append(metadataSpan(QFontMetrics(font).elidedText(lines[line],Qt::ElideRight,qMax(0,width)),Theme::current().textSecondary,true));
 QTextOption option;option.setWrapMode(QTextOption::NoWrap);document.setDefaultTextOption(option);document.setHtml(shownLines.join("<br/>"));document.setTextWidth(qMax(20,width));
}
class HistoryHeader final:public QWidget {
public:using QWidget::QWidget;
protected:void paintEvent(QPaintEvent*)override{QPainter painter(this);const auto& t=Theme::current();painter.fillRect(rect(),property("inspector").toBool()?t.surface:t.bg);}
};
class HistoryOutcome final:public QLabel {
public:
 explicit HistoryOutcome(QWidget* parent):QLabel(parent){setTextFormat(Qt::RichText);setWordWrap(true);QSizePolicy policy(QSizePolicy::Ignored,QSizePolicy::Preferred);policy.setHeightForWidth(true);setSizePolicy(policy);}
 void setSegments(const QStringList& segments,const QString& plain){segments_=segments;setToolTip(plain);setAccessibleName(plain);setAccessibleDescription(plain);refresh();}
 bool hasHeightForWidth()const override{return true;}
 int heightForWidth(int width)const override{QTextDocument document;document.setDocumentMargin(0);document.setDefaultFont(font());document.setHtml(Ui::separatedLines(segments_,font(),width));document.setTextWidth(qMax(1,width));return int(std::ceil(document.size().height()));}
 QSize sizeHint()const override{return {0,heightForWidth(width())};}
 QSize minimumSizeHint()const override{return {0,fontMetrics().height()};}
protected:void resizeEvent(QResizeEvent* event)override{QLabel::resizeEvent(event);refresh();}
private:
 void refresh(){const auto html=Ui::separatedLines(segments_,font(),contentsRect().width());if(text()!=html){setText(html);updateGeometry();}}
 QStringList segments_;
};
void setHistoryOutcome(QLabel* label,const QJsonObject& group){
 const auto remote=HistoryModel::remoteStateText(group);const bool failed=HistoryModel::failed(group);const auto text=HistoryModel::statusText(group);const auto& t=Theme::current();
 Ui::role(label,failed||remote=="Deletion failed"||remote=="Sharing failed"?"danger":!remote.isEmpty()?"muted":group["uploadStatus"]=="uploaded"?"success":"muted");
 QStringList segments;const auto parts=text.split(" · ");for(const auto& part:parts)segments.append(metadataSpan(part,failed?(part==HistoryModel::failureLabel(group)?t.danger:t.textSecondary):remote=="Deletion failed"||remote=="Sharing failed"?t.danger:remote.isEmpty()&&group["uploadStatus"]=="uploaded"?t.success:t.textSecondary,true));
 static_cast<HistoryOutcome*>(label)->setSegments(segments,text);
}
// A video's poster carries a play glyph and its length, so a still is never
// mistaken for a screenshot (#112).
bool video(const QJsonObject& record){return record["category"]=="videos"||record["mime"].toString().startsWith("video/");}
QString missingLocalCopyHint(const QJsonObject& data){
 const auto remote=data["remoteStatus"].toString();const auto kind=video(data)?QString("recording"):data["mime"].toString().startsWith("image/")?QString("image"):QString("file");
 QString text="No local copy is available. ";
 if(remote=="removed"||remote=="already_missing")text+="The uploaded "+kind+(remote=="removed"?" was deleted from its destination.":" is missing from its destination.")+" Check for another copy or a destination backup.";
 else if(remote=="hidden_version_retained"||remote=="version_retained")text+="The original uploaded version is retained at its destination. You may be able to download it using the destination’s version history.";
 else if(remote=="changed"||remote=="superseded"||data["superseded"].toBool())text+="The file at the destination has changed. Check its version history or backups for the original "+kind+".";
 else if(remote=="pending"||remote=="deleting")text+="Deletion is "+QString(remote=="pending"?"pending":"in progress")+" at its destination. Check there or in backups for a copy.";
 else if(remote=="unknown")text+="Remote availability is unknown. Check its destination or backups for a copy.";
 else if(data["uploadStatus"]=="uploaded")text+="You may still be able to download the uploaded "+kind+" from its destination.";
 else if(data["uploadStatus"]=="unknown")text+="The upload outcome is unknown. Check its destination for a copy.";
 else text+="No successful upload is recorded. Check the original source or another saved copy.";
 const auto remoteOutcome=HistoryModel::remoteStateText(data),shareOutcome=HistoryModel::remoteStateText(QJsonObject{{"shareStatus",data["shareStatus"]}});auto outcome=HistoryModel::statusText(data);
 if(HistoryModel::failed(data)||(!remoteOutcome.isEmpty()&&remoteOutcome==shareOutcome)||(remoteOutcome.isEmpty()&&data["uploadStatus"]=="uploaded"&&outcome!="Uploaded")){
  outcome.remove(" · Saved previously · Local file unavailable");outcome.remove(" · Saved locally");text=outcome+". "+text;
 }
 return text;
}
void paintVideoBadge(QPainter& p,const QRect& picture,const QString& length){
 p.save();p.setRenderHint(QPainter::Antialiasing);p.setPen(Qt::NoPen);p.setBrush(Theme::mediaScrim());
 const int size=qBound(18,qMin(picture.width(),picture.height())*2/5,48);const QRect circle(picture.center()-QPoint(size/2,size/2),QSize(size,size));
 p.drawEllipse(circle);Ui::icon("play",Theme::onMedia()).paint(&p,circle.adjusted(size/4+size/16,size/4,-size/4+size/16,-size/4));
 if(!length.isEmpty()&&picture.height()>=40){
  auto font=p.font();font.setPixelSize(qBound(10,size/3,13));font.setBold(true);p.setFont(font);const QFontMetrics metrics(font);
  const QSize pill(metrics.horizontalAdvance(length)+10,metrics.height()+2);const QRect box(picture.bottomRight()-QPoint(pill.width()+3,pill.height()+3),pill);
  p.setBrush(Theme::mediaScrim());p.drawRoundedRect(box,4,4);p.setPen(Theme::onMedia());p.drawText(box,Qt::AlignCenter,length);
 }
 p.restore();
}
// Keep source pixels so resizing an inspector or its dialog refits the image.
class HistoryPreview final:public QLabel {
public:
 explicit HistoryPreview(QWidget* parent):QLabel(parent){}
 QImage image;bool recording=false,thumbnail=false;std::function<void()> activate;QPushButton* expandButton=nullptr;
 void setThumbnail(bool compact){if(thumbnail==compact)return;thumbnail=compact;setAlignment(thumbnail?Qt::AlignLeft|Qt::AlignVCenter:Qt::AlignCenter);refit();updateExpand();}
 void updateExpand(){if(!expandButton)return;const int extent=qMin(width(),height()),side=qMin(Ui::scaled(28),qMax(1,extent-2*space(Space::xs))),inset=qMin(space(Space::s),qMax(0,(extent-side)/2)),glyph=qMax(1,qMin(Ui::scaled(Ui::Metric::icon),side-space(Space::xs)));expandButton->setText(thumbnail?QString("View image"):QString());expandButton->setIconSize(QSize(glyph,glyph));if(thumbnail){const int left=margin()+qMax(1,height()-2*margin())+space(Space::s);expandButton->setGeometry(left,(height()-side)/2,qMin(expandButton->sizeHint().width(),qMax(1,width()-left-inset)),side);}else expandButton->setGeometry(width()-inset-side,inset,side,side);expandButton->setVisible(expandable());}
 bool expandable()const{return activate&&!image.isNull();}
 void setImage(const QImage& source,bool isVideo){image=source;recording=isVideo;setFocusPolicy(expandable()?Qt::StrongFocus:Qt::NoFocus);if(expandable()){QPixmap cursor(28,28);cursor.fill(Qt::transparent);QPainter p(&cursor);p.setRenderHint(QPainter::Antialiasing);QPainterPath zoom;zoom.addEllipse(QRectF(3,3,15,15));zoom.moveTo(16,16);zoom.lineTo(24,24);zoom.moveTo(7,10.5);zoom.lineTo(14,10.5);zoom.moveTo(10.5,7);zoom.lineTo(10.5,14);p.setPen(QPen(Qt::white,4,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));p.drawPath(zoom);p.setPen(QPen(Qt::black,1.8,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));p.drawPath(zoom);p.end();setCursor(QCursor(cursor,10,10));}else unsetCursor();refit();updateExpand();}
 void clearImage(){image={};pressed_=false;setFocusPolicy(Qt::NoFocus);unsetCursor();clear();updateExpand();}
protected:
 bool event(QEvent* event)override{const bool result=QLabel::event(event);if(event->type()==QEvent::Enter||event->type()==QEvent::Leave||event->type()==QEvent::FocusIn||event->type()==QEvent::FocusOut)updateExpand();return result;}
 void resizeEvent(QResizeEvent* event)override{QLabel::resizeEvent(event);refit();updateExpand();}
 void mousePressEvent(QMouseEvent* event)override{pressed_=expandable()&&event->button()==Qt::LeftButton;if(pressed_){pressPosition_=event->position().toPoint();setFocus(Qt::MouseFocusReason);event->accept();}else QLabel::mousePressEvent(event);}
 void mouseReleaseEvent(QMouseEvent* event)override{const bool pressed=std::exchange(pressed_,false);if(pressed&&expandable()&&event->button()==Qt::LeftButton&&rect().contains(event->position().toPoint())&&(event->position().toPoint()-pressPosition_).manhattanLength()<=QApplication::startDragDistance()){activate();event->accept();}else QLabel::mouseReleaseEvent(event);}
 void keyPressEvent(QKeyEvent* event)override{if(expandable()&&(event->key()==Qt::Key_Space||event->key()==Qt::Key_Return||event->key()==Qt::Key_Enter)){if(!event->isAutoRepeat())activate();event->accept();}else QLabel::keyPressEvent(event);}
 void paintEvent(QPaintEvent* event)override{QLabel::paintEvent(event);if(hasFocus()&&expandable()){QPainter painter(this);painter.setPen(Theme::current().accent);painter.setBrush(Qt::NoBrush);painter.drawRoundedRect(rect().adjusted(1,1,-2,-2),7,7);}}
private:
 bool pressed_=false;QPoint pressPosition_;
 void refit(){if(image.isNull())return;const auto available=contentsRect().size()-QSize(2*margin(),2*margin());auto picture=QPixmap::fromImage(image).scaled(thumbnail?QSize(available.height(),available.height()):available,Qt::KeepAspectRatio,Qt::SmoothTransformation);if(recording){QPainter painter(&picture);paintVideoBadge(painter,QRect(QPoint(),picture.size()),HistoryModel::duration(image.text(HistoryModel::durationKey).toLongLong()));}setPixmap(picture);}
};
class HistoryLink final:public QLineEdit {
public:explicit HistoryLink(QWidget* parent):QLineEdit(parent){}
protected:
 void paintEvent(QPaintEvent* event)override{
  if(hasFocus()||text().isEmpty()){QLineEdit::paintEvent(event);return;}
  QPainter painter(this);QStyleOptionFrame option;initStyleOption(&option);style()->drawPrimitive(QStyle::PE_PanelLineEdit,&option,&painter,this);
  const auto area=style()->subElementRect(QStyle::SE_LineEditContents,&option,this).adjusted(2,0,-2-textMargins().right(),0);painter.setClipRect(area);painter.setFont(font());painter.setPen(palette().color(isEnabled()?QPalette::Active:QPalette::Disabled,QPalette::Text));painter.drawText(area,Qt::AlignLeft|Qt::AlignVCenter,fontMetrics().elidedText(text(),Qt::ElideMiddle,area.width()));
 }
};
// The link and its verb share one raised outline; text entry and copying stay native.
class HistoryLinkBar final:public QWidget {
public:explicit HistoryLinkBar(QWidget* parent):QWidget(parent){setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);setFixedHeight(Ui::scaled(Ui::Metric::control));}
protected:void paintEvent(QPaintEvent*)override{QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);painter.setPen(Ui::controlStroke());painter.setBrush(Theme::current().surfaceRaised);painter.drawRoundedRect(QRectF(rect()).adjusted(0.5,0.5,-0.5,-0.5),Ui::Metric::radius,Ui::Metric::radius);}
};
class HistoryLinkCopy final:public QPushButton {
public:explicit HistoryLinkCopy(QWidget* parent):QPushButton("Copy link",parent){auto refresh=[this]{setProperty("uiPrimaryFill",Theme::scheme()==Theme::Scheme::Dark?QColor("#D6D5CF"):QColor());update();};refresh();connect(&Theme::Notifier::instance(),&Theme::Notifier::changed,this,refresh);}
protected:void paintEvent(QPaintEvent*)override{QPainter painter(this);QStyleOptionButton option;initStyleOption(&option);const auto label=option.rect;option.rect.adjust(-Ui::Metric::radius,0,0,0);style()->drawControl(QStyle::CE_PushButtonBevel,&option,&painter,this);option.rect=label;style()->drawControl(QStyle::CE_PushButtonLabel,&option,&painter,this);painter.setPen(Ui::controlStroke());painter.drawLine(0,0,0,height()-1);}
};
// Keep one action row, dropping labels as a group when their measured widths do not fit.
class HistoryActionRow final:public QWidget {
public:explicit HistoryActionRow(QWidget* parent):QWidget(parent){setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);}
protected:
 bool event(QEvent* event)override{const bool result=QWidget::event(event);if(event->type()==QEvent::Resize||event->type()==QEvent::LayoutRequest)if(auto* box=qobject_cast<QBoxLayout*>(layout())){int required=0,count=0;QList<QPushButton*> buttons;for(int i=0;i<box->count();++i)if(auto* widget=box->itemAt(i)->widget();widget&&!widget->isHidden()){if(auto* button=qobject_cast<QPushButton*>(widget)){if(!button->property("historyLabel").isValid())button->setProperty("historyLabel",button->text());QStyleOptionButton option;option.initFrom(button);option.text=button->property("historyLabel").toString();option.icon=button->icon();option.iconSize=button->iconSize();required+=button->style()->sizeFromContents(QStyle::CT_PushButton,&option,QSize(),button).width();buttons.append(button);}else required+=widget->sizeHint().width();++count;}required+=qMax(0,count-1)*box->spacing();const bool labels=width()>=required;for(auto* button:buttons){const auto text=labels?button->property("historyLabel").toString():QString{};if(button->text()!=text)button->setText(text);}setProperty("labelsVisible",labels);}return result;}
};
// A minimum-date sentinel leaves date entry and calendar navigation native.
class HistoryDate final:public QDateEdit {
public:explicit HistoryDate(QWidget* parent):QDateEdit(parent){setMinimumDate(QDate(1752,9,14));setSpecialValueText("Any date");setCalendarPopup(true);setDate(minimumDate());calendarWidget()->installEventFilter(this);}
protected:
 void stepBy(int steps)override{if(date()==minimumDate())setDate(QDate::currentDate());else QDateEdit::stepBy(steps);}
 bool eventFilter(QObject* object,QEvent* event)override{if(object==calendarWidget()&&event->type()==QEvent::Show&&date()==minimumDate()){const auto today=QDate::currentDate();calendarWidget()->setCurrentPage(today.year(),today.month());}return QDateEdit::eventFilter(object,event);}
};
// Real label/value widgets keep selection native and share one measured gutter.
class HistorySummary final:public QWidget {
public:
 explicit HistorySummary(QWidget* parent):QWidget(parent){grid_=new QGridLayout(this);grid_->setContentsMargins(0,0,0,0);grid_->setHorizontalSpacing(space(Space::m));grid_->setVerticalSpacing(space(Space::s));setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);}
 void setFields(const QList<std::pair<QString,QString>>& fields){
  while(auto* item=grid_->takeAt(0)){delete item->widget();delete item;}labels_.clear();values_.clear();QStringList text;
  for(const auto& [key,value]:fields){auto* label=new QLabel(key,this);Ui::role(label,"meta");QLabel* content;if(key=="File"){auto* file=new Ui::ElidedLabel(this);file->setElideMode(titleElision(value));file->setText(value);file->setObjectName("historyFileValue");content=file;}else content=new QLabel(value,this);Ui::role(content,"body");content->setAlignment(Qt::AlignLeft|Qt::AlignTop);content->setTextFormat(Qt::PlainText);content->setWordWrap(key!="File");content->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);content->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);content->setMinimumWidth(0);content->setAccessibleName(key+": "+value);content->setToolTip(value);labels_.append(label);values_.append(content);text.append(key+": "+value);}
  setProperty("text",text.join("\n"));setAccessibleDescription(text.join("\n"));arrange();for(auto* label:labels_)label->show();for(auto* value:values_)value->show();updateGeometry();
 }
protected:void resizeEvent(QResizeEvent* event)override{QWidget::resizeEvent(event);arrange();}
private:
 void arrange(){
  const int gutter=Ui::scaled(96);const bool stacked=width()<gutter+space(Space::m)+Ui::scaled(140);
  while(auto* item=grid_->takeAt(0)){delete item;}grid_->setColumnMinimumWidth(0,stacked?0:gutter);grid_->setColumnStretch(0,stacked?1:0);grid_->setColumnStretch(1,stacked?0:1);
  for(int i=0;i<labels_.size();++i){labels_[i]->setWordWrap(true);labels_[i]->setFixedWidth(stacked?width():gutter);labels_[i]->setAlignment(Qt::AlignLeft|Qt::AlignTop);labels_[i]->setContentsMargins(0,stacked?0:qMax(0,values_[i]->fontMetrics().ascent()-labels_[i]->fontMetrics().ascent()),0,0);grid_->addWidget(labels_[i],stacked?2*i:i,0,Qt::AlignLeft|Qt::AlignTop);grid_->addWidget(values_[i],stacked?2*i+1:i,stacked?0:1,Qt::AlignTop);}
  setProperty("stacked",stacked);
 }
 QGridLayout* grid_;QList<QLabel*> labels_,values_;
};
// One filter's editor under its chip or Filter: the same control the panel holds,
// lent for as long as the editor is open (closed returns it). It floats inside
// the window rather than as a popup, so keyboard focus stays with the window;
// Esc, a click elsewhere or focus moving away closes it.
class HistoryFilterEditor final:public QFrame {
public:
 explicit HistoryFilterEditor(QWidget* parent):QFrame(parent){setAttribute(Qt::WA_DeleteOnClose);setAutoFillBackground(false);connect(qApp,&QApplication::focusChanged,this,[this](QWidget*,QWidget* now){if(isVisible()&&now&&!isAncestorOf(now)&&now->window()==window())QTimer::singleShot(0,this,[this]{if(isVisible()&&!isAncestorOf(QApplication::focusWidget())&&!QApplication::activePopupWidget())close();});});window()->installEventFilter(this);}
 std::function<void()> closed;
protected:
 void paintEvent(QPaintEvent*)override{QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);const auto& t=Theme::current();painter.setPen(t.border);painter.setBrush(t.surfaceRaised);painter.drawRoundedRect(QRectF(rect()).adjusted(0.5,0.5,-0.5,-0.5),Ui::Metric::radius,Ui::Metric::radius);}
 void keyPressEvent(QKeyEvent* event)override{if(event->key()==Qt::Key_Escape){close();event->accept();return;}QFrame::keyPressEvent(event);}
 bool eventFilter(QObject* object,QEvent* event)override{if(object==window()&&event->type()==QEvent::Resize)close();if(event->type()==QEvent::MouseButtonPress&&isVisible()){if(auto* widget=qobject_cast<QWidget*>(object);widget&&widget->window()==window()&&!isAncestorOf(widget)&&widget!=this)QTimer::singleShot(0,this,[this]{close();});}return QFrame::eventFilter(object,event);}
 void showEvent(QShowEvent* event)override{QFrame::showEvent(event);qApp->installEventFilter(this);}
 void hideEvent(QHideEvent* event)override{QFrame::hideEvent(event);qApp->removeEventFilter(this);if(auto done=std::exchange(closed,{}))done();}
};
// A filter chip: "Destination: Proton Drive" opens the filter's editor; × removes it.
class HistoryChipLabel final:public QToolButton {
public:
 HistoryChipLabel(const QString& key,const QString& value,QWidget* parent):QToolButton(parent),key_(key),value_(value){setText(key+": "+value);setProperty("visualRole","quiet");setProperty("uiFlush",true);setFocusPolicy(Qt::StrongFocus);setCursor(Qt::PointingHandCursor);}
 QSize sizeHint()const override{const QFontMetrics metrics(font());return {space(Space::m)+metrics.horizontalAdvance(key_+": ")+metrics.horizontalAdvance(value_)+space(Space::xs),Ui::scaled(Ui::Metric::control)};}
 QSize minimumSizeHint()const override{return sizeHint();}
protected:
 void paintEvent(QPaintEvent*)override{
  QPainter painter(this);QStyleOptionToolButton option;initStyleOption(&option);option.text.clear();option.icon={};style()->drawComplexControl(QStyle::CC_ToolButton,&option,&painter,this);
  const auto& t=Theme::current();const QFontMetrics metrics(font());const auto key=key_+": ";const int left=space(Space::m),keyWidth=metrics.horizontalAdvance(key);
  Ui::paintText(painter,QRectF(left,0,keyWidth+1,height()),key,t.textSecondary,font(),Qt::AlignLeft|Qt::AlignVCenter,Qt::ElideNone);
  Ui::paintText(painter,QRectF(left+keyWidth,0,qMax(0,width()-left-keyWidth),height()),value_,t.text,font());
 }
private:QString key_,value_;
};
class HistoryFilterChip final:public QWidget {
public:
 HistoryFilterChip(const QString& key,const QString& value,QWidget* parent):QWidget(parent){
  setFixedHeight(Ui::scaled(Ui::Metric::control));auto* row=new QHBoxLayout(this);row->setContentsMargins(1,1,space(Space::xs),1);row->setSpacing(0);
  edit=new HistoryChipLabel(key,value,this);row->addWidget(edit);remove=Ui::toolButton({},{},"quiet",this);remove->setIcon(Ui::chromeIcon("close"));remove->setIconSize(QSize(Ui::scaled(12),Ui::scaled(12)));remove->setFixedSize(Ui::scaled(24),Ui::scaled(24));row->addWidget(remove,0,Qt::AlignVCenter);
 }
 QToolButton *edit=nullptr,*remove=nullptr;
protected:void paintEvent(QPaintEvent*)override{QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);painter.setPen(Ui::controlStroke());painter.setBrush(Theme::current().surfaceRaised);painter.drawRoundedRect(QRectF(rect()).adjusted(0.5,0.5,-0.5,-0.5),Ui::Metric::radius,Ui::Metric::radius);}
};
// Chips in at most two lines: what does not fit folds into "+N more", which opens
// the panel of every filter; Clear filters ends the row.
class HistoryChipRow final:public QWidget {
public:
 explicit HistoryChipRow(QWidget* parent):QWidget(parent){QSizePolicy policy(QSizePolicy::Preferred,QSizePolicy::Fixed);policy.setHeightForWidth(true);setSizePolicy(policy);}
 void setItems(const QList<QWidget*>& chips,QToolButton* more,QWidget* trailing){chips_=chips;more_=more;trailing_=trailing;arrange(width(),true);updateGeometry();}
 bool hasHeightForWidth()const override{return true;}
 int heightForWidth(int width)const override{return const_cast<HistoryChipRow*>(this)->arrange(width,false);}
 QSize sizeHint()const override{return {Ui::scaled(400),heightForWidth(width()>0?width():Ui::scaled(800))};}
 QSize minimumSizeHint()const override{return {0,heightForWidth(width()>0?width():Ui::scaled(800))};}
 int folded()const{return folded_;}
protected:void resizeEvent(QResizeEvent* event)override{QWidget::resizeEvent(event);arrange(width(),true);}
private:
 int arrange(int width,bool apply){
  const auto margins=contentsMargins();const int available=qMax(1,width-margins.left()-margins.right()),gap=space(Space::s),line=Ui::scaled(Ui::Metric::control);
  const auto place=[&](const QList<QWidget*>& items,QList<QRect>* rects){int x=0,lines=1;for(auto* item:items){const int w=qMin(available,item->sizeHint().width());if(x>0&&x+w>available){++lines;x=0;}if(rects)rects->append(QRect(margins.left()+x,margins.top()+(lines-1)*(line+gap),w,line));x+=w+gap;}return lines;};
  const auto label=more_?more_->text():QString{};int shown=chips_.size();QList<QWidget*> items;
  for(;shown>=0;--shown){items=chips_.mid(0,shown);if(shown<chips_.size()&&more_){more_->setText(QString("+%1 more").arg(chips_.size()-shown));items.append(more_);}if(trailing_)items.append(trailing_);if(place(items,nullptr)<=2||shown==0)break;}
  QList<QRect> rects;const int lines=items.isEmpty()?0:place(items,&rects);
  if(!apply&&more_)more_->setText(label);
  if(apply){folded_=chips_.size()-shown;for(int i=0;i<chips_.size();++i)chips_[i]->setVisible(i<shown);if(more_)more_->setVisible(shown<chips_.size());for(int i=0;i<items.size();++i){items[i]->setGeometry(rects[i]);items[i]->show();}}
  return lines?margins.top()+lines*line+(lines-1)*gap+margins.bottom():0;
 }
 QList<QWidget*> chips_;QToolButton* more_=nullptr;QWidget* trailing_=nullptr;int folded_=0;
};
class HistoryCategoryDelegate final:public QStyledItemDelegate {
public:explicit HistoryCategoryDelegate(QObject* parent):QStyledItemDelegate(parent),navigation_(nullptr){}
 QSize sizeHint(const QStyleOptionViewItem& option,const QModelIndex& index)const override{if(index.flags()==Qt::NoItemFlags)return {0,9};auto measured=option;measured.rect.setWidth(static_cast<QListWidget*>(parent())->viewport()->width());auto size=navigation_.sizeHint(measured,index);if(parent()->property("denseHistory").toBool()&&size.height()<=Ui::scaled(Ui::Metric::navRow))size.setHeight(qMax(36,QFontMetrics(Ui::font()).height()+16));return size;}
 void paint(QPainter* painter,const QStyleOptionViewItem& option,const QModelIndex& index)const override{if(index.flags()==Qt::NoItemFlags){painter->save();painter->setPen(Theme::current().border);painter->drawLine(option.rect.left()+2,option.rect.center().y(),option.rect.right()-2,option.rect.center().y());painter->restore();}else navigation_.paint(painter,option,index);}
private:Ui::NavigationDelegate navigation_;
};
// Right of a wide row's text: its visible recovery action, the time column and
// a slot reserved for copy and more, independently of hover and focus.
struct Columns {QRect item,created,primary,actions;};
Columns historyColumns(const QRect& row,int timeWidth,int primary=0){
 Columns c;const int h=Ui::scaled(28),slot=2*h+space(Space::xs);
 c.actions=QRect(row.right()-slot+1,row.top(),slot,row.height());
 const int timeRight=c.actions.left()-space(Space::m);c.created=QRect(timeRight-timeWidth,row.top(),timeWidth,row.height());
 int textRight=c.created.left()-space(Space::l);if(primary){c.primary=QRect(textRight-primary,row.top(),primary,row.height());textRight=c.primary.left()-space(Space::l);}
 c.item=QRect(row.left(),row.top(),qMax(0,textRight-row.left()),row.height());return c;
}
// Row metrics follow only the application font and the UI family, and the
// list asks for them many times per row in every frame (#1064).
class FontMetric {
public:
 template<class Compute> int operator()(Compute compute){const auto font=QApplication::font();const auto family=Ui::uiFamily();if(!valid_||font!=font_||family!=family_){font_=font;family_=family;value_=compute();valid_=true;}return value_;}
private:QFont font_;QString family_;int value_=0;bool valid_=false;
};
// A day section's heading: 16 px semibold with its count beside it. The first
// sits under the header's own margin; every later day gets a hairline and 12 px
// of extra space above.
int dayHeadingLine(){static FontMetric memo;return memo([]{return qMax(Ui::lineHeight(Ui::Type::section),QFontMetrics(Ui::font(Ui::Type::section,true)).height());});}
int dayHeadingHeight(bool first=false){return (first?0:space(Space::m)+1+space(Space::l))+dayHeadingLine()+space(Space::xs);}
// The heading pinned over the list while its rows scroll under it.
int pinnedHeadingHeight(){return space(Space::s)+dayHeadingLine()+space(Space::s)+1;}
void paintDayHeading(QPainter& p,const QRect& line,const QString& title,const QString& count){
 const auto& t=Theme::current();const auto font=Ui::font(Ui::Type::section,true),note=Ui::font(Ui::Type::caption);const QFontMetricsF metrics(font);
 const auto shown=metrics.elidedText(title,Qt::ElideRight,line.width());const qreal width=metrics.horizontalAdvance(shown);
 Ui::paintText(p,QRectF(line.left(),line.top(),width+1,line.height()),shown,t.text,font,Qt::AlignLeft|Qt::AlignVCenter,Qt::ElideNone);
 if(!count.isEmpty()&&width+space(Space::s)<line.width()){const qreal baseline=Ui::opticalBaseline(metrics,shown,line.center().y()+0.5);p.save();p.setFont(note);p.setPen(t.textMuted);p.drawText(QPointF(line.left()+width+space(Space::s),baseline),QFontMetricsF(note).elidedText(count,Qt::ElideRight,line.width()-width-space(Space::s)));p.restore();}
}
// The time column fits the widest text it can show at this locale and text
// size: relative times, the short time of day, and Failed uploads' dates.
int timeColumnWidth(bool dates){
 static FontMetric plain,dated;
 auto measure=[dates]{const QFontMetrics metrics(Ui::font(Ui::Type::technical));int widest=0;const QDateTime now=QDateTime::currentDateTime();
  QStringList samples{QStringLiteral("Just now"),QStringLiteral("59 min ago")};for(const int hour:{0,11,12,23})samples.append(HistoryModel::timeColumn(QDateTime(QDate::currentDate().addDays(-1),QTime(hour,59)),false));
  if(dates)for(int month=1;month<=12;++month){const QDateTime when(QDate(now.date().year()-1,month,28),QTime(23,59));samples.append(HistoryModel::timeColumn(when,true));}
  for(const auto& sample:samples)widest=qMax(widest,metrics.horizontalAdvance(sample));return widest+2;};
 return dates?dated(measure):plain(measure);
}
// Compact targets follow text enlargement.
int actionHeight(){return Ui::scaled(28);}
constexpr int historyThumbWidth=96,historyThumbHeight=60;
int rowTextInset(){return space(Ui::scaled(100)>125?Space::xs:Space::s);}
int compactTextInset(){return Ui::scaled(100)>125?qMin(space(Space::xs),Ui::Metric::space):rowTextInset();}
QFont historyTitleFont(bool selected=false){auto font=Ui::font(Ui::Type::body);font.setWeight(selected?QFont::DemiBold:QFont::Medium);return font;}
int compactMetadataGap(){static FontMetric memo;return memo([]{return Ui::scaled(100)>125?qMin(space(Space::xs),Ui::Metric::space):qMax(space(Space::xs),(actionHeight()-QFontMetrics(Ui::font(Ui::Type::technical)).height())/2);});}
int compactDetailGap(){return qMax(compactMetadataGap(),(actionHeight()-QFontMetrics(Ui::font(Ui::Type::technical)).height()+1)/2);}
int rowContentHeight(){static FontMetric memo;return memo([]{const int title=QFontMetrics(historyTitleFont()).height(),meta=QFontMetrics(Ui::font(Ui::Type::technical)).height();return qMax(Ui::Metric::historyRow,qMax(historyThumbHeight+4,qMax(title+meta+space(Space::xs)+2*compactTextInset()+4,compactTextInset()+title+compactMetadataGap()+(meta+actionHeight()+1)/2+4)));});}
// Compact insets leave room for the taller outcome without losing a whole row.
int galleryTextHeight(){return QFontMetrics(historyTitleFont()).height()+space(Space::s)+2*QFontMetrics(Ui::font(Ui::Type::technical)).height()+space(Space::s)+actionHeight();}
// The picture spends only the space left after the caption and status lines.
int galleryPicture(int cardWidth,int cardHeight){return qMax(1,qMin((cardWidth-2)*10/16,cardHeight-1-2*space(Space::m)-galleryTextHeight()));}
}
void HistoryWindow::metadataDocument(QTextDocument& document,const QModelIndex& index,int width,bool createdColumn,bool bounded,bool compact,bool separated,int failureFacts){
 document.setDocumentMargin(0);document.setDefaultFont(Ui::font(Ui::Type::technical));QTextOption option;option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);document.setDefaultTextOption(option);
 const auto record=index.data(HistoryModel::RecordRole).toJsonObject();const auto& t=Theme::current();auto text=HistoryModel::failed(record)?HistoryModel::statusText(record):index.data(HistoryModel::SubtitleRole).toString();
 Q_UNUSED(createdColumn);
 // A wide row's one detail line: kind · size · destination · state. State
 // words are never cut; the destination elides first (or goes, rather than
 // shrink below its first word), then the size and the file count go, and
 // last the kind by the destination's rule, so no "Re…" stub is left.
 if(bounded&&!compact&&!separated&&!HistoryModel::failed(record)){
  const auto parts=text.split(" · ",Qt::SkipEmptyParts);const auto destination=index.data(HistoryModel::OutputRole).toString();
  const auto kind=HistoryModel::friendlyAction(record["operation"].toObject()["action"].toString(record["category"].toString())),size=HistoryModel::fileSize(record["bytes"].toInteger());
  const bool stored=!record["records"].toArray().isEmpty();
  // Which parts may give way: 0 kind, 1 size, 2 file count, 3 destination; the rest are state words.
  QList<std::pair<int,QString>> shown;bool seenDestination=false;
  for(int i=0;i<parts.size();++i){int role=4;if(stored&&i==0&&parts[i]==kind)role=0;else if(stored&&i==1&&parts[i]==size)role=1;else if(stored&&parts[i].endsWith(" files")&&parts[i].section(' ',0,0).toInt()>1)role=2;else if(stored&&!seenDestination&&parts[i]==destination){role=3;seenDestination=true;}shown.append({role,parts[i]});}
  const auto html=[&](const QList<std::pair<int,QString>>& line){QStringList spans;for(const auto& [role,part]:line)spans.append(metadataSpan(part,metadataColor(record,part,role<4?t.textMuted:t.textMuted),role==4&&outcomePart(record,part)));return spans.join(metadataSpan(" · ",t.textMuted));};
  const auto measure=[&](const QList<std::pair<int,QString>>& line){QTextDocument probe;probe.setDocumentMargin(0);probe.setDefaultFont(document.defaultFont());probe.setHtml(html(line));return probe.idealWidth();};
  const auto without=[&](int role){auto line=shown;line.erase(std::remove_if(line.begin(),line.end(),[role](const auto& part){return part.first==role;}),line.end());return line;};
  if(measure(shown)>width){
   const auto rest=without(3);const qreal spare=width-measure(rest)-QFontMetricsF(document.defaultFont()).horizontalAdvance(" · ");const QFontMetrics metrics(document.defaultFont());
   for(auto& [role,part]:shown)if(role==3){const auto elided=metrics.elidedText(part,Qt::ElideRight,int(spare));const auto first=part.section(' ',0,0);part=elided.size()>first.size()||elided==part?elided:QString{};}
   shown=without(-1);shown.erase(std::remove_if(shown.begin(),shown.end(),[](const auto& part){return part.second.isEmpty();}),shown.end());
   for(const int role:{1,2})if(measure(shown)>width)shown=without(role);
   if(measure(shown)>width)for(auto& [role,part]:shown)if(role==0){const auto others=without(0);const auto elided=metrics.elidedText(part,Qt::ElideRight,qMax(0,int(width-measure(others)-metrics.horizontalAdvance(" · "))));const auto first=part.section(' ',0,0);part=elided.size()>first.size()||elided==part?elided:QString{};}
   shown.erase(std::remove_if(shown.begin(),shown.end(),[](const auto& part){return part.second.isEmpty();}),shown.end());
  }
  option.setWrapMode(QTextOption::NoWrap);document.setDefaultTextOption(option);document.setHtml(html(shown));document.setTextWidth(qMax(20,width));return;
 }
 if(compact){
  const auto created=index.data(HistoryModel::CreatedRole).toString();const auto kind=HistoryModel::friendlyAction(record["operation"].toObject()["action"].toString(record["category"].toString())),size=HistoryModel::fileSize(record["bytes"].toInteger());
  const auto destination=HistoryModel::failed(record)?QString{}:index.data(HistoryModel::OutputRole).toString();auto parts=text.split(" · ",Qt::SkipEmptyParts);parts.removeAll(created);parts.removeAll(kind);parts.removeAll(size);if(!destination.isEmpty())parts.removeOne(destination);
  if(HistoryModel::failed(record)){parts={HistoryModel::failureLabel(record)};parts+=failureDetail(record).split(" · ",Qt::SkipEmptyParts).mid(0,failureFacts);}const auto status=parts.join(" · ");const QFontMetrics facts(Ui::font(Ui::Type::technical));
  const auto span=[&](const QString& part){return metadataSpan(part,metadataColor(record,part,parts.contains(part)?t.textSecondary:t.textMuted),parts.contains(part));};
  const auto measured=[&](const QStringList& shown){QStringList spans;for(const auto& part:shown.join(" · ").split(" · ",Qt::SkipEmptyParts))spans.append(span(part));QTextDocument measure;measure.setDocumentMargin(0);measure.setDefaultFont(document.defaultFont());measure.setHtml(spans.join(metadataSpan(" · ",t.textSecondary)));return measure.idealWidth();};
  QStringList shown;if(!HistoryModel::failed(record)){if(!kind.isEmpty())shown.append(kind);if(!size.isEmpty())shown.append(size);if(!destination.isEmpty())shown.append(destination);}if(!status.isEmpty())shown.append(status);if(!created.isEmpty())shown.append(created);
  for(const auto& optional:{destination,size,kind})if(!optional.isEmpty()&&measured(shown)>width)shown.removeAll(optional);
  if(!status.isEmpty()&&measured(shown)>width){shown.removeAll(status);const int reserved=created.isEmpty()?0:int(std::ceil(measured({created})))+facts.horizontalAdvance(" · ");const auto shortened=facts.elidedText(status,Qt::ElideRight,qMax(0,width-reserved));if(!shortened.isEmpty())shown.prepend(shortened);}
  QStringList spans;for(const auto& part:shown.join(" · ").split(" · ",Qt::SkipEmptyParts)){auto original=part;if(part.endsWith(QChar(0x2026)))for(const auto& candidate:parts)if(candidate.startsWith(part.chopped(1))){original=candidate;break;}spans.append(metadataSpan(part,metadataColor(record,original,parts.contains(original)?t.textSecondary:t.textMuted),parts.contains(original)));}
  option.setWrapMode(QTextOption::NoWrap);document.setDefaultTextOption(option);document.setHtml(spans.join(metadataSpan(" · ",t.textSecondary)));document.setTextWidth(qMax(20,width));return;
 }
 if(bounded&&!HistoryModel::failed(record))text=QFontMetrics(document.defaultFont()).elidedText(text,Qt::ElideRight,qMax(20,width));
 if(HistoryModel::failed(record)&&!separated){
  // Something to read before going on: body size, the destination in the label.
  auto line=index.data(HistoryModel::SubtitleRole).toString();if(line.isEmpty())line=HistoryModel::statusText(record);const auto label=line.section(" · ",0,0);auto detail=line.mid(label.size());const auto body=Ui::font(Ui::Type::body);document.setDefaultFont(body);
  document.setHtml("<span style=\"color:"+t.danger.name()+";font-size:"+QString::number(body.pixelSize())+"px\">"+label.toHtmlEscaped()+"</span><span style=\"color:"+t.textSecondary.name()+";font-size:"+QString::number(body.pixelSize())+"px\">"+statusPhrases(detail,true).toHtmlEscaped()+"</span>");
 }
 else if(HistoryModel::failed(record)){const auto label=HistoryModel::failureLabel(record);auto detail=text;if(detail.startsWith(label))detail.remove(0,label.size());document.setHtml(metadataSpan(label,t.danger,true)+"<span style=\"color:"+t.textSecondary.name()+";font-size:"+QString::number(Ui::font(Ui::Type::technical).pixelSize())+"px\">"+statusPhrases(detail,true).toHtmlEscaped()+"</span>");}
 else{QStringList spans;for(const auto& part:text.split(" · ",Qt::SkipEmptyParts))spans.append(metadataSpan(part,metadataColor(record,part,t.textMuted),outcomePart(record,part)));document.setHtml(spans.join(metadataSpan(" · ",t.textMuted)));}
 if(separated){QStringList spans;for(const auto& part:text.split(" · ",Qt::SkipEmptyParts))spans.append(metadataSpan(part,metadataColor(record,part,HistoryModel::failed(record)?t.textSecondary:t.textMuted),HistoryModel::failed(record)||outcomePart(record,part)));option.setWrapMode(QTextOption::NoWrap);document.setDefaultTextOption(option);document.setHtml(Ui::separatedLines(spans,Ui::font(Ui::Type::technical),width));}document.setTextWidth(qMax(20,width));
 if(HistoryModel::failed(record)){
  const auto created=index.data(HistoryModel::CreatedRole).toString();if(created.isEmpty())return;
  // The time has its own column beside a wide row.
  if(!separated)return;
  // Age spends only spare metadata space; it never displaces failure details.
  const auto original=document.toHtml();const qreal available=document.size().height();
  if(separated){QStringList spans;for(const auto& part:text.split(" · ",Qt::SkipEmptyParts))spans.append(metadataSpan(part,metadataColor(record,part,t.textSecondary),true));spans.append(metadataSpan(created,t.textMuted));document.setHtml(Ui::separatedLines(spans,Ui::font(Ui::Type::technical),width));}
  else{QTextCursor cursor(&document);cursor.movePosition(QTextCursor::End);QTextCharFormat format;format.setFont(Ui::font(Ui::Type::technical));format.setForeground(t.textMuted);cursor.insertText(QString(QChar(0x00a0))+"· "+created,format);}
  document.setTextWidth(qMax(20,width));document.size();if(separated?document.begin().layout()->lineCount()>2:document.size().height()>available)document.setHtml(original);
 }
}
namespace {
// The row's body: right of the thumbnail in a list, under it in a gallery tile.
QRect rowBody(const QRect& outer,bool gallery,int heading){
 const auto card=gallery?outer.adjusted(space(Space::s),space(Space::s),-space(Space::s),-space(Space::s)):outer.adjusted(space(Space::s),heading+2,-space(Space::s),-2);
 if(gallery){const int top=card.top()+1+galleryPicture(card.width(),card.height())+space(Space::m);return QRect(card.left()+space(Space::l),top,card.width()-2*space(Space::l),qMax(0,card.bottom()-top+1));}
 const int left=card.left()+space(Space::s)+historyThumbWidth+space(Space::l);
 return QRect(left,card.top(),card.right()-space(Space::s)-left+1,rowContentHeight()-4);
}

}
QRect HistoryWindow::failureTextArea(const QSize& row,bool gallery,bool dayHeading){
 const auto body=rowBody(QRect(QPoint(),row),gallery,gallery||!dayHeading?0:dayHeadingHeight());
 const auto title=historyTitleFont(),meta=Ui::font(Ui::Type::technical);
 const bool compact=!gallery&&body.width()<Ui::scaled(480);
 const int top=body.top()+(gallery?0:compact?compactTextInset():rowTextInset())+QFontMetrics(title).height();
 const int extra=compact?compactMetadataGap()+compactDetailGap()+QFontMetrics(meta).height():6;
 return {body.left(),top+extra,qMax(20,body.width()),QFontMetrics(meta).height()};
}
namespace {
// Painted rows retain native selection and expose their commands to assistive technology.
class AccessibleHistoryItem final:public QAccessibleInterface,public QAccessibleActionInterface {
public:
 AccessibleHistoryItem(QListView* view,const QModelIndex& index,std::function<void(const QModelIndex&)> primary):view_(view),index_(index),primary_(std::move(primary)){}
 bool isValid()const override{return view_&&index_.isValid();}QObject* object()const override{return nullptr;}
 QAccessibleInterface* parent()const override{return view_?QAccessible::queryAccessibleInterface(view_):nullptr;}
 QAccessibleInterface* child(int)const override{return nullptr;}int childCount()const override{return 0;}int indexOfChild(const QAccessibleInterface*)const override{return -1;}
 QAccessibleInterface* childAt(int,int)const override{return nullptr;}
 QString text(QAccessible::Text type)const override{if(!isValid())return {};const bool failed=index_.data(HistoryModel::FailedRole).toBool();return type==QAccessible::Name?(failed?failureDescription(index_):index_.data(Qt::AccessibleTextRole).toString()):type==QAccessible::Description?(failed?failureDescription(index_)+". Enter for details, Menu for actions":QString("Enter for details, Menu for actions")):QString{};}
 void setText(QAccessible::Text,const QString&)override{}
 QRect rect()const override{return isValid()?QRect(view_->viewport()->mapToGlobal(view_->visualRect(index_).topLeft()),view_->visualRect(index_).size()).intersected(QRect(view_->viewport()->mapToGlobal(QPoint()),view_->viewport()->size())):QRect{};}
 QAccessible::Role role()const override{return QAccessible::ListItem;}
 QAccessible::State state()const override{QAccessible::State state;state.invalid=!isValid();if(!isValid())return state;state.selectable=true;state.multiSelectable=true;state.focusable=true;state.selected=view_->selectionModel()->isSelected(index_);state.focused=view_->hasFocus()&&view_->currentIndex()==index_;state.disabled=!view_->isEnabled();state.invisible=!view_->isVisible();state.offscreen=rect().isEmpty();return state;}
 void* interface_cast(QAccessible::InterfaceType type)override{return type==QAccessible::ActionInterface?static_cast<QAccessibleActionInterface*>(this):nullptr;}
 QStringList actionNames()const override{if(!isValid())return {};QStringList names{QAccessibleActionInterface::setFocusAction(),QAccessibleActionInterface::toggleAction(),QAccessibleActionInterface::showMenuAction()};const auto action=index_.data(HistoryModel::PrimaryActionRole).toString();if(!action.isEmpty())names.append(action);return names;}
 void doAction(const QString& name)override{if(!isValid()||!view_->isEnabled())return;if(name==QAccessibleActionInterface::setFocusAction()){view_->setFocus();view_->setCurrentIndex(index_);view_->scrollTo(index_);}else if(name==QAccessibleActionInterface::toggleAction())view_->selectionModel()->select(index_,QItemSelectionModel::Toggle|QItemSelectionModel::Rows);else if(name==QAccessibleActionInterface::showMenuAction()){view_->setCurrentIndex(index_);emit view_->customContextMenuRequested(view_->visualRect(index_).center());}else if(name==index_.data(HistoryModel::PrimaryActionRole).toString()&&primary_)primary_(index_);}
 QStringList keyBindingsForAction(const QString& name)const override{return name==QAccessibleActionInterface::showMenuAction()?QStringList{"Menu","Shift+F10"}:QStringList{};}
 const QPersistentModelIndex& index()const{return index_;}
private:QPointer<QListView> view_;QPersistentModelIndex index_;std::function<void(const QModelIndex&)> primary_;
};
class AccessibleHistoryList final:public QAccessibleWidget {
public:
 std::function<void(const QModelIndex&)> primaryAction;
 explicit AccessibleHistoryList(QListView* view):QAccessibleWidget(view,QAccessible::List),view_(view){}
 int childCount()const override{return view_->model()?view_->model()->rowCount():0;}
 ~AccessibleHistoryList()override{for(const auto id:items_)QAccessible::deleteAccessibleInterface(id);}
 QAccessibleInterface* child(int row)const override{if(row<0||row>=childCount())return nullptr;const auto index=view_->model()->index(row,0);for(const auto id:items_)if(auto* item=static_cast<AccessibleHistoryItem*>(QAccessible::accessibleInterface(id));item&&item->index()==index)return item;auto primary=[this](const QModelIndex& i){if(primaryAction)primaryAction(i);};auto* item=new AccessibleHistoryItem(view_,index,primary);items_.append(QAccessible::registerAccessibleInterface(item));return item;}
 int indexOfChild(const QAccessibleInterface* child)const override{const auto* item=dynamic_cast<const AccessibleHistoryItem*>(child);return item&&item->index().isValid()?item->index().row():-1;}
 QAccessibleInterface* childAt(int x,int y)const override{const auto index=view_->indexAt(view_->viewport()->mapFromGlobal(QPoint(x,y)));return index.isValid()?child(index.row()):nullptr;}
 QAccessibleInterface* focusChild()const override{return view_->hasFocus()?child(view_->currentIndex().row()):nullptr;}
private:QListView* view_;mutable QList<QAccessible::Id> items_;
};
class AccessibleHistoryPreview final:public QAccessibleWidget {
public:
 explicit AccessibleHistoryPreview(HistoryPreview* preview):QAccessibleWidget(preview,QAccessible::Button),preview_(preview){}
 QStringList actionNames()const override{return preview_->expandable()?QStringList{QAccessibleActionInterface::pressAction(),QAccessibleActionInterface::setFocusAction()}:QStringList{};}
 void doAction(const QString& name)override{if(name==QAccessibleActionInterface::pressAction()&&preview_->expandable())preview_->activate();else QAccessibleWidget::doAction(name);}
 QStringList keyBindingsForAction(const QString& name)const override{return name==QAccessibleActionInterface::pressAction()?QStringList{"Space","Enter"}:QStringList{};}
private:HistoryPreview* preview_;
};
QAccessibleInterface* historyAccessibility(const QString&,QObject* object){if(auto* preview=dynamic_cast<HistoryPreview*>(object);preview&&preview->activate)return new AccessibleHistoryPreview(preview);auto* view=qobject_cast<QListView*>(object);return view&&view->objectName()=="historyList"?new AccessibleHistoryList(view):nullptr;}
class HistoryDelegate final:public QStyledItemDelegate {
public:
 explicit HistoryDelegate(QListView* view):QStyledItemDelegate(view),view_(view){
  paintSecondary_=Ui::toolButton({}, {},"secondary",view_->viewport());paintSecondary_->setProperty("uiDensity","compact");paintSecondary_->hide();paintQuiet_=Ui::toolButton({}, {},"quiet",view_->viewport());paintQuiet_->setProperty("uiDensity","compact");paintQuiet_->hide();
  primary_=Ui::toolButton({}, {},"secondary",view_->viewport());primary_->setObjectName("historyRowPrimary");primary_->setToolButtonStyle(Qt::ToolButtonTextOnly);primary_->setProperty("uiDensity","compact");primary_->hide();connect(primary_,&QToolButton::clicked,this,[this]{const auto i=grid()?QModelIndex(actionIndex_):view_->currentIndex();if(i.isValid()&&primaryShown(i)&&primary)primary(i);});
  more_=Ui::toolButton({}, {},"quiet",view_->viewport());more_->setObjectName("historyRowMore");more_->setIcon(Ui::chromeIcon("more"));more_->setProperty("uiDensity","compact");more_->hide();view_->viewport()->installEventFilter(this);
  // Opaque, so scrolling moves the pixels already drawn and paints only the
  // strip scrolled in; eventFilter fills what the transparent view used to show.
  view_->viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
  details_=Ui::toolButton({}, {},"quiet",view_->viewport());details_->setObjectName("historyRowDetails");details_->setIcon(Ui::chromeIcon("chevron-right"));details_->setAccessibleName("Show details");details_->setAccessibleDescription("Open the selected item's details. Enter also opens details.");details_->setToolTip("Show details (Enter)");details_->setProperty("uiDensity","compact");details_->hide();connect(details_,&QToolButton::clicked,this,[this]{if(details)details(view_->currentIndex());});
  connect(more_,&QToolButton::clicked,this,[this]{const auto i=grid()?QModelIndex(actionIndex_):view_->currentIndex();if(i.isValid()&&menu){menuOpen_=true;menu(i,more_->mapToGlobal(QPoint(0,more_->height())));menuOpen_=false;updateMore();}});
  for(auto* button:{primary_,more_})button->installEventFilter(this);QWidget::setTabOrder(view_,primary_);QWidget::setTabOrder(primary_,more_);
  auto update=[this]{QTimer::singleShot(0,this,[this]{updateMore();});};
  connect(view_->selectionModel(),&QItemSelectionModel::currentChanged,this,update);connect(view_->selectionModel(),&QItemSelectionModel::selectionChanged,this,update);connect(view_->verticalScrollBar(),&QScrollBar::valueChanged,this,update);connect(view_->model(),&QAbstractItemModel::modelReset,this,update);
  auto cancel=[this]{cancelPress();};connect(view_->model(),&QAbstractItemModel::modelAboutToBeReset,this,cancel);connect(view_->model(),&QAbstractItemModel::modelReset,this,[this]{captions_.clear();metadata_.clear();forgetPass();thumbnails_.setMaxCost(HistoryModel::thumbnailCacheCost(view_->model()->rowCount()));});connect(&Theme::Notifier::instance(),&Theme::Notifier::changed,this,[this]{captions_.clear();metadata_.clear();forgetPass();primary_->setProperty("historyGlyph",QVariant());});
  for(const auto change:{&QAbstractItemModel::rowsInserted,&QAbstractItemModel::rowsRemoved})connect(view_->model(),change,this,[this]{metadata_.clear();forgetPass();});
  connect(view_->model(),&QAbstractItemModel::layoutChanged,this,[this]{metadata_.clear();forgetPass();});
  connect(view_->model(),&QAbstractItemModel::dataChanged,this,[this](const QModelIndex&,const QModelIndex&,const QList<int>& roles){if(roles!=QList<int>{HistoryModel::ThumbnailRole}){metadata_.clear();forgetPass();}});connect(view_->model(),&QAbstractItemModel::rowsAboutToBeRemoved,this,cancel);connect(view_->model(),&QAbstractItemModel::layoutAboutToBeChanged,this,cancel);connect(view_->model(),&QAbstractItemModel::dataChanged,this,[this](const QModelIndex&,const QModelIndex&,const QList<int>& roles){if(roles.isEmpty()||roles.contains(HistoryModel::PrimaryActionRole))cancelPress();});
 }
 void updateMore(){
  // This runs on every viewport paint, so it changes the buttons only when
  // their state differs: each change repaints them, and the viewport under them.
  const auto set=[](QObject* object,const char* name,const QVariant& value){if(object->property(name)!=value)object->setProperty(name,value);};

  QModelIndex i=view_->currentIndex();
  if(grid()){
   if((primary_->hasFocus()||more_->hasFocus()||primary_->underMouse()||more_->underMouse()||menuOpen_)&&actionIndex_.isValid())i=actionIndex_;
   else if(hovered_.isValid())i=hovered_;
  }
  const auto row=view_->visualRect(i);const auto bounds=i.isValid()?menuRect(row,i):QRect();
  const bool visible=i.isValid()&&inlineActions(i)&&view_->viewport()->rect().intersects(bounds)&&bounds.top()>=(covered?covered():0)&&(grid()||view_->selectionModel()->isSelected(i))&&(grid()||view_->selectionModel()->selectedRows().size()<=1||row.contains(view_->viewport()->mapFromGlobal(QCursor::pos())));
  actionIndex_=i;set(more_,"uiOnMedia",grid());set(primary_,"uiOnMedia",grid()&&i.isValid()&&compactIcon(i));set(details_,"uiOnMedia",false);more_->setVisible(visible);const bool showDetails=visible&&detailsShown(i)&&view_->selectionModel()->selectedRows().size()==1;const bool showPrimary=visible&&primaryShown(i);
  set(primary_,"visualRole",QString((grid()&&!recovery(i))||(!grid()&&compactIcon(i))?"quiet":"secondary"));primary_->setVisible(showPrimary);
  if(showPrimary){const auto label=i.data(HistoryModel::PrimaryActionRole).toString();primary_->setEnabled(true);const bool icon=iconPrimary(row,i);const QString glyph=label=="Copy link"||icon?"copy":"";if(primary_->property("historyGlyph").toString()!=glyph||primary_->icon().isNull()!=glyph.isEmpty()){primary_->setIcon(glyph.isEmpty()?QIcon{}:Ui::icon(glyph));primary_->setProperty("historyGlyph",glyph);}primary_->setIconSize(QSize(Ui::scaled(16),Ui::scaled(16)));primary_->setToolButtonStyle(icon?Qt::ToolButtonIconOnly:label=="Copy link"?Qt::ToolButtonTextBesideIcon:Qt::ToolButtonTextOnly);primary_->setText(label);primary_->setToolTip(label);primary_->setAccessibleName(label);primary_->setAccessibleDescription(label+" for "+i.data().toString());primary_->setGeometry(primaryRect(row,i));}
  details_->setVisible(showDetails);details_->setText(grid()?"Details":QString{});details_->setToolButtonStyle(grid()?Qt::ToolButtonTextOnly:Qt::ToolButtonIconOnly);if(showDetails){details_->setGeometry(detailsRect(row,i));}
  if(!visible)return;const auto name="More actions for "+i.data(HistoryModel::RecordRole).toJsonObject()["name"].toString();more_->setAccessibleName(name);more_->setToolTip(name);more_->setGeometry(bounds);raiseActions();
 }
 // Keep the shown actions above the cards, raising only when out of order:
 // a raise repaints the viewport, and every viewport paint runs updateMore.
 void raiseActions(){
  QWidgetList order,shown;for(auto* button:{primary_,details_,more_})if(button->isVisible())order.append(button);
  for(auto* child:view_->viewport()->children())if(auto* widget=qobject_cast<QWidget*>(child);widget&&widget->isVisible())shown.append(widget);
  if(shown.mid(shown.size()-order.size())!=order)for(auto* button:order)button->raise();
 }
 bool helpEvent(QHelpEvent* event,QAbstractItemView* view,const QStyleOptionViewItem& option,const QModelIndex& i)override{
  // Qt also asks about the gaps between gallery tiles, where there is no row.
  if(!i.isValid())return QStyledItemDelegate::helpEvent(event,view,option,i);
  if(wide(option.rect,i)&&timeRect(option.rect,i).contains(event->pos())){QToolTip::showText(event->globalPos(),"Created "+i.data(HistoryModel::CreatedTipRole).toString(),view);return true;}
  if(i.data(HistoryModel::FailedRole).toBool()&&!menuRect(option.rect,i).contains(event->pos())){QToolTip::showText(event->globalPos(),failureDescription(i),view);return true;}
  if(inlineActions(i)&&menuRect(option.rect,i).contains(event->pos())){QToolTip::showText(event->globalPos(),"More actions for "+i.data(HistoryModel::RecordRole).toJsonObject()["name"].toString(),view);return true;}return QStyledItemDelegate::helpEvent(event,view,option,i);
 }
 bool eventFilter(QObject* watched,QEvent* event)override{
  if(grid()&&watched==view_->viewport()&&(event->type()==QEvent::MouseMove||event->type()==QEvent::Leave)){
   const auto hovered=event->type()==QEvent::Leave?QModelIndex{}:view_->indexAt(static_cast<QMouseEvent*>(event)->position().toPoint());if(hovered_!=hovered){hovered_=hovered;updateMore();view_->viewport()->update();}
  }
  if((watched==primary_||watched==more_)&&(event->type()==QEvent::FocusIn||event->type()==QEvent::FocusOut||event->type()==QEvent::Leave))QTimer::singleShot(0,this,[this]{updateMore();view_->viewport()->update();});
  if(watched==view_->viewport()&&event->type()==QEvent::MouseMove&&pressed_.isValid()&&(static_cast<QMouseEvent*>(event)->position().toPoint()-pressPosition_).manhattanLength()>QApplication::startDragDistance())cancelPress();
  if(watched==view_->viewport()&&(event->type()==QEvent::Hide||event->type()==QEvent::UngrabMouse))cancelPress();
  if(watched==view_->viewport()&&event->type()==QEvent::Hide)thumbnails_.clear();
  if(watched==view_->viewport()&&event->type()==QEvent::Paint){QPainter painter(view_->viewport());painter.fillRect(static_cast<QPaintEvent*>(event)->rect(),Theme::current().bg);}
  if(watched==view_->viewport()&&(event->type()==QEvent::Resize||event->type()==QEvent::Show||event->type()==QEvent::Paint)){
   updateMore();
  }
  if(watched==view_->viewport()&&event->type()==QEvent::Resize){
   // A scrollbar changes the wrapping width after the first layout. Recompute
   // the scroll range from that final width so an oversized error's last lines
   // remain reachable, including after selecting a row or resizing the splitter.
   QTimer::singleShot(0,view_,[view=view_]{view->doItemsLayout();});
  }
  if(watched==primary_||watched==more_)return false;
  return QStyledItemDelegate::eventFilter(watched,event);
 }
 std::function<void(const QModelIndex&)> primary,details;
 std::function<void(const QModelIndex&,const QPoint&)> menu;
 // Whether a gallery card already has its details open beside it.
 std::function<bool()> inspectorBeside,compactList;
 bool grid()const{return view_->viewMode()==QListView::IconMode;}
 bool menuOnly()const{return !grid();}
 bool dated()const{const auto* model=qobject_cast<HistoryModel*>(view_->model());return model&&!model->daySections();}
 // Copy link and Copy result use the reserved slot; a recovery action has a labelled button left of the time.
 bool slotPrimary(const QModelIndex& i)const{return compactIcon(i);}
 Columns rowColumns(const QRect& body,const QModelIndex& i)const{return historyColumns(body,timeColumnWidth(dated()),!i.data(HistoryModel::PrimaryActionRole).toString().isEmpty()&&!slotPrimary(i)?primaryWidth(i):0);}
 bool wide(const QRect& r,const QModelIndex& i)const{return !grid()&&!compactRow(r,i);}
 int actionTop(const QRect& r,const QModelIndex& i)const{const auto c=card(r,i);return i.data(HistoryModel::FailedRole).toBool()?c.top()+(c.height()-actionHeight())/2:c.top()+(rowContentHeight()-4-actionHeight())/2;}
 bool beside(const QModelIndex& i)const{return menuOnly()||(grid()&&inspectorBeside&&inspectorBeside()&&view_->selectionModel()->isSelected(i));}
 // Details open from Enter, a double-click, the row menu or the header's details toggle.
 bool detailsShown(const QModelIndex&)const{return false;}
 bool recovery(const QModelIndex& i)const{return i.data(HistoryModel::PrimaryActionRole).toString().startsWith("Retry ");}
 // An open inspector holds compact rows' actions; only failures keep Retry in the row.
 bool inlineActions(const QModelIndex& i)const{if(grid())return true;if(!inspectorBeside||!inspectorBeside())return true;return !(view_->selectionModel()->selectedRows().size()==1&&view_->selectionModel()->isSelected(i))&&(!compactRows()||i.data(HistoryModel::FailedRole).toBool());}
 bool rowPrimary(const QModelIndex& i)const{if(!inlineActions(i))return false;return !i.data(HistoryModel::PrimaryActionRole).toString().isEmpty();}
 bool primaryShown(const QModelIndex& i)const{if(grid())return !i.data(HistoryModel::PrimaryActionRole).toString().isEmpty()&&inlineActions(i);return rowPrimary(i)&&inlineActions(i)&&(view_->selectionModel()->selectedRows().size()<=1||recovery(i))&&!i.data(HistoryModel::PrimaryActionRole).toString().isEmpty();}
 int heading(const QModelIndex& i)const{return !grid()&&!i.data(HistoryModel::DayRole).toString().isEmpty()?dayHeadingHeight(i.row()==0):0;}
 QRect card(QRect r,const QModelIndex& i)const{return grid()?r.adjusted(space(Space::s),space(Space::s),-space(Space::s),-space(Space::s)):r.adjusted(space(Space::s),heading(i)+2,-space(Space::s),-2);}
 // Cards share their row; picture height stays bounded even in a single column.
 // The flow layout always reserves a scrollbar's extent (QListViewPrivate::prepareItemsLayout).
 int galleryWidth()const{const int s=view_->spacing(),reserve=view_->style()->pixelMetric(QStyle::PM_ScrollView_ScrollBarOverlap,nullptr,view_->verticalScrollBar())?0:view_->style()->pixelMetric(QStyle::PM_ScrollBarExtent,nullptr,view_->verticalScrollBar());const int available=qMax(160,view_->maximumViewportSize().width()-reserve-2*s),minimum=qMin(Ui::scaled(248),310),columns=qMax(1,available/(minimum+s)),width=available/columns-s-1;return qMax(160,width);}
 // Failure metadata wraps below the title and reserved actions.
 // Hover and selection never change the reserved height.
 int listDetailsSpace()const{return 0;}
 int detailsWidth()const{return qMax(details_->sizeHint().width(),QFontMetrics(Ui::font()).horizontalAdvance("Details")+30);}
 // A tile's caption, the same three lines on every tile: the name on up to two
 // lines, then the list's detail line with the time at its right. A failure
 // keeps one line of name and gives the next two to where the upload was going
 // and its recovery text, or "Details in ⋯"; when neither fits its row grows.
 struct GalleryCaption {QStringList title;std::shared_ptr<QTextLayout> failure;std::shared_ptr<QTextDocument> detail;int height=0;bool time=true;};
 // Every caption's size, paint and action placement asks for its whole row's.
 // Keep each one until the text it shows, its width or the theme changes.
 GalleryCaption galleryCaption(const QModelIndex& i,int width)const{
  const bool failed=i.data(HistoryModel::FailedRole).toBool();
  const auto source=QStringList{i.data().toString(),i.data(HistoryModel::SubtitleRole).toString(),i.data(HistoryModel::CreatedRole).toString(),i.data(HistoryModel::OutputRole).toString(),QString::number(failed),QString::number(Ui::font(Ui::Type::technical).pixelSize())}.join(QChar(0x1f));
  auto cached=captions_.constFind({i.row(),width});
  if(cached!=captions_.cend()&&cached->first==source)return cached->second;
  if(captions_.size()>4096)captions_.clear();
  const auto caption=layoutGalleryCaption(i,width);captions_.insert({i.row(),width},{source,caption});return caption;
 }
 static int galleryTimeWidth(const QModelIndex& i){return QFontMetrics(Ui::font(Ui::Type::technical)).horizontalAdvance(i.data(HistoryModel::CreatedRole).toString());}
 GalleryCaption layoutGalleryCaption(const QModelIndex& i,int width)const{
  GalleryCaption caption;const auto titleFont=historyTitleFont();const QFontMetrics title(titleFont);const auto name=i.data().toString();const bool failed=i.data(HistoryModel::FailedRole).toBool();
  const int meta=QFontMetrics(Ui::font(Ui::Type::technical)).height(),time=galleryTimeWidth(i);
  if(failed)caption.title={title.elidedText(name,titleElision(name),width)};
  else{QTextLayout layout(name,titleFont);QTextOption option;option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);layout.setTextOption(option);layout.beginLayout();auto line=layout.createLine();line.setLineWidth(width);const int end=line.textStart()+line.textLength();layout.endLayout();
   const auto rest=name.mid(end).trimmed();caption.title=rest.isEmpty()?QStringList{name}:QStringList{name.left(end).trimmed(),title.elidedText(rest,titleElision(name),width)};}
  if(!failed){caption.detail=std::make_shared<QTextDocument>();HistoryWindow::metadataDocument(*caption.detail,i,qMax(20,width-time-space(Space::s)),false,true);caption.height=2*title.height()+space(Space::xs)+meta;return caption;}
  const auto body=Ui::font(Ui::Type::body);const QFontMetrics bodyMetrics(body);const auto& t=Theme::current();auto line=i.data(HistoryModel::SubtitleRole).toString();if(line.isEmpty())line=HistoryModel::statusText(i.data(HistoryModel::RecordRole).toJsonObject());const auto label=line.section(" · ",0,0);
  const auto lay=[&](const QString& text,bool room=true){
   auto layout=std::make_shared<QTextLayout>(text,body);QTextOption option;option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);layout->setTextOption(option);
   QTextCharFormat danger,rest;danger.setForeground(t.danger);rest.setForeground(t.textSecondary);layout->setFormats({{0,int(label.size()),danger},{int(label.size()),int(text.size()-label.size()),rest}});
   // The last line leaves room for the time at its right.
   const auto pass=[&](int shortFrom){layout->beginLayout();int count=0;while(true){auto made=layout->createLine();if(!made.isValid())break;made.setLineWidth(count>=shortFrom?qMax(20,width-time-space(Space::s)):width);made.setPosition(QPointF(0,count*bodyMetrics.height()));++count;}layout->endLayout();return count;};
   int lines=pass(INT_MAX);if(room)for(int attempt=0;attempt<3;++attempt){const int next=pass(lines-1);if(next==lines)break;lines=next;}
   return layout;};
  // The time takes only spare room on the last line; it never displaces the failure text.
  caption.failure=lay(line);if(caption.failure->lineCount()>2){auto fallback=lay(label+" · Details in ⋯");if(fallback->lineCount()<=2)caption.failure=fallback;}
  {auto bare=lay(caption.failure->text(),false);if(bare->lineCount()<caption.failure->lineCount()||(caption.failure->lineCount()>2&&bare->lineCount()<=2)){caption.failure=bare;caption.time=false;}}
  caption.height=title.height()+space(Space::xs)+qMax(2,caption.failure->lineCount())*bodyMetrics.height();return caption;
 }
 int galleryPrimaryWidth(const QModelIndex& i)const{
  const auto action=i.data(HistoryModel::PrimaryActionRole).toString();if(action.isEmpty())return 0;if(action=="Copy link")return actionHeight();
  if(const auto found=passPrimaryWidths_.constFind(action);found!=passPrimaryWidths_.cend())return *found;
  const int width=qMax(Ui::scaled(84),QFontMetrics(Ui::font(Ui::Type::technical)).horizontalAdvance(action)+space(Space::xxl));holdForPass();passPrimaryWidths_.insert(action,width);return width;
 }
 // Smooth scaling every visible preview on every paint is most of a gallery
 // frame; keep each preview's scaled copy until its image or size changes.
 QImage scaledThumbnail(const QImage& image,const QSize& target)const{
  const auto mode=grid()?Qt::KeepAspectRatioByExpanding:Qt::KeepAspectRatio;const auto key=std::make_pair(image.cacheKey(),std::make_pair(std::make_pair(target.width(),target.height()),int(mode)));
  if(const auto* found=thumbnails_.object(key))return *found;
  const auto scaled=image.scaled(target,mode,Qt::SmoothTransformation);thumbnails_.insert(key,new QImage(scaled),qMax(1,int(scaled.sizeInBytes()/1024)));return scaled;
 }
 // One layout or paint pass asks for each gallery row many times: once per
 // card and again for each card's actions. Its answers hold until the event
 // loop turns, or until the model or theme changes within the pass.
 void holdForPass()const{if(passQueued_)return;passQueued_=true;QTimer::singleShot(0,const_cast<HistoryDelegate*>(this),[this]{forgetPass();});}
 void forgetPass()const{passRows_.clear();passPrimaryWidths_.clear();passQueued_=false;}
 // A row's rich-text metadata is the costliest part of its layout and paint,
 // and scrolling lays out every visible row again. Keep each document until
 // its row data, status line (age, local copy), font or theme changes (#1064).
 std::shared_ptr<QTextDocument> metadataFor(const QModelIndex& i,int width,bool createdColumn,bool bounded=false,bool compact=false,bool separated=false,int facts=0)const{
  const quint64 key=(quint64(quint32(i.row()))<<32)|(quint64(quint16(width))<<16)|(quint64(quint8(facts))<<4)|(createdColumn?1:0)|(bounded?2:0)|(compact?4:0)|(separated?8:0);
  const auto status=i.data(HistoryModel::SubtitleRole).toString()+QChar(u'\n')+i.data(HistoryModel::CreatedRole).toString();const auto font=QApplication::font();
  auto found=metadata_.find(key);
  if(found==metadata_.end()||found->status!=status||found->font!=font){
   if(metadata_.size()>2048)metadata_.clear();
   auto document=std::make_shared<QTextDocument>();HistoryWindow::metadataDocument(*document,i,width,createdColumn,bounded,compact,separated,facts);
   found=metadata_.insert(key,{status,font,document});
  }
  return found->document;
 }
 struct GalleryRow {int caption=0,picture=0,height=0;};
 // The tiles sharing a tile's row: a day section starts a new row (HistoryList).
 std::function<std::pair<int,int>(int)> galleryRun;
 // The height the view's pinned day heading covers at the top of the viewport.
 std::function<int()> covered;
 int galleryColumns()const{const int s=view_->spacing(),reserve=view_->style()->pixelMetric(QStyle::PM_ScrollView_ScrollBarOverlap,nullptr,view_->verticalScrollBar())?0:view_->style()->pixelMetric(QStyle::PM_ScrollBarExtent,nullptr,view_->verticalScrollBar());return qMax(1,(view_->maximumViewportSize().width()-reserve-2*s)/(galleryWidth()+s));}
 GalleryRow galleryRow(const QModelIndex& i,int width)const{
  // Fit is a row decision from full captions, never from interaction state.
  int start=0,end=0;if(galleryRun)std::tie(start,end)=galleryRun(i.row());else{const int columns=galleryColumns();start=i.row()/columns*columns;end=start+columns;}end=qMin(end,i.model()->rowCount());
  const int bodyWidth=qMax(1,width-2*space(Space::s)-2*space(Space::l));
  const auto key=std::make_pair(std::make_pair(start,width),std::make_pair(end,view_->viewport()->height()));
  if(const auto found=passRows_.constFind(key);found!=passRows_.cend())return *found;
  GalleryRow row;for(int index=start;index<end;++index)row.caption=qMax(row.caption,galleryCaption(i.model()->index(index,0),bodyWidth).height);
  const int padding=2*space(Space::s)+1+space(Space::m)+row.caption+space(Space::m);
  row.picture=qMax(1,qMin((width-2*space(Space::s)-2)*10/16,view_->viewport()->height()-2*view_->spacing()-padding));row.height=padding+row.picture;
  holdForPass();passRows_.insert(key,row);return row;
 }
 QRect galleryPictureRect(const QRect& r,const QModelIndex& i)const{const auto c=card(r,i);return {c.left()+1,c.top()+1,c.width()-2,galleryRow(i,r.width()).picture};}
 QRect galleryBody(const QRect& r,const QModelIndex& i)const{const auto c=card(r,i);const int top=c.top()+1+galleryRow(i,r.width()).picture+space(Space::m);return {c.left()+space(Space::l),top,qMax(1,c.width()-2*space(Space::l)),galleryRow(i,r.width()).caption};}
 // Copy link and more overlay the picture's top-right corner; a recovery action sits on its lower-left.
 QRect galleryActionRect(QRect r,const QModelIndex& i,int column)const{
  const auto picture=galleryPictureRect(r,i);const int h=actionHeight();const QRect more(picture.right()-space(Space::s)-h+1,picture.top()+space(Space::s),h,h);
  if(column==2)return more;if(slotPrimary(i))return QRect(more.left()-space(Space::xs)-h,more.top(),h,h);
  const int w=qMin(galleryPrimaryWidth(i),qMax(h,picture.width()-2*space(Space::s)));return QRect(picture.left()+space(Space::s),picture.bottom()-space(Space::s)-h+1,w,h);
 }
 QRect detailsRect(QRect r,const QModelIndex& i)const{if(grid())return galleryActionRect(r,i,1);const auto b=menuRect(r,i);const int w=grid()?detailsWidth():actionHeight();const int h=grid()?b.height():w;return QRect(b.left()-space(Space::s)-w,b.top()+(b.height()-h)/2,w,h);}
 int primaryWidth(const QModelIndex& i)const{return qMax(Ui::scaled(84),int(std::ceil(QFontMetricsF(Ui::labelFont()).horizontalAdvance(i.data(HistoryModel::PrimaryActionRole).toString())))+space(Space::xxl)+(i.data(HistoryModel::PrimaryActionRole)=="Copy link"?Ui::scaled(16)+space(Space::s):0));}
 bool compactRow(const QRect& r,const QModelIndex& i)const{return !grid()&&rowBody(r,false,heading(i)).width()<Ui::scaled(480);}
 bool compactRows()const{return !grid()&&rowBody(QRect(0,0,qMax(160,view_->viewport()->width()-2*view_->spacing()),rowContentHeight()),false,0).width()<Ui::scaled(480);}
 // Compact copy actions are icons (name in the tooltip); recovery stays labelled.
 bool compactIcon(const QModelIndex& i)const{const auto action=i.data(HistoryModel::PrimaryActionRole).toString();return !i.data(HistoryModel::FailedRole).toBool()&&(action=="Copy link"||action=="Copy result");}
 bool iconPrimary(const QRect& r,const QModelIndex& i)const{Q_UNUSED(r);return compactIcon(i);}
 // Independent of hover and selection, so nothing reflows on interaction.
 int compactActionWidth(const QModelIndex& i)const{
  const bool failed=i.data(HistoryModel::FailedRole).toBool();if(!failed&&inspectorBeside&&inspectorBeside())return 0;
  if(i.data(HistoryModel::PrimaryActionRole).toString().isEmpty())return space(Space::s)+actionHeight();
  return space(Space::s)+actionHeight()+(compactIcon(i)?space(Space::xs)+actionHeight():space(Space::s)+primaryWidth(i));
 }
 int compactMetadataTop(const QRect& r,const QModelIndex& i)const{return rowBody(r,false,heading(i)).top()+compactTextInset()+QFontMetrics(historyTitleFont()).height()+compactMetadataGap();}
 int compactErrorTop(const QRect& r,const QModelIndex& i)const{return compactMetadataTop(r,i)+QFontMetrics(Ui::font(Ui::Type::technical)).height()+compactDetailGap();}
 // A compact failure's facts flow as one block, never shortened while there is room:
 // the status line takes the facts that fit before its time, the rest share the detail
 // line with Retry and ⋯, and the actions take their own line only when that can't fit.
 int compactStatusFacts(const QRect& r,const QModelIndex& i)const{
  const auto record=i.data(HistoryModel::RecordRole).toJsonObject();const auto facts=failureDetail(record).split(" · ",Qt::SkipEmptyParts);const auto created=i.data(HistoryModel::CreatedRole).toString();const int width=rowBody(r,false,heading(i)).width();
  for(int count=facts.size();count>0;--count){QStringList expected{HistoryModel::failureLabel(record)};expected+=facts.mid(0,count);if(!created.isEmpty())expected.append(created);if(metadataFor(i,width,false,false,true,false,count)->toPlainText()==expected.join(" · "))return count;}
  return 0;
 }
 bool compactFailureActionsBelow(const QRect& r,const QModelIndex& i)const{
  if(!compactRow(r,i)||!i.data(HistoryModel::FailedRole).toBool())return false;const auto rest=failureDetail(i.data(HistoryModel::RecordRole).toJsonObject()).split(" · ",Qt::SkipEmptyParts).mid(compactStatusFacts(r,i)).join(" · ");
  return !rest.isEmpty()&&QFontMetrics(Ui::font(Ui::Type::technical)).horizontalAdvance(rest)>rowBody(r,false,heading(i)).width()-compactActionWidth(i);
 }
 int compactDetailHeight(const QRect& r,const QModelIndex& i)const{
  const int meta=QFontMetrics(Ui::font(Ui::Type::technical)).height();if(!compactFailureActionsBelow(r,i))return meta;
  QTextDocument detail;compactFailureDetail(detail,i,rowBody(r,false,heading(i)).width(),compactStatusFacts(r,i),true);return qMax(meta,int(std::ceil(detail.size().height())));
 }
 int inlineTextWidth(const QRect& r,const QModelIndex& i)const{
  const auto body=rowBody(r,false,heading(i));if(wide(r,i))return qMax(20,rowColumns(body,i).item.width());return qMax(20,(i.data(HistoryModel::FailedRole).toBool()?body.width()-actionHeight()-space(Space::s)-listDetailsSpace()-(rowPrimary(i)?primaryWidth(i)+space(Space::s):0):rowColumns(body,i).item.width())-space(Space::s));
 }
 bool actionsBelow(const QRect& r,const QModelIndex& i)const{
  if(compactRow(r,i))return !i.data(HistoryModel::FailedRole).toBool()&&rowBody(r,false,heading(i)).width()-compactActionWidth(i)<QFontMetrics(Ui::font(Ui::Type::technical)).horizontalAdvance(i.data(HistoryModel::CreatedRole).toString())+space(Space::xxl);
  if(!grid())return false;
  const auto body=rowBody(r,false,heading(i));const auto metadata=metadataFor(i,inlineTextWidth(r,i),!rowColumns(body,i).created.isEmpty());
  return std::ceil(metadata->size().height())>rowContentHeight()-4-2*rowTextInset()-QFontMetrics(historyTitleFont()).height()-6;
 }
 QRect primaryRect(QRect r,const QModelIndex& i)const{
  const auto c=card(r,i);const int h=actionHeight(),detailsSpace=detailsShown(i)?listDetailsSpace():0;const int w=qMin(primaryWidth(i),qMax(0,c.width()-2*space(Space::m)-h-space(Space::s)-(grid()?detailsWidth()+space(Space::s):detailsSpace)));
  if(grid())return galleryActionRect(r,i,0);
  if(wide(r,i)){const auto columns=rowColumns(rowBody(r,false,heading(i)),i);return slotPrimary(i)?QRect(columns.actions.left(),actionTop(r,i),h,h):QRect(columns.primary.left(),actionTop(r,i),columns.primary.width(),h);}
  // Compact failures keep their title and status line whole: actions share the detail line when it fits beside them.
  if(compactRow(r,i)&&i.data(HistoryModel::FailedRole).toBool()){const int meta=QFontMetrics(Ui::font(Ui::Type::technical)).height();return QRect(c.right()-space(Space::s)-h-space(Space::s)-w,compactErrorTop(r,i)+(compactFailureActionsBelow(r,i)?compactDetailHeight(r,i)+space(Space::xs):(meta-h)/2),w,h);}
  if(compactRow(r,i)){const bool icon=compactIcon(i);const int width=icon?h:w;return QRect(c.right()-space(Space::s)-h-(icon?space(Space::xs):space(Space::s))-width,actionsBelow(r,i)?c.bottom()-h-space(Space::s):c.top()+(c.height()-h)/2,width,h);}
  if(i.data(HistoryModel::FailedRole).toBool())return QRect(c.right()-space(Space::s)-h-space(Space::s)-detailsSpace-w,c.top()+(c.height()-h)/2,w,h);
  return QRect(c.right()-space(Space::s)-h-space(Space::s)-detailsSpace-w,actionsBelow(r,i)?c.bottom()-h-space(Space::s):c.top()+(rowContentHeight()-4-h)/2,w,h);
 }
 QRect menuRect(QRect r,const QModelIndex& i)const{if(grid())return galleryActionRect(r,i,2);if(wide(r,i)){const auto columns=rowColumns(rowBody(r,false,heading(i)),i);return QRect(columns.actions.right()-actionHeight()+1,actionTop(r,i),actionHeight(),actionHeight());}
 return compactMenuRect(r,i);}
 QRect timeRect(QRect r,const QModelIndex& i)const{const auto columns=rowColumns(rowBody(r,false,heading(i)),i);return QRect(columns.created.left(),actionTop(r,i),columns.created.width(),actionHeight());}
 QRect compactMenuRect(QRect r,const QModelIndex& i)const{const auto b=primaryRect(r,i);const auto c=card(r,i);const int h=actionHeight();return QRect(c.right()-space(Space::s)-h,b.top(),h,h);}
 static QString errorText(const QModelIndex& i){return HistoryModel::statusText(i.data(HistoryModel::RecordRole).toJsonObject());}
 QSize sizeHint(const QStyleOptionViewItem&,const QModelIndex& i)const override{
  if(grid()){
   const int width=galleryWidth();return {width,galleryRow(i,width).height};
  }
  const int width=qMax(160,view_->viewport()->width()-2*view_->spacing());
  int height=rowContentHeight()+heading(i);const auto outer=QRect(0,0,width,height);
  if(compactRow(outer,i)){
   const bool below=actionsBelow(outer,i),failed=i.data(HistoryModel::FailedRole).toBool();const int metaHeight=QFontMetrics(Ui::font(Ui::Type::technical)).height();int bottom=compactMetadataTop(outer,i)+metaHeight+(below?0:qMax(0,(actionHeight()-metaHeight)/2));
   // Room for the actions centred on the detail line, inset inside the card.
   if(failed)return {width,qMax(rowContentHeight()+metaHeight+space(Space::xs)+heading(i),compactErrorTop(outer,i)+(compactFailureActionsBelow(outer,i)?compactDetailHeight(outer,i)+space(Space::xs)+actionHeight():(metaHeight+actionHeight()+1)/2)+space(Space::xs)+3)};
   height=qMax(rowContentHeight(),bottom-heading(i)+2+(below?actionHeight()+space(Space::m):0))+heading(i);return {width,height};
  }
  if(i.data(HistoryModel::FailedRole).toBool()){const auto body=rowBody(outer,false,heading(i));const auto metadata=metadataFor(i,inlineTextWidth(outer,i),false);height=qMax(rowContentHeight(),rowTextInset()+QFontMetrics(historyTitleFont()).height()+6+int(std::ceil(metadata->size().height()))+rowTextInset()+4)+heading(i);}
  if(actionsBelow(outer,i)){
   const auto body=rowBody(outer,false,heading(i));const auto metadata=metadataFor(i,body.width(),false);
   const int text=rowTextInset()+qMax(8,rowTextInset())+4+QFontMetrics(historyTitleFont()).height()+6+int(std::ceil(metadata->size().height()));
   height=qMax(rowContentHeight(),text)+heading(i)+actionHeight()+12;
  }
  return {width,height};
 }
 void paint(QPainter* p,const QStyleOptionViewItem& option,const QModelIndex& i)const override{
  const auto& t=Theme::current();p->save();p->setRenderHint(QPainter::Antialiasing);const auto outer=option.rect;const auto group=i.data(HistoryModel::DayRole).toString();
  if(const int top=heading(i)){
   // A day after the first starts below a hairline; the heading aligns with the thumbnails and the page heading.
   if(i.row()>0){p->save();p->setRenderHint(QPainter::Antialiasing,false);p->setPen(t.border);p->drawLine(outer.left()+space(Space::s),outer.top()+space(Space::m),outer.right()-space(Space::s),outer.top()+space(Space::m));p->restore();}
   paintDayHeading(*p,QRect(outer.left()+2*space(Space::s),outer.top()+top-space(Space::xs)-dayHeadingLine(),outer.width()-4*space(Space::s),dayHeadingLine()),group,i.data(HistoryModel::DayCountRole).toString());
  }
  const auto r=card(outer,i);const bool chosen=option.state&QStyle::State_Selected;const bool hover=option.state&QStyle::State_MouseOver;const bool failed=i.data(HistoryModel::FailedRole).toBool();
  const auto record=i.data(HistoryModel::RecordRole).toJsonObject();
  // Selection is neutral; keyboard focus has its own amber outline.
  if(chosen)Ui::paintSelection(*p,r);
  else if(grid()){p->setPen(Qt::NoPen);p->setBrush(hover?t.surfaceRaised:t.surface);p->drawRoundedRect(QRectF(r).adjusted(0.5,0.5,-0.5,-0.5),Ui::Metric::containerRadius,Ui::Metric::containerRadius);}
  else{p->setPen(Qt::NoPen);p->setBrush(hover?t.surface:t.bg);p->drawRoundedRect(QRectF(r),8,8);}
  // The keyboard-current row carries the sidebar's accent bar, in single and multiple selection.
  if(chosen&&!grid()&&i==view_->currentIndex())p->fillRect(QRectF(r.left(),r.top()+space(Space::s),2,qMax(0,r.height()-2*space(Space::s))),t.accent);

  const int rowTop=r.top();const int rowHeight=grid()?r.height():rowContentHeight()-4;
  QRect thumb=grid()?QRect(r.left()+1,r.top()+1,r.width()-2,galleryRow(i,outer.width()).picture):QRect(r.left()+space(Space::s),rowTop+(rowHeight-historyThumbHeight)/2,historyThumbWidth,historyThumbHeight);
  // Dark pictures need an edge against the dark page: a hairline ring just outside the picture.
  if(!grid()&&Theme::scheme()==Theme::Scheme::Dark){p->setPen(Qt::NoPen);p->setBrush(t.border);p->drawRoundedRect(QRectF(thumb).adjusted(-1,-1,1,1),5,5);}
  p->setPen(Qt::NoPen);p->setBrush(t.well);p->drawRoundedRect(thumb,4,4);
  const auto image=qvariant_cast<QImage>(i.data(HistoryModel::ThumbnailRole));
  p->save();QPainterPath thumbnailClip;thumbnailClip.addRoundedRect(thumb,4,4);p->setClipPath(thumbnailClip,Qt::IntersectClip);
  if(!image.isNull()){const auto target=grid()?image.size()*qMin(1.0,qMax(double(thumb.width())/image.width(),double(thumb.height())/image.height())):thumb.size();const auto scaled=scaledThumbnail(image,target);const QPoint origin(thumb.center().x()-scaled.width()/2,thumb.center().y()-scaled.height()/2);p->drawImage(origin,scaled);if(video(record))paintVideoBadge(*p,grid()?thumb:QRect(origin,scaled.size()),HistoryModel::duration(image.text(HistoryModel::durationKey).toLongLong()));}
  else{
   // No preview: an honest file-type glyph, never a fabricated picture.
   const auto category=record["category"].toString();const int side=grid()?36:24;
   Ui::icon(category=="images"?"images":category=="videos"?"record":category=="text"?"clipboard":"document").paint(p,QRect(thumb.center()-QPoint(side/2,side/2),QSize(side,side)));
  }
  p->restore();

  // The primary action uses its own measured width; hover icons keep a fixed slot.
  const auto action=i.data(HistoryModel::PrimaryActionRole).toString();const bool showPrimary=primaryShown(i);const bool routineActionsShown=inlineActions(i)&&(hover||chosen||(grid()&&(i==view_->currentIndex()||i==actionIndex_))||((option.state&QStyle::State_HasFocus)&&view_->hasFocus()));const bool actionsShown=routineActionsShown||(inlineActions(i)&&(recovery(i)||(grid()&&failed)));
  const auto primaryBounds=primaryRect(outer,i);const auto moreBounds=menuRect(outer,i);
  const QRect body=grid()?galleryBody(outer,i):rowBody(outer,false,heading(i));
  const auto columns=grid()?Columns{body,{},{},{}}:rowColumns(body,i);
  const bool below=actionsBelow(outer,i),compact=compactRow(outer,i);
  int itemWidth=qMax(0,columns.item.width()-12);
  if(compact)itemWidth=qMax(0,body.width()-(below||failed?0:compactActionWidth(i)));
  else if(!grid()&&!below)itemWidth=inlineTextWidth(outer,i);
  else if(grid()||below)itemWidth=body.width();
  const auto titleFont=historyTitleFont(!grid()&&chosen),metaFont=Ui::font(Ui::Type::technical);
  const int lineTop=grid()?body.top():rowTop+(compact?compactTextInset():rowTextInset());
  if(!grid())Ui::paintText(*p,QRectF(columns.item.left(),lineTop,itemWidth,QFontMetrics(titleFont).height()),i.data().toString(),t.text,titleFont,Qt::AlignLeft|Qt::AlignVCenter,compact?Qt::ElideRight:titleElision(i.data().toString()));
  const int metadataWidth=compact?qMax(20,body.width()-(below||failed?0:compactActionWidth(i))):!grid()&&failed?inlineTextWidth(outer,i):itemWidth;
  const int metadataTop=compact?compactMetadataTop(outer,i):lineTop+QFontMetrics(titleFont).height()+(grid()?space(Space::xs):6);
  const QRectF metadata(columns.item.left(),metadataTop,metadataWidth,QFontMetrics(metaFont).height());
  if(compact){
   const int statusFacts=failed?compactStatusFacts(outer,i):0;const auto document=metadataFor(i,metadataWidth,false,false,true,false,statusFacts);p->save();p->translate(metadata.topLeft());p->setClipRect(QRectF(0,0,metadataWidth,QFontMetrics(metaFont).height()));document->drawContents(p);p->restore();
   if(failed){const bool own=compactFailureActionsBelow(outer,i);QTextDocument error;compactFailureDetail(error,i,body.width()-(own?0:compactActionWidth(i)),statusFacts,own);p->save();p->translate(body.left(),compactErrorTop(outer,i));p->setClipRect(QRectF(0,0,body.width(),own?compactDetailHeight(outer,i):QFontMetrics(metaFont).height()));error.drawContents(p);p->restore();}
  }
  else if(!grid()){const auto document=metadataFor(i,metadataWidth,false,true);p->save();p->translate(metadata.topLeft());const int available=below?r.bottom()-actionHeight()-12-metadata.top()+1:r.bottom()-metadata.top()+1;p->setClipRect(QRectF(0,0,metadataWidth,available));document->drawContents(p);p->restore();}
  else{
   // The name, then the detail line at the caption's foot with the time at its right.
   const auto caption=galleryCaption(i,body.width());const QFontMetrics titleMetrics(titleFont);int y=body.top();
   for(const auto& line:caption.title){Ui::paintText(*p,QRectF(body.left(),y,body.width(),titleMetrics.height()),line,t.text,titleFont,Qt::AlignLeft|Qt::AlignVCenter,Qt::ElideNone);y+=titleMetrics.height();}
   const int metaHeight=QFontMetrics(metaFont).height();
   if(caption.detail){const int top=body.top()+body.height()-metaHeight;p->save();p->translate(body.left(),top);p->setClipRect(QRectF(0,0,body.width(),metaHeight));caption.detail->drawContents(p);p->restore();Ui::paintText(*p,QRectF(body.left(),top,body.width(),metaHeight),i.data(HistoryModel::CreatedRole).toString(),t.textSecondary,metaFont,Qt::AlignRight|Qt::AlignVCenter);}
   else if(caption.failure){const int top=body.top()+titleMetrics.height()+space(Space::xs);caption.failure->draw(p,QPointF(body.left(),top));const auto last=caption.failure->lineAt(caption.failure->lineCount()-1);const int lineHeight=QFontMetrics(Ui::font(Ui::Type::body)).height();
    if(caption.time)Ui::paintText(*p,QRectF(body.left(),top+(caption.failure->lineCount()-1)*lineHeight,body.width(),lineHeight),i.data(HistoryModel::CreatedRole).toString(),t.textSecondary,metaFont,Qt::AlignRight|Qt::AlignVCenter);Q_UNUSED(last);}
  }
  if(!grid()){
   // The time column: right-aligned, apart from the picture so it never reads as a duration.
   if(!compact)Ui::paintText(*p,QRectF(timeRect(outer,i)),i.data(HistoryModel::CreatedRole).toString(),t.textSecondary,Ui::font(Ui::Type::technical),Qt::AlignRight|Qt::AlignVCenter);

  }
  if(actionsShown){
   auto* style=view_->style();
   // Actions on the picture keep a backdrop that stays readable over any image.
   if(paintQuiet_->property("uiOnMedia").toBool()!=grid())paintQuiet_->setProperty("uiOnMedia",grid());
   if(grid()){p->save();p->setPen(Qt::NoPen);p->setBrush(Theme::mediaScrim());for(const auto& backdrop:{moreBounds,showPrimary&&slotPrimary(i)?primaryBounds:QRect()})if(!backdrop.isEmpty())p->drawRoundedRect(QRectF(backdrop),Ui::Metric::compactRadius,Ui::Metric::compactRadius);p->restore();}
   if(showPrimary&&(!grid()||i!=actionIndex_||!primary_->isVisible())){QStyleOptionButton button;button.rect=primaryBounds;const bool icon=iconPrimary(outer,i);button.text=icon?QString{}:action;if(action=="Copy link"||icon){button.icon=Ui::icon("copy");button.iconSize=QSize(Ui::scaled(16),Ui::scaled(16));}button.state=QStyle::State_Enabled|(option.state&QStyle::State_MouseOver&&primaryBounds.contains(view_->viewport()->mapFromGlobal(QCursor::pos()))?QStyle::State_MouseOver:QStyle::State_None);if(pressed_==i&&pressedAction_==1)button.state|=QStyle::State_Sunken;button.fontMetrics=QFontMetrics(Ui::font());p->save();p->setFont(Ui::font());style->drawControl(QStyle::CE_PushButton,&button,p,(grid()&&!recovery(i))||(!grid()&&iconPrimary(outer,i))?paintQuiet_:paintSecondary_);p->restore();}
   if(!grid()||actionsShown){
    if(grid()&&detailsShown(i)&&(i!=view_->currentIndex()||!details_->isVisible())){QStyleOptionButton button;button.rect=detailsRect(outer,i);button.text="Details";button.state=QStyle::State_Enabled;style->drawControl(QStyle::CE_PushButton,&button,p,details_);}
    if(!grid()||i!=actionIndex_||!more_->isVisible()){QStyleOptionButton more;more.rect=moreBounds;more.state=QStyle::State_Enabled|(moreBounds.contains(view_->viewport()->mapFromGlobal(QCursor::pos()))?QStyle::State_MouseOver:QStyle::State_None);style->drawPrimitive(QStyle::PE_PanelButtonCommand,&more,p,more_);const int side=Ui::scaled(Ui::Metric::icon);Ui::paintChromeGlyph(*p,QRectF(moreBounds.center().x()-side/2.0,moreBounds.center().y()-side/2.0,side,side),"more",grid()?Theme::onMedia():t.textSecondary);}
   }
  }
  if(i.data(HistoryModel::ActiveRole).toBool()){const int progress=i.data(HistoryModel::ProgressRole).toInt();if(progress>=0){QRectF bar(grid()?galleryPictureRect(outer,i).left():columns.item.left(),grid()?galleryPictureRect(outer,i).bottom()-2:r.bottom()-(below?actionHeight()+12:0)-8,qMax(20,itemWidth),3);p->setPen(Qt::NoPen);p->setBrush(t.surfaceRaised);p->drawRoundedRect(bar,1.5,1.5);bar.setWidth(bar.width()*progress/100);p->setBrush(t.text);p->drawRoundedRect(bar,1.5,1.5);}}
  if((option.state&QStyle::State_HasFocus)&&view_->hasFocus()&&view_->window()->testAttribute(Qt::WA_KeyboardFocusChange)){p->setPen(QPen(t.accent,1));p->setBrush(Qt::NoBrush);p->drawRoundedRect(QRectF(r).adjusted(0.5,0.5,-0.5,-0.5),8,8);}
  p->restore();
 }
 bool editorEvent(QEvent* event,QAbstractItemModel*,const QStyleOptionViewItem& option,const QModelIndex& i)override{
  if(event->type()!=QEvent::MouseButtonPress&&event->type()!=QEvent::MouseButtonRelease)return false;
  auto* mouse=static_cast<QMouseEvent*>(event);if(mouse->button()!=Qt::LeftButton)return false;
  const auto pos=mouse->position().toPoint();const int target=!inlineActions(i)?0:primaryShown(i)&&primaryRect(option.rect,i).contains(pos)?1:detailsShown(i)&&detailsRect(option.rect,i).contains(pos)?2:menuRect(option.rect,i).contains(pos)?3:0;
  if(event->type()==QEvent::MouseButtonPress){cancelPress();if(target){pressed_=i;pressedAction_=target;pressPosition_=pos;view_->viewport()->update();}return target!=0;}
  const bool activate=target&&pressed_==i&&pressedAction_==target&&(pos-pressPosition_).manhattanLength()<=QApplication::startDragDistance();cancelPress();if(!activate)return false;
  if(target==1&&primary)primary(i);else if(target==2&&details)details(i);else if(target==3&&menu)menu(i,mouse->globalPosition().toPoint());return true;
 }
private:
 void cancelPress(){pressed_=QPersistentModelIndex{};pressedAction_=0;view_->viewport()->update();}
 mutable QHash<std::pair<int,int>,std::pair<QString,GalleryCaption>> captions_;
 struct Metadata {QString status;QFont font;std::shared_ptr<QTextDocument> document;};
 mutable QHash<quint64,Metadata> metadata_;
 // Scaled previews for the cards on screen, in KiB; dropped while the list is hidden.
 mutable QCache<std::pair<qint64,std::pair<std::pair<int,int>,int>>,QImage> thumbnails_{6144};
 mutable QHash<std::pair<std::pair<int,int>,std::pair<int,int>>,GalleryRow> passRows_;mutable QHash<QString,int> passPrimaryWidths_;mutable bool passQueued_=false;
 QListView* view_;QToolButton *primary_,*details_,*more_,*paintSecondary_,*paintQuiet_;QPersistentModelIndex pressed_,hovered_,actionIndex_;bool menuOpen_=false;int pressedAction_=0;QPoint pressPosition_;
};
}
namespace {
// The History list and gallery. The day at the top of the viewport keeps its
// heading pinned over the rows scrolling under it: painted after the rows,
// taking no clicks, and keyboard movement never leaves the current row under it.
class HistoryList final:public QListView {
public:
 explicit HistoryList(QWidget* parent):QListView(parent){
  connect(verticalScrollBar(),&QScrollBar::valueChanged,this,[this](int value){const int moved=qAbs(value-lastScroll_);lastScroll_=value;
   // Scrolling moves the pixels already drawn: repaint where the pinned heading was and is.
   if(pinnedShown_||viewMode()==IconMode)viewport()->update(QRect(0,0,viewport()->width(),pinnedHeadingHeight()+moved+2));});
 }
 struct Pinned {bool shown=false;QString title,count;int top=0;};
 // Gallery day sections: each day starts a new row of tiles under its heading.
 // QListView places icon-mode items itself only with free movement; the flow
 // still sizes them, then each tile moves to its section's grid (doItemsLayout).
 std::function<int()> columns;
 std::pair<int,int> run(int row)const{return row>=0&&row<int(runs_.size())?runs_.at(row):std::pair<int,int>{row,row+1};}
 void doItemsLayout()override{
  const bool gallery=viewMode()==IconMode&&model();if(gallery)computeRuns();else{runs_.clear();headings_.clear();}
  QListView::doItemsLayout();if(gallery)placeTiles();
 }
 // The section whose heading has scrolled above the viewport's top edge.
 Pinned pinned()const{
  Pinned result;if(!model()||!model()->rowCount())return result;
  if(viewMode()==IconMode){
   const int offset=verticalOffset();int current=-1;for(int i=0;i<headings_.size();++i)if(headings_[i].line.top()-offset<0)current=i;
   if(current<0)return result;const auto first=model()->index(headings_[current].row,0);result.shown=true;result.title=first.data(HistoryModel::SectionRole).toString();result.count=first.data(HistoryModel::DayCountRole).toString();
   if(current+1<headings_.size()){const int arriving=headings_[current+1].hairline-offset;result.top=qMin(0,arriving-pinnedHeadingHeight());}
   return result;
  }
  QModelIndex top;for(int y=0;y<qMin(viewport()->height(),pinnedHeadingHeight()*2)&&!top.isValid();y+=4)top=QListView::indexAt(QPoint(viewport()->width()/2,y));
  if(!top.isValid())return result;const auto title=top.data(HistoryModel::SectionRole).toString();if(title.isEmpty())return result;
  int start=top.row();while(start>0&&model()->index(start-1,0).data(HistoryModel::SectionRole).toString()==title)--start;
  const auto first=visualRect(model()->index(start,0));const int ownHeading=first.top()+dayHeadingHeight(start==0)-space(Space::xs)-dayHeadingLine();
  if(ownHeading>=0)return result;
  result.shown=true;result.title=title;result.count=top.data(HistoryModel::DayCountRole).toString();
  // The next day's heading pushes the pinned one up as it arrives.
  for(int row=top.row()+1;row<model()->rowCount();++row){const auto next=model()->index(row,0);if(next.data(HistoryModel::DayRole).toString().isEmpty())continue;const int arriving=visualRect(next).top()+space(Space::m);result.top=qMin(0,arriving-pinnedHeadingHeight());break;}
  return result;
 }
 // The pinned heading covers the top of the viewport: nothing under it is hit,
 // hovered or pressed through it.
 int coveredHeight()const{const auto pin=pinned();return pin.shown?pin.top+pinnedHeadingHeight():0;}
 QModelIndex indexAt(const QPoint& point)const override{return point.y()<coveredHeight()?QModelIndex{}:QListView::indexAt(point);}
 void scrollTo(const QModelIndex& index,ScrollHint hint=EnsureVisible)override{
  QListView::scrollTo(index,hint);
  if(!index.isValid()||(viewMode()==ListMode&&!index.data(HistoryModel::DayRole).toString().isEmpty()))return;
  const auto pin=pinned();const int covered=pin.shown?pin.top+pinnedHeadingHeight():0;const auto rect=visualRect(index);
  if(pin.shown&&rect.top()<covered)verticalScrollBar()->setValue(verticalScrollBar()->value()-(covered-rect.top()));
 }
protected:
 // A press on the pinned heading stays there: it neither selects the row under it nor clears the selection.
 void mousePressEvent(QMouseEvent* event)override{if((swallowed_=event->position().toPoint().y()<coveredHeight())){event->accept();return;}QListView::mousePressEvent(event);}
 void mouseDoubleClickEvent(QMouseEvent* event)override{if((swallowed_=event->position().toPoint().y()<coveredHeight())){event->accept();return;}QListView::mouseDoubleClickEvent(event);}
 void mouseMoveEvent(QMouseEvent* event)override{if(swallowed_&&event->buttons()!=Qt::NoButton){event->accept();return;}QListView::mouseMoveEvent(event);}
 void mouseReleaseEvent(QMouseEvent* event)override{if(std::exchange(swallowed_,false)){event->accept();return;}QListView::mouseReleaseEvent(event);}
 void paintEvent(QPaintEvent* event)override{
  QListView::paintEvent(event);QPainter painter(viewport());const auto& t=Theme::current();
  if(viewMode()==IconMode)for(const auto& heading:headings_){const int offset=verticalOffset();if(!event->rect().intersects(QRect(0,heading.hairline-offset,viewport()->width(),heading.line.bottom()-heading.hairline+1)))continue;
   if(!heading.first)painter.fillRect(QRect(space(Space::s),heading.hairline-offset,qMax(0,viewport()->width()-2*space(Space::s)),1),t.border);const auto first=model()->index(heading.row,0);paintDayHeading(painter,heading.line.translated(0,-offset),first.data(HistoryModel::DayRole).toString(),first.data(HistoryModel::DayCountRole).toString());}
  const auto pin=pinned();pinnedShown_=pin.shown;if(!pin.shown)return;const int height=pinnedHeadingHeight();const QRect band(0,pin.top,viewport()->width(),height);
  painter.fillRect(band,t.bg);painter.fillRect(QRect(space(Space::s),band.bottom(),qMax(0,viewport()->width()-2*space(Space::s)),1),t.border);
  const int left=viewMode()==IconMode?space(Space::s):2*space(Space::s);paintDayHeading(painter,QRect(left,pin.top+space(Space::s),viewport()->width()-left-2*space(Space::s),dayHeadingLine()),pin.title,pin.count);
 }
private:
 bool swallowed_=false;
 void computeRuns(){
  const int count=model()->rowCount(),width=qMax(1,columns?columns():1);runs_.assign(count,{0,0});int start=0;
  for(int row=0;row<=count;++row){const bool section=row<count&&row>0&&!model()->index(row,0).data(HistoryModel::DayRole).toString().isEmpty();if(row==count||section||row-start==width){for(int i=start;i<row;++i)runs_[i]={start,row};start=row;}}
 }
 void placeTiles(){
  headings_.clear();const int count=model()->rowCount();int y=spacing(),row=0;
  while(row<count){const auto [start,end]=runs_.at(row);const auto first=model()->index(start,0);const auto title=first.data(HistoryModel::DayRole).toString();
   if(!title.isEmpty()){Heading heading;heading.first=start==0;heading.hairline=y+(heading.first?0:space(Space::m));y+=heading.first?space(Space::s):space(Space::m)+1+space(Space::l);heading.line=QRect(space(Space::s),y,qMax(0,viewport()->width()-2*space(Space::s)),dayHeadingLine());heading.row=start;heading.title=title;headings_.append(heading);y+=dayHeadingLine()+space(Space::xs);}
   int height=0,x=spacing();for(int i=start;i<end;++i){const auto index=model()->index(i,0);const auto size=rectForIndex(index).size();setPositionForIndex(QPoint(x,y),index);x+=size.width()+spacing();height=qMax(height,size.height());}
   y+=height+spacing();row=qMax(end,row+1);}
 }
 struct Heading {QRect line;int hairline=0,row=0;QString title;bool first=false;};
 std::vector<std::pair<int,int>> runs_;QList<Heading> headings_;
 int lastScroll_=0;bool pinnedShown_=false;
};
}
HistoryWindow::HistoryWindow(UiSession& session,QWidget* parent):RetainedWindow(parent),session_(session){
 chooser_=new FileDialog(this);
 setObjectName("historyPage");setWindowTitle("put.gd — History");resize(1080,760);auto* page=new QHBoxLayout(this);page->setContentsMargins(0,0,0,0);page->setSpacing(0);
 // Sidebar: categories and failed uploads.
 auto* sidebar=new Ui::Surface("surface",Qt::RightEdge,this);sidebar->setObjectName("historySidebar");sidebar->setFixedWidth(Ui::railWidth());page->addWidget(sidebar);
 auto* side=new QVBoxLayout(sidebar);side->setContentsMargins(space(Space::m),space(Space::l),space(Space::m),space(Space::l));side->setSpacing(space(Space::s));
 auto* navigationScroll=new QScrollArea(sidebar);navigationScroll->setObjectName("historyNavigationScroll");navigationScroll->setFrameShape(QFrame::NoFrame);navigationScroll->setWidgetResizable(true);navigationScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);Ui::transparentView(navigationScroll);side->addWidget(navigationScroll,1);auto* navigation=new Ui::Surface("surface",{},navigationScroll);navigation->setObjectName("historyNavigation");auto* navigationBox=new QVBoxLayout(navigation);navigationBox->setContentsMargins(0,0,0,0);navigationBox->setSpacing(space(Space::s));navigationScroll->setWidget(navigation);
 categories_=new QListWidget(sidebar);categories_->setObjectName("historyCategories");categories_->setAccessibleName("Show history items");categories_->setFrameShape(QFrame::NoFrame);categories_->setIconSize(QSize(18,18));categories_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);categories_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);categories_->setItemDelegate(new HistoryCategoryDelegate(categories_));Ui::transparentView(categories_);categories_->setMouseTracking(true);
 for(const auto& [glyph,label,category]:QList<std::tuple<QString,QString,QString>>{{"history","All items",""},{"images","Images","images"},{"record","Recordings","videos"},{"document","Other files","text+other"}}){auto* item=new QListWidgetItem(Ui::icon(glyph),label,categories_);item->setData(Ui::NavigationDelegate::WrapLabelRole,true);item->setData(Qt::UserRole,category);}
 categories_->setToolTip("Other files includes clipboard text and any file that is not an image or a recording.");
 categories_->setCurrentRow(0);
 // Keep every category on one line at the current text size.

 {auto* separator=new QListWidgetItem(categories_);separator->setFlags(Qt::NoItemFlags);separator->setSizeHint(QSize(0,9));auto* failed=new QListWidgetItem(Ui::icon("failed-uploads"),"Failed uploads",categories_);failed->setData(Ui::NavigationDelegate::WrapLabelRole,true);failed->setData(Qt::UserRole,"failed");}
 categories_->setFixedHeight(5*Ui::scaled(Ui::Metric::navRow)+13);navigationBox->addWidget(categories_);

 navigationBox->addStretch();
 auto* content=new QWidget(this);content->setObjectName("historyContent");page->addWidget(content,1);auto* layout=new QVBoxLayout(content);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);
 auto* headerBar=new HistoryHeader(content);headerBar->setObjectName("historyHeader");auto* toolbar=new QWidget(headerBar);toolbar->setObjectName("historyCompactControls");toolbar->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);auto* headerBox=new QVBoxLayout(headerBar);headerBox->setContentsMargins(0,0,0,0);headerBox->addWidget(toolbar);auto* header=new QGridLayout(toolbar);header->setSizeConstraint(QLayout::SetNoConstraint);header->setContentsMargins(space(Space::xxl),Ui::pageHeaderTop(),space(Space::l),space(Space::s));header->setRowMinimumHeight(0,Ui::Metric::control);header->setSpacing(space(Space::s));header->setVerticalSpacing(space(Space::xs));// The page is already named by the navigation; the heading names what is shown.
 // Long combined headings ("Recordings · failed uploads") elide before they crowd search.
 // The compact layout opens the inspector over the list; this returns to it.
 detailsBack_=Ui::toolButton("Back to list",{},"quiet",this);detailsBack_->setObjectName("historyDetailsBack");detailsBack_->setProperty("uiFlush",true);detailsBack_->setIcon(Ui::chromeIcon("chevron-left"));detailsBack_->setIconSize(QSize(14,14));detailsBack_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);detailsBack_->setToolTip("Return to the list (Esc)");detailsBack_->hide();header->addWidget(detailsBack_,0,0);connect(detailsBack_,&QToolButton::clicked,this,[this]{setDetailsOpen(false);});
 {auto* back=new QAction(this);back->setObjectName("historyDetailsEscape");back->setShortcut(QKeySequence(Qt::Key_Escape));back->setShortcutContext(Qt::WidgetWithChildrenShortcut);back->setEnabled(false);addAction(back);connect(back,&QAction::triggered,this,[this]{for(auto* child:children())if(auto* editor=dynamic_cast<HistoryFilterEditor*>(child);editor&&editor->isVisible()){editor->close();return;}if(expandedPreview_&&expandedPreview_->isVisible())expandedPreview_->reject();else if(paneVisible())closeDetails();else if(table_->selectionModel()->selectedRows().size()>1){table_->clearSelection();table_->setFocus(Qt::OtherFocusReason);}});}
 auto* heading=new Ui::ElidedLabel(this);heading->setText("All items");heading->setObjectName("historyHeading");heading->setElideMode(Qt::ElideRight);heading->setTextFormat(Qt::PlainText);Ui::role(heading,"title");auto* headingGroup=new QWidget(toolbar);headingGroup->setObjectName("historyHeadingGroup");headingGroup->setFixedHeight(Ui::scaled(Ui::Metric::control));auto* headingBox=new QHBoxLayout(headingGroup);headingBox->setContentsMargins(0,0,0,0);headingBox->setSpacing(space(Space::s));headingBox->addWidget(heading,0,Qt::AlignVCenter);count_=new Ui::ElidedLabel(headingGroup);count_->setObjectName("historyCount");static_cast<Ui::ElidedLabel*>(count_)->setElideMode(Qt::ElideRight);Ui::role(count_,"bodySecondary");headingBox->addWidget(count_,1,Qt::AlignVCenter);headingBox->addStretch();header->addWidget(headingGroup,0,0);header->setColumnStretch(0,1);
 auto* search=new QLineEdit(this);search->setObjectName("historySearch");search->setAccessibleName("Search history by file name");search->setPlaceholderText("Search history");search->setMaxLength(256);search->setClearButtonEnabled(true);search->setMinimumWidth(180);search->setMaximumWidth(300);header->setHorizontalSpacing(space(Space::l));header->addWidget(search,0,1);
 // Filter opens the add-filter menu directly; active filters show as chips under the header.
 auto* advancedToggle=Ui::toolButton("Filter","filter","quiet",this);advancedToggle->setObjectName("historyFiltersExpanded");advancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);advancedToggle->setAccessibleName("Filter history");advancedToggle->setToolTip("Add a filter");advancedToggle->setPopupMode(QToolButton::InstantPopup);header->addWidget(advancedToggle,0,2);filtersToggle_=advancedToggle;
 // Restrictions stay visible while the filters are collapsed: "Filters · N" and the way back.
 clearFilters_=Ui::toolButton("Clear filters",{},"quiet",this);clearFilters_->setObjectName("historyClearFilters");clearFilters_->setToolTip("Clear the filters; search and the category stay");clearFilters_->hide();connect(clearFilters_,&QToolButton::clicked,this,[this]{clearFilters(false);});
 // List and Gallery: one segmented choice.
 // An icon segmented control, so it never reads as a second row of tabs.
 auto* listButton=new QToolButton(this);listButton->setObjectName("historyListMode");listButton->setIcon(Ui::icon("view-list"));listButton->setCheckable(true);listButton->setChecked(true);listButton->setAccessibleName("List view");listButton->setToolTip("List view");grid_=new QToolButton(this);grid_->setObjectName("historyGridMode");grid_->setIcon(Ui::icon("view-grid"));grid_->setCheckable(true);grid_->setAccessibleName("Gallery view");grid_->setToolTip("Gallery view");auto* views=new Ui::SegmentTrack(this);views->setObjectName("historyViewMode");views->setAccessibleName("History view");for(auto* b:{listButton,grid_}){b->setFocusPolicy(Qt::StrongFocus);b->setToolButtonStyle(Qt::ToolButtonIconOnly);b->setIconSize(QSize(Ui::scaled(Ui::Metric::icon),Ui::scaled(Ui::Metric::icon)));Ui::focusTooltips(b);views->addButton(b);}header->addWidget(views,0,4);
 // The details pane's switch: pressed while details are on.
 detailsToggle_=Ui::toolButton({},"panel-right","quiet",this);detailsToggle_->setObjectName("historyDetailsToggle");detailsToggle_->setCheckable(true);detailsToggle_->setChecked(true);detailsToggle_->setIconSize(QSize(Ui::scaled(Ui::Metric::icon),Ui::scaled(Ui::Metric::icon)));detailsToggle_->setFixedSize(Ui::scaled(Ui::Metric::control),Ui::scaled(Ui::Metric::control));Ui::focusTooltips(detailsToggle_);header->addWidget(detailsToggle_,0,5);connect(detailsToggle_,&QToolButton::clicked,this,[this]{toggleDetails();});
 layout->addWidget(headerBar);
 connect(listButton,&QToolButton::clicked,this,[this]{setDetailsOpen(false);setGridMode(false);});connect(grid_,&QToolButton::clicked,this,[this]{setDetailsOpen(false);setGridMode(true);});
 // Active filters as chips, aligned with the heading; at most two lines.
 filterChips_=new HistoryChipRow(headerBar);filterChips_->setObjectName("historyFilterChips");filterChips_->setContentsMargins(space(Space::xxl),0,space(Space::l),space(Space::s));filterChips_->setAccessibleName("Active filters");filterChips_->hide();
 auto* advanced=new QWidget(this);advanced->setObjectName("historyFiltersPanel");auto* advancedBox=new QVBoxLayout(advanced);advancedBox->setContentsMargins(space(Space::l),0,space(Space::l),space(Space::m));advancedBox->setSpacing(space(Space::s));auto* filters=new QGridLayout;filters->setContentsMargins(0,0,0,0);filters->setAlignment(Qt::AlignTop);filters->setHorizontalSpacing(space(Space::s));filters->setVerticalSpacing(space(Space::s));const QStringList keys{"from","to","mime","profileId","uploadStatus","localStatus","remoteStatus"};const QStringList labels{"From date","Through date","File type","Destination","Upload state","Local-copy state","Remote file state"};
 QList<QWidget*> filterCells;QMap<QString,QWidget*> filterWidgets;auto* technical=new QWidget(advanced);auto* techBox=new QVBoxLayout(technical);techBox->setContentsMargins(0,0,0,0);technical->hide();
 for(int i=0;i<keys.size();++i){const auto key=keys[i];auto* raw=new QLineEdit(advanced);raw->setMaxLength(256);raw->setObjectName("historyFilter"+key);raw->setAccessibleName(labels[i]);raw->hide();connect(raw,&QLineEdit::returnPressed,this,&HistoryWindow::applyFilters);
  auto* cell=new QWidget(advanced);auto* field=new QVBoxLayout(cell);cell->setMinimumWidth(Ui::scaled(160));field->setContentsMargins(0,0,0,0);field->setSpacing(space(Space::xs));field->setAlignment(Qt::AlignTop);auto* label=new QLabel(labels[i],cell);label->setWordWrap(true);Ui::role(label,"caption");label->setFixedHeight(qMax(Ui::scaled(20),label->fontMetrics().height()));field->addWidget(label);
  if(key=="from"||key=="to"){
   auto* line=new QHBoxLayout;line->setContentsMargins(0,0,0,0);line->setSpacing(space(Space::xs));auto* date=new HistoryDate(cell);date->setObjectName("historyDate"+key);date->setDisplayFormat(QLocale().dateFormat(QLocale::ShortFormat));date->setAccessibleName(labels[i]);date->setFixedHeight(qMax(Ui::scaled(32),date->fontMetrics().height()+space(Space::s)));label->setBuddy(date);line->addWidget(date,1);auto* clear=Ui::toolButton({}, {},"quiet",cell);clear->setObjectName("historyDateClear"+key);clear->setIcon(Ui::chromeIcon("close"));clear->setFixedSize(Ui::scaled(32),Ui::scaled(32));clear->setAccessibleName("Clear "+labels[i].toLower());clear->setToolTip(clear->accessibleName());clear->setEnabled(false);clear->hide();line->addWidget(clear);field->addLayout(line);
   auto dateValue=[key](QDate d){return QDateTime(d,key=="to"?QTime(23,59,59,999):QTime(0,0)).toUTC().toString(Qt::ISODateWithMs);};connect(clear,&QToolButton::clicked,raw,&QLineEdit::clear);connect(date,&QDateEdit::dateChanged,this,[raw,date,dateValue](QDate d){raw->setText(d==date->minimumDate()?QString{}:dateValue(d));});
   connect(raw,&QLineEdit::textChanged,this,[date,clear](const QString& value){const QSignalBlocker block(date);const auto parsed=value.contains('T')?QDateTime::fromString(value,Qt::ISODateWithMs).toLocalTime().date():QDate::fromString(value,Qt::ISODate);date->setDate(parsed.isValid()?parsed:date->minimumDate());clear->setEnabled(!value.isEmpty());clear->setVisible(!value.isEmpty());});
  }else if(key=="mime"){raw->setParent(cell);raw->show();raw->setPlaceholderText("Any type, e.g. image/png");label->setBuddy(raw);raw->setFixedHeight(qMax(Ui::scaled(32),raw->fontMetrics().height()+space(Space::s)));field->addWidget(raw);}
  else{
   auto* combo=new QComboBox(cell);combo->setObjectName("historyChoice"+key);combo->setAccessibleName(labels[i]);combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);combo->setMinimumContentsLength(10);combo->addItem("Any",QString{});label->setBuddy(combo);combo->setFixedHeight(qMax(Ui::scaled(32),combo->fontMetrics().height()+space(Space::s)));field->addWidget(combo);
   QList<std::pair<QString,QString>> choices;
   if(key=="uploadStatus")choices={{"uploaded","Uploaded"},{"local_only","Local only"},{"pending","Waiting to upload"},{"uploading","Uploading"},{"failed","Failed"},{"unknown","Outcome unknown"},{"cancelled","Canceled"},{"not_requested","Not requested"},{"skipped","Skipped"},{"conflict","Conflict"}};
   else if(key=="localStatus")choices={{"saved","Saved"},{"disabled","Not requested"},{"pending","Waiting to save"},{"failed","Save failed"}};
   else if(key=="remoteStatus")choices={{"not_requested","Deletion not requested"},{"present","Available"},{"pending","Waiting for deletion"},{"deleting","Deleting"},{"removed","Deleted"},{"already_missing","Already missing"},{"failed","Deletion failed"},{"unknown","Outcome unknown"},{"changed","Remote file changed"},{"superseded","Replaced by newer upload"},{"hidden_version_retained","Hidden; version retained"},{"version_retained","Version retained"},{"unsupported_safe_removal","Safe removal unavailable"}};
   for(const auto& [value,name]:choices)combo->addItem(name,value);
   if(key=="remoteStatus"){showCancelled_=new QCheckBox("Include canceled",technical);showCancelled_->setObjectName("historyShowCancelled");showCancelled_->setCheckable(true);showCancelled_->setAccessibleName("Include canceled");techBox->addWidget(showCancelled_);connect(showCancelled_,&QCheckBox::toggled,this,[this](bool show){if(model_){model_->setShowCancelled(show);updateFilterSummary();emit viewStateChanged();}});}
   if(key=="profileId"){
    auto refresh=[this,combo,raw]{const QSignalBlocker block(combo);const auto value=raw->text();combo->clear();combo->addItem("Any destination",QString{});for(const auto& v:session_.document()["profiles"].toArray()){const auto profile=v.toObject();combo->addItem(profile["label"].toString(),profile["id"].toString());}int index=combo->findData(value);if(index<0){combo->addItem("Unavailable destination",value);index=combo->count()-1;}combo->setCurrentIndex(index);};refresh();connect(&session_,&UiSession::accepted,this,refresh);

   }
   connect(combo,&QComboBox::currentIndexChanged,this,[combo,raw]{raw->setText(combo->currentData().toString());});connect(raw,&QLineEdit::textChanged,this,[combo,key](const QString& value){const QSignalBlocker block(combo);int index=combo->findData(value);if(index<0&&key=="remoteStatus"&&QStringList{"available","deleted","needs_attention"}.contains(value)){combo->setCurrentIndex(0);return;}if(index<0){combo->addItem(key=="profileId"?"Unavailable destination":"Other recorded state",value);index=combo->count()-1;}combo->setCurrentIndex(index);});
  }
  if(key=="remoteStatus"){
   filterCells.append(cell);filterWidgets["exactRemoteStatus"]=cell;label->setText("Exact remote state");auto* ordinary=new QWidget(advanced);auto* group=new QVBoxLayout(ordinary);ordinary->setMinimumWidth(Ui::scaled(160));group->setContentsMargins(0,0,0,0);group->setSpacing(space(Space::xs));auto* caption=new QLabel("Remote file state",ordinary);Ui::role(caption,"caption");caption->setFixedHeight(qMax(Ui::scaled(20),caption->fontMetrics().height()));group->addWidget(caption);auto* choice=new QComboBox(ordinary);choice->setObjectName("historyRemoteState");choice->setPlaceholderText("Technical filter");choice->setAccessibleName(caption->text());caption->setBuddy(choice);choice->setFixedHeight(qMax(Ui::scaled(32),choice->fontMetrics().height()+space(Space::s)));for(const auto& [value,name]:QList<std::pair<QString,QString>>{{"","Any"},{"available","Available"},{"deleted","Deleted"},{"needs_attention","Needs attention"}})choice->addItem(name,value);choice->setCurrentIndex(0);group->addWidget(choice);filterCells.append(ordinary);filterWidgets[key]=ordinary;ordinary->setProperty("filterKey",key);cell->setProperty("filterKey","exactRemoteStatus");connect(raw,&QLineEdit::textChanged,this,[this,cell,ordinary](const QString& value){const bool aggregate=QStringList{"available","deleted","needs_attention"}.contains(value);cell->setProperty("filterActive",!value.isEmpty()&&!aggregate);ordinary->setProperty("filterActive",aggregate);cell->setProperty("filterRevealed",false);ordinary->setProperty("filterRevealed",false);if(fitFilters_)fitFilters_();});
   connect(choice,&QComboBox::currentIndexChanged,this,[choice,raw]{raw->setText(choice->currentData().toString());});connect(raw,&QLineEdit::textChanged,this,[choice](const QString& value){const QSignalBlocker block(choice);choice->setCurrentIndex(choice->findData(value));});
  }else{filterCells.append(cell);filterWidgets[key]=cell;cell->setProperty("filterKey",key);connect(raw,&QLineEdit::textChanged,this,[this,cell](const QString& value){cell->setProperty("filterActive",!value.isEmpty());cell->setProperty("filterRevealed",false);if(fitFilters_)fitFilters_();});}
 }
 advancedBox->addLayout(filters);
 auto* techToggle=new QToolButton(advanced);techToggle->setObjectName("historyTechnicalFiltersExpanded");techToggle->setText("Technical filters");Ui::disclosure(techToggle);techToggle->setProperty("uiFlush",true);techToggle->hide();advancedBox->addWidget(technical);connect(techToggle,&QToolButton::toggled,technical,&QWidget::setVisible);
 auto* filterScroll=new QScrollArea(this);filterScroll->setObjectName("historyFiltersScroll");filterScroll->setWidgetResizable(true);filterScroll->setFrameShape(QFrame::NoFrame);filterScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);filterScroll->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);filterScroll->setWidget(advanced);Ui::transparentView(filterScroll);filterScroll->hide();auto* addFilter=Ui::toolButton("+ Filter",{},"quiet",advanced);addFilter->setObjectName("historyAddFilter");addFilter->setAccessibleName("Add a history filter");auto* filterMenu=new QMenu(addFilter);filterMenu->setObjectName("historyAddFilterMenu");addFilter->setMenu(filterMenu);addFilter->setPopupMode(QToolButton::InstantPopup);
 filterMenu->setToolTipsVisible(true);advancedToggle->setMenu(filterMenu);
 for(const auto& key:QStringList{"from","to","profileId","uploadStatus","localStatus","remoteStatus","mime","exactRemoteStatus"}){auto* cell=filterWidgets[key];auto* label=cell->findChild<QLabel*>();auto* action=filterMenu->addAction(label->text());action->setObjectName("historyAddFilter"+key);connect(action,&QAction::triggered,this,[this,cell,key]{
  // From the header the filter's own editor opens under Filter; with every filter shown it is revealed in the panel.
  if(!allFilters_->isChecked()&&editFilter_){editFilter_(key,filtersToggle_);return;}
  cell->setProperty("filterRevealed",true);if(fitFilters_)fitFilters_();auto* control=cell->findChild<QComboBox*>();if(control)control->setFocus(Qt::OtherFocusReason);else if(auto* date=cell->findChild<QDateEdit*>())date->setFocus(Qt::OtherFocusReason);else if(auto* line=cell->findChild<QLineEdit*>())line->setFocus(Qt::OtherFocusReason);});connect(filterMenu,&QMenu::aboutToShow,action,[this,cell,action,key]{const bool locked=key=="uploadStatus"&&categories_->currentRow()==5;action->setEnabled(!locked&&!cell->property("filterActive").toBool()&&!cell->property("filterRevealed").toBool());action->setToolTip(locked?"Locked to Failed while Failed uploads is selected":QString{});});}
 filterMenu->addSeparator();auto* canceledFilter=filterMenu->addAction("Include canceled");canceledFilter->setObjectName("historyAddFilterCancelled");canceledFilter->setCheckable(true);connect(canceledFilter,&QAction::toggled,showCancelled_,&QCheckBox::setChecked);connect(showCancelled_,&QCheckBox::toggled,canceledFilter,&QAction::setChecked);connect(showCancelled_,&QCheckBox::toggled,this,[this,technical](bool show){technical->setVisible(show);if(fitFilters_)fitFilters_();});
 // Every filter at once, technical ones included: the panel under the header.
 filterMenu->addSeparator();allFilters_=filterMenu->addAction("All filters…");allFilters_->setObjectName("historyAllFilters");allFilters_->setCheckable(true);allFilters_->setToolTip("Show every filter in a panel under the header");
 fitFilters_=[this,advanced,filterScroll,filters,filterCells,addFilter]{const int available=filterScroll->viewport()->width()-2*space(Space::l);int minimum=Ui::scaled(160);while(auto* item=filters->takeAt(0))delete item;QList<QWidget*> shown;for(auto* cell:filterCells){if(cell->property("filterEditing").toBool())continue;const bool visible=cell->property("filterActive").toBool()||cell->property("filterRevealed").toBool();cell->setVisible(visible);if(visible){shown.append(cell);minimum=qMax(minimum,cell->minimumSizeHint().width());}}const int columns=qBound(1,(available+space(Space::s))/(minimum+space(Space::s)),4);for(int i=0;i<4;++i){filters->setColumnMinimumWidth(i,i<qMin(columns,int(shown.size()))?minimum:0);filters->setColumnStretch(i,i<qMin(columns,int(shown.size()))?1:0);}for(int i=0;i<shown.size();++i)filters->addWidget(shown[i],i/columns,i%columns);filters->addWidget(addFilter,shown.size()/columns,shown.size()%columns,Qt::AlignLeft|Qt::AlignBottom);advanced->layout()->activate();const int column=split_?split_->height():height();int reserve=0;if(model_&&model_->rowCount()){for(int row=0;row<qMin(2,model_->rowCount());++row)reserve+=table_->sizeHintForRow(row)+2*table_->spacing();}else reserve+=2*rowContentHeight()+dayHeadingHeight();filterScroll->setFixedHeight(qMin(advanced->sizeHint().height(),qMax(1,qMin(column*2/5,column-reserve))));};fitFilters_();auto fitFilters=fitFilters_;connect(allFilters_,&QAction::toggled,this,[filterScroll,fitFilters]{QTimer::singleShot(0,filterScroll,fitFilters);});connect(techToggle,&QToolButton::toggled,this,[filterScroll,fitFilters]{QTimer::singleShot(0,filterScroll,fitFilters);});connect(allFilters_,&QAction::toggled,filterScroll,&QWidget::setVisible);
 editFilter_=[this,filterWidgets,advanced](const QString& key,QWidget* anchor){
  auto* cell=filterWidgets.value(key);if(!cell||cell->property("filterEditing").toBool())return;
  auto* editor=new HistoryFilterEditor(this);editor->setObjectName("historyFilterEditor");auto* box=new QVBoxLayout(editor);box->setContentsMargins(space(Space::l),space(Space::m),space(Space::l),space(Space::l));
  cell->setProperty("filterEditing",true);box->addWidget(cell);cell->show();editor->setMinimumWidth(qMax(Ui::scaled(260),cell->minimumSizeHint().width()+2*space(Space::l)));Ui::styleWindow(*editor);
  editor->closed=[this,cell,advanced]{cell->hide();cell->setParent(advanced);cell->setProperty("filterEditing",false);cell->setProperty("filterRevealed",false);if(fitFilters_)fitFilters_();applyFilters();};
  for(auto* child:children())if(auto* open=dynamic_cast<HistoryFilterEditor*>(child);open&&open!=editor)open->close();
  editor->adjustSize();auto origin=anchor->mapTo(this,QPoint(0,anchor->height()+space(Space::xs)));origin.setX(qBound(0,origin.x(),qMax(0,width()-editor->width()-space(Space::l))));
  editor->move(origin);editor->show();editor->raise();
  if(auto* control=cell->findChild<QComboBox*>())control->setFocus(Qt::PopupFocusReason);else if(auto* date=cell->findChild<QDateEdit*>())date->setFocus(Qt::PopupFocusReason);else if(auto* line=cell->findChild<QLineEdit*>())line->setFocus(Qt::PopupFocusReason);
 };

 activeBanner_=new QWidget(this);activeBanner_->setObjectName("historyActiveBanner");auto* activeLayout=new QGridLayout(activeBanner_);activeLayout->setContentsMargins(space(Space::xxl),space(Space::s),space(Space::l),space(Space::s));activeLayout->setSpacing(space(Space::m));activeLabel_=new Ui::ElidedLabel(this);activeLabel_->setTextFormat(Qt::PlainText);activeLabel_->setWordWrap(false);activeLabel_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);activeLabel_->setObjectName("historyActiveSummary");activeLayout->addWidget(activeLabel_,0,0);activeLayout->setColumnStretch(0,1);progress_=new QProgressBar(this);progress_->setAccessibleName("Active upload progress");progress_->setTextVisible(false);progress_->setFixedHeight(Ui::scaled(4));progress_->setMaximumWidth(180);activeLayout->addWidget(progress_,0,1);auto* moreActive=new QPushButton(this);moreActive->setObjectName("historyMoreActive");Ui::role(moreActive,"textAction");activeLayout->addWidget(moreActive,0,2);connect(moreActive,&QPushButton::clicked,this,[this]{clearFilters(true);findChild<QLineEdit*>("historyFilteruploadStatus")->setText("uploading");applyFilters();table_->scrollToTop();table_->setFocus();});auto* activeCancel=new QPushButton("Cancel this upload",this);activeCancel->setObjectName("cancelBannerOperation");activeCancel->setAccessibleName("Cancel active capture or upload");activeLayout->addWidget(activeCancel,0,3);auto* cancelAll=new QPushButton(this);cancelAll->setObjectName("cancelAllBannerOperations");activeLayout->addWidget(cancelAll,0,4);connect(cancelAll,&QPushButton::clicked,this,&HistoryWindow::prepareCancelAll);connect(activeCancel,&QPushButton::clicked,this,[this]{if(activeId_.isEmpty())return;session_.request("operation.cancel",{{"id",activeId_}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g){Ui::feedback(g->status_,e?e.message:"Cancellation requested.",bool(e));g->refresh();}});});layout->addWidget(activeBanner_);activeBanner_->hide();layout->addWidget(filterChips_);
 previewRequestTimer_=new QTimer(this);previewRequestTimer_->setSingleShot(true);previewRequestTimer_->setInterval(400);connect(previewRequestTimer_,&QTimer::timeout,this,&HistoryWindow::requestPreview);
 searchTimer_=new QTimer(this);searchTimer_->setSingleShot(true);searchTimer_->setInterval(250);connect(searchTimer_,&QTimer::timeout,this,&HistoryWindow::applyFilters);connect(search,&QLineEdit::textChanged,this,[this]{searchTimer_->start();});search->installEventFilter(this);connect(search,&QLineEdit::returnPressed,this,&HistoryWindow::applyFilters);for(const auto& key:keys)connect(findChild<QLineEdit*>("historyFilter"+key),&QLineEdit::textChanged,this,[this]{searchTimer_->start();});
 connect(categories_,&QListWidget::currentRowChanged,this,[this]{if(restoringCategory_)return;auto* state=findChild<QLineEdit*>("historyFilteruploadStatus");if(categories_->currentRow()==5)state->setText("failed");else if(state->text()=="failed")state->clear();applyFilters();});
 heading_=heading;connect(categories_,&QListWidget::currentRowChanged,this,&HistoryWindow::updateHeading);
 static const bool accessibilityInstalled=[] {QAccessible::installFactory(historyAccessibility);return true;}();Q_UNUSED(accessibilityInstalled);
 model_=new HistoryModel(session,this);table_=new HistoryList(this);table_->setObjectName("historyList");table_->setAccessibleName("History items and upload activity");table_->setAccessibleDescription("Enter for details, Menu for actions");table_->setModel(model_);table_->setFrameShape(QFrame::NoFrame);Ui::transparentView(table_);table_->setMouseTracking(true);table_->setSelectionMode(QAbstractItemView::ExtendedSelection);table_->setResizeMode(QListView::Adjust);table_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);table_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);table_->setSpacing(0);table_->setContextMenuPolicy(Qt::CustomContextMenu);auto* delegate=new HistoryDelegate(table_);table_->setItemDelegate(delegate);static_cast<HistoryList*>(table_)->columns=[delegate]{return delegate->galleryColumns();};delegate->galleryRun=[list=static_cast<HistoryList*>(table_)](int row){return list->run(row);};delegate->covered=[list=static_cast<HistoryList*>(table_)]{return list->coveredHeight();};delegate->details=[this](const QModelIndex& i){table_->selectionModel()->setCurrentIndex(i,QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);openSelectedDetails();};delegate->primary=[this](const QModelIndex& i){primaryAction(i);};if(auto* accessible=dynamic_cast<AccessibleHistoryList*>(QAccessible::queryAccessibleInterface(table_)))accessible->primaryAction=delegate->primary;delegate->menu=[this](const QModelIndex& i,const QPoint& p){showRowMenu(i,p,true);};delegate->inspectorBeside=[this]{return !compact_&&inspectorScroll_&&!inspectorScroll_->isHidden();};delegate->compactList=[this]{return compact_;};auto* openDetails=new QAction(this);openDetails->setShortcuts({QKeySequence(Qt::Key_Return),QKeySequence(Qt::Key_Enter)});openDetails->setShortcutContext(Qt::WidgetShortcut);table_->addAction(openDetails);connect(openDetails,&QAction::triggered,this,[this]{if(!table_->selectionModel()->selectedRows().isEmpty())openSelectedDetails();});connect(table_,&QListView::clicked,this,[this]{if(grid_->isChecked()&&QApplication::keyboardModifiers()==Qt::NoModifier&&table_->selectionModel()->selectedRows().size()==1)openSelectedDetails();});connect(table_,&QListView::doubleClicked,this,[this]{if(!table_->selectionModel()->selectedRows().isEmpty())openSelectedDetails();});connect(table_,&QWidget::customContextMenuRequested,this,[this](const QPoint& p){const auto i=table_->indexAt(p);if(i.isValid())showRowMenu(i,table_->viewport()->mapToGlobal(p));});auto* rowMenu=new QAction(this);rowMenu->setShortcuts({QKeySequence(Qt::Key_Menu),QKeySequence(Qt::SHIFT|Qt::Key_F10)});rowMenu->setShortcutContext(Qt::WidgetShortcut);table_->addAction(rowMenu);connect(rowMenu,&QAction::triggered,this,[this]{const auto i=table_->currentIndex();if(i.isValid())showRowMenu(i,table_->viewport()->mapToGlobal(table_->visualRect(i).center()));});
 // Empty history and no matching items say different things (applyFilters).
 emptyState_=new QWidget(this);emptyState_->setObjectName("historyEmptyState");{auto* box=new QVBoxLayout(emptyState_);box->setContentsMargins(space(Space::xl),space(Space::xl),space(Space::xl),space(Space::xl));box->setSpacing(space(Space::s));box->addStretch(2);
  empty_=new QLabel("No items yet",emptyState_);empty_->setObjectName("historyEmpty");empty_->setTextFormat(Qt::PlainText);empty_->setWordWrap(true);empty_->setAlignment(Qt::AlignCenter);Ui::role(empty_,"sectionTitle");box->addWidget(empty_);
  emptyBody_=new QLabel(emptyState_);emptyBody_->setObjectName("historyEmptyHint");emptyBody_->setTextFormat(Qt::PlainText);emptyBody_->setWordWrap(true);emptyBody_->setAlignment(Qt::AlignCenter);Ui::role(emptyBody_,"muted");box->addWidget(emptyBody_);
  resetView_=new QPushButton("Clear search and filters",emptyState_);resetView_->setObjectName("historyResetView");resetView_->hide();retryLoading_=new QPushButton("Retry loading",emptyState_);retryLoading_->setObjectName("historyRetryLoading");Ui::role(retryLoading_,"primary");retryLoading_->setAccessibleName("Retry loading history with the current filters");retryLoading_->hide();auto* loadActions=new QWidget(emptyState_);loadActions->setObjectName("historyLoadActions");auto* loadButtons=new QHBoxLayout(loadActions);loadButtons->setContentsMargins(0,0,0,0);loadButtons->setSpacing(space(Space::s));loadButtons->addWidget(retryLoading_);auto* diagnostics=new QPushButton("Save diagnostics…",loadActions);diagnostics->setObjectName("historyLoadDiagnostics");loadButtons->addWidget(diagnostics);loadActions->hide();box->addWidget(loadActions,0,Qt::AlignHCenter);connect(diagnostics,&QPushButton::clicked,this,[this,diagnostics]{diagnostics->setEnabled(false);session_.request("ui.diagnostics",{{"action","openPackage"}},this,[g=QPointer<HistoryWindow>(this),diagnostics](Error e,QJsonObject){if(!g)return;diagnostics->setEnabled(true);if(e)Ui::feedback(g->status_,e.message,true);});});connect(retryLoading_,&QPushButton::clicked,this,&HistoryWindow::refresh);box->addSpacing(space(Space::xs));box->addWidget(resetView_,0,Qt::AlignHCenter);connect(resetView_,&QPushButton::clicked,this,[this]{clearFilters(true);});box->addStretch(3);}
 // Align row content and gallery cards with the heading; selection keeps its inner padding.
 auto* listPane=new QWidget(this);auto* listBox=new QVBoxLayout(listPane);listBox->setContentsMargins(space(Space::xxl)-2*space(Space::s),0,space(Space::s),0);listBox->setSpacing(0);columns_=new QWidget(listPane);columns_->setObjectName("historyColumns");columns_->hide();filterScroll->installEventFilter(this);listBox->addWidget(table_,1);
 // Several selected rows: what they can do together.
 selectionBar_=new QWidget(listPane);selectionBar_->setObjectName("historySelectionBar");selectionBar_->setAccessibleName("Selection actions");{auto* bar=new QHBoxLayout(selectionBar_);bar->setContentsMargins(space(Space::xs),0,0,0);bar->setSpacing(space(Space::s));
  selectionCount_=new QLabel(selectionBar_);selectionCount_->setObjectName("historySelectionCount");selectionCount_->setTextFormat(Qt::PlainText);Ui::role(selectionCount_,"title");bar->addWidget(selectionCount_);selectionNote_=new Ui::ElidedLabel(selectionBar_);selectionNote_->setObjectName("historySelectionNote");static_cast<Ui::ElidedLabel*>(selectionNote_)->setElideMode(Qt::ElideRight);Ui::role(selectionNote_,"bodySecondary");bar->addWidget(selectionNote_,1);bar->addStretch();
  auto* details=new QAction("Show details",this);details->setObjectName("historyShowDetails");details->setToolTip("Open the selected items' files, link and actions (Enter)");connect(details,&QAction::triggered,this,[this]{openSelectedDetails();});
  copyLinks_=new QPushButton("Copy links",selectionBar_);copyLinks_->setObjectName("historyCopyLinks");copyLinks_->setToolTip("Copy available stored links from the selection, one per line");bar->addWidget(copyLinks_);selectionLinkHint_=new QLabel(selectionBar_);selectionLinkHint_->setObjectName("historySelectionLinkHint");selectionLinkHint_->setWordWrap(true);Ui::role(selectionLinkHint_,"meta");selectionLinkHint_->hide();bar->addWidget(selectionLinkHint_);connect(copyLinks_,&QPushButton::clicked,this,&HistoryWindow::copySelectedLinks);
  deleteSelected_=new QPushButton("Delete from destination…",selectionBar_);deleteSelected_->setObjectName("historyDeleteSelected");Ui::role(deleteSelected_,"danger");bar->addWidget(deleteSelected_);connect(deleteSelected_,&QPushButton::clicked,this,[this]{const auto ids=deletableIds();prepareDelete(ids,int(selectedFiles().size())-int(ids.size()));});
  auto* more=Ui::toolButton({}, {},"quiet",selectionBar_);more->setObjectName("historySelectionMore");more->setIcon(Ui::chromeIcon("more"));more->setAccessibleName("More selection actions");more->setToolTip(more->accessibleName());bar->addWidget(more);auto* menu=new QMenu(more);more->setMenu(menu);more->setPopupMode(QToolButton::InstantPopup);
  auto* copyDetails=menu->addAction("Copy details");copyDetails->setObjectName("historyCopySelectionDetails");connect(copyDetails,&QAction::triggered,this,[this]{QJsonArray details;for(const auto& row:table_->selectionModel()->selectedRows())details.append(model_->record(row.row()));QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(details).toJson(QJsonDocument::Indented)));Ui::feedback(status_,"Selected operation and receipt details copied.");});
  auto* clearSelection=menu->addAction("Clear selection");clearSelection->setObjectName("historyClearSelection");connect(clearSelection,&QAction::triggered,this,[this]{table_->clearSelection();table_->setFocus();});
  // Folded here when the header is too narrow for the bar's own buttons.
  auto* foldedDelete=new QAction("Delete from destination…",menu);foldedDelete->setObjectName("historySelectionMenuDelete");connect(foldedDelete,&QAction::triggered,deleteSelected_,&QPushButton::click);auto* foldedCopy=new QAction(menu);foldedCopy->setObjectName("historySelectionMenuCopyLinks");connect(foldedCopy,&QAction::triggered,copyLinks_,&QPushButton::click);menu->insertActions(copyDetails,{foldedCopy,foldedDelete});
  selectionClear_=Ui::toolButton("Clear selection",{},"quiet",selectionBar_);selectionClear_->setObjectName("historySelectionClear");selectionClear_->setToolTip("Clear selection (Esc)");bar->addWidget(selectionClear_);connect(selectionClear_,&QToolButton::clicked,this,[this]{table_->clearSelection();table_->setFocus();});}

 selectionBar_->hide();
 list_=new QStackedWidget(this);list_->addWidget(listPane);list_->addWidget(emptyState_);
 split_=new QSplitter(Qt::Horizontal,this);split_->setObjectName("historySplitter");split_->setChildrenCollapsible(false);auto* listColumn=new QWidget(split_);listColumn->setObjectName("historyListColumn");auto* listColumnBox=new QVBoxLayout(listColumn);listColumnBox->setContentsMargins(0,0,0,0);listColumnBox->setSpacing(0);listColumnBox->addWidget(filterScroll);listColumnBox->addWidget(columns_);listColumnBox->addWidget(list_,1);split_->addWidget(listColumn);inspectorScroll_=new QScrollArea(this);inspectorScroll_->setObjectName("historyInspectorScroll");inspectorScroll_->setWidgetResizable(true);inspectorScroll_->setFrameShape(QFrame::NoFrame);inspectorScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);auto* inspector=new Ui::Surface("surface",{},this);inspector->setObjectName("historyInspector");auto* inspectorBox=new QVBoxLayout(inspector);inspectorBox->setContentsMargins(0,0,0,0);singleInspector_=new QWidget(inspector);singleInspector_->setObjectName("historySingleInspector");auto* inspect=new QVBoxLayout(singleInspector_);inspect->setAlignment(Qt::AlignTop);inspect->setContentsMargins(space(Space::l),space(Space::l),space(Space::l),space(Space::l));inspect->setSpacing(space(Space::s));inspectorBox->addWidget(singleInspector_,1);selectionInspector_=new QWidget(inspector);selectionInspector_->setObjectName("historySelectionInspector");auto* selectionBox=new QVBoxLayout(selectionInspector_);selectionBox->setContentsMargins(space(Space::l),space(Space::l),space(Space::l),space(Space::l));selectionBox->setSpacing(space(Space::l));auto* selectionTitle=new QLabel("Selected items",selectionInspector_);selectionTitle->setObjectName("historySelectionTitle");Ui::role(selectionTitle,"sectionTitle");auto* selectionHeading=new QHBoxLayout;selectionHeading->addWidget(selectionTitle,1);selectionHeading->addWidget(findChild<QToolButton*>("historySelectionMore"));selectionDetailsClose_=Ui::toolButton({},{},"quiet",selectionInspector_);selectionDetailsClose_->setObjectName("historySelectionDetailsClose");selectionDetailsClose_->setIcon(Ui::chromeIcon("close"));selectionDetailsClose_->setAccessibleName("Close details");selectionDetailsClose_->setToolTip("Close details (Esc)");selectionDetailsClose_->setFixedSize(Ui::scaled(Ui::Metric::iconTarget),Ui::scaled(Ui::Metric::iconTarget));selectionHeading->addWidget(selectionDetailsClose_,0,Qt::AlignTop);connect(selectionDetailsClose_,&QToolButton::clicked,this,[this]{closeDetails();});selectionBox->addLayout(selectionHeading);selectionSummary_=new HistorySummary(selectionInspector_);selectionSummary_->setObjectName("historySelectionSummary");selectionSummary_->setAccessibleName("Selection count, total size, destinations and upload states");selectionBox->addWidget(selectionSummary_);selectionBox->addWidget(selectionBar_);selectionBox->addStretch();inspectorBox->addWidget(selectionInspector_,1);selectionInspector_->hide();
 preview_=new HistoryPreview(this);static_cast<HistoryPreview*>(preview_)->activate=[this]{expandPreview();};preview_->setToolTip("Expand preview (Enter or Space); Escape closes it");preview_->setAccessibleDescription("Expand preview with Enter or Space. Escape closes the expanded preview.");preview_->setText("Select a capture to preview");preview_->setObjectName("historyPreview");Ui::role(preview_,"well");preview_->setAccessibleName("Expand preview");preview_->setAlignment(Qt::AlignCenter);preview_->setWordWrap(true);preview_->setMinimumHeight(180);preview_->setMaximumHeight(260);preview_->setMargin(8);preview_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);inspect->addWidget(preview_);
 expandPreview_=new QPushButton(preview_);expandPreview_->setObjectName("expandHistoryPreview");expandPreview_->setAccessibleName("Expand preview");expandPreview_->setToolTip("Open a larger preview; Escape closes it");expandPreview_->setIcon(Ui::icon("zoom-fit"));expandPreview_->setIconSize(QSize(Ui::scaled(20),Ui::scaled(20)));expandPreview_->setFocusPolicy(Qt::NoFocus);Ui::role(expandPreview_,"quiet");expandPreview_->hide();static_cast<HistoryPreview*>(preview_)->expandButton=expandPreview_;connect(expandPreview_,&QPushButton::clicked,this,[this]{preview_->setFocus();expandPreview();});
 previewStatus_=new QLabel(inspector);previewStatus_->setObjectName("historyPreviewStatus");previewStatus_->setTextFormat(Qt::PlainText);previewStatus_->setWordWrap(true);Ui::role(previewStatus_,"meta");previewStatus_->hide();inspect->addWidget(previewStatus_);retryPreview_=new QPushButton("Retry preview",inspector);retryPreview_->setObjectName("retryHistoryPreview");retryPreview_->hide();inspect->addWidget(retryPreview_,0,Qt::AlignLeft);connect(retryPreview_,&QPushButton::clicked,this,[this]{const auto id=currentDetail_.value("id").toString();previewRetry_=true;previewRefused_.remove(id);previewErrors_.remove(id);pendingPreviewPath_.clear();requestPreview();});
 title_=new QLabel(this);title_->setTextFormat(Qt::PlainText);title_->setObjectName("historyTitle");title_->setWordWrap(true);Ui::role(title_,"sectionTitle");auto* inspectorHeader=new QHBoxLayout;inspectorHeader->addWidget(title_,1);detailsClose_=Ui::toolButton({}, {},"quiet",inspector);detailsClose_->setObjectName("historyDetailsClose");detailsClose_->setIcon(Ui::chromeIcon("close"));detailsClose_->setAccessibleName("Close details");detailsClose_->setToolTip("Close details (Esc)");detailsClose_->setFixedSize(Ui::scaled(Ui::Metric::iconTarget),Ui::scaled(Ui::Metric::iconTarget));detailsClose_->hide();inspectorHeader->addWidget(detailsClose_,0,Qt::AlignTop);connect(detailsClose_,&QToolButton::clicked,this,[this]{closeDetails();});inspect->insertLayout(0,inspectorHeader);outcome_=new HistoryOutcome(this);outcome_->setObjectName("historyOutcome");outcome_->setProperty("uiStatus",true);inspect->addWidget(outcome_);retry_=new QPushButton("Retry upload",this);retry_->setObjectName("retryOperation");Ui::role(retry_,"secondary");retry_->setFixedHeight(Ui::scaled(Ui::Metric::control));retry_->setMinimumWidth(0);retry_->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);inspect->addWidget(retry_);
 deletionStatus_=new QLabel(inspector);deletionStatus_->setObjectName("historyDeletionStatus");deletionStatus_->setTextFormat(Qt::PlainText);deletionStatus_->setWordWrap(true);inspect->addWidget(deletionStatus_);deletionStatus_->hide();
 deletionRetry_=new QPushButton("Retry deletion",inspector);deletionRetry_->setObjectName("retryHistoryDeletion");deletionCancel_=new QPushButton("Cancel deletion",inspector);deletionCancel_->setObjectName("cancelHistoryDeletion");
 Ui::role(deletionRetry_,"primary");auto* deletionButtons=new QHBoxLayout;deletionButtons->setSpacing(space(Space::s));for(auto* b:{deletionRetry_,deletionCancel_}){deletionButtons->addWidget(b);b->hide();}inspect->addLayout(deletionButtons);
 connect(deletionRetry_,&QPushButton::clicked,this,[this]{deletionAction("delete.retry");});connect(deletionCancel_,&QPushButton::clicked,this,[this]{deletionAction("delete.cancel");});

 items_=new QComboBox(this);items_->setObjectName("historyItems");items_->setAccessibleName("Select a file in this capture");items_->hide();inspect->addWidget(items_);connect(items_,&QComboBox::currentIndexChanged,this,[this]{if(restoringSelection_)return;selectedItemId_=items_->currentData().toString();loadSelectedItem();emit viewStateChanged();});
 auto* linkRule=Ui::rule(inspector);linkRule->setObjectName("historyLinkRule");inspect->addWidget(linkRule);
 auto* linkBar=new HistoryLinkBar(inspector);linkBar->setObjectName("historyLinkBar");auto* linkBox=new QHBoxLayout(linkBar);linkBox->setContentsMargins(space(Space::s),0,0,0);linkBox->setSpacing(0);link_=new HistoryLink(linkBar);link_->setFrame(false);link_->setObjectName("historyLink");link_->setReadOnly(true);Ui::role(link_,"technical");link_->setAccessibleName("Stored share link");link_->setPlaceholderText("No share link available");linkBox->addWidget(link_,1);inspect->addWidget(linkBar);
 linkExpiry_=new QLabel(this);linkExpiry_->setObjectName("historyLinkExpiry");linkExpiry_->setTextFormat(Qt::PlainText);linkExpiry_->setWordWrap(true);Ui::role(linkExpiry_,"meta");linkExpiry_->hide();inspect->addWidget(linkExpiry_);
 copy_=new HistoryLinkCopy(linkBar);copy_->setObjectName("copyHistoryLink");copy_->setIcon(Ui::icon("copy"));Ui::role(copy_,"primary");copy_->setFixedHeight(Ui::scaled(Ui::Metric::control));linkBox->addWidget(copy_);
 auto* unlockLink=new QPushButton("Unlock saved credentials",inspector);unlockLink->setObjectName("historyUnlockLinkCredentials");Ui::role(unlockLink,"textAction");inspect->addWidget(unlockLink,0,Qt::AlignLeft);unlockLink->hide();
 actions_=new QGridLayout;actions_->setSpacing(space(Space::s));actions_->setColumnStretch(0,1);inspect->addLayout(actions_);
 open_=new QPushButton("Open",this);open_->setObjectName("openHistoryLink");open_->setProperty("uiFlush",true);open_->setIcon(Ui::icon("open-external"));open_->setToolTip("Open in browser");open_->setAccessibleName("Open in browser");browserAction_=new HistoryActionRow(inspector);browserAction_->setObjectName("historyInspectorActions");browserAction_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);auto* actionRow=new QHBoxLayout(browserAction_);actionRow->setContentsMargins(0,0,0,0);actionRow->setSpacing(space(Space::xs));actionRow->addWidget(open_,0,Qt::AlignLeft|Qt::AlignVCenter);browserFeedback_=new QLabel(inspector);browserFeedback_->setObjectName("historyBrowserFeedback");browserFeedback_->setWordWrap(true);browserFeedback_->setTextFormat(Qt::PlainText);Ui::role(browserFeedback_,"danger");browserFeedback_->hide();inspect->addWidget(browserFeedback_);
 copyImage_=new QPushButton("Copy image",this);copyImage_->setObjectName("copyHistoryImage");saveAs_=new QPushButton("Save a copy…",this);saveAs_->setObjectName("saveHistoryAs");saveAs_->setToolTip("Save a copy of the retained local original");editAgain_=new QPushButton("Edit…",this);editAgain_->setObjectName("editHistoryImage");editAgain_->setIcon(Ui::icon("edit"));editAgain_->setAccessibleName("Edit image…");actionRow->addWidget(editAgain_,0,Qt::AlignLeft|Qt::AlignVCenter);editAgain_->setToolTip("Opens a new edit of the saved image; previous marks are flattened");connect(editAgain_,&QPushButton::clicked,this,[this]{editAgain(currentDetail_);});
 // #1428: Copy text beside Edit again; Show QR code after Open, beside Copy link.
 showQr_=new QPushButton("QR code",this);showQr_->setObjectName("showHistoryQrCode");showQr_->setIcon(Ui::icon("qr-code"));showQr_->setAccessibleName("Show QR code");actionRow->insertWidget(actionRow->indexOf(open_)+1,showQr_,0,Qt::AlignLeft|Qt::AlignVCenter);connect(showQr_,&QPushButton::clicked,this,[this]{requestQrCode(currentDetail_);});
 copyText_=new QPushButton("Copy text",this);copyText_->setObjectName("copyHistoryText");copyText_->setIcon(Ui::icon("copy-text"));copyText_->setAccessibleName("Copy text from image");actionRow->addWidget(copyText_,0,Qt::AlignLeft|Qt::AlignVCenter);connect(copyText_,&QPushButton::clicked,this,[this]{copyText(currentDetail_);});
 textPoll_=new QTimer(this);textPoll_->setInterval(250);connect(textPoll_,&QTimer::timeout,this,&HistoryWindow::pollText);saveDiagnostics_=new QPushButton("Save diagnostics…",this);saveDiagnostics_->setObjectName("saveHistoryDiagnostics");saveDiagnostics_->setAccessibleName("Save diagnostics for this upload");saveDiagnostics_->setIcon(Ui::icon("download"));actionRow->addWidget(saveDiagnostics_,0,Qt::AlignLeft|Qt::AlignVCenter);saveDiagnostics_->setToolTip("Save a redacted diagnostics package (.zip) for this upload on this computer. Nothing is uploaded.");saveDiagnostics_->hide();connect(saveDiagnostics_,&QPushButton::clicked,this,[this]{const auto rows=table_->selectionModel()->selectedRows();if(rows.size()==1)saveDiagnostics(model_->record(rows.first().row())["operationId"].toString());});for(auto* button:{open_,showQr_,copyText_,copyImage_,saveAs_,editAgain_,saveDiagnostics_}){button->setIconSize(QSize(Ui::scaled(16),Ui::scaled(16)));Ui::role(button,"quiet");button->setProperty("uiDensity","compact");}copyImage_->hide();saveAs_->hide();auto* actionMore=Ui::toolButton({}, {},"quiet",browserAction_);actionMore->setObjectName("historyInspectorMore");actionMore->setProperty("uiDensity","compact");actionMore->setIcon(Ui::chromeIcon("more"));actionMore->setAccessibleName("More file actions");actionMore->setToolTip(actionMore->accessibleName());auto* fileMenu=new QMenu(actionMore);fileMenu->setObjectName("historyInspectorMenu");actionMore->setMenu(fileMenu);actionMore->setPopupMode(QToolButton::InstantPopup);actionRow->addWidget(actionMore,0,Qt::AlignLeft|Qt::AlignVCenter);actionRow->addStretch();for(auto* button:{copyImage_,saveAs_}){auto* action=fileMenu->addAction(button->text());action->setObjectName(button->objectName()+"Action");connect(action,&QAction::triggered,button,&QPushButton::click);connect(fileMenu,&QMenu::aboutToShow,action,[button,action]{action->setVisible(button->isEnabled());action->setEnabled(button->isEnabled());});}cancel_=new QPushButton("Cancel this upload",this);cancel_->setObjectName("cancelOperation");
 share_=new QPushButton("Create share link",this);share_->setObjectName("shareHistoryLink");revoke_=new QPushButton("Revoke link…",this);revoke_->setObjectName("revokeHistoryLink");inspectLink_=new QPushButton("Inspect link",this);inspectLink_->setObjectName("inspectHistoryLink");for(auto* b:{share_,revoke_,inspectLink_})b->hide();connect(share_,&QPushButton::clicked,this,[this]{changeSharing("create");});connect(revoke_,&QPushButton::clicked,this,[this]{changeSharing("revoke");});connect(inspectLink_,&QPushButton::clicked,this,[this]{changeSharing("inspect");});
 capabilityHint_=new QLabel(inspector);capabilityHint_->setObjectName("historyCapabilityHint");capabilityHint_->setWordWrap(true);capabilityHint_->setTextFormat(Qt::PlainText);inspect->insertWidget(inspect->indexOf(outcome_),capabilityHint_);capabilityHint_->hide();changeLocalCopies_=new QPushButton("Local-copy settings",inspector);changeLocalCopies_->setObjectName("historyChangeLocalCopies");changeLocalCopies_->setToolTip("Turn on Keep a local copy in Settings → Capture to retain future captures and clipboard uploads.");Ui::role(changeLocalCopies_,"textAction");changeLocalCopies_->setProperty("uiFlush",true);inspect->addWidget(changeLocalCopies_,0,Qt::AlignLeft);changeLocalCopies_->hide();connect(changeLocalCopies_,&QPushButton::clicked,this,&HistoryWindow::localCopySettingsRequested);
 auto* actionBreak=new QVBoxLayout;actionBreak->setContentsMargins(0,space(Space::xs),0,0);auto* actionRule=Ui::rule(inspector);actionRule->setObjectName("historyActionsRule");actionBreak->addWidget(actionRule);inspect->addLayout(actionBreak);
 summary_=new HistorySummary(this);summary_->setObjectName("historySummary");summary_->setAccessibleName("File size, pixel dimensions, destination and outcomes");inspect->addWidget(summary_);
 auto* receiptToggle=new QToolButton(this);receiptToggle->setObjectName("historyReceiptExpanded");receiptToggle->setText("Advanced file details");Ui::disclosure(receiptToggle);receiptToggle->setProperty("uiFlush",true);receiptToggle->setAccessibleName("Show advanced receipt details");inspect->addWidget(receiptToggle,0,Qt::AlignLeft);details_=new QPlainTextEdit(this);details_->setObjectName("historyDetails");details_->setReadOnly(true);Ui::role(details_,"diagnostics");details_->setAccessibleName("Advanced original receipt details");details_->setMaximumHeight(180);details_->hide();previousLink_=new QWidget(inspector);previousLink_->setObjectName("historyPreviousLinkDetails");auto* previousBox=new QVBoxLayout(previousLink_);previousBox->setContentsMargins(0,0,0,0);previousBox->setSpacing(space(Space::xs));auto* previousLabel=new QLabel("Previous link — unavailable",previousLink_);Ui::role(previousLabel,"meta");previousBox->addWidget(previousLabel);previousUrl_=new HistoryLink(previousLink_);previousUrl_->setObjectName("historyPreviousLink");previousUrl_->setReadOnly(true);previousUrl_->setAccessibleName(previousLabel->text());previousLabel->setBuddy(previousUrl_);previousBox->addWidget(previousUrl_);previousLink_->hide();inspect->addWidget(previousLink_);inspect->addWidget(details_);connect(receiptToggle,&QToolButton::toggled,details_,&QWidget::setVisible);connect(receiptToggle,&QToolButton::toggled,this,[this](bool on){previousLink_->setVisible(on&&!previousUrl_->text().isEmpty());});inspect->addStretch();
 remove_=new QPushButton("Delete from destination…",this);remove_->setObjectName("deleteHistoryRemote");Ui::role(remove_,"danger");remove_->setToolTip("Delete the recorded remote file. Local copies and history records remain.");remove_->hide();inspect->addWidget(remove_,0,Qt::AlignLeft);connect(remove_,&QPushButton::clicked,this,qOverload<>(&HistoryWindow::prepareDelete));
 inspectorScroll_->setWidget(inspector);split_->addWidget(inspectorScroll_);layout->addWidget(split_,1);inspectorScroll_->hide();
 auto* footerBar=new QWidget(content);footerBar_=footerBar;footerBar->setObjectName("historyFooter");auto* footerBox=new QVBoxLayout(footerBar);footerBox->setContentsMargins(space(Space::l),space(Space::s),space(Space::l),space(Space::m));footerBox->setSpacing(space(Space::s));
 // The pager: one row under the list, a hairline above, grouped at the right inside the list's edges.
 footerRow_=new Ui::Surface("bg",Qt::TopEdge,footerBar);footerRow_->setObjectName("historyFooterRow");auto* footer=new QHBoxLayout(footerRow_);footer->setContentsMargins(space(Space::xxl),space(Space::s),space(Space::xl),space(Space::s));footer->setSpacing(space(Space::s));// Counts elide (full text in the tooltip) before they crowd the paging controls.
 footer->addStretch();pagePosition_=new QLabel(footerRow_);pagePosition_->setObjectName("historyPagePosition");pagePosition_->setTextFormat(Qt::RichText);pagePosition_->setWordWrap(true);pagePosition_->setAlignment(Qt::AlignRight|Qt::AlignVCenter);Ui::role(pagePosition_,"body");pagePosition_->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Preferred);footer->addWidget(pagePosition_,1);
 auto* previous=new QPushButton("Previous",this);previous->setObjectName("historyPrevious");previous->setIcon(Ui::chromeIcon("chevron-left"));previous->setToolTip("Previous page (Alt+Left)");auto* next=new QPushButton("Next",this);next->setObjectName("historyNext");next->setIcon(Ui::chromeIcon("chevron-right"));next->setProperty("uiIconTrailing",true);next->setToolTip("Next page (Alt+Right)");for(auto* b:{previous,next})b->setIconSize(QSize(Ui::scaled(14),Ui::scaled(14)));// Paging appears only when there is another page.
 footer->addWidget(previous);footer->addWidget(next);connect(previous,&QPushButton::clicked,this,[this]{goToPage(model_->query().offset/qMax(1,model_->query().limit));});connect(next,&QPushButton::clicked,this,[this]{goToPage(model_->query().offset/qMax(1,model_->query().limit)+2);});
 for(const auto& [key,step]:{std::pair<int,int>{Qt::Key_Left,-1},{Qt::Key_Right,1}}){auto* page=new QAction(this);page->setObjectName(step<0?"historyPreviousPage":"historyNextPage");page->setShortcut(QKeySequence(Qt::ALT|key));page->setShortcutContext(Qt::WidgetWithChildrenShortcut);addAction(page);connect(page,&QAction::triggered,this,[this,step]{if(!footerRow_->isHidden())goToPage(model_->query().offset/qMax(1,model_->query().limit)+1+step);});}auto* managementToggle=new QToolButton(this);managementToggle->setObjectName("historyManagementExpanded");managementToggle->setText("Manage");managementToggle->setIcon(Ui::chromeIcon("more"));managementToggle->setToolButtonStyle(Qt::ToolButtonTextOnly);managementToggle->setProperty("visualRole","quiet");managementToggle->setAutoRaise(true);managementToggle->setProperty("uiDisclosure",true);managementToggle->setCheckable(true);managementToggle->setAccessibleName("Manage history");managementToggle->setToolTip("Manage history");header->addWidget(managementToggle,0,5);listColumnBox->addWidget(footerRow_);footerBox->addWidget(selectionBar_);
 status_=new QLabel("Select an item to inspect its files and share link.",this);status_->setTextFormat(Qt::PlainText);status_->setAccessibleName("History status");status_->setWordWrap(true);Ui::role(status_,"status");footerBox->addWidget(status_);status_->hide();
 savedCopy_=new QWidget(footerBar);savedCopy_->setObjectName("historySavedCopy");{auto* saved=new QHBoxLayout(savedCopy_);saved->setContentsMargins(0,0,0,0);saved->setSpacing(space(Space::s));savedLabel_=new Ui::ElidedLabel(savedCopy_);savedLabel_->setObjectName("historySavedLabel");Ui::role(savedLabel_,"success");saved->addWidget(savedLabel_,1);auto* show=new QPushButton("Show in folder",savedCopy_);show->setObjectName("showHistorySavedCopy");show->setMinimumWidth(show->sizeHint().width());saved->addWidget(show);connect(show,&QPushButton::clicked,this,&HistoryWindow::showSavedCopy);auto* path=new QPushButton("Copy path",savedCopy_);path->setObjectName("copyHistorySavedPath");path->setMinimumWidth(path->sizeHint().width());saved->addWidget(path);connect(path,&QPushButton::clicked,this,&HistoryWindow::copySavedPath);auto* dismiss=Ui::toolButton({}, {},"quiet",savedCopy_);dismiss->setObjectName("dismissHistorySavedCopy");dismiss->setIcon(Ui::chromeIcon("close"));dismiss->setAccessibleName("Dismiss saved copy");dismiss->setToolTip("Dismiss");dismiss->setFixedSize(Ui::scaled(Ui::Metric::iconTarget),Ui::scaled(Ui::Metric::iconTarget));saved->addWidget(dismiss);connect(dismiss,&QToolButton::clicked,this,&HistoryWindow::dismissSavedCopy);}savedCopy_->hide();footerBox->addWidget(savedCopy_);
 auto* management=new QMenu(this);management->setObjectName("historyManagementMenu");managementToggle->setMenu(management);managementToggle->setPopupMode(QToolButton::DelayedPopup);auto* clear=management->addAction("Clear history records…");auto* cache=management->addAction("Clear temporary files…");connect(managementToggle,&QToolButton::toggled,this,[management,managementToggle](bool on){if(on)management->popup(managementToggle->mapToGlobal(QPoint(0,managementToggle->height())));else management->hide();});connect(management,&QMenu::aboutToHide,this,[managementToggle]{managementToggle->setChecked(false);});layout->addWidget(footerBar);
 sharingTimer_=new QTimer(this);sharingTimer_->setInterval(500);connect(sharingTimer_,&QTimer::timeout,this,&HistoryWindow::pollSharing);deletionTimer_=new QTimer(this);deletionTimer_->setInterval(500);connect(deletionTimer_,&QTimer::timeout,this,&HistoryWindow::pollDeletion);

 connect(clear,&QAction::triggered,this,[this]{if(!Ui::confirm(this,"Clear history records","Clear every local history record? Remote objects and user-owned local copies remain.","Clear history records",true))return;session_.request("history.clear",{},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g){Ui::feedback(g->status_,e?e.message:"History cleared.",bool(e));g->refresh();}});});connect(cache,&QAction::triggered,this,[this]{if(!Ui::confirm(this,"Clear temporary files","Removes cached previews and temporary upload files. Some failed uploads will no longer be retryable. Saved local copies and uploaded files remain.","Clear temporary files",true))return;session_.request("cache.clear",{},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject result){if(g){g->releasePreview();
  // An incomplete upload that couldn't be aborted keeps using storage (#146).
  const auto kept=result["incompleteUploads"].toArray();const auto uploads=kept.size()==1?QStringLiteral("an incomplete upload"):QStringLiteral("%1 incomplete uploads").arg(kept.size());
  const auto notice=result["incompleteUploadNotice"].toObject()["message"].toString();
  const auto message=kept.isEmpty()?QStringLiteral("Temporary files cleared."):!notice.isEmpty()?notice:QStringLiteral("Temporary files cleared, but %1 couldn't be removed. %2").arg(uploads,kept[0].toObject()["error"].toObject()["message"].toString());
  Ui::feedback(g->status_,e?e.message:message,bool(e)||!kept.isEmpty());g->refresh();}});});
 connect(unlockLink,&QPushButton::clicked,this,[this,unlockLink]{
  const auto id=unlockLinkItemId_;if(id.isEmpty()||selectedRecord()["id"]!=id)return;
  unlockLink->setEnabled(false);session_.request("history.unlockLinkCredentials",{{"id",id}},this,[g=QPointer<HistoryWindow>(this),id,unlockLink](Error e,QJsonObject){
   if(!g||g->selectedRecord()["id"]!=id||g->unlockLinkItemId_!=id)return;
   unlockLink->setEnabled(true);if(!e)g->unlockLinkItemId_.clear();unlockLink->setVisible(bool(e));Ui::feedback(g->status_,e?e.message:"Credentials loaded. Choose Refresh and copy link.",bool(e));
  });
 });
 connect(copy_,&QPushButton::clicked,this,[this]{const auto record=selectedRecord();if(record.isEmpty())return;if(record["uploadStatus"]=="local_only"){operationAction("operation.copy");return;}copyHistoryLink(record);});connect(open_,&QPushButton::clicked,this,[this]{if(currentDetail_["linkAvailable"].toBool())openBrowser(link_->text());});connect(retry_,&QPushButton::clicked,this,[this]{const auto rows=table_->selectionModel()->selectedRows();if(rows.size()==1)operationAction(HistoryModel::retryUpload(model_->record(rows.first().row()))?"operation.retry":"operation.retryLocalSave");});connect(cancel_,&QPushButton::clicked,this,[this]{operationAction("operation.cancel");});connect(copyImage_,&QPushButton::clicked,this,[this]{exportLocal(true);});connect(saveAs_,&QPushButton::clicked,this,[this]{exportLocal(false);});
 connect(table_->selectionModel(),&QItemSelectionModel::selectionChanged,this,[this]{if(!restoringSelection_){selectedItemId_.clear();selectionChanged();}});connect(model_,&QAbstractItemModel::modelAboutToBeReset,this,[this]{if(!restoringSelection_)restoreSelection_=selectedIds();restoringSelection_=true;});
 auto* filtering=new QWidget(list_);auto* filteringBox=new QVBoxLayout(filtering);filteringBox->addStretch();auto* busy=new QProgressBar(filtering);busy->setObjectName("historyFilteringProgress");busy->setRange(0,0);busy->setTextVisible(false);busy->setFixedSize(Ui::scaled(120),6);filteringBox->addWidget(busy,0,Qt::AlignHCenter);auto* filteringText=new QLabel("Filtering history…",filtering);filteringText->setObjectName("historyFiltering");filteringText->setAccessibleName(filteringText->text());filteringText->setAlignment(Qt::AlignCenter);Ui::role(filteringText,"body");filteringBox->addWidget(filteringText);filteringBox->addStretch();list_->addWidget(filtering);
 connect(model_,&HistoryModel::activityChanged,this,&HistoryWindow::updateActivity);connect(model_,&HistoryModel::countsChanged,this,[this]{updateCountNote();updateCategoryCounts();table_->viewport()->update();});
 connect(model_,&HistoryModel::busyChanged,this,[this,filtering](bool busy){table_->setEnabled(!busy);inspectorScroll_->setEnabled(!busy);table_->setAccessibleDescription(busy?"Busy: filtering history. Enter for details, Menu for actions":"Enter for details, Menu for actions");setProperty("busy",busy);updateCountNote();updatePager();if(busy){++selectionRequest_;currentDetail_={};selectedDeletion_={};selectionBar_->hide();inspectorScroll_->hide();list_->show();detailsOpen_=false;list_->setCurrentWidget(filtering);if(status_->text()=="Select an item to inspect its files and share link.")Ui::feedback(status_,{});}else{list_->setCurrentWidget(model_->rowCount()?table_->parentWidget():emptyState_);}emit viewStateChanged();});
 connect(model_,&HistoryModel::loaded,this,[this]{retryLoading_->hide();findChild<QWidget*>("historyLoadActions")->hide();for(int row=0;row<model_->rowCount();++row){const auto r=model_->record(row);bool selected=restoreSelection_.contains("operation:"+r["operationId"].toString());for(auto v:r["records"].toArray())selected|=restoreSelection_.contains(v.toObject()["id"].toString());if(selected){table_->selectionModel()->select(model_->index(row),QItemSelectionModel::Select|QItemSelectionModel::Rows);if(!table_->currentIndex().isValid())table_->selectionModel()->setCurrentIndex(model_->index(row),QItemSelectionModel::NoUpdate);}}restoreSelection_.clear();restoringSelection_=false;{const auto& q=model_->query();const bool filtered=!(q.search.isEmpty()&&q.category.isEmpty()&&q.mime.isEmpty()&&q.profileId.isEmpty()&&q.uploadStatus.isEmpty()&&q.localStatus.isEmpty()&&q.remoteStatus.isEmpty()&&q.from.isEmpty()&&q.to.isEmpty());const bool onlyCategory=filtered&&(!q.category.isEmpty()||categories_->currentRow()==5)&&q.search.isEmpty()&&activeFilters().isEmpty();const auto category=categories_->currentItem()?categories_->currentItem()->text():QString{};
   empty_->setText(onlyCategory?"Nothing in "+category:filtered?QString("No matching items"):QString("No items yet"));const bool searched=!q.search.isEmpty(),restricted=!activeFilters().isEmpty();const QString where=q.category.isEmpty()?QString{}:" in "+category;
   emptyBody_->setText(onlyCategory?QString("Items of this kind appear here once you capture or upload them."):searched&&restricted?"Nothing"+where+" matches “"+q.search+"” with these filters.":searched?"Nothing"+where+" matches “"+q.search+"”.":filtered?"Nothing"+where+" matches these filters.":QString("Capture a region or upload your clipboard to get started. Captures and uploads appear here."));
   resetView_->setText(onlyCategory?"Show all items":searched&&!restricted&&q.category.isEmpty()?"Clear search":!searched&&restricted&&q.category.isEmpty()?"Clear filters":"Show all items");resetView_->setAccessibleName(resetView_->text());resetView_->setVisible(filtered);}list_->setCurrentWidget(model_->rowCount()?table_->parentWidget():emptyState_);updateFilterSummary();
   // A deletion that empties the last page steps back to the new last page.
   {const auto& q=model_->query();if(!model_->filesReturned()&&!model_->rowCount()&&q.offset>0&&model_->total()>0){pagerTotal_=int(qMin<qint64>(model_->total(),INT_MAX));goToPage(pageCount());return;}}
   updatePager();updateCountNote();if(restoreDetailsOpen_){detailsOpen_=true;restoreDetailsOpen_=false;}selectionChanged();if(restoreScroll_){restoreScroll_=false;QTimer::singleShot(0,this,[this]{table_->doItemsLayout();table_->verticalScrollBar()->setValue(grid_->isChecked()?galleryScroll_:listScroll_);});}updateActivity();if(awaitingInitialization_){awaitingInitialization_=false;Ui::feedback(status_,{});}emit viewStateChanged();});connect(model_,&HistoryModel::error,this,[this](const QString& message){Ui::feedback(status_,message,true);if(!model_->busy()&&!model_->rowCount()){Ui::feedback(status_,{});empty_->setText("Couldn't load history");emptyBody_->setText("Couldn't read the history database on this computer.");retryLoading_->show();findChild<QWidget*>("historyLoadActions")->show();resetView_->hide();list_->setCurrentWidget(emptyState_);count_->setText("—");count_->setToolTip("History unavailable");count_->setAccessibleName("History unavailable");}restoringSelection_=false;restoreSelection_.clear();emit viewStateChanged();});
 connect(&session_,&UiSession::operationsChanged,this,&HistoryWindow::refresh);connect(&session_,&UiSession::initialized,this,&HistoryWindow::refresh);for(auto* e:findChildren<QLineEdit*>())if(!e->isReadOnly())connect(e,&QLineEdit::textChanged,this,&HistoryWindow::viewStateChanged);for(auto* t:{managementToggle,receiptToggle})connect(t,&QToolButton::toggled,this,&HistoryWindow::viewStateChanged);connect(allFilters_,&QAction::toggled,this,&HistoryWindow::viewStateChanged);connect(allFilters_,&QAction::toggled,this,&HistoryWindow::updateFilterSummary);connect(categories_,&QListWidget::currentRowChanged,this,&HistoryWindow::viewStateChanged);
 for(auto* b:findChildren<QPushButton*>())if(b->accessibleName().isEmpty())b->setAccessibleName(b->text());
 // Pixmaps and property values follow a scheme switch (#60).
 connect(&Theme::Notifier::instance(),&Theme::Notifier::changed,this,[this]{const auto rows=table_->selectionModel()->selectedRows();if(rows.size()==1)setHistoryOutcome(outcome_,model_->record(rows.first().row()));updateSummary();columns_->update();});
 connect(table_->verticalScrollBar(),&QScrollBar::valueChanged,this,[this]{if(grid_->isChecked()&&detailsOpen_)updateInspectorLayout();});split_->installEventFilter(this);status_->installEventFilter(this);savedCopy_->installEventFilter(this);setGridMode(false);refresh();
}
void HistoryWindow::applyFilters(){searchTimer_->stop();{const QSignalBlocker block(categories_);const bool failed=findChild<QLineEdit*>("historyFilteruploadStatus")->text()=="failed";if(categories_->currentRow()==5)findChild<QLineEdit*>("historyFilteruploadStatus")->setText("failed");else if(categories_->currentRow()==0&&failed)categories_->setCurrentRow(5);}auto q=model_->query();q.offset=0;if(auto* item=categories_->currentItem())q.category=categories_->currentRow()==5?QString{}:item->data(Qt::UserRole).toString();q.search=findChild<QLineEdit*>("historySearch")->text();q.from=findChild<QLineEdit*>("historyFilterfrom")->text();q.to=findChild<QLineEdit*>("historyFilterto")->text();q.mime=findChild<QLineEdit*>("historyFiltermime")->text();q.profileId=findChild<QLineEdit*>("historyFilterprofileId")->text();q.uploadStatus=findChild<QLineEdit*>("historyFilteruploadStatus")->text();q.localStatus=findChild<QLineEdit*>("historyFilterlocalStatus")->text();q.remoteStatus=findChild<QLineEdit*>("historyFilterremoteStatus")->text();updateHeading();model_->setQuery(q);updateFilterSummary();emit viewStateChanged();}
// The heading names what the list shows: the category, and failed uploads
// when only those are shown (the sidebar toggle or the Upload state filter).
void HistoryWindow::updateHeading(){if(!heading_)return;auto* upload=findChild<QComboBox*>("historyChoiceuploadStatus");const bool locked=categories_->currentRow()==5;Ui::setLocked(upload,locked);upload->setEnabled(!locked);upload->setToolTip(locked?"Locked to Failed while Failed uploads is selected":QString{});upload->setAccessibleDescription(upload->toolTip());const auto* item=categories_->currentItem();const auto category=item?item->text():QString("All items");const bool failed=findChild<QLineEdit*>("historyFilteruploadStatus")->text()=="failed";const auto text=!failed?category:categories_->currentRow()==0||categories_->currentRow()==5?QString("Failed uploads"):category+" · failed uploads";if(heading_->text()!=text){heading_->setText(text);updateFooterLayout();}}
// A wheel step defaults to the first item's height. Keep the list's, its first row
// under a day heading: a gallery tile would move about 2.4 times as far.
void HistoryWindow::setGridMode(bool grid){grid_->setChecked(grid);findChild<QToolButton*>("historyListMode")->setChecked(!grid);table_->parentWidget()->layout()->setContentsMargins(space(Space::xxl)-(grid?1:2)*space(Space::s),0,space(Space::s),0);table_->setViewMode(grid?QListView::IconMode:QListView::ListMode);table_->setMovement(grid?QListView::Free:QListView::Static);table_->setDragEnabled(false);table_->setDragDropMode(QAbstractItemView::NoDragDrop);table_->viewport()->setAcceptDrops(false);table_->setWrapping(grid);table_->setGridSize(QSize());table_->setUniformItemSizes(false);table_->verticalScrollBar()->setSingleStep(grid?rowContentHeight()+dayHeadingHeight():-1);table_->doItemsLayout();updateInspectorLayout();emit viewStateChanged();}
void HistoryWindow::updateInspectorLayout(){if(!split_)return;updateFooterLayout();
 // Side by side only while both panes get what they need at this text size:
 // the list a readable name beside its thumbnail and actions, the inspector
 // its preview and buttons. Below that the inspector opens over the list.
 const int selected=table_->selectionModel()->selectedRows().size();const bool single=selected==1,gallery=grid_->isChecked();
 const int inspectorWidth=qMax(Ui::scaled(380),qMax(copy_->minimumWidth(),copy_->sizeHint().width())+open_->sizeHint().width()+editAgain_->sizeHint().width()+findChild<QToolButton*>("historyInspectorMore")->sizeHint().width()+3*space(Space::s)+2*space(Space::l));compact_=split_->width()<Ui::scaled(420)+inspectorWidth;if((!compact_&&!detailsFromGallery_)||(!selected&&!restoringSelection_))detailsOpen_=false;
 const bool details=compact_&&detailsOpen_,back=detailsOpen_&&compact_;detailsClose_->setVisible(true);split_->setOrientation(Qt::Horizontal);
 inspectorScroll_->setMinimumWidth(compact_||details?0:inspectorWidth);inspectorScroll_->setMaximumWidth(compact_||details?QWIDGETSIZE_MAX:inspectorWidth);inspectorScroll_->setMaximumHeight(QWIDGETSIZE_MAX);
 // Spend the viewport left after the measured title, outcome and actions on the image.
 auto* inspect=singleInspector_->layout();const int textWidth=qMax(1,(compact_?split_->width():inspectorWidth)-2*space(Space::l));int facts=2*space(Space::l);
 for(int i=0;i<inspect->count();++i){auto* item=inspect->itemAt(i);if(item->widget()!=preview_&&!item->isEmpty())facts+=(item->hasHeightForWidth()?item->heightForWidth(textWidth):item->sizeHint().height())+space(Space::s);if(item->layout()==actions_)break;}
 const int viewport=inspectorScroll_->isVisible()?inspectorScroll_->viewport()->height():split_->height(),previewHeight=qBound(48,viewport-facts,180);const bool ready=!static_cast<HistoryPreview*>(preview_)->image.isNull();preview_->setMinimumHeight(compact_?previewHeight:180);preview_->setMaximumHeight(compact_?previewHeight:260);static_cast<HistoryPreview*>(preview_)->setThumbnail(compact_&&previewHeight<120);
 const auto* picture=static_cast<HistoryPreview*>(preview_);preview_->setMinimumWidth(0);preview_->setMaximumWidth(QWIDGETSIZE_MAX);inspect->setAlignment(preview_,{});if(ready&&!picture->thumbnail){const int inset=2*preview_->margin(),height=compact_?previewHeight:qBound(180,viewport-facts,260);const auto fitted=picture->image.size().scaled(QSize(qMax(1,textWidth-inset),height-inset),Qt::KeepAspectRatio);preview_->setFixedSize(fitted+QSize(inset,inset));inspect->setAlignment(preview_,Qt::AlignLeft);}
 // Centred at a natural size, but never taller than the viewport left after the title, outcome and actions.
 if(compact_&&ready&&!picture->thumbnail){const int inset=2*preview_->margin(),side=qMax(1,std::min({textWidth-inset,picture->image.width(),Ui::scaled(480)})),height=qMax(1,qMin(Ui::scaled(480),qMax(48,viewport-facts))-inset);const auto fitted=picture->image.size().scaled(QSize(side,height),Qt::KeepAspectRatio);preview_->setFixedSize(fitted+QSize(inset,inset));inspect->setAlignment(preview_,Qt::AlignHCenter);}
 const bool missing=previewErrors_.value(currentDetail_.value("id").toString()).code=="preview_source_missing",galleryCard=gallery&&detailsOpen_&&!compact_&&single&&table_->isVisibleTo(this)&&table_->viewport()->rect().intersects(table_->visualRect(table_->currentIndex()));
 static_cast<HistoryPreview*>(preview_)->updateExpand();preview_->setVisible(!missing&&!galleryCard&&(ready||(!currentDetail_.isEmpty()&&(currentDetail_.value("category")=="images"||video(currentDetail_))))&&previewErrors_.value(currentDetail_.value("id").toString()).code!="preview_unsupported");
 if(auto* box=singleInspector_->layout()){box->setContentsMargins(space(Space::l),space(Space::l),space(Space::l),space(Space::l));box->setSpacing(space(Space::s));}
 // Hide the old navigation before showing its replacement; Qt otherwise grows the window briefly.
 split_->setStretchFactor(0,1);split_->setStretchFactor(1,0);detailsBack_->setText(detailsFromGallery_?"Back to gallery":"Back to list");detailsBack_->setToolTip(detailsFromGallery_?"Return to the gallery (Esc)":"Return to the list (Esc)");if(!back)detailsBack_->hide();if(back)detailsBack_->show();const bool wasDetails=list_->isHidden();list_->setVisible(!details);findChild<QWidget*>("historyListColumn")->setVisible(!details);
 // The wide list shows details whenever they are wanted and something is selected;
 // the gallery and the compact window open them on request.
 const bool shown=selected>0&&(compact_?details:gallery?detailsOpen_:detailsWanted_);
 inspectorScroll_->setVisible(shown);singleInspector_->setVisible(single);selectionInspector_->setVisible(selected>1);
 if(auto* escape=findChild<QAction*>("historyDetailsEscape"))escape->setEnabled(shown||selected>1||(expandedPreview_&&expandedPreview_->isVisible()));
 {const bool on=selected>0&&(gallery||compact_)?shown:detailsWanted_;const QSignalBlocker block(detailsToggle_);detailsToggle_->setChecked(on);
  const auto name=selected>0?QString(shown?"Hide details":"Show details"):QString(detailsWanted_?"Details open when you select an item":"Details stay closed");
  detailsToggle_->setAccessibleName(name);detailsToggle_->setToolTip(selected>0?name+(shown?" (Esc)":" (Enter)"):name);}
 // Opened over the list, the inspector takes the whole width; the split's
 // inspector maximum bounds it again once both panes fit.
 if(details&&!wasDetails)split_->setSizes({0,qMax(1,split_->width())});else if(wasDetails&&!details)split_->setSizes({qMax(1,split_->width()-Ui::scaled(320)),Ui::scaled(320)});
 updateSelectionBar();static_cast<HistoryDelegate*>(table_->itemDelegate())->updateMore();table_->viewport()->update();}
void HistoryWindow::openSelectedDetails(){if(model_->busy())return;const bool gallery=grid_->isChecked();
 // In the wide list details are a standing choice: asking for them turns it back on.
 if(!gallery&&!compact_){setDetailsWanted(true);return;}
 if(detailsOpen_)return;if(!detailsOpen_){if(gallery)galleryScroll_=table_->verticalScrollBar()->value();else listScroll_=table_->verticalScrollBar()->value();}detailsFromGallery_=gallery;setDetailsOpen(true);}
void HistoryWindow::setDetailsOpen(bool open){
 const bool wasOpen=detailsOpen_,gallery=detailsFromGallery_;detailsOpen_=open&&(compact_||gallery)&&!table_->selectionModel()->selectedRows().isEmpty();
 if(!open)detailsFromGallery_=false;updateInspectorLayout();
 const auto closer=[this]{return compact_?static_cast<QWidget*>(detailsBack_):table_->selectionModel()->selectedRows().size()>1?static_cast<QWidget*>(selectionDetailsClose_):detailsClose_;};
 if(detailsOpen_){inspectorScroll_->verticalScrollBar()->setValue(0);closer()->setFocus(Qt::OtherFocusReason);QTimer::singleShot(0,this,[this,closer]{if(detailsOpen_)closer()->setFocus(Qt::OtherFocusReason);});}else if(wasOpen){table_->doItemsLayout();table_->verticalScrollBar()->setValue(gallery?galleryScroll_:listScroll_);table_->setFocus(Qt::OtherFocusReason);QTimer::singleShot(0,this,[this,gallery]{table_->verticalScrollBar()->setValue(gallery?galleryScroll_:listScroll_);});}emit viewStateChanged();
}
void HistoryWindow::updateFooterLayout(){
 if(!footerRow_||!columns_)return;
 // All History tools remain in this toolbar at every size; only its own
 // rows reflow. The rail keeps navigation and the compact result count.
 const bool dense=Ui::scaled(100)<=150&&height()<600;auto* header=findChild<QWidget*>("historyCompactControls");
 if(status_->text()=="Select an item to inspect its files and share link.")status_->hide();
 auto* sidebar=findChild<QWidget*>("historySidebar");auto* rail=qobject_cast<QVBoxLayout*>(sidebar->layout());auto* box=qobject_cast<QVBoxLayout*>(footerBar_->layout());auto* tools=qobject_cast<QGridLayout*>(header->layout());
 setProperty("denseHistory",dense);categories_->setProperty("denseHistory",dense);rail->setContentsMargins(space(Space::l),dense?space(Space::s):space(Space::l),space(Space::l),space(Space::l));box->setContentsMargins(space(Space::l),space(Space::s),space(Space::l),space(Space::s));footerBar_->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Maximum);
 tools->setContentsMargins(space(Space::xxl),Ui::pageHeaderTop(),space(Space::l),dense?(Ui::scaled(100)>125?0:space(Space::xs)):space(Space::s));tools->setHorizontalSpacing(space(Space::l));tools->setVerticalSpacing(dense&&Ui::scaled(100)>125?0:space(Space::xs));
 auto* search=findChild<QLineEdit*>("historySearch");auto* views=findChild<QWidget*>("historyViewMode");auto* management=findChild<QToolButton*>("historyManagementExpanded");views->setFixedWidth(views->sizeHint().width());
 // The search minimum is set once, after the fit: a transient 180 px floor would clamp
 // the search past its grid cell if the grid applied geometry during this pass.
 heading_->setMinimumWidth(0);int searchMinimum=180;
 const int gap=qMax(0,tools->horizontalSpacing()),available=header->width()-tools->contentsMargins().left()-tools->contentsMargins().right();
 // A narrow header (the compact window) takes the 20 px heading and drops Manage's word for ⋯.
 const bool narrow=header->width()<Ui::scaled(900);if(heading_->property("compactHeading").toBool()!=narrow){heading_->setProperty("compactHeading",narrow);Ui::role(heading_,"title");}
 {const bool words=!narrow;management->setToolButtonStyle(words?Qt::ToolButtonTextOnly:Qt::ToolButtonIconOnly);management->setProperty("noChevron",!words);management->setIconSize(QSize(Ui::scaled(Ui::Metric::icon),Ui::scaled(Ui::Metric::icon)));}
 const QList<QWidget*> controls{filtersToggle_,views,detailsToggle_,management};
 const auto controlWidth=[&]{int width=0;for(auto* control:controls)width+=qMax(control->minimumWidth(),control->sizeHint().width());return width;};
 // Filter keeps its word while the heading still fits beside the tools; then it keeps its glyph and chevron.
 filtersToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);count_->setMinimumWidth(0);
 if(available<180+controlWidth()+int(controls.size())*gap+findChild<QWidget*>("historyHeadingGroup")->minimumSizeHint().width()+space(Space::l))filtersToggle_->setToolButtonStyle(Qt::ToolButtonIconOnly);
 const int required=180+controlWidth()+int(controls.size())*gap;
 const bool details=compact_&&detailsOpen_,selecting=selectionHeader()&&!details,wrap=!selecting&&required>available;auto* heading=findChild<QWidget*>("historyHeadingGroup");
 // The count note gives way before the heading takes a row of its own: it elides in what is left, never below
 // an ellipsis, and the whole heading group carries its text as a tooltip.
 const int ellipsis=heading->layout()->spacing()+count_->fontMetrics().horizontalAdvance(QChar(0x2026))+2;
 const bool headingRow=!selecting&&available<required+heading_->minimumSizeHint().width()+ellipsis+space(Space::l),noteElides=!headingRow&&available<required+heading->minimumSizeHint().width()+space(Space::l);if(noteElides)count_->setMinimumWidth(ellipsis-heading->layout()->spacing());const int toolRow=headingRow&&!details?1:0;
 // A heading on its own row needs room for its text, rather than a control target.
 heading->setFixedHeight(headingRow?qMax(Ui::Metric::control,heading->layout()->totalMinimumSize().height()):Ui::scaled(Ui::Metric::control));
 while(auto* item=tools->takeAt(0))delete item;for(int i=0;i<7;++i)tools->setColumnStretch(i,0);
 heading->setVisible(!details&&!selecting);detailsBack_->setVisible(details);views->setVisible(!details);detailsToggle_->setVisible(!details);search->setVisible(!details&&!selecting);filtersToggle_->setVisible(!details&&!selecting);management->setVisible(!details&&!selecting);
 tools->setContentsMargins(space(details?Space::l:Space::xxl),Ui::pageHeaderTop(),space(Space::l),tools->contentsMargins().bottom());header->parentWidget()->setProperty("inspector",details);header->parentWidget()->update();
 if(filterChips_)filterChips_->setVisible(!details&&!selecting&&!filterEntries().isEmpty());
 int column=0;
 if(selecting){tools->addWidget(selectionBar_,0,column);tools->setColumnStretch(column++,1);for(auto* control:{views,static_cast<QWidget*>(detailsToggle_)})tools->addWidget(control,0,column++);}
 else{
 if(details)tools->addWidget(detailsBack_,0,column++,Qt::AlignLeft);else if(headingRow)tools->addWidget(heading,0,0,1,6);else tools->addWidget(heading,0,column++);
 if(wrap){int row=toolRow+1,used=0,columns=0;column=0;for(auto* control:controls){if(control->isHidden())continue;const int width=qMax(control->minimumWidth(),control->sizeHint().width()),before=column?gap:0;if(column&&used+before+width>available){++row;column=0;used=0;}tools->addWidget(control,row,column++,Qt::AlignLeft);columns=qMax(columns,column);used+=(used?gap:0)+width;}column=qMax(1,columns);tools->addWidget(search,toolRow,0,1,column);tools->setColumnStretch(column-1,1);}
 else{tools->addWidget(search,toolRow,column);tools->setColumnStretch(column++,1);int others=0;for(auto* control:controls)if(!control->isHidden()){tools->addWidget(control,toolRow,column++);others+=qMax(control->minimumWidth(),control->sizeHint().width());}
  // A heading beside search keeps its words: search gives way down to its placeholder first.
  if(!headingRow&&!details){const int full=heading_->sizeHint().width(),extra=noteElides?ellipsis:heading->sizeHint().width()-full,room=available-others-(column-1)*tools->horizontalSpacing()-extra;if(room-full<180){const int floor=qMin(180,search->fontMetrics().horizontalAdvance(search->placeholderText())+Ui::scaled(24));searchMinimum=qMax(floor,room-full);heading_->setMinimumWidth(qMax(0,qMin(full,room-searchMinimum)));}}}
 }
 search->setMinimumWidth(searchMinimum);
 // Empty spanned columns still consume spacing in QGridLayout.
 if(headingRow&&!details&&!selecting){tools->removeWidget(heading);tools->addWidget(heading,0,0,1,column);}header->show();header->setProperty("wrapped",wrap);
 columns_->hide();
 footerRow_->setVisible(pageCount()>1);
 auto* navigation=findChild<QWidget*>("historyNavigation");int categoryHeight=0;QStyleOptionViewItem option;option.initFrom(categories_);option.widget=categories_;option.rect=categories_->viewport()->rect();for(int i=0;i<categories_->count();++i)categoryHeight+=categories_->itemDelegate()->sizeHint(option,categories_->model()->index(i,0)).height();categories_->setFixedHeight(categoryHeight+4);categories_->doItemsLayout();
 navigation->layout()->invalidate();navigation->setMinimumHeight(navigation->layout()->minimumSize().height());
 footerBar_->setVisible(!status_->isHidden()||!savedCopy_->isHidden()||(selectionBar_->parentWidget()==footerBar_&&!selectionBar_->isHidden()));columns_->update();
}
// "236 items", "12 of 168 items" while a search or filter narrows the category,
// blank while the query loads; never 0 for a total that is not known.
QString HistoryWindow::countNote()const{
 if(model_->busy())return {};const auto& q=model_->query();const auto total=model_->total();
 const bool narrowed=!q.search.isEmpty()||!activeFilters().isEmpty();const auto items=[](qint64 count){return QString("%1 %2").arg(QLocale().toString(count),count==1?"item":"items");};
 if(!narrowed)return items(total);
 const int row=categories_->currentRow();const QString key=row==5?QString("failed"):row==1?QString("images"):row==2?QString("videos"):row==3?QString("text+other"):QString("all");
 const auto totals=model_->categoryTotals();if(!totals.contains(key))return QString("%1 matching %2").arg(QLocale().toString(total),total==1?"item":"items");
 return QString("%1 of %2").arg(QLocale().toString(total),items(totals[key].toInteger()));
}
void HistoryWindow::updateCountNote(){const auto note=countNote();count_->setText(note);count_->setToolTip(note);count_->parentWidget()->setToolTip(note);count_->setAccessibleName(note.isEmpty()?QString("Counting items"):note);}
int HistoryWindow::pageCount()const{const int limit=qMax(1,model_->query().limit);return pagerTotal_>0?int((pagerTotal_+limit-1)/limit):0;}
// Page position from the query's offset and the files it returned, not from rows.
QJsonObject HistoryWindow::pageState()const{
 const auto& q=model_->query();const int limit=qMax(1,q.limit),files=model_->filesReturned();
 return {{"page",q.offset/limit+1},{"pageCount",pageCount()},{"firstItem",files?q.offset+1:0},{"lastItem",q.offset+files},{"total",double(model_->busy()?pagerTotal_:model_->total())},{"limit",limit},{"offset",q.offset},{"loading",model_->busy()}};
}
void HistoryWindow::updatePager(){
 if(!footerRow_)return;if(!model_->busy())pagerTotal_=int(qMin<qint64>(model_->total(),INT_MAX));
 auto* previous=findChild<QPushButton*>("historyPrevious");auto* next=findChild<QPushButton*>("historyNext");const auto& q=model_->query();const int pages=pageCount(),page=q.offset/qMax(1,q.limit)+1,files=model_->filesReturned();
 const bool loading=model_->busy();previous->setEnabled(!loading&&q.offset>0);next->setEnabled(!loading&&page<pages);
 const auto plain=loading||!files?QString{}:QString("Page %1 of %2 · items %3–%4 of %5").arg(page).arg(pages).arg(QLocale().toString(q.offset+1),QLocale().toString(q.offset+files),QLocale().toString(model_->total()));
 pagePosition_->setText(plain.isEmpty()?QString{}:QString("<span style=\"font-weight:500\">Page %1 of %2</span> · items %3–%4 of %5").arg(page).arg(pages).arg(QLocale().toString(q.offset+1),QLocale().toString(q.offset+files),QLocale().toString(model_->total())));
 pagePosition_->setAccessibleName(plain);pagePosition_->setToolTip(plain);footerRow_->setVisible(pages>1);
}
void HistoryWindow::goToPage(int page){
 if(model_->busy())return;auto q=model_->query();const int limit=qMax(1,q.limit),last=qMax(1,pageCount());page=qBound(1,page,last);if((page-1)*limit==q.offset)return;
 // Changing page starts at the top of the new page; the list keeps the keyboard.
 const bool focus=table_->hasFocus()||(QApplication::focusWidget()&&QApplication::focusWidget()->objectName().startsWith("historyPrevious"))||(QApplication::focusWidget()&&QApplication::focusWidget()->objectName()=="historyNext");
 q.offset=(page-1)*limit;listScroll_=galleryScroll_=0;model_->setQuery(q);table_->scrollToTop();if(focus)table_->setFocus(Qt::OtherFocusReason);emit viewStateChanged();
}
// Unfiltered totals per category; Failed uploads in the error colour and only when there are some.
void HistoryWindow::updateCategoryCounts(){
 const auto totals=model_->categoryTotals();
 for(int row=0;row<categories_->count();++row){auto* item=categories_->item(row);if(item->flags()==Qt::NoItemFlags)continue;const auto category=row==5?QString("failed"):item->data(Qt::UserRole).toString().isEmpty()?QString("all"):item->data(Qt::UserRole).toString();
  const auto value=totals.value(category);const qint64 count=value.isDouble()?value.toInteger():-1;const bool failed=row==5;
  item->setData(Ui::NavigationDelegate::TrailingRole,count<0||(failed&&count==0)?QString{}:QLocale().toString(count));item->setData(Ui::NavigationDelegate::TrailingDangerRole,failed);
  item->setData(Qt::AccessibleDescriptionRole,count<0?QString{}:QString("%1 %2").arg(QLocale().toString(count),count==1?"item":"items"));}
}
void HistoryWindow::setDetailsWanted(bool wanted){
 const bool changed=detailsWanted_!=wanted;detailsWanted_=wanted;updateInspectorLayout();
 // Closing keeps the selection and returns to the list where it was.
 if(!wanted&&changed){table_->setFocus(Qt::OtherFocusReason);QTimer::singleShot(0,this,[this]{if(table_->currentIndex().isValid())table_->scrollTo(table_->currentIndex());});}
 if(changed)emit viewStateChanged();
}
bool HistoryWindow::paneVisible()const{return inspectorScroll_&&!inspectorScroll_->isHidden();}
void HistoryWindow::closeDetails(){if(!grid_->isChecked()&&!compact_)setDetailsWanted(false);else setDetailsOpen(false);}
void HistoryWindow::toggleDetails(){
 const bool overlay=grid_->isChecked()||compact_;const bool selected=!table_->selectionModel()->selectedRows().isEmpty();
 if(overlay&&selected){if(paneVisible())setDetailsOpen(false);else openSelectedDetails();}else setDetailsWanted(!detailsWanted_);
}
bool HistoryWindow::selectionHeader()const{
 // A query hides the pane while it loads and restores the selection. The selection
 // bar never replaces search while the user types in it, or the field would be
 // hidden mid-word and later keys sent elsewhere; it waits until search gives up
 // the keyboard. In the wide list the pane's own rule decides, not its momentary
 // visibility, so the header does not flicker during that reload either.
 if(const auto* search=findChild<QLineEdit*>("historySearch");search&&search->hasFocus())return false;
 const bool pane=grid_->isChecked()||compact_?paneVisible():detailsWanted_;
 return table_->selectionModel()->selectedRows().size()>1&&!pane;
}
QList<HistoryWindow::FilterEntry> HistoryWindow::filterEntries()const{
 const auto& q=model_->query();QList<FilterEntry> entries;
 const auto choice=[this](const char* name,const QString& value){const auto* combo=findChild<QComboBox*>(name);const int index=combo?combo->findData(value):-1;return index>=0?combo->itemText(index):value;};
 const auto date=[](const QString& value){const auto day=value.contains('T')?QDateTime::fromString(value,Qt::ISODateWithMs).toLocalTime().date():QDate::fromString(value,Qt::ISODate);return day.isValid()?QLocale().toString(day,"MMMM d, yyyy"):value;};
 if(!q.from.isEmpty())entries.append({"from","From",date(q.from)});if(!q.to.isEmpty())entries.append({"to","Through",date(q.to)});
 if(!q.profileId.isEmpty())entries.append({"profileId","Destination",choice("historyChoiceprofileId",q.profileId)});
 if(!q.uploadStatus.isEmpty()&&!(categories_->currentRow()==5&&q.uploadStatus=="failed"))entries.append({"uploadStatus","Upload state",choice("historyChoiceuploadStatus",q.uploadStatus)});
 if(!q.localStatus.isEmpty())entries.append({"localStatus","Local copy",choice("historyChoicelocalStatus",q.localStatus)});
 if(!q.remoteStatus.isEmpty()){if(QStringList{"available","deleted","needs_attention"}.contains(q.remoteStatus))entries.append({"remoteStatus","Remote file state",choice("historyRemoteState",q.remoteStatus)});else entries.append({"exactRemoteStatus","Exact remote state",choice("historyChoiceremoteStatus",q.remoteStatus)});}
 if(!q.mime.isEmpty())entries.append({"mime","File type",q.mime});
 if(showCancelled_->isChecked())entries.append({"includeCanceled","Include canceled",{}});
 return entries;
}
// An agent's or a chip's change to one filter, applied as its control would.
void HistoryWindow::setFilter(const QString& key,const QString& value){
 if(key=="includeCanceled"){showCancelled_->setChecked(!value.isEmpty()&&value!="false");return;}
 const auto field=key=="exactRemoteStatus"?QString("remoteStatus"):key;auto* raw=findChild<QLineEdit*>("historyFilter"+field);if(!raw)return;
 auto text=value;if((field=="from"||field=="to")&&QDate::fromString(value,Qt::ISODate).isValid()&&!value.contains('T'))text=QDateTime(QDate::fromString(value,Qt::ISODate),field=="to"?QTime(23,59,59,999):QTime(0,0)).toUTC().toString(Qt::ISODateWithMs);
 raw->setText(text);applyFilters();
}
void HistoryWindow::updateFilterChips(){
 auto* row=static_cast<HistoryChipRow*>(filterChips_);if(!row)return;const auto entries=filterEntries();
 QStringList signature;for(const auto& entry:entries)signature<<entry.key+'='+entry.value;if(row->property("chips").toStringList()==signature&&!entries.isEmpty()==!row->isHidden())return;row->setProperty("chips",signature);
 for(auto* child:row->children())if(auto* old=dynamic_cast<HistoryFilterChip*>(child)){old->hide();old->deleteLater();}
 auto* more=row->findChild<QToolButton*>("historyFilterChipsMore");if(!more){more=Ui::toolButton("+1 more",{},"secondary",row);more->setObjectName("historyFilterChipsMore");more->setToolTip("Show every filter");more->setFixedHeight(Ui::scaled(Ui::Metric::control));connect(more,&QToolButton::clicked,this,[this]{allFilters_->setChecked(true);});}
 if(clearFilters_->parentWidget()!=row){clearFilters_->setParent(row);clearFilters_->setFixedHeight(Ui::scaled(Ui::Metric::control));}
 QList<QWidget*> chips;
 for(const auto& entry:entries){auto* chip=new HistoryFilterChip(entry.label,entry.value,row);chip->setObjectName("historyFilterChip"+entry.key);chip->edit->setObjectName("historyFilterChipEdit"+entry.key);chip->remove->setObjectName("historyFilterChipRemove"+entry.key);
  const auto described=entry.value.isEmpty()?entry.label:entry.label+": "+entry.value;chip->edit->setAccessibleName("Edit filter: "+described);chip->edit->setToolTip(entry.key=="includeCanceled"?QString("Show every filter"):"Change this filter");chip->remove->setAccessibleName("Remove "+entry.label.toLower()+" filter"+(entry.value.isEmpty()?QString{}:": "+entry.value));chip->remove->setToolTip(chip->remove->accessibleName());
  connect(chip->edit,&QToolButton::clicked,this,[this,key=entry.key,chip]{if(key=="includeCanceled"||!editFilter_)allFilters_->setChecked(true);else editFilter_(key,chip);});
  connect(chip->remove,&QToolButton::clicked,this,[this,key=entry.key]{QTimer::singleShot(0,this,[this,key]{setFilter(key,{});if(clearFilters_->isVisible())clearFilters_->setFocus(Qt::OtherFocusReason);});});
  chip->show();chips.append(chip);}
 row->setItems(chips,more,entries.isEmpty()?nullptr:clearFilters_);clearFilters_->setVisible(!entries.isEmpty());row->setVisible(!entries.isEmpty()&&!selectionHeader());
}
void HistoryWindow::updateMultiSummary(){
 const auto rows=table_->selectionModel()->selectedRows();qint64 bytes=0;QStringList destinations;QMap<QString,int> states;const auto files=selectedFiles();
 for(const auto& file:files){bytes+=file["bytes"].toInteger();const auto destination=destinationLabel(file);if(!destinations.contains(destination))destinations.append(destination);++states[file["uploadStatus"].toString()];}
 for(const auto& index:rows){const auto group=model_->record(index.row());if(group["records"].toArray().isEmpty())++states[group["uploadStatus"].toString().isEmpty()?group["operation"].toObject()["phase"].toString():group["uploadStatus"].toString()];}
 destinations.sort();QStringList tally;QJsonObject counts;const auto* choices=findChild<QComboBox*>("historyChoiceuploadStatus");
 for(auto it=states.cbegin();it!=states.cend();++it){const int choice=choices->findData(it.key());const auto name=choice>=0?choices->itemText(choice):it.key().isEmpty()?QString("Not recorded"):it.key();tally.append(QString("%1: %2").arg(name).arg(it.value()));counts[it.key()]=it.value();}
 selectionSummaryState_={{"items",rows.size()},{"files",files.size()},{"bytes",double(bytes)},{"destinations",QJsonArray::fromStringList(destinations)},{"uploadStates",counts}};
 const bool same=rows.size()==files.size();findChild<QLabel*>("historySelectionTitle")->setText(same?QString("%1 selected files").arg(files.size()):QString("Selected items"));
 QList<std::pair<QString,QString>> fields;if(!same){fields.append({"Items",QString::number(rows.size())});fields.append({"Files",QString::number(files.size())});}fields.append({"Total size",HistoryModel::fileSize(bytes)});fields.append({"Destinations",destinations.isEmpty()?QString("No stored files"):destinations.join("\n")});fields.append({"Upload states",tally.join("\n")});static_cast<HistorySummary*>(selectionSummary_)->setFields(fields);
}
void HistoryWindow::prepareCancelAll(){
 QStringList ids;for(const auto& value:model_->activeOperations()){const auto id=value.toObject()["id"].toString();if(!id.isEmpty()&&!ids.contains(id))ids.append(id);}confirmUploads(ids);
}
void HistoryWindow::showFailedUploads(){clearFilters(true);categories_->setCurrentRow(5);}
void HistoryWindow::confirmUploads(const QStringList& ids){
 if(cancelAllConfirmation_&&cancelAllConfirmation_->isVisible()){cancelAllConfirmation_->raise();return;}cancelAllIds_=ids;cancelAllIds_.removeAll(QString{});cancelAllIds_.removeDuplicates();if(cancelAllIds_.isEmpty())return;
 auto* dialog=new QDialog(this);cancelAllConfirmation_=dialog;dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setObjectName("historyCancelAllConfirmation");dialog->setWindowTitle("Cancel all uploads");dialog->setWindowModality(Qt::WindowModal);auto* box=new QVBoxLayout(dialog);box->setContentsMargins(space(Space::xl),space(Space::xl),space(Space::xl),space(Space::xl));box->setSpacing(space(Space::l));auto* title=new QLabel(QString("Cancel all %1 active uploads?").arg(cancelAllIds_.size()),dialog);title->setWordWrap(true);Ui::role(title,"sectionTitle");box->addWidget(title);auto* effect=new QLabel("Stops the uploads active when this confirmation opened. Files already uploaded and local copies stay. New uploads are left running.",dialog);effect->setWordWrap(true);box->addWidget(effect);auto* buttons=new QHBoxLayout;buttons->addStretch();auto* keep=new QPushButton("Keep uploading",dialog);keep->setObjectName("historyKeepUploading");keep->setDefault(true);auto* cancel=new QPushButton(QString("Cancel %1 uploads").arg(cancelAllIds_.size()),dialog);cancel->setObjectName("historyConfirmCancelAll");cancel->setAutoDefault(false);Ui::role(cancel,"danger");buttons->addWidget(keep);buttons->addWidget(cancel);box->addLayout(buttons);connect(keep,&QPushButton::clicked,dialog,&QDialog::reject);connect(cancel,&QPushButton::clicked,this,&HistoryWindow::confirmCancelAll);connect(dialog,&QDialog::finished,this,[this](int){cancelAllIds_.clear();emit viewStateChanged();});Ui::styleWindow(*dialog);dialog->resize(Ui::scaled(480),dialog->sizeHint().height());dialog->open();keep->setFocus();emit viewStateChanged();
}
void HistoryWindow::confirmCancelAll(){
 if(!cancelAllConfirmation_||!cancelAllConfirmation_->isVisible()||cancelAllIds_.isEmpty())return;const auto ids=cancelAllIds_;cancelAllConfirmation_->accept();
 // An upload that finished after the dialog opened answers operation_inactive: it is
 // not a failure, and History shows it finished. Only real cancellations are counted.
 auto remaining=std::make_shared<int>(ids.size());auto requested=std::make_shared<int>(0);auto failures=std::make_shared<QStringList>();for(const auto& id:ids)session_.request("operation.cancel",{{"id",id}},this,[g=QPointer<HistoryWindow>(this),remaining,requested,failures](Error e,QJsonObject){if(!e)++*requested;else if(e.code!="operation_inactive")failures->append(e.message);if(--*remaining||!g)return;Ui::feedback(g->status_,!failures->isEmpty()?QString("Some uploads couldn't be canceled. ")+failures->join(" "):*requested==0?QString("Those uploads had already finished."):*requested==1?QString("Cancellation requested for 1 upload."):QString("Cancellation requested for %1 uploads.").arg(*requested),!failures->isEmpty());g->refresh();});
}
void HistoryWindow::updateSelectionBar(){if(!selectionBar_)return;
 const int rows=table_->selectionModel()->selectedRows().size();const bool several=rows>1;
 const auto files=selectedFiles();const auto links=selectedLinkIds();const int missing=int(files.size()-links.size());
 selectionCount_->setText(QString("%1 selected").arg(rows));selectionCount_->setAccessibleName(rows==files.size()?QString("%1 selected files").arg(rows):QString("%1 selected items · %2 files").arg(rows).arg(files.size()));
 // What the selection holds, from the same tallies the details pane shows.
 {QStringList note;if(files.size()!=rows)note<<QString("%1 files").arg(files.size());note<<HistoryModel::fileSize(selectionSummaryState_["bytes"].toInteger());const auto states=selectionSummaryState_["uploadStates"].toObject();
  if(states.size()==1&&states.contains("uploaded"))note<<"all uploaded";else if(!states.isEmpty()){const auto* choices=findChild<QComboBox*>("historyChoiceuploadStatus");QStringList tally;for(auto it=states.begin();it!=states.end();++it){const int choice=choices->findData(it.key());tally<<QString("%1 %2").arg(it.value().toInt()).arg((choice>=0?choices->itemText(choice):it.key().isEmpty()?QString("not recorded"):it.key()).toLower());}note<<tally.join(", ");}
  if(!model_->query().search.isEmpty())note<<"in results for “"+model_->query().search+"”";selectionNote_->setText(note.join(" · "));selectionNote_->setToolTip(note.join(" · "));}
 findChild<QAction*>("historyShowDetails")->setEnabled(rows>=1);copyLinks_->setVisible(several&&!links.isEmpty());copyLinks_->setText(QString("Copy %1 %2").arg(links.size()).arg(links.size()==1?"link":"links"));copyLinks_->setAccessibleName(copyLinks_->text());
 selectionLinkHint_->setText(QString("%1 selected %2 %3 no link").arg(missing).arg(rows==files.size()?(missing==1?"item":"items"):(missing==1?"file":"files")).arg(missing==1?"has":"have"));selectionLinkHint_->setVisible(several&&!links.isEmpty()&&missing>0);
 const auto deletable=several?deletableIds():QStringList{};const int fileCount=several?int(files.size()):0;deleteSelected_->setVisible(!deletable.isEmpty());
 deleteSelected_->setToolTip(deletable.size()==fileCount?QString("Delete the %1 selected files from their original destinations. Local copies and history records remain.").arg(fileCount):QString("Delete %1 of the %2 selected files from their original destinations; the others have no confirmed upload to delete.").arg(deletable.size()).arg(fileCount));
 const bool inInspector=several&&!inspectorScroll_->isHidden();auto* more=findChild<QToolButton*>("historySelectionMore");auto* selectionHeading=qobject_cast<QHBoxLayout*>(selectionInspector_->layout()->itemAt(0)->layout());auto* bar=static_cast<QBoxLayout*>(selectionBar_->layout());
 if(inInspector){if(more->parentWidget()!=selectionInspector_)selectionHeading->insertWidget(1,more);}else if(more->parentWidget()!=selectionBar_)bar->insertWidget(bar->indexOf(selectionClear_),more);more->setVisible(several);
 // With the pane closed the bar takes the header's place (P7); with it open, the pane's.
 auto* header=findChild<QWidget*>("historyCompactControls");auto* owner=inInspector?selectionInspector_:header;
 if(selectionBar_->parentWidget()!=owner){if(inInspector)static_cast<QVBoxLayout*>(owner->layout())->insertWidget(2,selectionBar_);else selectionBar_->setParent(owner);}
 bar->setDirection(inInspector?QBoxLayout::TopToBottom:QBoxLayout::LeftToRight);bar->setContentsMargins(0,0,0,0);bar->setSpacing(inInspector?space(Space::s):space(Space::m));
 for(auto* button:{copyLinks_,deleteSelected_}){button->ensurePolished();button->setMinimumWidth(0);button->setMaximumWidth(QWIDGETSIZE_MAX);if(inInspector)button->setFixedWidth(qMax(button->sizeHint().width(),Ui::scaled(240)));bar->setAlignment(button,inInspector?Qt::AlignLeft:Qt::Alignment{});}
 for(int i=0;i<bar->count();++i)if(auto* spacer=bar->itemAt(i)->spacerItem()){spacer->changeSize(0,0,inInspector?QSizePolicy::Fixed:QSizePolicy::Expanding,QSizePolicy::Fixed);bar->setStretch(i,inInspector?0:1);}
 selectionBar_->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Maximum);for(auto* widget:std::initializer_list<QWidget*>{selectionCount_,selectionNote_,selectionClear_})widget->setVisible(several&&!inInspector);
 // Too narrow for the bar's buttons: Delete folds into ⋯ first; Copy links stays longest.
 auto* foldedDelete=findChild<QAction*>("historySelectionMenuDelete");auto* foldedCopy=findChild<QAction*>("historySelectionMenuCopyLinks");foldedDelete->setVisible(false);foldedCopy->setVisible(false);
 if(several&&!inInspector){const int available=header->width()-space(Space::xxl)-space(Space::l)-findChild<QWidget*>("historyViewMode")->sizeHint().width()-detailsToggle_->width()-3*space(Space::l);
  auto natural=[&]{int width=0;for(auto* widget:std::initializer_list<QWidget*>{selectionCount_,copyLinks_,selectionLinkHint_,deleteSelected_,more,selectionClear_})if(!widget->isHidden())width+=widget->sizeHint().width()+bar->spacing();return width+Ui::scaled(120);};
  if(!deleteSelected_->isHidden()&&natural()>available){deleteSelected_->hide();foldedDelete->setVisible(true);}
  if(!selectionLinkHint_->isHidden()&&natural()>available)selectionLinkHint_->hide();
  if(!copyLinks_->isHidden()&&natural()>available){copyLinks_->hide();foldedCopy->setText(copyLinks_->text());foldedCopy->setVisible(true);}}
 bar->invalidate();selectionBar_->setAccessibleDescription(heading_->text()+(count_->text().isEmpty()?QString{}:" · "+count_->text()));selectionBar_->setVisible(several&&(inInspector||selectionHeader()));updateFooterLayout();updateFilterChips();table_->viewport()->update();}
bool HistoryWindow::eventFilter(QObject* object,QEvent* event){
 // The selection bar takes the header once search gives up the keyboard. A window
 // switch or a popup of its own leaves search in use.
 if(event->type()==QEvent::FocusOut&&object->objectName()=="historySearch"){const auto reason=static_cast<QFocusEvent*>(event)->reason();if(reason!=Qt::ActiveWindowFocusReason&&reason!=Qt::PopupFocusReason)QTimer::singleShot(0,this,&HistoryWindow::updateSelectionBar);}
 if((object==status_||object==savedCopy_)&&(event->type()==QEvent::ShowToParent||event->type()==QEvent::HideToParent||event->type()==QEvent::LayoutRequest))QTimer::singleShot(0,this,&HistoryWindow::updateFooterLayout);if(object==split_&&event->type()==QEvent::Resize){QTimer::singleShot(0,this,&HistoryWindow::updateInspectorLayout);if(fitFilters_)QTimer::singleShot(0,this,fitFilters_);}if(object->objectName()=="historyFiltersScroll"&&event->type()==QEvent::Resize&&fitFilters_)QTimer::singleShot(0,this,fitFilters_);return RetainedWindow::eventFilter(object,event);}
void HistoryWindow::resizeEvent(QResizeEvent* event){RetainedWindow::resizeEvent(event);updateInspectorLayout();if(fitFilters_)QTimer::singleShot(0,this,fitFilters_);if(activeBanner_)updateActivityLayout();}
void HistoryWindow::refresh(){if(!session_.ready()){awaitingInitialization_=true;Ui::feedback(status_,"Waiting for the resident journal to initialize.");return;}model_->refresh();}
QStringList HistoryWindow::selectedIds()const{QStringList ids;for(auto index:table_->selectionModel()->selectedRows()){auto r=model_->record(index.row());const auto records=r["records"].toArray();if(records.isEmpty())ids.append("operation:"+r["operationId"].toString());else for(auto v:records)ids.append(v.toObject()["id"].toString());}return ids;}
QJsonObject HistoryWindow::selectedRecord()const{if(model_->busy())return {};const auto rows=table_->selectionModel()->selectedRows();if(rows.size()!=1)return {};const auto group=model_->record(rows.first().row());for(const auto& v:group["records"].toArray())if(v.toObject()["id"]==items_->currentData().toString())return v.toObject();return group["records"].toArray().isEmpty()?QJsonObject{}:group["records"].toArray().first().toObject();}
QList<QJsonObject> HistoryWindow::selectedFiles()const{if(model_->busy())return {};QList<QJsonObject> files;auto rows=table_->selectionModel()->selectedRows();std::sort(rows.begin(),rows.end(),[](const QModelIndex& a,const QModelIndex& b){return a.row()<b.row();});for(const auto& index:rows)for(const auto& v:model_->record(index.row())["records"].toArray())files.append(v.toObject());return files;}
QStringList HistoryWindow::deletableIds()const{QStringList ids;for(const auto& file:selectedFiles())if(HistoryModel::remoteDeletable(file))ids.append(file["id"].toString());return ids;}
QStringList HistoryWindow::selectedLinkIds()const{QStringList ids;for(const auto& file:selectedFiles())if(file["linkAvailable"].toBool()||file["linkRefreshable"].toBool())ids.append(file["id"].toString());return ids;}
bool HistoryWindow::selectionHasLinks()const{return !selectedLinkIds().isEmpty();}
void HistoryWindow::copySelectedLinks(){
 const auto ids=selectedLinkIds();if(ids.isEmpty())return;
 session_.request("history.copyLinks",{{"ids",QJsonArray::fromStringList(ids)}},this,[g=QPointer<HistoryWindow>(this),count=ids.size()](Error e,QJsonObject){if(g)Ui::feedback(g->status_,e?e.message:count==1?QString("Stored link copied."):QString("%1 stored links copied, one per line.").arg(count),bool(e));});
}
// The applied query's advanced restrictions, by their field labels.
QStringList HistoryWindow::activeFilters()const{
 const auto& q=model_->query();QStringList active;
 for(const auto& [value,label]:QList<std::pair<QString,QString>>{{q.from,"From date"},{q.to,"To date"},{q.mime,"File type"},{q.profileId,"Destination ID"},{q.uploadStatus,"Upload state"},{q.localStatus,"Local-copy state"},{q.remoteStatus,"Remote-deletion state"}})if(!value.isEmpty()&&!(label=="Upload state"&&categories_->currentRow()==5&&value=="failed")){QString text=value;QString key;if(label=="Destination ID")key="profileId";else if(label=="Upload state")key="uploadStatus";else if(label=="Local-copy state")key="localStatus";else if(label=="Remote-deletion state")key="remoteStatus";if(!key.isEmpty()){const auto* combo=key=="remoteStatus"&&QStringList{"available","deleted","needs_attention"}.contains(value)?findChild<QComboBox*>("historyRemoteState"):findChild<QComboBox*>("historyChoice"+key);const int index=combo->findData(value);if(index>=0)text=combo->itemText(index);}else if(label=="From date"||label=="To date"){const auto date=value.contains('T')?QDateTime::fromString(value,Qt::ISODateWithMs).toLocalTime().date():QDate::fromString(value,Qt::ISODate);if(date.isValid())text=QLocale().toString(date,QLocale::LongFormat);}active.append((label=="Destination ID"?QString("Destination"):label)+": "+text);}
 if(showCancelled_->isChecked())active.append("Include canceled");
 return active;
}
void HistoryWindow::updateFilterSummary(){if(!filtersToggle_)return;
 const auto active=activeFilters();const auto state=allFilters_&&allFilters_->isChecked()?QString("Every filter shown"):QString("Filter menu");
 filtersToggle_->setAccessibleDescription(active.isEmpty()?state+", no active filters":state+QString(", %1 active: ").arg(active.size())+active.join(", "));filtersToggle_->setToolTip(active.isEmpty()?QString("Add a filter"):"Add a filter\nActive filters:\n"+active.join("\n"));
 clearFilters_->setVisible(!active.isEmpty());clearFilters_->setAccessibleDescription(active.join(", "));updateFilterChips();if(model_&&!model_->busy())updateCountNote();updateFooterLayout();
}
// Clear filters empties the advanced fields; the empty state's reset (all)
// also clears the search and shows every category.
void HistoryWindow::clearFilters(bool all){
 showCancelled_->setChecked(false);
 for(const auto& key:QStringList{"from","to","mime","profileId","uploadStatus","localStatus","remoteStatus"})findChild<QLineEdit*>("historyFilter"+key)->clear();
 for(auto* cell:findChild<QWidget*>("historyFiltersPanel")->findChildren<QWidget*>())if(cell->property("filterKey").isValid())cell->setProperty("filterRevealed",false);if(fitFilters_)fitFilters_();
 if(!all&&categories_->currentRow()==5)findChild<QLineEdit*>("historyFilteruploadStatus")->setText("failed");
 if(all){findChild<QLineEdit*>("historySearch")->clear();restoringCategory_=true;categories_->setCurrentRow(0);restoringCategory_=false;}
 const bool focus=clearFilters_->hasFocus()||resetView_->hasFocus();applyFilters();if(focus)(all?static_cast<QWidget*>(table_):filtersToggle_)->setFocus(Qt::OtherFocusReason);
}
QString HistoryWindow::destinationLabel(const QJsonObject& r)const{
 if(r["uploadStatus"]=="local_only")return "This device";
 auto destination=r["receipt"].toObject()["destination"].toObject();if(destination["providerId"].toString().isEmpty())destination=r["operation"].toObject()["destination"].toObject();
 const auto profiles=session_.document()["profiles"].toArray();if(destination["providerId"].toString().isEmpty())for(const auto& value:profiles){const auto profile=value.toObject();if(profile["id"]==r["profileId"]){destination={{"providerId",profile["provider"]},{"profileId",profile["id"]},{"settings",profile["settings"]}};break;}}
 if(destination["providerId"].toString().isEmpty())return "Original destination";return originalDestination({{"destination",destination}},profiles);
}
// #1361: shown while the selected upload's outcome is unknown at a destination that cannot be
// asked whether it arrived; cleared when that stops being true of the selection.
static const QString& resendNotice(){static const QString text="This upload may already be on the server: its outcome is unknown, and this destination cannot be asked. Retry upload sends the file again and may leave a second copy there.";return text;}
void HistoryWindow::selectionChanged(){emit viewStateChanged();const auto rows=table_->selectionModel()->selectedRows();
 // The instruction shows only while it applies; feedback stays until replaced.
 {static const QString instruction="Select an item to inspect its files and share link.";if(!rows.isEmpty()||!model_->rowCount()){if(status_->text()==instruction)Ui::feedback(status_,{});}else if(status_->text().isEmpty()&&Ui::scaled(100)<=125)Ui::feedback(status_,instruction);}{QSignalBlocker block(items_);items_->clear();if(rows.size()==1){const auto group=model_->record(rows.first().row());for(auto v:group["records"].toArray()){auto r=v.toObject();items_->addItem(r["name"].toString(),r["id"].toString());}const auto itemIndex=items_->findData(selectedItemId_);if(itemIndex>=0)items_->setCurrentIndex(itemIndex);items_->setVisible(items_->count()>1);}}loadSelectedItem();updateInspectorLayout();}
void HistoryWindow::loadSelectedItem(){
 if(auto next=std::exchange(afterDetail_,{}))next(false);
 // History reloads the selection whenever the resident reports a change.
 // The item it already shows keeps its preview and size until the reload
 // brings a different preview, so the inspector never flashes empty.
 const auto next=selectedRecord()["id"].toString();const bool keepPreview=!next.isEmpty()&&next==shownItemId_;
 if(!keepPreview){if(previewCancelled_)previewCancelled_->store(true);shownItemId_.clear();shownPreviewPath_.clear();pixelDimensions_.clear();static_cast<HistoryPreview*>(preview_)->clearImage();if(expandedPreview_)expandedPreview_->close();}
 pendingPreviewPath_.clear();const auto seq=++selectionRequest_;currentDetail_={};if(unlockLinkItemId_!=next){unlockLinkItemId_.clear();if(auto* unlock=findChild<QPushButton*>("historyUnlockLinkCredentials")){unlock->hide();unlock->setEnabled(true);}}previewStatus_->hide();retryPreview_->hide();details_->clear();static_cast<HistorySummary*>(summary_)->setFields({});capabilityHint_->hide();changeLocalCopies_->hide();title_->clear();outcome_->clear();outcome_->show();link_->clear();link_->hide();link_->parentWidget()->hide();previousUrl_->clear();previousLink_->hide();browserFeedback_->clear();browserFeedback_->hide();linkExpiry_->clear();linkExpiry_->hide();Ui::role(copy_,"primary");for(auto* b:{copy_,open_,copyImage_,saveAs_,editAgain_,copyText_,showQr_}){b->setEnabled(false);b->hide();}for(auto* b:{share_,revoke_,inspectLink_,retry_,cancel_,saveDiagnostics_})b->hide();
 const auto rows=table_->selectionModel()->selectedRows();remove_->hide();deletionStatus_->hide();deletionRetry_->hide();deletionCancel_->hide();selectedDeletion_={};updateActions();if(rows.size()!=1){if(status_->text()==resendNotice())Ui::feedback(status_,{});updateMultiSummary();return;}const auto group=model_->record(rows.first().row());title_->setText(rows.first().data().toString());setHistoryOutcome(outcome_,group);retry_->setText(HistoryModel::retryUpload(group)?"Retry upload":"Retry local save");retry_->setAccessibleName(retry_->text());saveDiagnostics_->setVisible(diagnosticsAvailable(rows.first().row()));retry_->setVisible(HistoryModel::retryUpload(group)||HistoryModel::retryLocalSave(group));cancel_->setVisible(model_->data(rows.first(),HistoryModel::ActiveRole).toBool());
 updateActions();const auto record=selectedRecord();if(record.isEmpty()){static_cast<HistorySummary*>(summary_)->setFields({{"Capture","File details appear after acquisition."}});return;}selectedItemId_=record["id"].toString();
 session_.request("history.detail",{{"id",record["id"]}},this,[g=QPointer<HistoryWindow>(this),seq](Error e,QJsonObject data){if(!g||g->selectionRequest_!=seq)return;if(e){Ui::feedback(g->status_,e.message,true);if(auto next=std::exchange(g->afterDetail_,{}))next(false);return;}g->currentDetail_=data;g->deletionNames_[data["id"].toString()]=data["name"].toString();const auto url=data["receipt"].toObject()["directUrl"].toString();const bool available=data["linkAvailable"].toBool();g->previousUrl_->setText(!available?url:QString{});g->previousUrl_->setCursorPosition(0);g->previousUrl_->setToolTip(g->previousUrl_->text());g->previousLink_->setVisible(!g->previousUrl_->text().isEmpty()&&g->findChild<QToolButton*>("historyReceiptExpanded")->isChecked());g->details_->setPlainText(QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Indented)));g->title_->setText(g->items_->count()>1?data["name"].toString():g->table_->currentIndex().data().toString());g->title_->setToolTip(data["name"].toString());g->title_->setAccessibleName(data["name"].toString());g->link_->setText(data["receipt"].toObject()["directUrl"].toString());g->link_->setCursorPosition(0);g->link_->setToolTip(g->link_->text());g->linkExpiry_->setText(linkExpiry(data));g->linkExpiry_->setVisible(!g->linkExpiry_->text().isEmpty());auto local=data["local"].toObject();const auto localPath=local["path"].toString();const bool localAvailable=local["status"]=="saved"&&QFileInfo(localPath).isFile();if(g->items_->count()==1){auto observed=data;observed["operation"]=g->model_->record(g->table_->currentIndex().row())["operation"];setHistoryOutcome(g->outcome_,observed);}g->saveAs_->setEnabled(localAvailable&&g->copyJob_["status"]!="saving");g->copyImage_->setEnabled(localAvailable&&data["mime"].toString().startsWith("image/")&&data["bytes"].toInteger()<=32*1024*1024);g->saveAs_->setToolTip(localAvailable?"Save the retained local original to another file":"A retained local original is needed to save another copy");g->copyImage_->setToolTip(g->copyImage_->isEnabled()?"Copy the retained original image":"A retained local image is needed to copy image bytes");g->editAgain_->setEnabled(editable(data));g->editAgain_->setToolTip(g->editAgain_->isEnabled()?"Opens a new edit of the saved image; previous marks are flattened":data["mime"]=="image/png"||data["mime"]=="image/jpeg"?"Editing needs the local copy. Turn on Keep a local copy in Settings → Capture for future captures.":"Only PNG and JPEG images can be edited");if(auto next=std::exchange(g->afterDetail_,{}))next(true);
 const bool sharing=data["capabilities"].toObject()["sharing"].toBool();for(auto* b:{g->share_,g->revoke_,g->inspectLink_})b->setVisible(sharing);const auto linkState=data["shareStatus"].toString();const bool stopped=g->sharingStopped_&&g->sharingId_==data["shareOperationId"].toString();const bool busy=g->sharingAccepting_||g->sharingTimer_->isActive()||(linkState=="pending"&&!stopped);g->share_->setEnabled(!busy&&linkState!="pending"&&linkState!="unknown"&&linkState!="changed");g->revoke_->setEnabled(!busy&&data["linkAvailable"].toBool());g->inspectLink_->setEnabled(!busy);const bool localResult=data["uploadStatus"]=="local_only"&&localAvailable;g->copy_->setText(localResult?"Copy result":data["linkRefreshable"].toBool()?"Refresh and copy link":"Copy link");g->copy_->setAccessibleName(g->copy_->text());g->copy_->setEnabled(data["linkAvailable"].toBool()||data["linkRefreshable"].toBool()||localResult);g->open_->setEnabled(data["linkAvailable"].toBool());
 // Actions come from capabilities and retained bytes: one that cannot apply
 // to this item is not shown, and a local-only result has no link field.
 for(auto* b:{g->copy_,g->open_,g->editAgain_})b->setVisible(b->isEnabled());
 // Copy text and Show QR code stay in view when they cannot apply, disabled with the reason.
 {const auto noText=copyTextUnavailable(data),noQr=qrUnavailable(data);const bool image=data["mime"].toString().startsWith("image/");const bool uploaded=data["uploadStatus"]=="uploaded";
  g->copyText_->setVisible(image);g->copyText_->setEnabled(noText.isEmpty()&&!g->textPoll_->isActive());g->copyText_->setToolTip(noText.isEmpty()?"Copy the text in this image. Nothing is saved or uploaded.":noText);
  g->showQr_->setVisible(uploaded);g->showQr_->setEnabled(noQr.isEmpty());g->showQr_->setToolTip(noQr.isEmpty()?"Show the link as a QR code":noQr);}auto observed=data;observed["operation"]=g->model_->record(g->table_->currentIndex().row())["operation"];g->capabilityHint_->setText(missingLocalCopyHint(observed));g->outcome_->setVisible(localAvailable);g->capabilityHint_->setVisible(!localAvailable);g->changeLocalCopies_->setVisible(!localAvailable);g->link_->setVisible(data["linkAvailable"].toBool());g->link_->parentWidget()->setVisible(g->copy_->isEnabled()||data["linkAvailable"].toBool());g->updateSummary();g->inspectSelectedDeletion();g->updateActions();
 // Remote deletion needs a settled confirmed upload and a provider that can delete exactly that target.
 {const auto caps=data["capabilities"].toObject();g->remove_->setVisible(HistoryModel::remoteDeletable(data)&&caps["delete"].toBool()&&(caps["referenceDelete"].toBool()||(caps["stat"].toBool()&&(caps["conditionalDelete"].toBool()||caps["versionAddressing"].toBool())))&&!caps["deletionEffect"].toString().isEmpty());
  // #1361: an upload that came back without a deletion URL says why it cannot be deleted here.
  if(HistoryModel::remoteDeletable(data)&&!caps["delete"].toBool()&&!caps["removeUnavailable"].toString().isEmpty()){g->deletionStatus_->setText(caps["removeUnavailable"].toString());g->deletionStatus_->show();}}
 if(data["superseded"].toBool()||data["remoteStatus"]=="superseded")Ui::feedback(g->status_,"Superseded: this stored link may serve replacement content. Deletion still targets the original receipt.",true);
 // #1361: a server that names its own uploads cannot be asked whether one arrived.
 else if(data["uploadStatus"]=="unknown"&&data["capabilities"].toObject()["serverNamesObjects"].toBool())Ui::feedback(g->status_,resendNotice(),true);else{if(g->status_->text()==resendNotice())Ui::feedback(g->status_,{});if(!g->sharingFeedback_.isEmpty())Ui::feedback(g->status_,g->sharingFeedback_,g->sharingFeedbackError_);}const auto previewPath=data["previewPath"].toString();
  // Cache replies use artifacts/../previews; older detail snapshots can use previews.
  // Equivalent spellings must not clear the image while a redundant read is queued.
  if(g->shownItemId_!=data["id"].toString()||QDir::cleanPath(g->shownPreviewPath_)!=QDir::cleanPath(previewPath)||!QFileInfo(previewPath).isFile()){g->shownItemId_.clear();g->shownPreviewPath_.clear();g->updatePreviewState();g->pendingPreviewPath_=previewPath;g->startPreview();}
  // History reloads the same selection whenever the resident reports a change;
  // only a different item restarts the wait.
  if(g->previewMissing(data)&&(!g->previewRequestTimer_->isActive()||g->previewCandidate_!=data["id"].toString())){g->previewCandidate_=data["id"].toString();g->previewRequestTimer_->start();}});
}
void HistoryWindow::updateActivityLayout(){
 auto* grid=static_cast<QGridLayout*>(activeBanner_->layout());auto* more=activeBanner_->findChild<QPushButton*>("historyMoreActive");auto* cancel=activeBanner_->findChild<QPushButton*>("cancelBannerOperation");auto* all=activeBanner_->findChild<QPushButton*>("cancelAllBannerOperations");const int buttons=cancel->sizeHint().width()+(all->isHidden()?0:all->sizeHint().width()+10),others=buttons+(more->isHidden()?0:more->sizeHint().width()+10)+qMin(180,progress_->sizeHint().width())+40;const int available=qMax(0,activeBanner_->width()-34);const bool narrow=available<others+Ui::scaled(180),stacked=available<buttons+(more->isHidden()?0:more->sizeHint().width()+20);
 while(auto* item=grid->takeAt(0))delete item;
 if(!narrow){grid->addWidget(activeLabel_,0,0);grid->addWidget(progress_,0,1);grid->addWidget(more,0,2);grid->addWidget(cancel,0,3);grid->addWidget(all,0,4);}
 else{grid->addWidget(activeLabel_,0,0,1,4);grid->addWidget(progress_,0,4);grid->addWidget(more,1,0,1,stacked?5:3,Qt::AlignLeft);grid->addWidget(cancel,stacked?2:1,3);grid->addWidget(all,stacked?2:1,4);}
}
void HistoryWindow::updateActivity(){
 activeId_.clear();QStringList labels;int firstPercent=-1;
 for(const auto& value:model_->activeOperations()){const auto op=value.toObject();const auto items=op["items"].toArray();QString name=items.isEmpty()?HistoryModel::friendlyAction(op["action"].toString())+" capture":items.first().toObject()["artifact"].toObject()["name"].toString();if(items.size()>1)name+=QString(" + %1 more").arg(items.size()-1);const int percent=HistoryModel::operationProgress(op);auto state=HistoryModel::statusText({{"operation",op}});if(percent>=0&&!state.contains(QString::number(percent)+"%"))state+=QString(" · %1%").arg(percent);labels<<name+" · "+state;if(activeId_.isEmpty()){activeId_=op["id"].toString();firstPercent=percent;}}
 activeLabel_->setText(labels.value(0));activeLabel_->setToolTip(labels.value(0));activeLabel_->setAccessibleDescription(labels.value(0));progress_->setRange(0,firstPercent<0?0:100);progress_->setValue(qMax(0,firstPercent));progress_->setAccessibleDescription(labels.value(0));auto* cancel=activeBanner_->findChild<QPushButton*>("cancelBannerOperation");cancel->setText("Cancel this upload");cancel->setToolTip("Cancel "+labels.value(0));cancel->setAccessibleName("Cancel this upload: "+labels.value(0));auto* more=activeBanner_->findChild<QPushButton*>("historyMoreActive");more->setText(QString("%1 more active").arg(qMax(0,labels.size()-1)));more->setVisible(labels.size()>1);more->setAccessibleDescription("Show all active operation rows in History");auto* all=activeBanner_->findChild<QPushButton*>("cancelAllBannerOperations");all->setText(QString("Cancel all (%1)…").arg(labels.size()));all->setAccessibleName(all->text());all->setVisible(labels.size()>1);activeBanner_->setVisible(!activeId_.isEmpty());updateActivityLayout();
}
void HistoryWindow::updateActions(){
 while(auto* item=actions_->takeAt(0))delete item;
 // While a selection loads every action is hidden. Keep the flush edge until
 // one shows: the inspector's width sums these hints, and the list beside it
 // would otherwise reflow twice on every selection change (#1064).
 auto* more=findChild<QToolButton*>("historyInspectorMore");more->setVisible(copyImage_->isEnabled()||saveAs_->isEnabled());
 const QList<QWidget*> row{open_,showQr_,editAgain_,copyText_,saveDiagnostics_,more};const bool shown=std::any_of(row.begin(),row.end(),[](const QWidget* widget){return !widget->isHidden();});
 bool first=true;for(auto* widget:row){const bool flush=first&&!widget->isHidden();if(shown&&widget->property("uiFlush").toBool()!=flush){widget->setProperty("uiFlush",flush);QEvent changed(QEvent::StyleChange);QCoreApplication::sendEvent(widget,&changed);widget->updateGeometry();widget->update();}if(!widget->isHidden())first=false;}browserAction_->setVisible(!first);int position=0;if(!browserAction_->isHidden())actions_->addWidget(browserAction_,position++,0);
 for(auto* button:{share_,revoke_,inspectLink_,cancel_})if(!button->isHidden())actions_->addWidget(button,position++,0,Qt::AlignLeft);
 findChild<QWidget*>("historyLinkRule")->setVisible(!link_->parentWidget()->isHidden());updateInspectorLayout();
}
void HistoryWindow::openBrowser(const QString& url){
 browserFeedback_->hide();const auto selection=selectionRequest_;host::openLink(QUrl(url),[g=QPointer<HistoryWindow>(this),selection](const QJsonObject& result){if(!g||g->selectionRequest_!=selection)return;if(!result["opened"].toBool()){g->browserFeedback_->setText("Couldn't open the browser. Copy the link instead");g->browserFeedback_->show();if(g->inspectorScroll_->isHidden())Ui::feedback(g->status_,g->browserFeedback_->text(),true);}});
}
void HistoryWindow::operationAction(const QString& method){if(model_->busy())return;const auto rows=table_->selectionModel()->selectedRows();if(rows.size()!=1)return;const auto id=model_->record(rows.first().row())["operationId"].toString();if(id.isEmpty())return;session_.request(method,{{"id",id}},this,[g=QPointer<HistoryWindow>(this),method](Error e,QJsonObject result){if(g&&method=="operation.cancel"&&e.code=="operation_inactive"){Ui::feedback(g->status_,"That upload had already finished.",false);g->refresh();return;}if(g){const auto message=method=="operation.cancel"?QString("Cancellation requested"):method=="operation.retry"?QString("Upload retry requested"):method=="operation.retryLocalSave"?QString("Local save retry requested"):result["copyStatus"]=="copied"?QString("Copied to clipboard"):QString("Nothing was copied to the clipboard");const bool copyFailed=method=="operation.copy"&&result["copyStatus"]!="copied";const auto reason=result["error"].toObject()["message"].toString();Ui::feedback(g->status_,e?e.message:message+(copyFailed&&!reason.isEmpty()?": "+reason:QString{}),bool(e)||copyFailed);g->refresh();}});}
void HistoryWindow::copyHistoryLink(const QJsonObject& record){
 // A row owns its copy request even while its inspector detail is still loading.
 const auto id=record["id"].toString();if(id.isEmpty())return;
 const bool keychain=record["receipt"].toObject()["destination"].toObject()["credentials"].toObject()["mode"]=="keychain";
 session_.request("history.copyLink",{{"id",id}},this,[g=QPointer<HistoryWindow>(this),id,keychain](Error e,QJsonObject result){
  if(!g||g->selectedRecord()["id"]!=id)return;
  const bool needsUnlock=e.code=="credential_setup_required"&&keychain;
  g->unlockLinkItemId_=needsUnlock?id:QString{};
  if(auto* unlock=g->findChild<QPushButton*>("historyUnlockLinkCredentials")){unlock->setEnabled(true);unlock->setVisible(needsUnlock);}
  Ui::feedback(g->status_,e?e.message:result["copied"].toBool()?"Stored link copied.":"The link was not copied.",bool(e));
  if(needsUnlock)g->openSelectedDetails();else if(!e)g->loadSelectedItem();
 });
}
void HistoryWindow::primaryAction(const QModelIndex& i){if(model_->busy())return;table_->selectionModel()->select(i,QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);table_->setCurrentIndex(i);const auto action=i.data(HistoryModel::PrimaryActionRole).toString();if(action=="Cancel upload")operationAction("operation.cancel");else if(action=="Retry upload")operationAction("operation.retry");else if(action=="Retry local save")operationAction("operation.retryLocalSave");else if(action=="Retry deletion"){const auto id=model_->record(i.row())["deleteOperationId"].toString();session_.request("delete.retry",{{"id",id}},this,[g=QPointer<HistoryWindow>(this),id](Error e,QJsonObject){if(!g)return;Ui::feedback(g->status_,e?e.message:"Deletion retry requested.",bool(e));if(!e){g->deletionId_=id;g->deletionTimer_->start();}g->refresh();});}else if(action=="Copy result")operationAction("operation.copy");else if(action=="Copy link"){const auto group=model_->record(i.row());if(group["records"].toArray().size()>1&&!group["operation"].toObject().isEmpty())copySelectedLinks();else {const auto files=group["records"].toArray();if(files.isEmpty())return;copyHistoryLink(files.first().toObject());}}}
bool HistoryWindow::editable(const QJsonObject& record){
 const auto local=record["local"].toObject();
 // A batch row holds several files; its inspector picks one to edit.
 if(record["records"].toArray().size()>1)return false;
 return (record["mime"]=="image/png"||record["mime"]=="image/jpeg")&&local["status"]=="saved"&&QFileInfo(local["path"].toString()).isFile();
}
QString HistoryWindow::copyTextUnavailable(const QJsonObject& record){
 if(record["mime"]!="image/png"&&record["mime"]!="image/jpeg")return "Copy text reads PNG and JPEG images";
 // The same rule as Edit again: put.gd never downloads an upload to read it.
 if(!editable(record))return "Copy text needs the local copy. Turn on Keep a local copy in Settings → Capture for future captures.";
 return {};
}
QString HistoryWindow::qrUnavailable(const QJsonObject& record){return qr::offered(record)?QString():qr::unavailable(record).message;}
void HistoryWindow::copyText(const QJsonObject& record){
 if(const auto reason=copyTextUnavailable(record);!reason.isEmpty()){Ui::feedback(status_,reason,true);return;}
 if(textPoll_->isActive()){Ui::feedback(status_,"Text recognition is still running",true);return;}
 copyText_->setEnabled(false);Ui::feedback(status_,"Reading text…");
 session_.request("history.copyText",{{"id",record["id"]}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject result){
  if(!g)return;
  if(e){Ui::feedback(g->status_,e.code=="text_busy"?QString("Text recognition is still running"):e.message,true);g->copyText_->setEnabled(copyTextUnavailable(g->currentDetail_).isEmpty());return;}
  g->textOperation_=result["operationId"].toString();g->textPoll_->start();
 });
}
// The outcome in the status line, from text.inspect's record: counts and the
// copy outcome only, never the text (the resident's notification says the same).
void HistoryWindow::pollText(){
 if(textOperation_.isEmpty()){textPoll_->stop();return;}
 session_.request("text.inspect",{{"id",textOperation_}},this,[g=QPointer<HistoryWindow>(this),id=textOperation_](Error e,QJsonObject record){
  if(!g||g->textOperation_!=id)return;
  const auto state=record["state"].toString();
  if(!e&&!QStringList{"completed","cancelled","failed","interrupted"}.contains(state))return;
  g->textPoll_->stop();g->textOperation_.clear();g->copyText_->setEnabled(copyTextUnavailable(g->currentDetail_).isEmpty());
  if(e){Ui::feedback(g->status_,e.message,true);return;}
  if(state=="cancelled"){Ui::feedback(g->status_,"Text recognition was cancelled.");return;}
  if(state!="completed"){Ui::feedback(g->status_,record["error"].toObject()["message"].toString("Text recognition failed"),true);return;}
  const auto characters=record["characterCount"].toInt(),codes=record["codeCount"].toInt();const auto copied=record["copyStatus"].toString();
  if(characters==0&&codes==0){Ui::feedback(g->status_,"No text found");return;}
  if(copied=="suppressed"){Ui::feedback(g->status_,"Text not copied: the clipboard changed",true);return;}
  if(copied!="copied"){Ui::feedback(g->status_,"Text could not be copied",true);return;}
  QStringList parts;if(characters)parts<<QLocale().toString(characters)+(characters==1?" character":" characters");if(codes)parts<<QString::number(codes)+(codes==1?" code":" codes");
  Ui::feedback(g->status_,"Copied "+parts.join(" and ")+(record["truncated"].toBool()?". The text was cut at a size or time limit.":""));
 });
}
void HistoryWindow::requestQrCode(const QJsonObject& record){
 if(const auto reason=qrUnavailable(record);!reason.isEmpty()){Ui::feedback(status_,reason,true);return;}
 // The resident refreshes an expired link first, as Refresh and copy link does,
 // then opens the window through showQrCode.
 showQr_->setEnabled(false);
 session_.request("history.qr",{{"id",record["id"]},{"mode","show"}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){
  if(!g)return;g->showQr_->setEnabled(qrUnavailable(g->currentDetail_).isEmpty());
  if(e)Ui::feedback(g->status_,e.message,true);
 });
}
void HistoryWindow::showQrCode(const QJsonObject& params,std::function<void(Error,QJsonObject)> reply){
 const auto id=params["historyId"].toString(),url=params["url"].toString();
 if(id.isEmpty()||url.isEmpty()){reply({"invalid_request","A QR code needs the item and its link",{}},{});return;}
 if(qrWindow_)qrWindow_->close();
 QString name=currentDetail_.value("id")==id?currentDetail_["name"].toString():QString();if(name.isEmpty())name="Link";
 QrCodeWindow::Actions actions;
 actions.copy=[g=QPointer<HistoryWindow>(this),id](QrCodeWindow::Reply done){if(g)g->session_.request("history.qr",{{"id",id},{"mode","copy"}},g,done);};
 actions.save=[g=QPointer<HistoryWindow>(this),id](const QString& path,bool overwrite,QrCodeWindow::Reply done){if(g)g->session_.request("history.qr",{{"id",id},{"mode","save"},{"path",path},{"overwrite",overwrite}},g,done);};
 auto* window=new QrCodeWindow(id,name,url,std::move(actions),this);qrWindow_=window;window->setWindowModality(Qt::NonModal);
 window->show();window->raise();window->activateWindow();
 reply({},window->state());
}
void HistoryWindow::editAgain(const QJsonObject& record){
 if(!editable(record)){Ui::feedback(status_,"Editing needs a retained local PNG or JPEG copy.",true);return;}
 emit editRequested("file",record["local"].toObject()["path"].toString());
}
// The row menu offers what this row can do now, from the same state and
// receipts as its inline action and the inspector; nothing it would refuse.
// Opening it on a row of a multi-selection keeps the selection and offers
// the batch actions.
void HistoryWindow::showRowMenu(const QModelIndex& i,const QPoint& point,bool overflow,bool collective){
 // A real Menu key arrives twice on xcb and Wayland: a keyboard context-menu
 // event, then the key press matching the row shortcut while the menu is open (#835).
 if(model_->busy()||rowMenuOpen_)return;
 QScopedValueRollback open(rowMenuOpen_,true);
 auto* selection=table_->selectionModel();
 if((collective||!overflow)&&selection->selectedRows().size()>1&&selection->isSelected(i)){
  selection->setCurrentIndex(i,QItemSelectionModel::NoUpdate);QMenu menu(this);menu.setObjectName("historyRowMenu");menu.addSection(QString("%1 selected").arg(selection->selectedRows().size()));
  if(!overflow&&selectionHasLinks())menu.addAction(copyLinks_->text(),this,&HistoryWindow::copySelectedLinks);
  if(const auto ids=deletableIds();!overflow&&!ids.isEmpty())menu.addAction(ids.size()==selectedFiles().size()?QString("Delete from destination…"):QString("Delete %1 from destination…").arg(ids.size()),this,[this]{const auto ids=deletableIds();prepareDelete(ids,int(selectedFiles().size())-int(ids.size()));});
  if(!overflow){menu.addSeparator();menu.addAction("Clear selection",this,[this]{table_->clearSelection();});}menu.addAction("Copy details",this,[this]{QJsonArray details;for(const auto& row:table_->selectionModel()->selectedRows())details.append(model_->record(row.row()));QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(details).toJson(QJsonDocument::Indented)));Ui::feedback(status_,"Selected operation and receipt details copied.");});menu.exec(point);return;
 }
 selection->select(i,QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);table_->setCurrentIndex(i);const auto row=model_->record(i.row());const auto files=row["records"].toArray();
 QMenu menu(this);menu.setObjectName("historyRowMenu");
 const bool inspector=!inspectorScroll_->isHidden();const auto primary=i.data(HistoryModel::PrimaryActionRole).toString();auto remaining=[&](const QString& action){return !overflow||action!=primary;};
 if(remaining("Cancel upload")&&i.data(HistoryModel::ActiveRole).toBool())menu.addAction("Cancel operation",this,[this]{operationAction("operation.cancel");});
 if(remaining("Retry upload")&&HistoryModel::retryUpload(row))menu.addAction("Retry upload",this,[this]{operationAction("operation.retry");});
 if(remaining("Retry local save")&&HistoryModel::retryLocalSave(row))menu.addAction("Retry local save",this,[this]{operationAction("operation.retryLocalSave");});
 // A batch's links are its result; one file's link is the stored link.
 if(remaining("Copy link")&&(row["linkAvailable"].toBool()||row["linkRefreshable"].toBool()))menu.addAction(Ui::icon("copy"),files.size()>1?"Copy links":"Copy link",this,[this,row,files]{if(files.size()>1&&!row["operationId"].toString().isEmpty()){copySelectedLinks();return;}copyHistoryLink(files.first().toObject());});
 if(remaining("Copy result")&&HistoryModel::copyLocalResult(row))menu.addAction("Copy result",this,[this]{operationAction("operation.copy");});
 if(remaining("Open in browser")&&files.size()==1&&row["linkAvailable"].toBool())menu.addAction("Open in browser",this,[this,url=files.first().toObject()["receipt"].toObject()["directUrl"].toString()]{openBrowser(url);});
 if(remaining("Edit image…")&&editable(row)){auto* edit=menu.addAction("Edit image…",this,[this,row]{editAgain(row);});edit->setToolTip("Opens a new edit of the saved image; previous marks are flattened");}
 // #1428: one file only; disabled with the reason when it cannot apply.
 if(files.size()==1){const auto file=files.first().toObject();
  if(file["mime"].toString().startsWith("image/")){const auto reason=copyTextUnavailable(file);auto* text=menu.addAction(Ui::icon("copy-text"),"Copy text",this,[this,file]{copyText(file);});text->setEnabled(reason.isEmpty()&&!textPoll_->isActive());text->setToolTip(reason.isEmpty()?"Copy the text in this image. Nothing is saved or uploaded.":reason);}
  if(file["uploadStatus"]=="uploaded"){const auto reason=qrUnavailable(file);auto* qr=menu.addAction(Ui::icon("qr-code"),"Show QR code",this,[this,file]{requestQrCode(file);});qr->setEnabled(reason.isEmpty());qr->setToolTip(reason.isEmpty()?"Show the link as a QR code":reason);}
  menu.setToolTipsVisible(true);}
 if(!inspector)menu.addAction(findChild<QAction*>("historyShowDetails"));
 menu.addSeparator();
 if(remaining("Save diagnostics for this upload")&&(!overflow||!inspector||saveDiagnostics_->isHidden())&&diagnosticsAvailable(i.row()))menu.addAction("Save diagnostics for this upload",this,[this,row]{saveDiagnostics(row["operationId"].toString());});
 menu.addAction("Copy details",this,[this,row]{QJsonObject detail=row;if(currentDetail_["operationId"]==row["operationId"])detail["selectedFile"]=currentDetail_;QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(detail).toJson(QJsonDocument::Indented)));Ui::feedback(status_,"Operation and receipt details copied.");});
 QStringList deletable;for(const auto& v:files)if(HistoryModel::remoteDeletable(v.toObject()))deletable.append(v.toObject()["id"].toString());
 if(remaining("Delete from destination…")&&!deletable.isEmpty()){menu.addSeparator();menu.addAction("Delete from destination…",this,[this,deletable,count=int(files.size())]{prepareDelete(deletable,count-int(deletable.size()));});}
 menu.exec(point);
}
void HistoryWindow::updateSummary(){
 if(currentDetail_.isEmpty())return;
 const auto data=currentDetail_;
 const QHash<QString,QString> copies{{"saved","Saved"},{"disabled","No local copy requested"},{"failed","Local save failed"},{"pending","Waiting for local save"}};
 const QHash<QString,QString> sharing{{"shared","Created"},{"not_requested","No share link requested"},{"private","Private upload"},{"pending","Creating share link"},{"revoked","Link revoked"},{"failed","Sharing failed"},{"unknown","Sharing outcome unknown"},{"changed","Share link changed"}};
 const QHash<QString,QString> remote{{"present","Available"},{"not_requested","Remote deletion not requested"},{"removed","Deleted from destination"},{"already_missing","Remote file already missing"},{"unknown","Remote outcome unknown"},{"failed","Remote deletion failed"},{"pending","Waiting for remote deletion"},{"deleting","Deleting from destination"},{"changed","Remote file changed"},{"superseded","Replaced by a newer upload"},{"hidden_version_retained","Hidden; original version remains"}};
 const auto size=HistoryModel::fileSize(data["bytes"].toInteger())+(pixelDimensions_.isEmpty()?QString{}:" · "+pixelDimensions_);const auto local=data["local"].toObject();
 // Label / value rows: type, output, created, size, file, then the recorded outcomes.
 const auto mime=data["mime"].toString();const auto category=data["category"].toString();
 const QString type=category=="images"?mime.mid(6).toUpper()+" image":category=="videos"?mime.mid(6).toUpper()+" recording":category=="text"?QString("Text"):QString("File")+(mime.isEmpty()?QString{}:" ("+mime+")");
 auto created=QDateTime::fromString(data["created"].toString(),Qt::ISODateWithMs);if(!created.isValid())created=QDateTime::fromString(data["created"].toString(),Qt::ISODate);
 const auto when=HistoryModel::longTimestamp(created);
 QList<std::pair<QString,QString>> rows;auto row=[&](const QString& label,const QString& value){if(!value.isEmpty())rows.append({label,value});};
 row("Type",type);row("Destination",destinationLabel(data));row("Created",when);row("Size",size);if(data["name"].toString()!=title_->text()&&QFileInfo(data["name"].toString()).completeBaseName()!=title_->text())row("File",data["name"].toString());
 // The recorded outcomes use the same grid; the type row already
 // names the format. The upload state is a row too unless the line under
 // the title already says it for this file: a batch's line describes the
 // whole capture, and a single file's line can lead with another outcome
 // ("Saved locally" after an interrupted upload).
 QTextDocument outcomeText;outcomeText.setHtml(outcome_->text());auto shownOutcome=outcomeText.toPlainText()+" "+(capabilityHint_->isHidden()?QString{}:capabilityHint_->text());shownOutcome.replace(QChar(0x00a0),QChar(' '));
 const QHash<QString,QString> uploads{{"uploaded","Uploaded"},{"local_only","Local capture"},{"pending","Waiting to upload"},{"uploading","Uploading"},{"failed","Upload failed"},{"unknown","Upload outcome unknown"},{"not_requested","No upload requested"}};
 {const auto upload=uploads.value(data["uploadStatus"].toString(),"Not recorded");if(items_->count()>1||(data["uploadStatus"]!="local_only"&&!data["linkAvailable"].toBool()&&!shownOutcome.contains(upload,Qt::CaseInsensitive)&&HistoryModel::remoteStateText(data).isEmpty()))row("Upload",upload);}
 const auto copy=HistoryModel::localCopyUnavailable(data)?QString("Local file unavailable"):copies.value(local["status"].toString(),"Not recorded"),link=data["uploadStatus"]=="failed"&&data["receipt"].toObject()["objectReference"].toObject().isEmpty()?QString("Not created (upload failed)"):data["linkRefreshable"].toBool()?QString("Expired; refresh on copy"):data["linkAvailable"].toBool()?QString("Created"):sharing.value(data["shareStatus"].toString(),"Not recorded");
 if(items_->count()>1||(!HistoryModel::localCopyUnavailable(data)&&capabilityHint_->isHidden()&&(!(local["status"]=="saved"&&shownOutcome.contains("Saved locally"))&&!shownOutcome.contains(copy,Qt::CaseInsensitive))))row("Local copy",copy);if(data["uploadStatus"]!="local_only"&&!data["linkAvailable"].toBool()&&(items_->count()>1||(!shownOutcome.contains("Upload failed")&&(HistoryModel::remoteStateText(data).isEmpty()||!shownOutcome.contains(HistoryModel::remoteStateText(data)))&&!shownOutcome.contains(link,Qt::CaseInsensitive))))row("Link",link);const auto remoteState=data["remoteStatus"].toString();if(!QStringList{"present","not_requested"}.contains(remoteState)&&(items_->count()>1||(HistoryModel::remoteStateText(data).isEmpty()||!shownOutcome.contains(HistoryModel::remoteStateText(data)))))row("Remote file",remote.value(remoteState,"Not recorded"));
 if(category!="images"&&category!="videos"&&!mime.isEmpty()&&!type.contains(mime))row("Content type",mime);
 static_cast<HistorySummary*>(summary_)->setFields(rows);summary_->setAccessibleDescription(summary_->accessibleDescription()+"\nFile: "+data["name"].toString()+"\nUpload: "+uploads.value(data["uploadStatus"].toString(),"Not recorded")+"\nLocal copy: "+copy+"\nLink: "+link);
}
void HistoryWindow::updatePreviewState(){
 static_cast<HistoryPreview*>(preview_)->clearImage();expandPreview_->hide();
 const auto id=currentDetail_.value("id").toString();const bool supported=currentDetail_.value("category")=="images"||video(currentDetail_);preview_->setVisible(supported);previewStatus_->setVisible(!supported&&capabilityHint_->isHidden());previewStatus_->setText(supported?QString{}:"No preview for this file type");retryPreview_->hide();
 if(supported){if(previewErrors_.contains(id))previewFailure(previewErrors_.value(id));else preview_->setText("Making a preview…");}
}
void HistoryWindow::previewFailure(const Error& error){
 static_cast<HistoryPreview*>(preview_)->clearImage();expandPreview_->hide();
 const auto id=currentDetail_.value("id").toString();previewErrors_.insert(id,error);const bool unsupported=error.code=="preview_unsupported",missing=error.code=="preview_source_missing";preview_->setVisible(!unsupported&&!missing);previewStatus_->setVisible(unsupported&&capabilityHint_->isHidden());previewStatus_->setText("No preview for this file type");
 if(!unsupported)preview_->setText(missing?"Local original unavailable":error.code=="previews_disabled"?"Previews are turned off":"Couldn't make a preview"+(error.message.isEmpty()?QString{}:". "+error.message));retryPreview_->setVisible(!unsupported&&!missing&&error.code!="previews_disabled");retryPreview_->setEnabled(previewRequestId_.isEmpty());
}
void HistoryWindow::startPreview(){
 if(pendingPreviewPath_.isEmpty()||!isVisible()||previewLoading_)return;
 const auto path=std::exchange(pendingPreviewPath_,{});const auto original=currentDetail_["local"].toObject()["status"]=="saved"?currentDetail_["local"].toObject()["path"].toString():QString{};const auto seq=selectionRequest_;const auto itemId=currentDetail_.value("id").toString();const bool isVideo=video(currentDetail_);previewLoading_=true;auto image=std::make_shared<QImage>();previewCancelled_=std::make_shared<std::atomic_bool>(false);auto cancelled=previewCancelled_;
 session_.background([path,original,image,cancelled](Error&){
  if(cancelled->load())return QJsonObject{};
  QFileInfo f(path);if(!f.isFile()||f.size()>8*1024*1024)return QJsonObject{};QImageReader reader(path);preparePreviewReader(reader,path);auto s=reader.size();if(!s.isValid()||qint64(s.width())*s.height()>16000000)return QJsonObject{};reader.setScaledSize(s.scaled(900,540,Qt::KeepAspectRatio));const auto length=reader.text(HistoryModel::durationKey);*image=reader.read();if(!length.isEmpty())image->setText(HistoryModel::durationKey,length);
  QJsonObject metadata;if(!original.isEmpty()&&QFileInfo(original).isFile()&&QFileInfo(original).size()<=32*1024*1024){QImageReader originalReader(original);preparePreviewReader(originalReader,original);const auto size=originalReader.size();if(size.isValid()){metadata["width"]=size.width();metadata["height"]=size.height();}}return metadata;
 },this,[g=QPointer<HistoryWindow>(this),seq,image,path,itemId,isVideo](Error,QJsonObject metadata){if(!g)return;g->previewLoading_=false;
  // A reload of the same item while the file was read still wants it.
  if(g->isVisible()&&(seq==g->selectionRequest_||(!itemId.isEmpty()&&itemId==g->selectedItemId_))){if(image->isNull())g->previewFailure({"preview_failed","The stored preview couldn't be read.",{}});else{g->previewErrors_.remove(itemId);g->retryPreview_->hide();g->preview_->show();static_cast<HistoryPreview*>(g->preview_)->setImage(*image,isVideo);const auto length=HistoryModel::duration(image->text(HistoryModel::durationKey).toLongLong());if(isVideo&&!length.isEmpty()){g->pixelDimensions_=length;g->updateSummary();g->inspectSelectedDeletion();}g->updateInspectorLayout();g->shownItemId_=itemId;g->shownPreviewPath_=path;}if(metadata["width"].toInt()>0){g->pixelDimensions_=QString("%1 × %2 px").arg(metadata["width"].toInt()).arg(metadata["height"].toInt());g->updateSummary();g->inspectSelectedDeletion();}}g->startPreview();});
}
bool HistoryWindow::previewMissing(const QJsonObject& detail)const{
 const auto category=detail["category"].toString(),id=detail["id"].toString(),path=detail["previewPath"].toString();
 return !id.isEmpty()&&(category=="images"||category=="videos")&&!previewRefused_.contains(id)&&(path.isEmpty()||!QFileInfo(path).isFile());
}
void HistoryWindow::requestPreview(){
 const auto id=currentDetail_.value("id").toString();if(!isVisible()||!previewRequestId_.isEmpty()||(!previewRetry_&&!previewMissing(currentDetail_)))return;
 previewRequestId_=id;retryPreview_->hide();preview_->setText("Making a preview…");
 QJsonObject previewParams{{"id",id}};if(std::exchange(previewRetry_,false))previewParams["retry"]=true;
 session_.request("history.preview",previewParams,this,[g=QPointer<HistoryWindow>(this),id](Error e,QJsonObject result){
  if(!g)return;
  g->previewRequestId_.clear();const bool current=g->currentDetail_.value("id")==id;
  if(e){
   // Fail-soft: no status message. A refused item is not asked for again in
   // this window; a busy queue or a running upload may change.
   if(!QStringList{"preview_busy","operation_active","client_timeout","initializing"}.contains(e.code)){g->previewRefused_.insert(id);g->previewErrors_.insert(id,e);}
   if(current&&g->preview_->pixmap().isNull())g->previewFailure(e);
  }else{
   const auto path=result["previewPath"].toString();g->model_->reloadThumbnail(path);
   if(current){g->pendingPreviewPath_=path;g->startPreview();}
  }
  // The selection moved on while this one was being made.
  if(!current&&g->previewMissing(g->currentDetail_))g->previewRequestTimer_->start();
 });
}
void HistoryWindow::releasePreview(){previewRequestTimer_->stop();if(previewCancelled_)previewCancelled_->store(true);pendingPreviewPath_.clear();shownItemId_.clear();shownPreviewPath_.clear();++selectionRequest_;static_cast<HistoryPreview*>(preview_)->clearImage();expandPreview_->hide();if(expandedPreview_)expandedPreview_->close();model_->releaseThumbnails();}
bool HistoryWindow::expandPreview(){
 const auto* source=static_cast<HistoryPreview*>(preview_);if(source->image.isNull())return false;
 if(expandedPreview_&&expandedPreview_->isVisible()){expandedPreview_->raise();expandedPreview_->activateWindow();return true;}
 auto* dialog=new QDialog(this);expandedPreview_=dialog;dialog->setObjectName("historyExpandedPreview");dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle("Preview — "+currentDetail_["name"].toString());dialog->setWindowModality(Qt::WindowModal);Ui::styleWindow(*dialog);
 auto* box=new QVBoxLayout(dialog);box->setContentsMargins(space(Space::l),space(Space::l),space(Space::l),space(Space::l));box->setSpacing(space(Space::m));auto* title=new Ui::ElidedLabel(dialog);title->setText(currentDetail_["name"].toString());title->setElideMode(titleElision(title->text()));Ui::role(title,"sectionTitle");box->addWidget(title);auto* picture=new HistoryPreview(dialog);picture->setObjectName("historyExpandedImage");picture->setAccessibleName("Expanded preview of "+currentDetail_["name"].toString());picture->setAlignment(Qt::AlignCenter);picture->setMinimumSize(120,120);picture->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Ignored);Ui::role(picture,"well");picture->setImage(source->image,source->recording);box->addWidget(picture,1);
 auto* close=new QPushButton("Close preview",dialog);close->setObjectName("closeHistoryPreview");close->setDefault(true);box->addWidget(close,0,Qt::AlignRight);connect(close,&QPushButton::clicked,dialog,&QDialog::accept);const QPointer<QWidget> returnFocus=preview_;connect(dialog,&QDialog::finished,this,[this,returnFocus]{QTimer::singleShot(0,this,[this,returnFocus]{if(!isVisible())return;auto* target=returnFocus&&returnFocus->isVisible()?returnFocus.data():preview_;if(target->isVisible()){window()->activateWindow();target->setFocus(Qt::OtherFocusReason);}});emit viewStateChanged();});
 const auto available=(screen()?screen()->availableGeometry().size():QSize(1000,700))-QSize(48,48);const int overhead=2*space(Space::l)+2*space(Space::m)+title->sizeHint().height()+close->sizeHint().height();const QSize limit(qMin(Ui::scaled(900),available.width()-2*space(Space::l)),qMin(Ui::scaled(640),available.height()-overhead));auto fitted=source->image.size();if(fitted.width()>limit.width()||fitted.height()>limit.height())fitted.scale(limit,Qt::KeepAspectRatio);picture->setMinimumSize(qMin(120,fitted.width()),qMin(120,fitted.height()));box->setAlignment(picture,{});dialog->resize(fitted+QSize(2*space(Space::l),overhead));dialog->show();close->setFocus(Qt::OtherFocusReason);emit viewStateChanged();return true;
}
void HistoryWindow::dismissSavedCopy(){savedCopyDismissed_=true;updateSavedCopy();emit viewStateChanged();}
void HistoryWindow::updateSavedCopy(){savedCopy_->setVisible(!savedPath_.isEmpty()&&!savedCopyDismissed_);savedLabel_->setText("Saved "+QFileInfo(savedPath_).fileName());savedLabel_->setToolTip(savedPath_);savedLabel_->setAccessibleDescription(savedPath_);}
void HistoryWindow::copySavedPath(){if(savedPath_.isEmpty())return;QApplication::clipboard()->setText(savedPath_);Ui::feedback(status_,"Saved copy path copied.");}
void HistoryWindow::showSavedCopy(){
 if(savedPath_.isEmpty())return;const auto path=savedPath_;host::showInFolder(path,[g=QPointer<HistoryWindow>(this),path](const QJsonObject& result){if(!g||g->savedPath_!=path)return;g->savedFolderResult_=result;if(!result["opened"].toBool())Ui::feedback(g->status_,"Couldn't open the saved copy's folder. Copy its path to open it manually.",true);emit g->viewStateChanged();});
}
void HistoryWindow::hideEvent(QHideEvent* event){if(auto* menu=findChild<QMenu*>("historyManagementMenu"))menu->close();releasePreview();RetainedWindow::hideEvent(event);}
void HistoryWindow::showEvent(QShowEvent* event){RetainedWindow::showEvent(event);selectionChanged();table_->viewport()->update();}
void HistoryWindow::exportLocal(bool image){
 if(image){const auto record=selectedRecord();if(record.isEmpty())return;session_.request("history.copyImage",{{"id",record["id"]}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g)Ui::feedback(g->status_,e?e.message:"Original image copied.",bool(e));});return;}

 if(copyJob_["status"]=="saving"||chooser_->isOpen())return;
 const auto historyId=currentDetail_.value("id").toString();const auto local=currentDetail_["local"].toObject();const auto path=local["path"].toString();if(local["status"]!="saved"||!QFileInfo(path).isFile())return;
 chooser_->open({FileDialog::Mode::Save,"Save original file as",{},currentDetail_["name"].toString(),{}},[this,path,historyId](const FileDialog::Result& result){
  if(result.failed()){Ui::feedback(status_,result.error,true);return;}
  if(!result.chosen())return;const auto destination=result.path;if(QFileInfo(destination).absoluteFilePath()==QFileInfo(path).absoluteFilePath()){Ui::feedback(status_,"This is already the retained local file.");return;}
  const auto filename=QFileInfo(destination).fileName();const auto job=QUuid::createUuid().toString(QUuid::WithoutBraces);copyJob_={{"id",job},{"historyId",historyId},{"filename",filename},{"path",QFileInfo(destination).absoluteFilePath()},{"status","saving"}};saveAs_->setEnabled(false);Ui::feedback(status_,"Saving “"+filename+"”…");emit viewStateChanged();
  session_.background([path,destination](Error& e){QFile source(path);QSaveFile target(destination);if(!source.open(QIODevice::ReadOnly)||!target.open(QIODevice::WriteOnly)){e={"file_copy","Couldn't open the original or selected destination file.",{}};return QJsonObject{};}while(!source.atEnd()){const auto bytes=source.read(1024*1024);if(bytes.isEmpty()&&source.error()!=QFileDevice::NoError){e={"file_copy","Couldn't read the retained original file.",{}};return QJsonObject{};}if(target.write(bytes)!=bytes.size()){e={"file_copy","Couldn't write the selected destination file.",{}};return QJsonObject{};}}if(!target.commit())e={"file_copy","Couldn't finish saving the original file.",{}};return QJsonObject{};},this,[g=QPointer<HistoryWindow>(this),destination,filename,job](Error e,QJsonObject){if(!g||g->copyJob_["id"]!=job)return;g->copyJob_["status"]=e?"failed":"saved";g->copyJob_["error"]=e?QJsonObject{{"code",e.code},{"message",e.message}}:QJsonObject{};g->saveAs_->setEnabled(g->currentDetail_["local"].toObject()["status"]=="saved"&&QFileInfo(g->currentDetail_["local"].toObject()["path"].toString()).isFile());Ui::feedback(g->status_,e?"Couldn't save “"+filename+"”. "+e.message:QString{},bool(e));if(!e){g->savedPath_=QFileInfo(destination).absoluteFilePath();g->savedCopyDismissed_=false;g->savedFolderResult_={};g->updateSavedCopy();}emit g->viewStateChanged();});
 });
}
void HistoryWindow::saveAsDialogCommand(const QString& action,const QString& id,std::function<void(Error,QJsonObject)> reply){
 if(action=="inspect"){reply({},chooser_->state());return;}
 if(action=="cancel"){chooser_->cancel();reply({},chooser_->state());return;}
 if(copyJob_["status"]=="saving"){reply({"history_copy_pending","Wait for the current copy to finish.",{}},historyState());return;}
 if(chooser_->isOpen()){reply({"file_dialog_open","Save as is already open.",{}},chooser_->state());return;}
 auto click=[this,reply](bool loaded){
  if(!loaded||!saveAs_->isEnabled()){reply({"history_save_unavailable","Save as needs a selected file with a retained local original.",{}},{});return;}
  exportLocal(false);reply({},chooser_->state());
 };
 if(id.isEmpty()){click(!currentDetail_.isEmpty());return;}
 for(int row=0;row<model_->rowCount();++row){
  const auto group=model_->record(row);bool match=group["operationId"]==id;
  for(const auto& v:group["records"].toArray())match|=v.toObject()["id"]==id;
  if(!match)continue;
  const bool selected=table_->selectionModel()->selectedRows()==QModelIndexList{model_->index(row)};
  if(selected&&!currentDetail_.isEmpty()){click(true);return;}
  // Selecting starts the detail request; Save as is clicked once it arrives.
  table_->selectionModel()->select(model_->index(row),QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);table_->setCurrentIndex(model_->index(row));
  afterDetail_=click;return;
 }
 reply({"history_row_not_found","That file is not on the History page shown; search or page to it first.",{{"id",id}}},{});
}
void HistoryWindow::changeSharing(const QString& action){
 const auto file=selectedRecord();if(file.isEmpty()||sharingAccepting_||sharingTimer_->isActive())return;
 const auto historyId=file["id"].toString();
 if(action=="revoke"&&!Ui::confirm(this,"Revoke share link",QString("Revoke the share link for ‘%1’ in %2?\nThe uploaded file remains. Existing member access and downloaded copies remain.").arg(file["name"].toString(),destinationLabel(file)),"Revoke link",true))return;
 sharingAccepting_=true;emit viewStateChanged();for(auto* button:{share_,revoke_,inspectLink_})button->setEnabled(false);
 const auto method=action=="create"?"history.share":action=="revoke"?"history.revokeLink":"history.inspectLink";
 session_.request(method,{{"id",historyId}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject data){
  if(!g)return;
  g->sharingAccepting_=false;emit g->viewStateChanged();
  if(e){Ui::feedback(g->status_,e.code+": "+e.message,true);g->selectionChanged();return;}
  g->sharingId_=data["shareOperationId"].toString();g->sharingStopped_=false;g->sharingFeedback_.clear();emit g->viewStateChanged();g->sharingTimer_->start();g->pollSharing();
 });
}
void HistoryWindow::pollSharing(){
 if(sharingPollActive_)return;
 if(sharingId_.isEmpty()){sharingTimer_->stop();return;}sharingPollActive_=true;const auto id=sharingId_;
 session_.request("share.inspect",{{"id",id}},this,[g=QPointer<HistoryWindow>(this),id](Error e,QJsonObject data){
  if(!g)return;
  g->sharingPollActive_=false;if(id!=g->sharingId_)return;
  if(e){g->sharingFeedback_=e.code+": "+e.message;g->sharingFeedbackError_=true;Ui::feedback(g->status_,g->sharingFeedback_,true);g->sharingTimer_->stop();g->refresh();return;}
  const auto state=data["state"].toString();g->sharingStopped_=state=="interrupted";if(state=="running"||state=="accepted")return;
  g->sharingTimer_->stop();const auto error=data["error"].toObject();g->sharingFeedback_="Link: "+data["status"].toString()+(error["message"].toString().isEmpty()?QString{}:" — "+error["message"].toString());g->sharingFeedbackError_=!error["code"].toString().isEmpty();Ui::feedback(g->status_,g->sharingFeedback_,g->sharingFeedbackError_);g->refresh();
 });
}
void HistoryWindow::prepareDelete(){const auto file=selectedRecord();if(!file.isEmpty())prepareDelete({file["id"].toString()},0);}
// Exact IDs go to delete.prepare; the confirmation shows its targets and
// confirms with that preparation's own acknowledgement.
void HistoryWindow::prepareDelete(const QStringList& ids,int skipped){
 if(ids.isEmpty())return;QHash<QString,QString> names;for(int row=0;row<model_->rowCount();++row)for(const auto& v:model_->record(row)["records"].toArray())if(ids.contains(v.toObject()["id"].toString()))names[v.toObject()["id"].toString()]=v.toObject()["name"].toString();
 session_.request("delete.prepare",{{"ids",QJsonArray::fromStringList(ids)}},this,[g=QPointer<HistoryWindow>(this),ids,names,skipped](Error e,QJsonObject preview){
  if(!g)return;if(e){const auto name=names.value(e.details["historyId"].toString());Ui::feedback(g->status_,e.code+": "+e.message+(name.isEmpty()?QString{}:" ("+name+")"),true);return;}
  g->deletionNames_=names;QPointer<QDialog> dialog=deletionConfirmation(g,preview,names,g->session_.document()["profiles"].toArray(),skipped);const bool accepted=dialog->exec()==QDialog::Accepted;if(!g)return;delete dialog.data();if(!accepted)return;
  g->deleteAccepting_=true;emit g->viewStateChanged();g->session_.request("delete.confirm",{{"ids",QJsonArray::fromStringList(ids)},{"acknowledgement",preview["acknowledgement"]}},g,[g](Error failure,QJsonObject data){if(!g)return;g->deleteAccepting_=false;emit g->viewStateChanged();Ui::feedback(g->status_,failure?failure.code+": "+failure.message:"Remote deletion accepted; individual outcomes will update.",bool(failure));if(!failure){g->deletionId_=data["deleteOperationId"].toString();emit g->viewStateChanged();g->deletionTimer_->start();}});
 });
}
namespace {
// Where a receipt's original target lives: the destination's name now, its
// service and the bucket or folder recorded at upload.
QString originalDestination(const QJsonObject& receipt,const QJsonArray& profiles){
 const auto d=receipt["destination"].toObject();const auto provider=d["providerId"].toString();const auto settings=d["settings"].toObject();
 const QString service=provider=="s3"?"S3":provider=="backblaze-b2"?"Backblaze B2":provider=="proton-drive"?"Proton Drive":provider=="custom-http"?"Custom uploader":provider;QString label;
 for(const auto& v:profiles)if(v.toObject()["id"]==d["profileId"]&&!d["profileId"].toString().isEmpty())label=v.toObject()["label"].toString();
 QStringList parts{label.isEmpty()?service:label};
 if(!settings["bucket"].toString().isEmpty())parts<<settings["bucket"].toString();if(!settings["folderName"].toString().isEmpty())parts<<settings["folderName"].toString();
 return parts.join(" · ");
}
}
QDialog* HistoryWindow::deletionConfirmation(QWidget* parent,const QJsonObject& preparation,const QHash<QString,QString>& names,const QJsonArray& profiles,int skipped){
 auto* dialog=new QDialog(parent);dialog->setObjectName("confirmAction");dialog->setWindowTitle("Delete from destination");dialog->setModal(true);
 auto* box=new QVBoxLayout(dialog);box->setContentsMargins(space(Space::xl),space(Space::l),space(Space::xl),space(Space::l));box->setSpacing(space(Space::m));const auto targets=preparation["targets"].toArray();
 const auto first=names.value(targets.first().toObject()["historyId"].toString(),"this file");
 auto* heading=new QLabel(targets.size()==1?QString("Delete “%1” from its destination?").arg(first):QString("Delete %1 files from their destinations?").arg(targets.size()),dialog);heading->setObjectName("deleteConfirmationHeading");heading->setTextFormat(Qt::PlainText);heading->setWordWrap(true);Ui::role(heading,"sectionTitle");box->addWidget(heading);
 const bool proton=std::any_of(targets.begin(),targets.end(),[](const QJsonValue& value){return value.toObject()["receipt"].toObject()["destination"].toObject()["providerId"]=="proton-drive";});
 if(proton){heading->setText("These files can't be deleted from Proton Drive");dialog->setWindowTitle(heading->text());}
 QString consequence=proton?"This selection includes Proton Drive files that put.gd can’t safely delete. Nothing will be deleted.":"put.gd deletes each file below from its destination. Local copies and history stay. Copies already cached by browsers or a CDN can stay visible for a while.";
 if(skipped>0)consequence+=QString("\n%1 other selected %2 no confirmed upload to delete and %3 left out.").arg(skipped).arg(skipped==1?"file has":"files have").arg(skipped==1?"is":"are");
 auto* text=new QLabel(consequence,dialog);text->setObjectName("deleteConfirmationText");text->setTextFormat(Qt::PlainText);text->setWordWrap(true);box->addWidget(text);
 bool unsupported=false;QStringList effects;QMap<QString,int> providerCounts;for(const auto& value:targets)++providerCounts[value.toObject()["receipt"].toObject()["destination"].toObject()["providerId"].toString()];
 for(auto it=providerCounts.cbegin();it!=providerCounts.cend();++it){const bool plural=it.value()>1;QString effect;
  if(it.key()=="proton-drive"){unsupported=true;effect=QString("Effect on Proton Drive: safe deletion of %1 unavailable. Files and share links remain. Revoke share links separately.").arg(plural?"these files is":"this file is");}
  else if(it.key()=="s3"){bool versioned=true;for(const auto& value:targets){const auto receipt=value.toObject()["receipt"].toObject();if(receipt["destination"].toObject()["providerId"]==it.key()&&receipt["objectReference"].toObject()["versionId"].toString().isEmpty())versioned=false;}effect=versioned?QString("Effect on S3: deletes only %1. Older versions and other objects remain.").arg(plural?"the selected object versions":"the selected object version"):QString("Effect on S3: removes %1 only if the recorded identity still matches. Other objects remain.").arg(plural?"the selected objects":"the selected object");}
  else if(it.key()=="backblaze-b2")effect=QString("Effect on Backblaze B2: deletes only %1 by %2. An older hidden version may become visible. Authorized links stop working when they expire.").arg(plural?"the selected file versions":"the selected file version",plural?"their IDs":"its ID");
  else for(const auto& value:targets){const auto target=value.toObject();if(target["receipt"].toObject()["destination"].toObject()["providerId"]==it.key()&&!target["effect"].toString().isEmpty()&&!effects.contains(target["effect"].toString()))effects.append(target["effect"].toString());}
  if(!effect.isEmpty())effects.append(effect);
 }if(!effects.isEmpty()){auto* effect=new QLabel(effects.join("\n"),dialog);effect->setObjectName("deleteConfirmationEffect");effect->setTextFormat(Qt::PlainText);effect->setWordWrap(true);Ui::role(effect,"secondary");box->addWidget(effect);}
 // Every exact target, scrollable so a large selection keeps the dialog bounded.
 auto* list=new QWidget;auto* rows=new QVBoxLayout(list);rows->setContentsMargins(0,space(Space::m),0,space(Space::m));rows->setSpacing(space(Space::m));rows->setAlignment(Qt::AlignTop);
 for(int index=0;index<targets.size();++index){
  const auto target=targets[index].toObject();const auto receipt=target["receipt"].toObject();const auto reference=receipt["objectReference"].toObject();
  if(index)rows->addWidget(Ui::rule(list));auto* grid=new QGridLayout;grid->setHorizontalSpacing(space(Space::m));grid->setVerticalSpacing(space(Space::xs));grid->setColumnStretch(1,1);
  auto* name=new QLabel(names.value(target["historyId"].toString(),target["historyId"].toString()),list);name->setTextFormat(Qt::PlainText);name->setWordWrap(true);Ui::role(name,"strong");grid->addWidget(name,0,0,1,2);
  QList<std::pair<QString,QString>> fields{{"Destination",originalDestination(receipt,profiles)}};
  if(receipt["destination"].toObject()["providerId"]!="proton-drive"){if(!reference["key"].toString().isEmpty())fields.append({"Key",reference["key"].toString()});
  for(const auto& [key,label]:QList<std::pair<QString,QString>>{{"versionId","Version"},{"fileId","File version"},{"revisionUid","Revision"},{"nodeUid","Node"}})if(!reference[key].toString().isEmpty())fields.append({label,reference[key].toString()});
  if(reference["key"].toString().isEmpty()&&fields.size()==1)fields.append({"Target",receipt["directUrl"].toString().isEmpty()?QString("Recorded remote file (see technical details)"):receipt["directUrl"].toString()});
  }
  int line=1;for(const auto& [label,value]:fields){auto* l=new QLabel(label,list);Ui::role(l,"meta");auto* v=new QLabel(value,list);v->setTextFormat(Qt::PlainText);v->setWordWrap(true);v->setTextInteractionFlags(Qt::TextSelectableByMouse);if(label!="Destination"&&label!="Effect")Ui::role(v,"mono");grid->addWidget(l,line,0,Qt::AlignTop);grid->addWidget(v,line,1);++line;}
  rows->addLayout(grid);
 }
 auto* scroll=new QScrollArea(dialog);scroll->setObjectName("deleteConfirmationTargets");scroll->setAccessibleName("Remote targets to delete");scroll->setWidget(list);scroll->setWidgetResizable(true);scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);scroll->setFrameShape(QFrame::NoFrame);Ui::transparentView(scroll);
 scroll->setMinimumHeight(0);box->addWidget(Ui::rule(dialog));box->addWidget(scroll,1);box->addWidget(Ui::rule(dialog));
 auto* toggle=new QToolButton(dialog);toggle->setObjectName("deleteConfirmationDetailsExpanded");toggle->setText("Technical details");Ui::disclosure(toggle);toggle->setProperty("uiFlush",true);toggle->setAccessibleName("Show the full deletion preparation");box->addWidget(toggle,0,Qt::AlignLeft);
 auto* detailsDialog=new QDialog(dialog);detailsDialog->setWindowTitle("Deletion technical details");auto* detailsBox=new QVBoxLayout(detailsDialog);auto* details=new QPlainTextEdit(QString::fromUtf8(QJsonDocument(preparation).toJson(QJsonDocument::Indented)),detailsDialog);details->setObjectName("deleteConfirmationDetails");details->setReadOnly(true);details->setLineWrapMode(QPlainTextEdit::NoWrap);Ui::role(details,"diagnostics");details->setAccessibleName("Deletion preparation receipt");detailsBox->addWidget(details);auto* copyDetails=new QPushButton("Copy technical details",detailsDialog);copyDetails->setObjectName("copyDeletionTechnicalDetails");detailsBox->addWidget(copyDetails);QObject::connect(copyDetails,&QPushButton::clicked,details,[details]{QApplication::clipboard()->setText(details->toPlainText());});detailsDialog->resize(Ui::scaled(520),Ui::scaled(320));
 QObject::connect(toggle,&QToolButton::toggled,detailsDialog,[detailsDialog](bool on){detailsDialog->setVisible(on);});QObject::connect(detailsDialog,&QDialog::finished,toggle,[toggle]{toggle->setChecked(false);});
 auto* buttons=new QHBoxLayout;buttons->setSpacing(space(Space::s));buttons->addStretch();auto* cancel=new QPushButton(unsupported?"Close":"Cancel",dialog);cancel->setObjectName("confirmCancel");cancel->setAutoDefault(true);cancel->setDefault(true);buttons->addWidget(cancel);QPushButton* action=nullptr;if(!unsupported){action=new QPushButton(QString("Delete %1 %2").arg(targets.size()).arg(targets.size()==1?"file":"files"),dialog);action->setObjectName("confirmActionButton");action->setAccessibleName(action->text());action->setAutoDefault(false);Ui::role(action,"danger");buttons->addWidget(action);}box->addLayout(buttons);
 QObject::connect(cancel,&QPushButton::clicked,dialog,&QDialog::reject);if(action)QObject::connect(action,&QPushButton::clicked,dialog,&QDialog::accept);
 const auto available=(parent?parent->screen():QGuiApplication::primaryScreen())->availableGeometry();const int height=qMax(1,qMin(available.height(),parent?parent->window()->height():available.height())-40),width=qMin(Ui::scaled(560),available.width()-40);box->setSizeConstraint(QLayout::SetNoConstraint);dialog->setMaximumSize(qMin(Ui::scaled(720),available.width()-40),height);dialog->resize(width,qMin(height,Ui::scaled(520)));box->activate();const int targetsHeight=rows->totalHeightForWidth(width-2*space(Space::xl));dialog->resize(width,std::min({height,Ui::scaled(520),dialog->height()-scroll->height()+targetsHeight}));cancel->setFocus(Qt::OtherFocusReason);
 return dialog;
}
void HistoryWindow::inspectSelectedDeletion(){
 const auto id=currentDetail_.value("deleteOperationId").toString(),file=currentDetail_.value("id").toString();if(id.isEmpty()||model_->busy())return;
 session_.request("delete.inspect",{{"id",id}},this,[g=QPointer<HistoryWindow>(this),id,file](Error e,QJsonObject data){if(!g||e||g->model_->busy()||g->currentDetail_.value("id")!=file||g->currentDetail_.value("deleteOperationId")!=id)return;
  QJsonObject item;for(const auto& v:data["items"].toArray())if(v.toObject()["historyId"]==file)item=v.toObject();if(item.isEmpty())return;
  const bool running=data["state"]=="running"||data["state"]=="accepted";const auto state=item["status"].toString();const bool activeTarget=running&&QStringList{"pending","deleting"}.contains(state);const bool retryable=!running&&QStringList{"pending","deleting","failed","unknown","hidden_version_retained","version_retained","unsupported_safe_removal"}.contains(state);
  g->selectedDeletion_=data;auto advanced=g->currentDetail_;advanced["deletion"]=data;g->details_->setPlainText(QString::fromUtf8(QJsonDocument(advanced).toJson(QJsonDocument::Indented)));g->deletionRetry_->setVisible(retryable);g->deletionCancel_->setVisible(activeTarget);g->deletionCancel_->setEnabled(activeTarget);g->deletionCancel_->setToolTip(activeTarget?"Cancel deletion requests that have not been sent":"This deletion has stopped. Already sent deletion requests can't be canceled.");Ui::role(g->copy_,retryable||activeTarget?"secondary":"primary");g->deletionStatus_->setVisible(activeTarget||retryable);
  const auto name=g->currentDetail_["name"].toString();const auto reason=item["error"].toObject()["message"].toString();g->deletionStatus_->setText(reason+(data["items"].toArray().size()>1?QString(" This deletion includes %1 files; retry or cancel applies to the whole deletion.").arg(data["items"].toArray().size()):QString{}));g->deletionStatus_->setVisible(!g->deletionStatus_->text().isEmpty()&&(activeTarget||retryable));
  g->deletionRetry_->setAccessibleName("Retry deletion of "+name);g->deletionCancel_->setAccessibleName("Cancel deletion of "+name);
  if(running){g->deletionId_=id;g->deletionTimer_->start();}emit g->viewStateChanged();
 });
}
void HistoryWindow::deletionAction(const QString& method){
 if(model_->busy()||selectedDeletion_.isEmpty()||selectedDeletion_["id"]!=currentDetail_.value("deleteOperationId"))return;
 const auto id=selectedDeletion_["id"].toString();deletionRetry_->setEnabled(false);deletionCancel_->setEnabled(false);
 session_.request(method,{{"id",id}},this,[g=QPointer<HistoryWindow>(this),id](Error e,QJsonObject){if(!g)return;g->deletionRetry_->setEnabled(true);g->deletionCancel_->setEnabled(true);Ui::feedback(g->status_,e?e.message:"Deletion action accepted; already sent deletion can't be rolled back.",bool(e));if(!e){g->deletionId_=id;g->deletionTimer_->start();}g->refresh();});
}
void HistoryWindow::pollDeletion(){if(deletionPollActive_)return;if(deletionId_.isEmpty()){deletionTimer_->stop();return;}deletionPollActive_=true;const auto id=deletionId_;session_.request("delete.inspect",{{"id",id}},this,[g=QPointer<HistoryWindow>(this),id](Error e,QJsonObject data){if(!g)return;g->deletionPollActive_=false;if(g->deletionId_!=id)return;if(e){Ui::feedback(g->status_,e.message,true);g->deletionTimer_->stop();return;}QStringList messages;bool failed=false;for(const auto& v:data["items"].toArray()){const auto item=v.toObject();const auto state=item["status"].toString(),id=item["historyId"].toString();auto name=g->deletionNames_.value(id);if(name.isEmpty())for(int row=0;row<g->model_->rowCount();++row)for(const auto& f:g->model_->record(row)["records"].toArray())if(f.toObject()["id"]==id)name=f.toObject()["name"].toString();if(name.isEmpty())name="Selected file";const auto reason=item["error"].toObject()["message"].toString();if(state=="removed"||state=="already_missing")messages<<"Deleted from destination: "+name;else if(state=="failed"){messages<<"Deletion failed: "+name+(reason.isEmpty()?QString{}:" — "+reason);failed=true;}else if(state=="pending"||state=="deleting")messages<<"Deleting from destination: "+name;else messages<<"Deletion needs attention: "+name+(reason.isEmpty()?QString{}:" — "+reason);}Ui::feedback(g->status_,messages.join("\n"),failed);g->status_->setProperty("deletionDetails",data);if(g->currentDetail_.value("deleteOperationId")==id){auto advanced=g->currentDetail_;advanced["deletion"]=data;g->details_->setPlainText(QString::fromUtf8(QJsonDocument(advanced).toJson(QJsonDocument::Indented)));}if(data["state"]!="running"&&data["state"]!="accepted"){g->deletionTimer_->stop();g->refresh();}});}
QJsonObject HistoryWindow::saveViewState()const{
 if(deleteAccepting_||sharingAccepting_)return {{"stateError","request_awaiting_acceptance"}};
 const auto& q=model_->query();QJsonObject query{{"limit",q.limit},{"offset",q.offset},{"search",q.search},{"category",q.category},{"mime",q.mime},{"profileId",q.profileId},{"uploadStatus",q.uploadStatus},{"localStatus",q.localStatus},{"remoteStatus",q.remoteStatus},{"from",q.from},{"to",q.to}};QJsonObject filters{{"search",findChild<QLineEdit*>("historySearch")->text()}};for(const auto& key:QStringList{"from","to","mime","profileId","uploadStatus","localStatus","remoteStatus"})filters[key]=findChild<QLineEdit*>("historyFilter"+key)->text();
 return {{"savedPath",savedPath_},{"query",query},{"filters",filters},{"selectedIds",QJsonArray::fromStringList(restoringSelection_?restoreSelection_:selectedIds())},{"selectedItemId",selectedItemId_},{"deletionId",deletionId_},{"sharingId",sharingId_},{"filtersExpanded",allFilters_->isChecked()},{"detailsWanted",detailsWanted_},{"managementExpanded",findChild<QToolButton*>("historyManagementExpanded")->isChecked()},{"receiptExpanded",findChild<QToolButton*>("historyReceiptExpanded")->isChecked()},{"gridMode",grid_->isChecked()},{"detailsOpen",detailsOpen_||restoreDetailsOpen_},{"detailsFromGallery",detailsFromGallery_},{"galleryScroll",grid_->isChecked()&&!detailsOpen_&&!restoreScroll_?table_->verticalScrollBar()->value():galleryScroll_},{"listScroll",!grid_->isChecked()&&!detailsOpen_&&!restoreScroll_?table_->verticalScrollBar()->value():listScroll_},{"showCancelled",showCancelled_->isChecked()},{"geometry",QJsonArray{x(),y(),width(),height()}}};
}
bool HistoryWindow::restoreViewState(const QJsonObject& state){
 if(validateUiState({{"schemaVersion",1},{"history",state}}))return false;
 const QSignalBlocker stateSignals(this);savedPath_=state["savedPath"].toString();savedFolderResult_={};updateSavedCopy();searchTimer_->stop();const auto box=state["geometry"].toArray();setGeometry(box[0].toInt(),box[1].toInt(),box[2].toInt(),box[3].toInt());const auto f=state["filters"].toObject();findChild<QLineEdit*>("historySearch")->setText(f["search"].toString());for(const auto& key:QStringList{"from","to","mime","profileId","uploadStatus","localStatus","remoteStatus"})findChild<QLineEdit*>("historyFilter"+key)->setText(f[key].toString());allFilters_->setChecked(state["filtersExpanded"].toBool());detailsWanted_=!state.contains("detailsWanted")||state["detailsWanted"].toBool();findChild<QToolButton*>("historyManagementExpanded")->setChecked(false);findChild<QToolButton*>("historyReceiptExpanded")->setChecked(state["receiptExpanded"].toBool());setGridMode(state["gridMode"].toBool()||(state["detailsFromGallery"].toBool()&&state["detailsOpen"].toBool()));detailsFromGallery_=state["detailsFromGallery"].toBool();restoreDetailsOpen_=state["detailsOpen"].toBool();galleryScroll_=state["galleryScroll"].toInt();listScroll_=state["listScroll"].toInt();restoreScroll_=true;showCancelled_->setChecked(state["showCancelled"].toBool());restoreSelection_.clear();for(auto id:state["selectedIds"].toArray())restoreSelection_.append(id.toString());selectedItemId_=state["selectedItemId"].toString();restoringSelection_=true;deletionId_=state["deletionId"].toString();if(!deletionId_.isEmpty()){deletionTimer_->start();pollDeletion();}sharingId_=state["sharingId"].toString();sharingStopped_=false;sharingFeedback_.clear();sharingTimer_->stop();if(!sharingId_.isEmpty()){sharingTimer_->start();pollSharing();}
 const auto data=state["query"].toObject();UiHistoryQuery q;q.limit=data["limit"].toInt(50);q.offset=data["offset"].toInt();q.search=data["search"].toString();q.category=data["category"].toString();q.mime=data["mime"].toString();q.profileId=data["profileId"].toString();q.uploadStatus=data["uploadStatus"].toString();q.localStatus=data["localStatus"].toString();q.remoteStatus=data["remoteStatus"].toString();q.from=data["from"].toString();q.to=data["to"].toString();{restoringCategory_=true;int row=0;for(int i=0;i<categories_->count();++i)if(categories_->item(i)->data(Qt::UserRole).toString()==q.category)row=i;categories_->setCurrentRow(q.category.isEmpty()&&f["uploadStatus"]=="failed"?5:row);restoringCategory_=false;}if(data["search"]!=f["search"]||data["mime"]!=f["mime"]||data["profileId"]!=f["profileId"]||data["uploadStatus"]!=f["uploadStatus"]||data["localStatus"]!=f["localStatus"]||data["remoteStatus"]!=f["remoteStatus"]||data["from"]!=f["from"]||data["to"]!=f["to"])q.offset=0;q.search=f["search"].toString();q.mime=f["mime"].toString();q.profileId=f["profileId"].toString();q.uploadStatus=f["uploadStatus"].toString();q.localStatus=f["localStatus"].toString();q.remoteStatus=f["remoteStatus"].toString();q.from=f["from"].toString();q.to=f["to"].toString();searchTimer_->stop();updateHeading();model_->setQuery(q);return true;
}
QJsonObject HistoryWindow::historyState()const{
 const auto& q=model_->query();const int rows=table_->selectionModel()->selectedRows().size();QJsonArray actions;
 if(rows>1){if(selectionHasLinks())actions.append("copyLinks");if(!deletableIds().isEmpty())actions.append("delete");actions.append("clearSelection");}
 return {{"query",QJsonObject{{"limit",q.limit},{"offset",q.offset},{"search",q.search},{"category",q.category},{"mime",q.mime},{"profileId",q.profileId},{"uploadStatus",q.uploadStatus},{"localStatus",q.localStatus},{"remoteStatus",q.remoteStatus},{"from",q.from},{"to",q.to}}},
  {"selectionSummary",rows>1?selectionSummaryState_:QJsonObject{}},{"cancelAllConfirmation",QJsonObject{{"open",cancelAllConfirmation_&&cancelAllConfirmation_->isVisible()},{"operationIds",QJsonArray::fromStringList(cancelAllIds_)}}},{"activeOperationCount",model_->activeOperations().size()},{"activeFilters",QJsonArray::fromStringList(activeFilters())},{"filtersLabel",activeFilters().isEmpty()?QString("Filters"):QString("Filters · %1").arg(activeFilters().size())},{"selectedIds",QJsonArray::fromStringList(selectedIds())},{"selectedRows",rows},{"selectionActions",actions},{"deletableIds",QJsonArray::fromStringList(deletableIds())},{"eligibleLinkIds",QJsonArray::fromStringList(selectedLinkIds())},
  {"savedPath",savedPath_},{"savedCopyVisible",!savedCopy_->isHidden()},{"copyJob",copyJob_},{"savedCopyFolder",savedFolderResult_},{"previewExpanded",expandedPreview_&&expandedPreview_->isVisible()},{"canExpandPreview",!static_cast<HistoryPreview*>(preview_)->image.isNull()},{"canRetryPreview",!retryPreview_->isHidden()&&retryPreview_->isEnabled()},{"canRetryLoading",!retryLoading_->isHidden()},{"busy",model_->busy()},{"deletion",selectedDeletion_.isEmpty()?QJsonObject{}:QJsonObject{{"id",selectedDeletion_["id"]},{"historyId",currentDetail_.value("id")},{"name",currentDetail_["name"]},{"canRetry",!deletionRetry_->isHidden()},{"canCancel",!deletionCancel_->isHidden()&&deletionCancel_->isEnabled()}}},{"layout",compact_?"compact":"split"},{"detailsOpen",detailsOpen_},{"detailsWanted",detailsWanted_},{"heading",heading_->text()},{"countLabel",count_->text()},{"categoryTotals",model_->categoryTotals()},{"days",model_->daySections()?model_->dayCounts():QJsonArray{}},{"page",pageState()},
  {"filterChips",[this]{QJsonArray chips;for(const auto& entry:filterEntries())chips.append(QJsonObject{{"key",entry.key},{"label",entry.label},{"value",entry.value}});return chips;}()},{"foldedFilters",filterChips_?static_cast<HistoryChipRow*>(filterChips_)->folded():0},{"filtersPanelOpen",allFilters_->isChecked()},{"selectionHeader",selectionHeader()},
  {"deletionConfirmationOpen",[this]{for(auto* dialog:findChildren<QDialog*>("confirmAction"))if(dialog->isVisible())return true;return false;}()},{"detailsOrigin",detailsFromGallery_?"gallery":"list"},{"galleryScroll",galleryScroll_},{"inspectorVisible",!inspectorScroll_->isHidden()},{"gridMode",grid_->isChecked()},{"rows",model_->rowCount()},{"total",double(model_->total())}};
}
void HistoryWindow::command(const QJsonObject& params,std::function<void(Error,QJsonObject)> reply){
 const auto action=params["action"].toString();if(model_->busy()&&action!="inspect"&&action!="clearFilters"&&action!="closePreview"&&action!="showSavedCopy"&&action!="copySavedPath"&&action!="dismissSavedCopy"){reply({"history_busy","Wait for History to finish filtering.",{}},historyState());return;}
 if(action=="retryLoading"){if(retryLoading_->isHidden()){reply({"history_load_available","History has no load failure to retry.",{}},historyState());return;}retryLoading_->click();}
 else if(action=="copyDeletionDetails"){auto* confirmation=findChild<QDialog*>("confirmAction");auto* copy=confirmation?confirmation->findChild<QPushButton*>("copyDeletionTechnicalDetails"):nullptr;if(!confirmation||!confirmation->isVisible()||!copy){reply({"history_confirmation_unavailable","Open a deletion confirmation before copying its technical details.",{}},historyState());return;}copy->click();}
 else if(action=="clearFilters")clearFilters(params["scope"].toString()=="all");
 else if(action=="select"){
  // By history ID, operation ID or a row's own ID, on the page shown.
  QSet<QString> wanted;for(const auto& v:params["ids"].toArray())wanted.insert(v.toString());QItemSelection chosen;QModelIndex first;
  for(int row=0;row<model_->rowCount();++row){const auto r=model_->record(row);bool match=wanted.contains(r["operationId"].toString())||wanted.contains(r["id"].toString());for(const auto& v:r["records"].toArray())match|=wanted.contains(v.toObject()["id"].toString());if(match){chosen.select(model_->index(row),model_->index(row));if(!first.isValid())first=model_->index(row);}}
  if(!first.isValid()){reply({"history_row_not_found","None of those items is on the History page shown; search or page to them first.",{}},historyState());return;}
  table_->selectionModel()->select(chosen,QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);table_->selectionModel()->setCurrentIndex(first,QItemSelectionModel::NoUpdate);table_->scrollTo(first);
 }
 else if(action=="cancelAll")prepareCancelAll();
 else if(action=="confirmCancelAll"||action=="keepUploading"){if(!cancelAllConfirmation_||!cancelAllConfirmation_->isVisible()){reply({"history_confirmation_unavailable","Open Cancel all before answering its confirmation.",{}},historyState());return;}if(action=="confirmCancelAll")confirmCancelAll();else cancelAllConfirmation_->reject();}
 else if(action=="clearSelection")table_->clearSelection();
 else if(action=="openDetails"){if((grid_->isChecked()||compact_)&&table_->selectionModel()->selectedRows().isEmpty()){reply({"history_selection_invalid","Select at least one row before opening its details.",{}},historyState());return;}openSelectedDetails();}
 else if(action=="closeDetails")closeDetails();
 else if(action=="toggleDetails")toggleDetails();
 else if(action=="nextPage"||action=="previousPage"||action=="goToPage"){
  const auto& q=model_->query();const int current=q.offset/qMax(1,q.limit)+1,target=action=="nextPage"?current+1:action=="previousPage"?current-1:params["page"].toInt();
  if(target<1||target>qMax(1,pageCount())){reply({"history_page_unavailable",QString("History has %1 %2 for this query.").arg(qMax(1,pageCount())).arg(pageCount()==1?"page":"pages"),{}},historyState());return;}goToPage(target);
 }
 else if(action=="setFilter"||action=="removeFilter"){
  const auto key=params["key"].toString();if(key=="uploadStatus"&&categories_->currentRow()==5){reply({"history_filter_locked","Upload state is locked to Failed while Failed uploads is shown.",{}},historyState());return;}
  setFilter(key,action=="setFilter"?params["value"].toString():QString{});
 }
 else if(action=="showCategory"){const auto category=params["category"].toString();int row=0;for(int i=0;i<categories_->count();++i)if(categories_->item(i)->flags()!=Qt::NoItemFlags&&categories_->item(i)->data(Qt::UserRole).toString()==(category=="all"?QString{}:category)){row=i;break;}categories_->setCurrentRow(row);}
 else if(action=="setView"){setDetailsOpen(false);setGridMode(params["view"].toString()=="gallery");}
 else if(action=="copySelectedLinks"){if(table_->selectionModel()->selectedRows().size()<2||selectedLinkIds().isEmpty()){reply({"history_selection_invalid","Select two or more rows with stored links first.",{}},historyState());return;}copySelectedLinks();}
 else if(action=="deleteSelected"){const auto ids=deletableIds();if(table_->selectionModel()->selectedRows().size()<2||ids.isEmpty()){reply({"history_selection_invalid","Select two or more rows with confirmed uploads first.",{}},historyState());return;}prepareDelete(ids,int(selectedFiles().size())-int(ids.size()));}
 else if(action=="cancelConfirmation"){bool closed=false;for(auto* dialog:findChildren<QDialog*>("confirmAction"))if(dialog->isVisible()){dialog->reject();closed=true;}if(!closed){reply({"history_confirmation_unavailable","No deletion confirmation is open.",{}},historyState());return;}}
 else if(action=="expandPreview"){if(!expandPreview()){reply({"history_preview_unavailable","Select a file with a loaded preview first.",{}},historyState());return;}}
 else if(action=="retryPreview"){if(retryPreview_->isHidden()||!retryPreview_->isEnabled()){reply({"history_preview_unavailable","Select a file with a failed preview first.",{}},historyState());return;}retryPreview_->click();}
 else if(action=="closePreview"){if(expandedPreview_)expandedPreview_->close();}
 else if(action=="dismissSavedCopy")dismissSavedCopy();
 else if(action=="showSavedCopy"||action=="copySavedPath"){if(savedPath_.isEmpty()){reply({"history_save_unavailable","Save a copy before using its path.",{}},historyState());return;}if(action=="showSavedCopy")showSavedCopy();else copySavedPath();}
 else if(action!="inspect"){reply({"invalid_request","Unknown History action",{}},{});return;}
 reply({},historyState());
}
// A failed (or partial or interrupted) upload with an operation gets a package for just
// that operation; the resident shows "diagnostics saved" with Show in folder (#122).
bool HistoryWindow::diagnosticsAvailable(int row)const{
 const auto index=model_->index(row);return index.isValid()&&index.data(HistoryModel::FailedRole).toBool()&&!index.data(HistoryModel::ActiveRole).toBool()&&!model_->record(row)["operationId"].toString().isEmpty();
}
void HistoryWindow::saveDiagnostics(const QString& operationId,std::function<void(Error,QJsonObject)> done){
 if(operationId.isEmpty()){if(done)done({"invalid_request","This row has no operation to package.",{}},{});return;}
 QJsonObject params{{"operation",operationId},{"notify",true}};
 const auto folder=session_.document()["preferences"].toObject()["diagnostics"].toObject()["packageFolder"].toString();if(QFileInfo(folder).isDir())params["output"]=folder;
 saveDiagnostics_->setEnabled(false);Ui::feedback(status_,"Saving diagnostics for this upload…");
 session_.request("diagnostics.bundle",params,this,[g=QPointer<HistoryWindow>(this),done](Error e,QJsonObject result){
  if(g){g->saveDiagnostics_->setEnabled(true);Ui::feedback(g->status_,e?e.message:"Diagnostics saved to "+QDir::toNativeSeparators(result["path"].toString())+". Nothing was uploaded.",bool(e));}
  if(done)done(e,result);
 });
}
void HistoryWindow::saveDiagnosticsFor(const QString& id,std::function<void(Error,QJsonObject)> reply){
 for(int row=0;row<model_->rowCount();++row){
  const auto group=model_->record(row);bool match=group["operationId"]==id;
  for(const auto& v:group["records"].toArray())match|=v.toObject()["id"]==id;
  if(!match)continue;
  table_->selectionModel()->select(model_->index(row),QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);table_->setCurrentIndex(model_->index(row));
  if(!diagnosticsAvailable(row)){reply({"diagnostics_row_not_failed","Save diagnostics is offered for failed, partial and interrupted uploads only.",{{"operationId",group["operationId"]}}},{});return;}
  saveDiagnostics(group["operationId"].toString(),std::move(reply));return;
 }
 reply({"history_row_not_found","That upload is not on the History page shown; search or page to it first.",{{"id",id}}},{});
}
}
