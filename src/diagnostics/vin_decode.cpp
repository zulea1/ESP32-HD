/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file vin_decode.cpp
 * @brief VIN decoding against the generated VIN table (see vin_decode.h).
 */
#include "vin_decode.h"
#include "vin_data.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *countryFromWmi(const char *vin) {
    if (!strncmp(vin, "1HD", 3) || !strncmp(vin, "5HD", 3)) return "USA";
    if (!strncmp(vin, "932", 3)) return "Brazil";
    if (!strncmp(vin, "MEG", 3)) return "India";
    if (!strncmp(vin, "MLY", 3)) return "Thailand";
    return "";
}

// VIN character 10 -> model year (2010 .. 2030 cycle for letters, 2001..2009 for digits); the older cycle is year - 30
static int yearCandidate(char modelYearChar) {
    static const char yearCycleCodes[] = "ABCDEFGHJKLMNPRSTVWXY";     // 2010 .. 2030
    for (int codeIndex = 0; yearCycleCodes[codeIndex]; codeIndex++) if (yearCycleCodes[codeIndex] == modelYearChar) return 2010 + codeIndex;
    if (modelYearChar >= '1' && modelYearChar <= '9') return 2000 + (modelYearChar - '0');
    return 0;
}

bool vinDecode(const char *vinInput, VinInfo &decodedInfo) {
    memset(&decodedInfo, 0, sizeof(decodedInfo));
    if (!vinInput || strlen(vinInput) != 17) return false;
    char upperCaseVin[18];
    for (int charIndex = 0; charIndex < 17; charIndex++) upperCaseVin[charIndex] = (char)toupper((unsigned char)vinInput[charIndex]);
    upperCaseVin[17] = 0;
    const char patternKey[7] = {upperCaseVin[0], upperCaseVin[1], upperCaseVin[2], upperCaseVin[4], upperCaseVin[5], upperCaseVin[6], upperCaseVin[9]};

    for (uint16_t entryIndex = 0; entryIndex < VIN_TABLE_COUNT; entryIndex++) {
        const VinEntry &tableEntry = VIN_TABLE[entryIndex];
        bool patternMatches = true;
        for (uint8_t positionIndex = 0; positionIndex < 7 && patternMatches; positionIndex++) {
            if (tableEntry.patternChars[positionIndex] != '#' && tableEntry.patternChars[positionIndex] != patternKey[positionIndex]) patternMatches = false;
        }
        if (!patternMatches) continue;

        decodedInfo.manufacturer = VIN_MAN[tableEntry.manufacturerIndex];
        decodedInfo.model        = VIN_FAM[tableEntry.familyIndex];
        decodedInfo.engine       = VIN_ENG[tableEntry.engineIndex];
        decodedInfo.country      = countryFromWmi(upperCaseVin);
        const char *typeAndRangeText = VIN_TYP[tableEntry.typeIndex];
        const char *rangeSeparator = strchr(typeAndRangeText, '|');
        size_t typeTextLength = rangeSeparator ? (size_t)(rangeSeparator - typeAndRangeText) : strlen(typeAndRangeText);
        if (typeTextLength >= sizeof(decodedInfo.type)) typeTextLength = sizeof(decodedInfo.type) - 1;
        memcpy(decodedInfo.type, typeAndRangeText, typeTextLength); decodedInfo.type[typeTextLength] = 0;

        int modelYearRangeLow = 0, modelYearRangeHigh = 9999;
        if (rangeSeparator && rangeSeparator[1] == '(') {
            modelYearRangeLow = atoi(rangeSeparator + 2);
            const char *rangeDash = strchr(rangeSeparator, '-');
            const char *rangeClose = strchr(rangeSeparator, ')');
            if (rangeDash && rangeClose && rangeDash < rangeClose) {
                modelYearRangeHigh = (rangeDash + 1 < rangeClose) ? atoi(rangeDash + 1) : 9999;
            } else {
                modelYearRangeHigh = modelYearRangeLow;
            }
            size_t rangeTextLength = rangeClose ? (size_t)(rangeClose - (rangeSeparator + 2)) : 0;
            if (rangeTextLength >= sizeof(decodedInfo.range)) rangeTextLength = sizeof(decodedInfo.range) - 1;
            memcpy(decodedInfo.range, rangeSeparator + 2, rangeTextLength); decodedInfo.range[rangeTextLength] = 0;
        }
        int candidateYear = yearCandidate(upperCaseVin[9]);
        if (candidateYear) {
            if (candidateYear >= modelYearRangeLow && candidateYear <= modelYearRangeHigh) {
                decodedInfo.year = candidateYear;
            } else if (candidateYear - 30 >= modelYearRangeLow && candidateYear - 30 <= modelYearRangeHigh) {
                decodedInfo.year = candidateYear - 30;
            } else {
                decodedInfo.year = candidateYear; // outside the database range: keep the VIN's most recent cycle
            }
        }
        return true;
    }
    return false;
}
