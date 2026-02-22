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
    inline void parseAndEnqueue(const RxFrame& frame){
        const auto* p = frame.data.data() + kEthHdrLen;

        TimingEvent ev{};

        if (p[0] == PKT_ADVANCE) {
            ev.pktType = PKT_ADVANCE;
            ev.channel = 0;
            std::memcpy(&ev.eventId, p + 2,  2);
            std::memcpy(&ev.slot,    p + 4,  2);
            std::memcpy(&ev.sec,     p + 6,  4);
            std::memcpy(&ev.nsec,    p + 10, 4);
            ev.offsetMs = 0;
        }
        else if (p[0] == PKT_FIRE) {
            ev.pktType = PKT_FIRE;
            std::memcpy(&ev.eventId,  p + 2,  2);
            std::memcpy(&ev.sec,      p + 4,  4);
            std::memcpy(&ev.nsec,     p + 8,  4);
            ev.channel = p[12];
            ev.slot = 0;
            std::memcpy(&ev.offsetMs, p + 14, 2);
        }
        else {
            return; // Unknown packet type — drop silently
        }

        if (!m_queue.try_push(ev)) [[unlikely]]
            std::fprintf(stderr, "[WRENReceiver] Queue full — dropped event %u\n", ev.eventId);
    }
};

#endif // ABTWREN_WRENRECEIVER_H
