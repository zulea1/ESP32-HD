/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file ecu_info.h
 * @brief "Read ECU Information" driven by a generated table (ecu_info_data.h).
 *
 * Each module has one or more INFO item lists; the list is chosen from the module's identification string
 * (Calibration ID, or Parts Number for BCM/SCM/LHC/RHC) by longest-prefix match.
 * Every item is one ReadDataByIdentifier (0x22) with its own decode rule (ASCII, hex, date, scaled number, bit text, value text).
 */
#pragma once
#include <Arduino.h>

class EcuInfo {
public:
    static bool plan(uint32_t moduleId, char *outputJsonBuffer, size_t outputBufferLength);                 // selects the list, returns the item labels (JSON)
    static bool vinJson(uint32_t moduleId, char *outputJsonBuffer, size_t outputBufferLength);              // reads the VIN (module, else ECM) and decodes it (JSON)
    static bool item(uint32_t moduleId, uint8_t itemIndex, char *outputJsonBuffer, size_t outputBufferLength);  // reads + decodes one item (JSON)

    // Resolves the DID that holds "VIN" for this module in the Read ECU Information table (ecu_info_data.h) -
    // e.g. 0xF1EF on most modules, 0xF190 on the calibration families that use that DID instead. Re-identifies
    // the module fresh (does not disturb plan()'s own activeModule/activeInfoSet), falls back to scanning every
    // set for this module if the matched set has no VIN item, and finally to 0xF1EF if the module defines none.
    static uint16_t vinDid(uint32_t moduleId);
};
