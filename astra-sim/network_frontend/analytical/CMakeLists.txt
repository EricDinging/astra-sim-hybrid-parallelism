# CMake Requirement
cmake_minimum_required(VERSION 3.15)

# C++ requirement
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set the build type to Release if not specified
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

# Setup project
project(AstraSim_Analytical)

# Compilation target
set(BUILDTARGET "all" CACHE STRING "Compilation target ([all]/congestion_unaware/congestion_aware)")

# Allocator: this simulator is allocation-bound -- a CPU profile of a
# cluster512 rfold run showed glibc malloc/free/malloc_consolidate at ~29% of
# self-time. jemalloc replaces them (dropping allocation to ~13%) and is ~24%
# faster wall-clock here with byte-identical simulation output. Prefer the
# static PIC archive so the
# deployed binary stays self-contained (the farm copies a prebuilt binary);
# fall back to the shared lib, then to system malloc. Opt out with
# -DUSE_JEMALLOC=OFF.
option(USE_JEMALLOC "Link jemalloc as the allocator" ON)
if (USE_JEMALLOC)
    find_library(JEMALLOC_LIB NAMES libjemalloc_pic.a jemalloc libjemalloc.so.2)
    if (JEMALLOC_LIB)
        message(STATUS "Using jemalloc allocator: ${JEMALLOC_LIB}")
    else ()
        message(WARNING "jemalloc not found; falling back to system malloc")
    endif ()
endif ()

# Include src files to compile
file(GLOB srcs_common
        ${CMAKE_CURRENT_SOURCE_DIR}/common/*.cc
)

file(GLOB srcs_congestion_unaware
        ${CMAKE_CURRENT_SOURCE_DIR}/congestion_unaware/*.cc
)

file(GLOB srcs_congestion_aware
        ${CMAKE_CURRENT_SOURCE_DIR}/congestion_aware/*.cc
)

file(GLOB srcs_reconfigurable
        ${CMAKE_CURRENT_SOURCE_DIR}/reconfigurable/*.cc
)

# Compile Congestion Unaware Backend
if (BUILDTARGET STREQUAL "all" OR BUILDTARGET STREQUAL "congestion_unaware")
    add_executable(AstraSim_Analytical_Congestion_Unaware ${srcs_congestion_unaware} ${srcs_common})
    target_sources(AstraSim_Analytical_Congestion_Unaware PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/congestion_unaware/main.cc)

    # Link libraries
    target_link_libraries(AstraSim_Analytical_Congestion_Unaware LINK_PRIVATE AstraSim)
    target_link_libraries(AstraSim_Analytical_Congestion_Unaware LINK_PRIVATE Analytical_Congestion_Unaware)
    if (JEMALLOC_LIB)
        target_link_libraries(AstraSim_Analytical_Congestion_Unaware LINK_PRIVATE ${JEMALLOC_LIB})
    endif ()

    # Include directories
    target_include_directories(AstraSim_Analytical_Congestion_Unaware PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/)
    target_include_directories(AstraSim_Analytical_Congestion_Unaware PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../extern/)
    target_include_directories(AstraSim_Analytical_Congestion_Unaware PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../extern/helper)

    # Properties
    # TODO: Switch to OFF after binary_function deprecation has been resolved
    set_target_properties(AstraSim_Analytical_Congestion_Unaware PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
    set_target_properties(AstraSim_Analytical_Congestion_Unaware
            PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/../bin/
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/../lib/
            ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/../lib/
    )
endif ()

# Compile Congestion Aware Backend
if (BUILDTARGET STREQUAL "all" OR BUILDTARGET STREQUAL "congestion_aware")
    add_executable(AstraSim_Analytical_Congestion_Aware ${srcs_congestion_aware} ${srcs_common})
    target_sources(AstraSim_Analytical_Congestion_Aware PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/congestion_aware/main.cc)

    # Link libraries
    target_link_libraries(AstraSim_Analytical_Congestion_Aware LINK_PRIVATE AstraSim)
    target_link_libraries(AstraSim_Analytical_Congestion_Aware LINK_PRIVATE Analytical_Congestion_Aware)
    if (JEMALLOC_LIB)
        target_link_libraries(AstraSim_Analytical_Congestion_Aware LINK_PRIVATE ${JEMALLOC_LIB})
    endif ()

    # Include directories
    target_include_directories(AstraSim_Analytical_Congestion_Aware PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/)
    target_include_directories(AstraSim_Analytical_Congestion_Aware PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../extern/)
    target_include_directories(AstraSim_Analytical_Congestion_Aware PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../extern/helper)

    # Properties
    # TODO: Switch to OFF after binary_function deprecation has been resolved
    set_target_properties(AstraSim_Analytical_Congestion_Aware PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
    set_target_properties(AstraSim_Analytical_Congestion_Aware
            PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/../bin/
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/../lib/
            ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/../lib/
    )
endif ()

# Compile Reconfigurable Backend
if (BUILDTARGET STREQUAL "all" OR BUILDTARGET STREQUAL "reconfigurable")
    add_executable(AstraSim_Analytical_Reconfigurable ${srcs_reconfigurable} ${srcs_common})
    target_sources(AstraSim_Analytical_Reconfigurable PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/reconfigurable/main.cc)

    # Link libraries
    target_link_libraries(AstraSim_Analytical_Reconfigurable LINK_PRIVATE AstraSim)
    target_link_libraries(AstraSim_Analytical_Reconfigurable LINK_PRIVATE Analytical_Reconfigurable)
    if (JEMALLOC_LIB)
        target_link_libraries(AstraSim_Analytical_Reconfigurable LINK_PRIVATE ${JEMALLOC_LIB})
    endif ()

    # Include directories
    target_include_directories(AstraSim_Analytical_Reconfigurable PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/)
    target_include_directories(AstraSim_Analytical_Reconfigurable PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/common/)
    target_include_directories(AstraSim_Analytical_Reconfigurable PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../extern/)
    target_include_directories(AstraSim_Analytical_Reconfigurable PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../extern/helper)

    # Properties
    set_target_properties(AstraSim_Analytical_Reconfigurable PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
    set_target_properties(AstraSim_Analytical_Reconfigurable
            PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/../bin/
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/../lib/
            ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/../lib/
    )
endif ()

