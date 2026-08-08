#!/bin/bash
set -e

# set paths
SCRIPT_DIR=$(dirname "$(realpath "$0")")
BUILD_DIR="${SCRIPT_DIR:?}"/build
CHAKRA_ET_DIR="${SCRIPT_DIR:?}"/../../extern/graph_frontend/chakra/schema/protobuf

# set functions
function compile_chakra_et() {
  # compile et_def.proto if one doesn't exist
  if [[ ! -f "${CHAKRA_ET_DIR:?}"/et_def.pb.h || ! -f "${CHAKRA_ET_DIR:?}"/et_def.pb.cc ]]; then
    protoc et_def.proto \
      --proto_path="${CHAKRA_ET_DIR:?}" \
      --cpp_out="${CHAKRA_ET_DIR:?}"
  fi

  if [[ ! -f "${CHAKRA_ET_DIR:?}"/et_def_pb2.py ]]; then
    protoc et_def.proto \
      --proto_path="${CHAKRA_ET_DIR:?}" \
      --python_out="${CHAKRA_ET_DIR:?}"
  fi
}

function setup() {
  # make build directory if one doesn't exist
  if [[ ! -d "${BUILD_DIR:?}" ]]; then
    mkdir -p "${BUILD_DIR:?}"
  fi

  # set concurrent build threads, capped at 16
  NUM_THREADS=$(nproc)
  if [[ ${NUM_THREADS} -ge 16 ]]; then
    NUM_THREADS=16
  fi
}

function compile_astrasim_analytical() {
  # compile AstraSim. The optional PGO phase (-p gen|use) appends the
  # profile-generate/-use flags through the CMakeLists EXTRA_*_FLAGS hooks:
  #   build.sh -p gen        # instrumented binary; profiles land in
  #                          # build/astra_analytical/pgo-profiles/
  #   <run representative sims with the instrumented binary>
  #   build.sh -p use        # rebuild optimized against those profiles
  # Results are byte-identical to a plain build; only speed changes.
  local extra_opt="" extra_link=""
  resolve_march
  if [[ ${pgo_phase:-} == "gen" ]]; then
    mkdir -p "${PGO_DIR:?}"
    extra_opt="-fprofile-generate -fprofile-dir=${PGO_DIR:?} -fprofile-update=single"
    extra_link="-fprofile-generate -fprofile-dir=${PGO_DIR:?}"
  elif [[ ${pgo_phase:-} == "use" ]]; then
    extra_opt="-fprofile-use -fprofile-dir=${PGO_DIR:?} -fprofile-correction -Wno-missing-profile"
    extra_link="-fprofile-use -fprofile-dir=${PGO_DIR:?} -fprofile-correction"
  fi
  if [[ ${march_arch:-} != "" ]]; then
    # -ffp-contract=off pins FP semantics (no FMA contraction) so results
    # stay byte-identical to a generic build; the win is integer/codegen.
    # The binary then requires the named ISA level on every deploy target
    # (haswell covers the current c6320/c6420/c6620/clnode farm groups).
    extra_opt="${extra_opt} -march=${march_arch} -mtune=skylake -ffp-contract=off"
  fi
  cd "${BUILD_DIR:?}" || exit
  cmake .. -DBUILDTARGET="$1" -DENABLE_VERBOSE_LOGS=OFF \
    -DEXTRA_OPT_FLAGS="${extra_opt}" -DEXTRA_LINK_FLAGS="${extra_link}"
  cmake --build . -j "${NUM_THREADS:?}"
}

function compile_astrasim_analytical_as_debug() {
  # compile AstraSim
  cd "${BUILD_DIR:?}" || exit
  cmake .. -DBUILDTARGET="$1" -DCMAKE_BUILD_TYPE=Debug -DENABLE_VERBOSE_LOGS=ON
  cmake --build . --config=Debug -j "${NUM_THREADS:?}"
}

function cleanup() {
  rm -rf "${BUILD_DIR:?}"
  rm -f "${CHAKRA_ET_DIR}/et_def.pb.cc"
  rm -f "${CHAKRA_ET_DIR}/et_def.pb.h"
  rm -f "${CHAKRA_ET_DIR}/et_def_pb2.py"
}

function create_symlink_astrasim() {
  # create symlinks for backward compatibility
  # ASTRA-sim library
  if [[ ! -d "${BUILD_DIR:?}"/AstraSim/lib/ ]]; then
    mkdir -p "${BUILD_DIR:?}"/AstraSim/lib/
  fi

  if [[ ! -L "${BUILD_DIR:?}"/AstraSim/lib/libAstraSim.a ]]; then
    ln -s "${BUILD_DIR:?}"/lib/libAstraSim.a "${BUILD_DIR:?}"/AstraSim/lib/libAstraSim.a
  fi
}

