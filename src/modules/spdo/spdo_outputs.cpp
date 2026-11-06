/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file spdo_outputs.cpp
 * @brief SPDO output test engine (see spdo_outputs.h).
 */
#include "spdo_outputs.h"
#include "config.h"
#include "uds_handler.h"
#include "can_driver.h"
#include "spdo_algo.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

static const uint32_t SPDOO_MAX_ON_MS_ONOFF  = 60000;    // automatic return of control
static const uint32_t SPDOO_MAX_ON_MS_ADJUST = 120000;
static const uint32_t SPDOO_KEEPALIVE_MS     = 2000;
static const uint32_t SPDOO_STATUS_MS        = 1000;
static const uint32_t SPDOO_POLL_MS          = 1500;     // how often the battery/rpm header reading is refreshed
static const uint32_t SPDOO_ENGINE_RUNNING_RPM = 150;

struct ActuatorState { uint8_t currentState; uint8_t currentStep; uint32_t startTimeMs; };   // currentState: 0 idle, 1 on

static ActuatorState outputStates[SPDO_OUT_KIND_COUNT];
static bool     outputsPageActive = false, sessionActive = false, sessionUnlocked = false, statusDirty = false, profileManuallySet = false;
static int      activeProfileIndex = -1;
static char     partsNumber[48] = {0};
static float    engineRpm = 0, batteryVoltage = 0;
static uint32_t lastTesterPresentMs = 0, lastStatusMs = 0, lastPollMs = 0;
static char     statusMessageText[128] = {0};

static void setStatusMessage(const char *formatText, ...) __attribute__((format(printf, 1, 2)));
static void setStatusMessage(const char *formatText, ...) {
    va_list argList; va_start(argList, formatText);
    vsnprintf(statusMessageText, sizeof(statusMessageText), formatText, argList);
    va_end(argList);
    statusDirty = true;
}

static int hexNibbleValue(char hexChar) {
    if (hexChar >= '0' && hexChar <= '9') return hexChar - '0';
    if (hexChar >= 'A' && hexChar <= 'F') return hexChar - 'A' + 10;
    if (hexChar >= 'a' && hexChar <= 'f') return hexChar - 'a' + 10;
    return -1;
}

// No "##" templating needed here (unlike ecm_outputs.cpp) - every ADJUST step is a literal full command
// already, straight from spdo_outputs_data.h.
static uint8_t hexStringToBytes(const char *hexString, uint8_t *outputBytes, uint8_t maxBytes) {
    uint8_t byteCount = 0;
    for (size_t charIndex = 0; hexString && hexString[charIndex] && hexString[charIndex + 1]; charIndex += 2) {
        int highNibble = hexNibbleValue(hexString[charIndex]), lowNibble = hexNibbleValue(hexString[charIndex + 1]);
        if (highNibble < 0 || lowNibble < 0 || byteCount >= maxBytes) return 0;
        outputBytes[byteCount++] = (uint8_t)((highNibble << 4) | lowNibble);
    }
    return byteCount;
}

static const SpdoOutProfile *activeProfile() {
    return (activeProfileIndex >= 0 && activeProfileIndex < (int)SPDO_OUT_PROFILE_COUNT) ? &SPDO_OUT_PROFILES[activeProfileIndex] : nullptr;
}

static int findOutputKindIndex(const char *testKey) {
    const SpdoOutProfile *profileDef = activeProfile();
    if (!profileDef || !testKey) return -1;
    for (uint8_t kindIndex = 0; kindIndex < profileDef->count; kindIndex++) if (strcmp(SPDO_OUT_KINDS[profileDef->kinds[kindIndex]].key, testKey) == 0) return profileDef->kinds[kindIndex];
    return -1;
}

static int classifyProfileFromPartsNumber(const char *partsNumberString) {
    int matchedProfile = SPDO_OUT_DEFAULT_PROFILE;
#if SPDO_OUT_RULE_COUNT > 0
    size_t bestPrefixLength = 0;
    for (uint16_t ruleIndex = 0; ruleIndex < SPDO_OUT_RULE_COUNT; ruleIndex++) {
        size_t prefixLength = strlen(SPDO_OUT_RULES[ruleIndex].prefix);
        if (prefixLength > bestPrefixLength && strncmp(partsNumberString, SPDO_OUT_RULES[ruleIndex].prefix, prefixLength) == 0) { bestPrefixLength = prefixLength; matchedProfile = SPDO_OUT_RULES[ruleIndex].profile; }
    }
#else
    (void)partsNumberString;      // no rules yet (single profile), see spdo_outputs_data.h
#endif
    return matchedProfile;
}

