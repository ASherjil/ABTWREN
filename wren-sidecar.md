# WREN Sidecar Spy Poller — Technical Reference

## WREN PCI Identification
- Vendor: `0x10DC` (CERN)
- Device: `0x0455`
- sysfs BAR1: `/sys/bus/pci/devices/<BDF>/resource1`

## BAR1 Memory Map

### Host Registers (BAR1 + 0x0000)
| Offset | Register | Description |
|--------|----------|-------------|
| 0x00 | IDENT | `0x5745524E` ("WREN") — use to verify mapping |
| 0x10 | WR_STATE | bit 0: link_up, bit 1: time_valid |
| 0x14 | TM_TAI_LO | Current TAI seconds (low 32b) |
| 0x18 | TM_TAI_HI | Current TAI seconds (high bits) |
| 0x1C | TM_CYCLES | 28-bit cycle counter @ 62.5MHz (16ns/tick) |
| 0x20 | TM_COMPACT | Compact time: cycles[27:0] + tai_sec[31:28] |
| 0x28 | ISR | Interrupt status (masked), bit 4 = ASYNC |
| 0x2C | ISR_RAW | Raw interrupt status (read-only, safe) |
| 0x30 | IMR | Interrupt mask |
| 0x34 | IACK | Interrupt ack (write-only, DO NOT touch) |

### Mailbox (BAR1 + 0x10000)
| Offset | Region | Size |
|--------|--------|------|
| 0x0000 | B2H (board-to-host sync reply) | 4KB |
| 0x1000 | H2B (host-to-board sync command) | 4KB |
| 0x2000 | async_data[0..2047] (ring buffer) | 8KB |
| 0x4000 | async_board_off (write pointer) | 4B |
| 0x4004 | async_host_off (read pointer) | 4B |

**Absolute offsets from BAR1 base:**
```
ASYNC_DATA_BASE  = BAR1 + 0x12000
ASYNC_BOARD_OFF  = BAR1 + 0x14000
ASYNC_HOST_OFF   = BAR1 + 0x14004  (never write this)
```

## Ring Buffer Protocol

- 2048 x 32-bit words, indices 0-2047
- Producer: R5F firmware writes data, advances async_board_off
- Consumer: kernel driver reads data, advances async_host_off
- Sidecar: reads data, never modifies async_host_off (non-destructive peek)
- Empty when: `async_board_off == async_host_off`
- Wrap mask: `(offset + len) & 2047`

## Capsule Header Format (1 word)

```
bits [7:0]   = typ          (message type)
bits [15:8]  = source_idx   (RX source index)
bits [31:16] = len          (total length in 32-bit words, including header)
```

### Message Types
| typ | Constant | Meaning |
|-----|----------|---------|
| 0x01 | CMD_ASYNC_CONTEXT | CTIM context received |
| 0x02 | CMD_ASYNC_EVENT | LTIM/CTIM event received |
| 0x03 | CMD_ASYNC_CONFIG | Comparator loaded (action triggered) |
| 0x04 | CMD_ASYNC_PULSE | Pulse generated (with timestamp) |
| 0x06 | CMD_ASYNC_PROMISC | Raw packet (debug mode) |
| 0x07 | CMD_ASYNC_CONT | Continuation of large payload |
| 0x08 | CMD_ASYNC_REL_ACT | Action released |

## Event Capsule Layout (typ=0x02)

```
word 0: header        { typ=0x02, source_idx, len }
word 1: IDs           { ev_id[15:0], ctxt_id[31:16] }
word 2: ts.nsec       (nanoseconds, TAI)
word 3: ts.sec        (seconds, TAI)
word 4+: parameters   (TLV-encoded, optional)
```

- `ev_id`: uint16_t, range 0-511
- `ctxt_id`: uint16_t, 0-127 valid, 0xFF = no context
- `ts`: the event's **due time** (scheduled action time, not reception time)

## Pulse Capsule Layout (typ=0x04)

```
word 0: header        { typ=0x04, source_idx, len }
word 1: comp_idx      (comparator/action index — NOT the same as LTIM channel number)
word 2: ts.sec        (TAI seconds)
word 3: ts.nsec       (nanoseconds, raw — NOT cycles, despite firmware comment)
```

**Important corrections** (verified empirically on real WREN hardware, Feb 2026):
- The firmware source comment says word 3 is "30-bit cycle counter (16ns resolution)" — **this is wrong**.
  The value is already raw nanoseconds. Multiplying by 16 produces values >1 second (nonsensical).
- `comp_idx` is NOT the LTIM channel number. Example: WRLTIM_0-23_EC (channel 23) → `comp_idx=1`.
- Word order is reversed vs event capsule: pulse has `sec, nsec` (not `nsec, sec`).

## Config Capsule Layout (typ=0x03)

```
word 0: header        { typ=0x03, source_idx, len=2 }
word 1: identifiers   { act_idx[15:0], sw_cmp_idx[31:16] }
```

- `act_idx`: firmware action index (same as LTIM slot number from `wrentest ltim list`)
- `sw_cmp_idx`: software comparator index (allocated by firmware, 0-511)
- Written by firmware `async_send_config()` when a hardware comparator is loaded
- Maps act_idx → sw_cmp_idx; PULSE capsule later reports sw_cmp_idx when it fires

**Critical bug found (Feb 2026):** Our code confused act_idx with sw_cmp_idx.
CONFIG gives `[act_idx:16][sw_cmp_idx:16]`, PULSE gives `sw_cmp_idx`.
The correct reverse mapping: CONFIG populates `m_swCmpInfo[sw_cmp_idx]` with act_idx,
then PULSE looks up `m_swCmpInfo[sw_cmp_idx]` to dispatch.

## Event ID Examples (from timing-domain-cern)

