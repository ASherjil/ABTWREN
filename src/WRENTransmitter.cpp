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
