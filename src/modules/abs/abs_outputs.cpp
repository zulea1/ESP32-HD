/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file abs_outputs.cpp
 * @brief ABS output test engine (see abs_outputs.h).
 */
#include "abs_outputs.h"
#include "config.h"
#include "uds_handler.h"
#include "can_driver.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

static const uint32_t ABSO_ENGINE_RUNNING_RPM = 150;
static const uint32_t ABSO_MAX_ON_MS    = 60000;
static const uint32_t ABSO_KEEPALIVE_MS = 2000;
static const uint32_t ABSO_POLL_MS      = 1500;
static const uint32_t ABSO_STATUS_MS    = 1000;

struct ActuatorState { uint8_t currentState; uint32_t startTimeMs; };    // currentState: 0 idle, 1 on, 2 pulse running

static ActuatorState outputStates[ABS_OUT_KIND_COUNT];
static bool     outputsPageActive = false, sessionActive = false, statusDirty = false, profileManuallySet = false, haveEngineData = false;
static int      activeProfileIndex = -1;
static char     calibrationId[48] = {0};
static float    engineRpm = 0, batteryVoltage = 0;
static uint32_t lastTesterPresentMs = 0, lastPollMs = 0, lastStatusMs = 0;
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

static uint8_t hexStringToBytes(const char *hexString, uint8_t *outputBytes, uint8_t maxBytes) {
    uint8_t byteCount = 0;
    for (size_t charIndex = 0; hexString && hexString[charIndex] && hexString[charIndex + 1]; charIndex += 2) {
        int highNibble = hexNibbleValue(hexString[charIndex]), lowNibble = hexNibbleValue(hexString[charIndex + 1]);
        if (highNibble < 0 || lowNibble < 0 || byteCount >= maxBytes) return 0;
        outputBytes[byteCount++] = (uint8_t)((highNibble << 4) | lowNibble);
    }
    return byteCount;
}

static const AbsOutProfile *activeProfile() {
    return (activeProfileIndex >= 0 && activeProfileIndex < ABS_OUT_PROFILE_COUNT) ? &ABS_OUT_PROFILES[activeProfileIndex] : nullptr;
}

static int findOutputKindIndex(const char *testKey) {
    const AbsOutProfile *profileDef = activeProfile();
    if (!profileDef || !testKey) return -1;
    for (uint8_t kindIndex = 0; kindIndex < profileDef->count; kindIndex++) if (strcmp(ABS_OUT_KINDS[profileDef->kinds[kindIndex]].key, testKey) == 0) return profileDef->kinds[kindIndex];
    return -1;
}

static int classifyProfileFromCalibration(const char *calibrationIdString) {
    size_t bestPrefixLength = 0; int matchedProfile = ABS_OUT_DEFAULT_PROFILE;
    for (uint16_t ruleIndex = 0; ruleIndex < ABS_OUT_RULE_COUNT; ruleIndex++) {
        size_t prefixLength = strlen(ABS_OUT_RULES[ruleIndex].prefix);
        if (prefixLength > bestPrefixLength && strncmp(calibrationIdString, ABS_OUT_RULES[ruleIndex].prefix, prefixLength) == 0) { bestPrefixLength = prefixLength; matchedProfile = ABS_OUT_RULES[ruleIndex].profile; }
    }
    return matchedProfile;
}

static const char *nrcDescription(uint8_t nrc) {
    switch (nrc) {
        case 0x12: return "sub-function not supported";
        case 0x13: return "incorrect message length";
        case 0x22: return "conditions not correct";
        case 0x31: return "request out of range";
        case 0x33: return "security access denied";
        case 0x7E: case 0x7F: return "not supported in active session";
        default:   return "rejected";
    }
}

struct TransmitResult { bool succeeded; bool gotReply; uint8_t nrc; };

static TransmitResult transmitHexRequest(uint32_t targetModuleId, const char *hexString) {
    TransmitResult result = {false, false, 0};
    uint8_t requestBytes[8];
    uint8_t requestLength = hexStringToBytes(hexString, requestBytes, sizeof(requestBytes));
    if (requestLength == 0 || requestLength > 7) return result;
    UdsRawResult rawResult;
    UdsHandler::rawTransaction(targetModuleId, requestBytes, requestLength, rawResult, 450);
    result.gotReply = rawResult.got_reply; result.succeeded = rawResult.got_reply && rawResult.positive; result.nrc = rawResult.nrc;
    if (result.succeeded && targetModuleId == HD_ECU_ABS) lastTesterPresentMs = millis();
    return result;
}

