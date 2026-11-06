/**
 * Copyright (C) 2026 Henzulea Daniel email: <zulea1@gmail.com>. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE).
 * @file bcm_algo.h
 * @brief BCM (module) UDS Security Access (0x27) seed -> key.
 *
 * Algorithm family 0x01: a flat table lookup, no flag -
 * key16 = BCM_SEEDKEY_TABLE[seed16] (table in bcm_algo_data.h). Verified: seed 0x66EE -> key 0x76CB.
 */
#pragma once
#include <stdint.h>

bool bcmComputeSecurityKey(const uint8_t *seed, uint8_t seed_len, uint8_t *key, uint8_t *key_len);
