/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file spdo_special.h
 * @brief SPDO (0x7F2) special functions - configuration options. Thin static wrapper around one
 *        CfgSpecialEngine instance (cfg_special.h); the catalogue is spdo_special_data.h.
 */
#pragma once
#include <Arduino.h>
#include "cfg_special.h"

class SpdoSpecial {
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

    // Standalone VIN read/write (DID resolved by EcuInfo::vinDid(), same as Read ECU Information - F1EF/F190
    // depending on calibration) and the identity-block ("0157"/"0158") read used to capture a replacement
    // cluster's pairing data without an external diagnostic tool. Same pattern as EcmSpecial::readVin()/
    // writeVin() (ecm_special.cpp), routed through the shared CfgSpecialEngine.
    static bool readVin(char *outputBuffer, size_t outputBufferLength);
    static bool writeVin(const char *vin, char *outputBuffer, size_t outputBufferLength);
    static bool readIdBlocks(char *outputBuffer, size_t outputBufferLength);
    // Writes both identity/config blocks back (plain hex text, as produced by readIdBlocks()/the saved file -
    // 24 hex digits each), for pairing a replacement cluster from the same card that reads and saves them.
    static bool writeIdBlocks(const char *hexBlock157, const char *hexBlock158, char *outputBuffer, size_t outputBufferLength);
};
