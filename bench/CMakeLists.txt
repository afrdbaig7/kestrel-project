# Benchmarks: Google Benchmark micro suite + engine-level latency harnesses
add_executable(kestrel_bench_micro
  bench_micro.cpp
)
target_link_libraries(kestrel_bench_micro PRIVATE kestrel_core benchmark::benchmark_main)

# Engine-level latency/throughput harnesses (Milestone D). They reuse the
# integration test host so benchmark configurations are IDENTICAL to the
# correctness tests (comparability is a methodology requirement).
add_executable(kestrel_bench_engine
  bench_engine.cpp
)
target_include_directories(kestrel_bench_engine PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../tests/integration)
target_link_libraries(kestrel_bench_engine PRIVATE kestrel_core)
