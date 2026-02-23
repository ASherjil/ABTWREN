# FESA Real-Time Action Integration

## How FESA Timing Events Work

There are two layers of event triggering in the current WREN/LTIM system.

### Layer 1: LTIM Class (WREN_DU) — Hardware Interrupt Handler

Each LTIM device in `WREN_DU.cfc-865-mkdev30.xml` uses a **Custom event source**:

```xml
<acquisitionInterrupt>
  <event-configuration name="AcquisitionInterrupt">
    <Custom>
      <AcquisitionEventSource custom-event="defaultEvent"/>
    </Custom>
  </event-configuration>
</acquisitionInterrupt>
```

`AcquisitionEventSource` (generated + user code in the LTIM FESA class):
- Implements `wait()` → calls `getMultiReceiver().waitEvent()` → blocks on `WrenReceiver`
- When a pulse fires on a channel, the WREN card generates an interrupt
- `wait()` unblocks, creates an `RTEvent` with device name + timing context
- Triggers the LTIM class's RT action

### Layer 2: Application Class (e.g. KiTSGeneric) — Timing Event Consumer

In `DeviceData_KiTSGeneric.instance`, the app uses **Timing event sources** referencing LTIM device names:

```xml
<leNewCycle>
  <event-configuration name="ecNewCycle">
    <Timing>
      <hardware-event name="MKDEV30.865.WRLTIM_0-1_TG-NC"/>
    </Timing>
  </event-configuration>
</leNewCycle>
```

`TimingEventSource`:
- Implements `wait()` → calls `receiver_.waitEvent()` → blocks on `timing::receiver::Receiver`
- Subscribes to hardware event names via `receiver_.subscribe("MKDEV30.865.WRLTIM_0-1_TG-NC")`
- When event fires, creates `TimingContext`, dispatches to RT actions (leFw, lePp, leNewCycle, etc.)

### Complete Chain (Current System, mkdev30)

```
WREN PCIe card fires pulse on channel N
    ↓
WrenReceiver (timing-receiver-wren) reads PCIe BAR1 ring buffer
    ↓
AcquisitionEventSource::wait() unblocks (Custom event source, LTIM class)
    ↓
LTIM RT action fires → notifies timing system
    ↓
TimingEventSource::wait() unblocks (Timing event source, KiTSGeneric)
    ↓
Application RT actions execute (leFw, lePp, leNewCycle, etc.)
```

---

## FESA Event Source Architecture

### Class Hierarchy

```
AbstractEventSource (abstract base, fesa-core)
├── TimingEventSource (fesa-core-cern, blocks on timing::receiver::Receiver)
├── TimerEventSource
├── OnDemandEventSource
├── MultiThreadedEventSource
│   └── OnSubscriptionEventSource
├── TransactionEventSource
└── Custom event sources (generated code per FESA class)
    └── AcquisitionEventSourceBase → AcquisitionEventSource (user code)
```

### Event Dispatch Flow

```
AbstractEventSource::run()           # event source thread (one per source)
    ↓
wait(RTEvent&)                       # pure virtual — blocks on hardware
    ↓
postEventToSchedulers(RTEvent)       # eventMap lookup: concrete name → LogicalEvent
    ↓
LogicalEvent::postEventToSchedulers  # routes to registered RTSchedulers
    ↓
RTScheduler::post()                  # enqueues to bounded event queue
    ↓
RTScheduler::run()                   # scheduler thread dequeues
    ↓
AbstractRTAction::executeAction()    # calls user's execute(RTEvent*)
```

### Key Virtual Methods

| Class | Method | Purpose |
|-------|--------|---------|
| AbstractEventSource | `wait(RTEvent&)` | Block until hardware event, populate RTEvent |
| AbstractEventSource | `connect(EventElement&)` | Subscribe to hardware when event registered |
| AbstractRTAction | `execute(RTEvent*)` | User code executed when event fires |

---

## The timing::receiver::Receiver Interface

Located at: `/opt/public/FESA_core_repos/timing/timing-wrt/timing-receiver/src/timing-receiver/Receiver.h`

### Pure Virtual Methods

```cpp
class Receiver {
public:
    virtual Time utc() const = 0;
    virtual Context getContext(const Domain& domain, const Time& time) const = 0;
    virtual Context getContext(const Domain& domain) const = 0;
    virtual void subscribe(const std::string& eventName) = 0;
    virtual void unsubscribe(const std::string& eventName) = 0;
    virtual void subscribeContext(const Domain& domain) = 0;
    virtual void unsubscribeContext(const Domain& domain) = 0;
    virtual Event waitEvent() = 0;
};
```

### Receiver Implementations

```
Receiver (abstract base)
├── ContextAwareReceiver (context caching helper)
│   ├── WrenReceiver      (PCIe BAR1 → wrenrx kernel driver)
│   ├── SoftReceiver      (software distributor)
│   └── MockReceiver      (XML supercycle playback / UDP multicast reception)
├── CtrReceiver           (legacy GMT)
└── WrenMultiReceiver     (aggregates multiple WrenReceivers via poll())
```

### How WrenReceiver::waitEvent() Works

1. Calls `rxHandle.waitForMessage()` — blocks on `wrenrx_wait2()` kernel driver fd
2. Receives raw `wrenrx_msg*`: event ID, timestamp, context ID, payload
3. Looks up `EventDescriptor::of(eventId)` — static registry maps ID → name + domain
4. For CTim events: creates event with EventDescriptor, context from cache, trigger time
5. For LTim triggers (TRIGGER type): creates event with TriggerDescriptor name, caches first pulse
6. Returns fully-formed `timing::Event`

