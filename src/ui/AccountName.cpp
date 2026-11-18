#include "AccountName.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <cerrno>
#include <pwd.h>
#include <unistd.h>

namespace putgd::ui {
namespace {
// The account's real name from AccountsService, for an account whose GECOS
// full name is empty. Checked on the lab desktops on 2026-11-18: f44 has
// "Patrick", d69 and m74 "patrick,,,", k72 "patrick", and aurora-amd (Aurora,
// Fedora Atomic) leaves it empty. AccountsService's RealName was empty there
// too, so this is a safety net for a desktop that keeps the name only in
// AccountsService (its user settings can set one). Two calls of
// at most 250 ms each on the system bus; any failure (no bus, no service, no
// such user) leaves the name empty.
QString accountsServiceName(uid_t uid) {
 auto bus=QDBusConnection::systemBus();
 if(!bus.isConnected())return {};
 constexpr int timeoutMs=250;
 auto find=QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.Accounts"),QStringLiteral("/org/freedesktop/Accounts"),QStringLiteral("org.freedesktop.Accounts"),QStringLiteral("FindUserById"));
 find<<qint64(uid);
 // No auto-start: a desktop without AccountsService answers at once.
 find.setAutoStartService(false);
 const auto user=bus.call(find,QDBus::Block,timeoutMs);
 if(user.type()!=QDBusMessage::ReplyMessage||user.arguments().isEmpty())return {};
 const auto path=qvariant_cast<QDBusObjectPath>(user.arguments().first()).path();
 if(path.isEmpty())return {};
 auto get=QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.Accounts"),path,QStringLiteral("org.freedesktop.DBus.Properties"),QStringLiteral("Get"));
 get<<QStringLiteral("org.freedesktop.Accounts.User")<<QStringLiteral("RealName");
 get.setAutoStartService(false);
 const auto name=bus.call(get,QDBus::Block,timeoutMs);
 if(name.type()!=QDBusMessage::ReplyMessage||name.arguments().isEmpty())return {};
 return qvariant_cast<QDBusVariant>(name.arguments().first()).variant().toString().trimmed();
}
}
Account currentAccount() {return currentAccount({});}
Account currentAccount(const std::function<bool()>& stopped) {
 Account account;
 const auto uid=getuid();
 // getpwuid_r: this runs on Quick redact's worker thread. A directory service
 // can return an entry larger than _SC_GETPW_R_SIZE_MAX suggests: ERANGE asks
 // for a bigger buffer, doubled up to 1 MiB.
 struct passwd entry{};struct passwd* found=nullptr;
 auto size=sysconf(_SC_GETPW_R_SIZE_MAX);if(size<=0)size=16384;
 QByteArray buffer;int status=ERANGE;
 for(;status==ERANGE&&size<=(1<<20);size*=2){buffer.resize(qsizetype(size));found=nullptr;status=getpwuid_r(uid,&entry,buffer.data(),size_t(buffer.size()),&found);}
 if(status==0&&found){
  account.login=QString::fromLocal8Bit(found->pw_name);
  if(found->pw_gecos)account.realName=QString::fromLocal8Bit(found->pw_gecos).section(',',0,0).trimmed();
 }
 // Stopped meanwhile (the editor closed): the system bus is never touched.
 if(account.realName.isEmpty()&&!(stopped&&stopped()))account.realName=accountsServiceName(uid);
 return account;
}
}
