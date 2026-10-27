#include "AboutWindow.h"
#include "VisualStyle.h"
#include "Graphite.h"
#include "platform/HostOpen.h"
#include <QGridLayout>
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
 if(auto* name=findChild<QLabel*>("qt_msgbox_label")){VisualStyle::role(name,"sectionTitle");name->setFont(Graphite::font(Graphite::Type::section,true));}
 if(auto* info=findChild<QLabel*>("qt_msgbox_informativelabel")){
  info->setTextFormat(Qt::RichText);info->setText(QString(R"(<span style="font-family: '%1'; font-size: %2px">%3</span><br>Screen capture and file sharing)").arg(Graphite::monoFamily().toHtmlEscaped()).arg(Graphite::scaled(Graphite::Type::metadata)).arg(residentBuild["version"].toString().toHtmlEscaped()));
 }
 setDetailedText(QString::fromUtf8(QJsonDocument(residentBuild).toJson(QJsonDocument::Indented)));
 // The button box runs QMessageBox's disclosure handler before ours, even
 // when Qt 6.4 reconnects the button itself on show.
 for(auto* button:findChildren<QPushButton*>())if(standardButton(button)==QMessageBox::NoButton){button->setObjectName("buildDetails");button->setText("Build details");connect(findChild<QDialogButtonBox*>(),&QDialogButtonBox::clicked,this,[this,button](QAbstractButton* clicked){if(clicked!=button)return;updateDetailsButton();});}
 // A small branded dialog: the put.gd mark, the version, build details on request.
 setIconPixmap(VisualStyle::icon("brand").pixmap(48,48));
 auto* website=new QPushButton("put.gd website",this);website->setObjectName("putgdWebsite");VisualStyle::role(website,"link");website->setProperty("graphiteFlush",true);
 auto* browser=new Graphite::BrowserFeedback(this);browser->setObjectName("aboutWebsiteFeedback");
 if(auto* grid=qobject_cast<QGridLayout*>(layout())){const int row=grid->rowCount();grid->addWidget(website,row,2,Qt::AlignLeft);grid->addWidget(browser,row+1,2);}
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