---

## Key Types (timing-domain library)

Located at: `/opt/public/FESA_core_repos/timing/timing-wrt/timing-domain/src/timing-domain/`

### timing::Id
```cpp
using Id = int32_t;
```

### timing::Time
```cpp
using Time = std::chrono::time_point<std::chrono::high_resolution_clock, std::chrono::nanoseconds>;
```
Serialized as 8 bytes (int64_t nanoseconds since epoch).

### timing::Event
```cpp
class Event : public FieldsAccessor {
    Event(Context context, const EventDescriptor& descriptor, const Time& triggerTime,
          std::shared_ptr<const FieldsContainer> fieldsContainer);
    Event(Context context, std::string name, const Time& triggerTime,
          std::shared_ptr<const FieldsContainer> fieldsContainer);

    const Context& getContext() const;
    const std::string& getName() const;
    const Time& getTriggerTime() const;
    void serialize(std::ostream& os) const;
};
```

### timing::Context
```cpp
class Context : public FieldsAccessor {
    Context(const Domain& domain, std::map<std::string, Field> fields, Time validFrom);
    const Time& getValidFrom() const;
    void serialize(std::ostream& os) const;
};
```

### timing::Domain
Static registry — `Domain::of("CPS")` returns a singleton reference.
CPS domain: `CpsDomain("CPS", 1200ms, 1)` — **domain ID = 1**.
Defined in `timing-domain-cern/src/timing-domain-cern/CpsDomain.cpp`.

### timing::Field
```cpp
using Field = FieldBase<UnknownFieldValueType>;
// FieldVariant = boost::variant<bool, int32_t, int64_t, float, std::reference_wrapper<const EnumItem>>
```

### FieldsContainer
Abstract base for field storage. `MapFieldsContainer` is the simplest concrete implementation using `std::map<std::string, Field>`.

### Serialization Primitives
```cpp
// timing-domain/Serialization.h — raw memcpy, native byte order
template<typename T> void write(std::ostream& os, const T& value) {
    os.write(reinterpret_cast<const char*>(&value), sizeof(value));
}
template<typename T> T read(std::istream& is) {
    T value;
    is.read(reinterpret_cast<char*>(&value), sizeof(value));
    return value;
}
```

All field values are serialized as `int64_t` (8 bytes) regardless of actual type (`bool`, `int32_t`, `float`, `EnumItem`). The `FieldVariantSerializer` casts everything to `int64_t` before writing. Floats use a union trick.

---

## ReceiverProvider Factory — CLOSED

Located at: `/opt/public/FESA_core_repos/timing/timing-wrt/timing-receiver-cern/src/timing-receiver-cern/ReceiverProvider.h`

### Why It's Closed

1. **ReceiverType enum** — hardcoded: CTR, WREN, SOFTWARE, MOCK, NONE
2. **ReceiverProvider::getReceiver()** — switch statement with `std::make_shared<ConcreteType>()`
3. **CERNControllerFactory::getTimingReceiver()** — only accepts `"gmt"`, `"soft"`, `"wrt"` strings
4. No plugin mechanism, no registry, no dynamic loading

**Cannot inject a custom receiver without modifying fesa-core-cern and timing-receiver-cern.**

---

## Integration Path: MockReceiver as UDP Backend

The `MockReceiver` has a UDP multicast reception mode already supported by the factory with zero FESA code changes.

### MockReceiver Network Config

| Parameter | Value |
|-----------|-------|
| Protocol | UDP IPv4 multicast |
| Default multicast address | `239.87.82.84` (mnemonic: 239.W.R.T) |
| Default port | `28020` |
| TTL | 0 (local machine by default) |
| Max packet size | 2048 bytes |
| Byte order | Host (little-endian on x86) |
| Serialization | Binary via `timing::utils::write<T>()` / `read<T>()` |

### Activation in FESA

Set in FESA process configuration properties:
- `timing.simulation.mode=true` → factory creates MockReceiver
- Optionally `timing.simulation=<ip>:<port>` → custom listen address
- If empty → listens on default `239.87.82.84:28020`

### MockReceiver Subscription Filtering

MockReceiver **filters by event name** (`MockReceiver.cpp:87`):

```cpp
if (enableAllEvents || subscribedEvents.find(event.getName()) != subscribedEvents.end()) {
    std::this_thread::sleep_until(event.getTriggerTime());
    return event;
}
```

- Events whose name is NOT in `subscribedEvents` are **silently discarded**.
- FESA's `TimingEventSource::connect()` calls `receiver_.subscribe(eventName)` for each registered event.
- Event names we send must **exactly match** the names in the FESA instance file.
- `sleep_until(triggerTime)`: for FIRE events the fire time is already in the past → **returns immediately**. Only ADVANCE events (with future times) would block. Solution: only send FIRE packets.

### LTIM Device Names (from KiTSGeneric instance)

```
MKDEV30.865.WRLTIM_0-1_TG-NC
MKDEV30.865.WRLTIM_0-2_TG-EC
MKDEV30.865.WRLTIM_0-3_TG-FW800
MKDEV30.865.WRLTIM_0-6_TG-BEFOREPP
MKDEV30.865.WRLTIM_0-7_TG-PP
MKDEV30.865.WRLTIM_0-8_TG-AFTERPP
MKDEV30.865.WRLTIM_0-23_EC
PX.SCY-CT                           (CTIM)
PIX.F900-CT                         (CTIM)
```

