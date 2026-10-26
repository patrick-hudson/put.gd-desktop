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
// meet 3:1 on them; test_theme checks both schemes.
const Palette dark{
 QColor("#191919"),QColor("#222222"),QColor("#2A2A2A"),QColor("#353535"),QColor("#343434"),QColor("#515151"),
 QColor("#F0F0EC"),QColor("#D2D2CF"),QColor("#AAAAA6"),QColor("#727270"),
 QColor("#E8B66D"),QColor("#F0C887"),QColor("#24201A"),QColor("#E8C591"),
 QColor("#91BCA2"),QColor("#E8B66D"),QColor("#E59B93"),QColor("#EFB4AE"),QColor("#29372E"),QColor("#3A3226"),QColor("#3D2A29"),
 QColor("#D6D6D2"),QColor("#777773"),
};
const Palette light{
 QColor("#EEEEEB"),QColor("#F8F8F5"),QColor("#FFFFFF"),QColor("#DEDEDA"),QColor("#D1D1CC"),QColor("#AAAAA3"),
 QColor("#292926"),QColor("#484844"),QColor("#62625B"),QColor("#91918A"),
 QColor("#8F5B20"),QColor("#774816"),QColor("#FFFFFF"),QColor("#80521D"),
 QColor("#386A4B"),QColor("#80521D"),QColor("#A44139"),QColor("#8F322B"),QColor("#DFEDE3"),QColor("#EEE3D0"),QColor("#F4E0DE"),
 QColor("#454540"),QColor("#A6A69D"),
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
 static const TrayTints dark{QColor("#D6D6D2"),QColor("#62D69B"),QColor("#F3C66F"),QColor("#FFB4AB")};
 static const TrayTints light{QColor("#454540"),QColor("#146C42"),QColor("#8A5300"),QColor("#B4312A")};
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
