/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file spdo_algo.cpp
 * @brief SPDO (Speedometer / instrument cluster, 0x7F2) seed -> key (see spdo_algo.h).
 *
 * The SPDO module uses a plain arithmetic seed/key algorithm (algorithm family 0x02).
 * Unlike the ECM it is a plain arithmetic routine with no lookup table and no F1F4 flag:
 *     key16 = (6151 * seed16  XOR  8209) & 0xFFFF
 * Verified against a real pair: seed 0x7355 -> key 0x3F42.
 */
#include "spdo_algo.h"

bool spdoComputeSecurityKey(const uint8_t *seed, uint8_t seed_len, uint8_t *key, uint8_t *key_len) {
    if (key_len) *key_len = 0;
    if (!seed || !key || !key_len || seed_len < 2) return false;

    const uint16_t seed16 = (uint16_t)(((uint16_t)seed[0] << 8) | (uint16_t)seed[1]);
    const uint16_t key16  = (uint16_t)(((uint32_t)6151u * (uint32_t)seed16) ^ 8209u);   // & 0xFFFF via truncation

    key[0] = (uint8_t)(key16 >> 8);
    key[1] = (uint8_t)(key16 & 0xFF);
    *key_len = 2;
    return true;
}