static const char *nrcDescription(uint8_t nrc) {
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

// Battery voltage / rpm shown in the SPDO output-tests header come from the ECM, same as ABS/BCM outputs -
// the speedometer itself exposes no such DID, so we poll the ECM's live-inputs DID 0x0200 for it.
static bool refreshEngine() {
    EcmInputs0200 ecmInputs;
    if (!UdsHandler::readEcmInputs0200Synchronous(HD_ECU_ECM, ecmInputs, 380)) return false;
    engineRpm = ecmInputs.engine_speed; batteryVoltage = ecmInputs.battery_voltage;
    return true;
}
static bool engineRunning() { return engineRpm >= (float)SPDOO_ENGINE_RUNNING_RPM; }

struct TransmitResult { bool succeeded; bool gotReply; uint8_t nrc; };

// Longest command here is the LCD test (21 data bytes + SID = ~22 bytes), well past the 7-byte single-frame
// limit the BCM/ECM wrapper functions use - UdsHandler::rawTransaction() itself supports up to 60 bytes via
// ISO-TP multi-frame, so only this wrapper's local buffer/length check needs raising, not the UDS engine.
static TransmitResult transmitHexRequest(const char *hexString, UdsRawResult *keepRawResult = nullptr) {
    TransmitResult result = {false, false, 0};
    uint8_t requestBytes[32];
    uint8_t requestLength = hexStringToBytes(hexString, requestBytes, sizeof(requestBytes));
    if (requestLength == 0 || requestLength > 31) return result;
    UdsRawResult rawResult;
    UdsHandler::rawTransaction(HD_ECU_SPEEDO, requestBytes, requestLength, rawResult, 450);
    if (keepRawResult) *keepRawResult = rawResult;
    result.gotReply = rawResult.got_reply;
    result.succeeded = rawResult.got_reply && rawResult.positive;
    result.nrc = rawResult.nrc;
    if (result.succeeded) lastTesterPresentMs = millis();
    return result;
}

static bool ensureSession() {
    if (sessionActive) return true;
    TransmitResult sessionResult = transmitHexRequest("1003");
    if (!sessionResult.succeeded) {
        if (sessionResult.gotReply) setStatusMessage("Extended session refused: %s (NRC 0x%02X)", nrcDescription(sessionResult.nrc), sessionResult.nrc);
        else       setStatusMessage("No response from SPDO (0x7F2) to extended session request");
        return false;
    }
    sessionActive = true; sessionUnlocked = false; lastTesterPresentMs = millis();
    return true;
}

static bool unlock() {
    if (sessionUnlocked) return true;
    UdsRawResult seedRawResult;
    // Seed request: retried up to 3 times on no-response. A negative reply is
    // not retried - e.g. NRC 0x37 'time delay not expired' would only get worse if hammered.
    TransmitResult seedResult = {false, false, 0};
    for (uint8_t attempt = 0; attempt < 3; attempt++) {
        if (attempt) delay(80);
        seedResult = transmitHexRequest("2701", &seedRawResult);
        if ((seedResult.succeeded && seedRawResult.len >= 2) || seedResult.gotReply) break;
    }
    if (!seedResult.succeeded || seedRawResult.len < 2) {
        setStatusMessage("Security access: seed request failed%s", seedResult.gotReply ? " (rejected)" : " (no response after 3 tries)");
        return false;
    }
    uint8_t seedLength = seedRawResult.len - 2;
    bool seedIsZero = true;
    for (uint8_t byteIndex = 0; byteIndex < seedLength; byteIndex++) if (seedRawResult.data[2 + byteIndex]) seedIsZero = false;
    if (seedIsZero) { sessionUnlocked = true; return true; }   // already unlocked

    uint8_t keyBytes[8]; uint8_t keyLength = 0;
    if (!spdoComputeSecurityKey(&seedRawResult.data[2], seedLength, keyBytes, &keyLength) || keyLength == 0 || keyLength > 6) {
        char seedHexText[40] = {0};
        for (uint8_t byteIndex = 0; byteIndex < seedLength && byteIndex < 8; byteIndex++) snprintf(seedHexText + strlen(seedHexText), sizeof(seedHexText) - strlen(seedHexText), "%02X ", seedRawResult.data[2 + byteIndex]);
        setStatusMessage("Security access required - unknown seed %s(no key known for it, see spdo_algo.h)", seedHexText);
        Serial.printf("[SPDO-OUT] Unknown security seed: %s\n", seedHexText);
        return false;
    }
    uint8_t keyRequest[8] = {0x27, 0x02};
    memcpy(keyRequest + 2, keyBytes, keyLength);
    UdsRawResult keyResult;
    UdsHandler::rawTransaction(HD_ECU_SPEEDO, keyRequest, (uint8_t)(2 + keyLength), keyResult, 450);
    if (!(keyResult.got_reply && keyResult.positive)) {
        setStatusMessage("Security access: key rejected (NRC 0x%02X)", keyResult.nrc);
        return false;
    }
    sessionUnlocked = true;
    return true;
}

static bool executeKindRequest(const SpdoOutKind &kindDef, const char *hexString) {
    TransmitResult execResult = transmitHexRequest(hexString);
    if (!execResult.succeeded && execResult.gotReply && execResult.nrc == 0x33 && kindDef.secure && !sessionUnlocked) {
        if (!unlock()) return false;
        execResult = transmitHexRequest(hexString);
    }
    if (!execResult.succeeded) {
        if (execResult.gotReply) setStatusMessage("%s: SPDO rejected the request - %s (NRC 0x%02X)", kindDef.name, nrcDescription(execResult.nrc), execResult.nrc);
        else       setStatusMessage("%s: no response from SPDO", kindDef.name);
        return false;
    }
    return true;
}

static void releaseOutputKind(int kindIndex) {
    const SpdoOutKind &kindDef = SPDO_OUT_KINDS[kindIndex];
    if (kindDef.end1[0]) transmitHexRequest(kindDef.end1);
    if (kindDef.end2[0]) transmitHexRequest(kindDef.end2);
    if (kindDef.end3[0]) transmitHexRequest(kindDef.end3);
    outputStates[kindIndex].currentState = 0;
    outputStates[kindIndex].currentStep = 0;
    statusDirty = true;
}

void SpdoOutputs::reset() {
    memset(outputStates, 0, sizeof(outputStates));
    outputsPageActive = sessionActive = sessionUnlocked = false;
    activeProfileIndex = -1; profileManuallySet = false;
    partsNumber[0] = 0; statusMessageText[0] = 0; engineRpm = 0; batteryVoltage = 0; statusDirty = true;
}
bool SpdoOutputs::pageActive() { return outputsPageActive; }
bool SpdoOutputs::anyOutputActive() {
    for (uint8_t kindIndex = 0; kindIndex < SPDO_OUT_KIND_COUNT; kindIndex++) if (outputStates[kindIndex].currentState) return true;
    return sessionActive;
}

bool SpdoOutputs::enter(char *statusMessage, size_t statusMessageLength) {
    outputsPageActive = true;
    if (!profileManuallySet) {
        char readPartsNumber[48] = {0};
        if (UdsHandler::readFamilyId(HD_ECU_SPEEDO, readPartsNumber, sizeof(readPartsNumber), 450)) {
            strncpy(partsNumber, readPartsNumber, sizeof(partsNumber) - 1); partsNumber[sizeof(partsNumber) - 1] = 0;
            activeProfileIndex = classifyProfileFromPartsNumber(partsNumber);
        } else if (activeProfileIndex < 0) {
            activeProfileIndex = SPDO_OUT_DEFAULT_PROFILE; partsNumber[0] = 0;
        }
    }
    refreshEngine();
    lastPollMs = millis();
    if (activeProfileIndex < 0) setStatusMessage("No output tests are defined for SPDO parts number %s in the database - choose a profile manually", partsNumber[0] ? partsNumber : "?");
    else               setStatusMessage("Outputs ready (%s)", SPDO_OUT_PROFILES[activeProfileIndex].name);
    if (statusMessage && statusMessageLength) snprintf(statusMessage, statusMessageLength, "%s", statusMessageText);
    return true;
}

void SpdoOutputs::returnAll(const char *reason) {
    bool keepPageActive = outputsPageActive;
    exitPage(reason);
    outputsPageActive = keepPageActive;
}

void SpdoOutputs::exitPage(const char *reason) {
    bool hadActiveOutput = false;
    for (uint8_t kindIndex = 0; kindIndex < SPDO_OUT_KIND_COUNT; kindIndex++) if (outputStates[kindIndex].currentState) { releaseOutputKind(kindIndex); hadActiveOutput = true; }
    if (sessionActive || hadActiveOutput) transmitHexRequest("1001");  // back to the default session (also drops security access)
    sessionActive = false; sessionUnlocked = false; outputsPageActive = false;
    setStatusMessage("Outputs closed (%s) - control returned to SPDO", reason ? reason : "exit");
    Serial.printf("[SPDO-OUT] Exit: %s\n", reason ? reason : "exit");
}

bool SpdoOutputs::setProfile(int profileIndex) {
    if (profileIndex < -1 || profileIndex >= (int)SPDO_OUT_PROFILE_COUNT) return false;
    for (uint8_t kindIndex = 0; kindIndex < SPDO_OUT_KIND_COUNT; kindIndex++) if (outputStates[kindIndex].currentState) releaseOutputKind(kindIndex);
    if (profileIndex == -1) { profileManuallySet = false; activeProfileIndex = partsNumber[0] ? classifyProfileFromPartsNumber(partsNumber) : SPDO_OUT_DEFAULT_PROFILE; }
    else         { profileManuallySet = true;  activeProfileIndex = profileIndex; }
    setStatusMessage("Profile %s", activeProfileIndex >= 0 ? SPDO_OUT_PROFILES[activeProfileIndex].name : "none");
    return true;
}

bool SpdoOutputs::command(const char *testKey, const char *operationName, int value, char *statusMessage, size_t statusMessageLength) {
    auto finish = [&](bool succeeded) { if (statusMessage && statusMessageLength) snprintf(statusMessage, statusMessageLength, "%s", statusMessageText); statusDirty = true; return succeeded; };
    int kindIndex = findOutputKindIndex(testKey);
    if (kindIndex < 0 || !operationName) { setStatusMessage("Unknown output"); return finish(false); }
    const SpdoOutKind &kindDef = SPDO_OUT_KINDS[kindIndex];
    ActuatorState &state = outputStates[kindIndex];

    if (strcmp(operationName, "off") == 0 || strcmp(operationName, "stop") == 0) {
        releaseOutputKind(kindIndex);
        setStatusMessage("%s stopped - control returned to SPDO", kindDef.name);
        return finish(true);
    }
    if (kindDef.mode == SPDOO_ONOFF && strcmp(operationName, "on") == 0) {
        if (!ensureSession()) return finish(false);
        if (!executeKindRequest(kindDef, kindDef.on)) return finish(false);
        state.currentState = 1; state.startTimeMs = millis();
        setStatusMessage("%s ON", kindDef.name);
        return finish(true);
    }
    if (kindDef.mode == SPDOO_ADJUST && strcmp(operationName, "set") == 0) {
        if (value < 0 || value >= kindDef.steps) value = 0;
        if (!ensureSession()) return finish(false);
        if (!executeKindRequest(kindDef, kindDef.cmds[value])) return finish(false);
        state.currentStep = (uint8_t)value; state.currentState = 1; state.startTimeMs = millis();
        setStatusMessage("%s set to %d", kindDef.name, kindDef.vals[state.currentStep]);
        return finish(true);
    }
    setStatusMessage("Operation '%s' is not valid for %s", operationName, kindDef.name);
    return finish(false);
}

void SpdoOutputs::loop() {
    if (!outputsPageActive) return;
    const uint32_t now = millis();
    for (uint8_t profilePos = 0; activeProfile() && profilePos < activeProfile()->count; profilePos++) {
        int kindIndex = activeProfile()->kinds[profilePos];
        const SpdoOutKind &kindDef = SPDO_OUT_KINDS[kindIndex];
        ActuatorState &state = outputStates[kindIndex];
        if (!state.currentState) continue;
        uint32_t limitMs = (kindDef.mode == SPDOO_ADJUST) ? SPDOO_MAX_ON_MS_ADJUST : SPDOO_MAX_ON_MS_ONOFF;
        if (now - state.startTimeMs >= limitMs) {
            releaseOutputKind(kindIndex);
            setStatusMessage("%s switched off automatically after %u s", kindDef.name, (unsigned)(limitMs / 1000U));
        }
    }
    if (sessionActive && now - lastTesterPresentMs >= SPDOO_KEEPALIVE_MS) {
        lastTesterPresentMs = now;
        const uint8_t testerPresentFrame[8] = {0x02, 0x3E, 0x80, 0, 0, 0, 0, 0};
        CanDriver::transmit(HD_ECU_SPEEDO, false, false, 8, testerPresentFrame);
    }
    if (now - lastPollMs >= SPDOO_POLL_MS) {
        lastPollMs = now;
        if (refreshEngine()) statusDirty = true;
    }
}

static bool appendf(char *outputBuffer, size_t maxLength, size_t &writtenLength, const char *formatText, ...) __attribute__((format(printf, 4, 5)));
static bool appendf(char *outputBuffer, size_t maxLength, size_t &writtenLength, const char *formatText, ...) {
    va_list argList; va_start(argList, formatText);
    int appendedLength = vsnprintf(outputBuffer + writtenLength, maxLength - writtenLength, formatText, argList);
    va_end(argList);
    if (appendedLength < 0 || (size_t)appendedLength >= maxLength - writtenLength) return false;
    writtenLength += (size_t)appendedLength;
    return true;
}

size_t SpdoOutputs::catalogJson(char *outputBuffer, size_t maxOutputLength) {
    if (!outputBuffer || maxOutputLength < 256) return 0;
    size_t writtenLength = 0;
    if (!appendf(outputBuffer, maxOutputLength, writtenLength, "{\"type\":\"spdo_out\",\"event\":\"catalog\",\"ok\":true,\"cal\":\"%s\",\"profile\":%d,\"profile_name\":\"%s\",\"auto\":%s,\"profiles\":[",
                 partsNumber, activeProfileIndex, activeProfileIndex >= 0 ? SPDO_OUT_PROFILES[activeProfileIndex].name : "none", profileManuallySet ? "false" : "true")) return 0;
    for (int profileIndex = 0; profileIndex < (int)SPDO_OUT_PROFILE_COUNT; profileIndex++)
        if (!appendf(outputBuffer, maxOutputLength, writtenLength, "%s{\"id\":%d,\"name\":\"%s\",\"count\":%u}", profileIndex ? "," : "", profileIndex, SPDO_OUT_PROFILES[profileIndex].name, SPDO_OUT_PROFILES[profileIndex].count)) return 0;
    if (!appendf(outputBuffer, maxOutputLength, writtenLength, "],\"outputs\":[")) return 0;
    const SpdoOutProfile *profileDef = activeProfile();
    for (uint8_t kindPos = 0; profileDef && kindPos < profileDef->count; kindPos++) {
        const SpdoOutKind &kindDef = SPDO_OUT_KINDS[profileDef->kinds[kindPos]];
        if (!appendf(outputBuffer, maxOutputLength, writtenLength, "%s{\"key\":\"%s\",\"name\":\"%s\",\"mode\":%u,\"secure\":%s,\"steps\":[",
                     kindPos ? "," : "", kindDef.key, kindDef.name, kindDef.mode, kindDef.secure ? "true" : "false")) return 0;
        for (uint8_t stepIndex = 0; stepIndex < kindDef.steps; stepIndex++)
            if (!appendf(outputBuffer, maxOutputLength, writtenLength, "%s%d", stepIndex ? "," : "", kindDef.vals[stepIndex])) return 0;
        if (!appendf(outputBuffer, maxOutputLength, writtenLength, "]}")) return 0;
    }
    if (!appendf(outputBuffer, maxOutputLength, writtenLength, "]}")) return 0;
    return writtenLength;
}

size_t SpdoOutputs::statusJson(char *outputBuffer, size_t maxOutputLength) {
    if (!outputBuffer || maxOutputLength < 256) return 0;
    size_t writtenLength = 0;
    if (!appendf(outputBuffer, maxOutputLength, writtenLength, "{\"type\":\"spdo_out\",\"event\":\"status\",\"page\":%s,\"session\":%s,\"unlocked\":%s,\"rpm\":%.0f,\"batt\":%.1f,\"running\":%s,\"msg\":\"%s\",\"outputs\":{",
                 outputsPageActive ? "true" : "false", sessionActive ? "true" : "false", sessionUnlocked ? "true" : "false",
                 (double)engineRpm, (double)batteryVoltage, engineRunning() ? "true" : "false", statusMessageText)) return 0;
    const SpdoOutProfile *profileDef = activeProfile();
    for (uint8_t kindPos = 0; profileDef && kindPos < profileDef->count; kindPos++) {
        int kindIndex = profileDef->kinds[kindPos];
        if (!appendf(outputBuffer, maxOutputLength, writtenLength, "%s\"%s\":{\"s\":%u,\"v\":%u}", kindPos ? "," : "", SPDO_OUT_KINDS[kindIndex].key, outputStates[kindIndex].currentState, outputStates[kindIndex].currentStep)) return 0;
    }
    if (!appendf(outputBuffer, maxOutputLength, writtenLength, "}}")) return 0;
    return writtenLength;
}

bool SpdoOutputs::takeStatusUpdate(char *outputBuffer, size_t maxOutputLength) {
    if (!outputsPageActive) return false;
    uint32_t now = millis();
    if (!statusDirty && now - lastStatusMs < SPDOO_STATUS_MS) return false;
    statusDirty = false; lastStatusMs = now;
    return statusJson(outputBuffer, maxOutputLength) > 0;
}
