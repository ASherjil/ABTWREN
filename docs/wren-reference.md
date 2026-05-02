# WREN (White Rabbit Event Node) — Technical Reference

## What It Is

WREN is a timing distribution board for CERN's accelerator complex. It replaces multiple
legacy timing networks (GMT, BST, RF distribution, SMP) with a single White Rabbit Ethernet
network providing sub-nanosecond synchronization.

**SoC**: Xilinx Ultrascale+ XCZU4CG (Zynq MPSoC)
- **FPGA gateware**: Time-critical — trigger generation, RF frame parsing (1ns precision)
- **ARM Cortex-R5**: Firmware (bare-metal) — event parsing, configuration, mailbox IPC
- **ARM Cortex-A53**: Reserved for future use

**Form factors**: PXIe (primary), VME64x, PCIe — target ~1000 CHF/board
**Connection**: Single-mode single-fiber to a WR Switch (1 Gbps Ethernet + PTP sync)
**Connectors**: 6, 14, or 38 I/Os depending on form factor, some bidirectional

As of summer 2025, pilot installations deployed on a few dozen FECs.
Reference: ICALEPCS'25 paper WEBR003 (Moscardi et al.)

---

## The Timing Data Model

### Domains
Each accelerator has its own timing domain with named fields, contexts, and events:
- **P** = CPS, **B** = PSB, **E** = LEIR, **S** = SPS, **H** = LHC, **D** = ADE(PS), **A** = LNA(LINAC), **C** = SCT

