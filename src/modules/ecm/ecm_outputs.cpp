/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file ecm_outputs.cpp
 * @brief ECM output test engine (see ecm_outputs.h).
 */
#include "ecm_outputs.h"
#include "config.h"
#include "uds_handler.h"
#include "can_driver.h"
#include "ecm_algo.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// ---------------------------------------------------------------------------
// Limits & thresholds
// ---------------------------------------------------------------------------
static const uint32_t ECMO_ENGINE_RUNNING_RPM = 150;     // database threshold: >=150 = engine running
static const uint32_t ECMO_MAX_ON_MS_ONOFF    = 60000;   // automatic return of control
static const uint32_t ECMO_MAX_ON_MS_ADJUST   = 120000;
static const uint32_t ECMO_KEEPALIVE_MS       = 2000;
static const uint32_t ECMO_POLL_MS            = 1500;
static const uint32_t ECMO_STATUS_MS          = 1000;
static const float    ECMO_LOW_BATT_V         = 11.8f;

struct ActuatorState { uint8_t currentState; uint8_t currentStep; uint32_t startTimeMs; };   // currentState: 0 idle, 1 on, 2 pulse running

static ActuatorState outputStates[ECM_OUT_KIND_COUNT];
static bool      outputsPageActive = false, sessionActive = false, sessionUnlocked = false, statusDirty = false;
static int       activeProfileIndex = -1;            // active profile (-1 none)
static bool      profileManuallySet = false;
static char      calibrationId[48] = {0};
static float     engineRpm = 0, batteryVoltage = 0;
static bool      haveEngineData = false;
static uint32_t  lastTesterPresentMs = 0, lastPollMs = 0, lastStatusMs = 0;
static char      statusMessageText[128] = {0};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
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

// Parses "2F05020380##" into bytes; '##' is replaced with stepHex (2 hex digits). Returns length (0 = error).
static uint8_t hexStringToBytes(const char *hexString, const char *stepHex, uint8_t *outputBytes, uint8_t maxBytes) {
    uint8_t byteCount = 0;
    for (size_t charIndex = 0; hexString && hexString[charIndex] && hexString[charIndex + 1]; charIndex += 2) {
        if (byteCount >= maxBytes) return 0;
        const char *pairPtr = hexString + charIndex;
        if (pairPtr[0] == '#' && pairPtr[1] == '#') {
            if (!stepHex || hexNibbleValue(stepHex[0]) < 0 || hexNibbleValue(stepHex[1]) < 0) return 0;
            pairPtr = stepHex;
        }
        int highNibble = hexNibbleValue(pairPtr[0]), lowNibble = hexNibbleValue(pairPtr[1]);
        if (highNibble < 0 || lowNibble < 0) return 0;
        outputBytes[byteCount++] = (uint8_t)((highNibble << 4) | lowNibble);
    }
    return byteCount;
}

static const EcmOutProfile *activeProfile() {
    return (activeProfileIndex >= 0 && activeProfileIndex < ECM_OUT_PROFILE_COUNT) ? &ECM_OUT_PROFILES[activeProfileIndex] : nullptr;
}

// Index of the kind with this key in the active profile (-1 if not available)
static int findOutputKindIndex(const char *testKey) {
    const EcmOutProfile *profileDef = activeProfile();
    if (!profileDef || !testKey) return -1;
    for (uint8_t kindIndex = 0; kindIndex < profileDef->count; kindIndex++) {
        if (strcmp(ECM_OUT_KINDS[profileDef->kinds[kindIndex]].key, testKey) == 0) return profileDef->kinds[kindIndex];
    }
    return -1;
}