---

## Complete Binary Wire Format

### Event Packet Layout (marker = `0x45` 'E')

All values are native-endian (little-endian on x86_64). `std::size_t` is **8 bytes** on x86_64.

```
Offset  Size       Field
──────  ────       ─────
0       1B         Marker: 0x45 ('E')

── Event::serialize() ──────────────────────────────────────────
1       4B         descriptorId (int32_t)
                     if descriptor exists: descriptor->getId()
                     if no descriptor: -1 (0xFFFFFFFF) ← WE USE THIS
5       8B         name.size() (std::size_t = 8B on x86_64)
13      NB         name chars (NOT null-terminated)

13+N    8B         triggerTime (int64_t, nanoseconds since epoch)

── FieldsAccessor::serialize() (Event base class) ─────────────
21+N    4B         domainId (int32_t, CPS = 1)

── EventFieldsContainer::serialize() ──────────────────────────
  ── MapFieldsContainer::serialize() (event fields) ───────────
  25+N    8B       event field count (std::size_t)
                     (= 0 for empty fields)
  per field (if any):
    +0     4B      fieldDescriptor.getId() (int32_t)
    +4     8B      field value (always int64_t, 8 bytes)

  ── Context::serialize() ─────────────────────────────────────
  33+N    8B       validFrom (int64_t, nanoseconds since epoch)

  ── FieldsAccessor::serialize() (Context base class) ─────────
  41+N    4B       domainId (int32_t, CPS = 1)

  ── MapFieldsContainer::serialize() (context fields) ─────────
  45+N    8B       context field count (std::size_t)
                     (= 0 for empty fields)
  per field (if any):
    +0     4B      fieldDescriptor.getId() (int32_t)
    +4     8B      field value (always int64_t, 8 bytes)
```

### Concrete Example: Empty-Fields Event Packet

For LTIM name `"MKDEV30.865.WRLTIM_0-1_TG-NC"` (29 chars), CPS domain (ID=1):

```
Offset  Size   Value
0       1B     0x45
1       4B     0xFFFFFFFF (NO_DESCRIPTOR)
5       8B     29 (name length)
13      29B    "MKDEV30.865.WRLTIM_0-1_TG-NC"
42      8B     <triggerTime>    ← PATCH PER EVENT
50      4B     1 (CPS domain ID)
54      8B     0 (zero event fields)
62      8B     <validFrom>      ← PATCH PER EVENT (= triggerTime)
70      4B     1 (CPS domain ID)
74      8B     0 (zero context fields)
────────────
Total: 82 bytes
```

---

## Latency Analysis

### Real WREN Path (mkdev30, standard FESA)

```
WREN FPGA fires pulse
    ↓ ~5-15us    kernel IRQ + DMA + context switch
wrenrx_wait2() unblocks (WrenRxHandle.cpp:459)
    ↓ ~5-15us    WrenMessage → EventDescriptor::of() → context cache → Event construction
                 (shared_ptr, WrenFieldsContainer, std::map)
Event returned to TimingEventSource::wait()
    ↓ ~1-5us     postEventToSchedulers → RTScheduler queue
RT action fires
────────────
Total: ~20-50us
```

### Our MockReceiver Path (mkdev16, packet_mmap)

```
packet_mmap NIC poll (already spinning, ~0us wait)
    ↓ ~1us       parseAndEnqueue → SPSCQueue (lockfree, ~50ns)
EventProcessor pops
    ↓ ~1-2us     template packet: 2x memcpy (timestamps) + sendto()
UDP loopback (kernel, memory-only)
    ↓ ~1-5us     kernel UDP stack, no wire
MockReceiver::getNext() → socket.receive_from() unblocks
    ↓ ~5-15us    std::string copy + istringstream + Event::deserialize()
                 (shared_ptr allocs, map construction — CANNOT AVOID)
    ↓ ~0us       sleep_until(past time) → immediate return
Event returned to TimingEventSource::wait()
    ↓ ~1-5us     postEventToSchedulers → RTScheduler queue
RT action fires
────────────
Total: ~10-30us (template packet path)
```

### Comparison

| Step | Real WREN | Our MockReceiver Path |
|------|-----------|----------------------|
| Hardware → userspace | ~15us (IRQ + DMA) | ~1us (packet_mmap polling) |
| Event construction | ~10us (WrenFieldsContainer) | ~2us (template memcpy + sendto) |
| UDP overhead | N/A | ~3us (loopback) |
| Deserialization | N/A | ~10us (MockReceiver, fixed) |
| FESA dispatch | ~5us | ~5us |
| **Total** | **~20-50us** | **~10-30us** |

Our path trades the kernel IRQ wake-up cost (~15us, which we avoid via polling) for the UDP serialize/deserialize cost. With the template packet approach, they roughly cancel out — possibly faster due to no kernel interrupt.

The MockReceiver deserialization overhead (~10us) is unavoidable without modifying FESA code, but with empty fields it's minimized (no FieldDescriptor lookups, no field map population).

---

## Heap Allocation Audit

### Our Side (EventProcessor → UDP send) — WE CONTROL

**Naive approach** (construct timing::Event + ostringstream):
1. `std::make_shared<MapFieldsContainer>` × 2 (event + context fields)
2. `std::make_shared<EventFieldsContainer>`
3. `std::map<string, Field>` copies
4. `std::ostringstream` internal heap buffer
5. `os.str()` copies the serialized string