| Name | Domain | Numeric ID |
|------|--------|------------|
| PX.SCY-CT | CPS | 156 |
| BX.SCY-CT | PSB | 351 |
| PX.FCY600-CT | CPS | 151 |

Event IDs are 32-bit in FESA (`EventDescriptor::getId()`) but 16-bit on the wire.

## Busy-Poll Detection Algorithm

```cpp
// One-time setup
uint32_t shadow_off = read32(ASYNC_BOARD_OFF);  // sync to current position

// Hot loop (pinned core, SCHED_FIFO, mlockall)
while (running) {
    uint32_t board_off = read32(ASYNC_BOARD_OFF);  // ~2.6us PCIe read

    if (board_off == shadow_off)
        continue;  // no new data

    // Process all new messages
    while (shadow_off != board_off) {
        uint32_t hdr = read32(ASYNC_DATA_BASE + shadow_off * 4);
        uint8_t  typ = hdr & 0xFF;
        uint16_t len = (hdr >> 16) & 0xFFFF;

        if (typ == 0x02) {  // CMD_ASYNC_EVENT
            uint32_t ids = read32(ASYNC_DATA_BASE + ((shadow_off + 1) & 2047) * 4);
            uint16_t ev_id = ids & 0xFFFF;

            if (ev_id == target_event_id) {
                // Read timestamp if needed
                // uint32_t nsec = read32(ASYNC_DATA_BASE + ((shadow_off+2)&2047)*4);
                // uint32_t sec  = read32(ASYNC_DATA_BASE + ((shadow_off+3)&2047)*4);
                fire_trigger_packet();
            }
        }

        shadow_off = (shadow_off + len) & 2047;  // advance with wrap
    }
}
```

### Per-iteration costs (measured, 2x 64-bit optimized reads)
| Step | Latency |
|------|---------|
| Poll async_board_off (1x 32-bit PCIe read) | ~1.3us |
| Parse full 4-word capsule (2x 64-bit PCIe reads) | ~1.9us typical, ~3.5us P100 (SMI) |
| Match + decode | <100ns |

Average detection: ~0.65us (half poll interval) + ~1.9us (parse) = **~2.6us typical**
P100 worst case: ~0.65us + ~3.5us = **~4.2us** (SMI hit on parse reads)

**64-bit read optimization**: Adjacent 32-bit registers are paired into single `volatile uint64_t*`
dereferences, cutting PCIe round-trips from 4 to 2 per capsule. 128-bit SSE (`_mm_loadu_si128`)
was tested but regresses — CPU splits it into 4x 32-bit reads on UC MMIO (Intel SDM confirmed).

## PCIe Mailbox Command Protocol

The WREN firmware exposes a synchronous command/reply mailbox via PCIe BAR1.
Used to configure conditions and actions at startup (not on hot path).

**Register Offsets (from BAR1 base):**

| Register | Offset | Direction |
|----------|--------|-----------|
| B2H CSR | 0x10000 | Read (poll for READY) |
| B2H CMD | 0x10008 | Read (reply command) |
| B2H LEN | 0x1000C | Read (reply length) |
| B2H DATA | 0x10010 | Read (reply data words) |
| H2B CSR | 0x11000 | Write MB_CSR_READY=1 to send |
| H2B CMD | 0x11008 | Write command ID |
| H2B LEN | 0x1100C | Write data length in words |
| H2B DATA | 0x11010 | Write data words |

**Handshake** (from `wren-core.c:484 wren_mb_msg`):
1. Write data words to H2B_DATA
2. Write command ID to H2B_CMD, length to H2B_LEN
3. Write `MB_CSR_READY` (0x1) to H2B_CSR → firmware processes
4. Poll B2H_CSR for `MB_CSR_READY`
5. Read B2H_CMD: `& CMD_REPLY (0x80000000)` = success, `& CMD_ERROR (0x40000000)` = failure
6. Write 0 to B2H_CSR to acknowledge

**Key Command IDs** (from `wren-mb-cmds.def` enum):
- `CMD_RX_SUBSCRIBE` = 38: subscribe to event ID on source/domain
- `CMD_RX_SET_COND` = 18: create condition matching event ID
- `CMD_RX_SET_ACTION` = 21: create action with pulser config + time offset

**Safety:** Only send during startup. Firmware is single-threaded. Avoid concurrent
mailbox access with kernel driver or wrentest. PCIe write ordering guarantees visibility.

## 0-Offset CTIM Fire Actions

To get CTIM fire at exact time T (not just ADVANCE seconds early):
1. Subscribe to target CTIM event on source 0
2. Create condition matching the event ID (high cond_idx to avoid LTIM conflicts)
3. Create action with `load_off_sec=0, load_off_nsec=0` and INT_EN flag

Firmware computes `due_time = event_ts + 0`, loads hardware comparator, which fires
immediately → `pulses_handler()` writes `CMD_ASYNC_PULSE` to async ring.

**Pulser config:**
- pulser_idx=31 (dedicated, unused by LTIMs), flags=0x84 (INT_EN|ENABLE)
- width/period/npulses/idelay: 0, load_off_sec/nsec: 0

**Firmware limits:** MAX_RX_CONDS=1152, MAX_RX_ACTIONS=2048, NBR_SW_COMP=512,
WREN_NBR_PULSERS=32, WREN_NBR_COMPARATORS=256.
Use high indices (cond 1100+, act 2040+) to avoid LTIM driver conflicts.

## Configuration Approach: Direct Mailbox vs wrentest

Two ways to configure the 0-offset CTIM fire actions on the WREN firmware:

### Option 1: Direct PCIe Mailbox (chosen)

Write CMD_RX_SUBSCRIBE / CMD_RX_SET_COND / CMD_RX_SET_ACTION directly to
BAR1 mailbox registers from our sidecar process at startup.

**Pros:**
- We control act_idx (2040+) and cond_idx (1100+) — trivial CONFIG/PULSE mapping
- No external process dependency
- Sub-millisecond startup (3 commands × ~100µs each)

