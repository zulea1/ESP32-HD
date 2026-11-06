/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file rhc_special.h
 * @brief RHC (0x7E5) DID F1E8 session/unlock/write primitives, called into by LhcSpecial (lhc_special.cpp) as
 *        the RHC half of the shared "Cruise / Push-to-Talk side" write (see lhc_special.h for the full story).
 *        RHC has no special-function UI page of its own - only this module, restored as its own file 2026-10-31
 *        (it was briefly inlined into lhc_special.cpp, then split back out).
 *
 * Security Access: key = seed XOR 0xFFFF, the same algorithm confirmed on LHC on real hardware 2026-10-31
 * (seed=062D -> key=F9D2 unlocked LHC) - no F1F4 flag read.
 *
 * The write is read-check-change-write, done fully independently of LHC against RHC's own F1E8 (never
 * assumes RHC's DID is the same length OR scheme as LHC's, even though both are currently the newer
 * illuminated part): read F1E8 fresh, set the selector byte to the value for the requested logical option
 * (0=left,1=right), write the whole block straight back. BOTH the byte position and the byte value depend on
 * RHC's own Hardware Part Number (DID 0xF1EA, first 8 digits only, any trailing revision letter ignored) -
 * see lhc_special.cpp's header comment for the full part-number table:
 *   71500129... -> byte[5], value 0x4A (left) / 0x49 (right)  - original part, widely-assumed scheme
 *   71500426... -> byte[0], value 0x00 (left) / 0x01 (right)  - newer illuminated part, confirmed-working
 * unrecognized part numbers fall back to the byte[0]/0x00/0x01 scheme. A hardcoded 1-byte write and a
 * hardcoded 6-byte write were both tried on real hardware 2026-10-31 and both got NRC 0x13 "incorrect message
 * length".
 */
#pragma once
#include <Arduino.h>
#include <stddef.h>

class RhcSpecial {
public:
    // Reads RHC's own F1E8, sets its selector byte (position AND value both picked from RHC's own part
    // number) for the given logical option (0=left,1=right), and writes it straight back, after its own
    // session/unlock. Atomic: opens the extended session, unlocks, reads, writes, closes the session, all in
    // one call. statusMessage carries a short OK text, or the step-by-step trace up to and including the
    // failing step's NRC/reason.
    static bool writeF1E8(int option, char *statusMessage, size_t statusMessageLength);

    // Read-only: opens RHC's session, reads F1E8 fresh, closes the session, and formats the raw bytes as
    // space-separated hex into rawHexOut (e.g. "01 00 00 00 00 00 00 00 "), for display/diagnostic purposes
    // only - no Security Access needed since this never writes. Returns false (rawHexOut gets "read failed")
    // on no/short reply.
    static bool readF1E8ForDisplay(char *rawHexOut, size_t rawHexOutLength);
};
