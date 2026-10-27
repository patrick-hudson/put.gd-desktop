#include "SettingsWindow.h"
#include "MainWindow.h"
#include "storage/proton/ProtonProvider.h"
#include "storage/s3/PublicUrl.h"
#include "storage/b2/B2Common.h"
#include "platform/HostOpen.h"
#include <QUrl>
#include "ViewState.h"
#include <QTimer>
#include <QSignalBlocker>
#include "ShortcutEditor.h"
#include "ByteSize.h"
#include <QProgressBar>
#include "VisualStyle.h"
#include "Graphite.h"
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
#include "RedesignTheme.h"
#include <QFileInfo>
#include <QDir>
#include <QLocale>
#include <QDateTime>
#include <QDialog>
#include <QContextMenuEvent>
#include <QToolTip>
#include <QHelpEvent>
#include <QStyleOptionFrame>
#include <QScreen>
#include <QWindow>
#include <QGuiApplication>
namespace putgd {
namespace {
// Give feedback the full row once its recovery actions would squeeze it.
class SettingsStatusRow final:public Graphite::Surface {
public:
 explicit SettingsStatusRow(QWidget* message):Graphite::Surface("bg",Qt::TopEdge){line_=new QBoxLayout(QBoxLayout::LeftToRight,this);line_->setContentsMargins(28,6,28,8);line_->setSpacing(12);line_->addWidget(message,1);buttons_=new QWidget(this);actions_=new QHBoxLayout(buttons_);actions_->setContentsMargins(0,0,0,0);actions_->setSpacing(8);line_->addWidget(buttons_,0,Qt::AlignTop|Qt::AlignRight);}
 QHBoxLayout* actions()const{return actions_;}
protected:
 bool event(QEvent* event)override{if(event->type()==QEvent::Resize||event->type()==QEvent::LayoutRequest){int visible=0;for(int i=0;i<actions_->count();++i)if(auto* widget=actions_->itemAt(i)->widget();widget&&!widget->isHidden())++visible;const bool stack=width()-56-buttons_->sizeHint().width()-12<Graphite::scaled(visible>1?480:300);const auto direction=stack?QBoxLayout::TopToBottom:QBoxLayout::LeftToRight;if(line_->direction()!=direction)line_->setDirection(direction);setProperty("stacked",stack);}return Graphite::Surface::event(event);}
private:QBoxLayout* line_;QWidget* buttons_;QHBoxLayout* actions_;
};
class CompactPageHeading final:public QObject {
public:
 CompactPageHeading(QScrollArea* scroll,QWidget* heading,QWidget* description):QObject(scroll),scroll_(scroll),heading_(heading),description_(description){scroll->viewport()->installEventFilter(this);apply();}
protected:
 bool eventFilter(QObject*,QEvent* event)override{if(event->type()==QEvent::Resize||event->type()==QEvent::Show)apply();return false;}
private:
 void apply(){const bool compact=scroll_->viewport()->height()<Graphite::scaled(520);description_->setVisible(!compact);heading_->setFont(Graphite::font(compact?Graphite::Type::section:Graphite::Type::page,true));heading_->setProperty("compactHeading",compact);}
 QScrollArea* scroll_;QWidget* heading_;QWidget* description_;
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
QString actionLabel(const QString& action){static const QHash<QString,QString> labels{{"region","Capture region"},{"window","Capture window"},{"desktop","Capture desktop"},{"clipboard","Upload clipboard"},{"edit","Capture region & edit"},{"record","Record screen"}};return labels.value(action);}
// Sharing and link copying live together under the default capture output.
QString sharingLabel(const QString& mode){return mode=="share"?QString("Create a share link"):mode=="private"?QString("Keep private"):QString("Destination default");}
QString storageClassLabel(const QString& value){
 static const QHash<QString,QString> labels{{"STANDARD","Standard storage"},{"REDUCED_REDUNDANCY","Reduced redundancy storage"},{"STANDARD_IA","Standard infrequent access"},{"ONEZONE_IA","One Zone infrequent access"},{"INTELLIGENT_TIERING","Intelligent-Tiering storage"},{"GLACIER","Glacier Flexible Retrieval"},{"DEEP_ARCHIVE","Glacier Deep Archive"},{"GLACIER_IR","Glacier Instant Retrieval"},{"EXPRESS_ONEZONE","S3 Express One Zone"},{"OUTPOSTS","S3 Outposts storage"},{"SNOW","S3 Snow storage"},{"FSX_OPENZFS","FSx OpenZFS storage"}};return labels.value(value,value);
}
QComboBox* choices(QWidget* parent,const QString& name,const QStringList& values){auto* c=VisualStyle::choiceBox(parent);c->setObjectName(name);c->setAccessibleName(name);c->addItems(values);return c;}
QLineEdit* input(QWidget* parent,const QString& name){auto* e=new QLineEdit(parent);e->setObjectName(name);e->setAccessibleName(name);return e;}
// Backblaze B2 settings edited through the S3 panel's shared rows (#282).
const QStringList& b2SharedKeys(){static const QStringList keys{"bucket","objectPrefix","publicUrlBase","connectTimeoutMs","requestTimeoutMs"};return keys;}
QJsonObject s3Profile(const QString& id){
 return {{"id",id},{"label","S3 destination"},{"provider","s3"},
  {"settings",QJsonObject{{"bucket",""},{"region","us-east-1"},{"endpoint","https://s3.us-east-1.amazonaws.com"},{"objectPrefix",""},{"storageClass","STANDARD"},{"aclMode","omit"},{"publicUrlBase",QJsonValue::Null},{"addressing","auto"},{"connectTimeoutMs",5000},{"requestTimeoutMs",60000},{"lowSpeedBytesPerSecond",1},{"lowSpeedTimeSeconds",30}}},
  {"credentials",QJsonObject{{"mode","keychain"},{"reference","putgd/"+id}}}};
}
// Drag and drop only proposes a move; the window reorders its draft and rebuilds
// the rows, so Qt never moves or removes items itself.
// User names elide; the status wraps independently and determines row height.
class DestinationRowDelegate final:public QStyledItemDelegate {
public:
 using QStyledItemDelegate::QStyledItemDelegate;
 QStyleOptionViewItem shown(const QStyleOptionViewItem& base,const QModelIndex& index)const{auto option=base;initStyleOption(&option,index);return option;}
 QSize sizeHint(const QStyleOptionViewItem& option,const QModelIndex& index)const override{const auto lines=index.data().toString().split('\n');const int width=qMax(24,(option.widget?option.widget->width():option.rect.width())-Graphite::scaled(28)-36);const QFontMetrics body(Graphite::font(Graphite::Type::body,true)),meta(Graphite::font(Graphite::Type::metadata));const auto status=meta.boundingRect(QRect(0,0,width,10000),Qt::TextWordWrap,lines.value(1));return {0,qMax(Graphite::scaled(76),body.height()+status.height()+32)};}
 void paint(QPainter* p,const QStyleOptionViewItem& base,const QModelIndex& index)const override{
  auto option=shown(base,index);const auto lines=option.text.split(QChar::LineSeparator);const auto& t=Theme::current();
  const bool selected=option.state&QStyle::State_Selected,hover=option.state&QStyle::State_MouseOver,enabled=option.state&QStyle::State_Enabled;
  p->save();p->setRenderHint(QPainter::Antialiasing);const QRectF box=QRectF(option.rect).adjusted(2,2,-2,-2);
  if(selected||hover){p->setPen(Qt::NoPen);p->setBrush(selected?t.selected:Graphite::mix(t.surface,t.surfaceRaised,0.7));p->drawRoundedRect(box,7,7);}
  if(selected)p->fillRect(QRectF(box.left(),box.top()+8,2,box.height()-16),t.accent);
  if(selected&&option.widget&&Graphite::keyboardFocus(option.widget)){p->setPen(QPen(t.accent,1));p->setBrush(Qt::NoBrush);p->drawRoundedRect(box.adjusted(0.5,0.5,-0.5,-0.5),7,7);}
  qreal x=box.left()+12;if(!option.icon.isNull()){option.icon.paint(p,QRect(int(x),int(box.center().y())-Graphite::scaled(9),Graphite::scaled(18),Graphite::scaled(18)),Qt::AlignCenter,!enabled?QIcon::Disabled:selected?QIcon::Selected:QIcon::Normal);x+=Graphite::scaled(28);}
  const QRectF text(x,box.top(),box.right()-10-x,box.height());const QFontMetrics body(Graphite::font(Graphite::Type::body,selected));
  Graphite::paintText(*p,QRectF(text.left(),box.top()+12,text.width(),body.height()),lines.value(0),enabled?(selected?t.text:t.textSecondary):t.textDisabled,Graphite::font(Graphite::Type::body,selected),Qt::AlignLeft|Qt::AlignVCenter,Qt::ElideRight);
  if(lines.size()>1){p->setFont(Graphite::font(Graphite::Type::metadata));p->setPen(enabled?t.textMuted:t.textDisabled);p->drawText(QRectF(text.left(),box.top()+16+body.height(),text.width(),box.height()-28-body.height()),Qt::AlignLeft|Qt::AlignTop|Qt::TextWordWrap,lines.value(1));}
  p->restore();
 }
protected:
 void initStyleOption(QStyleOptionViewItem* option,const QModelIndex& index)const override{
  QStyledItemDelegate::initStyleOption(option,index);
  // displayText() has already turned the label/state newline into a line separator.
  auto lines=option->text.split(QChar::LineSeparator);if(lines.size()<2)return;
  option->features&=~QStyleOptionViewItem::WrapText;
  const int width=qMax(16,option->rect.width()-Graphite::scaled(28)-36);
  lines[0]=QFontMetrics(Graphite::font(Graphite::Type::body,true)).elidedText(lines[0],Qt::ElideRight,width);
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
 // The displayed name is elided; the complete status wraps beneath it.
 QStringList shownLines(int row)const{
  QStyleOptionViewItem option;initViewItemOption(&option);option.rect=visualItemRect(item(row));
  return static_cast<DestinationRowDelegate*>(itemDelegate())->shown(option,model()->index(row,0)).text.split(QChar::LineSeparator);
 }
 // A slim, slightly translucent label beside and below the pointer, so the drop
 // line within half a row of it stays visible (#133).
 static QPixmap dragImage(const QListWidgetItem& item,const QFont& font,qreal ratio){
  const QFontMetrics metrics(font);const auto label=item.text().section('\n',0,0);
  const int height=metrics.height()+16,width=qMin(220,8+16+8+metrics.horizontalAdvance(label)+12);
  QPixmap image(QSize(width,height)*ratio);image.setDevicePixelRatio(ratio);image.fill(Qt::transparent);
  QPainter painter(&image);painter.setRenderHint(QPainter::Antialiasing);painter.setOpacity(0.86);
  painter.setPen(QPen(RedesignTheme::accent(),1));painter.setBrush(RedesignTheme::surfaceRaised());painter.drawRoundedRect(QRectF(0.5,0.5,width-1,height-1),7,7);
  item.icon().paint(&painter,QRect(8,(height-16)/2,16,16));
  painter.setFont(font);painter.setPen(RedesignTheme::text());
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
  QPainter painter(viewport());painter.setRenderHint(QPainter::Antialiasing);painter.setPen(QPen(RedesignTheme::accent(),2,Qt::SolidLine,Qt::RoundCap));
  painter.drawLine(rect.left()+6,y,rect.right()-6,y);painter.setBrush(RedesignTheme::accent());painter.drawEllipse(QPoint(rect.left()+5,y),3,3);
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
// width is the name's, so help sits beside the title instead of far right.
class DestinationTitle final:public QLineEdit {
public:
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
  if(event->type()==QEvent::ToolTip&&!hasFocus()&&elided()){QToolTip::showText(static_cast<QHelpEvent*>(event)->globalPos(),text(),this);return true;}
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
 static const QHash<QString,QString> actions{{"region","Region capture"},{"window","Window capture"},{"desktop","Desktop capture"},{"clipboard","Clipboard upload"},{"edit","Edited capture"},{"record","Screen recording"},{"file","File upload"}};
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
  setObjectName("diagnosticsPackageDialog");setWindowTitle("Create diagnostics package");setAccessibleName("Create diagnostics package");setModal(true);setMinimumWidth(560);
  // The body scrolls inside the screen's height; Cancel and Save stay pinned below it.
  auto* root=new QVBoxLayout(this);root->setContentsMargins(0,0,0,0);root->setSpacing(0);
  scroll_=new QScrollArea(this);scroll_->setObjectName("diagnosticsPackageScroll");scroll_->setWidgetResizable(true);scroll_->setFrameShape(QFrame::NoFrame);scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);root->addWidget(scroll_,1);
  auto* body=new QWidget;body->setObjectName("diagnosticsPackageBody");scroll_->setWidget(body);
  auto* layout=new QVBoxLayout(body);layout->setContentsMargins(24,20,24,14);layout->setSpacing(14);
  auto* heading=new QLabel("Create diagnostics package",this);VisualStyle::role(heading,"sectionTitle");layout->addWidget(heading);
  auto* intro=new QLabel("A .zip a maintainer can use to work out what went wrong. It is always saved on this computer; nothing is uploaded unless you choose to below.",this);intro->setWordWrap(true);VisualStyle::role(intro,"muted");layout->addWidget(intro);
  auto* form=new QFormLayout;VisualStyle::styleForm(form);form->setVerticalSpacing(12);layout->addLayout(form);
  auto label=[this](const char* name){auto* l=new QLabel(this);l->setObjectName(name);l->setWordWrap(true);l->setTextFormat(Qt::PlainText);l->setTextInteractionFlags(Qt::TextSelectableByMouse);return l;};
  scope_=label("packageScope");form->addRow("Covers",scope_);
  contents_=label("packageContents");contents_->setTextFormat(Qt::RichText);
  operations_=label("packageOperations");
  auto* never=new QLabel("Credentials, tokens, signed links, file contents, screenshots and clipboard text are never included.",this);never->setWordWrap(true);VisualStyle::role(never,"muted");form->addRow("Never",never);
  auto* names=new QWidget(this);auto* namesLayout=new QVBoxLayout(names);namesLayout->setContentsMargins(0,0,0,0);namesLayout->setSpacing(4);
  includeNames_=new QCheckBox("Include file names",this);includeNames_->setObjectName("includeFileNames");includeNames_->setAccessibleName("Include file names");namesLayout->addWidget(includeNames_);
  redaction_=label("packageRedaction");VisualStyle::role(redaction_,"muted");namesLayout->addWidget(redaction_);form->addRow("Names",names);
  auto* where=new QWidget(this);auto* whereLayout=new QHBoxLayout(where);whereLayout->setContentsMargins(0,0,0,0);whereLayout->setSpacing(8);
  folder_=label("packageFolder");VisualStyle::role(folder_,"technical");whereLayout->addWidget(folder_,1);
  auto* choose=new QPushButton("Choose folder…",this);choose->setObjectName("choosePackageFolder");whereLayout->addWidget(choose);
  useDefault_=new QPushButton("Use default",this);useDefault_->setObjectName("defaultPackageFolder");useDefault_->setToolTip("Save to <archive root>/diagnostics, where put.gd keeps the newest 10 packages for 30 days.");whereLayout->addWidget(useDefault_);
  form->addRow("Save to",where);
  // A chooser that could not open says so beside Save to, with Try again; the folder stays.
  chooserFailure_=new QWidget(this);chooserFailure_->setObjectName("packageFolderChooserFailure");auto* failureLayout=new QHBoxLayout(chooserFailure_);failureLayout->setContentsMargins(0,0,0,0);failureLayout->setSpacing(8);
  chooserError_=label("packageFolderChooserError");VisualStyle::role(chooserError_,"error");failureLayout->addWidget(chooserError_,1);
  auto* retry=new QPushButton("Try again",this);retry->setObjectName("retryPackageFolderChooser");failureLayout->addWidget(retry,0,Qt::AlignTop);form->addRow(QString(),chooserFailure_);chooserFailure_->hide();
  connect(retry,&QPushButton::clicked,this,[this]{chooseFolder();});
  // Also upload and copy a link (#123): offered only once diagnostics.uploadTarget
  // says the destination works; until then, or instead, a line says why not.
  auto* extras=new QWidget(this);extras->setObjectName("diagnosticsPackageExtras");auto* extrasLayout=new QVBoxLayout(extras);extrasLayout->setContentsMargins(0,0,0,0);extrasLayout->setSpacing(4);layout->addWidget(extras);
  upload_=new QCheckBox("Also upload and copy a link",this);upload_->setObjectName("uploadPackage");upload_->setAccessibleName("Also upload and copy a link");upload_->hide();extrasLayout->addWidget(upload_);
  uploadNote_=label("packageUploadNote");VisualStyle::role(uploadNote_,"muted");extrasLayout->addWidget(uploadNote_);
  auto* inventory=new QToolButton(this);inventory->setObjectName("packageContentsToggle");inventory->setText("Package contents");Graphite::disclosure(inventory);layout->addWidget(inventory);layout->addWidget(contents_);layout->addWidget(operations_);contents_->hide();operations_->hide();connect(inventory,&QToolButton::toggled,contents_,&QWidget::setVisible);connect(inventory,&QToolButton::toggled,operations_,&QWidget::setVisible);connect(inventory,&QToolButton::toggled,this,[this]{fit();});
  result_=label("packageResult");VisualStyle::role(result_,"status");result_->hide();layout->addWidget(result_);
  rule_=Graphite::rule(this);root->addWidget(rule_);
  auto* buttons=footer_=new QHBoxLayout;buttons->setContentsMargins(24,12,24,18);buttons->setSpacing(8);show_=new QPushButton("Show in folder",this);show_->setObjectName("showPackageInFolder");show_->hide();buttons->addWidget(show_);buttons->addStretch();
  close_=new QPushButton("Cancel",this);close_->setObjectName("closePackage");buttons->addWidget(close_);
  save_=new QPushButton("Save package",this);save_->setObjectName("savePackage");VisualStyle::role(save_,"primary");save_->setDefault(true);buttons->addWidget(save_);root->addLayout(buttons);
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
   if(error){save_->setText(saveLabel());save_->setEnabled(true);VisualStyle::feedback(result_,error.message,true);result_->show();}
   else {
    saved_=saved;save_->setText("Saved");close_->setText("Close");show_->show();
    auto text="Saved "+QFileInfo(saved["path"].toString()).fileName()+" ("+ByteSize::summary(saved["bytes"].toInteger())+") in "+QDir::toNativeSeparators(QFileInfo(saved["path"].toString()).absolutePath())+".";
    // The local copy is kept whatever happens to the upload.
    const auto uploadError=saved["uploadError"].toObject();
    if(uploadError["code"]=="diagnostics_link_unavailable")text+="\nUploaded to "+saved["upload"].toObject()["label"].toString()+", but this destination gives it no link.";
    // The diagnostics_* messages already say the package wasn't uploaded; a provider's doesn't.
    else if(!uploadError.isEmpty())text+="\n"+(uploadError["code"].toString().startsWith("diagnostics_")?QString{}:QString("Not uploaded: "))+uploadError["message"].toString()+(uploadError["message"].toString().endsWith('.')?"":".")+" The saved package is kept.";
    else if(saved.contains("link"))text+="\nUploaded to "+saved["upload"].toObject()["label"].toString()+(saved["linkCopied"].toBool()?". Link copied: ":". Link (not copied): ")+saved["link"].toString();
    if(uploadError.isEmpty()||uploadError["code"]=="diagnostics_link_unavailable")VisualStyle::success(result_,text);else VisualStyle::feedback(result_,text,true);
    result_->setTextInteractionFlags(Qt::TextSelectableByMouse);result_->show();
   }
   fit();if(reply)reply(error,state());
  });
 }
 void reveal(Reply reply){
  if(saved_.isEmpty()){if(reply)reply({"diagnostics_not_saved","Save the package first.",{}},state());return;}
  session_.request("diagnostics.reveal",{{"path",saved_["path"]}},this,[this,reply](Error error,QJsonObject result){if(error){VisualStyle::feedback(result_,error.message,true);result_->show();}if(reply)reply(error,result);});
 }
 // Grow with the wrapped body up to the screen's height; past that the body scrolls.
 void fit(){
  auto* body=scroll_->widget();body->layout()->activate();const int bodyWidth=scroll_->viewport()->width()>0?scroll_->viewport()->width():width();
  const int bodyHeight=body->layout()->hasHeightForWidth()?body->layout()->totalHeightForWidth(bodyWidth):body->layout()->totalSizeHint().height();
  body->setMinimumHeight(bodyHeight);const int footer=footer_->totalSizeHint().height()+1;
  const auto* screen=windowHandle()&&windowHandle()->screen()?windowHandle()->screen():parentWidget()&&parentWidget()->screen()?parentWidget()->screen():QGuiApplication::primaryScreen();
  // Room for the desktop's title bar and panels around the dialog.
  const int cap=screen?qMax(320,screen->availableGeometry().height()-64):QWIDGETSIZE_MAX;
  const int wanted=qMin(cap,bodyHeight+footer);setMaximumHeight(cap);setMinimumHeight(qMin(wanted,Graphite::scaled(320)));
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
   {"config.redacted.json","settings without credentials"},{"coredumps.json","system crash reports, stack traces only"},{"operations.json",(count==1?QString("1 upload"):QString("%1 uploads").arg(count))},{"logs/putgd.jsonl",QString("%1 log records").arg(records)+(preview_["droppedLogRecords"].toInt()?QString(", %1 older ones left out to stay under 20 MiB").arg(preview_["droppedLogRecords"].toInt()):QString{})}};
  for(const auto& name:preview_["files"].toArray())files.append(QString(R"(<span style="font-family: '%1'; font-size: %2px">%3</span>: %4)").arg(Graphite::monoFamily().toHtmlEscaped()).arg(Graphite::scaled(Graphite::Type::metadata)).arg(name.toString().toHtmlEscaped(),about.value(name.toString()).toHtmlEscaped()));
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
QLabel* plain(QLabel* label){label->setProperty("errorCode",{});label->setToolTip({});label->setAccessibleDescription({});return label;}
// Shows a status row only while its message or one of its actions is shown,
// so an empty row leaves no gap in the page's spacing.
class VisibleWithParts final:public QObject {
public:
 VisibleWithParts(QWidget* row,QList<QWidget*> parts):QObject(row),row_(row),parts_(std::move(parts)){for(auto* part:parts_)part->installEventFilter(this);update();}
protected:
 bool eventFilter(QObject* watched,QEvent* event)override{if(event->type()==QEvent::ShowToParent||event->type()==QEvent::HideToParent)update();return QObject::eventFilter(watched,event);}
private:
 void update(){bool shown=false;for(auto* part:parts_)shown|=part->isVisibleTo(row_);row_->setVisible(shown);}
 QWidget* row_;QList<QWidget*> parts_;
};
QString settingHelp(const QString& path,const QString& objectName){
 static const QHash<QString,QString> s3Help{
  {"endpoint","The HTTPS address of the S3 service, without the bucket name or object prefix. It starts with the standard AWS endpoint for the selected region. Use your service's endpoint for compatible storage. A custom value is preserved when you change the region."},
  {"storageClass","The S3 storage class requested for new uploads. STANDARD is the default. Other classes can have different access, retrieval and storage costs; use a value supported by your destination. This setting doesn't change the class of existing objects."},
  {"publicUrlBase","An optional HTTPS base URL for copied direct links, such as your own CDN or custom domain. The object path is appended to this URL. Leave it empty to use the destination's direct URL. A custom domain doesn't make private objects public or configure DNS or a bucket policy."},
  {"connectTimeoutMs","The connection timeout shown in seconds, with millisecond precision. The default is 5 seconds. It limits how long a storage request waits to establish a connection; it is separate from the overall request timeout below."},
  {"requestTimeoutMs","The request timeout shown in seconds, with millisecond precision. The default is 60 seconds. It controls the storage client's request timeout. Allow enough time for your connection and file sizes; a timed-out upload may still need reconciliation before its remote result is known."},
  {"lowSpeedBytesPerSecond","The minimum sustained transfer rate in bytes per second. The default is 1 byte/s. A transfer below this rate for the low-speed window is considered stalled. This is a stall threshold, not an upload speed limit or a file-size limit."},
  {"lowSpeedTimeSeconds","The number of seconds a transfer may remain below the minimum transfer rate before it is considered stalled. The default is 30 seconds. It works with Minimum transfer and doesn't cap an upload's total duration."}};
 static const QHash<QString,QString> numberHelp{
  {"pendingImageBytes","Maximum total decoded image bytes retained by jobs waiting for or undergoing encoding. The default is 64 MiB. An acquisition that can't fit fails with image_memory_limit; it isn't silently deferred or changed. The limit is released after disk persistence or queued cancellation. This bounds source payloads, not total app RSS, compositor frames, encoding workspace or the current native clipboard representation."},
  {"maxFiles","The maximum number of files accepted from one clipboard action. The default is 100. A batch above this limit is rejected before uploading its files. This is a per-action limit and doesn't restrict the total number of history entries."},
  {"pendingOperations","The maximum number of queued operations waiting for processing, in addition to those currently running. The default is 8. When the queue is full, a new action is rejected rather than accumulating more pending work."},
  {"concurrentUploads","How many files upload at the same time, across every capture and batch. The default is 3. A new capture starts while a large or slow upload continues, and a large file uses one upload however long it takes. Each S3 upload runs in its own small worker process (about 40 MB); Proton Drive uploads one file at a time per account whatever this is set to. Higher values help many small files more than one large one."},
  {"maxFileBytes","The largest individual uploaded file, in GiB. Auto, the default, sets no size limit of put.gd's own: uploads stream from disk, so memory doesn't grow with file size. Whatever this is set to, a file larger than the destination accepts (5 TiB for S3) or than the free disk space allows is rejected before uploading; put.gd always keeps 5% of the disk, up to 4 GiB and at least 1 GiB, free for the system. No limit (an empty field) also sets none. Fractional GiB values are accepted when they represent a whole number of bytes."},
  {"maxBatchBytes","The maximum combined size of one clipboard file batch, in GiB. Auto, the default, and No limit (an empty field) set none of put.gd's own; free disk space still bounds every batch. This applies to the whole batch in addition to the individual file-size limit."},
  {"pendingArtifactBytes","The maximum acquired content waiting in the pending artifact store on disk, in GiB. Auto, the default, and No limit (an empty field) set none of put.gd's own: free disk space is the bound, keeping 5% of the disk (up to 4 GiB, at least 1 GiB) free for the system. New uploads are rejected when they would exceed this limit. This is a disk-content ceiling, not memory allocated at startup."},
  {"clipboardTextBytes","The largest text payload accepted from the clipboard, in MiB. The default is 16 MiB. Larger text is rejected before uploading. One MiB equals 1,048,576 bytes; this limit doesn't apply to ordinary file uploads."},
  {"imagePixels","The largest image, shown in megapixels (one million pixels), calculated as width × height. The default is 100 megapixels. This protects against oversized decoded images independently of their encoded file size."},
  {"imageBytes","The maximum accepted image payload size, in MiB. The default is 512 MiB. It works alongside the image-pixel limit; an image must satisfy both limits. One MiB equals 1,048,576 bytes. This is a ceiling, not a persistent allocation."},
  {"failedArtifactDays","How many days failed or interrupted local artifacts are kept for explicit retry before cleanup may remove them. The default is 7 days. Set 0 to disable age-based removal of failed artifacts. This controls local retry content and doesn't delete remote objects."},
  {"recentActivityLimit","The maximum number of recent operation snapshots available to History. The default is 100. This bounds the activity list; it doesn't set the complete upload-history retention period below."},
  {"previewCacheBytes","The maximum total size of generated image previews stored on disk, in MiB. The default is 256 MiB. Set 0 to disable stored previews. This is a disk-cache limit, not a memory allowance; previews may be regenerated or removed by cleanup."},
  {"historyRetentionDays","How many days completed history metadata is retained before automatic pruning. Leave empty to keep history indefinitely, which is the default. Removing old history metadata doesn't delete remote uploads or your saved local files."}};
 static const QHash<QString,QString> descriptions{
  {"unlockCredentialsAtStartup","Reads saved destination credentials from the desktop keyring once when put.gd starts, so uploads work without pressing Unlock. If the keyring is locked, the desktop shows its own unlock prompt then. Uploads and shortcuts never unlock the keyring."},
  {"checkForUpdates","Once a day put.gd asks app.put.gd for the newest version on its channel. The request is the same from every computer: no version, account, locale or ID. The comparison happens here, and nothing is installed without you. Stable builds also show a notification once for each new version; nightly builds show it only here. Check now works with daily checks off."},
  {"updateChannel","Starts on the channel of this build: a nightly build follows nightly, a release or release candidate follows stable. Choose the other one to watch it instead. Changing it doesn't change what is installed: an AppImage then offers to switch to that channel's newest release, which you start yourself. A DEB or RPM that updates from put.gd's apt or dnf repository follows that repository's channel instead, which only you change, in a terminal."},
  {"updateStatus","The result of the last check and when the next one runs. Download opens the new package for this installation in your browser. When this installation updates from put.gd's apt or dnf repository, Open software updates opens your system's updater instead."},
  {"updateInstall","Download and install fetches the new AppImage, checks it against the release's SHA256SUMS and puts it in place of the one put.gd runs from, at the same path, so login startup and menu entries keep working. It uses appimageupdatetool or zsync2 when one is installed, which downloads only what changed, and otherwise the whole file. The current version is kept beside it as a hidden backup until the new one has started. When you watch the other channel, the same steps switch to that channel's release, checked with its own signing key; an older version is installed only after you confirm."},
  {"otherChannel","This installation updates from put.gd's apt or dnf repository, so your system decides which channel it installs. These commands put the package's copy of the other channel's repository file in place, then install that channel's package. put.gd never runs them for you."},
  {"debugLogging","Adds detailed debug records to put.gd's log on this computer, for a maintainer working out a problem. It turns itself off 24 hours after you turn it on; restarting doesn't extend it. Credentials, tokens and file contents are never logged."},
  {"logSummary","put.gd keeps a log of what it did in its state folder, at most 14 days and 50 MiB by default (Advanced settings in the configuration file). Clear logs deletes every log file and starts a new one; uploads, history, settings and saved diagnostics packages stay."},
  {"uncleanExit","When put.gd starts and finds it didn't shut down cleanly last time, it records why: a signal such as a logout, or an unknown cause such as a crash, a forced kill or a power loss. The newest one is shown here and included in diagnostics packages."},
  {"uploadClipboardBehavior","Copy each link writes successful upload links individually. Copy a list writes one newline-separated list in submission order after overlapping uploads finish. Copying something yourself pauses automatic copies; links stay available in History. Applies to new operations."},
  {"destination","Choose a destination for this action or turn uploads off. Local copies are set separately, under Local copies. Applies to new captures, without changing accepted jobs."},
  {"profileId","Choose a saved destination for uploads, or No destination to turn uploads off. Local copies are set separately, under Local copies. With both outputs off, capture actions are unavailable. Changing this doesn't move earlier uploads or alter accepted jobs."},
  {"publicationMode","Use the destination default, create a share link, or keep uploads private. Whether a share link is copied, and how, follows After uploading in Settings → Capture. Private mode creates no share link and leaves the clipboard unchanged; existing member or folder permissions still apply. Unsupported modes are rejected before uploading."},
  {"mode","Keep original names preserves the names of copied files. Random names uses a UUID and extension. Custom template renames copied and uploaded files too. Generated captures use their templates unless Random names is selected. Names remain fixed on retry."},
  {"collisionPolicy","For remote duplicates, add a unique suffix (default), skip the file, or replace the checked object where safely supported. Replacement may change content served by an existing link. Local files always preserve existing files and add a suffix."},
  {"label","Display name used in destination selectors. Renaming it doesn't rename a bucket, change credentials or move uploaded files."},
  {"aclMode","Use bucket policy sends no ACL with each upload (the default); the bucket's policy decides who can open files. Request public read asks S3 to make each new object publicly readable through an object ACL, which needs ACLs enabled on the bucket and the s3:PutObjectAcl permission. Neither choice changes the bucket policy, and the policy or Block Public Access can still refuse access, so test the resulting link."},
  {"addressing","Automatic lets the client select addressing (default). Virtual puts the bucket in the hostname; path puts it in the URL path. Some S3-compatible endpoints require path addressing."},
  {"reference","Identifier for the credentials in the desktop secret service, not an access key. Changing it selects a different stored entry. Session mode stores no reference. Unlock saved credentials to inspect an existing entry."},
  {"protonExpiration","Share-link expiration in days. Zero means no expiration. Applies to newly created links; it doesn't update existing shares or delete stored files."},
  {"protonRequestTimeout","Overall Proton request deadline in seconds, with millisecond precision. Default 120 seconds. Allow time for large transfers. A timeout may require inspecting the remote result before retrying."},
  {"protonSharePassword","Password for future Proton share links. Store link password explicitly; the configuration stores only its secret-store reference. Use no link password clears that reference for future links. Existing links are unchanged."},
  {"linkMode","Backblaze B2 only. Public bucket gives each upload a link that never expires; the bucket must be public in B2 (allPublic), which needs payment history on the account. Private bucket keeps files private and gives each upload a link with a download token for exactly that file, which stops working after the chosen number of days."},
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
SettingsWindow::SettingsWindow(UiSession& settings,QWidget* parent):RetainedWindow(parent),settings_(settings) {
 setObjectName("settingsPanel");VisualStyle::apply(*qApp);
 // Graphite (2026-10-26): a sidebar names the four categories; the pages
 // stay one QTabWidget (its tab bar hidden) so state, agents and tests keep
 // the same section indexes.
 auto* outer=new QHBoxLayout(this);outer->setContentsMargins(0,0,0,0);outer->setSpacing(0);
 auto* rail=new Graphite::Surface("surface",Qt::RightEdge,this);rail->setObjectName("settingsRail");settingsRail_=rail;auto* railBox=new QVBoxLayout(rail);railBox->setContentsMargins(10,16,10,14);railBox->setSpacing(10);
 // No title: the header navigation already names the page.
 
 auto* navigation=new QListWidget(rail);settingsNavigation_=navigation;navigation->setObjectName("settingsNavigation");navigation->setAccessibleName("Settings categories");navigation->setFrameShape(QFrame::NoFrame);navigation->setItemDelegate(new Graphite::NavigationDelegate(navigation));navigation->setMouseTracking(true);navigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);navigation->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
 for(const auto& [glyph,label]:QList<QPair<QString,QString>>{{"preferences","General"},{"region","Capture"},{"shortcuts","Shortcuts"},{"settings","Advanced"}})new QListWidgetItem(VisualStyle::icon(glyph),label,navigation);
 navigation->setFixedHeight(4*Graphite::scaled(Graphite::Metric::navRow)+4);Graphite::transparentView(navigation);railBox->addWidget(navigation);railBox->addStretch();outer->addWidget(rail);
 auto* content=new QWidget(this);outer->addWidget(content,1);auto* layout=new QVBoxLayout(content);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);
 tabs_=new QTabWidget(this);tabs_->setObjectName("settingsTabs");tabs_->setAccessibleName("Settings categories");tabs_->tabBar()->installEventFilter(this);tabs_->tabBar()->hide();tabs_->setDocumentMode(true);layout->addWidget(tabs_,1);
 connect(navigation,&QListWidget::currentRowChanged,tabs_,[this](int row){if(row>=0)tabs_->setCurrentIndex(row);});connect(tabs_,&QTabWidget::currentChanged,this,[this]{clearCompletedFeedback();});connect(tabs_,&QTabWidget::currentChanged,navigation,[navigation](int index){const QSignalBlocker block(navigation);navigation->setCurrentRow(index);});
 const QHash<QString,QString> pageIntros{{"General","Startup, notifications, appearance and updates."},{"Capture","Where captures go, how they are shared, which copies stay on this computer and how files are named."},{"Shortcuts","To change a shortcut, choose Change…, press the new key combination, then release all keys. Escape cancels; Clear turns the shortcut off."},{"Advanced","Limits, history retention, diagnostics and configuration."}};
 auto scrollPage=[&](const QString& title){auto* body=new QWidget;auto* box=new QVBoxLayout(body);box->setContentsMargins(28,24,28,20);box->setSpacing(16);box->setAlignment(Qt::AlignTop);auto* heading=new QLabel(title,body);VisualStyle::role(heading,"title");box->addWidget(heading);auto* intro=new QLabel(pageIntros.value(title),body);intro->setWordWrap(true);VisualStyle::role(intro,"muted");box->addWidget(intro);auto* scroll=new QScrollArea;scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);scroll->setWidget(body);heading->setObjectName("settingsPageHeading"+title);intro->setObjectName("settingsPageDescription"+title);new CompactPageHeading(scroll,heading,intro);tabs_->addTab(scroll,title);return box;};
 auto* general=scrollPage("General");auto* capture=scrollPage("Capture");auto* shortcutsPage=scrollPage("Shortcuts");auto* advancedPage=scrollPage("Advanced");navigation->setCurrentRow(0);
 auto section=[&](QVBoxLayout* box,const QString& heading,const QString& description=QString{}){auto* card=VisualStyle::card(heading,description,this);box->addWidget(card);auto* form=new QFormLayout;VisualStyle::styleForm(form);form->setVerticalSpacing(8);qobject_cast<QVBoxLayout*>(card->layout())->addLayout(form);return form;};
 auto bind=[&](QWidget* widget,const QString& path){widget->setProperty("configPath",path);};
 auto setting=[&](QFormLayout* form,const QString& label,QWidget* control,const QString& path,const QString& hint=QString{},const QString& unit=QString{}){
  if(!qobject_cast<QCheckBox*>(control))control->setAccessibleName(label);bind(control,path);auto* wrapper=new QWidget;wrapper->setProperty("visualRole","settingRow");auto* column=new QVBoxLayout(wrapper);column->setContentsMargins(0,0,0,0);column->setSpacing(8);auto* row=new QHBoxLayout;row->setContentsMargins(0,0,0,0);row->setSpacing(8);if(unit.isEmpty())row->addWidget(control,1);
  else{auto* unitField=new QWidget;auto* unitLayout=new QHBoxLayout(unitField);unitLayout->setContentsMargins(0,0,0,0);unitLayout->setSpacing(0);control->setProperty("hasUnit",true);unitLayout->addWidget(control,1);auto* suffix=new QLabel(unit);suffix->setProperty("visualRole","unitSuffix");unitLayout->addWidget(suffix);row->addWidget(unitField,1);}
  row->addStrut(Graphite::scaled(Graphite::Metric::control));column->addLayout(row);auto* error=new QLabel;error->setWordWrap(true);error->setObjectName(control->objectName()+"Error");error->setProperty("visualRole","error");error->hide();column->addWidget(error);errors_[control]=error;
  if(!hint.isEmpty()){auto* text=new QLabel(hint);text->setObjectName(control->objectName()+"Hint");text->setWordWrap(true);VisualStyle::role(text,"muted");column->addWidget(text);}
  const auto help=settingHelp(path,control->objectName());if(auto* check=qobject_cast<QCheckBox*>(control)){
   form->addRow(wrapper);check->setAccessibleName(check->text());if(!help.isEmpty())row->addWidget(VisualStyle::helpButton(check->text(),help,control,wrapper),0,Qt::AlignLeft);check->setAccessibleDescription(help.isEmpty()?hint:help+(hint.isEmpty()?QString{}:" "+hint));check->setProperty("settingDescription",check->accessibleDescription());row->setStretchFactor(check,0);row->addStretch(1);
  }else form->addRow(VisualStyle::settingLabel(label,help,control,this),wrapper);if(help.isEmpty()){const auto description=qobject_cast<QCheckBox*>(control)?label+(hint.isEmpty()?QString{}:". "+hint):hint;control->setAccessibleDescription(description);control->setProperty("settingDescription",description);}return wrapper;
 };
 auto* preferences=section(general,"Startup and notifications");
 login_=new QCheckBox("Launch at login");login_->setObjectName("launchAtLogin");setting(preferences,"Startup",login_,"/preferences/launchAtLogin");
 unlockAtStartup_=new QCheckBox("Unlock saved credentials when put.gd starts");unlockAtStartup_->setObjectName("unlockCredentialsAtStartup");setting(preferences,"Credentials",unlockAtStartup_,"/preferences/unlockCredentialsAtStartup");
 notifications_=new QCheckBox("Show desktop notifications");notifications_->setObjectName("notifications");setting(preferences,"Notifications",notifications_,"/preferences/notifications","Results remain available in History");
 auto* look=section(general,"Appearance","Colors of these windows. The tray icon follows your panel's color either way.");
 appearance_=choices(this,"appearanceTheme",{});appearance_->addItem("Match the desktop","system");appearance_->addItem("Dark","dark");appearance_->addItem("Light","light");setting(look,"Theme",appearance_,"/preferences/appearance/theme","Applies immediately to every open put.gd window.");
 // Updates (#293): the daily check, its channel and the last result. The
 // resident runs the check; this page shows it and starts Check now.
 auto* updatesForm=section(general,"Updates","Checks app.put.gd once a day. Nothing installs automatically.");
 auto* privacy=new QToolButton;privacy->setObjectName("updatePrivacyDetails");privacy->setText("What the check sends");Graphite::disclosure(privacy);updatesForm->addRow(privacy);auto* privacyText=new QLabel("GET /update/v1/stable.json or /update/v1/nightly.json on app.put.gd. Headers: User-Agent: put.gd-update-check/1; Accept: application/json; Accept-Language: en. No query or cookies. The server receives your network IP address. Version comparison happens on this computer.");privacyText->setObjectName("updatePrivacyFields");privacyText->setWordWrap(true);VisualStyle::role(privacyText,"muted");updatesForm->addRow(privacyText);privacyText->hide();connect(privacy,&QToolButton::toggled,privacyText,&QWidget::setVisible);
 updatesForm_=updatesForm;checkUpdates_=new QCheckBox("Check for updates once a day");checkUpdates_->setObjectName("checkForUpdates");updateRows_.append(setting(updatesForm,"Updates",checkUpdates_,"/preferences/updates/check"));
 updateChannel_=choices(this,"updateChannel",{});updateChannel_->addItem("Stable","stable");updateChannel_->addItem("Nightly","nightly");auto* channelRow=setting(updatesForm,"Channel",updateChannel_,"/preferences/updates/channel");updateRows_.append(channelRow);
 // A repository install shows its repository's channel and can't choose (#386).
 updateChannelSource_=new QLabel;updateChannelSource_->setObjectName("updateChannelSource");updateChannelSource_->setWordWrap(true);VisualStyle::role(updateChannelSource_,"muted");updateChannelSource_->hide();channelRow->layout()->addWidget(updateChannelSource_);
 auto* updateRow=new QWidget;auto* updateLayout=new QHBoxLayout(updateRow);updateLayout->setContentsMargins(0,0,0,0);updateLayout->setSpacing(8);
 updateStatus_=new QLabel;updateStatus_->setObjectName("updateStatus");updateStatus_->setWordWrap(true);updateStatus_->setAlignment(Qt::AlignLeft|Qt::AlignTop);updateStatus_->setContentsMargins(0,qMax(0,(Graphite::scaled(Graphite::Metric::control)-QFontMetrics(Graphite::font(Graphite::Type::body)).height())/2),0,0);updateStatus_->setAccessibleName("Update status");
 // Focusable, so a deep link can land on it when no update action applies.
 updateStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);updateLayout->addWidget(updateStatus_,0,Qt::AlignTop);
 checkNow_=new QPushButton("Check now");checkNow_->setObjectName("checkForUpdatesNow");updateLayout->addWidget(checkNow_,0,Qt::AlignTop);updateLayout->addStretch();
 updatesForm->addRow(VisualStyle::settingLabel("Status",settingHelp({},"updateStatus"),updateStatus_,this),updateRow);
 auto* updateActions=new QWidget;auto* updateActionsLayout=new QHBoxLayout(updateActions);updateActionsLayout->setContentsMargins(0,0,0,0);updateActionsLayout->setSpacing(8);
 // A repository install (#293 phase 2) updates through the system's software
 // updater, so it gets that and the apt/dnf command instead of Download.
 openUpdates_=new QPushButton(VisualStyle::icon("download"),"Open software updates");openUpdates_->setObjectName("openSoftwareUpdates");VisualStyle::role(openUpdates_,"primary");updateActionsLayout->addWidget(openUpdates_);
 // An AppImage installs the update itself (#293 phase 3): fetched, checked
 // against the release's SHA256SUMS and put in place of the running image.
 installUpdate_=new QPushButton(VisualStyle::icon("download"),"Download and install");installUpdate_->setObjectName("installUpdate");VisualStyle::role(installUpdate_,"primary");updateActionsLayout->addWidget(installUpdate_);installUpdate_->hide();
 restartUpdate_=new QPushButton(VisualStyle::icon("redo"),"Restart put.gd");restartUpdate_->setObjectName("restartAfterUpdate");VisualStyle::role(restartUpdate_,"primary");updateActionsLayout->addWidget(restartUpdate_);restartUpdate_->hide();
 downloadUpdate_=new QPushButton(VisualStyle::icon("download"),"Download");downloadUpdate_->setObjectName("downloadUpdate");VisualStyle::role(downloadUpdate_,"primary");updateActionsLayout->addWidget(downloadUpdate_);
 releaseNotes_=new QPushButton("Release notes");releaseNotes_->setObjectName("updateReleaseNotes");VisualStyle::role(releaseNotes_,"link");updateActionsLayout->addWidget(releaseNotes_);updateActionsLayout->addStretch();
 updateActionsRow_=updateActions;updatesForm->addRow(QString(),updateActions);updateActions->hide();
 auto* updateBrowser=new Graphite::BrowserFeedback;updateBrowser->setObjectName("updateBrowserFeedback");updatesForm->addRow(QString(),updateBrowser);
 installRow_=new QWidget;auto* installLayout=new QVBoxLayout(installRow_);installLayout->setContentsMargins(0,0,0,0);installLayout->setSpacing(6);
 installProgress_=new QProgressBar;installProgress_->setObjectName("updateInstallProgress");installProgress_->setAccessibleName("Update install progress");installProgress_->setTextVisible(false);installProgress_->setMaximumWidth(320);installLayout->addWidget(installProgress_);
 installStatus_=new QLabel;installStatus_->setObjectName("updateInstallStatus");installStatus_->setWordWrap(true);installStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse);installStatus_->setAccessibleName("Update install status");installLayout->addWidget(installStatus_);
 updatesForm->addRow(VisualStyle::settingLabel("Install",settingHelp({},"updateInstall"),installStatus_,this),installRow_);updatesForm->setRowVisible(installRow_,false);
 connect(installUpdate_,&QPushButton::clicked,this,[this]{
  // A channel switch (#344) names its channel; an older version is confirmed first.
  const auto offer=settings_.diagnostics()["update"].toObject()["install"].toObject()["switch"].toObject();
  QJsonObject params{{"confirm",true}};
  if(!offer.isEmpty()){
   params["channel"]=offer["to"];
   if(offer["older"].toBool()){
    const auto image=QFileInfo(settings_.diagnostics()["update"].toObject()["install"].toObject()["image"].toString()).fileName();
    const auto target=offer["to"].toString()+" "+offer["version"].toString();
    if(!Graphite::confirm(this,"Switch to an older version","Switch to "+target+", which is older than this build ("+
       settings_.diagnostics()["update"].toObject()["build"].toObject()["version"].toString()+")?\n\nput.gd replaces "+image+" with "+target+
       ", checked against the "+offer["to"].toString()+" release's signature, at the same path. An older version may not open settings or history saved by a newer one. "
       "This version is kept as a hidden backup until the new one has started.","Switch to "+target))return;
    params["allowOlder"]=true;
   }
  }
  installUpdate_->setEnabled(false);VisualStyle::feedback(installStatus_,"Starting…");updatesForm_->setRowVisible(installRow_,true);
  settings_.request("update.install",params,this,[g=QPointer<SettingsWindow>(this)](Error error,QJsonObject){
   if(!g)return;
   if(error){g->installUpdate_->setEnabled(true);VisualStyle::feedback(g->installStatus_,error.message+".",true);return;}
   g->settings_.refreshSnapshot([g](Error){if(g)g->refreshUpdateStatus();});
  });
 });
 connect(restartUpdate_,&QPushButton::clicked,this,[this]{
  restartUpdate_->setEnabled(false);
  settings_.request("update.restart",{{"confirm",true}},this,[g=QPointer<SettingsWindow>(this)](Error error,QJsonObject){
   if(!g)return;g->restartUpdate_->setEnabled(true);
   if(error)VisualStyle::feedback(g->installStatus_,error.message+".",true);else VisualStyle::feedback(g->installStatus_,"Restarting…");
  });
 });
 auto* commandRow=new QWidget;auto* commandLayout=new QHBoxLayout(commandRow);commandLayout->setContentsMargins(0,0,0,0);commandLayout->setSpacing(8);
 updateCommand_=new QLineEdit;updateCommand_->setObjectName("updateCommand");updateCommand_->setReadOnly(true);updateCommand_->setAccessibleName("Update command");VisualStyle::role(updateCommand_,"technical");commandLayout->addWidget(updateCommand_,1);
 auto* copyCommand=new QPushButton("Copy");copyCommand->setObjectName("copyUpdateCommand");copyCommand->setAccessibleName("Copy the update command");commandLayout->addWidget(copyCommand);
 updateCommandRow_=commandRow;updatesForm->addRow(VisualStyle::settingLabel("Or in a terminal",settingHelp({},"updateCommand"),updateCommand_,this),commandRow);updatesForm->setRowVisible(commandRow,false);
 // Moving a repository install to the other channel (#386): the commands to
 // run in a terminal. put.gd never runs a privileged command itself.
 otherChannelRow_=new QWidget;auto* otherLayout=new QVBoxLayout(otherChannelRow_);otherLayout->setContentsMargins(0,0,0,0);otherLayout->setSpacing(6);
 otherChannelText_=new QLabel;otherChannelText_->setObjectName("otherChannelText");otherChannelText_->setWordWrap(true);otherLayout->addWidget(otherChannelText_);
 auto* commandsLine=new QHBoxLayout;commandsLine->setContentsMargins(0,0,0,0);commandsLine->setSpacing(8);
 otherChannelCommands_=new QLabel;otherChannelCommands_->setObjectName("otherChannelCommands");otherChannelCommands_->setTextFormat(Qt::PlainText);otherChannelCommands_->setWordWrap(true);
 otherChannelCommands_->setTextInteractionFlags(Qt::TextSelectableByMouse);otherChannelCommands_->setAccessibleName("Commands that move to the other channel");VisualStyle::role(otherChannelCommands_,"technical");commandsLine->addWidget(otherChannelCommands_,1);
 auto* copyOther=new QPushButton("Copy");copyOther->setObjectName("copyOtherChannelCommands");copyOther->setAccessibleName("Copy the commands that move to the other channel");commandsLine->addWidget(copyOther,0,Qt::AlignTop);
 otherLayout->addLayout(commandsLine);
 auto* channelDocs=new QPushButton("How switching channels works");channelDocs->setObjectName("otherChannelDocs");VisualStyle::role(channelDocs,"link");otherLayout->addWidget(channelDocs,0,Qt::AlignLeft);
 updatesForm->addRow(VisualStyle::settingLabel("Other channel",settingHelp({},"otherChannel"),otherChannelText_,this),otherChannelRow_);updatesForm->setRowVisible(otherChannelRow_,false);
 connect(copyOther,&QPushButton::clicked,this,[this]{qApp->clipboard()->setText(otherChannelCommands_->text());});
 auto* channelBrowser=new Graphite::BrowserFeedback;channelBrowser->setObjectName("channelBrowserFeedback");otherLayout->addWidget(channelBrowser);
 connect(channelDocs,&QPushButton::clicked,this,[this,channelBrowser]{const QUrl url(settings_.diagnostics()["update"].toObject()["action"].toObject()["otherChannel"].toObject()["docs"].toString());if(url.scheme()=="https")host::openLink(url,channelBrowser->opening(url));});
 connect(openUpdates_,&QPushButton::clicked,this,[this]{
  openUpdates_->setEnabled(false);
  settings_.request("update.openSoftwareUpdates",{},this,[g=QPointer<SettingsWindow>(this)](Error error,QJsonObject){if(!g)return;g->openUpdates_->setEnabled(true);if(error)VisualStyle::feedback(g->updateStatus_,error.message+".");});
 });
 connect(copyCommand,&QPushButton::clicked,this,[this]{qApp->clipboard()->setText(updateCommand_->text());});
 connect(checkNow_,&QPushButton::clicked,this,[this]{checkForUpdates({});});
 connect(downloadUpdate_,&QPushButton::clicked,this,[this,updateBrowser]{const QUrl url(settings_.diagnostics()["update"].toObject()["result"].toObject()["downloadUrl"].toString());if(url.scheme()=="https")host::openLink(url,updateBrowser->opening(url));});
 connect(releaseNotes_,&QPushButton::clicked,this,[this,updateBrowser]{const QUrl url(settings_.diagnostics()["update"].toObject()["result"].toObject()["notesUrl"].toString());if(url.scheme()=="https")host::openLink(url,updateBrowser->opening(url));});

