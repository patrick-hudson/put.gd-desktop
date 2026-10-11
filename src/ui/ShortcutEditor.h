#pragma once
#include <QWidget>
#include <QHash>
#include <QJsonObject>
class QLineEdit;
class QLabel;
class QPlainTextEdit;
namespace putgd {
class ShortcutEditor final:public QWidget {
public:
 explicit ShortcutEditor(QWidget* parent=nullptr);
 void setActions(const QJsonObject&);
 QJsonObject actions(const QJsonObject&) const;
 void setStatus(const QJsonObject&);
 const QHash<QString,QLineEdit*>& editors() const{return editors_;}
private:QHash<QString,QLineEdit*> editors_;QHash<QString,QLabel*> bindingStates_;QLabel* status_;QPlainTextEdit* details_;
};
}
