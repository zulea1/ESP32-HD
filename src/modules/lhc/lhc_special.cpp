/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file lhc_special.cpp
 * @brief "Cruise / Push-to-Talk side" special function engine (see lhc_special.h). Owns LHC's (0x7E3) own
 *        session/unlock/read/write; the RHC (0x7E5) half of each write is delegated to RhcSpecial
 *        (src/modules/rhc/rhc_special.h/.cpp), its own equally self-contained module.
 */
#include "lhc_special.h"
#include "rhc_special.h"
#include "config.h"
#include "uds_handler.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// Every write - on LHC and independently on RHC - follows the same read-first sequence, never a cached or
// guessed length (a hardcoded 1-byte write and a hardcoded 6-byte write were both tried on real hardware
// 2026-10-31 and both got NRC 0x13 "incorrect message length"; this ECU's F1E8 is actually 8 bytes, not the
// widely-assumed 6):
//   1. read F1E8 fresh (22 F1 E8)
//   2. check the reply is long enough to reach the selector byte for this hardware part number
//   3. set the selector byte - the low-bit selector - to the value for the requested logical option
//   4. write the WHOLE block straight back (2E F1 E8 + all the bytes just read, selector byte changed)
// LHC does this itself; RHC does the exact same thing independently against its own F1E8 (RhcSpecial,
// src/modules/rhc/rhc_special.h/.cpp) - each ECU detects its OWN scheme from its OWN part number, so a
// mismatched old/new pair is still handled correctly.
//
// BOTH the byte INDEX and the byte VALUE for a given logical option (0=left,1=right) depend on the physical
// hand-control part fitted (2026-10-31: the original parts and the newer illuminated replacement parts lay
// F1E8 out differently - including the widely-assumed 0x49/0x4A convention not applying to the new parts at all), so
// they're picked at runtime from the module's own Hardware Part Number (DID 0xF1EA, same value the ECU
// Identification page shows), matching only the first 8 digits (any trailing revision letter, e.g. the "A" in
// "71500427A", is ignored):
//   LHC 71500128... -> byte[5], value 0x4A (left) / 0x49 (right)  - widely-assumed scheme, original part
//   LHC 71500427... -> byte[0], value 0x00 (left) / 0x01 (right)  - confirmed on real hardware 2026-10-31:
//                                F1E8 read 01 00 00 00 00 00 00 00 while the grip was physically
//                                Cruise-ON-RIGHT - the newer part does NOT use the 0x49/0x4A values at
//                                all, just the plain option bit
//   RHC 71500129... -> byte[5], value 0x4A (left) / 0x49 (right)  - original part, mirrors LHC's 71500128
//   RHC 71500426... -> byte[0], value 0x00 (left) / 0x01 (right)  - illuminated replacement, mirrors LHC's 71500427
// An unrecognized part number falls back to the byte[0]/0x00/0x01 scheme (the confirmed-working newer one).

static bool    pageActive_ = false;
static char    statusMessageText[160] = {0};
static int     origOption = 0;   // the logical option (0=left,1=right - the selector byte's low bit) as first seen, for "Restore original"
static bool    origValid = false;

// Step-by-step trace of the last unlock+write attempt on LHC (seed received, key computed, each UDS reply),
// so a failure in the UI shows exactly what happened instead of just a one-line NRC. Reset at the start of
// each hcWriteBoth() attempt, appended to by lhcEnterSession()/lhcUnlock()/lhcWriteOption() as they run.
static char    lhcTrace[220] = {0};
static void    lhcTraceReset() { lhcTrace[0] = 0; }
static void    lhcTraceAppend(const char *formatText, ...) __attribute__((format(printf, 1, 2)));
static void    lhcTraceAppend(const char *formatText, ...) {
    char piece[96];
    va_list argList; va_start(argList, formatText);
    vsnprintf(piece, sizeof(piece), formatText, argList);
    va_end(argList);
    size_t len = strlen(lhcTrace);
    if (len && len + 3 < sizeof(lhcTrace)) { lhcTrace[len] = ' '; lhcTrace[len+1] = '|'; lhcTrace[len+2] = ' '; lhcTrace[len+3] = 0; len += 3; }
    snprintf(lhcTrace + len, sizeof(lhcTrace) - len, "%s", piece);
}

static void setStatusMessage(const char *formatText, ...) __attribute__((format(printf, 1, 2)));
static void setStatusMessage(const char *formatText, ...) {
    va_list argList; va_start(argList, formatText);
    vsnprintf(statusMessageText, sizeof(statusMessageText), formatText, argList);
    va_end(argList);
}

