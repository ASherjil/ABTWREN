# TXMC635 Direct mmap Driver Reference

> **Source**: VHDL analysis of `tools/pcores/` and `tools/soc_txmc635/`
> **Target**: TXMC635 FPGA (ADAS3022 ADC + AD5764 DAC) via PCIe BAR0 mmap

---

## 1. PCIe Architecture & Memory Map

### Board Topology

The TXMC board has an on-board PCIe switch with two endpoints:

```
CPU ── PCIe Root ── PCIe Switch (on TXMC board)
                      ├── Port 1: MachXO2 CPLD  (carrier board config)
                      └── Port 2: Xilinx FPGA   (ADC/DAC application)
```

| Device | Vendor:Device | Edge Driver | BARs |
|--------|--------------|-------------|------|
| **FPGA** | `0xBAD7:0x7469` | `txmc_user` | BAR0=16MB (cores), BAR1=128MB (DMA) |
| **CPLD** | `0x1498:0x927b` | `txmc_conf` | BAR0=256B (regs), BAR1=256B (ISP data) |

PCI slot addresses (e.g. `0000:34:00.0`) **change between boots**.
Always discover by vendor:device ID, never hardcode.

### FPGA Memory Map (0xBAD7:0x7469)

```
BAR0 = 16 MB (FPGA register cores)  → resource0
BAR1 = 128 MB (DMA RAM)             → resource1
Core Stride = 0x1000 (4KB per core)
```

### Core Address Map (within BAR0)

| Core ID | Offset | Type | Description |
|---------|--------|------|-------------|
| 0 | 0x00000 | MKGen | Marker Generator |
| 1 | 0x01000 | MKDel | Marker Delay |
| 2 | 0x02000 | **ADC** | ADAS3022 #0 (ch 0-7) |
| 3 | 0x03000 | **ADC** | ADAS3022 #1 (ch 0-7) |
| 4 | 0x04000 | **ADC** | ADAS3022 #2 (ch 0-7) |
| 5 | 0x05000 | **ADC** | ADAS3022 #3 (ch 0-7) |
| 6 | 0x06000 | Follower | ADC Voltage Follower |
| 7 | 0x07000 | **DAC** | AD5764 #0 (ch 0-3) |
| 8 | 0x08000 | **DAC** | AD5764 #1 (ch 4-7) |
| 16 | 0x10000 | TG | Timing Generator |
| 32 | 0x20000 | **ADC Demux** | Aggregated 32-ch (4×8) |
| 33 | 0x21000 | SysParams | System Parameters |

### Core ID Values (read at offset shown)

| Core Type | Core ID | Offset |
|-----------|---------|--------|
| MK Generator | `0xBAD70800` | 0x28 |
| Timing Gen | `0xBAD70801` | 0x1C |
| V Follower | `0xBAD70802` | 0x18 |
| **DAC Mux** | `0xBAD70803` | **0x3C** |
| Watchdog | `0xBAD70804` | 0x00 |
| Reg Follower | `0xBAD70805` | 0x18 |
| Intlk Ctrl | `0xBAD7080D` | 0x00 |

---

## 2. Constants & Masks

```cpp
// === Memory Layout ===
constexpr uint32_t CORE_STRIDE       = 0x1000;      // 4KB per core
constexpr uint32_t CHANNEL_STRIDE    = 0x04;        // 4 bytes per channel
constexpr size_t   BAR0_SIZE         = 16*1024*1024;

// === ADC (ADAS3022) ===
constexpr uint32_t ADC_CFG_10V24     = 0x8CCF;      // PGIA=001 → ±10.24V, 312.5µV/LSB
constexpr uint32_t ADC_CFG_20V48     = 0x8FCF;      // PGIA=111 → ±20.48V (chip default after reset)
constexpr uint32_t ADC_CFG_DEFAULT   = ADC_CFG_10V24; // What CHWLib uses
constexpr uint32_t ADC_ACQ_MASK      = 0xFFFF0000;  // Bits [31:16]
constexpr uint32_t ADC_ACQ_SHIFT     = 16;
constexpr uint32_t ADC_CFG_MASK      = 0x0000FFFF;  // Bits [15:0]
constexpr uint32_t ADC_CH_IDX_MASK   = 0x00007000;  // Bits [14:12] in CFG
constexpr uint32_t ADC_CH_IDX_SHIFT  = 12;
constexpr uint32_t ADC_CONFIG_OFFSET = 0x20;
// PGIA[9:7] gain table: 000=±24.576V, 001=±10.24V, 010=±5.12V,
//   011=±2.56V, 100=±1.28V, 101=±0.64V, 111=±20.48V(default)

// === DAC (AD5764) ===
constexpr uint32_t DAC_WRITE_CMD     = 0x00100000;  // OR with 16-bit value
constexpr uint32_t DAC_COARSE_GAIN   = 0x001C0000;  // ±10V bipolar
constexpr uint32_t DAC_VALUE_MASK    = 0x0000FFFF;
constexpr uint32_t DAC_CORE_ID       = 0xBAD70803;
constexpr uint32_t DAC_RESET_OFFSET  = 0x38;
constexpr uint32_t DAC_COREID_OFFSET = 0x3C;

// === Reset Register (0x38) Bits ===
constexpr uint32_t DAC_RESET_RST     = (1 << 0);    // Hardware reset
constexpr uint32_t DAC_RESET_CLR     = (1 << 1);    // Clear
constexpr uint32_t DAC_RESET_SOFT    = (1 << 2);    // Soft reset

// === Channel ID Encoding (CERN format) ===
// channelId = (coreId << 8) | channelIndex
constexpr uint32_t decode_core(int32_t ch)    { return (ch >> 8) & 0x00FFFFFF; }
constexpr uint32_t decode_channel(int32_t ch) { return ch & 0xFF; }
constexpr int32_t  encode_channel(uint32_t core, uint32_t ch) { return (core << 8) | ch; }
```

