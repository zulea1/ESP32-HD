/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file abs_special.h
 * @brief ABS (0x7E1) special functions - one special-functions table per calibration family.
 *
 * "Perform bleeding and configuration of ABS module" - a guided front+rear brake-bleed / air-purge sequence
 * (enter extended session, then numbered steps mixing on-screen instructions for the technician with real 2F
 * InputOutputControl pump/solenoid commands and live 0x0200 reads).
 *
 * It does NOT require a Security Access unlock. The procedure sends 27 01 (request seed) exactly once to
 * wake/check the module and never sends a 27 02 key; the module answers 27 01 with an all-zero seed. The
 * pump/solenoid actuations are plain 2F
 * and run after a 10 03 session, not behind an unlock. The procedure is driven by abs_proc.cpp as an
 * interactive, step-at-a-time runner (start -> step(answer) -> ... -> done) that sends the real commands with
 * live brake-pressure telemetry and a wheel-speed interlock.
 */
#pragma once
#include <Arduino.h>

class AbsSpecial {
public:
    static void reset();
    static bool pageActive();
    static bool enter(char *statusMessage, size_t statusMessageLength);
    static void exitPage(const char *reason);
    static bool run(const char *testKey, int value, bool confirmed, char *outputBuffer, size_t outputBufferLength);
    static void loop();
    static size_t catalogJson(char *outputBuffer, size_t maxOutputLength);

    // Standalone VIN read/write (DID resolved by EcuInfo::vinDid(), same as Read ECU Information - 0xF1EF).
    // Same pattern as EcmSpecial::readVin()/writeVin() (ecm_special.cpp): extended session, Security Access
    // unlock with the ABS algorithm (algorithm family 0x03, abs_algo.cpp), write, verify, default session.
    static bool readVin(char *outputBuffer, size_t outputBufferLength);
    static bool writeVin(const char *vin, char *outputBuffer, size_t outputBufferLength);
};
