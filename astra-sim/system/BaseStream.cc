/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/system/BaseStream.hh"

#include "astra-sim/system/StreamBaseline.hh"

using namespace AstraSim;

void BaseStream::changeState(StreamState state) {
    this->state = state;
}

BaseStream::BaseStream(int stream_id,
                       Sys* owner,
                       std::list<CollectivePhase> phases_to_go) {
    this->stream_id = stream_id;
    this->owner = owner;
    this->initialized = false;
    this->phases_to_go = std::move(phases_to_go);
    // Iterate the member (the moved-in list): init() only touches the shared
    // Algorithm* held by each phase, so the same Algorithm objects get
    // initialized as before the move.
    for (auto& vn : this->phases_to_go) {
        if (vn.algorithm != nullptr) {
            vn.init(this);
        }
    }
    state = StreamState::Created;
    preferred_scheduling = SchedulingPolicy::None;
    creation_time = Sys::boostedTick();
    total_packets_sent = 0;
    current_queue_id = -1;
    priority = 0;
}
