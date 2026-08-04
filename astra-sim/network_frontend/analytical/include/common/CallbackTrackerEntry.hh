/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include <astra-network-analytical/common/Event.h>
#include <astra-network-analytical/common/Type.h>
#include <optional>

using namespace NetworkAnalytical;

namespace AstraSimAnalytical {

class ChunkIdGeneratorEntry;

/**
 * CallbackTrackerEntry manages sim_send() and sim_recv() callbacks
 * per each unique chunk.
 */
class CallbackTrackerEntry {
  public:
    /**
     * Constructor.
     */
    CallbackTrackerEntry() noexcept;

    /**
     * Register a callback for sim_send() call.
     *
     * @param callback callback function pointer
     * @param arg argument of the callback function
     */
    void register_send_callback(Callback callback, CallbackArg arg) noexcept;

    /**
     * Register a callback for sim_recv() call.
     *
     * @param callback callback function pointer
     * @param arg argument of the callback function
     */
    void register_recv_callback(Callback callback, CallbackArg arg) noexcept;

    /**
     * Check if the transmission of the chunk is finished.
     *
     * @return true if the transmission of the chunk is alread finished,
     *         false otherwise
     */
    [[nodiscard]] bool is_transmission_finished() const noexcept;

    /**
     * Mark the transmission of the chunk as finished.
     */
    void set_transmission_finished() noexcept;

    /**
     * Check the existence of both sim_send() and sim_recv() callbacks.
     *
     * @return true if both sim_send() and sim_recv() callbacks are registered,
     *         false otherwise
     */
    [[nodiscard]] bool both_callbacks_registered() const noexcept;

    /**
     * Invoke the sim_send() callback.
     */
    void invoke_send_handler() noexcept;

    /**
     * Invoke the sim_recv() callback.
     */
    void invoke_recv_handler() noexcept;

    /**
     * Cache the chunk-id-generator entry backing this chunk's
     * (tag, src, dest, chunk_size) key, so the completion path can retire
     * the chunk without re-hashing the generator map. Generator entries are
     * stable under rehash and outlive every in-flight chunk of their key.
     *
     * @param entry pointer to the generator entry
     */
    void set_generator_entry(ChunkIdGeneratorEntry* entry) noexcept;

    /**
     * Get the cached chunk-id-generator entry.
     *
     * @return pointer to the generator entry
     */
    [[nodiscard]] ChunkIdGeneratorEntry* get_generator_entry() const noexcept;

  private:
    /// sim_send() callback event
    std::optional<Event> send_event;

    /// sim_recv() callback event
    std::optional<Event> recv_event;

    /// true if the transmission of the chunk is already finished, false
    /// otherwise
    bool transmission_finished;

    /// chunk-id-generator entry of this chunk's key (for probe-free retire)
    ChunkIdGeneratorEntry* generator_entry;
};

}  // namespace AstraSimAnalytical
