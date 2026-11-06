/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file ecm_special.cpp
 * @brief ECM special functions engine (see ecm_special.h).
 */
#include "ecm_special.h"
#include "config.h"
#include "uds_handler.h"
#include "can_driver.h"
#include "ecm_algo.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

static const uint32_t ECMS_KEEPALIVE_MS = 2000;
static const uint16_t ECMS_ENGINE_RUNNING = 150;      // database threshold for DID 0200 bytes 25,26
static const uint8_t  ECMS_CFG_MAX = 56;              // largest configuration block we can write back (ISO-TP request <= 60 bytes)

static bool     specialPageActive = false, sessionActive = false, sessionUnlocked = false;
static int      activeProfileIndex = -1;
static bool     profileManuallySet = false;
static char     calibrationId[48] = {0};
static char     statusMessageText[160] = {0};
static uint32_t lastTesterPresentMs = 0;
static bool     operationBusy = false;                        // timed function in progress (AFV reset)
static uint32_t operationBusyEndMs = 0;
static char     busyOperationName[48] = {0};
static uint8_t  backupConfigBlock[ECMS_CFG_MAX];                // original F1E8 block (saved before the first write)
static uint8_t  backupConfigLength = 0;
static bool     backupConfigValid = false;

static void setStatusMessage(const char *formatText, ...) __attribute__((format(printf, 1, 2)));
static void setStatusMessage(const char *formatText, ...) {
    va_list argList; va_start(argList, formatText);
    vsnprintf(statusMessageText, sizeof(statusMessageText), formatText, argList);
    va_end(argList);
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

static void bytesToHex(const uint8_t *inputBytes, uint8_t byteCount, char *outputText, size_t maxOutputLength) {
    size_t writtenLength = 0;
    for (uint8_t byteIndex = 0; byteIndex < byteCount && writtenLength + 3 <= maxOutputLength; byteIndex++) writtenLength += (size_t)snprintf(outputText + writtenLength, maxOutputLength - writtenLength, "%02X", inputBytes[byteIndex]);
    if (maxOutputLength) outputText[writtenLength < maxOutputLength ? writtenLength : maxOutputLength - 1] = 0;
}

// JSON string escaping (descriptions contain line breaks)
static size_t jsonEsc(const char *inputText, char *outputBuffer, size_t maxOutputLength) {
    size_t writtenLength = 0;
    for (; inputText && *inputText && writtenLength + 7 < maxOutputLength; inputText++) {
        unsigned char currentChar = (unsigned char)*inputText;
        if (currentChar == '"' || currentChar == '\\') { outputBuffer[writtenLength++] = '\\'; outputBuffer[writtenLength++] = (char)currentChar; }
        else if (currentChar == '\n') { outputBuffer[writtenLength++] = '\\'; outputBuffer[writtenLength++] = 'n'; }
        else if (currentChar == '\r' || currentChar == '\t') { outputBuffer[writtenLength++] = ' '; }
        else if (currentChar < 0x20) { /* skip */ }
        else outputBuffer[writtenLength++] = (char)currentChar;
    }
    outputBuffer[writtenLength] = 0;
    return writtenLength;
}

static const EcmSpcProfile *activeProfile() {
    return (activeProfileIndex >= 0 && activeProfileIndex < ECM_SPC_PROFILE_COUNT) ? &ECM_SPC_PROFILES[activeProfileIndex] : nullptr;
}

static int classifyProfileFromCalibration(const char *calibrationIdString) {
    size_t bestPrefixLength = 0;
    int matchedProfile = ECM_SPC_DEFAULT_PROFILE;
    for (uint16_t ruleIndex = 0; ruleIndex < ECM_SPC_CAL_RULE_COUNT; ruleIndex++) {
        size_t prefixLength = strlen(ECM_SPC_CAL_RULES[ruleIndex].prefix);
        if (prefixLength > bestPrefixLength && strncmp(calibrationIdString, ECM_SPC_CAL_RULES[ruleIndex].prefix, prefixLength) == 0) {
            bestPrefixLength = prefixLength;
            matchedProfile = ECM_SPC_CAL_RULES[ruleIndex].profile;
        }
    }
    return matchedProfile;
}

static const EcmSpcKind *findKind(const char *testKey) {
    const EcmSpcProfile *profileDef = activeProfile();
    if (!profileDef || !testKey) return nullptr;
    for (uint8_t kindPos = 0; kindPos < profileDef->count; kindPos++) {
        if (strcmp(ECM_SPC_KINDS[profileDef->kinds[kindPos]].key, testKey) == 0) return &ECM_SPC_KINDS[profileDef->kinds[kindPos]];
    }
    return nullptr;
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
        case 0x72: return "general programming failure";
        case 0x7E: case 0x7F: return "not supported in active session";
        default:   return "rejected";
    }
}

static bool transmitBytes(const uint8_t *requestBytes, uint8_t requestLength, UdsRawResult &rawResult, uint32_t timeoutMs = 500) {
    UdsHandler::rawTransaction(HD_ECU_ECM, requestBytes, requestLength, rawResult, timeoutMs);
    if (rawResult.got_reply && rawResult.positive) lastTesterPresentMs = millis();
    return rawResult.got_reply && rawResult.positive;
}

