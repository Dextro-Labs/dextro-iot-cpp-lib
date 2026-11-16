#include <dextro-iot/clock.hpp>

#include <chrono>
#include <time.h>

namespace dextro::iot::clock {

std::int64_t monotonic_ns() noexcept
{
    timespec ts{};
    (void)::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

std::int64_t wall_ms() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace dextro::iot::clock