 auto* captureDefaults=section(capture,"Default capture output","Where captures and clipboard uploads go, and whether they get a share link.");captureDefaults->parentWidget()->layout()->setContentsMargins(0,16,0,0);
 captureProfile_=choices(this,"defaultCaptureDestination",{});auto* destinationRow=setting(captureDefaults,"Destination",captureProfile_,{});
 // With no destination and no local copy every capture fails: say so right under
 // Destination, where it is seen first, with the two ways out (#596 review).
 captureOutputBlock_=new QWidget;captureOutputBlock_->setObjectName("captureOutputBlock");auto* blockLayout=new QVBoxLayout(captureOutputBlock_);blockLayout->setContentsMargins(0,2,0,0);blockLayout->setSpacing(8);
 captureOutputNotice_=new QLabel;captureOutputNotice_->setObjectName("captureOutputNotice");captureOutputNotice_->setWordWrap(true);blockLayout->addWidget(captureOutputNotice_);
 auto* blockActions=new QHBoxLayout;blockActions->setContentsMargins(0,0,0,0);blockActions->setSpacing(8);
 auto* enableLocal=new QPushButton("Keep a local copy");enableLocal->setObjectName("enableLocalCopies");enableLocal->setAccessibleName("Turn on Keep a local copy");blockActions->addWidget(enableLocal);
 chooseOutput_=new QPushButton("Choose a destination");chooseOutput_->setObjectName("chooseCaptureDestination");blockActions->addWidget(chooseOutput_);blockActions->addStretch();blockLayout->addLayout(blockActions);
 qobject_cast<QVBoxLayout*>(destinationRow->layout())->addWidget(captureOutputBlock_);captureOutputBlock_->hide();
 connect(enableLocal,&QPushButton::clicked,this,[this]{keep_->setChecked(true);keep_->setFocus(Qt::OtherFocusReason);});
 connect(chooseOutput_,&QPushButton::clicked,this,[this]{if(captureProfile_->count()>1){captureProfile_->setFocus(Qt::OtherFocusReason);captureProfile_->showPopup();}else emit destinationsRequested();});
 captureSharing_=choices(this,"defaultCaptureSharing",{});for(const auto& mode:QStringList{"default","share","private"})captureSharing_->addItem(sharingLabel(mode),mode);setting(captureDefaults,"Sharing",captureSharing_,{});
 uploadClipboard_=choices(this,"uploadClipboardBehavior",{});uploadClipboard_->addItem("Copy each link as it finishes","each_link");uploadClipboard_->addItem("Copy a list when uploads finish","list");setting(captureDefaults,"After uploading",uploadClipboard_,"/preferences/uploadClipboardBehavior");
 captureExceptions_=new QToolButton;captureExceptions_->setObjectName("captureExceptionsToggle");captureExceptions_->setText("Per-action exceptions");Graphite::disclosure(captureExceptions_);captureExceptions_->setProperty("graphiteFlush",true);capture->addWidget(captureExceptions_,0,Qt::AlignLeft);
 // One compact row per action: its name, the output it gets and Override. Only
 // an overridden action shows its Destination and Sharing. The hidden Use
 // default box stays the model that tests, view state and agents read.
 auto* exceptionBody=new QWidget;exceptionBody->setObjectName("captureExceptions");exceptionBody->setMaximumWidth(Graphite::scaled(880));auto* exceptionBox=new QVBoxLayout(exceptionBody);exceptionBox->setContentsMargins(0,0,0,0);exceptionBox->setSpacing(12);capture->addWidget(exceptionBody);exceptionBody->hide();
 auto* exceptionIntro=new QLabel("Actions use the default output above. Record screen follows Capture region unless you override it.");exceptionIntro->setWordWrap(true);VisualStyle::role(exceptionIntro,"muted");exceptionBox->addWidget(exceptionIntro);
 auto* exceptionRows=captureExceptionRows_=new QFormLayout;VisualStyle::styleForm(exceptionRows);exceptionRows->setVerticalSpacing(8);exceptionBox->addLayout(exceptionRows);
 connect(captureExceptions_,&QToolButton::toggled,this,[=,this](bool open){exceptionBody->setVisible(open);captureExceptions_->setArrowType(open?Qt::DownArrow:Qt::RightArrow);emit viewStateChanged();});
 for(const auto& action:Settings::presentationOrder()){
  auto* inherit=useCaptureDefault_[action]=new QCheckBox(action=="record"?"Use Capture region output":"Use default",exceptionBody);inherit->setObjectName("useDefault"+title(action));inherit->setAccessibleName(actionLabel(action)+(action=="record"?": use Capture region output":": use default output"));inherit->hide();
  auto* line=new QWidget;line->setObjectName("captureRow"+title(action));auto* lineLayout=new QHBoxLayout(line);lineLayout->setContentsMargins(0,0,0,0);lineLayout->setSpacing(12);
  auto* summary=captureSummary_[action]=new QLabel;summary->setObjectName("captureSummary"+title(action));summary->setWordWrap(true);summary->setTextInteractionFlags(Qt::TextSelectableByMouse);VisualStyle::role(summary,"muted");
  auto* override=captureOverride_[action]=new QCheckBox("Override");override->setObjectName("override"+title(action));override->setAccessibleName(actionLabel(action)+(action=="record"?": override Capture region output":": override the default output"));
  auto* name=new QLabel(QString(actionLabel(action)).replace('&',QStringLiteral("&&")));name->setObjectName("captureRowName"+title(action));name->setBuddy(override);lineLayout->addWidget(name);lineLayout->addWidget(override);lineLayout->addWidget(summary,1);exceptionRows->addRow(line);
  auto* editor=captureEditors_[action]=new QWidget;editor->setObjectName("captureEditor"+title(action));auto* fields=new QFormLayout(editor);VisualStyle::styleForm(fields);fields->setContentsMargins(24,0,0,6);
  actionProfiles_[action]=choices(this,"profile"+title(action),{});publicationModes_[action]=choices(this,"publication"+title(action),{});for(const auto& mode:QStringList{"default","share","private"})publicationModes_[action]->addItem(sharingLabel(mode),mode);
  for(auto [combo,caption,path]:{std::tuple{actionProfiles_[action],QString("Destination"),"/actions/"+action+"/profileId"},std::tuple{publicationModes_[action],QString("Sharing"),"/actions/"+action+"/publicationMode"}}){
   auto* wrapper=new QWidget(editor);auto* column=new QVBoxLayout(wrapper);column->setContentsMargins(0,0,0,0);column->setSpacing(8);column->addWidget(combo);fields->addRow(VisualStyle::settingLabel(caption,settingHelp(path,combo->objectName()),combo,editor),wrapper);bind(combo,path);
   auto* error=new QLabel;error->setWordWrap(true);error->setObjectName(combo->objectName()+"Error");error->setProperty("visualRole","error");error->hide();column->addWidget(error);errors_[combo]=error;}
  // The explanation behind a failing output, for view state and agents; the row's summary says it.
  auto* status=captureStatus_[action]=new QLabel(editor);status->setObjectName("captureStatus"+title(action));status->setWordWrap(true);status->hide();
  exceptionRows->addRow(editor);editor->hide();
  connect(override,&QCheckBox::toggled,this,[inherit](bool on){inherit->setChecked(!on);});
  connect(inherit,&QCheckBox::toggled,this,[this,inherit]{if(!loading_)queueCaptureEdit(inherit);});
 }
 captureProfile_->setModel(actionProfiles_["region"]->model());
 for(auto* combo:{captureProfile_,captureSharing_})connect(combo,&QComboBox::currentIndexChanged,this,[this,combo]{if(!loading_)queueCaptureEdit(combo);});
 // Local copies sit with the destination (one output decision on one page); the
 // config paths and autosave are unchanged.
 auto* files=section(capture,"Local copies","Captures and clipboard uploads saved on this computer, with or without uploading. Category folders override Save to.");
 keep_=new QCheckBox("Keep a local copy");keep_->setObjectName("keepUploadCopies");setting(files,"Each capture",keep_,"/archive/keepUploads","Local-copy failures are reported separately from uploads.");
 root_=input(this,"archiveRoot");auto* rootRow=setting(files,"Save to",root_,"/archive/root","Use absolute folder paths. Existing files get unique names.");
 folderChooser_=new FileDialog(this);
 auto browse=[this](QLineEdit* field){chooseOutputFolder(field);};
 // Every folder row browses the same way, beside its field.
 // A chooser that could not open says so under its row, with Try again; the folder stays as it was.
 auto chooserFailure=[=,this](QWidget* row,QLineEdit* field){auto* failure=new QWidget;failure->setObjectName(field->objectName()+"ChooserFailure");auto* line=new QHBoxLayout(failure);line->setContentsMargins(0,0,0,0);line->setSpacing(8);auto* message=new QLabel;message->setObjectName(field->objectName()+"ChooserError");message->setWordWrap(true);VisualStyle::role(message,"error");line->addWidget(message,1);auto* retry=new QPushButton("Try again");retry->setObjectName(field->objectName()+"ChooserRetry");retry->setAccessibleName("Try opening the folder chooser again");line->addWidget(retry,0,Qt::AlignTop);qobject_cast<QVBoxLayout*>(row->layout())->insertWidget(1,failure);failure->hide();chooserFailures_[field]=failure;connect(retry,&QPushButton::clicked,this,[=,this]{browse(field);});};
 auto* chooseRoot=new QPushButton("Browse…");chooseRoot->setAccessibleName("Choose default output folder");qobject_cast<QHBoxLayout*>(rootRow->layout()->itemAt(0)->layout())->addWidget(chooseRoot);connect(chooseRoot,&QPushButton::clicked,this,[=,this]{browse(root_);});chooserFailure(rootRow,root_);
 auto* folderToggle=new QToolButton;folderToggle->setObjectName("categoryFoldersToggle");folderToggle->setText("Different folders by file type");Graphite::disclosure(folderToggle);files->addRow(folderToggle);auto* folderPanel=new QWidget;folderPanel->setObjectName("categoryFolders");auto* categoryFolderForm=new QFormLayout(folderPanel);VisualStyle::styleForm(categoryFolderForm);categoryFolderForm->setContentsMargins(0,0,0,0);files->addRow(folderPanel);folderPanel->hide();connect(folderToggle,&QToolButton::toggled,this,[this,folderPanel](bool expanded){folderPanel->setVisible(expanded);emit viewStateChanged();});
 for(const auto& category:QStringList{"images","videos","text","other"}){auto* field=folders_[category]=input(this,"folder"+title(category));field->setPlaceholderText("Same as Save to");connect(field,&QLineEdit::textChanged,folderToggle,[folderToggle](const QString& text){if(!text.isEmpty())folderToggle->setChecked(true);});auto* row=setting(categoryFolderForm,title(category),field,"/archive/folders/"+category);auto* button=new QPushButton("Browse…");button->setAccessibleName("Choose "+category+" folder");qobject_cast<QHBoxLayout*>(row->layout()->itemAt(0)->layout())->addWidget(button);connect(button,&QPushButton::clicked,this,[=,this]{browse(field);});chooserFailure(row,field);}
 connect(keep_,&QCheckBox::toggled,this,[this]{if(!loading_)updateCaptureInheritance();});
 auto* namingForm=section(capture,"File names");naming_=choices(this,"namingMode",{});naming_->addItem("Keep original names","original");naming_->addItem("Random names","generated");naming_->addItem("Custom template","template");setting(namingForm,"Naming",naming_,"/naming/mode");
 collision_=choices(this,"collisionPolicy",{"rename","skip","overwrite"});for(int i=0;i<collision_->count();++i)collision_->setItemData(i,collision_->itemText(i));collision_->setItemText(0,"Add a unique suffix");collision_->setItemText(1,"Skip duplicate names");collision_->setItemText(2,"Replace the checked object");setting(namingForm,"When a name exists",collision_,"/naming/collisionPolicy");
 for(const auto& key:QStringList{"fileTemplate","captureTemplate","imageTemplate","textTemplate"}){templates_[key]=input(this,key);setting(namingForm,key=="fileTemplate"?"Copied & uploaded files":key=="captureTemplate"?"Screen captures":key=="imageTemplate"?"Clipboard images":"Clipboard text",templates_[key],"/naming/"+key);}
 // The rail, the page heading and its intro already name the place: the
// editor follows under one rule, without a nested "Global shortcuts" heading.
 auto* shortcutCard=new QFrame;VisualStyle::role(shortcutCard,"card");shortcutCard->setObjectName("shortcutsCard");shortcutCard->setMaximumWidth(Graphite::scaled(880));auto* shortcutBox=new QVBoxLayout(shortcutCard);shortcutBox->setContentsMargins(0,16,0,16);shortcutsPage->addWidget(shortcutCard);shortcuts_=new ShortcutEditor(this);shortcutBox->addWidget(shortcuts_);shortcuts_->findChild<QToolButton*>("shortcutDetailsToggle")->hide();shortcuts_->setRecordingControl([session=&settings_,editor=shortcuts_](bool active,std::function<void(Error)> reply){session->setShortcutRecording(active,editor,std::move(reply));});shortcuts_->setRefreshAction([this]{settings_.refreshSnapshot();});
 for(const auto& action:Settings::presentationOrder()){auto* edit=shortcuts_->findChild<QLineEdit*>("shortcut"+title(action));bind(edit,"/actions/"+action+"/shortcut");}
 // Within the cards' width, so Reset limits lines up with the values it resets.
 auto* introRow=new QWidget;introRow->setMaximumWidth(Graphite::scaled(880));auto* intro=new QHBoxLayout(introRow);intro->setContentsMargins(0,0,0,0);intro->setSpacing(16);auto* introText=new QLabel("Defaults suit almost everyone.");introText->setWordWrap(true);VisualStyle::role(introText,"muted");intro->addWidget(introText,1);auto* resetAll=new QPushButton("Reset limits to defaults…");resetAll->setObjectName("resetAdvancedDefaults");resetAll->setToolTip("Resets the upload, image, memory, cache and history limits below. Diagnostics and the config file are unchanged.");intro->addWidget(resetAll);advancedPage->addWidget(introRow);
 // One column, like the other pages: two narrow columns crowded labels and values.
 resourceLimitsToggle_=new QToolButton;resourceLimitsToggle_->setObjectName("resourceLimitsToggle");resourceLimitsToggle_->setText("Resource limits");Graphite::disclosure(resourceLimitsToggle_);advancedPage->addWidget(resourceLimitsToggle_);resourceLimits_=new QWidget;resourceLimits_->setObjectName("resourceLimits");auto* resources=new QVBoxLayout(resourceLimits_);resources->setContentsMargins(0,0,0,0);resources->setSpacing(0);advancedPage->addWidget(resourceLimits_);resourceLimits_->hide();connect(resourceLimitsToggle_,&QToolButton::toggled,resourceLimits_,&QWidget::setVisible);
 auto* left=resources;auto* right=advancedPage;
 const auto defaults=Settings::defaults();
 auto number=[&](QFormLayout* form,const QString& key,const QString& name,const QString& unit){auto* edit=numbers_[key]=input(this,key);QWidget* editor=edit;if(key=="historyRetentionDays"){auto* combo=choices(this,"historyRetentionChoice",{});combo->setEditable(true);combo->setInsertPolicy(QComboBox::NoInsert);combo->addItem("Forever",QString{});combo->addItem("30 days","30");combo->addItem("90 days","90");combo->addItem("1 year","365");combo->setLineEdit(edit);editor=combo;connect(combo,&QComboBox::activated,this,[this,combo,edit](int index){edit->setText(combo->itemData(index).toString());queueEdit(edit);});}
  // File, batch and waiting-upload sizes: Auto follows this computer's memory, an empty field is No limit.
  else if(Settings::artifactByteLimitKeys().contains(key)){auto* combo=choices(this,key+"Choice",{});combo->setEditable(true);combo->setInsertPolicy(QComboBox::NoInsert);combo->addItem("Auto","Auto");combo->addItem("No limit",QString{});combo->setLineEdit(edit);edit->setPlaceholderText("No limit");editor=combo;connect(combo,&QComboBox::activated,this,[this,combo,edit](int index){edit->setText(combo->itemData(index).toString());queueEdit(edit);});}
  const bool retention=defaults["retention"].toObject().contains(key);auto* wrap=setting(form,name,editor,QString(retention?"/retention/":"/limits/")+key,{},unit);if(editor!=edit){errors_[edit]=errors_[editor];bind(edit,QString(retention?"/retention/":"/limits/")+key);}const QJsonValue value=retention?defaults["retention"].toObject()[key]:defaults["limits"].toObject()[key];const qint64 factor=displayUnit(key);const auto text=value.isNull()?QString{}:value.isString()?QStringLiteral("Auto"):ByteSize::exact(value.toInteger(),factor);edit->setProperty("defaultText",text);if(!retention)connect(edit,&QLineEdit::textChanged,this,[this]{updateResourceLimits();});if(!unit.isEmpty()){for(auto* suffix:wrap->findChildren<QLabel*>())if(suffix->property("visualRole")=="unitSuffix"){suffix->setObjectName(key+"Unit");auto updateUnit=[edit,editor,suffix]{const auto value=edit->text().trimmed();bool numeric=false;value.toDouble(&numeric);suffix->setVisible(numeric);editor->setProperty("hasUnit",numeric);editor->update();};connect(edit,&QLineEdit::textChanged,this,updateUnit);updateUnit();}}auto* reset=new QPushButton("Reset");reset->setAccessibleName("Reset "+name+" to default");reset->setToolTip("Default: "+(text.isEmpty()?QString(key=="historyRetentionDays"?"Forever":"No limit"):text+(unit.isEmpty()||text=="Auto"?QString{}:" "+unit)));auto* row=qobject_cast<QHBoxLayout*>(wrap->layout()->itemAt(0)->layout());
  // Every value box is the same width with its unit after it; a changed value
  // gets Reset beside it, an unchanged one nothing.
  editor->setFixedWidth(Graphite::scaled(112));for(int i=0;i<row->count();++i)row->setStretch(i,0);
  // What Auto resolves to on this computer (#149), from settings.get effectiveLimits.
  if(Settings::artifactByteLimitKeys().contains(key)){auto* resolved=limitHints_[key]=new QLabel;resolved->setObjectName(key+"Resolved");VisualStyle::role(resolved,"muted");resolved->setWordWrap(true);resolved->hide();
   // Below the value and its unit, so a long hint never stretches that control row.
   qobject_cast<QVBoxLayout*>(wrap->layout())->insertWidget(1,resolved);connect(edit,&QLineEdit::textChanged,this,[this]{updateLimitHints();});}
  row->addWidget(reset);row->addStretch(1);auto update=[=,this]{const bool changed=edit->text()!=text;reset->setVisible(changed);wrap->setProperty("changed",changed);wrap->style()->unpolish(wrap);wrap->style()->polish(wrap);};connect(edit,&QLineEdit::textChanged,this,update);connect(reset,&QPushButton::clicked,this,[=,this]{edit->setText(text);queueEdit(edit);});update();};
 auto* uploads=section(left,"Uploads");number(uploads,"concurrentUploads","Uploads at once",{});number(uploads,"maxFileBytes","Largest file","GiB");number(uploads,"maxBatchBytes","Largest batch","GiB");number(uploads,"pendingArtifactBytes","Space for queued files","GiB");number(uploads,"maxFiles","Files in a batch",{});number(uploads,"pendingOperations","Queued operations",{});
 uploadReserveHint_=new QLabel;uploadReserveHint_->setObjectName("uploadReserveHint");uploadReserveHint_->setWordWrap(true);VisualStyle::role(uploadReserveHint_,"muted");uploads->addRow(QString(),uploadReserveHint_);
 auto* images=section(left,"Images and clipboard");number(images,"imageBytes","Largest image","MiB");number(images,"imagePixels","Largest image size","megapixels");number(images,"clipboardTextBytes","Clipboard text limit","MiB");
 auto* memory=section(left,"Memory and cache");number(memory,"pendingImageBytes","Memory for queued images","MiB");number(memory,"previewCacheBytes","Thumbnail cache","MiB");
 auto* retention=section(right,"History");number(retention,"historyRetentionDays","Keep history for","days");numbers_["historyRetentionDays"]->setPlaceholderText("Forever");number(retention,"failedArtifactDays","Keep failed uploads for","days");number(retention,"recentActivityLimit","Items in recent list",{});
 connect(resetAll,&QPushButton::clicked,this,[this]{if(!Graphite::confirm(this,"Reset limits to defaults","Reset the limits on this page to their defaults: uploads, images and clipboard, memory and cache, and history retention?\n\nDebug logging, diagnostics, the config file and every other setting stay as they are.","Reset limits"))return;for(auto* field:numbers_){field->setText(field->property("defaultText").toString());queueEdit(field);}});
 // Diagnostics (#122): what the persistent log holds and the package a maintainer needs.
 // Advanced, above Config file (#238); Reset limits to defaults leaves it alone.
 auto* diagnosticsForm=section(right,"Diagnostics","Logs and diagnostics packages stay on this computer. A package is a redacted .zip you can send to a maintainer.");
 debugLogging_=new QCheckBox("Keep detailed debug logs for 24 hours");debugLogging_->setObjectName("debugLogging");auto* debugRow=setting(diagnosticsForm,"Debug logging",debugLogging_,"/preferences/diagnostics/debugLogging");
 debugLoggingStatus_=new QLabel;debugLoggingStatus_->setObjectName("debugLoggingStatus");debugLoggingStatus_->setWordWrap(true);VisualStyle::role(debugLoggingStatus_,"muted");qobject_cast<QVBoxLayout*>(debugRow->layout())->addWidget(debugLoggingStatus_);
 auto* logsRow=new QWidget;auto* logsLayout=new QHBoxLayout(logsRow);logsLayout->setContentsMargins(0,0,0,0);logsLayout->setSpacing(8);logSummary_=new QLabel;logSummary_->setObjectName("logSummary");logSummary_->setWordWrap(true);logSummary_->setAccessibleName("Log folder size, file count and oldest record");logsLayout->addWidget(logSummary_,1);
 clearLogs_=new QPushButton("Clear logs…");clearLogs_->setObjectName("clearLogs");logsLayout->addWidget(clearLogs_,0,Qt::AlignTop);
 diagnosticsForm->addRow(VisualStyle::settingLabel("Logs",settingHelp({},"logSummary"),logSummary_,this),logsRow);
 loggingWarning_=new QLabel;loggingWarning_->setObjectName("loggingDegraded");loggingWarning_->setWordWrap(true);VisualStyle::role(loggingWarning_,"warning");loggingWarning_->hide();diagnosticsForm->addRow(loggingWarning_);
 uncleanExit_=new QLabel;uncleanExit_->setObjectName("uncleanExit");uncleanExit_->setWordWrap(true);uncleanExit_->setAccessibleName("Last unclean exit");diagnosticsForm->addRow(VisualStyle::settingLabel("Last unclean exit",settingHelp({},"uncleanExit"),uncleanExit_,this),uncleanExit_);
 auto* createPackage=new QPushButton(VisualStyle::icon("save"),"Create diagnostics package…");createPackage->setObjectName("createDiagnosticsPackage");createPackage->setToolTip("Preview and save a redacted .zip of recent failures, settings and logs. Nothing is uploaded.");diagnosticsForm->addRow(createPackage);
 connect(clearLogs_,&QPushButton::clicked,this,[this]{if(!Graphite::confirm(this,"Clear logs","Delete every put.gd log file? Uploads, history, settings and saved diagnostics packages stay.","Clear logs",true))return;clearLogs({});});
 connect(createPackage,&QPushButton::clicked,this,[this]{openDiagnosticsPackage();});
 auto* config=section(right,"Config file");configPath_=new QLabel;configPath_->setObjectName("configPath");configPath_->setTextFormat(Qt::PlainText);configPath_->setWordWrap(true);configPath_->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);configPath_->setProperty("visualRole","technical");config->addRow(configPath_);auto* configActions=new QHBoxLayout;configActions->setContentsMargins(0,0,0,0);configActions->setSpacing(8);auto* editor=new QPushButton("Open in editor");editor->setObjectName("openConfigEditor");auto* reload=new QPushButton("Reload from file…");reload->setObjectName("reloadSettings");reload->setToolTip("Replace this window's settings with the configuration file as it is now.");configActions->addWidget(editor);configActions->addWidget(reload);configActions->addStretch();config->addRow(configActions);auto* editorFailure=new QLabel("Couldn't open the config file in an editor. Copy its path and open it manually.");editorFailure->setObjectName("configEditorFailure");editorFailure->setWordWrap(true);VisualStyle::role(editorFailure,"error");config->addRow(editorFailure);editorFailure->hide();auto* copyPath=new QPushButton("Copy path");copyPath->setObjectName("copyConfigPath");configActions->insertWidget(1,copyPath);copyPath->hide();connect(copyPath,&QPushButton::clicked,this,[this]{qApp->clipboard()->setText(settings_.path());});connect(editor,&QPushButton::clicked,this,[this,editorFailure,copyPath]{host::openFile(settings_.path(),[failure=QPointer<QLabel>(editorFailure),copy=QPointer<QPushButton>(copyPath)](const QJsonObject& result){if(failure)failure->setVisible(!result["opened"].toBool());if(copy)copy->setVisible(!result["opened"].toBool());});});connect(reload,&QPushButton::clicked,this,&SettingsWindow::reloadFromFile);
  auto* diagnostics=section(advancedPage,"Desktop support details");capabilities_=new QLabel;capabilities_->setAccessibleName("Desktop support diagnostics");capabilities_->setWordWrap(true);capabilities_->setTextInteractionFlags(Qt::TextSelectableByMouse);VisualStyle::role(capabilities_,"mono");diagnostics->addRow(capabilities_);metadata_=new MetadataLabel;metadata_->setObjectName("settingsMetadata");metadata_->setWordWrap(true);metadata_->setTextInteractionFlags(Qt::TextSelectableByMouse);VisualStyle::role(metadata_,"muted");diagnostics->addRow(metadata_);auto* diagnose=new QPushButton("Refresh desktop support");diagnose->setObjectName("refreshDesktopSupport");auto* copyDiagnostics=new QPushButton("Copy diagnostics");auto* diagnosticActions=new QHBoxLayout;diagnosticActions->addWidget(diagnose);diagnosticActions->addWidget(copyDiagnostics);diagnosticActions->addStretch();diagnostics->addRow(diagnosticActions);connect(diagnose,&QPushButton::clicked,this,[this]{settings_.refreshSnapshot();});connect(copyDiagnostics,&QPushButton::clicked,this,[this]{qApp->clipboard()->setText(capabilities_->text()+"\n"+metadataText_);});
 destinationPage_=new QWidget(this);destinationPage_->setObjectName("destinationsPage");destinationPage_->installEventFilter(this);auto* destinationLayout=new QHBoxLayout(destinationPage_);destinationLayout->setContentsMargins(0,0,0,0);destinationLayout->setSpacing(0);
 auto* destinationRail=new Graphite::Surface("surface",Qt::RightEdge);destinationRail->setObjectName("destinationRail");destinationRail_=destinationRail;auto* railLayout=new QVBoxLayout(destinationRail);railLayout->setContentsMargins(10,16,10,14);railLayout->setSpacing(10);auto* destinationList=new DestinationList;destinationList_=destinationList;destinationList_->setObjectName("destinationList");destinationList_->setAccessibleName("Destinations");destinationList_->setIconSize(QSize(Graphite::scaled(18),Graphite::scaled(18)));destinationList_->setWordWrap(false);destinationList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);railLayout->addWidget(destinationList_,1);auto* add=new QPushButton("Add destination");addDestination_=add;add->setObjectName("addS3Destination");railLayout->addWidget(add);destinationLayout->addWidget(destinationRail);destinationList_->setFrameShape(QFrame::NoFrame);destinationList_->setMouseTracking(true);Graphite::transparentView(destinationList_);
 auto* destinationContent=new QWidget;destinationContent->setObjectName("destinationContent");auto* destinationContentLayout=new QVBoxLayout(destinationContent);destinationContentLayout->setContentsMargins(0,0,0,0);destinationContentLayout->setSpacing(0);destinationLayout->addWidget(destinationContent,1);
 auto* destinationBody=new QWidget;auto* pageLayout=new QVBoxLayout(destinationBody);pageLayout->setContentsMargins(28,22,28,24);pageLayout->setSpacing(16);pageLayout->setAlignment(Qt::AlignTop);auto* destinationScroll=new QScrollArea;destinationScroll->setObjectName("destinationScroll");destinationScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);destinationScroll->setWidgetResizable(true);destinationScroll->setFrameShape(QFrame::NoFrame);destinationScroll->setWidget(destinationBody);destinationContentLayout->addWidget(destinationScroll,1);
 auto* emptyDestinations=new QLabel("No destinations yet\nAdd a destination to start uploading.");emptyDestinations->setObjectName("destinationsEmpty");emptyDestinations->setWordWrap(true);emptyDestinations->setAlignment(Qt::AlignCenter);VisualStyle::role(emptyDestinations,"muted");destinationLayout->addWidget(emptyDestinations,1);
 profile_=choices(this,"destinationProfile",{});profile_->hide();profileId_=input(this,"destinationId");profileId_->hide();providerJson_=new QPlainTextEdit(this);providerJson_->setObjectName("providerSettingsJson");providerJson_->hide();
 auto* destinationHeader=new QWidget;destinationHeader->setObjectName("destinationHeader");destinationHeader->setMaximumWidth(Graphite::scaled(880));auto* headerLayout=new QVBoxLayout(destinationHeader);headerLayout->setContentsMargins(0,0,0,0);headerLayout->setSpacing(18);pageLayout->addWidget(destinationHeader);
 auto* titleRow=new QHBoxLayout;auto* titleName=new QHBoxLayout;titleName->setSpacing(4);auto* titleActions=new QHBoxLayout;titleActions->setSpacing(8);
 profileLabel_=new DestinationTitle(this);profileLabel_->setObjectName("destinationLabel");profileLabel_->setProperty("visualRole","editableTitle");Graphite::showTextStartWhenUnfocused(profileLabel_);profileLabel_->setPlaceholderText("Destination name");profileLabel_->setAccessibleName("Destination name");bind(profileLabel_,"/profiles/{profile}/label");titleName->addWidget(profileLabel_);titleName->addWidget(VisualStyle::helpButton("Destination name",settingHelp("/profiles/{profile}/label",{}),profileLabel_,this));titleName->addStretch(1);
 auto* testScope=new QLabel;testScope->setObjectName("destinationTestScope");testScope->setWordWrap(true);testScope->setTextFormat(Qt::PlainText);VisualStyle::role(testScope,"muted");auto* test=new QPushButton("Test upload and link");test->setObjectName("testDestination");test->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Fixed);defaultButton_=new QPushButton("Make default");defaultButton_->setObjectName("makeDefaultDestination");titleActions->addWidget(defaultButton_);auto* more=new QPushButton;more->setIcon(Graphite::chromeIcon("more"));more->setToolTip("Destination actions");more->setProperty("noChevron",true);more->setAccessibleName("Destination actions");more->setObjectName("destinationActions");more->setFixedSize(Graphite::scaled(36),Graphite::scaled(36));titleActions->addWidget(more);titleActions->addStretch();
 titleRow->addLayout(titleName,1);titleRow->addLayout(titleActions);headerLayout->addLayout(titleRow);
 // The viewport is the available width even if the form's minimum size grows.
 // With testing in the footer, the trailing actions fit beside the name longer.
 Graphite::responsiveColumns(titleRow,destinationScroll->viewport(),400);
 auto* description=new QLabel("Amazon S3 or compatible storage. Existing uploads keep their original destination.");description->setWordWrap(true);VisualStyle::role(description,"muted");headerLayout->addWidget(description);new CompactPageHeading(destinationScroll,profileLabel_,description);
 auto* destinationMenu=new QMenu(more);more->setMenu(destinationMenu);auto* rename=destinationMenu->addAction("Rename destination");rename->setObjectName("renameDestination");connect(rename,&QAction::triggered,this,[this]{QTimer::singleShot(0,this,[this]{profileLabel_->setFocus(Qt::ShortcutFocusReason);profileLabel_->selectAll();});});destinationMenu->addSeparator();auto* serviceMenu=destinationMenu->addMenu("Storage service");auto* services=new QActionGroup(serviceMenu);services->setObjectName("storageServices");services->setExclusive(true);for(const auto& service:QList<QPair<QString,QString>>{{"Amazon S3 / compatible","s3"},{"Proton Drive","proton-drive"},{"Backblaze B2","backblaze-b2"}}){auto* action=serviceMenu->addAction(service.first);action->setCheckable(true);action->setData(service.second);services->addAction(action);connect(action,&QAction::triggered,this,[this,action]{providerType_->setCurrentIndex(providerType_->findData(action->data()));});}auto* remove=new QPushButton("Remove destination",this);remove->setObjectName("removeDestination");remove->hide();connect(destinationMenu->addAction("Remove destination…"),&QAction::triggered,remove,&QPushButton::click);destinationMenu->addSeparator();moveUp_=destinationMenu->addAction("Move up");moveDown_=destinationMenu->addAction("Move down");
 for(auto* move:{moveUp_,moveDown_}){move->setObjectName(move==moveUp_?"moveDestinationUp":"moveDestinationDown");move->setShortcut(QKeySequence(Qt::ALT|(move==moveUp_?Qt::Key_Up:Qt::Key_Down)));move->setShortcutContext(Qt::WidgetShortcut);destinationList_->addAction(move);connect(move,&QAction::triggered,this,[this,move]{QJsonObject result;if(auto error=moveSelectedDestination(move==moveUp_?-1:1,{},result))status(error);});}
 destinationList->moveRequested=[this](int from,int to){if(auto error=moveDestination(from,to))status(error);};
 destinationMenu->addSeparator();connect(destinationMenu->addAction("Copy details"),&QAction::triggered,this,[this]{qApp->clipboard()->setText(QString::fromUtf8(QJsonDocument(editedProfile()).toJson(QJsonDocument::Indented)));});
 auto* dev=new QPushButton("Import development S3",this);dev->setObjectName("importDevelopmentS3");dev->hide();auto* addMenu=new QMenu(add);add->setMenu(addMenu);connect(addMenu->addAction("Amazon S3 / S3-compatible"),&QAction::triggered,this,[this]{addS3Profile();});connect(addMenu->addAction("Proton Drive"),&QAction::triggered,this,[this]{const auto count=profile_->count();addS3Profile("Proton Drive");if(profile_->count()==count)return;providerType_->setCurrentIndex(providerType_->findData("proton-drive"));queueEdit(profileLabel_);});connect(addMenu->addAction("Backblaze B2"),&QAction::triggered,this,[this]{const auto count=profile_->count();addS3Profile("Backblaze B2");if(profile_->count()==count)return;providerType_->setCurrentIndex(providerType_->findData("backblaze-b2"));queueEdit(profileLabel_);});
