# # *****************************************************************************
# # This source code is licensed under the MIT license found in the
# # LICENSE file in the root directory of this source tree.
# #
# # Copyright (c) 2024 Georgia Institute of Technology
# # *****************************************************************************

# CMake Requirement
cmake_minimum_required(VERSION 3.22)

# C++ requirement
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set the build type to Release if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Setup project
project(AstraSim)

# Add libraries.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/extern/helper/fmt")
option(SPDLOG_FMT_EXTERNAL ON) # override default option for spdlog.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/extern/helper/spdlog")

# Find Protobuf
# TODO: We want to use the advanced semantics 'protobuf CONFIG REQUIRED'.
# However, this requires protobuf to be compiled from source,
# which is enabled only in Docker containers. (Our documentation makes users install protobuc from apt-get)
# Therefore, enable 'protobuf CONFIG REQUIRED' only when we are building in Docker.
# TODO: Find a better way to detect if protobuf has been compiled by source, OR
# find a way to enable 'protobuf CONFIG REQUIRED' regardless of how protubuf was installed.
if(DEFINED ENV{PROTOBUF_FROM_SOURCE} AND "$ENV{PROTOBUF_FROM_SOURCE}" STREQUAL "True")
    find_package(protobuf CONFIG REQUIRED)
else()
    find_package(Protobuf REQUIRED)
endif()

# Files to compile
file(GLOB srcs
    "${CMAKE_CURRENT_SOURCE_DIR}/astra-sim/system/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/astra-sim/workload/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/astra-sim/scheduling/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/astra-sim/system/astraccl/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/astra-sim/system/astraccl/native_collectives/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/astra-sim/system/astraccl/native_collectives/logical_topology/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/astra-sim/system/astraccl/native_collectives/collective_algorithm/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/astra-sim/system/astraccl/custom_collectives/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/astra-sim/system/memory/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/astra-sim/system/scheduling/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/astra-sim/common/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/graph_frontend/chakra/src/third_party/utils/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/graph_frontend/chakra/schema/protobuf/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/graph_frontend/chakra/src/feeder_v3/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/remote_memory_backend/analytical/*.cc")

# Compile AstraSim Library
add_library(AstraSim STATIC ${srcs})

# Link libraries
target_link_libraries(AstraSim PUBLIC fmt::fmt)
target_link_libraries(AstraSim PUBLIC spdlog::spdlog)

# Same as above.
if(DEFINED ENV{PROTOBUF_FROM_SOURCE} AND "$ENV{PROTOBUF_FROM_SOURCE}" STREQUAL "True")
    target_link_libraries(AstraSim PUBLIC protobuf::libprotobuf)
else()
    target_link_libraries(AstraSim PUBLIC ${Protobuf_LIBRARIES})
endif()


# Include Directories
target_include_directories(AstraSim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Include analytical network frontend public headers so system code can include
#+ them transitively (e.g., ReconfigurableNetworkApi includes common/CommonNetworkApi.hh)
target_include_directories(AstraSim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/extern/network_backend/analytical/extern/yaml-cpp/include)
target_include_directories(AstraSim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/astra-sim/network_frontend/analytical/include)
target_include_directories(AstraSim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/extern/network_backend/analytical/include)
target_include_directories(AstraSim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/extern/network_backend/analytical/include/astra-network-analytical)
target_include_directories(AstraSim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/extern/graph_frontend/chakra/)
target_include_directories(AstraSim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/extern/graph_frontend/chakra/schema/protobuf)
target_include_directories(AstraSim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/extern/graph_frontend/chakra/src/third_party/utils)
target_include_directories(AstraSim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/extern/helper/)
target_include_directories(AstraSim PUBLIC ${Protobuf_INCLUDE_DIR})

# Same as above.
if(DEFINED ENV{PROTOBUF_FROM_SOURCE} AND "$ENV{PROTOBUF_FROM_SOURCE}" STREQUAL "True")
    target_include_directories(AstraSim PUBLIC ${Protobuf_INCLUDE_DIRS})
endif()

# ---- TopoMatch (unconditionally required) ---------------------------------
include(ExternalProject)

# 1. Submodule initialized?
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/topomatch/configure")
    message(FATAL_ERROR
        "TopoMatch submodule is missing. Initialize it with:\n"
        "  git submodule update --init --recursive")
endif()

# 2. hwloc + Scotch development libraries present?
find_path(HWLOC_INCLUDE_DIR hwloc.h)
find_library(HWLOC_LIB hwloc)
find_path(SCOTCH_INCLUDE_DIR scotch.h PATH_SUFFIXES scotch)
find_library(SCOTCH_LIB scotch)
find_library(SCOTCHERR_LIB scotcherr)
if(NOT HWLOC_INCLUDE_DIR OR NOT HWLOC_LIB OR NOT SCOTCH_INCLUDE_DIR
        OR NOT SCOTCH_LIB OR NOT SCOTCHERR_LIB)
    message(FATAL_ERROR
        "TopoMatch requires the hwloc and Scotch development libraries. Install them:\n"
        "  sudo apt-get install libhwloc-dev libscotch-dev pkg-config")
endif()
get_filename_component(SCOTCH_LIBDIR "${SCOTCH_LIB}" DIRECTORY)

# 3. Build libtopomatch ONLY (never the `mapping` binary) into a staging prefix.
set(TOPOMATCH_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/topomatch-install")
set(TOPOMATCH_LIB "${TOPOMATCH_PREFIX}/lib/libtopomatch.a")
set(TOPOMATCH_INCLUDE "${TOPOMATCH_PREFIX}/include")
file(MAKE_DIRECTORY "${TOPOMATCH_INCLUDE}")
ExternalProject_Add(topomatch_ep
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/topomatch"
    CONFIGURE_COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/extern/topomatch/configure"
        "--prefix=${TOPOMATCH_PREFIX}"
        "CFLAGS=-I${SCOTCH_INCLUDE_DIR} -O3"
        "CPPFLAGS=-I${SCOTCH_INCLUDE_DIR}"
        "LDFLAGS=-L${SCOTCH_LIBDIR}"
    BUILD_COMMAND $(MAKE) -C src/libtopomatch
    INSTALL_COMMAND $(MAKE) -C src/libtopomatch install
    BUILD_IN_SOURCE 1
    BUILD_BYPRODUCTS "${TOPOMATCH_LIB}")

# 4. Imported target + propagate include/link to AstraSim consumers.
add_library(topomatch::topomatch STATIC IMPORTED GLOBAL)
set_target_properties(topomatch::topomatch PROPERTIES
    IMPORTED_LOCATION "${TOPOMATCH_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${TOPOMATCH_INCLUDE};${SCOTCH_INCLUDE_DIR}")
add_dependencies(topomatch::topomatch topomatch_ep)
add_dependencies(AstraSim topomatch_ep)
target_link_libraries(AstraSim PUBLIC
    topomatch::topomatch ${SCOTCH_LIB} ${SCOTCHERR_LIB} ${HWLOC_LIB} z pthread)

# Properties
set_target_properties(AstraSim PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
set_target_properties(AstraSim
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/../bin/
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/../lib/
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/../lib/
)
