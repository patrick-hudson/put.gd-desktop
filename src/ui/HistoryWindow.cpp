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
// The list's columns (Graphite): Item takes what Output and Created leave; a
// narrow list drops Output first, then Created, and the inspector keeps both.
struct Columns {QRect item,output,created;};
Columns historyColumns(const QRect& row){
 Columns c;const int width=row.width();const int created=width>=440?92:0;const int output=width>=600?132:0;
 c.created=created?QRect(row.right()-created+1,row.top(),created,row.height()):QRect();
 c.output=output?QRect(row.right()-created-output+1,row.top(),output,row.height()):QRect();
 c.item=QRect(row.left(),row.top(),width-created-output,row.height());return c;
}
int dayHeadingHeight(){return Graphite::scaled(32);}
// The row's body: right of the thumbnail in a list, under it in a gallery tile.
QRect rowBody(const QRect& outer,bool gallery,int heading){
 const auto card=outer.adjusted(6,heading+2,-6,-2);
 if(gallery)return QRect(card.left()+14,card.top()+156,card.width()-28,card.height()-156);
 const int left=card.left()+12+Graphite::Metric::thumbWidth+16;
 return QRect(left,card.top(),card.right()-12-left+1,Graphite::scaled(Graphite::Metric::historyRow)-4);
}
// Item / Output / Created above the list, on the same columns as its rows.
class HistoryColumns final:public QWidget {
public:
 explicit HistoryColumns(QListView* view,QWidget* parent):QWidget(parent),view_(view){setObjectName("historyColumns");setFixedHeight(Graphite::scaled(30));}
protected:
 void paintEvent(QPaintEvent*)override{
  QPainter p(this);const auto& t=Theme::current();const int left=view_->viewport()->mapTo(window(),QPoint(0,0)).x()-mapTo(window(),QPoint(0,0)).x();
  // The same body rectangle the delegate lays its columns out in.
  const int card=left+6,thumb=card+12,body=thumb+Graphite::Metric::thumbWidth+16,right=left+view_->viewport()->width()-6-12;
  const auto c=historyColumns(QRect(body,0,right-body+1,height()));const auto f=Graphite::font(Graphite::Type::metadata);
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
 bool grid()const{return view_->viewMode()==QListView::IconMode;}
 int heading(const QModelIndex& i)const{return !grid()&&!i.data(HistoryModel::DayRole).toString().isEmpty()?dayHeadingHeight():0;}
 QRect card(QRect r,const QModelIndex& i)const{return r.adjusted(6,heading(i)+2,-6,-2);}
 // Reserve action space even while it is hidden, so hovering never changes
 // the filename. Narrow rows put actions below instead of losing the title.
 int primaryWidth(const QModelIndex& i)const{return qMax(84,QFontMetrics(Graphite::font()).horizontalAdvance(i.data(HistoryModel::PrimaryActionRole).toString())+28);}
 bool actionsBelow(const QRect& r,const QModelIndex& i)const{
  if(grid())return false;
  const auto c=card(r,i);const int reserve=32+8+(i.data(HistoryModel::PrimaryActionRole).toString().isEmpty()?0:primaryWidth(i)+8);
  return c.right()-12-reserve-textLeft(c)<Graphite::scaled(120);
 }
 QRect primaryRect(QRect r,const QModelIndex& i)const{
  const auto c=card(r,i);const int h=32;const int w=qMin(primaryWidth(i),qMax(0,c.width()-12-12-h-8));
  if(grid())return QRect(c.left()+12,c.bottom()-h-11,w,h);
  return QRect(c.right()-12-h-8-w,actionsBelow(r,i)?c.bottom()-h-11:c.top()+Graphite::scaled(22)-6,w,h);
 }
 QRect menuRect(QRect r,const QModelIndex& i)const{const auto b=primaryRect(r,i);const auto c=card(r,i);return QRect(grid()?c.right()-12-32:c.right()-12-32,b.top(),32,32);}
 static QString errorText(const QModelIndex& i){return HistoryModel::statusText(i.data(HistoryModel::RecordRole).toJsonObject());}
 int textLeft(const QRect& c)const{return c.left()+12+Graphite::Metric::thumbWidth+16;}
 QSize sizeHint(const QStyleOptionViewItem& option,const QModelIndex& i)const override{
  const int available=qMax(160,view_->viewport()->width()-2*view_->spacing());const int width=grid()?qMin(248,available):available;
  int height=grid()?268:qMax(Graphite::Metric::historyRow,Graphite::scaled(Graphite::Metric::historyRow))+heading(i);
  if(actionsBelow(QRect(0,0,width,height),i))height+=44;
  if(i.data(HistoryModel::FailedRole).toBool()){
   const auto area=HistoryWindow::failureTextArea(QSize(width,height),grid(),heading(i)>0);
   const auto document=failureText(errorText(i),Graphite::font(Graphite::Type::metadata),area.width());
   height+=int(std::ceil(document->size().height()))+(grid()?0:4);
  }
  return {width,height};
 }
 void paint(QPainter* p,const QStyleOptionViewItem& option,const QModelIndex& i)const override{
  const auto& t=Theme::current();p->save();p->setRenderHint(QPainter::Antialiasing);const auto outer=option.rect;const auto group=i.data(HistoryModel::DayRole).toString();
  if(heading(i))Graphite::paintText(*p,QRectF(outer.left()+18,outer.top()+6,outer.width()-36,heading(i)-6),group,t.textMuted,Graphite::font(Graphite::Type::micro,true));
  const auto r=card(outer,i);const bool chosen=option.state&QStyle::State_Selected;const bool hover=option.state&QStyle::State_MouseOver;const bool failed=i.data(HistoryModel::FailedRole).toBool();
  const auto record=i.data(HistoryModel::RecordRole).toJsonObject();const auto op=record["operation"].toObject();
  // Row: quiet hover, the selected fill and a small amber marker.
  if(grid()){p->setPen(QPen(chosen?Graphite::mix(t.borderStrong,t.selected,0.3):t.border,1));p->setBrush(chosen?t.selected:hover?t.surfaceRaised:t.surface);p->drawRoundedRect(QRectF(r).adjusted(0.5,0.5,-0.5,-0.5),8,8);}
  else{p->setPen(Qt::NoPen);p->setBrush(chosen?t.selected:hover?t.surface:t.bg);p->drawRoundedRect(r,7,7);}
  if(chosen&&!grid()){p->setPen(Qt::NoPen);p->setBrush(t.accent);p->drawRoundedRect(QRectF(r.left()+3,r.top()+18,2.5,qMax(10,r.height()-36)),1.25,1.25);}
  const int rowTop=r.top();const int rowHeight=grid()?r.height():Graphite::scaled(Graphite::Metric::historyRow)-4;
  QRect thumb=grid()?QRect(r.left()+1,r.top()+1,r.width()-2,145):QRect(r.left()+12,rowTop+(rowHeight-Graphite::Metric::thumbHeight)/2,Graphite::Metric::thumbWidth,Graphite::Metric::thumbHeight);
  p->setPen(Qt::NoPen);p->setBrush(t.surfaceRaised);p->drawRoundedRect(thumb,grid()?8:5,grid()?8:5);
  const auto image=qvariant_cast<QImage>(i.data(HistoryModel::ThumbnailRole));
  if(!image.isNull()){const auto scaled=image.scaled(thumb.size(),Qt::KeepAspectRatio,Qt::SmoothTransformation);const QPoint origin(thumb.center().x()-scaled.width()/2,thumb.center().y()-scaled.height()/2);p->drawImage(origin,scaled);if(video(record))paintVideoBadge(*p,QRect(origin,scaled.size()),HistoryModel::duration(image.text(HistoryModel::durationKey).toLongLong()));}
  else{
   // No preview: an honest file-type glyph, never a fabricated picture.
   const auto category=record["category"].toString();const int side=grid()?36:24;
   VisualStyle::icon(category=="images"?"images":category=="videos"?"record":category=="text"?"clipboard":"view-list").paint(p,QRect(thumb.center()-QPoint(side/2,side/2),QSize(side,side)));
  }
  const auto action=i.data(HistoryModel::PrimaryActionRole).toString();const bool actionsShown=grid()||chosen||hover;
  const auto primaryBounds=primaryRect(outer,i);const auto moreBounds=menuRect(outer,i);
  const QRect body=rowBody(outer,grid(),heading(i));
  const auto columns=grid()?Columns{body,{},{}}:historyColumns(body);
  const bool below=actionsBelow(outer,i);
  int itemWidth=qMax(0,columns.item.width()-12);
  if(!grid()&&!below){const int actionLeft=action.isEmpty()?moreBounds.left():primaryBounds.left();itemWidth=qMin(itemWidth,qMax(0,actionLeft-columns.item.left()-8));}
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
   const auto document=failureText(errorText(i),metaFont,area.width());
   p->save();p->translate(area.topLeft());QAbstractTextDocumentLayout::PaintContext context;context.palette.setColor(QPalette::Text,t.textSecondary);document->documentLayout()->draw(p,context);p->restore();
  }
  if(!grid()&&(!actionsShown||below)){
   if(!columns.output.isEmpty())Graphite::paintText(*p,QRectF(columns.output.left(),lineTop,columns.output.width()-8,Graphite::scaled(22)),i.data(HistoryModel::OutputRole).toString(),t.textSecondary,metaFont);
   if(!columns.created.isEmpty())Graphite::paintText(*p,QRectF(columns.created.left(),lineTop,columns.created.width()-8,Graphite::scaled(22)),i.data(HistoryModel::CreatedRole).toString(),t.textSecondary,metaFont);
  }
  if(actionsShown){
   auto* style=view_->style();
   if(!action.isEmpty()){QStyleOptionButton button;button.rect=primaryBounds;button.text=action;button.state=QStyle::State_Enabled|(option.state&QStyle::State_MouseOver&&primaryBounds.contains(view_->viewport()->mapFromGlobal(QCursor::pos()))?QStyle::State_MouseOver:QStyle::State_None);button.fontMetrics=QFontMetrics(Graphite::font());p->save();p->setFont(Graphite::font());style->drawControl(QStyle::CE_PushButton,&button,p,nullptr);p->restore();}
   QStyleOptionButton more;more.rect=moreBounds;more.state=QStyle::State_Enabled|(moreBounds.contains(view_->viewport()->mapFromGlobal(QCursor::pos()))?QStyle::State_MouseOver:QStyle::State_None);style->drawPrimitive(QStyle::PE_PanelButtonCommand,&more,p,nullptr);Graphite::paintChromeGlyph(*p,QRectF(moreBounds).adjusted(7,7,-7,-7),"more",t.textSecondary);
  }
  if(i.data(HistoryModel::ActiveRole).toBool()){const int progress=i.data(HistoryModel::ProgressRole).toInt();if(progress>=0){QRectF bar(columns.item.left(),grid()?r.bottom()-56:rowTop+rowHeight-9,qMax(20,itemWidth),3);p->setPen(Qt::NoPen);p->setBrush(t.selected);p->drawRoundedRect(bar,1.5,1.5);bar.setWidth(bar.width()*progress/100);p->setBrush(t.accent);p->drawRoundedRect(bar,1.5,1.5);}}
  if((option.state&QStyle::State_HasFocus)&&view_->hasFocus()&&view_->window()->testAttribute(Qt::WA_KeyboardFocusChange)){p->setPen(QPen(t.accent,1));p->setBrush(Qt::NoBrush);p->drawRoundedRect(QRectF(r).adjusted(0.5,0.5,-0.5,-0.5),7,7);}
  p->restore();
 }
 bool editorEvent(QEvent* event,QAbstractItemModel*,const QStyleOptionViewItem& option,const QModelIndex& i)override{
  if(event->type()!=QEvent::MouseButtonRelease)return false;
  auto* mouse=static_cast<QMouseEvent*>(event);if(mouse->button()!=Qt::LeftButton)return false;
  if(!i.data(HistoryModel::PrimaryActionRole).toString().isEmpty()&&primaryRect(option.rect,i).contains(mouse->position().toPoint())){if(primary)primary(i);return true;}
  if(menuRect(option.rect,i).contains(mouse->position().toPoint())){if(menu)menu(i,mouse->globalPosition().toPoint());return true;}return false;
 }
private:QListView* view_;
};
}
HistoryWindow::HistoryWindow(UiSession& session,QWidget* parent):RetainedWindow(parent),session_(session){
 chooser_=new FileDialog(this);
 setObjectName("historyPage");setWindowTitle("put.gd — History");resize(1080,760);auto* page=new QHBoxLayout(this);page->setContentsMargins(0,0,0,0);page->setSpacing(0);
 // Sidebar: what to show (All items / Images / Recordings / Other files),
 // failed uploads, and at the bottom the capture output the main window adds.
 auto* sidebar=new Graphite::Surface("surface",Qt::RightEdge,this);sidebar->setObjectName("historySidebar");sidebar->setFixedWidth(Graphite::scaled(Graphite::Metric::historySidebar));page->addWidget(sidebar);
 auto* side=new QVBoxLayout(sidebar);side->setContentsMargins(10,16,10,14);side->setSpacing(8);
 categories_=new QListWidget(sidebar);categories_->setObjectName("historyCategories");categories_->setAccessibleName("Show history items");categories_->setFrameShape(QFrame::NoFrame);categories_->setIconSize(QSize(18,18));categories_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);categories_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);categories_->setItemDelegate(new Graphite::NavigationDelegate(categories_));Graphite::transparentView(categories_);categories_->setMouseTracking(true);
 for(const auto& [glyph,label,category]:QList<std::tuple<QString,QString,QString>>{{"history","All items",""},{"images","Images","images"},{"record","Recordings","videos"},{"view-list","Other files","text+other"}}){auto* item=new QListWidgetItem(VisualStyle::icon(glyph),label,categories_);item->setData(Qt::UserRole,category);}
 categories_->setToolTip("Other files includes clipboard text and any file that is not an image or a recording.");
 categories_->setCurrentRow(0);categories_->setFixedHeight(4*(Graphite::scaled(Graphite::Metric::navRow))+4);side->addWidget(categories_);side->addWidget(Graphite::rule(sidebar));
 failedOnly_=new QToolButton(sidebar);failedOnly_->setObjectName("historyFailedOnly");failedOnly_->setText("Failed uploads");failedOnly_->setIcon(VisualStyle::icon("info"));failedOnly_->setIconSize(QSize(18,18));failedOnly_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);failedOnly_->setCheckable(true);failedOnly_->setProperty("visualRole","quiet");failedOnly_->setProperty("alignLeft",true);failedOnly_->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);failedOnly_->setAccessibleName("Show only failed uploads");failedOnly_->setToolTip("Show only failed uploads (Upload state: failed)");side->addWidget(failedOnly_);
 side->addStretch();sidebarFooter_=new QVBoxLayout;sidebarFooter_->setContentsMargins(6,0,6,0);sidebarFooter_->setSpacing(4);side->addLayout(sidebarFooter_);
 auto* content=new QWidget(this);content->setObjectName("historyContent");page->addWidget(content,1);auto* layout=new QVBoxLayout(content);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);
 auto* headerBar=new QWidget(content);auto* header=new QHBoxLayout(headerBar);header->setContentsMargins(18,12,16,10);header->setSpacing(8);auto* heading=new QLabel("History",this);VisualStyle::role(heading,"sectionTitle");header->addWidget(heading);header->addStretch();
 auto* search=new QLineEdit(this);search->setObjectName("historySearch");search->setAccessibleName("Search history by file name");search->setPlaceholderText("Search history");search->setMaxLength(256);search->setClearButtonEnabled(true);search->setMinimumWidth(160);search->setMaximumWidth(300);header->addWidget(search,1);
 auto* advancedToggle=new QToolButton(this);advancedToggle->setObjectName("historyFiltersExpanded");advancedToggle->setText("Filters");Graphite::disclosure(advancedToggle);advancedToggle->setAccessibleName("Show advanced history filters");header->addWidget(advancedToggle);
 // List and Gallery: one segmented choice.
 auto* listButton=new QToolButton(this);listButton->setObjectName("historyListMode");listButton->setText("List");listButton->setCheckable(true);listButton->setChecked(true);listButton->setAccessibleName("Show history as a list");grid_=new QToolButton(this);grid_->setObjectName("historyGridMode");grid_->setText("Gallery");grid_->setCheckable(true);grid_->setAccessibleName("Show history as a thumbnail gallery");for(auto* b:{listButton,grid_}){b->setProperty("visualRole","quiet");b->setFocusPolicy(Qt::StrongFocus);header->addWidget(b);}
 layout->addWidget(headerBar);
 connect(listButton,&QToolButton::clicked,this,[this]{setGridMode(false);});connect(grid_,&QToolButton::clicked,this,[this]{setGridMode(true);});
 auto* advanced=new QWidget(this);auto* advancedBox=new QVBoxLayout(advanced);advancedBox->setContentsMargins(18,0,16,10);advancedBox->setSpacing(8);auto* filters=new QGridLayout;filters->setContentsMargins(0,0,0,0);filters->setHorizontalSpacing(8);filters->setVerticalSpacing(8);const QStringList keys{"from","to","mime","profileId","uploadStatus","localStatus","remoteStatus"};const QStringList labels{"From date (ISO)","To date (ISO)","Content type","Destination ID (advanced)","Upload state","Local-copy state","Remote-deletion state"};for(int i=0;i<keys.size();++i){auto* edit=new QLineEdit(this);edit->setMaxLength(256);edit->setObjectName("historyFilter"+keys[i]);edit->setPlaceholderText(labels[i]);edit->setAccessibleName(labels[i]);edit->setToolTip(labels[i]);filters->addWidget(edit,i/4,i%4);connect(edit,&QLineEdit::returnPressed,this,&HistoryWindow::applyFilters);}
 auto* filter=new QPushButton("Apply filters",this);filter->setObjectName("historyApplyFilters");filters->addWidget(filter,1,3);advancedBox->addLayout(filters);layout->addWidget(advanced);advanced->hide();connect(advancedToggle,&QToolButton::toggled,advanced,&QWidget::setVisible);
 activeBanner_=new QWidget(this);activeBanner_->setObjectName("historyActiveBanner");auto* activeLayout=new QHBoxLayout(activeBanner_);activeLayout->setContentsMargins(18,6,16,8);activeLayout->setSpacing(10);activeLabel_=new QLabel(this);activeLabel_->setTextFormat(Qt::PlainText);activeLabel_->setWordWrap(true);activeLayout->addWidget(activeLabel_,1);progress_=new QProgressBar(this);progress_->setAccessibleName("Active upload progress");progress_->setTextVisible(false);progress_->setMaximumWidth(180);activeLayout->addWidget(progress_);auto* activeCancel=new QPushButton("Cancel",this);activeCancel->setAccessibleName("Cancel active capture or upload");activeLayout->addWidget(activeCancel);connect(activeCancel,&QPushButton::clicked,this,[this]{if(activeId_.isEmpty())return;session_.request("operation.cancel",{{"id",activeId_}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g){VisualStyle::feedback(g->status_,e?e.message:"Cancellation requested.",bool(e));g->refresh();}});});layout->addWidget(activeBanner_);activeBanner_->hide();
 previewRequestTimer_=new QTimer(this);previewRequestTimer_->setSingleShot(true);previewRequestTimer_->setInterval(400);connect(previewRequestTimer_,&QTimer::timeout,this,&HistoryWindow::requestPreview);
 searchTimer_=new QTimer(this);searchTimer_->setSingleShot(true);searchTimer_->setInterval(250);connect(searchTimer_,&QTimer::timeout,this,&HistoryWindow::applyFilters);connect(search,&QLineEdit::textEdited,this,[this]{searchTimer_->start();});connect(search,&QLineEdit::returnPressed,this,&HistoryWindow::applyFilters);connect(filter,&QPushButton::clicked,this,&HistoryWindow::applyFilters);
 connect(categories_,&QListWidget::currentRowChanged,this,[this]{if(restoringCategory_)return;applyFilters();});
 connect(failedOnly_,&QToolButton::toggled,this,[this](bool on){auto* state=findChild<QLineEdit*>("historyFilteruploadStatus");if(on)state->setText("failed");else if(state->text()=="failed")state->clear();applyFilters();});
 model_=new HistoryModel(session,this);table_=new QListView(this);table_->setObjectName("historyList");table_->setAccessibleName("Captures and upload activity");table_->setModel(model_);table_->setFrameShape(QFrame::NoFrame);Graphite::transparentView(table_);table_->setMouseTracking(true);table_->setSelectionMode(QAbstractItemView::ExtendedSelection);table_->setResizeMode(QListView::Adjust);table_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);table_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);table_->setSpacing(1);table_->setContextMenuPolicy(Qt::CustomContextMenu);auto* delegate=new HistoryDelegate(table_);table_->setItemDelegate(delegate);delegate->primary=[this](const QModelIndex& i){primaryAction(i);};delegate->menu=[this](const QModelIndex& i,const QPoint& p){showRowMenu(i,p);};connect(table_,&QWidget::customContextMenuRequested,this,[this](const QPoint& p){const auto i=table_->indexAt(p);if(i.isValid())showRowMenu(i,table_->viewport()->mapToGlobal(p));});auto* rowMenu=new QAction(this);rowMenu->setShortcuts({QKeySequence(Qt::Key_Menu),QKeySequence(Qt::SHIFT|Qt::Key_F10)});rowMenu->setShortcutContext(Qt::WidgetShortcut);table_->addAction(rowMenu);connect(rowMenu,&QAction::triggered,this,[this]{const auto i=table_->currentIndex();if(i.isValid())showRowMenu(i,table_->viewport()->mapToGlobal(table_->visualRect(i).center()));});
 // Empty history and no matching items say different things (applyFilters).
 empty_=new QLabel("No captures yet\nCapture a region or upload your clipboard to get started.",this);empty_->setObjectName("historyEmpty");empty_->setWordWrap(true);empty_->setAlignment(Qt::AlignCenter);VisualStyle::role(empty_,"muted");
 auto* listPane=new QWidget(this);auto* listBox=new QVBoxLayout(listPane);listBox->setContentsMargins(0,0,0,0);listBox->setSpacing(0);columns_=new HistoryColumns(table_,listPane);listBox->addWidget(columns_);listBox->addWidget(table_,1);
 list_=new QStackedWidget(this);list_->addWidget(listPane);list_->addWidget(empty_);
 split_=new QSplitter(Qt::Horizontal,this);split_->setObjectName("historySplitter");split_->setChildrenCollapsible(false);split_->addWidget(list_);inspectorScroll_=new QScrollArea(this);inspectorScroll_->setObjectName("historyInspectorScroll");inspectorScroll_->setWidgetResizable(true);inspectorScroll_->setFrameShape(QFrame::NoFrame);inspectorScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);auto* inspector=new Graphite::Surface("surface",{},this);inspector->setObjectName("historyInspector");auto* inspect=new QVBoxLayout(inspector);inspect->setContentsMargins(18,16,18,16);inspect->setSpacing(10);
 preview_=new QLabel("Select a capture to preview",this);preview_->setObjectName("historyPreview");VisualStyle::role(preview_,"well");preview_->setAccessibleName("Selected capture preview");preview_->setAlignment(Qt::AlignCenter);preview_->setWordWrap(true);preview_->setMinimumHeight(180);preview_->setMaximumHeight(260);preview_->setMargin(8);preview_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);inspect->addWidget(preview_);
 title_=new QLabel(this);title_->setTextFormat(Qt::PlainText);title_->setObjectName("historyTitle");title_->setWordWrap(true);VisualStyle::role(title_,"sectionTitle");inspect->addWidget(title_);outcome_=new QLabel(this);outcome_->setTextFormat(Qt::PlainText);outcome_->setWordWrap(true);outcome_->setObjectName("historyOutcome");inspect->addWidget(outcome_);
 items_=new QComboBox(this);items_->setObjectName("historyItems");items_->setAccessibleName("Select a file in this capture");items_->hide();inspect->addWidget(items_);connect(items_,&QComboBox::currentIndexChanged,this,[this]{if(restoringSelection_)return;selectedItemId_=items_->currentData().toString();loadSelectedItem();emit viewStateChanged();});
 inspect->addWidget(Graphite::rule(inspector));
 link_=new QLineEdit(this);link_->setObjectName("historyLink");link_->setReadOnly(true);VisualStyle::role(link_,"link");link_->setAccessibleName("Stored share link");link_->setPlaceholderText("No share link available");inspect->addWidget(link_);
 linkExpiry_=new QLabel(this);linkExpiry_->setObjectName("historyLinkExpiry");linkExpiry_->setTextFormat(Qt::PlainText);VisualStyle::role(linkExpiry_,"meta");linkExpiry_->hide();inspect->addWidget(linkExpiry_);
 copy_=new QPushButton("Copy link",this);copy_->setObjectName("copyHistoryLink");VisualStyle::role(copy_,"primary");inspect->addWidget(copy_);
 auto* actions=new QGridLayout;actions->setSpacing(8);open_=new QPushButton("Open in browser",this);open_->setObjectName("openHistoryLink");copyImage_=new QPushButton("Copy image",this);copyImage_->setObjectName("copyHistoryImage");saveAs_=new QPushButton("Save a copy…",this);saveAs_->setObjectName("saveHistoryAs");saveAs_->setToolTip("Save a copy of the retained local original");actions->addWidget(open_,0,0);actions->addWidget(copyImage_,0,1);actions->addWidget(saveAs_,1,0);editAgain_=new QPushButton("Edit again",this);editAgain_->setObjectName("editHistoryImage");editAgain_->setToolTip("Edit the retained local PNG or JPEG; the result is a new item");actions->addWidget(editAgain_,1,1);connect(editAgain_,&QPushButton::clicked,this,[this]{editAgain(currentDetail_);});retry_=new QPushButton("Retry upload",this);retry_->setObjectName("retryOperation");VisualStyle::role(retry_,"primary");cancel_=new QPushButton("Cancel upload",this);cancel_->setObjectName("cancelOperation");actions->addWidget(retry_,2,0);actions->addWidget(cancel_,2,1);saveDiagnostics_=new QPushButton("Save diagnostics for this upload",this);saveDiagnostics_->setObjectName("saveHistoryDiagnostics");saveDiagnostics_->setToolTip("Save a redacted diagnostics package (.zip) for this upload on this computer. Nothing is uploaded.");actions->addWidget(saveDiagnostics_,3,0,1,2);saveDiagnostics_->hide();connect(saveDiagnostics_,&QPushButton::clicked,this,[this]{const auto rows=table_->selectionModel()->selectedRows();if(rows.size()==1)saveDiagnostics(model_->record(rows.first().row())["operationId"].toString());});actions->setColumnStretch(0,1);actions->setColumnStretch(1,1);inspect->addLayout(actions);
 share_=new QPushButton("Create share link",this);share_->setObjectName("shareHistoryLink");revoke_=new QPushButton("Revoke link…",this);revoke_->setObjectName("revokeHistoryLink");inspectLink_=new QPushButton("Inspect link",this);inspectLink_->setObjectName("inspectHistoryLink");auto* sharing=new QHBoxLayout;sharing->setSpacing(8);for(auto* b:{share_,revoke_,inspectLink_}){sharing->addWidget(b);b->hide();}inspect->addLayout(sharing);connect(share_,&QPushButton::clicked,this,[this]{changeSharing("create");});connect(revoke_,&QPushButton::clicked,this,[this]{changeSharing("revoke");});connect(inspectLink_,&QPushButton::clicked,this,[this]{changeSharing("inspect");});
 inspect->addWidget(Graphite::rule(inspector));
 summary_=new QLabel(this);summary_->setObjectName("historySummary");summary_->setAccessibleName("File size, pixel dimensions, destination and outcomes");summary_->setWordWrap(true);summary_->setTextFormat(Qt::RichText);summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);summary_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);inspect->addWidget(summary_);
 auto* receiptToggle=new QToolButton(this);receiptToggle->setObjectName("historyReceiptExpanded");receiptToggle->setText("Advanced file details");Graphite::disclosure(receiptToggle);receiptToggle->setAccessibleName("Show advanced receipt details");inspect->addWidget(receiptToggle,0,Qt::AlignLeft);details_=new QPlainTextEdit(this);details_->setObjectName("historyDetails");details_->setReadOnly(true);VisualStyle::role(details_,"diagnostics");details_->setAccessibleName("Advanced original receipt details");details_->setMaximumHeight(180);details_->hide();inspect->addWidget(details_);connect(receiptToggle,&QToolButton::toggled,details_,&QWidget::setVisible);inspect->addStretch();
 auto* remove=new QPushButton("Delete from destination…",this);remove->setObjectName("deleteHistoryRemote");VisualStyle::role(remove,"danger");remove->setToolTip("Delete the recorded remote file. Local copies and history records remain.");inspect->addWidget(remove,0,Qt::AlignLeft);connect(remove,&QPushButton::clicked,this,&HistoryWindow::prepareDelete);
 inspectorScroll_->setWidget(inspector);split_->addWidget(inspectorScroll_);layout->addWidget(split_,1);inspectorScroll_->hide();
 auto* footerBar=new Graphite::Surface("bg",Qt::TopEdge,content);auto* footerBox=new QVBoxLayout(footerBar);footerBox->setContentsMargins(16,8,16,10);footerBox->setSpacing(8);
 auto* footer=new QHBoxLayout;footer->setSpacing(8);// Counts elide (full text in the tooltip) before they crowd the paging controls.
 count_=new Graphite::ElidedLabel(this);VisualStyle::role(count_,"meta");footer->addWidget(count_,2);cancelled_=new Graphite::ElidedLabel(this);VisualStyle::role(cancelled_,"meta");cancelled_->setAlignment(Qt::AlignRight|Qt::AlignVCenter);footer->addWidget(cancelled_,1);showCancelled_=new QToolButton(this);showCancelled_->setObjectName("historyShowCancelled");showCancelled_->setText("Show");showCancelled_->setCheckable(true);showCancelled_->setProperty("visualRole","quiet");showCancelled_->setAccessibleName("Show cancelled captures");footer->addWidget(showCancelled_);connect(showCancelled_,&QToolButton::toggled,this,[this](bool show){model_->setShowCancelled(show);showCancelled_->setText(show?"Hide":"Show");emit viewStateChanged();});auto* previous=new QPushButton("Previous",this);previous->setObjectName("historyPrevious");auto* next=new QPushButton("Next",this);next->setObjectName("historyNext");footer->addWidget(previous);footer->addWidget(next);connect(previous,&QPushButton::clicked,this,[this]{auto q=model_->query();q.offset=qMax(0,q.offset-q.limit);model_->setQuery(q);});connect(next,&QPushButton::clicked,this,[this]{auto q=model_->query();if(q.offset+q.limit<model_->total()){q.offset+=q.limit;model_->setQuery(q);}});auto* managementToggle=new QToolButton(this);managementToggle->setObjectName("historyManagementExpanded");managementToggle->setText("Manage");Graphite::disclosure(managementToggle);managementToggle->setAccessibleName("Show history management actions");footer->addWidget(managementToggle);footerBox->addLayout(footer);
 status_=new QLabel("Select a capture to inspect its files and share link.",this);status_->setTextFormat(Qt::PlainText);status_->setAccessibleName("History status");status_->setWordWrap(true);VisualStyle::role(status_,"status");footerBox->addWidget(status_);
 auto* management=new QWidget(this);auto* manage=new QHBoxLayout(management);manage->setContentsMargins(0,0,0,0);manage->setSpacing(8);auto* deletionRetry=new QPushButton("Retry deletion",this);auto* deletionCancel=new QPushButton("Cancel deletion",this);auto* clear=new QPushButton("Clear history records…",this);auto* cache=new QPushButton("Clean retry artifacts and previews…",this);for(auto* b:{deletionRetry,deletionCancel,clear,cache})manage->addWidget(b);manage->addStretch();footerBox->addWidget(management);management->hide();connect(managementToggle,&QToolButton::toggled,management,&QWidget::setVisible);layout->addWidget(footerBar);
 sharingTimer_=new QTimer(this);sharingTimer_->setInterval(500);connect(sharingTimer_,&QTimer::timeout,this,&HistoryWindow::pollSharing);deletionTimer_=new QTimer(this);deletionTimer_->setInterval(500);connect(deletionTimer_,&QTimer::timeout,this,&HistoryWindow::pollDeletion);
 connect(deletionRetry,&QPushButton::clicked,this,[this]{if(deletionId_.isEmpty())return;session_.request("delete.retry",{{"id",deletionId_}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g){VisualStyle::feedback(g->status_,e?e.message:"Deletion retry accepted.",bool(e));if(!e)g->deletionTimer_->start();}});});connect(deletionCancel,&QPushButton::clicked,this,[this]{if(deletionId_.isEmpty())return;session_.request("delete.cancel",{{"id",deletionId_}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g)VisualStyle::feedback(g->status_,e?e.message:"Cancellation requested; already sent deletion cannot be rolled back.",bool(e));});});
 connect(clear,&QPushButton::clicked,this,[this]{if(!Graphite::confirm(this,"Clear history records","Clear every local history record? Remote objects and user-owned local copies remain.","Clear history records",true))return;session_.request("history.clear",{},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g){VisualStyle::feedback(g->status_,e?e.message:"History cleared.",bool(e));g->refresh();}});});connect(cache,&QPushButton::clicked,this,[this]{if(!Graphite::confirm(this,"Clean cached files","Clean eligible retry artifacts and image previews? Some cleaned artifacts cannot retry. Persistent history, remote objects and local copies remain.","Clean artifacts",true))return;session_.request("cache.clear",{},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject result){if(g){g->releasePreview();
  // An incomplete upload that could not be aborted keeps using storage (#146).
  const auto kept=result["incompleteUploads"].toArray();const auto uploads=kept.size()==1?QStringLiteral("an incomplete upload"):QStringLiteral("%1 incomplete uploads").arg(kept.size());
  const auto notice=result["incompleteUploadNotice"].toObject()["message"].toString();
  const auto message=kept.isEmpty()?QStringLiteral("Artifact cleanup completed."):!notice.isEmpty()?notice:QStringLiteral("Artifact cleanup completed, but %1 could not be removed. %2").arg(uploads,kept[0].toObject()["error"].toObject()["message"].toString());
  VisualStyle::feedback(g->status_,e?e.message:message,bool(e)||!kept.isEmpty());g->refresh();}});});
 connect(copy_,&QPushButton::clicked,this,[this]{const auto record=selectedRecord();if(record.isEmpty())return;if(record["uploadStatus"]=="local_only"){operationAction("operation.copy");return;}session_.request("history.copyLink",{{"id",record["id"]}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g)VisualStyle::feedback(g->status_,e?e.message:"Stored link copied.",bool(e));});});connect(open_,&QPushButton::clicked,this,[this]{if(currentDetail_["linkAvailable"].toBool())host::openLink(QUrl(link_->text()));});connect(retry_,&QPushButton::clicked,this,[this]{const auto file=selectedRecord();operationAction(file["uploadStatus"]=="local_only"?"operation.retryLocalSave":"operation.retry");});connect(cancel_,&QPushButton::clicked,this,[this]{operationAction("operation.cancel");});connect(copyImage_,&QPushButton::clicked,this,[this]{exportLocal(true);});connect(saveAs_,&QPushButton::clicked,this,[this]{exportLocal(false);});
 connect(table_->selectionModel(),&QItemSelectionModel::selectionChanged,this,[this]{if(!restoringSelection_){selectedItemId_.clear();selectionChanged();}});connect(model_,&QAbstractItemModel::modelAboutToBeReset,this,[this]{if(!restoringSelection_)restoreSelection_=selectedIds();restoringSelection_=true;});
 connect(model_,&HistoryModel::loaded,this,[this]{for(int row=0;row<model_->rowCount();++row){const auto r=model_->record(row);bool selected=restoreSelection_.contains("operation:"+r["operationId"].toString());for(auto v:r["records"].toArray())selected|=restoreSelection_.contains(v.toObject()["id"].toString());if(selected){table_->selectionModel()->select(model_->index(row),QItemSelectionModel::Select|QItemSelectionModel::Rows);if(!table_->currentIndex().isValid())table_->selectionModel()->setCurrentIndex(model_->index(row),QItemSelectionModel::NoUpdate);}}restoreSelection_.clear();restoringSelection_=false;{const auto& q=model_->query();const bool filtered=!(q.search.isEmpty()&&q.category.isEmpty()&&q.mime.isEmpty()&&q.profileId.isEmpty()&&q.uploadStatus.isEmpty()&&q.localStatus.isEmpty()&&q.remoteStatus.isEmpty()&&q.from.isEmpty()&&q.to.isEmpty());empty_->setText(filtered?QString("No matching items\nTry another search or change the filters."):QString("No captures yet\nCapture a region or upload your clipboard to get started."));}list_->setCurrentWidget(model_->rowCount()?table_->parentWidget():static_cast<QWidget*>(empty_));count_->setText(QString("%1 captures · %2 stored files").arg(model_->rowCount()).arg(model_->total()));count_->setToolTip(count_->text());cancelled_->setText(QString("%1 cancelled captures %2").arg(model_->hiddenCancelled()).arg(model_->showCancelled()?"shown":"hidden"));cancelled_->setToolTip(cancelled_->text());showCancelled_->setVisible(model_->hiddenCancelled()>0||model_->showCancelled());cancelled_->setVisible(showCancelled_->isVisible());selectionChanged();updateActivity();if(awaitingInitialization_){awaitingInitialization_=false;VisualStyle::feedback(status_,"Select a capture to inspect its files and share link.");}emit viewStateChanged();});connect(model_,&HistoryModel::error,this,[this](const QString& message){VisualStyle::feedback(status_,message,true);});
 connect(&session_,&UiSession::operationsChanged,this,&HistoryWindow::refresh);connect(&session_,&UiSession::initialized,this,&HistoryWindow::refresh);for(auto* e:findChildren<QLineEdit*>())if(!e->isReadOnly())connect(e,&QLineEdit::textChanged,this,&HistoryWindow::viewStateChanged);for(auto* t:{advancedToggle,managementToggle,receiptToggle})connect(t,&QToolButton::toggled,this,&HistoryWindow::viewStateChanged);connect(categories_,&QListWidget::currentRowChanged,this,&HistoryWindow::viewStateChanged);
 for(auto* b:findChildren<QPushButton*>())if(b->accessibleName().isEmpty())b->setAccessibleName(b->text());
 // Pixmaps and the summary's HTML colours are redrawn on a scheme switch (#60).
 connect(&Theme::Notifier::instance(),&Theme::Notifier::changed,this,[this]{updateSummary();columns_->update();});
 setGridMode(false);refresh();
}
void HistoryWindow::applyFilters(){searchTimer_->stop();auto q=model_->query();q.offset=0;if(auto* item=categories_->currentItem())q.category=item->data(Qt::UserRole).toString();{const QSignalBlocker block(failedOnly_);failedOnly_->setChecked(findChild<QLineEdit*>("historyFilteruploadStatus")->text()=="failed");}q.search=findChild<QLineEdit*>("historySearch")->text();q.from=findChild<QLineEdit*>("historyFilterfrom")->text();q.to=findChild<QLineEdit*>("historyFilterto")->text();q.mime=findChild<QLineEdit*>("historyFiltermime")->text();q.profileId=findChild<QLineEdit*>("historyFilterprofileId")->text();q.uploadStatus=findChild<QLineEdit*>("historyFilteruploadStatus")->text();q.localStatus=findChild<QLineEdit*>("historyFilterlocalStatus")->text();q.remoteStatus=findChild<QLineEdit*>("historyFilterremoteStatus")->text();model_->setQuery(q);emit viewStateChanged();}
void HistoryWindow::setGridMode(bool grid){grid_->setChecked(grid);findChild<QToolButton*>("historyListMode")->setChecked(!grid);table_->setViewMode(grid?QListView::IconMode:QListView::ListMode);table_->setMovement(QListView::Static);table_->setWrapping(grid);table_->setGridSize(QSize());table_->setUniformItemSizes(false);table_->doItemsLayout();updateInspectorLayout();emit viewStateChanged();}
void HistoryWindow::updateInspectorLayout(){if(!split_)return;
 // The list owns the space; the inspector keeps 280-380 px beside it, or
 // stacks below it when the page is too narrow for both.
 const bool stacked=split_->width()<640;split_->setOrientation(stacked?Qt::Vertical:Qt::Horizontal);inspectorScroll_->setMinimumWidth(stacked?0:Graphite::scaled(280));inspectorScroll_->setMaximumWidth(stacked?QWIDGETSIZE_MAX:Graphite::scaled(400));inspectorScroll_->setMaximumHeight(stacked?330:QWIDGETSIZE_MAX);split_->setStretchFactor(0,1);split_->setStretchFactor(1,0);if(columns_)columns_->setVisible(!grid_->isChecked());inspectorScroll_->setVisible(table_->selectionModel()->selectedRows().size()==1&&!grid_->isChecked());}