**Cons:**
- Must not overlap with kernel driver mailbox access (startup-only, safe in practice)
- Bypasses driver mutex (OK because wrentest/kernel driver not running during sidecar init)

### Option 2: wrentest ltim create (alternative)

Shell out via `system("wrentest ltim create --event <evId> --channel 32 --load-offset 0 --int-enabled")`.
Uses `WrenReceiver::createTrigger()` → kernel ioctl → firmware mailbox (properly serialized).

**Pros:**
- Serialized through kernel mutex — safe if other processes use the WREN
- Well-tested code path (same as LTIM driver)

**Cons:**
- Driver auto-allocates `config_idx` (act_idx) — we can't predict what value CONFIG
  capsules will report, making the sw_cmp_idx → act_idx → event_id mapping harder
- Would need to parse `wrentest ltim list` output to discover assigned config_ids
- Channel range 1-32 (1-indexed, maps to pulsers 0-31); channel 32 = pulser 31
- External binary dependency; `--load-offset` is in milliseconds (0 = immediate fire)

### Decision

Direct mailbox chosen because the CONFIG/PULSE dispatch depends on knowing act_idx
at compile time. With wrentest, the firmware assigns act_idx dynamically, requiring
runtime discovery (parse `wrentest ltim list`) and a more complex mapping table.

One LTIM per CTIM is required — firmware conditions match exactly one `evt_id`
(line 836 of `wrenrx-cmd.c`: `cond->cond.evt_id = arg->cond.evt_id`). So 3 CTIMs = 3
new conditions + 3 new actions, regardless of approach.

### Upstream Stability (verified Feb 20, 2026)

Checked 20 recent commits to both `wren-gw` and `timing-wrt` repos after `git pull`.
**No changes** to any of the following:
- `wren-mb-cmds.def` (command ID enum) — UNCHANGED
- `mb_map.h` (mailbox memory layout) — UNCHANGED
- `cmd_rx_subscribe`, `cmd_rx_set_cond`, `cmd_rx_set_action` handlers in `wrenrx-cmd.c`
- `wren_mb_msg` handshake in `wren-core.c`
- `exec_host_command` dispatch in `main.c`

Changes found were all non-breaking: include renames (`wren-hw.h` → `mb.h`), evlog
context header modifications, `wren_mb_hw_config` struct extensions, log read pin range
fixes. Our mailbox command structs (`wren_mb_rx_subscribe`, `wren_mb_rx_set_cond`,
`wren_mb_rx_set_action`, `wren_mb_pulser_config`) are untouched.

## Forwarding Strategy

**Events arrive seconds in advance.** The `ts` field is the **due time** (future TAI time
when the accelerator event occurs), not the reception time. The sidecar catches events
seconds before they're due.

**Strategy: forward immediately, always.** Catch event from ring buffer, send via
packet_mmap to remote FEC with the due time embedded in the payload. No timers, no
holding. The remote FEC decides:
- `due_time > now + threshold` → schedule action at due_time (normal, seconds of margin)
- `due_time <= now` → act immediately (emergency/zero-lead event)

This handles both normal events (seconds of lead time) and emergency events (~17-32us
forwarding latency) with identical sidecar logic.

## Expected Event Load

Typical subscription: 1-2 CTIMs + ~5 LTIMs = ~7 events per cycle.
Multiple events often arrive in the same WRT frame (firmware writes them back-to-back).

| Scenario | Sidecar Processing Time |
|----------|------------------------|
| 1 event | ~2.6us detect + ~3us TX = **~5.6us** |
| 7 events (batch) | ~2.6us detect + 7 x (~1.9us read + ~3us TX) = **~37us** |

With seconds of lead time, even 37us for 7 events is negligible.

## End-to-End Latency Estimate (per event)

| Stage | Latency (typical) | Latency (P100) |
|-------|--------------------|----------------|
| WREN R5F firmware (parse + ring write) | ~5-10us | ~10us |
| Sidecar detection (poll + 2x64-bit parse) | **~2.6us** | **~4.2us** |
| packet_mmap TX (sendto + kernel + NIC DMA) | ~3-5us | ~5us |
| Wire transit (1GbE direct cable) | ~1-2us | ~2us |
| packet_mmap RX (NIC + NAPI + ring) | ~4-7us | ~7us |
| **Total** | **~16-27us** | **~28us** |

For normal events this latency is irrelevant (seconds of lead time available).
For zero-lead emergency events this is the worst-case forwarding delay.

## CPU Core Selection (Critical)

The app and NIC IRQ **must** be on the same core. When a packet arrives, the IRQ
handler writes to the mmap ring and our spin loop reads it — same-core means L1
cache hit (~1ns). Cross-core means L3 roundtrip (~30-40ns per packet) and
occasional 10ms+ spikes from inter-processor interrupts.

**How to find which core the NIC IRQ is on:**
```bash
# Find the IRQ number(s) for the interface
grep eno2 /proc/interrupts    # e.g. IRQ 142: eno2-TxRx-0

# Check which core it's assigned to
cat /proc/irq/142/smp_affinity_list   # e.g. "4"
```

Choose that core for `taskset -c <core>`. If you need a different core, pin
the IRQ to match: `echo <core> > /proc/irq/142/smp_affinity_list`.

`NicTuner` handles this automatically — it finds the NIC IRQs via
`/proc/interrupts`, pins them to `kCpuCore`, and moves all other IRQs off.