#ifndef NDEBUG
 connect(addMenu->addAction("Import development S3"),&QAction::triggered,dev,&QPushButton::click);
#endif
 auto* destinationDetails=section(pageLayout,"Where files go");destinationDetails->parentWidget()->setObjectName("destinationEditor");
 providerType_=choices(this,"storageProvider",{});providerType_->addItem("Amazon S3","s3");providerType_->addItem("Proton Drive","proton-drive");providerType_->addItem("Backblaze B2","backblaze-b2");connect(providerType_,&QComboBox::currentIndexChanged,this,[this,services]{for(auto* action:services->actions())action->setChecked(action->data()==providerType_->currentData());});setting(destinationDetails,"Storage service",providerType_,"/profiles/{profile}");
 auto* s3Service=choices(this,"s3Service",{"Amazon S3","Cloudflare R2","Other S3-compatible"});s3Service->setAccessibleName("S3-compatible service");destinationDetails->addRow(VisualStyle::settingLabel("S3 service",settingHelp({},"s3Service"),s3Service,this),s3Service);
 const QHash<QString,QString> s3Labels{{"bucket","Bucket"},{"region","Region"},{"endpoint","Endpoint"},{"objectPrefix","Folder in bucket"},{"publicUrlBase","Link domain"},{"storageClass","Storage class"},{"connectTimeoutMs","Connect timeout"},{"requestTimeoutMs","Request timeout"},{"lowSpeedBytesPerSecond","Minimum transfer speed"},{"lowSpeedTimeSeconds","Low-speed window"}};
 for(const auto& key:QStringList{"bucket","region","endpoint","objectPrefix","publicUrlBase","storageClass","connectTimeoutMs","requestTimeoutMs","lowSpeedBytesPerSecond","lowSpeedTimeSeconds"})s3_[key]=input(this,"s3"+title(key));
 auto* bucketHint=setting(destinationDetails,"Bucket",s3_["bucket"],"/profiles/{profile}/settings/bucket","Required before uploads can go here.");auto* bucketText=bucketHint->findChild<QLabel*>("s3BucketHint");bucketText->setTextFormat(Qt::PlainText);auto updateBucketHint=[this,bucketText]{bucketText->setText(s3_["bucket"]->text().isEmpty()?QString("Required before uploads can go here."):"Uploads go to "+QString(providerType_->currentData()=="backblaze-b2"?"b2://":"s3://")+s3_["bucket"]->text()+"/"+s3_["objectPrefix"]->text());};connect(providerType_,&QComboBox::currentIndexChanged,this,updateBucketHint);connect(s3_["bucket"],&QLineEdit::textChanged,this,updateBucketHint);connect(s3_["objectPrefix"],&QLineEdit::textChanged,this,updateBucketHint);updateBucketHint();s3_["bucket"]->setPlaceholderText("my-screenshots");
 regionChoices_=choices(this,"s3RegionChoices",s3Regions());regionChoices_->setEditable(true);regionChoices_->setInsertPolicy(QComboBox::NoInsert);regionChoices_->setLineEdit(s3_["region"]);setting(destinationDetails,"Region",regionChoices_,"/profiles/{profile}/settings/region");bind(s3_["region"],"/profiles/{profile}/settings/region");
 setting(destinationDetails,"Folder in bucket",s3_["objectPrefix"],"/profiles/{profile}/settings/objectPrefix","Optional. Leave empty for the bucket root. Existing uploads stay where they are.");
 auto* links=section(pageLayout,"Links");linksPanel_=links->parentWidget();// What the upload asks S3 for, not a promise about who can open the link.
 acl_=choices(this,"s3AclMode",{"omit","public-read"});for(int i=0;i<acl_->count();++i)acl_->setItemData(i,acl_->itemText(i));acl_->setItemText(0,"Use bucket policy");acl_->setItemText(1,"Request public read");auto* aclRow=setting(links,"Upload access request",acl_,"/profiles/{profile}/settings/aclMode");auto* aclHint=new QLabel("Bucket policy also controls access. Test the resulting link.");aclHint->setObjectName("s3AclModeHint");aclHint->setWordWrap(true);VisualStyle::role(aclHint,"muted");aclHint->setAlignment(Qt::AlignLeft|Qt::AlignTop);links->addRow(static_cast<QWidget*>(nullptr),aclHint);b2LinkMode_=choices(this,"b2LinkMode",{});b2LinkMode_->addItem("Public bucket: links that never expire","public");b2LinkMode_->addItem("Private bucket: links that expire","authorized");auto* b2ModeRow=setting(links,"Who can open files",b2LinkMode_,"/profiles/{profile}/settings/linkMode");b2LinkDays_=new QSpinBox;b2LinkDays_->setObjectName("b2LinkDays");b2LinkDays_->setRange(1,b2::maximumLinkDays);auto* b2DaysRow=setting(links,"Links last",b2LinkDays_,"/profiles/{profile}/settings/linkValidityDays","Links expire after 1–7 days; files stay in the bucket.","days");connect(b2LinkMode_,&QComboBox::activated,this,[this]{queueEdit(b2LinkMode_);updateProviderVisibility_();});connect(b2LinkDays_,&QSpinBox::editingFinished,this,[this]{queueEdit(b2LinkDays_);});setting(links,"Link domain",s3_["publicUrlBase"],"/profiles/{profile}/settings/publicUrlBase");s3_["publicUrlBase"]->setPlaceholderText("https://i.example.com");linkExample_=new QLabel;linkExample_->setObjectName("linkExample");linkExample_->setWordWrap(true);linkExample_->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);linkExample_->setAccessibleName("Example link");VisualStyle::role(linkExample_,"technical");auto* exampleCaption=new QLabel("Example link");links->addRow(exampleCaption,linkExample_);VisualStyle::alignFormValue(exampleCaption,linkExample_);
 links->parentWidget()->layout()->setContentsMargins(0,16,0,0);auto* advanced=new QToolButton;advanced->setObjectName("advancedSettings");advanced->setText("Advanced connection settings");advanced->setAccessibleName("Advanced connection settings");Graphite::disclosure(advanced);advanced->setProperty("graphiteFlush",true);pageLayout->addWidget(advanced);auto* connectionSummary=new QLabel;connectionSummary->setObjectName("connectionSummary");connectionSummary->setWordWrap(true);VisualStyle::role(connectionSummary,"muted");pageLayout->addWidget(connectionSummary);auto* connectionPanel=new QWidget;connectionPanel->setMaximumWidth(Graphite::scaled(880));auto* connectionBox=new QVBoxLayout(connectionPanel);connectionBox->setContentsMargins(0,0,0,0);auto* connection=new QFormLayout;VisualStyle::styleForm(connection);connectionBox->addLayout(connection);pageLayout->addWidget(connectionPanel);connectionPanel_=connectionPanel;
 endpointHint_=new QLabel;endpointHint_->setObjectName("s3EndpointValue");endpointHint_->setWordWrap(true);endpointHint_->setTextInteractionFlags(Qt::TextSelectableByMouse);endpointHint_->setAccessibleName("Endpoint in use");VisualStyle::role(endpointHint_,"technical");auto* endpointCaption=new QLabel("Endpoint");connection->addRow(endpointCaption,endpointHint_);VisualStyle::alignFormValue(endpointCaption,endpointHint_);
 auto* endpointWarning=new QLabel("Backblaze B2's S3 API lacks the conditional writes put.gd needs, so uploads to it fail. Use the Backblaze B2 storage service instead.");endpointWarning->setObjectName("s3EndpointWarning");endpointWarning->setWordWrap(true);VisualStyle::role(endpointWarning,"warning");destinationDetails->addRow(QString(),endpointWarning);auto* customEndpoint=new QCheckBox("Use a custom endpoint");customEndpoint->setObjectName("customEndpoint");auto* customEndpointRow=VisualStyle::settingRow("Use a custom endpoint",settingHelp({},"customEndpoint"),customEndpoint,this);connection->addRow(QString(),customEndpointRow);setting(connection,"Endpoint",s3_["endpoint"],"/profiles/{profile}/settings/endpoint");connect(s3Service,&QComboBox::activated,this,[=,this](int index){if(index){advanced->setChecked(true);customEndpoint->setChecked(true);s3_["endpoint"]->setPlaceholderText(index==1?"https://ACCOUNT.r2.cloudflarestorage.com":"https://your-storage-endpoint");s3_["endpoint"]->setFocus();}});

 QHash<QString,QWidget*> connectionRows;addressing_=choices(this,"s3Addressing",{"path","virtual","auto"});for(int i=0;i<addressing_->count();++i)addressing_->setItemData(i,addressing_->itemText(i));addressing_->setItemText(0,"Bucket in URL path");addressing_->setItemText(1,"Bucket in hostname");addressing_->setItemText(2,"Automatic");
 for(const auto& key:QStringList{"storageClass","connectTimeoutMs","requestTimeoutMs","lowSpeedBytesPerSecond","lowSpeedTimeSeconds"}){auto* field=s3_[key];const auto suffix=key.endsWith("Ms")||key=="lowSpeedTimeSeconds"?"seconds":key=="lowSpeedBytesPerSecond"?"bytes/s":"";auto* row=setting(connection,s3Labels[key],field,"/profiles/{profile}/settings/"+key,{},suffix);connectionRows.insert(key,row);const QJsonValue value=s3Profile("default")["settings"].toObject()[key];const auto text=value.isDouble()?ByteSize::exact(value.toInteger(),key.endsWith("Ms")?1000:1):value.toString();auto* reset=new QPushButton("Reset");reset->setAccessibleName("Reset "+s3Labels[key]);reset->setToolTip("Default: "+text+(QString(suffix).isEmpty()?QString{}:" "+QString(suffix)));auto* line=qobject_cast<QHBoxLayout*>(row->layout()->itemAt(0)->layout());field->setFixedWidth(Graphite::scaled(key=="storageClass"?200:112));for(int i=0;i<line->count();++i)line->setStretch(i,0);line->addWidget(reset);line->addStretch(1);connect(field,&QLineEdit::textChanged,this,[=,this]{bool changed=field->text()!=text;reset->setVisible(changed);row->setProperty("changed",changed);row->style()->unpolish(row);row->style()->polish(row);});connect(reset,&QPushButton::clicked,this,[=,this]{field->setText(text);queueEdit(field);});}
 auto* addressRow=setting(connection,"Addressing",addressing_,"/profiles/{profile}/settings/addressing");auto* resetAddress=new QPushButton("Reset");resetAddress->setAccessibleName("Reset addressing to automatic");resetAddress->setToolTip("Default: Automatic");resetAddress->hide();auto* addressLine=qobject_cast<QHBoxLayout*>(addressRow->layout()->itemAt(0)->layout());for(int i=0;i<addressLine->count();++i)addressLine->setStretch(i,0);addressLine->addWidget(resetAddress);addressLine->addStretch(1);connect(addressing_,&QComboBox::currentIndexChanged,this,[=,this]{const bool changed=addressing_->currentData()!="auto";resetAddress->setVisible(changed);addressRow->setProperty("changed",changed);addressRow->style()->unpolish(addressRow);addressRow->style()->polish(addressRow);});connect(resetAddress,&QPushButton::clicked,this,[this]{addressing_->setCurrentIndex(addressing_->findData("auto"));});reference_=input(this,"credentialReference");setting(connection,"Secret-store reference",reference_,"/profiles/{profile}/credentials/reference");
 auto* credentialForm=section(pageLayout,"Credentials","Credentials never enter settings or history. Typed replacements are cleared after use and when this window closes or reloads.");auto* credentialsEditor=credentialForm->parentWidget();credentialsEditor->setObjectName("credentialsEditor");
 // One status row says which key is stored and where; the fields below only take a replacement.
 credentialHint_=new QLabel;credentialHint_->setObjectName("storedCredentialHint");credentialHint_->setWordWrap(true);credentialHint_->setAccessibleName("Stored credential identification");credentialForm->addRow(VisualStyle::settingLabel("Current key",{},credentialHint_,this),credentialHint_);credentialForm_=credentialForm;credentialForm->setRowVisible(credentialHint_,false);
 access_=input(this,"credentialAccessKey");secret_=input(this,"credentialSecretKey");token_=input(this,"credentialSessionToken");secret_->setEchoMode(QLineEdit::Password);token_->setEchoMode(QLineEdit::Password);auto* accessRow=setting(credentialForm,"Access key ID",access_,{});auto* secretRow=setting(credentialForm,"Secret access key",secret_,{});showSecret_=new QPushButton("Show");showSecret_->setObjectName("showCredentialSecret");showSecret_->setCheckable(true);showSecret_->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Preferred);showSecret_->setMinimumHeight(Graphite::scaled(Graphite::Metric::control));showSecret_->setAccessibleName("Show "+secret_->accessibleName().toLower());qobject_cast<QHBoxLayout*>(secretRow->layout()->itemAt(0)->layout())->addWidget(showSecret_);connect(showSecret_,&QPushButton::toggled,this,&SettingsWindow::toggleCredentialSecret);auto* tokenRow=setting(credentialForm,"Session token",token_,{},"Optional, for temporary AWS credentials.");credentialMode_=choices(this,"credentialMode",{"keychain","session"});for(int i=0;i<credentialMode_->count();++i)credentialMode_->setItemData(i,credentialMode_->itemText(i));credentialMode_->setItemText(0,"System keychain");credentialMode_->setItemText(1,"This session only");auto* storageRow=setting(credentialForm,"Credential storage",credentialMode_,"/profiles/{profile}/credentials");credentialStorageNotice_=new QLabel("Existing credentials aren't copied to the new storage location.");credentialStorageNotice_->setObjectName("credentialStorageNotice");credentialStorageNotice_->setWordWrap(true);VisualStyle::role(credentialStorageNotice_,"muted");storageRow->layout()->addWidget(credentialStorageNotice_);credentialStorageNotice_->hide();connect(credentialMode_,&QComboBox::currentIndexChanged,this,[this]{if(loading_)return;credentialStorageNotice_->show();if(credentialMode_->currentData()=="keychain"&&reference_->text().isEmpty())reference_->setText("putgd/"+profileId_->text());});
 // Content-sized and in the field column, 8 px apart side by side or stacked.
 auto* credentialActionRow=new QWidget;credentialActionRow->setObjectName("credentialActions");auto* credentialActions=new QHBoxLayout(credentialActionRow);credentialActions->setContentsMargins(0,0,0,0);credentialActions->setSpacing(8);auto* credentials=credentialStoreButton_=new QPushButton;credentials->setObjectName("storeCredentials");auto credentialLabel=[this,credentials]{credentials->setText(credentialMode_->currentData()=="session"?"Use for this session":"Save to keychain");const auto help="Choose "+credentials->text()+" to apply typed credentials after destination edits save. "+(credentialMode_->currentData()=="session"?QString("They stay in memory until put.gd exits."):QString("They are saved in the desktop secret service."));for(auto* field:QList<QWidget*>{credentialMode_,access_,secret_,token_}){field->setToolTip(help);field->setAccessibleDescription(help);field->setProperty("settingDescription",help);}};connect(credentialMode_,&QComboBox::currentIndexChanged,credentials,credentialLabel);credentialLabel();auto* unlock=new QPushButton("Unlock saved credentials");unlock->setObjectName("unlockCredentials");for(auto* button:{credentials,unlock}){button->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Fixed);credentialActions->addWidget(button,0,Qt::AlignLeft);}credentialActions->addStretch();credentialForm->addRow(QString(),credentialActionRow);Graphite::responsiveColumns(credentialActions,destinationScroll->viewport(),600);credentialStoreReason_=new QLabel;credentialStoreReason_->setObjectName("credentialStoreReason");credentialStoreReason_->setWordWrap(true);VisualStyle::role(credentialStoreReason_,"muted");credentialForm->addRow(QString(),credentialStoreReason_);
 protonPanel_=new QWidget;protonPanel_->setObjectName("protonDestinationEditor");auto* protonLayout=new QVBoxLayout(protonPanel_);protonLayout->setContentsMargins(0,0,0,0);protonLayout->setSpacing(16);pageLayout->addWidget(protonPanel_);auto* folderForm=section(protonLayout,"Where files go");auto* protonForm=section(protonLayout,"Account");protonFolderStatus_=new QLabel;protonFolderStatus_->setObjectName("protonFolderStatus");protonFolderStatus_->setWordWrap(true);protonFolderStatus_->hide();folderForm->addRow(protonFolderStatus_);auto* publicationForm=section(protonLayout,"Links");publicationForm->parentWidget()->layout()->setContentsMargins(0,16,0,0);auto* protonAdvancedForm=section(protonLayout,"Link settings");protonAdvancedForm->parentWidget()->setObjectName("protonAdvancedSettings");// The account and its Reconnect action share the field column.
 auto* accountField=new QWidget;auto* accountLayout=new QVBoxLayout(accountField);accountLayout->setContentsMargins(0,0,0,0);accountLayout->setSpacing(8);
 protonAccount_=new QLabel("Signed out");protonAccount_->setObjectName("protonAccountStatus");protonAccount_->setWordWrap(true);protonAccount_->setAccessibleName("Proton account");accountLayout->addWidget(protonAccount_);protonBrowserRetry_=new QPushButton("Try opening browser again");protonBrowserRetry_->setObjectName("retryProtonBrowser");accountLayout->addWidget(protonBrowserRetry_,0,Qt::AlignLeft);protonBrowserRetry_->hide();connect(protonBrowserRetry_,&QPushButton::clicked,this,&SettingsWindow::retryProtonBrowser);auto* connectProton=new QPushButton("Connect account…");connectProton->setObjectName("connectProtonAccount");connectProton->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Fixed);protonForm->addRow("Status",accountField);protonForm->addRow(QString(),connectProton);
 auto* inspectProton=new QPushButton("Refresh folders");inspectProton->setObjectName("inspectProtonAccount");inspectProton->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Fixed);inspectProton->setToolTip("Reload the available Proton folders without changing the selected upload folder.");
 protonFolder_=choices(this,"protonFolder",{});auto* folderRow=setting(folderForm,"Upload folder",protonFolder_,"/profiles/{profile}/settings","Choose a top-level folder for future uploads.");auto* createProton=new QPushButton("New folder…");createProton->setObjectName("createProtonFolder");createProton->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Fixed);
 // The folder, Refresh folders and New folder… are one left-aligned group, 8 px
 // apart; the two actions move under the folder when the page is narrow.
 {auto* line=qobject_cast<QHBoxLayout*>(folderRow->layout()->itemAt(0)->layout());line->setStretchFactor(protonFolder_,0);auto* folderActions=new QWidget;folderActions->setObjectName("protonFolderActions");auto* actionsLayout=new QHBoxLayout(folderActions);actionsLayout->setContentsMargins(0,0,0,0);actionsLayout->setSpacing(8);actionsLayout->addWidget(inspectProton);actionsLayout->addWidget(createProton);actionsLayout->addStretch();line->addWidget(folderActions,0,Qt::AlignLeft);line->addStretch(1);Graphite::responsiveColumns(line,destinationScroll->viewport(),780);}protonShare_=new QCheckBox("Create a share link");protonShare_->setObjectName("protonShareDefault");protonShare_->setParent(this);bind(protonShare_,"/profiles/{profile}/settings/shareByDefault");auto* privateMode=new QRadioButton(sharingLabel("private"));privateMode->setObjectName("protonKeepPrivate");auto* publicMode=new QRadioButton(sharingLabel("share"));publicMode->setObjectName("protonCreateShareLink");protonShare_->hide();auto* linkChoices=new QWidget;auto* linkColumn=new QVBoxLayout(linkChoices);linkColumn->setContentsMargins(0,0,0,0);linkColumn->setSpacing(8);linkColumn->addWidget(VisualStyle::settingRow(publicMode->text(),"Create a share link for future Proton uploads. Links are copied as set under Settings → Capture → After uploading. Link expiration and password apply to new shares; existing links stay unchanged.",publicMode,this));linkColumn->addWidget(VisualStyle::settingRow(privateMode->text(),"Upload privately without creating a link or changing the clipboard. Existing member or folder access still applies.",privateMode,this));auto inlineNote=[linkColumn](const QString& text){auto* note=new QLabel(text);note->setWordWrap(true);VisualStyle::role(note,"muted");linkColumn->addWidget(note);};inlineNote("People with the link can view it; a configured password is also required.");auto* privateRow=linkColumn->takeAt(1);linkColumn->addItem(privateRow);inlineNote("No share link is created. Existing folder access still applies.");auto* copying=new QLabel("Links are copied as set under Settings → Capture → After uploading.");copying->setObjectName("protonCopyingNote");copying->setWordWrap(true);VisualStyle::role(copying,"muted");linkColumn->addWidget(copying);publicationForm->addRow("Share link",linkChoices);connect(publicMode,&QRadioButton::clicked,this,[this]{protonShare_->setChecked(true);});connect(privateMode,&QRadioButton::clicked,this,[this]{protonShare_->setChecked(false);});connect(protonShare_,&QCheckBox::toggled,this,[=,this](bool share){publicMode->setChecked(share);privateMode->setChecked(!share);if(!loading_)updateCaptureInheritance();});
 protonExpiration_=new QSpinBox;protonExpiration_->setObjectName("protonExpiration");protonExpiration_->setRange(0,365);protonExpiration_->setSpecialValueText("No expiration");protonExpiration_->setSuffix(" days");setting(protonAdvancedForm,"Link expiration",protonExpiration_,"/profiles/{profile}/settings/linkExpirationDays");protonTimeout_=new QDoubleSpinBox;protonTimeout_->setObjectName("protonRequestTimeout");protonTimeout_->setDecimals(3);protonTimeout_->setRange(1,ProtonProvider::MaximumRequestTimeoutMs/1000);protonTimeout_->setSuffix(" seconds");setting(protonAdvancedForm,"Request deadline",protonTimeout_,"/profiles/{profile}/settings/requestTimeoutMs");protonPassword_=input(this,"protonSharePassword");protonPassword_->setEchoMode(QLineEdit::Password);setting(protonAdvancedForm,"New link password",protonPassword_,{});auto* storePassword=new QPushButton("Store link password");storePassword->setObjectName("storeProtonPassword");protonAdvancedForm->addRow(storePassword);passwordStoreReason_=new QLabel;passwordStoreReason_->setObjectName("passwordStoreReason");passwordStoreReason_->setWordWrap(true);VisualStyle::role(passwordStoreReason_,"muted");protonAdvancedForm->addRow(passwordStoreReason_);auto* clearPassword=new QPushButton("Use no link password");clearPassword->setObjectName("clearProtonPassword");protonAdvancedForm->addRow(clearPassword);
 connect(connectProton,&QPushButton::clicked,this,[this]{protonBrowserOpened_=false;pendingProtonUrl_.clear();pendingProtonSetup_.clear();protonBrowserOpening_=false;protonBrowserRetry_->hide();beginSetup("connectAccount");});connect(inspectProton,&QPushButton::clicked,this,[this]{beginSetup("inspectAccount");});connect(createProton,&QPushButton::clicked,this,[this]{bool ok=false;const auto name=QInputDialog::getText(this,"New Proton folder","Folder name",QLineEdit::Normal,"put.gd",&ok);if(ok&&!name.trimmed().isEmpty())beginSetup("createFolder",{{"name",name.trimmed()}});});
 connect(storePassword,&QPushButton::clicked,this,[this]{if(!admitSetup())return;const auto reference="putgd/proton/link/"+QUuid::createUuid().toString(QUuid::Id128);auto value=protonPassword_->text();beginSetup("storeSharePassword",{{"password",value},{"reference",reference}});value.fill(QChar{});});connect(clearPassword,&QPushButton::clicked,this,[this]{auto profiles=draft_["profiles"].toArray();if(profileIndex_<0||profileIndex_>=profiles.size())return;auto p=profiles[profileIndex_].toObject();auto s=p["settings"].toObject();s["sharePasswordReference"]=QJsonValue::Null;p["settings"]=s;profiles[profileIndex_]=p;draft_["profiles"]=profiles;queueEdit(protonShare_,"/profiles/{profile}/settings");});
 auto formRow=[this](QWidget* field)->QWidget*{return errors_.contains(field)?errors_[field]->parentWidget():field;};
 const auto relabel=[](QFormLayout* form,QWidget* row,const QString& text){if(auto* label=form->labelForField(row)?form->labelForField(row)->findChild<QLabel*>():nullptr)label->setText(text);};
 updateProviderVisibility_=[=,this]{const auto provider=providerType_->currentData().toString();for(auto* action:services->actions())action->setChecked(action->data()==provider);const bool s3=provider=="s3";const bool b2=provider=="backblaze-b2";const bool storage=s3||b2;const bool authorized=b2&&b2LinkMode_->currentData()=="authorized";const bool expanded=advanced->isChecked();advanced->setText(storage?"Advanced connection settings":"Advanced link settings");advanced->setArrowType(expanded?Qt::DownArrow:Qt::RightArrow);description->setText(s3?"Amazon S3 or compatible storage. Existing uploads keep their original destination.":b2?"Backblaze B2 through its native API. Existing uploads keep their original destination.":"Third-party Proton Drive integration. Share links open a Proton page.");for(auto* field:{s3_["bucket"],s3_["objectPrefix"]})destinationDetails->setRowVisible(formRow(field),storage);destinationDetails->parentWidget()->setVisible(storage);destinationDetails->setRowVisible(formRow(providerType_),false);destinationDetails->setRowVisible(s3Service,s3);destinationDetails->setRowVisible(formRow(regionChoices_),s3);connection->setRowVisible(endpointHint_,s3&&!customEndpoint->isChecked());destinationDetails->setRowVisible(endpointWarning,s3&&s3_["endpoint"]->text().contains("backblazeb2.com"));connection->setRowVisible(customEndpointRow,s3);connection->setRowVisible(formRow(s3_["endpoint"]),s3&&customEndpoint->isChecked());linksPanel_->setVisible(storage);links->setRowVisible(aclRow,s3);links->setRowVisible(aclHint,s3);links->setRowVisible(b2ModeRow,b2);links->setRowVisible(b2DaysRow,authorized);links->setRowVisible(formRow(s3_["publicUrlBase"]),s3||(b2&&!authorized));connectionPanel_->setVisible(storage&&expanded);for(auto it=connectionRows.begin();it!=connectionRows.end();++it)connection->setRowVisible(it.value(),s3||it.key()=="connectTimeoutMs"||it.key()=="requestTimeoutMs");connection->setRowVisible(addressRow,s3);credentialsEditor->setVisible(storage);credentialForm->setRowVisible(tokenRow,s3);relabel(credentialForm,accessRow,b2?"Application key ID":"Access key ID");relabel(credentialForm,secretRow,b2?"Application key":"Secret access key");access_->setAccessibleName(b2?"Application key ID":"Access key ID");secret_->setAccessibleName(b2?"Application key":"Secret access key");protonPanel_->setVisible(provider=="proton-drive");test->setVisible(storage||provider=="proton-drive");test->setText(storage?"Test upload and link":"Check account and folder");test->setAccessibleName(test->text());testScope->setText(storage?"Tests an upload, its link and cleanup.":"Checks account and folder access. Upload and sharing aren't tested.");showSecret_->setAccessibleName((showSecret_->isChecked()?QString("Hide "):QString("Show "))+secret_->accessibleName().toLower());test->setToolTip(s3?"Checks access, a test upload, public link and cleanup.":b2?"Checks the application key, a test upload, the link put.gd hands out, cleanup and a large-file start.":"Checks the signed-in Proton account and selected folder without uploading. Upload and sharing aren't tested.");advanced->setVisible(true);connectionSummary->setVisible(storage&&!expanded);connectionSummary->setText(b2?s3_["connectTimeoutMs"]->text()+" s connect · "+s3_["requestTimeoutMs"]->text()+" s request":storageClassLabel(s3_["storageClass"]->text())+" · "+addressing_->currentText()+" · "+s3_["connectTimeoutMs"]->text()+" s connect · "+s3_["requestTimeoutMs"]->text()+" s request");protonAdvancedForm->parentWidget()->setVisible(provider=="proton-drive"&&expanded);endpointHint_->setText(s3_["endpoint"]->text());{const QSignalBlocker block(s3Service);const auto endpoint=s3_["endpoint"]->text();s3Service->setCurrentIndex(endpoint.contains("cloudflarestorage.com")?1:endpoint.contains("amazonaws.com")?0:2);}if(b2){const auto settings=editedProfile()["settings"].toObject();linkExample_->setText(b2::fileUrl(settings,"https://f000.backblazeb2.com",b2::fileName(settings,"Screenshot.png"))+(authorized?QStringLiteral("?Authorization=…"):QString()));}else linkExample_->setText(publicObjectUrl(editedProfile()["settings"].toObject(),s3_["objectPrefix"]->text()+"Screenshot.png"));};
 pageLayout->removeWidget(credentialsEditor);pageLayout->insertWidget(pageLayout->indexOf(linksPanel_),credentialsEditor);
 pageLayout->removeWidget(advanced);pageLayout->removeWidget(connectionSummary);pageLayout->removeWidget(connectionPanel_);pageLayout->addWidget(advanced);pageLayout->addWidget(connectionSummary);pageLayout->addWidget(connectionPanel_);protonLayout->removeWidget(protonAdvancedForm->parentWidget());pageLayout->addWidget(protonAdvancedForm->parentWidget());
 connect(advanced,&QToolButton::toggled,this,[this]{updateProviderVisibility_();emit viewStateChanged();});connect(customEndpoint,&QCheckBox::toggled,this,[this]{updateProviderVisibility_();});
 connect(providerType_,&QComboBox::currentIndexChanged,this,&SettingsWindow::switchProvider);
 connect(destinationList_,&QListWidget::currentRowChanged,this,[this](int index){if(!loading_)profile_->setCurrentIndex(index);});connect(profile_,&QComboBox::currentIndexChanged,this,[this]{if(loading_)return;storeProfile();loadProfile();refreshDestinationList();if(destinationPage_->isVisible())activateDestinations();emit viewStateChanged();});
 connect(defaultButton_,&QPushButton::clicked,this,[this]{if(testedDestinations_.value(profileId_->text())!=editedProfile()&&!Graphite::confirm(this,"Use an untested destination","This connection has not passed a test in this session. Use \""+profileLabel_->text()+"\" as the capture default? Per-action exceptions will stay unchanged.","Make default"))return;{const QSignalBlocker block(captureProfile_);captureProfile_->setCurrentIndex(captureProfile_->findData(profileId_->text()));}queueCaptureEdit(defaultButton_);});
 connect(remove,&QPushButton::clicked,this,[this]{if(!Graphite::confirm(this,"Remove destination","Remove \""+profileLabel_->text()+"\" from settings? Existing history and remote files remain.","Remove destination",true))return;storeProfile();draft_=candidate();auto profiles=draft_["profiles"].toArray();const int index=profile_->currentIndex();if(index<0||index>=profiles.size())return;const auto id=profiles[index].toObject()["id"].toString();if(captureProfile_->currentData()==id){const QSignalBlocker block(captureProfile_);captureProfile_->setCurrentIndex(0);updateCaptureInheritance();}for(const auto& action:Settings::presentationOrder())if(actionProfiles_[action]->currentData()==id){actionProfiles_[action]->setCurrentIndex(0);}captureDefaultsEnabled_=true;captureRoutingEdited_=true;profiles.removeAt(index);if(index<profileEditors_.size())profileEditors_.removeAt(index);if(index<credentialDrafts_.size())credentialDrafts_.removeAt(index);draft_["profiles"]=profiles;rebuildProfiles(qMin(index,int(profiles.size())-1));queueEdit(defaultButton_,"/");refreshDestinationList();});
 connect(dev,&QPushButton::clicked,this,&SettingsWindow::importDevelopmentProfile);
 connect(s3_["region"],&QLineEdit::textChanged,this,[this](const QString& region){if(loading_)return;auto* endpoint=s3_["endpoint"];const auto previous=s3_["region"]->property("previousRegion").toString();if(endpoint->text()=="https://s3."+previous+".amazonaws.com")endpoint->setText("https://s3."+region+".amazonaws.com");s3_["region"]->setProperty("previousRegion",region);updateProviderVisibility_();});
 connect(credentials,&QPushButton::clicked,this,[this]{if(!admitSetup())return;resetCredentialSecretDisplay();QJsonObject value{{"accessKey",access_->text()},{"secretKey",secret_->text()},{"sessionToken",token_->text()}};beginSetup("storeCredentials",value);});connect(unlock,&QPushButton::clicked,this,[this]{beginSetup("unlockCredentials");});connect(test,&QPushButton::clicked,this,[this]{beginSetup("testDestination");});
 // Each page's status line carries its recovery actions beside the message:
 // Reload from file… for a conflict, Retry saving for a refused save. Saved
 // notes stay meta type; errors take body type (Graphite's meta role).
 status_=new QLabel;status_->setObjectName("settingsStatus");status_->setAccessibleName("Settings status");status_->setWordWrap(true);VisualStyle::role(status_,"meta");destinationStatus_=new QLabel;destinationStatus_->setObjectName("destinationStatus");destinationStatus_->setWordWrap(true);destinationStatus_->setAccessibleName("Destination status");VisualStyle::role(destinationStatus_,"muted");
 // Settings' status is a footer under the scrolling page, set off by a hairline.
 QList<SettingsStatusRow*> feedbackRows;QList<QHBoxLayout*> statusRows;for(int i=0;i<2;++i){auto* message=new QWidget;auto* text=new QVBoxLayout(message);text->setContentsMargins(0,0,0,0);text->setSpacing(4);text->addWidget(i?destinationStatus_:status_);if(i)text->addWidget(testScope);auto* row=new SettingsStatusRow(message);row->setMaximumWidth(Graphite::scaled(880)+56);row->setObjectName(i?"destinationStatusRow":"settingsStatusRow");feedbackRows.append(row);statusRows.append(row->actions());if(i){row->actions()->addWidget(test,0,Qt::AlignTop|Qt::AlignRight);destinationContentLayout->addWidget(row);}else layout->addWidget(row);}
 for(int i=0;i<2;++i){auto* reload=new QPushButton("Reload from file…");reload->setObjectName(i?"reloadDestinationConflict":"reloadConflict");reload->setAccessibleName("Reload settings from file");reload->setToolTip("Replace this window's settings with the configuration file as it is now. Asks first, naming the unsaved edits it discards.");reload->hide();reloadConflict_.insert(i,reload);statusRows[i]->addWidget(reload,0,Qt::AlignTop);connect(reload,&QPushButton::clicked,this,&SettingsWindow::reloadFromFile);}
 for(int i=0;i<2;++i){auto* retry=new QPushButton("Retry saving");retry->setObjectName(i?"retryDestinationAutosave":"retryAutosave");retry->hide();retryAutosave_.append(retry);statusRows[i]->addWidget(retry,0,Qt::AlignTop);connect(retry,&QPushButton::clicked,this,[this,retry]{autosaveBlocked_=false;for(auto* button:retryAutosave_)button->hide();if(pendingEdits_.isEmpty()){for(const auto& group:QStringList{"archive","preferences","naming","limits","retention","actions","profiles","captureDefaults"})if((group!="profiles"||!initialProfile_)&&(group!="captureDefaults"||captureDefaultsEnabled_))queueEdit(retry,"/"+group);}save();});}
 for(int i=0;i<2;++i){QList<QWidget*> parts{i?destinationStatus_:status_,reloadConflict_[i],retryAutosave_[i]};if(i)parts.append(test);new VisibleWithParts(feedbackRows[i],parts);}
 autosave_=new QTimer(this);autosave_->setSingleShot(true);autosave_->setInterval(400);connect(autosave_,&QTimer::timeout,this,&SettingsWindow::save);
 for(auto* edit:findChildren<QLineEdit*>()){if(edit==access_||edit==secret_||edit==token_||edit==protonPassword_||edit->property("configPath").toString().isEmpty())continue;connect(edit,&QLineEdit::textChanged,this,[this,edit]{dirty();if(!edit->hasFocus()||edit->isReadOnly())queueEdit(edit);});connect(edit,&QLineEdit::editingFinished,this,[this,edit]{queueEdit(edit);});}
 for(auto* edit:{access_,secret_,token_,protonPassword_})connect(edit,&QLineEdit::textChanged,this,&SettingsWindow::viewStateChanged);
 for(auto* combo:findChildren<QComboBox*>())if(combo!=profile_&&!combo->property("configPath").toString().isEmpty())connect(combo,&QComboBox::currentIndexChanged,this,[this,combo]{queueEdit(combo);});
 for(auto* check:findChildren<QCheckBox*>())if(!check->property("configPath").toString().isEmpty())connect(check,&QCheckBox::toggled,this,[this,check]{queueEdit(check);});
 // #237: a picked theme applies at once and saves without the autosave delay,
 // so the resident passes it to putgd-record straight away too.
 connect(appearance_,&QComboBox::currentIndexChanged,this,[this]{
  if(loading_||resetting_)return;
  appearancePreviewed_=true;emit appearancePreview(appearance_->currentData().toString());
  if(pendingEdits_.contains(themePath)){if(saving_)autosave_->start();else save();}
  // Nothing to save, or saving is blocked until Retry saving or Reload: the
  // accepted theme stays.
  if(!saving_&&(!pendingEdits_.contains(themePath)||autosaveBlocked_||conflict_))endAppearancePreview();
 });
 for(auto* spin:findChildren<QAbstractSpinBox*>())connect(spin,&QAbstractSpinBox::editingFinished,this,[this,spin]{queueEdit(spin);});
 // Paths, URLs, prefixes, bucket names and identifiers read as technical values.
 for(QWidget* field:std::initializer_list<QWidget*>{root_,reference_,access_,s3_["bucket"],s3_["region"],s3_["endpoint"],s3_["objectPrefix"],s3_["publicUrlBase"],s3_["storageClass"]})VisualStyle::role(field,"technical");
 for(auto* field:folders_)VisualStyle::role(field,"technical");for(auto* field:templates_)VisualStyle::role(field,"technical");
 // Trailing actions share their field's line box, including enlarged text.
 for(auto* row:findChildren<QWidget*>())if(row->property("visualRole")=="settingRow")for(auto* button:row->findChildren<QPushButton*>()){auto policy=button->sizePolicy();policy.setVerticalPolicy(QSizePolicy::Preferred);button->setSizePolicy(policy);button->setMinimumHeight(Graphite::scaled(Graphite::Metric::control));}
 const auto sharedForms=findChildren<QFormLayout*>();for(int page=0;page<tabs_->count();++page)if(auto* scroll=qobject_cast<QScrollArea*>(tabs_->widget(page)))Graphite::alignForms(scroll->widget(),sharedForms);Graphite::alignForms(destinationBody,sharedForms);
 connect(&settings_,&UiSession::accepted,this,[this]{configurationChanged();refreshEffectiveLimits();});connect(&settings_,&UiSession::statusChanged,this,[this]{configurationChanged();refreshDiagnostics();});connect(tabs_,&QTabWidget::currentChanged,this,&SettingsWindow::viewStateChanged);
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
 if(watched==tabs_->tabBar()&&event->type()==QEvent::Wheel){event->ignore();return true;}
 if(revealUpdates_&&(event->type()==QEvent::Move||event->type()==QEvent::Resize)&&watched==updatesCard_)revealUpdates();
 if(event->type()==QEvent::Hide&&watched==generalPage_&&!event->spontaneous())revealUpdates_=false;
 return RetainedWindow::eventFilter(watched,event);
}
void SettingsWindow::refreshMetadata(){if(configPath_)configPath_->setText(settings_.path());metadataText_="Configuration: "+settings_.path()+"\nAccepted revision: "+settings_.revision()+"\nObserved file revision: "+settings_.metadata()["observedRevision"].toString();
  // Paths and 64-digit revisions may break at a slash or every 16 digits, so a
  // narrow page wraps them instead of growing; the accessible name stays exact.
  static_cast<MetadataLabel*>(metadata_)->setExact(metadataText_);}