Domains are isolated: fields, contexts, and events are constrained to the same domain.
Equipment typically listens to a single domain (the accelerator it's installed in).

### Fields
Named typed variables belonging to a domain. Types: INT32, INT64, FLOAT, BOOL, ENUM.
```
BEAM_ID:          int32   (e.g. 24526)
PARTICLE:         enum    (PROTON, PB82, ...)
BASIC_PERIOD_NB:  int32   (e.g. 2)
```
Most fields go in contexts (stable per cycle). Only fields that change within a cycle
are transmitted with individual events.

### Contexts
Snapshots of field values valid for a time interval. Identified by an 8-bit rolling counter.
Only one context is "in force" at any given moment per domain.
Up to 4 cached per WREN.
```
Context ID: 0x1 | Valid from: 08:00:10.000 | Valid to: 08:00:12.400
Fields: {BEAM_ID=24526, PARTICLE=PROTON, BASIC_PERIOD_NB=2}
```

### Events
Named occurrences with a nanosecond-precision **due-time** (TAI timestamp).
Each event references a context (inherits all its field values) and may carry additional
event-specific field values.
```
Event: PX.SBP-CT | Due-time: 08:00:11.200 | Context: 0x1 | BASIC_PERIOD_NB: 2
Effective fields: BEAM_ID=24526, PARTICLE=PROTON (inherited from context)
```

Two types of events in cycling accelerators:
- **Scheduled events**: Predetermined in the supercycle, sent ~3 basic periods in advance
- **Asynchronous events**: Unpredictable (e.g. Beam Dump), sent with due-time in near future
  accounting only for network propagation time

---

## CTIM vs LTIM

### CTIM (Central TIMing)
Events **centrally generated and broadcast** over the WR network to all receivers.
These are the "global announcements" of accelerator cycle milestones.

Defined in `timing-domain-cern` (shared library on NFS, auto-generated from timing database):
```cpp
// CpsDomain.cpp
PX_FCY600_CT(151, "PX.FCY600-CT", "Forewarning Cycle -600ms", domain),
PX_WCY200_CT(159, "PX.WCY200-CT", "Warning start cycle -200ms", domain),
PX_SCY_CT(156,    "PX.SCY-CT",    "Start Cycle", domain),
PX_ECY_CT(148,    "PX.ECY-CT",    "End Cycle", domain),
PX_SBP_CT(155,    "PX.SBP-CT",    "Start Basic Period", domain),
```

**Naming convention**: `<Domain><Sub>.<Name>-CT`
- Domain prefix: P=CPS, B=PSB, S=SPS, E=LEIR, H=LHC
- Sub-prefix: I=Injection, E=Ejection, I2=2nd batch, X=general
- `-CT` suffix = Central Timing (i.e. it's a CTIM)
- Example: `PIX.F900-CT` = CPS Injection forewarning -900ms

### LTIM (Local TIMing)
**Locally generated actions** on a WREN receiver card. Highly configurable trains of pulses
accompanied by optional software interrupts. Two loading modes:

- **Event-bound**: Loaded upon reception of a matching CTIM event (with optional field conditions)
- **Immediate**: Loaded immediately upon configuration (no event trigger needed)

```
CTIM arrives (network) → matches LTIM condition → LTIM loads → start/delay → pulses fire
```

Once loaded, LTIMs are handled entirely by the FPGA gateware — guaranteeing timely
load, start, and stop operations independent of software.

| Feature | CTIM | LTIM |
|---------|------|------|
| Origin | Central, broadcast on network | Local, on receiver card |
| Trigger | Timing system generates it | Programmed, triggered by CTIM arrival or immediately |
| Delay | N/A | Configurable delay from CTIM |
| Hardware | N/A | Has a triggerer channel (1-32), produces pulses + IRQs |
| Managed by | Transmitter software | WrenReceiver API (not in base Receiver interface) |

---

## Wire Protocol: WRT Frames

Two frame types, both L2 Ethernet (max 1500 bytes payload):

### Packet Header (v1 — 3 words / 12 bytes)
```
word 0: [version:8 | domain_id:8 | xmit_id:8 | next_pkt_delay:8]
word 1: [frame_len:16 | seq_id:16]
word 2: [send_timestamp:24 (compressed) | tai_epoch:2 | reserved:6]
```
- version = 0x01 (active)
- seq_id: 0xFFFC = SYNC (resync), 0xFFF8 = INVALID
- send_timestamp: 22 bits @ 0.256us resolution

### Capsule Header (1 word / 4 bytes)
```
byte 0: type      (0x01=context, 0x02=event)
byte 1: padding
bytes 2-3: length (total capsule length in 32-bit words)
```

### Context Capsule (4+ words)
```
word 0: capsule header  {type=0x01, pad, len}
word 1: [context_id:16 | padding:16]      context_id 0-127 valid, 0xFF=none
word 2: valid_from.nsec  (nanoseconds, TAI)
word 3: valid_from.sec   (seconds, TAI)
word 4+: parameters (TLV-encoded)
```

### Event Capsule (4+ words)
```
word 0: capsule header  {type=0x02, pad, len}
word 1: [event_id:16 | context_id:16]     event_id 0-65535, context_id 0xFF=none
word 2: due_time.nsec   (nanoseconds, TAI)
word 3: due_time.sec    (seconds, TAI)
word 4+: parameters (TLV-encoded, optional)
```

### Parameter TLV Encoding (per parameter, 1+ words)
```
word 0: [length:10 | datatype:6 | param_id:16]
word 1+: data (length-1 words)

Datatypes: S32=1, U32=2, S64=3, U64=4, F32=5
```

### Event-type frames (max 1 kHz, no-loss)
```
[Ethernet/UDP Header] [Packet Header (3w)] [Capsules...] [CRC]
```
Payload contains context capsules followed by event capsules.
Context capsules must precede the event capsules that reference them.

### RF-type frames (max 50 kHz, no-loss)
```
[Streamer/RoE Header] [Tx Timestamp | Source ID | Sequence ID | FTW data] [CRC]
```
Carry Frequency Tuning Words for local RF clock regeneration (Bunch clock, Frev).

---

## How Events Are Sent: Legacy vs New

### Legacy GMT: Events fire at the moment they occur

GMT runs on RS-485 serial at 500 kbit/s. Eight 32-bit frames per millisecond.
Events are sent **at the instant** they happen. Equipment needing lead time relies on
a cascade of **forewarning/warning** events at fixed offsets:

```
CPS cycle example (legacy GMT, real-time firing):
  t-900ms:  PIX.F900-CT    fires -> "injection coming in 900ms"
  t-600ms:  PX.FCY600-CT   fires -> "cycle starting in 600ms"
  t-200ms:  PX.WCY200-CT   fires -> "cycle starting in 200ms"
  t-50ms:   PX.WCY50-CT    fires -> "cycle starting in 50ms"
  t=0:      PX.SCY-CT      fires -> "cycle starts NOW"
```

Legacy limitations: 500kbit/s bandwidth, 1.2km max link, 32 nodes/segment,
manual timing calibration needed per FEC, frames too small for contextual info.

### New WRT/WREN: Events sent seconds in advance with future due-times

In the new system, **all scheduled events** carry a **future TAI timestamp** (the due-time).
Events are sent out roughly **3 basic periods in advance** (~3.6 seconds for cycling
accelerators where 1 basic period = 1.2 seconds).

```
WRT/WREN (advance delivery):
  t-3.6s:  Event-type frame arrives containing:
           Context: {BEAM_ID=24526, PARTICLE=PROTON, ...}
           Event: PX.SCY-CT, due-time = t=0 (TAI)

  WREN FPGA loads triggerer config (<23us), then waits on WR-synchronized TAI clock...

  t=0 (nanosecond precision):  Triggerer fires hardware output
```

**Event Delivery Offset** (subscriber-side):
```
PX.SBP-CT example:
  Due time:                08:00:11.200
  Delivery offset:         -160 ms
  Delivery time to app:    08:00:11.040    (app gets notified 160ms early)
```
Range: -5s to +5s, microsecond precision. Only affects software delivery,
NOT trigger load time (use loadOffset in TriggerSettings for that).

**Asynchronous events** (e.g. Beam Dump) are sent with due-time in the near future,
accounting only for network propagation time (~500us max).

**Key consequences**:
1. No more forewarning cascades needed (one event + delivery offset replaces all)
2. Sub-nanosecond trigger precision (FPGA counts TAI clock edges)
3. Scheduled events arrive ~3.6s early (cycling accelerators)
4. Forewarning events still defined in domain-cern for backward compatibility

**LHC exception**: Timing is NOT based on supercycles. Operators initiate phase transitions
(injection, acceleration, collision) using **event tables** — pre-loaded sequences of events
with relative due-times and WAIT entries.

---

## The Trigger Generator (LTIM Hardware)

32 LTIM channels organized in **4 groups of 8**. Each channel can host one or more LTIMs.
This is the core action engine, implemented entirely in FPGA gateware.

### State Machine
```
unconfigured → idle → [event+condition] → loaded → [start] → delay → pulsing
                                                                  ↓
                                                            interrupts + pulses
```
Many LTIMs use simpler config: auto-start (skip loaded state), single pulse or interrupt.

### Configuration Parameters
1. **Loading condition**: Event-ID + up to 3 field predicates (&&, ||, operators: > < >= <= == & bitmask)
2. **Loading offset**: +/- delay from event timestamp
3. **Clock input**: PPS-aligned 1Hz, 1kHz, 1MHz, 10MHz, 40MHz, 1GHz, Fbunch, Frev, or external
4. **Trigger delay**: N clock cycles after start
5. **Start input** (opt): External pin, another LTIM, PPS, or AUTO (immediate)
6. **Start type/polarity** (opt): edge/level, active-high/low
7. **Stop input** (opt): External pin or another LTIM
8. **Stop type/polarity** (opt): edge/level, active-high/low
9. **Pulse width**: 32-bit, 1ns resolution, min 16ns (0 = level output)
10. **Pulse period**: 32-bit, in clock multiples, min 32ns
11. **Number of pulses**: 16-bit (0 = infinite)
12. **Output**: External pin and/or IRQ and/or another LTIM's input

### Group Output Logic
Each group's 8 LTIM outputs can be combined through **AND, NAND, OR, or NOR** gates
before reaching the physical output pin. Output polarity is configurable at the pin.

### Performance
- Processing latency (frame reception -> triggerer loaded): **< 23 us**
- Configuration latency (host -> triggerer): < 10 ms
- Max interrupt rate: ~1100/sec
- Max interrupt latency: 100 us

### Examples
**50MHz clock on 450GeV extraction:**
- Condition: `extraction AND Energy == 450GeV`
- Clock: WR TAI, Width: 20ns, Period: 40ns, Pulses: infinite

**5us pulse 100ns before flat-top:**
- Condition: `flat-top`, Offset: -100ns
- Width: 5000ns, Pulses: 1

**RF-synchronous pulse 3 revolutions after injection:**
- Condition: `injection AND Particle == protons`
- Clock: Frev, Delay: 3, Start: External (edge, active-high)
- Width: 1 Frev period, Pulses: 1

---

## Software Stack

```
┌──────────────────────────────────────────────────────────────┐
│                  FESA / EPICS / CLI (wrentest)                │
├──────────────┬───────────────┬────────────┬──────────────────┤
│ receiver-wren│ receiver-ctr  │receiver-soft│ receiver-mock   │  transmitter-wren
│ (WREN HW)   │ (legacy GMT)  │(TCP/PTP)   │ (testing)       │  (TX impl)
├──────────────┴───────────────┴────────────┴──────────────────┤
│              receiver-cern (ReceiverProvider factory)         │
├──────────────────────────────────────────────────────────────┤
│    Receiver (abstract interface)                              │
│    └── ContextAwareReceiver (context caching base)           │
├──────────────────────────────────────────────────────────────┤
│    domain (Domain, Field, Context, Event, FieldDescriptor)   │
│    domain-cern (CPS, PSB, SPS, LHC, LEIR, ADE, LNA, SCT)   │
├──────────────────────────────────────────────────────────────┤
│    libwrenrx.so / libwrentx.a  (low-level C API)            │
├──────────────────────────────────────────────────────────────┤
│    Linux kernel driver (wren-core.ko)                        │
│    64 clients, 256 buffers, mailbox IPC                      │
├──────────────────────────────────────────────────────────────┤
│    ARM R5 Firmware (bare-metal)                              │
├──────────────────────────────────────────────────────────────┤
│    FPGA Gateware (VHDL)  — triggerers, NIC, RF, NCO, clocks │
└──────────────────────────────────────────────────────────────┘
```

**Key design decisions**:
- LTIM management deliberately NOT in base Receiver interface (keeps it generic/reusable)
- receiver-ctr wraps legacy CTR library for transparent GMT-to-WRT migration
- domain-cern is a shared library on NFS — add events/fields without recompiling clients

### receiver-soft (relevant to sidecar project)
Official solution for platforms without WREN hardware (SoC boards, etc.).
Connects via **TCP to a Timing Distributor server**. Contexts and events transmitted
in advance. Accuracy limited by host system clock (improvable with PTP).
Expected to grow in importance as SoC platforms multiply in the accelerator complex.

**This is what the sidecar project competes with** — but via raw Ethernet (~30us)
instead of TCP through a server (~1.28ms via FESA/RDA path).

---

## Receiver C++ API

### Core Interface (Receiver)
```cpp
class Receiver {
    Time utc() const;                                  // Current UTC time
    void subscribe(const std::string& eventName);      // Subscribe to CTIM/LTIM
    void subscribe(const std::string& eventName, Duration offset);  // With delivery offset
    void unsubscribe(const std::string& eventName);
    void subscribeContext(const Domain& domain);
    void unsubscribeContext(const Domain& domain);
    Context getContext(const Domain& domain) const;     // Latest context
    Context getContext(const Domain& domain, const Time& time) const;
    Event waitEvent();                                  // BLOCKING — wait for next event
};
```

### Context Caching (ContextAwareReceiver)
Extends Receiver with boost::multi_index_container cache:
- Lookup by (domain, context_id) — hash index
- Lookup by (domain, validFrom timestamp) — ordered index
- Default retention: 300 seconds
- Auto-eviction of old contexts on new context arrival

### WrenReceiver Extensions (LTIM Management)
```cpp
class WrenReceiver : public ContextAwareReceiver {
    // LTIM creation
    std::shared_ptr<WrenEventBoundTriggerCondition>
        newEventBoundTriggerCondition(const EventDescriptor&);
    std::shared_ptr<WrenImmediateTriggerCondition>
        newImmediateTriggerCondition(const Domain&);

    std::vector<uint16_t> createTrigger(
        const std::string& name,
        uint8_t channel,    // 1-32
        const std::vector<std::tuple<TriggerSettings, std::shared_ptr<TriggerCondition>>>& configs
    );

    // LTIM management
    void updateTrigger(const std::string& name, uint16_t index, const TriggerSettings&);
    void deleteTrigger(const std::string& name);
    void forceTrigger(const std::string& name, uint16_t index);
    TriggerSettings getTriggerSettings(const std::string& name, uint16_t index);
    std::vector<TriggerInfo> getAllTriggers() const;
};
```

### TriggerSettings
```cpp
struct TriggerSettings {
    bool enabled, interruptEnabled, outputEnabled, repeat;
    TriggerStart start;       // Pin, LTIM chain, PPS, AUTO
    TriggerStop stop;         // Pin, LTIM chain, NONE
    TriggerClock clock;       // Pin, LTIM chain, 1kHz/1MHz/10MHz/40MHz/1GHz/FBUNCH/FREV
    uint64_t initialDelay;    // Clock cycles
    Duration pulseWidth;      // Nanoseconds
    uint32_t pulsePeriod;     // Clock cycles (0=level)
    uint32_t nPulses;         // 0=infinite
    Duration loadOffset;      // Offset on event due-time (negative=early)
};
```

### TriggerCondition
```cpp
class TriggerCondition {
    virtual bool isEventBound() const = 0;
    virtual const EventDescriptor& getEventDescriptor() const;  // throws if not event-bound
    virtual void setExpression(std::unique_ptr<condition::Node> expr) = 0;
};
// Expression: up to 3 field predicates combined with &&/||
// Operators: EQ, NE, LT, LE, GT, GE, BITMASK
```

### Usage Example (from demo)
```cpp
auto receiver = std::make_shared<timing::receiver::WrenReceiver>(lun);
receiver->subscribe("PX.SBP-CT");
receiver->subscribeContext(Domain::of("CPS"));

while (true) {
    Event event = receiver->waitEvent();  // blocking
    auto beam_id = event.getFieldValue<int32_t>("BEAM_ID");
    auto trigger_time = event.getTriggerTime();
    auto& context = event.getContext();
}
```

---

## Kernel Driver: Subscription & Event Delivery Path

### Subscription Setup (IOCTL path)

**1. Add RX Source** (`WREN_IOC_RX_ADD_SOURCE`):
- Allocates source slot, stores protocol info + holdoff
- Sends `CMD_RX_SET_SOURCE` to firmware via mailbox
- Returns source index to userspace

**2. Add Config = Condition + Action** (`WREN_IOC_RX_ADD_CONFIG`):
- Allocates/reuses condition (conditions shared across actions if identical)
- Sends `CMD_RX_SET_COND` to firmware (comparator logic)
- Allocates action slot, links to condition
- Sends `CMD_RX_SET_ACTION` to firmware (pulser config)
- Returns action index

**3. Subscribe to Events** (`WREN_IOC_RX_SUBSCRIBE_EVENT`):
- Sets bit in per-client bitmap: `client->sources[src_idx].evsubs[event_id]`
- Increments global counter: `wren->sources[src_idx].evsubs[event_id]`
- **On first global subscription**: sends `CMD_RX_SUBSCRIBE` to firmware
- Firmware only writes events to async ring if global subscription count > 0

**4. Subscribe to Contexts** (`WREN_IOC_RX_SUBSCRIBE_CONTEXT`):
- Sets bit in device bitmap: `wren->client_sources[src_idx]`
- Immediately sends cached contexts to client ring

**5. Subscribe to Configs/Pulses** (`WREN_IOC_RX_SUBSCRIBE_CONFIG`):
- Sets bit in device bitmap: `wren->client_configs[act_idx]`
- Implicitly subscribes to parent source's contexts

### Event Flow: Firmware -> Driver -> Userspace

```
WR Network -> FPGA -> Firmware R5
  |
  | Firmware checks: evsubs[event_id] > 0?
  | If NO: discard (no subscribers)
  | If YES:
  |   Write capsule to async_data[] ring buffer (2048 words)
  |   Update async_board_off (firmware write pointer)
  |   Assert ASYNC interrupt
  |
  v
Hardware IRQ -> wren_irq_handler()
  |   Read & acknowledge ISR
  |   Return IRQ_WAKE_THREAD (deferred to threaded handler)
  |
  v
Threaded handler -> wren_irq_thread()
  |   Read async_board_off (firmware) vs async_host_off (driver)
  |   For each new capsule between pointers:
  |
  |   CMD_ASYNC_CONTEXT (0x01):
  |     Allocate buffer(s) from 256-buffer pool (lock-free CAS)
  |     Copy capsule data from async_data[] to buffer
  |     For each client in client_sources[src_idx] bitmap:
  |       Put msg in client->ring[ring_tail] (128 entries per client)
  |       Increment buffer refcnt
  |       wake_up(&client->wqh)
  |     Store in cached_contexts[ctxt_id % 4]
  |
  |   CMD_ASYNC_EVENT (0x02):
  |     Allocate buffer, copy data
  |     For each client with context subscription to this source:
  |       Check: test_bit(event_id, client->sources[src_idx].evsubs)
  |       If subscribed: put msg in client ring, wake_up
  |     Store as last_evt[src_idx] for pulse association
  |
  |   CMD_ASYNC_PULSE (0x04):
  |     Lookup config_id from comparator table
  |     For each client in client_configs[config_id] bitmap:
  |       Put msg (with timestamp + config_id) in client ring
  |       Attach associated event buffer if available
  |       wake_up
  |
  |   CMD_ASYNC_CONFIG (0x03):
  |     Configuration successfully applied notification
  |
  |   CMD_ASYNC_CONT (0x07):
  |     Continuation for oversized capsules spanning multiple entries
  |
  |   Update async_host_off (driver read pointer)
  |
  v
Userspace -> read(client_fd) -> wren_usr_read()
  |   wait_event_interruptible(&client->wqh, ring not empty)
  |   Read msg from ring[ring_head]
  |   Extract cmd + buf_idx from atomic buf_cmd field
  |   Copy buffer data to userspace
  |   Advance ring_head
  |   wren_put_buffer() -> decrement refcnt -> return to pool if 0
```

### Buffer Management (256-buffer pool)
- Lock-free allocation via atomic CAS on free-list head
- Reference counting: +1 per client receiving, +1 for context cache
- Auto-release when all consumers have read
- Each buffer: 128 x 32-bit words max (512 bytes)

### 64-Client Model
- Each `open()` on the WREN device creates a client (max 64)
- Per-client: 128-entry ring buffer, per-source subscription bitmaps (4096 event IDs)
- Device-level bitmaps track which clients subscribe to which sources/configs
- Lock-free ring: head advanced by reader, tail by IRQ handler

### Key Filtering Points
| Where | What's filtered |
|-------|----------------|
| Firmware | Events with zero subscribers globally -> discarded |
| Firmware | Source holdoff (inter-packet minimum interval) |
| Driver (IRQ thread) | Events delivered only to clients subscribed to that event_id |
| Driver (IRQ thread) | Pulses delivered only to clients subscribed to that config_id |
| Driver (IRQ thread) | Contexts delivered to all source-subscribed clients (no filtering) |

---

## Full Event Delivery Path: FPGA Fire → Userspace Application

This traces the complete latency chain for an LTIM pulse from the FPGA gateware
through the R5 firmware, kernel driver, C library, and C++ framework to the
application. At each stage the mechanism and typical latency are documented.

### Stage 0: FPGA gateware — pulser fires

The pulser comparator in the FPGA gateware matches the current TAI time against
the scheduled fire time. The output pin transitions at the exact nanosecond.
Simultaneously, a hardware interrupt is raised to the R5 firmware.

| Mechanism | Latency |
|-----------|---------|
| Hardware comparator match + pin drive | **0 ns** (reference — the nanosecond-precision event) |

### Stage 1: R5 firmware — capsule write + IRQ assertion

File: `sw/firmware/common/wrenrx-cmd.c`, function `pulses_handler()`

The R5 receives the pulser interrupt in its main loop. It reads the pulser FIFO,
writes a PULSE capsule (type 0x04) to the `async_data[]` ring buffer in BTCM,
advances `async_board_off`, and asserts the ASYNC interrupt bit in the host
register space (`HOST_MAP_INTC.ISR`), generating a PCIe MSI interrupt to the host.

| Mechanism | Latency |
|-----------|---------|
| R5 interrupt + capsule write + MSI assert | 1-5 µs |

### Stage 2: Kernel driver — IRQ handler + client ring buffer

File: `sw/drivers/wren-core.c`, functions `wren_irq_handler()` (line ~4468),
`wren_irq_thread()`, `wren_irq_pulse()` (line ~4112)

The host CPU receives the MSI. `wren_irq_handler()` reads the ISR, sees
`HOST_MAP_INTC_ISR_ASYNC`, and returns `IRQ_WAKE_THREAD`. The kernel schedules
the threaded IRQ handler, which:

1. Reads `async_board_off` vs `async_host_off` from BAR1 via `ioread32()`
2. For each new capsule between the pointers, reads the capsule data via `ioread32()`
3. Allocates a buffer from the 256-entry kernel buffer pool (lock-free CAS)
4. Copies the capsule into the buffer
5. Iterates over `client_configs[config_id]` bitmap to find subscribed clients
6. Writes `CMD_ASYNC_PULSE | (buf_idx << 8)` to each client's ring buffer
7. Calls `wren_update_client_msg()` → `atomic_set(&client->ring_tail)` + `wake_up(&client->wqh)`

| Mechanism | Latency |
|-----------|---------|
| IRQ dispatch + threaded handler + `wake_up()` | 5-15 µs |

### Stage 3: Kernel driver — userspace `read()` unblocks

File: `sw/drivers/wren-core.c`, function `wren_usr_read()` (line 119)

The userspace process was sleeping in `wait_event_interruptible_locked(&client->wqh, ...)`.
The `wake_up()` from Stage 2 makes it runnable. After the Linux scheduler
context-switches to it (~5-30 µs depending on load and RT priority), it:

1. Reads the message header from `client->ring[ring_head]`
2. Extracts `buf_idx` from the atomic `buf_cmd` field
3. Copies the capsule header, timestamp, config ID, and source index
   from the kernel buffer to userspace via `put_user()`
4. Advances `ring_head`
5. Decrements the buffer refcount; returns buffer to pool if zero

| Mechanism | Latency |
|-----------|---------|
| Context switch + `put_user()` copy | 5-30 µs |

### Stage 4: Userspace C library — message decode

File: `sw/api/wrenrx-drv-wren.c`, function `wren_drv_wait()` (line 1485)

`wren_drv_wait()` loops on `read(h->wren_fd, ...)`. When `read()` returns data,
it dispatches on `u.hdr.hdr.typ`:

- `CMD_ASYNC_PULSE` → `wren_drv_read_pulse()` decodes the raw words into a
  `struct wrenrx_msg` with `.kind = wrenrx_msg_pulse`, populating config ID,
  timestamp, and (for event-bound triggers) the load event's fields and context.
- `CMD_ASYNC_CONTEXT` → delivered immediately.
- `CMD_ASYNC_EVENT` → enters the software event scheduler: computes delivery time
  (due-time minus subscription offset), inserts into a sorted linked list, and
  sets a kernel timer via `WREN_IOC_RX_SET_TIMEOUT`. The event is returned to
  userspace only when the timer expires and `read()` returns `-ETIME`.

| Mechanism | Latency |
|-----------|---------|
| Capsule decode + message construction | 1-5 µs |

### Stage 5: C++ framework — `WrenReceiver` event construction

Files: `timing-receiver-wren/src/timing-receiver-wren/WrenRxHandle.cpp` (line 457),
`timing-receiver-wren/src/timing-receiver-wren/WrenReceiver.cpp` (line 237)

`WrenRxHandle::waitForMessage()` wraps the C `wrenrx_wait2()` call, converting
the `wrenrx_msg` into a `WrenMessage` with type `TRIGGER`, `EVENT`, or `CONTEXT`.

`WrenReceiver::waitContextOrEventNoRetry()` processes the message:
- For TRIGGER: matches config ID to the trigger name via the action map,
  extracts the load event ID and fields (for event-bound triggers), merges
  with cached context fields, and constructs a `timing::Event` object.
- For CONTEXT: decodes the TLV-encoded fields and stores in the context cache
  (boost::multi_index_container, up to 4 per domain).
- For EVENT: creates an `Event` from the descriptor + cached context.

The application calls `receiver->waitEvent()` which loops until an Event
(or Context, depending on subscription) is available.

| Mechanism | Latency |
|-----------|---------|
| WrenMessage wrap + Event construction + context merge | 5-10 µs |

### Total end-to-end latency (LTIM pulse → userspace application)

| Stage | Latency (typical) | Latency (worst-case) |
|-------|--------------------|----------------------|
| 1. R5 firmware | 1-5 µs | 10 µs |
| 2. Kernel IRQ handler + wake_up | 5-15 µs | 25 µs |
| 3. Context switch + read() copy | 5-30 µs | 50 µs |
| 4. C library decode | 1-5 µs | 5 µs |
| 5. C++ Event construction | 5-10 µs | 10 µs |
| **Total** | **15-60 µs** | **~100 µs** |

### Why this design — not DMA, not busy-polling

The WREN's "nanosecond precision" claim is about the **FPGA output pin** —
the pulser drives the LEMO connector at the exact scheduled TAI time, verified
to sub-nanosecond accuracy against the White Rabbit grandmaster clock. The
hardware timing path (WR network → FPGA gateware → pulser → LEMO cable) has
no software involvement.

The software path is intentionally conservative because:

1. **Events arrive seconds in advance.** Scheduled CTIMs are delivered ~3.6
   seconds before their due-time. Software being notified 50 µs "late" is
   irrelevant with seconds of margin. The delivery offset mechanism (-5s to
   +5s, µs precision) provides additional scheduling control.

2. **The timing action is in hardware.** For LTIMs, the FPGA fires the output
   pin at the correct nanosecond. The software notification exists for
   logging, diagnostics, and non-timing-critical auxiliary actions — not
   for the primary timing output.

3. **FESA adds orders of magnitude more latency.** Even if the kernel path
   took 2 µs, the FESA real-time scheduler adds ~1.28 ms. Optimizing a
   50 µs kernel path that feeds into a 1280 µs scheduler is diminishing returns.

4. **Reliability beats latency.** `wait_event_interruptible()` + blocking
   `read()` is the most battle-tested IPC mechanism in the Linux kernel.
   No mmap cache-coherence bugs, no DMA descriptor races, no cache-line
   stomping on ARM. The 64-client fan-out with lock-free ring buffers
   and reference-counted buffer pools is proven in production accelerator
   complexes running 24/7 for years.

5. **The hardware data path doesn't support it.** As verified during the
   PCIe DMA investigation (2026-05-01), neither the egress engine's AXI
   slave port nor the scatter-gather DMA engine's M_AXI_SG port is
   connected in the current FPGA build. Device→host DMA or posted-write
   push is not possible without an FPGA rebuild.

### Kernel MMIO: exactly the same mechanism as our sidecar

The kernel driver reads the async ring buffer using `ioread32()`, which on x86
compiles to a plain `volatile uint32_t` load — identical to what our sidecar
does. The evidence from `sw/drivers/wren-core.c`:

Reading ring pointers (lines 359-360):
```c
b_off = ioread32(&mb->async_board_off);   // BAR1 MMIO read
h_off = ioread32(&mb->async_host_off);    // BAR1 MMIO read
```

Reading capsule header (line 4215):
```c
hdr.u32 = ioread32(&mb->async_data[h_off]); // BAR1 MMIO read
```

Reading each capsule data word (line 440):
```c
data = ioread32(&mb->async_data[h_off]);    // BAR1 MMIO read
```

The kernel does 5-8 `ioread32()` calls per capsule (header + 3-4 data words
+ pointer reads). Each is a ~1.3 µs PCIe read round-trip on mkdev30. That's
~8-12 µs of PCIe reads per event, before any scheduling overhead.

The kernel then copies the data to userspace via `put_user()` — an additional
copy that our sidecar doesn't need. Userspace never does its own MMIO; it
simply calls `read()` on `/dev/wren`, which blocks in `wait_event_interruptible_locked()`
(line 156) until the kernel's IRQ thread has done all the work.

### Sidecar: how we beat this

Our ABTWREN sidecar bypasses the entire kernel/userspace chain by reading
the async ring buffer directly via PCIe BAR1 MMIO from a busy-polling thread
on an isolated CPU core at `SCHED_FIFO` priority. Same `ioread32`/`volatile`
mechanism, but without the kernel's IRQ threading, scheduler, and copy overhead:

| Component | Kernel path | Sidecar path |
|-----------|-------------|--------------|
| Detection mechanism | MSI interrupt → IRQ_WAKE_THREAD → threaded handler | Busy-poll `volatile` read |
| `board_off` read | `ioread32()` in IRQ thread | `volatile` load in userspace |
| Capsule reads | `ioread32()` × 4-5 in IRQ thread | `volatile` load × 4-5 in userspace |
| Data path to consumer | `put_user()` copy to userspace | Direct read from BAR1 mmap |
| Scheduling | `wait_event_interruptible_locked()` context switch | None — pinned core, SCHED_FIFO |
| **Total detection** | **15-60 µs** | **~2.6 µs** |

| Approach | Detection latency |
|----------|-------------------|
| Official path (kernel `read()`) | 15-60 µs |
| Sidecar (BAR1 MMIO busy-poll) | ~2.6 µs |

The sidecar reads `async_board_off` from BAR1+0x14000 and processes capsules
directly from BAR1+0x12000 — no interrupts, no context switches, no copies.
The kernel driver continues to operate normally alongside it for FESA/wrentest.

## Async Ring Buffer Detail (what the sidecar reads)

```
BAR1 + 0x12000: async_data[0..2047]  (2048 x 32-bit words, circular)
BAR1 + 0x14000: async_board_off      (firmware write pointer, word index)
BAR1 + 0x14004: async_host_off       (driver read pointer — NEVER touch)
```

### Capsule Format in Ring (same as wire format)

Capsule header (1 word):
```
bits [7:0]   = typ          (0x01=context, 0x02=event, 0x03=config, 0x04=pulse,
                             0x07=continuation, 0x08=rel_act)
bits [15:8]  = source_idx   (RX source index, 0xff for config)
bits [31:16] = len          (total words including header)
```

Event capsule (typ=0x02):
```
word 0: header     {typ=0x02, source_idx, len}
word 1: IDs        {ev_id[15:0], ctxt_id[31:16]}
word 2: ts.nsec    (due-time nanoseconds, TAI)
word 3: ts.sec     (due-time seconds, TAI)
word 4+: params    (TLV-encoded, optional)
```

Context capsule (typ=0x01):
```
word 0: header     {typ=0x01, source_idx, len}
word 1: IDs        {ctxt_id[15:0], padding[31:16]}
word 2: valid_from.nsec
word 3: valid_from.sec
word 4+: params    (TLV-encoded fields)
```

Pulse capsule (typ=0x04):
```
word 0: header     {typ=0x04, source_idx, len}
word 1: comp_idx   (sw_comparator index, from comp_map[])
word 2: tai        (pulse execution seconds, from tm_tai_lo)
word 3: ts         (pulse execution cycles, 30-bit from pulser FIFO)
```
Note: PULSE word order (sec, nsec) is OPPOSITE to EVENT (nsec, sec).
This is because EVENT copies from `wren_packet_ts` {nsec, sec} while
PULSE copies from the hardware pulser FIFO {tm_tai, tm_cyc}.
Verified in `wrenrx-cmd.c` `pulses_handler()` lines 1867-1868.

**The ring buffer only contains events the kernel driver has subscribed to.**
Firmware checks `evsubs[event_id] > 0` before writing to the ring.

**Wire and async formats are identical for capsules** — only the delivery mechanism
differs (Ethernet frame vs shared-memory ring buffer).

---

## PCIe Architecture

The WREN uses a Xilinx AXI PCIe Bridge IP core (PG194/PG195). The Zynq
UltraScale+ PS-side PCIe controller connects to the PL-side bridge, which
provides BAR mapping, ingress/egress translation windows, and a scatter-gather
DMA engine.

### BAR Layout

| BAR | Size | Host label | Maps to | Purpose |
|-----|------|------------|---------|---------|
| BAR0 | 64 KB | `WREN_PSPCIE_BAR` | Bridge regs + DMA + ingress/egress | AXI bridge control, DMA engine at offset 0x0000, ingress engines at 0x8800, egress at 0x8C00 |
| BAR1 | 256 KB | `WREN_REGS_BAR` | PL_MAP_BASE + BTCM | `host_map` registers (0x0000–0x2000), WRPC RAM (0x1000), mailbox (0x10000–0x14000), async ring (0x12000–0x14000) |
| BAR2 | 64 KB | — | OCM (0xfffc0000) | On-chip memory, R5-accessible |
| BAR3 | 2 MB | — | DDR | Device DRAM window |
| BAR4 | 64 KB | — | QSPI (0x00FF0F0000) | Flash memory window |

### Ingress vs Egress

- **Ingress**: Host-initiated. Translates a PCIe BAR address → device AXI address.
  The 8 ingress engines (at BAR0+0x8800) are what make BAR1–BAR4 work.
  Configured once at boot by R5 firmware.

- **Egress**: Device-initiated. Translates a device AXI address → host PCIe address.
  The egress engines (at BAR0+0x8C00) enable the WREN to perform PCIe bus-master
  writes into host RAM. **Currently unused** but fully configured by the R5.

### Scatter-Gather DMA Engine

**Location**: BAR0 offset 0x0000 per channel (4 channels: 0 at +0x00, 1 at
+0x80, 2 at +0x100, 3 at +0x180). AXI address `0xFD0F_0000` (PCIe DMA).
Documented in UG1085 Chapter 30. Mapped by the kernel driver as `wren->psdma`.

**Current state: completely unused for data transfer.** The firmware
(`pcie-core.c`) maps the DMA register aperture (`dreg_base`) and enables the
PCIe interrupt line, but never configures a DMA channel. The kernel driver
only reads `pcie_interrupt_status` for software-generated interrupts
(bit 3, `XPCIE_SOFTWARE_INT`). The scratchpad registers (offset +0x60) are
used for host↔R5 handshake communication.

#### Register map (per channel, 128 bytes)

| Offset | Register | Description |
|--------|----------|-------------|
| 0x00 | SRC_Q_PTR_LO/HI | Source queue base address (PCIe address of descriptors) |
| 0x08 | SRC_Q_SIZE | Source queue size (number of SGL elements) |
| 0x0C | SRC_Q_LIMIT | Index of first element owned by software; DMA wraps here |
| 0x10 | DST_Q_PTR_LO/HI | Destination queue base address |
| 0x18 | DST_Q_SIZE | Destination queue size |
| 0x1C | DST_Q_LIMIT | Destination queue limit |
| 0x20 | STAS_Q_PTR_LO/HI | Source status queue base address |
| 0x30 | STAD_Q_PTR_LO/HI | Destination status queue base address |
| 0x40 | SRC_Q_NEXT | Source queue next pointer (init to 0, DMA advances) |
| 0x44 | DST_Q_NEXT | Destination queue next pointer |
| 0x48 | STAS_Q_NEXT | Source status queue next pointer |
| 0x4C | STAD_Q_NEXT | Destination status queue next (init to 0, write-only) |
| 0x60 | SCRATCH0–3 | Scratchpad registers (R5 ↔ host communication) |
| 0x70 | PCIE_INTERRUPT_CONTROL | PCIe interrupt mask |
| 0x78 | DMA_CONTROL | Channel enable/disable (bit 0 = enable) |
| 0x7C | DMA_STATUS | Channel status (bit 0 = running, bit 15 = channel present) |

#### SGL-Q descriptor format (128-bit entries, UG1085 §30.4)

No linked-list pointers. Queues are linear arrays with wrap at Q_SIZE.
Elements are 128 bits:

```
SRC-Q element:
  [63:0]   Source address (AXI or PCIe)
  [87:64]  Byte count (0 = 2^24 bytes)
  [95:88]  Flags: bit[0]=Location (1=AXI, 0=PCIe), bit[1]=EOP, bit[2]=Interrupt
  [111:96] UserHandle (copied to status Q on EOP)
  [127:112] UserID (copied to status Q)

DST-Q element:
  [63:0]   Destination address
  [87:64]  Byte count
  [95:88]  Flags: bit[0]=Location (1=AXI dest, 0=PCIe dest)
  [111:96] UserHandle

Status-Q element (32 or 64-bit):
  [0]      Completed   [1] SRC error   [2] DST error   [3] Internal error
  [30:4]   Completed byte count
```

#### C2S (Card-to-System / Device→Host) flow

Per UG1085 for single-CPU control:

1. SRC-Q elements: AXI buffer addresses (device memory)
2. DST-Q elements: PCIe buffer addresses (host RAM)
3. Status Q elements: in host RAM
4. Q descriptors are fetched **over PCIe** (must be in host-accessible memory)
5. DMA reads source data over AXI, writes to host via PCIe Memory Write TLPs
6. Status written on EOP

#### Initialization sequence (UG1085 §30.4.5)

1. Verify idle: read DMA_STATUS[running] == 0
2. Write Q_PTR_LO/HI for all four queues
3. Write Q_SIZE for all queues
4. Write Q_NEXT = 0 for all queues
5. Write SRC/DST_Q_LIMIT = 0 (empty), STAS/STAD_Q_LIMIT = SIZE-1
6. Initialize status Q elements to 0
7. Advance SRC/DST_Q_LIMIT to hand elements to DMA
8. Write DMA_CONTROL = 1 (enable)

Only Q_LIMIT registers may be modified while running.

#### DMA engine investigation (2026-05-01)

An exhaustive test campaign was conducted on mkdev30 attempting every
combination of descriptor placement, programming side, and initialization
sequence:

| Variation | Result |
|-----------|--------|
| Descriptors in OCM, programmed from host | SRC_NEXT=0 |
| Descriptors in DDR, programmed from host | SRC_NEXT=0 |
| Descriptors in host RAM, programmed from host | SRC_NEXT=0 |
| SRC/STAS from R5 via CMD_WRITE, DST/STAD from host | SRC_NEXT=0 |
| All registers from R5 via CMD_WRITE | SRC_NEXT=0 |
| Q_PTR_HI correct for >4 GB addresses, cache flushed | SRC_NEXT=0 |
| Various dma_control bit patterns | SRC_NEXT=0 |
| `axi_master` bits 0-2 set, `cfg_disable_pcie_dma_reg_access`=0 verified | SRC_NEXT=0 |

Every configuration gives identical behavior: DMA_STATUS = 0x8001 (bit 15 =
channel present, bit 0 = running), SRC_NEXT stays at 0 regardless of Q_LIMIT.
The channel accepts the enable command but never reads a single descriptor.

**Conclusion: the DMA scatter-gather data path is not functional in the current
FPGA build.** The registers are accessible and the channel accepts the enable
command, but the internal AXI datapaths needed to read descriptors and move
data were never connected in the Vivado block design. The WREN firmware only
uses the DMA IP for scratchpad registers and software interrupts — no DMA data
transfers are ever configured.

The egress engine has the same issue: its AXI slave port is not wired in the
PL block design, verified by confirming no PS→PL address routes into the
bridge's slave port. Both features require Vivado block design changes and an
FPGA bitstream rebuild by the WREN hardware team (BE-CEM).

#### Key documents

| Document | Content |
|----------|---------|
| UG1085 Ch30 | Zynq PS PCIe controller: DMA SGL-Q format, init sequence, C2S flow |
| UG1085 Ch10 | PS-PL register map (0xFD0F_0000 = PCIe DMA, 0xFD0E_0000 = bridge) |
| PG195 v4.1 | PL-based DMA/Bridge Subsystem IP (different IP, Magic=0xAD4B descriptors) |
| UG1087 | Zynq Register Reference (per-field bit descriptions) |
| `xpcie.h` | WREN firmware's DMA register struct, matches UG1085 layout |

### Sub-microsecond path: hot-patch the firmware, no source changes needed

The R5 firmware already exposes two powerful mailbox commands:

| Command | ID | Purpose |
|---------|----|---------|
| `CMD_WRITE` | 1 | Writes 32-bit words to any R5-accessible address |
| `CMD_EXEC` | 3 | Calls a function at any R5-accessible address |

**PL-PS address map limitation:** The PL can reach DDR (0x00000000), FPD
peripherals (0xFD000000-0xFDFFFFFF), and QSPI (0xC0000000). It cannot reach
PS LPD registers like CRL_APB (0xFF5E0000). However, the R5 can access
CRL_APB natively. This means `CMD_WRITE` to address `0xFF5E0218` with value
`0x10` triggers an R5 soft-reset — the same mechanism the firmware's own
`propagate_reset()` uses.

Combined, these primitives enable a firmware hot-patch workflow with zero
permanent changes:

```
1. Read firmware binary     CMD_READ from 0xFFFE0000 (OCM where code lives)
                            → exact ARM/Thumb machine code the R5 runs

2. Find injection point     disassemble, locate the store instruction that
                            bumps async_board_off after writing a capsule

3. Write stub to DDR        mmap BAR3, write ARM stub code:
                              • executes original async_board_off store
                              • writes to DMA_CONTROL to kick a transfer
                              • returns to main loop

4. Atomically patch         Send CMD_WRITE: overwrite one BL instruction
                            in OCM with a branch to our DDR stub

5. Result (requires         firmware writes capsule, bumps board_off,
   FPGA rebuild for HW      triggers DMA → host polls completion in L1
   data path)               cache at ~1 ns. FESA/wrentest unchanged.
```

**Note:** The egress engine's AXI slave port and the DMA engine's
scatter-gather master are not connected in the current FPGA build.
The hot-patch injection mechanism (steps 1–4) is fully operational,
but the hardware data path to push data to host RAM requires Vivado
block design changes.

**Safety:** Every change lives in volatile memory.
- OCM patch: lost on power-cycle
- DDR stub: lost on power-cycle
- Power-cycle → original QSPI firmware boots → factory state

**Expected end-to-end latency** (LTIM pulse on FPGA → host action):

| Step | Latency |
|------|---------|
| FPGA pulser fires → R5 interrupt | 1-5 µs |
| Firmware writes capsule + bumps board_off | ~100 ns |
| Host polls board_off via BAR1 MMIO read | ~1300 ns |
| Host reads capsule via BAR1 MMIO | ~1300 ns |
| **Total detection (current MMIO poll)** | **~2.6 µs** |
| **End-to-end (FPGA fire → host action)** | **~4-8 µs** |

The ~1300 ns poll cost is the PCIe read round-trip floor — a hardware
constraint of the current FPGA build. The egress engine's AXI slave port and
the DMA engine's scatter-gather master are not connected. Breaking below
~1300 ns would require either:

1. Connecting the egress engine's S_AXI port in the Vivado block design
2. Connecting the DMA engine's M_AXI_SG port in the Vivado block design
3. A custom VHDL module that writes directly to PCIe on pulser fire

Options 1-2 are VHDL/block design changes. Option 3 bypasses the R5
entirely (FPGA writes directly to PCIe posted-write → host L1 cache in
~100 ns).

### Custom firmware: full replacement path

For deeper changes (new mailbox commands, altered scheduling, DMA descriptor
programming), a complete firmware replacement can be tested without flashing
QSPI:

```
1. Write new firmware to DDR     mmap BAR3, write firmware binary

2. Patch BTCM reset vector       mmap BAR1, overwrite first 32 bytes at
                                 offset 0x10000 → reset vector points to
                                 DDR bootloader. Safe: exceptions go to
                                 0xFFFF0000 (HIVECS), not BTCM base.

3. Trigger R5 soft-reset        Send CMD_WRITE: address=0xFF5E0218, value=0x10
                                 R5 resets → boots DDR firmware

4. Recovery                     Power-cycle → QSPI firmware restored
```

**R5 BTCM at BAR1+0x10000** — 64 KB R5 program/data memory, readable and
writable via PCIe. The kernel driver already loads the RISC-V WRPC firmware
this way (`wrc.elf` to BAR1+0x1000 at boot).

**QSPI flash at BAR4** — 64 KB window into the boot flash. Only write here
once the firmware is fully validated via DDR/BTCM testing.

**MicroUSB** — UART serial console. The R5 firmware has a CLI (`main_loop`
polls `uart_can_read`). Recovery path if PCIe-based reflashing goes wrong.

**Risk on shared hardware:** mkdev30 is a shared CERN test machine. The
hot-patch approach (egress stub in DDR) is zero-risk — nothing survives
power-cycle. Full firmware replacement via BTCM/DDR is also safe as long as
QSPI is not touched. QSPI flashing requires WREN team (BE-CEM) approval.

---

## Physical I/O

- **Front panel**: 2-8 LEMO00 (bidirectional) + high-density coax (32+ I/Os) + SFP + JTAG + microUSB
- **Backplane**: Up to 32 outputs (VME/PXIe), duplicates of front panel signals
- **Bidirectional I/Os**: Configurable as triggerer output, external start/stop/clock input
- **Buffers**: 3V on 50 Ohms, LED per I/O
- **Input timestamping**: TAI timescale, ns precision

Group output logic: each group's 8 triggerer outputs combinable via AND/NAND/OR/NOR gates
with configurable polarity.

---

## CERN Deployment

- All timing domains on a **single WR network**, separated by VLANs
- Tree topology, 5 layers deep, ~200 WR switches, ~2000 receivers
- Traffic flows toward leaves only (precaution for determinism)
- Max propagation latency: ~500 us (estimated, to be measured)
- Grandmaster clock provides common time reference to all VLANs
- Migration: SPS/LHC by 2029 (LS3), remaining by 2033 (LS4)
- Parallel operation with legacy GMT during transition
- As of summer 2025: few dozen pilot FECs deployed

---

## Diagnostics: wrentest CLI

Command-line diagnostic tool with hierarchical sub-commands:
- `status` — network link, synchronization, time
- `events` / `fields` — CTIM event and field descriptions
- `ltim` — LTIM configuration and management
- `io` — I/O pin configuration
- `wait` — monitor CTIM events and LTIM interrupts as they occur
- `history` — past CTIMs, LTIMs, rising/falling edges on I/O pins
- `troubleshooting` — diagnose LTIM misbehavior, suggest fixes

---

## Key Source Files

| What | Path |
|------|------|
| **Ring buffer layout** | `wren-gw/sw/hw-include/mb_map.h` |
| **Capsule/packet format** | `wren-gw/sw/api/include/wren/wren-packet.h` |
| **Async message types** | `wren-gw/sw/hw-include/wren-mb-defs.h` |
| **Host register map** | `wren-gw/sw/hw-include/host_map.h` |
| **Mailbox commands** | `wren-gw/sw/hw-include/wren-mb-cmds.def` |
| **Kernel driver** | `wren-gw/sw/drivers/wren-core.c` |
| **Kernel structures** | `wren-gw/sw/drivers/wren-core.h` |
| **IOCTL definitions** | `wren-gw/sw/drivers/wren-ioctl.h` |
| **R5 firmware main** | `wren-gw/sw/firmware/main.c` |
| **RX firmware core** | `wren-gw/sw/firmware/wrenrx-core.c` |
| **Userspace RX API** | `wren-gw/sw/api/include/wren/wrenrx.h` |
| **VHDL top-level** | `wren-gw/hdl/rtl/wren_core.vhd` |
| **Triggerer VHDL** | `wren-gw/hdl/rtl/pulser_group.vhd` |
| **Receiver interface** | `timing-receiver/src/timing-receiver/Receiver.h` |
| **Context cache** | `timing-receiver/src/timing-receiver/ContextCache.h` |
| **WrenReceiver** | `timing-receiver-wren/src/timing-receiver-wren/WrenReceiver.h` |
| **Trigger conditions** | `timing-receiver-wren/src/timing-receiver-wren/TriggerCondition.h` |
| **WREN message types** | `timing-receiver-wren/src/timing-receiver-wren/WrenMessage.h` |
| **Domain classes** | `timing-domain/src/timing-domain/Domain.h` |
| **CPS events catalog** | `timing-domain-cern/src/timing-domain-cern/CpsDomain.cpp` |
| **PSB events catalog** | `timing-domain-cern/src/timing-domain-cern/PsbDomain.cpp` |
| **SPS events catalog** | `timing-domain-cern/src/timing-domain-cern/SpsDomain.cpp` |
| **LHC events catalog** | `timing-domain-cern/src/timing-domain-cern/LhcDomain.cpp` |
| **Demo receiver app** | `timing-receiver-wren/src/demo/receiver-wren.cpp` |
| **WREN functional spec** | `WREN_LTIM/WREN_Specs_20230209.pdf` |
| **WREN study document** | `WREN_LTIM/WREN_STUDY.pdf` |
| **ICALEPCS'25 paper** | WEBR003 (Moscardi et al.) |

All source paths under `/opt/public/FESA_core_repos/` prefixed with `WREN_LTIM/` or `timing/timing-wrt/`.

---

## FEC Boot Sequence: How WREN Is Actually Configured (Investigated Feb 2025)

Investigated on `cfc-865-mkdev30` (PCIe WREN at BDF `03:00.0`, device `10dc:0455`).

### The Boot Chain

1. **Power-on**: FPGA bitstream loads from flash. R5 firmware boots from flash, enters `main_loop()` (polls mailbox, NIC, pulsers). WR Core (RISC-V) also boots and begins PTP sync.

2. **`fec-hw.service`** (systemd): Runs `/usr/sbin/fec-hardware-setup`, which parses `/etc/transfer.ref` and executes each `#%` line via bash.

3. **`transfer.ref`** contains: `#% cd /usr/local/drivers/wren; ./install.sh`

4. **`install.sh`** → **`install-drv.sh`**, which does:

### install-drv.sh Step-by-Step

```
Step 1: Parse transfer.ref for PCIe_WREN entries
  awk for slot number → map to PCI BDF via /var/run/dynpci

Step 2: Enable PCI memory space
  setpci -s $pcie_slot COMMAND=0x02

Step 3: Determine firmware build version
  ./wren-ls driver-version pcie $pcie_slot  → reads BAR1 register → "fb000011"

Step 4: Load WR PTP Core firmware (RISC-V)
  ./wrpc load -b pci -f /sys/bus/pci/devices/0000:$slot/resource1 \
      -o 0x1000 $drv_ver/wrc.elf
  (Writes ELF into WR Core RAM at BAR1+0x1000)

Step 5: Install shared libraries
  ln -s $drv_ver/lib/libwrenrx.so* /tmp/wren/
  ln -s $drv_ver/lib/libwrenrx.so* /run/wren/
  ln -s $drv_ver/bin /run/wren/

Step 6: Load kernel driver
  insmod $drv_ver/wren-core.ko
  insmod $drv_ver/wren-pcie.ko

Step 7: Create /dev/wren* device links
  chmod 666 /dev/wrenN
```

### What the Kernel Driver Does on Probe (wren-pcie.c → wren-core.c)

```
wren_pcie_probe():
  1. pcim_enable_device()
  2. Map BAR0 (PS DMA) and BAR1 (registers + mailbox)
  3. Set DMA mask (64-bit or 32-bit fallback)
  4. pci_set_master()
  5. wren_register() → creates /dev/wrenN, sysfs entries
  6. request_threaded_irq() → MSI interrupt for mailbox + async events
  7. wren_init_hw():
     - Clear all pending interrupts: iack = 0xffffff
     - Set IMR = MSG | ASYNC | WR_SYNC | ALARM (+ CLOCK if already synced)
     - Sync async_host_off = async_board_off (empty the ring)
  8. wren_reset():
     - CMD_RX_RESET via mailbox → firmware clears all subscriptions/actions
     - CMD_HW_GET_CONFIG → firmware returns hardware capabilities
```

### The Broken State on mkdev30 (Feb 2025)

**Root cause**: Kernel module compiled against different kernel config, CRC mismatch:
```
wren_core: disagrees about version of symbol module_layout
```
Running kernel: `5.10.245-rt139-fecos03`

**Consequences**:
- Steps 6-7 fail → no `/dev/wren*`, no interrupts, no `wren_reset()`
- `wrpc load` (Step 4) disrupts R5 firmware — R5 sets `fw_version` register
  but stops polling mailbox (`h2b_csr` commands are never consumed)
- WR PTP Core still works (link_up=1, time_valid=1, TAI counting)
- Mailbox is completely unresponsive (R5 main_loop not executing)

### What We Can Do Without the Kernel Driver (Direct PCIe mmap)

Our `WRENTester` (ABTEdge) bypasses the kernel driver entirely via BAR1 mmap:

**Working** (confirmed on mkdev30):
- Discover WREN via sysfs PCI scan (vendor=0x10DC, device=0x0455)
- Map BAR1 via `mmap(/sys/bus/pci/devices/.../resource1)`
- Read all host registers: IDENT, MAP_VER, FW_VER, WR_STATE, TAI time, ISR, IMR
- Read async ring buffer (board_off / host_off pointers + capsule data)
- Send mailbox commands: CMD_RX_SUBSCRIBE, CMD_RX_SET_COND, CMD_RX_SET_ACTION
- Clean up: CMD_RX_DEL_ACTION, CMD_RX_DEL_COND

**Requires R5 firmware running** (i.e. kernel module loads OR boot scripts don't kill R5):
- Mailbox command/response (R5 polls `h2b_csr` in `main_loop()`)
- Event subscription delivery to async ring
- LTIM/pulser configuration and firing

### Key Insight: R5 Firmware Architecture

The R5 firmware (`main.c`) runs bare-metal on the ARM Cortex-R5 (PS side of Zynq).
Its `main_loop()` polls these in a tight loop every iteration:
1. WR sync state (`regs->wr_state`)
2. NIC receive (`nic->eic.eic_isr`)
3. **Mailbox** (`mb->h2b_csr` — this is what we need)
4. Pulser interrupts (`regs->pulsers_int`)
5. TX table polling
6. SW comparator polling
7. UART for CLI ('C' enters blocking menu — avoid sending chars to R5 UART)

The mailbox is at BTCM (R5 side: `0xF0000000`, PCIe side: BAR1+0x10000).
The `mb_map.h` struct defines: B2H (board→host) at +0x0000, H2B (host→board) at +0x1000,
async ring at +0x2000, async pointers at +0x4000.

### Fix Applied

Kernel team recompiling all 17 failed modules for `5.10.245-rt139-fecos03`.
Once `wren-core.ko` and `wren-pcie.ko` load, the standard boot sequence works end-to-end.

---

## Firmware Audit Checklist (for manual verification)

All files live under `/opt/public/FESA_core_repos/WREN_LTIM/wren-gw/sw/`.

### The 4 files that matter for the sidecar

| What to check | File | What breaks if it changes |
|----------------|------|---------------------------|
| **Host register offsets** (IDENT, WR_STATE, TAI, ISR, etc.) | `hw-include/host_map.h` | `performRegisterDump()`, all PCIe reads |
| **Async ring layout** (data array size, board_off/host_off) | `hw-include/mb_map.h` | `RING_MASK`, `WREN_ASYNC_*` offsets |
| **Capsule type codes + header format** (TYP_*, CMD_ASYNC_*) | `hw-include/wren-mb-defs.h` | `transmitAll()` switch cases |
| **Capsule word layouts** (how EVENT/PULSE/CONFIG are written) | `firmware/common/wrenrx-cmd.c` | sec/nsec order, field extraction |

### How to audit `wrenrx-cmd.c`

Search for these functions — each one writes a capsule type to the async ring:

| Capsule | Function | Lines to check |
|---------|----------|----------------|
| EVENT | `wrenrx_run_event()` | `async_write32` sequence for ev_id, nsec, sec |
| PULSE | `pulses_handler()` | `async_write32` sequence for comp_map, tai, ts |
| CONFIG | `async_send_config()` | `async_write32` sequence for act_idx, sw_cmp_idx |
| CONTEXT | `wrenrx_run_context()` | `async_write32` sequence for ctxt_id, nsec, sec |
| REL_ACT | `cmd_rx_del_action()` | 2-word capsule, only carries action index |

### Secondary files (good to skim)

- `hw-include/wren-mb-cmds.def` — mailbox command enum (new commands added?)
- `hw-include/pulser_group_map.h` — pulser hardware register layout
- `api/include/wren/wren-common.h` — `WREN_ETHERTYPE`, `MAX_RX_ACTIONS`, `WREN_EVENT_ID_MAX`
- `api/include/wren/wren-packet.h` — wire capsule structs (`wren_capsule_hdr`, `wren_packet_ts`)
- `api/include/wren/wren-hw.h` — pulser/group counts (`WREN_NBR_PULSERS`, `WREN_NBR_COMPARATORS`)
- `firmware/common/wrenrx-data.h` — `MAX_RX_ACTIONS`, `MAX_RX_SOURCES`, `MAX_RX_CONDS`

### Last verified

2026-04-24: All register offsets, capsule formats, type codes, and word ordering
match between firmware headers and `WRENProtocol.hpp` / `WRENTransmitter.cpp`.
New type `CMD_ASYNC_REL_ACT (0x08)` exists; handled safely by `default: break;`.
