//
// Created by asherjil on 2/25/26.
//

#ifndef ABTWREN_SHMSINK_H
#define ABTWREN_SHMSINK_H

#include "backends/ShmBackend.hpp"
#include "TimingEvent.hpp"

#include <cstdio>

class ShmSink {
public:

  explicit ShmSink(const char* shmName);

  ShmSink(const ShmSink&) = delete;
  ShmSink(ShmSink&&) = default;
  ShmSink& operator=(const ShmSink&) = delete;
  ShmSink& operator=(ShmSink&&) = default;
  ~ShmSink() = default;

  [[gnu::always_inline]]
  inline void push(const TimingEvent& ev) {
    m_sharedMemoryRegion.ptr<ShmEventSlot>()->event = ev;
    m_sharedMemoryRegion.ptr<ShmEventSlot>()->seq.store(++m_seq, std::memory_order_release);

    // TODO: Remove this for final production but keep for now, they are usefull debugs
    if (ev.pktType == PKT_ADVANCE) {
      std::printf("ADVANCE  ev_id:%u  slot:%u  due:%u.%09u\n",
                  ev.eventId, ev.slot, ev.sec, ev.nsec);
    }
    else if (ev.pktType == PKT_FIRE) {
      if (ev.channel == 0xFF)
        std::printf("CTIM_FIRE  ev_id:%u  at:%u.%09u\n",
                    ev.eventId, ev.sec, ev.nsec);
      else
        std::printf("LTIM_FIRE  ev_id:%u  ch:%u  +%ums  at:%u.%09u\n",
                    ev.eventId, ev.channel, ev.offsetMs, ev.sec, ev.nsec);
    }
  }

private:
  ShmBackend<ShmMode::Writer> m_sharedMemoryRegion;
  std::uint64_t m_seq{};

};

#endif // ABTWREN_SHMSINK_H
