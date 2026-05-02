//
// Created by asherjil on 2/15/26.
//

#ifndef ABTWREN_WRENPROTOCOL_HPP
#define ABTWREN_WRENPROTOCOL_HPP

#include <cstddef>
#include <cstdint>

// ── WREN PCI identification (CAPS = hardware/register constants) ─────────
constexpr std::uint16_t WREN_VENDOR_ID = 0x10DC; // CERN
constexpr std::uint16_t WREN_DEVICE_ID = 0x0455;
constexpr int WREN_BAR                 = 1;       // BAR1 = host registers + mailbox

// ── Host register offsets (BAR1 + 0x0000) — all safe to read ─────────────
constexpr std::size_t WREN_IDENT          = 0x00; // "WREN" = 0x5745524E
constexpr std::size_t WREN_MAP_VERSION    = 0x04;
constexpr std::size_t WREN_MODEL_ID       = 0x08;
constexpr std::size_t WREN_FW_VERSION     = 0x0C;
constexpr std::size_t WREN_WR_STATE       = 0x10; // bit 0: link_up, bit 1: time_valid
constexpr std::size_t WREN_TM_TAI_LO      = 0x14; // Current TAI seconds (low 32b)
constexpr std::size_t WREN_TM_TAI_HI      = 0x18; // Current TAI seconds (high bits)
constexpr std::size_t WREN_TM_CYCLES      = 0x1C; // 28-bit cycle counter @ 62.5MHz (16ns/tick)
constexpr std::size_t WREN_TM_COMPACT     = 0x20; // Compact time: cycles[27:0] + tai_sec[31:28]
constexpr std::size_t WREN_ISR            = 0x28; // Interrupt status (masked)
constexpr std::size_t WREN_ISR_RAW        = 0x2C; // Raw interrupt status (read-only, safe)
constexpr std::size_t WREN_IMR            = 0x30; // Interrupt mask register
// WARNING: 0x34 = IACK (write-only) — DO NOT read or write

// ── Mailbox async ring buffer offsets (BAR1 + 0x10000 + ...) ─────────────
constexpr std::size_t WREN_ASYNC_DATA_BASE  = 0x12000; // async_data[0..2047]
constexpr std::size_t WREN_ASYNC_BOARD_OFF  = 0x14000; // firmware write pointer
constexpr std::size_t WREN_ASYNC_HOST_OFF   = 0x14004; // driver read pointer — NEVER write

// ── Capsule header constants (from firmware wren-mb-defs.h) ──────────────
constexpr std::uint32_t TYP_MASK    = 0x000000FF;
constexpr unsigned      LEN_SHIFT   = 16;
constexpr std::uint8_t  TYP_CONTEXT = 0x01;
constexpr std::uint8_t  TYP_EVENT   = 0x02;
constexpr std::uint8_t  TYP_CONFIG  = 0x03;
constexpr std::uint8_t  TYP_PULSE   = 0x04;
constexpr std::uint32_t RING_MASK   = 2047;

// ── Sidecar wire protocol (shared between TX and RX) ─────────────────────
//
// Ethernet frame: [dst:6][src:6][ethertype:2][payload] = 64 bytes minimum
// Payload layouts (at byte 14):
//   ADVANCE: [type:1][0:1][evId:2][slot:2][sec:4][nsec:4]                 (14 bytes)
//   FIRE:    [type:1][0:1][evId:2][sec:4][nsec:4][ch:1][0:1][offsetMs:2] (16 bytes)
//            ch=0xFF + offsetMs=0xFFFF → CTIM fire (no physical output)
constexpr std::size_t   kMacLength  = 6;
constexpr std::size_t   kEthHdrLen  = 14;    // dst(6) + src(6) + ethertype(2)
constexpr std::uint32_t kFrameSize  = 64;    // Minimum Ethernet frame — always send full size
constexpr std::uint16_t kEtherType  = 0x88B5; // IEEE local experimental

enum PktType : std::uint8_t { PKT_ADVANCE = 1, PKT_FIRE = 2 };

// EventProcessor creates the shared memory region for IPC.
// shm_open() names must start with '/' but NOT be a full path —
// the kernel maps "/abtwren_events" → "/dev/shm/abtwren_events" internally.
constexpr const char* kShmName = "/abtwren_events";

#endif // ABTWREN_WRENPROTOCOL_HPP
