#include "SensitivePrompt.h"
#include "Ui.h"
#include "Theme.h"
#include <QCursor>
#include <algorithm>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QWindow>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

namespace putgd {
using Ui::Space;using Ui::space;
QJsonObject SensitiveFindings::json() const {
 QJsonObject counts;for(const auto& [category,count]:found)if(count>0)counts[category]=count;
 QJsonObject out{{"found",counts},{"complete",complete}};if(!complete)out["reason"]=reason;return out;
}
QString sensitiveReasonText(const QString& reason){
 if(reason=="truncated")return QStringLiteral("Recognition stopped at its time or size limit, so part of the image was not read.");
 if(reason=="too_large")return QStringLiteral("The image is over 24 megapixels, too large to read.");
 if(reason=="patterns_skipped")return QStringLiteral("A pattern in your redaction list didn't finish in time.");
 if(reason=="name_unchecked")return QStringLiteral("Your account name could not be looked up in time.");
 if(reason=="language_missing")return QStringLiteral("A language chosen in Settings has no data installed, so text in it wasn't read.");
 if(reason=="matching_unfinished")return QStringLiteral("Matching didn't finish in time.");
 if(reason=="unavailable")return QStringLiteral("Text recognition is not available.");
 return QStringLiteral("Text recognition failed.");
}
SensitivePrompt::SensitivePrompt(Model model,QWidget* parent)
 // A panel of its own drawing, with no title bar or frame from the desktop. An ordinary window: it opens raised
 // and focused, and another window can cover it; the tray or a notification brings it back (#1804).
 :QWidget(parent,Qt::Window|Qt::FramelessWindowHint),model_(std::move(model)){
 setObjectName("sensitivePrompt");setAttribute(Qt::WA_TranslucentBackground);
 const bool any=model_.findings.any(),complete=model_.findings.complete;
 // Nothing found (a check that couldn't finish) leaves nothing to cover in one step.
 model_.redactAllowed=model_.redactAllowed&&any;
 setWindowTitle(complete?QStringLiteral("Sensitive text found — put.gd"):QStringLiteral("Check before uploading — put.gd"));
 Ui::styleWindow(*this);
 auto* box=new QVBoxLayout(this);box->setContentsMargins(space(Space::xl),space(Space::xl),space(Space::xl),space(Space::l));box->setSpacing(space(Space::m));
 // #1898: the panel never outgrows 90% of the screen with the pointer: the preview, the minimum width and the
 // buttons' rows all fit inside it, at any text size.
 const auto* screen=QGuiApplication::screenAt(QCursor::pos());if(!screen)screen=QGuiApplication::primaryScreen();
 const int room=screen?screen->availableGeometry().width()*9/10:Ui::scaled(1200);
 const int inner=room-box->contentsMargins().left()-box->contentsMargins().right();
 auto* heading=new QHBoxLayout;heading->setSpacing(space(Space::s));
 auto* glyph=new QLabel(this);glyph->setObjectName("sensitiveGlyph");const int glyphSize=Ui::scaled(24);
 glyph->setPixmap(Ui::icon("alert",Theme::current().warning).pixmap(glyphSize,glyphSize));glyph->setFixedSize(glyphSize,glyphSize);heading->addWidget(glyph,0,Qt::AlignTop);
 auto* title=Ui::label(complete?QStringLiteral("Sensitive text found"):QStringLiteral("put.gd couldn't finish checking this capture"),"sectionTitle",this);
 title->setObjectName("sensitiveTitle");title->setWordWrap(true);heading->addWidget(title,1);box->addLayout(heading);
 QString body;
 const auto subject=model_.file.isEmpty()?QStringLiteral("This capture"):QStringLiteral("This image");
 if(any)body=QStringLiteral("%1 shows %2.").arg(subject,describeSensitive(model_.findings.found));
 // #1820: what was covered on its own stays covered, whichever choice is made but Cancel.
 const auto covers=describeSensitive(model_.covered);
 if(!covers.isEmpty())body=(body.isEmpty()?QString():body+" ")+QStringLiteral("put.gd already covered %1.").arg(covers);
 if(!complete)body=(body.isEmpty()?QString():body+" ")+sensitiveReasonText(model_.findings.reason);
 body+=model_.file.isEmpty()?QStringLiteral(" It hasn't been uploaded."):QStringLiteral(" Nothing has been uploaded yet.");
 auto* text=Ui::label(body.trimmed(),"body",this);text->setTextFormat(Qt::PlainText);text->setObjectName("sensitiveSummary");text->setWordWrap(true);box->addWidget(text);
 // A file-manager selection is answered as a whole: the editor edits one image.
 // A name with no spaces doesn't wrap: it is shortened in the middle to the panel's width.
 if(!model_.file.isEmpty()){auto* file=Ui::label(QString(),"meta",this);file->ensurePolished();file->setText(file->fontMetrics().elidedText(model_.file,Qt::ElideMiddle,inner)+(model_.redactAllowed?QStringLiteral(" · Redact and upload or Upload anyway sends this file and checks the rest, Cancel sends none"):QStringLiteral(" · Upload anyway keeps this file and checks the rest, Cancel sends none")));file->setTextFormat(Qt::PlainText);file->setObjectName("sensitiveFile");file->setWordWrap(true);box->addWidget(file);}
 if(!model_.preview.isNull()){
  // The capture as Redact and upload sends it (and Open in editor starts it), scaled down; no text is shown apart from the pixels.
  auto* preview=new QLabel(this);preview->setObjectName("sensitivePreview");preview->setAlignment(Qt::AlignCenter);
  const auto dpr=devicePixelRatioF();auto pixmap=QPixmap::fromImage(model_.preview.scaled(QSize(std::min(Ui::scaled(480),inner),Ui::scaled(300))*dpr,Qt::KeepAspectRatio,Qt::SmoothTransformation));pixmap.setDevicePixelRatio(dpr);
  preview->setPixmap(pixmap);preview->setAccessibleName(any?QStringLiteral("The capture with the suggested redactions"):QStringLiteral("The capture"));box->addWidget(preview,1);
 }
 auto* caution=Ui::label(QStringLiteral("Suggestions only. Recognition can miss text."),"secondary",this);caution->setObjectName("sensitiveCaution");caution->setWordWrap(true);box->addWidget(caution);
 if(!model_.destination.isEmpty()){auto* where=Ui::label(QString(),"meta",this);where->ensurePolished();where->setText(where->fontMetrics().elidedText(model_.destination,Qt::ElideMiddle,inner));where->setTextFormat(Qt::PlainText);where->setObjectName("sensitiveDestination");where->setWordWrap(true);box->addWidget(where);}
 // #1898: four choices. Cancel and Open in editor on the left, Upload anyway and the primary Redact and upload on
 // the right: one row where it fits the screen, else the right pair on a row of its own under the left pair, else
 // one button a row.
 cancel_=new QPushButton(QStringLiteral("Cancel"),this);cancel_->setObjectName("sensitiveCancel");cancel_->setToolTip(QStringLiteral("Upload nothing. With Keep a local copy on, the capture is saved locally."));
 if(model_.editAllowed){
  edit_=new QPushButton(Ui::icon("edit"),QStringLiteral("Open in editor"),this);edit_->setObjectName("sensitiveEdit");
  edit_->setToolTip(QStringLiteral("Open the editor with the suggested redactions placed, to change them; Upload there sends it"));
  connect(edit_,&QPushButton::clicked,this,[this]{choose("edit");});
 }
 upload_=new QPushButton(Ui::icon("upload"),QStringLiteral("Upload anyway"),this);upload_->setObjectName("sensitiveUpload");upload_->setToolTip(covers.isEmpty()?QStringLiteral("Upload the capture as it is"):QStringLiteral("Upload the capture with what put.gd already covered, and nothing else covered"));
 if(model_.redactAllowed){
  redact_=new QPushButton(Ui::icon("redact"),QStringLiteral("Redact and upload"),this);redact_->setObjectName("sensitiveRedact");
  redact_->setToolTip(QStringLiteral("Cover everything the preview shows covered and upload, without opening the editor"));Ui::role(redact_,"primary");redact_->setDefault(true);
  connect(redact_,&QPushButton::clicked,this,[this]{choose("redact_upload");});
 }
 connect(upload_,&QPushButton::clicked,this,[this]{choose("upload");});
 connect(cancel_,&QPushButton::clicked,this,[this]{choose("cancel");});
 {
  const auto row=[](std::initializer_list<QPushButton*> left,std::initializer_list<QPushButton*> right){
   auto* layout=new QHBoxLayout;layout->setSpacing(space(Space::s));
   for(auto* button:left)if(button)layout->addWidget(button);
   layout->addStretch();
   for(auto* button:right)if(button)layout->addWidget(button);
   return layout;
  };
  // Width of a row of these buttons with the spacing between them; the stretch needs none.
  const auto width=[](std::initializer_list<QPushButton*> buttons){int sum=0,count=0;for(auto* button:buttons)if(button){sum+=button->sizeHint().width();++count;}return sum+std::max(0,count-1)*space(Space::s);};
  if(width({cancel_,edit_,upload_,redact_})<=inner){rows_=1;box->addLayout(row({cancel_,edit_},{upload_,redact_}));}
  else if(std::max(width({cancel_,edit_}),width({upload_,redact_}))<=inner){rows_=2;box->addLayout(row({cancel_,edit_},{}));box->addLayout(row({},{upload_,redact_}));}
  // Not even a pair fits (a narrow screen at large text): one button a row, the primary last.
  else{rows_=0;for(auto* button:{cancel_,edit_,upload_,redact_})if(button){++rows_;box->addLayout(row({},{button}));}}
 }
 setMinimumWidth(std::min(Ui::scaled(460),room));
 // A fixed size: tiling compositors (Hyprland, Budgie's labwc) float such a window instead of tiling it.
 // Never wider than the room; wrapped text takes the height it needs at that width.
 adjustSize();const int width=std::min(sizeHint().expandedTo(minimumSize()).width(),room);
 setFixedSize(width,hasHeightForWidth()?heightForWidth(width):sizeHint().height());
}
void SensitivePrompt::paintEvent(QPaintEvent*){
 const auto& tokens=Theme::current();QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);
 const QRectF card=QRectF(rect()).adjusted(0.5,0.5,-0.5,-0.5);const qreal radius=Ui::scaled(12);
 QPainterPath path;path.addRoundedRect(card,radius,radius);
 painter.fillPath(path,tokens.surfaceRaised);
 // The accent edge says this is put.gd asking, before the text is read.
 painter.save();painter.setClipPath(path);painter.fillRect(QRectF(card.left(),card.top(),card.width(),Ui::scaled(4)),tokens.accent);painter.restore();
 painter.setPen(QPen(tokens.borderStrong,1));painter.drawPath(path);
}
void SensitivePrompt::mousePressEvent(QMouseEvent* event){
 // No title bar to hold: the panel moves by its body, through the desktop's own move.
 if(event->button()==Qt::LeftButton&&windowHandle()&&windowHandle()->startSystemMove()){event->accept();return;}
 QWidget::mousePressEvent(event);
}
void SensitivePrompt::showEvent(QShowEvent* event){
 QWidget::showEvent(event);
 // Centred on the screen with the pointer where the desktop lets a window choose (X11); Wayland places it itself.
 if(auto* screen=QGuiApplication::screenAt(QCursor::pos())){const auto area=screen->availableGeometry();move(area.center()-QPoint(width()/2,height()/2));}
 // Without Redact, nothing is chosen for the user: focus starts on Cancel.
 (redact_?redact_:cancel_)->setFocus(Qt::ActiveWindowFocusReason);
}
QJsonObject SensitivePrompt::state() const {
 const auto choices=QJsonArray::fromStringList(this->choices());
 auto out=model_.findings.json();out["shown"]=isVisible();
 {QJsonObject covers;for(const auto& [category,count]:model_.covered)if(count>0)covers[category]=count;if(!covers.isEmpty())out["covered"]=covers;}out["choices"]=choices;out["buttonRows"]=rows_;if(!model_.file.isEmpty())out["file"]=model_.file;return out;
}
QStringList SensitivePrompt::choices() const {
 QStringList out;if(redact_)out<<QStringLiteral("redact_upload");if(edit_)out<<QStringLiteral("edit");
 return out<<QStringLiteral("upload")<<QStringLiteral("cancel");
}
bool SensitivePrompt::choose(const QString& choice){
 if(decided_||!choices().contains(choice))return false;
 decided_=true;for(auto* button:{redact_,edit_,upload_,cancel_})if(button)button->setEnabled(false);
 emit chosen(choice);return true;
}
void SensitivePrompt::closeEvent(QCloseEvent* event){if(!decided_)choose("cancel");event->accept();}
void SensitivePrompt::keyPressEvent(QKeyEvent* event){
 if(event->key()==Qt::Key_Escape){choose("cancel");return;}
 // Not a dialog, so Return is ours: the focused choice, else the default one.
 if(event->key()==Qt::Key_Return||event->key()==Qt::Key_Enter){
  auto* button=qobject_cast<QPushButton*>(focusWidget());if(!button)button=redact_?redact_:upload_;
  if(button&&button->isEnabled())button->click();return;
 }
 QWidget::keyPressEvent(event);
}
}
