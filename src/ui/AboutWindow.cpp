#include "AboutWindow.h"
#include "VisualStyle.h"
#include "Graphite.h"
#include "platform/HostOpen.h"
#include <QGridLayout>
#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QDialogButtonBox>
#include <QJsonDocument>
#include <QPushButton>
#include <QStyle>
#include <QStyleOptionButton>
#include <QTextEdit>
#include <QTextDocument>
#include <QScreen>
#include <QtMath>

namespace putgd {
AboutWindow::AboutWindow(const QJsonObject& residentBuild, QWidget* parent)
 : QMessageBox(QMessageBox::Information, "About put.gd",
               QString("put.gd"),
               QMessageBox::Close, parent) {
 VisualStyle::apply(*qApp);
 setWindowModality(Qt::NonModal);
 setTextFormat(Qt::PlainText);
 setInformativeText("Screen capture and file sharing");
 setDetailedText(QString::fromUtf8(QJsonDocument(residentBuild).toJson(QJsonDocument::Indented)));
 // The button box runs QMessageBox's disclosure handler before ours, even
 // when Qt 6.4 reconnects the button itself on show.
 for(auto* button:findChildren<QPushButton*>())if(standardButton(button)==QMessageBox::NoButton){button->setObjectName("buildDetails");VisualStyle::role(button,"quiet");button->setText("Build details");connect(findChild<QDialogButtonBox*>(),&QDialogButtonBox::clicked,this,[this,button](QAbstractButton* clicked){if(clicked!=button)return;updateDetailsButton();});}
 // A small branded dialog, with the connected resident’s build on request.
 setIconPixmap(VisualStyle::icon("brand").pixmap(Graphite::scaled(64),Graphite::scaled(64)));
 auto* website=new QPushButton(VisualStyle::icon("open-external"),"put.gd website",this);website->setObjectName("putgdWebsite");VisualStyle::role(website,"link");website->setProperty("graphiteFlush",true);
 auto* browser=new Graphite::BrowserFeedback(this);browser->setObjectName("aboutWebsiteFeedback");
 setMinimumWidth(Graphite::scaled(380));
 auto* version=new QLabel(this);version->setTextFormat(Qt::PlainText);version->setWordWrap(false);version->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);version->setObjectName("aboutVersion");version->setText(residentBuild["version"].toString().section('-',0,0).section('+',0,0));version->setToolTip(residentBuild["version"].toString());VisualStyle::role(version,"binding");version->setMinimumWidth(Graphite::scaled(240));version->setMaximumWidth(Graphite::scaled(320));
 if(auto* grid=qobject_cast<QGridLayout*>(layout())){
  auto* message=findChild<QLabel*>("qt_msgbox_label");auto* informative=findChild<QLabel*>("qt_msgbox_informativelabel");message->setObjectName("aboutMessageText");informative->setObjectName("aboutMessageInfo");auto* icon=findChild<QLabel*>("qt_msgboxex_icon_label");auto* buttons=findChild<QDialogButtonBox*>();auto* details=findChild<QTextEdit*>();QLayoutItem* detailsItem=nullptr;
  // Keep Qt's build-details disclosure intact while replacing its message grid.
  while(auto* item=grid->takeAt(0)){if(details&&((item->layout()&&item->layout()->indexOf(details)>=0)||(item->widget()&&(item->widget()==details||item->widget()->isAncestorOf(details)))))detailsItem=item;else delete item;}
  for(int row=0;row<grid->rowCount();++row){grid->setRowMinimumHeight(row,0);grid->setRowStretch(row,0);}for(int column=0;column<grid->columnCount();++column){grid->setColumnMinimumWidth(column,0);grid->setColumnStretch(column,0);}
  grid->setContentsMargins(Graphite::space(Graphite::Space::xl),Graphite::space(Graphite::Space::xl),Graphite::space(Graphite::Space::xl),Graphite::space(Graphite::Space::xl));grid->setSpacing(0);
  auto* content=new QWidget(this);content->setObjectName("aboutContent");content->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Minimum);content->setMinimumWidth(Graphite::scaled(380)-2*Graphite::space(Graphite::Space::xl));auto* row=new QHBoxLayout(content);row->setContentsMargins(0,0,0,0);row->setSpacing(Graphite::space(Graphite::Space::l));if(icon)row->addWidget(icon,0,Qt::AlignTop);
  // Qt sizes its private text labels at the dialog font; the branded heading
  // needs its own label and line box. Keep the message properties intact.
  message->hide();informative->hide();auto* name=new QLabel(text(),content);name->setObjectName("qt_msgbox_label");VisualStyle::role(name,"title");auto* info=new QLabel(informativeText(),content);info->setObjectName("qt_msgbox_informativelabel");info->setWordWrap(true);VisualStyle::role(info,"secondary");
  auto* column=new QVBoxLayout;column->setContentsMargins(0,0,0,0);column->setSpacing(Graphite::space(Graphite::Space::s));row->addLayout(column,1);name->setAlignment(Qt::AlignLeft|Qt::AlignTop);column->addWidget(name);info->setContentsMargins(0,0,0,0);column->addWidget(info);column->addWidget(version);column->addWidget(website,0,Qt::AlignLeft);column->addWidget(browser);
  grid->addWidget(content,0,0);grid->setRowMinimumHeight(1,Graphite::space(Graphite::Space::xl));buttons->setCenterButtons(false);grid->addWidget(buttons,2,0,Qt::AlignRight);if(detailsItem)grid->addItem(detailsItem,3,0);
 }
 VisualStyle::role(button(QMessageBox::Close),"lead");setDefaultButton(QMessageBox::Close);
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
  auto* details=findChild<QTextEdit*>();if(details){VisualStyle::role(details,"mono");const int height=6*details->fontMetrics().lineSpacing()+qCeil(2*details->document()->documentMargin())+2*details->frameWidth();details->setFixedHeight(qMin(height,screen()->availableGeometry().height()/3));}button->setText(details&&details->isVisibleTo(this)?"Hide build details":"Build details");
  // Qt's private disclosure button measures only its built-in labels.
  QStyleOptionButton option;option.initFrom(button);option.text="Hide build details";
  button->setMinimumWidth(button->style()->sizeFromContents(QStyle::CT_PushButton,&option,button->fontMetrics().size(Qt::TextShowMnemonic,option.text),button).width());
 }
}
}
