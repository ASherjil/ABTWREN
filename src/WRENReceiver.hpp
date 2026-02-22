//
// WRENReceiver — NIC poller (producer): busy-polls packet_mmap,
// parses frames, pushes TimingEvent into SPSC queue.
//

#ifndef ABTWREN_WRENRECEIVER_H
#define ABTWREN_WRENRECEIVER_H

#include "TimingEvent.hpp"
#include "WRENProtocol.hpp"

#include <PacketMmapRx.hpp>
#include <rigtorp/SPSCQueue.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stop_token>

class WRENReceiver {
public:
    WRENReceiver(const RingConfig& cfg, rigtorp::SPSCQueue<TimingEvent>& queue);

    WRENReceiver(const WRENReceiver&) = delete;
    WRENReceiver(WRENReceiver&&) = delete;
    WRENReceiver& operator=(const WRENReceiver&) = delete;
    WRENReceiver& operator=(WRENReceiver&&) = delete;

    // Thread entry point — busy-polls NIC, pushes events into queue.
    void operator()(std::stop_token stopToken);

private:
    PacketMmapRx                     m_ethernetSocket;
    rigtorp::SPSCQueue<TimingEvent>& m_queue;

    [[gnu::always_inline]]
    inline void parseAndEnqueue(const RxFrame& frame);
};

#endif // ABTWREN_WRENRECEIVER_H
