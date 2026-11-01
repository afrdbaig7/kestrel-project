# Host tools
add_executable(kestrel_inspect kestrel_inspect.cpp)
target_link_libraries(kestrel_inspect PRIVATE kestrel_core)

# Cold-path projection: journal recording -> JSON-lines demo snapshots.
add_executable(kestrel_journal_export journal_export.cpp)
target_link_libraries(kestrel_journal_export PRIVATE kestrel_core)

# Demo recorder: drives the real engine + journal to a binary file.
add_executable(kestrel_record_demo record_demo.cpp)
target_include_directories(kestrel_record_demo PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../tests/integration)
target_link_libraries(kestrel_record_demo PRIVATE kestrel_core)
