#include "AboutWindow.h"
#include "VisualStyle.h"
#include <QJsonDocument>
#include <QPushButton>
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
 for(auto* button:findChildren<QPushButton*>())if(standardButton(button)==QMessageBox::NoButton){button->setObjectName("buildDetails");button->setText("Build details");connect(button,&QPushButton::clicked,this,[this,button]{auto* details=findChild<QTextEdit*>();button->setText(details&&details->isVisibleTo(this)?"Hide build details":"Build details");});}
 // A small branded dialog: the put.gd mark, the version, build details on request.
 setIconPixmap(VisualStyle::icon("brand").pixmap(48,48));
}
void AboutWindow::showEvent(QShowEvent* event){
 QMessageBox::showEvent(event);
 // QMessageBox restores its generic disclosure label when shown.
 if(auto* button=findChild<QPushButton*>("buildDetails")){auto* details=findChild<QTextEdit*>();button->setText(details&&details->isVisibleTo(this)?"Hide build details":"Build details");}
}
}