void HistoryWindow::resizeEvent(QResizeEvent* event){RetainedWindow::resizeEvent(event);updateInspectorLayout();}
void HistoryWindow::refresh(){if(!session_.ready()){awaitingInitialization_=true;VisualStyle::feedback(status_,"Waiting for the resident journal to initialize.");return;}model_->refresh();}
QStringList HistoryWindow::selectedIds()const{QStringList ids;for(auto index:table_->selectionModel()->selectedRows()){auto r=model_->record(index.row());const auto records=r["records"].toArray();if(records.isEmpty())ids.append("operation:"+r["operationId"].toString());else for(auto v:records)ids.append(v.toObject()["id"].toString());}return ids;}
QJsonObject HistoryWindow::selectedRecord()const{const auto rows=table_->selectionModel()->selectedRows();if(rows.size()!=1)return {};const auto group=model_->record(rows.first().row());for(const auto& v:group["records"].toArray())if(v.toObject()["id"]==items_->currentData().toString())return v.toObject();return group["records"].toArray().isEmpty()?QJsonObject{}:group["records"].toArray().first().toObject();}
QString HistoryWindow::destinationLabel(const QJsonObject& r)const{
 if(r["uploadStatus"]=="local_only")return "Local copy";
 const auto original=r["receipt"].toObject()["destination"].toObject();const auto settings=original["settings"].toObject();const auto provider=original["providerId"].toString();QString label=provider=="proton-drive"?"Proton Drive":provider=="s3"?"S3 destination":provider=="backblaze-b2"?"Backblaze B2":"Original destination";
 for(auto v:session_.document()["profiles"].toArray()){const auto profile=v.toObject();if(profile["id"]==r["profileId"]){label=profile["label"].toString();break;}}
 if(provider=="proton-drive"&&!settings["folderName"].toString().isEmpty())label+=" / "+settings["folderName"].toString();else if((provider=="s3"||provider=="backblaze-b2")&&!settings["objectPrefix"].toString().isEmpty())label+=" / "+settings["objectPrefix"].toString();return label;
}
void HistoryWindow::selectionChanged(){emit viewStateChanged();const auto rows=table_->selectionModel()->selectedRows();
 // The instruction shows only while it applies; feedback stays until replaced.
 {static const QString instruction="Select a capture to inspect its files and share link.";if(rows.size()==1&&status_->text()==instruction)VisualStyle::feedback(status_,{});else if(rows.isEmpty()&&status_->text().isEmpty())VisualStyle::feedback(status_,instruction);}{QSignalBlocker block(items_);items_->clear();if(rows.size()==1){const auto group=model_->record(rows.first().row());for(auto v:group["records"].toArray()){auto r=v.toObject();items_->addItem(r["name"].toString(),r["id"].toString());}const auto itemIndex=items_->findData(selectedItemId_);if(itemIndex>=0)items_->setCurrentIndex(itemIndex);items_->setVisible(items_->count()>1);}}loadSelectedItem();updateInspectorLayout();}
