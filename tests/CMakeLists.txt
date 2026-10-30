# Tests: unit, property, integration (Milestones A-C)
include(GoogleTest)

add_executable(kestrel_unit_tests
  unit/test_spsc_ring.cpp
  unit/test_object_pool.cpp
  unit/test_flat_map.cpp
  unit/test_bitmap.cpp
  unit/test_histogram.cpp
  unit/test_clock.cpp
  unit/test_protocols_kitch.cpp
  unit/test_protocols_kouch.cpp
  unit/test_book.cpp
  unit/test_feed_handler.cpp
  unit/test_risk.cpp
  unit/test_oms.cpp
  unit/test_journal.cpp
  unit/test_fuzz_harness.cpp
  property/property_primitives.cpp
  property/property_book.cpp
)
target_link_libraries(kestrel_unit_tests PRIVATE kestrel_core GTest::gtest_main)
gtest_discover_tests(kestrel_unit_tests DISCOVERY_TIMEOUT 120)

add_executable(kestrel_integration_tests
  integration/test_vertical_slice.cpp
  integration/test_golden_replay.cpp
  integration/test_fault_scenarios.cpp
  integration/test_allocation_guard.cpp
)
target_link_libraries(kestrel_integration_tests PRIVATE kestrel_core GTest::gtest_main)
gtest_discover_tests(kestrel_integration_tests DISCOVERY_TIMEOUT 120)
