//
// Created by asherjil on 2/21/26.
//

#include "WRENCTIMConfigurator.hpp"

WRENCTIMConfigurator::WRENCTIMConfigurator(PCIeBackend<VendorDeviceDiscovery>& pcie,
                                           const std::vector<CtimTarget>& targets)
    : m_pcie{pcie}, m_targets{targets} {
    setupAll();
    discoverActions();
}

WRENCTIMConfigurator::~WRENCTIMConfigurator() {
    cleanupAll();
}

// ── Mailbox send (fire-and-forget, checks reply status only) ─────────
bool WRENCTIMConfigurator::mbSend(std::uint32_t cmd, const void* data, std::size_t words) {
    const auto* src = static_cast<const std::uint32_t*>(data);

    for (std::size_t i = 0; i < words; ++i)
        *m_pcie.registerPtr<std::uint32_t>(MB_H2B_DATA + i * 4) = src[i];

    *m_pcie.registerPtr<std::uint32_t>(MB_H2B_CMD) = cmd;
    *m_pcie.registerPtr<std::uint32_t>(MB_H2B_LEN) = static_cast<std::uint32_t>(words);
    *m_pcie.registerPtr<std::uint32_t>(MB_H2B_CSR) = MB_CSR_READY;

    for (int i = 0; i < 1'000'000; ++i) {
        if (*m_pcie.registerPtr<std::uint32_t>(MB_B2H_CSR) & MB_CSR_READY) {
            std::uint32_t reply = *m_pcie.registerPtr<std::uint32_t>(MB_B2H_CMD);
            *m_pcie.registerPtr<std::uint32_t>(MB_B2H_CSR) = 0;

            if (reply & MB_CMD_ERROR) {
                std::fprintf(stderr, "  [CTIM] FIRMWARE REJECTED cmd %u (reply=0x%08X)\n", cmd, reply);
                return false;
            }
            return true;
        }
    }

    std::fprintf(stderr, "  [CTIM] TIMEOUT waiting for firmware reply (cmd %u)\n", cmd);
    return false;
}

// ── Mailbox send + read reply data ───────────────────────────────────
bool WRENCTIMConfigurator::mbSendRecv(std::uint32_t cmd, const void* data, std::size_t words,
                                       void* reply, std::size_t replyWords) {
    const auto* src = static_cast<const std::uint32_t*>(data);

    for (std::size_t i = 0; i < words; ++i)
        *m_pcie.registerPtr<std::uint32_t>(MB_H2B_DATA + i * 4) = src[i];

    *m_pcie.registerPtr<std::uint32_t>(MB_H2B_CMD) = cmd;
    *m_pcie.registerPtr<std::uint32_t>(MB_H2B_LEN) = static_cast<std::uint32_t>(words);
    *m_pcie.registerPtr<std::uint32_t>(MB_H2B_CSR) = MB_CSR_READY;

    for (int i = 0; i < 1'000'000; ++i) {
        if (*m_pcie.registerPtr<std::uint32_t>(MB_B2H_CSR) & MB_CSR_READY) {
            std::uint32_t cmdReply = *m_pcie.registerPtr<std::uint32_t>(MB_B2H_CMD);

            if (cmdReply & MB_CMD_ERROR) {
                *m_pcie.registerPtr<std::uint32_t>(MB_B2H_CSR) = 0;
                return false;  // silent — expected for unused indices
            }

            // Read reply data from B2H_DATA
            auto* dst = static_cast<std::uint32_t*>(reply);
            for (std::size_t j = 0; j < replyWords; ++j)
                dst[j] = *m_pcie.registerPtr<std::uint32_t>(MB_B2H_DATA + j * 4);

            *m_pcie.registerPtr<std::uint32_t>(MB_B2H_CSR) = 0;
            return true;
        }
    }
    return false;
}