void SettingsWindow::dirty(){if(loading_)return;dirty_=true;emit viewStateChanged();}
void SettingsWindow::status(const Error& error){
 auto e=error;
 if(e.code=="s3_settings_invalid"&&providerType_->currentData()=="s3"){
  const auto bucket=s3_["bucket"]->text();QString reason;
  if(bucket.isEmpty())reason="Enter a bucket name to finish setting up this destination.";
  else if(bucket.contains('/')||bucket.contains(':')||bucket.contains(' '))reason="Enter only the bucket name, without a URL, folder path or spaces.";
  else if(bucket.toUtf8().size()<3||bucket.toUtf8().size()>63)reason="Bucket names must contain 3–63 characters.";
  if(!reason.isEmpty()){e.message="Invalid S3 settings: "+reason;inlineError(s3_["bucket"],reason);}
 }
 // People read the message; the code and path go to the tooltip, the
 // accessible description and errorCode, for a report or an agent.
 const auto detail=e?(e.code+(e.details["path"].toString().isEmpty()?QString{}:" · "+e.details["path"].toString())):QString{};
 for(auto* label:{status_,destinationStatus_}){if(!label)continue;label->setProperty("errorCode",e?e.code:QString{});label->setToolTip(detail);label->setAccessibleDescription(detail);}
 if(destinationStatus_)VisualStyle::feedback(destinationStatus_,e.message,bool(e));
 if(e.code=="config_missing"&&settings_.revision().isEmpty()){VisualStyle::feedback(status_,"Changes save automatically when valid.");return;}
 VisualStyle::feedback(status_,e?e.message:"Done.",bool(e));
}
void SettingsWindow::refreshDiagnostics(){refreshLoggingStatus();refreshUpdateStatus();auto data=settings_.diagnostics();capabilities_->setText(QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Indented)));shortcuts_->setStatus(data["shortcuts"].toObject());}
void SettingsWindow::refresh(bool preserveDrafts){
 if(preserveDrafts)rememberCredentialInputs();else{clearCredentialInputs();profileEditors_={};provisionalProfile_={};}
 autosaveBlocked_=false;for(auto* button:retryAutosave_)button->hide();reorderBaseline_.clear();
 loading_=true;draft_=settings_.document().isEmpty()?Settings::defaults():settings_.document();baseDocument_=draft_;baseRevision_=settings_.revision();observed_=settings_.metadata()["observedRevision"].toString();dirty_=conflict_=false;refreshMetadata();
 VisualStyle::feedback(plain(status_),{});plain(destinationStatus_);showConflictActions();
 // Destinations are created only by an explicit Add action. Keep the legacy
 // first-launch draft flag only for restoring an older UI checkpoint.
 initialProfile_=false;
 populate();for(auto* field:errors_.keys())inlineError(field,{});refreshDestinationList();refreshCredentialHint();
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
 bool exceptions=false;for(auto* inherit:useCaptureDefault_)exceptions|=!inherit->isChecked();captureExceptions_->setChecked(exceptions);
 shortcuts_->setActions(actions);auto archive=draft_["archive"].toObject();root_->setText(archive["root"].toString());keep_->setChecked(archive["keepUploads"].toBool());for(auto it=folders_.begin();it!=folders_.end();++it)it.value()->setText(archive["folders"].toObject()[it.key()].toString());if(auto* toggle=findChild<QToolButton*>("categoryFoldersToggle")){bool overrides=false;for(auto* field:folders_)overrides|=!field->text().isEmpty();toggle->setChecked(overrides);}
 naming_->setCurrentIndex(naming_->findData(draft_["naming"].toObject()["mode"].toString()));const auto effective=Settings::fromValidated(draft_);const QHash<QString,QString> formats{{"fileTemplate",effective.fileTemplate},{"captureTemplate",effective.captureTemplate},{"imageTemplate",effective.imageTemplate},{"textTemplate",effective.textTemplate}};for(auto it=templates_.begin();it!=templates_.end();++it)it.value()->setText(formats.value(it.key()));collision_->setCurrentIndex(collision_->findData(draft_["naming"].toObject()["collisionPolicy"].toString()));auto prefs=draft_["preferences"].toObject();uploadClipboard_->setCurrentIndex(uploadClipboard_->findData(effective.uploadClipboardBehavior));appearance_->setCurrentIndex(appearance_->findData(effective.appearanceTheme));notifications_->setChecked(prefs["notifications"].toBool());login_->setChecked(prefs["launchAtLogin"].toBool());debugLogging_->setChecked(prefs["diagnostics"].toObject()["debugLogging"].toBool());unlockAtStartup_->setChecked(effective.unlockCredentialsAtStartup);checkUpdates_->setChecked(effective.checkForUpdates);updateChannelSetting_=effective.updateChannel;updateChannelShown_.clear();showUpdateChannel();
 for(auto it=numbers_.begin();it!=numbers_.end();++it){QJsonValue value=draft_["limits"].toObject().contains(it.key())?draft_["limits"].toObject().value(it.key()):draft_["retention"].toObject().value(it.key());if(it.key()=="pendingImageBytes"&&value.isUndefined())value=effective.limits.pendingImageBytes;it.value()->setText(value.isNull()?QString{}:value.isString()?QStringLiteral("Auto"):ByteSize::exact(value.toInteger(),displayUnit(it.key())));}
 updateCaptureInheritance();loading_=false;updateResourceLimits();
}
void SettingsWindow::refreshEffectiveLimits(){
 settings_.request("settings.get",{},this,[this](Error error,QJsonObject data){if(error)return;effectiveLimits_=data["effectiveLimits"].toObject();updateLimitHints();});
}
// "No limit on this computer · keeps 4 GiB free" below a field set to Auto: none of put.gd's own,
// with the free-disk reserve that bounds every import.
void SettingsWindow::updateResourceLimits(){
 if(!resourceLimitsToggle_||loading_)return;for(auto it=numbers_.begin();it!=numbers_.end();++it)if(!Settings::defaults()["retention"].toObject().contains(it.key())&&(it.value()->text().trimmed().compare(it.value()->property("defaultText").toString(),Qt::CaseInsensitive)!=0||it.value()->property("invalid").toBool())){resourceLimitsToggle_->setChecked(true);break;}
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
  it.value()->setToolTip(text.isEmpty()?QString{}:QString("On this computer, Auto sets no size limit of put.gd's own. Each upload still needs room on the disk: put.gd keeps %1 free (5% of the disk, between 1 and 4 GiB), and the destination's own maximum object size applies.").arg(ByteSize::summary(qMax<qint64>(reserve,0))));
 }
}
void SettingsWindow::addS3Profile(const QString& defaultLabel){
 storeProfile();draft_=candidate();auto profiles=draft_["profiles"].toArray();if(profiles.size()>=32){status({"profile_limit","At most 32 destinations are supported",{}});return;}
 bool accepted=false;const auto destinationName=QInputDialog::getText(this,"Add destination","Destination name",QLineEdit::Normal,defaultLabel,&accepted).trimmed();if(!accepted||destinationName.isEmpty())return;
 QJsonObject restored=provisionalProfile_.value("profile").toObject();
 QString id=restored.value("id").toString();int number=1;
 if(id.isEmpty()||std::any_of(profiles.begin(),profiles.end(),[&](const QJsonValue& p){return p.toObject()["id"].toString()==id;}))do{auto slug=destinationName.toLower();slug.replace(QRegularExpression("[^a-z0-9]+"),"-");slug=slug.left(48);if(slug.isEmpty()||!slug[0].isLetterOrNumber())slug="destination";id=slug+(number==1?QString{}:"-"+QString::number(number));++number;}while(std::any_of(profiles.begin(),profiles.end(),[&](const QJsonValue& p){return p.toObject()["id"].toString()==id;}));
 while(profileEditors_.size()<profiles.size())profileEditors_.append(QJsonValue::Null);
 credentialDrafts_.resize(profiles.size()+1);
 if(restored.isEmpty()){auto profile=s3Profile(id);profile["label"]=destinationName;profiles.append(profile);profileEditors_.append(QJsonValue::Null);}
 else{
  restored["id"]=id;profiles.append(restored);auto editor=provisionalProfile_["editor"].toObject();editor["id"]=id;profileEditors_.append(editor);
  credentialDrafts_.last()=std::move(provisionalCredentials_);provisionalProfile_={};provisionalCredentials_.clear();
 }
 draft_["profiles"]=profiles;initialProfile_=false;rebuildProfiles(profiles.size()-1);
 dirty();queueEdit(defaultButton_,"/");refreshDestinationList();
}
void SettingsWindow::resetDraft(bool reloadFile){
 if(resetting_||saving_||setupAccepting_)return;
 autosave_->stop();pendingEdits_.clear();pendingEditSerials_.clear();editControls_.clear();clearCredentialInputs();resetting_=true;setEnabled(false);destinationPage_->setEnabled(false);emit viewStateChanged();
 endAppearancePreview();
 const auto finish=[g=QPointer<SettingsWindow>(this),reloadFile](Error error){
  if(!g)return;
  g->resetting_=false;g->setEnabled(true);g->destinationPage_->setEnabled(true);
  if(error){g->configurationChanged();g->status(error);emit g->viewStateChanged();return;}
  g->refresh();emit g->viewStateChanged();
  if(reloadFile)VisualStyle::feedback(plain(g->status_),"Settings reloaded from file.");else g->close();
 };
 if(reloadFile)settings_.reloadAsync([finish](Result result){finish(result.error);});
 else settings_.refreshSnapshot(finish);
}

