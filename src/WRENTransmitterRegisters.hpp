//
// Created by asherjil on 2/15/26.
//

#ifndef ABTWREN_WRENREGISTERS_HPP
#define ABTWREN_WRENREGISTERS_HPP

#include <cstddef>
#include <cstdint>

// WREN PCI identification
constexpr std::uint16_t WREN_VENDOR_ID = 0x10DC; // CERN
constexpr std::uint16_t WREN_DEVICE_ID = 0x0455;
constexpr int WREN_BAR                 = 1;       // BAR1 = host registers + mailbox

// Host register offsets (BAR1 + 0x0000) — all safe to read
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

// Mailbox async ring buffer offsets (BAR1 + 0x10000 + ...)
constexpr std::size_t WREN_ASYNC_DATA_BASE  = 0x12000; // async_data[0..2047]
constexpr std::size_t WREN_ASYNC_BOARD_OFF  = 0x14000; // firmware write pointer
constexpr std::size_t WREN_ASYNC_HOST_OFF   = 0x14004; // driver read pointer — NEVER write

// --- Capsule header constants ---
constexpr std::uint32_t TYP_MASK    = 0x000000FF;
constexpr unsigned      LEN_SHIFT   = 16;
constexpr std::uint8_t  TYP_CONTEXT = 0x01;
constexpr std::uint8_t  TYP_EVENT   = 0x02;
constexpr std::uint8_t  TYP_CONFIG  = 0x03;
constexpr std::uint8_t  TYP_PULSE   = 0x04;
constexpr std::uint32_t RING_MASK   = 2047;

#endif // ABTWREN_WRENREGISTERS_HPP