**Template packet approach** (pre-built buffer, patch timestamps):
- **ZERO heap allocations on the hot path**
- 2× `memcpy` of 8 bytes (timestamps) into stack buffer
- 1× `sendto()` syscall

### MockReceiver Side (UDP recv → FESA) — CANNOT CHANGE

Every `getNext()` call (`MockReceiver.cpp:117-136`):
1. `std::string str(buffer.data(), received)` — heap copy of ~82 bytes
2. `std::istringstream s(str)` — heap buffer
3. `Event::deserialize()`:
   - `boost::container::small_vector<char, 64>` for name (29 chars fits!)
   - `MapFieldsContainer::deserialize()` — reads nFields=0, empty `std::map`
   - `Context::deserialize()` — another empty `MapFieldsContainer`
   - `new EventFieldsContainer(...)` via `shared_ptr`
4. `std::make_shared<MapFieldsContainer>(event.getFields())` (line 84) — copies fields again

With **zero fields**, this is ~3-4 heap allocations per event. Heavy but unavoidable.

---

## Context Field Requirements (TimingContext)

### What TimingContext::initFields() Needs

When MockReceiver returns a `timing::Event`, FESA constructs a `TimingContext` from it.
The constructor (`TimingContext.cpp:82-97`) calls `initFields()` which reads:

1. **CYCLE_STAMP** — `timing::cern::CycleStamp.getValue(fields)` → `int64_t` (nanoseconds since epoch)
2. **USER** — `timing::cern::User.getValue(fields)` → `EnumItem` (e.g. ZERO, SFTPRO1, LHC1, etc.)

These fields are looked up via `CernFieldDescriptor::getValue()` which calls
`FieldsAccessor::getFieldValue()` → needs the field's `FieldDescriptor::getId()`.

### Failure Mode: Both try-catch blocks are NON-FATAL

```cpp
TimingContext::TimingContext(timing::Event eventValue) {
    try {
        timeStamp_ = eventValue_->getTriggerTime().time_since_epoch().count();
        initFields(eventValue_.get());     // ← may throw
    } catch (const std::exception& e) {
        LOG_ERROR(...);                     // ← logs, does NOT rethrow
    }
}

void TimingContext::initFields(const FieldsAccessor& fields) {
    try {
        cycleStamp_ = CycleStamp.getValue(fields);      // ← throws if missing
        const auto& userField = User.getValue(fields);   // ← throws if missing
        cycleName_ = domain + ".USER." + userField.getName();
    } catch (const std::exception& e) {
        LOG_ERROR(...);                     // ← logs, does NOT rethrow
    }
}
```

**The RT action still fires** even with empty fields — just with degraded TimingContext:
- `getCycleName()` → empty string
- `getCycleStamp()` → 0
- `getCycleTime()` → garbage (divides by zero cycle stamp)

### Two Options

**Option A: Empty fields (simplest, for initial testing)**
- Zero fields in event and context packets
- RT actions fire, TimingContext is degraded
- FESA logs errors but continues
- Template packet stays at ~82 bytes, zero runtime deps

**Option B: CYCLE_STAMP + USER fields (production-correct)**
- Include exactly 2 context fields:
  - `CYCLE_STAMP` (int64_t): nanoseconds since epoch of cycle start
  - `USER` (EnumItem): serialized as `int64_t(enumItem.getValue())` — e.g. `ZERO` = value 7
- Need `FieldDescriptor::getId()` for each — from `timing-domain-cern` static registry
- Template packet grows by 2 × (4B descriptor ID + 8B value) = 24 bytes → ~106 bytes
- Full TimingContext with correct cycle name (`CPS.USER.ZERO`) and cycle timestamp

**Recommendation**: Start with Option A for testing, move to Option B for production.
For Option B, discover field IDs at startup by linking `libtiming-domain-cern.so`:
```cpp
auto cycleStampId = Domains::cps().CYCLE_STAMP.getId();
auto userId = Domains::cps().USER.getId();
auto zeroValue = Domains::cps().USER.ZERO.getValue();  // uint16_t
```

### Other Receiver Paths Investigated

**SoftReceiver** (`timing.receiver=soft`): Uses CMW RDA3 middleware to receive events from
Timing Distributor services over the network. Higher latency than MockReceiver UDP (RDA3
middleware overhead + network to distributor). Not useful for our case.

**WrenReceiver** (`timing.receiver=wrt`): Requires physical WREN card on the FEC. mkdev16 has
no WREN card — ruled out.

**LD_PRELOAD interposition**: Could theoretically intercept `socket.receive_from()` in
MockReceiver to read from shared memory instead of UDP. Extremely fragile, version-dependent,
unmaintainable. Not recommended.

**Custom AbstractEventSource (Path C)**: Modify the **application-level** KiTSGeneric FESA
class to use a Custom event source with shared-memory IPC. Does NOT require changes to
WREN_DU, LTIM, or fesa-core. See **Path C** section below for full details.

**Conclusion**: Two viable paths exist:
- **Path B (MockReceiver UDP)**: Zero FESA modification, ~10-30us latency
- **Path C (Custom EventSource + shared memory)**: Modifies KiTSGeneric only, ~5-10us latency (recommended)

---

## Implementation: Template Packet Approach

### Design

Pre-build one fixed-size packet per LTIM device at startup. At runtime, patch only the two 8-byte timestamps and call `sendto()`.

