//
// Created by asherjil on 2/17/26.
//

#ifndef ABTWREN_WRENRECEIVER_H
#define ABTWREN_WRENRECEIVER_H

#include "WRENProtocol.hpp"
#include <PacketMmapRx.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>

class WRENReceiver {
public:
  explicit WRENReceiver(const RingConfig& cfg /* TODO: SPSCQueue& queue*/);

  WRENReceiver(const WRENReceiver&) = delete;
  WRENReceiver(WRENReceiver&& other) noexcept;

  WRENReceiver& operator=(const WRENReceiver&) = delete;
  WRENReceiver& operator=(WRENReceiver&& other) noexcept;

  ~WRENReceiver() = default;

  [[noreturn]]
  void beginReceiving() {
    while (true) {
      RxFrame frame = m_ethernetSocket.tryReceive();

      if (!frame.data.empty()){
        if (frame.data.size() >= kFrameSize) [[likely]] {
          // TODO: take the frame.data and push into the SPSC queue
          printDebugLogs(frame);

          m_ethernetSocket.release();
        }
      }

    }
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
      std::uint16_t evId, slot;
      std::uint32_t sec, nsec;
      std::memcpy(&evId, p + 2,  2);
      std::memcpy(&slot, p + 4,  2);
      std::memcpy(&sec,  p + 6,  4);
      std::memcpy(&nsec, p + 10, 4);
      std::printf("ADVANCE  ev_id:%u  slot:%u  due:%u.%09u\n", evId, slot, sec, nsec);
    }
    else if (p[0] == PKT_FIRE) {
      std::uint16_t slot;
      std::uint32_t sec, nsec;
      std::memcpy(&slot, p + 2, 2);
      std::memcpy(&sec,  p + 4, 4);
      std::memcpy(&nsec, p + 8, 4);
      std::printf("FIRE     slot:%u  at:%u.%09u\n", slot, sec, nsec);
    }
  }
};

#endif // ABTWREN_WRENRECEIVER_H
