/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file rad_special.cpp
 * @brief RAD (0x7F1) special functions - module description + static wrapper (engine: cfg_special.cpp).
 */
#include "rad_special.h"
#include "rad_special_data.h"
#include "config.h"
#include "ecu_info.h"
#include <stdio.h>

// RADIO Security Access. Writes to the radio configuration DIDs are rejected (NRC 0x33) unless the module is
// first unlocked with 0x27, so the engine ALWAYS unlocks the radio up front (RAD_SPC_SPEC.unlockUpFront = true).
//
// No lookup table: the engine reads DID 0xFDBD right before sending 27 01 (CfgModuleSpec.preUnlockDid below)
// and hands that response to radComputeSecurityKey() as preUnlockBytes. The seed returned by 27 01 is checked
// against the FIRST TWO bytes of that 0xFDBD read; if they match, the key sent back with 27 02 is the THIRD
// and FOURTH bytes of the same 0xFDBD read. A mismatch (or a 0xFDBD read shorter than 4 bytes) fails the
// unlock, and this function fills outMsg with the specific reason - including both values being compared -
// so the web UI shows exactly why, instead of the engine's generic "no known key" message.
static bool radComputeSecurityKey(const uint8_t *seed, uint8_t seedLen, uint8_t *key, uint8_t *keyLen,
                                   const uint8_t *preUnlockBytes, uint8_t preUnlockLength,
                                   char *outMsg, size_t outMsgLen) {
    *keyLen = 0;
    if (seedLen < 2 || preUnlockLength < 4) {
        if (outMsg && outMsgLen) {
            snprintf(outMsg, outMsgLen, "RAD unlock failed: DID 0xFDBD returned only %u byte(s) (need at least 4)", (unsigned)preUnlockLength);
        }
        return false;
    }
    if (seed[0] != preUnlockBytes[0] || seed[1] != preUnlockBytes[1]) {
        if (outMsg && outMsgLen) {
            snprintf(outMsg, outMsgLen, "RAD unlock failed: seed %02X%02X from 27 01 does not match DID 0xFDBD bytes 1-2 (%02X%02X)",
                      seed[0], seed[1], preUnlockBytes[0], preUnlockBytes[1]);
        }
        return false;
    }
    key[0] = preUnlockBytes[2];   // FDBD byte 3
    key[1] = preUnlockBytes[3];   // FDBD byte 4
    *keyLen = 2;
    return true;
}

static const CfgModuleSpec RAD_SPC_SPEC = {
    HD_ECU_RADIO, "RAD", "rad_spc", "DID 0xFDBD compare/key in rad_special.cpp (radComputeSecurityKey)",
    RAD_SPC_PROFILES, RAD_SPC_PROFILE_COUNT,
    RAD_SPC_RULES, (uint16_t)RAD_SPC_RULE_COUNT, RAD_SPC_DEFAULT_PROFILE,
    0xF1EA,                  // identify by Parts Number
    0xFDBD,                  // pre-unlock DID: read right before 27 01, compared/keyed by radComputeSecurityKey
    radComputeSecurityKey,
    true,                    // unlockUpFront: the radio always needs 0x27 before a write
};

static CfgSpecialEngine engine(RAD_SPC_SPEC);

void   RadSpecial::reset() { engine.reset(); }
bool   RadSpecial::pageActive() { return engine.pageActive(); }
bool   RadSpecial::enter(char *statusMessage, size_t statusMessageLength) { return engine.enter(statusMessage, statusMessageLength); }
void   RadSpecial::exitPage(const char *reason) { engine.exitPage(reason); }
bool   RadSpecial::read(const char *testKey, char *outputBuffer, size_t outputBufferLength) { return engine.read(testKey, outputBuffer, outputBufferLength); }
bool   RadSpecial::run(const char *testKey, int option, bool confirmed, char *outputBuffer, size_t outputBufferLength) { return engine.run(testKey, option, confirmed, outputBuffer, outputBufferLength); }
bool   RadSpecial::restore(const char *testKey, char *outputBuffer, size_t outputBufferLength) { return engine.restore(testKey, outputBuffer, outputBufferLength); }
bool   RadSpecial::setProfile(int profileIndex) { return engine.setProfile(profileIndex); }
void   RadSpecial::loop() { engine.loop(); }
size_t RadSpecial::catalogJson(char *outputBuffer, size_t maxOutputLength) { return engine.catalogJson(outputBuffer, maxOutputLength); }

bool RadSpecial::readVin(char *outputBuffer, size_t outputBufferLength) {
    return engine.readVin(EcuInfo::vinDid(HD_ECU_RADIO), outputBuffer, outputBufferLength);
}

bool RadSpecial::writeVin(const char *vin, char *outputBuffer, size_t outputBufferLength) {
    return engine.writeVin(EcuInfo::vinDid(HD_ECU_RADIO), vin, outputBuffer, outputBufferLength);
}