## Optimizations
- TX side: `PACKET_QDISC_BYPASS` setsockopt (bypass kernel traffic control)
- RX side: `ethtool -C eno2 rx-usecs 0` (was 3µs — eliminated 6µs RTT penalty)
- NIC offloads: `ethtool -K eno2 gro off gso off tso off` (remove batching overhead)
- Scheduling: `SCHED_FIFO:49` app, `SCHED_FIFO:50` ksoftirqd/4 (NAPI can preempt to deliver)
- RT throttling: `sched_rt_runtime_us = -1` (prevents 50ms forced sleep every 1s)
- CPU isolation: core 4 dedicated — irqbalance stopped, userspace tasks moved off, NIC IRQ pinned
- Memory: `mlockall(MCL_CURRENT|MCL_FUTURE)`, `MAP_LOCKED` ring
- Note: `SO_BUSY_POLL` has no effect — we spin on `tp_status` in mmap'd memory, never call `poll()`/`recv()`

## Safety Notes
- NEVER write to IACK (0x34) -- that's the kernel driver's job
- NEVER write to async_host_off -- the kernel driver manages consumption
- ISR/ISR_RAW are read-only -- safe to read for diagnostics
- The sidecar is purely observational; kernel driver operates normally alongside it

## Timing Registers for Benchmarking
```
current_tai_sec  = read32(BAR1 + 0x14)  // TM_TAI_LO
current_cycles   = read32(BAR1 + 0x1C)  // TM_CYCLES (62.5MHz)
current_ns       = current_cycles * 16  // 16ns per tick
detection_lead   = event.ts - current_time  // how early we caught it
```
No hardware reception timestamp exists in the ring buffer. For benchmarking, use `clock_gettime(CLOCK_TAI)` on host or WREN's own timing registers.

## Benchmark: FESA/EDGE Baseline (to beat)

Existing approach measured in test lab (both FECs in adjacent racks):
1. WREN-installed FEC receives timing event
2. FESA publishes via RDA/CMW subscription (TCP/IP, CERN technical network)
3. Remote FEC (no timing card) receives event via subscription
4. FESA RT action writes pulse (1→0) to output patch panel using EDGE
5. LEMO cable carries pulse back to WREN-installed FEC
6. WREN hardware timestamps the input signal arrival

**Result: median 1.28ms, P100 3+ms**

Our target with sidecar + packet_mmap + ABTEdge: **~16-28us** (45-100x faster).
Same LEMO loopback setup can be used for apples-to-apples comparison.

## Measured: LTIM Pulse Jitter & PCIe Detection Latency (Feb 2026)

Hardware: cfc-865-mkdev30 (x86, WREN PCIe BAR1), SCHED_FIFO:99, mlockall.
Test: catch PIX.AMCLO-CT (ev_id=142), predict LTIM comp_idx=1 fire time (due + 50ms),
match against CMD_ASYNC_PULSE, measure jitter and 2x64-bit PCIe read time.

| Metric | Value |
|--------|-------|
| Samples | 18 matched pulses (30s run, 21 CTIMs) |
| LTIM timestamp jitter | **+136ns constant** (deterministic firmware pipeline offset) |
| PCIe parse (2x 64-bit reads) min | **1,908 ns** |
| PCIe parse typical | **~1,930 ns** |
| PCIe parse P100 (SMI hit) | **3,525 ns** |
| PCIe parse avg | **2,104 ns** |

The +136ns "jitter" is a fixed hardware offset (8.5 cycles @ 62.5MHz), not random —
true jitter is effectively **0 ns**. The bimodal PCIe distribution (~1.9us / ~3.5us)
is caused by SMI (System Management Interrupts) which cannot be avoided in software.

---

## Clock Synchronization Analysis

### The Problem
The WREN board's clock is synchronized via White Rabbit to sub-nanosecond precision.
Slave FECs without timing hardware rely on NTP, which introduces clock uncertainty.
If the slave fires events based on its own clock, clock sync error = execution jitter.

### Measured Clock Sync on Test Hardware

**cfc-865-mkdev16 (x86, i5-8500, Siemens FEC):**

| Method | Accuracy | Status |
|--------|----------|--------|
| NTP software timestamps (current) | ~18µs RMS | Running now |
| NTP + HW timestamps (`hwtimestamp *`) | ~1-3µs | One line uncomment in chrony.conf |
| Self-contained PTP (our two machines) | <1µs relative | We can do this ourselves (sudo) |
| CERN PTP grandmaster | ~100-500ns | Not available on our VLAN, would need IT |

- NICs: Intel I219-LM (eno1) + 2x Intel I210 (eno2, eno3) — all support PTP HW timestamping
- `hwtimestamp *` is **commented out** in `/etc/chrony.conf` — free 10x improvement available
- `linuxptp` installed, but no PTP grandmaster found on either network segment
- We have full sudo access: can edit configs, restart services, install packages, run ptp4l
- PTP devices: `/dev/ptp0` (I219), `/dev/ptp1` (I210/eno2), `/dev/ptp2` (I210/eno3)

**cfd-865-mkdev50 (ARM64, Zynq UltraScale+ MPSoC):**

| Method | Accuracy | Status |
|--------|----------|--------|
| NTP software timestamps (current) | ~15µs RMS | Running now |
| NTP + HW timestamps (`hwtimestamp *`) | ~1-5µs | One line uncomment in chrony.conf |

- SoC: Xilinx Zynq UltraScale+ (quad Cortex-A53), device tree model `diot_v4`
- Single NIC: `end0` (Cadence GEM MAC, `macb` driver, Microchip LAN8841 PHY, 1Gbps)
- **This is the NFS boot interface** — cannot risk aggressive tuning or packet_mmap
- GEM MAC has full PTP HW timestamping (`gem-ptp-timer`, supports one-step sync)
- `hwtimestamp *` also commented out in chrony.conf
- Kernel: 6.6.40-xlnxLTS20242-fecos02

### Key Insight: HW Timestamp NTP vs PTP Grandmaster

With `hwtimestamp *` enabled, chrony still uses **NTP protocol** (same ~260s polling interval,
same statistical correction model), but timestamps are taken at the NIC PHY layer instead of
in the kernel. This removes kernel scheduling jitter from measurements.

