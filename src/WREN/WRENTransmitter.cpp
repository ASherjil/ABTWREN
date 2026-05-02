//
// WRENTransmitter — template implementation (included from WRENTransmitter.hpp)
//

// ── Constructor ────────────────────────────────────────────────────

template<TxRing Tx>
WRENTransmitter<Tx>::WRENTransmitter(std::uint16_t vendorID, std::uint16_t deviceID,
                                      int bar, Tx& ethernetSocket)
  : m_pcieHandler{vendorID, deviceID, bar}, m_ethernetSocket{ethernetSocket}
{
    m_pcieConnectionResult = m_pcieHandler.open();
    if (!m_pcieConnectionResult) {
        std::fprintf(stderr, "[WRENTransmitter] FATAL: PCIe open failed\n");
        return;
    }

    m_frameTemplate[12] = static_cast<std::uint8_t>(kEtherType >> 8);
    m_frameTemplate[13] = static_cast<std::uint8_t>(kEtherType & 0xFF);

    performRegisterDump();
}

// ── Action map installation ────────────────────────────────────────

template<TxRing Tx>
void WRENTransmitter<Tx>::installActionMap(const std::vector<ActionInfo>& map) {
    for (const auto& a : map) {
        if (a.actIdx < kMaxActIdx) {
            m_actMeta[a.actIdx].eventId  = a.eventId;
            m_actMeta[a.actIdx].channel  = a.channel;
            m_actMeta[a.actIdx].offsetMs = static_cast<std::uint16_t>(a.offsetNs / 1'000'000);
        }
    }
}

// ── MAC/ring setup ─────────────────────────────────────────────────

template<TxRing Tx>
void WRENTransmitter<Tx>::setMacAddresses(const std::array<std::uint8_t, 6>& src,
                                           const std::array<std::uint8_t, 6>& dst) {
    std::memcpy(&m_frameTemplate[0], dst.data(), kMacLength);
    std::memcpy(&m_frameTemplate[6], src.data(), kMacLength);
    m_ethernetSocket.prefillRing(m_frameTemplate);
}

// ── Ring buffer helpers ────────────────────────────────────────────

template<TxRing Tx>
inline std::uint32_t WRENTransmitter<Tx>::readRingWord(std::uint32_t idx) const {
    return *m_pcieHandler.registerPtr<std::uint32_t>(
        WREN_ASYNC_DATA_BASE + (idx & RING_MASK) * 4);
}

template<TxRing Tx>
inline std::pair<const LtimTarget*, int>
WRENTransmitter<Tx>::resolveCtimToSlots(std::uint16_t evId) {
    static std::array<LtimTarget, kTargets.size()> hits;
    int n = 0;
    for (const auto& t : kTargets)
        if (t.event_id == evId) hits[static_cast<std::size_t>(n++)] = t;
    return {hits.data(), n};
}

// ── Packet builders ────────────────────────────────────────────────

template<TxRing Tx>
inline void WRENTransmitter<Tx>::sendAdvance(std::uint16_t evId, std::uint16_t slot,
                                              std::uint32_t sec, std::uint32_t nsec) {
    auto* frame = m_ethernetSocket.acquire(kFrameSize);
    if (!frame) [[unlikely]] return;

    auto* p = frame + kEthHdrLen;
    p[0] = PKT_ADVANCE; p[1] = 0;
    std::memcpy(p + 2,  &evId, 2);
    std::memcpy(p + 4,  &slot, 2);
    std::memcpy(p + 6,  &sec,  4);
    std::memcpy(p + 10, &nsec, 4);

    m_ethernetSocket.commit();
}

template<TxRing Tx>
inline void WRENTransmitter<Tx>::sendFire(const CompEntry& info,
                                           std::uint32_t sec, std::uint32_t nsec) {
    auto* frame = m_ethernetSocket.acquire(kFrameSize);
    if (!frame) [[unlikely]] return;

    auto* p = frame + kEthHdrLen;
    p[0] = PKT_FIRE; p[1] = 0;
    std::memcpy(p + 2,  &info.eventId, 2);
    std::memcpy(p + 4,  &sec,  4);
    std::memcpy(p + 8,  &nsec, 4);
    p[12] = info.channel; p[13] = 0;
    std::memcpy(p + 14, &info.offsetMs, 2);

    m_ethernetSocket.commit();
}

// ── Register dump ──────────────────────────────────────────────────