static bool refreshEngine() {
    EcmInputs0200 ecmInputs;
    if (!UdsHandler::readEcmInputs0200Synchronous(HD_ECU_ECM, ecmInputs, 380)) return false;
    engineRpm = ecmInputs.engine_speed; batteryVoltage = ecmInputs.battery_voltage; haveEngineData = true;
    return true;
}
static bool engineRunning() { return engineRpm >= (float)ABSO_ENGINE_RUNNING_RPM; }

static bool ensureSession() {
    if (sessionActive) return true;
    TransmitResult sessionResult = transmitHexRequest(HD_ECU_ABS, "1003");
    if (!sessionResult.succeeded) {
        if (sessionResult.gotReply) setStatusMessage("Extended session refused: %s (NRC 0x%02X)", nrcDescription(sessionResult.nrc), sessionResult.nrc);
        else       setStatusMessage("No response from ABS (0x7E1) to extended session request");
        return false;
    }
    sessionActive = true; lastTesterPresentMs = millis();
    return true;
}

static bool executeKindRequest(const AbsOutKind &kind, const char *hexString) {
    TransmitResult execResult = transmitHexRequest(HD_ECU_ABS, hexString);
    if (!execResult.succeeded) {
        if (execResult.gotReply) setStatusMessage("%s: ABS rejected the request - %s (NRC 0x%02X)", kind.name, nrcDescription(execResult.nrc), execResult.nrc);
        else       setStatusMessage("%s: no response from ABS", kind.name);
        return false;
    }
    return true;
}

static void releaseOutputKind(int kindIndex) {
    const AbsOutKind &kindDef = ABS_OUT_KINDS[kindIndex];
    if (kindDef.mode == ABSO_PULSE) {
        if (kindDef.ret[0]) transmitHexRequest(HD_ECU_ABS, kindDef.ret);
    } else {
        if (kindDef.end1[0]) transmitHexRequest(HD_ECU_ABS, kindDef.end1);
        if (kindDef.end2[0]) transmitHexRequest(HD_ECU_ABS, kindDef.end2);
    }
    outputStates[kindIndex].currentState = 0;
    statusDirty = true;
}

void AbsOutputs::reset() {
    memset(outputStates, 0, sizeof(outputStates));
    outputsPageActive = sessionActive = false; activeProfileIndex = -1; profileManuallySet = false;
    calibrationId[0] = 0; statusMessageText[0] = 0; haveEngineData = false; statusDirty = true;
}
bool AbsOutputs::pageActive() { return outputsPageActive; }
bool AbsOutputs::anyOutputActive() {
    for (uint8_t kindIndex = 0; kindIndex < ABS_OUT_KIND_COUNT; kindIndex++) if (outputStates[kindIndex].currentState) return true;
    return sessionActive;
}

bool AbsOutputs::enter(char *statusMessage, size_t statusMessageLength) {
    outputsPageActive = true;
    if (!profileManuallySet) {
        char readCalibrationId[48] = {0};
        bool gotCalibrationId = UdsHandler::readDidSynchronous(HD_ECU_ABS, HD_DID_CALIBRATION_F193, readCalibrationId, sizeof(readCalibrationId), 450) ||
                   UdsHandler::readDidSynchronous(HD_ECU_ABS, HD_DID_CALIBRATION_ID, readCalibrationId, sizeof(readCalibrationId), 450);
        if (gotCalibrationId) {
            strncpy(calibrationId, readCalibrationId, sizeof(calibrationId) - 1); calibrationId[sizeof(calibrationId) - 1] = 0;
            activeProfileIndex = classifyProfileFromCalibration(calibrationId);
        } else if (activeProfileIndex < 0) {
            activeProfileIndex = ABS_OUT_DEFAULT_PROFILE; calibrationId[0] = 0;
        }
    }
    refreshEngine();
    lastPollMs = millis();
    if (activeProfileIndex < 0) setStatusMessage("No output tests are defined for calibration %s in the database - choose a profile manually", calibrationId[0] ? calibrationId : "?");
    else               setStatusMessage("Outputs ready (%s)", ABS_OUT_PROFILES[activeProfileIndex].name);
    if (statusMessage && statusMessageLength) snprintf(statusMessage, statusMessageLength, "%s", statusMessageText);
    return true;
}

void AbsOutputs::returnAll(const char *reason) {
    bool keepPageActive = outputsPageActive;
    exitPage(reason);
    outputsPageActive = keepPageActive; lastPollMs = millis();
}

void AbsOutputs::exitPage(const char *reason) {
    bool hadActiveOutput = false;
    for (uint8_t kindIndex = 0; kindIndex < ABS_OUT_KIND_COUNT; kindIndex++) if (outputStates[kindIndex].currentState) { releaseOutputKind(kindIndex); hadActiveOutput = true; }
    if (sessionActive || hadActiveOutput) transmitHexRequest(HD_ECU_ABS, "1001");
    sessionActive = false; outputsPageActive = false;
    setStatusMessage("Outputs closed (%s) - control returned to ABS", reason ? reason : "exit");
    Serial.printf("[ABS-OUT] Exit: %s\n", reason ? reason : "exit");
}

