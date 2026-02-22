//
// WRENReceiver — NIC poller implementation
//

#include "WRENReceiver.hpp"

WRENReceiver::WRENReceiver(const RingConfig& cfg,
                           rigtorp::SPSCQueue<TimingEvent>& queue)
    : m_ethernetSocket{cfg}
    , m_queue{queue} {}

void WRENReceiver::operator()(std::stop_token stopToken) {
    std::fprintf(stderr, "[WRENReceiver] Poller thread started.\n");

    std::uint32_t spins = 0;

    while (!stopToken.stop_requested()) {
        RxFrame frame = m_ethernetSocket.tryReceive();

        if (!frame.data.empty()) {
            if (frame.data.size() >= kFrameSize) [[likely]] {
                parseAndEnqueue(frame);
                m_ethernetSocket.release();
            }
            spins = 0;
            continue;
        }

        if ((++spins & 0xFFFF) == 0 && stopToken.stop_requested()) break;
    }

    std::fprintf(stderr, "[WRENReceiver] Poller thread exiting.\n");
}

void WRENReceiver::parseAndEnqueue(const RxFrame& frame) {
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
