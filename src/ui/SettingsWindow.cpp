#include "SettingsWindow.h"
#include "storage/proton/ProtonProvider.h"
#include "storage/s3/PublicUrl.h"
#include <QDesktopServices>
#include <QUrl>
#include "ViewState.h"
#include <QTimer>
#include <QSignalBlocker>
#include "ShortcutEditor.h"
#include "ByteSize.h"
#include "VisualStyle.h"
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
#include <QFileDialog>
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
#include <QTabWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <QClipboard>
#include <QMenu>
#include <QRadioButton>
#include <QInputDialog>
#include <QMessageBox>
#include <QStyle>
#include <QAbstractSpinBox>
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
namespace putgd {
namespace {
constexpr int protonFolderNameRole=Qt::UserRole+1;
void addProtonFolder(QComboBox* combo,const QString& uid,const QString& name){
 combo->addItem(name.isEmpty()?(uid.isEmpty()?"Choose a folder after connecting":"Saved Proton folder"):name,uid);
 combo->setItemData(combo->count()-1,name,protonFolderNameRole);
}
QString title(const QString& s){return s.left(1).toUpper()+s.mid(1);}
QString actionLabel(const QString& action){static const QHash<QString,QString> labels{{"region","Region capture"},{"window","Window capture"},{"desktop","Desktop capture"},{"clipboard","Clipboard import"},{"edit","Region capture & edit"}};return labels.value(action);}
QString sharingLabel(const QString& mode){return mode=="share"?QString("Share and copy a link"):mode=="private"?QString("Keep private, copy nothing"):QString("Use destination default");}
QComboBox* choices(QWidget* parent,const QString& name,const QStringList& values){auto* c=VisualStyle::choiceBox(parent);c->setObjectName(name);c->setAccessibleName(name);c->addItems(values);return c;}
QLineEdit* input(QWidget* parent,const QString& name){auto* e=new QLineEdit(parent);e->setObjectName(name);e->setAccessibleName(name);return e;}
QJsonObject s3Profile(const QString& id){
 return {{"id",id},{"label","S3 destination"},{"provider","s3"},
  {"settings",QJsonObject{{"bucket",""},{"region","us-east-1"},{"endpoint","https://s3.us-east-1.amazonaws.com"},{"objectPrefix",""},{"storageClass","STANDARD"},{"aclMode","omit"},{"publicUrlBase",QJsonValue::Null},{"addressing","auto"},{"connectTimeoutMs",5000},{"requestTimeoutMs",60000},{"lowSpeedBytesPerSecond",1},{"lowSpeedTimeSeconds",30}}},
  {"credentials",QJsonObject{{"mode","keychain"},{"reference","putgd/"+id}}}};
}
// Drag and drop only proposes a move; the window reorders its draft and rebuilds
// the rows, so Qt never moves or removes items itself.
// Rows are a fixed height with two lines, the label and its state (Default ·
// Ready to test). Each line is elided instead of wrapped, so a long label can
// never push the state out of its row (#134).
class DestinationRowDelegate final:public QStyledItemDelegate {
public:
 using QStyledItemDelegate::QStyledItemDelegate;
 QStyleOptionViewItem shown(const QStyleOptionViewItem& base,const QModelIndex& index)const{auto option=base;initStyleOption(&option,index);return option;}
protected:
 void initStyleOption(QStyleOptionViewItem* option,const QModelIndex& index)const override{
  QStyledItemDelegate::initStyleOption(option,index);
  // displayText() has already turned the label/state newline into a line separator.
  auto lines=option->text.split(QChar::LineSeparator);if(lines.size()<2)return;
  option->features&=~QStyleOptionViewItem::WrapText;
  const auto* style=option->widget?option->widget->style():QApplication::style();
  const int width=qMax(16,style->subElementRect(QStyle::SE_ItemViewItemText,option,option->widget).width()-4);
  for(auto& line:lines)line=option->fontMetrics.elidedText(line,Qt::ElideRight,width);
  option->text=lines.join(QChar::LineSeparator);
 }
};
class DestinationList final:public QListWidget {
public:
 std::function<void(int from,int to)> moveRequested;
 DestinationList(){setDragEnabled(true);setAcceptDrops(true);setDragDropMode(QAbstractItemView::InternalMove);setDefaultDropAction(Qt::MoveAction);setDropIndicatorShown(false);setItemDelegate(new DestinationRowDelegate(this));setTextElideMode(Qt::ElideRight);}
 // The displayed lines of a row: the label and its state, each elided to one line.
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
QString localTime(const QString& iso){const auto time=QDateTime::fromString(iso,Qt::ISODateWithMs);return time.isValid()?QLocale().toString(time.toLocalTime(),QLocale::ShortFormat):iso;}
QString operationLine(const QJsonObject& op){
 static const QHash<QString,QString> actions{{"region","Region capture"},{"window","Window capture"},{"desktop","Desktop capture"},{"clipboard","Clipboard upload"},{"edit","Edited capture"},{"record","Screen recording"},{"file","File upload"}};
 const int items=op["items"].toInt();
 return actions.value(op["action"].toString(),op["action"].toString())+" · "+op["state"].toString()+" · "+localTime(op["created"].toString())+(items>1?QString(" · %1 files").arg(items):QString{});
}
// Create diagnostics package (#122): previews what diagnostics.bundle would save,
// where, and with which redaction, then saves it locally. Nothing is uploaded; the
// extras slot is where #123's optional upload goes.
class DiagnosticsPackageDialog final:public QDialog {
public:
 using Reply=std::function<void(Error,QJsonObject)>;
 std::function<void(const QString&)> folderChosen; // an empty folder means the default
 DiagnosticsPackageDialog(UiSession& session,QWidget* parent):QDialog(parent),session_(session){
  setObjectName("diagnosticsPackageDialog");setWindowTitle("Create diagnostics package");setAccessibleName("Create diagnostics package");setModal(true);setMinimumWidth(560);
  auto* layout=new QVBoxLayout(this);layout->setContentsMargins(24,20,24,18);layout->setSpacing(14);
  auto* heading=new QLabel("Create diagnostics package",this);VisualStyle::role(heading,"sectionTitle");layout->addWidget(heading);
  auto* intro=new QLabel("A .zip a maintainer can use to work out what went wrong. It is saved on this computer only; nothing is uploaded.",this);intro->setWordWrap(true);VisualStyle::role(intro,"muted");layout->addWidget(intro);
  auto* form=new QFormLayout;VisualStyle::styleForm(form);form->setVerticalSpacing(12);layout->addLayout(form);
  auto label=[this](const char* name){auto* l=new QLabel(this);l->setObjectName(name);l->setWordWrap(true);l->setTextFormat(Qt::PlainText);l->setTextInteractionFlags(Qt::TextSelectableByMouse);return l;};
  scope_=label("packageScope");form->addRow("Covers",scope_);
  contents_=label("packageContents");form->addRow("Contains",contents_);
  operations_=label("packageOperations");form->addRow("Uploads",operations_);
  auto* names=new QWidget(this);auto* namesLayout=new QVBoxLayout(names);namesLayout->setContentsMargins(0,0,0,0);namesLayout->setSpacing(4);
  includeNames_=new QCheckBox("Include file names",this);includeNames_->setObjectName("includeFileNames");includeNames_->setAccessibleName("Include file names");namesLayout->addWidget(includeNames_);
  redaction_=label("packageRedaction");VisualStyle::role(redaction_,"muted");namesLayout->addWidget(redaction_);form->addRow("Names",names);
  auto* never=new QLabel("Credentials, tokens, signed links, file contents, screenshots and clipboard text are never included.",this);never->setWordWrap(true);VisualStyle::role(never,"muted");form->addRow("Never",never);
  auto* where=new QWidget(this);auto* whereLayout=new QHBoxLayout(where);whereLayout->setContentsMargins(0,0,0,0);whereLayout->setSpacing(8);
  folder_=label("packageFolder");VisualStyle::role(folder_,"mono");whereLayout->addWidget(folder_,1);
  auto* choose=new QPushButton("Choose folder…",this);choose->setObjectName("choosePackageFolder");whereLayout->addWidget(choose);
  useDefault_=new QPushButton("Use default",this);useDefault_->setObjectName("defaultPackageFolder");useDefault_->setToolTip("Save to <archive root>/diagnostics, where put.gd keeps the newest 10 packages for 30 days.");whereLayout->addWidget(useDefault_);
  form->addRow("Save to",where);
  // Reserved for #123 (Also upload and copy a link).
  auto* extras=new QWidget(this);extras->setObjectName("diagnosticsPackageExtras");auto* extrasLayout=new QVBoxLayout(extras);extrasLayout->setContentsMargins(0,0,0,0);layout->addWidget(extras);
  result_=label("packageResult");VisualStyle::role(result_,"status");result_->hide();layout->addWidget(result_);
  auto* buttons=new QHBoxLayout;show_=new QPushButton("Show in folder",this);show_->setObjectName("showPackageInFolder");show_->hide();buttons->addWidget(show_);buttons->addStretch();
  close_=new QPushButton("Cancel",this);close_->setObjectName("closePackage");buttons->addWidget(close_);
  save_=new QPushButton("Save package",this);save_->setObjectName("savePackage");VisualStyle::role(save_,"primary");save_->setDefault(true);buttons->addWidget(save_);layout->addLayout(buttons);
  connect(includeNames_,&QCheckBox::toggled,this,[this]{refreshPreview();});
  connect(choose,&QPushButton::clicked,this,[this]{const auto path=QFileDialog::getExistingDirectory(this,"Save diagnostics packages to",folder_->text());if(!path.isEmpty())setFolder(path,true);});
  connect(useDefault_,&QPushButton::clicked,this,[this]{setFolder({},true);});
  connect(close_,&QPushButton::clicked,this,&QDialog::reject);
  connect(save_,&QPushButton::clicked,this,[this]{save({});});
  connect(show_,&QPushButton::clicked,this,[this]{reveal({});});
 }
 // Each opening starts with names hashed, the privacy default.
 void setScope(const QString& operation,const QString& folder){operation_=operation;custom_=QFileInfo(folder).isDir()?folder:QString{};{const QSignalBlocker block(includeNames_);includeNames_->setChecked(false);}saved_={};error_={};result_->hide();show_->hide();save_->setText("Save package");close_->setText("Cancel");refreshPreview();}
 void setIncludeNames(bool include){if(includeNames_->isChecked()!=include)includeNames_->setChecked(include);}
 Error setFolder(const QString& folder,bool remember){
  if(!folder.isEmpty()&&!QFileInfo(folder).isDir())return {"diagnostics_output_unavailable","Choose an existing folder.",{{"path",folder}}};
  custom_=folder;if(remember&&folderChosen)folderChosen(folder);refreshPreview();return {};
 }
 // Answers once the preview that is current when it arrives has come back.
 void whenPreviewed(Reply reply){if(!previewing_)reply(error_,state());else previewReplies_.append(std::move(reply));}
 QJsonObject params(bool preview)const{
  QJsonObject p{{"includeNames",includeNames_->isChecked()}};if(!operation_.isEmpty())p["operation"]=operation_;if(!custom_.isEmpty())p["output"]=custom_;
  if(preview)p["preview"]=true;else p["notify"]=true;return p;
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
  saving_=true;save_->setEnabled(false);save_->setText("Saving…");
  session_.request("diagnostics.bundle",params(false),this,[this,reply](Error error,QJsonObject saved){
   saving_=false;error_=error;
   if(error){save_->setText("Save package");save_->setEnabled(true);VisualStyle::feedback(result_,error.message,true);result_->show();}
   else {saved_=saved;save_->setText("Saved");close_->setText("Close");show_->show();VisualStyle::success(result_,"Saved "+QFileInfo(saved["path"].toString()).fileName()+" ("+ByteSize::summary(saved["bytes"].toInteger())+") in "+QDir::toNativeSeparators(QFileInfo(saved["path"].toString()).absolutePath())+".");result_->show();}
   fit();if(reply)reply(error,state());
  });
 }
 void reveal(Reply reply){
  if(saved_.isEmpty()){if(reply)reply({"diagnostics_not_saved","Save the package first.",{}},state());return;}
  session_.request("diagnostics.reveal",{{"path",saved_["path"]}},this,[this,reply](Error error,QJsonObject result){if(error){VisualStyle::feedback(result_,error.message,true);result_->show();}if(reply)reply(error,result);});
 }
 // Wrapped labels change height with their text; grow so none is clipped.
 void fit(){
  layout()->activate();const int needed=layout()->hasHeightForWidth()?layout()->totalHeightForWidth(width()):layout()->totalSizeHint().height();
  setMinimumHeight(needed);if(height()<needed)resize(width(),needed);
 }
 QJsonObject state()const{
  QJsonObject state{{"open",isVisible()},{"includeNames",includeNames_->isChecked()},{"folder",folder_->text()},{"defaultFolder",custom_.isEmpty()},{"previewing",previewing_},{"saving",saving_},
   {"scope",scope_->text()},{"contents",contents_->text()},{"operations",operations_->text()},{"redaction",redaction_->text()},{"preview",preview_}};
  if(!operation_.isEmpty())state["operation"]=operation_;
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
  for(const auto& name:preview_["files"].toArray())files.append(name.toString()+": "+about.value(name.toString()));
  contents_->setText(files.join('\n'));
  QStringList lines;const auto list=preview_["operationList"].toArray();
  for(int i=0;i<list.size()&&i<5;++i)lines.append(operationLine(list[i].toObject()));
  if(list.size()>5)lines.append(QString("and %1 more").arg(list.size()-5));
  operations_->setText(lines.isEmpty()?QString("No failed uploads in this period; the log is still included."):lines.join('\n'));
  folder_->setText(QDir::toNativeSeparators(preview_["folder"].toString()));
  save_->setEnabled(saved_.isEmpty()&&!saving_);fit();
 }
 UiSession& session_;
 QLabel *scope_,*contents_,*operations_,*redaction_,*folder_,*result_;QCheckBox* includeNames_;QPushButton *useDefault_,*show_,*close_,*save_;
 QString operation_,custom_;QJsonObject preview_,saved_;Error error_;bool previewing_=false,saving_=false;quint64 previewSequence_=0;QList<Reply> previewReplies_;
};
QString settingHelp(const QString& path,const QString& objectName){
 static const QHash<QString,QString> s3Help{
  {"bucket","The existing S3 bucket that receives uploads. Enter only the bucket name, without a URL or folder path. This field starts empty. Saving settings does not create a bucket or change its access policy; the saved credentials must have access to this bucket."},
  {"region","The region used to sign requests to your bucket, such as us-east-2 for Ohio. The default is us-east-1. Select an offline AWS suggestion or type a custom region for an S3-compatible service. Changing the region updates an unchanged standard AWS endpoint; it preserves a custom endpoint."},
  {"endpoint","The HTTPS address of the S3 service, without the bucket name or object prefix. It starts with the standard AWS endpoint for the selected region. Use your service's endpoint for compatible storage. A custom value is preserved when you change the region."},
  {"objectPrefix","An optional path prefix added before every remote object name, such as captures/. Leave it empty to upload at the bucket root. This does not change local folder routing and does not move previously uploaded objects."},
  {"storageClass","The S3 storage class requested for new uploads. STANDARD is the default. Other classes can have different access, retrieval and storage costs; use a value supported by your destination. This setting does not change the class of existing objects."},
  {"publicUrlBase","An optional HTTPS base URL for copied direct links, such as your own CDN or custom domain. The object path is appended to this URL. Leave it empty to use the destination's direct URL. A custom domain does not make private objects public or configure DNS or a bucket policy."},
  {"connectTimeoutMs","The connection timeout shown in seconds, with millisecond precision. The default is 5 seconds. It limits how long a storage request waits to establish a connection; it is separate from the overall request timeout below."},
  {"requestTimeoutMs","The request timeout shown in seconds, with millisecond precision. The default is 60 seconds. It controls the storage client's request timeout. Allow enough time for your connection and file sizes; a timed-out upload may still need reconciliation before its remote result is known."},
  {"lowSpeedBytesPerSecond","The minimum sustained transfer rate in bytes per second. The default is 1 byte/s. A transfer below this rate for the low-speed window is considered stalled. This is a stall threshold, not an upload speed limit or a file-size limit."},
  {"lowSpeedTimeSeconds","The number of seconds a transfer may remain below the minimum transfer rate before it is considered stalled. The default is 30 seconds. It works with Minimum transfer and does not cap an upload's total duration."}};
 static const QHash<QString,QString> numberHelp{
  {"pendingImageBytes","Maximum total decoded image bytes retained by jobs waiting for or undergoing encoding. The default is 64 MiB. An acquisition that cannot fit fails with image_memory_limit; it is not silently deferred or changed. The limit is released after disk persistence or queued cancellation. This bounds source payloads, not total app RSS, compositor frames, encoding workspace or the current native clipboard representation."},
  {"maxFiles","The maximum number of files accepted from one clipboard action. The default is 100. A batch above this limit is rejected before importing its files. This is a per-action limit and does not restrict the total number of history entries."},
  {"pendingOperations","The maximum number of queued operations waiting for processing, in addition to those currently running. The default is 8. When the queue is full, a new action is rejected rather than accumulating more pending work."},
  {"concurrentUploads","The maximum number of operations processed at the same time. The default is 1. Increasing it can improve throughput but uses more memory and network resources; it does not reserve that memory in advance."},
  {"maxFileBytes","The largest individual imported file, in GiB. Auto, the default, sets no size limit of put.gd's own: uploads stream from disk, so memory does not grow with file size. Whatever this is set to, a file larger than the destination accepts (5 TiB for S3) or than the free disk space allows is rejected before importing; put.gd always keeps 15% of the disk, up to 4 GiB and at least 1 GiB, free for the system. No limit (an empty field) also sets none. Fractional GiB values are accepted when they represent a whole number of bytes."},
  {"maxBatchBytes","The maximum combined size of one clipboard file batch, in GiB. Auto, the default, and No limit (an empty field) set none of put.gd's own; free disk space still bounds every batch. This applies to the whole batch in addition to the individual file-size limit."},
  {"pendingArtifactBytes","The maximum acquired content waiting in the pending artifact store on disk, in GiB. Auto, the default, and No limit (an empty field) set none of put.gd's own: free disk space is the bound, keeping 15% of the disk (up to 4 GiB, at least 1 GiB) free for the system. New imports are rejected when they would exceed this limit. This is a disk-content ceiling, not memory allocated at startup."},
  {"clipboardTextBytes","The largest text payload accepted from the clipboard, in MiB. The default is 16 MiB. Larger text is rejected before importing. One MiB equals 1,048,576 bytes; this limit does not apply to ordinary file uploads."},
  {"imagePixels","The largest image, shown in megapixels (one million pixels), calculated as width × height. The default is 100 megapixels. This protects against oversized decoded images independently of their encoded file size."},
  {"imageBytes","The maximum accepted image payload size, in MiB. The default is 512 MiB. It works alongside the image-pixel limit; an image must satisfy both limits. One MiB equals 1,048,576 bytes. This is a ceiling, not a persistent allocation."},
  {"failedArtifactDays","How many days failed or interrupted local artifacts are kept for explicit retry before cleanup may remove them. The default is 7 days. Set 0 to disable age-based removal of failed artifacts. This controls local retry content and does not delete remote objects."},
  {"recentActivityLimit","The maximum number of recent operation snapshots available to History. The default is 100. This bounds the activity list; it does not set the complete upload-history retention period below."},
  {"previewCacheBytes","The maximum total size of generated image previews stored on disk, in MiB. The default is 256 MiB. Set 0 to disable stored previews. This is a disk-cache limit, not a memory allowance; previews may be regenerated or removed by cleanup."},
  {"historyRetentionDays","How many days completed history metadata is retained before automatic pruning. Leave empty to keep history indefinitely, which is the default. Removing old history metadata does not delete remote uploads or your saved local files."}};
 static const QHash<QString,QString> descriptions{
  {"unlockCredentialsAtStartup","Reads saved destination credentials from the desktop keyring once when put.gd starts, so uploads work without pressing Unlock. If the keyring is locked, the desktop shows its own unlock prompt then. Uploads and shortcuts never unlock the keyring."},
  {"launchAtLogin","Starts put.gd when you sign in. Disabled by default. The desktop startup entry is updated after persistence; another app copy is not started."},
  {"notifications","Shows desktop notifications for operation results when supported. Enabled by default. Disabling notifications leaves results available in History."},
  {"debugLogging","Adds detailed debug records to put.gd's log on this computer, for a maintainer working out a problem. It turns itself off 24 hours after you turn it on; restarting doesn't extend it. Credentials, tokens and file contents are never logged."},
  {"logSummary","put.gd keeps a log of what it did in its state folder, at most 14 days and 50 MiB by default (Advanced settings in the configuration file). Clear logs deletes every log file and starts a new one; uploads, history, settings and saved diagnostics packages stay."},
  {"uncleanExit","When put.gd starts and finds it didn't shut down cleanly last time, it records why: a signal such as a logout, or an unknown cause such as a crash, a forced kill or a power loss. The newest one is shown here and included in diagnostics packages."},
  {"uploadClipboardBehavior","Copy each link writes successful upload links individually. Copy a list writes one newline-separated list in submission order after overlapping uploads finish. Copying something yourself pauses automatic copies; links stay available in History. Applies to new operations."},
  {"keepUploads","Save a local copy of captures and clipboard imports, independently of the upload destination. With a destination enabled, do both; with no destination, save locally only. If both are off, the action fails before capturing. Local-copy failures are reported separately from upload results."},
  {"root","Absolute default folder for captures, clipboard results and optional upload copies. Defaults to put.gd under Documents, or your home folder if unavailable. A category folder overrides it; existing files receive unique names."},
  {"destination","Choose a destination for this action or turn uploads off. Local copies are controlled independently in General. Applies to new captures, without changing accepted jobs."},
  {"profileId","Choose a saved destination for uploads, or No destination to turn uploads off. Local copies are controlled independently in General. If neither output is enabled, the action fails before capturing. Changing this does not move earlier uploads or alter accepted jobs."},
  {"publicationMode","Use the destination default, create a share link, or keep uploads private. Private mode creates no share link and leaves the clipboard unchanged; existing member or folder permissions still apply. Unsupported modes are rejected before uploading."},
  {"mode","Keep original names preserves imported names. Random names uses a UUID and extension. Custom template renames imports too. Generated captures use their templates unless Random names is selected. Names remain fixed on retry."},
  {"collisionPolicy","For remote duplicates, add a unique suffix (default), skip the file, or replace the checked object where safely supported. Replacement may change content served by an existing link. Local files always preserve existing files and add a suffix."},
  {"label","Display name used in destination selectors. Renaming it does not rename a bucket, change credentials or move uploaded files."},
  {"aclMode","Follow bucket policy sends no ACL header (default). Public read requests an object ACL and requires compatible bucket settings and permission. This does not change a bucket policy or guarantee public access."},
  {"addressing","Automatic lets the client select addressing (default). Virtual puts the bucket in the hostname; path puts it in the URL path. Some S3-compatible endpoints require path addressing."},
  {"credentialMode","Keychain saves credentials in the desktop secret service only when you choose Store credentials. Session keeps them in memory until put.gd exits. Configuration stores references, never keys. Changing this selector does not migrate existing credentials."},
  {"reference","Identifier for the credentials in the desktop secret service, not an access key. Changing it selects a different stored entry. Session mode stores no reference. Store or unlock credentials explicitly after destination edits save."},
  {"credentialAccessKey","Access key ID authorized for this destination. Store credentials explicitly after destination edits save. This input is never saved in configuration or history. Stored credentials appear only as a short identifying prefix; closing or reloading clears typed credential inputs."},
  {"credentialSecretKey","Secret access key paired with the access key ID. Never written to configuration, history or view checkpoints. Store credentials explicitly; typed values are cleared afterward and on close or reload. Show reveals the saved secret after credentials are unlocked. Hide clears revealed saved values; type a new value to replace the secret."},
  {"credentialSessionToken","Optional token for temporary AWS credentials. Leave empty for credentials without a token. Stored with the key pair only through Store credentials; never written to configuration, history or view checkpoints."},
  {"protonFolder","Choose an existing top-level folder in Proton My files or create a new folder. Saved connected destinations load named folders on opening; Refresh folders reloads them. Folder selection applies only to future uploads."},
  {"protonExpiration","Share-link expiration in days. Zero means no expiration. Applies to newly created links; it does not update existing shares or delete stored files."},
  {"protonRequestTimeout","Overall Proton request deadline in seconds, with millisecond precision. Default 120 seconds. Allow time for large transfers. A timeout may require inspecting the remote result before retrying."},
  {"protonSharePassword","Password for future Proton share links. Store link password explicitly; the configuration stores only its secret-store reference. Use no link password clears that reference for future links. Existing links are unchanged."},
  {"s3Service","Choose an S3-compatible service to see endpoint guidance. Amazon, Cloudflare R2 and Backblaze B2 use the same validated S3 adapter. Choosing a service does not create or move storage; enter the service endpoint and bucket."},
  {"customEndpoint","Show and edit the S3 service endpoint. An unchanged AWS endpoint follows the selected region; custom endpoints are preserved. Unchecking hides this editor without changing an existing custom endpoint."}};
 const auto key=path.section('/',-1);
 if(objectName.startsWith("proton"))return descriptions.value(objectName);
 if(s3Help.contains(key))return s3Help.value(key);
 if(numberHelp.contains(key))return numberHelp.value(key);
 if(path.startsWith("/archive/folders/"))return "Optional absolute folder for "+key+" content. Leave empty to use the default output folder. Applies to local captures and optional upload copies; existing files are preserved with unique names.";
 if(path.startsWith("/naming/")&&key.endsWith("Template"))return "Names "+QString(objectName=="fileTemplate"?"imported files when Custom template is selected":objectName=="captureTemplate"?"screen captures":objectName=="imageTemplate"?"clipboard images":"clipboard text")+". Tokens: {name} is the source stem, {ext} its extension without a dot, {yyyy}, {MM}, {dd}, {HH}, {mm}, {ss} use local action time, and {uuid} is unique per item. Include a dot before {ext}; paths and unknown tokens are rejected. Existing history and accepted jobs are unchanged.";
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
 auto* layout=new QVBoxLayout(this);layout->setContentsMargins(28,22,28,18);layout->setSpacing(18);
 tabs_=new QTabWidget(this);tabs_->setObjectName("settingsTabs");tabs_->setAccessibleName("Settings categories");tabs_->tabBar()->installEventFilter(this);layout->addWidget(tabs_,1);
 auto scrollPage=[&](const QString& title){auto* body=new QWidget;auto* box=new QVBoxLayout(body);box->setContentsMargins(0,22,8,12);box->setSpacing(16);box->setAlignment(Qt::AlignTop);auto* scroll=new QScrollArea;scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);scroll->setWidget(body);tabs_->addTab(scroll,title);return box;};
 auto* general=scrollPage("General");auto* capture=scrollPage("Capture");auto* shortcutsPage=scrollPage("Shortcuts");auto* advancedPage=scrollPage("Advanced");
 auto section=[&](QVBoxLayout* box,const QString& heading,const QString& description=QString{}){auto* card=VisualStyle::card(heading,description,this);box->addWidget(card);auto* form=new QFormLayout;VisualStyle::styleForm(form);form->setVerticalSpacing(14);qobject_cast<QVBoxLayout*>(card->layout())->addLayout(form);return form;};
 auto bind=[&](QWidget* widget,const QString& path){widget->setProperty("configPath",path);};
 auto setting=[&](QFormLayout* form,const QString& label,QWidget* control,const QString& path,const QString& hint=QString{},const QString& unit=QString{}){
  control->setAccessibleName(label);bind(control,path);auto* wrapper=new QWidget;wrapper->setProperty("visualRole","settingRow");auto* column=new QVBoxLayout(wrapper);column->setContentsMargins(0,0,0,0);column->setSpacing(5);auto* row=new QHBoxLayout;row->setContentsMargins(0,0,0,0);row->setSpacing(8);if(unit.isEmpty())row->addWidget(control,1);
  else{auto* unitField=new QWidget;auto* unitLayout=new QHBoxLayout(unitField);unitLayout->setContentsMargins(0,0,0,0);unitLayout->setSpacing(0);control->setProperty("hasUnit",true);unitLayout->addWidget(control,1);auto* suffix=new QLabel(unit);suffix->setProperty("visualRole","unitSuffix");unitLayout->addWidget(suffix);row->addWidget(unitField,1);}
  column->addLayout(row);auto* error=new QLabel;error->setWordWrap(true);error->setObjectName(control->objectName()+"Error");error->setProperty("visualRole","error");error->hide();column->addWidget(error);errors_[control]=error;
  if(!hint.isEmpty()){auto* text=new QLabel(hint);text->setWordWrap(true);VisualStyle::role(text,"muted");column->addWidget(text);}
  const auto help=settingHelp(path,control->objectName());form->addRow(VisualStyle::settingLabel(label,help,control,this),wrapper);return wrapper;
 };
 auto* preferences=section(general,"Everyday settings");
 login_=new QCheckBox("Launch at login");login_->setObjectName("launchAtLogin");setting(preferences,"Startup",login_,"/preferences/launchAtLogin");
 unlockAtStartup_=new QCheckBox("Unlock saved credentials when put.gd starts");unlockAtStartup_->setObjectName("unlockCredentialsAtStartup");setting(preferences,"Credentials",unlockAtStartup_,"/preferences/unlockCredentialsAtStartup");
 notifications_=new QCheckBox("Show a notification when a link is ready");notifications_->setObjectName("notifications");setting(preferences,"Notifications",notifications_,"/preferences/notifications");
 uploadClipboard_=choices(this,"uploadClipboardBehavior",{});uploadClipboard_->addItem("Copy each link as it finishes","each_link");uploadClipboard_->addItem("Copy a list when uploads finish","list");setting(preferences,"After uploading",uploadClipboard_,"/preferences/uploadClipboardBehavior");
 auto* files=section(general,"Local copies","Save captures and clipboard imports locally, with or without uploading to a destination.");
 keep_=new QCheckBox("Keep a local copy");keep_->setObjectName("keepUploadCopies");setting(files,"Local copies",keep_,"/archive/keepUploads");
 root_=input(this,"archiveRoot");auto* rootRow=setting(files,"Save to",root_,"/archive/root");
 auto browse=[this](QLineEdit* field){const auto path=QFileDialog::getExistingDirectory(this,"Choose output folder",field->text());if(!path.isEmpty()){field->setText(path);queueEdit(field);}};
 auto* chooseRoot=new QPushButton("Choose folder…");chooseRoot->setAccessibleName("Choose default output folder");qobject_cast<QVBoxLayout*>(rootRow->layout())->addWidget(chooseRoot,0,Qt::AlignLeft);connect(chooseRoot,&QPushButton::clicked,this,[=,this]{browse(root_);});
 for(const auto& category:QStringList{"images","videos","text","other"}){auto* field=folders_[category]=input(this,"folder"+title(category));field->setPlaceholderText("Use default folder");auto* row=setting(files,title(category),field,"/archive/folders/"+category);auto* button=new QPushButton("Browse…");button->setAccessibleName("Choose "+category+" folder");qobject_cast<QHBoxLayout*>(row->layout()->itemAt(0)->layout())->addWidget(button);connect(button,&QPushButton::clicked,this,[=,this]{browse(field);});}
 // Diagnostics (#122): what the persistent log holds and the package a maintainer needs.
 auto* diagnosticsForm=section(general,"Diagnostics","Logs and diagnostics packages stay on this computer. A package is a redacted .zip you can send to a maintainer.");
 debugLogging_=new QCheckBox("Keep detailed debug logs for 24 hours");debugLogging_->setObjectName("debugLogging");auto* debugRow=setting(diagnosticsForm,"Debug logging",debugLogging_,"/preferences/diagnostics/debugLogging");
 debugLoggingStatus_=new QLabel;debugLoggingStatus_->setObjectName("debugLoggingStatus");debugLoggingStatus_->setWordWrap(true);VisualStyle::role(debugLoggingStatus_,"muted");qobject_cast<QVBoxLayout*>(debugRow->layout())->addWidget(debugLoggingStatus_);
 auto* logsRow=new QWidget;auto* logsLayout=new QHBoxLayout(logsRow);logsLayout->setContentsMargins(0,0,0,0);logsLayout->setSpacing(8);logSummary_=new QLabel;logSummary_->setObjectName("logSummary");logSummary_->setWordWrap(true);logSummary_->setAccessibleName("Log folder size, file count and oldest record");logsLayout->addWidget(logSummary_,1);
 clearLogs_=new QPushButton("Clear logs…");clearLogs_->setObjectName("clearLogs");logsLayout->addWidget(clearLogs_,0,Qt::AlignTop);
 diagnosticsForm->addRow(VisualStyle::settingLabel("Logs",settingHelp({},"logSummary"),logSummary_,this),logsRow);
 loggingWarning_=new QLabel;loggingWarning_->setObjectName("loggingDegraded");loggingWarning_->setWordWrap(true);VisualStyle::role(loggingWarning_,"warning");loggingWarning_->hide();diagnosticsForm->addRow(loggingWarning_);
 uncleanExit_=new QLabel;uncleanExit_->setObjectName("uncleanExit");uncleanExit_->setWordWrap(true);uncleanExit_->setAccessibleName("Last unclean exit");diagnosticsForm->addRow(VisualStyle::settingLabel("Last unclean exit",settingHelp({},"uncleanExit"),uncleanExit_,this),uncleanExit_);
 auto* createPackage=new QPushButton(VisualStyle::icon("save"),"Create diagnostics package…");createPackage->setObjectName("createDiagnosticsPackage");createPackage->setToolTip("Preview and save a redacted .zip of recent failures, settings and logs. Nothing is uploaded.");diagnosticsForm->addRow(createPackage);
 connect(clearLogs_,&QPushButton::clicked,this,[this]{if(QMessageBox::question(this,"Clear logs","Delete every put.gd log file? Uploads, history, settings and saved diagnostics packages stay.")!=QMessageBox::Yes)return;clearLogs({});});
 connect(createPackage,&QPushButton::clicked,this,[this]{openDiagnosticsPackage();});
 auto* captureDefaults=section(capture,"Default capture output","Choose where captures and clipboard imports upload. Local copies are controlled separately in General → Local copies.");
 captureProfile_=choices(this,"defaultCaptureDestination",{});setting(captureDefaults,"Destination",captureProfile_,{},"Local copies are independent of this destination.");
 captureSharing_=choices(this,"defaultCaptureSharing",{});for(const auto& mode:QStringList{"default","share","private"})captureSharing_->addItem(sharingLabel(mode),mode);setting(captureDefaults,"Sharing",captureSharing_,{});
 captureOutputNotice_=new QLabel;captureOutputNotice_->setObjectName("captureOutputNotice");captureOutputNotice_->setWordWrap(true);captureDefaults->addRow(captureOutputNotice_);
 connect(keep_,&QCheckBox::toggled,this,[this]{if(!loading_)updateCaptureInheritance();});
 captureExceptions_=new QToolButton;captureExceptions_->setObjectName("captureExceptionsToggle");captureExceptions_->setText("Per-action exceptions");captureExceptions_->setCheckable(true);captureExceptions_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);captureExceptions_->setArrowType(Qt::RightArrow);capture->addWidget(captureExceptions_,0,Qt::AlignLeft);
 auto* exceptionBody=new QWidget;auto* exceptionBox=new QVBoxLayout(exceptionBody);exceptionBox->setContentsMargins(0,0,0,0);exceptionBox->setSpacing(16);capture->addWidget(exceptionBody);exceptionBody->hide();
 connect(captureExceptions_,&QToolButton::toggled,this,[=,this](bool open){exceptionBody->setVisible(open);captureExceptions_->setArrowType(open?Qt::DownArrow:Qt::RightArrow);emit viewStateChanged();});
 for(const auto& action:Settings::actionNames()){
  auto* outer=section(exceptionBox,actionLabel(action));auto* inherit=useCaptureDefault_[action]=new QCheckBox("Use default");inherit->setObjectName("useDefault"+title(action));inherit->setAccessibleName(actionLabel(action)+": use default output");outer->addRow(inherit);
  auto* editor=captureEditors_[action]=new QWidget;editor->setObjectName("captureEditor"+title(action));auto* form=new QFormLayout(editor);VisualStyle::styleForm(form);outer->addRow(editor);
  actionProfiles_[action]=choices(this,"profile"+title(action),{});setting(form,"Destination",actionProfiles_[action],"/actions/"+action+"/profileId");
  publicationModes_[action]=choices(this,"publication"+title(action),{});for(const auto& mode:QStringList{"default","share","private"})publicationModes_[action]->addItem(sharingLabel(mode),mode);setting(form,"Sharing",publicationModes_[action],"/actions/"+action+"/publicationMode");
  auto* status=captureStatus_[action]=new QLabel;status->setObjectName("captureStatus"+title(action));status->setWordWrap(true);form->addRow(status);
  connect(inherit,&QCheckBox::toggled,this,[this,inherit]{if(!loading_)queueCaptureEdit(inherit);});
 }
 captureProfile_->setModel(actionProfiles_["region"]->model());
 for(auto* combo:{captureProfile_,captureSharing_})connect(combo,&QComboBox::currentIndexChanged,this,[this,combo]{if(!loading_)queueCaptureEdit(combo);});
 auto* namingForm=section(capture,"File names");naming_=choices(this,"namingMode",{});naming_->addItem("Keep original names","original");naming_->addItem("Random names","generated");naming_->addItem("Custom template","template");setting(namingForm,"Naming",naming_,"/naming/mode");
 collision_=choices(this,"collisionPolicy",{"rename","skip","overwrite"});for(int i=0;i<collision_->count();++i)collision_->setItemData(i,collision_->itemText(i));collision_->setItemText(0,"Add a unique suffix");collision_->setItemText(1,"Skip duplicate names");collision_->setItemText(2,"Replace the checked object");setting(namingForm,"When a name exists",collision_,"/naming/collisionPolicy");
 for(const auto& key:QStringList{"fileTemplate","captureTemplate","imageTemplate","textTemplate"}){templates_[key]=input(this,key);setting(namingForm,key=="fileTemplate"?"Imported files":key=="captureTemplate"?"Screen captures":key=="imageTemplate"?"Clipboard images":"Clipboard text",templates_[key],"/naming/"+key);}
 auto* shortcutForm=section(shortcutsPage,"Global shortcuts","Click Record shortcut, press a combination, then release all keys. Escape cancels; Backspace clears.");shortcuts_=new ShortcutEditor(this);shortcutForm->addRow(shortcuts_);shortcuts_->findChild<QToolButton*>("shortcutDetailsToggle")->hide();shortcuts_->setRecordingControl([session=&settings_,editor=shortcuts_](bool active,std::function<void(Error)> reply){session->setShortcutRecording(active,editor,std::move(reply));});
 for(const auto& action:Settings::invocableActionNames()){auto* edit=shortcuts_->findChild<QLineEdit*>("shortcut"+title(action));bind(edit,"/actions/"+action+"/shortcut");}
 auto* intro=new QHBoxLayout;auto* introText=new QLabel("Defaults work for almost everyone. Changed values are marked so you can find them later.");introText->setWordWrap(true);VisualStyle::role(introText,"muted");intro->addWidget(introText,1);auto* resetAll=new QPushButton("Reset all to defaults");resetAll->setObjectName("resetAdvancedDefaults");intro->addWidget(resetAll);advancedPage->addLayout(intro);
 auto* columns=new QHBoxLayout;columns->setSpacing(18);advancedPage->addLayout(columns);auto* left=new QVBoxLayout;auto* right=new QVBoxLayout;columns->addLayout(left,1);columns->addLayout(right,1);
 const auto defaults=Settings::defaults();
 auto number=[&](QFormLayout* form,const QString& key,const QString& name,const QString& unit){auto* edit=numbers_[key]=input(this,key);QWidget* editor=edit;if(key=="historyRetentionDays"){auto* combo=choices(this,"historyRetentionChoice",{});combo->setEditable(true);combo->setInsertPolicy(QComboBox::NoInsert);combo->addItem("Forever",QString{});combo->addItem("30 days","30");combo->addItem("90 days","90");combo->addItem("1 year","365");combo->setLineEdit(edit);editor=combo;connect(combo,&QComboBox::activated,this,[this,combo,edit](int index){edit->setText(combo->itemData(index).toString());queueEdit(edit);});}
  // File, batch and waiting-upload sizes: Auto follows this computer's memory, an empty field is No limit.
  else if(Settings::artifactByteLimitKeys().contains(key)){auto* combo=choices(this,key+"Choice",{});combo->setEditable(true);combo->setInsertPolicy(QComboBox::NoInsert);combo->addItem("Auto","Auto");combo->addItem("No limit",QString{});combo->setLineEdit(edit);edit->setPlaceholderText("No limit");editor=combo;connect(combo,&QComboBox::activated,this,[this,combo,edit](int index){edit->setText(combo->itemData(index).toString());queueEdit(edit);});}
  const bool retention=defaults["retention"].toObject().contains(key);auto* wrap=setting(form,name,editor,QString(retention?"/retention/":"/limits/")+key,{},unit);if(editor!=edit){errors_[edit]=errors_[editor];bind(edit,QString(retention?"/retention/":"/limits/")+key);}const QJsonValue value=retention?defaults["retention"].toObject()[key]:defaults["limits"].toObject()[key];const qint64 factor=displayUnit(key);const auto text=value.isNull()?QString{}:value.isString()?QStringLiteral("Auto"):ByteSize::exact(value.toInteger(),factor);edit->setProperty("defaultText",text);auto* reset=new QPushButton("Reset");reset->setAccessibleName("Reset "+name+" to default");auto* state=new QLabel("Default");VisualStyle::role(state,"muted");auto* row=qobject_cast<QHBoxLayout*>(wrap->layout()->itemAt(0)->layout());row->addWidget(state);row->addWidget(reset);auto update=[=,this]{const bool changed=edit->text()!=text;state->setVisible(!changed);reset->setVisible(changed);wrap->setProperty("changed",changed);wrap->style()->unpolish(wrap);wrap->style()->polish(wrap);};connect(edit,&QLineEdit::textChanged,this,update);connect(reset,&QPushButton::clicked,this,[=,this]{edit->setText(text);queueEdit(edit);});update();};
 auto* uploads=section(left,"Uploads");number(uploads,"concurrentUploads","Uploads at once",{});number(uploads,"maxFileBytes","Largest file","GiB");number(uploads,"maxBatchBytes","Largest batch","GiB");number(uploads,"pendingArtifactBytes","Waiting uploads","GiB");number(uploads,"maxFiles","Files in a batch",{});number(uploads,"pendingOperations","Waiting operations",{});
 auto* images=section(left,"Images and clipboard");number(images,"imageBytes","Largest image","MiB");number(images,"imagePixels","Largest image size","megapixels");number(images,"clipboardTextBytes","Clipboard text limit","MiB");
 auto* memory=section(right,"Memory and cache");number(memory,"pendingImageBytes","Memory for queued images","MiB");number(memory,"previewCacheBytes","Thumbnail cache","MiB");
 auto* retention=section(right,"History");number(retention,"historyRetentionDays","Keep history for","days");numbers_["historyRetentionDays"]->setPlaceholderText("Forever");number(retention,"failedArtifactDays","Keep failed uploads for","days");number(retention,"recentActivityLimit","Items in recent list",{});
 connect(resetAll,&QPushButton::clicked,this,[this]{if(QMessageBox::question(this,"Reset advanced settings","Reset upload, image, memory, cache and history limits to their defaults?")!=QMessageBox::Yes)return;for(auto* field:numbers_){field->setText(field->property("defaultText").toString());queueEdit(field);}});
 auto* config=section(right,"Config file");configPath_=new QLabel;configPath_->setWordWrap(true);configPath_->setTextInteractionFlags(Qt::TextSelectableByMouse);configPath_->setProperty("visualRole","mono");config->addRow(configPath_);auto* configActions=new QHBoxLayout;auto* editor=new QPushButton("Open in editor");auto* reload=new QPushButton("Reload from file");reload->setObjectName("reloadSettings");configActions->addWidget(editor);configActions->addWidget(reload);config->addRow(configActions);connect(editor,&QPushButton::clicked,this,[this]{QDesktopServices::openUrl(QUrl::fromLocalFile(settings_.path()));});connect(reload,&QPushButton::clicked,this,[this]{resetDraft(true);});
 auto* diagnostics=section(advancedPage,"Desktop support details");capabilities_=new QLabel;capabilities_->setAccessibleName("Desktop support diagnostics");capabilities_->setWordWrap(true);capabilities_->setTextInteractionFlags(Qt::TextSelectableByMouse);VisualStyle::role(capabilities_,"mono");diagnostics->addRow(capabilities_);metadata_=new QLabel;metadata_->setObjectName("settingsMetadata");metadata_->setWordWrap(true);metadata_->setTextInteractionFlags(Qt::TextSelectableByMouse);VisualStyle::role(metadata_,"muted");diagnostics->addRow(metadata_);auto* diagnose=new QPushButton("Refresh diagnostics");auto* copyDiagnostics=new QPushButton("Copy diagnostics");auto* diagnosticActions=new QHBoxLayout;diagnosticActions->addWidget(diagnose);diagnosticActions->addWidget(copyDiagnostics);diagnosticActions->addStretch();diagnostics->addRow(diagnosticActions);connect(diagnose,&QPushButton::clicked,this,[this]{settings_.refreshSnapshot();});connect(copyDiagnostics,&QPushButton::clicked,this,[this]{qApp->clipboard()->setText(capabilities_->text()+"\n"+metadata_->text());});
 destinationPage_=new QWidget(this);destinationPage_->setObjectName("destinationsPage");destinationPage_->installEventFilter(this);auto* destinationLayout=new QHBoxLayout(destinationPage_);destinationLayout->setContentsMargins(0,0,0,0);destinationLayout->setSpacing(0);
 auto* rail=new QWidget;rail->setObjectName("destinationRail");rail->setFixedWidth(230);auto* railLayout=new QVBoxLayout(rail);railLayout->setContentsMargins(16,24,16,16);auto* railTitle=new QLabel("DESTINATIONS");VisualStyle::role(railTitle,"eyebrow");railLayout->addWidget(railTitle);auto* destinationList=new DestinationList;destinationList_=destinationList;destinationList_->setObjectName("destinationList");destinationList_->setAccessibleName("Destinations");destinationList_->setWordWrap(false);destinationList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);railLayout->addWidget(destinationList_,1);auto* add=new QPushButton("+ Add destination");add->setObjectName("addS3Destination");railLayout->addWidget(add);destinationLayout->addWidget(rail);
 auto* destinationBody=new QWidget;auto* pageLayout=new QVBoxLayout(destinationBody);pageLayout->setContentsMargins(24,24,20,24);pageLayout->setSpacing(18);pageLayout->setAlignment(Qt::AlignTop);auto* destinationScroll=new QScrollArea;destinationScroll->setObjectName("destinationScroll");destinationScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);destinationScroll->setWidgetResizable(true);destinationScroll->setFrameShape(QFrame::NoFrame);destinationScroll->setWidget(destinationBody);destinationLayout->addWidget(destinationScroll,1);
 profile_=choices(this,"destinationProfile",{});profile_->hide();profileId_=input(this,"destinationId");profileId_->hide();providerJson_=new QPlainTextEdit(this);providerJson_->setObjectName("providerSettingsJson");providerJson_->hide();
 auto* titleRow=new QHBoxLayout;profileLabel_=input(this,"destinationLabel");profileLabel_->setProperty("visualRole","editableTitle");profileLabel_->setPlaceholderText("Destination name");profileLabel_->setAccessibleName("Destination name");bind(profileLabel_,"/profiles/{profile}/label");titleRow->addWidget(profileLabel_,1);titleRow->addWidget(VisualStyle::helpButton("Destination name",settingHelp("/profiles/{profile}/label",{}),profileLabel_,this));auto* test=new QPushButton("Test connection");test->setObjectName("testDestination");titleRow->addWidget(test);defaultButton_=new QPushButton("Make default");defaultButton_->setObjectName("makeDefaultDestination");titleRow->addWidget(defaultButton_);auto* more=new QPushButton("⋯");more->setAccessibleName("Destination actions");titleRow->addWidget(more);pageLayout->addLayout(titleRow);
 auto* description=new QLabel("Amazon S3 or compatible storage. Existing uploads keep their original destination.");description->setWordWrap(true);VisualStyle::role(description,"muted");pageLayout->addWidget(description);
 auto* destinationMenu=new QMenu(more);more->setMenu(destinationMenu);auto* serviceMenu=destinationMenu->addMenu("Storage service");connect(serviceMenu->addAction("Amazon S3 / compatible"),&QAction::triggered,this,[this]{providerType_->setCurrentIndex(providerType_->findData("s3"));});connect(serviceMenu->addAction("Proton Drive"),&QAction::triggered,this,[this]{providerType_->setCurrentIndex(providerType_->findData("proton-drive"));});auto* remove=new QPushButton("Remove destination",this);remove->setObjectName("removeDestination");remove->hide();connect(destinationMenu->addAction("Remove destination…"),&QAction::triggered,remove,&QPushButton::click);destinationMenu->addSeparator();moveUp_=destinationMenu->addAction("Move up");moveDown_=destinationMenu->addAction("Move down");
 for(auto* move:{moveUp_,moveDown_}){move->setObjectName(move==moveUp_?"moveDestinationUp":"moveDestinationDown");move->setShortcut(QKeySequence(Qt::ALT|(move==moveUp_?Qt::Key_Up:Qt::Key_Down)));move->setShortcutContext(Qt::WidgetShortcut);destinationList_->addAction(move);connect(move,&QAction::triggered,this,[this,move]{QJsonObject result;if(auto error=moveSelectedDestination(move==moveUp_?-1:1,{},result))status(error);});}
 destinationList->moveRequested=[this](int from,int to){if(auto error=moveDestination(from,to))status(error);};
 destinationMenu->addSeparator();connect(destinationMenu->addAction("Copy details"),&QAction::triggered,this,[this]{qApp->clipboard()->setText(QString::fromUtf8(QJsonDocument(editedProfile()).toJson(QJsonDocument::Indented)));});
 auto* dev=new QPushButton("Import development S3",this);dev->setObjectName("importDevelopmentS3");dev->hide();auto* addMenu=new QMenu(add);add->setMenu(addMenu);connect(addMenu->addAction("Amazon S3 / S3-compatible"),&QAction::triggered,this,[this]{addS3Profile();});connect(addMenu->addAction("Proton Drive"),&QAction::triggered,this,[this]{const auto count=profile_->count();addS3Profile("Proton Drive");if(profile_->count()==count)return;providerType_->setCurrentIndex(providerType_->findData("proton-drive"));queueEdit(profileLabel_);});
