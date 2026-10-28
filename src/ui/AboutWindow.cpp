#include "AboutWindow.h"
#include "VisualStyle.h"
#include "Graphite.h"
#include "platform/HostOpen.h"
#include <QGridLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QDialogButtonBox>
#include <QJsonDocument>
#include <QPushButton>
#include <QStyle>
#include <QStyleOptionButton>
#include <QTextEdit>

namespace putgd {
AboutWindow::AboutWindow(const QJsonObject& residentBuild, QWidget* parent)
 : QMessageBox(QMessageBox::Information, "About put.gd",
               QString("put.gd"),
               QMessageBox::Close, parent) {
 setWindowModality(Qt::NonModal);
 setTextFormat(Qt::PlainText);
 setInformativeText("Screen capture and file sharing");
 if(auto* name=findChild<QLabel*>("qt_msgbox_label")){VisualStyle::role(name,"sectionTitle");}
 if(auto* info=findChild<QLabel*>("qt_msgbox_informativelabel"))info->setText("Screen capture and file sharing");
 setDetailedText(QString::fromUtf8(QJsonDocument(residentBuild).toJson(QJsonDocument::Indented)));
 // The button box runs QMessageBox's disclosure handler before ours, even
 // when Qt 6.4 reconnects the button itself on show.
 for(auto* button:findChildren<QPushButton*>())if(standardButton(button)==QMessageBox::NoButton){button->setObjectName("buildDetails");button->setText("Build details");connect(findChild<QDialogButtonBox*>(),&QDialogButtonBox::clicked,this,[this,button](QAbstractButton* clicked){if(clicked!=button)return;updateDetailsButton();});}
 // A small branded dialog, with the connected resident’s build on request.
 setIconPixmap(VisualStyle::icon("brand").pixmap(48,48));
 auto* website=new QPushButton("put.gd website",this);website->setObjectName("putgdWebsite");VisualStyle::role(website,"link");website->setProperty("graphiteFlush",true);
 auto* browser=new Graphite::BrowserFeedback(this);browser->setObjectName("aboutWebsiteFeedback");
 setMinimumWidth(Graphite::scaled(380));
 auto* version=new QLabel(this);version->setTextFormat(Qt::PlainText);version->setWordWrap(true);version->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);version->setObjectName("aboutVersion");version->setText(residentBuild["version"].toString());Graphite::setMono(version,Graphite::Type::metadata);version->setMinimumWidth(Graphite::scaled(220));version->setMaximumWidth(Graphite::scaled(280));
 if(auto* grid=qobject_cast<QGridLayout*>(layout())){
  auto* info=findChild<QLabel*>("qt_msgbox_informativelabel");auto* content=new QWidget(this);content->setMinimumWidth(Graphite::scaled(300));auto* column=new QVBoxLayout(content);column->setContentsMargins(0,0,0,0);column->setSpacing(Graphite::space(Graphite::Space::s));grid->removeWidget(info);column->addWidget(info);column->addWidget(website,0,Qt::AlignLeft);column->addWidget(version);column->addWidget(browser);grid->addWidget(content,1,2);
 }
 if(auto* buttons=findChild<QDialogButtonBox*>()){buttons->setCenterButtons(false);if(auto* grid=qobject_cast<QGridLayout*>(layout()))grid->setAlignment(buttons,Qt::AlignRight);}
 VisualStyle::role(button(QMessageBox::Close),"primary");setDefaultButton(QMessageBox::Close);
 connect(website,&QPushButton::clicked,this,[browser]{const QUrl url("https://put.gd");host::openLink(url,browser->opening(url));});
 updateDetailsButton();
}
void AboutWindow::showEvent(QShowEvent* event){
 QMessageBox::showEvent(event);
 // QMessageBox restores its generic disclosure label when shown.
 updateDetailsButton();
}
void AboutWindow::updateDetailsButton(){
 if(auto* button=findChild<QPushButton*>("buildDetails")){
  auto* details=findChild<QTextEdit*>();button->setText(details&&details->isVisibleTo(this)?"Hide build details":"Build details");
  // Qt's private disclosure button measures only its built-in labels.
  QStyleOptionButton option;option.initFrom(button);option.text="Hide build details";
  button->setMinimumWidth(button->style()->sizeFromContents(QStyle::CT_PushButton,&option,button->fontMetrics().size(Qt::TextShowMnemonic,option.text),button).width());
 }
}
}
