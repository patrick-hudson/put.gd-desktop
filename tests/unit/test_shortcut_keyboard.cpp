#include "ui/ShortcutKeyboard.h"
#include <QtTest>
#include <xkbcommon/xkbcommon.h>
#include <memory>
using namespace putgd;
namespace {
struct ContextDeleter {void operator()(xkb_context* value)const{xkb_context_unref(value);}};
struct MapDeleter {void operator()(xkb_keymap* value)const{xkb_keymap_unref(value);}};
using Map=std::unique_ptr<xkb_keymap,MapDeleter>;
// Group 1 follows US, group 2 French digits; AE03 has the German section sign.
// No system layout files, so the levels are exactly these.
constexpr char fixture[]=R"XKB(xkb_keymap {
 xkb_keycodes "fixture" {
  minimum=8; maximum=255;
  <AE01>=10; <AE02>=11; <AE03>=12; <AE04>=13; <AB08>=59; <AC01>=38; <KP1>=87; <LFSH>=50;
 };
 xkb_types "fixture" {
  type "ONE_LEVEL" { modifiers=None; map[None]=Level1; };
  type "TWO_LEVEL" { modifiers=Shift; map[None]=Level1; map[Shift]=Level2; };
  type "ALPHABETIC" { modifiers=Shift+Lock; map[None]=Level1; map[Shift]=Level2;
   map[Lock]=Level2; map[Shift+Lock]=Level1; preserve[Lock]=Lock; preserve[Shift+Lock]=Lock; };
 };
 xkb_compatibility "fixture" {};
 xkb_symbols "fixture" {
  key <AE01> { type="TWO_LEVEL", symbols[Group1]=[1,exclam], symbols[Group2]=[ampersand,1] };
  key <AE02> { type="TWO_LEVEL", symbols[Group1]=[2,at], symbols[Group2]=[eacute,2] };
  key <AE03> { type="TWO_LEVEL", symbols[Group1]=[3,section], symbols[Group2]=[quotedbl,3] };
  key <AE04> { type="TWO_LEVEL", symbols[Group1]=[4,dollar], symbols[Group2]=[apostrophe,4] };
  key <AB08> { type="TWO_LEVEL", symbols[Group1]=[comma,less], symbols[Group2]=[semicolon,period] };
  key <AC01> { type="ALPHABETIC", symbols[Group1]=[a,A], symbols[Group2]=[q,Q] };
  key <KP1> { type="ONE_LEVEL", [KP_1] };
  key <LFSH> { [Shift_L] };
  modifier_map Shift {<LFSH>};
 };
};)XKB";
Map compile() {
 std::unique_ptr<xkb_context,ContextDeleter> context(xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES));
 if(!context)return {};
 return Map(xkb_keymap_new_from_string(context.get(),fixture,XKB_KEYMAP_FORMAT_TEXT_V1,XKB_KEYMAP_COMPILE_NO_FLAGS));
}
QKeyCombination chord(Qt::KeyboardModifiers modifiers,int key) {return QKeyCombination(modifiers,static_cast<Qt::Key>(key));}
}
class ShortcutKeyboardTest:public QObject {
 Q_OBJECT
private slots:
 void shiftedDigitsAndPunctuationRecordTheUnshiftedKey() {
  auto map=compile();QVERIFY(map);
  const auto shift=Qt::ControlModifier|Qt::ShiftModifier;
  // Qt reports Shift+4 as Key_Dollar: the configuration names the 4 key.
  QCOMPARE(unshiftedChord(chord(shift,Qt::Key_Dollar),map.get(),0,13),chord(shift,Qt::Key_4));
  QCOMPARE(unshiftedChord(chord(shift,Qt::Key_Exclam),map.get(),0,10),chord(shift,Qt::Key_1));
  QCOMPARE(unshiftedChord(chord(shift,Qt::Key_Less),map.get(),0,59),chord(shift,Qt::Key_Comma));
  // Latin-1 Shift symbols (German §) map back to their ASCII digit as well.
  QCOMPARE(unshiftedChord(chord(shift,0x00a7),map.get(),0,12),chord(shift,Qt::Key_3));
  // French: the 1 key's base is &, which KDE resolves to Ctrl+1 again.
  QCOMPARE(unshiftedChord(chord(shift,Qt::Key_1),map.get(),1,10),chord(shift,Qt::Key_Ampersand));
 }
 void otherKeysKeepQtsKey() {
  auto map=compile();QVERIFY(map);
  const auto shift=Qt::ControlModifier|Qt::ShiftModifier;
  // A non-ASCII base (French é) would register literally on KDE.
  QCOMPARE(unshiftedChord(chord(shift,Qt::Key_2),map.get(),1,11),chord(shift,Qt::Key_2));
  QCOMPARE(unshiftedChord(chord(shift,Qt::Key_A),map.get(),0,38),chord(shift,Qt::Key_A));
  QCOMPARE(unshiftedChord(chord(Qt::ControlModifier,Qt::Key_4),map.get(),0,13),chord(Qt::ControlModifier,Qt::Key_4));
  QCOMPARE(unshiftedChord(chord(shift|Qt::KeypadModifier,Qt::Key_1),map.get(),0,87),chord(shift|Qt::KeypadModifier,Qt::Key_1));
  // A key that disagrees with the map, an unknown code or layout, or no map.
  QCOMPARE(unshiftedChord(chord(shift,Qt::Key_At),map.get(),0,13),chord(shift,Qt::Key_At));
  QCOMPARE(unshiftedChord(chord(shift,Qt::Key_Dollar),map.get(),0,300),chord(shift,Qt::Key_Dollar));
  QCOMPARE(unshiftedChord(chord(shift,Qt::Key_Dollar),map.get(),5,13),chord(shift,Qt::Key_Dollar));
  QCOMPARE(unshiftedChord(chord(shift,Qt::Key_Dollar),nullptr,0,13),chord(shift,Qt::Key_Dollar));
 }
 void symbolDecision() {
  const auto shift=Qt::ControlModifier|Qt::ShiftModifier;
  QCOMPARE(unshiftedChord(chord(shift,Qt::Key_Dollar),'4','$'),chord(shift,Qt::Key_4));
  QCOMPARE(unshiftedChord(chord(shift,Qt::Key_Dollar),'$','$'),chord(shift,Qt::Key_Dollar));
  QCOMPARE(unshiftedChord(chord(shift,Qt::Key_Dollar),0,'$'),chord(shift,Qt::Key_Dollar));
 }
};
QTEST_GUILESS_MAIN(ShortcutKeyboardTest)
#include "test_shortcut_keyboard.moc"