static bool transmitHex(const char *hexString, UdsRawResult &rawResult, uint32_t timeoutMs = 500) {
    uint8_t requestBytes[8];
    uint8_t requestLength = hexStringToBytes(hexString, requestBytes, sizeof(requestBytes));
    if (requestLength == 0) { memset(&rawResult, 0, sizeof(rawResult)); return false; }
    return transmitBytes(requestBytes, requestLength, rawResult, timeoutMs);
}

// Retry a read request (0x22) on no-response (lost frame); reads are idempotent, so this just adds the same
// tolerance readDidSynchronous() gives the ECU-info reads. A negative reply (the module answered) stops it.
static bool transmitHexRead(const char *hexString, UdsRawResult &rawResult, uint32_t timeoutMs = 500) {
    bool ok = false;
    for (uint8_t attempt = 0; attempt < 3; attempt++) {
        if (attempt) delay(60);
        ok = transmitHex(hexString, rawResult, timeoutMs);
        if (ok || rawResult.got_reply) break;
    }
    return ok;
}

static const uint8_t  ECM_SESSION_TRIES = 3;      // extended-session attempts before giving up
static const uint32_t ECM_SESSION_RETRY_MS = 60;  // gap between the priming frame and each retry

static bool ensureSession() {
    if (sessionActive) return true;
    UdsRawResult rawResult;
    // First frame after bus idle is often lost; prime with a TesterPresent and retry on no-response (see the
    // long note in cfg_special.cpp). A negative reply is reported without retrying - the module answered.
    const uint8_t testerPresentFrame[8] = {0x02, 0x3E, 0x80, 0, 0, 0, 0, 0};
    CanDriver::transmit(HD_ECU_ECM, false, false, 8, testerPresentFrame);
    delay(ECM_SESSION_RETRY_MS);
    for (uint8_t attempt = 0; attempt < ECM_SESSION_TRIES; attempt++) {
        if (transmitHex("1003", rawResult)) { sessionActive = true; sessionUnlocked = false; lastTesterPresentMs = millis(); return true; }
        if (rawResult.got_reply) { setStatusMessage("Extended session refused: %s (NRC 0x%02X)", nrcDescription(rawResult.nrc), rawResult.nrc); return false; }
        if (attempt + 1 < ECM_SESSION_TRIES) delay(ECM_SESSION_RETRY_MS);
    }
    setStatusMessage("No response from ECM (0x7E0) to extended session request after %u tries", (unsigned)ECM_SESSION_TRIES);
    return false;
}

static bool unlock() {
    if (sessionUnlocked) return true;
    UdsRawResult rawResult;
    // Seed request FIRST (the real tool does 27 01, then reads F1EC/F1F4/F1EA): the F1F4 key-flag is read after
    // the seed. Retried up to 3 times on no-response; a negative reply is not retried (e.g. NRC 0x37).
    bool seedOk = false;
    for (uint8_t attempt = 0; attempt < 3 && !seedOk; attempt++) {
        if (attempt) delay(80);
        seedOk = transmitHex("2701", rawResult) && rawResult.len >= 2;
        if (!seedOk && rawResult.got_reply) break;
    }
    if (!seedOk) {
        setStatusMessage("Security access: seed request failed%s", rawResult.got_reply ? " (rejected)" : " (no response after 3 tries)");
        return false;
    }
    // Copy the seed out of rawResult now, before the identification reads below overwrite rawResult.
    uint8_t seedLength = rawResult.len - 2;
    if (seedLength > 8) seedLength = 8;
    uint8_t seedBytes[8]; memcpy(seedBytes, &rawResult.data[2], seedLength);
    bool seedIsZero = true;
    for (uint8_t byteIndex = 0; byteIndex < seedLength; byteIndex++) if (seedBytes[byteIndex]) seedIsZero = false;
    if (seedIsZero) { sessionUnlocked = true; return true; }
    // Identification reads done around the seed; F1F4 carries the key-flag (byte index 1).
    // Identification triplet (22F1EA / 22F1F4 / 22F1EC). F1F4 byte index 1 is the low-byte XOR flag;
    // all three feed the table-selection logic, which picks table 1 vs table 2 from the ECM's own identity.
    UdsRawResult f1ecResult{}; transmitHexRead("22F1EC", f1ecResult);
    UdsRawResult f1f4Result{}; transmitHexRead("22F1F4", f1f4Result);   // retried read of the key-flag source
    UdsRawResult f1eaResult{}; transmitHexRead("22F1EA", f1eaResult);
    const uint8_t ecmFlag = (f1f4Result.got_reply && f1f4Result.positive && f1f4Result.len >= 5) ? f1f4Result.data[4] : 0x00;
    const uint8_t ecmTable = ecmSelectSecurityTable(f1eaResult.data, f1eaResult.len,
                                                    f1f4Result.data, f1f4Result.len,
                                                    f1ecResult.data, f1ecResult.len);
    Serial.printf("[ECM-SPC] F1F4 reply=%d pos=%d len=%u -> flag=0x%02X table=%u ; seed=%02X%02X\n",
                  f1f4Result.got_reply, f1f4Result.positive, (unsigned)f1f4Result.len, ecmFlag, ecmTable, seedBytes[0], seedBytes[1]);
    uint8_t keyBytes[8]; uint8_t keyLength = 0;
    if (!ecmComputeSecurityKeyTable(ecmTable, ecmFlag, seedBytes, seedLength, keyBytes, &keyLength)) {
        char seedHexText[40] = {0};
        for (uint8_t byteIndex = 0; byteIndex < seedLength && byteIndex < 8; byteIndex++) snprintf(seedHexText + strlen(seedHexText), sizeof(seedHexText) - strlen(seedHexText), "%02X ", seedBytes[byteIndex]);
        setStatusMessage("Security access: could not compute key for seed %s(see ecm_algo.h)", seedHexText);
        Serial.printf("[ECM-SPC] Unknown security seed: %s\n", seedHexText);
        return false;
    }
    Serial.printf("[ECM-SPC] computed key %02X%02X for seed %02X%02X (flag 0x%02X)\n",
                  keyBytes[0], keyBytes[1], seedBytes[0], seedBytes[1], ecmFlag);
    uint8_t keyRequest[8] = {0x27, 0x02};
    memcpy(keyRequest + 2, keyBytes, keyLength);
    UdsRawResult keyResult;
    if (!transmitBytes(keyRequest, (uint8_t)(2 + keyLength), keyResult, 450)) {
        setStatusMessage("Security access: key rejected (NRC 0x%02X)", keyResult.nrc);
        return false;
    }
    sessionUnlocked = true;
    return true;
}

