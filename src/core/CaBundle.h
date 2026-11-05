#pragma once
#include <QtNetwork/qtnetworkglobal.h>
#if QT_CONFIG(ssl)
#include <QSslConfiguration>
#endif
#include <QString>

namespace putgd {
// The AppImage's AppRun names the host's CA bundle file (#329). The image's Qt
// looks only for *.pem/*.crt files in Debian-style directories and finds none
// on Fedora. Other builds use their distribution's own TLS setup; nothing is set.
inline QString hostCaBundle() {return qEnvironmentVariable("PUTGD_CA_BUNDLE");}
// Adds the host's certificates to Qt's default CAs, once per process, before
// the first request. Verification stays on; nothing outside the host's own
// trust store is added.
inline bool useHostCaBundle() {
#if QT_CONFIG(ssl)
 const auto path=hostCaBundle();
 if(path.isEmpty())return false;
 auto configuration=QSslConfiguration::defaultConfiguration();
 if(!configuration.addCaCertificates(path,QSsl::Pem))return false;
 QSslConfiguration::setDefaultConfiguration(configuration);
 return true;
#else
 return false;
#endif
}
}
