/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file bcm_outputs.h
 * @brief BCM (0x7E2) output tests (UDS 0x10, 0x2F).
 *
 * Catalogue generated into bcm_outputs_data.h. Which tests the BCM supports is chosen from its Parts Number
 * (DID F1EA).
 *
 *   ONOFF : 2F <DID> <value> <mask> sets the output, the "off" request clears it, 2F <DID> 00 00 returns control to the BCM
 *   PULSE : start request, output active for delay_s, then the return-control request (Alarm)
 *
 * Safety: tests marked engine_off (fuel pump, coolant pump, alarm) are refused unless the ECM reports < 150 rpm and are
 * stopped if the engine starts; every active output is returned to the BCM and the default session restored on exit,
 * on WebSocket loss and after an automatic time limit.
 */
#pragma once
#include <Arduino.h>
#include "bcm_outputs_data.h"

class BcmOutputs {
public:
    static void reset();
    static bool pageActive();
    static bool anyOutputActive();
    static bool enter(char *statusMessage, size_t statusMessageLength);
    static void returnAll(const char *reason);
    static void exitPage(const char *reason);
    static bool command(const char *testKey, const char *operationName, char *statusMessage, size_t statusMessageLength);
    static bool setProfile(int profileIndex);                    // -1 = automatic (by Parts Number)
    static void loop();
    static size_t catalogJson(char *outputBuffer, size_t maxOutputLength);
    static size_t statusJson(char *outputBuffer, size_t maxOutputLength);
    static bool takeStatusUpdate(char *outputBuffer, size_t maxOutputLength);
};