static bool openSecured() { return ensureSession() && unlock(); }

static void closeSession() {
    if (sessionActive) { UdsRawResult rawResult; transmitHex("1001", rawResult); }
    sessionActive = false;
    sessionUnlocked = false;
}

// Engine data (DID 0200): rpm raw (bytes 25,26) and coolant temperature (byte 9 - 16)
static bool readEngine(int &outputRpm, int &outputTemp) {
    UdsRawResult rawResult;
    if (!transmitHex("220200", rawResult) || rawResult.len < 3 + 27) return false;
    outputRpm  = rawResult.data[3 + 25] * 256 + rawResult.data[3 + 26];
    outputTemp = (int)rawResult.data[3 + 9] - 16;
    return true;
}

// ---------------------------------------------------------------------------
// Configuration block F1E8
// ---------------------------------------------------------------------------
static bool readCfg(uint8_t *outputBlock, uint8_t &outputLength) {
    UdsRawResult rawResult;
    if (!transmitHexRead("22F1E8", rawResult)) {
        setStatusMessage("Reading configuration (F1E8) failed%s", rawResult.got_reply ? " - rejected" : " - no response after retries");
        return false;
    }
    if (rawResult.len < 4 || rawResult.len - 3 > ECMS_CFG_MAX) { setStatusMessage("Configuration block has an unexpected length (%u)", rawResult.len); return false; }
    outputLength = (uint8_t)(rawResult.len - 3);
    memcpy(outputBlock, rawResult.data + 3, outputLength);
    return true;
}