---

## 3. ADC Core Register Map (ADAS3022)

**10 registers per core, 8 channels**

| Offset | Access | Description |
|--------|--------|-------------|
| 0x00-0x1C | RO | Channel 0-7: `[31:16]=ACQ, [15:0]=CFG` |
| 0x20 | **RW** | Config register (write to set gain) |
| 0x24 | -- | Unused |

### ADC Data Format (32-bit)

```
┌────────────────────┬────────────────────┐
│ [31:16] ACQ Value  │ [15:0] CFG Echo    │
│   16-bit signed    │ CFG register echo  │
└────────────────────┴────────────────────┘
         ↓                    ↓
   ADC reading         [15]=CFG  [14:12]=INx  [11]=COM  [10]=RSV
   (two's complement)  [9:7]=PGIA  [6]=MUX  [5:4]=SEQ
                       [3]=TEMPB  [2]=REFEN  [1]=CMS  [0]=CPHA
```

### ADC Read

```cpp
volatile uint32_t* adc = bar0 + (core_id * 0x1000 / 4);
uint32_t raw = adc[channel];                    // channel = 0-7
int16_t  value = (int16_t)((raw >> 16) & 0xFFFF);  // Signed 16-bit
uint8_t  ch_idx = (raw >> 12) & 0x07;           // Channel index echo
```

### ADC Config Write

```cpp
// Set ±10.24V range for channel (PGIA=001)
adc[0x20/4] = 0x8CCF | (channel << 12);
```

---

## 4. DAC Mux Core Register Map (AD5764)

**16 registers, 8 logical channels → 2 physical DACs**

| Offset | Access | Description |
|--------|--------|-------------|
| 0x00-0x1C | RW | Channel 0-7 value |
| 0x20 | RW | DAC_0 raw SPI register |
| 0x24 | RW | DAC_1 raw SPI register |
| 0x28 | RO | DAC_0 status (bit0=busy) |
| 0x2C | RO | DAC_1 status (bit0=busy) |
| 0x30 | RO | DAC_0 last value |
| 0x34 | RO | DAC_1 last value |
| 0x38 | RW | Reset: `[0]=rst, [1]=clr, [2]=soft` |
| 0x3C | RO | Core ID = `0xBAD70803` |

### DAC Write

```cpp
volatile uint32_t* dac = bar0 + (core_id * 0x1000 / 4);

// Set coarse gain first (once per channel)
dac[channel] = 0x001C0000;  // ±10V range

// Write value
dac[channel] = 0x00100000 | (value & 0xFFFF);  // value = 0-65535
```

### DAC Read

```cpp
uint16_t value = dac[channel] & 0xFFFF;
bool busy = (dac[0x28/4] & 1);  // DAC_0 busy flag
```

---

## 5. ADC Demux Core Register Map

**32 registers (aggregates 4 ADC cores × 8 channels)**

| Offset | Access | Description |
|--------|--------|-------------|
| 0x00-0x1C | RO | ADC0 ch0-7 |
| 0x20-0x3C | RO | ADC1 ch0-7 |
| 0x40-0x5C | RO | ADC2 ch0-7 |
| 0x60-0x7C | RO | ADC3 ch0-7 |

