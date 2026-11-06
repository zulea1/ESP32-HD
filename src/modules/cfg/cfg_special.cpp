/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file cfg_special.cpp
 * @brief Generic configuration-option special-function engine (see cfg_special.h).
 */
#include "cfg_special.h"
#include "config.h"
#include "uds_handler.h"
#include "can_driver.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

static const uint32_t CFGS_KEEPALIVE_MS = 2000;
static const uint32_t CFGS_IDLE_EXIT_MS = 90000;
static const uint8_t  CFG_SESSION_TRIES = 3;      // extended-session attempts before giving up
static const uint32_t CFG_SESSION_RETRY_MS = 60;  // gap between the priming frame and each retry
static const uint8_t  CFG_READ_TRIES    = 3;      // 0x22 read attempts on no-response before reporting an error

CfgSpecialEngine::CfgSpecialEngine(const CfgModuleSpec &spec) : spec_(spec) {
    for (int fnIndex = 0; fnIndex < CFG_MAX_FNS; fnIndex++) { originalByte_[fnIndex] = -1; originalByte2_[fnIndex] = -1; }
}

void CfgSpecialEngine::setMsg(const char *formatText, ...) {
    va_list argList; va_start(argList, formatText);
    vsnprintf(msg_, sizeof(msg_), formatText, argList);
    va_end(argList);
}

