#include "KdeShortcutNormalization.h"
#include <xkbcommon/xkbcommon-keysyms.h>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <memory>

namespace putgd {
namespace {
constexpr unsigned MaxKeycodes=4096,MaxLayouts=8,MaxLevels=8,MaxCandidates=64;
constexpr size_t MaxLevelMasks=16;
struct NamedKey { int qt; xkb_keysym_t symbol; };
constexpr NamedKey NamedKeys[]={
 {Qt::Key_Escape,XKB_KEY_Escape},{Qt::Key_Tab,XKB_KEY_Tab},
 {Qt::Key_Backtab,XKB_KEY_ISO_Left_Tab},{Qt::Key_Backspace,XKB_KEY_BackSpace},
 {Qt::Key_Return,XKB_KEY_Return},{Qt::Key_Insert,XKB_KEY_Insert},
 {Qt::Key_Delete,XKB_KEY_Delete},{Qt::Key_Pause,XKB_KEY_Pause},
 {Qt::Key_Print,XKB_KEY_Print},{Qt::Key_Clear,XKB_KEY_Clear},
 {Qt::Key_Home,XKB_KEY_Home},{Qt::Key_End,XKB_KEY_End},
 {Qt::Key_Left,XKB_KEY_Left},{Qt::Key_Up,XKB_KEY_Up},
 {Qt::Key_Right,XKB_KEY_Right},{Qt::Key_Down,XKB_KEY_Down},
 {Qt::Key_PageUp,XKB_KEY_Page_Up},{Qt::Key_PageDown,XKB_KEY_Page_Down},
 {Qt::Key_Menu,XKB_KEY_Menu},{Qt::Key_Help,XKB_KEY_Help}
};
bool asciiLetter(int key) {return key>=Qt::Key_A&&key<=Qt::Key_Z;}
int qtKey(xkb_keysym_t symbol) {
 if(symbol>='a'&&symbol<='z')return static_cast<int>(symbol-'a'+'A');
 if(symbol>=0x20&&symbol<=0x7e)return static_cast<int>(symbol);
 if(symbol>=0xa0&&symbol<=0xff){
  // Qt uses uppercase Latin-1 keys, with KWin retaining the original Latin-1
  // symbol when its uppercase value leaves that range (mu and ydiaeresis).
  const auto upper=QChar(ushort(symbol)).toUpper().unicode();
  return upper<=0xff?int(upper):int(symbol);
 }
 if(symbol>=XKB_KEY_F1&&symbol<=XKB_KEY_F35)return Qt::Key_F1+symbol-XKB_KEY_F1;
 for(const auto& item:NamedKeys)if(item.symbol==symbol)return item.qt;
 // In particular, do not guess Qt's private non-Latin Control fallback or
 // collapse keypad/AltGr symbols into ordinary keys.
 return 0;
}
NormalizedKdeShortcut failure(const char* code) {return {{},QString::fromLatin1(code)};}
struct StateDeleter {void operator()(xkb_state* state)const {xkb_state_unref(state);}};
}
NormalizedKdeShortcut normalizeKdeShortcut(const QKeySequence& requested,xkb_keymap* map,xkb_layout_index_t layout) {
 if(requested.isEmpty())return {};
 if(requested.count()!=1)return failure("shortcut_unsupported");
 const auto chord=requested[0];
 const auto explicitQt=chord.keyboardModifiers();
 const int requestedKey=chord.key();
 const auto target=requestedKey>=0x20&&requestedKey<=0x7e?static_cast<xkb_keysym_t>(requestedKey):XKB_KEY_NoSymbol;
 const bool literalKey=asciiLetter(requestedKey)||(requestedKey>=0x80&&requestedKey!=Qt::Key_unknown)||
  explicitQt.testFlag(Qt::KeypadModifier)||explicitQt.testFlag(Qt::GroupSwitchModifier)||
  (requestedKey>0&&requestedKey!=Qt::Key_unknown&&!explicitQt.testFlag(Qt::ShiftModifier));
 const auto allowed=Qt::KeyboardModifiers(Qt::ShiftModifier|Qt::ControlModifier|Qt::AltModifier|Qt::MetaModifier|Qt::KeypadModifier|Qt::GroupSwitchModifier);
 if((explicitQt&~allowed)!=Qt::NoModifier||(target==XKB_KEY_NoSymbol&&!literalKey))
  return failure("shortcut_unsupported");
 if(!map)return failure("shortcut_unavailable");
 const auto layouts=xkb_keymap_num_layouts(map);
 const auto first=xkb_keymap_min_keycode(map),last=xkb_keymap_max_keycode(map);
 if(layouts==0||layouts>MaxLayouts||layout>=layouts||last<first||
    static_cast<uint64_t>(last)-first+1>MaxKeycodes||xkb_keymap_num_mods(map)>32)
  return failure("shortcut_unavailable");
 // Only explicit-Shift printable chords need consumed-Shift normalization.
 // Existing unshifted symbols (including AltGr-produced ones) remain literal.
 // This fix changes printable punctuation/digit registration only. Qt/KWin's
 // established letter (including Latin fallback), non-ASCII and special-key
 // translations remain literal; reproducing private Qt translation would widen the contract.
 if(literalKey)return {requested,{}};
 struct Modifier {Qt::KeyboardModifier qt;const char* name;xkb_mod_index_t index;};
 std::array<Modifier,4> modifiers{{
  {Qt::ShiftModifier,XKB_MOD_NAME_SHIFT,XKB_MOD_INVALID},
  {Qt::ControlModifier,XKB_MOD_NAME_CTRL,XKB_MOD_INVALID},
  {Qt::AltModifier,XKB_MOD_NAME_ALT,XKB_MOD_INVALID},
  {Qt::MetaModifier,XKB_MOD_NAME_LOGO,XKB_MOD_INVALID}
 }};
 xkb_mod_mask_t explicitMask=0,shiftMask=0;
 for(auto& modifier:modifiers) {
  modifier.index=xkb_keymap_mod_get_index(map,modifier.name);
  if(modifier.index!=XKB_MOD_INVALID&&modifier.index<32) {
   const auto bit=xkb_mod_mask_t{1}<<modifier.index;
   if(explicitQt.testFlag(modifier.qt))explicitMask|=bit;
   if(modifier.qt==Qt::ShiftModifier)shiftMask=bit;
  } else if(explicitQt.testFlag(modifier.qt))return failure("shortcut_unmapped");
 }
 std::unique_ptr<xkb_state,StateDeleter> state(xkb_state_new(map));
 if(!state)return failure("shortcut_unavailable");
 unsigned bestRank=std::numeric_limits<unsigned>::max(),candidates=0;
 int bestCombined=0;
 bool ambiguous=false,unsupportedBest=false,unsupportedTarget=false;
 for(xkb_keycode_t code=first;;++code) {
  xkb_state_update_mask(state.get(),0,0,0,0,0,layout);
  // Per-key groups wrap independently (e.g. a one-group F-key in a two-group map).
  const auto keyLayout=xkb_state_key_get_layout(state.get(),code);
  if(keyLayout!=XKB_LAYOUT_INVALID) {
   const auto levels=xkb_keymap_num_levels_for_key(map,code,keyLayout);
   if(levels>MaxLevels)return failure("shortcut_unavailable");
   for(xkb_level_index_t level=0;level<levels;++level) {
    const xkb_keysym_t* symbols=nullptr;
    const int count=xkb_keymap_key_get_syms_by_level(map,code,keyLayout,level,&symbols);
    if(count!=1)continue;
    const auto symbol=symbols[0];
    if(symbol!=target)continue;
    std::array<xkb_mod_mask_t,MaxLevelMasks> masks{};
    const auto maskCount=xkb_keymap_key_get_mods_for_level(map,code,keyLayout,level,masks.data(),masks.size());
    if(maskCount==masks.size())return failure("shortcut_unavailable");
    for(size_t index=0;index<maskCount;++index) {
     const auto implicit=masks[index];
     if((implicit&~shiftMask)!=0) {unsupportedTarget=true;continue;}
     if(++candidates>MaxCandidates)return failure("shortcut_unavailable");
     const auto effectiveMask=explicitMask|implicit;
     xkb_state_update_mask(state.get(),effectiveMask,0,0,0,0,layout);
     const int effectiveKey=qtKey(xkb_state_key_get_one_sym(state.get(),code));
     auto effectiveQt=explicitQt;
     if(implicit&shiftMask)effectiveQt|=Qt::ShiftModifier;
     Qt::KeyboardModifiers consumedQt;
     for(const auto& modifier:modifiers) {
      if(modifier.index!=XKB_MOD_INVALID&&effectiveQt.testFlag(modifier.qt)&&
         xkb_state_mod_index_is_consumed2(state.get(),code,modifier.index,XKB_CONSUMED_MODE_GTK)==1)
       consumedQt|=modifier.qt;
     }
     // Explicit Shift has to reach the key. One-level copies of a symbol, such
     // as evdev's KEY_DOLLAR or the keypad parentheses, ignore it: KWin never
     // sends their Shift chord for an ordinary keyboard (Ctrl+Shift+4 is Ctrl+$).
     const bool shiftIgnored=!consumedQt.testFlag(Qt::ShiftModifier);
     // Then resolve the configured symbol first; explicit Shift must not make
     // a higher-level copy compete with an unmodified physical key.
     const auto rank=(shiftIgnored?2u:0u)+static_cast<unsigned>(std::popcount(implicit));
     if(rank>bestRank)continue;
     if(rank<bestRank) {
      bestRank=rank;bestCombined=0;ambiguous=false;unsupportedBest=false;
     }
     // KWin preserves Shift for letter chords but consumes it for punctuation.
     if(effectiveQt.testFlag(Qt::ShiftModifier)&&consumedQt==Qt::ShiftModifier&&QChar::isLetter(uint(effectiveKey)))
      consumedQt=Qt::NoModifier;
     if(effectiveKey==0||(consumedQt&~Qt::KeyboardModifiers(Qt::ShiftModifier))!=Qt::NoModifier) {
      unsupportedBest=true;continue;
     }
     const auto wireModifiers=effectiveQt&~consumedQt;
     const int combined=QKeyCombination(wireModifiers,static_cast<Qt::Key>(effectiveKey)).toCombined();
     if(bestCombined!=0&&bestCombined!=combined)ambiguous=true;
     bestCombined=combined;
    }
   }
  }
  if(code==last)break;
 }
 if(ambiguous)return failure("shortcut_ambiguous");
 if(unsupportedBest)return failure("shortcut_unsupported");
 if(bestCombined==0)return failure(unsupportedTarget?"shortcut_unsupported":"shortcut_unmapped");
 return {QKeySequence(QKeyCombination::fromCombined(bestCombined)),{}};
}
}