A PTP grandmaster would additionally provide:
- Dedicated IEEE 1588 protocol (designed for sub-µs, not bolted onto NTP)
- Much faster polling (~1s vs ~260s), so less drift accumulates between corrections
- Continuous hardware clock discipline via `phc2sys`
- ~100-500ns accuracy vs ~1-3µs

**For Option C (hybrid) this difference is irrelevant** — the slave fires on trigger packets
from the master, never consulting its own clock for firing decisions. Clock sync only matters
for logging timestamps and for Option A (schedule-based local firing).

---

## WREN Async Ring: Two-Phase Event Delivery

### Critical Discovery: CMD_ASYNC_EVENT vs CMD_ASYNC_PULSE

The WREN async ring buffer delivers events at **two distinct moments**:

| Message | When Written | What It Contains | Timing |
|---------|-------------|------------------|--------|
| `CMD_ASYNC_EVENT` (0x02) | Event **arrives** from WR network | Event ID, context ID, future due-time, parameters | **Seconds before** due-time |
| `CMD_ASYNC_PULSE` (0x04) | Event actually **fires** on FPGA | Comparator index, exact TAI timestamp (hardware-captured) | **At due-time** (~1-5µs firmware latency) |

**Flow in firmware** (from `wrenrx-cmd.c` and `wrenrx-core.c`):

1. **T-3.6s**: Event packet arrives from WR network → firmware writes `CMD_ASYNC_EVENT`
   to ring with event ID + future due-time
2. **T-100ms**: Software comparator promoted to hardware comparator (`setup_pulser()`)
   → firmware writes `CMD_ASYNC_CONFIG` to ring (links comparator index to action)
3. **T=0**: FPGA comparator matches (`current_time >= due_time`) → hardware interrupt →
   `pulses_handler()` writes `CMD_ASYNC_PULSE` to ring with exact fire timestamp

The `CMD_ASYNC_PULSE` timestamp comes from hardware registers at the moment of fire:
```c
// wrenrx-cmd.c:1808-1809
off = async_write32(off, tai);   // TAI seconds from regs->tm_tai_lo
off = async_write32(off, ts);    // nanoseconds (firmware comment says "cycle counter" but value is raw ns)
```

### What This Means for the Sidecar

The master's busy-poll loop sees **both** messages. This enables three architecture options:

**Option A — Advance + Local Clock**: Forward `CMD_ASYNC_EVENT`, slave fires at due-time
using its own clock. Requires good clock sync (PTP). Jitter = slave clock accuracy.

**Option B — Fire-and-Forward**: Wait for `CMD_ASYNC_PULSE`, send "FIRE NOW" to slave.
No clock sync needed. Jitter = forwarding latency (~20-30µs deterministic).

**Option C — Hybrid (chosen)**: Forward `CMD_ASYNC_EVENT` in advance (slave prepares),
then forward `CMD_ASYNC_PULSE` at fire time (slave executes). Best of both worlds.

---

## Architecture: Hybrid Approach (Option C)

### Why Hybrid Beats Pure Fire-and-Forward

`CMD_ASYNC_PULSE` only contains a comparator index + timestamp — no event ID, no payload,
no application context. Without advance notice, the master must reconstruct full event info
at fire time (lookup + build fat packet on the critical path).

With hybrid, the advance phase does all heavy lifting **seconds before**:

| Work | Option B (at fire time) | Option C (advance phase) |
|------|------------------------|--------------------------|
| Event lookup | ~50-100ns on critical path | Done seconds early |
| Packet build | ~100ns (fat packet w/ full context) | ~30ns (tiny: comp_idx + timestamp) |
| Wire time | ~1µs (large frame) | ~0.7µs (minimal frame) |
| Slave parse + handler lookup | ~200-500ns | ~10ns (pre-armed array index) |
| Cache state | Cold (handler code may cache-miss) | Hot (advance packet pre-warmed everything) |
| **Total fire-time savings** | | **~400-900ns** |

### Pre-Arming Mechanism

The advance phase lets the slave build a **direct-indexed pre-armed array**:

```cpp
struct ArmedEvent {
    void (*handler)(uint64_t tai_sec, uint32_t cycles, void* ctx);
    void* ctx;          // pre-allocated callback context
    uint32_t event_id;  // for logging
    bool armed;         // set by advance packet, cleared after fire
};

ArmedEvent armed[MAX_COMPARATORS];  // direct-indexed by comp_idx, no hash lookup
```

When fire packet arrives: `armed[comp_idx].handler(tai, cycles, ctx)` — one cache-line
read + indirect call. The handler code and context are already in L1/L2 from the advance
packet's processing.

### Compile-Time Architecture Switch

```cpp
#if defined(SIDECAR_MODE_HYBRID)    // Default: advance + fire (Option C)
    // Slave pre-arms on CMD_ASYNC_EVENT, fires on CMD_ASYNC_PULSE
#elif defined(SIDECAR_MODE_ADVANCE) // Option A: advance + local clock scheduling
    // Slave schedules on CMD_ASYNC_EVENT, fires via two-phase CLOCK_TAI timer
#endif
```

Option A is useful when PTP provides sub-µs sync and autonomous slave timing is desired.
Option C is the default: no clock dependency, lowest fire-time latency.

---

## Master-Slave Architecture: Implementation Plan

### Master (Sender) — Single Thread

One thread on core 4, SCHED_FIFO:49. Busy-poll and forward loop:

```
mmap PCIe BAR1 → busy-poll async_board_off → read new capsule(s) →
    CMD_ASYNC_EVENT? → wrap + send ADVANCE packet (full event context + comp_idx mapping)
    CMD_ASYNC_PULSE? → wrap + send FIRE packet (comp_idx + timestamp only)
    other?           → forward verbatim (diagnostics, config messages)
```

