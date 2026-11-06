/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file rad_special.h
 * @brief RAD (0x7F1) special functions - configuration options. Thin static wrapper around one
 *        CfgSpecialEngine instance (cfg_special.h); the catalogue is rad_special_data.h.
 */
#pragma once
#include <Arduino.h>
#include "cfg_special.h"

class RadSpecial {
public:
    static void reset();
    static bool pageActive();
    static bool enter(char *statusMessage, size_t statusMessageLength);
    static void exitPage(const char *reason);
    static bool read(const char *testKey, char *outputBuffer, size_t outputBufferLength);
    static bool run(const char *testKey, int option, bool confirmed, char *outputBuffer, size_t outputBufferLength);
    static bool restore(const char *testKey, char *outputBuffer, size_t outputBufferLength);
    static bool setProfile(int profileIndex);
    static void loop();
    static size_t catalogJson(char *outputBuffer, size_t maxOutputLength);

    // Standalone VIN read/write (DID resolved by EcuInfo::vinDid(), same as Read ECU Information - 0xF1EF).
    // Same pattern as EcmSpecial::readVin()/writeVin() (ecm_special.cpp), routed through the shared
    // CfgSpecialEngine; the write still needs the radio's captured seed/key table (rad_special.cpp) to unlock.
    static bool readVin(char *outputBuffer, size_t outputBufferLength);
    static bool writeVin(const char *vin, char *outputBuffer, size_t outputBufferLength);
};
