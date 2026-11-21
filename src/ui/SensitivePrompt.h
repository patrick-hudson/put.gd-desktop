#pragma once
#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QWidget>

class QLabel;class QPushButton;

// #1804: the one prompt before an upload whose capture shows sensitive text.
// It appears only when the check found something or could not finish; a clean
// capture uploads with no prompt. It names what was found by kind and count,
// never the text. It is put.gd's own panel, drawn from the theme tokens with
// no desktop title bar or frame: dragged by its body, Escape cancels. Choices: Redact and upload (opens the editor with the
// suggestions placed), Upload anyway, Cancel (also Escape and closing).
namespace putgd {
struct SensitiveFindings {
 QList<QPair<QString,int>> found; // Quick redact category and count, in the editor's order
 bool complete=true;
 QString reason; // why checking did not finish: recognition_failed, truncated, too_large, patterns_skipped, name_unchecked, unavailable
 QJsonObject json() const;
 bool any() const {for(const auto& entry:found)if(entry.second>0)return true;return false;}
};
// "a card number and 2 keys or tokens"
QString describeSensitive(const QList<QPair<QString,int>>& found);
// "The text was cut at its time limit", for reason codes
QString sensitiveReasonText(const QString& reason);
class SensitivePrompt final:public QWidget {
 Q_OBJECT
public:
 struct Model {
  SensitiveFindings findings;
  QImage preview; // the capture with the suggested redactions, or null
  bool redactAllowed=true;
  QString destination; // "Upload to S3 · Shared link"
  QString file; // "File 2 of 3: notes.png" for a file-manager selection, or empty
 };
 explicit SensitivePrompt(Model,QWidget* parent=nullptr);
 QJsonObject state() const;
 // What an agent's operation.decide or a button chose: redact, upload or cancel.
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
 Model model_;bool decided_=false;
 QPushButton *redact_=nullptr,*upload_=nullptr,*cancel_=nullptr;
};
}
