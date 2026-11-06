/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file rhc_special.cpp
 * @brief RHC (0x7E5) DID F1E8 session/unlock/write primitives (see rhc_special.h).
 */
#include "rhc_special.h"
#include "config.h"
#include "uds_handler.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

static char rhcTrace[200] = {0};
static void rhcTraceReset() { rhcTrace[0] = 0; }
static void rhcTraceAppend(const char *formatText, ...) __attribute__((format(printf, 1, 2)));
static void rhcTraceAppend(const char *formatText, ...) {
    char piece[96];
    va_list argList; va_start(argList, formatText);
    vsnprintf(piece, sizeof(piece), formatText, argList);
    va_end(argList);
    size_t len = strlen(rhcTrace);
    if (len && len + 3 < sizeof(rhcTrace)) { rhcTrace[len] = ' '; rhcTrace[len+1] = '|'; rhcTrace[len+2] = ' '; rhcTrace[len+3] = 0; len += 3; }
    snprintf(rhcTrace + len, sizeof(rhcTrace) - len, "%s", piece);
}

static const char *rhcNrcText(uint8_t nrc) {
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

static bool rhcTx(const uint8_t *requestBytes, uint8_t requestLength, UdsRawResult &rawResult, uint32_t timeoutMs = 500) {
    UdsHandler::rawTransaction(HD_ECU_RHC, requestBytes, requestLength, rawResult, timeoutMs);
    return rawResult.got_reply && rawResult.positive;
}

static bool rhcEnterSession() {
    const uint8_t extendedSessionRequest[2] = {0x10, 0x03};
    UdsRawResult rawResult;
    bool ok = rhcTx(extendedSessionRequest, 2, rawResult, 450);
    if (!ok) rhcTraceAppend("10 03 (session) -> %s", rawResult.got_reply ? rhcNrcText(rawResult.nrc) : "no response");
    return ok;
}

static void rhcCloseSession() {
    const uint8_t defaultSessionRequest[2] = {0x10, 0x01};
    UdsRawResult rawResult;
    rhcTx(defaultSessionRequest, 2, rawResult, 300);
}

// Security Access: key = seed XOR 0xFFFF (bitwise complement of the 16-bit seed) - same algorithm confirmed
// on LHC on real hardware 2026-10-31. No F1F4 flag read (this function has no activation flag, so there's
// no flag byte to read).
static bool rhcUnlock() {
    UdsRawResult rawResult;
    const uint8_t seedRequest[2] = {0x27, 0x01};
    rhcTx(seedRequest, 2, rawResult, 500);
    if (!rawResult.got_reply) { rhcTraceAppend("27 01 (seed req) -> no response"); return false; }
    if (!rawResult.positive) { rhcTraceAppend("27 01 (seed req) -> NRC 0x%02X (%s)", rawResult.nrc, rhcNrcText(rawResult.nrc)); return false; }
    if (rawResult.len < 4) { rhcTraceAppend("27 01 (seed req) -> short reply (%u bytes)", (unsigned)rawResult.len); return false; }

    const uint8_t seedBytes[2] = {rawResult.data[2], rawResult.data[3]};
    rhcTraceAppend("seed=%02X%02X", seedBytes[0], seedBytes[1]);
    if (seedBytes[0] == 0 && seedBytes[1] == 0) { rhcTraceAppend("zero seed, no unlock needed"); return true; }

    const uint16_t seed16 = (uint16_t)(((uint16_t)seedBytes[0] << 8) | (uint16_t)seedBytes[1]);
    const uint16_t key16 = (uint16_t)(seed16 ^ 0xFFFFu);
    const uint8_t keyBytes[2] = {(uint8_t)(key16 >> 8), (uint8_t)(key16 & 0xFF)};
    rhcTraceAppend("key=%02X%02X (seed XOR 0xFFFF)", keyBytes[0], keyBytes[1]);

    const uint8_t keyRequest[4] = {0x27, 0x02, keyBytes[0], keyBytes[1]};
    UdsRawResult keyResult;
    rhcTx(keyRequest, 4, keyResult, 500);
    if (!keyResult.got_reply) { rhcTraceAppend("27 02 (send key) -> no response"); return false; }
    if (!keyResult.positive) { rhcTraceAppend("27 02 (send key) -> NRC 0x%02X (%s)", keyResult.nrc, rhcNrcText(keyResult.nrc)); return false; }
    rhcTraceAppend("27 02 (send key) -> unlocked");
    return true;
}

// Read the F1E8 block from RHC (caller has opened the session). The actual data length (total reply bytes
// minus the 3-byte 62 F1 E8 header) is reported via *outLen - measured every time, never assumed. Returns
// false on short/no reply.
static bool rhcReadF1E8(uint8_t out[8], uint8_t *outLen) {
    const uint8_t requestBytes[3] = {0x22, 0xF1, 0xE8};
    UdsRawResult rawResult;
    if (!rhcTx(requestBytes, 3, rawResult, 500) || rawResult.len <= 3) return false;
    uint8_t dataLength = (uint8_t)(rawResult.len - 3);
    if (dataLength > 8) dataLength = 8;
    memcpy(out, &rawResult.data[3], dataLength);
    if (outLen) *outLen = dataLength;
    return true;
}

// Picks the selector byte INDEX for RHC from its own Hardware Part Number (DID 0xF1EA), matching only the
// first 8 digits. Falls back to byte[0] (the confirmed-working newer layout) if unreadable/unrecognized.
static uint8_t rhcDetectSelectorIndex() {
    char partNumber[24] = {0};
    if (!UdsHandler::readFamilyId(HD_ECU_RHC, partNumber, sizeof(partNumber), 400) || partNumber[0] == '\0') {
        rhcTraceAppend("part# read failed -> default selector byte[0]");
        return 0;
    }
    if (strncmp(partNumber, "71500129", 8) == 0) { rhcTraceAppend("part#=%s -> selector byte[5]", partNumber); return 5; }
    if (strncmp(partNumber, "71500426", 8) == 0) { rhcTraceAppend("part#=%s -> selector byte[0]", partNumber); return 0; }
    rhcTraceAppend("part#=%s -> unrecognized, default selector byte[0]", partNumber);
    return 0;
}

// Computes the actual byte VALUE to write for a logical option (0=left,1=right) on RHC - same two schemes as
// LHC (see lhc_special.cpp): byte[5] takes the widely-assumed 0x4A/0x49 values (original part), byte[0] takes the
// plain option bit 0x00/0x01 (newer illuminated part, confirmed-working).
static uint8_t rhcSelectorValueForOption(uint8_t selectorIndex, int option) {
    if (selectorIndex == 5) return (uint8_t)(option ? 0x49 : 0x4A);
    return (uint8_t)(option ? 0x01 : 0x00);
}

bool RhcSpecial::writeF1E8(int option, char *statusMessage, size_t statusMessageLength) {
    rhcTraceReset();
    bool ok = rhcEnterSession() && rhcUnlock();
    if (ok) {
        // Read-check-change-write, fully independent of LHC's own read: never assume RHC's F1E8 is the same
        // length (or scheme) as LHC's, even though both are currently the newer illuminated part.
        uint8_t selectorIndex = rhcDetectSelectorIndex();
        uint8_t selectorByte = rhcSelectorValueForOption(selectorIndex, option);
        uint8_t block[8] = {0};
        uint8_t dataLength = 0;
        ok = rhcReadF1E8(block, &dataLength);
        if (!ok) {
            rhcTraceAppend("22 F1E8 (read before write) -> no response");
        } else if (dataLength <= selectorIndex) {
            rhcTraceAppend("22 F1E8 (read before write) -> only %u byte(s), need index %u", (unsigned)dataLength, (unsigned)selectorIndex);
            ok = false;
        } else {
            block[selectorIndex] = selectorByte;
            uint8_t requestBytes[3 + 8] = {0x2E, 0xF1, 0xE8};
            memcpy(requestBytes + 3, block, dataLength);
            UdsRawResult rawResult;
            ok = rhcTx(requestBytes, (uint8_t)(3 + dataLength), rawResult, 800);
            if (!ok) rhcTraceAppend("2E F1E8 (write) -> %s", rawResult.got_reply ? rhcNrcText(rawResult.nrc) : "no response");
            else     rhcTraceAppend("2E F1E8 (write) -> OK");
        }
    }
    rhcCloseSession();
    if (statusMessage) snprintf(statusMessage, statusMessageLength, "%s", ok ? "written OK" : rhcTrace);
    return ok;
}

bool RhcSpecial::readF1E8ForDisplay(char *rawHexOut, size_t rawHexOutLength) {
    rhcTraceReset();
    bool entered = rhcEnterSession();
    uint8_t block[8] = {0};
    uint8_t dataLength = 0;
    bool ok = entered && rhcReadF1E8(block, &dataLength);
    rhcCloseSession();
    if (rawHexOut && rawHexOutLength) {
        rawHexOut[0] = '\0';
        if (ok) {
            for (uint8_t i = 0; i < dataLength; i++) snprintf(rawHexOut + strlen(rawHexOut), rawHexOutLength - strlen(rawHexOut), "%02X ", block[i]);
        } else {
            snprintf(rawHexOut, rawHexOutLength, "read failed");
        }
    }
    return ok;
}
