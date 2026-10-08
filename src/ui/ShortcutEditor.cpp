#include "ShortcutEditor.h"
#include "core/Settings.h"
#include <QFormLayout>
#include <QLineEdit>
#include <QLabel>
#include <QJsonDocument>
namespace putgd {
ShortcutEditor::ShortcutEditor(QWidget* parent):QWidget(parent) {
 auto* form=new QFormLayout(this);
 for(const auto& action:Settings::actionNames()) {auto* input=new QLineEdit(this);input->setObjectName("shortcut"+action.left(1).toUpper()+action.mid(1));input->setAccessibleName(action+" shortcut");editors_[action]=input;form->addRow(action,input);}
 status_=new QLabel(this);status_->setObjectName("shortcutStatus");status_->setWordWrap(true);form->addRow(status_);
}
void ShortcutEditor::setActions(const QJsonObject& actions){for(auto it=editors_.begin();it!=editors_.end();++it)it.value()->setText(actions[it.key()].toObject()["shortcut"].toString());}
QJsonObject ShortcutEditor::actions(const QJsonObject& base) const {auto actions=base;for(auto it=editors_.begin();it!=editors_.end();++it){auto action=actions[it.key()].toObject();action["shortcut"]=it.value()->text();actions[it.key()]=action;}return actions;}
void ShortcutEditor::setStatus(const QJsonObject& status){status_->setText("Actual shortcut bindings and conflicts:\n"+QString::fromUtf8(QJsonDocument(status).toJson(QJsonDocument::Indented)));}
}
