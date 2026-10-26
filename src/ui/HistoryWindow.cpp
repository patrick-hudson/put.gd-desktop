#include "HistoryWindow.h"
#include "FileDialog.h"
#include "ViewState.h"
#include "VisualStyle.h"
#include "RedesignTheme.h"
#include "ByteSize.h"
#include "Graphite.h"
#include <QListWidget>
#include <QCursor>
#include <QApplication>
#include <QAction>
#include <QKeySequence>
#include <QClipboard>
#include <QComboBox>
#include "platform/HostOpen.h"
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QDateEdit>
#include <QCheckBox>
#include <QScreen>
#include <QDialog>
#include <QScrollBar>
#include <QDir>
#include <QGridLayout>
#include <QHideEvent>
#include <QImageReader>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSaveFile>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTextDocument>
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
// The recorded link's confirmed expiry (#253): upcoming while the link works,
// or passed once the service or the clock says it has.
QString linkExpiry(const QJsonObject& detail){
 const auto expires=QDateTime::fromString(detail["shareExpiresAt"].toString(),Qt::ISODateWithMs);
 if(!expires.isValid()||!QStringList{"shared","private"}.contains(detail["shareStatus"].toString()))return {};
 const auto when=QLocale().toString(expires.toLocalTime(),QLocale::ShortFormat);
 if(expires<=QDateTime::currentDateTimeUtc())return "Link expired on "+when;
 return detail["linkAvailable"].toBool()?"Link expires on "+when:QString{};
}
// A video's poster carries a play glyph and its length, so a still is never
// mistaken for a screenshot (#112).
bool video(const QJsonObject& record){return record["category"]=="videos"||record["mime"].toString().startsWith("video/");}
void paintVideoBadge(QPainter& p,const QRect& picture,const QString& length){
 using namespace RedesignTheme;p.save();p.setRenderHint(QPainter::Antialiasing);p.setPen(Qt::NoPen);p.setBrush(mediaScrim());
 const int size=qBound(18,qMin(picture.width(),picture.height())*2/5,48);const QRect circle(picture.center()-QPoint(size/2,size/2),QSize(size,size));
 p.drawEllipse(circle);VisualStyle::icon("play",onMedia()).paint(&p,circle.adjusted(size/4+size/16,size/4,-size/4+size/16,-size/4));
 if(!length.isEmpty()&&picture.height()>=40){
  auto font=p.font();font.setPixelSize(qBound(10,size/3,13));font.setBold(true);p.setFont(font);const QFontMetrics metrics(font);
  const QSize pill(metrics.horizontalAdvance(length)+10,metrics.height()+2);const QRect box(picture.bottomRight()-QPoint(pill.width()+3,pill.height()+3),pill);
  p.setBrush(mediaScrim());p.drawRoundedRect(box,4,4);p.setPen(onMedia());p.drawText(box,Qt::AlignCenter,length);
 }
 p.restore();
}
// One plain-text layout supplies both the failed card's height and its paint.
// Word-boundary fallback keeps commands, paths and long native error tokens
// readable without expanding the viewport or treating provider text as HTML.
std::unique_ptr<QTextDocument> failureText(const QString& text,const QFont& font,int width){
 auto document=std::make_unique<QTextDocument>();document->setDocumentMargin(0);document->setDefaultFont(font);
 auto option=document->defaultTextOption();option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);document->setDefaultTextOption(option);
 document->setPlainText(text);document->setTextWidth(qMax(20,width));return document;
}
// Real label/value widgets keep selection native and share one measured gutter.
class HistorySummary final:public QWidget {
public:
 explicit HistorySummary(QWidget* parent):QWidget(parent){grid_=new QGridLayout(this);grid_->setContentsMargins(0,0,0,0);grid_->setHorizontalSpacing(12);grid_->setVerticalSpacing(8);setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);}
 void setFields(const QList<std::pair<QString,QString>>& fields){
  while(auto* item=grid_->takeAt(0)){delete item->widget();delete item;}labels_.clear();values_.clear();QStringList text;
  for(const auto& [key,value]:fields){auto* label=new QLabel(key,this);VisualStyle::role(label,"meta");auto* content=new QLabel(value,this);content->setTextFormat(Qt::PlainText);content->setWordWrap(true);content->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);content->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);content->setMinimumWidth(0);content->setAccessibleName(key+": "+value);content->setToolTip(value);labels_.append(label);values_.append(content);text.append(key+": "+value);}
  setProperty("text",text.join("\n"));setAccessibleDescription(text.join("\n"));arrange();
 }