**Wire format** (custom Ethernet frame between master and slave):
```
Bytes 0-5:   Dst MAC
Bytes 6-11:  Src MAC
Bytes 12-13: EtherType (0x88B5)
Byte  14:    Version (0x01)
Byte  15:    Flags: bit 0 = ADVANCE, bit 1 = FIRE
Byte  16:    Capsule count
Bytes 17+:   Raw capsule words (copied verbatim from WREN async ring)
```

ADVANCE packets carry full capsule data (event ID, due-time, parameters).
FIRE packets carry only comp_idx + timestamp (~12 bytes payload) — minimal critical-path work.
Multiple capsules batched when they arrive in the same poll cycle.

**Estimated latency (FIRE packet)**: ~4-6µs from firmware ring write to wire (less than
ADVANCE because smaller packet, less data to read from PCIe).

### Slave (Receiver) — Three Threads

#### Option C (Hybrid) — Default Mode

```
Core 4 (FIFO:49)           Core 5 (FIFO:48)            Core 0 (OTHER)
┌──────────────┐           ┌───────────────────┐        ┌───────────┐
│  RX Thread   │           │ Dispatch Thread   │        │  Control  │
│ "The Catcher"│   SPSC    │ "The Executor"    │        │           │
│              ├──────────→│                   │        │ watchdog  │
│ packet_mmap  │  queue    │ ADVANCE → pre-arm │        │ stats     │
│ tryReceive() │ (1024)    │ FIRE → execute    │        │ signals   │
│ parse frame  │           │ LTIM → immediate  │        │           │
│ push events  │           │                   │        │           │
└──────────────┘           └───────────────────┘        └───────────┘
```

#### Option A (Advance) — With PTP Clock

```
Core 4 (FIFO:49)           Core 5 (FIFO:48)            Core 0 (OTHER)
┌──────────────┐           ┌───────────────────┐        ┌───────────┐
│  RX Thread   │           │ Scheduler Thread  │        │  Control  │
│ "The Catcher"│   SPSC    │ "The Timer"       │        │           │
│              ├──────────→│                   │        │ watchdog  │
│ packet_mmap  │  queue    │ drain queue       │        │ stats     │
│ tryReceive() │ (1024)    │ insert sorted list│        │ signals   │
│ parse frame  │           │ two-phase wait    │        │           │
│ push events  │           │ fire callback     │        │           │
└──────────────┘           └───────────────────┘        └───────────┘
```

#### RX Thread (Core 4) — "The Catcher" (shared by both modes)
Sole job: drain packets from the NIC as fast as possible, never block.
```
forever:
    frame = rx.tryReceive()           // ~0ns, pointer check on mmap'd memory
    if no frame: continue             // spin back immediately
    parse Ethernet payload → extract capsule(s) + flags
    for each capsule:
        build TimingEvent struct (64 bytes, one cache line)
        spsc.try_push(event)          // ~5ns, lock-free
    rx.release()
```

#### Dispatch Thread — Option C "The Executor" (Core 5)
Pre-arms on ADVANCE packets, fires on FIRE packets.
```
forever:
    while (spsc.try_pop(evt)):
        if evt.flags & FIRE:
            armed[evt.comp_idx].handler(evt.tai, evt.cycles, armed[evt.comp_idx].ctx)
            armed[evt.comp_idx].armed = false
        elif evt.flags & ADVANCE:
            armed[evt.comp_idx] = { lookup_handler(evt.event_id), alloc_ctx(), evt.event_id, true }
        elif evt.typ == CMD_ASYNC_PULSE:   // LTIM — already happened
            fire_callback(evt)              // bypass pre-arm, execute immediately
```

No timer, no sorted list, no clock dependency. Just array lookup and call.

#### Scheduler Thread — Option A "The Timer" (Core 5)
Drains SPSC queue, maintains a sorted event list, fires events at due-time.
```
forever:
    // Phase A: drain new events from queue
    while (spsc.try_pop(evt)):
        if evt is LTIM pulse (typ=0x04):
            fire_callback(evt)        // LTIM already happened — act immediately
        else:
            sorted_list.insert(evt)   // O(1) amortized — events arrive in order

    // Phase B: check if earliest event is due
    earliest = sorted_list.front()
    if none: continue to Phase A

    remaining_ns = earliest.due_time - tai_now()

    if remaining_ns > 1ms:
        clock_nanosleep(500us)        // yield CPU, re-check queue for new arrivals
    elif remaining_ns > 0:
        busy-spin on clock_gettime(CLOCK_TAI)   // ~20ns/call via vDSO
        fire when now >= due_time               // <1us jitter
    else:
        fire_callback(earliest)       // overdue — fire immediately
```

#### Why This Design Is Ultra-Low-Latency

**1. Zero allocation on hot path.**
Every structure pre-allocated at startup:
- SPSC queue: fixed 1024-entry array, power-of-two for bitmask wrap
- ArmedEvent array (Option C): fixed MAX_COMPARATORS entries, direct-indexed
- Sorted event list (Option A): pool of 4096 nodes, free-list managed
- TimingEvent: 64 bytes flat struct, no pointers, no heap
- No `new`, no `malloc`, no STL containers in the loop

**2. Option C: fire path is a single array index + function call.**
FIRE packet → `armed[comp_idx].handler(...)` — one cache-line read, one indirect call.
Handler code and context already in L1/L2 cache from the ADVANCE packet processing.

**3. Option A: two-phase timer — precision without waste.**
- Event >1ms away: `clock_nanosleep` yields CPU (no busy-wait for 3.6 seconds)
- Event <1ms away: busy-spin on `clock_gettime(CLOCK_TAI)` at ~50MHz via vDSO
- Result: <1us execution jitter without burning a core for the full advance period

