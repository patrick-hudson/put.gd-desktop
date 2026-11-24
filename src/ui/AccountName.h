#pragma once
#include <QString>
#include <functional>

// #1618: the account Quick redact's name category matches, read from
// getpwuid(getuid()) in the editor process, never from $USER. Nothing here is
// logged or sent to another process.
namespace putgd::ui {
struct Account {
 QString login; // pw_name
 QString realName; // pw_gecos before the first comma, trimmed; else AccountsService's RealName; may be empty
};
Account currentAccount();
// The same, asking `stopped` after the password entry and before the AccountsService
// call: once it is true, the bus is never contacted (Quick redact's worker after
// the editor closed).
Account currentAccount(const std::function<bool()>& stopped);
}