protected:void resizeEvent(QResizeEvent* event)override{QWidget::resizeEvent(event);arrange();}
private:
 void arrange(){
  int gutter=0;for(auto* label:labels_)gutter=qMax(gutter,label->fontMetrics().horizontalAdvance(label->text()));const bool stacked=width()<gutter+12+Graphite::scaled(140);
  while(auto* item=grid_->takeAt(0)){delete item;}grid_->setColumnMinimumWidth(0,stacked?0:gutter);grid_->setColumnStretch(0,stacked?1:0);grid_->setColumnStretch(1,stacked?0:1);
  for(int i=0;i<labels_.size();++i){grid_->addWidget(labels_[i],stacked?2*i:i,0,Qt::AlignTop);grid_->addWidget(values_[i],stacked?2*i+1:i,stacked?0:1);}
  setProperty("stacked",stacked);
 }
 QGridLayout* grid_;QList<QLabel*> labels_,values_;
};
// The list's columns (Graphite): Item takes what Output and Created leave; a
// narrow list drops Output first, then Created, and the inspector keeps both.
// Widths follow the text size, and a column stays only while Item keeps room
// for a readable name, so a heading and its values come and go together.
struct Columns {QRect item,output,created,actions;};
// menuOnly: the inspector beside the list holds the actions, so rows keep
// only their ⋯ menu and the action column shrinks to it. Recovery uses a footer.
Columns historyColumns(const QRect& row,bool menuOnly=false){
 Columns c;const int action=row.width()>=Graphite::scaled(menuOnly?320:600)?(menuOnly?qMax(32,QFontMetrics(Graphite::font()).height()+12)+12:Graphite::scaled(180)):0;const int width=row.width()-action,minimum=Graphite::scaled(300);c.actions=action?QRect(row.right()-action+1,row.top(),action,row.height()):QRect();const int created=width-Graphite::scaled(92)>=minimum?Graphite::scaled(92):0;const int output=created&&width-created-Graphite::scaled(132)>=minimum+Graphite::scaled(20)?Graphite::scaled(132):0;
 c.created=created?QRect(row.left()+width-created,row.top(),created,row.height()):QRect();
 c.output=output?QRect(row.left()+width-created-output,row.top(),output,row.height()):QRect();
 c.item=QRect(row.left(),row.top(),width-created-output,row.height());return c;
}
int dayHeadingHeight(){return Graphite::scaled(32);}
// Row actions are 32 px targets, taller when enlarged text needs it.
int actionHeight(){return qMax(32,QFontMetrics(Graphite::font()).height()+12);}
// A gallery card's picture keeps the 248 px card's proportions as cards grow.
int galleryPicture(int cardWidth){return qMax(90,(cardWidth-2)*145/234);}
// The row's body: right of the thumbnail in a list, under it in a gallery tile.
QRect rowBody(const QRect& outer,bool gallery,int heading){
 const auto card=outer.adjusted(6,heading+2,-6,-2);
 if(gallery){const int top=card.top()+1+galleryPicture(card.width())+10;return QRect(card.left()+14,top,card.width()-28,qMax(0,card.bottom()-top+1));}
 const int left=card.left()+12+Graphite::Metric::thumbWidth+16;
 return QRect(left,card.top(),card.right()-12-left+1,Graphite::scaled(Graphite::Metric::historyRow)-4);
}
// A list row at the viewport's current width. The column header and every
// row lay out their columns from this one geometry, scrollbar included.
QRect listRow(const QListView* view,int top,int height){return QRect(view->spacing(),top,qMax(160,view->viewport()->width()-2*view->spacing()),height);}
// Item / Output / Created above the list, on the same columns as its rows.
class HistoryColumns final:public QWidget {
public:
 std::function<bool()> menuOnly;
 explicit HistoryColumns(QListView* view,QWidget* parent):QWidget(parent),view_(view){setObjectName("historyColumns");setFixedHeight(Graphite::scaled(30));view_->viewport()->installEventFilter(this);}
 // A scrollbar narrows the viewport without resizing this header.
 bool eventFilter(QObject*,QEvent* event)override{if(event->type()==QEvent::Resize)update();return false;}
protected:
 void paintEvent(QPaintEvent*)override{
  QPainter p(this);const auto& t=Theme::current();const int left=view_->viewport()->mapTo(window(),QPoint(0,0)).x()-mapTo(window(),QPoint(0,0)).x();
  const auto row=listRow(view_,0,height()).translated(left,0);const auto body=rowBody(row,false,0);
  const auto c=historyColumns(QRect(body.left(),0,body.width(),height()),menuOnly&&menuOnly());const auto f=Graphite::font(Graphite::Type::metadata);const int thumb=row.left()+6+12;
  Graphite::paintText(p,QRectF(thumb,0,qMax(0,c.item.right()-thumb-8),height()),"Item",t.textMuted,f);
  if(!c.output.isEmpty())Graphite::paintText(p,c.output.adjusted(0,0,-8,0),"Output",t.textMuted,f);
  if(!c.created.isEmpty())Graphite::paintText(p,c.created.adjusted(0,0,-8,0),"Created",t.textMuted,f);
  p.setPen(t.border);p.drawLine(0,height()-1,width(),height()-1);
 }
private:QListView* view_;
};
}
QRect HistoryWindow::failureTextArea(const QSize& row,bool gallery,bool dayHeading){
 const auto body=rowBody(QRect(QPoint(),row),gallery,gallery||!dayHeading?0:dayHeadingHeight());
 const int width=gallery?body.width():qMax(20,historyColumns(body).item.width()-12);
 const int lineTop=gallery?body.top():body.top()+Graphite::scaled(14);
 return {body.left(),lineTop+Graphite::scaled(48),width,0};
}
namespace {
class HistoryDelegate final:public QStyledItemDelegate {
public:
 explicit HistoryDelegate(QListView* view):QStyledItemDelegate(view),view_(view){view_->viewport()->installEventFilter(this);}
 bool eventFilter(QObject* watched,QEvent* event)override{
  if(watched==view_->viewport()&&event->type()==QEvent::Resize){
   // A scrollbar changes the wrapping width after the first layout. Recompute
   // the scroll range from that final width so an oversized error's last lines
   // remain reachable, including after selecting a row or resizing the splitter.
   QTimer::singleShot(0,view_,[view=view_]{view->doItemsLayout();});
  }
  return QStyledItemDelegate::eventFilter(watched,event);
 }
 std::function<void(const QModelIndex&)> primary;
 std::function<void(const QModelIndex&,const QPoint&)> menu;
 // True while the inspector sits beside the list. It holds the selected
 // row's actions; other failed rows keep inline recovery.
 std::function<bool()> inspectorBeside;
 bool grid()const{return view_->viewMode()==QListView::IconMode;}
 bool menuOnly()const{return !grid()&&inspectorBeside&&inspectorBeside();}
 bool beside(const QModelIndex&)const{return menuOnly();}
 bool recovery(const QModelIndex& i)const{return i.data(HistoryModel::PrimaryActionRole).toString().startsWith("Retry ");}
 bool primaryShown(const QModelIndex& i)const{return !i.data(HistoryModel::PrimaryActionRole).toString().isEmpty()&&(!beside(i)||(recovery(i)&&!view_->selectionModel()->isSelected(i)));}
 int heading(const QModelIndex& i)const{return !grid()&&!i.data(HistoryModel::DayRole).toString().isEmpty()?dayHeadingHeight():0;}
 QRect card(QRect r,const QModelIndex& i)const{return r.adjusted(6,heading(i)+2,-6,-2);}
 // Gallery cards share each row evenly. The preferred width grows with the
 // text size, so enlarged text gives fewer columns, not heavily elided names.
 // The flow layout always reserves a scrollbar's extent (QListViewPrivate::prepareItemsLayout).
 int galleryWidth()const{const int s=view_->spacing(),reserve=view_->style()->pixelMetric(QStyle::PM_ScrollView_ScrollBarOverlap,nullptr,view_->verticalScrollBar())?0:view_->style()->pixelMetric(QStyle::PM_ScrollBarExtent,nullptr,view_->verticalScrollBar());const int available=qMax(160,view_->maximumViewportSize().width()-reserve-2*s);const int columns=qMax(1,available/(Graphite::scaled(248)+s));return qMax(160,available/columns-s);}
 // Reserve action space even while it is hidden, so hovering never changes
 // the filename. Narrow rows put actions below instead of losing the title.
 int primaryWidth(const QModelIndex& i)const{return qMax(84,QFontMetrics(Graphite::font()).horizontalAdvance(i.data(HistoryModel::PrimaryActionRole).toString())+28);}
 bool actionsBelow(const QRect& r,const QModelIndex& i)const{
  return !grid()&&((menuOnly()&&recovery(i))||historyColumns(rowBody(r,false,heading(i)),menuOnly()).actions.isEmpty());
 }
 QRect primaryRect(QRect r,const QModelIndex& i)const{
  const auto c=card(r,i);const int h=actionHeight();const int w=qMin(primaryWidth(i),qMax(0,c.width()-12-12-h-8));
  if(grid())return QRect(c.left()+12,c.bottom()-h-11,w,h);
  return QRect(c.right()-12-h-8-w,actionsBelow(r,i)?c.bottom()-h-11:c.top()+Graphite::scaled(22)-6,w,h);
 }
 QRect menuRect(QRect r,const QModelIndex& i)const{const auto b=primaryRect(r,i);const auto c=card(r,i);const int h=actionHeight();return QRect(c.right()-12-h,b.top(),h,h);}
 static QString errorText(const QModelIndex& i){return HistoryModel::statusText(i.data(HistoryModel::RecordRole).toJsonObject());}
 int textLeft(const QRect& c)const{return c.left()+12+Graphite::Metric::thumbWidth+16;}
 QSize sizeHint(const QStyleOptionViewItem&,const QModelIndex& i)const override{
  if(grid()){
   // Picture, the measured title, metadata and complete error, a 12 px gap,
   // then the action row: actions never sit over the text.
   const int width=galleryWidth();int text=Graphite::scaled(45);
   if(i.data(HistoryModel::FailedRole).toBool()){const auto area=HistoryWindow::failureTextArea(QSize(width,0),true,false);text=Graphite::scaled(48)+int(std::ceil(failureText(errorText(i),Graphite::font(Graphite::Type::body),area.width())->size().height()));}
   return {width,2+1+galleryPicture(width-12)+10+text+12+actionHeight()+12+2};
  }
  const int width=qMax(160,view_->viewport()->width()-2*view_->spacing());
  int height=qMax(Graphite::Metric::historyRow,Graphite::scaled(Graphite::Metric::historyRow))+heading(i);
  if(actionsBelow(QRect(0,0,width,height),i))height+=actionHeight()+12;
  if(i.data(HistoryModel::FailedRole).toBool()){
   const auto area=HistoryWindow::failureTextArea(QSize(width,height),false,heading(i)>0);
   const auto document=failureText(errorText(i),Graphite::font(Graphite::Type::body),area.width());
   height+=int(std::ceil(document->size().height()))+4;
  }
  return {width,height};
 }
 void paint(QPainter* p,const QStyleOptionViewItem& option,const QModelIndex& i)const override{
  const auto& t=Theme::current();p->save();p->setRenderHint(QPainter::Antialiasing);const auto outer=option.rect;const auto group=i.data(HistoryModel::DayRole).toString();
  if(heading(i))Graphite::paintText(*p,QRectF(outer.left()+18,outer.top()+6,outer.width()-36,heading(i)-6),group,t.textMuted,Graphite::font(Graphite::Type::micro,true));
  const auto r=card(outer,i);const bool chosen=option.state&QStyle::State_Selected;const bool hover=option.state&QStyle::State_MouseOver;const bool failed=i.data(HistoryModel::FailedRole).toBool();
  const auto record=i.data(HistoryModel::RecordRole).toJsonObject();const auto op=record["operation"].toObject();
  // Row: quiet hover, the selected fill and a small amber marker.
  if(grid()){p->setPen(QPen(chosen?t.accent:t.border,chosen?2:1));p->setBrush(chosen?t.selected:hover?t.surfaceRaised:t.surface);p->drawRoundedRect(QRectF(r).adjusted(0.5,0.5,-0.5,-0.5),8,8);}
  else{p->setPen(Qt::NoPen);p->setBrush(chosen?t.selected:hover?t.surface:t.bg);p->drawRoundedRect(r,7,7);}
  if(chosen&&!grid()){p->setPen(Qt::NoPen);p->setBrush(t.accent);p->drawRoundedRect(QRectF(r.left()+3,r.top()+18,2.5,qMax(10,r.height()-36)),1.25,1.25);}
  const int rowTop=r.top();const int rowHeight=grid()?r.height():Graphite::scaled(Graphite::Metric::historyRow)-4;
  QRect thumb=grid()?QRect(r.left()+1,r.top()+1,r.width()-2,galleryPicture(r.width())):QRect(r.left()+12,rowTop+(rowHeight-Graphite::Metric::thumbHeight)/2,Graphite::Metric::thumbWidth,Graphite::Metric::thumbHeight);
  p->setPen(Qt::NoPen);p->setBrush(t.surfaceRaised);p->drawRoundedRect(thumb,grid()?8:5,grid()?8:5);
  const auto image=qvariant_cast<QImage>(i.data(HistoryModel::ThumbnailRole));
  if(!image.isNull()){const auto scaled=image.scaled(thumb.size(),Qt::KeepAspectRatio,Qt::SmoothTransformation);const QPoint origin(thumb.center().x()-scaled.width()/2,thumb.center().y()-scaled.height()/2);p->drawImage(origin,scaled);if(video(record))paintVideoBadge(*p,QRect(origin,scaled.size()),HistoryModel::duration(image.text(HistoryModel::durationKey).toLongLong()));}
  else{
   // No preview: an honest file-type glyph, never a fabricated picture.
   const auto category=record["category"].toString();const int side=grid()?36:24;
   VisualStyle::icon(category=="images"?"images":category=="videos"?"record":category=="text"?"clipboard":"view-list").paint(p,QRect(thumb.center()-QPoint(side/2,side/2),QSize(side,side)));
  }
  // The selected row beside its inspector keeps its columns; the inspector
  // holds its actions, and hovering offers only the row menu.
  const auto action=i.data(HistoryModel::PrimaryActionRole).toString();const bool showPrimary=primaryShown(i);const bool actionsShown=recovery(i)||grid()||hover||(chosen&&!beside(i));
  const auto primaryBounds=primaryRect(outer,i);const auto moreBounds=menuRect(outer,i);
  const QRect body=rowBody(outer,grid(),heading(i));
  const auto columns=grid()?Columns{body,{},{},{}}:historyColumns(body,menuOnly());
  const bool below=actionsBelow(outer,i);
  int itemWidth=qMax(0,columns.item.width()-12);
  if(!grid()&&!below){const int actionLeft=showPrimary?primaryBounds.left():moreBounds.left();itemWidth=qMin(itemWidth,qMax(0,actionLeft-columns.item.left()-8));}
  else if(grid())itemWidth=body.width();
  const auto titleFont=Graphite::font(Graphite::Type::body,true),metaFont=Graphite::font(Graphite::Type::metadata);
  const int lineTop=grid()?body.top():rowTop+Graphite::scaled(14);
  Graphite::paintText(*p,QRectF(columns.item.left(),lineTop,itemWidth,Graphite::scaled(22)),i.data().toString(),t.text,titleFont,Qt::AlignLeft|Qt::AlignVCenter,Qt::ElideMiddle);
  const bool copied=op["copyStatus"]=="copied",suppressed=op["copyStatus"]=="suppressed";
  auto subtitle=i.data(HistoryModel::SubtitleRole).toString();
  // The complete error below already says "Saved locally" when it applies.
  if(failed)subtitle=HistoryModel::failureLabel(record);
  else if(!grid()){const auto created=i.data(HistoryModel::CreatedRole).toString();if(!columns.created.isEmpty()&&!created.isEmpty())subtitle.remove(" · "+created);}
  const QColor subtitleColour=failed?t.danger:suppressed?t.warning:copied?t.success:t.textMuted;
  Graphite::paintText(*p,QRectF(columns.item.left(),lineTop+Graphite::scaled(25),itemWidth,Graphite::scaled(20)),subtitle,subtitleColour,metaFont);
  if(failed){
   const auto area=HistoryWindow::failureTextArea(outer.size(),grid(),heading(i)>0).translated(outer.topLeft());
   const auto document=failureText(errorText(i),Graphite::font(Graphite::Type::body),area.width());
   p->save();p->translate(area.topLeft());QAbstractTextDocumentLayout::PaintContext context;context.palette.setColor(QPalette::Text,t.textSecondary);document->documentLayout()->draw(p,context);p->restore();
  }
  if(!grid()){
   if(!columns.output.isEmpty())Graphite::paintText(*p,QRectF(columns.output.left(),lineTop,columns.output.width()-8,Graphite::scaled(22)),i.data(HistoryModel::OutputRole).toString(),t.textSecondary,metaFont);
   if(!columns.created.isEmpty())Graphite::paintText(*p,QRectF(columns.created.left(),lineTop,columns.created.width()-8,Graphite::scaled(22)),i.data(HistoryModel::CreatedRole).toString(),t.textSecondary,metaFont);
  }
  if(actionsShown){
   auto* style=view_->style();
   if(showPrimary){QStyleOptionButton button;button.rect=primaryBounds;button.text=action;button.state=QStyle::State_Enabled|(option.state&QStyle::State_MouseOver&&primaryBounds.contains(view_->viewport()->mapFromGlobal(QCursor::pos()))?QStyle::State_MouseOver:QStyle::State_None);button.fontMetrics=QFontMetrics(Graphite::font());p->save();p->setFont(Graphite::font());style->drawControl(QStyle::CE_PushButton,&button,p,nullptr);p->restore();}
   QStyleOptionButton more;more.rect=moreBounds;more.state=QStyle::State_Enabled|(moreBounds.contains(view_->viewport()->mapFromGlobal(QCursor::pos()))?QStyle::State_MouseOver:QStyle::State_None);style->drawPrimitive(QStyle::PE_PanelButtonCommand,&more,p,nullptr);const qreal inset=moreBounds.width()*7.0/32;Graphite::paintChromeGlyph(*p,QRectF(moreBounds).adjusted(inset,inset,-inset,-inset),"more",t.textSecondary);
  }
  if(i.data(HistoryModel::ActiveRole).toBool()){const int progress=i.data(HistoryModel::ProgressRole).toInt();if(progress>=0){QRectF bar(columns.item.left(),grid()?primaryBounds.top()-8:rowTop+rowHeight-9,qMax(20,itemWidth),3);p->setPen(Qt::NoPen);p->setBrush(t.selected);p->drawRoundedRect(bar,1.5,1.5);bar.setWidth(bar.width()*progress/100);p->setBrush(t.accent);p->drawRoundedRect(bar,1.5,1.5);}}
  if((option.state&QStyle::State_HasFocus)&&view_->hasFocus()&&view_->window()->testAttribute(Qt::WA_KeyboardFocusChange)){p->setPen(QPen(t.accent,1));p->setBrush(Qt::NoBrush);p->drawRoundedRect(QRectF(r).adjusted(0.5,0.5,-0.5,-0.5),7,7);}
  p->restore();
 }
 bool editorEvent(QEvent* event,QAbstractItemModel*,const QStyleOptionViewItem& option,const QModelIndex& i)override{
  if(event->type()!=QEvent::MouseButtonRelease)return false;
  auto* mouse=static_cast<QMouseEvent*>(event);if(mouse->button()!=Qt::LeftButton)return false;
  if(primaryShown(i)&&primaryRect(option.rect,i).contains(mouse->position().toPoint())){if(primary)primary(i);return true;}
  if(menuRect(option.rect,i).contains(mouse->position().toPoint())){if(menu)menu(i,mouse->globalPosition().toPoint());return true;}return false;
 }
private:QListView* view_;
};
}
HistoryWindow::HistoryWindow(UiSession& session,QWidget* parent):RetainedWindow(parent),session_(session){
 chooser_=new FileDialog(this);
 setObjectName("historyPage");setWindowTitle("put.gd — History");resize(1080,760);auto* page=new QHBoxLayout(this);page->setContentsMargins(0,0,0,0);page->setSpacing(0);
 // Sidebar: categories and failed uploads.
 auto* sidebar=new Graphite::Surface("surface",Qt::RightEdge,this);sidebar->setObjectName("historySidebar");sidebar->setFixedWidth(Graphite::scaled(Graphite::Metric::historySidebar));page->addWidget(sidebar);
 auto* side=new QVBoxLayout(sidebar);side->setContentsMargins(10,16,10,14);side->setSpacing(8);
 categories_=new QListWidget(sidebar);categories_->setObjectName("historyCategories");categories_->setAccessibleName("Show history items");categories_->setFrameShape(QFrame::NoFrame);categories_->setIconSize(QSize(18,18));categories_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);categories_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);categories_->setItemDelegate(new Graphite::NavigationDelegate(categories_));Graphite::transparentView(categories_);categories_->setMouseTracking(true);
 for(const auto& [glyph,label,category]:QList<std::tuple<QString,QString,QString>>{{"history","All items",""},{"images","Images","images"},{"record","Recordings","videos"},{"view-list","Other files","text+other"}}){auto* item=new QListWidgetItem(VisualStyle::icon(glyph),label,categories_);item->setData(Qt::UserRole,category);}
 categories_->setToolTip("Other files includes clipboard text and any file that is not an image or a recording.");
 categories_->setCurrentRow(0);
 // Wide enough for its labels at the current text size, never more than the
 // shared sidebar cap (the Settings rails measure the same way).

 categories_->setFixedHeight(4*(Graphite::scaled(Graphite::Metric::navRow))+4);side->addWidget(categories_);side->addWidget(Graphite::rule(sidebar));
 failedOnly_=new QCheckBox("Failed uploads only",sidebar);failedOnly_->setFont(Graphite::font());failedOnly_->setObjectName("historyFailedOnly");failedOnly_->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);failedOnly_->setAccessibleName("Failed uploads only");failedOnly_->setToolTip("Show only failed uploads (Upload state: failed)");side->addWidget(failedOnly_);
 {const QFontMetrics strong(Graphite::font(Graphite::Type::body,true));int widest=0;for(int i=0;i<categories_->count();++i)widest=qMax(widest,strong.horizontalAdvance(categories_->item(i)->text()));const int filter=failedOnly_->sizeHint().width()+20;sidebar->setFixedWidth(Graphite::sidebarWidth(Graphite::Metric::historySidebar,qMax(widest+74,filter)));}
 side->addStretch();
 auto* content=new QWidget(this);content->setObjectName("historyContent");page->addWidget(content,1);auto* layout=new QVBoxLayout(content);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);
 auto* headerBar=new QWidget(content);auto* header=new QHBoxLayout(headerBar);header->setContentsMargins(18,12,16,10);header->setSpacing(8);// The page is already named by the navigation; the heading names what is shown.
 // Long combined headings ("Recordings · failed uploads") elide before they crowd search.
 // The compact layout opens the inspector over the list; this returns to it.
 detailsBack_=Graphite::toolButton("Back to list",{},"quiet",this);detailsBack_->setObjectName("historyDetailsBack");detailsBack_->setIcon(Graphite::chromeIcon("chevron-left"));detailsBack_->setIconSize(QSize(14,14));detailsBack_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);detailsBack_->setToolTip("Return to the list (Esc)");detailsBack_->hide();header->addWidget(detailsBack_);connect(detailsBack_,&QToolButton::clicked,this,[this]{setDetailsOpen(false);});
 {auto* back=new QAction(this);back->setObjectName("historyDetailsEscape");back->setShortcut(QKeySequence(Qt::Key_Escape));back->setShortcutContext(Qt::WidgetWithChildrenShortcut);back->setEnabled(false);addAction(back);connect(back,&QAction::triggered,this,[this]{setDetailsOpen(false);});}
 auto* heading=new Graphite::ElidedLabel(this);heading->setText("All items");heading->setObjectName("historyHeading");heading->setTextFormat(Qt::PlainText);VisualStyle::role(heading,"sectionTitle");header->addWidget(heading);header->addStretch();
 auto* search=new QLineEdit(this);search->setObjectName("historySearch");search->setAccessibleName("Search history by file name");search->setPlaceholderText("Search history");search->setMaxLength(256);search->setClearButtonEnabled(true);search->setMinimumWidth(160);search->setMaximumWidth(300);header->addWidget(search,1);
 auto* advancedToggle=new QToolButton(this);advancedToggle->setObjectName("historyFiltersExpanded");advancedToggle->setText("Filters");Graphite::disclosure(advancedToggle);advancedToggle->setAccessibleName("Show advanced history filters");header->addWidget(advancedToggle);filtersToggle_=advancedToggle;
 // Restrictions stay visible while the filters are collapsed: "Filters · N" and the way back.
 clearFilters_=Graphite::toolButton("Clear filters",{},"quiet",this);clearFilters_->setObjectName("historyClearFilters");clearFilters_->setToolTip("Clear the advanced filters; search and the category stay");clearFilters_->hide();header->addWidget(clearFilters_);connect(clearFilters_,&QToolButton::clicked,this,[this]{clearFilters(false);});
 // List and Gallery: one segmented choice.
 auto* listButton=new QToolButton(this);listButton->setObjectName("historyListMode");listButton->setText("List");listButton->setCheckable(true);listButton->setChecked(true);listButton->setAccessibleName("Show history as a list");grid_=new QToolButton(this);grid_->setObjectName("historyGridMode");grid_->setText("Gallery");grid_->setCheckable(true);grid_->setAccessibleName("Show history as a thumbnail gallery");auto* views=new Graphite::SegmentTrack(this);views->setObjectName("historyViewMode");views->setAccessibleName("History view");for(auto* b:{listButton,grid_}){b->setFocusPolicy(Qt::StrongFocus);views->addButton(b);}header->addWidget(views);
 layout->addWidget(headerBar);
 connect(listButton,&QToolButton::clicked,this,[this]{setDetailsOpen(false);setGridMode(false);});connect(grid_,&QToolButton::clicked,this,[this]{setDetailsOpen(false);setGridMode(true);});
 auto* advanced=new QWidget(this);auto* advancedBox=new QVBoxLayout(advanced);advancedBox->setContentsMargins(18,0,16,10);advancedBox->setSpacing(8);auto* filters=new QGridLayout;filters->setContentsMargins(0,0,0,0);filters->setHorizontalSpacing(8);filters->setVerticalSpacing(8);const QStringList keys{"from","to","mime","profileId","uploadStatus","localStatus","remoteStatus"};const QStringList labels{"From date","Through date","MIME type","Destination","Upload state","Local-copy state","Remote file state"};
 auto* technical=new QWidget(advanced);auto* techBox=new QVBoxLayout(technical);techBox->setContentsMargins(0,0,0,0);technical->hide();
 for(int i=0;i<keys.size();++i){const auto key=keys[i];auto* raw=new QLineEdit(advanced);raw->setMaxLength(256);raw->setObjectName("historyFilter"+key);raw->setAccessibleName(labels[i]);raw->hide();connect(raw,&QLineEdit::returnPressed,this,&HistoryWindow::applyFilters);
  auto* cell=new QWidget(advanced);auto* field=new QVBoxLayout(cell);field->setContentsMargins(0,0,0,0);field->setSpacing(4);auto* label=new QLabel(labels[i],cell);label->setWordWrap(true);VisualStyle::role(label,"meta");field->addWidget(label);
  if(key=="from"||key=="to"){
   auto* line=new QHBoxLayout;line->setContentsMargins(0,0,0,0);auto* enabled=new QCheckBox("Set date",cell);enabled->setObjectName("historyDateEnabled"+key);enabled->setAccessibleName("Set "+labels[i].toLower());auto* date=new QDateEdit(QDate::currentDate(),cell);date->setObjectName("historyDate"+key);date->setDisplayFormat(QLocale().dateFormat(QLocale::ShortFormat));date->setCalendarPopup(true);date->setEnabled(false);date->setAccessibleName(labels[i]);label->setBuddy(date);line->addWidget(enabled);line->addWidget(date,1);field->addLayout(line);
   auto dateValue=[key](QDate d){return QDateTime(d,key=="to"?QTime(23,59,59,999):QTime(0,0)).toUTC().toString(Qt::ISODateWithMs);};connect(enabled,&QCheckBox::toggled,this,[raw,date,dateValue](bool on){date->setEnabled(on);raw->setText(on?dateValue(date->date()):QString{});});connect(date,&QDateEdit::dateChanged,this,[raw,enabled,dateValue](QDate d){if(enabled->isChecked())raw->setText(dateValue(d));});
   connect(raw,&QLineEdit::textChanged,this,[date,enabled](const QString& value){const QSignalBlocker a(date),b(enabled);const auto parsed=value.contains('T')?QDateTime::fromString(value,Qt::ISODateWithMs).toLocalTime().date():QDate::fromString(value,Qt::ISODate);enabled->setChecked(!value.isEmpty());date->setEnabled(!value.isEmpty());if(parsed.isValid())date->setDate(parsed);});
  }else if(key=="mime"){raw->setParent(cell);raw->show();raw->setPlaceholderText("Any type, e.g. image/png");label->setBuddy(raw);field->addWidget(raw);}
  else{
   auto* combo=new QComboBox(cell);combo->setObjectName("historyChoice"+key);combo->setAccessibleName(labels[i]);combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);combo->setMinimumContentsLength(10);combo->addItem("Any",QString{});label->setBuddy(combo);field->addWidget(combo);
   QList<std::pair<QString,QString>> choices;
   if(key=="uploadStatus")choices={{"uploaded","Uploaded"},{"local_only","Local only"},{"pending","Waiting to upload"},{"uploading","Uploading"},{"failed","Upload failed"},{"unknown","Outcome unknown"},{"cancelled","Cancelled"},{"not_requested","Not requested"},{"skipped","Skipped"},{"conflict","Conflict"}};
   else if(key=="localStatus")choices={{"saved","Saved locally"},{"disabled","Not requested"},{"pending","Waiting to save"},{"failed","Save failed"}};
   else if(key=="remoteStatus")choices={{"not_requested","Deletion not requested"},{"present","Available"},{"pending","Waiting for deletion"},{"deleting","Deleting"},{"removed","Deleted"},{"already_missing","Already missing"},{"failed","Deletion failed"},{"unknown","Outcome unknown"},{"changed","Remote file changed"},{"superseded","Replaced by newer upload"},{"hidden_version_retained","Hidden; version retained"},{"version_retained","Version retained"},{"unsupported_safe_removal","Safe removal unavailable"}};
   for(const auto& [value,name]:choices)combo->addItem(name,value);
   if(key=="profileId"){
    auto refresh=[this,combo,raw]{const QSignalBlocker block(combo);const auto value=raw->text();combo->clear();combo->addItem("Any destination",QString{});for(const auto& v:session_.document()["profiles"].toArray()){const auto profile=v.toObject();combo->addItem(profile["label"].toString(),profile["id"].toString());}int index=combo->findData(value);if(index<0){combo->addItem("Unavailable destination",value);index=combo->count()-1;}combo->setCurrentIndex(index);};refresh();connect(&session_,&UiSession::accepted,this,refresh);
    auto* idLabel=new QLabel("Destination ID",technical);idLabel->setBuddy(raw);techBox->addWidget(idLabel);raw->setParent(technical);raw->show();techBox->addWidget(raw);
   }
   connect(combo,&QComboBox::currentIndexChanged,this,[combo,raw]{raw->setText(combo->currentData().toString());});connect(raw,&QLineEdit::textChanged,this,[combo,key](const QString& value){const QSignalBlocker block(combo);int index=combo->findData(value);if(index<0){combo->addItem(key=="profileId"?"Unavailable destination":"Other recorded state",value);index=combo->count()-1;}combo->setCurrentIndex(index);});
  }
  if(key=="mime"){cell->setParent(technical);techBox->insertWidget(0,cell);}else{const int position=i>2?i-1:i;filters->addWidget(cell,position/2,position%2);}
 }
 advancedBox->addLayout(filters);
 auto* techToggle=new QToolButton(advanced);techToggle->setObjectName("historyTechnicalFiltersExpanded");techToggle->setText("Technical filters");Graphite::disclosure(techToggle);advancedBox->addWidget(techToggle,0,Qt::AlignLeft);advancedBox->addWidget(technical);connect(techToggle,&QToolButton::toggled,technical,&QWidget::setVisible);
 auto* filterScroll=new QScrollArea(this);filterScroll->setObjectName("historyFiltersScroll");filterScroll->setWidgetResizable(true);filterScroll->setFrameShape(QFrame::NoFrame);filterScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);filterScroll->setMaximumHeight(240);filterScroll->setWidget(advanced);Graphite::transparentView(filterScroll);layout->addWidget(filterScroll);filterScroll->hide();connect(advancedToggle,&QToolButton::toggled,filterScroll,&QWidget::setVisible);

 activeBanner_=new QWidget(this);activeBanner_->setObjectName("historyActiveBanner");auto* activeLayout=new QHBoxLayout(activeBanner_);activeLayout->setContentsMargins(18,6,16,8);activeLayout->setSpacing(10);activeLabel_=new QLabel(this);activeLabel_->setTextFormat(Qt::PlainText);activeLabel_->setWordWrap(true);activeLayout->addWidget(activeLabel_,1);progress_=new QProgressBar(this);progress_->setAccessibleName("Active upload progress");progress_->setTextVisible(false);progress_->setMaximumWidth(180);activeLayout->addWidget(progress_);auto* activeCancel=new QPushButton("Cancel",this);activeCancel->setAccessibleName("Cancel active capture or upload");activeLayout->addWidget(activeCancel);connect(activeCancel,&QPushButton::clicked,this,[this]{if(activeId_.isEmpty())return;session_.request("operation.cancel",{{"id",activeId_}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g){VisualStyle::feedback(g->status_,e?e.message:"Cancellation requested.",bool(e));g->refresh();}});});layout->addWidget(activeBanner_);activeBanner_->hide();
 previewRequestTimer_=new QTimer(this);previewRequestTimer_->setSingleShot(true);previewRequestTimer_->setInterval(400);connect(previewRequestTimer_,&QTimer::timeout,this,&HistoryWindow::requestPreview);
 searchTimer_=new QTimer(this);searchTimer_->setSingleShot(true);searchTimer_->setInterval(250);connect(searchTimer_,&QTimer::timeout,this,&HistoryWindow::applyFilters);connect(search,&QLineEdit::textChanged,this,[this]{searchTimer_->start();});connect(search,&QLineEdit::returnPressed,this,&HistoryWindow::applyFilters);for(const auto& key:keys)connect(findChild<QLineEdit*>("historyFilter"+key),&QLineEdit::textChanged,this,[this]{searchTimer_->start();});
 connect(categories_,&QListWidget::currentRowChanged,this,[this]{if(restoringCategory_)return;applyFilters();});
 heading_=heading;connect(categories_,&QListWidget::currentRowChanged,this,&HistoryWindow::updateHeading);connect(failedOnly_,&QCheckBox::toggled,this,&HistoryWindow::updateHeading);
 connect(failedOnly_,&QCheckBox::toggled,this,[this](bool on){auto* state=findChild<QLineEdit*>("historyFilteruploadStatus");if(on)state->setText("failed");else if(state->text()=="failed")state->clear();applyFilters();});
 model_=new HistoryModel(session,this);table_=new QListView(this);table_->setObjectName("historyList");table_->setAccessibleName("Captures and upload activity");table_->setModel(model_);table_->setFrameShape(QFrame::NoFrame);Graphite::transparentView(table_);table_->setMouseTracking(true);table_->setSelectionMode(QAbstractItemView::ExtendedSelection);table_->setResizeMode(QListView::Adjust);table_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);table_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);table_->setSpacing(1);table_->setContextMenuPolicy(Qt::CustomContextMenu);auto* delegate=new HistoryDelegate(table_);table_->setItemDelegate(delegate);delegate->primary=[this](const QModelIndex& i){primaryAction(i);};delegate->menu=[this](const QModelIndex& i,const QPoint& p){showRowMenu(i,p,true);};delegate->inspectorBeside=[this]{return !compact_&&inspectorScroll_&&!inspectorScroll_->isHidden();};auto* openDetails=new QAction(this);openDetails->setShortcuts({QKeySequence(Qt::Key_Return),QKeySequence(Qt::Key_Enter)});openDetails->setShortcutContext(Qt::WidgetShortcut);table_->addAction(openDetails);connect(openDetails,&QAction::triggered,this,[this]{if(table_->selectionModel()->selectedRows().size()==1)openSelectedDetails();});connect(table_,&QListView::doubleClicked,this,[this]{if(table_->selectionModel()->selectedRows().size()==1)openSelectedDetails();});connect(table_,&QWidget::customContextMenuRequested,this,[this](const QPoint& p){const auto i=table_->indexAt(p);if(i.isValid())showRowMenu(i,table_->viewport()->mapToGlobal(p));});auto* rowMenu=new QAction(this);rowMenu->setShortcuts({QKeySequence(Qt::Key_Menu),QKeySequence(Qt::SHIFT|Qt::Key_F10)});rowMenu->setShortcutContext(Qt::WidgetShortcut);table_->addAction(rowMenu);connect(rowMenu,&QAction::triggered,this,[this]{const auto i=table_->currentIndex();if(i.isValid())showRowMenu(i,table_->viewport()->mapToGlobal(table_->visualRect(i).center()));});
 // Empty history and no matching items say different things (applyFilters).
 emptyState_=new QWidget(this);emptyState_->setObjectName("historyEmptyState");{auto* box=new QVBoxLayout(emptyState_);box->setContentsMargins(24,24,24,24);box->setSpacing(8);box->addStretch(2);
  empty_=new QLabel("No captures yet",emptyState_);empty_->setObjectName("historyEmpty");empty_->setTextFormat(Qt::PlainText);empty_->setWordWrap(true);empty_->setAlignment(Qt::AlignCenter);VisualStyle::role(empty_,"sectionTitle");box->addWidget(empty_);
  emptyBody_=new QLabel(emptyState_);emptyBody_->setObjectName("historyEmptyHint");emptyBody_->setTextFormat(Qt::PlainText);emptyBody_->setWordWrap(true);emptyBody_->setAlignment(Qt::AlignCenter);VisualStyle::role(emptyBody_,"muted");box->addWidget(emptyBody_);
  resetView_=new QPushButton("Clear search and filters",emptyState_);resetView_->setObjectName("historyResetView");resetView_->hide();retryLoading_=new QPushButton("Retry loading",emptyState_);retryLoading_->setObjectName("historyRetryLoading");VisualStyle::role(retryLoading_,"primary");retryLoading_->setAccessibleName("Retry loading history with the current filters");retryLoading_->hide();box->addWidget(retryLoading_,0,Qt::AlignHCenter);connect(retryLoading_,&QPushButton::clicked,this,&HistoryWindow::refresh);box->addSpacing(4);box->addWidget(resetView_,0,Qt::AlignHCenter);connect(resetView_,&QPushButton::clicked,this,[this]{clearFilters(true);});box->addStretch(3);}
 auto* listPane=new QWidget(this);auto* listBox=new QVBoxLayout(listPane);listBox->setContentsMargins(0,0,0,0);listBox->setSpacing(0);auto* columnHeader=new HistoryColumns(table_,listPane);columns_=columnHeader;columnHeader->menuOnly=[this]{return table_->viewMode()!=QListView::IconMode&&!compact_&&inspectorScroll_&&!inspectorScroll_->isHidden();};listBox->addWidget(columns_);listBox->addWidget(table_,1);
 // Several selected rows: what they can do together.
 selectionBar_=new Graphite::Surface("surface",Qt::TopEdge,listPane);selectionBar_->setObjectName("historySelectionBar");selectionBar_->setAccessibleName("Selection actions");{auto* bar=new QHBoxLayout(selectionBar_);bar->setContentsMargins(18,8,16,8);bar->setSpacing(8);
  selectionCount_=new QLabel(selectionBar_);selectionCount_->setObjectName("historySelectionCount");selectionCount_->setTextFormat(Qt::PlainText);VisualStyle::role(selectionCount_,"strong");bar->addWidget(selectionCount_);bar->addStretch();
  auto* details=new QAction("Show details",this);details->setObjectName("historyShowDetails");details->setToolTip("Open the selected capture's files, link and actions (Enter)");connect(details,&QAction::triggered,this,[this]{openSelectedDetails();});
  copyLinks_=new QPushButton("Copy links",selectionBar_);copyLinks_->setObjectName("historyCopyLinks");copyLinks_->setToolTip("Copy the stored links of every selected file, one per line");bar->addWidget(copyLinks_);connect(copyLinks_,&QPushButton::clicked,this,&HistoryWindow::copySelectedLinks);
  deleteSelected_=new QPushButton("Delete from destination…",selectionBar_);deleteSelected_->setObjectName("historyDeleteSelected");VisualStyle::role(deleteSelected_,"danger");bar->addWidget(deleteSelected_);connect(deleteSelected_,&QPushButton::clicked,this,[this]{const auto ids=deletableIds();prepareDelete(ids,int(selectedFiles().size())-int(ids.size()));});
  auto* clearSelection=new QPushButton("Clear selection",selectionBar_);clearSelection->setObjectName("historyClearSelection");VisualStyle::role(clearSelection,"quiet");bar->addWidget(clearSelection);connect(clearSelection,&QPushButton::clicked,this,[this]{table_->clearSelection();table_->setFocus();});}
 selectionBar_->hide();listBox->addWidget(selectionBar_);
 list_=new QStackedWidget(this);list_->addWidget(listPane);list_->addWidget(emptyState_);
 split_=new QSplitter(Qt::Horizontal,this);split_->setObjectName("historySplitter");split_->setChildrenCollapsible(false);split_->addWidget(list_);inspectorScroll_=new QScrollArea(this);inspectorScroll_->setObjectName("historyInspectorScroll");inspectorScroll_->setWidgetResizable(true);inspectorScroll_->setFrameShape(QFrame::NoFrame);inspectorScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);auto* inspector=new Graphite::Surface("surface",{},this);inspector->setObjectName("historyInspector");auto* inspect=new QVBoxLayout(inspector);inspect->setContentsMargins(18,16,18,16);inspect->setSpacing(10);
 preview_=new QLabel("Select a capture to preview",this);preview_->setObjectName("historyPreview");VisualStyle::role(preview_,"well");preview_->setAccessibleName("Selected capture preview");preview_->setAlignment(Qt::AlignCenter);preview_->setWordWrap(true);preview_->setMinimumHeight(180);preview_->setMaximumHeight(260);preview_->setMargin(8);preview_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);inspect->addWidget(preview_);
 title_=new QLabel(this);title_->setTextFormat(Qt::PlainText);title_->setObjectName("historyTitle");title_->setWordWrap(true);VisualStyle::role(title_,"sectionTitle");inspect->addWidget(title_);outcome_=new QLabel(this);outcome_->setTextFormat(Qt::PlainText);outcome_->setWordWrap(true);outcome_->setObjectName("historyOutcome");inspect->addWidget(outcome_);retry_=new QPushButton("Retry upload",this);retry_->setObjectName("retryOperation");VisualStyle::role(retry_,"primary");inspect->addWidget(retry_);
 items_=new QComboBox(this);items_->setObjectName("historyItems");items_->setAccessibleName("Select a file in this capture");items_->hide();inspect->addWidget(items_);connect(items_,&QComboBox::currentIndexChanged,this,[this]{if(restoringSelection_)return;selectedItemId_=items_->currentData().toString();loadSelectedItem();emit viewStateChanged();});
 inspect->addWidget(Graphite::rule(inspector));
 link_=new QLineEdit(this);link_->setObjectName("historyLink");link_->setReadOnly(true);VisualStyle::role(link_,"link");link_->setAccessibleName("Stored share link");link_->setPlaceholderText("No share link available");inspect->addWidget(link_);
 linkExpiry_=new QLabel(this);linkExpiry_->setObjectName("historyLinkExpiry");linkExpiry_->setTextFormat(Qt::PlainText);VisualStyle::role(linkExpiry_,"meta");linkExpiry_->hide();inspect->addWidget(linkExpiry_);
 copy_=new QPushButton("Copy link",this);copy_->setObjectName("copyHistoryLink");VisualStyle::role(copy_,"primary");inspect->addWidget(copy_);
 auto* actions=new QGridLayout;actions->setSpacing(8);open_=new QPushButton("Open in browser",this);open_->setObjectName("openHistoryLink");copyImage_=new QPushButton("Copy image",this);copyImage_->setObjectName("copyHistoryImage");saveAs_=new QPushButton("Save a copy…",this);saveAs_->setObjectName("saveHistoryAs");saveAs_->setToolTip("Save a copy of the retained local original");actions->addWidget(open_,0,0);actions->addWidget(copyImage_,0,1);actions->addWidget(saveAs_,2,0);editAgain_=new QPushButton("Edit again",this);editAgain_->setObjectName("editHistoryImage");editAgain_->setToolTip("Edit the retained local PNG or JPEG; the result is a new item");actions->addWidget(editAgain_,2,1);connect(editAgain_,&QPushButton::clicked,this,[this]{editAgain(currentDetail_);});cancel_=new QPushButton("Cancel upload",this);cancel_->setObjectName("cancelOperation");actions->addWidget(cancel_,3,1);saveDiagnostics_=new QPushButton("Save diagnostics for this upload",this);saveDiagnostics_->setObjectName("saveHistoryDiagnostics");saveDiagnostics_->setToolTip("Save a redacted diagnostics package (.zip) for this upload on this computer. Nothing is uploaded.");actions->addWidget(saveDiagnostics_,4,0,1,2);saveDiagnostics_->hide();connect(saveDiagnostics_,&QPushButton::clicked,this,[this]{const auto rows=table_->selectionModel()->selectedRows();if(rows.size()==1)saveDiagnostics(model_->record(rows.first().row())["operationId"].toString());});actions->setColumnStretch(0,1);actions->setColumnStretch(1,1);inspect->addLayout(actions);browserFeedback_=new QLabel(inspector);browserFeedback_->setObjectName("historyBrowserFeedback");browserFeedback_->setWordWrap(true);browserFeedback_->setTextFormat(Qt::PlainText);VisualStyle::role(browserFeedback_,"danger");browserFeedback_->hide();actions->addWidget(browserFeedback_,1,0,1,2);
 share_=new QPushButton("Create share link",this);share_->setObjectName("shareHistoryLink");revoke_=new QPushButton("Revoke link…",this);revoke_->setObjectName("revokeHistoryLink");inspectLink_=new QPushButton("Inspect link",this);inspectLink_->setObjectName("inspectHistoryLink");auto* sharing=new QHBoxLayout;sharing->setSpacing(8);for(auto* b:{share_,revoke_,inspectLink_}){sharing->addWidget(b);b->hide();}inspect->addLayout(sharing);connect(share_,&QPushButton::clicked,this,[this]{changeSharing("create");});connect(revoke_,&QPushButton::clicked,this,[this]{changeSharing("revoke");});connect(inspectLink_,&QPushButton::clicked,this,[this]{changeSharing("inspect");});
 capabilityHint_=new QLabel(inspector);capabilityHint_->setObjectName("historyCapabilityHint");capabilityHint_->setWordWrap(true);capabilityHint_->setTextFormat(Qt::PlainText);inspect->addWidget(capabilityHint_);capabilityHint_->hide();changeLocalCopies_=new QPushButton("Change local-copy settings…",inspector);changeLocalCopies_->setObjectName("historyChangeLocalCopies");VisualStyle::role(changeLocalCopies_,"link");inspect->addWidget(changeLocalCopies_,0,Qt::AlignLeft);changeLocalCopies_->hide();connect(changeLocalCopies_,&QPushButton::clicked,this,&HistoryWindow::localCopySettingsRequested);
 inspect->addWidget(Graphite::rule(inspector));
 summary_=new HistorySummary(this);summary_->setObjectName("historySummary");summary_->setAccessibleName("File size, pixel dimensions, destination and outcomes");inspect->addWidget(summary_);
 auto* receiptToggle=new QToolButton(this);receiptToggle->setObjectName("historyReceiptExpanded");receiptToggle->setText("Advanced file details");Graphite::disclosure(receiptToggle);receiptToggle->setAccessibleName("Show advanced receipt details");inspect->addWidget(receiptToggle,0,Qt::AlignLeft);details_=new QPlainTextEdit(this);details_->setObjectName("historyDetails");details_->setReadOnly(true);VisualStyle::role(details_,"diagnostics");details_->setAccessibleName("Advanced original receipt details");details_->setMaximumHeight(180);details_->hide();previousLink_=new QWidget(inspector);previousLink_->setObjectName("historyPreviousLinkDetails");auto* previousBox=new QVBoxLayout(previousLink_);previousBox->setContentsMargins(0,0,0,0);previousBox->setSpacing(4);auto* previousLabel=new QLabel("Previous link — unavailable",previousLink_);VisualStyle::role(previousLabel,"meta");previousBox->addWidget(previousLabel);previousUrl_=new QLineEdit(previousLink_);previousUrl_->setObjectName("historyPreviousLink");previousUrl_->setReadOnly(true);previousUrl_->setAccessibleName(previousLabel->text());previousLabel->setBuddy(previousUrl_);previousBox->addWidget(previousUrl_);previousLink_->hide();inspect->addWidget(previousLink_);inspect->addWidget(details_);connect(receiptToggle,&QToolButton::toggled,details_,&QWidget::setVisible);connect(receiptToggle,&QToolButton::toggled,this,[this](bool on){previousLink_->setVisible(on&&!previousUrl_->text().isEmpty());});inspect->addStretch();
 remove_=new QPushButton("Delete from destination…",this);remove_->setObjectName("deleteHistoryRemote");VisualStyle::role(remove_,"danger");remove_->setToolTip("Delete the recorded remote file. Local copies and history records remain.");remove_->hide();inspect->addWidget(remove_,0,Qt::AlignLeft);connect(remove_,&QPushButton::clicked,this,qOverload<>(&HistoryWindow::prepareDelete));
 deletionStatus_=new QLabel(inspector);deletionStatus_->setObjectName("historyDeletionStatus");deletionStatus_->setTextFormat(Qt::PlainText);deletionStatus_->setWordWrap(true);inspect->addWidget(deletionStatus_);deletionStatus_->hide();
 deletionRetry_=new QPushButton("Retry deletion",inspector);deletionRetry_->setObjectName("retryHistoryDeletion");deletionCancel_=new QPushButton("Cancel deletion",inspector);deletionCancel_->setObjectName("cancelHistoryDeletion");
 for(auto* b:{deletionRetry_,deletionCancel_}){inspect->addWidget(b,0,Qt::AlignLeft);b->hide();}
 connect(deletionRetry_,&QPushButton::clicked,this,[this]{deletionAction("delete.retry");});connect(deletionCancel_,&QPushButton::clicked,this,[this]{deletionAction("delete.cancel");});
 inspectorScroll_->setWidget(inspector);split_->addWidget(inspectorScroll_);layout->addWidget(split_,1);inspectorScroll_->hide();
 auto* footerBar=new Graphite::Surface("bg",Qt::TopEdge,content);auto* footerBox=new QVBoxLayout(footerBar);footerBox->setContentsMargins(16,8,16,10);footerBox->setSpacing(8);
 auto* footer=new QHBoxLayout;footer->setSpacing(8);// Counts elide (full text in the tooltip) before they crowd the paging controls.
 count_=new Graphite::ElidedLabel(this);count_->setObjectName("historyCount");VisualStyle::role(count_,"meta");footer->addWidget(count_,1);cancelled_=new QLabel(this);cancelled_->setObjectName("historyCancelledCount");cancelled_->setTextFormat(Qt::PlainText);VisualStyle::role(cancelled_,"meta");cancelled_->setAlignment(Qt::AlignRight|Qt::AlignVCenter);footer->addWidget(cancelled_);showCancelled_=new QToolButton(this);showCancelled_->setObjectName("historyShowCancelled");showCancelled_->setText("Show");showCancelled_->setCheckable(true);showCancelled_->setProperty("visualRole","quiet");showCancelled_->setAccessibleName("Show cancelled captures");footer->addWidget(showCancelled_);connect(showCancelled_,&QToolButton::toggled,this,[this](bool show){model_->setShowCancelled(show);showCancelled_->setText(show?"Hide":"Show");showCancelled_->setAccessibleName(show?"Hide cancelled captures":"Show cancelled captures");emit viewStateChanged();});auto* previous=new QPushButton("Previous",this);previous->setObjectName("historyPrevious");auto* next=new QPushButton("Next",this);next->setObjectName("historyNext");// Paging appears only when there is another page.
 previous->hide();next->hide();footer->addWidget(previous);footer->addWidget(next);connect(previous,&QPushButton::clicked,this,[this]{auto q=model_->query();q.offset=qMax(0,q.offset-q.limit);model_->setQuery(q);});connect(next,&QPushButton::clicked,this,[this]{auto q=model_->query();if(q.offset+q.limit<model_->total()){q.offset+=q.limit;model_->setQuery(q);}});auto* managementToggle=new QToolButton(this);managementToggle->setObjectName("historyManagementExpanded");managementToggle->setText("Manage");Graphite::disclosure(managementToggle);managementToggle->setAccessibleName("Show history management actions");footer->addWidget(managementToggle);footerBox->addLayout(footer);
 status_=new QLabel("Select a capture to inspect its files and share link.",this);status_->setTextFormat(Qt::PlainText);status_->setAccessibleName("History status");status_->setWordWrap(true);VisualStyle::role(status_,"status");footerBox->addWidget(status_);
 auto* management=new QMenu(this);management->setObjectName("historyManagementMenu");auto* clear=management->addAction("Clear history records…");auto* cache=management->addAction("Clean retry artifacts and previews…");connect(managementToggle,&QToolButton::toggled,this,[management,managementToggle](bool on){if(on)management->popup(managementToggle->mapToGlobal(QPoint(0,managementToggle->height())));else management->hide();});connect(management,&QMenu::aboutToHide,this,[managementToggle]{managementToggle->setChecked(false);});layout->addWidget(footerBar);
 sharingTimer_=new QTimer(this);sharingTimer_->setInterval(500);connect(sharingTimer_,&QTimer::timeout,this,&HistoryWindow::pollSharing);deletionTimer_=new QTimer(this);deletionTimer_->setInterval(500);connect(deletionTimer_,&QTimer::timeout,this,&HistoryWindow::pollDeletion);

 connect(clear,&QAction::triggered,this,[this]{if(!Graphite::confirm(this,"Clear history records","Clear every local history record? Remote objects and user-owned local copies remain.","Clear history records",true))return;session_.request("history.clear",{},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g){VisualStyle::feedback(g->status_,e?e.message:"History cleared.",bool(e));g->refresh();}});});connect(cache,&QAction::triggered,this,[this]{if(!Graphite::confirm(this,"Clean cached files","Clean eligible retry artifacts and image previews? Some cleaned artifacts cannot retry. Persistent history, remote objects and local copies remain.","Clean artifacts",true))return;session_.request("cache.clear",{},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject result){if(g){g->releasePreview();
  // An incomplete upload that could not be aborted keeps using storage (#146).
  const auto kept=result["incompleteUploads"].toArray();const auto uploads=kept.size()==1?QStringLiteral("an incomplete upload"):QStringLiteral("%1 incomplete uploads").arg(kept.size());
  const auto notice=result["incompleteUploadNotice"].toObject()["message"].toString();
  const auto message=kept.isEmpty()?QStringLiteral("Artifact cleanup completed."):!notice.isEmpty()?notice:QStringLiteral("Artifact cleanup completed, but %1 could not be removed. %2").arg(uploads,kept[0].toObject()["error"].toObject()["message"].toString());
  VisualStyle::feedback(g->status_,e?e.message:message,bool(e)||!kept.isEmpty());g->refresh();}});});
 connect(copy_,&QPushButton::clicked,this,[this]{const auto record=selectedRecord();if(record.isEmpty())return;if(record["uploadStatus"]=="local_only"){operationAction("operation.copy");return;}session_.request("history.copyLink",{{"id",record["id"]}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g)VisualStyle::feedback(g->status_,e?e.message:"Stored link copied.",bool(e));});});connect(open_,&QPushButton::clicked,this,[this]{if(currentDetail_["linkAvailable"].toBool())openBrowser(link_->text());});connect(retry_,&QPushButton::clicked,this,[this]{const auto rows=table_->selectionModel()->selectedRows();if(rows.size()==1)operationAction(HistoryModel::retryUpload(model_->record(rows.first().row()))?"operation.retry":"operation.retryLocalSave");});connect(cancel_,&QPushButton::clicked,this,[this]{operationAction("operation.cancel");});connect(copyImage_,&QPushButton::clicked,this,[this]{exportLocal(true);});connect(saveAs_,&QPushButton::clicked,this,[this]{exportLocal(false);});
 connect(table_->selectionModel(),&QItemSelectionModel::selectionChanged,this,[this]{if(!restoringSelection_){selectedItemId_.clear();selectionChanged();}});connect(model_,&QAbstractItemModel::modelAboutToBeReset,this,[this]{if(!restoringSelection_)restoreSelection_=selectedIds();restoringSelection_=true;});
 auto* filtering=new QLabel("Filtering history…",list_);filtering->setObjectName("historyFiltering");filtering->setAccessibleName("Filtering history…");filtering->setAlignment(Qt::AlignCenter);VisualStyle::role(filtering,"meta");list_->addWidget(filtering);
 connect(model_,&HistoryModel::busyChanged,this,[this,filtering](bool busy){table_->setEnabled(!busy);inspectorScroll_->setEnabled(!busy);table_->setAccessibleDescription(busy?"Busy: filtering history":"Captures matching the applied filters");setProperty("busy",busy);if(busy){++selectionRequest_;currentDetail_={};selectedDeletion_={};selectionBar_->hide();inspectorScroll_->hide();list_->show();detailsOpen_=false;list_->setCurrentWidget(filtering);count_->clear();cancelled_->hide();showCancelled_->hide();if(status_->text()=="Select a capture to inspect its files and share link.")VisualStyle::feedback(status_,{});}else{count_->clear();list_->setCurrentWidget(model_->rowCount()?table_->parentWidget():emptyState_);}emit viewStateChanged();});
 connect(model_,&HistoryModel::loaded,this,[this]{retryLoading_->hide();for(int row=0;row<model_->rowCount();++row){const auto r=model_->record(row);bool selected=restoreSelection_.contains("operation:"+r["operationId"].toString());for(auto v:r["records"].toArray())selected|=restoreSelection_.contains(v.toObject()["id"].toString());if(selected){table_->selectionModel()->select(model_->index(row),QItemSelectionModel::Select|QItemSelectionModel::Rows);if(!table_->currentIndex().isValid())table_->selectionModel()->setCurrentIndex(model_->index(row),QItemSelectionModel::NoUpdate);}}restoreSelection_.clear();restoringSelection_=false;{const auto& q=model_->query();const bool filtered=!(q.search.isEmpty()&&q.category.isEmpty()&&q.mime.isEmpty()&&q.profileId.isEmpty()&&q.uploadStatus.isEmpty()&&q.localStatus.isEmpty()&&q.remoteStatus.isEmpty()&&q.from.isEmpty()&&q.to.isEmpty());const bool onlyCategory=filtered&&!q.category.isEmpty()&&q.search.isEmpty()&&activeFilters().isEmpty();const auto category=categories_->currentItem()?categories_->currentItem()->text():QString{};
   empty_->setText(onlyCategory?"Nothing in "+category:filtered?QString("No matching items"):QString("No captures yet"));const bool searched=!q.search.isEmpty(),restricted=!activeFilters().isEmpty();const QString where=q.category.isEmpty()?QString{}:" in "+category;
   emptyBody_->setText(onlyCategory?QString("Items of this kind appear here once you capture or upload them."):searched&&restricted?"Nothing"+where+" matches “"+q.search+"” with these filters.":searched?"Nothing"+where+" matches “"+q.search+"”.":filtered?"Nothing"+where+" matches these filters.":QString("Capture a region or upload your clipboard to get started. Captures and uploads appear here."));
   resetView_->setText(onlyCategory?"Show all items":searched&&!restricted&&q.category.isEmpty()?"Clear search":!searched&&restricted&&q.category.isEmpty()?"Clear filters":"Show all items");resetView_->setAccessibleName(resetView_->text());resetView_->setVisible(filtered);}list_->setCurrentWidget(model_->rowCount()?table_->parentWidget():emptyState_);updateFilterSummary();{const auto& q=model_->query();const bool earlier=q.offset>0,later=q.offset+q.limit<model_->total();if(auto* b=findChild<QPushButton*>("historyPrevious")){b->setVisible(earlier||later);b->setEnabled(earlier);}if(auto* b=findChild<QPushButton*>("historyNext")){b->setVisible(earlier||later);b->setEnabled(later);}}{const auto& q=model_->query();const bool filtered=!q.search.isEmpty()||!q.category.isEmpty()||!activeFilters().isEmpty();int files=0;for(int row=0;row<model_->rowCount();++row)files+=model_->record(row)["records"].toArray().size();count_->setText(!model_->rowCount()&&filtered?QString("No matching files"):(q.offset>0||q.limit<model_->total())&&files>0?QString("Files %1–%2 of %3%4").arg(q.offset+1).arg(q.offset+files).arg(model_->total()).arg(filtered?" matching":""):QString("%1 %2 · %3 %4").arg(model_->rowCount()).arg(model_->rowCount()==1?"capture":"captures").arg(model_->total()).arg(model_->total()==1?"stored file":"stored files"));}count_->setToolTip(count_->text());cancelled_->setText(model_->showCancelled()?QString("Cancelled captures shown"):QString("%1 cancelled %2 hidden").arg(model_->hiddenCancelled()).arg(model_->hiddenCancelled()==1?"capture":"captures"));cancelled_->setToolTip(cancelled_->text());showCancelled_->setVisible(model_->hiddenCancelled()>0||model_->showCancelled());cancelled_->setVisible(showCancelled_->isVisible());if(restoreDetailsOpen_){detailsOpen_=true;restoreDetailsOpen_=false;}selectionChanged();if(restoreScroll_){restoreScroll_=false;QTimer::singleShot(0,this,[this]{table_->doItemsLayout();table_->verticalScrollBar()->setValue(grid_->isChecked()?galleryScroll_:listScroll_);});}updateActivity();if(awaitingInitialization_){awaitingInitialization_=false;VisualStyle::feedback(status_,"Select a capture to inspect its files and share link.");}emit viewStateChanged();});connect(model_,&HistoryModel::error,this,[this](const QString& message){VisualStyle::feedback(status_,message,true);if(!model_->busy()&&!model_->rowCount()){VisualStyle::feedback(status_,{});cancelled_->hide();showCancelled_->hide();empty_->setText("History could not be loaded");emptyBody_->setText(message);retryLoading_->show();const auto& q=model_->query();resetView_->setVisible(!q.search.isEmpty()||!q.category.isEmpty()||!activeFilters().isEmpty());resetView_->setText("Clear search and filters");resetView_->setAccessibleName(resetView_->text());list_->setCurrentWidget(emptyState_);count_->setText("History unavailable");}restoringSelection_=false;restoreSelection_.clear();emit viewStateChanged();});
 connect(&session_,&UiSession::operationsChanged,this,&HistoryWindow::refresh);connect(&session_,&UiSession::initialized,this,&HistoryWindow::refresh);for(auto* e:findChildren<QLineEdit*>())if(!e->isReadOnly())connect(e,&QLineEdit::textChanged,this,&HistoryWindow::viewStateChanged);for(auto* t:{advancedToggle,managementToggle,receiptToggle})connect(t,&QToolButton::toggled,this,&HistoryWindow::viewStateChanged);connect(advancedToggle,&QToolButton::toggled,this,&HistoryWindow::updateFilterSummary);connect(categories_,&QListWidget::currentRowChanged,this,&HistoryWindow::viewStateChanged);
 for(auto* b:findChildren<QPushButton*>())if(b->accessibleName().isEmpty())b->setAccessibleName(b->text());
 // Pixmaps and property values follow a scheme switch (#60).
 connect(&Theme::Notifier::instance(),&Theme::Notifier::changed,this,[this]{updateSummary();columns_->update();});
 setGridMode(false);refresh();
}
void HistoryWindow::applyFilters(){searchTimer_->stop();auto q=model_->query();q.offset=0;if(auto* item=categories_->currentItem())q.category=item->data(Qt::UserRole).toString();{const QSignalBlocker block(failedOnly_);failedOnly_->setChecked(findChild<QLineEdit*>("historyFilteruploadStatus")->text()=="failed");}q.search=findChild<QLineEdit*>("historySearch")->text();q.from=findChild<QLineEdit*>("historyFilterfrom")->text();q.to=findChild<QLineEdit*>("historyFilterto")->text();q.mime=findChild<QLineEdit*>("historyFiltermime")->text();q.profileId=findChild<QLineEdit*>("historyFilterprofileId")->text();q.uploadStatus=findChild<QLineEdit*>("historyFilteruploadStatus")->text();q.localStatus=findChild<QLineEdit*>("historyFilterlocalStatus")->text();q.remoteStatus=findChild<QLineEdit*>("historyFilterremoteStatus")->text();updateHeading();model_->setQuery(q);updateFilterSummary();emit viewStateChanged();}
// The heading names what the list shows: the category, and failed uploads
// when only those are shown (the sidebar toggle or the Upload state filter).
void HistoryWindow::updateHeading(){if(!heading_)return;const auto* item=categories_->currentItem();const auto category=item?item->text():QString("All items");heading_->setText(!failedOnly_->isChecked()?category:categories_->currentRow()<=0?QString("Failed uploads"):category+" · failed uploads");}
void HistoryWindow::setGridMode(bool grid){grid_->setChecked(grid);findChild<QToolButton*>("historyListMode")->setChecked(!grid);table_->setViewMode(grid?QListView::IconMode:QListView::ListMode);table_->setMovement(QListView::Static);table_->setWrapping(grid);table_->setGridSize(QSize());table_->setUniformItemSizes(false);table_->doItemsLayout();updateInspectorLayout();emit viewStateChanged();}
void HistoryWindow::updateInspectorLayout(){if(!split_)return;
 // Side by side only while both panes get what they need at this text size:
 // the list a readable name beside its thumbnail and actions, the inspector
 // its preview and buttons. Below that the inspector opens over the list.
 const bool single=table_->selectionModel()->selectedRows().size()==1,gallery=grid_->isChecked();
 compact_=split_->width()<Graphite::scaled(420)+Graphite::scaled(280);if((!compact_&&!detailsFromGallery_)||(!single&&!restoringSelection_)||gallery)detailsOpen_=false;
 const bool details=compact_&&detailsOpen_,back=detailsOpen_&&(compact_||detailsFromGallery_);split_->setOrientation(Qt::Horizontal);
 inspectorScroll_->setMinimumWidth(compact_||details?0:Graphite::scaled(280));inspectorScroll_->setMaximumWidth(compact_||details?QWIDGETSIZE_MAX:Graphite::scaled(400));inspectorScroll_->setMaximumHeight(QWIDGETSIZE_MAX);
 // A compact preview keeps the title and primary action above the fold.
 preview_->setMinimumHeight(compact_?Graphite::scaled(48):180);preview_->setMaximumHeight(compact_?qBound(Graphite::scaled(48),split_->height()/5,Graphite::scaled(120)):260);
 if(auto* box=inspectorScroll_->widget()->layout()){box->setContentsMargins(18,compact_?10:16,18,compact_?12:16);box->setSpacing(compact_?8:10);}if(auto* escape=findChild<QAction*>("historyDetailsEscape"))escape->setEnabled(back);
 split_->setStretchFactor(0,1);split_->setStretchFactor(1,0);if(columns_)columns_->setVisible(!gallery);detailsBack_->setText(detailsFromGallery_?"Back to gallery":"Back to list");detailsBack_->setToolTip(detailsFromGallery_?"Return to the gallery (Esc)":"Return to the list (Esc)");detailsBack_->setVisible(back);if(heading_)heading_->setVisible(!back);const bool wasDetails=list_->isHidden();list_->setVisible(!details);
 inspectorScroll_->setVisible(single&&!gallery&&(!compact_||details));
 // Opened over the list, the inspector takes the whole width; the split's
 // inspector maximum bounds it again once both panes fit.
 if(details&&!wasDetails)split_->setSizes({0,qMax(1,split_->width())});else if(wasDetails&&!details)split_->setSizes({qMax(1,split_->width()-Graphite::scaled(320)),Graphite::scaled(320)});
 updateSelectionBar();table_->viewport()->update();}