static const char *hcNrcText(uint8_t nrc) {
    switch (nrc) {
        case 0x12: return "sub-function not supported";
        case 0x13: return "incorrect message length";
        case 0x22: return "conditions not correct";
        case 0x24: return "request sequence error";
        case 0x31: return "request out of range";
        case 0x33: return "security access denied";
        case 0x35: return "invalid key";
        case 0x7E: case 0x7F: return "not supported in active session";
        default:   return "rejected";
    }
}
static const char *lhcLastErrorNrcText = "";   // set right before a failing lhcTx() so run()/restore() can report it

static bool lhcTx(const uint8_t *requestBytes, uint8_t requestLength, UdsRawResult &rawResult, uint32_t timeoutMs = 500) {
    UdsHandler::rawTransaction(HD_ECU_LHC, requestBytes, requestLength, rawResult, timeoutMs);
    if (!rawResult.got_reply || !rawResult.positive) lhcLastErrorNrcText = rawResult.got_reply ? hcNrcText(rawResult.nrc) : "no response";
    return rawResult.got_reply && rawResult.positive;
}

static bool lhcEnterSession() {
    const uint8_t extendedSessionRequest[2] = {0x10, 0x03};
    UdsRawResult rawResult;
    bool ok = lhcTx(extendedSessionRequest, 2, rawResult, 450);
    if (!ok) lhcTraceAppend("10 03 (session) -> %s", rawResult.got_reply ? hcNrcText(rawResult.nrc) : "no response");
    return ok;
}

static void lhcCloseSession() {
    const uint8_t defaultSessionRequest[2] = {0x10, 0x01};
    UdsRawResult rawResult;
    lhcTx(defaultSessionRequest, 2, rawResult, 300);
}

// Security Access: key = seed XOR 0xFFFF (bitwise complement of the 16-bit seed). Confirmed on real hardware
// 2026-10-31 (seed=062D -> key=F9D2 unlocked LHC); an earlier table-lookup algorithm (shared with the ECM's
// mode 'B' lookup) was rejected with NRC 0x35 "invalid key" on this ECU, so it has been removed rather than
// kept as a first try - this is the ONLY algorithm attempted now. No F1F4 flag read either (this function has
// no activation flag, so there's no flag byte to read): 27 01 -> key=seed^FFFF -> 27 02.
static bool lhcUnlock() {
    UdsRawResult rawResult;
    const uint8_t seedRequest[2] = {0x27, 0x01};
    UdsHandler::rawTransaction(HD_ECU_LHC, seedRequest, 2, rawResult, 500);
    if (!rawResult.got_reply) { lhcTraceAppend("27 01 (seed req) -> no response"); lhcLastErrorNrcText = "no response"; return false; }
    if (!rawResult.positive) {
        lhcTraceAppend("27 01 (seed req) -> NRC 0x%02X (%s)", rawResult.nrc, hcNrcText(rawResult.nrc));
        lhcLastErrorNrcText = hcNrcText(rawResult.nrc);
        return false;
    }
    if (rawResult.len < 4) { lhcTraceAppend("27 01 (seed req) -> positive but short reply (%u bytes)", (unsigned)rawResult.len); lhcLastErrorNrcText = "short seed reply"; return false; }

    uint8_t seedLength = (uint8_t)(rawResult.len - 2);
    if (seedLength > 8) seedLength = 8;
    uint8_t seedBytes[8]; memcpy(seedBytes, &rawResult.data[2], seedLength);
    char seedHexText[20] = {0};
    for (uint8_t byteIndex = 0; byteIndex < seedLength; byteIndex++) snprintf(seedHexText + strlen(seedHexText), sizeof(seedHexText) - strlen(seedHexText), "%02X", seedBytes[byteIndex]);
    lhcTraceAppend("seed=%s", seedHexText);

    bool seedIsZero = true;
    for (uint8_t byteIndex = 0; byteIndex < seedLength; byteIndex++) if (seedBytes[byteIndex]) seedIsZero = false;
    if (seedIsZero) { lhcTraceAppend("zero seed, no unlock needed"); return true; }

    const uint16_t seed16 = (uint16_t)(((uint16_t)seedBytes[0] << 8) | (uint16_t)seedBytes[1]);
    const uint16_t key16 = (uint16_t)(seed16 ^ 0xFFFFu);
    const uint8_t keyBytes[2] = {(uint8_t)(key16 >> 8), (uint8_t)(key16 & 0xFF)};
    lhcTraceAppend("key=%02X%02X (seed XOR 0xFFFF)", keyBytes[0], keyBytes[1]);

    const uint8_t keyRequest[4] = {0x27, 0x02, keyBytes[0], keyBytes[1]};
    UdsRawResult keyResult;
    UdsHandler::rawTransaction(HD_ECU_LHC, keyRequest, 4, keyResult, 500);
    if (!keyResult.got_reply) { lhcTraceAppend("27 02 (send key) -> no response"); lhcLastErrorNrcText = "no response"; return false; }
    if (!keyResult.positive) {
        lhcTraceAppend("27 02 (send key) -> NRC 0x%02X (%s)", keyResult.nrc, hcNrcText(keyResult.nrc));
        lhcLastErrorNrcText = hcNrcText(keyResult.nrc);
        return false;
    }
    lhcTraceAppend("27 02 (send key) -> unlocked");
    return true;
}

