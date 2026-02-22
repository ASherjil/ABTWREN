//
// Created by asherjil on 2/15/26.
//

#ifndef ABTWREN_WRENTRANSMITTER_H
#define ABTWREN_WRENTRANSMITTER_H

#include "WRENProtocol.hpp"
#include "WRENCTIMConfigurator.hpp"
#include <PCIeBackend.hpp>
#include <PacketMmapTx.hpp>

#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

constexpr int kMaxSlots  = 64;
constexpr int kMaxComp   = 512;
constexpr int kMaxActIdx = 2048;  // firmware MAX_RX_ACTIONS (covers CTIM entries at 2040+)

/// Metadata cached per sw_cmp_idx. Populated at CONFIG time from the action map,
/// read at PULSE time to stamp enriched data into the wire packet.
/// Defaults are sentinels: channel=0xFF, offsetMs=0xFFFF → "unresolved / CTIM fire".
struct CompEntry {
    std::uint16_t eventId{};
    std::uint8_t  channel{0xFF};     // 1-based wrentest channel (0xFF = no channel)
    std::uint16_t offsetMs{0xFFFF};  // delay in ms (0xFFFF = no offset / CTIM)
};

struct LtimTarget {
    std::uint16_t event_id;
    std::uint16_t ltim_slot;
    std::uint64_t delay_ns;
};

// From: wrentest ltim list -a + timing-domain-cern event IDs
constexpr auto kTargets = std::to_array<LtimTarget>({
    {142, 23,   50'000'000},  // PIX.AMCLO-CT  -> slot 23, 50ms
    {143, 20,   10'000'000},  // PIX.F900-CT   -> slot 20, 10ms
    {143, 21,  100'000'000},  // PIX.F900-CT   -> slot 21, 100ms
    {143, 22,  900'000'000},  // PIX.F900-CT   -> slot 22, 900ms
    {138, 21,  100'000'000},  // PI2X.F900-CT  -> slot 21, 100ms
    {138, 22,  900'000'000},  // PI2X.F900-CT  -> slot 22, 900ms
});

class WRENTransmitter {
public:
    explicit WRENTransmitter(std::uint16_t vendorID, std::uint16_t deviceID, int bar, const RingConfig& cfg);

    WRENTransmitter(const WRENTransmitter&) = delete;
    WRENTransmitter(WRENTransmitter&&) noexcept = default;

    WRENTransmitter& operator=(const WRENTransmitter&) = delete;
    WRENTransmitter& operator=(WRENTransmitter&&) noexcept = default;

    ~WRENTransmitter() = default;

    /// Access the PCIeBackend for shared use (e.g., WRENCTIMConfigurator).
    [[nodiscard]] PCIeBackend& pcie() { return m_pcieHandler; }

    /// Install the discovered action map into a flat array indexed by act_idx.
    /// Called once at startup before transmitAll(). CONFIG handler then does
    /// a single array copy: m_compInfo[swCmp] = m_actMeta[actIdx].
    void installActionMap(const std::vector<ActionInfo>& map);

    // Ethernet frame layout: [dst:0-5][src:6-11][ethertype:12-13][payload:14+]
    void setMacAddresses(const std::array<std::uint8_t, 6>& src, const std::array<std::uint8_t, 6>& dst);

    // ── Ring buffer helpers ──────────────────────────────────────

    // Single 32-bit read with index wrapping (fallback for ring boundary)
    [[gnu::always_inline]]
    inline std::uint32_t readRingWord(std::uint32_t idx) const {
        return *m_pcieHandler.registerPtr<std::uint32_t>(
            WREN_ASYNC_DATA_BASE + (idx & RING_MASK) * 4);
    }

    // Returns matching LTIM slots for a given CTIM event_id.
    // One CTIM can trigger multiple slots (e.g., PIX.F900-CT -> 20, 21, 22).
    // NOTE: returned pointers valid only until the next call (static storage).
    [[nodiscard, gnu::always_inline]]
    inline std::pair<const LtimTarget*, int> resolveCtimToSlots(std::uint16_t evId) {
        static std::array<LtimTarget, kTargets.size()> hits;
        int n = 0;
        for (const auto& t : kTargets) {
            if (t.event_id == evId)
                hits[static_cast<std::size_t>(n++)] = t;
        }
        return {hits.data(), n};
    }

