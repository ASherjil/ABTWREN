//
// Created by asherjil on 2/17/26.
//

#ifndef ABTWREN_WRENRECEIVER_H
#define ABTWREN_WRENRECEIVER_H

#include "WRENProtocol.hpp"
#include <PacketMmapRx.hpp>

#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>

class WRENReceiver {
public:
  explicit WRENReceiver(const RingConfig& cfg /* TODO: SPSCQueue& queue*/);

  WRENReceiver(const WRENReceiver&) = delete;
  WRENReceiver(WRENReceiver&& other) noexcept = default; 

  WRENReceiver& operator=(const WRENReceiver&) = delete;
  WRENReceiver& operator=(WRENReceiver&& other) noexcept = default;

  ~WRENReceiver() = default;

  // Polls until 'running' goes false (set by watchdog / SIGINT).
  // Checks the flag every 65536 idle spins (~65ms) — zero overhead on hot path.
  void beginReceiving(const volatile std::sig_atomic_t& running) {
    std::uint32_t spins = 0;

    while (running) {
      RxFrame frame = m_ethernetSocket.tryReceive();

      if (!frame.data.empty()){
        if (frame.data.size() >= kFrameSize) [[likely]] {
          // TODO: take the frame.data and push into the SPSC queue
          printDebugLogs(frame);

          m_ethernetSocket.release();
        }
        spins = 0;
        continue;
      }

      if ((++spins & 0xFFFF) == 0 && !running) break;
    }
    std::fprintf(stderr, "[RX] Exiting poll loop.\n");
  }

  // TODO: This will be second thread that will pop the SPSC queue and fast forward to FESA real-time actions
  void executeEvents() {
    return;
  }

private:
  PacketMmapRx m_ethernetSocket;
  //TODO: SPSCQueue& m_queue

  [[gnu::always_inline]]
  inline void printDebugLogs(const RxFrame& frame) {
    const auto* p = frame.data.data() + kEthHdrLen;

    if (p[0] == PKT_ADVANCE) {
      /*
      std::uint16_t evId, slot;
      std::uint32_t sec, nsec;
      std::memcpy(&evId, p + 2,  2);
      std::memcpy(&slot, p + 4,  2);
      std::memcpy(&sec,  p + 6,  4);
      std::memcpy(&nsec, p + 10, 4);
      std::printf("ADVANCE  ev_id:%u  slot:%u  due:%u.%09u\n", evId, slot, sec, nsec);
      */
    }
    else if (p[0] == PKT_FIRE) {
      std::uint16_t slot;
      std::uint32_t sec, nsec;
      std::memcpy(&slot, p + 2, 2);
      std::memcpy(&sec,  p + 4, 4);
      std::memcpy(&nsec, p + 8, 4);

      // Classify: is this a 0-offset CTIM fire or a regular LTIM fire?
      // act_idx 2040+ are our mailbox-configured 0-offset actions (see WRENCTIMConfigurator)
      // Order matches the target list passed to WRENCTIMConfigurator in main.cpp
      constexpr std::uint16_t kCtimFireActBase = 2040;
      constexpr std::array<std::uint16_t, 3> kCtimFireEvents = {142, 143, 138};

      if (slot >= kCtimFireActBase && slot < kCtimFireActBase + kCtimFireEvents.size()) {
        auto idx = static_cast<std::size_t>(slot - kCtimFireActBase);
        std::printf("CTIM_FIRE  ev_id:%u  at:%u.%09u\n", kCtimFireEvents[idx], sec, nsec);
      } else {
        std::printf("LTIM_FIRE  slot:%u  at:%u.%09u\n", slot, sec, nsec);
      }
    }
  }
};

#endif // ABTWREN_WRENRECEIVER_H