// ── Discover all existing firmware actions at startup ────────────────
// Scans act_idx 0..255 with CMD_RX_GET_ACTION. For each that exists,
// queries its condition with CMD_RX_GET_COND to get the event ID.
// Builds m_actionMap for the receiver/consumer to classify FIRE packets.
//
// GET_ACTION reply layout (struct wren_rx_action, 8 words LE):
//   word 0: [next:16 low][cond_idx:16 high]
//   word 1: [pulser_idx:8][flags:8][inputs:16 high]
//   word 2: width
//   word 3: period
//   word 4: npulses
//   word 5: idelay
//   word 6: load_off_sec
//   word 7: load_off_nsec
//
// GET_COND reply layout (struct wren_rx_cond, 11 words LE):
//   word 0: [next:16 low][act_idx:16 high]       (prefix)
//   word 1: [nbr_act:8][log:8][nparam:16 high]   (prefix)
//   word 2: [evt_id:16 low][src_idx:8][len:8]     ← event ID here
//   words 3..10: condition ops
void WRENCTIMConfigurator::discoverActions() {
    std::printf("[CTIM] Discovering firmware actions...\n");

    constexpr std::size_t kActionReplyWords = 8;   // sizeof(wren_rx_action) / 4
    constexpr std::size_t kCondReplyWords   = 11;  // sizeof(wren_rx_cond) / 4

    for (std::uint32_t idx = 0; idx < 256; ++idx) {
        std::uint32_t actionReply[kActionReplyWords] = {};
        if (!mbSendRecv(CMD_RX_GET_ACTION, &idx, 1, actionReply, kActionReplyWords))
            continue;

        // Word 0 high 16 = cond_idx
        auto condIdx   = static_cast<std::uint16_t>(actionReply[0] >> 16);
        // Word 1: pulser_idx (byte 0), flags (byte 1), inputs (bytes 2-3)
        auto pulserIdx = static_cast<std::uint8_t>(actionReply[1] & 0xFF);
        auto inputs    = static_cast<std::uint16_t>(actionReply[1] >> 16);
        // Word 5: idelay (in clock ticks)
        auto idelay    = actionReply[5];

        // Compute offset from idelay and clock source (inputs bits [14:10])
        auto clockSel = static_cast<std::uint8_t>((inputs >> 10) & 0x1F);
        std::int64_t offsetNs = 0;
        switch (clockSel) {
            case 23: offsetNs = static_cast<std::int64_t>(idelay) * 1'000'000; break; // 1 kHz
            case 25: offsetNs = static_cast<std::int64_t>(idelay) * 100;       break; // 10 MHz
            case 31: offsetNs = static_cast<std::int64_t>(idelay);             break; // 1 GHz
            default: offsetNs = static_cast<std::int64_t>(idelay);             break; // unknown, assume ns
        }

        // Query the condition to get the event ID
        std::uint16_t eventId = 0;
        std::uint32_t condArg = condIdx;
        std::uint32_t condReply[kCondReplyWords] = {};
        if (mbSendRecv(CMD_RX_GET_COND, &condArg, 1, condReply, kCondReplyWords)) {
            // Word 2 low 16 = evt_id
            eventId = static_cast<std::uint16_t>(condReply[2] & 0xFFFF);
        }

        auto channel = static_cast<std::uint8_t>(pulserIdx + 1);  // wrentest 1-based
        m_actionMap.push_back({static_cast<std::uint16_t>(idx), eventId, channel, offsetNs});

        std::printf("  act[%u] → ev_id:%u  ch:%u  offset:%u cycles\n",
                    idx, eventId, channel, idelay/*static_cast<long long>(offsetNs / 1'000'000)*/);
    }

    // Add our own CTIM fire actions (act_idx 2040+ not found by 0..255 scan).
    // Use sentinel values (channel=0xFF, offsetNs maps to offsetMs=0xFFFF) so the
    // receiver can distinguish CTIM fires from LTIM fires without any TX-side logic.
    for (std::size_t i = 0; i < m_targets.size(); ++i) {
        m_actionMap.push_back({
            static_cast<std::uint16_t>(kCtimActBase + i),
            m_targets[i].eventId,
            0xFF,                                       // sentinel: no physical channel
            static_cast<std::int64_t>(0xFFFF) * 1'000'000  // → offsetMs = 0xFFFF after /1e6
        });
    }

    std::printf("[CTIM] Discovered %zu firmware actions (%zu LTIM + %zu CTIM)\n",
                m_actionMap.size(), m_actionMap.size() - m_targets.size(), m_targets.size());
}

