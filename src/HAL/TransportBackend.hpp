//
// TransportBackend — platform-aware transport dispatch + TX/RX orchestration
//
// Creates the right TxRing/RxRing based on SidecarConfig, using if constexpr
// for compile-time platform selection (no #ifdef in dispatch logic).
// Transport objects (NIC + TapBridge) are created outside the Tx/Rx branch
// since both directions share the same object when using PMD transports.
//

#ifndef ABTWREN_TRANSPORTBACKEND_HPP
#define ABTWREN_TRANSPORTBACKEND_HPP

#include "ConfigParser.hpp"
#include "EventProcessor.hpp"
#include "MainReceiver.hpp"
#include "QueueSink.hpp"
#include "ShmSink.hpp"
#include "WRENCTIMConfigurator.hpp"
#include "WRENProtocol.hpp"
#include "WRENTransmitter.hpp"

#include <AFXDPSocket.hpp>
#include <AFXDPTx.hpp>
#include <AFXDPRx.hpp>
#include <Cadence_GEM.hpp>
#include <Intel_I210.hpp>
#include <NicTuner.hpp>
#include <PacketMmapRx.hpp>
#include <PacketMmapTx.hpp>
#include <RingConcepts.hpp>
#include <TapBridge.hpp>
#include <common/HugePageHelpers.hpp>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <thread>

// ═══════════════════════════════════════════════════════════════════════
// Platform detection — single #if location, everything else uses if constexpr
// ═══════════════════════════════════════════════════════════════════════

enum class Platform { x86_64, aarch64, unknown };

#if defined(__x86_64__)
inline constexpr Platform kPlatform = Platform::x86_64;
#elif defined(__aarch64__)
inline constexpr Platform kPlatform = Platform::aarch64;
#else
inline constexpr Platform kPlatform = Platform::unknown;
#endif

enum class TransportMode { Tx, Rx };

// ── Global running flag (set by SIGINT / watchdog) ─────────────────────

inline volatile std::sig_atomic_t g_running = 1;

// ═══════════════════════════════════════════════════════════════════════
// Helpers
// ═══════════════════════════════════════════════════════════════════════

inline void applyTxSystemTuning() {
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        std::fprintf(stderr, "[Warn] mlockall failed: %s\n", std::strerror(errno));

    sched_param sp{};
    sp.sched_priority = 49;
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
        std::fprintf(stderr, "[Warn] SCHED_FIFO:49 failed: %s\n", std::strerror(errno));
}

inline void spawnTxWatchdog(int watchdogSec) {
    std::thread([watchdogSec]() {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(0, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

        sched_param sp{};
        sp.sched_priority = 0;
        sched_setscheduler(0, SCHED_OTHER, &sp);

        std::this_thread::sleep_for(std::chrono::seconds(watchdogSec));
        if (g_running) {
            std::fprintf(stderr, "\n[Watchdog] %ds timeout. Stopping...\n", watchdogSec);
            g_running = 0;
        }
    }).detach();
}

template<typename Receiver>
std::jthread spawnRxWatchdog(Receiver& receiver, int watchdogSec) {
    return std::jthread([&receiver, watchdogSec](std::stop_token st) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(0, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

        sched_param sp{};
        sp.sched_priority = 0;
        sched_setscheduler(0, SCHED_OTHER, &sp);

        for (int elapsed = 0; elapsed < watchdogSec; ++elapsed) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (st.stop_requested()) return;
        }
        std::fprintf(stderr, "\n[Watchdog] %ds timeout. Stopping...\n", watchdogSec);
        receiver.requestStop();
    });
}

template<typename Receiver>
void runUntilSignal(Receiver& receiver, int watchdogSec) {
    std::jthread watchdog = spawnRxWatchdog(receiver, watchdogSec);
    receiver.start();

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    receiver.requestStop();
    receiver.wait();
    watchdog.request_stop();
}

