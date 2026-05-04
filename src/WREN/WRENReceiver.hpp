//
// WRENReceiver — NIC poller (producer): busy-polls NIC transport,
// parses frames, pushes TimingEvent into SPSC queue or ShmSink.
//

#ifndef ABTWREN_WRENRECEIVER_H
#define ABTWREN_WRENRECEIVER_H

#include "TimingEvent.hpp"
#include "WRENProtocol.hpp"
#include "fmt/ostream.h"

#include <RingConcepts.hpp>
#include <cstdio>
#include <cstring>
#include <stop_token>

template<RxRing Rx, typename Sink>
class WRENReceiver {
public:
    WRENReceiver(Rx& ethernetSocket, Sink& sink);

    WRENReceiver(const WRENReceiver&) = delete;
    WRENReceiver(WRENReceiver&&) = delete;
    WRENReceiver& operator=(const WRENReceiver&) = delete;
    WRENReceiver& operator=(WRENReceiver&&) = delete;

    void operator()(std::stop_token stopToken);

private:
    Rx&     m_ethernetSocket;
    Sink&   m_sink;

    [[gnu::always_inline]]
    inline void parseAndEnqueue(const RxFrame& frame);
};

// ── Member function definitions ───────────────────────────────────

template<RxRing Rx, typename Sink>
WRENReceiver<Rx, Sink>::WRENReceiver(Rx& ethernetSocket, Sink& sink)
    : m_ethernetSocket{ethernetSocket}, m_sink{sink} {}

template<RxRing Rx, typename Sink>
void WRENReceiver<Rx, Sink>::operator()(std::stop_token stopToken) {
    std::fprintf(stderr, "[WRENReceiver] Poller thread started.\n");

    while (!stopToken.stop_requested()) {
        RxFrame frame = m_ethernetSocket.tryReceive();

        if (!frame.data.empty()) [[unlikely]] {
            if (frame.data.size() >= kFrameSize) [[likely]] {
                parseAndEnqueue(frame);
                m_ethernetSocket.release();
            }
        }
    }
    std::fprintf(stderr, "[WRENReceiver] Poller thread exiting.\n");
}

template<RxRing Rx, typename Sink>
inline void WRENReceiver<Rx, Sink>::parseAndEnqueue(const RxFrame& frame) {
    const auto* p = frame.data.data() + kEthHdrLen;

    TimingEvent ev{};

    if constexpr (kDebugVerbose) {
        // Debug: full classification — handles both ADVANCE and FIRE packets
        if (p[0] == PKT_ADVANCE) {
            ev.pktType = PKT_ADVANCE;
            ev.channel = 0;
            std::memcpy(&ev.eventId, p + 2,  2);
            std::memcpy(&ev.slot,    p + 4,  2);
            std::memcpy(&ev.sec,     p + 6,  4);
            std::memcpy(&ev.nsec,    p + 10, 4);
            ev.offsetMs = 0;
        } else if (p[0] == PKT_FIRE) {
            ev.pktType = PKT_FIRE;
            std::memcpy(&ev.eventId,  p + 2,  2);
            std::memcpy(&ev.sec,      p + 4,  4);
            std::memcpy(&ev.nsec,     p + 8,  4);
            ev.channel = p[12];
            ev.slot = 0;
            std::memcpy(&ev.offsetMs, p + 14, 2);
        } else {
            fmt::println("[Error] [WRENReceiver] Unidentified WREN packet received function returning early.");
            return;
        }
    } else {
        // Release: LTIM-only — PKT_FIRE direct path, no branch overhead
        ev.pktType = PKT_FIRE;
        std::memcpy(&ev.eventId,  p + 2,  2);
        std::memcpy(&ev.sec,      p + 4,  4);
        std::memcpy(&ev.nsec,     p + 8,  4);
        ev.channel = p[12];
        std::memcpy(&ev.offsetMs, p + 14, 2);
    }

    m_sink.push(ev);
}

#endif // ABTWREN_WRENRECEIVER_H