bool AbsOutputs::setProfile(int profileIndex) {
    if (profileIndex < -1 || profileIndex >= ABS_OUT_PROFILE_COUNT) return false;
    for (uint8_t kindIndex = 0; kindIndex < ABS_OUT_KIND_COUNT; kindIndex++) if (outputStates[kindIndex].currentState) releaseOutputKind(kindIndex);
    if (profileIndex == -1) { profileManuallySet = false; activeProfileIndex = calibrationId[0] ? classifyProfileFromCalibration(calibrationId) : ABS_OUT_DEFAULT_PROFILE; }
    else         { profileManuallySet = true;  activeProfileIndex = profileIndex; }
    setStatusMessage("Profile %s", activeProfileIndex >= 0 ? ABS_OUT_PROFILES[activeProfileIndex].name : "none");
    return true;
}

bool AbsOutputs::command(const char *testKey, const char *operationName, char *statusMessage, size_t statusMessageLength) {
    auto finish = [&](bool succeeded) { if (statusMessage && statusMessageLength) snprintf(statusMessage, statusMessageLength, "%s", statusMessageText); statusDirty = true; return succeeded; };
    int kindIndex = findOutputKindIndex(testKey);
    if (kindIndex < 0 || !operationName) { setStatusMessage("Unknown output"); return finish(false); }
    const AbsOutKind &kindDef = ABS_OUT_KINDS[kindIndex];
    ActuatorState &state = outputStates[kindIndex];

    if (strcmp(operationName, "off") == 0 || strcmp(operationName, "stop") == 0) {
        releaseOutputKind(kindIndex);
        setStatusMessage("%s stopped - control returned to ABS", kindDef.name);
        return finish(true);
    }
    if (kindDef.engine_off) {
        if (!refreshEngine()) { setStatusMessage("Communication error: cannot read engine speed from the ECM - %s not started", kindDef.name); return finish(false); }
        if (engineRunning()) { setStatusMessage("Cannot execute test with engine running (%.0f rpm)", (double)engineRpm); return finish(false); }
    } else {
        refreshEngine();    // informational (header), failure is fine
    }
    if (kindDef.mode == ABSO_PULSE && strcmp(operationName, "fire") == 0) {
        if (state.currentState) { setStatusMessage("%s already running", kindDef.name); return finish(false); }
        if (!ensureSession()) return finish(false);
        if (!executeKindRequest(kindDef, kindDef.start)) return finish(false);
        state.currentState = 2; state.startTimeMs = millis();
        setStatusMessage("%s running (%u s)", kindDef.name, (unsigned)kindDef.delay_s);
        return finish(true);
    }
    if (kindDef.mode == ABSO_ONOFF && strcmp(operationName, "on") == 0) {
        if (!ensureSession()) return finish(false);
        if (!executeKindRequest(kindDef, kindDef.on)) return finish(false);
        state.currentState = 1; state.startTimeMs = millis();
        setStatusMessage("%s ON", kindDef.name);
        return finish(true);
    }
    setStatusMessage("Operation '%s' is not valid for %s", operationName, kindDef.name);
    return finish(false);
}