### ADC Demux Data Format (32-bit)

```
┌────────────────────┬────────────────────┐
│ [31:16] = 0x0000   │ [15:0] = ACQ Value │
└────────────────────┴────────────────────┘
```

**Note**: No CFG echo, just raw 16-bit value zero-extended.

```cpp
volatile uint32_t* demux = bar0 + (32 * 0x1000 / 4);  // Core 32
uint16_t adc2_ch5 = demux[2*8 + 5] & 0xFFFF;  // ADC2, channel 5
```

---

## 6. Carrier Board Registers (txmc_conf — MachXO2 CPLD)

| Property | Value |
|----------|-------|
| Vendor:Device | `0x1498:0x927b` |
| Driver | `txmc_conf` v0.2.0 |
| BAR0 | 256 bytes (config registers) |
| BAR1 | 256 bytes (ISP data — **do not touch**) |
| Headers | `/acc/local/deb12x64/drv/edge/modules/txmc_conf/0.2.0/include/txmc_conf/` |

### Register Map (BAR0, 11 × 32-bit registers)

**WARNING**: Offsets are NOT contiguous — extracted from Edge driver hw_desc binary.

| ID | hw_offset | Register | R/W | Safe | Description |
|----|-----------|----------|-----|------|-------------|
| 0 | **0xC0** | `int_enable` | RW | Yes | Interrupt enable |
| 1 | **0xC4** | `int_status` | RW | Yes | Interrupt status |
| 2 | **0xD0** | `conf_control` | RW | Yes | Config control/status |
| 3 | **0xD4** | `conf_data` | RW | Yes | Config data |
| 4 | **0xE0** | `isp_control` | RW | **NO** | ISP control — can brick CPLD |
| 5 | **0xE4** | `isp_config` | RW | **NO** | ISP config — can brick CPLD |
| 6 | **0xE8** | `isp_command` | RW | **NO** | ISP command — can brick CPLD |
| 7 | **0xEC** | `isp_status` | RO | Yes | ISP status |
| 8 | **0xF4** | `io_pull_config` | RW | Yes | IO pull resistor config |
| 9 | **0xF8** | `serial_number` | RO | Yes | Board serial number |
| 10 | **0xFC** | `code_version` | RO | Yes | MachXO2 firmware version |

```cpp
auto cpld = pcie_map(0x1498, 0x927b, 0);
auto* regs = reinterpret_cast<volatile uint32_t*>(
    reinterpret_cast<volatile uint8_t*>(cpld.ptr) + 0xC0);
printf("Serial:  0x%08X\n", cpld.ptr[0xF8 / 4]);  // hw_offset 0xF8
printf("Version: 0x%08X\n", cpld.ptr[0xFC / 4]);   // hw_offset 0xFC
```

---

## 7. Latency Analysis & Diagnostics

### Where does read latency come from?

The FPGA ADC core runs a **continuous autonomous sampling loop** (VHDL FSM):

```
CNV pulse → ADAS3022 converts (~700ns) → SPI readout 33 bits @ ~25MHz (~1.3µs)
→ store in buffer register → next channel → repeat all 8 channels
```

**CPU reads do NOT trigger SPI transactions.** Reads return buffered values via a
combinatorial AXI-Lite mux (~5ns FPGA-internal). The ~3µs read latency is
**100% PCIe round-trip** through the Pericom PI7C9X2G404 switch.

### Proving it: CPLD vs FPGA read latency

Both devices sit behind the same PCIe switch. If CPLD register reads (simple
static registers, no SPI) show the same ~3µs latency, the bottleneck is PCIe.

```cpp
// Read CPLD serial_number (static register, no SPI, no state machine)
auto cpld = pcie_map(0x1498, 0x927b, 0);
auto t0 = steady_clock::now();
volatile uint32_t serial = cpld.ptr[0xF8 / 4];  // hw_offset 0xF8
auto t1 = steady_clock::now();
printf("CPLD read: %ld ns\n", duration_cast<nanoseconds>(t1-t0).count());

// Read FPGA ADC channel (buffered register behind SPI FSM)
auto fpga = pcie_map(0xBAD7, 0x7469, 0);
auto t2 = steady_clock::now();
volatile uint32_t adc = fpga.ptr[2 * 0x400]; // core 2, ch 0
auto t3 = steady_clock::now();
printf("FPGA read: %ld ns\n", duration_cast<nanoseconds>(t3-t2).count());

// If both ~3µs → PCIe is the bottleneck, not ADC/SPI/VHDL
```

