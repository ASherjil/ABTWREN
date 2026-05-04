//
// Created by asherjil on 5/4/26.
//

#ifndef ABTWREN_ZYNQULTRASCALESINK_HPP
#define ABTWREN_ZYNQULTRASCALESINK_HPP

#include "TimingEvent.hpp"
#include "backends/AXIBackend.hpp"
#include "test/dummyRegisters.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fmt/core.h>
#include <thread>

/**
 * @class This class is only for testing the latency via a ZynqUltraScale+ SoC
 * where it simply writes to a certain registers to generate an interrupt(square
 * pulse) when a certain LTIM arrives.
 */
class ZynqUltraScaleSink {
public:
    ZynqUltraScaleSink(const TimingEvent& matchEvent,
                       std::chrono::microseconds pulseWidth,
                       std::size_t physicalAddressOfBlock = fpga::regs::dummy_hw_desc::DUMMY_PHYSICAL_ADDRESS)
        : m_matchEvent{matchEvent}
        , m_pulseWidth{pulseWidth} {

        if (!m_axiBusHandle.open("/dev/mem",
                                 physicalAddressOfBlock, 0x1000 /*4KB is enough*/)) {
            std::perror("Failed to open /dev/mem");
            std::abort();
        }
    }

    /// Called from WRENReceiver::parseAndEnqueue — generates a square pulse
    /// on the DIOT register when the arriving event matches the configured one.
    [[gnu::hot, gnu::always_inline]]
    void push(const TimingEvent& ev) {
        if ((ev.eventId == m_matchEvent.eventId) && (ev.pktType == m_matchEvent.pktType) && (ev.channel != m_matchEvent.channel)){
            triggerInterrupt();
        }

        if constexpr (kDebugVerbose) {
            fmt::println(stderr, "[ZynqSink] id={} type={} ch={}", ev.eventId, static_cast<int>(ev.pktType), ev.channel);
        }
    }

private:
    [[gnu::always_inline, gnu::hot]]
    void triggerInterrupt() {
        *m_axiBusHandle.registerPtr<std::uint32_t>(fpga::regs::dummy_hw_desc::MEAS_JITTER) = 1;

        if (m_pulseWidth.count() > 0) {
            std::this_thread::sleep_for(m_pulseWidth);
        }

        *m_axiBusHandle.registerPtr<std::uint32_t>(fpga::regs::dummy_hw_desc::MEAS_JITTER) = 0;
    }

    TimingEvent                    m_matchEvent;
    std::chrono::microseconds      m_pulseWidth;
    AXIBackend                     m_axiBusHandle;
};

#endif // ABTWREN_ZYNQULTRASCALESINK_HPP
