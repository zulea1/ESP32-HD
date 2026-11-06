/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file abs_algo.cpp
 * @brief ABS (0x7E1) seed -> key (see abs_algo.h).
 *
 * The ABS uses algorithm family 0x03, the plain 32-bit arithmetic form, no table and no flag:
 *     m   = ((seed16 - 25110) << 4) + 160       (signed 32-bit)
 *     key = [ (m >> 8) & 0xFF , m & 0xFF ]
 * Verified byte-for-byte for all 65536 seeds (e.g. seed 0x9AED -> 0x8E10).
 */
#include "abs_algo.h"

bool absComputeSecurityKey(const uint8_t *seed, uint8_t seed_len, uint8_t *key, uint8_t *key_len) {
    if (key_len) *key_len = 0;
    if (!seed || !key || !key_len || seed_len < 2) return false;

    const int32_t n = (int32_t)(((uint16_t)seed[0] << 8) | (uint16_t)seed[1]);
    const int32_t m = ((n - 25110) << 4) + 160;

    key[0] = (uint8_t)((m >> 8) & 0xFF);
    key[1] = (uint8_t)(m & 0xFF);
    *key_len = 2;
    return true;
}
