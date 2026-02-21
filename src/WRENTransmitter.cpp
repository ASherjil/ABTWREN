//
// Created by asherjil on 2/15/26.
//

#include "WRENTransmitter.hpp"

#include <chrono>
#include <cstdio>


WRENTransmitter::WRENTransmitter(std::uint16_t vendorID, std::uint16_t deviceID, int bar, const RingConfig& cfg)
  : m_pcieHandler{vendorID, deviceID, bar}, m_ethernetSocket{cfg} {

  m_pcieConnectionResult = m_pcieHandler.open();
  if (!m_pcieConnectionResult) {
    std::fprintf(stderr, "Connection failure, unable to open WREN PCIe device.\n");
    return;
  }

  std::fprintf(stderr, "DEBUG: WREN PCIe open=%d base=%p size=%zu\n",
                     m_pcieConnectionResult, m_pcieHandler.getBaseAddress(),
                     m_pcieHandler.getMmapSize());

  // Pre-fill EtherType in the frame template (MACs set later via setMacAddresses)
  m_frameTemplate[12] = static_cast<std::uint8_t>(kEtherType >> 8);
  m_frameTemplate[13] = static_cast<std::uint8_t>(kEtherType & 0xFF);

  performRegisterDump();
}


void WRENTransmitter::performRegisterDump() const {
  constexpr int NUM_TRANSACTIONS = 7; // 5x 64-bit + 2x 32-bit

  auto t0 = std::chrono::steady_clock::now();
  // 64-bit reads: pair adjacent registers into single PCIe transactions
  std::uint64_t identAndMap   = *m_pcieHandler.registerPtr<std::uint64_t>(WREN_IDENT);       // 0x00+0x04
  std::uint64_t modelAndFw    = *m_pcieHandler.registerPtr<std::uint64_t>(WREN_MODEL_ID);    // 0x08+0x0C
  std::uint64_t stateAndTaiLo = *m_pcieHandler.registerPtr<std::uint64_t>(WREN_WR_STATE);    // 0x10+0x14
  std::uint64_t taiHiAndCyc   = *m_pcieHandler.registerPtr<std::uint64_t>(WREN_TM_TAI_HI);  // 0x18+0x1C
  std::uint32_t compact       = *m_pcieHandler.registerPtr<std::uint32_t>(WREN_TM_COMPACT);  // 0x20 (gap to 0x28)
  std::uint64_t isrAndRaw     = *m_pcieHandler.registerPtr<std::uint64_t>(WREN_ISR);         // 0x28+0x2C
  std::uint32_t imr           = *m_pcieHandler.registerPtr<std::uint32_t>(WREN_IMR);         // 0x30
  auto t1 = std::chrono::steady_clock::now();

  auto totalNs = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

  // Little-endian: low 32 bits = lower address, high 32 bits = higher address
  auto ident      = static_cast<std::uint32_t>(identAndMap);
  auto mapVersion = static_cast<std::uint32_t>(identAndMap >> 32);
  auto modelId    = static_cast<std::uint32_t>(modelAndFw);
  auto fwVersion  = static_cast<std::uint32_t>(modelAndFw >> 32);
  auto wrState    = static_cast<std::uint32_t>(stateAndTaiLo);
  auto taiLo      = static_cast<std::uint32_t>(stateAndTaiLo >> 32);
  auto taiHi      = static_cast<std::uint32_t>(taiHiAndCyc);
  auto cycles     = static_cast<std::uint32_t>(taiHiAndCyc >> 32);
  auto isr        = static_cast<std::uint32_t>(isrAndRaw);
  auto isrRaw     = static_cast<std::uint32_t>(isrAndRaw >> 32);

  // Decode IDENT as ASCII string
  char identStr[5];
  identStr[0] = static_cast<char>((ident >> 24) & 0xFF);
  identStr[1] = static_cast<char>((ident >> 16) & 0xFF);
  identStr[2] = static_cast<char>((ident >>  8) & 0xFF);
  identStr[3] = static_cast<char>((ident >>  0) & 0xFF);
  identStr[4] = '\0';

  std::printf("\n=== WREN Host Register Dump (BAR1, %d transactions: 5x64-bit + 2x32-bit) ===\n", NUM_TRANSACTIONS);
  std::printf("  ident:        0x%08X  (\"%s\")\n", ident, identStr);
  std::printf("  map_version:  0x%08X\n", mapVersion);
  std::printf("  model_id:     0x%08X\n", modelId);
  std::printf("  fw_version:   0x%08X\n", fwVersion);
  std::printf("  wr_state:     0x%08X  (link=%s, time=%s)\n",
              wrState,
              (wrState & 0x1) ? "UP" : "DOWN",
              (wrState & 0x2) ? "VALID" : "INVALID");
  std::printf("  tm_tai_lo:    0x%08X  (%u s)\n", taiLo, taiLo);
  std::printf("  tm_tai_hi:    0x%08X\n", taiHi);
  std::printf("  tm_cycles:    0x%08X  (%u ns)\n", cycles, (cycles & 0x0FFFFFFF) * 16);
  std::printf("  tm_compact:   0x%08X\n", compact);
  std::printf("  isr:          0x%08X\n", isr);
  std::printf("  isr_raw:      0x%08X\n", isrRaw);
  std::printf("  imr:          0x%08X\n", imr);
  std::printf("  ---\n");
  std::printf("  %ld ns total / %d PCIe transactions = %ld ns avg\n\n", totalNs, NUM_TRANSACTIONS, totalNs / NUM_TRANSACTIONS);
}