void HistoryWindow::openSelectedDetails(){if(model_->busy()||detailsOpen_)return;const bool gallery=grid_->isChecked();if(!detailsOpen_){if(gallery)galleryScroll_=table_->verticalScrollBar()->value();else listScroll_=table_->verticalScrollBar()->value();}setGridMode(false);detailsFromGallery_=gallery;setDetailsOpen(true);}
void HistoryWindow::setDetailsOpen(bool open){
 const bool wasOpen=detailsOpen_,gallery=detailsFromGallery_;detailsOpen_=open&&(compact_||gallery)&&table_->selectionModel()->selectedRows().size()==1&&!grid_->isChecked();
 if(!open){detailsFromGallery_=false;if(gallery)setGridMode(true);}updateInspectorLayout();
 if(detailsOpen_){inspectorScroll_->verticalScrollBar()->setValue(0);detailsBack_->setFocus(Qt::OtherFocusReason);QTimer::singleShot(0,this,[this]{if(detailsOpen_)detailsBack_->setFocus(Qt::OtherFocusReason);});}else if(wasOpen){table_->doItemsLayout();table_->verticalScrollBar()->setValue(gallery?galleryScroll_:listScroll_);table_->setFocus(Qt::OtherFocusReason);QTimer::singleShot(0,this,[this,gallery]{table_->verticalScrollBar()->setValue(gallery?galleryScroll_:listScroll_);});}emit viewStateChanged();
}
void HistoryWindow::updateSelectionBar(){if(!selectionBar_)return;
 const int rows=table_->selectionModel()->selectedRows().size();const bool several=rows>1;
 selectionCount_->setText(QString("%1 selected").arg(rows));selectionCount_->setAccessibleName(selectionCount_->text());
 findChild<QAction*>("historyShowDetails")->setEnabled(rows==1);copyLinks_->setVisible(several&&selectionHasLinks());
 const auto deletable=several?deletableIds():QStringList{};const int files=several?int(selectedFiles().size()):0;deleteSelected_->setVisible(!deletable.isEmpty());
 deleteSelected_->setToolTip(deletable.size()==files?QString("Delete the %1 selected files from their original destinations. Local copies and history records remain.").arg(files):QString("Delete %1 of the %2 selected files from their original destinations; the others have no confirmed upload to delete.").arg(deletable.size()).arg(files));
 selectionBar_->setVisible(several);}