// Read the F1E8 block from LHC (caller has opened the session). The actual data length (total reply bytes
// minus the 3-byte 62 F1 E8 header) is reported via *outLen - measured every time, never assumed. Returns
// false on short/no reply.
static char lastReadHeaderHex[48] = {0};   // TEMPORARY diagnostic: SID+DID bytes of the last read, raw (2026-10-31)
static bool lhcReadF1E8(uint8_t out[8], uint8_t *outLen) {
    const uint8_t requestBytes[3] = {0x22, 0xF1, 0xE8};
    UdsRawResult rawResult;
    bool txOk = lhcTx(requestBytes, 3, rawResult, 500);
    snprintf(lastReadHeaderHex, sizeof(lastReadHeaderHex), "pos=%d len=%u hdr=%02X%02X%02X",
             rawResult.positive ? 1 : 0, (unsigned)rawResult.len,
             rawResult.len >= 1 ? rawResult.data[0] : 0, rawResult.len >= 2 ? rawResult.data[1] : 0, rawResult.len >= 3 ? rawResult.data[2] : 0);
    if (!txOk || rawResult.len <= 3) return false;
    uint8_t dataLength = (uint8_t)(rawResult.len - 3);
    if (dataLength > 8) dataLength = 8;
    memcpy(out, &rawResult.data[3], dataLength);
    if (outLen) *outLen = dataLength;
    return true;
}

// Picks the selector byte INDEX for LHC from its own Hardware Part Number (DID 0xF1EA), matching only the
// first 8 digits - see the header comment above for the part-number -> index table. Falls back to byte[0]
// (the confirmed-working newer layout) if the part number can't be read or doesn't match a known prefix.
static uint8_t lhcDetectSelectorIndex() {
    char partNumber[24] = {0};
    if (!UdsHandler::readFamilyId(HD_ECU_LHC, partNumber, sizeof(partNumber), 400) || partNumber[0] == '\0') {
        lhcTraceAppend("part# read failed -> default selector byte[0]");
        return 0;
    }
    if (strncmp(partNumber, "71500128", 8) == 0) { lhcTraceAppend("part#=%s -> selector byte[5]", partNumber); return 5; }
    if (strncmp(partNumber, "71500427", 8) == 0) { lhcTraceAppend("part#=%s -> selector byte[0]", partNumber); return 0; }
    lhcTraceAppend("part#=%s -> unrecognized, default selector byte[0]", partNumber);
    return 0;
}

// Read-check-change-write core (caller has unlocked LHC): reads F1E8 fresh, requires the reply to reach
// selectorIndex, records the pre-change option (that byte's low bit) the first time this runs (for "Restore
// original"), sets that byte to selectorByte exactly, and writes the whole block straight back - same length
// it was just read with. Returns true on a positive write reply.
static bool lhcWriteBlock(uint8_t selectorIndex, uint8_t selectorByte) {
    uint8_t block[8] = {0};
    uint8_t dataLength = 0;
    if (!lhcReadF1E8(block, &dataLength)) { lhcTraceAppend("22 F1E8 (read before write) -> no response"); lhcLastErrorNrcText = "no response"; return false; }
    if (dataLength <= selectorIndex) { lhcTraceAppend("22 F1E8 (read before write) -> only %u byte(s), need index %u", (unsigned)dataLength, (unsigned)selectorIndex); lhcLastErrorNrcText = "DID too short"; return false; }
    if (!origValid) { origOption = block[selectorIndex] & 0x01; origValid = true; }
    block[selectorIndex] = selectorByte;

    uint8_t requestBytes[3 + 8] = {0x2E, 0xF1, 0xE8};
    memcpy(requestBytes + 3, block, dataLength);
    UdsRawResult rawResult;
    UdsHandler::rawTransaction(HD_ECU_LHC, requestBytes, (uint8_t)(3 + dataLength), rawResult, 800);
    if (!rawResult.got_reply) { lhcTraceAppend("2E F1E8 (write) -> no response"); lhcLastErrorNrcText = "no response"; return false; }
    if (!rawResult.positive) {
        lhcTraceAppend("2E F1E8 (write) -> NRC 0x%02X (%s)", rawResult.nrc, hcNrcText(rawResult.nrc));
        lhcLastErrorNrcText = hcNrcText(rawResult.nrc);
        return false;
    }
    lhcTraceAppend("2E F1E8 (write) -> OK");
    return true;
}

