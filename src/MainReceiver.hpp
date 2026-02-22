//
// MainReceiver — Orchestrator: owns SPSC queue, WRENReceiver (producer),
// EventProcessor (consumer), and the two jthreads connecting them.
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

#include <rigtorp/SPSCQueue.h>

#include <cstddef>
#include <thread>

class MainReceiver {
public:
    MainReceiver(const char* interface, int pollerCore, int processorCore,
                 std::size_t queueCapacity);

    // Spawn both jthreads with core pinning and RT priority.
    void start();

    // Signal both threads to stop (signal-safe).
    void requestStop();

    // Join both threads.
    void wait();

    ~MainReceiver();

    MainReceiver(const MainReceiver&) = delete;
    MainReceiver(MainReceiver&&) = delete;
    MainReceiver& operator=(const MainReceiver&) = delete;
    MainReceiver& operator=(MainReceiver&&) = delete;

private:
    const char* m_interface;
    int         m_pollerCore;
    int         m_processorCore;

    rigtorp::SPSCQueue<TimingEvent> m_queue;
    WRENReceiver                    m_receiver;
    EventProcessor                  m_processor;

    std::jthread m_pollerThread;
    std::jthread m_processorThread;

    static void pinThread(std::jthread& t, int core, int priority);
};

#endif // ABTWREN_MAINRECEIVER_HPP
