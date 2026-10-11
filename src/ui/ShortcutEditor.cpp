#include "ShortcutEditor.h"
#include "VisualStyle.h"
#include "core/Settings.h"
#include <QApplication>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QToolButton>
#include <QVBoxLayout>

namespace putgd {
namespace {
QString title(const QString& action) { return action.left(1).toUpper()+action.mid(1); }
QString actualBinding(const QJsonValue& actual) {
 if(actual.isString())return actual.toString();
 QStringList bindings;
 for(const auto& value:actual.toArray())if(!value.toString().isEmpty())bindings.append(value.toString());
 return bindings.join(" / ");
}
struct BindingPresentation { QString text;const char* tone; };
BindingPresentation bindingPresentation(const QJsonObject& status,const QJsonObject& entry) {
 const auto state=entry["state"].toString();
 const auto error=entry["error"].isObject()?entry["error"].toObject()["code"].toString():entry["error"].toString();
 const auto actual=actualBinding(entry["actual"]);
 if(error=="disabled"||(state=="unbound"&&entry["requested"].toString().isEmpty()))
  return {"Disabled  ·  No global shortcut is assigned.","muted"};
 if(status.contains("available")&&!status["available"].toBool())
  return {"Unavailable  ·  Set up desktop shortcut support to enable this action.","warning"};
 if(state=="shortcut_conflict"||error=="shortcut_conflict")
  return {"Conflict  ·  Choose another shortcut."+(actual.isEmpty()?QString{}:" Desktop binding: "+actual),"error"};
 if(state=="shortcut_invalid"||error=="shortcut_invalid")
  return {"Invalid shortcut  ·  Enter a single key combination.","error"};
 if(state=="shortcut_unsupported"||error=="shortcut_unsupported")
  return {"Unsupported shortcut  ·  Try one key with modifiers.","warning"};
 if(state=="shortcut_unmapped"||error=="shortcut_unmapped")
  return {"Unavailable  ·  Your desktop cannot map this key combination.","warning"};
 if(state=="shortcut_unavailable"||error=="shortcut_unavailable"||error=="helper_unavailable")
  return {"Unavailable  ·  Enable your desktop shortcut service or helper.","warning"};
 if(state=="pending")return {"Binding  ·  Waiting for the desktop to confirm.","muted"};
 // Bound is the runtime observation, not a promise inferred from the draft.
 if(entry["bound"].toBool()&&error.isEmpty())
  return {"Active  ·  "+(actual.isEmpty()?QString("Binding confirmed by the desktop."):actual),"active"};
 if(!error.isEmpty())return {"Not bound  ·  Check desktop support for this action.","warning"};
 if(entry.isEmpty())return {"Not reported  ·  Refresh desktop support to check this action.","muted"};
 return {"Not bound  ·  Choose a shortcut or refresh desktop support.","muted"};
}
}
ShortcutEditor::ShortcutEditor(QWidget* parent):QWidget(parent) {
 VisualStyle::apply(*qApp);
 auto* layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(16);
 status_=new QLabel(this);status_->setObjectName("shortcutStatus");status_->setWordWrap(true);VisualStyle::role(status_,"muted");layout->addWidget(status_);
 auto* form=new QFormLayout;VisualStyle::styleForm(form);layout->addLayout(form);
 const QHash<QString,QString> labels{{"region","Screen region"},{"window","Active window"},{"desktop","Full desktop"},{"clipboard","Clipboard"}};
 const auto defaultActions=Settings::defaults()["actions"].toObject();
 for(const auto& action:Settings::actionNames()) {
  auto* field=new QWidget(this);auto* fieldLayout=new QVBoxLayout(field);fieldLayout->setContentsMargins(0,0,0,0);fieldLayout->setSpacing(6);
  auto* input=new QLineEdit(field);input->setObjectName("shortcut"+title(action));input->setAccessibleName(action+" shortcut");input->setPlaceholderText("For example: Ctrl+Shift+1");editors_[action]=input;fieldLayout->addWidget(input);
  auto* binding=new QLabel(field);binding->setObjectName("shortcut"+title(action)+"Status");binding->setAccessibleName(action+" actual shortcut status");binding->setWordWrap(true);VisualStyle::role(binding,"binding");bindingStates_[action]=binding;fieldLayout->addWidget(binding);
  const auto defaultShortcut=defaultActions[action].toObject()["shortcut"].toString();
  const auto label=labels.value(action)+" shortcut";
  form->addRow(VisualStyle::settingLabel(label,"The global key combination for the "+labels.value(action).toLower()+" action. The default is "+(defaultShortcut.isEmpty()?QString("unassigned"):defaultShortcut)+". Leave it empty to disable this shortcut. Enter one combination, such as Ctrl+Shift+1; save settings to request it from the desktop. The status below reports the actual binding, including conflicts or unsupported keys.",input,this),field);
 }
 auto* toggle=new QToolButton(this);toggle->setObjectName("shortcutDetailsToggle");toggle->setAccessibleName("Show exact desktop shortcut diagnostics");toggle->setText("Advanced binding details");toggle->setCheckable(true);toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);toggle->setArrowType(Qt::RightArrow);layout->addWidget(toggle);
 details_=new QPlainTextEdit(this);details_->setObjectName("shortcutDetails");details_->setAccessibleName("Exact desktop shortcut diagnostics JSON");details_->setReadOnly(true);details_->setMinimumHeight(120);details_->setMaximumHeight(220);layout->addWidget(details_);details_->hide();
 connect(toggle,&QToolButton::toggled,this,[this,toggle](bool visible){details_->setVisible(visible);toggle->setArrowType(visible?Qt::DownArrow:Qt::RightArrow);});
 setStatus({});
}
void ShortcutEditor::setActions(const QJsonObject& actions){for(auto it=editors_.begin();it!=editors_.end();++it)it.value()->setText(actions[it.key()].toObject()["shortcut"].toString());}
QJsonObject ShortcutEditor::actions(const QJsonObject& base) const {auto actions=base;for(auto it=editors_.begin();it!=editors_.end();++it){auto action=actions[it.key()].toObject();action["shortcut"]=it.value()->text();actions[it.key()]=action;}return actions;}
void ShortcutEditor::setStatus(const QJsonObject& status) {
 if(!status.contains("available"))status_->setText("Desktop bindings have not been reported. Refresh desktop support to check them.");
 else if(!status["available"].toBool())status_->setText("Global shortcuts are unavailable on this desktop. Check desktop support to restore shortcut bindings.");
 else status_->setText("Actual desktop bindings are shown below. Edits apply after you save settings.");
 const auto bindings=status["bindings"].toObject();
 for(const auto& action:Settings::actionNames()) {
  auto* label=bindingStates_[action];const auto presentation=bindingPresentation(status,bindings[action].toObject());label->setText(presentation.text);label->setProperty("bindingTone",presentation.tone);VisualStyle::role(label,"binding");
 }
 details_->setPlainText(QString::fromUtf8(QJsonDocument(status).toJson(QJsonDocument::Indented)));
}
}
