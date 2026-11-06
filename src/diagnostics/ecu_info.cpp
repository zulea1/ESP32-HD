/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file ecu_info.cpp
 * @brief Database driven ECU information reader (see ecu_info.h).
 */
#include "ecu_info.h"
#include "config.h"
#include "uds_handler.h"
#include "ecu_info_data.h"
#include "vin_decode.h"
#include <string.h>
#include <stdio.h>

static const InfoModuleDef *activeModule = nullptr;
static const InfoSetDef    *activeInfoSet = nullptr;

struct DidCache { uint16_t did; bool valid; bool ok; uint8_t nrc; uint8_t len; uint8_t data[96]; };
static DidCache didReadCache[4];
static uint8_t  nextCacheSlotIndex = 0;

static const InfoModuleDef *findModule(uint32_t moduleId) {
    for (uint8_t moduleIndex = 0; moduleIndex < INFO_MODULE_COUNT; moduleIndex++) {
        if (INFO_MODULES[moduleIndex].id == moduleId) return &INFO_MODULES[moduleIndex];
    }
    return nullptr;
}

static size_t jsonEsc(const char *inputText, char *outputBuffer, size_t maxOutputLength) {
    size_t writtenLength = 0;
    for (; inputText && *inputText && writtenLength + 7 < maxOutputLength; inputText++) {
        unsigned char currentChar = (unsigned char)*inputText;
        if (currentChar == '"' || currentChar == '\\') { outputBuffer[writtenLength++] = '\\'; outputBuffer[writtenLength++] = (char)currentChar; }
        else if (currentChar < 0x20 || currentChar > 0x7E) { outputBuffer[writtenLength++] = '?'; }
        else outputBuffer[writtenLength++] = (char)currentChar;
    }
    outputBuffer[writtenLength] = 0;
    return writtenLength;
}

bool EcuInfo::plan(uint32_t moduleId, char *outputJsonBuffer, size_t outputBufferLength) {
    activeModule = findModule(moduleId);
    activeInfoSet = nullptr;
    memset(didReadCache, 0, sizeof(didReadCache));
    if (!activeModule) { snprintf(outputJsonBuffer, outputBufferLength, "{\"ok\":false,\"error\":\"no information list for this module\"}"); return false; }

    char identificationString[48] = {0};
    bool gotIdentification = UdsHandler::readFamilyId(moduleId, identificationString, sizeof(identificationString), 350);
    uint8_t selectedSetIndex = activeModule->defaultSetIndex;
    size_t bestPrefixLength = 0;
    if (gotIdentification) {
        for (uint8_t ruleIndex = 0; ruleIndex < activeModule->ruleCount; ruleIndex++) {
            size_t prefixLength = strlen(activeModule->rules[ruleIndex].prefix);
            if (prefixLength > bestPrefixLength && strncmp(identificationString, activeModule->rules[ruleIndex].prefix, prefixLength) == 0) {
                bestPrefixLength = prefixLength;
                selectedSetIndex = activeModule->rules[ruleIndex].setIndex;
            }
        }
    }
    if (selectedSetIndex >= activeModule->setCount) selectedSetIndex = 0;
    activeInfoSet = &activeModule->sets[selectedSetIndex];

    char escapedSourceText[64];
    jsonEsc(identificationString, escapedSourceText, sizeof(escapedSourceText));
    size_t writtenLength = (size_t)snprintf(outputJsonBuffer, outputBufferLength, "{\"ok\":true,\"module\":\"%s\",\"set\":\"%s\",\"src\":\"%s\",\"items\":[", activeModule->name, activeInfoSet->name, escapedSourceText);
    for (uint8_t itemIndex = 0; itemIndex < activeInfoSet->count; itemIndex++) {
        char escapedLabel[96];
        jsonEsc(activeInfoSet->items[itemIndex].label, escapedLabel, sizeof(escapedLabel));
        int appendedLength = snprintf(outputJsonBuffer + writtenLength, outputBufferLength - writtenLength, "%s\"%s\"", itemIndex ? "," : "", escapedLabel);
        if (appendedLength < 0 || (size_t)appendedLength >= outputBufferLength - writtenLength) {
            snprintf(outputJsonBuffer, outputBufferLength, "{\"ok\":false,\"error\":\"too many items\"}");
            return false;
        }
        writtenLength += (size_t)appendedLength;
    }
    int closingLength = snprintf(outputJsonBuffer + writtenLength, outputBufferLength - writtenLength, "]}");
    return closingLength > 0 && (size_t)closingLength < outputBufferLength - writtenLength;
}