**4. Events arrive already sorted by due-time (Option A).**
The WRT transmitter sends events chronologically. Insert into sorted list
scans backward from tail — usually 0 hops (append). Effectively O(1).
The sorted list is insurance against rare out-of-order edge cases.

**5. SPSC queue uses cached counters.**
Producer caches consumer position locally. Common-case push: single relaxed store (~5ns).
Cross-core atomic load only when queue appears full (never at 6 Hz into 1024 entries).

**6. LTIM pulses bypass everything.**
LTIM `CMD_ASYNC_PULSE` has a **past** timestamp (the pulse already fired on the master FPGA).
No scheduling, no pre-arming — pop from queue → fire callback immediately.
This is the unique value the sidecar provides that receiver-soft cannot.

**7. Core isolation + same-core NIC IRQ.**
RX thread and NIC IRQ on core 4: packet data in L1 cache when tryReceive() reads it.
Dispatch/Scheduler on core 5: clean core, no interrupts, `nohz_full` eliminates timer ticks.

#### Kernel Tuning (for lowest slave jitter)
```
isolcpus=4,5          # remove from kernel scheduler
nohz_full=4,5         # disable timer ticks on isolated cores
rcu_nocbs=4,5         # offload RCU callbacks
```
Plus existing NicTuner: IRQ pinning, coalescing off, RT throttle off, ksoftirqd priority.

#### Phase 1 Callback: Log/Notify
```cpp
void fire_callback(const TimingEvent& evt) {
    TaiTime now = tai_now();
    int64_t jitter_ns = elapsed_ns(evt.due_time, now);
    stats.record(jitter_ns);
    // "EVENT ev_id=156 due=08:00:11.200000000 fired=08:00:11.200000312 jitter=+312ns"
}
```
Track: events received, events fired, events late, min/max/avg jitter, histogram.

---

## Transport Layer Options

### Current: packet_mmap TPACKET_V2

