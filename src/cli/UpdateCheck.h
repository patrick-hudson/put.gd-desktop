#pragma once
#include "core/Update.h"
namespace putgd {
// putgd-cli's resident-free modes for update checks (#293):
//   putgd-cli --update-info              this build's update channel and package; no network
//   putgd-cli --check-update CHANNEL     fetch CHANNEL's feed (stable or nightly) and compare;
//                                        the result's action says how this install updates
//   putgd-cli --open-software-updates    start the system's software updater for a
//                                        DEB/RPM install that follows put.gd's repository
// Each prints one JSON line. The resident runs them as short-lived children, so
// it never loads Qt Network or TLS, nor the repository and updater detection.
bool isUpdateMode(int argc,char** argv);
int runUpdateMode(int argc,char** argv);
// One GET of CHANNEL's feed, evaluated against build; needs a running
// QCoreApplication. Exposed for the request contract test.
QJsonObject fetchUpdate(const update::Build&,const QString& channel,Error&);
}