```cpp
struct EventPacketTemplate {
    std::array<char, 256> buf;     // pre-filled packet (stack)
    std::size_t len;               // actual packet length (~82B)
    std::size_t triggerTimeOff;    // byte offset of triggerTime field
    std::size_t validFromOff;      // byte offset of context.validFrom field
};
```

### Startup: Build Templates

For each known LTIM device (from ActionInfo map):
1. Map `(eventId, channel)` → LTIM device name string
2. Write the fixed bytes into `buf`:
   - `0x45` marker
   - `0xFFFFFFFF` (NO_DESCRIPTOR)
   - name length + name chars
   - placeholder triggerTime (will be patched)
   - CPS domain ID (1)
   - 0 event fields
   - placeholder validFrom (will be patched)
   - CPS domain ID (1)
   - 0 context fields
3. Record `triggerTimeOff` and `validFromOff`

### Hot Path: processEvent()

```cpp
void processEvent(const TimingEvent& ev) {
    if (ev.pktType != PKT_FIRE) return;  // only FIRE, never ADVANCE

    auto* tpl = lookupTemplate(ev.eventId, ev.channel);  // O(1)
    if (!tpl) return;

    // Convert sec/nsec → timing::Time (int64_t nanoseconds since epoch)
    std::int64_t ns = std::int64_t(ev.sec) * 1'000'000'000LL + ev.nsec;

    // Patch timestamps (only 2 memcpys!)
    std::memcpy(tpl->buf.data() + tpl->triggerTimeOff, &ns, 8);
    std::memcpy(tpl->buf.data() + tpl->validFromOff,   &ns, 8);

    // Send — no alloc, no serialize, no ostringstream
    sendto(m_udpFd, tpl->buf.data(), tpl->len, 0,
           &m_mcastAddr, sizeof(m_mcastAddr));
}
```

**2 memcpys + 1 syscall. Zero heap allocations.**

### Dependencies

- **No `timing-domain` dependency on the hot path** — raw bytes only
- CPS domain ID (1) can be hardcoded or discovered at startup via `Domain::of("CPS").getId()`
- UDP socket: plain POSIX `socket(AF_INET, SOCK_DGRAM, 0)`, pre-opened at startup
- No Boost.ASIO needed

### Library Availability (for optional startup validation)

`libtiming-domain.a` is available on all FECs:
```
/acc/local/deb12x64/fesa/fesa-core-cern/9.1.0/lib/libtiming-domain.a
  → symlink to /acc/local/deb12x64/timing/timing-wrt/timing-domain/2.0.1/lib/libtiming-domain.a

/acc/local/deb12x64/fesa/fesa-core-cern/9.1.0/include/timing-domain/
```

Static library, **zero boost dependencies**. Can optionally link for startup assertions
(e.g. `assert(Domain::of("CPS").getId() == 1)`).

### Proposed Architecture (Updated)

```
[mkdev30 - TX FEC]                     [mkdev16 - RX FEC]

WREN PCIe card                         FESA app (KiTSGeneric)
    ↓                                      ↑
WRENTransmitter                        MockReceiver::waitEvent()
    ↓ packet_mmap (0x88B5)                 ↑ UDP multicast 239.87.82.84:28020
    ↓                                      ↑
    ├───── Ethernet (eno2) ──────┤         ↑
                                 ↓         ↑
                          WRENReceiver (NIC poller, core 4)
                                 ↓
                          SPSCQueue<TimingEvent>
                                 ↓
                          EventProcessor (core 5)
                                 ↓
                          Template packet: patch 2x timestamps
                                 ↓
                          sendto() UDP to 239.87.82.84:28020
                                 ↑
                          MockReceiver::waitEvent() unblocks
                                 ↓
                          FESA RT actions fire normally
```

---

## Source File Locations

| Component | Path |
|-----------|------|
| FESA core (generic) | `/opt/public/FESA_core_repos/fesa-core/src/fesa-core/` |
| FESA core CERN | `/opt/public/FESA_core_repos/fesa-core-cern/src/fesa-core-cern/` |
| CERNControllerFactory | `.../Factory/CERNControllerFactory.{h,cpp}` |
| TimingEventSource | `.../RealTime/TimingEventSource.{h,cpp}` |
| TimingContext | `.../Synchronization/TimingContext.{h,cpp}` |
| timing-domain types | `/opt/public/FESA_core_repos/timing/timing-wrt/timing-domain/src/timing-domain/` |
| timing-domain-cern (CPS) | `/opt/public/FESA_core_repos/timing/timing-wrt/timing-domain-cern/src/timing-domain-cern/CpsDomain.cpp` |
| Receiver interface | `/opt/public/FESA_core_repos/timing/timing-wrt/timing-receiver/src/timing-receiver/Receiver.h` |
| ReceiverProvider | `/opt/public/FESA_core_repos/timing/timing-wrt/timing-receiver-cern/src/timing-receiver-cern/ReceiverProvider.{h,cpp}` |
| WrenReceiver | `/opt/public/FESA_core_repos/timing/timing-wrt/timing-receiver-wren/src/timing-receiver-wren/WrenReceiver.{h,cpp}` |
| MockReceiver | `/opt/public/FESA_core_repos/timing/timing-wrt/timing-receiver-mock/src/timing-receiver-mock/MockReceiver.{h,cpp}` |
| Broadcaster | `/opt/public/FESA_core_repos/timing/timing-wrt/timing-receiver-mock/src/timing-receiver-mock/Broadcaster.{h,cpp}` |
| Installed lib (NFS) | `/acc/local/deb12x64/fesa/fesa-core-cern/9.1.0/lib/libtiming-domain.a` |
| Installed headers | `/acc/local/deb12x64/fesa/fesa-core-cern/9.1.0/include/timing-domain/` |
| WREN_DU instance | `/opt/public/FESA_core_repos/WREN_LTIM/WREN_DU.cfc-865-mkdev30.xml` |
| KiTSGeneric instance | `/opt/public/FESA_core_repos/WREN_LTIM/DeviceData_KiTSGeneric.instance` |
| AcquisitionEventSource | `/opt/public/FESA_core_repos/WREN_LTIM/LTIM/src/LTIM/RealTime/AcquisitionEventSource.cpp` |

