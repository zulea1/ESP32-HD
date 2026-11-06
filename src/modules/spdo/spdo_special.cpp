/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file spdo_special.cpp
 * @brief SPDO special functions - module description + static wrapper (engine: cfg_special.cpp).
 */
#include "spdo_special.h"
#include "spdo_special_data.h"
#include "spdo_algo.h"
#include "config.h"
#include "ecu_info.h"
#include <stdio.h>

// No verified Parts-Number -> profile rules for SPDO (single profile), see spdo_outputs_data.h header.
static const CfgRule SPDO_SPC_RULES[] = { {"", 0} };

// Thin adapter: spdoComputeSecurityKey() (shared with spdo_outputs.cpp and proc_runner.cpp) doesn't take the
// preUnlockDid bytes or the error-message buffer CfgKeyFn now carries - SPDO has no preUnlockDid (always empty
// here) and reports a failed unlock through the engine's generic "no known key" message, same as before.
static bool spdoComputeSecurityKeyCfg(const uint8_t *seed, uint8_t seedLen, uint8_t *key, uint8_t *keyLen,
                                       const uint8_t * /*preUnlockBytes*/, uint8_t /*preUnlockLength*/,
                                       char * /*outputMessage*/, size_t /*outputMessageLength*/) {
    return spdoComputeSecurityKey(seed, seedLen, key, keyLen);
}

static const CfgModuleSpec SPDO_SPC_SPEC = {
    HD_ECU_SPEEDO, "SPDO", "spdo_spc", "spdo_algo.h",
    SPDO_SPC_PROFILES, 1,
    SPDO_SPC_RULES, 0, 0,
    0xF1EA,      // identify by Parts Number (same as BCM); readFamilyId() is the fallback
    0,           // no DID read before the seed request in the database (AccSendCmd=2701)
    spdoComputeSecurityKeyCfg,
    false,       // unlock lazily (only on NRC 0x33), as before
};

static CfgSpecialEngine engine(SPDO_SPC_SPEC);

void   SpdoSpecial::reset() { engine.reset(); }
bool   SpdoSpecial::pageActive() { return engine.pageActive(); }
bool   SpdoSpecial::enter(char *statusMessage, size_t statusMessageLength) { return engine.enter(statusMessage, statusMessageLength); }
void   SpdoSpecial::exitPage(const char *reason) { engine.exitPage(reason); }
bool   SpdoSpecial::read(const char *testKey, char *outputBuffer, size_t outputBufferLength) { return engine.read(testKey, outputBuffer, outputBufferLength); }
bool   SpdoSpecial::run(const char *testKey, int option, bool confirmed, char *outputBuffer, size_t outputBufferLength) { return engine.run(testKey, option, confirmed, outputBuffer, outputBufferLength); }
bool   SpdoSpecial::restore(const char *testKey, char *outputBuffer, size_t outputBufferLength) { return engine.restore(testKey, outputBuffer, outputBufferLength); }
bool   SpdoSpecial::setProfile(int profileIndex) { return engine.setProfile(profileIndex); }
void   SpdoSpecial::loop() { engine.loop(); }
size_t SpdoSpecial::catalogJson(char *outputBuffer, size_t maxOutputLength) { return engine.catalogJson(outputBuffer, maxOutputLength); }

bool SpdoSpecial::readVin(char *outputBuffer, size_t outputBufferLength) {
    return engine.readVin(EcuInfo::vinDid(HD_ECU_SPEEDO), outputBuffer, outputBufferLength);
}

bool SpdoSpecial::writeVin(const char *vin, char *outputBuffer, size_t outputBufferLength) {
    return engine.writeVin(EcuInfo::vinDid(HD_ECU_SPEEDO), vin, outputBuffer, outputBufferLength);
}

// Identity/config blocks (DID 0157/0158) used by the original "spdo_pair" procedure - read both here instead
// of requiring an external diagnostic tool, so they can be saved and typed back in during pairing.
bool SpdoSpecial::readIdBlocks(char *outputBuffer, size_t outputBufferLength) {
    char hexBlock157[32] = {0}, hexBlock158[32] = {0};
    char statusMessage157[96] = {0}, statusMessage158[96] = {0};
    bool ok157 = engine.readHexBlock(0x0157, 12, hexBlock157, sizeof(hexBlock157), statusMessage157, sizeof(statusMessage157));
    bool ok158 = engine.readHexBlock(0x0158, 12, hexBlock158, sizeof(hexBlock158), statusMessage158, sizeof(statusMessage158));
    bool succeeded = ok157 && ok158;
    char combinedMessage[180];
    if (succeeded) snprintf(combinedMessage, sizeof(combinedMessage), "Identity blocks read OK");
    else           snprintf(combinedMessage, sizeof(combinedMessage), "%s%s%s", ok157 ? "" : statusMessage157, (!ok157 && !ok158) ? " / " : "", ok158 ? "" : statusMessage158);
    snprintf(outputBuffer, outputBufferLength, "{\"ok\":%s,\"msg\":\"%s\",\"blk157\":\"%s\",\"blk158\":\"%s\"}",
             succeeded ? "true" : "false", combinedMessage, hexBlock157, hexBlock158);
    return succeeded;
}

bool SpdoSpecial::writeIdBlocks(const char *hexBlock157, const char *hexBlock158, char *outputBuffer, size_t outputBufferLength) {
    char statusMessage157[96] = {0}, statusMessage158[96] = {0};
    bool ok157 = engine.writeHexBlock(0x0157, hexBlock157, statusMessage157, sizeof(statusMessage157));
    bool ok158 = engine.writeHexBlock(0x0158, hexBlock158, statusMessage158, sizeof(statusMessage158));
    bool succeeded = ok157 && ok158;
    char combinedMessage[200];
    snprintf(combinedMessage, sizeof(combinedMessage), "%s / %s", statusMessage157, statusMessage158);
    snprintf(outputBuffer, outputBufferLength, "{\"ok\":%s,\"msg\":\"%s\"}", succeeded ? "true" : "false", combinedMessage);
    return succeeded;
}