void AbsOutputs::loop() {
    if (!outputsPageActive) return;
    const uint32_t now = millis();
    for (uint8_t profilePos = 0; activeProfile() && profilePos < activeProfile()->count; profilePos++) {
        int kindIndex = activeProfile()->kinds[profilePos];
        const AbsOutKind &kindDef = ABS_OUT_KINDS[kindIndex];
        ActuatorState &state = outputStates[kindIndex];
        if (!state.currentState) continue;
        if (kindDef.mode == ABSO_PULSE && state.currentState == 2 && now - state.startTimeMs >= (uint32_t)kindDef.delay_s * 1000U) {
            releaseOutputKind(kindIndex); setStatusMessage("%s finished - control returned to ABS", kindDef.name);
        } else if (kindDef.mode == ABSO_ONOFF && now - state.startTimeMs >= ABSO_MAX_ON_MS) {
            releaseOutputKind(kindIndex); setStatusMessage("%s switched off automatically after %u s", kindDef.name, (unsigned)(ABSO_MAX_ON_MS / 1000U));
        }
    }
    if (sessionActive && now - lastTesterPresentMs >= ABSO_KEEPALIVE_MS) {
        lastTesterPresentMs = now;
        const uint8_t testerPresentFrame[8] = {0x02, 0x3E, 0x80, 0, 0, 0, 0, 0};
        CanDriver::transmit(HD_ECU_ABS, false, false, 8, testerPresentFrame);
    }
    if (now - lastPollMs >= ABSO_POLL_MS) {
        lastPollMs = now;
        if (refreshEngine()) {
            statusDirty = true;
            if (engineRunning()) {
                bool stoppedAny = false;
                for (uint8_t profilePos = 0; activeProfile() && profilePos < activeProfile()->count; profilePos++) {
                    int kindIndex = activeProfile()->kinds[profilePos];
                    if (outputStates[kindIndex].currentState && ABS_OUT_KINDS[kindIndex].engine_off) { releaseOutputKind(kindIndex); stoppedAny = true; }
                }
                if (stoppedAny) setStatusMessage("Engine started - engine-off tests stopped, control returned to ABS");
            }
        }
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

size_t AbsOutputs::catalogJson(char *outputBuffer, size_t maxOutputLength) {
    if (!outputBuffer || maxOutputLength < 256) return 0;
    size_t writtenLength = 0;
    if (!appendf(outputBuffer, maxOutputLength, writtenLength, "{\"type\":\"abs_out\",\"event\":\"catalog\",\"ok\":true,\"cal\":\"%s\",\"profile\":%d,\"profile_name\":\"%s\",\"auto\":%s,\"profiles\":[",
                 calibrationId, activeProfileIndex, activeProfileIndex >= 0 ? ABS_OUT_PROFILES[activeProfileIndex].name : "none", profileManuallySet ? "false" : "true")) return 0;
    for (int profileIndex = 0; profileIndex < ABS_OUT_PROFILE_COUNT; profileIndex++)
        if (!appendf(outputBuffer, maxOutputLength, writtenLength, "%s{\"id\":%d,\"name\":\"%s\",\"count\":%u}", profileIndex ? "," : "", profileIndex, ABS_OUT_PROFILES[profileIndex].name, ABS_OUT_PROFILES[profileIndex].count)) return 0;
    if (!appendf(outputBuffer, maxOutputLength, writtenLength, "],\"outputs\":[")) return 0;
    const AbsOutProfile *profileDef = activeProfile();
    for (uint8_t kindPos = 0; profileDef && kindPos < profileDef->count; kindPos++) {
        const AbsOutKind &kindDef = ABS_OUT_KINDS[profileDef->kinds[kindPos]];
        if (!appendf(outputBuffer, maxOutputLength, writtenLength, "%s{\"key\":\"%s\",\"name\":\"%s\",\"mode\":%u,\"secure\":false,\"engine_on\":false,\"engine_off\":%s,\"delay\":%u,\"steps\":[],\"warn\":\"%s\"}",
                     kindPos ? "," : "", kindDef.key, kindDef.name, kindDef.mode, kindDef.engine_off ? "true" : "false", kindDef.delay_s, kindDef.warn)) return 0;
    }
    if (!appendf(outputBuffer, maxOutputLength, writtenLength, "]}")) return 0;
    return writtenLength;
}

size_t AbsOutputs::statusJson(char *outputBuffer, size_t maxOutputLength) {
    if (!outputBuffer || maxOutputLength < 256) return 0;
    size_t writtenLength = 0;
    if (!appendf(outputBuffer, maxOutputLength, writtenLength, "{\"type\":\"abs_out\",\"event\":\"status\",\"page\":%s,\"session\":%s,\"unlocked\":false,\"rpm\":%.0f,\"batt\":%.1f,\"running\":%s,\"msg\":\"%s\",\"outputs\":{",
                 outputsPageActive ? "true" : "false", sessionActive ? "true" : "false", (double)engineRpm, (double)batteryVoltage, engineRunning() ? "true" : "false", statusMessageText)) return 0;
    const AbsOutProfile *profileDef = activeProfile();
    for (uint8_t kindPos = 0; profileDef && kindPos < profileDef->count; kindPos++) {
        int kindIndex = profileDef->kinds[kindPos];
        if (!appendf(outputBuffer, maxOutputLength, writtenLength, "%s\"%s\":{\"s\":%u,\"v\":0}", kindPos ? "," : "", ABS_OUT_KINDS[kindIndex].key, outputStates[kindIndex].currentState)) return 0;
    }
    if (!appendf(outputBuffer, maxOutputLength, writtenLength, "}}")) return 0;
    return writtenLength;
}

bool AbsOutputs::takeStatusUpdate(char *outputBuffer, size_t maxOutputLength) {
    if (!outputsPageActive) return false;
    uint32_t now = millis();
    if (!statusDirty && now - lastStatusMs < ABSO_STATUS_MS) return false;
    statusDirty = false; lastStatusMs = now;
    return statusJson(outputBuffer, maxOutputLength) > 0;
}
