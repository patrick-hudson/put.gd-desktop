#pragma once
// A store-only ZIP writer for diagnostics packages: Windows ships no deflate
// library a resident can use without a third-party dependency, and a package
// is at most 20 MiB anyway. Names are UTF-8 (general-purpose flag bit 11).
#include <cstdint>
#include <string>

namespace putgd {
class Zip {
public:
    // Entry times are the package's creation time (UTC).
    explicit Zip(long long createdMs);
    void add(const std::string& name, const std::string& bytes);
    std::string finish() const;
private:
    std::string data_, central_;
    uint16_t time_ = 0, date_ = 0;
    int entries_ = 0;
};
}