---

## Path C: Custom EventSource + Shared Memory IPC (Recommended)

### Concept: "Ethernet LTIMs"

Path C creates **virtual LTIM devices** ("Ethernet LTIMs") that appear in FESA exactly like
real hardware LTIMs, but receive their timing events from the packet_mmap Ethernet network
rather than from a local WREN PCIe card.

They use the naming convention **`ETH_`** prefix instead of `WREN_`:
```
Real LTIM:     MKDEV30.865.WRLTIM_0-1_TG-NC
Ethernet LTIM: ETH_MKDEV30.865_0-1_TG-NC
```

This creates a clean separation: `WREN_` devices need a WREN card, `ETH_` devices need only an
Ethernet connection to a WREN-equipped FEC.

### Why This Works

The key insight comes from `AcquisitionEventSource` in the LTIM FESA class (`AcquisitionEventSource.cpp:113-142`). It proves that a Custom event source can:

1. Implement `wait()` with **any blocking mechanism** — it blocks on `WrenReceiver`, but could block on anything
2. Construct `TimingContext` from **scalars**, not from `timing::Event`:
   ```cpp
   // What LTIM does (line 134):
   new fesaCERN::TimingContext(payload->cycleStampNs, cycleName);
   ```
3. The `TimingContext(timestamp_ns cycleStamp, const std::string& cycleName)` constructor takes:
   - `cycleStamp`: `int64_t` nanoseconds since epoch
   - `cycleName`: e.g. `"CPS.USER.ZERO"` — fully controls the multiplexing context

**No `timing::Event` needed. No `timing-domain` library needed. No serialization.**

### IPC Mechanism: Spin-Poll on Shared Memory (Lock-Free)

The same lock-free pattern used by `rigtorp::SPSCQueue` between our two threads extends
naturally across the process boundary via POSIX shared memory. The entire pipeline — NIC poll
→ SPSC queue → cross-process shared memory — is a chain of spin-polls on atomics with **zero
kernel involvement** after the initial packet_mmap receive.

```
[ABTWREN sidecar process]              [FESA KiTSGeneric process]

EventProcessor (core 5)                 WrenEventSource::wait()
    ↓                                       ↑
    ↓ write TimingEvent to shm slot        ↑ read TimingEvent from shm slot
    ↓ atomic store(seq+1, release)         ↑ spin: atomic load(acquire) until seq changes
    ↓                                       ↑
    └─── cache-line transfer (L3) ──────────┘
         ~50-100ns, zero syscalls
```

#### Shared Memory Layout

```cpp
// Mapped at /dev/shm/abtwren-events by both processes
struct alignas(64) ShmEventSlot {
    std::atomic<uint64_t> seq;   // 8B: producer increments after writing
    TimingEvent           event; // 16B: the payload
    // padding to 64B cache line
};
```

Cache-line aligned (64B) to prevent false sharing. The atomic sequence counter uses
`memory_order_release` on the producer side and `memory_order_acquire` on the consumer side —
the same acquire/release pattern that `rigtorp::SPSCQueue` uses internally. On ARM64 this
emits `stlr`/`ldar` barrier instructions; on x86-64 the release store is a plain `mov`
(x86 TSO provides release semantics for free) and the acquire load is also a plain `mov`.

#### Why This Works (Same Pattern as SPSC Queue)

The SPSC queue between our poller and processor threads already proves this pattern:
1. **Producer** writes data, then publishes via `atomic store(release)`
2. **Consumer** spins on `atomic load(acquire)` until new data appears
3. Single producer, single consumer — no CAS, no mutex, no kernel

Crossing the process boundary via `mmap(MAP_SHARED)` changes nothing about the memory model.
The atomic operations work identically whether the two threads share a virtual address space
(intra-process) or access the same physical pages via shared memory (inter-process). The CPU
cache coherency protocol (MESI/MOESI) handles the cache-line transfer regardless.

#### Core Allocation (mkdev16 — 6 cores)

| Core | Thread | Priority | Duty |
|------|--------|----------|------|
| 4 | WRENReceiver (NIC poller) | SCHED_FIFO:49 | Spin-poll packet_mmap ring |
| 5 | EventProcessor (producer) | SCHED_FIFO:48 | SPSC pop → shm write → atomic release |
| 3 | WrenEventSource (consumer) | SCHED_FIFO:47 | Spin-poll atomic → TimingContext → dispatch |
| 0-2 | FESA RT schedulers, OS | SCHED_OTHER | RT actions, watchdog, housekeeping |

Three cores dedicated to spin-polling. Feasible on 6-core mkdev16.

