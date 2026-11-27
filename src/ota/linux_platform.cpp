#include <dextro-iot/ota/linux_platform.hpp>

#include <dextro-iot/clock.hpp>
#include <dextro-iot/ota/fs.hpp>
#include <dextro-iot/ota/rauc.hpp>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>

extern char** environ;

namespace dextro::iot::ota {
namespace {

constexpr std::size_t kMaxOutput = 4096;

std::string trim(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
        s.pop_back();
    }
    return s;
}

std::string upper(std::string s)
{
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

/// O ambiente deste processo sem as variáveis do protocolo do systemd (ver
/// `run_command` no header).
std::vector<char*> child_environment()
{
    static constexpr const char* kSystemd[] = {"NOTIFY_SOCKET=", "WATCHDOG_PID=", "WATCHDOG_USEC=",
                                               "LISTEN_FDS=",    "LISTEN_PID=",   "LISTEN_FDNAMES="};
    std::vector<char*> env;
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
        bool out = false;
        for (const char* prefix : kSystemd) {
            if (std::strncmp(*e, prefix, std::strlen(prefix)) == 0) {
                out = true;
                break;
            }
        }
        if (!out) {
            env.push_back(*e);
        }
    }
    env.push_back(nullptr);
    return env;
}

/// O SIGKILL só vale quando o filho sai de um sleep de I/O (estado D): quem
/// espera por ele é uma thread à parte, para o chamador não ficar preso e o
/// filho não virar zumbi (waitpid(2)).
void reap_later(pid_t pid)
{
    std::thread([pid] {
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }).detach();
}

}  // namespace

int run_command(const std::vector<std::string>& argv, std::string& output, const KeepGoing& tick,
                std::int64_t timeout_ms)
{
    output.clear();
    if (argv.empty()) {
        return -1;
    }
    int pipefd[2];
    if (::pipe2(pipefd, O_CLOEXEC) != 0) {
        return -1;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], 1);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], 2);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);

    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const std::string& a : argv) {
        args.push_back(const_cast<char*>(a.c_str()));
    }
    args.push_back(nullptr);

    std::vector<char*> env = child_environment();
    pid_t pid = 0;
    const int rc = ::posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), env.data());
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipefd[1]);
    if (rc != 0) {
        ::close(pipefd[0]);
        output = std::string("não executou ") + argv[0] + ": " + std::strerror(rc);
        return -1;
    }

    const auto started = std::chrono::steady_clock::now();
    bool timed_out = false;
    char buf[1024];
    for (;;) {
        int wait_ms = 1000;
        if (timeout_ms > 0) {
            const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - started)
                                   .count();
            if (spent >= timeout_ms) {
                timed_out = true;
                break;
            }
            wait_ms = static_cast<int>(std::min<std::int64_t>(wait_ms, timeout_ms - spent));
        }
        pollfd p{pipefd[0], POLLIN, 0};
        const int ready = ::poll(&p, 1, wait_ms);
        if (ready > 0) {
            const ssize_t n = ::read(pipefd[0], buf, sizeof buf);
            if (n > 0) {
                output.append(buf, static_cast<std::size_t>(n));
                if (output.size() > kMaxOutput) {
                    output.erase(0, output.size() - kMaxOutput);
                }
                continue;
            }
            if (n == 0) {
                break;  // o filho fechou a saída
            }
            if (errno != EINTR && errno != EAGAIN) {
                break;
            }
        }
        if (tick) {
            (void)tick();  // não interrompe: só alimenta o watchdog
        }
    }
    ::close(pipefd[0]);
    if (timed_out) {
        ::kill(pid, SIGKILL);
        reap_later(pid);
        output += "\n(" + argv[0] + ": prazo de " + std::to_string(timeout_ms) + " ms esgotado)";
        return kRunTimedOut;
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string slot_from_cmdline(const std::string& cmdline, const std::string& key)
{
    if (key.empty()) {
        return {};
    }
    std::istringstream in(cmdline);
    std::string word;
    while (in >> word) {
        if (word.rfind(key, 0) == 0) {
            return word.substr(key.size());
        }
    }
    return {};
}

MarkGood parse_mark_good(const std::string& show_output)
{
    std::istringstream in(show_output);
    std::string line;
    std::string active;
    std::string result;
    long long status = -1;
    long long exited_at = 0;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string value = trim(line.substr(eq + 1));
        if (key == "ActiveState") {
            active = value;
        } else if (key == "Result") {
            result = value;
        } else if (key == "ExecMainStatus") {
            status = std::atoll(value.c_str());
        } else if (key == "ExecMainExitTimestampMonotonic") {
            exited_at = std::atoll(value.c_str());
        }
    }
    if (active == "failed") {
        return MarkGood::Failed;
    }
    // Saiu NESTE boot (o relógio monotônico do systemd recomeça no boot), com
    // 0, e a unit não está mais rodando: o slot foi marcado bom.
    if (exited_at > 0 && status == 0 && result == "success" && active != "active" &&
        active != "activating") {
        return MarkGood::Good;
    }
    return MarkGood::Pending;
}

