//
// Created by asherjil on 2/25/26.
//

#include "ShmSink.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

ShmSink::ShmSink(const char* shmName){
  if (m_sharedMemoryRegion.open<ShmEventSlot>(shmName, &ShmEventSlot::seq)) {
    std::fprintf(stderr, "[ShmSink] Shared memory opened OK\n");
  }
  else {
    std::fprintf(stderr, "[ShmSink] FATAL: shared memory open failed: %s\n",
                 std::strerror(errno));
    std::abort();
  }
}