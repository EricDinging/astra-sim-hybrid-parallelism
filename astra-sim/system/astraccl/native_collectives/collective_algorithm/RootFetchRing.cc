/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/system/astraccl/native_collectives/collective_algorithm/RootFetchRing.hh"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <numeric>
#include <queue>
#include <random>
#include <string>
#include <unordered_map>

#include "astra-sim/system/RecvPacketEventHandlerData.hh"

using namespace AstraSim;

namespace {

constexpr int kTagStride = 100000;

struct StageKey {
    int round;
    int substep;

    bool operator<(const StageKey& other) const {
        if (round != other.round) {
            return round < other.round;
        }
        return substep < other.substep;
    }
};

}  // namespace

int AstraSim::g_root_fetch_mf = 2;
RootFetchRing::QuarterPlacement AstraSim::g_root_fetch_quarter =
    RootFetchRing::QuarterPlacement::Q1;
int AstraSim::g_root_fetch_total_failures = -1;
std::uint32_t AstraSim::g_root_fetch_failure_seed = 1;
bool AstraSim::g_root_fetch_fail_before_first_round = false;
std::vector<std::uint64_t> AstraSim::g_root_fetch_last_rank_tx;
std::vector<std::uint64_t> AstraSim::g_root_fetch_last_rank_rx;
std::uint64_t AstraSim::g_root_fetch_last_total_messages = 0;
std::vector<int> AstraSim::g_root_fetch_last_failed_senders;
std::vector<int> AstraSim::g_root_fetch_last_failed_receivers;
std::vector<int> AstraSim::g_root_fetch_last_failure_rounds;

namespace {

void apply_root_fetch_env_overrides() {
    if (const char* value = std::getenv("ASTRA_ROOT_FETCH_MF")) {
        AstraSim::g_root_fetch_mf = std::stoi(value);
    }
    if (const char* value = std::getenv("ASTRA_ROOT_FETCH_TOTAL_FAILURES")) {
        AstraSim::g_root_fetch_total_failures = std::stoi(value);
    }
    if (const char* value = std::getenv("ASTRA_ROOT_FETCH_FAILURE_SEED")) {
        AstraSim::g_root_fetch_failure_seed =
            static_cast<std::uint32_t>(std::stoul(value));
    }
    if (const char* value =
            std::getenv("ASTRA_ROOT_FETCH_FAIL_BEFORE_FIRST_ROUND")) {
        const std::string enabled(value);
        AstraSim::g_root_fetch_fail_before_first_round =
            enabled == "1" || enabled == "true" || enabled == "TRUE";
    }
    if (const char* value = std::getenv("ASTRA_ROOT_FETCH_QUARTER")) {
        const std::string quarter(value);
        if (quarter == "Q1") {
            AstraSim::g_root_fetch_quarter =
                RootFetchRing::QuarterPlacement::Q1;
        } else if (quarter == "Q2") {
            AstraSim::g_root_fetch_quarter =
                RootFetchRing::QuarterPlacement::Q2;
        } else if (quarter == "Q3") {
            AstraSim::g_root_fetch_quarter =
                RootFetchRing::QuarterPlacement::Q3;
        } else {
            Sys::sys_panic(
                "ASTRA_ROOT_FETCH_QUARTER must be one of Q1, Q2, Q3");
        }
    }
}

}  // namespace

RootFetchRing::RootFetchRing(ComType type,
                             int id,
                             RingTopology* ring_topology,
                             uint64_t data_size,
                             RingTopology::Direction direction,
                             InjectionPolicy injection_policy)
    : Algorithm(),
      ring_topology(ring_topology),
      direction(direction),
      nodes_in_ring(ring_topology->get_nodes_in_ring()),
      next_stage_index(0),
      pending_stage_recvs(0),
      protocol_finished(false) {
    (void)injection_policy;

    apply_root_fetch_env_overrides();

    if (type != ComType::Reduce_Scatter && type != ComType::All_Gather &&
        type != ComType::All_Reduce) {
        Sys::sys_panic(
            "RootFetchRing supports Reduce_Scatter, All_Gather, and All_Reduce");
    }
    if (g_root_fetch_mf != 1 && g_root_fetch_mf != 2 && g_root_fetch_mf != 4) {
        Sys::sys_panic(
            "RootFetchRing only supports hard-coded m_f values in {1, 2, 4}");
    }

    this->comType = type;
    this->id = id;
    this->logical_topo = ring_topology;
    this->data_size = data_size;
    switch (type) {
    case ComType::All_Gather:
        this->msg_size = data_size;
        this->final_data_size = data_size * nodes_in_ring;
        break;
    case ComType::All_Reduce:
        this->msg_size = data_size / nodes_in_ring;
        this->final_data_size = data_size;
        break;
    case ComType::Reduce_Scatter:
        this->msg_size = data_size / nodes_in_ring;
        this->final_data_size = data_size / nodes_in_ring;
        break;
    default:
        Sys::sys_panic("RootFetchRing received unsupported collective type");
    }
    this->name = Name::RootFetchRing;

    build_protocol();
}

