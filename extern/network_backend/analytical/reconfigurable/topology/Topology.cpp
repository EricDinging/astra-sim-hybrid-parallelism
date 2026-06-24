/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "reconfigurable/Topology.h"
#include "reconfigurable/Link.h"
#include <cassert>
#include <iostream>

using namespace NetworkAnalyticalReconfigurable;

void Topology::set_event_queue(std::shared_ptr<EventQueue> event_queue) noexcept {
    assert(event_queue != nullptr);

    // pass the given event_queue to Link
    Link::set_event_queue(std::move(event_queue));
}

int Topology::get_npus_count() const noexcept {
    return npus_count;
}

int Topology::get_devices_count() const noexcept {
    return devices_count;
}

Topology::Topology(int npus_count, int devices_count) noexcept {
    this->npus_count = npus_count;
    this->devices_count = devices_count;

    instantiate_devices();
    // Edges are added by TopologyManager once connectivity is known: the 6
    // torus neighbors per node in DOR mode (sparse), or all-pairs in BFS mode
    // (connect_all_pairs()). Only the self-loops are unconditional here.
    for (auto i = 0; i < devices_count; i++) {
        connect(i, i, Bandwidth(0), Latency(0), false);
    }
};

void Topology::connect_all_pairs() noexcept {
    for (auto i = 0; i < devices_count; i++) {
        for (auto j = i + 1; j < devices_count; j++) {
            connect(i, j, Bandwidth(0), Latency(0), true);
        }
    }
}

void Topology::connect_ocs_edge(const DeviceId src,
                                const DeviceId dest,
                                const Bandwidth bandwidth,
                                const Latency latency) noexcept {
    assert(0 <= src && src < devices_count);
    assert(0 <= dest && dest < devices_count);
    if (src != dest && !devices[src]->connected(dest)) {
        connect(src, dest, bandwidth, latency, true);
    }
}

void Topology::connect_torus_neighbors(const std::vector<int>& npus_per_dim, bool is_torus) noexcept {
    const int dims = static_cast<int>(npus_per_dim.size());
    assert(dims > 0);

    for (int id = 0; id < devices_count; ++id) {
        // id -> coordinates (dimension 0 is fastest-varying)
        std::vector<int> coord(dims);
        int rem = id;
        for (int d = 0; d < dims; ++d) {
            coord[d] = rem % npus_per_dim[d];
            rem /= npus_per_dim[d];
        }

        for (int d = 0; d < dims; ++d) {
            const int Nd = npus_per_dim[d];
            for (int step : {+1, -1}) {
                const int raw = coord[d] + step;
                if (!is_torus && (raw < 0 || raw >= Nd)) {
                    continue;  // mesh has no wrap-around link
                }
                std::vector<int> nbr = coord;
                nbr[d] = (raw + Nd) % Nd;

                int nbr_id = 0;
                int stride = 1;
                for (int k = 0; k < dims; ++k) {
                    nbr_id += nbr[k] * stride;
                    stride *= npus_per_dim[k];
                }
                // Dedup: small wrap-around dims (Nd<=2) make +1 and -1 the same
                // neighbor; connect() is bidirectional so skip if already linked.
                if (nbr_id != id && !devices[id]->connected(nbr_id)) {
                    connect(id, nbr_id, Bandwidth(0), Latency(0), true);
                }
            }
        }
    }
}

std::shared_ptr<Device> Topology::get_device(const DeviceId deviceId) noexcept {
    assert(0 <= deviceId && deviceId < devices_count);
    return devices[deviceId];
}

void Topology::send(std::unique_ptr<Chunk> chunk) noexcept {
    assert(chunk != nullptr);

    // get src npu node_id
    const auto src = chunk->current_device()->get_id();

    // assert src is valid
    assert(0 <= src && src < devices_count);

    // initiate transmission from src
    devices[src]->send(std::move(chunk));
}

void Topology::connect(const DeviceId src,
                       const DeviceId dest,
                       const Bandwidth bandwidth,
                       const Latency latency,
                       const bool bidirectional) noexcept {
    // assert the src and dest are valid
    assert(0 <= src && src < devices_count);
    assert(0 <= dest && dest < devices_count);

    // assert bandwidth and latency are valid
    assert(bandwidth >= 0);
    assert(latency >= 0);

    // std::cout << "Connecting device " << src << " to device " << dest
    //           << " with bandwidth: " << bandwidth
    //           << " and latency: " << latency << std::endl;

    // connect src -> dest
    devices[src]->connect(dest, bandwidth, latency);

    // if bidirectional, connect dest -> src
    if (bidirectional) {
        devices[dest]->connect(src, bandwidth, latency);
    }
}

void Topology::instantiate_devices() noexcept {
    // instantiate all devices
    for (auto i = 0; i < devices_count; i++) {
        devices.push_back(std::make_shared<Device>(i));
    }
}
