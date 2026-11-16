#include <dextro-iot/log.hpp>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace dextro::iot::log {
namespace {

std::atomic<int> g_level{static_cast<int>(Level::Info)};
std::mutex g_mutex;
Sink g_sink = nullptr;
void* g_user = nullptr;

int sd_prefix(Level level) noexcept
{
    switch (level) {
    case Level::Error:
        return 3;
    case Level::Warn:
        return 4;
    case Level::Info:
        return 6;
    case Level::Debug:
        return 7;
    }
    return 6;
}

}  // namespace

void set_sink(Sink sink, void* user) noexcept
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sink = sink;
    g_user = user;
}

void set_level(Level level) noexcept
{
    g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

Level level() noexcept
{
    return static_cast<Level>(g_level.load(std::memory_order_relaxed));
}

bool enabled(Level wanted) noexcept
{
    return static_cast<int>(wanted) <= g_level.load(std::memory_order_relaxed);
}

void write(Level wanted, const char* format, ...) noexcept
{
    if (!enabled(wanted) || format == nullptr) {
        return;
    }
    // Uma linha de log não pode ser o motivo de alocar: buffer fixo, truncado.
    char line[1024];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (n < 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_sink != nullptr) {
        g_sink(wanted, line, g_user);
        return;
    }
    std::fprintf(stderr, "<%d>%s\n", sd_prefix(wanted), line);
}

}  // namespace dextro::iot::log