void RootFetchRing::run(EventType event, CallData* data) {
    (void)data;

    if (protocol_finished) {
        return;
    }

    if (event == EventType::StreamInit) {
        advance();
    } else if (event == EventType::PacketReceived) {
        if (pending_stage_recvs <= 0) {
            return;
        }
        pending_stage_recvs--;
        if (pending_stage_recvs == 0) {
            next_stage_index++;
            advance();
        }
    }
}

void RootFetchRing::build_protocol() {
    std::vector<int> ring_order = build_canonical_ring_order();
    std::vector<FailureSpec> failures = build_failure_plan(ring_order);
    std::vector<TransferSpec> transfers = build_transfers(ring_order, failures);

    static bool printed_debug_config = false;
    if (!printed_debug_config) {
        std::cout << "[root-fetch-config] ring_size=" << nodes_in_ring
                  << " failures=" << failures.size()
                  << " transfers=" << transfers.size()
                  << " fail_before_first_round="
                  << (g_root_fetch_fail_before_first_round ? 1 : 0)
                  << std::endl;
        printed_debug_config = true;
    }

    g_root_fetch_last_failed_senders.clear();
    g_root_fetch_last_failed_receivers.clear();
    g_root_fetch_last_failure_rounds.clear();
    for (const FailureSpec& failure : failures) {
        g_root_fetch_last_failed_senders.push_back(failure.sender_rank);
        g_root_fetch_last_failed_receivers.push_back(failure.receiver_rank);
        g_root_fetch_last_failure_rounds.push_back(failure.round);
    }

    const int max_rank = *std::max_element(ring_order.begin(), ring_order.end());
    g_root_fetch_last_rank_tx.assign(static_cast<std::size_t>(max_rank + 1), 0);
    g_root_fetch_last_rank_rx.assign(static_cast<std::size_t>(max_rank + 1), 0);
    g_root_fetch_last_total_messages = 0;
    for (const TransferSpec& transfer : transfers) {
        g_root_fetch_last_rank_tx.at(transfer.src_rank) += 1;
        g_root_fetch_last_rank_rx.at(transfer.dst_rank) += 1;
        g_root_fetch_last_total_messages += 1;
    }

    build_local_stages(transfers);
}

std::vector<int> RootFetchRing::build_canonical_ring_order() const {
    std::vector<int> ring_order;
    ring_order.reserve(static_cast<std::size_t>(nodes_in_ring));

    int current = id;
    ring_order.push_back(current);
    for (int step = 1; step < nodes_in_ring; ++step) {
        current = ring_topology->get_receiver(current, direction);
        ring_order.push_back(current);
    }

    auto min_rank_it = std::min_element(ring_order.begin(), ring_order.end());
    std::rotate(ring_order.begin(), min_rank_it, ring_order.end());
    return ring_order;
}

std::vector<RootFetchRing::FailureSpec> RootFetchRing::build_failure_plan(
    const std::vector<int>& ring_order) const {
    const int default_failures = nodes_in_ring / 4;
    const int total_failures = g_root_fetch_total_failures >= 0
                                   ? std::min(g_root_fetch_total_failures,
                                              nodes_in_ring - 1)
                                   : default_failures;
    std::vector<int> sender_indices(static_cast<std::size_t>(nodes_in_ring));
    std::iota(sender_indices.begin(), sender_indices.end(), 0);

    std::mt19937 rng(g_root_fetch_failure_seed);
    std::shuffle(sender_indices.begin(), sender_indices.end(), rng);

    std::vector<FailureSpec> failures;
    failures.reserve(static_cast<std::size_t>(total_failures));
    for (int failure_index = 1; failure_index <= total_failures; ++failure_index) {
        const int sender_index = sender_indices.at(
            static_cast<std::size_t>(failure_index - 1));
        const int receiver_index = (sender_index + 1) % nodes_in_ring;
        failures.push_back(FailureSpec{
            failure_round_for(failure_index), ring_order.at(sender_index),
            ring_order.at(receiver_index), sender_index});
    }
    return failures;
}

