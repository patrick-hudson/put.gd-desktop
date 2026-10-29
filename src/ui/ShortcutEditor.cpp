#include "ShortcutEditor.h"
#include "ShortcutKeyboard.h"
#include "PreferenceRows.h"
#include "VisualStyle.h"
#include "Graphite.h"
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
using Graphite::Space;
using Graphite::space;
namespace {
QString title(const QString& action) { return action.left(1).toUpper()+action.mid(1); }
QString actualBinding(const QJsonValue& actual) {
 if(actual.isString())return actual.toString();
 QStringList bindings;
 for(const auto& value:actual.toArray())if(!value.toString().isEmpty())bindings.append(value.toString());
 return bindings.join(" / ");
}
// shown is false where the page-level status already says it (nothing reported
// yet, shortcuts unavailable on this desktop) or the field does (Disabled).
struct BindingPresentation { QString text;const char* tone;bool shown=true; };
BindingPresentation bindingPresentation(const QJsonObject& status,const QJsonObject& entry) {
 const auto state=entry["state"].toString();
 const auto error=entry["error"].isObject()?entry["error"].toObject()["code"].toString():entry["error"].toString();
 const auto actual=actualBinding(entry["actual"]);
 if(state=="recording")return {"Paused  ·  Recording a shortcut.","muted"};
 if(error=="disabled"||(state=="unbound"&&entry["requested"].toString().isEmpty()))
  return {"Disabled  ·  No global shortcut is assigned.","muted",false};
 if(status.contains("available")&&!status["available"].toBool())
  return {"Unavailable  ·  Global shortcuts are unavailable on this desktop.","warning",false};
 if(state=="shortcut_conflict"||error=="shortcut_conflict")
  return {"Conflict  ·  Choose another shortcut."+(actual.isEmpty()?QString{}:" Desktop binding: "+actual),"error"};
 if(state=="shortcut_invalid"||error=="shortcut_invalid")
  return {"Invalid shortcut  ·  Enter a single key combination.","error"};
 if(state=="shortcut_unsupported"||error=="shortcut_unsupported")
  return {"Unsupported shortcut  ·  Your desktop can't use this key combination.","warning"};
 if(state=="shortcut_unmapped"||error=="shortcut_unmapped")
  return {"Unavailable  ·  Your desktop can't map this key combination.","warning"};
 if(state=="shortcut_unavailable"||error=="shortcut_unavailable"||error=="helper_unavailable")
  return {"Unavailable  ·  Enable your desktop shortcut service or helper.","warning"};
 if(state=="pending")return {"Binding  ·  Waiting for the desktop to confirm.","muted"};
 // Bound is the runtime observation, not a promise inferred from the draft.
 if(entry["bound"].toBool()&&error.isEmpty())
  return {"Active  ·  "+(actual.isEmpty()?QString("Binding confirmed by the desktop."):actual),"active"};
 if(!error.isEmpty())return {"Not bound  ·  The desktop did not accept this shortcut.","warning"};
 if(entry.isEmpty())return {"Not reported  ·  The desktop has not reported this shortcut.","muted",false};
 return {"Not bound  ·  Choose another shortcut.","muted"};
}
}
ShortcutEditor::ShortcutEditor(QWidget* parent):QWidget(parent) {
 VisualStyle::apply(*qApp);
 auto* layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(space(Space::l));
 // The page-level state and its one recovery action; rows add only what is
 // specific to their action.
 auto* statusRow=new QHBoxLayout;statusRow->setContentsMargins(0,0,0,0);statusRow->setSpacing(space(Space::m));layout->addLayout(statusRow);
 status_=new QLabel(this);status_->setObjectName("shortcutStatus");status_->setWordWrap(true);VisualStyle::role(status_,"secondary");statusRow->addWidget(status_,1);
 refresh_=new QPushButton("Check shortcuts again",this);refresh_->setObjectName("refreshShortcutSupport");VisualStyle::role(refresh_,"textAction");refresh_->setToolTip("Ask the desktop again which shortcuts it holds.");statusRow->addWidget(refresh_,0,Qt::AlignVCenter);
 connect(refresh_,&QPushButton::clicked,this,[this]{if(refreshAction_)refreshAction_();});
 recordingStatus_=new QLabel(this);recordingStatus_->setObjectName("shortcutRecordingStatus");recordingStatus_->setAccessibleName("Shortcut recording status");recordingStatus_->setWordWrap(true);VisualStyle::role(recordingStatus_,"status");layout->addWidget(recordingStatus_);recordingStatus_->hide();
 auto* card=new Graphite::Surface("surface");card->setProperty("preferenceCard",true);card->setObjectName("shortcutPreferenceCard");auto* cardBox=new QVBoxLayout(card);cardBox->setContentsMargins(0,0,0,0);cardBox->setSpacing(0);auto* form=new QFormLayout;VisualStyle::styleForm(form);form->setHorizontalSpacing(0);form->setVerticalSpacing(0);cardBox->addLayout(form);layout->addWidget(card);
 // The command names the main window, menu and tray use.
 const QHash<QString,QString> labels{{"region","Capture region"},{"window","Capture window"},{"desktop","Capture desktop"},{"clipboard","Upload clipboard"},{"edit","Capture region & edit"},{"record","Record screen"}};
 auto* helpToggle=new QToolButton(this);helpToggle->setObjectName("shortcutHelpToggle");helpToggle->setText("How shortcuts work");Graphite::disclosure(helpToggle);helpToggle->setProperty("graphiteFlush",true);auto* help=new QLabel("Click a shortcut field, press one key with optional modifiers, then release all keys. Escape cancels; × turns the shortcut off. Changes save automatically. Your desktop confirms each binding and reports conflicts below.",this);help->setObjectName("shortcutHelp");help->setWordWrap(true);VisualStyle::role(help,"muted");help->hide();connect(helpToggle,&QToolButton::toggled,this,[help](bool open){help->setVisible(open);});
 const auto defaultActions=Settings::defaults()["actions"].toObject();
 for(const auto& action:Settings::presentationOrder()) {
  auto* field=new QWidget(this);auto* fieldLayout=new QVBoxLayout(field);fieldLayout->setContentsMargins(0,0,0,0);fieldLayout->setSpacing(space(Space::s));
  auto* row=new QHBoxLayout;fieldLayout->addLayout(row);
  auto* input=new QLineEdit(field);input->setObjectName("shortcut"+title(action));input->setAccessibleName(action+" shortcut");input->setReadOnly(true);input->setFrame(false);input->setPlaceholderText("Not set");input->setAccessibleDescription("Click a shortcut field, press one key with modifiers, then release all keys. Escape cancels. Changes save automatically.");editors_[action]=input;
  auto* record=new QPushButton(field);record->setObjectName("record"+title(action)+"Shortcut");record->setAccessibleName("Change "+labels.value(action).toLower()+" shortcut");record->setMinimumWidth(0);record->setMinimumHeight(Graphite::scaled(Graphite::Metric::control));recordButtons_[action]=record;row->addWidget(record,1);auto* keycap=new QHBoxLayout(record);keycap->setContentsMargins(space(Space::m),0,space(Space::xs),0);keycap->setSpacing(space(Space::xs));input->setParent(record);input->setAttribute(Qt::WA_TransparentForMouseEvents);input->setFocusPolicy(Qt::NoFocus);keycap->addWidget(input,1);
  auto* clear=new QPushButton(Graphite::chromeIcon("close"),{},record);clear->setIconSize(QSize(Graphite::scaled(12),Graphite::scaled(12)));clear->setObjectName("clear"+title(action)+"Shortcut");clear->setAccessibleName("Clear "+labels.value(action).toLower()+" shortcut");clear->setToolTip("Disable this shortcut.");VisualStyle::role(clear,"quiet");clear->setFixedSize(Graphite::scaled(24),Graphite::scaled(24));clear->setEnabled(false);clear->hide();connect(input,&QLineEdit::textChanged,clear,[clear](const QString& text){clear->setEnabled(!text.isEmpty());clear->setVisible(!text.isEmpty());});keycap->addWidget(clear);
  connect(record,&QPushButton::clicked,this,[this,action]{if(recordingAction_==action)cancelRecording();else startRecording(action);});
  connect(clear,&QPushButton::clicked,this,[this,input]{cancelRecording();input->clear();});
  auto* binding=new QLabel(field);binding->setObjectName("shortcut"+title(action)+"Status");binding->setAccessibleName(action+" actual shortcut status");binding->setWordWrap(true);VisualStyle::role(binding,"binding");bindingStates_[action]=binding;fieldLayout->addWidget(binding);
  const auto defaultShortcut=defaultActions[action].toObject()["shortcut"].toString();
  const auto label=labels.value(action);
  auto* wrapper=new QWidget;wrapper->setProperty("preferenceRow",true);wrapper->setProperty("preferenceResponsive",true);field->setProperty("preferenceChoiceValue",true);field->setProperty("preferenceShortcutValue",true);auto* rowBox=new QVBoxLayout(wrapper);rowBox->setContentsMargins(space(Space::l),space(Space::s),space(Space::l),space(Space::s));auto* line=new QHBoxLayout;line->setContentsMargins(0,0,0,0);rowBox->addLayout(line);line->setSpacing(space(Space::l));auto* caption=VisualStyle::settingLabel(label,{},input,this);caption->setMaximumHeight(QWIDGETSIZE_MAX);for(auto* text:caption->findChildren<QLabel*>())text->setWordWrap(true);wrapper->setProperty("preferenceLabel",QVariant::fromValue(static_cast<QObject*>(caption)));line->addWidget(caption,1,Qt::AlignTop);line->addWidget(field,0,Qt::AlignTop);form->addRow(wrapper);input->setAccessibleDescription(input->accessibleDescription()+" Default: "+(defaultShortcut.isEmpty()?QString("unassigned"):defaultShortcut)+".");
  input->setAccessibleName(labels.value(action)+" shortcut");
 }
 helpToggle->setProperty("preferenceDisclosure",true);helpToggle->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);helpToggle->setMinimumHeight(Graphite::scaled(Graphite::Metric::control)+2*space(Space::s));form->addRow(helpToggle);help->setContentsMargins(space(Space::l),space(Space::m),space(Space::l),space(Space::m));form->addRow(help);
 auto* toggle=new QToolButton(this);toggle->setObjectName("shortcutDetailsToggle");toggle->setAccessibleName("Show exact desktop shortcut diagnostics");toggle->setText("Advanced binding details");Graphite::disclosure(toggle);toggle->setProperty("graphiteFlush",true);layout->addWidget(toggle);
 details_=new QPlainTextEdit(this);details_->setObjectName("shortcutDetails");details_->setAccessibleName("Exact desktop shortcut diagnostics JSON");details_->setReadOnly(true);details_->setMinimumHeight(Graphite::scaled(120));details_->setMaximumHeight(Graphite::scaled(220));layout->addWidget(details_);details_->hide();
 connect(toggle,&QToolButton::toggled,this,[this](bool visible){details_->setVisible(visible);});
 qApp->installEventFilter(this);setStatus({});
}
ShortcutEditor::~ShortcutEditor(){cancelRecording();}
void ShortcutEditor::setActions(const QJsonObject& actions){cancelRecording();for(auto it=editors_.begin();it!=editors_.end();++it)it.value()->setText(actions[it.key()].toObject()["shortcut"].toString());}
void ShortcutEditor::startRecording(const QString& action){
 cancelRecording();recordingAction_=action;recordingReady_=false;chord_={};pressed_.clear();
 const auto generation=++recordingGeneration_;
 editors_[action]->setPlaceholderText("Press shortcut…");recordButtons_[action]->setFocusPolicy(Qt::NoFocus);
 recordingStatus_->setText("Preparing to record… Escape cancels.");recordingStatus_->show();
 editors_[action]->setFocus();qApp->installEventFilter(this);
 if(!chordResolver_)keyboard_=std::make_unique<ShortcutKeyboard>();
 if(!recordingControl_){finishRecording("Recording is unavailable. Reopen Settings from the running app.");return;}
 QPointer<ShortcutEditor> guard(this);
 recordingControl_(true,[guard,generation](Error error){
  if(!guard||generation!=guard->recordingGeneration_)return;
  if(error){guard->finishRecording("Can't record: "+error.message);return;}
  guard->recordingReady_=true;
  guard->recordingStatus_->setText("Press shortcut… Release all keys to keep it. Escape cancels.");
 });
}
void ShortcutEditor::cancelRecording(){if(!recordingAction_.isEmpty())finishRecording("Recording canceled. The draft shortcut is unchanged.");}
void ShortcutEditor::finishRecording(const QString& message){
 if(recordingAction_.isEmpty())return;
 ++recordingGeneration_;recordingReady_=false;
 editors_[recordingAction_]->setPlaceholderText("Not set");recordButtons_[recordingAction_]->setFocusPolicy(Qt::StrongFocus);recordingAction_.clear();pressed_.clear();chord_={};keyboard_.reset();
 recordingStatus_->setText(message);
 if(recordingControl_){QPointer<ShortcutEditor> guard(this);const auto generation=recordingGeneration_;recordingControl_(false,[guard,generation](Error error){
  // Saving settings can supersede this restore. The newer binding request owns
  // its result, which is reported by the actual desktop binding labels.
  if(guard&&generation==guard->recordingGeneration_&&error&&error.code!="shortcut_recording_interrupted")guard->recordingStatus_->setText("Couldn't restore shortcuts: "+error.message);
 });}
}
bool ShortcutEditor::eventFilter(QObject* object,QEvent* event){
 if(event->type()==QEvent::WindowActivate&&object==window()&&isVisible()&&recordingAction_.isEmpty()&&refresh_->isVisible()&&refreshAction_)refreshAction_();
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
  // Store the physical key's unshifted digit or punctuation (Ctrl+Shift+4,
  // not Ctrl+Shift+$), the form the defaults and every desktop backend share.
  const auto pressed=chordResolver_?chordResolver_(*key):keyboard_?keyboard_->chord(*key):key->keyCombination();
  if(!chord_.isEmpty()&&chord_[0].key()!=pressed.key()){finishRecording("Use one key with modifiers. The draft shortcut is unchanged.");return true;}
  chord_=QKeySequence(pressed);
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
 const auto bindings=status["bindings"].toObject();bool unreported=false,attention=false;
 for(const auto& action:Settings::presentationOrder()) {
  auto* label=bindingStates_[action];const auto presentation=bindingPresentation(status,bindings[action].toObject());label->setText(presentation.text);label->setProperty("bindingTone",presentation.tone);VisualStyle::role(label,"binding");const auto entry=bindings[action].toObject();
  // KDE registers Ctrl+Shift+1 as Ctrl+! (its "effective" chord). Hide the line
  // when the desktop holds what was asked of it, not only the literal draft.
  const auto draft=editors_[action]->text();
  const auto expected=entry["effective"].isString()&&entry["requested"].toString()==draft?entry["effective"].toString():draft;
  const bool confirmed=entry["bound"].toBool()&&actualBinding(entry["actual"])==expected&&entry["error"].toString().isEmpty();
  label->setVisible(presentation.shown&&!confirmed);
  unreported|=(!draft.isEmpty()&&entry.isEmpty())||entry["state"]=="pending";attention|=!confirmed&&presentation.shown&&!draft.isEmpty()&&!entry.isEmpty()&&entry["state"]!="pending"&&entry["state"]!="recording";
 }
 if(!status.contains("available"))status_->setText("Waiting for the desktop to confirm these shortcuts");
 else if(!status["available"].toBool())status_->setText("Global shortcuts are unavailable on this desktop.");
 else if(unreported)status_->setText("Waiting for the desktop to confirm these shortcuts");
 else if(attention)status_->setText("Changes save automatically. The notes below say what the desktop reported.");
 else status_->setText("Your desktop holds these shortcuts. Changes save automatically.");
 refresh_->setVisible(!status["available"].toBool()||unreported||attention);
 details_->setPlainText(QString::fromUtf8(QJsonDocument(status).toJson(QJsonDocument::Indented)));
}
}
