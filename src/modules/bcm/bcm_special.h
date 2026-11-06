/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file bcm_special.h
 * @brief BCM (0x7E2) special functions - configuration options.
 *
 * Every function is one option inside a configuration DID (22 01 xx / 2E 01 xx): the first data byte holds a bit field,
 * the function owns the bits in `mask`. Changing an option reads the byte, replaces only those bits, writes it back
 * (security access when the BCM asks for it), reads it again to verify and puts the old byte back if the check fails.
 * The original byte is remembered per function so it can be restored.
 */
#pragma once
#include <Arduino.h>
#include "bcm_special_data.h"

// Display label for the BCM Boost function - the ONLY place its name is defined. The firmware messages
// (bcm_special.cpp, by literal concatenation: BCM_BOOST_LABEL ": ...") and the web UI (sent in the BCM
// special-function catalog as "boost_label") both take it from here, so changing it renames the feature
// everywhere.
#define BCM_BOOST_LABEL "Anti-theft PIN"

class BcmSpecial {
public:
    static void reset();
    static bool pageActive();
    static bool enter(char *statusMessage, size_t statusMessageLength);
    static void exitPage(const char *reason);
    static bool read(const char *testKey, char *outputBuffer, size_t outputBufferLength);
    static bool run(const char *testKey, int option, bool confirmed, char *outputBuffer, size_t outputBufferLength);
    static bool restore(const char *testKey, char *outputBuffer, size_t outputBufferLength);
    // Boost: read / apply / restore, same read-modify-verify-restore pattern as the config options.
    static bool readBoost(char *outputBuffer, size_t outputBufferLength);
    static bool writeBoost(const char *digitsText, char *outputBuffer, size_t outputBufferLength);
    static bool restoreBoost(char *outputBuffer, size_t outputBufferLength);
    static bool openSecured();  // ensureSession() + unlock()
    // Seed -> key lookup for UDS security access (0x27). No seed/key pairs are stored in the firmware; this
    // forwards to the module's own algorithm in bcm_algo.cpp, which returns a key only when the real BCM
    // routine has been recovered for that seed, and otherwise the attempt fails cleanly.
    static bool bcmComputeSecurityKey(const uint8_t *seedBytes, uint8_t seedLength, uint8_t *outputKeyBytes, uint8_t *outputKeyLength);

    // TPMS (menus 11/12 of the special-function set). Menu 11's write encoding talks
    // to two other module addresses (0x7F2/0x7FA) with an undocumented bit layout, so it is read-only here;
    // menu 12 is a plain 4-byte-ID read/write on the BCM itself and is fully implemented.
    static bool tpmsConfigRead(char *outputBuffer, size_t outputBufferLength);                                            // menu 11, status byte only
    static bool tpmsSensorRead(char *outputBuffer, size_t outputBufferLength);                                             // menu 12, read both IDs
    static bool tpmsSensorWrite(const char *frontHex, const char *rearHex, bool confirmed, char *outputBuffer, size_t outputBufferLength); // menu 12, write both IDs

    // No VIN read/write here: the BCM has no VIN DID at all (confirmed 2026-10-31) - unlike ABS/SPDO/RAD/ECM,
    // which each have a genuine VIN item in Read ECU Information. A standalone readVin()/writeVin() pair used
    // to exist against a hardcoded DID 0xF1EF guess; it was removed along with the UI card for it.

    static void loop();
    static size_t catalogJson(char *outputBuffer, size_t maxOutputLength);
};
