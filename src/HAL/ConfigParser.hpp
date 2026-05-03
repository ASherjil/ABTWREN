//
// ConfigParser — SidecarConfig struct + TOML loader for DeviceInstance.toml
//

#ifndef ABTWREN_CONFIGPARSER_HPP
#define ABTWREN_CONFIGPARSER_HPP

#include <toml++/toml.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

struct SidecarConfig {
    // [general]
    std::uint16_t etherType   = 0x88B5;
    int           watchdogSec = 30;

    // [transmitter]
    struct Tx {
        std::string                 transport = "packet_mmap";
        std::string                 interface = "eno2";
        std::string                 driver;
        std::array<std::uint8_t, 6> mac{};
        int                         cpuCore = 4;
    } tx;

    // [receiver]
    struct Rx {
        std::string                 transport = "packet_mmap";
        std::string                 interface = "eno2";
        std::string                 driver;
        std::array<std::uint8_t, 6> mac{};
        int                         pollerCore    = 4;
        int                         processorCore = 5;
        std::size_t                 queueCapacity = 4096;
    } rx;

    // [packet_mmap]
    std::uint32_t mmapBlockSize   = 4096;
    std::uint32_t mmapBlockNumber = 64;

    // [af_xdp]
    std::uint32_t xdpUmemFrameSize = 4096;
    std::uint32_t xdpFrameCount    = 64;
    bool          xdpNeedWakeup    = true;
};

inline std::array<std::uint8_t, 6> parseMac(const std::string& s) {
    std::array<std::uint8_t, 6> mac{};
    unsigned int b[6];
    if (std::sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x",
                    &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        std::fprintf(stderr, "FATAL: Invalid MAC address: %s\n", s.c_str());
        std::abort();
    }
    for (int i = 0; i < 6; ++i)
        mac[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(b[i]);
    return mac;
}

inline SidecarConfig loadConfig(const char* path) {
    auto tbl = toml::parse_file(path);
    SidecarConfig cfg{};

    // [general]
    cfg.etherType   = static_cast<std::uint16_t>(tbl["general"]["ether_type"].value_or(0x88B5));
    cfg.watchdogSec = tbl["general"]["watchdog_sec"].value_or(30);

    // [transmitter]
    using namespace std::string_literals;
    cfg.tx.transport = tbl["transmitter"]["transport"].value_or("packet_mmap"s);
    cfg.tx.interface = tbl["transmitter"]["interface"].value_or("eno2"s);
    cfg.tx.driver    = tbl["transmitter"]["driver"].value_or(""s);
    cfg.tx.mac       = parseMac(tbl["transmitter"]["mac"].value_or(""s));
    cfg.tx.cpuCore   = tbl["transmitter"]["cpu_core"].value_or(4);

    // [receiver]
    cfg.rx.transport      = tbl["receiver"]["transport"].value_or("packet_mmap"s);
    cfg.rx.interface      = tbl["receiver"]["interface"].value_or("eno2"s);
    cfg.rx.driver         = tbl["receiver"]["driver"].value_or(""s);
    cfg.rx.mac            = parseMac(tbl["receiver"]["mac"].value_or(""s));
    cfg.rx.pollerCore     = tbl["receiver"]["poller_core"].value_or(4);
    cfg.rx.processorCore  = tbl["receiver"]["processor_core"].value_or(5);
    cfg.rx.queueCapacity  = static_cast<std::size_t>(tbl["receiver"]["queue_capacity"].value_or(4096LL));

    // [packet_mmap]
    cfg.mmapBlockSize   = static_cast<std::uint32_t>(tbl["packet_mmap"]["block_size"].value_or(4096LL));
    cfg.mmapBlockNumber = static_cast<std::uint32_t>(tbl["packet_mmap"]["block_number"].value_or(64LL));

    // [af_xdp]
    cfg.xdpUmemFrameSize = static_cast<std::uint32_t>(tbl["af_xdp"]["umem_frame_size"].value_or(4096LL));
    cfg.xdpFrameCount    = static_cast<std::uint32_t>(tbl["af_xdp"]["frame_count"].value_or(64LL));
    cfg.xdpNeedWakeup    = tbl["af_xdp"]["need_wakeup"].value_or(true);

    return cfg;
}

#endif // ABTWREN_CONFIGPARSER_HPP