### Latency: ~2-4us Total

```
packet_mmap NIC poll (already spinning, ~0us wait)
    ↓ ~50ns      parseAndEnqueue → SPSCQueue (atomic store/load)
EventProcessor pops
    ↓ ~50ns      write TimingEvent to shm + atomic store(release)
Cache-line transfer (same socket, shared L3)
    ↓ ~50-100ns  WrenEventSource spin-poll sees new seq
    ↓ ~500ns     extract fields → TimingContext(cycleStampNs, cycleName)
postEventToSchedulers
    ↓ ~1-2us     RTScheduler queue → RT action fires
────────────
Total: ~2-4us
```

### Latency Comparison: All Paths

| Step | Real WREN | Path B (MockReceiver UDP) | Path C (spin-poll shm) |
|------|-----------|--------------------------|----------------------|
| Hardware → userspace | ~15us (IRQ + DMA) | ~1us (polling) | ~1us (polling) |
| Intra-process queue | N/A | ~50ns (SPSC) | ~50ns (SPSC) |
| IPC to FESA | N/A | ~3us (UDP loopback) | ~100ns (cache-line) |
| Deserialization | ~10us (WrenFields) | ~10us (MockReceiver) | ~500ns (scalar extract) |
| FESA dispatch | ~5us | ~5us | ~2us |
| **Total** | **~20-50us** | **~10-30us** | **~2-4us** |

Path C eliminates:
- All `timing-domain` serialization/deserialization overhead
- MockReceiver's `std::string` + `istringstream` + `shared_ptr` heap allocations
- UDP kernel stack processing
- `sleep_until()` check
- **All kernel syscalls on the IPC path** (no eventfd, no sendto, no recvfrom)

### Fallback: eventfd (if core budget is tight)

If dedicating 3 cores to spin-polling is too expensive, the IPC step can use `eventfd`
instead of spin-polling. This adds ~1-3us (kernel wake-up) but the consumer thread sleeps
between events rather than burning a core:

```cpp
// Producer: eventfd_write(m_evtFd, 1);        // ~200ns syscall
// Consumer: eventfd_read(m_evtFd, &val);       // blocks until signaled, ~1-3us wake-up
```

| IPC variant | Wake-up latency | Syscalls/event | CPU cost |
|-------------|----------------|----------------|----------|
| Spin-poll atomic | ~50-100ns | 0 | Burns 1 core |
| eventfd | ~1-3us | 2 (write + read) | Sleeps between events |

**Recommendation**: Start with spin-poll (3 cores available). Fall back to eventfd only if
the FESA process needs those CPU cycles for other RT work.

### WrenEventSource Implementation (in KiTSGeneric)

The new Custom event source for KiTSGeneric. Replaces the `Timing` event source for
Ethernet LTIM devices.

#### FESA Design (.design file change)

```xml
<!-- KiTSGeneric.design: add a Custom event source for Ethernet LTIMs -->
<ethNewCycle>
  <event-configuration name="ecEthNewCycle">
    <Custom>
      <WrenEventSource custom-event="defaultEvent"/>
    </Custom>
  </event-configuration>
</ethNewCycle>
```

This generates `WrenEventSourceBase` in the KiTSGeneric generated code. We implement
`WrenEventSource` with the custom `wait()` and `connect()`.

#### wait() Implementation (spin-poll)

```cpp
void WrenEventSource::wait(boost::shared_ptr<fesa::RTEvent>& eventToFire)
{
    // Spin-poll until producer publishes a new event (~50-100ns)
    // Same acquire/release pattern as rigtorp::SPSCQueue
    while (m_shmSlot->seq.load(std::memory_order_acquire) == m_lastSeq) {
        _mm_pause();  // x86 hint: avoid pipeline stall, save power
    }
    ++m_lastSeq;

    // Read 16-byte POD from shared memory (single copy, no deserialize)
    const TimingEvent& ev = m_shmSlot->event;

    // Map (eventId, channel) → device name + cycle info — O(1) flat array
    const auto& info = m_deviceMap[ev.channel];

    // Convert TAI timestamp
    int64_t cycleStampNs = int64_t(ev.sec) * 1'000'000'000LL + ev.nsec;
    std::string cycleName = "CPS.USER.ZERO";  // or from lookup

    // Construct TimingContext from scalars — no timing::Event needed
    boost::shared_ptr<fesa::MultiplexingContext> ctx(
        static_cast<fesa::MultiplexingContext*>(
            new fesaCERN::TimingContext(cycleStampNs, cycleName)));

    eventToFire->setName(info.concreteEventName);
    eventToFire->setMultiplexingContext(ctx);
}
```

This is directly modeled on `AcquisitionEventSource::wait()` (LTIM class, line 113-142),
which uses the same `TimingContext(cycleStamp, cycleName)` constructor. The spin-poll
replaces the WrenReceiver kernel blocking call with the same lock-free pattern used
throughout the pipeline.

#### connect() Implementation

```cpp
void WrenEventSource::connect(const boost::shared_ptr<fesa::EventElement>& eventElement)
{
    // Open shared memory (created by ABTWREN sidecar)
    int shmFd = ::shm_open("/abtwren-events", O_RDONLY, 0);
    m_shmSlot = static_cast<ShmEventSlot*>(
        ::mmap(nullptr, sizeof(ShmEventSlot), PROT_READ, MAP_SHARED, shmFd, 0));
    ::close(shmFd);  // mapping persists after close
    m_lastSeq = m_shmSlot->seq.load(std::memory_order_acquire);

    // Build device map from instance data
    // ... map channel → (deviceName, concreteEventName)
}
```

