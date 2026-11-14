#include "Theme.h"
#include <algorithm>
#include <atomic>
#include <cmath>

namespace putgd::Theme {
namespace {
// put.gd's visual language (2026-10-26): warm charcoal surfaces, neutral primary
// actions and a restrained amber accent, with a parallel warm off-white light
// palette (not an inversion). Active and locked text meet WCAG AA (4.5:1) on bg,
// surface, surfaceRaised and selected; inactive text meets 3:1. Accent and icons
// meet 3:1 on them. Control outlines and the icons' secondary tone meet 3:1 on
// bg, surface and surfaceRaised, and the two icon tones 3:1 against each other
// so a duotone glyph keeps its inner detail. Selected glyphs have their own
// pair, held to the same rules on the selected fill; test_theme checks both
// schemes.
const Palette dark{
 QColor(0x121212),QColor(0x191919),QColor(0x202020),QColor(0x2A2A2A),QColor(0x333230),QColor(255,255,255,18),QColor(0x737373),
 QColor(0xF0F0EC),QColor(0xC4C4BF),QColor(0xAAAAA6),QColor(0x7B7B77),
 QColor(0xE8B66D),QColor(0xF0C887),QColor(0x24201A),QColor(0xE8C591),
 QColor(0x91BCA2),QColor(0xE8B66D),QColor(0xE59B93),QColor(0xEFB4AE),QColor(0x29372E),QColor(0x332C22),QColor(0x3D2A29),
 QColor(0xD6D6D2),QColor(0x777773),
 QColor(0x7C7C78),
 QColor(0xF0F0EC),QColor(0x848480),
 QColor(0x2E2E2C),QColor(0x7C7C78),QColor(0xF0524F),
 QColor(0xDEDED8),QColor(0xF0F0EC),
};
const Palette light{
 QColor(0xEDEDEA),QColor(0xFFFFFF),QColor(0xF5F5F2),QColor(0xFFFFFF),QColor(0xECEBE6),QColor(0,0,0,20),QColor(0x8E8E87),
 QColor(0x292926),QColor(0x484844),QColor(0x62625B),QColor(0x878780),
 QColor(0x8F5B20),QColor(0x774816),QColor(0xFFFFFF),QColor(0x80521D),
 QColor(0x386A4B),QColor(0x8F5B20),QColor(0xA44139),QColor(0x8F322B),QColor(0xDFEDE3),QColor(0xEEE5D8),QColor(0xF4E0DE),
 QColor(0x383834),QColor(0x86867E),
 QColor(0x82827B),
 QColor(0x292926),QColor(0x787874),
 QColor(0xF0EFEA),QColor(0x82827B),QColor(0xC8302B),
 QColor(0x292926),QColor(0x3A3A37),
};
struct Token {const char* name;QColor Palette::* member;};
constexpr Token table[]{
 {"well",&Palette::well},{"bg",&Palette::bg},{"surface",&Palette::surface},{"surfaceRaised",&Palette::surfaceRaised},{"selected",&Palette::selected},
 {"border",&Palette::border},{"borderStrong",&Palette::borderStrong},{"text",&Palette::text},{"textSecondary",&Palette::textSecondary},
 {"textMuted",&Palette::textMuted},{"textDisabled",&Palette::textDisabled},{"accent",&Palette::accent},{"accentHover",&Palette::accentHover},
 {"onAccent",&Palette::onAccent},{"link",&Palette::link},{"success",&Palette::success},{"warning",&Palette::warning},{"danger",&Palette::danger},
 {"dangerHover",&Palette::dangerHover},{"successSurface",&Palette::successSurface},{"warningSurface",&Palette::warningSurface},
 {"dangerSurface",&Palette::dangerSurface},{"icon",&Palette::icon},{"iconSecondary",&Palette::iconSecondary},
 {"primary",&Palette::primary},{"primaryHover",&Palette::primaryHover},
 {"lead",&Palette::lead},{"leadOutline",&Palette::leadOutline},{"live",&Palette::live},
 {"controlOutline",&Palette::controlOutline},{"iconSelected",&Palette::iconSelected},{"iconSecondarySelected",&Palette::iconSecondarySelected},
};
QColor tokenColor(const QColor& color,const Palette& values){
 if(color.alpha()==255)return color;
 const auto a=color.alphaF();return QColor::fromRgbF(color.redF()*a+values.bg.redF()*(1-a),color.greenF()*a+values.bg.greenF()*(1-a),color.blueF()*a+values.bg.blueF()*(1-a));
}
std::atomic<Scheme> active{Scheme::Dark};
double channel(double c){return c<=0.03928?c/12.92:std::pow((c+0.055)/1.055,2.4);}
double luminance(const QColor& c){return 0.2126*channel(c.redF())+0.7152*channel(c.greenF())+0.0722*channel(c.blueF());}
}
const Palette& palette(Scheme scheme){return scheme==Scheme::Light?light:dark;}
const Palette& current(){return palette(active.load());}
Scheme scheme(){return active.load();}
QColor preferencePage(){return scheme()==Scheme::Light?QColor(0xF5F5F2):current().bg;}
QColor preferenceCard(){return scheme()==Scheme::Light?QColor(0xFFFFFF):current().surface;}
QColor preferenceBorder(){return scheme()==Scheme::Light?QColor(0xE2E1DB):current().border;}
QColor preferenceSeparator(){return scheme()==Scheme::Light?QColor(0xECEBE6):current().border;}
QString name(Scheme scheme){return scheme==Scheme::Light?QStringLiteral("light"):QStringLiteral("dark");}
QJsonObject tokens(Scheme scheme){
 QJsonObject result;const auto& values=palette(scheme);
 for(const auto& token:table)result[token.name]=tokenColor(values.*token.member,values).name(QColor::HexRgb);
 return result;
}
QString stylesheet(QString qss,Scheme scheme){
 const auto& values=palette(scheme);
 for(const auto& token:table)qss.replace(QLatin1Char('%')+QLatin1String(token.name)+QLatin1Char('%'),tokenColor(values.*token.member,values).name(QColor::HexRgb));
 return qss;
}
const TrayTints& trayTints(bool lightPanel){
 const auto adjusted=[](Scheme scheme){
  const auto& p=palette(scheme);const bool lightPanel=scheme==Scheme::Light;
  const QColor panels[]= {lightPanel?QColor(0xffffff):QColor(0x141414),lightPanel?QColor(0xdedee2):QColor(0x3b3b3f)};
  const auto readable=[&](QColor color){for(int step=0;step<100&&(contrast(color,panels[0])<3.0||contrast(color,panels[1])<3.0);++step)color=lightPanel?color.darker(105):color.lighter(105);return color;};
  return TrayTints{readable(p.icon),readable(p.success),readable(p.accent),readable(p.danger),readable(p.live)};
 };
 static const auto dark=adjusted(Scheme::Dark),light=adjusted(Scheme::Light);
 return lightPanel?light:dark;
}
bool setScheme(Scheme scheme){
 if(active.exchange(scheme)==scheme)return false;
 emit Notifier::instance().changed();
 return true;
}
double contrast(const QColor& a,const QColor& b){
 const auto x=luminance(a),y=luminance(b);
 return (std::max(x,y)+0.05)/(std::min(x,y)+0.05);
}
Notifier& Notifier::instance(){static Notifier notifier;return notifier;}
}
