#include "AboutWindow.h"
#include "VisualStyle.h"
#include <QJsonDocument>

namespace putgd {
AboutWindow::AboutWindow(const QJsonObject& residentBuild, QWidget* parent)
 : QMessageBox(QMessageBox::Information, "About put.gd",
               QString("put.gd %1").arg(residentBuild["version"].toString()),
               QMessageBox::Ok, parent) {
 setWindowModality(Qt::NonModal);
 setTextFormat(Qt::PlainText);
 setInformativeText("Screen capture and file sharing");
 setDetailedText(QString::fromUtf8(QJsonDocument(residentBuild).toJson(QJsonDocument::Indented)));
 // A small branded dialog: the put.gd mark, the version, build details on request.
 setIconPixmap(VisualStyle::icon("brand").pixmap(48,48));
}
}
