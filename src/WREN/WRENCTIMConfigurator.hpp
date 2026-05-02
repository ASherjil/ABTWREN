//
// Created by asherjil on 2/21/26.
//
// Configures the WREN firmware (via PCIe BAR1 mailbox) to generate
// CMD_ASYNC_PULSE capsules at the exact CTIM fire time (0-offset actions).
// Subscribes, creates conditions + actions at startup; deletes them in destructor.
//
// Also discovers all existing firmware actions at startup and builds a
// lookup table mapping act_idx → {event_id, channel, offset_ns}.
//

#ifndef ABTWREN_WRENCTIMCONFIGURATOR_H
#define ABTWREN_WRENCTIMCONFIGURATOR_H

#include <PCIeBackend.hpp>
#include <VendorDeviceDiscovery.hpp>
#include <cstdint>
#include <cstdio>
#include <vector>

// ── Mailbox register offsets (BAR1) ──────────────────────────────────
constexpr std::size_t MB_B2H_CSR  = 0x10000;
constexpr std::size_t MB_B2H_CMD  = 0x10008;
constexpr std::size_t MB_B2H_LEN  = 0x1000C;
constexpr std::size_t MB_B2H_DATA = 0x10010;
constexpr std::size_t MB_H2B_CSR  = 0x11000;
constexpr std::size_t MB_H2B_CMD  = 0x11008;
constexpr std::size_t MB_H2B_LEN  = 0x1100C;
constexpr std::size_t MB_H2B_DATA = 0x11010;
constexpr std::uint32_t MB_CSR_READY = 0x1;
constexpr std::uint32_t MB_CMD_REPLY = 0x80000000;
constexpr std::uint32_t MB_CMD_ERROR = 0x40000000;

// ── Command IDs (from wren-mb-cmds.def enum) ─────────────────────────
constexpr std::uint32_t CMD_RX_SET_COND    = 18;
constexpr std::uint32_t CMD_RX_DEL_COND    = 19;
constexpr std::uint32_t CMD_RX_GET_COND    = 20;
constexpr std::uint32_t CMD_RX_SET_ACTION  = 21;
constexpr std::uint32_t CMD_RX_DEL_ACTION  = 22;
constexpr std::uint32_t CMD_RX_GET_ACTION  = 24;
constexpr std::uint32_t CMD_RX_SUBSCRIBE   = 38;

// ── Pulser flags (from wren-mb-defs.h) ───────────────────────────────
constexpr std::uint8_t FLAG_INT_EN  = 0x04;
constexpr std::uint8_t FLAG_ENABLE  = 0x80;

// ── Pulser input MUX values (from wren-hw.h) ────────────────────────
constexpr std::uint16_t INPUT_NOSTART   = 31;
constexpr std::uint16_t INPUT_NOSTOP    = 31;
constexpr std::uint16_t INPUT_CLK_1KHZ  = 23;

// ── Index allocation (high range avoids LTIM driver conflicts) ───────
constexpr std::uint16_t kCtimCondBase = 1100;  // condition indices start here
constexpr std::uint16_t kCtimActBase  = 2040;  // action indices start here
constexpr std::uint8_t  kCtimSrcIdx   = 0;     // primary WR timing domain

/// One CTIM to track: event ID + pulser channel for the 0-offset action.
struct CtimTarget {
    std::uint16_t eventId;
    std::uint8_t  pulserIdx;
};

/// Discovered firmware action: maps act_idx to human-readable LTIM info.
/// Built at startup by querying CMD_RX_GET_ACTION + CMD_RX_GET_COND.
struct ActionInfo {
    std::uint16_t actIdx;
    std::uint16_t eventId;
    std::uint8_t  channel;      // pulser_idx + 1 (wrentest 1-based display)
    std::int64_t  offsetNs;     // idelay / clock_hz (from inputs field)
};

class WRENCTIMConfigurator {
public:
    /// @param pcie     Reference to the PCIeBackend owned by WRENTransmitter.
    /// @param targets  List of CTIMs to configure (event ID + pulser channel).
    WRENCTIMConfigurator(PCIeBackend<VendorDeviceDiscovery>& pcie, const std::vector<CtimTarget>& targets);

    ~WRENCTIMConfigurator();

    WRENCTIMConfigurator(const WRENCTIMConfigurator&) = delete;
    WRENCTIMConfigurator& operator=(const WRENCTIMConfigurator&) = delete;

    [[nodiscard]] std::size_t configuredCount() const { return m_targets.size(); }

    /// Returns the action index for the i-th target (needed by WRENTransmitter
    /// to match CONFIG capsules to our 0-offset actions).
    [[nodiscard]] std::uint16_t actionIndex(std::size_t i) const {
        return static_cast<std::uint16_t>(kCtimActBase + i);
    }

    /// Discovered act_idx → LTIM mapping (populated at startup, never changes).
    [[nodiscard]] const std::vector<ActionInfo>& actionMap() const { return m_actionMap; }

private:
    bool mbSend(std::uint32_t cmd, const void* data, std::size_t words);
    bool mbSendRecv(std::uint32_t cmd, const void* data, std::size_t words,
                    void* reply, std::size_t replyWords);
    void setupAll();
    void cleanupAll();
    void discoverActions();

    PCIeBackend<VendorDeviceDiscovery>& m_pcie;
    std::vector<CtimTarget> m_targets;
    std::vector<ActionInfo> m_actionMap;
};

#endif // ABTWREN_WRENCTIMCONFIGURATOR_H