LinuxPlatform::LinuxPlatform(Config config) : config_(std::move(config))
{
}

std::string LinuxPlatform::boot_id()
{
    std::string text;
    fs::read_file("/proc/sys/kernel/random/boot_id", text, 256);
    return trim(text);
}

std::string LinuxPlatform::current_slot()
{
    std::string cmdline;
    fs::read_file("/proc/cmdline", cmdline, 8192);
    std::string slot = slot_from_cmdline(cmdline, config_.slot_cmdline_key);
    if (slot.empty()) {
        std::string out;
        RaucStatus status;
        if (run_command({config_.rauc, "status", "--output-format=shell"}, out) == 0 &&
            parse_rauc_status(out, status)) {
            slot = status.booted;
        }
    }
    return config_.slot_upper_case ? upper(slot) : slot;
}

std::string LinuxPlatform::other_slot(const std::string& current)
{
    std::string out;
    RaucStatus status;
    if (run_command({config_.rauc, "status", "--output-format=shell"}, out) != 0 ||
        !parse_rauc_status(out, status)) {
        return {};
    }
    const std::string other = status.other_bootname(current);
    return config_.slot_upper_case ? upper(other) : other;
}

MarkGood LinuxPlatform::mark_good()
{
    std::string out;
    const int rc = run_command({config_.systemctl, "show", "-p", "ActiveState", "-p", "Result",
                                "-p", "ExecMainStatus", "-p", "ExecMainExitTimestampMonotonic",
                                config_.mark_good_unit},
                               out);
    return rc == 0 ? parse_mark_good(out) : MarkGood::Pending;
}

bool LinuxPlatform::slot_bad(const std::string& slot)
{
    std::string out;
    RaucStatus status;
    if (run_command({config_.rauc, "status", "--output-format=shell"}, out) != 0 ||
        !parse_rauc_status(out, status)) {
        return false;
    }
    const RaucSlot* s = status.by_bootname(slot);
    return s != nullptr && s->boot_status == "bad";
}

bool LinuxPlatform::reboot()
{
    ::sync();
    std::string out;
    return run_command({config_.systemctl, "reboot"}, out) == 0;
}

std::int64_t LinuxPlatform::free_bytes(const std::string& dir)
{
    struct statvfs st {};
    if (::statvfs(dir.c_str(), &st) != 0) {
        return -1;
    }
    return static_cast<std::int64_t>(st.f_bavail) * static_cast<std::int64_t>(st.f_frsize);
}

std::int64_t LinuxPlatform::wall_ms()
{
    return clock::wall_ms();
}

std::int64_t LinuxPlatform::mono_ms()
{
    return clock::monotonic_ms();
}

std::string LinuxPlatform::device_token()
{
    return read_device_token(config_.token);
}

}  // namespace dextro::iot::ota
