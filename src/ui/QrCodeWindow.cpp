#include "QrCodeWindow.h"
#include "FileDialog.h"
#include "Ui.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>
#include <cmath>
namespace putgd {
using Ui::Space;using Ui::space;
namespace {
// The code area: white whatever the theme, the code centred at whole device
// pixels per module, so every module edge is sharp and the code scans.
class QrCodeView final:public QWidget {
public:
 QrCodeView(const qr::Matrix& matrix,QWidget* parent):QWidget(parent),matrix_(matrix) {
  setMinimumSize(qr::windowArea,qr::windowArea);setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
  setAccessibleName("QR code of the link");setAttribute(Qt::WA_OpaquePaintEvent);
 }
 QSize sizeHint() const override {return {qr::windowArea,qr::windowArea};}
 // Device pixels a side the code may use, and pixels per module in them.
 int area() const {const auto dpr=devicePixelRatioF();return int(std::floor(qMin(width(),height())*dpr));}
 int modulePixels() const {return qr::modulePixels(matrix_,area());}
protected:
 void paintEvent(QPaintEvent*) override {
  QPainter p(this);p.fillRect(rect(),Qt::white);if(matrix_.isNull())return;
  const auto dpr=devicePixelRatioF();const int scale=modulePixels(),edge=matrix_.edge()*scale;
  p.scale(1/dpr,1/dpr);const QPoint origin((int(std::lround(width()*dpr))-edge)/2,(int(std::lround(height()*dpr))-edge)/2);
  p.fillRect(QRect(origin,QSize(edge,edge)),Qt::white);
  for(int y=0;y<matrix_.size;++y)for(int x=0;x<matrix_.size;++x)if(matrix_.dark(x,y))
   p.fillRect(QRect(origin.x()+(x+qr::quietZone)*scale,origin.y()+(y+qr::quietZone)*scale,scale,scale),Qt::black);
 }
private:
 const qr::Matrix& matrix_;
};
}
QrCodeWindow::QrCodeWindow(const QString& historyId,const QString& name,const QString& url,Actions actions,QWidget* parent):QDialog(parent),historyId_(historyId),name_(name),actions_(std::move(actions)) {
 setObjectName("historyQrCode");setAttribute(Qt::WA_DeleteOnClose);setWindowTitle("QR code — "+name);Ui::styleWindow(*this);
 const auto error=qr::encode(url.toUtf8(),matrix_);
 auto* box=new QVBoxLayout(this);box->setContentsMargins(space(Space::l),space(Space::l),space(Space::l),space(Space::l));box->setSpacing(space(Space::m));
 auto* title=Ui::label(name,"sectionTitle",this);title->setObjectName("historyQrTitle");box->addWidget(title);
 code_=new QrCodeView(matrix_,this);code_->setObjectName("historyQrImage");box->addWidget(code_,1);
 // The link the code holds, in full, so it can be checked before scanning.
 auto* link=Ui::label(url,"meta",this);link->setObjectName("historyQrLink");link->setWordWrap(true);link->setTextInteractionFlags(Qt::TextSelectableByMouse);link->setMaximumWidth(qr::windowArea);box->addWidget(link);
 status_=Ui::label({},"meta",this);status_->setObjectName("historyQrStatus");status_->setWordWrap(true);status_->hide();box->addWidget(status_);
 if(error)Ui::feedback(status_,error.message,true);
 auto* buttons=new QHBoxLayout;buttons->setSpacing(space(Space::s));
 copy_=new QPushButton(Ui::icon("copy"),"Copy image",this);copy_->setObjectName("historyQrCopy");copy_->setToolTip("Copy the QR code as a 1,024 px PNG");
 save_=new QPushButton(Ui::icon("download"),"Save…",this);save_->setObjectName("historyQrSave");save_->setToolTip("Save the QR code as a 1,024 px PNG");save_->setAccessibleName("Save QR code…");
 auto* close=new QPushButton("Close",this);close->setObjectName("historyQrClose");close->setDefault(true);
 for(auto* button:{copy_,save_}){button->setEnabled(!error);buttons->addWidget(button);}
 buttons->addStretch();buttons->addWidget(close);box->addLayout(buttons);
 chooser_=new FileDialog(this);
 connect(copy_,&QPushButton::clicked,this,&QrCodeWindow::copyImage);connect(save_,&QPushButton::clicked,this,&QrCodeWindow::saveImage);connect(close,&QPushButton::clicked,this,&QDialog::accept);
}
int QrCodeWindow::modulePixels() const {return static_cast<const QrCodeView*>(code_)->modulePixels();}
QJsonObject QrCodeWindow::state() const {
 const int scale=modulePixels();
 return {{"shown",isVisible()},{"historyId",historyId_},{"modules",matrix_.size},{"modulePixels",scale},{"codePixels",matrix_.edge()*scale},{"exportPixels",matrix_.edge()*qr::exportScale(matrix_)},{"last",last_}};
}
void QrCodeWindow::copyImage() {
 if(!actions_.copy)return;copy_->setEnabled(false);
 actions_.copy([g=QPointer<QrCodeWindow>(this)](Error e,QJsonObject result){
  if(!g)return;g->copy_->setEnabled(true);g->last_={{"action","copy"},{"copyStatus",result["copyStatus"].toString(e?"failed":"copied")}};if(e)g->last_["error"]=e.code;
  const auto status=g->last_["copyStatus"].toString();
  Ui::feedback(g->status_,e?e.message:status=="suppressed"?"Not copied: the clipboard changed.":"QR code copied.",bool(e)||status!="copied");
 });
}
void QrCodeWindow::saveImage() {
 if(!actions_.save||chooser_->isOpen())return;
 auto file=name_;const auto dot=file.lastIndexOf('.');if(dot>0)file.truncate(dot);if(file.isEmpty())file="link";
 chooser_->open({FileDialog::Mode::Save,"Save QR code",{},file+" QR code.png",{{"PNG image",{"*.png"}}}},[g=QPointer<QrCodeWindow>(this)](const FileDialog::Result& chosen){
  if(!g)return;
  if(chosen.failed()){Ui::feedback(g->status_,chosen.error,true);return;}
  if(!chosen.chosen())return;
  auto path=chosen.path;const bool confirmed=path.endsWith(".png",Qt::CaseInsensitive);if(!confirmed)path+=".png";
  g->save_->setEnabled(false);
  g->actions_.save(path,confirmed,[g](Error e,QJsonObject result){
   if(!g)return;g->save_->setEnabled(true);g->last_={{"action","save"},{"saved",!e}};if(e)g->last_["error"]=e.code;else g->last_["path"]=result["path"];
   Ui::feedback(g->status_,e?e.message:"QR code saved.",bool(e));
  });
 });
}
}