void SettingsWindow::rebuildProfiles(int selected,const QString& defaultProfile){const auto captureProfile=captureProfile_->currentData().toString();const bool previous=loading_;loading_=true;const auto profiles=draft_["profiles"].toArray();profileIndex_=-1;profile_->clear();for(auto p:profiles)profile_->addItem(p.toObject()["label"].toString(),p.toObject()["id"].toString());profile_->setCurrentIndex(selected);loadProfile();for(auto* combo:actionProfiles_){auto id=combo->currentData().toString();combo->clear();combo->addItem("No destination",QString{});for(auto p:profiles)combo->addItem(p.toObject()["label"].toString(),p.toObject()["id"].toString());int index=combo->findData(id.isEmpty()?defaultProfile:id);combo->setCurrentIndex(index<0?0:index);}captureProfile_->setCurrentIndex(captureProfile_->findData(captureProfile.isEmpty()?defaultProfile:captureProfile));if(!defaultProfile.isEmpty()){captureDefaultsEnabled_=true;captureRoutingEdited_=true;}updateCaptureInheritance();loading_=previous;}
void SettingsWindow::loadProfile(){
 credentialStorageNotice_->hide();if(destinationStatus_&&!conflict_)VisualStyle::feedback(destinationStatus_,{});bool previous=loading_;loading_=true;profileIndex_=profile_->currentIndex();auto profiles=draft_["profiles"].toArray();auto p=profileIndex_>=0&&profileIndex_<profiles.size()?profiles[profileIndex_].toObject():QJsonObject{};profileId_->setText(p["id"].toString());profileLabel_->setText(p["label"].toString());const auto s=p["settings"].toObject();bool s3=p["provider"]=="s3";const bool b2=p["provider"]=="backblaze-b2";const bool storage=s3||b2;
 providerType_->setCurrentIndex(providerType_->findData(p["provider"].toString()));const bool proton=p["provider"]=="proton-drive";protonPanel_->setVisible(proton);protonAccountUid_=s["accountRootUid"].toString();protonAccountEmail_.clear();protonShare_->setChecked(s["shareByDefault"].toBool(true));protonExpiration_->setValue(s["linkExpirationDays"].toInt());protonTimeout_->setValue(s["requestTimeoutMs"].toInt(120000)/1000.0);protonFolder_->clear();addProtonFolder(protonFolder_,s["folderUid"].toString(),s["folderName"].toString());
 findControl<QPushButton*>("storeCredentials")->setVisible(storage);findControl<QPushButton*>("testDestination")->setVisible(storage||proton);access_->setVisible(storage);secret_->setVisible(storage);token_->setVisible(s3);for(auto it=s3_.begin();it!=s3_.end();++it){auto v=s[it.key()];it.value()->setText(v.isDouble()?ByteSize::exact(v.toInteger(),it.key().endsWith("Ms")?1000:1):v.toString());allowControl(it.value(),s3||(b2&&b2SharedKeys().contains(it.key())));}allowControl(b2LinkMode_,b2);allowControl(b2LinkDays_,b2);b2LinkMode_->setCurrentIndex(qMax(0,b2LinkMode_->findData(s["linkMode"].toString("public"))));b2LinkDays_->setValue(s["linkValidityDays"].toInt(b2::maximumLinkDays));s3_["region"]->setProperty("previousRegion",s["region"].toString());allowControl(regionChoices_,s3);allowControl(acl_,s3);allowControl(addressing_,s3);acl_->setCurrentIndex(acl_->findData(s["aclMode"].toString()));addressing_->setCurrentIndex(addressing_->findData(s["addressing"].toString()));allowControl(providerJson_,!storage);providerJson_->setPlainText(proton||b2?QString{}:QString::fromUtf8(QJsonDocument(s).toJson(QJsonDocument::Indented)));auto ref=p["credentials"].toObject();credentialMode_->setCurrentIndex(credentialMode_->findData(ref["mode"].toString()));reference_->setText(ref["reference"].toString());if(profileIndex_>=0&&profileIndex_<profileEditors_.size()&&profileEditors_[profileIndex_].toObject().contains("id"))applyProfileEditor(profileEditors_[profileIndex_].toObject());restoreCredentialInputs();if(updateProviderVisibility_)updateProviderVisibility_();loading_=previous;updateProtonAccountDisplay();updateSetupButton();refreshDestinationList();refreshCredentialHint();}
