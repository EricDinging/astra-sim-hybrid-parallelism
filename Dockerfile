# syntax=docker/dockerfile:1
#
# Image for astra-sim-hybrid-parallelism: the analytical backend plus the
# `stage` Chakra trace generator. The source is always cloned
# from GitHub at build time.
#
# Build (BuildKit/buildx required; default in Docker >= 23.0, otherwise install
# the docker-buildx-plugin package or set DOCKER_BUILDKIT=1):
#   docker buildx build -t astra:latest .


## Use Ubuntu
FROM ubuntu:22.04


### ================== System Setups ======================
## Install System Dependencies
ENV DEBIAN_FRONTEND=noninteractive
# Keep `apt update` in the SAME RUN as `apt install`: a separately-cached
# `apt update` layer goes stale when the mirror rolls package versions and
# deletes the old .debs, causing 404s on a later install. Combining them means
# any change to the package list re-runs update against a fresh index.
RUN apt -y update && apt -y upgrade && apt -y install \
    coreutils wget curl git \
    gcc g++ clang-format make cmake \
    libboost-dev libboost-program-options-dev \
    openmpi-bin openmpi-doc libopenmpi-dev \
    python3 python3-pip python3-venv \
    graphviz parallel \
    libhwloc-dev libscotch-dev pkg-config \
    libjemalloc-dev

## Create Python venv: Required for Python 3.10
RUN python3 -m venv /opt/venv/astra-sim
ENV PATH="/opt/venv/astra-sim/bin:$PATH"
RUN pip3 install --upgrade pip
### ======================================================


### ====== Abseil Installation: Protobuf Dependency ======
ARG ABSL_VERSION=20240722.0
WORKDIR /opt
RUN wget https://github.com/abseil/abseil-cpp/releases/download/${ABSL_VERSION}/abseil-cpp-${ABSL_VERSION}.tar.gz
RUN tar -xf abseil-cpp-${ABSL_VERSION}.tar.gz
RUN rm abseil-cpp-${ABSL_VERSION}.tar.gz

## Compile Abseil
WORKDIR /opt/abseil-cpp-${ABSL_VERSION}/build
RUN cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="/opt/abseil-cpp-${ABSL_VERSION}/install"
RUN cmake --build . --target install --config Release --parallel $(nproc)
# Required by protobuf compilation
ENV absl_DIR="/opt/abseil-cpp-${ABSL_VERSION}/install"
### ======================================================


### ============= Protobuf Installation ==================
ARG PROTOBUF_VERSION=29.0
WORKDIR /opt
RUN wget https://github.com/protocolbuffers/protobuf/releases/download/v${PROTOBUF_VERSION}/protobuf-${PROTOBUF_VERSION}.tar.gz
RUN tar -xf protobuf-${PROTOBUF_VERSION}.tar.gz
RUN rm protobuf-${PROTOBUF_VERSION}.tar.gz

## Compile Protobuf
WORKDIR /opt/protobuf-${PROTOBUF_VERSION}/build
RUN cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -Dprotobuf_BUILD_TESTS=OFF \
    -Dprotobuf_ABSL_PROVIDER=package \
    -DCMAKE_INSTALL_PREFIX="/opt/protobuf-${PROTOBUF_VERSION}/install"
RUN cmake --build . --target install --config Release --parallel $(nproc)
ENV PATH="/opt/protobuf-${PROTOBUF_VERSION}/install/bin:$PATH"
ENV protobuf_DIR="/opt/protobuf-${PROTOBUF_VERSION}/install"

# Also, install Python protobuf package
RUN pip3 install protobuf==5.${PROTOBUF_VERSION}

# Refer to astra-sim/CMakeLists.txt
ENV PROTOBUF_FROM_SOURCE="True"
### ======================================================


### ============= Get source code ==================
WORKDIR /app
RUN git clone https://github.com/EricDinging/astra-sim-hybrid-parallelism.git astra-sim
WORKDIR /app/astra-sim
RUN git checkout multitenant
RUN git submodule update --init --recursive
WORKDIR /app
RUN ln -s astra-sim/extern/graph_frontend/chakra .
RUN git clone https://github.com/meta-pytorch/chakra_replay.git
### ======================================================


### ============= Chakra Installation ==================
WORKDIR /app/chakra
RUN pip3 install .
RUN git config --global --add safe.directory /app/chakra_replay
WORKDIR /app/chakra_replay
RUN pip3 install -r requirements.txt
RUN pip3 install .
RUN pip3 install --upgrade protobuf
### ======================================================


### ============= Astra-sim Installation ==================
WORKDIR /app/astra-sim
RUN bash ./build/astra_analytical/build.sh
### ======================================================


### ============= stage Installation ==================
WORKDIR /app
RUN pip3 install tqdm
RUN git clone https://github.com/astra-sim/stage stage
### ======================================================


### ============= TopoMatch Installation ==================
WORKDIR /app
RUN apt -y update && apt -y install hwloc libhwloc-dev libscotch-dev pkg-config
RUN git clone --depth 1 https://github.com/shuoshuc/TopoMatch.git topomatch
WORKDIR /app/topomatch
RUN ./configure CFLAGS="-I/usr/include/scotch" CPPFLAGS="-I/usr/include/scotch" LDFLAGS="-L/usr/lib/x86_64-linux-gnu"
RUN make && make install
RUN echo "/usr/local/lib" | tee /etc/ld.so.conf.d/topomatch.conf && ldconfig
### ======================================================


### ================== Finalize ==========================
RUN pip3 install natsort hilbertcurve
## Move to the application directory
WORKDIR /app
### ======================================================