template<TxRing Tx, RxRing Rx>
std::jthread spawnTapDeviceThread(TapBridge<Tx, Rx>& tap) {
    auto t = std::jthread([&tap](std::stop_token st) {
        sched_param sp{};
        pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);

        cpu_set_t all;
        CPU_ZERO(&all);
        for (int c = 0; c < CPU_SETSIZE; ++c) CPU_SET(c, &all);
        pthread_setaffinity_np(pthread_self(), sizeof(all), &all);

        tap(st);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return t;
}

// ═══════════════════════════════════════════════════════════════════════
// WREN-specific setup — called after transport is created
// ═══════════════════════════════════════════════════════════════════════

template<TxRing Tx>
int runTransmitterWith(Tx& tx, const SidecarConfig& cfg) {
    std::printf("[TX] WREN sidecar transmitter on %s (core %d)\n",
                cfg.tx.interface.c_str(), cfg.tx.cpuCore);
    std::printf("[TX] Watchdog: auto-shutdown in %d seconds\n", cfg.watchdogSec);

    NicTuner tuner(cfg.tx.interface.c_str(), cfg.tx.cpuCore, NicTunerMode::Full);
    applyTxSystemTuning();
    spawnTxWatchdog(cfg.watchdogSec);

    WRENTransmitter transmitter(WREN_VENDOR_ID, WREN_DEVICE_ID, WREN_BAR, tx);
    transmitter.setMacAddresses(cfg.tx.mac, cfg.rx.mac);

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

    std::printf("[TX] Clean shutdown.\n");
    return 0;
}

template<RxRing Rx>
int runReceiverWith(Rx& rx, const SidecarConfig& cfg) {
    std::printf("[RX] WREN sidecar receiver on %s (poller=core%d, processor=core%d)\n",
                cfg.rx.interface.c_str(), cfg.rx.pollerCore, cfg.rx.processorCore);
    std::printf("[RX] Watchdog: auto-shutdown in %d seconds\n", cfg.watchdogSec);

    NicTuner tuner(cfg.rx.interface.c_str(), cfg.rx.pollerCore, NicTunerMode::Full);

#ifdef ABTWREN_USE_QUEUE
    rigtorp::SPSCQueue<TimingEvent> queue(cfg.rx.queueCapacity);
    QueueSink sink(queue);
    EventProcessor processor(kShmName, queue);
    MainReceiver<Rx, QueueSink> receiver(
        cfg.rx.interface.c_str(), cfg.rx.pollerCore, rx, sink, &processor, cfg.rx.processorCore);
    runUntilSignal(receiver, cfg.watchdogSec);
#else
    ShmSink sink(kShmName);
    MainReceiver<Rx, ShmSink> receiver(
        cfg.rx.interface.c_str(), cfg.rx.pollerCore, rx, sink);
    runUntilSignal(receiver, cfg.watchdogSec);
#endif
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════
// Transport dispatch — single template, transport objects shared across Tx/Rx
// ═══════════════════════════════════════════════════════════════════════

template<TransportMode Mode>
int dispatchTransport(const SidecarConfig& cfg) {
    const auto& transport = [&]() -> const std::string& {
        if constexpr (Mode == TransportMode::Tx) return cfg.tx.transport;
        else return cfg.rx.transport;
    }();

    const auto& iface = [&]() -> const std::string& {
        if constexpr (Mode == TransportMode::Tx) return cfg.tx.interface;
        else return cfg.rx.interface;
    }();

    // ── packet_mmap — split Rx/Tx ring configs ────────────────────────

    if (transport == "packet_mmap") {
        if constexpr (Mode == TransportMode::Tx) {
            RingConfig tx_cfg{};
            tx_cfg.interface     = iface.c_str();
            tx_cfg.direction     = RingDirection::TX;
            tx_cfg.blockSize     = cfg.mmapBlockSize;
            tx_cfg.blockNumber   = cfg.mmapBlockNumber;
            tx_cfg.protocol      = cfg.etherType;
            tx_cfg.packetVersion = TPACKET_V2;
            tx_cfg.qdiscBypass   = true;

            PacketMmapTx tx(tx_cfg);
            std::printf("[TX] Transport: packet_mmap on %s\n", iface.c_str());
            return runTransmitterWith(tx, cfg);
        } else {
            RingConfig rx_cfg{};
            rx_cfg.interface     = iface.c_str();
            rx_cfg.direction     = RingDirection::RX;
            rx_cfg.blockSize     = cfg.mmapBlockSize;
            rx_cfg.blockNumber   = cfg.mmapBlockNumber;
            rx_cfg.protocol      = cfg.etherType;
            rx_cfg.hwTimeStamp   = false;

            PacketMmapRx rx(rx_cfg);
            std::printf("[RX] Transport: packet_mmap on %s\n", iface.c_str());
            return runReceiverWith(rx, cfg);
        }
    }

    // ── af_xdp — common XdpConfig, only Tx/Rx wrapper differs ─────────

    if (transport == "af_xdp") {
        XdpConfig xdp_cfg{};
        xdp_cfg.interface  = iface.c_str();
        xdp_cfg.queueId    = 0;
        xdp_cfg.frameSize  = cfg.xdpUmemFrameSize;
        xdp_cfg.frameCount = cfg.xdpFrameCount;
        xdp_cfg.etherType  = cfg.etherType;
        xdp_cfg.needWakeup = cfg.xdpNeedWakeup;

        AFXDPSocket sock(xdp_cfg);

        if constexpr (Mode == TransportMode::Tx) {
            AFXDPTx tx(sock);
            std::printf("[TX] Transport: af_xdp on %s\n", iface.c_str());
            return runTransmitterWith(tx, cfg);
        } else {
            AFXDPRx rx(sock);
            std::printf("[RX] Transport: af_xdp on %s\n", iface.c_str());
            return runReceiverWith(rx, cfg);
        }
    }

    // ── intel_i210 — x86_64 only, always RxTx (shared NIC object) ─────

    if constexpr (kPlatform == Platform::x86_64) {
        if (transport == "intel_i210") {
            if (!ensureHugepages(16)) {
                std::fprintf(stderr, "Error: hugepage allocation failed\n");
                return 1;
            }

            const auto& drv_str = [&]() -> const std::string& {
                if constexpr (Mode == TransportMode::Tx) return cfg.tx.driver;
                else return cfg.rx.driver;
            }();
            const std::string_view drv = drv_str.empty() ? "igb" : drv_str;

            Intel_I210<DriverMode::RxTx> nic(iface, 0, drv);
            if (!nic.init()) {
                std::fprintf(stderr, "Error: Intel I210 init failed\n");
                return 1;
            }

            if constexpr (Mode == TransportMode::Tx) {
                std::printf("[TX] Transport: intel_i210 (PMD) on %s (driver=%s)\n",
                            iface.c_str(), drv.data());
                return runTransmitterWith(nic, cfg);
            } else {
                std::printf("[RX] Transport: intel_i210 (PMD) on %s (driver=%s)\n",
                            iface.c_str(), drv.data());
                return runReceiverWith(nic, cfg);
            }
        }
    }

    // ── cadence_gem — aarch64 only, always RxTx + TapBridge ────────────
    //
    // Cadence GEM is the only NIC on Zynq SoCs. Both TX and RX must keep
    // kernel traffic (NFS, SSH, ARP) flowing via the TAP bridge while the
    // PMD owns the hardware.

    if constexpr (kPlatform == Platform::aarch64) {
        if (transport == "cadence_gem") {
            if (!ensureHugepages(16)) {
                std::fprintf(stderr, "Error: hugepage allocation failed\n");
                return 1;
            }

            const auto& drv_str = [&]() -> const std::string& {
                if constexpr (Mode == TransportMode::Tx) return cfg.tx.driver;
                else return cfg.rx.driver;
            }();
            const std::string_view drv = drv_str.empty() ? "macb" : drv_str;

            Cadence_GEM<GEMDriverMode::RxTx> nic(iface, drv);
            if (!nic.init()) {
                std::fprintf(stderr, "Error: Cadence GEM init failed\n");
                return 1;
            }

            auto& slow = nic.slowPath();
            const std::string tapName = "tap_" + iface;
            TapBridge tap(slow, slow, tapName);
            (void)tap.setupAlias(nic.macAddress(), nic.savedAddr(), nic.savedGateway());
            std::jthread tapThread = spawnTapDeviceThread(tap);

            if constexpr (Mode == TransportMode::Tx) {
                std::printf("[TX] Transport: cadence_gem (PMD) on %s (driver=%s, tap=%s)\n",
                            iface.c_str(), drv.data(), tapName.c_str());
                return runTransmitterWith(nic, cfg);
            } else {
                std::printf("[RX] Transport: cadence_gem (PMD) on %s (driver=%s, tap=%s)\n",
                            iface.c_str(), drv.data(), tapName.c_str());
                return runReceiverWith(nic, cfg);
            }
        }
    }

    std::fprintf(stderr, "Error: unknown transport '%s'\n", transport.c_str());
    return 1;
}

// ═══════════════════════════════════════════════════════════════════════
// User-facing entry points
// ═══════════════════════════════════════════════════════════════════════

inline int runTransmitter(const SidecarConfig& cfg) {
    return dispatchTransport<TransportMode::Tx>(cfg);
}

inline int runReceiver(const SidecarConfig& cfg) {
    return dispatchTransport<TransportMode::Rx>(cfg);
}

#endif // ABTWREN_TRANSPORTBACKEND_HPP
