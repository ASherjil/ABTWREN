// ABTWREN - Sidecar Timing Distribution
//
// Transmitter (mkdev30): reads WREN PCIe ring, forwards events via packet_mmap
// Receiver    (mkdev16): receives events via packet_mmap, logs ADVANCE/FIRE
//
// Usage:
//   TX:  sudo taskset -c 4 ./abtwren --tx
//   RX:  sudo taskset -c 4 ./abtwren --rx

#include "WRENProtocol.hpp"
#include "WRENTransmitter.hpp"
#include "WRENCTIMConfigurator.hpp"
#include "WRENReceiver.hpp"
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

// =============================================================================
// Globals
// =============================================================================

static volatile std::sig_atomic_t g_running = 1;

static void sigint_handler(int) { g_running = 0; }

// =============================================================================
// Watchdog — safety net for RT lockups / unreachable SSH
//
// Runs on core 0 at SCHED_OTHER (normal priority) so it can always fire
// even when the hot path busy-spins at SCHED_FIFO on another core.
// After kWatchdogSec it sets g_running = 0, causing the hot path to
// return normally and destructors to run.
// =============================================================================

static void spawnWatchdog() {
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
// System tuning (applied before hot path)
// =============================================================================

static void applySystemTuning() {
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        std::fprintf(stderr, "[Warn] mlockall failed: %s\n", std::strerror(errno));
    }

    // SCHED_FIFO:49 — below ksoftirqd (FIFO:50 set by NicTuner)
    // so NAPI can still deliver packets to the mmap ring.
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
    // Uses free pulser channels (9-19, 24-30 available; 1-8,20-23,31-32 in use).
    // Destructor deletes actions+conditions from firmware on shutdown.
    WRENCTIMConfigurator ctimConfig(transmitter.pcie(), {
        {142, 24},  // PIX.AMCLO-CT  → pulser 24
        {143, 25},  // PIX.F900-CT   → pulser 25
        {138, 26},  // PI2X.F900-CT  → pulser 26
    });

    std::printf("[TX] PCIe open, MAC set, %zu CTIMs configured. Entering poll loop.\n",
                ctimConfig.configuredCount());
    transmitter.transmitAll(g_running);
    // Returns here when g_running goes false — destructors clean up PCIe + socket
    // ctimConfig destructor deletes mailbox actions/conditions before transmitter closes PCIe
}

// =============================================================================
// Receiver: packet_mmap RX → parse + log
// =============================================================================

static void runReceiver() {
    std::printf("[RX] WREN sidecar receiver on %s (core %d)\n", kInterface, kCpuCore);
    std::printf("[RX] Watchdog: auto-shutdown in %d seconds\n", kWatchdogSec);

    RingConfig rx_cfg{};
    rx_cfg.interface   = kInterface;
    rx_cfg.direction   = RingDirection::RX;
    rx_cfg.blockSize   = kBlockSize;
    rx_cfg.blockNumber = kBlockNumber;
    rx_cfg.protocol    = kEtherType;
    rx_cfg.hwTimeStamp = false;

    WRENReceiver receiver(rx_cfg);

    std::printf("[RX] Listening for ADVANCE/FIRE packets. Entering poll loop.\n");
    receiver.beginReceiving(g_running);
    // Returns here when g_running goes false — destructor cleans up socket
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

    // NicTuner: disable coalescing, offloads, RT throttling,
    // pin NIC IRQs to kCpuCore, boost ksoftirqd to FIFO:50,
    // move all other IRQs off kCpuCore.
    NicTuner tuner(kInterface, kCpuCore);
    applySystemTuning();
    spawnWatchdog();

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
    // NicTuner destructor restores original system settings
}
