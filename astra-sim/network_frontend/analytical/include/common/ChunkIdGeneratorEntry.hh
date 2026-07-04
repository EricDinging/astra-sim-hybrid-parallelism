/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

namespace AstraSimAnalytical {

/**
 * ChunkIdGeneratorEntry tracks the chunk id generated
 * for sim_send() and sim_recv() calls
 * per each (tag, src, dest, chunk_size) tuple.
 */
class ChunkIdGeneratorEntry {
  public:
    /**
     * Constructur.
     */
    ChunkIdGeneratorEntry() noexcept;

    /**
     * Get the chunk id for sim_send() call.
     *
     * @return chunk id for sim_send() call
     */
    [[nodiscard]] int get_send_id() const noexcept;

    /**
     * Get the chunk id for sim_recv() call.
     *
     * @return chunk id for sim_recv() call
     */
    [[nodiscard]] int get_recv_id() const noexcept;

    /**
     * Increment the chunk id for sim_send() call.
     */
    void increment_send_id() noexcept;

    /**
     * Increment the chunk id for sim_recv() call.
     */
    void increment_recv_id() noexcept;

    /**
     * Record that one chunk of this entry fully retired (both send and recv
     * callbacks fired and the tracker entry was popped).
     */
    void increment_completed() noexcept;

    /**
     * True when every issued chunk has retired and sends/recvs are balanced:
     * the entry can be erased, and a later reuse of the same
     * (tag, src, dest, chunk_size) key restarts both sides symmetrically.
     */
    [[nodiscard]] bool fully_retired() const noexcept;

  private:
    /// current available chunk id for sim_send() call
    int send_id;

    /// current available chunk id for sim_recv() call
    int recv_id;

    /// number of chunks of this entry that fully completed
    int completed;
};

}  // namespace AstraSimAnalytical