static bool writeCfg(const uint8_t *configBlock, uint8_t configLength) {
    uint8_t requestBytes[ECMS_CFG_MAX + 3] = {0x2E, 0xF1, 0xE8};
    memcpy(requestBytes + 3, configBlock, configLength);
    UdsRawResult rawResult;
    if (!transmitBytes(requestBytes, (uint8_t)(configLength + 3), rawResult, 900)) {
        if (rawResult.got_reply) setStatusMessage("Writing configuration refused: %s (NRC 0x%02X)", nrcDescription(rawResult.nrc), rawResult.nrc);
        else             setStatusMessage("Writing configuration: no response from ECM");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
void EcmSpecial::reset() {
    specialPageActive = sessionActive = sessionUnlocked = operationBusy = false;
    backupConfigValid = false; backupConfigLength = 0;
    statusMessageText[0] = 0;
}

bool EcmSpecial::pageActive() { return specialPageActive; }

bool EcmSpecial::enter(char *statusMessage, size_t statusMessageLength) {
    specialPageActive = true;
    if (!profileManuallySet) {
        char readCalibrationId[48] = {0};
        bool gotCalibrationId = UdsHandler::readDidSynchronous(HD_ECU_ECM, HD_DID_CALIBRATION_F193, readCalibrationId, sizeof(readCalibrationId), 450) ||
                   UdsHandler::readDidSynchronous(HD_ECU_ECM, HD_DID_CALIBRATION_ID, readCalibrationId, sizeof(readCalibrationId), 450);
        if (gotCalibrationId) {
            strncpy(calibrationId, readCalibrationId, sizeof(calibrationId) - 1);
            calibrationId[sizeof(calibrationId) - 1] = 0;
            activeProfileIndex = classifyProfileFromCalibration(calibrationId);
        } else if (activeProfileIndex < 0) {
            activeProfileIndex = ECM_SPC_DEFAULT_PROFILE;
            calibrationId[0] = 0;
        }
    }
    backupConfigValid = false;
    if (activeProfileIndex < 0) setStatusMessage("No special functions are defined for calibration %s in the database - choose a profile manually", calibrationId[0] ? calibrationId : "?");
    else               setStatusMessage("Special functions ready (profile %s)", ECM_SPC_PROFILES[activeProfileIndex].name);
    if (statusMessage && statusMessageLength) snprintf(statusMessage, statusMessageLength, "%s", statusMessageText);
    return true;
}

void EcmSpecial::exitPage(const char *reason) {
    if (!specialPageActive && !sessionActive && !operationBusy) return;
    closeSession();
    operationBusy = false;
    specialPageActive = false;
    setStatusMessage("Special functions closed (%s)", reason ? reason : "exit");
}

bool EcmSpecial::setProfile(int profileIndex) {
    if (profileIndex < -1 || profileIndex >= ECM_SPC_PROFILE_COUNT) return false;
    if (profileIndex < 0) {
        profileManuallySet = false;
        char dummyBuffer[8];
        enter(dummyBuffer, sizeof(dummyBuffer));
    } else {
        profileManuallySet = true;
        activeProfileIndex = profileIndex;
        backupConfigValid = false;
        setStatusMessage("Profile %s selected manually", ECM_SPC_PROFILES[profileIndex].name);
    }
    return true;
}

// Writes a JSON reply. succeeded=false means the request failed (message in statusMessageText).
static size_t reply(char *outputBuffer, size_t maxOutputLength, bool succeeded, const char *extraJson) {
    char escapedMessage[200];
    jsonEsc(statusMessageText, escapedMessage, sizeof(escapedMessage));
    int writtenLength = snprintf(outputBuffer, maxOutputLength, "{\"ok\":%s,\"msg\":\"%s\"%s%s}", succeeded ? "true" : "false", escapedMessage, extraJson && *extraJson ? "," : "", extraJson ? extraJson : "");
    return (writtenLength > 0 && (size_t)writtenLength < maxOutputLength) ? (size_t)writtenLength : 0;
}

static int idleFromByte(uint8_t byteValue) { return byteValue < 0x83 ? byteValue * 8 + 1000 : (byteValue - 0x83) * 8; }

bool EcmSpecial::read(const char *testKey, char *outputBuffer, size_t outputBufferLength) {
    const EcmSpcKind *kindDef = findKind(testKey);
    if (!kindDef) { setStatusMessage("Function not available for this calibration"); reply(outputBuffer, outputBufferLength, false, ""); return false; }
    char extraJson[260] = {0};
    int engineRpm = 0, engineTemp = 0;
    bool haveEngineData = readEngine(engineRpm, engineTemp);
    UdsRawResult rawResult;
    switch (kindDef->type) {
        case ECMS_IDLE: {
            if (!transmitHexRead("220101", rawResult) || rawResult.len < 4) { setStatusMessage("Reading idle speed failed"); break; }
            snprintf(extraJson, sizeof(extraJson), "\"idle\":%d,\"rpm\":%d,\"temp\":%d,\"engine_on\":%s", idleFromByte(rawResult.data[3]), engineRpm, engineTemp, engineRpm >= ECMS_ENGINE_RUNNING ? "true" : "false");
            setStatusMessage("Idle speed %d rpm", idleFromByte(rawResult.data[3]));
            reply(outputBuffer, outputBufferLength, true, extraJson);
            return true;
        }
        case ECMS_CFGBIT: {
            uint8_t configBlock[ECMS_CFG_MAX], configLength = 0;
            if (!readCfg(configBlock, configLength)) break;
            if (kindDef->pos >= configLength) { setStatusMessage("Configuration block is too short (%u bytes) for this function", configLength); break; }
            bool isEnabled = (configBlock[kindDef->pos] & kindDef->mask) == kindDef->mask;
            bool changedFromBackup = backupConfigValid && kindDef->pos < backupConfigLength &&
                (((backupConfigBlock[kindDef->pos] & kindDef->mask) == kindDef->mask) != isEnabled);
            char blockHexText[2 * ECMS_CFG_MAX + 1];
            bytesToHex(configBlock, configLength, blockHexText, sizeof(blockHexText));
            // "option"/"text" drive the dropdown (type 6, same as BCM/SPDO); "enabled"/"len"/"block" kept for
            // anything still reading the old fields.
            snprintf(extraJson, sizeof(extraJson), "\"option\":%d,\"text\":\"%s\",\"enabled\":%s,\"len\":%u,\"block\":\"%s\",\"changed\":%s",
                     isEnabled ? 1 : 0, isEnabled ? "Enabled" : "Disabled", isEnabled ? "true" : "false", configLength, blockHexText, changedFromBackup ? "true" : "false");
            setStatusMessage("%s is %s", kindDef->name, isEnabled ? "enabled" : "disabled");
            reply(outputBuffer, outputBufferLength, true, extraJson);
            return true;
        }
        case ECMS_SPEEDO: {
            if (!transmitHexRead("220100", rawResult) || rawResult.len < 5) { setStatusMessage("Reading speedometer calibration failed"); break; }
            int calibrationValue = rawResult.data[3] * 256 + rawResult.data[4];
            snprintf(extraJson, sizeof(extraJson), "\"value\":%d,\"engine_on\":%s", calibrationValue, haveEngineData && engineRpm >= ECMS_ENGINE_RUNNING ? "true" : "false");
            setStatusMessage("Speedometer calibration value %d", calibrationValue);
            reply(outputBuffer, outputBufferLength, true, extraJson);
            return true;
        }
        case ECMS_RESET:
        case ECMS_PAIR: {
            snprintf(extraJson, sizeof(extraJson), "\"rpm\":%d,\"engine_on\":%s", haveEngineData ? engineRpm : -1, haveEngineData && engineRpm >= ECMS_ENGINE_RUNNING ? "true" : "false");
            setStatusMessage(haveEngineData ? "ECM reachable" : "No engine data from ECM");
            reply(outputBuffer, outputBufferLength, haveEngineData, extraJson);
            return haveEngineData;
        }
        default:
            setStatusMessage("%s", kindDef->note && *kindDef->note ? kindDef->note : "Not supported");
            break;
    }
    reply(outputBuffer, outputBufferLength, false, "");
    return false;
}

bool EcmSpecial::run(const char *testKey, int value, char *outputBuffer, size_t outputBufferLength) {
    const EcmSpcKind *kindDef = findKind(testKey);
    if (!kindDef) { setStatusMessage("Function not available for this calibration"); reply(outputBuffer, outputBufferLength, false, ""); return false; }
    if (operationBusy) { setStatusMessage("%s is still in progress", busyOperationName); reply(outputBuffer, outputBufferLength, false, ""); return false; }
    int engineRpm = 0, engineTemp = 0;
    bool haveEngineData = readEngine(engineRpm, engineTemp);
    bool engineIsOn = haveEngineData && engineRpm >= ECMS_ENGINE_RUNNING;
    UdsRawResult rawResult;
    bool succeeded = false;
    char extraJson[160] = {0};     // populated for ECMS_CFGBIT so the dropdown (type 6) can refresh itself

    switch (kindDef->type) {
        case ECMS_IDLE: {
            if (!haveEngineData) { setStatusMessage("No engine data from ECM"); break; }
            if (!engineIsOn) { setStatusMessage("Idle speed adjustment can be changed only with engine running"); break; }
            if (value < kindDef->vmin || value > kindDef->vmax) { setStatusMessage("Idle speed out of range (%u..%u rpm)", kindDef->vmin, kindDef->vmax); break; }
            value = kindDef->vmin + ((value - kindDef->vmin + kindDef->step / 2) / kindDef->step) * kindDef->step;      // multiple of the DB step (8 rpm)
            if (value > kindDef->vmax) value = kindDef->vmax;
            uint8_t requestBytes[4] = {0x2E, 0x01, 0x01, (uint8_t)(((value - 1000) / 8) & 0xFF)};
            if (!openSecured()) break;
            if (!transmitBytes(requestBytes, 4, rawResult, 700)) { setStatusMessage("Idle speed write refused: %s (NRC 0x%02X)", nrcDescription(rawResult.nrc), rawResult.nrc); break; }
            if (transmitHex("220101", rawResult) && rawResult.len >= 4) setStatusMessage("Idle speed set to %d rpm (ECU reports %d rpm)", value, idleFromByte(rawResult.data[3]));
            else setStatusMessage("Idle speed set to %d rpm", value);
            succeeded = true;
            break;
        }
        case ECMS_CFGBIT: {
            if (!openSecured()) break;
            uint8_t configBlock[ECMS_CFG_MAX], configLength = 0;
            if (!readCfg(configBlock, configLength)) break;
            if (kindDef->pos >= configLength) { setStatusMessage("Configuration block is too short (%u bytes) for this function", configLength); break; }
            if (!backupConfigValid) { memcpy(backupConfigBlock, configBlock, configLength); backupConfigLength = configLength; backupConfigValid = true; }
            uint8_t newConfigBlock[ECMS_CFG_MAX];
            memcpy(newConfigBlock, configBlock, configLength);
            if (value) newConfigBlock[kindDef->pos] |= kindDef->mask; else newConfigBlock[kindDef->pos] &= (uint8_t)~kindDef->mask;
            if (memcmp(newConfigBlock, configBlock, configLength) == 0) {
                setStatusMessage("%s is already %s", kindDef->name, value ? "enabled" : "disabled");
                snprintf(extraJson, sizeof(extraJson), "\"option\":%d,\"text\":\"%s\",\"changed\":false", value ? 1 : 0, value ? "Enabled" : "Disabled");
                succeeded = true;
                break;
            }
            if (!writeCfg(newConfigBlock, configLength)) break;
            uint8_t verifyBlock[ECMS_CFG_MAX], verifyLength = 0;
            if (!readCfg(verifyBlock, verifyLength) || verifyLength != configLength || memcmp(verifyBlock, newConfigBlock, configLength) != 0) {
                bool restoredBack = writeCfg(configBlock, configLength);
                setStatusMessage("Verification failed after writing %s - %s", kindDef->name, restoredBack ? "previous configuration written back" : "could NOT write the previous configuration back");
                break;
            }
            setStatusMessage("%s %s and verified (cycle ignition to apply)", kindDef->name, value ? "enabled" : "disabled");
            snprintf(extraJson, sizeof(extraJson), "\"option\":%d,\"text\":\"%s\",\"changed\":true", value ? 1 : 0, value ? "Enabled" : "Disabled");
            succeeded = true;
            break;
        }
        case ECMS_RESET: {
            uint8_t requestBytes[8];
            uint8_t requestLength = hexStringToBytes(kindDef->cmd, requestBytes, sizeof(requestBytes));
            if (requestLength == 0) { setStatusMessage("Internal error: bad command"); break; }
            if (!openSecured()) break;
            if (!transmitBytes(requestBytes, requestLength, rawResult, 700)) { setStatusMessage("%s refused: %s (NRC 0x%02X)", kindDef->name, nrcDescription(rawResult.nrc), rawResult.nrc); break; }
            operationBusy = true;
            operationBusyEndMs = millis() + kindDef->delay_ms;
            snprintf(busyOperationName, sizeof(busyOperationName), "%s", kindDef->name);
            setStatusMessage("%s in progress (%u s) - keep ignition on", kindDef->name, (unsigned)(kindDef->delay_ms / 1000U));
            succeeded = true;
            break;
        }
        case ECMS_PAIR: {
            if (value != 1) { setStatusMessage("Pairing needs explicit confirmation"); break; }
            if (engineIsOn) { setStatusMessage("Switch the engine off (ignition on) before pairing"); break; }
            // Database order: functional clear DTC (0x7DF), functional extended session, ECM security access, wait, functional default session
            const uint8_t clearDtcFrame[8] = {0x04, 0x14, 0xFF, 0xFF, 0xFF, 0, 0, 0};
            const uint8_t extendedSessionFrame[8] = {0x02, 0x10, 0x03, 0, 0, 0, 0, 0};
            const uint8_t defaultSessionFrame[8] = {0x02, 0x10, 0x01, 0, 0, 0, 0, 0};
            CanDriver::transmit(0x7DF, false, false, 8, clearDtcFrame);
            delay(60);
            CanDriver::transmit(0x7DF, false, false, 8, extendedSessionFrame);
            delay(60);
            sessionActive = true;                 // all modules entered the extended session by the broadcast
            sessionUnlocked = false;
            if (!unlock()) { CanDriver::transmit(0x7DF, false, false, 8, defaultSessionFrame); sessionActive = false; break; }
            delay(kindDef->delay_ms);
            CanDriver::transmit(0x7DF, false, false, 8, defaultSessionFrame);
            sessionActive = false; sessionUnlocked = false;
            setStatusMessage("Pairing sequence sent - attempt a normal start one time");
            succeeded = true;
            break;
        }
        case ECMS_SPEEDO: {
            if (value < kindDef->vmin || value > kindDef->vmax) { setStatusMessage("Value out of range (%u..%u)", kindDef->vmin, kindDef->vmax); break; }
            if (!openSecured()) break;
            uint8_t requestBytes[5] = {0x2E, 0x01, 0x00, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
            if (!transmitBytes(requestBytes, 5, rawResult, 700)) { setStatusMessage("Speedometer calibration refused: %s (NRC 0x%02X)", nrcDescription(rawResult.nrc), rawResult.nrc); break; }
            if (transmitHex("220100", rawResult) && rawResult.len >= 5) setStatusMessage("Speedometer calibration set to %d (ECU reports %d)", value, rawResult.data[3] * 256 + rawResult.data[4]);
            else setStatusMessage("Speedometer calibration set to %d", value);
            succeeded = true;
            break;
        }
        default:
            setStatusMessage("%s", kindDef->note && *kindDef->note ? kindDef->note : "Not supported");
            break;
    }
    if (!succeeded && sessionActive && kindDef->type != ECMS_RESET) closeSession();
    if (succeeded && kindDef->type != ECMS_RESET && kindDef->type != ECMS_PAIR) closeSession();   // default session also drops security access
    reply(outputBuffer, outputBufferLength, succeeded, extraJson);
    return succeeded;
}

bool EcmSpecial::restore(const char *testKey, char *outputBuffer, size_t outputBufferLength) {
    const EcmSpcKind *kindDef = findKind(testKey);
    if (!kindDef || kindDef->type != ECMS_CFGBIT) { setStatusMessage("Nothing to restore for this function"); reply(outputBuffer, outputBufferLength, false, ""); return false; }
    if (!backupConfigValid) { setStatusMessage("No saved configuration - nothing was changed in this session"); reply(outputBuffer, outputBufferLength, false, ""); return false; }
    bool succeeded = openSecured() && writeCfg(backupConfigBlock, backupConfigLength);
    if (succeeded) {
        uint8_t verifyBlock[ECMS_CFG_MAX], verifyLength = 0;
        succeeded = readCfg(verifyBlock, verifyLength) && verifyLength == backupConfigLength && memcmp(verifyBlock, backupConfigBlock, verifyLength) == 0;
        setStatusMessage(succeeded ? "Original configuration restored and verified" : "Restore written but verification failed");
    }
    closeSession();
    reply(outputBuffer, outputBufferLength, succeeded, "");
    return succeeded;
}

void EcmSpecial::loop() {
    const uint32_t now = millis();
    if (operationBusy && (int32_t)(now - operationBusyEndMs) >= 0) {
        operationBusy = false;
        closeSession();                 // end command of the database: default session
        setStatusMessage("%s finished", busyOperationName);
    }
    if (sessionActive && now - lastTesterPresentMs >= ECMS_KEEPALIVE_MS) {
        lastTesterPresentMs = now;
        const uint8_t testerPresentFrame[8] = {0x02, 0x3E, 0x80, 0, 0, 0, 0, 0};
        CanDriver::transmit(HD_ECU_ECM, false, false, 8, testerPresentFrame);
    }
}

size_t EcmSpecial::catalogJson(char *outputBuffer, size_t maxOutputLength) {
    if (!outputBuffer || maxOutputLength < 256) return 0;
    size_t writtenLength = (size_t)snprintf(outputBuffer, maxOutputLength,
        "{\"type\":\"ecm_spc\",\"event\":\"catalog\",\"ok\":true,\"cal\":\"%s\",\"profile\":%d,\"profile_name\":\"%s\",\"auto\":%s,\"profiles\":[",
        calibrationId, activeProfileIndex, activeProfileIndex >= 0 ? ECM_SPC_PROFILES[activeProfileIndex].name : "none", profileManuallySet ? "false" : "true");
    if (writtenLength >= maxOutputLength) return 0;
    for (int profileIndex = 0; profileIndex < ECM_SPC_PROFILE_COUNT; profileIndex++) {
        int appendedLength = snprintf(outputBuffer + writtenLength, maxOutputLength - writtenLength, "%s{\"id\":%d,\"name\":\"%s\",\"count\":%u}", profileIndex ? "," : "", profileIndex, ECM_SPC_PROFILES[profileIndex].name, ECM_SPC_PROFILES[profileIndex].count);
        if (appendedLength < 0 || (size_t)appendedLength >= maxOutputLength - writtenLength) return 0;
        writtenLength += (size_t)appendedLength;
    }
    int functionsHeaderLength = snprintf(outputBuffer + writtenLength, maxOutputLength - writtenLength, "],\"functions\":[");
    if (functionsHeaderLength < 0 || (size_t)functionsHeaderLength >= maxOutputLength - writtenLength) return 0;
    writtenLength += (size_t)functionsHeaderLength;
    const EcmSpcProfile *profileDef = activeProfile();
    for (uint8_t kindPos = 0; profileDef && kindPos < profileDef->count; kindPos++) {
        const EcmSpcKind &kindDef = ECM_SPC_KINDS[profileDef->kinds[kindPos]];
        char escapedDesc[300], escapedNote[200], escapedName[100];
        jsonEsc(kindDef.desc, escapedDesc, sizeof(escapedDesc)); jsonEsc(kindDef.note, escapedNote, sizeof(escapedNote)); jsonEsc(kindDef.name, escapedName, sizeof(escapedName));
        // Enable/disable (CFGBIT) functions are sent as the same "option dropdown" type (6) the BCM/SPDO/RAD
        // special-function pages use, with a plain Disabled/Enabled choice, instead of the old Enable/Disable
        // button pair - keeps the UI (and ecmSpcApply()) identical across every module.
        const bool isToggle = (kindDef.type == ECMS_CFGBIT);
        int appendedLength = snprintf(outputBuffer + writtenLength, maxOutputLength - writtenLength,
            "%s{\"key\":\"%s\",\"name\":\"%s\",\"type\":%u,\"desc\":\"%s\",\"note\":\"%s\",\"min\":%u,\"max\":%u,\"step\":%u,\"delay\":%u%s}",
            kindPos ? "," : "", kindDef.key, escapedName, isToggle ? 6u : (unsigned)kindDef.type, escapedDesc, escapedNote, kindDef.vmin, kindDef.vmax, kindDef.step, (unsigned)(kindDef.delay_ms / 1000U),
            isToggle ? ",\"opts\":[\"Disabled\",\"Enabled\"]" : "");
        if (appendedLength < 0 || (size_t)appendedLength >= maxOutputLength - writtenLength) return 0;
        writtenLength += (size_t)appendedLength;
    }
    int closingLength = snprintf(outputBuffer + writtenLength, maxOutputLength - writtenLength, "]}");
    if (closingLength < 0 || (size_t)closingLength >= maxOutputLength - writtenLength) return 0;
    return writtenLength + (size_t)closingLength;
}

size_t EcmSpecial::statusJson(char *outputBuffer, size_t maxOutputLength) {
    char escapedMessage[200];
    jsonEsc(statusMessageText, escapedMessage, sizeof(escapedMessage));
    int writtenLength = snprintf(outputBuffer, maxOutputLength,
        "{\"type\":\"ecm_spc\",\"event\":\"status\",\"active\":%s,\"session\":%s,\"unlocked\":%s,\"busy\":%s,\"busy_left\":%d,\"backup\":%s,\"msg\":\"%s\"}",
        specialPageActive ? "true" : "false", sessionActive ? "true" : "false", sessionUnlocked ? "true" : "false", operationBusy ? "true" : "false",
        operationBusy ? (int)((int32_t)(operationBusyEndMs - millis()) / 1000) : 0, backupConfigValid ? "true" : "false", escapedMessage);
    return (writtenLength > 0 && (size_t)writtenLength < maxOutputLength) ? (size_t)writtenLength : 0;
}

// ---------------------------------------------------------------------------
// Standalone actions - these three used to be multi-step guided procedures (proc_tables.h: ecm_tps_reset,
// ecm_relearn, vin_write_ecm) driving the generic ProcRunner popup wizard. They are plain one-shot UDS
// exchanges with nothing for the operator to do mid-sequence, so they are now ordinary special-function
// actions (Run / Read+Write) like everything else here, always offered regardless of calibration profile.
// ---------------------------------------------------------------------------

// TPS reset / throttle relearn: ECM unlock (Security Access mode 0x0B) then IOControl DID 0x0508 (02 F0 5 08 03 01).
bool EcmSpecial::runTpsReset(char *outputBuffer, size_t outputBufferLength) {
    if (!openSecured()) { reply(outputBuffer, outputBufferLength, false, ""); return false; }
    uint8_t requestBytes[8];
    uint8_t requestLength = hexStringToBytes("2F05080301", requestBytes, sizeof(requestBytes));
    UdsRawResult rawResult;
    bool succeeded = requestLength && transmitBytes(requestBytes, requestLength, rawResult, 700);
    if (succeeded) setStatusMessage("TPS reset sent - cycle the ignition and let the ECM relearn idle");
    else setStatusMessage("TPS reset refused: %s (NRC 0x%02X)", nrcDescription(rawResult.nrc), rawResult.nrc);
    closeSession();
    reply(outputBuffer, outputBufferLength, succeeded, "");
    return succeeded;
}

// Clear adaptations + relearn: ClearDiagnosticInformation (14 FF FF FF). No Security Access needed.
bool EcmSpecial::runClearAdaptations(char *outputBuffer, size_t outputBufferLength) {
    if (!ensureSession()) { reply(outputBuffer, outputBufferLength, false, ""); return false; }
    UdsRawResult rawResult;
    bool succeeded = transmitHex("14FFFFFF", rawResult, 700);
    if (succeeded) setStatusMessage("Adaptations cleared - attempt a normal start once; the engine may start and stall while relearning");
    else setStatusMessage("Clear adaptations refused: %s (NRC 0x%02X)", nrcDescription(rawResult.nrc), rawResult.nrc);
    closeSession();
    reply(outputBuffer, outputBufferLength, succeeded, "");
    return succeeded;
}

// VIN read (DID F1EF). Printable bytes are shown as-is; anything outside 0x20..0x7E is shown as '.'.
bool EcmSpecial::readVin(char *outputBuffer, size_t outputBufferLength) {
    UdsRawResult rawResult;
    if (!ensureSession() || !transmitHexRead("22F1EF", rawResult) || rawResult.len < 4) {
        setStatusMessage("Reading VIN failed%s", rawResult.got_reply ? " - rejected" : " - no response after retries");
        closeSession();
        reply(outputBuffer, outputBufferLength, false, "");
        return false;
    }
    char vinText[32] = {0};
    uint8_t vinLength = (uint8_t)(rawResult.len - 3);
    if (vinLength > sizeof(vinText) - 1) vinLength = sizeof(vinText) - 1;
    for (uint8_t i = 0; i < vinLength; i++) {
        uint8_t c = rawResult.data[3 + i];
        vinText[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    vinText[vinLength] = 0;
    setStatusMessage("Current VIN: %s", vinText);
    closeSession();
    char escapedVin[40]; jsonEsc(vinText, escapedVin, sizeof(escapedVin));
    char extraJson[64]; snprintf(extraJson, sizeof(extraJson), "\"vin\":\"%s\"", escapedVin);
    reply(outputBuffer, outputBufferLength, true, extraJson);
    return true;
}

// VIN write (DID F1EF, 17 ASCII chars). ECM unlock first (Security Access mode 0x0B), write, then read back
// to confirm - a verification mismatch is reported but does not itself count the write as failed, since the
// write (2E) already got a positive reply by that point.
bool EcmSpecial::writeVin(const char *vin, char *outputBuffer, size_t outputBufferLength) {
    size_t vinLength = vin ? strlen(vin) : 0;
    if (vinLength != 17) { setStatusMessage("VIN must be exactly 17 characters (got %u)", (unsigned)vinLength); reply(outputBuffer, outputBufferLength, false, ""); return false; }
    if (!openSecured()) { reply(outputBuffer, outputBufferLength, false, ""); return false; }
    uint8_t requestBytes[3 + 17] = {0x2E, 0xF1, 0xEF};
    memcpy(requestBytes + 3, vin, 17);
    UdsRawResult rawResult;
    if (!transmitBytes(requestBytes, sizeof(requestBytes), rawResult, 900)) {
        setStatusMessage("VIN write refused: %s (NRC 0x%02X)", nrcDescription(rawResult.nrc), rawResult.nrc);
        closeSession();
        reply(outputBuffer, outputBufferLength, false, "");
        return false;
    }
    UdsRawResult verifyResult;
    bool verified = transmitHex("22F1EF", verifyResult) && verifyResult.len >= 3 + 17 && memcmp(verifyResult.data + 3, vin, 17) == 0;
    if (verified) setStatusMessage("VIN written and verified: %.17s", vin);
    else          setStatusMessage("VIN write sent, but could not be verified on read-back - check the ECU: %.17s", vin);
    closeSession();
    char escapedVin[40]; jsonEsc(vin, escapedVin, sizeof(escapedVin));
    char extraJson[64]; snprintf(extraJson, sizeof(extraJson), "\"vin\":\"%s\"", escapedVin);
    reply(outputBuffer, outputBufferLength, true, extraJson);
    return true;
}
