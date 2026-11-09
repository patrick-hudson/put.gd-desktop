#include "SettingsGuiFixture.h"
class SettingsGui:public SettingsGuiFixture {
 Q_OBJECT
private slots:
 void emptyShortcutRecordingPromptFitsAndSettles(){
  QWidget window;window.setFixedSize(760,900);auto* layout=new QVBoxLayout(&window);auto* editor=new ShortcutEditor;layout->addWidget(editor);
  // Hold the surrounding geometry steady: the placeholder must be a sizing input itself.
  editor->findChild<QWidget*>("shortcutPreferenceCard")->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Fixed);
  auto* status=editor->findChild<QLabel*>("shortcutRecordingStatus");status->setText("Ready to record");status->setFixedHeight(status->fontMetrics().height());auto policy=status->sizePolicy();policy.setRetainSizeWhenHidden(true);status->setSizePolicy(policy);
  auto actions=Settings::defaults()["actions"].toObject();for(const auto& name:Settings::presentationOrder()){auto action=actions[name].toObject();action["shortcut"]="";actions[name]=action;}
  editor->setActions(actions);editor->setRecordingControl([](bool,auto reply){reply({});});PreferenceRows::alignChoices(&window);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  auto* input=editor->editors().value("region");auto* record=editor->findChild<QPushButton*>("recordRegionShortcut");QVERIFY(input&&record);PUTGD_VERIFY_STEADY(input->placeholderText()=="Not set",100);const int idleWidth=input->width();QSignalSpy changed(input,&QLineEdit::textChanged);
  record->click();QCOMPARE(input->placeholderText(),QString("Press shortcut…"));QCOMPARE(changed.count(),0);
  PUTGD_TRY_VERIFY(input->contentsRect().width()>=input->fontMetrics().horizontalAdvance(input->placeholderText())+4);
  editor->cancelRecording();PUTGD_TRY_COMPARE(input->width(),idleWidth);PUTGD_VERIFY_STEADY(input->width()==idleWidth,100);
 }
 void shortcutDisplayAndClearingFollowItsValue(){
  ShortcutEditor editor;editor.setActions(Settings::defaults()["actions"].toObject());editor.show();QStringList order;for(auto* button:editor.findChildren<QPushButton*>())if(button->objectName().startsWith("record"))order<<button->objectName().mid(6).chopped(8).toLower();QCOMPARE(order,Settings::presentationOrder());
  for(const auto& action:Settings::presentationOrder()){auto* input=editor.editors().value(action);auto* clear=editor.findChild<QPushButton*>("clear"+action.left(1).toUpper()+action.mid(1)+"Shortcut");QVERIFY(input->isReadOnly());QVERIFY(!input->hasFrame());QVERIFY(clear->parentWidget()->isAncestorOf(input));QCOMPARE(clear->isEnabled(),!input->text().isEmpty());input->setText("Ctrl+1");QVERIFY(clear->isEnabled());QVERIFY(!clear->isHidden());QCOMPARE(clear->iconSize(),QSize(Ui::scaled(12),Ui::scaled(12)));QVERIFY(!clear->icon().isNull());clear->click();QVERIFY(input->text().isEmpty());QVERIFY(!clear->isEnabled());QVERIFY(clear->isHidden());QCOMPARE(input->placeholderText(),QString("Not set"));}
 }
 void shortcutRefreshIsOnlyOfferedWhileWaitingOrFailed(){
  ShortcutEditor editor;editor.setActions(Settings::defaults()["actions"].toObject());editor.show();auto* refresh=editor.refreshButton();QJsonObject bindings;const auto editors=editor.editors();for(auto it=editors.cbegin();it!=editors.cend();++it)bindings[it.key()]=QJsonObject{{"requested",it.value()->text()},{"actual",it.value()->text()},{"bound",true},{"state","bound"}};editor.setStatus({{"available",true},{"bindings",bindings}});QVERIFY(refresh->isHidden());auto entry=bindings["region"].toObject();entry["bound"]=false;entry["state"]="pending";bindings["region"]=entry;editor.setStatus({{"available",true},{"bindings",bindings}});QVERIFY(!refresh->isHidden());entry["state"]="shortcut_conflict";bindings["region"]=entry;editor.setStatus({{"available",true},{"bindings",bindings}});QVERIFY(!refresh->isHidden());
 }
 void shortcutSupportRetriesWhenItsWindowRegainsFocus(){
  ShortcutEditor editor;editor.setActions(Settings::defaults()["actions"].toObject());int retries=0;editor.setRefreshAction([&]{++retries;});editor.show();putgd::test::settle(10);auto* refresh=editor.findChild<QPushButton*>("refreshShortcutSupport");QVERIFY(refresh);QCOMPARE(refresh->property("visualRole").toString(),QString("textAction"));const int before=retries;QEvent activated(QEvent::WindowActivate);QApplication::sendEvent(&editor,&activated);QCOMPARE(retries,before+1);editor.setStatus({{"available",true},{"bindings",QJsonObject{}}});
 }

 void shortcutRecoveryIsOnePageLevelAction(){
  ShortcutEditor editor;editor.setActions(Settings::defaults()["actions"].toObject());int refreshes=0;editor.setRefreshAction([&]{++refreshes;});editor.show();
  auto title=[](const QString& name){return name.left(1).toUpper()+name.mid(1);};
  auto* refresh=editor.findChild<QPushButton*>("refreshShortcutSupport");QVERIFY(refresh);
  // Nothing reported yet: one status and one Refresh, no repeated row notes.
  editor.setStatus({});QVERIFY(refresh->isVisible());for(const auto& action:Settings::invocableActionNames())QVERIFY2(editor.findChild<QLabel*>("shortcut"+title(action)+"Status")->isHidden(),qPrintable(action));
  refresh->click();QCOMPARE(refreshes,1);
  // A problem specific to one action keeps its own line.
  QJsonObject bindings;for(const auto& action:Settings::invocableActionNames()){const auto draft=editor.editors().value(action)->text();if(!draft.isEmpty())bindings[action]=QJsonObject{{"requested",draft},{"actual",QJsonArray{draft}},{"bound",true},{"state","bound"}};}
  auto conflicted=bindings;conflicted["region"]=QJsonObject{{"requested",editor.editors().value("region")->text()},{"state","shortcut_conflict"}};
  editor.setStatus({{"available",true},{"bindings",conflicted}});auto* region=editor.findChild<QLabel*>("shortcutRegionStatus");QVERIFY(!region->isHidden());QVERIFY(region->text().startsWith("Conflict"));QVERIFY(refresh->isVisible());
  for(const auto& action:Settings::invocableActionNames())if(action!="region")QVERIFY2(editor.findChild<QLabel*>("shortcut"+title(action)+"Status")->isHidden(),qPrintable(action));
  // Everything confirmed: nothing to refresh.
  editor.setStatus({{"available",true},{"bindings",bindings}});QVERIFY(refresh->isHidden());QVERIFY(region->isHidden());
  // In Settings, the action asks the resident again.
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));QVERIFY(service.save(configuration(dir.path(),true),{}));CredentialStore secrets;UiSessionFixture fixture(service,secrets);int snapshots=0;fixture.server.setUiDiagnostics([&]{++snapshots;return fixture.operations().diagnostics();});
  MainWindow window(fixture.session);window.settings();PUTGD_TRY_VERIFY(fixture.session.ready());const int before=snapshots;window.findChild<QPushButton*>("refreshShortcutSupport")->click();PUTGD_TRY_VERIFY(snapshots>before);
 }
 void shortcutRecordsOnReleaseAndEscapePreservesDraft(){
  ShortcutEditor editor;editor.setRecordingControl([](bool,auto reply){reply({});});editor.setActions(Settings::defaults()["actions"].toObject());editor.show();
  auto* record=editor.findChild<QPushButton*>("recordRegionShortcut");QVERIFY(record);
  auto* input=editor.editors().value("region");const auto previous=input->text();
  record->click();QTest::keyPress(input,Qt::Key_Control);QTest::keyPress(input,Qt::Key_1,Qt::ControlModifier);
  QCOMPARE(input->text(),previous);
  QKeyEvent releaseOne(QEvent::KeyRelease,Qt::Key_1,Qt::ControlModifier);QApplication::sendEvent(input,&releaseOne);QCOMPARE(input->text(),previous);
  QTest::keyRelease(input,Qt::Key_Control);QCOMPARE(input->text(),QString("Ctrl+1"));
  record->click();QTest::keyPress(input,Qt::Key_Control);QTest::keyPress(input,Qt::Key_2,Qt::ControlModifier);
  QTest::keyClick(input,Qt::Key_Escape,Qt::ControlModifier);QTest::keyRelease(input,Qt::Key_2,Qt::ControlModifier);QTest::keyRelease(input,Qt::Key_Control);
  QCOMPARE(input->text(),QString("Ctrl+1"));
 }
 void shortcutSecondMouseClickCancelsRecording_data(){
  QTest::addColumn<bool>("ready");QTest::newRow("preparing")<<false;QTest::newRow("ready")<<true;
 }
 void shortcutSecondMouseClickCancelsRecording(){
  QFETCH(bool,ready);std::function<void(Error)> acknowledge;
  QList<bool> transitions;QScrollArea page;ShortcutEditor editor;
  page.setWidget(&editor);page.setWidgetResizable(true);page.resize(800,900);
  editor.setRecordingControl([&](bool active,auto reply){
   transitions.append(active);QJsonObject bindings;
   for(auto it=editor.editors().cbegin();it!=editor.editors().cend();++it)bindings[it.key()]=QJsonObject{{"requested",it.value()->text()},{"actual",it.value()->text()},{"bound",!active},{"state",active?"recording":"bound"}};
   editor.setStatus({{"available",true},{"bindings",bindings}});
   if(active&&!ready)acknowledge=reply;else reply({});
  });
  editor.setActions(Settings::defaults()["actions"].toObject());page.show();
  QVERIFY(putgd::test::waitForWindowExposed(&page));page.activateWindow();QVERIFY(putgd::test::waitForWindowActive(&page));
  auto* input=editor.editors().value("region");const auto saved=input->text();
  auto click=[&]{QTest::mouseClick(page.windowHandle(),Qt::LeftButton,Qt::NoModifier,input->mapTo(&page,input->rect().center()));};
  click();PUTGD_TRY_COMPARE(transitions,QList<bool>({true}));
  QCOMPARE(input->placeholderText(),QString("Press shortcut…"));
  click();PUTGD_TRY_COMPARE(transitions,QList<bool>({true,false}));
  if(acknowledge)acknowledge({});
  QCOMPARE(input->text(),saved);QCOMPARE(input->placeholderText(),QString("Not set"));
  QVERIFY(editor.findChild<QLabel*>("shortcutRecordingStatus")->text().startsWith("Recording canceled."));
  // Another real keycap click switches rows; Escape still restores bindings.
  click();auto* next=editor.editors().value("window");const auto nextSaved=next->text();
  const auto nextPoint=next->mapTo(&page,next->rect().center());
  QTest::mousePress(page.windowHandle(),Qt::LeftButton,Qt::NoModifier,nextPoint);putgd::test::settle(50);
  QTest::mouseRelease(page.windowHandle(),Qt::LeftButton,Qt::NoModifier,nextPoint);
  PUTGD_TRY_COMPARE(transitions,QList<bool>({true,false,true,false,true}));
  QCOMPARE(input->placeholderText(),QString("Not set"));QCOMPARE(next->placeholderText(),QString("Press shortcut…"));
  QTest::keyClick(page.windowHandle(),Qt::Key_Escape);
  PUTGD_TRY_COMPARE(transitions,QList<bool>({true,false,true,false,true,false}));QCOMPARE(next->text(),nextSaved);
  click();const auto abandoned=next->mapTo(&page,next->rect().center());
  QTest::mousePress(page.windowHandle(),Qt::LeftButton,Qt::NoModifier,abandoned);putgd::test::settle(50);
  QTest::mouseRelease(page.windowHandle(),Qt::LeftButton,Qt::NoModifier,QPoint(1,1));
  PUTGD_TRY_COMPARE(transitions,QList<bool>({true,false,true,false,true,false,true,false}));
  QCOMPARE(input->text(),saved);QCOMPARE(input->placeholderText(),QString("Not set"));
  click();QTest::mousePress(page.windowHandle(),Qt::LeftButton,Qt::NoModifier,next->mapTo(&page,next->rect().center()));
  QTest::keyClick(page.windowHandle(),Qt::Key_Escape);putgd::test::settle(50);
  QTest::mouseRelease(page.windowHandle(),Qt::LeftButton,Qt::NoModifier,next->mapTo(&page,next->rect().center()));
  PUTGD_TRY_COMPARE(transitions,QList<bool>({true,false,true,false,true,false,true,false,true,false}));
  QCOMPARE(input->text(),saved);QCOMPARE(next->text(),nextSaved);
 }
 void shortcutRowSwitchIgnoresKeysUntilRelease_data(){
  QTest::addColumn<QString>("ending");
  for(const auto* ending:{"switch","abandon","escape","deactivate"})QTest::newRow(ending)<<QString(ending);
 }
 void shortcutRowSwitchIgnoresKeysUntilRelease(){
  QFETCH(QString,ending);QList<bool> transitions;QScrollArea page;ShortcutEditor editor;
  page.setWidget(&editor);page.setWidgetResizable(true);page.resize(800,900);
  editor.setRecordingControl([&](bool active,auto reply){
   transitions.append(active);QJsonObject bindings;
   for(auto it=editor.editors().cbegin();it!=editor.editors().cend();++it)bindings[it.key()]=QJsonObject{{"requested",it.value()->text()},{"actual",it.value()->text()},{"bound",!active},{"state",active?"recording":"bound"}};
   editor.setStatus({{"available",true},{"bindings",bindings}});reply({});
  });
  editor.setChordResolver([](const QKeyEvent& key){return key.keyCombination();});
  editor.setActions(Settings::defaults()["actions"].toObject());page.show();
  QVERIFY(putgd::test::waitForWindowExposed(&page));page.activateWindow();QVERIFY(putgd::test::waitForWindowActive(&page));
  auto* input=editor.editors().value("region");auto* next=editor.editors().value("window");
  const auto saved=input->text(),nextSaved=next->text();
  QTest::mouseClick(page.windowHandle(),Qt::LeftButton,Qt::NoModifier,input->mapTo(&page,input->rect().center()));
  QCOMPARE(transitions,QList<bool>({true}));
  const auto point=next->mapTo(&page,next->rect().center());
  QTest::mousePress(page.windowHandle(),Qt::LeftButton,Qt::NoModifier,point);
  bool held=true;auto release=qScopeGuard([&]{if(held)QTest::mouseRelease(page.windowHandle(),Qt::LeftButton,Qt::NoModifier,point);});
  PUTGD_TRY_COMPARE(QApplication::focusWidget(),editor.findChild<QPushButton*>("recordWindowShortcut"));
  // A complete chord and Backspace during the hold must change neither draft.
  QTest::keyClick(page.windowHandle(),Qt::Key_8,Qt::ControlModifier|Qt::AltModifier|Qt::ShiftModifier);
  QTest::keyClick(page.windowHandle(),Qt::Key_Backspace);
  QCOMPARE(input->text(),saved);QCOMPARE(next->text(),nextSaved);QCOMPARE(transitions,QList<bool>({true}));
  if(ending=="escape")QTest::keyClick(page.windowHandle(),Qt::Key_Escape);
  if(ending=="deactivate"){QEvent event(QEvent::WindowDeactivate);QApplication::sendEvent(&page,&event);}
  QTest::mouseRelease(page.windowHandle(),Qt::LeftButton,Qt::NoModifier,ending=="abandon"?QPoint(1,1):point);held=false;
  if(ending=="switch"){
   PUTGD_TRY_COMPARE(transitions,QList<bool>({true,false,true}));
   QTest::keyClick(page.windowHandle(),Qt::Key_9,Qt::ControlModifier|Qt::AltModifier|Qt::ShiftModifier);
   QCOMPARE(next->text(),QString("Ctrl+Alt+Shift+9"));
   QCOMPARE(transitions,QList<bool>({true,false,true,false}));
  }else{
   PUTGD_TRY_COMPARE(transitions,QList<bool>({true,false}));QCOMPARE(next->text(),nextSaved);
  }
  QCOMPARE(input->text(),saved);
 }
 void shortcutFocusedKeycapAnnouncesRecordingAction(){
  ShortcutEditor editor;std::function<void(Error)> acknowledge;
  editor.setRecordingControl([&](bool active,auto reply){if(active)acknowledge=reply;else reply({});});
  editor.setActions(Settings::defaults()["actions"].toObject());editor.show();
  QVERIFY(putgd::test::waitForWindowExposed(&editor));editor.activateWindow();QVERIFY(putgd::test::waitForWindowActive(&editor));
  for(const auto& action:Settings::presentationOrder()){
   auto* button=editor.findChild<QPushButton*>("record"+action.left(1).toUpper()+action.mid(1)+"Shortcut");
   auto* accessible=QAccessible::queryAccessibleInterface(button);QVERIFY(accessible);QCOMPARE(accessible->role(),QAccessible::Button);
   const auto idleName=accessible->text(QAccessible::Name),idleDescription=accessible->text(QAccessible::Description);
   button->click();QCOMPARE(QApplication::focusWidget(),button);QVERIFY(accessible->state().focused);
   const auto recording=accessible->text(QAccessible::Name)+" "+accessible->text(QAccessible::Description);
   QVERIFY2(recording.contains("recording",Qt::CaseInsensitive),qPrintable(recording));
   QVERIFY2(recording.contains(editor.editors().value(action)->accessibleName(),Qt::CaseInsensitive),qPrintable(recording));
   acknowledge({});QCOMPARE(accessible->text(QAccessible::Name)+" "+accessible->text(QAccessible::Description),recording);
   QTest::keyClick(button,Qt::Key_Escape);
   QCOMPARE(accessible->text(QAccessible::Name),idleName);QCOMPARE(accessible->text(QAccessible::Description),idleDescription);
  }
 }
 void shortcutRecordingNormalizesEveryActionAndIgnoresRepeat(){
  ShortcutEditor editor;editor.setRecordingControl([](bool,auto reply){reply({});});editor.setActions(Settings::defaults()["actions"].toObject());editor.show();
  for(const auto& action:Settings::invocableActionNames()){
   const auto title=action.left(1).toUpper()+action.mid(1);auto* input=editor.editors().value(action);QVERIFY2(input,qPrintable(action));const auto old=input->text();
   editor.findChild<QPushButton*>("record"+title+"Shortcut")->click();
   QTest::keyPress(input,Qt::Key_Control);QTest::keyRelease(input,Qt::Key_Control);QCOMPARE(input->text(),old);
   QTest::keyPress(input,Qt::Key_Control);QTest::keyPress(input,Qt::Key_Shift,Qt::ControlModifier);QTest::keyPress(input,Qt::Key_F8,Qt::ControlModifier|Qt::ShiftModifier);
   QKeyEvent repeatedPress(QEvent::KeyPress,Qt::Key_F8,Qt::ControlModifier|Qt::ShiftModifier,{},true);QApplication::sendEvent(input,&repeatedPress);
   QKeyEvent repeatedRelease(QEvent::KeyRelease,Qt::Key_F8,Qt::ControlModifier|Qt::ShiftModifier,{},true);QApplication::sendEvent(input,&repeatedRelease);QCOMPARE(input->text(),old);
   QTest::keyRelease(input,Qt::Key_Control,Qt::ShiftModifier);QTest::keyRelease(input,Qt::Key_Shift);QCOMPARE(input->text(),old);
   QTest::keyRelease(input,Qt::Key_F8);QCOMPARE(input->text(),QString("Ctrl+Shift+F8"));
   QCOMPARE(editor.actions({})[action].toObject()["shortcut"].toString(),QString("Ctrl+Shift+F8"));
   QTest::keyClick(input,Qt::Key_Tab);QVERIFY(QApplication::focusWidget()!=input);
   editor.findChild<QPushButton*>("clear"+title+"Shortcut")->click();QVERIFY(input->text().isEmpty());
  }
 }
 void shortcutRecordingTracksPhysicalKeysAcrossModifierRelease(){
  ShortcutEditor editor;editor.setRecordingControl([](bool,auto reply){reply({});});editor.setActions(Settings::defaults()["actions"].toObject());editor.show();
  auto* record=editor.findChild<QPushButton*>("recordRegionShortcut");auto* input=editor.editors().value("region");const auto old=input->text();record->click();
  const auto send=[&](QEvent::Type type,int key,Qt::KeyboardModifiers modifiers,quint32 scan){QKeyEvent event(type,key,modifiers,scan,0,0);QApplication::sendEvent(input,&event);};
  send(QEvent::KeyPress,Qt::Key_Shift,Qt::ShiftModifier,50);send(QEvent::KeyPress,Qt::Key_Exclam,Qt::ShiftModifier,10);
  send(QEvent::KeyRelease,Qt::Key_Shift,Qt::NoModifier,50);QCOMPARE(input->text(),old);
  // X11 can report a repeated key with a new symbol when modifiers change.
  send(QEvent::KeyPress,Qt::Key_1,Qt::NoModifier,10);QCOMPARE(input->placeholderText(),QString("Press shortcut…"));
  send(QEvent::KeyRelease,Qt::Key_1,Qt::NoModifier,10);QCOMPARE(input->text(),QString("Shift+!"));QCOMPARE(input->placeholderText(),QString("Not set"));
  record->click();send(QEvent::KeyPress,Qt::Key_1,Qt::KeypadModifier,87);send(QEvent::KeyRelease,Qt::Key_1,Qt::KeypadModifier,87);
  QCOMPARE(input->text(),QKeySequence(QKeyCombination(Qt::KeypadModifier,Qt::Key_1)).toString(QKeySequence::PortableText));QCOMPARE(input->placeholderText(),QString("Not set"));
  for(const auto modifierFirst:{false,true}){
   record->click();send(QEvent::KeyPress,Qt::Key_Control,Qt::ControlModifier,37);send(QEvent::KeyPress,Qt::Key_2,Qt::ControlModifier|Qt::KeypadModifier,88);
   if(modifierFirst){send(QEvent::KeyRelease,Qt::Key_Control,Qt::NoModifier,37);send(QEvent::KeyRelease,Qt::Key_2,Qt::KeypadModifier,88);}
   else {send(QEvent::KeyRelease,Qt::Key_2,Qt::ControlModifier|Qt::KeypadModifier,88);send(QEvent::KeyRelease,Qt::Key_Control,Qt::NoModifier,37);}
   QCOMPARE(input->text(),QKeySequence(QKeyCombination(Qt::ControlModifier|Qt::KeypadModifier,Qt::Key_2)).toString(QKeySequence::PortableText));QCOMPARE(input->placeholderText(),QString("Not set"));
  }
 }
 void shortcutRecordingStoresTheUnshiftedKey(){
  // A US map for scan code 13: Shift+4 is $. KDE never sends Ctrl+Shift+$.
  ShortcutEditor editor;editor.setRecordingControl([](bool,auto reply){reply({});});
  editor.setChordResolver([](const QKeyEvent& event){return event.nativeScanCode()==13?unshiftedChord(event.keyCombination(),'4','$'):event.keyCombination();});
  editor.setActions(Settings::defaults()["actions"].toObject());editor.show();
  auto* record=editor.findChild<QPushButton*>("recordEditShortcut");auto* input=editor.editors().value("edit");QVERIFY(record);QVERIFY(input);
  const auto send=[&](QEvent::Type type,int key,Qt::KeyboardModifiers modifiers,quint32 scan){QKeyEvent event(type,key,modifiers,scan,0,0);QApplication::sendEvent(input,&event);};
  const auto shift=Qt::ControlModifier|Qt::ShiftModifier;
  record->click();send(QEvent::KeyPress,Qt::Key_Control,Qt::ControlModifier,37);send(QEvent::KeyPress,Qt::Key_Shift,shift,50);
  send(QEvent::KeyPress,Qt::Key_Dollar,shift,13);
  QVERIFY(editor.findChild<QLabel*>("shortcutRecordingStatus")->text().startsWith(QKeySequence(shift|Qt::Key_4).toString(QKeySequence::NativeText)));
  send(QEvent::KeyRelease,Qt::Key_Dollar,shift,13);send(QEvent::KeyRelease,Qt::Key_Shift,Qt::ControlModifier,50);send(QEvent::KeyRelease,Qt::Key_Control,Qt::NoModifier,37);
  QCOMPARE(input->text(),QString("Ctrl+Shift+4"));
  // Letters keep Qt's key.
  record->click();send(QEvent::KeyPress,Qt::Key_Control,Qt::ControlModifier,37);send(QEvent::KeyPress,Qt::Key_Shift,shift,50);
  send(QEvent::KeyPress,Qt::Key_E,shift,26);send(QEvent::KeyRelease,Qt::Key_E,shift,26);send(QEvent::KeyRelease,Qt::Key_Shift,Qt::ControlModifier,50);send(QEvent::KeyRelease,Qt::Key_Control,Qt::NoModifier,37);
  QCOMPARE(input->text(),QString("Ctrl+Shift+E"));
 }
 void shortcutStatusComparesTheEffectiveBinding(){
  ShortcutEditor editor;editor.setActions(Settings::defaults()["actions"].toObject());editor.show();
  auto* input=editor.editors().value("region");auto* label=editor.findChild<QLabel*>("shortcutRegionStatus");QVERIFY(label);
  input->setText("Ctrl+Shift+1");
  const auto status=[&](const QJsonObject& entry){editor.setStatus({{"available",true},{"bindings",QJsonObject{{"region",entry}}}});};
  // KDE holds Ctrl+Shift+1 as Ctrl+!, which is what was asked of it: no line.
  status({{"requested","Ctrl+Shift+1"},{"effective","Ctrl+!"},{"actual",QJsonArray{"Ctrl+!"}},{"bound",true},{"state","bound"}});
  QVERIFY(label->isHidden());
  // A desktop binding other than the effective chord is reported.
  status({{"requested","Ctrl+Shift+1"},{"effective","Ctrl+!"},{"actual",QJsonArray{"Ctrl+Shift+!"}},{"bound",true},{"state","bound"}});
  QVERIFY(!label->isHidden());QCOMPARE(label->text(),QString("Active  ·  Ctrl+Shift+!"));
  // The effective chord of an older request does not vouch for a newer draft.
  status({{"requested","Ctrl+Shift+2"},{"effective","Ctrl+@"},{"actual",QJsonArray{"Ctrl+@"}},{"bound",true},{"state","bound"}});
  QVERIFY(!label->isHidden());
  // Backends without an effective chord still compare the draft.
  status({{"requested","Ctrl+Shift+1"},{"actual",QJsonArray{"Ctrl+Shift+1"}},{"bound",true},{"state","bound"}});
  QVERIFY(label->isHidden());
 }
 void shortcutRecordingCancellationAndFailuresReleaseSuspension(){
  QList<bool> transitions;std::function<void(Error)> ready;ShortcutEditor editor;
  editor.setRecordingControl([&](bool active,auto reply){transitions.append(active);if(active)ready=reply;else reply({});});
  editor.setActions(Settings::defaults()["actions"].toObject());editor.show();putgd::test::settle(1);
  auto* input=editor.editors().value("region");auto* record=editor.findChild<QPushButton*>("recordRegionShortcut");const auto old=input->text();
  record->click();QTest::keyClick(input,Qt::Key_1,Qt::ControlModifier);QCOMPARE(input->text(),old);
  QTest::keyClick(input,Qt::Key_Escape);QCOMPARE(transitions,QList<bool>({true,false}));ready({});QCOMPARE(input->placeholderText(),QString("Not set"));
  record->click();ready({"shortcut_unavailable","Synthetic suspension failure",{}});QCOMPARE(transitions,QList<bool>({true,false,true,false}));QCOMPARE(input->text(),old);
  record->click();ready({});QTest::keyPress(input,Qt::Key_Control);QTest::mouseClick(record,Qt::LeftButton);QCOMPARE(input->placeholderText(),QString("Not set"));QCOMPARE(transitions.last(),false);
  record->click();ready({});editor.editors().value("window")->setFocus();QCOMPARE(transitions.last(),false);QCOMPARE(input->text(),old);
  record->click();ready({});QEvent deactivate(QEvent::WindowDeactivate);QApplication::sendEvent(&editor,&deactivate);QCOMPARE(transitions.last(),false);
  record->click();ready({});QKeyEvent unsupported(QEvent::KeyPress,Qt::Key_unknown,Qt::NoModifier);QApplication::sendEvent(input,&unsupported);QCOMPARE(transitions.last(),false);QVERIFY(editor.findChild<QLabel*>("shortcutRecordingStatus")->text().contains("Unsupported"));
  record->click();ready({});editor.hide();QCOMPARE(transitions.last(),false);QCOMPARE(input->text(),old);
 }
 void shortcutRecordingExportsEveryStatusThroughAccessibility(){
  std::function<void(Error)> prepared,restored;ShortcutEditor editor;
  editor.setRecordingControl([&](bool active,auto reply){if(active)prepared=std::move(reply);else restored=std::move(reply);});
  editor.setChordResolver([](const QKeyEvent& key){return key.keyCombination();});
  editor.setActions(Settings::defaults()["actions"].toObject());editor.show();putgd::test::settle(1);
  auto* record=editor.findChild<QPushButton*>("recordRegionShortcut");auto* input=editor.editors().value("region");
  auto* status=editor.findChild<QLabel*>("shortcutRecordingStatus");
  auto* accessible=QAccessible::queryAccessibleInterface(status);QVERIFY(accessible);
  auto check=[&](const QString& expected){
   QVERIFY2(status->text().contains(expected),qPrintable(status->text()));
   QCOMPARE(accessible->text(QAccessible::Name),QString("Shortcut recording status"));
   QCOMPARE(accessible->text(QAccessible::Description),status->text());
  };
  record->click();check("Preparing");prepared({});check("Press shortcut");
  QTest::keyPress(input,Qt::Key_1);check("release all keys");QTest::keyRelease(input,Qt::Key_1);check("recorded.");restored({});
  record->click();editor.cancelRecording();check("Recording canceled.");prepared({});check("Recording canceled.");
  restored({"shortcut_restore_failed","Synthetic cancel restore failure",{}});check("Couldn't restore shortcuts: Synthetic cancel restore failure");
  record->click();prepared({"shortcut_unavailable","Synthetic preparation failure",{}});check("Can't record: Synthetic preparation failure");restored({});
  record->click();prepared({});QKeyEvent unsupported(QEvent::KeyPress,Qt::Key_unknown,Qt::NoModifier);QApplication::sendEvent(input,&unsupported);check("Unsupported key");restored({});
  record->click();prepared({});QTest::keyPress(input,Qt::Key_1);QTest::keyPress(input,Qt::Key_2);check("Use one key with modifiers");restored({});
  record->click();prepared({});QTest::keyClick(input,Qt::Key_Backspace);check("Shortcut cleared.");
  restored({"shortcut_recording_interrupted","Superseded",{}});check("Shortcut cleared.");
 }
 void shortcutRecordingDistinguishesSupersededRestoreFromFailure(){
  std::function<void(Error)> restored;ShortcutEditor editor;
  editor.setRecordingControl([&](bool active,auto reply){if(active)reply({});else restored=std::move(reply);});
  editor.setActions(Settings::defaults()["actions"].toObject());editor.show();putgd::test::settle(1);
  auto* record=editor.findChild<QPushButton*>("recordRegionShortcut");auto* input=editor.editors().value("region");auto* status=editor.findChild<QLabel*>("shortcutRecordingStatus");
  record->click();QTest::keyClick(input,Qt::Key_1,Qt::ControlModifier);const auto recorded=status->text();
  restored({"shortcut_recording_interrupted","Global shortcut configuration changed.",{}});QCOMPARE(status->text(),recorded);QCOMPARE(input->text(),QString("Ctrl+1"));
  record->click();QTest::keyClick(input,Qt::Key_Escape);restored({"shortcut_restore_failed","Synthetic restore failure",{}});QVERIFY(status->text().contains("Couldn't restore shortcuts"));QCOMPARE(input->text(),QString("Ctrl+1"));
 }
 // #344: Channel offers Stable and Nightly and starts on this build's own; the
 // build's own is stored as "build", the other one explicitly.
 // Final read: with Check now shown the status folded into three ragged,
 // right-aligned lines ("This is 26.1026.1-" / "nightly.").
 void shortcutHelpIsOneDisclosure(){
  ShortcutEditor editor;editor.setActions(Settings::defaults()["actions"].toObject());editor.show();auto* toggle=editor.findChild<QToolButton*>("shortcutHelpToggle");auto* help=editor.findChild<QLabel*>("shortcutHelp");QVERIFY(toggle&&help);QVERIFY(!help->isVisible());toggle->click();QVERIFY(help->isVisible());toggle->click();QVERIFY(!help->isVisible());QCOMPARE(editor.findChildren<QToolButton*>().size(),2);
 }

 void shortcutsAndAdvancedUseContainedCards(){
  QTemporaryDir dir;SettingsService service(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(service,secrets);MainWindow window(fixture.session);window.resize(1440,960);window.navigate("settings");window.show();auto* tabs=window.findChild<QTabWidget*>("settingsTabs");
  int left=-1;for(int page=0;page<4;++page){tabs->setCurrentIndex(page);putgd::test::settle(30);auto* heading=window.findChild<QLabel*>("settingsPageHeading"+tabs->tabText(page));const int x=heading->mapTo(&window,QPoint()).x();if(left<0)left=x;QCOMPARE(x,left);}
  tabs->setCurrentIndex(2);putgd::test::settle(30);auto* card=window.findChild<QWidget*>("shortcutPreferenceCard");auto* help=window.findChild<QToolButton*>("shortcutHelpToggle");QVERIFY(card&&help);QVERIFY(card->isAncestorOf(help));for(const auto& action:Settings::presentationOrder()){const auto title=action.left(1).toUpper()+action.mid(1);QVERIFY(card->isAncestorOf(window.findChild<QPushButton*>("record"+title+"Shortcut")));}help->setChecked(true);QVERIFY(window.findChild<QLabel*>("shortcutHelp")->isVisible());help->setChecked(false);QVERIFY(window.findChild<QLabel*>("shortcutHelp")->isHidden());auto* refresh=window.findChild<QPushButton*>("refreshShortcutSupport");QCOMPARE(refresh->parentWidget(),window.findChild<QLabel*>("settingsPageHeadingShortcuts")->parentWidget());
  tabs->setCurrentIndex(3);putgd::test::settle(30);auto* toggle=window.findChild<QToolButton*>("resourceLimitsToggle");auto* history=toggle->parentWidget();QVERIFY(history->property("preferenceCard").toBool());QVERIFY(history->isAncestorOf(window.findChild<QWidget*>("resourceLimits")));toggle->setChecked(true);QVERIFY(window.findChild<QLineEdit*>("maxFileBytes")->isVisible());toggle->setChecked(false);QVERIFY(!window.findChild<QLineEdit*>("maxFileBytes")->isVisible());int cards=0;for(auto* child:window.findChild<QWidget*>("advancedPreferenceColumn")->findChildren<QWidget*>())if(child->property("preferenceCard").toBool())++cards;QCOMPARE(cards,2);
 }

};
QTEST_MAIN(SettingsGui)
#include "test_settings_shortcuts.moc"
