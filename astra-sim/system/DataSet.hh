/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __DATASET_HH__
#define __DATASET_HH__

#include "astra-sim/system/CallData.hh"
#include "astra-sim/system/Callable.hh"
#include "astra-sim/system/Common.hh"
#include "astra-sim/system/StreamStat.hh"

namespace AstraSim {

class DataSet : public Callable, public StreamStat {
  public:
    DataSet(int total_streams);
    void set_notifier(Callable* layer, EventType event);
    void notify_stream_finished(StreamStat* data);
    void call(EventType event, CallData* data);

    // 64-bit: incremented once per collective per rank for the whole
    // process; a plain int plausibly wraps on multi-day large-cluster runs.
    static uint64_t id_auto_increment;
    uint64_t my_id;
    int total_streams;
    int finished_streams;
    bool finished;
    bool active;
    Tick finish_tick;
    Tick creation_tick;
    // Inline notifier (a non-null callable means "set"), replacing a
    // heap-allocated std::pair allocated once per collective.
    Callable* notifier_callable = nullptr;
    EventType notifier_event{};
};

}  // namespace AstraSim

#endif /* __DATASET_HH__ */