    // ── Packet builders (zero-copy: write directly to TX ring) ───
    //
    // The TX ring is pre-stamped at startup with:
    //   [dst:6][src:6][ethertype:2][zeros:50] = 64 bytes per slot
    // Hot path only overwrites the payload bytes that change.
    //
    // Payload layouts (at frame + 14):
    //   ADVANCE: [type:1][0:1][evId:2][slot:2][sec:4][nsec:4]                 (14 bytes)
    //   FIRE:    [type:1][0:1][evId:2][sec:4][nsec:4][ch:1][0:1][offsetMs:2] (16 bytes)

    [[gnu::always_inline]]
    inline void sendAdvance(std::uint16_t evId, std::uint16_t slot,
                            std::uint32_t sec, std::uint32_t nsec) {
        auto* frame = m_ethernetSocket.acquire(kFrameSize);
        if (!frame) [[unlikely]] return;

        // Ethernet header already in slot from prefillRing() — skip to payload
        auto* p = frame + kEthHdrLen;
        p[0] = PKT_ADVANCE;
        p[1] = 0;
        std::memcpy(p + 2,  &evId, 2);
        std::memcpy(p + 4,  &slot, 2);
        std::memcpy(p + 6,  &sec,  4);
        std::memcpy(p + 10, &nsec, 4);

        m_ethernetSocket.commit();
    }

    [[gnu::always_inline]]
    inline void sendFire(const CompEntry& info, std::uint32_t sec, std::uint32_t nsec) {
        auto* frame = m_ethernetSocket.acquire(kFrameSize);
        if (!frame) [[unlikely]] return;

        auto* p = frame + kEthHdrLen;
        p[0] = PKT_FIRE;
        p[1] = 0;
        std::memcpy(p + 2,  &info.eventId, 2);
        std::memcpy(p + 4,  &sec,  4);
        std::memcpy(p + 8,  &nsec, 4);
        p[12] = info.channel;
        p[13] = 0;
        std::memcpy(p + 14, &info.offsetMs, 2);

        m_ethernetSocket.commit();
    }

    // ── Main busy-poll + forward loop ───────────────────────────
    //
    // Polls until 'running' goes false (set by watchdog / SIGINT).
    // Checks the flag every 65536 idle spins (~65ms) — zero overhead on hot path.
    void transmitAll(const volatile std::sig_atomic_t& running);
private:
    void performRegisterDump() const;

    // ── Hot path members (cache-line ordered) ────────────────────
    PCIeBackend  m_pcieHandler;
    std::uint32_t m_shadowOff{};
    PacketMmapTx m_ethernetSocket;

    // 64-byte frame template stamped into every TX ring slot at startup.
    // [dst:6][src:6][ethertype:2][zeros:50] — hot path never touches bytes 0-13.
    std::array<std::uint8_t, kFrameSize> m_frameTemplate{};

    // ── Lookup tables ────────────────────────────────────────────
    // Indexed by sw_cmp_idx (0..511). Populated by CONFIG capsule,
    // read by PULSE capsule to stamp enriched metadata into wire packets.
    alignas(64) std::array<bool, kMaxComp>          m_compActive{};
    std::array<CompEntry, kMaxComp>                 m_compInfo{};

    // ── Action metadata (flat array, O(1) lookup by act_idx) ────────
    // Populated once at startup by installActionMap(). CONFIG handler
    // copies: m_compInfo[swCmp] = m_actMeta[actIdx]. ~12KB, fits in L2.
    std::array<CompEntry, kMaxActIdx>               m_actMeta{};

    // ── Cold state ───────────────────────────────────────────────
    bool m_pcieConnectionResult{};
};

#endif // ABTWREN_WRENTRANSMITTER_H