static DidCache *readDid(uint32_t moduleId, uint16_t did) {
    for (auto &cacheEntry : didReadCache) if (cacheEntry.valid && cacheEntry.did == did) return &cacheEntry;
    DidCache *cacheSlot = &didReadCache[nextCacheSlotIndex++ % 4];
    memset(cacheSlot, 0, sizeof(*cacheSlot));
    cacheSlot->did = did; cacheSlot->valid = true;
    uint8_t requestBytes[3] = {0x22, (uint8_t)(did >> 8), (uint8_t)(did & 0xFF)};
    for (uint8_t attemptIndex = 0; attemptIndex < 2; attemptIndex++) {
        UdsRawResult rawResult;
        UdsHandler::rawTransaction(moduleId, requestBytes, 3, rawResult, 500);
        if (rawResult.got_reply && rawResult.positive && rawResult.len >= 3) {
            cacheSlot->ok = true; cacheSlot->len = rawResult.len - 3;
            memcpy(cacheSlot->data, rawResult.data + 3, cacheSlot->len);
            return cacheSlot;
        }
        if (rawResult.got_reply) { cacheSlot->nrc = rawResult.nrc; return cacheSlot; }     // negative response: no retry
        delay(60);
    }
    return cacheSlot;
}

static void decode(const InfoItem &item, const uint8_t *rawData, uint8_t rawDataLength, char *outputText, size_t maxOutputLength) {
    outputText[0] = 0;
    if (item.kind != IK_ASCII && (size_t)item.bytePosition + item.byteLength > rawDataLength) { snprintf(outputText, maxOutputLength, "N/A"); return; }
    switch (item.kind) {
        case IK_ASCII: {
            size_t writtenLength = 0;
            for (uint8_t byteIndex = item.bytePosition; byteIndex < rawDataLength && writtenLength + 1 < maxOutputLength; byteIndex++) {
                if (item.byteLength && byteIndex >= item.bytePosition + item.byteLength) break;
                uint8_t byteValue = rawData[byteIndex];
                if (byteValue >= 0x20 && byteValue <= 0x7E) outputText[writtenLength++] = (char)byteValue;
            }
            while (writtenLength && outputText[writtenLength - 1] == ' ') writtenLength--;
            outputText[writtenLength] = 0;
            if (!writtenLength) snprintf(outputText, maxOutputLength, "N/A");
            break;
        }
        case IK_HEX: {
            size_t writtenLength = 0;
            for (uint8_t byteIndex = 0; byteIndex < item.byteLength && writtenLength + 4 < maxOutputLength; byteIndex++) {
                writtenLength += (size_t)snprintf(outputText + writtenLength, maxOutputLength - writtenLength, "%s%02X", byteIndex ? " " : "", rawData[item.bytePosition + byteIndex]);
            }
            break;
        }
        case IK_DATE: {
            size_t writtenLength = 0;
            for (uint8_t byteIndex = 0; byteIndex < item.byteLength && writtenLength + 3 < maxOutputLength; byteIndex++) {
                writtenLength += (size_t)snprintf(outputText + writtenLength, maxOutputLength - writtenLength, "%02X", rawData[item.bytePosition + byteIndex]);
            }
            break;
        }
        case IK_NUM: {
            uint32_t rawValue = 0;
            for (uint8_t byteIndex = 0; byteIndex < item.byteLength; byteIndex++) rawValue = (rawValue << 8) | rawData[item.bytePosition + byteIndex];
            double scaledValue = (double)rawValue * (double)item.scale + (double)item.offset;
            if (item.decimalPlaces) snprintf(outputText, maxOutputLength, "%.1f", scaledValue); else snprintf(outputText, maxOutputLength, "%.0f", scaledValue);
            break;
        }
        case IK_MASK:
            snprintf(outputText, maxOutputLength, "%s", (rawData[item.bytePosition] & item.mask) == item.mask ? item.on : item.off);
            break;
        case IK_ENUM: {
            const InfoEnum &enumDef = INFO_ENUMS[item.enumId];
            const char *matchedText = enumDef.defaultText;
            for (uint8_t entryIndex = 0; entryIndex < enumDef.count; entryIndex++) {
                if (enumDef.entries[entryIndex].value == rawData[item.bytePosition]) { matchedText = enumDef.entries[entryIndex].text; break; }
            }
            snprintf(outputText, maxOutputLength, "%s", matchedText);
            break;
        }
    }
}

