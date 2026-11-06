/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file spdo_outputs.h
 * @brief SPDO (0x7F2, Speedometer / Instrument Cluster) output tests - table-driven (UDS 0x10, 0x27, 0x2F).
 *
 * Catalogue is spdo_outputs_data.h (see that file for exactly what this implements, and
 * the scope it deliberately does not cover). There is only one profile defined (see spdo_outputs_data.h),
 * so profile selection here exists only for UI symmetry with ECM/BCM/ABS.
 *
 * Test modes
 *   ONOFF  : ON request, the "off"/"stop" request clears the shared register and returns control
 *   ADJUST : 5 selectable steps (needle sweep position), control returned on stop
 *
 * Safety: every active output is returned to the SPDO module and the default session restored on exit, on
 * WebSocket loss and after an automatic time limit. Unlike ECM/BCM/ABS there is no engine-state interlock -
 * these are dash-cluster self-tests, not actuators with a mechanical safety concern.
 */
#pragma once
#include <Arduino.h>
#include "spdo_outputs_data.h"

class SpdoOutputs {
public:
    static void reset();
    static bool pageActive();
    static bool anyOutputActive();
    static bool enter(char *statusMessage, size_t statusMessageLength);
    static void returnAll(const char *reason);
    static void exitPage(const char *reason);
    static bool command(const char *testKey, const char *operationName, int value, char *statusMessage, size_t statusMessageLength);
    static bool setProfile(int profileIndex);                    // -1 = automatic (only one profile exists today)
    static void loop();
    static size_t catalogJson(char *outputBuffer, size_t maxOutputLength);
    static size_t statusJson(char *outputBuffer, size_t maxOutputLength);
    static bool takeStatusUpdate(char *outputBuffer, size_t maxOutputLength);
};
