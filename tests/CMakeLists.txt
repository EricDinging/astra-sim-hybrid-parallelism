# Parent CMake for astra-sim unit tests. Each per-module subdir is added below.
# Regression tests under tests/rt_* are NOT added here — they are driven by
# tests/run_all.sh and do not participate in the CMake build.

include(GoogleTest)

add_subdirectory(scheduling)
# Future per-module unit-test subdirs land here:
#   add_subdirectory(system)
#   add_subdirectory(workload)
