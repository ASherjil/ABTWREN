//
// Created by asherjil on 2/21/26.
//

#include "WRENCTIMConfigurator.hpp"

WRENCTIMConfigurator::WRENCTIMConfigurator(PCIeBackend& pcie,
                                           const std::vector<CtimTarget>& targets)
    : m_pcie{pcie}, m_targets{targets} {
    setupAll();
}

WRENCTIMConfigurator::~WRENCTIMConfigurator() {
    cleanupAll();
}

// ── Mailbox send (mirrors kernel driver wren_mb_msg) ─────────────────
bool WRENCTIMConfigurator::mbSend(std::uint32_t cmd, const void* data, std::size_t words) {
    const auto* src = static_cast<const std::uint32_t*>(data);

    // 1. Write data words to H2B_DATA
    for (std::size_t i = 0; i < words; ++i)
        *m_pcie.registerPtr<std::uint32_t>(MB_H2B_DATA + i * 4) = src[i];

    // 2. Write command, length, then CSR=READY (PCIe ordering guarantees in-order)
    *m_pcie.registerPtr<std::uint32_t>(MB_H2B_CMD) = cmd;
    *m_pcie.registerPtr<std::uint32_t>(MB_H2B_LEN) = static_cast<std::uint32_t>(words);
    *m_pcie.registerPtr<std::uint32_t>(MB_H2B_CSR) = MB_CSR_READY;

    // 3. Poll B2H_CSR for READY (firmware reply) — timeout ~1s
    for (int i = 0; i < 1'000'000; ++i) {
        if (*m_pcie.registerPtr<std::uint32_t>(MB_B2H_CSR) & MB_CSR_READY) {
            std::uint32_t reply = *m_pcie.registerPtr<std::uint32_t>(MB_B2H_CMD);
            // 4. Acknowledge (clear B2H_CSR)
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

// ── Setup: subscribe + create condition + create action for each CTIM ─
void WRENCTIMConfigurator::setupAll() {
    for (std::size_t i = 0; i < m_targets.size(); ++i) {
        const auto& t = m_targets[i];
        auto condIdx = static_cast<std::uint16_t>(kCtimCondBase + i);
        auto actIdx  = static_cast<std::uint16_t>(kCtimActBase  + i);

        // 1. Subscribe to event (idempotent — just sets bit in firmware subscription map)
        //    struct wren_mb_rx_subscribe { uint32_t src_idx; uint32_t ev_id; }
        std::uint32_t subData[2] = {kCtimSrcIdx, t.eventId};
        std::printf("[CTIM] SUBSCRIBE src=%u ev_id=%u\n", kCtimSrcIdx, t.eventId);
        if (!mbSend(CMD_RX_SUBSCRIBE, subData, 2)) {
            std::fprintf(stderr, "[CTIM] Subscribe failed for ev%u, skipping\n", t.eventId);
            continue;
        }

        // 2. Create condition matching this event ID
        //    Word 0: [cond_idx:16][padding:16]
        //    Word 1: [evt_id:16][src_idx:8][len:8]
        //    Words 2-9: ops[0..7] = 0 (unused, len=0 means unconditional)
        std::uint32_t condData[10] = {};
        condData[0] = condIdx;
        condData[1] = static_cast<std::uint32_t>(t.eventId)
                    | (static_cast<std::uint32_t>(kCtimSrcIdx) << 16)
                    | (0u << 24);  // len=0: no conditional ops

        std::printf("[CTIM] SET_COND cond=%u evt_id=%u src=%u\n", condIdx, t.eventId, kCtimSrcIdx);
        if (!mbSend(CMD_RX_SET_COND, condData, 10)) {
            std::fprintf(stderr, "[CTIM] Set condition failed for ev%u, skipping\n", t.eventId);
            continue;
        }

        // 3. Create action with 0-offset on this condition
        //    struct wren_mb_rx_set_action {
        //      uint32_t act_idx, cond_idx;
        //      struct wren_mb_pulser_config { pulser_idx:8, flags:8, inputs:16,
        //          width:32, period:32, npulses:32, idelay:32, load_off_sec:32, load_off_nsec:32 }
        //    }
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
        act.flags        = FLAG_ENABLE | FLAG_INT_EN;  // 0x84
        act.inputs       = (INPUT_CLK_1KHZ << 10) | (INPUT_NOSTOP << 5) | INPUT_NOSTART;
        act.width        = 1000;   // 1us pulse (just need the interrupt)
        act.period       = 1;
        act.npulses      = 1;
        act.idelay       = 0;
        act.load_off_sec  = 0;     // ZERO offset — fire at exact CTIM due time
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

        // 1. Delete action first (unlinks from condition)
        if (!mbSend(CMD_RX_DEL_ACTION, &actIdx, 1))
            std::fprintf(stderr, "[CTIM] DEL_ACTION act=%u failed\n", actIdx);

        // 2. Delete condition (safe — no action linked)
        if (!mbSend(CMD_RX_DEL_COND, &condIdx, 1))
            std::fprintf(stderr, "[CTIM] DEL_COND cond=%u failed\n", condIdx);

        // Do NOT unsubscribe — other LTIMs may use the same event ID
    }
    std::printf("[CTIM] Cleanup: %zu actions + conditions deleted\n", m_targets.size());
}