static int classifyProfileFromCalibration(const char *calibrationIdString) {
    size_t bestPrefixLength = 0;
    int matchedProfile = ECM_OUT_DEFAULT_PROFILE;
    for (uint16_t ruleIndex = 0; ruleIndex < ECM_OUT_CAL_RULE_COUNT; ruleIndex++) {
        size_t prefixLength = strlen(ECM_OUT_CAL_RULES[ruleIndex].prefix);
        if (prefixLength > bestPrefixLength && strncmp(calibrationIdString, ECM_OUT_CAL_RULES[ruleIndex].prefix, prefixLength) == 0) {
            bestPrefixLength = prefixLength;
            matchedProfile = ECM_OUT_CAL_RULES[ruleIndex].profile;
        }
    }
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
        case 0x7E: return "not supported in active session";
        case 0x7F: return "service not supported in active session";
        default:   return "rejected";
    }
}

struct TransmitResult { bool succeeded; bool gotReply; uint8_t nrc; };

static TransmitResult transmitHexRequest(const char *hexString, const char *stepHex, UdsRawResult *keepRawResult = nullptr) {
    TransmitResult result = {false, false, 0};
    uint8_t requestBytes[8];
    uint8_t requestLength = hexStringToBytes(hexString, stepHex, requestBytes, sizeof(requestBytes));
    if (requestLength == 0 || requestLength > 7) return result;
    UdsRawResult rawResult;
    UdsHandler::rawTransaction(HD_ECU_ECM, requestBytes, requestLength, rawResult, 450);
    if (keepRawResult) *keepRawResult = rawResult;
    result.gotReply = rawResult.got_reply;
    result.succeeded  = rawResult.got_reply && rawResult.positive;
    result.nrc = rawResult.nrc;
    if (result.succeeded) lastTesterPresentMs = millis();
    return result;
}

static bool refreshEngine() {
    EcmInputs0200 ecmInputs;
    if (!UdsHandler::readEcmInputs0200Synchronous(HD_ECU_ECM, ecmInputs, 380)) return false;
    engineRpm = ecmInputs.engine_speed;
    batteryVoltage = ecmInputs.battery_voltage;
    haveEngineData = true;
    return true;
}

static bool engineRunning() { return engineRpm >= (float)ECMO_ENGINE_RUNNING_RPM; }

static bool ensureSession() {
    if (sessionActive) return true;
    TransmitResult sessionResult = transmitHexRequest("1003", nullptr);
    if (!sessionResult.succeeded) {
        if (sessionResult.gotReply) setStatusMessage("Extended session refused: %s (NRC 0x%02X)", nrcDescription(sessionResult.nrc), sessionResult.nrc);
        else       setStatusMessage("No response from ECM (0x7E0) to extended session request");
        return false;
    }
    sessionActive = true;
    sessionUnlocked = false;
    lastTesterPresentMs = millis();
    return true;
}