template<TxRing Tx>
void WRENTransmitter<Tx>::performRegisterDump() const {
    constexpr int N = 7;
    auto t0 = std::chrono::steady_clock::now();

    std::uint64_t identAndMap   = *m_pcieHandler.registerPtr<std::uint64_t>(WREN_IDENT);
    std::uint64_t modelAndFw    = *m_pcieHandler.registerPtr<std::uint64_t>(WREN_MODEL_ID);
    std::uint64_t stateAndTaiLo = *m_pcieHandler.registerPtr<std::uint64_t>(WREN_WR_STATE);
    std::uint64_t taiHiAndCyc   = *m_pcieHandler.registerPtr<std::uint64_t>(WREN_TM_TAI_HI);
    std::uint32_t compact       = *m_pcieHandler.registerPtr<std::uint32_t>(WREN_TM_COMPACT);
    std::uint64_t isrAndRaw     = *m_pcieHandler.registerPtr<std::uint64_t>(WREN_ISR);
    std::uint32_t imr           = *m_pcieHandler.registerPtr<std::uint32_t>(WREN_IMR);
    auto t1 = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

    auto ident      = static_cast<std::uint32_t>(identAndMap);
    auto mapVersion = static_cast<std::uint32_t>(identAndMap >> 32);
    auto modelId    = static_cast<std::uint32_t>(modelAndFw);
    auto fwVersion  = static_cast<std::uint32_t>(modelAndFw >> 32);
    auto wrState    = static_cast<std::uint32_t>(stateAndTaiLo);
    auto taiLo      = static_cast<std::uint32_t>(stateAndTaiLo >> 32);
    auto taiHi      = static_cast<std::uint32_t>(taiHiAndCyc);
    auto cycles     = static_cast<std::uint32_t>(taiHiAndCyc >> 32);
    auto isr        = static_cast<std::uint32_t>(isrAndRaw);
    auto isrRaw     = static_cast<std::uint32_t>(isrAndRaw >> 32);

    char identStr[5];
    identStr[0] = static_cast<char>((ident >> 24) & 0xFF);
    identStr[1] = static_cast<char>((ident >> 16) & 0xFF);
    identStr[2] = static_cast<char>((ident >>  8) & 0xFF);
    identStr[3] = static_cast<char>((ident >>  0) & 0xFF);
    identStr[4] = '\0';

    std::printf("\n=== WREN Host Register Dump (BAR1, %d PCIe transactions) ===\n", N);
    std::printf("  ident:        0x%08X  (\"%s\")\n", ident, identStr);
    std::printf("  map_version:  0x%08X    model_id:     0x%08X\n", mapVersion, modelId);
    std::printf("  fw_version:   0x%08X    wr_state:     0x%08X  (link=%s, time=%s)\n",
                fwVersion, wrState,
                (wrState & 0x1) ? "UP" : "DOWN",
                (wrState & 0x2) ? "VALID" : "INVALID");
    std::printf("  tm_tai_lo:    0x%08X    tai_hi:       0x%08X\n", taiLo, taiHi);
    std::printf("  tm_cycles:    0x%08X    compact:      0x%08X\n", cycles, compact);
    std::printf("  isr:          0x%08X    isr_raw:      0x%08X    imr: 0x%08X\n",
                isr, isrRaw, imr);
    std::printf("  %ld ns total / %d = %ld ns avg\n\n", ns, N, ns / N);
}

// ── Main poll loop ─────────────────────────────────────────────────

template<TxRing Tx>
void WRENTransmitter<Tx>::transmitAll(const volatile std::sig_atomic_t& running) {
    m_shadowOff = *m_pcieHandler.registerPtr<std::uint32_t>(WREN_ASYNC_BOARD_OFF);
    std::uint32_t spins = 0;

    while (running) {
        std::uint32_t boardOff =
            *m_pcieHandler.registerPtr<std::uint32_t>(WREN_ASYNC_BOARD_OFF);

        if (boardOff == m_shadowOff) [[likely]] {
            if ((++spins & 0xFFFF) == 0 && !running) break;
            continue;
        }
        spins = 0;

        do {
            bool safe64 = (m_shadowOff + 3) <= RING_MASK;
            std::uint32_t hdr, w1;

            if (safe64) [[likely]] {
                std::uint64_t hdrW1 = *m_pcieHandler.registerPtr<std::uint64_t>(
                    WREN_ASYNC_DATA_BASE + m_shadowOff * 4);
                hdr = static_cast<std::uint32_t>(hdrW1);
                w1  = static_cast<std::uint32_t>(hdrW1 >> 32);
            } else {
                hdr = readRingWord(m_shadowOff);
                w1  = readRingWord(m_shadowOff + 1);
            }

            auto typ = static_cast<std::uint8_t>(hdr & TYP_MASK);
            auto len = static_cast<std::uint16_t>(hdr >> LEN_SHIFT);
            if (len == 0) [[unlikely]] { m_shadowOff = (m_shadowOff + 1) & RING_MASK; continue; }

            switch (typ) {
            case TYP_PULSE: {
                auto comp = static_cast<std::uint16_t>(w1);
                if (!m_compActive[comp]) break;
                std::uint32_t sec, nsec;
                if (safe64) [[likely]] {
                    std::uint64_t w2w3 = *m_pcieHandler.registerPtr<std::uint64_t>(
                        WREN_ASYNC_DATA_BASE + (m_shadowOff + 2) * 4);
                    sec = static_cast<std::uint32_t>(w2w3);
                    nsec = static_cast<std::uint32_t>(w2w3 >> 32);
                } else {
                    sec  = readRingWord(m_shadowOff + 2);
                    nsec = readRingWord(m_shadowOff + 3);
                }
                sendFire(m_compInfo[comp], sec, nsec);
                break;
            }
            case TYP_CONFIG: {
                const auto actIdx = static_cast<std::uint16_t>(w1);
                const auto swCmp  = static_cast<std::uint16_t>(w1 >> 16);
                m_compInfo[swCmp] = m_actMeta[actIdx];
                m_compActive[swCmp] = true;
                break;
            }
            case TYP_EVENT: {
                auto evId = static_cast<std::uint16_t>(w1);
                auto [targets, count] = resolveCtimToSlots(evId);
                if (count == 0) break;
                std::uint32_t nsec, sec;
                if (safe64) [[likely]] {
                    std::uint64_t w2w3 = *m_pcieHandler.registerPtr<std::uint64_t>(
                        WREN_ASYNC_DATA_BASE + (m_shadowOff + 2) * 4);
                    nsec = static_cast<std::uint32_t>(w2w3);
                    sec  = static_cast<std::uint32_t>(w2w3 >> 32);
                } else {
                    nsec = readRingWord(m_shadowOff + 2);
                    sec  = readRingWord(m_shadowOff + 3);
                }
                for (int i = 0; i < count; ++i)
                    sendAdvance(evId, targets[i].ltim_slot, sec, nsec);
                break;
            }
            case TYP_CONTEXT:
                break;
            default:
                break;
            }
            m_shadowOff = (m_shadowOff + len) & RING_MASK;
        } while (m_shadowOff != boardOff);
    }
    std::fprintf(stderr, "[TX] Exiting poll loop (transmitAll).\n");
}

