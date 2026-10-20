#include "Theme.h"
#include <algorithm>
#include <atomic>
#include <cmath>

namespace putgd::Theme {
namespace {
// The dark values are the redesign's; the settings stylesheet's near-copies
// (#111318 for #111317 and so on) now read these too. Every text colour meets
// WCAG AA (4.5:1) on bg, surface, surfaceRaised and selected, and the accent
// (focus rings) meets 3:1 on them; test_theme checks both schemes.
const Palette dark{
 QColor("#111317"),QColor("#181B21"),QColor("#20242C"),QColor("#232838"),QColor("#2A2F38"),QColor("#343A45"),
 QColor("#E7E9EE"),QColor("#C4C9D2"),QColor("#9AA1AD"),QColor("#6F7682"),
 QColor("#6EA8FE"),QColor("#8DBBFF"),QColor("#111317"),QColor("#8DBBFF"),
 QColor("#4CC38A"),QColor("#E5A443"),QColor("#F07A6A"),QColor("#F4A497"),QColor("#1C3328"),QColor("#3A3020"),QColor("#3A2326"),
 QColor("#9BBCFF"),QColor("#9BBCFF").darker(190),
};
const Palette light{
 QColor("#F5F6F8"),QColor("#FFFFFF"),QColor("#EDEFF3"),QColor("#DCE7FA"),QColor("#D5D9E0"),QColor("#AEB5C1"),
 QColor("#1B1E24"),QColor("#3A414D"),QColor("#585F6B"),QColor("#8B929D"),
 QColor("#1F64D6"),QColor("#1753B5"),QColor("#FFFFFF"),QColor("#1754BD"),
 QColor("#146C42"),QColor("#8A5300"),QColor("#B4312A"),QColor("#96271F"),QColor("#E3F4EA"),QColor("#FBF0DC"),QColor("#FBE7E5"),
 QColor("#2759B8"),QColor("#B4C8EE"),
};
struct Token {const char* name;QColor Palette::* member;};
constexpr Token table[]{
 {"bg",&Palette::bg},{"surface",&Palette::surface},{"surfaceRaised",&Palette::surfaceRaised},{"selected",&Palette::selected},
 {"border",&Palette::border},{"borderStrong",&Palette::borderStrong},{"text",&Palette::text},{"textSecondary",&Palette::textSecondary},
 {"textMuted",&Palette::textMuted},{"textDisabled",&Palette::textDisabled},{"accent",&Palette::accent},{"accentHover",&Palette::accentHover},
 {"onAccent",&Palette::onAccent},{"link",&Palette::link},{"success",&Palette::success},{"warning",&Palette::warning},{"danger",&Palette::danger},
 {"dangerHover",&Palette::dangerHover},{"successSurface",&Palette::successSurface},{"warningSurface",&Palette::warningSurface},
 {"dangerSurface",&Palette::dangerSurface},{"icon",&Palette::icon},{"iconSecondary",&Palette::iconSecondary},
};
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
 for(const auto& token:table)result[token.name]=(values.*token.member).name(QColor::HexRgb);
 return result;
}
QString stylesheet(QString qss,Scheme scheme){
 const auto& values=palette(scheme);
 for(const auto& token:table)qss.replace(QLatin1Char('%')+QLatin1String(token.name)+QLatin1Char('%'),(values.*token.member).name(QColor::HexRgb));
 return qss;
}
const TrayTints& trayTints(bool lightPanel){
 static const TrayTints dark{QColor("#9BBCFF"),QColor("#62D69B"),QColor("#F3C66F"),QColor("#FFB4AB")};
 static const TrayTints light{QColor("#2759B8"),QColor("#146C42"),QColor("#8A5300"),QColor("#B4312A")};
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