### Latency budget

| Component | Latency | Notes |
|-----------|---------|-------|
| PCIe TLP round-trip | **~3 µs** | Through switch, UC memory |
| AXI-Lite register read | ~5 ns | Combinatorial mux in FPGA |
| SPI transaction | ~1.3 µs | 33 bits @ 25 MHz (NOT on read path) |
| ADC conversion (ADAS3022) | ~700 ns | CNV→BUSY deassert (NOT on read path) |
| Full 8-ch sampling cycle | ~18 ms | All 8 channels sequentially |
| **Data staleness** | **0–18 ms** | Time since last buffer update |
| Edge driver overhead | ~1-4 µs | Kernel transition + locking |

**Conclusion**: Software optimization beyond eliminating Edge overhead (~1µs saved)
yields diminishing returns. The PCIe physical layer is the floor at ~3µs per read.
For sub-µs reads, the FPGA would need DMA (push to host RAM) instead of
CPU-initiated MMIO (pull from device).

---

## 8. Voltage Conversion


### ADC: ADAS3022 (16-bit signed two's complement, ±10.24V with 0x8CCF config)

```
Formula: voltage = raw_signed × V_REF / (32768 × PGIA_gain)
         V_REF = 4.096V, PGIA_gain = 0.4 (for ±10.24V)
         → voltage = raw_signed / 32768.0 × 10.24
```

```cpp
constexpr double ADC_VREF = 10.24;  // = 4.096V / 0.4 PGIA gain

double adc_to_volts(int16_t raw) {
    return (raw / 32768.0) * ADC_VREF;
}

int16_t volts_to_adc(double v) {
    return (int16_t)((v / ADC_VREF) * 32768.0);
}
// LSB = 312.5 µV, Range: -10.24V (0x8000) to +10.2397V (0x7FFF)
```

### DAC: AD5764 (16-bit signed two's complement, ±10V bipolar with 0x001C0000 config)

```
Formula: V_OUT = D_signed / 32768.0 × 2 × V_REFIN
         V_REFIN = 5V (standard)
         → V_OUT = D_signed / 32768.0 × 10.0
```

```cpp
constexpr double DAC_VREF = 10.0;  // = 2 × 5V reference

double dac_to_volts(int16_t raw) {
    return (raw / 32768.0) * DAC_VREF;  // 0x8000=−10V, 0x0000=0V, 0x7FFF≈+10V
}

int16_t volts_to_dac(double v) {
    return (int16_t)((v / DAC_VREF) * 32768.0);
}
// LSB = 305.2 µV, Range: -10V (0x8000) to +9.9997V (0x7FFF)
```

---

## 9. Minimal mmap Example

```cpp
int main() {
    // Dynamic discovery — no hardcoded PCI addresses
    auto fpga = pcie_map(0xBAD7, 0x7469, 0);  // FPGA BAR0
    if (!fpga.ptr) { fprintf(stderr, "FPGA not found\n"); return 1; }
    auto bar0 = fpga.ptr;

    // === ADC Read (Core 2, Channel 0) ===
    volatile uint32_t* adc = bar0 + (2 * 0x1000 / 4);
    adc[0x20/4] = 0x8CCF;                       // Config: PGIA=001, ±10.24V, ch0
    uint32_t raw = adc[0];                      // Read channel 0
    int16_t adc_val = (int16_t)(raw >> 16);     // Extract signed value
    double volts = (adc_val / 32768.0) * 10.24;
    printf("ADC: raw=0x%08X val=%d (%.4f V)\n", raw, adc_val, volts);

    // === DAC Write (Core 7, Channel 0) ===
    volatile uint32_t* dac = bar0 + (7 * 0x1000 / 4);
    dac[0] = 0x001C0000;                        // Coarse gain ±10V
    int16_t dac_val = 0;                        // 0V (two's complement)
    dac[0] = 0x00100000 | ((uint16_t)dac_val);  // Write value
    printf("DAC: wrote %d (0V)\n", dac_val);

    // === Verify DAC Core ID ===
    uint32_t core_id = dac[0x3C/4];
    printf("DAC Core ID: 0x%08X (expect 0xBAD70803)\n", core_id);

    munmap((void*)bar0, fpga.size);
    return 0;
}
```

**Compile**: `g++ -std=c++20 -O2 -o test test.cpp && sudo ./test`

---

## 10. Device Discovery & mmap (C++20)

PCI slot addresses change between boots. Scan `/sys/bus/pci/devices/` by vendor:device ID.
Each device directory contains: `vendor`, `device` (hex text files), `resource` (BAR listing),
and `resource0`..`resource5` (mmap-able BAR files).