// ── LTIM-only hot path ────────────────────────────────────────────

template<TxRing Tx>
inline void WRENTransmitter<Tx>::transmitLTIM(const volatile std::sig_atomic_t& running) {
    m_shadowOff = *m_pcieHandler.registerPtr<std::uint32_t>(WREN_ASYNC_BOARD_OFF);
    std::uint32_t spins = 0;

    while (running) {
        std::uint32_t boardOff =
            *m_pcieHandler.registerPtr<std::uint32_t>(WREN_ASYNC_BOARD_OFF);

        if (boardOff == m_shadowOff) [[likely]] {
            if ((++spins & 0xFFFF) == 0 && !running) break;
            continue;
        }
        spins = 0;

        do {
            bool safe64 = (m_shadowOff + 3) <= RING_MASK;
            std::uint32_t hdr, w1;

            if (safe64) [[likely]] {
                std::uint64_t hdrW1 = *m_pcieHandler.registerPtr<std::uint64_t>(
                    WREN_ASYNC_DATA_BASE + m_shadowOff * 4);
                hdr = static_cast<std::uint32_t>(hdrW1);
                w1  = static_cast<std::uint32_t>(hdrW1 >> 32);
            } else {
                hdr = readRingWord(m_shadowOff);
                w1  = readRingWord(m_shadowOff + 1);
            }

            auto typ = static_cast<std::uint8_t>(hdr & TYP_MASK);
            auto len = static_cast<std::uint16_t>(hdr >> LEN_SHIFT);
            if (len == 0) [[unlikely]] { m_shadowOff = (m_shadowOff + 1) & RING_MASK; continue; }

            if (typ == TYP_PULSE) {
                auto comp = static_cast<std::uint16_t>(w1);
                if (m_compActive[comp]) [[likely]] {
                    std::uint32_t sec, nsec;
                    if (safe64) [[likely]] {
                        std::uint64_t w2w3 = *m_pcieHandler.registerPtr<std::uint64_t>(
                            WREN_ASYNC_DATA_BASE + (m_shadowOff + 2) * 4);
                        sec = static_cast<std::uint32_t>(w2w3);
                        nsec = static_cast<std::uint32_t>(w2w3 >> 32);
                    } else {
                        sec  = readRingWord(m_shadowOff + 2);
                        nsec = readRingWord(m_shadowOff + 3);
                    }
                    sendFire(m_compInfo[comp], sec, nsec);
                }
            }
            else if (typ == TYP_CONFIG) {
                const auto actIdx = static_cast<std::uint16_t>(w1);
                const auto swCmp  = static_cast<std::uint16_t>(w1 >> 16);
                m_compInfo[swCmp] = m_actMeta[actIdx];
                m_compActive[swCmp] = true;
                // No TX packet — metadata setup only
            }
            // Drop everything else (EVENT, CONTEXT, unknown) — LTIM hot path

            m_shadowOff = (m_shadowOff + len) & RING_MASK;
        } while (m_shadowOff != boardOff);
    }
    std::fprintf(stderr, "[TX] Exiting poll loop (transmitLTIM).\n");
}
