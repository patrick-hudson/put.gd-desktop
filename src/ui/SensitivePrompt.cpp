#include "SensitivePrompt.h"
#include "Ui.h"
#include "Theme.h"
#include <QCursor>
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
 setWindowTitle(complete?QStringLiteral("Sensitive text found — put.gd"):QStringLiteral("Check before uploading — put.gd"));
 Ui::styleWindow(*this);
 auto* box=new QVBoxLayout(this);box->setContentsMargins(space(Space::xl),space(Space::xl),space(Space::xl),space(Space::l));box->setSpacing(space(Space::m));
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
 if(!model_.file.isEmpty()){auto* file=Ui::label(model_.file+QStringLiteral(" · Upload anyway keeps this file and checks the rest, Cancel sends none"),"meta",this);file->setTextFormat(Qt::PlainText);file->setObjectName("sensitiveFile");file->setWordWrap(true);box->addWidget(file);}
 if(!model_.preview.isNull()){
  // The capture as Redact and upload would start, scaled down; no text is shown apart from the pixels.
  auto* preview=new QLabel(this);preview->setObjectName("sensitivePreview");preview->setAlignment(Qt::AlignCenter);
  const auto dpr=devicePixelRatioF();auto pixmap=QPixmap::fromImage(model_.preview.scaled(QSize(Ui::scaled(480),Ui::scaled(300))*dpr,Qt::KeepAspectRatio,Qt::SmoothTransformation));pixmap.setDevicePixelRatio(dpr);
  preview->setPixmap(pixmap);preview->setAccessibleName(any?QStringLiteral("The capture with the suggested redactions"):QStringLiteral("The capture"));box->addWidget(preview,1);
 }
 auto* caution=Ui::label(QStringLiteral("Suggestions only. Recognition can miss text."),"secondary",this);caution->setObjectName("sensitiveCaution");caution->setWordWrap(true);box->addWidget(caution);
 if(!model_.destination.isEmpty()){auto* where=Ui::label(model_.destination,"meta",this);where->setTextFormat(Qt::PlainText);where->setObjectName("sensitiveDestination");where->setWordWrap(true);box->addWidget(where);}
 auto* buttons=new QHBoxLayout;buttons->setSpacing(space(Space::s));
 cancel_=new QPushButton(QStringLiteral("Cancel"),this);cancel_->setObjectName("sensitiveCancel");cancel_->setToolTip(QStringLiteral("Upload nothing. With Keep a local copy on, the capture is saved locally."));
 upload_=new QPushButton(Ui::icon("upload"),QStringLiteral("Upload anyway"),this);upload_->setObjectName("sensitiveUpload");upload_->setToolTip(covers.isEmpty()?QStringLiteral("Upload the capture as it is"):QStringLiteral("Upload the capture with what put.gd already covered, and nothing else covered"));
 buttons->addWidget(cancel_);buttons->addStretch();buttons->addWidget(upload_);
 if(model_.redactAllowed){
  redact_=new QPushButton(Ui::icon("redact"),QStringLiteral("Redact and upload"),this);redact_->setObjectName("sensitiveRedact");
  redact_->setToolTip(QStringLiteral("Open the editor with the suggested redactions placed; Upload there sends it"));Ui::role(redact_,"primary");redact_->setDefault(true);buttons->addWidget(redact_);
  connect(redact_,&QPushButton::clicked,this,[this]{choose("redact");});
 }
 box->addLayout(buttons);
 connect(upload_,&QPushButton::clicked,this,[this]{choose("upload");});
 connect(cancel_,&QPushButton::clicked,this,[this]{choose("cancel");});
 setMinimumWidth(Ui::scaled(460));
 // A fixed size: tiling compositors (Hyprland, Budgie's labwc) float such a window instead of tiling it.
 adjustSize();setFixedSize(sizeHint().expandedTo(minimumSize()));
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
 QJsonArray choices{"upload","cancel"};if(model_.redactAllowed)choices.prepend("redact");
 auto out=model_.findings.json();out["shown"]=isVisible();
 {QJsonObject covers;for(const auto& [category,count]:model_.covered)if(count>0)covers[category]=count;if(!covers.isEmpty())out["covered"]=covers;}out["choices"]=choices;if(!model_.file.isEmpty())out["file"]=model_.file;return out;
}
bool SensitivePrompt::choose(const QString& choice){
 if(decided_||!(choice=="upload"||choice=="cancel"||(choice=="redact"&&model_.redactAllowed)))return false;
 decided_=true;for(auto* button:{redact_,upload_,cancel_})if(button)button->setEnabled(false);
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
