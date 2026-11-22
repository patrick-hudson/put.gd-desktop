#pragma once
#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QWidget>
#include "core/SensitiveText.h"

class QLabel;class QPushButton;

// #1804: the one prompt before an upload whose capture shows sensitive text.
// It appears only when the check found something or could not finish; a clean
// capture uploads with no prompt. It names what was found by kind and count,
// never the text. It is put.gd's own panel, drawn from the theme tokens with
// no desktop title bar or frame: dragged by its body, Escape cancels. Choices (#1898): Redact and upload
// (covers exactly what the preview shows and uploads, no editor), Open in editor (the editor with the
// suggestions placed), Upload anyway, Cancel (also Escape and closing).
namespace putgd {
struct SensitiveFindings {
 QList<QPair<QString,int>> found; // Quick redact category and count, in the editor's order
 bool complete=true;
 QString reason; // why checking did not finish: recognition_failed, truncated, too_large, patterns_skipped, name_unchecked, unavailable
 QJsonObject json() const;
 bool any() const {for(const auto& entry:found)if(entry.second>0)return true;return false;}
};
// "The text was cut at its time limit", for reason codes
QString sensitiveReasonText(const QString& reason);
class SensitivePrompt final:public QWidget {
 Q_OBJECT
public:
 struct Model {
  SensitiveFindings findings;
  // #1820: what was covered automatically before this prompt (redact kinds), by kind and count.
  QList<QPair<QString,int>> covered;
  QImage preview; // the capture with the suggested redactions, or null
  // #1898: Redact and upload is offered when something was found to cover; Open in editor when the
  // editor can take this image (one image, not a file of a larger selection).
  bool redactAllowed=true,editAllowed=true;
  QString destination; // "Upload to S3 · Shared link"
  QString file; // "File 2 of 3: notes.png" for a file-manager selection, or empty
 };
 explicit SensitivePrompt(Model,QWidget* parent=nullptr);
 QJsonObject state() const;
 // What an agent's operation.decide or a button chose: redact_upload, edit, upload or cancel.
 QStringList choices() const;
 bool choose(const QString& choice);
signals:
 void chosen(const QString& choice);
protected:
 void closeEvent(QCloseEvent*) override;
 void keyPressEvent(QKeyEvent*) override;
 void paintEvent(QPaintEvent*) override;
 void mousePressEvent(QMouseEvent*) override;
 void showEvent(QShowEvent*) override;
private:
 Model model_;bool decided_=false;int rows_=1;
 QPushButton *redact_=nullptr,*edit_=nullptr,*upload_=nullptr,*cancel_=nullptr;
};
}
