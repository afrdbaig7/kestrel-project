// kestrel_inspect — system inspection for measurement reproducibility.
// Prints the environment facts that must accompany any benchmark run
// (see docs/benchmarks/methodology.md). Read-only; changes nothing.
#include <cstdio>
#include <string>
#include <vector>

#include "kestrel/core/clock.hpp"
#include "kestrel/core/cpu.hpp"

int main() {
    using namespace kestrel::core;
    const CpuInfo info = inspect_cpu();
    std::printf("kestrel_inspect\n");
    std::printf("cpu_model: %s\n", info.model.c_str());
    std::printf("kernel: %s\n", kernel_release().c_str());
    std::printf("allowed_cpus:");
    for (const int c : allowed_cpus()) std::printf(" %d", c);
    std::printf("\n");
    std::printf("invariant_tsc: %s\n", info.invariant_tsc ? "true" : "false");
    std::printf("rdtscp: %s\n", info.rdtscp ? "true" : "false");
    std::printf("constant_tsc: %s\n", info.constant_tsc ? "true" : "false");
    std::printf("clock_source: %s\n", TscClock::source_name());
    if (TscClock::invariant_tsc_available()) {
        TscClock::calibrate(6, 20);
        std::printf("tsc_ticks_per_ns: %.6f\n", TscClock::ticks_per_ns());
    }
    return 0;
}