void HistoryWindow::loadSelectedItem(){
 if(auto next=std::exchange(afterDetail_,{}))next(false);
 // History reloads the selection whenever the resident reports a change.
 // The item it already shows keeps its preview and size until the reload
 // brings a different preview, so the inspector never flashes empty.
 const auto next=selectedRecord()["id"].toString();const bool keepPreview=!next.isEmpty()&&next==shownItemId_;
 if(!keepPreview){if(previewCancelled_)previewCancelled_->store(true);shownItemId_.clear();shownPreviewPath_.clear();pixelDimensions_.clear();preview_->clear();}
 pendingPreviewPath_.clear();const auto seq=++selectionRequest_;currentDetail_={};details_->clear();summary_->clear();title_->clear();outcome_->clear();link_->clear();linkExpiry_->clear();linkExpiry_->hide();for(auto* b:{copy_,open_,copyImage_,saveAs_,editAgain_})b->setEnabled(false);for(auto* b:{share_,revoke_,inspectLink_,retry_,cancel_,saveDiagnostics_})b->hide();
 const auto rows=table_->selectionModel()->selectedRows();findChild<QPushButton*>("deleteHistoryRemote")->setEnabled(!rows.isEmpty());if(rows.size()!=1)return;const auto group=model_->record(rows.first().row());title_->setText(rows.first().data().toString());outcome_->setText(HistoryModel::statusText(group));VisualStyle::role(outcome_,model_->data(rows.first(),HistoryModel::FailedRole).toBool()?"danger":group["uploadStatus"]=="uploaded"?"success":"muted");retry_->setText(model_->data(rows.first(),HistoryModel::PrimaryActionRole).toString()=="Retry local save"?"Retry local save":"Retry upload");saveDiagnostics_->setVisible(diagnosticsAvailable(rows.first().row()));retry_->setVisible(model_->data(rows.first(),HistoryModel::FailedRole).toBool()&&!model_->data(rows.first(),HistoryModel::ActiveRole).toBool()&&!group["operationId"].toString().isEmpty()&&!HistoryModel::storedNothing(group));cancel_->setVisible(model_->data(rows.first(),HistoryModel::ActiveRole).toBool());
 const auto record=selectedRecord();if(record.isEmpty()){summary_->setText("Capture in progress. File details appear after acquisition.");return;}selectedItemId_=record["id"].toString();if(!deletionTimer_->isActive())deletionId_=record["deleteOperationId"].toString();
 session_.request("history.detail",{{"id",record["id"]}},this,[g=QPointer<HistoryWindow>(this),seq](Error e,QJsonObject data){if(!g||g->selectionRequest_!=seq)return;if(e){VisualStyle::feedback(g->status_,e.message,true);if(auto next=std::exchange(g->afterDetail_,{}))next(false);return;}g->currentDetail_=data;g->details_->setPlainText(QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Indented)));g->title_->setText(g->items_->count()>1?data["name"].toString():g->table_->currentIndex().data().toString());g->link_->setText(data["receipt"].toObject()["directUrl"].toString());g->link_->setCursorPosition(0);g->link_->setToolTip(g->link_->text());g->linkExpiry_->setText(linkExpiry(data));g->linkExpiry_->setVisible(!g->linkExpiry_->text().isEmpty());auto local=data["local"].toObject();const auto localPath=local["path"].toString();const bool localAvailable=local["status"]=="saved"&&QFileInfo(localPath).isFile();g->saveAs_->setEnabled(localAvailable);g->copyImage_->setEnabled(localAvailable&&data["mime"].toString().startsWith("image/")&&data["bytes"].toInteger()<=32*1024*1024);g->saveAs_->setToolTip(localAvailable?"Save the retained local original to another file":"A retained local original is needed to save another copy");g->copyImage_->setToolTip(g->copyImage_->isEnabled()?"Copy the retained original image":"A retained local image is needed to copy image bytes");g->editAgain_->setEnabled(editable(data));g->editAgain_->setToolTip(g->editAgain_->isEnabled()?"Open the retained local image in the editor and upload the result as a new item":data["mime"]=="image/png"||data["mime"]=="image/jpeg"?"Edit again needs the local copy. Turn on Keep a local copy in Settings → General for future captures.":"Only PNG and JPEG images can be edited");if(auto next=std::exchange(g->afterDetail_,{}))next(true);
 g->updateSummary();
 const bool sharing=data["capabilities"].toObject()["sharing"].toBool();for(auto* b:{g->share_,g->revoke_,g->inspectLink_})b->setVisible(sharing);const auto linkState=data["shareStatus"].toString();const bool stopped=g->sharingStopped_&&g->sharingId_==data["shareOperationId"].toString();const bool busy=g->sharingAccepting_||g->sharingTimer_->isActive()||(linkState=="pending"&&!stopped);g->share_->setEnabled(!busy&&linkState!="pending"&&linkState!="unknown"&&linkState!="changed");g->revoke_->setEnabled(!busy&&data["linkAvailable"].toBool());g->inspectLink_->setEnabled(!busy);const bool localResult=data["uploadStatus"]=="local_only"&&local["status"]=="saved";g->copy_->setText(localResult?"Copy result":"Copy link");g->copy_->setAccessibleName(g->copy_->text());g->copy_->setEnabled(data["linkAvailable"].toBool()||localResult);g->open_->setEnabled(data["linkAvailable"].toBool());
 // Actions come from capabilities and retained bytes: one that cannot apply
 // to this item is not shown, and a local-only result has no link field.
 for(auto* b:{g->copy_,g->open_,g->copyImage_,g->saveAs_,g->editAgain_})b->setVisible(b->isEnabled());g->link_->setVisible(data["uploadStatus"]!="local_only");
 if(data["superseded"].toBool()||data["remoteStatus"]=="superseded")VisualStyle::feedback(g->status_,"Superseded: this stored link may serve replacement content. Deletion still targets the original receipt.",true);else if(!g->sharingFeedback_.isEmpty())VisualStyle::feedback(g->status_,g->sharingFeedback_,g->sharingFeedbackError_);const auto previewPath=data["previewPath"].toString();
  if(g->shownItemId_!=data["id"].toString()||g->shownPreviewPath_!=previewPath||!QFileInfo(previewPath).isFile()){g->shownItemId_.clear();g->shownPreviewPath_.clear();g->preview_->setText("Preview unavailable");g->pendingPreviewPath_=previewPath;g->startPreview();}
  // History reloads the same selection whenever the resident reports a change;
  // only a different item restarts the wait.
  if(g->previewMissing(data)&&(!g->previewRequestTimer_->isActive()||g->previewCandidate_!=data["id"].toString())){g->previewCandidate_=data["id"].toString();g->previewRequestTimer_->start();}});
}
void HistoryWindow::updateActivity(){activeId_.clear();for(int row=0;row<model_->rowCount();++row){auto i=model_->index(row);if(!i.data(HistoryModel::ActiveRole).toBool())continue;const auto r=model_->record(row);activeId_=r["operationId"].toString();activeLabel_->setText(r["name"].toString()+" · "+HistoryModel::statusText(r));const int percent=i.data(HistoryModel::ProgressRole).toInt();progress_->setRange(0,percent<0?0:100);progress_->setValue(qMax(0,percent));break;}activeBanner_->setVisible(!activeId_.isEmpty());}
void HistoryWindow::operationAction(const QString& method){const auto rows=table_->selectionModel()->selectedRows();if(rows.size()!=1)return;const auto id=model_->record(rows.first().row())["operationId"].toString();if(id.isEmpty())return;session_.request(method,{{"id",id}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g){VisualStyle::feedback(g->status_,e?e.message:"Action accepted.",bool(e));g->refresh();}});}
void HistoryWindow::primaryAction(const QModelIndex& i){table_->selectionModel()->select(i,QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);table_->setCurrentIndex(i);const auto action=i.data(HistoryModel::PrimaryActionRole).toString();if(action=="Cancel")operationAction("operation.cancel");else if(action=="Retry upload")operationAction("operation.retry");else if(action=="Retry local save")operationAction("operation.retryLocalSave");else if(action=="Copy result")operationAction("operation.copy");else if(action=="Copy link"){const auto group=model_->record(i.row());if(group["records"].toArray().size()>1&&!group["operation"].toObject().isEmpty())operationAction("operation.copy");else {const auto files=group["records"].toArray();if(files.isEmpty())return;session_.request("history.copyLink",{{"id",files.first().toObject()["id"]}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g)VisualStyle::feedback(g->status_,e?e.message:"Stored link copied.",bool(e));});}}}
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
void HistoryWindow::showRowMenu(const QModelIndex& i,const QPoint& point){table_->selectionModel()->select(i,QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);table_->setCurrentIndex(i);const auto rowRecord=model_->record(i.row());QMenu menu(this);menu.addAction("Inspect files",this,[this]{setGridMode(false);});const bool operation=!model_->record(i.row())["operationId"].toString().isEmpty()&&!HistoryModel::storedNothing(rowRecord);auto* result=menu.addAction("Copy result",this,[this]{operationAction("operation.copy");});result->setEnabled(operation);auto* retry=menu.addAction("Retry upload",this,[this]{operationAction("operation.retry");});retry->setEnabled(operation);auto* cancel=menu.addAction("Cancel operation",this,[this]{operationAction("operation.cancel");});cancel->setEnabled(i.data(HistoryModel::ActiveRole).toBool());auto* retryLocal=menu.addAction("Retry local save",this,[this]{operationAction("operation.retryLocalSave");});retryLocal->setEnabled(operation);auto* edit=menu.addAction("Edit again",this,[this,rowRecord]{editAgain(rowRecord);});edit->setEnabled(editable(rowRecord));auto* diagnostics=menu.addAction("Save diagnostics for this upload",this,[this,rowRecord]{saveDiagnostics(rowRecord["operationId"].toString());});diagnostics->setEnabled(diagnosticsAvailable(i.row()));menu.addAction("Copy details",this,[this,rowRecord]{QJsonObject detail=rowRecord;if(currentDetail_["operationId"]==rowRecord["operationId"])detail["selectedFile"]=currentDetail_;QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(detail).toJson(QJsonDocument::Indented)));VisualStyle::feedback(status_,"Operation and receipt details copied.");});menu.addSeparator();auto* remove=menu.addAction("Remove from history");remove->setEnabled(false);remove->setToolTip("Removing one capture is not supported by the resident. Manage → Clear history records removes all local records.");menu.setToolTipsVisible(true);menu.exec(point);}
void HistoryWindow::updateSummary(){
 if(currentDetail_.isEmpty())return;
 const auto data=currentDetail_;auto escaped=[](const QString& s){return s.toHtmlEscaped();};const auto muted=RedesignTheme::textMuted().name();
 const QHash<QString,QString> uploads{{"uploaded","Uploaded"},{"local_only","Local capture"},{"pending","Waiting to upload"},{"uploading","Uploading"},{"failed","Upload failed"},{"unknown","Upload outcome unknown"},{"not_requested","No upload requested"}};
 const QHash<QString,QString> copies{{"saved","Saved locally"},{"disabled","No local copy requested"},{"failed","Local save failed"},{"pending","Waiting for local save"}};
 const QHash<QString,QString> sharing{{"shared","Shared link"},{"not_requested","No share link requested"},{"private","Private upload"},{"pending","Creating share link"},{"revoked","Share link revoked"},{"failed","Sharing failed"},{"unknown","Sharing outcome unknown"},{"changed","Share link changed"}};
 const QHash<QString,QString> remote{{"present","Remote file available"},{"not_requested","Remote deletion not requested"},{"removed","Deleted from destination"},{"already_missing","Remote file already missing"},{"unknown","Remote outcome unknown"},{"failed","Remote deletion failed"},{"pending","Waiting for remote deletion"},{"deleting","Deleting from destination"},{"changed","Remote file changed"},{"superseded","Replaced by a newer upload"},{"hidden_version_retained","Hidden; original version remains"}};
 const auto size=HistoryModel::fileSize(data["bytes"].toInteger())+(pixelDimensions_.isEmpty()?QString{}:" · "+pixelDimensions_);const auto local=data["local"].toObject();
 // Label / value rows: type, output, created, size, file, then the recorded outcomes.
 const auto mime=data["mime"].toString();const auto category=data["category"].toString();
 const QString type=category=="images"?mime.mid(6).toUpper()+" image":category=="videos"?mime.mid(6).toUpper()+" recording":category=="text"?QString("Text"):QString("File")+(mime.isEmpty()?QString{}:" ("+mime+")");
 auto created=QDateTime::fromString(data["created"].toString(),Qt::ISODateWithMs);if(!created.isValid())created=QDateTime::fromString(data["created"].toString(),Qt::ISODate);
 const auto when=created.isValid()?QLocale().toString(created.toLocalTime(),QLocale::ShortFormat):QString{};
 QString rows;auto row=[&](const QString& label,const QString& value){if(!value.isEmpty())rows+="<tr><td width=\"34%\" valign=\"top\"><font color=\""+muted+"\">"+escaped(label)+"</font></td><td valign=\"top\">"+escaped(value)+"</td></tr>";};
 row("Type",type);row("Output",destinationLabel(data));row("Created",when);row("Size",size);row("File",data["name"].toString());
 const auto html="<table width=\"100%\" cellspacing=\"0\" cellpadding=\"3\">"+rows+"</table><p><font color=\""+muted+"\">"+escaped(uploads.value(data["uploadStatus"].toString(),"Upload not recorded"))+" · "+escaped(copies.value(local["status"].toString(),"Local copy not recorded"))+"<br>"+escaped(sharing.value(data["shareStatus"].toString(),"Sharing not recorded"))+" · "+escaped(remote.value(data["remoteStatus"].toString(),"Remote outcome not recorded"))+"<br>"+escaped(mime)+"</font></p>";
 summary_->setText(html);
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
  if(g->isVisible()&&(seq==g->selectionRequest_||(!itemId.isEmpty()&&itemId==g->selectedItemId_))){if(!image->isNull()){auto pixmap=QPixmap::fromImage(*image).scaled(g->preview_->contentsRect().size(),Qt::KeepAspectRatio,Qt::SmoothTransformation);const auto length=HistoryModel::duration(image->text(HistoryModel::durationKey).toLongLong());if(isVideo){QPainter painter(&pixmap);paintVideoBadge(painter,QRect(QPoint(0,0),pixmap.deviceIndependentSize().toSize()),length);if(!length.isEmpty()){g->pixelDimensions_=length;g->updateSummary();}}g->preview_->setPixmap(pixmap);g->shownItemId_=itemId;g->shownPreviewPath_=path;}if(metadata["width"].toInt()>0){g->pixelDimensions_=QString("%1×%2").arg(metadata["width"].toInt()).arg(metadata["height"].toInt());g->updateSummary();}}g->startPreview();});
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
void HistoryWindow::hideEvent(QHideEvent* event){releasePreview();RetainedWindow::hideEvent(event);}
void HistoryWindow::showEvent(QShowEvent* event){RetainedWindow::showEvent(event);selectionChanged();table_->viewport()->update();}
void HistoryWindow::exportLocal(bool image){
 if(image){const auto record=selectedRecord();if(record.isEmpty())return;session_.request("history.copyImage",{{"id",record["id"]}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g)VisualStyle::feedback(g->status_,e?e.message:"Original image copied.",bool(e));});return;}

 const auto local=currentDetail_["local"].toObject();const auto path=local["path"].toString();if(local["status"]!="saved"||!QFileInfo(path).isFile())return;
 chooser_->open({FileDialog::Mode::Save,"Save original file as",{},currentDetail_["name"].toString(),{}},[this,path](const QString& destination){
  if(destination.isEmpty())return;if(QFileInfo(destination).absoluteFilePath()==QFileInfo(path).absoluteFilePath()){VisualStyle::feedback(status_,"This is already the retained local file.");return;}
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
 if(action=="revoke"&&!Graphite::confirm(this,"Revoke share link","Revoke this file’s public share link?\nThe uploaded file remains. Existing member access and downloaded copies remain.","Revoke link",true))return;
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
void HistoryWindow::prepareDelete(){auto ids=selectedIds();ids.removeIf([](const QString& id){return id.startsWith("operation:");});if(table_->selectionModel()->selectedRows().size()==1&&!selectedRecord().isEmpty())ids={selectedRecord()["id"].toString()};if(ids.isEmpty())return;session_.request("delete.prepare",{{"ids",QJsonArray::fromStringList(ids)}},this,[g=QPointer<HistoryWindow>(this),ids](Error e,QJsonObject preview){if(!g)return;if(e){VisualStyle::feedback(g->status_,e.code+": "+e.message,true);return;}QString text="Delete these original recorded remote targets?\nLocal copies and history records remain. Browser/CDN caches may still serve bytes.\n";for(auto v:preview["targets"].toArray()){auto target=v.toObject();auto receipt=target["receipt"].toObject();text+="\nHistory ID: "+target["historyId"].toString()+"\nOriginal destination / exact target: "+QString::fromUtf8(QJsonDocument(receipt).toJson(QJsonDocument::Compact))+"\nEffect: "+target["effect"].toString()+"\n";}if(!Graphite::confirm(g,"Delete from destination",text,"Delete from destination",true))return;g->deleteAccepting_=true;emit g->viewStateChanged();g->session_.request("delete.confirm",{{"ids",QJsonArray::fromStringList(ids)},{"acknowledgement",preview["acknowledgement"]}},g,[g](Error failure,QJsonObject data){if(!g)return;g->deleteAccepting_=false;emit g->viewStateChanged();VisualStyle::feedback(g->status_,failure?failure.code+": "+failure.message:"Remote deletion accepted; individual outcomes will update.",bool(failure));if(!failure){g->deletionId_=data["deleteOperationId"].toString();emit g->viewStateChanged();g->deletionTimer_->start();}});});}
void HistoryWindow::pollDeletion(){if(deletionPollActive_)return;if(deletionId_.isEmpty()){deletionTimer_->stop();return;}deletionPollActive_=true;const auto id=deletionId_;session_.request("delete.inspect",{{"id",id}},this,[g=QPointer<HistoryWindow>(this),id](Error e,QJsonObject data){if(!g)return;g->deletionPollActive_=false;if(g->deletionId_!=id)return;if(e){VisualStyle::feedback(g->status_,e.message,true);g->deletionTimer_->stop();return;}QString status="Remote deletion: "+data["state"].toString();for(auto v:data["items"].toArray()){auto item=v.toObject();status+="\n"+item["status"].toString()+" "+item["error"].toObject()["message"].toString();}g->status_->setText(status);if(data["state"]!="running"&&data["state"]!="accepted"){g->deletionTimer_->stop();g->refresh();}});}
QJsonObject HistoryWindow::saveViewState()const{
 if(deleteAccepting_||sharingAccepting_)return {{"stateError","request_awaiting_acceptance"}};
 const auto& q=model_->query();QJsonObject query{{"limit",q.limit},{"offset",q.offset},{"search",q.search},{"category",q.category},{"mime",q.mime},{"profileId",q.profileId},{"uploadStatus",q.uploadStatus},{"localStatus",q.localStatus},{"remoteStatus",q.remoteStatus},{"from",q.from},{"to",q.to}};QJsonObject filters{{"search",findChild<QLineEdit*>("historySearch")->text()}};for(const auto& key:QStringList{"from","to","mime","profileId","uploadStatus","localStatus","remoteStatus"})filters[key]=findChild<QLineEdit*>("historyFilter"+key)->text();
 return {{"query",query},{"filters",filters},{"selectedIds",QJsonArray::fromStringList(restoringSelection_?restoreSelection_:selectedIds())},{"selectedItemId",selectedItemId_},{"deletionId",deletionId_},{"sharingId",sharingId_},{"filtersExpanded",findChild<QToolButton*>("historyFiltersExpanded")->isChecked()},{"managementExpanded",findChild<QToolButton*>("historyManagementExpanded")->isChecked()},{"receiptExpanded",findChild<QToolButton*>("historyReceiptExpanded")->isChecked()},{"gridMode",grid_->isChecked()},{"showCancelled",showCancelled_->isChecked()},{"geometry",QJsonArray{x(),y(),width(),height()}}};
}
bool HistoryWindow::restoreViewState(const QJsonObject& state){
 if(validateUiState({{"schemaVersion",1},{"history",state}}))return false;
 const QSignalBlocker stateSignals(this);searchTimer_->stop();const auto box=state["geometry"].toArray();setGeometry(box[0].toInt(),box[1].toInt(),box[2].toInt(),box[3].toInt());const auto f=state["filters"].toObject();findChild<QLineEdit*>("historySearch")->setText(f["search"].toString());for(const auto& key:QStringList{"from","to","mime","profileId","uploadStatus","localStatus","remoteStatus"})findChild<QLineEdit*>("historyFilter"+key)->setText(f[key].toString());findChild<QToolButton*>("historyFiltersExpanded")->setChecked(state["filtersExpanded"].toBool());findChild<QToolButton*>("historyManagementExpanded")->setChecked(state["managementExpanded"].toBool());findChild<QToolButton*>("historyReceiptExpanded")->setChecked(state["receiptExpanded"].toBool());setGridMode(state["gridMode"].toBool());showCancelled_->setChecked(state["showCancelled"].toBool());restoreSelection_.clear();for(auto id:state["selectedIds"].toArray())restoreSelection_.append(id.toString());selectedItemId_=state["selectedItemId"].toString();restoringSelection_=true;deletionId_=state["deletionId"].toString();if(!deletionId_.isEmpty()){deletionTimer_->start();pollDeletion();}sharingId_=state["sharingId"].toString();sharingStopped_=false;sharingFeedback_.clear();sharingTimer_->stop();if(!sharingId_.isEmpty()){sharingTimer_->start();pollSharing();}
 const auto data=state["query"].toObject();UiHistoryQuery q;q.limit=data["limit"].toInt(50);q.offset=data["offset"].toInt();q.search=data["search"].toString();q.category=data["category"].toString();q.mime=data["mime"].toString();q.profileId=data["profileId"].toString();q.uploadStatus=data["uploadStatus"].toString();q.localStatus=data["localStatus"].toString();q.remoteStatus=data["remoteStatus"].toString();q.from=data["from"].toString();q.to=data["to"].toString();{restoringCategory_=true;int row=0;for(int i=0;i<categories_->count();++i)if(categories_->item(i)->data(Qt::UserRole).toString()==q.category)row=i;categories_->setCurrentRow(row);restoringCategory_=false;const QSignalBlocker block(failedOnly_);failedOnly_->setChecked(f["uploadStatus"].toString()=="failed");}model_->setQuery(q);return true;
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