void HistoryWindow::resizeEvent(QResizeEvent* event){RetainedWindow::resizeEvent(event);updateInspectorLayout();}
void HistoryWindow::refresh(){if(!session_.ready()){awaitingInitialization_=true;VisualStyle::feedback(status_,"Waiting for the resident journal to initialize.");return;}model_->refresh();}
QStringList HistoryWindow::selectedIds()const{QStringList ids;for(auto index:table_->selectionModel()->selectedRows()){auto r=model_->record(index.row());const auto records=r["records"].toArray();if(records.isEmpty())ids.append("operation:"+r["operationId"].toString());else for(auto v:records)ids.append(v.toObject()["id"].toString());}return ids;}
QJsonObject HistoryWindow::selectedRecord()const{if(model_->busy())return {};const auto rows=table_->selectionModel()->selectedRows();if(rows.size()!=1)return {};const auto group=model_->record(rows.first().row());for(const auto& v:group["records"].toArray())if(v.toObject()["id"]==items_->currentData().toString())return v.toObject();return group["records"].toArray().isEmpty()?QJsonObject{}:group["records"].toArray().first().toObject();}
QList<QJsonObject> HistoryWindow::selectedFiles()const{if(model_->busy())return {};QList<QJsonObject> files;auto rows=table_->selectionModel()->selectedRows();std::sort(rows.begin(),rows.end(),[](const QModelIndex& a,const QModelIndex& b){return a.row()<b.row();});for(const auto& index:rows)for(const auto& v:model_->record(index.row())["records"].toArray())files.append(v.toObject());return files;}
QStringList HistoryWindow::deletableIds()const{QStringList ids;for(const auto& file:selectedFiles())if(HistoryModel::remoteDeletable(file))ids.append(file["id"].toString());return ids;}
bool HistoryWindow::selectionHasLinks()const{const auto files=selectedFiles();return !files.isEmpty()&&std::all_of(files.cbegin(),files.cend(),[](const QJsonObject& f){return f["linkAvailable"].toBool();});}
void HistoryWindow::copySelectedLinks(){
 QStringList ids;for(const auto& file:selectedFiles())ids.append(file["id"].toString());if(ids.isEmpty())return;
 session_.request("history.copyLinks",{{"ids",QJsonArray::fromStringList(ids)}},this,[g=QPointer<HistoryWindow>(this),count=ids.size()](Error e,QJsonObject){if(g)VisualStyle::feedback(g->status_,e?e.message:count==1?QString("Stored link copied."):QString("%1 stored links copied, one per line.").arg(count),bool(e));});
}
// The applied query's advanced restrictions, by their field labels.
QStringList HistoryWindow::activeFilters()const{
 const auto& q=model_->query();QStringList active;
 for(const auto& [value,label]:QList<std::pair<QString,QString>>{{q.from,"From date"},{q.to,"To date"},{q.mime,"MIME type"},{q.profileId,"Destination ID"},{q.uploadStatus,"Upload state"},{q.localStatus,"Local-copy state"},{q.remoteStatus,"Remote-deletion state"}})if(!value.isEmpty()){QString text=value;QString key;if(label=="Destination ID")key="profileId";else if(label=="Upload state")key="uploadStatus";else if(label=="Local-copy state")key="localStatus";else if(label=="Remote-deletion state")key="remoteStatus";if(!key.isEmpty()){const auto* combo=findChild<QComboBox*>("historyChoice"+key);const int index=combo->findData(value);if(index>=0)text=combo->itemText(index);}else if(label=="From date"||label=="To date"){const auto date=value.contains('T')?QDateTime::fromString(value,Qt::ISODateWithMs).toLocalTime().date():QDate::fromString(value,Qt::ISODate);if(date.isValid())text=QLocale().toString(date,QLocale::ShortFormat);}active.append((label=="Destination ID"?QString("Destination"):label)+": "+text);}
 return active;
}
void HistoryWindow::updateFilterSummary(){if(!filtersToggle_)return;
 const auto active=activeFilters();filtersToggle_->setText(active.isEmpty()?QString("Filters"):QString("Filters · %1").arg(active.size()));
 const auto state=filtersToggle_->isChecked()?QString("Expanded"):QString("Collapsed");
 filtersToggle_->setAccessibleDescription(active.isEmpty()?state+", no active filters":state+QString(", %1 active: ").arg(active.size())+active.join(", "));filtersToggle_->setToolTip(active.isEmpty()?QString("Show the advanced filters"):"Active filters:\n"+active.join("\n"));
 clearFilters_->setVisible(!active.isEmpty());clearFilters_->setAccessibleDescription(active.join(", "));
}
// Clear filters empties the advanced fields; the empty state's reset (all)
// also clears the search and shows every category.
void HistoryWindow::clearFilters(bool all){
 for(const auto& key:QStringList{"from","to","mime","profileId","uploadStatus","localStatus","remoteStatus"})findChild<QLineEdit*>("historyFilter"+key)->clear();
 if(all){findChild<QLineEdit*>("historySearch")->clear();restoringCategory_=true;categories_->setCurrentRow(0);restoringCategory_=false;}
 const bool focus=clearFilters_->hasFocus()||resetView_->hasFocus();applyFilters();if(focus)(all?static_cast<QWidget*>(table_):filtersToggle_)->setFocus(Qt::OtherFocusReason);
}
QString HistoryWindow::destinationLabel(const QJsonObject& r)const{
 if(r["uploadStatus"]=="local_only")return "Local copy";
 const auto original=r["receipt"].toObject()["destination"].toObject();const auto settings=original["settings"].toObject();const auto provider=original["providerId"].toString();QString label=provider=="proton-drive"?"Proton Drive":provider=="s3"?"S3 destination":provider=="backblaze-b2"?"Backblaze B2":"Original destination";
 for(auto v:session_.document()["profiles"].toArray()){const auto profile=v.toObject();if(profile["id"]==r["profileId"]){label=profile["label"].toString();break;}}
 if(provider=="proton-drive"&&!settings["folderName"].toString().isEmpty())label+=" / "+settings["folderName"].toString();else if((provider=="s3"||provider=="backblaze-b2")&&!settings["objectPrefix"].toString().isEmpty())label+=" / "+settings["objectPrefix"].toString();return label;
}
void HistoryWindow::selectionChanged(){emit viewStateChanged();const auto rows=table_->selectionModel()->selectedRows();
 // The instruction shows only while it applies; feedback stays until replaced.
 {static const QString instruction="Select a capture to inspect its files and share link.";if(!rows.isEmpty()||!model_->rowCount()){if(status_->text()==instruction)VisualStyle::feedback(status_,{});}else if(status_->text().isEmpty())VisualStyle::feedback(status_,instruction);}{QSignalBlocker block(items_);items_->clear();if(rows.size()==1){const auto group=model_->record(rows.first().row());for(auto v:group["records"].toArray()){auto r=v.toObject();items_->addItem(r["name"].toString(),r["id"].toString());}const auto itemIndex=items_->findData(selectedItemId_);if(itemIndex>=0)items_->setCurrentIndex(itemIndex);items_->setVisible(items_->count()>1);}}loadSelectedItem();updateInspectorLayout();}