Primary transport. Zero-copy-ish (mmap'd ring buffer shared with kernel).
Fully tested, ~16µs one-way median on 1GbE direct cable between FECs.
Works on both x86 (dedicated NIC) and ARM64 (shared NFS interface, without NicTuner).

### Future: AF_XDP Generic (Copy) Mode

FECOS team has agreed to enable `CONFIG_BPF_SYSCALL` + `CONFIG_XDP_SOCKETS` in kernel.
Will be available as a compile-time alternative to packet_mmap for benchmarking.

**Current kernel status:**

| Machine | Kernel | BPF_SYSCALL | XDP_SOCKETS | XDP Driver Support |
|---------|--------|-------------|-------------|-------------------|
| mkdev16 (x86) | 5.10-RT | **Not set** | Absent | igb: no zero-copy, copy mode only |
| mkdev50 (ARM64) | 6.6-xlnx | Set | **Not set** | macb: **no XDP at all** (never upstreamed) |

**After FECOS kernel rebuild:**

| Machine | AF_XDP Copy Mode | AF_XDP Zero-Copy |
|---------|-----------------|------------------|
| mkdev16 (x86, igb) | Yes | **No** — igb lacks `ndo_xsk_wakeup` |
| mkdev50 (ARM64, macb) | Yes (generic, no driver changes) | **No** — macb has no XDP support |

AF_XDP generic mode expected improvement over packet_mmap: **~1-3µs** (fewer skb clones,
cleaner ring semantics, built-in `SO_PREFER_BUSY_POLL`). Not a paradigm shift — the real
AF_XDP wins require zero-copy mode which needs driver support (Intel ice/i40e/mlx5 only).

Zero-copy AF_XDP on the I210 would require igb driver changes. On the Zynq GEM, it would
require writing ~500-1500 lines of XDP support for the macb driver. Neither is planned.

### ARM64 Considerations

The Zynq UltraScale+ SoC has only one NIC (`end0`, Cadence GEM, NFS boot interface).
Cannot use aggressive NicTuner settings without risking NFS boot path.

| Approach | Latency | NFS Risk |
|----------|---------|----------|
| packet_mmap RX (no NicTuner) | ~50-100µs | Safe — BPF filter only captures our EtherType |
| Regular UDP socket | ~80-150µs | Zero risk |
| packet_mmap RX (gentle tuning) | ~30-60µs | Low — reduced coalescing, not disabled |
| AF_XDP generic (future) | ~40-80µs | Safe — XDP prog selectively redirects |

For ARM64, transport is a compile-time choice. Same protocol, same event model,
gentler socket backend. The x86 FEC with dedicated NICs gets full packet_mmap + NicTuner.

---

## New Source Files

| File | Purpose | Lines (est) |
|------|---------|-------------|
| `src/WrenPeek.hpp/cpp` | PCIe BAR1 mmap + read32 (RAII) | ~80 |
| `src/WrenRingPoller.hpp/cpp` | Async ring poller + capsule extraction | ~120 |
| `src/SidecarFrame.hpp` | Wire format builder/parser (ADVANCE + FIRE flags) | ~70 |
| `src/SpscQueue.hpp` | Lock-free SPSC queue (header-only) | ~80 |
| `src/TimingEvent.hpp` | 64-byte event struct (header-only) | ~30 |
| `src/ArmedEvents.hpp` | Pre-armed comparator array (Option C) | ~60 |
| `src/EventScheduler.hpp/cpp` | Sorted list + two-phase timer (Option A) | ~150 |
| `src/TaiClock.hpp/cpp` | CLOCK_TAI utils + vDSO busy-spin | ~60 |
| `src/SidecarStats.hpp/cpp` | Jitter tracking + histogram | ~80 |
| `app/sidecar_master.cpp` | Master main (poll + classify + forward) | ~140 |
| `app/sidecar_slave.cpp` | Slave main (3 threads, mode switch) | ~220 |
| **Total** | | **~1090** |

### Implementation Order
1. WrenPeek + WrenRingPoller (PCIe access, validate on real WREN hardware)
2. SidecarFrame + TimingEvent (wire format with ADVANCE/FIRE flags, capsule parser)
3. sidecar_master (poll + classify CMD_ASYNC_EVENT vs CMD_ASYNC_PULSE + forward)
4. SpscQueue (lock-free queue, unit test throughput)
5. TaiClock (TAI utilities, test vDSO latency)
6. ArmedEvents (Option C pre-arm array, unit test with synthetic events)
7. EventScheduler (Option A sorted list + timer, unit test)
8. SidecarStats (jitter tracking)
9. sidecar_slave (integrate everything, compile-time mode switch)
10. End-to-end test (master on WREN FEC ↔ slave on spare FEC, measure jitter)

---

## Reference Source Files
- Ring buffer struct: `wren-gw/sw/hw-include/mb_map.h`
- Capsule format: `wren-gw/sw/api/include/wren/wren-packet.h`
- Async message types: `wren-gw/sw/hw-include/wren-mb-defs.h`
- Host register map: `wren-gw/sw/hw-include/host_map.h`
- Kernel driver: `wren-gw/sw/drivers/wren-core.c`
- Userspace API: `wren-gw/sw/api/include/wren/wrenrx.h`
- Event ID catalog: `timing-wrt/timing-domain-cern/src/timing-domain-cern/*.cpp`

## packet_mmap Forwarding Layer (ABTRDA3)

### Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| TPACKET version | **V2** for both RX and TX | Deterministic frame-by-frame processing. V3 block batching introduced jitter/timeouts for single events. |
| Ring sizing | `frame_size = 4096` | Page-aligned frames. One frame per slot. No block aggregation. |
| TX send mechanism | `send(fd, NULL, 0, MSG_DONTWAIT)` | Immediate transmission of marked frame. |
| Qdisc bypass | `PACKET_QDISC_BYPASS` on TX | Skips kernel traffic control layer, saves ~1-2us per packet. |
| HW timestamps | RX: `SOF_TIMESTAMPING_RAW_HARDWARE` | Zero cost in `tpacket2_hdr`. |
| Safety | 30s Watchdog Thread | Prevents system lockup when running `SCHED_FIFO` at 100% CPU on isolated cores. |

### TPACKET_V2 Ring Setup (Deterministic Low-Latency)

```cpp
struct tpacket_req req{};
req.tp_block_size = 4096;   // Page size
req.tp_frame_size = 4096;   // One frame per block/slot
req.tp_block_nr   = 64;     // Ring depth
req.tp_frame_nr   = 64;     // Total frames

setsockopt(fd, SOL_PACKET, PACKET_RX_RING, &req, sizeof(req)); // or TX_RING
```

### Measured Performance (Feb 2026)
Hardware: CERN FECs (cfc-865-mkdev30 ↔ cfc-865-mkdev16), 1GbE direct cable, igb driver
Kernel: 5.10.245-rt139-fecos03 (PREEMPT_RT)
500,000 packets, no kernel bypass (no XDP/DPDK)

| Metric | Value |
|--------|-------|
| Min RTT | 24 µs |
| Median RTT | 33 µs |
| P100 (Max) RTT | 77 µs |
| Avg RTT | 33 µs |
| **One-Way Latency (median)** | **~16 µs** |
| **One-Way Latency (P100)** | **<39 µs** |

### RX Busy-Poll Hot Loop (V2)

```cpp
while (running) {
    auto* hdr = (struct tpacket2_hdr*)next_frame_ptr;

    if ((hdr->tp_status & TP_STATUS_USER) == 0)
        continue;  // spin

    // Data ready at: (uint8_t*)hdr + hdr->tp_mac
    process_packet((uint8_t*)hdr + hdr->tp_mac, hdr->tp_snaplen);

    // Release frame
    hdr->tp_status = TP_STATUS_KERNEL;
    advance_ring_pointer();
}
```

### TX Hot Path (V2)

```cpp
bool send_packet(const uint8_t* payload, size_t len) {
    auto* hdr = (struct tpacket2_hdr*)next_frame_ptr;

    if (hdr->tp_status != TP_STATUS_AVAILABLE)
        return false;

    // Copy payload
    memcpy((uint8_t*)hdr + TPACKET_ALIGN(sizeof(tpacket2_hdr)), payload, len);
    hdr->tp_len = len;
    hdr->tp_snaplen = len;

    // Mark and flush
    hdr->tp_status = TP_STATUS_SEND_REQUEST;
    send(fd, NULL, 0, MSG_DONTWAIT);

    advance_ring_pointer();
    return true;
}
```

### Runtime Tuning Checklist (both machines, reverts on reboot)

```bash
# 1. NIC tuning (eno2)
ethtool -C eno2 rx-usecs 0 tx-usecs 0       # disable interrupt coalescing
ethtool -K eno2 gro off gso off tso off      # disable NIC offloads

# 2. CPU isolation for core 4
systemctl stop irqbalance
# Move all non-NIC IRQs off core 4
for irq in $(ls /proc/irq/ | grep -E '^[0-9]+$' | grep -v '^142$'); do
    echo 0-3,5 > /proc/irq/$irq/smp_affinity_list 2>/dev/null
done
# Move userspace tasks off core 4
for pid in $(ps -eo pid,psr | awk '$2==4 {print $1}'); do
    taskset -apc 0-3,5 $pid 2>/dev/null
done

# 3. RT scheduling: ksoftirqd/4 must be above app priority
chrt -f -p 50 $(pgrep -x ksoftirqd/4)       # NAPI delivery thread
echo -1 > /proc/sys/kernel/sched_rt_runtime_us  # disable RT throttling

# 4. Run (app sets SCHED_FIFO:49 internally, watchdog auto-stops after 30s)
sudo taskset -c 4 ./abtrda3_test --server
sudo taskset -c 4 ./abtrda3_test --client --count 200000
```