function create_symlink_congestion_unaware() {
  # create symlinks for backward compatibility
  # congestion_unaware
  if [[ ! -d "${BUILD_DIR:?}"/AnalyticalAstra/bin/ ]]; then
    mkdir -p "${BUILD_DIR:?}"/AnalyticalAstra/bin/
  fi

  if [[ ! -L "${BUILD_DIR:?}"/AnalyticalAstra/bin/AnalyticalAstra ]]; then
    ln -s "${BUILD_DIR:?}"/bin/AstraSim_Analytical_Congestion_Unaware "${BUILD_DIR:?}"/AnalyticalAstra/bin/AnalyticalAstra
  fi
}

function create_symlink_congestion_aware() {
  # create symlinks for backward compatibility
  # congestion_aware
  if [[ ! -d "${BUILD_DIR:?}"/AstraCongestion/bin/ ]]; then
    mkdir -p "${BUILD_DIR:?}"/AstraCongestion/bin/
  fi

  if [[ ! -L "${BUILD_DIR:?}"/AstraCongestion/bin/AstraCongestion ]]; then
    ln -s "${BUILD_DIR:?}"/bin/AstraSim_Analytical_Congestion_Aware "${BUILD_DIR:?}"/AstraCongestion/bin/AstraCongestion
  fi
}

function print_usage() {
  echo "print usage"
}

# set default option values
build_target="all"
build_as_debug=false
should_clean=false
pgo_phase=""
march_arch="auto"
PGO_DIR="${SCRIPT_DIR:?}/pgo-profiles"

# Resolve -a auto: enable the haswell ISA floor iff the BUILD host has the
# instructions (AVX2/BMI2/FMA), else fall back to a generic portable build.
# Note this is a proxy: it checks the machine compiling the binary, not the
# machines it will be copied to. Deploy targets must be >= Haswell (2013) /
# any AMD Zen when the floor is enabled; use -a generic to force a build
# that runs anywhere.
function resolve_march() {
  if [[ ${march_arch:?} == "auto" ]]; then
    if grep -m1 -q "avx2" /proc/cpuinfo &&
      grep -m1 -q "bmi2" /proc/cpuinfo &&
      grep -m1 -q "fma" /proc/cpuinfo; then
      march_arch="haswell"
    else
      march_arch=""
    fi
    echo "ISA floor (-a auto): ${march_arch:-generic}"
  elif [[ ${march_arch} == "generic" ]]; then
    march_arch=""
  fi
}

# Process command-line options
while getopts "t:ldp:a:" OPT; do
  case "${OPT:?}" in
  t)
    build_target="${OPTARG:?}"
    ;;
  l)
    should_clean=true
    ;;
  d)
    build_as_debug=true
    ;;
  p)
    pgo_phase="${OPTARG:?}"
    if [[ ${pgo_phase} != "gen" && ${pgo_phase} != "use" ]]; then
      echo "Invalid PGO phase: ${pgo_phase} (expected gen or use)" >&2
      exit 1
    fi
    ;;
  a)
    # ISA floor: "auto" (default) detects on the build host, "generic"
    # forces a fully portable build, anything else (e.g. "haswell") is
    # passed to -march verbatim. A floored binary SIGILLs on older CPUs.
    march_arch="${OPTARG:?}"
    ;;
  *)
    exit 1
    ;;
  esac
done

# check the validity of build target
if [[ ${build_target:?} != "all" &&
  ${build_target:?} != "congestion_unaware" &&
  ${build_target:?} != "congestion_aware" ]]; then
  echo "Invalid build target: ${build_target:?}" >&2
  exit 1
fi

# run operations as required
if [[ ${should_clean:?} == true ]]; then
  cleanup
else
  # setup ASTRA-sim build
  setup
  compile_chakra_et

  # compile ASTRA-sim
  if [[ ${build_as_debug:?} == true ]]; then
    compile_astrasim_analytical_as_debug "${build_target:?}"
  else
    compile_astrasim_analytical "${build_target:?}"
  fi

  # create symlinks as appropriate (for backward compatibility)
  if [[ ${build_target:?} == "all" ]]; then
    create_symlink_congestion_unaware
    create_symlink_congestion_aware
  elif [[ ${build_target:?} == "congestion_unaware" ]]; then
    create_symlink_congestion_unaware
  elif [[ ${build_target:?} == "congestion_aware" ]]; then
    create_symlink_congestion_aware
  fi
fi
