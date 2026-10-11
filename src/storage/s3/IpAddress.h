#pragma once
#include <QString>
namespace putgd {
struct IpAddress {bool numeric=false;bool loopback=false;};
// Input is the normalized host returned by QUrl, not arbitrary address text.
IpAddress classifyIpAddress(const QString& normalizedHost);
}
