/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __MY_PACKET_HH__
#define __MY_PACKET_HH__

#include "astra-sim/system/Callable.hh"
#include "astra-sim/system/Common.hh"

namespace AstraSim {

class MyPacket : public Callable {
  public:
    MyPacket(int preferred_vnet, int preferred_src, int preferred_dest);
    MyPacket(uint64_t msg_size,
             int preferred_vnet,
             int preferred_src,
             int preferred_dest);
    void set_notifier(Callable* c);
    void call(EventType event, CallData* data);

    // Default-initialized: the ctors only set the preferred_* fields and
    // msg_size, so the rest used to start as indeterminate garbage (UB to
    // read; byte-identical runs confirm nothing depended on the old values).
    int cycles_needed = 0;
    int fm_id = 0;
    int stream_id = 0;
    Callable* notifier = nullptr;
    Callable* sender = nullptr;
    int preferred_vnet = 0;
    int preferred_dest = 0;
    int preferred_src = 0;
    uint64_t msg_size = 0;
    Tick ready_time = 0;
};

}  // namespace AstraSim

#endif /* __MY_PACKET_HH__ */
