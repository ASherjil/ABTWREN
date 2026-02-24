//
// EventProcessor — Consumer thread implementation
//

#include "EventProcessor.hpp"

#include <cstdio>
#include <fcntl.h>       // O_CREAT, O_RDWR
#include <sys/mman.h>    // shm_open, mmap, MAP_SHARED
#include <unistd.h>      // ftruncate, close

EventProcessor::EventProcessor(const char* shmName, rigtorp::SPSCQueue<TimingEvent>& queue)
    : m_queue{queue}, m_shmName{shmName} {

    //1. Create/open the shared memory object
    m_shmFd = ::shm_open(m_shmName.c_str(), O_CREAT | O_RDWR, 0666);
    // O_CREAT -> create if it doesn't exist
    // 0666 -> Both processes can read/write

    // 2. Set size to exactly one cache line, 64 bytes
    ::ftruncate(m_shmFd, sizeof(ShmEventSlot));

    // 3. Perform the mmap into our address space and store the pointer
    void* ptr = ::mmap(
        nullptr,
        sizeof(ShmEventSlot),
        PROT_READ | PROT_WRITE,
        MAP_SHARED, // visible to other processes
        m_shmFd,
        0);

    m_shmSlot = static_cast<ShmEventSlot*>(ptr); // store as member variable

    // 4. Initialise the atomic seq to 0, atomic store cannot be used on uninitialised memory
    new (&m_shmSlot -> seq) std::atomic<std::uint64_t>(0);
}

EventProcessor::~EventProcessor() {
    ::munmap(m_shmSlot, sizeof(ShmEventSlot)); // undo the mmap
    ::close(m_shmFd); // close the file descriptor
    ::shm_unlink(m_shmName.c_str()); // delete the /dev/shm/abtwren_events
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
