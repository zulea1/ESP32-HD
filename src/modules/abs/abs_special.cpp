/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file abs_special.cpp
 * @brief ABS special functions engine (see abs_special.h).
 */
#include "abs_special.h"
#include "abs_algo.h"
#include "proc_runner.h"
#include "config.h"
#include "uds_handler.h"
#include "can_driver.h"
#include "ecu_info.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

static const uint32_t ABSS_IDLE_EXIT_MS = 120000;

static bool     specialPageActive = false;
static char     statusMessageText[200] = {0};
static uint32_t lastOperationMs = 0;

static void setStatusMessage(const char *formatText, ...) __attribute__((format(printf, 1, 2)));
static void setStatusMessage(const char *formatText, ...) {
    va_list argList; va_start(argList, formatText);
    vsnprintf(statusMessageText, sizeof(statusMessageText), formatText, argList);
    va_end(argList);
}

void AbsSpecial::reset() { specialPageActive = false; statusMessageText[0] = 0; }
bool AbsSpecial::pageActive() { return specialPageActive; }

bool AbsSpecial::enter(char *statusMessage, size_t statusMessageLength) {
    specialPageActive = true;
    lastOperationMs = millis();
    setStatusMessage("ABS bleed ready. The front and rear procedures run step-by-step with on-screen instructions; "
           "the tool works the ABS pump and valves for you while you operate the brake. Put the bike on a stand with "
           "the wheels off the ground, engine off, ignition on.");
    if (statusMessage && statusMessageLength) snprintf(statusMessage, statusMessageLength, "%s", statusMessageText);
    return true;
}

void AbsSpecial::exitPage(const char *reason) {
    if (specialPageActive) {
        uint8_t defaultSessionRequest[2] = {0x10, 0x01};   // best-effort return to the default session
        UdsRawResult rawResult;
        UdsHandler::rawTransaction(HD_ECU_ABS, defaultSessionRequest, 2, rawResult, 300);
    }
    specialPageActive = false;
    setStatusMessage("ABS special functions closed (%s) - default session restored.", reason ? reason : "exit");
    Serial.printf("[ABS-SPC] Exit: %s\n", reason ? reason : "exit");
}

bool AbsSpecial::run(const char *testKey, int value, bool confirmed, char *outputBuffer, size_t outputBufferLength) {
    lastOperationMs = millis();
    if (!testKey) { snprintf(outputBuffer, outputBufferLength, "{\"ok\":false,\"note\":\"Unknown function\"}"); return false; }

    // The FRONT and REAR bleed/air-purge procedures are interactive guided procedures driven directly through
    // the shared engine at /api/proc/* (proc ids "abs_bleed_front" / "abs_bleed_rear") - there are no native
    // ABS functions here.
    (void)value; (void)confirmed;
    snprintf(outputBuffer, outputBufferLength, "{\"ok\":false,\"note\":\"Unknown function\"}");
    return false;
}

void AbsSpecial::loop() {
    if (!specialPageActive) return;
    if (millis() - lastOperationMs >= ABSS_IDLE_EXIT_MS) exitPage("idle timeout");
}

size_t AbsSpecial::catalogJson(char *outputBuffer, size_t maxOutputLength) {
    if (!outputBuffer || maxOutputLength < 256) return 0;
    int writtenLength = snprintf(outputBuffer, maxOutputLength,
        "{\"type\":\"abs_spc\",\"event\":\"catalog\",\"ok\":true,\"cal\":\"\",\"profile\":0,\"profile_name\":\"ABS\","
        "\"auto\":true,\"profiles\":[],\"functions\":["
        "{\"key\":\"abs_bleed_front\",\"proc\":\"abs_bleed_front\",\"name\":\"ABS bleed - Front circuit\",\"type\":9,\"delay\":0,\"quiet\":true,"
        "\"desc\":\"Guided front brake bleed and ABS air-purge. Follow the on-screen steps - the tool cycles the ABS "
        "pump and valves while you work the front brake lever. Bike on a stand, wheels off the ground, engine off, ignition on.\"},"
        "{\"key\":\"abs_bleed_rear\",\"proc\":\"abs_bleed_rear\",\"name\":\"ABS bleed - Rear circuit\",\"type\":9,\"delay\":0,\"quiet\":true,"
        "\"desc\":\"Guided rear brake bleed and ABS air-purge. Follow the on-screen steps - the tool cycles the ABS "
        "pump and valves while you work the rear brake pedal. Bike on a stand, wheels off the ground, engine off, ignition on.\"}]}");
    return (writtenLength > 0 && (size_t)writtenLength < maxOutputLength) ? (size_t)writtenLength : 0;
}