bool EcuInfo::item(uint32_t moduleId, uint8_t itemIndex, char *outputJsonBuffer, size_t outputBufferLength) {
    if (!activeModule || !activeInfoSet || activeModule->id != moduleId || itemIndex >= activeInfoSet->count) {
        snprintf(outputJsonBuffer, outputBufferLength, "{\"ok\":false,\"error\":\"no plan\"}");
        return false;
    }
    const InfoItem &selectedItem = activeInfoSet->items[itemIndex];
    DidCache *cacheResult = readDid(moduleId, selectedItem.did);
    char decodedValue[200] = "N/A";
    bool decodedSuccessfully = false;
    if (cacheResult && cacheResult->ok) {
        decode(selectedItem, cacheResult->data, cacheResult->len, decodedValue, sizeof(decodedValue));
        decodedSuccessfully = strcmp(decodedValue, "N/A") != 0;
    }
    char escapedValue[240];
    jsonEsc(decodedValue, escapedValue, sizeof(escapedValue));
    snprintf(outputJsonBuffer, outputBufferLength, "{\"ok\":true,\"i\":%u,\"got\":%s,\"value\":\"%s\",\"nrc\":%u}", itemIndex, decodedSuccessfully ? "true" : "false", escapedValue, cacheResult ? cacheResult->nrc : 0);
    return true;
}

// VIN lives in the ECM; other modules often answer NRC 0x31. Try the module first (0xF1EF, 0xF190), then the ECM.
static bool readVin(uint32_t moduleId, char *vinOutput, size_t vinOutputLength) {
    const uint32_t targetModuleIds[2] = {moduleId, 0x7E0};
    const uint16_t vinDids[2] = {0xF1EF, 0xF190};
    for (uint8_t targetIndex = 0; targetIndex < 2; targetIndex++) {
        if (targetIndex == 1 && moduleId == 0x7E0) break;
        for (uint8_t didIndex = 0; didIndex < 2; didIndex++) {
            const uint8_t requestBytes[3] = {0x22, (uint8_t)(vinDids[didIndex] >> 8), (uint8_t)(vinDids[didIndex] & 0xFF)};
            UdsRawResult rawResult;
            UdsHandler::rawTransaction(targetModuleIds[targetIndex], requestBytes, 3, rawResult, 500);
            if (rawResult.got_reply && rawResult.positive && rawResult.len >= 3 + 17) {
                size_t writtenLength = 0;
                for (uint8_t byteIndex = 3; byteIndex < rawResult.len && writtenLength + 1 < vinOutputLength; byteIndex++) {
                    if (rawResult.data[byteIndex] >= 0x21 && rawResult.data[byteIndex] <= 0x7E) vinOutput[writtenLength++] = (char)rawResult.data[byteIndex];
                }
                vinOutput[writtenLength] = 0;
                if (writtenLength == 17) return true;
            }
        }
    }
    return false;
}

