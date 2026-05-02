//
// MainReceiver — Orchestrator: owns WRENReceiver (producer),
// EventProcessor (consumer), and the jthreads connecting them.
//
// Thread architecture:
//   jthread #1: WRENReceiver::operator()   — Core pollerCore,    SCHED_FIFO:49
//   jthread #2: EventProcessor::operator() — Core processorCore, SCHED_FIFO:48
//

#ifndef ABTWREN_MAINRECEIVER_HPP
#define ABTWREN_MAINRECEIVER_HPP

#include "EventProcessor.hpp"
#include "TimingEvent.hpp"
#include "WRENReceiver.hpp"
#include "QueueSink.hpp"

#include <RingConcepts.hpp>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <thread>
#include <sys/mman.h>

template<RxRing Rx, typename Sink>
class MainReceiver{
    static constexpr bool kQueueMode = std::is_same_v<Sink, QueueSink>;
public:
    MainReceiver(const char* iface, int pollerCore,
                 Rx& ethernetSocket, Sink& sink,
                 EventProcessor* processor = nullptr, int processorCore =-1)
        :m_pollerCore{pollerCore}, m_processorCore{processorCore},
         m_receiver{ethernetSocket, sink}, m_processor{processor} {

        if constexpr (kQueueMode) {
            std::fprintf(stderr, "[MainReceiver] %s (poller=core%d, processor=core%d, queue-mode)\n",
                iface, pollerCore, processorCore);
        }
        else {
            std::fprintf(stderr, "[MainReceiver] %s (poller=core%d, direct-shm)\n",
                iface, pollerCore);
        }
    }

    MainReceiver(const MainReceiver&) = delete;
    MainReceiver(MainReceiver&&) = delete;
    MainReceiver& operator=(const MainReceiver&) = delete;
    MainReceiver& operator=(MainReceiver&&) = delete;

    ~MainReceiver() {
        requestStop();
        wait();
    }

    void start() {
        if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
            std::fprintf(stderr, "[Warn] mlockall failed: %s\n", std::strerror(errno));

        m_pollerThread = std::jthread(std::ref(m_receiver));
        pinThread(m_pollerThread, m_pollerCore, 49);

        if constexpr (kQueueMode) {
            m_processorThread = std::jthread(std::ref(*m_processor));
            pinThread(m_processorThread, m_processorCore, 48);
            std::fprintf(stderr, "[MainReceiver] Both threads started.\n");
        } else {
            std::fprintf(stderr, "[MainReceiver] Poller thread started.\n");
        }
    }

    void requestStop() {
        m_pollerThread.request_stop();
        if constexpr (kQueueMode) {
            m_processorThread.request_stop();
        }
    }

    void wait() {
        if (m_pollerThread.joinable()) m_pollerThread.join();
        if constexpr (kQueueMode) {
            if (m_processorThread.joinable()) m_processorThread.join();
        }
        std::fprintf(stderr, "[MainReceiver] All threads joined.\n");
    }
private:
    int m_pollerCore;
    int m_processorCore;

    WRENReceiver<Rx, Sink>  m_receiver;
    EventProcessor*          m_processor;

    std::jthread m_pollerThread;
    std::jthread m_processorThread;

    static void pinThread(std::jthread& t, int core, int priority) {
        const pthread_t handle = t.native_handle();
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
};

#endif // ABTWREN_MAINRECEIVER_HPP