#ifndef NDEBUG
 connect(addMenu->addAction("Import development S3"),&QAction::triggered,dev,&QPushButton::click);
#endif
 auto* destinationDetails=section(pageLayout,"Where files go");destinationDetails->parentWidget()->setObjectName("destinationEditor");
 providerType_=choices(this,"storageProvider",{});providerType_->addItem("Amazon S3","s3");providerType_->addItem("Proton Drive","proton-drive");setting(destinationDetails,"Storage service",providerType_,"/profiles/{profile}");
 auto* s3Service=choices(this,"s3Service",{"Amazon S3","Cloudflare R2","Backblaze B2","Other S3-compatible"});s3Service->setAccessibleName("S3-compatible service");destinationDetails->addRow(VisualStyle::settingLabel("S3 service",settingHelp({},"s3Service"),s3Service,this),s3Service);
 const QHash<QString,QString> s3Labels{{"bucket","Bucket"},{"region","Region"},{"endpoint","Endpoint"},{"objectPrefix","Folder in bucket"},{"publicUrlBase","Link domain"},{"storageClass","Storage class"},{"connectTimeoutMs","Connect timeout"},{"requestTimeoutMs","Request timeout"},{"lowSpeedBytesPerSecond","Minimum transfer speed"},{"lowSpeedTimeSeconds","Low-speed window"}};
 for(const auto& key:QStringList{"bucket","region","endpoint","objectPrefix","publicUrlBase","storageClass","connectTimeoutMs","requestTimeoutMs","lowSpeedBytesPerSecond","lowSpeedTimeSeconds"})s3_[key]=input(this,"s3"+title(key));
 auto* bucketHint=setting(destinationDetails,"Bucket",s3_["bucket"],"/profiles/{profile}/settings/bucket","Required before uploads can go here.");Q_UNUSED(bucketHint);s3_["bucket"]->setPlaceholderText("my-screenshots");
 regionChoices_=choices(this,"s3RegionChoices",s3Regions());regionChoices_->setEditable(true);regionChoices_->setInsertPolicy(QComboBox::NoInsert);regionChoices_->setLineEdit(s3_["region"]);setting(destinationDetails,"Region",regionChoices_,"/profiles/{profile}/settings/region");bind(s3_["region"],"/profiles/{profile}/settings/region");
 endpointHint_=new QLabel;endpointHint_->setWordWrap(true);VisualStyle::role(endpointHint_,"muted");destinationDetails->addRow("",endpointHint_);auto* customEndpoint=new QCheckBox("Use a custom endpoint");customEndpoint->setObjectName("customEndpoint");auto* customEndpointRow=VisualStyle::settingRow("Use a custom endpoint",settingHelp({},"customEndpoint"),customEndpoint,this);destinationDetails->addRow(customEndpointRow);setting(destinationDetails,"Endpoint",s3_["endpoint"],"/profiles/{profile}/settings/endpoint");connect(s3Service,&QComboBox::activated,this,[=,this](int index){if(index){customEndpoint->setChecked(true);s3_["endpoint"]->setPlaceholderText(index==1?"https://ACCOUNT.r2.cloudflarestorage.com":index==2?"https://s3.REGION.backblazeb2.com":"https://your-storage-endpoint");s3_["endpoint"]->setFocus();}});
 setting(destinationDetails,"Folder in bucket",s3_["objectPrefix"],"/profiles/{profile}/settings/objectPrefix","Optional. Leave empty to upload to the bucket root.");
 auto* links=section(pageLayout,"Links");linksPanel_=links->parentWidget();acl_=choices(this,"s3AclMode",{"public-read","omit"});for(int i=0;i<acl_->count();++i)acl_->setItemData(i,acl_->itemText(i));acl_->setItemText(0,"Request public read access");acl_->setItemText(1,"Follow the bucket's policy (send no ACL)");setting(links,"Who can open files",acl_,"/profiles/{profile}/settings/aclMode");setting(links,"Link domain",s3_["publicUrlBase"],"/profiles/{profile}/settings/publicUrlBase");s3_["publicUrlBase"]->setPlaceholderText("https://i.example.com");linkExample_=new QLabel;linkExample_->setWordWrap(true);VisualStyle::role(linkExample_,"link");links->addRow("",linkExample_);
 auto* advanced=new QToolButton;advanced->setObjectName("advancedSettings");advanced->setText("Advanced connection settings");advanced->setAccessibleName("Advanced connection settings");advanced->setCheckable(true);advanced->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);advanced->setArrowType(Qt::RightArrow);pageLayout->addWidget(advanced);auto* connectionSummary=new QLabel;connectionSummary->setWordWrap(true);VisualStyle::role(connectionSummary,"muted");pageLayout->addWidget(connectionSummary);auto* connection=section(pageLayout,"Connection settings");connectionPanel_=connection->parentWidget();
 addressing_=choices(this,"s3Addressing",{"path","virtual","auto"});for(int i=0;i<addressing_->count();++i)addressing_->setItemData(i,addressing_->itemText(i));addressing_->setItemText(0,"Bucket in URL path");addressing_->setItemText(1,"Bucket in hostname");addressing_->setItemText(2,"Automatic");
 for(const auto& key:QStringList{"storageClass","connectTimeoutMs","requestTimeoutMs","lowSpeedBytesPerSecond","lowSpeedTimeSeconds"}){auto* field=s3_[key];const auto suffix=key.endsWith("Ms")||key=="lowSpeedTimeSeconds"?"seconds":key=="lowSpeedBytesPerSecond"?"bytes/s":"";auto* row=setting(connection,s3Labels[key],field,"/profiles/{profile}/settings/"+key,{},suffix);const QJsonValue value=s3Profile("default")["settings"].toObject()[key];const auto text=value.isDouble()?ByteSize::exact(value.toInteger(),key.endsWith("Ms")?1000:1):value.toString();auto* reset=new QPushButton("Reset");reset->setAccessibleName("Reset "+s3Labels[key]);auto* state=new QLabel("Default");VisualStyle::role(state,"muted");auto* line=qobject_cast<QHBoxLayout*>(row->layout()->itemAt(0)->layout());line->addWidget(state);line->addWidget(reset);connect(field,&QLineEdit::textChanged,this,[=,this]{bool changed=field->text()!=text;state->setVisible(!changed);reset->setVisible(changed);row->setProperty("changed",changed);row->style()->unpolish(row);row->style()->polish(row);});connect(reset,&QPushButton::clicked,this,[=,this]{field->setText(text);queueEdit(field);});}
 auto* addressRow=setting(connection,"Addressing",addressing_,"/profiles/{profile}/settings/addressing");auto* addressDefault=new QLabel("Default");VisualStyle::role(addressDefault,"muted");auto* resetAddress=new QPushButton("Reset");resetAddress->setAccessibleName("Reset addressing to automatic");auto* addressLine=qobject_cast<QHBoxLayout*>(addressRow->layout()->itemAt(0)->layout());addressLine->addWidget(addressDefault);addressLine->addWidget(resetAddress);connect(addressing_,&QComboBox::currentIndexChanged,this,[=,this]{const bool changed=addressing_->currentData()!="auto";addressDefault->setVisible(!changed);resetAddress->setVisible(changed);addressRow->setProperty("changed",changed);addressRow->style()->unpolish(addressRow);addressRow->style()->polish(addressRow);});connect(resetAddress,&QPushButton::clicked,this,[this]{addressing_->setCurrentIndex(addressing_->findData("auto"));});reference_=input(this,"credentialReference");setting(connection,"Secret-store reference",reference_,"/profiles/{profile}/credentials/reference");
 auto* credentialForm=section(pageLayout,"Credentials");auto* credentialsEditor=credentialForm->parentWidget();credentialsEditor->setObjectName("credentialsEditor");access_=input(this,"credentialAccessKey");secret_=input(this,"credentialSecretKey");token_=input(this,"credentialSessionToken");secret_->setEchoMode(QLineEdit::Password);token_->setEchoMode(QLineEdit::Password);setting(credentialForm,"Access key ID",access_,{});auto* secretRow=setting(credentialForm,"Secret access key",secret_,{});showSecret_=new QPushButton("Show");showSecret_->setObjectName("showCredentialSecret");showSecret_->setCheckable(true);showSecret_->setAccessibleName("Show secret access key");qobject_cast<QHBoxLayout*>(secretRow->layout()->itemAt(0)->layout())->addWidget(showSecret_);connect(showSecret_,&QPushButton::toggled,this,&SettingsWindow::toggleCredentialSecret);setting(credentialForm,"Session token",token_,{},"Optional, for temporary AWS credentials.");credentialMode_=choices(this,"credentialMode",{"keychain","session"});for(int i=0;i<credentialMode_->count();++i)credentialMode_->setItemData(i,credentialMode_->itemText(i));credentialMode_->setItemText(0,"System keychain");credentialMode_->setItemText(1,"This session only");setting(credentialForm,"Stored in",credentialMode_,"/profiles/{profile}/credentials");
 auto* credentialActions=new QHBoxLayout;auto* credentials=new QPushButton("Store credentials");credentials->setObjectName("storeCredentials");auto* unlock=new QPushButton("Unlock saved credentials");credentialActions->addWidget(credentials);credentialActions->addWidget(unlock);credentialForm->addRow(credentialActions);credentialHint_=new QLabel;credentialHint_->setObjectName("storedCredentialHint");credentialHint_->setWordWrap(true);credentialHint_->setAccessibleName("Stored credential identification");VisualStyle::role(credentialHint_,"muted");credentialForm->addRow(credentialHint_);
 protonPanel_=new QWidget;protonPanel_->setObjectName("protonDestinationEditor");auto* protonLayout=new QVBoxLayout(protonPanel_);protonLayout->setContentsMargins(0,0,0,0);protonLayout->setSpacing(18);pageLayout->addWidget(protonPanel_);auto* protonForm=section(protonLayout,"Account");auto* folderForm=section(protonLayout,"Where files go");protonFolderStatus_=new QLabel;protonFolderStatus_->setObjectName("protonFolderStatus");protonFolderStatus_->setWordWrap(true);protonFolderStatus_->hide();folderForm->addRow(protonFolderStatus_);auto* publicationForm=section(protonLayout,"After upload");auto* protonAdvancedForm=section(protonLayout,"Link settings");protonAccount_=new QLabel("Signed out");protonAccount_->setObjectName("protonAccountStatus");protonAccount_->setWordWrap(true);protonForm->addRow("Account",protonAccount_);auto* connectProton=new QPushButton("Connect account…");connectProton->setObjectName("connectProtonAccount");connectProton->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Fixed);auto* inspectProton=new QPushButton("Refresh folders");inspectProton->setObjectName("inspectProtonAccount");inspectProton->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Fixed);inspectProton->setToolTip("Reload the available Proton folders without changing the selected upload folder.");protonForm->addRow(connectProton);
 protonFolder_=choices(this,"protonFolder",{});auto* folderRow=setting(folderForm,"Upload folder",protonFolder_,"/profiles/{profile}/settings");qobject_cast<QHBoxLayout*>(folderRow->layout()->itemAt(0)->layout())->addWidget(inspectProton);auto* createProton=new QPushButton("New folder…");createProton->setObjectName("createProtonFolder");createProton->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Fixed);folderForm->addRow(createProton);protonShare_=new QCheckBox("Create a share link and copy it");protonShare_->setObjectName("protonShareDefault");protonShare_->setParent(this);bind(protonShare_,"/profiles/{profile}/settings/shareByDefault");auto* privateMode=new QRadioButton("Keep private, copy nothing");auto* publicMode=new QRadioButton("Create a share link and copy it");protonShare_->hide();publicationForm->addRow(VisualStyle::settingRow(publicMode->text(),"Create a share link for future Proton uploads and copy it. Link expiration and password apply to new shares; existing links remain unchanged.",publicMode,this));publicationForm->addRow(VisualStyle::settingRow(privateMode->text(),"Upload privately without creating a link or changing the clipboard. Existing member or folder access still applies.",privateMode,this));connect(publicMode,&QRadioButton::clicked,this,[this]{protonShare_->setChecked(true);});connect(privateMode,&QRadioButton::clicked,this,[this]{protonShare_->setChecked(false);});connect(protonShare_,&QCheckBox::toggled,this,[=,this](bool share){publicMode->setChecked(share);privateMode->setChecked(!share);if(!loading_)updateCaptureInheritance();});
 protonExpiration_=new QSpinBox;protonExpiration_->setObjectName("protonExpiration");protonExpiration_->setRange(0,365);protonExpiration_->setSpecialValueText("No expiration");protonExpiration_->setSuffix(" days");setting(protonAdvancedForm,"Link expiration",protonExpiration_,"/profiles/{profile}/settings/linkExpirationDays");protonTimeout_=new QDoubleSpinBox;protonTimeout_->setObjectName("protonRequestTimeout");protonTimeout_->setDecimals(3);protonTimeout_->setRange(1,ProtonProvider::MaximumRequestTimeoutMs/1000);protonTimeout_->setSuffix(" seconds");setting(protonAdvancedForm,"Request deadline",protonTimeout_,"/profiles/{profile}/settings/requestTimeoutMs");protonPassword_=input(this,"protonSharePassword");protonPassword_->setEchoMode(QLineEdit::Password);setting(protonAdvancedForm,"New link password",protonPassword_,{});auto* storePassword=new QPushButton("Store link password");storePassword->setObjectName("storeProtonPassword");protonAdvancedForm->addRow(storePassword);auto* clearPassword=new QPushButton("Use no link password");clearPassword->setObjectName("clearProtonPassword");protonAdvancedForm->addRow(clearPassword);
 connect(connectProton,&QPushButton::clicked,this,[this]{protonBrowserOpened_=false;beginSetup("connectAccount");});connect(inspectProton,&QPushButton::clicked,this,[this]{beginSetup("inspectAccount");});connect(createProton,&QPushButton::clicked,this,[this]{bool ok=false;const auto name=QInputDialog::getText(this,"New Proton folder","Folder name",QLineEdit::Normal,"put.gd",&ok);if(ok&&!name.trimmed().isEmpty())beginSetup("createFolder",{{"name",name.trimmed()}});});
 connect(storePassword,&QPushButton::clicked,this,[this]{const auto reference="putgd/proton/link/"+QUuid::createUuid().toString(QUuid::Id128);auto value=protonPassword_->text();protonPassword_->clear();beginSetup("storeSharePassword",{{"password",value},{"reference",reference}});value.fill(QChar{});});connect(clearPassword,&QPushButton::clicked,this,[this]{auto profiles=draft_["profiles"].toArray();if(profileIndex_<0||profileIndex_>=profiles.size())return;auto p=profiles[profileIndex_].toObject();auto s=p["settings"].toObject();s["sharePasswordReference"]=QJsonValue::Null;p["settings"]=s;profiles[profileIndex_]=p;draft_["profiles"]=profiles;queueEdit(protonShare_,"/profiles/{profile}/settings");});
 auto formRow=[this](QWidget* field)->QWidget*{return errors_.contains(field)?errors_[field]->parentWidget():field;};
 updateProviderVisibility_=[=,this]{const bool s3=providerType_->currentData()=="s3";const bool expanded=advanced->isChecked();advanced->setText(s3?"Advanced connection settings":"Advanced link settings");advanced->setArrowType(expanded?Qt::DownArrow:Qt::RightArrow);description->setText(s3?"Amazon S3 or compatible storage. Existing uploads keep their original destination.":"Third-party Proton Drive integration. Share links open a Proton page.");for(auto* field:{s3_["bucket"],s3_["objectPrefix"]})destinationDetails->setRowVisible(formRow(field),s3);destinationDetails->parentWidget()->setVisible(s3);destinationDetails->setRowVisible(formRow(providerType_),false);destinationDetails->setRowVisible(s3Service,s3);destinationDetails->setRowVisible(formRow(regionChoices_),s3);destinationDetails->setRowVisible(endpointHint_,s3);destinationDetails->setRowVisible(customEndpointRow,s3);destinationDetails->setRowVisible(formRow(s3_["endpoint"]),s3&&customEndpoint->isChecked());linksPanel_->setVisible(s3);connectionPanel_->setVisible(s3&&expanded);credentialsEditor->setVisible(s3);protonPanel_->setVisible(!s3);test->setVisible(s3||providerType_->currentData()=="proton-drive");test->setToolTip(s3?"Checks access, a test upload, public link and cleanup.":"Checks the signed-in Proton account and selected folder without uploading. Upload and sharing are not tested.");advanced->setVisible(true);connectionSummary->setVisible(s3);connectionSummary->setText(s3_["storageClass"]->text()+" · "+addressing_->currentText()+" · "+s3_["connectTimeoutMs"]->text()+" s connect · "+s3_["requestTimeoutMs"]->text()+" s request");protonAdvancedForm->parentWidget()->setVisible(expanded);endpointHint_->setText("Endpoint: "+s3_["endpoint"]->text());{const QSignalBlocker block(s3Service);const auto endpoint=s3_["endpoint"]->text();s3Service->setCurrentIndex(endpoint.contains("cloudflarestorage.com")?1:endpoint.contains("backblazeb2.com")?2:endpoint.contains("amazonaws.com")?0:3);}linkExample_->setText(publicObjectUrl(editedProfile()["settings"].toObject(),s3_["objectPrefix"]->text()+"Screenshot.png"));};
 pageLayout->removeWidget(advanced);pageLayout->removeWidget(connectionSummary);pageLayout->removeWidget(connectionPanel_);pageLayout->addWidget(advanced);pageLayout->addWidget(connectionSummary);pageLayout->addWidget(connectionPanel_);
 connect(advanced,&QToolButton::toggled,this,[this]{updateProviderVisibility_();emit viewStateChanged();});connect(customEndpoint,&QCheckBox::toggled,this,[this]{updateProviderVisibility_();});
 connect(providerType_,&QComboBox::currentIndexChanged,this,&SettingsWindow::switchProvider);
 connect(destinationList_,&QListWidget::currentRowChanged,this,[this](int index){if(!loading_)profile_->setCurrentIndex(index);});connect(profile_,&QComboBox::currentIndexChanged,this,[this]{if(loading_)return;storeProfile();loadProfile();refreshDestinationList();if(destinationPage_->isVisible())activateDestinations();emit viewStateChanged();});
 connect(defaultButton_,&QPushButton::clicked,this,[this]{if(testedDestinations_.value(profileId_->text())!=editedProfile()&&QMessageBox::question(this,"Use this destination?","This connection has not passed a test in this session. Use it as the capture default? Per-action exceptions will stay unchanged.")!=QMessageBox::Yes)return;{const QSignalBlocker block(captureProfile_);captureProfile_->setCurrentIndex(captureProfile_->findData(profileId_->text()));}queueCaptureEdit(defaultButton_);});
 connect(remove,&QPushButton::clicked,this,[this]{if(QMessageBox::question(this,"Remove destination","Remove this destination from settings? Existing history and remote files remain.")!=QMessageBox::Yes)return;storeProfile();draft_=candidate();auto profiles=draft_["profiles"].toArray();const int index=profile_->currentIndex();if(index<0||index>=profiles.size())return;const auto id=profiles[index].toObject()["id"].toString();if(captureProfile_->currentData()==id){const QSignalBlocker block(captureProfile_);captureProfile_->setCurrentIndex(0);updateCaptureInheritance();}for(const auto& action:Settings::actionNames())if(actionProfiles_[action]->currentData()==id){actionProfiles_[action]->setCurrentIndex(0);}captureDefaultsEnabled_=true;captureRoutingEdited_=true;profiles.removeAt(index);if(index<profileEditors_.size())profileEditors_.removeAt(index);if(index<credentialDrafts_.size())credentialDrafts_.removeAt(index);draft_["profiles"]=profiles;rebuildProfiles(qMin(index,int(profiles.size())-1));queueEdit(defaultButton_,"/");refreshDestinationList();});
 connect(dev,&QPushButton::clicked,this,&SettingsWindow::importDevelopmentProfile);
 connect(s3_["region"],&QLineEdit::textChanged,this,[this](const QString& region){if(loading_)return;auto* endpoint=s3_["endpoint"];const auto previous=s3_["region"]->property("previousRegion").toString();if(endpoint->text()=="https://s3."+previous+".amazonaws.com")endpoint->setText("https://s3."+region+".amazonaws.com");s3_["region"]->setProperty("previousRegion",region);updateProviderVisibility_();});
 connect(credentials,&QPushButton::clicked,this,[this]{resetCredentialSecretDisplay();QJsonObject value{{"accessKey",access_->text()},{"secretKey",secret_->text()},{"sessionToken",token_->text()}};clearCurrentCredentialInputs();beginSetup("storeCredentials",value);});connect(unlock,&QPushButton::clicked,this,[this]{beginSetup("unlockCredentials");});connect(test,&QPushButton::clicked,this,[this]{beginSetup("testDestination");});
 status_=new QLabel;status_->setObjectName("settingsStatus");status_->setAccessibleName("Settings status");status_->setWordWrap(true);VisualStyle::role(status_,"muted");layout->addWidget(status_);destinationStatus_=new QLabel;destinationStatus_->setObjectName("destinationStatus");destinationStatus_->setWordWrap(true);destinationStatus_->setAccessibleName("Destination status");VisualStyle::role(destinationStatus_,"muted");pageLayout->insertWidget(1,destinationStatus_);
 for(int i=0;i<2;++i){auto* retry=new QPushButton("Retry saving");retry->setObjectName(i?"retryDestinationAutosave":"retryAutosave");retry->hide();retryAutosave_.append(retry);if(i)pageLayout->addWidget(retry,0,Qt::AlignLeft);else layout->addWidget(retry,0,Qt::AlignLeft);connect(retry,&QPushButton::clicked,this,[this,retry]{autosaveBlocked_=false;for(auto* button:retryAutosave_)button->hide();if(pendingEdits_.isEmpty()){for(const auto& group:QStringList{"archive","preferences","naming","limits","retention","actions","profiles","captureDefaults"})if((group!="profiles"||!initialProfile_)&&(group!="captureDefaults"||captureDefaultsEnabled_))queueEdit(retry,"/"+group);}save();});}
 quit_=new QPushButton("Quit put.gd");quit_->setObjectName("quitApplication");general->addWidget(quit_,0,Qt::AlignLeft);connect(quit_,&QPushButton::clicked,this,&SettingsWindow::quitRequested);
 autosave_=new QTimer(this);autosave_->setSingleShot(true);autosave_->setInterval(400);connect(autosave_,&QTimer::timeout,this,&SettingsWindow::save);
 for(auto* edit:findChildren<QLineEdit*>()){if(edit==access_||edit==secret_||edit==token_||edit==protonPassword_||edit->property("configPath").toString().isEmpty())continue;connect(edit,&QLineEdit::textChanged,this,[this,edit]{dirty();if(!edit->hasFocus()||edit->isReadOnly())queueEdit(edit);});connect(edit,&QLineEdit::editingFinished,this,[this,edit]{queueEdit(edit);});}
 for(auto* edit:{access_,secret_,token_,protonPassword_})connect(edit,&QLineEdit::textChanged,this,&SettingsWindow::viewStateChanged);
 for(auto* combo:findChildren<QComboBox*>())if(combo!=profile_&&!combo->property("configPath").toString().isEmpty())connect(combo,&QComboBox::currentIndexChanged,this,[this,combo]{queueEdit(combo);});
 for(auto* check:findChildren<QCheckBox*>())if(!check->property("configPath").toString().isEmpty())connect(check,&QCheckBox::toggled,this,[this,check]{queueEdit(check);});
 for(auto* spin:findChildren<QAbstractSpinBox*>())connect(spin,&QAbstractSpinBox::editingFinished,this,[this,spin]{queueEdit(spin);});
 connect(&settings_,&UiSession::accepted,this,[this]{configurationChanged();});connect(&settings_,&UiSession::statusChanged,this,[this]{configurationChanged();refreshDiagnostics();});connect(tabs_,&QTabWidget::currentChanged,this,&SettingsWindow::viewStateChanged);
 setupTimer_=new QTimer(this);setupTimer_->setInterval(500);connect(setupTimer_,&QTimer::timeout,this,&SettingsWindow::pollSetup);
 for(auto* label:findChildren<QLabel*>())label->setTextFormat(Qt::PlainText);
 refresh();refreshDiagnostics();refreshDestinationList();updateProviderVisibility_();
}
SettingsWindow::~SettingsWindow(){
 for(auto* child:findChildren<QObject*>())disconnect(child,nullptr,this,nullptr);
 if(destinationPage_)for(auto* child:destinationPage_->findChildren<QObject*>())disconnect(child,nullptr,this,nullptr);
 disconnect(&settings_,nullptr,this,nullptr);
}
bool SettingsWindow::eventFilter(QObject* watched,QEvent* event){
 if(watched==destinationPage_&&event->type()==QEvent::Hide)resetCredentialSecretDisplay();
 if(watched==tabs_->tabBar()&&event->type()==QEvent::Wheel){event->ignore();return true;}
 return RetainedWindow::eventFilter(watched,event);
}
void SettingsWindow::refreshMetadata(){if(configPath_)configPath_->setText(settings_.path());metadata_->setText("Configuration: "+settings_.path()+"\nAccepted revision: "+settings_.revision()+"\nObserved file revision: "+settings_.metadata()["observedRevision"].toString());}
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
 if(destinationStatus_)VisualStyle::feedback(destinationStatus_,e.message,bool(e));
 if(e.code=="config_missing"&&settings_.revision().isEmpty()){VisualStyle::feedback(status_,"Changes save automatically when valid.");return;}
 VisualStyle::feedback(status_,e?e.code+": "+e.message+" "+e.details["path"].toString():"Done.",bool(e));
}
void SettingsWindow::refreshDiagnostics(){refreshLoggingStatus();auto data=settings_.diagnostics();quit_->setVisible(data["trayAvailable"].isBool()&&!data["trayAvailable"].toBool());capabilities_->setText(QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Indented)));shortcuts_->setStatus(data["shortcuts"].toObject());}
void SettingsWindow::refresh(bool preserveDrafts){
 if(preserveDrafts)rememberCredentialInputs();else{clearCredentialInputs();profileEditors_={};provisionalProfile_={};}
 autosaveBlocked_=false;for(auto* button:retryAutosave_)button->hide();reorderBaseline_.clear();
 loading_=true;draft_=settings_.document().isEmpty()?Settings::defaults():settings_.document();baseRevision_=settings_.revision();observed_=settings_.metadata()["observedRevision"].toString();dirty_=conflict_=false;refreshMetadata();
 status_->clear();
 auto actions=draft_["actions"].toObject();auto profiles=draft_["profiles"].toArray();initialProfile_=settings_.revision().isEmpty()&&profiles.isEmpty();
 if(initialProfile_){
  const QString id="s3-1";profiles.append(s3Profile(id));draft_["profiles"]=profiles;
  for(const auto& name:Settings::actionNames()){auto action=actions[name].toObject();if(action["destination"]=="upload"&&action["profileId"].toString().isEmpty())action["profileId"]=id;actions[name]=action;}
  draft_["actions"]=actions;
 }
 populate();for(auto* field:errors_.keys())inlineError(field,{});refreshDestinationList();refreshCredentialHint();
}
void SettingsWindow::populate(){
 loading_=true;const auto profiles=draft_["profiles"].toArray();const auto actions=draft_["actions"].toObject();
 auto selected=profile_->currentData().toString();profileIndex_=-1;profile_->clear();for(auto p:profiles)profile_->addItem(p.toObject()["label"].toString(),p.toObject()["id"].toString());int selectedIndex=profile_->findData(selected);if(selectedIndex>=0)profile_->setCurrentIndex(selectedIndex);loadProfile();
 captureDefaultsEnabled_=draft_.contains("captureDefaults");captureRoutingEdited_=false;
 auto base=captureDefaultsEnabled_?draft_["captureDefaults"].toObject():Settings::effectiveAction(draft_,"region");
 for(const auto& name:Settings::actionNames()){
  const auto effective=Settings::effectiveAction(draft_,name);const auto raw=actions[name].toObject();
  bool inherit=captureDefaultsEnabled_?raw["destination"]=="default":effective["destination"]==base["destination"]&&effective["profileId"]==base["profileId"]&&effective["publicationMode"].toString("default")==base["publicationMode"].toString("default");
  useCaptureDefault_[name]->setChecked(inherit);
  publicationModes_[name]->setCurrentIndex(publicationModes_[name]->findData(effective["publicationMode"].toString("default")));auto* c=actionProfiles_[name];c->clear();c->addItem("No destination (uploads off)",QString{});for(auto p:profiles)c->addItem(p.toObject()["label"].toString(),p.toObject()["id"].toString());c->setCurrentIndex(c->findData(effective["destination"]=="local"?QString{}:effective["profileId"].toString()));
 }
 captureProfile_->setCurrentIndex(captureProfile_->findData(base["destination"]=="local"?QString{}:base["profileId"].toString()));captureSharing_->setCurrentIndex(captureSharing_->findData(base["publicationMode"].toString("default")));
 bool exceptions=false;for(auto* inherit:useCaptureDefault_)exceptions|=!inherit->isChecked();captureExceptions_->setChecked(exceptions);
 shortcuts_->setActions(actions);auto archive=draft_["archive"].toObject();root_->setText(archive["root"].toString());keep_->setChecked(archive["keepUploads"].toBool());for(auto it=folders_.begin();it!=folders_.end();++it)it.value()->setText(archive["folders"].toObject()[it.key()].toString());
 naming_->setCurrentIndex(naming_->findData(draft_["naming"].toObject()["mode"].toString()));const auto effective=Settings::fromValidated(draft_);const QHash<QString,QString> formats{{"fileTemplate",effective.fileTemplate},{"captureTemplate",effective.captureTemplate},{"imageTemplate",effective.imageTemplate},{"textTemplate",effective.textTemplate}};for(auto it=templates_.begin();it!=templates_.end();++it)it.value()->setText(formats.value(it.key()));collision_->setCurrentIndex(collision_->findData(draft_["naming"].toObject()["collisionPolicy"].toString()));auto prefs=draft_["preferences"].toObject();uploadClipboard_->setCurrentIndex(uploadClipboard_->findData(effective.uploadClipboardBehavior));notifications_->setChecked(prefs["notifications"].toBool());login_->setChecked(prefs["launchAtLogin"].toBool());debugLogging_->setChecked(prefs["diagnostics"].toObject()["debugLogging"].toBool());unlockAtStartup_->setChecked(effective.unlockCredentialsAtStartup);
 for(auto it=numbers_.begin();it!=numbers_.end();++it){QJsonValue value=draft_["limits"].toObject().contains(it.key())?draft_["limits"].toObject().value(it.key()):draft_["retention"].toObject().value(it.key());if(it.key()=="pendingImageBytes"&&value.isUndefined())value=effective.limits.pendingImageBytes;it.value()->setText(value.isNull()?QString{}:value.isString()?QStringLiteral("Auto"):ByteSize::exact(value.toInteger(),displayUnit(it.key())));}
 updateCaptureInheritance();loading_=false;
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
 const auto finish=[g=QPointer<SettingsWindow>(this),reloadFile](Error error){
  if(!g)return;
  g->resetting_=false;g->setEnabled(true);g->destinationPage_->setEnabled(true);
  if(error){g->configurationChanged();g->status(error);emit g->viewStateChanged();return;}
  g->refresh();emit g->viewStateChanged();
  if(reloadFile)VisualStyle::feedback(g->status_,"Settings reloaded from file.");else g->close();
 };
 if(reloadFile)settings_.reloadAsync([finish](Result result){finish(result.error);});
 else settings_.refreshSnapshot(finish);
}

