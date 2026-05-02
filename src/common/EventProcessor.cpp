//
// EventProcessor — Consumer thread implementation
//

#include "EventProcessor.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

EventProcessor::EventProcessor(const char* shmName, rigtorp::SPSCQueue<TimingEvent>& queue)
    : m_queue{queue}{

    if (m_sharedMemoryRegion.open<ShmEventSlot>(shmName, &ShmEventSlot::seq)) {
        std::fprintf(stderr, "[EventProcessor] Shared memory opened OK\n");
    }
    else {
        std::fprintf(stderr, "[EventProcessor] FATAL: shared memory open failed: %s\n",
                     std::strerror(errno));
        std::abort();
    }
}

void EventProcessor::operator()(std::stop_token stopToken) {
    std::fprintf(stderr, "[EventProcessor] Consumer thread started.\n");

    while (!stopToken.stop_requested()) {
        auto* ev = m_queue.front();
        if (ev)[[unlikely]] {
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
