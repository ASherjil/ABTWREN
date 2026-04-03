//
// Created by asherjil on 2/25/26.
//

#include "ShmSink.hpp"

#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

ShmSink::ShmSink(const char* shmName)
  :m_shmName{shmName}{

  //1. Create/open the shared memory object
  m_shmFd = ::shm_open(m_shmName.c_str(), O_CREAT | O_RDWR, 0666);
  if (m_shmFd < 0) {
    std::fprintf(stderr, "[ShmSink] shm_open failed: %s\n", m_shmName.c_str());
    return;
  }
  // O_CREAT -> create if it doesn't exist
  // 0666 -> Both processes can read/write

  // 2. Set size to exactly one cache line, 64 bytes
  ::ftruncate(m_shmFd, sizeof(ShmEventSlot));

  // 3. Perform the mmap into our address space and store the pointer
  void* ptr = ::mmap(
      nullptr,
      sizeof(ShmEventSlot),
      PROT_READ | PROT_WRITE,
      MAP_SHARED, // visible to other processes
      m_shmFd,
      0);

  if (ptr == MAP_FAILED) {
    std::fprintf(stderr, "[ShmSink] mmap failed.\n");
    return;
  }

  m_shmSlot = static_cast<ShmEventSlot*>(ptr); // store as member variable

  // 4. Initialise the atomic seq to 0, atomic store cannot be used on uninitialised memory
  new (&m_shmSlot -> seq) std::atomic<std::uint64_t>(0);
}

ShmSink::~ShmSink() {
  if (m_shmSlot) {
    ::munmap(m_shmSlot, sizeof(ShmEventSlot)); // undo the mmap
    ::close(m_shmFd); // close the file descriptor
    ::shm_unlink(m_shmName.c_str()); // delete the /dev/shm/abtwren_events
  }
}