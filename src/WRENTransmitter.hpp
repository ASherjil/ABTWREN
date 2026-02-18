//
// Created by asherjil on 2/15/26.
//

#ifndef ABTWREN_WRENTRANSMITTER_H
#define ABTWREN_WRENTRANSMITTER_H

#include "WRENProtocol.hpp"
#include <PCIeBackend.hpp>
#include <PacketMmapTx.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <utility>

constexpr int kMaxSlots = 64;
constexpr int kMaxComp  = 256;

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
    //   ADVANCE: [type:1][0:1][evId:2][slot:2][sec:4][nsec:4]  (14 bytes)
    //   FIRE:    [type:1][0:1][slot:2][sec:4][nsec:4]          (12 bytes)

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
    inline void sendFire(std::uint16_t slot, std::uint32_t sec, std::uint32_t nsec) {
        auto* frame = m_ethernetSocket.acquire(kFrameSize);
        if (!frame) [[unlikely]] return;

        // Ethernet header already in slot from prefillRing() — skip to payload
        auto* p = frame + kEthHdrLen;
        p[0] = PKT_FIRE;
        p[1] = 0;
        std::memcpy(p + 2, &slot, 2);
        std::memcpy(p + 4, &sec,  4);
        std::memcpy(p + 8, &nsec, 4);

        m_ethernetSocket.commit();
    }

    // ── Main busy-poll + forward loop (never returns) ────────────

    [[noreturn]]
    void transmitAll() {
        // Sync shadow pointer to current firmware position
        m_shadowOff = *m_pcieHandler.registerPtr<std::uint32_t>(WREN_ASYNC_BOARD_OFF);

        for (;;) {
            // ── Gate: 1 PCIe read (~1.0us) ───────────────────────
            std::uint32_t boardOff =
                *m_pcieHandler.registerPtr<std::uint32_t>(WREN_ASYNC_BOARD_OFF);

            if (boardOff == m_shadowOff) [[likely]]
                continue;

            // ── Drain all new capsules ───────────────────────────
            do {
                // Can all 4 words be read as 2x 64-bit without wrapping?
                bool safe64 = (m_shadowOff + 3) <= RING_MASK;

                std::uint32_t hdr, w1;

                if (safe64) [[likely]] {
                    // 1 PCIe transaction: header + w1
                    std::uint64_t hdrW1 = *m_pcieHandler.registerPtr<std::uint64_t>(
                        WREN_ASYNC_DATA_BASE + m_shadowOff * 4);
                    hdr = static_cast<std::uint32_t>(hdrW1);
                    w1  = static_cast<std::uint32_t>(hdrW1 >> 32);
                } else {
                    // Ring wrap boundary — 2 individual 32-bit reads
                    hdr = readRingWord(m_shadowOff);
                    w1  = readRingWord(m_shadowOff + 1);
                }

                auto typ = static_cast<std::uint8_t>(hdr & TYP_MASK);
                auto len = static_cast<std::uint16_t>(hdr >> LEN_SHIFT);

                // Guard against corrupted ring (prevents infinite loop)
                if (len == 0) [[unlikely]] {
                    m_shadowOff = (m_shadowOff + 1) & RING_MASK;
                    continue;
                }

                switch (typ) {

                // ── FIRE path (latency-critical) ─────────────────
                case TYP_PULSE: {
                    auto comp = static_cast<std::uint16_t>(w1);
                    if (!m_compActive[comp]) {
                        break;
                    }

                    std::uint32_t sec, nsec;
                    if (safe64) [[likely]] {
                        std::uint64_t w2w3 = *m_pcieHandler.registerPtr<std::uint64_t>(
                            WREN_ASYNC_DATA_BASE + (m_shadowOff + 2) * 4);
                        sec  = static_cast<std::uint32_t>(w2w3);        // pulse: w2=sec
                        nsec = static_cast<std::uint32_t>(w2w3 >> 32);  // pulse: w3=nsec
                    } else {
                        sec  = readRingWord(m_shadowOff + 2);
                        nsec = readRingWord(m_shadowOff + 3);
                    }
                    sendFire(m_compToSlot[comp], sec, nsec);
                    break;
                }

                // ── CONFIG: track comp_idx <-> ltim_slot mapping ─
                case TYP_CONFIG: {
                    auto comp = static_cast<std::uint16_t>(w1);
                    auto slot = static_cast<std::uint16_t>(w1 >> 16);
                    m_compToSlot[comp] = slot;

                    // Activate if this slot is in our target set
                    for (const auto& t : kTargets) {
                        if (t.ltim_slot == slot) {
                            m_compActive[comp] = true;
                            break;
                        }
                    }
                    break;
                }

                // ── ADVANCE path (relaxed — seconds early) ───────
                case TYP_EVENT: {
                    auto evId = static_cast<std::uint16_t>(w1);
                    auto [targets, count] = resolveCtimToSlots(evId);
                    if (count == 0) break;  // CTIM we don't care about

                    // Read timestamp once (event: w2=nsec, w3=sec)
                    std::uint32_t nsec, sec;
                    if (safe64) [[likely]] {
                        std::uint64_t w2w3 = *m_pcieHandler.registerPtr<std::uint64_t>(
                            WREN_ASYNC_DATA_BASE + (m_shadowOff + 2) * 4);
                        nsec = static_cast<std::uint32_t>(w2w3);
                        sec  = static_cast<std::uint32_t>(w2w3 >> 32);
                    } else {
                        nsec = readRingWord(m_shadowOff + 2);
                        sec  = readRingWord(m_shadowOff + 3);
                    }

                    for (int i = 0; i < count; ++i)
                        sendAdvance(evId, targets[i].ltim_slot, sec, nsec);
                    break;
                }

                // ── New cycle: comp_idx rotates, reset armed state
                case TYP_CONTEXT:
                    m_compActive.fill(false);
                    break;

                default:
                    break;
                }

                m_shadowOff = (m_shadowOff + len) & RING_MASK;

            } while (m_shadowOff != boardOff);
        }
    }


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
    alignas(64) std::array<bool, kMaxComp>          m_compActive{};
    std::array<std::uint16_t, kMaxComp>             m_compToSlot{};

    // ── Cold state ───────────────────────────────────────────────
    bool m_pcieConnectionResult{};
};

#endif // ABTWREN_WRENTRANSMITTER_H
