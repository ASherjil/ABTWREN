//
// EventProcessor — Consumer thread: pops TimingEvent from SPSC queue,
// prints debug logs (extension point for future FESA real-time actions).
//

#ifndef ABTWREN_EVENTPROCESSOR_HPP
#define ABTWREN_EVENTPROCESSOR_HPP

#include "TimingEvent.hpp"
#include "backends/ShmBackend.hpp"

#include <cstdio>
#include <rigtorp/SPSCQueue.h>
#include <stop_token>

class EventProcessor {
public:
    explicit EventProcessor(const char* shmName, rigtorp::SPSCQueue<TimingEvent>& queue);

    EventProcessor(const EventProcessor&) = delete;
    EventProcessor(EventProcessor&&) = delete;
    EventProcessor& operator=(const EventProcessor&) = delete;
    EventProcessor& operator=(EventProcessor&&) = delete;
    ~EventProcessor() = default;
    // Thread entry point — busy-polls queue, drains remaining items after stop.
    void operator()(std::stop_token stopToken);
private:

    [[gnu::always_inline]]
    inline void processEvent(const TimingEvent& ev) {
        m_sharedMemoryRegion.ptr<ShmEventSlot>()->event = ev;
        m_sharedMemoryRegion.ptr<ShmEventSlot>()->seq.store(++m_seq, std::memory_order_release);

        // TODO: Remove this for final production but keep for now, they are usefull debugs
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

    rigtorp::SPSCQueue<TimingEvent>& m_queue;// Lock-free SPSC queue for popping items coming from the Rx
    ShmBackend<ShmMode::Writer> m_sharedMemoryRegion;
    std::uint64_t    m_seq{};// local copy of sequence counter
};

#endif // ABTWREN_EVENTPROCESSOR_HPP
