/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file ecm_special.h
 * @brief ECM (0x7E0) special functions - table-driven.
 *
 * The function list is chosen by the ECM calibration ID (ecm_special_data.h, generated).
 *   IDLE    : idle speed, read 22 01 01, write 2E 01 01 xx (engine running)
 *   CFGBIT  : enable/disable bits of the configuration block F1E8 (read, change one bit, write the whole block,
 *             read back and verify). The original block is kept so it can be restored.
 *   RESET   : reset air-fuel values (AFV) - 2F input/output control, returned to the ECU after the DB delay
 *   PAIR    : security module pairing (password learn) - functional (0x7DF) clear DTC + extended session sequence
 *   SPEEDO  : speedometer calibration value (22 01 00 / 2E 01 00 hh ll)
 */
#pragma once
#include <Arduino.h>
#include "ecm_special_data.h"

class EcmSpecial {
public:
    static void reset();
    static bool pageActive();
    static bool enter(char *statusMessage, size_t statusMessageLength);          // identify calibration/profile
    static void exitPage(const char *reason);              // default session, forget security access
    static bool setProfile(int profileIndex);                   // -1 = automatic
    static bool read(const char *testKey, char *outputBuffer, size_t outputBufferLength);          // JSON result with the current value
    static bool run(const char *testKey, int value, char *outputBuffer, size_t outputBufferLength);// execute (value: 0/1, rpm, raw speedo value)
    static bool restore(const char *testKey, char *outputBuffer, size_t outputBufferLength);       // write back the saved configuration block
    static void loop();                                    // keep-alive and timed completion
    static size_t catalogJson(char *outputBuffer, size_t maxOutputLength);
    static size_t statusJson(char *outputBuffer, size_t maxOutputLength);

    // Standalone actions, always offered regardless of calibration profile (ex-guided-procedure versions of
    // these same three operations: TPS reset, clear adaptations, VIN write). One-shot, no "current value".
    static bool runTpsReset(char *outputBuffer, size_t outputBufferLength);
    static bool runClearAdaptations(char *outputBuffer, size_t outputBufferLength);
    static bool readVin(char *outputBuffer, size_t outputBufferLength);
    static bool writeVin(const char *vin, char *outputBuffer, size_t outputBufferLength);
};
