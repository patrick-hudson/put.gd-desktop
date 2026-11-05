#include "core/SystemUpdates.h"
#include "core/Update.h"
// Compiled alone so changing PUTGD_PACKAGE_TARGET, PUTGD_UPDATE_CHECK or
// PUTGD_PACKAGE_REPOSITORY_URL rebuilds only this file (#293).
namespace putgd::update {
QString packageTarget() {return QStringLiteral(PUTGD_PACKAGE_TARGET_NAME);}
bool checkBuilt() {return PUTGD_UPDATE_CHECK_BUILT;}
QString packageRepository() {return QStringLiteral(PUTGD_PACKAGE_REPOSITORY);}
}