QJsonObject SettingsWindow::editedProfile()const{auto profiles=draft_["profiles"].toArray();if(profileIndex_<0||profileIndex_>=profiles.size())return {};auto p=profiles[profileIndex_].toObject();p["id"]=profileId_->text();p["label"]=profileLabel_->text();auto s=p["settings"].toObject();if(p["provider"]=="s3"){for(auto it=s3_.begin();it!=s3_.end();++it){if(it.key().endsWith("Ms")||it.key().startsWith("lowSpeed")){auto value=ByteSize::fromDecimal(it.value()->text(),it.key().endsWith("Ms")?1000:1);s[it.key()]=value;}else if(it.key()=="publicUrlBase"&&it.value()->text().isEmpty())s[it.key()]=QJsonValue::Null;else s[it.key()]=it.value()->text();}s["aclMode"]=acl_->currentData().toString();s["addressing"]=addressing_->currentData().toString();}else if(p["provider"]=="proton-drive"){s["accountRootUid"]=protonAccountUid_;s["folderUid"]=protonFolder_->currentData().toString();const auto folderName=protonFolder_->currentData(protonFolderNameRole).toString();if(folderName.isEmpty())s.remove("folderName");else s["folderName"]=folderName;s["shareByDefault"]=protonShare_->isChecked();s["linkExpirationDays"]=protonExpiration_->hasAcceptableInput()?QJsonValue(protonExpiration_->value()):QJsonValue(protonExpiration_->text());s["requestTimeoutMs"]=protonTimeout_->hasAcceptableInput()?QJsonValue(qRound64(protonTimeout_->value()*1000)):QJsonValue(protonTimeout_->text());}else if(p["provider"]=="backblaze-b2"){for(const auto& key:b2SharedKeys()){auto* field=s3_.value(key);if(key.endsWith("Ms"))s[key]=ByteSize::fromDecimal(field->text(),1000);else if(key=="publicUrlBase"&&field->text().isEmpty())s[key]=QJsonValue::Null;else s[key]=field->text();}s["linkMode"]=b2LinkMode_->currentData().toString();s["linkValidityDays"]=b2LinkDays_->value();if(s["linkMode"]=="authorized")s["publicUrlBase"]=QJsonValue::Null;}else {auto parsed=QJsonDocument::fromJson(providerJson_->toPlainText().toUtf8());if(parsed.isObject())s=parsed.object();else s={{"invalidJson",true}};}p["settings"]=s;p["credentials"]=QJsonObject{{"mode",credentialMode_->currentData().toString()},{"reference",credentialMode_->currentData().toString()=="session"?QJsonValue(QJsonValue::Null):QJsonValue(reference_->text())}};return p;}
void SettingsWindow::storeProfile(){
 auto profiles=draft_["profiles"].toArray();if(profileIndex_<0||profileIndex_>=profiles.size())return;
 while(profileEditors_.size()<profiles.size())profileEditors_.append(QJsonValue::Null);
 rememberCredentialInputs();profileEditors_[profileIndex_]=retainedProfileEditor();
 const auto p=editedProfile();
 for(auto* combo:actionProfiles_){const int index=profileIndex_+1;if(index<combo->count()){combo->setItemData(index,p["id"].toString());combo->setItemText(index,p["label"].toString());}}
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
 const QHash<QString,QString> defaultTemplates{{"fileTemplate",effective.fileTemplate},{"captureTemplate",effective.captureTemplate},{"imageTemplate",effective.imageTemplate},{"textTemplate",effective.textTemplate}};
 for(auto it=templates_.begin();it!=templates_.end();++it)if(naming.contains(it.key())||it.value()->text()!=defaultTemplates.value(it.key()))naming[it.key()]=it.value()->text();
 doc["naming"]=naming;auto preferences=doc["preferences"].toObject();preferences["notifications"]=notifications_->isChecked();preferences["launchAtLogin"]=login_->isChecked();if(preferences.contains("unlockCredentialsAtStartup")||!unlockAtStartup_->isChecked())preferences["unlockCredentialsAtStartup"]=unlockAtStartup_->isChecked();
 if(preferences.contains("uploadClipboardBehavior")||uploadClipboard_->currentData()!="each_link")preferences["uploadClipboardBehavior"]=uploadClipboard_->currentData().toString();
 // An untouched config keeps no appearance object; it reads as "system".
 if(preferences.contains("appearance")||appearance_->currentData()!="system"){auto appearance=preferences["appearance"].toObject();appearance["theme"]=appearance_->currentData().toString();preferences["appearance"]=appearance;}
 // An untouched config keeps no updates object; it reads as a daily check on this build's channel.
 // The stored value stays until the choice shown is changed; then the build's
 // own channel is stored as "build", so a DEB and an AppImage that share this
 // file each keep following their own (#344), and the other one by name.
 const auto chosen=updateChannel_->currentData().toString();const bool changed=!updateChannelShown_.isEmpty()&&chosen!=updateChannelShown_;
 const auto channel=!changed?updateChannelSetting_:chosen==buildUpdateChannel()?QString("build"):chosen;
 if(preferences.contains("updates")||!checkUpdates_->isChecked()||channel!="build"){auto updates=preferences["updates"].toObject();updates["check"]=checkUpdates_->isChecked();updates["channel"]=channel;preferences["updates"]=updates;}
 // debugLogging is always written with the object; packageFolder stays as the draft holds it.
 if(preferences.contains("diagnostics")||debugLogging_->isChecked()){auto diagnostics=preferences["diagnostics"].toObject();diagnostics["debugLogging"]=debugLogging_->isChecked();preferences["diagnostics"]=diagnostics;}
 doc["preferences"]=preferences;auto limits=doc["limits"].toObject(),retention=doc["retention"].toObject();
 for(auto it=numbers_.begin();it!=numbers_.end();++it){
  bool ok=false;const auto n=it.value()->text().toLongLong(&ok);QJsonValue value=displayUnit(it.key())!=1?ByteSize::fromDecimal(it.value()->text(),displayUnit(it.key())):(ok?QJsonValue(n):QJsonValue(it.value()->text()));
  if(it.key()=="historyRetentionDays"&&it.value()->text().isEmpty())value=QJsonValue::Null;
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
QJsonValue atPath(QJsonValue value,const QStringList& path){for(const auto& part:path)value=value.isArray()?value.toArray().at(part.toInt()):value.toObject().value(part);return value;}
QJsonValue replacePath(QJsonValue value,const QStringList& path,int index,const QJsonValue& replacement){if(index==path.size())return replacement;if(value.isArray()){auto array=value.toArray();const int row=path[index].toInt();if(row<0||row>=array.size())return value;array[row]=replacePath(array[row],path,index+1,replacement);return array;}auto object=value.toObject();object[path[index]]=replacePath(object[path[index]],path,index+1,replacement);return object;}
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
 rebuildProfiles(selectedIndex);refreshDestinationList();
}
Error SettingsWindow::moveDestination(int from,int to){
 auto order=profileOrder();
 if(from<0||to<0||from>=order.size()||to>=order.size()||from==to)return {"destination_move_invalid","That destination can't move further in this direction.",{{"from",from},{"to",to}}};
 if(resetting_||!autosave_)return {"ui_busy","Settings are reloading. Try again when they finish.",{}};
 if(conflict_)return {"config_conflict","Settings changed elsewhere — reload to continue.",{}};
 if(setupAccepting_||!setupJobs_.isEmpty())return {"setup_pending","Wait for destination setup to finish before reordering destinations.",{}};
 if(reorderBaseline_.isEmpty())reorderBaseline_=order;
 order.move(from,to);applyProfileOrder(order);
 // Save only the new order of what is accepted or already queued, so an edit
 // that was rejected earlier cannot block the reorder. Index-addressed edits
 // are folded in; the "/profiles" edit supersedes them.
 auto projected=saving_?savingDocument_:settings_.document().isEmpty()?Settings::defaults():settings_.document();
 for(auto it=pendingEdits_.cbegin();it!=pendingEdits_.cend();++it)projected=replacePath(projected,it.key().split('/',Qt::SkipEmptyParts),0,it.value()).toObject();
 QJsonObject reordered;QJsonArray value=Settings::reorderProfiles(projected,QJsonArray::fromStringList(order),reordered)?candidate()["profiles"].toArray():reordered["profiles"].toArray();
 for(auto it=pendingEdits_.begin();it!=pendingEdits_.end();){if(it.key()=="/profiles"||it.key().startsWith("/profiles/")){editControls_.remove(it.key());pendingEditSerials_.remove(it.key());it=pendingEdits_.erase(it);}else ++it;}
 stageEdit("/profiles",value,destinationList_);dirty();
 if(saving_)autosave_->start();else save();
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
 updateChannel_->setEnabled(!repository);Graphite::setLocked(updateChannel_,repository);updateChannelSource_->setVisible(repository);
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
 // The choice shows this build's channel until it is changed (#344).
 showUpdateChannel();
 // A build that never checks has nothing to choose: only the reason shows.
 const bool development=settings_.build()["version"].toString().contains("-dev");for(auto* row:updateRows_)updatesForm_->setRowVisible(row,supported||(update.isEmpty()&&!development));
 if(auto* row=qobject_cast<QHBoxLayout*>(updateStatus_->parentWidget()->layout())){row->setStretch(0,supported?0:1);row->setStretch(2,supported?1:0);}
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
 if(update.isEmpty()){VisualStyle::feedback(updateStatus_,settings_.build()["version"].toString().contains("-dev")?"Update checks aren't available for development builds":"Update status isn't available yet.");return;}
 if(!supported){
  static const QHash<QString,QString> reasons{
   {"flatpak","Flatpak keeps this installation up to date. Use your software manager, or run flatpak update."},
   {"development_build","Update checks aren't available for development builds"},
   {"no_package","This build wasn't made by a put.gd package (for example, a build from source), so it doesn't check for updates."},
   {"disabled_at_build","Update checks were turned off when this build was made, for example by a distribution's own package."}};
  VisualStyle::feedback(updateStatus_,reasons.value(update["reason"].toString(),"This build doesn't check for updates."));return;
 }
 const auto channel=update["channel"].toString();const auto current=build["version"].toString();
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
 if(available||!offer.isEmpty())VisualStyle::success(updateStatus_,text);else VisualStyle::feedback(updateStatus_,text);
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
 VisualStyle::role(downloadUpdate_,installable?"secondary":"primary");
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
  VisualStyle::feedback(installStatus_,text);return;
 }
 // A switch says so (#344): the record names both channels.
 const auto from=installed["previousChannel"].toString(),to=installed["channel"].toString();
 const bool switched=!from.isEmpty()&&!to.isEmpty()&&from!=to;
 if(restart){
  const auto how=installed["method"].toString();
  QString text=(switched?"Switched to "+to+" ":QString("Installed "))+installed["version"].toString()+(how=="zsync"?QString(" with a zsync update"):how=="download"?QString(" from a full download"):QString())+". Restart put.gd to use it.";
  if(install["backup"].isString())text+=" The previous version is kept as "+QFileInfo(install["backup"].toString()).fileName()+" until the new one has started.";
  VisualStyle::success(installStatus_,text);return;
 }
 const auto error=install["error"].toObject();
 if(state!="failed"&&updated){
  const auto how=installed["method"].toString();
  VisualStyle::success(installStatus_,(switched?"Switched from "+from+" "+installed["previousVersion"].toString()+" to "+to+" ":"Updated from "+installed["previousVersion"].toString()+" to ")+installed["version"].toString()+
   (how=="zsync"?QString(" with a zsync update"):how=="download"?QString(" from a full download"):QString())+" on "+localTime(installed["installedAt"].toString())+".");
  return;
 }
 VisualStyle::feedback(installStatus_,"The update wasn't installed: "+error["message"].toString()+".",true);
}
void SettingsWindow::checkForUpdates(std::function<void(Error,QJsonObject)> done){
 checkNow_->setEnabled(false);VisualStyle::feedback(updateStatus_,"Checking…");
 settings_.request("update.check",{},this,[g=QPointer<SettingsWindow>(this),done](Error error,QJsonObject result){
  if(!g)return;
  g->settings_.refreshSnapshot([g,done,error,result](Error){if(!g)return;g->refreshUpdateStatus();if(done)done(error,result);});
 });
}
void SettingsWindow::refreshLoggingStatus(){
 const auto data=settings_.diagnostics();const auto logging=data["logging"].toObject();
 const auto expires=logging["debugLoggingExpiresAt"].toString();
 debugLoggingStatus_->setText(logging["debugLogging"].toBool()&&!expires.isEmpty()?"On. Turns itself off at "+localTime(expires)+".":debugLogging_->isChecked()?QString("On. Turns itself off 24 hours after it was turned on."):QString("Off. Turn it on to record detailed logs for a maintainer; it turns itself off after 24 hours."));
 if(!logging["available"].toBool(true)||logging.isEmpty())logSummary_->setText(logging.isEmpty()?QString("Log status isn't available yet."):"The log folder isn't available: "+logging["directory"].toString());
 else{
  const int files=logging["files"].toInt();const auto oldest=logging["oldestRecord"].toString();const auto retention=logging["retention"].toObject();
  logSummary_->setText((files==1?QString("1 file"):QString("%1 files").arg(files))+" · "+ByteSize::summary(logging["bytes"].toInteger())+(oldest.isEmpty()?QString{}:" · oldest record "+localTime(oldest))+
   (retention.isEmpty()?QString{}:QString(" · kept %1 days, up to %2").arg(retention["days"].toInt()).arg(ByteSize::summary(retention["bytes"].toInteger()))));
 }
 logSummary_->setToolTip(logging["directory"].toString());
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
  VisualStyle::role(uncleanExit_,last["cause"]=="signal"?"muted":"warning");
 }
}
void SettingsWindow::clearLogs(std::function<void(Error,QJsonObject)> done){
 clearLogs_->setEnabled(false);
 settings_.request("logs.clear",{},this,[g=QPointer<SettingsWindow>(this),done](Error error,QJsonObject result){
  if(g){
   g->clearLogs_->setEnabled(true);
   if(error)g->status(error);else VisualStyle::feedback(plain(g->status_),QString("Logs cleared: %1 files, %2 removed.").arg(result["removedFiles"].toInt()).arg(ByteSize::summary(result["removedBytes"].toInteger())));
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
  {"autosave",autosaveText()}};
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
QPixmap SettingsWindow::destinationDragImage(int row,QPoint* hotSpot)const{if(hotSpot)*hotSpot=-DestinationList::dragOffset;return row>=0&&row<destinationList_->count()?DestinationList::dragImage(*destinationList_->item(row),destinationList_->font(),destinationList_->devicePixelRatioF()):QPixmap{};}
QStringList SettingsWindow::destinationRowLines(int row)const{return row>=0&&row<destinationList_->count()?static_cast<DestinationList*>(destinationList_)->shownLines(row):QStringList{};}
Error SettingsWindow::moveSelectedDestination(int delta,const QString& profileId,QJsonObject& result){
 if(!profileId.isEmpty()){const int index=profileOrder().indexOf(profileId);if(index<0)return {"profile_not_found","No destination has that ID.",{{"profileId",profileId}}};if(index!=profile_->currentIndex())profile_->setCurrentIndex(index);}
 const int from=profile_->currentIndex();if(from<0)return {"destination_not_selected","Select a destination first.",{}};
 if(auto error=moveDestination(from,from+delta))return error;
 result={{"selected",profileId_->text()},{"order",QJsonArray::fromStringList(profileOrder())},{"saving",saving_||pendingEdits_.contains("/profiles")}};return {};
}
void SettingsWindow::restoreReorderBaseline(){
 if(reorderBaseline_.isEmpty())return;
 const auto order=std::exchange(reorderBaseline_,{});
 pendingEdits_.remove("/profiles");pendingEditSerials_.remove("/profiles");editControls_.remove("/profiles");
 applyProfileOrder(order);
}
qint64 SettingsWindow::displayUnit(const QString& key){return key=="imagePixels"?1000000:key=="maxFileBytes"||key=="maxBatchBytes"||key=="pendingArtifactBytes"?ByteSize::GiB:key.endsWith("Bytes")?ByteSize::MiB:1;}
void SettingsWindow::inlineError(QWidget* control,const QString& message){if(!control)return;control->setProperty("invalid",!message.isEmpty());if(!message.isEmpty())updateResourceLimits();control->style()->unpolish(control);control->style()->polish(control);if(auto* label=errors_.value(control)){VisualStyle::role(label,"error");label->setText(message);label->setVisible(!message.isEmpty());}const auto description=control->property("settingDescription").isValid()?control->property("settingDescription").toString():control->toolTip();control->setAccessibleDescription(message.isEmpty()?description:description+"\n"+message);}
bool SettingsWindow::editChangedSinceSubmission(const QString& path)const{
 return pendingEdits_.contains(path)&&(!saving_||!submittedEditSerials_.contains(path)||pendingEditSerials_.value(path)!=submittedEditSerials_.value(path));
}
void SettingsWindow::stageEdit(const QString& path,const QJsonValue& value,QWidget* control){
 pendingEdits_[path]=value;editControls_[path]=control;pendingEditSerials_[path]=++editSerial_;
}
void SettingsWindow::queueEdit(QWidget* control,QString path){
 if(loading_||resetting_||!autosave_)return;
 if(path.isEmpty())path=control->property("configPath").toString();
 if(path.isEmpty())return;
 if(path.startsWith("/actions/")&&!path.endsWith("/shortcut")){
  const auto action=path.split('/')[2];if(useCaptureDefault_.contains(action)){const QSignalBlocker block(useCaptureDefault_[action]);useCaptureDefault_[action]->setChecked(false);queueCaptureEdit(control);return;}
 }
 path.replace("{profile}",QString::number(profileIndex_));if(path.contains("/-1"))return;if(path.endsWith("/settings/region"))path=path.left(path.lastIndexOf('/'));
 const auto doc=candidate();
 if(path.startsWith("/profiles")){
  const auto proposed=doc["profiles"].toArray(),accepted=settings_.document()["profiles"].toArray();
  if(proposed.size()!=accepted.size()||initialProfile_)path="/";
  else if(profileIndex_>=0&&profileIndex_<proposed.size()){
   const auto draft=proposed[profileIndex_].toObject(),saved=accepted[profileIndex_].toObject();
   if(draft["provider"]!=saved["provider"]||draft["id"]!=saved["id"])path="/profiles/"+QString::number(profileIndex_);
  }
 }
 for(auto it=pendingEdits_.begin();it!=pendingEdits_.end();){if(path=="/"||it.key().startsWith(path+"/")){editControls_.remove(it.key());pendingEditSerials_.remove(it.key());it=pendingEdits_.erase(it);}else ++it;}
 const auto parts=path.split('/',Qt::SkipEmptyParts);
 auto baseline=saving_?savingDocument_:settings_.document().isEmpty()?Settings::defaults():settings_.document();
 for(auto it=pendingEdits_.cbegin();it!=pendingEdits_.cend();++it)if(it.key()!=path&&editChangedSinceSubmission(it.key())&&(it.key()=="/"||path.startsWith(it.key()+"/")))baseline=replacePath(baseline,it.key().split('/',Qt::SkipEmptyParts),0,it.value()).toObject();
 if(atPath(doc,parts)==atPath(baseline,parts)){
  pendingEdits_.remove(path);pendingEditSerials_.remove(path);editControls_.remove(path);inlineError(control,{});if(pendingEdits_.isEmpty())autosave_->stop();dirty_=dirty_&&candidate()!=settings_.document();emit viewStateChanged();return;
 }
 stageEdit(path,atPath(doc,parts),control);inlineError(control,{});dirty();autosave_->start();refreshDestinationList(doc);
 if(updateProviderVisibility_)updateProviderVisibility_();
}
void SettingsWindow::updateCaptureInheritance(){
 auto describe=[this](QLabel* label,bool upload){
  const bool local=keep_->isChecked();
  const auto text=upload?(local?QString("Uploads to the selected destination and keeps a local copy."):QString("Uploads to the selected destination; no local copy is kept.")):(local?QString("Uploads are off; keeps a local copy in your folders."):QString("New captures have no output. Choose a destination or turn on Keep a local copy."));
  VisualStyle::feedback(label,text,!upload&&!local);
 };
 const auto defaultLabel=[this](QComboBox* profile){const auto resolved=destinationDefaultSharing(profile->currentData().toString());return resolved.isEmpty()?sharingLabel("default"):QString("Destination default — ")+(resolved=="private"?"private upload":"shared link");};
 captureSharing_->setItemText(captureSharing_->findData("default"),defaultLabel(captureProfile_));
 const bool upload=!captureProfile_->currentData().toString().isEmpty();captureSharing_->setEnabled(upload);describe(captureOutputNotice_,upload);
 // The default's summary only repeats the controls around it; it shows while
 // there is no output, and its actions add one.
 const bool blocked=!upload&&!keep_->isChecked()&&!changesUnsaved();captureOutputBlock_->setVisible(blocked);
 if(blocked)VisualStyle::feedback(captureOutputNotice_,"New captures have no output. Choose a destination or turn on Keep a local copy.",true);
 chooseOutput_->setText(captureProfile_->count()>1?"Choose a destination":"Add a destination…");
 for(const auto& name:Settings::presentationOrder()){
  auto *profile=actionProfiles_[name],*sharing=publicationModes_[name];
  // An inherited card stays visible: its disabled controls show the default output.
  const bool inherit=useCaptureDefault_[name]->isChecked();
  if(inherit||(name=="record"&&!recordingRoutingEdited_&&draft_["actions"].toObject()[name].toObject()["destination"]=="default")){const QSignalBlocker a(profile),b(sharing);auto* sourceProfile=inherit&&name=="record"?actionProfiles_["region"]:captureProfile_;auto* sourceSharing=inherit&&name=="record"?publicationModes_["region"]:captureSharing_;profile->setCurrentIndex(profile->findData(sourceProfile->currentData()));sharing->setCurrentIndex(sharing->findData(sourceSharing->currentData()));}
  // Inherited values are locked (readable, quietly framed); sharing without an
  // upload is unavailable.
  const bool actionUpload=!profile->currentData().toString().isEmpty();profile->setEnabled(!inherit);Graphite::setLocked(profile,inherit);sharing->setEnabled(!inherit&&actionUpload);Graphite::setLocked(sharing,inherit&&actionUpload);for(auto* control:{profile,sharing})control->setMinimumHeight(qMax(Graphite::scaled(Graphite::Metric::control),control->sizeHint().height()));
  sharing->setItemText(sharing->findData("default"),defaultLabel(profile));
  const auto inherited=[inherit,name](QComboBox* combo){return inherit?(name=="record"?": inherited from Capture region, ":": inherited from default, ")+combo->currentText():QString{};};
  profile->setAccessibleName(actionLabel(name)+" destination"+inherited(profile));sharing->setAccessibleName(actionLabel(name)+" sharing"+inherited(sharing));
  // The status line feeds view state and agents; the row's summary shows it.
  describe(captureStatus_[name],actionUpload);captureStatus_[name]->hide();
  // Inherited rows defer to the default; errors belong to real overrides.
  // Only an overridden action shows its two fields.
  {const QSignalBlocker block(captureOverride_[name]);captureOverride_[name]->setChecked(!inherit);}captureEditors_[name]->setVisible(!inherit);captureEditors_[name]->layout()->invalidate();captureEditors_[name]->setMinimumHeight(captureEditors_[name]->layout()->minimumSize().height());captureEditors_[name]->updateGeometry();
  bool unsupportedPrivate=false;
  if(actionUpload&&sharing->currentData()=="private")for(const auto& value:draft_["profiles"].toArray())if(value.toObject()["id"].toString()==profile->currentData().toString())unsupportedPrivate=value.toObject()["provider"]!="proton-drive";
  const bool fails=(!actionUpload&&!keep_->isChecked())||unsupportedPrivate;
  // An inherited failure is the default's, explained once under Destination above.
  const bool needsOutput=outputRecoveryActions_.contains(name)&&fails;
  captureSummary_[name]->setProperty("outputNeedsAttention",needsOutput);
  VisualStyle::feedback(captureSummary_[name],inherit?(name=="record"?QString("Uses Capture region output · ")+outputSummary(profile,sharing):fails?QString("Off: no output"):QString("Uses default output")):outputSummary(profile,sharing),!inherit&&fails);
 }
 captureExceptionRows_->invalidate();captureExceptionRows_->parentWidget()->layout()->invalidate();captureExceptionRows_->parentWidget()->updateGeometry();
}
// The output a capture gets, in the status band's words: Shared link or Private
// upload · destination, + local copy; Local only; or why the action fails.
QString SettingsWindow::outputSummary(QComboBox* profile,QComboBox* sharing)const{
 const auto id=profile->currentData().toString();const bool local=keep_->isChecked();
 if(id.isEmpty())return local?QString("Local only"):QString("Off: no output");
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
 if(loading_||resetting_||!autosave_)return;
 if(control==useCaptureDefault_.value("record")||control==actionProfiles_.value("record")||control==publicationModes_.value("record"))recordingRoutingEdited_=true;
 captureDefaultsEnabled_=true;captureRoutingEdited_=true;updateCaptureInheritance();
 if(initialProfile_&&captureProfile_->currentData().toString().isEmpty()&&std::all_of(actionProfiles_.cbegin(),actionProfiles_.cend(),[](auto* combo){return combo->currentData().toString().isEmpty();})){
  // Park unfinished first-launch connection details outside saved routing.
  storeProfile();provisionalProfile_={{"profile",editedProfile()},{"editor",retainedProfileEditor()}};
  if(profileIndex_>=0&&profileIndex_<credentialDrafts_.size())provisionalCredentials_=std::move(credentialDrafts_[profileIndex_]);
  loading_=true;clearCurrentCredentialInputs();draft_["profiles"]=QJsonArray{};profileEditors_={};credentialDrafts_.clear();initialProfile_=false;rebuildProfiles(-1);loading_=false;
 }
 const auto proposed=candidate();
 auto projected=editChangedSinceSubmission("/")?pendingEdits_["/"].toObject():saving_?savingDocument_:settings_.document();if(projected.isEmpty())projected=Settings::defaults();
 projected["captureDefaults"]=proposed["captureDefaults"];
 auto routes=projected["actions"].toObject();for(const auto& name:Settings::presentationOrder()){auto route=routes[name].toObject();const auto proposal=proposed["actions"].toObject()[name].toObject();for(const auto& key:QStringList{"destination","profileId","publicationMode"}){if(proposal.contains(key))route[key]=proposal[key];else route.remove(key);}routes[name]=route;}projected["actions"]=routes;
 for(auto it=pendingEdits_.begin();it!=pendingEdits_.end();){if((it.key().startsWith("/actions")&&!it.key().endsWith("/shortcut"))||it.key().startsWith("/captureDefaults")){editControls_.remove(it.key());pendingEditSerials_.remove(it.key());it=pendingEdits_.erase(it);}else ++it;}
 stageEdit("/",projected,control);inlineError(control,{});dirty();autosave_->start();
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
 VisualStyle::feedback(plain(status_),{});
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
 for(const auto& action:targets)if(actionProfiles_.contains(action)&&!useCaptureDefault_[action]->isChecked()){target=actionProfiles_[action];overriding=true;break;}
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
bool SettingsWindow::flushAutosave(){if(autosave_)autosave_->stop();if(!autosaveBlocked_&&!pendingEdits_.isEmpty()&&!conflict_&&(setupAccepting_||!setupJobs_.isEmpty())){const auto waiting=QStringLiteral("Waiting for destination setup to finish before saving your edits.");if(status_->text()!=waiting||status_->property("feedbackTone").toString()!="neutral")VisualStyle::feedback(plain(status_),waiting);return false;}if(!saving_&&!pendingEdits_.isEmpty()&&!conflict_)save();return !saving_;}
void SettingsWindow::save(){
 if(autosaveBlocked_||conflict_||saving_||resetting_||setupAccepting_||!setupJobs_.isEmpty())return;
 if(pendingEdits_.isEmpty())return;
 storeProfile();auto doc=settings_.document().isEmpty()?Settings::defaults():settings_.document();for(auto it=pendingEdits_.begin();it!=pendingEdits_.end();++it)doc=replacePath(doc,it.key().split('/',Qt::SkipEmptyParts),0,it.value()).toObject();
 if(doc==settings_.document()){pendingEdits_.clear();pendingEditSerials_.clear();editControls_.clear();dirty_=candidate()!=settings_.document();endAppearancePreview();emit viewStateChanged();return;}
 saving_=true;savingDocument_=doc;submittedEditSerials_=pendingEditSerials_;emit viewStateChanged();
 settings_.saveAsync(doc,observed_,[g=QPointer<SettingsWindow>(this)](Result result){
  if(!g)return;
  if(!result){if(result.error.code!="config_invalid"&&result.error.code!="s3_settings_invalid"){g->autosaveFailure(result.error);return;}g->persistNextEdit();return;}
  g->saving_=false;g->autosaveBlocked_=false;for(auto* button:g->retryAutosave_)button->hide();g->baseRevision_=g->settings_.revision();g->baseDocument_=g->settings_.document();g->observed_=g->settings_.metadata()["observedRevision"].toString();if(!g->settings_.document()["profiles"].toArray().isEmpty())g->initialProfile_=false;g->draft_=g->candidate();
  for(auto it=g->submittedEditSerials_.cbegin();it!=g->submittedEditSerials_.cend();++it)if(g->pendingEdits_.contains(it.key())&&g->pendingEditSerials_.value(it.key())==it.value()){g->pendingEdits_.remove(it.key());g->pendingEditSerials_.remove(it.key());g->inlineError(g->editControls_.take(it.key()),{});}
  g->submittedEditSerials_.clear();g->dirty_=g->candidate()!=g->settings_.document();if(!g->pendingEdits_.contains("/profiles"))g->reorderBaseline_.clear();
  if(!g->pendingEdits_.contains(themePath))g->endAppearancePreview();
  if(!g->pendingEdits_.isEmpty())g->autosave_->start();
  else if(!g->dirty_){VisualStyle::feedback(plain(g->status_),{});VisualStyle::feedback(plain(g->destinationStatus_),{});}
  g->refreshMetadata();g->refreshDestinationList();g->refreshCredentialHint();emit g->viewStateChanged();
 });
}
void SettingsWindow::endAppearancePreview(){
 if(!std::exchange(appearancePreviewed_,false))return;
 emit appearancePreview({});
}
void SettingsWindow::autosaveFailure(const Error& error){
 saving_=false;submittedEditSerials_.clear();autosaveBlocked_=true;autosave_->stop();if(error.code=="config_conflict")conflict_=true;
 // The theme shown goes back to the accepted one; the draft keeps the pick for Retry saving.
 endAppearancePreview();
 // A conflicting reorder is not kept: the list goes back to the saved order.
 const bool reordered=conflict_&&!reorderBaseline_.isEmpty();if(reordered)restoreReorderBaseline();
 auto message=error;if(conflict_)message.message="Settings changed elsewhere. Your edits are kept here. Reload discards them."+QString(reordered?" The previous destination order is restored.":"");else message.message+=" Your edits remain in this window. Retry saving when ready; the request won't repeat automatically.";status(message);
 for(auto* button:retryAutosave_)button->setVisible(!conflict_);showConflictActions();
 emit viewStateChanged();
}
void SettingsWindow::persistNextEdit(){
 if(pendingEdits_.isEmpty()){saving_=false;submittedEditSerials_.clear();dirty_=candidate()!=settings_.document();endAppearancePreview();emit viewStateChanged();return;}
 const auto path=pendingEdits_.firstKey();const auto value=pendingEdits_.take(path);const auto serial=pendingEditSerials_.take(path);const QPointer<QWidget> control=editControls_.take(path);const auto accepted=settings_.document().isEmpty()?Settings::defaults():settings_.document();const auto doc=replacePath(accepted,path.split('/',Qt::SkipEmptyParts),0,value).toObject();
 if(doc==accepted){inlineError(control,{});persistNextEdit();return;}
 savingDocument_=doc;submittedEditSerials_={{path,serial}};
 settings_.saveAsync(doc,observed_,[g=QPointer<SettingsWindow>(this),control,path,value](Result result){
  if(!g)return;
  if(!result){if(result.error.code!="config_invalid"&&result.error.code!="s3_settings_invalid"){bool superseded=false;for(auto it=g->pendingEdits_.begin();it!=g->pendingEdits_.end();++it)if(it.key()=="/"||path==it.key()||path.startsWith(it.key()+"/"))superseded=true;if(!superseded){g->stageEdit(path,value,control);}g->autosaveFailure(result.error);return;}g->inlineError(control,result.error.message);g->status(result.error);g->persistNextEdit();return;}
  g->baseRevision_=g->settings_.revision();g->baseDocument_=g->settings_.document();g->observed_=g->settings_.metadata()["observedRevision"].toString();g->draft_=g->candidate();g->inlineError(control,{});if(path=="/profiles"&&!g->pendingEdits_.contains("/profiles"))g->reorderBaseline_.clear();g->refreshMetadata();g->persistNextEdit();
 });
}
void SettingsWindow::refreshCredentialHint(){
 if(!credentialHint_)return;
 credentialHint_->clear();credentialForm_->setRowVisible(credentialHint_,false);access_->setPlaceholderText({});secret_->setPlaceholderText({});token_->setPlaceholderText({});
 if(!QStringList{"s3","backblaze-b2"}.contains(providerType_->currentData().toString())||initialProfile_||profileIndex_<0)return;
 const auto id=profileId_->text();const auto revision=settings_.revision();
 settings_.request("setup.credentialHint",{{"profileId",id},{"expectedRevision",revision}},this,[g=QPointer<SettingsWindow>(this),id,revision](Error error,QJsonObject hint){
  if(!g||g->profileId_->text()!=id||g->settings_.revision()!=revision||!QStringList{"s3","backblaze-b2"}.contains(g->providerType_->currentData().toString()))return;
  if(error)return;
  const bool keychain=hint["mode"]=="keychain";g->credentialAvailability_[id]=hint["available"].toBool()||keychain;g->refreshDestinationList();g->credentialForm_->setRowVisible(g->credentialHint_,true);
  if(hint["available"].toBool()){
   // Only the short identifying prefix, never the key.
   VisualStyle::feedback(g->credentialHint_,hint["accessKeyHint"].toString()+" · Secret key stored"+(hint["tokenStored"].toBool()?QString(" · Session token stored"):QString{})+" · "+(keychain?"In the system keychain":"For this session"));
   g->access_->setPlaceholderText("Type a new key ID to replace it");g->secret_->setPlaceholderText("Type a new secret to replace it");if(hint["tokenStored"].toBool())g->token_->setPlaceholderText("Type a new token to replace it");
  }else VisualStyle::feedback(g->credentialHint_,keychain?"Locked in the system keychain · Unlock saved credentials to identify it":"None stored");
 });
}
void SettingsWindow::refreshDestinationList(const QJsonObject& document){
 if(!destinationList_)return;
 const QSignalBlocker block(destinationList_);const auto doc=document.isEmpty()?candidate():document;const auto profiles=doc["profiles"].toArray();
 findControl<QWidget*>("destinationContent")->setVisible(!profiles.isEmpty());findControl<QWidget*>("destinationScroll")->setVisible(!profiles.isEmpty());findControl<QLabel*>("destinationsEmpty")->setVisible(profiles.isEmpty());
 // Rows are updated in place: rebuilding them here, which runs when a press changes the
 // selection, would drop the view's pressed row, so the drag that follows never starts.
 while(destinationList_->count()>profiles.size())delete destinationList_->takeItem(destinationList_->count()-1);int row=0;const auto actions=doc["actions"].toObject();bool selectedDefault=false;
 for(const auto& value:profiles){const auto profile=value.toObject();bool isDefault=true;if(doc.contains("captureDefaults")){const auto capture=doc["captureDefaults"].toObject();isDefault=capture["destination"]=="upload"&&capture["profileId"]==profile["id"];}else for(const auto& action:Settings::actionNames())if(actions[action].toObject()["profileId"]!=profile["id"]||actions[action].toObject()["destination"]!="upload")isDefault=false;const auto s=profile["settings"].toObject();const bool proton=profile["provider"]=="proton-drive";QString state=proton?(s["accountRootUid"].toString().isEmpty()?"Signed out":s["folderUid"].toString().isEmpty()?"Choose a folder":testedDestinations_.value(profile["id"].toString())==profile?"Connection tested":"Account configured"):(s["bucket"].toString().isEmpty()?"Needs a bucket":credentialAvailability_.contains(profile["id"].toString())&&!credentialAvailability_.value(profile["id"].toString())?"Needs credentials":testedDestinations_.value(profile["id"].toString())==profile?"Connection tested":"Ready to test");if(isDefault)state="Default · "+state;auto* item=row<destinationList_->count()?destinationList_->item(row):new QListWidgetItem(destinationList_);++row;item->setIcon(VisualStyle::icon(proton?"proton":"bucket"));item->setText(profile["label"].toString()+"\n"+state);item->setToolTip(profile["label"].toString()+"\n"+state);if(profile["id"]==profileId_->text()){selectedDefault=isDefault;defaultButton_->setText(testedDestinations_.value(profile["id"].toString())==profile?"Make default":"Make default…");}}
 destinationList_->setCurrentRow(profileIndex_);const bool has=profileIndex_>=0;if(moveUp_){moveUp_->setEnabled(profileIndex_>0);moveDown_->setEnabled(has&&profileIndex_<int(profiles.size())-1);}defaultButton_->setEnabled(has&&!selectedDefault&&!(providerType_->currentData()=="s3"&&s3_["bucket"]->text().isEmpty()));
 // The sidebar already identifies the default; only offer an action for others.
 defaultButton_->setVisible(has&&!selectedDefault);Graphite::setLocked(defaultButton_,false);defaultButton_->setToolTip(selectedDefault?"Captures and clipboard uploads use this destination by default.":"Use this destination for captures and clipboard uploads by default.");
 fitRails();
}
QJsonObject SettingsWindow::profileEditorState()const{
 QJsonObject s3;for(auto it=s3_.begin();it!=s3_.end();++it)s3[it.key()]=it.value()->text();
 QJsonArray folders;for(int i=0;i<protonFolder_->count();++i)folders.append(QJsonObject{{"uid",protonFolder_->itemData(i).toString()},{"name",protonFolder_->itemData(i,protonFolderNameRole).toString()},{"label",protonFolder_->itemText(i)}});
 const QJsonObject proton{{"accountRootUid",protonAccountUid_},{"accountEmail",protonAccountEmail_},{"folders",folders},{"folderIndex",protonFolder_->currentIndex()},{"share",protonShare_->isChecked()},{"expiration",protonExpiration_->findChild<QLineEdit*>()->text()},{"timeout",protonTimeout_->findChild<QLineEdit*>()->text()}};
 return {{"proton",proton},{"id",profileId_->text()},{"label",profileLabel_->text()},{"credentialMode",credentialMode_->currentData().toString()},{"reference",reference_->text()},{"acl",acl_->currentData().toString()},{"addressing",addressing_->currentData().toString()},{"s3",s3},{"providerJson",providerJson_->toPlainText()}};
}
void SettingsWindow::applyProfileEditor(const QJsonObject& state){
 profileId_->setText(state["id"].toString());profileLabel_->setText(state["label"].toString());credentialMode_->setCurrentIndex(credentialMode_->findData(state["credentialMode"].toString()));reference_->setText(state["reference"].toString());acl_->setCurrentIndex(acl_->findData(state["acl"].toString()));addressing_->setCurrentIndex(addressing_->findData(state["addressing"].toString()));const auto s3=state["s3"].toObject();for(auto it=s3_.begin();it!=s3_.end();++it)it.value()->setText(s3[it.key()].toString());s3_["region"]->setProperty("previousRegion",s3_["region"]->text());providerJson_->setPlainText(state["providerJson"].toString());
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
 QJsonObject numbers;for(auto it=numbers_.begin();it!=numbers_.end();++it)numbers[it.key()]=it.value()->text();
 auto editors=profileEditors_;const auto count=draft_["profiles"].toArray().size();while(editors.size()<count)editors.append(QJsonValue::Null);if(profileIndex_>=0&&profileIndex_<count)editors[profileIndex_]=retainedProfileEditor();
 QJsonObject state{{"draft",candidate()},{"numbers",numbers},{"profileEditors",editors},{"baseRevision",baseRevision_},{"observedRevision",observed_},{"dirty",dirty_},{"autosaveBlocked",autosaveBlocked_},{"conflict",conflict_},{"initialProfile",initialProfile_},{"profileIndex",profile_->currentIndex()},{"section",tabs_->currentIndex()},{"advanced",findControl<QToolButton*>("advancedSettings")->isChecked()},{"geometry",QJsonArray{x(),y(),width(),height()}},{"setupJobs",setupJobs_},{"provisionalProfile",provisionalProfile_},{"captureRouting",captureRoutingState()}};
 // Do not even export unclassified provider text to the resident. A content-free
 // rejection keeps the actual draft in this window until the user corrects it.
 if(auto error=validateUiState({{"schemaVersion",1},{"settings",state}}))return {{"stateError",error.code}};
 return state;
}
bool SettingsWindow::restoreViewState(const QJsonObject& state){
 if(validateUiState({{"schemaVersion",1},{"settings",state}}))return false;
 QSignalBlocker stateSignals(this);protonBrowserOpened_=false;pendingProtonUrl_.clear();pendingProtonSetup_.clear();protonBrowserOpening_=false;protonBrowserRetry_->hide();clearCredentialInputs();loading_=true;draft_=state["draft"].toObject();profileEditors_=state["profileEditors"].toArray();provisionalProfile_=state["provisionalProfile"].toObject();baseRevision_=state["baseRevision"].toString();observed_=state["observedRevision"].toString();initialProfile_=state["initialProfile"].toBool();
 populate();loading_=true;profile_->setCurrentIndex(state["profileIndex"].toInt(-1));loadProfile();const auto numbers=state["numbers"].toObject();for(auto it=numbers_.begin();it!=numbers_.end();++it)it.value()->setText(numbers[it.key()].toString());
 findControl<QToolButton*>("advancedSettings")->setChecked(state["advanced"].toBool());tabs_->setCurrentIndex(state["section"].toInt());const auto box=state["geometry"].toArray();setGeometry(box[0].toInt(),box[1].toInt(),box[2].toInt(),box[3].toInt());
 dirty_=state["dirty"].toBool();autosaveBlocked_=state["autosaveBlocked"].toBool();for(auto* button:retryAutosave_)button->setVisible(autosaveBlocked_&&!state["conflict"].toBool());conflict_=state["conflict"].toBool();baseDocument_=settings_.document();showConflictActions();loading_=false;updateResourceLimits();setupJobs_=state["setupJobs"].toArray();updateSetupButton();if(!setupJobs_.isEmpty()){setupTimer_->start();pollSetup();}
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
  if(selected=="proton-drive"){
   profile["settings"]=ProtonProvider::defaults();
   profile["credentials"]=QJsonObject{{"mode","keychain"},{"reference","putgd/proton/"+QUuid::createUuid().toString(QUuid::Id128)}};
  }else if(selected=="backblaze-b2"){
   profile["settings"]=b2::defaults();
   profile["credentials"]=QJsonObject{{"mode","keychain"},{"reference","putgd/b2/"+profile["id"].toString()}};
  }else{
   const auto defaults=s3Profile(profile["id"].toString());profile["settings"]=defaults["settings"];profile["credentials"]=defaults["credentials"];
  }
 }else{
  profile["settings"]=retained["settings"];profile["credentials"]=retained["credentials"];
 }
 // ID and name belong to the destination; raw provider fields belong to its service.
 auto editor=retained["editor"].toObject();
 if(!editor.isEmpty()){editor["id"]=profile["id"];editor["label"]=profile["label"];}
 editor["providers"]=providers;profileEditors_[profileIndex_]=editor;
 profiles[profileIndex_]=profile;draft_["profiles"]=profiles;loadProfile();dirty();queueEdit(providerType_,"/profiles/{profile}");
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
 if(showSecret_){const QSignalBlocker block(showSecret_);showSecret_->setChecked(false);showSecret_->setText("Show");showSecret_->setAccessibleName("Show "+secret_->accessibleName().toLower());}
}
void SettingsWindow::toggleCredentialSecret(bool shown){
 if(!shown){resetCredentialSecretDisplay();return;}
 showSecret_->setText("Hide");showSecret_->setAccessibleName("Hide "+secret_->accessibleName().toLower());
 if(!secret_->text().isEmpty()){secret_->setEchoMode(QLineEdit::Normal);return;}
 if(providerType_->currentData()!="s3"||selectedDestinationChanged()||saving_||conflict_||initialProfile_){
  resetCredentialSecretDisplay();status({"save_first","Save valid destination settings before showing credentials",{}});return;
 }
 const auto serial=++secretRevealSerial_;const auto id=profileId_->text(),revision=settings_.revision();
 secret_->setReadOnly(true);
 settings_.request("setup.revealCredentialSecret",{{"profileId",id},{"expectedRevision",revision}},this,
  [this,serial,id,revision](Error error,QJsonObject value){
   if(serial!=secretRevealSerial_)return;
   if(profileId_->text()!=id||settings_.revision()!=revision||providerType_->currentData()!="s3"||selectedDestinationChanged()){
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
 credentialDrafts_[profileIndex_][profiles[profileIndex_].toObject()["provider"].toString()]={access_->text(),secret_->text(),token_->text(),protonPassword_->text()};
}
void SettingsWindow::restoreCredentialInputs(){
 resetCredentialSecretDisplay();
 const auto profiles=draft_["profiles"].toArray();
 CredentialInputs inputs;
 if(profileIndex_>=0&&profileIndex_<profiles.size()&&profileIndex_<credentialDrafts_.size())
  inputs=credentialDrafts_[profileIndex_].value(profiles[profileIndex_].toObject()["provider"].toString());
 access_->setText(inputs.access);secret_->setText(inputs.secret);token_->setText(inputs.token);protonPassword_->setText(inputs.password);
}
void SettingsWindow::clearCurrentCredentialInputs(){
 resetCredentialSecretDisplay();
 const auto profiles=draft_["profiles"].toArray();
 if(profileIndex_>=0&&profileIndex_<profiles.size()&&profileIndex_<credentialDrafts_.size())
  credentialDrafts_[profileIndex_].remove(profiles[profileIndex_].toObject()["provider"].toString());
 access_->clear();secret_->clear();token_->clear();protonPassword_->clear();
}
void SettingsWindow::importDevelopmentProfile(){
 if(initialProfile_&&s3_["bucket"]->text().isEmpty()){
  draft_["profiles"]=QJsonArray{};profileEditors_={};credentialDrafts_.clear();profileIndex_=-1;for(auto* combo:actionProfiles_)combo->setCurrentIndex(0);
 }else storeProfile();
 draft_=candidate();auto profiles=draft_["profiles"].toArray();const auto development=Settings::developmentTemplate()["profiles"].toArray().first();
 int index=-1;for(int i=0;i<profiles.size();++i)if(profiles[i].toObject()["id"]=="development-s3")index=i;
 if(index<0){index=profiles.size();profiles.append(development);}else profiles[index]=development;
 while(profileEditors_.size()<profiles.size())profileEditors_.append(QJsonValue::Null);
 profileEditors_[index]=QJsonValue::Null;credentialDrafts_.resize(profiles.size());credentialDrafts_[index].clear();
 draft_["profiles"]=profiles;initialProfile_=false;rebuildProfiles(index,"development-s3");dirty();queueEdit(defaultButton_,"/");refreshDestinationList();
}

void SettingsWindow::clearCredentialInputs(){
 resetCredentialSecretDisplay();
 credentialDrafts_.clear();provisionalCredentials_.clear();
 if(protonPassword_)protonPassword_->clear();
 access_->clear();secret_->clear();token_->clear();
}
void SettingsWindow::closeEvent(QCloseEvent* event){clearCredentialInputs();RetainedWindow::closeEvent(event);}
Error SettingsWindow::actionSaveError()const{
 if(!conflict_&&!autosaveBlocked_&&!dirty_)return {};
 const auto message=status_->property("feedbackTone")=="error"&&!status_->text().isEmpty()?status_->text():QStringLiteral("The latest settings couldn’t be saved. The draft is still open.");
 return {"settings_not_saved",message,{}};
}
bool SettingsWindow::hasCredentialInputs()const{
 if(!access_->text().isEmpty()||(!savedSecretShown_&&!secret_->text().isEmpty())||!token_->text().isEmpty()||!protonPassword_->text().isEmpty())return true;
 for(int index=0;index<credentialDrafts_.size();++index){
  const auto& destination=credentialDrafts_[index];
  for(auto it=destination.cbegin();it!=destination.cend();++it){
   if(index==profileIndex_&&it.key()==providerType_->currentData().toString())continue;
   const auto& inputs=it.value();
   if(!inputs.access.isEmpty()||!inputs.secret.isEmpty()||!inputs.token.isEmpty()||!inputs.password.isEmpty())return true;
  }
 }
 for(const auto& inputs:provisionalCredentials_)
  if(!inputs.access.isEmpty()||!inputs.secret.isEmpty()||!inputs.token.isEmpty()||!inputs.password.isEmpty())return true;
 return false;
}
bool SettingsWindow::hasRetainedDrafts()const{
 if(!provisionalProfile_.isEmpty()||hasCredentialInputs())return true;
 for(const auto& editor:profileEditors_)if(editor.toObject()["providers"].toObject().size()>1)return true;
 return false;
}
void SettingsWindow::configurationChanged(){
 if(baseRevision_!=settings_.revision()||observed_!=settings_.metadata()["observedRevision"].toString())resetCredentialSecretDisplay();
 refreshMetadata();if(saving_||resetting_)return;
 if(conflict_||((dirty_||hasRetainedDrafts())&&(baseRevision_!=settings_.revision()||observed_!=settings_.metadata()["observedRevision"].toString()))){
  // Keep a conflict message that is already shown, such as a restored destination order.
  const bool shown=conflict_&&!status_->text().isEmpty();conflict_=true;if(!shown)status({"config_conflict","Settings changed elsewhere. Your edits are kept here. Reload discards them.",{}});showConflictActions();emit viewStateChanged();return;}
 if(!dirty_&&(baseRevision_!=settings_.revision()||observed_!=settings_.metadata()["observedRevision"].toString()))refresh();
 auto error=settings_.metadata()["validation"].toObject()["error"].toObject();if(!error["code"].toString().isEmpty())status({error["code"].toString(),error["message"].toString(),error["details"].toObject()});
}
void SettingsWindow::updateProtonAccountDisplay(){
 const bool connected=!protonAccountUid_.isEmpty();const bool tested=testedDestinations_.contains(profileId_->text())&&testedDestinations_.value(profileId_->text())==editedProfile();
 protonAccount_->setText(tested?QString("Connection tested"):connected?(protonAccountEmail_.isEmpty()?QString("Account configured"):protonAccountEmail_+" · Account configured"):QString("Not connected"));
 findControl<QPushButton*>("connectProtonAccount")->setText(connected?"Reconnect account…":"Connect account…");
}
bool SettingsWindow::applyProtonAccountResult(const QString& id,const QJsonObject& result){
 auto profiles=draft_["profiles"].toArray();int index=-1;for(int i=0;i<profiles.size();++i)if(profiles[i].toObject()["id"]==id){index=i;break;}
 if(index<0||profiles[index].toObject()["provider"]!="proton-drive"||result["accountRootUid"].toString().isEmpty())return false;
 storeProfile();profiles=draft_["profiles"].toArray();auto profile=profiles[index].toObject();auto settings=profile["settings"].toObject();
 if(index>=profileEditors_.size()||!profileEditors_[index].toObject().contains("id"))return false;
 auto editor=profileEditors_[index].toObject();const auto previous=editor["proton"].toObject();const auto root=result["accountRootUid"].toString();
 auto email=result["accountEmail"].toString();if(email.size()>320||email.contains(QRegularExpression("[\\x00-\\x1f\\x7f]")))email.clear();
 if(email.isEmpty()&&previous["accountRootUid"]==root)email=previous["accountEmail"].toString();
 if(settings["accountRootUid"]!=root){settings["folderUid"]="";settings.remove("folderName");}
 settings["accountRootUid"]=root;const auto selected=settings["folderUid"].toString();QJsonArray folders;int selectedIndex=0;
 folders.append(QJsonObject{{"uid",""},{"name",""},{"label","Choose a folder"}});
 for(const auto& value:result["folders"].toArray()){
  const auto folder=value.toObject();const auto uid=folder["uid"].toString(),name=folder["name"].toString();if(uid.isEmpty())continue;
  if(uid==selected){selectedIndex=folders.size();settings["folderName"]=name;}
  folders.append(QJsonObject{{"uid",uid},{"name",name},{"label",name.isEmpty()?QString("Saved Proton folder"):name}});
 }
 if(!selected.isEmpty()&&selectedIndex==0){selectedIndex=folders.size();const auto name=settings["folderName"].toString();folders.append(QJsonObject{{"uid",selected},{"name",name},{"label",name.isEmpty()?QString("Saved Proton folder"):name}});}
 auto proton=previous;proton["accountRootUid"]=root;proton["accountEmail"]=email;proton["folders"]=folders;proton["folderIndex"]=selectedIndex;editor["proton"]=proton;
 auto providers=editor["providers"].toObject();auto retained=providers["proton-drive"].toObject();auto providerEditor=retained["editor"].toObject();providerEditor["proton"]=proton;retained["editor"]=providerEditor;retained["settings"]=settings;providers["proton-drive"]=retained;editor["providers"]=providers;profileEditors_[index]=editor;
 profile["settings"]=settings;profiles[index]=profile;draft_["profiles"]=profiles;
 if(index==profileIndex_){const bool previousLoading=loading_;loading_=true;applyProfileEditor(editor);loading_=previousLoading;}
 const auto accepted=settings_.document()["profiles"].toArray();
 if(index<accepted.size()&&accepted[index].toObject()["id"]==id&&accepted[index].toObject()["settings"]!=settings){
  const auto path="/profiles/"+QString::number(index)+"/settings";stageEdit(path,settings,protonFolder_);dirty();autosave_->start();
 }
 refreshDestinationList();return true;
}
void SettingsWindow::activateDestinations(bool forceRefresh){
 if(loading_||selectedDestinationChanged()||saving_||conflict_||initialProfile_||setupAccepting_||!setupJobs_.isEmpty())return;
 if(providerType_->currentData()!="proton-drive"||protonAccountUid_.isEmpty())return;
 if(!settings_.diagnostics()["capabilities"].toObject()["providers"].toObject().contains("proton-drive"))return;
 const auto key=profileId_->text()+"/"+protonAccountUid_;
 if(!forceRefresh&&requestedFolders_.contains(key))return;
 requestedFolders_.insert(key);beginSetup("inspectAccount");
}
void SettingsWindow::setupFeedback(const QString& message,bool success){
 for(auto* label:{status_,destinationStatus_}){
  plain(label);if(success)VisualStyle::success(label,message);else VisualStyle::feedback(label,message);
 }
}
QString SettingsWindow::setupBlockedReason()const{
 if(conflict_)return "Resolve the settings conflict before saving credentials";
 if(autosaveBlocked_)return "Retry saving your changes before storing. Typed values are kept here.";
 if(saving_||!pendingEdits_.isEmpty())return "Wait for settings to finish saving. Typed values are kept here.";
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
 if(kind=="testDestination"){testedDestinations_.remove(profileId);refreshDestinationList();testAccepting_=true;setupFeedback(testMessage);}
 updateSetupButton();
 if(kind=="connectAccount"||kind=="inspectAccount")folderRequests_.insert(profileId);
 ++setupAccepting_;updateSetupButton();emit viewStateChanged();
 settings_.request("setup."+kind,params,this,[this,kind,profileId,revision,testMessage](Error e,QJsonObject data){
  folderRequests_.remove(profileId);--setupAccepting_;if(kind=="testDestination")testAccepting_=false;
  if(e){updateSetupButton();status(e);if(kind=="connectAccount"&&profileId==profileId_->text())protonAccount_->setText("Connection failed: "+e.message);emit viewStateChanged();return;}
  // Only an admitted request consumes typed secrets; a refusal preserves them.
  if(profileId==profileId_->text()){
   if(kind=="storeCredentials")clearCurrentCredentialInputs();
   else if(kind=="storeSharePassword"){protonPassword_->clear();rememberCredentialInputs();}
  }
  for(int i=setupJobs_.size()-1;i>=0;--i){const auto old=setupJobs_[i].toObject();if(old["kind"]==kind&&old["profileId"]==profileId)setupJobs_.removeAt(i);}
  setupJobs_.append(QJsonObject{{"setupId",data["setupId"]},{"kind",kind},{"profileId",profileId},{"revision",revision}});
  // Say what is happening; the confirmed result replaces it. Folder loading is
  // shown beside the folder, so the page status doesn't repeat it.
  static const QHash<QString,QString> progress{{"connectAccount","Connecting account…"},{"storeCredentials","Storing credentials…"},{"unlockCredentials","Unlocking saved credentials…"},{"createFolder","Creating folder…"},{"storeSharePassword","Storing link password…"}};
  updateSetupButton();if(kind=="inspectAccount")VisualStyle::feedback(destinationStatus_,{});else setupFeedback(kind=="testDestination"?testMessage:progress.value(kind,"Working…"));
  if(kind=="connectAccount"&&profileId==profileId_->text())protonAccount_->setText("Connecting to Proton…");
  emit viewStateChanged();setupTimer_->start();pollSetup();
 });
}
void SettingsWindow::allowControl(QWidget* control,bool allowed){control->setProperty("providerEnabled",allowed);control->setEnabled(allowed&&!setupBusy_);}
void SettingsWindow::updateSetupButton(){
 const auto id=profileId_->text();bool foldersLoading=folderRequests_.contains(id);
 for(const auto& value:setupJobs_){const auto job=value.toObject();if(job["profileId"]==id&&(job["kind"]=="connectAccount"||job["kind"]=="inspectAccount"))foldersLoading=true;}
 protonFolderStatus_->setText(foldersLoading?(pendingProtonUrl_.isEmpty()?QString("Loading folders…"):QString("Waiting for Proton sign-in…")):QString{});protonFolderStatus_->setVisible(foldersLoading&&providerType_->currentData()=="proton-drive");
 const bool busy=setupAccepting_||!setupJobs_.isEmpty();
 // While setup runs only its actionable controls are disabled: names, account
 // identity, loading text and help stay readable.
 setupBusy_=busy;
 for(auto* container:{findControl<QWidget*>("destinationEditor"),findControl<QWidget*>("credentialsEditor"),static_cast<QWidget*>(protonPanel_),findControl<QWidget*>("protonAdvancedSettings")})
  for(auto* control:container->findChildren<QWidget*>()){
   const bool editor=qobject_cast<QAbstractButton*>(control)||qobject_cast<QComboBox*>(control)||qobject_cast<QAbstractSpinBox*>(control)||qobject_cast<QPlainTextEdit*>(control)||(qobject_cast<QLineEdit*>(control)&&!qobject_cast<QComboBox*>(control->parentWidget())&&!qobject_cast<QAbstractSpinBox*>(control->parentWidget()));
   if(editor&&control->property("visualRole").toString()!="info"){const auto allowed=control->property("providerEnabled");const bool applies=!allowed.isValid()||allowed.toBool();control->setEnabled(!busy&&applies);
    // A value held while setup runs stays readable; its actions are unavailable.
    Graphite::setLocked(control,busy&&applies&&!qobject_cast<QPushButton*>(control)&&!qobject_cast<QToolButton*>(control));}
  }
 protonBrowserRetry_->setEnabled(!pendingProtonUrl_.isEmpty()&&!protonBrowserOpening_);
 allowControl(findControl<QPushButton*>("inspectProtonAccount"),!protonAccountUid_.isEmpty());
 if(!busy&&autosave_&&!pendingEdits_.isEmpty()&&!conflict_&&!resetting_)autosave_->start();
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
 protonBrowserOpening_=false;const bool failed=!result["opened"].toBool();protonBrowserRetry_->setEnabled(true);protonBrowserRetry_->setVisible(failed);if(failed)protonAccount_->setText("Couldn't open the default browser for Proton sign-in.");
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
   if(pendingProtonSetup_==id){pendingProtonUrl_.clear();pendingProtonSetup_.clear();protonBrowserOpening_=false;protonBrowserRetry_->hide();}
   setupJobs_.removeAt(index);updateSetupButton();if(setupJobs_.isEmpty())setupTimer_->stop();
   if(e)e.message="This setup result is no longer available; the action was not repeated.";
   else{const auto error=data["error"].toObject();e={error["code"].toString(),error["message"].toString(),error["details"].toObject()};}
   if(e){if(job["kind"]=="testDestination"&&(job["profileId"]!=profileId_->text()||(!job["revision"].toString().isEmpty()&&job["revision"]!=settings_.revision())))e.message="Connection test failed for the original destination settings. "+e.message;status(e);if(data["kind"]=="connectAccount"&&data["profileId"]==profileId_->text())protonAccount_->setText("Connection failed: "+e.message);}else {
    const bool currentTest=data["profileId"]==profileId_->text()&&(job["revision"].toString().isEmpty()||job["revision"]==settings_.revision())&&!selectedDestinationChanged();
    if(data["kind"]=="testDestination"&&currentTest){testedDestinations_.insert(data["profileId"].toString(),editedProfile());updateProtonAccountDisplay();refreshDestinationList();}
    if(data["kind"]=="storeCredentials"||data["kind"]=="unlockCredentials")refreshCredentialHint();
    const auto result=data["result"].toObject();const auto kind=data["kind"].toString();
    if(kind=="connectAccount"||kind=="inspectAccount"){
     const auto revision=job["revision"].toString();
     if(conflict_||(!revision.isEmpty()&&revision!=settings_.revision())||!applyProtonAccountResult(data["profileId"].toString(),result)){VisualStyle::feedback(plain(status_),"Account details changed while loading. Refresh the original destination to continue.",true);emit viewStateChanged();return;}
     requestedFolders_.insert(data["profileId"].toString()+"/"+result["accountRootUid"].toString());
     if(kind=="connectAccount")setupFeedback("Account connected. Folders loaded.",true);else{VisualStyle::success(plain(status_),"Folders loaded.");VisualStyle::feedback(destinationStatus_,{});}emit viewStateChanged();
     if(data["profileId"]!=profileId_->text()&&destinationPage_->isVisible())activateDestinations();
     return;
    }
    if(QStringList{"createFolder","storeSharePassword"}.contains(kind)&&(data["profileId"]!=profileId_->text()||providerType_->currentData()!="proton-drive"||conflict_)){VisualStyle::feedback(plain(status_),"Setup completed for the original destination. Refresh that destination before applying its result.",true);emit viewStateChanged();return;}

    if(kind=="createFolder"){protonAccountUid_=result["accountRootUid"].toString();addProtonFolder(protonFolder_,result["uid"].toString(),result["name"].toString());protonFolder_->setCurrentIndex(protonFolder_->count()-1);dirty();queueEdit(protonFolder_);}
    else if(kind=="storeSharePassword"){auto profiles=draft_["profiles"].toArray();if(profileIndex_>=0&&profileIndex_<profiles.size()){auto p=profiles[profileIndex_].toObject();auto s=p["settings"].toObject();s["sharePasswordReference"]=result["reference"];p["settings"]=s;profiles[profileIndex_]=p;draft_["profiles"]=profiles;dirty();queueEdit(protonFolder_);}}
    if(kind=="testDestination")setupFeedback(currentTest?(result["testScope"]=="account-folder"?"Connection successful. Account and selected folder are accessible. Upload and sharing were not tested.":"Connection successful. Test upload, link and cleanup passed."):"Connection test succeeded for the original destination settings. Test again to check the current settings.",true);
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
void SettingsWindow::showConflictActions(){for(auto* button:reloadConflict_)button->setVisible(conflict_);if(captureOutputBlock_)updateCaptureInheritance();}
// What a reload discards, in the words of the page that shows it.
QStringList SettingsWindow::draftEdits()const{
 QStringList edits;const auto add=[&edits](const QString& text){if(!edits.contains(text))edits.append(text);};
 const auto draft=candidate();auto base=baseDocument_.isEmpty()?settings_.document():baseDocument_;if(base.isEmpty())base=Settings::defaults();
 // Settings controls by their bound path; their accessible names are the visible labels.
 QHash<QString,QString> names;QList<QWidget*> controls=findChildren<QWidget*>();if(destinationPage_)controls+=destinationPage_->findChildren<QWidget*>();
 for(auto* control:controls){const auto path=control->property("configPath").toString();if(path.isEmpty()||path.contains("{profile}")||names.contains(path)||(qobject_cast<QLineEdit*>(control)&&qobject_cast<QComboBox*>(control->parentWidget())))continue;auto name=qobject_cast<QAbstractButton*>(control)?qobject_cast<QAbstractButton*>(control)->text():control->accessibleName();name=name.section(':',0,0);if(!name.isEmpty())names.insert(path,name);}
 const auto describe=[&](const QString& path)->QString{
  const auto parts=path.split('/',Qt::SkipEmptyParts);const auto group=parts.value(0);
  if(group=="actions"&&parts.size()>2&&parts[2]=="shortcut")return "Shortcuts → "+actionLabel(parts[1]);
  if(group=="actions")return "Capture → "+actionLabel(parts.value(1))+" output";
  if(group=="captureDefaults")return QString("Capture → Default capture output");
  for(auto prefix=path;!prefix.isEmpty();prefix=prefix.left(qMax(0,int(prefix.lastIndexOf('/'))))){
   if(names.contains(prefix)){const auto page=group=="limits"||group=="retention"||path.startsWith("/preferences/diagnostics")?QString("Advanced"):group=="archive"||group=="naming"||path=="/preferences/uploadClipboardBehavior"?QString("Capture"):QString("General");return page+" → "+names.value(prefix);}
  }
  return "Settings → "+parts.join(" › ");
 };
 // Leaf differences, outside the destination list.
 std::function<void(const QJsonValue&,const QJsonValue&,const QString&)> compare=[&](const QJsonValue& now,const QJsonValue& before,const QString& path){
  if(now==before)return;
  if(now.isObject()&&before.isObject()){const auto a=now.toObject(),b=before.toObject();auto keys=a.keys();for(const auto& key:b.keys())if(!a.contains(key))keys.append(key);for(const auto& key:keys)compare(a.value(key),b.value(key),path+"/"+key);return;}
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
 if(!Graphite::confirm(this,"Reload settings from file",text,"Reload from file",!edits.isEmpty()))return;
 resetDraft(true);
}
void SettingsWindow::fitRails(){
 int widest=0;const QFontMetrics strong(Graphite::font(Graphite::Type::body,true));if(settingsNavigation_)for(int i=0;i<settingsNavigation_->count();++i)widest=qMax(widest,strong.horizontalAdvance(settingsNavigation_->item(i)->text()));const int width=Graphite::sidebarWidth(Graphite::Metric::sidebar,qMax(widest+Graphite::scaled(18)+56,addDestination_?addDestination_->sizeHint().width()+20:0));
 if(settingsRail_)settingsRail_->setFixedWidth(width);
 if(destinationRail_)destinationRail_->setFixedWidth(width);
}
}