std::vector<RootFetchRing::TransferSpec> RootFetchRing::build_transfers(
    const std::vector<int>& ring_order,
    const std::vector<FailureSpec>& failures) const {
    std::vector<TransferSpec> transfers;
    const int phase_count = comType == ComType::All_Reduce ? 2 : 1;
    const int baseline_messages = phase_count * nodes_in_ring * (nodes_in_ring - 1);
    const int repair_slack = baseline_messages * std::max(1, g_root_fetch_mf);
    transfers.reserve(static_cast<std::size_t>(baseline_messages + repair_slack));

    int transfer_id = 0;
    if (comType == ComType::All_Gather) {
        append_ring_phase_transfers(
            transfers, ring_order, failures, true, 0, transfer_id);
    } else if (comType == ComType::All_Reduce) {
        append_ring_phase_transfers(
            transfers, ring_order, failures, false, 0, transfer_id);
        append_ring_phase_transfers(
            transfers, ring_order, failures, true, nodes_in_ring - 1,
            transfer_id);
    } else {
        append_ring_phase_transfers(
            transfers, ring_order, failures, false, 0, transfer_id);
    }

    return transfers;
}

void RootFetchRing::append_ring_phase_transfers(
    std::vector<TransferSpec>& transfers,
    const std::vector<int>& ring_order,
    const std::vector<FailureSpec>& failures,
    bool all_gather_phase,
    int round_offset,
    int& transfer_id) const {
    std::unordered_map<int, FailureSpec> failures_by_link;
    for (const FailureSpec& failure : failures) {
        failures_by_link[failure.link_index] = failure;
    }

    for (int root_index = 0; root_index < nodes_in_ring; ++root_index) {
        const int root_rank = ring_order.at(root_index);
        for (int round = 1; round <= nodes_in_ring - 1; ++round) {
            const int sender_index =
                all_gather_phase ? (root_index + round - 1) % nodes_in_ring
                                 : (root_index + round) % nodes_in_ring;
            const int receiver_index = (sender_index + 1) % nodes_in_ring;
            const int sender_rank = ring_order.at(sender_index);
            const int receiver_rank = ring_order.at(receiver_index);
            const int protocol_round = round_offset + round;

            auto failure_it = failures_by_link.find(sender_index);
            if (failure_it == failures_by_link.end() ||
                failure_it->second.round > protocol_round) {
                transfers.push_back(TransferSpec{
                    sender_rank, receiver_rank, protocol_round, 0,
                    transfer_id++, false, root_rank});
                continue;
            }

            std::vector<int> query_path = build_fetch_query_path(
                receiver_rank, sender_rank, protocol_round, failures,
                ring_order);
            std::vector<int> data_path(query_path.rbegin(), query_path.rend());
            for (std::size_t hop = 0; hop + 1 < data_path.size(); ++hop) {
                transfers.push_back(TransferSpec{
                    data_path.at(hop), data_path.at(hop + 1), protocol_round,
                    static_cast<int>(hop), transfer_id++, true, root_rank});
            }
        }
    }
}

std::vector<int> RootFetchRing::build_fetch_query_path(
    int root_rank,
    int source_rank,
    int round,
    const std::vector<FailureSpec>& failures,
    const std::vector<int>& ring_order) const {
    if (g_root_fetch_mf <= 0) {
        return {root_rank};
    }

    if (g_root_fetch_mf == 1) {
        if (is_failed_link_active(root_rank, source_rank, round, failures)) {
            Sys::sys_panic(
                "RootFetchRing could not find a live 1-hop fetch path");
        }
        return {root_rank, source_rank};
    }

    std::queue<int> frontier;
    std::unordered_map<int, int> parent;
    frontier.push(root_rank);
    parent[root_rank] = -1;

    while (!frontier.empty() && parent.find(source_rank) == parent.end()) {
        const int current = frontier.front();
        frontier.pop();

        for (const int candidate : ring_order) {
            if (candidate == current ||
                parent.find(candidate) != parent.end()) {
                continue;
            }
            if (is_failed_link_active(current, candidate, round, failures)) {
                continue;
            }
            parent[candidate] = current;
            frontier.push(candidate);
            if (candidate == source_rank) {
                break;
            }
        }
    }

    if (parent.find(source_rank) == parent.end()) {
        Sys::sys_panic("RootFetchRing could not construct a live fetch path");
    }

    std::vector<int> path;
    for (int rank = source_rank; rank != -1; rank = parent.at(rank)) {
        path.push_back(rank);
    }
    std::reverse(path.begin(), path.end());
    return path;
}

