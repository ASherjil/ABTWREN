// ABTWREN - Sidecar Timing Distribution
//
// Transmitter (mkdev30): reads WREN PCIe ring, forwards events via packet_mmap
// Receiver    (mkdev16): receives events via packet_mmap, two-thread SPSC arch
//
// Usage:
//   TX:  sudo taskset -c 4 ./abtwren --tx
//   RX:  sudo taskset -c 4,5 ./abtwren --rx

#include "WRENProtocol.hpp"
#include "WRENTransmitter.hpp"
#include "WRENCTIMConfigurator.hpp"
#include "MainReceiver.hpp"
#include <NicTuner.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <sched.h>
#include <pthread.h>
#include <sys/mman.h>
#include <thread>
#include <chrono>

// =============================================================================
// Configuration
// =============================================================================

constexpr const char* kInterface = "eno2";
constexpr int         kCpuCore   = 4;

// cfc-865-mkdev30 (eno2) — transmitter
constexpr std::array<std::uint8_t, 6> kTxMac = {0xd4, 0xf5, 0x27, 0x2a, 0xa9, 0x59};
// cfc-865-mkdev16 (eno2) — receiver
constexpr std::array<std::uint8_t, 6> kRxMac = {0x20, 0x87, 0x56, 0xb6, 0x33, 0x67};

constexpr std::uint32_t kBlockSize   = 4096;
constexpr std::uint32_t kBlockNumber = 64;
constexpr int           kWatchdogSec = 30;

// RX thread architecture
constexpr int         kPollerCore      = 4;
constexpr int         kProcessorCore   = 5;
constexpr std::size_t kQueueCapacity   = 4096;

// =============================================================================
// Globals
// =============================================================================

static volatile std::sig_atomic_t g_running = 1;
static MainReceiver*              g_receiver = nullptr;

static void sigint_handler(int) {
    g_running = 0;
    if (g_receiver) g_receiver->requestStop();
}

// =============================================================================
// TX Watchdog — safety net for RT lockups / unreachable SSH
//
// Runs on core 0 at SCHED_OTHER (normal priority) so it can always fire
// even when the hot path busy-spins at SCHED_FIFO on another core.
// After kWatchdogSec it sets g_running = 0, causing the hot path to
// return normally and destructors to run.
// =============================================================================

static void spawnTxWatchdog() {
    std::thread([](){
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(0, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

        sched_param sp{};
        sp.sched_priority = 0;
        sched_setscheduler(0, SCHED_OTHER, &sp);

        std::this_thread::sleep_for(std::chrono::seconds(kWatchdogSec));
        if (g_running) {
            std::fprintf(stderr, "\n[Watchdog] %ds timeout. Stopping...\n", kWatchdogSec);
            g_running = 0;
        }
    }).detach();
}

// =============================================================================
// RX Watchdog — uses jthread + stop_token, calls MainReceiver::requestStop()
// =============================================================================

static std::jthread spawnRxWatchdog(MainReceiver& receiver) {
    return std::jthread([&receiver](std::stop_token st) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(0, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

        sched_param sp{};
        sp.sched_priority = 0;
        sched_setscheduler(0, SCHED_OTHER, &sp);

        for (int elapsed = 0; elapsed < kWatchdogSec; ++elapsed) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (st.stop_requested()) return;
        }
        std::fprintf(stderr, "\n[Watchdog] %ds timeout. Stopping...\n", kWatchdogSec);
        receiver.requestStop();
    });
}

// =============================================================================
// TX system tuning (applied before hot path)
// =============================================================================

static void applyTxSystemTuning() {
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        std::fprintf(stderr, "[Warn] mlockall failed: %s\n", std::strerror(errno));
    }

    // SCHED_FIFO:49 — below ksoftirqd (FIFO:50 set by NicTuner)
    sched_param sp{};
    sp.sched_priority = 49;
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
        std::fprintf(stderr, "[Warn] SCHED_FIFO:49 failed: %s\n", std::strerror(errno));
    }
}

// =============================================================================
// Transmitter: PCIe ring → packet_mmap TX
// =============================================================================

static void runTransmitter() {
    std::printf("[TX] WREN sidecar transmitter on %s (core %d)\n", kInterface, kCpuCore);
    std::printf("[TX] Watchdog: auto-shutdown in %d seconds\n", kWatchdogSec);

    // NicTuner for TX lives here (stack-allocated, destructor restores settings)
    NicTuner tuner(kInterface, kCpuCore);
    applyTxSystemTuning();
    spawnTxWatchdog();

    RingConfig tx_cfg{};
    tx_cfg.interface     = kInterface;
    tx_cfg.direction     = RingDirection::TX;
    tx_cfg.blockSize     = kBlockSize;
    tx_cfg.blockNumber   = kBlockNumber;
    tx_cfg.protocol      = kEtherType;
    tx_cfg.packetVersion = TPACKET_V2;
    tx_cfg.qdiscBypass   = true;

    WRENTransmitter transmitter(WREN_VENDOR_ID, WREN_DEVICE_ID, WREN_BAR, tx_cfg);
    transmitter.setMacAddresses(kTxMac, kRxMac);  // src=mkdev30, dst=mkdev16

    // Configure 0-offset CTIM fire actions via PCIe mailbox.
    WRENCTIMConfigurator ctimConfig(transmitter.pcie(), {
        {142, 24},  // PIX.AMCLO-CT  → pulser 24
        {143, 25},  // PIX.F900-CT   → pulser 25
        {138, 26},  // PI2X.F900-CT  → pulser 26
        {156, 27},  // PX.SCY-CT     → pulser 27 (Start Cycle, needed by MKController)
    });

    transmitter.installActionMap(ctimConfig.actionMap());

    std::printf("[TX] PCIe open, MAC set, %zu CTIMs configured, %zu actions mapped. Entering poll loop.\n",
                ctimConfig.configuredCount(), ctimConfig.actionMap().size());
    transmitter.transmitAll(g_running);
}

// =============================================================================
// Receiver: packet_mmap RX → SPSC queue → EventProcessor
// =============================================================================

static void runReceiver() {
    std::printf("[RX] WREN sidecar receiver on %s (poller=core%d, processor=core%d)\n",
                kInterface, kPollerCore, kProcessorCore);
    std::printf("[RX] Watchdog: auto-shutdown in %d seconds\n", kWatchdogSec);

    MainReceiver receiver(kInterface, kPollerCore, kProcessorCore, kQueueCapacity);
    g_receiver = &receiver;

    auto watchdog = spawnRxWatchdog(receiver);

    std::printf("[RX] Starting poller + processor threads.\n");
    receiver.start();
    receiver.wait();

    watchdog.request_stop();
    // watchdog jthread joins in its destructor
    g_receiver = nullptr;
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::printf("Usage:\n"
                     "  %s --tx    (transmitter on mkdev30)\n"
                     "  %s --rx    (receiver on mkdev16)\n", argv[0], argv[0]);
        return 1;
    }

    std::signal(SIGINT, sigint_handler);

    if (std::strcmp(argv[1], "--tx") == 0) {
        runTransmitter();
    } else if (std::strcmp(argv[1], "--rx") == 0) {
        runReceiver();
    } else {
        std::fprintf(stderr, "Unknown mode: %s\n", argv[1]);
        return 1;
    }

    std::printf("Clean shutdown.\n");
    return 0;
}
