# CMake Requirement
cmake_minimum_required(VERSION 3.15)

# C++ requirement
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set the build type to Release if not specified
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -fsanitize=address,undefined,leak -g")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -g")

# TODO: Remove this once binary_function deprecation is resolved.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-error")

# Extra flag injection point. The set(CMAKE_CXX_FLAGS ...) above runs before
# project(), so it creates a normal variable that shadows the cache/env value
# for the rest of the configure -- CXXFLAGS/-DCMAKE_CXX_FLAGS from outside are
# silently swallowed. These cache options are the supported way to append
# flags (used by build.sh for the PGO passes).
set(EXTRA_OPT_FLAGS "" CACHE STRING "Extra C++ compile flags appended to all targets")
set(EXTRA_LINK_FLAGS "" CACHE STRING "Extra linker flags appended to all targets")
if(EXTRA_OPT_FLAGS)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${EXTRA_OPT_FLAGS}")
endif()
if(EXTRA_LINK_FLAGS)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${EXTRA_LINK_FLAGS}")
endif()

# Setup project
project(AstraSim_Analytical)

# Interprocedural / link-time optimization for optimized builds. Enables
# cross-translation-unit inlining across the many small .cc files of the
# AstraSim library and the analytical backend. Guarded by a toolchain support
# check and skipped for Debug (LTO interacts poorly with the sanitizers used
# there). Set before the add_subdirectory calls so all targets inherit it.
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if(ipo_supported AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    message(STATUS "IPO/LTO enabled")
else()
    message(STATUS "IPO/LTO not enabled: ${ipo_error}")
endif()

# Compile-time flag to enable verbose logging in the analytical network backend.
option(ENABLE_VERBOSE_LOGS "Enable verbose logging" OFF)
if(ENABLE_VERBOSE_LOGS)
    add_compile_definitions(VERBOSELOG)
endif()

# Add vcpkg toolchain file
if (DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")
endif ()

# Compilation target
set(BUILDTARGET "all" CACHE STRING "Compilation target ([all]/congestion_unaware/congestion_aware)")

# Compile AstraSim library
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../ AstraSim)

# build appropriate analytical network backend as library
set(NETWORK_BACKEND_BUILD_AS_LIBRARY ON)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../extern/network_backend/analytical/ Analytical)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../astra-sim/network_frontend/analytical/ AstraSim_Analytical)

# Unit tests (opt-in via -DBUILD_UNIT_TESTS=ON; off by default so normal builds stay fast).
option(BUILD_UNIT_TESTS "Build astra-sim unit tests" OFF)
if(BUILD_UNIT_TESTS)
    enable_testing()
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../extern/network_backend/analytical/extern/googletest googletest)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../tests tests)
endif()