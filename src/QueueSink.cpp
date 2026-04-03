//
// Created by asherjil on 2/25/26.
//
#include "QueueSink.hpp"

QueueSink::QueueSink(rigtorp::SPSCQueue<TimingEvent> &queue)
  :m_queue{queue} {}