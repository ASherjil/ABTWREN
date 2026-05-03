// ABTWREN - Sidecar Timing Distribution
//
// Transmitter (mkdev30): reads WREN PCIe ring, forwards events via selected transport
// Receiver    (mkdev16): receives events via selected transport, two-thread SPSC arch
//
// Usage:
//   TX:  sudo taskset -c 4 ./abtwren --tx [--config DeviceInstance.toml]
//   RX:  sudo taskset -c 4,5 ./abtwren --rx [--config DeviceInstance.toml]

#include "ConfigParser.hpp"
#include "TransportBackend.hpp"

#include <csignal>
#include <cstdio>
#include <cstring>

static void sigint_handler(int) {
    g_running = 0;
}

int main(int argc, char* argv[]) {
    const char* mode  = nullptr;
    const char* configPath = "DeviceInstance.toml";

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--tx") == 0 || std::strcmp(argv[i], "--rx") == 0) {
            mode = argv[i];
        } else if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            configPath = argv[++i];
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            std::fprintf(stderr, "Usage:\n"
                                  "  %s --tx [--config <path>]   (transmitter)\n"
                                  "  %s --rx [--config <path>]   (receiver)\n",
                         argv[0], argv[0]);
            return 1;
        }
    }

    if (!mode) {
        std::fprintf(stderr, "No mode specified. Use --tx or --rx.\n"
                              "Usage:\n"
                              "  %s --tx [--config <path>]\n"
                              "  %s --rx [--config <path>]\n",
                     argv[0], argv[0]);
        return 1;
    }

    std::signal(SIGINT, sigint_handler);

    std::printf("Loading config: %s\n", configPath);
    SidecarConfig cfg = loadConfig(configPath);

    if (std::strcmp(mode, "--tx") == 0) {
        return runTransmitter(cfg);
    } else {
        return runReceiver(cfg);
    }
}
