#include "core/Update.h"
// Compiled alone so changing PUTGD_PACKAGE_TARGET or PUTGD_UPDATE_CHECK
// rebuilds only this file (#293).
namespace putgd::update {
QString packageTarget() {return QStringLiteral(PUTGD_PACKAGE_TARGET_NAME);}
bool checkBuilt() {return PUTGD_UPDATE_CHECK_BUILT;}
}
