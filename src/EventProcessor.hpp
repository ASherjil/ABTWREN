//
// EventProcessor — Consumer thread: pops TimingEvent from SPSC queue,
// prints debug logs (extension point for future FESA real-time actions).
//

#ifndef ABTWREN_EVENTPROCESSOR_HPP
#define ABTWREN_EVENTPROCESSOR_HPP

#include "TimingEvent.hpp"

#include <rigtorp/SPSCQueue.h>
#include <stop_token>

class EventProcessor {
public:
    explicit EventProcessor(rigtorp::SPSCQueue<TimingEvent>& queue);

    EventProcessor(const EventProcessor&) = delete;
    EventProcessor(EventProcessor&&) = delete;
    EventProcessor& operator=(const EventProcessor&) = delete;
    EventProcessor& operator=(EventProcessor&&) = delete;

    // Thread entry point — busy-polls queue, drains remaining items after stop.
    void operator()(std::stop_token stopToken);
private:
    rigtorp::SPSCQueue<TimingEvent>& m_queue;

    // TODO: With actual processing make this function [[gnu::always_inline]] and inline void
    void processEvent(const TimingEvent& ev);
};

#endif // ABTWREN_EVENTPROCESSOR_HPP
