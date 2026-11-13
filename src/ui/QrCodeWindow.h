#pragma once
#include "core/Error.h"
#include "core/QrImage.h"
#include <QDialog>
#include <QJsonObject>
#include <functional>
class QLabel;class QPushButton;
namespace putgd {
class FileDialog;
// History's Show QR code (#1428): the code of the link Copy link would copy,
// with Copy image and Save. The code is black on white in both themes and
// drawn at the largest whole number of pixels per module that fits the code
// area (560 px a side by default, growing with the window), never under 3.
// Copy image and Save go through the resident (history.qr), which refreshes the
// link again and writes the 1,024 px PNG; this window touches neither the
// clipboard nor a file.
class QrCodeWindow final:public QDialog {
 Q_OBJECT
public:
 using Reply=std::function<void(Error,QJsonObject)>;
 struct Actions {
  std::function<void(Reply)> copy;
  // overwrite: the chooser already asked about replacing that file.
  std::function<void(const QString& path,bool overwrite,Reply)> save;
 };
 QrCodeWindow(const QString& historyId,const QString& name,const QString& url,Actions,QWidget* parent=nullptr);
 QString historyId() const {return historyId_;}
 // What history.qr show answers: shown, historyId, modules, modulePixels,
 // codePixels, and the last copy or save.
 QJsonObject state() const;
 // The pixels per module the code area draws now.
 int modulePixels() const;
private:
 void copyImage();void saveImage();
 QString historyId_,name_;qr::Matrix matrix_;Actions actions_;
 QWidget* code_=nullptr;QLabel* status_=nullptr;QPushButton *copy_=nullptr,*save_=nullptr;
 FileDialog* chooser_=nullptr;QJsonObject last_;
};
}