```cpp
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace fs = std::filesystem;

struct MappedBAR {
    volatile uint32_t* ptr = nullptr;
    size_t size = 0;
    std::string pci_slot;
};

/// Scan sysfs, match vendor:device, read BAR size from "resource" file, mmap it.
MappedBAR pcie_map(uint16_t vendor, uint16_t device, int bar) {
    auto read_hex = [](const fs::path& p) -> uint16_t {
        std::ifstream f(p); uint16_t v{}; f >> std::hex >> v; return v;
    };
    for (const auto& entry : fs::directory_iterator("/sys/bus/pci/devices")) {
        if (read_hex(entry.path() / "vendor") != vendor) continue;
        if (read_hex(entry.path() / "device") != device) continue;

        auto res_path = entry.path() / ("resource" + std::to_string(bar));
        int fd = open(res_path.c_str(), O_RDWR | O_SYNC);
        if (fd < 0) continue;

        // Read BAR size from "resource" file (line N: "start end flags")
        std::ifstream res_file(entry.path() / "resource");
        std::string line;
        for (int i = 0; i <= bar && std::getline(res_file, line); ++i);
        uint64_t start{}, end{};
        sscanf(line.c_str(), "%lx %lx", &start, &end);
        size_t size = end - start + 1;

        void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (ptr == MAP_FAILED) continue;
        return {static_cast<volatile uint32_t*>(ptr), size, entry.path().filename()};
    }
    return {};
}
```

Usage:

```cpp
// FPGA (ADC/DAC cores)
auto fpga = pcie_map(0xBAD7, 0x7469, 0);   // BAR0 = 16MB cores
// Carrier board (serial number, version)
auto cpld = pcie_map(0x1498, 0x927b, 0);   // BAR0 = 256B regs
```

---

## 11. Quick Reference

| Operation | Code |
|-----------|------|
| Core base | `bar0 + (core_id * 0x400)` |
| ADC read | `(int16_t)(core[ch] >> 16)` |
| ADC config | `core[8] = 0x8CCF \| (ch << 12)` |
| DAC write | `core[ch] = 0x00100000 \| val` |
| DAC gain | `core[ch] = 0x001C0000` |
| Read core ID | `core[0x3C/4]` (DAC only) |
| ADC→Volts | `raw / 32768.0 * 10.24` |
| Volts→DAC | `(int16_t)(v/10.0 * 32768)` |

---

## 12. Loopback Test (DAC→ADC)

```cpp
// Connect DAC ch0 output to ADC ch0 input physically
volatile uint32_t* adc = bar0 + (2 * 0x400);  // ADC core 2
volatile uint32_t* dac = bar0 + (7 * 0x400);  // DAC core 7

adc[8] = 0x8CCF;           // ADC config: PGIA=001, ±10.24V
dac[0] = 0x001C0000;       // DAC coarse gain ±10V

double test_v[] = {-5.0, 0.0, 5.0};
for (double v : test_v) {
    int16_t dac_val = (int16_t)((v / 10.0) * 32768.0);
    dac[0] = 0x00100000 | ((uint16_t)dac_val);
    usleep(1000);
    int16_t adc_raw = (int16_t)(adc[0] >> 16);
    double adc_v = (adc_raw / 32768.0) * 10.24;
    printf("Out: %.2fV → In: %.3fV (err: %.3fV)\n", v, adc_v, adc_v - v);
}
```

---

## References

- **Datasheets**: `tools/ADAS3022.pdf` (Rev. D), `tools/AD5764.pdf` (Rev. F)
- **VHDL ADC**: `tools/pcores/.../axilite_slave_adc_adas3022_v1_00_a/`
- **VHDL DAC**: `tools/pcores/.../axilite_slave_dac_mux_v1_00_a/`
- **System**: `tools/soc_txmc635/system.mhs`
- **CHWLib**: `adc/CTXMC635ADCCore.cpp`, `dac/CTXMC635DACMuxCore.cpp`
- **Edge drivers**: `txmc_user` (FPGA, 0xBAD7:0x7469), `txmc_conf` (CPLD, 0x1498:0x927b)
- **Driver headers**: `/acc/local/deb12x64/drv/edge/modules/txmc_user/0.2.0/include/`
- **Key datasheet pages**: ADAS3022 Table 11-12 (p.38, CFG register), Table 7-8 (p.25-27, transfer function & PGIA); AD5764 Table 7-8 (p.20, transfer function)
