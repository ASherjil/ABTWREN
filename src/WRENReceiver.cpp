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
