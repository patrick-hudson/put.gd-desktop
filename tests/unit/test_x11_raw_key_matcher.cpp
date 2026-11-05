// XInput2 raw-key matching against installed shortcut grabs (#341). Pure logic:
// keycodes and modifier bits are synthetic, no X server is involved.
#include "platform/x11/X11RawKeyMatcher.h"
#include <QtTest>
using namespace putgd;
namespace {
constexpr uint8_t shiftL=50,shiftR=62,controlL=37,altL=64,caps=66,num=77,one=10,three=12,a=38;
constexpr uint16_t shift=1,lock=2,control=4,mod1=8,mod2=16;
X11RawKeyMatcher matcher(){
 std::array<uint16_t,256> map{};map[shiftL]=shift;map[shiftR]=shift;map[controlL]=control;map[altL]=mod1;map[caps]=lock;map[num]=mod2;
 X11RawKeyMatcher result;result.setModifierMap(map);result.setLocks(lock|mod2);
 // The passive path installs one grab per lock combination; the matcher
 // must fire once for the plain one whatever locks are on.
 result.setBindings({{"region",one,control|shift},{"region",one,control|shift|lock},{"region",one,control|shift|mod2},{"region",one,control|shift|lock|mod2},{"desktop",three,control|shift},{"edit",a,0}});
 return result;
}
}
class X11RawKeyMatcherTest final:public QObject {
 Q_OBJECT
private slots:
 void chordMatchesItsBindingOnly(){
  auto keys=matcher();
  keys.press(controlL);keys.press(shiftL);
  QCOMPARE(keys.press(one).value_or(QString()),QString("region"));keys.release(one);
  QCOMPARE(keys.press(three).value_or(QString()),QString("desktop"));keys.release(three);
  keys.release(shiftL);
  // Control alone is not the Ctrl+Shift binding.
  QVERIFY(!keys.press(one));keys.release(one);
  keys.release(controlL);
  QCOMPARE(keys.press(a).value_or(QString()),QString("edit"));
 }
 void extraModifierDoesNotMatch(){
  auto keys=matcher();
  keys.press(controlL);keys.press(shiftL);keys.press(altL);
  QVERIFY(!keys.press(one));
 }
 void rightHandModifiersCount(){
  auto keys=matcher();
  keys.press(controlL);keys.press(shiftR);
  QCOMPARE(keys.press(one).value_or(QString()),QString("region"));
 }
 void heldLockKeysAreIgnored(){
  auto keys=matcher();
  keys.press(caps);keys.press(num);keys.press(controlL);keys.press(shiftL);
  QCOMPARE(keys.held(),uint16_t(control|shift));
  QCOMPARE(keys.press(one).value_or(QString()),QString("region"));
 }
 void autorepeatFiresOnce(){
  auto keys=matcher();
  keys.press(controlL);keys.press(shiftL);
  QCOMPARE(keys.press(one).value_or(QString()),QString("region"));
  // A key already down repeats: raw repeats must not capture again.
  QVERIFY(!keys.press(one));QVERIFY(!keys.press(one));
  keys.release(one);
  QCOMPARE(keys.press(one).value_or(QString()),QString("region"));
 }
 void releasedModifiersNoLongerApply(){
  auto keys=matcher();
  keys.press(controlL);keys.press(shiftL);keys.release(shiftL);keys.release(controlL);
  QCOMPARE(keys.held(),uint16_t(0));
  QVERIFY(!keys.press(one));
 }
 void noBindingsNeverMatch(){
  auto keys=matcher();keys.setBindings({});
  keys.press(controlL);keys.press(shiftL);
  QVERIFY(!keys.press(one));
 }
 void keymapSyncRestoresMissedReleases(){
  // A passive activation hides the trigger key's raw release; the keymap
  // read afterwards must clear it, or every later press looks like a repeat.
  auto keys=matcher();
  keys.press(controlL);keys.press(shiftL);QCOMPARE(keys.press(one).value_or(QString()),QString("region"));
  uint8_t bitmap[32]={};bitmap[controlL/8]|=uint8_t(1<<(controlL%8));bitmap[shiftL/8]|=uint8_t(1<<(shiftL%8));
  keys.sync(bitmap);
  QCOMPARE(keys.held(),uint16_t(control|shift));
  QCOMPARE(keys.press(one).value_or(QString()),QString("region"));
 }
 void resetForgetsHeldKeys(){
  auto keys=matcher();
  keys.press(controlL);keys.press(shiftL);keys.press(one);keys.reset();
  QCOMPARE(keys.held(),uint16_t(0));
  keys.press(controlL);keys.press(shiftL);
  QCOMPARE(keys.press(one).value_or(QString()),QString("region"));
 }
};
QTEST_APPLESS_MAIN(X11RawKeyMatcherTest)
#include "test_x11_raw_key_matcher.moc"