// ---------------------------------------------------------------------------
// Standalone VIN read/write. Same session/unlock/verify pattern as EcmSpecial::readVin()/writeVin()
// (ecm_special.cpp), but for the ABS (0x7E1) using its own Security Access algorithm (algorithm family
// 0x03, abs_algo.cpp) - the same algorithm the "3" P_UNLOCK mode uses for other ABS procedures.
// ---------------------------------------------------------------------------
static bool absVinSessionActive = false, absVinSessionUnlocked = false;
static char absVinStatusMessage[160] = {0};

static void absVinSetStatus(const char *formatText, ...) __attribute__((format(printf, 1, 2)));
static void absVinSetStatus(const char *formatText, ...) {
    va_list argList; va_start(argList, formatText);
    vsnprintf(absVinStatusMessage, sizeof(absVinStatusMessage), formatText, argList);
    va_end(argList);
}

static const char *absVinNrcText(uint8_t nrc) {
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

static bool absVinTx(const uint8_t *requestBytes, uint8_t requestLength, UdsRawResult &rawResult, uint32_t timeoutMs = 450) {
    UdsHandler::rawTransaction(HD_ECU_ABS, requestBytes, requestLength, rawResult, timeoutMs);
    return rawResult.got_reply && rawResult.positive;
}

static bool absVinEnsureSession() {
    if (absVinSessionActive) return true;
    const uint8_t testerPresentFrame[8] = {0x02, 0x3E, 0x80, 0, 0, 0, 0, 0};
    CanDriver::transmit(HD_ECU_ABS, false, false, 8, testerPresentFrame);
    delay(60);
    const uint8_t extendedSessionRequest[2] = {0x10, 0x03};
    UdsRawResult rawResult;
    for (uint8_t attempt = 0; attempt < 3; attempt++) {
        if (absVinTx(extendedSessionRequest, 2, rawResult)) { absVinSessionActive = true; absVinSessionUnlocked = false; return true; }
        if (rawResult.got_reply) { absVinSetStatus("Extended session refused: %s (NRC 0x%02X)", absVinNrcText(rawResult.nrc), rawResult.nrc); return false; }
        if (attempt < 2) delay(60);
    }
    absVinSetStatus("No response from ABS (0x7E1) to extended session request after 3 tries");
    return false;
}

static bool absVinUnlock() {
    if (absVinSessionUnlocked) return true;
    const uint8_t seedRequest[2] = {0x27, 0x01};
    UdsRawResult rawResult;
    bool seedOk = false;
    for (uint8_t attempt = 0; attempt < 3 && !seedOk; attempt++) {
        if (attempt) delay(80);
        seedOk = absVinTx(seedRequest, 2, rawResult) && rawResult.len >= 2;
        if (!seedOk && rawResult.got_reply) break;
    }
    if (!seedOk) {
        absVinSetStatus("Security access: seed request failed%s", rawResult.got_reply ? " (rejected)" : " (no response after 3 tries)");
        return false;
    }
    uint8_t seedLength = rawResult.len - 2;
    if (seedLength > 8) seedLength = 8;
    uint8_t seedBytes[8]; memcpy(seedBytes, &rawResult.data[2], seedLength);
    bool seedIsZero = true;
    for (uint8_t byteIndex = 0; byteIndex < seedLength; byteIndex++) if (seedBytes[byteIndex]) seedIsZero = false;
    if (seedIsZero) { absVinSessionUnlocked = true; return true; }
    uint8_t keyBytes[8]; uint8_t keyLength = 0;
    if (!absComputeSecurityKey(seedBytes, seedLength, keyBytes, &keyLength)) {
        absVinSetStatus("Security access: could not compute key for seed %02X%02X (see abs_algo.h)", seedBytes[0], seedBytes[1]);
        return false;
    }
    uint8_t keyRequest[10] = {0x27, 0x02};
    memcpy(keyRequest + 2, keyBytes, keyLength);
    UdsRawResult keyResult;
    if (!absVinTx(keyRequest, (uint8_t)(2 + keyLength), keyResult)) {
        absVinSetStatus("Security access: key rejected (NRC 0x%02X)", keyResult.nrc);
        return false;
    }
    absVinSessionUnlocked = true;
    return true;
}

static void absVinCloseSession() {
    if (absVinSessionActive) { const uint8_t defaultSessionRequest[2] = {0x10, 0x01}; UdsRawResult rawResult; absVinTx(defaultSessionRequest, 2, rawResult); }
    absVinSessionActive = false;
    absVinSessionUnlocked = false;
}

bool AbsSpecial::readVin(char *outputBuffer, size_t outputBufferLength) {
    const uint16_t did = EcuInfo::vinDid(HD_ECU_ABS);
    if (!absVinEnsureSession()) { snprintf(outputBuffer, outputBufferLength, "{\"ok\":false,\"msg\":\"%s\"}", absVinStatusMessage); return false; }
    const uint8_t requestBytes[3] = {0x22, (uint8_t)(did >> 8), (uint8_t)(did & 0xFF)};
    UdsRawResult rawResult;
    bool ok = false;
    for (uint8_t attempt = 0; attempt < 3; attempt++) {
        if (attempt) delay(60);
        ok = absVinTx(requestBytes, 3, rawResult);
        if (ok || rawResult.got_reply) break;
    }
    if (!ok || rawResult.len < 3 + 17) {
        absVinSetStatus("VIN read failed: %s", rawResult.got_reply ? absVinNrcText(rawResult.nrc) : "no response after retries");
        absVinCloseSession();
        snprintf(outputBuffer, outputBufferLength, "{\"ok\":false,\"msg\":\"%s\"}", absVinStatusMessage);
        return false;
    }
    char vinText[24] = {0};
    uint8_t vinLength = (uint8_t)(rawResult.len - 3);
    if (vinLength > sizeof(vinText) - 1) vinLength = sizeof(vinText) - 1;
    for (uint8_t byteIndex = 0; byteIndex < vinLength; byteIndex++) {
        uint8_t byteValue = rawResult.data[3 + byteIndex];
        vinText[byteIndex] = (byteValue >= 0x20 && byteValue < 0x7F) ? (char)byteValue : '.';
    }
    vinText[vinLength] = 0;
    absVinSetStatus("Current VIN: %s", vinText);
    absVinCloseSession();
    snprintf(outputBuffer, outputBufferLength, "{\"ok\":true,\"msg\":\"%s\",\"vin\":\"%s\"}", absVinStatusMessage, vinText);
    return true;
}

bool AbsSpecial::writeVin(const char *vin, char *outputBuffer, size_t outputBufferLength) {
    const uint16_t did = EcuInfo::vinDid(HD_ECU_ABS);
    size_t vinLength = vin ? strlen(vin) : 0;
    if (vinLength != 17) {
        absVinSetStatus("VIN must be exactly 17 characters (got %u)", (unsigned)vinLength);
        snprintf(outputBuffer, outputBufferLength, "{\"ok\":false,\"msg\":\"%s\"}", absVinStatusMessage);
        return false;
    }
    if (!absVinEnsureSession() || !absVinUnlock()) {
        absVinCloseSession();
        snprintf(outputBuffer, outputBufferLength, "{\"ok\":false,\"msg\":\"%s\"}", absVinStatusMessage);
        return false;
    }
    uint8_t requestBytes[3 + 17] = {0x2E, (uint8_t)(did >> 8), (uint8_t)(did & 0xFF)};
    memcpy(requestBytes + 3, vin, 17);
    UdsRawResult rawResult;
    if (!absVinTx(requestBytes, (uint8_t)sizeof(requestBytes), rawResult, 900)) {
        absVinSetStatus("VIN write refused: %s (NRC 0x%02X)", absVinNrcText(rawResult.nrc), rawResult.nrc);
        absVinCloseSession();
        snprintf(outputBuffer, outputBufferLength, "{\"ok\":false,\"msg\":\"%s\"}", absVinStatusMessage);
        return false;
    }
    const uint8_t verifyRequest[3] = {0x22, (uint8_t)(did >> 8), (uint8_t)(did & 0xFF)};
    UdsRawResult verifyResult;
    bool verified = absVinTx(verifyRequest, 3, verifyResult) && verifyResult.len >= 3 + 17 && memcmp(verifyResult.data + 3, vin, 17) == 0;
    if (verified) absVinSetStatus("VIN written and verified: %.17s", vin);
    else          absVinSetStatus("VIN write sent, but could not be verified on read-back - check the ABS: %.17s", vin);
    absVinCloseSession();
    snprintf(outputBuffer, outputBufferLength, "{\"ok\":true,\"msg\":\"%s\",\"vin\":\"%s\"}", absVinStatusMessage, vin);
    return true;
}
