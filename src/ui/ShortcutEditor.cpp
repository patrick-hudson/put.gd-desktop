#include "ShortcutEditor.h"
#include "ShortcutKeyboard.h"
#include "PreferenceRows.h"
#include "Ui.h"
#include "core/Settings.h"
#include <QApplication>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>

namespace putgd {
using Ui::Space;
using Ui::space;
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
 Ui::apply(*qApp);
 auto* layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(space(Space::l));
 // The page-level state and its one recovery action; rows add only what is
 // specific to their action.
 auto* statusRow=new QHBoxLayout;statusRow->setContentsMargins(0,0,0,0);statusRow->setSpacing(space(Space::m));layout->addLayout(statusRow);
 status_=new QLabel(this);status_->setObjectName("shortcutStatus");status_->setWordWrap(true);Ui::role(status_,"secondary");statusRow->addWidget(status_,1);
 refresh_=new QPushButton("Check shortcuts again",this);refresh_->setObjectName("refreshShortcutSupport");Ui::role(refresh_,"textAction");refresh_->setToolTip("Ask the desktop again which shortcuts it holds.");statusRow->addWidget(refresh_,0,Qt::AlignVCenter);
 connect(refresh_,&QPushButton::clicked,this,[this]{if(refreshAction_)refreshAction_();});
 recordingStatus_=new QLabel(this);recordingStatus_->setObjectName("shortcutRecordingStatus");recordingStatus_->setAccessibleName("Shortcut recording status");recordingStatus_->setWordWrap(true);Ui::role(recordingStatus_,"status");layout->addWidget(recordingStatus_);recordingStatus_->hide();
 auto* card=new Ui::Surface("surface");card->setProperty("preferenceCard",true);card->setObjectName("shortcutPreferenceCard");auto* cardBox=new QVBoxLayout(card);cardBox->setContentsMargins(0,0,0,0);cardBox->setSpacing(0);auto* form=new QFormLayout;Ui::styleForm(form);form->setHorizontalSpacing(0);form->setVerticalSpacing(0);cardBox->addLayout(form);layout->addWidget(card);
 // The command names the main window, menu and tray use.
 const QHash<QString,QString> labels{{"region","Capture region"},{"window","Capture window"},{"desktop","Capture desktop"},{"clipboard","Upload clipboard"},{"edit","Capture region & edit"},{"record","Record screen"}};
 auto* helpToggle=new QToolButton(this);helpToggle->setObjectName("shortcutHelpToggle");helpToggle->setText("How shortcuts work");Ui::disclosure(helpToggle);helpToggle->setProperty("uiFlush",true);auto* help=new QLabel("Click a shortcut field, press one key with optional modifiers, then release all keys. Escape cancels; × turns the shortcut off. Changes save automatically. Your desktop confirms each binding and reports conflicts below.",this);help->setObjectName("shortcutHelp");help->setWordWrap(true);Ui::role(help,"muted");help->hide();connect(helpToggle,&QToolButton::toggled,this,[help](bool open){help->setVisible(open);});
 const auto defaultActions=Settings::defaults()["actions"].toObject();
 for(const auto& action:Settings::presentationOrder()) {
  auto* field=new QWidget(this);auto* fieldLayout=new QVBoxLayout(field);fieldLayout->setContentsMargins(0,0,0,0);fieldLayout->setSpacing(space(Space::s));
  auto* row=new QHBoxLayout;fieldLayout->addLayout(row);
  auto* input=new QLineEdit(field);input->setObjectName("shortcut"+title(action));input->setAccessibleName(action+" shortcut");input->setReadOnly(true);input->setFrame(false);input->setPlaceholderText("Not set");input->setAccessibleDescription("Click a shortcut field, press one key with modifiers, then release all keys. Escape cancels. Changes save automatically.");editors_[action]=input;
  auto* record=new QPushButton(field);record->setObjectName("record"+title(action)+"Shortcut");record->setAccessibleName("Change "+labels.value(action).toLower()+" shortcut");record->setMinimumWidth(0);record->setMinimumHeight(Ui::scaled(Ui::Metric::control));recordButtons_[action]=record;row->addWidget(record,1);auto* keycap=new QHBoxLayout(record);keycap->setContentsMargins(space(Space::m),0,space(Space::xs),0);keycap->setSpacing(space(Space::xs));input->setParent(record);input->setAttribute(Qt::WA_TransparentForMouseEvents);input->setFocusPolicy(Qt::NoFocus);keycap->addWidget(input,1);
  auto* clear=new QPushButton(Ui::chromeIcon("close"),{},record);clear->setIconSize(QSize(Ui::scaled(12),Ui::scaled(12)));clear->setObjectName("clear"+title(action)+"Shortcut");clear->setAccessibleName("Clear "+labels.value(action).toLower()+" shortcut");clear->setToolTip("Disable this shortcut.");Ui::role(clear,"quiet");clear->setFixedSize(Ui::scaled(24),Ui::scaled(24));clear->setEnabled(false);clear->hide();connect(input,&QLineEdit::textChanged,clear,[clear](const QString& text){clear->setEnabled(!text.isEmpty());clear->setVisible(!text.isEmpty());});keycap->addWidget(clear);
  connect(record,&QPushButton::clicked,this,[this,action]{if(recordingAction_==action)cancelRecording();else startRecording(action);});
  connect(clear,&QPushButton::clicked,this,[this,input]{cancelRecording();input->clear();});
  auto* binding=new QLabel(field);binding->setObjectName("shortcut"+title(action)+"Status");binding->setAccessibleName(action+" actual shortcut status");binding->setWordWrap(true);Ui::role(binding,"binding");bindingStates_[action]=binding;fieldLayout->addWidget(binding);
  const auto defaultShortcut=defaultActions[action].toObject()["shortcut"].toString();
  const auto label=labels.value(action);
  auto* wrapper=new QWidget;wrapper->setProperty("preferenceRow",true);wrapper->setProperty("preferenceResponsive",true);field->setProperty("preferenceChoiceValue",true);field->setProperty("preferenceShortcutValue",true);auto* rowBox=new QVBoxLayout(wrapper);rowBox->setContentsMargins(space(Space::l),space(Space::s),space(Space::l),space(Space::s));auto* line=new QHBoxLayout;line->setContentsMargins(0,0,0,0);rowBox->addLayout(line);line->setSpacing(space(Space::l));auto* caption=Ui::settingLabel(label,{},input,this);caption->setMaximumHeight(QWIDGETSIZE_MAX);for(auto* text:caption->findChildren<QLabel*>())text->setWordWrap(true);wrapper->setProperty("preferenceLabel",QVariant::fromValue(static_cast<QObject*>(caption)));line->addWidget(caption,1,Qt::AlignTop);line->addWidget(field,0,Qt::AlignTop);form->addRow(wrapper);input->setAccessibleDescription(input->accessibleDescription()+" Default: "+(defaultShortcut.isEmpty()?QString("unassigned"):defaultShortcut)+".");
  input->setAccessibleName(labels.value(action)+" shortcut");
 }
 helpToggle->setProperty("preferenceDisclosure",true);helpToggle->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);helpToggle->setMinimumHeight(Ui::scaled(Ui::Metric::control)+2*space(Space::s));form->addRow(helpToggle);help->setContentsMargins(space(Space::l),space(Space::m),space(Space::l),space(Space::m));form->addRow(help);
 auto* toggle=new QToolButton(this);toggle->setObjectName("shortcutDetailsToggle");toggle->setAccessibleName("Show exact desktop shortcut diagnostics");toggle->setText("Advanced binding details");Ui::disclosure(toggle);toggle->setProperty("uiFlush",true);layout->addWidget(toggle);
 details_=new QPlainTextEdit(this);details_->setObjectName("shortcutDetails");details_->setAccessibleName("Exact desktop shortcut diagnostics JSON");details_->setReadOnly(true);details_->setMinimumHeight(Ui::scaled(120));details_->setMaximumHeight(Ui::scaled(220));layout->addWidget(details_);details_->hide();
 connect(toggle,&QToolButton::toggled,this,[this](bool visible){details_->setVisible(visible);});
 qApp->installEventFilter(this);setStatus({});
}
ShortcutEditor::~ShortcutEditor(){cancelRecording();}
void ShortcutEditor::setActions(const QJsonObject& actions){cancelRecording();for(auto it=editors_.begin();it!=editors_.end();++it)it.value()->setText(actions[it.key()].toObject()["shortcut"].toString());}
void ShortcutEditor::startRecording(const QString& action){
 cancelRecording();recordingAction_=action;recordingReady_=false;chord_={};pressed_.clear();
 const auto generation=++recordingGeneration_;
 editors_[action]->setPlaceholderText("Press shortcut…");if(promptChanged_)promptChanged_();
 recordingStatus_->setText("Preparing to record… Escape cancels.");recordingStatus_->show();
 // Keep focus on the clickable keycap. Focusing its mouse-transparent child
 // while making the button unfocusable lets a real click focus the surrounding
 // scroll area: FocusOut cancels, then clicked starts recording again.
 recordButtons_[action]->setAccessibleDescription("Recording "+editors_[action]->accessibleName()+". Press one key with optional modifiers, then release all keys. Escape cancels.");
 recordButtons_[action]->setFocus();qApp->installEventFilter(this);
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
 // Escape or deactivation also abandons a keycap switch held under the mouse.
 if(pendingMouseFocusCancel_)if(auto* button=qobject_cast<QPushButton*>(QApplication::focusWidget());recordButtons_.values().contains(button)){canceledMouseRelease_=button;button->setDown(false);}
 ++recordingGeneration_;recordingReady_=false;pendingMouseFocusCancel_=false;
 recordButtons_[recordingAction_]->setAccessibleDescription({});
 editors_[recordingAction_]->setPlaceholderText("Not set");recordingAction_.clear();pressed_.clear();chord_={};keyboard_.reset();if(promptChanged_)promptChanged_();
 recordingStatus_->setText(message);
 if(recordingControl_){QPointer<ShortcutEditor> guard(this);const auto generation=recordingGeneration_;recordingControl_(false,[guard,generation](Error error){
  // Saving settings can supersede this restore. The newer binding request owns
  // its result, which is reported by the actual desktop binding labels.
  if(guard&&generation==guard->recordingGeneration_&&error&&error.code!="shortcut_recording_interrupted")guard->recordingStatus_->setText("Couldn't restore shortcuts: "+error.message);
 });}
}
bool ShortcutEditor::eventFilter(QObject* object,QEvent* event){
 if((event->type()==QEvent::MouseButtonPress||event->type()==QEvent::MouseButtonDblClick)&&static_cast<QMouseEvent*>(event)->button()==Qt::LeftButton)canceledMouseRelease_.clear();
 if(canceledMouseRelease_&&object==canceledMouseRelease_.data()&&event->type()==QEvent::MouseButtonRelease&&static_cast<QMouseEvent*>(event)->button()==Qt::LeftButton){canceledMouseRelease_->setDown(false);canceledMouseRelease_.clear();}
 if(event->type()==QEvent::WindowActivate&&object==window()&&isVisible()&&recordingAction_.isEmpty()&&refresh_->isVisible()&&refreshAction_)refreshAction_();
 if(recordingAction_.isEmpty())return false;
 auto* input=editors_[recordingAction_];
 if(object==recordButtons_[recordingAction_]&&event->type()==QEvent::FocusOut){
  // Another keycap gets mouse focus before its press is delivered. Restoring
  // bindings here hides the Paused labels and can move that keycap away from
  // the release point. Let its completed click switch the recorder first.
  auto* focus=qobject_cast<QPushButton*>(QApplication::focusWidget());
  if(focus&&recordButtons_.values().contains(focus)&&QApplication::mouseButtons().testFlag(Qt::LeftButton)){pendingMouseFocusCancel_=true;return false;}
  cancelRecording();return false;
 }
 if(pendingMouseFocusCancel_&&event->type()==QEvent::MouseButtonRelease&&static_cast<QMouseEvent*>(event)->button()==Qt::LeftButton){
  const auto generation=recordingGeneration_;QPointer<ShortcutEditor> guard(this);
  // An abandoned click still cancels on focus loss. A completed switch has
  // already advanced the generation by the time this queued check runs.
  QTimer::singleShot(0,this,[guard,generation]{if(guard&&generation==guard->recordingGeneration_)guard->cancelRecording();});
 }
 if((object==this&&event->type()==QEvent::Hide)||(object==window()&&(event->type()==QEvent::WindowDeactivate||event->type()==QEvent::Close||event->type()==QEvent::Hide))||event->type()==QEvent::ApplicationDeactivate){cancelRecording();return false;}
 if(event->type()!=QEvent::ShortcutOverride&&event->type()!=QEvent::KeyPress&&event->type()!=QEvent::KeyRelease)return false;
 auto* key=static_cast<QKeyEvent*>(event);event->accept();
 if(event->type()==QEvent::ShortcutOverride)return true;
 if(key->key()==Qt::Key_Escape){cancelRecording();return true;}
 // Focus already belongs to the pressed keycap. Keep the old recorder paused
 // through release (and its queued abandoned-click check), without accepting
 // a chord or Backspace for the previous action. The click owns the handoff.
 if(pendingMouseFocusCancel_)return true;
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
  auto* label=bindingStates_[action];const auto presentation=bindingPresentation(status,bindings[action].toObject());label->setText(presentation.text);label->setProperty("bindingTone",presentation.tone);Ui::role(label,"binding");const auto entry=bindings[action].toObject();
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
