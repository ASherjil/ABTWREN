//
// TimingEvent — POD element for the lock-free SPSC queue between
// NIC poller (producer) and event processor (consumer).
//
// 16 bytes, trivially copyable, cache-line friendly.
//

#ifndef ABTWREN_TIMINGEVENT_HPP
#define ABTWREN_TIMINGEVENT_HPP

#include "WRENProtocol.hpp"

#include <cstdint>
#include <type_traits>
#include <atomic>

struct TimingEvent {
    PktType       pktType;    // 1B: PKT_ADVANCE or PKT_FIRE
    std::uint8_t  channel;    // 1B: pulser channel (0xFF = CTIM sentinel)
    std::uint16_t eventId;    // 2B
    std::uint16_t slot;       // 2B: LTIM slot (ADVANCE only; 0 for FIRE)
    std::uint16_t offsetMs;   // 2B: delay ms (0xFFFF = CTIM sentinel)
    std::uint32_t sec;        // 4B: TAI seconds
    std::uint32_t nsec;       // 4B: nanoseconds
};

// Use hardware cache-line alignment to avoid false sharing
// Value is 64 on x86_64, 256 on some ARM64 compiler toolchains
static constexpr std::size_t kCacheLine = std::hardware_destructive_interference_size;
struct alignas(kCacheLine) ShmEventSlot{
    std::atomic<std::uint64_t> seq;
    TimingEvent                event;
    // Implicit padding from alignas to fill the cache line
};

static_assert(std::is_trivially_copyable_v<TimingEvent>);
static_assert(sizeof(TimingEvent) == 16);
static_assert(std::is_trivially_copyable_v<ShmEventSlot>, "ShmEventSlot must be trivially copyable for ShmBackend");

static_assert(sizeof(ShmEventSlot) >= kCacheLine);
static_assert(alignof(ShmEventSlot) == kCacheLine);

#endif // ABTWREN_TIMINGEVENT_HPP
