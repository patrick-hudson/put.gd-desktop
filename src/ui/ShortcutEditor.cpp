#include "ShortcutEditor.h"
#include "VisualStyle.h"
#include "core/Settings.h"
#include <QApplication>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QKeyEvent>
#include <QPointer>
#include <QPushButton>
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
 if(state=="recording")return {"Paused  ·  Recording a shortcut.","muted"};
 if(error=="disabled"||(state=="unbound"&&entry["requested"].toString().isEmpty()))
  return {"Disabled  ·  No global shortcut is assigned.","muted"};
 if(status.contains("available")&&!status["available"].toBool())
  return {"Unavailable  ·  Set up desktop shortcut support to enable this action.","warning"};
 if(state=="shortcut_conflict"||error=="shortcut_conflict")
  return {"Conflict  ·  Choose another shortcut."+(actual.isEmpty()?QString{}:" Desktop binding: "+actual),"error"};
 if(state=="shortcut_invalid"||error=="shortcut_invalid")
  return {"Invalid shortcut  ·  Enter a single key combination.","error"};
 if(state=="shortcut_unsupported"||error=="shortcut_unsupported")
  return {"Unsupported shortcut  ·  Your desktop cannot use this key combination.","warning"};
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
 recordingStatus_=new QLabel(this);recordingStatus_->setObjectName("shortcutRecordingStatus");recordingStatus_->setAccessibleName("Shortcut recording status");recordingStatus_->setWordWrap(true);VisualStyle::role(recordingStatus_,"status");layout->addWidget(recordingStatus_);recordingStatus_->hide();
 auto* form=new QFormLayout;VisualStyle::styleForm(form);layout->addLayout(form);
 const QHash<QString,QString> labels{{"region","Screen region"},{"window","Active window"},{"desktop","Full desktop"},{"clipboard","Clipboard"}};
 const auto defaultActions=Settings::defaults()["actions"].toObject();
 for(const auto& action:Settings::actionNames()) {
  auto* field=new QWidget(this);auto* fieldLayout=new QVBoxLayout(field);fieldLayout->setContentsMargins(0,0,0,0);fieldLayout->setSpacing(6);
  auto* row=new QHBoxLayout;fieldLayout->addLayout(row);
  auto* input=new QLineEdit(field);input->setObjectName("shortcut"+title(action));input->setAccessibleName(action+" shortcut");input->setReadOnly(true);input->setPlaceholderText("Disabled");input->setAccessibleDescription("Choose Record shortcut, press one key with modifiers, then release all keys. Escape cancels. Changes save automatically.");editors_[action]=input;row->addWidget(input,1);
  auto* record=new QPushButton("Record shortcut",field);record->setObjectName("record"+title(action)+"Shortcut");record->setAccessibleName("Record "+labels.value(action).toLower()+" shortcut");recordButtons_[action]=record;row->addWidget(record);
  auto* clear=new QPushButton("Clear",field);clear->setObjectName("clear"+title(action)+"Shortcut");clear->setAccessibleName("Clear "+labels.value(action).toLower()+" shortcut");clear->setToolTip("Disable this shortcut.");row->addWidget(clear);
  connect(record,&QPushButton::clicked,this,[this,action]{if(recordingAction_==action)cancelRecording();else startRecording(action);});
  connect(clear,&QPushButton::clicked,this,[this,input]{cancelRecording();input->clear();});
  auto* binding=new QLabel(field);binding->setObjectName("shortcut"+title(action)+"Status");binding->setAccessibleName(action+" actual shortcut status");binding->setWordWrap(true);VisualStyle::role(binding,"binding");bindingStates_[action]=binding;fieldLayout->addWidget(binding);
  const auto defaultShortcut=defaultActions[action].toObject()["shortcut"].toString();
  const auto label=labels.value(action)+" shortcut";
  form->addRow(VisualStyle::settingLabel(label,"The global key combination for the "+labels.value(action).toLower()+" action. The default is "+(defaultShortcut.isEmpty()?QString("unassigned"):defaultShortcut)+". Choose Record shortcut, press one key with optional modifiers, then release all keys to keep it in the draft. Escape or Cancel stops recording without changing the draft. Clear disables the shortcut. Completed edits save automatically; the desktop applies the new shortcut. The status below reports the actual binding, including conflicts or unsupported keys. JSON configuration still accepts portable text such as Ctrl+Shift+1.",input,this),field);
 }
 auto* toggle=new QToolButton(this);toggle->setObjectName("shortcutDetailsToggle");toggle->setAccessibleName("Show exact desktop shortcut diagnostics");toggle->setText("Advanced binding details");toggle->setCheckable(true);toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);toggle->setArrowType(Qt::RightArrow);layout->addWidget(toggle);
 details_=new QPlainTextEdit(this);details_->setObjectName("shortcutDetails");details_->setAccessibleName("Exact desktop shortcut diagnostics JSON");details_->setReadOnly(true);details_->setMinimumHeight(120);details_->setMaximumHeight(220);layout->addWidget(details_);details_->hide();
 connect(toggle,&QToolButton::toggled,this,[this,toggle](bool visible){details_->setVisible(visible);toggle->setArrowType(visible?Qt::DownArrow:Qt::RightArrow);});
 setStatus({});
}
ShortcutEditor::~ShortcutEditor(){cancelRecording();}
void ShortcutEditor::setActions(const QJsonObject& actions){cancelRecording();for(auto it=editors_.begin();it!=editors_.end();++it)it.value()->setText(actions[it.key()].toObject()["shortcut"].toString());}
void ShortcutEditor::startRecording(const QString& action){
 cancelRecording();recordingAction_=action;recordingReady_=false;chord_={};pressed_.clear();
 const auto generation=++recordingGeneration_;
 recordButtons_[action]->setText("Cancel");recordButtons_[action]->setFocusPolicy(Qt::NoFocus);
 recordingStatus_->setText("Preparing to record… Escape cancels.");recordingStatus_->show();
 editors_[action]->setFocus();qApp->installEventFilter(this);
 if(!recordingControl_){finishRecording("Recording is unavailable. Reopen Settings from the running app.");return;}
 QPointer<ShortcutEditor> guard(this);
 recordingControl_(true,[guard,generation](Error error){
  if(!guard||generation!=guard->recordingGeneration_)return;
  if(error){guard->finishRecording("Cannot record: "+error.message);return;}
  guard->recordingReady_=true;
  guard->recordingStatus_->setText("Press shortcut… Release all keys to keep it. Escape cancels.");
 });
}
void ShortcutEditor::cancelRecording(){if(!recordingAction_.isEmpty())finishRecording("Recording cancelled. The draft shortcut is unchanged.");}
void ShortcutEditor::finishRecording(const QString& message){
 if(recordingAction_.isEmpty())return;
 ++recordingGeneration_;recordingReady_=false;qApp->removeEventFilter(this);
 recordButtons_[recordingAction_]->setText("Record shortcut");recordButtons_[recordingAction_]->setFocusPolicy(Qt::StrongFocus);recordingAction_.clear();pressed_.clear();chord_={};
 recordingStatus_->setText(message);
 if(recordingControl_){QPointer<ShortcutEditor> guard(this);const auto generation=recordingGeneration_;recordingControl_(false,[guard,generation](Error error){
  // Saving settings can supersede this restore. The newer binding request owns
  // its result, which is reported by the actual desktop binding labels.
  if(guard&&generation==guard->recordingGeneration_&&error&&error.code!="shortcut_recording_interrupted")guard->recordingStatus_->setText("Could not restore shortcuts: "+error.message);
 });}
}
bool ShortcutEditor::eventFilter(QObject* object,QEvent* event){
 if(recordingAction_.isEmpty())return false;
 auto* input=editors_[recordingAction_];
 if((object==input&&event->type()==QEvent::FocusOut)||(object==this&&event->type()==QEvent::Hide)||(object==window()&&(event->type()==QEvent::WindowDeactivate||event->type()==QEvent::Close||event->type()==QEvent::Hide))||event->type()==QEvent::ApplicationDeactivate){cancelRecording();return false;}
 if(event->type()!=QEvent::ShortcutOverride&&event->type()!=QEvent::KeyPress&&event->type()!=QEvent::KeyRelease)return false;
 auto* key=static_cast<QKeyEvent*>(event);event->accept();
 if(event->type()==QEvent::ShortcutOverride)return true;
 if(key->key()==Qt::Key_Escape){cancelRecording();return true;}
 if(key->key()==Qt::Key_Backspace&&event->type()==QEvent::KeyPress&&recordingReady_){input->clear();finishRecording("Shortcut cleared. Changes save automatically.");return true;}
 if(!recordingReady_||key->isAutoRepeat())return true;
 // Shift can change a key's Qt symbol between press and release. Native scan
 // codes keep that physical key (and left/right modifiers) paired correctly.
 const quint64 identity=key->nativeScanCode()?quint64(key->nativeScanCode()):(quint64(1)<<32)|quint32(key->key());
 const auto modifier=[&]{switch(key->key()){case Qt::Key_Control:return Qt::ControlModifier;case Qt::Key_Shift:return Qt::ShiftModifier;case Qt::Key_Alt:return Qt::AltModifier;case Qt::Key_Meta:return Qt::MetaModifier;default:return Qt::NoModifier;}}();
 if(event->type()==QEvent::KeyPress){
  if(pressed_.contains(identity))return true;
  pressed_.insert(identity);
  if(modifier!=Qt::NoModifier)return true;
  if(key->key()==Qt::Key_unknown||key->key()==0||key->key()==Qt::Key_AltGr||key->modifiers().testFlag(Qt::GroupSwitchModifier)){
   finishRecording("Unsupported key. Choose another shortcut.");return true;
  }
  if(!chord_.isEmpty()&&chord_[0].key()!=key->key()){finishRecording("Use one key with modifiers. The draft shortcut is unchanged.");return true;}
  chord_=QKeySequence(key->keyCombination());
  recordingStatus_->setText(chord_.toString(QKeySequence::NativeText)+" — release all keys to keep it. Escape cancels.");
 }else{
  pressed_.remove(identity);
  const auto heldModifiers=key->modifiers()&(Qt::ControlModifier|Qt::ShiftModifier|Qt::AltModifier|Qt::MetaModifier)&~Qt::KeyboardModifiers(modifier);
  if(!chord_.isEmpty()&&pressed_.isEmpty()&&heldModifiers==Qt::NoModifier){
   const auto text=chord_.toString(QKeySequence::PortableText);
   input->setText(text);finishRecording(text+" recorded. Changes save automatically.");
  }
 }
 return true;
}
QJsonObject ShortcutEditor::actions(const QJsonObject& base) const {auto actions=base;for(auto it=editors_.begin();it!=editors_.end();++it){auto action=actions[it.key()].toObject();action["shortcut"]=it.value()->text();actions[it.key()]=action;}return actions;}
void ShortcutEditor::setStatus(const QJsonObject& status) {
 if(!status.contains("available"))status_->setText("Desktop bindings have not been reported. Refresh desktop support to check them.");
 else if(!status["available"].toBool())status_->setText("Global shortcuts are unavailable on this desktop. Check desktop support to restore shortcut bindings.");
 else status_->setText("Changes save automatically. Choose a shortcut to record a new combination.");
 const auto bindings=status["bindings"].toObject();
 for(const auto& action:Settings::actionNames()) {
  auto* label=bindingStates_[action];const auto presentation=bindingPresentation(status,bindings[action].toObject());label->setText(presentation.text);label->setProperty("bindingTone",presentation.tone);VisualStyle::role(label,"binding");const auto entry=bindings[action].toObject();label->setVisible(!(entry["bound"].toBool()&&actualBinding(entry["actual"])==editors_[action]->text()&&entry["error"].toString().isEmpty()));
 }
 details_->setPlainText(QString::fromUtf8(QJsonDocument(status).toJson(QJsonDocument::Indented)));
}
}
