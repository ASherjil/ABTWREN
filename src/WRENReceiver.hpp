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
      std::uint16_t evId, offsetMs;
      std::uint32_t sec, nsec;
      std::memcpy(&evId,     p + 2,  2);
      std::memcpy(&sec,      p + 4,  4);
      std::memcpy(&nsec,     p + 8,  4);
      auto ch = p[12];
      std::memcpy(&offsetMs, p + 14, 2);
      std::printf("LTIM_FIRE  ev_id:%u  ch:%u  +%ums  at:%u.%09u\n",
                  evId, ch, offsetMs, sec, nsec);
    }
    else if (p[0] == PKT_CTIM_FIRE) {
      std::uint16_t evId;
      std::uint32_t sec, nsec;
      std::memcpy(&evId, p + 2, 2);
      std::memcpy(&sec,  p + 4, 4);
      std::memcpy(&nsec, p + 8, 4);
      std::printf("CTIM_FIRE  ev_id:%u  at:%u.%09u\n", evId, sec, nsec);
    }
  }
};

#endif // ABTWREN_WRENRECEIVER_H
