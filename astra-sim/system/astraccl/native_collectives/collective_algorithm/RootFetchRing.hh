/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ROOT_FETCH_RING_HH__
#define __ROOT_FETCH_RING_HH__

#include <cstdint>
#include <vector>

#include "astra-sim/system/astraccl/Algorithm.hh"
#include "astra-sim/system/astraccl/native_collectives/logical_topology/RingTopology.hh"

namespace AstraSim {

class RootFetchRing : public Algorithm {
  public:
    enum class QuarterPlacement { Q1, Q2, Q3 };

    RootFetchRing(ComType type,
                  int id,
                  RingTopology* ring_topology,
                  uint64_t data_size,
                  RingTopology::Direction direction,
                  InjectionPolicy injection_policy);

    void run(EventType event, CallData* data) override;

  private:
    enum class OperationKind { Send, Recv };

    struct FailureSpec {
        int round = 0;
        int sender_rank = -1;
        int receiver_rank = -1;
        int link_index = -1;
    };

    struct TransferSpec {
        int src_rank = -1;
        int dst_rank = -1;
        int round = 0;
        int substep = 0;
        int transfer_id = 0;
        bool is_fetch = false;
        int root_rank = -1;
    };

    struct Operation {
        OperationKind kind = OperationKind::Send;
        int peer_rank = -1;
        int transfer_id = 0;
        bool is_fetch = false;
        int root_rank = -1;
    };

    struct Stage {
        int round = 0;
        int substep = 0;
        std::vector<Operation> sends;
        std::vector<Operation> recvs;
    };

    void build_protocol();
    std::vector<int> build_canonical_ring_order() const;
    std::vector<FailureSpec> build_failure_plan(
        const std::vector<int>& ring_order) const;
    std::vector<TransferSpec> build_transfers(
        const std::vector<int>& ring_order,
        const std::vector<FailureSpec>& failures) const;
    void append_ring_phase_transfers(
        std::vector<TransferSpec>& transfers,
        const std::vector<int>& ring_order,
        const std::vector<FailureSpec>& failures,
        bool all_gather_phase,
        int round_offset,
        int& transfer_id) const;
    std::vector<int> build_fetch_query_path(
        int root_rank,
        int source_rank,
        int round,
        const std::vector<FailureSpec>& failures,
        const std::vector<int>& ring_order) const;
    int failure_round_for(int failure_index) const;
    bool is_failed_link_active(int src_rank,
                               int dst_rank,
                               int round,
                               const std::vector<FailureSpec>& failures) const;

    void build_local_stages(const std::vector<TransferSpec>& transfers);
    void advance();
    void issue_stage(const Stage& stage);
    void issue_send(const Operation& op);
    void issue_recv(const Operation& op);

    RingTopology* ring_topology;
    RingTopology::Direction direction;
    int nodes_in_ring;
    uint64_t msg_size;
    std::vector<Stage> local_stages;
    std::size_t next_stage_index;
    int pending_stage_recvs;
    bool protocol_finished;
};

/*
 * Experiment knobs for the root-fetch protocol simulation.
 *
 * Supported values:
 *   g_root_fetch_mf       in {1, 2, 4}
 *   g_root_fetch_quarter  in {Q1, Q2, Q3}
 *
 * Failures are generated as T random ring links using the seed below.
 * If g_root_fetch_total_failures <= 0, we default to T = N/4.
 * Update these globals before rebuilding if you want a different scenario.
 */
extern int g_root_fetch_mf;
extern RootFetchRing::QuarterPlacement g_root_fetch_quarter;
extern int g_root_fetch_total_failures;
extern std::uint32_t g_root_fetch_failure_seed;
extern bool g_root_fetch_fail_before_first_round;

extern std::vector<std::uint64_t> g_root_fetch_last_rank_tx;
extern std::vector<std::uint64_t> g_root_fetch_last_rank_rx;
extern std::uint64_t g_root_fetch_last_total_messages;
extern std::vector<int> g_root_fetch_last_failed_senders;
extern std::vector<int> g_root_fetch_last_failed_receivers;
extern std::vector<int> g_root_fetch_last_failure_rounds;

}  // namespace AstraSim

#endif /* __ROOT_FETCH_RING_HH__ */