// Computes the actual byte VALUE to write for a logical option (0=left,1=right), which depends on the part's
// scheme just like the index does: the original part's byte[5] takes the widely-assumed 0x4A/0x49 values, while
// the newer illuminated part's byte[0] simply takes the option bit itself (0x00/0x01 - confirmed on real
// hardware 2026-10-31: F1E8 byte[0] read 0x01 while the grip was physically Cruise-ON-RIGHT, and 0x00 for
// Cruise-ON-LEFT - NOT the 0x49/0x4A values, which only apply at byte[5] on the original part).
static uint8_t lhcSelectorValueForOption(uint8_t selectorIndex, int option) {
    if (selectorIndex == 5) return (uint8_t)(option ? 0x49 : 0x4A);
    return (uint8_t)(option ? 0x01 : 0x00);
}

// For "Apply": detect this LHC's own scheme (index + value convention) from its part number, then write.
static bool lhcWriteOption(int option) {
    uint8_t selectorIndex = lhcDetectSelectorIndex();
    uint8_t selectorByte = lhcSelectorValueForOption(selectorIndex, option);
    return lhcWriteBlock(selectorIndex, selectorByte);
}

// Set the same logical option (0=left,1=right) on BOTH hand controls: LHC via this module's own
// read-check-change-write above, RHC via RhcSpecial::writeF1E8() (rhc_special.cpp) doing the exact same
// sequence independently against its own F1E8 - each ECU detects its OWN scheme (index + byte value) from
// its OWN part number, so a mismatched old/new pair is still handled correctly. Each ECU is fully
// session/unlock/write/close atomic on its own. Both are attempted even if the first one failed, so a bad
// LHC doesn't hide a bad RHC (or vice versa); *failEcu names the FIRST one that failed, and failReasonOut
// carries its NRC/reason text.
static bool hcWriteBoth(int option, uint32_t *failEcu, char *failReasonOut, size_t failReasonOutLength) {
    lhcTraceReset();
    bool lhcOk = lhcEnterSession() && lhcUnlock() && lhcWriteOption(option);
    lhcCloseSession();
    if (!lhcOk && failEcu && *failEcu == 0) { *failEcu = HD_ECU_LHC; if (failReasonOut) snprintf(failReasonOut, failReasonOutLength, "%s", lhcTrace); }

    char rhcReason[200] = {0};
    bool rhcOk = RhcSpecial::writeF1E8(option, rhcReason, sizeof(rhcReason));
    if (!rhcOk && failEcu && *failEcu == 0) { *failEcu = HD_ECU_RHC; if (failReasonOut) snprintf(failReasonOut, failReasonOutLength, "%s", rhcReason); }

    return lhcOk && rhcOk;
}

// ---------------------------------------------------------------------------
void LhcSpecial::reset() { pageActive_ = false; statusMessageText[0] = 0; origValid = false; }
bool LhcSpecial::pageActive() { return pageActive_; }

bool LhcSpecial::enter(char *statusMessage, size_t statusMessageLength) {
    pageActive_ = true;
    setStatusMessage("Hand control special functions ready (LHC 0x7E3 + RHC 0x7E5). Read the current grip assignment first, then change it.");
    if (statusMessage && statusMessageLength) snprintf(statusMessage, statusMessageLength, "%s", statusMessageText);
    return true;
}

void LhcSpecial::exitPage(const char *reason) {
    pageActive_ = false;
    setStatusMessage("Hand control special functions closed (%s).", reason ? reason : "exit");
}

// Builds the JSON the type-6 config-option card expects from a selector byte (option = its low bit). No
// length is reported here - length is purely an internal read/write-matching detail now.
static size_t hcReply(char *outputBuffer, size_t outputBufferLength, bool ok, uint8_t selectorByte, const char *msg) {
    int option = (ok && (selectorByte & 0x01)) ? 1 : 0;
    const char *optionText = option ? "Cruise RIGHT grip / PTT LEFT" : "Cruise LEFT grip / PTT RIGHT";
    bool changed = ok && origValid && option != origOption;
    return (size_t)snprintf(outputBuffer, outputBufferLength, "{\"ok\":%s,\"option\":%d,\"text\":\"%s\",\"byte\":%u,\"changed\":%s,\"msg\":\"%s\"}",
                             ok ? "true" : "false", option, optionText, ok ? selectorByte : 0, changed ? "true" : "false", msg);
}

