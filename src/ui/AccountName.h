#pragma once
#include <QString>

// #1618: the account Quick redact's name category matches, read from
// getpwuid(getuid()) in the editor process, never from $USER. Nothing here is
// logged or sent to another process.
namespace putgd::ui {
struct Account {
 QString login; // pw_name
 QString realName; // pw_gecos before the first comma, trimmed; else AccountsService's RealName; may be empty
};
Account currentAccount();
}
