/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/system/DataSet.hh"

#include "astra-sim/system/IntData.hh"
#include "astra-sim/system/Sys.hh"

using namespace AstraSim;

uint64_t DataSet::id_auto_increment = 0;

DataSet::DataSet(int total_streams) {
    this->my_id = id_auto_increment++;
    this->total_streams = total_streams;
    this->finished_streams = 0;
    this->finished = false;
    this->finish_tick = 0;
    this->active = true;
    this->creation_tick = Sys::boostedTick();
    this->notifier_callable = nullptr;
}

void DataSet::set_notifier(Callable* callable, EventType event) {
    notifier_callable = callable;
    notifier_event = event;
}

void DataSet::notify_stream_finished(StreamStat* data) {
    finished_streams++;
    if (data != nullptr) {
        update_stream_stats(data);
    }
    if (finished_streams == total_streams) {
        finished = true;
        finish_tick = Sys::boostedTick();
        if (notifier_callable != nullptr) {
            take_stream_stats_average();
            Callable* c = notifier_callable;
            EventType ev = notifier_event;
            // Stack object: the callee (Workload::call) only reads the
            // payload during the call and never retains it (it was deleted
            // right after the call here anyway).
            IntData int_data(my_id);
            int_data.execution_time = finish_tick - creation_tick;
            c->call(ev, &int_data);
        }
    }
}

void DataSet::call(EventType event, CallData* data) {
    notify_stream_finished(((StreamStat*)data));
}
