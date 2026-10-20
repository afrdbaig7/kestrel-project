# ADR-0002: Toolchain — GCC 14.2 only; libFuzzer and PGO deferred behind opt-in

Date: 2026-11-01 · Status: accepted

## Context

Environment has GCC 14.2.0 (full C++23). Clang 18 is not installed; therefore
libFuzzer (`-fsanitize=fuzzer`) and clang-based PGO cannot be built or verified here.
Installing a second toolchain would inflate build time on a 2-vCPU sandbox.

## Decision

- Engine/tests/benches build with GCC 14.2, C++23, CMake + Ninja.
- Malformed/truncated frame coverage is provided by a seeded structured-mutation
  harness that runs as a normal (fast, deterministic) unit test in every build.
- Real libFuzzer targets exist in `tests/fuzz/` and are compiled only when a Clang
  toolchain is available (`-DKESTREL_ENABLE_LIBFUZZER=ON`, requires clang). They are
  NOT claimed as verified in this environment.
- PGO is deferred; LTO is available opt-in (`KESTREL_ENABLE_LTO=ON`) and evaluated
  separately with evidence in the benchmark report.

## Consequences

Fuzz-style coverage is reproducible in CI today; deep mutation fuzzing needs clang
in a future session. This is recorded as an environment limitation, not a gap in the
test plan.
