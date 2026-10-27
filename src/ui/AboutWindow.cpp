#include "AboutWindow.h"
#include "VisualStyle.h"
#include <QDialogButtonBox>
#include <QJsonDocument>
#include <QPushButton>
#include <QStyle>
#include <QStyleOptionButton>
#include <QTextEdit>

namespace putgd {
AboutWindow::AboutWindow(const QJsonObject& residentBuild, QWidget* parent)
 : QMessageBox(QMessageBox::Information, "About put.gd",
               QString("put.gd %1").arg(residentBuild["version"].toString()),
               QMessageBox::Close, parent) {
 setWindowModality(Qt::NonModal);
 setTextFormat(Qt::PlainText);
 setInformativeText("Screen capture and file sharing");
 setDetailedText(QString::fromUtf8(QJsonDocument(residentBuild).toJson(QJsonDocument::Indented)));
 // The button box runs QMessageBox's disclosure handler before ours, even
 // when Qt 6.4 reconnects the button itself on show.
 for(auto* button:findChildren<QPushButton*>())if(standardButton(button)==QMessageBox::NoButton){button->setObjectName("buildDetails");button->setText("Build details");connect(findChild<QDialogButtonBox*>(),&QDialogButtonBox::clicked,this,[this,button](QAbstractButton* clicked){if(clicked!=button)return;updateDetailsButton();});}
 // A small branded dialog: the put.gd mark, the version, build details on request.
 setIconPixmap(VisualStyle::icon("brand").pixmap(48,48));
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
