/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file bcm_algo.cpp
 * @brief BCM (Body Control Module, 0x7E2) seed -> key (see bcm_algo.h).
 *
 * The BCM uses algorithm family 0x01. It is a flat table lookup with no flag: key16 = BCM_SEEDKEY_TABLE[seed16].
 * The table in bcm_algo_data.h was verified byte-for-byte for all 65536 seeds.
 * Verified pair: seed 0x66EE -> key 0x76CB.
 */
#include "bcm_algo.h"
#include "bcm_algo_data.h"
#include <pgmspace.h>

bool bcmComputeSecurityKey(const uint8_t *seed, uint8_t seed_len, uint8_t *key, uint8_t *key_len) {
    if (key_len) *key_len = 0;
    if (!seed || !key || !key_len || seed_len < 2) return false;

    const uint16_t seed16 = (uint16_t)(((uint16_t)seed[0] << 8) | (uint16_t)seed[1]);
    const uint16_t key16  = pgm_read_word(&BCM_SEEDKEY_TABLE[seed16]);   // flat: table[seed] = key

    key[0] = (uint8_t)(key16 >> 8);
    key[1] = (uint8_t)(key16 & 0xFF);
    *key_len = 2;
    return true;
}
