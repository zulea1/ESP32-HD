/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file lhc_special.h
 * @brief LHC (0x7E3, Left Hand Control) special functions - the page that owns the one UI function this ECU
 *        has, "Cruise / Push-to-Talk side".
 *
 * That function is a single two-value config option that writes DID F1E8 to BOTH hand controls (LHC 0x7E3 +
 * RHC 0x7E5) in step, so it doesn't fit the single-ECU CfgSpecialEngine model (cfg_special.h) used by
 * SPDO/RAD. This module owns LHC's own session/unlock/read/write of that DID; the RHC half of each write is
 * delegated to RhcSpecial (src/modules/rhc/rhc_special.h/.cpp), its own self-contained module. There is no
 * separate /api/rhc/special page - the UI card stays under /api/lhc/special.
 *
 * Security Access: key = seed XOR 0xFFFF, confirmed on real hardware 2026-10-31 (seed=062D -> key=F9D2
 * unlocked LHC) - same algorithm used for both LHC and RHC. No F1F4 flag read (this function has no
 * activation flag). This replaced an earlier attempt that reused the ECM's table-lookup algorithm
 * (ecm_algo.cpp) - that one was consistently rejected with NRC 0x35 "invalid key" on this ECU, so it was
 * removed rather than kept as a first try.
 *
 * Every write is read-check-change-write, done fresh each time (never a cached length, never a hardcoded
 * length): read F1E8, require at least 1 byte back, set the 1st byte (index 0, the low-bit selector) to the
 * requested value, write the whole block straight back at the same length it was just read with. LHC does
 * this against its own F1E8; RHC does the exact same thing completely independently against its own F1E8
 * (RhcSpecial) - the two ECUs' DID lengths are never assumed to match, even though both measure 8 bytes on
 * this hardware.
 *
 * The selector is byte[0], NOT byte[5] like a widely-assumed 6-byte "FF 60 00 00 00 49/4A" template
 * assumed: real hardware 2026-10-31 showed F1E8 = 01 00 00 00 00 00 00 00 while the grip was physically
 * Cruise-ON-RIGHT, and byte[0]'s low bit (1) is what matches the "low bit set = Cruise RIGHT / PTT
 * LEFT" convention (byte[5] was 00 there, which would have read as the wrong side). A 1-byte selector-only
 * write and a hardcoded 6-byte write were both tried on real hardware and both got NRC 0x13 "incorrect
 * message length".
 */
#pragma once
#include <Arduino.h>
#include <stddef.h>

class LhcSpecial {
public:
    static void   reset();
    static bool   pageActive();
    static bool   enter(char *statusMessage, size_t statusMessageLength);
    static void   exitPage(const char *reason);
    static bool   read(const char *testKey, char *outputBuffer, size_t outputBufferLength);
    static bool   run(const char *testKey, int option, bool confirmed, char *outputBuffer, size_t outputBufferLength);
    static bool   restore(const char *testKey, char *outputBuffer, size_t outputBufferLength);
    static size_t catalogJson(char *outputBuffer, size_t maxOutputLength);
};
