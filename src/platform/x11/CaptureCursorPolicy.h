#pragma once
#include <QString>
#include <QVersionNumber>
#include <QtGlobal>
namespace putgd {
// Qt 6.5 changed from libXcursor to libxcb-cursor. Their theme inputs differ;
// an optional libXcursor seed cannot replace the newer Qt cursor safely.
inline bool captureCursorCacheCompatible(const QString& runtimeVersion){
 qsizetype suffix=0;const auto version=QVersionNumber::fromString(runtimeVersion,&suffix);
 return suffix==runtimeVersion.size()&&version.segmentCount()==3&&
        version>=QVersionNumber(6,4,2)&&version<QVersionNumber(6,5,0);
}
inline bool captureCursorCacheCompatible(){
 return !qEnvironmentVariableIsSet("XCURSOR_RESIZED")&&captureCursorCacheCompatible(QString::fromLatin1(qVersion()));
}
}
