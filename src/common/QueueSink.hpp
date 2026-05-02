//
// Created by asherjil on 2/25/26.
//

#ifndef ABTWREN_QUEUESINK_HPP
#define ABTWREN_QUEUESINK_HPP

#include "TimingEvent.hpp"
#include <rigtorp/SPSCQueue.h>

class QueueSink {
public:
  explicit QueueSink(rigtorp::SPSCQueue<TimingEvent>& queue);

  [[gnu::always_inline]]
  inline void push(const TimingEvent& ev) {
    if (!m_queue.try_push(ev))[[unlikely]]{
      std::fprintf(stderr, "[QueueSink] Queue full - dropped event %u\n", ev.eventId);
    }
  }
private:
  rigtorp::SPSCQueue<TimingEvent>& m_queue;
};

#endif // ABTWREN_QUEUESINK_HPP