### Instance File: Ethernet LTIMs

Add Ethernet LTIM devices to `DeviceData_KiTSGeneric.instance`:

```xml
<!-- Existing real WREN LTIMs (triggered by Timing event source via WREN_DU) -->
<leNewCycle>
  <event-configuration name="ecNewCycle">
    <Timing>
      <hardware-event name="MKDEV30.865.WRLTIM_0-1_TG-NC"/>
    </Timing>
  </event-configuration>
</leNewCycle>

<!-- New Ethernet LTIMs (triggered by Custom WrenEventSource via packet_mmap) -->
<leEthNewCycle>
  <event-configuration name="ecEthNewCycle">
    <Custom>
      <WrenEventSource custom-event="ETH_MKDEV30.865_0-1_TG-NC"/>
    </Custom>
  </event-configuration>
</leEthNewCycle>
```

#### Naming Convention

| Type | Pattern | Example |
|------|---------|---------|
| Real WREN LTIM | `MKDEV30.865.WRLTIM_<slot>-<ch>_TG-<name>` | `MKDEV30.865.WRLTIM_0-1_TG-NC` |
| Ethernet LTIM | `ETH_MKDEV30.865_<slot>-<ch>_TG-<name>` | `ETH_MKDEV30.865_0-1_TG-NC` |

The `ETH_` prefix immediately identifies which LTIMs arrive over the Ethernet network vs
which come from a local WREN card.

### What Changes (and What Doesn't)

| Component | Changed? | Details |
|-----------|----------|---------|
| fesa-core | NO | Unchanged |
| fesa-core-cern | NO | Unchanged |
| WREN_DU (LTIM class) | NO | Unchanged |
| timing-receiver-* | NO | Unchanged |
| **KiTSGeneric.design** | YES | Add Custom event source `WrenEventSource` |
| **KiTSGeneric (user code)** | YES | Implement `WrenEventSource::wait()` and `connect()` |
| **DeviceData_KiTSGeneric.instance** | YES | Add `ETH_*` device entries |
| **ABTWREN sidecar** | YES | EventProcessor writes to shared memory + atomic release |

### Dependencies

- **No `timing-domain` library on the hot path** — only `fesaCERN::TimingContext(int64_t, string)` constructor
- **No Boost.ASIO** — plain POSIX `shm_open()` + `mmap()`
- **No UDP, no eventfd** — pure userspace spin-poll on atomic, zero syscalls
- **fesa-core-cern headers only** — for `TimingContext` class definition (already linked by KiTSGeneric)

### Architecture (Path C)

```
[mkdev30 - TX FEC]                     [mkdev16 - RX FEC]

WREN PCIe card                         FESA app (KiTSGeneric)
    ↓                                      ↑
WRENTransmitter                        WrenEventSource::wait()  [core 3, FIFO:47]
    ↓ packet_mmap (0x88B5)                 ↑ spin: atomic load(acquire) — ~50-100ns
    ↓                                      ↑ read TimingEvent from shm
    ├───── Ethernet (eno2) ──────┤         ↑
                                 ↓         ↑
                          WRENReceiver (NIC poller, core 4, FIFO:49)
                                 ↓ atomic store/load (~50ns)
                          SPSCQueue<TimingEvent>
                                 ↓ atomic store/load (~50ns)
                          EventProcessor (core 5, FIFO:48)
                                 ↓ atomic store(release) (~50ns)
                          ShmEventSlot → /dev/shm/abtwren-events
                                 ↑ cache-line transfer via L3
                          WrenEventSource spin-poll sees new seq
                                 ↓
                          TimingContext(cycleStampNs, cycleName)
                                 ↓
                          FESA RT actions fire (ETH_* devices)
```

**End-to-end lock-free pipeline**: Every hop in the chain uses the same acquire/release
atomic pattern. No mutexes, no kernel syscalls, no serialization. The 16-byte `TimingEvent`
POD struct flows from NIC hardware through three spin-poll hops to FESA RT actions in ~2-4us.

### Comparison: Path B vs Path C

| Aspect | Path B (MockReceiver UDP) | Path C (Custom EventSource) |
|--------|--------------------------|---------------------------|
| FESA code changes | None | KiTSGeneric only (design + user code) |
| Latency | ~10-30us | ~2-4us |
| Heap allocs per event | ~4 (MockReceiver side, unavoidable) | 1 (`new TimingContext`) |
| IPC mechanism | UDP loopback (~3us, 2 syscalls) | Spin-poll atomic (~100ns, 0 syscalls) |
| Kernel involvement | UDP stack + socket buffers | None (pure userspace) |
| Dependencies | timing-domain (serialization) | None (POSIX shm only) |
| TimingContext quality | Degraded (Option A) or full (Option B) | Full (scalar constructor) |
| Activation | `timing.simulation.mode=true` | Always-on Custom event source |
| CPU cost | ~0 (sleeps between events) | Burns 1 core (spin-poll) |
| Deployment | Zero FESA changes | KiTSGeneric rebuild required |

**Recommendation**: Path C is the better long-term solution. The ~5-10x lower latency, zero
kernel involvement on the hot path, and production-quality TimingContext make it the clear
winner. The only costs are modifying KiTSGeneric (which we can freely do) and dedicating one
additional core to spin-polling in the FESA process.
