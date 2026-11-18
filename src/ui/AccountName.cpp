#include "AccountName.h"
#include <pwd.h>
#include <unistd.h>

namespace putgd::ui {
Account currentAccount() {
 // #1618 stub: the matcher worker owns this file.
 Account account;
 if(const auto* entry=getpwuid(getuid())){account.login=QString::fromLocal8Bit(entry->pw_name);if(entry->pw_gecos)account.realName=QString::fromLocal8Bit(entry->pw_gecos).section(',',0,0).trimmed();}
 return account;
}
}
