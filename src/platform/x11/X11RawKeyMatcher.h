#pragma once
#include <QList>
#include <QString>
#include <array>
#include <cstdint>
#include <optional>
namespace putgd {
// Matches XInput2 raw key events against the installed passive grabs (#341).
// Raw events carry no modifier state, so the matcher keeps the pressed keycodes
// itself and derives the core modifier mask from the server's modifier map.
// A keycode already held is an autorepeat and never matches again.
class X11RawKeyMatcher final {
public:
 struct Binding {QString action;uint8_t code=0;uint16_t modifiers=0;};
 // Per-keycode core modifier bits, from GetModifierMapping.
 void setModifierMap(const std::array<uint16_t,256>& map){modifiers_=map;}
 // Lock modifiers (Caps, Num, Scroll) that a held lock key must not add.
 void setLocks(uint16_t locks){locks_=locks;}
 void setBindings(QList<Binding> bindings){bindings_=std::move(bindings);}
 std::optional<QString> press(uint8_t code){
  if(pressed_[code]){return std::nullopt;}
  const auto state=held();pressed_[code]=true;
  for(const auto& binding:bindings_)if(binding.code==code&&uint16_t(binding.modifiers&~locks_)==state&&!(binding.modifiers&locks_))return binding.action;
  return std::nullopt;
 }
 void release(uint8_t code){pressed_[code]=false;}
 void reset(){pressed_.fill(false);}
 // Replaces the held keys with the server's QueryKeymap bitmap (32 bytes).
 // The server sends a client no raw events while that client's own core
 // grab is active, so a release during a passive activation is resynced here.
 void sync(const uint8_t* keys){for(int code=0;code<256;++code)pressed_[code]=keys[code/8]&(1<<(code%8));}
 // The core modifier mask the held keys produce, without lock modifiers.
 uint16_t held()const{uint16_t state=0;for(int code=0;code<256;++code)if(pressed_[code])state|=modifiers_[code];return uint16_t(state&~locks_);}
private:
 std::array<uint16_t,256> modifiers_{};
 std::array<bool,256> pressed_{};
 QList<Binding> bindings_;
 uint16_t locks_=0;
};
}
