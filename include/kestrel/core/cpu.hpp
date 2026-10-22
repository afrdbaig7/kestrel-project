#pragma once
// ============================================================================
// kestrel/core/cpu.hpp
//
// CPU affinity helpers and host environment inspection (Linux).
// Used at init / setup time only — never on the hot path.
// ============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <sched.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace kestrel::core {

/// Pin the calling thread to one CPU. Returns false on failure.
inline bool pin_to_cpu(int cpu) noexcept {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return ::sched_setaffinity(0, sizeof(set), &set) == 0;
}

/// Current CPU the calling thread runs on (hint; may change unless pinned).
inline int current_cpu() noexcept {
    return ::sched_getcpu();
}

/// CPUs this process is allowed to run on (from the process affinity mask).
inline std::vector<int> allowed_cpus() noexcept {
    std::vector<int> out;
    cpu_set_t set;
    CPU_ZERO(&set);
    if (::sched_getaffinity(0, sizeof(set), &set) != 0) return out;
    for (int i = 0; i < CPU_SETSIZE; ++i)
        if (CPU_ISSET(i, &set)) out.push_back(i);
    return out;
}

struct CpuInfo {
    std::string model;
    bool invariant_tsc = false;
    bool rdtscp = false;
    bool constant_tsc = false;
    int physical_package = -1;
};

/// Parse /proc/cpuinfo for one logical CPU (first by default).
inline CpuInfo inspect_cpu(int cpu = -1) noexcept {
    CpuInfo info;
    FILE* f = std::fopen("/proc/cpuinfo", "re");
    if (!f) return info;
    char line[512];
    bool want = cpu < 0;      // default: first processor block
    bool have = false;
    while (!have && std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "processor", 9) == 0) {
            int id = -1;
            std::sscanf(line, "processor : %d", &id);
            want = (cpu < 0) || (id == cpu);
        }
        if (!want) continue;
        if (std::strncmp(line, "model name", 10) == 0) {
            const char* p = std::strchr(line, ':');
            if (p) {
                std::string v(p + 2);
                while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
                info.model = v;
            }
        } else if (std::strncmp(line, "flags", 5) == 0) {
            info.invariant_tsc = std::strstr(line, " nonstop_tsc ") != nullptr;
            info.rdtscp = std::strstr(line, " rdtscp ") != nullptr;
            info.constant_tsc = std::strstr(line, " constant_tsc ") != nullptr;
            have = true;   // flags is the last field we need
        } else if (std::strncmp(line, "physical id", 11) == 0) {
            std::sscanf(line, "physical id : %d", &info.physical_package);
        }
    }
    std::fclose(f);
    return info;
}

/// Quick kernel identity (uname release) for reproducibility metadata.
inline std::string kernel_release() noexcept {
    utsname u{};
    if (::uname(&u) != 0) return "unknown";
    return std::string(u.release);
}

}  // namespace kestrel::core
