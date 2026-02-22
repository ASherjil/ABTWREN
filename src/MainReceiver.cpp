//
// MainReceiver — Orchestrator implementation
//

#include "MainReceiver.hpp"
#include "WRENProtocol.hpp"

#include <NicTuner.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>

MainReceiver::MainReceiver(const char* interface, int pollerCore,
                           int processorCore, std::size_t queueCapacity)
    : m_interface{interface}
    , m_pollerCore{pollerCore}
    , m_processorCore{processorCore}
    , m_queue{queueCapacity}
    , m_receiver{[&] {
          RingConfig cfg{};
          cfg.interface   = interface;
          cfg.direction   = RingDirection::RX;
          cfg.blockSize   = 4096;
          cfg.blockNumber = 64;
          cfg.protocol    = kEtherType;
          cfg.hwTimeStamp = false;
          return cfg;
      }(), m_queue}
    , m_processor{m_queue}
{
    // NicTuner: disable coalescing, offloads, RT throttling,
    // pin NIC IRQs to pollerCore, boost ksoftirqd to FIFO:50.
    // Destructor only closes ethtool fd — all settings persist until reboot.
    NicTuner tuner(interface, pollerCore);

    std::fprintf(stderr, "[MainReceiver] Constructed on %s "
                 "(poller=core%d, processor=core%d, queue=%zu)\n",
                 interface, pollerCore, processorCore, queueCapacity);
}

void MainReceiver::start() {
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        std::fprintf(stderr, "[Warn] mlockall failed: %s\n", std::strerror(errno));

    m_pollerThread = std::jthread(std::ref(m_receiver));
    pinThread(m_pollerThread, m_pollerCore, 49);       // SCHED_FIFO:49, below ksoftirqd

    m_processorThread = std::jthread(std::ref(m_processor));
    pinThread(m_processorThread, m_processorCore, 48); // SCHED_FIFO:48

    std::fprintf(stderr, "[MainReceiver] Both threads started.\n");
}

void MainReceiver::requestStop() {
    m_pollerThread.request_stop();
    m_processorThread.request_stop();
}

void MainReceiver::wait() {
    if (m_pollerThread.joinable())    m_pollerThread.join();
    if (m_processorThread.joinable()) m_processorThread.join();
    std::fprintf(stderr, "[MainReceiver] Both threads joined.\n");
}

MainReceiver::~MainReceiver() {
    requestStop();
    wait();
}

void MainReceiver::pinThread(std::jthread& t, int core, int priority) {
    auto handle = t.native_handle();

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    if (pthread_setaffinity_np(handle, sizeof(cpuset), &cpuset) != 0)
        std::fprintf(stderr, "[Warn] Failed to pin to core %d: %s\n",
                     core, std::strerror(errno));

    sched_param sp{};
    sp.sched_priority = priority;
    if (pthread_setschedparam(handle, SCHED_FIFO, &sp) != 0)
        std::fprintf(stderr, "[Warn] SCHED_FIFO:%d failed: %s\n",
                     priority, std::strerror(errno));
}
