/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file ecm_outputs.h
 * @brief ECM (0x7E0) output / actuator tests - table-driven engine (UDS 0x10, 0x27, 0x2F, 0x31).
 *
 * The test catalogue (commands, modes, step values) is generated into
 * ecm_outputs_data.h. The profile (which tests the ECU supports) is chosen from the ECM calibration ID.
 *
 * Test modes
 *   PULSE  : start request, output stays active for delay_s, then control is returned to the ECU
 *   ONOFF  : ON request / OFF request, control returned to the ECU on stop
 *   ADJUST : arm + 5 selectable steps (engine must be running), control returned on stop
 *
 * Safety: engine-off tests are blocked/stopped when the engine runs; every active output is returned to the
 * ECU and the default session restored on exit, on WebSocket loss and on the automatic time limits.
 */
#pragma once
#include <Arduino.h>
#include "ecm_outputs_data.h"

class EcmOutputs {
public:
    static void reset();                                   // forget state (module change / boot)
    static bool pageActive();
    static bool anyOutputActive();

    static bool enter(char *statusMessage, size_t statusMessageLength);           // open the outputs page, identify the profile
    static void returnAll(const char *reason);              // same as exitPage but the page stays open
    static void exitPage(const char *reason);               // return control of everything + default session
    static bool command(const char *testKey, const char *operationName, int value, bool neutralOk, char *statusMessage, size_t statusMessageLength);
    static bool setProfile(int profileIndex);                    // -1 = automatic (by calibration ID)

    static void loop();                                     // call from the web loop (keep-alive, timers, interlock)
    static size_t catalogJson(char *outputBuffer, size_t maxOutputLength);
    static size_t statusJson(char *outputBuffer, size_t maxOutputLength);
    static bool takeStatusUpdate(char *outputBuffer, size_t maxOutputLength);// true if a status frame was produced (changed or 1 s refresh)
};
