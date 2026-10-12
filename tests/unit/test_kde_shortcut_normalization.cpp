#include "platform/kde/KdeShortcutNormalization.h"
#include <QtTest>
#include <memory>
using namespace putgd;
namespace {
struct ContextDeleter {void operator()(xkb_context* value)const{xkb_context_unref(value);}};
struct MapDeleter {void operator()(xkb_keymap* value)const{xkb_keymap_unref(value);}};
using Map=std::unique_ptr<xkb_keymap,MapDeleter>;
// No system layout files/includes: these fixtures describe the actual level
// semantics independently of the production normalizer and installed layouts.
QByteArray fixture(const QByteArray& extraKeys={},const QByteArray& extraSymbols={}) {
 return QByteArray(R"XKB(xkb_keymap {
 xkb_keycodes "fixture" {
  minimum=8; maximum=255;
  <AE01>=10; <AE02>=11; <AE03>=12; <AE04>=13;
  <AC01>=38; <AD01>=24;
  <FK09>=75; <FK10>=76; <FK11>=95; <FK12>=96;
  <LFSH>=50; <LCTL>=37; <LALT>=64; <LWIN>=133; <RALT>=108; <CAPS>=66;
  <KP1>=87;
 )XKB")+extraKeys+R"XKB(
 };
 xkb_types "fixture" {
  type "ONE_LEVEL" { modifiers=None; map[None]=Level1; };
  type "TWO_LEVEL" { modifiers=Shift; map[None]=Level1; map[Shift]=Level2; };
  type "ALPHABETIC" { modifiers=Shift+Lock; map[None]=Level1; map[Shift]=Level2;
   map[Lock]=Level2; map[Shift+Lock]=Level1; preserve[Lock]=Lock; preserve[Shift+Lock]=Lock; };
  type "FOUR_LEVEL" { modifiers=Shift+Mod5; map[None]=Level1; map[Shift]=Level2;
   map[Mod5]=Level3; map[Shift+Mod5]=Level4; };
  type "CTRL_ALT_SYMBOL" { modifiers=Control+Mod1; map[None]=Level1;
   map[Control]=Level1; map[Mod1]=Level1; map[Control+Mod1]=Level2; };
 };
 xkb_compatibility "fixture" {};
 xkb_symbols "fixture" {
  name[Group1]="us"; name[Group2]="fr";
  key <AE01> { type="TWO_LEVEL", symbols[Group1]=[1,exclam], symbols[Group2]=[ampersand,1] };
  key <AE02> { type="TWO_LEVEL", symbols[Group1]=[2,at], symbols[Group2]=[eacute,2] };
  key <AE03> { type="TWO_LEVEL", symbols[Group1]=[3,numbersign], symbols[Group2]=[quotedbl,3] };
  key <AE04> { type="TWO_LEVEL", symbols[Group1]=[4,dollar], symbols[Group2]=[apostrophe,4] };
  key <AC01> { type="ALPHABETIC", symbols[Group1]=[a,A], symbols[Group2]=[q,Q] };
  key <AD01> { type="ALPHABETIC", symbols[Group1]=[q,Q], symbols[Group2]=[a,A] };
  key <FK09> { type="ONE_LEVEL", [F9] };
  key <FK10> { type="ONE_LEVEL", [F10] };
  key <FK11> { type="ONE_LEVEL", [F11] };
  key <FK12> { type="ONE_LEVEL", [F12] };
  key <LFSH> { [Shift_L] }; key <LCTL> { [Control_L] };
  key <LALT> { [Alt_L] }; key <LWIN> { [Super_L] };
  key <RALT> { [ISO_Level3_Shift] }; key <CAPS> { [Caps_Lock] };
  key <KP1> { [KP_1] };
  modifier_map Shift {<LFSH>}; modifier_map Control {<LCTL>};
  modifier_map Mod1 {<LALT>}; modifier_map Mod4 {<LWIN>};
  modifier_map Mod5 {<RALT>}; modifier_map Lock {<CAPS>};
 )XKB"+extraSymbols+R"XKB(
 };
};)XKB";
}
Map compile(const QByteArray& text) {
 std::unique_ptr<xkb_context,ContextDeleter> context(xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES));
 if(!context)return {};
 xkb_context_set_log_level(context.get(),XKB_LOG_LEVEL_CRITICAL);
 return Map(xkb_keymap_new_from_string(context.get(),text.constData(),XKB_KEYMAP_FORMAT_TEXT_V1,XKB_KEYMAP_COMPILE_NO_FLAGS));
}
QKeySequence sequence(Qt::KeyboardModifiers modifiers,Qt::Key key) {
 return QKeySequence(QKeyCombination(modifiers,key));
}
}
class KdeShortcutNormalizationTest:public QObject {
 Q_OBJECT
private slots:
 void shiftedUsDigits() {
  auto map=compile(fixture());QVERIFY(map);
  const Qt::Key shifted[]={Qt::Key_Exclam,Qt::Key_At,Qt::Key_NumberSign,Qt::Key_Dollar};
  for(int i=0;i<4;++i) {
   const auto digit=static_cast<Qt::Key>(Qt::Key_1+i);
   const auto requested=sequence(Qt::ControlModifier|Qt::ShiftModifier,digit);
   auto result=normalizeKdeShortcut(requested,map.get(),0);
   QCOMPARE(result.error,QString());QCOMPARE(result.sequence,sequence(Qt::ControlModifier,shifted[i]));
   QCOMPARE(requested,sequence(Qt::ControlModifier|Qt::ShiftModifier,digit));
   result=normalizeKdeShortcut(sequence(Qt::ControlModifier,digit),map.get(),0);
   QCOMPARE(result.error,QString());QCOMPARE(result.sequence,sequence(Qt::ControlModifier,digit));
   result=normalizeKdeShortcut(sequence(Qt::ControlModifier,shifted[i]),map.get(),0);
   QCOMPARE(result.error,QString());QCOMPARE(result.sequence,sequence(Qt::ControlModifier,shifted[i]));
  }
 }
 void latin1ShiftedPunctuation() {
  struct Case {const char* symbol;int qt;};
  // Independent fixtures for the native shifted-number symbols used by
  // British, German and Spanish layouts; no installed layout files involved.
  const Case cases[]={{"sterling",0x00a3},{"section",0x00a7},{"periodcentered",0x00b7}};
  for(const auto& item:cases) {
   auto text=fixture();
   text.replace("[3,numbersign]",QByteArray("[3,")+item.symbol+"]");
   auto map=compile(text);QVERIFY(map);
   const auto requested=sequence(Qt::ControlModifier|Qt::ShiftModifier,Qt::Key_3);
   const auto result=normalizeKdeShortcut(requested,map.get(),0);
   QCOMPARE(result.error,QString());
   QCOMPARE(result.sequence,sequence(Qt::ControlModifier,static_cast<Qt::Key>(item.qt)));
   QCOMPARE(requested,sequence(Qt::ControlModifier|Qt::ShiftModifier,Qt::Key_3));
  }
 }
 void latin1LettersKeepShiftAndQtUppercaseRules() {
  struct Case {const char* symbol;int qt;};
  // Qt uppercases Latin-1 letters only when the uppercase key remains in
  // Latin-1. Micro sign and y-diaeresis keep their original key values.
  const Case cases[]={{"adiaeresis",0x00c4},{"mu",0x00b5},{"ydiaeresis",0x00ff}};
  for(const auto& item:cases) {
   const auto symbols=QByteArray("key <LAT1> { type=\"TWO_LEVEL\", [5,")+item.symbol+"] };";
   auto map=compile(fixture("<LAT1>=200;",symbols));QVERIFY(map);
   const auto requested=sequence(Qt::ControlModifier|Qt::ShiftModifier,Qt::Key_5);
   const auto result=normalizeKdeShortcut(requested,map.get(),0);
   QCOMPARE(result.error,QString());
   QCOMPARE(result.sequence,sequence(Qt::ControlModifier|Qt::ShiftModifier,static_cast<Qt::Key>(item.qt)));
  }
 }
 void frenchNumbersAndCurrentGroup() {
  auto map=compile(fixture());QVERIFY(map);QCOMPARE(xkb_keymap_num_layouts(map.get()),2u);
  for(int i=0;i<4;++i)for(const auto modifiers:{Qt::KeyboardModifiers(Qt::ControlModifier),Qt::ControlModifier|Qt::ShiftModifier}) {
   const auto digit=static_cast<Qt::Key>(Qt::Key_1+i);
   const auto result=normalizeKdeShortcut(sequence(modifiers,digit),map.get(),1);
   QCOMPARE(result.error,QString());QCOMPARE(result.sequence,sequence(Qt::ControlModifier,digit));
  }
  const auto requested=sequence(Qt::ControlModifier|Qt::ShiftModifier,Qt::Key_1);
  QCOMPARE(normalizeKdeShortcut(requested,map.get(),0).sequence,sequence(Qt::ControlModifier,Qt::Key_Exclam));
  QCOMPARE(normalizeKdeShortcut(requested,map.get(),1).sequence,sequence(Qt::ControlModifier,Qt::Key_1));
 }
 void lettersAndFunctionKeysPreserveExplicitShift() {
  auto map=compile(fixture());QVERIFY(map);
  for(xkb_layout_index_t group=0;group<2;++group) {
   for(const auto modifiers:{Qt::KeyboardModifiers(Qt::ControlModifier),Qt::ControlModifier|Qt::ShiftModifier}) {
    auto requested=sequence(modifiers,Qt::Key_A);
    auto result=normalizeKdeShortcut(requested,map.get(),group);
    QCOMPARE(result.error,QString());QCOMPARE(result.sequence,requested);
   }
   for(int i=0;i<4;++i) {
    const auto requested=sequence(Qt::ControlModifier|Qt::ShiftModifier,static_cast<Qt::Key>(Qt::Key_F9+i));
    const auto result=normalizeKdeShortcut(requested,map.get(),group);
    QCOMPARE(result.error,QString());QCOMPARE(result.sequence,requested);
   }
  }
 }
 void nonLatinLayoutsRetainEstablishedLiteralLetterWire() {
  auto text=fixture();
  text.replace("symbols[Group2]=[q,Q]","symbols[Group2]=[Cyrillic_a,Cyrillic_A]");
  text.replace("symbols[Group2]=[a,A]","symbols[Group2]=[Cyrillic_ya,Cyrillic_YA]");
  auto map=compile(text);QVERIFY(map);
  // This proves preservation of registration, not Qt's private event translation.
  for(const auto modifiers:{Qt::KeyboardModifiers(Qt::ControlModifier),Qt::ControlModifier|Qt::ShiftModifier}) {
   const auto requested=sequence(modifiers,Qt::Key_A);
   const auto result=normalizeKdeShortcut(requested,map.get(),1);
   QCOMPARE(result.error,QString());QCOMPARE(result.sequence,requested);
  }
 }
 void emptyUnavailableAndUnsupported() {
  auto map=compile(fixture());QVERIFY(map);
  auto result=normalizeKdeShortcut({},nullptr,XKB_LAYOUT_INVALID);
  QVERIFY(result.error.isEmpty());QVERIFY(result.sequence.isEmpty());
  const auto requested=sequence(Qt::ControlModifier,Qt::Key_1);
  QCOMPARE(normalizeKdeShortcut(requested,nullptr,0).error,QString("shortcut_unavailable"));
  QCOMPARE(normalizeKdeShortcut(requested,map.get(),2).error,QString("shortcut_unavailable"));
  QCOMPARE(normalizeKdeShortcut(requested,map.get(),XKB_LAYOUT_INVALID).error,QString("shortcut_unavailable"));
  auto malformed=compile("this is not an xkb keymap");QVERIFY(!malformed);
  QCOMPARE(normalizeKdeShortcut(requested,malformed.get(),0).error,QString("shortcut_unavailable"));
  QCOMPARE(normalizeKdeShortcut(QKeySequence("Ctrl+1, Ctrl+2"),map.get(),0).error,QString("shortcut_unsupported"));
  const auto keypad=sequence(Qt::ControlModifier|Qt::KeypadModifier,Qt::Key_1);
  QCOMPARE(normalizeKdeShortcut(keypad,map.get(),0).sequence,keypad);
  const auto groupSwitch=sequence(Qt::GroupSwitchModifier,Qt::Key_1);
  QCOMPARE(normalizeKdeShortcut(groupSwitch,map.get(),0).sequence,groupSwitch);
  const auto nonLatin=sequence(Qt::ControlModifier,static_cast<Qt::Key>(0x0410));
  QCOMPARE(normalizeKdeShortcut(nonLatin,map.get(),0).sequence,nonLatin);
  const auto enter=sequence(Qt::ControlModifier,Qt::Key_Enter);
  QCOMPARE(normalizeKdeShortcut(enter,map.get(),0).sequence,enter);
  const auto keypadEnter=sequence(Qt::ControlModifier|Qt::KeypadModifier,Qt::Key_Enter);
  QCOMPARE(normalizeKdeShortcut(keypadEnter,map.get(),0).sequence,keypadEnter);
  const auto media=sequence(Qt::ControlModifier,Qt::Key_MediaPlay);
  QCOMPARE(normalizeKdeShortcut(media,map.get(),0).sequence,media);
  const auto absentLetter=sequence(Qt::ControlModifier,Qt::Key_Z);
  QCOMPARE(normalizeKdeShortcut(absentLetter,map.get(),0).sequence,absentLetter);
  QCOMPARE(normalizeKdeShortcut(sequence(Qt::ControlModifier|Qt::ShiftModifier,Qt::Key_9),map.get(),0).error,QString("shortcut_unmapped"));
 }
 void ambiguousPhysicalResolutionsFailClosed() {
  auto map=compile(fixture("<AMB>=200;","key <AMB> { type=\"TWO_LEVEL\", [1,at] };"));QVERIFY(map);
  const auto result=normalizeKdeShortcut(sequence(Qt::ControlModifier|Qt::ShiftModifier,Qt::Key_1),map.get(),0);
  QCOMPARE(result.error,QString("shortcut_ambiguous"));QVERIFY(result.sequence.isEmpty());
 }
 void minimumImplicitModifiersWin() {
  auto map=compile(fixture("<AMB>=200;","key <AMB> { type=\"TWO_LEVEL\", [ampersand,1] };"));QVERIFY(map);
  const auto result=normalizeKdeShortcut(sequence(Qt::ControlModifier,Qt::Key_1),map.get(),0);
  QCOMPARE(result.error,QString());QCOMPARE(result.sequence,sequence(Qt::ControlModifier,Qt::Key_1));
  const auto shifted=normalizeKdeShortcut(sequence(Qt::ControlModifier|Qt::ShiftModifier,Qt::Key_1),map.get(),0);
  QCOMPARE(shifted.error,QString());QCOMPARE(shifted.sequence,sequence(Qt::ControlModifier,Qt::Key_Exclam));
 }
 void unsupportedAltGrAndConsumedControlAlt() {
  auto map=compile(fixture("<ALTG>=200; <CALT>=201; <NLAT>=202;",R"(
   key <ALTG> { type="FOUR_LEVEL", [x,X,EuroSign,EuroSign] };
   key <CALT> { type="CTRL_ALT_SYMBOL", [5,6] };
   key <NLAT> { type="TWO_LEVEL", [z,Greek_ZETA] };
  )"));QVERIFY(map);
  const auto euro=sequence(Qt::ControlModifier,static_cast<Qt::Key>(0x20ac));
  QCOMPARE(normalizeKdeShortcut(euro,map.get(),0).sequence,euro);
  // '%' is reachable only with Mod5; printable normalization must not guess AltGr.
  auto altGrText=fixture("<ALTG>=200;","key <ALTG> { type=\"FOUR_LEVEL\", [x,X,percent,percent] };");
  auto altGr=compile(altGrText);QVERIFY(altGr);
  QCOMPARE(normalizeKdeShortcut(sequence(Qt::ControlModifier|Qt::ShiftModifier,Qt::Key_Percent),altGr.get(),0).error,QString("shortcut_unsupported"));
  auto result=normalizeKdeShortcut(sequence(Qt::ControlModifier|Qt::AltModifier|Qt::ShiftModifier,Qt::Key_5),map.get(),0);
  QCOMPARE(result.error,QString("shortcut_unsupported"));QVERIFY(result.sequence.isEmpty());
  const auto letter=sequence(Qt::ControlModifier|Qt::ShiftModifier,Qt::Key_Z);
  result=normalizeKdeShortcut(letter,map.get(),0);
  QCOMPARE(result.error,QString());QCOMPARE(result.sequence,letter);
 }
 void unshiftedAltGrAndUnmappedKeysKeepExistingLiteralRegistration() {
  auto map=compile(fixture("<ALTG>=200;",R"(
   replace key <AE02> { type="TWO_LEVEL", [2,EuroSign] };
   key <ALTG> { type="FOUR_LEVEL", [x,X,at,at] };
  )"));QVERIFY(map);
  const auto altGr=sequence(Qt::ControlModifier,Qt::Key_At);
  auto result=normalizeKdeShortcut(altGr,map.get(),0);QCOMPARE(result.error,QString());QCOMPARE(result.sequence,altGr);
  const auto absent=sequence(Qt::ControlModifier,Qt::Key_9);
  result=normalizeKdeShortcut(absent,map.get(),0);QCOMPARE(result.error,QString());QCOMPARE(result.sequence,absent);
 }
 void oversizedCompiledMapFailsBeforeScanning() {
  auto map=compile(fixture("<HIGH>=8192;","key <HIGH> { [F1] };"));QVERIFY(map);
  QVERIFY(xkb_keymap_max_keycode(map.get())>=8192u);
  const auto result=normalizeKdeShortcut(sequence(Qt::ControlModifier,Qt::Key_1),map.get(),0);
  QCOMPARE(result.error,QString("shortcut_unavailable"));QVERIFY(result.sequence.isEmpty());
 }
};
QTEST_GUILESS_MAIN(KdeShortcutNormalizationTest)
#include "test_kde_shortcut_normalization.moc"