static bool unlock() {
    if (sessionUnlocked) return true;
    // The database reads these identification DIDs before requesting the seed.
    UdsRawResult f1eaRaw{}; transmitHexRequest("22F1EA", nullptr, &f1eaRaw);
    UdsRawResult f1f4Raw{}; transmitHexRequest("22F1F4", nullptr, &f1f4Raw);   // ECM key-flag source: F1F4 byte index 1
    UdsRawResult f1ecRaw{}; transmitHexRequest("22F1EC", nullptr, &f1ecRaw);
    const uint8_t ecmFlag = (f1f4Raw.len >= 5) ? f1f4Raw.data[4] : 0x00;     // b1 of 62 F1 F4 b0 b1 b2 b3
    // Table 1 vs table 2 pick, from the three identification responses.
    const uint8_t ecmTable = ecmSelectSecurityTable(f1eaRaw.data, f1eaRaw.len,
                                                    f1f4Raw.data, f1f4Raw.len,
                                                    f1ecRaw.data, f1ecRaw.len);
    UdsRawResult seedRawResult;
    // Seed request: retried up to 3 times on no-response. A negative reply is
    // not retried - e.g. NRC 0x37 'time delay not expired' would only get worse if hammered.
    TransmitResult seedResult = {false, false, 0};
    for (uint8_t attempt = 0; attempt < 3; attempt++) {
        if (attempt) delay(80);
        seedResult = transmitHexRequest("2701", nullptr, &seedRawResult);
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
    if (!ecmComputeSecurityKeyTable(ecmTable, ecmFlag, &seedRawResult.data[2], seedLength, keyBytes, &keyLength) || keyLength == 0 || keyLength > 6) {
        char seedHexText[40] = {0};
        for (uint8_t byteIndex = 0; byteIndex < seedLength && byteIndex < 8; byteIndex++) snprintf(seedHexText + strlen(seedHexText), sizeof(seedHexText) - strlen(seedHexText), "%02X ", seedRawResult.data[2 + byteIndex]);
        setStatusMessage("Security access: could not compute key for seed %s(see ecm_algo.h)", seedHexText);
        Serial.printf("[ECM-OUT] Unknown security seed: %s\n", seedHexText);
        return false;
    }
    uint8_t keyRequest[8] = {0x27, 0x02};
    memcpy(keyRequest + 2, keyBytes, keyLength);
    UdsRawResult keyResult;
    UdsHandler::rawTransaction(HD_ECU_ECM, keyRequest, (uint8_t)(2 + keyLength), keyResult, 450);
    if (!(keyResult.got_reply && keyResult.positive)) {
        setStatusMessage("Security access: key rejected (NRC 0x%02X)", keyResult.nrc);
        return false;
    }
    sessionUnlocked = true;
    return true;
}

// Sends one request of a kind. On "security access denied" for a protected test: unlock once and retry.
static bool executeKindRequest(const EcmOutKind &kindDef, const char *hexString, const char *stepHex) {
    TransmitResult execResult = transmitHexRequest(hexString, stepHex);
    if (!execResult.succeeded && execResult.gotReply && execResult.nrc == 0x33 && kindDef.secure && !sessionUnlocked) {
        if (!unlock()) return false;
        execResult = transmitHexRequest(hexString, stepHex);
    }
    if (!execResult.succeeded) {
        if (execResult.gotReply) setStatusMessage("%s: ECU rejected the request - %s (NRC 0x%02X)", kindDef.name, nrcDescription(execResult.nrc), execResult.nrc);
        else       setStatusMessage("%s: no response from ECM", kindDef.name);
        return false;
    }
    return true;
}

// Return control to the ECU for one kind (end sequence, no session reset)
static void releaseOutputKind(int kindIndex) {
    const EcmOutKind &kindDef = ECM_OUT_KINDS[kindIndex];
    if (kindDef.mode == ECMO_PULSE) {
        if (kindDef.ret[0]) transmitHexRequest(kindDef.ret, nullptr);
    } else {
        if (kindDef.end1[0]) transmitHexRequest(kindDef.end1, nullptr);
        if (kindDef.end2[0]) transmitHexRequest(kindDef.end2, nullptr);
    }
    outputStates[kindIndex].currentState = 0;
    outputStates[kindIndex].currentStep = 0;
    statusDirty = true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void EcmOutputs::reset() {
    memset(outputStates, 0, sizeof(outputStates));
    outputsPageActive = sessionActive = sessionUnlocked = false;
    activeProfileIndex = -1; profileManuallySet = false;
    calibrationId[0] = 0; statusMessageText[0] = 0; haveEngineData = false;
    statusDirty = true;
}

bool EcmOutputs::pageActive() { return outputsPageActive; }

bool EcmOutputs::anyOutputActive() {
    for (uint8_t kindIndex = 0; kindIndex < ECM_OUT_KIND_COUNT; kindIndex++) if (outputStates[kindIndex].currentState) return true;
    return sessionActive;
}

bool EcmOutputs::enter(char *statusMessage, size_t statusMessageLength) {
    outputsPageActive = true;
    if (!profileManuallySet) {
        char readCalibrationId[48] = {0};
        bool gotCalibrationId = UdsHandler::readDidSynchronous(HD_ECU_ECM, HD_DID_CALIBRATION_F193, readCalibrationId, sizeof(readCalibrationId), 450) ||
                   UdsHandler::readDidSynchronous(HD_ECU_ECM, HD_DID_CALIBRATION_ID, readCalibrationId, sizeof(readCalibrationId), 450);
        if (gotCalibrationId) {
            strncpy(calibrationId, readCalibrationId, sizeof(calibrationId) - 1);
            calibrationId[sizeof(calibrationId) - 1] = 0;
            activeProfileIndex = classifyProfileFromCalibration(calibrationId);
        } else if (activeProfileIndex < 0) {
            activeProfileIndex = ECM_OUT_DEFAULT_PROFILE;
            calibrationId[0] = 0;
        }
    }
    refreshEngine();
    lastPollMs = millis();
    if (activeProfileIndex < 0) setStatusMessage("No output tests are defined for calibration %s in the database - choose a profile manually", calibrationId[0] ? calibrationId : "?");
    else               setStatusMessage("Outputs ready (profile %s)", ECM_OUT_PROFILES[activeProfileIndex].name);
    if (statusMessage && statusMessageLength) snprintf(statusMessage, statusMessageLength, "%s", statusMessageText);
    return true;
}

void EcmOutputs::returnAll(const char *reason) {
    bool keepPageActive = outputsPageActive;
    exitPage(reason);
    outputsPageActive = keepPageActive;
    lastPollMs = millis();
}

void EcmOutputs::exitPage(const char *reason) {
    bool hadActiveOutput = false;
    for (uint8_t kindIndex = 0; kindIndex < ECM_OUT_KIND_COUNT; kindIndex++) {
        if (outputStates[kindIndex].currentState) { releaseOutputKind(kindIndex); hadActiveOutput = true; }
    }
    if (sessionActive || hadActiveOutput) {
        transmitHexRequest("1001", nullptr);                 // back to the default session (also drops security access)
    }
    sessionActive = false;
    sessionUnlocked = false;
    outputsPageActive = false;
    setStatusMessage("Outputs closed (%s) - control returned to ECU", reason ? reason : "exit");
    Serial.printf("[ECM-OUT] Exit: %s\n", reason ? reason : "exit");
}

bool EcmOutputs::setProfile(int profileIndex) {
    if (profileIndex < -1 || profileIndex >= ECM_OUT_PROFILE_COUNT) return false;
    // return control of anything running under the previous profile first
    for (uint8_t kindIndex = 0; kindIndex < ECM_OUT_KIND_COUNT; kindIndex++) if (outputStates[kindIndex].currentState) releaseOutputKind(kindIndex);
    if (profileIndex == -1) {
        profileManuallySet = false;
        activeProfileIndex = calibrationId[0] ? classifyProfileFromCalibration(calibrationId) : ECM_OUT_DEFAULT_PROFILE;
    } else {
        profileManuallySet = true;
        activeProfileIndex = profileIndex;
    }
    setStatusMessage("Profile %s", activeProfileIndex >= 0 ? ECM_OUT_PROFILES[activeProfileIndex].name : "none");
    return true;
}

bool EcmOutputs::command(const char *testKey, const char *operationName, int value, bool neutralOk, char *statusMessage, size_t statusMessageLength) {
    auto finish = [&](bool succeeded) { if (statusMessage && statusMessageLength) snprintf(statusMessage, statusMessageLength, "%s", statusMessageText); statusDirty = true; return succeeded; };
    int kindIndex = findOutputKindIndex(testKey);
    if (kindIndex < 0 || !operationName) { setStatusMessage("Unknown output"); return finish(false); }
    const EcmOutKind &kindDef = ECM_OUT_KINDS[kindIndex];
    ActuatorState &state = outputStates[kindIndex];
    const bool isStopOperation = (strcmp(operationName, "off") == 0 || strcmp(operationName, "stop") == 0);

    if (isStopOperation) {
        releaseOutputKind(kindIndex);
        setStatusMessage("%s stopped - control returned to ECU", kindDef.name);
        return finish(true);
    }

    // ---- start-type operations: interlocks ----
    if (!neutralOk) { setStatusMessage("Confirm that the gearbox is in neutral first"); return finish(false); }
    if (!refreshEngine()) { setStatusMessage("Cannot read engine speed (DID 0x0200) - interlock unavailable"); return finish(false); }
    if (kindDef.engine_on) {
        if (!engineRunning()) { setStatusMessage("%s needs the engine running", kindDef.name); return finish(false); }
    } else if (engineRunning()) {
        setStatusMessage("Cannot execute test with engine running (%.0f rpm)", (double)engineRpm);
        return finish(false);
    }
    if (batteryVoltage > 1.0f && batteryVoltage < ECMO_LOW_BATT_V) {
        setStatusMessage("Battery voltage too low (%.1f V < %.1f V)", (double)batteryVoltage, (double)ECMO_LOW_BATT_V);
        return finish(false);
    }

    if (kindDef.mode == ECMO_PULSE && strcmp(operationName, "fire") == 0) {
        if (state.currentState) { setStatusMessage("%s already running", kindDef.name); return finish(false); }
        if (!ensureSession()) return finish(false);
        if (!executeKindRequest(kindDef, kindDef.start, nullptr)) return finish(false);
        state.currentState = 2; state.startTimeMs = millis();
        setStatusMessage("%s running (%u s)", kindDef.name, (unsigned)kindDef.delay_s);
        return finish(true);
    }
    if (kindDef.mode == ECMO_ONOFF && strcmp(operationName, "on") == 0) {
        if (!ensureSession()) return finish(false);
        if (!executeKindRequest(kindDef, kindDef.on, nullptr)) return finish(false);
        state.currentState = 1; state.startTimeMs = millis();
        setStatusMessage("%s ON", kindDef.name);
        return finish(true);
    }
    if (kindDef.mode == ECMO_ADJUST && (strcmp(operationName, "arm") == 0 || strcmp(operationName, "set") == 0)) {
        if (value < 0 || value >= kindDef.steps) value = 0;
        if (strcmp(operationName, "arm") == 0) {
            if (!executeKindRequest(kindDef, kindDef.start, nullptr)) return finish(false);
            state.currentStep = 0;
        } else {
            if (!executeKindRequest(kindDef, kindDef.on, kindDef.cmds[value])) return finish(false);
            state.currentStep = (uint8_t)value;
        }
        if (state.currentState == 0) state.startTimeMs = millis();
        state.currentState = 1;
        setStatusMessage("%s %s %d", kindDef.name, strcmp(operationName, "arm") == 0 ? "armed, value" : "set to", kindDef.vals[state.currentStep]);
        return finish(true);
    }
    setStatusMessage("Operation '%s' is not valid for %s", operationName, kindDef.name);
    return finish(false);
}

void EcmOutputs::loop() {
    if (!outputsPageActive) return;
    const uint32_t now = millis();

    // Pulse completion / automatic time limits
    for (uint8_t profilePos = 0; activeProfile() && profilePos < activeProfile()->count; profilePos++) {
        int kindIndex = activeProfile()->kinds[profilePos];
        const EcmOutKind &kindDef = ECM_OUT_KINDS[kindIndex];
        ActuatorState &state = outputStates[kindIndex];
        if (!state.currentState) continue;
        if (kindDef.mode == ECMO_PULSE && state.currentState == 2 && now - state.startTimeMs >= (uint32_t)kindDef.delay_s * 1000U) {
            releaseOutputKind(kindIndex);
            setStatusMessage("%s finished - control returned to ECU", kindDef.name);
        } else if (kindDef.mode == ECMO_ONOFF && now - state.startTimeMs >= ECMO_MAX_ON_MS_ONOFF) {
            releaseOutputKind(kindIndex);
            setStatusMessage("%s switched off automatically after %u s", kindDef.name, (unsigned)(ECMO_MAX_ON_MS_ONOFF / 1000U));
        } else if (kindDef.mode == ECMO_ADJUST && now - state.startTimeMs >= ECMO_MAX_ON_MS_ADJUST) {
            releaseOutputKind(kindIndex);
            setStatusMessage("%s stopped automatically after %u s", kindDef.name, (unsigned)(ECMO_MAX_ON_MS_ADJUST / 1000U));
        }
    }

    // Session keep-alive: 02 3E 80 (suppress positive response)
    if (sessionActive && now - lastTesterPresentMs >= ECMO_KEEPALIVE_MS) {
        lastTesterPresentMs = now;
        const uint8_t testerPresentFrame[8] = {0x02, 0x3E, 0x80, 0, 0, 0, 0, 0};
        CanDriver::transmit(HD_ECU_ECM, false, false, 8, testerPresentFrame);
    }

    // Engine speed / battery refresh + safety: engine started during an engine-off test
    if (now - lastPollMs >= ECMO_POLL_MS) {
        lastPollMs = now;
        if (refreshEngine()) {
            statusDirty = true;
            if (engineRunning()) {
                bool stoppedAny = false;
                for (uint8_t profilePos = 0; activeProfile() && profilePos < activeProfile()->count; profilePos++) {
                    int kindIndex = activeProfile()->kinds[profilePos];
                    if (outputStates[kindIndex].currentState && !ECM_OUT_KINDS[kindIndex].engine_on) { releaseOutputKind(kindIndex); stoppedAny = true; }
                }
                if (stoppedAny) setStatusMessage("Engine started - engine-off tests stopped, control returned to ECU");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------
size_t EcmOutputs::catalogJson(char *outputBuffer, size_t maxOutputLength) {
    if (!outputBuffer || maxOutputLength < 256) return 0;
    size_t writtenLength = (size_t)snprintf(outputBuffer, maxOutputLength,
        "{\"type\":\"ecm_out\",\"event\":\"catalog\",\"ok\":true,\"cal\":\"%s\",\"profile\":%d,\"profile_name\":\"%s\","
        "\"auto\":%s,\"profiles\":[",
        calibrationId, activeProfileIndex, activeProfileIndex >= 0 ? ECM_OUT_PROFILES[activeProfileIndex].name : "none", profileManuallySet ? "false" : "true");
    if (writtenLength >= maxOutputLength) return 0;
    for (int profileIndex = 0; profileIndex < ECM_OUT_PROFILE_COUNT; profileIndex++) {
        int appendedLength = snprintf(outputBuffer + writtenLength, maxOutputLength - writtenLength, "%s{\"id\":%d,\"name\":\"%s\",\"count\":%u}", profileIndex ? "," : "", profileIndex, ECM_OUT_PROFILES[profileIndex].name, ECM_OUT_PROFILES[profileIndex].count);
        if (appendedLength < 0 || (size_t)appendedLength >= maxOutputLength - writtenLength) return 0;
        writtenLength += (size_t)appendedLength;
    }
    int outputsHeaderLength = snprintf(outputBuffer + writtenLength, maxOutputLength - writtenLength, "],\"outputs\":[");
    if (outputsHeaderLength < 0 || (size_t)outputsHeaderLength >= maxOutputLength - writtenLength) return 0;
    writtenLength += (size_t)outputsHeaderLength;
    const EcmOutProfile *profileDef = activeProfile();
    for (uint8_t kindPos = 0; profileDef && kindPos < profileDef->count; kindPos++) {
        const EcmOutKind &kindDef = ECM_OUT_KINDS[profileDef->kinds[kindPos]];
        int appendedLength = snprintf(outputBuffer + writtenLength, maxOutputLength - writtenLength, "%s{\"key\":\"%s\",\"name\":\"%s\",\"mode\":%u,\"secure\":%s,\"engine_on\":%s,\"delay\":%u,\"steps\":[",
                     kindPos ? "," : "", kindDef.key, kindDef.name, kindDef.mode, kindDef.secure ? "true" : "false", kindDef.engine_on ? "true" : "false", kindDef.delay_s);
        if (appendedLength < 0 || (size_t)appendedLength >= maxOutputLength - writtenLength) return 0;
        writtenLength += (size_t)appendedLength;
        for (uint8_t stepIndex = 0; stepIndex < kindDef.steps; stepIndex++) {
            appendedLength = snprintf(outputBuffer + writtenLength, maxOutputLength - writtenLength, "%s%d", stepIndex ? "," : "", kindDef.vals[stepIndex]);
            if (appendedLength < 0 || (size_t)appendedLength >= maxOutputLength - writtenLength) return 0;
            writtenLength += (size_t)appendedLength;
        }
        appendedLength = snprintf(outputBuffer + writtenLength, maxOutputLength - writtenLength, "]}");
        if (appendedLength < 0 || (size_t)appendedLength >= maxOutputLength - writtenLength) return 0;
        writtenLength += (size_t)appendedLength;
    }
    int closingLength = snprintf(outputBuffer + writtenLength, maxOutputLength - writtenLength, "]}");
    if (closingLength < 0 || (size_t)closingLength >= maxOutputLength - writtenLength) return 0;
    return writtenLength + (size_t)closingLength;
}

size_t EcmOutputs::statusJson(char *outputBuffer, size_t maxOutputLength) {
    if (!outputBuffer || maxOutputLength < 256) return 0;
    size_t writtenLength = (size_t)snprintf(outputBuffer, maxOutputLength,
        "{\"type\":\"ecm_out\",\"event\":\"status\",\"page\":%s,\"session\":%s,\"unlocked\":%s,\"rpm\":%.0f,\"batt\":%.1f,"
        "\"running\":%s,\"msg\":\"%s\",\"outputs\":{",
        outputsPageActive ? "true" : "false", sessionActive ? "true" : "false", sessionUnlocked ? "true" : "false",
        (double)engineRpm, (double)batteryVoltage, engineRunning() ? "true" : "false", statusMessageText);
    if (writtenLength >= maxOutputLength) return 0;
    const EcmOutProfile *profileDef = activeProfile();
    for (uint8_t kindPos = 0; profileDef && kindPos < profileDef->count; kindPos++) {
        int kindIndex = profileDef->kinds[kindPos];
        int appendedLength = snprintf(outputBuffer + writtenLength, maxOutputLength - writtenLength, "%s\"%s\":{\"s\":%u,\"v\":%u}", kindPos ? "," : "", ECM_OUT_KINDS[kindIndex].key, outputStates[kindIndex].currentState, outputStates[kindIndex].currentStep);
        if (appendedLength < 0 || (size_t)appendedLength >= maxOutputLength - writtenLength) return 0;
        writtenLength += (size_t)appendedLength;
    }
    int closingLength = snprintf(outputBuffer + writtenLength, maxOutputLength - writtenLength, "}}");
    if (closingLength < 0 || (size_t)closingLength >= maxOutputLength - writtenLength) return 0;
    return writtenLength + (size_t)closingLength;
}

bool EcmOutputs::takeStatusUpdate(char *outputBuffer, size_t maxOutputLength) {
    if (!outputsPageActive) return false;
    uint32_t now = millis();
    if (!statusDirty && now - lastStatusMs < ECMO_STATUS_MS) return false;
    statusDirty = false;
    lastStatusMs = now;
    return statusJson(outputBuffer, maxOutputLength) > 0;
}