void SettingsWindow::rebuildProfiles(int selected,const QString& defaultProfile){const auto captureProfile=captureProfile_->currentData().toString();const bool previous=loading_;loading_=true;const auto profiles=draft_["profiles"].toArray();profileIndex_=-1;profile_->clear();for(auto p:profiles)profile_->addItem(p.toObject()["label"].toString(),p.toObject()["id"].toString());profile_->setCurrentIndex(selected);loadProfile();for(auto* combo:actionProfiles_){auto id=combo->currentData().toString();combo->clear();combo->addItem("No destination (uploads off)",QString{});for(auto p:profiles)combo->addItem(p.toObject()["label"].toString(),p.toObject()["id"].toString());int index=combo->findData(id.isEmpty()?defaultProfile:id);combo->setCurrentIndex(index<0?0:index);}captureProfile_->setCurrentIndex(captureProfile_->findData(captureProfile.isEmpty()?defaultProfile:captureProfile));if(!defaultProfile.isEmpty()){captureDefaultsEnabled_=true;captureRoutingEdited_=true;}updateCaptureInheritance();loading_=previous;}
void SettingsWindow::loadProfile(){if(destinationStatus_)VisualStyle::feedback(destinationStatus_,{});bool previous=loading_;loading_=true;profileIndex_=profile_->currentIndex();auto profiles=draft_["profiles"].toArray();auto p=profileIndex_>=0&&profileIndex_<profiles.size()?profiles[profileIndex_].toObject():QJsonObject{};profileId_->setText(p["id"].toString());profileLabel_->setText(p["label"].toString());const auto s=p["settings"].toObject();bool s3=p["provider"]=="s3";
 providerType_->setCurrentIndex(providerType_->findData(p["provider"].toString()));const bool proton=p["provider"]=="proton-drive";protonPanel_->setVisible(proton);protonAccountUid_=s["accountRootUid"].toString();protonAccountEmail_.clear();protonShare_->setChecked(s["shareByDefault"].toBool(true));protonExpiration_->setValue(s["linkExpirationDays"].toInt());protonTimeout_->setValue(s["requestTimeoutMs"].toInt(120000)/1000.0);protonFolder_->clear();addProtonFolder(protonFolder_,s["folderUid"].toString(),s["folderName"].toString());
 findControl<QPushButton*>("storeCredentials")->setVisible(s3);findControl<QPushButton*>("testDestination")->setVisible(s3||proton);access_->setVisible(s3);secret_->setVisible(s3);token_->setVisible(s3);for(auto it=s3_.begin();it!=s3_.end();++it){auto v=s[it.key()];it.value()->setText(v.isDouble()?ByteSize::exact(v.toInteger(),it.key().endsWith("Ms")?1000:1):v.toString());it.value()->setEnabled(s3);}s3_["region"]->setProperty("previousRegion",s["region"].toString());regionChoices_->setEnabled(s3);acl_->setEnabled(s3);addressing_->setEnabled(s3);acl_->setCurrentIndex(acl_->findData(s["aclMode"].toString()));addressing_->setCurrentIndex(addressing_->findData(s["addressing"].toString()));providerJson_->setEnabled(!s3);providerJson_->setPlainText(proton?QString{}:QString::fromUtf8(QJsonDocument(s).toJson(QJsonDocument::Indented)));auto ref=p["credentials"].toObject();credentialMode_->setCurrentIndex(credentialMode_->findData(ref["mode"].toString()));reference_->setText(ref["reference"].toString());if(profileIndex_>=0&&profileIndex_<profileEditors_.size()&&profileEditors_[profileIndex_].toObject().contains("id"))applyProfileEditor(profileEditors_[profileIndex_].toObject());restoreCredentialInputs();if(updateProviderVisibility_)updateProviderVisibility_();loading_=previous;updateProtonAccountDisplay();updateSetupButton();refreshDestinationList();refreshCredentialHint();}
