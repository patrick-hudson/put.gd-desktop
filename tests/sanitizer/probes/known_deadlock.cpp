// Deliberately invalid control: sanitizer detection must fail with exit 66.
// This is an opt-in diagnostic executable, never an application test.
#include <mutex>
#include <thread>
int main() {
    std::mutex first, second;
    std::thread a([&] { std::lock_guard one(first); std::lock_guard two(second); });
    a.join();
    std::thread b([&] { std::lock_guard two(second); std::lock_guard one(first); });
    b.join();
}