bool LhcSpecial::read(const char *testKey, char *outputBuffer, size_t outputBufferLength) {
    (void)testKey;
    lhcTraceReset();
    bool entered = lhcEnterSession();
    uint8_t selectorIndex = entered ? lhcDetectSelectorIndex() : 0;
    uint8_t block[8] = {0};
    uint8_t dataLength = 0;
    bool ok = entered && lhcReadF1E8(block, &dataLength);
    lhcCloseSession();
    if (ok && dataLength <= selectorIndex) ok = false;   // too short to reach the selector byte for this part
    uint8_t selectorByte = ok ? block[selectorIndex] : 0;
    if (ok && !origValid) { origOption = selectorByte & 0x01; origValid = true; }

    // Also read RHC's own F1E8 (read-only, no unlock needed) so both buffers show side by side.
    char rhcRawHex[40] = {0};
    bool rhcOk = RhcSpecial::readF1E8ForDisplay(rhcRawHex, sizeof(rhcRawHex));

    char message[260];
    if (ok) {
        char rawHex[40] = {0};
        for (uint8_t i = 0; i < dataLength; i++) snprintf(rawHex + strlen(rawHex), sizeof(rawHex) - strlen(rawHex), "%02X ", block[i]);
        snprintf(message, sizeof(message), "LHC: %s(idx %u) | RHC: %s%s [%s]",
                 rawHex, (unsigned)selectorIndex, rhcOk ? rhcRawHex : "", rhcOk ? "" : "read failed", lastReadHeaderHex);
    } else {
        snprintf(message, sizeof(message), "LHC read failed [%s] | RHC: %s%s", lastReadHeaderHex, rhcOk ? rhcRawHex : "", rhcOk ? "" : "read failed");
    }
    hcReply(outputBuffer, outputBufferLength, ok, selectorByte, message);
    return ok;
}

bool LhcSpecial::run(const char *testKey, int option, bool confirmed, char *outputBuffer, size_t outputBufferLength) {
    (void)testKey; (void)confirmed;
    if (option != 0 && option != 1) option = 0;
    uint32_t failEcu = 0; char failReason[220] = {0};
    bool ok = hcWriteBoth(option, &failEcu, failReason, sizeof(failReason));
    char message[320];
    if (ok) snprintf(message, sizeof(message), "written to both hand controls");
    else    snprintf(message, sizeof(message), "write/unlock failed on 0x%03X: %s", (unsigned)failEcu, failReason);
    hcReply(outputBuffer, outputBufferLength, ok, (uint8_t)option, message);
    return ok;
}

bool LhcSpecial::restore(const char *testKey, char *outputBuffer, size_t outputBufferLength) {
    (void)testKey;
    if (!origValid) { snprintf(outputBuffer, outputBufferLength, "{\"ok\":false,\"msg\":\"nothing to restore - read the value first\"}"); return false; }
    uint32_t failEcu = 0; char failReason[220] = {0};
    bool ok = hcWriteBoth(origOption, &failEcu, failReason, sizeof(failReason));
    char message[320];
    if (ok) snprintf(message, sizeof(message), "original value restored to both hand controls");
    else    snprintf(message, sizeof(message), "restore failed on 0x%03X: %s", (unsigned)failEcu, failReason);
    hcReply(outputBuffer, outputBufferLength, ok, (uint8_t)origOption, message);
    return ok;
}

size_t LhcSpecial::catalogJson(char *outputBuffer, size_t maxOutputLength) {
    if (!outputBuffer || maxOutputLength < 256) return 0;
    int writtenLength = snprintf(outputBuffer, maxOutputLength,
        "{\"ok\":true,\"cal\":\"LHC\",\"profile_name\":\"Hand controls\",\"profile\":0,\"auto\":true,\"profiles\":[],"
        "\"functions\":[{\"key\":\"hc_cruise\",\"type\":6,\"name\":\"Cruise / Push-to-Talk side\","
        "\"desc\":\"Which grip has cruise control and which has push-to-talk. Writes both hand-control modules (LHC + RHC).\","
        "\"opts\":[\"Cruise LEFT grip / PTT RIGHT\",\"Cruise RIGHT grip / PTT LEFT\"],\"warn\":\"\"}]}");
    return (writtenLength > 0 && (size_t)writtenLength < maxOutputLength) ? (size_t)writtenLength : 0;
}
