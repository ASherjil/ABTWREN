//
// EventProcessor — Consumer thread implementation
//

#include "EventProcessor.hpp"

#include <cstdio>

EventProcessor::EventProcessor(rigtorp::SPSCQueue<TimingEvent>& queue)
    : m_queue{queue} {}

void EventProcessor::operator()(std::stop_token stopToken) {
    std::fprintf(stderr, "[EventProcessor] Consumer thread started.\n");

    while (!stopToken.stop_requested()) {
        auto* ev = m_queue.front();
        if (ev) {
            processEvent(*ev);
            m_queue.pop();
        }
    }

    // Drain remaining items after stop is requested
    while (auto* ev = m_queue.front()) {
        processEvent(*ev);
        m_queue.pop();
    }

    std::fprintf(stderr, "[EventProcessor] Consumer thread exiting.\n");
}

void EventProcessor::processEvent(const TimingEvent& ev) {
    if (ev.pktType == PKT_ADVANCE) {
        std::printf("ADVANCE  ev_id:%u  slot:%u  due:%u.%09u\n",
                    ev.eventId, ev.slot, ev.sec, ev.nsec);
    }
    else if (ev.pktType == PKT_FIRE) {
        if (ev.channel == 0xFF)
            std::printf("CTIM_FIRE  ev_id:%u  at:%u.%09u\n",
                        ev.eventId, ev.sec, ev.nsec);
        else
            std::printf("LTIM_FIRE  ev_id:%u  ch:%u  +%ums  at:%u.%09u\n",
                        ev.eventId, ev.channel, ev.offsetMs, ev.sec, ev.nsec);
    }
}
