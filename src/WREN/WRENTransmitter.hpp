//
// WRENTransmitter — WREN PCIe ring reader + packet transmitter
//
// Busy-polls the WREN async ring buffer via BAR1 MMIO, decodes capsules
// (EVENT/CONFIG/PULSE), and forwards enriched timing packets via any TxRing
// transport (packet_mmap, AF_XDP, custom PMD).
//

#ifndef ABTWREN_WRENTRANSMITTER_H
#define ABTWREN_WRENTRANSMITTER_H

#include "WRENProtocol.hpp"
#include "WRENCTIMConfigurator.hpp"
#include <PCIeBackend.hpp>
#include <RingConcepts.hpp>

#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

constexpr int kMaxSlots  = 64;
constexpr int kMaxComp   = 512;
constexpr int kMaxActIdx = 2048;

struct CompEntry {
    std::uint16_t eventId{};
    std::uint8_t  channel{0xFF};
    std::uint16_t offsetMs{0xFFFF};
};

struct LtimTarget {
    std::uint16_t event_id;
    std::uint16_t ltim_slot;
    std::uint64_t delay_ns;
};

constexpr auto kTargets = std::to_array<LtimTarget>({
    {142, 23,   50'000'000},  {143, 20,   10'000'000},
    {143, 21,  100'000'000},  {143, 22,  900'000'000},
    {138, 21,  100'000'000},  {138, 22,  900'000'000},
});

template<TxRing Tx>
class WRENTransmitter {
public:
    WRENTransmitter(std::uint16_t vendorID, std::uint16_t deviceID, int bar, Tx& ethernetSocket);

    WRENTransmitter(const WRENTransmitter&) = delete;
    WRENTransmitter(WRENTransmitter&&) noexcept = default;
    WRENTransmitter& operator=(const WRENTransmitter&) = delete;
    WRENTransmitter& operator=(WRENTransmitter&&) noexcept = default;
    ~WRENTransmitter() = default;

    [[nodiscard]] PCIeBackend<VendorDeviceDiscovery>& pcie() { return m_pcieHandler; }
    [[nodiscard]] bool isConnected() const { return m_pcieConnectionResult; }

    void installActionMap(const std::vector<ActionInfo>& map);
    void setMacAddresses(const std::array<std::uint8_t, 6>& src, const std::array<std::uint8_t, 6>& dst);

    [[nodiscard, gnu::always_inline]]
    inline std::uint32_t readRingWord(std::uint32_t idx) const;

    [[nodiscard, gnu::always_inline]]
    inline std::pair<const LtimTarget*, int> resolveCtimToSlots(std::uint16_t evId);

    [[gnu::always_inline]]
    inline void sendAdvance(std::uint16_t evId, std::uint16_t slot,
                            std::uint32_t sec, std::uint32_t nsec);

    [[gnu::always_inline]]
    inline void sendFire(const CompEntry& info, std::uint32_t sec, std::uint32_t nsec);

    void transmitAll(const volatile std::sig_atomic_t& running);

    // LTIM-only hot path — forwards only PULSE capsules (no EVENT/CONTEXT overhead).
    // CONFIG is still processed for metadata setup but generates no TX packets.
    [[gnu::hot, gnu::always_inline]]
    inline void transmitLTIM(const volatile std::sig_atomic_t& running);

private:
    void performRegisterDump() const;

    PCIeBackend<VendorDeviceDiscovery>   m_pcieHandler;
    Tx&                                  m_ethernetSocket;
    std::uint32_t                        m_shadowOff{};
    std::array<std::uint8_t, kFrameSize> m_frameTemplate{};

    alignas(64) std::array<bool,  kMaxComp>   m_compActive{};
    std::array<CompEntry, kMaxComp>            m_compInfo{};
    std::array<CompEntry, kMaxActIdx>          m_actMeta{};

    bool m_pcieConnectionResult{};
};

#include "WRENTransmitter.cpp"

#endif // ABTWREN_WRENTRANSMITTER_H