static const char *cfgNrcText(uint8_t nrc) {
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

bool CfgSpecialEngine::tx(const uint8_t *requestBytes, uint8_t requestLength, UdsRawResult &rawResult) {
    UdsHandler::rawTransaction(spec_.ecuId, requestBytes, requestLength, rawResult, 450);
    if (rawResult.got_reply && rawResult.positive) lastTesterPresentMs_ = lastOperationMs_ = millis();
    return rawResult.got_reply && rawResult.positive;
}

bool CfgSpecialEngine::ensureSession() {
    if (sessionActive_) return true;
    const uint8_t extendedSessionRequest[2] = {0x10, 0x03};
    UdsRawResult rawResult;
    // With atomic sessions every operation opens a cold session, and the first frame after the bus has been
    // idle is often lost on this network. Send a priming 02 3E 80, then retry the session request a few times on
    // no-response. A negative reply means the module answered, so that is reported without retrying.
    const uint8_t testerPresentFrame[8] = {0x02, 0x3E, 0x80, 0, 0, 0, 0, 0};
    CanDriver::transmit(spec_.ecuId, false, false, 8, testerPresentFrame);
    delay(CFG_SESSION_RETRY_MS);
    for (uint8_t attempt = 0; attempt < CFG_SESSION_TRIES; attempt++) {
        if (tx(extendedSessionRequest, 2, rawResult)) { sessionActive_ = true; sessionUnlocked_ = false; return true; }
        if (rawResult.got_reply) { setMsg("Extended session refused: %s (NRC 0x%02X)", cfgNrcText(rawResult.nrc), rawResult.nrc); return false; }
        if (attempt + 1 < CFG_SESSION_TRIES) delay(CFG_SESSION_RETRY_MS);
    }
    setMsg("No response from the %s (0x%03X) after %u tries", spec_.tag, (unsigned)spec_.ecuId, (unsigned)CFG_SESSION_TRIES);
    return false;
}

bool CfgSpecialEngine::unlock() {
    if (sessionUnlocked_) return true;
    if (!spec_.computeKey) return false;   // module has no Security Access key - never send 0x27
    UdsRawResult rawResult;
    uint8_t preUnlockBytes[8] = {0};
    uint8_t preUnlockLength = 0;
    if (spec_.preUnlockDid) {                                            // the database reads this DID right before the seed request
        const uint8_t preRequest[3] = {0x22, (uint8_t)(spec_.preUnlockDid >> 8), (uint8_t)(spec_.preUnlockDid & 0xFF)};
        UdsRawResult preResult;
        // A positive 0x22 (ReadDataByIdentifier) reply is "62 <DID-hi> <DID-lo> <data...>" - THREE header
        // bytes before the actual payload, same as readBlock()/readByte() elsewhere in this codebase (they
        // both start at data[3]). This used to read from data[2] with length (len - 2), which is the header
        // shape of a SecurityAccess reply ("67 01 <seed...>", 2 header bytes) - off by one byte for a DID
        // read, so preUnlockBytes[0] was actually the DID's own low byte (e.g. 0xBD of 0xFDBD) rather than
        // the first real data byte, making every seed-vs-DID compare (e.g. RAD's radComputeSecurityKey) fail.
        if (tx(preRequest, 3, preResult) && preResult.len > 3) {
            preUnlockLength = (uint8_t)(preResult.len - 3);
            if (preUnlockLength > sizeof(preUnlockBytes)) preUnlockLength = (uint8_t)sizeof(preUnlockBytes);
            memcpy(preUnlockBytes, &preResult.data[3], preUnlockLength);
        }
    }
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
    if (seedIsZero) { sessionUnlocked_ = true; return true; }
    uint8_t keyBytes[4]; uint8_t keyLength = 0;
    char keyErrorMsg[96] = {0};
    if (!spec_.computeKey ||
        !spec_.computeKey(&rawResult.data[2], seedLength, keyBytes, &keyLength, preUnlockBytes, preUnlockLength, keyErrorMsg, sizeof(keyErrorMsg)) ||
        keyLength == 0) {
        if (keyErrorMsg[0]) {                 // computeKey gave a specific reason (e.g. RAD's seed-vs-DID mismatch)
            setMsg("%s", keyErrorMsg);
            Serial.printf("[%s-SPC] %s\n", spec_.tag, keyErrorMsg);
        } else {
            char seedHexText[16] = {0};
            for (uint8_t byteIndex = 0; byteIndex < seedLength; byteIndex++) snprintf(seedHexText + strlen(seedHexText), sizeof(seedHexText) - strlen(seedHexText), "%02X", rawResult.data[2 + byteIndex]);
            setMsg("The %s requires security access and its seed %s has no known key (see %s)", spec_.tag, seedHexText, spec_.securityFile);
            Serial.printf("[%s-SPC] Unknown seed: %s\n", spec_.tag, seedHexText);
        }
        return false;
    }
    uint8_t keyRequest[8] = {0x27, 0x02};
    memcpy(keyRequest + 2, keyBytes, keyLength);
    UdsRawResult keyResult;
    if (!tx(keyRequest, (uint8_t)(2 + keyLength), keyResult)) { setMsg("Security access: key rejected (NRC 0x%02X)", keyResult.nrc); return false; }
    sessionUnlocked_ = true;
    return true;
}

int CfgSpecialEngine::classify(const char *idString) const {
    size_t bestPrefixLength = 0; int matchedProfile = spec_.defaultProfile;
    for (uint16_t ruleIndex = 0; ruleIndex < spec_.ruleCount; ruleIndex++) {
        size_t prefixLength = strlen(spec_.rules[ruleIndex].prefix);
        if (prefixLength > bestPrefixLength && strncmp(idString, spec_.rules[ruleIndex].prefix, prefixLength) == 0) { bestPrefixLength = prefixLength; matchedProfile = spec_.rules[ruleIndex].profile; }
    }
    return matchedProfile;
}

const CfgProfile *CfgSpecialEngine::profile() const {
    return (activeProfile_ >= 0 && activeProfile_ < spec_.profileCount) ? &spec_.profiles[activeProfile_] : nullptr;
}

int CfgSpecialEngine::findFn(const char *testKey) const {
    const CfgProfile *profileDef = profile();
    if (!profileDef || !testKey) return -1;
    for (int fnIndex = 0; fnIndex < profileDef->count && fnIndex < CFG_MAX_FNS; fnIndex++) if (strcmp(profileDef->fns[fnIndex].key, testKey) == 0) return fnIndex;
    return -1;
}

// 22 <DID> -> whole data block. Fails if the block is shorter than the byte position the function needs.
// A read (0x22) is idempotent, so a lost frame is retried a few times before reporting - matching the
// tolerance readDidSynchronous() gives the ECU-info reads. A negative reply (the module answered) is not
// retried.
bool CfgSpecialEngine::readBlock(const CfgFn &fn, uint16_t did, uint8_t pos, Block &block) {
    const uint8_t requestBytes[3] = {0x22, (uint8_t)(did >> 8), (uint8_t)(did & 0xFF)};
    UdsRawResult rawResult;
    block.len = 0;
    bool ok = false;
    for (uint8_t attempt = 0; attempt < CFG_READ_TRIES; attempt++) {
        if (attempt) delay(CFG_SESSION_RETRY_MS);
        ok = tx(requestBytes, 3, rawResult);
        if (ok || rawResult.got_reply) break;
    }
    if (!ok) {
        if (rawResult.got_reply) setMsg("%s: %s rejected the read of DID 0x%04X - %s (NRC 0x%02X)", fn.name, spec_.tag, did, cfgNrcText(rawResult.nrc), rawResult.nrc);
        else             setMsg("%s: no response from the %s after %u tries", fn.name, spec_.tag, (unsigned)CFG_READ_TRIES);
        return false;
    }
    if (rawResult.len < 3) { setMsg("%s: %s answer too short", fn.name, spec_.tag); return false; }
    uint8_t dataLength = rawResult.len - 3;
    if (dataLength > CFG_MAX_BLOCK) dataLength = CFG_MAX_BLOCK;
    if (pos >= dataLength) { setMsg("%s: DID 0x%04X returned %u data byte(s), but this option lives in byte %u - not supported by this unit", fn.name, did, dataLength, pos + 1); return false; }
    memcpy(block.bytes, &rawResult.data[3], dataLength);
    block.len = dataLength;
    return true;
}

// 2E <DID> <whole block> (multi-frame when needed; UdsHandler::rawTransaction handles up to 60 bytes)
bool CfgSpecialEngine::writeBlock(const CfgFn &fn, uint16_t did, const Block &block) {
    uint8_t requestBytes[3 + CFG_MAX_BLOCK];
    requestBytes[0] = 0x2E; requestBytes[1] = (uint8_t)(did >> 8); requestBytes[2] = (uint8_t)(did & 0xFF);
    memcpy(requestBytes + 3, block.bytes, block.len);
    const uint8_t requestLength = (uint8_t)(3 + block.len);
    UdsRawResult rawResult;
    // A module whose writes need Security Access (e.g. the RADIO) is unlocked UP FRONT before the write;
    // if the unlock fails (e.g. an unknown seed) the message from unlock() already
    // names the seed, so just stop here.
    if (spec_.unlockUpFront && !sessionUnlocked_ && !unlock()) return false;
    if (tx(requestBytes, requestLength, rawResult)) return true;
    if (rawResult.got_reply && rawResult.nrc == 0x33) {                    // security access needed
        if (!unlock()) return false;
        if (tx(requestBytes, requestLength, rawResult)) return true;
    }
    if (rawResult.got_reply) setMsg("%s: %s rejected the write of DID 0x%04X - %s (NRC 0x%02X)", fn.name, spec_.tag, did, cfgNrcText(rawResult.nrc), rawResult.nrc);
    else             setMsg("%s: no response from the %s to the write", fn.name, spec_.tag);
    return false;
}

// Read-modify-write of the bits `mask` in byte `pos` of `did`; `block` returns the block as written.
// Returns true also when nothing needed changing.
bool CfgSpecialEngine::writeField(const CfgFn &fn, uint16_t did, uint8_t pos, uint8_t mask, uint8_t value, Block &block) {
    if (!readBlock(fn, did, pos, block)) return false;
    const uint8_t newByte = (uint8_t)((block.bytes[pos] & ~mask) | (value & mask));
    if (newByte == block.bytes[pos]) return true;
    block.bytes[pos] = newByte;
    return writeBlock(fn, did, block);
}

int CfgSpecialEngine::optionOf(const CfgFn &fn, uint8_t currentByte) const {
    for (uint8_t optionIndex = 0; optionIndex < fn.nopt; optionIndex++) if ((currentByte & fn.rmask) == (fn.opts[optionIndex].val & fn.rmask)) return optionIndex;
    return -1;
}

size_t CfgSpecialEngine::reply(char *outputBuffer, size_t maxOutputLength, bool succeeded, const char *extraJson) {
    return (size_t)snprintf(outputBuffer, maxOutputLength, "{\"ok\":%s,\"msg\":\"%s\"%s%s}", succeeded ? "true" : "false", msg_, extraJson && extraJson[0] ? "," : "", extraJson ? extraJson : "");
}

// ---------------------------------------------------------------------------
void CfgSpecialEngine::reset() {
    pageActive_ = sessionActive_ = sessionUnlocked_ = profileManuallySet_ = false;
    activeProfile_ = -1; msg_[0] = 0; idString_[0] = 0;
    for (int fnIndex = 0; fnIndex < CFG_MAX_FNS; fnIndex++) { originalByte_[fnIndex] = -1; originalByte2_[fnIndex] = -1; }
}

bool CfgSpecialEngine::pageActive() { return pageActive_ || sessionActive_; }

bool CfgSpecialEngine::enter(char *statusMessage, size_t statusMessageLength) {
    pageActive_ = true;
    if (!profileManuallySet_) {
        char readId[48] = {0};
        bool gotId = spec_.idDid ? UdsHandler::readDidSynchronous(spec_.ecuId, spec_.idDid, readId, sizeof(readId), 450) : false;
        if (!gotId) gotId = UdsHandler::readFamilyId(spec_.ecuId, readId, sizeof(readId), 450);
        if (gotId && readId[0]) {
            strncpy(idString_, readId, sizeof(idString_) - 1); idString_[sizeof(idString_) - 1] = 0;
            activeProfile_ = classify(idString_);
        } else if (activeProfile_ < 0) {
            activeProfile_ = spec_.defaultProfile; idString_[0] = 0;
        }
    }
    if (activeProfile_ < 0) setMsg("No special functions are defined for %s %s in the database - choose a profile manually", spec_.tag, idString_[0] ? idString_ : "?");
    else                    setMsg("%s special functions ready (%s). Read a value first, then change it.", spec_.tag, spec_.profiles[activeProfile_].name);
    if (statusMessage && statusMessageLength) snprintf(statusMessage, statusMessageLength, "%s", msg_);
    return true;
}

void CfgSpecialEngine::exitPage(const char *reason) {
    if (sessionActive_) {
        const uint8_t defaultSessionRequest[2] = {0x10, 0x01};
        UdsRawResult rawResult;
        tx(defaultSessionRequest, 2, rawResult);
    }
    sessionActive_ = sessionUnlocked_ = pageActive_ = false;
    setMsg("Session ended (%s) - default session restored", reason ? reason : "exit");
}

// Atomic-session helper: every public operation opens the extended session (ensureSession/unlock via
// readBlock's caller), does its work, then calls this to return the module to the default session. So the
// session never persists between button presses - there is nothing to keep alive and no manual "end session".
void CfgSpecialEngine::closeSession() {
    if (sessionActive_) {
        const uint8_t defaultSessionRequest[2] = {0x10, 0x01};
        UdsRawResult rawResult;
        tx(defaultSessionRequest, 2, rawResult);
    }
    sessionActive_ = sessionUnlocked_ = false;
}

// Closes the session on every return path of the operation it is declared in.
namespace { struct CfgSessionGuard { CfgSpecialEngine *engine; ~CfgSessionGuard() { engine->closeSession(); } }; }

bool CfgSpecialEngine::setProfile(int profileIndex) {
    if (profileIndex < -1 || profileIndex >= spec_.profileCount) return false;
    if (profileIndex == -1) { profileManuallySet_ = false; activeProfile_ = idString_[0] ? classify(idString_) : spec_.defaultProfile; }
    else                    { profileManuallySet_ = true;  activeProfile_ = profileIndex; }
    for (int fnIndex = 0; fnIndex < CFG_MAX_FNS; fnIndex++) { originalByte_[fnIndex] = -1; originalByte2_[fnIndex] = -1; }   // indices belong to the profile
    setMsg("Profile %s", activeProfile_ >= 0 ? spec_.profiles[activeProfile_].name : "none");
    return true;
}

bool CfgSpecialEngine::read(const char *testKey, char *outputBuffer, size_t outputBufferLength) {
    CfgSessionGuard guard{this};        // atomic: return to the default session on every exit path
    int fnIndex = findFn(testKey);
    if (fnIndex < 0) { setMsg("Unknown function"); reply(outputBuffer, outputBufferLength, false, ""); return false; }
    const CfgFn &fn = profile()->fns[fnIndex];
    Block block;
    if (!ensureSession() || !readBlock(fn, fn.did, fn.pos, block)) { reply(outputBuffer, outputBufferLength, false, ""); return false; }
    const uint8_t currentByte = block.bytes[fn.pos];
    if (originalByte_[fnIndex] < 0) originalByte_[fnIndex] = currentByte;
    if (fn.did2 && originalByte2_[fnIndex] < 0) {
        Block block2;
        if (readBlock(fn, fn.did2, fn.pos2, block2)) originalByte2_[fnIndex] = block2.bytes[fn.pos2];
    }
    int optionIndex = optionOf(fn, currentByte);
    const char *optionText = optionIndex >= 0 ? fn.opts[optionIndex].text : "unknown value";
    setMsg("%s: %s", fn.name, optionText);
    char extraJson[200];
    snprintf(extraJson, sizeof(extraJson), "\"option\":%d,\"text\":\"%s\",\"byte\":%u,\"changed\":%s", optionIndex < 0 ? 0 : optionIndex, optionText, currentByte, (originalByte_[fnIndex] != currentByte) ? "true" : "false");
    reply(outputBuffer, outputBufferLength, true, extraJson);
    return true;
}

bool CfgSpecialEngine::run(const char *testKey, int option, bool confirmed, char *outputBuffer, size_t outputBufferLength) {
    CfgSessionGuard guard{this};        // atomic: return to the default session on every exit path
    int fnIndex = findFn(testKey);
    if (fnIndex < 0) { setMsg("Unknown function"); reply(outputBuffer, outputBufferLength, false, ""); return false; }
    const CfgFn &fn = profile()->fns[fnIndex];
    if (option < 0 || option >= fn.nopt) { setMsg("Invalid option"); reply(outputBuffer, outputBufferLength, false, ""); return false; }
    if (fn.warn[0] && !confirmed) { setMsg("Confirmation required"); reply(outputBuffer, outputBufferLength, false, ""); return false; }
    Block block;
    if (!ensureSession() || !readBlock(fn, fn.did, fn.pos, block)) { reply(outputBuffer, outputBufferLength, false, ""); return false; }
    const uint8_t currentByte = block.bytes[fn.pos];
    if (originalByte_[fnIndex] < 0) originalByte_[fnIndex] = currentByte;
    const uint8_t newByte = (uint8_t)((currentByte & ~fn.mask) | (fn.opts[option].val & fn.mask));
    if (newByte == currentByte) {
        setMsg("%s is already %s - nothing written", fn.name, fn.opts[option].text);
        char extraJson[96]; snprintf(extraJson, sizeof(extraJson), "\"option\":%d,\"text\":\"%s\",\"byte\":%u", option, fn.opts[option].text, currentByte);
        reply(outputBuffer, outputBufferLength, true, extraJson);
        return true;
    }
    block.bytes[fn.pos] = newByte;
    if (!writeBlock(fn, fn.did, block)) { reply(outputBuffer, outputBufferLength, false, ""); return false; }

    Block verifyBlock;
    const bool readSucceeded = readBlock(fn, fn.did, fn.pos, verifyBlock);
    if (!readSucceeded || optionOf(fn, verifyBlock.bytes[fn.pos]) != option) {
        char reasonText[96] = {0};
        if (!readSucceeded) snprintf(reasonText, sizeof(reasonText), " (%.80s)", msg_);
        block.bytes[fn.pos] = currentByte;
        writeBlock(fn, fn.did, block);                               // put the old value back
        if (readSucceeded) setMsg("%s: verification failed - previous value written back", fn.name);
        else               setMsg("%s: could not verify the write%s - previous value written back", fn.name, reasonText);
        reply(outputBuffer, outputBufferLength, false, "");
        return false;
    }
    Serial.printf("[%s-SPC] %s: DID 0x%04X byte %u 0x%02X -> 0x%02X\n", spec_.tag, fn.key, fn.did, fn.pos + 1, currentByte, newByte);

    // Linked second field (the database writes it in step with the primary choice)
    if (fn.did2) {
        Block block2;
        if (readBlock(fn, fn.did2, fn.pos2, block2)) {
            if (originalByte2_[fnIndex] < 0) originalByte2_[fnIndex] = block2.bytes[fn.pos2];
            if (!writeField(fn, fn.did2, fn.pos2, fn.mask2, fn.opts[option].val2, block2)) {
                setMsg("%s set to %s, but the linked DID 0x%04X write failed: %.60s", fn.name, fn.opts[option].text, fn.did2, msg_);
                reply(outputBuffer, outputBufferLength, false, "");
                return false;
            }
        }
    }
    setMsg("%s set to %s", fn.name, fn.opts[option].text);
    char extraJson[96]; snprintf(extraJson, sizeof(extraJson), "\"option\":%d,\"text\":\"%s\",\"byte\":%u,\"changed\":true", option, fn.opts[option].text, newByte);
    reply(outputBuffer, outputBufferLength, true, extraJson);
    return true;
}

bool CfgSpecialEngine::restore(const char *testKey, char *outputBuffer, size_t outputBufferLength) {
    CfgSessionGuard guard{this};        // atomic: return to the default session on every exit path
    int fnIndex = findFn(testKey);
    if (fnIndex < 0 || originalByte_[fnIndex] < 0) { setMsg("Nothing to restore - read the value first"); reply(outputBuffer, outputBufferLength, false, ""); return false; }
    const CfgFn &fn = profile()->fns[fnIndex];
    Block block;
    if (!ensureSession()) { reply(outputBuffer, outputBufferLength, false, ""); return false; }
    // only this function's own bits go back, other options sharing the byte keep their current value
    if (!writeField(fn, fn.did, fn.pos, fn.rmask, (uint8_t)originalByte_[fnIndex], block)) { reply(outputBuffer, outputBufferLength, false, ""); return false; }
    if (fn.did2 && originalByte2_[fnIndex] >= 0) {
        Block block2;
        writeField(fn, fn.did2, fn.pos2, fn.mask2, (uint8_t)originalByte2_[fnIndex], block2);
    }
    setMsg("%s: original value restored", fn.name);
    reply(outputBuffer, outputBufferLength, true, "");
    return true;
}

void CfgSpecialEngine::loop() {
    if (!sessionActive_) return;
    const uint32_t now = millis();
    if (now - lastOperationMs_ >= CFGS_IDLE_EXIT_MS) { exitPage("idle timeout"); return; }
    if (now - lastTesterPresentMs_ >= CFGS_KEEPALIVE_MS) {
        lastTesterPresentMs_ = now;
        const uint8_t testerPresentFrame[8] = {0x02, 0x3E, 0x80, 0, 0, 0, 0, 0};
        CanDriver::transmit(spec_.ecuId, false, false, 8, testerPresentFrame);
    }
}

static bool cfgAppend(char *outputBuffer, size_t maxLength, size_t &writtenLength, const char *formatText, ...) __attribute__((format(printf, 4, 5)));
static bool cfgAppend(char *outputBuffer, size_t maxLength, size_t &writtenLength, const char *formatText, ...) {
    va_list argList; va_start(argList, formatText);
    int appendedLength = vsnprintf(outputBuffer + writtenLength, maxLength - writtenLength, formatText, argList);
    va_end(argList);
    if (appendedLength < 0 || (size_t)appendedLength >= maxLength - writtenLength) return false;
    writtenLength += (size_t)appendedLength;
    return true;
}

// ---------------------------------------------------------------------------
// Generic "custom card" helpers - VIN text (DID resolved per module by EcuInfo::vinDid(), same as Read ECU
// Information) and a plain hex block read, for functions that don't fit the CfgFn bit-option model.
// ---------------------------------------------------------------------------

bool CfgSpecialEngine::readVin(uint16_t did, char *outputBuffer, size_t outputBufferLength) {
    CfgSessionGuard guard{this};        // atomic: return to the default session on every exit path
    if (!ensureSession()) { reply(outputBuffer, outputBufferLength, false, ""); return false; }
    const uint8_t requestBytes[3] = {0x22, (uint8_t)(did >> 8), (uint8_t)(did & 0xFF)};
    UdsRawResult rawResult;
    bool ok = false;
    for (uint8_t attempt = 0; attempt < CFG_READ_TRIES; attempt++) {
        if (attempt) delay(CFG_SESSION_RETRY_MS);
        ok = tx(requestBytes, 3, rawResult);
        if (ok || rawResult.got_reply) break;
    }
    if (!ok || rawResult.len < 3 + 17) {
        setMsg("VIN read failed: %s", rawResult.got_reply ? cfgNrcText(rawResult.nrc) : "no response after retries");
        reply(outputBuffer, outputBufferLength, false, "");
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
    setMsg("Current VIN: %s", vinText);
    char extraJson[48];
    snprintf(extraJson, sizeof(extraJson), "\"vin\":\"%s\"", vinText);
    reply(outputBuffer, outputBufferLength, true, extraJson);
    return true;
}

bool CfgSpecialEngine::writeVin(uint16_t did, const char *vin, char *outputBuffer, size_t outputBufferLength) {
    CfgSessionGuard guard{this};        // atomic: return to the default session on every exit path
    size_t vinLength = vin ? strlen(vin) : 0;
    if (vinLength != 17) { setMsg("VIN must be exactly 17 characters (got %u)", (unsigned)vinLength); reply(outputBuffer, outputBufferLength, false, ""); return false; }
    if (!ensureSession() || !unlock()) { reply(outputBuffer, outputBufferLength, false, ""); return false; }
    uint8_t requestBytes[3 + 17] = {0x2E, (uint8_t)(did >> 8), (uint8_t)(did & 0xFF)};
    memcpy(requestBytes + 3, vin, 17);
    UdsRawResult rawResult;
    if (!tx(requestBytes, (uint8_t)sizeof(requestBytes), rawResult)) {
        setMsg("VIN write refused: %s (NRC 0x%02X)", cfgNrcText(rawResult.nrc), rawResult.nrc);
        reply(outputBuffer, outputBufferLength, false, "");
        return false;
    }
    const uint8_t verifyRequest[3] = {0x22, (uint8_t)(did >> 8), (uint8_t)(did & 0xFF)};
    UdsRawResult verifyResult;
    bool verified = tx(verifyRequest, 3, verifyResult) && verifyResult.len >= 3 + 17 && memcmp(verifyResult.data + 3, vin, 17) == 0;
    if (verified) setMsg("VIN written and verified: %.17s", vin);
    else          setMsg("VIN write sent, but could not be verified on read-back - check the %s: %.17s", spec_.tag, vin);
    char extraJson[48];
    snprintf(extraJson, sizeof(extraJson), "\"vin\":\"%s\"", vin);
    reply(outputBuffer, outputBufferLength, true, extraJson);
    return true;
}

bool CfgSpecialEngine::readHexBlock(uint16_t did, uint8_t minLength, char *hexOutput, size_t hexOutputLength, char *statusMessage, size_t statusMessageLength) {
    CfgSessionGuard guard{this};        // atomic: return to the default session on every exit path
    if (hexOutputLength) hexOutput[0] = 0;
    if (!ensureSession()) { if (statusMessage) snprintf(statusMessage, statusMessageLength, "%s", msg_); return false; }
    const uint8_t requestBytes[3] = {0x22, (uint8_t)(did >> 8), (uint8_t)(did & 0xFF)};
    // A multi-byte block needs ISO-TP multi-frame (First Frame + Consecutive Frame(s)); a dropped or corrupted
    // Consecutive Frame over the flaky bridge still looks like a valid, positive reply - just a short or wrong
    // one - so no single reply is trusted here, however long it is. Read up to READ_TRIES times and only
    // accept a value once at least NEEDED_MATCHES of those reads agree byte-for-byte with each other - that is
    // what a clean, undropped read looks like, and a one-off short/garbled reply just won't get a second vote.
    static const uint8_t READ_TRIES     = 6;
    static const uint8_t NEEDED_MATCHES = 3;
    struct Candidate { uint8_t len; uint8_t bytes[CFG_MAX_BLOCK]; uint8_t votes; };
    Candidate candidates[READ_TRIES];
    uint8_t candidateCount = 0;
    bool anyReply = false; uint8_t lastNrc = 0;
    bool consensusReached = false;
    for (uint8_t attempt = 0; attempt < READ_TRIES && !consensusReached; attempt++) {
        if (attempt) delay(CFG_SESSION_RETRY_MS);
        UdsRawResult rawResult;
        bool ok = tx(requestBytes, 3, rawResult);
        if (!ok && !rawResult.got_reply) continue;                         // no response at all - just retry
        if (!ok) { anyReply = true; lastNrc = rawResult.nrc; continue; }    // negative reply - retry
        anyReply = true;
        uint8_t dataLength = (rawResult.len >= 3) ? (uint8_t)(rawResult.len - 3) : 0;
        if (dataLength > CFG_MAX_BLOCK) dataLength = CFG_MAX_BLOCK;
        int matchedIndex = -1;
        for (uint8_t i = 0; i < candidateCount; i++)
            if (candidates[i].len == dataLength && memcmp(candidates[i].bytes, &rawResult.data[3], dataLength) == 0) { matchedIndex = (int)i; break; }
        if (matchedIndex < 0 && candidateCount < READ_TRIES) {
            matchedIndex = candidateCount++;
            candidates[matchedIndex].len = dataLength;
            memcpy(candidates[matchedIndex].bytes, &rawResult.data[3], dataLength);
            candidates[matchedIndex].votes = 0;
        }
        if (matchedIndex >= 0 && ++candidates[matchedIndex].votes >= NEEDED_MATCHES) consensusReached = true;
    }
    int bestIndex = -1;
    for (uint8_t i = 0; i < candidateCount; i++) if (bestIndex < 0 || candidates[i].votes > candidates[bestIndex].votes) bestIndex = (int)i;
    if (bestIndex < 0 || candidates[bestIndex].votes < NEEDED_MATCHES) {
        if (candidateCount == 0) setMsg("DID 0x%04X read failed: %s", did, anyReply ? cfgNrcText(lastNrc) : "no response after retries");
        else                     setMsg("DID 0x%04X: %u read(s) did not agree (best: %u/%u matching) - a CAN frame was likely dropped, try Read again", did, (unsigned)READ_TRIES, candidates[bestIndex].votes, (unsigned)READ_TRIES);
        if (statusMessage) snprintf(statusMessage, statusMessageLength, "%s", msg_);
        return false;
    }
    const uint8_t bestLength = candidates[bestIndex].len;
    size_t writtenLength = 0;
    for (uint8_t byteIndex = 0; byteIndex < bestLength && writtenLength + 3 <= hexOutputLength; byteIndex++)
        writtenLength += (size_t)snprintf(hexOutput + writtenLength, hexOutputLength - writtenLength, "%02X", candidates[bestIndex].bytes[byteIndex]);
    const bool complete = bestLength >= minLength;
    if (complete) setMsg("DID 0x%04X: %u byte(s) read (%u/%u reads agreed)", did, bestLength, candidates[bestIndex].votes, (unsigned)READ_TRIES);
    else          setMsg("DID 0x%04X: %u/%u reads agreed on only %u of %u expected byte(s) - try Read again", did, candidates[bestIndex].votes, (unsigned)READ_TRIES, bestLength, minLength);
    if (statusMessage) snprintf(statusMessage, statusMessageLength, "%s", msg_);
    return complete;
}

// Plain hex text (no "0x"/separators) -> bytes. Used by writeHexBlock() for data captured by readHexBlock() or
// pasted/loaded from a saved file, both of which are in that same plain-hex format.
static bool cfgParseHex(const char *hexText, uint8_t *outBytes, uint8_t maxBytes, uint8_t *outLength) {
    size_t hexLength = hexText ? strlen(hexText) : 0;
    if (hexLength == 0 || (hexLength % 2) != 0 || (hexLength / 2) > maxBytes) return false;
    uint8_t byteCount = (uint8_t)(hexLength / 2);
    for (uint8_t byteIndex = 0; byteIndex < byteCount; byteIndex++) {
        unsigned int byteValue = 0;
        if (sscanf(hexText + byteIndex * 2, "%2x", &byteValue) != 1) return false;
        outBytes[byteIndex] = (uint8_t)byteValue;
    }
    *outLength = byteCount;
    return true;
}

bool CfgSpecialEngine::writeHexBlock(uint16_t did, const char *hexInput, char *statusMessage, size_t statusMessageLength) {
    CfgSessionGuard guard{this};        // atomic: return to the default session on every exit path
    uint8_t blockBytes[CFG_MAX_BLOCK]; uint8_t blockLength = 0;
    if (!cfgParseHex(hexInput, blockBytes, CFG_MAX_BLOCK, &blockLength)) {
        setMsg("DID 0x%04X: invalid hex data (expected an even number of hex digits)", did);
        if (statusMessage) snprintf(statusMessage, statusMessageLength, "%s", msg_);
        return false;
    }
    if (!ensureSession() || !unlock()) { if (statusMessage) snprintf(statusMessage, statusMessageLength, "%s", msg_); return false; }
    uint8_t requestBytes[3 + CFG_MAX_BLOCK];
    requestBytes[0] = 0x2E; requestBytes[1] = (uint8_t)(did >> 8); requestBytes[2] = (uint8_t)(did & 0xFF);
    memcpy(requestBytes + 3, blockBytes, blockLength);
    UdsRawResult rawResult;
    if (!tx(requestBytes, (uint8_t)(3 + blockLength), rawResult)) {
        setMsg("DID 0x%04X write refused: %s (NRC 0x%02X)", did, cfgNrcText(rawResult.nrc), rawResult.nrc);
        if (statusMessage) snprintf(statusMessage, statusMessageLength, "%s", msg_);
        return false;
    }
    // Verify by read-back. This block is multi-frame, so a dropped Consecutive Frame on the VERIFY read can
    // make a perfectly good write look like it failed - retry the verify read a few times before reporting a
    // mismatch (the write itself is a single 2E, already acknowledged above, so it is not repeated here).
    const uint8_t verifyRequest[3] = {0x22, (uint8_t)(did >> 8), (uint8_t)(did & 0xFF)};
    bool verified = false;
    for (uint8_t attempt = 0; attempt < 3 && !verified; attempt++) {
        if (attempt) delay(CFG_SESSION_RETRY_MS);
        UdsRawResult verifyResult;
        if (!tx(verifyRequest, 3, verifyResult)) continue;
        uint8_t verifyLength = (verifyResult.len >= 3) ? (uint8_t)(verifyResult.len - 3) : 0;
        verified = verifyLength == blockLength && memcmp(verifyResult.data + 3, blockBytes, blockLength) == 0;
    }
    if (verified) setMsg("DID 0x%04X: %u byte(s) written and verified", did, blockLength);
    else          setMsg("DID 0x%04X: write sent, but could not be verified on read-back - check the %s", did, spec_.tag);
    if (statusMessage) snprintf(statusMessage, statusMessageLength, "%s", msg_);
    return verified;
}

size_t CfgSpecialEngine::catalogJson(char *outputBuffer, size_t maxOutputLength) {
    if (!outputBuffer || maxOutputLength < 512) return 0;
    size_t writtenLength = 0;
    if (!cfgAppend(outputBuffer, maxOutputLength, writtenLength, "{\"type\":\"%s\",\"ok\":true,\"cal\":\"%s\",\"auto\":%s,\"profile\":%d,\"profile_name\":\"%s\",\"profiles\":[",
                   spec_.jsonType, idString_, profileManuallySet_ ? "false" : "true", activeProfile_, activeProfile_ >= 0 ? spec_.profiles[activeProfile_].name : "none")) return 0;
    if (spec_.profileCount > 1)
        for (int profileIndex = 0; profileIndex < spec_.profileCount; profileIndex++)
            if (!cfgAppend(outputBuffer, maxOutputLength, writtenLength, "%s{\"id\":%d,\"name\":\"%s\",\"count\":%u}", profileIndex ? "," : "", profileIndex, spec_.profiles[profileIndex].name, spec_.profiles[profileIndex].count)) return 0;
    if (!cfgAppend(outputBuffer, maxOutputLength, writtenLength, "],\"functions\":[")) return 0;
    const CfgProfile *profileDef = profile();
    for (int fnIndex = 0; profileDef && fnIndex < profileDef->count; fnIndex++) {
        const CfgFn &fn = profileDef->fns[fnIndex];
        if (!cfgAppend(outputBuffer, maxOutputLength, writtenLength, "%s{\"key\":\"%s\",\"name\":\"%s\",\"desc\":\"%s\",\"type\":6,\"warn\":\"%s\",\"opts\":[", fnIndex ? "," : "", fn.key, fn.name, fn.desc, fn.warn)) return 0;
        for (uint8_t optionIndex = 0; optionIndex < fn.nopt; optionIndex++)
            if (!cfgAppend(outputBuffer, maxOutputLength, writtenLength, "%s\"%s\"", optionIndex ? "," : "", fn.opts[optionIndex].text)) return 0;
        if (!cfgAppend(outputBuffer, maxOutputLength, writtenLength, "]}")) return 0;
    }
    if (!cfgAppend(outputBuffer, maxOutputLength, writtenLength, "]}")) return 0;
    return writtenLength;
}
