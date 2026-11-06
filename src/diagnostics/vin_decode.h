/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file vin_decode.h
 * @brief VIN -> vehicle (manufacturer, family, engine, model-year range, year, country) using the VIN pattern tables.
 *
 * There is no country or weight table: the country is derived here from the WMI (first 3 VIN characters of the
 * patterns present); the exact year comes from VIN character 10 constrained to the known year range.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

struct VinInfo {
    const char *manufacturer;
    const char *model;        // family (Softail, Touring, ...)
    const char *engine;       // e.g. "1868 cc (Twin Cam 114 Milwaukee-Eight)"
    char        type[40];     // e.g. "Motorcycle (Custom)"
    char        range[16];    // e.g. "2019-"
    int         year;         // 0 = unknown
    const char *country;
};

bool vinDecode(const char *vin, VinInfo &decodedInfo);