// Builds the 64-byte frame template and stamps it into every TX ring slot.
// After this, the hot path never writes bytes 0-13 — only the payload changes.
void WRENTransmitter::setMacAddresses(const std::array<std::uint8_t, 6>& src, const std::array<std::uint8_t, 6>& dst) {
  std::memcpy(&m_frameTemplate[0], dst.data(), kMacLength);  // destination MAC at bytes 0-5
  std::memcpy(&m_frameTemplate[6], src.data(), kMacLength);   // source MAC at bytes 6-11
  // EtherType at bytes 12-13 was set in constructor; bytes 14-63 are zero from init.

  // Stamp the complete template into every available TX ring slot.
  // The kernel never modifies slot data on recycle — only tp_status.
  m_ethernetSocket.prefillRing(m_frameTemplate);
}

void WRENTransmitter::transmitAll(const volatile std::sig_atomic_t& running) {
    // Sync shadow pointer to current firmware position
    m_shadowOff = *m_pcieHandler.registerPtr<std::uint32_t>(WREN_ASYNC_BOARD_OFF);
    std::uint32_t spins = 0;

    while (running) {
        // ── Gate: 1 PCIe read (~1.0us) ───────────────────────
        std::uint32_t boardOff =
            *m_pcieHandler.registerPtr<std::uint32_t>(WREN_ASYNC_BOARD_OFF);

        if (boardOff == m_shadowOff) [[likely]] {
            if ((++spins & 0xFFFF) == 0 && !running) break;
            continue;
        }
        spins = 0;

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
                if (!m_compActive[comp]) break;

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

                const auto& info = m_compInfo[comp];
                if (info.isCtim)
                    sendCtimFire(info.eventId, sec, nsec);
                else
                    sendFire(info, sec, nsec);
                break;
            }

            // ── CONFIG: enrich sw_cmp with action metadata ────
            // Look up act_idx in the action map to get eventId/channel/offset.
            // This is NOT on the hot path (CONFIG arrives once per comparator load,
            // well before the corresponding PULSE). Linear scan of ~20 entries is fine.
            case TYP_CONFIG: {
                const auto actIdx = static_cast<std::uint16_t>(w1);
                const auto swCmp  = static_cast<std::uint16_t>(w1 >> 16);

                CompEntry entry{};
                for (const auto& a : m_actionMap) {
                    if (a.actIdx == actIdx) {
                        entry.eventId  = a.eventId;
                        entry.channel  = a.channel;
                        entry.isCtim   = (a.offsetNs == 0);
                        entry.offsetMs = static_cast<std::uint16_t>(a.offsetNs / 1'000'000);
                        break;
                    }
                }
                m_compInfo[swCmp] = entry;
                m_compActive[swCmp] = true;
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

            // CONTEXT: informational (new timing context). Do NOT clear
            // m_compActive — firmware reuses sw_cmp from a pool and CONFIG
            // always updates m_compToSlot before the corresponding PULSE fires.
            // Clearing here would drop PULSEs with long offsets (e.g. 900ms).
            case TYP_CONTEXT:
                break;

            default:
                break;
            }

            m_shadowOff = (m_shadowOff + len) & RING_MASK;

        } while (m_shadowOff != boardOff);
    }
    std::fprintf(stderr, "[TX] Exiting poll loop.\n");
}