QJsonObject SettingsWindow::editedProfile()const{auto profiles=draft_["profiles"].toArray();if(profileIndex_<0||profileIndex_>=profiles.size())return {};auto p=profiles[profileIndex_].toObject();p["id"]=profileId_->text();p["label"]=profileLabel_->text();auto s=p["settings"].toObject();if(p["provider"]=="s3"){for(auto it=s3_.begin();it!=s3_.end();++it){if(it.key().endsWith("Ms")||it.key().startsWith("lowSpeed")){auto value=ByteSize::fromDecimal(it.value()->text(),it.key().endsWith("Ms")?1000:1);s[it.key()]=value;}else if(it.key()=="publicUrlBase"&&it.value()->text().isEmpty())s[it.key()]=QJsonValue::Null;else s[it.key()]=it.value()->text();}s["aclMode"]=acl_->currentData().toString();s["addressing"]=addressing_->currentData().toString();}else if(p["provider"]=="proton-drive"){s["accountRootUid"]=protonAccountUid_;s["folderUid"]=protonFolder_->currentData().toString();const auto folderName=protonFolder_->currentData(protonFolderNameRole).toString();if(folderName.isEmpty())s.remove("folderName");else s["folderName"]=folderName;s["shareByDefault"]=protonShare_->isChecked();s["linkExpirationDays"]=protonExpiration_->hasAcceptableInput()?QJsonValue(protonExpiration_->value()):QJsonValue(protonExpiration_->text());s["requestTimeoutMs"]=protonTimeout_->hasAcceptableInput()?QJsonValue(qRound64(protonTimeout_->value()*1000)):QJsonValue(protonTimeout_->text());}else {auto parsed=QJsonDocument::fromJson(providerJson_->toPlainText().toUtf8());if(parsed.isObject())s=parsed.object();else s={{"invalidJson",true}};}p["settings"]=s;p["credentials"]=QJsonObject{{"mode",credentialMode_->currentData().toString()},{"reference",credentialMode_->currentData().toString()=="session"?QJsonValue(QJsonValue::Null):QJsonValue(reference_->text())}};return p;}
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
 for(const auto& name:Settings::actionNames()){
  auto action=actions[name].toObject();
  if(captureRoutingEdited_){
   if(publicationModes_[name]->currentData()!="default"||action.contains("publicationMode"))action["publicationMode"]=publicationModes_[name]->currentData().toString();
   auto id=actionProfiles_[name]->currentData().toString();if(profileIndex_>=0&&profileIndex_<draft_["profiles"].toArray().size()&&id==draft_["profiles"].toArray()[profileIndex_].toObject()["id"].toString())id=profileId_->text();
   action["destination"]=id.isEmpty()?"local":"upload";action["profileId"]=id.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(id);
   if(captureDefaultsEnabled_&&useCaptureDefault_[name]->isChecked()){action["destination"]="default";action["profileId"]=QJsonValue::Null;if(action.contains("publicationMode"))action["publicationMode"]="default";}
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
 if(from<0||to<0||from>=order.size()||to>=order.size()||from==to)return {"destination_move_invalid","That destination cannot move further in this direction.",{{"from",from},{"to",to}}};
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
void SettingsWindow::refreshLoggingStatus(){
 const auto data=settings_.diagnostics();const auto logging=data["logging"].toObject();
 const auto expires=logging["debugLoggingExpiresAt"].toString();
 debugLoggingStatus_->setText(logging["debugLogging"].toBool()&&!expires.isEmpty()?"On. Turns itself off at "+localTime(expires)+".":debugLogging_->isChecked()?QString("On. Turns itself off 24 hours after it was turned on."):QString("Off. Turn it on to record detailed logs for a maintainer; it turns itself off after 24 hours."));
 if(!logging["available"].toBool(true)||logging.isEmpty())logSummary_->setText(logging.isEmpty()?QString("Log status is not available yet."):"The log folder is not available: "+logging["directory"].toString());
 else{
  const int files=logging["files"].toInt();const auto oldest=logging["oldestRecord"].toString();const auto retention=logging["retention"].toObject();
  logSummary_->setText((files==1?QString("1 file"):QString("%1 files").arg(files))+" · "+ByteSize::summary(logging["bytes"].toInteger())+(oldest.isEmpty()?QString{}:" · oldest record "+localTime(oldest))+
   (retention.isEmpty()?QString{}:QString(" · kept %1 days, up to %2").arg(retention["days"].toInt()).arg(ByteSize::summary(retention["bytes"].toInteger()))));
 }
 logSummary_->setToolTip(logging["directory"].toString());
 const bool degraded=data["loggingDegraded"].toBool()||logging["degraded"].toBool();
 static const QHash<QString,QString> reasons{{"unavailable","the log folder can't be used"},{"write_failed","writing to it failed"},{"disk_full","the disk is full"},{"queue_full","records arrived faster than they could be written"}};
 const auto dropped=logging["droppedRecords"].toInteger();
 loggingWarning_->setText(degraded?"Logging is degraded: "+reasons.value(logging["degradedReason"].toString(),logging["degradedReason"].toString("unknown reason"))+(dropped?QString(". %1 records were dropped").arg(dropped):QString{})+". Uploads and captures are not affected.":QString{});
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
   if(error)g->status(error);else VisualStyle::feedback(g->status_,QString("Logs cleared: %1 files, %2 removed.").arg(result["removedFiles"].toInt()).arg(ByteSize::summary(result["removedBytes"].toInteger())));
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
void SettingsWindow::diagnosticsCommand(const QJsonObject& params,std::function<void(Error,QJsonObject)> reply){
 const auto action=params["action"].toString();
 auto* dialog=static_cast<DiagnosticsPackageDialog*>(packageDialog_.data());const bool open=dialog&&dialog->isVisible();
 auto withState=[g=QPointer<SettingsWindow>(this),reply](Error error,QJsonObject result){if(!g){reply(error,result);return;}auto state=g->diagnosticsState();if(!result.isEmpty())state["result"]=result;reply(error,state);};
 if(action=="inspect"){reply({},diagnosticsState());return;}
 if(action=="setDebugLogging"){
  if(!params["enabled"].isBool()){reply({"invalid_request","setDebugLogging takes enabled, true or false.",{}},{});return;}
  // The checkbox autosaves like any setting; inspect shows when the resident accepted it.
  tabs_->setCurrentIndex(0);debugLogging_->setChecked(params["enabled"].toBool());flushAutosave();refreshLoggingStatus();reply({},diagnosticsState());return;
 }
 if(action=="clearLogs"){tabs_->setCurrentIndex(0);clearLogs(withState);return;}
 if(action=="openPackage"){tabs_->setCurrentIndex(0);openDiagnosticsPackage(params["operation"].toString());dialog=static_cast<DiagnosticsPackageDialog*>(packageDialog_.data());if(params.contains("includeNames"))dialog->setIncludeNames(params["includeNames"].toBool());dialog->whenPreviewed([withState](Error error,QJsonObject){withState(error,{});});return;}
 if(!open){reply({"diagnostics_dialog_closed","Open Create diagnostics package first (action openPackage).",{}},{});return;}
 if(action=="setPackage"){
  if(params.contains("folder"))if(auto error=dialog->setFolder(params["folder"].toString(),true)){reply(error,{});return;}
  if(params.contains("includeNames"))dialog->setIncludeNames(params["includeNames"].toBool());
  dialog->whenPreviewed([withState](Error error,QJsonObject){withState(error,{});});return;
 }
 if(action=="savePackage"){dialog->save([withState](Error error,QJsonObject){withState(error,{});});return;}
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
void SettingsWindow::inlineError(QWidget* control,const QString& message){if(!control)return;control->setProperty("invalid",!message.isEmpty());control->style()->unpolish(control);control->style()->polish(control);if(auto* label=errors_.value(control)){VisualStyle::role(label,"error");label->setText(message);label->setVisible(!message.isEmpty());}control->setAccessibleDescription(message.isEmpty()?control->toolTip():control->toolTip()+"\n"+message);}
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
  const auto text=upload?(local?QString("Uploads to the selected destination and saves a local copy."):QString("Uploads to the selected destination. Local copies are off in General.")):(local?QString("Uploads are off. Saves a local copy using your folders in General."):QString("This action will fail: no destination and local copies are off. Choose a destination or enable Keep a local copy in General."));
  VisualStyle::feedback(label,text,!upload&&!local);
 };
 const bool upload=!captureProfile_->currentData().toString().isEmpty();captureSharing_->setEnabled(upload);describe(captureOutputNotice_,upload);
 for(const auto& name:Settings::actionNames()){
  auto *profile=actionProfiles_[name],*sharing=publicationModes_[name];
  // An inherited card stays visible: its disabled controls show the default output.
  const bool inherit=useCaptureDefault_[name]->isChecked();
  if(inherit){const QSignalBlocker a(profile),b(sharing);profile->setCurrentIndex(profile->findData(captureProfile_->currentData()));sharing->setCurrentIndex(sharing->findData(captureSharing_->currentData()));}
  const bool actionUpload=!profile->currentData().toString().isEmpty();profile->setEnabled(!inherit);sharing->setEnabled(!inherit&&actionUpload);
  const auto resolved=inherit&&actionUpload?destinationDefaultSharing(profile->currentData().toString()):QString{};
  sharing->setItemText(sharing->findData("default"),resolved.isEmpty()?sharingLabel("default"):sharingLabel(resolved)+" (destination default)");
  const auto inherited=[inherit](QComboBox* combo){return inherit?": inherited from default, "+combo->currentText():QString{};};
  profile->setAccessibleName(actionLabel(name)+" destination"+inherited(profile));sharing->setAccessibleName(actionLabel(name)+" sharing"+inherited(sharing));
  describe(captureStatus_[name],actionUpload);
 }
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
 for(const auto& name:Settings::actionNames()){auto action=route(actionProfiles_[name],publicationModes_[name],captureStatus_[name]);action["inherited"]=useCaptureDefault_[name]->isChecked();routing[name]=action;}
 return routing;
}
void SettingsWindow::queueCaptureEdit(QWidget* control){
 if(loading_||resetting_||!autosave_)return;
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
 auto routes=projected["actions"].toObject();for(const auto& name:Settings::actionNames()){auto route=routes[name].toObject();const auto proposal=proposed["actions"].toObject()[name].toObject();for(const auto& key:QStringList{"destination","profileId","publicationMode"}){if(proposal.contains(key))route[key]=proposal[key];else route.remove(key);}routes[name]=route;}projected["actions"]=routes;
 for(auto it=pendingEdits_.begin();it!=pendingEdits_.end();){if((it.key().startsWith("/actions")&&!it.key().endsWith("/shortcut"))||it.key().startsWith("/captureDefaults")){editControls_.remove(it.key());pendingEditSerials_.remove(it.key());it=pendingEdits_.erase(it);}else ++it;}
 stageEdit("/",projected,control);inlineError(control,{});dirty();autosave_->start();
}
QString SettingsWindow::autosaveText()const{
 if(conflict_)return "Settings changed elsewhere — reload to continue.";
 if(autosaveBlocked_)return "Couldn’t save changes — retry saving.";
 if(!access_->text().isEmpty()||(!savedSecretShown_&&!secret_->text().isEmpty())||!token_->text().isEmpty()||!protonPassword_->text().isEmpty())return "Use Store to save credentials";
 return {};
}
void SettingsWindow::showCaptureSettings(){tabs_->setCurrentIndex(1);}
bool SettingsWindow::flushAutosave(){if(autosave_)autosave_->stop();if(!autosaveBlocked_&&!pendingEdits_.isEmpty()&&!conflict_&&(setupAccepting_||!setupJobs_.isEmpty())){const auto waiting=QStringLiteral("Waiting for destination setup to finish before saving your edits.");if(status_->text()!=waiting||status_->property("feedbackTone").toString()!="neutral")VisualStyle::feedback(status_,waiting);return false;}if(!saving_&&!pendingEdits_.isEmpty()&&!conflict_)save();return !saving_;}
void SettingsWindow::save(){
 if(autosaveBlocked_||conflict_||saving_||resetting_||setupAccepting_||!setupJobs_.isEmpty())return;
 if(pendingEdits_.isEmpty())return;
 storeProfile();auto doc=settings_.document().isEmpty()?Settings::defaults():settings_.document();for(auto it=pendingEdits_.begin();it!=pendingEdits_.end();++it)doc=replacePath(doc,it.key().split('/',Qt::SkipEmptyParts),0,it.value()).toObject();
 if(doc==settings_.document()){pendingEdits_.clear();pendingEditSerials_.clear();editControls_.clear();dirty_=candidate()!=settings_.document();emit viewStateChanged();return;}
 saving_=true;savingDocument_=doc;submittedEditSerials_=pendingEditSerials_;emit viewStateChanged();
 settings_.saveAsync(doc,observed_,[g=QPointer<SettingsWindow>(this)](Result result){
  if(!g)return;
  if(!result){if(result.error.code!="config_invalid"&&result.error.code!="s3_settings_invalid"){g->autosaveFailure(result.error);return;}g->persistNextEdit();return;}
  g->saving_=false;g->autosaveBlocked_=false;for(auto* button:g->retryAutosave_)button->hide();g->baseRevision_=g->settings_.revision();g->observed_=g->settings_.metadata()["observedRevision"].toString();if(!g->settings_.document()["profiles"].toArray().isEmpty())g->initialProfile_=false;g->draft_=g->candidate();
  for(auto it=g->submittedEditSerials_.cbegin();it!=g->submittedEditSerials_.cend();++it)if(g->pendingEdits_.contains(it.key())&&g->pendingEditSerials_.value(it.key())==it.value()){g->pendingEdits_.remove(it.key());g->pendingEditSerials_.remove(it.key());g->inlineError(g->editControls_.take(it.key()),{});}
  g->submittedEditSerials_.clear();g->dirty_=g->candidate()!=g->settings_.document();if(!g->pendingEdits_.contains("/profiles"))g->reorderBaseline_.clear();
  if(!g->pendingEdits_.isEmpty())g->autosave_->start();
  else if(!g->dirty_){g->status_->clear();g->destinationStatus_->clear();}
  g->refreshMetadata();g->refreshDestinationList();g->refreshCredentialHint();emit g->viewStateChanged();
 });
}
void SettingsWindow::autosaveFailure(const Error& error){
 saving_=false;submittedEditSerials_.clear();autosaveBlocked_=true;autosave_->stop();if(error.code=="config_conflict")conflict_=true;
 // A conflicting reorder is not kept: the list goes back to the saved order.
 const bool reordered=conflict_&&!reorderBaseline_.isEmpty();if(reordered)restoreReorderBaseline();
 auto message=error;message.message+=" Your edits remain in this draft. "+QString(reordered?"The previous destination order is restored. ":"")+QString(conflict_?"Reload from file to resolve the conflict.":"Use Retry saving when ready; the request will not repeat automatically.");status(message);
 for(auto* button:retryAutosave_)button->setVisible(!conflict_);
 emit viewStateChanged();
}
void SettingsWindow::persistNextEdit(){
 if(pendingEdits_.isEmpty()){saving_=false;submittedEditSerials_.clear();dirty_=candidate()!=settings_.document();emit viewStateChanged();return;}
 const auto path=pendingEdits_.firstKey();const auto value=pendingEdits_.take(path);const auto serial=pendingEditSerials_.take(path);const QPointer<QWidget> control=editControls_.take(path);const auto accepted=settings_.document().isEmpty()?Settings::defaults():settings_.document();const auto doc=replacePath(accepted,path.split('/',Qt::SkipEmptyParts),0,value).toObject();
 if(doc==accepted){inlineError(control,{});persistNextEdit();return;}
 savingDocument_=doc;submittedEditSerials_={{path,serial}};
 settings_.saveAsync(doc,observed_,[g=QPointer<SettingsWindow>(this),control,path,value](Result result){
  if(!g)return;
  if(!result){if(result.error.code!="config_invalid"&&result.error.code!="s3_settings_invalid"){bool superseded=false;for(auto it=g->pendingEdits_.begin();it!=g->pendingEdits_.end();++it)if(it.key()=="/"||path==it.key()||path.startsWith(it.key()+"/"))superseded=true;if(!superseded){g->stageEdit(path,value,control);}g->autosaveFailure(result.error);return;}g->inlineError(control,result.error.message);g->status(result.error);g->persistNextEdit();return;}
  g->baseRevision_=g->settings_.revision();g->observed_=g->settings_.metadata()["observedRevision"].toString();g->draft_=g->candidate();g->inlineError(control,{});if(path=="/profiles"&&!g->pendingEdits_.contains("/profiles"))g->reorderBaseline_.clear();g->refreshMetadata();g->persistNextEdit();
 });
}
void SettingsWindow::refreshCredentialHint(){
 if(!credentialHint_)return;
 credentialHint_->clear();access_->setPlaceholderText({});secret_->setPlaceholderText({});token_->setPlaceholderText({});
 if(providerType_->currentData()!="s3"||initialProfile_||profileIndex_<0)return;
 const auto id=profileId_->text();const auto revision=settings_.revision();
 settings_.request("setup.credentialHint",{{"profileId",id},{"expectedRevision",revision}},this,[g=QPointer<SettingsWindow>(this),id,revision](Error error,QJsonObject hint){
  if(!g||g->profileId_->text()!=id||g->settings_.revision()!=revision||g->providerType_->currentData()!="s3")return;
  if(error)return;
  if(hint["available"].toBool()){
   const auto location=hint["mode"]=="keychain"?QString("Saved in keychain"):QString("Available for this session");g->credentialHint_->setText(hint["accessKeyHint"].toString()+" · "+location+" · Secret key stored");g->access_->setPlaceholderText(hint["accessKeyHint"].toString()+" · "+location);g->secret_->setPlaceholderText(QString::fromUtf8("•••••••• · Stored"));if(hint["tokenStored"].toBool())g->token_->setPlaceholderText(QString::fromUtf8("•••••••• · Stored"));
  }else g->credentialHint_->setText(hint["mode"]=="keychain"?"Keychain selected · Unlock saved credentials to identify the stored key":"No credentials loaded for this session");
 });
}
void SettingsWindow::refreshDestinationList(const QJsonObject& document){
 if(!destinationList_)return;
 const QSignalBlocker block(destinationList_);const auto doc=document.isEmpty()?candidate():document;const auto profiles=doc["profiles"].toArray();
 // Rows are updated in place: rebuilding them here, which runs when a press changes the
 // selection, would drop the view's pressed row, so the drag that follows never starts.
 while(destinationList_->count()>profiles.size())delete destinationList_->takeItem(destinationList_->count()-1);int row=0;const auto actions=doc["actions"].toObject();bool selectedDefault=false;
 for(const auto& value:profiles){const auto profile=value.toObject();bool isDefault=true;if(doc.contains("captureDefaults")){const auto capture=doc["captureDefaults"].toObject();isDefault=capture["destination"]=="upload"&&capture["profileId"]==profile["id"];}else for(const auto& action:Settings::actionNames())if(actions[action].toObject()["profileId"]!=profile["id"]||actions[action].toObject()["destination"]!="upload")isDefault=false;const auto s=profile["settings"].toObject();const bool proton=profile["provider"]=="proton-drive";QString state=proton?(s["accountRootUid"].toString().isEmpty()?"Signed out":s["folderUid"].toString().isEmpty()?"Choose a folder":"Connected"):(s["bucket"].toString().isEmpty()?"Needs a bucket":testedDestinations_.value(profile["id"].toString())==profile?"Connection tested":"Ready to test");if(isDefault)state="Default · "+state;auto* item=row<destinationList_->count()?destinationList_->item(row):new QListWidgetItem(destinationList_);++row;item->setIcon(VisualStyle::icon(proton?"files":"destinations"));item->setText(profile["label"].toString()+"\n"+state);item->setSizeHint(QSize(0,76));item->setToolTip(profile["label"].toString()+"\n"+state);if(profile["id"]==profileId_->text()){selectedDefault=isDefault;defaultButton_->setText(isDefault?"Default":"Make default");}}
 destinationList_->setCurrentRow(profileIndex_);const bool has=profileIndex_>=0;if(moveUp_){moveUp_->setEnabled(profileIndex_>0);moveDown_->setEnabled(has&&profileIndex_<int(profiles.size())-1);}defaultButton_->setEnabled(has&&!selectedDefault&&!(providerType_->currentData()=="s3"&&s3_["bucket"]->text().isEmpty()));
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
 QSignalBlocker stateSignals(this);clearCredentialInputs();loading_=true;draft_=state["draft"].toObject();profileEditors_=state["profileEditors"].toArray();provisionalProfile_=state["provisionalProfile"].toObject();baseRevision_=state["baseRevision"].toString();observed_=state["observedRevision"].toString();initialProfile_=state["initialProfile"].toBool();
 populate();loading_=true;profile_->setCurrentIndex(state["profileIndex"].toInt(-1));loadProfile();const auto numbers=state["numbers"].toObject();for(auto it=numbers_.begin();it!=numbers_.end();++it)it.value()->setText(numbers[it.key()].toString());
 findControl<QToolButton*>("advancedSettings")->setChecked(state["advanced"].toBool());tabs_->setCurrentIndex(state["section"].toInt());const auto box=state["geometry"].toArray();setGeometry(box[0].toInt(),box[1].toInt(),box[2].toInt(),box[3].toInt());
 dirty_=state["dirty"].toBool();autosaveBlocked_=state["autosaveBlocked"].toBool();for(auto* button:retryAutosave_)button->setVisible(autosaveBlocked_&&!state["conflict"].toBool());conflict_=state["conflict"].toBool();loading_=false;setupJobs_=state["setupJobs"].toArray();updateSetupButton();if(!setupJobs_.isEmpty()){setupTimer_->start();pollSetup();}
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
 if(showSecret_){const QSignalBlocker block(showSecret_);showSecret_->setChecked(false);showSecret_->setText("Show");showSecret_->setAccessibleName("Show secret access key");}
}
void SettingsWindow::toggleCredentialSecret(bool shown){
 if(!shown){resetCredentialSecretDisplay();return;}
 showSecret_->setText("Hide");showSecret_->setAccessibleName("Hide secret access key");
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
  const bool shown=conflict_&&!status_->text().isEmpty();conflict_=true;if(!shown)VisualStyle::feedback(status_,"config_conflict: The configuration changed elsewhere. Your draft is preserved. Reload from file before saving.",true);emit viewStateChanged();return;}
 if(!dirty_&&(baseRevision_!=settings_.revision()||observed_!=settings_.metadata()["observedRevision"].toString()))refresh();
 auto error=settings_.metadata()["validation"].toObject()["error"].toObject();if(!error["code"].toString().isEmpty())status({error["code"].toString(),error["message"].toString(),error["details"].toObject()});
}
void SettingsWindow::updateProtonAccountDisplay(){
 const bool connected=!protonAccountUid_.isEmpty();
 protonAccount_->setText(connected?(protonAccountEmail_.isEmpty()?QString("Connected account"):protonAccountEmail_+" · Connected"):QString("Not connected"));
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
  if(success)VisualStyle::success(label,message);else VisualStyle::feedback(label,message);
 }
}
void SettingsWindow::beginSetup(const QString& kind,const QJsonObject& input){
 if(selectedDestinationChanged()||saving_||conflict_||initialProfile_||profile_->currentIndex()<0){status({"save_first","Finish valid destination edits before connecting. Changes save automatically.",{}});return;}
 const auto profileId=profileId_->text(),revision=settings_.revision();QJsonObject params{{"profileId",profileId},{"expectedRevision",revision}};if(kind=="storeCredentials")params["credentials"]=input;else if(kind=="createFolder"||kind=="storeSharePassword")params["input"]=input;
 const auto testMessage=providerType_->currentData()=="proton-drive"?QString("Testing connection: checking the Proton account and selected folder…"):QString("Testing connection: checking access, upload, public link and cleanup…");
 if(kind=="testDestination"){testedDestinations_.remove(profileId);testAccepting_=true;setupFeedback(testMessage);}
 updateSetupButton();
 if(kind=="connectAccount"||kind=="inspectAccount")folderRequests_.insert(profileId);
 ++setupAccepting_;updateSetupButton();emit viewStateChanged();
 settings_.request("setup."+kind,params,this,[this,kind,profileId,revision,testMessage](Error e,QJsonObject data){
  folderRequests_.remove(profileId);--setupAccepting_;if(kind=="testDestination")testAccepting_=false;
  if(e){updateSetupButton();status(e);if(kind=="connectAccount"&&profileId==profileId_->text())protonAccount_->setText("Connection failed: "+e.message);emit viewStateChanged();return;}
  for(int i=setupJobs_.size()-1;i>=0;--i){const auto old=setupJobs_[i].toObject();if(old["kind"]==kind&&old["profileId"]==profileId)setupJobs_.removeAt(i);}
  setupJobs_.append(QJsonObject{{"setupId",data["setupId"]},{"kind",kind},{"profileId",profileId},{"revision",revision}});
  updateSetupButton();setupFeedback(kind=="testDestination"?testMessage:QString("Setup accepted; waiting for the resident result."));
  if(kind=="connectAccount"&&profileId==profileId_->text())protonAccount_->setText("Connecting to Proton…");
  emit viewStateChanged();setupTimer_->start();pollSetup();
 });
}
void SettingsWindow::updateSetupButton(){
 const auto id=profileId_->text();bool foldersLoading=folderRequests_.contains(id);
 for(const auto& value:setupJobs_){const auto job=value.toObject();if(job["profileId"]==id&&(job["kind"]=="connectAccount"||job["kind"]=="inspectAccount"))foldersLoading=true;}
 protonFolderStatus_->setText(foldersLoading?QString("Loading folders…"):QString{});protonFolderStatus_->setVisible(foldersLoading&&providerType_->currentData()=="proton-drive");
 const bool busy=setupAccepting_||!setupJobs_.isEmpty();
 findControl<QPushButton*>("inspectProtonAccount")->setEnabled(!busy&&!protonAccountUid_.isEmpty());
 if(!busy&&autosave_&&!pendingEdits_.isEmpty()&&!conflict_&&!resetting_)autosave_->start();
 findControl<QWidget*>("destinationEditor")->setEnabled(!busy);findControl<QWidget*>("credentialsEditor")->setEnabled(!busy);protonPanel_->setEnabled(!busy);profile_->setEnabled(!busy);findControl<QPushButton*>("addS3Destination")->setEnabled(!busy);findControl<QPushButton*>("removeDestination")->setEnabled(!busy);findControl<QPushButton*>("importDevelopmentS3")->setEnabled(!busy);
 const bool pending=std::any_of(setupJobs_.cbegin(),setupJobs_.cend(),[](const QJsonValue& value){return value.toObject()["kind"]=="testDestination";});
 findControl<QPushButton*>("testDestination")->setEnabled(!testAccepting_&&!pending);
}
void SettingsWindow::pollSetup(){
 if(setupPollActive_||setupJobs_.isEmpty()){if(setupJobs_.isEmpty())setupTimer_->stop();return;}
 setupPollActive_=true;const auto id=setupJobs_.first().toObject()["setupId"].toString();
 settings_.request("setup.inspect",{{"id",id}},this,[this,id](Error e,QJsonObject data){
  setupPollActive_=false;
  int index=0;while(index<setupJobs_.size()&&setupJobs_[index].toObject()["setupId"]!=id)++index;
  if(index==setupJobs_.size())return;
  const auto job=setupJobs_[index].toObject();const auto event=data["event"].toObject();
  if(!e&&event["event"]=="auth"&&!protonBrowserOpened_){const QUrl url(event["url"].toString());if(url.scheme()=="https"&&url.host()=="account.proton.me"){protonBrowserOpened_=true;const bool opened=QDesktopServices::openUrl(url);protonAccount_->setText(opened?"Finish signing in to Proton in your browser.":"Could not open the default browser for Proton sign-in.");}}
  if(e.code=="setup_not_found"||(!e&&data["state"]!="running")){
   setupJobs_.removeAt(index);updateSetupButton();if(setupJobs_.isEmpty())setupTimer_->stop();
   if(e)e.message="This setup result is no longer available; the action was not repeated.";
   else{const auto error=data["error"].toObject();e={error["code"].toString(),error["message"].toString(),error["details"].toObject()};}
   if(e){if(job["kind"]=="testDestination"&&(job["profileId"]!=profileId_->text()||(!job["revision"].toString().isEmpty()&&job["revision"]!=settings_.revision())))e.message="Connection test failed for the original destination settings. "+e.message;status(e);if(data["kind"]=="connectAccount"&&data["profileId"]==profileId_->text())protonAccount_->setText("Connection failed: "+e.message);}else {
    const bool currentTest=data["profileId"]==profileId_->text()&&(job["revision"].toString().isEmpty()||job["revision"]==settings_.revision())&&!selectedDestinationChanged();
    if(data["kind"]=="testDestination"&&currentTest)testedDestinations_.insert(data["profileId"].toString(),editedProfile());
    if(data["kind"]=="storeCredentials"||data["kind"]=="unlockCredentials")refreshCredentialHint();
    const auto result=data["result"].toObject();const auto kind=data["kind"].toString();
    if(kind=="connectAccount"||kind=="inspectAccount"){
     const auto revision=job["revision"].toString();
     if(conflict_||(!revision.isEmpty()&&revision!=settings_.revision())||!applyProtonAccountResult(data["profileId"].toString(),result)){VisualStyle::feedback(status_,"Account details changed while loading. Refresh the original destination to continue.",true);emit viewStateChanged();return;}
     requestedFolders_.insert(data["profileId"].toString()+"/"+result["accountRootUid"].toString());
     VisualStyle::success(status_,"Account and folders loaded.");emit viewStateChanged();
     if(data["profileId"]!=profileId_->text()&&destinationPage_->isVisible())activateDestinations();
     return;
    }
    if(QStringList{"createFolder","storeSharePassword"}.contains(kind)&&(data["profileId"]!=profileId_->text()||providerType_->currentData()!="proton-drive"||conflict_)){VisualStyle::feedback(status_,"Setup completed for the original destination. Refresh that destination before applying its result.",true);emit viewStateChanged();return;}

    if(kind=="createFolder"){protonAccountUid_=result["accountRootUid"].toString();addProtonFolder(protonFolder_,result["uid"].toString(),result["name"].toString());protonFolder_->setCurrentIndex(protonFolder_->count()-1);dirty();queueEdit(protonFolder_);}
    else if(kind=="storeSharePassword"){auto profiles=draft_["profiles"].toArray();if(profileIndex_>=0&&profileIndex_<profiles.size()){auto p=profiles[profileIndex_].toObject();auto s=p["settings"].toObject();s["sharePasswordReference"]=result["reference"];p["settings"]=s;profiles[profileIndex_]=p;draft_["profiles"]=profiles;dirty();queueEdit(protonFolder_);}}
    if(kind=="testDestination")setupFeedback(currentTest?(result["testScope"]=="account-folder"?"Connection successful. Account and selected folder are accessible. Upload and sharing were not tested.":"Connection successful. Test upload, public link and cleanup passed."):"Connection test succeeded for the original destination settings. Test again to check the current settings.",true);
    else setupFeedback(QStringList{"connectAccount","inspectAccount","createFolder","storeSharePassword"}.contains(kind)&&dirty_?"Account setup completed. Changes save automatically.":"Setup completed.",true);
   }
   emit viewStateChanged();
  }else{
   if(e)status(e);
   // Retry only the read-only inspection. Keep other accepted jobs moving too.
   if(setupJobs_.size()>1){const QJsonValue pending=setupJobs_[index];setupJobs_.removeAt(index);setupJobs_.append(pending);}
  }
 });
}
}
