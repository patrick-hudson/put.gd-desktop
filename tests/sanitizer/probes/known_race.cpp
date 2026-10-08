// Deliberately invalid control: sanitizer detection must fail with exit 66.
// This is an opt-in diagnostic executable, never an application test.
#include <atomic>
#include <thread>

int main()
{
    std::atomic_bool start{false};
    int shared = 0;
    auto write = [&] {
        while (!start.load(std::memory_order_acquire)) {}
        shared = 1;
    };
    std::thread first(write);
    std::thread second(write);
    start.store(true, std::memory_order_release);
    first.join();
    second.join();
    return shared == 1 ? 0 : 1;
}