// ── Setup: subscribe + create condition + create action for each CTIM ─
void WRENCTIMConfigurator::setupAll() {
    for (std::size_t i = 0; i < m_targets.size(); ++i) {
        const auto& t = m_targets[i];
        auto condIdx = static_cast<std::uint16_t>(kCtimCondBase + i);
        auto actIdx  = static_cast<std::uint16_t>(kCtimActBase  + i);

        // 1. Subscribe to event (idempotent)
        std::uint32_t subData[2] = {kCtimSrcIdx, t.eventId};
        std::printf("[CTIM] SUBSCRIBE src=%u ev_id=%u\n", kCtimSrcIdx, t.eventId);
        if (!mbSend(CMD_RX_SUBSCRIBE, subData, 2)) {
            std::fprintf(stderr, "[CTIM] Subscribe failed for ev%u, skipping\n", t.eventId);
            continue;
        }

        // 2. Create condition
        std::uint32_t condData[10] = {};
        condData[0] = condIdx;
        condData[1] = static_cast<std::uint32_t>(t.eventId)
                    | (static_cast<std::uint32_t>(kCtimSrcIdx) << 16)
                    | (0u << 24);

        std::printf("[CTIM] SET_COND cond=%u evt_id=%u src=%u\n", condIdx, t.eventId, kCtimSrcIdx);
        if (!mbSend(CMD_RX_SET_COND, condData, 10)) {
            std::fprintf(stderr, "[CTIM] Set condition failed for ev%u, skipping\n", t.eventId);
            continue;
        }

        // 3. Create 0-offset action
        struct {
            std::uint32_t act_idx;
            std::uint32_t cond_idx;
            std::uint8_t  pulser_idx;
            std::uint8_t  flags;
            std::uint16_t inputs;
            std::uint32_t width;
            std::uint32_t period;
            std::uint32_t npulses;
            std::uint32_t idelay;
            std::int32_t  load_off_sec;
            std::int32_t  load_off_nsec;
        } act = {};

        act.act_idx      = actIdx;
        act.cond_idx     = condIdx;
        act.pulser_idx   = t.pulserIdx;
        act.flags        = FLAG_ENABLE | FLAG_INT_EN;
        act.inputs       = (INPUT_CLK_1KHZ << 10) | (INPUT_NOSTOP << 5) | INPUT_NOSTART;
        act.width        = 1000;
        act.period       = 1;
        act.npulses      = 1;
        act.idelay       = 0;
        act.load_off_sec  = 0;
        act.load_off_nsec = 0;

        std::printf("[CTIM] SET_ACTION act=%u cond=%u pulser=%u flags=0x%02X offset=0\n",
                    actIdx, condIdx, t.pulserIdx, act.flags);

        if (!mbSend(CMD_RX_SET_ACTION, &act, sizeof(act) / 4)) {
            std::fprintf(stderr, "[CTIM] Set action failed for ev%u\n", t.eventId);
        }
    }
}

// ── Cleanup: delete action first, then condition (order matters) ─────
void WRENCTIMConfigurator::cleanupAll() {
    for (std::size_t i = 0; i < m_targets.size(); ++i) {
        auto actIdx  = static_cast<std::uint32_t>(kCtimActBase  + i);
        auto condIdx = static_cast<std::uint32_t>(kCtimCondBase + i);

        if (!mbSend(CMD_RX_DEL_ACTION, &actIdx, 1))
            std::fprintf(stderr, "[CTIM] DEL_ACTION act=%u failed\n", actIdx);

        if (!mbSend(CMD_RX_DEL_COND, &condIdx, 1))
            std::fprintf(stderr, "[CTIM] DEL_COND cond=%u failed\n", condIdx);
    }
    std::printf("[CTIM] Cleanup: %zu actions + conditions deleted\n", m_targets.size());
}