void HistoryWindow::loadSelectedItem(){
 if(auto next=std::exchange(afterDetail_,{}))next(false);
 // History reloads the selection whenever the resident reports a change.
 // The item it already shows keeps its preview and size until the reload
 // brings a different preview, so the inspector never flashes empty.
 const auto next=selectedRecord()["id"].toString();const bool keepPreview=!next.isEmpty()&&next==shownItemId_;
 if(!keepPreview){if(previewCancelled_)previewCancelled_->store(true);shownItemId_.clear();shownPreviewPath_.clear();pixelDimensions_.clear();preview_->clear();}
 pendingPreviewPath_.clear();const auto seq=++selectionRequest_;currentDetail_={};details_->clear();static_cast<HistorySummary*>(summary_)->setFields({});capabilityHint_->hide();changeLocalCopies_->hide();title_->clear();outcome_->clear();link_->clear();link_->hide();previousUrl_->clear();previousLink_->hide();browserFeedback_->clear();browserFeedback_->hide();linkExpiry_->clear();linkExpiry_->hide();for(auto* b:{copy_,open_,copyImage_,saveAs_,editAgain_})b->setEnabled(false);for(auto* b:{share_,revoke_,inspectLink_,retry_,cancel_,saveDiagnostics_})b->hide();
 const auto rows=table_->selectionModel()->selectedRows();remove_->hide();deletionStatus_->hide();deletionRetry_->hide();deletionCancel_->hide();selectedDeletion_={};if(rows.size()!=1)return;const auto group=model_->record(rows.first().row());title_->setText(rows.first().data().toString());outcome_->setText(HistoryModel::statusText(group));VisualStyle::role(outcome_,model_->data(rows.first(),HistoryModel::FailedRole).toBool()?"danger":group["uploadStatus"]=="uploaded"?"success":"muted");retry_->setText(HistoryModel::retryUpload(group)?"Retry upload":"Retry local save");retry_->setAccessibleName(retry_->text());saveDiagnostics_->setVisible(diagnosticsAvailable(rows.first().row()));retry_->setVisible(HistoryModel::retryUpload(group)||HistoryModel::retryLocalSave(group));cancel_->setVisible(model_->data(rows.first(),HistoryModel::ActiveRole).toBool());
 const auto record=selectedRecord();if(record.isEmpty()){static_cast<HistorySummary*>(summary_)->setFields({{"Capture","File details appear after acquisition."}});return;}selectedItemId_=record["id"].toString();
 session_.request("history.detail",{{"id",record["id"]}},this,[g=QPointer<HistoryWindow>(this),seq](Error e,QJsonObject data){if(!g||g->selectionRequest_!=seq)return;if(e){VisualStyle::feedback(g->status_,e.message,true);if(auto next=std::exchange(g->afterDetail_,{}))next(false);return;}g->currentDetail_=data;const auto url=data["receipt"].toObject()["directUrl"].toString();const bool available=data["linkAvailable"].toBool();g->previousUrl_->setText(!available?url:QString{});g->previousUrl_->setCursorPosition(0);g->previousUrl_->setToolTip(g->previousUrl_->text());g->previousLink_->setVisible(!g->previousUrl_->text().isEmpty()&&g->findChild<QToolButton*>("historyReceiptExpanded")->isChecked());g->details_->setPlainText(QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Indented)));g->title_->setText(g->items_->count()>1?data["name"].toString():g->table_->currentIndex().data().toString());g->link_->setText(data["receipt"].toObject()["directUrl"].toString());g->link_->setCursorPosition(0);g->link_->setToolTip(g->link_->text());g->linkExpiry_->setText(linkExpiry(data));g->linkExpiry_->setVisible(!g->linkExpiry_->text().isEmpty());auto local=data["local"].toObject();const auto localPath=local["path"].toString();const bool localAvailable=local["status"]=="saved"&&QFileInfo(localPath).isFile();g->saveAs_->setEnabled(localAvailable);g->copyImage_->setEnabled(localAvailable&&data["mime"].toString().startsWith("image/")&&data["bytes"].toInteger()<=32*1024*1024);g->saveAs_->setToolTip(localAvailable?"Save the retained local original to another file":"A retained local original is needed to save another copy");g->copyImage_->setToolTip(g->copyImage_->isEnabled()?"Copy the retained original image":"A retained local image is needed to copy image bytes");g->editAgain_->setEnabled(editable(data));g->editAgain_->setToolTip(g->editAgain_->isEnabled()?"Open the retained local image in the editor and upload the result as a new item":data["mime"]=="image/png"||data["mime"]=="image/jpeg"?"Edit again needs the local copy. Turn on Keep a local copy in Settings → Capture for future captures.":"Only PNG and JPEG images can be edited");if(auto next=std::exchange(g->afterDetail_,{}))next(true);
 g->updateSummary();g->inspectSelectedDeletion();
 const bool sharing=data["capabilities"].toObject()["sharing"].toBool();for(auto* b:{g->share_,g->revoke_,g->inspectLink_})b->setVisible(sharing);const auto linkState=data["shareStatus"].toString();const bool stopped=g->sharingStopped_&&g->sharingId_==data["shareOperationId"].toString();const bool busy=g->sharingAccepting_||g->sharingTimer_->isActive()||(linkState=="pending"&&!stopped);g->share_->setEnabled(!busy&&linkState!="pending"&&linkState!="unknown"&&linkState!="changed");g->revoke_->setEnabled(!busy&&data["linkAvailable"].toBool());g->inspectLink_->setEnabled(!busy);const bool localResult=data["uploadStatus"]=="local_only"&&local["status"]=="saved";g->copy_->setText(localResult?"Copy result":"Copy link");g->copy_->setAccessibleName(g->copy_->text());g->copy_->setEnabled(data["linkAvailable"].toBool()||localResult);g->open_->setEnabled(data["linkAvailable"].toBool());
 // Actions come from capabilities and retained bytes: one that cannot apply
 // to this item is not shown, and a local-only result has no link field.
 for(auto* b:{g->copy_,g->open_,g->copyImage_,g->saveAs_,g->editAgain_})b->setVisible(b->isEnabled());g->capabilityHint_->setText("Editing needs a retained PNG or JPEG.");const bool missingImage=(data["mime"]=="image/png"||data["mime"]=="image/jpeg")&&!localAvailable;g->capabilityHint_->setVisible(missingImage);g->changeLocalCopies_->setVisible(missingImage);g->link_->setVisible(data["linkAvailable"].toBool());
 // Remote deletion needs a settled confirmed upload and a provider that can delete exactly that target.
 {const auto caps=data["capabilities"].toObject();g->remove_->setVisible(HistoryModel::remoteDeletable(data)&&caps["delete"].toBool()&&caps["stat"].toBool()&&(caps["conditionalDelete"].toBool()||caps["versionAddressing"].toBool())&&!caps["deletionEffect"].toString().isEmpty());}
 if(data["superseded"].toBool()||data["remoteStatus"]=="superseded")VisualStyle::feedback(g->status_,"Superseded: this stored link may serve replacement content. Deletion still targets the original receipt.",true);else if(!g->sharingFeedback_.isEmpty())VisualStyle::feedback(g->status_,g->sharingFeedback_,g->sharingFeedbackError_);const auto previewPath=data["previewPath"].toString();
  if(g->shownItemId_!=data["id"].toString()||g->shownPreviewPath_!=previewPath||!QFileInfo(previewPath).isFile()){g->shownItemId_.clear();g->shownPreviewPath_.clear();g->preview_->setText("Preview unavailable");g->pendingPreviewPath_=previewPath;g->startPreview();}
  // History reloads the same selection whenever the resident reports a change;
  // only a different item restarts the wait.
  if(g->previewMissing(data)&&(!g->previewRequestTimer_->isActive()||g->previewCandidate_!=data["id"].toString())){g->previewCandidate_=data["id"].toString();g->previewRequestTimer_->start();}});
}
void HistoryWindow::updateActivity(){activeId_.clear();for(int row=0;row<model_->rowCount();++row){auto i=model_->index(row);if(!i.data(HistoryModel::ActiveRole).toBool())continue;const auto r=model_->record(row);activeId_=r["operationId"].toString();activeLabel_->setText(r["name"].toString()+" · "+HistoryModel::statusText(r));const int percent=i.data(HistoryModel::ProgressRole).toInt();progress_->setRange(0,percent<0?0:100);progress_->setValue(qMax(0,percent));break;}activeBanner_->setVisible(!activeId_.isEmpty());}
void HistoryWindow::openBrowser(const QString& url){
 browserFeedback_->hide();const auto selection=selectionRequest_;host::openLink(QUrl(url),[g=QPointer<HistoryWindow>(this),selection](const QJsonObject& result){if(!g||g->selectionRequest_!=selection)return;if(!result["opened"].toBool()){g->browserFeedback_->setText("Couldn't open the browser. Copy the link instead");g->browserFeedback_->show();if(g->inspectorScroll_->isHidden())VisualStyle::feedback(g->status_,g->browserFeedback_->text(),true);}});
}
void HistoryWindow::operationAction(const QString& method){if(model_->busy())return;const auto rows=table_->selectionModel()->selectedRows();if(rows.size()!=1)return;const auto id=model_->record(rows.first().row())["operationId"].toString();if(id.isEmpty())return;session_.request(method,{{"id",id}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g){VisualStyle::feedback(g->status_,e?e.message:"Action accepted.",bool(e));g->refresh();}});}
void HistoryWindow::primaryAction(const QModelIndex& i){if(model_->busy())return;table_->selectionModel()->select(i,QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);table_->setCurrentIndex(i);const auto action=i.data(HistoryModel::PrimaryActionRole).toString();if(action=="Cancel")operationAction("operation.cancel");else if(action=="Retry upload")operationAction("operation.retry");else if(action=="Retry local save")operationAction("operation.retryLocalSave");else if(action=="Copy result")operationAction("operation.copy");else if(action=="Copy link"){const auto group=model_->record(i.row());if(group["records"].toArray().size()>1&&!group["operation"].toObject().isEmpty())operationAction("operation.copy");else {const auto files=group["records"].toArray();if(files.isEmpty())return;session_.request("history.copyLink",{{"id",files.first().toObject()["id"]}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g)VisualStyle::feedback(g->status_,e?e.message:"Stored link copied.",bool(e));});}}}
bool HistoryWindow::editable(const QJsonObject& record){
 const auto local=record["local"].toObject();
 // A batch row holds several files; its inspector picks one to edit.
 if(record["records"].toArray().size()>1)return false;
 return (record["mime"]=="image/png"||record["mime"]=="image/jpeg")&&local["status"]=="saved"&&QFileInfo(local["path"].toString()).isFile();
}
void HistoryWindow::editAgain(const QJsonObject& record){
 if(!editable(record)){VisualStyle::feedback(status_,"Edit again needs a retained local PNG or JPEG copy.",true);return;}
 emit editRequested("file",record["local"].toObject()["path"].toString());
}
// The row menu offers what this row can do now, from the same state and
// receipts as its inline action and the inspector; nothing it would refuse.
// Opening it on a row of a multi-selection keeps the selection and offers
// the batch actions.
void HistoryWindow::showRowMenu(const QModelIndex& i,const QPoint& point,bool overflow){
 if(model_->busy())return;
 auto* selection=table_->selectionModel();
 if(selection->selectedRows().size()>1&&selection->isSelected(i)){
  selection->setCurrentIndex(i,QItemSelectionModel::NoUpdate);QMenu menu(this);menu.setObjectName("historyRowMenu");menu.addSection(QString("%1 selected").arg(selection->selectedRows().size()));
  if(!overflow&&selectionHasLinks())menu.addAction("Copy links",this,&HistoryWindow::copySelectedLinks);
  if(const auto ids=deletableIds();!overflow&&!ids.isEmpty())menu.addAction(ids.size()==selectedFiles().size()?QString("Delete from destination…"):QString("Delete %1 from destination…").arg(ids.size()),this,[this]{const auto ids=deletableIds();prepareDelete(ids,int(selectedFiles().size())-int(ids.size()));});
  if(!overflow){menu.addSeparator();menu.addAction("Clear selection",this,[this]{table_->clearSelection();});}menu.addAction("Copy details",this,[this]{QJsonArray details;for(const auto& row:table_->selectionModel()->selectedRows())details.append(model_->record(row.row()));QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(details).toJson(QJsonDocument::Indented)));VisualStyle::feedback(status_,"Selected operation and receipt details copied.");});menu.exec(point);return;
 }
 selection->select(i,QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);table_->setCurrentIndex(i);const auto row=model_->record(i.row());const auto files=row["records"].toArray();
 QMenu menu(this);menu.setObjectName("historyRowMenu");
 const bool inspector=!inspectorScroll_->isHidden();const auto primary=i.data(HistoryModel::PrimaryActionRole).toString();auto remaining=[&](const QString& action){if(!overflow)return true;if(inspector){if(action=="Retry local save"&&HistoryModel::retryUpload(row))return true;if(files.size()>1&&(action=="Copy link"||action=="Delete from destination…"))return true;return false;}return action!=primary;};
 if(remaining("Cancel")&&i.data(HistoryModel::ActiveRole).toBool())menu.addAction("Cancel operation",this,[this]{operationAction("operation.cancel");});
 if(remaining("Retry upload")&&HistoryModel::retryUpload(row))menu.addAction("Retry upload",this,[this]{operationAction("operation.retry");});
 if(remaining("Retry local save")&&HistoryModel::retryLocalSave(row))menu.addAction("Retry local save",this,[this]{operationAction("operation.retryLocalSave");});
 // A batch's links are its result; one file's link is the stored link.
 if(remaining("Copy link")&&row["linkAvailable"].toBool())menu.addAction(files.size()>1?"Copy links":"Copy link",this,[this,row,files]{if(files.size()>1&&!row["operationId"].toString().isEmpty()){operationAction("operation.copy");return;}session_.request("history.copyLink",{{"id",files.first().toObject()["id"]}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g)VisualStyle::feedback(g->status_,e?e.message:"Stored link copied.",bool(e));});});
 if(remaining("Copy result")&&HistoryModel::copyLocalResult(row))menu.addAction("Copy result",this,[this]{operationAction("operation.copy");});
 if(remaining("Open in browser")&&files.size()==1&&row["linkAvailable"].toBool())menu.addAction("Open in browser",this,[this,url=files.first().toObject()["receipt"].toObject()["directUrl"].toString()]{openBrowser(url);});
 if(remaining("Edit again")&&editable(row))menu.addAction("Edit again",this,[this,row]{editAgain(row);});
 if(compact_||grid_->isChecked())menu.addAction(findChild<QAction*>("historyShowDetails"));
 menu.addSeparator();
 if(remaining("Save diagnostics for this upload")&&diagnosticsAvailable(i.row()))menu.addAction("Save diagnostics for this upload",this,[this,row]{saveDiagnostics(row["operationId"].toString());});
 menu.addAction("Copy details",this,[this,row]{QJsonObject detail=row;if(currentDetail_["operationId"]==row["operationId"])detail["selectedFile"]=currentDetail_;QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(detail).toJson(QJsonDocument::Indented)));VisualStyle::feedback(status_,"Operation and receipt details copied.");});
 QStringList deletable;for(const auto& v:files)if(HistoryModel::remoteDeletable(v.toObject()))deletable.append(v.toObject()["id"].toString());
 if(remaining("Delete from destination…")&&!deletable.isEmpty()){menu.addSeparator();menu.addAction("Delete from destination…",this,[this,deletable,count=int(files.size())]{prepareDelete(deletable,count-int(deletable.size()));});}
 menu.exec(point);
}
void HistoryWindow::updateSummary(){
 if(currentDetail_.isEmpty())return;
 const auto data=currentDetail_;
 const QHash<QString,QString> copies{{"saved","Saved locally"},{"disabled","No local copy requested"},{"failed","Local save failed"},{"pending","Waiting for local save"}};
 const QHash<QString,QString> sharing{{"shared","Shared link"},{"not_requested","No share link requested"},{"private","Private upload"},{"pending","Creating share link"},{"revoked","Share link revoked"},{"failed","Sharing failed"},{"unknown","Sharing outcome unknown"},{"changed","Share link changed"}};
 const QHash<QString,QString> remote{{"present","Remote file available"},{"not_requested","Remote deletion not requested"},{"removed","Deleted from destination"},{"already_missing","Remote file already missing"},{"unknown","Remote outcome unknown"},{"failed","Remote deletion failed"},{"pending","Waiting for remote deletion"},{"deleting","Deleting from destination"},{"changed","Remote file changed"},{"superseded","Replaced by a newer upload"},{"hidden_version_retained","Hidden; original version remains"}};
 const auto size=HistoryModel::fileSize(data["bytes"].toInteger())+(pixelDimensions_.isEmpty()?QString{}:" · "+pixelDimensions_);const auto local=data["local"].toObject();
 // Label / value rows: type, output, created, size, file, then the recorded outcomes.
 const auto mime=data["mime"].toString();const auto category=data["category"].toString();
 const QString type=category=="images"?mime.mid(6).toUpper()+" image":category=="videos"?mime.mid(6).toUpper()+" recording":category=="text"?QString("Text"):QString("File")+(mime.isEmpty()?QString{}:" ("+mime+")");
 auto created=QDateTime::fromString(data["created"].toString(),Qt::ISODateWithMs);if(!created.isValid())created=QDateTime::fromString(data["created"].toString(),Qt::ISODate);
 const auto when=created.isValid()?QLocale().toString(created.toLocalTime(),QLocale::ShortFormat):QString{};
 QList<std::pair<QString,QString>> rows;auto row=[&](const QString& label,const QString& value){if(!value.isEmpty())rows.append({label,value});};
 row("Type",type);row("Output",destinationLabel(data));row("Created",when);row("Size",size);row("File",data["name"].toString());
 // The recorded outcomes use the same grid; the type row already
 // names the format. The upload state is a row too unless the line under
 // the title already says it for this file: a batch's line describes the
 // whole capture, and a single file's line can lead with another outcome
 // ("Saved locally" after an interrupted upload).
 const QHash<QString,QString> uploads{{"uploaded","Uploaded"},{"local_only","Local capture"},{"pending","Waiting to upload"},{"uploading","Uploading"},{"failed","Upload failed"},{"unknown","Upload outcome unknown"},{"not_requested","No upload requested"}};
 {const auto upload=uploads.value(data["uploadStatus"].toString(),"Not recorded");if(items_->count()>1||!outcome_->text().contains(upload,Qt::CaseInsensitive))row("Upload",upload);}
 row("Local copy",copies.value(local["status"].toString(),"Not recorded"));row("Link",sharing.value(data["shareStatus"].toString(),"Not recorded"));row("Remote file",remote.value(data["remoteStatus"].toString(),"Not recorded"));
 if(category!="images"&&category!="videos"&&!mime.isEmpty()&&!type.contains(mime))row("Content type",mime);
 static_cast<HistorySummary*>(summary_)->setFields(rows);
}
void HistoryWindow::startPreview(){
 if(pendingPreviewPath_.isEmpty()||!isVisible()||previewLoading_)return;
 const auto path=std::exchange(pendingPreviewPath_,{});const auto original=currentDetail_["local"].toObject()["status"]=="saved"?currentDetail_["local"].toObject()["path"].toString():QString{};const auto seq=selectionRequest_;const auto itemId=currentDetail_["id"].toString();const bool isVideo=video(currentDetail_);previewLoading_=true;auto image=std::make_shared<QImage>();previewCancelled_=std::make_shared<std::atomic_bool>(false);auto cancelled=previewCancelled_;
 session_.background([path,original,image,cancelled](Error&){
  if(cancelled->load())return QJsonObject{};
  QFileInfo f(path);if(!f.isFile()||f.size()>8*1024*1024)return QJsonObject{};QImageReader reader(path);auto s=reader.size();if(!s.isValid()||qint64(s.width())*s.height()>16000000)return QJsonObject{};reader.setScaledSize(s.scaled(900,540,Qt::KeepAspectRatio));const auto length=reader.text(HistoryModel::durationKey);*image=reader.read();if(!length.isEmpty())image->setText(HistoryModel::durationKey,length);
  QJsonObject metadata;if(!original.isEmpty()&&QFileInfo(original).isFile()&&QFileInfo(original).size()<=32*1024*1024){QImageReader originalReader(original);const auto size=originalReader.size();if(size.isValid()){metadata["width"]=size.width();metadata["height"]=size.height();}}return metadata;
 },this,[g=QPointer<HistoryWindow>(this),seq,image,path,itemId,isVideo](Error,QJsonObject metadata){if(!g)return;g->previewLoading_=false;
  // A reload of the same item while the file was read still wants it.
  if(g->isVisible()&&(seq==g->selectionRequest_||(!itemId.isEmpty()&&itemId==g->selectedItemId_))){if(!image->isNull()){auto pixmap=QPixmap::fromImage(*image).scaled(g->preview_->contentsRect().size(),Qt::KeepAspectRatio,Qt::SmoothTransformation);const auto length=HistoryModel::duration(image->text(HistoryModel::durationKey).toLongLong());if(isVideo){QPainter painter(&pixmap);paintVideoBadge(painter,QRect(QPoint(0,0),pixmap.deviceIndependentSize().toSize()),length);if(!length.isEmpty()){g->pixelDimensions_=length;g->updateSummary();g->inspectSelectedDeletion();}}g->preview_->setPixmap(pixmap);g->shownItemId_=itemId;g->shownPreviewPath_=path;}if(metadata["width"].toInt()>0){g->pixelDimensions_=QString("%1×%2").arg(metadata["width"].toInt()).arg(metadata["height"].toInt());g->updateSummary();g->inspectSelectedDeletion();}}g->startPreview();});
}
bool HistoryWindow::previewMissing(const QJsonObject& detail)const{
 const auto category=detail["category"].toString(),id=detail["id"].toString(),path=detail["previewPath"].toString();
 return !id.isEmpty()&&(category=="images"||category=="videos")&&!previewRefused_.contains(id)&&(path.isEmpty()||!QFileInfo(path).isFile());
}
void HistoryWindow::requestPreview(){
 const auto id=currentDetail_["id"].toString();if(!isVisible()||!previewRequestId_.isEmpty()||!previewMissing(currentDetail_))return;
 previewRequestId_=id;preview_->setText("Making a preview…");
 session_.request("history.preview",{{"id",id}},this,[g=QPointer<HistoryWindow>(this),id](Error e,QJsonObject result){
  if(!g)return;
  g->previewRequestId_.clear();const bool current=g->currentDetail_["id"]==id;
  if(e){
   // Fail-soft: no status message. A refused item is not asked for again in
   // this window; a busy queue or a running upload may change.
   if(!QStringList{"preview_busy","operation_active","client_timeout","initializing"}.contains(e.code))g->previewRefused_.insert(id);
   if(current&&g->preview_->pixmap().isNull())g->preview_->setText("Preview unavailable");
  }else{
   const auto path=result["previewPath"].toString();g->model_->reloadThumbnail(path);
   if(current){g->pendingPreviewPath_=path;g->startPreview();}
  }
  // The selection moved on while this one was being made.
  if(!current&&g->previewMissing(g->currentDetail_))g->previewRequestTimer_->start();
 });
}
void HistoryWindow::releasePreview(){previewRequestTimer_->stop();if(previewCancelled_)previewCancelled_->store(true);pendingPreviewPath_.clear();shownItemId_.clear();shownPreviewPath_.clear();++selectionRequest_;preview_->clear();model_->releaseThumbnails();}
void HistoryWindow::hideEvent(QHideEvent* event){if(auto* menu=findChild<QMenu*>("historyManagementMenu"))menu->close();releasePreview();RetainedWindow::hideEvent(event);}
void HistoryWindow::showEvent(QShowEvent* event){RetainedWindow::showEvent(event);selectionChanged();table_->viewport()->update();}
void HistoryWindow::exportLocal(bool image){
 if(image){const auto record=selectedRecord();if(record.isEmpty())return;session_.request("history.copyImage",{{"id",record["id"]}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g)VisualStyle::feedback(g->status_,e?e.message:"Original image copied.",bool(e));});return;}

 const auto local=currentDetail_["local"].toObject();const auto path=local["path"].toString();if(local["status"]!="saved"||!QFileInfo(path).isFile())return;
 chooser_->open({FileDialog::Mode::Save,"Save original file as",{},currentDetail_["name"].toString(),{}},[this,path](const FileDialog::Result& result){
  if(result.failed()){VisualStyle::feedback(status_,result.error,true);return;}
  if(!result.chosen())return;const auto destination=result.path;if(QFileInfo(destination).absoluteFilePath()==QFileInfo(path).absoluteFilePath()){VisualStyle::feedback(status_,"This is already the retained local file.");return;}
  session_.background([path,destination](Error& e){QFile source(path);QSaveFile target(destination);if(!source.open(QIODevice::ReadOnly)||!target.open(QIODevice::WriteOnly)){e={"file_copy","Could not open the original or selected destination file.",{}};return QJsonObject{};}while(!source.atEnd()){const auto bytes=source.read(1024*1024);if(bytes.isEmpty()&&source.error()!=QFileDevice::NoError){e={"file_copy","Could not read the retained original file.",{}};return QJsonObject{};}if(target.write(bytes)!=bytes.size()){e={"file_copy","Could not write the selected destination file.",{}};return QJsonObject{};}}if(!target.commit())e={"file_copy","Could not finish saving the original file.",{}};return QJsonObject{};},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g)VisualStyle::feedback(g->status_,e?e.message:"Original file saved.",bool(e));});
 });
}
void HistoryWindow::saveAsDialogCommand(const QString& action,const QString& id,std::function<void(Error,QJsonObject)> reply){
 if(action=="inspect"){reply({},chooser_->state());return;}
 if(action=="cancel"){chooser_->cancel();reply({},chooser_->state());return;}
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
 if(action=="revoke"&&!Graphite::confirm(this,"Revoke share link",QString("Revoke the share link for ‘%1’ in %2?\nThe uploaded file remains. Existing member access and downloaded copies remain.").arg(file["name"].toString(),destinationLabel(file)),"Revoke link",true))return;
 sharingAccepting_=true;emit viewStateChanged();for(auto* button:{share_,revoke_,inspectLink_})button->setEnabled(false);
 const auto method=action=="create"?"history.share":action=="revoke"?"history.revokeLink":"history.inspectLink";
 session_.request(method,{{"id",historyId}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject data){
  if(!g)return;
  g->sharingAccepting_=false;emit g->viewStateChanged();
  if(e){VisualStyle::feedback(g->status_,e.code+": "+e.message,true);g->selectionChanged();return;}
  g->sharingId_=data["shareOperationId"].toString();g->sharingStopped_=false;g->sharingFeedback_.clear();emit g->viewStateChanged();g->sharingTimer_->start();g->pollSharing();
 });
}
void HistoryWindow::pollSharing(){
 if(sharingPollActive_)return;
 if(sharingId_.isEmpty()){sharingTimer_->stop();return;}sharingPollActive_=true;const auto id=sharingId_;
 session_.request("share.inspect",{{"id",id}},this,[g=QPointer<HistoryWindow>(this),id](Error e,QJsonObject data){
  if(!g)return;
  g->sharingPollActive_=false;if(id!=g->sharingId_)return;
  if(e){g->sharingFeedback_=e.code+": "+e.message;g->sharingFeedbackError_=true;VisualStyle::feedback(g->status_,g->sharingFeedback_,true);g->sharingTimer_->stop();g->refresh();return;}
  const auto state=data["state"].toString();g->sharingStopped_=state=="interrupted";if(state=="running"||state=="accepted")return;
  g->sharingTimer_->stop();const auto error=data["error"].toObject();g->sharingFeedback_="Link: "+data["status"].toString()+(error["message"].toString().isEmpty()?QString{}:" — "+error["message"].toString());g->sharingFeedbackError_=!error["code"].toString().isEmpty();VisualStyle::feedback(g->status_,g->sharingFeedback_,g->sharingFeedbackError_);g->refresh();
 });
}
void HistoryWindow::prepareDelete(){const auto file=selectedRecord();if(!file.isEmpty())prepareDelete({file["id"].toString()},0);}
// Exact IDs go to delete.prepare; the confirmation shows its targets and
// confirms with that preparation's own acknowledgement.
void HistoryWindow::prepareDelete(const QStringList& ids,int skipped){
 if(ids.isEmpty())return;QHash<QString,QString> names;for(int row=0;row<model_->rowCount();++row)for(const auto& v:model_->record(row)["records"].toArray())if(ids.contains(v.toObject()["id"].toString()))names[v.toObject()["id"].toString()]=v.toObject()["name"].toString();
 session_.request("delete.prepare",{{"ids",QJsonArray::fromStringList(ids)}},this,[g=QPointer<HistoryWindow>(this),ids,names,skipped](Error e,QJsonObject preview){
  if(!g)return;if(e){const auto name=names.value(e.details["historyId"].toString());VisualStyle::feedback(g->status_,e.code+": "+e.message+(name.isEmpty()?QString{}:" ("+name+")"),true);return;}
  QPointer<QDialog> dialog=deletionConfirmation(g,preview,names,g->session_.document()["profiles"].toArray(),skipped);const bool accepted=dialog->exec()==QDialog::Accepted;if(!g)return;delete dialog.data();if(!accepted)return;
  g->deleteAccepting_=true;emit g->viewStateChanged();g->session_.request("delete.confirm",{{"ids",QJsonArray::fromStringList(ids)},{"acknowledgement",preview["acknowledgement"]}},g,[g](Error failure,QJsonObject data){if(!g)return;g->deleteAccepting_=false;emit g->viewStateChanged();VisualStyle::feedback(g->status_,failure?failure.code+": "+failure.message:"Remote deletion accepted; individual outcomes will update.",bool(failure));if(!failure){g->deletionId_=data["deleteOperationId"].toString();emit g->viewStateChanged();g->deletionTimer_->start();}});
 });
}
namespace {
// Where a receipt's original target lives: the destination's name now, its
// service and the bucket or folder recorded at upload.
QString originalDestination(const QJsonObject& receipt,const QJsonArray& profiles){
 const auto d=receipt["destination"].toObject();const auto provider=d["providerId"].toString();const auto settings=d["settings"].toObject();
 const QString service=provider=="s3"?"S3":provider=="backblaze-b2"?"Backblaze B2":provider=="proton-drive"?"Proton Drive":provider;QString label;
 for(const auto& v:profiles)if(v.toObject()["id"]==d["profileId"]&&!d["profileId"].toString().isEmpty())label=v.toObject()["label"].toString();
 QStringList parts{label.isEmpty()||label==service?service:label+" ("+service+")"};
 if(!settings["bucket"].toString().isEmpty())parts<<"bucket "+settings["bucket"].toString();if(!settings["folderName"].toString().isEmpty())parts<<"folder "+settings["folderName"].toString();
 return parts.join(" · ");
}
}
QDialog* HistoryWindow::deletionConfirmation(QWidget* parent,const QJsonObject& preparation,const QHash<QString,QString>& names,const QJsonArray& profiles,int skipped){
 auto* dialog=new QDialog(parent);dialog->setObjectName("confirmAction");dialog->setWindowTitle("Delete from destination");dialog->setModal(true);
 auto* box=new QVBoxLayout(dialog);box->setContentsMargins(20,18,20,16);box->setSpacing(12);const auto targets=preparation["targets"].toArray();
 const auto first=names.value(targets.first().toObject()["historyId"].toString(),"this file");
 auto* heading=new QLabel(targets.size()==1?QString("Delete “%1” from its destination?").arg(first):QString("Delete %1 files from their destinations?").arg(targets.size()),dialog);heading->setObjectName("deleteConfirmationHeading");heading->setTextFormat(Qt::PlainText);heading->setWordWrap(true);VisualStyle::role(heading,"sectionTitle");box->addWidget(heading);
 QString consequence=targets.size()==1?"The original remote target below is deleted.":"Each original remote target below is deleted.";consequence+=" Local copies and history records remain.";if(preparation.contains("cacheNotice"))consequence+=" "+preparation["cacheNotice"].toString()+".";
 if(skipped>0)consequence+=QString("\n%1 other selected %2 no confirmed upload to delete and %3 left out.").arg(skipped).arg(skipped==1?"file has":"files have").arg(skipped==1?"is":"are");
 auto* text=new QLabel(consequence,dialog);text->setObjectName("deleteConfirmationText");text->setTextFormat(Qt::PlainText);text->setWordWrap(true);box->addWidget(text);
 // Every exact target, scrollable so a large selection keeps the dialog bounded.
 auto* list=new QWidget;auto* rows=new QVBoxLayout(list);rows->setContentsMargins(12,10,12,10);rows->setSpacing(10);
 for(int index=0;index<targets.size();++index){
  const auto target=targets[index].toObject();const auto receipt=target["receipt"].toObject();const auto reference=receipt["objectReference"].toObject();
  if(index)rows->addWidget(Graphite::rule(list));auto* grid=new QGridLayout;grid->setHorizontalSpacing(12);grid->setVerticalSpacing(4);grid->setColumnStretch(1,1);
  auto* name=new QLabel(names.value(target["historyId"].toString(),target["historyId"].toString()),list);name->setTextFormat(Qt::PlainText);name->setWordWrap(true);VisualStyle::role(name,"strong");grid->addWidget(name,0,0,1,2);
  QList<std::pair<QString,QString>> fields{{"Destination",originalDestination(receipt,profiles)}};
  if(!reference["key"].toString().isEmpty())fields.append({"Key",reference["key"].toString()});
  for(const auto& [key,label]:QList<std::pair<QString,QString>>{{"versionId","Version"},{"fileId","File version"},{"revisionUid","Revision"},{"nodeUid","Node"}})if(!reference[key].toString().isEmpty())fields.append({label,reference[key].toString()});
  if(reference["key"].toString().isEmpty()&&fields.size()==1)fields.append({"Target",QString::fromUtf8(QJsonDocument(reference).toJson(QJsonDocument::Compact))});
  fields.append({"Effect",target["effect"].toString()});
  int line=1;for(const auto& [label,value]:fields){auto* l=new QLabel(label,list);VisualStyle::role(l,"meta");auto* v=new QLabel(value,list);v->setTextFormat(Qt::PlainText);v->setWordWrap(true);v->setTextInteractionFlags(Qt::TextSelectableByMouse);if(label!="Destination"&&label!="Effect")VisualStyle::role(v,"mono");grid->addWidget(l,line,0,Qt::AlignTop);grid->addWidget(v,line,1);++line;}
  rows->addLayout(grid);
 }
 rows->addStretch();auto* scroll=new QScrollArea(dialog);scroll->setObjectName("deleteConfirmationTargets");scroll->setAccessibleName("Remote targets to delete");scroll->setWidget(list);scroll->setWidgetResizable(true);scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);scroll->setFrameShape(QFrame::NoFrame);Graphite::transparentView(scroll);
 scroll->setMinimumHeight(0);box->addWidget(Graphite::rule(dialog));box->addWidget(scroll,1);box->addWidget(Graphite::rule(dialog));
 auto* toggle=new QToolButton(list);toggle->setObjectName("deleteConfirmationDetailsExpanded");toggle->setText("Technical details");Graphite::disclosure(toggle);toggle->setAccessibleName("Show the full deletion preparation");rows->insertWidget(rows->count()-1,toggle,0,Qt::AlignLeft);
 auto* details=new QPlainTextEdit(QString::fromUtf8(QJsonDocument(preparation).toJson(QJsonDocument::Indented)),list);details->setObjectName("deleteConfirmationDetails");details->setReadOnly(true);VisualStyle::role(details,"diagnostics");details->setAccessibleName("Deletion preparation receipt");details->setMinimumHeight(Graphite::scaled(140));details->setMaximumHeight(Graphite::scaled(200));details->hide();rows->insertWidget(rows->count()-1,details);
 QObject::connect(toggle,&QToolButton::toggled,details,&QWidget::setVisible);
 auto* buttons=new QHBoxLayout;buttons->setSpacing(8);buttons->addStretch();auto* cancel=new QPushButton("Cancel",dialog);cancel->setObjectName("confirmCancel");cancel->setAutoDefault(true);cancel->setDefault(true);auto* action=new QPushButton("Delete from destination",dialog);action->setObjectName("confirmActionButton");action->setAccessibleName(action->text());action->setAutoDefault(false);VisualStyle::role(action,"danger");buttons->addWidget(cancel);buttons->addWidget(action);box->addLayout(buttons);
 QObject::connect(cancel,&QPushButton::clicked,dialog,&QDialog::reject);QObject::connect(action,&QPushButton::clicked,dialog,&QDialog::accept);
 const auto available=(parent?parent->screen():QGuiApplication::primaryScreen())->availableGeometry();const int height=qMax(1,qMin(available.height(),parent?parent->window()->height():available.height())-40),width=qMin(Graphite::scaled(560),available.width()-40);box->setSizeConstraint(QLayout::SetNoConstraint);dialog->setMaximumSize(qMin(Graphite::scaled(720),available.width()-40),height);dialog->resize(width,qMin(height,Graphite::scaled(520)));cancel->setFocus(Qt::OtherFocusReason);
 return dialog;
}
void HistoryWindow::inspectSelectedDeletion(){
 const auto id=currentDetail_["deleteOperationId"].toString(),file=currentDetail_["id"].toString();if(id.isEmpty()||model_->busy())return;
 session_.request("delete.inspect",{{"id",id}},this,[g=QPointer<HistoryWindow>(this),id,file](Error e,QJsonObject data){if(!g||e||g->model_->busy()||g->currentDetail_["id"]!=file||g->currentDetail_["deleteOperationId"]!=id)return;
  QJsonObject item;for(const auto& v:data["items"].toArray())if(v.toObject()["historyId"]==file)item=v.toObject();if(item.isEmpty())return;
  const bool running=data["state"]=="running"||data["state"]=="accepted";const auto state=item["status"].toString();const bool activeTarget=running&&QStringList{"pending","deleting"}.contains(state);const bool retryable=!running&&QStringList{"pending","deleting","failed","unknown","hidden_version_retained","version_retained","unsupported_safe_removal"}.contains(state);
  g->selectedDeletion_=data;g->deletionRetry_->setVisible(retryable);g->deletionCancel_->setVisible(activeTarget);g->deletionStatus_->setVisible(activeTarget||retryable);
  const auto name=g->currentDetail_["name"].toString();g->deletionStatus_->setText((running?"Deleting “":"Deletion needs attention for “")+name+"”."+(data["items"].toArray().size()>1?QString(" This deletion includes %1 files; retry or cancel applies to the whole deletion.").arg(data["items"].toArray().size()):QString{})+(item["error"].toObject()["message"].toString().isEmpty()?QString{}:"\n"+item["error"].toObject()["message"].toString()));
  g->deletionRetry_->setAccessibleName("Retry deletion of "+name);g->deletionCancel_->setAccessibleName("Cancel deletion of "+name);
  if(running){g->deletionId_=id;g->deletionTimer_->start();}emit g->viewStateChanged();
 });
}
void HistoryWindow::deletionAction(const QString& method){
 if(model_->busy()||selectedDeletion_.isEmpty()||selectedDeletion_["id"]!=currentDetail_["deleteOperationId"])return;
 const auto id=selectedDeletion_["id"].toString();deletionRetry_->setEnabled(false);deletionCancel_->setEnabled(false);
 session_.request(method,{{"id",id}},this,[g=QPointer<HistoryWindow>(this),id](Error e,QJsonObject){if(!g)return;g->deletionRetry_->setEnabled(true);g->deletionCancel_->setEnabled(true);VisualStyle::feedback(g->status_,e?e.message:"Deletion action accepted; already sent deletion cannot be rolled back.",bool(e));if(!e){g->deletionId_=id;g->deletionTimer_->start();}g->refresh();});
}
void HistoryWindow::pollDeletion(){if(deletionPollActive_)return;if(deletionId_.isEmpty()){deletionTimer_->stop();return;}deletionPollActive_=true;const auto id=deletionId_;session_.request("delete.inspect",{{"id",id}},this,[g=QPointer<HistoryWindow>(this),id](Error e,QJsonObject data){if(!g)return;g->deletionPollActive_=false;if(g->deletionId_!=id)return;if(e){VisualStyle::feedback(g->status_,e.message,true);g->deletionTimer_->stop();return;}QString status="Remote deletion: "+data["state"].toString();for(auto v:data["items"].toArray()){auto item=v.toObject();status+="\n"+item["status"].toString()+" "+item["error"].toObject()["message"].toString();}g->status_->setText(status);if(data["state"]!="running"&&data["state"]!="accepted"){g->deletionTimer_->stop();g->refresh();}});}
QJsonObject HistoryWindow::saveViewState()const{
 if(deleteAccepting_||sharingAccepting_)return {{"stateError","request_awaiting_acceptance"}};
 const auto& q=model_->query();QJsonObject query{{"limit",q.limit},{"offset",q.offset},{"search",q.search},{"category",q.category},{"mime",q.mime},{"profileId",q.profileId},{"uploadStatus",q.uploadStatus},{"localStatus",q.localStatus},{"remoteStatus",q.remoteStatus},{"from",q.from},{"to",q.to}};QJsonObject filters{{"search",findChild<QLineEdit*>("historySearch")->text()}};for(const auto& key:QStringList{"from","to","mime","profileId","uploadStatus","localStatus","remoteStatus"})filters[key]=findChild<QLineEdit*>("historyFilter"+key)->text();
 return {{"query",query},{"filters",filters},{"selectedIds",QJsonArray::fromStringList(restoringSelection_?restoreSelection_:selectedIds())},{"selectedItemId",selectedItemId_},{"deletionId",deletionId_},{"sharingId",sharingId_},{"filtersExpanded",findChild<QToolButton*>("historyFiltersExpanded")->isChecked()},{"managementExpanded",findChild<QToolButton*>("historyManagementExpanded")->isChecked()},{"receiptExpanded",findChild<QToolButton*>("historyReceiptExpanded")->isChecked()},{"gridMode",grid_->isChecked()},{"detailsOpen",detailsOpen_||restoreDetailsOpen_},{"detailsFromGallery",detailsFromGallery_},{"galleryScroll",grid_->isChecked()&&!restoreScroll_?table_->verticalScrollBar()->value():galleryScroll_},{"listScroll",!grid_->isChecked()&&!detailsOpen_&&!restoreScroll_?table_->verticalScrollBar()->value():listScroll_},{"showCancelled",showCancelled_->isChecked()},{"geometry",QJsonArray{x(),y(),width(),height()}}};
}
bool HistoryWindow::restoreViewState(const QJsonObject& state){
 if(validateUiState({{"schemaVersion",1},{"history",state}}))return false;
 const QSignalBlocker stateSignals(this);searchTimer_->stop();const auto box=state["geometry"].toArray();setGeometry(box[0].toInt(),box[1].toInt(),box[2].toInt(),box[3].toInt());const auto f=state["filters"].toObject();findChild<QLineEdit*>("historySearch")->setText(f["search"].toString());for(const auto& key:QStringList{"from","to","mime","profileId","uploadStatus","localStatus","remoteStatus"})findChild<QLineEdit*>("historyFilter"+key)->setText(f[key].toString());findChild<QToolButton*>("historyFiltersExpanded")->setChecked(state["filtersExpanded"].toBool());findChild<QToolButton*>("historyManagementExpanded")->setChecked(false);findChild<QToolButton*>("historyReceiptExpanded")->setChecked(state["receiptExpanded"].toBool());setGridMode(state["gridMode"].toBool());detailsFromGallery_=state["detailsFromGallery"].toBool();restoreDetailsOpen_=state["detailsOpen"].toBool();galleryScroll_=state["galleryScroll"].toInt();listScroll_=state["listScroll"].toInt();restoreScroll_=true;showCancelled_->setChecked(state["showCancelled"].toBool());restoreSelection_.clear();for(auto id:state["selectedIds"].toArray())restoreSelection_.append(id.toString());selectedItemId_=state["selectedItemId"].toString();restoringSelection_=true;deletionId_=state["deletionId"].toString();if(!deletionId_.isEmpty()){deletionTimer_->start();pollDeletion();}sharingId_=state["sharingId"].toString();sharingStopped_=false;sharingFeedback_.clear();sharingTimer_->stop();if(!sharingId_.isEmpty()){sharingTimer_->start();pollSharing();}
 const auto data=state["query"].toObject();UiHistoryQuery q;q.limit=data["limit"].toInt(50);q.offset=data["offset"].toInt();q.search=data["search"].toString();q.category=data["category"].toString();q.mime=data["mime"].toString();q.profileId=data["profileId"].toString();q.uploadStatus=data["uploadStatus"].toString();q.localStatus=data["localStatus"].toString();q.remoteStatus=data["remoteStatus"].toString();q.from=data["from"].toString();q.to=data["to"].toString();{restoringCategory_=true;int row=0;for(int i=0;i<categories_->count();++i)if(categories_->item(i)->data(Qt::UserRole).toString()==q.category)row=i;categories_->setCurrentRow(row);restoringCategory_=false;const QSignalBlocker block(failedOnly_);failedOnly_->setChecked(f["uploadStatus"].toString()=="failed");}if(data["search"]!=f["search"]||data["mime"]!=f["mime"]||data["profileId"]!=f["profileId"]||data["uploadStatus"]!=f["uploadStatus"]||data["localStatus"]!=f["localStatus"]||data["remoteStatus"]!=f["remoteStatus"]||data["from"]!=f["from"]||data["to"]!=f["to"])q.offset=0;q.search=f["search"].toString();q.mime=f["mime"].toString();q.profileId=f["profileId"].toString();q.uploadStatus=f["uploadStatus"].toString();q.localStatus=f["localStatus"].toString();q.remoteStatus=f["remoteStatus"].toString();q.from=f["from"].toString();q.to=f["to"].toString();searchTimer_->stop();updateHeading();model_->setQuery(q);return true;
}
QJsonObject HistoryWindow::historyState()const{
 const auto& q=model_->query();const int rows=table_->selectionModel()->selectedRows().size();QJsonArray actions;
 if(rows>1){if(selectionHasLinks())actions.append("copyLinks");if(!deletableIds().isEmpty())actions.append("delete");actions.append("clearSelection");}
 return {{"query",QJsonObject{{"limit",q.limit},{"offset",q.offset},{"search",q.search},{"category",q.category},{"mime",q.mime},{"profileId",q.profileId},{"uploadStatus",q.uploadStatus},{"localStatus",q.localStatus},{"remoteStatus",q.remoteStatus},{"from",q.from},{"to",q.to}}},
  {"activeFilters",QJsonArray::fromStringList(activeFilters())},{"filtersLabel",filtersToggle_->text()},{"selectedIds",QJsonArray::fromStringList(selectedIds())},{"selectedRows",rows},{"selectionActions",actions},{"deletableIds",QJsonArray::fromStringList(deletableIds())},
  {"busy",model_->busy()},{"deletion",selectedDeletion_.isEmpty()?QJsonObject{}:QJsonObject{{"id",selectedDeletion_["id"]},{"historyId",currentDetail_["id"]},{"name",currentDetail_["name"]},{"canRetry",!deletionRetry_->isHidden()},{"canCancel",!deletionCancel_->isHidden()}}},{"layout",compact_?"compact":"split"},{"detailsOpen",detailsOpen_},{"detailsOrigin",detailsFromGallery_?"gallery":"list"},{"galleryScroll",galleryScroll_},{"inspectorVisible",!inspectorScroll_->isHidden()},{"gridMode",grid_->isChecked()},{"rows",model_->rowCount()},{"total",double(model_->total())}};
}
void HistoryWindow::command(const QJsonObject& params,std::function<void(Error,QJsonObject)> reply){
 const auto action=params["action"].toString();if(model_->busy()&&action!="inspect"&&action!="clearFilters"){reply({"history_busy","Wait for History to finish filtering.",{}},historyState());return;}
 if(action=="clearFilters")clearFilters(params["scope"].toString()=="all");
 else if(action=="select"){
  // By history ID, operation ID or a row's own ID, on the page shown.
  QSet<QString> wanted;for(const auto& v:params["ids"].toArray())wanted.insert(v.toString());QItemSelection chosen;QModelIndex first;
  for(int row=0;row<model_->rowCount();++row){const auto r=model_->record(row);bool match=wanted.contains(r["operationId"].toString())||wanted.contains(r["id"].toString());for(const auto& v:r["records"].toArray())match|=wanted.contains(v.toObject()["id"].toString());if(match){chosen.select(model_->index(row),model_->index(row));if(!first.isValid())first=model_->index(row);}}
  if(!first.isValid()){reply({"history_row_not_found","None of those items is on the History page shown; search or page to them first.",{}},historyState());return;}
  table_->selectionModel()->select(chosen,QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);table_->selectionModel()->setCurrentIndex(first,QItemSelectionModel::NoUpdate);table_->scrollTo(first);
 }
 else if(action=="clearSelection")table_->clearSelection();
 else if(action=="openDetails"){if(table_->selectionModel()->selectedRows().size()!=1){reply({"history_selection_invalid","Select exactly one row before opening its details.",{}},historyState());return;}openSelectedDetails();}
 else if(action=="closeDetails")setDetailsOpen(false);
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
 saveDiagnostics_->setEnabled(false);VisualStyle::feedback(status_,"Saving diagnostics for this upload…");
 session_.request("diagnostics.bundle",params,this,[g=QPointer<HistoryWindow>(this),done](Error e,QJsonObject result){
  if(g){g->saveDiagnostics_->setEnabled(true);VisualStyle::feedback(g->status_,e?e.message:"Diagnostics saved to "+QDir::toNativeSeparators(result["path"].toString())+". Nothing was uploaded.",bool(e));}
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