uint16_t EcuInfo::vinDid(uint32_t moduleId) {
    const InfoModuleDef *moduleDef = findModule(moduleId);
    if (!moduleDef) return 0xF1EF;

    // Same identification + longest-prefix rule match plan() uses, kept separate so calling this doesn't
    // disturb an open "Read ECU Information" session's own activeModule/activeInfoSet.
    char identificationString[48] = {0};
    bool gotIdentification = UdsHandler::readFamilyId(moduleId, identificationString, sizeof(identificationString), 350);
    uint8_t selectedSetIndex = moduleDef->defaultSetIndex;
    size_t bestPrefixLength = 0;
    if (gotIdentification) {
        for (uint8_t ruleIndex = 0; ruleIndex < moduleDef->ruleCount; ruleIndex++) {
            size_t prefixLength = strlen(moduleDef->rules[ruleIndex].prefix);
            if (prefixLength > bestPrefixLength && strncmp(identificationString, moduleDef->rules[ruleIndex].prefix, prefixLength) == 0) {
                bestPrefixLength = prefixLength;
                selectedSetIndex = moduleDef->rules[ruleIndex].setIndex;
            }
        }
    }
    if (selectedSetIndex >= moduleDef->setCount) selectedSetIndex = 0;

    // Look for an item whose label mentions "VIN" - first in the matched set, then (if that set doesn't carry
    // one) across every set this module defines.
    const InfoSetDef &matchedSet = moduleDef->sets[selectedSetIndex];
    for (uint8_t itemIndex = 0; itemIndex < matchedSet.count; itemIndex++) {
        if (strstr(matchedSet.items[itemIndex].label, "VIN")) return matchedSet.items[itemIndex].did;
    }
    for (uint8_t setIndex = 0; setIndex < moduleDef->setCount; setIndex++) {
        const InfoSetDef &setDef = moduleDef->sets[setIndex];
        for (uint8_t itemIndex = 0; itemIndex < setDef.count; itemIndex++) {
            if (strstr(setDef.items[itemIndex].label, "VIN")) return setDef.items[itemIndex].did;
        }
    }
    return 0xF1EF;   // this module defines no VIN item at all - caller shouldn't have offered the card
}

bool EcuInfo::vinJson(uint32_t moduleId, char *outputJsonBuffer, size_t outputBufferLength) {
    char decodedVin[24] = {0};
    if (!readVin(moduleId, decodedVin, sizeof(decodedVin))) {
        snprintf(outputJsonBuffer, outputBufferLength, "{\"ok\":false,\"error\":\"VIN not available\"}");
        return false;
    }
    VinInfo vinInfo;
    char escapedVin[40], escapedModel[48], escapedEngine[96], escapedType[48];
    if (!vinDecode(decodedVin, vinInfo)) {
        jsonEsc(decodedVin, escapedVin, sizeof(escapedVin));
        snprintf(outputJsonBuffer, outputBufferLength, "{\"ok\":true,\"vin\":\"%s\",\"found\":false}", escapedVin);
        return true;
    }
    jsonEsc(decodedVin, escapedVin, sizeof(escapedVin));
    jsonEsc(vinInfo.model, escapedModel, sizeof(escapedModel));
    jsonEsc(vinInfo.engine, escapedEngine, sizeof(escapedEngine));
    jsonEsc(vinInfo.type, escapedType, sizeof(escapedType));
    snprintf(outputJsonBuffer, outputBufferLength,
        "{\"ok\":true,\"found\":true,\"vin\":\"%s\",\"manufacturer\":\"%s\",\"model\":\"%s\",\"engine\":\"%s\",\"type\":\"%s\",\"range\":\"%s\",\"year\":%d,\"country\":\"%s\"}",
        escapedVin, vinInfo.manufacturer, escapedModel, escapedEngine, escapedType, vinInfo.range, vinInfo.year, vinInfo.country);
    return true;
}