int RootFetchRing::failure_round_for(int failure_index) const {
    if (g_root_fetch_fail_before_first_round) {
        return 0;
    }
    switch (g_root_fetch_quarter) {
    case QuarterPlacement::Q1:
        return failure_index;
    case QuarterPlacement::Q2:
        return (nodes_in_ring / 4) + failure_index;
    case QuarterPlacement::Q3:
        return (nodes_in_ring / 2) + failure_index;
    }
    return failure_index;
}

bool RootFetchRing::is_failed_link_active(
    int src_rank,
    int dst_rank,
    int round,
    const std::vector<FailureSpec>& failures) const {
    for (const FailureSpec& failure : failures) {
        if (failure.round > round) {
            continue;
        }
        const bool same_direction =
            failure.sender_rank == src_rank && failure.receiver_rank == dst_rank;
        const bool opposite_direction =
            failure.sender_rank == dst_rank && failure.receiver_rank == src_rank;
        if (same_direction || opposite_direction) {
            return true;
        }
    }
    return false;
}

void RootFetchRing::build_local_stages(
    const std::vector<TransferSpec>& transfers) {
    std::map<StageKey, Stage> stage_map;

    for (const TransferSpec& transfer : transfers) {
        if (transfer.src_rank != id && transfer.dst_rank != id) {
            continue;
        }

        StageKey key{transfer.round, transfer.substep};
        Stage& stage = stage_map[key];
        stage.round = transfer.round;
        stage.substep = transfer.substep;

        if (transfer.src_rank == id) {
            stage.sends.push_back(Operation{OperationKind::Send,
                                            transfer.dst_rank,
                                            transfer.transfer_id,
                                            transfer.is_fetch,
                                            transfer.root_rank});
        }
        if (transfer.dst_rank == id) {
            stage.recvs.push_back(Operation{OperationKind::Recv,
                                            transfer.src_rank,
                                            transfer.transfer_id,
                                            transfer.is_fetch,
                                            transfer.root_rank});
        }
    }

    local_stages.clear();
    local_stages.reserve(stage_map.size());
    for (const auto& entry : stage_map) {
        local_stages.push_back(entry.second);
    }
}

void RootFetchRing::advance() {
    if (protocol_finished) {
        return;
    }

    if (stream->state == StreamState::Created ||
        stream->state == StreamState::Ready) {
        stream->changeState(StreamState::Executing);
    }

    while (next_stage_index < local_stages.size()) {
        pending_stage_recvs = 0;
        issue_stage(local_stages.at(next_stage_index));
        if (pending_stage_recvs > 0) {
            return;
        }
        next_stage_index++;
    }

    protocol_finished = true;
    exit();
}

void RootFetchRing::issue_stage(const Stage& stage) {
    for (const Operation& op : stage.recvs) {
        issue_recv(op);
        pending_stage_recvs++;
    }
    for (const Operation& op : stage.sends) {
        issue_send(op);
    }
}

void RootFetchRing::issue_send(const Operation& op) {
    if (op.transfer_id >= kTagStride) {
        Sys::sys_panic("RootFetchRing transfer id exceeded tag stride");
    }

    sim_request snd_req;
    snd_req.srcRank = id;
    snd_req.dstRank = op.peer_rank;
    snd_req.tag = (stream->stream_id * kTagStride) + op.transfer_id;
    snd_req.reqType = UINT8;
    snd_req.vnet = stream->current_queue_id;
    stream->owner->front_end_sim_send(
        0, Sys::dummy_data, msg_size, UINT8, op.peer_rank, snd_req.tag, &snd_req,
        Sys::FrontEndSendRecvType::COLLECTIVE, &Sys::handleEvent, nullptr);
}

void RootFetchRing::issue_recv(const Operation& op) {
    if (op.transfer_id >= kTagStride) {
        Sys::sys_panic("RootFetchRing transfer id exceeded tag stride");
    }

    sim_request rcv_req;
    rcv_req.vnet = stream->current_queue_id;
    RecvPacketEventHandlerData* ehd = new RecvPacketEventHandlerData(
        stream, stream->owner->id, EventType::PacketReceived,
        stream->current_queue_id,
        (stream->stream_id * kTagStride) + op.transfer_id);
    stream->owner->front_end_sim_recv(
        0, Sys::dummy_data, msg_size, UINT8, op.peer_rank,
        (stream->stream_id * kTagStride) + op.transfer_id, &rcv_req,
        Sys::FrontEndSendRecvType::COLLECTIVE, &Sys::handleEvent, ehd);
}
