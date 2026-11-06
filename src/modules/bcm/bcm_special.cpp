/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file bcm_special.cpp
 * @brief BCM special functions engine (see bcm_special.h): the configuration-option engine, security access,
 *        Boost read/write, and the TPMS functions, all sharing the same BCM session/transaction/messaging
 *        helpers below (file-local, like every other module's *_special.cpp).
 */
#include "bcm_special.h"
#include "bcm_algo.h"
#include "config.h"
#include "uds_handler.h"
#include "can_driver.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

static const uint32_t BCMS_KEEPALIVE_MS = 2000;
static const uint32_t BCMS_IDLE_EXIT_MS = 90000;

static bool     specialPageActive = false, sessionActive = false, sessionUnlocked = false;
static uint32_t lastTesterPresentMs = 0, lastOperationMs = 0;
static char     statusMessageText[160] = {0};
static char     partsNumber[48] = {0};
static int16_t  originalByteValues[BCM_SPC_FN_COUNT];     // original first data byte (-1 = not read yet)
static bool     originalValuesInitialized = false;

static void setMsg(const char *formatText, ...) __attribute__((format(printf, 1, 2)));
static void setMsg(const char *formatText, ...) {
    va_list argList; va_start(argList, formatText);
    vsnprintf(statusMessageText, sizeof(statusMessageText), formatText, argList);
    va_end(argList);
}

static const char *nrcText(uint8_t nrc) {
    switch (nrc) {
        case 0x12: return "sub-function not supported";
        case 0x13: return "incorrect message length";
        case 0x22: return "conditions not correct - the vehicle may be armed, disarm it first";
        case 0x24: return "request sequence error";
        case 0x31: return "request out of range";
        case 0x33: return "security access denied";
        case 0x35: return "invalid key";
        case 0x7E: case 0x7F: return "not supported in active session";
        default:   return "rejected";
    }
}

static bool tx(const uint8_t *requestBytes, uint8_t requestLength, UdsRawResult &rawResult) {
    UdsHandler::rawTransaction(HD_ECU_BCM, requestBytes, requestLength, rawResult, 450);
    if (rawResult.got_reply && rawResult.positive) lastTesterPresentMs = lastOperationMs = millis();
    return rawResult.got_reply && rawResult.positive;
}

static const uint8_t  BCM_SESSION_TRIES = 3;      // extended-session attempts before giving up
static const uint32_t BCM_SESSION_RETRY_MS = 60;  // gap between the priming frame and each retry

static bool ensureSession() {
    if (sessionActive) return true;
    const uint8_t extendedSessionRequest[2] = {0x10, 0x03};
    UdsRawResult rawResult;
    // First frame after bus idle is often lost; prime with a TesterPresent and retry on no-response (see the
    // long note in cfg_special.cpp). A negative reply is reported without retrying - the module answered.
    const uint8_t testerPresentFrame[8] = {0x02, 0x3E, 0x80, 0, 0, 0, 0, 0};
    CanDriver::transmit(HD_ECU_BCM, false, false, 8, testerPresentFrame);
    delay(BCM_SESSION_RETRY_MS);
    for (uint8_t attempt = 0; attempt < BCM_SESSION_TRIES; attempt++) {
        if (tx(extendedSessionRequest, 2, rawResult)) { sessionActive = true; sessionUnlocked = false; return true; }
        if (rawResult.got_reply) { setMsg("Extended session refused: %s (NRC 0x%02X)", nrcText(rawResult.nrc), rawResult.nrc); return false; }
        if (attempt + 1 < BCM_SESSION_TRIES) delay(BCM_SESSION_RETRY_MS);
    }
    setMsg("No response from the BCM (0x7E2) after %u tries", (unsigned)BCM_SESSION_TRIES);
    return false;
}

static bool unlock() {
    if (sessionUnlocked) return true;
    UdsRawResult rawResult;
    const uint8_t seedRequest[2] = {0x27, 0x01};
    // Seed request: retried up to 3 times on no-response. A negative reply is
    // not retried - e.g. NRC 0x37 'time delay not expired' would only get worse if hammered.
    bool seedOk = false;
    for (uint8_t attempt = 0; attempt < 3 && !seedOk; attempt++) {
        if (attempt) delay(80);
        seedOk = tx(seedRequest, 2, rawResult) && rawResult.len >= 3;
        if (!seedOk && rawResult.got_reply) break;
    }
    if (!seedOk) { setMsg("Security access: seed request failed%s", rawResult.got_reply ? " (rejected)" : " (no response after 3 tries)"); return false; }
    uint8_t seedLength = rawResult.len - 2;
    if (seedLength > 4) seedLength = 4;
    bool seedIsZero = true;
    for (uint8_t byteIndex = 0; byteIndex < seedLength; byteIndex++) if (rawResult.data[2 + byteIndex]) seedIsZero = false;
    if (seedIsZero) { sessionUnlocked = true; return true; }
    uint8_t keyBytes[4]; uint8_t keyLength = 0;
    if (!BcmSpecial::bcmComputeSecurityKey(&rawResult.data[2], seedLength, keyBytes, &keyLength) || keyLength == 0) {
        char seedHexText[16] = {0};
        for (uint8_t byteIndex = 0; byteIndex < seedLength; byteIndex++) snprintf(seedHexText + strlen(seedHexText), sizeof(seedHexText) - strlen(seedHexText), "%02X", rawResult.data[2 + byteIndex]);
        setMsg("The BCM requires security access and its seed %s has no known key (see bcm_algo.cpp)", seedHexText);
        Serial.printf("[BCM-SEC] Unknown seed: %s\n", seedHexText);
        return false;
    }
    uint8_t keyRequest[8] = {0x27, 0x02};
    memcpy(keyRequest + 2, keyBytes, keyLength);
    UdsRawResult keyResult;
    if (!tx(keyRequest, (uint8_t)(2 + keyLength), keyResult)) { setMsg("Security access: key rejected (NRC 0x%02X)", keyResult.nrc); return false; }
    sessionUnlocked = true;
    return true;
}

static int findFn(const char *testKey) {
    if (!testKey) return -1;
    for (int fnIndex = 0; fnIndex < BCM_SPC_FN_COUNT; fnIndex++) if (strcmp(BCM_SPC_FNS[fnIndex].key, testKey) == 0) return fnIndex;
    return -1;
}

// Read the configuration byte of a function (22 01 xx -> first data byte). A read is idempotent, so a lost
// frame is retried a few times (like readDidSynchronous does for the ECU-info reads) before reporting an
// error; a negative reply means the module answered and is not retried.
static const uint8_t BCM_READ_TRIES = 3;
static bool readByte(const BcmSpcFn &fn, uint8_t &outputByte) {
    const uint8_t requestBytes[3] = {0x22, (uint8_t)(fn.did >> 8), (uint8_t)(fn.did & 0xFF)};
    UdsRawResult rawResult;
    bool ok = false;
    for (uint8_t attempt = 0; attempt < BCM_READ_TRIES; attempt++) {
        if (attempt) delay(60);
        ok = tx(requestBytes, 3, rawResult);
        if (ok || rawResult.got_reply) break;
    }
    if (!ok) {
        if (rawResult.got_reply) setMsg("%s: BCM rejected the read - %s (NRC 0x%02X)", fn.name, nrcText(rawResult.nrc), rawResult.nrc);
        else             setMsg("%s: no response from the BCM after %u tries", fn.name, (unsigned)BCM_READ_TRIES);
        return false;
    }
    if (rawResult.len < 4) { setMsg("%s: BCM answer too short", fn.name); return false; }
    outputByte = rawResult.data[3];
    return true;
}

static bool writeByte(const BcmSpcFn &fn, uint8_t newByte) {
    const uint8_t requestBytes[4] = {0x2E, (uint8_t)(fn.did >> 8), (uint8_t)(fn.did & 0xFF), newByte};
    UdsRawResult rawResult;
    if (tx(requestBytes, 4, rawResult)) return true;
    if (rawResult.got_reply && rawResult.nrc == 0x33) {                    // security access needed
        if (!unlock()) return false;
        if (tx(requestBytes, 4, rawResult)) return true;
    }
    if (rawResult.got_reply) setMsg("%s: BCM rejected the write - %s (NRC 0x%02X)", fn.name, nrcText(rawResult.nrc), rawResult.nrc);
    else             setMsg("%s: no response from the BCM to the write", fn.name);
    return false;
}

// Option index shown for a byte (-1 = none of the options matches)
static int optionOf(const BcmSpcFn &fn, uint8_t currentByte) {
    for (uint8_t optionIndex = 0; optionIndex < fn.nopt; optionIndex++) if ((currentByte & fn.rmask) == (fn.opts[optionIndex].val & fn.rmask)) return optionIndex;
    return 0;                                              // the database falls back to its first option
}

static size_t reply(char *outputBuffer, size_t maxOutputLength, bool succeeded, const char *extraJson) {
    return (size_t)snprintf(outputBuffer, maxOutputLength, "{\"ok\":%s,\"msg\":\"%s\"%s%s}", succeeded ? "true" : "false", statusMessageText, extraJson && extraJson[0] ? "," : "", extraJson ? extraJson : "");
}

void BcmSpecial::reset() {
    specialPageActive = sessionActive = sessionUnlocked = false; statusMessageText[0] = 0; partsNumber[0] = 0;
    for (int fnIndex = 0; fnIndex < BCM_SPC_FN_COUNT; fnIndex++) originalByteValues[fnIndex] = -1;
    originalValuesInitialized = true;
}
bool BcmSpecial::pageActive() { return specialPageActive || sessionActive; }

bool BcmSpecial::enter(char *statusMessage, size_t statusMessageLength) {
    if (!originalValuesInitialized) reset();
    specialPageActive = true;
    char readPartsNumber[48] = {0};
    if (UdsHandler::readFamilyId(HD_ECU_BCM, readPartsNumber, sizeof(readPartsNumber), 450)) { strncpy(partsNumber, readPartsNumber, sizeof(partsNumber) - 1); partsNumber[sizeof(partsNumber) - 1] = 0; }
    setMsg("BCM special functions ready. Read a value first, then change it.");
    if (statusMessage && statusMessageLength) snprintf(statusMessage, statusMessageLength, "%s", statusMessageText);
    return true;
}

void BcmSpecial::exitPage(const char *reason) {
    if (sessionActive) {
        const uint8_t defaultSessionRequest[2] = {0x10, 0x01};
        UdsRawResult rawResult;
        tx(defaultSessionRequest, 2, rawResult);
    }
    sessionActive = sessionUnlocked = specialPageActive = false;
    setMsg("Session ended (%s) - default session restored", reason ? reason : "exit");
}

// Atomic-session close (keeps the page open). Called by BcmSessionGuard at the end of every operation below,
// so a BCM special-function action opens the extended session, does its work and drops back to the default
// session on its own.
static void closeSession() {
    if (sessionActive) {
        const uint8_t defaultSessionRequest[2] = {0x10, 0x01};
        UdsRawResult rawResult;
        tx(defaultSessionRequest, 2, rawResult);
    }
    sessionActive = sessionUnlocked = false;
}

// Scope guard: declare one at the top of any BCM special-function operation and the BCM is returned to its
// default session (10 01) on every exit path. This makes each operation atomic - open session, do the work,
// close session - so no session persists between button presses (no keepalive, no manual end-session).
struct BcmSessionGuard { ~BcmSessionGuard() { closeSession(); } };

bool BcmSpecial::read(const char *testKey, char *outputBuffer, size_t outputBufferLength) {
    BcmSessionGuard guard;              // atomic: return to the default session on every exit path
    int fnIndex = findFn(testKey);
    if (fnIndex < 0) { setMsg("Unknown function"); reply(outputBuffer, outputBufferLength, false, ""); return false; }
    const BcmSpcFn &fn = BCM_SPC_FNS[fnIndex];
    uint8_t currentByte = 0;
    if (!ensureSession() || !readByte(fn, currentByte)) { reply(outputBuffer, outputBufferLength, false, ""); return false; }
    if (originalByteValues[fnIndex] < 0) originalByteValues[fnIndex] = currentByte;
    int optionIndex = optionOf(fn, currentByte);
    setMsg("%s: %s", fn.name, fn.opts[optionIndex].text);
    char extraJson[160];
    snprintf(extraJson, sizeof(extraJson), "\"option\":%d,\"text\":\"%s\",\"byte\":%u,\"changed\":%s", optionIndex, fn.opts[optionIndex].text, currentByte, (originalByteValues[fnIndex] != currentByte) ? "true" : "false");
    reply(outputBuffer, outputBufferLength, true, extraJson);
    return true;
}

bool BcmSpecial::run(const char *testKey, int option, bool confirmed, char *outputBuffer, size_t outputBufferLength) {
    BcmSessionGuard guard;              // atomic: return to the default session on every exit path
    int fnIndex = findFn(testKey);
    if (fnIndex < 0) { setMsg("Unknown function"); reply(outputBuffer, outputBufferLength, false, ""); return false; }
    const BcmSpcFn &fn = BCM_SPC_FNS[fnIndex];
    if (option < 0 || option >= fn.nopt) { setMsg("Invalid option"); reply(outputBuffer, outputBufferLength, false, ""); return false; }
    if (fn.warn[0] && !confirmed) { setMsg("Confirmation required"); reply(outputBuffer, outputBufferLength, false, ""); return false; }
    uint8_t currentByte = 0;
    if (!ensureSession() || !readByte(fn, currentByte)) { reply(outputBuffer, outputBufferLength, false, ""); return false; }
    if (originalByteValues[fnIndex] < 0) originalByteValues[fnIndex] = currentByte;
    const uint8_t newByte = (uint8_t)((currentByte & ~fn.mask) | fn.opts[option].val);
    if (newByte == currentByte) {
        setMsg("%s is already %s - nothing written", fn.name, fn.opts[option].text);
        char extraJson[96]; snprintf(extraJson, sizeof(extraJson), "\"option\":%d,\"text\":\"%s\",\"byte\":%u", option, fn.opts[option].text, currentByte);
        reply(outputBuffer, outputBufferLength, true, extraJson);
        return true;
    }
    if (!writeByte(fn, newByte)) { reply(outputBuffer, outputBufferLength, false, ""); return false; }
    uint8_t verifyByte = 0;
    const bool readSucceeded = readByte(fn, verifyByte);
    if (!readSucceeded || optionOf(fn, verifyByte) != option) {
        char reasonText[96] = {0};
        if (!readSucceeded) snprintf(reasonText, sizeof(reasonText), " (%.80s)", statusMessageText);
        writeByte(fn, currentByte);                                  // put the old value back
        if (readSucceeded) setMsg("%s: verification failed (BCM byte 0x%02X, expected 0x%02X) - previous value written back", fn.name, verifyByte, newByte);
        else         setMsg("%s: could not verify the write%s - previous value written back", fn.name, reasonText);
        reply(outputBuffer, outputBufferLength, false, "");
        return false;
    }
    setMsg("%s set to %s", fn.name, fn.opts[option].text);
    Serial.printf("[BCM-SPC] %s: 0x%02X -> 0x%02X\n", fn.key, currentByte, verifyByte);
    char extraJson[96]; snprintf(extraJson, sizeof(extraJson), "\"option\":%d,\"text\":\"%s\",\"byte\":%u,\"changed\":true", option, fn.opts[option].text, verifyByte);
    reply(outputBuffer, outputBufferLength, true, extraJson);
    return true;
}

bool BcmSpecial::restore(const char *testKey, char *outputBuffer, size_t outputBufferLength) {
    BcmSessionGuard guard;              // atomic: return to the default session on every exit path
    int fnIndex = findFn(testKey);
    if (fnIndex < 0 || originalByteValues[fnIndex] < 0) { setMsg("Nothing to restore - read the value first"); reply(outputBuffer, outputBufferLength, false, ""); return false; }
    const BcmSpcFn &fn = BCM_SPC_FNS[fnIndex];
    uint8_t currentByte = 0;
    if (!ensureSession() || !readByte(fn, currentByte)) { reply(outputBuffer, outputBufferLength, false, ""); return false; }
    if (currentByte != (uint8_t)originalByteValues[fnIndex] && !writeByte(fn, (uint8_t)originalByteValues[fnIndex])) { reply(outputBuffer, outputBufferLength, false, ""); return false; }
    setMsg("%s: original value restored", fn.name);
    reply(outputBuffer, outputBufferLength, true, "");
    return true;
}

void BcmSpecial::loop() {
    if (!sessionActive) return;
    const uint32_t now = millis();
    if (now - lastOperationMs >= BCMS_IDLE_EXIT_MS) { exitPage("idle timeout"); return; }
    if (now - lastTesterPresentMs >= BCMS_KEEPALIVE_MS) {
        lastTesterPresentMs = now;
        const uint8_t testerPresentFrame[8] = {0x02, 0x3E, 0x80, 0, 0, 0, 0, 0};
        CanDriver::transmit(HD_ECU_BCM, false, false, 8, testerPresentFrame);
    }
}

size_t BcmSpecial::catalogJson(char *outputBuffer, size_t maxOutputLength) {
    if (!outputBuffer || maxOutputLength < 512) return 0;
    size_t writtenLength = (size_t)snprintf(outputBuffer, maxOutputLength, "{\"type\":\"bcm_spc\",\"ok\":true,\"cal\":\"%s\",\"boost_label\":\"%s\",\"auto\":true,\"profile\":0,\"profile_name\":\"BCM\",\"profiles\":[],\"functions\":[", partsNumber, BCM_BOOST_LABEL);
    if (writtenLength >= maxOutputLength) return 0;
    for (int fnIndex = 0; fnIndex < BCM_SPC_FN_COUNT; fnIndex++) {
        const BcmSpcFn &fn = BCM_SPC_FNS[fnIndex];
        int appendedLength = snprintf(outputBuffer + writtenLength, maxOutputLength - writtenLength, "%s{\"key\":\"%s\",\"name\":\"%s\",\"desc\":\"%s\",\"type\":6,\"warn\":\"%s\",\"opts\":[", fnIndex ? "," : "", fn.key, fn.name, fn.desc, fn.warn);
        if (appendedLength < 0 || (size_t)appendedLength >= maxOutputLength - writtenLength) return 0;
        writtenLength += (size_t)appendedLength;
        for (uint8_t optionIndex = 0; optionIndex < fn.nopt; optionIndex++) {
            appendedLength = snprintf(outputBuffer + writtenLength, maxOutputLength - writtenLength, "%s\"%s\"", optionIndex ? "," : "", fn.opts[optionIndex].text);
            if (appendedLength < 0 || (size_t)appendedLength >= maxOutputLength - writtenLength) return 0;
            writtenLength += (size_t)appendedLength;
        }
        appendedLength = snprintf(outputBuffer + writtenLength, maxOutputLength - writtenLength, "]}");
        if (appendedLength < 0 || (size_t)appendedLength >= maxOutputLength - writtenLength) return 0;
        writtenLength += (size_t)appendedLength;
        // TPMS (DB menus 11/12) sit right after "fork lock switch" (menu 10) and before "power lock switch"
        // (menu 13) in the database's menu order - insert them here so the catalog stays in that order.
        if (strcmp(fn.key, "fork_lock_switch") == 0) {
            appendedLength = snprintf(outputBuffer + writtenLength, maxOutputLength - writtenLength,
                ",{\"key\":\"tpms11\",\"name\":\"TPMS replacement (status)\","
                "\"desc\":\"Tire Pressure Monitoring Sensors status, Front and Rear. Read-only here: the write "
                "encoding for this field talks to two other module addresses with an unverified bit layout, so "
                "changing it is not offered to avoid mis-configuring the tire monitoring.\",\"type\":8,\"warn\":\"\"}"
                ",{\"key\":\"tpms12\",\"name\":\"TPMS sensor ID assignment\","
                "\"desc\":\"Manually assign the Tire Pressure Sensor identification in the BCM. Use the sensor ID "
                "printed on the sensor body.\",\"type\":7,\"warn\":\"Do you really want to assign Front and Rear "
                "Sensor ID?\"}");
            if (appendedLength < 0 || (size_t)appendedLength >= maxOutputLength - writtenLength) return 0;
            writtenLength += (size_t)appendedLength;
        }
    }
    int closingLength = snprintf(outputBuffer + writtenLength, maxOutputLength - writtenLength, "]}");
    if (closingLength < 0 || (size_t)closingLength >= maxOutputLength - writtenLength) return 0;
    return writtenLength + (size_t)closingLength;
}

// ---------------------------------------------------------------------------
// Security access, Boost read/write and the TPMS functions. None of these perform a security-access bypass:
// if the BCM asks for 27 01/27 02 and no key is known (bcm_algo.cpp), the attempt fails cleanly and is
// reported back, same as any other rejected request.
// ---------------------------------------------------------------------------

// Seed -> key lookup for BCM security access (UDS 0x27). Forwards to the module's own algorithm in bcm_algo.cpp
// (currently a placeholder returning key 0x0000 until the real BCM routine is recovered). The leading "::" calls
// the free function in bcm_algo.h, not this same-named member.
bool BcmSpecial::bcmComputeSecurityKey(const uint8_t *seedBytes, uint8_t seedLength, uint8_t *outputKeyBytes, uint8_t *outputKeyLength) {
    return ::bcmComputeSecurityKey(seedBytes, seedLength, outputKeyBytes, outputKeyLength);
}

bool BcmSpecial::openSecured() { return ensureSession() && unlock(); }

// Boost (not in the database function list). The DID block is exactly 6 bytes, the 6th always
// 0x00; a read that does not match that shape is an error. The value is the low nibble of each of the first 5
// bytes (mask 0F 0F 0F 0F 0F), shown as one digit per byte, e.g. F1 F2 F3 F4 F5 00 -> "12345". Apply takes the
// 5 digits, spreads them over the first 5 bytes (high nibble forced to 0 by the same mask) with a 0x00 6th
// byte, writes the block, then reads it back to verify. The block read before the first change this session is
// remembered so restoreBoost() can put it back.
static const uint16_t BOOST_DID    = (0x0333 ^ 0x0200) + 0x000E;   // TODO: placeholder - this evaluates to 0x0141, adjust before use
static const uint8_t  BOOST_LEN    = 6;    // DID data-block length (bytes)
static const uint8_t  BOOST_DIGITS = 5;    // first 5 bytes carry one BCD-ish digit each
static uint8_t  boostOriginalBlock[BOOST_LEN];
static bool     boostOriginalValid = false;

void bcmBoostForgetOriginal() { boostOriginalValid = false; }

static void boostFormatHex(const uint8_t *block, char *hexText, size_t hexTextLength) {
    size_t writtenLength = 0;
    hexText[0] = 0;
    for (uint8_t byteIndex = 0; byteIndex < BOOST_LEN && writtenLength + 3 < hexTextLength; byteIndex++)
        writtenLength += (size_t)snprintf(hexText + writtenLength, hexTextLength - writtenLength, "%02X ", block[byteIndex]);
    if (writtenLength) hexText[writtenLength - 1] = 0;              // drop trailing space
}

// Read the 6-byte block into outBlock (plain 22 03 33 - the caller has already opened the secured session).
// Returns false (with setMsg done) on no answer / rejection / wrong length / non-zero 6th byte.
static const uint8_t BCM_BOOST_READ_TRIES = 6;   // 0x22 read attempts (reads are idempotent); over the WiFi CAN
                                           // bridge a single frame is often lost, so retry often with a short
                                           // timeout - the same approach "Read ECU information" uses.

// Retry a read request on no-response (lost frame); stop as soon as the module answers (positive or NRC).
static bool txReadRetry(const uint8_t *requestBytes, uint8_t requestLength, UdsRawResult &rawResult) {
    bool ok = false;
    for (uint8_t attempt = 0; attempt < BCM_BOOST_READ_TRIES; attempt++) {
        if (attempt) delay(60);
        ok = tx(requestBytes, requestLength, rawResult);
        if (ok || rawResult.got_reply) break;
    }
    return ok;
}

static bool boostReadBlock(uint8_t outBlock[BOOST_LEN]) {
    const uint8_t requestBytes[3] = {0x22, (uint8_t)(BOOST_DID >> 8), (uint8_t)(BOOST_DID & 0xFF)};
    // Read up to BCM_BOOST_READ_TRIES times and trust the value only once two reads return the SAME well-formed
    // block (6 bytes, 6th = 0x00). A lost frame, a short answer or a one-off garbled read is ignored and we
    // keep trying instead of erroring - this is what stops the spurious "read again" over the flaky bridge.
    uint8_t samples[BCM_BOOST_READ_TRIES][BOOST_LEN];
    uint8_t nSamples = 0;
    bool anyValid = false, anyNrc = false;
    uint8_t lastNrc = 0;
    for (uint8_t attempt = 0; attempt < BCM_BOOST_READ_TRIES; attempt++) {
        if (attempt) delay(60);
        UdsRawResult rawResult;
        if (tx(requestBytes, 3, rawResult)) {
            const uint8_t dataLength = (rawResult.len >= 3) ? (uint8_t)(rawResult.len - 3) : 0;
            if (dataLength == BOOST_LEN && rawResult.data[3 + BOOST_LEN - 1] == 0x00) {   // well-formed block
                for (uint8_t p = 0; p < nSamples; p++)
                    if (memcmp(samples[p], &rawResult.data[3], BOOST_LEN) == 0) {         // two reads agree -> trust it
                        memcpy(outBlock, &rawResult.data[3], BOOST_LEN); return true;
                    }
                memcpy(samples[nSamples++], &rawResult.data[3], BOOST_LEN);
                anyValid = true;
            }
        } else if (rawResult.got_reply) { anyNrc = true; lastNrc = rawResult.nrc; }
    }
    if (anyValid) { memcpy(outBlock, samples[nSamples - 1], BOOST_LEN); return true; }     // best effort: a valid read that just never repeated
    if (anyNrc) setMsg(BCM_BOOST_LABEL ": BCM rejected the read - %s (NRC 0x%02X)", nrcText(lastNrc), lastNrc);
    else        setMsg(BCM_BOOST_LABEL ": no response from the BCM after %u tries", (unsigned)BCM_BOOST_READ_TRIES);
    return false;
}

static bool boostWriteBlock(const uint8_t block[BOOST_LEN]) {
    uint8_t requestBytes[3 + BOOST_LEN] = {0x2E, (uint8_t)(BOOST_DID >> 8), (uint8_t)(BOOST_DID & 0xFF)};
    memcpy(requestBytes + 3, block, BOOST_LEN);
    UdsRawResult rawResult;
    if (tx(requestBytes, (uint8_t)(3 + BOOST_LEN), rawResult)) return true;      // caller has opened the secured session
    if (rawResult.got_reply) setMsg(BCM_BOOST_LABEL ": BCM rejected the write - %s (NRC 0x%02X)", nrcText(rawResult.nrc), rawResult.nrc);
    else             setMsg(BCM_BOOST_LABEL ": no response from the BCM to the write");
    return false;
}

static size_t boostReply(char *outputBuffer, size_t outputBufferLength, const uint8_t block[BOOST_LEN]) {
    char digits[BOOST_DIGITS + 1];
    for (uint8_t digitIndex = 0; digitIndex < BOOST_DIGITS; digitIndex++) snprintf(digits + digitIndex, 2, "%X", block[digitIndex] & 0x0F);
    char hexText[3 * BOOST_LEN + 1];
    boostFormatHex(block, hexText, sizeof(hexText));
    char extraJson[96];
    snprintf(extraJson, sizeof(extraJson), "\"value\":\"%s\",\"current\":\"%s\"", digits, hexText);
    return reply(outputBuffer, outputBufferLength, true, extraJson);
}

bool BcmSpecial::readBoost(char *outputBuffer, size_t outputBufferLength) {
    BcmSessionGuard guard;              // atomic: return to the default session on every exit path
    if (!openSecured()) { reply(outputBuffer, outputBufferLength, false, ""); return false; }
    uint8_t block[BOOST_LEN];
    if (!boostReadBlock(block)) { reply(outputBuffer, outputBufferLength, false, ""); return false; }
    if (!boostOriginalValid) { memcpy(boostOriginalBlock, block, BOOST_LEN); boostOriginalValid = true; }
    char digits[BOOST_DIGITS + 1];
    for (uint8_t digitIndex = 0; digitIndex < BOOST_DIGITS; digitIndex++) snprintf(digits + digitIndex, 2, "%X", block[digitIndex] & 0x0F);
    setMsg(BCM_BOOST_LABEL ": %s", digits);
    boostReply(outputBuffer, outputBufferLength, block);
    return true;
}

// Apply: digitsText is the 5 digits to write. Per the "PIN read/change" procedure (DID 0141 = 5 PIN-digit
// bytes), each digit is 1-9 ONLY - 0 is never a valid PIN digit. A digit out of that range is caught HERE,
// before anything is sent on the bus, and reported with the message text ("Input number is out of
// range, valid range is 1 - 9"), which re-prompts without touching the module. Each digit is written
// verbatim into byte 0..4 (no hex nibble packing - 1-9 fits in one nibble anyway), byte 5 is 0x00.
bool BcmSpecial::writeBoost(const char *digitsText, char *outputBuffer, size_t outputBufferLength) {
    BcmSessionGuard guard;              // atomic: return to the default session on every exit path
    if (!digitsText || strlen(digitsText) != BOOST_DIGITS) { setMsg(BCM_BOOST_LABEL ": enter exactly %u digits", (unsigned)BOOST_DIGITS); reply(outputBuffer, outputBufferLength, false, ""); return false; }
    uint8_t newBlock[BOOST_LEN] = {0};
    for (uint8_t digitIndex = 0; digitIndex < BOOST_DIGITS; digitIndex++) {
        const char c = digitsText[digitIndex];
        if (c < '1' || c > '9') { setMsg("Input number is out of range, valid range is 1 - 9"); reply(outputBuffer, outputBufferLength, false, ""); return false; }
        newBlock[digitIndex] = (uint8_t)(c - '0');
    }
    newBlock[BOOST_LEN - 1] = 0x00;
    if (!openSecured()) { reply(outputBuffer, outputBufferLength, false, ""); return false; }
    uint8_t currentBlock[BOOST_LEN];
    if (!boostReadBlock(currentBlock)) { reply(outputBuffer, outputBufferLength, false, ""); return false; }
    if (!boostOriginalValid) { memcpy(boostOriginalBlock, currentBlock, BOOST_LEN); boostOriginalValid = true; }
    if (!boostWriteBlock(newBlock)) { reply(outputBuffer, outputBufferLength, false, ""); return false; }
    uint8_t verifyBlock[BOOST_LEN];
    if (!boostReadBlock(verifyBlock) || memcmp(verifyBlock, newBlock, BOOST_LEN) != 0) {
        boostWriteBlock(currentBlock);                                     // put the old value back
        setMsg(BCM_BOOST_LABEL ": verification failed - previous value written back");
        reply(outputBuffer, outputBufferLength, false, "");
        return false;
    }
    char digits[BOOST_DIGITS + 1], oldDigits[BOOST_DIGITS + 1];
    for (uint8_t digitIndex = 0; digitIndex < BOOST_DIGITS; digitIndex++) snprintf(digits + digitIndex, 2, "%X", verifyBlock[digitIndex] & 0x0F);
    for (uint8_t digitIndex = 0; digitIndex < BOOST_DIGITS; digitIndex++) snprintf(oldDigits + digitIndex, 2, "%X", currentBlock[digitIndex] & 0x0F);
    // Message matches the "PIN read/change" procedure's success screen wording ("PIN was Change to
    // <new>"); "old" is reported too so the UI's confirmation popup can show "PIN <old> Change to <new>?"
    // without needing its own separately-tracked original value.
    setMsg(BCM_BOOST_LABEL " was Change to %s", digits);
    char extraJson[96];
    snprintf(extraJson, sizeof(extraJson), "\"value\":\"%s\",\"old\":\"%s\"", digits, oldDigits);
    reply(outputBuffer, outputBufferLength, true, extraJson);
    return true;
}

bool BcmSpecial::restoreBoost(char *outputBuffer, size_t outputBufferLength) {
    BcmSessionGuard guard;              // atomic: return to the default session on every exit path
    if (!boostOriginalValid) { setMsg(BCM_BOOST_LABEL ": nothing to restore - read the value first"); reply(outputBuffer, outputBufferLength, false, ""); return false; }
    if (!openSecured()) { reply(outputBuffer, outputBufferLength, false, ""); return false; }
    uint8_t currentBlock[BOOST_LEN];
    if (!boostReadBlock(currentBlock)) { reply(outputBuffer, outputBufferLength, false, ""); return false; }
    if (memcmp(currentBlock, boostOriginalBlock, BOOST_LEN) != 0 && !boostWriteBlock(boostOriginalBlock)) { reply(outputBuffer, outputBufferLength, false, ""); return false; }
    setMsg(BCM_BOOST_LABEL ": original value restored");
    boostReply(outputBuffer, outputBufferLength, boostOriginalBlock);
    return true;
}

// Parse a 4-byte hex ID, accepting "AABBCCDD" or "AA-BB-CC-DD" / "AA:BB:CC:DD" / "AA BB CC DD".
static bool parseHex4(const char *hexText, uint8_t outputBytes[4]) {
    if (!hexText) return false;
    uint8_t byteCount = 0; uint32_t currentValue = 0; int nibbleCount = 0;
    for (const char *charPtr = hexText; *charPtr; charPtr++) {
        char currentChar = *charPtr;
        if (currentChar == '-' || currentChar == ':' || currentChar == ' ') continue;
        uint8_t digitValue;
        if (currentChar >= '0' && currentChar <= '9') digitValue = (uint8_t)(currentChar - '0');
        else if (currentChar >= 'A' && currentChar <= 'F') digitValue = (uint8_t)(currentChar - 'A' + 10);
        else if (currentChar >= 'a' && currentChar <= 'f') digitValue = (uint8_t)(currentChar - 'a' + 10);
        else return false;
        currentValue = (currentValue << 4) | digitValue; nibbleCount++;
        if (nibbleCount == 2) { if (byteCount >= 4) return false; outputBytes[byteCount++] = (uint8_t)currentValue; currentValue = 0; nibbleCount = 0; }
    }
    return byteCount == 4 && nibbleCount == 0;
}

bool BcmSpecial::tpmsConfigRead(char *outputBuffer, size_t outputBufferLength) {
    BcmSessionGuard guard;              // atomic: return to the default session on every exit path
    const uint8_t requestBytes[3] = {0x22, 0x01, 0x3C};
    UdsRawResult rawResult;
    if (!openSecured()) { reply(outputBuffer, outputBufferLength, false, ""); return false; }
    setMsg("Reading TPMS status (DID 0x013C)");
    if (!txReadRetry(requestBytes, 3, rawResult)) {
        if (rawResult.got_reply) setMsg("TPMS status: BCM rejected the read - %s (NRC 0x%02X)", nrcText(rawResult.nrc), rawResult.nrc);
        else             setMsg("TPMS status: no response from the BCM after %u tries", (unsigned)BCM_BOOST_READ_TRIES);
        reply(outputBuffer, outputBufferLength, false, "");
        return false;
    }
    if (rawResult.len < 4) { setMsg("TPMS status: BCM answer too short"); reply(outputBuffer, outputBufferLength, false, ""); return false; }
    uint8_t statusByte = rawResult.data[3];
    setMsg("TPMS status byte read (0x%02X)", statusByte);
    char extraJson[64];
    snprintf(extraJson, sizeof(extraJson), "\"byte\":%u", statusByte);
    reply(outputBuffer, outputBufferLength, true, extraJson);
    return true;
}

bool BcmSpecial::tpmsSensorRead(char *outputBuffer, size_t outputBufferLength) {
    BcmSessionGuard guard;              // atomic: return to the default session on every exit path
    const uint8_t frontRequestBytes[3] = {0x22, 0x01, 0x89};
    const uint8_t rearRequestBytes[3] = {0x22, 0x01, 0x8A};
    UdsRawResult frontResult, rearResult;
    if (!openSecured()) { reply(outputBuffer, outputBufferLength, false, ""); return false; }
    setMsg("Reading TPMS sensor IDs");
    if (!txReadRetry(frontRequestBytes, 3, frontResult)) {
        if (frontResult.got_reply) setMsg("Front sensor ID: BCM rejected the read - %s (NRC 0x%02X)", nrcText(frontResult.nrc), frontResult.nrc);
        else              setMsg("Front sensor ID: no response from the BCM after %u tries", (unsigned)BCM_BOOST_READ_TRIES);
        reply(outputBuffer, outputBufferLength, false, "");
        return false;
    }
    if (!txReadRetry(rearRequestBytes, 3, rearResult)) {
        if (rearResult.got_reply) setMsg("Rear sensor ID: BCM rejected the read - %s (NRC 0x%02X)", nrcText(rearResult.nrc), rearResult.nrc);
        else              setMsg("Rear sensor ID: no response from the BCM after %u tries", (unsigned)BCM_BOOST_READ_TRIES);
        reply(outputBuffer, outputBufferLength, false, "");
        return false;
    }
    if (frontResult.len < 7 || rearResult.len < 7) { setMsg("TPMS sensor ID: BCM answer too short"); reply(outputBuffer, outputBufferLength, false, ""); return false; }
    char frontHexText[16], rearHexText[16];
    snprintf(frontHexText, sizeof(frontHexText), "%02X-%02X-%02X-%02X", frontResult.data[3], frontResult.data[4], frontResult.data[5], frontResult.data[6]);
    snprintf(rearHexText, sizeof(rearHexText), "%02X-%02X-%02X-%02X", rearResult.data[3], rearResult.data[4], rearResult.data[5], rearResult.data[6]);
    setMsg("TPMS sensor IDs read (front %s, rear %s)", frontHexText, rearHexText);
    char extraJson[96];
    snprintf(extraJson, sizeof(extraJson), "\"front\":\"%s\",\"rear\":\"%s\"", frontHexText, rearHexText);
    reply(outputBuffer, outputBufferLength, true, extraJson);
    return true;
}

bool BcmSpecial::tpmsSensorWrite(const char *frontHex, const char *rearHex, bool confirmed, char *outputBuffer, size_t outputBufferLength) {
    BcmSessionGuard guard;              // atomic: return to the default session on every exit path
    if (!confirmed) { setMsg("Confirmation required - this changes the vehicle's assigned TPMS sensor IDs"); reply(outputBuffer, outputBufferLength, false, ""); return false; }
    uint8_t frontBytes[4], rearBytes[4];
    if (!parseHex4(frontHex, frontBytes) || !parseHex4(rearHex, rearBytes)) {
        setMsg("Sensor ID must be 4 hex byte pairs (e.g. AA-BB-CC-DD)");
        reply(outputBuffer, outputBufferLength, false, "");
        return false;
    }
    if (!openSecured()) { reply(outputBuffer, outputBufferLength, false, ""); return false; }   // extended session + security access up front
    UdsRawResult rawResult;
    const uint8_t frontWriteRequest[7] = {0x2E, 0x01, 0x89, frontBytes[0], frontBytes[1], frontBytes[2], frontBytes[3]};
    if (!tx(frontWriteRequest, 7, rawResult)) {
        if (rawResult.got_reply) setMsg("Front sensor ID write rejected - %s (NRC 0x%02X)", nrcText(rawResult.nrc), rawResult.nrc);
        else             setMsg("Front sensor ID write: no response from the BCM");
        reply(outputBuffer, outputBufferLength, false, "");
        return false;
    }
    const uint8_t rearWriteRequest[7] = {0x2E, 0x01, 0x8A, rearBytes[0], rearBytes[1], rearBytes[2], rearBytes[3]};
    if (!tx(rearWriteRequest, 7, rawResult)) {
        if (rawResult.got_reply) setMsg("Rear sensor ID write rejected - %s (NRC 0x%02X)", nrcText(rawResult.nrc), rawResult.nrc);
        else             setMsg("Rear sensor ID write: no response from the BCM");
        reply(outputBuffer, outputBufferLength, false, "");
        return false;
    }
    setMsg("TPMS sensor IDs written (front %s, rear %s)", frontHex, rearHex);
    Serial.printf("[BCM-SPC] TPMS sensor IDs written: front=%s rear=%s\n", frontHex, rearHex);
    reply(outputBuffer, outputBufferLength, true, "");
    return true;
}

// readVin()/writeVin() (DID 0xF1EF guess) removed 2026-10-31 - the BCM has no VIN DID at all, confirmed on
// real hardware, so the "VIN write (BCM)" card and these two functions were a false feature.
