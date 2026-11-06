/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file abs_outputs.h
 * @brief ABS (0x7E1) output tests - table-driven (UDS 0x10, 0x2F/0x31).
 *
 * Catalogue generated into abs_outputs_data.h. Which tests the ABS module supports is chosen
 * from its Calibration ID (DID F1ED / F193), exactly like the ECM output tests choose their profile.
 *
 *   ONOFF : InputOutputControlByIdentifier sets the solenoid, the end commands return control to the ABS
 *   PULSE : start request, output active for delay_s, then the return-control request
 *
 * Safety: every solenoid/pump test in the database requires the engine to be off (ActEnterExpress present),
 * so all outputs here are refused unless the ECM reports < 150 rpm and are stopped if the engine starts;
 * every active output is returned to the ABS and the default session restored on exit, on WebSocket loss
 * and after an automatic time limit.
 */
#pragma once
#include <Arduino.h>
#include "abs_outputs_data.h"

class AbsOutputs {
public:
    static void reset();
    static bool pageActive();
    static bool anyOutputActive();
    static bool enter(char *statusMessage, size_t statusMessageLength);
    static void returnAll(const char *reason);
    static void exitPage(const char *reason);
    static bool command(const char *testKey, const char *operationName, char *statusMessage, size_t statusMessageLength);
    static bool setProfile(int profileIndex);                    // -1 = automatic (by Calibration ID)
    static void loop();
    static size_t catalogJson(char *outputBuffer, size_t maxOutputLength);
    static size_t statusJson(char *outputBuffer, size_t maxOutputLength);
    static bool takeStatusUpdate(char *outputBuffer, size_t maxOutputLength);
};
