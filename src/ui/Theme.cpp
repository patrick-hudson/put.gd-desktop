#include "Theme.h"
#include <algorithm>
#include <atomic>
#include <cmath>

namespace putgd::Theme {
namespace {
// The Graphite language (2026-10-26): warm graphite surfaces, neutral primary
// actions and a restrained amber accent, with a parallel warm off-white light
// palette (not an inversion). Every text colour meets WCAG AA (4.5:1) on bg,
// surface, surfaceRaised and selected, and the accent (focus rings) and icons
// meet 3:1 on them. Control outlines and the icons' secondary tone meet 3:1 on
// bg, surface and surfaceRaised, and the two icon tones 3:1 against each other
// so a duotone glyph keeps its inner detail. Selected glyphs have their own
// pair, held to the same rules on the selected fill; test_theme checks both
// schemes.
const Palette dark{
 QColor("#121212"),QColor("#191919"),QColor("#202020"),QColor("#2A2A2A"),QColor("#333230"),QColor(255,255,255,18),QColor("#515151"),
 QColor("#F0F0EC"),QColor("#D2D2CF"),QColor("#AAAAA6"),QColor("#9F9F9B"),
 QColor("#E8B66D"),QColor("#F0C887"),QColor("#24201A"),QColor("#E8C591"),
 QColor("#91BCA2"),QColor("#E8B66D"),QColor("#E59B93"),QColor("#EFB4AE"),QColor("#29372E"),QColor("#332C22"),QColor("#3D2A29"),
 QColor("#D6D6D2"),QColor("#777773"),
 QColor("#7C7C78"),
 QColor("#F0F0EC"),QColor("#848480"),
 QColor("#2E2E2C"),QColor("#4A4A46"),QColor("#F0524F"),
};
const Palette light{
 QColor("#EDEDEA"),QColor("#FFFFFF"),QColor("#F5F5F2"),QColor("#FFFFFF"),QColor("#ECEBE6"),QColor(0,0,0,20),QColor("#AAAAA3"),
 QColor("#292926"),QColor("#484844"),QColor("#62625B"),QColor("#62625B"),
 QColor("#8F5B20"),QColor("#774816"),QColor("#FFFFFF"),QColor("#80521D"),
 QColor("#386A4B"),QColor("#8F5B20"),QColor("#A44139"),QColor("#8F322B"),QColor("#DFEDE3"),QColor("#EEE5D8"),QColor("#F4E0DE"),
 QColor("#383834"),QColor("#86867E"),
 QColor("#82827B"),
 QColor("#292926"),QColor("#787874"),
 QColor("#F0EFEA"),QColor("#C9C9C3"),QColor("#C8302B"),
};
struct Token {const char* name;QColor Palette::* member;};
constexpr Token table[]{
 {"well",&Palette::well},{"bg",&Palette::bg},{"surface",&Palette::surface},{"surfaceRaised",&Palette::surfaceRaised},{"selected",&Palette::selected},
 {"border",&Palette::border},{"borderStrong",&Palette::borderStrong},{"text",&Palette::text},{"textSecondary",&Palette::textSecondary},
 {"textMuted",&Palette::textMuted},{"textDisabled",&Palette::textDisabled},{"accent",&Palette::accent},{"accentHover",&Palette::accentHover},
 {"onAccent",&Palette::onAccent},{"link",&Palette::link},{"success",&Palette::success},{"warning",&Palette::warning},{"danger",&Palette::danger},
 {"dangerHover",&Palette::dangerHover},{"successSurface",&Palette::successSurface},{"warningSurface",&Palette::warningSurface},
 {"dangerSurface",&Palette::dangerSurface},{"icon",&Palette::icon},{"iconSecondary",&Palette::iconSecondary},
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
  const QColor panels[]= {lightPanel?QColor("#ffffff"):QColor("#141414"),lightPanel?QColor("#dedee2"):QColor("#3b3b3f")};
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
