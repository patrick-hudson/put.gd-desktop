#pragma once
#include "core/Error.h"
namespace putgd {
class LoginStartup {
public:
 static Error apply(const QString& configurationRoot,const QString& executable,bool enabled);
};
}
